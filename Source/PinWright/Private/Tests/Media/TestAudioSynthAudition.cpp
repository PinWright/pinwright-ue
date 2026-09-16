// Copyright (c) 2026 Alexander Penkin. MIT License.

// Tests for audio.synth.audition and AudioGen/PwAudioAudition.h.
//
// HEADLESS SAFETY. The suite runs under -unattended, where the host may have no
// audio device manager at all (UEngine::UseSound() == false, UnrealEngine.cpp:4262).
// Nothing here asserts that a sound was audible. Every test asserts a CONTRACT that
// holds either way:
//   - the source-selection rejections (rpc-design.md §3),
//   - the stop form and the §11 `previous` / `stopWith` payload,
//   - a null GEditor failing with a message instead of crashing,
//   - the tick-unsafe table entry (§10),
//   - and, for the one test that actually starts playback, an either/or that pins
//     BOTH directions: no audio device must produce a reported failure, and a
//     success must carry a `playing` flag equal to an independent read of the
//     editor's own component - so a literal `{"playing": true}` fails the test
//     (§1, §12).

#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"
#include "Tests/TestUtils.h"
#include "Tests/TestSkipReporting.h"

#include "AudioGen/PwAudioAudition.h"
#include "AudioGen/PwAudioBuffer.h"
#include "AudioGen/PwCandidateRegistry.h"
#include "Dispatch/SafePoint.h"
#include "Handlers/ErrorCodes.h"

#include "Components/AudioComponent.h"
#include "Editor.h"
#include "Editor/EditorEngine.h"
#include "Sound/SoundWave.h"
#include "UObject/Package.h"

// Named namespace, not anonymous: Unity merges this TU with the sibling audio tests,
// and TestSoundWaveAuthoringHandler.cpp already owns an anonymous `MakeTransientWave`.
namespace PwAuditionTestHelpers
{
    const TCHAR* const MethodName = TEXT("audio.synth.audition");

    // A USoundWave with no imported audio data, addressable by a /Game/ object path.
    // The handler resolves it in-memory via StaticFindObject, so it never needs a
    // package on disk. AddToRoot keeps it alive for the duration of the test.
    USoundWave* MakeAuditionFixtureWave(FString& OutPackagePath)
    {
        OutPackagePath = FString::Printf(TEXT("/Game/PinWrightTests/SW_Audition_%s"),
            *FGuid::NewGuid().ToString(EGuidFormats::Digits));
        const FString AssetName = FPackageName::GetLongPackageAssetName(OutPackagePath);
        UPackage* Package = CreatePackage(*OutPackagePath);
        USoundWave* Wave = NewObject<USoundWave>(Package, FName(*AssetName),
            RF_Public | RF_Standalone);
        if (Wave)
        {
            Wave->AddToRoot();
        }
        return Wave;
    }
}

// =========================================================================
// A. RejectsMissingSource - §3: neither source supplied is an error, never a
//    silent no-op and never an implicit stop.
// =========================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAudioSynthAuditionRejectsMissingSourceTest,
    "PinWright.audio.synth.audition.RejectsMissingSource",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAudioSynthAuditionRejectsMissingSourceTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();

    FTestResponseCapture Capture;
    const bool bFound = InvokeHandlerWithCapture(
        PwAuditionTestHelpers::MethodName, Payload, Capture);

    TestTrue(TEXT("audio.synth.audition is registered"), bFound);
    TestTrue(TEXT("handler responded"), Capture.bWasCalled);
    TestFalse(TEXT("an empty payload is an error, not a silent no-op"), Capture.bSuccess);
    TestEqual(TEXT("error code is AUDITION_FAILED"),
        Capture.ErrorCode, FString(TEXT("AUDITION_FAILED")));
    // The message must name the way out (§7).
    TestTrue(TEXT("the message names assetPath as a remedy"),
        Capture.Message.Contains(TEXT("assetPath")));
    return true;
}

