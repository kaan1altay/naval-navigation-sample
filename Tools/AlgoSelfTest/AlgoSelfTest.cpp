// Copyright (c) 2026 Kaan Altay. Licensed under the MIT License.
//
// Standalone test runner for the engine-free navigation core.
//
// The same scenarios are covered by the UE automation tests in Source/NavalNav/Tests, which
// are the authoritative ones. This runner exists so the algorithm can be verified with a
// plain compiler on a machine without Unreal installed — including CI. See Shim/CoreMinimal.h.
//
// Build and run:  Tools/AlgoSelfTest/run_tests.sh

#include "Grid/SeaGridPathfinder.h"
#include "Grid/SeaGridTypes.h"
#include "Ship/SailingModel.h"
#include "Threat/ThreatEvaluator.h"

#include <cmath>
#include <cstdio>
#include <random>
#include <queue>

namespace
{
	int32 NumChecks = 0;
	int32 NumFailures = 0;
	const char* CurrentTest = "";

	void Check(bool bCondition, const char* Expression)
	{
		++NumChecks;
		if (!bCondition)
		{
			++NumFailures;
			std::printf("    FAIL  %s :: %s\n", CurrentTest, Expression);
		}
	}

	void CheckNearly(float A, float B, float Tolerance, const char* Expression)
	{
		++NumChecks;
		if (!(std::abs(A - B) <= Tolerance))
		{
			++NumFailures;
			std::printf("    FAIL  %s :: %s (%.4f vs %.4f)\n", CurrentTest, Expression, A, B);
		}
	}

#define TEST_CHECK(Expr) Check((Expr), #Expr)
#define TEST_NEARLY(A, B, Tol) CheckNearly((A), (B), (Tol), #A " ~= " #B)

	void BeginTest(const char* Name)
	{
		CurrentTest = Name;
		std::printf("  [ run ] %s\n", Name);
	}

	/** A small square grid with 100 uu cells, origin at (0,0). Cell (X,Y) centres at (100X+50, 100Y+50). */
	FSeaGridData MakeGrid(int32 CellsPerSide, float CellSize = 100.0f)
	{
		FSeaGridConfig Config;
		Config.CellSize = CellSize;
		Config.Extent = FVector2D(CellsPerSide * CellSize * 0.5, CellsPerSide * CellSize * 0.5);
		Config.Center = FVector2D(Config.Extent.X, Config.Extent.Y);

		FSeaGridData Grid;
		Grid.Rebuild(Config);
		return Grid;
	}

	/**
	 * Reference implementation: plain Dijkstra over the same cost model, used to prove that A*
	 * with an admissible heuristic really returns the optimal cost and not merely a valid one.
	 */
	float DijkstraCost(const FSeaGridData& Grid, const FIntPoint& Start, const FIntPoint& Goal)
	{
		constexpr float Diagonal = 1.4142135623730951f;
		const int32 NumCells = Grid.GetNumCells();
		std::vector<float> Dist(static_cast<size_t>(NumCells), std::numeric_limits<float>::max());
		std::priority_queue<std::pair<float, int32>, std::vector<std::pair<float, int32>>, std::greater<>> Open;

		Dist[static_cast<size_t>(Grid.CellToIndex(Start))] = 0.0f;
		Open.push({ 0.0f, Grid.CellToIndex(Start) });

		while (!Open.empty())
		{
			const auto [Cost, Index] = Open.top();
			Open.pop();
			if (Cost > Dist[static_cast<size_t>(Index)])
			{
				continue;
			}
			if (Index == Grid.CellToIndex(Goal))
			{
				return Cost;
			}

			const FIntPoint Cell = Grid.IndexToCell(Index);
			for (int32 OffsetY = -1; OffsetY <= 1; ++OffsetY)
			{
				for (int32 OffsetX = -1; OffsetX <= 1; ++OffsetX)
				{
					if (OffsetX == 0 && OffsetY == 0)
					{
						continue;
					}

					const FIntPoint Next(Cell.X + OffsetX, Cell.Y + OffsetY);
					if (!Grid.IsValidCell(Next))
					{
						continue;
					}

					const float NextCost = Grid.GetCellCost(Next);
					if (NextCost >= NavalNav::ImpassableCost)
					{
						continue;
					}

					const bool bDiagonal = (OffsetX != 0 && OffsetY != 0);
					if (bDiagonal)
					{
						// Mirror the pathfinder's corner rule, otherwise the two disagree.
						const FIntPoint SideA(Cell.X + OffsetX, Cell.Y);
						const FIntPoint SideB(Cell.X, Cell.Y + OffsetY);
						if (Grid.GetCellCost(SideA) >= NavalNav::ImpassableCost || Grid.GetCellCost(SideB) >= NavalNav::ImpassableCost)
						{
							continue;
						}
					}

					const float Tentative = Cost + NextCost * (bDiagonal ? Diagonal : 1.0f);
					const size_t NextIndex = static_cast<size_t>(Grid.CellToIndex(Next));
					if (Tentative < Dist[NextIndex])
					{
						Dist[NextIndex] = Tentative;
						Open.push({ Tentative, Grid.CellToIndex(Next) });
					}
				}
			}
		}

		return -1.0f;
	}

