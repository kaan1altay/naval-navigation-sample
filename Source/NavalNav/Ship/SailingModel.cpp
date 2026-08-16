// Copyright (c) 2026 Kaan Altay. Licensed under the MIT License.

#include "Ship/SailingModel.h"

namespace
{
	/** Radius (uu) returned when the ship has too little way on to turn at all. */
	constexpr float NoTurnRadius = 1.0e12f;

	constexpr float DegToRad = UE_PI / 180.0f;

	/** Classic Hermite smoothstep, clamped. Kept local so the model needs nothing from the engine. */
	float SmoothStep(float T)
	{
		T = FMath::Clamp(T, 0.0f, 1.0f);
		return T * T * (3.0f - 2.0f * T);
	}
}

float FSailingModel::PolarThrustFactor(float AngleOffWindDegrees, const FSailingModelParams& P)
{
	const float Angle = FMath::Clamp(FMath::Abs(AngleOffWindDegrees), 0.0f, 180.0f);

	const float NoGo = FMath::Clamp(P.NoGoAngleDegrees, 0.0f, 89.0f);
	if (Angle <= NoGo)
	{
		// Dead upwind: the sails luff and make no drive. This is the whole point of a sailing
		// ship over a motorboat, and what forces the follower to tack instead of pointing straight.
		return 0.0f;
	}

	// The peak has to sit strictly between the no-go edge and dead downwind, or the two curve
	// halves collapse; clamp it there rather than trust the authored value.
	const float Beam = FMath::Clamp(P.BeamReachAngleDegrees, NoGo + 1.0f, 179.0f);

	if (Angle <= Beam)
	{
		// Close-hauled edge of the no-go (0) rising to the beam-reach peak (1).
		return SmoothStep((Angle - NoGo) / (Beam - NoGo));
	}

	// Beam-reach peak (1) easing down to DownwindFactor dead downwind.
	const float Down = FMath::Clamp(P.DownwindFactor, 0.0f, 1.0f);
	const float T = SmoothStep((Angle - Beam) / (180.0f - Beam));
	return 1.0f - (1.0f - Down) * T;
}

float FSailingModel::SteeringAuthority(float SpeedRatio, const FSailingModelParams& P)
{
	const float Ref = FMath::Max(P.SteeringResponseSpeedRatio, 0.01f);
	return FMath::Clamp(FMath::Max(SpeedRatio, 0.0f) / Ref, 0.0f, 1.0f);
}

float FSailingModel::NormalizeDegrees(float Degrees)
{
	// A loop rather than fmod: callers normalise every tick, so the head never spins more than a
	// turn or two off centre and this runs a handful of iterations at most. It also keeps the
	// model free of <cmath> so the engine-free test harness compiles it unchanged.
	while (Degrees > 180.0f)
	{
		Degrees -= 360.0f;
	}
	while (Degrees <= -180.0f)
	{
		Degrees += 360.0f;
	}
	return Degrees;
}

float FSailingModel::AngleOffWind(float HeadingDegrees, float WindFromDegrees)
{
	return FMath::Abs(NormalizeDegrees(HeadingDegrees - WindFromDegrees));
}

