// Copyright (c) 2026 Kaan Altay. Licensed under the MIT License.

#include "Navigation/PredictiveHelmsman.h"

namespace
{
	/** 2D dot product on the XY plane. */
	double Dot2D(const FVector& A, const FVector& B)
	{
		return A.X * B.X + A.Y * B.Y;
	}

	/** Clamps the corner angle before tan(): dead-reverse corners would otherwise blow the turn-in up. */
	constexpr float MaxCornerForTurnIn = 160.0f;
}

void FPredictiveHelmsman::Reset()
{
	ActiveWaypoint = 1;
	bTacking = false;
	TackSign = 0;
	bHasPrevHeading = false;
	ApproachTurnDeg = 0.0f;
}

float FPredictiveHelmsman::BearingDegrees(const FVector& From, const FVector& To)
{
	return static_cast<float>(FMath::RadiansToDegrees(FMath::Atan2(To.Y - From.Y, To.X - From.X)));
}

float FPredictiveHelmsman::TurnInDistance(float CornerTurnDeg, float Speed, const FSailingModelParams& Sail, float LeadScale)
{
	FSailingModel Model;
	Model.Params = Sail;

	const float Radius = Model.PredictTurnRadius(Speed);
	// A ship with no way on cannot pre-turn; there is nothing to lead into.
	if (Radius >= 1.0e9f)
	{
		return 0.0f;
	}

	const float HalfAngle = FMath::Clamp(FMath::Abs(CornerTurnDeg), 0.0f, MaxCornerForTurnIn) * 0.5f;
	return Radius * FMath::Tan(FMath::DegreesToRadians(HalfAngle)) * FMath::Max(LeadScale, 0.0f);
}

void FPredictiveHelmsman::AdvanceWaypoint(const FNavalPath& Path, const FVector& ShipLocation)
{
	const int32 Num = Path.Num();
	ActiveWaypoint = FMath::Clamp(ActiveWaypoint, 1, FMath::Max(1, Num - 1));

	// Never advance off the goal: the final waypoint is where arrival is judged, not skipped.
	while (ActiveWaypoint < Num - 1)
	{
		const FVector& Target = Path.Waypoints[ActiveWaypoint];
		const FVector& Prev = Path.Waypoints[ActiveWaypoint - 1];

		const bool bReached = FVector::Dist2D(ShipLocation, Target) < Params.WaypointAcceptRadius;

		// "Behind the ship": the ship has projected past the far end of this leg. This is what stops
		// the helmsman circling back for a waypoint it has already sailed past.
		bool bPassed = false;
		const FVector Segment = Target - Prev;
		const double SegLenSq = Dot2D(Segment, Segment);
		if (SegLenSq > UE_KINDA_SMALL_NUMBER)
		{
			bPassed = (Dot2D(ShipLocation - Prev, Segment) / SegLenSq) > 1.0;
		}

		if (bReached || bPassed)
		{
			++ActiveWaypoint;
		}
		else
		{
			break;
		}
	}
}

