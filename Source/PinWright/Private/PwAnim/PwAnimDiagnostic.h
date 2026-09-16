// Copyright (c) 2026 Alexander Penkin. MIT License.

// PwAnimDiagnostic.h - Diagnostic codes owned by the .pwanim source format.
#pragma once

#include "CoreMinimal.h"

#include "PwSource/PwDiagnostic.h"

// Lexical, generic source-shape and parameter errors stay in PwSourceDiagnosticCodes.
// This registry contains only animation-format meaning: document structure, animation
// semantics and the format-specific skeleton-reference outcomes.
// There is intentionally no cycle diagnostic: `use skeleton` resolves one compiled asset,
// not another source document, and the grammar has no recursive reference edge.
namespace PwAnimDiagnosticCodes
{
    inline constexpr TCHAR PWANIM_WRONG_FORMAT[] = TEXT("PWANIM_WRONG_FORMAT");

    inline constexpr TCHAR PWANIM_MISSING_TIMEBASE[] = TEXT("PWANIM_MISSING_TIMEBASE");
    inline constexpr TCHAR PWANIM_DUPLICATE_TIMEBASE[] = TEXT("PWANIM_DUPLICATE_TIMEBASE");

    inline constexpr TCHAR PWANIM_MISSING_SKELETON[] = TEXT("PWANIM_MISSING_SKELETON");
    inline constexpr TCHAR PWANIM_DUPLICATE_SKELETON[] = TEXT("PWANIM_DUPLICATE_SKELETON");
    inline constexpr TCHAR PWANIM_UNSUPPORTED_USE_KIND[] = TEXT("PWANIM_UNSUPPORTED_USE_KIND");
    inline constexpr TCHAR PWANIM_SKELETON_NOT_AN_ASSET_PATH[] = TEXT("PWANIM_SKELETON_NOT_AN_ASSET_PATH");
    inline constexpr TCHAR PWANIM_SKELETON_NOT_FOUND[] = TEXT("PWANIM_SKELETON_NOT_FOUND");
    inline constexpr TCHAR PWANIM_SKELETON_WRONG_KIND[] = TEXT("PWANIM_SKELETON_WRONG_KIND");
    inline constexpr TCHAR PWANIM_SKELETON_HAS_NO_BONES[] = TEXT("PWANIM_SKELETON_HAS_NO_BONES");

    inline constexpr TCHAR PWANIM_NO_BONES[] = TEXT("PWANIM_NO_BONES");
    inline constexpr TCHAR PWANIM_EMPTY_BONE[] = TEXT("PWANIM_EMPTY_BONE");
    inline constexpr TCHAR PWANIM_DUPLICATE_BONE[] = TEXT("PWANIM_DUPLICATE_BONE");
    inline constexpr TCHAR PWANIM_DUPLICATE_KEY[] = TEXT("PWANIM_DUPLICATE_KEY");
    inline constexpr TCHAR PWANIM_KEYS_OUT_OF_ORDER[] = TEXT("PWANIM_KEYS_OUT_OF_ORDER");
    inline constexpr TCHAR PWANIM_TRAILING_EASE[] = TEXT("PWANIM_TRAILING_EASE");
    inline constexpr TCHAR PWANIM_LOOP_SEAM[] = TEXT("PWANIM_LOOP_SEAM");
    inline constexpr TCHAR PWANIM_UNKNOWN_BONE[] = TEXT("PWANIM_UNKNOWN_BONE");

    // The destination is owned by another source (or has no source stamp). The message names
    // the current source when one is stamped and tells the caller that overwrite is the explicit
    // takeover permission. This is not a malformed source value.
    inline constexpr TCHAR PWANIM_ASSET_PROVENANCE_CONFLICT[] =
        TEXT("PWANIM_ASSET_PROVENANCE_CONFLICT");

    // Warning. An in-place rebuild reuses the object so referencers survive, which also carries
    // over state bound to the OLD timeline or skeleton - sync markers, notifies, RetargetSource.
    // A shorter rebuild clamps every out-of-range marker to the new end, so the count is intact
    // and the data is not. The format cannot express any of it, so this reports rather than
    // repairs; it is a warning because the animation itself is correct.
}
