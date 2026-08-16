// Copyright (c) 2026 Kaan Altay. Licensed under the MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"

#include "WindSubsystem.generated.h"

/**
 * World-scoped source of the global wind.
 *
 * There is exactly one wind per world and everything that sails reads it here, so a UWorldSubsystem
 * is the natural home — the same reasoning as the sea grid in Slice 1. It carries a direction and
 * a strength, can drift the direction slowly for a demo, and can be overridden live from the
 * console (naval.Wind.Yaw, naval.Wind.Strength) without touching a placed actor.
 *
 * A note on convention, because sailing makes it easy to get backwards: the stored yaw is the
 * direction the wind blows TOWARD (the way a windsock points, the way a leaf drifts). The bearing
 * a ship steers to be head-to-wind is the opposite, which GetWindFromYaw() returns — that is the
 * angle the sailing model reasons about.
 */
UCLASS(Config = Game)
class NAVALNAV_API UWindSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	//~ Begin UWorldSubsystem interface
	virtual bool DoesSupportWorldType(const EWorldType::Type WorldType) const override;
	//~ End UWorldSubsystem interface

	//~ Begin FTickableGameObject interface
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;
	//~ End FTickableGameObject interface

	/** Direction the wind blows toward, in yaw degrees. Honours the console override if one is set. */
	UFUNCTION(BlueprintPure, Category = "Naval|Wind")
	float GetWindDirectionYaw() const;

	/** Bearing the wind blows FROM, in yaw degrees. This is the angle the sailing polar is measured against. */
	UFUNCTION(BlueprintPure, Category = "Naval|Wind")
	float GetWindFromYaw() const;

	/** Wind strength, 0..1. Honours the console override if one is set. */
	UFUNCTION(BlueprintPure, Category = "Naval|Wind")
	float GetWindStrength() const;

	/** Unit XY vector the wind blows toward, for debug arrows and leeway. */
	UFUNCTION(BlueprintPure, Category = "Naval|Wind")
	FVector GetWindDirection() const;

	/** Sets the wind direction (yaw the wind blows toward). Normalised to (-180, 180]. */
	UFUNCTION(BlueprintCallable, Category = "Naval|Wind")
	void SetWindDirectionYaw(float NewYawDegrees);

	/** Sets the wind strength, clamped to 0..1. */
	UFUNCTION(BlueprintCallable, Category = "Naval|Wind")
	void SetWindStrength(float NewStrength);

	/** Direction the wind blows toward, in yaw degrees. Editable in DefaultGame.ini. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Config, Category = "Naval|Wind")
	float WindDirectionYawDegrees = 45.0f;

	/** Wind strength, 0..1: how much drive the sails can make at best trim on the best point of sail. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Config, Category = "Naval|Wind", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float WindStrength = 0.7f;

	/** Slowly rotate the wind over time. Purely for demos, so a recorded clip shows the route and helm reacting. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Config, Category = "Naval|Wind|Drift")
	bool bDriftDirection = false;

	/** Drift rate in degrees per second when bDriftDirection is on. May be negative to back the wind. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Config, Category = "Naval|Wind|Drift", meta = (EditCondition = "bDriftDirection"))
	float DriftDegPerSec = 2.0f;
};
