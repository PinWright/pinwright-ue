// Copyright (c) 2026 Alexander Penkin. MIT License.

// Behavioural regression for B-source-effect-save-ignored. Both verbs used to read
// `save` and then always call the dirty-only McpSafeAssetSave path. These tests drive
// the handlers and assert the shared measured-save response plus disk/package state.
#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"
#include "Tests/TestUtils.h"
#include "UObject/UObjectGlobals.h"

#if __has_include("Sound/SoundEffectSource.h")
#include "Sound/SoundEffectSource.h"
#define MCP_TEST_HAS_SOURCE_EFFECT_SAVE 1
#else
#define MCP_TEST_HAS_SOURCE_EFFECT_SAVE 0
#endif

#if MCP_TEST_HAS_SOURCE_EFFECT_SAVE

namespace
{
    bool SourceEffectSaveTest_ReadBool(const TSharedPtr<FJsonObject>& Result,
        const TCHAR* Field, bool& OutValue)
    {
        return Result.IsValid() && Result->TryGetBoolField(Field, OutValue);
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSourceEffectCreateChainMeasuredPersistenceTest,
    "PinWright.Audio.SourceEffectSave.CreateChainMeasuredPersistence",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSourceEffectCreateChainMeasuredPersistenceTest::RunTest(const FString& Parameters)
{
    const FString Name = FString::Printf(TEXT("SFXChain_Save_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    const FString Folder = TEXT("/Game/PinWrightTests/SourceEffectSave");
    const FString PackageName = Folder / Name;
    const FString Filename = FPackageName::LongPackageNameToFilename(
        PackageName, FPackageName::GetAssetPackageExtension());
    ON_SCOPE_EXIT { CleanupTestAsset(PackageName); };

    TestTrue(TEXT("fresh chain has no file before handler call"),
        IFileManager::Get().FileSize(*Filename) < 0);

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("name"), Name);
    Payload->SetStringField(TEXT("path"), Folder);
    Payload->SetBoolField(TEXT("save"), true);

    FTestResponseCapture Capture;
    TestTrue(TEXT("create_source_effect_chain dispatched"),
        InvokeHandlerWithCapture(TEXT("audio.authoring.create_source_effect_chain"), Payload, Capture));
    if (!TestTrue(TEXT("create_source_effect_chain saved successfully"), Capture.bSuccess) ||
        !TestTrue(TEXT("create result present"), Capture.Result.IsValid()))
    {
        return false;
    }

    bool bSaveRequested = false;
    bool bSaved = false;
    TestTrue(TEXT("saveRequested field present"),
        SourceEffectSaveTest_ReadBool(Capture.Result, TEXT("saveRequested"), bSaveRequested));
    TestTrue(TEXT("save:true is reported"), bSaveRequested);
    TestTrue(TEXT("saved field present"),
        SourceEffectSaveTest_ReadBool(Capture.Result, TEXT("saved"), bSaved));
    TestTrue(TEXT("save:true is measured durable"), bSaved);
    TestEqual(TEXT("fresh chain reports written state"),
        Capture.Result->GetStringField(TEXT("saveState")), FString(TEXT("written")));
    TestFalse(TEXT("durable save has no pendingFlush"),
        Capture.Result->HasField(TEXT("pendingFlush")));
    TestTrue(TEXT("chain .uasset exists after success"),
        IFileManager::Get().FileSize(*Filename) > 0);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSourceEffectAddRespectsSaveFlagTest,
    "PinWright.Audio.SourceEffectSave.AddEffectRespectsSaveFlag",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSourceEffectAddRespectsSaveFlagTest::RunTest(const FString& Parameters)
{
    const FString Stamp = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString Folder = TEXT("/Game/PinWrightTests/SourceEffectSave");
    const FString ChainName = FString::Printf(TEXT("SFXChain_Add_%s"), *Stamp);
    const FString PresetName = FString::Printf(TEXT("SFXP_Add_%s"), *Stamp);
    const FString ChainPackageName = Folder / ChainName;
    const FString PresetPackageName = Folder / PresetName;
    const FString ChainObjectPath = FString::Printf(
        TEXT("%s.%s"), *ChainPackageName, *ChainName);
    const FString PresetObjectPath = FString::Printf(
        TEXT("%s.%s"), *PresetPackageName, *PresetName);
    const FString ChainFilename = FPackageName::LongPackageNameToFilename(
        ChainPackageName, FPackageName::GetAssetPackageExtension());
    ON_SCOPE_EXIT
    {
        CleanupTestAsset(ChainPackageName);
        CleanupTestAsset(PresetPackageName);
    };

    TSharedPtr<FJsonObject> CreateChain = MakeShared<FJsonObject>();
    CreateChain->SetStringField(TEXT("name"), ChainName);
    CreateChain->SetStringField(TEXT("path"), Folder);
    CreateChain->SetBoolField(TEXT("save"), true);
    FTestResponseCapture ChainCapture;
    TestTrue(TEXT("chain creation dispatched"),
        InvokeHandlerWithCapture(TEXT("audio.authoring.create_source_effect_chain"), CreateChain, ChainCapture));
    if (!TestTrue(TEXT("chain creation succeeded"), ChainCapture.bSuccess))
    {
        return false;
    }

    TSharedPtr<FJsonObject> CreatePreset = MakeShared<FJsonObject>();
    CreatePreset->SetStringField(TEXT("name"), PresetName);
    CreatePreset->SetStringField(TEXT("path"), Folder);
    CreatePreset->SetStringField(TEXT("effectClass"), TEXT("Filter"));
    CreatePreset->SetBoolField(TEXT("save"), false);
    FTestResponseCapture PresetCapture;
    TestTrue(TEXT("preset creation dispatched"),
        InvokeHandlerWithCapture(TEXT("audio.authoring.create_source_effect_preset"), CreatePreset, PresetCapture));
    if (!TestTrue(TEXT("preset creation succeeded"), PresetCapture.bSuccess))
    {
        return false;
    }

    USoundEffectSourcePresetChain* Chain = FindObject<USoundEffectSourcePresetChain>(
        nullptr, *ChainObjectPath);
    if (!TestNotNull(TEXT("created chain resolves"), Chain))
    {
        return false;
    }

    TSharedPtr<FJsonObject> AddWithoutSave = MakeShared<FJsonObject>();
    AddWithoutSave->SetStringField(TEXT("assetPath"), ChainObjectPath);
    AddWithoutSave->SetStringField(TEXT("effectPresetPath"), PresetObjectPath);
    AddWithoutSave->SetBoolField(TEXT("save"), false);
    FTestResponseCapture NoSaveCapture;
    TestTrue(TEXT("add_source_effect save:false dispatched"),
        InvokeHandlerWithCapture(TEXT("audio.authoring.add_source_effect"), AddWithoutSave, NoSaveCapture));
    if (!TestTrue(TEXT("save:false mutation succeeded"), NoSaveCapture.bSuccess) ||
        !TestTrue(TEXT("save:false result present"), NoSaveCapture.Result.IsValid()))
    {
        return false;
    }

    bool bSaveRequested = true;
    bool bSaved = true;
    TestTrue(TEXT("save:false response has saveRequested"),
        SourceEffectSaveTest_ReadBool(NoSaveCapture.Result, TEXT("saveRequested"), bSaveRequested));
    TestFalse(TEXT("save:false is not rewritten to a save request"), bSaveRequested);
    TestTrue(TEXT("save:false response has saved"),
        SourceEffectSaveTest_ReadBool(NoSaveCapture.Result, TEXT("saved"), bSaved));
    TestFalse(TEXT("save:false does not claim persistence"), bSaved);
    TestEqual(TEXT("save:false reports notRequested"),
        NoSaveCapture.Result->GetStringField(TEXT("saveState")), FString(TEXT("notRequested")));
    TestTrue(TEXT("save:false leaves a measured pending package"),
        NoSaveCapture.Result->GetBoolField(TEXT("pendingSave")));
    TestTrue(TEXT("save:false leaves the chain package dirty"), Chain->GetOutermost()->IsDirty());
    TestEqual(TEXT("first effect was added"), Chain->Chain.Num(), 1);

    TSharedPtr<FJsonObject> AddAndSave = MakeShared<FJsonObject>();
    AddAndSave->SetStringField(TEXT("assetPath"), ChainObjectPath);
    AddAndSave->SetStringField(TEXT("effectPresetPath"), PresetObjectPath);
    AddAndSave->SetBoolField(TEXT("save"), true);
    FTestResponseCapture SaveCapture;
    TestTrue(TEXT("add_source_effect save:true dispatched"),
        InvokeHandlerWithCapture(TEXT("audio.authoring.add_source_effect"), AddAndSave, SaveCapture));
    if (!TestTrue(TEXT("save:true mutation succeeded"), SaveCapture.bSuccess) ||
        !TestTrue(TEXT("save:true result present"), SaveCapture.Result.IsValid()))
    {
        return false;
    }

    TestTrue(TEXT("save:true response has saveRequested"),
        SourceEffectSaveTest_ReadBool(SaveCapture.Result, TEXT("saveRequested"), bSaveRequested));
    TestTrue(TEXT("save:true is reported"), bSaveRequested);
    TestTrue(TEXT("save:true response has saved"),
        SourceEffectSaveTest_ReadBool(SaveCapture.Result, TEXT("saved"), bSaved));
    TestTrue(TEXT("save:true is measured durable"), bSaved);
    TestEqual(TEXT("save:true reports written"),
        SaveCapture.Result->GetStringField(TEXT("saveState")), FString(TEXT("written")));
    TestFalse(TEXT("saved chain package is clean"), Chain->GetOutermost()->IsDirty());
    TestEqual(TEXT("second effect was added"), Chain->Chain.Num(), 2);
    TestTrue(TEXT("chain remains present on disk"),
        IFileManager::Get().FileSize(*ChainFilename) > 0);
    return true;
}

#endif // MCP_TEST_HAS_SOURCE_EFFECT_SAVE
