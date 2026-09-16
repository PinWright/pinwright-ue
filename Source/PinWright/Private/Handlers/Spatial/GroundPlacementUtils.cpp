// Copyright (c) 2026 Alexander Penkin. MIT License.

// GroundPlacementUtils.cpp - the footprint-sampling ground seat solver and its verification.
// See GroundPlacementUtils.h for the design rationale and the engine-reuse decisions.

#include "Handlers/Spatial/GroundPlacementUtils.h"

#include "Handlers/ErrorCodes.h"
#include "Utils/ActorUtils.h" // McpActorUtils::FindActorByName, for the shared surface parser
#include "Utils/CollisionSummaryUtils.h" // the shared trace-flag read (CTF_UseDefault resolved)

#include "CollisionQueryParams.h"
#include "Dom/JsonValue.h"
#include "Components/InstancedStaticMeshComponent.h" // HISM and the foliage component derive from it
#include "Components/PrimitiveComponent.h"
#include "Engine/HitResult.h"
#include "Engine/StaticMesh.h" // UStaticMesh::GetBounds, the per-instance footprint source
#include "Engine/World.h"
#include "EngineUtils.h" // TActorIterator
#include "GameFramework/Actor.h"
#include "LandscapeProxy.h"
#include "Math/UnrealMathUtility.h"

namespace
{
    // Cap on how many rejected-surface-actor names a report echoes. Past a couple of dozen
    // names the COUNT is the useful signal, not the roster - the same rule
    // RaycastHandler.cpp:36 applies to its filteredOut list.
    constexpr int32 GroundMaxReportedRejects = 16;

    // Cap on how many distinct ground primitives a report names. A footprint is at most
    // MaxGridSize^2 columns, so this can never truncate a small grid; on a large one the head of
    // the distribution is what identifies the surface and the tail is noise. The true distinct
    // total stays in FGroundProvenance::SurfaceComponentCount, so a truncated list is never
    // mistaken for the whole roster.
    constexpr int32 GroundMaxReportedSurfaceComponents = 8;

    // Slack added above/below the actor's AABB when probing its own underside, so a component
    // whose geometry sits exactly on the AABB face is still crossed by the segment.
    constexpr double GroundUndersideProbeSlack = 2.0;

    // Below this the actor has no usable footprint (a pure logic actor, a light with no
    // primitive, a fully-degenerate mesh) and there is nothing to seat.
    constexpr double GroundMinExtentCm = 0.01;

    // Cosine guard for the align-to-surface path: a normal this far from the sampled average
    // is treated as unusable rather than followed.
    constexpr double GroundMinNormalLength = 0.001;

    // Lowest point of the actor's OWN geometry in the given world column, found by tracing
    // UPWARD from below its bounds: the first thing an upward ray meets is the underside.
    //
    // Queries each primitive component directly through UPrimitiveComponent::LineTraceComponent
    // (UE 5.8 Components/PrimitiveComponent.h:3053) rather than through the world, because that
    // path goes straight to the component's own FBodyInstance and therefore does NOT depend on
    // the component's collision-channel responses. A prop that ignores ECC_Visibility still
    // reports its own shape, and no other actor can shadow the answer.
    //
    // Complex first, then simple: complex collision for a StaticMesh is its render triangles,
    // which is the true underside; a mesh that has only simple primitives answers the second
    // pass instead. Returns false when NO component answered either way - the caller then
    // falls back to the flat bounds plane and records that it did.
    bool GroundProbeUndersideZ(AActor* Actor, double X, double Y, double TopZ, double BottomZ,
                               double& OutZ)
    {
        if (!Actor)
        {
            return false;
        }

        TArray<UPrimitiveComponent*> Components;
        Actor->GetComponents<UPrimitiveComponent>(Components);
        if (Components.Num() == 0)
        {
            return false;
        }

        const FVector Start(X, Y, BottomZ - GroundUndersideProbeSlack);
        const FVector End(X, Y, TopZ + GroundUndersideProbeSlack);

        bool bFound = false;
        double Lowest = 0.0;
        for (UPrimitiveComponent* Component : Components)
        {
            if (!Component)
            {
                continue;
            }
            for (int32 Pass = 0; Pass < 2; ++Pass)
            {
                const bool bComplex = (Pass == 0);
                FCollisionQueryParams Params(FName(TEXT("PinWrightGroundUnderside")), bComplex);
                FHitResult Hit;
                if (Component->LineTraceComponent(Hit, Start, End, Params))
                {
                    const double HitZ = Hit.ImpactPoint.Z;
                    if (!bFound || HitZ < Lowest)
                    {
                        Lowest = HitZ;
                        bFound = true;
                    }
                    break; // this component answered; don't double-count it on the simple pass
                }
            }
        }

        if (bFound)
        {
            OutZ = Lowest;
        }
        return bFound;
    }

    // Fill the identity + failure fields of a result that is giving up. AppliedTransform is
    // left untouched on every path through here, so a result routed through it can never
    // report IsSeated() - the failure paths need no discipline of their own.
    //
    // File-scope anonymous namespace with a Ground* prefix, per the Unity-build rule in
    // CLAUDE.md: everything file-local in this TU lives in this one block.
    void GroundFailResult(GroundPlacement::FGroundSeatResult& Result,
                          GroundPlacement::EGroundSeatStatus Status,
                          const TCHAR* Code, const FString& Reason)
    {
        Result.Status = Status;
        Result.ReasonCode = Code;
        Result.Reason = Reason;
    }

    // Map a pre-move measurement's own failure code onto a seat status. Shared by the actor and
    // the instance seat so the two cannot classify the same measurement differently: the reason
    // text and code both come from EvaluateContact, and this is the only place that decides which
    // status they arrive under.
    GroundPlacement::EGroundSeatStatus GroundStatusForMeasurementFailure(const FString& FailReasonCode)
    {
        if (FailReasonCode == ErrorCodes::ERR_GROUND_HITS_ALL_REJECTED)
        {
            return GroundPlacement::EGroundSeatStatus::GroundHitsAllRejected;
        }
        if (FailReasonCode == ErrorCodes::ERR_PARTIAL_GROUND_COVERAGE)
        {
            return GroundPlacement::EGroundSeatStatus::PartialGroundCoverage;
        }
        if (FailReasonCode == ErrorCodes::ERR_GROUND_NOT_MEASURED)
        {
            return GroundPlacement::EGroundSeatStatus::ActorHasNoBounds;
        }
        return GroundPlacement::EGroundSeatStatus::NoGroundFound;
    }

    // Record which PRIMITIVE answered Column, as one row per distinct component with a column
    // tally. Called once per SUPPORTED column, so the tally is over exactly the columns every
    // other provenance number is folded over.
    //
    // A column whose component could not be resolved contributes no ROW: an unknown is not a
    // surface, and inventing an "unnamed" row for it would read as a distinct surface that
    // answered. It is still COUNTED, in FGroundProvenance::UnclassifiedColumns, because the
    // trust verdict has to be able to say "could not determine" rather than fall silent and be
    // read as "nothing wrong". The SupportedColumns-minus-tallies derivation this comment used
    // to point at stopped being available once the roster gained a cap.
    //
    // Classifies each distinct primitive once. The two reads are cheap but not free - the trace
    // flag goes through the component's body setup - and a full grid answered by one surface
    // would otherwise pay for them on every sample.
    void GroundClassifySurfaceTrustPW(GroundPlacement::FGroundProvenance::FSurfaceComponent& Entry,
                                      UPrimitiveComponent* Component)
    {
        // HISM and the foliage component both derive from UInstancedStaticMeshComponent, so one
        // ancestry test covers every scatter shape any_solid deliberately admits.
        Entry.bInstancedScatter = Component->IsA<UInstancedStaticMeshComponent>();
        // Through the shared collision seam rather than off BodySetup->CollisionTraceFlag: that
        // raw field carries CTF_UseDefault, which resolves against the project's default shape
        // complexity, and reading it directly classifies every defaulted mesh wrong.
        Entry.bComplexAsSimple =
            PinWrightCollisionSummary::Summarize(Component).Trace
                == PinWrightCollisionSummary::ETraceKind::ComplexAsSimple;
    }

    // Folds one column's trust facts into the row that answered it and into the batch counters.
    // bRenderGeometry is per COLUMN because FSpatialHit::bRenderGeometryHit is: it is
    // bTraceComplex AND a zero simple-shape count, so a single component can answer one column
    // that way and another not, and OR is the honest fold - "it happened here at least once".
    void GroundFoldSurfaceTrustPW(GroundPlacement::FGroundProvenance& Provenance,
                                  GroundPlacement::FGroundProvenance::FSurfaceComponent& Entry,
                                  const GroundPlacement::FGroundColumn& Column)
    {
        Entry.bRenderGeometry |= Column.bGroundRenderGeometryHit;
        Provenance.InstancedScatterColumns += Entry.bInstancedScatter ? 1 : 0;
        Provenance.ComplexAsSimpleColumns += Entry.bComplexAsSimple ? 1 : 0;
    }

    void GroundNoteSurfaceComponent(GroundPlacement::FGroundProvenance& Provenance,
                                    const GroundPlacement::FGroundColumn& Column)
    {
        UPrimitiveComponent* Component = Column.GroundComponent.Get();
        if (!Component)
        {
            ++Provenance.UnclassifiedColumns;
            return;
        }

        for (GroundPlacement::FGroundProvenance::FSurfaceComponent& Existing :
             Provenance.SurfaceComponents)
        {
            if (Existing.Component.Get() == Component)
            {
                ++Existing.Columns;
                GroundFoldSurfaceTrustPW(Provenance, Existing, Column);
                return;
            }
        }

        GroundPlacement::FGroundProvenance::FSurfaceComponent Entry;
        Entry.Component = Component;
        Entry.ComponentName = Component->GetName();
        Entry.ComponentClass = Component->GetClass()->GetName();
        if (const AActor* Owner = Column.GroundActor.Get())
        {
            Entry.ActorLabel = Owner->GetActorLabel();
        }
        Entry.Columns = 1;
        GroundClassifySurfaceTrustPW(Entry, Component);
        GroundFoldSurfaceTrustPW(Provenance, Entry, Column);
        Provenance.SurfaceComponents.Add(MoveTemp(Entry));
    }
}

namespace GroundPlacement
{
    // ---- Surface spec ------------------------------------------------------------------

