// Copyright (c) 2026 Alexander Penkin. MIT License.

// Contract tests for sequencer.set_playhead.
//
// The verb exists so frame bursts (set position -> capture -> repeat) are
// deterministic without PIE. Its whole value is that a caller can trust the
// position it asked for was applied, so every path that CANNOT apply a position
// must say so with a typed code instead of reporting a success the caller would
// build a capture loop on top of.
//
// These tests drive the PRODUCTION registered handler through the real
// registration list (InvokeHandlerWithCapture), not a helper, and assert the
// validation surface end to end:
//   * the position/units/precondition params are registered and discoverable
//   * no position at all is rejected, rather than defaulting to frame 0
//   * an unrecognized updateMethod is rejected by name, rather than silently
//     falling back to a method the caller did not ask for
//   * a sequence that is not the one open in Sequencer, with open=false, is
//     rejected with SEQUENCE_NOT_OPEN — never a fake success
//   * a path that resolves to no asset is rejected with SEQUENCE_NOT_FOUND
//
// Deliberately NOT covered here: the display-rate/tick-resolution conversion and
// the applied playhead position itself. Both require a live Sequencer editor
// instance (ULevelSequenceEditorBlueprintLibrary addresses whichever sequence is
// open), which this suite does not stand up under -unattended. Those are verified
// at runtime against a real editor — see docs/wiki-src/sequencer.md.
#include "Misc/AutomationTest.h"

#include "Dom/JsonObject.h"
#include "EditorAssetLibrary.h"
#include "Misc/Guid.h"
#include "Misc/ScopeExit.h"

#include "LevelSequence.h"

#include "Tests/TestUtils.h"

