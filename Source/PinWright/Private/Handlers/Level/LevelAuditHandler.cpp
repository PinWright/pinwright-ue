// Copyright (c) 2026 Alexander Penkin. MIT License.

// LevelAuditHandler.cpp - level.audit, the one-call level lint.
//
// One RPC walks the world on the game thread and returns every actor it can show to be wrong,
// with per-check accounting of what it did and did not measure. It is BATCH by construction
// for the same reason spatial.ground_actors is: a per-actor Python loop over a level once
// wedged an editor for 168 minutes across 5100 uncancellable calls, so there is no per-actor
// audit verb to invite the loop back.
//
// Three things this verb refuses to do, each of them a failure this project has already had:
//   * It will not report a check as passed when it did not run. Every check reports per actor
//     as flagged / clean / unrunnable / not-applicable / ignored, and the buckets sum to the
//     number of actors examined.
//   * It will not guess what "the ground" or "the play area" is. Those checks need `surface`
//     and `playArea` and error rather than default, exactly as spatial.ground_actors does.
//   * It will not silently swallow an ignore rule. Every rule is echoed with its reason and
//     the number of actors it matched, so an over-broad rule is visible in the response.
//
// The solver behind the ground checks is GroundPlacement::MeasureContact - the same footprint
// sampler spatial.ground_actors and spatial.verify_grounding use - so the audit and the verb
// that fixes what it finds cannot disagree about what "seated" means.
//
// Coordinates are unreal units (cm), left-handed (+X fwd, +Y right, +Z up).

#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/ParamSpec.h"
#include "Handlers/ErrorCodes.h"
#include "Handlers/Level/LevelAuditUtils.h"
#include "Handlers/Spatial/GroundPlacementUtils.h"
#include "PinWrightSubsystem.h" // LogPinWrightSubsystem
#include "Utils/ActorUtils.h"
#include "Utils/NameMatchFilter.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Engine/Selection.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"

// No `using namespace LevelAudit;` here: this plugin builds with bUseUnity = true,
// so a using-directive in an unnamed namespace leaks into every other translation
// unit merged into the same blob - and the audit's type names are exactly the kind that would
// then collide. Every LevelAudit name below is qualified instead.
namespace
{
    // Default per-call ceiling on how many actors receive a footprint ground measurement.
    // Each costs samples^2 columns of layered traces, so this bounds worst-case game-thread
    // time. Actors past it are reported UNRUNNABLE, never skipped.
    constexpr int32 AuditRpcDefaultGroundBudget = 5000;
    constexpr int32 AuditRpcMaxGroundBudget = 50000;

    // Default actor ceiling for one call. Large enough to sweep a whole hand-built level in
    // one go (this project's map is ~2800 actors); pair with offset to page past it.
    constexpr int32 AuditRpcDefaultLimit = 20000;
    constexpr int32 AuditRpcMaxLimit = 200000;

    // Row cap. Counts are always exact and uncapped; only the rows are bounded, the same split
    // spatial.ground_actors uses for results[].
    constexpr int32 AuditRpcDefaultMaxFindings = 200;
    constexpr int32 AuditRpcMaxFindings = 2000;

    // How far above the actor's own top the ground probe starts, for the audit specifically.
    // GroundPlacement's own default is 500 cm, which is right for seating a prop that is
    // already roughly in place and WRONG here: an actor buried 1000 cm under a cliff has the
    // surface far above its top, and a probe that starts below that surface can never see it -
    // which is precisely how 620 underground actors measured as fine. 1 km of lift costs
    // nothing (a longer ray is not a more expensive query) and makes "is anything above this
    // actor" answerable. Overridable via surface.probeLift.
    constexpr double AuditRpcProbeLiftCm = 100000.0;
    // Likewise the layer budget: with the probe starting a kilometre up, a landscape-only
    // filter may have to peel foliage, roofs and fog cards before it reaches terrain.
    constexpr int32 AuditRpcMaxLayers = 16;

    // FParamSpec carrying BOTH a documented default and snake_case aliases. RPC_PARAM_DEF
    // cannot express aliases and ParamAliasUtils::MakeAliasParamSpec cannot express a default.
    // Declaring the aliases is load-bearing: the dispatcher rejects any top-level field that is
    // neither a declared name nor a declared alias. Field order is FParamSpec's declaration
    // order (Handlers/ParamSpec.h:15-22).
    FParamSpec AuditRpcParam(const TCHAR* Name, const TCHAR* Type, const TCHAR* Desc,
                             const TCHAR* Default, const TArray<FString>& Aliases)
    {
        FParamSpec Spec{FString(Name), FString(Type), FString(Desc), false, FString(Default)};
        Spec.Aliases = Aliases;
        return Spec;
    }

