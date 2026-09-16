// Copyright (c) 2026 Alexander Penkin. MIT License.

// PythonCallbackRegistry.h - the record of every tick/shutdown callback a Python script
// registered through this plugin, shared by python.execute (which reports how many its
// slot left behind) and python.callbacks (which lists and clears them).
//
// Why a shim and not an engine query. Registration goes through
// PySlate.cpp's RegisterSlateTickCallback, which appends the FDelegateHandle to
// PySlateUtil::PythonPreTickCallbackHandles / PythonPostTickCallbackHandles - file-scope
// statics in a Private engine module with no accessor of any kind. FSlateApplication's
// OnPreTick()/OnPostTick() events are reachable but a multicast delegate cannot say which
// of its subscribers came from Python, and clearing them would take out the engine's own.
// So the only place a Python-registered callback can be observed is at the moment Python
// registers it, which is what Content/Python/pinwright_callbacks.py wraps.
//
// Why the records live in C++ rather than in that module. Two readers need them at moments
// when running Python is the wrong thing to do: the EndPIE warning fires during PIE
// teardown, and python.execute reports its own leak count on a stack that has just returned
// from the interpreter. A C++ read costs nothing and cannot re-enter the interpreter
// (board B-python-execute-reentrant-gc-crash is the shape being avoided). The Python module
// keeps only what C++ cannot hold: the engine handle objects, which are the sole route back
// to unregister_slate_post_tick_callback.

#pragma once

#include "CoreMinimal.h"
#include "Misc/DateTime.h"

PINWRIGHT_API DECLARE_LOG_CATEGORY_EXTERN(LogPinWrightPythonCallbacks, Log, All);

namespace PinWright::PythonCallbacks
{
    struct FRecord
    {
        // Plugin-assigned, stable for the life of the registration. Also the key the
        // Python module files its handle under.
        FString Id;
        // Monotonic allocation order behind Id. Only FScopedRequestSlot reads it, to tell
        // the callbacks one call registered from ones an earlier call left behind.
        int32 Serial = 0;
        // slate_post_tick | slate_pre_tick | python_shutdown
        FString Kind;
        // RPC request id of the python.execute call that registered it; empty when the
        // registration came from outside a tracked call (an editor startup script, say).
        FString RequestId;
        // "<file>:<line> in <function>" of the registering frame.
        FString Source;
        FDateTime RegisteredAt;
        int64 Invocations = 0;
        // Traceback of the most recent invocation that raised; empty while it is healthy.
        FString LastError;
    };

    enum class EReadyStatus : uint8
    {
        Ready,
        PythonNotAvailable,
        PythonInitFailed,
        ShimInstallFailed,
    };

    // Resolves and initializes the Python interpreter if needed, then installs the tracking
    // shim once per session. Cheap and idempotent after the first successful call.
    //
    // A FAILED install is remembered and not retried: the install runs a Python import, so
    // retrying it from every python.execute would write a traceback per script on a host
    // where it cannot work. Pass bRetryFailedInstall from an explicit user-facing call
    // (python.callbacks) so someone who fixed the cause has a route back without an editor
    // restart - which is the outcome this whole file exists to remove.
    PINWRIGHT_API EReadyStatus EnsureTrackingReady(bool bRetryFailedInstall = false);

    // When the shim was installed. Callbacks registered before this instant are invisible
    // to every function here, which is why the verb reports it.
    PINWRIGHT_API FDateTime GetTrackingInstalledAt();

    // Reported by the Python shim through UPinWrightPythonCallbackLibrary.
    PINWRIGHT_API FString NotifyRegistered(const FString& Kind, const FString& Source);
    PINWRIGHT_API void NotifyUnregistered(const FString& Id);
    PINWRIGHT_API void NotifyInvoked(const FString& Id, const FString& ErrorText);

    // True while a record for Id is still held. The shim's re-adoption pass reads it so a
    // reload re-files only what this registry actually lost.
    PINWRIGHT_API bool IsTracked(const FString& Id);

    PINWRIGHT_API TArray<FRecord> Snapshot();

    // Asks the Python shim to unregister the given ids. Returns false only when the script
    // itself could not run; which ids actually went away is decided by re-reading Snapshot,
    // never by the script's own report.
    PINWRIGHT_API bool RequestClear(const TArray<FString>& Ids);

    // Attributes registrations made on this stack to one RPC request, and counts what that
    // stack left behind. Counting by allocation watermark rather than by request id is what
    // makes the count exact: two calls can carry the same client-supplied request id, and a
    // callback the script registered and then unregistered itself must not be reported.
    class PINWRIGHT_API FScopedRequestSlot
    {
    public:
        explicit FScopedRequestSlot(const FString& RequestId);
        ~FScopedRequestSlot();

        FScopedRequestSlot(const FScopedRequestSlot&) = delete;
        FScopedRequestSlot& operator=(const FScopedRequestSlot&) = delete;

        // Callbacks registered since this guard was constructed that are still registered.
        int32 CountSurviving() const;

    private:
        FString Previous;
        int32 Watermark = 0;
    };

    // Drops the EndPIE subscription and clears the records, so an unloaded DLL leaves no
    // binding on the engine's delegate and a reloaded one re-installs (which re-adopts the
    // callbacks still live in the interpreter) instead of short-circuiting on stale state.
    PINWRIGHT_API void Shutdown();
}
