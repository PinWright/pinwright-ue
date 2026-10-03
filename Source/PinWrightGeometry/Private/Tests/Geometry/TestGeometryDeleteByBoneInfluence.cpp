// Copyright (c) 2026 Alexander Penkin. MIT License.

// GeometryOps::DeleteTrianglesByBoneInfluence (geometry.delete_triangles_by_bone_influence).
//
// Fixture: a bar along X from -150 to +150 with a three-bone chain b_root -> b_mid -> b_tip plus a
// separate root b_unused that skins nothing. Every vertex is hard-weighted by X: below -50 to
// b_root, -50..50 to b_mid, above 50 to b_tip. Vertices sit at -150 + k*300/7 (8 per edge), so none lies on a
// band boundary and every triangle's region membership is unambiguous at threshold 0.5.
//
// Counterfactuals: ignore bIncludeDescendants and the keep/descendants pair stops differing;
// invert bKeepRegion and the keep and delete tests swap ends; drop a pre-check and the refusal
// test sees the mesh change.
#include "Misc/AutomationTest.h"

#include "Handlers/ErrorCodes.h"
#include "Tests/TestUtils.h"
#include "Handlers/Geometry/GeometryOps_Elements.h"

#include "BoneWeights.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "DynamicMesh/DynamicBoneAttribute.h"
#include "DynamicMesh/DynamicMesh3.h"
#include "DynamicMesh/DynamicMeshAttributeSet.h"
#include "DynamicMesh/DynamicVertexSkinWeightsAttribute.h"
#include "UDynamicMesh.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

#include "GeometryScript/MeshBoneWeightFunctions.h"
#include "GeometryScript/MeshPrimitiveFunctions.h"

namespace GeometryDeleteByBoneInfluenceTest
{
    UDynamicMesh* NewBox()
    {
        UDynamicMesh* Mesh = NewObject<UDynamicMesh>(GetTransientPackage());
        FGeometryScriptPrimitiveOptions Options;
        UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendBox(
            Mesh, Options, FTransform::Identity, 300.0f, 40.0f, 40.0f,
            /*StepsX=*/8, /*StepsY=*/1, /*StepsZ=*/1, EGeometryScriptPrimitiveOriginMode::Center, nullptr);
        return Mesh;
    }

    UDynamicMesh* NewSkinnedBar()
    {
        UDynamicMesh* Mesh = NewBox();
        Mesh->EditMesh([](UE::Geometry::FDynamicMesh3& EditMesh)
        {
            EditMesh.EnableAttributes();
            UE::Geometry::FDynamicMeshAttributeSet* Attributes = EditMesh.Attributes();
            Attributes->EnableBones(4);
            const TCHAR* Names[] = { TEXT("b_root"), TEXT("b_mid"), TEXT("b_tip"), TEXT("b_unused") };
            const int32 Parents[] = { INDEX_NONE, 0, 1, INDEX_NONE };
            for (int32 Bone = 0; Bone < 4; ++Bone)
            {
                Attributes->GetBoneNames()->SetValue(Bone, FName(Names[Bone]));
                Attributes->GetBoneParentIndices()->SetValue(Bone, Parents[Bone]);
                Attributes->GetBonePoses()->SetValue(Bone, FTransform::Identity);
            }

            UE::Geometry::FDynamicMeshVertexSkinWeightsAttribute* Weights =
                new UE::Geometry::FDynamicMeshVertexSkinWeightsAttribute(&EditMesh);
            Attributes->AttachSkinWeightsAttribute(FGeometryScriptBoneWeightProfile().GetProfileName(), Weights);
            for (const int32 VertexID : EditMesh.VertexIndicesItr())
            {
                const double X = EditMesh.GetVertex(VertexID).X;
                const int32 Bone = X < -50.0 ? 0 : (X > 50.0 ? 2 : 1);
                const UE::AnimationCore::FBoneWeight Influence(static_cast<FBoneIndexType>(Bone), 1.0f);
                Weights->SetValue(VertexID, UE::AnimationCore::FBoneWeights::Create(
                    TArrayView<const UE::AnimationCore::FBoneWeight>(&Influence, 1)));
            }
        });
        return Mesh;
    }

    void XRange(UDynamicMesh* Mesh, double& OutMin, double& OutMax)
    {
        OutMin = TNumericLimits<double>::Max();
        OutMax = TNumericLimits<double>::Lowest();
        for (const int32 VertexID : Mesh->GetMeshRef().VertexIndicesItr())
        {
            const double X = Mesh->GetMeshRef().GetVertex(VertexID).X;
            OutMin = FMath::Min(OutMin, X);
            OutMax = FMath::Max(OutMax, X);
        }
    }

    FString JoinNames(const TArray<FName>& Names)
    {
        TArray<FString> Strings;
        for (const FName& Name : Names)
        {
            Strings.Add(Name.ToString());
        }
        return FString::Join(Strings, TEXT(","));
    }

