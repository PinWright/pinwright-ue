// Copyright (c) 2026 Alexander Penkin. MIT License.

// GroundPlacementHandler.cpp - the two batch ground verbs:
//   spatial.ground_actors     - seat a set of actors on a stated surface, bedded in, and report
//                               per actor whether that actually happened.
//   spatial.verify_grounding  - measure contact quality for a set of actors. Non-mutating.
//
// Both are BATCH by construction. A per-actor Python loop over a level is what wedged an editor
// for 168 minutes with 5100 uncancellable calls; one RPC that walks the set on the game thread
// is the fix, so neither verb has a single-actor form that invites the loop back.
//
// Both take a REQUIRED `surface` argument. That is the deliberate structural guarantee: "which
// surface did you mean" is the question every observed ground-placement failure answered wrong
// (a fog card, a tree, the render geometry of a haze plane), and it is the one thing these verbs
// will not guess at. See GroundPlacementUtils.h for the solver and its rationale.
//
// Neither verb reports a placement it did not make. `placed` per actor is
// FGroundSeatResult::IsSeated(), which is a TOptional<FTransform> being set by the code that
// actually moved the actor, ANDed with a post-move re-measurement that passed.
//
// The MUTATING verb carries two further structural guarantees, both earned from one call that
// seated six actors belonging to another agent in the same level:
//   - a pattern selector ('prefix' / 'filter') requires `expectedMatches`, and a match count
//     that disagrees with it is refused before the first move. A name pattern is not a scope.
//   - every actor it moves is listed in `movedActors[]` with its pre-move transform, at every
//     detail level and successes included. That is the only undo the verb has, so it cannot be
//     conditional on the move having failed.
//
// Coordinates are unreal units (cm), left-handed (+X fwd, +Y right, +Z up).

#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/ParamSpec.h"
#include "Handlers/ErrorCodes.h"
#include "Handlers/Actor/ActorNameParamUtils.h"  // spatial.ground_instances names ONE holder actor
#include "Handlers/Actor/InstancedMeshUtils.h"   // ...and shares the actor.*_instances seams
#include "Handlers/Spatial/GroundPlacementUtils.h"
#include "Handlers/Spatial/SpatialTraceUtils.h"
#include "PinWrightSubsystem.h" // LogPinWrightSubsystem
#include "Utils/ActorUtils.h"
#include "Utils/NameMatchFilter.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Engine/EngineTypes.h"
#include "Engine/Selection.h"
#include "Engine/World.h"
#include "EngineUtils.h" // TActorIterator
#include "GameFramework/Actor.h"
#include "ScopedTransaction.h"

namespace
{
    // Per-call ceiling on how many actors one batch may touch. Chosen to bound worst-case game
    // thread time rather than response size: each actor costs up to three footprint
    // measurements, and a measurement is (samples^2) columns of traces. A caller with more
    // actors pages with offset.
    constexpr int32 GroundRpcMaxBatch = 5000;
    constexpr int32 GroundRpcDefaultLimit = 512;

    // Ceiling on per-actor rows echoed in the response. Counts are always exact and uncapped;
    // only the detail rows are bounded, the same split actor.spawn_batch uses for skipped[]
    // (SpawnBatchHandler.cpp:248-261).
    constexpr int32 GroundRpcMaxDetailRows = 256;

    // FParamSpec carrying BOTH a documented default and snake_case aliases.
    // RPC_PARAM_DEF cannot express aliases and ParamAliasUtils::MakeAliasParamSpec cannot
    // express a default; nearly every param here needs both, so the aggregate-init is wrapped
    // once instead of repeated. Field order is FParamSpec's declaration order
    // (Handlers/ParamSpec.h:15-22): Name, Type, Description, bRequired, Default, Aliases,
    // TypedAliases. Declaring the aliases is load-bearing, not cosmetic: the dispatcher rejects
    // any top-level field that is neither a declared name nor a declared alias.
    FParamSpec GroundRpcParam(const TCHAR* Name, const TCHAR* Type, const TCHAR* Desc,
                              const TCHAR* Default, const TArray<FString>& Aliases)
    {
        FParamSpec Spec{FString(Name), FString(Type), FString(Desc), false, FString(Default)};
        Spec.Aliases = Aliases;
        return Spec;
    }

    void GroundRpcAddAxisEcho(const TSharedPtr<FJsonObject>& Data)
    {
        Data->SetStringField(TEXT("units"), TEXT("cm"));
        Data->SetStringField(TEXT("axis"), TEXT("+X fwd, +Y right, +Z up, left-handed"));
    }

