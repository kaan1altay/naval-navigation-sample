// Copyright (c) 2026 Kaan Altay. Licensed under the MIT License.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameModeBase.h"
#include "Grid/SeaGridTypes.h"

#include "NavalNavDemoGameMode.generated.h"

class UNavalNavigatorComponent;

/**
 * Stands the whole sample up from an empty level.
 *
 * Set this as the GameMode (or make it the project default) and press Play on a blank map: it
 * configures the sea grid and the wind, scatters a few danger zones, spawns a small fleet of
 * navigator-driven ships, and — if left to wander — keeps handing them fresh destinations so the
 * predictive helmsman is always doing something worth recording. No .umap authoring required.
 *
 * One ship is given a high power level on purpose, to show relative threat: it sails straight
 * through zones the weaker ships route right around.
 */
UCLASS()
class NAVALNAV_API ANavalNavDemoGameMode : public AGameModeBase
{
	GENERATED_BODY()

public:
	ANavalNavDemoGameMode();

	//~ Begin AGameModeBase interface
	virtual void BeginPlay() override;
	//~ End AGameModeBase interface

	/** Sea grid to build on BeginPlay. The default covers the standard 40 km field. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Naval|Demo")
	FSeaGridConfig GridConfig;

	/** How many ships to spawn. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Naval|Demo", meta = (ClampMin = "1", ClampMax = "16"))
	int32 NumShips = 3;

	/** How many danger zones to scatter. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Naval|Demo", meta = (ClampMin = "0", ClampMax = "32"))
	int32 NumDangerZones = 4;

	/** Half-size (uu) of the area ships and zones are spawned within, around the grid centre. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Naval|Demo", meta = (ClampMin = "1000.0"))
	float FieldRadius = 15000.0f;

	/** Wind direction (yaw the wind blows toward) to set on the world's wind subsystem. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Naval|Demo")
	float WindDirectionYaw = 45.0f;

	/** Wind strength, 0..1. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Naval|Demo", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float WindStrength = 0.7f;

	/** Power given to the one "flagship"; well above a danger zone so it ignores them. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Naval|Demo", meta = (ClampMin = "0.0"))
	float FlagshipPower = 8.0f;

	/** When true, each ship gets a random destination and a new one every time it arrives. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Naval|Demo")
	bool bAutoWander = true;

private:
	/** Re-orders a ship to a fresh random destination when it arrives, so the demo never stops. */
	UFUNCTION()
	void OnShipArrived(UNavalNavigatorComponent* Navigator);

	/** A random point within FieldRadius of the grid centre, at sea level. */
	FVector RandomSeaPoint() const;
};
