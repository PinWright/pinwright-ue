// Copyright (c) 2026 Alexander Penkin. MIT License.

// Unit tests for Asset domain handlers.
// Covers all 38 handlers across: AssetManageHandler, AssetMaterialHandler,
// AssetMetadataHandler, AssetQueryHandler, AssetWorkflowHandler.
//
// Test strategy:
//   - Handlers with RPC_PARAM_REQ: call with an EMPTY payload to exercise the
//     "missing required param" error-return path.
//   - Handlers with only RPC_PARAM_OPT / RPC_NO_PARAMS: call with a realistic
//     fake payload to exercise the happy-path parse branch without crashing.
//   All tests assert only that InvokeHandler() returns true (handler found and
//   dispatched).  Actual UE editor APIs are absent in test builds, so responses
//   will be error codes — that is expected and acceptable.

#include "Misc/AutomationTest.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/HandlerRegistration.h"
#include "Handlers/ParamSpec.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonTypes.h"
#include "Tests/TestUtils.h"
#include "Tests/TestSkipReporting.h"
#include "Tests/IrCore/IrTestFixture.h"
#include "Tests/Utility/AssetDumpMismatchedNameFixture.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Factories/MaterialFactoryNew.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "HAL/FileManager.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

// ============================================================================
// AssetManageHandler — asset.delete (optional params)
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDeleteValidParamsTest,
    "PinWright.asset.delete.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAssetDeleteValidParamsTest::RunTest(const FString& Parameters)
{
    // Provide a single path; handler will attempt deletion and fail gracefully
    // since the asset does not exist in a test environment.
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("path"), TEXT("/Game/TestAssets/NonExistentAsset"));
    bSuppressLogErrors = true;
    TestTrue(TEXT("asset.delete handler found"), InvokeHandler(TEXT("asset.delete"), Payload));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDeleteEmptyPathsTest,
    "PinWright.asset.delete.EmptyPathsReturnsError",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAssetDeleteEmptyPathsTest::RunTest(const FString& Parameters)
{
    // No path or paths field — exercises the "No paths provided" error branch.
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    TestTrue(TEXT("asset.delete handler found"), InvokeHandler(TEXT("asset.delete"), Payload));
    return true;
}

