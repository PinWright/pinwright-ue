// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "MGIR/MGIROpcodes.h"

class FMGIRTypeGrammar
{
public:
    static bool TryParseType(const FString& Text, EMGIRValueType& OutType);
    static FString TypeToText(EMGIRValueType Type);
    static bool IsKnownType(const FString& Text);
};
