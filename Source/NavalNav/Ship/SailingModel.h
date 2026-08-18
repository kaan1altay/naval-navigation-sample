// Copyright (c) 2026 Kaan Altay. Licensed under the MIT License.

#pragma once

#include "CoreMinimal.h"

#include "SailingModel.generated.h"

/**
 * Tunables for the sailing model. Grouped so the details panel reads like a rig sheet.
 *
 * Deliberately a plain USTRUCT with no engine dependency beyond the reflection macros: the
 * model that consumes it (FSailingModel) is pure math, which is what lets the whole thing be
 * unit-tested — and compiled — without a UWorld, mirroring the Slice 1 pathfinder core.
 */
USTRUCT(BlueprintType)
struct NAVALNAV_API FSailingModelParams
{
	GENERATED_BODY()

	//~ Speed -----------------------------------------------------------------------------------

	/** Top speed (uu/s) on the best point of sail in full wind. Everything else is a fraction of it. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sailing|Speed", meta = (ClampMin = "1.0"))
	float MaxSpeed = 1200.0f;

	/**
	 * Peak forward acceleration (uu/s^2) at full drive. It sets both how briskly the ship builds
	 * way and, together with MaxSpeed, the quadratic drag: drag is scaled so that thrust and drag
	 * balance exactly at MaxSpeed, which is what makes the top speed bounded rather than a tuned
	 * coincidence.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sailing|Speed", meta = (ClampMin = "1.0"))
	float MaxThrustAccel = 350.0f;

	//~ Steering --------------------------------------------------------------------------------

	/** Yaw rate (deg/s) at full rudder and full speed. Scaled down by speed via SteeringResponseSpeedRatio. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sailing|Steering", meta = (ClampMin = "0.0"))
	float MaxTurnRateDegPerSec = 22.0f;

	/** Hard-over rudder deflection in degrees; the -1..1 helm order maps onto +/- this. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sailing|Steering", meta = (ClampMin = "0.0"))
	float MaxRudderAngleDegrees = 35.0f;

	/** How fast the rudder itself swings toward the ordered angle. A real helm is not instantaneous. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sailing|Steering", meta = (ClampMin = "0.0"))
	float RudderRateDegPerSec = 45.0f;

	/**
	 * Speed ratio (speed / MaxSpeed) at which the rudder reaches full authority. Below it the
	 * rudder bites proportionally less, which is the constraint that makes the Slice 3 follower
	 * non-trivial (a ship must have way on to corner hard).
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sailing|Steering", meta = (ClampMin = "0.01", ClampMax = "1.0"))
	float SteeringResponseSpeedRatio = 0.5f;

	/**
	 * Fraction of MaxTurnRate a *stationary* ship can still turn at. Zero would let a ship stall
	 * head-to-wind with no way to steer out of the no-go cone ("in irons") forever. A small floor
	 * (~0.2) lets it slowly rotate out; think of it as the game-friendly stand-in for backing the
	 * sails, poling, or drifting the bow round. It never gives drive — only the ability to turn.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sailing|Steering", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float MinYawAuthorityAtRest = 0.2f;

	//~ Polar curve -----------------------------------------------------------------------------

	/** Half-width of the no-go zone: within this angle of the wind's eye the sails luff and thrust is zero. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sailing|Polar", meta = (ClampMin = "0.0", ClampMax = "89.0"))
	float NoGoAngleDegrees = 40.0f;

	/** Angle off the wind where thrust peaks (a beam-to-broad reach). The polar curve tops out at 1 here. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sailing|Polar", meta = (ClampMin = "1.0", ClampMax = "179.0"))
	float BeamReachAngleDegrees = 90.0f;

	/** Polar value dead downwind, relative to the beam-reach peak of 1. Running is fast, but not the fastest. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sailing|Polar", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float DownwindFactor = 0.65f;

	//~ Leeway ----------------------------------------------------------------------------------

	/** Whether the hull sideslips downwind. Purely cosmetic drift; the planner never sees it. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sailing|Leeway")
	bool bEnableLeeway = true;

	/** Fraction of the ship's speed that leaks sideways downwind when leeway is on. Kept small. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sailing|Leeway", meta = (ClampMin = "0.0", ClampMax = "0.5"))
	float LeewayFactor = 0.06f;
};

/**
 * The evolving state of a sailing hull. All the pawn's per-tick memory lives here, so the model
 * is a pure function of (state, input, wind, dt) and nothing hides in the actor.
 */
USTRUCT(BlueprintType)
struct NAVALNAV_API FSailingState
{
	GENERATED_BODY()

