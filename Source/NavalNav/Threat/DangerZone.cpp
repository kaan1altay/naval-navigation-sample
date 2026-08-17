// Copyright (c) 2026 Kaan Altay. Licensed under the MIT License.

#include "Threat/DangerZone.h"

#include "Components/SphereComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "Grid/SeaGrid.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "Threat/ThreatEvaluator.h"
#include "UObject/ConstructorHelpers.h"

namespace
{
	/** Ceiling on how much a very strong zone may multiply its own cost. */
	constexpr float MaxRelativeThreat = 3.0f;

	/** A zone has to drift this fraction of a cell before the threat layer is re-stamped. */
	constexpr float RestampCellFraction = 0.5f;

	/** Height (uu) the disc floats above the sea so it reads without z-fighting the water plane. */
	constexpr float DiscHeight = 25.0f;

	/** The engine cylinder is 100 uu across, so world radius R needs an XY scale of R / 50. */
	constexpr float CylinderUnitRadius = 50.0f;
}

ADangerZone::ADangerZone()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bStartWithTickEnabled = false;

	ZoneSphere = CreateDefaultSubobject<USphereComponent>(TEXT("ZoneSphere"));
	ZoneSphere->InitSphereRadius(Radius);
	ZoneSphere->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	ZoneSphere->SetGenerateOverlapEvents(false);
	// Visible in the editor as a placement aid; at runtime the debug draw owns the visuals.
	ZoneSphere->SetHiddenInGame(true);
	ZoneSphere->ShapeColor = FColor(255, 80, 60);
	SetRootComponent(ZoneSphere);

	// A flat disc on the sea so the zone is visible in a plain build with no debug overlay.
	ZoneDisc = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("ZoneDisc"));
	ZoneDisc->SetupAttachment(ZoneSphere);
	ZoneDisc->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	ZoneDisc->SetGenerateOverlapEvents(false);
	ZoneDisc->SetCastShadow(false);

	static ConstructorHelpers::FObjectFinder<UStaticMesh> CylinderMesh(TEXT("/Engine/BasicShapes/Cylinder.Cylinder"));
	if (CylinderMesh.Succeeded())
	{
		ZoneDisc->SetStaticMesh(CylinderMesh.Object);
	}
	static ConstructorHelpers::FObjectFinder<UMaterialInterface> DiscMat(TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial"));
	if (DiscMat.Succeeded())
	{
		DiscBaseMaterial = DiscMat.Object;
	}
}

void ADangerZone::OnConstruction(const FTransform& Transform)
{
	Super::OnConstruction(Transform);

	if (ZoneSphere)
	{
		ZoneSphere->SetSphereRadius(Radius, /*bUpdateOverlaps=*/false);
	}

	UpdateZoneVisual();
}

void ADangerZone::UpdateZoneVisual()
{
	if (!ZoneDisc)
	{
		return;
	}

	// Make (or reuse) the dynamic material so the colour can track power.
	if (!DiscMaterial && DiscBaseMaterial)
	{
		DiscMaterial = UMaterialInstanceDynamic::Create(DiscBaseMaterial, this);
		ZoneDisc->SetMaterial(0, DiscMaterial);
	}

	const float ScaleXY = FMath::Max(Radius, 1.0f) / CylinderUnitRadius;
	ZoneDisc->SetRelativeScale3D(FVector(ScaleXY, ScaleXY, 0.2));
	ZoneDisc->SetRelativeLocation(FVector(0.0, 0.0, DiscHeight));

	if (DiscMaterial)
	{
		// Amber for a light threat deepening to blood-red as power rises.
		const float Strength = FMath::Clamp(PowerLevel / 6.0f, 0.0f, 1.0f);
		const FLinearColor Color = FMath::Lerp(FLinearColor(1.0f, 0.55f, 0.10f), FLinearColor(1.0f, 0.05f, 0.02f), Strength);
		DiscMaterial->SetVectorParameterValue(TEXT("Color"), Color);
	}
}

void ADangerZone::BeginPlay()
{
	Super::BeginPlay();

	HomeLocation = GetActorLocation();
	LastStampedLocation = HomeLocation;
	MoveAxis = MoveAxis.GetSafeNormal();
	if (MoveAxis.IsNearlyZero())
	{
		MoveAxis = FVector(0.0, 1.0, 0.0);
	}

	// Only moving zones need a tick; a static battery costs nothing per frame.
	SetActorTickEnabled(bMoves);

	UpdateZoneVisual();

	if (UWorld* World = GetWorld())
	{
		if (USeaGridSubsystem* SeaGrid = World->GetSubsystem<USeaGridSubsystem>())
		{
			SeaGrid->RegisterDangerZone(this);
		}
	}
}

