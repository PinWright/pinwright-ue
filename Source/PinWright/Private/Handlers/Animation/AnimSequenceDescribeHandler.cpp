// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Handlers/HandlerRegistration.h"
#include "Handlers/ParamSpec.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/Asset/AnimSequenceDumpBuilder.h"
#include "Animation/AnimSequence.h"
#include "Dom/JsonObject.h"

REGISTER_RPC_HANDLER("animation.describe_sequence", "animation",
    "Return read-only AnimSequence metadata using the same JSON shape as anim_sequence.json asset dumps: length, frame rate, additive type, skeleton, notifies, curves, sync markers.",
    RPC_PARAMS(
        RPC_PARAM_REQ("assetPath", "path", "AnimSequence asset path")
    ))
{
    FString AssetPath;
    if (!Ctx.RequireAssetPath(TEXT("assetPath"), AssetPath))
    {
        return true;
    }

    UAnimSequence* Sequence = LoadObject<UAnimSequence>(nullptr, *AssetPath);
    if (!Sequence)
    {
        Ctx.SendError(TEXT("ASSET_NOT_FOUND"),
            FString::Printf(TEXT("Could not load AnimSequence: %s"), *AssetPath));
        return true;
    }

    Ctx.SendSuccess(AnimSequenceDumpBuilder::BuildAnimSequenceJson(Sequence));
    return true;
}
