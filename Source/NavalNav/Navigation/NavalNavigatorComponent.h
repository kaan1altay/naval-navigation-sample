// Copyright (c) 2026 Kaan Altay. Licensed under the MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Grid/SeaGridPathfinder.h"
#include "Grid/SeaGridTypes.h"
#include "Navigation/PredictiveHelmsman.h"
#include "Navigation/ReplanPolicy.h"

#include "NavalNavigatorComponent.generated.h"

class ASailingShipPawn;
class USeaGridSubsystem;

/** Where the navigator is in its life cycle. */
UENUM(BlueprintType)
enum class ENavigatorState : uint8
{
	/** No order; the ship is left to coast. */
	Idle,
	/** A move order was given and a route is being planned (synchronous, so this is momentary). */
	Planning,
	/** A route exists and the helmsman is steering along it. */
	Following,
	/** The ship is in hostile water and is breaking off to safety before resuming its goal. */
	Escaping,
	/** The ship reached the goal and has stopped driving. */
	Arrived
};

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnNavigatorArrived, UNavalNavigatorComponent*, Navigator);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnNavigatorStateChanged, UNavalNavigatorComponent*, Navigator, ENavigatorState, NewState);

/**
 * Gives a sailing ship a destination and gets it there — and keeps it there as the world changes.
 *
 * The seam that keeps the design honest: the **planner is physics-agnostic** (it asks the sea grid
 * for the cheapest route given the ship's power) and the **helmsman owns the physics** (it sails
 * that route against wind and turning circles). On top of those, an engine-free FReplanPolicy
 * decides *when* the route has gone stale — the ship drifted off it, a zone moved onto it, this
 * ship's power changed, or it is simply old — and the navigator replans **without stopping**,
 * splicing the new route in from the ship's current position. When the ship finds itself in
 * hostile water it breaks off (Escaping) to the nearest safe cell, or the least-bad exit if boxed
 * in, then resumes its original goal.
 *
 * State machine: Idle -> Planning -> Following -> Arrived, with Following -> Escaping -> Following
 * when a ship has to fight its way clear. Console: naval.Nav.Debug.
 */
