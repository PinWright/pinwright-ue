// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"

struct FPwDocumentHeader
{
    FString FormatKeyword; // "pwmodel" | "pwanim" -- as matched. Empty when unparsed.
    int32 Version = -1; // -1 when missing or unparsable
    int32 VersionLine = 0; // 1-based; 0 when not set
    int32 VersionColumn = 0;
};

// Syntax only. The core never resolves, normalises or sniffs Path. A .pwmodel `use`
// names a file relative to the document; a .pwanim `use skeleton` names a /Game/...
// object path. Only the format knows which.
struct FPwUse
{
    FString Kind;
    FString Path; // exactly as written, escapes decoded, nothing else
    int32 Line = 0;
    int32 Column = 0;
};