    TSharedPtr<FJsonObject> GroundRpcVectorObject(const FVector& Vec)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetNumberField(TEXT("x"), Vec.X);
        Obj->SetNumberField(TEXT("y"), Vec.Y);
        Obj->SetNumberField(TEXT("z"), Vec.Z);
        return Obj;
    }

    TSharedPtr<FJsonObject> GroundRpcTransformObject(const FTransform& Xf)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetObjectField(TEXT("location"), GroundRpcVectorObject(Xf.GetLocation()));
        const FRotator Rot = Xf.GetRotation().Rotator();
        TSharedPtr<FJsonObject> RotObj = MakeShared<FJsonObject>();
        RotObj->SetNumberField(TEXT("pitch"), Rot.Pitch);
        RotObj->SetNumberField(TEXT("yaw"), Rot.Yaw);
        RotObj->SetNumberField(TEXT("roll"), Rot.Roll);
        Obj->SetObjectField(TEXT("rotation"), RotObj);
        Obj->SetObjectField(TEXT("scale"), GroundRpcVectorObject(Xf.GetScale3D()));
        return Obj;
    }

    // Editor world by default; PIE world when a play session is active, matching
    // RaycastResolveWorld / PlacementResolveWorld so every spatial verb targets the same world.
    UWorld* GroundRpcResolveWorld()
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

    bool GroundRpcMapChannel(const FString& Name, ECollisionChannel& OutChannel)
    {
        const FString Lower = Name.ToLower();
        if (Lower == TEXT("visibility"))  { OutChannel = ECC_Visibility;   return true; }
        if (Lower == TEXT("camera"))      { OutChannel = ECC_Camera;       return true; }
        if (Lower == TEXT("worldstatic")) { OutChannel = ECC_WorldStatic;  return true; }
        if (Lower == TEXT("worlddynamic")){ OutChannel = ECC_WorldDynamic; return true; }
        return false;
    }

    // Non-empty trimmed strings out of a JSON array field, skipping non-strings.
    void GroundRpcCollectStrings(const TSharedPtr<FJsonObject>& Obj, const TCHAR* Key,
                                 TArray<FString>& Out)
    {
        if (!Obj.IsValid())
        {
            return;
        }
        const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
        if (!Obj->TryGetArrayField(Key, Array) || !Array)
        {
            return;
        }
        for (const TSharedPtr<FJsonValue>& Val : *Array)
        {
            FString Item;
            if (Val.IsValid() && Val->TryGetString(Item))
            {
                Item.TrimStartAndEndInline();
                if (!Item.IsEmpty())
                {
                    Out.AddUnique(Item);
                }
            }
        }
    }

    void GroundRpcWriteStringArray(const TSharedPtr<FJsonObject>& Data, const TCHAR* Key,
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

    // ---- The required `surface` argument -------------------------------------------------

    // Parse the caller's surface spec. Returns false having sent a typed error; never returns
    // true with a partially-populated spec, and never falls back to "trace everything" - an
    // unrecognized preset is an error, not a default.
    //
    // `surface` is ALSO declared RPC_PARAM_REQ, so the dispatcher rejects an absent one before
    // the handler runs (RpcDispatcher.cpp:95-115, MISSING_REQUIRED_PARAM). The check here is the
    // second layer and is not redundant: it is what runs for a `surface` that is present but not
    // an object or carries an unknown preset, it carries the explanation the generic dispatcher
    // message cannot, and it is what the direct-invoke tests exercise. Same belt-and-braces
    // reasoning as RaycastHandler.cpp:290.
    bool GroundRpcParseSurface(const FHandlerContext& Ctx, UWorld* World,
                               GroundPlacement::FGroundSurfaceSpec& OutSpec,
                               TArray<FString>& OutUnresolvedIgnores)
    {
        TSharedPtr<FJsonObject> SurfaceObj = Ctx.GetObject(TEXT("surface"));
        if (!SurfaceObj.IsValid())
        {
            Ctx.SendError(ErrorCodes::ERR_INVALID_SURFACE_SPEC,
                TEXT("'surface' is required and must be an object. State what counts as ground: "
                     "{\"preset\":\"landscape\"} for terrain (the right answer for a height probe - "
                     "a landscape heightfield is single-valued per column so nothing can shadow "
                     "it), {\"preset\":\"any_solid\"} for anything solid except foliage and effect "
                     "geometry, or {\"preset\":\"custom\", \"onlyClasses\":[...], \"excludeNames\":[...]} "
                     "to say it exactly. There is no default: a wrong ground surface is the cause "
                     "of every floating-prop bug this verb exists to prevent."));
            return false;
        }

        FString PresetName = TEXT("custom");
        SurfaceObj->TryGetStringField(TEXT("preset"), PresetName);
        GroundPlacement::ESurfacePreset Preset = GroundPlacement::ESurfacePreset::Custom;
        if (!GroundPlacement::ParseSurfacePreset(PresetName, Preset))
        {
            Ctx.SendError(ErrorCodes::ERR_INVALID_SURFACE_SPEC,
                FString::Printf(TEXT("Unknown surface preset '%s'. Valid: landscape, any_solid, custom."),
                    *PresetName));
            return false;
        }
        OutSpec.Preset = Preset;

        FString ChannelName;
        if (SurfaceObj->TryGetStringField(TEXT("channel"), ChannelName) && !ChannelName.IsEmpty())
        {
            if (!GroundRpcMapChannel(ChannelName, OutSpec.Channel))
            {
                Ctx.SendError(ErrorCodes::ERR_INVALID_SURFACE_SPEC,
                    FString::Printf(TEXT("Unknown surface channel '%s'. Valid: visibility, camera, "
                        "worldstatic, worlddynamic."), *ChannelName));
                return false;
            }
        }

        // Presets first, so explicit caller lists ADD to the preset rather than being erased by
        // it, and a caller can write {"preset":"landscape","excludeNames":["FX_Haze_*"]}.
        OutSpec.ApplyPreset();

        bool bComplex = false;
        if (SurfaceObj->TryGetBoolField(TEXT("traceComplex"), bComplex)
            || SurfaceObj->TryGetBoolField(TEXT("trace_complex"), bComplex))
        {
            OutSpec.bTraceComplex = bComplex;
        }

        // Read after ApplyPreset because this one OVERRIDES the preset rather than adding to
        // it: any_solid turns the intrinsic effect-card rejection on, and a caller whose ground
        // genuinely is translucent (a glass walkway) needs a way to turn it back off. Kept in
        // step with GroundPlacement::ParseSurfaceJson, which is the canonical parser this copy
        // predates - see the integration note on that function.
        bool bExcludeEffects = false;
        if (SurfaceObj->TryGetBoolField(TEXT("excludeEffectGeometry"), bExcludeEffects)
            || SurfaceObj->TryGetBoolField(TEXT("exclude_effect_geometry"), bExcludeEffects))
        {
            OutSpec.Filter.bExcludeEffectGeometry = bExcludeEffects;
        }

        GroundRpcCollectStrings(SurfaceObj, TEXT("onlyClasses"), OutSpec.Filter.OnlyClasses);
        GroundRpcCollectStrings(SurfaceObj, TEXT("only_classes"), OutSpec.Filter.OnlyClasses);
        GroundRpcCollectStrings(SurfaceObj, TEXT("excludeClasses"), OutSpec.Filter.ExcludeClasses);
        GroundRpcCollectStrings(SurfaceObj, TEXT("exclude_classes"), OutSpec.Filter.ExcludeClasses);
        GroundRpcCollectStrings(SurfaceObj, TEXT("excludeComponentClasses"),
            OutSpec.Filter.ExcludeComponentClasses);
        GroundRpcCollectStrings(SurfaceObj, TEXT("exclude_component_classes"),
            OutSpec.Filter.ExcludeComponentClasses);
        GroundRpcCollectStrings(SurfaceObj, TEXT("excludeNames"), OutSpec.Filter.ExcludeNames);
        GroundRpcCollectStrings(SurfaceObj, TEXT("exclude_names"), OutSpec.Filter.ExcludeNames);

        TArray<FString> IgnoreNames;
        GroundRpcCollectStrings(SurfaceObj, TEXT("ignoreActors"), IgnoreNames);
        GroundRpcCollectStrings(SurfaceObj, TEXT("ignore_actors"), IgnoreNames);
        for (const FString& Name : IgnoreNames)
        {
            if (AActor* Found = McpActorUtils::FindActorByName(World, Name))
            {
                OutSpec.IgnoreActors.AddUnique(TWeakObjectPtr<AActor>(Found));
            }
            else
            {
                // Echoed rather than silently dropped: an ignore list with a typo in it is a
                // filter that is not doing what the caller believes it is doing.
                OutUnresolvedIgnores.AddUnique(Name);
            }
        }

        double MaxLayers = 0.0;
        if (SurfaceObj->TryGetNumberField(TEXT("maxLayers"), MaxLayers)
            || SurfaceObj->TryGetNumberField(TEXT("max_layers"), MaxLayers))
        {
            OutSpec.MaxLayers = FMath::Clamp(static_cast<int32>(MaxLayers), 1,
                SpatialTraceUtils::MaxAllowedLayers);
        }
        double MaxDrop = 0.0;
        if (SurfaceObj->TryGetNumberField(TEXT("maxDrop"), MaxDrop)
            || SurfaceObj->TryGetNumberField(TEXT("max_drop"), MaxDrop))
        {
            OutSpec.MaxDropCm = FMath::Max(MaxDrop, 0.0);
        }
        double ProbeLift = 0.0;
        if (SurfaceObj->TryGetNumberField(TEXT("probeLift"), ProbeLift)
            || SurfaceObj->TryGetNumberField(TEXT("probe_lift"), ProbeLift))
        {
            OutSpec.ProbeLiftCm = FMath::Max(ProbeLift, 0.0);
        }

        return true;
    }

    TSharedPtr<FJsonObject> GroundRpcSurfaceEcho(const GroundPlacement::FGroundSurfaceSpec& Spec,
                                                 const TArray<FString>& UnresolvedIgnores)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetStringField(TEXT("preset"), GroundPlacement::SurfacePresetToString(Spec.Preset));
        Obj->SetBoolField(TEXT("traceComplex"), Spec.bTraceComplex);
        Obj->SetNumberField(TEXT("maxLayers"), Spec.MaxLayers);
        Obj->SetNumberField(TEXT("maxDropCm"), Spec.MaxDropCm);
        Obj->SetNumberField(TEXT("probeLiftCm"), Spec.ProbeLiftCm);
        GroundRpcWriteStringArray(Obj, TEXT("onlyClasses"), Spec.Filter.OnlyClasses);
        GroundRpcWriteStringArray(Obj, TEXT("excludeClasses"), Spec.Filter.ExcludeClasses);
        GroundRpcWriteStringArray(Obj, TEXT("excludeComponentClasses"),
            Spec.Filter.ExcludeComponentClasses);
        GroundRpcWriteStringArray(Obj, TEXT("excludeNames"), Spec.Filter.ExcludeNames);
        GroundRpcWriteStringArray(Obj, TEXT("unresolvedIgnoreActors"), UnresolvedIgnores);
        return Obj;
    }

    // ---- Actor selection ------------------------------------------------------------------

    // Resolve the batch's target actors. Exactly one selector must be supplied; the caller is
    // never allowed to fall through to "every actor in the level" by omission.
    //
    // Returns false having sent a typed error. OutTotalMatches is the true match count before
    // limit/offset, so a truncated batch reports what it did not reach.
    //
    // OutAllMatchedLabels, when supplied, receives the label of EVERY matched actor before
    // limit/offset. Only the mutating verb asks for it, and only to name the matched set back to
    // the caller when the count disagrees with what they expected - a count alone cannot say
    // WHICH actors a pattern caught.
    bool GroundRpcResolveActors(const FHandlerContext& Ctx, UWorld* World,
                                TArray<AActor*>& OutActors, int32& OutTotalMatches,
                                TArray<FString>& OutUnresolvedNames,
                                TArray<FString>* OutAllMatchedLabels = nullptr)
    {
        const TSharedPtr<FJsonObject>& Payload = Ctx.GetRawPayload();

        TArray<FString> ExplicitNames;
        GroundRpcCollectStrings(Payload, TEXT("actors"), ExplicitNames);
        GroundRpcCollectStrings(Payload, TEXT("actorNames"), ExplicitNames);
        GroundRpcCollectStrings(Payload, TEXT("actor_names"), ExplicitNames);
        const FString Prefix = Ctx.GetString(TEXT("prefix"));
        const bool bUseSelection = Ctx.GetBool(TEXT("selection"), false);

        NameMatch::FFilter Filter;
        if (!NameMatch::Require(Ctx, TArray<FString>{TEXT("filter")}, Filter))
        {
            return false;
        }

        int32 SelectorCount = 0;
        SelectorCount += (ExplicitNames.Num() > 0) ? 1 : 0;
        SelectorCount += (!Prefix.IsEmpty()) ? 1 : 0;
        SelectorCount += bUseSelection ? 1 : 0;
        SelectorCount += Filter.IsActive() ? 1 : 0;

        if (SelectorCount == 0)
        {
            Ctx.SendError(ErrorCodes::ERR_MISSING_REQUIRED_PARAM,
                TEXT("Name the actors to operate on with exactly one of: 'actors' (array of "
                     "labels/names/paths), 'prefix' (string), 'filter' (pattern + matchMode), or "
                     "'selection': true (the current editor selection). There is no implicit "
                     "whole-level default."));
            return false;
        }
        if (SelectorCount > 1)
        {
            Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
                TEXT("Provide exactly ONE of 'actors' / 'prefix' / 'filter' / 'selection'; more "
                     "than one was given and their intersection is ambiguous."));
            return false;
        }

        TArray<AActor*> Matched;

        if (ExplicitNames.Num() > 0)
        {
            for (const FString& Name : ExplicitNames)
            {
                if (AActor* Found = McpActorUtils::FindActorByName(World, Name))
                {
                    Matched.AddUnique(Found);
                }
                else
                {
                    // A name that resolves to nothing is reported, never counted as processed.
                    OutUnresolvedNames.AddUnique(Name);
                }
            }
        }
        else if (bUseSelection)
        {
            if (GEditor)
            {
                TArray<AActor*> Selected;
                GEditor->GetSelectedActors()->GetSelectedObjects(Selected);
                for (AActor* Actor : Selected)
                {
                    if (Actor)
                    {
                        Matched.AddUnique(Actor);
                    }
                }
            }
        }
        else
        {
            // prefix / filter both walk the level once.
            for (TActorIterator<AActor> It(World); It; ++It)
            {
                AActor* Actor = *It;
                if (!Actor)
                {
                    continue;
                }
                const FString Label = Actor->GetActorLabel();
                const FString Name = Actor->GetName();
                const bool bMatches = Prefix.IsEmpty()
                    ? Filter.MatchesEither(Label, Name)
                    : (Label.StartsWith(Prefix, ESearchCase::IgnoreCase)
                        || Name.StartsWith(Prefix, ESearchCase::IgnoreCase));
                if (bMatches)
                {
                    Matched.Add(Actor);
                }
            }
        }

        OutTotalMatches = Matched.Num();
        if (OutAllMatchedLabels)
        {
            OutAllMatchedLabels->Reserve(Matched.Num());
            for (AActor* Actor : Matched)
            {
                OutAllMatchedLabels->Add(Actor->GetActorLabel());
            }
        }
        if (OutTotalMatches == 0)
        {
            Ctx.SendError(ErrorCodes::ERR_NO_ACTORS_MATCHED,
                TEXT("No actors matched the selector. An empty batch is reported as an error "
                     "rather than a zero-item success, because a typo in a prefix otherwise looks "
                     "identical to a clean run."));
            return false;
        }

        const int32 Offset = FMath::Max(Ctx.GetInt(TEXT("offset"), 0), 0);
        int32 Limit = Ctx.GetInt(TEXT("limit"), GroundRpcDefaultLimit);
        Limit = FMath::Clamp(Limit, 1, GroundRpcMaxBatch);

        for (int32 Index = Offset; Index < Matched.Num() && OutActors.Num() < Limit; ++Index)
        {
            OutActors.Add(Matched[Index]);
        }

        if (OutActors.Num() == 0)
        {
            Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
                FString::Printf(TEXT("offset %d is past the end of the %d matched actor(s)."),
                    Offset, OutTotalMatches));
            return false;
        }
        return true;
    }

    // ---- Response writers -----------------------------------------------------------------

    // The ground-provenance block moved to GroundPlacement::MakeProvenanceJson
    // (GroundPlacementUtils.h): foliage.paint now seats through the same solve and publishes the
    // same block, and two copies of that writer is how two verbs come to describe one probe
    // differently. The three verbs here still reach it through GroundRpcContactObject only.

    TSharedPtr<FJsonObject> GroundRpcContactObject(const GroundPlacement::FGroundContactReport& Report)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetBoolField(TEXT("measured"), Report.bMeasured);
        Obj->SetBoolField(TEXT("pass"), Report.bPass);
        if (!Report.bPass)
        {
            Obj->SetStringField(TEXT("failCode"), Report.FailReasonCode);
            Obj->SetStringField(TEXT("failReason"), Report.FailReason);
        }

        // Every number below is omitted when nothing was measured. A zero that is really an
        // absence is how an unmeasured actor reads as a perfectly seated one.
        if (!Report.bMeasured)
        {
            return Obj;
        }

        Obj->SetNumberField(TEXT("sampledColumns"), Report.SampledColumns);
        Obj->SetNumberField(TEXT("actorColumns"), Report.ActorColumns);
        Obj->SetNumberField(TEXT("supportedColumns"), Report.SupportedColumns);
        Obj->SetNumberField(TEXT("coverage"), Report.Coverage);
        Obj->SetNumberField(TEXT("contactPoints"), Report.ContactPoints);
        Obj->SetStringField(TEXT("undersideModel"),
            Report.UndersideModel == GroundPlacement::EUndersideModel::BoundsPlane
                ? TEXT("bounds_plane") : TEXT("mesh"));
        if (Report.bUsedBoundsPlaneFallback)
        {
            Obj->SetBoolField(TEXT("boundsPlaneFallback"), true);
        }

        // WHICH BOX every number above and below describes. coverage, contactPoints and the three
        // gap terms are all computed over this half-extent and all report cleanly about it, so
        // until it was named a caller could read `coverage: 1.00, pass: true,
        // undersideReliefCm: 0` off a footprint that was the canopy of a tree rather than its
        // trunk - and 177 of 177 correctly-planted instances were proposed for a lift with every
        // one of those fields green. `footprintSource` says where it came from: "bounds" is the
        // object's own AABB pulled in by footprintInset, "contact_radius" is a caller-stated
        // contact patch (spatial.ground_instances' contactRadius) that replaced it.
        TSharedPtr<FJsonObject> FootprintObj = MakeShared<FJsonObject>();
        FootprintObj->SetNumberField(TEXT("x"), Report.FootprintHalfExtentCm.X);
        FootprintObj->SetNumberField(TEXT("y"), Report.FootprintHalfExtentCm.Y);
        Obj->SetObjectField(TEXT("footprintHalfExtentCm"), FootprintObj);
        Obj->SetStringField(TEXT("footprintSource"),
            Report.bFootprintFromContact ? TEXT("contact_radius") : TEXT("bounds"));

        // The gap numbers only mean something when at least one column found ground.
        if (Report.SupportedColumns > 0)
        {
            Obj->SetNumberField(TEXT("maxGapCm"), Report.MaxGapCm);
            Obj->SetNumberField(TEXT("minGapCm"), Report.MinGapCm);
            // Both terms of the clearance, beside the one that gates: a caller judging a
            // borderline result needs to see how much of the number is the actor's own shape.
            // A large maxColumnClearanceCm with a small maxGapCm is a shaped actor sitting
            // correctly, not a floating one.
            Obj->SetNumberField(TEXT("maxColumnClearanceCm"), Report.MaxColumnClearanceCm);
            Obj->SetNumberField(TEXT("undersideReliefCm"), Report.UndersideReliefCm);
            Obj->SetNumberField(TEXT("penetrationCm"), Report.PenetrationCm);
            Obj->SetNumberField(TEXT("groundSpreadCm"), Report.GroundSpreadCm);
            Obj->SetObjectField(TEXT("averageNormal"), GroundRpcVectorObject(Report.AverageNormal));
            // Beside the numbers, never instead of them: this says which of the two surfaces
            // every number above describes.
            Obj->SetObjectField(TEXT("groundProvenance"),
                GroundPlacement::MakeProvenanceJson(Report));
        }
        if (Report.RejectedSurfaceActorCount > 0)
        {
            Obj->SetNumberField(TEXT("rejectedSurfaceActorCount"), Report.RejectedSurfaceActorCount);
            GroundRpcWriteStringArray(Obj, TEXT("rejectedSurfaceActors"), Report.RejectedSurfaceActors);
        }
        if (Report.bOverLandscape.IsSet())
        {
            Obj->SetBoolField(TEXT("overLandscape"), Report.bOverLandscape.GetValue());
        }
        if (Report.SeatErrorCm.IsSet())
        {
            Obj->SetNumberField(TEXT("seatErrorCm"), Report.SeatErrorCm.GetValue());
        }
        return Obj;
    }

    TSharedPtr<FJsonObject> GroundRpcSeatResultObject(const GroundPlacement::FGroundSeatResult& Result)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetStringField(TEXT("actor"), Result.ActorLabel);
        Obj->SetStringField(TEXT("path"), Result.ActorPath);
        // Both booleans are DERIVED, never assigned from a literal: moved is "a transform was
        // recorded by the code that applied one", placed is that ANDed with a post-move
        // measurement that passed.
        Obj->SetBoolField(TEXT("moved"), Result.WasMoved());
        Obj->SetBoolField(TEXT("placed"), Result.IsSeated());
        Obj->SetStringField(TEXT("status"), GroundPlacement::SeatStatusToString(Result.Status));
        if (!Result.ReasonCode.IsEmpty())
        {
            Obj->SetStringField(TEXT("reasonCode"), Result.ReasonCode);
        }
        if (!Result.Reason.IsEmpty())
        {
            Obj->SetStringField(TEXT("reason"), Result.Reason);
        }
        if (Result.WasMoved())
        {
            Obj->SetNumberField(TEXT("deltaZCm"), Result.AppliedDeltaZCm);
            Obj->SetNumberField(TEXT("embedCm"), Result.AppliedEmbedCm);
            Obj->SetObjectField(TEXT("transform"),
                GroundRpcTransformObject(Result.AppliedTransform.GetValue()));
        }
        // The pre-move transform is echoed whenever one was captured - SUCCESSES INCLUDED. "The
        // move succeeded" and "the move was intended" are different facts, and this is the only
        // undo the verb has: a batch selected by name prefix once seated six actors belonging to
        // someone else in the same level, and because every one of them counted as a success the
        // response carried nothing to put them back with. Gating recovery information on failure
        // is what made that unrecoverable.
        if (Result.PreviousTransform.IsSet())
        {
            Obj->SetObjectField(TEXT("previousTransform"),
                GroundRpcTransformObject(Result.PreviousTransform.GetValue()));
        }
        Obj->SetObjectField(TEXT("contact"), GroundRpcContactObject(Result.Contact));
        return Obj;
    }

    // "summary" | "failures" (default) | "all". Anything else is a caller error rather than a
    // silent fallback, so a typo cannot quietly halve the response.
    bool GroundRpcParseDetail(const FHandlerContext& Ctx, bool& bOutIncludeSuccesses,
                              bool& bOutIncludeFailures)
    {
        FString Detail = Ctx.GetString(TEXT("detail"), TEXT("failures")).ToLower();
        if (Detail == TEXT("summary"))
        {
            bOutIncludeSuccesses = false;
            bOutIncludeFailures = false;
            return true;
        }
        if (Detail == TEXT("failures"))
        {
            bOutIncludeSuccesses = false;
            bOutIncludeFailures = true;
            return true;
        }
        if (Detail == TEXT("all"))
        {
            bOutIncludeSuccesses = true;
            bOutIncludeFailures = true;
            return true;
        }
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
            FString::Printf(TEXT("Unknown detail '%s'. Valid: summary, failures, all."), *Detail));
        return false;
    }

    // Shared sampling params, used identically by both verbs so a verify cannot measure a
    // different footprint than the seat that produced it.
    void GroundRpcReadSampling(const FHandlerContext& Ctx, int32& OutGridSize, double& OutInset,
                               GroundPlacement::EUndersideModel& OutModel)
    {
        const int32 Samples = Ctx.GetInt(TEXT("samples"),
            Ctx.GetInt(TEXT("gridSize"),
                Ctx.GetInt(TEXT("grid_size"), GroundPlacement::DefaultGridSize)));
        OutGridSize = FMath::Clamp(Samples, GroundPlacement::MinGridSize, GroundPlacement::MaxGridSize);

        OutInset = FMath::Clamp(
            Ctx.GetNumber(TEXT("footprintInset"),
                Ctx.GetNumber(TEXT("footprint_inset"), GroundPlacement::DefaultFootprintInset)),
            0.0, 0.45);

        const FString ModelName = Ctx.GetStringFirstOf(
            {TEXT("undersideModel"), TEXT("underside_model")}, TEXT("mesh")).ToLower();
        OutModel = (ModelName == TEXT("bounds_plane") || ModelName == TEXT("boundsplane"))
            ? GroundPlacement::EUndersideModel::BoundsPlane
            : GroundPlacement::EUndersideModel::MeshProfile;
    }

    // ---- The expectedMatches pre-flight (mutating verb only) ------------------------------

    // Ceiling on how many matched labels the refusal lists. The counts are exact either way;
    // only the names are bounded, the same split the detail rows use.
    constexpr int32 GroundRpcMaxNamedMatches = 64;

    // A name pattern is not a scope. In a shared level any prefix is somebody else's prefix too,
    // and this verb MOVES what it selects: one call matched 89 actors for a caller who had
    // spawned 83, and relocated all 89. rpc-design.md section 3 states the rule that was missing
    // - a query whose result set the verb then mutates is scoped to what the caller named, and
    // the wide scope is a named opt-in. Here the opt-in is a NUMBER rather than a flag, because a
    // flag would simply have been set: the caller believed the prefix was theirs. The count they
    // state is a fact the verb can check against the level; a boolean is not.
    //
    // So: a PATTERN selector ('prefix' / 'filter') requires 'expectedMatches', and a match count
    // that disagrees with it is refused BEFORE the first move, naming what matched. 'actors' and
    // 'selection' need no bound - both already enumerate what the caller named - but honour
    // 'expectedMatches' when it is given. The non-mutating sibling spatial.verify_grounding
    // takes the same selectors and requires none of this: it is how a caller finds the number.
    bool GroundRpcConfirmMatchScope(const FHandlerContext& Ctx, bool bPatternSelector,
                                    int32 TotalMatches, const TArray<FString>& MatchedLabels)
    {
        const TOptional<int32> Expected =
            Ctx.GetIntFirstOf({TEXT("expectedMatches"), TEXT("expected_matches")});

        if (!Expected.IsSet())
        {
            if (!bPatternSelector)
            {
                return true;
            }
            Ctx.SendError(ErrorCodes::ERR_MISSING_REQUIRED_PARAM,
                FString::Printf(TEXT("'prefix' and 'filter' select by pattern, and this verb MOVES "
                    "what it selects, so one of them also requires 'expectedMatches': how many "
                    "actors you believe the pattern names. This selector matched %d. A name "
                    "pattern is not a scope in a shared level - another agent's actors can start "
                    "with your prefix, and a move that succeeds is not a move that was intended. "
                    "Run spatial.verify_grounding with the SAME selector to see what it matches "
                    "(it moves nothing), then pass expectedMatches:%d - or name the actors "
                    "outright with 'actors'."), TotalMatches, TotalMatches));
            return false;
        }

        if (Expected.GetValue() == TotalMatches)
        {
            return true;
        }

        TSharedPtr<FJsonObject> Detail = MakeShared<FJsonObject>();
        Detail->SetNumberField(TEXT("expectedMatches"), Expected.GetValue());
        Detail->SetNumberField(TEXT("totalMatches"), TotalMatches);
        TArray<FString> Named = MatchedLabels;
        const int32 NamesDropped = FMath::Max(Named.Num() - GroundRpcMaxNamedMatches, 0);
        if (NamesDropped > 0)
        {
            Named.SetNum(GroundRpcMaxNamedMatches);
        }
        GroundRpcWriteStringArray(Detail, TEXT("matchedActors"), Named);
        if (NamesDropped > 0)
        {
            Detail->SetNumberField(TEXT("matchedActorsDropped"), NamesDropped);
        }
        Ctx.SendError(ErrorCodes::ERR_MATCH_COUNT_MISMATCH,
            FString::Printf(TEXT("The selector matched %d actor(s), but expectedMatches says %d. "
                "NOTHING WAS MOVED. matchedActors lists what the selector caught, so the "
                "difference can be read rather than guessed at."),
                TotalMatches, Expected.GetValue()),
            Detail);
        return false;
    }
}

