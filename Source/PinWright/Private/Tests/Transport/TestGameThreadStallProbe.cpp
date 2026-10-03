// Copyright (c) 2026 Alexander Penkin. MIT License.

// Tests for the game-thread liveness half of ModalStateProbe: the heartbeat
// stamped by UPinWrightSubsystem::Tick, the staleness verdict the socket I/O
// thread reads from it, the in-flight RPC record the dispatcher publishes, and
// the `ping` response McpRequestCore builds out of all three.
//
// Why this matters enough to test: before the heartbeat existed, `ping` answered
// editorReady:true for the entire duration of a wedged python.execute, because
// the only liveness signal was a modal-loop latch that a looping handler never
// fires. The invariants below are the ones that make the difference between an
// honest verdict and that lie, plus the ones that keep a healthy-but-busy editor
// from being slandered as wedged.
//
// The probe is process-global and the live subsystem re-stamps its heartbeat
// every 0.1 s. Automation tests run synchronously on the game thread, so no tick
// can interleave inside a RunTest body; each test still calls ResetForTests()
// first so it does not inherit the previous test's (or the live editor's) state.
#include "Misc/AutomationTest.h"

#include "Handlers/ErrorCodes.h"
#include "JsonRpc.h"
#include "Transport/McpRequestCore.h"
#include "Transport/ModalStateProbe.h"
#include "Tests/TestSkipReporting.h"
#include "Tests/Transport/SocketTestClient.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/PlatformTime.h"

#if WITH_DEV_AUTOMATION_TESTS

// Named namespace (not anonymous): the plugin's tests share a module with Unity
// builds, and same-name anonymous-namespace helpers across .cpp files produce
// ODR / redefinition errors when Unity merges them into one TU.
namespace PinWrightStallProbeTest
{
    // An operational editor with no modal and no stall — the baseline every
    // "byte-identical below threshold" assertion compares against.
    inline McpRequestCore::FRequestConfig HealthyConfig()
    {
        McpRequestCore::FRequestConfig Config;
        Config.MaxBodyBytes = 1024 * 1024;
        Config.bEditorReady = true;
        Config.bEditorLoadingPackage = false;
        Config.bEditorSelectionSetAvailable = true;
        return Config;
    }

    inline FString PingEnvelope(int32 Id)
    {
        TSharedPtr<FJsonObject> Env = MakeShared<FJsonObject>();
        Env->SetStringField(TEXT("jsonrpc"), TEXT("2.0"));
        Env->SetNumberField(TEXT("id"), Id);
        Env->SetStringField(TEXT("method"), TEXT("ping"));
        return JsonRpc::Serialize(Env);
    }

    inline FString ToolsCallEnvelope(int32 Id, const FString& Method)
    {
        TSharedPtr<FJsonObject> Arguments = MakeShared<FJsonObject>();
        Arguments->SetStringField(TEXT("method"), Method);
        Arguments->SetObjectField(TEXT("args"), MakeShared<FJsonObject>());

        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("name"), TEXT("call"));
        Params->SetObjectField(TEXT("arguments"), Arguments);

        TSharedPtr<FJsonObject> Env = MakeShared<FJsonObject>();
        Env->SetStringField(TEXT("jsonrpc"), TEXT("2.0"));
        Env->SetNumberField(TEXT("id"), Id);
        Env->SetStringField(TEXT("method"), TEXT("tools/call"));
        Env->SetObjectField(TEXT("params"), Params);
        return JsonRpc::Serialize(Env);
    }

    inline TSharedPtr<FJsonObject> ResultOf(const TSharedPtr<FJsonObject>& Envelope)
    {
        if (!Envelope.IsValid()) return nullptr;
        const TSharedPtr<FJsonObject>* ResultPtr = nullptr;
        if (Envelope->TryGetObjectField(TEXT("result"), ResultPtr) && ResultPtr)
        {
            return *ResultPtr;
        }
        return nullptr;
    }

    // Run one ping through the full transport-agnostic pipeline and hand back the
    // `result` object.
    inline TSharedPtr<FJsonObject> Ping(const McpRequestCore::FRequestConfig& Config, int32 Id = 1)
    {
        McpRequestCore::FRequestDecision Out;
        McpRequestCore::ProcessRequestBody(PingEnvelope(Id), FString(), Out, Config);
        return ResultOf(Out.ImmediateBody);
    }
}

