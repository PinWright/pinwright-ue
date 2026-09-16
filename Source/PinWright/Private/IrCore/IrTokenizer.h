// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "IrCore/IrToken.h"

class IIrGrammar;

class PINWRIGHT_API FIrTokenizer
{
public:
    static TArray<FIrToken> Tokenize(const FString& Line, const IIrGrammar& Grammar);
    static bool IsKeyword(const FString& Word, const IIrGrammar& Grammar);
};
