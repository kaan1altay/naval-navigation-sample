// Copyright (c) 2026 Kaan Altay. Licensed under the MIT License.
//
// Automation tests for the navigation core. Run them from the editor's Session Frontend, or
// headless with:
//   UnrealEditor-Cmd.exe NavalNavSample.uproject -ExecCmds="Automation RunTests NavalNav" -unattended -nopause -testexit="Automation Test Queue Empty"
//
// The same scenarios also run without an engine via Tools/AlgoSelfTest/run_tests.sh, which is
// what makes them usable in a CI job that has no Unreal installation.

#include "CoreMinimal.h"
#include "Grid/SeaGridPathfinder.h"
#include "Grid/SeaGridTypes.h"
#include "Misc/AutomationTest.h"
#include "Threat/ThreatEvaluator.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace NavalNavTest
{
	/** Square grid of CellsPerSide cells at 100 uu, with its lower corner at the world origin. */
	static FSeaGridData MakeGrid(int32 CellsPerSide, float CellSize = 100.0f)
	{
		FSeaGridConfig Config;
		Config.CellSize = CellSize;
		Config.Extent = FVector2D(CellsPerSide * CellSize * 0.5, CellsPerSide * CellSize * 0.5);
		Config.Center = FVector2D(Config.Extent.X, Config.Extent.Y);

		FSeaGridData Grid;
		Grid.Rebuild(Config);
		return Grid;
	}

	/** Stamps a threat blob exactly the way ADangerZone does at runtime. */
	static void StampBlob(FSeaGridData& Grid, const FIntPoint& CenterCell, float RadiusInCells,
		float ThreatScale, float LethalFraction)
	{
		const FVector Center = Grid.CellToWorld(CenterCell);
		const float Radius = RadiusInCells * Grid.GetCellSize();

		Grid.ForEachCellInRadius(Center, Radius, [&Grid, Radius, ThreatScale, LethalFraction](const FIntPoint& Cell, float Distance)
		{
			const float Cost = FThreatEvaluator::CellThreatCost(Distance, Radius, ThreatScale, 2.0f, LethalFraction);
			if (Cost > 0.0f)
			{
				Grid.AddThreatCost(Cell, Cost);
			}
		});
	}
}

//---------------------------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNavalNavGridGeometryTest, "NavalNav.SeaGrid.GridGeometry",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FNavalNavGridGeometryTest::RunTest(const FString& Parameters)
{
	FSeaGridData Grid = NavalNavTest::MakeGrid(20);

	TestTrue(TEXT("Grid is built"), Grid.IsBuilt());
	TestEqual(TEXT("Cell count on X"), Grid.GetNumCellsX(), 20);
	TestEqual(TEXT("Cell count on Y"), Grid.GetNumCellsY(), 20);

	for (int32 Y = 0; Y < Grid.GetNumCellsY(); ++Y)
	{
		for (int32 X = 0; X < Grid.GetNumCellsX(); ++X)
		{
			const FIntPoint Cell(X, Y);
			if (Grid.WorldToCell(Grid.CellToWorld(Cell)) != Cell)
			{
				AddError(FString::Printf(TEXT("World/cell round trip failed for cell %s"), *Cell.ToString()));
				return false;
			}
		}
	}

	TestFalse(TEXT("Negative cell is outside the grid"), Grid.IsValidCell(FIntPoint(-1, 0)));
	TestTrue(TEXT("Cells outside the grid are impassable"), Grid.GetCellCost(FIntPoint(-1, 0)) >= NavalNav::ImpassableCost);
	TestTrue(TEXT("Clamping pulls a cell back into range"), Grid.ClampCell(FIntPoint(-5, 99)) == FIntPoint(0, 19));

	// An absurd configuration must be clamped rather than allocated.
	FSeaGridConfig Huge;
	Huge.CellSize = 1.0f;
	Huge.Extent = FVector2D(500000.0, 500000.0);
	FSeaGridData HugeGrid;
	HugeGrid.Rebuild(Huge);
	TestTrue(TEXT("Cell count is clamped"), HugeGrid.GetNumCells() <= NavalNav::MaxGridCells);
	TestTrue(TEXT("Cell size grew to honour the clamp"), HugeGrid.GetCellSize() > 1.0f);

	return true;
}

