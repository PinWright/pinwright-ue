// Copyright (c) 2026 Alexander Penkin. MIT License.

// LevelAuditUtils.cpp - the one-pass level audit. See LevelAuditUtils.h for the design.
//
// ---------------------------------------------------------------------------------------
// What the engine already does, and why almost none of it is reusable here
// ---------------------------------------------------------------------------------------
// Verified against UE 5.8 (C:/UE_5.8/Engine/Source). Line numbers are that tree.
//
// AActor::CheckForErrors() (Actor.h:2720, ENGINE_API, WITH_EDITOR; body
// Private/ActorEditor.cpp:1652-1694) catches FOUR things and nothing else:
//   * deprecated / abstract class, via CheckForDeprecated (ActorEditor.cpp:1629) - and it
//     RETURNS EARLY if that produced a message (ActorEditor.cpp:1654-1659), so a deprecated
//     actor gets none of the checks below;
//   * a non-Movable root primitive with bSimulatePhysics (ActorEditor.cpp:1661-1670);
//   * zero scale (ActorEditor.cpp:1672-1684) - as the PRODUCT
//     `FMath::IsNearlyZero(Sx*Sy*Sz)`, which means (0.0001, 1, 1) trips it and (-1,-1,-1)
//     does not. It reports neither which axis nor how small;
//   * a fan-out to CheckForErrors on every REGISTERED component (ActorEditor.cpp:1686-1693) -
//     which is where the useful null-static-mesh warning actually lives
//     (StaticMeshComponent.cpp:611-617, FMapErrors::StaticMeshNull).
// Every one of those writes into FMessageLog("MapCheck") with no output parameter, no return
// value and no injectable sink. The only way to read them back is
// FMessageLogModule::GetLogListing("MapCheck")->GetFilteredMessages()
// (MessageLogModule.h:57, IMessageLogListing.h:47), which is page-scoped, subject to the
// listing's own UI severity filters (registered with bShowFilters=true at
// UnrealEdMisc.cpp:541-543), and reached through a single global delegate
// (FMessageLog::OnGetLog, MessageLog.h:110-114). An audit whose completeness depends on a
// Slate filter state is not an audit, so this file does NOT harvest that log.
//
// UEditorEngine::Map_Check (EditorEngine.h:2726) is `private:` (nearest specifier
// EditorEngine.h:2701) and therefore not callable from a plugin at all; the only entry point
// is GEditor->Exec(World, TEXT("MAP CHECK ...")) (EditorServer.cpp:6404-6421), which returns
// a bool and puts everything in that same listing. There is no MapCheck commandlet.
//
// So ONE engine hook is wrapped here rather than reimplemented, and it is the modern one:
// AActor::IsDataValid(FDataValidationContext&) (Actor.h:3009, ENGINE_API, WITH_EDITOR,
// const; body ActorEditor.cpp:1714-1741). It is structured (FDataValidationContext::GetIssues,
// Misc/DataValidation.h), lives in CoreUObject so it costs no new module dependency, touches
// no Slate, fans out to every component, and is the hook UEditorValidatorSubsystem itself
// drives - so anything a project or plugin validator says about an actor arrives here too.
// It is ECheck::DataValidation and it is OFF by default, because a third-party override can
// be arbitrarily expensive and this sweep must stay bounded.
//
// Everything else below has no engine equivalent at editor time. Confirmed absent in 5.8:
// an editor-time KillZ check (AActor::CheckStillInWorld, Actor.cpp:2300, is runtime-only,
// authority-gated, tests the PIVOT, and DESTROYS what it finds via FellOutOfWorld /
// SetActorEnableCollision(false), Actor.cpp:2326-2345; FMapErrors::NoKillZ, MapErrors.h:150,
// is declared with no emitter anywhere); NaN/Inf transform reporting (FTransform::IsValid,
// TransformNonVectorized.h:600, exists as a primitive but nothing validates with it, and
// DiagnosticCheckNaN_* compiles to {} whenever ENABLE_NAN_DIAGNOSTIC is 0, i.e. every
// non-debug editor build, UnrealMathUtility.h:13-20); negative or mirrored scale;
// non-uniform scale (FMapErrors::SimpleCollisionButNonUniformScale, MapErrors.h:218, also has
// no emitter); actors at the world origin; any notion of a play area; missing or
// default-substituted materials; anything "below the terrain"; and a general
// duplicate-at-identical-transform check - the engine has one only for AStaticMeshActor
// (StaticMeshActor.cpp:182-210, FMapErrors::SameLocation), and it needs a non-null mesh and a
// working collision query, so a collisionless or non-SMA duplicate is invisible to it.
//
// The world-bounds threshold deserves its own note. HALF_WORLD_MAX is not a usable lint bound
// on 5.8: EngineDefines.h:41-56 defines WORLD_MAX as UE_LARGE_WORLD_MAX (8.796e12 cm) unless
// UE_USE_UE4_WORLD_MAX is forced, so HALF_WORLD_MAX is ~4.4e12 cm - about 44 million km. An
// actor a thousand kilometres off the map passes it. FThresholds::WorldBoundsCm therefore
// defaults to 1,048,576 cm, which is simultaneously UE_OLD_HALF_WORLD_MAX (EngineDefines.h:38)
// and UE_FLOAT_HUGE_DISTANCE (EngineDefines.h:59, documented as the largest distance a float
// still holds to 1/16 cm). The engine's own value is echoed in the report beside it.
//
// Everything file-local lives in one anonymous namespace with an Audit* prefix, per the
// Unity-build rule in CLAUDE.md.

#include "Handlers/Level/LevelAuditUtils.h"

#include <initializer_list>

#include "Compat/EngineVersionCompat.h"
#include "Handlers/ErrorCodes.h"
#include "Handlers/Spatial/GroundPlacementUtils.h"
#include "Utils/ActorUtils.h"

#include "Components/InstancedStaticMeshComponent.h"
#include "Components/MeshComponent.h"
#include "Components/PrimitiveComponent.h"
#include "Components/SkinnedMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/World.h"
#include "EngineDefines.h" // HALF_WORLD_MAX (LWC-sized on 5.8 - see the header note)
#include "EngineUtils.h"   // TActorIterator, EActorIteratorFlags
#include "GameFramework/Actor.h"
#include "GameFramework/WorldSettings.h"
#include "LandscapeProxy.h"
#include "MaterialDomain.h" // EMaterialDomain / MD_Surface
#include "Materials/Material.h"
#include "Materials/MaterialInterface.h"
#include "Misc/DataValidation.h"
#include "WorldPartition/WorldPartition.h"
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 4, 0)
#include "WorldPartition/WorldPartitionActorDescInstance.h"
#else
#include "WorldPartition/WorldPartitionActorDesc.h"
#endif
#include "WorldPartition/WorldPartitionHelpers.h"

// The file-local helpers live in an unnamed namespace NESTED INSIDE LevelAudit, not at global
// scope. That gets them unqualified access to the LevelAudit types without a
// `using namespace LevelAudit;`, which under this plugin's Unity build (bUseUnity = true) would
// leak into every other translation unit merged into the same blob - and `FConfig` / `FReport`
// are exactly the kind of names that would then collide. Same reasoning as the named-namespace
// helper consolidation described in CLAUDE.md.
namespace LevelAudit
{
namespace
{
    // Bounds smaller than this on every axis mean the actor has no measurable footprint.
    // Same floor GroundPlacementUtils.cpp uses (GroundMinExtentCm), deliberately: an actor
    // that has no footprint for the seat solver has none for the audit either.
    constexpr double AuditMinExtentCm = 0.01;

    // Longest support chain the assembly check will walk before giving up and reporting the
    // actor Unrunnable. A stack deeper than this in a hand-placed level is far more likely to
    // be a measurement artefact than a real structure.
    constexpr int32 AuditMaxSupportDepth = 16;

    // Ceiling on how many names an ignore rule or a duplicate group echoes. Past a couple of
    // dozen the COUNT is the signal, not the roster - the same rule RaycastHandler.cpp:36 and
    // GroundPlacementUtils.cpp:24 apply.
    constexpr int32 AuditMaxEchoedNames = 16;

    // Stand-in for "this axis imposes no constraint" in the play-area test. A plain large
    // negative rather than a NaN or an optional, so FMath::Max3 stays a max.
    constexpr double AuditAxisIgnored = -1.0e30;

    // /Engine/EngineMaterials/WorldGridMaterial is what the editor substitutes for an
    // unassigned slot; an EXPLICIT assignment of it is a placeholder someone forgot to
    // replace, which is a different fact from a null slot and is reported as such.
    const TCHAR* AuditWorldGridMaterialPath =
        TEXT("/Engine/EngineMaterials/WorldGridMaterial.WorldGridMaterial");

    // True when the actor's class or ANY ancestor matches one of Patterns as a
    // case-insensitive substring of the bare class name OR the full /Script path. Ancestry is
    // what lets one "LandscapeProxy" entry cover ALandscape and ALandscapeStreamingProxy.
    //
    // Deliberately the same rule as SpatialTraceUtils.cpp's SpatialTraceMatchesAnyClass; it
    // lives in that file's anonymous namespace and cannot be reached from here, so the rule is
    // restated rather than reinvented. If one changes, change both.
    bool AuditMatchesAnyClass(const AActor* Actor, const TArray<FString>& Patterns)
    {
        if (!Actor)
        {
            return false;
        }
        for (const UClass* Cls = Actor->GetClass(); Cls; Cls = Cls->GetSuperClass())
        {
            const FString ClassName = Cls->GetName();
            const FString ClassPath = Cls->GetPathName();
            for (const FString& Wanted : Patterns)
            {
                if (ClassName.Contains(Wanted, ESearchCase::IgnoreCase)
                    || ClassPath.Contains(Wanted, ESearchCase::IgnoreCase))
                {
                    return true;
                }
            }
        }
        return false;
    }

    bool AuditMatchesAnyName(const TArray<FString>& Values, const FString& Label,
                             const FString& Name, const FString& Path)
    {
        for (const FString& Value : Values)
        {
            if (Value.IsEmpty())
            {
                continue;
            }
            if (Label.Equals(Value, ESearchCase::IgnoreCase)
                || Name.Equals(Value, ESearchCase::IgnoreCase)
                || Path.Equals(Value, ESearchCase::IgnoreCase))
            {
                return true;
            }
        }
        return false;
    }

    bool AuditMatchesAnyPattern(const TArray<FString>& Patterns, const FString& Label,
                                const FString& Name)
    {
        for (const FString& Pattern : Patterns)
        {
            if (Pattern.IsEmpty())
            {
                continue;
            }
            if (Label.MatchesWildcard(Pattern, ESearchCase::IgnoreCase)
                || Name.MatchesWildcard(Pattern, ESearchCase::IgnoreCase))
            {
                return true;
            }
        }
        return false;
    }

    // Every populated criterion must match (AND). An empty rule matches nothing rather than
    // everything: a rule that was meant to name something and named nothing must not silence
    // the whole level.
    bool AuditRuleMatches(const FIgnoreRule& Rule, AActor* Actor, const FString& Label,
                          const FString& Name, const FString& Path)
    {
        if (Rule.IsEmpty())
        {
            return false;
        }
        if (Rule.Actors.Num() > 0 && !AuditMatchesAnyName(Rule.Actors, Label, Name, Path))
        {
            return false;
        }
        if (Rule.NamePatterns.Num() > 0 && !AuditMatchesAnyPattern(Rule.NamePatterns, Label, Name))
        {
            return false;
        }
        if (Rule.Classes.Num() > 0 && !AuditMatchesAnyClass(Actor, Rule.Classes))
        {
            return false;
        }
        if (Rule.Tags.Num() > 0)
        {
            bool bHasTag = false;
            for (const FName& Tag : Rule.Tags)
            {
                if (Actor && Actor->ActorHasTag(Tag))
                {
                    bHasTag = true;
                    break;
                }
            }
            if (!bHasTag)
            {
                return false;
            }
        }
        return true;
    }

