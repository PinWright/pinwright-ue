// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once
#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Engine/EngineTypes.h" // ECollisionChannel
#include "Handlers/Spatial/SpatialTraceUtils.h"
#include "UObject/WeakObjectPtr.h"

class AActor;
class UInstancedStaticMeshComponent;
class UPrimitiveComponent;
class UWorld;

// Ground placement: seat an actor on terrain so that it looks BEDDED rather than balanced
// or floating, and report - per actor - whether that actually happened.
//
// This is deliberately NOT a wrapper around "raycast down at the pivot, set Z". That model
// has four structural failure modes and this project has hit all four:
//
//   1. The probe hits the wrong thing. A VFX haze card - render geometry with a simple box
//      collider - blocks a downward trace and becomes "the ground", which on this project put
//      a whole batch of characters at Z 4299.6.
//      -> FGroundSurfaceSpec is REQUIRED, not defaulted: the caller must state what counts
//         as ground before anything moves, and the any_solid preset recognises effect cards
//         intrinsically (SpatialTraceUtils::IsEffectGeometryActor) rather than by a prefix.
//   2. A pivot is not the mesh's lowest point, and the mesh's lowest point is not a plane.
//      An AABB-bottom model touches a slope at ONE point and hangs everywhere else.
//      -> The underside is sampled per column (FGroundColumn::UndersideZ), from the actor's
//         own geometry, so the solve knows the shape of what it is seating.
//   3. Resting exactly tangent to the surface reads as a physics glitch, not as a rock.
//      -> Embedding is first-class (FGroundSeatConfig::EmbedFraction / EmbedDepth).
//   4. Nothing verified the result, so a re-seat pass could report success while 132 rocks
//      hung in the air.
//      -> MeasureContact() re-probes the world AFTER the move and EvaluateContact() derives
//         pass/fail from those numbers. FGroundSeatResult cannot express "placed" except by
//         way of a TOptional<FTransform> that only the code path which actually called
//         SetActorTransform can set.
//
// Engine reuse, evaluated and rejected for the core solve: FActorPositioning::
// GetSurfaceAlignedTransform (UE 5.8 Editor/UnrealEd/Public/Editor/ActorPositioning.h:174)
// is exactly the single-point flat-AABB model of failure 2 - its whole location solve is
// `SurfaceNormal * max(SnapOffsetExtent, FVector::BoxPushOut(SurfaceNormal, PlacementExtent))`
// (ActorPositioning.cpp:195-200) - and `SnapOffsetExtent` is a per-USER Editor Preferences
// value (ULevelEditorViewportSettings::SnapToSurface), which would make the same batch RPC
// produce different world positions on two machines. Both disqualify it here. It remains the
// right call for the single-point verb spatial.place_on_surface, which still uses it
// (PlacementHandler.cpp:381-385).
//
// Coordinates are unreal units (cm), left-handed (+X fwd, +Y right, +Z up). Everything in
// this module is game-thread only (it traces the editor world and moves actors).
namespace GroundPlacement
{
    // ---- Tunable ceilings -------------------------------------------------------------

    // Per-axis column count over the actor's XY footprint. 3 -> a 3x3 = 9 column grid, which
    // is the smallest grid that has a centre AND both extremes on each axis, i.e. the
    // smallest grid that can tell a slope from a step. The ceiling bounds worst-case trace
    // cost per actor at MaxGridSize^2 columns.
    constexpr int32 DefaultGridSize = 3;
    constexpr int32 MinGridSize = 1;
    constexpr int32 MaxGridSize = 9;

    // Trace-layer budget per ground column. Each layer peels one blocking actor, so this is
    // "how many props may stand between the actor and the ground before the probe gives up".
    // Lower than SpatialTraceUtils::DefaultMaxLayers (32) because this cost is paid per
    // COLUMN per ACTOR in a batch, not once per RPC.
    constexpr int32 DefaultMaxLayers = 8;

    // How far the ground probe may reach below the actor's underside, and how far above the
    // actor's top it starts. The rise matters as much as the drop: an actor BURIED in
    // geometry has its ground above its underside, and a probe that starts at the underside
    // can never see it. That is the case that left rocks embedded in 1700 uu cliff faces
    // looking correct until the cliffs flattened.
    constexpr double DefaultMaxDropCm = 100000.0;
    constexpr double DefaultProbeLiftCm = 500.0;

    // A column counts as "in contact" when its clearance is at or below this. Contact is
    // deliberately one-sided: touching (0) and buried (negative) both count, floating does
    // not.
    constexpr double DefaultContactToleranceCm = 2.0;

    // Fraction of the footprint half-extent that samples are pulled inward from the AABB
    // edge. An AABB corner over a rounded rock is empty air; sampling it would measure a
    // column the mesh does not occupy.
    constexpr double DefaultFootprintInset = 0.10;

    // Default embedding, as a fraction of the actor's bounds height. Non-zero on purpose:
    // a rock resting exactly tangent to terrain reads as balanced, and every caller that
    // wanted bedding had to know to ask for it. Set to 0 for a pure tangent rest.
    constexpr double DefaultEmbedFraction = 0.02;

    // Minimum fraction of the actor's own footprint columns that must find ground before the
    // actor is considered seatable. Half the footprint over nothing means the actor is at a
    // cliff edge or off the terrain entirely - the shape of every "rock in the sky beyond the
    // map edge" screenshot.
    constexpr double DefaultMinCoverage = 0.5;

    // Default tilt ceiling when surface alignment is requested, in degrees. Terrain normals
    // on a cliff face approach horizontal; letting a prop follow one lays it on its side.
    constexpr double DefaultMaxTiltDegrees = 20.0;