// ============================================================================
// AssetManageHandler — asset.delete verdict vs. the measured existsAfter
// ============================================================================
// Regression guard for B-asset-delete-force-delete-leaves-uasset-on-disk: the engine
// reported the delete, the .uasset stayed on disk, and the entry emitted
// deleteReported:true beside its own existsAfter:true — a claim of work it had not done.
//
// The fixture reproduces the engine's silent skip deterministically instead of depending on
// whichever native holder the original report hit: two RF_Standalone assets are saved into
// ONE package and only the first is deleted. ObjectTools::ForceDeleteObjects tears the target
// down and returns 1 (so UEditorAssetLibrary::DeleteAsset returns true), then
// CleanupAfterSuccessfulDelete runs GatherObjectReferencersForDeletion on the package, finds
// the surviving sibling (GARBAGE_COLLECTION_KEEPFLAGS is RF_Standalone in the editor), drops
// the package from its delete list and never touches the file — no error, no log line. That
// path is unchanged across UE 5.3-5.8.
//
// If a future engine version does delete the file, existsAfter goes false and every equality
// below still holds: this pins the plugin's reconciliation, not the engine's behaviour.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDeleteVerdictMatchesExistsAfterTest,
    "PinWright.asset.delete.VerdictMatchesExistsAfter",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAssetDeleteVerdictMatchesExistsAfterTest::RunTest(const FString& Parameters)
{
    // A two-asset package is unusual enough that the registry and the delete path both log
    // about it; the assertions below are what this test judges on.
    bSuppressLogErrors = true;

    const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString FolderPath = FString::Printf(TEXT("/Game/__PW_DeleteTests/%s"), *Suffix);
    const FString PackagePath = FString::Printf(TEXT("%s/PW_DeleteVerdict_%s"), *FolderPath, *Suffix);
    const FString TargetName = FString::Printf(TEXT("MI_Target_%s"), *Suffix);
    const FString KeeperName = FString::Printf(TEXT("MI_Keeper_%s"), *Suffix);
    const FString TargetObjectPath = FString::Printf(TEXT("%s.%s"), *PackagePath, *TargetName);
    const FString KeeperObjectPath = FString::Printf(TEXT("%s.%s"), *PackagePath, *KeeperName);

    FString PackageFilename;
    if (!TestTrue(TEXT("fixture package path resolves to a filename"),
            FPackageName::TryConvertLongPackageNameToFilename(
                PackagePath, PackageFilename, FPackageName::GetAssetPackageExtension())))
    {
        return true;
    }

    // Teardown first: the keeper goes before the target, so the force-delete of the keeper
    // leaves the package unreferenced and takes the shared .uasset with it.
    ON_SCOPE_EXIT
    {
        CleanupTestAsset(KeeperObjectPath);
        CleanupTestAsset(TargetObjectPath);
        FString FolderFilename;
        if (FPackageName::TryConvertLongPackageNameToFilename(FolderPath, FolderFilename, TEXT("")))
        {
            IFileManager::Get().DeleteDirectory(*FolderFilename, /*RequireExists=*/false, /*Tree=*/true);
        }
    };

    UPackage* Package = CreatePackage(*PackagePath);
    if (!TestNotNull(TEXT("fixture package created"), Package))
    {
        return true;
    }

    // No Parent on either instance: keeps the fixture self-contained and off the shader path.
    UMaterialInstanceConstant* Target = NewObject<UMaterialInstanceConstant>(
        Package, FName(*TargetName), RF_Public | RF_Standalone);
    UMaterialInstanceConstant* Keeper = NewObject<UMaterialInstanceConstant>(
        Package, FName(*KeeperName), RF_Public | RF_Standalone);
    if (!TestNotNull(TEXT("delete target created"), Target)
        || !TestNotNull(TEXT("keeper sibling created"), Keeper))
    {
        return true;
    }

    bool bSaved = false;
    {
        AssetDumpMismatchedNameFixture::FScopedDisableValidateOnSaveReflect DisableValidation;
        FSavePackageArgs SaveArgs;
        SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
        SaveArgs.SaveFlags = SAVE_NoError;
        bSaved = UPackage::SavePackage(Package, Target, *PackageFilename, SaveArgs);
    }
    if (!TestTrue(TEXT("fixture package saved to disk"), bSaved))
    {
        return true;
    }

    IAssetRegistry& Registry =
        FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
    Registry.ScanFilesSynchronous({PackageFilename}, /*bForceRescan=*/true);

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("path"), TargetObjectPath);

    FTestResponseCapture Capture;
    if (!TestTrue(TEXT("asset.delete handler found"),
            InvokeHandlerWithCapture(TEXT("asset.delete"), Payload, Capture))
        || !TestTrue(TEXT("asset.delete sent a response"), Capture.bWasCalled)
        || !TestTrue(TEXT("asset.delete response carries a result"), Capture.Result.IsValid()))
    {
        return true;
    }

    const TSharedPtr<FJsonObject> Entry = JsonArrayFindObjectByStringField(
        Capture.Result, TEXT("results"), TEXT("path"), TargetObjectPath);
    if (!TestTrue(TEXT("results[] carries an entry for the requested path"), Entry.IsValid()))
    {
        return true;
    }

    bool bExistsAfter = false;
    bool bDeleteReported = false;
    bool bDeleted = false;
    bool bSuccess = false;
    Entry->TryGetBoolField(TEXT("existsAfter"), bExistsAfter);
    Entry->TryGetBoolField(TEXT("deleteReported"), bDeleteReported);
    Entry->TryGetBoolField(TEXT("deleted"), bDeleted);
    Capture.Result->TryGetBoolField(TEXT("success"), bSuccess);

    // The defect: the engine's raw return was emitted as deleteReported, so the entry
    // claimed a delete it had not performed right next to its own existsAfter.
    TestTrue(TEXT("deleteReported agrees with the measured existsAfter"),
        bDeleteReported == !bExistsAfter);
    TestTrue(TEXT("deleted agrees with the measured existsAfter"), bDeleted == !bExistsAfter);
    TestTrue(TEXT("success agrees with the measured existsAfter"), bSuccess == !bExistsAfter);

    // A surviving .uasset must read as still existing, whatever the asset registry now says.
    const bool bFileOnDisk = IFileManager::Get().FileExists(*PackageFilename);
    TestTrue(TEXT("existsAfter is set whenever the .uasset survived"), bExistsAfter || !bFileOnDisk);

    if (bExistsAfter)
    {
        // Name the real holder: referencingBlueprints answers the on-disk question and is
        // empty here, because what blocked the delete is the sibling inside the package.
        const TArray<TSharedPtr<FJsonValue>>* Referencers = nullptr;
        if (TestTrue(TEXT("failed entry lists inMemoryReferencers"),
                Entry->TryGetArrayField(TEXT("inMemoryReferencers"), Referencers)
                    && Referencers != nullptr))
        {
            bool bNamesKeeper = false;
            for (const TSharedPtr<FJsonValue>& Val : *Referencers)
            {
                if (Val.IsValid() && Val->AsString().Contains(KeeperName))
                {
                    bNamesKeeper = true;
                    break;
                }
            }
            TestTrue(TEXT("inMemoryReferencers names the holder that blocked the delete"),
                bNamesKeeper);
        }
        TestTrue(TEXT("a surviving path carries a failureHint"),
            Capture.Result->HasField(TEXT("failureHint")));
    }

    return true;
}

