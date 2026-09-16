// Copyright (c) 2026 Alexander Penkin. MIT License.

// CRIRControlValueParser.h
//
// Parses the typed-function-prefix value grammar emitted by CRIRTextEmitter's
// FormatControlValue. The grammar uses one of 11 barewords corresponding to
// ERigControlType, followed by `(...)`. Positional args for low-arity types
// (bool/float/int/vector2d/position/scale/scale_float), keyword args for
// compound types (rotator/transform/transform_no_scale/euler_transform).
//
// Validation rule: the prefix's ERigControlType must equal the caller's
// ExpectedType. Mismatches emit a distinct error string ("does not match
// type=...") so the compiler can promote it to CRIR_CONTROL_VALUE_TYPE_MISMATCH.

#pragma once

#include "CoreMinimal.h"
#include "Rigs/RigHierarchyDefines.h"

// Failure-cause discriminator returned by ParseWithCause so callers can pick a
// CRIR error code without substring sniffing the error message.
enum class ECRIRControlValueParseError : uint8
{
    None,
    PrefixTypeMismatch,
    Other,
};

class FCRIRControlValueParser
{
public:
    // Parses Literal into OutValue using ExpectedType to validate prefix and
    // route arg parsing. Returns false on any failure with a human-readable
    // OutError. The caller decides which error code to raise (the helper does
    // not couple to CRIR error vocabulary).
    static bool Parse(
        const FString& Literal,
        ERigControlType ExpectedType,
        FRigControlValue& OutValue,
        FString& OutError);

    // As Parse, but additionally classifies the failure cause so callers can
    // pick a typed error code (e.g. CRIR_CONTROL_VALUE_TYPE_MISMATCH vs
    // CRIR_CONTROL_BAD_VALUE) without substring-sniffing the error message.
    static bool ParseWithCause(
        const FString& Literal,
        ERigControlType ExpectedType,
        FRigControlValue& OutValue,
        FString& OutError,
        ECRIRControlValueParseError& OutCause);

    // Maps a value-prefix bareword to ERigControlType. 11 entries, one per
    // enum variant. Returns false on unknown prefix.
    static bool TryResolvePrefix(const FString& Prefix, ERigControlType& OutType);

    // Inverse of TryResolvePrefix. Single source of truth shared by the
    // compiler's type=<name> resolution and the emitter's type prefix render.
    static const TCHAR* PrefixForType(ERigControlType Type);

    // Parses `(a,b,...)` into ExpectedCount doubles. Depth-aware comma split so
    // nested tuples are tolerated. Used by element-line attributes
    // (location/rotation/scale/shape_color) and comment node size/color.
    static bool TryParseDoubleTuple(const FString& Tuple, int32 ExpectedCount, TArray<double>& Out);

    // Parses `(k=v,k=v,...)` (with optional outer parens) into a key->raw-value
    // map. Used by `limits` sub-block and rotator/transform value bodies.
    static bool TryParseKeyValueTuple(const FString& Tuple, TMap<FString, FString>& Out);
};
