// Copyright (c) 2026 Kaan Altay. Licensed under the MIT License.

#include "Grid/SeaGridPathfinder.h"

#include "Algo/Reverse.h"

namespace
{
	/** Cost of a diagonal step relative to an orthogonal one. */
	constexpr float DiagonalStep = 1.4142135623730951f;

	/** The eight-neighbourhood, orthogonals first so equal-cost ties prefer straight moves. */
	constexpr int32 NumNeighbours = 8;
	constexpr int32 NeighbourOffsetX[NumNeighbours] = { 1, -1, 0, 0, 1, 1, -1, -1 };
	constexpr int32 NeighbourOffsetY[NumNeighbours] = { 0, 0, 1, -1, 1, -1, 1, -1 };

	/**
	 * How far ahead the string-pull pass looks for a shortcut. Without a cap the pass is
	 * quadratic in path length and each test walks a line, which is easy to feel on a long
	 * ocean crossing. 64 cells is far beyond the turning radius of any ship in the sample.
	 */
	constexpr int32 MaxStringPullLookahead = 64;
}

float FSeaGridPathfinder::OctileDistance(const FIntPoint& From, const FIntPoint& To)
{
	const float DX = static_cast<float>(FMath::Abs(To.X - From.X));
	const float DY = static_cast<float>(FMath::Abs(To.Y - From.Y));

	// Straight run plus the discount earned by folding the shorter axis into diagonal steps.
	return (DX + DY) + (DiagonalStep - 2.0f) * FMath::Min(DX, DY);
}

bool FSeaGridPathfinder::SampleSegment(const FSeaGridData& Grid, FCellCostFunc CostFunc,
	const FIntPoint& From, const FIntPoint& To, float& OutCost)
{
	const int32 DeltaX = To.X - From.X;
	const int32 DeltaY = To.Y - From.Y;
	const int32 NumSteps = FMath::Max(FMath::Abs(DeltaX), FMath::Abs(DeltaY));

	if (!Grid.IsValidCell(From) || CostFunc(From) >= NavalNav::ImpassableCost)
	{
		return false;
	}

	if (NumSteps == 0)
	{
		OutCost = 0.0f;
		return true;
	}

	// Cost model matches the search: every cell entered is charged its own cost, scaled by the
	// distance travelled to enter it. Sampling at NumSteps points keeps the two in agreement.
	const float SegmentLength = FMath::Sqrt(static_cast<float>(DeltaX * DeltaX + DeltaY * DeltaY));
	const float PerStepLength = SegmentLength / NumSteps;

	float Total = 0.0f;
	for (int32 Step = 1; Step <= NumSteps; ++Step)
	{
		const float Alpha = static_cast<float>(Step) / static_cast<float>(NumSteps);
		const FIntPoint Cell(
			From.X + FMath::RoundToInt32(DeltaX * Alpha),
			From.Y + FMath::RoundToInt32(DeltaY * Alpha));

		if (!Grid.IsValidCell(Cell))
		{
			return false;
		}

		const float Cost = CostFunc(Cell);
		if (Cost >= NavalNav::ImpassableCost)
		{
			return false;
		}

		Total += Cost * PerStepLength;
	}

	OutCost = Total;
	return true;
}

bool FSeaGridPathfinder::FindNearestCellBelowCost(const FSeaGridData& Grid, const FIntPoint& Origin,
	FCellCostFunc CostFunc, float AcceptCost, int32 MaxRing, FIntPoint& OutCell)
{
	if (!Grid.IsBuilt())
	{
		return false;
	}

	bool bHaveBest = false;
	float BestCost = TNumericLimits<float>::Max();
	FIntPoint BestCell = Origin;

	// Returns true when Cell is acceptable (nearest-safe found); otherwise records the least-cost
	// passable cell, which is the fallback exit when nothing acceptable is in range.
	auto Consider = [&](const FIntPoint& Cell) -> bool
	{
		if (!Grid.IsValidCell(Cell))
		{
			return false;
		}
		const float Cost = CostFunc(Cell);
		if (Cost >= NavalNav::ImpassableCost)
		{
			return false; // land or a lethal core is never a valid target
		}
		if (Cost <= AcceptCost)
		{
			OutCell = Cell;
			return true;
		}
		if (Cost < BestCost)
		{
			BestCost = Cost;
			BestCell = Cell;
			bHaveBest = true;
		}
		return false;
	};

	if (Consider(Origin))
	{
		return true;
	}
	for (int32 Ring = 1; Ring <= MaxRing; ++Ring)
	{
		for (int32 DX = -Ring; DX <= Ring; ++DX)
		{
			if (Consider(FIntPoint(Origin.X + DX, Origin.Y - Ring))) { return true; }
			if (Consider(FIntPoint(Origin.X + DX, Origin.Y + Ring))) { return true; }
		}
		for (int32 DY = -Ring + 1; DY <= Ring - 1; ++DY)
		{
			if (Consider(FIntPoint(Origin.X - Ring, Origin.Y + DY))) { return true; }
			if (Consider(FIntPoint(Origin.X + Ring, Origin.Y + DY))) { return true; }
		}
	}

	if (bHaveBest)
	{
		OutCell = BestCell; // least-bad exit through the weakest part of the enclosure
		return true;
	}
	return false;
}

