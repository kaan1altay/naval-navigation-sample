// Copyright (c) 2026 Kaan Altay. Licensed under the MIT License.

#include "Demo/NavalNavDemoHUD.h"

#include "CanvasItem.h"
#include "Demo/NavalNavDemoGameMode.h"
#include "Engine/Canvas.h"
#include "Engine/Engine.h"
#include "Ship/WindSubsystem.h"

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
	const FString KeyMap = TEXT("L-click move   Tab cycle ship   1 nav / 2 ship / 3 grid   5-9 scenarios   P weaken   arrows: wind   wheel: zoom");

	// --- Backing box + text -------------------------------------------------------------------
	const float Margin = 24.0f;
	const float BoxWidth = FMath::Min(Canvas->SizeX - 2.0f * Margin, 1200.0f);
	const float BoxHeight = 128.0f;

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
	DrawText(WindLine, MediumFont, Margin + 16.0f, Margin + 54.0f, FLinearColor(0.70f, 0.86f, 1.0f), 1.1f);
	DrawText(KeyMap, MediumFont, Margin + 16.0f, Margin + 86.0f, FLinearColor(0.78f, 0.82f, 0.88f), 1.0f);

	// --- Wind compass, top-right: an arrow pointing where the wind blows TO --------------------
	const FVector2D Centre(Canvas->SizeX - 110.0f, 110.0f);
	const float Angle = FMath::DegreesToRadians(WindTowardYaw);
	// World yaw 0 is +X (east); screen Y runs down, so negate the Y component to keep north up.
	const FVector2D Dir(FMath::Cos(Angle), -FMath::Sin(Angle));
	const FVector2D Perp(-Dir.Y, Dir.X);
	const float Length = 50.0f + 40.0f * WindStrength;
	const FVector2D Tip = Centre + Dir * Length;
	const FVector2D Tail = Centre - Dir * Length;
	const FLinearColor WindColour(0.55f, 0.82f, 1.0f);
	const float Thickness = 6.0f;

	DrawLine(Tail.X, Tail.Y, Tip.X, Tip.Y, WindColour, Thickness);
	const FVector2D HeadBase = Tip - Dir * 28.0f;
	DrawLine(Tip.X, Tip.Y, (HeadBase + Perp * 20.0f).X, (HeadBase + Perp * 20.0f).Y, WindColour, Thickness);
	DrawLine(Tip.X, Tip.Y, (HeadBase - Perp * 20.0f).X, (HeadBase - Perp * 20.0f).Y, WindColour, Thickness);
	// Label the head so there is no doubt which end is which.
	DrawText(TEXT("W"), MediumFont, Tip.X + 8.0f, Tip.Y - 8.0f, WindColour, 1.1f);
}
