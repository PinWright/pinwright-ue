// Copyright (c) 2026 Alexander Penkin. MIT License.

// Shared param-alias helper for the asset.* path slots (read-back verbs first, plus
// asset.generate_lods' mesh slot and asset.delete's two path slots — see below).
// The asset-path slot accepts both `assetPath` (canonical) and `path` across the
// read-back verbs (asset.exists / asset.get / asset.validate / asset.dump).
// asset.list teaches callers the slot is `path`; without the alias the very next
// probe verb hard-fails MISSING_REQUIRED_PARAM 'assetPath'. The dispatcher honors
// FParamSpec aliases both for required-param satisfaction and the known-params set,
// so annotating the spec via ParamAliasUtils makes `path` resolve end-to-end; the
// body must read the value via RequireAssetPathRaw (which loops AssetPathKeys()) for
// the alias to reach the handler. The alias-builder itself lives in the generic
// Handlers/ParamAliasUtils.h, shared with the material/widget/blueprint families.
#pragma once

#include "CoreMinimal.h"
#include "Handlers/ParamSpec.h"
#include "Handlers/ParamAliasUtils.h"
#include "Handlers/HandlerContext.h"

namespace AssetPathParamUtils
{

// Candidate wire names for the asset-path slot, canonical first. Used both to populate
// FParamSpec aliases at registration and to read the value body-side via
// FHandlerContext::GetStringFirstOf so the alias resolves end-to-end.
inline const TArray<FString>& AssetPathKeys()
{
    static const TArray<FString> Keys = {
        TEXT("assetPath"),
        TEXT("path")
    };
    return Keys;
}

inline FParamSpec AssetPathParamReq(const TCHAR* Name, const TCHAR* Type, const TCHAR* Desc)
{
    return ParamAliasUtils::MakeAliasParamSpec(Name, Type, Desc, /*bRequired=*/true, AssetPathKeys());
}

// Body-side counterpart to the alias-annotated spec: resolves the asset-path value
// from the first matching key (canonical first) and, when none is present, sends the
// one canonical missing-path error so all four read-back verbs report it identically.
// Returns false (after sending the error) when the slot is empty; the body then does
// `if (!RequireAssetPathRaw(Ctx, Out)) return true;`. This is intentionally the raw
// (un-sanitized) value — asset.dump's TryResolveAssetPath / the manage verbs'
// UEditorAssetLibrary calls do their own normalization, so this does NOT call
// SanitizeProjectRelativePath (unlike FHandlerContext::RequireAssetPath).
inline bool RequireAssetPathRaw(const FHandlerContext& Ctx, FString& Out)
{
    Out = Ctx.GetStringFirstOf(AssetPathKeys());
    if (Out.IsEmpty())
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("assetPath required"));
        return false;
    }
    return true;
}

// Candidate wire names for asset.generate_lods' optional single-mesh slot, canonical
// first. Distinct from AssetPathKeys() (the read-back verbs' assetPath/path slot): this
// slot also accepts `meshPath` (matching asset.nanite_rebuild_mesh) and `landscapePath`,
// the original misnomer kept only as a backward-compat alias (the slot loads a
// UStaticMesh, not a landscape). Used both to populate FParamSpec aliases at
// registration and to read the value body-side via FHandlerContext::GetStringFirstOf.
inline const TArray<FString>& GenerateLodsSingleMeshKeys()
{
    static const TArray<FString> Keys = {
        TEXT("assetPath"),
        TEXT("meshPath"),
        TEXT("landscapePath")
    };
    return Keys;
}

// Optional single-mesh FParamSpec for asset.generate_lods, carrying the alias set
// above. The canonical name is the head of GenerateLodsSingleMeshKeys() so it is
// defined in exactly one place (mirrors ActorNameParamUtils::ActorNameParamReq).
inline FParamSpec GenerateLodsSingleMeshParamOpt(const TCHAR* Desc)
{
    return ParamAliasUtils::MakeAliasParamSpec(*GenerateLodsSingleMeshKeys()[0],
        TEXT("path"), Desc, /*bRequired=*/false, GenerateLodsSingleMeshKeys());
}

// Body-side counterpart: resolve the single-mesh value from the first matching key
// (canonical first). Returns the empty string when none is present; the slot is
// optional (the batch assetPaths slot may be used instead), so the caller decides
// whether absence is an error.
inline FString ResolveGenerateLodsSingleMesh(const FHandlerContext& Ctx)
{
    return Ctx.GetStringFirstOf(GenerateLodsSingleMeshKeys());
}

