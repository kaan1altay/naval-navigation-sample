// Copyright (c) 2026 Kaan Altay. Licensed under the MIT License.

#include "Demo/NavalNavDemoGameMode.h"

#include "Demo/NavalNavDemoPlayerController.h"
#include "Engine/World.h"
#include "Grid/SeaGrid.h"
#include "NavalNav.h"
#include "Navigation/NavalNavigatorComponent.h"
#include "Ship/SailingShipPawn.h"
#include "Ship/ShipPowerComponent.h"
#include "Ship/WindSubsystem.h"
#include "Threat/DangerZone.h"

ANavalNavDemoGameMode::ANavalNavDemoGameMode()
{
	PlayerControllerClass = ANavalNavDemoPlayerController::StaticClass();

	// We spawn and possess ships ourselves; there is no PlayerStart on a blank map, so leaving the
	// default pawn null avoids an orphan pawn at the origin.
	DefaultPawnClass = nullptr;
}

void ANavalNavDemoGameMode::BeginPlay()
{
	Super::BeginPlay();

	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	// --- Sea grid + wind ----------------------------------------------------------------------
	if (USeaGridSubsystem* SeaGrid = World->GetSubsystem<USeaGridSubsystem>())
	{
		SeaGrid->ConfigureGrid(GridConfig);
	}
	if (UWindSubsystem* Wind = World->GetSubsystem<UWindSubsystem>())
	{
		Wind->SetWindDirectionYaw(WindDirectionYaw);
		Wind->SetWindStrength(WindStrength);
	}

	// --- Danger zones -------------------------------------------------------------------------
	for (int32 Index = 0; Index < NumDangerZones; ++Index)
	{
		FActorSpawnParameters Params;
		Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		if (ADangerZone* Zone = World->SpawnActor<ADangerZone>(RandomSeaPoint(), FRotator::ZeroRotator, Params))
		{
			Zone->SetRadius(FMath::RandRange(2500.0f, 4500.0f));
			Zone->SetPowerLevel(FMath::RandRange(2.0f, 4.0f));
		}
	}

	// --- Fleet --------------------------------------------------------------------------------
	for (int32 Index = 0; Index < NumShips; ++Index)
	{
		// Spread the ships out so they are not on top of each other at the start.
		const float Angle = (2.0f * UE_PI * Index) / FMath::Max(1, NumShips);
		const FVector Center(GridConfig.Center.X, GridConfig.Center.Y, 0.0);
		const FVector Start = Center + FVector(FMath::Cos(Angle), FMath::Sin(Angle), 0.0) * (FieldRadius * 0.6f);

		FActorSpawnParameters Params;
		Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		ASailingShipPawn* Ship = World->SpawnActor<ASailingShipPawn>(Start, FRotator::ZeroRotator, Params);
		if (!Ship)
		{
			continue;
		}

		// The first ship is the flagship: strong enough to ignore the zones the others avoid.
		if (Ship->GetPowerComponent())
		{
			Ship->GetPowerComponent()->SetPowerLevel(Index == 0 ? FlagshipPower : 1.0f);
		}

		UNavalNavigatorComponent* Navigator = NewObject<UNavalNavigatorComponent>(Ship);
		Navigator->RegisterComponent();

		if (bAutoWander)
		{
			Navigator->OnArrived.AddDynamic(this, &ANavalNavDemoGameMode::OnShipArrived);
			Navigator->RequestMoveTo(RandomSeaPoint());
		}
	}

	// The controller may have run its BeginPlay before the ships existed; point it at one now.
	if (ANavalNavDemoPlayerController* PC = Cast<ANavalNavDemoPlayerController>(World->GetFirstPlayerController()))
	{
		PC->PossessFirstAvailableShip();
	}

	UE_LOG(LogNavalNav, Log, TEXT("Demo: %d ships, %d danger zones, wind %.0f deg @ %.0f%%."),
		NumShips, NumDangerZones, WindDirectionYaw, WindStrength * 100.0f);
}

void ANavalNavDemoGameMode::OnShipArrived(UNavalNavigatorComponent* Navigator)
{
	if (Navigator)
	{
		Navigator->RequestMoveTo(RandomSeaPoint());
	}
}

FVector ANavalNavDemoGameMode::RandomSeaPoint() const
{
	const FVector Center(GridConfig.Center.X, GridConfig.Center.Y, 0.0);
	return Center + FVector(
		FMath::RandRange(-FieldRadius, FieldRadius),
		FMath::RandRange(-FieldRadius, FieldRadius),
		0.0);
}
