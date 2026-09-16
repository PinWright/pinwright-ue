// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"

namespace PinWright
{
    inline FString NormalizeToken(FString Value)
    {
        Value.TrimStartAndEndInline();
        Value.ReplaceInline(TEXT("-"), TEXT("_"));
        return Value.ToLower();
    }

    // Case-sensitive count of non-overlapping occurrences of Needle in Text.
    inline int32 CountSubstring(const FString& Text, FStringView Needle)
    {
        if (Needle.IsEmpty())
        {
            return 0;
        }
        const FString NeedleStr(Needle);
        int32 Count = 0;
        int32 SearchStart = 0;
        while (true)
        {
            const int32 Found = Text.Find(NeedleStr, ESearchCase::CaseSensitive, ESearchDir::FromStart, SearchStart);
            if (Found == INDEX_NONE)
            {
                return Count;
            }
            ++Count;
            SearchStart = Found + NeedleStr.Len();
        }
    }
}