    TSharedPtr<FJsonObject> AuditVectorObject(const FVector& Vec)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetNumberField(TEXT("x"), Vec.X);
        Obj->SetNumberField(TEXT("y"), Vec.Y);
        Obj->SetNumberField(TEXT("z"), Vec.Z);
        return Obj;
    }

    // ---- Per-actor working record ---------------------------------------------------------

    struct FAuditRecord
    {
        AActor* Actor = nullptr;
        FString Label;
        // GetName(). Label is what the outliner shows and is NOT unique, so a finding row
        // identified by label alone cannot be looked up again reliably; ObjectName is unique
        // within the level and is the collision-safe key. Also feeds the ignore-rule match,
        // which already compared against the internal name.
        FString ObjectName;
        FString Path;
        FString ClassName;

        // Checks silenced for this actor by an ignore rule.
        uint32 IgnoredMask = 0;

        FTransform Transform;
        bool bHasRootComponent = false;
        bool bHasPrimitive = false;
        // Bounds are above the degenerate floor, so every bounds-derived check can run.
        bool bBoundsValid = false;
        FVector BoundsOrigin = FVector::ZeroVector;
        FVector BoundsExtent = FVector::ZeroVector;

        int32 MeshComponentCount = 0;
        int32 NullMeshComponentCount = 0;
        TArray<FString> NullMeshComponentNames;

        int32 MaterialSlotCount = 0;
        int32 NullMaterialSlotCount = 0;
        int32 PlaceholderMaterialSlotCount = 0;
        TArray<FString> MissingMaterialSlots;

        bool bGroundRequested = false;
        FGroundMeasure Ground;

        bool bAnyFinding = false;
    };

    // ---- Ground measurement ----------------------------------------------------------------

    // One GroundPlacement::MeasureContact call, reshaped into the terms the audit needs.
    // Nothing here re-traces: CoverDepthCm and the support edge are derived from the very
    // same per-column samples MeasureContact already produced, which is the whole reason the
    // audit shares that solver instead of growing a second notion of "seated".
    void AuditMeasureGround(UWorld* World, FAuditRecord& Record, const FConfig& Config)
    {
        FGroundMeasure& Out = Record.Ground;

        if (!Record.bBoundsValid)
        {
            Out.UnrunnableCode = ErrorCodes::ERR_ACTOR_HAS_NO_BOUNDS;
            Out.UnrunnableReason = TEXT("The actor has no measurable bounds, so there is no "
                                        "footprint to probe.");
            return;
        }

        GroundPlacement::FContactThresholds Thresholds;
        Thresholds.ContactToleranceCm = Config.Thresholds.ContactToleranceCm;
        Thresholds.MinCoverage = Config.Thresholds.MinCoverage;
        Thresholds.MinContactPoints = 0;
        // The audit asks its own gap questions per check; letting MeasureContact's absolute
        // bounds decide would collapse "floating", "buried" and "balanced" into one verdict.
        Thresholds.bEnforceGapBounds = false;

        TArray<GroundPlacement::FGroundColumn> Columns;
        const GroundPlacement::FGroundContactReport Report = GroundPlacement::MeasureContact(
            World, Record.Actor, Config.Surface, Config.GridSize, Config.FootprintInset,
            Config.UndersideModel, Thresholds, &Columns);

        Out.bBoundsPlaneFallback = Report.bUsedBoundsPlaneFallback;
        Out.RejectedSurfaceActorCount = Report.RejectedSurfaceActorCount;
        Out.RejectedSurfaceActors = Report.RejectedSurfaceActors;
        Out.bOverLandscape = Report.bOverLandscape;

        if (!Report.bMeasured)
        {
            Out.UnrunnableCode = ErrorCodes::ERR_GROUND_NOT_MEASURED;
            Out.UnrunnableReason = TEXT("No ground measurement could be taken for this actor.");
            return;
        }

        Out.bMeasured = true;
        Out.ActorColumns = Report.ActorColumns;
        Out.SupportedColumns = Report.SupportedColumns;
        Out.ContactPoints = Report.ContactPoints;
        Out.Coverage = Report.Coverage;
        Out.MinGapCm = Report.MinGapCm;
        // MaxColumnClearanceCm, not MaxGapCm: FGroundContactReport::MaxGapCm now carries the
        // seating-scoped float term (the actor's lowest point vs the lowest ground under it),
        // while this field's contract - and the two findings that echo it - is the silhouette
        // max, "how far the worst column hangs". The audit gates on MinGapCm either way and sets
        // bEnforceGapBounds=false, so nothing here changes verdict; this keeps the reported
        // number the same one it has always been.
        Out.MaxGapCm = Report.MaxColumnClearanceCm;
        Out.PenetrationCm = Report.PenetrationCm;
        Out.BoundsHeightCm = 2.0 * Record.BoundsExtent.Z;
        Out.FootprintMaxCm = 2.0 * FMath::Max(Record.BoundsExtent.X, Record.BoundsExtent.Y);
        Out.EmbedFraction = (Out.BoundsHeightCm > UE_KINDA_SMALL_NUMBER)
            ? (Out.PenetrationCm / Out.BoundsHeightCm) : 0.0;

        if (Report.SupportedColumns == 0)
        {
            // Nothing accepted under any column. Whether that is a defect or a failed
            // measurement depends entirely on WHY, and the two must not be conflated: a
            // filter that refused every hit measured nothing, while an empty column really
            // is an actor with nothing beneath it.
            if (Report.RejectedSurfaceActorCount > 0)
            {
                Out.UnrunnableCode = ErrorCodes::ERR_GROUND_HITS_ALL_REJECTED;
                Out.UnrunnableReason = FString::Printf(
                    TEXT("Every hit under this actor was refused by the surface filter (%d actor(s), "
                         "e.g. '%s'), so nothing beneath it could be measured."),
                    Report.RejectedSurfaceActorCount,
                    Report.RejectedSurfaceActors.Num() > 0 ? *Report.RejectedSurfaceActors[0]
                                                           : TEXT("?"));
            }
            return;
        }

        // ---- The terms only the per-column samples can give ----
        const double TopZ = Record.BoundsOrigin.Z + Record.BoundsExtent.Z;
        double MinGroundZ = 0.0;
        double MaxGroundZ = 0.0;
        bool bFirst = true;

        // Support is attributed from CONTACT columns only. An actor floating 3 m over a rock
        // is not supported by that rock, and counting it would fabricate a structure.
        TMap<AActor*, int32> ContactSupportVotes;

        for (const GroundPlacement::FGroundColumn& Column : Columns)
        {
            if (!Column.IsSupported())
            {
                continue;
            }
            if (bFirst)
            {
                MinGroundZ = Column.GroundZ;
                MaxGroundZ = Column.GroundZ;
                bFirst = false;
            }
            else
            {
                MinGroundZ = FMath::Min(MinGroundZ, Column.GroundZ);
                MaxGroundZ = FMath::Max(MaxGroundZ, Column.GroundZ);
            }
            if (Column.Clearance() <= Config.Thresholds.ContactToleranceCm)
            {
                if (AActor* GroundActor = Column.GroundActor.Get())
                {
                    int32& Votes = ContactSupportVotes.FindOrAdd(GroundActor);
                    ++Votes;
                }
            }
        }

        // Positive = the LOWEST accepted surface under this actor is still above its highest
        // point, i.e. every sampled column has ground over the top of it. That is the
        // difference between an actor bedded into the terrain (some of it is still above the
        // surface, so this is negative) and one that has fallen through the world.
        Out.CoverDepthCm = MinGroundZ - TopZ;
        Out.MaxCoverDepthCm = MaxGroundZ - TopZ;

        AActor* BestSupport = nullptr;
        int32 BestVotes = 0;
        for (const TPair<AActor*, int32>& Vote : ContactSupportVotes)
        {
            if (Vote.Value > BestVotes)
            {
                BestVotes = Vote.Value;
                BestSupport = Vote.Key;
            }
        }
        if (BestSupport)
        {
            Out.SupportActor = BestSupport;
            Out.bSupportIsTerrain = BestSupport->IsA(ALandscapeProxy::StaticClass());
        }
    }

    // ---- Finding construction ---------------------------------------------------------------

    FFinding AuditMakeFinding(const FAuditRecord& Record, ECheck Check, EFindingStatus Status,
                              ESeverity Severity, const FString& Code, const FString& Message)
    {
        FFinding Finding;
        Finding.ActorLabel = Record.Label;
        Finding.ActorObjectName = Record.ObjectName;
        Finding.ActorPath = Record.Path;
        Finding.ActorClass = Record.ClassName;
        Finding.Check = Check;
        Finding.Status = Status;
        Finding.Severity = Severity;
        Finding.Code = Code;
        Finding.Message = Message;
        Finding.Measurements = MakeShared<FJsonObject>();
        return Finding;
    }

    // The one place a finding reaches the report, so every finding is counted exactly once and
    // the tallies cannot drift from the rows.
    struct FAuditSink
    {
        FReport* Report = nullptr;
        FAuditRecord* Record = nullptr;

        void Add(FFinding&& Finding)
        {
            const int32 Index = static_cast<int32>(Finding.Check);
            FCheckTally& Tally = Report->Tallies[Index];
            if (Finding.Status == EFindingStatus::Unrunnable)
            {
                ++Tally.Unrunnable;
                ++Report->UnrunnableCount;
            }
            else
            {
                ++Tally.Flagged;
                if (Finding.Severity == ESeverity::Error) { ++Report->ErrorCount; }
                else                                      { ++Report->WarningCount; }
                Record->bAnyFinding = true;
            }
            Report->Findings.Add(MoveTemp(Finding));
        }
    };

    // Per-check, per-actor gate. Returns false when the check must not produce a row for this
    // actor, having already put the actor in the right bucket. The four buckets are mutually
    // exclusive by construction, which is what makes the tally identities hold.
    bool AuditBeginCheck(FReport& Report, const FAuditRecord& Record, const FConfig& Config,
                         ECheck Check, bool bApplicable)
    {
        FCheckTally& Tally = Report.Tallies[static_cast<int32>(Check)];
        if (!HasCheck(Config.SelectedChecks, Check))
        {
            return false; // not selected: contributes to no tally at all
        }
        if (HasCheck(Record.IgnoredMask, Check))
        {
            ++Tally.Ignored;
            return false;
        }
        if (!bApplicable)
        {
            ++Tally.NotApplicable;
            return false;
        }
        ++Tally.Applicable;
        return true;
    }
} // anonymous namespace inside LevelAudit
} // namespace LevelAudit

namespace LevelAudit
{
    // ---- Check registry -------------------------------------------------------------------

