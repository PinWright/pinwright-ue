// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Containers/StringView.h"

#include "PwDiagnostic.h"
#include "PwToken.h"

// Always total: every input yields a token array terminated by EndOfFile, and a lexical
// error emits an Unknown token spanning the offending run rather than aborting the scan.
class PINWRIGHT_API FPwTokenizer
{
public:
    // Returns true when no Error-severity diagnostic was emitted.
    static bool Tokenize(FStringView Source, TArray<FPwToken>& OutTokens,
                         TArray<FPwDiagnostic>& OutDiagnostics);
};
