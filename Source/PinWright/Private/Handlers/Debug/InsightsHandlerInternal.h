// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once
#include "CoreMinimal.h"

// Test-visible helper: returns the active trace destination string when a trace
// is connected, or an empty string otherwise. Wraps FTraceAuxiliary so tests can
// link to a stable symbol without pulling in the registration translation unit.
namespace PinWrightRpc::Insights
{
    PINWRIGHT_API FString ResolveActiveTracePath();

    // Test-visible helper: writes a trace snapshot and reports the resolved path.
    // Returns the FTraceAuxiliary::WriteSnapshot bool. On return OutResolvedPath
    // holds the absolutized written path, and bOutResolvedFromEngine is true only
    // when the engine itself resolved the destination (vs. echoing RequestedFilePath).
    // See InsightsHandler.cpp for how the path is recovered from OnSnapshotSaved.
    PINWRIGHT_API bool WriteSnapshotResolvingPath(const FString& RequestedFilePath, FString& OutResolvedPath, bool& bOutResolvedFromEngine);

    // FTraceAuxiliary::Start only queues the connection and TraceLog's worker thread adopts it on its
    // next update; until then Stop is refused (Writer_Stop returns false while a pending handle is
    // set), so a stop issued right after a start is silently lost and the trace stays open. Retries
    // Stop for up to TimeoutSeconds. Returns true when a stop is in effect (accepted now, or an
    // earlier stop's close is pending), false when nothing is connected or the stop never took.
    PINWRIGHT_API bool RequestTraceStop(double TimeoutSeconds);

    // An accepted stop only queues the close; the worker thread completes it a couple of updates
    // later, and until then IsConnected() stays true and a new Start logs "Unable to start trace,
    // already tracing to" at Error. Waits up to TimeoutSeconds; returns !IsConnected().
    PINWRIGHT_API bool WaitForTraceClose(double TimeoutSeconds);

#if WITH_DEV_AUTOMATION_TESTS
    PINWRIGHT_API bool IsTraceExportWorkerQuiescentForTests();
#endif
}
