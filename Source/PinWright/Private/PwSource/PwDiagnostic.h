// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"

enum class EPwSeverity : uint8
{
    Error,
    Warning
};

struct FPwDiagnostic
{
    EPwSeverity Severity = EPwSeverity::Error;
    int32 Line = -1;
    int32 Column = -1;
    FString Code;
    FString Message;
    FString ScopeLabel;
    FString ScopeName;
    TArray<FString> Suggestions;

    FString ToString() const
    {
        FString Result;

        if (Severity == EPwSeverity::Warning)
        {
            Result += TEXT("warning: ");
        }

        if (!Code.IsEmpty())
        {
            Result += FString::Printf(TEXT("[%s] "), *Code);
        }

        if (Line >= 0 && Column >= 0)
        {
            Result += FString::Printf(TEXT("line %d, col %d: "), Line, Column);
        }
        else if (Line >= 0)
        {
            Result += FString::Printf(TEXT("line %d: "), Line);
        }

        Result += Message;

        if (!ScopeName.IsEmpty())
        {
            const TCHAR* Label = ScopeLabel.IsEmpty() ? TEXT("scope") : *ScopeLabel;
            Result += FString::Printf(TEXT(" (%s \"%s\")"), Label, *ScopeName);
        }

        if (Suggestions.Num() > 0)
        {
            Result += FString::Printf(TEXT(" [did you mean: %s]"), *FString::Join(Suggestions, TEXT(", ")));
        }

        return Result;
    }

    static FPwDiagnostic MakeError(const TCHAR* InCode, int32 InLine, int32 InColumn, FString InMessage)
    {
        FPwDiagnostic Diagnostic;
        Diagnostic.Severity = EPwSeverity::Error;
        Diagnostic.Code = InCode;
        Diagnostic.Line = InLine;
        Diagnostic.Column = InColumn;
        Diagnostic.Message = MoveTemp(InMessage);
        return Diagnostic;
    }

    static FPwDiagnostic MakeWarning(const TCHAR* InCode, int32 InLine, int32 InColumn, FString InMessage)
    {
        FPwDiagnostic Diagnostic = MakeError(InCode, InLine, InColumn, MoveTemp(InMessage));
        Diagnostic.Severity = EPwSeverity::Warning;
        return Diagnostic;
    }
};

inline bool PwDiagnosticsHaveError(const TArray<FPwDiagnostic>& Diagnostics)
{
    for (const FPwDiagnostic& Diagnostic : Diagnostics)
    {
        if (Diagnostic.Severity == EPwSeverity::Error)
        {
            return true;
        }
    }
    return false;
}

inline FString JoinPwDiagnostics(const TArray<FPwDiagnostic>& Diagnostics)
{
    TArray<FString> Lines;
    Lines.Reserve(Diagnostics.Num());
    for (const FPwDiagnostic& Diagnostic : Diagnostics)
    {
        Lines.Add(Diagnostic.ToString());
    }
    return FString::Join(Lines, TEXT("; "));
}

// Diagnostic codes shared by every PinWright source format. Format-specific
// registries stay with their format; lexical and structural source errors do
// not belong to .pwmodel merely because that was the first consumer.
namespace PwSourceDiagnosticCodes
{
    inline constexpr TCHAR PWSRC_UNEXPECTED_CHARACTER[] = TEXT("PWSRC_UNEXPECTED_CHARACTER");
    inline constexpr TCHAR PWSRC_UNTERMINATED_STRING[] = TEXT("PWSRC_UNTERMINATED_STRING");
    inline constexpr TCHAR PWSRC_INVALID_STRING[] = TEXT("PWSRC_INVALID_STRING");
    inline constexpr TCHAR PWSRC_INVALID_NUMBER[] = TEXT("PWSRC_INVALID_NUMBER");

    inline constexpr TCHAR PWSRC_MISSING_VERSION[] = TEXT("PWSRC_MISSING_VERSION");
    inline constexpr TCHAR PWSRC_UNSUPPORTED_VERSION[] = TEXT("PWSRC_UNSUPPORTED_VERSION");
    inline constexpr TCHAR PWSRC_UNEXPECTED_TOKEN[] = TEXT("PWSRC_UNEXPECTED_TOKEN");
    inline constexpr TCHAR PWSRC_UNCLOSED_BRACE[] = TEXT("PWSRC_UNCLOSED_BRACE");
    inline constexpr TCHAR PWSRC_UNKNOWN_OP[] = TEXT("PWSRC_UNKNOWN_OP");
    inline constexpr TCHAR PWSRC_UNKNOWN_PARAM[] = TEXT("PWSRC_UNKNOWN_PARAM");
    inline constexpr TCHAR PWSRC_DUPLICATE_PARAM[] = TEXT("PWSRC_DUPLICATE_PARAM");
    inline constexpr TCHAR PWSRC_MISSING_PARAM[] = TEXT("PWSRC_MISSING_PARAM");
    inline constexpr TCHAR PWSRC_BAD_TUPLE_ARITY[] = TEXT("PWSRC_BAD_TUPLE_ARITY");
    inline constexpr TCHAR PWSRC_BAD_VALUE[] = TEXT("PWSRC_BAD_VALUE");
    inline constexpr TCHAR PWSRC_BAD_BLOCK[] = TEXT("PWSRC_BAD_BLOCK");

    // A same-source recompile or takeover found serialized asset state that the incoming source
    // does not reproduce. Same-source checks use the previous baseline; takeovers do not trust a
    // baseline owned by another source. This is shared because .pwmodel, .pwskel and .pwanim all
    // have the same one-source/one-asset ownership rule.
    inline constexpr TCHAR PWSRC_RECOMPILE_UNMANAGED_STATE[] =
        TEXT("PWSRC_RECOMPILE_UNMANAGED_STATE");
}
