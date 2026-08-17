// Copyright (c) 2026 Kaan Altay. Licensed under the MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Grid/SeaGridPathfinder.h"
#include "Grid/SeaGridTypes.h"
#include "Subsystems/WorldSubsystem.h"

#include "SeaGrid.generated.h"

class ADangerZone;

/** Broadcast when a danger zone moves or changes power. Carries the change's location and radius
 *  so a listener can cheaply decide whether it is close enough to its route to care. */
DECLARE_MULTICAST_DELEGATE_TwoParams(FOnSeaGridThreatChanged, const FVector& /*Location*/, float /*Radius*/);

/**
 * World-scoped owner of the sea grid.
 *
 * A UWorldSubsystem is the right home for this: exactly one grid per world, created and torn
 * down with the world, reachable from any actor without a manager pointer being wired up in a
 * level. Actors that produce threat (danger zones) register here; actors that consume it
 * (the debug actor now, the helmsman in Slice 3) ask for paths here.
 *
 * The threat layer is rebuilt lazily. Zones only raise a dirty flag when they move or change
 * power, and the layer is re-stamped at most once per frame, immediately before the first
 * query that needs it. Ten moving zones therefore cost one rebuild, not ten.
 */
UCLASS(Config = Game)
class NAVALNAV_API USeaGridSubsystem : public UWorldSubsystem
{
	GENERATED_BODY()

public:
	//~ Begin USubsystem interface
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	//~ End USubsystem interface

	//~ Begin UWorldSubsystem interface
	virtual bool DoesSupportWorldType(const EWorldType::Type WorldType) const override;
	//~ End UWorldSubsystem interface

	/** Rebuilds the grid from NewConfig, discarding all costs. Safe to call at runtime. */
	UFUNCTION(BlueprintCallable, Category = "Naval|Sea Grid")
	void ConfigureGrid(const FSeaGridConfig& NewConfig);

	/** Read-only view of the grid, for debug drawing and for tests. */
	const FSeaGridData& GetGrid() const { return Grid; }

	/** Mutable view. Used by danger zones while stamping their footprint. */
	FSeaGridData& GetMutableGrid() { return Grid; }

	UFUNCTION(BlueprintCallable, Category = "Naval|Sea Grid")
	bool IsValidCell(const FIntPoint& Cell) const { return Grid.IsValidCell(Cell); }

	UFUNCTION(BlueprintCallable, Category = "Naval|Sea Grid")
	FIntPoint WorldToCell(const FVector& WorldLocation) const { return Grid.WorldToCell(WorldLocation); }

	UFUNCTION(BlueprintCallable, Category = "Naval|Sea Grid")
	FVector CellToWorld(const FIntPoint& Cell) const { return Grid.CellToWorld(Cell); }

	/** Total cost (base + threat) of a cell, with the threat layer brought up to date first. */
	UFUNCTION(BlueprintCallable, Category = "Naval|Sea Grid")
	float GetCellCost(const FIntPoint& Cell);

	UFUNCTION(BlueprintCallable, Category = "Naval|Sea Grid")
	void SetThreatCost(const FIntPoint& Cell, float Cost) { Grid.SetThreatCost(Cell, Cost); }

	/** Clears the threat layer. Zones re-stamp themselves on the next rebuild. */
	UFUNCTION(BlueprintCallable, Category = "Naval|Sea Grid")
	void ClearThreat() { Grid.ClearThreat(); }

	/** Visits every cell with its current total cost. */
	void ForEachCell(TFunctionRef<void(const FIntPoint&, float)> Visitor) const { Grid.ForEachCell(Visitor); }

	/** Called by danger zones when their footprint changed; the layer is re-stamped lazily. */
	void MarkThreatDirty() { bThreatDirty = true; }

	/** Marks the threat layer dirty and tells listeners a zone at Location/Radius changed. */
	void NotifyZoneChanged(const FVector& Location, float Radius);

	/** Fires whenever a registered zone moves or changes power. Navigators subscribe to trigger replans. */
	FOnSeaGridThreatChanged OnThreatChanged;

	/**
	 * Traversal cost (base + threat) at a world point for a *specific* observer, computed directly
	 * from the registered zones without touching the shared stamped layer. This is what a ship uses
	 * to reason about its own danger, since the stamped layer may currently be stamped for another
	 * ship's power.
	 */
	float GetObserverCellCost(const FVector& WorldLocation, float ObserverPowerLevel, float InHostilityThreshold) const;

	/**
	 * Bounded outward search for somewhere safer to be. Returns the nearest cell whose observer
	 * cost is open water; failing that, the least-bad passable cell found within SearchRadius (the
	 * lowest-cost exit when a ship is boxed in). Returns false only if no passable cell exists at all.
	 */
	bool FindEscapeTarget(const FVector& From, float ObserverPowerLevel, float InHostilityThreshold,
		float SearchRadius, FVector& OutTarget) const;

	/** Re-stamps the threat layer from all registered zones if anything changed since last time. */
	void EnsureThreatUpToDate();

	/** Unconditional re-stamp of the threat layer from all registered zones. */
	void RebuildThreatLayer();

	void RegisterDangerZone(ADangerZone* Zone);
	void UnregisterDangerZone(ADangerZone* Zone);

	/**
	 * Plans a route between two world positions.
	 *
	 * Uses a subsystem-owned pathfinder so the open/closed buffers survive between calls;
	 * replanning every second on a 200x200 grid then costs no heap traffic after the first
	 * query. Not re-entrant, which is fine while planning happens on the game thread.
	 */
	UFUNCTION(BlueprintCallable, Category = "Naval|Sea Grid")
	FNavalPath FindPath(const FVector& Start, const FVector& Goal, const FSeaGridPathQuery& Query);

	/** Power level a route is planned for. Zones weaker than this by HostilityThreshold are ignored. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Config, Category = "Naval|Threat")
	float ObserverPowerLevel = 1.0f;

	/** How much stronger a zone has to be before it is treated as a threat. See FThreatEvaluator. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Config, Category = "Naval|Threat")
	float HostilityThreshold = 0.0f;

	/** Grid used when nothing configured one explicitly. Editable in DefaultGame.ini. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Config, Category = "Naval|Sea Grid")
	FSeaGridConfig DefaultConfig;

private:
	FSeaGridData Grid;

	/** Persistent scratch buffers for A*. Mutable state, hence not exposed. */
	FSeaGridPathfinder Pathfinder;

	UPROPERTY()
	TArray<TWeakObjectPtr<ADangerZone>> DangerZones;

	bool bThreatDirty = true;
};
