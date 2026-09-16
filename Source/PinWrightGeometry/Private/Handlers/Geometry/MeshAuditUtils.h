// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once
#include "CoreMinimal.h"
#include "Audit/AuditFramework.h" // the shared check-table / finding / verdict contract
#include "Dom/JsonObject.h"
#include "Handlers/Geometry/GeometryUtils.h" // GeometryUtils::FMeshHealth, DegenerateAreaEpsilon

class UDynamicMesh;
class UStaticMesh;
class USkeletalMesh;
class UAnimSequence;
struct FAssetData;

// Static-mesh health sweep: one call that reads a SET of saved UStaticMesh assets and reports,
// per asset, whether it is inside out, wound inconsistently, open, degenerate or empty.
//
// Why it exists. GeometryUtils::FMeshOrientation / FMeshHealth have measured all of this
// correctly for some time, but the only verb that could reach the measurement -
// geometry.check_health - resolves an ADynamicMeshActor in the live world (GeometryTarget.h),
// and the only route from a saved asset to such an actor is geometry.create_from_static_mesh,
// which SPAWNS one, per asset. So "are any of my shipped meshes inside out?" was a question
// with a correct answer and no way to ask it, and two inverted tree meshes shipped.
//
// Four design constraints, each inherited from a defect this project actually shipped:
//
//   1. BATCH BY CONSTRUCTION. LevelAuditUtils.h:18-20 records a per-actor Python loop wedging
//      an editor for 168 minutes across 5100 uncancellable calls. Run() walks the page once,
//      reusing ONE scratch UDynamicMesh across every asset, and spawns nothing. There is
//      deliberately no per-asset entry point in the RPC layer.
//   2. AN UNMEASURED ASSET IS NOT A HEALTHY ASSET. Every check reports, per asset, one of
//      {flagged, clean, unrunnable, not-applicable}, and those buckets sum to the number of
//      assets examined. An asset that would not load, or whose LOD would not copy, produces
//      EFindingStatus::Unrunnable rows naming the reason. Silence never means "fine".
//   3. AN EMPTY MATCH SET IS AN ERROR. Enforced by the handler, not here, but it is the same
//      rule audio.analysis.audit_folder ships (AudioAnalysisHandler.cpp:1716): a folder that
//      matched nothing and a folder full of correct meshes must not produce the same answer.
//      This project has been burned exactly there - check_actors once exited 0 having
//      examined nothing.
//   4. A CONTENT DEFECT IS NEVER AN RPC ERROR. Every finding travels on a SendSuccess with
//      pass:false, exactly as level.audit does. The ERR_* values below are FINDING codes, not
//      transport errors; nothing here can make "this mesh is inverted" fail a call.
//
// HANDEDNESS IS LOAD-BEARING and is not re-derived here. Unreal's facing normal is
// (V2-V0) x (V1-V0) - the NEGATION of the right-hand rule, because Unreal is left-handed -
// and the signed volume this sweep reads comes from TMeshQueries::GetVolumeArea through
// GeometryUtils::MeasureMeshOrientation, which already encodes it. Get the sign convention
// wrong and every verdict inverts, so there is exactly one implementation of it in the tree
// and this file calls it rather than repeating it. The offline cross-check that reproduces
// the same convention over .pwmodel sources is Content/Python/measure_pwm.py.
//
// This namespace is NOT the .pwmodel compiler's winding gate and must not become one.
// PwModelCompiler.cpp:2571-2579 deliberately declines to gate winding, because at merge time
// it cannot name WHICH op inverted what, and a model-level "something is inside out" with no
// line number is a worse answer than a number the caller gates on. That argument holds at
// model scope. This sweep works at ASSET scope, where there is nothing to attribute: the only
// question is "is this shipped mesh inverted", which is exactly answerable.
//
// Game-thread only (package loads + MeshDescription reads). Read-only: nothing here calls
// Modify(), MarkPackageDirty(), or saves anything.
namespace MeshAudit
{
    // ---- Checks -------------------------------------------------------------------------