void ADangerZone::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (UWorld* World = GetWorld())
	{
		if (USeaGridSubsystem* SeaGrid = World->GetSubsystem<USeaGridSubsystem>())
		{
			SeaGrid->UnregisterDangerZone(this);
		}
	}

	Super::EndPlay(EndPlayReason);
}

void ADangerZone::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	if (bMoves)
	{
		const float Period = FMath::Max(MovePeriod, UE_KINDA_SMALL_NUMBER);
		const float Phase = FMath::Sin(2.0f * UE_PI * GetWorld()->GetTimeSeconds() / Period);
		SetActorLocation(HomeLocation + MoveAxis * (MoveAmplitude * Phase));
	}

	NotifyGridIfMoved();
}

void ADangerZone::NotifyGridIfMoved()
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	USeaGridSubsystem* SeaGrid = World->GetSubsystem<USeaGridSubsystem>();
	if (!SeaGrid)
	{
		return;
	}

	// Re-stamping the layer is cheap but not free, and a zone that moved a tenth of a cell
	// produces the same footprint. Only pay for it once the footprint can actually differ.
	const float CellSize = FMath::Max(SeaGrid->GetGrid().GetCellSize(), 1.0f);
	const double Threshold = CellSize * RestampCellFraction;
	if (FVector::DistSquared2D(GetActorLocation(), LastStampedLocation) >= Threshold * Threshold)
	{
		LastStampedLocation = GetActorLocation();
		SeaGrid->MarkThreatDirty();
	}
}

void ADangerZone::StampThreat(FSeaGridData& Grid, float ObserverPowerLevel, float HostilityThreshold) const
{
	if (Radius <= 0.0f || !FThreatEvaluator::IsHostile(ObserverPowerLevel, PowerLevel, HostilityThreshold))
	{
		// Weak enough to ignore: this zone contributes nothing and the ship sails straight on.
		return;
	}

	// A zone that only just qualifies as hostile still gets the full base cost; the power gap
	// then scales it up from there.
	const float Relative = FMath::Min(FThreatEvaluator::RelativeThreat(ObserverPowerLevel, PowerLevel), MaxRelativeThreat);
	const float ThreatScale = MaxThreatCost * (1.0f + Relative);

	const FVector Center = GetActorLocation();
	Grid.ForEachCellInRadius(Center, Radius, [&Grid, this, ThreatScale](const FIntPoint& Cell, float Distance)
	{
		const float Cost = FThreatEvaluator::CellThreatCost(Distance, Radius, ThreatScale, FalloffExponent, LethalRadiusFraction);
		if (Cost > 0.0f)
		{
			Grid.AddThreatCost(Cell, Cost);
		}
	});
}

void ADangerZone::SetRadius(float NewRadius)
{
	Radius = FMath::Max(0.0f, NewRadius);
	if (ZoneSphere)
	{
		ZoneSphere->SetSphereRadius(Radius, /*bUpdateOverlaps=*/false);
	}
	UpdateZoneVisual();

	if (UWorld* World = GetWorld())
	{
		if (USeaGridSubsystem* SeaGrid = World->GetSubsystem<USeaGridSubsystem>())
		{
			SeaGrid->MarkThreatDirty();
		}
	}
}

void ADangerZone::SetPowerLevel(float NewPowerLevel)
{
	PowerLevel = FMath::Max(0.0f, NewPowerLevel);
	UpdateZoneVisual();

	if (UWorld* World = GetWorld())
	{
		if (USeaGridSubsystem* SeaGrid = World->GetSubsystem<USeaGridSubsystem>())
		{
			SeaGrid->MarkThreatDirty();
		}
	}
}

#if WITH_EDITOR
void ADangerZone::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);

	if (ZoneSphere)
	{
		ZoneSphere->SetSphereRadius(Radius, /*bUpdateOverlaps=*/false);
	}

	// Any tuning change invalidates the stamped footprint; the layer is rebuilt lazily.
	if (UWorld* World = GetWorld())
	{
		if (USeaGridSubsystem* SeaGrid = World->GetSubsystem<USeaGridSubsystem>())
		{
			SeaGrid->MarkThreatDirty();
		}
	}
}
#endif