//---------------------------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNavalNavStraightPathTest, "NavalNav.SeaGrid.StraightPathAcrossOpenWater",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FNavalNavStraightPathTest::RunTest(const FString& Parameters)
{
	FSeaGridData Grid = NavalNavTest::MakeGrid(20);
	FSeaGridPathfinder Pathfinder;
	FSeaGridPathQuery Query;

	const FIntPoint Start(2, 2);
	const FIntPoint Goal(17, 2);
	const FNavalPath Path = Pathfinder.FindPath(Grid, Start, Goal, Query);

	TestTrue(TEXT("A route exists across open water"), Path.bSuccess);
	TestEqual(TEXT("Costs and waypoints are parallel arrays"), Path.Costs.Num(), Path.Waypoints.Num());
	TestEqual(TEXT("A straight run collapses to its endpoints"), Path.Num(), 2);
	TestTrue(TEXT("Route starts at the start cell"), Grid.WorldToCell(Path.Waypoints[0]) == Start);
	TestTrue(TEXT("Route ends at the goal cell"), Grid.WorldToCell(Path.Waypoints.Last()) == Goal);
	TestEqual(TEXT("Accumulated cost starts at zero"), Path.Costs[0], 0.0f, 1.0e-4f);
	TestEqual(TEXT("Fifteen open-water steps cost fifteen"), Path.GetTotalCost(), 15.0f, 1.0e-3f);
	TestEqual(TEXT("Route is 1500 uu long"), Path.GetLength(), 1500.0f, 1.0e-2f);

	// A diagonal run must be charged sqrt(2) per step, not 2.
	const FNavalPath Diagonal = Pathfinder.FindPath(Grid, FIntPoint(2, 2), FIntPoint(12, 12), Query);
	TestTrue(TEXT("Diagonal route exists"), Diagonal.bSuccess);
	TestEqual(TEXT("Diagonal steps cost sqrt(2)"), Diagonal.GetTotalCost(), 10.0f * 1.41421356f, 1.0e-3f);

	const FNavalPath Trivial = Pathfinder.FindPath(Grid, Start, Start, Query);
	TestTrue(TEXT("Start equal to goal succeeds"), Trivial.bSuccess);
	TestEqual(TEXT("Degenerate route is a single waypoint"), Trivial.Num(), 1);

	return true;
}

//---------------------------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNavalNavThreatAvoidanceTest, "NavalNav.SeaGrid.PathBendsAroundThreat",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FNavalNavThreatAvoidanceTest::RunTest(const FString& Parameters)
{
	FSeaGridData Grid = NavalNavTest::MakeGrid(40);
	FSeaGridPathfinder Pathfinder;
	FSeaGridPathQuery Query;

	const FIntPoint Start(2, 20);
	const FIntPoint Goal(37, 20);

	const FNavalPath Clear = Pathfinder.FindPath(Grid, Start, Goal, Query);
	TestTrue(TEXT("Route across empty water is straight"), Clear.bSuccess && Clear.Num() == 2);

	NavalNavTest::StampBlob(Grid, FIntPoint(20, 20), 8.0f, 20.0f, 0.4f);
	TestTrue(TEXT("Blob centre is impassable"), Grid.GetCellCost(FIntPoint(20, 20)) >= NavalNav::ImpassableCost);
	TestTrue(TEXT("Blob edge is merely expensive"), Grid.GetThreatCost(FIntPoint(20, 26)) > 0.0f);
	TestEqual(TEXT("Threat does not leak past the radius"), Grid.GetThreatCost(FIntPoint(20, 30)), 0.0f, 1.0e-4f);

	const FNavalPath Bent = Pathfinder.FindPath(Grid, Start, Goal, Query);
	TestTrue(TEXT("A route around the blob exists"), Bent.bSuccess);
	TestTrue(TEXT("The route is no longer a straight line"), Bent.Num() > 2);
	TestTrue(TEXT("Avoiding the blob costs more than open water"), Bent.GetTotalCost() > Clear.GetTotalCost());

	float MaxDeviation = 0.0f;
	for (int32 Index = 0; Index < Bent.Num(); ++Index)
	{
		const FIntPoint Cell = Grid.WorldToCell(Bent.Waypoints[Index]);
		TestTrue(TEXT("No waypoint sits in the lethal core"), Grid.GetCellCost(Cell) < NavalNav::ImpassableCost);
		MaxDeviation = FMath::Max(MaxDeviation, static_cast<float>(FMath::Abs(Cell.Y - 20)));

		if (Index > 0)
		{
			float LegCost = 0.0f;
			const bool bClearLeg = FSeaGridPathfinder::SampleSegment(Grid,
				[&Grid](const FIntPoint& C) { return Grid.GetCellCost(C); },
				Grid.WorldToCell(Bent.Waypoints[Index - 1]), Cell, LegCost);
			TestTrue(TEXT("Every leg stays clear of the lethal core"), bClearLeg);
		}

		if (Index > 0)
		{
			TestTrue(TEXT("Accumulated costs are monotonic"), Bent.Costs[Index] >= Bent.Costs[Index - 1]);
		}
	}
	TestTrue(TEXT("The route goes around the blob, not merely alongside it"), MaxDeviation >= 3.0f);

	// A ship strong enough to ignore the zone plans straight through it: the cost functor is
	// the whole hook, which is what makes per-ship threat evaluation possible.
	const FNavalPath Fearless = Pathfinder.FindPath(Grid, Start, Goal,
		[](const FIntPoint&) { return NavalNav::OpenWaterCost; }, Query);
	TestTrue(TEXT("Ignoring the threat restores the straight route"), Fearless.bSuccess && Fearless.Num() == 2);

	return true;
}