    // One entry per check. The ordering is the response ordering; the bit index is NOT
    // serialized, so a reorder only affects readers who compare the checks[] array
    // positionally.
    enum class ECheck : uint8
    {
        // The defect this whole namespace exists for. A closed shell wound inside out is
        // byte-identical to a correct one in vertex count, triangle count, boundary edges,
        // bowties and component count, and renders identically under backface culling.
        Inverted = 0,
        // Partial inversion: neighbours that disagree. A UNIFORMLY inverted shell is
        // perfectly consistent, so this is not a substitute for Inverted and Inverted is
        // not a substitute for this.
        InconsistentWinding,
        NotClosed,
        DegenerateTriangles,
        NonManifoldVertices,
        EmptyMesh,
        // Names the CAUSE when Inverted fires on an asset whose triangles are fine: a build
        // scale with an odd number of negative axes mirrors the mesh at build time.
        MirroredBuildScale,
        // Off by default. A "closed" shell whose enclosed volume is negligible against its
        // own surface area is a sheet folded back on itself - it passes Inverted (volume is
        // not negative) and NotClosed (no boundary edges) while enclosing nothing.
        ThinShell,
        ZFighting,
        // Components may be disconnected; this warns only for spatially isolated islands.
        FloatingComponents,
        Count
    };

    constexpr int32 CheckCount = static_cast<int32>(ECheck::Count);
    static_assert(CheckCount <= 32, "The check selection bitmask is a uint32.");

    // Severity, finding status and the verdict come from the one shared audit contract
    // (Audit/AuditFramework.h - reachable from this module through the PrivateIncludePaths
    // entry that already points at Source/PinWright/Private). Aliased rather than redeclared
    // so MeshAudit::ESeverity::Error stays the spelling at every call site while there is only
    // one definition of the type. Warning is reported but does not fail under the default
    // failOn:"error"; Error is reserved for conditions that cannot be deliberate for a mesh
    // that is shipped and placed.
    using ESeverity = PinWrightAudit::ESeverity;
    using EFindingStatus = PinWrightAudit::EFindingStatus;

    struct FCheckInfo
    {
        ECheck Check;
        const TCHAR* Id;    // wire id, e.g. "inverted"
        const TCHAR* Code;  // ErrorCodes.h ERR_* value carried on a finding
        ESeverity Severity;
        // In the default check set. Everything needing an extra threshold, and everything
        // known to be noisy on real imported content, is off by default and must be asked for.
        bool bDefaultOn;
        // The check is only meaningful on a CLOSED mesh; on an open one it reports
        // not-applicable rather than clean. FMeshOrientation::IsInverted() is deliberately
        // false on an open mesh - an open surface is not inside out, it is open - and folding
        // that into "clean" would put a second wrong answer next to the first.
        bool bNeedsClosed;
        const TCHAR* Summary;
    };

    // Every check, in ECheck order. Index i is the check whose ECheck value is i.
    const TArray<FCheckInfo>& AllChecks();

    // The rest are the shared contract's operations over this table (Audit/AuditFramework.h).
    inline const FCheckInfo& CheckInfo(ECheck Check) { return PinWrightAudit::CheckInfo(AllChecks(), Check); }
    // Wire id -> ECheck. False for an unknown id, which callers must turn into an ERROR
    // rather than a silent no-op: a typo in `checks` that silently ran nothing looks exactly
    // like a folder of clean meshes.
    inline bool ParseCheckId(const FString& Id, ECheck& OutCheck)
    {
        return PinWrightAudit::ParseCheckId(AllChecks(), Id, OutCheck);
    }

    inline uint32 CheckBit(ECheck Check) { return PinWrightAudit::CheckBit(Check); }
    inline bool HasCheck(uint32 Mask, ECheck Check) { return PinWrightAudit::HasCheck(Mask, Check); }
    inline uint32 DefaultCheckMask() { return PinWrightAudit::DefaultCheckMask(AllChecks()); }
    inline uint32 AllCheckMask() { return PinWrightAudit::AllCheckMask(AllChecks()); }

