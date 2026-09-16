// Copyright (c) 2026 Alexander Penkin. MIT License.

// Live regression test: asset.search with parentClassPath must include Blueprint assets
// whose ParentClass is (or derives from) the specified native class.
//
// Counterfactual: before the BP parent-tag walk fix, the same call returns zero
// Blueprint-class rows for a transient UBlueprint with ParentClass=AActor, because
// FARFilter::ClassPaths+bRecursiveClasses only matches assets whose *own* class is
// in the subclass tree — Blueprint assets' own class is /Script/Engine.Blueprint,
// never the native parent.

#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Tests/TestUtils.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "GameFramework/Actor.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

// ============================================================================
// asset.search parentClassPath — must return Blueprint assets via parent-tag walk
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetSearchNativeSubclassFindsBlueprintAssetTest,
    "PinWright.asset.search.FindsBlueprintAsset",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetSearchNativeSubclassFindsBlueprintAssetTest::RunTest(const FString& Parameters)
{
    // 1. Create a transient Blueprint with ParentClass=AActor so the asset registry
    //    has a /Script/Engine.Blueprint-class asset whose ParentClassPath tag points
    //    to /Script/Engine.Actor.
    const FString AssetPath = TEXT("/Engine/Transient/TestBP_AActorChild_NativeSubclassLive");
    const FString AssetName = FPackageName::GetLongPackageAssetName(AssetPath);

    UPackage* Pkg = CreatePackage(*AssetPath);
    if (!TestNotNull(TEXT("Package created"), Pkg))
    {
        return true;
    }

    UBlueprint* BP = FKismetEditorUtilities::CreateBlueprint(
        AActor::StaticClass(),
        Pkg,
        FName(*AssetName),
        BPTYPE_Normal,
        UBlueprint::StaticClass(),
        UBlueprintGeneratedClass::StaticClass());

    if (!TestNotNull(TEXT("Blueprint created"), BP))
    {
        return true;
    }

    // 2. Force the asset registry to index the new in-memory package so its tags
    //    (including ParentClassPath) are available for tag-based queries.
    IAssetRegistry& AssetRegistry =
        FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();
    AssetRegistry.ScanPathsSynchronous({ TEXT("/Engine/Transient") }, /*bForceRescan=*/true);

    // 3. Invoke asset.search with parentClassPath="/Script/Engine.Actor".
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("query"), TEXT("*"));
    Payload->SetStringField(TEXT("parentClassPath"), TEXT("/Script/Engine.Actor"));
    Payload->SetStringField(TEXT("path"), TEXT("/Engine/Transient"));
    Payload->SetNumberField(TEXT("limit"), 500.0);

    FTestResponseCapture Capture;
    const bool bInvoked = InvokeHandlerWithCapture(TEXT("asset.search"), Payload, Capture);
    TestTrue(TEXT("asset.search handler found"), bInvoked);
    TestTrue(TEXT("handler sent a response"), Capture.bWasCalled);
    TestTrue(TEXT("response reports success"), Capture.bSuccess);

    if (!Capture.bSuccess || !Capture.Result.IsValid())
    {
        return true;
    }

    // 4. Parse the returned assets[] array and assert our BP is present by ObjectPath.
    //    The expected path is the package-path.asset-name form:
    //    "/Engine/Transient/TestBP_AActorChild_NativeSubclassLive.TestBP_AActorChild_NativeSubclassLive"
    const FString ExpectedObjectPath = FString::Printf(TEXT("%s.%s"), *AssetPath, *AssetName);

    const TArray<TSharedPtr<FJsonValue>>* AssetsArray = nullptr;
    if (!Capture.Result->TryGetArrayField(TEXT("assets"), AssetsArray) || !AssetsArray)
    {
        AddError(TEXT("Response missing 'assets' array"));
        return true;
    }

    bool bFound = false;
    for (const TSharedPtr<FJsonValue>& Val : *AssetsArray)
    {
        const TSharedPtr<FJsonObject>* Obj = nullptr;
        if (!Val->TryGetObject(Obj) || !Obj)
        {
            continue;
        }
        FString Path;
        (*Obj)->TryGetStringField(TEXT("path"), Path);
        if (Path == ExpectedObjectPath)
        {
            bFound = true;
            break;
        }
    }

    TestTrue(
        TEXT("asset.search with parentClassPath returns the Blueprint asset (not just BPGC instances)"),
        bFound);

    // Remove the transient BP from the asset registry so it doesn't bleed into
    // subsequent tests that query /Engine/Transient with parentClassPath=Actor.
    if (BP)
    {
        FAssetRegistryModule::AssetDeleted(BP);
        BP->ClearFlags(RF_Standalone | RF_Public);
        BP->RemoveFromRoot();
        BP->MarkAsGarbage();
    }
    if (Pkg)
    {
        Pkg->ClearFlags(RF_Standalone | RF_Public);
        Pkg->RemoveFromRoot();
        Pkg->MarkAsGarbage();
    }
    CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);

    return true;
}
