// Copyright (c) 2026 Alexander Penkin. MIT License.

// RaycastHandler.cpp - spatial.raycast: cast a world line trace and report the hit(s).
//
// Beyond the plain "first blocking hit" trace this verb answers the question a caller
// usually MEANS to ask: "what does this ray hit ON THE THING I CARE ABOUT". Two knobs do
// that work - multiHit (walk the layers along the ray) and the actor filters
// (onlyActors / actorFilter / onlyClasses). With traceComplex:true a static mesh that has NO
// simple collision still BLOCKS, because complex tracing resolves against the render
// triangles. That is correct engine behaviour, not a bug; the defect was that the API gave
// a caller no way to say "I only want the landscape", so a naive height probe silently
// returned the height of a collisionless tree. Responses now also carry
// simpleCollisionShapes / renderGeometryHit so the case is detectable without knowing the
// engine rule.

#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/ParamSpec.h"
#include "Handlers/Spatial/SpatialTraceUtils.h"
#include "Utils/ActorUtils.h"
#include "Utils/NameMatchFilter.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#include "Editor.h"
#include "Engine/EngineTypes.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Components/PrimitiveComponent.h"

namespace
{
    // Cap on how many rejected-actor names the response echoes. The list exists so a
    // filtered trace can say WHAT it skipped; past a couple of dozen names the count is
    // the useful signal, not the roster.
    constexpr int32 RaycastMaxReportedRejects = 32;

    // Coordinate-convention echo attached to every raycast result so a caller never
    // has to guess units/handedness when interpreting location/normal/distance.
    void RaycastAddAxisEcho(const TSharedPtr<FJsonObject>& Data)
    {
        Data->SetStringField(TEXT("units"), TEXT("cm"));
        Data->SetStringField(TEXT("axis"), TEXT("+X fwd, +Y right, +Z up, left-handed"));
    }