// ============================================================================
// Heartbeat: an unstamped clock is not a stall, a fresh stamp is not a stall, a
// backdated stamp is, and the threshold brackets it on both sides.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGameThreadStallHeartbeatTest,
    "PinWright.transport.liveness.Heartbeat.StalenessVerdict",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGameThreadStallHeartbeatTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightStallProbeTest;

    // Never stamped. This is the state between module load and the subsystem's
    // first tick; reporting it as a stall would make every cold start look wedged.
    ModalStateProbe::ResetForTests();
    TestEqual(TEXT("unstamped heartbeat reports no stall duration"),
        ModalStateProbe::GameThreadStalledSeconds(), 0.0);
    TestFalse(TEXT("unstamped heartbeat is not a stall"),
        ModalStateProbe::ShouldReportGameThreadStalled(90.0));

    // Stamped now — the healthy steady state.
    ModalStateProbe::NoteGameThreadAlive();
    TestTrue(TEXT("fresh stamp is at most a moment old"),
        ModalStateProbe::GameThreadStalledSeconds() < 5.0);
    TestFalse(TEXT("fresh stamp is not a stall"),
        ModalStateProbe::ShouldReportGameThreadStalled(90.0));

    // Backdated 200 s: the wedge.
    ModalStateProbe::SetLastAliveSecondsForTests(FPlatformTime::Seconds() - 200.0);
    const double Stalled = ModalStateProbe::GameThreadStalledSeconds();
    TestTrue(TEXT("backdated stamp reports roughly its age"),
        Stalled >= 199.0 && Stalled <= 210.0);
    TestTrue(TEXT("200 s clears a 90 s threshold"),
        ModalStateProbe::ShouldReportGameThreadStalled(90.0));
    // The other side of the bracket: a threshold above the stall stays silent, so
    // raising the setting genuinely suppresses the report.
    TestFalse(TEXT("200 s does not clear a 300 s threshold"),
        ModalStateProbe::ShouldReportGameThreadStalled(300.0));

    ModalStateProbe::ResetForTests();
    return true;
}

// ============================================================================
// The bug this file exists to prevent regressing: NoteGameThreadAlive() must
// stamp on EVERY call, not only when it has a modal latch to clear. The original
// function early-returned when unblocked, which is the common path — putting the
// stamp behind that early-out would mean the heartbeat is refreshed only while a
// modal is up, i.e. never during the wedge it is meant to detect.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGameThreadStallTickClearsStallTest,
    "PinWright.transport.liveness.Heartbeat.TickClearsStallWhenUnblocked",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGameThreadStallTickClearsStallTest::RunTest(const FString& Parameters)
{
    ModalStateProbe::ResetForTests();

    ModalStateProbe::SetLastAliveSecondsForTests(FPlatformTime::Seconds() - 500.0);
    if (!TestTrue(TEXT("precondition: probe reports a stall"),
            ModalStateProbe::ShouldReportGameThreadStalled(90.0)))
    {
        ModalStateProbe::ResetForTests();
        return true;
    }

    // No modal is latched, so this exercises exactly the early-out path.
    TestFalse(TEXT("precondition: no modal latch to clear"), ModalStateProbe::IsBlocked());
    ModalStateProbe::NoteGameThreadAlive();

    TestFalse(TEXT("a single healthy tick clears the stall"),
        ModalStateProbe::ShouldReportGameThreadStalled(90.0));
    TestTrue(TEXT("heartbeat is fresh again"),
        ModalStateProbe::GameThreadStalledSeconds() < 5.0);

    ModalStateProbe::ResetForTests();
    return true;
}

// ============================================================================
// A zero or negative threshold disables the probe. Without the guard the >=
// comparison would be satisfied by the 0.0 an unstamped heartbeat returns, so
// setting the threshold to 0 would report a permanent stall instead of none.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGameThreadStallZeroThresholdTest,
    "PinWright.transport.liveness.Heartbeat.NonPositiveThresholdDisables",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGameThreadStallZeroThresholdTest::RunTest(const FString& Parameters)
{
    ModalStateProbe::ResetForTests();

    ModalStateProbe::SetLastAliveSecondsForTests(FPlatformTime::Seconds() - 10000.0);
    TestFalse(TEXT("zero threshold disables the probe"),
        ModalStateProbe::ShouldReportGameThreadStalled(0.0));
    TestFalse(TEXT("negative threshold disables the probe"),
        ModalStateProbe::ShouldReportGameThreadStalled(-1.0));

    ModalStateProbe::ResetForTests();
    TestFalse(TEXT("zero threshold on an unstamped heartbeat is still not a stall"),
        ModalStateProbe::ShouldReportGameThreadStalled(0.0));

    return true;
}

