// Copyright (c) 2026 Kaan Altay. Licensed under the MIT License.

#include "Navigation/ReplanPolicy.h"

namespace
{
	double ReplanDot2D(const FVector& A, const FVector& B)
	{
		return A.X * B.X + A.Y * B.Y;
	}

	/** Shortest distance (uu) from Point to the segment [A, B] on the XY plane. */
	float DistanceToSegment2D(const FVector& Point, const FVector& A, const FVector& B)
	{
		const FVector Segment = B - A;
		const double LengthSq = ReplanDot2D(Segment, Segment);
		if (LengthSq < UE_KINDA_SMALL_NUMBER)
		{
			return static_cast<float>(FVector::Dist2D(Point, A));
		}
		const double T = FMath::Clamp(ReplanDot2D(Point - A, Segment) / LengthSq, 0.0, 1.0);
		return static_cast<float>(FVector::Dist2D(Point, A + Segment * T));
	}
}

float FReplanPolicy::CrossTrackDistance(const FNavalPath& Path, int32 ActiveWaypoint, const FVector& ShipLocation)
{
	const int32 Num = Path.Num();
	if (Num < 2)
	{
		return 0.0f;
	}

	const int32 A = FMath::Clamp(ActiveWaypoint, 1, Num - 1);
	const FVector& P0 = Path.Waypoints[A - 1];
	const FVector& P1 = Path.Waypoints[A];

	const FVector Segment = P1 - P0;
	const double LengthSq = ReplanDot2D(Segment, Segment);
	if (LengthSq < UE_KINDA_SMALL_NUMBER)
	{
		return static_cast<float>(FVector::Dist2D(ShipLocation, P1));
	}

	const double T = FMath::Clamp(ReplanDot2D(ShipLocation - P0, Segment) / LengthSq, 0.0, 1.0);
	const FVector Projection = P0 + Segment * T;
	return static_cast<float>(FVector::Dist2D(ShipLocation, Projection));
}

bool FReplanPolicy::IsChangeRelevant(const FVector& Point, float PointRadius, const FVector& ShipLocation,
	const FNavalPath& Path, int32 ActiveWaypoint, float RelevanceRadius)
{
	const float Reach = RelevanceRadius + FMath::Max(PointRadius, 0.0f);

	if (FVector::Dist2D(Point, ShipLocation) <= Reach)
	{
		return true;
	}

	// Test the remaining legs, not just the waypoints: a zone can drift onto the middle of a long
	// leg while sitting far from either of its endpoints.
	const int32 Num = Path.Num();
	const int32 First = FMath::Clamp(ActiveWaypoint, 1, FMath::Max(1, Num - 1));
	for (int32 Index = First; Index < Num; ++Index)
	{
		if (DistanceToSegment2D(Point, Path.Waypoints[Index - 1], Path.Waypoints[Index]) <= Reach)
		{
			return true;
		}
	}
	return false;
}

bool FReplanPolicy::IsRemainingPathBlocked(const FReplanSituation& Situation, TFunctionRef<float(const FVector&)> CostSampler) const
{
	const FNavalPath& Path = *Situation.Path;
	const int32 Num = Path.Num();
	const int32 A = FMath::Clamp(Situation.ActiveWaypoint, 1, Num - 1);
	const float Spacing = FMath::Max(Params.PathBlockSampleSpacing, 10.0f);

	// Walk from the ship, through the active waypoint and on to the goal, sampling as we go.
	FVector Previous = Situation.ShipLocation;
	for (int32 Index = A; Index < Num; ++Index)
	{
		const FVector& Next = Path.Waypoints[Index];
		const float Length = static_cast<float>(FVector::Dist2D(Previous, Next));
		const int32 Steps = FMath::Max(1, FMath::CeilToInt32(Length / Spacing));
		for (int32 Step = 1; Step <= Steps; ++Step)
		{
			const double T = static_cast<double>(Step) / static_cast<double>(Steps);
			const FVector Point = Previous + (Next - Previous) * T;
			if (CostSampler(Point) >= Params.BlockedCostThreshold)
			{
				return true;
			}
		}
		Previous = Next;
	}
	return false;
}

