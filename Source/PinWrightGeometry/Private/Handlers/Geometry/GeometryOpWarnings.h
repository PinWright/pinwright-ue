// Copyright (c) 2026 Alexander Penkin. MIT License.

// GeometryOpWarnings.h - the single place a geometry RPC wrapper turns GeometryOps::FOpResult's
// non-fatal notes into something a caller can actually read.
//
// Why this file exists at all: FOpResult has carried Warnings since the op extraction, and until
// now NO RPC wrapper read them. Every clamp-reports-itself warning the ops layer emits - a
// segment count raised to the engine floor, a substituted revolve profile, a subdivide capped at
// GEOM_MAX_SUBDIVIDE_ITERATIONS - was visible only through the .pwmodel compiler
// (PwModelCompiler.cpp routes them to PWMODEL_STAGE_WARNING). An RPC caller passing segments=1
// got a bare success and never learned the engine used 3, which is precisely the silent footgun
// the warnings were written to close.
//
// Shape: `warnings` is a JSON string array emitted ONLY when the op produced at least one, which
// is the dominant convention for the field across the plugin (actor.set_property,
// actor.add_component, actor.spawn_batch, material.*, render.*, landscape.* all gate on
// Num() > 0; only the compile/decompile report families - agir/bpir/crir/mgir/pcg - emit it
// unconditionally, because there the report IS the payload). It is also the only shape that
// keeps this an additive change: ~146 automation tests drive the real dispatcher and assert on
// response fields, and a warning-free call - which is every call those tests make - keeps a
// byte-identical response.
//
// Placement rule for callers: add the warnings LAST, next to the SendSuccess, so a wrapper that
// grows an early return cannot skip it while still reporting success.
#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Handlers/Geometry/GeometryOps.h"
#include "Utils/JsonUtils.h"

namespace GeometryOps
{
    // Surface Op.Warnings on an RPC response as `warnings`. No-op when the op warned about
    // nothing, so the response shape is unchanged for every call that did not trip a clamp.
    inline void AddOpWarnings(const TSharedPtr<FJsonObject>& Result, const FOpResult& Op)
    {
        if (!Result.IsValid() || Op.Warnings.Num() == 0)
        {
            return;
        }
        Result->SetArrayField(TEXT("warnings"), EmitStringArray(Op.Warnings));
    }

    // Surface the skin-weight repair on an RPC response as a `skinWeights` object. Emitted ONLY
    // when the op ran on a mesh that carries skin weights - same additive rule as `warnings`
    // above, and for the same reason: every dispatcher test booleans unskinned boxes, so a
    // skinned-only field keeps those responses byte-identical.
    //
    // Why a caller wants it: a boolean over a skinned mesh silently re-weights the vertices it
    // creates, and until this field existed there was no way to see how many. verticesTransferred
    // is the honest size of that edit; verticesUnresolved above zero means the mesh still carries
    // influences nothing could account for and should be audited before it is baked.
    inline void AddOpSkinWeights(const TSharedPtr<FJsonObject>& Result, const FOpResult& Op)
    {
        if (!Result.IsValid() || !Op.bSkinned)
        {
            return;
        }
        TSharedPtr<FJsonObject> SkinWeights = MakeShared<FJsonObject>();
        SkinWeights->SetNumberField(TEXT("verticesTransferred"), Op.SkinWeightsTransferred);
        SkinWeights->SetNumberField(TEXT("verticesUnresolved"), Op.SkinWeightsUnresolved);
        Result->SetObjectField(TEXT("skinWeights"), SkinWeights);
    }
}