// ---- asset.delete's two path slots -------------------------------------------
//
// asset.delete is the one asset.* mutator whose slots are spelled `path` / `paths`.
// Both stay CANONICAL — they are documented, shipped, and used by existing callers and
// build scripts (docs/wiki-src/level-building.build-scripts.md shows `asset.delete
// {paths}`) — so `assetPath` / `assetPaths` are added purely as accepted aliases.
// They are the surface-dominant spellings (289 verbs declare `assetPath`; the batch
// spelling `assetPaths` is what asset.bulk_delete, asset.bulk_rename, asset.checkout,
// asset.submit, asset.generate_lods and the sourcecontrol verbs all require), so a
// caller arriving from any neighbouring asset verb otherwise ate a hard UNKNOWN_PARAMS
// round trip on the delete. Note the pairing is deliberate: `assetPath` aliases the
// single slot and `assetPaths` the batch slot, matching the singular/plural split the
// rest of the surface already uses.

// Candidate wire names for asset.delete's optional single-path slot, canonical first.
// Order differs from AssetPathKeys() on purpose: there `assetPath` is canonical, here
// `path` is, and MakeAliasParamSpec derives FParamSpec.Name from the head of the list.
inline const TArray<FString>& DeleteSinglePathKeys()
{
    static const TArray<FString> Keys = {
        TEXT("path"),
        TEXT("assetPath")
    };
    return Keys;
}

// Candidate wire names for asset.delete's optional batch slot, canonical first.
inline const TArray<FString>& DeleteBatchPathsKeys()
{
    static const TArray<FString> Keys = {
        TEXT("paths"),
        TEXT("assetPaths")
    };
    return Keys;
}

inline FParamSpec DeleteSinglePathParamOpt(const TCHAR* Desc)
{
    return ParamAliasUtils::MakeAliasParamSpec(*DeleteSinglePathKeys()[0],
        TEXT("path"), Desc, /*bRequired=*/false, DeleteSinglePathKeys());
}

inline FParamSpec DeleteBatchPathsParamOpt(const TCHAR* Desc)
{
    return ParamAliasUtils::MakeAliasParamSpec(*DeleteBatchPathsKeys()[0],
        TEXT("array"), Desc, /*bRequired=*/false, DeleteBatchPathsKeys());
}

// Body-side counterpart for the single-path slot. Returns the empty string when neither
// key is present; both slots are optional (either one satisfies the verb), so the
// handler — not the resolver — decides that an empty result set is the error.
inline FString ResolveDeleteSinglePath(const FHandlerContext& Ctx)
{
    return Ctx.GetStringFirstOf(DeleteSinglePathKeys());
}

// Body-side counterpart for the batch slot: appends the string entries of whichever
// accepted key the caller used. Non-string entries are skipped, which is the behaviour
// of the TryGetArrayField loop this replaces. Without this half the alias would clear
// the dispatcher's gates and then reach nothing — the shipped-defect shape documented
// in Handlers/Actor/SpawnParamUtils.h.
inline void ResolveDeleteBatchPaths(const FHandlerContext& Ctx, TArray<FString>& Out)
{
    const TSharedPtr<FJsonValue> Value = Ctx.GetJsonValueFirstOf(DeleteBatchPathsKeys());
    if (!Value.IsValid() || Value->Type != EJson::Array)
    {
        return;
    }
    for (const TSharedPtr<FJsonValue>& Entry : Value->AsArray())
    {
        if (Entry.IsValid() && Entry->Type == EJson::String)
        {
            Out.Add(Entry->AsString());
        }
    }
}

} // namespace AssetPathParamUtils

// Name-pattern slot for the search verbs. Separate namespace from the path slots
// above because it is a different axis (what to match, not where to look), but it
// lives in this header so the asset family has one param-alias home.
namespace SearchQueryParamUtils
{

// Candidate wire names for the name-pattern slot, canonical first. `pattern` is
// the spelling a caller arrives with from the actor-side find verbs and from the
// plain-English "match this pattern"; without the alias the dispatcher's
// unknown-parameter gate rejects the call with UNKNOWN_PARAMS before the body
// runs, so the alias must be declared on the FParamSpec (below) AND read
// body-side through ResolveSearchQuery — declaring only one half means the alias
// either never clears the gate or clears it and reaches nothing.
inline const TArray<FString>& SearchQueryKeys()
{
    static const TArray<FString> Keys = {
        TEXT("query"),
        TEXT("pattern")
    };
    return Keys;
}

inline FParamSpec SearchQueryParamReq(const TCHAR* Name, const TCHAR* Type, const TCHAR* Desc)
{
    return ParamAliasUtils::MakeAliasParamSpec(Name, Type, Desc, /*bRequired=*/true, SearchQueryKeys());
}

// Body-side counterpart: first matching key wins, canonical first. Returns the
// empty string when neither is present; the handler decides what that means.
inline FString ResolveSearchQuery(const FHandlerContext& Ctx)
{
    return Ctx.GetStringFirstOf(SearchQueryKeys());
}

} // namespace SearchQueryParamUtils