	/** Stamps a circular threat blob, exactly as ADangerZone does at runtime. */
	void StampBlob(FSeaGridData& Grid, const FIntPoint& CenterCell, float RadiusInCells, float ThreatScale, float LethalFraction)
	{
		const FVector Center = Grid.CellToWorld(CenterCell);
		const float Radius = RadiusInCells * Grid.GetCellSize();

		Grid.ForEachCellInRadius(Center, Radius, [&](const FIntPoint& Cell, float Distance)
		{
			const float Cost = FThreatEvaluator::CellThreatCost(Distance, Radius, ThreatScale, 2.0f, LethalFraction);
			if (Cost > 0.0f)
			{
				Grid.AddThreatCost(Cell, Cost);
			}
		});
	}

	/** Every cell the finished polyline actually crosses, sampled the way the planner charges it. */
	std::vector<FIntPoint> CollectTraversedCells(const FSeaGridData& Grid, const FNavalPath& Path)
	{
		std::vector<FIntPoint> Cells;
		for (int32 Index = 0; Index < Path.Num(); ++Index)
		{
			const FIntPoint To = Grid.WorldToCell(Path.Waypoints[Index]);
			if (Index == 0)
			{
				Cells.push_back(To);
				continue;
			}

			const FIntPoint From = Grid.WorldToCell(Path.Waypoints[Index - 1]);
			const int32 DeltaX = To.X - From.X;
			const int32 DeltaY = To.Y - From.Y;
			const int32 NumSteps = FMath::Max(FMath::Abs(DeltaX), FMath::Abs(DeltaY));
			for (int32 Step = 1; Step <= NumSteps; ++Step)
			{
				const float Alpha = static_cast<float>(Step) / static_cast<float>(NumSteps);
				Cells.push_back(FIntPoint(
					From.X + FMath::RoundToInt32(DeltaX * Alpha),
					From.Y + FMath::RoundToInt32(DeltaY * Alpha)));
			}
		}
		return Cells;
	}

	// -------------------------------------------------------------------------------------
	void TestGridGeometry()
	{
		BeginTest("Grid geometry round-trips world and cell coordinates");

		FSeaGridData Grid = MakeGrid(20);
		TEST_CHECK(Grid.IsBuilt());
		TEST_CHECK(Grid.GetNumCellsX() == 20);
		TEST_CHECK(Grid.GetNumCellsY() == 20);
		TEST_CHECK(Grid.GetNumCells() == 400);

		for (int32 Y = 0; Y < Grid.GetNumCellsY(); ++Y)
		{
			for (int32 X = 0; X < Grid.GetNumCellsX(); ++X)
			{
				const FIntPoint Cell(X, Y);
				TEST_CHECK(Grid.WorldToCell(Grid.CellToWorld(Cell)) == Cell);
			}
		}

		TEST_CHECK(!Grid.IsValidCell(FIntPoint(-1, 0)));
		TEST_CHECK(!Grid.IsValidCell(FIntPoint(20, 0)));
		TEST_CHECK(Grid.ClampCell(FIntPoint(-5, 99)) == FIntPoint(0, 19));
		TEST_NEARLY(Grid.GetCellCost(FIntPoint(3, 3)), NavalNav::OpenWaterCost, 1.0e-4f);
		TEST_CHECK(Grid.GetCellCost(FIntPoint(-1, 0)) >= NavalNav::ImpassableCost);

		// The cell-count clamp must not silently allocate a gigantic grid.
		FSeaGridConfig Huge;
		Huge.CellSize = 1.0f;
		Huge.Extent = FVector2D(500000.0, 500000.0);
		FSeaGridData HugeGrid;
		HugeGrid.Rebuild(Huge);
		TEST_CHECK(HugeGrid.GetNumCells() <= NavalNav::MaxGridCells);
		TEST_CHECK(HugeGrid.GetCellSize() > 1.0f);
	}

