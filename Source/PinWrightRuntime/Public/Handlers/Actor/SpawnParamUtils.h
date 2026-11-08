// Copyright (c) 2026 Alexander Penkin. MIT License.

// Alias helper for the actor.spawn* family's three drifting wire slots: the
// spawn-time display label, the "what to spawn" class/asset path, and the mesh path.
//
// Three separate defects motivated this, all of the same shape — the body already
// read alternate spellings via GetStringFirstOf, but the FParamSpec carried no
// Aliases, so the dispatcher's UNKNOWN_PARAMS gate rejected the payload BEFORE the
// body ran. The spelling was documented as accepted and was not:
//
//   1. `class_name` / `className` — SpawnHandler.cpp reads
//      GetStringFirstOf({classPath, class_name, className}) and the param description
//      said "Accepts class_name and className aliases", yet `class_name` returned
//      UNKNOWN_PARAMS. Same for `mesh_path` against the `meshPath` slot.
//   2. `assetPath` — the single most common asset-path spelling on the surface
//      (289 verbs declare it, vs 12 for `meshPath`), and the spelling every
//      neighbouring read verb uses (static_mesh.describe, asset.get,
//      render.capture_asset_preview). actor.spawn declared neither it nor an alias,
//      so "spawn this asset" cost a round trip. It resolves onto `classPath`, not
//      `meshPath`, because classPath is the strictly more capable slot: it already
//      accepts UClass names, /Script paths, BP asset paths AND static/skeletal mesh
//      paths (auto-picking StaticMeshActor/SkeletalMeshActor).
//   3. `label` / `name` — the spawn-time label slot. `actorName` stays canonical
//      (144 verbs use it for actor identity, so it is not moving), but the sibling
//      spawn verbs in other namespaces spell this same "label the actor I am
//      creating" slot `name` (lighting.spawn_light, environment.spawn_*,
//      water.spawn_*, niagara.spawn_actor, effect.spawn_niagara), and actor.set_label
//      spells the identical value `label`. A caller arriving from either eats a round
//      trip on actor.spawn.
//
// Canonical names are unchanged, so no existing caller or wiki example breaks. The
// dispatcher honors FParamSpec.Aliases for both required-param satisfaction
// (PayloadHasParamOrAlias) and the known-params set (AddKnownParamNames); the body
// must still read via the matching Resolve* helper for the alias to reach the handler.
//
// `assetPath` is deliberately attached to ONE slot only. Aliasing it onto both
// classPath and meshPath would make a single wire key populate two mutually
// informing slots in the same request.
//
// Mirrors the per-family alias helpers (ActorNameParamUtils, AssetPathParamUtils,
// LevelNameParamUtils, GeometryNameParamUtils, SpawnMaterialUtils) built on the
// generic Handlers/ParamAliasUtils.h.
#pragma once

#include "CoreMinimal.h"
#include "Handlers/ParamSpec.h"
#include "Handlers/ParamAliasUtils.h"
#include "Handlers/HandlerContext.h"

namespace SpawnParamUtils
{

// ---- spawn-time display label ------------------------------------------------

// Accepted wire keys for the spawn-time label slot, canonical first. `actorName` is
// the documented canonical and the surface-wide spelling for actor identity; `label`
// matches actor.set_label's slot for the identical value; `name` matches the spawn
// verbs in the lighting/environment/water/niagara/effect namespaces; `actor_name` is
// the snake_case variant of the canonical key.
inline const TArray<FString>& SpawnLabelKeys()
{
    static const TArray<FString> Keys = {
        TEXT("actorName"),
        TEXT("label"),
        TEXT("name"),
        TEXT("actor_name")
    };
    return Keys;
}

// Optional spawn-time label spec carrying the alias set above. Each verb passes its
// own description because the default-when-omitted behaviour differs (actor.spawn
// derives from the mesh/class name, actor.spawn_shape from the shape's mesh name,
// actor.spawn_from_blueprint lets UE auto-generate one).
inline FParamSpec SpawnLabelParamOpt(const TCHAR* Desc)
{
    return ParamAliasUtils::MakeAliasParamSpec(*SpawnLabelKeys()[0], TEXT("string"), Desc,
        /*bRequired=*/false, SpawnLabelKeys());
}

// Read the spawn-time label from whichever accepted key the caller used. Returns the
// empty string when none is present; the slot is optional, so each verb applies its
// own fallback.
inline FString ResolveSpawnLabel(const FHandlerContext& Ctx)
{
    return Ctx.GetStringFirstOf(SpawnLabelKeys());
}

// ---- class / asset path ------------------------------------------------------

// Accepted wire keys for the "what to spawn" slot, canonical first. `assetPath` is the
// surface-dominant asset-path spelling and resolves here because classPath already
// loads mesh and Blueprint ASSET paths, not just UClass names. `class_name` and
// `className` were read body-side long before they were declared.
inline const TArray<FString>& SpawnClassPathKeys()
{
    static const TArray<FString> Keys = {
        TEXT("classPath"),
        TEXT("assetPath"),
        TEXT("class_name"),
        TEXT("className")
    };
    return Keys;
}

inline FParamSpec SpawnClassPathParamOpt(const TCHAR* Desc)
{
    return ParamAliasUtils::MakeAliasParamSpec(*SpawnClassPathKeys()[0], TEXT("classref"), Desc,
        /*bRequired=*/false, SpawnClassPathKeys());
}

inline FString ResolveSpawnClassPath(const FHandlerContext& Ctx)
{
    return Ctx.GetStringFirstOf(SpawnClassPathKeys());
}

// ---- mesh path ---------------------------------------------------------------

// Accepted wire keys for the explicit mesh slot, canonical first. `assetPath` is NOT
// here on purpose — it resolves onto classPath (see the header note), which handles a
// mesh path identically and a class path as well.
inline const TArray<FString>& SpawnMeshPathKeys()
{
    static const TArray<FString> Keys = {
        TEXT("meshPath"),
        TEXT("mesh_path")
    };
    return Keys;
}

inline FParamSpec SpawnMeshPathParamOpt(const TCHAR* Desc)
{
    return ParamAliasUtils::MakeAliasParamSpec(*SpawnMeshPathKeys()[0], TEXT("path"), Desc,
        /*bRequired=*/false, SpawnMeshPathKeys());
}

inline FString ResolveSpawnMeshPath(const FHandlerContext& Ctx)
{
    return Ctx.GetStringFirstOf(SpawnMeshPathKeys());
}

} // namespace SpawnParamUtils
