// Copyright (c) 2026 Alexander Penkin. MIT License.

// THE one definition of the assertions-skipped wire marker, and the only supported way to emit it.
//
// WHY THIS EXISTS. A test that takes a conditional-skip path reports success without running its
// assertions, and the suite's started/succeeded totals cannot tell that apart from a real pass
// (`B-test-skips-assertions-silently`). The marker below is what makes such a run visible:
// `Content/Python/check_suite_log.py` counts it and refuses to classify the run COMPLETED_CLEAN.
//
// It is a WIRE FORMAT shared with that parser, which matches it as a PREFIX -- the engine appends
// " [file(line)]" to every logged entry (`AutomationTest.cpp:1544`) -- so the parser must never be
// tightened to match a whole line. The count is read off the LOG even when a valid automation
// report wins the report-first evidence selection, because `index.json` carries no skip field and
// reading one off a report would return a structural zero (`docs/arch.md`).
//
// WHY IT IS A HEADER AND NOT A CONVENTION. The literal used to be written out at every emitter:
// nine file-local helpers under five different names plus twenty-odd inline `AddWarning` calls,
// each carrying a comment instructing the next reader to "change the literal in both places or in
// neither". It was already spelled two ways -- with and without the trailing colon -- and only
// survived because the Python regex spells the colon `:?`. Tightening that regex in a future
// cleanup would have silently dropped those files from the gate. One definition makes the property
// structural instead of aspirational: there is a single literal, so it cannot be spelled two ways,
// and the instruction the comments gave is now enforced by the compiler rather than by discipline.
//
// THREE PROPERTIES ARE LOAD-BEARING. None is decoration.
//
//  1. `AddWarning`, NOT `UE_LOG(..., Warning, ...)`. A UE_LOG warning raised inside a running
//     automation test is captured by FAutomationTestOutputDevice and ELEVATED TO AN ERROR whenever
//     UAutomationControllerSettings::bElevateLogWarningsToErrors is set -- and it defaults to TRUE
//     (`AutomationControllerSettings.cpp:14`; the elevation is `AutomationTest.cpp:134`). Routing
//     the marker through UE_LOG would therefore fail the test on every headless host, which is
//     exactly the already-fixed defect `B-tests-host-dependent-fixtures-hard-fail`. `AddWarning`
//     appends an EAutomationEventType::Warning entry and nothing else: the worker derives the
//     test's state from GetErrorTotal() alone (`AutomationWorkerModule.cpp:209`), so the test stays
//     `Result={Success}` and the headless green is preserved.
//
//  2. Warning verbosity, not Info. `AddInfo` lands as `LogAutomationController: Display:`,
//     textually identical to every other line of a passing run -- which is how this went unnoticed
//     for 20 markers in Tests/Render/ that no log-based checker could ever see. A Warning entry is
//     logged as `LogAutomationController: Warning:` (`AutomationControllerManager.cpp:1698`) and is
//     separately counted into the JSON report's `succeededWithWarnings` bucket, so the skip is
//     visible to a reader and the test lands in the report's warned bucket -- which records that
//     SOMETHING warned, never that an assertion was stepped over. Only the log distinguishes the
//     two; see item 3.
//
//  3. The leading token is a WIRE FORMAT shared with `Content/Python/check_suite_log.py`, which
//     greps it, reconciles a `skipped` outcome from it, and refuses to call such a run
//     COMPLETED_CLEAN. That count is read off the LOG even when a valid automation report wins the
//     report-first evidence selection, because `index.json` carries no skip field and reading one
//     off a report would return a structural zero (`docs/arch.md`). The engine appends
//     " [file(line)]" to every logged entry (`AutomationTest.cpp:1544`), so that parser matches a
//     PREFIX and must never be tightened to match a whole line.

#pragma once

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"

namespace PinWrightTestSkip
{
    // The wire literal. Every emitter in the tree goes through the function below, so this is the
    // only place it appears in C++; its counterpart is the prefix regex in
    // Content/Python/mcp_proxy.py (`_count_assertion_skips`), pinned by
    // Content/Python/tests/test_skip_marker_literal.py.
    inline const TCHAR* Marker()
    {
        return TEXT("PINWRIGHT_ASSERTIONS_SKIPPED:");
    }

    // Report that this test stepped over its substantive assertions and why.
    //
    // The message carries three things, all load-bearing. The MARKER is what the suite gate greps.
    // The TEST FULL NAME is what turns a counted marker into a named test -- emitters that omitted
    // it left `check_suite_log` reporting a skip it could not attribute, which is the difference
    // between "re-run this test on a capable host" and "something, somewhere, measured nothing".
    // The REASON SLUG is a stable identifier for the class of skip, so a run can be compared
    // against an earlier one without diffing prose; the DETAIL carries the measurement that
    // produced the decision, because a recalibration has to start from the numbers this host
    // actually produced rather than from a bare "skipped".
    //
    // Call this INSTEAD of the early return's own AddWarning, not beside it: a skip with no marker
    // is invisible downstream (`docs/rpc-design.md` section 17).
    inline FString FormatSkipMessage(const FString& TestFullName, const TCHAR* ReasonSlug,
        const FString& Detail)
    {
        return FString::Printf(TEXT("%s %s reason=%s -- %s"),
            Marker(), *TestFullName, ReasonSlug, *Detail);
    }

    inline void SkipAssertions(FAutomationTestBase& Test, const TCHAR* ReasonSlug,
        const FString& Detail)
    {
        Test.AddWarning(FormatSkipMessage(Test.GetTestFullName(), ReasonSlug, Detail));
    }
}