    // Dimensionless: a half-percent of the model's own bounding-sphere radius.
    inline constexpr double DefaultFloatingToleranceFraction = 0.005;
    inline constexpr double DefaultZFightPlaneExtentFraction = 1.0e-5;
    inline constexpr double DefaultZFightNormalDotThreshold = 0.999;
    // Broad-phase work bounds. The fine grid is scale-relative; triangles spanning too many
    // fine cells use the coarse-grid fallback instead. The fallback counts every coarse-grid
    // reference it inspects before filtering or deduplication. All limits fail closed as
    // unrunnable rather than silently dropping pairs and reporting clean.
    inline constexpr int32 ZFightGridResolution = 32;
    inline constexpr int32 ZFightLargeGridResolution = 4;
    inline constexpr int32 ZFightMaxFineCellsPerTriangle = 256;
    inline constexpr int32 ZFightMaxTrianglesPerFineCell = 256;
    inline constexpr int32 ZFightMaxCoarseReferencesPerLargeTriangle = 256;
    inline constexpr int32 ZFightMaxLargeCandidatesPerTriangle = 256;
    inline constexpr int32 ZFightMaxCandidatePairsPerTriangle = 256;
    // Unique region-area union is quadratic in the number of accepted pair polygons. Keep that
    // local post-process bounded so the overall detector remains expected-linear in triangle and
    // candidate counts, and fail closed rather than spending unbounded time on dense overlap.
    inline constexpr int32 ZFightMaxUnionPolygonsPerRegion = 512;
    inline constexpr int32 ZFightMaxTopPairsPerRegion = 16;

    // ---- Thresholds ---------------------------------------------------------------------

    struct FThresholds
    {
        // ThinShell fires below this dimensionless ratio |SignedVolume| / SurfaceArea^1.5.
        // Inverted also treats a component at or below this ratio as UNKNOWN: its sign is not
        // evidence of a solid shell. Reusing this scale-free threshold gives both checks the
        // same answer without an absolute epsilon that changes meaning with mesh size.
        // The ratio is scale-free by construction (volume ~ L^3, area^1.5 ~ L^3), so one
        // number covers a 10 cm bolt and a 100 m cliff. Reference values: a cube is
        // 1/6^1.5 = 0.068, a sphere 0.094, a 1x1x0.01 slab 1.6e-3. The default sits just
        // below that slab, so a genuinely thin panel is not called a folded sheet.
        double MinVolumeRatio = 1.0e-3;

        // The z-fighting distance is derived from each measured mesh's largest bounds
        // dimension. This fraction is deliberately a geometric proxy for depth-buffer
        // precision, not a prediction of every camera's rasterization behavior.
        double ZFightPlaneExtentFraction = DefaultZFightPlaneExtentFraction;
        // Unit-normal alignment threshold. The detector accepts both parallel and
        // anti-parallel surfaces by comparing the absolute dot product.
        double ZFightNormalDotThreshold = DefaultZFightNormalDotThreshold;
        // Dimensionless proximity tolerance, multiplied by each mesh's bounding-sphere radius.
        double FloatingToleranceFraction = DefaultFloatingToleranceFraction;
    };

    // One triangle passed to the pure z-fighting detector. ComponentIndex comes from the
    // same edge-connected decomposition used by the winding checks; triangles with the same
    // component are never compared because this check is about duplicate surfaces from
    // separate shells/parts, not ordinary tessellation within one shell.
    struct FZFightTriangle
    {
        int32 TriangleID = -1;
        int32 ComponentIndex = -1;
        FVector3d V0 = FVector3d::ZeroVector;
        FVector3d V1 = FVector3d::ZeroVector;
        FVector3d V2 = FVector3d::ZeroVector;
        FVector3d Normal = FVector3d::ZeroVector;
        FVector3d BoundsMin = FVector3d::ZeroVector;
        FVector3d BoundsMax = FVector3d::ZeroVector;
        double Area = 0.0;
    };

    struct FZFightRegion
    {
        int32 Rank = 0;
        double OverlapArea = 0.0;
        double LargestPairOverlapArea = 0.0;
        int32 PairCount = 0;
        TArray<int32> TriangleIds;
        TArray<int32> ComponentIds;
        // Ranked, bounded evidence: at most ZFightMaxTopPairsPerRegion strongest pairs.
        struct FPairEvidence
        {
            int32 TriangleA = -1;
            int32 TriangleB = -1;
            int32 ComponentA = -1;
            int32 ComponentB = -1;
            double OverlapArea = 0.0;
            double PlaneDistance = 0.0;
            double AbsNormalDot = 0.0;
        };
        TArray<FPairEvidence> TopPairs;
    };

    struct FZFightAnalysis
    {
        bool bUnrunnable = false;
        FString UnrunnableCode;
        FString UnrunnableReason;

