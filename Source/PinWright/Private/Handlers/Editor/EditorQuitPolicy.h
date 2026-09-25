// Copyright (c) 2026 Alexander Penkin. MIT License.

// The two shutdown-safety decisions of editor.quit, factored out of
// EditorQuitHandler.cpp so they can be tested without running the verb that ends
// the process. Header-only.
//
//   1. IN USE - refuse to end a process another client is still driving.
//   2. ASSET EDITORS - close every open asset editor BEFORE requesting exit.
//
// Both are the same lesson the PIE guard in EditorQuitHandler.cpp already
// records: what shutdown does is decided long before FEngineLoop::Exit() runs.
#pragma once

#include "CoreMinimal.h"

// GEditor->GetEditorSubsystem<UAssetEditorSubsystem>().
#include "Editor.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Handlers/ErrorCodes.h"
#include "PinWrightSubsystem.h"
#include "State/JobRegistry.h"

namespace EditorQuitPolicy
{

// How recently another client must have called for this editor to count as in
// use. Sized against the failure it exists to stop: an editor was reaped 49 s
// after its last RPC by a caller that believed it was orphaned, and an agent
// idles minutes between tool calls while it reads a capture or thinks. Costing a
// legitimate reap a five-minute wait is the cheap side of that trade - an editor
// nobody is driving stays silent forever, so the window always elapses.
inline constexpr double InUseWindowSeconds = 300.0;

// The in-use decision. Pure: the caller supplies the ledger answer, this only
// weighs it. bForce is the caller's explicit override, and it wins outright -
// the guard is here to make a reap deliberate, not impossible.
inline bool ShouldRefuseAsInUse(bool bForce, bool bHasOtherClient, double SecondsAgo,
                                double WindowSeconds = InUseWindowSeconds)
{
    if (bForce || !bHasOtherClient)
    {
        return false;
    }
    // A non-positive window disables the guard rather than refusing everything -
    // the same discipline ModalStateProbe::ShouldReportGameThreadStalled uses, and
    // for the same reason: a misconfigured threshold must fail open, not wedge the
    // only way out of the editor.
    if (WindowSeconds <= 0.0)
    {
        return false;
    }
    return SecondsAgo < WindowSeconds;
}

struct FAssetEditorCloseResult
{
    // How many asset editors were open when quit committed to exiting.
    int32 OpenCount = 0;
    // How many were still open afterwards. Non-zero means a toolkit refused to
    // close; reported, not fatal (see CloseOpenAssetEditors).
    int32 RemainingCount = 0;
};

// Closes every open asset editor and reports what it found. Call this on the
// game thread, on the committed-to-exit path only, AFTER the dirty-package
// disposition and after PIE has stopped.
//
// Why it exists: an asset editor left open at exit is torn down by Slate from
// inside FEngineLoop::Exit(), long after the editor subsystems are gone, and the
// engine's toolkit destructors do not survive that. FStaticMeshEditor's runs
//
//     GEditor->GetEditorSubsystem<UImportSubsystem>()->OnAssetReimport.RemoveAll(this);
//
// unguarded (StaticMeshEditor.cpp:271 on 5.8) - a null subsystem there faults on
// a small member offset, which is the EXCEPTION_ACCESS_VIOLATION reading 0x78
// seen under FEngineLoop::Exit -> FSlateApplication::Shutdown ->
// SStandaloneAssetEditorToolkitHost::ShutdownToolkitHost -> ~FStaticMeshEditor.
// The engine bug is not ours to fix and cannot be guarded from outside; the
// precondition is. Closing while the subsystems are still alive runs those same
// destructors on a healthy editor, which is the identical ordering argument the
// PIE guard in EditorQuitHandler.cpp makes.
//
// Best-effort by design: a toolkit that refuses to close is counted and reported,
// not turned into a refusal to exit. Unlike a live PIE session the crash here is
// likely rather than certain, and an editor that could never be shut down would
// be the worse failure. The dirty-package disposition has already run by this
// point, so there is nothing left for a toolkit to prompt about.
inline FAssetEditorCloseResult CloseOpenAssetEditors()
{
    FAssetEditorCloseResult Result;
    if (!GEditor)
    {
        return Result;
    }
    UAssetEditorSubsystem* Subsystem = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();
    if (!Subsystem)
    {
        return Result;
    }

    Result.OpenCount = Subsystem->GetAllEditedAssets().Num();
    if (Result.OpenCount > 0)
    {
        Subsystem->CloseAllAssetEditors();
        Result.RemainingCount = Subsystem->GetAllEditedAssets().Num();
    }
    return Result;
}

// Ends every job still "running" when quit commits to exiting, and returns their
// ticket ids. A job with a cancel hook is cancelled (its work is stopped, e.g. an
// isolated run_tests child process tree); one without is failed with
// EDITOR_EXITING. Either way the ticket turns terminal and fires its job event
// while the transport is still up, so a client streaming it gets a final answer
// instead of blocking on an editor that is gone. Each ticket is logged so a crash
// later in teardown can be read against what was still live.
inline TArray<FString> TerminateRunningJobs(FJobRegistry& Registry)
{
    TArray<FString> Terminated;
    for (const FJobTicket& Ticket : Registry.List())
    {
        if (Ticket.Status != TEXT("running"))
        {
            continue;
        }
        UE_LOG(LogPinWrightSubsystem, Warning,
            TEXT("editor.quit: job %s (%s) still running at exit, terminating it"),
            *Ticket.TicketId, *Ticket.Method);
        if (Registry.Cancel(Ticket.TicketId) == EJobCancelResult::Unsupported)
        {
            Registry.Complete(Ticket.TicketId, false, nullptr, ErrorCodes::ERR_EDITOR_EXITING);
        }
        Terminated.Add(Ticket.TicketId);
    }
    return Terminated;
}

}
