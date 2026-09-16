// Copyright (c) 2026 Alexander Penkin. MIT License.

// Direct tests for the shared skip emitter (Tests/TestSkipReporting.h).
//
// WHY THIS EXISTS, AND WHY IT IS NOT REDUNDANT WITH THE PYTHON SUITE. Two halves of the
// assertions-skipped mechanism were each covered and the join between them was not. The Python
// fixtures in Content/Python/tests/ prove `check_suite_log` classifies a log carrying the marker
// as COMPLETED_WITH_SKIPS; older suite logs prove an `AddWarning` message reaches the automation
// log. What nothing covered is that the C++ emitters actually PRODUCE that text on that channel.
// The gap is invisible in a green run by construction: the last full suite was 4288/4288/0 with
// ZERO markers, so no skip site fired and the emitters were exercised not at all. A mechanism only
// exercised on unhappy hosts cannot be validated by a happy host's run -- it has to be tested
// directly.
//
// WHY THE PROBE, AND WHY NO REAL SKIP IS EMITTED HERE. The obvious test -- call SkipAssertions on
// `*this` and read the warning back -- would put a real PINWRIGHT_ASSERTIONS_SKIPPED marker into
// every suite log, so `check_suite_log` would refuse COMPLETED_CLEAN on every run forever. That
// trades an unproven mechanism for a permanently untrustworthy verdict. The probe below is a
// scoped FAutomationTestBase that is never run: `AddWarning` writes only into that instance's own
// FAutomationTestExecutionInfo (`AutomationTest.cpp:1735-1742`) and nothing global, so the message
// is readable here and reaches no log. The base registers in its constructor and unregisters in
// its destructor (`AutomationTest.cpp:159-177`), so the registry is unchanged either side of this
// test -- which the last assertion checks rather than assumes.
#include "Misc/AutomationTest.h"
#include "Tests/TestSkipReporting.h"

#include "Compat/EngineVersionCompat.h"

namespace
{
    // Distinctly prefixed: anonymous namespaces merge inside one Unity translation unit.
    const TCHAR* const PWSkipProbeName = TEXT("PinWrightSkipEmissionProbe");

    // A test that exists only to be emitted into. Never enqueued: RunTest is never called, and the
    // name carries no dot so it cannot become a branch node of any real id
    // (see infra.automation_registry.NoPrefixCollisions).
    class FPWSkipProbe final : public FAutomationTestBase
    {
    public:
        FPWSkipProbe() : FAutomationTestBase(PWSkipProbeName, /*bInComplexTask=*/false) {}

        virtual MCP_AUTOMATION_TEST_FLAGS GetTestFlags() const override
        {
            return EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter;
        }
        virtual uint32 GetRequiredDeviceNum() const override { return 1; }
        virtual FString GetBeautifiedTestName() const override { return PWSkipProbeName; }
        virtual void GetTests(TArray<FString>& OutBeautifiedNames,
            TArray<FString>& OutTestCommands) const override {}
        virtual bool RunTest(const FString& Parameters) override { return true; }
    };
}

