// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "GenericPlatform/GenericWindowDefinition.h"
#include "Misc/Optional.h"
#include "Handlers/Drive/DriveTypes.h"
#include "Handlers/Drive/DriveLiveResolver.h"

class SWidget;
class SWindow;

// Editor-chrome backend for the "drive" capability: addresses the editor's own live Slate
// across all top-level windows (the main frame, asset editors, dialogs, and the transient
// popup/menu windows that open on click). Unlike the game UMG surface (FDriveLiveResolver),
// the editor's windows are always present during automation, so this unit observes/acts for
// real even headless. It deliberately reuses FDriveResolveResult / EDriveResolveStatus from
// DriveLiveResolver so a later integration step can dispatch surface=editor_chrome handles
// through the same result shape, and reuses FDriveLiveResolver::IsLikelyInteractable (a pure,
// public type-whitelist predicate) instead of re-porting it. The label / segment-name helpers
// are ported from the host project's UI-recording subsystem.

// One open top-level editor window, as surfaced by ListWindows so an agent can pick a target
// before observing it. Geometry is absolute screen-space (mirrors FDriveElement).
struct FDriveWindowInfo
{
    FString Title;
    // EWindowType rendered as a readable string: "Normal" / "Menu" / "ToolTip" / etc. An
    // opened menu or dropdown is its own transient "Menu"/"ToolTip" top-level window, so it
    // shows up here distinct from the window that spawned it.
    FString Type;
    FVector2D AbsolutePosition = FVector2D::ZeroVector;
    FVector2D AbsoluteSize = FVector2D::ZeroVector;
    // Index into the ordered visible-window list; the same value selects this window via
    // FDriveWindowSelector::Index.
    int32 Index = 0;
    // Native maximized state — the SAME signal editor.resize_window's WINDOW_MAXIMIZED gate
    // (SWindow::IsWindowMaximized) and editor.set_window_state's isMaximized readback use,
    // surfaced so list_windows (the discovery verb an agent hits first) agrees with those
    // control verbs instead of presenting a maximized window as plain. No minimized flag is
    // surfaced: the GetAllVisibleWindowsOrdered enumeration this list is built from excludes
    // minimized windows (SlateApplication filters IsVisible() && !IsWindowMinimized()), so a
    // minimized window never appears here and a per-record minimized bool could only ever read
    // false. The index-order objection that clause used to carry is ANSWERED and no longer a
    // reason not to: AppendMinimizedTolerantWindowCandidates below appends minimized windows
    // after the visible ones, so no window_index changes meaning. ListWindows still enumerates
    // visible-only (B-set-window-state-cannot-restore-minimized fixed the CONTROL verb, not the
    // discovery verb); restoring the field is that ticket's remaining half.
    bool bMaximized = false;
};

// Selects which top-level editor window a drive call addresses. Index takes precedence
// (positional, 0-based into GetAllVisibleWindowsOrdered); otherwise a non-empty Title is a
// case-insensitive substring match against the window title (first match wins). Both unset
// means the active top-level window (falling back to the first regular window, then the first
// visible window).
struct FDriveWindowSelector
{
    FString Title;
    TOptional<int32> Index;
};

// Static service that walks editor Slate. Every call re-walks the live tree (no caching) so
// an action handler can re-resolve a handle immediately before acting, exactly like
// FDriveLiveResolver does for the game surface.
class FDriveEditorChrome
{
public:
    // Enumerate the open top-level windows (via FSlateApplication::GetAllVisibleWindowsOrdered)
    // so an agent can pick one. Includes transient popup/menu windows. Returns an empty array
    // when Slate is not initialized.
    static TArray<FDriveWindowInfo> ListWindows();

    // Render an EWindowType with the same stable vocabulary used by drive.list_windows.
    static FString WindowTypeToString(EWindowType Type);

    // Walk the selected window's Slate tree and emit one FDriveElement per interactable OR
    // text/label-bearing widget (structural panels are skipped). Menu-bar entries are ordinary
    // widgets in the window tree and are included. Returns true and fills OutElements +
    // OutWindowTitle on success; on failure returns false with OutErrorCode / OutErrorMessage
    // set (SLATE_NOT_INITIALIZED / NO_WINDOWS / NO_ACTIVE_WINDOW / WINDOW_NOT_FOUND).
    static bool BuildElementList(
        const FDriveWindowSelector& Selector,
        TArray<FDriveElement>& OutElements,
        FString& OutWindowTitle,
        FString& OutErrorCode,
        FString& OutErrorMessage);

