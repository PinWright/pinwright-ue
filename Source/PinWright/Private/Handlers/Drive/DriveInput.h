// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "InputCoreTypes.h"
#include "Math/Vector2D.h"
#include "Misc/EnumClassFlags.h"

// Synthetic-input service for the drive capability. Injects mouse and keyboard
// input through FSlateApplication at resolved screen-space points (the same
// absolute pixel space as a widget's AbsolutePosition in FDriveElement). Ported
// from the host project's proven UI-recording injection path, this centralizes the two things
// ad-hoc callers get wrong: resolving the native window under the point (so
// clicks land even when the editor is not the active OS window) and enabling
// device input while the application is not active. Action handlers call these
// and never touch FSlateApplication directly.
//
// All coordinates are absolute screen pixels. Every method is a no-op that
// returns false when Slate is not initialized.

// Forward declarations keep heavy Slate/ApplicationCore headers out of this file.
class FSlateApplication;
class FGenericWindow;
class FModifierKeysState;
class SWindow;

// Mouse button to inject.
enum class EDriveMouseButton : uint8
{
    Left,
    Right,
    Middle
};

// What a key injection does: a full press (down then up) or one edge only.
enum class EDriveKeyAction : uint8
{
    Press,
    Down,
    Up
};

// Modifier keys to hold for a key injection. Bit flags; combine with '|'.
enum class EDriveModifierKeys : uint8
{
    None  = 0,
    Shift = 1 << 0,
    Ctrl  = 1 << 1,
    Alt   = 1 << 2,
    Cmd   = 1 << 3
};
ENUM_CLASS_FLAGS(EDriveModifierKeys);

// Result of mapping a single character to the physical key that types it.
struct FDriveCharKeyMapping
{
    // Physical key to press (e.g. 'A' for both 'a' and 'A'); invalid for
    // characters with no US-layout physical key (those route via the char event
    // only).
    FKey Key;
    // Whether Shift must be held to produce this character on a US layout.
    bool bShift = false;
};

class FDriveInput
{
public:
    // Move the cursor to ScreenPos, then inject a button down + up there,
    // resolving the native window under the point first.
    static bool ClickAt(const FVector2D& ScreenPos, EDriveMouseButton Button = EDriveMouseButton::Left);

    // Same injection as ClickAt, but additionally reports whether a widget under
    // the point CONSUMED the button-press — the honest "the click landed on an
    // interactive target" signal callers need instead of assuming success. Sets
    // bOutPressHandled to the handled state Slate returned for the button-down
    // (false when the point resolves to empty space / no window). Returns false
    // only when Slate is not initialized (the click could not be injected at
    // all); a successful injection returns true regardless of bOutPressHandled.
    static bool ClickAtReportingHandled(const FVector2D& ScreenPos, EDriveMouseButton Button, bool& bOutPressHandled);

    // Dispatch a real mouse-move to ScreenPos so hover enter/leave fire (unlike a
    // bare SetCursorPos). HoverAt is an alias for the same operation.
    static bool MoveTo(const FVector2D& ScreenPos);
    static bool HoverAt(const FVector2D& ScreenPos) { return MoveTo(ScreenPos); }

    // Press at From, emit interpolated moves spread across DurationMs with the
    // button held, then release at To. DurationMs <= 0 uses a default duration.
    static bool DragFromTo(const FVector2D& From, const FVector2D& To, int32 DurationMs);

    // Inject a mouse-wheel event at ScreenPos. Positive Delta scrolls up.
    static bool ScrollAt(const FVector2D& ScreenPos, float Delta);

    // Inject a key event for Key with the given modifiers held. Action selects a
    // full press or a single down/up edge.
    static bool PressKey(const FKey& Key, EDriveModifierKeys Modifiers = EDriveModifierKeys::None,
        EDriveKeyAction Action = EDriveKeyAction::Press);

    // Same injection as PressKey, but additionally reports whether Slate CONSUMED the
    // event — the honest "something on the focus path handled it" signal, the keyboard
    // counterpart of ClickAtReportingHandled. bOutHandled is the OR of the down/up
    // handled results for a full press. Returns false only when the event could not be
    // injected at all (Slate not initialized, or an invalid key).
    static bool PressKeyReportingHandled(const FKey& Key, EDriveModifierKeys Modifiers,
        EDriveKeyAction Action, bool& bOutHandled);

    // Device-aware form for callers that already resolved Slate, the receiving device, and
    // Slate user. It owns the same modifier/key-code construction, inactive-input scope, and
    // down/up dispatch as the convenience overload without reacquiring global Slate state.
    static bool PressKeyReportingHandled(FSlateApplication& SlateApp, const FKey& Key,
        EDriveModifierKeys Modifiers, EDriveKeyAction Action,
        FInputDeviceId InputDevice, uint32 SlateUserIndex, bool& bOutHandled);

    // Type Text one character at a time (key down, character event, key up per
    // char), holding Shift for characters that need it on a US layout.
    static bool TypeString(const FString& Text);

    // The top-level window a pointer event at ScreenPos is routed to right now: the same
    // FSlateApplication::LocateWindowUnderMouse call ProcessMouseButtonDownEvent, the mouse-move
    // and the wheel paths make, so it predicts where an injection lands. That call trusts the
    // platform's window-under-cursor first, and on Linux that cache follows a cursor warp only
    // once SDL's enter/leave event is pumped on a later frame, so right after a warp into another
    // window it can still name the previous one. Null when no window takes input at the point.
    static TSharedPtr<SWindow> WindowUnderPoint(const FVector2D& ScreenPos);

    // ---- Pure helpers (no Slate dependency; unit-tested) ----

    // Interpolated move points for a drag from From to To over DurationMs. The
    // returned array excludes From (the press point) and its last element equals
    // To. Points advance monotonically from From toward To. DurationMs <= 0 uses
    // the same default as DragFromTo.
    static TArray<FVector2D> ComputeDragStepPoints(const FVector2D& From, const FVector2D& To, int32 DurationMs);

    // Maps a character to the physical key + shift state that types it on a US
    // QWERTY layout. Key is invalid for unmapped characters.
    static FDriveCharKeyMapping MapCharToKey(TCHAR Char);

    // Parses a button token ("left" | "right" | "middle", case-insensitive) into
    // the matching mouse button; any unrecognized token (including empty) maps to
    // Left. Centralizes the button-token contract in the class that owns
    // EDriveMouseButton so click handlers share one parse instead of re-typing it.
    static EDriveMouseButton ParseMouseButton(const FString& Token);

private:
    // Moves the OS cursor and dispatches a real ProcessMouseMoveEvent. Does not
    // manage the unfocused-input flag; callers own that scope.
    static void DispatchMouseMove(FSlateApplication& SlateApp, const FVector2D& ScreenPos);

    // Native window under ScreenPos via LocateWindowUnderMouse over the
    // interactive top-level windows; null when none (e.g. point fully offscreen).
    static TSharedPtr<FGenericWindow> ResolveNativeWindowUnder(FSlateApplication& SlateApp, const FVector2D& ScreenPos);

    // FModifierKeysState with the requested modifiers reported as their left-hand
    // variants.
    static FModifierKeysState MakeModifierState(EDriveModifierKeys Modifiers);

    // EKeys mouse-button constant for a drive button.
    static FKey MouseButtonToKey(EDriveMouseButton Button);
};