    TSharedPtr<FJsonObject> AuditRpcVector(const FVector& Vec)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetNumberField(TEXT("x"), Vec.X);
        Obj->SetNumberField(TEXT("y"), Vec.Y);
        Obj->SetNumberField(TEXT("z"), Vec.Z);
        return Obj;
    }

    void AuditRpcCollectStrings(const TSharedPtr<FJsonObject>& Obj, const TCHAR* Key,
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

    void AuditRpcWriteStrings(const TSharedPtr<FJsonObject>& Data, const TCHAR* Key,
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

    // Read one threshold out of the `thresholds` object under either spelling, leaving the
    // default in place when neither is present.
    void AuditRpcReadThreshold(const TSharedPtr<FJsonObject>& Obj, const TCHAR* Camel,
                               const TCHAR* Snake, double& InOutValue)
    {
        if (!Obj.IsValid())
        {
            return;
        }
        double Value = 0.0;
        if (Obj->TryGetNumberField(Camel, Value) || (Snake && Obj->TryGetNumberField(Snake, Value)))
        {
            InOutValue = Value;
        }
    }

    void AuditRpcReadThresholdInt(const TSharedPtr<FJsonObject>& Obj, const TCHAR* Camel,
                                  const TCHAR* Snake, int32& InOutValue)
    {
        double Value = 0.0;
        if (!Obj.IsValid())
        {
            return;
        }
        if (Obj->TryGetNumberField(Camel, Value) || (Snake && Obj->TryGetNumberField(Snake, Value)))
        {
            InOutValue = static_cast<int32>(Value);
        }
    }

    // "summary" | "findings" (default) | "all". Anything else is a caller error rather than a
    // silent fallback, so a typo cannot quietly halve the response.
    bool AuditRpcParseDetail(const FHandlerContext& Ctx, bool& bOutRows, int32& OutCleanCap)
    {
        const FString Detail = Ctx.GetString(TEXT("detail"), TEXT("findings")).ToLower();
        if (Detail == TEXT("summary")) { bOutRows = false; OutCleanCap = 0;   return true; }
        if (Detail == TEXT("findings")){ bOutRows = true;  OutCleanCap = 0;   return true; }
        if (Detail == TEXT("all"))     { bOutRows = true;  OutCleanCap = 500; return true; }
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
            FString::Printf(TEXT("Unknown detail '%s'. Valid: summary, findings, all."), *Detail));
        return false;
    }

    // Turn the caller's `checks` / `excludeChecks` into a selection mask. An unrecognized id is
    // rejected: a typo that silently ran nothing looks exactly like a clean level, which is the
    // single most dangerous way for a lint to fail.
    bool AuditRpcResolveChecks(const FHandlerContext& Ctx, bool bHasSurface, bool bHasPlayArea,
                               uint32& OutMask, TArray<FString>& OutExplicit)
    {
        const TSharedPtr<FJsonObject>& Payload = Ctx.GetRawPayload();

        TArray<FString> Wanted;
        AuditRpcCollectStrings(Payload, TEXT("checks"), Wanted);
        TArray<FString> Excluded;
        AuditRpcCollectStrings(Payload, TEXT("excludeChecks"), Excluded);
        AuditRpcCollectStrings(Payload, TEXT("exclude_checks"), Excluded);

        auto Reject = [&Ctx](const FString& Id)
        {
            const FString Valid = PinWrightAudit::ValidCheckIdList(LevelAudit::AllChecks());
            Ctx.SendError(ErrorCodes::ERR_AUDIT_UNKNOWN_CHECK,
                FString::Printf(
                    TEXT("Unknown check '%s'. A misspelled check is rejected rather than skipped, "
                         "because a run that silently checked nothing is indistinguishable from a "
                         "clean level. Valid checks: %s."), *Id, *Valid));
        };

        if (Wanted.Num() > 0)
        {
            OutMask = 0;
            for (const FString& Id : Wanted)
            {
                LevelAudit::ECheck Check = LevelAudit::ECheck::NanTransform;
                if (!LevelAudit::ParseCheckId(Id, Check))
                {
                    Reject(Id);
                    return false;
                }
                OutMask |= LevelAudit::CheckBit(Check);
                OutExplicit.AddUnique(Id);
            }
        }
        else
        {
            OutMask = LevelAudit::DefaultCheckMask();
            // Supplying `surface` / `playArea` without naming `checks` opts into the checks
            // those arguments exist for. Stated rather than inferred silently: the resolved
            // set is echoed per check in the response, so what ran is never a guess.
            if (bHasSurface)  { OutMask |= LevelAudit::SurfaceCheckMask(); }
            if (bHasPlayArea) { OutMask |= LevelAudit::PlayAreaCheckMask(); }
            // DeeplyEmbedded stays opt-in even then: on real content most actors it flags are
            // deliberately bedded, and a lint that cries wolf gets ignored.
            OutMask &= ~LevelAudit::CheckBit(LevelAudit::ECheck::DeeplyEmbedded);
        }

        for (const FString& Id : Excluded)
        {
            LevelAudit::ECheck Check = LevelAudit::ECheck::NanTransform;
            if (!LevelAudit::ParseCheckId(Id, Check))
            {
                Reject(Id);
                return false;
            }
            OutMask &= ~LevelAudit::CheckBit(Check);
        }

        if (OutMask == 0)
        {
            Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
                TEXT("No checks are selected, so this call would examine every actor and report "
                     "nothing. Name at least one check, or omit 'checks' for the default set."));
            return false;
        }
        return true;
    }

    // One caller-supplied ignore rule.
    bool AuditRpcParseIgnoreRule(const TSharedPtr<FJsonObject>& RuleObj, LevelAudit::FIgnoreRule& OutRule,
                                 FString& OutError)
    {
        if (!RuleObj.IsValid())
        {
            OutError = TEXT("Each entry of 'ignore' must be an object.");
            return false;
        }
        AuditRpcCollectStrings(RuleObj, TEXT("actors"), OutRule.Actors);
        AuditRpcCollectStrings(RuleObj, TEXT("names"), OutRule.NamePatterns);
        AuditRpcCollectStrings(RuleObj, TEXT("namePatterns"), OutRule.NamePatterns);
        AuditRpcCollectStrings(RuleObj, TEXT("name_patterns"), OutRule.NamePatterns);
        AuditRpcCollectStrings(RuleObj, TEXT("classes"), OutRule.Classes);

        TArray<FString> Tags;
        AuditRpcCollectStrings(RuleObj, TEXT("tags"), Tags);
        for (const FString& Tag : Tags)
        {
            OutRule.Tags.AddUnique(FName(*Tag));
        }

        TArray<FString> Checks;
        AuditRpcCollectStrings(RuleObj, TEXT("checks"), Checks);
        for (const FString& Id : Checks)
        {
            LevelAudit::ECheck Check = LevelAudit::ECheck::NanTransform;
            if (!LevelAudit::ParseCheckId(Id, Check))
            {
                OutError = FString::Printf(
                    TEXT("Ignore rule names unknown check '%s'."), *Id);
                return false;
            }
            OutRule.CheckMask |= LevelAudit::CheckBit(Check);
        }

        RuleObj->TryGetStringField(TEXT("reason"), OutRule.Reason);

        if (OutRule.IsEmpty())
        {
            // An empty rule would match nothing (LevelAuditUtils.cpp AuditRuleMatches), which
            // is the safe direction, but it is also certainly not what the caller meant.
            OutError = TEXT("An ignore rule must name at least one of actors / names / classes / "
                            "tags. An empty rule matches nothing and is almost certainly a "
                            "mistake, so it is rejected rather than silently ignored.");
            return false;
        }
        return true;
    }

    // Absent `playArea` is fine and leaves OutArea invalid; the check that needs it then
    // errors by name. A PRESENT but unusable one is always an error - never a fallback to a
    // different area, because measuring against a box the caller did not ask for is worse than
    // refusing to measure.
    bool AuditRpcParsePlayArea(const FHandlerContext& Ctx, UWorld* World, LevelAudit::FPlayArea& OutArea)
    {
        TSharedPtr<FJsonObject> Obj = Ctx.GetObject(TEXT("playArea"));
        if (!Obj.IsValid())
        {
            Obj = Ctx.GetObject(TEXT("play_area"));
        }
        if (!Obj.IsValid())
        {
            return true;
        }

        FString SourceName = TEXT("landscape");
        Obj->TryGetStringField(TEXT("source"), SourceName);
        if (!LevelAudit::ParsePlayAreaSource(SourceName, OutArea.Source))
        {
            Ctx.SendError(ErrorCodes::ERR_AUDIT_INVALID_PLAY_AREA_SPEC,
                FString::Printf(
                    TEXT("Unknown playArea source '%s'. Valid: landscape (union of the landscape "
                         "proxies' bounds - the terrain footprint IS the playable area, and unlike "
                         "level_bounds it is not inflated by the very strays you are looking for), "
                         "level_bounds, actor, box."), *SourceName));
            return false;
        }

        if (OutArea.Source == LevelAudit::EPlayAreaSource::Box)
        {
            const TSharedPtr<FJsonObject>* MinObj = nullptr;
            const TSharedPtr<FJsonObject>* MaxObj = nullptr;
            const TSharedPtr<FJsonObject>* CentreObj = nullptr;
            const TSharedPtr<FJsonObject>* ExtentObj = nullptr;
            auto ReadVec = [](const TSharedPtr<FJsonObject>* Src, FVector& Out)
            {
                if (!Src || !(*Src).IsValid()) { return false; }
                double X = 0.0, Y = 0.0, Z = 0.0;
                (*Src)->TryGetNumberField(TEXT("x"), X);
                (*Src)->TryGetNumberField(TEXT("y"), Y);
                (*Src)->TryGetNumberField(TEXT("z"), Z);
                Out = FVector(X, Y, Z);
                return true;
            };
            FVector A = FVector::ZeroVector;
            FVector B = FVector::ZeroVector;
            if (Obj->TryGetObjectField(TEXT("min"), MinObj)
                && Obj->TryGetObjectField(TEXT("max"), MaxObj)
                && ReadVec(MinObj, A) && ReadVec(MaxObj, B))
            {
                OutArea.Box = FBox(A.ComponentMin(B), A.ComponentMax(B));
            }
            else if ((Obj->TryGetObjectField(TEXT("center"), CentreObj)
                      || Obj->TryGetObjectField(TEXT("centre"), CentreObj))
                     && Obj->TryGetObjectField(TEXT("extent"), ExtentObj)
                     && ReadVec(CentreObj, A) && ReadVec(ExtentObj, B))
            {
                OutArea.Box = FBox(A - B.GetAbs(), A + B.GetAbs());
            }
            else
            {
                Ctx.SendError(ErrorCodes::ERR_AUDIT_INVALID_PLAY_AREA_SPEC,
                    TEXT("playArea source 'box' needs either {min:{x,y,z}, max:{x,y,z}} or "
                         "{center:{x,y,z}, extent:{x,y,z}}."));
                return false;
            }
        }

        double Margin = 0.0;
        if (Obj->TryGetNumberField(TEXT("margin"), Margin)
            || Obj->TryGetNumberField(TEXT("expand"), Margin))
        {
            OutArea.MarginCm = FMath::Max(Margin, 0.0);
        }
        bool bIgnoreZ = true;
        if (Obj->TryGetBoolField(TEXT("ignoreZ"), bIgnoreZ)
            || Obj->TryGetBoolField(TEXT("ignore_z"), bIgnoreZ))
        {
            OutArea.bIgnoreZ = bIgnoreZ;
        }

        FString ActorName;
        Obj->TryGetStringField(TEXT("actor"), ActorName);

        FString Error;
        if (!LevelAudit::ResolvePlayArea(World, OutArea, ActorName, Error))
        {
            Ctx.SendError(ErrorCodes::ERR_AUDIT_INVALID_PLAY_AREA_SPEC, Error);
            return false;
        }
        return true;
    }

    TSharedPtr<FJsonObject> AuditRpcFindingObject(const LevelAudit::FFinding& Finding)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        // 'actor' keeps its meaning (the display LABEL) for existing callers, but labels are
        // not unique — acting on one can hit a different actor than the one flagged.
        // objectName is GetName(), unique within the level and the collision-safe key.
        Obj->SetStringField(TEXT("actor"), Finding.ActorLabel);
        Obj->SetStringField(TEXT("objectName"), Finding.ActorObjectName);
        Obj->SetStringField(TEXT("path"), Finding.ActorPath);
        Obj->SetStringField(TEXT("class"), Finding.ActorClass);
        Obj->SetStringField(TEXT("check"), LevelAudit::CheckInfo(Finding.Check).Id);
        Obj->SetStringField(TEXT("code"), Finding.Code);
        // status is the load-bearing field: "unrunnable" means this check did NOT evaluate
        // this actor, and reading it as anything other than an unanswered question is the
        // mistake this whole verb is shaped to prevent.
        Obj->SetStringField(TEXT("status"), PinWrightAudit::StatusToWire(Finding.Status));
        Obj->SetStringField(TEXT("severity"), PinWrightAudit::SeverityToWire(Finding.Severity));
        Obj->SetStringField(TEXT("message"), Finding.Message);
        if (Finding.Measurements.IsValid() && Finding.Measurements->Values.Num() > 0)
        {
            Obj->SetObjectField(TEXT("measurements"), Finding.Measurements);
        }
        return Obj;
    }
}

