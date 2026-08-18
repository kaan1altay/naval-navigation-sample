// Copyright (c) 2026 Kaan Altay. Licensed under the MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Grid/SeaGridTypes.h"

#include "ReplanPolicy.generated.h"

/** Why a route was judged stale. Shown in the navigator overlay and asserted by the tests. */
UENUM(BlueprintType)
enum class EReplanReason : uint8
{
	None,
	/** The ship drifted off the active leg for longer than the grace time. */
	OffRoute,
	/** A cell on the remaining path is now blocking for this ship (a zone moved onto the route). */
	PathBlocked,
	/** A relevant danger zone moved or changed power. */
	ThreatChanged,
	/** This ship's own power changed, so what counts as a threat changed with it. */
	PowerChanged,
	/** The cheap backstop: the path is simply old. */
	Periodic
};

/**
 * When to replan. All thresholds are tunable and each trigger can be switched off independently,
 * which is what lets the demo scenarios isolate one behaviour at a time and the tests pin each
 * trigger down on its own.
 */
USTRUCT(BlueprintType)
struct NAVALNAV_API FReplanPolicyParams
{
	GENERATED_BODY()

	//~ Trigger toggles -------------------------------------------------------------------------
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Replan|Triggers")
	bool bEnableOffRoute = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Replan|Triggers")
	bool bEnablePathBlocked = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Replan|Triggers")
	bool bEnableThreatChanged = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Replan|Triggers")
	bool bEnablePowerChanged = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Replan|Triggers")
	bool bEnablePeriodic = true;

	//~ Off-route -------------------------------------------------------------------------------

	/** Cross-track distance (uu) from the active leg beyond which the ship counts as off-route. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Replan|OffRoute", meta = (ClampMin = "0.0"))
	float OffRouteDistance = 1500.0f;

	/** How long (s) the ship must stay off-route before a replan fires, so a wave of leeway does not. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Replan|OffRoute", meta = (ClampMin = "0.0"))
	float OffRouteGraceSeconds = 1.5f;

	//~ Path-blocked ----------------------------------------------------------------------------

	/** Cost at or above which a cell on the remaining path is treated as blocking for this ship. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Replan|PathBlocked", meta = (ClampMin = "0.0"))
	float BlockedCostThreshold = 1.0e8f;

	/** Spacing (uu) between samples when scanning the remaining path for a block. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Replan|PathBlocked", meta = (ClampMin = "10.0"))
	float PathBlockSampleSpacing = 200.0f;

	/** How often (s) the remaining path is scanned. Cheap, but not worth doing every frame. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Replan|PathBlocked", meta = (ClampMin = "0.0"))
	float PathCheckInterval = 0.5f;

	//~ Threat relevance ------------------------------------------------------------------------

	/** A zone change farther than this from the ship or its remaining path is ignored. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Replan|Threat", meta = (ClampMin = "0.0"))
	float RelevanceRadius = 7000.0f;

	//~ Hysteresis / backstop -------------------------------------------------------------------

	/** Minimum seconds between replans. This is the storm-guard: many triggers in a window cost one A*. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Replan|Hysteresis", meta = (ClampMin = "0.0"))
	float MinReplanInterval = 1.0f;

	/**
	 * Re-validate the current path once it is older than this (s): re-plan and, only if the result
	 * is meaningfully better, adopt it. A backstop, not a churn source — on a static route the
	 * re-plan matches the current path and nothing changes.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Replan|Hysteresis", meta = (ClampMin = "0.0"))
	float MaxPathAge = 30.0f;

	/** A still-valid path is only replaced when the new one is cheaper than old * this ratio. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Replan|Hysteresis", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float ImprovementCostRatio = 0.9f;
};

/** Everything the policy needs about the world this tick. A plain struct; the owner fills it. */
struct FReplanSituation
{
	float DeltaSeconds = 0.0f;
	bool bFollowing = false;
	FVector ShipLocation = FVector::ZeroVector;
	const FNavalPath* Path = nullptr;
	int32 ActiveWaypoint = 1;
};

/** The policy's verdict for this tick. */
struct FReplanDecision
{
	bool bShouldReplan = false;
	EReplanReason Reason = EReplanReason::None;
	float CrossTrackDistance = 0.0f;
	bool bPathBlocked = false;
};

/**
 * Decides *when* a route is stale — never *how* to route, which stays with the pathfinder.
 *
 * A plain, engine-free struct holding the trigger timers and hysteresis state, so the whole
 * decision logic runs in a unit test without a world (the same discipline as the sailing model,
 * the pathfinder and the helmsman). The one thing it cannot know on its own — whether a cell on
 * the path is now blocking — it gets through an injected cost sampler, exactly as the pathfinder
 * takes an injected cost functor.
 */
struct NAVALNAV_API FReplanPolicy
{
	FReplanPolicyParams Params;

	/** Flags a relevant danger-zone change; consumed on the next replan. */
	void NotifyThreatChanged() { bThreatChangedPending = true; }

	/** Flags a change to this ship's own power; consumed on the next replan. */
	void NotifyPowerChanged() { bPowerChangedPending = true; }

	/**
	 * The per-tick decision. CostSampler returns the traversal cost at a world point for THIS
	 * ship's power; it is only invoked at PathCheckInterval, so passing an expensive one is fine.
	 * When the returned decision says to replan, the policy has already reset its own timers and
	 * counters, so the caller just needs to act.
	 */
	FReplanDecision Evaluate(const FReplanSituation& Situation, TFunctionRef<float(const FVector&)> CostSampler);

	/** Whether a freshly planned path should replace the current one. Old-invalid always adopts. */
	bool ShouldAdoptNewPath(float OldPathCost, float NewPathCost, bool bOldInvalid) const;

	/**
	 * Resets the trigger timers after an externally-driven plan (an explicit move order or an
	 * escape), so the hysteresis interval and path-check cadence start fresh. Does not count as an
	 * auto-replan, so it leaves the replan counter alone.
	 */
	void NotePlanned();

	/** Distance (uu) from the ship to the active leg, clamped to the segment. */
	static float CrossTrackDistance(const FNavalPath& Path, int32 ActiveWaypoint, const FVector& ShipLocation);

	/** Whether a change at Point (with its own radius) is close enough to the ship or path to matter. */
	static bool IsChangeRelevant(const FVector& Point, float PointRadius, const FVector& ShipLocation,
		const FNavalPath& Path, int32 ActiveWaypoint, float RelevanceRadius);

	/** How many times a trigger fired and the owner was asked to re-plan (a *validation*, not
	 *  necessarily a route change — the owner may keep the old path if the new one is no better). */
	int32 GetValidationCount() const { return ValidationCount; }
	EReplanReason GetLastReason() const { return LastReason; }

private:
	bool IsRemainingPathBlocked(const FReplanSituation& Situation, TFunctionRef<float(const FVector&)> CostSampler) const;

	/** Large initial values so the first tick may fire immediately if a trigger already stands. */
	float TimeSinceReplan = 1.0e9f;
	float TimeSincePathCheck = 1.0e9f;
	float OffRouteTimer = 0.0f;
	bool bThreatChangedPending = false;
	bool bPowerChangedPending = false;
	bool bLastPathBlocked = false;
	int32 ValidationCount = 0;
	EReplanReason LastReason = EReplanReason::None;
};
