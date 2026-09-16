// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression test for E-asset-get-doc-promises-tags.
//
// asset.get's registered summary advertises "asset registry tags" as part of its
// output (AssetManageHandler.cpp asset.get REGISTER_RPC_HANDLER), but the result
// historically carried ONLY {name, path, class, packagePath} and no tags field —
// so the doc named a field the verb never returned, and agents reading the doc for
// a registry value (a material's BlendMode, a mesh's NaniteEnabled) were misdirected.
//
// The fix emits AssetData.TagsAndValues as a typed name->value `tags` object on the
// asset.get result, mirroring asset.get_metadata. This test builds a real in-memory
// Blueprint (which always carries asset-registry tags — ParentClass / BlueprintType /
// NativeParentClass via UBlueprint::GetAssetRegistryTags), notifies the asset registry
// so asset.get's FindAssetData resolves it, then dispatches asset.get through the real
// production dispatcher and asserts the result carries a non-empty `tags` map.
//
// Counterfactual: revert the fix (drop the tags map from asset.get's result) and the
// `result.tags` field is absent, failing this test — pinning the doc-vs-output contract.

#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "GameFramework/Actor.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"

#include "Dispatch/RpcDispatcher.h"
#include "Tests/Infra/DispatcherTestHelpers.h"
#include "Tests/TestUtils.h"

using DispatcherTestHelpers::MakeDispatcher;
using DispatcherTestHelpers::Dispatch;

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetGetReturnsRegistryTagsTest,
    "PinWright.asset.get.ReturnsRegistryTags",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAssetGetReturnsRegistryTagsTest::RunTest(const FString& Parameters)
{
    // Build a real in-memory asset so asset.get's FindAssetData/DoesAssetExist resolve it.
    const FString PackagePath = FString::Printf(
        TEXT("/Game/__PW_GatewayTests/AssetGetTags_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));

    UPackage* Pkg = CreatePackage(*PackagePath);
    if (!TestNotNull(TEXT("test package created"), Pkg))
    {
        return true;
    }

    const FName AssetName(*FPackageName::GetLongPackageAssetName(PackagePath));
    UBlueprint* BP = FKismetEditorUtilities::CreateBlueprint(
        AActor::StaticClass(), Pkg, AssetName,
        BPTYPE_Normal, UBlueprint::StaticClass(),
        UBlueprintGeneratedClass::StaticClass());

    if (!TestNotNull(TEXT("test blueprint created"), BP))
    {
        CleanupTestAsset(PackagePath);
        return true;
    }

    // Make the registry aware of the new asset so DoesAssetExist / FindAssetData
    // (the asset.get code path) resolve it without a disk save.
    FAssetRegistryModule::AssetCreated(BP);

    // Dispatch asset.get through the real production dispatcher.
    DispatcherTestHelpers::FSinkPtr Sink;
    FRpcDispatcher Dispatcher;
    MakeDispatcher(Sink, Dispatcher);

    TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
    Params->SetStringField(TEXT("assetPath"), PackagePath);

    bool bSuccess = false;
    FString ErrorCode;
    TSharedPtr<FJsonObject> Result;
    Dispatch(Dispatcher, Sink, TEXT("asset.get"),
        TEXT("req-asset-get-tags"), Params, bSuccess, Result, ErrorCode);

    TestTrue(TEXT("asset.get reports success"), bSuccess);
    // asset.get returns the success envelope { success, result:{ name, path, class,
    // packagePath, tags } } — the summary fields (and the doc-promised tags map) live
    // on the nested `result` object, not on the top-level envelope. Drill in before
    // asserting, mirroring asset.get's actual response shape.
    const TSharedPtr<FJsonObject>* ResultObj = nullptr;
    if (TestTrue(TEXT("asset.get returned a result object"),
            Result.IsValid() && Result->TryGetObjectField(TEXT("result"), ResultObj)
            && ResultObj && (*ResultObj).IsValid()))
    {
        // The doc promises "asset registry tags" — the result must carry a tags object.
        const TSharedPtr<FJsonObject>* TagsObj = nullptr;
        const bool bHasTags = (*ResultObj)->TryGetObjectField(TEXT("tags"), TagsObj);
        TestTrue(TEXT("asset.get result carries a 'tags' object (the doc-promised field)"), bHasTags);

        // A freshly created Blueprint always carries asset-registry tags
        // (e.g. ParentClass / NativeParentClass / BlueprintType), so the map is non-empty.
        if (bHasTags && TagsObj && (*TagsObj).IsValid())
        {
            TestTrue(TEXT("asset.get 'tags' map is non-empty for a Blueprint"),
                (*TagsObj)->Values.Num() > 0);

            // Num() > 0 alone would be satisfied by any single junk entry, or by the
            // tags of a different asset. Name a tag UBlueprint::GetAssetRegistryTags is
            // contractually required to emit, and read its value: that pins the map to
            // THIS asset. (The full tag set is engine-version dependent, so only the
            // guaranteed ones are asserted.)
            FString NativeParentClass;
            const bool bHasNativeParent =
                (*TagsObj)->TryGetStringField(TEXT("NativeParentClass"), NativeParentClass);
            TestTrue(TEXT("asset.get tags carry the Blueprint's NativeParentClass tag"),
                bHasNativeParent);
            TestTrue(TEXT("NativeParentClass tag names the AActor parent this test created"),
                bHasNativeParent && NativeParentClass.Contains(TEXT("Actor")));
        }
    }

    CleanupTestAsset(PackagePath);
    return true;
}