//---------------------------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNavalNavNoPathTest, "NavalNav.SeaGrid.NoPathWhenWalled",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FNavalNavNoPathTest::RunTest(const FString& Parameters)
{
	FSeaGridData Grid = NavalNavTest::MakeGrid(20);
	for (int32 Y = 0; Y < Grid.GetNumCellsY(); ++Y)
	{
		Grid.SetBaseCost(FIntPoint(10, Y), NavalNav::ImpassableCost);
	}

	FSeaGridPathfinder Pathfinder;
	FSeaGridPathQuery Query;

	const FNavalPath Blocked = Pathfinder.FindPath(Grid, FIntPoint(2, 10), FIntPoint(18, 10), Query);
	TestFalse(TEXT("A fully walled sea has no route"), Blocked.bSuccess);
	TestEqual(TEXT("A failed route carries no waypoints"), Blocked.Num(), 0);
	TestTrue(TEXT("The search still ran"), Blocked.CellsSearched > 0);

	Grid.SetBaseCost(FIntPoint(10, 4), NavalNav::OpenWaterCost);
	const FNavalPath Through = Pathfinder.FindPath(Grid, FIntPoint(2, 10), FIntPoint(18, 10), Query);
	TestTrue(TEXT("One gate is enough"), Through.bSuccess);

	// Endpoints inside land are a failure, not a nearest-reachable fallback.
	TestFalse(TEXT("Starting inside land fails"),
		Pathfinder.FindPath(Grid, FIntPoint(10, 10), FIntPoint(18, 10), Query).bSuccess);
	TestFalse(TEXT("Ending inside land fails"),
		Pathfinder.FindPath(Grid, FIntPoint(2, 10), FIntPoint(10, 10), Query).bSuccess);
	TestFalse(TEXT("Off-grid endpoints fail"),
		Pathfinder.FindPath(Grid, FIntPoint(-1, 0), FIntPoint(5, 5), Query).bSuccess);

	// The budget must cut a hopeless search short rather than let it run the whole grid.
	FSeaGridPathQuery Budgeted;
	Budgeted.MaxSearchedCells = 25;
	const FNavalPath Starved = Pathfinder.FindPath(Grid, FIntPoint(2, 10), FIntPoint(18, 10), Budgeted);
	TestFalse(TEXT("An exhausted budget reports failure"), Starved.bSuccess);
	TestTrue(TEXT("An exhausted budget is respected"), Starved.CellsSearched <= 25);

	return true;
}