	void TestStraightPathOnEmptyGrid()
	{
		BeginTest("Straight path across open water");

		FSeaGridData Grid = MakeGrid(20);
		FSeaGridPathfinder Pathfinder;
		FSeaGridPathQuery Query;

		const FIntPoint Start(2, 2);
		const FIntPoint Goal(17, 2);
		const FNavalPath Path = Pathfinder.FindPath(Grid, Start, Goal, Query);

		TEST_CHECK(Path.bSuccess);
		TEST_CHECK(Path.Num() >= 2);
		TEST_CHECK(Path.Costs.Num() == Path.Waypoints.Num());

		// Simplification must collapse a straight run to its two endpoints.
		TEST_CHECK(Path.Num() == 2);
		TEST_CHECK(Grid.WorldToCell(Path.Waypoints[0]) == Start);
		TEST_CHECK(Grid.WorldToCell(Path.Waypoints.Last()) == Goal);

		// 15 orthogonal steps over open water.
		TEST_NEARLY(Path.Costs[0], 0.0f, 1.0e-4f);
		TEST_NEARLY(Path.GetTotalCost(), 15.0f, 1.0e-3f);
		TEST_NEARLY(Path.GetLength(), 1500.0f, 1.0e-2f);
		TEST_CHECK(Path.CellsSearched > 0);

		// A pure diagonal run costs sqrt(2) per step, not 2.
		const FNavalPath Diagonal = Pathfinder.FindPath(Grid, FIntPoint(2, 2), FIntPoint(12, 12), Query);
		TEST_CHECK(Diagonal.bSuccess);
		TEST_CHECK(Diagonal.Num() == 2);
		TEST_NEARLY(Diagonal.GetTotalCost(), 10.0f * 1.41421356f, 1.0e-3f);

		// Degenerate query: start == goal.
		const FNavalPath Trivial = Pathfinder.FindPath(Grid, Start, Start, Query);
		TEST_CHECK(Trivial.bSuccess);
		TEST_CHECK(Trivial.Num() == 1);
		TEST_NEARLY(Trivial.GetTotalCost(), 0.0f, 1.0e-4f);
	}

	void TestPathBendsAroundThreat()
	{
		BeginTest("Path bends around a threat blob");

		FSeaGridData Grid = MakeGrid(40);
		FSeaGridPathfinder Pathfinder;
		FSeaGridPathQuery Query;

		const FIntPoint Start(2, 20);
		const FIntPoint Goal(37, 20);

		const FNavalPath Clear = Pathfinder.FindPath(Grid, Start, Goal, Query);
		TEST_CHECK(Clear.bSuccess);
		TEST_CHECK(Clear.Num() == 2);

		// A blob with a lethal core straddling the straight route.
		StampBlob(Grid, FIntPoint(20, 20), /*RadiusInCells=*/8.0f, /*ThreatScale=*/20.0f, /*LethalFraction=*/0.4f);
		TEST_CHECK(Grid.GetCellCost(FIntPoint(20, 20)) >= NavalNav::ImpassableCost);
		TEST_CHECK(Grid.GetThreatCost(FIntPoint(20, 26)) > 0.0f);
		TEST_NEARLY(Grid.GetThreatCost(FIntPoint(20, 30)), 0.0f, 1.0e-4f);

		const FNavalPath Bent = Pathfinder.FindPath(Grid, Start, Goal, Query);
		TEST_CHECK(Bent.bSuccess);
		TEST_CHECK(Bent.Num() > 2);
		TEST_CHECK(Bent.GetTotalCost() > Clear.GetTotalCost());
		TEST_CHECK(Bent.GetLength() > Clear.GetLength());

		// Every waypoint, and every cell between them, must stay out of the lethal core.
		float MaxAbsDeviation = 0.0f;
		for (int32 Index = 0; Index < Bent.Num(); ++Index)
		{
			const FIntPoint Cell = Grid.WorldToCell(Bent.Waypoints[Index]);
			TEST_CHECK(Grid.GetCellCost(Cell) < NavalNav::ImpassableCost);
			MaxAbsDeviation = FMath::Max(MaxAbsDeviation, static_cast<float>(FMath::Abs(Cell.Y - 20)));

			if (Index > 0)
			{
				float LegCost = 0.0f;
				const bool bClearLeg = FSeaGridPathfinder::SampleSegment(Grid,
					[&Grid](const FIntPoint& C) { return Grid.GetCellCost(C); },
					Grid.WorldToCell(Bent.Waypoints[Index - 1]), Cell, LegCost);
				TEST_CHECK(bClearLeg);
			}
		}

		// It has to go around, not nudge sideways: the lethal core alone is over 3 cells wide.
		TEST_CHECK(MaxAbsDeviation >= 3.0f);

		// Costs stay monotonic and consistent with the reported total.
		for (int32 Index = 1; Index < Bent.Costs.Num(); ++Index)
		{
			TEST_CHECK(Bent.Costs[Index] >= Bent.Costs[Index - 1]);
		}
		TEST_NEARLY(Bent.GetTotalCost(), Bent.Costs.Last(), 1.0e-4f);

		// A ship strong enough to ignore the zone sails straight through: same grid, but the
		// cost functor reports open water, which is what a hostility check would produce.
		const FNavalPath Fearless = Pathfinder.FindPath(Grid, Start, Goal,
			[](const FIntPoint&) { return NavalNav::OpenWaterCost; }, Query);
		TEST_CHECK(Fearless.bSuccess);
		TEST_CHECK(Fearless.Num() == 2);
	}

