// Copyright (c) 2026 Kaan Altay. Licensed under the MIT License.

#include "Demo/NavalNavDemoPlayerController.h"

#include "EnhancedInputComponent.h"
#include "EnhancedInputSubsystems.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "HAL/IConsoleManager.h"
#include "InputAction.h"
#include "InputActionValue.h"
#include "InputCoreTypes.h"
#include "InputMappingContext.h"
#include "InputTriggers.h"
#include "Demo/NavalNavDemoGameMode.h"
#include "NavalNav.h"
#include "Navigation/NavalNavigatorComponent.h"
#include "Ship/SailingShipPawn.h"
#include "Ship/ShipPowerComponent.h"

void ANavalNavDemoPlayerController::EnsureInputAssets()
{
	if (ClickAction)
	{
		return;
	}

	auto MakeAction = [this](const TCHAR* Name, EInputActionValueType Type)
	{
		UInputAction* Action = NewObject<UInputAction>(this, Name);
		Action->ValueType = Type;
		return Action;
	};

	ClickAction = MakeAction(TEXT("DemoClickAction"), EInputActionValueType::Boolean);
	CycleAction = MakeAction(TEXT("DemoCycleAction"), EInputActionValueType::Boolean);
	NavDebugAction = MakeAction(TEXT("DemoNavDebugAction"), EInputActionValueType::Boolean);
	ShipDebugAction = MakeAction(TEXT("DemoShipDebugAction"), EInputActionValueType::Boolean);
	GridDebugAction = MakeAction(TEXT("DemoGridDebugAction"), EInputActionValueType::Boolean);
	ZoomAction = MakeAction(TEXT("DemoZoomAction"), EInputActionValueType::Axis1D);
	WeakenAction = MakeAction(TEXT("DemoWeakenAction"), EInputActionValueType::Boolean);
	for (int32 i = 0; i < 5; ++i)
	{
		ScenarioActions[i] = MakeAction(*FString::Printf(TEXT("DemoScenario%dAction"), i + 5), EInputActionValueType::Boolean);
	}

	MappingContext = NewObject<UInputMappingContext>(this, TEXT("DemoMappingContext"));
	MappingContext->MapKey(ClickAction, EKeys::LeftMouseButton);
	MappingContext->MapKey(CycleAction, EKeys::Tab);
	MappingContext->MapKey(NavDebugAction, EKeys::One);
	MappingContext->MapKey(ShipDebugAction, EKeys::Two);
	MappingContext->MapKey(GridDebugAction, EKeys::Three);
	MappingContext->MapKey(ZoomAction, EKeys::MouseWheelAxis);
	MappingContext->MapKey(WeakenAction, EKeys::P);
	MappingContext->MapKey(ScenarioActions[0], EKeys::Five);
	MappingContext->MapKey(ScenarioActions[1], EKeys::Six);
	MappingContext->MapKey(ScenarioActions[2], EKeys::Seven);
	MappingContext->MapKey(ScenarioActions[3], EKeys::Eight);
	MappingContext->MapKey(ScenarioActions[4], EKeys::Nine);
}

void ANavalNavDemoPlayerController::BeginPlay()
{
	Super::BeginPlay();

	bShowMouseCursor = true;
	EnsureInputAssets();

	if (ULocalPlayer* LocalPlayer = GetLocalPlayer())
	{
		if (UEnhancedInputLocalPlayerSubsystem* Subsystem = LocalPlayer->GetSubsystem<UEnhancedInputLocalPlayerSubsystem>())
		{
			Subsystem->AddMappingContext(MappingContext, /*Priority=*/0);
		}
	}

	// Take the camera on the first ship in the level, if any.
	PossessShipAtIndex(0);
}

void ANavalNavDemoPlayerController::SetupInputComponent()
{
	Super::SetupInputComponent();

	EnsureInputAssets();

	if (UEnhancedInputComponent* EnhancedInput = Cast<UEnhancedInputComponent>(InputComponent))
	{
		EnhancedInput->BindAction(ClickAction, ETriggerEvent::Started, this, &ANavalNavDemoPlayerController::OnClickMove);
		EnhancedInput->BindAction(CycleAction, ETriggerEvent::Started, this, &ANavalNavDemoPlayerController::OnCycleShip);
		EnhancedInput->BindAction(NavDebugAction, ETriggerEvent::Started, this, &ANavalNavDemoPlayerController::OnToggleNavDebug);
		EnhancedInput->BindAction(ShipDebugAction, ETriggerEvent::Started, this, &ANavalNavDemoPlayerController::OnToggleShipDebug);
		EnhancedInput->BindAction(GridDebugAction, ETriggerEvent::Started, this, &ANavalNavDemoPlayerController::OnToggleGridDebug);
		EnhancedInput->BindAction(ZoomAction, ETriggerEvent::Triggered, this, &ANavalNavDemoPlayerController::OnZoom);
		EnhancedInput->BindAction(WeakenAction, ETriggerEvent::Started, this, &ANavalNavDemoPlayerController::OnWeakenShip);
		EnhancedInput->BindAction(ScenarioActions[0], ETriggerEvent::Started, this, &ANavalNavDemoPlayerController::OnScenario5);
		EnhancedInput->BindAction(ScenarioActions[1], ETriggerEvent::Started, this, &ANavalNavDemoPlayerController::OnScenario6);
		EnhancedInput->BindAction(ScenarioActions[2], ETriggerEvent::Started, this, &ANavalNavDemoPlayerController::OnScenario7);
		EnhancedInput->BindAction(ScenarioActions[3], ETriggerEvent::Started, this, &ANavalNavDemoPlayerController::OnScenario8);
		EnhancedInput->BindAction(ScenarioActions[4], ETriggerEvent::Started, this, &ANavalNavDemoPlayerController::OnScenario9);
	}
}