	/** Heading (yaw) in degrees, normalised to (-180, 180]. The direction the bow points. */
	UPROPERTY(BlueprintReadOnly, Category = "Sailing")
	float HeadingDegrees = 0.0f;

	/** Forward speed along the heading, in uu/s. Never negative: this hull does not make sternway. */
	UPROPERTY(BlueprintReadOnly, Category = "Sailing")
	float Speed = 0.0f;

	/** Current rudder deflection in degrees, slewing toward the ordered angle. */
	UPROPERTY(BlueprintReadOnly, Category = "Sailing")
	float RudderAngleDegrees = 0.0f;

	/** Current sail trim, 0 (sheets out, no drive) to 1 (sheeted in for the current point of sail). */
	UPROPERTY(BlueprintReadOnly, Category = "Sailing")
	float SailTrim = 1.0f;
};

/**
 * A simple, deterministic, physically plausible sailing model on the XY plane — no PhysX, no
 * Chaos, no buoyancy sim. It answers three questions the game actually needs: how much drive do
 * the sails make on this point of sail, how fast can the ship turn right now, and where is it a
 * moment later.
 *
 * The heart of it is the polar curve: forward drive is zero in a no-go zone dead upwind, climbs
 * to a peak on a reach, and eases off again running downwind. Speed then integrates against a
 * quadratic drag toward a bounded terminal, and yaw comes from the rudder scaled by how much way
 * the ship has on.
 *
 * Kept as a plain struct with static, dependency-free math so it can be exercised in a unit test
 * without a world — the same discipline as FThreatEvaluator in Slice 1. FSailingShipPawn is a
 * thin shell around it; the Slice 3 helmsman will lean on PredictTurnRadius / EstimateTimeToTurn.
 */
struct NAVALNAV_API FSailingModel
{
	FSailingModelParams Params;

	/**
	 * Forward drive, 0..1, as a function of the angle (deg, 0..180) between the bow and the
	 * bearing the wind is blowing FROM. Zero at or inside the no-go zone, exactly 1 at the
	 * beam-reach angle, easing to DownwindFactor dead downwind. This one function is the sail
	 * model; everything else is bookkeeping around it.
	 */
	static float PolarThrustFactor(float AngleOffWindDegrees, const FSailingModelParams& P);

	/** Rudder effectiveness, 0..1, as a function of speed ratio. Zero at rest, full at SteeringResponseSpeedRatio. */
	static float SteeringAuthority(float SpeedRatio, const FSailingModelParams& P);

	/** Smallest angle in [0, 180] between a heading bearing and the wind-from bearing. */
	static float AngleOffWind(float HeadingDegrees, float WindFromDegrees);

	/** Normalises a bearing to (-180, 180]. */
	static float NormalizeDegrees(float Degrees);

	/**
	 * Advances State by DeltaSeconds.
	 *
	 * @param RudderInput      Helm order, -1..1, mapped onto +/- the max rudder angle.
	 * @param SailTrimInput    Sheet setting, 0..1.
	 * @param WindFromDegrees  Bearing the wind blows FROM (a ship on this heading is head-to-wind).
	 * @param WindStrength     Wind strength, 0..1.
	 * @param DeltaSeconds     Frame time; clamped internally so a hitch cannot explode the sim.
	 */
	void Advance(FSailingState& State, float RudderInput, float SailTrimInput,
		float WindFromDegrees, float WindStrength, float DeltaSeconds) const;

	/** Speed the ship converges to for a given drive (polar * wind * trim, 0..1): MaxSpeed * sqrt(Drive). */
	float TerminalSpeedForDrive(float Drive) const;

	/**
	 * Tightest turn radius (world units) the ship can hold at Speed, at full rudder. Returns a
	 * large sentinel when the ship has too little way on to turn. Slice 3 uses this to decide how
	 * early to put the helm over so it rolls out onto the next leg rather than overshooting it.
	 */
	float PredictTurnRadius(float Speed) const;

	/**
	 * Rough time (seconds) to swing the head through DeltaYawDegrees at Speed and full rudder.
	 * Ignores the rudder's own slew, which is small next to the manoeuvre. Also for the helmsman.
	 */
	float EstimateTimeToTurn(float DeltaYawDegrees, float Speed) const;
};
