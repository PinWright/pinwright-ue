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

#if WITH_DEV_AUTOMATION_TESTS
    PINWRIGHT_API bool IsTraceExportWorkerQuiescentForTests();
#endif
}
