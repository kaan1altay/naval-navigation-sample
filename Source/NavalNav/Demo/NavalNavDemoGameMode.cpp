// Copyright (c) 2026 Kaan Altay. Licensed under the MIT License.

#include "Demo/NavalNavDemoGameMode.h"

#include "Camera/PlayerCameraManager.h"
#include "Components/DirectionalLightComponent.h"
#include "Components/SkyAtmosphereComponent.h"
#include "Components/SkyLightComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Demo/NavalNavDemoHUD.h"
#include "Demo/NavalNavDemoPlayerController.h"
#include "DrawDebugHelpers.h"
#include "Engine/DirectionalLight.h"
#include "Engine/Engine.h"
#include "Engine/SkyLight.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
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
	HUDClass = ANavalNavDemoHUD::StaticClass();

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

	// Everything else — ships and zones — belongs to a scenario. Start with the baseline.
	StartScenario(CurrentScenario);

	UE_LOG(LogNavalNav, Log, TEXT("Demo up: scenario %d, wind %.0f deg @ %.0f%%."),
		CurrentScenario, WindDirectionYaw, WindStrength * 100.0f);
}

void ANavalNavDemoGameMode::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	// The scenario title, wind and key map now live on the HUD (ANavalNavDemoHUD); here we only draw
	// the world-space overlays.
	DrawGridOverlay();
	DrawZoneAnnotations();
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
			// The plane primitive is 100 uu across. Cover several times the grid so the sea's edge is
			// never in shot even at maximum zoom-out; the sky fills the horizon beyond it anyway.
			const float PlaneScale = FMath::Max(Extent, FieldRadius) * 10.0f / 100.0f;
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

ASailingShipPawn* ANavalNavDemoGameMode::SpawnShip(const FVector& Loc, float Power, const FLinearColor& Color, float HeadingYaw)
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return nullptr;
	}

	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	ASailingShipPawn* Ship = World->SpawnActor<ASailingShipPawn>(Loc, FRotator(0.0f, HeadingYaw, 0.0f), Params);
	if (!Ship)
	{
		return nullptr;
	}

	if (Ship->GetPowerComponent())
	{
		Ship->GetPowerComponent()->SetPowerLevel(Power);
	}
	Ship->SetHullColor(Color);
	return Ship;
}

UNavalNavigatorComponent* ANavalNavDemoGameMode::AddNavigator(ASailingShipPawn* Ship, bool bWander, const FVector& InitialGoal)
{
	UNavalNavigatorComponent* Navigator = NewObject<UNavalNavigatorComponent>(Ship);
	Navigator->RegisterComponent();

	if (bWander)
	{
		Navigator->OnArrived.AddDynamic(this, &ANavalNavDemoGameMode::OnShipArrived);
	}
	Navigator->RequestMoveTo(InitialGoal);
	return Navigator;
}

ADangerZone* ANavalNavDemoGameMode::SpawnZone(const FVector& Loc, float Radius, float Power, EZoneMovement Movement)
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return nullptr;
	}

	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	ADangerZone* Zone = World->SpawnActor<ADangerZone>(Loc, FRotator::ZeroRotator, Params);
	if (!Zone)
	{
		return nullptr;
	}

	Zone->MovementPattern = Movement;
	Zone->SetActorTickEnabled(Movement != EZoneMovement::Static);
	Zone->SetRadius(Radius);
	Zone->SetPowerLevel(Power);
	return Zone;
}

void ANavalNavDemoGameMode::ClearScenarioActors()
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	for (TActorIterator<ASailingShipPawn> It(World); It; ++It)
	{
		It->Destroy();
	}
	for (TActorIterator<ADangerZone> It(World); It; ++It)
	{
		It->Destroy();
	}
}

void ANavalNavDemoGameMode::PossessFirstShip()
{
	if (ANavalNavDemoPlayerController* PC = Cast<ANavalNavDemoPlayerController>(GetWorld()->GetFirstPlayerController()))
	{
		PC->PossessFirstAvailableShip();
	}
}

