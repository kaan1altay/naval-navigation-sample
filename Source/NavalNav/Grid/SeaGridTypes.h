// Copyright (c) 2026 Kaan Altay. Licensed under the MIT License.

#pragma once

#include "CoreMinimal.h"

#include "SeaGridTypes.generated.h"

namespace NavalNav
{
	/**
	 * Cells at or above this cost are treated as impassable by the pathfinder.
	 * A sentinel is used instead of a separate bitmask because threat is a continuous
	 * quantity: a land tile and a "certain death" tile are the same thing to the planner,
	 * and one comparison in the inner loop is cheaper than a second array lookup.
	 */
	inline constexpr float ImpassableCost = 1.0e9f;

	/** Base traversal cost of open water. Everything else is expressed as a multiple of this. */
	inline constexpr float OpenWaterCost = 1.0f;

	/**
	 * Upper bound on total cell count. 40 km x 40 km at 200 uu resolution is 40k cells; the
	 * clamp exists so a mistyped CellSize in a level cannot allocate gigabytes on BeginPlay.
	 */
	inline constexpr int32 MaxGridCells = 512 * 512;
}

/** Authoring-time description of the sea grid. Lives on the subsystem and on the debug actor. */
USTRUCT(BlueprintType)
struct NAVALNAV_API FSeaGridConfig
{
	GENERATED_BODY()

	/** World-space centre of the navigable area (XY only; Z is taken from SeaLevelZ). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sea Grid")
	FVector2D Center = FVector2D::ZeroVector;

	/** Half-size of the navigable area in world units. Default covers 40 km x 40 km. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sea Grid", meta = (ClampMin = "100.0"))
	FVector2D Extent = FVector2D(20000.0, 20000.0);

	/**
	 * Edge length of a single cell in world units. 200 uu is roughly a ship length, which is
	 * the coarsest resolution at which a hull can still be steered around a threat blob.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sea Grid", meta = (ClampMin = "10.0"))
	float CellSize = 200.0f;

	/** Height at which waypoints are emitted. The planner itself is purely 2D. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sea Grid")
	float SeaLevelZ = 0.0f;
};

/**
 * The grid itself: geometry plus two cost layers.
 *
 * Deliberately a plain USTRUCT rather than part of the subsystem, for two reasons:
 *  - the pathfinder can be unit-tested (and compiled) without a UWorld, and
 *  - the base layer can be built once and the threat layer re-stamped every frame without
 *    touching the static data.
 */
USTRUCT()
struct NAVALNAV_API FSeaGridData
{
	GENERATED_BODY()

	/** Allocates the cost layers for Config and fills the base layer with open water. */
	void Rebuild(const FSeaGridConfig& InConfig);

	/** Drops all storage. The grid reports zero cells until the next Rebuild. */
	void Reset();

	bool IsBuilt() const { return NumCellsX > 0 && NumCellsY > 0; }

	const FSeaGridConfig& GetConfig() const { return Config; }
	int32 GetNumCellsX() const { return NumCellsX; }
	int32 GetNumCellsY() const { return NumCellsY; }
	int32 GetNumCells() const { return NumCellsX * NumCellsY; }
	float GetCellSize() const { return Config.CellSize; }

	bool IsValidCell(const FIntPoint& Cell) const
	{
		return Cell.X >= 0 && Cell.Y >= 0 && Cell.X < NumCellsX && Cell.Y < NumCellsY;
	}

	/** Row-major flat index. Callers must have validated the cell. */
	int32 CellToIndex(const FIntPoint& Cell) const { return Cell.Y * NumCellsX + Cell.X; }
	FIntPoint IndexToCell(int32 Index) const { return FIntPoint(Index % NumCellsX, Index / NumCellsX); }

	/** Nearest cell to a world position. The result may be outside the grid; check IsValidCell. */
	FIntPoint WorldToCell(const FVector& WorldLocation) const;

	/** Centre of a cell in world space, at SeaLevelZ. */
	FVector CellToWorld(const FIntPoint& Cell) const;

	/** Clamps a cell into the grid. Useful when a ship drifts past the navigable border. */
	FIntPoint ClampCell(const FIntPoint& Cell) const;

