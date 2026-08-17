// Copyright (c) 2026 Kaan Altay. Licensed under the MIT License.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/PlayerController.h"

#include "NavalNavDemoPlayerController.generated.h"

class ASailingShipPawn;
class UInputAction;
class UInputMappingContext;
struct FInputActionValue;

/**
 * The demo's cursor-driven controller: left-click on the water orders the selected ship there,
 * Tab cycles which ship you command (and watch, via its chase cam), and 1/2/3 toggle the debug
 * overlays. It possesses a ship only for the camera — the navigator does the steering, so your
 * click sets intent and the helmsman does the sailing.
 *
 * Enhanced Input is built in code, so the demo needs no input assets and no level wiring.
 */
UCLASS()
class NAVALNAV_API ANavalNavDemoPlayerController : public APlayerController
{
	GENERATED_BODY()

public:
	//~ Begin APlayerController interface
	virtual void BeginPlay() override;
	virtual void SetupInputComponent() override;
	//~ End APlayerController interface

	/** Sea plane height clicks are projected onto. Matches the ships' SeaLevelZ. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Naval|Demo")
	float SeaLevelZ = 0.0f;

	/** Re-gathers the ships and takes the camera on the first one. The demo GameMode calls this
	 *  after it has spawned the fleet, since the controller's own BeginPlay may run first. */
	void PossessFirstAvailableShip() { PossessShipAtIndex(0); }

private:
	void EnsureInputAssets();

	void OnClickMove(const FInputActionValue& Value);
	void OnCycleShip(const FInputActionValue& Value);
	void OnToggleNavDebug(const FInputActionValue& Value);
	void OnToggleShipDebug(const FInputActionValue& Value);
	void OnToggleGridDebug(const FInputActionValue& Value);
	void OnZoom(const FInputActionValue& Value);

	/** Rebuilds the ship list and possesses the ship at Index (for its chase cam). */
	void PossessShipAtIndex(int32 Index);

	/** Flips a 0/1 console variable and logs the new state. */
	static void ToggleCVar(const TCHAR* Name, const TCHAR* Label);

	UPROPERTY(Transient)
	TObjectPtr<UInputAction> ClickAction;

	UPROPERTY(Transient)
	TObjectPtr<UInputAction> CycleAction;

	UPROPERTY(Transient)
	TObjectPtr<UInputAction> NavDebugAction;

	UPROPERTY(Transient)
	TObjectPtr<UInputAction> ShipDebugAction;

	UPROPERTY(Transient)
	TObjectPtr<UInputAction> GridDebugAction;

	UPROPERTY(Transient)
	TObjectPtr<UInputAction> ZoomAction;

	UPROPERTY(Transient)
	TObjectPtr<UInputMappingContext> MappingContext;

	/** Ships in the level, gathered on BeginPlay; Tab steps through them. */
	UPROPERTY(Transient)
	TArray<TObjectPtr<ASailingShipPawn>> Ships;

	int32 CurrentShipIndex = 0;
};
