// Copyright (c) 2026 Alexander Penkin. MIT License.

// Tests for the skin-weight repair that GeometryOps::Boolean runs over a skinned mesh, and for
// the dominant-bone reach check that reports what is left.
//
// THE DEFECT THESE PIN, restated from GeometrySkinWeightRepair.h: FMeshBoolean deletes the
// triangles it discards with bRemoveIsolatedVertices, freeing those vertex IDs, then appends the
// other operand's geometry into the SAME mesh, where FDynamicMesh3::AppendVertex re-uses the
// freed IDs. The skin-weight layer's OnNewVertex only ever grows its store
// (DynamicVertexSkinWeightsAttribute.h:461-464) and never clears a re-used slot, and AppendMesh
// writes skin weights only for profiles the APPENDED mesh carries (DynamicMeshEditor.cpp:2073) -
// which a cutting box has none of. So every vertex the boolean invents either inherits a deleted
// vertex's bone weights or gets none at all, and both outcomes are silent: the boolean reports
// changed:true and the skeletal write reports fullyWeighted:true.
//
// Measured on stock SKM_Manny before the fix: vertices at the FEET 0.934-weighted to `head`
// 162.6 cm away, with all 160 other bones nearer.
//
// The fixture is the smallest mesh that can express that failure: a bar along X with one bone at
// each end and every vertex hard-weighted to the nearer one, cut by a tool that bores a tunnel
// down the WHOLE length. That shape is the point - it deletes vertices at BOTH ends (so the
// stale slots carry both bones) and creates hundreds of tunnel-wall vertices spread along the
// entire bar, so a wall vertex at one end that inherited the other end's bone is exactly the
// SKM_Manny failure at 1/500th the size.
//
// Counterfactuals:
//  - drop the FPreEditSnapshot/Repair pair from GeometryOps::Boolean and
//    SubtractKeepsEveryVertexOnItsNearerBone fails on the FIRST assertion, because the tunnel
//    wall vertices whose IDs were past the end of the store carry no influences at all;
//  - keep the repair but transfer from the POST-op mesh instead of the snapshot and the second
//    assertion fails, because the post-op mesh is the one carrying the corrupt weights;
//  - make the repair rewrite every vertex instead of only the unaccountable ones and
//    SubtractLeavesUntouchedVerticesExactlyAsTheyWere fails - that test is what keeps the repair
//    from quietly re-deriving the 99% of a mesh a cut never touched;
//  - revert GeometryUtils::ScanDominantBoneReach and both FarBoneReach tests fail, taking the
//    only automated statement that `fullyWeighted` means something with them.
#include "Misc/AutomationTest.h"

#include "Handlers/Geometry/GeometryOps_Boolean.h"
#include "Handlers/Geometry/GeometryUtils.h"

#include "BoneWeights.h"
#include "DynamicMesh/DynamicBoneAttribute.h"
#include "DynamicMesh/DynamicMesh3.h"
#include "DynamicMesh/DynamicMeshAttributeSet.h"
#include "DynamicMesh/DynamicVertexSkinWeightsAttribute.h"
#include "UDynamicMesh.h"
#include "UObject/Package.h"

#if __has_include("GeometryScript/GeometryScriptTypes.h")
#include "GeometryScript/GeometryScriptTypes.h"
#else
#include "GeometryScriptTypes.h"
#endif
#include "GeometryScript/MeshBoneWeightFunctions.h"
#include "GeometryScript/MeshPrimitiveFunctions.h"