	void TestNoPathWhenWalled()
	{
		BeginTest("No path when the sea is fully walled off");

		FSeaGridData Grid = MakeGrid(20);
		for (int32 Y = 0; Y < Grid.GetNumCellsY(); ++Y)
		{
			Grid.SetBaseCost(FIntPoint(10, Y), NavalNav::ImpassableCost);
		}

		FSeaGridPathfinder Pathfinder;
		FSeaGridPathQuery Query;

		const FNavalPath Blocked = Pathfinder.FindPath(Grid, FIntPoint(2, 10), FIntPoint(18, 10), Query);
		TEST_CHECK(!Blocked.bSuccess);
		TEST_CHECK(Blocked.Num() == 0);
		TEST_CHECK(Blocked.CellsSearched > 0);

		// Same wall with one gate: now it must find the way through.
		Grid.SetBaseCost(FIntPoint(10, 4), NavalNav::OpenWaterCost);
		const FNavalPath Through = Pathfinder.FindPath(Grid, FIntPoint(2, 10), FIntPoint(18, 10), Query);
		TEST_CHECK(Through.bSuccess);

		// The gate itself is not necessarily a waypoint — it sits in the middle of a straight
		// run, so simplification drops it. What matters is that the route passes through it.
		const std::vector<FIntPoint> Traversed = CollectTraversedCells(Grid, Through);
		bool bPassesGate = false;
		for (const FIntPoint& Cell : Traversed)
		{
			bPassesGate |= (Cell == FIntPoint(10, 4));
		}
		TEST_CHECK(bPassesGate);

		// Starting or ending inside land is a failure, not a nearest-reachable fallback.
		const FNavalPath FromLand = Pathfinder.FindPath(Grid, FIntPoint(10, 10), FIntPoint(18, 10), Query);
		TEST_CHECK(!FromLand.bSuccess);
		const FNavalPath ToLand = Pathfinder.FindPath(Grid, FIntPoint(2, 10), FIntPoint(10, 10), Query);
		TEST_CHECK(!ToLand.bSuccess);

		// Off-grid endpoints are rejected rather than clamped silently.
		const FNavalPath OffGrid = Pathfinder.FindPath(Grid, FIntPoint(-1, 0), FIntPoint(5, 5), Query);
		TEST_CHECK(!OffGrid.bSuccess);
	}

	void TestCornerCutting()
	{
		BeginTest("Diagonal steps do not cut through touching obstacles");

		FSeaGridData Grid = MakeGrid(4);
		Grid.SetBaseCost(FIntPoint(1, 0), NavalNav::ImpassableCost);
		Grid.SetBaseCost(FIntPoint(0, 1), NavalNav::ImpassableCost);

		FSeaGridPathfinder Pathfinder;
		FSeaGridPathQuery Query;

		// (0,0) is boxed in: the only free neighbour is the diagonal, which is off limits.
		const FNavalPath Prevented = Pathfinder.FindPath(Grid, FIntPoint(0, 0), FIntPoint(1, 1), Query);
		TEST_CHECK(!Prevented.bSuccess);

		Query.bPreventCornerCutting = false;
		const FNavalPath Allowed = Pathfinder.FindPath(Grid, FIntPoint(0, 0), FIntPoint(1, 1), Query);
		TEST_CHECK(Allowed.bSuccess);
		TEST_CHECK(Allowed.Num() == 2);
	}

	void TestOptimalityAgainstDijkstra()
	{
		BeginTest("A* matches Dijkstra on randomised cost fields");

		std::mt19937 Random(20260816u);
		std::uniform_real_distribution<float> CostNoise(0.0f, 6.0f);
		std::uniform_int_distribution<int32> Coordinate(0, 29);

		FSeaGridPathQuery Query;
		// Simplification changes the geometry, so compare the raw search cost.
		Query.bStringPull = false;
		Query.bRemoveCollinearWaypoints = false;

		FSeaGridPathfinder Pathfinder;
		int32 NumComparisons = 0;

		for (int32 Trial = 0; Trial < 12; ++Trial)
		{
			FSeaGridData Grid = MakeGrid(30);
			for (int32 Y = 0; Y < 30; ++Y)
			{
				for (int32 X = 0; X < 30; ++X)
				{
					Grid.AddThreatCost(FIntPoint(X, Y), CostNoise(Random));
				}
			}
			for (int32 Blob = 0; Blob < 3; ++Blob)
			{
				StampBlob(Grid, FIntPoint(Coordinate(Random), Coordinate(Random)), 4.0f, 15.0f, 0.3f);
			}

			const FIntPoint Start(1, 1);
			const FIntPoint Goal(28, 28);
			if (Grid.GetCellCost(Start) >= NavalNav::ImpassableCost || Grid.GetCellCost(Goal) >= NavalNav::ImpassableCost)
			{
				continue;
			}

			const FNavalPath Path = Pathfinder.FindPath(Grid, Start, Goal, Query);
			const float Reference = DijkstraCost(Grid, Start, Goal);

			TEST_CHECK(Path.bSuccess == (Reference >= 0.0f));
			if (Path.bSuccess && Reference >= 0.0f)
			{
				++NumComparisons;
				TEST_NEARLY(Path.GetTotalCost(), Reference, 1.0e-2f);
			}
		}

		TEST_CHECK(NumComparisons >= 8);
	}

