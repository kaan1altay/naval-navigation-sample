// Copyright (c) 2026 Kaan Altay. Licensed under the MIT License.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/HUD.h"

#include "NavalNavDemoHUD.generated.h"

/**
 * The demo's on-screen text, drawn to the Canvas rather than through AddOnScreenDebugMessage so it
 * is large, legible on a recording, and sits on a semi-transparent backing box. Shows the current
 * scenario, the live wind, and the key map. Purely presentational.
 */
UCLASS()
class NAVALNAV_API ANavalNavDemoHUD : public AHUD
{
	GENERATED_BODY()

public:
	//~ Begin AHUD interface
	virtual void DrawHUD() override;
	//~ End AHUD interface

	/** Shows a short centred message for a couple of seconds (e.g. a rejected click). */
	void ShowTransientMessage(const FString& Message);

private:
	FString TransientMessage;
	float TransientMessageExpiry = 0.0f;
};