// =========================================================================
// B. RejectsUnknownAsset - an unloadable path errors instead of falling back,
//    and must not have taken the shared preview slot on the way out (§11).
// =========================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAudioSynthAuditionRejectsUnknownAssetTest,
    "PinWright.audio.synth.audition.RejectsUnknownAsset",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAudioSynthAuditionRejectsUnknownAssetTest::RunTest(const FString& Parameters)
{
    const UAudioComponent* SlotBefore = PwGetAuditionComponent();

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"),
        FString::Printf(TEXT("/Game/PinWrightTests/DoesNotExist_%s"),
            *FGuid::NewGuid().ToString(EGuidFormats::Digits)));

    FTestResponseCapture Capture;
    const bool bFound = InvokeHandlerWithCapture(
        PwAuditionTestHelpers::MethodName, Payload, Capture);

    TestTrue(TEXT("handler found"), bFound);
    TestTrue(TEXT("handler responded"), Capture.bWasCalled);
    TestFalse(TEXT("an unresolvable assetPath is an error"), Capture.bSuccess);
    TestEqual(TEXT("error code is ASSET_NOT_FOUND"),
        Capture.ErrorCode, FString(TEXT("ASSET_NOT_FOUND")));

    // Source resolution happens BEFORE the shared slot is touched, so a rejected
    // call cannot have displaced somebody else's preview.
    TestTrue(TEXT("the shared preview slot was not touched by a rejected call"),
        PwGetAuditionComponent() == SlotBefore);
    return true;
}

// =========================================================================
// C. RejectsAmbiguousSource - two sources at once is a caller error, not a
//    silently-picked winner.
// =========================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAudioSynthAuditionRejectsAmbiguousSourceTest,
    "PinWright.audio.synth.audition.RejectsAmbiguousSource",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAudioSynthAuditionRejectsAmbiguousSourceTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), TEXT("/Game/PinWrightTests/Whatever"));
    Payload->SetBoolField(TEXT("stop"), true);

    FTestResponseCapture Capture;
    const bool bFound = InvokeHandlerWithCapture(
        PwAuditionTestHelpers::MethodName, Payload, Capture);

    TestTrue(TEXT("handler found"), bFound);
    TestFalse(TEXT("assetPath + stop is rejected"), Capture.bSuccess);
    TestEqual(TEXT("error code is INVALID_ARGUMENT"),
        Capture.ErrorCode, FString(TEXT("INVALID_ARGUMENT")));
    return true;
}

