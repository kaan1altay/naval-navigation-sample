// Copyright (c) 2026 Kaan Altay. Licensed under the MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "Ship/SailingModel.h"

#include "SailingShipConfig.generated.h"

/**
 * A named bundle of sailing tunables.
 *
 * Optional sugar: a ship can carry its parameters inline, but pulling them into a data asset lets
 * a "sloop" and a "galleon" be authored once and shared across many pawns, and re-tuned without
 * reopening a level. When a pawn references one of these, it wins over the pawn's inline values.
 */
UCLASS(BlueprintType)
class NAVALNAV_API USailingShipConfig : public UDataAsset
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sailing")
	FSailingModelParams Params;
};