    const TArray<FCheckInfo>& AllChecks()
    {
        // Function-local static so the table is built once and its address is stable. Order
        // is ECheck order; the runtime assert below is what keeps the two from drifting.
        static const TArray<FCheckInfo> Checks = {
            {ECheck::NanTransform, TEXT("nan_transform"), ErrorCodes::ERR_AUDIT_NAN_TRANSFORM,
             ESeverity::Error, true, false, false,
             TEXT("Location, rotation or scale holds a NaN or an infinity, or the rotation is "
                  "not normalized. Nothing downstream of a non-finite transform is meaningful.")},
            {ECheck::ZeroScale, TEXT("zero_scale"), ErrorCodes::ERR_AUDIT_ZERO_SCALE,
             ESeverity::Error, true, false, false,
             TEXT("A scale axis is at or below zeroScaleEpsilon. Reported PER AXIS, unlike the "
                  "engine's own map check, which tests the product of the three and so cannot "
                  "tell a flattened actor from a tiny one.")},
            {ECheck::NegativeScale, TEXT("negative_scale"), ErrorCodes::ERR_AUDIT_NEGATIVE_SCALE,
             ESeverity::Warning, true, false, false,
             TEXT("A scale axis is negative, which mirrors the mesh and inverts its normals and "
                  "triangle winding. Sometimes deliberate, which is why it is a warning.")},
            {ECheck::ExtremeScale, TEXT("extreme_scale"), ErrorCodes::ERR_AUDIT_EXTREME_SCALE,
             ESeverity::Warning, true, false, false,
             TEXT("|scale| exceeds maxScale on some axis, or the actor's world bounds exceed "
                  "maxBoundsCm. Catches an uncapped scale multiplier - the defect that left a "
                  "rim rock wider than the pit floor it bordered.")},
            {ECheck::MissingMesh, TEXT("no_mesh"), ErrorCodes::ERR_AUDIT_MISSING_MESH,
             ESeverity::Error, true, false, false,
             TEXT("A static or skinned mesh component with a null mesh asset. Only actors that "
                  "HAVE such a component are considered: a light or a trigger with no mesh is "
                  "not-applicable, not a defect.")},
            {ECheck::MissingMaterial, TEXT("missing_material"),
             ErrorCodes::ERR_AUDIT_MISSING_MATERIAL, ESeverity::Warning, true, false, false,
             TEXT("A mesh material slot is null, or holds WorldGridMaterial / the engine "
                  "default material, i.e. renders as the grey placeholder grid.")},
            {ECheck::AtWorldOrigin, TEXT("at_world_origin"),
             ErrorCodes::ERR_AUDIT_AT_WORLD_ORIGIN, ESeverity::Warning, true, false, false,
             TEXT("A visible actor sits within originToleranceCm of (0,0,0) - the default "
                  "transform of anything spawned without one. Applies only to actors with real "
                  "bounds, so engine singletons do not fill the report.")},
            {ECheck::BelowKillZ, TEXT("below_kill_z"), ErrorCodes::ERR_AUDIT_BELOW_KILL_Z,
             ESeverity::Error, true, false, false,
             TEXT("The actor's bounds TOP is below AWorldSettings::KillZ. The engine's own "
                  "equivalent runs only at runtime, tests the pivot, and destroys the actor; "
                  "this is the read-only editor-time form.")},
            {ECheck::OutsideWorldBounds, TEXT("outside_world_bounds"),
             ErrorCodes::ERR_AUDIT_OUTSIDE_WORLD_BOUNDS, ESeverity::Error, true, false, false,
             TEXT("The actor's bounds leave the practical world box (worldBoundsCm, default "
                  "1,048,576 cm). NOT the engine's HALF_WORLD_MAX, which under Large World "
                  "Coordinates is ~44 million km and so passes everything.")},
            {ECheck::DuplicateTransform, TEXT("duplicate_transform"),
             ErrorCodes::ERR_AUDIT_DUPLICATE_TRANSFORM, ESeverity::Warning, true, false, false,
             TEXT("Two or more actors of the same class share a location and rotation within "
                  "tolerance - a paste that landed twice. The engine's own SameLocation check "
                  "covers AStaticMeshActor only and needs a working collision query.")},
            {ECheck::OutsidePlayArea, TEXT("outside_play_area"),
             ErrorCodes::ERR_AUDIT_OUTSIDE_PLAY_AREA, ESeverity::Warning, false, false, true,
             TEXT("The actor's bounds centre is outside the stated play area. Requires "
                  "`playArea`; the audit will not guess where the map is.")},
            {ECheck::BelowSurface, TEXT("below_surface"), ErrorCodes::ERR_AUDIT_BELOW_SURFACE,
             ESeverity::Error, false, true, false,
             TEXT("Every sampled column has an accepted surface above the actor's HIGHEST "
                  "point: the actor is entirely underground. Deliberately not 'the underside is "
                  "below the surface', which is also true of every correctly bedded rock.")},
            {ECheck::DeeplyEmbedded, TEXT("deeply_embedded"),
             ErrorCodes::ERR_AUDIT_DEEPLY_EMBEDDED, ESeverity::Warning, false, true, false,
             TEXT("More than maxEmbedFraction of the actor's height is under the surface while "
                  "part of it is still above. Off by default: deliberate bedding and buried "
                  "foundations both live here, so this is a question you ask on purpose.")},
            {ECheck::Airborne, TEXT("airborne"), ErrorCodes::ERR_AUDIT_AIRBORNE,
             ESeverity::Warning, false, true, false,
             TEXT("The CLOSEST part of the actor is more than maxGapCm above the surface, or "
                  "nothing was found beneath it at all. Uses the minimum column gap, so an "
                  "actor resting on a slope is not called floating.")},
            {ECheck::BalancedOnPoint, TEXT("balanced"),
             ErrorCodes::ERR_AUDIT_BALANCED_ON_POINT, ESeverity::Warning, false, true, false,
             TEXT("A large actor touches the surface in fewer than balancedMinContactPoints "
                  "footprint columns - physically a valid rest, visually a physics glitch. Only "
                  "actors at least balancedMinFootprintCm across are considered.")},
            {ECheck::UnsupportedAssembly, TEXT("unsupported_assembly"),
             ErrorCodes::ERR_AUDIT_UNSUPPORTED_ASSEMBLY, ESeverity::Warning, false, true, false,
             TEXT("The actor rests on another actor whose own support chain ends in something "
                  "airborne - the stacked-pieces case no per-actor check can see. Needs a "
                  "surface that accepts non-terrain hits, or there are no edges to follow.")},
            {ECheck::DataValidation, TEXT("data_validation"),
             ErrorCodes::ERR_AUDIT_DATA_VALIDATION, ESeverity::Warning, false, false, false,
             TEXT("Republishes the engine's own AActor::IsDataValid result for the actor, "
                  "including anything a project or plugin validator says. Off by default "
                  "because a third-party override can be arbitrarily expensive.")},
        };
        checkf(Checks.Num() == CheckCount, TEXT("LevelAudit check table is out of sync with ECheck."));
        return Checks;
    }

    // CheckInfo / ParseCheckId / CheckBit / HasCheck and the three masks are the shared
    // contract's, inlined in the header over this table (Audit/AuditFramework.h). The masks
    // are MaskWhere over bNeedsSurface / bNeedsPlayArea / bDefaultOn, so a check added to the
    // table above joins them without another loop being written here.

    // ---- Default ignore rules --------------------------------------------------------------

    TArray<FIgnoreRule> DefaultIgnoreRules()
    {
        auto MakeRule = [](std::initializer_list<const TCHAR*> Classes,
                           std::initializer_list<ECheck> Checks, const TCHAR* Reason)
        {
            FIgnoreRule Rule;
            for (const TCHAR* Cls : Classes) { Rule.Classes.Add(FString(Cls)); }
            for (const ECheck Check : Checks) { Rule.CheckMask |= CheckBit(Check); }
            Rule.Reason = Reason;
            Rule.bBuiltIn = true;
            return Rule;
        };

        TArray<FIgnoreRule> Rules;

        Rules.Add(MakeRule(
            {TEXT("WorldSettings"), TEXT("LevelScriptActor"), TEXT("LevelBounds"),
             TEXT("DefaultPhysicsVolume"), TEXT("AbstractNavData"), TEXT("WorldDataLayers"),
             TEXT("WorldPartitionMiniMap"), TEXT("Brush")},
            {ECheck::AtWorldOrigin, ECheck::ZeroScale, ECheck::MissingMesh,
             ECheck::MissingMaterial, ECheck::ExtremeScale, ECheck::DuplicateTransform,
             ECheck::OutsidePlayArea},
            TEXT("Engine bookkeeping actors: they legitimately sit at the origin, carry no "
                 "mesh, and have no place in the play area.")));

        Rules.Add(MakeRule(
            {TEXT("LandscapeProxy")},
            {ECheck::ExtremeScale, ECheck::AtWorldOrigin, ECheck::DuplicateTransform,
             ECheck::OutsidePlayArea, ECheck::BelowSurface, ECheck::Airborne,
             ECheck::BalancedOnPoint, ECheck::DeeplyEmbedded},
            TEXT("Terrain is legitimately kilometre-scale, is commonly anchored at the origin, "
                 "and is the surface the ground checks measure against rather than a thing "
                 "measured by them.")));

        Rules.Add(MakeRule(
            {TEXT("SkyAtmosphere"), TEXT("SkyLight"), TEXT("ExponentialHeightFog"),
             TEXT("VolumetricCloud"), TEXT("DirectionalLight"), TEXT("PostProcessVolume"),
             TEXT("ReflectionCapture"), TEXT("LightmassImportanceVolume")},
            {ECheck::AtWorldOrigin, ECheck::BelowSurface, ECheck::Airborne,
             ECheck::BalancedOnPoint, ECheck::DeeplyEmbedded, ECheck::OutsidePlayArea,
             ECheck::MissingMesh, ECheck::MissingMaterial},
            TEXT("Environment and lighting actors have no meaningful ground relationship and "
                 "conventionally sit at the origin.")));

        Rules.Add(MakeRule(
            {TEXT("InstancedFoliageActor")},
            {ECheck::AtWorldOrigin, ECheck::BelowSurface, ECheck::Airborne,
             ECheck::BalancedOnPoint, ECheck::DeeplyEmbedded, ECheck::DuplicateTransform,
             ECheck::ExtremeScale, ECheck::OutsidePlayArea},
            TEXT("A foliage actor is a container; its OWN transform says nothing about where "
                 "its instances are. Per-instance placement is invisible to an actor-level "
                 "audit and is reported as a coverage caveat instead of silently passing.")));

        return Rules;
    }

    // ---- Play area ---------------------------------------------------------------------------

    const TCHAR* PlayAreaSourceToString(EPlayAreaSource Source)
    {
        switch (Source)
        {
            case EPlayAreaSource::Landscape:   return TEXT("landscape");
            case EPlayAreaSource::LevelBounds: return TEXT("level_bounds");
            case EPlayAreaSource::Actor:       return TEXT("actor");
            default:                           return TEXT("box");
        }
    }

    bool ParsePlayAreaSource(const FString& Name, EPlayAreaSource& OutSource)
    {
        const FString Lower = Name.ToLower();
        if (Lower == TEXT("landscape"))    { OutSource = EPlayAreaSource::Landscape;   return true; }
        if (Lower == TEXT("level_bounds") || Lower == TEXT("levelbounds"))
                                           { OutSource = EPlayAreaSource::LevelBounds; return true; }
        if (Lower == TEXT("actor"))        { OutSource = EPlayAreaSource::Actor;       return true; }
        if (Lower == TEXT("box"))          { OutSource = EPlayAreaSource::Box;         return true; }
        return false;
    }

