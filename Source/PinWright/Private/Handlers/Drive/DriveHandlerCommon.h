// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Handlers/ParamSpec.h"                 // RPC_PARAM_OPT (DRIVE_WINDOW_SELECTOR_PARAMS)
#include "Handlers/Drive/DriveTypes.h"
#include "Handlers/Drive/DriveLiveResolver.h"  // FDriveRootSelector, FDriveResolveResult
#include "Handlers/Drive/DriveEditorChrome.h"  // FDriveWindowSelector

class FHandlerContext;

// Editor-chrome window-selector param descriptors, single-sourced so every drive.* verb
// whose body reads FDriveHandlerCommon::ParseWindowSelector (drive.observe, drive.expect,
// and the six action verbs via DRIVE_COMMON_ACTION_PARAMS) advertises the exact keys the
// parser consumes. Expands to a comma-separated FParamSpec list (no trailing comma) for use
// inside an RPC_PARAMS(...) / another param macro; the window block is never the last entry,
// so callers append a comma after it. Keeping the schema here next to ParseWindowSelector
// makes "the declared params match the parsed keys" a single edit point instead of three.
#define DRIVE_WINDOW_SELECTOR_PARAMS \
    RPC_PARAM_OPT("window_title", "string", "Editor-chrome window selector: substring-matched against a visible top-level window title (see drive.list_windows). Both selectors unset picks the active top-level window. Ignored on the game surface."), \
    RPC_PARAM_OPT("title", "string", "Alias for window_title."), \
    RPC_PARAM_OPT("window_index", "integer", "Editor-chrome window selector: the Nth visible top-level window (0-based, see drive.list_windows). Takes precedence over window_title. Ignored on the game surface."), \
    RPC_PARAM_OPT("index", "integer", "Alias for window_index.")

// Result of one journal delta query from the live tail, owned by PinWrightRecorder.
struct FLiveTailDelta;

// Shared orchestration helper for the drive.* RPC handlers. It is the one place that
// turns wire params into the drive value contract and dispatches surface providers,
// so the synchronous observe/expect/events_since handlers and the later async action
// verbs all build their observations through the same code path. The provider switch
// in GetElementsForSurface is the single extension point for new surfaces (only the
// game/PIE surface is wired in this phase; editor-chrome and web arrive later).
class FDriveHandlerCommon
{
public:
    // Parse the `surface` param (game | editor_chrome | web | auto). Auto - and any
    // unrecognized token - resolves to Game for v1, so callers always receive a concrete
    // surface and never have to handle Auto downstream.
    static EDriveSurface ResolveSurface(const FHandlerContext& Ctx);

    // Read the live-root selector from instance_name / root_index (and camelCase aliases).
    static FDriveRootSelector ParseRootSelector(const FHandlerContext& Ctx);

    // Read the editor-chrome window selector from title / window_title (substring match)
    // and window_index / index (positional, takes precedence). Both unset selects the
    // active top-level window. Only consulted for the EditorChrome surface; the game
    // surface keeps its FDriveRootSelector vocabulary (instance_name / root_index).
    static FDriveWindowSelector ParseWindowSelector(const FHandlerContext& Ctx);

    // Read the screenshot delivery mode from the `screenshot_mode` param: 'file' (any case)
    // means write the PNG out and return its path; anything else (default 'inline') keeps the
    // legacy inline-base64 delivery. Single-sources the wire literals for both the game
    // (drive.observe) and web (drive.observe web) observe paths.
    static bool ParseScreenshotToFile(const FHandlerContext& Ctx);

    // Build the element list for a resolved surface. Game routes to FDriveLiveResolver
    // (root Selector); EditorChrome routes to FDriveEditorChrome (window WindowSelector,
    // its window title mapped to OutRootName); Web/Auto return false with
    // SURFACE_NOT_SUPPORTED. The WindowSelector defaults to the active window so the
    // game-only callers (which never pass it) keep their exact behavior. Add a new
    // provider by extending this one switch.
    static bool GetElementsForSurface(
        EDriveSurface Surface,
        const FDriveRootSelector& Selector,
        TArray<FDriveElement>& OutElements,
        FString& OutRootName,
        FString& OutErrorCode,
        FString& OutErrorMessage,
        const FDriveWindowSelector& WindowSelector = FDriveWindowSelector());

