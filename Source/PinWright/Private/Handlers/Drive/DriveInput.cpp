// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Handlers/Drive/DriveInput.h"

#include "Framework/Application/SlateApplication.h"
#include "GenericPlatform/GenericApplication.h"
#include "GenericPlatform/GenericWindow.h"
#include "GenericPlatform/ICursor.h"
#include "InputCoreTypes.h"
#include "Layout/WidgetPath.h"
#include "Misc/ScopeExit.h"
#include "Widgets/SWindow.h"

// ────────────────────────────────────────────────────────────────────────────
// Pure helpers (no Slate dependency)
// ────────────────────────────────────────────────────────────────────────────

TArray<FVector2D> FDriveInput::ComputeDragStepPoints(const FVector2D& From, const FVector2D& To, int32 DurationMs)
{
    // Default duration matches the porting source (the host project's UI-recording
    // subsystem); one move per 60 Hz frame, with a floor so even instant drags emit a few moves.
    constexpr float DefaultDurationMs = 200.0f;
    constexpr float FrameMs = 1000.0f / 60.0f;

    const float EffectiveMs = (DurationMs > 0) ? static_cast<float>(DurationMs) : DefaultDurationMs;
    const int32 NumSteps = FMath::Max(5, FMath::CeilToInt(EffectiveMs / FrameMs));

    TArray<FVector2D> Points;
    Points.Reserve(NumSteps);
    for (int32 Step = 1; Step <= NumSteps; ++Step)
    {
        const double Alpha = static_cast<double>(Step) / static_cast<double>(NumSteps);
        Points.Add(FMath::Lerp(From, To, Alpha));
    }
    return Points;
}

FDriveCharKeyMapping FDriveInput::MapCharToKey(TCHAR Char)
{
    FDriveCharKeyMapping Mapping;

    // Letters: physical key is the uppercase letter (EKeys letter FNames are the
    // single uppercase letter); Shift only for uppercase.
    if ((Char >= TEXT('a') && Char <= TEXT('z')) || (Char >= TEXT('A') && Char <= TEXT('Z')))
    {
        const TCHAR Upper = FChar::ToUpper(Char);
        const FString Name = FString::Chr(Upper);
        Mapping.Key = FKey(*Name);
        Mapping.bShift = (Char >= TEXT('A') && Char <= TEXT('Z'));
        return Mapping;
    }

    // Plain digits map to the number-row keys, no Shift.
    if (Char >= TEXT('0') && Char <= TEXT('9'))
    {
        static const FKey Digits[10] = {
            EKeys::Zero, EKeys::One, EKeys::Two, EKeys::Three, EKeys::Four,
            EKeys::Five, EKeys::Six, EKeys::Seven, EKeys::Eight, EKeys::Nine };
        Mapping.Key = Digits[Char - TEXT('0')];
        return Mapping;
    }

    // Whitespace, punctuation, and shifted symbols. Shifted symbols share the
    // physical key of their unshifted counterpart (US layout).
    switch (Char)
    {
        case TEXT(' '):  Mapping.Key = EKeys::SpaceBar; break;
        case TEXT('\t'): Mapping.Key = EKeys::Tab;      break;
        case TEXT('\n'):
        case TEXT('\r'): Mapping.Key = EKeys::Enter;    break;

        // Shifted number-row symbols.
        case TEXT('!'): Mapping.Key = EKeys::One;   Mapping.bShift = true; break;
        case TEXT('@'): Mapping.Key = EKeys::Two;   Mapping.bShift = true; break;
        case TEXT('#'): Mapping.Key = EKeys::Three; Mapping.bShift = true; break;
        case TEXT('$'): Mapping.Key = EKeys::Four;  Mapping.bShift = true; break;
        case TEXT('%'): Mapping.Key = EKeys::Five;  Mapping.bShift = true; break;
        case TEXT('^'): Mapping.Key = EKeys::Six;   Mapping.bShift = true; break;
        case TEXT('&'): Mapping.Key = EKeys::Seven; Mapping.bShift = true; break;
        case TEXT('*'): Mapping.Key = EKeys::Eight; Mapping.bShift = true; break;
        case TEXT('('): Mapping.Key = EKeys::Nine;  Mapping.bShift = true; break;
        case TEXT(')'): Mapping.Key = EKeys::Zero;  Mapping.bShift = true; break;

        // Punctuation: unshifted then its shifted variant.
        case TEXT('`'):  Mapping.Key = EKeys::Tilde;                              break;
        case TEXT('~'):  Mapping.Key = EKeys::Tilde;        Mapping.bShift = true; break;
        case TEXT('-'):  Mapping.Key = EKeys::Hyphen;                             break;
        case TEXT('_'):  Mapping.Key = EKeys::Hyphen;       Mapping.bShift = true; break;
        case TEXT('='):  Mapping.Key = EKeys::Equals;                             break;
        case TEXT('+'):  Mapping.Key = EKeys::Equals;       Mapping.bShift = true; break;
        case TEXT('['):  Mapping.Key = EKeys::LeftBracket;                        break;
        case TEXT('{'):  Mapping.Key = EKeys::LeftBracket;  Mapping.bShift = true; break;
        case TEXT(']'):  Mapping.Key = EKeys::RightBracket;                       break;
        case TEXT('}'):  Mapping.Key = EKeys::RightBracket; Mapping.bShift = true; break;
        case TEXT('\\'): Mapping.Key = EKeys::Backslash;                          break;
        case TEXT('|'):  Mapping.Key = EKeys::Backslash;    Mapping.bShift = true; break;
        case TEXT(';'):  Mapping.Key = EKeys::Semicolon;                          break;
        case TEXT(':'):  Mapping.Key = EKeys::Semicolon;    Mapping.bShift = true; break;
        case TEXT('\''): Mapping.Key = EKeys::Apostrophe;                         break;
        case TEXT('"'):  Mapping.Key = EKeys::Apostrophe;   Mapping.bShift = true; break;
        case TEXT(','):  Mapping.Key = EKeys::Comma;                              break;
        case TEXT('<'):  Mapping.Key = EKeys::Comma;        Mapping.bShift = true; break;
        case TEXT('.'):  Mapping.Key = EKeys::Period;                             break;
        case TEXT('>'):  Mapping.Key = EKeys::Period;       Mapping.bShift = true; break;
        case TEXT('/'):  Mapping.Key = EKeys::Slash;                              break;
        case TEXT('?'):  Mapping.Key = EKeys::Slash;        Mapping.bShift = true; break;

        default: break; // Unmapped: Key stays invalid; caller routes via char event.
    }

    return Mapping;
}