    bool ResolvePlayArea(UWorld* World, FPlayArea& InOutArea, const FString& ActorName,
                         FString& OutError)
    {
        InOutArea.bValid = false;
        if (!World)
        {
            OutError = TEXT("No world to resolve a play area against.");
            return false;
        }

        switch (InOutArea.Source)
        {
            case EPlayAreaSource::Landscape:
            {
                FBox Box(ForceInit);
                int32 Proxies = 0;
                for (TActorIterator<ALandscapeProxy> It(World); It; ++It)
                {
                    if (ALandscapeProxy* Proxy = *It)
                    {
                        const FBox ProxyBox = Proxy->GetComponentsBoundingBox(true);
                        if (ProxyBox.IsValid)
                        {
                            Box += ProxyBox;
                            ++Proxies;
                        }
                    }
                }
                if (Proxies == 0 || !Box.IsValid)
                {
                    OutError = TEXT("playArea preset 'landscape' found no landscape in this world. "
                                    "Name a bounding actor with {\"source\":\"actor\",\"actor\":...} "
                                    "or state the box directly; the audit will not silently "
                                    "substitute a different play area.");
                    return false;
                }
                InOutArea.Box = Box;
                InOutArea.Description = FString::Printf(
                    TEXT("union of %d landscape proxy bounds"), Proxies);
                break;
            }
            case EPlayAreaSource::LevelBounds:
            {
                const FBox Box = McpActorUtils::SumActorBounds(World);
                if (!Box.IsValid)
                {
                    OutError = TEXT("playArea preset 'level_bounds' produced no valid box: no actor "
                                    "in this world reported finite renderable bounds.");
                    return false;
                }
                InOutArea.Box = Box;
                InOutArea.Description = TEXT("summed bounds of every actor in the world - note "
                                             "that a stray actor INFLATES this box and is then "
                                             "inside it, so this source under-reports strays");
                break;
            }
            case EPlayAreaSource::Actor:
            {
                if (ActorName.IsEmpty())
                {
                    OutError = TEXT("playArea source 'actor' needs an 'actor' name.");
                    return false;
                }
                AActor* Found = McpActorUtils::FindActorByName(World, ActorName);
                if (!Found)
                {
                    OutError = FString::Printf(
                        TEXT("playArea actor '%s' did not resolve in this world."), *ActorName);
                    return false;
                }
                const FBox Box = Found->GetComponentsBoundingBox(true);
                if (!Box.IsValid)
                {
                    OutError = FString::Printf(
                        TEXT("playArea actor '%s' has no valid bounds to use as a box."), *ActorName);
                    return false;
                }
                InOutArea.Box = Box;
                InOutArea.Description = FString::Printf(TEXT("bounds of actor '%s'"), *ActorName);
                break;
            }
            case EPlayAreaSource::Box:
            default:
            {
                if (!InOutArea.Box.IsValid)
                {
                    OutError = TEXT("playArea source 'box' needs a valid min/max (or centre/extent).");
                    return false;
                }
                InOutArea.Description = TEXT("caller-supplied box");
                break;
            }
        }

        InOutArea.bValid = true;
        return true;
    }

    // ---- The sweep ---------------------------------------------------------------------------