// ============================================================================
// AssetManageHandler — asset.create_folder (optional params)
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetCreateFolderValidParamsTest,
    "PinWright.asset.create_folder.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAssetCreateFolderValidParamsTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("path"), TEXT("/Game/TestFolder"));
    TestTrue(TEXT("asset.create_folder handler found"),
             InvokeHandler(TEXT("asset.create_folder"), Payload));
    CleanupTestAsset(TEXT("/Game/TestFolder"));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetCreateFolderEmptyPathTest,
    "PinWright.asset.create_folder.EmptyPathReturnsError",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAssetCreateFolderEmptyPathTest::RunTest(const FString& Parameters)
{
    // Both optional path aliases missing — exercises the INVALID_ARGUMENT branch.
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    TestTrue(TEXT("asset.create_folder handler found"),
             InvokeHandler(TEXT("asset.create_folder"), Payload));
    return true;
}

// ============================================================================
// AssetManageHandler — asset.list (all optional)
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetListValidParamsTest,
    "PinWright.asset.list.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAssetListValidParamsTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("path"), TEXT("/Game/__Automation_NoAssets__"));
    Payload->SetBoolField(TEXT("recursive"), false);
    TestTrue(TEXT("asset.list handler found"), InvokeHandler(TEXT("asset.list"), Payload));
    return true;
}