// =========================================================================
// D. StopReportsPreviousState - the §11 undo form. Works with or without an
//    audio device: stopping needs only GEditor.
// =========================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAudioSynthAuditionStopReportsPreviousStateTest,
    "PinWright.audio.synth.audition.StopReportsPreviousState",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAudioSynthAuditionStopReportsPreviousStateTest::RunTest(const FString& Parameters)
{
    if (!GEditor)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("AUDIO-SKIP: no GEditor in this process; the stop form has no editor "
                         "preview slot to operate on."));
        return true;
    }

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetBoolField(TEXT("stop"), true);

    FTestResponseCapture Capture;
    const bool bFound = InvokeHandlerWithCapture(
        PwAuditionTestHelpers::MethodName, Payload, Capture);

    TestTrue(TEXT("handler found"), bFound);
    TestTrue(TEXT("handler responded"), Capture.bWasCalled);
    TestTrue(FString::Printf(TEXT("stop succeeds (errorCode='%s')"), *Capture.ErrorCode),
        Capture.bSuccess);
    TestTrue(TEXT("stop returned a result payload"), Capture.Result.IsValid());
    if (!Capture.bSuccess || !Capture.Result.IsValid())
    {
        return false;
    }

    bool bStopped = false;
    TestTrue(TEXT("response carries a measured `stopped`"),
        Capture.Result->TryGetBoolField(TEXT("stopped"), bStopped));
    TestTrue(TEXT("nothing is playing after stop"), bStopped);

    bool bPlaying = true;
    TestTrue(TEXT("response carries `playing`"),
        Capture.Result->TryGetBoolField(TEXT("playing"), bPlaying));
    TestFalse(TEXT("`playing` is false after stop"), bPlaying);
    // Independent read of the engine's own component - the write path cannot fake this.
    const UAudioComponent* After = PwGetAuditionComponent();
    TestFalse(TEXT("the editor's preview component really is not playing"),
        (After != nullptr) && After->IsPlaying());

    // The §11 previous block: shape and honesty.
    const TSharedPtr<FJsonObject>* Previous = nullptr;
    TestTrue(TEXT("response carries a `previous` block"),
        Capture.Result->TryGetObjectField(TEXT("previous"), Previous));
    if (Previous && (*Previous).IsValid())
    {
        bool bMeasured = false;
        TestTrue(TEXT("`previous.measured` is present"),
            (*Previous)->TryGetBoolField(TEXT("measured"), bMeasured));
        TestTrue(TEXT("`previous` was actually measured (GEditor exists here)"), bMeasured);

        bool bWasPlaying = true;
        TestTrue(TEXT("`previous.wasPlaying` is present"),
            (*Previous)->TryGetBoolField(TEXT("wasPlaying"), bWasPlaying));

        bool bComponentPresent = false;
        TestTrue(TEXT("`previous.previewComponentPresent` is present"),
            (*Previous)->TryGetBoolField(TEXT("previewComponentPresent"), bComponentPresent));

        FString PreviousSoundPath;
        TestTrue(TEXT("`previous.soundPath` is present (possibly empty)"),
            (*Previous)->TryGetStringField(TEXT("soundPath"), PreviousSoundPath));

        // `displacedPlayingPreview` must agree with what was measured, not be a literal.
        bool bDisplaced = !bWasPlaying;
        TestTrue(TEXT("response carries `displacedPlayingPreview`"),
            Capture.Result->TryGetBoolField(TEXT("displacedPlayingPreview"), bDisplaced));
        TestTrue(TEXT("`displacedPlayingPreview` matches `previous.wasPlaying`"),
            bDisplaced == bWasPlaying);
    }

    // The undo pointer names a verb that exists (§7: a remedy must be reachable).
    const TSharedPtr<FJsonObject>* StopWith = nullptr;
    TestTrue(TEXT("response carries `stopWith`"),
        Capture.Result->TryGetObjectField(TEXT("stopWith"), StopWith));
    if (StopWith && (*StopWith).IsValid())
    {
        FString StopMethod;
        (*StopWith)->TryGetStringField(TEXT("method"), StopMethod);
        TestEqual(TEXT("`stopWith.method` is audio.synth.audition"),
            StopMethod, FString(PwAuditionTestHelpers::MethodName));
        TestTrue(TEXT("`stopWith.method` names a registered handler"),
            IsHandlerRegistered(StopMethod));

        const TSharedPtr<FJsonObject>* StopArgs = nullptr;
        TestTrue(TEXT("`stopWith.args` is present"),
            (*StopWith)->TryGetObjectField(TEXT("args"), StopArgs));
        if (StopArgs && (*StopArgs).IsValid())
        {
            bool bStopArg = false;
            (*StopArgs)->TryGetBoolField(TEXT("stop"), bStopArg);
            TestTrue(TEXT("`stopWith.args.stop` is true"), bStopArg);
        }
    }
    return true;
}

