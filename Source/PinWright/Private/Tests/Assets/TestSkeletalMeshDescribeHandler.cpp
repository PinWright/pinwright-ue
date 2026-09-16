// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/Guid.h"
#include "Misc/ScopeExit.h"
#include "Tests/TestUtils.h"


#include "Engine/SkeletalMesh.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSkeletonDescribeMeshReturnsDumpShapeTest,
    "PinWright.skeleton.describe_mesh.ReturnsDumpShape",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSkeletonDescribeMeshReturnsDumpShapeTest::RunTest(const FString& Parameters)
{
    const FString PackagePath = FString::Printf(TEXT("/Game/PinWrightTests/SK_DescribeMesh_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    const FString AssetName = FPackageName::GetLongPackageAssetName(PackagePath);
    UPackage* Package = CreatePackage(*PackagePath);
    USkeletalMesh* Mesh = NewObject<USkeletalMesh>(
        Package, FName(*AssetName), RF_Public | RF_Standalone);

    TestNotNull(TEXT("Temporary SkeletalMesh created"), Mesh);
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
    Payload->SetStringField(TEXT("skeletalMeshPath"), Mesh->GetPathName());

    FTestResponseCapture Capture;
    const bool bFound = InvokeHandlerWithCapture(
        TEXT("skeleton.describe_mesh"), Payload, Capture);

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
    const TArray<TSharedPtr<FJsonValue>>* SectionsByLod = nullptr;
    const TArray<TSharedPtr<FJsonValue>>* NumTexCoordsByLod = nullptr;
    TestTrue(TEXT("trianglesByLod is an array"),
        Capture.Result->TryGetArrayField(TEXT("trianglesByLod"), TrianglesByLod));
    TestTrue(TEXT("verticesByLod is an array"),
        Capture.Result->TryGetArrayField(TEXT("verticesByLod"), VerticesByLod));
    TestTrue(TEXT("sectionsByLod is an array"),
        Capture.Result->TryGetArrayField(TEXT("sectionsByLod"), SectionsByLod));
    TestTrue(TEXT("numTexCoordsByLod is an array"),
        Capture.Result->TryGetArrayField(TEXT("numTexCoordsByLod"), NumTexCoordsByLod));

    TestTrue(TEXT("physicsAsset is a string"),
        Capture.Result->HasTypedField<EJson::String>(TEXT("physicsAsset")));
    TestTrue(TEXT("skeleton is a string"),
        Capture.Result->HasTypedField<EJson::String>(TEXT("skeleton")));

    return true;
}