    GeometryOps::FDeleteByBoneInfluenceParams MakeParams(std::initializer_list<const TCHAR*> Bones, bool bKeep, bool bDescendants = true)
    {
        GeometryOps::FDeleteByBoneInfluenceParams Params;
        for (const TCHAR* Bone : Bones)
        {
            Params.BoneNames.Add(FName(Bone));
        }
        Params.bKeepRegion = bKeep;
        Params.bIncludeDescendants = bDescendants;
        return Params;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryDeleteByBoneInfluenceKeepTest,
    "PinWright.Geometry.Ops.BoneInfluence.KeepRegionFollowsDescendants",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGeometryDeleteByBoneInfluenceKeepTest::RunTest(const FString& Parameters)
{
    using namespace GeometryDeleteByBoneInfluenceTest;

    // With descendants: b_mid's region is b_mid + b_tip, so the root end goes and the tip stays.
    TStrongObjectPtr<UDynamicMesh> WithDescendants(NewSkinnedBar());
    const int32 TrianglesBefore = WithDescendants->GetTriangleCount();
    GeometryOps::FDeleteByBoneInfluenceReport Report;
    const GeometryOps::FOpResult Op = GeometryOps::DeleteTrianglesByBoneInfluence(
        WithDescendants.Get(), MakeParams({ TEXT("b_mid") }, /*bKeep=*/true), Report);
    if (!TestTrue(FString::Printf(TEXT("keep succeeds: %s %s"), *Op.ErrorCode, *Op.ErrorMessage), Op.bSuccess))
    {
        return false;
    }
    TestEqual(TEXT("region bones are b_mid and its descendant b_tip"), JoinNames(Report.RegionBones), FString(TEXT("b_mid,b_tip")));
    TestTrue(TEXT("some triangles removed"), Report.TrianglesRemoved > 0);
    TestTrue(TEXT("changed"), Op.bChanged);
    TestEqual(TEXT("removed + kept = before"), Report.TrianglesRemoved + WithDescendants->GetTriangleCount(), TrianglesBefore);
    TestEqual(TEXT("kept triangles are the region"), WithDescendants->GetTriangleCount(), Report.RegionTriangles);
    double MinX, MaxX;
    XRange(WithDescendants.Get(), MinX, MaxX);
    TestTrue(FString::Printf(TEXT("root end removed (min x %.1f)"), MinX), MinX > -100.0);
    TestTrue(FString::Printf(TEXT("tip end kept through descendants (max x %.1f)"), MaxX), MaxX > 149.0);

    // Without descendants: only b_mid, so the tip end goes too. The differential pair.
    TStrongObjectPtr<UDynamicMesh> MidOnly(NewSkinnedBar());
    GeometryOps::FDeleteByBoneInfluenceReport MidReport;
    const GeometryOps::FOpResult MidOp = GeometryOps::DeleteTrianglesByBoneInfluence(
        MidOnly.Get(), MakeParams({ TEXT("b_mid") }, /*bKeep=*/true, /*bDescendants=*/false), MidReport);
    TestTrue(TEXT("keep without descendants succeeds"), MidOp.bSuccess);
    TestEqual(TEXT("region is b_mid alone"), JoinNames(MidReport.RegionBones), FString(TEXT("b_mid")));
    XRange(MidOnly.Get(), MinX, MaxX);
    TestTrue(FString::Printf(TEXT("tip end removed without descendants (max x %.1f)"), MaxX), MaxX < 100.0);
    TestTrue(TEXT("fewer triangles kept without descendants"), MidOnly->GetTriangleCount() < WithDescendants->GetTriangleCount());

    // Threshold is read: corners are hard-weighted, so band-edge triangles have a b_mid mean of
    // 1/3, inside the region at 0.3 and outside it at 0.5.
    TStrongObjectPtr<UDynamicMesh> LowThreshold(NewSkinnedBar());
    GeometryOps::FDeleteByBoneInfluenceParams LowParams = MakeParams({ TEXT("b_mid") }, /*bKeep=*/true, /*bDescendants=*/false);
    LowParams.Threshold = 0.3;
    GeometryOps::FDeleteByBoneInfluenceReport LowReport;
    TestTrue(TEXT("keep at threshold 0.3 succeeds"),
        GeometryOps::DeleteTrianglesByBoneInfluence(LowThreshold.Get(), LowParams, LowReport).bSuccess);
    TestTrue(FString::Printf(TEXT("threshold 0.3 selects more than 0.5 (%d vs %d)"), LowReport.RegionTriangles, MidReport.RegionTriangles),
        LowReport.RegionTriangles > MidReport.RegionTriangles);
    return true;
}

// Wire-level refusals the op never sees: they are rejected before the actor is resolved.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryDeleteByBoneInfluenceHandlerArgsTest,
    "PinWright.Geometry.Ops.BoneInfluence.HandlerRefusesBadModeAndBoneNames",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGeometryDeleteByBoneInfluenceHandlerArgsTest::RunTest(const FString& Parameters)
{
    auto Invoke = [this](const TCHAR* Label, const FString& Mode, const TSharedPtr<FJsonValue>& Bone)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("actorName"), TEXT("PwNoSuchBoneInfluenceActor"));
        Payload->SetStringField(TEXT("mode"), Mode);
        Payload->SetArrayField(TEXT("boneNames"), { Bone });
        FTestResponseCapture Capture;
        TestTrue(FString::Printf(TEXT("%s: handler registered"), Label),
            InvokeHandlerWithCapture(TEXT("geometry.delete_triangles_by_bone_influence"), Payload, Capture));
        TestFalse(FString::Printf(TEXT("%s is refused"), Label), Capture.bSuccess);
        TestEqual(FString::Printf(TEXT("%s error code"), Label), Capture.ErrorCode, FString(ErrorCodes::ERR_INVALID_ARGUMENT));
    };
    Invoke(TEXT("unknown mode"), TEXT("cut"), MakeShared<FJsonValueString>(TEXT("b_mid")));
    Invoke(TEXT("non-string bone name"), TEXT("keep"), MakeShared<FJsonValueNumber>(1));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryDeleteByBoneInfluenceDeleteTest,
    "PinWright.Geometry.Ops.BoneInfluence.DeleteModeRemovesOnlyTheRegion",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGeometryDeleteByBoneInfluenceDeleteTest::RunTest(const FString& Parameters)
{
    using namespace GeometryDeleteByBoneInfluenceTest;
    TStrongObjectPtr<UDynamicMesh> Mesh(NewSkinnedBar());
    GeometryOps::FDeleteByBoneInfluenceReport Report;
    const GeometryOps::FOpResult Op = GeometryOps::DeleteTrianglesByBoneInfluence(
        Mesh.Get(), MakeParams({ TEXT("b_tip") }, /*bKeep=*/false), Report);
    if (!TestTrue(FString::Printf(TEXT("delete succeeds: %s %s"), *Op.ErrorCode, *Op.ErrorMessage), Op.bSuccess))
    {
        return false;
    }
    TestEqual(TEXT("removed exactly the region"), Report.TrianglesRemoved, Report.RegionTriangles);
    double MinX, MaxX;
    XRange(Mesh.Get(), MinX, MaxX);
    TestTrue(FString::Printf(TEXT("tip end removed (max x %.1f)"), MaxX), MaxX < 100.0);
    TestTrue(FString::Printf(TEXT("root end untouched (min x %.1f)"), MinX), MinX < -149.0);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryDeleteByBoneInfluenceRefusalsTest,
    "PinWright.Geometry.Ops.BoneInfluence.RefusalsLeaveTheMeshUntouched",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGeometryDeleteByBoneInfluenceRefusalsTest::RunTest(const FString& Parameters)
{
    using namespace GeometryDeleteByBoneInfluenceTest;
    TStrongObjectPtr<UDynamicMesh> Mesh(NewSkinnedBar());
    const int32 Triangles = Mesh->GetTriangleCount();

    auto ExpectRefused = [this, &Mesh, Triangles](const TCHAR* Label, const GeometryOps::FDeleteByBoneInfluenceParams& Params, const TCHAR* Code)
    {
        GeometryOps::FDeleteByBoneInfluenceReport Report;
        const GeometryOps::FOpResult Op = GeometryOps::DeleteTrianglesByBoneInfluence(Mesh.Get(), Params, Report);
        TestFalse(FString::Printf(TEXT("%s is refused"), Label), Op.bSuccess);
        TestEqual(FString::Printf(TEXT("%s error code"), Label), Op.ErrorCode, FString(Code));
        TestEqual(FString::Printf(TEXT("%s leaves the mesh untouched"), Label), Mesh->GetTriangleCount(), Triangles);
    };

    ExpectRefused(TEXT("unknown bone"), MakeParams({ TEXT("b_mid"), TEXT("no_such_bone") }, true), ErrorCodes::ERR_BONE_NOT_FOUND);
    ExpectRefused(TEXT("bone that skins nothing"), MakeParams({ TEXT("b_unused") }, true), ErrorCodes::ERR_BONE_REGION_EMPTY);
    ExpectRefused(TEXT("delete covering every triangle"), MakeParams({ TEXT("b_root") }, false), ErrorCodes::ERR_INVALID_ARGUMENT);
    ExpectRefused(TEXT("no bones named"), MakeParams({}, true), ErrorCodes::ERR_INVALID_ARGUMENT);
    GeometryOps::FDeleteByBoneInfluenceParams ZeroThreshold = MakeParams({ TEXT("b_mid") }, true);
    ZeroThreshold.Threshold = 0.0;
    ExpectRefused(TEXT("threshold 0"), ZeroThreshold, ErrorCodes::ERR_INVALID_ARGUMENT);

    // An unskinned mesh has nothing to select by.
    TStrongObjectPtr<UDynamicMesh> Plain(NewBox());
    GeometryOps::FDeleteByBoneInfluenceReport Report;
    const GeometryOps::FOpResult Op = GeometryOps::DeleteTrianglesByBoneInfluence(
        Plain.Get(), MakeParams({ TEXT("b_mid") }, true), Report);
    TestFalse(TEXT("unskinned mesh is refused"), Op.bSuccess);
    TestEqual(TEXT("unskinned mesh error code"), Op.ErrorCode, FString(ErrorCodes::ERR_NO_SKIN_WEIGHTS));
    return true;
}
