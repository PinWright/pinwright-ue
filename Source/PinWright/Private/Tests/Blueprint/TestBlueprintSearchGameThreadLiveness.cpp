// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression tests for the blueprint.search game-thread liveness fix
// (board: B-blueprint-search-wedges-game-thread).
//
// The defect: the handler did all of its work on the RPC stack, which
// FRpcDispatcher::ProcessRequest marshals onto the game thread. Three separate
// blocking sites, all unbounded in the size of the content tree:
//
//   1. `while (FiB.IsCacheInProgress()) { FiB.Tick(0.0f); Sleep(0.001f); }` -
//      the editor's own frame loop with the frame taken out. Measured 30.9 s on a
//      large project, and its 60 s bound was unenforceable because elapsed time was
//      only sampled between Tick() calls and one Tick() can load a level.
//   2. `while (!Search->IsComplete()) Sleep(0.01f)` for up to timeoutSeconds.
//   3. FFindInBlueprintsResult::GetParentBlueprint() per matched root, which is
//      LoadObject underneath - it mass-loaded levels and built a multi-GB static
//      mesh in the reported incident.
//
// Reported outcome: the game thread had not ticked for 136 s, the caller's MCP
// stream dropped at its 120 s deadline, and the editor died at ~7 minutes.
//
// The fix: the scope check answers inline, everything after it runs from
// FTSTicker::GetCoreTicker() and answers through an FAsyncResponseToken, and result
// resolution is two AssetRegistry hash lookups instead of a load.

#include "Misc/AutomationTest.h"

#include "Containers/Ticker.h"
#include "Dom/JsonObject.h"
#include "HAL/PlatformTime.h"

#include "AssetRegistry/ARFilter.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Tests/TestUtils.h"

namespace
{
    /** True when the host project has at least one Blueprint under /Game. */
    bool HostHasBlueprintsUnderGame()
    {
        FAssetRegistryModule& AssetRegistryModule =
            FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));

        FARFilter Filter;
        Filter.PackagePaths.Add(FName(TEXT("/Game")));
        Filter.bRecursivePaths = true;
        Filter.ClassPaths.Add(FTopLevelAssetPath(TEXT("/Script/Engine"), TEXT("Blueprint")));

        TArray<FAssetData> Blueprints;
        AssetRegistryModule.Get().GetAssets(Filter, Blueprints);
        return Blueprints.Num() > 0;
    }
}

// ============================================================================
// 1. The searched path answers OFF the RPC stack
// ============================================================================

