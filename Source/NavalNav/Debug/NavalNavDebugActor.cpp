// Copyright (c) 2026 Kaan Altay. Licensed under the MIT License.

#include "Debug/NavalNavDebugActor.h"

#include "Camera/PlayerCameraManager.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/PlayerController.h"
#include "Grid/SeaGrid.h"
#include "NavalNav.h"
#include "Threat/DangerZone.h"

ANavalNavDebugActor::ANavalNavDebugActor()
{
	PrimaryActorTick.bCanEverTick = true;

	SceneRoot = CreateDefaultSubobject<USceneComponent>(TEXT("SceneRoot"));
	SetRootComponent(SceneRoot);
}

void ANavalNavDebugActor::BeginPlay()
{
	Super::BeginPlay();

	UWorld* World = GetWorld();
	USeaGridSubsystem* SeaGrid = World ? World->GetSubsystem<USeaGridSubsystem>() : nullptr;
	if (!SeaGrid)
	{
		UE_LOG(LogNavalNav, Warning, TEXT("%s found no sea grid subsystem; nothing will be drawn."), *GetName());
		return;
	}

	if (bConfigureGridOnBeginPlay)
	{
		SeaGrid->ConfigureGrid(GridConfig);
	}
	SeaGrid->ObserverPowerLevel = ObserverPowerLevel;

	// Plan once immediately so the first frame already shows a route.
	Replan();
}

void ANavalNavDebugActor::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	TimeSinceLastPlan += DeltaSeconds;
	if (TimeSinceLastPlan >= ReplanInterval)
	{
		Replan();
	}

	// Primitives live exactly one frame: they are redrawn every tick, and a longer lifetime
	// would leave a trail behind a moving zone.
	DrawEverything(DeltaSeconds);
}

void ANavalNavDebugActor::Replan()
{
	UWorld* World = GetWorld();
	USeaGridSubsystem* SeaGrid = World ? World->GetSubsystem<USeaGridSubsystem>() : nullptr;
	if (!SeaGrid)
	{
		return;
	}

	SeaGrid->ObserverPowerLevel = ObserverPowerLevel;

	const double StartTime = FPlatformTime::Seconds();
	LastPath = SeaGrid->FindPath(GetStartLocation(), GetGoalLocation(), PathQuery);
	LastPlanMilliseconds = (FPlatformTime::Seconds() - StartTime) * 1000.0;

	TimeSinceLastPlan = 0.0f;

	if (!LastPath.bSuccess)
	{
		// Worth a warning rather than a silent empty overlay: with a full threat layer this
		// usually means the goal ended up inside a lethal core.
		UE_LOG(LogNavalNav, Warning, TEXT("%s failed to plan a route after expanding %d cells."),
			*GetName(), LastPath.CellsSearched);
	}
}

FVector ANavalNavDebugActor::GetViewLocation() const
{
	if (const UWorld* World = GetWorld())
	{
		if (const APlayerController* PlayerController = World->GetFirstPlayerController())
		{
			if (const APlayerCameraManager* CameraManager = PlayerController->PlayerCameraManager)
			{
				return CameraManager->GetCameraLocation();
			}
		}
	}

	// No camera (editor tick, dedicated server): centre the overlay on the demo route instead.
	return (GetStartLocation() + GetGoalLocation()) * 0.5;
}

void ANavalNavDebugActor::DrawEverything(float Duration) const
{
	// GetWorld() is const-callable but hands back a mutable world, which the actor iterator
	// and the subsystem's lazy threat rebuild both need.
	UWorld* World = GetWorld();
	USeaGridSubsystem* SeaGrid = World ? World->GetSubsystem<USeaGridSubsystem>() : nullptr;
	if (!SeaGrid)
	{
		return;
	}

	// Reading costs is what forces the threat layer to be current, so do it before drawing.
	SeaGrid->EnsureThreatUpToDate();

	if (FSeaGridDebugDraw::IsGridDrawEnabled())
	{
		FSeaGridDebugDraw::DrawGrid(World, SeaGrid->GetGrid(), GetViewLocation(), DrawSettings,
			FSeaGridDebugDraw::IsCostDrawEnabled(), Duration);

		for (TActorIterator<ADangerZone> It(World); It; ++It)
		{
			const ADangerZone* Zone = *It;
			FSeaGridDebugDraw::DrawDangerZone(World, Zone->GetActorLocation(), Zone->GetRadius(),
				Zone->LethalRadiusFraction, FColor(255, 90, 60), Duration);
		}
	}

	if (FSeaGridDebugDraw::IsPathDrawEnabled())
	{
		FSeaGridDebugDraw::DrawPath(World, LastPath, DrawSettings, FColor(60, 220, 255), /*bLabelWaypoints=*/true, Duration);
	}

#if ENABLE_DRAW_DEBUG
	if (bShowStatsOnScreen && GEngine)
	{
		const FString Stats = LastPath.bSuccess
			? FString::Printf(TEXT("NavalNav | cost %.1f | length %.0f uu | %d waypoints | %d cells expanded | %.2f ms"),
				LastPath.GetTotalCost(), LastPath.GetLength(), LastPath.Num(), LastPath.CellsSearched, LastPlanMilliseconds)
			: FString::Printf(TEXT("NavalNav | no route (%d cells expanded)"), LastPath.CellsSearched);

		// A stable key means the line is replaced rather than stacked every frame.
		GEngine->AddOnScreenDebugMessage(/*Key=*/0x4E41564C, Duration, FColor::Cyan, Stats);
	}
#endif // ENABLE_DRAW_DEBUG
}
