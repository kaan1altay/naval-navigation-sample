// Copyright (c) 2026 Kaan Altay. Licensed under the MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Grid/SeaGridTypes.h"
#include "Ship/SailingModel.h"

#include "PredictiveHelmsman.generated.h"

/**
 * Tunables for the helmsman. Grouped so the details panel reads like a set of standing orders.
 *
 * A plain USTRUCT with no engine dependency beyond the reflection macros, on purpose: the
 * helmsman that consumes it (FPredictiveHelmsman) is pure geometry, so it can be unit-tested and
 * run in a closed loop with FSailingModel without a UWorld — the same discipline as the sailing
 * model and the pathfinder core.
 */
USTRUCT(BlueprintType)
struct NAVALNAV_API FHelmsmanParams
{
	GENERATED_BODY()

	//~ Look-ahead ------------------------------------------------------------------------------

	/** Base look-ahead distance (uu). The helmsman steers at a point this far along the path, not at the next corner. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Helmsman|Look-ahead", meta = (ClampMin = "0.0"))
	float LookAheadBase = 1500.0f;

	/** Extra look-ahead per unit of speed (seconds, effectively): faster ship looks further ahead. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Helmsman|Look-ahead", meta = (ClampMin = "0.0"))
	float LookAheadPerSpeed = 1.5f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Helmsman|Look-ahead", meta = (ClampMin = "1.0"))
	float LookAheadMin = 1000.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Helmsman|Look-ahead", meta = (ClampMin = "1.0"))
	float LookAheadMax = 6000.0f;

	//~ Steering --------------------------------------------------------------------------------

	/** Proportional gain: rudder per degree of bearing error. 0.05 saturates the helm at a 20 deg error. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Helmsman|Steering", meta = (ClampMin = "0.0"))
	float SteerP = 0.05f;

	/** Derivative gain: rudder per deg/s of yaw rate. Damps the swing so the ship settles instead of hunting. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Helmsman|Steering", meta = (ClampMin = "0.0"))
	float SteerD = 0.02f;

	/** Bearing error (deg) below which the helm is left amidships, to stop it twitching dead on course. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Helmsman|Steering", meta = (ClampMin = "0.0"))
	float BearingDeadbandDeg = 2.0f;

	//~ Predictive turn-in ----------------------------------------------------------------------

	/** Multiplies the geometric turn-in distance. Above 1 starts corners earlier, below 1 later. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Helmsman|Turn-in", meta = (ClampMin = "0.0"))
	float TurnInLeadScale = 1.0f;

	//~ Arrival ---------------------------------------------------------------------------------

	/** Within this distance (uu) of a waypoint it counts as reached and the helmsman moves to the next. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Helmsman|Arrival", meta = (ClampMin = "0.0"))
	float WaypointAcceptRadius = 800.0f;

	/** Within this distance (uu) of the final waypoint the ship is Arrived and stops driving. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Helmsman|Arrival", meta = (ClampMin = "0.0"))
	float ArrivalRadius = 500.0f;

	/** Distance (uu) from the goal at which the ship starts easing sheets to slow for arrival. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Helmsman|Arrival", meta = (ClampMin = "0.0"))
	float SlowdownRadius = 4000.0f;

	/**
	 * A ship cannot reach a point inside its own turning circle by turning toward it — it just
	 * orbits. So rather than instantly "arriving" near any close goal (which would ignore a player
	 * clicking beside the ship), the helmsman lets it try, and only gives up once it has turned
	 * this many degrees near the goal without reaching it — i.e. it is demonstrably circling.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Helmsman|Arrival", meta = (ClampMin = "90.0"))
	float OrbitGiveUpTurnDeg = 400.0f;

	/** A final waypoint that has fallen behind the ship counts as reached within this multiple of ArrivalRadius. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Helmsman|Arrival", meta = (ClampMin = "1.0"))
	float ArrivalBehindFactor = 2.0f;

	//~ Trim ------------------------------------------------------------------------------------

	/** Ease the sheets in a hard turn so the ship carves rather than powering wide. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Helmsman|Trim")
	bool bEaseTrimInTurns = true;

	/** Bearing error (deg) at which trim is eased fully to MinTurnTrim. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Helmsman|Trim", meta = (ClampMin = "1.0"))
	float TurnTrimEaseDeg = 60.0f;

	/** Floor the turn-ease trim never drops below, so the ship keeps enough way on to steer. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Helmsman|Trim", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float MinTurnTrim = 0.5f;

	/**
	 * Trim is never eased below this while still following, so the ship keeps steerage way. Arrival
	 * slowdown and turn-easing both floor here — a ship with no way on has no rudder authority and
	 * would otherwise coast to a stall short of the goal and orbit it. Only released once Arrived.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Helmsman|Trim", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float MinSteerageTrim = 0.35f;

	//~ Tacking ---------------------------------------------------------------------------------

	/** How far outside the no-go cone (deg) the tack heading is held, so the ship actually makes way close-hauled. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Helmsman|Tacking", meta = (ClampMin = "0.0"))
	float TackMarginDeg = 3.0f;

	/** Extra margin (deg) the true bearing must clear the no-go by before the helmsman stops tacking, to avoid flip-flopping. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Helmsman|Tacking", meta = (ClampMin = "0.0"))
	float TackHysteresisDeg = 5.0f;
};

/** Everything the helmsman needs to know about the world this tick. A plain struct; the caller fills it. */
struct FHelmsmanInput
{
	/** World-space ship position (XY used). */
	FVector ShipLocation = FVector::ZeroVector;

