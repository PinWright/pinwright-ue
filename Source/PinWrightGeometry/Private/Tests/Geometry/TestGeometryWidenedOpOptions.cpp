// Copyright (c) 2026 Alexander Penkin. MIT License.

// Tests for the six ops whose parameter sets were widened from a fraction of their engine
// options struct to (nearly) all of it: simplify_mesh, remesh_uniform, `uv mode=layout`,
// `uv mode=patch_builder`, the three symmetric booleans, and trim.
//
// Two questions per op, and they are different questions:
//
//  1. DOES THE DEFAULT PATH STILL DO WHAT IT DID? Answered structurally rather than by
//     geometry, in FGeometryOpsWidenedDefaultsMatchEngineTest below: every field of every
//     widened params struct is compared against the DEFAULT-CONSTRUCTED engine options struct
//     it maps onto. That is the whole "omitting a parameter reproduces today's behaviour"
//     contract in one place, and it is a stronger check than any before/after geometry
//     comparison could be - a geometry comparison can only show that two runs agree, while this
//     shows WHY they agree and names the field when they stop.
//
//     The one deliberate exception is FSimplifyMeshParams::Method, which is asserted to equal
//     the engine default precisely because it used to be forced to StandardQEM. That assertion
//     is the record of the fix: it fails if anyone re-pins it.
//
//  2. DOES A NON-DEFAULT VALUE REACH THE ENGINE? Answered per op by running the op twice and
//     showing the geometry differs. Each test picks the field whose effect is STRUCTURAL rather
//     than metric - a compaction, a triangle budget, a UV bound, an empty result - because a
//     "these two quality metrics disagree" assertion is a coin flip on a simple fixture and a
//     flaky test is worse than no test.
//
//     The exception is simplify's `method`, which has no structural effect and is the field the
//     audit was actually about. It is tested as a FOUR-WAY comparison - all four metrics run,
//     at least two must disagree - which fails only if the parameter reaches nothing at all,
//     and does not depend on any particular pair of metrics differing on any particular mesh.
#include "Misc/AutomationTest.h"
#include "Compat/EngineVersionCompat.h"

#include "Handlers/ErrorCodes.h"
#include "Handlers/Geometry/GeometryOps_Boolean.h"
#include "Handlers/Geometry/GeometryOps_Modeling.h"

#include "DynamicMesh/DynamicMesh3.h"
#include "DynamicMesh/DynamicMeshAttributeSet.h"
#include "DynamicMesh/DynamicMeshOverlay.h"
#include "UDynamicMesh.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

#include "GeometryScript/MeshBooleanFunctions.h"
#include "GeometryScript/MeshPrimitiveFunctions.h"
#include "GeometryScript/MeshRemeshFunctions.h"
#include "GeometryScript/MeshSimplifyFunctions.h"
#include "GeometryScript/MeshUVFunctions.h"

namespace
{
// Uniquely prefixed: this module builds with bUseUnity = true, so a plainly-named
// anonymous-namespace helper would collide with a sibling test TU once Unity merges them.

UDynamicMesh* WidenedOpsTest_NewMesh()
{
    return NewObject<UDynamicMesh>(GetTransientPackage());
}

// A lat-long sphere: curvature, a UV seam, and enough triangles that a simplifier has real
// choices to make. 20 x 32 steps is 1216 triangles - large enough that two simplification
// metrics have room to diverge, small enough that four runs of it stay fast.
UDynamicMesh* WidenedOpsTest_NewSphereMesh()
{
    UDynamicMesh* Mesh = WidenedOpsTest_NewMesh();
    FGeometryScriptPrimitiveOptions Options;
    UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendSphereLatLong(
        Mesh, Options, FTransform::Identity, 50.0f, 20, 32,
        EGeometryScriptPrimitiveOriginMode::Center, nullptr);
    return Mesh;
}

// Steps is AppendBox's EdgeVertices, NOT a subdivision count: FGridBoxMeshGenerator takes it as
// the number of VERTICES along each edge (MeshPrimitiveFunctions.cpp:236), so 0 and 2 both mean
// a plain 12-triangle box and 5 means a 5x5 vertex grid - four quads - on every face.
//
// The grid is load-bearing for the two simplify_output tests below and for nothing else. See
// WidenedOpsTest_SubdividedBoxSize / the comment on the boolean simplify test.
UDynamicMesh* WidenedOpsTest_NewBoxMesh(double Size, const FVector& Center, int32 Steps = 0)
{
    UDynamicMesh* Mesh = WidenedOpsTest_NewMesh();
    FGeometryScriptPrimitiveOptions Options;
    UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendBox(
        Mesh, Options, FTransform(Center), Size, Size, Size, Steps, Steps, Steps,
        EGeometryScriptPrimitiveOriginMode::Center, nullptr);
    return Mesh;
}

// ORDER-INDEPENDENT position digest. Deliberately not a per-index position comparison: two
// simplification runs produce different vertex ORDERS as well as different positions, and an
// indexed comparison would report a difference that is only a renumbering. Summing a
// coordinate-mixed term per vertex is invariant to order and still moves when any vertex moves.
double WidenedOpsTest_PositionDigest(UDynamicMesh* Mesh)
{
    double Digest = 0.0;
    Mesh->ProcessMesh([&Digest](const UE::Geometry::FDynamicMesh3& ReadMesh)
    {
        for (int32 VertexID : ReadMesh.VertexIndicesItr())
        {
            const FVector3d P = ReadMesh.GetVertex(VertexID);
            Digest += P.X * 1.0 + P.Y * 3.0 + P.Z * 7.0 + P.SquaredLength();
        }
    });
    return Digest;
}

// The same idea over UV channel 0. Returns 0 for a mesh with no such layer, which every caller
// here rules out first by asserting the element count.
double WidenedOpsTest_UVDigest(UDynamicMesh* Mesh, int32 UVChannel)
{
    double Digest = 0.0;
    Mesh->ProcessMesh([&Digest, UVChannel](const UE::Geometry::FDynamicMesh3& ReadMesh)
    {
        if (!ReadMesh.HasAttributes() || UVChannel >= ReadMesh.Attributes()->NumUVLayers())
        {
            return;
        }
        const UE::Geometry::FDynamicMeshUVOverlay* UVs = ReadMesh.Attributes()->GetUVLayer(UVChannel);
        for (int32 ElementID : UVs->ElementIndicesItr())
        {
            const FVector2f UV = UVs->GetElement(ElementID);
            Digest += UV.X * 1.0 + UV.Y * 3.0;
        }
    });
    return Digest;
}

FBox2D WidenedOpsTest_UVBounds(UDynamicMesh* Mesh, int32 UVChannel)
{
    FBox2D Bounds(ForceInit);
    Mesh->ProcessMesh([&Bounds, UVChannel](const UE::Geometry::FDynamicMesh3& ReadMesh)
    {
        if (!ReadMesh.HasAttributes() || UVChannel >= ReadMesh.Attributes()->NumUVLayers())
        {
            return;
        }
        const UE::Geometry::FDynamicMeshUVOverlay* UVs = ReadMesh.Attributes()->GetUVLayer(UVChannel);
        for (int32 ElementID : UVs->ElementIndicesItr())
        {
            const FVector2f UV = UVs->GetElement(ElementID);
            Bounds += FVector2D(UV.X, UV.Y);
        }
    });
    return Bounds;
}

int32 WidenedOpsTest_UVElementCount(UDynamicMesh* Mesh, int32 UVChannel)
{
    int32 Count = 0;
    Mesh->ProcessMesh([&Count, UVChannel](const UE::Geometry::FDynamicMesh3& ReadMesh)
    {
        if (!ReadMesh.HasAttributes() || UVChannel >= ReadMesh.Attributes()->NumUVLayers())
        {
            return;
        }
        Count = ReadMesh.Attributes()->GetUVLayer(UVChannel)->ElementCount();
    });
    return Count;
}

// Triangles whose CENTROID falls strictly inside an axis-aligned box. The honest form of "did the
// boolean actually remove the tool's region?", which the triangle-count delta cannot answer - see
// the boolean simplify test below.
int32 WidenedOpsTest_TrianglesInsideBox(UDynamicMesh* Mesh, const FVector3d& Min, const FVector3d& Max)
{
    int32 Count = 0;
    Mesh->ProcessMesh([&Count, &Min, &Max](const UE::Geometry::FDynamicMesh3& ReadMesh)
    {
        for (int32 TriangleID : ReadMesh.TriangleIndicesItr())
        {
            const FVector3d Centroid = ReadMesh.GetTriCentroid(TriangleID);
            if (Centroid.X > Min.X && Centroid.Y > Min.Y && Centroid.Z > Min.Z
                && Centroid.X < Max.X && Centroid.Y < Max.Y && Centroid.Z < Max.Z)
            {
                ++Count;
            }
        }
    });
    return Count;
}
}