FHelmsmanOutput FPredictiveHelmsman::Update(const FNavalPath& Path, const FHelmsmanInput& Input,
	const FSailingModelParams& Sail, float DeltaSeconds)
{
	FHelmsmanOutput Out;

	const int32 Num = Path.Num();
	if (!Path.bSuccess || Num == 0)
	{
		// No path to follow: sheets out, helm amidships, and let the ship coast to a stop.
		Out.SailTrim = 0.0f;
		Out.DesiredHeadingDeg = Input.ShipHeadingDeg;
		return Out;
	}

	const TArray<FVector>& W = Path.Waypoints;
	const FVector& Ship = Input.ShipLocation;
	const FVector& Goal = W[Num - 1];
	const float GoalDist = static_cast<float>(FVector::Dist2D(Ship, Goal));

	// The heading is needed both for arrival and, later, for steering.
	const float HeadingRad = FMath::DegreesToRadians(Input.ShipHeadingDeg);
	const FVector Forward(FMath::Cos(HeadingRad), FMath::Sin(HeadingRad), 0.0);

	// Orbit detection: accumulate how much the ship has turned while near the goal. A ship that has
	// come most of the way round without arriving is circling a point inside its own turning circle
	// and never will — so it gives up. Crucially this does NOT fire on a fresh order (the ship gets
	// to try first), which is what a player clicking beside the ship expects.
	if (bHasPrevHeading)
	{
		const float TurnedThisTick = FMath::Abs(FSailingModel::NormalizeDegrees(Input.ShipHeadingDeg - PrevHeadingDeg));
		ApproachTurnDeg = (GoalDist < Params.SlowdownRadius) ? (ApproachTurnDeg + TurnedThisTick) : 0.0f;
	}
	PrevHeadingDeg = Input.ShipHeadingDeg;
	bHasPrevHeading = true;

	// --- Arrival short-circuits everything ---------------------------------------------------
	// Reached if plainly inside the accept radius, if the goal has slipped just behind the ship, or
	// if the ship has demonstrably orbited a goal it cannot physically reach.
	bool bArrived = GoalDist < Params.ArrivalRadius;
	if (!bArrived)
	{
		const bool bGoalBehind = Dot2D(Forward, Goal - Ship) < 0.0;
		if (bGoalBehind && GoalDist < Params.ArrivalRadius * Params.ArrivalBehindFactor)
		{
			bArrived = true;
		}
	}
	if (!bArrived && ApproachTurnDeg >= Params.OrbitGiveUpTurnDeg)
	{
		bArrived = true;
	}

	if (bArrived)
	{
		bTacking = false;
		TackSign = 0;
		Out.bArrived = true;
		Out.SailTrim = 0.0f;
		Out.ActiveWaypoint = FMath::Clamp(ActiveWaypoint, 0, Num - 1);
		Out.DesiredHeadingDeg = Input.ShipHeadingDeg;
		Out.LookAheadPoint = Goal;
		Out.TurnInPoint = Goal;
		return Out;
	}

	AdvanceWaypoint(Path, Ship);
	const int32 A = ActiveWaypoint;

	// --- Look-ahead point along the path -----------------------------------------------------
	const float LookAheadDist = FMath::Clamp(
		Params.LookAheadBase + Input.ShipSpeed * Params.LookAheadPerSpeed,
		Params.LookAheadMin, Params.LookAheadMax);

	// Project the ship onto the active leg, then walk forward LookAheadDist along the polyline.
	const FVector& Prev = W[A - 1];
	const FVector& Target = W[A];
	const FVector ActiveSeg = Target - Prev;
	const double ActiveLenSq = Dot2D(ActiveSeg, ActiveSeg);
	const double ProjT = ActiveLenSq > UE_KINDA_SMALL_NUMBER
		? FMath::Clamp(Dot2D(Ship - Prev, ActiveSeg) / ActiveLenSq, 0.0, 1.0)
		: 0.0;
	const FVector Projection = Prev + ActiveSeg * ProjT;
	const float DistToCorner = static_cast<float>(FVector::Dist2D(Projection, Target));

	FVector LookAhead = Goal;
	{
		double Remaining = LookAheadDist;
		FVector Current = Projection;
		int32 Idx = A;
		while (true)
		{
			const FVector& Next = W[Idx];
			const double LegLen = FVector::Dist2D(Current, Next);
			if (Remaining <= LegLen || Idx == Num - 1)
			{
				const double Frac = LegLen > UE_KINDA_SMALL_NUMBER ? FMath::Min(Remaining, LegLen) / LegLen : 0.0;
				LookAhead = Current + (Next - Current) * Frac;
				break;
			}
			Remaining -= LegLen;
			Current = Next;
			++Idx;
		}
	}

	float DesiredHeading = BearingDegrees(Ship, LookAhead);
	FVector TurnInPoint = Target;

	// --- Predictive turn-in: start the corner before we reach it -----------------------------
	if (A < Num - 1)
	{
		const float InHeading = BearingDegrees(W[A - 1], W[A]);
		const float OutHeading = BearingDegrees(W[A], W[A + 1]);
		const float CornerTurn = FSailingModel::NormalizeDegrees(OutHeading - InHeading);
		const float CornerMag = FMath::Abs(CornerTurn);

		if (CornerMag > 1.0f)
		{
			const float TurnIn = TurnInDistance(CornerMag, Input.ShipSpeed, Sail, Params.TurnInLeadScale);
			const FVector InDir = (W[A] - W[A - 1]).GetSafeNormal();
			TurnInPoint = W[A] - InDir * TurnIn;

			// Once inside the turn-in distance, bias the wanted heading toward the next leg, more
			// and more as the corner nears, so the ship carves through it rather than overshooting.
			if (TurnIn > UE_KINDA_SMALL_NUMBER && DistToCorner < TurnIn)
			{
				const float Blend = FMath::Clamp(1.0f - DistToCorner / TurnIn, 0.0f, 1.0f);
				DesiredHeading = DesiredHeading + FSailingModel::NormalizeDegrees(OutHeading - DesiredHeading) * Blend;
			}
		}
	}

	// --- Wind awareness: tack when the wanted course is dead upwind ---------------------------
	// Simple and readable on purpose: when the course we want lies inside the no-go cone, hold the
	// nearer close-hauled edge instead, with hysteresis so the tack does not flip every frame. This
	// does NOT beat to windward with laylines — a genuinely upwind goal is sailed as one tack and a
	// bear-away once the bearing clears the cone; full multi-tack beating is intentionally out of
	// scope (see docs/STATUS.md).
	const float NoGo = Sail.NoGoAngleDegrees;
	const float OffWind = FSailingModel::AngleOffWind(DesiredHeading, Input.WindFromDeg);
	bool bTackingNow = false;

	auto TackHeadingForSign = [&](int32 Sign)
	{
		return FSailingModel::NormalizeDegrees(Input.WindFromDeg + Sign * (NoGo + Params.TackMarginDeg));
	};

	if (OffWind < NoGo)
	{
		bTackingNow = true;
		int32 Sign = (bTacking && TackSign != 0)
			? TackSign
			: (FSailingModel::NormalizeDegrees(DesiredHeading - Input.WindFromDeg) >= 0.0f ? 1 : -1);
		TackSign = Sign;
		DesiredHeading = TackHeadingForSign(Sign);
	}
	else if (bTacking && OffWind < NoGo + Params.TackHysteresisDeg)
	{
		// Just outside the cone but not yet clear of the hysteresis margin: hold the tack.
		bTackingNow = true;
		const int32 Sign = TackSign != 0 ? TackSign
			: (FSailingModel::NormalizeDegrees(DesiredHeading - Input.WindFromDeg) >= 0.0f ? 1 : -1);
		TackSign = Sign;
		DesiredHeading = TackHeadingForSign(Sign);
	}
	else
	{
		TackSign = 0;
	}
	bTacking = bTackingNow;

	// --- Recovering from irons ----------------------------------------------------------------
	// A ship that wants to go upwind but has barely any way on is (or is about to be) in irons. The
	// close-hauled tack edge makes almost no drive, so from a standstill hold a fuller point of sail
	// (bear away past the no-go) until the ship has way on; the normal tack heading then resumes and
	// it points back up. While the bow is still physically inside the cone, put the helm hard over so
	// it rotates out under the model's at-rest yaw authority instead of waiting on a weak PD term.
	const float HeadingOffWind = FSailingModel::AngleOffWind(Input.ShipHeadingDeg, Input.WindFromDeg);
	const float SpeedRatioNow = Input.ShipSpeed / FMath::Max(Sail.MaxSpeed, 1.0f);
	const bool bLowSpeed = SpeedRatioNow < Params.IronsSpeedRatio;
	if (bLowSpeed && bTacking)
	{
		DesiredHeading = FSailingModel::NormalizeDegrees(Input.WindFromDeg + TackSign * (NoGo + Params.IronsBearAwayDeg));
	}

	// --- Rudder: PD on bearing error, damped by the measured yaw rate -------------------------
	const float BearingError = FSailingModel::NormalizeDegrees(DesiredHeading - Input.ShipHeadingDeg);
	const float ProportionalError = FMath::Abs(BearingError) < Params.BearingDeadbandDeg ? 0.0f : BearingError;
	float Rudder = FMath::Clamp(
		Params.SteerP * ProportionalError - Params.SteerD * Input.ShipYawRateDeg,
		-1.0f, 1.0f);

	if (bLowSpeed && HeadingOffWind < NoGo && FMath::Abs(BearingError) > UE_KINDA_SMALL_NUMBER)
	{
		Rudder = BearingError >= 0.0f ? 1.0f : -1.0f;
	}

	// --- Trim: full, eased for arrival and for hard turns -------------------------------------
	float Trim = 1.0f;

	const float SlowdownSpan = FMath::Max(Params.SlowdownRadius - Params.ArrivalRadius, UE_KINDA_SMALL_NUMBER);
	Trim = FMath::Min(Trim, FMath::Clamp((GoalDist - Params.ArrivalRadius) / SlowdownSpan, 0.0f, 1.0f));

	if (Params.bEaseTrimInTurns)
	{
		const float TurnFrac = FMath::Clamp(FMath::Abs(BearingError) / FMath::Max(Params.TurnTrimEaseDeg, 1.0f), 0.0f, 1.0f);
		Trim = FMath::Min(Trim, 1.0f - (1.0f - Params.MinTurnTrim) * TurnFrac);
	}

	// Keep steerage way: we only reach here while still following (arrival already returned), so the
	// ship must not sheet out so far it loses the way it needs to turn onto — that is the orbit trap.
	Trim = FMath::Max(Trim, Params.MinSteerageTrim);

	// --- Fill the output ----------------------------------------------------------------------
	Out.RudderInput = Rudder;
	Out.SailTrim = FMath::Clamp(Trim, 0.0f, 1.0f);
	Out.bArrived = false;
	Out.ActiveWaypoint = A;
	Out.DesiredHeadingDeg = DesiredHeading;
	Out.BearingErrorDeg = BearingError;
	Out.bTacking = bTacking;
	Out.LookAheadPoint = LookAhead;
	Out.TurnInPoint = TurnInPoint;
	return Out;
}