// ================= spatial.ground_actors =================

REGISTER_RPC_HANDLER("spatial.ground_actors", "spatial",
    "Seat a BATCH of actors on the ground and report, per actor, whether it actually happened. "
    "Samples each actor's own underside across its footprint (not one ray at the pivot), finds "
    "the ground under each sampled column, seats the actor on the column chosen by "
    "seatPercentile, sinks it a further embed depth so it beds into the terrain instead of "
    "balancing on it, then RE-MEASURES the world and only reports placed:true when that second "
    "measurement agrees with the solve. An actor with no ground beneath it, with a footprint "
    "that overhangs the terrain edge, or whose only hits were rejected by the surface filter is "
    "reported unplaced with a reason and is NOT counted as placed. "
    "An ISM/HISM scatter HOLDER is refused outright with HOLDER_NOT_SEATABLE, naming the "
    "component: its bounds are the union of every instance, so seating it would move the whole "
    "scatter with one transform. Ground the instances individually instead. "
    "`surface` is REQUIRED - state what counts as ground; the verb will not guess. "
    "Selecting by 'prefix' or 'filter' additionally REQUIRES 'expectedMatches', and a match count "
    "that disagrees with it is refused before anything moves: a name pattern is not a scope in a "
    "shared level. Every actor this verb moves is listed in movedActors[] with its pre-move "
    "transform, at every detail level - that is the undo, and it is echoed for successes too. "
    "Coordinates are unreal units (cm), left-handed (+X fwd, +Y right, +Z up).",
    RPC_PARAMS(
        RPC_PARAM_REQ("surface", "object",
            "REQUIRED. What counts as ground: {preset, channel?, traceComplex?, "
            "excludeEffectGeometry?, onlyClasses?, excludeClasses?, excludeComponentClasses?, "
            "excludeNames?, ignoreActors?, "
            "maxLayers?, maxDrop?, probeLift?}. "
            "preset is 'landscape' (only LandscapeProxy - the right answer for terrain, because a "
            "landscape heightfield is single-valued per column so nothing can shadow it), "
            "'any_solid' (anything blocking except foliage and effect geometry - haze, fog, "
            "glow and decal cards, detected from component class and material blend mode, not from "
            "any naming convention), or 'custom' (your filters only). Explicit lists ADD to the "
            "preset; excludeEffectGeometry OVERRIDES it, so set it false when your ground really is "
            "translucent. excludeClasses names ACTOR classes and excludeComponentClasses names the "
            "class of the COMPONENT that answered the probe - the only axis that can name a scatter "
            "carried as ISM/HISM components on an ordinary actor, since no actor class "
            "distinguishes such a holder. any_solid already excludes the two engine vegetation "
            "component classes (Foliage/Grass InstancedStaticMeshComponent) but NOT plain "
            "ISM/HISM, which is legitimate ground for paving, rocks and modular tiles; pass "
            "excludeComponentClasses:['HierarchicalInstancedStaticMeshComponent'] when yours is "
            "not. traceComplex defaults false "
            "and should stay false: complex collision for a StaticMesh is its render triangle "
            "soup, so a collisionless tree or haze card BLOCKS a complex probe and becomes 'the "
            "ground'. There is no default surface - a wrong one is the cause of every floating-prop "
            "bug this verb exists to prevent."),
        FParamSpec{TEXT("actors"), TEXT("array"),
            TEXT("Explicit actor labels / internal names / paths. Provide exactly one of actors / "
                 "prefix / filter / selection. Names that do not resolve are echoed in "
                 "unresolvedActors and never counted as placed."),
            false, TEXT(""), TArray<FString>({TEXT("actorNames"), TEXT("actor_names")})},
        RPC_PARAM_OPT("prefix", "string",
            "Match actors whose display label or internal name starts with this (case-insensitive). "
            "Provide exactly one of actors / prefix / filter / selection. A pattern selector on "
            "this MUTATING verb also requires expectedMatches - a prefix is not a scope in a "
            "shared level."),
        RPC_PARAM_OPT("filter", "string",
            "Match actors by display label or internal name using the shared pattern vocabulary. "
            "Provide exactly one of actors / prefix / filter / selection. A pattern selector on "
            "this MUTATING verb also requires expectedMatches."),
        NameMatch::MatchModeParam(TEXT("filter")),
        NameMatch::CaseSensitiveParam(TEXT("filter")),
        RPC_PARAM_OPT("selection", "boolean",
            "Operate on the current editor selection. Provide exactly one of actors / prefix / "
            "filter / selection."),
        FParamSpec{TEXT("expectedMatches"), TEXT("number"),
            TEXT("REQUIRED with 'prefix' or 'filter': how many actors you believe the pattern "
                 "names. When the selector matches a different number the call is refused with "
                 "MATCH_COUNT_MISMATCH BEFORE anything moves, and the refusal lists what it "
                 "matched so you can see the difference. This exists because a name pattern is "
                 "not a scope in a shared level - another agent's actors can start with your "
                 "prefix, and this verb moves what it selects. Find the number with "
                 "spatial.verify_grounding, which takes the same selectors and moves nothing. "
                 "Optional with 'actors' / 'selection', which already enumerate what you named, "
                 "but honoured there too. Counts the full match set, so it is the same value on "
                 "every page of a limit/offset walk."),
            false, TEXT(""), TArray<FString>({TEXT("expected_matches")})},
        GroundRpcParam(TEXT("samples"), TEXT("number"),
            TEXT("Footprint sampling grid per axis: 3 means a 3x3 = 9 column grid over the actor's "
                 "bounds. 1 degrades to a single centre column (the old pivot-probe behaviour, and "
                 "the reason a boulder ends up balanced on one point). Clamped to 1-9."),
            TEXT("3"), TArray<FString>({TEXT("gridSize"), TEXT("grid_size")})),
        GroundRpcParam(TEXT("footprintInset"), TEXT("number"),
            TEXT("Fraction of the footprint half-extent that samples are pulled inward from the "
                 "bounds edge (0-0.45). An AABB corner over a rounded rock is empty air."),
            TEXT("0.1"), TArray<FString>({TEXT("footprint_inset")})),
        GroundRpcParam(TEXT("undersideModel"), TEXT("string"),
            TEXT("'mesh' (default) traces each column against the actor's own geometry, so the "
                 "solve knows the real shape of the bottom of the mesh. 'bounds_plane' models the "
                 "underside as a flat plane at the bounds minimum - cheaper, and wrong for anything "
                 "whose bottom is not flat."),
            TEXT("mesh"), TArray<FString>({TEXT("underside_model")})),
        GroundRpcParam(TEXT("seatPercentile"), TEXT("number"),
            TEXT("Which sampled column the actor comes to rest against, as a percentile over the "
                 "columns' clearances. 0 (default) rests on the FIRST contact - nothing is buried, "
                 "and on uneven ground exactly one column touches. 1 sinks until NO column floats, "
                 "at the cost of burying the high side. 0.5 is the median. Clamped to 0-1."),
            TEXT("0"), TArray<FString>({TEXT("seat_percentile")})),
        GroundRpcParam(TEXT("embedFraction"), TEXT("number"),
            TEXT("Sink this fraction of the actor's bounds HEIGHT into the ground after the seat "
                 "solve. Non-zero by default on purpose: an object resting exactly tangent to "
                 "terrain reads as balanced rather than placed. Set 0 for a pure tangent rest."),
            TEXT("0.02"), TArray<FString>({TEXT("embed_fraction")})),
        GroundRpcParam(TEXT("embedDepth"), TEXT("number"),
            TEXT("Absolute extra sink in cm, added to embedFraction's contribution."),
            TEXT("0"), TArray<FString>({TEXT("embed_depth")})),
        GroundRpcParam(TEXT("alignToSurface"), TEXT("boolean"),
            TEXT("Tilt the actor's +Z toward the AVERAGE of the sampled ground normals (more stable "
                 "than any single normal), clamped by maxTilt."),
            TEXT("false"), TArray<FString>({TEXT("align_to_surface"), TEXT("alignToNormal")})),
        GroundRpcParam(TEXT("maxTilt"), TEXT("number"),
            TEXT("Ceiling in degrees on the alignToSurface tilt away from world up. Terrain normals "
                 "on a cliff face approach horizontal; following one lays a prop on its side."),
            TEXT("20"), TArray<FString>({TEXT("max_tilt"), TEXT("maxTiltDegrees")})),
        GroundRpcParam(TEXT("minCoverage"), TEXT("number"),
            TEXT("Fraction of the actor's own footprint columns that must find ground, or the actor "
                 "is reported PARTIAL_GROUND_COVERAGE and left unplaced. This is what catches a "
                 "prop overhanging a hole or the terrain edge."),
            TEXT("0.5"), TArray<FString>({TEXT("min_coverage")})),
        GroundRpcParam(TEXT("minContactPoints"), TEXT("number"),
            TEXT("Minimum footprint columns that must touch the surface after seating."),
            TEXT("1"), TArray<FString>({TEXT("min_contact_points")})),
        GroundRpcParam(TEXT("contactTolerance"), TEXT("number"),
            TEXT("A column counts as touching when its clearance is at or below this many cm. "
                 "Buried counts as touching; floating does not."),
            TEXT("2"), TArray<FString>({TEXT("contact_tolerance")})),
        GroundRpcParam(TEXT("maxSeatError"), TEXT("number"),
            TEXT("Post-move readback tolerance in cm. Every column's measured clearance must match "
                 "what the solve predicted within this much, or the actor is reported "
                 "GROUND_SEAT_READBACK_MISMATCH and NOT counted as placed."),
            TEXT("1"), TArray<FString>({TEXT("max_seat_error")})),
        GroundRpcParam(TEXT("revertOnFailure"), TEXT("boolean"),
            TEXT("Move an actor back to its original transform when its post-move verification "
                 "fails. Off by default; previousTransform is echoed for every actor that was "
                 "moved either way, successes included, and movedActors[] carries the same "
                 "record at every detail level."),
            TEXT("false"), TArray<FString>({TEXT("revert_on_failure")})),
        RPC_PARAM_DEF("limit", "number",
            "Maximum actors to process in this call (1-5000). Pair with offset to page a larger "
            "set; totalMatches always reports the true match count.", "512"),
        RPC_PARAM_DEF("offset", "number", "Index into the matched set to start from.", "0"),
        RPC_PARAM_DEF("detail", "string",
            "'summary' (counts only), 'failures' (default: counts plus a full row per failed "
            "actor), or 'all' (a row per actor). Row arrays are capped; counts never are. "
            "movedActors[] - the pre-move transform of every actor this call moved - is NOT "
            "governed by detail: it is the receipt for a mutation, not a diagnostic.", "failures")
    ))
{
    UWorld* World = GroundRpcResolveWorld();
    if (!World)
    {
        Ctx.SendError(ErrorCodes::ERR_EDITOR_WORLD_NOT_AVAILABLE,
            TEXT("No editor/PIE world available for ground placement."));
        return true;
    }

    GroundPlacement::FGroundSurfaceSpec Surface;
    TArray<FString> UnresolvedIgnores;
    if (!GroundRpcParseSurface(Ctx, World, Surface, UnresolvedIgnores))
    {
        return true;
    }

    bool bIncludeSuccesses = false;
    bool bIncludeFailures = true;
    if (!GroundRpcParseDetail(Ctx, bIncludeSuccesses, bIncludeFailures))
    {
        return true;
    }

    TArray<AActor*> Actors;
    TArray<FString> UnresolvedActors;
    TArray<FString> MatchedLabels;
    int32 TotalMatches = 0;
    if (!GroundRpcResolveActors(Ctx, World, Actors, TotalMatches, UnresolvedActors, &MatchedLabels))
    {
        return true;
    }

    // The scope pre-flight runs after resolution (so a typo'd pattern still reads as
    // NO_ACTORS_MATCHED) and before the first move (so a selector that caught the wrong actors
    // costs nothing). 'filter' is active exactly when its pattern string is non-empty, which is
    // the same condition NameMatch::FFilter::IsActive() tests.
    const bool bPatternSelector = !Ctx.GetString(TEXT("prefix")).IsEmpty()
        || !Ctx.GetString(TEXT("filter")).IsEmpty();
    if (!GroundRpcConfirmMatchScope(Ctx, bPatternSelector, TotalMatches, MatchedLabels))
    {
        return true;
    }

    GroundPlacement::FGroundSeatConfig Config;
    GroundRpcReadSampling(Ctx, Config.GridSize, Config.FootprintInset, Config.UndersideModel);
    Config.SeatPercentile = FMath::Clamp(
        Ctx.GetNumber(TEXT("seatPercentile"), Ctx.GetNumber(TEXT("seat_percentile"), 0.0)), 0.0, 1.0);
    Config.EmbedFraction = FMath::Max(
        Ctx.GetNumber(TEXT("embedFraction"),
            Ctx.GetNumber(TEXT("embed_fraction"), GroundPlacement::DefaultEmbedFraction)), 0.0);
    Config.EmbedDepthCm = FMath::Max(
        Ctx.GetNumber(TEXT("embedDepth"), Ctx.GetNumber(TEXT("embed_depth"), 0.0)), 0.0);
    Config.bAlignToSurface = Ctx.GetBoolFirstOf(
        {TEXT("alignToSurface"), TEXT("align_to_surface"), TEXT("alignToNormal")}, false);
    Config.MaxTiltDegrees = FMath::Clamp(
        Ctx.GetNumber(TEXT("maxTilt"),
            Ctx.GetNumber(TEXT("max_tilt"),
                Ctx.GetNumber(TEXT("maxTiltDegrees"), GroundPlacement::DefaultMaxTiltDegrees))),
        0.0, 89.0);
    Config.MaxSeatErrorCm = FMath::Max(
        Ctx.GetNumber(TEXT("maxSeatError"), Ctx.GetNumber(TEXT("max_seat_error"), 1.0)), 0.0);
    Config.bRevertOnFailure =
        Ctx.GetBoolFirstOf({TEXT("revertOnFailure"), TEXT("revert_on_failure")}, false);

    Config.Thresholds.MinCoverage = FMath::Clamp(
        Ctx.GetNumber(TEXT("minCoverage"),
            Ctx.GetNumber(TEXT("min_coverage"), GroundPlacement::DefaultMinCoverage)), 0.0, 1.0);
    Config.Thresholds.MinContactPoints = FMath::Max(
        Ctx.GetInt(TEXT("minContactPoints"), Ctx.GetInt(TEXT("min_contact_points"), 1)), 0);
    Config.Thresholds.ContactToleranceCm = FMath::Max(
        Ctx.GetNumber(TEXT("contactTolerance"),
            Ctx.GetNumber(TEXT("contact_tolerance"), GroundPlacement::DefaultContactToleranceCm)), 0.0);

    // ---- Run the batch ----
    TArray<TSharedPtr<FJsonValue>> Rows;
    TArray<TSharedPtr<FJsonValue>> MovedRows;
    int32 PlacedCount = 0;
    int32 MovedCount = 0;
    int32 FailedCount = 0;
    int32 DetailRowsDropped = 0;

    for (AActor* Actor : Actors)
    {
        const GroundPlacement::FGroundSeatResult Result =
            GroundPlacement::SeatActor(World, Actor, Surface, Config);

        // Counters are derived from the result's own predicates, one actor at a time. There is
        // no separate "success" bookkeeping that could drift from what was reported per actor.
        const bool bPlaced = Result.IsSeated();
        PlacedCount += bPlaced ? 1 : 0;
        FailedCount += bPlaced ? 0 : 1;
        MovedCount += Result.WasMoved() ? 1 : 0;

        // The undo record, built from the same predicate the `moved` counter is built from. A
        // reverted actor is deliberately absent: WasMoved() is false once the revert clears
        // AppliedTransform, and the actor is already back where it started.
        if (Result.WasMoved() && Result.PreviousTransform.IsSet())
        {
            TSharedPtr<FJsonObject> Undo = MakeShared<FJsonObject>();
            Undo->SetStringField(TEXT("actor"), Result.ActorLabel);
            Undo->SetStringField(TEXT("path"), Result.ActorPath);
            Undo->SetObjectField(TEXT("previousTransform"),
                GroundRpcTransformObject(Result.PreviousTransform.GetValue()));
            MovedRows.Add(MakeShared<FJsonValueObject>(Undo));
        }

        const bool bWantRow = bPlaced ? bIncludeSuccesses : bIncludeFailures;
        if (bWantRow)
        {
            if (Rows.Num() < GroundRpcMaxDetailRows)
            {
                Rows.Add(MakeShared<FJsonValueObject>(GroundRpcSeatResultObject(Result)));
            }
            else
            {
                ++DetailRowsDropped;
            }
        }
    }

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetNumberField(TEXT("requested"), Actors.Num());
    Data->SetNumberField(TEXT("placed"), PlacedCount);
    Data->SetNumberField(TEXT("failed"), FailedCount);
    Data->SetNumberField(TEXT("moved"), MovedCount);
    Data->SetNumberField(TEXT("totalMatches"), TotalMatches);
    Data->SetBoolField(TEXT("truncated"), Actors.Num() < TotalMatches);
    Data->SetObjectField(TEXT("surface"), GroundRpcSurfaceEcho(Surface, UnresolvedIgnores));

    TSharedPtr<FJsonObject> SeatEcho = MakeShared<FJsonObject>();
    SeatEcho->SetNumberField(TEXT("samples"), Config.GridSize);
    SeatEcho->SetNumberField(TEXT("footprintInset"), Config.FootprintInset);
    SeatEcho->SetNumberField(TEXT("seatPercentile"), Config.SeatPercentile);
    SeatEcho->SetNumberField(TEXT("embedFraction"), Config.EmbedFraction);
    SeatEcho->SetNumberField(TEXT("embedDepthCm"), Config.EmbedDepthCm);
    SeatEcho->SetBoolField(TEXT("alignToSurface"), Config.bAlignToSurface);
    SeatEcho->SetNumberField(TEXT("maxTiltDegrees"), Config.MaxTiltDegrees);
    SeatEcho->SetNumberField(TEXT("maxSeatErrorCm"), Config.MaxSeatErrorCm);
    SeatEcho->SetStringField(TEXT("undersideModel"),
        Config.UndersideModel == GroundPlacement::EUndersideModel::BoundsPlane
            ? TEXT("bounds_plane") : TEXT("mesh"));
    Data->SetObjectField(TEXT("seat"), SeatEcho);

    // The undo log, written before results[] because it is the more important of the two and is
    // deliberately NOT governed by `detail`: `detail:"summary"` is exactly what hid six foreign
    // actors' original transforms until they were unrecoverable. It is also not capped
    // separately - one entry per actor actually moved, and `limit` is what bounds both the
    // mutation and this record of it.
    if (MovedRows.Num() > 0)
    {
        Data->SetArrayField(TEXT("movedActors"), MovedRows);
    }
    if (Rows.Num() > 0)
    {
        Data->SetArrayField(TEXT("results"), Rows);
    }
    if (DetailRowsDropped > 0)
    {
        Data->SetBoolField(TEXT("resultsTruncated"), true);
        Data->SetNumberField(TEXT("resultsDropped"), DetailRowsDropped);
    }
    GroundRpcWriteStringArray(Data, TEXT("unresolvedActors"), UnresolvedActors);
    GroundRpcAddAxisEcho(Data);

    UE_LOG(LogPinWrightSubsystem, Display,
        TEXT("spatial.ground_actors: seated %d/%d actor(s) on '%s' (%d moved, %d failed)"),
        PlacedCount, Actors.Num(), GroundPlacement::SurfacePresetToString(Surface.Preset),
        MovedCount, FailedCount);

    Ctx.SendSuccess(Data);
    return true;
}

