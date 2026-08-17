// Copyright (c) 2026 Kaan Altay. Licensed under the MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Grid/SeaGridTypes.h"

#include "SeaGridPathfinder.generated.h"

/** Tunables for a single plan. Passed by value; defaults are what the demo uses. */
USTRUCT(BlueprintType)
struct NAVALNAV_API FSeaGridPathQuery
{
	GENERATED_BODY()

	/**
	 * Multiplier on the octile heuristic. 1.0 is admissible and returns the optimal route.
	 * Values above 1.0 trade optimality for search speed (weighted A*): 1.2 typically halves
	 * the number of expanded cells on an open sea grid while staying within a few percent of
	 * the optimal cost, which is a good deal for a ship that replans every few seconds.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pathfinding", meta = (ClampMin = "1.0"))
	float HeuristicWeight = 1.0f;

	/** Hard cap on expanded cells so a hopeless query cannot stall a frame. 0 means no cap. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pathfinding", meta = (ClampMin = "0"))
	int32 MaxSearchedCells = 0;

	/** Allow the eight-neighbourhood. Disabling it yields Manhattan-only routes (rarely useful). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pathfinding")
	bool bAllowDiagonal = true;

	/**
	 * Refuse a diagonal step when either orthogonal neighbour it passes is impassable, so a
	 * ship never plans through the gap between two touching islands.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pathfinding")
	bool bPreventCornerCutting = true;

	/** Drop waypoints that lie on a straight line between their neighbours. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pathfinding")
	bool bRemoveCollinearWaypoints = true;

	/**
	 * Additionally merge waypoints when the straight shortcut is not meaningfully more
	 * expensive than the grid route it replaces. This is the "string pull" pass; it is
	 * cost-aware on purpose, so a shortcut is never taken straight through a threat blob.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pathfinding")
	bool bStringPull = true;

	/** How much extra cost a shortcut may carry, as a fraction. 0.05 means "up to 5% worse". */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pathfinding", meta = (ClampMin = "0.0"))
	float StringPullTolerance = 0.05f;
};

/**
 * A* over FSeaGridData with an eight-neighbourhood and an octile heuristic.
 *
 * Kept as a stateful object rather than a free function so the open/closed buffers can be
 * reused across queries. The visited marker is a generation stamp compared against a counter
 * that increments per query, which avoids clearing 40k entries every time a ship replans.
 *
 * The cost of every cell comes from an injected functor, not from the grid directly, so the
 * same search can be run against "what the ship knows" rather than ground truth — that is
 * what Slice 4 needs for fog-of-war style replanning, and what the unit tests use to build
 * synthetic obstacles.
 */
struct NAVALNAV_API FSeaGridPathfinder
{
	/** Cost of entering a cell. Values >= NavalNav::ImpassableCost block the cell. */
	using FCellCostFunc = TFunctionRef<float(const FIntPoint&)>;

	/** Plans between two cells using an arbitrary cost function. */
	FNavalPath FindPath(const FSeaGridData& Grid, const FIntPoint& StartCell, const FIntPoint& GoalCell,
		FCellCostFunc CostFunc, const FSeaGridPathQuery& Query);

	/** Plans between two cells using the grid's own base + threat cost. */
	FNavalPath FindPath(const FSeaGridData& Grid, const FIntPoint& StartCell, const FIntPoint& GoalCell,
		const FSeaGridPathQuery& Query);

	/** Plans between two world positions, snapping both ends to the nearest valid cell. */
	FNavalPath FindPathWorld(const FSeaGridData& Grid, const FVector& Start, const FVector& Goal,
		const FSeaGridPathQuery& Query);

	/** Octile distance in cells, i.e. the cost of the cheapest possible route on open water. */
	static float OctileDistance(const FIntPoint& From, const FIntPoint& To);

	/**
	 * Walks the straight cell line between two cells, summing entry costs.
	 * Returns false (and leaves OutCost untouched) if any sampled cell is impassable.
	 * Shared by the string-pull pass and by the final cost recomputation.
	 */
	static bool SampleSegment(const FSeaGridData& Grid, FCellCostFunc CostFunc,
		const FIntPoint& From, const FIntPoint& To, float& OutCost);

	/**
	 * Bounded outward ring search for the nearest passable cell whose cost is at or below
	 * AcceptCost. If none is found within MaxRing rings, returns the least-cost passable cell seen
	 * instead — the least-bad exit when a ship is boxed in. Returns false only when there is no
	 * passable cell at all. Impassable cells (land, lethal cores) are never returned.
	 *
	 * This is how a ship finds its way out of hostile water: run it with a cost functor that rates
	 * cells for that ship's own power.
	 */
	static bool FindNearestCellBelowCost(const FSeaGridData& Grid, const FIntPoint& Origin,
		FCellCostFunc CostFunc, float AcceptCost, int32 MaxRing, FIntPoint& OutCell);

	/** Frees the scratch buffers. Only worth calling when a level is being torn down. */
	void ReleaseBuffers();

	/** Number of cells expanded by the most recent query. */
	int32 GetLastSearchedCells() const { return LastSearchedCells; }

private:
	/** Entry in the binary heap. Duplicates are tolerated and skipped on pop (lazy deletion). */
	struct FOpenNode
	{
		float FScore = 0.0f;
		int32 CellIndex = INDEX_NONE;
	};

	/** Grows and stamps the scratch buffers for a grid of NumCells cells. */
	void BeginQuery(int32 NumCells);

	bool IsVisited(int32 Index) const { return VisitStamp[Index] == CurrentStamp; }

	void HeapPush(const FOpenNode& Node);
	FOpenNode HeapPop();

	/** Turns the CameFrom chain into world waypoints, applying the simplification passes. */
	void BuildPath(const FSeaGridData& Grid, FCellCostFunc CostFunc, const FSeaGridPathQuery& Query,
		int32 StartIndex, int32 GoalIndex, FNavalPath& OutPath);

	TArray<float> GScore;
	TArray<int32> CameFrom;
	TArray<uint32> VisitStamp;
	TArray<uint8> ClosedFlags;
	TArray<FOpenNode> OpenHeap;

	/** Scratch for path reconstruction, kept here so BuildPath does not allocate per query. */
	TArray<FIntPoint> CellPath;
	TArray<FIntPoint> SimplifiedPath;

	uint32 CurrentStamp = 0;
	int32 LastSearchedCells = 0;
};