    // ---- What counts as ground --------------------------------------------------------

    // Named starting points for a surface spec. There is no "unset default that quietly means
    // everything": the RPC layer requires the caller to name one, because "which surface did
    // you mean" is the question every one of the observed failures answered wrong.
    enum class ESurfacePreset : uint8
    {
        // Only ALandscape / ALandscapeStreamingProxy. Simple collision. The correct answer
        // for terrain: a landscape heightfield is single-valued per column, so "the ground at
        // this XY" is unambiguous, and nothing else can shadow it.
        Landscape,
        // Anything that blocks, minus the two families that are render geometry rather than
        // ground: foliage actors (by class) and effect cards (haze, fog, glow, decals -
        // recognised intrinsically by component class and material blend mode, NOT by any
        // naming convention). Use when props must sit on structures as well as terrain.
        AnySolid,
        // Nothing implied; the caller supplies every filter itself.
        Custom
    };

    struct FGroundSurfaceSpec
    {
        ESurfacePreset Preset = ESurfacePreset::Custom;

        ECollisionChannel Channel = ECC_Visibility;

        // Complex (per-triangle) collision. Left false by every preset, and that IS the
        // documented right default for terrain: with true, a static mesh with NO simple
        // collision still blocks, because complex collision for a StaticMesh is its render
        // triangle soup. Measured on this project: 268 of 930 downward traceComplex probes
        // were blocked by a collisionless HISM foliage mesh rather than the landscape
        // (docs/wiki-src/spatial.md, "traceComplex hits render geometry").
        bool bTraceComplex = false;

        // Accept + reject criteria, applied to every ground hit. Presets populate this; a
        // Custom spec is whatever the caller passed.
        SpatialTraceUtils::FSpatialHitFilter Filter;

        // Actors removed from the query entirely (engine-level ignore list). The actor being
        // seated is always added to this by the solver - a caller never has to remember to.
        TArray<TWeakObjectPtr<AActor>> IgnoreActors;

        int32 MaxLayers = DefaultMaxLayers;
        double MaxDropCm = DefaultMaxDropCm;
        double ProbeLiftCm = DefaultProbeLiftCm;

        // Apply the named preset's filters on top of whatever is already set. Custom is a
        // no-op. Idempotent.
        void ApplyPreset();
    };

    // Fill Spec from a preset name ("landscape" / "any_solid" / "custom"). Returns false and
    // leaves Spec untouched for an unrecognized name, so an unknown preset can never silently
    // degrade into "trace everything".
    bool ParseSurfacePreset(const FString& Name, ESurfacePreset& OutPreset);
    const TCHAR* SurfacePresetToString(ESurfacePreset Preset);

    // Parse a wire `surface` object into a spec: {preset, channel?, traceComplex?,
    // excludeEffectGeometry?, onlyClasses?, excludeClasses?, excludeNames?, ignoreActors?,
    // maxLayers?, maxDrop?, probeLift?}, each accepted in both camelCase and snake_case.
    // Explicit lists ADD to the preset's, so {"preset":"landscape","excludeNames":["FX_Haze_*"]}
    // means what it looks like. `excludeEffectGeometry` is the one field that OVERRIDES the
    // preset rather than adding to it, so a caller whose real ground is translucent can say so.
    //
    // Returns false with OutError set and OutSpec left as the caller found it; never returns
    // true with a partially populated spec, and an unrecognized preset is an error rather than
    // a fallback to "trace everything". Fields the caller did NOT supply are left alone, so a
    // caller may pre-seed OutSpec with its own defaults (level.audit pre-seeds a much larger
    // ProbeLiftCm before calling, because a probe that starts below whatever buried an actor
    // can never see it).
    //
    // Shared so that every verb taking a `surface` argument agrees on what one is. This
    // function is the canonical parser; GroundPlacementHandler.cpp still carries its own copy
    // from before this existed and should be switched to it - see the integration notes in
    // scratchpad/level_audit_design.md.
    bool ParseSurfaceJson(const TSharedPtr<FJsonObject>& SurfaceObj, UWorld* World,
                          FGroundSurfaceSpec& OutSpec, TArray<FString>& OutUnresolvedIgnores,
                          FString& OutError);

    // ---- How the actor's own underside is modelled -------------------------------------

    enum class EUndersideModel : uint8
    {
        // Per-column trace against the actor's own primitive components, so the solve sees
        // the real shape of the bottom of the mesh. Falls back to BoundsPlane per actor when
        // no component answers a geometry query (no body instance, e.g. collision disabled),
        // and says so via FGroundContactReport::bUsedBoundsPlaneFallback.
        MeshProfile,
        // Flat plane at the actor's world-AABB minimum Z. Cheap and fully deterministic, and
        // wrong for anything whose bottom is not flat - this is the model spatial.
        // place_on_surface uses and the reason a boulder ends up balanced on one point.
        BoundsPlane
    };

    // ---- One sampled column ------------------------------------------------------------

    struct FGroundColumn
    {
        double X = 0.0;
        double Y = 0.0;

        // The actor's own geometry answered at this column. False means the footprint AABB
        // covers this XY but the mesh does not - the column imposes no seating constraint and
        // is excluded from coverage, which is why an AABB corner over a rounded rock cannot
        // drag the solve.
        bool bHasActorGeometry = false;
        double UndersideZ = 0.0;

        // An ACCEPTED ground surface was found in this column.
        bool bHasGround = false;
        double GroundZ = 0.0;
        FVector GroundNormal = FVector::UpVector;
        TWeakObjectPtr<AActor> GroundActor;