    // Surface-aware single-handle resolve. Game re-walks the live UMG root selector
    // (FDriveLiveResolver::ResolveHandle); EditorChrome re-walks the selected editor
    // window (FDriveEditorChrome::ResolveHandle). Both selectors are parsed from Ctx so
    // the action flow has one resolve entry point regardless of surface. Web/Auto yield
    // a NoLiveUi result coded SURFACE_NOT_SUPPORTED.
    static FDriveResolveResult ResolveForSurface(
        EDriveSurface Surface,
        const FHandlerContext& Ctx,
        const FString& Handle);

    // Map a PinWrightRecorder live-tail delta into the editor-facing drive contract
    // (lowercase domain/severity strings, stringified values, cursor passthrough).
    static FDriveJournalDelta MapJournalDelta(const FLiveTailDelta& Delta);

    // Query the active session's live tail since a cursor. Returns false and leaves Out
    // empty (cursor 0) when no recording session/PIE is active - this is the clean
    // "nothing to report" path, not an error.
    static bool GetJournalDelta(uint64 SinceCursor, FDriveJournalDelta& Out);

    // Approximate upper-bound estimate of one element's serialized JSON size, in characters (the
    // unit the spill threshold also measures; see FDriveJson::WriteElement). The variable cost is
    // the four string fields (handle/type/label/value); the four bools, the geometry object, an
    // optional mark, the JSON keys, quotes and punctuation are covered by a fixed ceiling.
    // Deliberately over-counts for the common ASCII-dominant case; heavy non-ASCII/escaped label
    // or value text (a JSON \uXXXX escape is 6 chars per source char) can in the worst case exceed
    // it, so treat the result as a budgeting heuristic, not a hard guarantee.
    static int32 EstimateElementJsonBytes(const FDriveElement& Element);

    // Compact an observation's element list in place for an explicit drive.observe:
    // first drop non-interactable elements when bInteractablesOnly, then cap the array at
    // MaxElements (0 = unlimited), then cap the total serialized element size at MaxBytes
    // (0 = unlimited) via EstimateElementJsonBytes. The byte cap exists because each element's
    // handle is the full widget-tree path, so bInteractablesOnly/MaxElements bound element COUNT
    // but not payload BYTES; the byte cap keeps a scan/discovery observe inline instead of
    // spilling. At least one element is always kept. OutOmittedCount reports how many either cap
    // dropped (0 when none) so truncation is never silent. The default args (false, 0, 0) are a
    // no-op, so the settle loop / diff baseline callers that fetch elements never filter.
    static void FilterObservationElements(
        TArray<FDriveElement>& Elements,
        bool bInteractablesOnly,
        int32 MaxElements,
        int32 MaxBytes,
        int32& OutOmittedCount);

    // Assemble a full observation: elements (required), an optional Set-of-Mark
    // screenshot, and an optional journal delta, stamped with the current frame and
    // UTC time. Returns false (with OutErrorCode/OutErrorMessage) only when the element
    // list fails; a screenshot capture failure leaves the screenshot unset and continues.
    // bInteractablesOnly / MaxElements compact the element list (see
    // FilterObservationElements) for explicit reads; both default to off so action/wait
    // observations keep the full list. The Set-of-Mark screenshot is taken after the
    // filter, so its marks match the returned elements, and Out.OmittedCount carries any
    // max_elements truncation. When bScreenshotToFile is set the screenshot is written to a
    // file and delivered as Out.Screenshot.Path (no inline base64); default off keeps the
    // legacy inline base64 delivery. MaxBytes (0 = unlimited) additionally caps the total
    // serialized element size so a verbose observe stays inline (see FilterObservationElements).
    static bool BuildObservation(
        EDriveSurface Surface,
        const FDriveRootSelector& Selector,
        bool bScreenshot,
        int32 MarkCap,
        bool bIncludeJournal,
        uint64 JournalSince,
        FDriveObservation& Out,
        FString& OutErrorCode,
        FString& OutErrorMessage,
        const FDriveWindowSelector& WindowSelector = FDriveWindowSelector(),
        bool bInteractablesOnly = false,
        int32 MaxElements = 0,
        bool bScreenshotToFile = false,
        int32 MaxBytes = 0);
};
