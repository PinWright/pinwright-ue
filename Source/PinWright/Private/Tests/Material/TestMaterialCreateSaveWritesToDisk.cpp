// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression test for B-material-authoring-save-no-disk-write.
//
// material.authoring create_material / compile_material / create_material_instance
// take a save param that defaults to true and return existsAfter:true (and, for
// compile_material, saved:true), implying the new .uasset is on disk. Before this
// fix the file-local SaveMaterialAsset / SaveMaterialFunctionAsset /
// SaveMaterialInstanceAsset helpers were mark-dirty no-ops (MarkPackageDirty only,
// no package-save API), so save:true wrote nothing — the asset lived only in memory
// + the registry and vanished on a cold editor restart / git reset, while every
// call reported success. The fix routes the save trio through the real-save helper
// SaveAssetToDiskReportingPresence (forced SaveLoadedAsset + IFileManager::FileSize
// disk probe, gated by ShouldTreatAssetSaveAsSuccess) — the same helper the audio /
// niagara / metasound create fixes adopted — and the create/compile handlers now
// emit an honest saved (gated on disk presence) / pendingFlush signal via
// AddAssetSaveReport instead of an unqualified existsAfter:true / echoed saved flag.
//
// These tests drive the real registered material.authoring.create_material handler
// through the dispatcher via the shared TestCreateHandlerSaveWritesToDisk contract
// runner and assert that, with save:true, the .uasset genuinely lands on disk and the
// response reports saved:true; with save:false, nothing is written and saved is false.
// If SaveMaterialAsset is reverted to the mark-dirty-only body, the save:true
// disk-presence assertion (and the saved:true assertion) fail.

#include "Misc/AutomationTest.h"
#include "Materials/Material.h"

#include "Tests/TestUtils.h"

// save:true must persist the new Material to disk and report saved:true.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaterialCreateMaterialSaveWritesToDiskTest,
    "PinWright.Material.CreateMaterial.SaveWritesToDisk",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMaterialCreateMaterialSaveWritesToDiskTest::RunTest(const FString& Parameters)
{
    return TestCreateHandlerSaveWritesToDisk(*this,
        TEXT("material.authoring.create_material"), UMaterial::StaticClass(),
        TEXT("M_MatSave"), TEXT("/Game/PinWrightTests/Material"),
        /*bSave=*/true, /*bExpectDiskFile=*/true);
}

// save:false must NOT write the .uasset and must report saved:false — the field is
// honest about durable state in both directions.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaterialCreateMaterialNoSaveLeavesNoDiskFileTest,
    "PinWright.Material.CreateMaterial.NoSaveLeavesNoDiskFile",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMaterialCreateMaterialNoSaveLeavesNoDiskFileTest::RunTest(const FString& Parameters)
{
    return TestCreateHandlerSaveWritesToDisk(*this,
        TEXT("material.authoring.create_material"), UMaterial::StaticClass(),
        TEXT("M_MatSave"), TEXT("/Game/PinWrightTests/Material"),
        /*bSave=*/false, /*bExpectDiskFile=*/false);
}
