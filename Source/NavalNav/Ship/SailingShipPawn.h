// Copyright (c) 2026 Kaan Altay. Licensed under the MIT License.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Pawn.h"
#include "Ship/SailingModel.h"

#include "SailingShipPawn.generated.h"

class UCameraComponent;
class USceneComponent;
class USpringArmComponent;
class UStaticMeshComponent;
class UShipPowerComponent;
class USailingShipConfig;

/**
 * A sailing ship as a kinematic pawn.
 *
 * It does not use PhysX or Chaos: the whole hull is FSailingModel integrated on the XY plane and
 * written straight onto the actor transform. That is a deliberate choice for a navigation sample
 * — the follower in Slice 3 needs a ship whose behaviour is readable, deterministic and cheap to
 * predict, not a buoyancy sim with a mind of its own.
 *
 * Inputs are two numbers: a rudder order (-1..1) and a sail trim (0..1). Everything the ship does
 * with them lives in FSailingModel; this class is the shell that feeds it the wind, moves the
 * actor, and draws the debug overlay (naval.Ship.Debug).
 */
UCLASS(Blueprintable, HideCategories = (Replication, Collision, HLOD, Networking))
class NAVALNAV_API ASailingShipPawn : public APawn
{
	GENERATED_BODY()

public:
	ASailingShipPawn();

	//~ Begin AActor / APawn interface
	virtual void OnConstruction(const FTransform& Transform) override;
	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;
	/** Lets the ship sail and draw in an editor viewport for tuning, without entering PIE. */
	virtual bool ShouldTickIfViewportsOnly() const override { return true; }
	//~ End AActor / APawn interface

	/** Orders the rudder. -1 is hard over one way, +1 the other, 0 amidships. Held until changed. */
	UFUNCTION(BlueprintCallable, Category = "Sailing")
	void SetRudderInput(float InRudderInput) { RudderInput = FMath::Clamp(InRudderInput, -1.0f, 1.0f); }

	/** Sets the sail trim, 0 (sheets out) to 1 (sheeted in). Held until changed. */
	UFUNCTION(BlueprintCallable, Category = "Sailing")
	void SetSailTrim(float InSailTrim) { SailTrimInput = FMath::Clamp(InSailTrim, 0.0f, 1.0f); }

	UFUNCTION(BlueprintPure, Category = "Sailing")
	float GetHeadingDegrees() const { return State.HeadingDegrees; }

	UFUNCTION(BlueprintPure, Category = "Sailing")
	float GetSpeed() const { return State.Speed; }

	UFUNCTION(BlueprintPure, Category = "Sailing")
	float GetRudderAngleDegrees() const { return State.RudderAngleDegrees; }

	UFUNCTION(BlueprintPure, Category = "Sailing")
	float GetSailTrim() const { return State.SailTrim; }

	UFUNCTION(BlueprintPure, Category = "Sailing")
	const FSailingState& GetSailingState() const { return State; }

	/** The parameters actually in force (the referenced config if any, else the inline set). */
	const FSailingModelParams& GetEffectiveParams() const;

	/** A ready-to-use model built from the effective parameters. Cheap; construct one per query. */
	FSailingModel MakeModel() const;

	/** Tightest turn radius at the ship's current speed. Convenience wrapper for the helmsman. */
	UFUNCTION(BlueprintPure, Category = "Sailing")
	float PredictTurnRadius(float Speed) const { return MakeModel().PredictTurnRadius(Speed); }

	/** Seconds to swing the head through DeltaYawDegrees at the ship's current speed. */
	UFUNCTION(BlueprintPure, Category = "Sailing")
	float EstimateTimeToTurn(float DeltaYawDegrees) const { return MakeModel().EstimateTimeToTurn(DeltaYawDegrees, State.Speed); }

	UShipPowerComponent* GetPowerComponent() const { return PowerComponent; }

	/** Tints the hull. Used to colour the demo fleet (flagship gold, escorts steel blue). */
	UFUNCTION(BlueprintCallable, Category = "Sailing")
	void SetHullColor(FLinearColor Color);

