// Copyright (c) 2026 Kaan Altay. Licensed under the MIT License.

#include "Grid/SeaGridDebugDraw.h"

#include "DrawDebugHelpers.h"
#include "Engine/World.h"
#include "HAL/IConsoleManager.h"

namespace
{
	TAutoConsoleVariable<int32> CVarNavalDrawGrid(
		TEXT("naval.DrawGrid"),
		1,
		TEXT("Draw the sea grid cost field around the viewpoint. 0 = off, 1 = on."),
		ECVF_Cheat);

	TAutoConsoleVariable<int32> CVarNavalDrawCosts(
		TEXT("naval.DrawCosts"),
		0,
		TEXT("Draw numeric per-cell costs on top of the grid overlay. Off by default: the text is\n")
		TEXT("an order of magnitude more expensive than the boxes."),
		ECVF_Cheat);

	TAutoConsoleVariable<int32> CVarNavalDrawPath(
		TEXT("naval.DrawPath"),
		1,
		TEXT("Draw the planned route with numbered waypoints. 0 = off, 1 = on."),
		ECVF_Cheat);
}

bool FSeaGridDebugDraw::IsGridDrawEnabled()
{
	return CVarNavalDrawGrid.GetValueOnGameThread() != 0;
}

bool FSeaGridDebugDraw::IsCostDrawEnabled()
{
	return CVarNavalDrawCosts.GetValueOnGameThread() != 0;
}

bool FSeaGridDebugDraw::IsPathDrawEnabled()
{
	return CVarNavalDrawPath.GetValueOnGameThread() != 0;
}

FColor FSeaGridDebugDraw::CostToColor(float Cost, float CostColorScale)
{
	if (Cost >= NavalNav::ImpassableCost)
	{
		// Land and lethal cores read as solid black, so they are never mistaken for "expensive".
		return FColor(10, 10, 10);
	}

	// Open water sits at the bottom of the ramp; anything above it is threat.
	const float Excess = FMath::Max(0.0f, Cost - NavalNav::OpenWaterCost);
	const float Alpha = FMath::Clamp(Excess / FMath::Max(CostColorScale, UE_KINDA_SMALL_NUMBER), 0.0f, 1.0f);

	// Blue -> green -> yellow -> red, which is the ramp people already read as "danger".
	static const FLinearColor Ramp[] =
	{
		FLinearColor(0.05f, 0.20f, 0.60f),
		FLinearColor(0.10f, 0.65f, 0.35f),
		FLinearColor(0.90f, 0.80f, 0.10f),
		FLinearColor(0.85f, 0.10f, 0.05f)
	};
	constexpr int32 NumStops = UE_ARRAY_COUNT(Ramp);

	const float Scaled = Alpha * (NumStops - 1);
	const int32 Low = FMath::Clamp(FMath::FloorToInt32(Scaled), 0, NumStops - 2);
	const float Blend = Scaled - Low;

	return FLinearColor::LerpUsingHSV(Ramp[Low], Ramp[Low + 1], Blend).ToFColor(/*bSRGB=*/true);
}

void FSeaGridDebugDraw::DrawGrid(const UWorld* World, const FSeaGridData& Grid, const FVector& ViewLocation,
	const FSeaGridDebugDrawSettings& Settings, bool bDrawCostLabels, float Duration)
{
#if ENABLE_DRAW_DEBUG
	if (!World || !Grid.IsBuilt())
	{
		return;
	}

	const float HalfCell = Grid.GetCellSize() * 0.45f;
	const FVector BoxExtent(HalfCell, HalfCell, 1.0);
	const float MaxDistanceSquared = Settings.MaxDrawDistance * Settings.MaxDrawDistance;

	int32 DrawnCells = 0;
	int32 DrawnLabels = 0;

	Grid.ForEachCell([&](const FIntPoint& Cell, float Cost)
	{
		if (DrawnCells >= Settings.MaxDrawnCells)
		{
			return;
		}

		const bool bThreatened = (Cost > NavalNav::OpenWaterCost + UE_KINDA_SMALL_NUMBER);
		if (Settings.bOnlyDrawThreatenedCells && !bThreatened)
		{
			return;
		}

		FVector CellCenter = Grid.CellToWorld(Cell);
		if (FVector::DistSquared2D(CellCenter, ViewLocation) > MaxDistanceSquared)
		{
			return;
		}

		CellCenter.Z += Settings.DrawHeightOffset;
		const FColor Color = CostToColor(Cost, Settings.CostColorScale);
		DrawDebugBox(World, CellCenter, BoxExtent, Color, /*bPersistentLines=*/false, Duration, /*DepthPriority=*/0);
		++DrawnCells;

		// Labels are budgeted separately and drawn nearest-first by virtue of the distance
		// test above; past the budget the boxes still convey the shape of the field.
		if (bDrawCostLabels && DrawnLabels < Settings.MaxCostLabels && Cost < NavalNav::ImpassableCost)
		{
			DrawDebugString(World, CellCenter, FString::Printf(TEXT("%.1f"), Cost),
				/*TestBaseActor=*/nullptr, Color, Duration, /*bDrawShadow=*/false, /*FontScale=*/0.8f);
			++DrawnLabels;
		}
	});
#endif // ENABLE_DRAW_DEBUG
}