    void FGroundSurfaceSpec::ApplyPreset()
    {
        switch (Preset)
        {
            case ESurfacePreset::Landscape:
                // ALandscape and ALandscapeStreamingProxy both derive from ALandscapeProxy, so
                // one accept entry covers a monolithic landscape and a World-Partition-streamed
                // one. Matched by ancestry (SpatialTraceUtils.cpp, FSpatialHitFilter::Matches).
                Filter.OnlyClasses.AddUnique(TEXT("LandscapeProxy"));
                bTraceComplex = false;
                break;

            case ESurfacePreset::AnySolid:
                // No accept list - anything that blocks is a candidate - minus the two families
                // that are render geometry pretending to be ground. Foliage is excluded by class
                // because its instanced components are collisionless yet still block a complex
                // query; effect cards (haze, fog, glow, light shafts, decals) are excluded by
                // what they ARE, because they DO carry a simple collider and therefore block
                // even a simple query.
                //
                // The effect-card exclusion used to be the wildcard name "FG_*", which is one
                // project's prefix. That silently disqualified any customer actor whose name
                // happened to start FG_, and gave a customer whose cards use a different prefix
                // none of the protection this preset advertises. IsEffectGeometryActor answers
                // the same question from the component classes and material blend modes, which
                // every project has and none has to be told about.
                //
                // The foliage exclusion needs BOTH axes, and the actor one alone was the guard
                // protecting only the road nobody takes. AInstancedFoliageActor is the level's
                // shared foliage-tool holder; a builder who needs an addressable, rebuildable
                // vegetation layer scatters it as instanced components on an ordinary AActor
                // instead, and that actor is not an AInstancedFoliageActor, so the class entry
                // below never fires on it. Only the STRUCK COMPONENT distinguishes it, which is
                // what ExcludeComponentClasses reads.
                //
                // The two entries are the engine component classes that exist for vegetation and
                // for nothing else, so excluding them is exactly what "except foliage" already
                // promises. They are SIBLINGS, both deriving straight from
                // UHierarchicalInstancedStaticMeshComponent, so neither covers the other by
                // ancestry and both must be listed.
                //
                // The generic instanced classes are deliberately NOT here. A HISM/ISM scatter of
                // paving stones, rocks, debris or modular tiles is legitimate ground, and a
                // preset that excluded UInstancedStaticMeshComponent wholesale would move every
                // actor a caller had already seated on one. That case is the caller's to state,
                // via the excludeComponentClasses list this preset shares.
                Filter.ExcludeClasses.AddUnique(TEXT("InstancedFoliageActor"));
                Filter.ExcludeComponentClasses.AddUnique(TEXT("FoliageInstancedStaticMeshComponent"));
                Filter.ExcludeComponentClasses.AddUnique(TEXT("GrassInstancedStaticMeshComponent"));
                Filter.bExcludeEffectGeometry = true;
                bTraceComplex = false;
                break;

            case ESurfacePreset::Custom:
            default:
                break;
        }
    }

    bool ParseSurfacePreset(const FString& Name, ESurfacePreset& OutPreset)
    {
        const FString Lower = Name.ToLower();
        if (Lower == TEXT("landscape"))
        {
            OutPreset = ESurfacePreset::Landscape;
            return true;
        }
        if (Lower == TEXT("any_solid") || Lower == TEXT("anysolid"))
        {
            OutPreset = ESurfacePreset::AnySolid;
            return true;
        }
        if (Lower == TEXT("custom"))
        {
            OutPreset = ESurfacePreset::Custom;
            return true;
        }
        return false;
    }

    const TCHAR* SurfacePresetToString(ESurfacePreset Preset)
    {
        switch (Preset)
        {
            case ESurfacePreset::Landscape: return TEXT("landscape");
            case ESurfacePreset::AnySolid:  return TEXT("any_solid");
            default:                        return TEXT("custom");
        }
    }

