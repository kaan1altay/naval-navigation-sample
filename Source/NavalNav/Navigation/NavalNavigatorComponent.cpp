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
		case ENavigatorState::Escaping:  return TEXT("Escaping");
		case ENavigatorState::Arrived:   return TEXT("Arrived");
		default:                         return TEXT("?");
		}
	}

	const TCHAR* ReasonName(EReplanReason Reason)
	{
		switch (Reason)
		{
		case EReplanReason::OffRoute:      return TEXT("off-route");
		case EReplanReason::PathBlocked:   return TEXT("path blocked");
		case EReplanReason::ThreatChanged: return TEXT("threat changed");
		case EReplanReason::PowerChanged:  return TEXT("power changed");
		case EReplanReason::Periodic:      return TEXT("periodic");
		default:                           return TEXT("none");
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
	CacheShipAndSubscribe();
}

void UNavalNavigatorComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (ThreatChangedHandle.IsValid())
	{
		if (USeaGridSubsystem* SeaGrid = GetSeaGrid())
		{
			SeaGrid->OnThreatChanged.Remove(ThreatChangedHandle);
		}
		ThreatChangedHandle.Reset();
	}
	if (Ship && Ship->GetPowerComponent())
	{
		Ship->GetPowerComponent()->OnPowerChanged.RemoveDynamic(this, &UNavalNavigatorComponent::HandlePowerChanged);
	}

	Super::EndPlay(EndPlayReason);
}

void UNavalNavigatorComponent::CacheShipAndSubscribe()
{
	Ship = Cast<ASailingShipPawn>(GetOwner());
	if (!Ship)
	{
		UE_LOG(LogNavalNav, Warning, TEXT("%s is not on an ASailingShipPawn; it will do nothing."), *GetName());
		return;
	}

	LastHeadingDeg = Ship->GetHeadingDegrees();
	ReplanPolicy.Params = ReplanParams;

	// Replans are event-driven wherever possible: subscribe to zone changes and to this ship's own
	// power changes, so the policy only has to poll the cheap geometric triggers per tick.
	if (USeaGridSubsystem* SeaGrid = GetSeaGrid())
	{
		ThreatChangedHandle = SeaGrid->OnThreatChanged.AddUObject(this, &UNavalNavigatorComponent::HandleThreatChanged);
	}
	if (Ship->GetPowerComponent())
	{
		Ship->GetPowerComponent()->OnPowerChanged.AddDynamic(this, &UNavalNavigatorComponent::HandlePowerChanged);
	}
}

USeaGridSubsystem* UNavalNavigatorComponent::GetSeaGrid() const
{
	UWorld* World = GetWorld();
	return World ? World->GetSubsystem<USeaGridSubsystem>() : nullptr;
}

float UNavalNavigatorComponent::GetShipPower() const
{
	return (Ship && Ship->GetPowerComponent()) ? Ship->GetPowerComponent()->GetPowerLevel() : 1.0f;
}

void UNavalNavigatorComponent::RequestMoveTo(const FVector& Goal, bool bPlayerOrder)
{
	if (!Ship)
	{
		return;
	}

	if (bPlayerOrder)
	{
		bPlayerControlled = true;
	}

	CurrentGoal = Goal;
	bHasGoal = true;

	if (State == ENavigatorState::Escaping)
	{
		// Don't abandon a break-off mid-run: make this the goal to resume once the ship is clear.
		OriginalGoal = Goal;
		bHasOriginalGoal = true;
		return;
	}

	bHasOriginalGoal = false;

	SetState(ENavigatorState::Planning);
	CurrentPath = PlanPath(Ship->GetActorLocation(), Goal);
	Helmsman.Reset();
	ReplanPolicy.NotePlanned();
	SetState(ENavigatorState::Following);
}

void UNavalNavigatorComponent::Stop()
{
	SetState(ENavigatorState::Idle);
	CurrentPath.Reset();
	bHasGoal = false;
	bHasOriginalGoal = false;

	if (Ship)
	{
		Ship->SetRudderInput(0.0f);
		Ship->SetSailTrim(0.0f);
	}
}