void FSeaGridDebugDraw::DrawPath(const UWorld* World, const FNavalPath& Path, const FSeaGridDebugDrawSettings& Settings,
	const FColor& Color, bool bLabelWaypoints, float Duration)
{
#if ENABLE_DRAW_DEBUG
	if (!World || !Path.bSuccess || Path.Num() == 0)
	{
		return;
	}

	const FVector HeightOffset(0.0, 0.0, Settings.DrawHeightOffset + 20.0);

	for (int32 Index = 0; Index < Path.Num(); ++Index)
	{
		const FVector Point = Path.Waypoints[Index] + HeightOffset;

		if (Index > 0)
		{
			DrawDebugLine(World, Path.Waypoints[Index - 1] + HeightOffset, Point, Color,
				/*bPersistentLines=*/false, Duration, /*DepthPriority=*/0, Settings.PathThickness);
		}

		// Endpoints are called out so a plan is readable even when it is one long leg.
		const bool bEndpoint = (Index == 0 || Index == Path.Num() - 1);
		DrawDebugSphere(World, Point, bEndpoint ? 180.0f : 90.0f, /*Segments=*/12,
			bEndpoint ? FColor::White : Color, /*bPersistentLines=*/false, Duration);

		if (bLabelWaypoints)
		{
			DrawDebugString(World, Point + FVector(0.0, 0.0, 120.0),
				FString::Printf(TEXT("%d  (%.1f)"), Index, Path.Costs.IsValidIndex(Index) ? Path.Costs[Index] : 0.0f),
				/*TestBaseActor=*/nullptr, FColor::White, Duration, /*bDrawShadow=*/true);
		}
	}
#endif // ENABLE_DRAW_DEBUG
}

void FSeaGridDebugDraw::DrawDangerZone(const UWorld* World, const FVector& Center, float Radius, float LethalRadiusFraction,
	const FColor& Color, float Duration)
{
#if ENABLE_DRAW_DEBUG
	if (!World || Radius <= 0.0f)
	{
		return;
	}

	// Flat circles rather than spheres: the planner is 2D, and drawing it in 2D keeps the
	// overlay honest about what the ship actually reasons over.
	DrawDebugCircle(World, Center, Radius, /*Segments=*/48, Color, /*bPersistentLines=*/false, Duration,
		/*DepthPriority=*/0, /*Thickness=*/8.0f, FVector(1.0, 0.0, 0.0), FVector(0.0, 1.0, 0.0), /*bDrawAxis=*/false);

	const float LethalRadius = Radius * FMath::Clamp(LethalRadiusFraction, 0.0f, 1.0f);
	if (LethalRadius > 0.0f)
	{
		DrawDebugCircle(World, Center, LethalRadius, /*Segments=*/32, FColor::Black, /*bPersistentLines=*/false, Duration,
			/*DepthPriority=*/0, /*Thickness=*/8.0f, FVector(1.0, 0.0, 0.0), FVector(0.0, 1.0, 0.0), /*bDrawAxis=*/false);
	}
#endif // ENABLE_DRAW_DEBUG
}
