// Copyright (c) 2026 Alexander Penkin. MIT License.

// Red test for B-sequencer-tick-resolution-substring-parse.
//
// sequencer.set_tick_resolution takes a `resolution` string and is supposed to build the
// MovieScene's tick resolution from the parsed number (or `num/den` rational). Instead it
// runs two loose substring fast-paths BEFORE the rational/numeric branches
// (SequenceHandler.cpp:2355-2358):
//
//     if (ResolutionStr.Contains(TEXT("24000")))      TickResolution = FFrameRate(24000, 1);
//     else if (ResolutionStr.Contains(TEXT("60000"))) TickResolution = FFrameRate(60000, 1);
//     else if (ResolutionStr.Contains(TEXT("/")))     { ...rational... }
//     else if (ResolutionStr.IsNumeric())             { ...numeric... }
//
// so ANY resolution string whose decimal form CONTAINS the substring "24000" or "60000" is
// silently clamped to 24000/1 or 60000/1 — a 10x-low error. "240000" -> 24000/1;
// "600000" -> 60000/1. Because the substring checks precede the `/` branch, the rational
// escape "240000/1" is ALSO clamped (it contains "24000"), so there is no string workaround.
// The handler then reports a bare {} success with no echo, so nothing in the response flags
// the downgrade.
//
// This test drives the PRODUCTION sequencer.set_tick_resolution handler through the real
// registration list (InvokeHandlerWithCapture), then reads the MovieScene's stored tick
// resolution back and asserts the CORRECT behavior: a requested resolution of 240000 must
// produce FFrameRate(240000, 1), NOT the substring-clamped 24000/1. Two forms the ticket
// calls out are exercised in one test — the plain numeric string "240000" and the rational
// form "240000/1" (the "no workaround" case). The expected value (240000/1) is the literal
// the caller asked for, read straight off the same MovieScene the handler mutated
// (LoadObject returns the cached UObject), never a re-implementation of the handler's parse.
//
// Fixture is content-free: a real /Game LevelSequence created via the registered
// sequencer.create handler (so set_tick_resolution's ResolveSequencePath/LoadObject resolves
// it). The default fresh-sequence tick resolution (24000/1) is captured up front purely as
// evidence; the assertions do not depend on it.
//
// Differential property: pre-fix, "240000".Contains("24000") is true, so the handler stores
// 24000/1 and BOTH the numeric-form and rational-form assertions FAIL (got 24000, wanted
// 240000), reproducing the defect. Once the handler parses the resolution to a number/rational
// before comparing (dropping the substring fast-paths), both forms store 240000/1 and the
// assertions flip green. The handler reports success both pre- and post-fix (bare {}), so the
// success check is only a precondition documenting the silent-wrong-data context; the stored
// tick-resolution assertions are what distinguish fixed from broken.
#include "Misc/AutomationTest.h"

#include "Dom/JsonObject.h"
#include "EditorAssetLibrary.h"
#include "Misc/FrameRate.h"
#include "Misc/Guid.h"
#include "Misc/ScopeExit.h"

#include "LevelSequence.h"
#include "MovieScene.h"

#include "Tests/TestUtils.h"

namespace
{
    // Distinctly named (mirrors CreateSectionRangeUnitsSequence / CreateAddKeyframeUnitsSequence
    // in sibling sequencer test .cpp files) so anonymous-namespace symbols don't ODR-collide
    // when Unity merges these TUs: create a real /Game LevelSequence via the registered
    // sequencer.create handler so set_tick_resolution's LoadObject<ULevelSequence> can resolve
    // it. Empty path + nullptr on failure.
    ULevelSequence* CreateTickResolutionProbeSequence(FAutomationTestBase& Test, FString& OutFullPath)
    {
        const FString SeqName = FString::Printf(TEXT("MCP_TickResSeq_%s"),
            *FGuid::NewGuid().ToString(EGuidFormats::Digits));
        const FString DestFolder = TEXT("/Game/MCP_TickResProbe");
        OutFullPath = FString::Printf(TEXT("%s/%s"), *DestFolder, *SeqName);

        TSharedPtr<FJsonObject> CreatePayload = MakeShared<FJsonObject>();
        CreatePayload->SetStringField(TEXT("name"), SeqName);
        CreatePayload->SetStringField(TEXT("path"), DestFolder);
        FTestResponseCapture CreateCapture;
        InvokeHandlerWithCapture(TEXT("sequencer.create"), CreatePayload, CreateCapture);

        if (!CreateCapture.bWasCalled || !CreateCapture.bSuccess ||
            !UEditorAssetLibrary::DoesAssetExist(OutFullPath))
        {
            Test.AddError(TEXT("Could not create a probe LevelSequence via sequencer.create — "
                               "the set_tick_resolution substring-parse repro cannot be exercised without it."));
            OutFullPath.Reset();
            return nullptr;
        }
        return Cast<ULevelSequence>(UEditorAssetLibrary::LoadAsset(OutFullPath));
    }