    bool ParseSurfaceJson(const TSharedPtr<FJsonObject>& SurfaceObj, UWorld* World,
                          FGroundSurfaceSpec& OutSpec, TArray<FString>& OutUnresolvedIgnores,
                          FString& OutError)
    {
        if (!SurfaceObj.IsValid())
        {
            OutError = TEXT("'surface' must be an object. State what counts as ground: "
                            "{\"preset\":\"landscape\"} for terrain (the right answer for a height "
                            "probe - a landscape heightfield is single-valued per column so "
                            "nothing can shadow it), {\"preset\":\"any_solid\"} for anything solid "
                            "except foliage and effect geometry, or {\"preset\":\"custom\", "
                            "\"onlyClasses\":[...], \"excludeNames\":[...]} to say it exactly.");
            return false;
        }

        // Everything below writes into a scratch copy, so a rejected spec leaves the caller's
        // untouched rather than half-populated.
        FGroundSurfaceSpec Spec = OutSpec;

        FString PresetName = TEXT("custom");
        SurfaceObj->TryGetStringField(TEXT("preset"), PresetName);
        ESurfacePreset Preset = ESurfacePreset::Custom;
        if (!ParseSurfacePreset(PresetName, Preset))
        {
            OutError = FString::Printf(
                TEXT("Unknown surface preset '%s'. Valid: landscape, any_solid, custom."),
                *PresetName);
            return false;
        }
        Spec.Preset = Preset;

        FString ChannelName;
        if (SurfaceObj->TryGetStringField(TEXT("channel"), ChannelName) && !ChannelName.IsEmpty())
        {
            const FString Lower = ChannelName.ToLower();
            if      (Lower == TEXT("visibility"))   { Spec.Channel = ECC_Visibility; }
            else if (Lower == TEXT("camera"))       { Spec.Channel = ECC_Camera; }
            else if (Lower == TEXT("worldstatic"))  { Spec.Channel = ECC_WorldStatic; }
            else if (Lower == TEXT("worlddynamic")) { Spec.Channel = ECC_WorldDynamic; }
            else
            {
                OutError = FString::Printf(
                    TEXT("Unknown surface channel '%s'. Valid: visibility, camera, worldstatic, "
                         "worlddynamic."), *ChannelName);
                return false;
            }
        }

        // Preset first, so explicit caller lists ADD to it rather than being erased by it.
        Spec.ApplyPreset();

        bool bComplex = false;
        if (SurfaceObj->TryGetBoolField(TEXT("traceComplex"), bComplex)
            || SurfaceObj->TryGetBoolField(TEXT("trace_complex"), bComplex))
        {
            Spec.bTraceComplex = bComplex;
        }

        // Read AFTER ApplyPreset so an explicit false can turn the any_solid preset's intrinsic
        // effect-card rejection back off - the one knob here where the caller may need to
        // override the preset rather than add to it (a project whose real ground genuinely is a
        // translucent surface, e.g. a glass walkway).
        bool bExcludeEffects = false;
        if (SurfaceObj->TryGetBoolField(TEXT("excludeEffectGeometry"), bExcludeEffects)
            || SurfaceObj->TryGetBoolField(TEXT("exclude_effect_geometry"), bExcludeEffects))
        {
            Spec.Filter.bExcludeEffectGeometry = bExcludeEffects;
        }

        // Non-empty trimmed strings out of a JSON array field, skipping non-strings.
        auto CollectStrings = [&SurfaceObj](const TCHAR* Key, TArray<FString>& Out)
        {
            const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
            if (!SurfaceObj->TryGetArrayField(Key, Array) || !Array)
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
        };

        CollectStrings(TEXT("onlyClasses"), Spec.Filter.OnlyClasses);
        CollectStrings(TEXT("only_classes"), Spec.Filter.OnlyClasses);
        CollectStrings(TEXT("excludeClasses"), Spec.Filter.ExcludeClasses);
        CollectStrings(TEXT("exclude_classes"), Spec.Filter.ExcludeClasses);
        CollectStrings(TEXT("excludeComponentClasses"), Spec.Filter.ExcludeComponentClasses);
        CollectStrings(TEXT("exclude_component_classes"), Spec.Filter.ExcludeComponentClasses);
        CollectStrings(TEXT("excludeNames"), Spec.Filter.ExcludeNames);
        CollectStrings(TEXT("exclude_names"), Spec.Filter.ExcludeNames);

        TArray<FString> IgnoreNames;
        CollectStrings(TEXT("ignoreActors"), IgnoreNames);
        CollectStrings(TEXT("ignore_actors"), IgnoreNames);
        for (const FString& Name : IgnoreNames)
        {
            if (AActor* Found = McpActorUtils::FindActorByName(World, Name))
            {
                Spec.IgnoreActors.AddUnique(TWeakObjectPtr<AActor>(Found));
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
            Spec.MaxLayers = FMath::Clamp(static_cast<int32>(MaxLayers), 1,
                SpatialTraceUtils::MaxAllowedLayers);
        }
        double MaxDrop = 0.0;
        if (SurfaceObj->TryGetNumberField(TEXT("maxDrop"), MaxDrop)
            || SurfaceObj->TryGetNumberField(TEXT("max_drop"), MaxDrop))
        {
            Spec.MaxDropCm = FMath::Max(MaxDrop, 0.0);
        }
        double ProbeLift = 0.0;
        if (SurfaceObj->TryGetNumberField(TEXT("probeLift"), ProbeLift)
            || SurfaceObj->TryGetNumberField(TEXT("probe_lift"), ProbeLift))
        {
            Spec.ProbeLiftCm = FMath::Max(ProbeLift, 0.0);
        }

        OutSpec = MoveTemp(Spec);
        return true;
    }

    const TCHAR* SeatStatusToString(EGroundSeatStatus Status)
    {
        switch (Status)
        {
            case EGroundSeatStatus::ActorNotFound:         return TEXT("actor_not_found");
            case EGroundSeatStatus::ActorHasNoBounds:      return TEXT("actor_has_no_bounds");
            case EGroundSeatStatus::ActorLocationLocked:   return TEXT("actor_location_locked");
            case EGroundSeatStatus::HolderNotSeatable:     return TEXT("holder_not_seatable");
            case EGroundSeatStatus::DryRun:                return TEXT("dry_run");
            case EGroundSeatStatus::NoGroundFound:         return TEXT("no_ground_found");
            case EGroundSeatStatus::GroundHitsAllRejected: return TEXT("ground_hits_all_rejected");
            case EGroundSeatStatus::PartialGroundCoverage: return TEXT("partial_ground_coverage");
            case EGroundSeatStatus::VerificationFailed:    return TEXT("verification_failed");
            case EGroundSeatStatus::Reverted:              return TEXT("reverted");
            case EGroundSeatStatus::Seated:                return TEXT("seated");
            default:                                       return TEXT("not_attempted");
        }
    }

    double FGroundSeatConfig::ResolveEmbedCm(double BoundsHeightCm) const
    {
        const double FromFraction = EmbedFraction * FMath::Max(BoundsHeightCm, 0.0);
        return FMath::Max(EmbedDepthCm + FromFraction, 0.0);
    }

    // ---- Landscape diagnostic ----------------------------------------------------------

    TOptional<double> ProbeLandscapeHeight(UWorld* World, double X, double Y)
    {
        if (!World)
        {
            return TOptional<double>();
        }
        // The Z handed in is ignored by the heightfield lookup (it converts to actor space and
        // indexes by XY), so any finite Z is fine.
        const FVector Query(X, Y, 0.0);
        for (TActorIterator<ALandscapeProxy> It(World); It; ++It)
        {
            ALandscapeProxy* Proxy = *It;
            if (!Proxy)
            {
                continue;
            }
            const TOptional<float> Height = Proxy->GetHeightAtLocation(Query);
            if (Height.IsSet())
            {
                return TOptional<double>(static_cast<double>(Height.GetValue()));
            }
        }
        return TOptional<double>();
    }

    // ---- Aggregation ---------------------------------------------------------------------

    void AggregateColumns(const TArray<FGroundColumn>& Columns,
                          const FContactThresholds& Thresholds,
                          FGroundContactReport& Report)
    {
        Report.SampledColumns = Columns.Num();
        Report.ActorColumns = 0;
        Report.SupportedColumns = 0;
        Report.ContactPoints = 0;
        // Counts only - Provenance.bTraceComplex describes the probe, which this function did
        // not issue, and clearing the whole struct here would erase it.
        Report.Provenance.TriangleColumns = 0;
        Report.Provenance.PrimitiveColumns = 0;
        Report.Provenance.RenderGeometryColumns = 0;
        Report.Provenance.InstancedScatterColumns = 0;
        Report.Provenance.ComplexAsSimpleColumns = 0;
        Report.Provenance.UnclassifiedColumns = 0;
        Report.Provenance.MinSimpleCollisionShapes = -1;
        Report.Provenance.MaxSimpleCollisionShapes = -1;
        Report.Provenance.SurfaceComponents.Reset();
        Report.Provenance.SurfaceComponentCount = 0;

        FVector NormalSum = FVector::ZeroVector;
        double MinGround = 0.0;
        double MaxGround = 0.0;
        double MinUnderside = 0.0;
        double MaxUnderside = 0.0;
        bool bFirstSupported = true;

        for (const FGroundColumn& Column : Columns)
        {
            if (Column.bHasActorGeometry)
            {
                ++Report.ActorColumns;
            }
            if (!Column.IsSupported())
            {
                continue;
            }
            ++Report.SupportedColumns;

            // A face index is the engine's own statement that a triangle mesh or heightfield
            // answered; its absence says a simple primitive did, or that the backend reported
            // none. Counted both ways rather than reduced to one label.
            if (Column.GroundFaceIndex >= 0)
            {
                ++Report.Provenance.TriangleColumns;
            }
            else
            {
                ++Report.Provenance.PrimitiveColumns;
            }
            if (Column.bGroundRenderGeometryHit)
            {
                ++Report.Provenance.RenderGeometryColumns;
            }
            if (Column.GroundSimpleCollisionShapes >= 0)
            {
                // -1 is "no body setup at all", not "zero simple shapes", so it must not be
                // folded into the range as a 0.
                Report.Provenance.MinSimpleCollisionShapes =
                    (Report.Provenance.MinSimpleCollisionShapes < 0)
                        ? Column.GroundSimpleCollisionShapes
                        : FMath::Min(Report.Provenance.MinSimpleCollisionShapes,
                                     Column.GroundSimpleCollisionShapes);
                Report.Provenance.MaxSimpleCollisionShapes = FMath::Max(
                    Report.Provenance.MaxSimpleCollisionShapes, Column.GroundSimpleCollisionShapes);
            }
            GroundNoteSurfaceComponent(Report.Provenance, Column);

            const double Clearance = Column.Clearance();
            if (bFirstSupported)
            {
                Report.MaxColumnClearanceCm = Clearance;
                Report.MinGapCm = Clearance;
                MinGround = Column.GroundZ;
                MaxGround = Column.GroundZ;
                MinUnderside = Column.UndersideZ;
                MaxUnderside = Column.UndersideZ;
                bFirstSupported = false;
            }
            else
            {
                Report.MaxColumnClearanceCm = FMath::Max(Report.MaxColumnClearanceCm, Clearance);
                Report.MinGapCm = FMath::Min(Report.MinGapCm, Clearance);
                MinGround = FMath::Min(MinGround, Column.GroundZ);
                MaxGround = FMath::Max(MaxGround, Column.GroundZ);
                MinUnderside = FMath::Min(MinUnderside, Column.UndersideZ);
                MaxUnderside = FMath::Max(MaxUnderside, Column.UndersideZ);
            }

            if (Clearance <= Thresholds.ContactToleranceCm)
            {
                ++Report.ContactPoints;
            }
            NormalSum += Column.GroundNormal;
        }

        if (Report.SupportedColumns > 0)
        {
            // The float term, isolated from the shape term: where the actor's lowest point sits
            // relative to the lowest ground it has to fall onto. See FGroundContactReport for
            // why the silhouette max cannot answer this question.
            Report.MaxGapCm = MinUnderside - MinGround;
            Report.UndersideReliefCm = MaxUnderside - MinUnderside;
            Report.PenetrationCm = FMath::Max(0.0, -Report.MinGapCm);
            Report.GroundSpreadCm = MaxGround - MinGround;
            const FVector Averaged = NormalSum.GetSafeNormal();
            Report.AverageNormal = Averaged.IsNearlyZero() ? FVector::UpVector : Averaged;
        }
        else
        {
            // Nothing to bound. Leave every gap number at zero and let Coverage /
            // SupportedColumns carry the failure, rather than emitting a MaxGap of 0 that reads
            // as a perfect result.
            Report.MaxGapCm = 0.0;
            Report.MinGapCm = 0.0;
            Report.MaxColumnClearanceCm = 0.0;
            Report.UndersideReliefCm = 0.0;
        }

        // The surface that answered most of the footprint reads first. StableSort so equal
        // tallies keep the order the grid met them in and two runs over the same world produce
        // the same list - an unstable order here would make a response diff look like a change
        // in what was measured.
        Report.Provenance.SurfaceComponentCount = Report.Provenance.SurfaceComponents.Num();
        Report.Provenance.SurfaceComponents.StableSort(
            [](const FGroundProvenance::FSurfaceComponent& A,
               const FGroundProvenance::FSurfaceComponent& B)
            {
                return A.Columns > B.Columns;
            });
        if (Report.Provenance.SurfaceComponents.Num() > GroundMaxReportedSurfaceComponents)
        {
            Report.Provenance.SurfaceComponents.SetNum(GroundMaxReportedSurfaceComponents);
        }

        Report.Coverage = (Report.ActorColumns > 0)
            ? (static_cast<double>(Report.SupportedColumns) / static_cast<double>(Report.ActorColumns))
            : 0.0;
        Report.bMeasured = true;
    }

    // ---- Instanced-mesh scatter holders ------------------------------------------------

    FInstancedHolderInfo FindInstancedHolder(const AActor* Actor)
    {
        FInstancedHolderInfo Info;
        if (!Actor)
        {
            return Info;
        }

        // UHierarchicalInstancedStaticMeshComponent and UFoliageInstancedStaticMeshComponent both
        // derive from UInstancedStaticMeshComponent, so one query covers every scatter shape.
        // bIncludeFromChildActors is left at its default false to match AActor::GetActorBounds,
        // which is what produced the footprint being refused - a child actor's scatter is that
        // actor's to answer for.
        TArray<UInstancedStaticMeshComponent*> Instanced;
        Actor->GetComponents<UInstancedStaticMeshComponent>(Instanced);
        for (const UInstancedStaticMeshComponent* Component : Instanced)
        {
            if (!Component)
            {
                continue;
            }
            const int32 Count = Component->GetInstanceCount();
            // One instance is an ordinary prop: the component's bounds ARE that instance's bounds
            // and moving the actor moves it, so there is nothing meaningless to refuse. Keeping
            // the largest scatter names the component that actually defines the bounds, which is
            // the one a caller has to go and fix.
            if (Count < 2 || Count <= Info.InstanceCount)
            {
                continue;
            }
            Info.bIsHolder = true;
            Info.ComponentName = Component->GetName();
            Info.ComponentClass = Component->GetClass()->GetName();
            Info.InstanceCount = Count;
        }
        return Info;
    }

    FString DescribeInstancedHolder(const FInstancedHolderInfo& Info)
    {
        if (!Info.bIsHolder)
        {
            return FString();
        }
        return FString::Printf(
            TEXT("This actor is an instanced-mesh scatter holder: component '%s' (%s) carries %d "
                 "instances. Its bounds are the union of all %d, so its footprint is a whole "
                 "scatter and its underside is a surface no single instance has - a verdict "
                 "measured from them describes nothing, and a move derived from them relocates "
                 "every instance at once. Ground the instances individually instead: "
                 "spatial.ground_instances {actorName, component:'%s'} runs this same solve per "
                 "instance and takes apply:false for a dry run; actor.get_instances and "
                 "actor.set_instance_transforms read and write the per-instance transforms "
                 "directly. Refused rather than answered for the holder."),
            *Info.ComponentName, *Info.ComponentClass, Info.InstanceCount, Info.InstanceCount,
            *Info.ComponentName);
    }

    // ---- Pass/fail ---------------------------------------------------------------------

    void EvaluateContact(FGroundContactReport& Report, const FContactThresholds& Thresholds)
    {
        // Reset first, unconditionally. This function is the only writer of these three
        // fields, and it must be safe to call twice (the seat verb re-evaluates after it
        // injects SeatErrorCm), so it can never leave a stale pass behind.
        Report.bPass = false;
        Report.FailReasonCode.Reset();
        Report.FailReason.Reset();

        if (Report.InstancedHolder.bIsHolder)
        {
            // Ahead of bMeasured and of every threshold, because for a scatter holder the numbers
            // are not a borderline answer to be judged - they answer a question the actor does not
            // have. This is the verify half of the refusal; SeatActor refuses the same actor from
            // its own pre-flight, with the same code and the same text.
            Report.FailReasonCode = ErrorCodes::ERR_HOLDER_NOT_SEATABLE;
            Report.FailReason = DescribeInstancedHolder(Report.InstancedHolder);
            return;
        }

        if (!Report.bMeasured)
        {
            Report.FailReasonCode = ErrorCodes::ERR_GROUND_NOT_MEASURED;
            Report.FailReason = TEXT("No ground measurement was taken (no world, no actor, or the "
                                     "actor has no bounds to sample).");
            return;
        }

        if (Report.SupportedColumns == 0)
        {
            if (Report.RejectedSurfaceActorCount > 0)
            {
                Report.FailReasonCode = ErrorCodes::ERR_GROUND_HITS_ALL_REJECTED;
                Report.FailReason = FString::Printf(
                    TEXT("Every hit under this actor was rejected by the surface filter (%d actor(s), "
                         "e.g. '%s'). Nothing under it qualifies as ground."),
                    Report.RejectedSurfaceActorCount,
                    Report.RejectedSurfaceActors.Num() > 0 ? *Report.RejectedSurfaceActors[0] : TEXT("?"));
            }
            else if (Report.bOverLandscape.IsSet() && !Report.bOverLandscape.GetValue())
            {
                Report.FailReasonCode = ErrorCodes::ERR_GROUND_NOT_FOUND;
                Report.FailReason = TEXT("No ground under any part of this actor, and the landscape "
                                         "reports no height at its footprint either - it is off the "
                                         "terrain entirely.");
            }
            else
            {
                Report.FailReasonCode = ErrorCodes::ERR_GROUND_NOT_FOUND;
                Report.FailReason = TEXT("No accepted ground surface under any sampled column of this "
                                         "actor's footprint.");
            }
            return;
        }

        if (Report.Coverage + UE_KINDA_SMALL_NUMBER < Thresholds.MinCoverage)
        {
            Report.FailReasonCode = ErrorCodes::ERR_PARTIAL_GROUND_COVERAGE;
            Report.FailReason = FString::Printf(
                TEXT("Only %.0f%% of this actor's footprint has ground under it (%d of %d columns); "
                     "%.0f%% required. It overhangs a hole or the terrain edge."),
                Report.Coverage * 100.0, Report.SupportedColumns, Report.ActorColumns,
                Thresholds.MinCoverage * 100.0);
            return;
        }

        if (Report.ContactPoints < Thresholds.MinContactPoints)
        {
            if (Report.ContactPoints == 0)
            {
                // No contact at all is not the balanced-boulder case INSUFFICIENT_GROUND_CONTACT
                // describes - there is no contact for it to be insufficient, and telling a caller
                // the actor is "balanced on too few points" would send it to reseat something that
                // is simply hanging in the air. Answered here rather than by falling through to
                // the gap check below, so a caller with bEnforceGapBounds off still cannot score a
                // floating actor as a pass.
                Report.FailReasonCode = ErrorCodes::ERR_ACTOR_NOT_GROUNDED;
                Report.FailReason = FString::Printf(
                    TEXT("This actor touches no ground: its closest point floats %.2f cm above "
                         "the surface."),
                    Report.MinGapCm);
                return;
            }

            Report.FailReasonCode = ErrorCodes::ERR_INSUFFICIENT_GROUND_CONTACT;
            Report.FailReason = FString::Printf(
                TEXT("Only %d of %d supported columns touch the surface (%d required, tolerance "
                     "%.2f cm). The actor is balanced on too few points."),
                Report.ContactPoints, Report.SupportedColumns, Thresholds.MinContactPoints,
                Thresholds.ContactToleranceCm);
            return;
        }

        if (Thresholds.bEnforceGapBounds)
        {
            if (Report.MaxGapCm > Thresholds.MaxGapCm)
            {
                Report.FailReasonCode = ErrorCodes::ERR_ACTOR_NOT_GROUNDED;
                // Names the quantity that was actually compared, and says outright that the
                // silhouette is not it. The old wording ("part of this actor floats 2327.90 cm")
                // was a true statement about a capital doing what a capital does, and it read as
                // a placement bug: it cost one caller four re-seat cycles on correctly seated
                // actors before a control isolated it.
                Report.FailReason = FString::Printf(
                    TEXT("This actor's lowest point sits %.2f cm above the lowest ground under "
                         "its footprint (max allowed %.2f cm), so nothing is holding it up. "
                         "Measured from the actor's own lowest underside sample: geometry that "
                         "overhangs or curves away higher up is shape, not float, and is not "
                         "counted here (its underside varies by %.2f cm, largest single-column "
                         "clearance %.2f cm)."),
                    Report.MaxGapCm, Thresholds.MaxGapCm,
                    Report.UndersideReliefCm, Report.MaxColumnClearanceCm);
                return;
            }
            if (Report.PenetrationCm > Thresholds.MaxPenetrationCm)
            {
                Report.FailReasonCode = ErrorCodes::ERR_ACTOR_BURIED;
                Report.FailReason = FString::Printf(
                    TEXT("This actor is buried %.2f cm into the surface (max allowed %.2f cm)."),
                    Report.PenetrationCm, Thresholds.MaxPenetrationCm);
                return;
            }
        }

        if (Thresholds.MaxSeatErrorCm.IsSet())
        {
            if (!Report.SeatErrorCm.IsSet())
            {
                // Asked for a readback check and did not get one. Refusing to pass is the only
                // honest answer: an unmeasured check is not a passed check.
                Report.FailReasonCode = ErrorCodes::ERR_GROUND_NOT_MEASURED;
                Report.FailReason = TEXT("A seat readback tolerance was requested but no readback "
                                         "was performed.");
                return;
            }
            if (Report.SeatErrorCm.GetValue() > Thresholds.MaxSeatErrorCm.GetValue())
            {
                Report.FailReasonCode = ErrorCodes::ERR_GROUND_SEAT_READBACK_MISMATCH;
                Report.FailReason = FString::Printf(
                    TEXT("The measured result disagrees with the solve by %.2f cm (tolerance %.2f cm): "
                         "the actor did not end up where the solve put it, or the ground answer was "
                         "not reproducible."),
                    Report.SeatErrorCm.GetValue(), Thresholds.MaxSeatErrorCm.GetValue());
                return;
            }
        }

        Report.bPass = true;
    }

    // ---- Provenance serialization ------------------------------------------------------

    // WHICH SURFACE ANSWERED. Every gap number in the contact block is a measurement of one of
    // two different surfaces - the collision hull or the render mesh - and until this block
    // existed the response could not say which one, so a prop seated on a hull face the visible
    // mesh does not have came back as a clean pass with in-family numbers.
    //
    // Costs nothing: the trace already computed all of it (SpatialTraceUtils::FSpatialHit) and
    // the ground probe used to discard it before serialization.
    //
    // Lives here rather than beside one verb's response writer because the same measurement is
    // now published by four verbs across two handler files, and a second copy of this text is
    // how two of them come to describe the same probe differently.
    TSharedPtr<FJsonObject> MakeProvenanceJson(const FGroundContactReport& Report)
    {
        const FGroundProvenance& Prov = Report.Provenance;

        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetBoolField(TEXT("traceComplex"), Prov.bTraceComplex);
        Obj->SetNumberField(TEXT("triangleColumns"), Prov.TriangleColumns);
        Obj->SetNumberField(TEXT("primitiveColumns"), Prov.PrimitiveColumns);
        if (Prov.RenderGeometryColumns > 0)
        {
            Obj->SetNumberField(TEXT("renderGeometryColumns"), Prov.RenderGeometryColumns);
        }
        // The two counts the face-index split cannot express, because both of these answer WITH
        // a face index and so land in triangleColumns beside a clean cliff.
        if (Prov.InstancedScatterColumns > 0)
        {
            Obj->SetNumberField(TEXT("instancedScatterColumns"), Prov.InstancedScatterColumns);
        }
        if (Prov.ComplexAsSimpleColumns > 0)
        {
            Obj->SetNumberField(TEXT("complexAsSimpleColumns"), Prov.ComplexAsSimpleColumns);
        }
        // Supported columns nothing above describes. Present only when there are some, so its
        // absence is "every supported column was accounted for" rather than an unstated zero.
        if (Prov.UnclassifiedColumns > 0)
        {
            Obj->SetNumberField(TEXT("unclassifiedColumns"), Prov.UnclassifiedColumns);
        }
        // -1 means no supported column's component exposed a body setup at all (Landscape).
        // Omitted rather than forged into a 0, which reads as "has a body setup holding no
        // simple shapes" - the opposite claim, and the one renderGeometryHit is built on.
        if (Prov.MinSimpleCollisionShapes >= 0)
        {
            Obj->SetNumberField(TEXT("minSimpleCollisionShapes"), Prov.MinSimpleCollisionShapes);
            Obj->SetNumberField(TEXT("maxSimpleCollisionShapes"), Prov.MaxSimpleCollisionShapes);
        }

        // WHAT the surface was, beside which representation of it answered. The counts above
        // cannot tell a kelp scatter from a cliff; these rows name the primitive, so a caller
        // can see that their "ground" was a HISM rather than stone without re-probing to find
        // out. Omitted entirely when no column's component resolved, rather than emitted empty.
        if (Prov.SurfaceComponents.Num() > 0)
        {
            TArray<TSharedPtr<FJsonValue>> Rows;
            Rows.Reserve(Prov.SurfaceComponents.Num());
            for (const FGroundProvenance::FSurfaceComponent& Entry : Prov.SurfaceComponents)
            {
                TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
                Row->SetStringField(TEXT("actor"), Entry.ActorLabel);
                Row->SetStringField(TEXT("component"), Entry.ComponentName);
                Row->SetStringField(TEXT("componentClass"), Entry.ComponentClass);
                Row->SetNumberField(TEXT("columns"), Entry.Columns);
                // Why THIS surface is or is not one to trust as ground, on the row that names
                // it - so a straddled footprint keeps one verdict per surface instead of one
                // for the whole batch. Written only when true: the row's own existence is the
                // statement that this primitive was resolved and examined, so an absent flag
                // here is "examined, not this" rather than "not looked at".
                if (Entry.bInstancedScatter)
                {
                    Row->SetBoolField(TEXT("instancedScatter"), true);
                }
                if (Entry.bRenderGeometry)
                {
                    Row->SetBoolField(TEXT("renderGeometry"), true);
                }
                if (Entry.bComplexAsSimple)
                {
                    Row->SetBoolField(TEXT("complexAsSimple"), true);
                }
                Rows.Add(MakeShared<FJsonValueObject>(Row));
            }
            Obj->SetArrayField(TEXT("surfaceComponents"), Rows);
            // The true distinct total, beside a possibly-truncated roster - the same shape
            // rejectedSurfaceActorCount carries for rejectedSurfaceActors.
            Obj->SetNumberField(TEXT("surfaceComponentCount"), Prov.SurfaceComponentCount);
        }

        // ---- Is the surface that answered one to TRUST as ground? ----
        //
        // The hull sentence below answers a NARROWER question - which collision representation
        // replied - and it is gated on the ABSENCE of a face index. So every surface that
        // answers WITH triangles sits structurally outside it and cannot raise it, no matter
        // how wrong a ground it is: a complex-traced HISM scatter, a collisionless foliage
        // mesh, a UseComplexAsSimple body. Those are the surfaces a caller most needs told
        // about, and surfaceComponents[] right beside this already names them - so the alarm is
        // derived from the component roster rather than from the cheaper face-index proxy that
        // by construction cannot see them.
        //
        // ADDED to the hull sentence, never substituted for it: that one is correct on the case
        // it was built for. And none of these is a failCode - a HISM of paving stones IS
        // legitimate ground, which is exactly why any_solid admits it, so the caller is the one
        // who decides and this only makes the decision reachable.
        TArray<FString> ScatterNames;
        TArray<FString> RenderNames;
        TArray<FString> ComplexAsSimpleNames;
        for (const FGroundProvenance::FSurfaceComponent& Entry : Prov.SurfaceComponents)
        {
            const FString Named = FString::Printf(TEXT("%s.%s [%s]"),
                *Entry.ActorLabel, *Entry.ComponentName, *Entry.ComponentClass);
            if (Entry.bInstancedScatter) { ScatterNames.Add(Named); }
            if (Entry.bRenderGeometry) { RenderNames.Add(Named); }
            if (Entry.bComplexAsSimple) { ComplexAsSimpleNames.Add(Named); }
        }
        // The roster is capped while the counts are not, so a surface can raise a condition
        // without surviving into the names. Say so rather than let a short list read as the
        // whole set.
        auto GroundNameOrRoster = [](const TArray<FString>& Names) -> FString
        {
            return Names.Num() > 0
                ? FString::Join(Names, TEXT("; "))
                : FString(TEXT("below the surfaceComponents[] cap - re-read the roster"));
        };

        TArray<FString> Warnings;
        // Name the trap in the response itself, the way spatial.raycast names its own. The
        // wording holds under either traceComplex setting, because a primitive can answer a
        // complex query too (a body set to use simple as complex, or a shape component).
        if (Prov.PrimitiveColumns > 0)
        {
            Warnings.Add(FString::Printf(
                TEXT("%d of %d supported column(s) were answered by a SIMPLE collision primitive (no faceIndex), i.e. by a collision hull rather than by the surface a viewer sees. A hull is a different surface from its render mesh wherever the mesh carries subtract/boolean or erosion detail, and wherever a sphere hull is non-uniformly scaled (UE scales FKSphereElem by the MINIMUM absolute scale component), so this height can be one the visible geometry does not have - in either direction, with no safe 'take the higher hit' rule. Re-probe these columns with spatial.raycast at the opposite traceComplex and compare before trusting the numbers. Absence of a faceIndex is a strong prior, not proof: the physics backend can also report none."),
                Prov.PrimitiveColumns, Report.SupportedColumns));
        }
        if (Prov.InstancedScatterColumns > 0)
        {
            Warnings.Add(FString::Printf(
                TEXT("%d of %d supported column(s) were answered by an INSTANCED-MESH scatter (%s). Not a failure: any_solid admits a scatter as ground deliberately, because a HISM/ISM of paving stones, rocks or modular tiles IS ground and a preset excluding the generic instanced classes would move every actor already seated on one. But a scatter of foliage or debris is not a floor, per-instance heights under one footprint need not agree, and none of the counts above can tell you a scatter replied - under a complex trace it answers with triangles exactly like a cliff. State the call: put the componentClass in surface.excludeComponentClasses to peel it off, or accept it deliberately."),
                Prov.InstancedScatterColumns, Report.SupportedColumns,
                *GroundNameOrRoster(ScatterNames)));
        }
        if (Prov.RenderGeometryColumns > 0)
        {
            Warnings.Add(FString::Printf(
                TEXT("%d of %d supported column(s) were answered by RENDER triangles, because the struck component carries no simple collision at all (%s). Only a complex probe reaches those triangles, so this height comes from geometry that blocks nothing at traceComplex:false - a collisionless foliage mesh standing in for the ground beneath it. Re-probe at traceComplex:false, or peel the component off with surface.excludeComponentClasses, before trusting the numbers."),
                Prov.RenderGeometryColumns, Report.SupportedColumns,
                *GroundNameOrRoster(RenderNames)));
        }
        // Only a probe that did NOT ask for complex tracing is a disagreement. Under
        // traceComplex:true, per-triangle geometry answering is exactly what was requested and
        // there is nothing to warn about.
        if (Prov.ComplexAsSimpleColumns > 0 && !Prov.bTraceComplex)
        {
            Warnings.Add(FString::Printf(
                TEXT("%d of %d supported column(s) were answered by a body set to UseComplexAsSimple (%s), so PER-TRIANGLE geometry replied to a traceComplex:false probe. traceComplex above echoes what was REQUESTED, not what answered, and triangleColumns here is therefore not evidence that the simple hull was read. Check the mesh's collision complexity (static_mesh.set_collision_complexity writes it) before reasoning about which surface these numbers describe."),
                Prov.ComplexAsSimpleColumns, Report.SupportedColumns,
                *GroundNameOrRoster(ComplexAsSimpleNames)));
        }

        // The affirmative statement, and the reason this is more than another count: an absent
        // warning used to mean BOTH "the surface checked out" and "nothing about the surface was
        // checkable", and a caller cannot tell those apart from one silence. Published on every
        // measured report so the distinction is readable rather than inferred:
        //   untrusted    - a condition above fired; warning says which, and names the surface.
        //   undetermined - nothing fired, but some supported column's primitive never resolved,
        //                  so there was no surface to check. warning is OMITTED rather than
        //                  written as an all-clear over columns nobody classified.
        //   trusted      - every supported column named a surface and none of them tripped.
        const TCHAR* SurfaceTrust = TEXT("trusted");
        if (Warnings.Num() > 0)
        {
            SurfaceTrust = TEXT("untrusted");
        }
        else if (Prov.UnclassifiedColumns > 0)
        {
            SurfaceTrust = TEXT("undetermined");
        }
        Obj->SetStringField(TEXT("surfaceTrust"), SurfaceTrust);

        // One field, because warning is the field a caller stops at. A second key beside it
        // would have left the new conditions exactly as unread as no condition at all.
        if (Warnings.Num() > 0)
        {
            Obj->SetStringField(TEXT("warning"), FString::Join(Warnings, TEXT(" ")));
        }
        return Obj;
    }

    // ---- Measurement -------------------------------------------------------------------

    FGroundContactReport MeasureContactForBounds(UWorld* World, const FBox& WorldBounds,
                                                 AActor* UndersideGeometry,
                                                 const TArray<AActor*>& ExtraIgnoreActors,
                                                 const FGroundSurfaceSpec& Surface,
                                                 int32 GridSize, double FootprintInset,
                                                 const TOptional<FVector2D>& ContactHalfExtentCm,
                                                 EUndersideModel UndersideModel,
                                                 const FContactThresholds& Thresholds,
                                                 TArray<FGroundColumn>* OutColumns)
    {
        FGroundContactReport Report;
        Report.UndersideModel = UndersideModel;
        if (OutColumns)
        {
            OutColumns->Reset();
        }

        if (!World || !WorldBounds.IsValid)
        {
            EvaluateContact(Report, Thresholds);
            return Report;
        }

        const FVector Origin = WorldBounds.GetCenter();
        const FVector Extent = WorldBounds.GetExtent();
        if (Extent.X < GroundMinExtentCm && Extent.Y < GroundMinExtentCm && Extent.Z < GroundMinExtentCm)
        {
            // No footprint at all. bMeasured stays false, so EvaluateContact reports
            // GROUND_NOT_MEASURED rather than a coverage figure computed from nothing.
            EvaluateContact(Report, Thresholds);
            return Report;
        }

        const int32 Grid = FMath::Clamp(GridSize, MinGridSize, MaxGridSize);
        const double Inset = FMath::Clamp(FootprintInset, 0.0, 0.45);

        // The XY span of the sample grid, and the ONE place either source is chosen. The default
        // is the sampled box itself pulled in by the inset; a stated contact half-extent replaces
        // it outright, because for anything whose contact patch is not its silhouette the box is
        // the wrong quantity to take a fraction of - a trunked tree's AABB is its canopy, 46.7x
        // the contact area, and the inset is clamped at 0.45 besides.
        //
        // Clamped to the bounds: a contact extent wider than the box would put columns where no
        // underside model can answer, which is the failure the inset itself exists to avoid.
        // TopZ/BottomZ stay the real bounds - the underside plane and the probe span are
        // properties of the whole object, not of its contact patch.
        const bool bContactFootprint = ContactHalfExtentCm.IsSet()
            && ContactHalfExtentCm->X > 0.0 && ContactHalfExtentCm->Y > 0.0;
        const double HalfX = bContactFootprint
            ? FMath::Min(ContactHalfExtentCm->X, Extent.X)
            : Extent.X * (1.0 - Inset);
        const double HalfY = bContactFootprint
            ? FMath::Min(ContactHalfExtentCm->Y, Extent.Y)
            : Extent.Y * (1.0 - Inset);
        const double TopZ = Origin.Z + Extent.Z;
        const double BottomZ = Origin.Z - Extent.Z;

        // Published before anything is probed, so the box every number below describes is named
        // by the same code that chose it rather than reconstructed by a reader from the inset.
        Report.FootprintHalfExtentCm = FVector2D(HalfX, HalfY);
        Report.bFootprintFromContact = bContactFootprint;

        // The ground probe starts ABOVE the actor, not at its underside. An actor buried in
        // geometry has its ground above its own bottom face, and a probe that starts at the
        // bottom face can never see it - that is how rocks embedded in cliff walls looked
        // correctly placed right up until the cliffs were flattened.
        const double ProbeStartZ = TopZ + FMath::Max(Surface.ProbeLiftCm, 0.0);
        const double ProbeEndZ = BottomZ - FMath::Max(Surface.MaxDropCm, 0.0);

        TArray<AActor*> Ignore;
        for (const TWeakObjectPtr<AActor>& Weak : Surface.IgnoreActors)
        {
            if (AActor* Ignored = Weak.Get())
            {
                Ignore.AddUnique(Ignored);
            }
        }
        // Whatever is being measured is ignored on top of the surface spec's own list. Nothing is
        // added implicitly here - this function does not know what the bounds belong to - so the
        // entry points state it: MeasureContact passes the measured actor, an instance caller
        // passes its holder.
        for (AActor* Extra : ExtraIgnoreActors)
        {
            if (Extra)
            {
                Ignore.AddUnique(Extra);
            }
        }

        SpatialTraceUtils::FSpatialLayeredTraceOptions TraceOptions;
        TraceOptions.MaxLayers = FMath::Clamp(Surface.MaxLayers, 1, SpatialTraceUtils::MaxAllowedLayers);
        TraceOptions.MaxAcceptedHits = 1;
        TraceOptions.Filter = Surface.Filter;

        TArray<FGroundColumn> Columns;
        Columns.Reserve(Grid * Grid);

        for (int32 IX = 0; IX < Grid; ++IX)
        {
            // A 1x1 grid samples the centre; larger grids span [-half, +half] inclusive.
            const double FracX = (Grid == 1) ? 0.5 : (static_cast<double>(IX) / static_cast<double>(Grid - 1));
            const double X = Origin.X - HalfX + 2.0 * HalfX * FracX;
            for (int32 IY = 0; IY < Grid; ++IY)
            {
                const double FracY = (Grid == 1) ? 0.5 : (static_cast<double>(IY) / static_cast<double>(Grid - 1));
                const double Y = Origin.Y - HalfY + 2.0 * HalfY * FracY;

                FGroundColumn Column;
                Column.X = X;
                Column.Y = Y;

                if (UndersideModel == EUndersideModel::MeshProfile)
                {
                    // Null UndersideGeometry answers false in every column, which lands on the
                    // bounds-plane fallback below - the instance path's normal outcome, reported
                    // rather than hidden.
                    Column.bHasActorGeometry =
                        GroundProbeUndersideZ(UndersideGeometry, X, Y, TopZ, BottomZ, Column.UndersideZ);
                }
                else
                {
                    Column.bHasActorGeometry = true;
                    Column.UndersideZ = BottomZ;
                }

                const SpatialTraceUtils::FSpatialLayeredTraceResult Traced =
                    SpatialTraceUtils::TraceLineLayered(World,
                        FVector(X, Y, ProbeStartZ), FVector(X, Y, ProbeEndZ),
                        Surface.Channel, Surface.bTraceComplex, Ignore, TraceOptions);

                if (Traced.Hits.Num() > 0)
                {
                    const SpatialTraceUtils::FSpatialHit& Hit = Traced.Hits[0];
                    Column.bHasGround = true;
                    Column.GroundZ = Hit.Location.Z;
                    Column.GroundNormal = Hit.Normal.GetSafeNormal();
                    if (Column.GroundNormal.IsNearlyZero())
                    {
                        Column.GroundNormal = FVector::UpVector;
                    }
                    Column.GroundActor = Hit.HitActor;
                    // The provenance the trace already computed. Reading Z, normal and actor off
                    // this hit and dropping the rest is what made a hull measurement
                    // indistinguishable from a render-surface one in the response.
                    Column.GroundComponent = Hit.HitComponent;
                    Column.GroundFaceIndex = Hit.FaceIndex;
                    Column.GroundSimpleCollisionShapes = Hit.SimpleCollisionShapes;
                    Column.bGroundRenderGeometryHit = Hit.bRenderGeometryHit;
                }

                // Actors the filter peeled are named once each, and - because a rejection in one
                // column is a rejection in every column - are added to the engine-level ignore
                // list so later columns do not pay to peel them again.
                for (const TWeakObjectPtr<AActor>& Weak : Traced.RejectedActors)
                {
                    if (AActor* Rejected = Weak.Get())
                    {
                        // Ignore doubles as the seen-set, keyed on the ACTOR rather than its
                        // display label: labels are not unique, and deduping by label would
                        // undercount two differently-placed fog cards that share one.
                        if (!Ignore.Contains(Rejected))
                        {
                            ++Report.RejectedSurfaceActorCount;
                            if (Report.RejectedSurfaceActors.Num() < GroundMaxReportedRejects)
                            {
                                Report.RejectedSurfaceActors.Add(Rejected->GetActorLabel());
                            }
                            Ignore.Add(Rejected);
                        }
                    }
                }

                Columns.Add(Column);
            }
        }

        // Mesh-profile fallback: the underside geometry answered no geometry query anywhere
        // (collision disabled, no body instance, nothing cooked, or - for an instance - no actor
        // whose own components could be asked). Fall back to the flat AABB plane and
        // SAY SO - the numbers below mean something different under that model, and a caller
        // that cannot tell the two apart is back to trusting an unverifiable result.
        int32 GeometryColumns = 0;
        for (const FGroundColumn& Column : Columns)
        {
            GeometryColumns += Column.bHasActorGeometry ? 1 : 0;
        }
        if (UndersideModel == EUndersideModel::MeshProfile && GeometryColumns == 0)
        {
            Report.bUsedBoundsPlaneFallback = true;
            for (FGroundColumn& Column : Columns)
            {
                Column.bHasActorGeometry = true;
                Column.UndersideZ = BottomZ;
            }
        }

        // ---- Aggregate ----
        // Set here rather than in AggregateColumns: it is a property of the PROBE, which only
        // this function issued, and AggregateColumns must stay callable on constructed columns.
        Report.Provenance.bTraceComplex = Surface.bTraceComplex;
        AggregateColumns(Columns, Thresholds, Report);

        if (Report.SupportedColumns == 0)
        {
            // Only now is the landscape lookup worth its cost, and only it can distinguish
            // "something blocked the probe" from "this actor is not over terrain at all". It
            // runs before EvaluateContact because EvaluateContact reads its answer.
            const TOptional<double> LandscapeZ = ProbeLandscapeHeight(World, Origin.X, Origin.Y);
            Report.bOverLandscape = LandscapeZ.IsSet();
        }

        if (OutColumns)
        {
            *OutColumns = MoveTemp(Columns);
        }

        EvaluateContact(Report, Thresholds);
        return Report;
    }

    FGroundContactReport MeasureContact(UWorld* World, AActor* Actor,
                                        const FGroundSurfaceSpec& Surface,
                                        int32 GridSize, double FootprintInset,
                                        EUndersideModel UndersideModel,
                                        const FContactThresholds& Thresholds,
                                        TArray<FGroundColumn>* OutColumns)
    {
        if (!World || !Actor)
        {
            FGroundContactReport Report;
            Report.UndersideModel = UndersideModel;
            if (OutColumns)
            {
                OutColumns->Reset();
            }
            EvaluateContact(Report, Thresholds);
            return Report;
        }

        FVector Origin = FVector::ZeroVector;
        FVector Extent = FVector::ZeroVector;
        // bOnlyCollidingComponents=false, i.e. INCLUDE non-colliding components: the footprint
        // that must not float is the one you can see, not the one the physics scene knows about.
        // Same call and same argument as actor.get_bounding_box / spatial.measure_* /
        // PlacementHandler.cpp:222, so every spatial number in this plugin agrees.
        Actor->GetActorBounds(false, Origin, Extent);

        // The measured actor is both the underside-geometry source and the one actor the ground
        // probe must never see. A caller cannot forget the second, which is how a ground probe
        // ends up reporting the actor's own height.
        // No contact half-extent: an ACTOR has neither one mesh nor one scale for a mesh-local
        // radius to be defined against, so the actor entry point samples its bounds and says so
        // on the report. spatial.ground_instances is where the override is expressible.
        FGroundContactReport Report = MeasureContactForBounds(World,
            FBox(Origin - Extent, Origin + Extent), Actor, TArray<AActor*>{Actor}, Surface,
            GridSize, FootprintInset, TOptional<FVector2D>(), UndersideModel, Thresholds,
            OutColumns);

        // Recorded after the sampling, because it decides whether the samples MEAN anything, not
        // whether they are taken: callers that read the raw numbers rather than the verdict
        // (level.audit) keep getting them. Re-evaluated only for a holder, and safely so -
        // EvaluateContact resets its own three outputs first, so the second call replaces the
        // verdict with the refusal rather than layering on top of it.
        Report.InstancedHolder = FindInstancedHolder(Actor);
        if (Report.InstancedHolder.bIsHolder)
        {
            EvaluateContact(Report, Thresholds);
        }
        return Report;
    }

    // ---- Seat --------------------------------------------------------------------------

    FGroundSeatResult SeatActor(UWorld* World, AActor* Actor,
                                const FGroundSurfaceSpec& Surface,
                                const FGroundSeatConfig& Config)
    {
        FGroundSeatResult Result;

        if (!Actor)
        {
            GroundFailResult(Result, EGroundSeatStatus::ActorNotFound,
                ErrorCodes::ERR_ACTOR_NOT_FOUND, TEXT("Actor is null."));
            return Result;
        }

        Result.ActorLabel = Actor->GetActorLabel();
        Result.ActorPath = Actor->GetPathName();

        if (!World)
        {
            GroundFailResult(Result, EGroundSeatStatus::NotAttempted,
                ErrorCodes::ERR_EDITOR_WORLD_NOT_AVAILABLE, TEXT("No world to trace against."));
            return Result;
        }

        if (Actor->IsLockLocation())
        {
            // AActor::IsLockLocation (UE 5.8 GameFramework/Actor.h:1189). Moving a locked actor
            // would succeed silently at the C++ level and be a surprise in the editor.
            GroundFailResult(Result, EGroundSeatStatus::ActorLocationLocked,
                ErrorCodes::ERR_ACTOR_LOCATION_LOCKED,
                TEXT("This actor's location is locked in the editor; it was not moved."));
            return Result;
        }

        const FInstancedHolderInfo Holder = FindInstancedHolder(Actor);
        if (Holder.bIsHolder)
        {
            // Refused BEFORE the pre-move probe and before anything moves. Seating this actor
            // would solve against the union of every instance and then relocate all of them with
            // one SetActorTransform - a move that succeeds, reports placed, and is wrong for every
            // instance. This is the seat half of the refusal; MeasureContact's report carries the
            // same code and the same text, so the verify verb cannot decline to judge what this
            // one would move, or the other way round.
            GroundFailResult(Result, EGroundSeatStatus::HolderNotSeatable,
                ErrorCodes::ERR_HOLDER_NOT_SEATABLE, DescribeInstancedHolder(Holder));
            return Result;
        }

        FVector Origin = FVector::ZeroVector;
        FVector Extent = FVector::ZeroVector;
        Actor->GetActorBounds(false, Origin, Extent);
        if (Extent.X < GroundMinExtentCm && Extent.Y < GroundMinExtentCm && Extent.Z < GroundMinExtentCm)
        {
            GroundFailResult(Result, EGroundSeatStatus::ActorHasNoBounds,
                ErrorCodes::ERR_ACTOR_HAS_NO_BOUNDS,
                TEXT("This actor has no measurable bounds (no primitive components), so there is "
                     "no footprint to seat."));
            return Result;
        }

        // Thresholds for the PRE-move probe: absolute gap bounds are meaningless before the
        // solve has run, so only coverage / ground presence gate it.
        FContactThresholds PreThresholds = Config.Thresholds;
        PreThresholds.bEnforceGapBounds = false;
        PreThresholds.MaxSeatErrorCm.Reset();
        PreThresholds.MinContactPoints = 0;

        TArray<FGroundColumn> PreColumns;
        FGroundContactReport Pre = MeasureContact(World, Actor, Surface, Config.GridSize,
            Config.FootprintInset, Config.UndersideModel, PreThresholds, &PreColumns);

        Result.Contact = Pre;
        if (!Pre.bPass)
        {
            // Map the measurement's own reason onto a seat status. The reason text and code
            // come from EvaluateContact, so the two verbs cannot describe the same condition
            // differently.
            GroundFailResult(Result, GroundStatusForMeasurementFailure(Pre.FailReasonCode),
                *Pre.FailReasonCode, Pre.FailReason);
            return Result;
        }

        Result.PreviousTransform = Actor->GetActorTransform();

        // ---- Rotation, if asked for ----
        // Applied BEFORE the seat solve and BEFORE the columns are re-sampled, so the solve
        // measures the footprint the actor will actually have. Computing the rotated footprint
        // analytically is possible but re-reading the engine's own bounds after the rotation is
        // both simpler and immune to getting the transform algebra wrong.
        bool bRotated = false;
        if (Config.bAlignToSurface)
        {
            FVector Target = Pre.AverageNormal.GetSafeNormal();
            if (Target.Size() > GroundMinNormalLength)
            {
                const double MaxTiltRad = FMath::DegreesToRadians(
                    FMath::Clamp(Config.MaxTiltDegrees, 0.0, 89.0));
                const double CosAngle = FMath::Clamp(
                    static_cast<double>(FVector::DotProduct(FVector::UpVector, Target)), -1.0, 1.0);
                const double Angle = FMath::Acos(CosAngle);
                if (Angle > MaxTiltRad)
                {
                    // Clamp the tilt by rotating world-up toward the normal by only the allowed
                    // angle, about the axis the two span. A cliff-face normal then produces a
                    // lean, not a prop lying on its side.
                    const FVector Axis = FVector::CrossProduct(FVector::UpVector, Target).GetSafeNormal();
                    Target = Axis.IsNearlyZero()
                        ? FVector::UpVector
                        : FQuat(Axis, MaxTiltRad).RotateVector(FVector::UpVector);
                }
                if (Angle > UE_KINDA_SMALL_NUMBER)
                {
                    const FQuat Delta = FQuat::FindBetweenNormals(FVector::UpVector, Target);
                    Actor->Modify();
                    Actor->SetActorRotation(Delta * Actor->GetActorQuat(), ETeleportType::TeleportPhysics);
                    bRotated = true;
                }
            }
        }

        // ---- Solve ----
        TArray<FGroundColumn> SolveColumns;
        FGroundContactReport Solve = Pre;
        if (bRotated)
        {
            Solve = MeasureContact(World, Actor, Surface, Config.GridSize, Config.FootprintInset,
                Config.UndersideModel, PreThresholds, &SolveColumns);
            Actor->GetActorBounds(false, Origin, Extent);
        }
        else
        {
            SolveColumns = PreColumns;
        }

        TArray<double> Clearances;
        Clearances.Reserve(SolveColumns.Num());
        for (const FGroundColumn& Column : SolveColumns)
        {
            if (Column.IsSupported())
            {
                Clearances.Add(Column.Clearance());
            }
        }
        if (Clearances.Num() == 0)
        {
            // Only reachable when the rotation changed the footprint out from under the ground.
            // The actor HAS been rotated at this point, so the rotation is undone before
            // reporting failure - a half-applied transform is not a state to hand back.
            if (bRotated && Result.PreviousTransform.IsSet())
            {
                Actor->SetActorTransform(Result.PreviousTransform.GetValue(), false, nullptr,
                    ETeleportType::TeleportPhysics);
                Actor->MarkComponentsRenderStateDirty();
            }
            Result.Contact = Solve;
            GroundFailResult(Result, EGroundSeatStatus::NoGroundFound,
                ErrorCodes::ERR_GROUND_NOT_FOUND,
                TEXT("After aligning to the surface normal, no sampled column had ground under it."));
            return Result;
        }

        Clearances.Sort();
        const double Percentile = FMath::Clamp(Config.SeatPercentile, 0.0, 1.0);
        // RoundToInt32, not RoundToInt: FMath::RoundToInt(double) returns int64 in UE 5.8
        // (GenericPlatform/GenericPlatformMath.h:325), which would not deduce against the int32
        // bounds of FMath::Clamp.
        const int32 SeatIndex = FMath::Clamp(
            FMath::RoundToInt32(Percentile * static_cast<double>(Clearances.Num() - 1)),
            0, Clearances.Num() - 1);
        const double SeatClearance = Clearances[SeatIndex];

        const double EmbedCm = Config.ResolveEmbedCm(2.0 * Extent.Z);
        // Move down by the seated column's clearance, then a further EmbedCm so the actor beds
        // into the ground rather than resting tangent to it. After the move the seated column's
        // clearance is exactly -EmbedCm; that identity is what the readback below checks.
        const double DeltaZ = -(SeatClearance + EmbedCm);

        Actor->Modify();
        const FVector NewLocation = Actor->GetActorLocation() + FVector(0.0, 0.0, DeltaZ);
        Actor->SetActorLocation(NewLocation, false, nullptr, ETeleportType::TeleportPhysics);
        Actor->MarkComponentsRenderStateDirty();
        Actor->MarkPackageDirty();

        // The one place AppliedTransform is set, immediately after the only place the actor is
        // moved. FGroundSeatResult::WasMoved()/IsSeated() read this, so "moved" cannot be
        // claimed by a path that did not move anything.
        Result.AppliedTransform = Actor->GetActorTransform();
        Result.AppliedEmbedCm = EmbedCm;
        Result.AppliedDeltaZCm = DeltaZ;

        // ---- Readback ----
        FContactThresholds PostThresholds = Config.Thresholds;
        // Absolute gap bounds are the wrong question for a seat that deliberately rests on the
        // first contact point: the far side of a footprint on a slope is legitimately above the
        // surface. The readback identity below is the check that means something here.
        PostThresholds.bEnforceGapBounds = false;
        PostThresholds.MaxSeatErrorCm = FMath::Max(Config.MaxSeatErrorCm, 0.0);

        TArray<FGroundColumn> PostColumns;
        FGroundContactReport Post = MeasureContact(World, Actor, Surface, Config.GridSize,
            Config.FootprintInset, Config.UndersideModel, PostThresholds, &PostColumns);

        // Worst per-column disagreement between "where the solve said each column would end up"
        // and where it actually is. The grids are identical in size and ordering and the move
        // was Z-only, so column i corresponds to column i.
        if (PostColumns.Num() == SolveColumns.Num())
        {
            double WorstError = 0.0;
            int32 Compared = 0;
            for (int32 Index = 0; Index < PostColumns.Num(); ++Index)
            {
                if (!PostColumns[Index].IsSupported() || !SolveColumns[Index].IsSupported())
                {
                    continue;
                }
                const double Predicted = SolveColumns[Index].Clearance() + DeltaZ;
                WorstError = FMath::Max(WorstError,
                    FMath::Abs(PostColumns[Index].Clearance() - Predicted));
                ++Compared;
            }
            if (Compared > 0)
            {
                Post.SeatErrorCm = WorstError;
            }
        }
        // Re-evaluate with SeatErrorCm now present. EvaluateContact resets its outputs first,
        // so this replaces the verdict rather than layering on top of it.
        EvaluateContact(Post, PostThresholds);
        Result.Contact = Post;

        if (Post.bPass)
        {
            Result.Status = EGroundSeatStatus::Seated;
            return Result;
        }

        if (Config.bRevertOnFailure && Result.PreviousTransform.IsSet())
        {
            Actor->Modify();
            Actor->SetActorTransform(Result.PreviousTransform.GetValue(), false, nullptr,
                ETeleportType::TeleportPhysics);
            Actor->MarkComponentsRenderStateDirty();
            // Clearing AppliedTransform is what makes WasMoved() false again. The actor is back
            // where it started, so reporting it as moved would be a lie in the other direction.
            Result.AppliedTransform.Reset();
            Result.AppliedDeltaZCm = 0.0;
            GroundFailResult(Result, EGroundSeatStatus::Reverted, *Post.FailReasonCode,
                FString::Printf(TEXT("%s Reverted to the original transform."), *Post.FailReason));
            return Result;
        }

        GroundFailResult(Result, EGroundSeatStatus::VerificationFailed, *Post.FailReasonCode,
            Post.FailReason);
        return Result;
    }

    // ---- Seat one instance -------------------------------------------------------------

    FGroundInstanceSeatResult SeatInstance(UWorld* World, UInstancedStaticMeshComponent* Component,
                                           int32 InstanceIndex, const FGroundSurfaceSpec& Surface,
                                           const FGroundSeatConfig& Config, bool bApply)
    {
        FGroundInstanceSeatResult Result;
        Result.InstanceIndex = InstanceIndex;

        if (!World || !Component)
        {
            GroundFailResult(Result.Seat, EGroundSeatStatus::NotAttempted,
                ErrorCodes::ERR_EDITOR_WORLD_NOT_AVAILABLE,
                TEXT("No world or no instanced component to seat against."));
            return Result;
        }

        AActor* Holder = Component->GetOwner();
        Result.Seat.ActorLabel = Holder ? Holder->GetActorLabel() : FString();
        // The COMPONENT's path, not the actor's: an actor may carry several scatters and the
        // instance index is meaningless without knowing which one it indexes.
        Result.Seat.ActorPath = Component->GetPathName();

        FTransform InstanceWorld;
        if (!Component->GetInstanceTransform(InstanceIndex, InstanceWorld, /*bWorldSpace*/ true))
        {
            GroundFailResult(Result.Seat, EGroundSeatStatus::NotAttempted,
                ErrorCodes::ERR_INSTANCE_INDEX_OUT_OF_RANGE,
                FString::Printf(TEXT("Instance %d does not exist on '%s'; it carries %d instance(s), "
                                     "so valid indices are 0..%d."),
                    InstanceIndex, *Component->GetName(), Component->GetInstanceCount(),
                    Component->GetInstanceCount() - 1));
            return Result;
        }
        Result.CurrentTransform = InstanceWorld;

        const UStaticMesh* Mesh = Component->GetStaticMesh();
        if (!Mesh)
        {
            GroundFailResult(Result.Seat, EGroundSeatStatus::ActorHasNoBounds,
                ErrorCodes::ERR_ACTOR_HAS_NO_BOUNDS,
                TEXT("The instanced component holds no static mesh, so its instances have no "
                     "footprint to seat."));
            return Result;
        }

        // The instance's footprint: the mesh's own bounds under the instance's world transform,
        // which carries the instance's rotation and scale. This is the box the holder's
        // GetActorBounds could never produce - that one is the union of every instance.
        const FBox InstanceBounds = Mesh->GetBounds().GetBox().TransformBy(InstanceWorld);

        // ...and the box the sample grid spans, which is NOT the same question. For a mesh whose
        // contact patch is its silhouette - a rock, a slab, a log - the two coincide and the
        // bounds are right. For a trunked tree they differ by 46.7x in area: the AABB is the
        // canopy, so a grid spanning it probes ground the trunk never touches and the solve lifts
        // an already-planted tree to clear it, reporting coverage 1.00 and pass true about the
        // box the whole time.
        //
        // ContactRadiusCm is stated in MESH-LOCAL cm and scaled HERE by the instance's own mean XY
        // scale, so one number covers a scatter whose instances differ in size and yaw. That is
        // the reason it is not a bounds ratio: a ratio is re-derived from the world AABB per
        // instance, and FBox::TransformBy returns the axis-aligned hull of the ROTATED box, so
        // instance yaw alone moves it.
        const FVector InstanceScale = InstanceWorld.GetScale3D();
        const double MeanScaleXY =
            0.5 * (FMath::Abs(InstanceScale.X) + FMath::Abs(InstanceScale.Y));
        TOptional<FVector2D> ContactHalfExtent;
        if (Config.ContactRadiusCm > 0.0 && MeanScaleXY > 0.0)
        {
            const double ScaledRadius = Config.ContactRadiusCm * MeanScaleXY;
            ContactHalfExtent = FVector2D(ScaledRadius, ScaledRadius);
        }

        // The holder is the one actor the ground probe must not see. That necessarily hides every
        // SIBLING instance too - they share this actor and a world trace cannot separate them - so
        // an instance is never seated onto another instance of the same scatter.
        TArray<AActor*> Ignore;
        if (Holder)
        {
            Ignore.Add(Holder);
        }

        // Identical to SeatActor's pre-move thresholds, and deliberately so: absolute gap bounds
        // are meaningless before the solve has run, and a dry run that used different thresholds
        // would predict something other than what apply does.
        FContactThresholds PreThresholds = Config.Thresholds;
        PreThresholds.bEnforceGapBounds = false;
        PreThresholds.MaxSeatErrorCm.Reset();
        PreThresholds.MinContactPoints = 0;

        TArray<FGroundColumn> SolveColumns;
        FGroundContactReport Pre = MeasureContactForBounds(World, InstanceBounds, nullptr, Ignore,
            Surface, Config.GridSize, Config.FootprintInset, ContactHalfExtent,
            EUndersideModel::BoundsPlane, PreThresholds, &SolveColumns);

        Result.Seat.Contact = Pre;
        if (!Pre.bPass)
        {
            GroundFailResult(Result.Seat, GroundStatusForMeasurementFailure(Pre.FailReasonCode),
                *Pre.FailReasonCode, Pre.FailReason);
            return Result;
        }

        TArray<double> Clearances;
        Clearances.Reserve(SolveColumns.Num());
        for (const FGroundColumn& Column : SolveColumns)
        {
            if (Column.IsSupported())
            {
                Clearances.Add(Column.Clearance());
            }
        }
        if (Clearances.Num() == 0)
        {
            // Pre.bPass implies at least one supported column, so this is unreachable through
            // EvaluateContact - but the array is indexed below and a crash in the editor is not
            // an acceptable way to learn that the invariant moved.
            GroundFailResult(Result.Seat, EGroundSeatStatus::NoGroundFound,
                ErrorCodes::ERR_GROUND_NOT_FOUND,
                TEXT("No sampled column under this instance had ground."));
            return Result;
        }

        Clearances.Sort();
        const double Percentile = FMath::Clamp(Config.SeatPercentile, 0.0, 1.0);
        const int32 SeatIndex = FMath::Clamp(
            FMath::RoundToInt32(Percentile * static_cast<double>(Clearances.Num() - 1)),
            0, Clearances.Num() - 1);
        const double SeatClearance = Clearances[SeatIndex];

        const double EmbedCm = Config.ResolveEmbedCm(2.0 * InstanceBounds.GetExtent().Z);
        const double DeltaZ = -(SeatClearance + EmbedCm);

        FTransform Proposed = InstanceWorld;
        Proposed.AddToTranslation(FVector(0.0, 0.0, DeltaZ));

        if (!bApply)
        {
            // Nothing was written, so nothing is claimed: AppliedTransform stays unset,
            // Seat.WasMoved() stays false, and no undo record is produced because there is
            // nothing to undo.
            Result.ProposedTransform = Proposed;
            Result.ProposedDeltaZCm = DeltaZ;
            Result.Seat.AppliedEmbedCm = EmbedCm;
            Result.Seat.Status = EGroundSeatStatus::DryRun;
            return Result;
        }

        Result.Seat.PreviousTransform = InstanceWorld;

        // The ONLY write. UpdateInstanceTransform is what does Modify(), InvalidateCachedBounds(),
        // the store, PrimitiveInstanceDataManager::TransformChanged, the instance body update and
        // the navigation update (InstancedStaticMesh.cpp:4310-4364). A reflection store into
        // PerInstanceSMData does none of it, reports success, and reads back the new matrix while
        // the world keeps the old one.
        //
        // bMarkRenderStateDirty=false: a batch dirties ONCE afterwards, and a HISM additionally
        // needs its cluster tree rebuilt - see InstancedMeshUtils::FinishInstanceWrites.
        // bTeleport=true so a physics body is moved rather than swept to the new pose.
        if (!Component->UpdateInstanceTransform(InstanceIndex, Proposed, /*bWorldSpace*/ true,
                /*bMarkRenderStateDirty*/ false, /*bTeleport*/ true))
        {
            Result.Seat.PreviousTransform.Reset();
            GroundFailResult(Result.Seat, EGroundSeatStatus::NotAttempted,
                ErrorCodes::ERR_INSTANCE_INDEX_OUT_OF_RANGE,
                FString::Printf(TEXT("UpdateInstanceTransform refused instance %d on '%s'."),
                    InstanceIndex, *Component->GetName()));
            return Result;
        }

        // Read back from the component rather than echoing Proposed: the stored transform is what
        // the world holds, and a write that landed somewhere else must show as such.
        FTransform AppliedWorld = Proposed;
        Component->GetInstanceTransform(InstanceIndex, AppliedWorld, /*bWorldSpace*/ true);
        Result.Seat.AppliedTransform = AppliedWorld;
        Result.Seat.AppliedEmbedCm = EmbedCm;
        Result.Seat.AppliedDeltaZCm = DeltaZ;

        FContactThresholds PostThresholds = Config.Thresholds;
        PostThresholds.bEnforceGapBounds = false;
        PostThresholds.MaxSeatErrorCm = FMath::Max(Config.MaxSeatErrorCm, 0.0);

        TArray<FGroundColumn> PostColumns;
        // Same footprint the solve used - the move was Z-only, so the contact half-extent is
        // unchanged. A readback taken over a different box would compare two different questions
        // and the SeatErrorCm below would be meaningless.
        FGroundContactReport Post = MeasureContactForBounds(World,
            Mesh->GetBounds().GetBox().TransformBy(AppliedWorld), nullptr, Ignore, Surface,
            Config.GridSize, Config.FootprintInset, ContactHalfExtent,
            EUndersideModel::BoundsPlane, PostThresholds, &PostColumns);

        // The move was Z-only, so the grids are identical in size and ordering and column i
        // corresponds to column i.
        if (PostColumns.Num() == SolveColumns.Num())
        {
            double WorstError = 0.0;
            int32 Compared = 0;
            for (int32 Index = 0; Index < PostColumns.Num(); ++Index)
            {
                if (!PostColumns[Index].IsSupported() || !SolveColumns[Index].IsSupported())
                {
                    continue;
                }
                const double Predicted = SolveColumns[Index].Clearance() + DeltaZ;
                WorstError = FMath::Max(WorstError,
                    FMath::Abs(PostColumns[Index].Clearance() - Predicted));
                ++Compared;
            }
            if (Compared > 0)
            {
                Post.SeatErrorCm = WorstError;
            }
        }
        EvaluateContact(Post, PostThresholds);
        Result.Seat.Contact = Post;

        if (Post.bPass)
        {
            Result.Seat.Status = EGroundSeatStatus::Seated;
            return Result;
        }

        // No revert branch. The instance stays where the solve put it and CurrentTransform /
        // PreviousTransform carry what it was, which actor.set_instance_transforms can write back
        // verbatim - the same "the echoed pre-move transform IS the undo" contract
        // spatial.ground_actors documents.
        GroundFailResult(Result.Seat, EGroundSeatStatus::VerificationFailed, *Post.FailReasonCode,
            Post.FailReason);
        return Result;
    }
}