// ────────────────────────────────────────────────────────────────────────────
// Private Slate helpers
// ────────────────────────────────────────────────────────────────────────────

void FDriveInput::DispatchMouseMove(FSlateApplication& SlateApp, const FVector2D& ScreenPos)
{
    const FVector2D LastPos(SlateApp.GetLastCursorPos());

    // Keep the OS cursor in sync so later GetCursorPos()/GetLastCursorPos() agree.
    if (TSharedPtr<ICursor> Cursor = SlateApp.GetPlatformCursor())
    {
        Cursor->SetPosition(FMath::RoundToInt(ScreenPos.X), FMath::RoundToInt(ScreenPos.Y));
    }

    // ProcessMouseMoveEvent directly: bypasses the IsFakingTouchEvents guard and
    // the LastPlatformCursorPosition dedup that silently drop synthetic moves.
    FPointerEvent MoveEvent(
        static_cast<uint32>(SlateApp.GetUserIndexForMouse()),
        FSlateApplication::CursorPointerIndex,
        ScreenPos,
        LastPos,
        SlateApp.GetPressedMouseButtons(),
        EKeys::Invalid,
        0.0f,
        FModifierKeysState());
    SlateApp.ProcessMouseMoveEvent(MoveEvent);
}

TSharedPtr<FGenericWindow> FDriveInput::ResolveNativeWindowUnder(FSlateApplication& SlateApp, const FVector2D& ScreenPos)
{
    // GetActiveTopLevelWindow() is null when the editor is unfocused, so resolve
    // the window from the point itself over the interactive top-level windows.
    FWidgetPath WidgetsUnderCursor = SlateApp.LocateWindowUnderMouse(
        ScreenPos, SlateApp.GetInteractiveTopLevelWindows());
    if (WidgetsUnderCursor.IsValid())
    {
        if (TSharedPtr<SWindow> Window = WidgetsUnderCursor.GetWindow())
        {
            return Window->GetNativeWindow();
        }
    }
    return nullptr;
}