namespace
{
    // Distinctly named (mirrors CreateSectionRangeUnitsSequence / CreateEvalReadbackSequence
    // in sibling sequencer test .cpp files) so anonymous-namespace symbols don't ODR-collide
    // when Unity merges these TUs. Creates a real /Game LevelSequence through the registered
    // sequencer.create handler so set_playhead's LoadAsset can resolve it.
    // Empty path + nullptr on failure.
    ULevelSequence* CreateSetPlayheadSequence(FAutomationTestBase& Test, FString& OutFullPath)
    {
        const FString SeqName = FString::Printf(TEXT("MCP_SetPlayheadSeq_%s"),
            *FGuid::NewGuid().ToString(EGuidFormats::Digits));
        const FString DestFolder = TEXT("/Game/MCP_SetPlayheadProbe");
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
                               "the set_playhead contract cannot be exercised without one."));
            OutFullPath.Reset();
            return nullptr;
        }
        return Cast<ULevelSequence>(UEditorAssetLibrary::LoadAsset(OutFullPath));
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSequencerSetPlayheadParamSurfaceTest,
    "PinWright.Sequencer.SetPlayhead.ParamSurfaceIsDiscoverable",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSequencerSetPlayheadParamSurfaceTest::RunTest(const FString& Parameters)
{
    // Callers discover the verb's shape from the registered param specs, so the
    // burst-critical slots must be declared, not merely readable from the payload.
    const TCHAR* RequiredParams[] = {
        TEXT("path"), TEXT("frame"), TEXT("time"),
        TEXT("updateMethod"), TEXT("forceUpdate"), TEXT("open")
    };
    for (const TCHAR* ParamName : RequiredParams)
    {
        TestNotNull(*FString::Printf(TEXT("sequencer.set_playhead declares a '%s' param"), ParamName),
            GetRegisteredParamSpec(TEXT("sequencer.set_playhead"), ParamName));
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSequencerSetPlayheadRejectsMissingPositionTest,
    "PinWright.Sequencer.SetPlayhead.RejectsMissingPosition",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSequencerSetPlayheadRejectsMissingPositionTest::RunTest(const FString& Parameters)
{
    // No frame and no time. Defaulting to 0 here would silently scrub a capture
    // burst back to the start of the sequence, so the call must be rejected.
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("path"), TEXT("/Game/MCP_SetPlayheadProbe/DoesNotMatter"));

    FTestResponseCapture Capture;
    if (!TestTrue(TEXT("sequencer.set_playhead handler is registered and invoked"),
            InvokeHandlerWithCapture(TEXT("sequencer.set_playhead"), Payload, Capture)))
    {
        return false;
    }
    TestTrue(TEXT("sequencer.set_playhead responded"), Capture.bWasCalled);
    TestFalse(TEXT("a position-less call is an error, not a success"), Capture.bSuccess);
    TestEqual(TEXT("missing position is reported as INVALID_ARGUMENT"),
        Capture.ErrorCode, FString(TEXT("INVALID_ARGUMENT")));
    // The message must name both accepted spellings — this is the caller's only
    // in-band hint about which unit to supply.
    TestTrue(TEXT("the rejection names 'frame'"), Capture.Message.Contains(TEXT("frame")));
    TestTrue(TEXT("the rejection names 'time'"), Capture.Message.Contains(TEXT("time")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSequencerSetPlayheadRejectsUnknownUpdateMethodTest,
    "PinWright.Sequencer.SetPlayhead.RejectsUnknownUpdateMethod",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSequencerSetPlayheadRejectsUnknownUpdateMethodTest::RunTest(const FString& Parameters)
{
    // A misspelled method must not quietly degrade to the default: a caller asking
    // for "jump" and silently getting "scrub" (or vice versa) changes which events
    // fire between positions, which is exactly what a burst is trying to control.
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("path"), TEXT("/Game/MCP_SetPlayheadProbe/DoesNotMatter"));
    Payload->SetNumberField(TEXT("frame"), 12.0);
    Payload->SetStringField(TEXT("updateMethod"), TEXT("teleport"));

    FTestResponseCapture Capture;
    if (!TestTrue(TEXT("sequencer.set_playhead handler is registered and invoked"),
            InvokeHandlerWithCapture(TEXT("sequencer.set_playhead"), Payload, Capture)))
    {
        return false;
    }
    TestFalse(TEXT("an unknown updateMethod is an error, not a success"), Capture.bSuccess);
    TestEqual(TEXT("unknown updateMethod is reported as INVALID_ARGUMENT"),
        Capture.ErrorCode, FString(TEXT("INVALID_ARGUMENT")));
    TestTrue(TEXT("the rejection echoes the offending value"),
        Capture.Message.Contains(TEXT("teleport")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSequencerSetPlayheadRejectsMissingSequenceTest,
    "PinWright.Sequencer.SetPlayhead.RejectsMissingSequence",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSequencerSetPlayheadRejectsMissingSequenceTest::RunTest(const FString& Parameters)
{
    // A path that resolves to nothing must be a typed lookup failure, distinct
    // from the "exists but is not open" precondition below.
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("path"),
        FString::Printf(TEXT("/Game/MCP_SetPlayheadAbsent/NoSuchSequence_%s"),
            *FGuid::NewGuid().ToString(EGuidFormats::Digits)));
    Payload->SetNumberField(TEXT("frame"), 0.0);

    FTestResponseCapture Capture;
    if (!TestTrue(TEXT("sequencer.set_playhead handler is registered and invoked"),
            InvokeHandlerWithCapture(TEXT("sequencer.set_playhead"), Payload, Capture)))
    {
        return false;
    }
    TestFalse(TEXT("an unresolvable path is an error, not a success"), Capture.bSuccess);
    TestEqual(TEXT("an unresolvable path is reported as SEQUENCE_NOT_FOUND"),
        Capture.ErrorCode, FString(TEXT("SEQUENCE_NOT_FOUND")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSequencerSetPlayheadRejectsClosedSequenceTest,
    "PinWright.Sequencer.SetPlayhead.RejectsClosedSequenceWhenOpenIsFalse",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSequencerSetPlayheadRejectsClosedSequenceTest::RunTest(const FString& Parameters)
{
    // The load-bearing no-silent-success case. The engine's editor playhead APIs
    // only ever address the sequence open in Sequencer; a caller who opts out of
    // opening must learn the position was NOT applied, or a capture burst records
    // N identical frames while every call reported success.
    FString FullPath;
    ULevelSequence* Sequence = CreateSetPlayheadSequence(*this, FullPath);
    if (!Sequence)
    {
        return false;
    }
    ON_SCOPE_EXIT { CleanupTestAsset(FullPath); };

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("path"), FullPath);
    Payload->SetNumberField(TEXT("frame"), 24.0);
    Payload->SetBoolField(TEXT("open"), false);

    FTestResponseCapture Capture;
    if (!TestTrue(TEXT("sequencer.set_playhead handler is registered and invoked"),
            InvokeHandlerWithCapture(TEXT("sequencer.set_playhead"), Payload, Capture)))
    {
        return false;
    }
    TestTrue(TEXT("sequencer.set_playhead responded"), Capture.bWasCalled);
    TestFalse(TEXT("a sequence that is not open is an error, never a success the "
                   "caller would build a capture loop on"),
        Capture.bSuccess);
    TestEqual(TEXT("a closed sequence with open=false is reported as SEQUENCE_NOT_OPEN"),
        Capture.ErrorCode, FString(TEXT("SEQUENCE_NOT_OPEN")));
    return true;
}