    // Drives sequencer.set_tick_resolution for one resolution string against SeqPath, asserts the
    // handler was found + succeeded (precondition — it reports success both pre- and post-fix),
    // and returns the MovieScene's stored tick resolution afterward. bInvoked reports whether the
    // success precondition held, so the caller can skip the value assertions on a fixture failure.
    // OutResponse receives the handler's response JSON so the caller can also assert the echoed
    // tickResolution (bare {} pre-fix -> the echo assertion fails there too; a {numerator,
    // denominator} object post-fix).
    FFrameRate SetTickResolutionAndReadBack(FAutomationTestBase& Test, const FString& SeqPath,
        UMovieScene& MovieScene, const FString& ResolutionArg, bool& bInvoked,
        TSharedPtr<FJsonObject>& OutResponse)
    {
        bInvoked = false;
        OutResponse.Reset();

        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("path"), SeqPath);
        Payload->SetStringField(TEXT("resolution"), ResolutionArg);

        FTestResponseCapture Capture;
        const bool bFound = InvokeHandlerWithCapture(TEXT("sequencer.set_tick_resolution"), Payload, Capture);
        if (!Test.TestTrue(TEXT("sequencer.set_tick_resolution handler is registered and invoked"), bFound))
        {
            return MovieScene.GetTickResolution();
        }
        if (!Test.TestTrue(FString::Printf(
                TEXT("sequencer.set_tick_resolution responded for resolution=\"%s\""), *ResolutionArg),
                Capture.bWasCalled))
        {
            return MovieScene.GetTickResolution();
        }
        if (!Test.TestTrue(FString::Printf(
                TEXT("sequencer.set_tick_resolution reported success for resolution=\"%s\""), *ResolutionArg),
                Capture.bSuccess))
        {
            Test.AddError(FString::Printf(
                TEXT("set_tick_resolution failed (code=%s msg=%s) — fixture problem, not the substring-parse defect"),
                *Capture.ErrorCode, *Capture.Message));
            return MovieScene.GetTickResolution();
        }