// =========================================================================
// E. RejectsCandidateWithoutRegistry - with no resolver bound the seam refuses
//    rather than guessing. AudioSynthCandidateHandler.cpp binds the resolver at
//    static init, so the test unbinds it for the duration of the call and puts it
//    back on every exit path (§11 applies to test fixtures too) - otherwise this
//    case would silently stop running the moment the registry landed.
// =========================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAudioSynthAuditionRejectsCandidateWithoutRegistryTest,
    "PinWright.audio.synth.audition.RejectsCandidateWithoutRegistry",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAudioSynthAuditionRejectsCandidateWithoutRegistryTest::RunTest(const FString& Parameters)
{
    FPwAuditionCandidateResolver SavedResolver = PwAuditionCandidateResolver();
    ON_SCOPE_EXIT
    {
        PwAuditionCandidateResolver() = SavedResolver;
    };
    PwAuditionCandidateResolver().Unbind();

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("candidateId"), TEXT("cand-not-a-real-id"));

    FTestResponseCapture Capture;
    const bool bFound = InvokeHandlerWithCapture(
        PwAuditionTestHelpers::MethodName, Payload, Capture);

    TestTrue(TEXT("handler found"), bFound);
    TestFalse(TEXT("an unresolvable candidateId is an error, not a fake success"),
        Capture.bSuccess);
    TestEqual(TEXT("error code is ASSET_NOT_FOUND"),
        Capture.ErrorCode, FString(TEXT("ASSET_NOT_FOUND")));
    TestTrue(TEXT("the message steers the caller to assetPath"),
        Capture.Message.Contains(TEXT("assetPath")));
    return true;
}

// =========================================================================
// F. GatedAsTickUnsafe - §10. Audition drives the editor audio device from the
//    handler stack, so it must be in the shared table (never a hand-written gate).
// =========================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAudioSynthAuditionGatedAsTickUnsafeTest,
    "PinWright.audio.synth.audition.GatedAsTickUnsafe",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAudioSynthAuditionGatedAsTickUnsafeTest::RunTest(const FString& Parameters)
{
    TestTrue(TEXT("audio.synth.audition is in the tick-unsafe method table"),
        PinWrightSafePoint::IsTickUnsafeMethod(FString(PwAuditionTestHelpers::MethodName)));
    TestTrue(TEXT("the table entry is spelled the same as the registered handler"),
        PinWrightSafePoint::GetTickUnsafeMethods().Contains(
            FString(PwAuditionTestHelpers::MethodName)));
    return true;
}

// =========================================================================
// G. EmptyBufferIsRejected - PwAuditionBuffer refuses a 0-frame buffer before
//    it allocates anything. Pure library test, no audio device involved.
// =========================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAudioSynthAuditionEmptyBufferIsRejectedTest,
    "PinWright.audio.synth.audition.EmptyBufferIsRejected",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAudioSynthAuditionEmptyBufferIsRejectedTest::RunTest(const FString& Parameters)
{
    FPwAudioBuffer Empty;
    TestEqual(TEXT("precondition: a default-constructed buffer has no frames"),
        Empty.NumFrames(), 0);

    FString Error;
    TestFalse(TEXT("PwAuditionBuffer refuses an empty buffer"),
        PwAuditionBuffer(Empty, Error));
    TestFalse(TEXT("the refusal carries a reason"), Error.IsEmpty());
    return true;
}

// =========================================================================
// H. NullEditorFailsCleanly - the suite runs unattended, and a commandlet has
//    no GEditor at all. Every entry point must report that rather than crash.
//    GEditor is nulled for exactly the duration of these synchronous calls and
//    restored on every exit path; nothing else runs on this stack in between.
// =========================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAudioSynthAuditionNullEditorFailsCleanlyTest,
    "PinWright.audio.synth.audition.NullEditorFailsCleanly",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAudioSynthAuditionNullEditorFailsCleanlyTest::RunTest(const FString& Parameters)
{
    UEditorEngine* SavedEditor = GEditor;
    if (!SavedEditor)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("AUDIO-SKIP: GEditor is already null in this process; the guard under "
                         "test is the ambient condition, so there is nothing to simulate."));
        return true;
    }

    // Leave the preview slot quiet before pretending the editor is gone, so the
    // restore below cannot hand back a slot this test started something in.
    PwStopAudition();

    GEditor = nullptr;
    ON_SCOPE_EXIT
    {
        GEditor = SavedEditor;
    };

    FString Reason;
    TestFalse(TEXT("PwIsAuditionSlotObservable reports unavailable with no GEditor"),
        PwIsAuditionSlotObservable(Reason));
    TestFalse(TEXT("...and says why"), Reason.IsEmpty());

    Reason.Empty();
    TestFalse(TEXT("PwIsAuditionAvailable reports unavailable with no GEditor"),
        PwIsAuditionAvailable(Reason));
    TestFalse(TEXT("...and says why"), Reason.IsEmpty());

    const FPwAuditionPreviewState State = PwCaptureAuditionPreviewState();
    TestFalse(TEXT("an unobservable slot reports bMeasured=false, not a clean reading"),
        State.bMeasured);
    TestFalse(TEXT("an unmeasured snapshot never claims something was playing"),
        State.bPlaying);
    TestNull(TEXT("PwGetAuditionComponent is null with no GEditor"), PwGetAuditionComponent());

    FString Error;
    TestFalse(TEXT("PwAuditionSoundBase fails with no GEditor"),
        PwAuditionSoundBase(nullptr, Error));
    TestFalse(TEXT("...and says why"), Error.IsEmpty());

    // Must not crash, must not touch anything.
    PwStopAudition();
    return true;
}

