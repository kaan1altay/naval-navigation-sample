// Copyright (c) 2026 Kaan Altay. Licensed under the MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"

#include "ShipPowerComponent.generated.h"

/**
 * Broadcast when a ship's power level changes. Slice 4 hangs replanning off this: a ship that
 * loses power stops outgunning zones it used to sail straight through, so its route has to be
 * re-evaluated.
 */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnPowerChanged, float, OldPower, float, NewPower);

/**
 * A ship's combat power, as one number.
 *
 * Threat in this sample is relative (see FThreatEvaluator): a zone only bends a route when it
 * outguns the ship by enough. That comparison needs the ship's power to live somewhere, and a
 * small component is the least surprising home — it drops onto any pawn, and it fires an event
 * when the number moves so the planner can react instead of polling.
 */
UCLASS(ClassGroup = (Naval), meta = (BlueprintSpawnableComponent))
class NAVALNAV_API UShipPowerComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UShipPowerComponent();

	/** Fires whenever the power level actually changes. Bound by Slice 4's replanning triggers. */
	UPROPERTY(BlueprintAssignable, Category = "Naval|Power")
	FOnPowerChanged OnPowerChanged;

	UFUNCTION(BlueprintPure, Category = "Naval|Power")
	float GetPowerLevel() const { return PowerLevel; }

	/** Sets the power level (clamped to >= 0) and broadcasts if it changed. */
	UFUNCTION(BlueprintCallable, Category = "Naval|Power")
	void SetPowerLevel(float NewPowerLevel);

	/** Adds Delta to the power level. Negative to model taking damage. */
	UFUNCTION(BlueprintCallable, Category = "Naval|Power")
	void ModifyPowerLevel(float Delta) { SetPowerLevel(PowerLevel + Delta); }

protected:
	//~ Begin UActorComponent interface
	virtual void BeginPlay() override;
	//~ End UActorComponent interface

	/** Current combat power, compared against danger-zone power to decide what is a threat. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Naval|Power", meta = (ClampMin = "0.0"))
	float PowerLevel = 1.0f;
};
