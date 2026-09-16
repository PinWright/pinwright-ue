// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once
#include "CoreMinimal.h"
#include "Audit/AuditFramework.h" // the shared check-table / finding / verdict contract
#include "Dom/JsonObject.h"
#include "Handlers/Spatial/GroundPlacementUtils.h"
#include "UObject/WeakObjectPtr.h"
#include "Utils/NameMatchFilter.h" // NameMatch::FFilter - the shared name/label match policy

class AActor;
class UWorld;

// Level audit: one game-thread sweep over every actor in a world that reports the actors
// which are demonstrably wrong, with per-check accounting of what was and was not measured.
//
// Three design constraints, each from a defect this project actually shipped:
//
//   1. ONE CALL, C++ SIDE. A per-actor Python loop over a level wedged an editor for 168
//      minutes across 5100 uncancellable calls. Run() walks the world once and returns one
//      report; there is deliberately no per-actor entry point in the RPC layer.
//   2. AN UNRUN CHECK IS NOT A PASSED CHECK. Every check reports, per actor, one of
//      {flagged, clean, unrunnable, not-applicable}, and the four counts sum to the number
//      of actors examined. A check that could not run (no collision, no bounds, unresolvable
//      support chain, trace budget spent) produces an EFindingStatus::Unrunnable row naming
//      the reason. Silence never means "fine".
//   3. MOST BURIED ACTORS ARE BURIED ON PURPOSE. 620 of 2759 actors in this project's map
//      measured as underground and most of them are foundations and bedded rock. The check
//      that distinguishes them is not "how deep is the underside" (which is the same number
//      for both) but "is anything ABOVE the actor" - see FGroundMeasure::CoverDepthCm.
//
// Engine reuse: the transform / mesh / material / KillZ checks below re-derive nothing that
// AActor::CheckForErrors (UE 5.8 Runtime/Engine/Classes/GameFramework/Actor.h:2720) already
// reports, because that hook writes into an FMessageLog listing and exposes no structured
// result; and the engine's own out-of-world enforcement,
// AActor::CheckStillInWorld (Actor.h:3005, implemented Actor.cpp:2300), is RUNTIME-only,
// destroys the actor it finds (FellOutOfWorld / SetActorEnableCollision(false),
// Actor.cpp:2327-2345), tests the PIVOT rather than the bounds, and no-ops entirely when
// AWorldSettings::AreWorldBoundsChecksEnabled() is false (WorldSettings.h:904). None of that
// is usable from a read-only editor-time audit. See Docs on the level.audit wiki page.
//
// Coordinates are unreal units (cm), left-handed (+X fwd, +Y right, +Z up). Game-thread only.
namespace LevelAudit
{
    // ---- Checks -------------------------------------------------------------------------

    // One entry per check. The ordering is the response ordering; nothing may be reordered
    // without changing the wire order of checks[] (the bit index is NOT serialized, so a
    // reorder is safe for stored configs - only for readers who compare arrays positionally).
    enum class ECheck : uint8
    {
        NanTransform = 0,
        ZeroScale,
        NegativeScale,
        ExtremeScale,
        MissingMesh,
        MissingMaterial,
        AtWorldOrigin,
        BelowKillZ,
        OutsideWorldBounds,
        DuplicateTransform,
        OutsidePlayArea,
        BelowSurface,
        DeeplyEmbedded,
        Airborne,
        BalancedOnPoint,
        UnsupportedAssembly,
        // The one check that is a WRAPPER rather than a reimplementation: it calls the
        // engine's own per-actor AActor::IsDataValid(FDataValidationContext&) (Actor.h:3009,
        // ENGINE_API, WITH_EDITOR, const) and republishes whatever the engine and any
        // project/plugin validator on the class had to say. See the .cpp for why this hook
        // and not AActor::CheckForErrors().
        DataValidation,
        Count
    };

    // Number of checks. Also the width of the selection bitmask, which must stay <= 32.
    constexpr int32 CheckCount = static_cast<int32>(ECheck::Count);
    static_assert(CheckCount <= 32, "The check selection bitmask is a uint32.");