// ============================================================================
// In-flight RPC record: set and cleared together, aged from its own start, and
// empty when the game thread is not inside a dispatch.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGameThreadStallInFlightRecordTest,
    "PinWright.transport.liveness.InFlight.RecordedAndCleared",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGameThreadStallInFlightRecordTest::RunTest(const FString& Parameters)
{
    ModalStateProbe::ResetForTests();

    FString Method;
    FString RequestId;
    double Seconds = -1.0;

    ModalStateProbe::GetInFlightRpc(Method, RequestId, Seconds);
    TestTrue(TEXT("idle: method empty"), Method.IsEmpty());
    TestTrue(TEXT("idle: request id empty"), RequestId.IsEmpty());
    TestEqual(TEXT("idle: age is zero"), Seconds, 0.0);

    ModalStateProbe::NoteRpcDispatchBegin(TEXT("req-42"), TEXT("python.execute"));
    ModalStateProbe::GetInFlightRpc(Method, RequestId, Seconds);
    TestEqual(TEXT("in flight: method published"), Method, FString(TEXT("python.execute")));
    TestEqual(TEXT("in flight: request id published"), RequestId, FString(TEXT("req-42")));
    TestTrue(TEXT("in flight: age is non-negative and small"),
        Seconds >= 0.0 && Seconds < 5.0);

    ModalStateProbe::NoteRpcDispatchEnd();
    ModalStateProbe::GetInFlightRpc(Method, RequestId, Seconds);
    TestTrue(TEXT("after end: method cleared"), Method.IsEmpty());
    TestTrue(TEXT("after end: request id cleared"), RequestId.IsEmpty());
    TestEqual(TEXT("after end: age reset"), Seconds, 0.0);

    // Begin overwrites rather than nests — the dispatcher's reentrancy guard means
    // only one request is ever in flight, and the deferred drain re-stamps.
    ModalStateProbe::NoteRpcDispatchBegin(TEXT("req-1"), TEXT("actor.list"));
    ModalStateProbe::NoteRpcDispatchBegin(TEXT("req-2"), TEXT("level.save"));
    ModalStateProbe::GetInFlightRpc(Method, RequestId, Seconds);
    TestEqual(TEXT("second begin wins"), Method, FString(TEXT("level.save")));
    TestEqual(TEXT("second begin's id wins"), RequestId, FString(TEXT("req-2")));

    ModalStateProbe::ResetForTests();
    ModalStateProbe::GetInFlightRpc(Method, RequestId, Seconds);
    TestTrue(TEXT("ResetForTests clears the in-flight record"), Method.IsEmpty());

    return true;
}

// ============================================================================
// ping, wedged with a PinWright RPC in flight. This is the exact response the
// 168-minute hang should have produced and did not.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGameThreadStallPingReportsWedgeTest,
    "PinWright.transport.liveness.Ping.StalledReportsWedgeTruthfully",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGameThreadStallPingReportsWedgeTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightStallProbeTest;

    // The readiness snapshot is deliberately left POSITIVE: during a wedge it is
    // frozen stale-but-positive, because the game thread that writes it is the
    // thread that stopped. If the stall did not override it, ping would answer
    // editorReady:true — the original defect.
    McpRequestCore::FRequestConfig Config = HealthyConfig();
    Config.bGameThreadStalled = true;
    Config.GameThreadStalledSeconds = 412.0;
    Config.InFlightMethod = TEXT("python.execute");
    Config.InFlightRequestId = TEXT("9f2c-...-a1");
    Config.InFlightSeconds = 411.0;

    TSharedPtr<FJsonObject> Result = Ping(Config, 7);
    if (!TestTrue(TEXT("ping produced a result object"), Result.IsValid())) return true;

    bool bEditorReady = true;
    TestTrue(TEXT("editorReady reported"),
        Result->TryGetBoolField(TEXT("editorReady"), bEditorReady));
    TestFalse(TEXT("a wedged editor is NOT ready, despite a positive stale snapshot"),
        bEditorReady);

    // Retryable, unlike a modal: the handler may still return and the queued work
    // will run. A client that stops polling here would give up on a recoverable
    // condition.
    bool bRetryable = false;
    TestTrue(TEXT("retryable reported"),
        Result->TryGetBoolField(TEXT("retryable"), bRetryable));
    TestTrue(TEXT("a stall is retryable"), bRetryable);

    TestEqual(TEXT("error code names the stall"),
        Result->GetStringField(TEXT("error")),
        FString(ErrorCodes::ERR_EDITOR_GAME_THREAD_STALLED));

    bool bStalled = false;
    TestTrue(TEXT("gameThreadStalled flag present"),
        Result->TryGetBoolField(TEXT("gameThreadStalled"), bStalled) && bStalled);

    double StalledSeconds = 0.0;
    TestTrue(TEXT("stalledSeconds present"),
        Result->TryGetNumberField(TEXT("stalledSeconds"), StalledSeconds));
    TestEqual(TEXT("stalledSeconds carries the measured duration"), StalledSeconds, 412.0);

    // Naming the verb is the difference between "something is wrong" and "your
    // python.execute is the thing that is wrong".
    TestEqual(TEXT("in-flight method named"),
        Result->GetStringField(TEXT("inFlightMethod")), FString(TEXT("python.execute")));
    TestEqual(TEXT("in-flight request id named"),
        Result->GetStringField(TEXT("inFlightRequestId")), FString(TEXT("9f2c-...-a1")));
    double InFlightSeconds = 0.0;
    TestTrue(TEXT("inFlightSeconds present"),
        Result->TryGetNumberField(TEXT("inFlightSeconds"), InFlightSeconds));
    TestEqual(TEXT("inFlightSeconds carries the dispatch age"), InFlightSeconds, 411.0);

    FString Message;
    TestTrue(TEXT("message present"), Result->TryGetStringField(TEXT("message"), Message));
    TestTrue(TEXT("message names the wedged verb"), Message.Contains(TEXT("python.execute")));
    TestTrue(TEXT("message quotes the duration"), Message.Contains(TEXT("412")));
    TestTrue(TEXT("message states PinWright cannot interrupt it"),
        Message.Contains(TEXT("cannot interrupt")));

    // The modal fields belong to the other probe and must not leak into this one.
    TestFalse(TEXT("no modal claim on a stall"), Result->HasField(TEXT("blockedOnModal")));

    return true;
}

