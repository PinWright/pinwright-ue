// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression test for the chooser.* persistence lie.
//
// All six chooser.* verbs take a `save` param and finish through the file-local
// FinishMutation chokepoint. That chokepoint called the mark-dirty-only helper
// McpSafeAssetSave, which returns without writing any package, so nothing chooser.*
// ever did reached disk. Three separate fields hid it and corroborated each other:
// FinishMutation reported nothing, AddAssetVerification asserted existsAfter as a
// literal true, and chooser.create's `saved` was Ctx.GetBool("save") — the caller's own
// request flag read a second time, downstream of no save at all. So
// create -> add_column -> add_row -> set_cell -> compile all reported success with real
// columnCount / rowCount numbers, in-session readbacks confirmed it because they read the
// asset registry that McpSafeAssetSave had populated, and the entire table was gone after
// an editor restart.
//
// The accepted fix for the identical defect one namespace over is
// PoseSearchHandler.cpp:156-167 (B-pose-search-create-save-no-disk-write); this file
// mirrors its test, TestPoseSearchCreateSaveWritesToDisk.cpp.
//
// The load-bearing assertion is the IFileManager::FileSize probe, not the `saved` field:
// pre-fix `saved` merely echoed the request and so could not tell a real save from a
// dirty-only no-op. Pre-fix FileSize < 0 for save:true and this test is red; post-fix the
// .uasset lands and it is green. The save:false half is the other direction — it asserts
// the verb reports honestly that it did NOT persist, which a constant-true cannot do.

#include "Misc/AutomationTest.h"

#include "Chooser.h"
#include "Compat/EngineVersionCompat.h"
#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Tests/TestUtils.h"

namespace
{
// Distinct name from MakeChooserPackagePath in the sibling TestChooserAuthoringHandlers.cpp
// so a Unity merge of the two Assets TUs does not collide on an ODR duplicate.
FString MakeChooserSavePackagePath()
{
    return FString::Printf(
        TEXT("/Game/PinWrightTests/CH_SaveToDisk_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));
}

// Resolves the .uasset filename from the response's assetPath and returns its on-disk
// size, or -1 when no file exists.
int64 OnDiskSizeFromResponse(FAutomationTestBase& Test, const TSharedPtr<FJsonObject>& Result)
{
    FString AssetPath;
    Result->TryGetStringField(TEXT("assetPath"), AssetPath);
    const FString PackageFilename = PackageFilenameFromAssetPath(AssetPath);
    Test.TestFalse(TEXT("resolved a package filename from assetPath"), PackageFilename.IsEmpty());
    if (PackageFilename.IsEmpty())
    {
        return -1;
    }
    return IFileManager::Get().FileSize(*PackageFilename);
}
} // namespace

// chooser.create with save:true must land the UChooserTable .uasset on disk and report
// saved:true. Pre-fix the mark-dirty-only FinishMutation wrote nothing, so FileSize < 0.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FChooserCreateSaveWritesToDiskTest,
    "PinWright.chooser.CreateSaveWritesToDisk",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FChooserCreateSaveWritesToDiskTest::RunTest(const FString& Parameters)
{
    const FString PackagePath = MakeChooserSavePackagePath();
    CleanupTestAsset(PackagePath);

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("path"), PackagePath);
    Payload->SetBoolField(TEXT("save"), true);

    FTestResponseCapture Capture;
    const bool bFound = InvokeHandlerWithCapture(TEXT("chooser.create"), Payload, Capture);
    TestTrue(TEXT("chooser.create handler is registered"), bFound);
    TestTrue(TEXT("chooser.create responded"), Capture.bWasCalled);
    TestTrue(TEXT("chooser.create succeeded"), Capture.bSuccess);
    if (!bFound || !Capture.bSuccess || !Capture.Result.IsValid())
    {
        AddError(FString::Printf(TEXT("chooser.create did not succeed: %s %s"),
            *Capture.ErrorCode, *Capture.Message));
        CleanupTestAsset(PackagePath);
        return false;
    }

    // The genuine differential: save:true must produce a .uasset on disk.
    const int64 OnDiskSize = OnDiskSizeFromResponse(*this, Capture.Result);
    TestTrue(
        *FString::Printf(TEXT("chooser.create save:true persists the .uasset to disk (size=%lld)"),
            OnDiskSize),
        OnDiskSize > 0);

    // The reporting side must now agree with the disk rather than echo the request.
    bool bSavedReported = false;
    TestTrue(TEXT("chooser.create reports a saved field"),
        Capture.Result->TryGetBoolField(TEXT("saved"), bSavedReported));
    TestTrue(TEXT("chooser.create save:true reports saved:true"), bSavedReported);

    bool bSaveRequested = false;
    TestTrue(TEXT("chooser.create reports saveRequested"),
        Capture.Result->TryGetBoolField(TEXT("saveRequested"), bSaveRequested));
    TestTrue(TEXT("chooser.create echoes saveRequested:true"), bSaveRequested);

    // A persisted asset must not also be flagged as awaiting a flush.
    TestFalse(TEXT("chooser.create save:true sets no pendingFlush"),
        Capture.Result->HasField(TEXT("pendingFlush")));

    bool bExistsOnDisk = false;
    TestTrue(TEXT("chooser.create reports existsOnDisk"),
        Capture.Result->TryGetBoolField(TEXT("existsOnDisk"), bExistsOnDisk));
    TestTrue(TEXT("chooser.create save:true reports existsOnDisk:true"), bExistsOnDisk);

    CleanupTestAsset(PackagePath);
    return true;
}

// The other direction, and the one a constant cannot express: save:false must report that
// nothing was persisted. Pre-fix chooser.create answered saved:<the request flag> and
// existsAfter:true regardless, so a save:false create looked identical in the response to
// a save:true one apart from the echoed bool.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FChooserCreateNoSaveReportsNotPersistedTest,
    "PinWright.chooser.CreateNoSaveReportsNotPersisted",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FChooserCreateNoSaveReportsNotPersistedTest::RunTest(const FString& Parameters)
{
    const FString PackagePath = MakeChooserSavePackagePath();
    CleanupTestAsset(PackagePath);

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("path"), PackagePath);
    Payload->SetBoolField(TEXT("save"), false);

