// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression test for B-geometry-convert-static-mesh-no-disk-write.
//
// geometry.convert_to_static_mesh — the terminal "bake the DynamicMesh into a
// real /Game/... StaticMesh asset" verb — used to call
// CreateNewStaticMeshAssetFromMesh (which only does CreatePackage +
// MarkPackageDirty + FAssetRegistryModule::AssetCreated) and then immediately
// build a success payload with exists:true / created:true. No save call ran, so
// the baked StaticMesh was dirty-in-memory only: a cold-load open returned
// ASSET_NOT_FOUND and no .uasset ever reached disk, even though the response
// asserted created:true. (Two cold-load reproductions confirmed this on the board.)
//
// The fix routes the baked package through the shared real-save wrapper
// SaveAssetToDiskReportingPresence (forced SaveLoadedAsset + IFileManager::FileSize
// disk probe gated by ShouldTreatAssetSaveAsSuccess), gates exists/created on the
// .uasset actually landing on disk, and emits saved/pendingFlush via AddAssetSaveReport.
// (MeshOpsHandler.cpp geometry.convert_to_static_mesh success branch.)
//
// Strategy: spawn a real DynamicMeshActor via geometry.create_box, bake it through
// the real dispatcher, then probe IFileManager::FileSize on the resolved package
// filename to assert the .uasset is genuinely on disk — the exact persistence the
// cold-load repro proved was missing. Counterfactual: reverting the fix (dropping
// the SaveAssetToDiskReportingPresence call) leaves the package dirty-only, so no
// file lands on disk and the FileSize >= 0 assertion fails.
#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Dispatch/RpcDispatcher.h"
#include "Tests/Infra/DispatcherTestHelpers.h"
#include "Tests/Geometry/GeometryTestHelpers.h"
#include "Tests/TestUtils.h"
#include "Tests/TestSkipReporting.h"

#include "Editor.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "HAL/FileManager.h"
#include "Misc/PackageName.h"

using DispatcherTestHelpers::MakeDispatcher;
using DispatcherTestHelpers::Dispatch;
using GeometryTestHelpers::DestroyActorsWithLabel;

// geometry.convert_to_static_mesh must persist the baked StaticMesh to disk for
// real, not leave it dirty-in-memory only. The .uasset has to exist on disk after
// the convert returns — otherwise the model is silently lost on the next launch.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryConvertToStaticMeshSavesToDiskTest,
    "PinWright.geometry.convert_to_static_mesh.SavesToDisk",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGeometryConvertToStaticMeshSavesToDiskTest::RunTest(const FString& Parameters)
{
    if (!GEditor || !IsValid(GEditor->GetEditorWorldContext().World()))
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("No editor world available; skipping convert_to_static_mesh disk-write test"));
        return true;
    }

    const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString Label = FString::Printf(TEXT("PW_ConvertDiskProbe_%s"), *Suffix);
    const FString AssetPath = FString::Printf(TEXT("/Game/GeneratedMeshes/%s"), *Label);

    DispatcherTestHelpers::FSinkPtr Sink;
    FRpcDispatcher Dispatcher;
    MakeDispatcher(Sink, Dispatcher);

    // 1. Spawn a real DynamicMeshActor whose label is the actorName convert resolves.
    {
        TSharedPtr<FJsonObject> CreateParams = MakeShared<FJsonObject>();
        CreateParams->SetStringField(TEXT("name"), Label);
        bool bCreated = false;
        FString CreateErr;
        Dispatch(Dispatcher, Sink, TEXT("geometry.create_box"),
            TEXT("req-convert-disk-create"), CreateParams, bCreated, CreateErr);
        if (!TestTrue(TEXT("geometry.create_box spawned the probe DynamicMeshActor"), bCreated))
        {
            DestroyActorsWithLabel(Label);
            return true;
        }
    }

    // 2. Bake it and capture the success payload.
    TSharedPtr<FJsonObject> ConvertParams = MakeShared<FJsonObject>();
    ConvertParams->SetStringField(TEXT("actorName"), Label);
    ConvertParams->SetStringField(TEXT("assetPath"), AssetPath);

    bool bSuccess = false;
    FString ErrorCode;
    TSharedPtr<FJsonObject> Result;
    Dispatch(Dispatcher, Sink, TEXT("geometry.convert_to_static_mesh"),
        TEXT("req-convert-disk"), ConvertParams, bSuccess, Result, ErrorCode);

    if (!TestTrue(TEXT("geometry.convert_to_static_mesh baked the StaticMesh"), bSuccess) ||
        !TestTrue(TEXT("convert success carries a result object"), Result.IsValid()))
    {
        CleanupTestAsset(AssetPath);
        DestroyActorsWithLabel(Label);
        return true;
    }

    // 3. THE HEART OF THE FIX: the .uasset must actually be on disk after convert.
    //    Resolve the package the handler echoed (sanitized assetPath) to its on-disk
    //    filename and probe its size. Reverting the fix (no SaveAssetToDiskReportingPresence)
    //    leaves the package dirty-in-memory only, so FileSize returns -1 and this fails —
    //    the same loss the cold-load repro observed as ASSET_NOT_FOUND.
    FString EchoedAssetPath;
    Result->TryGetStringField(TEXT("assetPath"), EchoedAssetPath);
    if (EchoedAssetPath.IsEmpty())
    {
        EchoedAssetPath = AssetPath;
    }
    const FString PackageFilename = PackageFilenameFromAssetPath(EchoedAssetPath);
    const int64 SizeOnDisk = IFileManager::Get().FileSize(*PackageFilename);
    TestTrue(TEXT("the baked .uasset is genuinely present on disk after convert"),
        SizeOnDisk >= 0);

    // 4. The honest persistence verdict the fix added: saved:true and a positive
    //    sizeBytes, with NO pendingFlush (the package is durable, not dirty-only).
    bool bSaved = false;
    TestTrue(TEXT("convert reports saved:true once the .uasset is on disk"),
        Result->TryGetBoolField(TEXT("saved"), bSaved) && bSaved);

    double SizeBytes = 0.0;
    TestTrue(TEXT("convert echoes a sizeBytes field"),
        Result->TryGetNumberField(TEXT("sizeBytes"), SizeBytes));
    TestTrue(TEXT("the on-disk .uasset has a non-zero size"), SizeBytes > 0.0);

    bool bPendingFlush = false;
    Result->TryGetBoolField(TEXT("pendingFlush"), bPendingFlush);
    TestFalse(TEXT("no pendingFlush when the asset is durably on disk"), bPendingFlush);

    // 5. exists/created now track real on-disk presence — true here because it persisted.
    bool bExists = false;
    TestTrue(TEXT("exists:true reflects the .uasset on disk"),
        Result->TryGetBoolField(TEXT("exists"), bExists) && bExists);
    bool bCreated = false;
    TestTrue(TEXT("created:true reflects the .uasset on disk"),
        Result->TryGetBoolField(TEXT("created"), bCreated) && bCreated);

    // Cleanup: drop the baked asset (registry + disk) and the probe actor.
    CleanupTestAsset(AssetPath);
    DestroyActorsWithLabel(Label);
    return true;
}