// ============================================================================
// ping, wedged with nothing dispatched. A stall outside any RPC is engine work,
// not a PinWright bug, and the response must not invent an unnamed RPC by
// emitting an empty inFlightMethod.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGameThreadStallPingNoInFlightTest,
    "PinWright.transport.liveness.Ping.StalledWithoutInFlightOmitsMethod",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGameThreadStallPingNoInFlightTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightStallProbeTest;

    McpRequestCore::FRequestConfig Config = HealthyConfig();
    Config.bGameThreadStalled = true;
    Config.GameThreadStalledSeconds = 120.0;

    TSharedPtr<FJsonObject> Result = Ping(Config, 8);
    if (!TestTrue(TEXT("ping produced a result object"), Result.IsValid())) return true;

    TestEqual(TEXT("still the stall error code"),
        Result->GetStringField(TEXT("error")),
        FString(ErrorCodes::ERR_EDITOR_GAME_THREAD_STALLED));
    TestFalse(TEXT("inFlightMethod omitted, not emitted empty"),
        Result->HasField(TEXT("inFlightMethod")));
    TestFalse(TEXT("inFlightRequestId omitted"), Result->HasField(TEXT("inFlightRequestId")));
    TestFalse(TEXT("inFlightSeconds omitted"), Result->HasField(TEXT("inFlightSeconds")));

    FString Message;
    TestTrue(TEXT("message present"), Result->TryGetStringField(TEXT("message"), Message));
    TestTrue(TEXT("message attributes the stall to engine-internal work"),
        Message.Contains(TEXT("engine-internal")));
    // The counter is monotone during a wedge; the message must tell the caller how to
    // read that, and must not promise the condition clears on its own.
    TestTrue(TEXT("message explains a rising counter means no tick in between"),
        Message.Contains(TEXT("not draining")));
    TestFalse(TEXT("message no longer promises the stall clears by itself"),
        Message.Contains(TEXT("clears by itself")));
    TestFalse(TEXT("no GPU claim while the GPU is healthy"), Result->HasField(TEXT("gpuCrashed")));

    return true;
}

// ============================================================================
// A modal stops the core ticker too, so past the stall threshold BOTH probes are
// true. The modal must win: it is the more specific explanation of the same
// physical fact and it is the non-retryable one, and telling a client to retry a
// dialog nobody will dismiss is the loop that burns a watchdog.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGameThreadStallModalOutranksStallTest,
    "PinWright.transport.liveness.Ping.ModalOutranksStall",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGameThreadStallModalOutranksStallTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightStallProbeTest;

    McpRequestCore::FRequestConfig Config = HealthyConfig();
    Config.bBlockedOnModal = true;
    Config.BlockedOnModalSeconds = 300.0;
    Config.ModalTitle = TEXT("Restore Packages");
    Config.bGameThreadStalled = true;
    Config.GameThreadStalledSeconds = 298.0;
    Config.InFlightMethod = TEXT("level.save");

    TSharedPtr<FJsonObject> Result = Ping(Config, 9);
    if (!TestTrue(TEXT("ping produced a result object"), Result.IsValid())) return true;

    TestEqual(TEXT("modal wins the error code"),
        Result->GetStringField(TEXT("error")),
        FString(ErrorCodes::ERR_EDITOR_BLOCKED_ON_MODAL));
    TestTrue(TEXT("modal fields still emitted"), Result->HasField(TEXT("blockedOnModal")));
    TestEqual(TEXT("modal title still emitted"),
        Result->GetStringField(TEXT("modalTitle")), FString(TEXT("Restore Packages")));
    TestFalse(TEXT("stall branch suppressed"), Result->HasField(TEXT("gameThreadStalled")));

    bool bRetryable = true;
    TestTrue(TEXT("retryable reported"),
        Result->TryGetBoolField(TEXT("retryable"), bRetryable));
    TestFalse(TEXT("a modal stays non-retryable even when also stale"), bRetryable);

    bool bEditorReady = true;
    Result->TryGetBoolField(TEXT("editorReady"), bEditorReady);
    TestFalse(TEXT("not ready either way"), bEditorReady);

    return true;
}

