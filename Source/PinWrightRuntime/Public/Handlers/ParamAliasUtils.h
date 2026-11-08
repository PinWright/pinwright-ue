// Copyright (c) 2026 Alexander Penkin. MIT License.

// Generic FParamSpec alias-builder primitives, shared across handler families
// whose wire params drift between siblings (e.g. the asset-path slot accepting
// `assetPath` / `materialPath` / `path`). A per-family Keys() list (canonical
// first) is passed in; the canonical name becomes FParamSpec.Name and the
// remaining keys become FParamSpec.Aliases so the dispatcher honors them for
// both required-param satisfaction and the known-params set.
//
// This is the generic form of MaterialHandlerUtils::MakeAliasList — promoted
// here so asset/material/widget/blueprint families can share one builder
// instead of each copying the alias-dedup loop and spec-construction.
#pragma once

#include "CoreMinimal.h"
#include "Handlers/ParamSpec.h"

namespace ParamAliasUtils
{

// Build the alias list for CanonicalName from AllKeys: every key except the
// canonical one becomes an alias (order preserved, canonical dropped).
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

// Construct an FParamSpec whose canonical Name is Name and whose Aliases are the
// remaining entries of Keys. bRequired controls required-vs-optional.
inline FParamSpec MakeAliasParamSpec(const TCHAR* Name, const TCHAR* Type, const TCHAR* Desc,
                                     bool bRequired, const TArray<FString>& Keys)
{
    FParamSpec Spec{FString(Name), FString(Type), FString(Desc), bRequired, TEXT("")};
    Spec.Aliases = MakeAliasList(Keys, Name);
    return Spec;
}

// The RPC_PARAM_DEF form of the above: an optional param that documents a DEFAULT value AND
// accepts alternate spellings. Needed because MakeAliasParamSpec leaves Default empty, and a
// defaulted param converted to it silently loses the "Default: X" line the wiki renders from it.
inline FParamSpec MakeAliasParamSpecWithDefault(const TCHAR* Name, const TCHAR* Type, const TCHAR* Desc,
                                                const TCHAR* Default, const TArray<FString>& Keys)
{
    FParamSpec Spec{FString(Name), FString(Type), FString(Desc), false, FString(Default)};
    Spec.Aliases = MakeAliasList(Keys, Name);
    return Spec;
}

} // namespace ParamAliasUtils

// RPC_PARAM_REQ / RPC_PARAM_OPT plus one alternate wire spelling on the same slot. Same
// leading arguments as the plain macros, so a declaration whose body already honours a
// second spelling converts by renaming the macro and naming that spelling -- without
// re-wrapping a multi-hundred-character description. Both names land in the dispatcher's
// known-params set, which is what makes the body's fallback read reachable at all
// (PinWright.infra.declared_params.HandlersOnlyReadDeclaredParams).
#define RPC_PARAM_REQ_ALIAS(Name, Type, Desc, Alias) \
    ParamAliasUtils::MakeAliasParamSpec(TEXT(Name), TEXT(Type), TEXT(Desc), /*bRequired=*/true, \
        TArray<FString>({TEXT(Name), TEXT(Alias)}))

#define RPC_PARAM_OPT_ALIAS(Name, Type, Desc, Alias) \
    ParamAliasUtils::MakeAliasParamSpec(TEXT(Name), TEXT(Type), TEXT(Desc), /*bRequired=*/false, \
        TArray<FString>({TEXT(Name), TEXT(Alias)}))