// =========================================================================
// I. PlayingIsMeasuredNotAsserted - the one test that starts playback.
//    Both directions are pinned: with no audio device the verb must REPORT the
//    failure, and with one it must report a `playing` flag equal to an
//    independent read of the editor's own component. A hardcoded
//    {"playing": true} fails the second branch; a fake success fails the first.
// =========================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAudioSynthAuditionPlayingIsMeasuredTest,
    "PinWright.audio.synth.audition.PlayingIsMeasuredNotAsserted",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAudioSynthAuditionPlayingIsMeasuredTest::RunTest(const FString& Parameters)
{
    if (!GEditor)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("AUDIO-SKIP: no GEditor in this process; there is no preview audio "
                         "device to audition through."));
        return true;
    }

    FString PackagePath;
    USoundWave* Wave = PwAuditionTestHelpers::MakeAuditionFixtureWave(PackagePath);
    TestNotNull(TEXT("fixture SoundWave created"), Wave);
    if (!Wave)
    {
        return false;
    }

    ON_SCOPE_EXIT
    {
        // Release the shared slot first: the preview component holds a strong
        // reference to the wave through its UPROPERTY Sound.
        PwStopAudition();
        Wave->RemoveFromRoot();
        CleanupTestAsset(PackagePath);
    };

    const bool bHasAudioDevice = GEditor->UseSound();

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), Wave->GetPathName());

    FTestResponseCapture Capture;
    const bool bFound = InvokeHandlerWithCapture(
        PwAuditionTestHelpers::MethodName, Payload, Capture);
    TestTrue(TEXT("handler found"), bFound);
    TestTrue(TEXT("handler responded"), Capture.bWasCalled);
    TestTrue(TEXT("a result payload came back on both branches"), Capture.Result.IsValid());
    if (!Capture.Result.IsValid())
    {
        return false;
    }

    // Common to both branches: `playing` is present, and it is never true unless the
    // editor really is holding a component for the sound we asked for.
    bool bReportedPlaying = false;
    TestTrue(TEXT("response carries `playing`"),
        Capture.Result->TryGetBoolField(TEXT("playing"), bReportedPlaying));
    const UAudioComponent* Component = PwGetAuditionComponent();
    if (bReportedPlaying)
    {
        TestNotNull(TEXT("`playing` is only true when a preview component exists"),
            Component);
        if (Component)
        {
            TestTrue(TEXT("`playing` is only true for the sound that was requested"),
                Component->Sound == Wave);
        }
    }

    const TSharedPtr<FJsonObject>* Previous = nullptr;
    TestTrue(TEXT("both branches carry the §11 `previous` block"),
        Capture.Result->TryGetObjectField(TEXT("previous"), Previous));

    if (!bHasAudioDevice)
    {
        // The failure direction (§12). Without a device manager
        // ResetPreviewAudioComponent returns null (EditorEngine.cpp:2891), so the verb
        // must say so instead of reporting a sound nobody can hear.
        AddInfo(TEXT("AUDIO-SKIP: UEngine::UseSound() is false on this host, so the audible "
                     "path is not exercised; asserting the no-device failure contract instead."));
        TestFalse(TEXT("no audio device produces a reported failure, not a fake success"),
            Capture.bSuccess);
        TestEqual(TEXT("error code is AUDITION_FAILED"),
            Capture.ErrorCode, FString(TEXT("AUDITION_FAILED")));
        TestFalse(TEXT("a failed audition never reports itself as playing"), bReportedPlaying);

        bool bStarted = true;
        Capture.Result->TryGetBoolField(TEXT("started"), bStarted);
        TestFalse(TEXT("a failed audition reports started=false"), bStarted);
        return true;
    }

    // A device exists. The audition may still be refused by the audio engine, so accept
    // either outcome - but hold each one to its own contract.
    if (!Capture.bSuccess)
    {
        TestEqual(TEXT("a refused audition reports AUDITION_FAILED"),
            Capture.ErrorCode, FString(TEXT("AUDITION_FAILED")));
        TestFalse(TEXT("a refused audition never reports itself as playing"), bReportedPlaying);
        return true;
    }

    bool bStarted = false;
    TestTrue(TEXT("response carries `started`"),
        Capture.Result->TryGetBoolField(TEXT("started"), bStarted));
    TestTrue(TEXT("a successful audition reports started=true"), bStarted);

    // The teeth: `playing` must equal an independent read of the editor's component.
    // No engine tick runs between the handler's measurement and this one, so the two
    // cannot legitimately disagree - and a hardcoded literal will, because a
    // zero-length wave finishes inside Play() (AudioComponent.cpp:926-928).
    const bool bIndependentlyPlaying = (Component != nullptr) && Component->IsPlaying();
    TestTrue(TEXT("`playing` matches UAudioComponent::IsPlaying() read off the editor"),
        bReportedPlaying == bIndependentlyPlaying);

    // The sound identity is read back off the component, not echoed from the request.
    FString SoundPath;
    TestTrue(TEXT("response carries `soundPath`"),
        Capture.Result->TryGetStringField(TEXT("soundPath"), SoundPath));
    TestEqual(TEXT("`soundPath` is the wave that was auditioned"),
        SoundPath, Wave->GetPathName());

    // A caller must be able to tell how long to wait, and to undo the mutation.
    TestTrue(TEXT("response carries `durationSeconds`"),
        Capture.Result->HasField(TEXT("durationSeconds")));
    TestTrue(TEXT("response carries `stopWith`"),
        Capture.Result->HasField(TEXT("stopWith")));
    return true;
}