// ============================================================================
// Below the threshold, nothing changes — byte for byte. This is the invariant
// that makes a false-positive threshold the only real risk of the whole probe:
// as long as the transport does not set the flag, the response is the one that
// shipped before the probe existed.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGameThreadStallBelowThresholdIdenticalTest,
    "PinWright.transport.liveness.Ping.BelowThresholdIsByteIdentical",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGameThreadStallBelowThresholdIdenticalTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightStallProbeTest;

    McpRequestCore::FRequestDecision Baseline;
    McpRequestCore::ProcessRequestBody(
        PingEnvelope(3), FString(), Baseline, HealthyConfig());

    // Everything the probe measures is populated EXCEPT the gating flag, which is
    // exactly the state the transport publishes for a busy-but-under-threshold
    // editor.
    McpRequestCore::FRequestConfig Busy = HealthyConfig();
    Busy.GameThreadStalledSeconds = 42.0;
    Busy.InFlightMethod = TEXT("asset.save");
    Busy.InFlightRequestId = TEXT("req-7");
    Busy.InFlightSeconds = 41.0;

    McpRequestCore::FRequestDecision UnderThreshold;
    McpRequestCore::ProcessRequestBody(
        PingEnvelope(3), FString(), UnderThreshold, Busy);

    if (!TestTrue(TEXT("both pings produced a body"),
            Baseline.ImmediateBody.IsValid() && UnderThreshold.ImmediateBody.IsValid()))
    {
        return true;
    }

    TestEqual(TEXT("an under-threshold editor answers byte-identically to a healthy one"),
        JsonRpc::Serialize(UnderThreshold.ImmediateBody),
        JsonRpc::Serialize(Baseline.ImmediateBody));

    return true;
}

// ============================================================================
// The stall is diagnostic, NOT a gate. tools/call must still be handed to the
// dispatcher during a stall: the wedged handler may return, and the queued
// request runs when it does. Hard-failing here would break every legitimately
// long game-thread operation, which is the reason the modal gate was not simply
// widened to cover stalls.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGameThreadStallDoesNotGateToolsCallTest,
    "PinWright.transport.liveness.ToolsCall.StallDoesNotGateDispatch",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGameThreadStallDoesNotGateToolsCallTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightStallProbeTest;

    McpRequestCore::FRequestConfig Config = HealthyConfig();
    Config.bGameThreadStalled = true;
    Config.GameThreadStalledSeconds = 600.0;
    Config.InFlightMethod = TEXT("python.execute");
    Config.InFlightRequestId = TEXT("req-99");
    Config.InFlightSeconds = 599.0;

    McpRequestCore::FRequestDecision Out;
    TestTrue(TEXT("request accepted"), McpRequestCore::ProcessRequestBody(
        ToolsCallEnvelope(10, TEXT("system.status")), FString(), Out, Config));
    TestEqual(TEXT("a stall does not divert tools/call away from the dispatcher"),
        Out.Kind, McpRequestCore::FRequestDecision::EKind::DispatchRpc);
    TestEqual(TEXT("the dotted method still reaches the dispatcher"),
        Out.Method, FString(TEXT("system.status")));

    // Contrast: a modal DOES gate, and that difference is deliberate.
    McpRequestCore::FRequestConfig ModalConfig = HealthyConfig();
    ModalConfig.bBlockedOnModal = true;
    ModalConfig.BlockedOnModalSeconds = 5.0;

    McpRequestCore::FRequestDecision ModalOut;
    McpRequestCore::ProcessRequestBody(
        ToolsCallEnvelope(11, TEXT("system.status")), FString(), ModalOut, ModalConfig);
    TestEqual(TEXT("a modal still gates tools/call"),
        ModalOut.Kind, McpRequestCore::FRequestDecision::EKind::ImmediateResponse);

    return true;
}

// ============================================================================
// When the readiness gate trips on its OWN grounds and the thread is also
// stalled, the visible text and the structured `error` must name the same
// condition. The snapshot that closed the gate is written by the thread that
// stopped, so the stall is the honest explanation of both — reporting
// "still starting" over a 200 s wedge would send the agent back to polling.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGameThreadStallNotReadyDetailsAgreeTest,
    "PinWright.transport.liveness.ToolsCall.NotReadyAndStalledAgree",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGameThreadStallNotReadyDetailsAgreeTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightStallProbeTest;

    // Not operational on its own terms (a package load never finished) AND the
    // thread has not ticked since — the long-map-load shape.
    McpRequestCore::FRequestConfig Config;
    Config.MaxBodyBytes = 1024 * 1024;
    Config.bEditorReady = true;
    Config.bEditorLoadingPackage = true;
    Config.bEditorSelectionSetAvailable = true;
    Config.bGameThreadStalled = true;
    Config.GameThreadStalledSeconds = 200.0;

    McpRequestCore::FRequestDecision Out;
    McpRequestCore::ProcessRequestBody(
        ToolsCallEnvelope(12, TEXT("system.status")), FString(), Out, Config);
    TestEqual(TEXT("the readiness gate still trips on its own grounds"),
        Out.Kind, McpRequestCore::FRequestDecision::EKind::ImmediateResponse);

    TSharedPtr<FJsonObject> ToolResult = ResultOf(Out.ImmediateBody);
    if (!TestTrue(TEXT("tool result present"), ToolResult.IsValid())) return true;

    const TSharedPtr<FJsonObject>* Details = nullptr;
    if (TestTrue(TEXT("structured details present"),
            ToolResult->TryGetObjectField(TEXT("structuredContent"), Details)
            && Details && (*Details).IsValid()))
    {
        TestEqual(TEXT("structured error names the stall, not the startup state"),
            (*Details)->GetStringField(TEXT("error")),
            FString(ErrorCodes::ERR_EDITOR_GAME_THREAD_STALLED));
    }

    const TArray<TSharedPtr<FJsonValue>>* Content = nullptr;
    if (TestTrue(TEXT("content array present"),
            ToolResult->TryGetArrayField(TEXT("content"), Content)
            && Content && Content->Num() > 0))
    {
        const TSharedPtr<FJsonObject> Block = (*Content)[0]->AsObject();
        if (TestTrue(TEXT("content[0] is an object"), Block.IsValid()))
        {
            const FString Text = Block->GetStringField(TEXT("text"));
            TestTrue(TEXT("visible text carries the same code as the structured field"),
                Text.StartsWith(
                    FString::Printf(TEXT("[%s]"), ErrorCodes::ERR_EDITOR_GAME_THREAD_STALLED)));
        }
    }

    return true;
}

