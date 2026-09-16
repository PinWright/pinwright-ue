// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Misc/Optional.h"
#include "Handlers/Drive/DriveTypes.h"

class SWidget;

// Live Slate/UMG I/O for the drive capability: walks the in-PIE UMG tree NOW and
// produces FDriveElement value types for the pure fingerprint/condition units to
// consume elsewhere. This is the only drive unit that touches live Slate; everything
// downstream operates on the TArray<FDriveElement> it emits. Root resolution mirrors
// FLiveUiSnapshot (instance_name / root_index selector against the live UMG roots in
// the game viewport window) and reuses FLiveUiSnapshotService::SelectRootCandidate so
// the multi-root ambiguity errors match the snapshot RPCs exactly.

// Root selector for the live walk. Same semantics as FLiveUiSnapshotRequest's
// instance_name / root_index: substring match against the backing widget name, then a
// positional fallback. Both unset means "the single root" (ambiguous if more than one).
struct FDriveRootSelector
{
    // Substring-matched against each live UMG root's backing widget name.
    FString InstanceName;
    // Positional selector: the Nth live root (0-based). Takes precedence over InstanceName.
    TOptional<int32> RootIndex;
};

// How ResolveHandle terminated.
enum class EDriveResolveStatus : uint8
{
    // The handle resolved to exactly one live widget; Widget and Element are valid.
    Found,
    // The root resolved but no live element carried the requested handle.
    NotFound,
    // The root selector was ambiguous (multiple live roots, no/loose selector).
    Ambiguous,
    // No live UI to walk (Slate down, no PIE, no viewport, no root candidate, or a
    // selector that matched nothing). ErrorCode/ErrorMessage carry the specific reason.
    NoLiveUi
};

// Outcome of re-resolving a single handle against the live tree.
struct FDriveResolveResult
{
    EDriveResolveStatus Status = EDriveResolveStatus::NotFound;
    // The live Slate widget the handle resolved to (only when Status == Found).
    TSharedPtr<SWidget> Widget;
    // A fresh element snapshot (geometry + state captured at resolve time, Status == Found).
    FDriveElement Element;
    // Mirrors the LiveUiSnapshot error vocabulary for the non-Found root-resolution failures
    // (SLATE_NOT_INITIALIZED, PIE_NOT_RUNNING, GAME_VIEWPORT_NOT_FOUND, LIVE_UI_NOT_FOUND,
    // AMBIGUOUS_LIVE_ROOT, LIVE_ROOT_NOT_FOUND). Empty for Found / plain NotFound.
    FString ErrorCode;
    FString ErrorMessage;
};

// Focused live-UMG service: an element-list builder and a single-handle resolver. Both
// re-walk the live tree on every call (no caching) so action handlers can re-resolve a
// handle immediately before acting.
class FDriveLiveResolver
{
public:
    // Walk the selected live UMG root and emit one FDriveElement per interactable OR
    // text/label-bearing widget (structural panels are skipped). Returns true and fills
    // OutElements + OutRootName on success. On failure returns false and sets
    // OutErrorCode/OutErrorMessage using the LiveUiSnapshot error vocabulary; callers may
    // treat the "no capturable UI" codes as a skip the same way the snapshot tests do.
    static bool BuildElementList(
        const FDriveRootSelector& Selector,
        TArray<FDriveElement>& OutElements,
        FString& OutRootName,
        FString& OutErrorCode,
        FString& OutErrorMessage);

    // Re-walk the live tree NOW and resolve a single handle to its current Slate widget
    // and a fresh element snapshot. Never asserts: every failure mode maps to a status
    // (NotFound / Ambiguous / NoLiveUi) with ErrorCode/ErrorMessage populated for the
    // root-resolution failures.
    static FDriveResolveResult ResolveHandle(
        const FDriveRootSelector& Selector,
        const FString& Handle);

    // Pure interactability predicate (type whitelist OR keyboard-focus support), ported
    // from the host project's UI-recording subsystem. Exposed so it can be unit-tested
    // over synthetic widgets without a live PIE viewport.
    static bool IsLikelyInteractable(const TSharedRef<SWidget>& Widget);

    // Test seam for the handle scheme. Runs the exact AssignHandles core the live walk uses
    // (named-ancestor chain + leaf type, with "[k]" disambiguation for collisions), driven by
    // explicit name maps instead of a live backing map. Each leaf's real GetParentWidget()
    // chain is walked up to and including RootBoundary; an ancestor contributes a chain segment
    // iff it is a key in AnchorNames, and a leaf keeps its own short name iff it is a key in
    // LeafNames. Returns one handle per leaf, in the same order as Leaves. Exposed so the
    // uniqueness / disambiguation / round-trip behavior can be asserted without a PIE viewport.
    static TArray<FString> BuildHandlesForTest(
        const TArray<TSharedRef<SWidget>>& Leaves,
        const SWidget* RootBoundary,
        const TMap<const SWidget*, FString>& LeafNames,
        const TMap<const SWidget*, FString>& AnchorNames);
};
