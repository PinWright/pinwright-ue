// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "IrCore/IrTextUtils.h"

// Shared MGIR helpers extracted from per-translation-unit anonymous namespaces to
// avoid ODR collisions under unity builds.
namespace MGIRHelpers
{
    // MGIR string literals: ONE escape set, and these two functions are its only spelling.
    // The set is the shared IR one documented in docs/ir-authoring.md ("Grammar Target") and
    // implemented once in FIrTextUtils: `\\`, `\"`, `\n`, `\r`, `\t`, `\uNNNN`.
    //
    // They must stay exact inverses, because the decompiler writes every literal with the
    // encoder and the compiler reads every literal with the decoder. When they were not, a
    // decompiled Custom HLSL node's `Code: "a\nb"` recompiled into a node holding a literal
    // backslash-n: the compiler's local un-quoter reversed only `\"` and `\\`, so every
    // newline in the program was dropped, the HLSL collapsed onto one line, and the material
    // reported a successful compile while failing to translate
    // (B-mgir-custom-node-inputs-and-newlines-lost).
    inline FString EncodeStringLiteral(const FString& Value)
    {
        return FIrTextUtils::Quote(Value);
    }

    // Inverse of EncodeStringLiteral. Text that is not a well-formed literal is returned
    // trimmed and otherwise unchanged: call sites hand this both real literals and bare
    // tokens (an already-unwrapped backtick name, an enum member, a number).
    inline FString DecodeStringLiteral(const FString& Value)
    {
        FString Decoded;
        FString Error;
        if (FIrTextUtils::TryUnwrapStringLiteral(Value, Decoded, Error))
        {
            return Decoded;
        }
        return Value.TrimStartAndEnd();
    }

    // Normalizes an MGIR symbol identifier for case-insensitive symbol-table lookup.
    inline FString NormalizeSymbolName(const FString& Name)
    {
        return Name.TrimStartAndEnd().ToLower();
    }

    // The symbol name the decompiler issues for an expression: "n" plus the first 12 hex
    // digits of the persisted MaterialExpressionGuid. Returns empty for an invalid guid,
    // where the decompiler falls back to a positional local id that identifies nothing.
    //
    // Shared because two callers must agree exactly: the decompiler that MINTS a handle and
    // the Extend-mode guard that RECOGNISES one. If they drift, the guard silently stops
    // matching and Extend goes back to duplicating the expression it was asked to update.
    inline FString ExpressionHandleName(const FGuid& ExpressionGuid)
    {
        return ExpressionGuid.IsValid()
            ? TEXT("n") + ExpressionGuid.ToString(EGuidFormats::Digits).Left(12)
            : FString();
    }
}