void FSeaGridPathfinder::ReleaseBuffers()
{
	GScore.Empty();
	CameFrom.Empty();
	VisitStamp.Empty();
	ClosedFlags.Empty();
	OpenHeap.Empty();
	CellPath.Empty();
	SimplifiedPath.Empty();
	CurrentStamp = 0;
}

void FSeaGridPathfinder::BeginQuery(int32 NumCells)
{
	if (VisitStamp.Num() != NumCells)
	{
		// SetNumUninitialized keeps the allocation when a grid is rebuilt at the same size.
		GScore.SetNumUninitialized(NumCells);
		CameFrom.SetNumUninitialized(NumCells);
		ClosedFlags.SetNumUninitialized(NumCells);
		VisitStamp.Reset();
		VisitStamp.AddZeroed(NumCells);
		CurrentStamp = 0;
	}

	// A generation counter replaces clearing the visited array between queries. On wrap the
	// stamps really do have to be reset once, which happens after four billion plans.
	++CurrentStamp;
	if (CurrentStamp == 0)
	{
		for (uint32& Stamp : VisitStamp)
		{
			Stamp = 0;
		}
		CurrentStamp = 1;
	}

	OpenHeap.Reset();
	LastSearchedCells = 0;
}

void FSeaGridPathfinder::HeapPush(const FOpenNode& Node)
{
	int32 Index = OpenHeap.Add(Node);

	// Sift up.
	while (Index > 0)
	{
		const int32 Parent = (Index - 1) / 2;
		if (OpenHeap[Parent].FScore <= OpenHeap[Index].FScore)
		{
			break;
		}
		Swap(OpenHeap[Parent], OpenHeap[Index]);
		Index = Parent;
	}
}

FSeaGridPathfinder::FOpenNode FSeaGridPathfinder::HeapPop()
{
	const FOpenNode Result = OpenHeap[0];
	OpenHeap[0] = OpenHeap.Last();
	OpenHeap.Pop(EAllowShrinking::No);

	// Sift down.
	int32 Index = 0;
	const int32 Count = OpenHeap.Num();
	while (true)
	{
		const int32 Left = 2 * Index + 1;
		const int32 Right = Left + 1;
		int32 Smallest = Index;

		if (Left < Count && OpenHeap[Left].FScore < OpenHeap[Smallest].FScore)
		{
			Smallest = Left;
		}
		if (Right < Count && OpenHeap[Right].FScore < OpenHeap[Smallest].FScore)
		{
			Smallest = Right;
		}
		if (Smallest == Index)
		{
			break;
		}

		Swap(OpenHeap[Index], OpenHeap[Smallest]);
		Index = Smallest;
	}

	return Result;
}

FNavalPath FSeaGridPathfinder::FindPath(const FSeaGridData& Grid, const FIntPoint& StartCell, const FIntPoint& GoalCell,
	const FSeaGridPathQuery& Query)
{
	return FindPath(Grid, StartCell, GoalCell,
		[&Grid](const FIntPoint& Cell) { return Grid.GetCellCost(Cell); },
		Query);
}

FNavalPath FSeaGridPathfinder::FindPathWorld(const FSeaGridData& Grid, const FVector& Start, const FVector& Goal,
	const FSeaGridPathQuery& Query)
{
	// Ships routinely sit slightly outside the navigable rectangle (spawned at the border,
	// pushed out by wind). Clamping instead of failing keeps the demo robust.
	const FIntPoint StartCell = Grid.ClampCell(Grid.WorldToCell(Start));
	const FIntPoint GoalCell = Grid.ClampCell(Grid.WorldToCell(Goal));

	return FindPath(Grid, StartCell, GoalCell, Query);
}

