// Copyright (c) 2026 Alexander Penkin. MIT License.

// Red test for E-dialogue-context-default-not-reclaimed.
//
// A freshly created UDialogueWave always carries exactly one engine-seeded, pristine
// FDialogueContextMapping: the constructor (Engine/Private/DialogueWave.cpp) runs
// ContextMappings.Add(FDialogueContextMapping()) and UDialogueWaveFactory::FactoryCreateNew
// leaves ContextMappings[0] with a null Speaker / empty Targets when no initial context is
// supplied (as create_dialogue_wave does). So a brand-new wave has ContextMappings.Num()==1.
//
// The ticket's defect: audio.authoring.set_dialogue_context is append-only in its default
// (non-replace) branch — AudioAuthoringHandler.cpp does an unconditional
// Wave->ContextMappings.Add(NewMapping) and never reclaims that pristine seed. So the FIRST
// set_dialogue_context on a fresh wave leaves the empty seed in place and appends a second
// entry, yielding ContextMappings.Num()==2 and reporting contextCount:2 — an off-by-one for
// what is semantically the wave's one and only context.
//
// This test asserts the CORRECT behavior: the first set_dialogue_context should reclaim the
// pristine seed, so a fresh wave with one meaningful context ends up with exactly ONE mapping
// (carrying the speaker we set) and reports contextCount:1. Pre-fix this fails (Num()==2,
// contextCount==2); the reclaim fix flips it green.
#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Misc/Guid.h"
#include "Tests/TestUtils.h"
#include "Tests/TestSkipReporting.h"

#if __has_include("Sound/DialogueVoice.h")
#include "Sound/DialogueVoice.h"
#include "Sound/DialogueWave.h"
#define MCP_DIALOGUE_RECLAIM_TEST_HAS_DIALOGUE 1
#else
#define MCP_DIALOGUE_RECLAIM_TEST_HAS_DIALOGUE 0
#endif

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAudioAuthoringSetDialogueContextFirstSetReclaimsSeedTest,
    "PinWright.audio.authoring.set_dialogue_context.FirstSetReclaimsSeedMapping",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAudioAuthoringSetDialogueContextFirstSetReclaimsSeedTest::RunTest(const FString& Parameters)
{
#if MCP_DIALOGUE_RECLAIM_TEST_HAS_DIALOGUE
    const FString Stamp = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString TestPath = FString::Printf(TEXT("/Game/PinWrightTests/DialogueReclaim_%s"), *Stamp);

    const FString VoiceName = TEXT("Speaker");
    const FString WaveName = TEXT("Line01");
    const FString VoicePath = FString::Printf(TEXT("%s/%s.%s"), *TestPath, *VoiceName, *VoiceName);
    const FString WavePath = FString::Printf(TEXT("%s/%s.%s"), *TestPath, *WaveName, *WaveName);

    // --- fixtures: a speaker voice and a dialogue wave, created via the production handlers
    //     (save=false, in-memory) exactly as the set_class_parent regression test builds its
    //     SoundClass fixtures. A create failure is a test FAILURE, not a skip. ---
    {
        TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
        P->SetStringField(TEXT("name"), VoiceName);
        P->SetStringField(TEXT("path"), TestPath);
        P->SetBoolField(TEXT("save"), false);
        FTestResponseCapture Cap;
        TestTrue(TEXT("create_dialogue_voice dispatched"),
            InvokeHandlerWithCapture(TEXT("audio.authoring.create_dialogue_voice"), P, Cap));
        TestTrue(TEXT("create_dialogue_voice success"), Cap.bSuccess);
    }
    {
        TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
        P->SetStringField(TEXT("name"), WaveName);
        P->SetStringField(TEXT("path"), TestPath);
        P->SetBoolField(TEXT("save"), false);
        FTestResponseCapture Cap;
        TestTrue(TEXT("create_dialogue_wave dispatched"),
            InvokeHandlerWithCapture(TEXT("audio.authoring.create_dialogue_wave"), P, Cap));
        TestTrue(TEXT("create_dialogue_wave success"), Cap.bSuccess);
    }

    UDialogueVoice* SpeakerVoice = FindObject<UDialogueVoice>(nullptr, *VoicePath);
    UDialogueWave* Wave = FindObject<UDialogueWave>(nullptr, *WavePath);
    TestNotNull(TEXT("speaker DialogueVoice fixture created"), SpeakerVoice);
    TestNotNull(TEXT("DialogueWave fixture created"), Wave);

    if (SpeakerVoice && Wave)
    {
        // Precondition: a fresh wave carries exactly the one engine-seeded pristine mapping.
        TestEqual(TEXT("fresh DialogueWave carries exactly the engine-seeded empty mapping"),
            Wave->ContextMappings.Num(), 1);

        // --- the one meaningful set_dialogue_context (default replace=false) ---
        TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
        P->SetStringField(TEXT("assetPath"), WavePath);
        P->SetStringField(TEXT("speakerPath"), VoicePath);
        P->SetBoolField(TEXT("save"), false);
        FTestResponseCapture Cap;
        TestTrue(TEXT("set_dialogue_context dispatched"),
            InvokeHandlerWithCapture(TEXT("audio.authoring.set_dialogue_context"), P, Cap));
        TestTrue(TEXT("set_dialogue_context success"), Cap.bSuccess);

        // Correct behavior #1: the response reports contextCount:1 (the seed was reclaimed,
        // not appended past). Pre-fix this is 2.
        int32 ReportedContextCount = -1;
        if (Cap.Result.IsValid())
        {
            Cap.Result->TryGetNumberField(TEXT("contextCount"), ReportedContextCount);
        }
        TestEqual(TEXT("first set reclaims the seed: response contextCount is 1, not 2"),
            ReportedContextCount, 1);

        // Correct behavior #2: the live asset holds exactly one mapping. Pre-fix Num()==2
        // (pristine seed + appended entry).
        TestEqual(TEXT("first set reclaims the seed: ContextMappings holds exactly one entry"),
            Wave->ContextMappings.Num(), 1);

        // Correct behavior #3: that single mapping carries the speaker we set — proving the
        // seed slot was reclaimed and populated, not left as a stray empty mapping. Pre-fix
        // ContextMappings[0] is the untouched seed (null Speaker).
        if (Wave->ContextMappings.Num() >= 1)
        {
            TestTrue(TEXT("the single mapping carries the speaker we set (seed reclaimed, not left empty)"),
                Wave->ContextMappings[0].Context.Speaker == SpeakerVoice);
        }
    }

    CleanupTestAsset(FString::Printf(TEXT("%s/%s"), *TestPath, *WaveName));
    CleanupTestAsset(FString::Printf(TEXT("%s/%s"), *TestPath, *VoiceName));
    return true;
#else
    // Dialogue system headers are unavailable on this engine — the handler itself is compiled
    // out and reports DIALOGUE_NOT_AVAILABLE, so there is no behavior to reproduce here.
    PinWrightTestSkip::SkipAssertions(*this, TEXT("dialogue-system-unavailable"),
        TEXT("Dialogue system unavailable on this engine; skipping reclaim reproduction."));
    return true;
#endif
}
