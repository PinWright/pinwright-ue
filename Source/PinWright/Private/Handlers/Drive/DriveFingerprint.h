// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Handlers/Drive/DriveTypes.h"

// Pure-logic UI fingerprint, change detection, and diff for the drive capability.
// No Slate/CEF/UObject dependency: everything here operates on plain TArray<FDriveElement>
// value types so it can be unit-tested without a live editor. The fingerprint is a cheap
// "does the UI look the same?" (shape-only) signal that the settle loop polls each tick; the
// diff is the richer one-shot per-handle change summary (shape plus editable value) attached
// to an action/observation.

// A cheap, stable digest of a UI surface's visible structure. Two fingerprints compare
// equal when the visible element set has the same types, the same whole-pixel rects, and
// the same visible count. Handles, labels, focus, and enabled state are intentionally NOT
// part of the fingerprint - it answers "did the UI change shape?", not "did addressing or
// content change?". Cheap to compute and compare (one uint64 + one int32).
struct FDriveFingerprint
{
    // 64-bit FNV-1a digest over each visible element's Type + whole-pixel rect, taken in a
    // normalized (sorted) order so reordering the input array alone does not change it.
    uint64 Hash = 0;

    // Number of visible elements that contributed to the hash. Folded into Hash as well, but
    // kept as a separate field so callers can read the count and so the equality check is exact.
    int32 VisibleCount = 0;

    bool operator==(const FDriveFingerprint& Other) const
    {
        return Hash == Other.Hash && VisibleCount == Other.VisibleCount;
    }

    bool operator!=(const FDriveFingerprint& Other) const
    {
        return !(*this == Other);
    }
};

// A per-handle diff between two element sets, keyed by Handle. Each array holds the handles
// in a category, sorted lexicographically for deterministic output regardless of input order.
// Feeds an action/observation change summary. Catches structural changes (type, geometry,
// visibility) AND editable-value changes (the one-shot-vs-per-tick settle rationale is in the
// file-top comment above).
struct FDriveDiff
{
    // Handles present in Current but not in Previous.
    TArray<FString> Appeared;

    // Handles present in Previous but not in Current.
    TArray<FString> Disappeared;

    // Handles present in both whose Type, visibility, whole-pixel rect, or live editable
    // Value changed (covers "moved", "reshaped", and "retyped" - e.g. a drive.type edit).
    TArray<FString> Changed;

    bool IsEmpty() const
    {
        return Appeared.Num() == 0 && Disappeared.Num() == 0 && Changed.Num() == 0;
    }
};

// Stateless helpers that compute fingerprints and diffs from element sets. All methods are
// deterministic: identical input always yields identical output.
class FDriveChangeDetector
{
public:
    // Computes the fingerprint of a UI surface from its elements. Only elements with
    // bVisible == true contribute. Elements are sorted by a stable key (Type, then rounded
    // x/y/w/h) before hashing, so two arrays that differ only in element order produce the
    // same fingerprint. Sub-pixel jitter is removed by rounding each rect component to the
    // nearest whole pixel before it enters the hash.
    static FDriveFingerprint Compute(const TArray<FDriveElement>& Elements);

    // Equality/compare helper for two fingerprints (same as operator==).
    static bool Equals(const FDriveFingerprint& A, const FDriveFingerprint& B);

    // Per-handle diff between a previous and a current element set. An element is considered
    // "changed" when its Type, bVisible flag, whole-pixel rect, or live editable Value differs
    // across the two sets. Handles are assumed unique within each set; on a duplicate handle
    // the last occurrence wins. Output arrays are sorted lexicographically by handle.
    static FDriveDiff Diff(const TArray<FDriveElement>& Previous, const TArray<FDriveElement>& Current);
};