    void Run(UWorld* World, const FConfig& Config, FReport& OutReport)
    {
        if (!World)
        {
            return;
        }

        // The report owns the rules from here on, because the sweep writes each rule's
        // MatchedActors back into it. A rule that turns out to have silenced half the level is
        // then visible in the response instead of only in its effect.
        OutReport.IgnoreRules = Config.IgnoreRules;

        // ---- World facts ----
        if (AWorldSettings* Settings = World->GetWorldSettings())
        {
            OutReport.KillZ = static_cast<double>(Settings->KillZ);
            OutReport.bWorldBoundsChecksEnabled = Settings->AreWorldBoundsChecksEnabled();
        }
        OutReport.EngineHalfWorldMaxCm = static_cast<double>(HALF_WORLD_MAX);

        // ---- Collect ----
        // EActorIteratorFlags::SkipPendingKill ONLY. The iterator's default adds
        // OnlyActiveLevels (EngineUtils.h:580), which would skip every actor in a hidden or
        // inactive sublevel and report the level clean without ever having looked at them -
        // exactly the silence this audit exists to remove. UEditorActorSubsystem::
        // GetAllLevelActors drops the same flag for the same reason
        // (EditorActorSubsystem.cpp:378).
        TArray<AActor*> Matched;
        TSet<const AActor*> ScopeSet;
        for (const TWeakObjectPtr<AActor>& Weak : Config.ScopeActors)
        {
            if (const AActor* Scoped = Weak.Get())
            {
                ScopeSet.Add(Scoped);
            }
        }

        for (TActorIterator<AActor> It(World, AActor::StaticClass(),
                                       EActorIteratorFlags::SkipPendingKill); It; ++It)
        {
            AActor* Actor = *It;
            if (!IsValid(Actor) || Actor->IsTemplate())
            {
                continue;
            }
            ++OutReport.ActorsInWorld;

            if (Config.bScopeActorsProvided)
            {
                if (!ScopeSet.Contains(Actor))
                {
                    continue;
                }
            }
            else if (!Config.Prefix.IsEmpty())
            {
                if (!Actor->GetActorLabel().StartsWith(Config.Prefix, ESearchCase::IgnoreCase)
                    && !Actor->GetName().StartsWith(Config.Prefix, ESearchCase::IgnoreCase))
                {
                    continue;
                }
            }
            else if (Config.Filter.IsActive())
            {
                if (!Config.Filter.MatchesEither(Actor->GetActorLabel(), Actor->GetName()))
                {
                    continue;
                }
            }
            Matched.Add(Actor);
        }

        OutReport.ActorsMatched = Matched.Num();

        const int32 Offset = FMath::Max(Config.Offset, 0);
        const int32 Limit = FMath::Max(Config.Limit, 1);
        TArray<FAuditRecord> Records;
        Records.Reserve(FMath::Min(Limit, Matched.Num()));

        // ---- Pass A: per-actor facts ----
        int32 GroundBudget = FMath::Max(Config.MaxGroundActors, 0);
        const uint32 SurfaceMask = SurfaceCheckMask();
        const bool bAnySurfaceCheck = Config.bHasSurface && (Config.SelectedChecks & SurfaceMask) != 0;

        for (int32 Index = Offset; Index < Matched.Num() && Records.Num() < Limit; ++Index)
        {
            AActor* Actor = Matched[Index];
            FAuditRecord Record;
            Record.Actor = Actor;
            Record.Label = Actor->GetActorLabel();
            Record.ObjectName = Actor->GetName();
            Record.Path = Actor->GetPathName();
            Record.ClassName = Actor->GetClass() ? Actor->GetClass()->GetName() : FString();
            Record.Transform = Actor->GetActorTransform();
            Record.bHasRootComponent = (Actor->GetRootComponent() != nullptr);

            // ---- Ignore rules ----
            for (FIgnoreRule& Rule : OutReport.IgnoreRules)
            {
                if (AuditRuleMatches(Rule, Actor, Record.Label, Record.ObjectName, Record.Path))
                {
                    ++Rule.MatchedActors;
                    Record.IgnoredMask |= (Rule.CheckMask == 0) ? ~0u : Rule.CheckMask;
                }
            }
            if (!Config.IgnoreTag.IsNone() && Actor->ActorHasTag(Config.IgnoreTag))
            {
                Record.IgnoredMask = ~0u;
            }

            // ---- Components ----
            TArray<UPrimitiveComponent*> Primitives;
            Actor->GetComponents<UPrimitiveComponent>(Primitives);
            Record.bHasPrimitive = Primitives.Num() > 0;

            // The engine's stand-in for an unassigned slot. Hoisted out of the slot loop
            // because it is a lookup, not a field read (Materials/Material.h:1455).
            UMaterial* DefaultMaterial = UMaterial::GetDefaultMaterial(MD_Surface);

            for (UPrimitiveComponent* Primitive : Primitives)
            {
                UMeshComponent* Mesh = Cast<UMeshComponent>(Primitive);
                if (!Mesh)
                {
                    continue;
                }

                bool bIsMeshBearing = false;
                bool bAssetPresent = false;
                if (UStaticMeshComponent* StaticMesh = Cast<UStaticMeshComponent>(Mesh))
                {
                    bIsMeshBearing = true;
                    bAssetPresent = (StaticMesh->GetStaticMesh() != nullptr);
                }
                else if (USkinnedMeshComponent* Skinned = Cast<USkinnedMeshComponent>(Mesh))
                {
                    bIsMeshBearing = true;
                    bAssetPresent = (Skinned->GetSkinnedAsset() != nullptr);
                }

                if (bIsMeshBearing)
                {
                    ++Record.MeshComponentCount;
                    if (!bAssetPresent)
                    {
                        ++Record.NullMeshComponentCount;
                        if (Record.NullMeshComponentNames.Num() < AuditMaxEchoedNames)
                        {
                            Record.NullMeshComponentNames.Add(Mesh->GetName());
                        }
                        // No mesh means no material slots worth reporting; the null-mesh
                        // finding is the whole story for this component.
                        continue;
                    }
                }

                const int32 SlotCount = Mesh->GetNumMaterials();
                for (int32 Slot = 0; Slot < SlotCount; ++Slot)
                {
                    ++Record.MaterialSlotCount;
                    UMaterialInterface* Material = Mesh->GetMaterial(Slot);
                    const bool bNull = (Material == nullptr);
                    bool bPlaceholder = false;
                    if (!bNull)
                    {
                        bPlaceholder = (Material == DefaultMaterial)
                            || Material->GetPathName().Equals(AuditWorldGridMaterialPath,
                                                              ESearchCase::IgnoreCase);
                    }
                    if (bNull || bPlaceholder)
                    {
                        if (bNull) { ++Record.NullMaterialSlotCount; }
                        else       { ++Record.PlaceholderMaterialSlotCount; }
                        if (Record.MissingMaterialSlots.Num() < AuditMaxEchoedNames)
                        {
                            Record.MissingMaterialSlots.Add(FString::Printf(TEXT("%s[%d]%s"),
                                *Mesh->GetName(), Slot, bNull ? TEXT("") : TEXT(" (placeholder)")));
                        }
                    }
                }
            }

            // ---- Bounds ----
            // bOnlyCollidingComponents=false: the footprint that must not float is the one you
            // can SEE, not the one the physics scene knows about. Same call and argument as
            // actor.get_bounding_box, spatial.measure_* and GroundPlacementUtils.cpp:374, so
            // every spatial number in this plugin agrees.
            Actor->GetActorBounds(false, Record.BoundsOrigin, Record.BoundsExtent);
            Record.bBoundsValid = Record.bHasPrimitive
                && (Record.BoundsExtent.X >= AuditMinExtentCm
                    || Record.BoundsExtent.Y >= AuditMinExtentCm
                    || Record.BoundsExtent.Z >= AuditMinExtentCm)
                && !Record.BoundsOrigin.ContainsNaN() && !Record.BoundsExtent.ContainsNaN();

            // ---- Ground ----
            if (bAnySurfaceCheck && (Record.IgnoredMask & SurfaceMask) != SurfaceMask)
            {
                Record.bGroundRequested = true;
                if (GroundBudget > 0)
                {
                    --GroundBudget;
                    AuditMeasureGround(World, Record, Config);
                    if (Record.Ground.bMeasured)
                    {
                        ++OutReport.GroundMeasured;
                    }
                }
                else
                {
                    ++OutReport.GroundBudgetExhausted;
                    Record.Ground.UnrunnableCode = ErrorCodes::ERR_AUDIT_TRACE_BUDGET_EXHAUSTED;
                    Record.Ground.UnrunnableReason = FString::Printf(
                        TEXT("The per-call ground-measurement budget of %d actor(s) was spent "
                             "before this actor was reached. Raise maxGroundActors or page with "
                             "offset; this actor was NOT measured."),
                        FMath::Max(Config.MaxGroundActors, 0));
                }
            }

            Records.Add(MoveTemp(Record));
        }

        OutReport.ActorsExamined = Records.Num();
        OutReport.bTruncated = (Offset + Records.Num()) < Matched.Num();

        // ---- Pass B: per-actor checks ----
        for (FAuditRecord& Record : Records)
        {
            if (Record.IgnoredMask == ~0u)
            {
                ++OutReport.ActorsIgnored;
            }

            FAuditSink Sink;
            Sink.Report = &OutReport;
            Sink.Record = &Record;

            const FVector Scale = Record.Transform.GetScale3D();
            const FVector Location = Record.Transform.GetLocation();

            // ---- nan_transform ----
            if (AuditBeginCheck(OutReport, Record, Config, ECheck::NanTransform,
                                Record.bHasRootComponent))
            {
                // FTransform::IsValid (TransformNonVectorized.h:600) is ContainsNaN plus a
                // normalized-rotation test, and ContainsNaN is really !IsFinite, so it covers
                // infinities too (Vector.h:2294-2298).
                if (!Record.Transform.IsValid())
                {
                    FFinding Finding = AuditMakeFinding(Record, ECheck::NanTransform,
                        EFindingStatus::Flagged, ESeverity::Error,
                        ErrorCodes::ERR_AUDIT_NAN_TRANSFORM,
                        TEXT("The actor's transform is not finite or its rotation is not "
                             "normalized. Every measurement taken from it, here and elsewhere, "
                             "is meaningless."));
                    Finding.Measurements->SetBoolField(TEXT("locationNonFinite"),
                        Location.ContainsNaN());
                    Finding.Measurements->SetBoolField(TEXT("scaleNonFinite"), Scale.ContainsNaN());
                    Finding.Measurements->SetBoolField(TEXT("rotationNonFinite"),
                        Record.Transform.GetRotation().ContainsNaN());
                    Finding.Measurements->SetBoolField(TEXT("rotationNormalized"),
                        Record.Transform.IsRotationNormalized());
                    Sink.Add(MoveTemp(Finding));
                }
                else
                {
                    ++OutReport.Tallies[static_cast<int32>(ECheck::NanTransform)].Clean;
                }
            }

            // ---- zero_scale ----
            if (AuditBeginCheck(OutReport, Record, Config, ECheck::ZeroScale,
                                Record.bHasRootComponent && !Scale.ContainsNaN()))
            {
                const double MinAbs = FMath::Min3(FMath::Abs(Scale.X), FMath::Abs(Scale.Y),
                                                  FMath::Abs(Scale.Z));
                if (MinAbs <= Config.Thresholds.ZeroScaleEpsilon)
                {
                    FFinding Finding = AuditMakeFinding(Record, ECheck::ZeroScale,
                        EFindingStatus::Flagged, ESeverity::Error,
                        ErrorCodes::ERR_AUDIT_ZERO_SCALE,
                        FString::Printf(
                            TEXT("Scale (%.6f, %.6f, %.6f) has an axis at or below %g: the actor "
                                 "is collapsed to nothing on that axis."),
                            Scale.X, Scale.Y, Scale.Z, Config.Thresholds.ZeroScaleEpsilon));
                    Finding.Measurements->SetObjectField(TEXT("scale"), AuditVectorObject(Scale));
                    Finding.Measurements->SetNumberField(TEXT("minAbsScale"), MinAbs);
                    Finding.Measurements->SetNumberField(TEXT("epsilon"),
                        Config.Thresholds.ZeroScaleEpsilon);
                    Sink.Add(MoveTemp(Finding));
                }
                else
                {
                    ++OutReport.Tallies[static_cast<int32>(ECheck::ZeroScale)].Clean;
                }
            }

            // ---- negative_scale ----
            if (AuditBeginCheck(OutReport, Record, Config, ECheck::NegativeScale,
                                Record.bHasRootComponent && !Scale.ContainsNaN()))
            {
                if (Scale.X < 0.0 || Scale.Y < 0.0 || Scale.Z < 0.0)
                {
                    FFinding Finding = AuditMakeFinding(Record, ECheck::NegativeScale,
                        EFindingStatus::Flagged, ESeverity::Warning,
                        ErrorCodes::ERR_AUDIT_NEGATIVE_SCALE,
                        FString::Printf(
                            TEXT("Scale (%.4f, %.4f, %.4f) is negative on at least one axis, which "
                                 "mirrors the mesh and inverts its normals and triangle winding. "
                                 "Deliberate mirroring looks identical to a mistake here, so this "
                                 "is a warning."),
                            Scale.X, Scale.Y, Scale.Z));
                    Finding.Measurements->SetObjectField(TEXT("scale"), AuditVectorObject(Scale));
                    Finding.Measurements->SetNumberField(TEXT("scaleDeterminant"),
                        Scale.X * Scale.Y * Scale.Z);
                    Sink.Add(MoveTemp(Finding));
                }
                else
                {
                    ++OutReport.Tallies[static_cast<int32>(ECheck::NegativeScale)].Clean;
                }
            }

            // ---- extreme_scale ----
            if (AuditBeginCheck(OutReport, Record, Config, ECheck::ExtremeScale,
                                Record.bBoundsValid && !Scale.ContainsNaN()))
            {
                const double MaxAbs = FMath::Max3(FMath::Abs(Scale.X), FMath::Abs(Scale.Y),
                                                  FMath::Abs(Scale.Z));
                const double MaxDim = 2.0 * FMath::Max3(Record.BoundsExtent.X,
                                                        Record.BoundsExtent.Y,
                                                        Record.BoundsExtent.Z);
                const bool bScaleOver = MaxAbs > Config.Thresholds.MaxScale;
                const bool bSizeOver = MaxDim > Config.Thresholds.MaxBoundsCm;
                if (bScaleOver || bSizeOver)
                {
                    FFinding Finding = AuditMakeFinding(Record, ECheck::ExtremeScale,
                        EFindingStatus::Flagged, ESeverity::Warning,
                        ErrorCodes::ERR_AUDIT_EXTREME_SCALE,
                        FString::Printf(
                            TEXT("Largest |scale| is %.3f (limit %.3f) and the actor measures "
                                 "%.1f cm across its longest axis (limit %.1f cm)."),
                            MaxAbs, Config.Thresholds.MaxScale, MaxDim,
                            Config.Thresholds.MaxBoundsCm));
                    Finding.Measurements->SetObjectField(TEXT("scale"), AuditVectorObject(Scale));
                    Finding.Measurements->SetNumberField(TEXT("maxAbsScale"), MaxAbs);
                    Finding.Measurements->SetNumberField(TEXT("maxBoundsDimCm"), MaxDim);
                    Finding.Measurements->SetBoolField(TEXT("scaleOverLimit"), bScaleOver);
                    Finding.Measurements->SetBoolField(TEXT("sizeOverLimit"), bSizeOver);
                    Sink.Add(MoveTemp(Finding));
                }
                else
                {
                    ++OutReport.Tallies[static_cast<int32>(ECheck::ExtremeScale)].Clean;
                }
            }

            // ---- no_mesh ----
            if (AuditBeginCheck(OutReport, Record, Config, ECheck::MissingMesh,
                                Record.MeshComponentCount > 0))
            {
                if (Record.NullMeshComponentCount > 0)
                {
                    FFinding Finding = AuditMakeFinding(Record, ECheck::MissingMesh,
                        EFindingStatus::Flagged, ESeverity::Error,
                        ErrorCodes::ERR_AUDIT_MISSING_MESH,
                        FString::Printf(
                            TEXT("%d of %d mesh component(s) on this actor have no mesh asset "
                                 "assigned, so the actor renders nothing."),
                            Record.NullMeshComponentCount, Record.MeshComponentCount));
                    Finding.Measurements->SetNumberField(TEXT("meshComponents"),
                        Record.MeshComponentCount);
                    Finding.Measurements->SetNumberField(TEXT("nullMeshComponents"),
                        Record.NullMeshComponentCount);
                    TArray<TSharedPtr<FJsonValue>> Names;
                    for (const FString& Name : Record.NullMeshComponentNames)
                    {
                        Names.Add(MakeShared<FJsonValueString>(Name));
                    }
                    Finding.Measurements->SetArrayField(TEXT("components"), Names);
                    Sink.Add(MoveTemp(Finding));
                }
                else
                {
                    ++OutReport.Tallies[static_cast<int32>(ECheck::MissingMesh)].Clean;
                }
            }

            // ---- missing_material ----
            if (AuditBeginCheck(OutReport, Record, Config, ECheck::MissingMaterial,
                                Record.MaterialSlotCount > 0))
            {
                const int32 Bad = Record.NullMaterialSlotCount + Record.PlaceholderMaterialSlotCount;
                if (Bad > 0)
                {
                    FFinding Finding = AuditMakeFinding(Record, ECheck::MissingMaterial,
                        EFindingStatus::Flagged, ESeverity::Warning,
                        ErrorCodes::ERR_AUDIT_MISSING_MATERIAL,
                        FString::Printf(
                            TEXT("%d of %d material slot(s) are unset or hold the placeholder grid "
                                 "material (%d null, %d placeholder)."),
                            Bad, Record.MaterialSlotCount, Record.NullMaterialSlotCount,
                            Record.PlaceholderMaterialSlotCount));
                    Finding.Measurements->SetNumberField(TEXT("materialSlots"),
                        Record.MaterialSlotCount);
                    Finding.Measurements->SetNumberField(TEXT("nullSlots"),
                        Record.NullMaterialSlotCount);
                    Finding.Measurements->SetNumberField(TEXT("placeholderSlots"),
                        Record.PlaceholderMaterialSlotCount);
                    TArray<TSharedPtr<FJsonValue>> Slots;
                    for (const FString& Slot : Record.MissingMaterialSlots)
                    {
                        Slots.Add(MakeShared<FJsonValueString>(Slot));
                    }
                    Finding.Measurements->SetArrayField(TEXT("slots"), Slots);
                    Sink.Add(MoveTemp(Finding));
                }
                else
                {
                    ++OutReport.Tallies[static_cast<int32>(ECheck::MissingMaterial)].Clean;
                }
            }

            // ---- at_world_origin ----
            if (AuditBeginCheck(OutReport, Record, Config, ECheck::AtWorldOrigin,
                                Record.bBoundsValid && !Location.ContainsNaN()))
            {
                const double Distance = Location.Size();
                if (Distance <= Config.Thresholds.OriginToleranceCm)
                {
                    FFinding Finding = AuditMakeFinding(Record, ECheck::AtWorldOrigin,
                        EFindingStatus::Flagged, ESeverity::Warning,
                        ErrorCodes::ERR_AUDIT_AT_WORLD_ORIGIN,
                        FString::Printf(
                            TEXT("A visible actor sits %.3f cm from the world origin, which is "
                                 "the default transform of anything spawned without one."),
                            Distance));
                    Finding.Measurements->SetNumberField(TEXT("distanceFromOriginCm"), Distance);
                    Finding.Measurements->SetNumberField(TEXT("toleranceCm"),
                        Config.Thresholds.OriginToleranceCm);
                    Sink.Add(MoveTemp(Finding));
                }
                else
                {
                    ++OutReport.Tallies[static_cast<int32>(ECheck::AtWorldOrigin)].Clean;
                }
            }

            // ---- below_kill_z ----
            if (AuditBeginCheck(OutReport, Record, Config, ECheck::BelowKillZ,
                                Record.bBoundsValid))
            {
                if (!OutReport.KillZ.IsSet())
                {
                    Sink.Add(AuditMakeFinding(Record, ECheck::BelowKillZ,
                        EFindingStatus::Unrunnable, ESeverity::Warning,
                        ErrorCodes::ERR_AUDIT_BELOW_KILL_Z,
                        TEXT("This world has no AWorldSettings, so there is no KillZ to compare "
                             "against. The check did not run - this is not a pass.")));
                }
                else
                {
                    const double TopZ = Record.BoundsOrigin.Z + Record.BoundsExtent.Z;
                    const double Line = OutReport.KillZ.GetValue() - Config.Thresholds.KillZMarginCm;
                    if (TopZ < Line)
                    {
                        FFinding Finding = AuditMakeFinding(Record, ECheck::BelowKillZ,
                            EFindingStatus::Flagged, ESeverity::Error,
                            ErrorCodes::ERR_AUDIT_BELOW_KILL_Z,
                            FString::Printf(
                                TEXT("The actor's highest point is at Z %.1f, entirely below KillZ "
                                     "%.1f. At runtime the engine would delete it."),
                                TopZ, OutReport.KillZ.GetValue()));
                        Finding.Measurements->SetNumberField(TEXT("boundsTopZ"), TopZ);
                        Finding.Measurements->SetNumberField(TEXT("killZ"),
                            OutReport.KillZ.GetValue());
                        Finding.Measurements->SetNumberField(TEXT("belowByCm"), Line - TopZ);
                        // The engine only ENFORCES KillZ when bounds checks are on
                        // (AWorldSettings::AreWorldBoundsChecksEnabled, WorldSettings.h:904).
                        // The actor is under the map either way; whether the engine would act
                        // on it is a separate fact and is reported as one.
                        Finding.Measurements->SetBoolField(TEXT("worldBoundsChecksEnabled"),
                            OutReport.bWorldBoundsChecksEnabled);
                        Sink.Add(MoveTemp(Finding));
                    }
                    else
                    {
                        ++OutReport.Tallies[static_cast<int32>(ECheck::BelowKillZ)].Clean;
                    }
                }
            }

            // ---- outside_world_bounds ----
            if (AuditBeginCheck(OutReport, Record, Config, ECheck::OutsideWorldBounds,
                                Record.bBoundsValid))
            {
                const double Bound = Config.Thresholds.WorldBoundsCm;
                const FVector Min = Record.BoundsOrigin - Record.BoundsExtent;
                const FVector Max = Record.BoundsOrigin + Record.BoundsExtent;
                const double Worst = FMath::Max3(
                    FMath::Max(FMath::Abs(Min.X), FMath::Abs(Max.X)),
                    FMath::Max(FMath::Abs(Min.Y), FMath::Abs(Max.Y)),
                    FMath::Max(FMath::Abs(Min.Z), FMath::Abs(Max.Z)));
                if (Worst > Bound)
                {
                    FFinding Finding = AuditMakeFinding(Record, ECheck::OutsideWorldBounds,
                        EFindingStatus::Flagged, ESeverity::Error,
                        ErrorCodes::ERR_AUDIT_OUTSIDE_WORLD_BOUNDS,
                        FString::Printf(
                            TEXT("The actor reaches %.0f cm from the origin, past the practical "
                                 "world bound of %.0f cm. Beyond this a float no longer holds "
                                 "position to 1/16 cm."),
                            Worst, Bound));
                    Finding.Measurements->SetNumberField(TEXT("worstCoordinateCm"), Worst);
                    Finding.Measurements->SetNumberField(TEXT("worldBoundsCm"), Bound);
                    Finding.Measurements->SetNumberField(TEXT("engineHalfWorldMaxCm"),
                        OutReport.EngineHalfWorldMaxCm);
                    Sink.Add(MoveTemp(Finding));
                }
                else
                {
                    ++OutReport.Tallies[static_cast<int32>(ECheck::OutsideWorldBounds)].Clean;
                }
            }

            // ---- outside_play_area ----
            if (AuditBeginCheck(OutReport, Record, Config, ECheck::OutsidePlayArea,
                                Record.bBoundsValid && Config.PlayArea.bValid))
            {
                const FBox Area = Config.PlayArea.Box.ExpandBy(Config.PlayArea.MarginCm);
                const FVector Centre = Record.BoundsOrigin;
                const double DX = FMath::Max(Area.Min.X - Centre.X, Centre.X - Area.Max.X);
                const double DY = FMath::Max(Area.Min.Y - Centre.Y, Centre.Y - Area.Max.Y);
                const double DZ = Config.PlayArea.bIgnoreZ
                    ? AuditAxisIgnored
                    : FMath::Max(Area.Min.Z - Centre.Z, Centre.Z - Area.Max.Z);
                const double Outside = FMath::Max3(DX, DY, DZ);
                if (Outside > 0.0)
                {
                    FFinding Finding = AuditMakeFinding(Record, ECheck::OutsidePlayArea,
                        EFindingStatus::Flagged, ESeverity::Warning,
                        ErrorCodes::ERR_AUDIT_OUTSIDE_PLAY_AREA,
                        FString::Printf(
                            TEXT("The actor's centre is %.0f cm outside the play area (%s)."),
                            Outside, *Config.PlayArea.Description));
                    Finding.Measurements->SetNumberField(TEXT("outsideByCm"), Outside);
                    Finding.Measurements->SetObjectField(TEXT("location"),
                        AuditVectorObject(Centre));
                    Finding.Measurements->SetBoolField(TEXT("ignoreZ"), Config.PlayArea.bIgnoreZ);
                    Sink.Add(MoveTemp(Finding));
                }
                else
                {
                    ++OutReport.Tallies[static_cast<int32>(ECheck::OutsidePlayArea)].Clean;
                }
            }

            // ---- The ground family --------------------------------------------------------
            // All four share one measurement. When the measurement failed, each SELECTED
            // ground check emits its own Unrunnable row: an actor that could not be probed is
            // not clean for any of them, and folding them into one row would hide which
            // questions went unanswered.
            const FGroundMeasure& Ground = Record.Ground;
            const bool bGroundUsable = Ground.bMeasured && Ground.UnrunnableCode.IsEmpty();
            const bool bCoverageOk = bGroundUsable
                && (Ground.Coverage + UE_KINDA_SMALL_NUMBER >= Config.Thresholds.MinCoverage);
            const bool bHasSupportSamples = bGroundUsable && Ground.SupportedColumns > 0;
            const bool bFloating = bHasSupportSamples
                && (Ground.MinGapCm > Config.Thresholds.MaxGapCm);

            auto GroundUnrunnable = [&](ECheck Check, const TCHAR* Extra)
            {
                const FString Code = Record.Ground.UnrunnableCode.IsEmpty()
                    ? FString(ErrorCodes::ERR_GROUND_NOT_MEASURED)
                    : Record.Ground.UnrunnableCode;
                FString Reason = Record.Ground.UnrunnableReason.IsEmpty()
                    ? FString(TEXT("The ground measurement for this actor did not produce a "
                                   "usable result."))
                    : Record.Ground.UnrunnableReason;
                if (Extra)
                {
                    Reason += TEXT(" ");
                    Reason += Extra;
                }
                Sink.Add(AuditMakeFinding(Record, Check, EFindingStatus::Unrunnable,
                    ESeverity::Warning, Code, Reason));
            };

            // ---- below_surface ----
            if (AuditBeginCheck(OutReport, Record, Config, ECheck::BelowSurface,
                                Record.bGroundRequested && Record.bBoundsValid))
            {
                if (!bHasSupportSamples)
                {
                    GroundUnrunnable(ECheck::BelowSurface,
                        TEXT("Nothing above or below this actor was accepted as a surface, so "
                             "whether it is underground is unknown - not 'no'."));
                }
                else if (Ground.CoverDepthCm > Config.Thresholds.MinCoverDepthCm)
                {
                    const bool bDeep = Ground.CoverDepthCm > Config.Thresholds.DeepCoverDepthCm;
                    FFinding Finding = AuditMakeFinding(Record, ECheck::BelowSurface,
                        EFindingStatus::Flagged, ESeverity::Error,
                        ErrorCodes::ERR_AUDIT_BELOW_SURFACE,
                        FString::Printf(
                            TEXT("The surface is above this actor's HIGHEST point in every "
                                 "sampled column, by %.1f cm at the shallowest. None of it is "
                                 "visible above ground."),
                            Ground.CoverDepthCm));
                    Finding.Measurements->SetNumberField(TEXT("coverDepthCm"), Ground.CoverDepthCm);
                    Finding.Measurements->SetNumberField(TEXT("maxCoverDepthCm"),
                        Ground.MaxCoverDepthCm);
                    Finding.Measurements->SetNumberField(TEXT("boundsHeightCm"),
                        Ground.BoundsHeightCm);
                    Finding.Measurements->SetNumberField(TEXT("penetrationCm"),
                        Ground.PenetrationCm);
                    Finding.Measurements->SetNumberField(TEXT("supportedColumns"),
                        Ground.SupportedColumns);
                    // The triage handle: 620 buried actors in this project, 170 of them by more
                    // than 1000 cm. `deep` is what separates those two populations.
                    Finding.Measurements->SetBoolField(TEXT("deep"), bDeep);
                    Sink.Add(MoveTemp(Finding));
                }
                else
                {
                    ++OutReport.Tallies[static_cast<int32>(ECheck::BelowSurface)].Clean;
                }
            }

            // ---- deeply_embedded ----
            if (AuditBeginCheck(OutReport, Record, Config, ECheck::DeeplyEmbedded,
                                Record.bGroundRequested && Record.bBoundsValid))
            {
                if (!bHasSupportSamples)
                {
                    GroundUnrunnable(ECheck::DeeplyEmbedded, nullptr);
                }
                else if (Ground.CoverDepthCm > Config.Thresholds.MinCoverDepthCm)
                {
                    // Entirely underground is the stronger statement and below_surface makes
                    // it; reporting the embed fraction as well would double-count one defect.
                    ++OutReport.Tallies[static_cast<int32>(ECheck::DeeplyEmbedded)].Clean;
                }
                else if (Ground.EmbedFraction > Config.Thresholds.MaxEmbedFraction)
                {
                    FFinding Finding = AuditMakeFinding(Record, ECheck::DeeplyEmbedded,
                        EFindingStatus::Flagged, ESeverity::Warning,
                        ErrorCodes::ERR_AUDIT_DEEPLY_EMBEDDED,
                        FString::Printf(
                            TEXT("%.0f%% of this actor's height is below the surface (%.1f cm of "
                                 "%.1f cm), above the %.0f%% you allowed. Deliberate bedding and "
                                 "a foundation both look like this."),
                            Ground.EmbedFraction * 100.0, Ground.PenetrationCm,
                            Ground.BoundsHeightCm, Config.Thresholds.MaxEmbedFraction * 100.0));
                    Finding.Measurements->SetNumberField(TEXT("embedFraction"),
                        Ground.EmbedFraction);
                    Finding.Measurements->SetNumberField(TEXT("penetrationCm"),
                        Ground.PenetrationCm);
                    Finding.Measurements->SetNumberField(TEXT("boundsHeightCm"),
                        Ground.BoundsHeightCm);
                    Sink.Add(MoveTemp(Finding));
                }
                else
                {
                    ++OutReport.Tallies[static_cast<int32>(ECheck::DeeplyEmbedded)].Clean;
                }
            }

            // ---- airborne ----
            if (AuditBeginCheck(OutReport, Record, Config, ECheck::Airborne,
                                Record.bGroundRequested && Record.bBoundsValid))
            {
                if (!bGroundUsable)
                {
                    GroundUnrunnable(ECheck::Airborne, nullptr);
                }
                else if (Ground.SupportedColumns == 0)
                {
                    // Measured cleanly and found NOTHING under any column within the surface
                    // spec's reach. That is a positive finding of floating, not a failure to
                    // measure - the two are separated above by RejectedSurfaceActorCount.
                    FFinding Finding = AuditMakeFinding(Record, ECheck::Airborne,
                        EFindingStatus::Flagged, ESeverity::Warning,
                        ErrorCodes::ERR_AUDIT_AIRBORNE,
                        TEXT("No accepted surface was found beneath any sampled column of this "
                             "actor's footprint, within the surface spec's drop distance."));
                    Finding.Measurements->SetBoolField(TEXT("groundFound"), false);
                    Finding.Measurements->SetNumberField(TEXT("actorColumns"), Ground.ActorColumns);
                    if (Ground.bOverLandscape.IsSet())
                    {
                        // The structural difference between "something is in the way" and
                        // "this actor is not over the terrain at all".
                        Finding.Measurements->SetBoolField(TEXT("overLandscape"),
                            Ground.bOverLandscape.GetValue());
                    }
                    Sink.Add(MoveTemp(Finding));
                }
                else if (!bCoverageOk)
                {
                    Sink.Add(AuditMakeFinding(Record, ECheck::Airborne,
                        EFindingStatus::Unrunnable, ESeverity::Warning,
                        ErrorCodes::ERR_PARTIAL_GROUND_COVERAGE,
                        FString::Printf(
                            TEXT("Only %.0f%% of this actor's footprint had ground under it "
                                 "(%.0f%% required), so its gap numbers describe a fragment of "
                                 "the actor and cannot decide whether it floats."),
                            Ground.Coverage * 100.0, Config.Thresholds.MinCoverage * 100.0)));
                }
                else if (bFloating)
                {
                    FFinding Finding = AuditMakeFinding(Record, ECheck::Airborne,
                        EFindingStatus::Flagged, ESeverity::Warning,
                        ErrorCodes::ERR_AUDIT_AIRBORNE,
                        FString::Printf(
                            TEXT("The CLOSEST part of this actor is %.1f cm above the surface "
                                 "(limit %.1f cm), so no part of it is resting on anything."),
                            Ground.MinGapCm, Config.Thresholds.MaxGapCm));
                    Finding.Measurements->SetBoolField(TEXT("groundFound"), true);
                    Finding.Measurements->SetNumberField(TEXT("minGapCm"), Ground.MinGapCm);
                    Finding.Measurements->SetNumberField(TEXT("maxGapCm"), Ground.MaxGapCm);
                    Finding.Measurements->SetNumberField(TEXT("coverage"), Ground.Coverage);
                    Finding.Measurements->SetNumberField(TEXT("contactPoints"),
                        Ground.ContactPoints);
                    Sink.Add(MoveTemp(Finding));
                }
                else
                {
                    ++OutReport.Tallies[static_cast<int32>(ECheck::Airborne)].Clean;
                }
            }

            // ---- balanced ----
            // A floating actor is excluded from APPLICABILITY rather than handled inside:
            // "how many points is it balanced on" has no answer when nothing is touching, and
            // Airborne already owns that actor. bFloating is false for an unmeasured actor, so
            // one still enters here and gets its Unrunnable row.
            if (AuditBeginCheck(OutReport, Record, Config, ECheck::BalancedOnPoint,
                                Record.bGroundRequested && Record.bBoundsValid && !bFloating
                                && Ground.FootprintMaxCm >= Config.Thresholds.BalancedMinFootprintCm))
            {
                if (!bHasSupportSamples)
                {
                    GroundUnrunnable(ECheck::BalancedOnPoint, nullptr);
                }
                else if (!bCoverageOk)
                {
                    Sink.Add(AuditMakeFinding(Record, ECheck::BalancedOnPoint,
                        EFindingStatus::Unrunnable, ESeverity::Warning,
                        ErrorCodes::ERR_PARTIAL_GROUND_COVERAGE,
                        FString::Printf(
                            TEXT("Only %.0f%% of the footprint had ground under it, so the "
                                 "contact-point count is not a measure of how this actor rests."),
                            Ground.Coverage * 100.0)));
                }
                else if (Ground.ContactPoints < Config.Thresholds.BalancedMinContactPoints)
                {
                    FFinding Finding = AuditMakeFinding(Record, ECheck::BalancedOnPoint,
                        EFindingStatus::Flagged, ESeverity::Warning,
                        ErrorCodes::ERR_AUDIT_BALANCED_ON_POINT,
                        FString::Printf(
                            TEXT("This actor is %.0f cm across but touches the surface in only "
                                 "%d of %d supported footprint columns (%d required). Physically "
                                 "a valid rest; visually a physics glitch."),
                            Ground.FootprintMaxCm, Ground.ContactPoints, Ground.SupportedColumns,
                            Config.Thresholds.BalancedMinContactPoints));
                    Finding.Measurements->SetNumberField(TEXT("contactPoints"),
                        Ground.ContactPoints);
                    Finding.Measurements->SetNumberField(TEXT("supportedColumns"),
                        Ground.SupportedColumns);
                    Finding.Measurements->SetNumberField(TEXT("footprintMaxCm"),
                        Ground.FootprintMaxCm);
                    Finding.Measurements->SetNumberField(TEXT("maxGapCm"), Ground.MaxGapCm);
                    Sink.Add(MoveTemp(Finding));
                }
                else
                {
                    ++OutReport.Tallies[static_cast<int32>(ECheck::BalancedOnPoint)].Clean;
                }
            }

            // ---- data_validation ----
            if (AuditBeginCheck(OutReport, Record, Config, ECheck::DataValidation,
                                Record.Actor != nullptr))
            {
                // The engine's own structured per-actor hook (Actor.h:3009). The return value
                // is deliberately discarded: EDataValidationResult::NotValidated and
                // ::Valid both mean "nothing to report", and the issue list is the payload.
                FDataValidationContext Context;
                Record.Actor->IsDataValid(Context);
                const TArray<FDataValidationContext::FIssue>& Issues = Context.GetIssues();
                if (Issues.Num() > 0)
                {
                    int32 Reported = 0;
                    for (const FDataValidationContext::FIssue& Issue : Issues)
                    {
                        const bool bIsError = (Issue.Severity == EMessageSeverity::Error);
                        FString Text = Issue.Message.ToString();
                        // FIssue gained a TokenizedMessage (and the ability to carry its text
                        // there instead of in Message) in UE 5.4. On 5.3 FIssue is Message +
                        // Severity only, so Message is always the whole payload.
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 4, 0)
                        if (Text.IsEmpty() && Issue.TokenizedMessage.IsValid())
                        {
                            Text = Issue.TokenizedMessage->ToText().ToString();
                        }
#endif
                        FFinding Finding = AuditMakeFinding(Record, ECheck::DataValidation,
                            EFindingStatus::Flagged,
                            bIsError ? ESeverity::Error : ESeverity::Warning,
                            ErrorCodes::ERR_AUDIT_DATA_VALIDATION, Text);
                        Finding.Measurements->SetNumberField(TEXT("severity"),
                            static_cast<int32>(Issue.Severity));
                        Finding.Measurements->SetNumberField(TEXT("issueCount"), Issues.Num());
                        Sink.Add(MoveTemp(Finding));
                        if (++Reported >= AuditMaxEchoedNames)
                        {
                            break;
                        }
                    }
                }
                else
                {
                    ++OutReport.Tallies[static_cast<int32>(ECheck::DataValidation)].Clean;
                }
            }
        }

