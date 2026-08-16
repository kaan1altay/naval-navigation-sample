// Copyright (c) 2026 Kaan Altay. Licensed under the MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Grid/SeaGridTypes.h"

#include "SeaGridDebugDraw.generated.h"

class UWorld;

/**
 * Throttling and styling for the grid overlay.
 *
 * A 200x200 grid is 40,000 cells; drawing all of them as debug boxes with text costs more
 * than the navigation it is meant to explain. Every field here exists to keep the overlay
 * readable and affordable rather than complete.
 */
USTRUCT(BlueprintType)
struct NAVALNAV_API FSeaGridDebugDrawSettings
{
	GENERATED_BODY()

	/** Cells further than this from the viewpoint are skipped. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debug Draw", meta = (ClampMin = "0.0"))
	float MaxDrawDistance = 20000.0f;

	/** Hard cap on drawn cells, so a huge grid degrades gracefully instead of hitching. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debug Draw", meta = (ClampMin = "0"))
	int32 MaxDrawnCells = 3000;

	/** Numeric cost labels are far more expensive than boxes, so they get their own budget. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debug Draw", meta = (ClampMin = "0"))
	int32 MaxCostLabels = 250;

	/** Skip open-water cells: usually the threat layer is the only interesting part. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debug Draw")
	bool bOnlyDrawThreatenedCells = true;

	/** Cost at which the colour ramp saturates to red. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debug Draw", meta = (ClampMin = "0.1"))
	float CostColorScale = 8.0f;

	/** Lifts the overlay off the water plane so it is not z-fighting the sea surface. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debug Draw")
	float DrawHeightOffset = 50.0f;

	/** Line thickness for path segments. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debug Draw", meta = (ClampMin = "0.0"))
	float PathThickness = 12.0f;
};

/**
 * Immediate-mode debug drawing for the sea grid.
 *
 * Static helpers rather than a component: drawing has no state worth owning, and any actor,
 * cheat manager or automation hook should be able to visualise a grid it happens to have a
 * reference to. Everything compiles out in shipping builds along with ENABLE_DRAW_DEBUG.
 */
struct NAVALNAV_API FSeaGridDebugDraw
{
	/** Maps a cell cost to a colour: blue open water through yellow to red, black for land. */
	static FColor CostToColor(float Cost, float CostColorScale);

	/**
	 * Draws the cost field around a viewpoint.
	 *
	 * @param Duration How long the primitives live. Pass the frame delta when calling per tick.
	 */
	static void DrawGrid(const UWorld* World, const FSeaGridData& Grid, const FVector& ViewLocation,
		const FSeaGridDebugDrawSettings& Settings, bool bDrawCostLabels, float Duration);

	/** Draws a route as a numbered polyline, labelled with the accumulated cost per waypoint. */
	static void DrawPath(const UWorld* World, const FNavalPath& Path, const FSeaGridDebugDrawSettings& Settings,
		const FColor& Color, bool bLabelWaypoints, float Duration);

	/** Draws a danger zone as an outer ring plus its lethal core. */
	static void DrawDangerZone(const UWorld* World, const FVector& Center, float Radius, float LethalRadiusFraction,
		const FColor& Color, float Duration);

	/** State of the naval.DrawGrid console variable. */
	static bool IsGridDrawEnabled();

	/** State of the naval.DrawCosts console variable. */
	static bool IsCostDrawEnabled();

	/** State of the naval.DrawPath console variable. */
	static bool IsPathDrawEnabled();
};
