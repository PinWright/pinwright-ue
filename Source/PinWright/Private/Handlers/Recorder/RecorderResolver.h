// Copyright (c) 2026 Alexander Penkin. MIT License.

// Resolves a query's `session` argument to a recording file on disk and enumerates
// discoverable sessions.
//
// A `session` arg is either an existing absolute/relative path or a bare id under
// the recordings directory (Saved/PinWright/Recordings), with or without the .ndjson
// extension. File-only in v1 (no "live" in-editor querying).
#pragma once

#include "CoreMinimal.h"

namespace RecorderResolver
{

// One discoverable session on disk.
struct FSessionListing
{
    FString Id;
    FString Path;
    double ModifiedUtcSeconds = 0.0;
};

// Absolute path to the recordings directory (Saved/PinWright/Recordings).
FString RecordingsDir();

// Map a session argument to an on-disk absolute path. Returns empty when nothing
// resolves.
FString ResolvePath(const FString& Session);

// Enumerate every session file, newest first.
TArray<FSessionListing> ListSessions();

} // namespace RecorderResolver