FNavalPath UNavalNavigatorComponent::PlanPath(const FVector& Start, const FVector& Goal)
{
	FNavalPath Path;

	USeaGridSubsystem* SeaGrid = GetSeaGrid();
	if (SeaGrid && SeaGrid->GetGrid().IsBuilt())
	{
		// Plan for THIS ship's power: a strong ship sails through zones a weak one routes around.
		// Setting the observer power invalidates the stamped layer, so mark it dirty before the plan.
		SeaGrid->ObserverPowerLevel = GetShipPower();
		SeaGrid->HostilityThreshold = HostilityThreshold;
		SeaGrid->MarkThreatDirty();

		Path = SeaGrid->FindPath(Start, Goal, PathQuery);
		if (!Path.bSuccess)
		{
			// Blocked or off-grid: steer straight so the ship still visibly tries.
			BuildStraightPath(Start, Goal, Path);
		}
	}
	else
	{
		BuildStraightPath(Start, Goal, Path);
	}

	return Path;
}

void UNavalNavigatorComponent::BuildStraightPath(const FVector& Start, const FVector& Goal, FNavalPath& OutPath)
{
	OutPath.Reset();
	OutPath.Waypoints.Add(Start);
	OutPath.Waypoints.Add(Goal);
	OutPath.Costs.Add(0.0f);
	OutPath.Costs.Add(static_cast<float>(FVector::Dist2D(Start, Goal)));
	OutPath.bSuccess = true;
}

void UNavalNavigatorComponent::ConsiderReplan(EReplanReason Reason)
{
	if (!bHasGoal)
	{
		return;
	}

	const FVector Start = Ship->GetActorLocation();
	const FNavalPath Candidate = PlanPath(Start, CurrentGoal);

	// Off-route and blocked mean the old path can no longer be trusted, so any valid replacement is
	// an improvement; otherwise only switch for a meaningfully cheaper route, to avoid dithering.
	// Crucially the old path is re-costed for the ship's *current* power: after a power drop the route
	// it is on may now cross hostile water, which must make it look expensive so the safe replan wins.
	const bool bOldInvalid = (Reason == EReplanReason::PathBlocked || Reason == EReplanReason::OffRoute || !CurrentPath.bSuccess);
	if (ReplanPolicy.ShouldAdoptNewPath(RecostPath(CurrentPath), Candidate.GetTotalCost(), bOldInvalid))
	{
		// The candidate starts at the ship's current position, so the helmsman's fresh waypoint 1 is
		// already ahead of the bow: the splice keeps heading continuity, no reset-to-start jerk.
		CurrentPath = Candidate;
		Helmsman.Reset();

		// Only an actual route change counts as a replan. A periodic re-validation that yields the
		// same path is not adopted here and does not tick this counter — it stays honest on a static
		// route.
		++ReplanCount;
		LastReplanReason = Reason;
	}
}

float UNavalNavigatorComponent::RecostPath(const FNavalPath& Path) const
{
	if (!Path.bSuccess || Path.Num() < 2)
	{
		return 0.0f;
	}

	USeaGridSubsystem* SeaGrid = GetSeaGrid();
	if (!SeaGrid || !SeaGrid->GetGrid().IsBuilt())
	{
		return Path.GetTotalCost();
	}

	// Walk the path sampling this ship's observer cost per cell, so the total is on the same scale
	// as the A* cost (sum of cell cost x cells traversed). A leg through a lethal core reads as huge.
	const float Power = GetShipPower();
	const float CellSize = FMath::Max(SeaGrid->GetGrid().GetCellSize(), 1.0f);
	float Total = 0.0f;
	for (int32 Index = 1; Index < Path.Num(); ++Index)
	{
		const FVector& A = Path.Waypoints[Index - 1];
		const FVector& B = Path.Waypoints[Index];
		const float Length = static_cast<float>(FVector::Dist2D(A, B));
		const int32 Steps = FMath::Max(1, FMath::CeilToInt32(Length / CellSize));
		const float CellsPerStep = (Length / Steps) / CellSize;
		for (int32 Step = 1; Step <= Steps; ++Step)
		{
			const FVector Point = A + (B - A) * (static_cast<double>(Step) / Steps);
			Total += SeaGrid->GetObserverCellCost(Point, Power, HostilityThreshold) * CellsPerStep;
		}
	}
	return Total;
}