	/** Ship heading in degrees (0 = +X, 90 = +Y), matching the pawn and the sailing model. */
	float ShipHeadingDeg = 0.0f;

	/** Forward speed, uu/s. */
	float ShipSpeed = 0.0f;

	/** Measured yaw rate, deg/s. Used only to damp the helm; zero is a safe default. */
	float ShipYawRateDeg = 0.0f;

	/** Bearing the wind blows FROM, in degrees (as UWindSubsystem::GetWindFromYaw reports it). */
	float WindFromDeg = 0.0f;

	/** Wind strength, 0..1. Currently informational; the tack decision only needs the direction. */
	float WindStrength = 1.0f;
};

/** The helm and sheet orders for this tick, plus telemetry the debug draw and tests read. */
USTRUCT(BlueprintType)
struct NAVALNAV_API FHelmsmanOutput
{
	GENERATED_BODY()

	/** Rudder order, -1..1, to hand to ASailingShipPawn::SetRudderInput. */
	UPROPERTY(BlueprintReadOnly, Category = "Helmsman")
	float RudderInput = 0.0f;

	/** Sail trim order, 0..1, to hand to ASailingShipPawn::SetSailTrim. */
	UPROPERTY(BlueprintReadOnly, Category = "Helmsman")
	float SailTrim = 1.0f;

	/** True once the ship is within ArrivalRadius of the final waypoint. */
	UPROPERTY(BlueprintReadOnly, Category = "Helmsman")
	bool bArrived = false;

	/** Index of the waypoint currently being steered toward. */
	UPROPERTY(BlueprintReadOnly, Category = "Helmsman")
	int32 ActiveWaypoint = 0;

	/** Heading the helmsman wants (after turn-in blend and any tack), degrees. */
	UPROPERTY(BlueprintReadOnly, Category = "Helmsman")
	float DesiredHeadingDeg = 0.0f;

	/** Signed bearing error (desired - actual), normalised to (-180, 180]. */
	UPROPERTY(BlueprintReadOnly, Category = "Helmsman")
	float BearingErrorDeg = 0.0f;

	/** True while the desired course is upwind and the ship is holding a tack instead. */
	UPROPERTY(BlueprintReadOnly, Category = "Helmsman")
	bool bTacking = false;

	/** The point on the path the helmsman is steering at. */
	UPROPERTY(BlueprintReadOnly, Category = "Helmsman")
	FVector LookAheadPoint = FVector::ZeroVector;

	/** The point on the incoming leg where the ship should begin the next corner. */
	UPROPERTY(BlueprintReadOnly, Category = "Helmsman")
	FVector TurnInPoint = FVector::ZeroVector;
};

/**
 * A predictive helmsman: it drives a sailing ship along an FNavalPath the way a real one would —
 * looking ahead, starting turns *before* the corner, and tacking when the course it wants is dead
 * upwind.
 *
 * The design rule this slice preserves is that **the planner is physics-agnostic and the helmsman
 * owns the physics.** The path is intent, not a rail: A* knows nothing about wind, turn radius or
 * momentum, so it is the helmsman that reconciles the geometric route with a hull that cannot
 * point upwind and cannot corner on a dime. It leans on FSailingModel's PredictTurnRadius /
 * EstimateTimeToTurn to know how early to put the helm over.
 *
 * Kept as a plain struct with a little persistent state (the active waypoint and the current tack)
 * so a closed-loop sim of FSailingModel + FPredictiveHelmsman runs in a unit test with no world.
 */
struct NAVALNAV_API FPredictiveHelmsman
{
	FHelmsmanParams Params;

	/** Produces this tick's helm and sheet orders. Call every tick while following a path. */
	FHelmsmanOutput Update(const FNavalPath& Path, const FHelmsmanInput& Input,
		const FSailingModelParams& Sail, float DeltaSeconds);

	/** Clears the tracked waypoint and tack. Call when handed a fresh path. */
	void Reset();

	/** Index of the waypoint currently being steered toward. Exposed for the navigator and tests. */
	int32 GetActiveWaypoint() const { return ActiveWaypoint; }

	//~ Static geometry helpers, pure and testable ---------------------------------------------

	/** Compass bearing (deg, 0 = +X, 90 = +Y) from one point to another. */
	static float BearingDegrees(const FVector& From, const FVector& To);

	/**
	 * Distance (uu) before a corner at which the ship should begin turning, so it rolls out onto
	 * the next leg instead of overshooting. Geometrically R * tan(turn/2), where R is the turn
	 * radius at this speed — so it grows with both the sharpness of the corner and the speed.
	 */
	static float TurnInDistance(float CornerTurnDeg, float Speed, const FSailingModelParams& Sail, float LeadScale = 1.0f);

private:
	/** Advances ActiveWaypoint past any waypoint that is reached or already behind the ship. */
	void AdvanceWaypoint(const FNavalPath& Path, const FVector& ShipLocation);

	/** Persistent: the waypoint being steered toward (1..Num-1; 0 is the start point). */
	int32 ActiveWaypoint = 1;

	/** Persistent: holding a tack because the wanted course is upwind. */
	bool bTacking = false;

	/** Persistent: which side of the wind the current tack is on (+1 or -1). */
	int32 TackSign = 0;

	/** Persistent: heading last tick, to accumulate turning while near the goal (orbit detection). */
	float PrevHeadingDeg = 0.0f;
	bool bHasPrevHeading = false;

	/** Persistent: total turning (deg) done while within the slowdown radius of the goal. */
	float ApproachTurnDeg = 0.0f;
};