// ============================================================================
// A stall on a crashed GPU (GIsGPUCrashed) never drains. The benign enumeration
// ("asset compile, map load, package save") and the wait advice were exactly
// wrong for it, and `retryable: true` fed every queued caller's retry loop
// (B-editor-not-ready-blames-benign-work-when-gpu-is-removed).
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGameThreadStallCrashedGpuPingTest,
    "PinWright.transport.liveness.Ping.StalledOnCrashedGpuIsTerminal",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGameThreadStallCrashedGpuPingTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightStallProbeTest;

    McpRequestCore::FRequestConfig Config = HealthyConfig();
    Config.bGameThreadStalled = true;
    Config.GameThreadStalledSeconds = 269.0;
    Config.bGpuCrashed = true;

    TSharedPtr<FJsonObject> Result = Ping(Config, 13);
    if (!TestTrue(TEXT("ping produced a result object"), Result.IsValid())) return true;

    TestEqual(TEXT("still the stall error code"),
        Result->GetStringField(TEXT("error")),
        FString(ErrorCodes::ERR_EDITOR_GAME_THREAD_STALLED));
    bool bRetryable = true;
    TestTrue(TEXT("retryable reported"), Result->TryGetBoolField(TEXT("retryable"), bRetryable));
    TestFalse(TEXT("a stall on a dead GPU is not retryable"), bRetryable);
    bool bGpuCrashed = false;
    TestTrue(TEXT("gpuCrashed reported"), Result->TryGetBoolField(TEXT("gpuCrashed"), bGpuCrashed));
    TestTrue(TEXT("gpuCrashed is true"), bGpuCrashed);

    FString Message;
    TestTrue(TEXT("message present"), Result->TryGetStringField(TEXT("message"), Message));
    TestTrue(TEXT("message names the GPU crash"), Message.Contains(TEXT("GPU crash")));
    TestTrue(TEXT("message gives the terminal verdict"), Message.Contains(TEXT("The editor is dead")));
    TestFalse(TEXT("benign enumeration is gone"), Message.Contains(TEXT("asset compile")));
    TestFalse(TEXT("no wait advice"), Message.Contains(TEXT("Wait only if")));

    // Contrast: the same stall on a healthy GPU stays retryable.
    Config.bGpuCrashed = false;
    TSharedPtr<FJsonObject> Healthy = Ping(Config, 14);
    if (!TestTrue(TEXT("healthy-GPU ping produced a result object"), Healthy.IsValid())) return true;
    bool bHealthyRetryable = false;
    Healthy->TryGetBoolField(TEXT("retryable"), bHealthyRetryable);
    TestTrue(TEXT("a healthy-GPU stall stays retryable"), bHealthyRetryable);

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGameThreadStallCrashedGpuGatesToolsCallTest,
    "PinWright.transport.liveness.ToolsCall.CrashedGpuStallGatesDispatch",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGameThreadStallCrashedGpuGatesToolsCallTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightStallProbeTest;

    McpRequestCore::FRequestConfig Config = HealthyConfig();
    Config.bGameThreadStalled = true;
    Config.GameThreadStalledSeconds = 300.0;
    Config.bGpuCrashed = true;

    McpRequestCore::FRequestDecision Out;
    McpRequestCore::ProcessRequestBody(
        ToolsCallEnvelope(15, TEXT("system.status")), FString(), Out, Config);
    TestEqual(TEXT("a dead-GPU stall answers tools/call immediately instead of queueing it"),
        Out.Kind, McpRequestCore::FRequestDecision::EKind::ImmediateResponse);

    TSharedPtr<FJsonObject> ToolResult = ResultOf(Out.ImmediateBody);
    if (!TestTrue(TEXT("tool result present"), ToolResult.IsValid())) return true;
    const TArray<TSharedPtr<FJsonValue>>* Content = nullptr;
    if (TestTrue(TEXT("content array present"),
            ToolResult->TryGetArrayField(TEXT("content"), Content)
            && Content && Content->Num() > 0))
    {
        const TSharedPtr<FJsonObject> Block = (*Content)[0]->AsObject();
        if (TestTrue(TEXT("content[0] is an object"), Block.IsValid()))
        {
            const FString Text = Block->GetStringField(TEXT("text"));
            TestTrue(TEXT("visible text carries the stall code"),
                Text.StartsWith(
                    FString::Printf(TEXT("[%s]"), ErrorCodes::ERR_EDITOR_GAME_THREAD_STALLED)));
            TestTrue(TEXT("visible text names the GPU crash"), Text.Contains(TEXT("GPU crash")));
        }
    }
    const TSharedPtr<FJsonObject>* Details = nullptr;
    if (TestTrue(TEXT("structured details present"),
            ToolResult->TryGetObjectField(TEXT("structuredContent"), Details)
            && Details && (*Details).IsValid()))
    {
        bool bRetryable = true;
        TestTrue(TEXT("structured retryable reported"),
            (*Details)->TryGetBoolField(TEXT("retryable"), bRetryable));
        TestFalse(TEXT("structured retryable is false"), bRetryable);
        bool bGpuCrashed = false;
        TestTrue(TEXT("structured gpuCrashed reported"),
            (*Details)->TryGetBoolField(TEXT("gpuCrashed"), bGpuCrashed));
        TestTrue(TEXT("structured gpuCrashed is true"), bGpuCrashed);
    }

    return true;
}


