// Copyright (c) 2026 Alexander Penkin. MIT License.

// PwSkelDiagnostic.h - Diagnostics owned by the .pwskel source format.
#pragma once

#include "CoreMinimal.h"

#include "PwSource/PwDiagnostic.h"

// Structural skeleton diagnostics. Lexical and generic source-shape failures stay in
// PwSourceDiagnosticCodes; they are not prefixed with the format that happened to emit them.
namespace PwSkelDiagnosticCodes
{
    // The file contains no top-level bone declaration.
    inline constexpr TCHAR PWSKEL_NO_BONES[] = TEXT("PWSKEL_NO_BONES");

    // Two or more top-level bones would create multiple roots in one USkeleton.
    inline constexpr TCHAR PWSKEL_MULTIPLE_ROOTS[] = TEXT("PWSKEL_MULTIPLE_ROOTS");

    // A bone name is declared more than once anywhere in the hierarchy.
    inline constexpr TCHAR PWSKEL_DUPLICATE_BONE[] = TEXT("PWSKEL_DUPLICATE_BONE");
    inline constexpr TCHAR PWSKEL_DUPLICATE_PREVIEW_MESH[] =
        TEXT("PWSKEL_DUPLICATE_PREVIEW_MESH");
    inline constexpr TCHAR PWSKEL_DUPLICATE_CURVE[] = TEXT("PWSKEL_DUPLICATE_CURVE");
    inline constexpr TCHAR PWSKEL_UNKNOWN_LINKED_BONE[] = TEXT("PWSKEL_UNKNOWN_LINKED_BONE");

    // The created UObject did not read back the hierarchy or metadata described by the source.
    inline constexpr TCHAR PWSKEL_ASSET_POSTCONDITION_FAILED[] =
        TEXT("PWSKEL_ASSET_POSTCONDITION_FAILED");

    // A scale axis is at or inside the engine's own "effectively zero" tolerance, so the
    // bone's reference-pose transform is singular: the subtree collapses onto one point and
    // geometry skinned through the chain has no volume.
    inline constexpr TCHAR PWSKEL_DEGENERATE_SCALE[] = TEXT("PWSKEL_DEGENERATE_SCALE");

    // A scale axis is negative, which negates the determinant of the bone's frame and flips
    // the handedness of the bone and every descendant. Legal and occasionally deliberate, so
    // it is a warning rather than an error.
    inline constexpr TCHAR PWSKEL_MIRRORED_SCALE[] = TEXT("PWSKEL_MIRRORED_SCALE");

}