// ============================================================================
// The emitter puts the exact wire text on the WARNING channel.
//
// Counterfactual: change SkipAssertions to AddInfo and the warning-total assertion fails; change
// it to AddError and both the error-total assertion and the message assertion fail. Change the
// literal's trailing colon and the prefix assertion fails. None of the three can be weakened
// without a red test.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPWSkipMarkerEmissionTest,
    "PinWright.infra.skip_marker.EmitterWritesTheWireTextAsAWarning",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPWSkipMarkerEmissionTest::RunTest(const FString& Parameters)
{
    const FString Detail = TEXT("the fixture host drew no frame");

    {
        FPWSkipProbe Probe;
        PinWrightTestSkip::SkipAssertions(Probe, TEXT("no-preview-viewport"), Detail);

        FAutomationTestExecutionInfo Info;
        Probe.GetExecutionInfo(Info);

        // The channel. `AddInfo` events are never written to the automation log at all, so a
        // marker on that channel is invisible rather than quiet; `AddError` would fail the test on
        // every headless host, re-creating B-tests-host-dependent-fixtures-hard-fail.
        TestEqual(TEXT("the emitter produced exactly one warning"), Info.GetWarningTotal(), 1);
        TestEqual(TEXT("the emitter produced no error"), Info.GetErrorTotal(), 0);

        FString Emitted;
        for (const FAutomationExecutionEntry& Entry : Info.GetEntries())
        {
            if (Entry.Event.Type == EAutomationEventType::Warning)
            {
                Emitted = Entry.Event.Message;
                break;
            }
        }
        TestTrue(TEXT("the entry carries the Warning event type, not Info"), !Emitted.IsEmpty());

        // The wire text, byte for byte. Written out here rather than rebuilt from the helper: a
        // test that formats the expectation with the code under test agrees with itself whatever
        // the format becomes.
        const FString Expected = FString::Printf(
            TEXT("PINWRIGHT_ASSERTIONS_SKIPPED: %s reason=no-preview-viewport -- %s"),
            PWSkipProbeName, *Detail);
        TestEqual(TEXT("the emitted message is the expected wire text"), Emitted, Expected);

        // ...and the property the log-side parser actually depends on: the marker leads the line.
        // The engine appends " [file(line)]" downstream, which is why that parser matches a prefix.
        TestTrue(TEXT("the message STARTS with the marker, which the parser prefix-matches"),
            Emitted.StartsWith(PinWrightTestSkip::Marker()));
    }

    // The probe unregistered itself. Asserted rather than assumed, because a leaked registration
    // would add a phantom id to GetValidTestNames on every later enumeration.
    TArray<FAutomationTestInfo> Registered;
    FAutomationTestFramework::Get().GetValidTestNames(Registered);
    bool bProbeStillRegistered = false;
    for (const FAutomationTestInfo& Info : Registered)
    {
        bProbeStillRegistered |= Info.GetTestName() == PWSkipProbeName;
    }
    TestFalse(TEXT("the probe left no registration behind"), bProbeStillRegistered);

    return true;
}

// ============================================================================
// Both id shapes the log-side parser tolerates still format correctly.
//
// mcp_proxy.py matches the marker with an OPTIONAL colon and takes the remainder as best-effort
// context, so a marker with no test id still COUNTS even though it cannot be attributed. Every
// emitter in this tree now carries the id, which is the shape worth having; this pins that the
// id-less shape a future emitter (or an older log) can still produce stays countable.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPWSkipMarkerShapesTest,
    "PinWright.infra.skip_marker.BothIdShapesStayCountable",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPWSkipMarkerShapesTest::RunTest(const FString& Parameters)
{
    const FString WithId = PinWrightTestSkip::FormatSkipMessage(
        TEXT("PinWright.render.capture.SomeTest"), TEXT("no-gpu"), TEXT("detail"));
    TestEqual(TEXT("the with-id shape is the documented wire text"), WithId,
        FString(TEXT("PINWRIGHT_ASSERTIONS_SKIPPED: PinWright.render.capture.SomeTest "
                     "reason=no-gpu -- detail")));

    // The degenerate shape: no test name to attribute it to. It must still lead with the marker,
    // because that prefix is the whole of what the gate counts.
    const FString NoId = PinWrightTestSkip::FormatSkipMessage(
        FString(), TEXT("no-gpu"), TEXT("detail"));
    TestTrue(TEXT("the no-id shape still leads with the marker"),
        NoId.StartsWith(PinWrightTestSkip::Marker()));
    TestTrue(TEXT("the no-id shape still carries its reason slug"),
        NoId.Contains(TEXT("reason=no-gpu")));

    // The literal itself, asserted as a LITERAL and never against Marker(): comparing the constant
    // against the same constant would still pass if the constant's value changed, and this string
    // is a wire format shared with a Python parser that cannot see this header.
    TestEqual(TEXT("the marker literal is spelled with its trailing colon, once, here"),
        FString(PinWrightTestSkip::Marker()), FString(TEXT("PINWRIGHT_ASSERTIONS_SKIPPED:")));

    return true;
}
