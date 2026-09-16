// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression test for B-audio-create-save-no-disk-write.
//
// The audio.authoring create handlers (create_sound_class / create_sound_mix /
// create_reverb_effect and the rest of the namespace) take a save param that
// defaults to true and return existsAfter:true, implying the new .uasset is on
// disk. Before this fix the file-local SaveAudioAsset helper was a mark-dirty
// no-op (MarkPackageDirty + FAssetRegistryModule::AssetCreated, no package-save
// API), so save:true wrote nothing — the asset lived only in memory + the
// registry and vanished on a cold editor restart / git reset, while every call
// reported success. The fix routes SaveAudioAsset through the real-save helper
// SaveAssetToDiskReportingPresence (forced SaveLoadedAsset + IFileManager::FileSize
// disk probe, gated by ShouldTreatAssetSaveAsSuccess) — the same helper the
// niagara.create_* / metasound create fixes adopted — and the create handlers now
// emit an honest saved (gated on disk presence) / pendingFlush signal.
//
// These tests drive the real registered audio.authoring.create_sound_class handler
// through the dispatcher via the shared TestCreateHandlerSaveWritesToDisk contract
// runner and assert that, with save:true, the .uasset genuinely lands on disk and the
// response reports saved:true; with save:false, nothing is written and saved is false.
// If SaveAudioAsset is reverted to the mark-dirty-only body, the save:true disk-presence
// assertion (and the saved:true assertion) fail.

#include "Misc/AutomationTest.h"
#include "Sound/SoundClass.h"

#include "Tests/TestUtils.h"

// save:true must persist the new SoundClass to disk and report saved:true.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAudioCreateSoundClassSaveWritesToDiskTest,
    "PinWright.Assets.Audio.CreateSoundClass.SaveWritesToDisk",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAudioCreateSoundClassSaveWritesToDiskTest::RunTest(const FString& Parameters)
{
    return TestCreateHandlerSaveWritesToDisk(*this,
        TEXT("audio.authoring.create_sound_class"), USoundClass::StaticClass(),
        TEXT("SC_AudioSave"), TEXT("/Game/PinWrightTests/Audio"),
        /*bSave=*/true, /*bExpectDiskFile=*/true);
}

// save:false must NOT write the .uasset and must report saved:false — the field is
// honest about durable state in both directions.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAudioCreateSoundClassNoSaveLeavesNoDiskFileTest,
    "PinWright.Assets.Audio.CreateSoundClass.NoSaveLeavesNoDiskFile",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAudioCreateSoundClassNoSaveLeavesNoDiskFileTest::RunTest(const FString& Parameters)
{
    return TestCreateHandlerSaveWritesToDisk(*this,
        TEXT("audio.authoring.create_sound_class"), USoundClass::StaticClass(),
        TEXT("SC_AudioSave"), TEXT("/Game/PinWrightTests/Audio"),
        /*bSave=*/false, /*bExpectDiskFile=*/false);
}
