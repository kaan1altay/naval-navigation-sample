// Copyright (c) 2026 Kaan Altay. Licensed under the MIT License.

#include "Ship/SailingShipPlayerController.h"

#include "EnhancedInputComponent.h"
#include "EnhancedInputSubsystems.h"
#include "InputAction.h"
#include "InputActionValue.h"
#include "InputCoreTypes.h"
#include "InputMappingContext.h"
#include "InputModifiers.h"
#include "InputTriggers.h"
#include "Ship/SailingShipPawn.h"

void ASailingShipPlayerController::EnsureInputAssets()
{
	if (RudderAction)
	{
		return;
	}

	// Two Axis1D actions, built in code. A/D drive the rudder, W/S the trim rate; the "A" and "S"
	// keys carry a Negate modifier so the single axis reads negative for them.
	RudderAction = NewObject<UInputAction>(this, TEXT("RudderAction"));
	RudderAction->ValueType = EInputActionValueType::Axis1D;

	TrimAction = NewObject<UInputAction>(this, TEXT("TrimAction"));
	TrimAction->ValueType = EInputActionValueType::Axis1D;

	MappingContext = NewObject<UInputMappingContext>(this, TEXT("SailingMappingContext"));

	MappingContext->MapKey(RudderAction, EKeys::D);
	MappingContext->MapKey(RudderAction, EKeys::A).Modifiers.Add(NewObject<UInputModifierNegate>(this));

	MappingContext->MapKey(TrimAction, EKeys::W);
	MappingContext->MapKey(TrimAction, EKeys::S).Modifiers.Add(NewObject<UInputModifierNegate>(this));
}

void ASailingShipPlayerController::BeginPlay()
{
	Super::BeginPlay();

	EnsureInputAssets();

	if (ULocalPlayer* LocalPlayer = GetLocalPlayer())
	{
		if (UEnhancedInputLocalPlayerSubsystem* Subsystem = LocalPlayer->GetSubsystem<UEnhancedInputLocalPlayerSubsystem>())
		{
			Subsystem->AddMappingContext(MappingContext, /*Priority=*/0);
		}
	}
}

void ASailingShipPlayerController::SetupInputComponent()
{
	Super::SetupInputComponent();

	EnsureInputAssets();

	if (UEnhancedInputComponent* EnhancedInput = Cast<UEnhancedInputComponent>(InputComponent))
	{
		EnhancedInput->BindAction(RudderAction, ETriggerEvent::Triggered, this, &ASailingShipPlayerController::OnRudder);
		EnhancedInput->BindAction(RudderAction, ETriggerEvent::Completed, this, &ASailingShipPlayerController::OnRudderReleased);
		EnhancedInput->BindAction(TrimAction, ETriggerEvent::Triggered, this, &ASailingShipPlayerController::OnTrim);
		EnhancedInput->BindAction(TrimAction, ETriggerEvent::Completed, this, &ASailingShipPlayerController::OnTrimReleased);
	}
}

void ASailingShipPlayerController::PlayerTick(float DeltaTime)
{
	Super::PlayerTick(DeltaTime);

	ASailingShipPawn* Ship = Cast<ASailingShipPawn>(GetPawn());
	if (!Ship)
	{
		return;
	}

	Ship->SetRudderInput(RudderAxis);

	// W/S nudge the trim up and down and it stays put, the way a sheet does — you set it and sail.
	CurrentTrim = FMath::Clamp(CurrentTrim + TrimAxis * TrimRatePerSec * DeltaTime, 0.0f, 1.0f);
	Ship->SetSailTrim(CurrentTrim);
}

void ASailingShipPlayerController::OnRudder(const FInputActionValue& Value)
{
	RudderAxis = FMath::Clamp(Value.Get<float>(), -1.0f, 1.0f);
}

void ASailingShipPlayerController::OnRudderReleased(const FInputActionValue& Value)
{
	// Helm returns amidships when the keys come up; the model still slews the rudder back smoothly.
	RudderAxis = 0.0f;
}

void ASailingShipPlayerController::OnTrim(const FInputActionValue& Value)
{
	TrimAxis = FMath::Clamp(Value.Get<float>(), -1.0f, 1.0f);
}

void ASailingShipPlayerController::OnTrimReleased(const FInputActionValue& Value)
{
	TrimAxis = 0.0f;
}