void ANavalNavDemoPlayerController::PossessShipAtIndex(int32 Index)
{
	// Rebuild the list each time so ships spawned after BeginPlay are picked up.
	Ships.Reset();
	if (UWorld* World = GetWorld())
	{
		for (TActorIterator<ASailingShipPawn> It(World); It; ++It)
		{
			Ships.Add(*It);
		}
	}

	if (Ships.Num() == 0)
	{
		return;
	}

	CurrentShipIndex = ((Index % Ships.Num()) + Ships.Num()) % Ships.Num();

	// Highlight only the ship we are commanding, so it is obvious which one your clicks steer.
	for (int32 ShipIndex = 0; ShipIndex < Ships.Num(); ++ShipIndex)
	{
		if (ASailingShipPawn* Ship = Ships[ShipIndex])
		{
			Ship->SetSelected(ShipIndex == CurrentShipIndex);
		}
	}

	if (ASailingShipPawn* Ship = Ships[CurrentShipIndex])
	{
		Possess(Ship);
	}
}

void ANavalNavDemoPlayerController::OnClickMove(const FInputActionValue& Value)
{
	FVector WorldOrigin;
	FVector WorldDirection;
	if (!DeprojectMousePositionToWorld(WorldOrigin, WorldDirection))
	{
		return;
	}

	// Intersect the view ray with the flat sea plane at SeaLevelZ; no floor collision needed.
	if (FMath::Abs(WorldDirection.Z) < UE_KINDA_SMALL_NUMBER)
	{
		return;
	}
	const double T = (SeaLevelZ - WorldOrigin.Z) / WorldDirection.Z;
	if (T <= 0.0)
	{
		return;
	}
	const FVector Target = WorldOrigin + WorldDirection * T;

	if (ASailingShipPawn* Ship = Cast<ASailingShipPawn>(GetPawn()))
	{
		if (UNavalNavigatorComponent* Navigator = Ship->FindComponentByClass<UNavalNavigatorComponent>())
		{
			// A player order takes the ship off auto-wander and always replaces the current goal.
			Navigator->RequestMoveTo(Target, /*bPlayerOrder=*/true);
		}
	}
}

void ANavalNavDemoPlayerController::OnCycleShip(const FInputActionValue& Value)
{
	PossessShipAtIndex(CurrentShipIndex + 1);
}

void ANavalNavDemoPlayerController::OnToggleNavDebug(const FInputActionValue& Value)
{
	ToggleCVar(TEXT("naval.Nav.Debug"), TEXT("Navigator overlay"));
}

void ANavalNavDemoPlayerController::OnToggleShipDebug(const FInputActionValue& Value)
{
	ToggleCVar(TEXT("naval.Ship.Debug"), TEXT("Ship overlay"));
}

void ANavalNavDemoPlayerController::OnToggleGridDebug(const FInputActionValue& Value)
{
	ToggleCVar(TEXT("naval.DrawGrid"), TEXT("Grid overlay"));
}

void ANavalNavDemoPlayerController::OnZoom(const FInputActionValue& Value)
{
	if (ASailingShipPawn* Ship = Cast<ASailingShipPawn>(GetPawn()))
	{
		// Wheel up (positive) pulls the camera in; wheel down pushes it out.
		Ship->AddCameraZoom(-Value.Get<float>() * 800.0f);
	}
}

void ANavalNavDemoPlayerController::OnScenario5(const FInputActionValue& Value) { StartScenario(5); }
void ANavalNavDemoPlayerController::OnScenario6(const FInputActionValue& Value) { StartScenario(6); }
void ANavalNavDemoPlayerController::OnScenario7(const FInputActionValue& Value) { StartScenario(7); }
void ANavalNavDemoPlayerController::OnScenario8(const FInputActionValue& Value) { StartScenario(8); }
void ANavalNavDemoPlayerController::OnScenario9(const FInputActionValue& Value) { StartScenario(9); }

void ANavalNavDemoPlayerController::StartScenario(int32 Index)
{
	if (ANavalNavDemoGameMode* GameMode = GetWorld() ? GetWorld()->GetAuthGameMode<ANavalNavDemoGameMode>() : nullptr)
	{
		GameMode->StartScenario(Index);
	}
}

void ANavalNavDemoPlayerController::OnWeakenShip(const FInputActionValue& Value)
{
	// Scenario 9's payoff: knock the selected ship's power down and let the navigator react.
	if (ASailingShipPawn* Ship = Cast<ASailingShipPawn>(GetPawn()))
	{
		if (UShipPowerComponent* Power = Ship->GetPowerComponent())
		{
			Power->SetPowerLevel(1.0f);
			UE_LOG(LogNavalNav, Log, TEXT("Weakened %s to power 1.0"), *Ship->GetName());
		}
	}
}

void ANavalNavDemoPlayerController::ToggleCVar(const TCHAR* Name, const TCHAR* Label)
{
	if (IConsoleVariable* CVar = IConsoleManager::Get().FindConsoleVariable(Name))
	{
		const int32 NewValue = CVar->GetInt() != 0 ? 0 : 1;
		CVar->Set(NewValue);
		UE_LOG(LogNavalNav, Log, TEXT("%s: %s"), Label, NewValue ? TEXT("ON") : TEXT("OFF"));
	}
}
