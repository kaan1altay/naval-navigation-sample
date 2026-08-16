// Copyright (c) 2026 Kaan Altay. Licensed under the MIT License.
//
// Shim for Unreal's Algo/Reverse.h. Development aid only; see CoreMinimal.h in this folder.

#pragma once

#include "CoreMinimal.h"

namespace Algo
{
	template <typename ContainerType>
	void Reverse(ContainerType& Container)
	{
		for (int32 Low = 0, High = Container.Num() - 1; Low < High; ++Low, --High)
		{
			Swap(Container[Low], Container[High]);
		}
	}
}