// ============================================================================
// AssetManageHandler — asset.search
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetSearchValidQueryTest,
    "PinWright.asset.search.ValidQueryNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAssetSearchValidQueryTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("query"), TEXT("TestMesh"));
    Payload->SetNumberField(TEXT("limit"), 10.0);
    TestTrue(TEXT("asset.search handler found"), InvokeHandler(TEXT("asset.search"), Payload));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetSearchNativeSubclassTest,
    "PinWright.asset.search.NativeSubclass",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAssetSearchNativeSubclassTest::RunTest(const FString& Parameters)
{
    // asset.search with parentClassPath must find BPs via the asset registry's native recursive
    // class filter. Uses /Script/CoreUObject.Object as the parent — every BP in any non-empty
    // registry terminates at UObject, so this exercises the feature without depending on any
    // specific project content.
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("query"), TEXT("*"));
    Payload->SetStringField(TEXT("parentClassPath"), TEXT("/Script/CoreUObject.Object"));
    Payload->SetNumberField(TEXT("limit"), 100.0);

    FTestResponseCapture Capture;
    const bool bInvoked = InvokeHandlerWithCapture(TEXT("asset.search"), Payload, Capture);
    TestTrue(TEXT("asset.search handler found"), bInvoked);
    TestTrue(TEXT("response reports success"), Capture.bSuccess);
    TestTrue(TEXT("Capture.Result is valid"), Capture.Result.IsValid());
    if (!Capture.Result.IsValid()) return true;

    int32 Count = 0;
    Capture.Result->TryGetNumberField(TEXT("count"), Count);

    if (Count == 0)
    {
        // Registry genuinely empty (headless context with no content scanned). The feature can't be
        // exercised here; don't fail — but make it visible so reruns with real content can surface.
        PinWrightTestSkip::SkipAssertions(*this, TEXT("asset-registry-empty"),
            TEXT("asset.search returned 0 results for parentClassPath=/Script/CoreUObject.Object — asset registry appears empty; parent-class filter could not be exercised."));
        return true;
    }

    // Pin the feature: with parentClassPath set, the handler unions an FARFilter recursive
    // class match (any asset whose own class derives from the parent) with a Blueprint
    // parent-tag walk (BP assets whose ParentClass tag resolves into the parent's subclass
    // tree). Using /Script/CoreUObject.Object as parent, the FARFilter half matches every
    // asset class, so the result set is broad — but the parent-tag walk must still surface
    // at least one Blueprint asset in any non-empty registry. Asserting this avoids
    // depending on specific project content while still pinning the BP-tag walk path.
    const TArray<TSharedPtr<FJsonValue>>* Assets = nullptr;
    TestTrue(TEXT("result has assets array"), Capture.Result->TryGetArrayField(TEXT("assets"), Assets));
    if (!Assets) return true;

    int32 BlueprintCount = 0;
    for (const TSharedPtr<FJsonValue>& Val : *Assets)
    {
        const TSharedPtr<FJsonObject>* Entry = nullptr;
        if (Val.IsValid() && Val->TryGetObject(Entry) && Entry && (*Entry)->HasField(TEXT("class")))
        {
            if ((*Entry)->GetStringField(TEXT("class")) == TEXT("Blueprint"))
            {
                ++BlueprintCount;
            }
        }
    }
    TestTrue(TEXT("at least one returned asset is a Blueprint (parent-tag walk surfaces BP assets)"),
        BlueprintCount > 0);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetSearchMutuallyExclusiveFiltersTest,
    "PinWright.asset.search.MutuallyExclusiveFilters",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAssetSearchMutuallyExclusiveFiltersTest::RunTest(const FString& Parameters)
{
    // Passing both classPathFilter and parentClassPath together must return an error.
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("query"), TEXT("*"));
    Payload->SetStringField(TEXT("classPathFilter"), TEXT("/Script/Engine.Blueprint"));
    Payload->SetStringField(TEXT("parentClassPath"), TEXT("/Script/Engine.Actor"));

    FTestResponseCapture Capture;
    const bool bInvoked = InvokeHandlerWithCapture(TEXT("asset.search"), Payload, Capture);
    TestTrue(TEXT("asset.search handler found"), bInvoked);
    TestFalse(TEXT("response must report failure"), Capture.bSuccess);

    // Error message must mention "together" or "cannot" to pin the validation wording.
    bool bErrorMentionsConflict = false;
    if (!Capture.Message.IsEmpty())
    {
        bErrorMentionsConflict = Capture.Message.Contains(TEXT("together"), ESearchCase::IgnoreCase)
            || Capture.Message.Contains(TEXT("cannot"), ESearchCase::IgnoreCase);
    }
    TestTrue(TEXT("error message mentions the mutual-exclusion constraint"), bErrorMentionsConflict);
    return true;
}

// ============================================================================
// AssetMaterialHandler — asset.get_material_stats
// ============================================================================

