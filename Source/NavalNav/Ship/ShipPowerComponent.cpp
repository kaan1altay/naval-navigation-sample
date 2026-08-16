// Copyright (c) 2026 Kaan Altay. Licensed under the MIT License.

#include "Ship/ShipPowerComponent.h"

UShipPowerComponent::UShipPowerComponent()
{
	// Nothing to do per frame; power changes are event-driven.
	PrimaryComponentTick.bCanEverTick = false;
}

void UShipPowerComponent::BeginPlay()
{
	Super::BeginPlay();

	// Announce the starting value so listeners that bound before BeginPlay start from the truth
	// rather than from whatever they assumed. OldPower == NewPower marks it as the initial state.
	OnPowerChanged.Broadcast(PowerLevel, PowerLevel);
}

void UShipPowerComponent::SetPowerLevel(float NewPowerLevel)
{
	const float Clamped = FMath::Max(0.0f, NewPowerLevel);
	if (FMath::IsNearlyEqual(Clamped, PowerLevel))
	{
		// No real change: staying silent keeps a per-frame damage trickle from spamming replans.
		return;
	}

	const float OldPower = PowerLevel;
	PowerLevel = Clamped;
	OnPowerChanged.Broadcast(OldPower, PowerLevel);
}
