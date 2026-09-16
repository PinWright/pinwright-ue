// Copyright (c) 2026 Alexander Penkin. MIT License.

// model.compile must refuse a save-bearing compile BEFORE it builds anything while a
// Play-In-Editor session is holding the editor.
//
// The defect this guards: the editor refuses every single-asset write while any PIE session is
// up, and model.compile met that refusal only at the save - after BuildStaticMesh had already
// rebuilt the live UStaticMesh in place and dirtied its package. The write was correctly reported
// as blocked, but the loaded asset was then AHEAD of its .uasset, in an editor several agents
// share, with no verb that reconciles the two: the next save-all from any stream persists a
// revision nobody reviewed, and an editor crash loses the rebuild while the on-disk asset is
// silently one revision behind its own committed source.
//
// The two assertions that matter are therefore paired: the response must name PIE in the shared
// save vocabulary, AND no asset may exist afterwards. Reverting the pre-flight leaves the first
// half nearly intact (the save path still reports blockedByPie) and breaks the second, which is
// exactly the state the board entry describes.
//
// A live PIE session is deliberately not started: doing so under -unattended walks dirty
// transient Blueprints left by sibling tests and can take the suite down. GIsPlayInEditorWorld is
// one half of the exact two-global engine predicate the guard mirrors
// (EditorScriptingHelpers::CheckIfInEditorAndPIE), so scoping it exercises the real production
// path without creating a world.

#include "Misc/AutomationTest.h"

#include "CoreGlobals.h"
#include "Dispatch/RpcDispatcher.h"
#include "Dom/JsonObject.h"
#include "Engine/StaticMesh.h"
#include "HAL/FileManager.h"
#include "Handlers/ErrorCodes.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Templates/UnrealTemplate.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

#include "Tests/Infra/DispatcherTestHelpers.h"
#include "Tests/TestUtils.h"

using DispatcherTestHelpers::MakeDispatcher;
using DispatcherTestHelpers::Dispatch;
using DispatcherTestHelpers::FSinkPtr;

namespace
{
// Prefixed: this module builds with bUseUnity = true, so a helper sharing a name with a sibling
// test TU collides once Unity merges them.

const TCHAR* ModelPieRefusalTest_Document =
    TEXT("pwmodel 0\n")
    TEXT("part body {\n")
    TEXT("    box size=(10, 10, 10)\n")
    TEXT("}\n");

FString ModelPieRefusalTest_WriteSource()
{
    const FString Directory =
        FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("PinWright"), TEXT("ModelPieRefusalTests"));
    IFileManager::Get().MakeDirectory(*Directory, /*Tree=*/true);

    const FString Path = FPaths::Combine(Directory,
        FString::Printf(TEXT("PwModelPieRefusal_%s.pwmodel"),
            *FGuid::NewGuid().ToString(EGuidFormats::Digits)));
    return FFileHelper::SaveStringToFile(ModelPieRefusalTest_Document, *Path) ? Path : FString();
}