    // Severity, finding status and the verdict come from the one shared audit contract
    // (Audit/AuditFramework.h), which level.audit's own rule was the first copy of. Aliased
    // rather than redeclared so LevelAudit::ESeverity::Error stays the spelling at every call
    // site while there is only one definition of the type. Warning is reported but does not
    // fail under the default failOn:"error"; Error is reserved for conditions that cannot be
    // deliberate for a visible, placed actor.
    using ESeverity = PinWrightAudit::ESeverity;
    using EFindingStatus = PinWrightAudit::EFindingStatus;

    struct FCheckInfo
    {
        ECheck Check;
        const TCHAR* Id;       // wire id, e.g. "zero_scale"
        const TCHAR* Code;     // ErrorCodes.h ERR_* value emitted on a finding
        ESeverity Severity;
        // In the default check set. Everything requiring an extra argument, and everything
        // known to be noisy on real content, is off by default and must be asked for.
        bool bDefaultOn;
        bool bNeedsSurface;    // requires a ground measurement (and therefore `surface`)
        bool bNeedsPlayArea;   // requires `playArea`
        const TCHAR* Summary;
    };

    // Every check, in ECheck order. Index i is the check whose ECheck value is i.
    const TArray<FCheckInfo>& AllChecks();

    // The rest are the shared contract's operations over this table (Audit/AuditFramework.h).
    inline const FCheckInfo& CheckInfo(ECheck Check) { return PinWrightAudit::CheckInfo(AllChecks(), Check); }
    // Wire id -> ECheck. False for an unknown id, which callers must turn into an error
    // rather than a silent no-op: a typo in `checks` that silently ran nothing would be
    // indistinguishable from a clean level.
    inline bool ParseCheckId(const FString& Id, ECheck& OutCheck)
    {
        return PinWrightAudit::ParseCheckId(AllChecks(), Id, OutCheck);
    }

    inline uint32 CheckBit(ECheck Check) { return PinWrightAudit::CheckBit(Check); }
    inline bool HasCheck(uint32 Mask, ECheck Check) { return PinWrightAudit::HasCheck(Mask, Check); }
    // Mask of every check whose bNeedsSurface is set - i.e. the checks that make the sweep
    // pay for a footprint ground measurement per actor.
    inline uint32 SurfaceCheckMask()
    {
        return PinWrightAudit::MaskWhere(AllChecks(), [](const FCheckInfo& Info) { return Info.bNeedsSurface; });
    }
    inline uint32 PlayAreaCheckMask()
    {
        return PinWrightAudit::MaskWhere(AllChecks(), [](const FCheckInfo& Info) { return Info.bNeedsPlayArea; });
    }
    inline uint32 DefaultCheckMask() { return PinWrightAudit::DefaultCheckMask(AllChecks()); }

    // ---- Thresholds ---------------------------------------------------------------------

    // Every number the audit compares against, with the value it defaults to and why. All are
    // caller-overridable, and all are echoed in the response: a lint whose thresholds are not
    // visible in its own output cannot be argued with.
    struct FThresholds
    {
        // A scale axis at or below this is degenerate. 1e-4 on a 1 m mesh is 0.01 mm - not a
        // small object, an invisible one - and is three orders of magnitude above the float
        // noise (~1e-7) around a scale of 1.
        double ZeroScaleEpsilon = 1.0e-4;

        // Largest |scale| on any axis before the actor is called extreme. 100x turns a 1 m
        // prop into a 100 m one. The observed defect here was a rim rock measuring 4472 cm
        // across - wider than the pit floor it bordered - from an uncapped scale multiplier.
        double MaxScale = 100.0;

        // Largest world-space bounds dimension before the actor is called extreme, in cm.
        // Measures the RESULT rather than the multiplier, because a 5x scale on a large mesh
        // and a 50x scale on a small one are the same defect. 500 m is far larger than any
        // hand-placed prop and far smaller than a landscape (which the default ignore rules
        // exempt from this check anyway).
        double MaxBoundsCm = 50000.0;

        // Distance from (0,0,0) within which an actor counts as sitting on the world origin.
        double OriginToleranceCm = 1.0;

