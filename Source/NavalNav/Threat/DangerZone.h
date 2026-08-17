// Copyright (c) 2026 Kaan Altay. Licensed under the MIT License.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"

#include "DangerZone.generated.h"

class USphereComponent;
class UStaticMeshComponent;
class UMaterialInterface;
class UMaterialInstanceDynamic;
struct FSeaGridData;

/**
 * A patch of sea that a ship would rather not sail through: a shore battery, a hostile
 * squadron, a reef under fire.
 *
 * The actor itself carries no navigation logic. It owns a footprint (position, radius, power)
 * and knows how to stamp that footprint into the grid's threat layer; the sea grid subsystem
 * decides when stamping happens. Keeping the write one-directional means several zones can
 * overlap without any of them having to know about the others.
 */
UCLASS(Blueprintable, HideCategories = (Replication, Collision, HLOD, Physics, Networking))
class NAVALNAV_API ADangerZone : public AActor
{
	GENERATED_BODY()

public:
	ADangerZone();

	//~ Begin AActor interface
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void Tick(float DeltaSeconds) override;
	virtual void OnConstruction(const FTransform& Transform) override;
#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif
	//~ End AActor interface

	/**
	 * Adds this zone's contribution to the threat layer.
	 * Costs are accumulated rather than assigned, so two overlapping zones are worse than one.
	 *
	 * @param Grid                 Grid to write into.
	 * @param ObserverPowerLevel   Power of the ship the route is being planned for.
	 * @param HostilityThreshold   Power advantage required before this zone bends routes.
	 */
	void StampThreat(FSeaGridData& Grid, float ObserverPowerLevel, float HostilityThreshold) const;

	UFUNCTION(BlueprintPure, Category = "Naval|Threat")
	float GetRadius() const { return Radius; }

	UFUNCTION(BlueprintPure, Category = "Naval|Threat")
	float GetPowerLevel() const { return PowerLevel; }

	/** Applies a new radius and refreshes both the visualiser and the threat layer. */
	UFUNCTION(BlueprintCallable, Category = "Naval|Threat")
	void SetRadius(float NewRadius);

	/** Applies a new power level and refreshes the threat layer. */
	UFUNCTION(BlueprintCallable, Category = "Naval|Threat")
	void SetPowerLevel(float NewPowerLevel);

	/** How far the danger reaches, in world units. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Naval|Threat", meta = (ClampMin = "0.0", UIMin = "0.0"))
	float Radius = 3000.0f;

	/** Strength of whatever sits here, compared against the planning ship's own power. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Naval|Threat", meta = (ClampMin = "0.0", UIMin = "0.0"))
	float PowerLevel = 2.0f;

	/** Cost added at the centre, in multiples of open-water cost, before power scaling. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Naval|Threat", meta = (ClampMin = "0.0"))
	float MaxThreatCost = 8.0f;

	/** 1 is a linear falloff; higher values keep the outskirts cheap. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Naval|Threat", meta = (ClampMin = "0.1"))
	float FalloffExponent = 2.0f;

	/** Inner fraction of the radius that is flatly impassable. 0 leaves the zone crossable. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Naval|Threat", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float LethalRadiusFraction = 0.25f;

	/** Drives the zone back and forth so replanning can be demonstrated without gameplay. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Naval|Threat|Movement")
	bool bMoves = false;

	/** Direction of the demo movement in world space; normalised on BeginPlay. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Naval|Threat|Movement", meta = (EditCondition = "bMoves"))
	FVector MoveAxis = FVector(0.0, 1.0, 0.0);

	/** Peak offset from the placed location, in world units. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Naval|Threat|Movement", meta = (EditCondition = "bMoves", ClampMin = "0.0"))
	float MoveAmplitude = 5000.0f;

	/** Seconds for one full there-and-back cycle. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Naval|Threat|Movement", meta = (EditCondition = "bMoves", ClampMin = "0.1"))
	float MovePeriod = 20.0f;

private:
	/** Notifies the sea grid that this footprint changed, if it moved far enough to matter. */
	void NotifyGridIfMoved();

	/** Rescales and recolours the flat disc so a zone reads in-game without any debug overlay. */
	void UpdateZoneVisual();

	/** Sphere used purely as an editor visualiser; it never collides. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Naval|Threat", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<USphereComponent> ZoneSphere;

	/** Flat translucent-looking disc drawn on the sea so the zone is visible in a plain build. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Naval|Threat", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<UStaticMeshComponent> ZoneDisc;

	/** Base material the disc's dynamic instance is made from (an engine primitive material). */
	UPROPERTY()
	TObjectPtr<UMaterialInterface> DiscBaseMaterial;

	/** Dynamic instance whose colour tracks the zone's power. */
	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> DiscMaterial;

	/** Placed location, i.e. the centre the demo movement oscillates around. */
	FVector HomeLocation = FVector::ZeroVector;

	/** Location at the last threat-layer update, used to suppress sub-cell churn. */
	FVector LastStampedLocation = FVector::ZeroVector;
};
