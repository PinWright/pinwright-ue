// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression tests for the blueprint.search liveness fix.
//
// The defect: BlueprintIndexHandler.cpp waited for the Find-in-Blueprints worker
// with
//
//     while (!Search->IsComplete()) { FPlatformProcess::Sleep(0.01f); }
//
// and nothing bounded it. FStreamSearch::Run walks the whole FiB corpus
// (FindInBlueprintManager.cpp:169-223); if the worker stalls or the corpus is large
// enough, the game thread spins there forever inside a verb agents call constantly.
// The stall probe reports such a wedge through `ping` after 90s, but detecting a
// wedge is not fixing one.
//
// The fix: a caller-visible `timeoutSeconds` bound (default 120). On expiry the
// handler drains whatever the worker matched so far — GetFilteredItems takes the
// same SearchCriticalSection the worker appends under, so partial results survive —
// answers with timedOut=true, and hands the stop-and-join to the core ticker via
// DetachStreamSearchToCoreTicker. That last part is load-bearing: both obvious
// alternatives re-introduce an unbounded block on the game thread.
//   - FStreamSearch::EnsureCompletion spins until the worker exits AND pumps the
//     game-thread task queue while waiting (FindInBlueprintManager.cpp:246-254).
//   - Dropping the TSharedRef runs ~FStreamSearch -> ~FRunnableThread ->
//     Kill(true) -> WaitForSingleObject(INFINITE)
//     (Core/Private/Microsoft/MicrosoftRunnableThread.h:51-57, :90-101).

#include "Misc/AutomationTest.h"

#include "Containers/Ticker.h"
#include "Dom/JsonObject.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"

#include "FindInBlueprintManager.h"
#include "Handlers/Blueprint/BlueprintIndexHandler.h"
#include "Tests/TestUtils.h"

// ============================================================================
// 1. The bound is declared, so callers can see and raise it
// ============================================================================

// The wait is only genuinely bounded if the bound is part of the method contract:
// an undeclared param would additionally be rejected by the dispatcher's
// unknown-param validation, making the timeout unreachable from the wire.
//
// Counterfactual: drop the RPC_PARAM_DEF("timeoutSeconds", ...) line and this fails.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintSearchDeclaresTimeoutSecondsTest,
    "PinWright.blueprint.search.DeclaresTimeoutSeconds",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBlueprintSearchDeclaresTimeoutSecondsTest::RunTest(const FString& Parameters)
{
    const FParamSpec* Spec =
        GetRegisteredParamSpec(TEXT("blueprint.search"), TEXT("timeoutSeconds"));
    if (!TestTrue(TEXT("blueprint.search declares a timeoutSeconds parameter"),
            Spec != nullptr))
    {
        return false;
    }

    TestEqual(TEXT("timeoutSeconds is a number"), Spec->Type, FString(TEXT("number")));
    TestFalse(TEXT("timeoutSeconds is optional"), Spec->bRequired);
    TestFalse(TEXT("timeoutSeconds documents its default"), Spec->Default.IsEmpty());
    return true;
}

// ============================================================================
// 2. Every response discloses whether the search actually finished
// ============================================================================

// A truncated answer that looks identical to a complete one is worse than no
// answer, so `timedOut` is unconditional on both response shapes. This drives the
// no-blueprints-in-scope early-out (autoIndex=false keeps the FiB pump out of the
// test), which is the branch that can be exercised without a real corpus walk.
//
// Counterfactual: emit `timedOut` only on the timeout path and this fails.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintSearchAlwaysReportsTimedOutTest,
    "PinWright.blueprint.search.AlwaysReportsTimedOut",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBlueprintSearchAlwaysReportsTimedOutTest::RunTest(const FString& Parameters)
{
    FTestResponseCapture Capture;
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("query"), TEXT("PinWrightBoundedWaitProbe"));
    // A path no project mounts content under, so the asset-registry scope is empty
    // and the handler answers without ever starting an FStreamSearch.
    Payload->SetStringField(TEXT("path"), TEXT("/Game/__PinWrightNoSuchPath__"));
    Payload->SetBoolField(TEXT("autoIndex"), false);

    TestTrue(TEXT("blueprint.search is registered"),
        InvokeHandlerWithCapture(TEXT("blueprint.search"), Payload, Capture));
    TestTrue(TEXT("blueprint.search responded"), Capture.bWasCalled);
    TestTrue(TEXT("blueprint.search succeeded"), Capture.bSuccess);

    if (!Capture.Result.IsValid())
    {
        AddError(TEXT("blueprint.search produced no result object."));
        return false;
    }

    bool bTimedOut = true;
    TestTrue(TEXT("the response carries timedOut"),
        Capture.Result->TryGetBoolField(TEXT("timedOut"), bTimedOut));
    TestFalse(TEXT("an empty-scope search did not time out"), bTimedOut);
    return true;
}

// ============================================================================
// 3. The detach primitive — the part that keeps the game thread free
// ============================================================================