// ============================================================================
// The default path, checked against the engine rather than against itself
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryOpsWidenedDefaultsMatchEngineTest,
    "PinWright.Geometry.Ops.WidenedOptions.DefaultsMatchTheEngineStructDefaults",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGeometryOpsWidenedDefaultsMatchEngineTest::RunTest(const FString& Parameters)
{
    // simplify_mesh -------------------------------------------------------------
    {
        const GeometryOps::FSimplifyMeshParams Params;
        const FGeometryScriptSimplifyMeshOptions Engine;

        // THE assertion this file exists for. The op used to pin Method to StandardQEM, which is
        // the engine's first enumerator and not its default; re-pinning it fails here.
        TestTrue(TEXT("simplify_mesh defaults Method to the engine's AttributeAware, not StandardQEM"),
            Params.Method == GeometryOps::ESimplifyMethod::AttributeAware
            && Engine.Method == EGeometryScriptRemoveMeshSimplificationType::AttributeAware);

        TestEqual(TEXT("simplify allow_seam_collapse"), Params.bAllowSeamCollapse, Engine.bAllowSeamCollapse);
        TestEqual(TEXT("simplify allow_seam_smoothing"), Params.bAllowSeamSmoothing, Engine.bAllowSeamSmoothing);
        TestEqual(TEXT("simplify allow_seam_splits"), Params.bAllowSeamSplits, Engine.bAllowSeamSplits);
        TestEqual(TEXT("simplify preserve_vertex_positions"), Params.bPreserveVertexPositions, Engine.bPreserveVertexPositions);
        TestEqual(TEXT("simplify retain_quadric_memory"), Params.bRetainQuadricMemory, Engine.bRetainQuadricMemory);
        TestEqual(TEXT("simplify auto_compact"), Params.bAutoCompact, Engine.bAutoCompact);
        // The seven below joined FGeometryScriptSimplifyMeshOptions in UE 5.8, together with
        // EGeometryScriptMeshSimplificationQuadricVariant. On 5.3-5.7 the engine struct is exactly
        // the six fields asserted above plus Method, so there is no engine default to compare
        // these local ones against - and SimplifyMesh warns rather than writing them there.
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 8, 0)
        TestEqual(TEXT("simplify regularize_weight"), Params.RegularizeWeight, static_cast<double>(Engine.RegularizeWeight));
        TestTrue(TEXT("simplify quadric_variant"),
            Params.QuadricVariant == GeometryOps::ESimplifyQuadricVariant::PlaneQuadric
            && Engine.QuadricVariant == EGeometryScriptMeshSimplificationQuadricVariant::PlaneQuadric);
        TestEqual(TEXT("simplify normal_attribute_weight"), Params.NormalAttributeWeight, static_cast<double>(Engine.NormalAttributeWeight));
        TestEqual(TEXT("simplify tangent_attribute_weight"), Params.TangentAttributeWeight, static_cast<double>(Engine.TangentAttributeWeight));
        TestEqual(TEXT("simplify color_attribute_weight"), Params.ColorAttributeWeight, static_cast<double>(Engine.ColorAttributeWeight));
        TestEqual(TEXT("simplify texcoord_attribute_weight"), Params.TexCoordAttributeWeight, static_cast<double>(Engine.TexCoordAttributeWeight));
        TestEqual(TEXT("simplify scale_correction"), Params.ScaleCorrection, static_cast<double>(Engine.ScaleCorrection));
#endif
    }

    // remesh_uniform ------------------------------------------------------------
    {
        const GeometryOps::FRemeshUniformParams Params;
        const FGeometryScriptRemeshOptions Engine;
        const FGeometryScriptUniformRemeshOptions EngineUniform;

        TestEqual(TEXT("remesh discard_attributes"), Params.bDiscardAttributes, Engine.bDiscardAttributes);
        TestEqual(TEXT("remesh reproject_to_input_mesh"), Params.bReprojectToInputMesh, Engine.bReprojectToInputMesh);
        TestTrue(TEXT("remesh smoothing_type"),
            Params.SmoothingType == GeometryOps::ERemeshSmoothingType::Mixed
            && Engine.SmoothingType == EGeometryScriptRemeshSmoothingType::Mixed);
        TestEqual(TEXT("remesh smoothing_rate"), Params.SmoothingRate, static_cast<double>(Engine.SmoothingRate));
        TestTrue(TEXT("remesh mesh_boundary_constraint"),
            Params.MeshBoundaryConstraint == GeometryOps::ERemeshEdgeConstraint::Free
            && Engine.MeshBoundaryConstraint == EGeometryScriptRemeshEdgeConstraintType::Free);
        TestTrue(TEXT("remesh group_boundary_constraint"),
            Params.GroupBoundaryConstraint == GeometryOps::ERemeshEdgeConstraint::Free
            && Engine.GroupBoundaryConstraint == EGeometryScriptRemeshEdgeConstraintType::Free);
        TestTrue(TEXT("remesh material_boundary_constraint"),
            Params.MaterialBoundaryConstraint == GeometryOps::ERemeshEdgeConstraint::Free
            && Engine.MaterialBoundaryConstraint == EGeometryScriptRemeshEdgeConstraintType::Free);
        TestEqual(TEXT("remesh allow_flips"), Params.bAllowFlips, Engine.bAllowFlips);
        TestEqual(TEXT("remesh allow_splits"), Params.bAllowSplits, Engine.bAllowSplits);
        TestEqual(TEXT("remesh allow_collapses"), Params.bAllowCollapses, Engine.bAllowCollapses);
        TestEqual(TEXT("remesh prevent_normal_flips"), Params.bPreventNormalFlips, Engine.bPreventNormalFlips);
        TestEqual(TEXT("remesh prevent_tiny_triangles"), Params.bPreventTinyTriangles, Engine.bPreventTinyTriangles);
        TestEqual(TEXT("remesh use_full_remesh_passes"), Params.bUseFullRemeshPasses, Engine.bUseFullRemeshPasses);
        TestEqual(TEXT("remesh iterations"), Params.RemeshIterations, Engine.RemeshIterations);
        TestEqual(TEXT("remesh auto_compact"), Params.bAutoCompact, Engine.bAutoCompact);
        TestTrue(TEXT("remesh target_type"),
            Params.TargetType == GeometryOps::ERemeshTargetType::TriangleCount
            && EngineUniform.TargetType == EGeometryScriptUniformRemeshTargetType::TriangleCount);
        TestEqual(TEXT("remesh target_triangle_count"), Params.TargetTriangleCount, EngineUniform.TargetTriangleCount);
        TestEqual(TEXT("remesh target_edge_length"), Params.TargetEdgeLength, static_cast<double>(EngineUniform.TargetEdgeLength));
    }

    // uv mode=layout ------------------------------------------------------------
    // FGeometryScriptLayoutUVsOptions (and LayoutMeshUVs with it) arrived in UE 5.5. On 5.4 the
    // engine publishes only FGeometryScriptRepackUVsOptions, whose two fields are a resolution
    // and an island-rotation switch, so there is no per-field engine default to compare
    // FLayoutUVParams against and GeometryOps::LayoutUV takes its RepackMeshUVs branch.
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 5, 0)
    {
        const GeometryOps::FLayoutUVParams Params;
        const FGeometryScriptLayoutUVsOptions Engine;

        TestTrue(TEXT("layout layout_type"),
            Params.LayoutType == GeometryOps::EUVLayoutType::Repack
            && Engine.LayoutType == EGeometryScriptUVLayoutType::Repack);
        // 1024, not the 512 the field used to carry from a struct no `uv` mode calls.
        TestEqual(TEXT("layout texture_resolution"), Params.TextureResolution, Engine.TextureResolution);
        TestEqual(TEXT("layout layout_scale"), Params.Scale, static_cast<double>(Engine.Scale));
        TestTrue(TEXT("layout translation"), Params.Translation == Engine.Translation);
        TestEqual(TEXT("layout preserve_scale"), Params.bPreserveScale, Engine.bPreserveScale);
        TestEqual(TEXT("layout preserve_rotation"), Params.bPreserveRotation, Engine.bPreserveRotation);
        TestEqual(TEXT("layout allow_flips"), Params.bAllowFlips, Engine.bAllowFlips);
        TestEqual(TEXT("layout enable_udim_layout"), Params.bEnableUDIMLayout, Engine.bEnableUDIMLayout);
    }