    // Re-walk the selected window NOW and resolve a single Handle (a window-rooted widget path)
    // to its current Slate widget and a fresh element snapshot. Never asserts: a window that
    // can't be resolved maps to NoLiveUi (with ErrorCode/ErrorMessage); a window that resolves
    // but carries no matching path maps to NotFound; a path that matches more than one widget
    // maps to Ambiguous.
    static FDriveResolveResult ResolveHandle(
        const FDriveWindowSelector& Selector,
        const FString& Handle);

    // Capture the selected window into a row-major FColor bitmap via
    // FSlateApplication::TakeScreenshot rooted at the window widget (alpha forced opaque), for
    // the later Set-of-Mark rendering of editor windows. Returns false with OutErrorCode set
    // (the window-resolution codes, or CAPTURE_FAILED) on failure. OutDesktopOrigin is the
    // window's desktop position, i.e. where the bitmap's (0,0) sits in element-geometry space.
    static bool CaptureWindow(
        const FDriveWindowSelector& Selector,
        TArray<FColor>& OutPixels,
        int32& OutWidth,
        int32& OutHeight,
        FVector2D& OutDesktopOrigin,
        FString& OutErrorCode);

    // Ordering rule for the minimized-tolerant candidate list, factored out of the resolver so
    // it is unit-testable over a constructed window set instead of by minimizing a real editor
    // window. OutCandidates is InVisibleOrdered VERBATIM (so every window_index over the visible
    // enumeration keeps the exact value it had before minimized tolerance existed), followed by
    // each entry of InExtraWindows that is not already present, in enumeration order, deduped by
    // widget identity. Pure: touches no FSlateApplication state and shows/hides nothing.
    static void AppendMinimizedTolerantWindowCandidates(
        const TArray<TSharedRef<SWindow>>& InVisibleOrdered,
        const TArray<TSharedRef<SWindow>>& InExtraWindows,
        TArray<TSharedRef<SWindow>>& OutCandidates);

    // Resolve an FDriveWindowSelector to a single live top-level SWindow, using the exact
    // targeting rule every drive.* verb uses: Index takes precedence over Title (substring),
    // else the active top-level window (falling back to the first regular, then the first
    // visible window). Public so the editor.* window verbs (frame_graph / resize_window /
    // screenshot_window) target identically. Returns true and fills OutWindow + OutTitle on
    // success; on failure returns false with OutErrorCode / OutErrorMessage set
    // (SLATE_NOT_INITIALIZED / NO_WINDOWS / WINDOW_NOT_FOUND / NO_ACTIVE_WINDOW).
    //
    // bIncludeMinimizedWindows is OPT-IN and defaults to false, so every existing caller keeps
    // the visible-only candidate set it was written against. Pass true ONLY from a verb whose
    // job is the window's own window-manager state (editor.set_window_state): observing,
    // clicking, resizing or capturing a minimized window measures or acts on nothing, so those
    // verbs deliberately keep refusing it. When true, minimized top-level windows are APPENDED
    // after the visible ones (never interleaved), so no window_index changes meaning.
    static bool ResolveWindow(
        const FDriveWindowSelector& Selector,
        TSharedPtr<SWindow>& OutWindow,
        FString& OutTitle,
        FString& OutErrorCode,
        FString& OutErrorMessage,
        bool bIncludeMinimizedWindows = false);

    // Readable name for one path segment: UMG reflection metadata > SObjectWidget BP class name
    // > Slate type. Ported from the host project's UI-recording subsystem. Public so the
    // path/segment scheme can be unit-tested over synthetic widgets without a live window.
    static FString GetWidgetSegmentName(const TSharedPtr<SWidget>& Widget);

    // Window-rooted, sibling-indexed parent-chain path used as the stable element Handle for
    // editor chrome. Editor widgets carry no UMG names, so each segment is
    // "<SegmentName>[<childIndexInParent>]" (the index disambiguates same-typed siblings); the
    // top-level window root has no index suffix. Pure; testable over a synthetic widget tree.
    static FString BuildWidgetPath(const TSharedRef<SWidget>& Widget);
};