	/** Marks this ship the player-selected one (draws a ring on the water and enables its overlays). */
	UFUNCTION(BlueprintCallable, Category = "Sailing")
	void SetSelected(bool bInSelected);

	/** Whether this is the player-selected ship. Gates the per-ship overlays and the selection ring. */
	UFUNCTION(BlueprintPure, Category = "Sailing")
	bool IsSelected() const { return bSelected; }

	/** Multi-line status text (heading, speed, rudder, trim, wind, off-wind) for the HUD overlay. */
	UFUNCTION(BlueprintPure, Category = "Sailing")
	FString GetStatusText() const;

	/** Nudges the chase-cam boom length, clamped to a sensible range. Used by the demo's mouse-wheel zoom. */
	UFUNCTION(BlueprintCallable, Category = "Sailing")
	void AddCameraZoom(float Delta);

	/** Inline sailing tunables, used unless SailingConfig points at a data asset. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sailing")
	FSailingModelParams SailingParams;

	/** Optional shared tunables. When set, these win over SailingParams. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sailing")
	TObjectPtr<USailingShipConfig> SailingConfig;

	/** Z height the hull is pinned to; the model is purely 2D, like the planner. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sailing")
	float SeaLevelZ = 0.0f;

	/** Wind assumed when there is no wind subsystem (a bare test level). Bearing the wind blows FROM. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sailing|Fallback Wind")
	float FallbackWindFromYaw = 225.0f;

	/** Wind strength assumed when there is no wind subsystem. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sailing|Fallback Wind", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float FallbackWindStrength = 0.7f;

private:
	/** Reads the wind subsystem (or the fallback), advances the model, writes the transform. */
	void StepSailing(float DeltaSeconds);

	/** Immediate-mode overlay: heading arrow, wind arrow, rudder line (selected ship only). */
	void DrawShipDebug() const;

	/** A bright ring on the water marking the selected ship. Always on, not a debug toggle. */
	void DrawSelectionRing() const;

	/** Pushes the current position into the wake buffer and draws a short fading trail (all ships). */
	void UpdateAndDrawWake();

	/** Recent positions for the wake trail, oldest first. */
	TArray<FVector> WakePoints;

	/** Root the visuals and camera hang off. The hull never collides, so a scene root is enough. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Sailing", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<USceneComponent> ShipRoot;

	/** Builds the hull's dynamic material (once) and applies the current colour and selection. */
	void RefreshHullMaterial();

	/** Placeholder hull mesh; a level can swap the mesh, but it keeps its dynamic material tint. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Sailing", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<UStaticMeshComponent> HullMesh;

	/** Base lit material the hull's dynamic instance is made from (an engine primitive material). */
	UPROPERTY()
	TObjectPtr<class UMaterialInterface> HullBaseMaterial;

	/** Dynamic instance carrying the per-ship colour, so tinting is reliable regardless of the mesh. */
	UPROPERTY(Transient)
	TObjectPtr<class UMaterialInstanceDynamic> HullMaterial;

	/** The ship's assigned hull colour. Default is a saturated crimson that reads on the blue sea. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sailing", meta = (AllowPrivateAccess = "true"))
	FLinearColor HullColor = FLinearColor(0.80f, 0.06f, 0.06f);

	/** Whether this ship is the player-selected one (draws a ring, enables its overlays). */
	bool bSelected = false;

	/** Chase-cam boom, so possessing the ship gives a view over the transom without extra setup. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Sailing", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<USpringArmComponent> CameraBoom;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Sailing", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<UCameraComponent> ChaseCamera;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Naval|Power", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<UShipPowerComponent> PowerComponent;

	/** Evolving hull state. Visible for tuning, not editable: the model owns it. */
	UPROPERTY(VisibleInstanceOnly, BlueprintReadOnly, Category = "Sailing", meta = (AllowPrivateAccess = "true"))
	FSailingState State;

	/** Current helm order, -1..1. */
	float RudderInput = 0.0f;

	/** Current sail trim order, 0..1. */
	float SailTrimInput = 1.0f;
};