	/** Static terrain cost only (land, shallows). 1.0 for open water. */
	float GetBaseCost(const FIntPoint& Cell) const;

	/** Base cost plus the current threat layer. This is what the planner consumes. */
	float GetCellCost(const FIntPoint& Cell) const;

	/** Threat contribution only, in multiples of OpenWaterCost. */
	float GetThreatCost(const FIntPoint& Cell) const;

	bool IsPassable(const FIntPoint& Cell) const
	{
		return IsValidCell(Cell) && GetCellCost(Cell) < NavalNav::ImpassableCost;
	}

	/** Overwrites the static layer, e.g. to carve out an island. */
	void SetBaseCost(const FIntPoint& Cell, float Cost);

	/** Overwrites the threat contribution of a cell. */
	void SetThreatCost(const FIntPoint& Cell, float Cost);

	/** Accumulates threat, so overlapping danger zones stack instead of overwriting. */
	void AddThreatCost(const FIntPoint& Cell, float Cost);

	/** Zeroes the whole threat layer, leaving the base layer intact. */
	void ClearThreat();

	/** Zeroes the threat layer inside a world-space disc (cheap partial clear). */
	void ClearThreatInRadius(const FVector& WorldCenter, float Radius);

	/** Visits every cell in row-major order. Signature: void(FIntPoint Cell, float TotalCost). */
	void ForEachCell(TFunctionRef<void(const FIntPoint&, float)> Visitor) const;

	/**
	 * Visits every cell whose centre lies within Radius of WorldCenter.
	 * Used by danger zones to stamp their footprint without walking the whole grid.
	 * Signature: void(FIntPoint Cell, float DistanceToCenter).
	 */
	void ForEachCellInRadius(const FVector& WorldCenter, float Radius, TFunctionRef<void(const FIntPoint&, float)> Visitor) const;

private:
	UPROPERTY()
	FSeaGridConfig Config;

	UPROPERTY()
	int32 NumCellsX = 0;

	UPROPERTY()
	int32 NumCellsY = 0;

	/** Static cost layer, built once. */
	UPROPERTY()
	TArray<float> BaseCost;

	/** Dynamic cost layer, re-stamped whenever a danger zone moves or changes power. */
	UPROPERTY()
	TArray<float> ThreatCost;
};

/** Result of a plan. Waypoints are world-space, Costs is the accumulated cost at each waypoint. */
USTRUCT(BlueprintType)
struct NAVALNAV_API FNavalPath
{
	GENERATED_BODY()

	/** Ordered world-space waypoints, start first and goal last. Empty when bSuccess is false. */
	UPROPERTY(BlueprintReadOnly, Category = "Naval Path")
	TArray<FVector> Waypoints;

	/**
	 * Accumulated traversal cost at each waypoint, so Costs.Num() == Waypoints.Num() and
	 * Costs[0] == 0. Per-leg cost is the difference of two entries; the follower in Slice 3
	 * uses this to decide where it is worth burning speed.
	 */
	UPROPERTY(BlueprintReadOnly, Category = "Naval Path")
	TArray<float> Costs;

	UPROPERTY(BlueprintReadOnly, Category = "Naval Path")
	bool bSuccess = false;

	/** Cells popped from the open set. Diagnostics only; drawn by the debug actor. */
	UPROPERTY(BlueprintReadOnly, Category = "Naval Path")
	int32 CellsSearched = 0;

	void Reset()
	{
		Waypoints.Reset();
		Costs.Reset();
		bSuccess = false;
		CellsSearched = 0;
	}

	int32 Num() const { return Waypoints.Num(); }

	/** Total cost of the plan, or 0 for an empty path. */
	float GetTotalCost() const { return Costs.Num() > 0 ? Costs.Last() : 0.0f; }

	/** Geometric length in world units, ignoring cost. */
	float GetLength() const
	{
		float Length = 0.0f;
		for (int32 Index = 1; Index < Waypoints.Num(); ++Index)
		{
			Length += static_cast<float>(FVector::Dist2D(Waypoints[Index - 1], Waypoints[Index]));
		}
		return Length;
	}
};
