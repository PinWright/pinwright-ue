// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression test for audio.authoring.create_source_effect_preset (F-source-effect-preset-authoring).
// Before the fix, the source-effect authoring surface was a dead end: create_source_effect_chain
// made an empty USoundEffectSourcePresetChain and add_source_effect could only append a *pre-existing*
// USoundEffectSourcePreset, but no RPC created one, so add_source_effect always returned
// [PRESET_NOT_FOUND]. create_source_effect_preset creates that preset (a concrete Synthesis
// SourceEffect*Preset resolved by reflection) so the loop closes.
//
// Counterfactual: if create_source_effect_preset is reverted (removed / errored out), the preset
// asset never exists, so the "add_source_effect success + Chain.Num()==1" assertions below fail —
// exactly the dead-end the ticket reported. The concrete preset classes live in the Synthesis plugin
// (EnabledByDefault); its absence is a real environment failure for this RPC, asserted up front, not
// a skip (a passing test never returns true on a missing required fixture).
#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Misc/Guid.h"
#include "UObject/UObjectGlobals.h"
#include "Tests/TestUtils.h"
#include "Handlers/HandlerContext.h"

#if __has_include("Sound/SoundEffectSource.h")
#include "Sound/SoundEffectSource.h"
#define MCP_TEST_HAS_SOURCE_EFFECT 1
#else
#define MCP_TEST_HAS_SOURCE_EFFECT 0
#endif