        int32 ModelTriangleCount = 0;
        int32 ValidTriangleCount = 0;
        int32 CandidatePairCount = 0;
        int32 ExactOverlapTestCount = 0;
        // Total references written across the fine grid and the coarse fallback grid.
        int32 GridReferenceCount = 0;
        int32 LargeTriangleCount = 0;
        // Coarse references actually inspected by the large-triangle fallback, including
        // references rejected by component, AABB, duplicate, or self filters.
        int32 LargeReferenceInspectCount = 0;
        int32 MaxLargeReferenceInspectCount = 0;
        int32 FightingPairCount = 0;
        int32 FightingTriangleCount = 0;
        double ModelExtent = 0.0;
        double PlaneDistanceEpsilon = 0.0;
        double NormalDotThreshold = 0.0;
        double GridCellSize = 0.0;
        double TotalOverlapArea = 0.0;
        TArray<FZFightRegion> Regions;
    };

    // One edge-connected triangle component inside a static mesh. Components are measured
    // separately because a model can contain many unwelded shells whose signed volumes cancel
    // at asset scope. BoundaryEdges and DegenerateTriangles are component-local, so an open or
    // degenerate shell is UNKNOWN even when another shell in the same asset is a valid solid.
    struct FComponentMeasurement
    {
        int32 Index = 0;
        int32 FirstTriangleID = -1;
        int32 TriangleCount = 0;
        int32 BoundaryEdges = 0;
        int32 DegenerateTriangles = 0;
        double SignedVolume = 0.0;
        double SurfaceArea = 0.0;

        // Implementation evidence used by the spatial pass; not serialized directly.
        TArray<int32> TriangleIDs;
        FBox Bounds = FBox(ForceInit);
        FVector Center = FVector::ZeroVector;

        bool IsClosed() const { return BoundaryEdges == 0; }

        // Scale-free and zero when there is no usable surface area. The caller treats zero,
        // non-finite values and ratios at the configured threshold as UNKNOWN.
        double VolumeRatio() const
        {
            if (SurfaceArea <= 0.0 || !FMath::IsFinite(SurfaceArea)
                || !FMath::IsFinite(SignedVolume))
            {
                return 0.0;
            }
            return FMath::Abs(SignedVolume) / FMath::Pow(SurfaceArea, 1.5);
        }
    };

    // Exact nearest distances between edge-connected components. AABB lower bounds are used
    // before triangle-tree queries, but Distance is always a triangle-to-triangle measurement.
    struct FComponentPairDistance
    {
        int32 ComponentA = -1;
        int32 ComponentB = -1;
        double Distance = TNumericLimits<double>::Max();
    };

    struct FSpatialMeasurement
    {
        bool bMeasured = false;
        FBox Bounds = FBox(ForceInit);
        double BoundingSphereRadius = 0.0;
        TArray<FComponentPairDistance> PairDistances;
    };

    struct FFloatingComponent
    {
        int32 ComponentIndex = -1;
        int32 ProximityIslandId = -1;
        int32 TriangleCount = 0;
        double SignedVolume = 0.0;
        FVector Center = FVector::ZeroVector;
        int32 NearestComponentIndex = -1;
        double NearestDistance = TNumericLimits<double>::Max();
        TArray<int32> OwningPartIndices;
        bool bSuppressed = false;
    };

    struct FFloatingReport
    {
        bool bMeasured = false;
        int32 ComponentCount = 0;
        int32 IslandCount = 0;
        int32 NonMainIslandCount = 0;
        int32 FloatingCount = 0;
        int32 FloatingComponentRows = 0;
        int32 SuppressedCount = 0;
        int32 UnsuppressedCount = 0;
        int32 LargestComponentIndex = -1;
        int32 LargestIslandTriangleCount = 0;
        double ToleranceFraction = 0.005;
        double Tolerance = 0.0;
        double BoundingSphereRadius = 0.0;
        // One canonical component index per measured component. The value is the smallest
        // component index in that proximity island and is stable across row granularity.
        TArray<int32> ComponentIslandIds;
        TArray<FFloatingComponent> Components;
    };

    struct FAnimationFloatingComponent
    {
        int32 ComponentIndex = -1;
        int32 ProximityIslandId = -1;
        int32 TriangleCount = 0;
        double SignedVolume = 0.0;
        FVector Center = FVector::ZeroVector;
        int32 NearestComponentIndex = -1;
        double FirstSeparation = 0.0;
        int32 FirstSeparatedFrame = -1;
        int32 WorstFrame = -1;
        double WorstSeparation = 0.0;
        bool bAlreadySeparatedAtBindPose = false;
        bool bSuppressed = false;
    };

