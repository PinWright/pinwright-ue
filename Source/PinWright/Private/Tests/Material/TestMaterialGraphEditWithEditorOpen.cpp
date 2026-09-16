// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression test for B-material-graph-edit-clobbered-by-open-editor.
//
// material.authoring graph mutators load the asset via plain LoadObject and edit the
// UMaterial directly. While FMaterialEditor has the asset open it owns an authoritative
// working-copy graph that clobbers those external edits on its next sync/save, so the
// handlers previously reported success while the on-disk graph stayed unchanged. The fix
// rejects the edit with error code EDITOR_OPEN while an editor is open.
//
// This test creates and saves a material, opens it via editor.open_asset, asserts
// add_custom_expression is rejected with EDITOR_OPEN, then closes via editor.close_asset
// and asserts the same call now succeeds and actually appends an expression.
//
// This plugin's automation runs in a real editor with a real RHI (never -NullRHI), so
// OpenEditorForAsset registers the Material Editor and the EDITOR_OPEN rejection
// assertion runs unconditionally; a failure to open is itself a hard test failure
// rather than a silently skipped assertion.
//
// Counterfactual: if the open-editor check in MaterialAuthoringHandler.cpp is reverted,
// add_custom_expression returns success (a nodeId) while the editor is open instead of
// EDITOR_OPEN, so the EDITOR_OPEN assertion fails.

#include "Misc/AutomationTest.h"
#include "Misc/EngineVersionComparison.h"
#include "Dom/JsonObject.h"
#include "Tests/TestUtils.h"
#include "Editor.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Handlers/Material/MaterialFinders.h"
#include "Tests/Material/MaterialEditorOpenTestHelpers.h"
#include "Tests/TestSkipReporting.h"
#include "Materials/Material.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

