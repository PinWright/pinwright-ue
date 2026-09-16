// Copyright (c) 2026 Alexander Penkin. MIT License.

// BlueprintEnumHelpers.h - Shared enum-pin literal/value helpers used by Blueprint handlers.
// Extracted from BlueprintGraphHandler.cpp and BlueprintHandlerUtils.cpp to avoid
// duplicate definitions when those translation units are merged by a unity build.
#pragma once


#include "CoreMinimal.h"

class UEnum;
class UEdGraphPin;
class UEdGraphSchema;

namespace BlueprintEnumHelpers
{
    // Returns true if the supplied enum entry name is the synthetic "_MAX" sentinel
    // that UE appends to every UENUM.
    inline bool IsEnumMaxEntryName(const FString& EntryName)
    {
        return EntryName.EndsWith(TEXT("_MAX"), ESearchCase::IgnoreCase);
    }

    // Strips any "Scope::" prefix from a fully-qualified enum entry name, returning the
    // bare entry identifier.
    FString StripEnumScope(const FString& InName);

    // Attempts to resolve a free-form literal (numeric, short name, fully-qualified,
    // authored, or display name) against the supplied UEnum. Writes the numeric value
    // on success.
    bool TryResolveEnumLiteralToValue(const UEnum* EnumType, const FString& InLiteral, int64& OutValue);

    // Attempts to set an enum-typed pin's default to RequestedValue using the supplied
    // schema, trying multiple candidate literal forms. Writes the literal that stuck
    // (OutAppliedLiteral) or an error code/message on failure.
    bool TryApplyEnumPinDefaultValue(
        const UEdGraphSchema* Schema,
        UEdGraphPin* Pin,
        const FString& RequestedValue,
        FString& OutAppliedLiteral,
        FString& OutErrorCode,
        FString& OutErrorMessage);
}