    FTestResponseCapture Capture;
    const bool bFound = InvokeHandlerWithCapture(TEXT("chooser.create"), Payload, Capture);
    TestTrue(TEXT("chooser.create handler is registered"), bFound);
    TestTrue(TEXT("chooser.create succeeded"), Capture.bSuccess);
    if (!bFound || !Capture.bSuccess || !Capture.Result.IsValid())
    {
        CleanupTestAsset(PackagePath);
        return false;
    }

    const int64 OnDiskSize = OnDiskSizeFromResponse(*this, Capture.Result);
    TestTrue(
        *FString::Printf(TEXT("chooser.create save:false writes no .uasset (size=%lld)"), OnDiskSize),
        OnDiskSize < 0);

    bool bSavedReported = true;
    TestTrue(TEXT("chooser.create reports a saved field"),
        Capture.Result->TryGetBoolField(TEXT("saved"), bSavedReported));
    TestFalse(TEXT("chooser.create save:false reports saved:false"), bSavedReported);

    // existsAfter was a hardcoded true. The in-memory chooser IS resolvable, so
    // existsAfter may legitimately be true here — existsOnDisk is the field that must
    // separate resolvable from durable, and it has to be false.
    bool bExistsOnDisk = true;
    TestTrue(TEXT("chooser.create reports existsOnDisk"),
        Capture.Result->TryGetBoolField(TEXT("existsOnDisk"), bExistsOnDisk));
    TestFalse(TEXT("chooser.create save:false reports existsOnDisk:false"), bExistsOnDisk);

    CleanupTestAsset(PackagePath);
    return true;
}

// The mutate path, which is where the table's contents (not just its existence) were
// lost. A chooser created without saving, then mutated with save:true, must end up on
// disk WITH the mutation. add_row is the cheapest verb through the shared FinishMutation
// chokepoint that add_column / set_cell / compile / set_context_type also use.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FChooserAddRowSaveWritesToDiskTest,
    "PinWright.chooser.AddRowSaveWritesToDisk",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FChooserAddRowSaveWritesToDiskTest::RunTest(const FString& Parameters)
{
    const FString PackagePath = MakeChooserSavePackagePath();
    CleanupTestAsset(PackagePath);

    TSharedPtr<FJsonObject> CreatePayload = MakeShared<FJsonObject>();
    CreatePayload->SetStringField(TEXT("path"), PackagePath);
    CreatePayload->SetBoolField(TEXT("save"), false);

    FTestResponseCapture CreateCapture;
    if (!InvokeHandlerWithCapture(TEXT("chooser.create"), CreatePayload, CreateCapture)
        || !CreateCapture.bSuccess)
    {
        AddError(FString::Printf(TEXT("chooser.create did not succeed: %s %s"),
            *CreateCapture.ErrorCode, *CreateCapture.Message));
        CleanupTestAsset(PackagePath);
        return false;
    }

    TSharedPtr<FJsonObject> RowPayload = MakeShared<FJsonObject>();
    RowPayload->SetStringField(TEXT("path"), PackagePath);
    RowPayload->SetBoolField(TEXT("save"), true);

    FTestResponseCapture RowCapture;
    const bool bFound = InvokeHandlerWithCapture(TEXT("chooser.add_row"), RowPayload, RowCapture);
    TestTrue(TEXT("chooser.add_row handler is registered"), bFound);
    TestTrue(TEXT("chooser.add_row succeeded"), RowCapture.bSuccess);
    if (!bFound || !RowCapture.bSuccess || !RowCapture.Result.IsValid())
    {
        AddError(FString::Printf(TEXT("chooser.add_row did not succeed: %s %s"),
            *RowCapture.ErrorCode, *RowCapture.Message));
        CleanupTestAsset(PackagePath);
        return false;
    }

    const int64 OnDiskSize = OnDiskSizeFromResponse(*this, RowCapture.Result);
    TestTrue(
        *FString::Printf(TEXT("chooser.add_row save:true persists the .uasset to disk (size=%lld)"),
            OnDiskSize),
        OnDiskSize > 0);

    bool bSavedReported = false;
    TestTrue(TEXT("chooser.add_row reports a saved field"),
        RowCapture.Result->TryGetBoolField(TEXT("saved"), bSavedReported));
    TestTrue(TEXT("chooser.add_row save:true reports saved:true"), bSavedReported);

    // And the mutation itself must be what landed, not an empty table.
    double RowCount = 0.0;
    TestTrue(TEXT("chooser.add_row reports rowCount"),
        RowCapture.Result->TryGetNumberField(TEXT("rowCount"), RowCount));
    TestTrue(TEXT("the saved table carries the appended row"), RowCount >= 1.0);

    CleanupTestAsset(PackagePath);
    return true;
}
