// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/Guid.h"
#include "Misc/ScopeExit.h"
#include "Tests/TestUtils.h"
#include "Utils/MeshRenderConsumerScan.h"

#include "BodySetupEnums.h"
#include "Components/ActorComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Misc/PackageName.h"
#include "PhysicsEngine/BodySetup.h"
#include "PhysicsEngine/BoxElem.h"
#include "UObject/Package.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStaticMeshDescribeReturnsDumpShapeTest,
    "PinWright.static_mesh.describe.ReturnsDumpShape",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FStaticMeshDescribeReturnsDumpShapeTest::RunTest(const FString& Parameters)
{
    const FString PackagePath = FString::Printf(TEXT("/Game/PinWrightTests/SM_DescribeStatic_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    const FString AssetName = FPackageName::GetLongPackageAssetName(PackagePath);
    UPackage* Package = CreatePackage(*PackagePath);
    UStaticMesh* Mesh = NewObject<UStaticMesh>(
        Package, FName(*AssetName), RF_Public | RF_Standalone);

    TestNotNull(TEXT("Temporary StaticMesh created"), Mesh);
    if (!Mesh)
    {
        return false;
    }

    Mesh->CreateBodySetup();
    UBodySetup* BodySetup = Mesh->GetBodySetup();
    TestNotNull(TEXT("Temporary StaticMesh body setup created"), BodySetup);
    if (!BodySetup)
    {
        return false;
    }
    BodySetup->CollisionTraceFlag = CTF_UseComplexAsSimple;
    BodySetup->AggGeom.BoxElems.Add(FKBoxElem(10.0f, 20.0f, 30.0f));

    Mesh->AddToRoot();
    ON_SCOPE_EXIT
    {
        Mesh->RemoveFromRoot();
        CleanupTestAsset(PackagePath);
    };

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), Mesh->GetPathName());

    FTestResponseCapture Capture;
    const bool bFound = InvokeHandlerWithCapture(
        TEXT("static_mesh.describe"), Payload, Capture);

    TestTrue(TEXT("Handler found in registration list"), bFound);
    TestTrue(TEXT("Handler responded"), Capture.bWasCalled);
    TestTrue(FString::Printf(TEXT("Handler succeeded (errorCode='%s')"), *Capture.ErrorCode),
        Capture.bSuccess);
    TestTrue(TEXT("Result returned"), Capture.Result.IsValid());
    if (!bFound || !Capture.bSuccess || !Capture.Result.IsValid())
    {
        return false;
    }

    const TSharedPtr<FJsonObject>* Bounds = nullptr;
    TestTrue(TEXT("bounds is an object"),
        Capture.Result->TryGetObjectField(TEXT("bounds"), Bounds));

    const TArray<TSharedPtr<FJsonValue>>* Materials = nullptr;
    TestTrue(TEXT("materials is an array"),
        Capture.Result->TryGetArrayField(TEXT("materials"), Materials));

    TestTrue(TEXT("lods is a number"),
        Capture.Result->HasTypedField<EJson::Number>(TEXT("lods")));

    const TArray<TSharedPtr<FJsonValue>>* TrianglesByLod = nullptr;
    const TArray<TSharedPtr<FJsonValue>>* VerticesByLod = nullptr;
    TestTrue(TEXT("trianglesByLod is an array"),
        Capture.Result->TryGetArrayField(TEXT("trianglesByLod"), TrianglesByLod));
    TestTrue(TEXT("verticesByLod is an array"),
        Capture.Result->TryGetArrayField(TEXT("verticesByLod"), VerticesByLod));

    const TArray<TSharedPtr<FJsonValue>>* Sections = nullptr;
    const TArray<TSharedPtr<FJsonValue>>* SlotUsage = nullptr;
    const TArray<TSharedPtr<FJsonValue>>* UvChannelsByLod = nullptr;
    TestTrue(TEXT("sections is an array"),
        Capture.Result->TryGetArrayField(TEXT("sections"), Sections));
    TestTrue(TEXT("slotUsage is an array"),
        Capture.Result->TryGetArrayField(TEXT("slotUsage"), SlotUsage));
    TestTrue(TEXT("uvChannelsByLod is an array"),
        Capture.Result->TryGetArrayField(TEXT("uvChannelsByLod"), UvChannelsByLod));
    TestTrue(TEXT("lightMapCoordinateIndex is a number"),
        Capture.Result->HasTypedField<EJson::Number>(TEXT("lightMapCoordinateIndex")));

    FString CollisionTraceFlag;
    TestTrue(TEXT("collisionTraceFlag is a string"),
        Capture.Result->TryGetStringField(TEXT("collisionTraceFlag"), CollisionTraceFlag));
    TestEqual(TEXT("collisionTraceFlag matches BodySetup"),
        CollisionTraceFlag, FString(TEXT("CTF_UseComplexAsSimple")));

    const TSharedPtr<FJsonObject>* Collision = nullptr;
    if (TestTrue(TEXT("collision is an object"),
        Capture.Result->TryGetObjectField(TEXT("collision"), Collision)))
    {
        const TSharedPtr<FJsonObject>* Elements = nullptr;
        if (TestTrue(TEXT("collision.elements is an object"),
            (*Collision)->TryGetObjectField(TEXT("elements"), Elements)))
        {
            double BoxCount = 0.0;
            double TaperedCapsuleCount = 0.0;
            TestTrue(TEXT("collision.elements.box is a number"),
                (*Elements)->TryGetNumberField(TEXT("box"), BoxCount));
            TestTrue(TEXT("collision.elements.taperedCapsule is a number"),
                (*Elements)->TryGetNumberField(TEXT("taperedCapsule"), TaperedCapsuleCount));
            TestEqual(TEXT("collision.elements.box matches BodySetup"),
                static_cast<int32>(BoxCount), BodySetup->AggGeom.BoxElems.Num());
            TestEqual(TEXT("collision.elements.taperedCapsule matches BodySetup"),
                static_cast<int32>(TaperedCapsuleCount), BodySetup->AggGeom.TaperedCapsuleElems.Num());
        }
    }

    return true;
}

