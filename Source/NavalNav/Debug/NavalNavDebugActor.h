// Copyright (c) 2026 Kaan Altay. Licensed under the MIT License.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Grid/SeaGridDebugDraw.h"
#include "Grid/SeaGridPathfinder.h"
#include "Grid/SeaGridTypes.h"

#include "NavalNavDebugActor.generated.h"

/**
 * Drop-in-a-level harness for the navigation slice.
 *
 * Owns two draggable endpoints, configures the sea grid, replans between them on an interval
 * and draws the result. This is what makes the system demonstrable before there is a ship to
 * sail it: move a danger zone in the editor and watch the route bend around it.
 *
 * Console variables: naval.DrawGrid, naval.DrawCosts, naval.DrawPath.
 */
UCLASS(Blueprintable, HideCategories = (Replication, Collision, HLOD, Physics, Networking, Input))
class NAVALNAV_API ANavalNavDebugActor : public AActor
{
	GENERATED_BODY()

public:
	ANavalNavDebugActor();

	//~ Begin AActor interface
	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;
	/** Lets the overlay update in an editor viewport, without entering PIE. */
	virtual bool ShouldTickIfViewportsOnly() const override { return true; }
	//~ End AActor interface

	/** Plans immediately, ignoring the replan interval. */
	UFUNCTION(BlueprintCallable, CallInEditor, Category = "Naval|Debug")
	void Replan();

	/** Most recent plan, kept for inspection in the details panel and by Blueprints. */
	UFUNCTION(BlueprintPure, Category = "Naval|Debug")
	const FNavalPath& GetLastPath() const { return LastPath; }

	/** Start of the demo route, as an offset from this actor. Draggable in the viewport. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Naval|Debug", meta = (MakeEditWidget = "true"))
	FVector StartOffset = FVector(-15000.0, 0.0, 0.0);

	/** End of the demo route, as an offset from this actor. Draggable in the viewport. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Naval|Debug", meta = (MakeEditWidget = "true"))
	FVector GoalOffset = FVector(15000.0, 0.0, 0.0);

	/** Applied to the sea grid subsystem on BeginPlay when bConfigureGridOnBeginPlay is set. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Naval|Debug")
	FSeaGridConfig GridConfig;

	/** Leave enabled unless the grid is configured elsewhere (a game mode, a data asset). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Naval|Debug")
	bool bConfigureGridOnBeginPlay = true;

	/** Power level routes are planned for; zones weaker than this are ignored entirely. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Naval|Debug", meta = (ClampMin = "0.0"))
	float ObserverPowerLevel = 1.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Naval|Debug")
	FSeaGridPathQuery PathQuery;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Naval|Debug")
	FSeaGridDebugDrawSettings DrawSettings;

	/** Seconds between automatic replans. Zero replans every frame, which is only for tuning. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Naval|Debug", meta = (ClampMin = "0.0"))
	float ReplanInterval = 0.5f;

	/** Prints plan cost, waypoint count and expanded cells to the on-screen debug messages. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Naval|Debug")
	bool bShowStatsOnScreen = true;

private:
	FVector GetStartLocation() const { return GetActorLocation() + StartOffset; }
	FVector GetGoalLocation() const { return GetActorLocation() + GoalOffset; }

	/** Camera position when there is one, otherwise this actor. Used to throttle the overlay. */
	FVector GetViewLocation() const;

	void DrawEverything(float Duration) const;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Naval|Debug", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<USceneComponent> SceneRoot;

	UPROPERTY(VisibleInstanceOnly, BlueprintReadOnly, Category = "Naval|Debug", meta = (AllowPrivateAccess = "true"))
	FNavalPath LastPath;

	float TimeSinceLastPlan = 0.0f;

	/** Milliseconds taken by the most recent plan; shown on screen next to the cost. */
	double LastPlanMilliseconds = 0.0;
};