// ================= spatial.verify_grounding =================

REGISTER_RPC_HANDLER("spatial.verify_grounding", "spatial",
    "Measure ground-contact quality for a BATCH of actors. Non-mutating. For each actor it "
    "samples the actor's own underside across its footprint, probes the stated surface under "
    "each column, and reports coverage (how much of the footprint has ground under it), "
    "contactPoints (how many columns actually touch), maxGap (how far the actor's LOWEST point "
    "sits above the lowest ground under it - overhanging or curved geometry higher up is shape "
    "and does not count, and maxColumnClearance / undersideRelief report that separately), "
    "penetration (how deep the deepest part is buried) and groundSpread, plus a derived pass. "
    "Unlike spatial.verify_placement's single-ray grounded check, this sees an actor SUNK into a "
    "surface (a bottom-up ray cannot report a negative gap), an actor balanced on one point, and "
    "an actor overhanging the terrain edge. An ISM/HISM scatter HOLDER is refused with "
    "HOLDER_NOT_SEATABLE, naming the component, rather than judged: its bounds are the union of "
    "every instance, so a verdict about it would describe nothing. spatial.ground_actors refuses "
    "the same actor with the same code. `surface` is REQUIRED. "
    "Coordinates are unreal units (cm), left-handed (+X fwd, +Y right, +Z up).",
    RPC_PARAMS(
        RPC_PARAM_REQ("surface", "object",
            "REQUIRED. What counts as ground - identical shape and defaults to "
            "spatial.ground_actors' surface argument. Verify with the SAME surface you seated "
            "with, or the two answer different questions."),
        FParamSpec{TEXT("actors"), TEXT("array"),
            TEXT("Explicit actor labels / internal names / paths. Provide exactly one of actors / "
                 "prefix / filter / selection."),
            false, TEXT(""), TArray<FString>({TEXT("actorNames"), TEXT("actor_names")})},
        RPC_PARAM_OPT("prefix", "string",
            "Match actors whose display label or internal name starts with this (case-insensitive)."),
        RPC_PARAM_OPT("filter", "string",
            "Match actors by display label or internal name using the shared pattern vocabulary."),
        NameMatch::MatchModeParam(TEXT("filter")),
        NameMatch::CaseSensitiveParam(TEXT("filter")),
        RPC_PARAM_OPT("selection", "boolean", "Verify the current editor selection."),
        GroundRpcParam(TEXT("samples"), TEXT("number"),
            TEXT("Footprint sampling grid per axis (1-9); 3 means a 3x3 = 9 column grid."),
            TEXT("3"), TArray<FString>({TEXT("gridSize"), TEXT("grid_size")})),
        GroundRpcParam(TEXT("footprintInset"), TEXT("number"),
            TEXT("Fraction of the footprint half-extent that samples are pulled inward (0-0.45)."),
            TEXT("0.1"), TArray<FString>({TEXT("footprint_inset")})),
        GroundRpcParam(TEXT("undersideModel"), TEXT("string"),
            TEXT("'mesh' (default) or 'bounds_plane'. Must match what the seat used to be "
                 "comparable."),
            TEXT("mesh"), TArray<FString>({TEXT("underside_model")})),
        GroundRpcParam(TEXT("maxGap"), TEXT("number"),
            TEXT("Largest allowed clearance in cm between the actor's LOWEST underside sample "
                 "and the lowest ground under its footprint. This is the floating check. It is "
                 "deliberately NOT a bound on the silhouette: a capital wider than its base or a "
                 "cylinder on its flank has a large clearance far from where it rests, and that "
                 "is shape, not float - it is reported as maxColumnClearanceCm / "
                 "undersideReliefCm and never fails an actor."),
            TEXT("2"), TArray<FString>({TEXT("max_gap")})),
        GroundRpcParam(TEXT("maxPenetration"), TEXT("number"),
            TEXT("Deepest allowed burial in cm. Raise it to at least the embed depth when "
                 "verifying actors that were deliberately bedded in."),
            TEXT("2"), TArray<FString>({TEXT("max_penetration")})),
        GroundRpcParam(TEXT("minCoverage"), TEXT("number"),
            TEXT("Fraction of the actor's footprint columns that must have ground under them."),
            TEXT("0.5"), TArray<FString>({TEXT("min_coverage")})),
        GroundRpcParam(TEXT("minContactPoints"), TEXT("number"),
            TEXT("Minimum columns that must touch. Raise it above 1 for wide actors: one contact "
                 "point is a physically valid rest and a visually balanced result."),
            TEXT("1"), TArray<FString>({TEXT("min_contact_points")})),
        GroundRpcParam(TEXT("contactTolerance"), TEXT("number"),
            TEXT("A column counts as touching at or below this clearance in cm."),
            TEXT("2"), TArray<FString>({TEXT("contact_tolerance")})),
        RPC_PARAM_DEF("limit", "number", "Maximum actors to measure in this call (1-5000).", "512"),
        RPC_PARAM_DEF("offset", "number", "Index into the matched set to start from.", "0"),
        RPC_PARAM_DEF("detail", "string",
            "'summary', 'failures' (default), or 'all'.", "failures")
    ))
{
    UWorld* World = GroundRpcResolveWorld();
    if (!World)
    {
        Ctx.SendError(ErrorCodes::ERR_EDITOR_WORLD_NOT_AVAILABLE,
            TEXT("No editor/PIE world available for the grounding check."));
        return true;
    }

    GroundPlacement::FGroundSurfaceSpec Surface;
    TArray<FString> UnresolvedIgnores;
    if (!GroundRpcParseSurface(Ctx, World, Surface, UnresolvedIgnores))
    {
        return true;
    }

    bool bIncludeSuccesses = false;
    bool bIncludeFailures = true;
    if (!GroundRpcParseDetail(Ctx, bIncludeSuccesses, bIncludeFailures))
    {
        return true;
    }

    TArray<AActor*> Actors;
    TArray<FString> UnresolvedActors;
    int32 TotalMatches = 0;
    if (!GroundRpcResolveActors(Ctx, World, Actors, TotalMatches, UnresolvedActors))
    {
        return true;
    }

    int32 GridSize = GroundPlacement::DefaultGridSize;
    double Inset = GroundPlacement::DefaultFootprintInset;
    GroundPlacement::EUndersideModel Model = GroundPlacement::EUndersideModel::MeshProfile;
    GroundRpcReadSampling(Ctx, GridSize, Inset, Model);

    GroundPlacement::FContactThresholds Thresholds;
    Thresholds.MaxGapCm = FMath::Max(
        Ctx.GetNumber(TEXT("maxGap"),
            Ctx.GetNumber(TEXT("max_gap"), GroundPlacement::DefaultContactToleranceCm)), 0.0);
    Thresholds.MaxPenetrationCm = FMath::Max(
        Ctx.GetNumber(TEXT("maxPenetration"),
            Ctx.GetNumber(TEXT("max_penetration"), GroundPlacement::DefaultContactToleranceCm)), 0.0);
    Thresholds.MinCoverage = FMath::Clamp(
        Ctx.GetNumber(TEXT("minCoverage"),
            Ctx.GetNumber(TEXT("min_coverage"), GroundPlacement::DefaultMinCoverage)), 0.0, 1.0);
    Thresholds.MinContactPoints = FMath::Max(
        Ctx.GetInt(TEXT("minContactPoints"), Ctx.GetInt(TEXT("min_contact_points"), 1)), 0);
    Thresholds.ContactToleranceCm = FMath::Max(
        Ctx.GetNumber(TEXT("contactTolerance"),
            Ctx.GetNumber(TEXT("contact_tolerance"), GroundPlacement::DefaultContactToleranceCm)), 0.0);
    // This verb asks the absolute question, so the gap bounds ARE the criteria here.
    Thresholds.bEnforceGapBounds = true;

    TArray<TSharedPtr<FJsonValue>> Rows;
    int32 PassCount = 0;
    int32 FailCount = 0;
    int32 DetailRowsDropped = 0;

    for (AActor* Actor : Actors)
    {
        const GroundPlacement::FGroundContactReport Report = GroundPlacement::MeasureContact(
            World, Actor, Surface, GridSize, Inset, Model, Thresholds, nullptr);

        PassCount += Report.bPass ? 1 : 0;
        FailCount += Report.bPass ? 0 : 1;

        const bool bWantRow = Report.bPass ? bIncludeSuccesses : bIncludeFailures;
        if (bWantRow)
        {
            if (Rows.Num() < GroundRpcMaxDetailRows)
            {
                TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
                Row->SetStringField(TEXT("actor"), Actor ? Actor->GetActorLabel() : FString());
                Row->SetStringField(TEXT("path"), Actor ? Actor->GetPathName() : FString());
                Row->SetBoolField(TEXT("pass"), Report.bPass);
                Row->SetObjectField(TEXT("contact"), GroundRpcContactObject(Report));
                Rows.Add(MakeShared<FJsonValueObject>(Row));
            }
            else
            {
                ++DetailRowsDropped;
            }
        }
    }

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    // The batch-level pass is the AND over every actor MEASURED IN THIS CALL. It is deliberately
    // false for a truncated batch's unreached actors by way of `truncated` - a caller reading
    // pass alone on a paged run is reading a partial answer, and truncated says so.
    Data->SetBoolField(TEXT("pass"), FailCount == 0);
    Data->SetNumberField(TEXT("checked"), Actors.Num());
    Data->SetNumberField(TEXT("passed"), PassCount);
    Data->SetNumberField(TEXT("failed"), FailCount);
    Data->SetNumberField(TEXT("totalMatches"), TotalMatches);
    Data->SetBoolField(TEXT("truncated"), Actors.Num() < TotalMatches);
    Data->SetObjectField(TEXT("surface"), GroundRpcSurfaceEcho(Surface, UnresolvedIgnores));

    TSharedPtr<FJsonObject> Criteria = MakeShared<FJsonObject>();
    Criteria->SetNumberField(TEXT("samples"), GridSize);
    Criteria->SetNumberField(TEXT("footprintInset"), Inset);
    Criteria->SetNumberField(TEXT("maxGapCm"), Thresholds.MaxGapCm);
    Criteria->SetNumberField(TEXT("maxPenetrationCm"), Thresholds.MaxPenetrationCm);
    Criteria->SetNumberField(TEXT("minCoverage"), Thresholds.MinCoverage);
    Criteria->SetNumberField(TEXT("minContactPoints"), Thresholds.MinContactPoints);
    Criteria->SetNumberField(TEXT("contactToleranceCm"), Thresholds.ContactToleranceCm);
    Criteria->SetStringField(TEXT("undersideModel"),
        Model == GroundPlacement::EUndersideModel::BoundsPlane ? TEXT("bounds_plane") : TEXT("mesh"));
    Data->SetObjectField(TEXT("criteria"), Criteria);

    if (Rows.Num() > 0)
    {
        Data->SetArrayField(TEXT("results"), Rows);
    }
    if (DetailRowsDropped > 0)
    {
        Data->SetBoolField(TEXT("resultsTruncated"), true);
        Data->SetNumberField(TEXT("resultsDropped"), DetailRowsDropped);
    }
    GroundRpcWriteStringArray(Data, TEXT("unresolvedActors"), UnresolvedActors);
    GroundRpcAddAxisEcho(Data);

    Ctx.SendSuccess(Data);
    return true;
}