void ANavalNavDemoGameMode::StartScenario(int32 Index)
{
	CurrentScenario = FMath::Clamp(Index, 5, 9);
	ClearScenarioActors();
	DemoRandom.Initialize(1000 + CurrentScenario); // deterministic reset per scenario

	const FVector C(GridConfig.Center.X, GridConfig.Center.Y, 0.0);
	const FLinearColor Gold(1.0f, 0.78f, 0.12f);
	const FLinearColor Blue(0.55f, 0.68f, 0.85f);
	const FLinearColor Steel(0.72f, 0.75f, 0.80f);

	switch (CurrentScenario)
	{
	case 5:
	default:
	{
		ScenarioTitle = TEXT("[5] Baseline - static zones, ships wander between goals");
		for (int32 i = 0; i < NumDangerZones; ++i)
		{
			SpawnZone(C + (RandomSeaPoint() - C) * 0.6, DemoRandom.FRandRange(2500.0f, 4500.0f), DemoRandom.FRandRange(2.0f, 4.0f));
		}
		for (int32 i = 0; i < NumShips; ++i)
		{
			const float Angle = (2.0f * UE_PI * i) / FMath::Max(1, NumShips);
			const FVector Start = C + FVector(FMath::Cos(Angle), FMath::Sin(Angle), 0.0) * (FieldRadius * 0.85f);
			ASailingShipPawn* Ship = SpawnShip(Start, (i == 0) ? FlagshipPower : 1.0f, (i == 0) ? Gold : Blue);
			if (Ship)
			{
				AddNavigator(Ship, /*bWander=*/true, RandomSeaPoint());
			}
		}
		break;
	}
	case 6:
	{
		ScenarioTitle = TEXT("[6] Moving zone - a patrol slides across the route, forcing a mid-voyage replan");
		// A ship crossing west-to-east; a zone patrolling north-south straddles the middle of the line.
		const FVector Start = C + FVector(-FieldRadius, 0.0, 0.0);
		const FVector Goal = C + FVector(FieldRadius, 0.0, 0.0);
		ADangerZone* Zone = SpawnZone(C, 3200.0f, 3.0f, EZoneMovement::Patrol);
		if (Zone)
		{
			Zone->MoveAxis = FVector(0.0, 1.0, 0.0);
			Zone->MoveAmplitude = FieldRadius * 0.5f;
			Zone->MovePeriod = 16.0f;
		}
		if (ASailingShipPawn* Ship = SpawnShip(Start, 1.0f, Blue))
		{
			AddNavigator(Ship, /*bWander=*/false, Goal);
		}
		break;
	}
	case 7:
	{
		ScenarioTitle = TEXT("[7] Power contrast - weak (blue) routes around the zone, strong (gold) sails through");
		const FVector Start = C + FVector(-FieldRadius, -1500.0, 0.0);
		const FVector Goal = C + FVector(FieldRadius, 1500.0, 0.0);
		SpawnZone(C, 4000.0f, 3.0f); // squarely on the direct line
		if (ASailingShipPawn* Weak = SpawnShip(Start, 1.0f, Blue))
		{
			AddNavigator(Weak, /*bWander=*/false, Goal);
		}
		if (ASailingShipPawn* Strong = SpawnShip(Start + FVector(0.0, 400.0, 0.0), FlagshipPower, Gold))
		{
			AddNavigator(Strong, /*bWander=*/false, Goal + FVector(0.0, 400.0, 0.0));
		}
		break;
	}
	case 8:
	{
		ScenarioTitle = TEXT("[8] Enclosure - ringed by zones with one weak gap; the ship escapes through it");
		// A ring of strong zones with one deliberately weaker, leaving a low-cost exit.
		const int32 RingCount = 6;
		const float RingRadius = 6500.0f;
		for (int32 i = 0; i < RingCount; ++i)
		{
			const float Angle = (2.0f * UE_PI * i) / RingCount;
			const FVector At = C + FVector(FMath::Cos(Angle), FMath::Sin(Angle), 0.0) * RingRadius;
			const bool bGap = (i == 0);
			SpawnZone(At, 3200.0f, bGap ? 1.5f : 5.0f);
		}
		if (ASailingShipPawn* Ship = SpawnShip(C, 1.0f, Steel))
		{
			// Goal well outside the ring; the ship must break out first.
			AddNavigator(Ship, /*bWander=*/false, C + FVector(FieldRadius, 0.0, 0.0));
		}
		break;
	}
	case 9:
	{
		ScenarioTitle = TEXT("[9] Power drop - strong ship crossing a zone; press P to weaken it and watch it re-solve");
		const FVector Start = C + FVector(-FieldRadius, 0.0, 0.0);
		const FVector Goal = C + FVector(FieldRadius, 0.0, 0.0);
		SpawnZone(C, 4000.0f, 4.0f); // on the direct line
		if (ASailingShipPawn* Ship = SpawnShip(Start, FlagshipPower, Gold))
		{
			AddNavigator(Ship, /*bWander=*/false, Goal);
		}
		break;
	}
	}

	PossessFirstShip();
	UE_LOG(LogNavalNav, Log, TEXT("Scenario %d: %s"), CurrentScenario, *ScenarioTitle);
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

	// Stamp the layer for a fixed "overlay observer" every frame, so the drawn threat does not flip
	// between ships of different power as they replan — that was half the flicker.
	SeaGrid->ObserverPowerLevel = OverlayObserverPower;
	SeaGrid->HostilityThreshold = 0.0f;
	SeaGrid->MarkThreatDirty();
	SeaGrid->EnsureThreatUpToDate();

	FVector ViewLocation(GridConfig.Center.X, GridConfig.Center.Y, 0.0);
	if (const APlayerController* PC = World->GetFirstPlayerController())
	{
		if (PC->PlayerCameraManager)
		{
			ViewLocation = PC->PlayerCameraManager->GetCameraLocation();
		}
	}

	// Draw with a lifetime a little longer than a frame, redrawn every frame: any single-frame gap
	// in the debug-line batch is covered by the previous frame's cells, so the overlay is steady.
	const FSeaGridDebugDrawSettings Settings;
	FSeaGridDebugDraw::DrawGrid(World, SeaGrid->GetGrid(), ViewLocation, Settings,
		FSeaGridDebugDraw::IsCostDrawEnabled(), /*Duration=*/0.15f);
}

