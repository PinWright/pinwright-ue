// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "CoreGlobals.h"
#include "PinWrightSettings.h"

// Automation mode: the process-global GIsRunningUnattendedScript, refcounted.
//
// It is the ONLY lever both engine dialog families consult, and it is the safe
// one. The two consumers:
//
//   Runtime/Core/Private/Misc/MessageDialog.cpp:157
//       if (!FApp::IsUnattended() && !GIsRunningUnattendedScript)  // else Result = DefaultValue
//   Runtime/Slate/Private/Framework/Application/SlateApplication.cpp:2134 (5.3 :1990)
//       if (GIsRunningUnattendedScript && !bSlowTaskWindow) { ...log...; return; }
//
// The Slate guard cancels the window outright on every supported version
// (5.3 :1990, 5.4 :2004, 5.5 :2030, 5.6 :2032, 5.7 :2098, 5.8 :2134), and
// GEditor->EditorAddModalWindow routes through it (EditorEngine.cpp:4172), so a
// raw Slate modal cannot deadlock the game thread while this is set. Both
// consumers also log the suppression, which is the "log loudly" half for free.
//
// NOT taken, deliberately: FCoreDelegates::ModalMessageDialog is a SINGLE-CAST
// delegate already bound by UEditorEngine::Init (EditorEngine.cpp:1291) and
// unconditionally Unbind()'d by Shutdown (:1664). Binding it from a plugin
// REPLACES the editor's own handler process-wide with no chain-and-forward
// pattern available. Never bind it.
//
// Caveat that survives: suppressed is not the same as safely answered.
// FMessageDialog::Open returns a documented DefaultValue; a cancelled raw Slate
// modal (SCustomDialog/SWindow ShowModal) returns its uninitialised widget-local
// value. Callers must re-read state after a mutating verb that could have hit
// one. See call("unattended").
//
// Refcounted rather than a bare TGuardValue because the interval that matters is
// "any PinWright work is outstanding", which nests: a naive `= false` on scope
// exit would punch a hole in an outer interval. Depth is game-thread-only
// (FRpcDispatcher enforces game-thread dispatch), so a plain int32 needs no
// atomics and no lock.
namespace PinWrightAutomationMode
{
namespace Detail
{
    // Function-local statics in inline functions have exactly one instance across
    // every translation unit, which keeps this header-only under Unity builds.
    inline int32& Depth()
    {
        static int32 GDepth = 0;
        return GDepth;
    }

    // False when the kill switch was off at the outermost Enter(); the whole
    // interval is then inert and restores nothing.
    inline bool& Engaged()
    {
        static bool bGEngaged = false;
        return bGEngaged;
    }

    // GIsRunningUnattendedScript as it was before the outermost Enter(), captured
    // and written back rather than assumed false.
    //
    // Note that -unattended does NOT set it: only -RUNNINGUNATTENDEDSCRIPT does
    // (LaunchEngineLoop.cpp:6858-6860); -unattended sets FApp::IsUnattended().
    // FMessageDialog checks both (MessageDialog.cpp:157) but
    // FSlateApplication::AddModalWindow checks only this flag
    // (SlateApplication.cpp:2134), so under a bare -unattended run a raw Slate
    // modal is suppressed only while an engaged interval is open.
    inline bool& SavedFlag()
    {
        static bool bGSaved = false;
        return bGSaved;
    }
}

// True while an engaged interval is open. Read by tests and by any transport
// code that wants to know whether a suppression was in force.
inline bool IsActive()
{
    return Detail::Depth() > 0 && Detail::Engaged();
}

// Open (or re-enter) the interval. Pair every Enter() with exactly one Leave().
inline void Enter()
{
    if (Detail::Depth()++ > 0)
    {
        return;
    }

    const UPinWrightSettings* Settings = GetDefault<UPinWrightSettings>();
    Detail::Engaged() = (Settings == nullptr) || Settings->bSuppressModalDialogsDuringRpc;
    if (!Detail::Engaged())
    {
        return;
    }

    Detail::SavedFlag() = GIsRunningUnattendedScript;
    GIsRunningUnattendedScript = true;
}

// Close one nesting level; the outermost Leave() restores the previous value.
inline void Leave()
{
    if (Detail::Depth() <= 0)
    {
        return;
    }
    if (--Detail::Depth() > 0)
    {
        return;
    }
    if (Detail::Engaged())
    {
        GIsRunningUnattendedScript = Detail::SavedFlag();
        Detail::Engaged() = false;
    }
}

// Tests only - the depth is a process-global latch.
inline void ResetForTests()
{
    if (Detail::Depth() > 0 && Detail::Engaged())
    {
        GIsRunningUnattendedScript = Detail::SavedFlag();
    }
    Detail::Depth() = 0;
    Detail::Engaged() = false;
}
}

// RAII wrapper placed around each synchronous handler invocation in
// FRpcDispatcher. Nesting inside an already-open interval is a no-op by
// construction (the refcount, not save/restore, owns the restore), so a handler
// that re-enters the dispatcher cannot end the outer interval early.
//
// Not automatically covered by this scope: async/ticket continuations that
// resume on a later tick outside Func(Ctx). Production continuations must reopen
// the scope explicitly. Current covered paths:
//
//   asset.fixup_redirectors  AsyncTask body (AssetWorkflowHandler.cpp) - reached
//       IAssetTools::FixupReferencers and ObjectTools::DeleteObjects. Now takes
//       its own scope, and the fix-up runs through Utils/RedirectorFixupPolicy.
//   asset.import             DeferRequestToSafePoint (AssetManageHandler.cpp) -
//       DeferToSafePoint reopens this scope around import, rename, verification,
//       and response while the dispatcher retains the active request.
//
// Remaining user-controlled escape:
//
//   python.execute           a user script may call
//       unreal.register_slate_post_tick_callback and open a dialog from its own
//       engine-side callback, outside any scope here.
//
// Whether the guard is open when a continuation lands depends on whether some
// OTHER RPC is in flight, so the failure mode is nondeterministic: scope shut ->
// the modal really opens and owns the game thread; scope open -> the modal is
// cancelled, and if its caller reads the result unchecked the editor asserts.
//
// A handler that adds a deferred continuation calling IAssetTools::CreateAsset /
// DuplicateAsset / RenameAssets / FixupReferencers / ObjectTools::Delete* /
// FEditorFileUtils::SaveDirtyPackages / PromptForCheckoutAndSave must route it
// through a safe-point helper that reopens this scope, or hold its own balanced
// PinWrightAutomationMode::Enter()/Leave() pair across that continuation.
struct FScopedUnattendedRpc
{
    FScopedUnattendedRpc()
    {
        PinWrightAutomationMode::Enter();
    }

    ~FScopedUnattendedRpc()
    {
        PinWrightAutomationMode::Leave();
    }

    FScopedUnattendedRpc(const FScopedUnattendedRpc&) = delete;
    FScopedUnattendedRpc& operator=(const FScopedUnattendedRpc&) = delete;
};