// Regression test for B-material-stats-instruction-count-hardcoded.
//
// The handler used to write a fixed `int32 InstructionCount = -1;` straight into
// the stats object via SetNumberField, so every material — no matter how trivial
// or heavy — reported instructionCount:-1 dressed up as a real measured statistic
// next to the genuine shadingModel/samplerCount fields. The fix stops fabricating a
// number and emits an honest JSON null when the value is not computed.
//
// Counterfactual: if the fix is reverted to SetNumberField(-1), instructionCount is
// a JSON Number (-1) instead of Null, and the EJson::Null assertion below fails.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetGetMaterialStatsInstructionCountNotHardcodedTest,
    "PinWright.asset.get_material_stats.InstructionCountNotHardcoded",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAssetGetMaterialStatsInstructionCountNotHardcodedTest::RunTest(const FString& Parameters)
{
    // Build a real on-disk-registered scratch material so the handler runs its full
    // stats-building path (LoadAsset → EnsureIsComplete → emit stats), not an early-out.
    IrTest::FScratchAsset Scratch(TEXT("M_MatStatsInstrCount"));
    UMaterial* Material =
        IrTest::CreateFactoryScratchAsset<UMaterial, UMaterialFactoryNew>(Scratch);
    if (!TestNotNull(TEXT("scratch material created"), Material))
    {
        return false;
    }

    // ObjectPath form (Package.Asset) so DoesAssetExist / LoadAsset resolve the
    // freshly-created in-memory asset (matches the asset.save integrity-gate test).
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), ToObjectPath(Scratch.PackagePath));

    FTestResponseCapture Capture;
    TestTrue(TEXT("asset.get_material_stats handler found"),
        InvokeHandlerWithCapture(TEXT("asset.get_material_stats"), Payload, Capture));
    TestTrue(TEXT("asset.get_material_stats succeeded for a real material"), Capture.bSuccess);
    if (!Capture.bSuccess || !Capture.Result.IsValid())
    {
        return false;
    }

    const TSharedPtr<FJsonObject>* StatsObj = nullptr;
    TestTrue(TEXT("response carries a stats object"),
        Capture.Result->TryGetObjectField(TEXT("stats"), StatsObj));
    if (!StatsObj || !(*StatsObj).IsValid())
    {
        return false;
    }

    // The real sibling field must still be present — proves we reached the stats block
    // and are asserting on the live handler output, not a degenerate response.
    FString ShadingModel;
    TestTrue(TEXT("stats still carries a real shadingModel"),
        (*StatsObj)->TryGetStringField(TEXT("shadingModel"), ShadingModel));

    // The defect: instructionCount must NOT be a fabricated number (the old -1). It is
    // present but emitted as JSON null ("not computed"), never read as a real count.
    const TSharedPtr<FJsonValue>* InstrField = (*StatsObj)->Values.Find(TEXT("instructionCount"));
    TestTrue(TEXT("stats carries an instructionCount field"),
        InstrField != nullptr && InstrField->IsValid());
    if (InstrField && InstrField->IsValid())
    {
        TestEqual(TEXT("instructionCount is JSON null, not a hardcoded -1 number"),
            (*InstrField)->Type, EJson::Null);
    }

    return true;
}

// ============================================================================
// AssetMetadataHandler — asset.get_dependencies_classified
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetGetDependenciesClassifiedInvalidModeTest,
    "PinWright.asset.get_dependencies_classified.InvalidModeReturnsError",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAssetGetDependenciesClassifiedInvalidModeTest::RunTest(const FString& Parameters)
{
    // Provide a valid assetPath but an invalid mode — exercises the INVALID_MODE branch.
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), TEXT("/Game/TestAssets/MyAsset"));
    Payload->SetStringField(TEXT("mode"), TEXT("nonexistent_mode"));
    TestTrue(TEXT("asset.get_dependencies_classified handler found"),
             InvokeHandler(TEXT("asset.get_dependencies_classified"), Payload));
    return true;
}

// ============================================================================
// AssetQueryHandler — asset.find_by_tag
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetFindByTagValidParamsTest,
    "PinWright.asset.find_by_tag.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAssetFindByTagValidParamsTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("tag"), TEXT("MyTag"));
    Payload->SetStringField(TEXT("path"), TEXT("/Game/__Automation_NoAssets__"));
    TestTrue(TEXT("asset.find_by_tag handler found"),
             InvokeHandler(TEXT("asset.find_by_tag"), Payload));
    return true;
}

