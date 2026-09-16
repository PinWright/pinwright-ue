// Copyright (c) 2026 Alexander Penkin. MIT License.

// Shared param-alias helpers for Material/*.cpp handler files.
// Header-only; mirrors BlueprintHandlerUtils.h's FParamSpec alias pattern so the
// material asset-path slot (assetPath / materialPath / path) and the expression-class
// slot (expressionClass / nodeType / className) accept the same wire names across all
// material.authoring.* and material.graph.* siblings.
#pragma once

#include "CoreMinimal.h"
#include "Handlers/ParamSpec.h"

namespace MaterialHandlerUtils
{

// Candidate wire names for the material asset-path slot, canonical first. Used both
// to populate FParamSpec aliases at registration and to read the value body-side via
// FHandlerContext::RequireAssetPath / GetStringFirstOf so the alias resolves end-to-end.
inline const TArray<FString>& MaterialAssetPathKeys()
{
    static const TArray<FString> Keys = {
        TEXT("assetPath"),
        TEXT("materialPath"),
        TEXT("path")
    };
    return Keys;
}

// Candidate wire names for the expression-class slot, canonical first.
inline const TArray<FString>& MaterialExpressionClassKeys()
{
    static const TArray<FString> Keys = {
        TEXT("expressionClass"),
        TEXT("nodeType"),
        TEXT("className")
    };
    return Keys;
}

inline TArray<FString> MakeAliasList(const TArray<FString>& AllKeys, const TCHAR* CanonicalName)
{
    TArray<FString> Aliases;
    const FString Canonical(CanonicalName);
    for (const FString& Key : AllKeys)
    {
        if (!Key.Equals(Canonical))
        {
            Aliases.Add(Key);
        }
    }
    return Aliases;
}

inline FParamSpec MaterialAssetPathParamReq(const TCHAR* Name, const TCHAR* Type, const TCHAR* Desc)
{
    FParamSpec Spec{FString(Name), FString(Type), FString(Desc), true, TEXT("")};
    Spec.Aliases = MakeAliasList(MaterialAssetPathKeys(), Name);
    return Spec;
}

inline FParamSpec MaterialAssetPathParamOpt(const TCHAR* Name, const TCHAR* Type, const TCHAR* Desc)
{
    FParamSpec Spec{FString(Name), FString(Type), FString(Desc), false, TEXT("")};
    Spec.Aliases = MakeAliasList(MaterialAssetPathKeys(), Name);
    return Spec;
}

inline FParamSpec MaterialExpressionClassParamReq(const TCHAR* Name, const TCHAR* Type, const TCHAR* Desc)
{
    FParamSpec Spec{FString(Name), FString(Type), FString(Desc), true, TEXT("")};
    Spec.Aliases = MakeAliasList(MaterialExpressionClassKeys(), Name);
    return Spec;
}

inline FParamSpec MaterialExpressionClassParamOpt(const TCHAR* Name, const TCHAR* Type, const TCHAR* Desc)
{
    FParamSpec Spec{FString(Name), FString(Type), FString(Desc), false, TEXT("")};
    Spec.Aliases = MakeAliasList(MaterialExpressionClassKeys(), Name);
    return Spec;
}

} // namespace MaterialHandlerUtils
