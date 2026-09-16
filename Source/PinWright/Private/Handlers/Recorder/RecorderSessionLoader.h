// Copyright (c) 2026 Alexander Penkin. MIT License.

// Loads a journal NDJSON file into an in-memory FSessionModel.
//
// Reuses the gateway's ExtractTopLevelJsonObjects to split the file into per-line
// objects (tolerant of a crash-truncated tail: parsing stops cleanly on the first
// malformed line). Parsed sessions are cached by absolute-path + mtime so the
// describe -> summarize -> series flow parses each file once.
#pragma once

#include "CoreMinimal.h"
#include "RecorderSessionModel.h"

namespace RecorderSessionLoader
{

// Parse a file into a session model, serving a cached copy when the file is
// unchanged since the last load. Returns null and fills OutError on failure
// (file missing / unreadable / no header line).
TSharedPtr<RecorderModel::FSessionModel> Load(const FString& AbsolutePath, FString& OutError);

// Parse NDJSON content already in memory (used by tests and the live path).
// SessionId labels the model when the header omits one.
TSharedPtr<RecorderModel::FSessionModel> ParseContent(const FString& Content);

// Drop all cached sessions. Test-only seam to keep cases independent.
void ClearCache();

} // namespace RecorderSessionLoader