void ANavalNavDemoGameMode::DrawZoneAnnotations() const
{
#if ENABLE_DRAW_DEBUG
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	// A crisp rim and a power number on every zone, always on, so a recording explains itself even
	// with the debug overlays off. Redrawn each frame (lifetime -1 = one frame) so it stays steady.
	for (TActorIterator<ADangerZone> It(World); It; ++It)
	{
		const ADangerZone* Zone = *It;
		const FVector Centre = Zone->GetActorLocation();
		const FColor Band = ADangerZone::PowerBandColor(Zone->GetPowerLevel()).ToFColor(/*bSRGB=*/true);

		DrawDebugCircle(World, Centre + FVector(0, 0, 30), Zone->GetRadius(), /*Segments=*/48, Band,
			/*bPersistentLines=*/false, /*LifeTime=*/-1.0f, /*DepthPriority=*/0, /*Thickness=*/20.0f,
			FVector(1, 0, 0), FVector(0, 1, 0), /*bDrawAxis=*/false);

		DrawDebugString(World, Centre + FVector(0, 0, 250), FString::Printf(TEXT("P %.0f"), Zone->GetPowerLevel()),
			/*TestBaseActor=*/nullptr, Band, /*Duration=*/0.0f, /*bDrawShadow=*/true, /*FontScale=*/1.4f);
	}
#endif // ENABLE_DRAW_DEBUG
}

void ANavalNavDemoGameMode::OnShipArrived(UNavalNavigatorComponent* Navigator)
{
	// A ship the player has taken command of keeps its orders — wander must not re-task it.
	if (Navigator && !Navigator->IsPlayerControlled())
	{
		Navigator->RequestMoveTo(RandomSeaPoint());
	}
}

FVector ANavalNavDemoGameMode::RandomSeaPoint()
{
	const FVector Center(GridConfig.Center.X, GridConfig.Center.Y, 0.0);
	return Center + FVector(
		DemoRandom.FRandRange(-FieldRadius, FieldRadius),
		DemoRandom.FRandRange(-FieldRadius, FieldRadius),
		0.0);
}
