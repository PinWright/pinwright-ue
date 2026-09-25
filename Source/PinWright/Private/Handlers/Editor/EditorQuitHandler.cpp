// Copyright (c) 2026 Alexander Penkin. MIT License.

// EditorQuitHandler.cpp - editor.quit
// Gracefully requests the editor process to exit so an automation loop can
// stop the editor via MCP instead of force-killing the OS process.
//
// Guards against silent data loss: if any user content/map package is dirty
// the caller must explicitly opt into discarding (discard:true) or saving
// (save:true). Without one of those the handler refuses to exit and reports
// UNSAVED_CHANGES instead, so the editor never silently drops or silently
// writes the user's unsaved work.
//
// Guards against ending someone else's session: this verb stops the process for
// EVERY client of it, and until the in-use check existed it weighed nothing but
// unsaved work. A second agent shut down an editor that had served four RPCs for
// a first agent minutes earlier, believing it orphaned. Recent traffic from a
// different client is now a refusal (EDITOR_IN_USE) that force:true clears - the
// evidence lives in State/ClientActivity.h, the decision in EditorQuitPolicy.h.
//
// Also guards against a hard shutdown crash, twice, for the same reason. Both
// are the engine tearing an editor UI down from inside FEngineLoop::Exit(), past
// the point where the subsystems those destructors reach for still exist, and in
// both cases the fix is ordering rather than a guard at the fault:
//
//   - PIE live at exit -> EXCEPTION_ACCESS_VIOLATION (0x60).
//     UEditorEngine::EndPlayMap() is driven from FAssetEditorViewportLayout's
//     destructor *during* FSlateApplication::Shutdown(), and its
//     CloseAllEditorsForAsset() call (PlayLevel.cpp:437) reaches into a
//     UAssetEditorSubsystem that shutdown already tore down. End PIE first, wait
//     for the session to be really gone, only then request exit.
//   - An asset editor open at exit -> EXCEPTION_ACCESS_VIOLATION (0x78) in
//     ~FStaticMeshEditor (StaticMeshEditor.cpp:271). Close them first; see
//     EditorQuitPolicy::CloseOpenAssetEditors for the full trace.

#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/ParamSpec.h"
#include "Handlers/ErrorCodes.h"
#include "Handlers/Editor/EditorQuitPolicy.h"
#include "State/ClientActivity.h"
#include "State/PluginState.h"
#include "Dom/JsonObject.h"

#include "CoreGlobals.h"
#include "Containers/Ticker.h"
#include "HAL/PlatformTime.h"

// GEditor->IsPlaySessionInProgress() / RequestEndPlayMap() / ShouldEndPlayMap().
#include "Editor.h"

// FEditorFileUtils::GetDirtyContentPackages / GetDirtyWorldPackages / SaveDirtyPackages.
#include "FileHelpers.h"
// UPackage::GetName() / UPackage::SetDirtyFlag().
#include "UObject/Package.h"

// Number of dirty package names embedded inline in the UNSAVED_CHANGES message
// text. The full list is always carried in the structured result payload; this
// cap only bounds the human-readable summary string.
static const int32 GEditorQuitMaxNamesInMessage = 5;

// Schedules the deferred engine exit. The one-shot ticker (returns false) fires
// after a ~1s delay, giving the async transport time to flush the already-sent
// response before RequestEngineExit() starts tearing the engine down. Reason is
// captured by value so it outlives this handler invocation.
static void EditorQuit_ScheduleDeferredExit(const FString& Reason)
{
  FTSTicker::GetCoreTicker().AddTicker(
      FTickerDelegate::CreateLambda([Reason](float /*DeltaTime*/) -> bool {
        RequestEngineExit(Reason);
        return false;
      }),
      1.0f);
}

// Terminates the jobs still running at exit (EditorQuitPolicy::TerminateRunningJobs)
// and reports their tickets as jobsTerminated.
static void EditorQuit_TerminateRunningJobs(FJsonObject& Resp)
{
  TArray<TSharedPtr<FJsonValue>> Ids;
  for (const FString& Id : EditorQuitPolicy::TerminateRunningJobs(FPluginState::Get().GetJobRegistry()))
  {
    Ids.Add(MakeShared<FJsonValueString>(Id));
  }
  Resp.SetArrayField(TEXT("jobsTerminated"), Ids);
}

