// Copyright (c) 2026 Alexander Penkin. MIT License.

// Tests for the shared safe-point gate (Dispatch/SafePoint.h).
//
// The defect class: an RPC handler that tears a ULevel/UWorld down, or pumps Slate
// and draws a viewport, runs on whatever game-thread stack the request landed on.
// Requests are marshalled with AsyncTask(ENamedThreads::GameThread, ...)
// (RpcDispatcher.cpp:380-385) and that queue is drained from INSIDE UWorld::Tick
// while the engine waits on tick groups, so such a handler lands mid-frame purely
// by timing. Full engine evidence is in Dispatch/SafePoint.h.
//
// level.load was gated by hand and four sibling level/lighting verbs plus the whole
// capture family were missed. These tests therefore cover the SHARED mechanism, not
// one verb: the method table, the dispatcher route that consumes it, and the
// in-handler RunAtSafePoint route. level.load's own end-to-end coverage stays in
// TestLevelLoadSafePoint.cpp.

#include "Misc/AutomationTest.h"

#include "Async/Async.h"
#include "Async/TaskGraphInterfaces.h"
#include "Containers/Ticker.h"
#include "Dom/JsonObject.h"
#include "HAL/PlatformProcess.h"
#include "Logging/LogVerbosity.h"
#include "Misc/OutputDevice.h"
#include "Misc/OutputDeviceRedirector.h"

#include "Dispatch/RpcDispatcher.h"
#include "Dispatch/SafePoint.h"
#include "Handlers/HandlerContext.h"
#include "Tests/Infra/DispatcherTestHelpers.h"
#include "Tests/TestSkipReporting.h"
#include "Tests/TestUtils.h"

namespace SafePointGateTests
{
    // The dispatcher gate is driven against the existing `_test.beta` fixture
    // (Tests/Infra/TestAutoRegistration.cpp: RPC_NO_PARAMS, responds
    // synchronously) rather than a real capture verb, which a test must never
    // actually execute. It is injected into the tick-unsafe set only for the
    // duration of the test below, so it stays an ordinary immediate handler for
    // every other caller.
    static const TCHAR* const GFixtureMethod = TEXT("_test.beta");

    // RAII for both process-global test latches, so a failing check macro that
    // aborts a test body can never leave the gate skewed for the rest of the suite.
    struct FScopedForcedUnsafe
    {
        FScopedForcedUnsafe() { PinWrightSafePoint::SetForcedUnsafeForTests(true); }
        ~FScopedForcedUnsafe() { PinWrightSafePoint::SetForcedUnsafeForTests(false); }
    };

    struct FScopedExtraTickUnsafeMethod
    {
        explicit FScopedExtraTickUnsafeMethod(const FString& Method)
        {
            PinWrightSafePoint::SetExtraTickUnsafeMethodForTests(Method);
        }
        ~FScopedExtraTickUnsafeMethod()
        {
            PinWrightSafePoint::SetExtraTickUnsafeMethodForTests(FString());
        }
    };

    // Records the LogPinWrightSafePoint lines emitted while it is in scope, so the
    // gate's own log output can be asserted on. Registered on GLog rather than
    // replacing it, so the automation framework's capture and the .log file still
    // see everything. Same shape as the capture in Tests/Transport/
    // TestModalEpisodeLogging.cpp; kept local because that one lives in a .cpp.
    class FScopedSafePointLogCapture : public FOutputDevice
    {
    public:
        FScopedSafePointLogCapture()
        {
            if (GLog)
            {
                GLog->AddOutputDevice(this);
            }
        }

        virtual ~FScopedSafePointLogCapture()
        {
            if (GLog)
            {
                GLog->RemoveOutputDevice(this);
            }
        }