#else
    {
        const GeometryOps::FLayoutUVParams Params;
        const FGeometryScriptRepackUVsOptions Engine;

        TestTrue(TEXT("layout layout_type defaults to the only mode 5.4 can run"),
            Params.LayoutType == GeometryOps::EUVLayoutType::Repack);
        // The one field that is read inverted, so an untouched call is the engine's own default.
        TestEqual(TEXT("layout preserve_rotation"), !Params.bPreserveRotation, Engine.bOptimizeIslandRotation);
        // TextureResolution is deliberately NOT the repack struct's default: FLayoutUVParams
        // carries 1024, the value `uv mode=layout` has always sent, while
        // FGeometryScriptRepackUVsOptions defaults to 512. The op writes ours over it.
        TestEqual(TEXT("layout texture_resolution is the op's 1024"), Params.TextureResolution, 1024);
    }
#endif

    // uv mode=patch_builder -----------------------------------------------------
    {
        const GeometryOps::FPatchBuilderUVParams Params;
        const FGeometryScriptPatchBuilderOptions Engine;

        TestEqual(TEXT("patch initial_patch_count"), Params.InitialPatchCount, Engine.InitialPatchCount);
        TestEqual(TEXT("patch min_patch_size"), Params.MinPatchSize, Engine.MinPatchSize);
        TestEqual(TEXT("patch patch_curvature_alignment_weight"),
            Params.PatchCurvatureAlignmentWeight, static_cast<double>(Engine.PatchCurvatureAlignmentWeight));
        TestEqual(TEXT("patch patch_merging_metric_thresh"),
            Params.PatchMergingMetricThresh, static_cast<double>(Engine.PatchMergingMetricThresh));
        TestEqual(TEXT("patch patch_merging_angle_thresh"),
            Params.PatchMergingAngleThresh, static_cast<double>(Engine.PatchMergingAngleThresh));
        TestEqual(TEXT("patch exp_map_normal_smoothing_rounds"),
            Params.ExpMapNormalSmoothingRounds, Engine.ExpMapOptions.NormalSmoothingRounds);
        TestEqual(TEXT("patch exp_map_normal_smoothing_alpha"),
            Params.ExpMapNormalSmoothingAlpha, static_cast<double>(Engine.ExpMapOptions.NormalSmoothingAlpha));
        TestEqual(TEXT("patch respect_input_groups"), Params.bRespectInputGroups, Engine.bRespectInputGroups);
        TestEqual(TEXT("patch auto_pack"), Params.bAutoPack, Engine.bAutoPack);
        // 512 here and 1024 on layout above: two different engine structs feeding two different
        // packers. Matching them up would be inventing a default, so the two assertions sit
        // deliberately close together.
        TestEqual(TEXT("patch packing_target_image_width"),
            Params.PackingTargetImageWidth, Engine.PackingOptions.TargetImageWidth);
        TestEqual(TEXT("patch packing_optimize_island_rotation"),
            Params.bPackingOptimizeIslandRotation, Engine.PackingOptions.bOptimizeIslandRotation);
    }

    // union / subtract / intersection, and trim ---------------------------------
    {
        const GeometryOps::FBooleanParams BoolParams;
        const GeometryOps::FTrimParams TrimParams;
        const FGeometryScriptMeshBooleanOptions Engine;

        TestEqual(TEXT("boolean fill_holes"), BoolParams.bFillHoles, Engine.bFillHoles);
        // bAllowEmptyResult exists on the engine struct only from UE 5.4; before that the engine
        // always refuses an empty result, which is what the op's default false already means.
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 4, 0)
        TestEqual(TEXT("boolean allow_empty_result"), BoolParams.bAllowEmptyResult, Engine.bAllowEmptyResult);