// Poll cadence and budget for the pre-exit PIE teardown. Ending PIE is asynchronous:
// RequestEndPlayMap() only queues the request and the engine runs EndPlayMap() on a
// later editor tick, so the exit cannot be issued from the same tick.
static const float GEditorQuitPieStopPollSeconds = 0.1f;
static const double GEditorQuitPieStopTimeoutSeconds = 15.0;

// Polls until the PIE session is fully torn down, then sends the held response and
// schedules the deferred exit. On timeout it fails loudly (PIE_STOP_FAILED) and does
// NOT exit: exiting with PIE live is the known shutdown crash, so a session that will
// not stop must surface as a visible error instead of a crash after a success reply.
static void EditorQuit_WaitForPieStopThenExit(const TSharedRef<FAsyncResponseToken>& Token,
    const FString& Reason, const TSharedPtr<FJsonObject>& Resp)
{
  const double Deadline = FPlatformTime::Seconds() + GEditorQuitPieStopTimeoutSeconds;
  FTSTicker::GetCoreTicker().AddTicker(
      FTickerDelegate::CreateLambda([Token, Reason, Resp, Deadline](float /*DeltaTime*/) -> bool {
        if (GEditor && GEditor->IsPlaySessionInProgress())
        {
          // A session that was still only *queued* when quit arrived starts on a later
          // tick, and RequestEndPlayMap() no-ops while PlayWorld is null — so re-issue
          // it once the session is live and no end request is pending yet.
          if (GEditor->PlayWorld && !GEditor->ShouldEndPlayMap())
          {
            GEditor->RequestEndPlayMap();
          }
          if (FPlatformTime::Seconds() < Deadline)
          {
            return true; // Keep polling.
          }
          Resp->SetBoolField(TEXT("pieStopped"), false);
          Token->SendError(TEXT("PIE_STOP_FAILED"),
              FString::Printf(
                  TEXT("PIE session still active %.0fs after RequestEndPlayMap(); editor will NOT exit ")
                  TEXT("(requesting exit with PIE live crashes the editor during shutdown). ")
                  TEXT("Stop PIE (editor.stop) and re-call editor.quit."),
                  GEditorQuitPieStopTimeoutSeconds),
              Resp);
          return false;
        }

        Resp->SetBoolField(TEXT("pieStopped"), true);
        const EditorQuitPolicy::FAssetEditorCloseResult Closed =
            EditorQuitPolicy::CloseOpenAssetEditors();
        Resp->SetNumberField(TEXT("assetEditorsClosed"), Closed.OpenCount);
        Resp->SetNumberField(TEXT("assetEditorsRemaining"), Closed.RemainingCount);
        EditorQuit_TerminateRunningJobs(*Resp);
        Token->SendSuccess(Resp);
        EditorQuit_ScheduleDeferredExit(Reason);
        return false;
      }),
      GEditorQuitPieStopPollSeconds);
}

