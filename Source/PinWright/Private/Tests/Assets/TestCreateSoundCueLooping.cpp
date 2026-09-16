// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression test for B-create-sound-cue-looping-noop.
//
// audio.authoring.create_sound_cue previously gated the entire node-graph
// build (wave player + looping + modulator) behind `if (!WavePath.IsEmpty())`,
// so passing looping/volume/pitch without a wavePath silently dropped those
// params and returned a clean success with an empty cue (firstNode null).
// These tests drive the real registered handler through the dispatcher and
// assert the requested nodes actually materialize even with no wavePath. They
// fail if the gating fix is reverted.

#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "UObject/UObjectGlobals.h"

#include "Sound/SoundCue.h"
#include "Sound/SoundNode.h"
#include "Sound/SoundNodeLooping.h"
#include "Sound/SoundNodeModulator.h"

#include "Tests/TestUtils.h"

namespace
{
    FString MakeUniqueCueName(const TCHAR* Prefix)
    {
        return FString::Printf(TEXT("%s_%s"), Prefix, *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    }

    // Resolves the created SoundCue from the handler's reported assetPath.
    // AddAssetVerification (Utils/AssetUtils.cpp) overwrites the result's
    // assetPath with the asset's *package* path (e.g. /Game/.../SC_X), not the
    // object path (/Game/.../SC_X.SC_X) — this is the plugin-wide create_*
    // contract. Build the object path from it (mirrors CleanupTestAsset in
    // TestUtils.h) and resolve the in-memory (save=false) cue with
    // StaticFindObject, the proven resolver for handler-created assets.
    USoundCue* ResolveCueFromResult(const TSharedPtr<FJsonObject>& Result)
    {
        if (!Result.IsValid())
        {
            return nullptr;
        }
        FString AssetPath;
        if (!Result->TryGetStringField(TEXT("assetPath"), AssetPath) || AssetPath.IsEmpty())
        {
            return nullptr;
        }
        FString ObjectPath = AssetPath;
        // Append .AssetName only when assetPath is a package path (no object delimiter).
        if (!ObjectPath.Contains(TEXT(".")))
        {
            const FString AssetName = FPackageName::GetLongPackageAssetName(AssetPath);
            if (!AssetName.IsEmpty())
            {
                ObjectPath = FString::Printf(TEXT("%s.%s"), *AssetPath, *AssetName);
            }
        }
        return Cast<USoundCue>(StaticFindObject(USoundCue::StaticClass(), nullptr, *ObjectPath));
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCreateSoundCueLoopingNoWaveBuildsLoopingNodeTest,
    "PinWright.Assets.SoundCue.CreateSoundCue.LoopingNoWaveBuildsLoopingNode",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCreateSoundCueLoopingNoWaveBuildsLoopingNodeTest::RunTest(const FString& Parameters)
{
    const FString CueName = MakeUniqueCueName(TEXT("SC_LoopNoWave"));
    const FString CuePath = TEXT("/Game/PinWrightTests/Audio");

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("name"), CueName);
    Payload->SetStringField(TEXT("path"), CuePath);
    Payload->SetBoolField(TEXT("looping"), true);
    Payload->SetBoolField(TEXT("save"), false);

    FTestResponseCapture Capture;
    const bool bFound = InvokeHandlerWithCapture(TEXT("audio.authoring.create_sound_cue"), Payload, Capture);
    TestTrue(TEXT("create_sound_cue handler is registered"), bFound);
    TestTrue(TEXT("handler responded"), Capture.bWasCalled);
    TestTrue(TEXT("create_sound_cue succeeded"), Capture.bSuccess);

    USoundCue* Cue = ResolveCueFromResult(Capture.Result);
    TestNotNull(TEXT("created SoundCue resolves from assetPath"), Cue);
    if (!Cue)
    {
        return false;
    }

    // The core regression assertion: with looping requested and no wavePath,
    // the cue must be rooted at a USoundNodeLooping. Pre-fix this was null.
    TestNotNull(TEXT("FirstNode is set even without a wavePath"), Cue->FirstNode.Get());
    USoundNodeLooping* LoopRoot = Cast<USoundNodeLooping>(Cue->FirstNode);
    TestNotNull(TEXT("FirstNode is a USoundNodeLooping"), LoopRoot);

    if (Cue->GetPackage())
    {
        CleanupTestAsset(FString::Printf(TEXT("%s/%s"), *CuePath, *CueName));
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCreateSoundCueVolumePitchNoWaveBuildsModulatorTest,
    "PinWright.Assets.SoundCue.CreateSoundCue.VolumePitchNoWaveBuildsModulator",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCreateSoundCueVolumePitchNoWaveBuildsModulatorTest::RunTest(const FString& Parameters)
{
    const FString CueName = MakeUniqueCueName(TEXT("SC_ModNoWave"));
    const FString CuePath = TEXT("/Game/PinWrightTests/Audio");

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("name"), CueName);
    Payload->SetStringField(TEXT("path"), CuePath);
    Payload->SetNumberField(TEXT("volume"), 0.5);
    Payload->SetNumberField(TEXT("pitch"), 1.5);
    Payload->SetBoolField(TEXT("save"), false);

    FTestResponseCapture Capture;
    const bool bFound = InvokeHandlerWithCapture(TEXT("audio.authoring.create_sound_cue"), Payload, Capture);
    TestTrue(TEXT("create_sound_cue handler is registered"), bFound);
    TestTrue(TEXT("create_sound_cue succeeded"), Capture.bSuccess);

    USoundCue* Cue = ResolveCueFromResult(Capture.Result);
    TestNotNull(TEXT("created SoundCue resolves from assetPath"), Cue);
    if (!Cue)
    {
        return false;
    }

    // Non-default volume/pitch with no wavePath must root the cue at a
    // modulator carrying those values. Pre-fix this was silently dropped.
    USoundNodeModulator* ModRoot = Cast<USoundNodeModulator>(Cue->FirstNode);
    TestNotNull(TEXT("FirstNode is a USoundNodeModulator"), ModRoot);
    if (ModRoot)
    {
        TestEqual(TEXT("modulator volume captured"), ModRoot->VolumeMin, 0.5f);
        TestEqual(TEXT("modulator pitch captured"), ModRoot->PitchMax, 1.5f);
    }

    if (Cue->GetPackage())
    {
        CleanupTestAsset(FString::Printf(TEXT("%s/%s"), *CuePath, *CueName));
    }
    return true;
}
