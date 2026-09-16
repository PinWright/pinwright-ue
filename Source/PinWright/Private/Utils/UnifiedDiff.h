// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once
#include "CoreMinimal.h"

namespace UnifiedDiff
{
    PINWRIGHT_API FString MakeUnifiedDiff(
        const FString& Old,
        const FString& New,
        const FString& OldLabel = TEXT("old"),
        const FString& NewLabel = TEXT("new"),
        int32 ContextLines = 3);
}