void FSailingModel::Advance(FSailingState& State, float RudderInput, float SailTrimInput,
	float WindFromDegrees, float WindStrength, float DeltaSeconds) const
{
	// A dropped frame or an editor pause can hand us a huge dt; clamp it so a single step can
	// never leap the ship across the map or ring the integrator.
	const float Dt = FMath::Clamp(DeltaSeconds, 0.0f, 0.25f);
	if (Dt <= 0.0f)
	{
		return;
	}

	const FSailingModelParams& P = Params;

	// --- Sheets set directly; rudder slews toward the ordered angle -------------------------
	State.SailTrim = FMath::Clamp(SailTrimInput, 0.0f, 1.0f);

	const float MaxRudder = FMath::Max(P.MaxRudderAngleDegrees, 0.0f);
	const float TargetRudder = FMath::Clamp(RudderInput, -1.0f, 1.0f) * MaxRudder;
	const float RudderStep = FMath::Max(P.RudderRateDegPerSec, 0.0f) * Dt;
	State.RudderAngleDegrees += FMath::Clamp(TargetRudder - State.RudderAngleDegrees, -RudderStep, RudderStep);

	// --- Yaw: rudder deflection scaled by how much way the ship has on -----------------------
	const float MaxSpeed = FMath::Max(P.MaxSpeed, 1.0f);
	const float SpeedRatio = FMath::Clamp(State.Speed / MaxSpeed, 0.0f, 1.0f);
	const float Authority = SteeringAuthority(SpeedRatio, P);
	const float RudderFrac = MaxRudder > 0.0f ? State.RudderAngleDegrees / MaxRudder : 0.0f;
	const float YawRateDeg = P.MaxTurnRateDegPerSec * RudderFrac * Authority;
	State.HeadingDegrees = NormalizeDegrees(State.HeadingDegrees + YawRateDeg * Dt);

	// --- Speed: polar thrust against quadratic drag, toward a bounded terminal ----------------
	const float Angle = AngleOffWind(State.HeadingDegrees, WindFromDegrees);
	const float Polar = PolarThrustFactor(Angle, P);
	const float Drive = Polar * FMath::Clamp(WindStrength, 0.0f, 1.0f) * State.SailTrim; // 0..1
	const float MaxAccel = FMath::Max(P.MaxThrustAccel, 0.0f);
	const float Thrust = MaxAccel * Drive;

	// Drag is scaled so thrust and drag balance at MaxSpeed on the best point of sail; that is
	// what pins the terminal speed to MaxSpeed * sqrt(Drive) rather than leaving it to chance.
	const float DragCoeff = MaxAccel / (MaxSpeed * MaxSpeed);

	// Semi-implicit drag (old speed in the denominator): unconditionally stable and never sends
	// the speed negative, no matter how large the frame, while converging to the same terminal
	// as the explicit form.
	const float OldSpeed = FMath::Max(State.Speed, 0.0f);
	State.Speed = (OldSpeed + Thrust * Dt) / (1.0f + DragCoeff * OldSpeed * Dt);
	State.Speed = FMath::Max(State.Speed, 0.0f);
}

float FSailingModel::TerminalSpeedForDrive(float Drive) const
{
	return FMath::Max(Params.MaxSpeed, 1.0f) * FMath::Sqrt(FMath::Clamp(Drive, 0.0f, 1.0f));
}

float FSailingModel::PredictTurnRadius(float Speed) const
{
	const float MaxSpeed = FMath::Max(Params.MaxSpeed, 1.0f);
	const float SpeedRatio = FMath::Clamp(Speed / MaxSpeed, 0.0f, 1.0f);
	const float YawRateDeg = Params.MaxTurnRateDegPerSec * SteeringAuthority(SpeedRatio, Params); // full rudder
	if (YawRateDeg <= UE_KINDA_SMALL_NUMBER)
	{
		return NoTurnRadius;
	}

	// r = v / omega, with omega in radians per second.
	return FMath::Max(Speed, 0.0f) / (YawRateDeg * DegToRad);
}

float FSailingModel::EstimateTimeToTurn(float DeltaYawDegrees, float Speed) const
{
	const float MaxSpeed = FMath::Max(Params.MaxSpeed, 1.0f);
	const float SpeedRatio = FMath::Clamp(Speed / MaxSpeed, 0.0f, 1.0f);
	const float YawRateDeg = Params.MaxTurnRateDegPerSec * SteeringAuthority(SpeedRatio, Params); // full rudder
	if (YawRateDeg <= UE_KINDA_SMALL_NUMBER)
	{
		return NoTurnRadius;
	}

	return FMath::Abs(DeltaYawDegrees) / YawRateDeg;
}
