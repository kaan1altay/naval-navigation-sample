// Copyright (c) 2026 Kaan Altay. Licensed under the MIT License.

#include "Ship/SailingShipPawn.h"

#include "Camera/CameraComponent.h"
#include "Components/StaticMeshComponent.h"
#include "DrawDebugHelpers.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GameFramework/SpringArmComponent.h"
#include "HAL/IConsoleManager.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Ship/SailingShipConfig.h"
#include "Ship/ShipPowerComponent.h"
#include "Ship/WindSubsystem.h"
#include "UObject/ConstructorHelpers.h"

namespace
{
	TAutoConsoleVariable<int32> CVarShipDebug(
		TEXT("naval.Ship.Debug"),
		0,
		TEXT("Draw a sailing ship's state: heading arrow, wind arrow, rudder and a text readout.\n")
		TEXT("0 = off, 1 = on."),
		ECVF_Cheat);
}

ASailingShipPawn::ASailingShipPawn()
{
	PrimaryActorTick.bCanEverTick = true;

	ShipRoot = CreateDefaultSubobject<USceneComponent>(TEXT("ShipRoot"));
	SetRootComponent(ShipRoot);

	// A visual only. It never collides — this pawn is moved kinematically, not swept.
	HullMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("HullMesh"));
	HullMesh->SetupAttachment(ShipRoot);
	HullMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	HullMesh->SetGenerateOverlapEvents(false);

	// Give the ship a visible default hull from an engine primitive, so it shows up in an empty
	// map with no external assets. A cone's apex points +Z; pitching it -90 aims it along +X, the
	// ship's forward, and the scale stretches it into a rough hull. Any level can swap the mesh.
	static ConstructorHelpers::FObjectFinder<UStaticMesh> ConeMesh(TEXT("/Engine/BasicShapes/Cone.Cone"));
	if (ConeMesh.Succeeded())
	{
		HullMesh->SetStaticMesh(ConeMesh.Object);
		HullMesh->SetRelativeRotation(FRotator(-90.0f, 0.0f, 0.0f));
		// The cone is 100 uu; this makes the hull ~400 uu long (about two 200 uu grid cells) so it
		// reads clearly from the demo camera at default zoom.
		HullMesh->SetRelativeScale3D(FVector(1.3, 1.3, 4.0));
	}

	// Explicitly base the hull on a lit material that exposes a "Color" vector parameter. The cone's
	// own default material does not, which is why the ships were rendering grey.
	static ConstructorHelpers::FObjectFinder<UMaterialInterface> HullMat(TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial"));
	if (HullMat.Succeeded())
	{
		HullBaseMaterial = HullMat.Object;
	}

	// Chase cam over the transom. The boom hangs off the root, so it inherits the ship's yaw and
	// stays behind the stern as she turns, without any per-frame camera code.
	CameraBoom = CreateDefaultSubobject<USpringArmComponent>(TEXT("CameraBoom"));
	CameraBoom->SetupAttachment(ShipRoot);
	// A high, tilted-down chase so several ships and the zones around them read in one frame.
	CameraBoom->TargetArmLength = 6500.0f;
	CameraBoom->SetRelativeRotation(FRotator(-55.0f, 0.0f, 0.0f));
	CameraBoom->bInheritPitch = false;
	CameraBoom->bInheritRoll = false;
	CameraBoom->bInheritYaw = true;
	CameraBoom->bEnableCameraLag = true;
	CameraBoom->CameraLagSpeed = 3.0f;
	CameraBoom->bDoCollisionTest = false;

	ChaseCamera = CreateDefaultSubobject<UCameraComponent>(TEXT("ChaseCamera"));
	ChaseCamera->SetupAttachment(CameraBoom, USpringArmComponent::SocketName);

	PowerComponent = CreateDefaultSubobject<UShipPowerComponent>(TEXT("PowerComponent"));

	// A sailing ship follows its own heading, not the controller's view rotation.
	bUseControllerRotationYaw = false;
	bUseControllerRotationPitch = false;
	bUseControllerRotationRoll = false;
}

void ASailingShipPawn::OnConstruction(const FTransform& Transform)
{
	Super::OnConstruction(Transform);
	RefreshHullMaterial();
}

void ASailingShipPawn::BeginPlay()
{
	Super::BeginPlay();

	RefreshHullMaterial();

	// Start sailing on whatever heading the ship was placed at, so a level author points the bow
	// and the model takes it from there.
	State.HeadingDegrees = FSailingModel::NormalizeDegrees(static_cast<float>(GetActorRotation().Yaw));
	State.SailTrim = SailTrimInput;

	FVector Location = GetActorLocation();
	Location.Z = SeaLevelZ;
	SetActorLocation(Location);
}

void ASailingShipPawn::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	StepSailing(DeltaSeconds);

	UpdateAndDrawWake();

	if (bSelected)
	{
		DrawSelectionRing();
	}

	// The ship overlay (heading / rudder / wind arrows) is for the selected ship only, to keep the
	// screen readable with a whole fleet under way.
	if (bSelected && CVarShipDebug.GetValueOnGameThread() != 0)
	{
		DrawShipDebug();
	}
}