        virtual void Serialize(const TCHAR* Message, ELogVerbosity::Type /*Verbosity*/,
                               const FName& Category) override
        {
            if (Category == LogPinWrightSafePoint.GetCategoryName())
            {
                Lines.Add(FString(Message));
            }
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

    // Runs Probe on the game thread from INSIDE a genuine task-graph named-thread
    // pump - the stack an editor tickable opens when it blocks on a task
    // (FTaskBase::WaitWithNamedThreadsSupport -> TryWaitOnNamedThread ->
    // ProcessThreadUntilRequestReturn, TaskPrivate.cpp:230-241 / :407-430, ending in
    // FNamedTaskThread::ProcessTasksUntilQuit at TaskGraph.cpp:685). Forcing the
    // test latch instead would prove nothing about this defect: the whole point is
    // that the REAL stack used to report safe.
    //
    // Ordering is guaranteed rather than raced. Probe is queued onto the game
    // thread's named queue before the blocker is launched, and the engine's own
    // return task cannot be queued until the blocker completes, so the pump reaches
    // Probe first.
    //
    // Returns false when the pump did not run Probe, in which case the caller must
    // skip rather than assert a conclusion it did not observe. The queued lambda is
    // drained before returning either way, so it can never outlive the caller's
    // test body.
    inline bool RunInsideNamedThreadPump(TFunction<void()> Probe)
    {
        TSharedRef<bool> bRan = MakeShared<bool>(false);
        AsyncTask(ENamedThreads::GameThread, [bRan, Probe]()
        {
            *bRan = true;
            Probe();
        });

        FGraphEventRef Blocker = FFunctionGraphTask::CreateAndDispatchWhenReady(
            []() { FPlatformProcess::Sleep(0.025f); }, TStatId{}, nullptr,
            ENamedThreads::AnyThread);
        FTaskGraphInterface::Get().WaitUntilTaskCompletes(Blocker, ENamedThreads::GameThread);

        const bool bRanInsidePump = *bRan;
        if (!bRanInsidePump)
        {
            FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);
        }
        return bRanInsidePump;
    }
}

// ============================================================================
// 1. The table
// ============================================================================

// A typo in the tick-unsafe table fails silently — the verb simply stays ungated,
// which is the exact failure mode this whole change exists to prevent. Every entry
// must name a handler that is actually registered.
//
// Counterfactual: misspell any entry in Dispatch/SafePoint.cpp and this fails
// naming it.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSafePointTickUnsafeMethodsAreRegisteredTest,
    "PinWright.core.safe_point.TickUnsafeMethodsAreRegistered",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSafePointTickUnsafeMethodsAreRegisteredTest::RunTest(const FString& Parameters)
{
    const TArray<FString>& Methods = PinWrightSafePoint::GetTickUnsafeMethods();
    TestTrue(TEXT("the tick-unsafe table is not empty"), Methods.Num() > 0);

    for (const FString& Method : Methods)
    {
        if (!IsHandlerRegistered(Method))
        {
            AddError(FString::Printf(
                TEXT("Tick-unsafe table lists '%s', but no handler is registered under that "
                     "name. The entry gates nothing — fix the spelling in "
                     "Dispatch/SafePoint.cpp or drop the entry."),
                *Method));
        }

        TestTrue(FString::Printf(TEXT("IsTickUnsafeMethod agrees with the table for '%s'"), *Method),
            PinWrightSafePoint::IsTickUnsafeMethod(Method));
    }

    // Sanity: an unrelated verb must NOT be gated, or the predicate is degenerate.
    TestFalse(TEXT("an ordinary read-only verb is not gated"),
        PinWrightSafePoint::IsTickUnsafeMethod(TEXT("actor.list")));

    return true;
}

// The four level/lighting siblings the hand-written level.load gate missed, plus the
// capture verbs visual verification calls constantly. Pinned by name so a later
// refactor cannot quietly drop one back out of the gate.
//
// Counterfactual: delete any listed entry from Dispatch/SafePoint.cpp and this fails.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSafePointKnownVictimsAreGatedTest,
    "PinWright.core.safe_point.KnownVictimsAreGated",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSafePointKnownVictimsAreGatedTest::RunTest(const FString& Parameters)
{
    const TArray<FString> MustBeGated = {
        // Level / world teardown -> CollectGarbage -> ~ULevel.
        TEXT("level.create"),
        TEXT("level.remove_from_world"),
        TEXT("level.structure.create_level"),
        TEXT("lighting.create_lighting_enabled_level"),
        // Re-entrant Slate tick / viewport draw / render flush.
        TEXT("render.capture_open_level"),
        TEXT("render.capture_asset_preview"),
        TEXT("render.capture_annotated"),
        TEXT("render.capture_animation_preview"),
        TEXT("render.detect_z_fighting"),
        TEXT("render.capture_ortho_tiles"),
        TEXT("camera.frame_actor"),
        TEXT("camera.orbit_shots"),
        TEXT("camera.animation_shots"),
        TEXT("editor.screenshot"),
        TEXT("widget.screenshot_designer"),
        TEXT("ui.screenshot"),
        // Blueprint creators call CreatePackage and synchronously save the new asset.
        TEXT("blueprint.create"),
        TEXT("widget.create_widget_blueprint"),
        TEXT("editor.create_utility_widget"),
        TEXT("pcg.create_graph"),
        TEXT("physics.setup_physics_simulation"),
        TEXT("skeleton.create_physics_asset"),
        TEXT("asset.save"),
        TEXT("audio.authoring.set_sound_wave_properties"),
        TEXT("data_table.add_row"),
        TEXT("data_table.remove_row"),
        TEXT("data_table.set_row"),
        TEXT("data_table.set_row_struct"),
        TEXT("eqs.set_context_class"),
        TEXT("asset.reset_instance_parameters"),
        TEXT("asset.set_metadata"),
        TEXT("sequencer.bake_control_space"),
        // The arbitrary-payload entry points. Listed for the opposite reason to
        // everything above: not because their bodies are known to reach a hazard,
        // but because they can reach ANY of them (`open <map>` is the whole level
        // teardown chain) and the table cannot classify a payload in advance. They
        // shipped ungated on the argument that a name table cannot decide for
        // them; the safe default for "could be anything" is to assume the worst.
        // Dropping one back out re-opens the gap, so they are pinned by name.
        TEXT("python.execute"),
        TEXT("system.console_command"),
        TEXT("editor.console_command"),
        // StaticMesh asset creation + a synchronous mesh build on the handler's own
        // stack (family H). Pinned because it is the first geometry entry and the
        // one an "it already runs on the game thread" reading would drop: the game
        // thread is not the question, the position inside the frame is.
        TEXT("model.compile"),
        // StaticMesh rebuilds release render data and the shared guard flushes/recreates
        // live consumers around the mutation.
        TEXT("asset.generate_lods"),
        TEXT("asset.nanite_rebuild_mesh"),
        TEXT("render.nanite_rebuild_mesh"),
        TEXT("static_mesh.bake_transform"),
        TEXT("geometry.set_lod_settings"),
        // Package eviction + whole-object-graph reference fixup (family J): two
        // CollectGarbage passes, an editor-wide FGlobalComponentReregisterContext, and a
        // raw UObject* snapshot of the entire object graph serialised entry by entry.
        // Pinned because it took a shared editor down while sitting OFF this table
        // entirely (board B-asset-reload-blueprint-package-crash), and because the entry
        // is only effective while the handler stays synchronous — see the counterfactual
        // in Tests/Assets/TestAssetReloadHandler.cpp.
        TEXT("asset.reload"),
    };

    for (const FString& Method : MustBeGated)
    {
        TestTrue(FString::Printf(
                     TEXT("'%s' is gated against running inside UWorld::Tick"), *Method),
            PinWrightSafePoint::IsTickUnsafeMethod(Method));
    }

    // level.load is deliberately absent: it gates in-handler because
    // editor.open_level / editor.open_asset reach it through
    // FRpcDispatcher::DispatchMethod, which bypasses the dispatcher gate entirely.
    // Listing it here as well would give one verb two gates.
    TestFalse(TEXT("level.load is NOT in the table (it gates in-handler instead)"),
        PinWrightSafePoint::IsTickUnsafeMethod(TEXT("level.load")));

    return true;
}

// ============================================================================
// 2. The dispatcher route — the valuable test
// ============================================================================

// Drives the REAL FRpcDispatcher::ProcessRequest with the safe point forced unsafe
// (standing in for "a world is inside UWorld::Tick") and asserts the request did
// NOT execute on the caller's stack. Then clears the override — the world tick has
// ended — and drains the queue exactly as UPinWrightSubsystem::Tick does from the
// core ticker (PinWrightSubsystem.cpp:303), and asserts the handler ran there.
//
// Counterfactual: remove the IsTickUnsafeMethod/IsSafeNow branch from
// FRpcDispatcher::ProcessRequest and "did not respond on the unsafe stack" fails
// immediately, because the fixture handler responds synchronously.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSafePointDispatcherDefersTickUnsafeMethodTest,
    "PinWright.core.safe_point.DispatcherDefersTickUnsafeMethod",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSafePointDispatcherDefersTickUnsafeMethodTest::RunTest(const FString& Parameters)
{
    using namespace SafePointGateTests;

    DispatcherTestHelpers::FSinkPtr Sink;
    FRpcDispatcher Dispatcher;
    DispatcherTestHelpers::MakeDispatcher(Sink, Dispatcher);

    const FString Method(GFixtureMethod);
    FScopedExtraTickUnsafeMethod GatedFixture(Method);

    TestTrue(TEXT("the fixture method is gated for the duration of this test"),
        PinWrightSafePoint::IsTickUnsafeMethod(Method));

    {
        FScopedForcedUnsafe ForcedUnsafe;

        TestFalse(TEXT("the override makes IsSafeNow() report unsafe"),
            PinWrightSafePoint::IsSafeNow());

        Dispatcher.ProcessRequest(TEXT("req-safe-point-defer"), Method, MakeShared<FJsonObject>());

        // THE COUNTERFACTUAL. Ungated, this is already true: the fixture handler
        // responds synchronously inside ProcessRequest.
        TestFalse(TEXT("a gated method does not run on a stack inside UWorld::Tick"),
            Sink->bWasCalled);
    }

    // The world tick has ended. This is what UPinWrightSubsystem::Tick does from the
    // core ticker, and the core ticker is pumped by FEngineLoop::Tick only after
    // GEngine->Tick() (and therefore UWorld::Tick) has returned.
    TestTrue(TEXT("the safe point reports safe once nothing is ticking"),
        PinWrightSafePoint::IsSafeNow());
    Dispatcher.ProcessPendingRequests();

    TestTrue(TEXT("the deferred request ran at the safe point"), Sink->bWasCalled);
    TestTrue(TEXT("the deferred request succeeded"), Sink->bSuccess);

    return true;
}

// An ungated method must be completely untouched by the gate — no added latency, no
// queue hop — otherwise the fix taxes all ~1,170 verbs to protect twenty.
//
// Counterfactual: drop the IsTickUnsafeMethod() half of the branch (defer whenever
// unsafe) and this fails.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSafePointDispatcherRunsUngatedMethodInlineTest,
    "PinWright.core.safe_point.DispatcherRunsUngatedMethodInline",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSafePointDispatcherRunsUngatedMethodInlineTest::RunTest(const FString& Parameters)
{
    using namespace SafePointGateTests;

    DispatcherTestHelpers::FSinkPtr Sink;
    FRpcDispatcher Dispatcher;
    DispatcherTestHelpers::MakeDispatcher(Sink, Dispatcher);

    const FString Method(GFixtureMethod);
    TestFalse(TEXT("the fixture is not gated by default"),
        PinWrightSafePoint::IsTickUnsafeMethod(Method));

    {
        // Unsafe stack, but the method is not in the table: it must still run inline.
        FScopedForcedUnsafe ForcedUnsafe;
        Dispatcher.ProcessRequest(TEXT("req-safe-point-inline"), Method,
            MakeShared<FJsonObject>());
    }

    TestTrue(TEXT("an ungated method runs inline even on an unsafe stack"),
        Sink->bWasCalled);
    TestTrue(TEXT("and it succeeded"), Sink->bSuccess);
    return true;
}

// The inline half of the gate must leave a trace, not only the deferral half. The
// asymmetry is silent exactly where it is most expensive: after an editor death a
// log can prove a deferral happened but cannot separate "the gate allowed this"
// from "the gate never fired". That matters most on a host where the two nested
// named-thread pump tests skip (reason=nested-named-thread-pump-not-entered), which
// leaves the log as the only available evidence that the widened gate behaves.
//
// This runs on a real safe stack rather than a forced one - the automation stack is
// already outside UWorld::Tick and outside any named-thread pump, which is what
// SafePointReportsSafeOffTick asserts independently.
//
// Counterfactual: delete the UE_LOG placed after the deferral block in
// RpcDispatcher.cpp and both line assertions fail.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSafePointDispatcherLogsInlineTickUnsafeRunTest,
    "PinWright.core.safe_point.DispatcherLogsInlineTickUnsafeRun",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSafePointDispatcherLogsInlineTickUnsafeRunTest::RunTest(const FString& Parameters)
{
    using namespace SafePointGateTests;

    DispatcherTestHelpers::FSinkPtr Sink;
    FRpcDispatcher Dispatcher;
    DispatcherTestHelpers::MakeDispatcher(Sink, Dispatcher);

    const FString Method(GFixtureMethod);
    const FString RequestId(TEXT("req-safe-point-inline-log"));
    TestTrue(TEXT("an automation stack is a safe point"), PinWrightSafePoint::IsSafeNow());

    FScopedSafePointLogCapture Capture;
    {
        // Gated method, safe stack: the deferral branch is skipped and the request
        // runs inline, which is the branch that used to log nothing at all.
        FScopedExtraTickUnsafeMethod GatedFixture(Method);
        Dispatcher.ProcessRequest(RequestId, Method, MakeShared<FJsonObject>());
    }

    TestTrue(TEXT("a gated method still runs inline on a safe stack"), Sink->bWasCalled);
    TestTrue(TEXT("and it succeeded"), Sink->bSuccess);
    TestEqual(TEXT("the inline run logged exactly one line naming the request"),
        Capture.CountContaining(*RequestId), 1);
    TestEqual(TEXT("and that line names both terms of the gate that let it through"),
        Capture.CountContaining(
            TEXT("no world is inside UWorld::Tick and the game thread is not draining "
                 "a task-graph named-thread queue")),
        1);
    return true;
}

// The same assertion for the real entry points, which the fixture-driven test
// above deliberately stands in for. This one drives FRpcDispatcher::ProcessRequest
// with the ACTUAL registered `python.execute` / `system.console_command` /
// `editor.console_command` names on a stack that reports unsafe, and asserts
// nothing reached a handler.
//
// It never drains the queue afterwards. Draining would execute a real console
// command or a real Python interpreter call, which a test must not do; the
// locally-scoped dispatcher is destroyed with the request still parked, which is
// exactly the observation we want. That the parked request DOES run on the next
// drain is proved separately, safely, by DispatcherDefersTickUnsafeMethod above.
//
// The registration check is what makes "the sink was never called" mean
// "deferred" rather than "the method does not exist": the gate returns before the
// handler lookup, so an unregistered name would also produce silence. Both halves
// together are the proof.
//
// Counterfactual: drop any of the three from Dispatch/SafePoint.cpp and the
// corresponding "did not run on an unsafe stack" assertion fails immediately,
// because all three handlers respond synchronously.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSafePointDefersArbitraryPayloadEntryPointsTest,
    "PinWright.core.safe_point.DefersArbitraryPayloadEntryPoints",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSafePointDefersArbitraryPayloadEntryPointsTest::RunTest(const FString& Parameters)
{
    using namespace SafePointGateTests;

    const TArray<FString> EntryPoints = {
        TEXT("python.execute"),
        TEXT("system.console_command"),
        TEXT("editor.console_command"),
    };

    for (const FString& Method : EntryPoints)
    {
        TestTrue(FString::Printf(TEXT("'%s' is a registered handler"), *Method),
            IsHandlerRegistered(Method));

        DispatcherTestHelpers::FSinkPtr Sink;
        FRpcDispatcher Dispatcher;
        DispatcherTestHelpers::MakeDispatcher(Sink, Dispatcher);

        // A payload the handler would reject anyway if it ever ran, so a
        // regression that lets one through cannot also have a side effect: no
        // `command`, no `code`. The gate fires before any of that is looked at.
        FScopedForcedUnsafe ForcedUnsafe;
        Dispatcher.ProcessRequest(FString::Printf(TEXT("req-entry-%s"), *Method), Method,
            MakeShared<FJsonObject>());

        TestFalse(FString::Printf(
                      TEXT("'%s' did not reach a handler on a stack inside UWorld::Tick"),
                      *Method),
            Sink->bWasCalled);
    }

    return true;
}

// The same end-to-end assertion for family H, the first geometry entry in the table.
//
// model.compile is the entry an "it already runs on the game thread" reading would
// drop: FRpcDispatcher::ProcessRequest marshals every off-thread request to the game
// thread, so the verb is never off it — what the gate decides is WHERE IN THE FRAME
// it lands, and the hazard is a UStaticMesh build (render fences, FlushRenderingCommands)
// plus a package create and a synchronous .uasset save on the handler's own stack.
//
// The payload is empty on purpose. If the gate ever stops firing, the handler runs,
// the dispatcher's required-parameter check rejects the call, and the sink IS called —
// so the assertion below fails loudly without a compile ever being attempted. Nothing
// here can create an asset even on the regression path.
//
// Counterfactual: drop TEXT("model.compile") from Dispatch/SafePoint.cpp and this fails.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSafePointDefersModelCompileTest,
    "PinWright.core.safe_point.DefersModelCompile",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSafePointDefersModelCompileTest::RunTest(const FString& Parameters)
{
    using namespace SafePointGateTests;

    // Registration is half the proof: the gate returns before the handler lookup, so
    // an unregistered name would produce the same silence as a deferred one.
    TestTrue(TEXT("'model.compile' is a registered handler"),
        IsHandlerRegistered(TEXT("model.compile")));

    DispatcherTestHelpers::FSinkPtr Sink;
    FRpcDispatcher Dispatcher;
    DispatcherTestHelpers::MakeDispatcher(Sink, Dispatcher);

    FScopedForcedUnsafe ForcedUnsafe;
    Dispatcher.ProcessRequest(TEXT("req-model-compile"), TEXT("model.compile"),
        MakeShared<FJsonObject>());

    TestFalse(TEXT("'model.compile' did not reach a handler on a stack inside UWorld::Tick"),
        Sink->bWasCalled);

    return true;
}

// Family I: the three verbs that complete an edit to a master UMaterial. Each runs
// PinWright::MaterialConsumers::NotifyMasterMaterialChanged, whose bare
// FMaterialUpdateContext takes EOptions::Default = RecreateRenderStates |
// SyncWithRenderingThread and therefore calls FlushRenderingCommands in both its
// constructor and its destructor (MaterialShared.cpp:5026, :5078), then rebuilds every
// consuming landscape's per-component MICs. Two editor kills 90 seconds apart faulted on
// the RENDER thread in BeginReleaseResource with the game thread parked in a render fence
// below PinWright frames, after a storm of "FlushRenderingCommands called recursively"
// (board: B-compile-material-not-tick-gated).
//
// BOTH branches are asserted, because gating a verb changes when its response is
// delivered and "it defers" alone would be satisfied by a verb that never answers:
//   - unsafe stack -> nothing reaches a handler;
//   - real safe stack -> the request is admitted and answered.
//
// The payload is empty on purpose, and the inline half is what makes that safe rather
// than merely convenient: FRpcDispatcher::ValidateHandlerParams rejects a missing
// required parameter BEFORE the handler body runs, so the admitted request answers
// MISSING_REQUIRED_PARAM without loading a UMaterial, opening an update context, or
// compiling a single shader. Asserting that exact error code is therefore also the proof
// that the inline branch stopped where this test intends it to.
//
// Counterfactual: drop any of the three from Dispatch/SafePoint.cpp and its "did not
// reach a handler on an unsafe stack" assertion fails immediately, because the parameter
// rejection answers synchronously inside ProcessRequest.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSafePointDefersMasterMaterialEditVerbsTest,
    "PinWright.core.safe_point.DefersMasterMaterialEditVerbs",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSafePointDefersMasterMaterialEditVerbsTest::RunTest(const FString& Parameters)
{
    using namespace SafePointGateTests;

    const TArray<FString> MasterMaterialEditVerbs = {
        TEXT("material.authoring.compile_material"),
        TEXT("material.authoring.configure_layer_blend"),
        TEXT("material.compile_mgir"),
    };

    for (const FString& Method : MasterMaterialEditVerbs)
    {
        // Registration is half the proof of the deferral: the gate returns before the
        // handler lookup, so an unregistered name would produce the same silence.
        TestTrue(FString::Printf(TEXT("'%s' is a registered handler"), *Method),
            IsHandlerRegistered(Method));

        TestTrue(FString::Printf(
                     TEXT("'%s' is gated against running inside the engine's frame"), *Method),
            PinWrightSafePoint::IsTickUnsafeMethod(Method));

        // A dispatcher per branch, not one shared between them. ProcessRequest drains
        // PendingQueue from its own ON_SCOPE_EXIT when anything was parked, so a second
        // call on the same dispatcher would run the parked request as a side effect and
        // the inline assertion below could no longer say which call it measured. The
        // deferred one is left parked and destroyed with its dispatcher, exactly as
        // DefersArbitraryPayloadEntryPoints does.
        {
            DispatcherTestHelpers::FSinkPtr Sink;
            FRpcDispatcher Dispatcher;
            DispatcherTestHelpers::MakeDispatcher(Sink, Dispatcher);

            FScopedForcedUnsafe ForcedUnsafe;
            Dispatcher.ProcessRequest(FString::Printf(TEXT("req-material-unsafe-%s"), *Method),
                Method, MakeShared<FJsonObject>());

            TestFalse(FString::Printf(
                          TEXT("'%s' did not reach a handler on an unsafe stack"), *Method),
                Sink->bWasCalled);
        }

        // The other half, on the REAL automation stack rather than a forced one: the gate
        // must admit the verb where it is legal, or "gated" would just mean "broken".
        {
            DispatcherTestHelpers::FSinkPtr Sink;
            FRpcDispatcher Dispatcher;
            DispatcherTestHelpers::MakeDispatcher(Sink, Dispatcher);

            TestTrue(TEXT("an automation stack is a safe point"),
                PinWrightSafePoint::IsSafeNow());
            Dispatcher.ProcessRequest(FString::Printf(TEXT("req-material-safe-%s"), *Method),
                Method, MakeShared<FJsonObject>());

            TestTrue(FString::Printf(TEXT("'%s' runs inline on a safe stack"), *Method),
                Sink->bWasCalled);
            TestEqual(FString::Printf(
                          TEXT("'%s' stopped at parameter validation, so no material work ran"),
                          *Method),
                Sink->ErrorCode, FString(TEXT("MISSING_REQUIRED_PARAM")));
        }
    }

    return true;
}

// ============================================================================
// 3. The in-handler route (RunAtSafePoint + FSafePointResponder)
// ============================================================================

// The split exists because FHandlerContext::MakeAsyncToken deliberately drops the
// raw ResponseCapture pointer (HandlerContext.cpp:532-538). If RunAtSafePoint
// answered the inline path through a token, every synchronous capture — the
// dispatcher's text-formatter path and every raw-pointer test fixture — would
// silently stop receiving responses.
//
// Counterfactual: make RunAtSafePoint always use Ctx.MakeAsyncToken() and the
// inline assertion below fails (the raw-pointer capture never fires).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSafePointRunAtSafePointInlineUsesContextTest,
    "PinWright.core.safe_point.RunAtSafePointInlineUsesContext",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSafePointRunAtSafePointInlineUsesContextTest::RunTest(const FString& Parameters)
{
    FTestResponseCapture Capture;
    FHandlerContext Ctx = FHandlerContext::MakeTestContextWithCapture(
        TEXT("test-id"), TEXT("_test.run_at_safe_point"), MakeShared<FJsonObject>(), &Capture);

    TestTrue(TEXT("an automation stack is a safe point"), PinWrightSafePoint::IsSafeNow());

    bool bWorkRan = false;
    bool bReportedDeferred = true;
    const bool bReturned = PinWrightSafePoint::RunAtSafePoint(Ctx, TEXT("unit test"),
        [&bWorkRan, &bReportedDeferred](const PinWrightSafePoint::FSafePointResponder& Responder)
        {
            bWorkRan = true;
            bReportedDeferred = Responder.IsDeferred();
            TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
            Result->SetBoolField(TEXT("ok"), true);
            Responder.SendSuccess(Result);
        });

    TestTrue(TEXT("RunAtSafePoint returns true so a handler can tail-call it"), bReturned);
    TestTrue(TEXT("work ran on the caller's stack"), bWorkRan);
    TestFalse(TEXT("the responder reports the inline branch"), bReportedDeferred);
    TestTrue(TEXT("the RAW-pointer capture received the inline response"), Capture.bWasCalled);
    TestTrue(TEXT("the inline response was a success"), Capture.bSuccess);
    return true;
}

// The deferred half: nothing runs on the caller's stack, and one core-ticker pass
// later the work runs and answers through the async token.
//
// Counterfactual: make RunAtSafePoint call Work() unconditionally and "work has not
// run yet" fails.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSafePointRunAtSafePointDefersWhenUnsafeTest,
    "PinWright.core.safe_point.RunAtSafePointDefersWhenUnsafe",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSafePointRunAtSafePointDefersWhenUnsafeTest::RunTest(const FString& Parameters)
{
    using namespace SafePointGateTests;

    // Shared-owned: the async token holds a WEAK handle to it, and MakeAsyncToken
    // only forwards a capture that is shared-owned.
    TSharedRef<FTestResponseCapture> Capture = MakeShared<FTestResponseCapture>();
    FHandlerContext Ctx = FHandlerContext::MakeTestContextWithSharedCapture(
        TEXT("test-id"), TEXT("_test.run_at_safe_point"), MakeShared<FJsonObject>(), Capture);

    TSharedRef<int32> RunCount = MakeShared<int32>(0);
    TSharedRef<bool> ReportedDeferred = MakeShared<bool>(false);

    {
        FScopedForcedUnsafe ForcedUnsafe;

        PinWrightSafePoint::RunAtSafePoint(Ctx, TEXT("unit test"),
            [RunCount, ReportedDeferred](const PinWrightSafePoint::FSafePointResponder& Responder)
            {
                ++(*RunCount);
                *ReportedDeferred = Responder.IsDeferred();
                TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
                Result->SetBoolField(TEXT("ok"), true);
                Responder.SendSuccess(Result);
            });

        TestEqual(TEXT("work has not run yet on the caller's stack"), *RunCount, 0);
        TestFalse(TEXT("nothing responded on the caller's stack"), Capture->bWasCalled);
    }

    // One core-ticker pass is all the deferral costs.
    FTSTicker::GetCoreTicker().Tick(0.0f);

    TestEqual(TEXT("work ran after one core-ticker pass"), *RunCount, 1);
    TestTrue(TEXT("the responder reports the deferred branch"), *ReportedDeferred);
    TestTrue(TEXT("the shared capture received the deferred response"), Capture->bWasCalled);
    TestTrue(TEXT("the deferred response was a success"), Capture->bSuccess);

    // One-shot: the ticker delegate removed itself.
    FTSTicker::GetCoreTicker().Tick(0.0f);
    TestEqual(TEXT("deferred work runs exactly once"), *RunCount, 1);
    return true;
}

// ============================================================================
// 4. The nested named-thread pump — the stack bInTick cannot see
// ============================================================================

// The gate's second half. Board:
// B-safepoint-tick-gate-inert-on-simpletickobjects-path.
//
// An editor tickable that blocks on a task pumps the game thread's own named-thread
// queue from FTickableEditorObject::TickObjects (EditorEngine.cpp:1933), which runs
// BEFORE the editor world tick at :1967. A PinWright request marshalled with
// AsyncTask(ENamedThreads::GameThread, ...) therefore executes inside the engine
// frame with NO world reporting bInTick — so the bInTick-only gate reported "safe"
// on the one stack it exists to refuse, for all 35 listed verbs.
//
// This test enters that pump for real and asks the gate what it thinks.
//
// Counterfactual: revert IsSafeNow() to `!ForcedUnsafeForTests() &&
// !IsAnyWorldTicking()` and the last assertion fails — the probe observes
// IsSafeNow() == true while running inside the pump. Note that the "no world is
// ticking" assertion PASSES both before and after the fix: it is the evidence that
// bInTick genuinely cannot see this window, not the regression check.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSafePointNestedNamedThreadPumpIsUnsafeTest,
    "PinWright.core.safe_point.NestedNamedThreadPumpIsUnsafe",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSafePointNestedNamedThreadPumpIsUnsafeTest::RunTest(const FString& Parameters)
{
    using namespace SafePointGateTests;

    // The automation controller is ticked directly by FEngineLoop::Tick
    // (LaunchEngineLoop.cpp:6031), not out of a task queue, so this stack must stay
    // safe — the whole inline path depends on it.
    TestFalse(TEXT("an automation stack is not inside a named-thread pump"),
        PinWrightSafePoint::IsInsideNamedThreadPump());
    TestTrue(TEXT("an automation stack is still a safe point"),
        PinWrightSafePoint::IsSafeNow());

    bool bObservedInPump = false;
    bool bObservedWorldTicking = true;
    bool bObservedSafe = true;
    const bool bRanInsidePump = RunInsideNamedThreadPump(
        [&bObservedInPump, &bObservedWorldTicking, &bObservedSafe]()
        {
            bObservedInPump = PinWrightSafePoint::IsInsideNamedThreadPump();
            bObservedWorldTicking = PinWrightSafePoint::IsAnyWorldTicking();
            bObservedSafe = PinWrightSafePoint::IsSafeNow();
        });

    if (!bRanInsidePump)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("nested-named-thread-pump-not-entered"),
            TEXT("the task graph did not run the probe inside a nested named-thread pump, so "
                 "IsSafeNow() could not be observed on that stack."));
        return true;
    }

    TestTrue(TEXT("the probe ran inside a named-thread pump"), bObservedInPump);

    // The measured shape of the defect: this stack is inside the engine frame and
    // bInTick is false for every world, so the old gate had nothing to fire on.
    TestFalse(TEXT("no world reports bInTick inside the nested pump"),
        bObservedWorldTicking);

    // THE COUNTERFACTUAL.
    TestFalse(TEXT("IsSafeNow() reports unsafe inside a nested named-thread pump"),
        bObservedSafe);

    return true;
}