        // The PRIMITIVE that answered this column, straight off the trace hit. The actor alone
        // cannot say what the ground WAS: a scatter carried as instanced components on an
        // ordinary AActor has no actor class that distinguishes it from any other actor, which
        // is the same reason the surface filter needed a component axis
        // (SpatialTraceUtils::FSpatialHitFilter::ExcludeComponentClasses). Folded into
        // FGroundProvenance::SurfaceComponents so the response can NAME the surface it measured
        // instead of leaving the caller to guess whether it was stone or vegetation.
        TWeakObjectPtr<UPrimitiveComponent> GroundComponent;

        // WHICH collision representation produced GroundZ, carried through verbatim from the
        // trace hit (SpatialTraceUtils::FSpatialHit). The probe used to read Z, normal and actor
        // off that hit and drop these three, and dropping them is why a seat measured against a
        // collision HULL published as a seat against the surface a viewer sees: a hull and its
        // render mesh are different surfaces wherever the mesh carries boolean/erosion detail,
        // and - because FKSphereElem scales by the MINIMUM absolute scale component
        // (BodySetup.cpp, FKSphereElem::GetFinalScaled) - wherever a sphere hull is
        // non-uniformly scaled, with nothing authored wrongly by anyone.
        //
        // GroundFaceIndex is the provenance signal and the only one that works at the default
        // traceComplex:false: >= 0 means a triangle mesh or heightfield answered, INDEX_NONE
        // means a simple primitive did OR the backend reported none. A strong prior, not proof.
        int32 GroundFaceIndex = INDEX_NONE;
        int32 GroundSimpleCollisionShapes = -1;
        bool bGroundRenderGeometryHit = false;

        // UndersideZ - GroundZ. Positive = the mesh floats here, negative = it is buried
        // here. Meaningful only when bHasActorGeometry && bHasGround.
        double Clearance() const { return UndersideZ - GroundZ; }
        bool IsSupported() const { return bHasActorGeometry && bHasGround; }
    };

    // ---- Instanced-mesh scatter holders ------------------------------------------------

    // An ISM/HISM holder actor is not a prop, and measuring one as if it were is a silent wrong
    // answer rather than a wrong number. AActor::GetActorBounds on a holder returns the union of
    // EVERY instance, so the sampled "footprint" spans the whole scatter and the sampled
    // "underside" is a surface no single instance has. A pass/fail derived from those columns
    // describes nothing, and the seat solve's single SetActorTransform moves every instance at
    // once. Both ground verbs refuse such an actor rather than answer for it.
    struct FInstancedHolderInfo
    {
        // False leaves every other field at its default. Only a component carrying MORE THAN ONE
        // instance sets it: a single-instance component's bounds ARE that instance's bounds and
        // moving the actor moves it, so such an actor is an ordinary prop with nothing to refuse.
        bool bIsHolder = false;
        FString ComponentName;
        FString ComponentClass;
        int32 InstanceCount = 0;
    };

    // The instanced component that most defines Actor's bounds - the one carrying the most
    // instances - or a default (bIsHolder == false) info when the actor holds none.
    FInstancedHolderInfo FindInstancedHolder(const AActor* Actor);

    // The refusal text both verbs emit for Info, so neither can describe the condition
    // differently or point a caller at a different remedy. Empty string for a non-holder.
    FString DescribeInstancedHolder(const FInstancedHolderInfo& Info);

    // ---- Pass/fail thresholds ----------------------------------------------------------

    struct FContactThresholds
    {
        // Ceiling on FGroundContactReport::MaxGapCm - how far the actor's LOWEST point may sit
        // above the lowest ground under its footprint. Deliberately not a ceiling on the
        // silhouette (MaxColumnClearanceCm): overhanging or curved geometry is shape, and a
        // threshold on shape cannot be set to any useful value. See FGroundContactReport.
        double MaxGapCm = DefaultContactToleranceCm;
        double MaxPenetrationCm = DefaultContactToleranceCm;
        double MinCoverage = DefaultMinCoverage;
        int32 MinContactPoints = 1;
        double ContactToleranceCm = DefaultContactToleranceCm;

        // Whether MaxGapCm / MaxPenetrationCm are pass criteria at all.
        //
        // They are the right question for "is this actor acceptably grounded?", which is what
        // the verify verb asks. They are the WRONG question for a seat solve that deliberately
        // rests on first contact: on a slope the far side of the footprint is then legitimately
        // above the surface, and an absolute gap bound would fail a correct result. The seat
        // verb turns them off and relies on MaxSeatErrorCm instead, which is a check the solve
        // can honestly make about itself.
        bool bEnforceGapBounds = true;

        // Readback tolerance, in cm. When set, every column's clearance measured AFTER the
        // move must match what the solve predicted for that column within this much.
        //
        // This is the check the previous re-seat pass did not have. It fails when the actor
        // did not actually move, when the ground answer was not reproducible between the solve
        // and the readback, and when something else shifted underneath - i.e. exactly the
        // class of "reported success, 132 rocks in the air" failure. Unset = not checked.
        TOptional<double> MaxSeatErrorCm;
    };

    // ---- Measured contact quality ------------------------------------------------------

