// Copyright (c) 2026 Alexander Penkin. MIT License.

// Geometry-local alias helper for the create-verb actor-name slot.
//
// The geometry.create_* family (create_box / create_sphere / ... /
// create_procedural_mesh) names the new actor's slot `name`, while every verb
// that subsequently operates on that same actor requires `actorName`
// (geometry.get_mesh_info, append_triangle, recalculate_normals, ...). So
// within one procedural-mesh build the caller had to spell the same actor's
// name two different ways. This helper annotates the create-verb `name` slot
// with an `actorName` alias (canonical stays `name`, so existing callers keep
// working) and reads the value first-of {name, actorName}, removing the
// mid-build spelling flip. Mirrors the per-namespace alias helpers
// (BlueprintHandlerUtils, MaterialHandlerUtils, ...) built on the generic
// ParamAliasUtils, and the dispatcher honors FParamSpec.Aliases for both
// required-param satisfaction and the known-params set.
#pragma once

#include "CoreMinimal.h"
#include "Handlers/ParamSpec.h"
#include "Handlers/ParamAliasUtils.h"
#include "Handlers/HandlerContext.h"

namespace GeometryNameParamUtils
{

// Accepted wire keys for the create-verb actor-name slot, canonical first.
// `name` is the documented canonical; `actorName` is the spelling every operate
// verb in the namespace demands, accepted here as an alias so the same name
// reuses across one build without a UNKNOWN_PARAMS round-trip.
inline const TArray<FString>& CreateNameKeys()
{
    static const TArray<FString> Keys = { TEXT("name"), TEXT("actorName") };
    return Keys;
}

// Optional create-verb name spec carrying the `actorName` alias. The default
// description is shared by every geometry.create_* verb (the slot is identical
// across the family), so it lives here once; a verb that ever needs custom text
// can still pass an override.
inline FParamSpec CreateNameParamOpt(const TCHAR* Desc = TEXT("Name for the new actor (accepts the 'actorName' alias the operate verbs use)"))
{
    return ParamAliasUtils::MakeAliasParamSpec(TEXT("name"), TEXT("string"), Desc,
        /*bRequired=*/false, CreateNameKeys());
}

// Read the actor name from whichever accepted key the caller used, falling back
// to Default. Resolves the `actorName` alias end-to-end so the value reaches the
// handler body even when the caller used the operate-verb spelling.
inline FString ResolveCreateName(const FHandlerContext& Ctx, const FString& Default)
{
    return Ctx.GetStringFirstOf(CreateNameKeys(), Default);
}

} // namespace GeometryNameParamUtils
