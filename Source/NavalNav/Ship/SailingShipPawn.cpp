// Copyright (c) 2026 Kaan Altay. Licensed under the MIT License.

#include "Ship/SailingShipPawn.h"

#include "Camera/CameraComponent.h"
#include "Components/StaticMeshComponent.h"
#include "DrawDebugHelpers.h"
#include "Engine/World.h"
#include "GameFramework/SpringArmComponent.h"
#include "HAL/IConsoleManager.h"
#include "Ship/SailingShipConfig.h"
#include "Ship/ShipPowerComponent.h"
#include "Ship/WindSubsystem.h"

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

	// A visual only, and optional: no mesh is set, so the ship is invisible until a level assigns
	// one. It never collides — this pawn is moved kinematically, not swept.
	HullMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("HullMesh"));
	HullMesh->SetupAttachment(ShipRoot);
	HullMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	HullMesh->SetGenerateOverlapEvents(false);

	// Chase cam over the transom. The boom hangs off the root, so it inherits the ship's yaw and
	// stays behind the stern as she turns, without any per-frame camera code.
	CameraBoom = CreateDefaultSubobject<USpringArmComponent>(TEXT("CameraBoom"));
	CameraBoom->SetupAttachment(ShipRoot);
	CameraBoom->TargetArmLength = 4000.0f;
	CameraBoom->SetRelativeRotation(FRotator(-25.0f, 0.0f, 0.0f));
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

void ASailingShipPawn::BeginPlay()
{
	Super::BeginPlay();

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

	if (CVarShipDebug.GetValueOnGameThread() != 0)
	{
		DrawShipDebug();
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
	float WindStrength = FallbackWindStrength;
	FVector WindToward(FMath::Cos(FMath::DegreesToRadians(FallbackWindFromYaw + 180.0f)),
		FMath::Sin(FMath::DegreesToRadians(FallbackWindFromYaw + 180.0f)), 0.0);
	float WindFromYaw = FallbackWindFromYaw;
	if (UWindSubsystem* Wind = World->GetSubsystem<UWindSubsystem>())
	{
		WindStrength = Wind->GetWindStrength();
		WindToward = Wind->GetWindDirection();
		WindFromYaw = Wind->GetWindFromYaw();
	}
	const FVector WindArrowStart = Origin - WindToward * 2000.0f + FVector(0.0, 0.0, 300.0);
	const FVector WindArrowEnd = Origin + WindToward * 2000.0f + FVector(0.0, 0.0, 300.0);
	DrawDebugDirectionalArrow(World, WindArrowStart, WindArrowEnd, 300.0f, FColor(80, 160, 255),
		/*bPersistentLines=*/false, -1.0f, /*DepthPriority=*/0, /*Thickness=*/8.0f * (0.5f + WindStrength));

	const float AngleOffWind = FSailingModel::AngleOffWind(State.HeadingDegrees, WindFromYaw);
	const FString Readout = FString::Printf(
		TEXT("%s\nheading %.0f deg | speed %.0f uu/s | rudder %.0f deg | trim %.2f\nwind %.0f%% | off-wind %.0f deg%s"),
		*GetName(), State.HeadingDegrees, State.Speed, State.RudderAngleDegrees, State.SailTrim,
		WindStrength * 100.0f, AngleOffWind,
		AngleOffWind <= GetEffectiveParams().NoGoAngleDegrees ? TEXT("  (NO-GO)") : TEXT(""));
	DrawDebugString(World, Origin + FVector(0.0, 0.0, 400.0), Readout, /*TestBaseActor=*/nullptr,
		FColor::White, /*Duration=*/0.0f, /*bDrawShadow=*/true);
#endif // ENABLE_DRAW_DEBUG
}