	void TestWeightedSearchAndBudget()
	{
		BeginTest("Weighted heuristic and search budget behave as documented");

		FSeaGridData Grid = MakeGrid(60);
		StampBlob(Grid, FIntPoint(30, 30), 10.0f, 12.0f, 0.2f);

		FSeaGridPathfinder Pathfinder;
		FSeaGridPathQuery Admissible;
		const FNavalPath Optimal = Pathfinder.FindPath(Grid, FIntPoint(2, 30), FIntPoint(57, 30), Admissible);
		TEST_CHECK(Optimal.bSuccess);

		FSeaGridPathQuery Weighted;
		Weighted.HeuristicWeight = 1.6f;
		const FNavalPath Fast = Pathfinder.FindPath(Grid, FIntPoint(2, 30), FIntPoint(57, 30), Weighted);
		TEST_CHECK(Fast.bSuccess);
		// Weighted A* must expand no more cells than the admissible search, and stay close.
		TEST_CHECK(Fast.CellsSearched <= Optimal.CellsSearched);
		TEST_CHECK(Fast.GetTotalCost() >= Optimal.GetTotalCost() - 1.0e-3f);

		FSeaGridPathQuery Budgeted;
		Budgeted.MaxSearchedCells = 25;
		const FNavalPath Starved = Pathfinder.FindPath(Grid, FIntPoint(2, 30), FIntPoint(57, 30), Budgeted);
		TEST_CHECK(!Starved.bSuccess);
		TEST_CHECK(Starved.CellsSearched <= 25);
	}

	void TestStringPullRespectsCost()
	{
		BeginTest("String pull never shortcuts through danger");

		FSeaGridData Grid = MakeGrid(40);
		// Expensive but crossable: the search routes around it, and a naive line-of-sight
		// string pull would happily cut the corner straight back through it.
		StampBlob(Grid, FIntPoint(20, 20), 8.0f, 25.0f, 0.0f);

		FSeaGridPathfinder Pathfinder;
		FSeaGridPathQuery Query;

		const FNavalPath Pulled = Pathfinder.FindPath(Grid, FIntPoint(2, 20), FIntPoint(37, 20), Query);
		TEST_CHECK(Pulled.bSuccess);

		FSeaGridPathQuery Raw;
		Raw.bStringPull = false;
		Raw.bRemoveCollinearWaypoints = false;
		const FNavalPath Unpulled = Pathfinder.FindPath(Grid, FIntPoint(2, 20), FIntPoint(37, 20), Raw);
		TEST_CHECK(Unpulled.bSuccess);

		// Fewer waypoints, and no meaningful extra cost for the smoothing.
		TEST_CHECK(Pulled.Num() < Unpulled.Num());
		TEST_CHECK(Pulled.GetTotalCost() <= Unpulled.GetTotalCost() * 1.05f + 1.0e-3f);

		// The centre of the blob is the cheapest thing to cut through and must stay untouched.
		const float CenterCost = Grid.GetCellCost(FIntPoint(20, 20));
		for (const FVector& Waypoint : Pulled.Waypoints)
		{
			TEST_CHECK(Grid.GetCellCost(Grid.WorldToCell(Waypoint)) < CenterCost);
		}
	}

