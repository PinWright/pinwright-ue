// Copyright (c) 2026 Alexander Penkin. MIT License.

// Level-local alias helper for the create-verb level-name slot.
//
// level.create and level.structure.create_level name the new level's leaf/short-
// name slot `levelName`, but a caller reaching for the obvious generic `name`
// (the spelling many sibling "create" RPCs expose, and the name task briefs use)
// eats a UNKNOWN_PARAMS round-trip — the spec carried no alias, so the dispatcher
// rejected the payload before the body ran. This helper annotates the `levelName`
// slot with a `name` alias (canonical stays `levelName`, so existing callers and
// wiki examples are unchanged) and reads the value first-of {levelName, name}.
//
// Scope: the `name` synonym ONLY. The destination-path slot (`levelPath`) is left
// unaliased on purpose — on level.create it is a FULL destination package path,
// which conflicts with the settled create-verb convention where `path` means a
// destination FOLDER (Handlers/Material/MaterialCreatePathParamUtils.h, with a
// combined full path carried by `assetPath` and split server-side). The combined-
// full-path angle is the E-material-create-combined-assetpath-split umbrella's
// call, not this ticket's.
//
// Mirrors the per-namespace alias helpers (GeometryNameParamUtils,
// BlueprintHandlerUtils, MaterialHandlerUtils, ...) built on the generic
// ParamAliasUtils; the dispatcher honors FParamSpec.Aliases for both required-
// param satisfaction and the known-params set.
#pragma once

#include "CoreMinimal.h"
#include "Handlers/ParamSpec.h"
#include "Handlers/ParamAliasUtils.h"
#include "Handlers/HandlerContext.h"

namespace LevelNameParamUtils
{

// Accepted wire keys for the create-verb level-name slot, canonical first.
// `levelName` is the documented canonical; `name` is the generic spelling callers
// reach for, accepted here as an alias so it does not trip UNKNOWN_PARAMS.
inline const TArray<FString>& CreateNameKeys()
{
    static const TArray<FString> Keys = { TEXT("levelName"), TEXT("name") };
    return Keys;
}

// FParamSpec for the create-verb level-name slot carrying the `name` alias.
// bRequired matches the verb (required on level.structure.create_level, optional
// on level.create). Pass the per-verb description so each page stays accurate.
inline FParamSpec CreateNameParam(const TCHAR* Desc, bool bRequired)
{
    return ParamAliasUtils::MakeAliasParamSpec(TEXT("levelName"), TEXT("path"), Desc,
        bRequired, CreateNameKeys());
}

// Read the level name from whichever accepted key the caller used, falling back
// to Default. Resolves the `name` alias end-to-end so the value reaches the
// handler body even when the caller used the generic spelling.
inline FString ResolveCreateName(const FHandlerContext& Ctx, const FString& Default = TEXT(""))
{
    return Ctx.GetStringFirstOf(CreateNameKeys(), Default);
}

} // namespace LevelNameParamUtils
