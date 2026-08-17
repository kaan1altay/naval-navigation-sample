// Copyright (c) 2026 Kaan Altay. Licensed under the MIT License.

#include "Navigation/NavalNavigatorComponent.h"

#include "DrawDebugHelpers.h"
#include "Engine/World.h"
#include "Grid/SeaGrid.h"
#include "HAL/IConsoleManager.h"
#include "NavalNav.h"
#include "Ship/SailingShipPawn.h"
#include "Ship/ShipPowerComponent.h"
#include "Ship/WindSubsystem.h"

namespace
{
	TAutoConsoleVariable<int32> CVarNavDebug(
		TEXT("naval.Nav.Debug"),
		0,
		TEXT("Draw the navigator: route, active waypoint, look-ahead and turn-in points, state.\n")
		TEXT("0 = off, 1 = on."),
		ECVF_Cheat);

	const TCHAR* StateName(ENavigatorState State)
	{
		switch (State)
		{
		case ENavigatorState::Idle:      return TEXT("Idle");
		case ENavigatorState::Planning:  return TEXT("Planning");
		case ENavigatorState::Following: return TEXT("Following");
		case ENavigatorState::Arrived:   return TEXT("Arrived");
		default:                         return TEXT("?");
		}
	}
}

UNavalNavigatorComponent::UNavalNavigatorComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.bStartWithTickEnabled = true;
}

void UNavalNavigatorComponent::BeginPlay()
{
	Super::BeginPlay();

	Ship = Cast<ASailingShipPawn>(GetOwner());
	if (!Ship)
	{
		UE_LOG(LogNavalNav, Warning, TEXT("%s is not on an ASailingShipPawn; it will do nothing."), *GetName());
		return;
	}

	LastHeadingDeg = Ship->GetHeadingDegrees();
}

void UNavalNavigatorComponent::RequestMoveTo(const FVector& Goal)
{
	if (!Ship)
	{
		return;
	}

	State = ENavigatorState::Planning;

	const FVector Start = Ship->GetActorLocation();

	UWorld* World = GetWorld();
	USeaGridSubsystem* SeaGrid = World ? World->GetSubsystem<USeaGridSubsystem>() : nullptr;
	if (SeaGrid && SeaGrid->GetGrid().IsBuilt())
	{
		// Plan for THIS ship's power: a strong ship sails through zones a weak one routes around.
		// Setting the observer power invalidates the stamped layer, so mark it dirty before the plan.
		const float ShipPower = Ship->GetPowerComponent() ? Ship->GetPowerComponent()->GetPowerLevel() : 1.0f;
		SeaGrid->ObserverPowerLevel = ShipPower;
		SeaGrid->HostilityThreshold = HostilityThreshold;
		SeaGrid->MarkThreatDirty();

		CurrentPath = SeaGrid->FindPath(Start, Goal, PathQuery);

		if (!CurrentPath.bSuccess)
		{
			// Blocked or off-grid. Fall back to a straight line so the ship still visibly tries —
			// enclosure/escape handling is Slice 4's job, not the follower's.
			UE_LOG(LogNavalNav, Warning, TEXT("%s could not plan to %s; steering straight."), *GetName(), *Goal.ToString());
			BuildStraightPath(Start, Goal);
		}
	}
	else
	{
		// No grid in this world (a bare test level): just steer straight at the goal.
		BuildStraightPath(Start, Goal);
	}

	Helmsman.Reset();
	State = ENavigatorState::Following;
}

void UNavalNavigatorComponent::Stop()
{
	State = ENavigatorState::Idle;
	CurrentPath.Reset();

	if (Ship)
	{
		Ship->SetRudderInput(0.0f);
		Ship->SetSailTrim(0.0f);
	}
}

void UNavalNavigatorComponent::BuildStraightPath(const FVector& Start, const FVector& Goal)
{
	CurrentPath.Reset();
	CurrentPath.Waypoints.Add(Start);
	CurrentPath.Waypoints.Add(Goal);
	CurrentPath.Costs.Add(0.0f);
	CurrentPath.Costs.Add(static_cast<float>(FVector::Dist2D(Start, Goal)));
	CurrentPath.bSuccess = true;
}