    // Which collision representation answered the ground probe, folded over the SUPPORTED
    // columns. Counts rather than one label, because a footprint can straddle a heightfield
    // column and a hull column and collapsing that would invent an answer for the other half.
    //
    // This block exists because every number in FGroundContactReport is a measurement of one of
    // two different surfaces and the response could not say which. A prop seated on a hull face
    // the visible mesh does not have produced a flawless, on-contract pass: the gap terms agreed
    // with each other precisely because they all read the same wrong surface, so no amount of
    // cross-checking them could break the tie.
    //
    // Nothing here costs an extra trace. Every field is computed on the hit the probe already
    // took and was previously discarded before serialization.
    struct FGroundProvenance
    {
        // What the probe ASKED for. A primitive answering a complex query means something
        // different from a primitive answering a simple one, so one contact row carries it
        // rather than making the caller cross-reference the batch-level surface echo.
        bool bTraceComplex = false;

        // Supported columns whose hit carried a face index: a triangle mesh or a landscape
        // heightfield answered - the surface a viewer sees.
        int32 TriangleColumns = 0;
        // Supported columns whose hit carried none: a simple primitive answered, i.e. the
        // collision HULL. See FGroundColumn::GroundFaceIndex - presumptive, not proof.
        int32 PrimitiveColumns = 0;
        // Supported columns that hit RENDER triangles because the struck component has no simple
        // collision at all (FSpatialHit::bRenderGeometryHit). Only a complex probe can produce
        // this, and it is the collisionless-foliage trap, not the hull one.
        int32 RenderGeometryColumns = 0;

        // Supported columns answered by an INSTANCED-MESH scatter component (ISM / HISM /
        // foliage), independent of which collision representation replied. The any_solid preset
        // admits a scatter as ground DELIBERATELY - a HISM of paving stones is legitimate ground
        // and excluding the generic instanced classes would move every actor already seated on
        // one - so this is the caller's call to make, which they can only make if the response
        // says a scatter answered. Batch-level because SurfaceComponents is capped and a row
        // cannot carry a total.
        int32 InstancedScatterColumns = 0;
        // Supported columns whose component's body setup resolves to CTF_UseComplexAsSimple, so
        // per-triangle geometry answers even a SIMPLE query (Utils/CollisionSummaryUtils.h).
        // Recorded raw rather than pre-judged: only a probe that did NOT ask for complex tracing
        // is a disagreement between what was requested and what replied.
        int32 ComplexAsSimpleColumns = 0;
        // Supported columns whose answering primitive did not resolve, so NOTHING above is a
        // statement about them. Its own number rather than an implied zero: "measured and clean"
        // and "could not be measured" are different answers, and one silence for both is how the
        // second reads as the first.
        int32 UnclassifiedColumns = 0;

        // Range of FSpatialHit::SimpleCollisionShapes over the supported columns; both stay -1
        // when no column's component exposed a body setup at all (the Landscape case). A count
        // of 1 standing in for an elaborately modelled mesh is the signature shape of a hull
        // that is not its own render surface - cheap triage, never a verdict: it carries neither
        // the magnitude nor the SIGN of any divergence.
        int32 MinSimpleCollisionShapes = -1;
        int32 MaxSimpleCollisionShapes = -1;

        // One entry per DISTINCT primitive that answered a supported column: what the ground
        // actually WAS, by name and by class.
        //
        // The counts above say which collision REPRESENTATION answered; they cannot say that the
        // representation belonged to a kelp scatter rather than to a cliff. Nothing else in the
        // report can either - the ground actor is never published, and a scatter holder is an
        // ordinary AActor whose class names nothing. So a probe that resolved against foliage
        // the any_solid preset believed it had excluded read exactly like a probe that resolved
        // against stone, which is the blind spot the component axis on the FILTER closed and
        // this closes on the REPORT. Without it the next blind spot in the filter is as
        // invisible as that one was.
        struct FSurfaceComponent
        {
            // Identity key. Dedup is by the component itself and never by name: labels are not
            // unique and two actors can carry identically-named components, so a name key would
            // merge two surfaces that are not the same surface - the same reason MeasureContact
            // keys its rejected-actor seen-set on the actor rather than on its label.
            TWeakObjectPtr<UPrimitiveComponent> Component;

            FString ActorLabel;     // owner's display label - a bare component name locates nothing
            FString ComponentName;  // UPrimitiveComponent::GetName()
            FString ComponentClass; // its UClass name, e.g. "HierarchicalInstancedStaticMeshComponent"

            // Supported columns this primitive answered. A footprint may straddle several
            // surfaces, so this is a distribution rather than a single label - collapsing it
            // would invent an answer for whichever half lost.
            int32 Columns = 0;

            // WHY this surface may not be one to trust as ground, measured on the primitive
            // itself. None of these can be carried by the face-index counts above: a
            // complex-traced scatter, a collisionless foliage mesh and a UseComplexAsSimple body
            // all answer WITH a face index, so they land in TriangleColumns and the hull warning
            // - gated on the ABSENCE of one - is structurally unable to fire on any of them.
            //
            // Three independent facts, never reduced to one verdict per row, for the same reason
            // Columns is a distribution: a footprint straddling a scatter and a cliff has two
            // different answers and neither row may be made to speak for the other.
            bool bInstancedScatter = false; // a UInstancedStaticMeshComponent (ISM / HISM / foliage)
            bool bRenderGeometry = false;   // >= 1 of ITS columns answered off render triangles
            bool bComplexAsSimple = false;  // its body setup resolves to CTF_UseComplexAsSimple
        };

        // Descending by Columns, so the surface that answered most of the footprint reads first.
        // Capped; SurfaceComponentCount carries the true distinct total.
        TArray<FSurfaceComponent> SurfaceComponents;
        int32 SurfaceComponentCount = 0;
    };

