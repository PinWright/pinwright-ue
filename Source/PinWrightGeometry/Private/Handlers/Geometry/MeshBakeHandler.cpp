// Copyright (c) 2026 Alexander Penkin. MIT License.

// MeshBakeHandler.cpp - geometry.bake_ambient_occlusion.
//
// The one verb that computes a signal FROM the geometry and writes it back into the mesh, rather
// than moving geometry around. It exists because a mesh assembled from interpenetrating parts has
// no contact shading at any junction and no other verb can author one: a tiling detail map is a
// function of UV, so it structurally cannot know where two solids meet.
//
// Vertex colours, not a texture: the vertex/alpha bake costs no extra interpolator, no second UV
// set and no texture memory, and it needs no UV unwrap to exist first. A UV-space variant would
// pair naturally with geometry.pack_uv_islands and is deliberately not here.
#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/ErrorCodes.h"
#include "Handlers/Geometry/GeometryOpWarnings.h"
#include "Handlers/Geometry/GeometryOps_Elements.h"
#include "Handlers/Geometry/GeometryTarget.h"
#include "Handlers/Geometry/GeometryUtils.h"
// Full ADynamicMeshActor definition: GeometryTarget.h only forward-declares it, and the resolved
// FGeometryTarget::Actor is passed where an AActor* is expected.
#include "DynamicMeshActor.h"
#include "Dom/JsonObject.h"

namespace
{
    // Uniquely prefixed: this module builds with bUseUnity = true, so an anonymous-namespace helper
    // with a common name would collide with a sibling TU when Unity merges them.
    //
    // One statistics block as {min, mean, max, count}. `count` is what separates "measured and
    // came out flat" from "measured nothing", and those two must never read alike.
    TSharedPtr<FJsonObject> MeshBakeHandler_BuildStatisticsJson(const GeometryOps::FValueStatistics& Stats)
    {
        TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
        Out->SetNumberField(TEXT("min"), Stats.Min);
        Out->SetNumberField(TEXT("mean"), Stats.Mean);
        Out->SetNumberField(TEXT("max"), Stats.Max);
        Out->SetNumberField(TEXT("count"), Stats.Count);
        return Out;
    }
}