    struct FAnimationFloatingReport
    {
        bool bMeasured = false;
        // FrameCount is the animation timeline span. SampledFrameIds are frame positions, so a
        // complete unit-stride scan contains FrameCount + 1 entries (including frame zero).
        int32 FrameCount = 0;
        int32 SampleStride = 1;
        int32 SampledFrameCount = 0;
        TArray<int32> SampledFrameIds;
        int32 BindIslandCount = 0;
        int32 BindNonMainIslandCount = 0;
        int32 BindFloatingCount = 0;
        int32 FloatingComponentRows = 0;
        int32 UniqueAnimationSeparatedIslandCount = 0;
        int32 SuppressedCount = 0;
        int32 UnsuppressedCount = 0;
        double ToleranceFraction = DefaultFloatingToleranceFraction;
        double BoundingSphereRadius = 0.0;
        double Tolerance = 0.0;
        TArray<int32> AllowedComponentIndices;
        // One canonical component index per measured component. The value is the smallest
        // component index in that proximity island and is stable across row granularity.
        TArray<int32> ComponentIslandIds;
        TArray<FAnimationFloatingComponent> Components;
        FString UnrunnableCode;
        FString UnrunnableReason;
    };

    // ---- Reading the asset ---------------------------------------------------------------

    // Which mesh inside the asset is measured. Mirrors geometry.create_from_static_mesh's
    // lodType vocabulary rather than inventing a second one, but is a local enum so this
    // header does not drag GeometryScripting into every translation unit that includes it.
    enum class ELodSource : uint8
    {
        // Highest-detail SOURCE mesh available (HiRes source model, else source LOD 0).
        // The default, and the only sane one for an orientation audit - see ELodSource
        // notes on RenderData below.
        MaxAvailable,
        HiResSourceModel,
        SourceModel,
        // The BUILT render mesh. It is split at every UV seam and hard-normal crease, so a
        // perfectly closed authored mesh reads as thousands of boundary edges and every
        // closed-only check goes not-applicable. Available because the caller may genuinely
        // want to know what shipped in the render data; never the default.
        RenderData
    };

    bool ParseLodSource(const FString& Token, ELodSource& OutSource);
    const TCHAR* LodSourceToString(ELodSource Source);

    struct FReadOptions
    {
        ELodSource LodSource = ELodSource::MaxAvailable;
        // The engine SILENTLY CLAMPS this to the available LOD count, so what is echoed is
        // what was asked for, not necessarily what was read. Same caveat as
        // geometry.create_from_static_mesh.
        int32 LodIndex = 0;
        bool bApplyBuildSettings = true;
        // Default true so the sweep measures the mesh AS IT IS PLACED. A build scale with an
        // odd number of negative axes mirrors the geometry, which really does ship inverted;
        // measuring without it would report that asset healthy.
        bool bUseBuildScale = true;
    };

    // ---- Per-asset measurement -----------------------------------------------------------

    struct FAssetMeasurement
    {
        // False = every selected check is Unrunnable for this asset.
        bool bMeasured = false;
        // ErrorCodes.h value + prose, set iff !bMeasured.
        FString UnrunnableCode;
        FString UnrunnableReason;

        GeometryUtils::FMeshHealth Health;
        // Not on FMeshHealth, and needed by ThinShell as the scale the volume is read
        // against. Comes from GeometryUtils::MeasureMeshOrientation, which is the same walk
        // MeasureMeshHealth delegates its orientation half to, so the two cannot disagree.
        double SurfaceArea = 0.0;

        // Every measured non-empty asset has one entry per edge-connected triangle component.
        // This is the evidence used by `inverted`; Health.SignedVolume remains the inexpensive
        // whole-mesh figure for callers that want to see the cancellation directly.
        TArray<FComponentMeasurement> Components;

        // Exact component-pair distances from the same component walk. Keeping this cache lets
        // EvaluateAsset apply a caller-selected fraction without touching the UObject again.
        FSpatialMeasurement Spatial;

        // Geometry copied from the same component walk. This is deliberately a plain value
        // array so EvaluateAsset can run over synthetic fixtures without an engine object.
        TArray<FZFightTriangle> ZFightTriangles;