        // Two actors count as duplicates when their locations agree within this and their
        // rotations within DuplicateAngleToleranceDeg, and they share a class.
        double DuplicateToleranceCm = 1.0;
        double DuplicateAngleToleranceDeg = 0.5;

        // Extra margin below AWorldSettings::KillZ before an actor is reported. 0 = the
        // engine's own line. The check uses the actor's bounds TOP, not its pivot, so an
        // actor is only reported when all of it is below the line - the engine's runtime
        // check (Actor.cpp:2326) uses the pivot and would fire earlier.
        double KillZMarginCm = 0.0;

        // Half-extent of the practical world box, in cm. Defaults to UE_OLD_HALF_WORLD_MAX
        // (1,048,576 cm, ~10.5 km from the origin) and NOT to the engine's own HALF_WORLD_MAX:
        // under Large World Coordinates that macro is UE_LARGE_WORLD_MAX * 0.5 =
        // 4.398e12 cm, ~43.98 million km (Runtime/Engine/Public/EngineDefines.h:41-56), so
        // testing against it would pass an actor a thousand kilometres off the map. The
        // engine's own value is echoed in the response beside this one.
        double WorldBoundsCm = 1048576.0;

        // Margin added around the resolved play area before an actor outside it is reported.
        double PlayAreaMarginCm = 0.0;

        // Largest clearance in cm between the CLOSEST part of the actor and the surface
        // before it counts as airborne. Deliberately the minimum column gap, not the maximum:
        // an actor with one column touching is resting, however far the far side is off the
        // ground on a slope - that case is BalancedOnPoint's, not Airborne's.
        double MaxGapCm = 2.0;

        // BelowSurface fires when the surface is above the actor's bounds TOP by more than
        // this in every sampled column. 0 = "nothing of this actor is above the ground".
        double MinCoverDepthCm = 0.0;
        // Above this cover depth the finding is reported at severity error rather than
        // warning even under a caller who lowered MinCoverDepthCm. 1000 cm is the depth past
        // which 170 of this project's 620 underground actors sat.
        double DeepCoverDepthCm = 1000.0;

        // DeeplyEmbedded (off by default) fires when penetration exceeds this fraction of the
        // actor's own bounds height. 0.5 = more than half of it is under the surface.
        // Deliberate bedding is normally a few percent (spatial.ground_actors defaults to
        // 0.02); a foundation is legitimately 1.0, which is why this check is opt-in.
        double MaxEmbedFraction = 0.5;

        // BalancedOnPoint only applies to actors at least this wide, because one contact
        // column under a 20 cm pebble is a correct rest and one under a 4 m boulder is a
        // physics glitch.
        double BalancedMinFootprintCm = 100.0;
        // Contact columns required for an actor above that size.
        int32 BalancedMinContactPoints = 2;

        // Fraction of an actor's own footprint columns that must find ground before the
        // ground-relative checks are considered measurable at all. Below it the actor
        // overhangs a hole or the terrain edge and its gap numbers describe a fragment.
        double MinCoverage = 0.5;

        // A column counts as touching at or below this clearance, in cm. Shared with
        // GroundPlacement::FContactThresholds so "in contact" means one thing in this plugin.
        double ContactToleranceCm = GroundPlacement::DefaultContactToleranceCm;
    };

    // ---- Ignore rules --------------------------------------------------------------------

    // One ignore rule. A rule matches an actor when EVERY populated criterion matches (AND),
    // and applies to the checks in CheckMask (0 = every check).
    //
    // Why an ARRAY of rules rather than one flat list: "exempt the landscape from the size
    // check" and "exempt this one rock from the burial check" are different statements, and a
    // single flat ignore list can only express "never look at this actor again", which is how
    // an ignore list quietly grows into a blindfold. Each rule also carries a Reason, echoed
    // beside the number of actors it matched, so an over-broad rule shows up in the report
    // instead of just making findings disappear.
    struct FIgnoreRule
    {
        // Exact display label OR internal name OR object path. Compared case-insensitively.
        TArray<FString> Actors;
        // FString::MatchesWildcard patterns (`*`, `?`), case-insensitive, against the actor's
        // internal name and display label.
        TArray<FString> NamePatterns;
        // Class name or /Script path, matched as a case-insensitive substring against the
        // actor's class AND every ancestor class - so one "LandscapeProxy" entry covers
        // ALandscape and ALandscapeStreamingProxy.
        TArray<FString> Classes;
        // AActor::Tags entries (Actor.h:2073, ActorHasTag).
        TArray<FName> Tags;