//---------------------------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNavalNavCornerCuttingTest, "NavalNav.SeaGrid.CornerCutting",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FNavalNavCornerCuttingTest::RunTest(const FString& Parameters)
{
	FSeaGridData Grid = NavalNavTest::MakeGrid(4);
	Grid.SetBaseCost(FIntPoint(1, 0), NavalNav::ImpassableCost);
	Grid.SetBaseCost(FIntPoint(0, 1), NavalNav::ImpassableCost);

	FSeaGridPathfinder Pathfinder;
	FSeaGridPathQuery Query;

	TestFalse(TEXT("A ship cannot squeeze between two touching obstacles"),
		Pathfinder.FindPath(Grid, FIntPoint(0, 0), FIntPoint(1, 1), Query).bSuccess);

	Query.bPreventCornerCutting = false;
	TestTrue(TEXT("Disabling the corner rule allows the diagonal"),
		Pathfinder.FindPath(Grid, FIntPoint(0, 0), FIntPoint(1, 1), Query).bSuccess);

	return true;
}

//---------------------------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNavalNavStringPullTest, "NavalNav.SeaGrid.StringPullRespectsCost",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FNavalNavStringPullTest::RunTest(const FString& Parameters)
{
	FSeaGridData Grid = NavalNavTest::MakeGrid(40);
	// Expensive but crossable: a naive line-of-sight string pull would cut straight back
	// through the blob the search just spent cost avoiding.
	NavalNavTest::StampBlob(Grid, FIntPoint(20, 20), 8.0f, 25.0f, 0.0f);

	FSeaGridPathfinder Pathfinder;

	FSeaGridPathQuery Pulled;
	const FNavalPath Smoothed = Pathfinder.FindPath(Grid, FIntPoint(2, 20), FIntPoint(37, 20), Pulled);

	FSeaGridPathQuery Raw;
	Raw.bStringPull = false;
	Raw.bRemoveCollinearWaypoints = false;
	const FNavalPath Unsmoothed = Pathfinder.FindPath(Grid, FIntPoint(2, 20), FIntPoint(37, 20), Raw);

	TestTrue(TEXT("Both routes exist"), Smoothed.bSuccess && Unsmoothed.bSuccess);
	TestTrue(TEXT("Smoothing removes waypoints"), Smoothed.Num() < Unsmoothed.Num());
	TestTrue(TEXT("Smoothing stays within tolerance of the raw route"),
		Smoothed.GetTotalCost() <= Unsmoothed.GetTotalCost() * 1.05f + 1.0e-3f);

	const float CenterCost = Grid.GetCellCost(FIntPoint(20, 20));
	for (const FVector& Waypoint : Smoothed.Waypoints)
	{
		TestTrue(TEXT("No shortcut lands in the most dangerous water"),
			Grid.GetCellCost(Grid.WorldToCell(Waypoint)) < CenterCost);
	}

	return true;
}