FString ModelPieRefusalTest_UniqueAssetPath()
{
    return FString::Printf(TEXT("/Game/PwModelPieRefusalTests/PW_PieRefusal_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));
}

FString ModelPieRefusalTest_Str(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field)
{
    FString Value;
    return (Object.IsValid() && Object->TryGetStringField(Field, Value))
        ? Value
        : FString(TEXT("<absent>"));
}
}

// The regression guard for the ticket: a save-bearing compile during PIE must produce the typed
// refusal AND no geometry.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FModelCompileRefusesBeforeBuildDuringPieTest,
    "PinWright.Model.Handlers.CompileRefusesBeforeBuildingWhilePieHoldsTheEditor",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FModelCompileRefusesBeforeBuildDuringPieTest::RunTest(const FString& /*Parameters*/)
{
    const FString SourcePath = ModelPieRefusalTest_WriteSource();
    if (!TestFalse(TEXT("a temporary .pwmodel source was written"), SourcePath.IsEmpty()))
    {
        return true;
    }
    ON_SCOPE_EXIT { IFileManager::Get().Delete(*SourcePath, false, true, true); };

    const FString AssetPath = ModelPieRefusalTest_UniqueAssetPath();
    // Declared before the PIE guard so the flag is restored before any cleanup runs.
    ON_SCOPE_EXIT { CleanupTestAsset(AssetPath); };

    FRpcDispatcher Dispatcher;
    FSinkPtr Sink;
    MakeDispatcher(Sink, Dispatcher);

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("filePath"), SourcePath);
    Payload->SetStringField(TEXT("outputPath"), AssetPath);
    // 'save' left at its default of true: the refusal must follow from the requested write.

    bool bSuccess = false;
    TSharedPtr<FJsonObject> Result;
    FString ErrorCode;
    {
        TGuardValue<bool> PieWorldFlagGuard(GIsPlayInEditorWorld, true);
        Dispatch(Dispatcher, Sink, TEXT("model.compile"), TEXT("pie-refusal"),
            Payload, bSuccess, Result, ErrorCode);
    }

    TestFalse(TEXT("a save-bearing compile during PIE is refused, not reported as a success"),
        bSuccess);
    TestEqual(TEXT("the refusal carries the same typed code asset.save uses"),
        ErrorCode, FString(ErrorCodes::ERR_PIE_ACTIVE));

    if (!TestTrue(TEXT("the refusal carries a result payload"), Result.IsValid()))
    {
        return true;
    }

    // The save vocabulary, identical to the post-hoc path so a caller branches on one shape.
    TestEqual(TEXT("the refusal names the blocked save state"),
        ModelPieRefusalTest_Str(Result, TEXT("saveState")), FString(TEXT("blockedByPie")));
    TestFalse(TEXT("the refusal carries actionable saveDetail"),
        ModelPieRefusalTest_Str(Result, TEXT("saveDetail")).IsEmpty());
    TestEqual(TEXT("the refusal names the editor mode"),
        ModelPieRefusalTest_Str(Result, TEXT("editorMode")), FString(TEXT("PIE")));
    TestEqual(TEXT("the refusal names the asset that was not written"),
        ModelPieRefusalTest_Str(Result, TEXT("assetPath")), AssetPath);

    bool bPieActive = false;
    TestTrue(TEXT("the refusal reports pieActive"),
        Result->TryGetBoolField(TEXT("pieActive"), bPieActive) && bPieActive);
    bool bSaveRequested = false;
    TestTrue(TEXT("the refusal records that a save was requested"),
        Result->TryGetBoolField(TEXT("saveRequested"), bSaveRequested) && bSaveRequested);
    bool bSaved = true;
    TestTrue(TEXT("the refusal reports saved:false"),
        Result->TryGetBoolField(TEXT("saved"), bSaved) && !bSaved);
    bool bPendingFlush = true;
    TestTrue(TEXT("a PIE refusal is not queued flush work"),
        Result->TryGetBoolField(TEXT("pendingFlush"), bPendingFlush) && !bPendingFlush);

    // THE half the pre-flight exists for: the mesh must not have been built, so nothing in memory
    // is ahead of disk. Reverting the pre-flight leaves a fully built UStaticMesh here.
    TestNull(TEXT("no UStaticMesh was built for the refused compile"),
        FindObject<UStaticMesh>(nullptr, *ToObjectPath(AssetPath)));
    TestNull(TEXT("no package was created for the refused compile"),
        FindPackage(nullptr, *AssetPath));
    TestFalse(TEXT("no .uasset reached disk"), FPackageName::DoesPackageExist(AssetPath));
    return true;
}

// The other direction, and it is what keeps the guard from becoming "refuse every compile during
// PIE": save:false asks for no write, so there is nothing for PIE to block and the in-memory build
// is the caller's stated intent. Widening the refusal to cover it would take away the one way to
// keep authoring while somebody else's session runs.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FModelCompileWithoutSaveStillRunsDuringPieTest,
    "PinWright.Model.Handlers.CompileWithoutSaveIsNotRefusedDuringPie",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FModelCompileWithoutSaveStillRunsDuringPieTest::RunTest(const FString& /*Parameters*/)
{
    const FString SourcePath = ModelPieRefusalTest_WriteSource();
    if (!TestFalse(TEXT("a temporary .pwmodel source was written"), SourcePath.IsEmpty()))
    {
        return true;
    }
    ON_SCOPE_EXIT { IFileManager::Get().Delete(*SourcePath, false, true, true); };

    const FString AssetPath = ModelPieRefusalTest_UniqueAssetPath();
    ON_SCOPE_EXIT { CleanupTestAsset(AssetPath); };

    FRpcDispatcher Dispatcher;
    FSinkPtr Sink;
    MakeDispatcher(Sink, Dispatcher);

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("filePath"), SourcePath);
    Payload->SetStringField(TEXT("outputPath"), AssetPath);
    Payload->SetBoolField(TEXT("save"), false);

    bool bSuccess = false;
    TSharedPtr<FJsonObject> Result;
    FString ErrorCode;
    {
        TGuardValue<bool> PieWorldFlagGuard(GIsPlayInEditorWorld, true);
        Dispatch(Dispatcher, Sink, TEXT("model.compile"), TEXT("pie-no-save"),
            Payload, bSuccess, Result, ErrorCode);
    }

    TestTrue(*FString::Printf(TEXT("a save:false compile is not refused during PIE (code '%s')"),
        *ErrorCode), bSuccess);
    TestNotEqual(TEXT("a save:false compile does not raise the PIE refusal"),
        ErrorCode, FString(ErrorCodes::ERR_PIE_ACTIVE));
    TestNotNull(TEXT("a save:false compile still builds the mesh in memory"),
        FindObject<UStaticMesh>(nullptr, *ToObjectPath(AssetPath)));
    return true;
}