        // Checks this rule silences. 0 = all of them.
        uint32 CheckMask = 0;

        FString Reason;
        // True for the rules the audit ships with, so the response can separate "the audit
        // did this" from "you asked for this".
        bool bBuiltIn = false;

        // Filled during the sweep, reported afterwards. Deliberately only the number of actors
        // the rule MATCHED, not the number of findings it suppressed: the audit never runs a
        // silenced check, so it does not know what would have been found, and reporting a
        // guess there would be the same kind of invented number this whole feature exists to
        // stop.
        int32 MatchedActors = 0;

        bool IsEmpty() const
        {
            return Actors.Num() == 0 && NamePatterns.Num() == 0 && Classes.Num() == 0
                && Tags.Num() == 0;
        }
    };

    // The rules the audit applies unless the caller passes useDefaultIgnores:false. Each one
    // exists because the condition it silences is CORRECT for that class, not because the
    // finding was inconvenient.
    TArray<FIgnoreRule> DefaultIgnoreRules();

    // ---- Play area -----------------------------------------------------------------------

    enum class EPlayAreaSource : uint8
    {
        // Union of every ALandscapeProxy's component bounds. The terrain footprint IS the
        // playable area on a terrain map, and unlike level bounds it is not inflated by the
        // very outliers the check is looking for.
        Landscape,
        // McpActorUtils::SumActorBounds over the world. Honest but self-inflating: a prop
        // 5 km off the map is INSIDE the box it helped compute. Reported as a caveat.
        LevelBounds,
        // Bounds of a named actor (a volume, a bounds actor, a blocking volume).
        Actor,
        // Caller-supplied box.
        Box
    };

    struct FPlayArea
    {
        bool bValid = false;
        EPlayAreaSource Source = EPlayAreaSource::Landscape;
        FBox Box = FBox(ForceInit);
        // Margin added on every axis before an actor counts as outside.
        double MarginCm = 0.0;
        // Test XY only. Default true: "far from the play area" is a plan-view question, and
        // a level-bounds Z is dominated by sky/atmosphere actors kilometres up, which would
        // make the Z term meaningless. Vertical strays are Airborne's and BelowKillZ's job.
        bool bIgnoreZ = true;
        // Human text naming what the box came from, echoed in the response.
        FString Description;
    };

    const TCHAR* PlayAreaSourceToString(EPlayAreaSource Source);
    bool ParsePlayAreaSource(const FString& Name, EPlayAreaSource& OutSource);
    // Resolve the box for Source. Returns false with OutError set when the source produced
    // nothing (no landscape in the world, named actor not found, degenerate box) - never a
    // silent fallback to a different source, because an audit that measured against a box the
    // caller did not ask for is worse than one that refused.
    bool ResolvePlayArea(UWorld* World, FPlayArea& InOutArea, const FString& ActorName,
                         FString& OutError);

    // ---- Ground measurement --------------------------------------------------------------

    // The ground-relative facts about one actor, derived from ONE
    // GroundPlacement::MeasureContact call plus the actor's own AABB. Everything here is
    // computed from the per-column samples that call already produces; nothing re-traces.
    struct FGroundMeasure
    {
        // The measurement was attempted AND produced columns. False = every ground check is
        // Unrunnable for this actor.
        bool bMeasured = false;
        // Reason the measurement is unusable, as an ErrorCodes.h value. Empty iff bMeasured
        // and at least one column found ground.
        FString UnrunnableCode;
        FString UnrunnableReason;

        int32 ActorColumns = 0;
        int32 SupportedColumns = 0;
        int32 ContactPoints = 0;
        double Coverage = 0.0;

        // Clearances across supported columns. MinGapCm is what "the whole actor floats"
        // means; MaxGapCm is what "part of it hangs" means; they answer different questions
        // and the audit uses each for exactly one check.
        double MinGapCm = 0.0;
        double MaxGapCm = 0.0;
        double PenetrationCm = 0.0;