//---------------------------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNavalNavThreatEvaluatorTest, "NavalNav.Threat.Evaluator",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FNavalNavThreatEvaluatorTest::RunTest(const FString& Parameters)
{
	TestTrue(TEXT("A stronger zone is hostile"), FThreatEvaluator::IsHostile(1.0f, 3.0f, 0.0f));
	TestFalse(TEXT("A weaker zone is ignored"), FThreatEvaluator::IsHostile(5.0f, 3.0f, 0.0f));
	TestFalse(TEXT("An evenly matched zone is ignored"), FThreatEvaluator::IsHostile(3.0f, 3.0f, 0.0f));
	TestFalse(TEXT("The threshold raises the bar"), FThreatEvaluator::IsHostile(1.0f, 3.0f, 5.0f));

	TestEqual(TEXT("Twice as strong reads as 1.0"), FThreatEvaluator::RelativeThreat(2.0f, 4.0f), 1.0f, 1.0e-4f);
	TestEqual(TEXT("A weaker zone reads as 0.0"), FThreatEvaluator::RelativeThreat(4.0f, 2.0f), 0.0f, 1.0e-4f);
	TestTrue(TEXT("An unarmed observer does not divide by zero"), FThreatEvaluator::RelativeThreat(0.0f, 4.0f) > 0.0f);

	const float Radius = 1000.0f;
	float Previous = FThreatEvaluator::CellThreatCost(0.0f, Radius, 10.0f, 2.0f, 0.0f);
	TestEqual(TEXT("Cost peaks at the centre"), Previous, 10.0f, 1.0e-3f);
	for (float Distance = 100.0f; Distance < Radius; Distance += 100.0f)
	{
		const float Cost = FThreatEvaluator::CellThreatCost(Distance, Radius, 10.0f, 2.0f, 0.0f);
		TestTrue(TEXT("Cost falls off strictly with distance"), Cost < Previous && Cost > 0.0f);
		Previous = Cost;
	}
	TestEqual(TEXT("Cost is zero at the edge"),
		FThreatEvaluator::CellThreatCost(Radius, Radius, 10.0f, 2.0f, 0.0f), 0.0f, 1.0e-4f);
	TestTrue(TEXT("The lethal core is impassable"),
		FThreatEvaluator::CellThreatCost(100.0f, Radius, 10.0f, 2.0f, 0.25f) >= NavalNav::ImpassableCost);
	TestTrue(TEXT("Water outside the lethal core is crossable"),
		FThreatEvaluator::CellThreatCost(300.0f, Radius, 10.0f, 2.0f, 0.25f) < NavalNav::ImpassableCost);

	// Overlapping zones stack instead of overwriting each other.
	FSeaGridData Grid = NavalNavTest::MakeGrid(20);
	NavalNavTest::StampBlob(Grid, FIntPoint(10, 10), 5.0f, 4.0f, 0.0f);
	const float Single = Grid.GetThreatCost(FIntPoint(10, 10));
	NavalNavTest::StampBlob(Grid, FIntPoint(10, 10), 5.0f, 4.0f, 0.0f);
	TestEqual(TEXT("Two zones stack"), Grid.GetThreatCost(FIntPoint(10, 10)), 2.0f * Single, 1.0e-3f);

	Grid.ClearThreat();
	TestEqual(TEXT("Clearing threat leaves open water"), Grid.GetCellCost(FIntPoint(10, 10)),
		NavalNav::OpenWaterCost, 1.0e-4f);

	return true;
}

//---------------------------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNavalNavBufferReuseTest, "NavalNav.SeaGrid.BufferReuseAcrossQueries",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FNavalNavBufferReuseTest::RunTest(const FString& Parameters)
{
	FSeaGridData Grid = NavalNavTest::MakeGrid(30);
	NavalNavTest::StampBlob(Grid, FIntPoint(15, 15), 6.0f, 12.0f, 0.3f);

	FSeaGridPathfinder Pathfinder;
	FSeaGridPathQuery Query;

	const FNavalPath First = Pathfinder.FindPath(Grid, FIntPoint(1, 1), FIntPoint(28, 28), Query);
	TestTrue(TEXT("The first plan succeeds"), First.bSuccess);

	// The generation-stamp trick is where a stale-state bug would hide, so hammer it.
	for (int32 Iteration = 0; Iteration < 50; ++Iteration)
	{
		const FNavalPath Other = Pathfinder.FindPath(Grid, FIntPoint(28, 1), FIntPoint(1, 28), Query);
		const FNavalPath Repeat = Pathfinder.FindPath(Grid, FIntPoint(1, 1), FIntPoint(28, 28), Query);

		if (!Other.bSuccess || Repeat.Num() != First.Num() || Repeat.CellsSearched != First.CellsSearched
			|| !FMath::IsNearlyEqual(Repeat.GetTotalCost(), First.GetTotalCost(), 1.0e-4f))
		{
			AddError(FString::Printf(TEXT("Reused buffers produced a different result on iteration %d"), Iteration));
			return false;
		}
	}

	// A different grid size must not trip over the buffers sized for the previous one.
	FSeaGridData Smaller = NavalNavTest::MakeGrid(12);
	TestTrue(TEXT("A smaller grid plans correctly with reused buffers"),
		Pathfinder.FindPath(Smaller, FIntPoint(0, 0), FIntPoint(11, 11), Query).bSuccess);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