namespace
{
// Uniquely prefixed: this module builds with bUseUnity = true, so an anonymous-namespace helper
// with a common name would collide with a sibling test TU once Unity merges them.

// The bar runs from -BarHalfLength to +BarHalfLength along X, with one bone at each end.
constexpr double GeometryBooleanSkinTest_BarHalfLength = 100.0;

// How far from the midline a vertex has to be before the test judges which bone owns it. Inside
// this band the correct answer is a genuine 50/50 blend of the two bones and the dominant one is
// a coin flip, so asserting there would be asserting on float noise rather than on skinning.
// Three times the bar's subdivision spacing, so the triangle a vertex projects onto cannot span
// the midline.
constexpr double GeometryBooleanSkinTest_MidlineBand = 60.0;

constexpr int32 GeometryBooleanSkinTest_BoneLow = 0;
constexpr int32 GeometryBooleanSkinTest_BoneHigh = 1;

FName GeometryBooleanSkinTest_ProfileName()
{
    return FGeometryScriptBoneWeightProfile().GetProfileName();
}

UDynamicMesh* GeometryBooleanSkinTest_NewMesh()
{
    return NewObject<UDynamicMesh>(GetTransientPackage());
}

UE::AnimationCore::FBoneWeights GeometryBooleanSkinTest_SingleInfluence(int32 BoneIndex)
{
    const UE::AnimationCore::FBoneWeight Influence(static_cast<FBoneIndexType>(BoneIndex), 1.0f);
    return UE::AnimationCore::FBoneWeights::Create(
        TArrayView<const UE::AnimationCore::FBoneWeight>(&Influence, 1));
}

// Which bone a vertex at this position ought to be dominated by. The fixture weights by nearest
// bone, and nearest bone is decided by the sign of X because both bones sit on the X axis.
int32 GeometryBooleanSkinTest_ExpectedBone(const FVector3d& Position)
{
    return Position.X < 0.0 ? GeometryBooleanSkinTest_BoneLow : GeometryBooleanSkinTest_BoneHigh;
}

int32 GeometryBooleanSkinTest_DominantBone(const UE::AnimationCore::FBoneWeights& Weights)
{
    int32 Dominant = INDEX_NONE;
    uint16 BestRawWeight = 0;
    for (const UE::AnimationCore::FBoneWeight& Influence : Weights)
    {
        if (Dominant == INDEX_NONE || Influence.GetRawWeight() > BestRawWeight)
        {
            Dominant = static_cast<int32>(Influence.GetBoneIndex());
            BestRawWeight = Influence.GetRawWeight();
        }
    }
    return Dominant;
}

// A bar along X, subdivided so it carries vertices along its length rather than only at its eight
// corners, with two bones at its ends and every vertex hard-weighted to the nearer one.
UDynamicMesh* GeometryBooleanSkinTest_NewSkinnedBar()
{
    UDynamicMesh* Mesh = GeometryBooleanSkinTest_NewMesh();

    FGeometryScriptPrimitiveOptions Options;
    UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendBox(
        Mesh, Options, FTransform::Identity,
        static_cast<float>(GeometryBooleanSkinTest_BarHalfLength * 2.0), 60.0f, 60.0f,
        /*StepsX=*/8, /*StepsY=*/6, /*StepsZ=*/6,
        EGeometryScriptPrimitiveOriginMode::Center, nullptr);

    Mesh->EditMesh([](UE::Geometry::FDynamicMesh3& EditMesh)
    {
        EditMesh.EnableAttributes();

        // Two bones, both on the X axis at the ends of the bar, parented root -> tip so the
        // composed reference pose GetAllBonesInfo returns puts them where the local transforms
        // say. bone_high's local transform is therefore relative to bone_low.
        EditMesh.Attributes()->EnableBones(2);
        EditMesh.Attributes()->GetBoneNames()->SetValue(
            GeometryBooleanSkinTest_BoneLow, FName(TEXT("bone_low")));
        EditMesh.Attributes()->GetBoneNames()->SetValue(
            GeometryBooleanSkinTest_BoneHigh, FName(TEXT("bone_high")));
        EditMesh.Attributes()->GetBoneParentIndices()->SetValue(
            GeometryBooleanSkinTest_BoneLow, INDEX_NONE);
        EditMesh.Attributes()->GetBoneParentIndices()->SetValue(
            GeometryBooleanSkinTest_BoneHigh, GeometryBooleanSkinTest_BoneLow);
        EditMesh.Attributes()->GetBonePoses()->SetValue(
            GeometryBooleanSkinTest_BoneLow,
            FTransform(FVector(-GeometryBooleanSkinTest_BarHalfLength, 0.0, 0.0)));
        EditMesh.Attributes()->GetBonePoses()->SetValue(
            GeometryBooleanSkinTest_BoneHigh,
            FTransform(FVector(GeometryBooleanSkinTest_BarHalfLength * 2.0, 0.0, 0.0)));

        UE::Geometry::FDynamicMeshVertexSkinWeightsAttribute* Weights =
            new UE::Geometry::FDynamicMeshVertexSkinWeightsAttribute(&EditMesh);
        EditMesh.Attributes()->AttachSkinWeightsAttribute(
            GeometryBooleanSkinTest_ProfileName(), Weights);

        for (const int32 VertexID : EditMesh.VertexIndicesItr())
        {
            Weights->SetValue(VertexID, GeometryBooleanSkinTest_SingleInfluence(
                GeometryBooleanSkinTest_ExpectedBone(EditMesh.GetVertex(VertexID))));
        }
    });

    return Mesh;
}

// A square tunnel bored down the whole length of the bar and out both ends. Subtracting it
// deletes vertices at BOTH ends of the bar and appends a wall that runs the bar's whole length -
// the shape that puts one end's stale bone weights onto the other end's new vertices.
//
// SUBDIVIDED ALONG X on purpose. An 8-corner tool box has both its ends outside the bar, so the
// only wall vertices the append would contribute are the two rings the cut itself creates at
// x = +/-100 - each already beside the bone it belongs to, which would let a stale-slot swap go
// unnoticed half the time. Subdividing puts tool vertices at intervals down the whole tunnel, so
// the appended set spans the bar and a swapped weight lands where it is unambiguous.
UDynamicMesh* GeometryBooleanSkinTest_NewTunnelTool()
{
    UDynamicMesh* Mesh = GeometryBooleanSkinTest_NewMesh();
    FGeometryScriptPrimitiveOptions Options;
    UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendBox(
        Mesh, Options, FTransform::Identity,
        static_cast<float>(GeometryBooleanSkinTest_BarHalfLength * 3.0), 30.0f, 30.0f,
        /*StepsX=*/8, /*StepsY=*/2, /*StepsZ=*/2,
        EGeometryScriptPrimitiveOriginMode::Center, nullptr);
    return Mesh;
}
}