// ============================================================================
// AssetQueryHandler — asset.search_assets (all optional)
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetSearchAssetsValidParamsTest,
    "PinWright.asset.search_assets.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAssetSearchAssetsValidParamsTest::RunTest(const FString& Parameters)
{
    TArray<TSharedPtr<FJsonValue>> PackagePaths;
    PackagePaths.Add(MakeShared<FJsonValueString>(TEXT("/Game/__Automation_NoAssets__")));

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetArrayField(TEXT("packagePaths"), PackagePaths);
    Payload->SetBoolField(TEXT("recursivePaths"), false);
    Payload->SetNumberField(TEXT("limit"), 5.0);
    TestTrue(TEXT("asset.search_assets handler found"),
             InvokeHandler(TEXT("asset.search_assets"), Payload));
    return true;
}

// ============================================================================
// AssetWorkflowHandler — asset.fixup_redirectors (all optional)
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetFixupRedirectorsValidParamsTest,
    "PinWright.asset.fixup_redirectors.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAssetFixupRedirectorsValidParamsTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("directoryPath"), TEXT("/Game/__Automation_NoAssets__"));
    Payload->SetBoolField(TEXT("checkoutFiles"), false);
    TestTrue(TEXT("asset.fixup_redirectors handler found"),
             InvokeHandler(TEXT("asset.fixup_redirectors"), Payload));
    return true;
}

// ============================================================================
// AssetWorkflowHandler — asset.generate_lods (all optional)
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetGenerateLodsValidParamsTest,
    "PinWright.asset.generate_lods.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAssetGenerateLodsValidParamsTest::RunTest(const FString& Parameters)
{
    // No assetPaths provided — exercises the "landscapePath or assetPaths required" branch.
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetNumberField(TEXT("lodCount"), 4.0);
    TestTrue(TEXT("asset.generate_lods handler found"),
             InvokeHandler(TEXT("asset.generate_lods"), Payload));
    return true;
}

// ============================================================================
// AssetWorkflowHandler — asset.find_objects_by_tag
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetFindObjectsByTagValidParamsTest,
    "PinWright.asset.find_objects_by_tag.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAssetFindObjectsByTagValidParamsTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("tag"), TEXT("PlayerSpawn"));
    Payload->SetBoolField(TEXT("searchActors"), true);
    Payload->SetBoolField(TEXT("searchComponents"), false);
    Payload->SetNumberField(TEXT("maxResults"), 10.0);
    TestTrue(TEXT("asset.find_objects_by_tag handler found"),
             InvokeHandler(TEXT("asset.find_objects_by_tag"), Payload));
    return true;
}

// ============================================================================
// AssetManageHandler — asset.list (short class name must not fire ensure)
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetListShortClassNameNoEnsureTest,
    "PinWright.asset.list.ShortClassNameNoEnsure",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAssetListShortClassNameNoEnsureTest::RunTest(const FString& Parameters)
{
    // asset.list must route short class names through ResolveUClass so the
    // FTopLevelAssetPath ensure never fires; if it did fire, the automation
    // framework would surface it as an error automatically.
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    TSharedPtr<FJsonObject> Filter = MakeShared<FJsonObject>();
    Filter->SetStringField(TEXT("class"), TEXT("WidgetBlueprint"));
    Filter->SetStringField(TEXT("pathStartsWith"), TEXT("/Engine"));
    Payload->SetObjectField(TEXT("filter"), Filter);

    FTestResponseCapture Capture;
    const bool bInvoked = InvokeHandlerWithCapture(TEXT("asset.list"), Payload, Capture);
    TestTrue(TEXT("asset.list handler found"), bInvoked);
    TestTrue(TEXT("handler sent a response"), Capture.bWasCalled);
    TestTrue(TEXT("response reports success"), Capture.bSuccess);
    return true;
}

