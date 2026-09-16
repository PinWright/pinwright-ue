// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression test for B-sequencer-create-save-no-disk-write.
//
// sequencer.create builds a new ULevelSequence via LevelSequenceFactoryNew and, on
// success, returns an existsAfter:true / assetClass:LevelSequence verification block
// (AddAssetVerification), so a caller reasonably believes the new .uasset is on disk.
// It is not. The handler routes the save through the shared mark-dirty no-op helper
// McpSafeAssetSave (SequenceHandler.cpp:335), which only MarkPackageDirty() +
// FAssetRegistryModule::AssetCreated() and never calls any package-save API — so the
// sequence lives only in memory + the registry and vanishes on a cold editor restart
// / git reset, while the call reports success. existsAfter:true is sourced from the
// asset registry, NOT from disk, which masks the loss.
//
// This test drives the REAL registered sequencer.create handler through the dispatcher
// (production handler code, not a copy) with documented-valid params and asserts the
// honest persistence property the ticket demands: after a successful create, the
// .uasset is genuinely on disk (IFileManager::FileSize > 0). Pre-fix the mark-dirty-only
// save writes nothing, so FileSize < 0 and the disk-presence assertion FAILS (red).
// The accepted sibling fixes (B-niagara-save-no-disk-write, B-metasound-create-save-
// no-disk-write, B-create-level-saved-true-no-umap, B-audio/-material create-save)
// reroute their own create path through the real-save helper
// SaveAssetToDiskReportingPresence; applying the same reroute here lands the .uasset
// on disk and flips this green.

#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "EditorAssetLibrary.h"
#include "HAL/FileManager.h"
#include "LevelSequence.h"

#include "Tests/TestUtils.h"

// sequencer.create must persist the new LevelSequence to disk — a successful create
// that reports existsAfter:true has to be backed by a real .uasset on disk.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSequencerCreateSaveWritesToDiskTest,
    "PinWright.sequencer.create.SaveWritesToDisk",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSequencerCreateSaveWritesToDiskTest::RunTest(const FString& Parameters)
{
    TestTrue(TEXT("sequencer.create handler registered"),
        IsHandlerRegistered(TEXT("sequencer.create")));

    // Unique name under a throwaway folder so the asset cannot pre-exist (the
    // pre-exist branch is a different, verify-only response path).
    const FString SeqName = FString::Printf(TEXT("SEQ_CreateSave_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    const FString DestFolder = TEXT("/Game/PinWrightTests/Sequencer");
    const FString FullPath = FString::Printf(TEXT("%s/%s"), *DestFolder, *SeqName);

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("name"), SeqName);
    Payload->SetStringField(TEXT("path"), DestFolder);

    FTestResponseCapture Capture;
    const bool bFound = InvokeHandlerWithCapture(TEXT("sequencer.create"), Payload, Capture);
    TestTrue(TEXT("sequencer.create handler invoked"), bFound);
    TestTrue(TEXT("sequencer.create responded"), Capture.bWasCalled);
    TestTrue(TEXT("sequencer.create succeeded"), Capture.bSuccess);
    if (!Capture.bSuccess || !Capture.Result.IsValid())
    {
        // A create that cannot even succeed can't demonstrate the persistence
        // property — surface it as a failure (the factory is available in a real
        // editor run) rather than masking it.
        return false;
    }

    // The handler reports existsAfter:true from the asset registry — this is the
    // false-success signal the ticket flags. Document that it is present so the
    // disk-presence miss below is unambiguously the reported-vs-durable gap.
    bool bExistsAfter = false;
    Capture.Result->TryGetBoolField(TEXT("existsAfter"), bExistsAfter);
    TestTrue(TEXT("sequencer.create reports existsAfter:true"), bExistsAfter);

    // Honest persistence verdict: the fix's new wire field must report saved:true when
    // the create genuinely lands the .uasset on disk. Default to false so an absent
    // field reads as "not saved" — a regression that still writes the file but drops or
    // false-reports this signal (saved:false / field missing) would otherwise stay green.
    bool bSavedReported = false;
    Capture.Result->TryGetBoolField(TEXT("saved"), bSavedReported);
    TestTrue(TEXT("sequencer.create reports saved:true"), bSavedReported);

    // Hard disk-presence proof: the new .uasset must actually be on disk. Pre-fix the
    // mark-dirty-only McpSafeAssetSave writes nothing, so FileSize < 0 here and this
    // assertion fails — the reproduction of B-sequencer-create-save-no-disk-write.
    FString AssetPath;
    Capture.Result->TryGetStringField(TEXT("assetPath"), AssetPath);
    const FString PackageFilename = PackageFilenameFromAssetPath(AssetPath);
    TestFalse(TEXT("resolved a package filename from assetPath"), PackageFilename.IsEmpty());
    const int64 OnDiskSize = IFileManager::Get().FileSize(*PackageFilename);
    TestTrue(TEXT("the .uasset is on disk after sequencer.create"), OnDiskSize > 0);

    // Teardown: on the green (post-fix) path the file is on disk and CleanupTestAsset
    // deletes it; on the red (pre-fix) path nothing is on disk, so also de-dirty the
    // leftover in-memory package so the disposable host isn't left carrying it.
    CleanupTestAsset(FullPath);
    if (UObject* Created =
            StaticFindObject(ULevelSequence::StaticClass(), nullptr, *ToObjectPath(FullPath)))
    {
        if (UPackage* Pkg = Created->GetOutermost())
        {
            Pkg->SetDirtyFlag(false);
        }
    }
    return true;
}