void UNavalNavigatorComponent::EnterEscape()
{
	USeaGridSubsystem* SeaGrid = GetSeaGrid();
	if (!SeaGrid)
	{
		return; // no grid to search; nothing to do but keep sailing
	}

	OriginalGoal = bHasGoal ? CurrentGoal : Ship->GetActorLocation();
	bHasOriginalGoal = bHasGoal;

	FVector Target;
	if (SeaGrid->FindEscapeTarget(Ship->GetActorLocation(), GetShipPower(), HostilityThreshold, EscapeSearchRadius, Target))
	{
		EscapeTarget = Target;
		CurrentPath = PlanPath(Ship->GetActorLocation(), Target);
		Helmsman.Reset();
		ReplanPolicy.NotePlanned();
		SetState(ENavigatorState::Escaping);
		UE_LOG(LogNavalNav, Verbose, TEXT("%s escaping to %s"), *GetOwner()->GetName(), *Target.ToString());
	}
}

void UNavalNavigatorComponent::SetState(ENavigatorState NewState)
{
	if (State == NewState)
	{
		return;
	}
	State = NewState;
	OnStateChanged.Broadcast(this, NewState);
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

	// Let the policy be re-tuned live from the details panel.
	ReplanPolicy.Params = ReplanParams;

	if (State == ENavigatorState::Following || State == ENavigatorState::Escaping)
	{
		const FHelmsmanInput Input = GatherInput(DeltaTime);
		LastOutput = Helmsman.Update(CurrentPath, Input, Ship->GetEffectiveParams(), DeltaTime);
		Ship->SetRudderInput(LastOutput.RudderInput);
		Ship->SetSailTrim(LastOutput.SailTrim);

		USeaGridSubsystem* SeaGrid = GetSeaGrid();
		const float ShipPower = GetShipPower();
		const FVector ShipLoc = Ship->GetActorLocation();
		const float SelfCost = SeaGrid ? SeaGrid->GetObserverCellCost(ShipLoc, ShipPower, HostilityThreshold) : NavalNav::OpenWaterCost;

		if (State == ENavigatorState::Following)
		{
			if (LastOutput.bArrived)
			{
				SetState(ENavigatorState::Arrived);
				Ship->SetRudderInput(0.0f);
				Ship->SetSailTrim(0.0f);
				OnArrived.Broadcast(this);
			}
			else if (SeaGrid && SelfCost >= EscapeCostThreshold)
			{
				// The ship is in hostile water — a zone moved onto it, or its power dropped. Break off.
				EnterEscape();
			}
			else
			{
				FReplanSituation Situation;
				Situation.DeltaSeconds = DeltaTime;
				Situation.bFollowing = true;
				Situation.ShipLocation = ShipLoc;
				Situation.Path = &CurrentPath;
				Situation.ActiveWaypoint = LastOutput.ActiveWaypoint;

				const float Threshold = HostilityThreshold;
				auto CostSampler = [SeaGrid, ShipPower, Threshold](const FVector& Point)
				{
					return SeaGrid ? SeaGrid->GetObserverCellCost(Point, ShipPower, Threshold) : NavalNav::OpenWaterCost;
				};

				const FReplanDecision Decision = ReplanPolicy.Evaluate(Situation, CostSampler);
				if (Decision.bShouldReplan)
				{
					ConsiderReplan(Decision.Reason);
				}
			}
		}
		else // Escaping
		{
			// Resume the goal the moment the ship is back in open water, or once it reaches the exit.
			const bool bClear = SelfCost <= NavalNav::OpenWaterCost + 0.5f;
			if (LastOutput.bArrived || bClear)
			{
				if (bHasOriginalGoal)
				{
					SetState(ENavigatorState::Planning);
					CurrentGoal = OriginalGoal;
					bHasGoal = true;
					bHasOriginalGoal = false;
					CurrentPath = PlanPath(ShipLoc, CurrentGoal);
					Helmsman.Reset();
					ReplanPolicy.NotePlanned();
					SetState(ENavigatorState::Following);
				}
				else
				{
					Stop();
				}
			}
		}
	}

	LastHeadingDeg = Ship->GetHeadingDegrees();

	if (CVarNavDebug.GetValueOnGameThread() != 0)
	{
		DrawNavDebug();
	}
}