void ASailingShipPawn::UpdateAndDrawWake()
{
#if ENABLE_DRAW_DEBUG
	const UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	// Add a point only once the ship has made some way, so a stationary ship does not pile them up.
	const FVector Position = GetActorLocation();
	if (WakePoints.Num() == 0 || FVector::Dist2D(Position, WakePoints.Last()) > 250.0f)
	{
		WakePoints.Add(Position);
		const int32 MaxPoints = 16;
		if (WakePoints.Num() > MaxPoints)
		{
			WakePoints.RemoveAt(0, WakePoints.Num() - MaxPoints);
		}
	}

	// Fade from a dim tail to a bright, thicker head — cheap, reads clearly as speed and direction.
	const int32 Num = WakePoints.Num();
	for (int32 Index = 1; Index < Num; ++Index)
	{
		const float Frac = static_cast<float>(Index) / static_cast<float>(Num - 1);
		const uint8 Value = static_cast<uint8>(80.0f + 175.0f * Frac);
		const FColor Colour(Value, Value, static_cast<uint8>(FMath::Min(255, Value + 25)));
		const float Thickness = 6.0f + 12.0f * Frac;
		DrawDebugLine(World, WakePoints[Index - 1] + FVector(0, 0, 15), WakePoints[Index] + FVector(0, 0, 15),
			Colour, /*bPersistentLines=*/false, -1.0f, /*DepthPriority=*/0, Thickness);
	}
#endif // ENABLE_DRAW_DEBUG
}

void ASailingShipPawn::DrawSelectionRing() const
{
#if ENABLE_DRAW_DEBUG
	if (const UWorld* World = GetWorld())
	{
		DrawDebugCircle(World, GetActorLocation() + FVector(0.0, 0.0, 20.0), 520.0f, /*Segments=*/40,
			FColor(80, 240, 255), /*bPersistentLines=*/false, /*LifeTime=*/-1.0f, /*DepthPriority=*/0,
			/*Thickness=*/16.0f, FVector(1, 0, 0), FVector(0, 1, 0), /*bDrawAxis=*/false);
	}
#endif // ENABLE_DRAW_DEBUG
}

void ASailingShipPawn::RefreshHullMaterial()
{
	if (!HullMesh)
	{
		return;
	}

	// Make (once) a dynamic instance of a material we know has a "Color" param, and assign it, so
	// the tint takes regardless of what mesh the level assigned.
	if (!HullMaterial && HullBaseMaterial)
	{
		HullMaterial = UMaterialInstanceDynamic::Create(HullBaseMaterial, this);
		HullMesh->SetMaterial(0, HullMaterial);
	}

	if (HullMaterial)
	{
		HullMaterial->SetVectorParameterValue(TEXT("Color"), HullColor);
		// Matte so the strong directional sun does not blow the colour out to white; the selection
		// is shown by a ring on the water (DrawSelectionRing), not by changing the hull colour.
		HullMaterial->SetScalarParameterValue(TEXT("Roughness"), 1.0f);
	}
}

void ASailingShipPawn::SetHullColor(FLinearColor Color)
{
	HullColor = Color;
	RefreshHullMaterial();
}

void ASailingShipPawn::SetSelected(bool bInSelected)
{
	bSelected = bInSelected;
	RefreshHullMaterial();
}

void ASailingShipPawn::AddCameraZoom(float Delta)
{
	if (CameraBoom)
	{
		// Clamp zoom so the camera never pulls back far enough to see the sea's edge.
		CameraBoom->TargetArmLength = FMath::Clamp(CameraBoom->TargetArmLength + Delta, 3000.0f, 14000.0f);
	}
}

const FSailingModelParams& ASailingShipPawn::GetEffectiveParams() const
{
	return SailingConfig ? SailingConfig->Params : SailingParams;
}

FSailingModel ASailingShipPawn::MakeModel() const
{
	FSailingModel Model;
	Model.Params = GetEffectiveParams();
	return Model;
}

void ASailingShipPawn::StepSailing(float DeltaSeconds)
{
	// Wind: the world's if there is one, otherwise the fallback so a bare test level still sails.
	float WindFromYaw = FallbackWindFromYaw;
	float WindStrength = FallbackWindStrength;
	FVector WindToward = FVector::ZeroVector;

	const UWorld* World = GetWorld();
	UWindSubsystem* Wind = World ? World->GetSubsystem<UWindSubsystem>() : nullptr;
	if (Wind)
	{
		WindFromYaw = Wind->GetWindFromYaw();
		WindStrength = Wind->GetWindStrength();
		WindToward = Wind->GetWindDirection();
	}

	const FSailingModel Model = MakeModel();
	Model.Advance(State, RudderInput, SailTrimInput, WindFromYaw, WindStrength, DeltaSeconds);

	// Integrate position from heading and speed. Forward at yaw 0 is +X, matching FRotator's yaw.
	const float HeadingRad = FMath::DegreesToRadians(State.HeadingDegrees);
	const FVector Forward(FMath::Cos(HeadingRad), FMath::Sin(HeadingRad), 0.0);
	FVector Velocity = Forward * State.Speed;

	// Optional leeway: a little of the ship's way leaks sideways downwind. Cosmetic — the planner
	// reasons about the heading, not this drift — but it reads as a boat that is not on rails.
	const FSailingModelParams& Params = GetEffectiveParams();
	if (Params.bEnableLeeway && !WindToward.IsNearlyZero())
	{
		Velocity += WindToward * (Params.LeewayFactor * WindStrength * State.Speed);
	}

	const float Dt = FMath::Clamp(DeltaSeconds, 0.0f, 0.25f);
	FVector NewLocation = GetActorLocation() + Velocity * Dt;
	NewLocation.Z = SeaLevelZ;

	SetActorLocationAndRotation(NewLocation, FRotator(0.0f, State.HeadingDegrees, 0.0f));
}