        // Asset-level facts, read off the UStaticMesh rather than off the copied triangles.
        bool bHasSourceModel = false;
        int32 SourceLodCount = 0;
        FVector BuildScale = FVector::OneVector;
        bool bNaniteEnabled = false;

        // Number of BuildScale axes that are negative. Odd = the mesh is mirrored.
        int32 NegativeBuildScaleAxes() const
        {
            return (BuildScale.X < 0.0 ? 1 : 0) + (BuildScale.Y < 0.0 ? 1 : 0)
                 + (BuildScale.Z < 0.0 ? 1 : 0);
        }
        bool IsMirroredByBuildScale() const { return (NegativeBuildScaleAxes() % 2) == 1; }

        // |SignedVolume| / SurfaceArea^1.5, or 0 when there is no area to divide by.
        double VolumeRatio() const;
    };

    // ---- Findings --------------------------------------------------------------------------

    struct FFinding
    {
        FString AssetPath;
        FString AssetName;
        ECheck Check = ECheck::Inverted;
        EFindingStatus Status = EFindingStatus::Flagged;
        ESeverity Severity = ESeverity::Warning;
        // For Flagged, the check's own code; for Unrunnable, the reason the check could not
        // run (MESH_LOAD_FAILED, CONVERSION_FAILED, MEMORY_PRESSURE, ...).
        FString Code;
        FString Message;
        // The numbers the verdict was derived from, always.
        TSharedPtr<FJsonObject> Measurements;
    };

    // Per-check accounting. Two identities hold for every SELECTED check and are asserted by
    // the tests, because an asset that falls out of every bucket is exactly how a check
    // silently stops running:
    //     Applicable + NotApplicable == assetsExamined
    //     Flagged + Unrunnable + Clean == Applicable
    // Unselected checks keep an all-zero tally and are echoed with selected:false, so
    // "nothing was found" and "nobody looked" are never the same row.
    struct FCheckTally
    {
        int32 Applicable = 0;
        // The check does not mean anything for this asset (a closed-only check on an open
        // mesh, a build-scale check on an asset with no source model). `inverted` deliberately
        // keeps an open component applicable so it can report UNKNOWN/unrunnable. Distinct
        // from Unrunnable, which means the check DOES apply and could not be evaluated.
        int32 NotApplicable = 0;
        int32 Flagged = 0;
        int32 Unrunnable = 0;
        int32 Clean = 0;
    };

    // ---- Configuration ----------------------------------------------------------------------

    struct FConfig
    {
        uint32 SelectedChecks = 0;
        FThresholds Thresholds;
        FReadOptions Read;

        // Cap on findings[] ROWS. Per-check tallies are always exact; only the rows are
        // capped, and a capped page reports findingsTruncated and therefore cannot pass.
        int32 MaxFindings = 200;
        // Cap on asset paths collected into FReport::CleanAssets. 0 = do not collect them.
        int32 MaxCleanAssets = 0;
    };

    // ---- Report ------------------------------------------------------------------------------

    struct FReport
    {
        int32 AssetsMatched = 0;   // the whole match set, before offset/limit
        int32 AssetsExamined = 0;  // actually read in this call
        int32 AssetsFlagged = 0;
        int32 AssetsUnmeasured = 0;
        int32 Offset = 0;

        // (Offset + AssetsExamined) < AssetsMatched: the sweep did not reach the end of the
        // set. Same derivation as LevelAuditUtils.cpp:1048.
        bool bPageTruncated = false;
        bool bFindingsTruncated = false;
        int32 FindingsDropped = 0;
        // Either of the two above. This is the term the pass rule names.
        bool IsTruncated() const { return bPageTruncated || bFindingsTruncated; }

        TArray<FFinding> Findings;
        FCheckTally Tallies[CheckCount];
        TArray<FString> CleanAssets;
        // Statements about what this sweep could NOT see.
        TArray<FString> Caveats;

        int32 ErrorCount = 0;
        int32 WarningCount = 0;
        int32 UnrunnableCount = 0;

        int64 TotalTriangles = 0;
        int64 TotalVertices = 0;

        // The four terms the shared verdict reads, in one place, so the handler cannot pick a
        // different set of them than the response's own counters report.
        PinWrightAudit::FVerdict Verdict() const
        {
            PinWrightAudit::FVerdict Out;
            Out.ErrorCount = ErrorCount;
            Out.WarningCount = WarningCount;
            Out.UnrunnableCount = UnrunnableCount;
            Out.bTruncated = IsTruncated();
            return Out;
        }

