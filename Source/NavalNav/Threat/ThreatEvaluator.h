// Copyright (c) 2026 Kaan Altay. Licensed under the MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Grid/SeaGridTypes.h"

/**
 * Rules that turn "a hostile thing is over there" into a number the planner understands.
 *
 * Pure static functions with no engine dependencies, on purpose: threat shaping is the part
 * of the system that gets tuned most often, so it has to be testable in isolation and usable
 * from anywhere (grid stamping, the follower's danger check, UI colouring) without dragging
 * in a UWorld.
 */
struct NAVALNAV_API FThreatEvaluator
{
	/**
	 * Whether a zone is worth avoiding at all.
	 *
	 * Threat is relative: a frigate does not detour around a fishing boat, and the same
	 * fishing boat is a wall to a rowing dinghy. HostilityThreshold is the power advantage the
	 * zone needs before it starts bending routes, so a single knob turns a nervous captain
	 * into a reckless one.
	 */
	static bool IsHostile(float ShipPowerLevel, float ZonePowerLevel, float HostilityThreshold);

	/**
	 * How much stronger the zone is than the ship, normalised so 0 means "evenly matched"
	 * and 1 means "twice as strong". Used to scale the cost a hostile zone contributes.
	 */
	static float RelativeThreat(float ShipPowerLevel, float ZonePowerLevel);

	/**
	 * Extra traversal cost a single cell picks up from one zone.
	 *
	 * Falls off from the centre outwards so the planner prefers to graze a zone rather than
	 * cross it, and only refuses outright inside the lethal core. A hard-edged disc would make
	 * routes flip between "straight through" and "all the way around" as a zone drifts by;
	 * a smooth falloff makes the route slide continuously instead.
	 *
	 * @param DistanceToCenter    World-space distance from the cell centre to the zone centre.
	 * @param Radius              Zone radius in world units. Zero or less means no threat.
	 * @param ThreatScale         Cost multiplier at the edge of the lethal core, in multiples
	 *                            of open-water cost. Typically PowerLevel-derived.
	 * @param FalloffExponent     1 is linear; higher values keep the outskirts cheap and
	 *                            concentrate the cost near the centre.
	 * @param LethalRadiusFraction Fraction of Radius that is flatly impassable. 0 disables it.
	 */
	static float CellThreatCost(float DistanceToCenter, float Radius, float ThreatScale,
		float FalloffExponent = 2.0f, float LethalRadiusFraction = 0.0f);
};