// E-static-mesh-describe-no-live-consumer-report: model.compile refuses a rebuild in place
// with MESH_REBUILD_CONSUMER_NOT_QUIESCABLE when a live component's scene proxy still caches
// the mesh's render data. Until this field existed the refusal was the FIRST time a caller
// heard such a component was there - static_mesh.describe, the read whose whole job is to
// tell you about a mesh, said nothing about who was drawing it.
//
// What is pinned here is that the read reports the same set the GUARD acts on, not a second
// opinion computed beside it. A report from an independent walk could drift and answer "safe"
// for a rebuild the guard then refuses, which is worse than reporting nothing. The scan
// itself - that it finds live proxy-holding components and skips unregistered ones - is
// pinned on the guard side by PinWright.Model.RebuildRenderGuard.*.
namespace
{
    // Prefixed rather than generically named: unity merges test TUs, and an anonymous-namespace
    // helper called something like SortedStrings would collide with its neighbours.
    TArray<FString> StaticMeshDescribeTest_SortedStrings(const TArray<TSharedPtr<FJsonValue>>& Values)
    {
        TArray<FString> Out;
        Out.Reserve(Values.Num());
        for (const TSharedPtr<FJsonValue>& Value : Values)
        {
            FString Text;
            if (Value.IsValid() && Value->TryGetString(Text))
            {
                Out.Add(Text);
            }
        }
        Out.Sort();
        return Out;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStaticMeshDescribeReportsRenderConsumersTest,
    "PinWright.static_mesh.describe.ReportsLiveRenderConsumers",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FStaticMeshDescribeReportsRenderConsumersTest::RunTest(const FString& Parameters)
{
    const FString PackagePath = FString::Printf(TEXT("/Game/PinWrightTests/SM_DescribeConsumers_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    const FString AssetName = FPackageName::GetLongPackageAssetName(PackagePath);
    UPackage* Package = CreatePackage(*PackagePath);
    UStaticMesh* Mesh = NewObject<UStaticMesh>(
        Package, FName(*AssetName), RF_Public | RF_Standalone);

    TestNotNull(TEXT("Temporary StaticMesh created"), Mesh);
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

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), Mesh->GetPathName());

    FTestResponseCapture Capture;
    const bool bFound = InvokeHandlerWithCapture(
        TEXT("static_mesh.describe"), Payload, Capture);

    TestTrue(TEXT("Handler found in registration list"), bFound);
    TestTrue(FString::Printf(TEXT("Handler succeeded (errorCode='%s')"), *Capture.ErrorCode),
        Capture.bSuccess);
    if (!bFound || !Capture.bSuccess || !Capture.Result.IsValid())
    {
        return false;
    }

    const TSharedPtr<FJsonObject>* RenderConsumers = nullptr;
    if (!TestTrue(TEXT("rebuildRenderConsumers is an object - the read now says something about "
                       "who is holding the mesh instead of leaving the compile refusal to say it"),
        Capture.Result->TryGetObjectField(TEXT("rebuildRenderConsumers"), RenderConsumers)))
    {
        return false;
    }

    const TArray<TSharedPtr<FJsonValue>>* ScannedClasses = nullptr;
    const TArray<TSharedPtr<FJsonValue>>* Components = nullptr;
    const bool bHasClasses = TestTrue(TEXT("rebuildRenderConsumers.scannedClasses is an array"),
        (*RenderConsumers)->TryGetArrayField(TEXT("scannedClasses"), ScannedClasses));
    const bool bHasComponents = TestTrue(TEXT("rebuildRenderConsumers.components is an array"),
        (*RenderConsumers)->TryGetArrayField(TEXT("components"), Components));
    if (!bHasClasses || !bHasComponents)
    {
        return false;
    }

    // The guard's own table and walk, run here for comparison. Nothing spawns or destroys a
    // component between the handler call and these two calls, so any difference is the report
    // disagreeing with the guard rather than the editor moving underneath both.
    TArray<UClass*> ExpectedClasses;
    ExpectedClasses.Add(UStaticMeshComponent::StaticClass());
    ExpectedClasses.Append(PinWrightMeshRebuild::ResolveStaleRenderStateComponentClasses());

    TArray<FString> ExpectedClassPaths;
    ExpectedClassPaths.Reserve(ExpectedClasses.Num());
    for (const UClass* ExpectedClass : ExpectedClasses)
    {
        ExpectedClassPaths.Add(ExpectedClass->GetPathName());
    }
    ExpectedClassPaths.Sort();

    // Joined rather than compared as arrays: the automation framework's generic TestEqual
    // reports a mismatch through ValueType::ToString(), which TArray does not have, and a
    // joined string names the offending entry in the failure message.
    TestEqual(TEXT("scannedClasses is the guard's resolved candidate table, so an empty components "
                   "list can be read as 'looked for, found none' rather than 'never looked'"),
        FString::Join(StaticMeshDescribeTest_SortedStrings(*ScannedClasses), TEXT(", ")),
        FString::Join(ExpectedClassPaths, TEXT(", ")));

    TArray<UStaticMesh*> TargetMeshes;
    TargetMeshes.Add(Mesh);
    TArray<FString> ExpectedComponentPaths;
    for (const UActorComponent* Component : PinWrightMeshRebuild::ScanForStaticMeshRebuildConsumers(
             TargetMeshes))
    {
        ExpectedComponentPaths.Add(Component->GetPathName());
    }
    ExpectedComponentPaths.Sort();

    TestEqual(TEXT("components is the guard's own live-consumer scan, so the read cannot answer "
                   "'safe' for a rebuild MESH_REBUILD_CONSUMER_NOT_QUIESCABLE would refuse"),
        FString::Join(StaticMeshDescribeTest_SortedStrings(*Components), TEXT(", ")),
        FString::Join(ExpectedComponentPaths, TEXT(", ")));

    // Non-vacuity. Everything above compares the report against the shipped scan, which an
    // empty hardcoded report would satisfy on a host that happens to have no candidates. The
    // candidate class the crash was filed for must actually reach the response.
    if (FindObject<UClass>(nullptr, TEXT("/Script/Niagara.NiagaraComponent")) == nullptr)
    {
        AddWarning(TEXT("PINWRIGHT_ASSERTIONS_SKIPPED: Niagara is not enabled on this host, so "
                        "only the always-scanned StaticMeshComponent class is present."));
        return true;
    }

    TestTrue(TEXT("scannedClasses names UNiagaraComponent - the class whose mesh renderer aborted "
                  "the render thread after a rebuild, so the report is wired to the shipped table "
                  "and not to an empty stub"),
        StaticMeshDescribeTest_SortedStrings(*ScannedClasses).Contains(
            FString(TEXT("/Script/Niagara.NiagaraComponent"))));

    return true;
}
