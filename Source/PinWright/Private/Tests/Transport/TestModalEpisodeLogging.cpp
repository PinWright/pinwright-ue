// Copyright (c) 2026 Alexander Penkin. MIT License.

// Tests that a modal block leaves a trace on disk.
//
// THE DEFECT THIS PINS. EDITOR_BLOCKED_ON_MODAL is a WIRE-ONLY field: both emit sites
// (McpRequestCore.cpp BuildPingResult and BuildEditorNotReadyToolResult) write it into an
// HTTP response body, and neither is a UE_LOG. The client-side proxy re-emits it to stderr
// only (Content/Python/mcp_proxy.py: "Diagnostics go to stderr ONLY"). So the single
// failure class that no amount of polling can clear used to leave Saved/Logs/ completely
// silent - a grep for it over the whole log corpus returns zero hits whether it fired once
// or a thousand times.
//
// That is not a cosmetic gap. It made a claim of "779 EDITOR_BLOCKED_ON_MODAL occurrences
// across five days" neither confirmable nor refutable without reading plugin source, and
// two triage passes were spent on a failure class that had never fired. A condition that
// cannot be counted from disk will be miscounted from somewhere else.
//
// The probe is a process-global latch, so every test resets it first and last. Automation
// tests run synchronously on the game thread, so the live subsystem's 0.1 s ticker cannot
// interleave inside a RunTest body and clear the latch underneath an assertion.

#include "Misc/AutomationTest.h"
#include "Compat/EngineVersionCompat.h"

#include "Handlers/ErrorCodes.h"
#include "Transport/ModalStateProbe.h"

#include "HAL/PlatformTime.h"
#include "Logging/LogVerbosity.h"
#include "Misc/OutputDevice.h"
#include "Misc/OutputDeviceRedirector.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace PinWrightModalLogTest
{
    // Records every line the engine logs while it is in scope. Registered on
    // GLog rather than replacing it, so the automation framework's own capture
    // (and the .log file the assertion is really about) still sees everything.
    class FScopedLogCapture : public FOutputDevice
    {
    public:
        FScopedLogCapture()
        {
            if (GLog)
            {
                GLog->AddOutputDevice(this);
            }
        }

        virtual ~FScopedLogCapture()
        {
            if (GLog)
            {
                GLog->RemoveOutputDevice(this);
            }
        }

        virtual void Serialize(const TCHAR* Message, ELogVerbosity::Type Verbosity,
                               const FName& Category) override
        {
            Lines.Add(FString(Message));
        }

        virtual bool CanBeUsedOnAnyThread() const override { return true; }
        virtual bool CanBeUsedOnMultipleThreads() const override { return true; }

        int32 CountContaining(const TCHAR* Needle) const
        {
            int32 Found = 0;
            for (const FString& Line : Lines)
            {
                if (Line.Contains(Needle))
                {
                    ++Found;
                }
            }
            return Found;
        }

    private:
        TArray<FString> Lines;
    };
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FModalEpisodeIsLoggedOnceTest,
    "PinWright.transport.liveness.Modal.EpisodeLeavesLogTrace",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FModalEpisodeIsLoggedOnceTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightModalLogTest;

    // Both lines are Warning-verbosity by design (a blocked editor is not a normal
    // state), so they are declared expected here or the automation framework would
    // report them as test warnings.
    AddExpectedMessagePlain(TEXT("Modal window owns the game thread"),
        ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1);
    AddExpectedMessagePlain(TEXT("Modal block cleared after"),
        ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1);

    ModalStateProbe::ResetForTests();

    {
        FScopedLogCapture Capture;

        // Arm the latch the way the nested Slate modal loop does, then tick it again:
        // the loop broadcasts at ~60 Hz for the whole life of the dialog, and a
        // per-tick log would bury the file under thousands of identical lines.
        ModalStateProbe::OnModalLoopTick(0.0f);
        TestTrue(TEXT("the first modal-loop tick latches the block"),
            ModalStateProbe::IsBlocked());
        for (int32 Tick = 0; Tick < 8; ++Tick)
        {
            ModalStateProbe::OnModalLoopTick(0.016f);
        }

        TestEqual(TEXT("the block is logged once per episode, not once per modal-loop tick"),
            Capture.CountContaining(TEXT("Modal window owns the game thread")), 1);
        // The log has to name the code an agent will actually see on the wire, or a
        // reader cannot connect the two halves of the same event.
        TestEqual(TEXT("the log names the wire error code the caller receives"),
            Capture.CountContaining(ErrorCodes::ERR_EDITOR_BLOCKED_ON_MODAL), 1);
        TestEqual(TEXT("no clear line is written while the block is still held"),
            Capture.CountContaining(TEXT("Modal block cleared after")), 0);

        // The core ticker running again is the proof the modal is gone.
        ModalStateProbe::NoteGameThreadAlive();
        TestFalse(TEXT("the heartbeat clears the latch"), ModalStateProbe::IsBlocked());
        TestEqual(TEXT("the episode is closed with exactly one clear line"),
            Capture.CountContaining(TEXT("Modal block cleared after")), 1);

        // A healthy editor must stay silent: NoteGameThreadAlive runs every 0.1 s.
        for (int32 Tick = 0; Tick < 8; ++Tick)
        {
            ModalStateProbe::NoteGameThreadAlive();
        }
        TestEqual(TEXT("an unblocked heartbeat writes nothing"),
            Capture.CountContaining(TEXT("Modal block cleared after")), 1);
    }

    ModalStateProbe::ResetForTests();
    return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