#if MCP_TEST_HAS_SOURCE_EFFECT

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSourceEffectPresetAuthoringTest,
    "PinWright.Audio.SourceEffectPresetAuthoring",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSourceEffectPresetAuthoringTest::RunTest(const FString& Parameters)
{
    // Unique-per-run names to avoid collisions across repeated runs in the same editor session.
    const FString Stamp = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString TestPath = TEXT("/Game/PinWrightTests/SourceEffectPreset");
    const FString ChainName = FString::Printf(TEXT("SFXChain_%s"), *Stamp);
    const FString PresetName = FString::Printf(TEXT("SFXP_Filter_%s"), *Stamp);

    const FString ChainPath = FString::Printf(TEXT("%s/%s.%s"), *TestPath, *ChainName, *ChainName);
    const FString PresetPath = FString::Printf(TEXT("%s/%s.%s"), *TestPath, *PresetName, *PresetName);

    // The concrete preset classes live in the Synthesis plugin (EnabledByDefault). Its absence is a
    // real environment failure for this RPC, not a skip — assert it up front so the failure is legible.
    UClass* FilterClass = FindObject<UClass>(nullptr, TEXT("/Script/Synthesis.SourceEffectFilterPreset"));
    if (!FilterClass)
    {
        FilterClass = LoadObject<UClass>(nullptr, TEXT("/Script/Synthesis.SourceEffectFilterPreset"));
    }
    TestNotNull(TEXT("Synthesis SourceEffectFilterPreset class resolves (Synthesis plugin enabled)"), FilterClass);

    // --- 1. Create the (empty) source effect chain container ---
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("name"), ChainName);
        Payload->SetStringField(TEXT("path"), TestPath);
        Payload->SetBoolField(TEXT("save"), false);
        FTestResponseCapture Cap;
        TestTrue(TEXT("create_source_effect_chain dispatched"),
            InvokeHandlerWithCapture(TEXT("audio.authoring.create_source_effect_chain"), Payload, Cap));
        TestTrue(TEXT("create_source_effect_chain success"), Cap.bSuccess);
    }

    USoundEffectSourcePresetChain* Chain = FindObject<USoundEffectSourcePresetChain>(nullptr, *ChainPath);
    TestNotNull(TEXT("Chain loaded after create"), Chain);

    // --- 2. Create the source effect preset (the verb the ticket added) via a short name ---
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("name"), PresetName);
        Payload->SetStringField(TEXT("effectClass"), TEXT("Filter")); // short name -> SourceEffectFilterPreset
        Payload->SetStringField(TEXT("path"), TestPath);
        Payload->SetBoolField(TEXT("save"), false);
        FTestResponseCapture Cap;
        TestTrue(TEXT("create_source_effect_preset dispatched"),
            InvokeHandlerWithCapture(TEXT("audio.authoring.create_source_effect_preset"), Payload, Cap));
        TestTrue(TEXT("create_source_effect_preset success"), Cap.bSuccess);
    }

    USoundEffectSourcePreset* Preset = FindObject<USoundEffectSourcePreset>(nullptr, *PresetPath);
    TestNotNull(TEXT("Preset asset exists after create_source_effect_preset"), Preset);
    if (Preset && FilterClass)
    {
        TestEqual(TEXT("Preset is the resolved SourceEffectFilterPreset class"), Preset->GetClass(), FilterClass);
    }

    // --- 3. add_source_effect now succeeds with the created preset (the closed loop) ---
    // Core counterfactual: without create_source_effect_preset the preset never exists and this call
    // returns [PRESET_NOT_FOUND] with Chain.Num() == 0.
    if (Chain && Preset)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("assetPath"), ChainPath);
        Payload->SetStringField(TEXT("effectPresetPath"), PresetPath);
        Payload->SetBoolField(TEXT("bypass"), false);
        Payload->SetBoolField(TEXT("save"), false);
        FTestResponseCapture Cap;
        TestTrue(TEXT("add_source_effect dispatched"),
            InvokeHandlerWithCapture(TEXT("audio.authoring.add_source_effect"), Payload, Cap));
        TestTrue(TEXT("add_source_effect success with created preset"), Cap.bSuccess);
        TestEqual(TEXT("Chain has exactly one entry after add"), Chain->Chain.Num(), 1);
        if (Chain->Chain.Num() == 1)
        {
            TestEqual(TEXT("Chain entry references the created preset"),
                Chain->Chain[0].Preset.Get(), Preset);
        }
    }

    // --- 4. A full /Script path resolves too (path branch of the resolver) ---
    {
        const FString PresetName2 = FString::Printf(TEXT("SFXP_EQ_%s"), *Stamp);
        const FString PresetPath2 = FString::Printf(TEXT("%s/%s.%s"), *TestPath, *PresetName2, *PresetName2);
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("name"), PresetName2);
        Payload->SetStringField(TEXT("effectClass"), TEXT("/Script/Synthesis.SourceEffectEQPreset"));
        Payload->SetStringField(TEXT("path"), TestPath);
        Payload->SetBoolField(TEXT("save"), false);
        FTestResponseCapture Cap;
        TestTrue(TEXT("create_source_effect_preset (/Script path) dispatched"),
            InvokeHandlerWithCapture(TEXT("audio.authoring.create_source_effect_preset"), Payload, Cap));
        TestTrue(TEXT("create_source_effect_preset (/Script path) success"), Cap.bSuccess);
        USoundEffectSourcePreset* Preset2 = FindObject<USoundEffectSourcePreset>(nullptr, *PresetPath2);
        TestNotNull(TEXT("EQ preset asset exists"), Preset2);
        CleanupTestAsset(FString::Printf(TEXT("%s/%s"), *TestPath, *PresetName2));
    }

    // --- 5. An unresolvable effectClass is rejected, not fake-succeeded ---
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("name"), FString::Printf(TEXT("SFXP_Bogus_%s"), *Stamp));
        Payload->SetStringField(TEXT("effectClass"), TEXT("NotARealSourceEffectXyz"));
        Payload->SetStringField(TEXT("path"), TestPath);
        Payload->SetBoolField(TEXT("save"), false);
        FTestResponseCapture Cap;
        TestTrue(TEXT("create_source_effect_preset (bogus) dispatched"),
            InvokeHandlerWithCapture(TEXT("audio.authoring.create_source_effect_preset"), Payload, Cap));
        TestFalse(TEXT("bogus effectClass is not a success"), Cap.bSuccess);
        TestEqual(TEXT("bogus effectClass error code is EFFECT_CLASS_NOT_FOUND"),
            Cap.ErrorCode, FString(TEXT("EFFECT_CLASS_NOT_FOUND")));
    }

    // Best-effort cleanup (assets created with save=false; nothing on disk). The chain is torn down
    // BEFORE the preset it references, and that order is load-bearing: force-deleting a referenced
    // object makes ObjectTools null the reference and then call PostEditChangeProperty on every
    // referencer, and USoundEffectSourcePresetChain::PostEditChangeProperty derefs
    // GEngine->GetAudioDeviceManager() unguarded (Engine/Private/SoundEffectPreset.cpp) - that
    // manager is null in a -nosound automation editor, so deleting the preset first crashes the run.
    // Deleting the chain first leaves no live referencer, so the engine path is never entered.
    CleanupTestAsset(FString::Printf(TEXT("%s/%s"), *TestPath, *ChainName));
    CleanupTestAsset(FString::Printf(TEXT("%s/%s"), *TestPath, *PresetName));

    return true;
}

#endif // MCP_TEST_HAS_SOURCE_EFFECT
