// Copyright (c) 2026 Kaan Altay. Licensed under the MIT License.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/PlayerController.h"

#include "SailingShipPlayerController.generated.h"

class UInputAction;
class UInputMappingContext;
struct FInputActionValue;

/**
 * Lets a human sail an ASailingShipPawn: A/D work the rudder, W/S trim the sails, and the pawn's
 * own chase cam does the rest. Its reason to exist is recording clips — the follower in Slice 3
 * drives the same pawn through SetRudderInput / SetSailTrim, so nothing here is load-bearing.
 *
 * The Enhanced Input actions and mappings are built in code rather than authored as assets, so
 * the sample stays self-contained: there is no .uasset to ship and nothing to wire up in a level.
 */
UCLASS()
class NAVALNAV_API ASailingShipPlayerController : public APlayerController
{
	GENERATED_BODY()

public:
	//~ Begin AController / APlayerController interface
	virtual void BeginPlay() override;
	virtual void SetupInputComponent() override;
	virtual void PlayerTick(float DeltaTime) override;
	//~ End AController / APlayerController interface

	/** Sail trim change per second at full W/S deflection. Trim is held between presses, not sprung. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sailing|Input", meta = (ClampMin = "0.0"))
	float TrimRatePerSec = 0.6f;

private:
	/** Lazily builds the input actions and mapping context. Safe to call more than once. */
	void EnsureInputAssets();

	void OnRudder(const FInputActionValue& Value);
	void OnRudderReleased(const FInputActionValue& Value);
	void OnTrim(const FInputActionValue& Value);
	void OnTrimReleased(const FInputActionValue& Value);

	UPROPERTY(Transient)
	TObjectPtr<UInputAction> RudderAction;

	UPROPERTY(Transient)
	TObjectPtr<UInputAction> TrimAction;

	UPROPERTY(Transient)
	TObjectPtr<UInputMappingContext> MappingContext;

	/** Latest helm order from input, forwarded to the pawn each tick. */
	float RudderAxis = 0.0f;

	/** Latest trim rate order from input (-1 easing out .. +1 sheeting in). */
	float TrimAxis = 0.0f;

	/** Accumulated trim, since W/S set a rate rather than an absolute position. */
	float CurrentTrim = 1.0f;
};