namespace
{
    // add_custom_expression requires assetPath, code, inputs, x and y.
    TSharedPtr<FJsonObject> MakeAddCustomExpressionPayload(const FString& AssetPath)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("assetPath"), AssetPath);
        Payload->SetStringField(TEXT("code"), TEXT("return 0.0;"));
        Payload->SetArrayField(TEXT("inputs"), TArray<TSharedPtr<FJsonValue>>());
        Payload->SetNumberField(TEXT("x"), 100.0);
        Payload->SetNumberField(TEXT("y"), 100.0);
        return Payload;
    }

    using MaterialEditorOpenTestHelpers::ExpressionCount;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaterialGraphEditWithEditorOpenTest,
    "PinWright.material.authoring.edit-with-editor-open",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMaterialGraphEditWithEditorOpenTest::RunTest(const FString& Parameters)
{
#if UE_VERSION_OLDER_THAN(5, 5, 0)
    // On UE 5.4, opening the full Material Editor via editor.open_asset in a headless
    // (-RenderOffscreen) run spawns async shader/blueprint compilation on a background
    // worker thread that crashes the editor: "Runnable thread Background Worker crashed"
    // → ensure(SkeletonCompiledBlueprints.Num() == 1) (BlueprintCompilationManager.cpp:361)
    // → EXCEPTION_ACCESS_VIOLATION. The crash is entirely engine-side async work with no
    // plugin frame, and the test's premise requires the editor to actually open. 5.5+ is
    // stable. Skip the editor-open scenario on 5.4.
    PinWrightTestSkip::SkipAssertions(*this, TEXT("engine-version-unsupported"),
        TEXT("Skipped on UE 5.4: opening the Material Editor headlessly crashes a background compile worker."));
    return true;
#else
    const FString AssetName = FString::Printf(
        TEXT("EditWhileOpen_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    const FString PackagePath = FString::Printf(TEXT("/Game/__PW_GatewayTests/%s"), *AssetName);
    const FString ObjectPath = FString::Printf(TEXT("%s.%s"), *PackagePath, *AssetName);

    UPackage* Pkg = CreatePackage(*PackagePath);
    if (!TestNotNull(TEXT("Package created"), Pkg))
        return true;

    UMaterial* Material = NewObject<UMaterial>(
        Pkg, FName(*AssetName), RF_Public | RF_Standalone);
    if (!TestNotNull(TEXT("Material created"), Material))
    {
        CleanupTestAsset(PackagePath);
        return true;
    }
    Material->PostEditChange();
    FAssetRegistryModule::AssetCreated(Material);

    // editor.open_asset checks DoesAssetExist, so the package must be on disk.
    const FString PackageFile = FPackageName::LongPackageNameToFilename(
        PackagePath, FPackageName::GetAssetPackageExtension());
    Pkg->MarkPackageDirty();
    FSavePackageArgs SaveArgs;
    SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
    SaveArgs.SaveFlags = SAVE_NoError;
    if (!TestTrue(TEXT("Material saved to disk"),
            UPackage::SavePackage(Pkg, Material, *PackageFile, SaveArgs)))
    {
        CleanupTestAsset(PackagePath);
        return true;
    }

    const int32 BaselineCount = ExpressionCount(Material);

    // Open the editor through the production handler, as a real caller would.
    TSharedPtr<FJsonObject> OpenPayload = MakeShared<FJsonObject>();
    OpenPayload->SetStringField(TEXT("assetPath"), ObjectPath);
    FTestResponseCapture OpenCapture;
    TestTrue(TEXT("editor.open_asset handler found"),
        InvokeHandlerWithCapture(TEXT("editor.open_asset"), OpenPayload, OpenCapture));

    // The EDITOR_OPEN rejection is the spec's required assertion, so it must run
    // unconditionally. These tests run in a real editor with a real RHI (never -NullRHI),
    // where OpenEditorForAsset spawns and registers the Material Editor; assert that the
    // open actually took effect rather than silently degrading coverage if it did not.
    if (TestTrue(TEXT("Material Editor is registered open after editor.open_asset"),
            PinWright::Material::IsMaterialEditorOpen(Material)))
    {
        FTestResponseCapture RejectCapture;
        TestTrue(TEXT("add_custom_expression handler found (editor open)"),
            InvokeHandlerWithCapture(
                TEXT("material.authoring.add_custom_expression"),
                MakeAddCustomExpressionPayload(ObjectPath), RejectCapture));
        TestFalse(TEXT("rejected: response is error, not success"), RejectCapture.bSuccess);
        TestEqual(TEXT("error code is EDITOR_OPEN"),
            RejectCapture.ErrorCode, FString(TEXT("EDITOR_OPEN")));
        TestEqual(TEXT("no expression appended while editor open"),
            ExpressionCount(Material), BaselineCount);
    }

    // Close the editor through the production handler.
    TSharedPtr<FJsonObject> ClosePayload = MakeShared<FJsonObject>();
    ClosePayload->SetStringField(TEXT("assetPath"), ObjectPath);
    FTestResponseCapture CloseCapture;
    TestTrue(TEXT("editor.close_asset handler found"),
        InvokeHandlerWithCapture(TEXT("editor.close_asset"), ClosePayload, CloseCapture));
    TestFalse(TEXT("editor reports closed"),
        PinWright::Material::IsMaterialEditorOpen(Material));

    // With no editor open, the identical edit must succeed and actually append a node.
    FTestResponseCapture SuccessCapture;
    TestTrue(TEXT("add_custom_expression handler found (editor closed)"),
        InvokeHandlerWithCapture(
            TEXT("material.authoring.add_custom_expression"),
            MakeAddCustomExpressionPayload(ObjectPath), SuccessCapture));
    if (TestTrue(TEXT("editor-closed edit succeeds"), SuccessCapture.bSuccess) &&
        TestTrue(TEXT("success response has a result"), SuccessCapture.Result.IsValid()))
    {
        FString NodeId;
        TestTrue(TEXT("response carries a nodeId"),
            SuccessCapture.Result->TryGetStringField(TEXT("nodeId"), NodeId));
        TestFalse(TEXT("nodeId is non-empty"), NodeId.IsEmpty());
    }
    TestEqual(TEXT("expression count increased by one"),
        ExpressionCount(Material), BaselineCount + 1);

    CleanupTestAsset(PackagePath);
    return true;
#endif
}