        // ---- Pass C: duplicate transforms (needs every record) ----
        if (HasCheck(Config.SelectedChecks, ECheck::DuplicateTransform))
        {
            FCheckTally& Tally = OutReport.Tallies[static_cast<int32>(ECheck::DuplicateTransform)];
            TArray<int32> Candidates;
            Candidates.Reserve(Records.Num());
            for (int32 Index = 0; Index < Records.Num(); ++Index)
            {
                const FAuditRecord& Record = Records[Index];
                if (HasCheck(Record.IgnoredMask, ECheck::DuplicateTransform))
                {
                    ++Tally.Ignored;
                    continue;
                }
                if (!Record.bHasRootComponent || Record.Transform.ContainsNaN())
                {
                    ++Tally.NotApplicable;
                    continue;
                }
                ++Tally.Applicable;
                Candidates.Add(Index);
            }

            // Sorted-scan rather than a hash grid: a grid quantized at the tolerance splits
            // near-equal values across neighbouring cells, so it MISSES duplicates unless it
            // also scans 26 neighbours. Sorting is exact and n log n on a set this size.
            Candidates.Sort([&Records](int32 A, int32 B)
            {
                const FAuditRecord& RA = Records[A];
                const FAuditRecord& RB = Records[B];
                if (RA.ClassName != RB.ClassName) { return RA.ClassName < RB.ClassName; }
                const FVector LA = RA.Transform.GetLocation();
                const FVector LB = RB.Transform.GetLocation();
                if (LA.X != LB.X) { return LA.X < LB.X; }
                if (LA.Y != LB.Y) { return LA.Y < LB.Y; }
                return LA.Z < LB.Z;
            });

            const double Tol = FMath::Max(Config.Thresholds.DuplicateToleranceCm, 0.0);
            const double AngleTol = FMath::Max(Config.Thresholds.DuplicateAngleToleranceDeg, 0.0);
            TArray<bool> Grouped;
            Grouped.Init(false, Candidates.Num());

            for (int32 I = 0; I < Candidates.Num(); ++I)
            {
                if (Grouped[I])
                {
                    continue;
                }
                const FAuditRecord& Head = Records[Candidates[I]];
                const FVector HeadLoc = Head.Transform.GetLocation();
                const FRotator HeadRot = Head.Transform.Rotator();

                TArray<int32> Group;
                Group.Add(I);
                for (int32 J = I + 1; J < Candidates.Num(); ++J)
                {
                    const FAuditRecord& Other = Records[Candidates[J]];
                    if (Other.ClassName != Head.ClassName)
                    {
                        break; // class blocks are contiguous after the sort
                    }
                    const FVector OtherLoc = Other.Transform.GetLocation();
                    if (OtherLoc.X - HeadLoc.X > Tol)
                    {
                        break; // sorted on X: nothing further can be within tolerance
                    }
                    if (Grouped[J])
                    {
                        continue;
                    }
                    if (FMath::Abs(OtherLoc.Y - HeadLoc.Y) > Tol
                        || FMath::Abs(OtherLoc.Z - HeadLoc.Z) > Tol)
                    {
                        continue;
                    }
                    // TRotator<double>::Equals(const TRotator<T>&, T Tolerance) - Rotator.h:232,
                    // so the tolerance is a double here, not a float.
                    if (!Other.Transform.Rotator().Equals(HeadRot, AngleTol))
                    {
                        continue;
                    }
                    Group.Add(J);
                }

                if (Group.Num() < 2)
                {
                    ++Tally.Clean;
                    continue;
                }

                TArray<TSharedPtr<FJsonValue>> Peers;
                for (const int32 Slot : Group)
                {
                    if (Peers.Num() < AuditMaxEchoedNames)
                    {
                        Peers.Add(MakeShared<FJsonValueString>(Records[Candidates[Slot]].Label));
                    }
                }

                for (const int32 Slot : Group)
                {
                    Grouped[Slot] = true;
                    FAuditRecord& Member = Records[Candidates[Slot]];
                    FFinding Finding = AuditMakeFinding(Member, ECheck::DuplicateTransform,
                        EFindingStatus::Flagged, ESeverity::Warning,
                        ErrorCodes::ERR_AUDIT_DUPLICATE_TRANSFORM,
                        FString::Printf(
                            TEXT("%d actors of class %s share this location and rotation within "
                                 "%.2f cm / %.2f deg."),
                            Group.Num(), *Member.ClassName, Tol, AngleTol));
                    Finding.Measurements->SetNumberField(TEXT("groupSize"), Group.Num());
                    Finding.Measurements->SetObjectField(TEXT("location"),
                        AuditVectorObject(Member.Transform.GetLocation()));
                    Finding.Measurements->SetArrayField(TEXT("actors"), Peers);
                    // Through the same sink as every other finding, so the tallies and the
                    // rows cannot drift apart here either.
                    FAuditSink Sink;
                    Sink.Report = &OutReport;
                    Sink.Record = &Member;
                    Sink.Add(MoveTemp(Finding));
                }
            }
        }