#endif
        // OutputTransformSpace exists on both structs only from UE 5.6; before that the engine
        // has no selector and GeometryOps::FBooleanParams compiles the field out to match.
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 6, 0)
        TestTrue(TEXT("boolean output space"),
            BoolParams.OutputTransformSpace == EGeometryScriptBooleanOutputSpace::TargetTransformSpace
            && Engine.OutputTransformSpace == EGeometryScriptBooleanOutputSpace::TargetTransformSpace);
#endif

        // The field the two structs used to DISAGREE on: the boolean side pinned it false with
        // no front-end able to reach it, while trim ran the engine's true. Both now follow the
        // engine, so this pair of assertions is what fails if either surface is re-pinned.
        TestEqual(TEXT("boolean simplify_output matches the engine"), BoolParams.bSimplifyOutput, Engine.bSimplifyOutput);
        TestEqual(TEXT("trim simplify_output matches the engine"), TrimParams.bSimplifyOutput, Engine.bSimplifyOutput);
        TestTrue(TEXT("and the engine's own default really is ON, which is what the two are following"),
            Engine.bSimplifyOutput);
        TestEqual(TEXT("trim fill_holes"), TrimParams.bFillHoles, Engine.bFillHoles);
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 4, 0)
        TestEqual(TEXT("trim allow_empty_result"), TrimParams.bAllowEmptyResult, Engine.bAllowEmptyResult);
#endif
    }

    return true;
}

// ============================================================================
// simplify_mesh
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryOpsWidenedSimplifyMethodReachesEngineTest,
    "PinWright.Geometry.Ops.WidenedOptions.SimplifyMethodReachesTheEngine",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGeometryOpsWidenedSimplifyMethodReachesEngineTest::RunTest(const FString& Parameters)
{
    // A four-way comparison rather than a chosen pair. `method` selects an error METRIC, so its
    // effect is not structural and any single pair of metrics might agree by luck on any given
    // fixture; requiring only that the four are not ALL identical fails exactly when the
    // parameter reaches nothing, which is the regression worth catching.
    // AttributeAwareV2 has no engine target before UE 5.8 and SimplifyMesh refuses it there
    // rather than substituting AttributeAware, so on those engines the comparison is three-way
    // and the refusal is asserted below instead.
    const GeometryOps::ESimplifyMethod Methods[] = {
        GeometryOps::ESimplifyMethod::StandardQEM,
        GeometryOps::ESimplifyMethod::VolumePreserving,
        GeometryOps::ESimplifyMethod::AttributeAware,
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 8, 0)
        GeometryOps::ESimplifyMethod::AttributeAwareV2,
#endif
    };

    TArray<double> Digests;
    for (GeometryOps::ESimplifyMethod Method : Methods)
    {
        TStrongObjectPtr<UDynamicMesh> Mesh(WidenedOpsTest_NewSphereMesh());

        GeometryOps::FSimplifyMeshParams Params;
        Params.TargetPercentage = 20.0;
        Params.Method = Method;

        const GeometryOps::FOpResult Op = GeometryOps::SimplifyMesh(Mesh.Get(), Params);
        TestTrue(TEXT("every simplification method runs"), Op.bSuccess);
        TestTrue(TEXT("and every one of them removes triangles"), Op.TrianglesAfter < Op.TrianglesBefore);

        Digests.Add(WidenedOpsTest_PositionDigest(Mesh.Get()));
    }

    bool bAnyDiffer = false;
    for (int32 Index = 1; Index < Digests.Num(); ++Index)
    {
        bAnyDiffer |= !FMath::IsNearlyEqual(Digests[0], Digests[Index], 1e-6);
    }
    TestTrue(TEXT("the simplification methods do not all produce identical geometry - "
                  "if they do, `method` is reaching nothing"), bAnyDiffer);

#if !UE_VERSION_NEWER_THAN_OR_EQUAL(5, 8, 0)
    // The fourth method on this engine: refused by name, not quietly served as AttributeAware.
    {
        TStrongObjectPtr<UDynamicMesh> Mesh(WidenedOpsTest_NewSphereMesh());

        GeometryOps::FSimplifyMeshParams Params;
        Params.TargetPercentage = 20.0;
        Params.Method = GeometryOps::ESimplifyMethod::AttributeAwareV2;

        const GeometryOps::FOpResult Op = GeometryOps::SimplifyMesh(Mesh.Get(), Params);
        TestFalse(TEXT("AttributeAwareV2 is refused on an engine that has no such metric"), Op.bSuccess);
        TestEqual(TEXT("the refusal is UNSUPPORTED_ENGINE_VERSION"),
            Op.ErrorCode, FString(ErrorCodes::ERR_UNSUPPORTED_ENGINE_VERSION));
        TestTrue(TEXT("the refusal names the method and the engine it needs"),
            Op.ErrorMessage.Contains(TEXT("AttributeAwareV2")) && Op.ErrorMessage.Contains(TEXT("5.8")));
    }
#endif

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryOpsWidenedSimplifyAutoCompactTest,
    "PinWright.Geometry.Ops.WidenedOptions.SimplifyAutoCompactReachesTheEngine",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGeometryOpsWidenedSimplifyAutoCompactTest::RunTest(const FString& Parameters)
{
    // The structural half of the simplify widening. Compaction is not a quality judgement: with
    // it on the index space has no gaps and MaxVertexID == VertexCount, with it off the collapsed
    // vertices leave holes behind. Nothing about the fixture can make the two agree.
    auto RunWithCompact = [this](bool bAutoCompact)
    {
        TStrongObjectPtr<UDynamicMesh> Mesh(WidenedOpsTest_NewSphereMesh());

        GeometryOps::FSimplifyMeshParams Params;
        Params.TargetPercentage = 20.0;
        Params.bAutoCompact = bAutoCompact;

        const GeometryOps::FOpResult Op = GeometryOps::SimplifyMesh(Mesh.Get(), Params);
        TestTrue(TEXT("simplify runs either way"), Op.bSuccess);

        bool bCompact = false;
        Mesh->ProcessMesh([&bCompact](const UE::Geometry::FDynamicMesh3& ReadMesh)
        {
            bCompact = ReadMesh.IsCompact();
        });
        return bCompact;
    };

    TestTrue(TEXT("the default path compacts, matching the engine default"), RunWithCompact(true));
    TestFalse(TEXT("auto_compact=false leaves gaps in the index space, so the flag reaches the engine"),
        RunWithCompact(false));

    return true;
}