FReplanDecision FReplanPolicy::Evaluate(const FReplanSituation& Situation, TFunctionRef<float(const FVector&)> CostSampler)
{
	FReplanDecision Decision;

	TimeSinceReplan += Situation.DeltaSeconds;
	TimeSincePathCheck += Situation.DeltaSeconds;

	if (!Situation.bFollowing || Situation.Path == nullptr || !Situation.Path->bSuccess || Situation.Path->Num() < 2)
	{
		OffRouteTimer = 0.0f;
		return Decision;
	}

	// --- Off-route: cross-track beyond the threshold for longer than the grace time -----------
	const float Cross = CrossTrackDistance(*Situation.Path, Situation.ActiveWaypoint, Situation.ShipLocation);
	Decision.CrossTrackDistance = Cross;
	if (Params.bEnableOffRoute && Cross > Params.OffRouteDistance)
	{
		OffRouteTimer += Situation.DeltaSeconds;
	}
	else
	{
		OffRouteTimer = 0.0f;
	}

	// --- Path-blocked: expensive scan, only at the configured cadence -------------------------
	if (Params.bEnablePathBlocked && TimeSincePathCheck >= Params.PathCheckInterval)
	{
		bLastPathBlocked = IsRemainingPathBlocked(Situation, CostSampler);
		TimeSincePathCheck = 0.0f;
	}
	Decision.bPathBlocked = bLastPathBlocked;

	// --- Pick the highest-priority standing trigger -------------------------------------------
	EReplanReason Reason = EReplanReason::None;
	if (Params.bEnablePathBlocked && bLastPathBlocked)
	{
		Reason = EReplanReason::PathBlocked;
	}
	else if (Params.bEnableOffRoute && OffRouteTimer >= Params.OffRouteGraceSeconds)
	{
		Reason = EReplanReason::OffRoute;
	}
	else if (Params.bEnablePowerChanged && bPowerChangedPending)
	{
		Reason = EReplanReason::PowerChanged;
	}
	else if (Params.bEnableThreatChanged && bThreatChangedPending)
	{
		Reason = EReplanReason::ThreatChanged;
	}
	else if (Params.bEnablePeriodic && TimeSinceReplan >= Params.MaxPathAge)
	{
		Reason = EReplanReason::Periodic;
	}

	// --- Gate by the hysteresis interval, then fire and reset ---------------------------------
	if (Reason != EReplanReason::None && TimeSinceReplan >= Params.MinReplanInterval)
	{
		Decision.bShouldReplan = true;
		Decision.Reason = Reason;

		TimeSinceReplan = 0.0f;
		OffRouteTimer = 0.0f;
		bThreatChangedPending = false;
		bPowerChangedPending = false;
		bLastPathBlocked = false; // recomputed against the new path on the next cadence tick
		++ReplanCount;
		LastReason = Reason;
	}

	return Decision;
}

void FReplanPolicy::NotePlanned()
{
	TimeSinceReplan = 0.0f;
	OffRouteTimer = 0.0f;
	TimeSincePathCheck = 1.0e9f; // force a fresh path scan against the new route on the next tick
	bThreatChangedPending = false;
	bPowerChangedPending = false;
	bLastPathBlocked = false;
}

bool FReplanPolicy::ShouldAdoptNewPath(float OldPathCost, float NewPathCost, bool bOldInvalid) const
{
	if (bOldInvalid)
	{
		// The old path is off-route or blocked (or there is none): take whatever we just found.
		return NewPathCost > 0.0f || OldPathCost <= 0.0f;
	}
	if (NewPathCost <= 0.0f)
	{
		return false;
	}
	// The old path is still good; only switch for a meaningfully cheaper one, to avoid dithering.
	return NewPathCost < OldPathCost * Params.ImprovementCostRatio;
}