FModifierKeysState FDriveInput::MakeModifierState(EDriveModifierKeys Modifiers)
{
    const bool bShift = EnumHasAnyFlags(Modifiers, EDriveModifierKeys::Shift);
    const bool bCtrl  = EnumHasAnyFlags(Modifiers, EDriveModifierKeys::Ctrl);
    const bool bAlt   = EnumHasAnyFlags(Modifiers, EDriveModifierKeys::Alt);
    const bool bCmd   = EnumHasAnyFlags(Modifiers, EDriveModifierKeys::Cmd);

    // Each held modifier is reported as its left-hand variant; right stays up.
    return FModifierKeysState(
        bShift, false,
        bCtrl,  false,
        bAlt,   false,
        bCmd,   false,
        /*bAreCapsLocked*/ false);
}

FKey FDriveInput::MouseButtonToKey(EDriveMouseButton Button)
{
    switch (Button)
    {
        case EDriveMouseButton::Right:  return EKeys::RightMouseButton;
        case EDriveMouseButton::Middle: return EKeys::MiddleMouseButton;
        case EDriveMouseButton::Left:
        default:                        return EKeys::LeftMouseButton;
    }
}

EDriveMouseButton FDriveInput::ParseMouseButton(const FString& Token)
{
    if (Token.Equals(TEXT("right"), ESearchCase::IgnoreCase))  { return EDriveMouseButton::Right; }
    if (Token.Equals(TEXT("middle"), ESearchCase::IgnoreCase)) { return EDriveMouseButton::Middle; }
    return EDriveMouseButton::Left;
}

// ────────────────────────────────────────────────────────────────────────────
// Mouse injection
// ────────────────────────────────────────────────────────────────────────────

bool FDriveInput::ClickAt(const FVector2D& ScreenPos, EDriveMouseButton Button)
{
    // Delegate to the handled-reporting variant; existing callers only care that
    // the click was injected (true when Slate is up), not whether a widget
    // consumed it, so the handled result is discarded here.
    bool bDiscardHandled = false;
    return ClickAtReportingHandled(ScreenPos, Button, bDiscardHandled);
}

bool FDriveInput::ClickAtReportingHandled(const FVector2D& ScreenPos, EDriveMouseButton Button, bool& bOutPressHandled)
{
    bOutPressHandled = false;

    if (!FSlateApplication::IsInitialized())
    {
        return false;
    }

    FSlateApplication& SlateApp = FSlateApplication::Get();

    // Allow input while the editor is not the active OS application; without this
    // ProcessReply skips mouse capture and the button-up never fires the click.
    const bool bPrevHandleInactive = SlateApp.GetHandleDeviceInputWhenApplicationNotActive();
    SlateApp.SetHandleDeviceInputWhenApplicationNotActive(true);
    ON_SCOPE_EXIT { SlateApp.SetHandleDeviceInputWhenApplicationNotActive(bPrevHandleInactive); };

    DispatchMouseMove(SlateApp, ScreenPos);

    const FKey ButtonKey = MouseButtonToKey(Button);
    const uint32 UserIdx = static_cast<uint32>(SlateApp.GetUserIndexForMouse());
    const FVector2D LastPos(SlateApp.GetLastCursorPos());
    const TSharedPtr<FGenericWindow> NativeWindow = ResolveNativeWindowUnder(SlateApp, ScreenPos);

    FPointerEvent DownEvent(UserIdx, FSlateApplication::CursorPointerIndex,
        ScreenPos, LastPos, SlateApp.GetPressedMouseButtons(),
        ButtonKey, 0.0f, FModifierKeysState());
    // Capture the handled state Slate returns for the press: this is the honest
    // "a widget under the point received the click" signal (false for empty space).
    bOutPressHandled = SlateApp.ProcessMouseButtonDownEvent(NativeWindow, DownEvent);

    FPointerEvent UpEvent(UserIdx, FSlateApplication::CursorPointerIndex,
        ScreenPos, LastPos, SlateApp.GetPressedMouseButtons(),
        ButtonKey, 0.0f, FModifierKeysState());
    SlateApp.ProcessMouseButtonUpEvent(UpEvent);

    return true;
}

bool FDriveInput::MoveTo(const FVector2D& ScreenPos)
{
    if (!FSlateApplication::IsInitialized())
    {
        return false;
    }

    FSlateApplication& SlateApp = FSlateApplication::Get();
    const bool bPrevHandleInactive = SlateApp.GetHandleDeviceInputWhenApplicationNotActive();
    SlateApp.SetHandleDeviceInputWhenApplicationNotActive(true);
    ON_SCOPE_EXIT { SlateApp.SetHandleDeviceInputWhenApplicationNotActive(bPrevHandleInactive); };

    DispatchMouseMove(SlateApp, ScreenPos);
    return true;
}