FHelmsmanInput UNavalNavigatorComponent::GatherInput(float DeltaTime)
{
	FHelmsmanInput Input;
	Input.ShipLocation = Ship->GetActorLocation();
	Input.ShipHeadingDeg = Ship->GetHeadingDegrees();
	Input.ShipSpeed = Ship->GetSpeed();

	// Finite-difference yaw rate, purely to damp the helm. Guard the divide on the first/hitched tick.
	if (DeltaTime > UE_KINDA_SMALL_NUMBER)
	{
		Input.ShipYawRateDeg = FSailingModel::NormalizeDegrees(Input.ShipHeadingDeg - LastHeadingDeg) / DeltaTime;
	}

	// Read the same wind the ship sails by, so the tack decision matches what the hull actually does.
	if (UWindSubsystem* Wind = GetWorld() ? GetWorld()->GetSubsystem<UWindSubsystem>() : nullptr)
	{
		Input.WindFromDeg = Wind->GetWindFromYaw();
		Input.WindStrength = Wind->GetWindStrength();
	}
	else
	{
		Input.WindFromDeg = Ship->FallbackWindFromYaw;
		Input.WindStrength = Ship->FallbackWindStrength;
	}

	return Input;
}

void UNavalNavigatorComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	if (!Ship)
	{
		return;
	}

	if (State == ENavigatorState::Following)
	{
		const FHelmsmanInput Input = GatherInput(DeltaTime);
		LastOutput = Helmsman.Update(CurrentPath, Input, Ship->GetEffectiveParams(), DeltaTime);

		Ship->SetRudderInput(LastOutput.RudderInput);
		Ship->SetSailTrim(LastOutput.SailTrim);

		if (LastOutput.bArrived)
		{
			State = ENavigatorState::Arrived;
			Ship->SetRudderInput(0.0f);
			Ship->SetSailTrim(0.0f);
			OnArrived.Broadcast(this);
		}
	}

	LastHeadingDeg = Ship->GetHeadingDegrees();

	if (CVarNavDebug.GetValueOnGameThread() != 0)
	{
		DrawNavDebug();
	}
}

void UNavalNavigatorComponent::DrawNavDebug() const
{
#if ENABLE_DRAW_DEBUG
	const UWorld* World = GetWorld();
	if (!World || !Ship)
	{
		return;
	}

	const FVector Lift(0.0, 0.0, 60.0);

	// The route: a line through every waypoint, with a small sphere at each.
	if (CurrentPath.bSuccess)
	{
		for (int32 Index = 0; Index < CurrentPath.Num(); ++Index)
		{
			const FVector Point = CurrentPath.Waypoints[Index] + Lift;
			if (Index > 0)
			{
				DrawDebugLine(World, CurrentPath.Waypoints[Index - 1] + Lift, Point, FColor(70, 130, 180),
					/*bPersistentLines=*/false, -1.0f, /*DepthPriority=*/0, /*Thickness=*/6.0f);
			}
			const bool bActive = (Index == LastOutput.ActiveWaypoint);
			DrawDebugSphere(World, Point, bActive ? 300.0f : 150.0f, 12,
				bActive ? FColor(255, 220, 80) : FColor(70, 130, 180), false, -1.0f);
		}
	}

	// Look-ahead point (where the helm is aiming) and the predicted turn-in point (where it will
	// begin the next corner). These two are the whole story of "predictive".
	DrawDebugSphere(World, LastOutput.LookAheadPoint + Lift, 220.0f, 16, FColor(60, 230, 120), false, -1.0f, 0, 6.0f);
	DrawDebugLine(World, Ship->GetActorLocation() + Lift, LastOutput.LookAheadPoint + Lift,
		FColor(60, 230, 120), false, -1.0f, 0, 4.0f);
	DrawDebugSphere(World, LastOutput.TurnInPoint + Lift, 180.0f, 12, FColor(255, 140, 40), false, -1.0f);

	const FString Readout = FString::Printf(
		TEXT("Navigator: %s\nwaypoint %d/%d | bearing err %.0f deg | rudder %.2f | trim %.2f%s"),
		StateName(State), LastOutput.ActiveWaypoint, FMath::Max(0, CurrentPath.Num() - 1),
		LastOutput.BearingErrorDeg, LastOutput.RudderInput, LastOutput.SailTrim,
		LastOutput.bTacking ? TEXT(" | TACKING") : TEXT(""));
	DrawDebugString(World, Ship->GetActorLocation() + FVector(0.0, 0.0, 650.0), Readout,
		/*TestBaseActor=*/nullptr, FColor::Yellow, /*Duration=*/0.0f, /*bDrawShadow=*/true);
#endif // ENABLE_DRAW_DEBUG
}
