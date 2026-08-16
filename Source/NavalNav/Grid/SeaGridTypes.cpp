// Copyright (c) 2026 Kaan Altay. Licensed under the MIT License.

#include "Grid/SeaGridTypes.h"

void FSeaGridData::Rebuild(const FSeaGridConfig& InConfig)
{
	Config = InConfig;
	Config.CellSize = FMath::Max(Config.CellSize, 1.0f);
	Config.Extent.X = FMath::Max(Config.Extent.X, static_cast<double>(Config.CellSize));
	Config.Extent.Y = FMath::Max(Config.Extent.Y, static_cast<double>(Config.CellSize));

	NumCellsX = FMath::Max(1, FMath::CeilToInt32(2.0 * Config.Extent.X / Config.CellSize));
	NumCellsY = FMath::Max(1, FMath::CeilToInt32(2.0 * Config.Extent.Y / Config.CellSize));

	// Guard against an authoring mistake allocating an absurd amount of memory. Growing the
	// cell size (rather than cropping the area) keeps the covered world region intact, which
	// is almost always what the author meant.
	if (static_cast<int64>(NumCellsX) * static_cast<int64>(NumCellsY) > NavalNav::MaxGridCells)
	{
		const double Scale = FMath::Sqrt(static_cast<double>(NumCellsX) * static_cast<double>(NumCellsY) / NavalNav::MaxGridCells);
		Config.CellSize = static_cast<float>(Config.CellSize * Scale);
		NumCellsX = FMath::Max(1, FMath::CeilToInt32(2.0 * Config.Extent.X / Config.CellSize));
		NumCellsY = FMath::Max(1, FMath::CeilToInt32(2.0 * Config.Extent.Y / Config.CellSize));
	}

	const int32 NumCells = NumCellsX * NumCellsY;
	BaseCost.Reset();
	BaseCost.Init(NavalNav::OpenWaterCost, NumCells);
	ThreatCost.Reset();
	ThreatCost.Init(0.0f, NumCells);
}

void FSeaGridData::Reset()
{
	NumCellsX = 0;
	NumCellsY = 0;
	BaseCost.Empty();
	ThreatCost.Empty();
}

FIntPoint FSeaGridData::WorldToCell(const FVector& WorldLocation) const
{
	const double OriginX = Config.Center.X - Config.Extent.X;
	const double OriginY = Config.Center.Y - Config.Extent.Y;

	return FIntPoint(
		FMath::FloorToInt32((WorldLocation.X - OriginX) / Config.CellSize),
		FMath::FloorToInt32((WorldLocation.Y - OriginY) / Config.CellSize));
}

FVector FSeaGridData::CellToWorld(const FIntPoint& Cell) const
{
	const double OriginX = Config.Center.X - Config.Extent.X;
	const double OriginY = Config.Center.Y - Config.Extent.Y;

	return FVector(
		OriginX + (Cell.X + 0.5) * Config.CellSize,
		OriginY + (Cell.Y + 0.5) * Config.CellSize,
		Config.SeaLevelZ);
}

FIntPoint FSeaGridData::ClampCell(const FIntPoint& Cell) const
{
	return FIntPoint(
		FMath::Clamp(Cell.X, 0, FMath::Max(0, NumCellsX - 1)),
		FMath::Clamp(Cell.Y, 0, FMath::Max(0, NumCellsY - 1)));
}

float FSeaGridData::GetBaseCost(const FIntPoint& Cell) const
{
	if (!IsValidCell(Cell))
	{
		return NavalNav::ImpassableCost;
	}
	return BaseCost[CellToIndex(Cell)];
}

float FSeaGridData::GetThreatCost(const FIntPoint& Cell) const
{
	if (!IsValidCell(Cell))
	{
		return 0.0f;
	}
	return ThreatCost[CellToIndex(Cell)];
}

float FSeaGridData::GetCellCost(const FIntPoint& Cell) const
{
	if (!IsValidCell(Cell))
	{
		// Outside the navigable area is as good as land: the planner will refuse to leave.
		return NavalNav::ImpassableCost;
	}

	const int32 Index = CellToIndex(Cell);
	const float Base = BaseCost[Index];
	if (Base >= NavalNav::ImpassableCost)
	{
		return NavalNav::ImpassableCost;
	}

	return Base + ThreatCost[Index];
}

void FSeaGridData::SetBaseCost(const FIntPoint& Cell, float Cost)
{
	if (IsValidCell(Cell))
	{
		BaseCost[CellToIndex(Cell)] = Cost;
	}
}

void FSeaGridData::SetThreatCost(const FIntPoint& Cell, float Cost)
{
	if (IsValidCell(Cell))
	{
		ThreatCost[CellToIndex(Cell)] = Cost;
	}
}

void FSeaGridData::AddThreatCost(const FIntPoint& Cell, float Cost)
{
	if (IsValidCell(Cell))
	{
		ThreatCost[CellToIndex(Cell)] += Cost;
	}
}

void FSeaGridData::ClearThreat()
{
	// Assign over the existing allocation; the threat layer is cleared every time a zone
	// moves, so this runs often enough that reallocating would show up in a profile.
	for (float& Value : ThreatCost)
	{
		Value = 0.0f;
	}
}

void FSeaGridData::ClearThreatInRadius(const FVector& WorldCenter, float Radius)
{
	ForEachCellInRadius(WorldCenter, Radius, [this](const FIntPoint& Cell, float /*Distance*/)
	{
		ThreatCost[CellToIndex(Cell)] = 0.0f;
	});
}

void FSeaGridData::ForEachCell(TFunctionRef<void(const FIntPoint&, float)> Visitor) const
{
	for (int32 Y = 0; Y < NumCellsY; ++Y)
	{
		for (int32 X = 0; X < NumCellsX; ++X)
		{
			const FIntPoint Cell(X, Y);
			Visitor(Cell, GetCellCost(Cell));
		}
	}
}

void FSeaGridData::ForEachCellInRadius(const FVector& WorldCenter, float Radius, TFunctionRef<void(const FIntPoint&, float)> Visitor) const
{
	if (!IsBuilt() || Radius <= 0.0f)
	{
		return;
	}

	// Convert the disc to a cell-space bounding box first, then reject the corners by distance.
	const FIntPoint MinCell = ClampCell(WorldToCell(WorldCenter - FVector(Radius, Radius, 0.0)));
	const FIntPoint MaxCell = ClampCell(WorldToCell(WorldCenter + FVector(Radius, Radius, 0.0)));

	for (int32 Y = MinCell.Y; Y <= MaxCell.Y; ++Y)
	{
		for (int32 X = MinCell.X; X <= MaxCell.X; ++X)
		{
			const FIntPoint Cell(X, Y);
			const float Distance = static_cast<float>(FVector::Dist2D(CellToWorld(Cell), WorldCenter));
			if (Distance <= Radius)
			{
				Visitor(Cell, Distance);
			}
		}
	}
}
