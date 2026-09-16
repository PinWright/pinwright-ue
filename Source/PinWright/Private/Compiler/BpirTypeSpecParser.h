// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once
#include "CoreMinimal.h"
#include "Compiler/BpirTypeSpec.h"

namespace BpirTypeSpecParser
{
    // Parses a single type substring (already extracted from surrounding
    // context — e.g. the portion before a param name, or the part after
    // a function's -> arrow). Returns true on success.
    // On failure: OutError is a human-readable message, OutErrorColumn
    // is the 0-based byte offset into Source where the parse failed
    // (or INDEX_NONE if not locatable).
    PINWRIGHT_API bool ParseTypeSpec(
        const FString& Source,
        FBpirTypeSpec& OutSpec,
        FString& OutError,
        int32& OutErrorColumn);

    // Canonical emission (round-trip pair with ParseTypeSpec).
    PINWRIGHT_API FString TypeSpecToBpirText(const FBpirTypeSpec& Spec);

    // Formats a parser-error tuple into a human-readable detail suffix.
    // Returns "<ParseError> (col N)" when the column is set (>= 0); otherwise just "<ParseError>".
    // Used by producer sites to append a column hint to their own "Invalid <kind> type '<src>': "
    // prefixes without inlining the INDEX_NONE check at each site.
    PINWRIGHT_API FString FormatTypeSpecErrorDetail(const FString& ParseError, int32 ParseErrCol);
}