    // Everything a caller needs to see the failures that used to survive: how much of the
    // footprint has ground under it, how far the actor floats, how much of what looks like
    // float is its own shape, how deep the deepest column is buried, how many columns actually
    // touch, and what the surface filter threw away.
    struct FGroundContactReport
    {
        // False means NOTHING below was measured and every number is at its zero default.
        // JSON serialization omits the numbers entirely in that case rather than emitting
        // zeros that read as real measurements.
        bool bMeasured = false;

        // Set when the measured actor is an instanced-mesh scatter holder. Every number below is
        // then a figure about the union of a whole scatter, which is exactly why EvaluateContact
        // refuses on this ahead of bMeasured and ahead of every threshold instead of judging them.
        FInstancedHolderInfo InstancedHolder;

        int32 SampledColumns = 0;   // grid columns probed
        int32 ActorColumns = 0;     // ...that the actor's own geometry occupies
        int32 SupportedColumns = 0; // ...that ALSO found an accepted ground surface

        // SupportedColumns / ActorColumns. 1.0 = ground under the whole footprint; 0.5 = half
        // the actor overhangs nothing.
        double Coverage = 0.0;

        // Columns whose clearance is <= ContactToleranceCm, i.e. touching or buried. One
        // contact point on a wide actor is the balanced-boulder signature.
        int32 ContactPoints = 0;

        // A column's clearance is the sum of two independent terms, and only one of them says
        // anything about placement:
        //
        //   Clearance_i = (UndersideZ_i - min_j UndersideZ_j) + (min_j UndersideZ_j - GroundZ_i)
        //                 \____ relief: the actor's SHAPE ____/  \___ float: the PLACEMENT ____/
        //
        // The relief term is a property of the mesh and is there however well the actor is
        // seated - a column with a capital wider than its base, a cylinder lying on its side, an
        // arch, a figure with a rounded body. Taking the max over RAW clearances mixes the two,
        // and on a shaped actor the relief term dominates by orders of magnitude, so no
        // threshold can separate a seated actor from a hovering one. That is what these three
        // fields exist to split apart.
        //
        // MaxGapCm is the float term at its worst: the actor's LOWEST underside sample minus the
        // LOWEST accepted ground under its footprint. It is what FContactThresholds::MaxGapCm
        // bounds and the only one of the three that can fail an actor. Provably
        // MinGapCm <= MaxGapCm <= MaxColumnClearanceCm, so nothing that passed under the old
        // silhouette rule can start failing under this one.
        double MaxGapCm = 0.0;
        // Smallest clearance across supported columns. Negative where the actor is buried.
        double MinGapCm = 0.0;
        // Largest RAW clearance over any supported column, relief included - the silhouette
        // figure MaxGapCm used to hold. Reported, never a pass criterion: it is what a caller
        // reads, next to UndersideReliefCm, to see why a shaped actor's numbers look the way
        // they do.
        double MaxColumnClearanceCm = 0.0;
        // Highest minus lowest underside sample across supported columns: how far the actor's
        // own underside is from being a plane. Large relief with a small MaxGapCm is a shaped
        // actor that is properly seated - exactly the case that used to read as a placement bug.
        double UndersideReliefCm = 0.0;
        // max(0, -MinGapCm).
        double PenetrationCm = 0.0;

        // Highest minus lowest accepted ground Z across supported columns. A large spread
        // under a small actor means the footprint straddles a discontinuity (a cliff lip),
        // where any single-point answer would have been arbitrary.
        double GroundSpreadCm = 0.0;

        // Mean of the accepted ground normals, renormalized. More stable than any single
        // normal, which is what makes it safe to align rotation to.
        FVector AverageNormal = FVector::UpVector;

        // Which surface every number above was measured against. Meaningful only when
        // SupportedColumns > 0, and serialized only then.
        FGroundProvenance Provenance;

        EUndersideModel UndersideModel = EUndersideModel::MeshProfile;
        // The mesh-profile probe found no geometry in ANY column and the flat AABB plane was
        // used instead. Reported because it silently changes what the numbers mean.
        bool bUsedBoundsPlaneFallback = false;

        // The XY half-extent, in cm, the sample grid actually spanned - world-axis-aligned and
        // centred on the sampled bounds. Every OTHER number in this report is computed over this
        // box: Coverage, ContactPoints and all three gap terms are truthful about whatever was
        // sampled, and none of them names it. Sampling a trunked tree over its own AABB measures
        // the CANOPY - measured 656.8 x 801.3 uu of bounds half-extent around a 109.7 x 102.8
        // contact patch, 46.7x the area - reports coverage 1.00, pass true, undersideReliefCm 0,
        // and proposes lifting an already-planted tree by a median of 196 cm. Naming the box is
        // what makes those three green fields readable as statements about a box rather than
        // about the object.
        FVector2D FootprintHalfExtentCm = FVector2D::ZeroVector;
        // Where FootprintHalfExtentCm came from. False: the sampled bounds' own half-extent,
        // reduced by the footprint inset. True: a caller-stated contact half-extent that replaced
        // it. The inset cannot express the second - it is a FRACTION of the bounds, so it moves
        // with yaw and per-instance scale and is clamped at 0.45 besides.
        bool bFootprintFromContact = false;

        // Display labels of actors the surface filter peeled off the ground probe, capped.
        // This is the field that names the fog card when a probe "found no ground".
        TArray<FString> RejectedSurfaceActors;
        int32 RejectedSurfaceActorCount = 0;

        // Landscape heightfield cross-check, set only when a landscape is present in the
        // world and only for columns the trace could not answer. Unset means "not asked".
        // False means the landscape itself reports no height at this XY, which is the
        // structural difference between "something is in the way" and "this actor is not
        // over the terrain at all" - the second being the rock-beyond-the-map-edge case.
        TOptional<bool> bOverLandscape;