UCLASS(ClassGroup = (Naval), meta = (BlueprintSpawnableComponent))
class NAVALNAV_API UNavalNavigatorComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UNavalNavigatorComponent();

	//~ Begin UActorComponent interface
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;
	//~ End UActorComponent interface

	/**
	 * Plans a route to Goal (world space) and starts following it, replacing any current order.
	 * A player order marks the ship player-owned so the demo's wander logic leaves it alone; issued
	 * while Escaping, it becomes the goal to resume once the ship is clear rather than interrupting
	 * the break-off.
	 */
	UFUNCTION(BlueprintCallable, Category = "Naval|Navigation")
	void RequestMoveTo(const FVector& Goal, bool bPlayerOrder = false);

	/** Drops the current order; the ship sheets out and coasts to a stop. */
	UFUNCTION(BlueprintCallable, Category = "Naval|Navigation")
	void Stop();

	UFUNCTION(BlueprintPure, Category = "Naval|Navigation")
	ENavigatorState GetState() const { return State; }

	UFUNCTION(BlueprintPure, Category = "Naval|Navigation")
	const FNavalPath& GetCurrentPath() const { return CurrentPath; }

	UFUNCTION(BlueprintPure, Category = "Naval|Navigation")
	const FHelmsmanOutput& GetLastOutput() const { return LastOutput; }

	/** How many times the route was actually re-planned (adopted a new path). */
	UFUNCTION(BlueprintPure, Category = "Naval|Navigation")
	int32 GetReplanCount() const { return ReplanCount; }

	/** How many times the policy fired a validation (asked to re-plan, adopted or not). */
	UFUNCTION(BlueprintPure, Category = "Naval|Navigation")
	int32 GetValidationCount() const { return ReplanPolicy.GetValidationCount(); }

	/** Multi-line status text (state, replans, power/hostile, waypoint/helm) for the HUD overlay. */
	UFUNCTION(BlueprintPure, Category = "Naval|Navigation")
	FString GetStatusText() const;

	/** True once a player order was issued; the demo's auto-wander then leaves this ship alone. */
	UFUNCTION(BlueprintPure, Category = "Naval|Navigation")
	bool IsPlayerControlled() const { return bPlayerControlled; }

	/** Fires once when the ship reaches its goal. */
	UPROPERTY(BlueprintAssignable, Category = "Naval|Navigation")
	FOnNavigatorArrived OnArrived;

	/** Fires whenever the state machine transitions. */
	UPROPERTY(BlueprintAssignable, Category = "Naval|Navigation")
	FOnNavigatorStateChanged OnStateChanged;

	/** Helmsman tuning. Shared with the tests, so the feel here is the feel that is verified. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Naval|Navigation")
	FHelmsmanParams HelmsmanParams;

	/** When to replan. Copied into the policy each tick so it can be tuned live. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Naval|Navigation")
	FReplanPolicyParams ReplanParams;

	/** Pathfinding tunables for the plan (heuristic weight, string pull, budget). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Naval|Navigation")
	FSeaGridPathQuery PathQuery;

	/** How much stronger a danger zone must be than this ship before it bends the route. See FThreatEvaluator. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Naval|Navigation", meta = (ClampMin = "0.0"))
	float HostilityThreshold = 0.0f;

	/** Observer cost at the ship's own cell at or above which it breaks off to escape. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Naval|Navigation|Escape", meta = (ClampMin = "0.0"))
	float EscapeCostThreshold = 3.0f;

	/** How far (uu) the escape search looks for safe water before settling for the least-bad exit. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Naval|Navigation|Escape", meta = (ClampMin = "0.0"))
	float EscapeSearchRadius = 12000.0f;

	/**
	 * Colour of this ship's drawn route. Set per ship (the demo keys it off power) so two routes
	 * over the same water stay tellable apart at recording zoom; the escape state still overrides
	 * it with red. Bright cyan by default, which is the highest-contrast colour on the blue sea.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Naval|Navigation|Debug")
	FColor RouteColor = FColor(0, 230, 255);

private:
	/** Cast the owner and cache the ship; subscribe to zone and power events. */
	void CacheShipAndSubscribe();

	/** The sea grid subsystem for this world, or null in a bare test level. */
	USeaGridSubsystem* GetSeaGrid() const;

	/** This ship's combat power, or 1 if it has no power component. */
	float GetShipPower() const;

	/** Plans a route from Start to Goal for this ship's power. Falls back to a straight line. */
	FNavalPath PlanPath(const FVector& Start, const FVector& Goal);

	/** Fills a two-waypoint straight path, used when there is no sea grid to plan against. */
	static void BuildStraightPath(const FVector& Start, const FVector& Goal, FNavalPath& OutPath);

	/** Plans to CurrentGoal and adopts the result only if the policy says it is worth it. */
	void ConsiderReplan(EReplanReason Reason);

	/** Cost of an existing path re-evaluated for this ship's *current* power, so a route that has
	 *  become dangerous (e.g. after a power drop) compares fairly against a fresh plan. */
	float RecostPath(const FNavalPath& Path) const;

	/** Breaks off toward safety and remembers the goal to resume. */
	void EnterEscape();

	/** Transitions state and broadcasts, ignoring no-op transitions. */
	void SetState(ENavigatorState NewState);

	/** Gathers ship + wind state into the helmsman's input struct. */
	FHelmsmanInput GatherInput(float DeltaTime);

	/** Immediate-mode overlay of the route, active waypoint, look-ahead / turn-in points and state. */
	void DrawNavDebug() const;

	UFUNCTION()
	void HandlePowerChanged(float OldPower, float NewPower);

	/** Bound to the grid's zone-changed event; flags a replan if the change is near this route. */
	void HandleThreatChanged(const FVector& Location, float Radius);

	/** The ship this navigator drives; cached from the owner on BeginPlay. */
	UPROPERTY(Transient)
	TObjectPtr<ASailingShipPawn> Ship;

	UPROPERTY(VisibleInstanceOnly, BlueprintReadOnly, Category = "Naval|Navigation", meta = (AllowPrivateAccess = "true"))
	ENavigatorState State = ENavigatorState::Idle;

	/** The route currently being followed (to the goal, or to safety while Escaping). */
	FNavalPath CurrentPath;

	/** The helmsman; a plain struct holding its own tack/waypoint state between ticks. */
	FPredictiveHelmsman Helmsman;

	/** The replan policy; a plain struct holding its trigger timers and hysteresis. */
	FReplanPolicy ReplanPolicy;

	/** Most recent helm orders and telemetry, kept for the details panel and the debug draw. */
	UPROPERTY(VisibleInstanceOnly, BlueprintReadOnly, Category = "Naval|Navigation", meta = (AllowPrivateAccess = "true"))
	FHelmsmanOutput LastOutput;

	/** The destination the ship is ultimately headed to. */
	FVector CurrentGoal = FVector::ZeroVector;
	bool bHasGoal = false;

	/** The goal stashed while Escaping, to resume once clear. */
	FVector OriginalGoal = FVector::ZeroVector;
	bool bHasOriginalGoal = false;

	/** Where the current escape run is headed. */
	FVector EscapeTarget = FVector::ZeroVector;

	/** Handle for the grid's zone-changed subscription, removed on EndPlay. */
	FDelegateHandle ThreatChangedHandle;

	/** Previous heading, for a finite-difference yaw rate to feed the helmsman's damping term. */
	float LastHeadingDeg = 0.0f;

	/** Times the route was actually replaced (distinct from the policy's validation count). */
	UPROPERTY(VisibleInstanceOnly, BlueprintReadOnly, Category = "Naval|Navigation", meta = (AllowPrivateAccess = "true"))
	int32 ReplanCount = 0;

	/** Reason for the most recent *adopted* replan, for the overlay. */
	EReplanReason LastReplanReason = EReplanReason::None;

	/** Set by a player order; the demo's wander will not re-task this ship. */
	UPROPERTY(VisibleInstanceOnly, BlueprintReadOnly, Category = "Naval|Navigation", meta = (AllowPrivateAccess = "true"))
	bool bPlayerControlled = false;
};