// ---- editor.quit ----
REGISTER_RPC_HANDLER("editor.quit", "editor",
    "Gracefully request the editor to exit. This editor is shared: if another client has driven it within the last 5 minutes, refuses with EDITOR_IN_USE naming that client's last method and how long ago, unless force:true is passed - an editor nobody is driving falls silent, so a genuinely abandoned one stops refusing once the window elapses. If user content/map packages are dirty, refuses with UNSAVED_CHANGES unless save:true (save first) or discard:true (exit without saving) is passed. An active Play-In-Editor session is ended first and waited out (exiting with PIE live crashes the editor during Slate shutdown); the response is held until the session is gone and reports pieWasActive/pieStopped, or fails with PIE_STOP_FAILED (no exit) if PIE will not stop within 15s. Every open asset editor is then closed (one left open crashes the editor in its own destructor during Slate shutdown) and counted as assetEditorsClosed/assetEditorsRemaining. Every job still running is then ended - cancelled if it has a cancel hook, otherwise failed with EDITOR_EXITING - so streaming clients get a terminal event before the process goes, and its tickets are listed as jobsTerminated. Acks once clear, then exits ~1s later so the async response flushes before the engine tears down.",
    RPC_PARAMS(
        RPC_PARAM_OPT("reason", "string", "Optional reason string recorded in the exit log."),
        RPC_PARAM_OPT("save", "boolean", "If true, save all dirty content/map packages (unattended, no prompt) before exiting."),
        RPC_PARAM_OPT("discard", "boolean", "If true, exit without saving by clearing the dirty flags on all dirty packages."),
        RPC_PARAM_OPT("force", "boolean", "If true, exit even when another client has driven this editor recently (clears EDITOR_IN_USE only; never overrides UNSAVED_CHANGES).")
    ))
{
  const FString Reason = Ctx.GetString(TEXT("reason"), TEXT("mcp.editor.quit"));
  const bool bSave = Ctx.GetBool(TEXT("save"), false);
  const bool bDiscard = Ctx.GetBool(TEXT("discard"), false);
  const bool bForce = Ctx.GetBool(TEXT("force"), false);

  // 0) In use by someone else? Checked BEFORE the dirty-package pass on purpose:
  // it is an objection to WHO is exiting, not to what would be lost, and telling
  // a stranger's caller to re-send with discard:true would answer the wrong
  // question. The caller's own traffic never counts - it is excluded by id, so a
  // client closing the editor it has been using is not obstructed and needs no
  // force flag.
  {
    const FString CallerId = ClientActivity::GetCallerId();
    ClientActivity::FActivity Other;
    const bool bHasOther = ClientActivity::GetMostRecentOtherClient(CallerId, Other);
    if (EditorQuitPolicy::ShouldRefuseAsInUse(bForce, bHasOther, Other.SecondsAgo))
    {
      TSharedPtr<FJsonObject> ErrData = MakeShared<FJsonObject>();
      ErrData->SetStringField(TEXT("lastMethod"), Other.Method);
      ErrData->SetNumberField(TEXT("lastSecondsAgo"), Other.SecondsAgo);
      ErrData->SetStringField(TEXT("lastClient"), Other.ClientId);
      ErrData->SetNumberField(TEXT("windowSeconds"), EditorQuitPolicy::InUseWindowSeconds);

      Ctx.SendError(ErrorCodes::ERR_EDITOR_IN_USE,
          FString::Printf(
              TEXT("Another client is using this editor: it served '%s' for a different client ")
              TEXT("%.0fs ago (in-use window %.0fs). Exiting would end that session mid-work. ")
              TEXT("Wait for the editor to go quiet, or re-call editor.quit with force:true if ")
              TEXT("you are certain that client is gone."),
              *Other.Method, Other.SecondsAgo, EditorQuitPolicy::InUseWindowSeconds),
          ErrData);
      return true; // No exit scheduled.
    }
  }

  // 1) Collect the dirty, savable user packages (content + maps) that would be
  // lost on exit, merged into a unique list. These FEditorFileUtils helpers
  // *append* to the array, so a single output array accumulates both queries;
  // AddUnique de-dups in case a package is reported by both.
  TArray<UPackage*> DirtyPackages;
  TArray<UPackage*> WorldPackages;
  FEditorFileUtils::GetDirtyContentPackages(DirtyPackages);
  FEditorFileUtils::GetDirtyWorldPackages(WorldPackages);
  for (UPackage* Pkg : WorldPackages)
  {
    DirtyPackages.AddUnique(Pkg);
  }

  TArray<FString> DirtyNames;
  DirtyNames.Reserve(DirtyPackages.Num());
  for (const UPackage* Pkg : DirtyPackages)
  {
    if (Pkg)
    {
      DirtyNames.Add(Pkg->GetName());
    }
  }
  const int32 DirtyCount = DirtyNames.Num();

  // 2) Dirty work exists but the caller gave no disposition -> refuse to exit.
  if (DirtyCount > 0 && !bSave && !bDiscard)
  {
    // Inline a short summary in the message text (capped) and carry the full
    // list plus count in the structured result payload (SendError supports a
    // data object overload: SendError(code, message, result)).
    const int32 NamesShown = FMath::Min(DirtyCount, GEditorQuitMaxNamesInMessage);
    FString NameSummary;
    for (int32 i = 0; i < NamesShown; ++i)
    {
      if (i > 0)
      {
        NameSummary += TEXT(", ");
      }
      NameSummary += DirtyNames[i];
    }
    if (DirtyCount > NamesShown)
    {
      NameSummary += FString::Printf(TEXT(", ... (+%d more)"), DirtyCount - NamesShown);
    }

    const FString Message = FString::Printf(
        TEXT("%d package(s) have unsaved changes and would be lost on exit (%s). ")
        TEXT("Re-call editor.quit with discard:true to exit without saving, or save:true to save first."),
        DirtyCount, *NameSummary);

    TArray<TSharedPtr<FJsonValue>> NamesArray;
    NamesArray.Reserve(DirtyCount);
    for (const FString& Name : DirtyNames)
    {
      NamesArray.Add(MakeShared<FJsonValueString>(Name));
    }
    TSharedPtr<FJsonObject> ErrData = MakeShared<FJsonObject>();
    ErrData->SetNumberField(TEXT("dirtyCount"), DirtyCount);
    ErrData->SetArrayField(TEXT("dirtyPackages"), NamesArray);

    Ctx.SendError(TEXT("UNSAVED_CHANGES"), Message, ErrData);
    return true; // No exit scheduled.
  }

  // 3) Proceed to exit. Apply the requested disposition first.
  bool bSaved = false;
  bool bDiscarded = false;

  if (bSave && DirtyCount > 0)
  {
    // Unattended save: bPromptUserToSave=false saves all dirty packages without
    // any UI prompt. Save both map and content packages.
    const bool bSaveOk = FEditorFileUtils::SaveDirtyPackages(
        /*bPromptUserToSave=*/false,
        /*bSaveMapPackages=*/true,
        /*bSaveContentPackages=*/true);
    if (!bSaveOk)
    {
      TSharedPtr<FJsonObject> ErrData = MakeShared<FJsonObject>();
      ErrData->SetNumberField(TEXT("dirtyCount"), DirtyCount);
      Ctx.SendError(TEXT("SAVE_FAILED"),
          FString::Printf(TEXT("Failed to save %d dirty package(s); editor will not exit."), DirtyCount),
          ErrData);
      return true; // Do NOT exit on save failure.
    }
    bSaved = true;
  }
  else if (bDiscard && DirtyCount > 0)
  {
    // Discard without saving: clear each package's dirty flag so the engine
    // shutdown path does not attempt to re-save or prompt for these packages.
    for (UPackage* Pkg : DirtyPackages)
    {
      if (Pkg)
      {
        Pkg->SetDirtyFlag(false);
      }
    }
    bDiscarded = true;
  }

  TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
  Resp->SetBoolField(TEXT("requested"), true);
  Resp->SetStringField(TEXT("reason"), Reason);
  Resp->SetBoolField(TEXT("saved"), bSaved);
  Resp->SetBoolField(TEXT("discarded"), bDiscarded);
  Resp->SetNumberField(TEXT("dirtyCount"), DirtyCount);

  // 4) End any live PIE session BEFORE requesting exit (see the teardown-ordering
  // note at the top of this file). IsPlaySessionInProgress() also covers a session
  // that is merely queued to start on the next tick, which would otherwise come up
  // mid-shutdown. The response is held until the session is really gone so the
  // caller learns whether the stop succeeded rather than getting a success that is
  // followed by a crash.
  const bool bPieWasActive = GEditor && GEditor->IsPlaySessionInProgress();
  Resp->SetBoolField(TEXT("pieWasActive"), bPieWasActive);

  if (bPieWasActive)
  {
    GEditor->RequestEndPlayMap();
    EditorQuit_WaitForPieStopThenExit(Ctx.MakeAsyncToken(), Reason, Resp);
    return true;
  }

  // No PIE: close any open asset editors while the editor subsystems their
  // destructors reach for are still alive, then send the ack FIRST so the HTTP
  // response can flush before teardown begins, then schedule the deferred exit.
  Resp->SetBoolField(TEXT("pieStopped"), false);
  const EditorQuitPolicy::FAssetEditorCloseResult Closed =
      EditorQuitPolicy::CloseOpenAssetEditors();
  Resp->SetNumberField(TEXT("assetEditorsClosed"), Closed.OpenCount);
  Resp->SetNumberField(TEXT("assetEditorsRemaining"), Closed.RemainingCount);
  EditorQuit_TerminateRunningJobs(*Resp);
  Ctx.SendSuccess(Resp);

  EditorQuit_ScheduleDeferredExit(Reason);
  return true;
}