// ================= spatial.ground_instances =================
//
// The per-instance half of spatial.ground_actors, and the verb its HOLDER_NOT_SEATABLE refusal
// now points at. Same solver, same required `surface`, same "placed is a post-move measurement
// that passed" contract - only the footprint source and the write differ, and both differences are
// what make a scatter addressable at all (see GroundPlacement::SeatInstance).
//
// It is deliberately NOT split into a verify/apply pair the way spatial.ground_actors is split
// from spatial.verify_grounding. That split exists because a NAME PATTERN selector on a mutating
// verb needs an expectedMatches guard and a non-mutating sibling to find the number with.
// Instances are addressed by explicit index against one named component on one named actor, so
// there is no pattern hazard to guard - and apply:false covers the verify half by running the
// identical solve and reporting the move it would have made.
//
// "ONE NAMED COMPONENT" IS THE PREMISE, AND IT USED NOT TO HOLD. `component` was optional and
// resolved to whichever component on the actor carried the most instances. That inference IS the
// pattern hazard wearing a different name: it reads mutable level-wide state, and on
// AInstancedFoliageActor - one component per foliage type, for every caller in the level - it
// routinely named someone else's scatter. Three incidents in one session relocated 2,048 instances
// of other callers' content. InstancedMeshUtils::ResolveInstancedComponent now REFUSES an omitted
// component whenever the actor carries more than one, before the surface is probed and before the
// first move, and the refusal names every candidate with its instance count. `expectedCount` below
// covers what that guard cannot: a one-component actor, or an explicitly named component whose
// name was reassigned to a different foliage type between the read and this call.