FNavalPath FSeaGridPathfinder::FindPath(const FSeaGridData& Grid, const FIntPoint& StartCell, const FIntPoint& GoalCell,
	FCellCostFunc CostFunc, const FSeaGridPathQuery& Query)
{
	FNavalPath Path;

	if (!Grid.IsBuilt() || !Grid.IsValidCell(StartCell) || !Grid.IsValidCell(GoalCell))
	{
		return Path;
	}

	// A plan that begins or ends inside land is not a plan; report failure rather than
	// returning a route the follower cannot execute.
	if (CostFunc(StartCell) >= NavalNav::ImpassableCost || CostFunc(GoalCell) >= NavalNav::ImpassableCost)
	{
		return Path;
	}

	const int32 NumCells = Grid.GetNumCells();
	const int32 StartIndex = Grid.CellToIndex(StartCell);
	const int32 GoalIndex = Grid.CellToIndex(GoalCell);

	BeginQuery(NumCells);

	const float Weight = FMath::Max(1.0f, Query.HeuristicWeight);

	GScore[StartIndex] = 0.0f;
	CameFrom[StartIndex] = INDEX_NONE;
	ClosedFlags[StartIndex] = 0;
	VisitStamp[StartIndex] = CurrentStamp;
	HeapPush({ Weight * OctileDistance(StartCell, GoalCell) * NavalNav::OpenWaterCost, StartIndex });

	const int32 NeighbourCount = Query.bAllowDiagonal ? NumNeighbours : 4;
	bool bReachedGoal = false;

	while (OpenHeap.Num() > 0)
	{
		const FOpenNode Node = HeapPop();
		const int32 CurrentIndex = Node.CellIndex;

		// Lazy deletion: a cell can sit in the heap several times with stale scores.
		if (ClosedFlags[CurrentIndex] != 0)
		{
			continue;
		}
		ClosedFlags[CurrentIndex] = 1;
		++LastSearchedCells;

		if (CurrentIndex == GoalIndex)
		{
			bReachedGoal = true;
			break;
		}

		if (Query.MaxSearchedCells > 0 && LastSearchedCells >= Query.MaxSearchedCells)
		{
			// Budget exhausted. Reporting failure is honest: the caller can widen the budget
			// or fall back, which beats handing a follower a route to nowhere.
			break;
		}

		const FIntPoint CurrentCell = Grid.IndexToCell(CurrentIndex);
		const float CurrentG = GScore[CurrentIndex];

		for (int32 Neighbour = 0; Neighbour < NeighbourCount; ++Neighbour)
		{
			const int32 OffsetX = NeighbourOffsetX[Neighbour];
			const int32 OffsetY = NeighbourOffsetY[Neighbour];
			const FIntPoint NextCell(CurrentCell.X + OffsetX, CurrentCell.Y + OffsetY);

			if (!Grid.IsValidCell(NextCell))
			{
				continue;
			}

			const int32 NextIndex = Grid.CellToIndex(NextCell);
			if (VisitStamp[NextIndex] == CurrentStamp && ClosedFlags[NextIndex] != 0)
			{
				continue;
			}

			const float NextCost = CostFunc(NextCell);
			if (NextCost >= NavalNav::ImpassableCost)
			{
				continue;
			}

			const bool bDiagonal = (OffsetX != 0 && OffsetY != 0);
			if (bDiagonal && Query.bPreventCornerCutting)
			{
				const FIntPoint SideA(CurrentCell.X + OffsetX, CurrentCell.Y);
				const FIntPoint SideB(CurrentCell.X, CurrentCell.Y + OffsetY);
				const bool bSideABlocked = !Grid.IsValidCell(SideA) || CostFunc(SideA) >= NavalNav::ImpassableCost;
				const bool bSideBBlocked = !Grid.IsValidCell(SideB) || CostFunc(SideB) >= NavalNav::ImpassableCost;
				if (bSideABlocked || bSideBBlocked)
				{
					continue;
				}
			}

			const float StepLength = bDiagonal ? DiagonalStep : 1.0f;
			const float TentativeG = CurrentG + NextCost * StepLength;

			const bool bSeen = (VisitStamp[NextIndex] == CurrentStamp);
			if (bSeen && TentativeG >= GScore[NextIndex])
			{
				continue;
			}

			if (!bSeen)
			{
				VisitStamp[NextIndex] = CurrentStamp;
				ClosedFlags[NextIndex] = 0;
			}

			GScore[NextIndex] = TentativeG;
			CameFrom[NextIndex] = CurrentIndex;
			HeapPush({ TentativeG + Weight * OctileDistance(NextCell, GoalCell) * NavalNav::OpenWaterCost, NextIndex });
		}
	}

	Path.CellsSearched = LastSearchedCells;
	if (!bReachedGoal)
	{
		return Path;
	}

	BuildPath(Grid, CostFunc, Query, StartIndex, GoalIndex, Path);
	Path.CellsSearched = LastSearchedCells;
	Path.bSuccess = Path.Waypoints.Num() > 0;
	return Path;
}