// The same window, end to end through the REAL FRpcDispatcher::ProcessRequest: a
// gated method that arrives on the nested-pump stack must not reach its handler
// there, and must run on the next safe point instead.
//
// Driven against the `_test.beta` fixture (Tests/Infra/TestAutoRegistration.cpp,
// responds synchronously) injected into the tick-unsafe set for the duration of the
// test, exactly as DispatcherDefersTickUnsafeMethod above does — a test must never
// execute a real capture or map-swap verb.
//
// Counterfactual: revert IsSafeNow() to the bInTick-only form and "did not run
// inside the nested pump" fails immediately, because the fixture handler responds
// on the caller's stack. That is the pre-fix behaviour for all 35 table entries
// whenever an editor tickable happens to open the pump that frame.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSafePointDispatcherDefersFromNestedPumpTest,
    "PinWright.core.safe_point.DispatcherDefersFromNestedNamedThreadPump",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSafePointDispatcherDefersFromNestedPumpTest::RunTest(const FString& Parameters)
{
    using namespace SafePointGateTests;

    // Shared-owned so the queued lambda can never dangle, whichever pump runs it.
    TSharedRef<FRpcDispatcher> Dispatcher = MakeShared<FRpcDispatcher>();
    DispatcherTestHelpers::FSinkPtr Sink;
    DispatcherTestHelpers::MakeDispatcher(Sink, Dispatcher.Get());

    const FString Method(GFixtureMethod);
    FScopedExtraTickUnsafeMethod GatedFixture(Method);

    TestTrue(TEXT("the fixture method is gated for the duration of this test"),
        PinWrightSafePoint::IsTickUnsafeMethod(Method));

    const bool bRanInsidePump = RunInsideNamedThreadPump([Dispatcher, Method]()
    {
        Dispatcher->ProcessRequest(TEXT("req-safe-point-nested-pump"), Method,
            MakeShared<FJsonObject>());
    });

    if (!bRanInsidePump)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("nested-named-thread-pump-not-entered"),
            TEXT("the task graph did not run the request inside a nested named-thread pump, so "
                 "the dispatcher gate could not be observed on that stack."));
        return true;
    }

    // THE COUNTERFACTUAL.
    TestFalse(TEXT("a gated method did not run inside the nested named-thread pump"),
        Sink->bWasCalled);

    // Back on the automation stack, which is the same kind of stack the core ticker
    // drains from: no world ticking, no queue being pumped.
    TestTrue(TEXT("the automation stack is a safe point"), PinWrightSafePoint::IsSafeNow());
    Dispatcher->ProcessPendingRequests();

    TestTrue(TEXT("the deferred request ran at the safe point"), Sink->bWasCalled);
    TestTrue(TEXT("the deferred request succeeded"), Sink->bSuccess);

    return true;
}