        // ---- Pass D: support chains (needs every record's ground measure) ----
        if (HasCheck(Config.SelectedChecks, ECheck::UnsupportedAssembly))
        {
            FCheckTally& Tally = OutReport.Tallies[static_cast<int32>(ECheck::UnsupportedAssembly)];

            TMap<const AActor*, int32> ActorToRecord;
            ActorToRecord.Reserve(Records.Num());
            for (int32 Index = 0; Index < Records.Num(); ++Index)
            {
                ActorToRecord.Add(Records[Index].Actor, Index);
            }

            for (int32 Index = 0; Index < Records.Num(); ++Index)
            {
                FAuditRecord& Record = Records[Index];
                if (HasCheck(Record.IgnoredMask, ECheck::UnsupportedAssembly))
                {
                    ++Tally.Ignored;
                    continue;
                }
                // Applicable only when this actor demonstrably rests on ANOTHER ACTOR. An
                // actor on terrain, or one whose contact columns named nothing, has no
                // assembly above it to be wrong about.
                AActor* Support = Record.Ground.SupportActor.Get();
                if (!Record.Ground.bMeasured || !Support || Record.Ground.bSupportIsTerrain)
                {
                    ++Tally.NotApplicable;
                    continue;
                }
                ++Tally.Applicable;

                FAuditSink Sink;
                Sink.Report = &OutReport;
                Sink.Record = &Record;

                TSet<int32> Visited;
                Visited.Add(Index);
                const AActor* Link = Support;
                FString ChainText = Record.Label;
                bool bResolved = false;

                for (int32 Depth = 0; Depth < AuditMaxSupportDepth; ++Depth)
                {
                    const int32* LinkIndex = ActorToRecord.Find(Link);
                    if (!LinkIndex)
                    {
                        Sink.Add(AuditMakeFinding(Record, ECheck::UnsupportedAssembly,
                            EFindingStatus::Unrunnable, ESeverity::Warning,
                            ErrorCodes::ERR_AUDIT_SUPPORT_CHAIN_UNRESOLVED,
                            FString::Printf(
                                TEXT("This actor rests on '%s', which is not in the audited set "
                                     "(scoped out, paged out, or its ground was not measured), so "
                                     "whether the stack is supported is unknown. Chain so far: %s."),
                                *Link->GetActorLabel(), *ChainText)));
                        bResolved = true;
                        break;
                    }
                    if (Visited.Contains(*LinkIndex))
                    {
                        Sink.Add(AuditMakeFinding(Record, ECheck::UnsupportedAssembly,
                            EFindingStatus::Unrunnable, ESeverity::Warning,
                            ErrorCodes::ERR_AUDIT_SUPPORT_CHAIN_UNRESOLVED,
                            FString::Printf(
                                TEXT("The support chain from this actor is cyclic (%s), so it "
                                     "cannot be resolved to a ground surface."), *ChainText)));
                        bResolved = true;
                        break;
                    }
                    Visited.Add(*LinkIndex);

                    const FAuditRecord& LinkRecord = Records[*LinkIndex];
                    ChainText += TEXT(" -> ");
                    ChainText += LinkRecord.Label;

                    if (!LinkRecord.Ground.bMeasured
                        || !LinkRecord.Ground.UnrunnableCode.IsEmpty())
                    {
                        Sink.Add(AuditMakeFinding(Record, ECheck::UnsupportedAssembly,
                            EFindingStatus::Unrunnable, ESeverity::Warning,
                            ErrorCodes::ERR_AUDIT_SUPPORT_CHAIN_UNRESOLVED,
                            FString::Printf(
                                TEXT("'%s' in this actor's support chain has no usable ground "
                                     "measurement, so the stack cannot be resolved. Chain: %s."),
                                *LinkRecord.Label, *ChainText)));
                        bResolved = true;
                        break;
                    }

                    const bool bLinkFloating =
                        (LinkRecord.Ground.SupportedColumns == 0)
                        || (LinkRecord.Ground.MinGapCm > Config.Thresholds.MaxGapCm);
                    if (bLinkFloating)
                    {
                        FFinding Finding = AuditMakeFinding(Record, ECheck::UnsupportedAssembly,
                            EFindingStatus::Flagged, ESeverity::Warning,
                            ErrorCodes::ERR_AUDIT_UNSUPPORTED_ASSEMBLY,
                            FString::Printf(
                                TEXT("This actor rests on a stack that ends in mid-air: '%s' has "
                                     "nothing under it. Chain: %s. Each piece passes a per-actor "
                                     "contact check on its own; the assembly does not."),
                                *LinkRecord.Label, *ChainText));
                        Finding.Measurements->SetStringField(TEXT("chain"), ChainText);
                        Finding.Measurements->SetStringField(TEXT("floatingRoot"),
                            LinkRecord.Label);
                        Finding.Measurements->SetNumberField(TEXT("rootMinGapCm"),
                            LinkRecord.Ground.MinGapCm);
                        Sink.Add(MoveTemp(Finding));
                        bResolved = true;
                        break;
                    }

                    if (LinkRecord.Ground.bSupportIsTerrain)
                    {
                        ++Tally.Clean;
                        bResolved = true;
                        break;
                    }
                    AActor* Next = LinkRecord.Ground.SupportActor.Get();
                    if (!Next)
                    {
                        // Touching something the filter accepted but could not name. It is
                        // resting on SOMETHING, so this is not a floating assembly.
                        ++Tally.Clean;
                        bResolved = true;
                        break;
                    }
                    Link = Next;
                }

                if (!bResolved)
                {
                    Sink.Add(AuditMakeFinding(Record, ECheck::UnsupportedAssembly,
                        EFindingStatus::Unrunnable, ESeverity::Warning,
                        ErrorCodes::ERR_AUDIT_SUPPORT_CHAIN_UNRESOLVED,
                        FString::Printf(
                            TEXT("The support chain from this actor is deeper than %d links and "
                                 "was not resolved. Chain so far: %s."),
                            AuditMaxSupportDepth, *ChainText)));
                }
            }
        }