        // Worst per-column disagreement, in cm, between the clearance the solve predicted
        // after its move and the clearance actually measured afterwards. Set only on the
        // post-move report of a seat operation. See FContactThresholds::MaxSeatErrorCm.
        TOptional<double> SeatErrorCm;

        // Derived by EvaluateContact() only. Never assigned from a literal.
        bool bPass = false;
        FString FailReasonCode; // an ErrorCodes.h ERR_* value; empty iff bPass
        FString FailReason;     // human text; empty iff bPass
    };

    // Fold per-column samples into Report's aggregate numbers (counts, coverage, the three gap
    // terms, penetration, ground spread, average normal) and set bMeasured. Does NOT derive
    // pass/fail - the caller runs any world-dependent diagnostics it wants (the landscape
    // cross-check) and then calls EvaluateContact.
    //
    // Split out of MeasureContact so the arithmetic that turns samples into a verdict can be
    // exercised on constructed columns: the shapes that matter here - a capital wider than its
    // base, a cylinder on its flank - are underside profiles, not levels, and building them as
    // numbers is both exact and free.
    void AggregateColumns(const TArray<FGroundColumn>& Columns,
                          const FContactThresholds& Thresholds,
                          FGroundContactReport& Report);

    // Derive bPass / FailReason* on Report from Thresholds. The ONLY writer of those three
    // fields. Both the seat verb and the verify verb call it, so they cannot disagree about
    // what "seated" means.
    void EvaluateContact(FGroundContactReport& Report, const FContactThresholds& Thresholds);

    // The wire form of Report.Provenance: which surface every gap number in the report was
    // measured against, and which primitive answered. Meaningful only where the report found
    // ground, so callers gate it on SupportedColumns > 0 exactly as the contact writer does.
    //
    // Publishes `surfaceTrust` ("trusted" / "untrusted" / "undetermined") next to the counts,
    // and `warning` carrying every fired condition in one string. The verdict is over the SET
    // of conditions, not over the face-index split alone: a scatter, a collisionless mesh and a
    // UseComplexAsSimple body all answer WITH a face index, so a warning gated on its absence
    // could never reach them. "undetermined" exists because a caller cannot read the difference
    // between a clean surface and an unexaminable one out of a single missing warning.
    //
    // Shared rather than file-local because more than one verb now seats against a stated
    // surface: spatial.ground_actors / verify_grounding / ground_instances all reach it through
    // one contact writer, and foliage.paint publishes it for each instance it projected. A
    // second copy of this text next to the second caller is how two verbs come to describe the
    // same measurement differently.
    TSharedPtr<FJsonObject> MakeProvenanceJson(const FGroundContactReport& Report);

    // ---- Seat configuration ------------------------------------------------------------

    struct FGroundSeatConfig
    {
        int32 GridSize = DefaultGridSize;
        double FootprintInset = DefaultFootprintInset;

        // Radius of the object's CONTACT PATCH in MESH-LOCAL cm - the part of it that actually
        // rests on the ground, which for anything with a trunk, a stem or a pedestal is a small
        // fraction of its silhouette. 0 (the default) keeps the bounds footprint.
        //
        // Read only by SeatInstance, which scales it by the instance's own mean XY scale before
        // handing it to the sampler. Mesh-local rather than a bounds ratio because the ratio is
        // the wrong quantity: FootprintInset is a fraction of the world AABB, so it moves with
        // instance yaw and instance scale, and at its 0.45 ceiling still leaves 361 x 441 uu of
        // sample half-extent around a measured 100 uu contact radius.
        double ContactRadiusCm = 0.0;

        EUndersideModel UndersideModel = EUndersideModel::MeshProfile;

        // Which column the actor comes to rest against, as a percentile over the columns'
        // clearances:
        //   0.0 -> rest on the FIRST contact (the highest ground under the footprint). Nothing
        //          is buried; on uneven ground exactly one column touches. This is physically
        //          what "resting" means and visually what "balanced" looks like, which is why
        //          embedding exists.
        //   1.0 -> sink until NO column floats (the lowest ground under the footprint). The
        //          "no part floats" constraint, at the cost of burying the high side.
        //   0.5 -> median; half the footprint above the surface, half below.
        // Clamped to [0,1].
        double SeatPercentile = 0.0;

        // Extra sink after the percentile solve, so the actor beds into the ground instead of
        // touching it. Total = EmbedDepthCm + EmbedFraction * (bounds height). Both default
        // to a small non-zero total (see DefaultEmbedFraction).
        double EmbedFraction = DefaultEmbedFraction;
        double EmbedDepthCm = 0.0;

        // Rotate the actor's +Z toward the averaged ground normal before seating, clamped to
        // MaxTiltDegrees away from world up.
        bool bAlignToSurface = false;
        double MaxTiltDegrees = DefaultMaxTiltDegrees;

        FContactThresholds Thresholds;

        // Readback tolerance in cm for the post-move check (see
        // FContactThresholds::MaxSeatErrorCm). 1 cm is well inside terrain triangle scale and
        // well outside float noise on a 100 km world.
        double MaxSeatErrorCm = 1.0;

        // Move the actor back to where it was when the post-move verification fails. Off by
        // default: a failed actor left in place is inspectable, and the previous transform is
        // echoed per actor either way.
        bool bRevertOnFailure = false;

        // Total embed depth in cm for an actor of the given world bounds height.
        double ResolveEmbedCm(double BoundsHeightCm) const;
    };