        // The term the previous instruments could not produce. Lowest accepted ground Z
        // across supported columns MINUS the actor's bounds top. Positive means EVERY
        // sampled column has a surface above the actor's highest point: the actor is
        // entirely under the ground, which is the difference between deliberate bedding
        // (some of it is still visible) and having fallen through the world.
        double CoverDepthCm = 0.0;
        // Same, using the HIGHEST accepted ground Z: how deep the most-covered column is.
        // Informational; nothing keys off it.
        double MaxCoverDepthCm = 0.0;

        // PenetrationCm / bounds height, clamped at 0. The "how much of it is under" measure
        // that CoverDepthCm deliberately is not.
        double EmbedFraction = 0.0;

        double BoundsHeightCm = 0.0;
        double FootprintMaxCm = 0.0;

        // The actor the contact columns rest on, when it is not terrain. This is the only
        // cross-actor structural fact the sweep can observe, and it is what makes
        // UnsupportedAssembly possible at all.
        TWeakObjectPtr<AActor> SupportActor;
        // The support actor is an ALandscapeProxy, i.e. the chain terminates in terrain.
        bool bSupportIsTerrain = false;

        // GroundPlacement fell back to the flat bounds plane because the actor answered no
        // geometry query anywhere. The numbers still mean something, but a different
        // something; reported per actor rather than folded away.
        bool bBoundsPlaneFallback = false;
        // The surface filter refused everything under this actor, naming what it refused.
        int32 RejectedSurfaceActorCount = 0;
        TArray<FString> RejectedSurfaceActors;
        // Set only when nothing was found: false = the landscape reports no height at this
        // XY either, i.e. the actor is not over terrain at all.
        TOptional<bool> bOverLandscape;
    };

    // ---- Findings ------------------------------------------------------------------------

    struct FFinding
    {
        FString ActorLabel;
        // GetName(). The label a finding names is not unique, so a caller acting on the row
        // can address the wrong actor; ActorObjectName is unique within the level and is the
        // collision-safe key to resolve the flagged actor by.
        FString ActorObjectName;
        FString ActorPath;
        FString ActorClass;
        ECheck Check = ECheck::NanTransform;
        EFindingStatus Status = EFindingStatus::Flagged;
        ESeverity Severity = ESeverity::Warning;
        // An ErrorCodes.h value. For Flagged it is the check's own code; for Unrunnable it is
        // the reason the check could not run (GROUND_NOT_MEASURED, ACTOR_HAS_NO_BOUNDS, ...).
        FString Code;
        FString Message;
        // Check-specific numbers, always the ones the verdict was derived from.
        TSharedPtr<FJsonObject> Measurements;
    };

    // Per-check accounting. Two identities hold for every selected check and are asserted by
    // the tests, because an actor that falls out of every bucket is exactly how a check
    // silently stops running:
    //     Applicable + NotApplicable + Ignored == actorsExamined
    //     Flagged + Unrunnable + Clean         == Applicable
    struct FCheckTally
    {
        int32 Applicable = 0;
        // The check does not mean anything for this actor's class or state (a mesh check on a
        // light, a ground check on an actor with no footprint). Distinct from Unrunnable,
        // which means the check DOES apply and could not be evaluated.
        int32 NotApplicable = 0;
        // Silenced by an ignore rule covering this check.
        int32 Ignored = 0;
        int32 Flagged = 0;
        int32 Unrunnable = 0;
        int32 Clean = 0;
    };

    // ---- Configuration --------------------------------------------------------------------

    struct FConfig
    {
        uint32 SelectedChecks = 0;
        FThresholds Thresholds;
        TArray<FIgnoreRule> IgnoreRules;
        // Actors carrying this tag are exempt from every check. Empty disables the mechanism.
        FName IgnoreTag = FName(TEXT("PinWright.AuditIgnore"));