	void TestThreatEvaluator()
	{
		BeginTest("Threat evaluator scales with power and distance");

		// Hostility is relative to the observer.
		TEST_CHECK(FThreatEvaluator::IsHostile(1.0f, 3.0f, 0.0f));
		TEST_CHECK(!FThreatEvaluator::IsHostile(5.0f, 3.0f, 0.0f));
		TEST_CHECK(!FThreatEvaluator::IsHostile(3.0f, 3.0f, 0.0f));
		TEST_CHECK(!FThreatEvaluator::IsHostile(1.0f, 3.0f, 5.0f));
		TEST_CHECK(FThreatEvaluator::IsHostile(1.0f, 7.0f, 5.0f));

		TEST_NEARLY(FThreatEvaluator::RelativeThreat(2.0f, 4.0f), 1.0f, 1.0e-4f);
		TEST_NEARLY(FThreatEvaluator::RelativeThreat(4.0f, 2.0f), 0.0f, 1.0e-4f);
		// An unarmed observer must not divide by zero.
		TEST_CHECK(FThreatEvaluator::RelativeThreat(0.0f, 4.0f) > 0.0f);

		// Falloff: strictly decreasing outwards, zero at and beyond the edge.
		const float Radius = 1000.0f;
		float Previous = FThreatEvaluator::CellThreatCost(0.0f, Radius, 10.0f, 2.0f, 0.0f);
		TEST_NEARLY(Previous, 10.0f, 1.0e-3f);
		for (float Distance = 100.0f; Distance < Radius; Distance += 100.0f)
		{
			const float Cost = FThreatEvaluator::CellThreatCost(Distance, Radius, 10.0f, 2.0f, 0.0f);
			TEST_CHECK(Cost < Previous);
			TEST_CHECK(Cost > 0.0f);
			Previous = Cost;
		}
		TEST_NEARLY(FThreatEvaluator::CellThreatCost(Radius, Radius, 10.0f, 2.0f, 0.0f), 0.0f, 1.0e-4f);
		TEST_NEARLY(FThreatEvaluator::CellThreatCost(2.0f * Radius, Radius, 10.0f, 2.0f, 0.0f), 0.0f, 1.0e-4f);

		// Lethal core is impassable, and the falloff outside it is still continuous.
		TEST_CHECK(FThreatEvaluator::CellThreatCost(100.0f, Radius, 10.0f, 2.0f, 0.25f) >= NavalNav::ImpassableCost);
		TEST_CHECK(FThreatEvaluator::CellThreatCost(300.0f, Radius, 10.0f, 2.0f, 0.25f) < NavalNav::ImpassableCost);
		TEST_NEARLY(FThreatEvaluator::CellThreatCost(0.0f, Radius, 0.0f, 2.0f, 0.0f), 0.0f, 1.0e-4f);

		// Zones stack rather than overwrite.
		FSeaGridData Grid = MakeGrid(20);
		StampBlob(Grid, FIntPoint(10, 10), 5.0f, 4.0f, 0.0f);
		const float Single = Grid.GetThreatCost(FIntPoint(10, 10));
		StampBlob(Grid, FIntPoint(10, 10), 5.0f, 4.0f, 0.0f);
		TEST_NEARLY(Grid.GetThreatCost(FIntPoint(10, 10)), 2.0f * Single, 1.0e-3f);

		Grid.ClearThreat();
		TEST_NEARLY(Grid.GetThreatCost(FIntPoint(10, 10)), 0.0f, 1.0e-4f);
		TEST_NEARLY(Grid.GetCellCost(FIntPoint(10, 10)), NavalNav::OpenWaterCost, 1.0e-4f);
	}

	void TestBufferReuseAcrossQueries()
	{
		BeginTest("Repeated queries reuse buffers and stay stable");

		FSeaGridData Grid = MakeGrid(30);
		StampBlob(Grid, FIntPoint(15, 15), 6.0f, 12.0f, 0.3f);

		FSeaGridPathfinder Pathfinder;
		FSeaGridPathQuery Query;

		const FNavalPath First = Pathfinder.FindPath(Grid, FIntPoint(1, 1), FIntPoint(28, 28), Query);
		TEST_CHECK(First.bSuccess);

		// The generation-stamp trick is the one place a stale-state bug would hide: run the
		// same query many times and interleave others; every result must be identical.
		for (int32 Iteration = 0; Iteration < 50; ++Iteration)
		{
			const FNavalPath Other = Pathfinder.FindPath(Grid, FIntPoint(28, 1), FIntPoint(1, 28), Query);
			TEST_CHECK(Other.bSuccess);

			const FNavalPath Repeat = Pathfinder.FindPath(Grid, FIntPoint(1, 1), FIntPoint(28, 28), Query);
			if (Repeat.Num() != First.Num() || Repeat.CellsSearched != First.CellsSearched)
			{
				TEST_CHECK(false);
				break;
			}
			TEST_NEARLY(Repeat.GetTotalCost(), First.GetTotalCost(), 1.0e-4f);
		}

		// A grid rebuilt at a different size must not trip over the previous buffers.
		FSeaGridData Smaller = MakeGrid(12);
		const FNavalPath OnSmaller = Pathfinder.FindPath(Smaller, FIntPoint(0, 0), FIntPoint(11, 11), Query);
		TEST_CHECK(OnSmaller.bSuccess);
	}

