// Copyright (c) 2026 Kaan Altay. Licensed under the MIT License.

#include "Ship/WindSubsystem.h"

#include "NavalNav.h"
#include "Ship/SailingModel.h"

namespace
{
	// Sentinels well outside the valid range mean "no override": the subsystem's own values win
	// unless someone has actually set the cvar. A yaw is a bearing, so -1000 is safely unreachable.
	constexpr float WindYawUnset = -1000.0f;
	constexpr float WindStrengthUnset = -1.0f;

	TAutoConsoleVariable<float> CVarWindYaw(
		TEXT("naval.Wind.Yaw"),
		WindYawUnset,
		TEXT("Override global wind direction (yaw degrees, the way the wind blows TOWARD).\n")
		TEXT("Any value <= -1000 leaves the world's own setting in charge."),
		ECVF_Cheat);

	TAutoConsoleVariable<float> CVarWindStrength(
		TEXT("naval.Wind.Strength"),
		WindStrengthUnset,
		TEXT("Override global wind strength, 0..1. A negative value leaves the world's own setting in charge."),
		ECVF_Cheat);
}

bool UWindSubsystem::DoesSupportWorldType(const EWorldType::Type WorldType) const
{
	// Editor worlds included so wind drift and the ship's debug arrows work in a viewport without PIE.
	return WorldType == EWorldType::Game
		|| WorldType == EWorldType::PIE
		|| WorldType == EWorldType::Editor;
}

void UWindSubsystem::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

	if (bDriftDirection && DeltaTime > 0.0f)
	{
		WindDirectionYawDegrees = FSailingModel::NormalizeDegrees(WindDirectionYawDegrees + DriftDegPerSec * DeltaTime);
	}
}

TStatId UWindSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UWindSubsystem, STATGROUP_Tickables);
}

float UWindSubsystem::GetWindDirectionYaw() const
{
	const float Override = CVarWindYaw.GetValueOnGameThread();
	return Override > WindYawUnset ? Override : WindDirectionYawDegrees;
}

float UWindSubsystem::GetWindFromYaw() const
{
	return FSailingModel::NormalizeDegrees(GetWindDirectionYaw() + 180.0f);
}

float UWindSubsystem::GetWindStrength() const
{
	const float Override = CVarWindStrength.GetValueOnGameThread();
	const float Value = Override >= 0.0f ? Override : WindStrength;
	return FMath::Clamp(Value, 0.0f, 1.0f);
}

FVector UWindSubsystem::GetWindDirection() const
{
	const float Rad = FMath::DegreesToRadians(GetWindDirectionYaw());
	return FVector(FMath::Cos(Rad), FMath::Sin(Rad), 0.0);
}

void UWindSubsystem::SetWindDirectionYaw(float NewYawDegrees)
{
	WindDirectionYawDegrees = FSailingModel::NormalizeDegrees(NewYawDegrees);
}

void UWindSubsystem::SetWindStrength(float NewStrength)
{
	WindStrength = FMath::Clamp(NewStrength, 0.0f, 1.0f);
}

void UWindSubsystem::AddWindYaw(float DeltaDegrees)
{
	// Fold in any active console override, then clear it so this manual control now wins.
	WindDirectionYawDegrees = FSailingModel::NormalizeDegrees(GetWindDirectionYaw() + DeltaDegrees);
	CVarWindYaw->Set(*FString::SanitizeFloat(WindYawUnset), ECVF_SetByConsole);
	UE_LOG(LogNavalNav, Log, TEXT("Wind direction: %.0f deg (toward)"), WindDirectionYawDegrees);
}

void UWindSubsystem::AddWindStrength(float Delta)
{
	WindStrength = FMath::Clamp(GetWindStrength() + Delta, 0.0f, 1.0f);
	CVarWindStrength->Set(*FString::SanitizeFloat(WindStrengthUnset), ECVF_SetByConsole);
	UE_LOG(LogNavalNav, Log, TEXT("Wind strength: %.0f%%"), WindStrength * 100.0f);
}