// The whole ticket in one assertion: when there is something to search, the handler
// must RETURN WITHOUT HAVING ANSWERED. A response that is already in the capture by
// the time the handler returns can only have been produced by work done on the
// caller's stack - which is the game thread - and that is exactly the wedge.
//
// Counterfactual: restore either blocking wait (the FiB index pump or the
// `while (!Search->IsComplete())` spin) and the capture IS populated on return, so
// `bWasCalled == false` fails. The elapsed-time check is a second, coarser net for
// the same thing: it is a "did not wait for the corpus" gate, not a latency budget.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintSearchAnswersOffTheRpcStackTest,
    "PinWright.blueprint.search.AnswersOffTheRpcStack",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBlueprintSearchAnswersOffTheRpcStackTest::RunTest(const FString& Parameters)
{
    if (!HostHasBlueprintsUnderGame())
    {
        AddWarning(TEXT("PINWRIGHT_ASSERTIONS_SKIPPED: this host project has no Blueprint under "
                        "/Game, so blueprint.search takes its empty-scope early-out and the "
                        "off-stack path cannot be driven."));
        return true;
    }

    const TSharedRef<FTestResponseCapture> Capture = MakeShared<FTestResponseCapture>();

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    // A token no Blueprint can contain, so the worker walks the corpus and matches
    // nothing. The subject here is liveness and disclosure, not recall.
    Payload->SetStringField(TEXT("query"), TEXT("PinWrightLivenessProbeToken"));
    Payload->SetStringField(TEXT("path"), TEXT("/Game"));
    // autoIndex=false keeps this test off the index phase, which is timing-dependent
    // on a cold editor; the index phase has the same non-blocking shape.
    Payload->SetBoolField(TEXT("autoIndex"), false);
    Payload->SetNumberField(TEXT("timeoutSeconds"), 5.0);
    Payload->SetNumberField(TEXT("limit"), 5);

    const double InvokeStart = FPlatformTime::Seconds();
    const bool bFound =
        InvokeHandlerWithSharedCapture(TEXT("blueprint.search"), Payload, Capture);
    const double InvokeSeconds = FPlatformTime::Seconds() - InvokeStart;

    if (!TestTrue(TEXT("blueprint.search is registered"), bFound))
    {
        return false;
    }

    TestFalse(TEXT("blueprint.search returned WITHOUT answering — the search runs off the RPC "
                   "stack, not on the game thread"),
        Capture->bWasCalled);
    TestTrue(FString::Printf(
                 TEXT("blueprint.search returned promptly (%.3fs) instead of waiting out the "
                      "corpus walk"),
                 InvokeSeconds),
        InvokeSeconds < 2.0);

    // Generous relative to the 5s budget the payload asked for: the point is that the
    // run terminates on its own bound, not how close to it the ticker lands.
    PumpUntilCaptured(*Capture, 60.0);

    TestTrue(TEXT("the run answered from the core ticker"), Capture->bWasCalled);
    TestTrue(TEXT("the run answered with success"), Capture->bSuccess);

    if (!Capture->Result.IsValid())
    {
        AddError(TEXT("blueprint.search produced no result object."));
        return false;
    }

    // Both disclosure flags are unconditional, so a caller never has to tell "field
    // absent" from "field false" when deciding whether it holds a complete answer.
    bool bTimedOut = false;
    TestTrue(TEXT("the response carries timedOut"),
        Capture->Result->TryGetBoolField(TEXT("timedOut"), bTimedOut));

    bool bSearchRan = false;
    TestTrue(TEXT("the response carries searchRan"),
        Capture->Result->TryGetBoolField(TEXT("searchRan"), bSearchRan));
    TestTrue(TEXT("a search over a non-empty scope actually ran a worker"), bSearchRan);

    bool bIndexInProgress = true;
    TestTrue(TEXT("the response carries indexInProgress"),
        Capture->Result->TryGetBoolField(TEXT("indexInProgress"), bIndexInProgress));

    // Let a detached worker (timeout path) finish being joined by its own ticker
    // delegate rather than by this scope dropping the last reference.
    FTSTicker::GetCoreTicker().Tick(0.01f);
    FTSTicker::GetCoreTicker().Tick(0.01f);
    return true;
}

// ============================================================================
// 2. An empty scope is decided BEFORE any Find-in-Blueprints work
// ============================================================================

// The auto-index pump used to run first, so `autoIndex:true` paid for a whole
// project index even when the requested path could not contain a single match. The
// scope query is cheap and decisive, so it goes first: nothing that cannot change
// the answer may cost the game thread anything.
//
// Counterfactual: move the index kick back above the AssetRegistry scope query and
// this call pays the pump on a cold editor (30.9 s measured; the old bound was 60 s)
// and blows the budget below. The bound is a "did not pump the index" gate, not a
// latency budget.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintSearchEmptyScopeSkipsIndexWorkTest,
    "PinWright.blueprint.search.EmptyScopeSkipsIndexWork",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBlueprintSearchEmptyScopeSkipsIndexWorkTest::RunTest(const FString& Parameters)
{
    FTestResponseCapture Capture;

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("query"), TEXT("PinWrightEmptyScopeProbe"));
    // A path no project mounts content under, so the scope is empty by construction.
    Payload->SetStringField(TEXT("path"), TEXT("/Game/__PinWrightNoSuchPath__"));
    // The whole point: indexing is REQUESTED and must still not be paid for.
    Payload->SetBoolField(TEXT("autoIndex"), true);

    const double InvokeStart = FPlatformTime::Seconds();
    const bool bFound = InvokeHandlerWithCapture(TEXT("blueprint.search"), Payload, Capture);
    const double InvokeSeconds = FPlatformTime::Seconds() - InvokeStart;

    if (!TestTrue(TEXT("blueprint.search is registered"), bFound))
    {
        return false;
    }

    TestTrue(TEXT("an empty scope is answered inline, with no worker and no ticker"),
        Capture.bWasCalled);
    TestTrue(TEXT("the empty-scope answer is a success"), Capture.bSuccess);
    TestTrue(FString::Printf(
                 TEXT("an empty scope did not pay for the Find-in-Blueprints index (%.3fs)"),
                 InvokeSeconds),
        InvokeSeconds < 5.0);

    if (!Capture.Result.IsValid())
    {
        AddError(TEXT("blueprint.search produced no result object."));
        return false;
    }

    bool bIndexInProgress = true;
    TestTrue(TEXT("the empty-scope response carries indexInProgress, like the searched path"),
        Capture.Result->TryGetBoolField(TEXT("indexInProgress"), bIndexInProgress));
    return true;
}
