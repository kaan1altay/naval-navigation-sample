// Copyright (c) 2026 Kaan Altay. Licensed under the MIT License.

#include "Demo/NavalNavDemoGameMode.h"

#include "Camera/PlayerCameraManager.h"
#include "Components/DirectionalLightComponent.h"
#include "Components/SkyAtmosphereComponent.h"
#include "Components/SkyLightComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Demo/NavalNavDemoPlayerController.h"
#include "Engine/DirectionalLight.h"
#include "Engine/SkyLight.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "Grid/SeaGrid.h"
#include "Grid/SeaGridDebugDraw.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
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

	// The GameMode ticks so it can draw the sea-grid overlay itself, rather than needing a debug
	// actor placed in the level.
	PrimaryActorTick.bCanEverTick = true;
}

void ANavalNavDemoGameMode::BeginPlay()
{
	Super::BeginPlay();

	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	// --- Light, sky and sea, so an empty level is actually visible -----------------------------
	SpawnEnvironment();

	// --- Sea grid + wind ----------------------------------------------------------------------
	if (USeaGridSubsystem* SeaGrid = World->GetSubsystem<USeaGridSubsystem>())
	{
		SeaGrid->ConfigureGrid(GridConfig);
		const FSeaGridData& Grid = SeaGrid->GetGrid();
		UE_LOG(LogNavalNav, Log, TEXT("Demo grid live: %dx%d cells at %.0f uu, built=%s."),
			Grid.GetNumCellsX(), Grid.GetNumCellsY(), Grid.GetCellSize(), Grid.IsBuilt() ? TEXT("yes") : TEXT("NO"));
	}
	if (UWindSubsystem* Wind = World->GetSubsystem<UWindSubsystem>())
	{
		Wind->SetWindDirectionYaw(WindDirectionYaw);
		Wind->SetWindStrength(WindStrength);
	}

	// --- Danger zones (biased toward the centre so wandering ships cross them and routes bend) --
	for (int32 Index = 0; Index < NumDangerZones; ++Index)
	{
		const FVector Center(GridConfig.Center.X, GridConfig.Center.Y, 0.0);
		const FVector At = Center + (RandomSeaPoint() - Center) * 0.6;

		FActorSpawnParameters Params;
		Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		if (ADangerZone* Zone = World->SpawnActor<ADangerZone>(At, FRotator::ZeroRotator, Params))
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
		const FVector Start = Center + FVector(FMath::Cos(Angle), FMath::Sin(Angle), 0.0) * (FieldRadius * 0.85f);

		FActorSpawnParameters Params;
		Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		ASailingShipPawn* Ship = World->SpawnActor<ASailingShipPawn>(Start, FRotator::ZeroRotator, Params);
		if (!Ship)
		{
			continue;
		}

		SetupShipAppearanceAndPower(Ship, Index);

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

void ANavalNavDemoGameMode::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	DrawGridOverlay();
}

void ANavalNavDemoGameMode::SpawnEnvironment()
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

	// Sun that also lights the sky atmosphere.
	if (ADirectionalLight* Sun = World->SpawnActor<ADirectionalLight>(FVector::ZeroVector, FRotator(-46.0f, -35.0f, 0.0f), Params))
	{
		if (UDirectionalLightComponent* Light = Cast<UDirectionalLightComponent>(Sun->GetLightComponent()))
		{
			Light->SetIntensity(6.0f);
			Light->SetAtmosphereSunLight(true);
			Light->MarkRenderStateDirty();
		}
	}

	// Blue sky from the sun.
	World->SpawnActor<ASkyAtmosphere>(FVector::ZeroVector, FRotator::ZeroRotator, Params);

	// Ambient fill so shadowed hull sides are not pitch black.
	if (ASkyLight* Sky = World->SpawnActor<ASkyLight>(FVector::ZeroVector, FRotator::ZeroRotator, Params))
	{
		if (USkyLightComponent* Light = Sky->GetLightComponent())
		{
			Light->SetMobility(EComponentMobility::Movable);
			Light->bRealTimeCapture = true;
			Light->RecaptureSky();
		}
	}

	// A big flat sea plane under everything.
	const float Extent = FMath::Max(static_cast<float>(GridConfig.Extent.X), static_cast<float>(GridConfig.Extent.Y));
	const FVector SeaCentre(GridConfig.Center.X, GridConfig.Center.Y, -5.0);
	if (AStaticMeshActor* Sea = World->SpawnActor<AStaticMeshActor>(SeaCentre, FRotator::ZeroRotator, Params))
	{
		Sea->SetMobility(EComponentMobility::Movable);
		if (UStaticMeshComponent* Mesh = Sea->GetStaticMeshComponent())
		{
			if (UStaticMesh* Plane = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Plane.Plane")))
			{
				Mesh->SetStaticMesh(Plane);
			}
			Mesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
			// The plane primitive is 100 uu across; cover a bit more than the whole grid.
			const float PlaneScale = (Extent * 2.4f) / 100.0f;
			Sea->SetActorScale3D(FVector(PlaneScale, PlaneScale, 1.0f));

			if (UMaterialInterface* Base = LoadObject<UMaterialInterface>(nullptr, TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial")))
			{
				UMaterialInstanceDynamic* SeaMaterial = UMaterialInstanceDynamic::Create(Base, this);
				SeaMaterial->SetVectorParameterValue(TEXT("Color"), SeaColor);
				Mesh->SetMaterial(0, SeaMaterial);
			}
		}
	}
}

float ANavalNavDemoGameMode::SetupShipAppearanceAndPower(ASailingShipPawn* Ship, int32 Index) const
{
	// The first ship is the flagship: strong enough to ignore the zones the others avoid, and gold
	// so it is easy to pick out; the escorts are a cool grey-blue.
	const float Power = (Index == 0) ? FlagshipPower : 1.0f;
	if (Ship->GetPowerComponent())
	{
		Ship->GetPowerComponent()->SetPowerLevel(Power);
	}
	Ship->SetHullColor(Index == 0 ? FLinearColor(1.0f, 0.78f, 0.12f) : FLinearColor(0.55f, 0.68f, 0.85f));
	return Power;
}

void ANavalNavDemoGameMode::DrawGridOverlay() const
{
	if (!FSeaGridDebugDraw::IsGridDrawEnabled())
	{
		return;
	}

	UWorld* World = GetWorld();
	USeaGridSubsystem* SeaGrid = World ? World->GetSubsystem<USeaGridSubsystem>() : nullptr;
	if (!SeaGrid)
	{
		return;
	}

	SeaGrid->EnsureThreatUpToDate();

	FVector ViewLocation(GridConfig.Center.X, GridConfig.Center.Y, 0.0);
	if (const APlayerController* PC = World->GetFirstPlayerController())
	{
		if (PC->PlayerCameraManager)
		{
			ViewLocation = PC->PlayerCameraManager->GetCameraLocation();
		}
	}

	const FSeaGridDebugDrawSettings Settings;
	FSeaGridDebugDraw::DrawGrid(World, SeaGrid->GetGrid(), ViewLocation, Settings,
		FSeaGridDebugDraw::IsCostDrawEnabled(), /*Duration=*/0.0f);
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