// ============================================================================
// Board B-screenshot-designer-hangs-game-thread: the editor wedged right after a
// widget.screenshot_designer call, and ping answered "No PinWright RPC is in
// flight ... engine-internal" - true of the handler stack, but it named no
// request while that call sat queued behind the wedge. A request handed to the
// game thread and still unanswered is now named as awaiting, and only when no
// handler is in flight (an in-flight one is the better explanation).
//
// Counterfactual: drop the AwaitingMethod branch in BuildPingResult and the
// awaiting* fields vanish; drop it from GameThreadStalledMessage and the message
// goes back to "engine-internal" without the method.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGameThreadStallPingNamesAwaitingRequestTest,
    "PinWright.transport.liveness.Ping.StalledNamesAwaitingRequest",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGameThreadStallPingNamesAwaitingRequestTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightStallProbeTest;

    McpRequestCore::FRequestConfig Config = HealthyConfig();
    Config.bGameThreadStalled = true;
    Config.GameThreadStalledSeconds = 128.0;
    Config.AwaitingMethod = TEXT("widget.screenshot_designer");
    Config.AwaitingRequestId = TEXT("req-awaiting");
    Config.AwaitingSeconds = 130.0;

    TSharedPtr<FJsonObject> Result = Ping(Config, 9);
    if (!TestTrue(TEXT("ping produced a result object"), Result.IsValid())) return true;

    TestEqual(TEXT("still the stall error code"),
        Result->GetStringField(TEXT("error")),
        FString(ErrorCodes::ERR_EDITOR_GAME_THREAD_STALLED));
    TestFalse(TEXT("no in-flight claim"), Result->HasField(TEXT("inFlightMethod")));
    TestEqual(TEXT("awaiting method named"),
        Result->GetStringField(TEXT("awaitingMethod")), FString(TEXT("widget.screenshot_designer")));
    TestEqual(TEXT("awaiting request id named"),
        Result->GetStringField(TEXT("awaitingRequestId")), FString(TEXT("req-awaiting")));
    double AwaitingSeconds = 0.0;
    TestTrue(TEXT("awaitingSeconds present"),
        Result->TryGetNumberField(TEXT("awaitingSeconds"), AwaitingSeconds));
    TestEqual(TEXT("awaitingSeconds carries the handoff age"), AwaitingSeconds, 130.0);

    FString Message;
    TestTrue(TEXT("message present"), Result->TryGetStringField(TEXT("message"), Message));
    TestTrue(TEXT("message names the awaiting verb"),
        Message.Contains(TEXT("widget.screenshot_designer")));
    TestFalse(TEXT("message no longer says no PinWright RPC is involved"),
        Message.Contains(TEXT("engine-internal")));

    // An executing handler outranks a queued one: same config plus an in-flight RPC
    // reports only the in-flight fields.
    Config.InFlightMethod = TEXT("python.execute");
    Config.InFlightRequestId = TEXT("req-running");
    Config.InFlightSeconds = 127.0;
    TSharedPtr<FJsonObject> Busy = Ping(Config, 10);
    if (!TestTrue(TEXT("busy ping produced a result object"), Busy.IsValid())) return true;
    TestEqual(TEXT("in-flight method wins"),
        Busy->GetStringField(TEXT("inFlightMethod")), FString(TEXT("python.execute")));
    TestFalse(TEXT("awaiting fields omitted while a handler runs"),
        Busy->HasField(TEXT("awaitingMethod")));
    return true;
}