// ============================================================================
// remesh_uniform
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryOpsWidenedRemeshTargetTypeTest,
    "PinWright.Geometry.Ops.WidenedOptions.RemeshTargetTypeAndEdgeLengthReachTheEngine",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGeometryOpsWidenedRemeshTargetTypeTest::RunTest(const FString& Parameters)
{
    // target_type is the field that decides which of the two goals the remesher pursues, and the
    // two goals here are set far enough apart that no tuning can make them meet: a 300-triangle
    // budget on a 50uu-radius sphere against an explicit 40uu edge length, which is comparable
    // to the sphere's own radius and so drives the mesh to its coarsest legal form.
    TStrongObjectPtr<UDynamicMesh> ByCount(WidenedOpsTest_NewSphereMesh());
    TStrongObjectPtr<UDynamicMesh> ByEdgeLength(WidenedOpsTest_NewSphereMesh());

    GeometryOps::FRemeshUniformParams CountParams;
    CountParams.TargetTriangleCount = 300;
    const GeometryOps::FOpResult CountOp = GeometryOps::RemeshUniform(ByCount.Get(), CountParams);
    TestTrue(TEXT("remesh by triangle count runs"), CountOp.bSuccess);

    GeometryOps::FRemeshUniformParams EdgeParams;
    // Left at the same budget on purpose: if target_type were ignored, this run would be the
    // previous one and the counts would match.
    EdgeParams.TargetTriangleCount = 300;
    EdgeParams.TargetType = GeometryOps::ERemeshTargetType::TargetEdgeLength;
    EdgeParams.TargetEdgeLength = 40.0;
    const GeometryOps::FOpResult EdgeOp = GeometryOps::RemeshUniform(ByEdgeLength.Get(), EdgeParams);
    TestTrue(TEXT("remesh by edge length runs"), EdgeOp.bSuccess);

    TestNotEqual(TEXT("target_type=target_edge_length produces a different mesh than the same "
                      "request under the triangle-count goal, so both it and target_edge_length "
                      "reach the engine"),
        EdgeOp.TrianglesAfter, CountOp.TrianglesAfter);

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryOpsWidenedRemeshDiscardAttributesTest,
    "PinWright.Geometry.Ops.WidenedOptions.RemeshDiscardAttributesReachesTheEngine",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGeometryOpsWidenedRemeshDiscardAttributesTest::RunTest(const FString& Parameters)
{
    // The second remesh field with a structural effect, and the one most likely to be reached for
    // by accident: it silently throws away the UV layout, which is exactly the kind of loss that
    // has to be an explicit request rather than a side effect of remeshing.
    auto UVElementsAfterRemesh = [this](bool bDiscardAttributes)
    {
        TStrongObjectPtr<UDynamicMesh> Mesh(WidenedOpsTest_NewSphereMesh());
        TestTrue(TEXT("the sphere fixture starts with UVs"),
            WidenedOpsTest_UVElementCount(Mesh.Get(), 0) > 0);

        GeometryOps::FRemeshUniformParams Params;
        Params.TargetTriangleCount = 400;
        Params.bDiscardAttributes = bDiscardAttributes;

        const GeometryOps::FOpResult Op = GeometryOps::RemeshUniform(Mesh.Get(), Params);
        TestTrue(TEXT("remesh runs either way"), Op.bSuccess);
        return WidenedOpsTest_UVElementCount(Mesh.Get(), 0);
    };

    TestTrue(TEXT("the default path keeps the UV layout"), UVElementsAfterRemesh(false) > 0);
    TestEqual(TEXT("discard_attributes=true throws the UV layout away, so the flag reaches the engine"),
        UVElementsAfterRemesh(true), 0);

    return true;
}

// ============================================================================
// uv mode=layout
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryOpsWidenedLayoutUVScaleTest,
    "PinWright.Geometry.Ops.WidenedOptions.LayoutUVScaleAndTypeReachTheEngine",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGeometryOpsWidenedLayoutUVScaleTest::RunTest(const FString& Parameters)
{
    // layout_scale is applied AFTER packing, so its effect is a pure scaling of the packed
    // bounds - measurable to a tolerance rather than merely "different". The default repack puts
    // everything inside the unit square, which is the first assertion and the one that pins the
    // default path.
    FBox2D DefaultBounds(ForceInit);
    {
        TStrongObjectPtr<UDynamicMesh> Mesh(WidenedOpsTest_NewSphereMesh());

        const GeometryOps::FLayoutUVParams Params;
        const GeometryOps::FOpResult Op = GeometryOps::LayoutUV(Mesh.Get(), Params);
        TestTrue(TEXT("layout runs on its defaults"), Op.bSuccess);

        DefaultBounds = WidenedOpsTest_UVBounds(Mesh.Get(), 0);
        TestTrue(TEXT("the default repack fits the islands inside the unit square"),
            DefaultBounds.Min.X >= -KINDA_SMALL_NUMBER && DefaultBounds.Min.Y >= -KINDA_SMALL_NUMBER
            && DefaultBounds.Max.X <= 1.0 + KINDA_SMALL_NUMBER
            && DefaultBounds.Max.Y <= 1.0 + KINDA_SMALL_NUMBER);
    }

    // The post-pack scale and the layout MODE are both LayoutMeshUVs vocabulary, added in UE 5.5.
    // On 5.4 GeometryOps::LayoutUV runs RepackMeshUVs, which has neither: the scale is reported as
    // a warning and a non-repack mode is refused by name. Those are the assertions below the #else,
    // and they are the same contract stated for the engine that cannot honour it.
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 5, 0)
    {
        TStrongObjectPtr<UDynamicMesh> Mesh(WidenedOpsTest_NewSphereMesh());

        GeometryOps::FLayoutUVParams Params;
        Params.Scale = 0.5;
        const GeometryOps::FOpResult Op = GeometryOps::LayoutUV(Mesh.Get(), Params);
        TestTrue(TEXT("layout runs with a post-pack scale"), Op.bSuccess);

        // MEASURED AS A SIZE, NOT AS A CORNER, because the engine does not scale about the UV
        // origin. FUVLayoutOp applies Scale and Translation in "external" UV space, which is the
        // overlay's space with V FLIPPED - ExternalUVToInternalUV(UV) = (UV.X, 1 - UV.Y),
        // UVLayoutOp.cpp:19-27 - so a 0.5 scale of a unit-square pack lands U in [0, 0.5] and V
        // in [0.5, 1], not in [0, 0.5]. An assertion on Max.Y therefore fails on a scale that
        // reached the engine perfectly, which is exactly what it used to do here. The SIZE
        // halving is the frame-independent statement and is just as strong: a dropped Scale
        // leaves the pack at its full unit-square size and fails this on both axes.
        const FBox2D Bounds = WidenedOpsTest_UVBounds(Mesh.Get(), 0);
        const FVector2D Scaled = Bounds.GetSize();
        const FVector2D Expected = DefaultBounds.GetSize() * 0.5;
        TestTrue(*FString::Printf(
                TEXT("layout_scale=0.5 halves the packed bounds, so it reaches the engine ")
                TEXT("(%.6f x %.6f against the expected %.6f x %.6f)"),
                Scaled.X, Scaled.Y, Expected.X, Expected.Y),
            FMath::IsNearlyEqual(Scaled.X, Expected.X, 1e-3)
            && FMath::IsNearlyEqual(Scaled.Y, Expected.Y, 1e-3));

        // The V-flip itself, pinned rather than merely described: the scaled pack sits in the
        // UPPER half of V. This is the assertion that would notice the engine changing its mind
        // about which corner Scale is anchored to.
        TestTrue(*FString::Printf(
                TEXT("and anchors V at 1 rather than 0 (V spans %.6f..%.6f)"),
                Bounds.Min.Y, Bounds.Max.Y),
            Bounds.Min.Y >= 0.5 - KINDA_SMALL_NUMBER && Bounds.Max.Y <= 1.0 + KINDA_SMALL_NUMBER);
    }

    // layout_type is the field that decides what the op does at all - stack overlaps the islands
    // where repack does not - so the two cannot produce the same UVs on a multi-island mesh.
    {
        TStrongObjectPtr<UDynamicMesh> Repacked(WidenedOpsTest_NewSphereMesh());
        TStrongObjectPtr<UDynamicMesh> Stacked(WidenedOpsTest_NewSphereMesh());

        GeometryOps::FLayoutUVParams RepackParams;
        TestTrue(TEXT("repack runs"), GeometryOps::LayoutUV(Repacked.Get(), RepackParams).bSuccess);

        GeometryOps::FLayoutUVParams StackParams;
        StackParams.LayoutType = GeometryOps::EUVLayoutType::Stack;
        TestTrue(TEXT("stack runs"), GeometryOps::LayoutUV(Stacked.Get(), StackParams).bSuccess);

        TestFalse(TEXT("layout_type=stack does not produce the repacked UVs, so it reaches the engine"),
            FMath::IsNearlyEqual(
                WidenedOpsTest_UVDigest(Repacked.Get(), 0),
                WidenedOpsTest_UVDigest(Stacked.Get(), 0), 1e-6));
    }
#else
    {
        TStrongObjectPtr<UDynamicMesh> Mesh(WidenedOpsTest_NewSphereMesh());

        GeometryOps::FLayoutUVParams Params;
        Params.Scale = 0.5;
        const GeometryOps::FOpResult Op = GeometryOps::LayoutUV(Mesh.Get(), Params);
        TestTrue(TEXT("layout still repacks on an engine with no post-pack scale"), Op.bSuccess);
        TestTrue(TEXT("and says so, rather than dropping layout_scale silently"),
            Op.Warnings.Num() > 0);
    }

    {
        TStrongObjectPtr<UDynamicMesh> Mesh(WidenedOpsTest_NewSphereMesh());

        GeometryOps::FLayoutUVParams StackParams;
        StackParams.LayoutType = GeometryOps::EUVLayoutType::Stack;
        const GeometryOps::FOpResult Op = GeometryOps::LayoutUV(Mesh.Get(), StackParams);
        TestFalse(TEXT("layout_type=stack is refused rather than silently repacked"), Op.bSuccess);
        TestEqual(TEXT("and is refused as an engine-version limit"),
            Op.ErrorCode, FString(ErrorCodes::ERR_UNSUPPORTED_ENGINE_VERSION));
    }
#endif

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryOpsWidenedLayoutRejectsBadChannelTest,
    "PinWright.Geometry.Ops.WidenedOptions.LayoutAndPatchBuilderRejectAnOutOfRangeChannel",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGeometryOpsWidenedLayoutRejectsBadChannelTest::RunTest(const FString& Parameters)
{
    // Both new ops carry ProjectUV's channel guard VERBATIM, message included. That matters
    // because the .pwmodel compiler used to apply the guard itself in a branch shared by three
    // modes: moving it into the ops has to leave the one failure with exactly one text.
    TStrongObjectPtr<UDynamicMesh> Mesh(WidenedOpsTest_NewSphereMesh());

    GeometryOps::FLayoutUVParams LayoutParams;
    LayoutParams.UVChannel = 9;
    const GeometryOps::FOpResult LayoutOp = GeometryOps::LayoutUV(Mesh.Get(), LayoutParams);
    TestFalse(TEXT("layout refuses a channel above 7"), LayoutOp.bSuccess);
    TestEqual(TEXT("and refuses it as INVALID_ARGUMENT"),
        LayoutOp.ErrorCode, FString(ErrorCodes::ERR_INVALID_ARGUMENT));

    GeometryOps::FPatchBuilderUVParams PatchParams;
    PatchParams.UVChannel = 9;
    const GeometryOps::FOpResult PatchOp = GeometryOps::AutoUVPatchBuilder(Mesh.Get(), PatchParams);
    TestFalse(TEXT("patch_builder refuses a channel above 7"), PatchOp.bSuccess);
    TestEqual(TEXT("and the two ops word that refusal identically"),
        PatchOp.ErrorMessage, LayoutOp.ErrorMessage);

    return true;
}

// ============================================================================
// uv mode=patch_builder
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryOpsWidenedPatchBuilderOptionsTest,
    "PinWright.Geometry.Ops.WidenedOptions.PatchBuilderOptionsReachTheEngine",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGeometryOpsWidenedPatchBuilderOptionsTest::RunTest(const FString& Parameters)
{
    // auto_pack is the structural knob: with it on the patches are repacked into the unit square,
    // with it off the raw ExpMap flattening is left where the solver put it. The UV VALUES
    // therefore cannot match, whatever the patch segmentation turned out to be.
    TStrongObjectPtr<UDynamicMesh> Packed(WidenedOpsTest_NewSphereMesh());
    TStrongObjectPtr<UDynamicMesh> Unpacked(WidenedOpsTest_NewSphereMesh());

    const GeometryOps::FPatchBuilderUVParams PackedParams;
    const GeometryOps::FOpResult PackedOp = GeometryOps::AutoUVPatchBuilder(Packed.Get(), PackedParams);
    TestTrue(TEXT("patch_builder runs on its defaults"), PackedOp.bSuccess);
    TestTrue(TEXT("and writes UV elements"), WidenedOpsTest_UVElementCount(Packed.Get(), 0) > 0);

    GeometryOps::FPatchBuilderUVParams UnpackedParams;
    UnpackedParams.bAutoPack = false;
    const GeometryOps::FOpResult UnpackedOp = GeometryOps::AutoUVPatchBuilder(Unpacked.Get(), UnpackedParams);
    TestTrue(TEXT("patch_builder runs with packing off"), UnpackedOp.bSuccess);

    TestFalse(TEXT("auto_pack=false does not produce the packed UVs, so the flag reaches the engine"),
        FMath::IsNearlyEqual(
            WidenedOpsTest_UVDigest(Packed.Get(), 0),
            WidenedOpsTest_UVDigest(Unpacked.Get(), 0), 1e-6));

    // The segmentation half. min_patch_size raised to a large fraction of the mesh forces the
    // builder to merge aggressively, which changes the seam set and so the UV element count -
    // a count, not a metric, so there is nothing here to be lucky about.
    TStrongObjectPtr<UDynamicMesh> Coarse(WidenedOpsTest_NewSphereMesh());
    GeometryOps::FPatchBuilderUVParams CoarseParams;
    CoarseParams.InitialPatchCount = 2;
    CoarseParams.MinPatchSize = 200;
    const GeometryOps::FOpResult CoarseOp = GeometryOps::AutoUVPatchBuilder(Coarse.Get(), CoarseParams);
    TestTrue(TEXT("patch_builder runs with a coarse segmentation"), CoarseOp.bSuccess);

    TestNotEqual(TEXT("initial_patch_count / min_patch_size change the seam set, so they reach the engine"),
        WidenedOpsTest_UVElementCount(Coarse.Get(), 0),
        WidenedOpsTest_UVElementCount(Packed.Get(), 0));

    return true;
}

// ============================================================================
// union / subtract / intersection, and trim
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryOpsWidenedBooleanAllowEmptyResultTest,
    "PinWright.Geometry.Ops.WidenedOptions.BooleanAllowEmptyResultReachesTheEngine",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGeometryOpsWidenedBooleanAllowEmptyResultTest::RunTest(const FString& Parameters)
{
    // A small box subtracted out of existence by a large one that encloses it. The two runs
    // differ in the only way this option can express.
    //
    // WHAT THE REFUSAL LOOKS LIKE, and this is the half that changed. ApplyMeshBoolean returns
    // the TARGET MESH from the empty-result path, never null, and reports the refusal ONLY
    // through its UGeometryScriptDebug* argument (MeshBooleanFunctions.cpp: bFailDueToEmptyResult
    // -> AppendError -> return TargetMesh). While that argument was passed as nullptr the refusal
    // was invisible and this op answered SUCCESS over a boolean the engine had declined to
    // perform - which is what this test used to assert, wrongly, as "the engine does not fail, it
    // declines". Boolean() now passes a real sink, so the refusal arrives as a FAILURE, and the
    // assertions below are the stronger form: a code, the engine's own remedy text, and the
    // target still standing.
    //
    // The target mesh count is read INSIDE the lambda because the handle dies with it.
    auto SubtractEverything = [](bool bAllowEmptyResult)
    {
        TStrongObjectPtr<UDynamicMesh> Target(WidenedOpsTest_NewBoxMesh(20.0, FVector::ZeroVector));
        TStrongObjectPtr<UDynamicMesh> Tool(WidenedOpsTest_NewBoxMesh(200.0, FVector::ZeroVector));

        GeometryOps::FBooleanParams Params;
        Params.bAllowEmptyResult = bAllowEmptyResult;

        const GeometryOps::FOpResult Op = GeometryOps::Boolean(
            Target.Get(), FTransform::Identity, Tool.Get(), FTransform::Identity,
            EGeometryScriptBooleanOperation::Subtract, Params);
        return TTuple<GeometryOps::FOpResult, int32>(Op, Target->GetTriangleCount());
    };

    const TTuple<GeometryOps::FOpResult, int32> Refused = SubtractEverything(false);
    TestFalse(TEXT("the default path REFUSES - the engine declines an empty result and now says so"),
        Refused.Get<0>().bSuccess);
    TestEqual(TEXT("and reports it as BOOLEAN_FAILED"),
        Refused.Get<0>().ErrorCode, FString(ErrorCodes::ERR_BOOLEAN_FAILED));
    // The engine's sentence is forwarded verbatim because it names the option the caller has to
    // turn on. A paraphrase would lose the only actionable half of the message.
    // 5.3's message names no remedy: the Allow Empty Result option it would point at only exists
    // from UE 5.4, and 5.3's engine text is the bare "BooleanUnion: Boolean operation failed".
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 4, 0)
    TestTrue(TEXT("and forwards the engine's own remedy, which names this very option"),
        Refused.Get<0>().ErrorMessage.Contains(TEXT("Allow Empty Result")));
#else
    TestTrue(TEXT("and still forwards the engine's own sentence"),
        Refused.Get<0>().ErrorMessage.Contains(TEXT("Boolean operation failed")));
#endif
    // The refusal is in-place: ApplyMeshBoolean returns before its SetMesh, so the caller keeps
    // exactly the geometry it had. FailIn keeps the before-count that proves it.
    TestEqual(TEXT("the target still has its 12 triangles"), Refused.Get<1>(), 12);
    TestEqual(TEXT("and the before-count survives the failure"),
        Refused.Get<0>().TrianglesBefore, 12);

    const TTuple<GeometryOps::FOpResult, int32> Allowed = SubtractEverything(true);
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 4, 0)
    TestTrue(TEXT("allow_empty_result=true succeeds where the default refused"),
        Allowed.Get<0>().bSuccess);
    TestEqual(TEXT("and empties the target, so the flag reaches the engine"),
        Allowed.Get<0>().TrianglesAfter, 0);
    TestEqual(TEXT("which the mesh itself agrees with"), Allowed.Get<1>(), 0);
    TestTrue(TEXT("and reports the change"), Allowed.Get<0>().bChanged);
