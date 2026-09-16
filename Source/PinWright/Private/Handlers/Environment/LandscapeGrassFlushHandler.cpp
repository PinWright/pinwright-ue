// Copyright (c) 2026 Alexander Penkin. MIT License.

// LandscapeGrassFlushHandler.cpp - landscape.flush_grass
//
// The named, discoverable form of the one thing that makes a grass-type edit reach the
// screen (board B-grass-varieties-edit-does-not-reach-renderer).
//
// It exists because the two flush routes a caller reaches for by name do not:
// `grass.FlushCacheAll` is not a registered console command on any engine (only
// `grass.FlushCache` and `grass.FlushCachePIE` are, 5.8 LandscapeGrass.cpp:3474-3483),
// and `unreal.Landscape.flush_grass_components` is not a Python attribute because
// ALandscapeProxy::FlushGrassComponents is LANDSCAPE_API but not a UFUNCTION. Neither
// name appears anywhere in this plugin or its docs — they were guesses, and the fix for
// a guess that fails is a real verb with a real name, not a documentation patch.
//
// The verbs in this plugin that edit a grass type already refresh it themselves (the
// reflected mutators via Handlers/Utility/UtilityPropertyHandler.cpp), so this is the
// escape hatch for edits made somewhere else: the asset editor, a Python script, an
// undo. It refuses to be the whole fix — a flush a caller has to remember is exactly the
// shape the ticket calls the weaker half.
//
// All policy lives in Handlers/Environment/GrassTypeConsumers.h, shared with the
// reflected-mutator path so the two surfaces cannot drift apart.

#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/ParamSpec.h"
#include "Handlers/ErrorCodes.h"
#include "Handlers/Environment/GrassTypeConsumers.h"
#include "Utils/DerivedStateReport.h"

#include "Dom/JsonObject.h"
#include "LandscapeGrassType.h"
#include "UObject/UObjectGlobals.h"

// ---- landscape.flush_grass ----
REGISTER_RPC_HANDLER("landscape.flush_grass", "landscape",
    "Invalidate and rebuild the grass instances every landscape in the editor world built from one "
    "ULandscapeGrassType, so an edit to that asset reaches the renderer. Editing GrassVarieties does "
    "NOT do this on its own: from UE 5.4 the engine's own PostEditChangeProperty stopped flushing the "
    "per-proxy grass cache, so the components keep drawing the instances built from the previous "
    "values while every verb reports success. Verbs in this plugin that edit a grass type already "
    "call this internally; use it after an edit made elsewhere (asset editor, Python, undo). "
    "NON-DESTRUCTIVE, and the response proves it: this drops the built grass INSTANCES only and "
    "leaves the per-component grass density maps intact, so the carpet comes back as the camera "
    "moves. The engine console command system.console_command {command: \"grass.FlushCache\"} is "
    "NOT equivalent - it additionally deletes those density maps on every landscape in the process, "
    "which in the editor is only recovered by the amortised camera-driven grass-map builder or an "
    "editor restart. The response carries two MEASURED blocks: consumerRefresh (consumersFound / "
    "consumersRefreshed / refreshed[], read from the proxies' grass cache before and after, not "
    "from the fact the call returned) and grassMaps (componentsHoldingMapsBefore / "
    "componentsHoldingMapsAfter, equal on every correct run; a `discarded` field appears only if "
    "maps were lost, which would be a defect in this verb).",
    RPC_PARAMS(
        RPC_PARAM_REQ("grassTypePath", "path",
            "Object path of the ULandscapeGrassType whose consumers should be refreshed.")
    ))
{
    FString GrassTypePath;
    if (!Ctx.RequireAssetPath(TEXT("grassTypePath"), GrassTypePath))
    {
        return true;
    }

    ULandscapeGrassType* GrassType = Cast<ULandscapeGrassType>(StaticLoadObject(
        ULandscapeGrassType::StaticClass(), nullptr, *GrassTypePath, nullptr, LOAD_NoWarn));
    if (!GrassType)
    {
        Ctx.SendError(ErrorCodes::ERR_ASSET_NOT_FOUND,
            FString::Printf(TEXT("ULandscapeGrassType not found: %s"), *GrassTypePath));
        return true;
    }

    PinWright::GrassConsumers::FGrassRefreshReport GrassRefresh;
    PinWright::GrassConsumers::ApplyGrassTypeEdit(GrassType, GrassRefresh);
    PinWright::GrassConsumers::AddKnownUnrefreshedGrassConsumers(GrassRefresh.Consumers);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetBoolField(TEXT("success"), true);
    Result->SetStringField(TEXT("grassTypePath"), GrassType->GetPathName());
    PinWright::DerivedState::AddConsumerRefreshReport(Result, GrassRefresh.Consumers);
    // consumerRefresh alone cannot tell an invalidation from a destruction - both empty
    // the cache entries it counts - and the first version of this verb destroyed the
    // grass while reporting subObjectsRefreshed: 136 as coverage. grassMaps is the
    // second instrument that separates them, so it is published on every run.
    PinWright::GrassConsumers::AddGrassMapIntegrityReport(Result, GrassRefresh.GrassMaps);

    // The message reports what was measured, never what was requested. "No landscape in
    // the editor world consumes this grass type" is a real and common answer - a grass
    // type referenced by no landscape material has nothing to refresh - and it must not
    // read like a refresh that happened. A lost density map is named here too rather
    // than only in the block, because it contradicts the sentence beside it.
    FString Message = GrassRefresh.Consumers.ConsumersFound == 0
        ? TEXT("No landscape in the editor world holds grass built from this grass type")
        : FString::Printf(TEXT("Grass refreshed on %d of %d landscape(s)"),
            GrassRefresh.Consumers.ConsumersRefreshed, GrassRefresh.Consumers.ConsumersFound);
    if (const int32 Discarded = GrassRefresh.GrassMaps.Discarded(); Discarded > 0)
    {
        Message += FString::Printf(
            TEXT(" - but %d landscape component(s) LOST their grass density map, which this "
                 "verb must never cause; see grassMaps.warning"), Discarded);
    }

    Ctx.SendSuccess(Message, Result);

    return true;
}