// Builds a REAL FStreamSearch (the same empty-query construction the auto-index
// trigger uses), hands it to DetachStreamSearchToCoreTicker, and asserts:
//   a) the call itself returns promptly — it must not join the worker, and
//   b) the worker is stopped and joined from the core ticker afterwards.
//
// Counterfactual: replace the ticker hand-off with a direct
// Search->EnsureCompletion() and (a) starts blocking for the full corpus walk;
// return without stopping the search at all and (b) never observes completion.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintSearchDetachJoinsOffRpcStackTest,
    "PinWright.blueprint.search.DetachJoinsOffRpcStack",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBlueprintSearchDetachJoinsOffRpcStackTest::RunTest(const FString& Parameters)
{
    FStreamSearchOptions Options;
    const TSharedRef<FStreamSearch> Search = MakeShared<FStreamSearch>(TEXT(""), Options);

    const double DetachStart = FPlatformTime::Seconds();
    PinWright::BlueprintIndexHandler::DetachStreamSearchToCoreTicker(Search);
    const double DetachSeconds = FPlatformTime::Seconds() - DetachStart;

    // Generous: the point is "did not wait for the worker", not a latency budget.
    TestTrue(FString::Printf(
                 TEXT("detaching returned promptly (%.3fs) instead of joining the worker"),
                 DetachSeconds),
        DetachSeconds < 2.0);

    // Stop() was signalled, so the worker leaves its ContinueSearchQuery loop at the
    // next opportunity; the ticker then joins it and drops its reference.
    const double WaitStart = FPlatformTime::Seconds();
    while (!Search->IsComplete() && (FPlatformTime::Seconds() - WaitStart) < 60.0)
    {
        FTSTicker::GetCoreTicker().Tick(0.01f);
        FPlatformProcess::Sleep(0.001f);
    }

    TestTrue(TEXT("the detached search completed off the RPC stack"), Search->IsComplete());

    // Drain one more pass so the ticker's one-shot delegate has certainly run its
    // EnsureCompletion and removed itself before this scope drops its reference —
    // otherwise ~FStreamSearch here would be the thing joining the thread, which is
    // exactly what the detach exists to avoid.
    FTSTicker::GetCoreTicker().Tick(0.01f);
    return true;
}

// ============================================================================
// 4. The cost of the call is not misrepresented by its own parameter docs
//    (board: E-blueprint-search-path-and-autoindex)
// ============================================================================

// `path` was declared as "Content path scope for the search" — one word, "scope",
// that every caller reads as "this is how I limit the work". It is not: the handler
// hands the whole query to FStreamSearch and drops out-of-scope roots afterwards
// (see the resolution loop in FinishSearchRun), and on the autoIndex path the index
// wait happens before any of that. Narrowing `/Game` to a subfolder changes the
// result set and nothing else. A parameter description is the only thing most
// callers will ever read about cost, so it is the contract under test here.
//
// Counterfactual: restore the old description and both positive assertions fail.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintSearchPathIsDocumentedAsAResultFilterTest,
    "PinWright.blueprint.search.PathIsDocumentedAsAResultFilter",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBlueprintSearchPathIsDocumentedAsAResultFilterTest::RunTest(const FString& Parameters)
{
    const FParamSpec* Spec = GetRegisteredParamSpec(TEXT("blueprint.search"), TEXT("path"));
    if (!TestTrue(TEXT("blueprint.search declares a path parameter"), Spec != nullptr))
    {
        return false;
    }

    // The retired wording, verbatim.
    TestFalse(TEXT("`path` is no longer described as a bare search scope"),
        Spec->Description.Contains(TEXT("Content path scope for the search")));

    TestTrue(TEXT("`path` is described as a filter over results"),
        Spec->Description.Contains(TEXT("filter"), ESearchCase::IgnoreCase));

    // The load-bearing half: narrowing `path` buys nothing on the expensive phase,
    // and the description has to say so rather than leaving it to be inferred.
    TestTrue(TEXT("`path` is described as not being a cost control"),
        Spec->Description.Contains(TEXT("cost control"), ESearchCase::IgnoreCase));
    return true;
}

// The expensive phase is opt-OUT: a caller who passes nothing gets the index wait.
// That default is deliberate — the wait is passive (the editor keeps ticking, see
// TestBlueprintSearchGameThreadLiveness.cpp) and the alternative, `autoIndex:false`
// by default, answers a cold first-of-session call with a confident empty result
// over an index nothing has kicked. What the default owes the caller instead is
// disclosure, and that disclosure lives in this description.
//
// This is a contract test, so flipping the default is *supposed* to fail it: whoever
// flips it has to come here, to the wiki page, and to the message hint together.
//
// Counterfactual: drop the wait or the bound from the description and this fails.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintSearchAutoIndexDefaultDisclosesItsCostTest,
    "PinWright.blueprint.search.AutoIndexDefaultDisclosesItsCost",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBlueprintSearchAutoIndexDefaultDisclosesItsCostTest::RunTest(const FString& Parameters)
{
    const FParamSpec* Spec = GetRegisteredParamSpec(TEXT("blueprint.search"), TEXT("autoIndex"));
    if (!TestTrue(TEXT("blueprint.search declares an autoIndex parameter"), Spec != nullptr))
    {
        return false;
    }

    TestEqual(TEXT("autoIndex is a boolean"), Spec->Type, FString(TEXT("boolean")));
    TestFalse(TEXT("autoIndex is optional"), Spec->bRequired);
    TestEqual(TEXT("autoIndex still defaults to true"), Spec->Default, FString(TEXT("true")));

    TestTrue(TEXT("the autoIndex description says a wait happens"),
        Spec->Description.Contains(TEXT("wait"), ESearchCase::IgnoreCase));
    TestTrue(TEXT("the autoIndex description names what bounds that wait"),
        Spec->Description.Contains(TEXT("timeoutSeconds")));
    return true;
}
