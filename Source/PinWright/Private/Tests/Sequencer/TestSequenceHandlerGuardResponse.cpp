// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression test for B-sequencer-create-dangling-else-hangs.
//
// SequenceHandler.cpp was riddled with braceless guard clauses of the form
//     if (Cond)
//         Ctx.SendError(...);
//         return true;        // <-- dangling: unconditional, runs on EVERY path
// Only the SendError was guarded by the if; the return true; was unconditional,
// so on the VALID path the handler returned true WITHOUT ever calling
// SendSuccess / SendError. A handler that returns true without responding leaves
// the transport completion unresolved -> the live MCP caller hangs ~120s and
// nothing is created. The fix wraps each guard's body in braces so return true;
// only runs after the error is sent, restoring the success path.
//
// These tests drive the REAL registered sequencer.create / sequencer.set_display_rate
// handlers through InvokeHandlerWithCapture (production handler code, not a copy)
// with documented-valid params and assert the response capture was actually
// populated. Counterfactual: revert the braces and the valid path returns true
// without sending -> Capture.bWasCalled stays false -> these tests fail.
#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "EditorAssetLibrary.h"
#include "Tests/TestSkipReporting.h"
#include "Tests/TestUtils.h"

// ---- sequencer.create — valid path sends a response (no dangling-else hang) ----
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSequencerCreateValidPathRespondsTest,
    "PinWright.sequencer.create.ValidPathResponds",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSequencerCreateValidPathRespondsTest::RunTest(const FString& Parameters)
{
    TestTrue(TEXT("sequencer.create handler registered"),
        IsHandlerRegistered(TEXT("sequencer.create")));

    // Unique name under a throwaway folder so the asset does not pre-exist (the
    // pre-exist branch is a different, already-braced response path).
    const FString SeqName = FString::Printf(TEXT("MCP_GuardProbeSeq_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    const FString DestFolder = TEXT("/Game/MCP_SequencerGuardProbe");
    const FString FullPath = FString::Printf(TEXT("%s/%s"), *DestFolder, *SeqName);

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("name"), SeqName);
    Payload->SetStringField(TEXT("path"), DestFolder);

    FTestResponseCapture Capture;
    const bool bFound = InvokeHandlerWithCapture(TEXT("sequencer.create"), Payload, Capture);
    TestTrue(TEXT("sequencer.create handler invoked"), bFound);

    // Core regression assertion: the valid (non-empty name) path MUST send a
    // response. With the dangling-else bug the handler returns true without ever
    // calling SendSuccess / SendError and this stays false.
    TestTrue(TEXT("sequencer.create responded on the valid path (bWasCalled)"),
        Capture.bWasCalled);
    // And it must not be the no-response sentinel that the dispatcher synthesizes
    // for a silent handler.
    TestNotEqual(TEXT("response is not NO_HANDLER_RESPONSE"),
        Capture.ErrorCode, FString(TEXT("NO_HANDLER_RESPONSE")));

    CleanupTestAsset(FullPath);
    return true;
}

// ---- sequencer.create — empty name still routes through the guard's SendError ----
// Confirms the braces did not break the intended guard: an empty name must send
// the INVALID_ARGUMENT error (and still respond).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSequencerCreateEmptyNameErrorsTest,
    "PinWright.sequencer.create.EmptyNameErrors",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSequencerCreateEmptyNameErrorsTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("name"), TEXT(""));

    FTestResponseCapture Capture;
    const bool bFound = InvokeHandlerWithCapture(TEXT("sequencer.create"), Payload, Capture);
    TestTrue(TEXT("sequencer.create handler invoked"), bFound);
    TestTrue(TEXT("empty-name guard responded"), Capture.bWasCalled);
    TestFalse(TEXT("empty-name guard reported failure"), Capture.bSuccess);
    TestEqual(TEXT("empty-name guard error code"),
        Capture.ErrorCode, FString(TEXT("INVALID_ARGUMENT")));
    return true;
}

// ---- sequencer.set_display_rate — valid path sends a response ----
// The same dangling-else pattern hit set_display_rate's first guard
// (if (SeqPath.IsEmpty()) ... return true;), so before the fix EVERY valid call
// returned without responding. Drive it against a sequence created in-test.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSequencerSetDisplayRateValidPathRespondsTest,
    "PinWright.sequencer.set_display_rate.ValidPathResponds",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSequencerSetDisplayRateValidPathRespondsTest::RunTest(const FString& Parameters)
{
    TestTrue(TEXT("sequencer.set_display_rate handler registered"),
        IsHandlerRegistered(TEXT("sequencer.set_display_rate")));

    const FString SeqName = FString::Printf(TEXT("MCP_GuardRateSeq_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    const FString DestFolder = TEXT("/Game/MCP_SequencerGuardProbe");
    const FString FullPath = FString::Printf(TEXT("%s/%s"), *DestFolder, *SeqName);

    // Create the sequence first so set_display_rate has a real target.
    TSharedPtr<FJsonObject> CreatePayload = MakeShared<FJsonObject>();
    CreatePayload->SetStringField(TEXT("name"), SeqName);
    CreatePayload->SetStringField(TEXT("path"), DestFolder);
    FTestResponseCapture CreateCapture;
    InvokeHandlerWithCapture(TEXT("sequencer.create"), CreatePayload, CreateCapture);

    if (!CreateCapture.bWasCalled || !CreateCapture.bSuccess ||
        !UEditorAssetLibrary::DoesAssetExist(FullPath))
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("sequence-factory-unavailable"),
            TEXT("Could not create a probe sequence (factory unavailable in this host); "
                 "skipping set_display_rate valid-path assertion"));
        CleanupTestAsset(FullPath);
        return true;
    }

    TSharedPtr<FJsonObject> RatePayload = MakeShared<FJsonObject>();
    RatePayload->SetStringField(TEXT("path"), FullPath);
    RatePayload->SetStringField(TEXT("frameRate"), TEXT("30fps"));

    FTestResponseCapture RateCapture;
    const bool bFound =
        InvokeHandlerWithCapture(TEXT("sequencer.set_display_rate"), RatePayload, RateCapture);
    TestTrue(TEXT("sequencer.set_display_rate handler invoked"), bFound);

    // Core regression assertion: the valid path responds. With the dangling-else
    // bug the first guard's unconditional return true; fires and this stays false.
    TestTrue(TEXT("sequencer.set_display_rate responded on the valid path (bWasCalled)"),
        RateCapture.bWasCalled);
    TestNotEqual(TEXT("response is not NO_HANDLER_RESPONSE"),
        RateCapture.ErrorCode, FString(TEXT("NO_HANDLER_RESPONSE")));

    CleanupTestAsset(FullPath);
    return true;
}