// ============================================================================
// bake_ambient_occlusion
// ============================================================================
REGISTER_RPC_HANDLER("geometry.bake_ambient_occlusion", "geometry",
    "Ray-cast a dynamic mesh against itself and write the resulting ambient-occlusion term into chosen vertex-colour channels; returns per-channel min/mean/max so a flat bake is visible in the response",
    RPC_PARAMS(
        RPC_PARAM_REQ("actorName", "string", "Name of the DynamicMeshActor"),
        RPC_PARAM_REQ("occlusionRadius", "number", "Maximum ray length, in the mesh's own LOCAL units (actor scale does not apply). Required: it is what makes a part junction occlude while the far side of the same mesh does not"),
        RPC_PARAM_REQ("channels", "string", "Which vertex-colour channels receive the bake: any combination of r, g, b, a. Required - RGB and A each already carry a signal on a real mesh, so there is no channel this can pick for you"),
        RPC_PARAM_DEF("samples", "integer", "Rays cast per colour element. Trades noise for time linearly", "64"),
        RPC_PARAM_DEF("biasAngleDegrees", "number", "Rays arriving within this angle of the surface's own tangent plane have their weight rolled off, which stops a faceted surface reading its neighbouring facet as an occluder. An ANGLE, not a distance offset", "15"),
        RPC_PARAM_DEF("blend", "string", "replace (write the bake into the masked channels) or multiply (scale what they already carry by it)", "replace"),
        RPC_PARAM_DEF("strength", "number", "0-1, lerps the bake toward fully exposed; 0 writes white and darkens nothing", "1")
    ))
{
    const FString ActorName = Ctx.GetString(TEXT("actorName"));
    if (ActorName.IsEmpty())
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT, TEXT("actorName required"));
        return true;
    }

    GeometryOps::EColorChannels Channels = GeometryOps::EColorChannels::None;
    {
        FString ChannelError;
        if (!GeometryOps::ParseColorChannels(Ctx.GetString(TEXT("channels")), Channels, ChannelError))
        {
            Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT, ChannelError);
            return true;
        }
    }

    // An unrecognized blend is refused rather than falling through to `replace`: the two produce
    // visibly different meshes, and a caller who misspelled one would get the other silently.
    const FString BlendSpec = Ctx.GetString(TEXT("blend"), TEXT("replace"));
    bool bMultiply = false;
    if (BlendSpec.Equals(TEXT("multiply"), ESearchCase::IgnoreCase))
    {
        bMultiply = true;
    }
    else if (!BlendSpec.Equals(TEXT("replace"), ESearchCase::IgnoreCase))
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT, FString::Printf(
            TEXT("blend '%s' is not recognized; use 'replace' or 'multiply'."), *BlendSpec));
        return true;
    }

    GeometryOps::FBakeAmbientOcclusionParams Params;
    Params.OcclusionRadius = Ctx.GetNumber(TEXT("occlusionRadius"), 0.0);
    Params.Samples = Ctx.GetInt(TEXT("samples"), Params.Samples);
    Params.BiasAngleDegrees = Ctx.GetNumber(TEXT("biasAngleDegrees"), Params.BiasAngleDegrees);
    Params.Strength = Ctx.GetNumber(TEXT("strength"), Params.Strength);
    Params.Channels = Channels;
    Params.bMultiply = bMultiply;

    FGeometryTarget Target;
    if (!GeometryTarget::ResolveOrSendError(Ctx, ActorName, Target))
        return true;

    GeometryOps::FBakeAmbientOcclusionOutputs Outputs;
    const GeometryOps::FOpResult Op =
        GeometryOps::BakeAmbientOcclusion(Target.Mesh, Params, Outputs);
    if (!Op.bSuccess)
    {
        Ctx.SendError(Op.ErrorCode, Op.ErrorMessage);
        return true;
    }

    GeometryUtils::MarkGeometryActorModified(Target.Component);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("actorName"), ActorName);
    GeometryUtils::AddResolvedActorIdentity(Result, Target.Actor);
    Result->SetStringField(TEXT("channels"), GeometryOps::ColorChannelsToString(Channels));
    Result->SetNumberField(TEXT("occlusionRadius"), Params.OcclusionRadius);
    Result->SetNumberField(TEXT("samples"), Params.Samples);
    Result->SetNumberField(TEXT("biasAngleDegrees"), Params.BiasAngleDegrees);
    Result->SetStringField(TEXT("blend"), bMultiply ? TEXT("multiply") : TEXT("replace"));
    Result->SetNumberField(TEXT("strength"), Params.Strength);
    Result->SetNumberField(TEXT("verticesModified"), Outputs.VerticesModified);
    Result->SetNumberField(TEXT("elementsWritten"), Outputs.ElementsWritten);
    Result->SetNumberField(TEXT("colorElementsCreated"), Outputs.ElementsCreated);
    // Two repairs the bake had to make to the mesh before it could measure anything. Both are
    // edits the caller did not ask for, so they are reported rather than done quietly - and both
    // are guards against an engine crash or an uninitialized read, not tidying.
    Result->SetNumberField(TEXT("orphanColorElementsFreed"), Outputs.OrphanElementsFreed);
    Result->SetNumberField(TEXT("trianglesGivenNormals"), Outputs.TrianglesGivenNormals);

    // The flatness detector, and the reason this verb reports statistics at all. Three successive
    // hand-written AO producers shipped dead-flat output while passing their authors' own
    // hand-picked spot checks; a single min/mean/max over every measured corner is what makes
    // that visible without opening the mesh. `occlusion` is the RAW term, before strength and
    // before a multiply blend, so a bake that measured nothing reads min == mean == max == 1
    // here however the written channels end up looking.
    Result->SetObjectField(TEXT("occlusion"), MeshBakeHandler_BuildStatisticsJson(Outputs.Occlusion));

    // Per channel, over the values actually stored. Only the channels the mask named appear.
    const TCHAR* ChannelNames[4] = { TEXT("r"), TEXT("g"), TEXT("b"), TEXT("a") };
    TSharedPtr<FJsonObject> Written = MakeShared<FJsonObject>();
    for (int32 Channel = 0; Channel < 4; ++Channel)
    {
        if (Outputs.Written[Channel].Count > 0)
        {
            Written->SetObjectField(ChannelNames[Channel], MeshBakeHandler_BuildStatisticsJson(Outputs.Written[Channel]));
        }
    }
    Result->SetObjectField(TEXT("written"), Written);

    GeometryOps::AddOpWarnings(Result, Op);
    Ctx.SendSuccess(TEXT("Ambient occlusion baked"), Result);
    return true;
}