// ================= level.audit =================

REGISTER_RPC_HANDLER("level.audit", "level",
    "Sweep every actor in the level ONCE and report the ones that are demonstrably wrong: "
    "non-finite transforms, zero / negative / extreme scale, null meshes, missing or "
    "placeholder materials, actors on the world origin, actors below KillZ or outside the "
    "practical world bounds, duplicate actors at one transform, and - when you state a "
    "`surface` - actors that are entirely underground, floating, balanced on a single contact "
    "point, or resting on a stack that ends in mid-air. Checks are individually selectable and "
    "an ignore list silences the ones that are deliberate. "
    "The report says what it COULD NOT check: every check reports per actor as flagged, clean, "
    "unrunnable, not-applicable or ignored, and those buckets sum to the number of actors "
    "examined, so a check that could not run for an actor never reads as a pass. "
    "This is a read-only sweep - nothing is moved, modified or marked dirty. It is also "
    "deliberately batch-only: use it instead of looping a per-actor query over a level. "
    "Coordinates are unreal units (cm), left-handed (+X fwd, +Y right, +Z up).",
    RPC_PARAMS(
        RPC_PARAM_OPT("checks", "array",
            "Check ids to run, e.g. [\"zero_scale\",\"no_mesh\",\"below_surface\"]. Omit for the "
            "default set (everything that needs no extra argument, plus the ground checks when "
            "you supply `surface` and the play-area check when you supply `playArea`). An "
            "unknown id is an ERROR, not a skip: a typo that silently ran nothing looks exactly "
            "like a clean level. The response echoes every check with whether it ran."),
        AuditRpcParam(TEXT("excludeChecks"), TEXT("array"),
            TEXT("Check ids to subtract from the resolved set. Applied after `checks`."),
            TEXT(""), TArray<FString>({TEXT("exclude_checks")})),
        RPC_PARAM_OPT("surface", "object",
            "What counts as ground, in the SAME shape spatial.ground_actors takes: {preset, "
            "channel?, traceComplex?, onlyClasses?, excludeClasses?, excludeNames?, "
            "ignoreActors?, maxLayers?, maxDrop?, probeLift?}. preset is 'landscape' (terrain "
            "only - the right answer, and the only one for which below_surface is meaningful, "
            "because a non-terrain spec accepts a ROOF above an actor as its surface), "
            "'any_solid', or 'custom'. Required by below_surface / airborne / balanced / "
            "unsupported_assembly; those checks error rather than guess. probeLift defaults to "
            "100000 cm here rather than the 500 cm the placement verbs use, because a probe "
            "that starts below whatever buried an actor can never see it."),
        AuditRpcParam(TEXT("playArea"), TEXT("object"),
            TEXT("Where the map is: {source, actor?, min?, max?, center?, extent?, margin?, "
                 "ignoreZ?}. source is 'landscape' (union of landscape proxy bounds - preferred, "
                 "because it is not inflated by the strays you are hunting), 'level_bounds' "
                 "(summed actor bounds; a stray actor INFLATES this box and is then inside it), "
                 "'actor' (bounds of a named volume), or 'box'. ignoreZ defaults true: 'far from "
                 "the play area' is a plan-view question, and a level's Z extent is dominated by "
                 "sky actors kilometres up. Required by outside_play_area."),
            TEXT(""), TArray<FString>({TEXT("play_area")})),
        RPC_PARAM_OPT("ignore", "array",
            "Ignore rules, each {actors?, names?, classes?, tags?, checks?, reason?}. Every "
            "populated field must match (AND); `checks` limits the rule to those checks (omit "
            "for all of them); `reason` is echoed back with the number of actors the rule "
            "matched, so an over-broad rule is visible instead of just making findings vanish. "
            "`names` are wildcard patterns (* and ?) against label and internal name; `classes` "
            "match the actor class or ANY ancestor, so one 'LandscapeProxy' entry covers "
            "ALandscape and ALandscapeStreamingProxy."),
        AuditRpcParam(TEXT("useDefaultIgnores"), TEXT("boolean"),
            TEXT("Apply the audit's built-in ignore rules (engine bookkeeping actors at the "
                 "origin, landscape size, environment actors' ground relationship, foliage "
                 "containers). Each is echoed with its reason. Set false to see everything."),
            TEXT("true"), TArray<FString>({TEXT("use_default_ignores")})),
        AuditRpcParam(TEXT("ignoreTag"), TEXT("string"),
            TEXT("Actors carrying this AActor tag are exempt from EVERY check. This is the "
                 "durable, per-actor exemption: 'this rock is buried on purpose' is knowledge "
                 "that belongs with the actor, survives renames and moves between levels, and "
                 "cannot be expressed by a name pattern. Pass \"\" to disable. The audit only "
                 "READS the tag; it never writes one."),
            TEXT("PinWright.AuditIgnore"), TArray<FString>({TEXT("ignore_tag")})),
        RPC_PARAM_OPT("thresholds", "object",
            "Override any comparison the audit makes: {zeroScaleEpsilon, maxScale, maxBoundsCm, "
            "originToleranceCm, duplicateToleranceCm, duplicateAngleToleranceDeg, killZMarginCm, "
            "worldBoundsCm, playAreaMarginCm, maxGapCm, minCoverDepthCm, deepCoverDepthCm, "
            "maxEmbedFraction, balancedMinFootprintCm, balancedMinContactPoints, minCoverage, "
            "contactToleranceCm}. Every threshold actually used is echoed in the response, "
            "because a lint whose numbers are invisible cannot be argued with."),
        AuditRpcParam(TEXT("samples"), TEXT("number"),
            TEXT("Ground sampling grid per axis for the surface checks: 3 means a 3x3 = 9 column "
                 "grid over each actor's footprint. 1 degrades to a single centre probe, which "
                 "is what cannot tell a seated actor from a balanced one. Clamped 1-9."),
            TEXT("3"), TArray<FString>({TEXT("gridSize"), TEXT("grid_size")})),
        AuditRpcParam(TEXT("footprintInset"), TEXT("number"),
            TEXT("Fraction of the footprint half-extent samples are pulled inward from the "
                 "bounds edge (0-0.45). An AABB corner over a rounded rock is empty air."),
            TEXT("0.1"), TArray<FString>({TEXT("footprint_inset")})),
        AuditRpcParam(TEXT("undersideModel"), TEXT("string"),
            TEXT("'mesh' (default) traces each column against the actor's own geometry; "
                 "'bounds_plane' models the underside as a flat plane at the bounds minimum."),
            TEXT("mesh"), TArray<FString>({TEXT("underside_model")})),
        AuditRpcParam(TEXT("maxGroundActors"), TEXT("number"),
            TEXT("Ceiling on how many actors receive a footprint ground measurement in this "
                 "call. Actors past it are reported UNRUNNABLE for every ground check, never "
                 "skipped - a budget that silently stops measuring is the same failure as a "
                 "check that silently stops running."),
            TEXT("5000"), TArray<FString>({TEXT("max_ground_actors")})),
        FParamSpec{TEXT("actors"), TEXT("array"),
            TEXT("Restrict the sweep to these actor labels / internal names / paths. Omit all of "
                 "actors / prefix / filter / selection to audit the WHOLE level, which is the "
                 "normal use - unlike the mutating spatial verbs, a read-only sweep's dangerous "
                 "default is one that looks at too little."),
            false, TEXT(""), TArray<FString>({TEXT("actorNames"), TEXT("actor_names")})},
        RPC_PARAM_OPT("prefix", "string",
            "Restrict the sweep to actors whose label or internal name starts with this."),
        RPC_PARAM_OPT("filter", "string",
            "Restrict the sweep by label or internal name using the shared pattern vocabulary."),
        NameMatch::MatchModeParam(TEXT("filter")),
        NameMatch::CaseSensitiveParam(TEXT("filter")),
        RPC_PARAM_OPT("selection", "boolean", "Restrict the sweep to the editor selection."),
        RPC_PARAM_DEF("world", "string",
            "'editor' (default), 'pie', or 'auto'. Defaults to the EDITOR world because an audit "
            "is about authored content; a PIE world also holds runtime-spawned actors nobody "
            "placed.", "editor"),
        RPC_PARAM_DEF("limit", "number",
            "Maximum actors to examine in this call. Pair with offset to page; the response "
            "always reports the true matched total and flips truncated.", "20000"),
        RPC_PARAM_DEF("offset", "number", "Index into the matched set to start from.", "0"),
        RPC_PARAM_DEF("detail", "string",
            "'summary' (counts only), 'findings' (default: counts plus a row per finding), or "
            "'all' (adds a capped list of actors with no findings). Counts are never capped.",
            "findings"),
        AuditRpcParam(TEXT("maxFindings"), TEXT("number"),
            TEXT("Cap on finding ROWS in the response (1-2000). Per-check counts stay exact and "
                 "findingsDropped reports what was elided."),
            TEXT("200"), TArray<FString>({TEXT("max_findings")})),
        AuditRpcParam(TEXT("failOn"), TEXT("string"),
            TEXT("What makes `pass` false: 'error' (default), 'any' (errors and warnings), or "
                 "'none'. Regardless of this, `pass` is ALSO false whenever any check was "
                 "unrunnable or the sweep was truncated - an unmeasured check is not a passed "
                 "check. errorCount / warningCount / unrunnableCount are reported separately so "
                 "you can gate on whatever you actually mean."),
            TEXT("error"), TArray<FString>({TEXT("fail_on")}))
    ))
{
    FString ResolvedWorldMode;
    UWorld* World = McpActorUtils::ResolveQueryWorld(
        Ctx.GetString(TEXT("world"), TEXT("editor")), ResolvedWorldMode);
    if (!World)
    {
        Ctx.SendError(ErrorCodes::ERR_EDITOR_WORLD_NOT_AVAILABLE,
            FString::Printf(TEXT("No %s world is available to audit."), *ResolvedWorldMode));
        return true;
    }

    bool bIncludeRows = true;
    int32 CleanCap = 0;
    if (!AuditRpcParseDetail(Ctx, bIncludeRows, CleanCap))
    {
        return true;
    }

    LevelAudit::FConfig Config;
    Config.MaxCleanActors = CleanCap;

    // ---- surface ----
    TArray<FString> UnresolvedIgnoreActors;
    const TSharedPtr<FJsonObject> SurfaceObj = Ctx.GetObject(TEXT("surface"));
    if (SurfaceObj.IsValid())
    {
        // Pre-seed the audit's own probe geometry; ParseSurfaceJson only overwrites fields the
        // caller actually supplied, so an explicit probeLift still wins.
        Config.Surface.ProbeLiftCm = AuditRpcProbeLiftCm;
        Config.Surface.MaxLayers = AuditRpcMaxLayers;
        FString SurfaceError;
        if (!GroundPlacement::ParseSurfaceJson(SurfaceObj, World, Config.Surface,
                                               UnresolvedIgnoreActors, SurfaceError))
        {
            Ctx.SendError(ErrorCodes::ERR_INVALID_SURFACE_SPEC, SurfaceError);
            return true;
        }
        Config.bHasSurface = true;
    }

    // ---- play area ----
    if (!AuditRpcParsePlayArea(Ctx, World, Config.PlayArea))
    {
        return true;
    }

    // ---- checks ----
    TArray<FString> ExplicitChecks;
    if (!AuditRpcResolveChecks(Ctx, Config.bHasSurface, Config.PlayArea.bValid,
                               Config.SelectedChecks, ExplicitChecks))
    {
        return true;
    }

    // A check that needs an argument it did not get is a hard error, never a quiet no-op.
    // Naming exactly which checks are affected is the difference between an actionable
    // rejection and a caller re-reading the wiki.
    {
        const uint32 MissingSurface = Config.bHasSurface
            ? 0u : (Config.SelectedChecks & LevelAudit::SurfaceCheckMask());
        const uint32 MissingPlayArea = Config.PlayArea.bValid
            ? 0u : (Config.SelectedChecks & LevelAudit::PlayAreaCheckMask());
        if (MissingSurface != 0)
        {
            FString Names;
            for (const LevelAudit::FCheckInfo& Info : LevelAudit::AllChecks())
            {
                if (LevelAudit::HasCheck(MissingSurface, Info.Check))
                {
                    if (!Names.IsEmpty()) { Names += TEXT(", "); }
                    Names += Info.Id;
                }
            }
            Ctx.SendError(ErrorCodes::ERR_INVALID_SURFACE_SPEC,
                FString::Printf(
                    TEXT("These checks measure against the ground and need a `surface` argument: "
                         "%s. State what counts as ground - {\"preset\":\"landscape\"} for terrain "
                         "- or drop them from `checks`. There is no default surface: a wrong one "
                         "is the cause of every floating- and buried-prop bug this audit exists "
                         "to find."), *Names));
            return true;
        }
        if (MissingPlayArea != 0)
        {
            Ctx.SendError(ErrorCodes::ERR_AUDIT_INVALID_PLAY_AREA_SPEC,
                TEXT("The outside_play_area check needs a `playArea` argument. State where the "
                     "map is - {\"source\":\"landscape\"} is usually right - or drop the check. "
                     "The audit will not infer a play area from the actors it is auditing."));
            return true;
        }
    }

    // ---- ignore rules ----
    if (Ctx.GetBoolFirstOf({TEXT("useDefaultIgnores"), TEXT("use_default_ignores")}, true))
    {
        Config.IgnoreRules = LevelAudit::DefaultIgnoreRules();
    }
    if (const TArray<TSharedPtr<FJsonValue>>* RuleArray = Ctx.GetArray(TEXT("ignore")))
    {
        for (const TSharedPtr<FJsonValue>& Value : *RuleArray)
        {
            const TSharedPtr<FJsonObject>* RuleObj = nullptr;
            if (!Value.IsValid() || !Value->TryGetObject(RuleObj) || !RuleObj)
            {
                Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
                    TEXT("Each entry of 'ignore' must be an object."));
                return true;
            }
            LevelAudit::FIgnoreRule Rule;
            FString RuleError;
            if (!AuditRpcParseIgnoreRule(*RuleObj, Rule, RuleError))
            {
                Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT, RuleError);
                return true;
            }
            Config.IgnoreRules.Add(MoveTemp(Rule));
        }
    }
    const FString TagName = Ctx.GetStringFirstOf({TEXT("ignoreTag"), TEXT("ignore_tag")},
                                                 TEXT("PinWright.AuditIgnore"));
    Config.IgnoreTag = TagName.IsEmpty() ? NAME_None : FName(*TagName);

    // ---- thresholds ----
    const TSharedPtr<FJsonObject> ThresholdObj = Ctx.GetObject(TEXT("thresholds"));
    LevelAudit::FThresholds& T = Config.Thresholds;
    AuditRpcReadThreshold(ThresholdObj, TEXT("zeroScaleEpsilon"), TEXT("zero_scale_epsilon"),
                          T.ZeroScaleEpsilon);
    AuditRpcReadThreshold(ThresholdObj, TEXT("maxScale"), TEXT("max_scale"), T.MaxScale);
    AuditRpcReadThreshold(ThresholdObj, TEXT("maxBoundsCm"), TEXT("max_bounds_cm"), T.MaxBoundsCm);
    AuditRpcReadThreshold(ThresholdObj, TEXT("originToleranceCm"), TEXT("origin_tolerance_cm"),
                          T.OriginToleranceCm);
    AuditRpcReadThreshold(ThresholdObj, TEXT("duplicateToleranceCm"), TEXT("duplicate_tolerance_cm"),
                          T.DuplicateToleranceCm);
    AuditRpcReadThreshold(ThresholdObj, TEXT("duplicateAngleToleranceDeg"),
                          TEXT("duplicate_angle_tolerance_deg"), T.DuplicateAngleToleranceDeg);
    AuditRpcReadThreshold(ThresholdObj, TEXT("killZMarginCm"), TEXT("kill_z_margin_cm"),
                          T.KillZMarginCm);
    AuditRpcReadThreshold(ThresholdObj, TEXT("worldBoundsCm"), TEXT("world_bounds_cm"),
                          T.WorldBoundsCm);
    AuditRpcReadThreshold(ThresholdObj, TEXT("playAreaMarginCm"), TEXT("play_area_margin_cm"),
                          T.PlayAreaMarginCm);
    AuditRpcReadThreshold(ThresholdObj, TEXT("maxGapCm"), TEXT("max_gap_cm"), T.MaxGapCm);
    AuditRpcReadThreshold(ThresholdObj, TEXT("minCoverDepthCm"), TEXT("min_cover_depth_cm"),
                          T.MinCoverDepthCm);
    AuditRpcReadThreshold(ThresholdObj, TEXT("deepCoverDepthCm"), TEXT("deep_cover_depth_cm"),
                          T.DeepCoverDepthCm);
    AuditRpcReadThreshold(ThresholdObj, TEXT("maxEmbedFraction"), TEXT("max_embed_fraction"),
                          T.MaxEmbedFraction);
    AuditRpcReadThreshold(ThresholdObj, TEXT("balancedMinFootprintCm"),
                          TEXT("balanced_min_footprint_cm"), T.BalancedMinFootprintCm);
    AuditRpcReadThresholdInt(ThresholdObj, TEXT("balancedMinContactPoints"),
                             TEXT("balanced_min_contact_points"), T.BalancedMinContactPoints);
    AuditRpcReadThreshold(ThresholdObj, TEXT("minCoverage"), TEXT("min_coverage"), T.MinCoverage);
    AuditRpcReadThreshold(ThresholdObj, TEXT("contactToleranceCm"), TEXT("contact_tolerance_cm"),
                          T.ContactToleranceCm);
    // The play-area margin is carried on the area itself (it is applied to the box), so the
    // threshold spelling and the playArea.margin spelling must not disagree.
    if (T.PlayAreaMarginCm > 0.0)
    {
        Config.PlayArea.MarginCm = T.PlayAreaMarginCm;
    }
    else
    {
        T.PlayAreaMarginCm = Config.PlayArea.MarginCm;
    }

    // ---- ground sampling ----
    Config.GridSize = FMath::Clamp(
        Ctx.GetInt(TEXT("samples"),
            Ctx.GetInt(TEXT("gridSize"),
                Ctx.GetInt(TEXT("grid_size"), GroundPlacement::DefaultGridSize))),
        GroundPlacement::MinGridSize, GroundPlacement::MaxGridSize);
    Config.FootprintInset = FMath::Clamp(
        Ctx.GetNumber(TEXT("footprintInset"),
            Ctx.GetNumber(TEXT("footprint_inset"), GroundPlacement::DefaultFootprintInset)),
        0.0, 0.45);
    const FString ModelName = Ctx.GetStringFirstOf(
        {TEXT("undersideModel"), TEXT("underside_model")}, TEXT("mesh")).ToLower();
    Config.UndersideModel = (ModelName == TEXT("bounds_plane") || ModelName == TEXT("boundsplane"))
        ? GroundPlacement::EUndersideModel::BoundsPlane
        : GroundPlacement::EUndersideModel::MeshProfile;
    Config.MaxGroundActors = FMath::Clamp(
        Ctx.GetInt(TEXT("maxGroundActors"),
            Ctx.GetInt(TEXT("max_ground_actors"), AuditRpcDefaultGroundBudget)),
        0, AuditRpcMaxGroundBudget);

    // ---- scope ----
    if (!NameMatch::Require(Ctx, TArray<FString>{TEXT("filter")}, Config.Filter))
    {
        return true;
    }
    Config.Prefix = Ctx.GetString(TEXT("prefix"));

    TArray<FString> ExplicitNames;
    AuditRpcCollectStrings(Ctx.GetRawPayload(), TEXT("actors"), ExplicitNames);
    AuditRpcCollectStrings(Ctx.GetRawPayload(), TEXT("actorNames"), ExplicitNames);
    AuditRpcCollectStrings(Ctx.GetRawPayload(), TEXT("actor_names"), ExplicitNames);
    TArray<FString> UnresolvedActors;
    if (ExplicitNames.Num() > 0)
    {
        Config.bScopeActorsProvided = true;
        for (const FString& Name : ExplicitNames)
        {
            if (AActor* Found = McpActorUtils::FindActorByName(World, Name))
            {
                Config.ScopeActors.AddUnique(TWeakObjectPtr<AActor>(Found));
            }
            else
            {
                UnresolvedActors.AddUnique(Name);
            }
        }
    }
    else if (Ctx.GetBool(TEXT("selection"), false))
    {
        Config.bScopeActorsProvided = true;
        if (GEditor)
        {
            TArray<AActor*> Selected;
            GEditor->GetSelectedActors()->GetSelectedObjects(Selected);
            for (AActor* Actor : Selected)
            {
                if (Actor)
                {
                    Config.ScopeActors.AddUnique(TWeakObjectPtr<AActor>(Actor));
                }
            }
        }
    }

    Config.Offset = FMath::Max(Ctx.GetInt(TEXT("offset"), 0), 0);
    Config.Limit = FMath::Clamp(Ctx.GetInt(TEXT("limit"), AuditRpcDefaultLimit), 1, AuditRpcMaxLimit);

    const int32 MaxRows = FMath::Clamp(
        Ctx.GetInt(TEXT("maxFindings"),
            Ctx.GetInt(TEXT("max_findings"), AuditRpcDefaultMaxFindings)), 1, AuditRpcMaxFindings);

    const FString FailOnToken = Ctx.GetStringFirstOf({TEXT("failOn"), TEXT("fail_on")},
                                                     TEXT("error")).ToLower();
    PinWrightAudit::EFailOn FailOnMode;
    if (!PinWrightAudit::ParseFailOn(FailOnToken, FailOnMode))
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
            FString::Printf(TEXT("Unknown failOn '%s'. Valid: error, any, none."), *FailOnToken));
        return true;
    }
    const FString FailOn = PinWrightAudit::FailOnToWire(FailOnMode);

    // ---- Run ----
    LevelAudit::FReport Report;
    LevelAudit::Run(World, Config, Report);

    // ---- Response ----
    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();

    // `pass` is the shared verdict (Audit/AuditFramework.h), and its rule is stated in the
    // response so nobody has to infer it: any unrunnable check or a truncated sweep makes it
    // false regardless of failOn, because a check that did not run is not a check that passed.
    const bool bPass = Report.Verdict().DerivePass(FailOnMode);
    Data->SetBoolField(TEXT("pass"), bPass);
    Data->SetStringField(TEXT("failOn"), FailOn);
    Data->SetStringField(TEXT("passRule"), PinWrightAudit::PassRuleText(/*bIncludeTruncation=*/true));

    TSharedPtr<FJsonObject> WorldObj = MakeShared<FJsonObject>();
    WorldObj->SetStringField(TEXT("name"), World->GetName());
    WorldObj->SetStringField(TEXT("path"), World->GetPathName());
    WorldObj->SetStringField(TEXT("mode"), ResolvedWorldMode);
    Data->SetObjectField(TEXT("world"), WorldObj);

    TSharedPtr<FJsonObject> Summary = MakeShared<FJsonObject>();
    Summary->SetNumberField(TEXT("actorsInWorld"), Report.ActorsInWorld);
    Summary->SetNumberField(TEXT("actorsMatched"), Report.ActorsMatched);
    Summary->SetNumberField(TEXT("actorsExamined"), Report.ActorsExamined);
    Summary->SetNumberField(TEXT("actorsIgnored"), Report.ActorsIgnored);
    Summary->SetNumberField(TEXT("actorsFlagged"), Report.ActorsFlagged);
    Summary->SetNumberField(TEXT("findings"), Report.Findings.Num());
    Summary->SetNumberField(TEXT("errors"), Report.ErrorCount);
    Summary->SetNumberField(TEXT("warnings"), Report.WarningCount);
    Summary->SetNumberField(TEXT("unrunnable"), Report.UnrunnableCount);
    Summary->SetBoolField(TEXT("truncated"), Report.bTruncated);
    Data->SetObjectField(TEXT("summary"), Summary);

    // Every check, selected or not, with its five buckets. The two identities
    // (applicable + notApplicable + ignored == actorsExamined, and
    //  flagged + unrunnable + clean == applicable) are what make a silent stop detectable.
    TArray<TSharedPtr<FJsonValue>> CheckRows;
    for (const LevelAudit::FCheckInfo& Info : LevelAudit::AllChecks())
    {
        const LevelAudit::FCheckTally& Tally = Report.Tallies[static_cast<int32>(Info.Check)];
        const bool bSelected = LevelAudit::HasCheck(Config.SelectedChecks, Info.Check);
        TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
        Row->SetStringField(TEXT("id"), Info.Id);
        Row->SetStringField(TEXT("code"), Info.Code);
        Row->SetStringField(TEXT("severity"), PinWrightAudit::SeverityToWire(Info.Severity));
        Row->SetBoolField(TEXT("selected"), bSelected);
        if (bSelected)
        {
            Row->SetNumberField(TEXT("applicable"), Tally.Applicable);
            Row->SetNumberField(TEXT("notApplicable"), Tally.NotApplicable);
            Row->SetNumberField(TEXT("ignored"), Tally.Ignored);
            Row->SetNumberField(TEXT("flagged"), Tally.Flagged);
            Row->SetNumberField(TEXT("unrunnable"), Tally.Unrunnable);
            Row->SetNumberField(TEXT("clean"), Tally.Clean);
        }
        else
        {
            // "not selected" is a THIRD state beside "ran and found nothing" and "could not
            // run". Collapsing it into a zero count is how a caller concludes a level is clean
            // of something nobody looked for.
            const TCHAR* Reason =
                (Info.bNeedsSurface && !Config.bHasSurface)
                    ? TEXT("not selected: needs a `surface` argument")
                : (Info.bNeedsPlayArea && !Config.PlayArea.bValid)
                    ? TEXT("not selected: needs a `playArea` argument")
                : ExplicitChecks.Num() > 0
                    ? TEXT("not selected: not named in `checks`")
                : TEXT("not selected: not in the default set, or excluded");
            Row->SetStringField(TEXT("notSelectedReason"), Reason);
        }
        Row->SetStringField(TEXT("summary"), Info.Summary);
        CheckRows.Add(MakeShared<FJsonValueObject>(Row));
    }
    Data->SetArrayField(TEXT("checks"), CheckRows);

    TSharedPtr<FJsonObject> ThresholdEcho = MakeShared<FJsonObject>();
    ThresholdEcho->SetNumberField(TEXT("zeroScaleEpsilon"), T.ZeroScaleEpsilon);
    ThresholdEcho->SetNumberField(TEXT("maxScale"), T.MaxScale);
    ThresholdEcho->SetNumberField(TEXT("maxBoundsCm"), T.MaxBoundsCm);
    ThresholdEcho->SetNumberField(TEXT("originToleranceCm"), T.OriginToleranceCm);
    ThresholdEcho->SetNumberField(TEXT("duplicateToleranceCm"), T.DuplicateToleranceCm);
    ThresholdEcho->SetNumberField(TEXT("duplicateAngleToleranceDeg"), T.DuplicateAngleToleranceDeg);
    ThresholdEcho->SetNumberField(TEXT("killZMarginCm"), T.KillZMarginCm);
    ThresholdEcho->SetNumberField(TEXT("worldBoundsCm"), T.WorldBoundsCm);
    ThresholdEcho->SetNumberField(TEXT("playAreaMarginCm"), T.PlayAreaMarginCm);
    ThresholdEcho->SetNumberField(TEXT("maxGapCm"), T.MaxGapCm);
    ThresholdEcho->SetNumberField(TEXT("minCoverDepthCm"), T.MinCoverDepthCm);
    ThresholdEcho->SetNumberField(TEXT("deepCoverDepthCm"), T.DeepCoverDepthCm);
    ThresholdEcho->SetNumberField(TEXT("maxEmbedFraction"), T.MaxEmbedFraction);
    ThresholdEcho->SetNumberField(TEXT("balancedMinFootprintCm"), T.BalancedMinFootprintCm);
    ThresholdEcho->SetNumberField(TEXT("balancedMinContactPoints"), T.BalancedMinContactPoints);
    ThresholdEcho->SetNumberField(TEXT("minCoverage"), T.MinCoverage);
    ThresholdEcho->SetNumberField(TEXT("contactToleranceCm"), T.ContactToleranceCm);
    Data->SetObjectField(TEXT("thresholds"), ThresholdEcho);

    if (Config.bHasSurface)
    {
        TSharedPtr<FJsonObject> SurfaceEcho = MakeShared<FJsonObject>();
        SurfaceEcho->SetStringField(TEXT("preset"),
            GroundPlacement::SurfacePresetToString(Config.Surface.Preset));
        SurfaceEcho->SetBoolField(TEXT("traceComplex"), Config.Surface.bTraceComplex);
        SurfaceEcho->SetNumberField(TEXT("maxLayers"), Config.Surface.MaxLayers);
        SurfaceEcho->SetNumberField(TEXT("maxDropCm"), Config.Surface.MaxDropCm);
        SurfaceEcho->SetNumberField(TEXT("probeLiftCm"), Config.Surface.ProbeLiftCm);
        AuditRpcWriteStrings(SurfaceEcho, TEXT("onlyClasses"), Config.Surface.Filter.OnlyClasses);
        AuditRpcWriteStrings(SurfaceEcho, TEXT("excludeClasses"),
                             Config.Surface.Filter.ExcludeClasses);
        AuditRpcWriteStrings(SurfaceEcho, TEXT("excludeNames"), Config.Surface.Filter.ExcludeNames);
        AuditRpcWriteStrings(SurfaceEcho, TEXT("unresolvedIgnoreActors"), UnresolvedIgnoreActors);
        Data->SetObjectField(TEXT("surface"), SurfaceEcho);

        TSharedPtr<FJsonObject> GroundEcho = MakeShared<FJsonObject>();
        GroundEcho->SetNumberField(TEXT("samples"), Config.GridSize);
        GroundEcho->SetNumberField(TEXT("footprintInset"), Config.FootprintInset);
        GroundEcho->SetStringField(TEXT("undersideModel"),
            Config.UndersideModel == GroundPlacement::EUndersideModel::BoundsPlane
                ? TEXT("bounds_plane") : TEXT("mesh"));
        GroundEcho->SetNumberField(TEXT("measured"), Report.GroundMeasured);
        GroundEcho->SetNumberField(TEXT("budget"), Config.MaxGroundActors);
        GroundEcho->SetNumberField(TEXT("budgetExhausted"), Report.GroundBudgetExhausted);
        Data->SetObjectField(TEXT("ground"), GroundEcho);
    }

    if (Config.PlayArea.bValid)
    {
        TSharedPtr<FJsonObject> AreaEcho = MakeShared<FJsonObject>();
        AreaEcho->SetStringField(TEXT("source"), LevelAudit::PlayAreaSourceToString(Config.PlayArea.Source));
        AreaEcho->SetStringField(TEXT("description"), Config.PlayArea.Description);
        AreaEcho->SetObjectField(TEXT("min"), AuditRpcVector(Config.PlayArea.Box.Min));
        AreaEcho->SetObjectField(TEXT("max"), AuditRpcVector(Config.PlayArea.Box.Max));
        AreaEcho->SetNumberField(TEXT("marginCm"), Config.PlayArea.MarginCm);
        AreaEcho->SetBoolField(TEXT("ignoreZ"), Config.PlayArea.bIgnoreZ);
        Data->SetObjectField(TEXT("playArea"), AreaEcho);
    }

    TSharedPtr<FJsonObject> IgnoreEcho = MakeShared<FJsonObject>();
    IgnoreEcho->SetStringField(TEXT("tag"), Config.IgnoreTag.IsNone()
        ? FString() : Config.IgnoreTag.ToString());
    TArray<TSharedPtr<FJsonValue>> RuleRows;
    for (int32 Index = 0; Index < Report.IgnoreRules.Num(); ++Index)
    {
        const LevelAudit::FIgnoreRule& Rule = Report.IgnoreRules[Index];
        TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
        Row->SetNumberField(TEXT("index"), Index);
        Row->SetStringField(TEXT("source"), Rule.bBuiltIn ? TEXT("default") : TEXT("caller"));
        Row->SetStringField(TEXT("reason"), Rule.Reason);
        Row->SetNumberField(TEXT("matchedActors"), Rule.MatchedActors);
        AuditRpcWriteStrings(Row, TEXT("classes"), Rule.Classes);
        AuditRpcWriteStrings(Row, TEXT("actors"), Rule.Actors);
        AuditRpcWriteStrings(Row, TEXT("names"), Rule.NamePatterns);
        if (Rule.Tags.Num() > 0)
        {
            TArray<FString> TagStrings;
            for (const FName& Tag : Rule.Tags) { TagStrings.Add(Tag.ToString()); }
            AuditRpcWriteStrings(Row, TEXT("tags"), TagStrings);
        }
        TArray<FString> RuleChecks;
        for (const LevelAudit::FCheckInfo& Info : LevelAudit::AllChecks())
        {
            if (Rule.CheckMask == 0 || LevelAudit::HasCheck(Rule.CheckMask, Info.Check))
            {
                RuleChecks.Add(Info.Id);
            }
        }
        AuditRpcWriteStrings(Row, TEXT("checks"), RuleChecks);
        RuleRows.Add(MakeShared<FJsonValueObject>(Row));
    }
    if (RuleRows.Num() > 0)
    {
        IgnoreEcho->SetArrayField(TEXT("rules"), RuleRows);
    }
    Data->SetObjectField(TEXT("ignore"), IgnoreEcho);

    // World facts the numbers depend on.
    if (Report.KillZ.IsSet())
    {
        Data->SetNumberField(TEXT("killZ"), Report.KillZ.GetValue());
    }
    Data->SetBoolField(TEXT("worldBoundsChecksEnabled"), Report.bWorldBoundsChecksEnabled);
    Data->SetNumberField(TEXT("engineHalfWorldMaxCm"), Report.EngineHalfWorldMaxCm);
    if (Report.bWorldPartition)
    {
        TSharedPtr<FJsonObject> Partition = MakeShared<FJsonObject>();
        Partition->SetNumberField(TEXT("actorDescriptors"), Report.ActorDescCount);
        Partition->SetNumberField(TEXT("unloadedActors"), Report.UnloadedActorCount);
        Data->SetObjectField(TEXT("worldPartition"), Partition);
    }

    AuditRpcWriteStrings(Data, TEXT("caveats"), Report.Caveats);
    AuditRpcWriteStrings(Data, TEXT("unresolvedActors"), UnresolvedActors);

    if (bIncludeRows)
    {
        TArray<TSharedPtr<FJsonValue>> Rows;
        int32 Dropped = 0;
        for (const LevelAudit::FFinding& Finding : Report.Findings)
        {
            if (Rows.Num() < MaxRows)
            {
                Rows.Add(MakeShared<FJsonValueObject>(AuditRpcFindingObject(Finding)));
            }
            else
            {
                ++Dropped;
            }
        }
        if (Rows.Num() > 0)
        {
            Data->SetArrayField(TEXT("findings"), Rows);
        }
        if (Dropped > 0)
        {
            Data->SetBoolField(TEXT("findingsTruncated"), true);
            Data->SetNumberField(TEXT("findingsDropped"), Dropped);
        }
    }
    if (Report.CleanActors.Num() > 0)
    {
        AuditRpcWriteStrings(Data, TEXT("clean"), Report.CleanActors);
    }

    Data->SetStringField(TEXT("units"), TEXT("cm"));
    Data->SetStringField(TEXT("axis"), TEXT("+X fwd, +Y right, +Z up, left-handed"));

    UE_LOG(LogPinWrightSubsystem, Display,
        TEXT("level.audit: %d/%d actor(s) examined, %d flagged, %d error(s), %d warning(s), "
             "%d unrunnable"),
        Report.ActorsExamined, Report.ActorsInWorld, Report.ActorsFlagged, Report.ErrorCount,
        Report.WarningCount, Report.UnrunnableCount);

    Ctx.SendSuccess(Data);
    return true;
}