    TSharedPtr<FJsonObject> RaycastMakeVectorObject(const FVector& Vec)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetNumberField(TEXT("x"), Vec.X);
        Obj->SetNumberField(TEXT("y"), Vec.Y);
        Obj->SetNumberField(TEXT("z"), Vec.Z);
        return Obj;
    }

    // Editor world by default; PIE world when a play session is active so a raycast
    // hits the same actors actor.spawn / actor lookups resolve against.
    UWorld* RaycastResolveWorld()
    {
        if (!GEditor)
        {
            return nullptr;
        }
        if (GEditor->PlayWorld)
        {
            return GEditor->PlayWorld;
        }
        return GEditor->GetEditorWorldContext().World();
    }

    // Maps the wire `channel` string to a trace channel. Only the channels a caller
    // realistically raycasts against are accepted; anything else is a caller error.
    bool RaycastMapChannel(const FString& Name, ECollisionChannel& OutChannel)
    {
        const FString Lower = Name.ToLower();
        if (Lower == TEXT("visibility"))
        {
            OutChannel = ECC_Visibility;
            return true;
        }
        if (Lower == TEXT("camera"))
        {
            OutChannel = ECC_Camera;
            return true;
        }
        if (Lower == TEXT("worldstatic"))
        {
            OutChannel = ECC_WorldStatic;
            return true;
        }
        if (Lower == TEXT("worlddynamic"))
        {
            OutChannel = ECC_WorldDynamic;
            return true;
        }
        return false;
    }

    // Reads a {x,y,z}/[x,y,z] vector from the first present key among Keys. Returns
    // false when none of the keys is present so the caller can distinguish an omitted
    // vector from a legitimately-zero one (GetVector alone cannot).
    bool RaycastReadVectorFirstOf(const FHandlerContext& Ctx, const TArray<FString>& Keys,
                                  FVector& OutVec)
    {
        const TSharedPtr<FJsonObject>& Payload = Ctx.GetRawPayload();
        if (!Payload.IsValid())
        {
            return false;
        }
        for (const FString& Key : Keys)
        {
            if (Payload->HasField(Key))
            {
                OutVec = Ctx.GetVector(Key);
                return true;
            }
        }
        return false;
    }

    // First present array param among Keys, or null when none is present. Mirrors
    // RaycastReadVectorFirstOf's presence-aware lookup for the camelCase/snake_case pairs.
    const TArray<TSharedPtr<FJsonValue>>* RaycastGetArrayFirstOf(const FHandlerContext& Ctx,
                                                                 const TArray<FString>& Keys)
    {
        for (const FString& Key : Keys)
        {
            if (const TArray<TSharedPtr<FJsonValue>>* Found = Ctx.GetArray(Key))
            {
                return Found;
            }
        }
        return nullptr;
    }

    // Non-empty trimmed strings out of a JSON array param, skipping non-string elements.
    void RaycastCollectStrings(const TArray<TSharedPtr<FJsonValue>>* Array, TArray<FString>& Out)
    {
        if (!Array)
        {
            return;
        }
        for (const TSharedPtr<FJsonValue>& Val : *Array)
        {
            FString Name;
            if (Val.IsValid() && Val->TryGetString(Name))
            {
                Name.TrimStartAndEndInline();
                if (!Name.IsEmpty())
                {
                    Out.AddUnique(Name);
                }
            }
        }
    }

    // Identity block for a struck actor. name/path keep their historical meaning
    // (name == display label) so existing callers are untouched; label/internalName/class
    // are additive and exist so a caller can filter on the SAME key it will pass back in
    // onlyActors / actorFilter without a second actor.get round-trip.
    TSharedPtr<FJsonObject> RaycastMakeActorObject(AActor* Actor)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetStringField(TEXT("name"), Actor->GetActorLabel());
        Obj->SetStringField(TEXT("path"), Actor->GetPathName());
        Obj->SetStringField(TEXT("label"), Actor->GetActorLabel());
        Obj->SetStringField(TEXT("internalName"), Actor->GetName());
        Obj->SetStringField(TEXT("class"),
            Actor->GetClass() ? Actor->GetClass()->GetPathName() : FString());
        return Obj;
    }

    // Writes one hit's fields onto Target. Used for both the top-level single-hit shape
    // (Target == the response root, so the legacy keys stay exactly where they were) and
    // each element of the multiHit `hits` array.
    void RaycastWriteHitFields(const TSharedPtr<FJsonObject>& Target,
                               const SpatialTraceUtils::FSpatialHit& Hit)
    {
        Target->SetObjectField(TEXT("location"), RaycastMakeVectorObject(Hit.Location));
        Target->SetObjectField(TEXT("normal"), RaycastMakeVectorObject(Hit.Normal));
        Target->SetNumberField(TEXT("distance"), Hit.Distance);

        if (AActor* HitActor = Hit.HitActor.Get())
        {
            Target->SetObjectField(TEXT("actor"), RaycastMakeActorObject(HitActor));
        }
        if (UPrimitiveComponent* HitComp = Hit.HitComponent.Get())
        {
            Target->SetStringField(TEXT("component"), HitComp->GetName());
        }
        // Diagnostics are emitted only when meaningful: an absent faceIndex /
        // simpleCollisionShapes means "the backend reported nothing", which is different
        // from a real 0 and must not be forged into one.
        if (Hit.FaceIndex >= 0)
        {
            Target->SetNumberField(TEXT("faceIndex"), Hit.FaceIndex);
        }
        if (Hit.SimpleCollisionShapes >= 0)
        {
            Target->SetNumberField(TEXT("simpleCollisionShapes"), Hit.SimpleCollisionShapes);
        }
        if (Hit.bRenderGeometryHit)
        {
            Target->SetBoolField(TEXT("renderGeometryHit"), true);
        }
    }

    // Display labels of the actors a filter peeled away, capped for response size.
    void RaycastWriteRejects(const TSharedPtr<FJsonObject>& Data,
                             const TArray<TWeakObjectPtr<AActor>>& Rejected)
    {
        if (Rejected.Num() == 0)
        {
            return;
        }
        TArray<TSharedPtr<FJsonValue>> Names;
        for (const TWeakObjectPtr<AActor>& Weak : Rejected)
        {
            if (Names.Num() >= RaycastMaxReportedRejects)
            {
                break;
            }
            if (AActor* Actor = Weak.Get())
            {
                Names.Add(MakeShared<FJsonValueString>(Actor->GetActorLabel()));
            }
        }
        Data->SetArrayField(TEXT("filteredOut"), Names);
        Data->SetNumberField(TEXT("filteredOutCount"), Rejected.Num());
    }

    void RaycastWriteStringArray(const TSharedPtr<FJsonObject>& Data, const TCHAR* Key,
                                 const TArray<FString>& Values)
    {
        if (Values.Num() == 0)
        {
            return;
        }
        TArray<TSharedPtr<FJsonValue>> Array;
        for (const FString& Value : Values)
        {
            Array.Add(MakeShared<FJsonValueString>(Value));
        }
        Data->SetArrayField(Key, Array);
    }
}