void UNavalNavigatorComponent::HandlePowerChanged(float OldPower, float NewPower)
{
	ReplanPolicy.NotifyPowerChanged();
}

void UNavalNavigatorComponent::HandleThreatChanged(const FVector& Location, float Radius)
{
	if (State != ENavigatorState::Following || !Ship)
	{
		return;
	}

	if (FReplanPolicy::IsChangeRelevant(Location, Radius, Ship->GetActorLocation(), CurrentPath,
		LastOutput.ActiveWaypoint, ReplanParams.RelevanceRadius))
	{
		ReplanPolicy.NotifyThreatChanged();
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
		const FColor RouteColor = (State == ENavigatorState::Escaping) ? FColor(255, 90, 90) : FColor(70, 130, 180);
		for (int32 Index = 0; Index < CurrentPath.Num(); ++Index)
		{
			const FVector Point = CurrentPath.Waypoints[Index] + Lift;
			if (Index > 0)
			{
				DrawDebugLine(World, CurrentPath.Waypoints[Index - 1] + Lift, Point, RouteColor,
					/*bPersistentLines=*/false, -1.0f, /*DepthPriority=*/0, /*Thickness=*/6.0f);
			}
			const bool bActive = (Index == LastOutput.ActiveWaypoint);
			DrawDebugSphere(World, Point, bActive ? 300.0f : 150.0f, 12,
				bActive ? FColor(255, 220, 80) : RouteColor, false, -1.0f);
		}
	}

	// Look-ahead point (where the helm is aiming) and the predicted turn-in point (where it will
	// begin the next corner). These two are the whole story of "predictive".
	DrawDebugSphere(World, LastOutput.LookAheadPoint + Lift, 220.0f, 16, FColor(60, 230, 120), false, -1.0f, 0, 6.0f);
	DrawDebugLine(World, Ship->GetActorLocation() + Lift, LastOutput.LookAheadPoint + Lift,
		FColor(60, 230, 120), false, -1.0f, 0, 4.0f);
	DrawDebugSphere(World, LastOutput.TurnInPoint + Lift, 180.0f, 12, FColor(255, 140, 40), false, -1.0f);

	// This ship's power and whether it is currently sitting in water hostile to it.
	const float ShipPower = GetShipPower();
	float SelfCost = NavalNav::OpenWaterCost;
	if (USeaGridSubsystem* SeaGrid = GetSeaGrid())
	{
		SelfCost = SeaGrid->GetObserverCellCost(Ship->GetActorLocation(), ShipPower, HostilityThreshold);
	}
	const bool bHostile = SelfCost > NavalNav::OpenWaterCost + 0.01f;

	const FString Readout = FString::Printf(
		TEXT("Navigator: %s%s  | validations %d / replans %d (%s)\npower %.1f | hostile: %s\nwaypoint %d/%d | bearing err %.0f deg | rudder %.2f | trim %.2f%s"),
		StateName(State), bPlayerControlled ? TEXT(" [player]") : TEXT(""),
		ReplanPolicy.GetValidationCount(), ReplanCount, ReasonName(LastReplanReason),
		ShipPower, bHostile ? TEXT("YES") : TEXT("no"),
		LastOutput.ActiveWaypoint, FMath::Max(0, CurrentPath.Num() - 1),
		LastOutput.BearingErrorDeg, LastOutput.RudderInput, LastOutput.SailTrim,
		LastOutput.bTacking ? TEXT(" | TACKING") : TEXT(""));
	// Offset to the ship's starboard side so it never collides with the ship overlay (drawn to port).
	// The chase cam inherits the ship's yaw, so starboard reads as screen-right.
	const float HeadRad = FMath::DegreesToRadians(Ship->GetHeadingDegrees());
	const FVector Starboard(FMath::Sin(HeadRad), -FMath::Cos(HeadRad), 0.0);
	DrawDebugString(World, Ship->GetActorLocation() + Starboard * 1500.0 + FVector(0.0, 0.0, 550.0), Readout,
		/*TestBaseActor=*/nullptr, FColor::Yellow, /*Duration=*/0.0f, /*bDrawShadow=*/true, /*FontScale=*/1.5f);
#endif // ENABLE_DRAW_DEBUG
}
