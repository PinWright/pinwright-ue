// Copyright (c) 2026 Alexander Penkin. MIT License.

// Red/acceptance test for audio.authoring.create_sound_concurrency (F-sound-concurrency-asset-authoring).
// The audio.authoring surface can point a SoundCue at a USoundConcurrency group
// (set_cue_concurrency, concurrencyPath) but ships NO verb to create the USoundConcurrency
// asset that reference requires — so the shared-concurrency workflow is only half-supported.
// It mirrors the already-shipped create_sound_submix / create_source_effect_preset creators.
//
// Counterfactual (why this is red pre-fix): create_sound_concurrency does not exist, so
// InvokeHandlerWithCapture returns false, the success/asset-exists assertions fail, and there
// is no asset to close the create -> set_cue_concurrency loop against — exactly the capability
// gap the ticket reports. On the fix it flips green: the verb NewObjects a USoundConcurrency,
// maps maxCount/resolutionRule onto FSoundConcurrencySettings, and set_cue_concurrency can then
// reference a real asset entirely inside the RPC surface.
#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Misc/Guid.h"
#include "UObject/UObjectGlobals.h"
#include "Sound/SoundConcurrency.h"
#include "Sound/SoundCue.h"
#include "Tests/TestUtils.h"
#include "Handlers/HandlerContext.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSoundConcurrencyAuthoringTest,
    "PinWright.Audio.SoundConcurrencyAuthoring",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSoundConcurrencyAuthoringTest::RunTest(const FString& Parameters)
{
    // Unique-per-run names so repeated runs in the same editor session never collide.
    const FString Stamp = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString TestPath = TEXT("/Game/PinWrightTests/SoundConcurrency");
    const FString ConcName = FString::Printf(TEXT("CG_ManipulationSFX_%s"), *Stamp);
    const FString ConcPath = FString::Printf(TEXT("%s/%s.%s"), *TestPath, *ConcName, *ConcName);

    // --- 1. The creator verb the ticket adds: create a USoundConcurrency with mapped settings ---
    // Pre-fix these fail because no handler is registered under this name.
    TestTrue(TEXT("audio.authoring.create_sound_concurrency is registered"),
        IsHandlerRegistered(TEXT("audio.authoring.create_sound_concurrency")));

    const int32 RequestedMaxCount = 3;
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("name"), ConcName);
        Payload->SetStringField(TEXT("path"), TestPath);
        Payload->SetNumberField(TEXT("maxCount"), RequestedMaxCount);
        Payload->SetStringField(TEXT("resolutionRule"), TEXT("StopOldest"));
        Payload->SetBoolField(TEXT("save"), false);
        FTestResponseCapture Cap;
        TestTrue(TEXT("create_sound_concurrency dispatched"),
            InvokeHandlerWithCapture(TEXT("audio.authoring.create_sound_concurrency"), Payload, Cap));
        TestTrue(TEXT("create_sound_concurrency success"), Cap.bSuccess);
    }

    // The created asset must exist as a real USoundConcurrency (not a fake-success no-op).
    USoundConcurrency* Conc = FindObject<USoundConcurrency>(nullptr, *ConcPath);
    TestNotNull(TEXT("USoundConcurrency asset exists after create_sound_concurrency"), Conc);

    if (Conc)
    {
        // The ticket's concrete mapping: maxCount -> FSoundConcurrencySettings.MaxCount and
        // resolutionRule -> ResolutionRule. The engine default (MaxCount!=3, ResolutionRule is
        // StopFarthestThenOldest) differs from what we requested, so an unmapped create fails here.
        TestEqual(TEXT("maxCount mapped onto Concurrency.MaxCount"),
            Conc->Concurrency.MaxCount, RequestedMaxCount);
        TestEqual(TEXT("resolutionRule mapped onto Concurrency.ResolutionRule (StopOldest)"),
            (int32)Conc->Concurrency.ResolutionRule.GetValue(),
            (int32)EMaxConcurrentResolutionRule::StopOldest);

        // --- 2. Close the loop: set_cue_concurrency can now reference a real asset ---
        // This is the shared-limit flow the ticket says should live entirely on the RPC surface.
        const FString CueName = FString::Printf(TEXT("SC_Manip_%s"), *Stamp);
        const FString CuePath = FString::Printf(TEXT("%s/%s.%s"), *TestPath, *CueName, *CueName);
        {
            TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
            Payload->SetStringField(TEXT("name"), CueName);
            Payload->SetStringField(TEXT("path"), TestPath);
            Payload->SetBoolField(TEXT("save"), false);
            FTestResponseCapture Cap;
            TestTrue(TEXT("create_sound_cue dispatched"),
                InvokeHandlerWithCapture(TEXT("audio.authoring.create_sound_cue"), Payload, Cap));
            TestTrue(TEXT("create_sound_cue success"), Cap.bSuccess);
        }

        USoundCue* Cue = FindObject<USoundCue>(nullptr, *CuePath);
        TestNotNull(TEXT("SoundCue loaded after create"), Cue);
        if (Cue)
        {
            TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
            Payload->SetStringField(TEXT("assetPath"), CuePath);
            Payload->SetStringField(TEXT("concurrencyPath"), Conc->GetPathName());
            Payload->SetBoolField(TEXT("save"), false);
            FTestResponseCapture Cap;
            TestTrue(TEXT("set_cue_concurrency dispatched"),
                InvokeHandlerWithCapture(TEXT("audio.authoring.set_cue_concurrency"), Payload, Cap));
            TestTrue(TEXT("set_cue_concurrency success with the created concurrency"), Cap.bSuccess);
            TestEqual(TEXT("cue references exactly one concurrency after set"),
                Cue->ConcurrencySet.Num(), 1);
            TestTrue(TEXT("cue's concurrency set contains the created asset"),
                Cue->ConcurrencySet.Contains(Conc));
        }

        CleanupTestAsset(FString::Printf(TEXT("%s/%s"), *TestPath, *CueName));
    }

    // Best-effort cleanup (assets created with save=false; nothing on disk).
    CleanupTestAsset(FString::Printf(TEXT("%s/%s"), *TestPath, *ConcName));

    return true;
}
