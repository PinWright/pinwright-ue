// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"

// Editorial overlay loader for the wiki. Reads per-namespace markdown files at
// <plugin>/docs/wiki-src/<dotted-path>.md and extracts:
//   - the prelude body (everything between the H1 / leading HTML comments and the
//     first `### ` heading), used for branch / leaf-namespace / hybrid pages.
//   - per-method H3 sections keyed by the full method name, used for method pages.
//
// Files are cached by absolute path; cache entries are invalidated when the file
// mtime changes, so editing an overlay file picks up on the next call without an
// editor restart.

namespace WikiOverlay
{
    // Returns the prelude body for the overlay file at <plugin>/docs/wiki-src/<Namespace>.md.
    // - The file's leading H1 line (`# <ns>`) and any leading HTML comments / blank lines
    //   are stripped.
    // - Reading stops at the first `### ` heading.
    // - Trailing whitespace is trimmed.
    // - Missing file or empty body returns an empty string.
    PINWRIGHT_API FString LoadGroupPrelude(const FString& Namespace);

    // Returns the body of the H3 section whose heading line is exactly
    // `### <FullMethodName>` (case-sensitive; trailing whitespace tolerated).
    // The enclosing file is derived from FullMethodName by stripping the trailing
    // leaf segment to yield the Category, then reading <plugin>/docs/wiki-src/<Category>.md.
    // - Reading stops at the next `### ` heading or end-of-file.
    // - Missing file, missing section, or empty body returns an empty string.
    PINWRIGHT_API FString LoadMethodSection(const FString& FullMethodName);
}