    // ---- Per-actor outcome -------------------------------------------------------------

    // NotAttempted is value 0, so a default-constructed result is a FAILURE. There is no
    // default-constructible success.
    enum class EGroundSeatStatus : uint8
    {
        NotAttempted = 0,
        ActorNotFound,
        ActorHasNoBounds,
        ActorLocationLocked,
        // The actor is an ISM/HISM scatter holder (see FInstancedHolderInfo). Refused before the
        // pre-move probe: seating it would solve against the union of every instance and then
        // relocate all of them with one transform.
        HolderNotSeatable,
        // spatial.ground_instances with apply:false. The solve ran and produced a move; nothing
        // was written. The only status other than Seated that carries an empty ReasonCode,
        // because a dry run is not a failure - it is the absence of an attempt.
        DryRun,
        NoGroundFound,
        GroundHitsAllRejected,
        PartialGroundCoverage,
        VerificationFailed,
        Reverted,
        Seated
    };

    const TCHAR* SeatStatusToString(EGroundSeatStatus Status);

    struct FGroundSeatResult
    {
        FString ActorLabel;
        FString ActorPath;

        EGroundSeatStatus Status = EGroundSeatStatus::NotAttempted;
        // an ErrorCodes.h ERR_* value; empty iff Status is Seated or DryRun
        FString ReasonCode;
        FString Reason; // human text; empty iff Status is Seated or DryRun

        // Set ONLY where SetActorTransform is actually called, and cleared again if the move
        // is reverted. This is the single source of truth for "did this actor move": the JSON
        // `moved` field is IsSet() on it, never a literal, so a code path that forgets to move
        // an actor cannot report that it did.
        TOptional<FTransform> AppliedTransform;

        // Captured immediately before the move, so a caller can restore a bad result.
        TOptional<FTransform> PreviousTransform;

        // Post-move re-probe. Pre-move probe when nothing was moved.
        FGroundContactReport Contact;

        // Applied embed depth in cm, echoed so a caller can tell a deliberate sink from a
        // solver error when reading PenetrationCm.
        double AppliedEmbedCm = 0.0;
        // Vertical distance the actor was moved by, signed (negative = downward).
        double AppliedDeltaZCm = 0.0;

        bool WasMoved() const { return AppliedTransform.IsSet(); }

        // The only definition of success, and it is a conjunction of two independently
        // established facts: something actually moved, and the world was re-measured
        // afterwards and passed. Neither is assertable on its own.
        bool IsSeated() const { return AppliedTransform.IsSet() && Contact.bPass; }
    };

    // ---- Operations --------------------------------------------------------------------

    // Landscape heightfield height at a world XY, bypassing collision entirely:
    // ALandscapeProxy::GetHeightAtLocation (UE 5.8 Runtime/Landscape/Classes/LandscapeProxy.h
    // :1101, LANDSCAPE_API, not editor-gated) walks ULandscapeInfo::XYtoCollisionComponentMap
    // and returns an unset TOptional off the landscape.
    //
    // Used ONLY as a diagnostic, never as the seating surface: a trace is what the rest of the
    // world agrees with, and quietly seating on a heightfield the collision system disagrees
    // with would trade one silent wrongness for another. What it buys is the ability to say
    // "this actor is not over the terrain at all" instead of "no ground found", which are the
    // same trace result and completely different bugs.
    //
    // Returns unset when there is no landscape in the world, or the XY is off every landscape.
    TOptional<double> ProbeLandscapeHeight(UWorld* World, double X, double Y);

    // Probe the actor where it stands and report contact quality. Non-mutating. Every trace
    // ignores Actor itself, so it can never measure against its own geometry.
    //
    // OutColumns, when non-null, receives the raw per-column samples in row-major grid order.
    //
    // An ISM/HISM scatter holder is still SAMPLED - callers such as level.audit read the raw
    // numbers - but the report carries FInstancedHolderInfo and EvaluateContact refuses it, so
    // no verdict about the union of a scatter can be read as a verdict about a prop.
    FGroundContactReport MeasureContact(UWorld* World, AActor* Actor,
                                        const FGroundSurfaceSpec& Surface,
                                        int32 GridSize, double FootprintInset,
                                        EUndersideModel UndersideModel,
                                        const FContactThresholds& Thresholds,
                                        TArray<FGroundColumn>* OutColumns = nullptr);

