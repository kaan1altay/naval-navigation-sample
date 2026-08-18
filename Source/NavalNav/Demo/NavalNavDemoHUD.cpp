// Copyright (c) 2026 Kaan Altay. Licensed under the MIT License.

#include "Demo/NavalNavDemoHUD.h"

#include "CanvasItem.h"
#include "Demo/NavalNavDemoGameMode.h"
#include "Engine/Canvas.h"
#include "Engine/Engine.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"
#include "Navigation/NavalNavigatorComponent.h"
#include "Ship/SailingShipPawn.h"
#include "Ship/ShipPowerComponent.h"
#include "Ship/WindSubsystem.h"

namespace
{
	bool IsCVarOn(const TCHAR* Name)
	{
		const IConsoleVariable* CVar = IConsoleManager::Get().FindConsoleVariable(Name);
		return CVar && CVar->GetInt() != 0;
	}
}

void ANavalNavDemoHUD::DrawHUD()
{
	Super::DrawHUD();

	if (!Canvas || !GEngine)
	{
		return;
	}

	UFont* LargeFont = GEngine->GetLargeFont();
	UFont* MediumFont = GEngine->GetMediumFont();

	FString Title;
	if (const ANavalNavDemoGameMode* GameMode = GetWorld()->GetAuthGameMode<ANavalNavDemoGameMode>())
	{
		Title = GameMode->GetScenarioTitle();
	}

	float WindTowardYaw = 0.0f;
	float WindStrength = 0.0f;
	if (const UWindSubsystem* Wind = GetWorld()->GetSubsystem<UWindSubsystem>())
	{
		WindTowardYaw = Wind->GetWindDirectionYaw();
		WindStrength = Wind->GetWindStrength();
	}
	const FString WindLine = FString::Printf(TEXT("Wind  %.0f deg toward   |   strength %.0f%%"), WindTowardYaw, WindStrength * 100.0f);

	// The selected (possessed) ship's power, so P/O have visible feedback.
	FString ShipLine = TEXT("Selected ship: (none)");
	if (PlayerOwner)
	{
		if (const ASailingShipPawn* Ship = Cast<ASailingShipPawn>(PlayerOwner->GetPawn()))
		{
			const float Power = Ship->GetPowerComponent() ? Ship->GetPowerComponent()->GetPowerLevel() : 0.0f;
			ShipLine = FString::Printf(TEXT("Selected ship: %s   |   power %.1f   (O stronger / P weaker)"), *Ship->GetName(), Power);
		}
	}

	const FString KeyMap = TEXT("L-click move   Tab cycle ship   1 nav / 2 ship / 3 grid   5-9 scenarios   O/P power   arrows: wind   wheel: zoom");

	// --- Backing box + text -------------------------------------------------------------------
	const float Margin = 24.0f;
	const float BoxWidth = FMath::Min(Canvas->SizeX - 2.0f * Margin, 1200.0f);
	const float BoxHeight = 158.0f;

	FCanvasTileItem Box(FVector2D(Margin, Margin), FVector2D(BoxWidth, BoxHeight), FLinearColor(0.0f, 0.0f, 0.0f, 0.55f));
	Box.BlendMode = SE_BLEND_Translucent;
	Canvas->DrawItem(Box);

	auto DrawText = [&](const FString& Text, UFont* Font, float X, float Y, const FLinearColor& Colour, float Scale)
	{
		FCanvasTextItem Item(FVector2D(X, Y), FText::FromString(Text), Font, Colour);
		Item.Scale = FVector2D(Scale, Scale);
		Item.EnableShadow(FLinearColor::Black);
		Canvas->DrawItem(Item);
	};

	DrawText(Title, LargeFont, Margin + 16.0f, Margin + 12.0f, FLinearColor::White, 1.3f);
	DrawText(WindLine, MediumFont, Margin + 16.0f, Margin + 52.0f, FLinearColor(0.70f, 0.86f, 1.0f), 1.1f);
	DrawText(ShipLine, MediumFont, Margin + 16.0f, Margin + 82.0f, FLinearColor(1.0f, 0.92f, 0.6f), 1.1f);
	DrawText(KeyMap, MediumFont, Margin + 16.0f, Margin + 116.0f, FLinearColor(0.78f, 0.82f, 0.88f), 1.0f);

	// --- Selected-ship overlays, pinned to the screen edges below the scenario box -------------
	// A block of lines on a translucent panel; overlay 1 (navigator) hugs the right edge, overlay 2
	// (ship/wind) hugs the left. Each is shown only when its debug toggle (key 1 / key 2) is on.
	auto DrawBlock = [&](const FString& Block, float X, float Y, float Width, const FLinearColor& TextColour)
	{
		TArray<FString> Lines;
		Block.ParseIntoArray(Lines, TEXT("\n"), false);
		const float LineHeight = 26.0f;
		const float Pad = 10.0f;
		const float PanelHeight = Lines.Num() * LineHeight + 2.0f * Pad;

		FCanvasTileItem Panel(FVector2D(X, Y), FVector2D(Width, PanelHeight), FLinearColor(0.0f, 0.0f, 0.0f, 0.55f));
		Panel.BlendMode = SE_BLEND_Translucent;
		Canvas->DrawItem(Panel);
		for (int32 Index = 0; Index < Lines.Num(); ++Index)
		{
			DrawText(Lines[Index], MediumFont, X + Pad, Y + Pad + Index * LineHeight, TextColour, 1.1f);
		}
	};

	if (const ASailingShipPawn* Ship = PlayerOwner ? Cast<ASailingShipPawn>(PlayerOwner->GetPawn()) : nullptr)
	{
		const float BelowY = Margin + BoxHeight + 18.0f;
		const float PanelWidth = 480.0f;

		if (IsCVarOn(TEXT("naval.Nav.Debug")))
		{
			if (const UNavalNavigatorComponent* Nav = Ship->FindComponentByClass<UNavalNavigatorComponent>())
			{
				DrawBlock(Nav->GetStatusText(), Canvas->SizeX - Margin - PanelWidth, BelowY, PanelWidth, FLinearColor(1.0f, 0.95f, 0.55f));
			}
		}
		if (IsCVarOn(TEXT("naval.Ship.Debug")))
		{
			DrawBlock(Ship->GetStatusText(), Margin, BelowY, PanelWidth, FLinearColor(0.90f, 0.94f, 1.0f));
		}
	}
}
