// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Math/Box2D.h"
#include "Math/Vector2D.h"
#include "Misc/DateTime.h"
#include "Misc/Optional.h"

// Shared data contract for the "drive" capability: an agent-facing surface for
// observing and acting on live UI (game UMG, editor chrome, embedded web). These
// are plain value types with no editor/Slate/CEF dependency; serialization to and
// from JSON lives in DriveJson.h/.cpp. Geometry mirrors FLiveUiSnapshotRuntimeState
// (absolute screen-space position + size as FVector2D).

// Which UI surface a drive observation or element belongs to. Auto means the
// resolver picks the surface; the others name a specific one.
enum class EDriveSurface : uint8
{
    Auto,
    Game,
    EditorChrome,
    Web
};

// A single addressable UI element in a drive observation.
struct FDriveElement
{
    // Stable addressing handle used to re-target this element across ticks.
    FString Handle;
    // Widget type string (e.g. "Button", "SButton").
    FString Type;
    // Human-readable label (button text, etc.) when resolvable.
    FString Label;
    // Live typed value of an editable widget (SEditableText / SMultiLineEditableText and their
    // *Box variants), read via GetText(); empty for non-editable widgets. Kept distinct from
    // Label because Label falls back to the accessible/HINT text for an editable field, so a
    // field showing only its placeholder has an empty Value but a non-empty (hint) Label. The
    // text_equals/text_contains conditions verify against this when present.
    FString Value;
    // EFFECTIVE state, folded down the ancestor chain, not the widget's own Slate attribute:
    // a widget under a disabled or Collapsed/Hidden ancestor reads false here even though its
    // own IsEnabled()/GetVisibility() say otherwise. bVisible means "is drawn", so a
    // SelfHitTestInvisible label is visible.
    bool bEnabled = true;
    bool bVisible = true;
    bool bFocused = false;
    // True for actionable widgets like buttons/inputs; false for static text/labels included only for verification.
    bool bInteractable = false;
    // Geometry in DESKTOP pixels - the owning window's screen origin is included, so this is
    // directly the space FDriveInput injects into and the space an external OS-level injector
    // needs. Mirrors FLiveUiSnapshotRuntimeState. Serialized with its space named explicitly
    // (geometry.space) so a caller never has to infer it. One exception exists and drive cannot
    // currently detect it: content under a retained SRetainerWidget (URetainerBox) is painted
    // into an SVirtualWindow rooted at (0,0), so its geometry is retainer-local, not desktop.
    FVector2D AbsolutePosition = FVector2D::ZeroVector;
    FVector2D AbsoluteSize = FVector2D::ZeroVector;
    // True when the element is not being arranged, i.e. it or an ancestor is Collapsed/Hidden;
    // the rect above is then zeroed rather than carrying the last frame that DID draw it, and
    // the element is never a valid click target.
    //
    // This is derived as !bVisible and is therefore a SUFFICIENT, not a complete, staleness
    // test. It does NOT cover a widget that is effectively visible but has no current rect:
    // one that has never been painted (freshly added, or its window never drawn), or one
    // culled out of the paint pass this frame. Those still report bGeometryStale == false with
    // whatever geometry the widget last stored - zero for the never-painted case. Detecting
    // them needs a per-frame arrangement stamp Slate does not expose publicly.
    bool bGeometryStale = false;
    // Optional widget/slate path string.
    FString Path;
    // Surface this element belongs to.
    EDriveSurface Surface = EDriveSurface::Auto;
    // Optional Set-of-Mark index; unset when this element carries no mark.
    TOptional<int32> MarkIndex;
};

// Optional screenshot attached to a drive observation, with the Set-of-Mark
// overlay state (which marks were actually painted versus omitted).
struct FDriveScreenshot
{
    // Base64-encoded image bytes. Populated in the default inline delivery mode; empty when
    // the screenshot was written to a file (Path set) so the payload stays small.
    FString Base64;
    // On-disk path to the written PNG, populated only in file delivery mode
    // (screenshot_mode=file); empty in the default inline mode. Mutually exclusive with
    // Base64 so an observation carries the image exactly one way.
    FString Path;
    // MIME type, e.g. "image/png".
    FString Mime;
    int32 Width = 0;
    int32 Height = 0;
    // Mark indices actually drawn on the image.
    TArray<int32> MarksDrawn;
    // Mark indices intentionally omitted (offscreen, too small, etc.).
    TArray<int32> MarksOmitted;
};

// One key/value property pair on a journal event.
struct FDriveJournalProp
{
    FString Key;
    FString Value;
};

