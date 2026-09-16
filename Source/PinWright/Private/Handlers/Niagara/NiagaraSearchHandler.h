// Copyright (c) 2026 Alexander Penkin. MIT License.

// Public surface of NiagaraSearchHandler.cpp helpers — exposed for unit tests.
#pragma once
#include "CoreMinimal.h"

#include "NiagaraEditorCommon.h"
#include "NiagaraScript.h"

struct FAssetData;
class FHandlerContext;

namespace NiagaraSearch
{
    inline constexpr int32 DefaultSearchLimit = 50;
    inline constexpr int32 MaxSearchLimit = 500;

    /**
     * Read and validate the shared limit used by the Niagara search verbs.
     * Uses the same number, boolean, numeric-string, and truncating fractional-number
     * coercions as FHandlerContext::GetInt. A negative coerced value is rejected with
     * INVALID_ARGUMENT; positive values above MaxSearchLimit are clamped, and zero
     * remains a valid zero-row cap.
     */
    PINWRIGHT_API bool ResolveSearchLimit(const FHandlerContext& Ctx, int32& OutLimit);

    /** Classify a UNiagaraNode subclass by the kind of data its payload encodes. */
    PINWRIGHT_API FString PayloadKindFor(UClass* NodeClass);

    /** Build a human-readable signature string for a Niagara op, e.g. "Add(float, float) -> float". */
    FString BuildOpSignature(const FNiagaraOpInfo& Op);

    /**
     * Score how well Query matches a Niagara op.
     * Returns 0 when the query is empty or there is no match.
     * Exact name match = 1000, prefix = 500, contains = 100, token-contains = 50,
     * fuzzy AlternateSearchName = 200.
     */
    PINWRIGHT_API int32 ScoreOpMatch(const FString& Query,
                       const FString& Name,
                       const FString& Alternate,
                       const FString& Category,
                       const FString& Keywords);

    // -----------------------------------------------------------------------
    // Module-search helpers (used by niagara.search_modules and its tests)
    // -----------------------------------------------------------------------

    /**
     * Returns true when the asset's Usage tag matches UsageName (case-insensitive).
     * Tag is emitted by the AssetRegistrySearchable UPROPERTY on UNiagaraScript::Usage
     * as the enum display string (e.g. "Module", "DynamicInput").
     */
    bool FilterByUsage(const FAssetData& AssetData, const FString& UsageName);

    /**
     * Returns true when the asset's ModuleUsageBitmask tag indicates that Stage
     * is a supported context.  Reads the raw int32 tag and calls
     * UNiagaraScript::IsSupportedUsageContextForBitmask.
     */
    bool FilterByStageBitmask(const FAssetData& AssetData, ENiagaraScriptUsage Stage);

    /**
     * Classify a Niagara script asset package path as:
     *   "engine"  — path starts with "/Niagara/" (engine Niagara content root)
     *   "project" — path starts with "/Game/"
     *   "plugin"  — starts with "/" but not "/Engine/" or "/Game/"
     *   "unknown" — anything else
     * Note: must use StartsWith (not Contains) — "/Game/Niagara/Foo" is project, not engine.
     */
    PINWRIGHT_API FString ClassifySource(const FString& PackagePath);

    /**
     * Score how well Query matches a module script by name / description / keywords.
     * Same tier structure as ScoreOpMatch.  Returns 0 if Query is empty.
     */
    PINWRIGHT_API int32 ScoreModuleMatch(const FString& Query,
                           const FString& Name,
                           const FString& Description,
                           const FString& Keywords);
}