bool FDriveInput::DragFromTo(const FVector2D& From, const FVector2D& To, int32 DurationMs)
{
    if (!FSlateApplication::IsInitialized())
    {
        return false;
    }

    FSlateApplication& SlateApp = FSlateApplication::Get();
    const bool bPrevHandleInactive = SlateApp.GetHandleDeviceInputWhenApplicationNotActive();
    SlateApp.SetHandleDeviceInputWhenApplicationNotActive(true);
    ON_SCOPE_EXIT { SlateApp.SetHandleDeviceInputWhenApplicationNotActive(bPrevHandleInactive); };

    const uint32 UserIdx = static_cast<uint32>(SlateApp.GetUserIndexForMouse());

    // Press at the start point, resolving the window under it.
    DispatchMouseMove(SlateApp, From);
    const TSharedPtr<FGenericWindow> NativeWindow = ResolveNativeWindowUnder(SlateApp, From);

    FPointerEvent DownEvent(UserIdx, FSlateApplication::CursorPointerIndex,
        From, FVector2D(SlateApp.GetLastCursorPos()), SlateApp.GetPressedMouseButtons(),
        EKeys::LeftMouseButton, 0.0f, FModifierKeysState());
    SlateApp.ProcessMouseButtonDownEvent(NativeWindow, DownEvent);

    // Interpolated moves with the button held.
    FVector2D PrevPos = From;
    for (const FVector2D& StepPos : ComputeDragStepPoints(From, To, DurationMs))
    {
        if (TSharedPtr<ICursor> Cursor = SlateApp.GetPlatformCursor())
        {
            Cursor->SetPosition(FMath::RoundToInt(StepPos.X), FMath::RoundToInt(StepPos.Y));
        }

        FPointerEvent MoveEvent(UserIdx, FSlateApplication::CursorPointerIndex,
            StepPos, PrevPos, SlateApp.GetPressedMouseButtons(),
            EKeys::Invalid, 0.0f, FModifierKeysState());
        SlateApp.ProcessMouseMoveEvent(MoveEvent);
        PrevPos = StepPos;
    }

    FPointerEvent UpEvent(UserIdx, FSlateApplication::CursorPointerIndex,
        To, PrevPos, SlateApp.GetPressedMouseButtons(),
        EKeys::LeftMouseButton, 0.0f, FModifierKeysState());
    SlateApp.ProcessMouseButtonUpEvent(UpEvent);

    return true;
}

bool FDriveInput::ScrollAt(const FVector2D& ScreenPos, float Delta)
{
    if (!FSlateApplication::IsInitialized())
    {
        return false;
    }

    FSlateApplication& SlateApp = FSlateApplication::Get();
    const bool bPrevHandleInactive = SlateApp.GetHandleDeviceInputWhenApplicationNotActive();
    SlateApp.SetHandleDeviceInputWhenApplicationNotActive(true);
    ON_SCOPE_EXIT { SlateApp.SetHandleDeviceInputWhenApplicationNotActive(bPrevHandleInactive); };

    DispatchMouseMove(SlateApp, ScreenPos);
    SlateApp.OnMouseWheel(Delta, ScreenPos);
    return true;
}

// ────────────────────────────────────────────────────────────────────────────
// Keyboard injection
// ────────────────────────────────────────────────────────────────────────────

bool FDriveInput::PressKey(const FKey& Key, EDriveModifierKeys Modifiers, EDriveKeyAction Action)
{
    // Delegate to the handled-reporting variant; existing callers only care that the
    // event was injected, not whether a widget consumed it.
    bool bDiscardHandled = false;
    return PressKeyReportingHandled(Key, Modifiers, Action, bDiscardHandled);
}

bool FDriveInput::PressKeyReportingHandled(const FKey& Key, EDriveModifierKeys Modifiers,
    EDriveKeyAction Action, bool& bOutHandled)
{
    bOutHandled = false;

    if (!FSlateApplication::IsInitialized() || !Key.IsValid())
    {
        return false;
    }

    FSlateApplication& SlateApp = FSlateApplication::Get();
    return PressKeyReportingHandled(SlateApp, Key, Modifiers, Action,
        SlateApp.GetInputDeviceIdForKeyboard(),
        static_cast<uint32>(SlateApp.GetUserIndexForKeyboard()), bOutHandled);
}