REGISTER_RPC_HANDLER("spatial.ground_instances", "spatial",
    "Seat individual ISM/HISM INSTANCES on the ground and report, per instance, whether it actually "
    "happened. This is what spatial.ground_actors refuses to do for a scatter holder: that actor's "
    "bounds are the union of every instance, so seating it would solve against the whole scatter "
    "and relocate all of it with one transform. Here each instance is measured on its OWN footprint "
    "- the component's mesh bounds under that instance's transform - seated on the column chosen by "
    "seatPercentile, sunk a further embed depth, and then RE-MEASURED; placed:true means the second "
    "measurement agreed with the solve. "
    "That footprint is the instance's BOUNDING BOX, which for anything with a trunk, a stem or a "
    "pedestal is its canopy rather than its contact patch: pass contactRadius (mesh-local cm) to "
    "sample the contact patch instead, and read contact.footprintHalfExtentCm on each row for the "
    "box that was actually sampled - coverage, contactPoints and every gap number are computed "
    "over it, so they are green about the box whether or not it is the object. "
    "`surface` is REQUIRED - state what counts as ground; the verb will not guess. "
    "apply:false runs the identical solve and writes nothing, reporting proposedDeltaZCm per "
    "instance: that is the dry run, and it is why there is no separate verify verb. "
    "Every instance this verb moves is listed in movedInstances[] with its pre-move transform and "
    "space:\"world\", at every detail level - that is the undo, and actor.set_instance_transforms "
    "writes it back verbatim, adopting the rows' stated space rather than assuming its own default "
    "agrees. The apply path is also wrapped in one editor transaction, so editor.undo reaches it. "
    "The scatter's holder actor is excluded from the ground probe, so an instance is "
    "never seated onto a sibling instance. "
    "Coordinates are unreal units (cm), left-handed (+X fwd, +Y right, +Z up).",
    RPC_PARAMS(
        RPC_PARAM_REQ("surface", "object",
            "REQUIRED. What counts as ground - identical shape and defaults to "
            "spatial.ground_actors' surface argument: {preset, channel?, traceComplex?, "
            "excludeEffectGeometry?, onlyClasses?, excludeClasses?, excludeComponentClasses?, "
            "excludeNames?, ignoreActors?, "
            "maxLayers?, maxDrop?, probeLift?}. preset is 'landscape', 'any_solid' or 'custom'. "
            "excludeComponentClasses is worth knowing here in particular: this verb seats "
            "instances against neighbouring scatters, and a plain ISM/HISM neighbour is nameable "
            "on no other axis. "
            "There is no default surface - a wrong one is the cause of every floating-prop bug "
            "these verbs exist to prevent."),
        ActorNameParamUtils::ActorNameParamReq(TEXT("string"),
            TEXT("Display label, internal name or path of the actor carrying the scatter.")),
        FParamSpec{TEXT("component"), TEXT("string"),
            TEXT("Object name of the InstancedStaticMesh/HierarchicalInstancedStaticMesh component "
                 "to seat (case-insensitive). Omit ONLY when the actor carries exactly one; with "
                 "several the call is refused with AMBIGUOUS_INSTANCED_COMPONENT before the "
                 "surface is probed and before anything moves, and the refusal lists every "
                 "candidate with its instance count so you can name the one you meant. There is no "
                 "largest-wins default: on a shared holder such as InstancedFoliageActor - one "
                 "component per foliage type, for every caller in the level - the biggest "
                 "component is routinely another caller's scatter. spatial.ground_actors names the "
                 "component in its HOLDER_NOT_SEATABLE refusal; actor.get_components lists them."),
            false, TEXT(""), TArray<FString>({TEXT("componentName"), TEXT("component_name")})},
        RPC_PARAM_OPT("indices", "array",
            "Specific instance indices to seat. Omit to walk the whole scatter under limit/offset. "
            "An index outside 0..instanceCount-1 refuses the call rather than being skipped."),
        FParamSpec{TEXT("expectedCount"), TEXT("integer"),
            TEXT("How many instances you believe the component carries. When it disagrees the call "
                 "is refused with MATCH_COUNT_MISMATCH BEFORE the first move, naming the component "
                 "and its true count. This is the instance-side twin of spatial.ground_actors' "
                 "expectedMatches, and the only guard that catches a component NAME reassigned to "
                 "a different scatter between the call that read the indices and this one - the "
                 "name still resolves, so nothing else would notice. Read the current number from "
                 "actor.get_instances' instanceCount."),
            false, TEXT(""), TArray<FString>({TEXT("expected_count")})},
        RPC_PARAM_DEF("apply", "boolean",
            "false runs the solve and writes NOTHING, reporting per instance what it would have "
            "done (proposedDeltaZCm / proposedTransform, status 'dry_run'). The solve and its "
            "thresholds are identical to the applying path's, so a dry run predicts the same move "
            "rather than answering a different question. This is the verify half of the verb - but "
            "only for the SOLVE. It is not the scope disclosure: the response names the component "
            "either way, and a caller who runs with apply defaulted to true never sees that name "
            "until after the move. Scope is guarded by the required 'component' and by "
            "expectedCount, not by this flag.", "true"),
        GroundRpcParam(TEXT("samples"), TEXT("number"),
            TEXT("Footprint sampling grid per axis over each INSTANCE's own bounds: 3 means a "
                 "3x3 = 9 column grid. 1 degrades to a single centre column. Clamped to 1-9. Cost "
                 "is samples^2 columns per instance per measurement, and each seated instance is "
                 "measured twice."),
            TEXT("3"), TArray<FString>({TEXT("gridSize"), TEXT("grid_size")})),
        GroundRpcParam(TEXT("footprintInset"), TEXT("number"),
            TEXT("Fraction of the footprint half-extent that samples are pulled inward from the "
                 "instance's bounds edge (0-0.45). A ratio of the BOUNDS - see contactRadius for "
                 "when that is the wrong quantity to take a fraction of."),
            TEXT("0.1"), TArray<FString>({TEXT("footprint_inset")})),
        GroundRpcParam(TEXT("contactRadius"), TEXT("number"),
            TEXT("Radius of the mesh's CONTACT PATCH in mesh-local cm - the part that actually "
                 "rests on the ground - scaled per instance by that instance's mean XY scale. "
                 "Replaces the bounds-derived XY span of the sample grid and changes nothing "
                 "else. 0 (default) keeps the bounds footprint, which is correct for a rock, a "
                 "slab, a log or a boulder (measured bounds/contact area ratios 0.98-2.49x) and "
                 "WRONG for anything with a trunk, a stem or a pedestal: a measured tree's AABB "
                 "is its canopy at 46.7x the contact area, so the grid probes ground the trunk "
                 "never touches and the solve lifts an already-planted instance to clear it - "
                 "177/177 of them by a median of 196 cm in one measured level, every row "
                 "reporting coverage 1.00, pass true and undersideReliefCm 0, because all three "
                 "are truthful about the box. footprintInset cannot reach it: it is a fraction of "
                 "the bounds, clamped at 0.45, so it still leaves 361 x 441 uu of sample "
                 "half-extent around a 100 uu contact radius, and being a ratio it moves with "
                 "instance yaw and scale. Clamped to the instance's own bounds. Read the "
                 "footprint that was actually sampled back from contact.footprintHalfExtentCm / "
                 "contact.footprintSource on each row."),
            TEXT("0"), TArray<FString>({TEXT("contact_radius")})),
        GroundRpcParam(TEXT("seatPercentile"), TEXT("number"),
            TEXT("Which sampled column the instance comes to rest against, as a percentile over the "
                 "columns' clearances. 0 (default) rests on the FIRST contact; 1 sinks until no "
                 "column floats; 0.5 is the median. Clamped to 0-1."),
            TEXT("0"), TArray<FString>({TEXT("seat_percentile")})),
        GroundRpcParam(TEXT("embedFraction"), TEXT("number"),
            TEXT("Sink this fraction of the instance's bounds HEIGHT into the ground after the seat "
                 "solve, so it beds in rather than resting tangent. Set 0 for a pure tangent rest."),
            TEXT("0.02"), TArray<FString>({TEXT("embed_fraction")})),
        GroundRpcParam(TEXT("embedDepth"), TEXT("number"),
            TEXT("Absolute extra sink in cm, added to embedFraction's contribution."),
            TEXT("0"), TArray<FString>({TEXT("embed_depth")})),
        GroundRpcParam(TEXT("minCoverage"), TEXT("number"),
            TEXT("Fraction of the instance's own footprint columns that must find ground, or it is "
                 "reported PARTIAL_GROUND_COVERAGE and left where it was. This is what catches an "
                 "instance overhanging a hole or the terrain edge."),
            TEXT("0.5"), TArray<FString>({TEXT("min_coverage")})),
        GroundRpcParam(TEXT("minContactPoints"), TEXT("number"),
            TEXT("Minimum footprint columns that must touch the surface after seating."),
            TEXT("1"), TArray<FString>({TEXT("min_contact_points")})),
        GroundRpcParam(TEXT("contactTolerance"), TEXT("number"),
            TEXT("A column counts as touching when its clearance is at or below this many cm. "
                 "Buried counts as touching; floating does not."),
            TEXT("2"), TArray<FString>({TEXT("contact_tolerance")})),
        GroundRpcParam(TEXT("maxSeatError"), TEXT("number"),
            TEXT("Post-move readback tolerance in cm. Every column's measured clearance must match "
                 "what the solve predicted within this much, or the instance is reported "
                 "GROUND_SEAT_READBACK_MISMATCH and NOT counted as placed."),
            TEXT("1"), TArray<FString>({TEXT("max_seat_error")})),
        RPC_PARAM_DEF("limit", "number",
            "Maximum instances to process in this call (1-5000). Pair with offset to page a larger "
            "scatter; instanceCount always reports the component's true total.", "512"),
        RPC_PARAM_DEF("offset", "number",
            "Index into the selected instance list to start from.", "0"),
        RPC_PARAM_DEF("detail", "string",
            "'summary' (counts only), 'failures' (default: counts plus a row per instance that was "
            "not placed - which on a dry run is every one of them), or 'all'. Row arrays are "
            "capped; counts never are. movedInstances[] is NOT governed by detail: it is the "
            "receipt for a mutation, not a diagnostic.", "failures")
    ))
{
    UWorld* World = GroundRpcResolveWorld();
    if (!World)
    {
        Ctx.SendError(ErrorCodes::ERR_EDITOR_WORLD_NOT_AVAILABLE,
            TEXT("No editor/PIE world available for instance grounding."));
        return true;
    }

    GroundPlacement::FGroundSurfaceSpec Surface;
    TArray<FString> UnresolvedIgnores;
    if (!GroundRpcParseSurface(Ctx, World, Surface, UnresolvedIgnores))
    {
        return true;
    }

    bool bIncludeSuccesses = false;
    bool bIncludeFailures = true;
    if (!GroundRpcParseDetail(Ctx, bIncludeSuccesses, bIncludeFailures))
    {
        return true;
    }

    FString ActorName;
    if (!ActorNameParamUtils::RequireActorName(Ctx, ActorName))
    {
        return true;
    }
    AActor* Actor = McpActorUtils::FindActorByName(World, ActorName);
    if (!Actor)
    {
        Ctx.SendError(ErrorCodes::ERR_ACTOR_NOT_FOUND,
            FString::Printf(TEXT("No actor named '%s' in the current world."), *ActorName));
        return true;
    }

    const InstancedMeshUtils::FInstancedComponentResolution Resolution =
        InstancedMeshUtils::ResolveInstancedComponent(Actor, Ctx.GetStringFirstOf(
            {TEXT("component"), TEXT("componentName"), TEXT("component_name")}));
    if (!Resolution.Component)
    {
        Ctx.SendError(Resolution.ErrorCode, Resolution.Error);
        return true;
    }
    UInstancedStaticMeshComponent* Component = Resolution.Component;
    const int32 InstanceCount = Component->GetInstanceCount();

    // The identity block is built HERE, from the component as it stands before the solve, and
    // carried into whichever answer this call ends up sending. It used to be written after the
    // seat loop closed, which made it a post-mortem: the caller learned which component had been
    // addressed only in the same breath as learning it had been moved. Same fields, same values -
    // seating changes transforms, not counts - but a pre-flight reading is what a refusal below
    // can attach, and what a future change to the loop cannot quietly invalidate.
    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    InstancedMeshUtils::WriteComponentIdentity(Data, Actor, Component);

    // The instance-side twin of spatial.ground_actors' expectedMatches. The component guard in
    // ResolveInstancedComponent cannot see this case: a one-component actor resolves without
    // ambiguity - it is still a shared foliage actor if it is one - and an
    // explicitly named component whose name was reassigned to a different scatter resolves
    // cleanly too, because the match is on the object name and nothing else.
    const TOptional<int32> Expected =
        Ctx.GetIntFirstOf({TEXT("expectedCount"), TEXT("expected_count")});
    if (Expected.IsSet() && Expected.GetValue() != InstanceCount)
    {
        TSharedPtr<FJsonObject> Detail = MakeShared<FJsonObject>();
        InstancedMeshUtils::WriteComponentIdentity(Detail, Actor, Component);
        Detail->SetNumberField(TEXT("expectedCount"), Expected.GetValue());
        Ctx.SendError(ErrorCodes::ERR_MATCH_COUNT_MISMATCH,
            FString::Printf(TEXT("'%s' carries %d instance(s), but expectedCount says %d. NOTHING "
                "WAS MOVED. Instance indices are positional, so a component whose count changed "
                "has renumbered them - and a component NAME can be reassigned to a different "
                "scatter entirely, which resolves without complaint."),
                *Component->GetName(), InstanceCount, Expected.GetValue()),
            Detail);
        return true;
    }

    // Explicit indices are validated against the component BEFORE anything is measured or moved.
    // Out of range is a refusal rather than a skip: instances are positional, so a stale index
    // would otherwise quietly seat a different instance than the caller named.
    TArray<int32> Indices;
    if (const TArray<TSharedPtr<FJsonValue>>* IndexArray = Ctx.GetArray(TEXT("indices")))
    {
        for (const TSharedPtr<FJsonValue>& Value : *IndexArray)
        {
            double Number = 0.0;
            if (!Value.IsValid() || !Value->TryGetNumber(Number))
            {
                Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
                    TEXT("'indices' must be an array of instance indices (numbers)."));
                return true;
            }
            const int32 Index = static_cast<int32>(Number);
            if (Index < 0 || Index >= InstanceCount)
            {
                Ctx.SendError(ErrorCodes::ERR_INSTANCE_INDEX_OUT_OF_RANGE,
                    FString::Printf(TEXT("Instance index %d is outside 0..%d; '%s' carries %d "
                                         "instance(s). NOTHING WAS MOVED."),
                        Index, InstanceCount - 1, *Component->GetName(), InstanceCount));
                return true;
            }
            Indices.AddUnique(Index);
        }
    }
    else
    {
        Indices.Reserve(InstanceCount);
        for (int32 Index = 0; Index < InstanceCount; ++Index)
        {
            Indices.Add(Index);
        }
    }

    const bool bApply = Ctx.GetBool(TEXT("apply"), true);

    GroundPlacement::FGroundSeatConfig Config;
    // Read here rather than through GroundRpcReadSampling: that helper also reads undersideModel,
    // and an instance has no mesh-profile underside to offer (see GroundPlacement::SeatInstance),
    // so declaring the parameter would advertise a choice this verb cannot honour.
    Config.GridSize = FMath::Clamp(
        Ctx.GetInt(TEXT("samples"),
            Ctx.GetInt(TEXT("gridSize"),
                Ctx.GetInt(TEXT("grid_size"), GroundPlacement::DefaultGridSize))),
        GroundPlacement::MinGridSize, GroundPlacement::MaxGridSize);
    Config.FootprintInset = FMath::Clamp(
        Ctx.GetNumber(TEXT("footprintInset"),
            Ctx.GetNumber(TEXT("footprint_inset"), GroundPlacement::DefaultFootprintInset)),
        0.0, 0.45);
    // Floored at 0 rather than refused: 0 IS the documented "use the bounds footprint" value, so
    // a negative number has an unambiguous nearest meaning and needs no error code of its own.
    // The upper bound is the instance's own AABB and is applied per instance by the sampler,
    // which is the only place the bounds are known.
    Config.ContactRadiusCm = FMath::Max(
        Ctx.GetNumber(TEXT("contactRadius"), Ctx.GetNumber(TEXT("contact_radius"), 0.0)), 0.0);
    Config.UndersideModel = GroundPlacement::EUndersideModel::BoundsPlane;
    Config.SeatPercentile = FMath::Clamp(
        Ctx.GetNumber(TEXT("seatPercentile"),
            Ctx.GetNumber(TEXT("seat_percentile"), 0.0)), 0.0, 1.0);
    Config.EmbedFraction = FMath::Max(
        Ctx.GetNumber(TEXT("embedFraction"),
            Ctx.GetNumber(TEXT("embed_fraction"), GroundPlacement::DefaultEmbedFraction)), 0.0);
    Config.EmbedDepthCm = FMath::Max(
        Ctx.GetNumber(TEXT("embedDepth"), Ctx.GetNumber(TEXT("embed_depth"), 0.0)), 0.0);
    Config.MaxSeatErrorCm = FMath::Max(
        Ctx.GetNumber(TEXT("maxSeatError"), Ctx.GetNumber(TEXT("max_seat_error"), 1.0)), 0.0);
    Config.Thresholds.MinCoverage = FMath::Clamp(
        Ctx.GetNumber(TEXT("minCoverage"),
            Ctx.GetNumber(TEXT("min_coverage"), GroundPlacement::DefaultMinCoverage)), 0.0, 1.0);
    Config.Thresholds.MinContactPoints = FMath::Max(
        Ctx.GetInt(TEXT("minContactPoints"), Ctx.GetInt(TEXT("min_contact_points"), 1)), 0);
    Config.Thresholds.ContactToleranceCm = FMath::Max(
        Ctx.GetNumber(TEXT("contactTolerance"),
            Ctx.GetNumber(TEXT("contact_tolerance"),
                GroundPlacement::DefaultContactToleranceCm)), 0.0);

    const int32 Offset = FMath::Max(Ctx.GetInt(TEXT("offset"), 0), 0);
    const int32 Limit = FMath::Clamp(Ctx.GetInt(TEXT("limit"), GroundRpcDefaultLimit),
        1, GroundRpcMaxBatch);

    // ---- Run the batch ----
    TArray<TSharedPtr<FJsonValue>> Rows;
    TArray<TSharedPtr<FJsonValue>> MovedRows;
    int32 Processed = 0;
    int32 PlacedCount = 0;
    int32 MovedCount = 0;
    int32 SolvedCount = 0;
    int32 FailedCount = 0;
    int32 DetailRowsDropped = 0;

    // ONE transaction for the whole seat, and only when this call can write. SeatInstance goes
    // through UInstancedStaticMeshComponent::UpdateInstanceTransform, whose Modify() has nowhere
    // to record without it; the cost is one FObjectRecord for the component regardless of how many
    // instances are seated, because FTransaction::SaveObject builds a record only for an object
    // that has none yet. A dry run opens nothing, so it leaves no empty entry on the undo stack.
    TUniquePtr<FScopedTransaction> SeatTransaction;
    if (bApply)
    {
        SeatTransaction = MakeUnique<FScopedTransaction>(
            FText::FromString(TEXT("MCP: spatial.ground_instances")));
    }

    for (int32 Cursor = Offset; Cursor < Indices.Num() && Processed < Limit; ++Cursor)
    {
        const GroundPlacement::FGroundInstanceSeatResult Result = GroundPlacement::SeatInstance(
            World, Component, Indices[Cursor], Surface, Config, bApply);
        ++Processed;

        // Every counter is derived from the result's own predicates. A dry run counts as SOLVED
        // rather than failed: nothing was attempted, so nothing failed.
        const bool bPlaced = Result.Seat.IsSeated();
        const bool bDryRun = Result.Seat.Status == GroundPlacement::EGroundSeatStatus::DryRun;
        PlacedCount += bPlaced ? 1 : 0;
        MovedCount += Result.Seat.WasMoved() ? 1 : 0;
        SolvedCount += bDryRun ? 1 : 0;
        FailedCount += (bPlaced || bDryRun) ? 0 : 1;

        if (Result.Seat.WasMoved() && Result.Seat.PreviousTransform.IsSet())
        {
            // Built through the shared row builder, and STAMPED world - GroundPlacement::SeatInstance
            // hardcodes bWorldSpace=true on both its GetInstanceTransform read and its
            // UpdateInstanceTransform write, and this verb declares no `space` parameter at all.
            // Replaying these rows through actor.set_instance_transforms used to be correct only by
            // coincidence: one verb's hardcode happened to equal the other verb's default, recorded
            // in no field on either side. The stamp makes the dependency explicit and checked.
            MovedRows.Add(MakeShared<FJsonValueObject>(
                InstancedMeshUtils::MakeMovedInstanceRow(Result.InstanceIndex,
                    Result.Seat.PreviousTransform.GetValue(), /*bWorldSpace*/ true)));
        }

        const bool bWantRow = bPlaced ? bIncludeSuccesses : bIncludeFailures;
        if (!bWantRow)
        {
            continue;
        }
        if (Rows.Num() >= GroundRpcMaxDetailRows)
        {
            ++DetailRowsDropped;
            continue;
        }

        TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
        Row->SetNumberField(TEXT("index"), Result.InstanceIndex);
        // Both booleans are DERIVED, never assigned from a literal: moved is "a transform was
        // recorded by the code that wrote one", placed is that ANDed with a post-move measurement
        // that passed.
        Row->SetBoolField(TEXT("moved"), Result.Seat.WasMoved());
        Row->SetBoolField(TEXT("placed"), Result.Seat.IsSeated());
        Row->SetStringField(TEXT("status"),
            GroundPlacement::SeatStatusToString(Result.Seat.Status));
        if (!Result.Seat.ReasonCode.IsEmpty())
        {
            Row->SetStringField(TEXT("reasonCode"), Result.Seat.ReasonCode);
        }
        if (!Result.Seat.Reason.IsEmpty())
        {
            Row->SetStringField(TEXT("reason"), Result.Seat.Reason);
        }
        if (Result.CurrentTransform.IsSet())
        {
            Row->SetObjectField(TEXT("currentTransform"),
                GroundRpcTransformObject(Result.CurrentTransform.GetValue()));
        }
        if (Result.Seat.WasMoved())
        {
            Row->SetNumberField(TEXT("deltaZCm"), Result.Seat.AppliedDeltaZCm);
            Row->SetNumberField(TEXT("embedCm"), Result.Seat.AppliedEmbedCm);
            Row->SetObjectField(TEXT("transform"),
                GroundRpcTransformObject(Result.Seat.AppliedTransform.GetValue()));
        }
        if (Result.Seat.PreviousTransform.IsSet())
        {
            Row->SetObjectField(TEXT("previousTransform"),
                GroundRpcTransformObject(Result.Seat.PreviousTransform.GetValue()));
        }
        if (Result.ProposedTransform.IsSet())
        {
            Row->SetNumberField(TEXT("proposedDeltaZCm"), Result.ProposedDeltaZCm);
            Row->SetNumberField(TEXT("embedCm"), Result.Seat.AppliedEmbedCm);
            Row->SetObjectField(TEXT("proposedTransform"),
                GroundRpcTransformObject(Result.ProposedTransform.GetValue()));
        }
        Row->SetObjectField(TEXT("contact"), GroundRpcContactObject(Result.Seat.Contact));
        Rows.Add(MakeShared<FJsonValueObject>(Row));
    }

    // Closed before the refresh below: the transaction records the instance data, and both ISM's
    // and HISM's PostEditUndo already redo the render/nav/cluster-tree refresh themselves.
    SeatTransaction.Reset();

    // One render-state dirty, one synchronous HISM cluster-tree rebuild and one package dirty for
    // the whole batch - and only when something was actually written.
    if (MovedCount > 0)
    {
        InstancedMeshUtils::FinishInstanceWrites(Component);
    }

    // Data already carries the component identity, written before the solve ran.
    Data->SetBoolField(TEXT("applied"), bApply);
    Data->SetNumberField(TEXT("selected"), Indices.Num());
    Data->SetNumberField(TEXT("requested"), Processed);
    Data->SetNumberField(TEXT("placed"), PlacedCount);
    Data->SetNumberField(TEXT("moved"), MovedCount);
    Data->SetNumberField(TEXT("solved"), SolvedCount);
    Data->SetNumberField(TEXT("failed"), FailedCount);
    Data->SetNumberField(TEXT("offset"), Offset);
    Data->SetBoolField(TEXT("truncated"), Processed < FMath::Max(Indices.Num() - Offset, 0));
    Data->SetObjectField(TEXT("surface"), GroundRpcSurfaceEcho(Surface, UnresolvedIgnores));

    TSharedPtr<FJsonObject> SeatEcho = MakeShared<FJsonObject>();
    SeatEcho->SetNumberField(TEXT("samples"), Config.GridSize);
    SeatEcho->SetNumberField(TEXT("footprintInset"), Config.FootprintInset);
    SeatEcho->SetNumberField(TEXT("seatPercentile"), Config.SeatPercentile);
    SeatEcho->SetNumberField(TEXT("embedFraction"), Config.EmbedFraction);
    SeatEcho->SetNumberField(TEXT("embedDepthCm"), Config.EmbedDepthCm);
    SeatEcho->SetNumberField(TEXT("maxSeatErrorCm"), Config.MaxSeatErrorCm);
    // Which footprint the batch sampled over, at call level. The per-instance half-extent the
    // grid actually spanned is on every row (contact.footprintHalfExtentCm) and is the truthful
    // one - contactRadius is mesh-local and scaled by each instance's own XY scale, so one call
    // legitimately samples a different box per instance. This pair says which question was asked;
    // the rows say what it measured on each.
    SeatEcho->SetStringField(TEXT("footprintSource"),
        Config.ContactRadiusCm > 0.0 ? TEXT("contact_radius") : TEXT("bounds"));
    if (Config.ContactRadiusCm > 0.0)
    {
        SeatEcho->SetNumberField(TEXT("contactRadiusCm"), Config.ContactRadiusCm);
    }
    // Stated rather than parameterised: an instance's underside cannot be probed per column,
    // because UInstancedStaticMeshComponent::LineTraceComponent answers from every instance body
    // at once and cannot attribute a hit to one of them.
    SeatEcho->SetStringField(TEXT("undersideModel"), TEXT("bounds_plane"));
    Data->SetObjectField(TEXT("seat"), SeatEcho);

    // The undo log, written before results[] and deliberately NOT governed by `detail`: it is the
    // receipt for a mutation, and gating recovery information on a diagnostic level is what made
    // the actor-side incident unrecoverable.
    if (MovedRows.Num() > 0)
    {
        Data->SetArrayField(TEXT("movedInstances"), MovedRows);
    }
    if (Rows.Num() > 0)
    {
        Data->SetArrayField(TEXT("results"), Rows);
    }
    if (DetailRowsDropped > 0)
    {
        Data->SetBoolField(TEXT("resultsTruncated"), true);
        Data->SetNumberField(TEXT("resultsDropped"), DetailRowsDropped);
    }
    GroundRpcAddAxisEcho(Data);

    UE_LOG(LogPinWrightSubsystem, Display,
        TEXT("spatial.ground_instances: %s %d/%d instance(s) of '%s' on '%s' (%d moved, %d failed)"),
        bApply ? TEXT("seated") : TEXT("solved (dry run)"),
        bApply ? PlacedCount : SolvedCount, Processed, *Component->GetName(),
        GroundPlacement::SurfacePresetToString(Surface.Preset), MovedCount, FailedCount);

    Ctx.SendSuccess(Data);
    return true;
}