// ============================================================================
// AssetManageHandler — asset.list (top-level `path` respected when filter lacks path)
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetListTopLevelPathRespectedTest,
    "PinWright.asset.list.TopLevelPathRespected",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAssetListTopLevelPathRespectedTest::RunTest(const FString& Parameters)
{
    // Top-level `path` must be honored when the `filter` object is present but
    // provides neither `path` nor `pathStartsWith`. Prior behavior silently
    // dropped the top-level path and fell back to /Game.
    const TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("path"), TEXT("/Game/UI"));
    const TSharedPtr<FJsonObject> Filter = MakeShared<FJsonObject>();
    Filter->SetStringField(TEXT("class"), TEXT("/Script/UMGEditor.WidgetBlueprint"));
    Payload->SetObjectField(TEXT("filter"), Filter);

    FTestResponseCapture Capture;
    const bool bInvoked = InvokeHandlerWithCapture(TEXT("asset.list"), Payload, Capture);
    TestTrue(TEXT("asset.list handler found"), bInvoked);
    TestTrue(TEXT("handler sent a response"), Capture.bWasCalled);
    TestTrue(TEXT("response reports success"), Capture.bSuccess);

    // Capture.Result is the already-constructed response JSON object (see
    // FHandlerContext::SendSuccess). No string parse needed.
    if (!Capture.Result.IsValid())
    {
        AddError(TEXT("response result object is invalid"));
        return false;
    }

    const TArray<TSharedPtr<FJsonValue>>* Assets = nullptr;
    if (!Capture.Result->TryGetArrayField(TEXT("assets"), Assets) || Assets == nullptr)
    {
        AddError(TEXT("response has no assets array"));
        return false;
    }

    for (const TSharedPtr<FJsonValue>& V : *Assets)
    {
        if (!V.IsValid()) continue;
        const TSharedPtr<FJsonObject> AssetObj = V->AsObject();
        if (!AssetObj.IsValid()) continue;
        FString PackagePath;
        AssetObj->TryGetStringField(TEXT("packagePath"), PackagePath);
        TestTrue(*FString::Printf(TEXT("asset packagePath starts with /Game/UI: got '%s'"), *PackagePath),
            PackagePath.StartsWith(TEXT("/Game/UI")));
    }
    return true;
}

// ============================================================================
// AssetManageHandler — asset.list (per-row namesOnly/fields projection)
// ============================================================================