        // ---- Totals, coverage statements, clean list ----
        for (const FAuditRecord& Record : Records)
        {
            if (Record.bAnyFinding)
            {
                ++OutReport.ActorsFlagged;
            }
            else if (OutReport.CleanActors.Num() < Config.MaxCleanActors)
            {
                OutReport.CleanActors.Add(Record.Label);
            }
        }

        // Foliage instances are not actors, so nothing above ever looked at one. Saying so is
        // the difference between "the audit found no problems" and "the audit cannot see
        // this".
        int32 FoliageInstances = 0;
        int32 FoliageComponents = 0;
        for (const FAuditRecord& Record : Records)
        {
            TArray<UInstancedStaticMeshComponent*> Instanced;
            Record.Actor->GetComponents<UInstancedStaticMeshComponent>(Instanced);
            for (const UInstancedStaticMeshComponent* Component : Instanced)
            {
                if (Component)
                {
                    ++FoliageComponents;
                    FoliageInstances += Component->GetInstanceCount();
                }
            }
        }
        if (FoliageInstances > 0)
        {
            OutReport.Caveats.Add(FString::Printf(
                TEXT("%d instance(s) across %d instanced-mesh component(s) were NOT examined: "
                     "instances are not actors and no check here can see one. Their placement is "
                     "invisible to this audit."),
                FoliageInstances, FoliageComponents));
        }

        if (Config.bHasSurface
            && Config.Surface.Preset == GroundPlacement::ESurfacePreset::Landscape
            && HasCheck(Config.SelectedChecks, ECheck::UnsupportedAssembly))
        {
            OutReport.Caveats.Add(TEXT(
                "surface preset 'landscape' accepts terrain hits only, so no actor-on-actor "
                "support edge can be observed and unsupported_assembly is not-applicable for "
                "every actor. Use preset 'any_solid' (or a custom accept list) to resolve "
                "stacks."));
        }
        if (Config.bHasSurface
            && Config.Surface.Preset != GroundPlacement::ESurfacePreset::Landscape
            && HasCheck(Config.SelectedChecks, ECheck::BelowSurface))
        {
            OutReport.Caveats.Add(TEXT(
                "below_surface probes downward from above the actor and takes the FIRST accepted "
                "surface. With a non-landscape surface spec a roof or an overhang above an actor "
                "is accepted as that surface, and the actor is reported underground. Prefer "
                "preset 'landscape' for this check."));
        }

        // ---- World Partition coverage ----
        // A TActorIterator sweep can only report on LOADED actors: FActorIteratorState builds
        // its working set from ULevel::Actors (EngineUtils.h:216-229), and an unloaded World
        // Partition actor is in no level's array. Counting the descriptors is how the report
        // says "there are N more actors here that I did not look at" instead of implying the
        // level is clean.
        if (UWorldPartition* Partition = World->GetWorldPartition())
        {
            OutReport.bWorldPartition = true;
            int32 Total = 0;
            int32 Unloaded = 0;
            // Generic lambda: the descriptor type is FWorldPartitionActorDescInstance on 5.4+ and
            // FWorldPartitionActorDesc on 5.3. Both expose IsLoaded().
            FWorldPartitionHelpers::MCP_FOR_EACH_ACTOR_DESC(Partition, AActor::StaticClass(),
                [&Total, &Unloaded](const auto* Desc)
                {
                    ++Total;
                    if (Desc && !Desc->IsLoaded())
                    {
                        ++Unloaded;
                    }
                    return true;
                });
            OutReport.ActorDescCount = Total;
            OutReport.UnloadedActorCount = Unloaded;
            if (Unloaded > 0)
            {
                OutReport.Caveats.Add(FString::Printf(
                    TEXT("This is a World Partition map and %d of %d actor descriptors are NOT "
                         "loaded. Those actors were not examined by any check; load them (or the "
                         "relevant region) before treating this report as complete."),
                    Unloaded, Total));
            }
        }
    }
}