bool FDriveInput::PressKeyReportingHandled(FSlateApplication& SlateApp, const FKey& Key,
    EDriveModifierKeys Modifiers, EDriveKeyAction Action,
    FInputDeviceId InputDevice, uint32 SlateUserIndex, bool& bOutHandled)
{
    bOutHandled = false;
    if (!Key.IsValid())
    {
        return false;
    }

    const bool bPrevHandleInactive = SlateApp.GetHandleDeviceInputWhenApplicationNotActive();
    SlateApp.SetHandleDeviceInputWhenApplicationNotActive(true);
    ON_SCOPE_EXIT { SlateApp.SetHandleDeviceInputWhenApplicationNotActive(bPrevHandleInactive); };

    const FModifierKeysState ModState = MakeModifierState(Modifiers);

    // Resolve the platform key/char codes so listeners that read them work.
    const uint32* KeyCodePtr = nullptr;
    const uint32* CharCodePtr = nullptr;
    FInputKeyManager::Get().GetCodesFromKey(Key, KeyCodePtr, CharCodePtr);
    const uint32 KeyCode = KeyCodePtr ? *KeyCodePtr : 0;
    const uint32 CharCode = CharCodePtr ? *CharCodePtr : 0;

    if (Action == EDriveKeyAction::Press || Action == EDriveKeyAction::Down)
    {
        FKeyEvent DownEvent(Key, ModState, InputDevice, /*bIsRepeat*/ false,
            CharCode, KeyCode, TOptional<int32>(static_cast<int32>(SlateUserIndex)));
        bOutHandled |= SlateApp.ProcessKeyDownEvent(DownEvent);
    }
    if (Action == EDriveKeyAction::Press || Action == EDriveKeyAction::Up)
    {
        FKeyEvent UpEvent(Key, ModState, InputDevice, /*bIsRepeat*/ false,
            CharCode, KeyCode, TOptional<int32>(static_cast<int32>(SlateUserIndex)));
        bOutHandled |= SlateApp.ProcessKeyUpEvent(UpEvent);
    }

    return true;
}

bool FDriveInput::TypeString(const FString& Text)
{
    if (!FSlateApplication::IsInitialized())
    {
        return false;
    }

    FSlateApplication& SlateApp = FSlateApplication::Get();
    const bool bPrevHandleInactive = SlateApp.GetHandleDeviceInputWhenApplicationNotActive();
    SlateApp.SetHandleDeviceInputWhenApplicationNotActive(true);
    ON_SCOPE_EXIT { SlateApp.SetHandleDeviceInputWhenApplicationNotActive(bPrevHandleInactive); };

    const uint32 UserIdx = static_cast<uint32>(SlateApp.GetUserIndexForKeyboard());

    for (int32 i = 0; i < Text.Len(); ++i)
    {
        const TCHAR Char = Text[i];
        const FDriveCharKeyMapping Mapping = MapCharToKey(Char);
        const FModifierKeysState ModState = MakeModifierState(
            Mapping.bShift ? EDriveModifierKeys::Shift : EDriveModifierKeys::None);

        // The char event carries the literal glyph so the typed text is exactly
        // Char (uppercase / shifted symbols included).
        const uint32 CharCode = static_cast<uint32>(Char);
        uint32 KeyCode = 0;
        if (Mapping.Key.IsValid())
        {
            const uint32* KeyCodePtr = nullptr;
            const uint32* CharCodePtr = nullptr;
            FInputKeyManager::Get().GetCodesFromKey(Mapping.Key, KeyCodePtr, CharCodePtr);
            KeyCode = KeyCodePtr ? *KeyCodePtr : 0;

            FKeyEvent DownEvent(Mapping.Key, ModState, UserIdx, /*bIsRepeat*/ false, CharCode, KeyCode);
            SlateApp.ProcessKeyDownEvent(DownEvent);
        }

        // The character event inserts the glyph into the focused widget.
        FCharacterEvent CharEvent(Char, ModState, UserIdx, /*bIsRepeat*/ false);
        SlateApp.ProcessKeyCharEvent(CharEvent);

        if (Mapping.Key.IsValid())
        {
            FKeyEvent UpEvent(Mapping.Key, ModState, UserIdx, /*bIsRepeat*/ false, CharCode, KeyCode);
            SlateApp.ProcessKeyUpEvent(UpEvent);
        }
    }

    return true;
}
