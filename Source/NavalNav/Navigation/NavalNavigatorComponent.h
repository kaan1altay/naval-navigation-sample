// Copyright (c) 2026 Kaan Altay. Licensed under the MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Grid/SeaGridPathfinder.h"
#include "Grid/SeaGridTypes.h"
#include "Navigation/PredictiveHelmsman.h"

#include "NavalNavigatorComponent.generated.h"

class ASailingShipPawn;

/** Where the navigator is in its life cycle. Escaping (breaking off from a threat) arrives in Slice 4. */
UENUM(BlueprintType)
enum class ENavigatorState : uint8
{
	/** No order; the ship is left to coast. */
	Idle,
	/** A move order was given and a route is being planned (synchronous, so this is momentary). */
	Planning,
	/** A route exists and the helmsman is steering along it. */
	Following,
	/** The ship reached the goal and has stopped driving. */
	Arrived
};

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnNavigatorArrived, UNavalNavigatorComponent*, Navigator);

/**
 * Gives a sailing ship a destination and gets it there.
 *
 * This is the seam that keeps the design honest: the **planner is physics-agnostic** — it asks the
 * sea grid for the cheapest route given the ship's power, and gets back an FNavalPath that knows
 * nothing about wind or turning circles — while the **helmsman owns the physics**, reconciling that
 * geometric intent with a hull that cannot point upwind or corner instantly. The component wires
 * the two together and drives an ASailingShipPawn with the result.
 *
 * State machine: Idle -> Planning -> Following -> Arrived. Console: naval.Nav.Debug.
 */
UCLASS(ClassGroup = (Naval), meta = (BlueprintSpawnableComponent))
class NAVALNAV_API UNavalNavigatorComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UNavalNavigatorComponent();

	//~ Begin UActorComponent interface
	virtual void BeginPlay() override;
	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;
	//~ End UActorComponent interface

	/** Plans a route to Goal (world space) and starts following it. Replaces any current order. */
	UFUNCTION(BlueprintCallable, Category = "Naval|Navigation")
	void RequestMoveTo(const FVector& Goal);

	/** Drops the current order; the ship sheets out and coasts to a stop. */
	UFUNCTION(BlueprintCallable, Category = "Naval|Navigation")
	void Stop();

	UFUNCTION(BlueprintPure, Category = "Naval|Navigation")
	ENavigatorState GetState() const { return State; }

	UFUNCTION(BlueprintPure, Category = "Naval|Navigation")
	const FNavalPath& GetCurrentPath() const { return CurrentPath; }

	UFUNCTION(BlueprintPure, Category = "Naval|Navigation")
	const FHelmsmanOutput& GetLastOutput() const { return LastOutput; }

	/** Fires once when the ship reaches its goal. Slice 4 will use this to hand out the next order. */
	UPROPERTY(BlueprintAssignable, Category = "Naval|Navigation")
	FOnNavigatorArrived OnArrived;

	/** Helmsman tuning. Shared with the tests, so the feel here is the feel that is verified. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Naval|Navigation")
	FHelmsmanParams HelmsmanParams;

	/** Pathfinding tunables for the plan (heuristic weight, string pull, budget). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Naval|Navigation")
	FSeaGridPathQuery PathQuery;

	/** How much stronger a danger zone must be than this ship before it bends the route. See FThreatEvaluator. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Naval|Navigation", meta = (ClampMin = "0.0"))
	float HostilityThreshold = 0.0f;

private:
	/** Fills a two-waypoint straight path, used when there is no sea grid to plan against. */
	void BuildStraightPath(const FVector& Start, const FVector& Goal);

	/** Gathers ship + wind state into the helmsman's input struct. */
	FHelmsmanInput GatherInput(float DeltaTime);

	/** Immediate-mode overlay of the route, active waypoint, look-ahead / turn-in points and state. */
	void DrawNavDebug() const;

	/** The ship this navigator drives; cached from the owner on BeginPlay. */
	UPROPERTY(Transient)
	TObjectPtr<ASailingShipPawn> Ship;

	UPROPERTY(VisibleInstanceOnly, BlueprintReadOnly, Category = "Naval|Navigation", meta = (AllowPrivateAccess = "true"))
	ENavigatorState State = ENavigatorState::Idle;

	/** The route currently being followed. */
	FNavalPath CurrentPath;

	/** The helmsman; a plain struct holding its own tack/waypoint state between ticks. */
	FPredictiveHelmsman Helmsman;

	/** Most recent helm orders and telemetry, kept for the details panel and the debug draw. */
	UPROPERTY(VisibleInstanceOnly, BlueprintReadOnly, Category = "Naval|Navigation", meta = (AllowPrivateAccess = "true"))
	FHelmsmanOutput LastOutput;

	/** Previous heading, for a finite-difference yaw rate to feed the helmsman's damping term. */
	float LastHeadingDeg = 0.0f;
};