        // Ground sampling, identical in name and meaning to spatial.ground_actors /
        // spatial.verify_grounding, so a caller cannot get one answer from the audit and a
        // different one from the verb that fixes it.
        GroundPlacement::FGroundSurfaceSpec Surface;
        bool bHasSurface = false;
        int32 GridSize = GroundPlacement::DefaultGridSize;
        double FootprintInset = GroundPlacement::DefaultFootprintInset;
        GroundPlacement::EUndersideModel UndersideModel = GroundPlacement::EUndersideModel::MeshProfile;

        FPlayArea PlayArea;

        // Hard ceiling on how many actors may receive a footprint ground measurement in one
        // call. Actors past it are reported Unrunnable for every ground check rather than
        // skipped: a budget that silently stops measuring is the same failure as a check that
        // silently stops running.
        int32 MaxGroundActors = 5000;

        // Optional scope. All three unset = every actor in the world, which for an audit is
        // the point. spatial.ground_actors refuses that default deliberately, because there
        // an omitted selector would mean a whole-level MUTATION; a read-only sweep has the
        // opposite risk, where a narrow default would quietly audit a corner of the map and
        // report it clean.
        TArray<TWeakObjectPtr<AActor>> ScopeActors; // resolved by the caller (actors/selection)
        bool bScopeActorsProvided = false;
        FString Prefix;
        NameMatch::FFilter Filter;

        int32 Offset = 0;
        int32 Limit = 20000;

        // Cap on labels collected into FReport::CleanActors. 0 = do not collect them.
        int32 MaxCleanActors = 0;
    };

    struct FReport
    {
        int32 ActorsInWorld = 0;
        int32 ActorsMatched = 0;   // after the optional scope, before limit/offset
        int32 ActorsExamined = 0;  // actually walked in this call
        int32 ActorsIgnored = 0;   // matched an ignore rule covering EVERY check
        int32 ActorsFlagged = 0;
        bool bTruncated = false;

        int32 GroundMeasured = 0;
        int32 GroundBudgetExhausted = 0;

        TArray<FFinding> Findings;
        FCheckTally Tallies[CheckCount];
        TArray<FIgnoreRule> IgnoreRules; // echoed with MatchedActors filled in
        // Statements about what this sweep could NOT see, e.g. foliage instances, which are
        // not actors and are therefore invisible to an actor-level audit.
        TArray<FString> Caveats;
        // Labels of actors with zero findings, capped by the caller's detail setting.
        TArray<FString> CleanActors;

        int32 ErrorCount = 0;
        int32 WarningCount = 0;
        int32 UnrunnableCount = 0;

        // The four terms the shared verdict reads, in one place, so the handler cannot pick a
        // different set of them than the response's own counters report.
        // See PinWrightAudit::FVerdict::DerivePass for the rule and why failOn cannot reach
        // the unrunnable and truncation terms.
        PinWrightAudit::FVerdict Verdict() const
        {
            PinWrightAudit::FVerdict Out;
            Out.ErrorCount = ErrorCount;
            Out.WarningCount = WarningCount;
            Out.UnrunnableCount = UnrunnableCount;
            Out.bTruncated = bTruncated;
            return Out;
        }

        // World facts the caller needs in order to read the numbers above. KillZ is unset
        // when the world has no AWorldSettings, which is also what makes BelowKillZ
        // Unrunnable rather than clean.
        TOptional<double> KillZ;
        bool bWorldBoundsChecksEnabled = false;
        // The engine's own HALF_WORLD_MAX for this build, echoed beside the audit's much
        // smaller practical bound so the difference is visible rather than surprising.
        double EngineHalfWorldMaxCm = 0.0;

        // World Partition coverage. A TActorIterator sweep sees only LOADED actors
        // (FActorIteratorState builds its set from ULevel::Actors, EngineUtils.h:216-229), so
        // on a World Partition map an unloaded actor is invisible to every check here. These
        // two fields are how the report says so instead of implying the level is clean.
        bool bWorldPartition = false;
        int32 ActorDescCount = 0;
        int32 UnloadedActorCount = 0;
    };

    // Walk World once and fill OutReport. Non-mutating: nothing here calls Modify(),
    // SetActorTransform, Destroy, or MarkPackageDirty.
    void Run(UWorld* World, const FConfig& Config, FReport& OutReport);
}