	void TestWorldSpaceEntryPoints()
	{
		BeginTest("World-space planning clamps out-of-bounds endpoints");

		FSeaGridData Grid = MakeGrid(20);
		FSeaGridPathfinder Pathfinder;
		FSeaGridPathQuery Query;

		// A ship pushed past the border still gets a plan back.
		const FNavalPath Path = Pathfinder.FindPathWorld(Grid, FVector(-9999.0, 250.0, 0.0), FVector(9999.0, 250.0, 0.0), Query);
		TEST_CHECK(Path.bSuccess);
		TEST_CHECK(Grid.WorldToCell(Path.Waypoints[0]) == FIntPoint(0, 2));
		TEST_CHECK(Grid.WorldToCell(Path.Waypoints.Last()) == FIntPoint(19, 2));

		TEST_NEARLY(FSeaGridPathfinder::OctileDistance(FIntPoint(0, 0), FIntPoint(3, 0)), 3.0f, 1.0e-4f);
		TEST_NEARLY(FSeaGridPathfinder::OctileDistance(FIntPoint(0, 0), FIntPoint(3, 3)), 3.0f * 1.41421356f, 1.0e-4f);
		TEST_NEARLY(FSeaGridPathfinder::OctileDistance(FIntPoint(0, 0), FIntPoint(5, 2)), 3.0f + 2.0f * 1.41421356f, 1.0e-4f);
	}

	// -------------------------------------------------------------------------------------
	/** Advances a state on a fixed helm/trim for Seconds and returns where it ends up. */
	FSailingState SailFor(const FSailingModel& Model, FSailingState Start, float Rudder, float Trim,
		float WindFromYaw, float WindStrength, float Seconds, float Dt = 0.05f)
	{
		const int32 Steps = FMath::Max(1, FMath::RoundToInt32(Seconds / Dt));
		for (int32 Step = 0; Step < Steps; ++Step)
		{
			Model.Advance(Start, Rudder, Trim, WindFromYaw, WindStrength, Dt);
		}
		return Start;
	}

	void TestPolarCurve()
	{
		BeginTest("Polar curve is zero upwind and peaks on a reach");

		const FSailingModelParams P;

		TEST_NEARLY(FSailingModel::PolarThrustFactor(0.0f, P), 0.0f, 1.0e-4f);
		TEST_NEARLY(FSailingModel::PolarThrustFactor(P.NoGoAngleDegrees - 5.0f, P), 0.0f, 1.0e-4f);
		TEST_NEARLY(FSailingModel::PolarThrustFactor(P.NoGoAngleDegrees, P), 0.0f, 1.0e-4f);

		TEST_NEARLY(FSailingModel::PolarThrustFactor(P.BeamReachAngleDegrees, P), 1.0f, 1.0e-3f);
		TEST_NEARLY(FSailingModel::PolarThrustFactor(70.0f, P), FSailingModel::PolarThrustFactor(-70.0f, P), 1.0e-4f);

		const float CloseHauled = FSailingModel::PolarThrustFactor(P.NoGoAngleDegrees + 10.0f, P);
		const float BroadReach = FSailingModel::PolarThrustFactor(140.0f, P);
		TEST_CHECK(CloseHauled > 0.0f);
		TEST_CHECK(CloseHauled < 1.0f);
		TEST_CHECK(BroadReach < 1.0f);
		TEST_CHECK(BroadReach > CloseHauled);
		TEST_NEARLY(FSailingModel::PolarThrustFactor(180.0f, P), P.DownwindFactor, 1.0e-3f);

		float Previous = -1.0f;
		for (float Angle = P.NoGoAngleDegrees; Angle <= P.BeamReachAngleDegrees; Angle += 2.0f)
		{
			const float Value = FSailingModel::PolarThrustFactor(Angle, P);
			TEST_CHECK(Value >= Previous - 1.0e-4f);
			Previous = Value;
		}
	}

	void TestConvergesToBoundedSpeed()
	{
		BeginTest("Speed converges to a bounded terminal on a reach");

		FSailingModel Model;
		const float WindFromYaw = -90.0f; // heading 0 is a beam reach

		FSailingState Beam;
		Beam.HeadingDegrees = 0.0f;
		const FSailingState Settled = SailFor(Model, Beam, 0.0f, 1.0f, WindFromYaw, 1.0f, 120.0f);
		const float Terminal = Model.TerminalSpeedForDrive(1.0f);

		TEST_NEARLY(Settled.Speed, Terminal, Terminal * 0.02f);
		TEST_NEARLY(Terminal, Model.Params.MaxSpeed, 1.0e-2f);
		TEST_CHECK(Settled.Speed <= Model.Params.MaxSpeed * 1.001f);
		TEST_CHECK(Settled.Speed > Model.Params.MaxSpeed * 0.9f);

		const FSailingState Light = SailFor(Model, Beam, 0.0f, 1.0f, WindFromYaw, 0.5f, 120.0f);
		TEST_NEARLY(Light.Speed, Model.TerminalSpeedForDrive(0.5f), Terminal * 0.03f);
		TEST_CHECK(Light.Speed < Settled.Speed);

		FSailingState IntoWind;
		IntoWind.HeadingDegrees = FSailingModel::NormalizeDegrees(WindFromYaw);
		const FSailingState Stalled = SailFor(Model, IntoWind, 0.0f, 1.0f, WindFromYaw, 1.0f, 30.0f);
		TEST_CHECK(Stalled.Speed < 1.0f);
	}