// =========================================================================
// J. SurfacesRegistryMissErrorCode - the candidate miss code must reach the wire
//    unflattened. An evicted candidate ("re-render, mind the budget") and an
//    unknown id ("your id is wrong") have divergent remedies and therefore
//    divergent codes; the audition door hardcoded ASSET_NOT_FOUND for both, so an
//    agent branching on the code - the first thing error handling reads - went off
//    fixing the wrong thing (rpc-design.md §5b: enumerate every door into the
//    same state).
//
//    The expected code is taken from FPwCandidateRegistry's own mapping for a
//    GENUINELY evicted id, never from a literal, so the registry and the audition
//    verb cannot drift apart later.
// =========================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAudioSynthAuditionSurfacesRegistryMissErrorCodeTest,
    "PinWright.audio.synth.audition.SurfacesRegistryMissErrorCode",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAudioSynthAuditionSurfacesRegistryMissErrorCodeTest::RunTest(const FString& Parameters)
{
    // A PRIVATE registry, driven to a real eviction. Its budgets are constructor arguments
    // for exactly this reason (PwCandidateRegistry.h:131-136), so nothing here disturbs the
    // session registry other verbs are using.
    FPwCandidateRegistry Local(FPwCandidateRegistry::DefaultMaxBytes, /*InMaxCandidates=*/ 1);

    FPwCandidate First;
    First.Buffer.SetNumFrames(64);
    const FString EvictedId = Local.Add(MoveTemp(First));
    TestFalse(TEXT("the registry issued an id for the first candidate"), EvictedId.IsEmpty());

    FPwCandidate Second;
    Second.Buffer.SetNumFrames(64);
    Local.Add(MoveTemp(Second));  // a count budget of 1 reclaims the first

    const FPwCandidateLookupResult Miss = Local.Get(EvictedId);
    if (Miss.Status != EPwCandidateLookup::Evicted)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("eviction-fixture-unavailable"),
            FString::Printf(
                TEXT("SEAM-SKIP: could not stage an evicted candidate (status=%s). The registry's "
                     "eviction shape changed; this fixture needs updating rather than the verb."),
                FPwCandidateRegistry::LexLookupStatus(Miss.Status)));
        return true;
    }

    // The registry's own answer for this id. Not a literal.
    const FString ExpectedCode = FPwCandidateRegistry::MissErrorCode(Miss.Status);
    const FString ExpectedStatus = FPwCandidateRegistry::LexLookupStatus(Miss.Status);

    FPwAuditionCandidateResolver SavedResolver = PwAuditionCandidateResolver();
    ON_SCOPE_EXIT
    {
        PwAuditionCandidateResolver() = SavedResolver;
    };

    // Report that real miss through the seam, exactly as
    // AudioSynthCandidateHandler.cpp's binder does for the session registry.
    PwAuditionCandidateResolver().BindLambda(
        [&Local, &Miss](const FString& Id, FPwAudioBuffer& OutBuffer, FString& OutErrorCode,
                        FString& OutError, TSharedPtr<FJsonObject>& OutErrorData) -> bool
        {
            OutErrorCode = FPwCandidateRegistry::MissErrorCode(Miss.Status);
            OutError = FPwCandidateRegistry::MakeMissMessage(Id, Miss);
            OutErrorData = Local.BuildMissPayload(Id, Miss);
            return false;
        });

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("candidateId"), EvictedId);

    FTestResponseCapture Capture;
    TestTrue(TEXT("handler found"),
        InvokeHandlerWithCapture(PwAuditionTestHelpers::MethodName, Payload, Capture));
    TestTrue(TEXT("handler responded"), Capture.bWasCalled);
    TestFalse(TEXT("an evicted candidate is an error, not a fake success"), Capture.bSuccess);

    // The teeth. Pre-fix this was the hardcoded ASSET_NOT_FOUND.
    TestEqual(TEXT("audition emits the registry's own miss code for this id"),
        Capture.ErrorCode, ExpectedCode);
    TestFalse(TEXT("the miss code is not flattened to ASSET_NOT_FOUND"),
        Capture.ErrorCode == FString(ErrorCodes::ERR_ASSET_NOT_FOUND));

    // The structured half has to survive the trip too, or the code is the only signal left.
    if (Capture.Result.IsValid())
    {
        FString Status;
        Capture.Result->TryGetStringField(TEXT("status"), Status);
        TestEqual(TEXT("the structured miss keeps its status discriminator"),
            Status, ExpectedStatus);
    }
    else
    {
        AddError(TEXT("the resolver's structured miss payload never reached the response"));
    }

    // The other direction: a resolver that names no code must still produce the documented
    // fallback rather than an empty code on the wire.
    PwAuditionCandidateResolver().BindLambda(
        [](const FString&, FPwAudioBuffer&, FString&, FString& OutError,
           TSharedPtr<FJsonObject>&) -> bool
        {
            OutError = TEXT("resolver declined without naming a code");
            return false;
        });

    FTestResponseCapture Fallback;
    TestTrue(TEXT("handler found (fallback case)"),
        InvokeHandlerWithCapture(PwAuditionTestHelpers::MethodName, Payload, Fallback));
    TestFalse(TEXT("a code-less resolver failure is still an error"), Fallback.bSuccess);
    TestEqual(TEXT("a code-less resolver failure falls back to ASSET_NOT_FOUND"),
        Fallback.ErrorCode, FString(ErrorCodes::ERR_ASSET_NOT_FOUND));
    return true;
}
