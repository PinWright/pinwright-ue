// Copyright (c) 2026 Alexander Penkin. MIT License.

// PwAnimParser.h - Parser and published vocabulary for the .pwanim format.
#pragma once

#include "CoreMinimal.h"
#include "Containers/ArrayView.h"
#include "Containers/StringView.h"

#include "PwAnim/PwAnimAst.h"
#include "PwAnim/PwAnimDiagnostic.h"
#include "PwSource/PwParamSpec.h"

// A format-local op specification.  The parameter type and validation machinery remain
// in PwSource; this table owns the animation vocabulary and its block shape.
struct FPwAnimOpSpec
{
    FString Name;
    FString Description;
    bool bAcceptsBlock = false;
    bool bRequiresBlock = false;
    TArray<FPwParamSpec> Params;

    const FPwParamSpec* FindParam(const FString& ParamName) const;
};

namespace PwAnimOpTable
{
    // Get() contains the statement op that may occur inside a bone.  timebase, bone and
    // sync_marker are format-level constructs and publish their parameter sets through the
    // dedicated accessors below rather than being mixed into the model op vocabulary.
    const TArray<FPwAnimOpSpec>& Get();
    const FPwAnimOpSpec* Find(const FString& OpName);
    TArray<FString> Names();
    TArray<FString> TopLevelNames();

    TArrayView<const FPwParamSpec> TimebaseParams();
    TArrayView<const FPwParamSpec> BoneHeaderParams();
    TArrayView<const FPwParamSpec> KeyParams();
    TArrayView<const FPwParamSpec> SyncMarkerParams();
}

class PINWRIGHT_API FPwAnimParser
{
public:
    // Tokenizes and parses the complete source.  OutDocument and OutDiagnostics are reset
    // for every call.  A false result means at least one error-severity diagnostic exists;
    // the partially recovered AST is still useful for reporting all source errors.
    static bool Parse(FStringView Source, FPwAnimDocument& OutDocument,
                      TArray<FPwDiagnostic>& OutDiagnostics);
};

using FPwAnimationParser = FPwAnimParser;