	void TestRudderSign()
	{
		BeginTest("Rudder turns the head the expected way, and only with way on");

		FSailingModel Model;
		const float WindFromYaw = -90.0f;

		FSailingState Making;
		Making.HeadingDegrees = 0.0f;
		Making = SailFor(Model, Making, 0.0f, 1.0f, WindFromYaw, 1.0f, 40.0f);
		TEST_CHECK(Making.Speed > 100.0f);

		const float Before = Making.HeadingDegrees;
		const FSailingState Port = SailFor(Model, Making, +1.0f, 1.0f, WindFromYaw, 1.0f, 3.0f);
		const FSailingState Starboard = SailFor(Model, Making, -1.0f, 1.0f, WindFromYaw, 1.0f, 3.0f);
		TEST_CHECK(Port.HeadingDegrees > Before + 1.0f);
		TEST_CHECK(Starboard.HeadingDegrees < Before - 1.0f);

		FSailingState Adrift;
		Adrift.HeadingDegrees = 30.0f;
		Adrift.Speed = 0.0f;
		const FSailingState Tried = SailFor(Model, Adrift, 1.0f, 0.0f, WindFromYaw, 0.0f, 2.0f);
		TEST_NEARLY(Tried.HeadingDegrees, 30.0f, 0.5f);
	}

	void TestTurnHelpers()
	{
		BeginTest("Turn radius and time-to-turn helpers are sane");

		FSailingModel Model;

		TEST_CHECK(Model.PredictTurnRadius(0.0f) > 1.0e9f);
		TEST_CHECK(Model.EstimateTimeToTurn(90.0f, 0.0f) > 1.0e9f);

		const float Radius = Model.PredictTurnRadius(Model.Params.MaxSpeed);
		TEST_CHECK(Radius > 0.0f);
		TEST_CHECK(Radius < 1.0e6f);

		const float QuarterTurn = Model.EstimateTimeToTurn(90.0f, Model.Params.MaxSpeed);
		TEST_CHECK(QuarterTurn > 0.0f);
		TEST_NEARLY(Model.EstimateTimeToTurn(180.0f, Model.Params.MaxSpeed), QuarterTurn * 2.0f, 1.0e-3f);
		TEST_NEARLY(Model.EstimateTimeToTurn(-90.0f, Model.Params.MaxSpeed), QuarterTurn, 1.0e-4f);
	}

	void TestStableUnderRandomInput()
	{
		BeginTest("No NaNs over 10k ticks of random input");

		FSailingModel Model;
		std::mt19937 Random(0x5A11u);
		std::uniform_real_distribution<float> Rudder(-1.5f, 1.5f);
		std::uniform_real_distribution<float> Trim(-0.2f, 1.2f);
		std::uniform_real_distribution<float> WindFrom(-720.0f, 720.0f);
		std::uniform_real_distribution<float> Strength(-0.2f, 1.5f);
		std::uniform_real_distribution<float> Delta(0.001f, 0.5f);

		FSailingState State;
		State.HeadingDegrees = 10.0f;

		bool bFinite = true;
		for (int32 Tick = 0; Tick < 10000; ++Tick)
		{
			Model.Advance(State, Rudder(Random), Trim(Random), WindFrom(Random), Strength(Random), Delta(Random));
			bFinite &= std::isfinite(State.HeadingDegrees) && std::isfinite(State.Speed)
				&& std::isfinite(State.RudderAngleDegrees) && std::isfinite(State.SailTrim);
			if (!bFinite)
			{
				break;
			}
		}

		TEST_CHECK(bFinite);
		TEST_CHECK(State.HeadingDegrees > -180.5f && State.HeadingDegrees <= 180.5f);
		TEST_CHECK(State.Speed >= 0.0f && State.Speed <= Model.Params.MaxSpeed * 1.01f);
		TEST_CHECK(FMath::Abs(State.RudderAngleDegrees) <= Model.Params.MaxRudderAngleDegrees + 0.5f);
		TEST_CHECK(State.SailTrim >= 0.0f && State.SailTrim <= 1.0f);
	}
}

int main()
{
	std::printf("NavalNav algorithm self-test\n");

	TestGridGeometry();
	TestStraightPathOnEmptyGrid();
	TestPathBendsAroundThreat();
	TestNoPathWhenWalled();
	TestCornerCutting();
	TestOptimalityAgainstDijkstra();
	TestWeightedSearchAndBudget();
	TestStringPullRespectsCost();
	TestThreatEvaluator();
	TestBufferReuseAcrossQueries();
	TestWorldSpaceEntryPoints();

	TestPolarCurve();
	TestConvergesToBoundedSpeed();
	TestRudderSign();
	TestTurnHelpers();
	TestStableUnderRandomInput();

	std::printf("\n%d checks, %d failures\n", NumChecks, NumFailures);
	return NumFailures == 0 ? 0 : 1;
}