        // The verdict, which is now literally level.audit's rather than a second copy of it:
        // PinWrightAudit::FVerdict::DerivePass. A finding below failOn does not fail the sweep,
        // but an unrunnable check or a truncated sweep does, regardless of failOn - because a
        // check that did not run is not a check that passed, and half a folder reported clean
        // is the answer this verb must never give. FailOn is "error" | "any" | "none".
        bool DerivePass(const FString& FailOn) const { return Verdict().DerivePass(FailOn); }
        bool DerivePass(PinWrightAudit::EFailOn FailOn) const { return Verdict().DerivePass(FailOn); }
    };

    // ---- Entry points --------------------------------------------------------------------------

    // Measure ONE already-resolved UStaticMesh. Loads no package; the caller owns Scratch and
    // is expected to REUSE it across the sweep (CopyMeshFromStaticMesh replaces the
    // destination's contents wholesale, so no clear step is needed between assets).
    // Never sends anything; failure lands in Out.UnrunnableCode / UnrunnableReason.
    void MeasureStaticMesh(UStaticMesh* Mesh, const FConfig& Config, UDynamicMesh* Scratch,
                           FAssetMeasurement& Out);

    // Measure the connected shells of an already-resolved dynamic mesh without mutating it.
    // Exposed so synthetic automation fixtures exercise the same component walk as the asset
    // sweep rather than manufacturing component volumes by hand.
    void MeasureComponents(UDynamicMesh* Mesh, TArray<FComponentMeasurement>& OutComponents);

    // Build exact pair distances by reusing MeasureComponents. AABB lower bounds prune queries;
    // the stored distances are from triangle geometry, not boxes. FixedLinkTolerance is used by
    // animation scans so every posed frame uses the tolerance derived from bind-pose model scale.
    void MeasureSpatialProximity(UDynamicMesh* Mesh, FSpatialMeasurement& OutSpatial,
                                 TArray<FComponentMeasurement>& InOutComponents,
                                 double ToleranceFraction = DefaultFloatingToleranceFraction,
                                 double FixedLinkTolerance = -1.0);

    void ClassifyFloatingComponents(const TArray<FComponentMeasurement>& Components,
                                    const FSpatialMeasurement& Spatial, double ToleranceFraction,
                                    FFloatingReport& OutReport,
                                    double FixedLinkTolerance = -1.0);

    // Evaluate real skeletal vertices under the sequence's actual per-frame bone transforms.
    // The topology is copied from the imported LOD and every vertex influence is applied for
    // each sampled pose; no rigid-component shortcut is used.
    void MeasureSkeletalAnimationFloating(USkeletalMesh* SkeletalMesh, UAnimSequence* Sequence,
                                          int32 SampleStride, double ToleranceFraction,
                                          FAnimationFloatingReport& OutReport);

    // Pure response serialization kept public to automation so the RPC contract (including
    // sparse frame identities) is covered without loading host assets or starting a job.
    TSharedPtr<FJsonObject> SerializeAnimationFloatingResult(
        const FString& MeshPath, const FString& AnimationPath,
        const FAnimationFloatingReport& Report, PinWrightAudit::EFailOn FailOn);
    void MeasureComponents(UDynamicMesh* Mesh, TArray<FComponentMeasurement>& OutComponents,
                           TArray<FZFightTriangle>& OutTriangles);

    // Pure broad-phase/narrow-phase detector. It receives triangle data and component ids from
    // the existing decomposition, so no package, world or UObject is needed by its tests.
    void AnalyzeZFighting(const TArray<FZFightTriangle>& Triangles, const FThresholds& Thresholds,
                          FZFightAnalysis& Out);

    // Turn ONE measurement into findings and tallies. Pure: no engine calls, no loads, no
    // UObject access - which is what makes it drivable from an automation test over synthetic
    // meshes, with no dependency on host content.
    void EvaluateAsset(const FString& AssetPath, const FString& AssetName,
                       const FAssetMeasurement& Measurement, const FConfig& Config,
                       FReport& InOutReport);

    // The sweep. Page is the already-ordered, already-paged match set; Matched is the size of
    // the WHOLE set the page came from, which is what makes bPageTruncated derivable.
    void Run(const TArray<FAssetData>& Page, int32 Offset, int32 Matched, const FConfig& Config,
             FReport& OutReport);
}
