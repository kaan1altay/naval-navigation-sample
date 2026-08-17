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
#include "NavalNav.h"
#include "Navigation/NavalNavigatorComponent.h"
#include "Ship/SailingShipPawn.h"

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

	MappingContext = NewObject<UInputMappingContext>(this, TEXT("DemoMappingContext"));
	MappingContext->MapKey(ClickAction, EKeys::LeftMouseButton);
	MappingContext->MapKey(CycleAction, EKeys::Tab);
	MappingContext->MapKey(NavDebugAction, EKeys::One);
	MappingContext->MapKey(ShipDebugAction, EKeys::Two);
	MappingContext->MapKey(GridDebugAction, EKeys::Three);
	MappingContext->MapKey(ZoomAction, EKeys::MouseWheelAxis);
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
			Navigator->RequestMoveTo(Target);
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

void ANavalNavDemoPlayerController::ToggleCVar(const TCHAR* Name, const TCHAR* Label)
{
	if (IConsoleVariable* CVar = IConsoleManager::Get().FindConsoleVariable(Name))
	{
		const int32 NewValue = CVar->GetInt() != 0 ? 0 : 1;
		CVar->Set(NewValue);
		UE_LOG(LogNavalNav, Log, TEXT("%s: %s"), Label, NewValue ? TEXT("ON") : TEXT("OFF"));
	}
}
