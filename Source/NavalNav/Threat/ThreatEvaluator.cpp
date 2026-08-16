// Copyright (c) 2026 Kaan Altay. Licensed under the MIT License.

#include "Threat/ThreatEvaluator.h"

bool FThreatEvaluator::IsHostile(float ShipPowerLevel, float ZonePowerLevel, float HostilityThreshold)
{
	return (ZonePowerLevel - ShipPowerLevel) > HostilityThreshold;
}

float FThreatEvaluator::RelativeThreat(float ShipPowerLevel, float ZonePowerLevel)
{
	// Guard the divide: an unarmed observer (power 0) still has to rate threats, and it should
	// rate them as "everything is dangerous" rather than as infinity.
	const float SafeShipPower = FMath::Max(ShipPowerLevel, 1.0f);
	return FMath::Max(0.0f, (ZonePowerLevel - ShipPowerLevel) / SafeShipPower);
}

float FThreatEvaluator::CellThreatCost(float DistanceToCenter, float Radius, float ThreatScale,
	float FalloffExponent, float LethalRadiusFraction)
{
	if (Radius <= 0.0f || ThreatScale <= 0.0f || DistanceToCenter >= Radius)
	{
		return 0.0f;
	}

	const float LethalRadius = Radius * FMath::Clamp(LethalRadiusFraction, 0.0f, 1.0f);

	// The zero check matters: with no lethal core, the single cell exactly at the centre would
	// otherwise come out impassable and quietly punch a hole through an avoidable zone.
	if (LethalRadius > 0.0f && DistanceToCenter <= LethalRadius)
	{
		return NavalNav::ImpassableCost;
	}

	// Remap [LethalRadius, Radius] to [1, 0] so the cost is continuous across the core edge.
	const float Span = FMath::Max(Radius - LethalRadius, UE_KINDA_SMALL_NUMBER);
	const float Normalised = FMath::Clamp((Radius - DistanceToCenter) / Span, 0.0f, 1.0f);
	const float Falloff = FMath::Pow(Normalised, FMath::Max(FalloffExponent, UE_KINDA_SMALL_NUMBER));

	return ThreatScale * Falloff;
}