        bInvoked = true;
        OutResponse = Capture.Result;
        return MovieScene.GetTickResolution();
    }

    // Asserts the handler echoed the applied tick resolution as a {numerator, denominator} object
    // (mirroring sequencer.get_properties) matching the requested value — pre-fix the response is
    // a bare {} with no echo, so this fails there too.
    void AssertEchoedTickResolution(FAutomationTestBase& Test, const TSharedPtr<FJsonObject>& Response,
        const FString& ResolutionArg, int32 ExpectedNumerator, int32 ExpectedDenominator)
    {
        const TSharedPtr<FJsonObject>* EchoObj = nullptr;
        if (!Test.TestTrue(FString::Printf(
                TEXT("response echoes a tickResolution object for resolution=\"%s\""), *ResolutionArg),
                Response.IsValid() && Response->TryGetObjectField(TEXT("tickResolution"), EchoObj) &&
                    EchoObj != nullptr && EchoObj->IsValid()))
        {
            return;
        }
        double EchoNum = 0.0;
        double EchoDen = 0.0;
        (*EchoObj)->TryGetNumberField(TEXT("numerator"), EchoNum);
        (*EchoObj)->TryGetNumberField(TEXT("denominator"), EchoDen);
        Test.TestEqual(FString::Printf(
            TEXT("echoed tickResolution.numerator matches the applied value for resolution=\"%s\""), *ResolutionArg),
            static_cast<int32>(EchoNum), ExpectedNumerator);
        Test.TestEqual(FString::Printf(
            TEXT("echoed tickResolution.denominator matches the applied value for resolution=\"%s\""), *ResolutionArg),
            static_cast<int32>(EchoDen), ExpectedDenominator);
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSequencerSetTickResolutionSubstringParseTest,
    "PinWright.Sequencer.SetTickResolution.NumericStringNotSubstringClamped",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSequencerSetTickResolutionSubstringParseTest::RunTest(const FString& Parameters)
{
    // A real /Game LevelSequence for set_tick_resolution to mutate.
    FString FullPath;
    ULevelSequence* Sequence = CreateTickResolutionProbeSequence(*this, FullPath);
    if (!Sequence)
    {
        return false; // error already emitted by helper
    }
    ON_SCOPE_EXIT { CleanupTestAsset(FullPath); };

    UMovieScene* MovieScene = Sequence->GetMovieScene();
    if (!TestNotNull(TEXT("MovieScene present on the probe sequence"), MovieScene))
    {
        return false;
    }

    // The value the caller asks for. 240000 CONTAINS the substring "24000" that trips the
    // buggy fast-path; the correct result is exactly FFrameRate(240000, 1).
    const int32 ExpectedNumerator   = 240000;
    const int32 ExpectedDenominator = 1;
    // What the substring fast-path stores instead — evidence in the log, not an assertion target.
    const int32 ClampedNumerator    = 24000;

    const FFrameRate InitialTick = MovieScene->GetTickResolution();
    AddInfo(FString::Printf(
        TEXT("probe sequence initial tick resolution %d/%d; requesting %d/%d "
             "(the substring fast-path would clamp to %d/1)"),
        InitialTick.Numerator, InitialTick.Denominator,
        ExpectedNumerator, ExpectedDenominator, ClampedNumerator));

    // Case A — plain numeric string "240000".
    {
        bool bInvoked = false;
        TSharedPtr<FJsonObject> Response;
        const FFrameRate Result =
            SetTickResolutionAndReadBack(*this, FullPath, *MovieScene, TEXT("240000"), bInvoked, Response);
        if (bInvoked)
        {
            AddInfo(FString::Printf(
                TEXT("resolution=\"240000\" -> stored tick resolution %d/%d"),
                Result.Numerator, Result.Denominator));
            // CORRECT-BEHAVIOR ASSERTIONS (the defect surfaces here). Pre-fix Result.Numerator == 24000.
            TestEqual(TEXT("numeric \"240000\" sets tick-resolution numerator to 240000 "
                           "(not the substring-clamped 24000)"),
                Result.Numerator, ExpectedNumerator);
            TestEqual(TEXT("numeric \"240000\" sets tick-resolution denominator to 1"),
                Result.Denominator, ExpectedDenominator);
            AssertEchoedTickResolution(*this, Response, TEXT("240000"), ExpectedNumerator, ExpectedDenominator);
        }
    }

    // Case B — rational form "240000/1" (the ticket's "no string workaround" case: the substring
    // check precedes the `/` branch, so this is clamped to 24000/1 too, pre-fix).
    {
        bool bInvoked = false;
        TSharedPtr<FJsonObject> Response;
        const FFrameRate Result =
            SetTickResolutionAndReadBack(*this, FullPath, *MovieScene, TEXT("240000/1"), bInvoked, Response);
        if (bInvoked)
        {
            AddInfo(FString::Printf(
                TEXT("resolution=\"240000/1\" -> stored tick resolution %d/%d"),
                Result.Numerator, Result.Denominator));
            // CORRECT-BEHAVIOR ASSERTIONS. Pre-fix Result.Numerator == 24000 (rational escape also clamped).
            TestEqual(TEXT("rational \"240000/1\" sets tick-resolution numerator to 240000 "
                           "(the substring fast-path must not preempt the rational parse)"),
                Result.Numerator, ExpectedNumerator);
            TestEqual(TEXT("rational \"240000/1\" sets tick-resolution denominator to 1"),
                Result.Denominator, ExpectedDenominator);
            AssertEchoedTickResolution(*this, Response, TEXT("240000/1"), ExpectedNumerator, ExpectedDenominator);
        }
    }

    return true;
}
