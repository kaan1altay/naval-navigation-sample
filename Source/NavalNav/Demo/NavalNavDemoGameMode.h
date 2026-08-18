// Copyright (c) 2026 Kaan Altay. Licensed under the MIT License.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameModeBase.h"
#include "Grid/SeaGridTypes.h"
#include "Threat/DangerZone.h" // for EZoneMovement, used as a default argument below

#include "NavalNavDemoGameMode.generated.h"

class ASailingShipPawn;
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
	virtual void Tick(float DeltaSeconds) override;
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

	/** Colour of the navigable sea (a mid blue, matte). The outer sea is a darker desaturated navy. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Naval|Demo")
	FLinearColor SeaColor = FLinearColor(0.035f, 0.12f, 0.30f);

	/** Ship power the grid overlay is stamped for, so the drawn threat stays steady as ships replan. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Naval|Demo", meta = (ClampMin = "0.0"))
	float OverlayObserverPower = 1.0f;

	/**
	 * Tears down the fleet and zones and sets up scenario 5..9 deterministically. Called by the
	 * demo controller's number keys.
	 *   5 Baseline      - static zones, ships wander between random goals.
	 *   6 Moving zone   - a patrolling zone slides across a ship's route -> mid-voyage replan.
	 *   7 Power contrast- two ships, weak and strong, same start and goal -> different routes.
	 *   8 Enclosure     - a ship ringed by zones with one weak gap -> escapes through the gap.
	 *   9 Power drop    - a strong ship crossing a zone; press P to weaken it -> it re-solves around.
	 */
	UFUNCTION(BlueprintCallable, Category = "Naval|Demo")
	void StartScenario(int32 Index);

	/** One-line title/help for the current scenario, shown on screen. */
	const FString& GetScenarioTitle() const { return ScenarioTitle; }

private:
	/** Re-orders a ship to a fresh random destination when it arrives, so the demo never stops. */
	UFUNCTION()
	void OnShipArrived(UNavalNavigatorComponent* Navigator);

	/** Spawns a sun, sky and the sea so an empty level is actually visible. */
	void SpawnEnvironment();

	/** Spawns one flat matte sea plane of the given colour (no grid material — the water stays blue). */
	void SpawnSeaPlane(const FVector& Centre, float WorldSize, const FLinearColor& Colour);

	/** Scatters dark islets just outside the grid to frame the play area. */
	void SpawnRocks();

	/** Draws a persistent frame around the navigable grid so the playable edge is always visible. */
	void DrawBoundaryFrame() const;

	/** Draws a sparse dark-blue lattice over the navigable area (a nautical-chart motion reference). */
	void DrawChartLattice() const;

	/** Destroys every ship and danger zone, ready for a fresh scenario. */
	void ClearScenarioActors();

	/** Spawns a hull at Loc with the given power and colour. */
	ASailingShipPawn* SpawnShip(const FVector& Loc, float Power, const FLinearColor& Color, float HeadingYaw = 0.0f);

	/** Adds a navigator to a ship; optionally binds auto-wander and gives it an initial goal. */
	UNavalNavigatorComponent* AddNavigator(ASailingShipPawn* Ship, bool bWander, const FVector& InitialGoal);

	/** Spawns a danger zone. */
	ADangerZone* SpawnZone(const FVector& Loc, float Radius, float Power, EZoneMovement Movement = EZoneMovement::Static);

	/** A random point within FieldRadius of the grid centre, at sea level (seeded, deterministic). */
	FVector RandomSeaPoint();

	/** Draws the sea-grid cost field around the view when naval.DrawGrid is on (no debug actor needed). */
	void DrawGridOverlay() const;

	/** Draws each zone's rim ring and power number, always on, so recordings are self-explanatory. */
	void DrawZoneAnnotations() const;

	/** Puts the demo controller's camera on the first ship after a scenario is spawned. */
	void PossessFirstShip();

	/** Deterministic RNG so scenarios reset the same way every time. */
	FRandomStream DemoRandom;

	/** The scenario currently running (5..9). */
	int32 CurrentScenario = 5;

	/** On-screen title/help for the current scenario. */
	FString ScenarioTitle;
};