#else
    // There is no engine flag to reach on 5.3, so the op refuses the request by name rather than
    // running the boolean and reporting a result the caller did not ask for.
    TestFalse(TEXT("allow_empty_result=true is refused on an engine without the option"),
        Allowed.Get<0>().bSuccess);
    TestEqual(TEXT("with UNSUPPORTED_ENGINE_VERSION"),
        Allowed.Get<0>().ErrorCode, FString(ErrorCodes::ERR_UNSUPPORTED_ENGINE_VERSION));
    TestEqual(TEXT("and the target keeps its 12 triangles"), Allowed.Get<1>(), 12);
#endif

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryOpsWidenedBooleanSimplifyOutputTest,
    "PinWright.Geometry.Ops.WidenedOptions.BooleanSimplifyOutputReachesTheEngine",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGeometryOpsWidenedBooleanSimplifyOutputTest::RunTest(const FString& Parameters)
{
    // The field these verbs used to pin false with no way to reach it. Now that the default
    // follows the engine, the direction that proves it reaches the engine is turning it OFF: the
    // unsimplified result must keep the coplanar fans the default collapses. That is also the
    // assertion that pins the escape hatch - `simplify_output=false` is what an existing document
    // uses to get the old triangulation back, so it has to actually do something.
    //
    // THE TARGET IS SUBDIVIDED, AND THAT IS THE WHOLE FIXTURE. bSimplifyOutput reaches
    // FMeshBoolean::bSimplifyAlongNewEdges (MeshBooleanFunctions.cpp), and SimplifyAlongNewEdges
    // (MeshBoolean.cpp:796) walks ONLY the cut-boundary edges and collapses a vertex only when
    // FLocalPlanarSimplify::IsFlat says its whole one-ring is coplanar. Two PLAIN axis-aligned
    // boxes give it nothing to do: the cut curve on each face runs corner-to-corner through the
    // face's own diagonal vertex, so the boolean's retriangulation is already the minimal
    // 4-triangles-per-L-hexagon, and every cut-boundary vertex sits on a box EDGE - not flat, not
    // collapsible. That fixture reported 36 triangles under both settings and could not have
    // reported anything else; it was measuring the box, not the flag.
    //
    // With EdgeVertices=5 the target's faces carry grid lines at -50/-25/0/25/50 while the tool's
    // faces cut at 20, so the cut line crosses grid edges at points that are INTERIOR to a flat
    // face. Those crossings are cut-boundary vertices with a coplanar one-ring lying on a
    // straight cut, which is exactly the collapse this pass exists to perform - and the box's
    // per-face affine UVs and constant per-face normals mean the CollapseWouldChangeShapeOrUVs
    // guard has nothing to object to either.
    //
    // "ACTUALLY CUTS", AND WHY IT IS NOT Op.bChanged. The four boolean verbs define bChanged as
    // the TRIANGLE-COUNT DELTA alone and say so (GeometryOps_Boolean.cpp, the one deliberate
    // divergence from FinishOp): it is a wire contract, not a statement about geometry. On this
    // fixture the delta is exactly the wrong instrument - the target is 192 triangles, the corner
    // subtraction deletes some and adds cut walls and retriangulation back, and one of the two
    // simplification settings lands the result back on 192 exactly. The op then reports
    // changed=false over a mesh with a 30uu notch cut out of it, and the precondition fails while
    // the cut it was guarding is perfectly real. Asking instead whether any triangle SURVIVES
    // inside the tool's solid states what a subtraction MEANS, and no count coincidence can
    // satisfy it.
    //
    // The probe box is the tool's [20,80] span inset by 1uu, because the walls the subtraction
    // adds lie exactly ON the tool's faces at 20: a centroid at 20.0 has to read as outside.
    const FVector3d ToolInteriorMin(21.0, 21.0, 21.0);
    const FVector3d ToolInteriorMax(79.0, 79.0, 79.0);

    auto SubtractCorner = [this, &ToolInteriorMin, &ToolInteriorMax](bool bSimplifyOutput)
    {
        TStrongObjectPtr<UDynamicMesh> Target(WidenedOpsTest_NewBoxMesh(100.0, FVector::ZeroVector, 5));
        TStrongObjectPtr<UDynamicMesh> Tool(WidenedOpsTest_NewBoxMesh(60.0, FVector(50, 50, 50)));

        // Read BEFORE the cut, so "none survive" below cannot pass over a target that never had
        // any there. The 4x4 grid puts two whole triangles of each of the +X, +Y and +Z faces
        // inside the tool - six, on this fixture.
        TestTrue(TEXT("the target has geometry where the tool is about to remove it"),
            WidenedOpsTest_TrianglesInsideBox(Target.Get(), ToolInteriorMin, ToolInteriorMax) > 0);

        GeometryOps::FBooleanParams Params;
        Params.bSimplifyOutput = bSimplifyOutput;

        const GeometryOps::FOpResult Op = GeometryOps::Boolean(
            Target.Get(), FTransform::Identity, Tool.Get(), FTransform::Identity,
            EGeometryScriptBooleanOperation::Subtract, Params);
        TestTrue(TEXT("the corner subtraction runs"), Op.bSuccess);
        // ApplyMeshBoolean writes the result back into Target, so this reads the cut mesh.
        TestEqual(TEXT("and actually cuts - no triangle survives inside the tool"),
            WidenedOpsTest_TrianglesInsideBox(Target.Get(), ToolInteriorMin, ToolInteriorMax), 0);
        return Op.TrianglesAfter;
    };

    const int32 Simplified = SubtractCorner(true);
    const int32 Unsimplified = SubtractCorner(false);

    TestTrue(TEXT("simplify_output=false keeps the coplanar fans the default collapses, so the "
                  "flag reaches the engine and the escape hatch works"),
        Unsimplified > Simplified);

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryOpsWidenedTrimOptionsTest,
    "PinWright.Geometry.Ops.WidenedOptions.TrimOptionsReachTheEngine",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGeometryOpsWidenedTrimOptionsTest::RunTest(const FString& Parameters)
{
    // trim's defaults are the engine's, INCLUDING simplification, so the direction that proves
    // the field reaches the engine is the opposite one from the boolean test above: here it is
    // turning simplification OFF that must add triangles back.
    //
    // Same subdivided target and for the same reason - see the boolean simplify test above for
    // why a PLAIN box-vs-box cut leaves SimplifyAlongNewEdges with no flat cut-boundary vertex to
    // collapse and so reports the same triangle count either way.
    auto TrimCorner = [this](bool bSimplifyOutput)
    {
        TStrongObjectPtr<UDynamicMesh> Target(WidenedOpsTest_NewBoxMesh(100.0, FVector::ZeroVector, 5));
        TStrongObjectPtr<UDynamicMesh> Tool(WidenedOpsTest_NewBoxMesh(60.0, FVector(50, 50, 50)));

        GeometryOps::FTrimParams Params;
        Params.bSimplifyOutput = bSimplifyOutput;

        const GeometryOps::FOpResult Op = GeometryOps::Trim(
            Target.Get(), FTransform::Identity, Tool.Get(), FTransform::Identity, Params);
        TestTrue(TEXT("the trim runs"), Op.bSuccess);
        return Op.TrianglesAfter;
    };

    const int32 Simplified = TrimCorner(true);
    const int32 Unsimplified = TrimCorner(false);
    TestTrue(TEXT("simplify_output=false keeps the triangles trim's default collapses, so the "
                  "field reaches the engine"),
        Unsimplified > Simplified);

    // And the same empty-result behaviour as the booleans, on the struct whose defaults are
    // otherwise the engine's.
    TStrongObjectPtr<UDynamicMesh> Target(WidenedOpsTest_NewBoxMesh(20.0, FVector::ZeroVector));
    TStrongObjectPtr<UDynamicMesh> Tool(WidenedOpsTest_NewBoxMesh(200.0, FVector::ZeroVector));

    GeometryOps::FTrimParams EmptyParams;
    EmptyParams.bAllowEmptyResult = true;
    const GeometryOps::FOpResult EmptyOp = GeometryOps::Trim(
        Target.Get(), FTransform::Identity, Tool.Get(), FTransform::Identity, EmptyParams);

#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 4, 0)
    TestTrue(TEXT("trim with allow_empty_result succeeds"), EmptyOp.bSuccess);
    TestEqual(TEXT("and empties the target, so the flag reaches the engine"), EmptyOp.TrianglesAfter, 0);
#else
    // Same contract as the boolean above on an engine with no bAllowEmptyResult.
    TestFalse(TEXT("trim with allow_empty_result is refused"), EmptyOp.bSuccess);
    TestEqual(TEXT("with UNSUPPORTED_ENGINE_VERSION"),
        EmptyOp.ErrorCode, FString(ErrorCodes::ERR_UNSUPPORTED_ENGINE_VERSION));
#endif

    return true;
}
