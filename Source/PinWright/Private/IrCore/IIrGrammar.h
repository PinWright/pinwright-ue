// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"

class IIrGrammar
{
public:
    virtual ~IIrGrammar() = default;

    virtual bool IsKeyword(const FString& Word) const = 0;
    virtual bool TryGetOpcode(const FString& Keyword, int32& OutOpcode) const = 0;
};