// ---- spatial.raycast ----
REGISTER_RPC_HANDLER("spatial.raycast", "spatial",
    "Cast a world-space line trace and report the hit. Provide origin plus either a direction (ray) or a target point (segment). By default returns the first blocking hit; multiHit:true walks the layers along the ray (one hit per actor, nearest first) and onlyActors/actorFilter/onlyClasses restrict which hits count, so a ground probe can ask for the terrain and skip whatever is in front of it. A clean miss is a success with hit:false, not an error. WARNING: traceComplex:true resolves against RENDER triangles, so a mesh with no simple collision still blocks - each hit reports simpleCollisionShapes and renderGeometryHit so that case is detectable. Coordinates are in unreal units (cm), left-handed (+X forward, +Y right, +Z up).",
    RPC_PARAMS(
        FParamSpec{TEXT("origin"), TEXT("object"),
            TEXT("Ray start as {x,y,z} (or [x,y,z]) in unreal units (cm). Required."),
            true, TEXT(""), TArray<FString>({TEXT("start"), TEXT("from")})},
        FParamSpec{TEXT("direction"), TEXT("object"),
            TEXT("Ray direction as {x,y,z} (normalized internally). Provide this or target."),
            false, TEXT(""), TArray<FString>({TEXT("dir")})},
        FParamSpec{TEXT("target"), TEXT("object"),
            TEXT("Segment end point as {x,y,z} in cm. When set, direction is derived and the trace stops here. Provide this or direction."),
            false, TEXT(""), TArray<FString>({TEXT("end"), TEXT("to")})},
        FParamSpec{TEXT("maxDistance"), TEXT("number"),
            TEXT("Max ray length in cm when using direction (default 1e7). Ignored when target is given."),
            false, TEXT(""), TArray<FString>({TEXT("max_distance"), TEXT("distance")})},
        RPC_PARAM_OPT("channel", "string",
            "Trace channel: visibility (default), camera, worldstatic, or worlddynamic."),
        FParamSpec{TEXT("traceComplex"), TEXT("boolean"),
            TEXT("Trace against per-triangle (complex) collision instead of simple collision. Default false. WARNING: complex collision for a StaticMesh IS its render triangle soup, so a mesh with NO collision set up still BLOCKS the ray - expected engine behaviour, and the usual cause of a ground probe reporting the height of a tree. Filter with onlyActors/actorFilter/onlyClasses, or use multiHit and pick the hit you want."),
            false, TEXT(""), TArray<FString>({TEXT("trace_complex")})},
        FParamSpec{TEXT("ignoreActors"), TEXT("array"),
            TEXT("Names/labels/paths of actors to exclude from the trace entirely (engine-level ignore list). Names that do not resolve are skipped and echoed back as unresolvedIgnoreActors."),
            false, TEXT(""), TArray<FString>({TEXT("ignore_actors")})},
        FParamSpec{TEXT("multiHit"), TEXT("boolean"),
            TEXT("Return every layer along the ray instead of only the first blocking hit: at most one hit per actor, nearest first, in a hits[] array (count + truncated alongside). Default false. Costs one trace per layer, bounded by maxHits."),
            false, TEXT(""), TArray<FString>({TEXT("multi_hit"), TEXT("returnAllHits"), TEXT("return_all_hits")})},
        FParamSpec{TEXT("maxHits"), TEXT("number"),
            TEXT("Cap on layers walked and hits returned (default 32, ceiling 256). Unlike actor.list's limit, 0 is NOT 'all' - each layer costs a line trace - and is rejected with INVALID_PARAMS."),
            false, TEXT("32"), TArray<FString>({TEXT("max_hits")})},
        FParamSpec{TEXT("onlyActors"), TEXT("array"),
            TEXT("Accept hits ONLY on these actors (names/labels/paths, resolved to exact actors). Anything else on the ray is peeled and reported in filteredOut. If none of the given names resolve the call fails with ACTOR_NOT_FOUND rather than reporting a silent miss."),
            false, TEXT(""), TArray<FString>({TEXT("only_actors")})},
        FParamSpec{TEXT("actorFilter"), TEXT("string"),
            TEXT("Accept hits only on actors whose internal name OR display label matches this pattern, e.g. \"Terrain\". Matched with the shared name-filter policy: case-insensitive 'contains' by default, tunable with matchMode and caseSensitive (same vocabulary as actor.list's filter)."),
            false, TEXT(""), TArray<FString>({TEXT("actor_filter"), TEXT("filter")})},
        NameMatch::MatchModeParam(TEXT("actorFilter")),
        NameMatch::CaseSensitiveParam(TEXT("actorFilter")),
        FParamSpec{TEXT("onlyClasses"), TEXT("array"),
            TEXT("Accept hits only on actors whose class or ANY ancestor class matches one of these, as a case-insensitive substring of the class name or its /Script path (matchMode/caseSensitive govern actorFilter only - these are class identifiers, not labels). The terrain-probe shortcut: [\"LandscapeProxy\"] covers both Landscape and LandscapeStreamingProxy without knowing any actor name."),
            false, TEXT(""), TArray<FString>({TEXT("only_classes"), TEXT("classFilter"), TEXT("class_filter")})}
    ))
{
    // origin is required. Validate in the handler body too (not just the dispatcher's
    // param-spec check) so the direct-invocation path reports the same typed error.
    FVector Origin;
    if (!RaycastReadVectorFirstOf(Ctx, {TEXT("origin"), TEXT("start"), TEXT("from")}, Origin))
    {
        Ctx.SendError(TEXT("MISSING_REQUIRED_PARAM"),
            TEXT("Missing required parameter 'origin' ({x,y,z} in cm)."));
        return true;
    }

    // Exactly one of direction / target must be supplied.
    FVector DirectionInput;
    const bool bHasDirection =
        RaycastReadVectorFirstOf(Ctx, {TEXT("direction"), TEXT("dir")}, DirectionInput);
    FVector Target;
    const bool bHasTarget =
        RaycastReadVectorFirstOf(Ctx, {TEXT("target"), TEXT("end"), TEXT("to")}, Target);

    if (!bHasDirection && !bHasTarget)
    {
        Ctx.SendError(TEXT("INVALID_PARAMS"),
            TEXT("Provide either 'direction' {x,y,z} or 'target' {x,y,z}."));
        return true;
    }

    const double MaxDistance = Ctx.GetNumber(
        TEXT("maxDistance"),
        Ctx.GetNumber(TEXT("max_distance"), Ctx.GetNumber(TEXT("distance"), 1.0e7)));

    // Resolve the trace direction and segment end.
    FVector Direction;
    FVector End;
    if (bHasTarget)
    {
        Direction = (Target - Origin).GetSafeNormal();
        End = Target;
    }
    else
    {
        Direction = DirectionInput.GetSafeNormal();
        End = Origin + Direction * MaxDistance;
    }

    if (Direction.IsNearlyZero())
    {
        Ctx.SendError(TEXT("INVALID_PARAMS"),
            bHasTarget
                ? TEXT("target coincides with origin; cannot derive a ray direction.")
                : TEXT("direction is zero-length; provide a non-zero {x,y,z}."));
        return true;
    }

    // Channel (optional, default visibility).
    ECollisionChannel Channel = ECC_Visibility;
    const FString ChannelName = Ctx.GetString(TEXT("channel"));
    if (!ChannelName.IsEmpty() && !RaycastMapChannel(ChannelName, Channel))
    {
        Ctx.SendError(TEXT("INVALID_PARAMS"),
            FString::Printf(TEXT("Unknown channel '%s'. Valid: visibility, camera, worldstatic, worlddynamic."),
                *ChannelName));
        return true;
    }

    const bool bTraceComplex =
        Ctx.GetBoolFirstOf({TEXT("traceComplex"), TEXT("trace_complex")}, false);

    const bool bMultiHit = Ctx.GetBoolFirstOf(
        {TEXT("multiHit"), TEXT("multi_hit"), TEXT("returnAllHits"), TEXT("return_all_hits")}, false);

    // maxHits bounds both the returned hits and the number of internal trace passes, so a
    // non-positive value would make the call a guaranteed no-op. Reject it instead of
    // silently substituting a default - the caller asked for something impossible.
    const int32 MaxHitsRequested =
        Ctx.GetIntFirstOf({TEXT("maxHits"), TEXT("max_hits")})
            .Get(SpatialTraceUtils::DefaultMaxLayers);
    if (MaxHitsRequested < 1)
    {
        Ctx.SendError(TEXT("INVALID_PARAMS"),
            FString::Printf(TEXT("maxHits must be >= 1 (got %d). Each hit costs one trace pass, so 0 does not mean 'all' here; the ceiling is %d."),
                MaxHitsRequested, SpatialTraceUtils::MaxAllowedLayers));
        return true;
    }
    const int32 MaxHits = FMath::Min(MaxHitsRequested, SpatialTraceUtils::MaxAllowedLayers);

    UWorld* World = RaycastResolveWorld();
    if (!World)
    {
        Ctx.SendError(TEXT("EDITOR_WORLD_NOT_AVAILABLE"),
            TEXT("No editor/PIE world available for the trace."));
        return true;
    }

    // Resolve ignore-actor names to actors in this world. Unresolved names are still
    // skipped (a stale name must not fail an otherwise-valid trace - that is the shipped
    // contract) but they are now ECHOED so the silent drop is at least visible.
    TArray<FString> IgnoreNames;
    RaycastCollectStrings(
        RaycastGetArrayFirstOf(Ctx, {TEXT("ignoreActors"), TEXT("ignore_actors")}), IgnoreNames);
    TArray<AActor*> IgnoreActors;
    TArray<FString> UnresolvedIgnore;
    for (const FString& Name : IgnoreNames)
    {
        if (AActor* Found = McpActorUtils::FindActorByName(World, Name))
        {
            IgnoreActors.AddUnique(Found);
        }
        else
        {
            UnresolvedIgnore.Add(Name);
        }
    }

    // ---- Accept-filter (onlyActors / actorFilter / onlyClasses) ----
    SpatialTraceUtils::FSpatialHitFilter Filter;

    TArray<FString> OnlyNames;
    RaycastCollectStrings(
        RaycastGetArrayFirstOf(Ctx, {TEXT("onlyActors"), TEXT("only_actors")}), OnlyNames);
    TArray<FString> UnresolvedOnly;
    for (const FString& Name : OnlyNames)
    {
        if (AActor* Found = McpActorUtils::FindActorByName(World, Name))
        {
            Filter.OnlyActors.AddUnique(TWeakObjectPtr<AActor>(Found));
        }
        else
        {
            UnresolvedOnly.Add(Name);
        }
    }
    // A restriction where nothing resolved can never match, so every trace would report a
    // miss that looks exactly like "nothing is there". Fail loudly instead.
    if (OnlyNames.Num() > 0 && Filter.OnlyActors.Num() == 0)
    {
        Ctx.SendError(TEXT("ACTOR_NOT_FOUND"),
            FString::Printf(TEXT("onlyActors named %d actor(s) and none resolved in this world: [%s]. A restriction that cannot match would report hit:false for every ray."),
                OnlyNames.Num(), *FString::Join(OnlyNames, TEXT(", "))));
        return true;
    }

    // Shared name-filter policy (Utils/NameMatchFilter.h): parses actorFilter + matchMode +
    // caseSensitive together and rejects an unusable combination itself (INVALID_MODE,
    // INVALID_PATTERN for a bad regex, INVALID_ARGUMENT for matchMode with no pattern),
    // so this verb speaks exactly the vocabulary actor.list does.
    if (!NameMatch::Require(Ctx,
            TArray<FString>{TEXT("actorFilter"), TEXT("actor_filter"), TEXT("filter")},
            Filter.ActorFilter))
    {
        return true;
    }
    RaycastCollectStrings(
        RaycastGetArrayFirstOf(Ctx, {TEXT("onlyClasses"), TEXT("only_classes"),
                                     TEXT("classFilter"), TEXT("class_filter")}),
        Filter.OnlyClasses);

    const bool bFiltered = !Filter.IsEmpty();

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    RaycastAddAxisEcho(Data);
    Data->SetStringField(TEXT("channel"),
        ChannelName.IsEmpty() ? FString(TEXT("visibility")) : ChannelName.ToLower());
    Data->SetBoolField(TEXT("traceComplex"), bTraceComplex);
    // Echo the resolved pattern + matchMode + caseSensitive (no-op for an inactive filter,
    // so an unfiltered response keeps its shape). Without it a caller cannot tell WHICH
    // matching semantics ran - the exact blind spot B-actor-list-filter-case-mismatch hit.
    NameMatch::AddFilterEcho(Data, Filter.ActorFilter, TEXT("actorFilter"));
    RaycastWriteStringArray(Data, TEXT("unresolvedIgnoreActors"), UnresolvedIgnore);
    RaycastWriteStringArray(Data, TEXT("unresolvedOnlyActors"), UnresolvedOnly);

    // Collected hits, in ray order. The legacy path produces zero or one; the layered path
    // produces up to maxHits. Either way hits[0] (when present) drives the top-level
    // single-hit fields, so an existing caller sees no shape change.
    TArray<SpatialTraceUtils::FSpatialHit> Hits;

    if (bMultiHit || bFiltered)
    {
        SpatialTraceUtils::FSpatialLayeredTraceOptions Options;
        Options.Filter = Filter;
        Options.MaxLayers = MaxHits;
        // Unfiltered multi-hit wants every layer; a filtered single-hit call only needs the
        // nearest ACCEPTED hit, so it stops peeling the moment it finds one.
        Options.MaxAcceptedHits = bMultiHit ? MaxHits : 1;

        const SpatialTraceUtils::FSpatialLayeredTraceResult Layered =
            SpatialTraceUtils::TraceLineLayered(World, Origin, End, Channel, bTraceComplex,
                                                IgnoreActors, Options);
        Hits = Layered.Hits;
        RaycastWriteRejects(Data, Layered.RejectedActors);
        // truncated == the layer budget ran out with geometry still on the ray. Reported for
        // the filtered single-hit path too: without it, "budget exhausted before I found
        // your actor" would be indistinguishable from "your actor is not on this ray".
        Data->SetBoolField(TEXT("truncated"), Layered.bTruncated);

        if (bMultiHit)
        {
            TArray<TSharedPtr<FJsonValue>> HitsArray;
            for (const SpatialTraceUtils::FSpatialHit& Hit : Hits)
            {
                TSharedPtr<FJsonObject> HitObj = MakeShared<FJsonObject>();
                RaycastWriteHitFields(HitObj, Hit);
                HitsArray.Add(MakeShared<FJsonValueObject>(HitObj));
            }
            Data->SetArrayField(TEXT("hits"), HitsArray);
            Data->SetNumberField(TEXT("count"), HitsArray.Num());
        }
    }
    else
    {
        // Legacy path: exactly one LineTraceSingleByChannel, byte-identical behaviour to
        // the pre-filter verb (the extra per-hit diagnostic fields are purely additive).
        const SpatialTraceUtils::FSpatialHit Hit =
            SpatialTraceUtils::TraceLine(World, Origin, End, Channel, bTraceComplex, IgnoreActors);
        if (Hit.bHit)
        {
            Hits.Add(Hit);
        }
    }

    if (Hits.Num() == 0)
    {
        // A miss is a valid outcome, not an error.
        Data->SetBoolField(TEXT("hit"), false);
        Ctx.SendSuccess(Data);
        return true;
    }

    Data->SetBoolField(TEXT("hit"), true);
    RaycastWriteHitFields(Data, Hits[0]);

    // Name the trap in the response itself: a returned hit that resolved against render
    // triangles is the case that silently corrupts height maps, and an agent that never
    // read the wiki page has no other way to know the rule.
    TArray<FString> Warnings;
    for (const SpatialTraceUtils::FSpatialHit& Hit : Hits)
    {
        if (!Hit.bRenderGeometryHit)
        {
            continue;
        }
        AActor* HitActor = Hit.HitActor.Get();
        const FString ActorLabel =
            HitActor ? HitActor->GetActorLabel() : FString(TEXT("<unknown actor>"));
        Warnings.Add(FString::Printf(
            TEXT("traceComplex hit the RENDER geometry of '%s', which has no simple collision at all. This is expected engine behaviour (complex collision for a StaticMesh is its render triangles). If you meant to probe the ground, restrict the trace with onlyActors/actorFilter/onlyClasses or add it to ignoreActors."),
            *ActorLabel));
        if (Warnings.Num() >= RaycastMaxReportedRejects)
        {
            break;
        }
    }
    RaycastWriteStringArray(Data, TEXT("warnings"), Warnings);

    Ctx.SendSuccess(Data);
    return true;
}
