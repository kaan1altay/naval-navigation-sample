// Copyright (c) 2026 Kaan Altay. Licensed under the MIT License.

#include "Grid/SeaGrid.h"

#include "Engine/World.h"
#include "NavalNav.h"
#include "Threat/DangerZone.h"

void USeaGridSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	Grid.Rebuild(DefaultConfig);
	bThreatDirty = true;

	UE_LOG(LogNavalNav, Log, TEXT("Sea grid built: %dx%d cells at %.0f uu (%.0f x %.0f uu area)."),
		Grid.GetNumCellsX(), Grid.GetNumCellsY(), Grid.GetCellSize(),
		2.0 * Grid.GetConfig().Extent.X, 2.0 * Grid.GetConfig().Extent.Y);
}

void USeaGridSubsystem::Deinitialize()
{
	DangerZones.Empty();
	Pathfinder.ReleaseBuffers();
	Grid.Reset();

	Super::Deinitialize();
}

bool USeaGridSubsystem::DoesSupportWorldType(const EWorldType::Type WorldType) const
{
	// Editor worlds are included so the debug actor can draw the grid without pressing Play.
	return WorldType == EWorldType::Game
		|| WorldType == EWorldType::PIE
		|| WorldType == EWorldType::Editor;
}

void USeaGridSubsystem::ConfigureGrid(const FSeaGridConfig& NewConfig)
{
	Grid.Rebuild(NewConfig);

	// The new grid has an empty threat layer; every registered zone has to stamp itself again.
	bThreatDirty = true;

	UE_LOG(LogNavalNav, Log, TEXT("Sea grid reconfigured: %dx%d cells at %.0f uu."),
		Grid.GetNumCellsX(), Grid.GetNumCellsY(), Grid.GetCellSize());
}

float USeaGridSubsystem::GetCellCost(const FIntPoint& Cell)
{
	EnsureThreatUpToDate();
	return Grid.GetCellCost(Cell);
}

void USeaGridSubsystem::RegisterDangerZone(ADangerZone* Zone)
{
	if (!Zone)
	{
		return;
	}

	DangerZones.AddUnique(Zone);
	bThreatDirty = true;
}

void USeaGridSubsystem::UnregisterDangerZone(ADangerZone* Zone)
{
	if (!Zone)
	{
		return;
	}

	if (DangerZones.Remove(Zone) > 0)
	{
		// The removed zone's cost is still baked into the layer, so it has to be re-stamped.
		bThreatDirty = true;
	}
}

void USeaGridSubsystem::EnsureThreatUpToDate()
{
	if (bThreatDirty)
	{
		RebuildThreatLayer();
	}
}

void USeaGridSubsystem::RebuildThreatLayer()
{
	if (!Grid.IsBuilt())
	{
		return;
	}

	// Clear-and-restamp rather than incremental add/remove. Zones overlap and stack, so
	// subtracting a moving zone's old footprint would need per-zone bookkeeping to stay exact;
	// re-stamping is O(zones x footprint) and inherently correct.
	Grid.ClearThreat();

	for (int32 Index = DangerZones.Num() - 1; Index >= 0; --Index)
	{
		const ADangerZone* Zone = DangerZones[Index].Get();
		if (!Zone)
		{
			DangerZones.RemoveAtSwap(Index, EAllowShrinking::No);
			continue;
		}

		Zone->StampThreat(Grid, ObserverPowerLevel, HostilityThreshold);
	}

	bThreatDirty = false;
}

FNavalPath USeaGridSubsystem::FindPath(const FVector& Start, const FVector& Goal, const FSeaGridPathQuery& Query)
{
	EnsureThreatUpToDate();
	return Pathfinder.FindPathWorld(Grid, Start, Goal, Query);
}

void USeaGridSubsystem::NotifyZoneChanged(const FVector& Location, float Radius)
{
	bThreatDirty = true;
	OnThreatChanged.Broadcast(Location, Radius);
}

float USeaGridSubsystem::GetObserverCellCost(const FVector& WorldLocation, float ObserverPower, float InHostilityThreshold) const
{
	const FIntPoint Cell = Grid.WorldToCell(WorldLocation);
	if (!Grid.IsValidCell(Cell))
	{
		return NavalNav::ImpassableCost;
	}

	float Cost = Grid.GetBaseCost(Cell);
	if (Cost >= NavalNav::ImpassableCost)
	{
		return Cost; // land: no point summing zones on top of infinity
	}

	// Sum every registered zone's contribution for THIS observer, straight from the zones rather
	// than the shared stamped layer (which may be stamped for a different ship right now).
	for (const TWeakObjectPtr<ADangerZone>& ZonePtr : DangerZones)
	{
		if (const ADangerZone* Zone = ZonePtr.Get())
		{
			Cost += Zone->GetThreatCostAt(WorldLocation, ObserverPower, InHostilityThreshold);
		}
	}
	return Cost;
}

bool USeaGridSubsystem::FindEscapeTarget(const FVector& From, float ObserverPower, float InHostilityThreshold,
	float SearchRadius, FVector& OutTarget) const
{
	if (!Grid.IsBuilt())
	{
		return false;
	}

	const float CellSize = FMath::Max(Grid.GetCellSize(), 1.0f);
	const int32 MaxRing = FMath::Clamp(FMath::CeilToInt32(SearchRadius / CellSize), 1, 80);
	const float SafeCost = NavalNav::OpenWaterCost + 0.01f;

	// The ring search is a shared, engine-free grid utility; here it is fed a cost functor that
	// rates each cell for THIS ship's power, straight from the zones.
	FIntPoint OutCell;
	const bool bFound = FSeaGridPathfinder::FindNearestCellBelowCost(Grid, Grid.WorldToCell(From),
		[this, ObserverPower, InHostilityThreshold](const FIntPoint& Cell)
		{
			return GetObserverCellCost(Grid.CellToWorld(Cell), ObserverPower, InHostilityThreshold);
		},
		SafeCost, MaxRing, OutCell);

	if (bFound)
	{
		OutTarget = Grid.CellToWorld(OutCell);
	}
	return bFound;
}
