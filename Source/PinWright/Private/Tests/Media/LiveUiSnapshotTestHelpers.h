// Copyright (c) 2026 Alexander Penkin. MIT License.

// Shared helpers for live UI snapshot tests. Extracted from the per-file anonymous
// namespaces in TestLiveUiSnapshotCapture.cpp and TestLiveUiSnapshotHandlers.cpp so that
// when Unity build merges both .cpp files into a single translation unit, identically
// named helpers don't collide at link/redefinition time.
#pragma once

#include "CoreMinimal.h"

namespace LiveUiSnapshotTestHelpers
{
    // Error codes a live-capture RPC may legitimately return when the editor isn't in
    // a state where a live UI snapshot can be taken (no PIE, no viewport, etc.). Tests
    // treat these as "skip" conditions rather than failures.
    inline bool IsAcceptedLiveCaptureFailureCode(const FString& ErrorCode)
    {
        return ErrorCode == TEXT("SLATE_NOT_INITIALIZED")
            || ErrorCode == TEXT("PIE_NOT_RUNNING")
            || ErrorCode == TEXT("GAME_VIEWPORT_NOT_FOUND")
            || ErrorCode == TEXT("LIVE_UI_NOT_FOUND")
            || ErrorCode == TEXT("AMBIGUOUS_LIVE_ROOT");
    }
}
