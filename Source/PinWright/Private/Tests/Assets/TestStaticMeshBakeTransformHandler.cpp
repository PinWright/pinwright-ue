// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/Guid.h"
#include "Misc/ScopeExit.h"
#include "Tests/TestUtils.h"

#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshSocket.h"
#include "MeshDescription.h"
#include "Misc/PackageName.h"
#include "PhysicsEngine/BodySetup.h"
#include "PhysicsEngine/BoxElem.h"
#include "PhysicsEngine/ConvexElem.h"
#include "StaticMeshAttributes.h"
#include "UObject/Package.h"

namespace
{
// Creates a transient one-triangle StaticMesh at PackagePath with built render
// data and a committed LOD0 mesh description. Vertices at (100,0,0), (0,100,0),
// (0,0,50); one vertex instance per vertex, all instance normals (0,0,1).
// bRecomputeNormals is disabled on LOD0 so a bake rebuild preserves the authored
// normals. Returns nullptr on failure; caller owns rooting + cleanup.
UStaticMesh* CreateBakeTransformFixtureMesh(const FString& PackagePath)
{
    const FString AssetName = FPackageName::GetLongPackageAssetName(PackagePath);
    UPackage* Package = CreatePackage(*PackagePath);
    UStaticMesh* Mesh = NewObject<UStaticMesh>(
        Package, FName(*AssetName), RF_Public | RF_Standalone);
    if (!Mesh)
    {
        return nullptr;
    }

    FMeshDescription MD;
    FStaticMeshAttributes Attrs(MD);
    Attrs.Register();

    const FVertexID V0 = MD.CreateVertex();
    const FVertexID V1 = MD.CreateVertex();
    const FVertexID V2 = MD.CreateVertex();
    TVertexAttributesRef<FVector3f> Positions = Attrs.GetVertexPositions();
    Positions[V0] = FVector3f(100.0f, 0.0f, 0.0f);
    Positions[V1] = FVector3f(0.0f, 100.0f, 0.0f);
    Positions[V2] = FVector3f(0.0f, 0.0f, 50.0f);

    const FVertexInstanceID VI0 = MD.CreateVertexInstance(V0);
    const FVertexInstanceID VI1 = MD.CreateVertexInstance(V1);
    const FVertexInstanceID VI2 = MD.CreateVertexInstance(V2);
    TVertexInstanceAttributesRef<FVector3f> Normals = Attrs.GetVertexInstanceNormals();
    Normals[VI0] = FVector3f(0.0f, 0.0f, 1.0f);
    Normals[VI1] = FVector3f(0.0f, 0.0f, 1.0f);
    Normals[VI2] = FVector3f(0.0f, 0.0f, 1.0f);

    const FPolygonGroupID PolygonGroup = MD.CreatePolygonGroup();
    const FVertexInstanceID TriangleInstances[3] = { VI0, VI1, VI2 };
    MD.CreateTriangle(PolygonGroup, TriangleInstances);

    UStaticMesh::FBuildMeshDescriptionsParams Params;
    Params.bFastBuild = true;
    if (!Mesh->BuildFromMeshDescriptions({ &MD }, Params))
    {
        return nullptr;
    }
    Mesh->GetSourceModel(0).BuildSettings.bRecomputeNormals = false;
    return Mesh;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStaticMeshBakeTransformYaw180Test,
    "PinWright.static_mesh.bake_transform.Yaw180TransformsGeometryCollisionAndSockets",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FStaticMeshBakeTransformYaw180Test::RunTest(const FString& Parameters)
{
    const FString PackagePath = FString::Printf(TEXT("/Game/PinWrightTests/SM_BakeTransform_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    UStaticMesh* Mesh = CreateBakeTransformFixtureMesh(PackagePath);
    TestNotNull(TEXT("Temporary StaticMesh created and built"), Mesh);
    if (!Mesh)
    {
        return false;
    }

    Mesh->AddToRoot();
    ON_SCOPE_EXIT
    {
        Mesh->RemoveFromRoot();
        CleanupTestAsset(PackagePath);
    };

    Mesh->CreateBodySetup();
    UBodySetup* BodySetup = Mesh->GetBodySetup();
    TestNotNull(TEXT("Temporary StaticMesh body setup created"), BodySetup);
    if (!BodySetup)
    {
        return false;
    }
    FKConvexElem ConvexElem;
    ConvexElem.VertexData = {
        FVector(10.0, 0.0, 0.0), FVector(0.0, 10.0, 0.0),
        FVector(0.0, 0.0, 10.0), FVector(0.0, 0.0, 0.0) };
    ConvexElem.UpdateElemBox();
    BodySetup->AggGeom.ConvexElems.Add(ConvexElem);
    FKBoxElem BoxElem(10.0f, 20.0f, 30.0f);
    BoxElem.Center = FVector(5.0, 0.0, 0.0);
    BodySetup->AggGeom.BoxElems.Add(BoxElem);

    UStaticMeshSocket* Socket = NewObject<UStaticMeshSocket>(Mesh);
    Socket->RelativeLocation = FVector(100.0, 0.0, 0.0);
    Mesh->Sockets.Add(Socket);

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), Mesh->GetPathName());
    TSharedPtr<FJsonObject> Rotation = MakeShared<FJsonObject>();
    Rotation->SetNumberField(TEXT("yaw"), 180.0);
    Payload->SetObjectField(TEXT("rotation"), Rotation);
    Payload->SetBoolField(TEXT("save"), false);

    FTestResponseCapture Capture;
    const bool bFound = InvokeHandlerWithCapture(
        TEXT("static_mesh.bake_transform"), Payload, Capture);

    TestTrue(TEXT("Handler found in registration list"), bFound);
    TestTrue(TEXT("Handler responded"), Capture.bWasCalled);
    TestTrue(FString::Printf(TEXT("Handler succeeded (errorCode='%s')"), *Capture.ErrorCode),
        Capture.bSuccess);
    TestTrue(TEXT("Result returned"), Capture.Result.IsValid());
    if (!bFound || !Capture.bSuccess || !Capture.Result.IsValid())
    {
        return false;
    }

    double LodsTransformed = 0.0;
    TestTrue(TEXT("lodsTransformed is a number"),
        Capture.Result->TryGetNumberField(TEXT("lodsTransformed"), LodsTransformed));
    TestEqual(TEXT("lodsTransformed is 1"), static_cast<int32>(LodsTransformed), 1);

    double SocketsTransformed = 0.0;
    TestTrue(TEXT("socketsTransformed is a number"),
        Capture.Result->TryGetNumberField(TEXT("socketsTransformed"), SocketsTransformed));
    TestEqual(TEXT("socketsTransformed is 1"), static_cast<int32>(SocketsTransformed), 1);

    const TSharedPtr<FJsonObject>* Collision = nullptr;
    if (TestTrue(TEXT("collision is an object"),
        Capture.Result->TryGetObjectField(TEXT("collision"), Collision)))
    {
        double ConvexCount = 0.0;
        double BoxCount = 0.0;
        TestTrue(TEXT("collision.convex is a number"),
            (*Collision)->TryGetNumberField(TEXT("convex"), ConvexCount));
        TestTrue(TEXT("collision.box is a number"),
            (*Collision)->TryGetNumberField(TEXT("box"), BoxCount));
        TestEqual(TEXT("collision.convex is 1"), static_cast<int32>(ConvexCount), 1);
        TestEqual(TEXT("collision.box is 1"), static_cast<int32>(BoxCount), 1);
    }

    // The AddAssetSaveReport contract: save:false must report saveRequested:false
    // and saved:false — no disk write happened.
    bool bSaveRequested = true;
    TestTrue(TEXT("saveRequested is a bool"),
        Capture.Result->TryGetBoolField(TEXT("saveRequested"), bSaveRequested));
    TestFalse(TEXT("save:false reports saveRequested:false"), bSaveRequested);
    bool bSaved = true;
    TestTrue(TEXT("saved is a bool"),
        Capture.Result->TryGetBoolField(TEXT("saved"), bSaved));
    TestFalse(TEXT("save:false reports saved:false"), bSaved);

    // In-memory ground truth: yaw 180 maps every position (x,y,z) -> (-x,-y,z).
    const float Tolerance = 1e-2f;
    const FMeshDescription* BakedMD = Mesh->GetMeshDescription(0);
    TestNotNull(TEXT("LOD0 mesh description available after bake"), BakedMD);
    if (BakedMD && BakedMD->Vertices().Num() == 3)
    {
        FStaticMeshConstAttributes BakedAttrs(*BakedMD);
        const FVector FirstVertex(BakedAttrs.GetVertexPositions()[FVertexID(0)]);
        TestEqual(TEXT("first vertex position mirrored by yaw 180"),
            FirstVertex, FVector(-100.0, 0.0, 0.0), Tolerance);
        const FVector FirstNormal(BakedAttrs.GetVertexInstanceNormals()[FVertexInstanceID(0)]);
        TestEqual(TEXT("up-facing normal invariant under yaw 180"),
            FirstNormal, FVector(0.0, 0.0, 1.0), Tolerance);
    }

    TestEqual(TEXT("one convex elem remains"), BodySetup->AggGeom.ConvexElems.Num(), 1);
    if (BodySetup->AggGeom.ConvexElems.Num() == 1
        && BodySetup->AggGeom.ConvexElems[0].VertexData.Num() == 4)
    {
        const FKConvexElem& BakedConvex = BodySetup->AggGeom.ConvexElems[0];
        TestEqual(TEXT("convex VertexData[0] mirrored by yaw 180"),
            BakedConvex.VertexData[0], FVector(-10.0, 0.0, 0.0), Tolerance);
        TestEqual(TEXT("convex ElemBox.Min.X recomputed from rotated hull"),
            BakedConvex.ElemBox.Min.X, -10.0, static_cast<double>(Tolerance));
    }

    TestEqual(TEXT("one box elem remains"), BodySetup->AggGeom.BoxElems.Num(), 1);
    if (BodySetup->AggGeom.BoxElems.Num() == 1)
    {
        const FKBoxElem& BakedBox = BodySetup->AggGeom.BoxElems[0];
        TestEqual(TEXT("box Center mirrored by yaw 180"),
            BakedBox.Center, FVector(-5.0, 0.0, 0.0), Tolerance);
        TestEqual(TEXT("box Rotation.Yaw magnitude is 180"),
            FMath::Abs(BakedBox.Rotation.Yaw), 180.0, static_cast<double>(Tolerance));
    }

    TestEqual(TEXT("one socket remains"), Mesh->Sockets.Num(), 1);
    if (Mesh->Sockets.Num() == 1 && Mesh->Sockets[0])
    {
        TestEqual(TEXT("socket RelativeLocation mirrored by yaw 180"),
            Mesh->Sockets[0]->RelativeLocation, FVector(-100.0, 0.0, 0.0), Tolerance);
    }

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStaticMeshBakeTransformRejectsNonPositiveScaleTest,
    "PinWright.static_mesh.bake_transform.RejectsNonPositiveScale",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FStaticMeshBakeTransformRejectsNonPositiveScaleTest::RunTest(const FString& Parameters)
{
    const FString PackagePath = FString::Printf(TEXT("/Game/PinWrightTests/SM_BakeTransform_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    UStaticMesh* Mesh = CreateBakeTransformFixtureMesh(PackagePath);
    TestNotNull(TEXT("Temporary StaticMesh created and built"), Mesh);
    if (!Mesh)
    {
        return false;
    }

    Mesh->AddToRoot();
    ON_SCOPE_EXIT
    {
        Mesh->RemoveFromRoot();
        CleanupTestAsset(PackagePath);
    };

    const double InvalidScales[] = { 0.0, -1.0 };
    for (const double InvalidScale : InvalidScales)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("assetPath"), Mesh->GetPathName());
        Payload->SetNumberField(TEXT("scale"), InvalidScale);

        FTestResponseCapture Capture;
        const bool bFound = InvokeHandlerWithCapture(
            TEXT("static_mesh.bake_transform"), Payload, Capture);

        TestTrue(TEXT("Handler found in registration list"), bFound);
        TestTrue(FString::Printf(TEXT("Handler responded (scale=%g)"), InvalidScale),
            Capture.bWasCalled);
        TestFalse(FString::Printf(TEXT("scale=%g is rejected"), InvalidScale),
            Capture.bSuccess);
        TestEqual(FString::Printf(TEXT("scale=%g error code is INVALID_ARGUMENT"), InvalidScale),
            Capture.ErrorCode, FString(TEXT("INVALID_ARGUMENT")));
    }

    return true;
}