    // The bounds-level half of MeasureContact: the footprint grid, the ground probe, the
    // aggregation and the verdict, for a caller that already knows the world AABB it wants
    // sampled. MeasureContact is a thin wrapper over this, so the two cannot measure differently.
    //
    // It exists because an ISM/HISM INSTANCE has no actor of its own - its footprint is the static
    // mesh's bounds under (instance transform x component transform) - and the actor-bound entry
    // point derives the footprint from AActor::GetActorBounds, which for a scatter holder is the
    // union of every instance.
    //
    // UndersideGeometry is the actor whose OWN primitive components answer the per-column
    // underside probe under EUndersideModel::MeshProfile. Pass null - an instance has no such
    // actor - and the flat bounds plane is used instead with bUsedBoundsPlaneFallback set, the
    // same honesty flag an actor gets when none of its components answers a geometry query.
    //
    // ExtraIgnoreActors is ADDED to the actors Surface.IgnoreActors already names, and nothing is
    // added implicitly: this function does not know what is being measured, so the caller states
    // it. MeasureContact passes the measured actor. An instance caller passes its HOLDER, which
    // necessarily excludes every sibling instance too - an ISM's instances share one actor and a
    // world trace has no finer granularity - so a scatter can never be seated onto itself.
    //
    // ContactHalfExtentCm, when set and positive on both axes, REPLACES the inset bounds
    // half-extent as the XY span of the sample grid - clamped to the bounds, because a grid wider
    // than the box would sample columns no underside model can answer. TopZ, BottomZ, the probe
    // span and the rest of the solve are untouched: the defect this exists for is the FOOTPRINT
    // the underside model is applied over, not the model. Unset keeps the historical behaviour.
    // Whichever source wins is published on the report as FootprintHalfExtentCm /
    // bFootprintFromContact, so a reader is never left to infer which box the numbers describe.
    //
    // Does NOT look for an instanced holder. FInstancedHolderInfo is an actor-level fact and the
    // instance path must not refuse itself; MeasureContact records it after this returns.
    FGroundContactReport MeasureContactForBounds(UWorld* World, const FBox& WorldBounds,
                                                 AActor* UndersideGeometry,
                                                 const TArray<AActor*>& ExtraIgnoreActors,
                                                 const FGroundSurfaceSpec& Surface,
                                                 int32 GridSize, double FootprintInset,
                                                 const TOptional<FVector2D>& ContactHalfExtentCm,
                                                 EUndersideModel UndersideModel,
                                                 const FContactThresholds& Thresholds,
                                                 TArray<FGroundColumn>* OutColumns = nullptr);

    // Measure, solve, move, then measure AGAIN and evaluate the second measurement. The
    // returned result reports Seated only when that second measurement passed.
    //
    // Never returns a partially-populated success: every early-out path leaves
    // AppliedTransform unset, so FGroundSeatResult::IsSeated() is false for all of them.
    //
    // An ISM/HISM scatter holder is refused before anything is probed or moved, with the same
    // code and the same text MeasureContact's report carries for it. The refusal is symmetric on
    // purpose: a verb that moved what the other verb declines to judge would be the worse half.
    FGroundSeatResult SeatActor(UWorld* World, AActor* Actor,
                                const FGroundSurfaceSpec& Surface,
                                const FGroundSeatConfig& Config);

    // ---- Per-instance seating ----------------------------------------------------------

    // One instance's outcome. Composes FGroundSeatResult rather than restating it, so an
    // instance row reports status, reason, contact quality and the undo transform in exactly the
    // vocabulary an actor row does.
    struct FGroundInstanceSeatResult
    {
        int32 InstanceIndex = INDEX_NONE;

        FGroundSeatResult Seat;

        // The instance's world transform as it was FOUND, set on every path that managed to read
        // one - dry runs included. Seat.PreviousTransform stays the undo record and is set only
        // where a write followed, so "what it was" and "what to put back" cannot be confused.
        TOptional<FTransform> CurrentTransform;

        // Dry run only (bApply == false): what an applying call would have written, and the
        // signed Z it would have moved by. Unset on an applying call, where Seat.AppliedTransform
        // and Seat.AppliedDeltaZCm carry what actually happened.
        TOptional<FTransform> ProposedTransform;
        double ProposedDeltaZCm = 0.0;
    };

    // Seat ONE instance of an instanced-mesh component: measure the ground under the instance's
    // own footprint, solve the drop, write the instance transform, then measure AGAIN and evaluate
    // the second measurement. Same solve, same thresholds and same verdict as SeatActor - only the
    // footprint source and the write differ.
    //
    // The footprint is the component's static mesh bounds under the instance's world transform,
    // and the underside is modelled as that box's bottom PLANE. A mesh-profile underside is not
    // available per instance: UInstancedStaticMeshComponent::LineTraceComponent answers from every
    // instance body at once (InstancedStaticMesh.cpp:5442-5444) and cannot attribute a hit, so a
    // per-column probe would silently measure a NEIGHBOUR's geometry. The report says
    // bounds_plane, which is the honest label for what was measured.
    //
    // FGroundSeatConfig::ContactRadiusCm overrides the XY span of the sample grid - and ONLY that
    // span: mesh-local cm, scaled here by the instance's own mean XY scale. Without it the grid is
    // the instance's AABB, which for anything with a trunk is its canopy; the flat underside plane
    // is then solved over ground the object never touches and a correctly-planted instance is
    // proposed for a lift. Either way the sampled half-extent is published on the contact report
    // (FGroundContactReport::FootprintHalfExtentCm), because every quality number in that report
    // is computed over it and none of the others names it.
    //
    // The write is UInstancedStaticMeshComponent::UpdateInstanceTransform, which is the only route
    // that tells the render tracker, the physics bodies and navigation. It is NEVER the reflection
    // property layer: a store into PerInstanceSMData succeeds, reads back, and moves nothing,
    // because ISM's instance handling lives entirely in PostEditChangeChainProperty and the
    // non-chain notification this plugin emits cannot reach it.
    //
    // bApply == false solves and reports without writing (ProposedTransform / ProposedDeltaZCm,
    // Status == DryRun). The solve is identical to the applying path's, including its thresholds,
    // so a dry run predicts what apply would do rather than answering a different question.
    //
    // Does NOT dirty render state, rebuild a HISM cluster tree or dirty the package: those are
    // once-per-batch, and the caller does them after its loop (see
    // Handlers/Actor/InstancedMeshUtils.h, FinishInstanceWrites).
    FGroundInstanceSeatResult SeatInstance(UWorld* World, UInstancedStaticMeshComponent* Component,
                                           int32 InstanceIndex, const FGroundSurfaceSpec& Surface,
                                           const FGroundSeatConfig& Config, bool bApply);
}