// ============================================================================
// The repair
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryBooleanSkinWeightsNearerBoneTest,
    "PinWright.Geometry.Ops.Boolean.SubtractKeepsEveryVertexOnItsNearerBone",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGeometryBooleanSkinWeightsNearerBoneTest::RunTest(const FString& Parameters)
{
    UDynamicMesh* Target = GeometryBooleanSkinTest_NewSkinnedBar();
    UDynamicMesh* Tool = GeometryBooleanSkinTest_NewTunnelTool();

    GeometryOps::FBooleanParams Params;
    // Both halves of the caller's report, explicitly: the ticket this fixes was FILED against
    // fillHoles, and the controlled re-run proved the corruption identical either way. Holes are
    // filled here because that is the verb's default and the cap vertices it adds are part of
    // what the repair has to answer for.
    Params.bFillHoles = true;
    Params.bSimplifyOutput = false;

    const GeometryOps::FOpResult Op = GeometryOps::Boolean(
        Target, FTransform::Identity, Tool, FTransform::Identity,
        EGeometryScriptBooleanOperation::Subtract, Params);

    TestTrue(TEXT("subtract succeeds"), Op.bSuccess);
    TestTrue(TEXT("subtract actually cut the bar"), Op.bChanged);
    TestTrue(TEXT("the op reports the mesh as skinned"), Op.bSkinned);
    TestTrue(TEXT("the repair re-derived the vertices the cut invented"), Op.SkinWeightsTransferred > 0);
    TestEqual(TEXT("no vertex was left unaccounted for"), Op.SkinWeightsUnresolved, 0);

    int32 UnweightedCount = 0;
    int32 WrongBoneCount = 0;
    int32 JudgedCount = 0;

    Target->ProcessMesh([&](const UE::Geometry::FDynamicMesh3& ReadMesh)
    {
        const UE::Geometry::FDynamicMeshVertexSkinWeightsAttribute* Weights =
            ReadMesh.HasAttributes()
                ? ReadMesh.Attributes()->GetSkinWeightsAttribute(GeometryBooleanSkinTest_ProfileName())
                : nullptr;
        if (!Weights)
        {
            return;
        }

        for (const int32 VertexID : ReadMesh.VertexIndicesItr())
        {
            UE::AnimationCore::FBoneWeights VertexWeights;
            Weights->GetValue(VertexID, VertexWeights);

            // Assertion one, and the one that fails without the repair at all: the tunnel wall
            // and hole-fill vertices whose IDs landed past the end of the attribute store carry
            // an empty FBoneWeights.
            if (VertexWeights.Num() == 0)
            {
                ++UnweightedCount;
                continue;
            }

            const FVector3d Position = ReadMesh.GetVertex(VertexID);
            if (FMath::Abs(Position.X) <= GeometryBooleanSkinTest_MidlineBand)
            {
                continue;
            }
            ++JudgedCount;

            // Assertion two, and the one that fails when the repair transfers from the wrong
            // source: a wall vertex at one end of the bar dominated by the bone at the other.
            if (GeometryBooleanSkinTest_DominantBone(VertexWeights)
                != GeometryBooleanSkinTest_ExpectedBone(Position))
            {
                ++WrongBoneCount;
            }
        }
    });

    TestEqual(TEXT("no vertex is left without influences"), UnweightedCount, 0);
    TestEqual(TEXT("no vertex is dominated by the far bone"), WrongBoneCount, 0);
    TestTrue(TEXT("the sweep actually judged vertices"), JudgedCount > 0);

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryBooleanSkinWeightsUntouchedTest,
    "PinWright.Geometry.Ops.Boolean.SubtractLeavesUntouchedVerticesExactlyAsTheyWere",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGeometryBooleanSkinWeightsUntouchedTest::RunTest(const FString& Parameters)
{
    UDynamicMesh* Target = GeometryBooleanSkinTest_NewSkinnedBar();
    UDynamicMesh* Tool = GeometryBooleanSkinTest_NewTunnelTool();

    GeometryOps::FBooleanParams Params;
    Params.bFillHoles = true;
    Params.bSimplifyOutput = false;

    const GeometryOps::FOpResult Op = GeometryOps::Boolean(
        Target, FTransform::Identity, Tool, FTransform::Identity,
        EGeometryScriptBooleanOperation::Subtract, Params);
    TestTrue(TEXT("subtract succeeds"), Op.bSuccess);

    // The bar's outer skin is nowhere near the tunnel, so the cut has no business touching it.
    // Every one of those vertices must still carry the SINGLE full-weight influence the fixture
    // gave it - a repair that re-derived the whole mesh would blend neighbouring bones in and
    // leave two influences here instead of one.
    const UE::AnimationCore::FBoneWeights ExpectedLow =
        GeometryBooleanSkinTest_SingleInfluence(GeometryBooleanSkinTest_BoneLow);
    const UE::AnimationCore::FBoneWeights ExpectedHigh =
        GeometryBooleanSkinTest_SingleInfluence(GeometryBooleanSkinTest_BoneHigh);

    int32 CheckedCount = 0;
    int32 AlteredCount = 0;

    Target->ProcessMesh([&](const UE::Geometry::FDynamicMesh3& ReadMesh)
    {
        const UE::Geometry::FDynamicMeshVertexSkinWeightsAttribute* Weights =
            ReadMesh.HasAttributes()
                ? ReadMesh.Attributes()->GetSkinWeightsAttribute(GeometryBooleanSkinTest_ProfileName())
                : nullptr;
        if (!Weights)
        {
            return;
        }

        for (const int32 VertexID : ReadMesh.VertexIndicesItr())
        {
            const FVector3d Position = ReadMesh.GetVertex(VertexID);
            // The outer surface of the bar, well away from the tunnel mouth in Y and Z and away
            // from the midline in X.
            const bool bOnOuterSkin = FMath::Max(FMath::Abs(Position.Y), FMath::Abs(Position.Z)) > 25.0;
            if (!bOnOuterSkin || FMath::Abs(Position.X) <= GeometryBooleanSkinTest_MidlineBand)
            {
                continue;
            }
            ++CheckedCount;

            UE::AnimationCore::FBoneWeights VertexWeights;
            Weights->GetValue(VertexID, VertexWeights);
            const UE::AnimationCore::FBoneWeights& Expected =
                Position.X < 0.0 ? ExpectedLow : ExpectedHigh;
            if (VertexWeights != Expected)
            {
                ++AlteredCount;
            }
        }
    });

    TestTrue(TEXT("the sweep found untouched outer-skin vertices"), CheckedCount > 0);
    TestEqual(TEXT("untouched vertices keep their original influences byte for byte"), AlteredCount, 0);

    return true;
}

// ============================================================================
// The reach check
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometrySkinWeightsReachCleanTest,
    "PinWright.Geometry.SkinWeights.FarBoneReachPassesACorrectlyWeightedMesh",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGeometrySkinWeightsReachCleanTest::RunTest(const FString& Parameters)
{
    UDynamicMesh* Mesh = GeometryBooleanSkinTest_NewSkinnedBar();

    const GeometryUtils::FSkinWeightCoverage Coverage = GeometryUtils::ScanSkinWeightCoverage(Mesh);

    TestTrue(TEXT("the mesh carries a skin-weight profile"), Coverage.bHasProfile);
    TestTrue(TEXT("the reach check ran"), Coverage.bBoneReachChecked);
    TestEqual(TEXT("a nearest-bone weighting flags nothing"), Coverage.FarBoneCount, 0);
    TestTrue(TEXT("a clean mesh is trustworthy"), Coverage.IsWeightingTrustworthy());

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometrySkinWeightsReachFlagsFarBoneTest,
    "PinWright.Geometry.SkinWeights.FarBoneReachFlagsAVertexBoundToTheFarthestBone",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGeometrySkinWeightsReachFlagsFarBoneTest::RunTest(const FString& Parameters)
{
    UDynamicMesh* Mesh = GeometryBooleanSkinTest_NewSkinnedBar();

    // One vertex at the low end re-bound to the bone at the high end - the two-bone miniature of
    // a foot vertex weighted to `head`. Everything else is left correct, so the count below is
    // also a statement that the check does not smear across the mesh.
    int32 CorruptedVertex = INDEX_NONE;
    Mesh->EditMesh([&CorruptedVertex](UE::Geometry::FDynamicMesh3& EditMesh)
    {
        UE::Geometry::FDynamicMeshVertexSkinWeightsAttribute* Weights =
            EditMesh.Attributes()->GetSkinWeightsAttribute(GeometryBooleanSkinTest_ProfileName());
        for (const int32 VertexID : EditMesh.VertexIndicesItr())
        {
            if (EditMesh.GetVertex(VertexID).X < -GeometryBooleanSkinTest_MidlineBand)
            {
                CorruptedVertex = VertexID;
                Weights->SetValue(VertexID, GeometryBooleanSkinTest_SingleInfluence(
                    GeometryBooleanSkinTest_BoneHigh));
                break;
            }
        }
    });
    TestTrue(TEXT("the fixture found a vertex to corrupt"), CorruptedVertex != INDEX_NONE);

    const GeometryUtils::FSkinWeightCoverage Coverage = GeometryUtils::ScanSkinWeightCoverage(Mesh);

    TestEqual(TEXT("exactly the corrupted vertex is flagged"), Coverage.FarBoneCount, 1);
    TestEqual(TEXT("the flagged vertex is the one that was corrupted"),
        Coverage.WorstFarBoneVertex, CorruptedVertex);
    TestEqual(TEXT("the offending bone is named"),
        Coverage.WorstFarBoneName.ToString(), FString(TEXT("bone_high")));

    // The whole point of the check: the mesh is FULLY weighted and still not trustworthy, which
    // is exactly the pair of answers that let a broken viewmodel ship.
    TestTrue(TEXT("every vertex still carries an influence"), Coverage.IsFullyWeighted());
    TestFalse(TEXT("but the weighting is not trustworthy"), Coverage.IsWeightingTrustworthy());

    return true;
}