void ASailingShipPawn::DrawShipDebug() const
{
#if ENABLE_DRAW_DEBUG
	const UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	const FVector Origin = GetActorLocation() + FVector(0.0, 0.0, 50.0);
	const float HeadingRad = FMath::DegreesToRadians(State.HeadingDegrees);
	const FVector Forward(FMath::Cos(HeadingRad), FMath::Sin(HeadingRad), 0.0);

	// Heading: a green arrow along the bow, length scaled by speed so a stopped ship reads as stopped.
	const float HeadingLength = 1500.0f + State.Speed;
	DrawDebugDirectionalArrow(World, Origin, Origin + Forward * HeadingLength, 250.0f,
		FColor(60, 220, 90), /*bPersistentLines=*/false, -1.0f, /*DepthPriority=*/0, /*Thickness=*/12.0f);

	// Rudder: a short red line off the stern, kicked to the current rudder angle.
	const float RudderRad = FMath::DegreesToRadians(State.HeadingDegrees + 180.0f + State.RudderAngleDegrees);
	const FVector Stern = Origin - Forward * 600.0f;
	const FVector RudderDir(FMath::Cos(RudderRad), FMath::Sin(RudderRad), 0.0);
	DrawDebugLine(World, Stern, Stern + RudderDir * 500.0f, FColor(230, 60, 60),
		/*bPersistentLines=*/false, -1.0f, /*DepthPriority=*/0, /*Thickness=*/10.0f);

	// Wind: a blue arrow through the ship, pointing the way the wind blows.
	FVector WindToward(FMath::Cos(FMath::DegreesToRadians(FallbackWindFromYaw + 180.0f)),
		FMath::Sin(FMath::DegreesToRadians(FallbackWindFromYaw + 180.0f)), 0.0);
	if (UWindSubsystem* Wind = World->GetSubsystem<UWindSubsystem>())
	{
		WindToward = Wind->GetWindDirection();
	}
	// A blue arrow through the ship with a big, unambiguous head at the *downwind* end, so the sense
	// (which way it blows) reads at a glance, plus a "W" at the head.
	const FVector WindLift(0.0, 0.0, 300.0);
	const FVector WindTail = Origin - WindToward * 1600.0f + WindLift;
	const FVector WindHead = Origin + WindToward * 1600.0f + WindLift;
	DrawDebugDirectionalArrow(World, WindTail, WindHead, /*ArrowSize=*/700.0f, FColor(80, 160, 255),
		/*bPersistentLines=*/false, -1.0f, /*DepthPriority=*/0, /*Thickness=*/10.0f);
	DrawDebugString(World, WindHead + FVector(0.0, 0.0, 120.0), TEXT("W"), /*TestBaseActor=*/nullptr,
		FColor(120, 190, 255), /*Duration=*/0.0f, /*bDrawShadow=*/true, /*FontScale=*/1.5f);
	// The text readout lives on the HUD (GetStatusText), pinned to the screen edge.
#endif // ENABLE_DRAW_DEBUG
}

FString ASailingShipPawn::GetStatusText() const
{
	float WindStrength = FallbackWindStrength;
	float WindFromYaw = FallbackWindFromYaw;
	if (const UWorld* World = GetWorld())
	{
		if (UWindSubsystem* Wind = World->GetSubsystem<UWindSubsystem>())
		{
			WindStrength = Wind->GetWindStrength();
			WindFromYaw = Wind->GetWindFromYaw();
		}
	}

	const float AngleOffWind = FSailingModel::AngleOffWind(State.HeadingDegrees, WindFromYaw);
	return FString::Printf(
		TEXT("SHIP  %s\nheading %.0f deg  |  speed %.0f uu/s\nrudder %.0f deg  |  trim %.2f\nwind %.0f%%  |  off-wind %.0f deg%s"),
		*GetName(), State.HeadingDegrees, State.Speed, State.RudderAngleDegrees, State.SailTrim,
		WindStrength * 100.0f, AngleOffWind,
		AngleOffWind <= GetEffectiveParams().NoGoAngleDegrees ? TEXT("  (NO-GO)") : TEXT(""));
}
