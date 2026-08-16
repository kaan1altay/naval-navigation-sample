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
