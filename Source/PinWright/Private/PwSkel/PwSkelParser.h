// Copyright (c) 2026 Alexander Penkin. MIT License.

// PwSkelParser.h - Parser and published vocabulary for the .pwskel format.
#pragma once

#include "CoreMinimal.h"
#include "Containers/ArrayView.h"
#include "Containers/StringView.h"

#include "PwSkel/PwSkelAst.h"
#include "PwSkel/PwSkelDiagnostic.h"
#include "PwSource/PwParamSpec.h"

// The format-specific description used by skeleton.describe_ops. The parser and the
// description verb both consume this table, so a parameter accepted by the parser cannot
// silently disappear from the published vocabulary.
struct FPwSkelOpSpec
{
    FString Name;
    FString Context;
    FString Description;
    bool bAcceptsBlock = false;
    bool bRequiresBlock = false;
    TArray<FPwParamSpec> Params;

    const FPwParamSpec* FindParam(const FString& ParamName) const;
};

namespace PwSkelOpTable
{
    // Top-level rig constructs plus the curve-block linked_bone entry.
    const TArray<FPwSkelOpSpec>& Get();
    const FPwSkelOpSpec* Find(const FString& OpName);
    TArray<FString> Names();

    // Header parameters accepted by every bone declaration. They are returned as source-core
    // specs because FPwParseCursor owns the generic type, arity and range validation.
    TArrayView<const FPwParamSpec> BoneHeaderParams();
    TArrayView<const FPwParamSpec> PreviewMeshParams();
    TArrayView<const FPwParamSpec> CurveHeaderParams();
}

class PINWRIGHT_API FPwSkelParser
{
public:
    // Always tokenizes and parses the complete source, returning false when any error-severity
    // diagnostic was emitted. OutDocument and OutDiagnostics are reset for every call.
    static bool Parse(FStringView Source, FPwSkelDocument& OutDocument,
                      TArray<FPwDiagnostic>& OutDiagnostics);
};

// Long-form alias for callers that use the format name rather than its file-extension spelling.
using FPwSkeletonParser = FPwSkelParser;