// End to end through FSocketHttpServer, in the ticket's order: the request is handed
// off, its caller times out and disconnects (dropping the completion), then the
// thread is stale and a fresh client pings. The awaiting record must outlive the
// dropped completion - that is the ticket's exact state.
//
// Counterfactual: remove the AwaitingRequests fill in HandleCompleteRequest, or key the
// record off PendingCompletions, and awaitingMethod is absent from the ping.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGameThreadStallAwaitingSurvivesDisconnectTest,
    "PinWright.transport.liveness.Awaiting.SurvivesClientDisconnect",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGameThreadStallAwaitingSurvivesDisconnectTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightSocketTest;

    const UPinWrightSettings* Settings = GetDefault<UPinWrightSettings>();
    const double Threshold = Settings
        ? static_cast<double>(Settings->GameThreadStallReportSeconds)
        : 90.0;
    if (Threshold <= 0.0)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("stall-probe-disabled"),
            TEXT("GameThreadStallReportSeconds <= 0 disables the stall probe; skipping."));
        return true;
    }

    FScopedAuthRequirement Auth(false);
    uint32 Port = 0;
    TSharedPtr<FSocketHttpServer> Server = StartServerOnFreePort(Port);
    if (!Server.IsValid())
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("test-port-unbindable"),
            TEXT("Could not bind a test port for FSocketHttpServer; skipping."));
        return true;
    }
    ON_SCOPE_EXIT
    {
        Server->Stop();
        ModalStateProbe::ResetForTests();
    };
    TSharedRef<FDispatchCapture> Capture = MakeShared<FDispatchCapture>();
    BindCaptureDispatcher(*Server, Capture, EDispatchMode::Hold);

    FString RequestId;
    {
        FSocketTestClient Caller;
        if (!TestTrue(TEXT("caller connected"), Caller.Connect(Port))) return true;
        const FEstablishedStream Stream =
            EstablishSseStream(*this, Caller, Server.Get(), Capture, 41);
        if (!Stream.bOk) return true;
        RequestId = Stream.RequestId;
        Caller.Close();
    }
    const FString RequestIdCopy = RequestId;
    TestTrue(TEXT("the disconnected caller's completion was dropped"),
        WaitForCondition([&Server, RequestIdCopy]()
            { return !Server->IsStreamingRequest(RequestIdCopy); }, Server.Get()));

    // Wedged with no handler on the stack. From here on nothing may tick the core
    // ticker: the live subsystem would re-stamp the heartbeat.
    ModalStateProbe::ResetForTests();
    ModalStateProbe::SetLastAliveSecondsForTests(FPlatformTime::Seconds() - (Threshold + 60.0));

    FSocketTestClient Pinger;
    if (!TestTrue(TEXT("pinger connected"), Pinger.Connect(Port))) return true;
    const FString PingRequest = BuildHttpRequest(TEXT("POST"), TEXT("/mcp"),
        {{TEXT("Content-Type"), TEXT("application/json")}}, BuildPingBody(42));
    FTCHARToUTF8 PingUtf8(*PingRequest);
    int32 Sent = 0;
    TestTrue(TEXT("ping sent in one write"),
        Pinger.Sock->Send(reinterpret_cast<const uint8*>(PingUtf8.Get()), PingUtf8.Length(), Sent)
        && Sent == PingUtf8.Length());

    // The I/O thread answers ping on its own; poll the socket only.
    FParsedHttpResponse Response;
    int32 Next = 0;
    const double Deadline = FPlatformTime::Seconds() + kDefaultTimeoutSeconds;
    while (!ParseOneHttpResponse(Pinger.Received, 0, Response, Next)
        && FPlatformTime::Seconds() < Deadline)
    {
        if (Pinger.RecvStep() != ERecvStep::Data)
        {
            FPlatformProcess::Sleep(0.001f);
        }
    }
    if (!TestTrue(TEXT("ping answered while the game thread is 'wedged'"), Response.bComplete))
    {
        return true;
    }

    const TSharedPtr<FJsonObject> Envelope = ParseJsonObject(Response.Body);
    const TSharedPtr<FJsonObject>* ResultPtr = nullptr;
    if (!TestTrue(TEXT("ping result object"),
            Envelope.IsValid() && Envelope->TryGetObjectField(TEXT("result"), ResultPtr) && ResultPtr))
    {
        return true;
    }
    const TSharedPtr<FJsonObject> Result = *ResultPtr;
    TestEqual(TEXT("stall reported"),
        Result->GetStringField(TEXT("error")),
        FString(ErrorCodes::ERR_EDITOR_GAME_THREAD_STALLED));
    TestFalse(TEXT("no handler in flight"), Result->HasField(TEXT("inFlightMethod")));
    TestEqual(TEXT("the queued request is named although its caller left"),
        Result->GetStringField(TEXT("awaitingMethod")), FString(TEXT("test.stream")));
    TestEqual(TEXT("its request id is named"),
        Result->GetStringField(TEXT("awaitingRequestId")), RequestId);
    return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