void FSeaGridPathfinder::BuildPath(const FSeaGridData& Grid, FCellCostFunc CostFunc, const FSeaGridPathQuery& Query,
	int32 StartIndex, int32 GoalIndex, FNavalPath& OutPath)
{
	// Walk the parent chain back to the start, then flip it.
	CellPath.Reset();
	for (int32 Index = GoalIndex; Index != INDEX_NONE; Index = CameFrom[Index])
	{
		CellPath.Add(Grid.IndexToCell(Index));
		if (Index == StartIndex)
		{
			break;
		}
	}
	Algo::Reverse(CellPath);

	if (Query.bRemoveCollinearWaypoints && CellPath.Num() > 2)
	{
		// A grid route is mostly long straight runs; only the corners carry information.
		SimplifiedPath.Reset();
		SimplifiedPath.Add(CellPath[0]);
		for (int32 Index = 1; Index < CellPath.Num() - 1; ++Index)
		{
			const FIntPoint Incoming = CellPath[Index] - CellPath[Index - 1];
			const FIntPoint Outgoing = CellPath[Index + 1] - CellPath[Index];
			if (Incoming != Outgoing)
			{
				SimplifiedPath.Add(CellPath[Index]);
			}
		}
		SimplifiedPath.Add(CellPath.Last());
		Swap(CellPath, SimplifiedPath);
	}

	if (Query.bStringPull && CellPath.Num() > 2)
	{
		// Greedy string pull: from each anchor, reach for the furthest waypoint whose straight
		// shortcut is passable and not meaningfully more expensive than the route it replaces.
		// Comparing costs (not just passability) is what keeps a shortcut from slicing through
		// the edge of a threat blob that the search deliberately avoided.
		const float Tolerance = 1.0f + FMath::Max(0.0f, Query.StringPullTolerance);

		SimplifiedPath.Reset();
		SimplifiedPath.Add(CellPath[0]);

		int32 Anchor = 0;
		while (Anchor < CellPath.Num() - 1)
		{
			int32 Best = Anchor + 1;
			const int32 FurthestCandidate = FMath::Min(Anchor + MaxStringPullLookahead, CellPath.Num() - 1);

			for (int32 Candidate = FurthestCandidate; Candidate > Anchor + 1; --Candidate)
			{
				float ShortcutCost = 0.0f;
				if (!SampleSegment(Grid, CostFunc, CellPath[Anchor], CellPath[Candidate], ShortcutCost))
				{
					continue;
				}

				// The search already knows what the original route costs.
				const float OriginalCost = GScore[Grid.CellToIndex(CellPath[Candidate])] - GScore[Grid.CellToIndex(CellPath[Anchor])];
				if (ShortcutCost <= OriginalCost * Tolerance + UE_KINDA_SMALL_NUMBER)
				{
					Best = Candidate;
					break;
				}
			}

			SimplifiedPath.Add(CellPath[Best]);
			Anchor = Best;
		}

		Swap(CellPath, SimplifiedPath);
	}

	// Emit world waypoints and the accumulated cost at each of them. Costs are re-sampled
	// rather than copied from GScore because string pulling changed the actual route.
	OutPath.Waypoints.Reset(CellPath.Num());
	OutPath.Costs.Reset(CellPath.Num());

	float Accumulated = 0.0f;
	for (int32 Index = 0; Index < CellPath.Num(); ++Index)
	{
		if (Index > 0)
		{
			float LegCost = 0.0f;
			if (!SampleSegment(Grid, CostFunc, CellPath[Index - 1], CellPath[Index], LegCost))
			{
				// Cannot happen for a route the search just produced, but a wrong cost is
				// better than a silently truncated path if it ever does.
				LegCost = GScore[Grid.CellToIndex(CellPath[Index])] - GScore[Grid.CellToIndex(CellPath[Index - 1])];
			}
			Accumulated += LegCost;
		}

		OutPath.Waypoints.Add(Grid.CellToWorld(CellPath[Index]));
		OutPath.Costs.Add(Accumulated);
	}
}