// A single journal event surfaced to the editor module. This is the
// editor-module-facing shape; a later PinWrightRecorder-backed handler maps the
// recorder's own structs into this contract. Do NOT depend on PinWrightRecorder
// from this layer.
struct FDriveJournalEvent
{
    FString Id;
    // Event timestamp in seconds (matches the recorder's double Ts convention).
    double Ts = 0.0;
    FString Domain;
    FString Severity;
    FString Name;
    TArray<FDriveJournalProp> Props;
};

// A changed-variable entry in a journal delta.
struct FDriveJournalVariable
{
    FString Name;
    FString Value;
};

// Journal accumulated since a cursor: newly emitted events plus changed
// variables. Cursor advances monotonically so a caller can request the next delta.
struct FDriveJournalDelta
{
    TArray<FDriveJournalEvent> Events;
    TArray<FDriveJournalVariable> ChangedVariables;
    int64 Cursor = 0;
};

// A single observation of a surface: the elements present plus optional
// screenshot and journal payloads, stamped with a frame counter and capture time.
struct FDriveObservation
{
    EDriveSurface Surface = EDriveSurface::Auto;
    TArray<FDriveElement> Elements;
    TOptional<FDriveScreenshot> Screenshot;
    TOptional<FDriveJournalDelta> Journal;
    // Monotonic frame counter at capture time.
    int64 Frame = 0;
    // Wall-clock capture time (serialized as ISO-8601).
    FDateTime Timestamp;
    // The resolved root the observation was taken from (live UMG/Slate root name).
    FString RootName;
    // Count of elements dropped by a max_elements cap on an explicit observe (0 when
    // nothing was truncated). Serialized as `omitted_count` only when > 0 so callers
    // always learn that the list was shortened (no silent truncation).
    int32 OmittedCount = 0;
};

// The kind of check a drive condition evaluates.
enum class EDriveConditionType : uint8
{
    WidgetPresent,
    WidgetAbsent,
    WidgetEnabled,
    WidgetVisible,
    TextEquals,
    TextContains,
    Count,
    GeometryInBounds,
    JournalEvent,
    JournalSeverity
};

// Comparison operator applied to the Count condition.
enum class EDriveCompareOp : uint8
{
    Equal,
    NotEqual,
    Less,
    LessOrEqual,
    Greater,
    GreaterOrEqual
};

// A condition to evaluate against a surface (e.g. an explicit wait_for). Only the
// fields relevant to Type are meaningful; the rest carry defaults.
struct FDriveCondition
{
    EDriveConditionType Type = EDriveConditionType::WidgetPresent;
    // Widget name (widget/text/count/geometry checks) or event name (journal checks).
    FString Target;
    // Expected text for TextEquals / TextContains.
    FString ExpectedText;
    // Expected count for Count, compared with CountOp.
    int32 ExpectedCount = 0;
    EDriveCompareOp CountOp = EDriveCompareOp::Equal;
    // Expected screen-space bounds for GeometryInBounds.
    FBox2D ExpectedBounds = FBox2D(ForceInit);
    // Severity threshold for JournalSeverity (e.g. "error").
    FString SeverityThreshold;
};

// The outcome of evaluating a single condition.
struct FDriveConditionResult
{
    bool bMet = false;
    FString Actual;
    FString Expected;
    FString Detail;
};

// How a settle/wait operation terminated.
enum class EDriveSettleOutcome : uint8
{
    // Still in progress (no terminal state reached).
    Continue,
    // The UI changed then went stable for the required number of ticks.
    SettledChanged,
    // No change observed within the quiet budget.
    NoChangeWithinBudget,
    // The explicit wait_for condition was met.
    WaitForMet,
    // A budget elapsed before any terminal condition was reached.
    Timeout
};

// Tuning for a settle/wait operation. Budgets are in milliseconds.
struct FDriveSettleConfig
{
    // Consecutive stable ticks required before the UI is considered settled.
    int32 StableTicks = 2;
    // Budget for the quiet phase (no change), in ms.
    int32 QuietBudgetMs = 500;
    // Budget for the overall settle phase, in ms.
    int32 SettleBudgetMs = 1500;
    // Timeout for an explicit wait_for condition, in ms.
    int32 WaitForTimeoutMs = 5000;
    // Optional explicit wait_for condition.
    TOptional<FDriveCondition> WaitFor;
};

// The result of a settle/wait operation.
struct FDriveSettleResult
{
    bool bChanged = false;
    bool bSettled = false;
    bool bConditionMet = false;
    int32 ElapsedMs = 0;
    int32 Ticks = 0;
    EDriveSettleOutcome Outcome = EDriveSettleOutcome::Continue;
};
