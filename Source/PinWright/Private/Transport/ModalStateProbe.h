// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"

// Detects "the game thread cannot run an RPC" and publishes it to the socket I/O
// thread. Two independent probes live here, for two causes of the same symptom:
//
//   1. MODAL LATCH - a modal dialog owns the game thread. Latched from inside the
//      nested Slate loop, cleared from the subsystem tick. Threshold ~2 s,
//      NON-retryable: only a human or a process kill clears it.
//   2. HEARTBEAT STALENESS - a handler (or the engine) is simply not returning, so
//      the game thread never completes a frame. Stamped from the subsystem tick
//      while healthy; the I/O thread declares a stall from the stamp's age.
//      Threshold ~90 s, RETRYABLE: it may drain on its own.
//
// The namespace name is historical - it predates (2). Both probes share the same
// purpose, the same publish-to-I/O-thread mechanics, and the same clock rationale
// (see BlockedSeconds below), so they stay in one file.
//
// -- (1) the modal latch ------------------------------------------------------
//
// While a modal window is up the game thread sits in a nested Slate loop
// (SlateApplication.cpp:2238 on 5.8), so NOTHING game-thread-driven can report
// it: UPinWrightSubsystem's 0.1 s FTSTicker stops, which stops the readiness
// refresh AND FSocketHttpServer::Tick's completion-timeout sweep AND the
// dispatcher's deferred queue. The readiness snapshot the transport already
// publishes is stale-but-positive, so `ping` keeps answering "ready" from the
// I/O thread while no RPC can ever run. That is the hang an agent cannot
// distinguish from a crash.
//
// FSlateApplication::GetOnModalLoopTickEvent() is broadcast from INSIDE that
// nested loop (SlateApplication.cpp:2253 on 5.8), ~60 Hz, and is declared
// identically on UE 5.3-5.8 (SlateApplication.h:1669 on 5.3 ... :1844 on 5.8),
// so no version branch is needed. We latch there and clear from the subsystem
// tick, which can only run once the core ticker resumes - i.e. once the modal
// is gone. That inversion is what makes the clear trustworthy.
//
// Deliberately not used: FCoreDelegates::PreSlateModalWithContext is 5.8-only
// and carries no title, and a cancelled modal returns from AddModalWindow
// before that delegate is broadcast at all.
//
// -- (2) the heartbeat -------------------------------------------------------
//
// The modal latch is blind to the far more common wedge: a handler that simply
// does not return. `python.execute` calls IPythonScriptPlugin::ExecPythonCommandEx
// synchronously on the game thread and blocks for the whole script; nothing
// broadcasts anything while it runs, so the latch never fires and `ping` answered
// editorReady:true for a 168-minute hang. There is no "I am blocked" signal to
// latch, so the construction inverts: NoteGameThreadAlive() stamps a timestamp
// while the thread is DEMONSTRABLY healthy (the core ticker ran), and the I/O
// thread reads the stamp's AGE. Nothing has to fire during the wedge for the
// wedge to be visible - the absence is the signal.
//
// Threshold discipline matters more here than for the modal case: multi-second
// game-thread stalls are routine (map loads, saves, synchronous asset compiles),
// and the C++ automation suite has shown legitimate stalls of 53.9 s under load.
// A threshold that false-positives on a healthy-but-busy editor is worse than no
// probe at all, so the default (UPinWrightSettings::GameThreadStallReportSeconds,
// 90 s) sits well above the worst observed legitimate stall. Unlike a modal, a
// stall is reported RETRYABLE and does NOT gate tools/call - the work may still
// complete, and the queued request will run when it does.
namespace ModalStateProbe
{
    // Subscribe to the Slate modal-loop tick. Called from the module's
    // OnPostEngineInit handler, which is strictly earlier than the boot-time
    // "Restore Packages" prompt (UnrealEdMisc.cpp:376), so even that failure is
    // reportable. Safe to call when Slate is unavailable (no-op) and idempotent.
    void Register();

    // Drop the subscription. Called from ShutdownModule.
    void Unregister();

    // Game thread, inside the nested modal loop. Latches the block start and
    // copies the active modal window's title.
    void OnModalLoopTick(float DeltaTime);

    // Game thread, from UPinWrightSubsystem::Tick. Does BOTH jobs: stamps the
    // liveness heartbeat, then clears the modal latch. The core ticker running at
    // all is the proof for both - a nested Slate modal loop pumps only Slate, and
    // a wedged handler pumps nothing.
    void NoteGameThreadAlive();

    // Game thread, from FRpcDispatcher::ProcessRequest, bracketing the handler
    // call. Names what the thread is inside when it stalls: a duration alone
    // cannot tell an agent whether ITS request caused the wedge. Begin overwrites
    // rather than nests - the dispatcher's reentrancy guard means at most one
    // request is ever in flight, and the deferred-queue drain re-stamps.
    void NoteRpcDispatchBegin(const FString& RequestId, const FString& Method);
    void NoteRpcDispatchEnd();

    // ---- I/O-thread-safe readers ----

    bool IsBlocked();

    // Wall-clock seconds since the latch, 0 when clear. Uses
    // FPlatformTime::Seconds() on both threads because FSocketHttpServer's
    // TickClockSeconds is advanced by the game-thread tick and freezes with
    // everything else - it cannot measure this.
    double BlockedSeconds();

    // Empty when the title could not be read (no active modal window, or Slate
    // unavailable). Callers must omit the field rather than emit "".
    FString GetModalTitle();

    // IsBlocked() && BlockedSeconds() >= ThresholdSeconds. Below the threshold
    // every response stays byte-identical to the unblocked case, so a human
    // dismissing a dialog in under two seconds never produces a spurious
    // non-retryable failure.
    bool ShouldReportBlocked(double ThresholdSeconds);

    // Wall-clock seconds since the last NoteGameThreadAlive(), or 0.0 when the
    // heartbeat has never been stamped. Zero-when-never-stamped is load-bearing:
    // between module load and the subsystem's first tick there is no heartbeat to
    // be stale, and reporting that gap as a stall would make every cold start
    // look wedged. Same FPlatformTime::Seconds() clock as BlockedSeconds, for the
    // same reason.
    double GameThreadStalledSeconds();

    // GameThreadStalledSeconds() >= ThresholdSeconds, with a positive threshold
    // required so a zero/negative setting disables the probe rather than
    // reporting a permanent stall. Below the threshold every response stays
    // byte-identical to the healthy case.
    bool ShouldReportGameThreadStalled(double ThresholdSeconds);

    // The RPC the game thread is inside, empty when none. Both strings are set
    // and cleared together under one lock, so a reader never sees a method
    // without its request id. OutSeconds is how long it has been in flight, 0
    // when idle.
    void GetInFlightRpc(FString& OutMethod, FString& OutRequestId, double& OutSeconds);

    // Clears all state. Tests only - the probe is a process-global latch.
    void ResetForTests();

    // Backdates the heartbeat so a test can observe staleness without sleeping.
    // Pass an absolute FPlatformTime::Seconds() value; 0.0 models "never
    // stamped". Tests only.
    void SetLastAliveSecondsForTests(double AbsoluteSeconds);
}