// Regression test for E-asset-list-no-projection-spills.
//
// asset.list built every row with a fixed, verbose shape — name+path+class+
// packagePath AND a per-asset tags array — with no lever to drop it, so the
// "list assets of class X so I can pick one" call spilled the inline budget on the
// tags weight even at a small pagination.limit. The fix adds the shared per-row
// projection (FHandlerContext::ReadFieldProjection, the same lever landed on
// actor.list): namesOnly=true returns just name+path, and a fields=[...] allow-list
// returns exactly the requested keys (case-insensitive).
//
// The fixture is built IN-CODE: a scratch UMaterial registered with the asset
// registry (FScratchAsset -> FAssetRegistryModule::AssetCreated) under
// /Game/PinWrightTests, then listed by that folder. No project/Lyra content is
// loaded, and a fixture the handler fails to return is a hard failure, not a skip.
//
// Counterfactual: revert the projection and namesOnly/fields are ignored, so the
// row still carries class/packagePath/tags — the "namesOnly row drops class/
// packagePath/tags" and "fields=[Name] row drops everything else" assertions fail.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetListFieldProjectionTest,
    "PinWright.asset.list.FieldProjection",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAssetListFieldProjectionTest::RunTest(const FString& Parameters)
{
    // Build a real registry-visible fixture so asset.list returns at least one row
    // we control. FScratchAsset registers via FAssetRegistryModule::AssetCreated and
    // cleans up on scope exit; its package folder is /Game/PinWrightTests.
    IrTest::FScratchAsset Scratch(TEXT("AssetListProj"));
    UMaterial* Material =
        IrTest::CreateFactoryScratchAsset<UMaterial, UMaterialFactoryNew>(Scratch);
    if (!TestNotNull(TEXT("scratch fixture material created"), Material))
    {
        return false;
    }
    const FString FixtureName = Scratch.AssetName;
    const FString FolderPath = FPackageName::GetLongPackagePath(Scratch.PackagePath);

    // Invoke asset.list scoped to the fixture folder with an extra projection knob,
    // then return the row object for our fixture asset (located by its unique name).
    auto ListAndFindFixtureRow =
        [&](TFunctionRef<void(const TSharedPtr<FJsonObject>&)> AddProjection) -> TSharedPtr<FJsonObject>
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("path"), FolderPath);
        Payload->SetBoolField(TEXT("recursive"), true);
        AddProjection(Payload);

        FTestResponseCapture Capture;
        const bool bInvoked = InvokeHandlerWithCapture(TEXT("asset.list"), Payload, Capture);
        TestTrue(TEXT("asset.list handler found"), bInvoked);
        TestTrue(TEXT("asset.list reports success"), Capture.bSuccess);
        if (!Capture.bSuccess || !Capture.Result.IsValid())
        {
            return nullptr;
        }
        return JsonArrayFindObjectByStringField(
            Capture.Result, TEXT("assets"), TEXT("name"), FixtureName);
    };

    // 1) Default (no projection): the full, unprojected shape — every key present.
    {
        const TSharedPtr<FJsonObject> Row =
            ListAndFindFixtureRow([](const TSharedPtr<FJsonObject>&) {});
        if (!TestTrue(TEXT("default list returns the fixture row"), Row.IsValid()))
        {
            return false;
        }
        TestTrue(TEXT("default row has name"), Row->HasField(TEXT("name")));
        TestTrue(TEXT("default row has path"), Row->HasField(TEXT("path")));
        TestTrue(TEXT("default row has class"), Row->HasField(TEXT("class")));
        TestTrue(TEXT("default row has packagePath"), Row->HasField(TEXT("packagePath")));
        TestTrue(TEXT("default row has tags"), Row->HasField(TEXT("tags")));
    }

    // 2) namesOnly=true: only name+path; class/packagePath/tags dropped. This is the
    //    core lever — reverting the fix keeps all five and fails these drops.
    {
        const TSharedPtr<FJsonObject> Row = ListAndFindFixtureRow(
            [](const TSharedPtr<FJsonObject>& P) { P->SetBoolField(TEXT("namesOnly"), true); });
        if (!TestTrue(TEXT("namesOnly list returns the fixture row"), Row.IsValid()))
        {
            return false;
        }
        TestTrue(TEXT("namesOnly row keeps name"), Row->HasField(TEXT("name")));
        TestTrue(TEXT("namesOnly row keeps path"), Row->HasField(TEXT("path")));
        TestFalse(TEXT("namesOnly row drops class"), Row->HasField(TEXT("class")));
        TestFalse(TEXT("namesOnly row drops packagePath"), Row->HasField(TEXT("packagePath")));
        TestFalse(TEXT("namesOnly row drops the verbose tags array"), Row->HasField(TEXT("tags")));
    }

    // 3) fields=["Name"] (mixed case on purpose): the allow-list returns exactly that
    //    key case-insensitively, dropping path too — proves the fields path, not just
    //    the namesOnly shorthand.
    {
        const TSharedPtr<FJsonObject> Row = ListAndFindFixtureRow(
            [](const TSharedPtr<FJsonObject>& P) {
                TArray<TSharedPtr<FJsonValue>> FieldArr;
                FieldArr.Add(MakeShared<FJsonValueString>(TEXT("Name")));
                P->SetArrayField(TEXT("fields"), FieldArr);
            });
        if (!TestTrue(TEXT("fields=[Name] list returns the fixture row"), Row.IsValid()))
        {
            return false;
        }
        TestTrue(TEXT("fields=[Name] row keeps name (case-insensitive)"), Row->HasField(TEXT("name")));
        TestFalse(TEXT("fields=[Name] row drops path"), Row->HasField(TEXT("path")));
        TestFalse(TEXT("fields=[Name] row drops class"), Row->HasField(TEXT("class")));
        TestFalse(TEXT("fields=[Name] row drops packagePath"), Row->HasField(TEXT("packagePath")));
        TestFalse(TEXT("fields=[Name] row drops tags"), Row->HasField(TEXT("tags")));
    }

    return true;
}
