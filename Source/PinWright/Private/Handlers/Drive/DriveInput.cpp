// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Handlers/Drive/DriveInput.h"

#include "Containers/Ticker.h"
#include "Editor.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/GameViewportClient.h"
#include "Engine/LocalPlayer.h"
#include "Engine/World.h"
#include "Framework/Application/SlateApplication.h"
#include "GenericPlatform/GenericApplication.h"
#include "GenericPlatform/GenericWindow.h"
#include "GenericPlatform/ICursor.h"
#include "Input/HittestGrid.h"
#include "InputCoreTypes.h"
#include "Layout/WidgetPath.h"
#include "Misc/ScopeExit.h"
#include "Widgets/SViewport.h"
#include "Widgets/SWindow.h"

#if PLATFORM_LINUX
#include <dlfcn.h>
#endif

namespace DriveInputModifiers
{
    // A modifier a chord holds, in press order: its drive flag, the physical key a keyboard
    // sends for it, and its SDL_Keymod bit (SDL_KMOD_LCTRL/LALT/LSHIFT/LGUI).
    struct FChordModifier
    {
        EDriveModifierKeys Flag;
        FKey Key;
        uint16 SdlMask;
    };

    const TArray<FChordModifier>& ChordModifiers()
    {
        static const TArray<FChordModifier> Modifiers = {
            { EDriveModifierKeys::Ctrl,  EKeys::LeftControl, 0x0040 },
            { EDriveModifierKeys::Alt,   EKeys::LeftAlt,     0x0100 },
            { EDriveModifierKeys::Shift, EKeys::LeftShift,   0x0001 },
            { EDriveModifierKeys::Cmd,   EKeys::LeftCommand, 0x0400 },
        };
        return Modifiers;
    }

    // FSlateApplication::GetModifierKeys() reads the platform keyboard state, never the injected
    // event, and handlers that query it see a bare key: the PIE game viewport hands that state to
    // the editor's play-world chords (Shift+F1 mouse release). On Linux it is SDL's modifier
    // state, which ApplicationCore exports, so hold the same bits a pressed key would. Under
    // -RenderOffScreen the platform application is FNullApplication, which reports no modifiers
    // whatever SDL holds, and Windows keeps its state private; there only the event flags and
    // key events carry it, and the caller's read-back reports that.
    void SetPlatformModifiersHeld(EDriveModifierKeys Modifiers, bool bHeld)
    {
#if PLATFORM_LINUX
        using FGetModState = uint16 (*)();
        using FSetModState = void (*)(uint16);
        static const FGetModState GetModState = reinterpret_cast<FGetModState>(dlsym(RTLD_DEFAULT, "SDL_GetModState"));
        static const FSetModState SetModState = reinterpret_cast<FSetModState>(dlsym(RTLD_DEFAULT, "SDL_SetModState"));
        if (!GetModState || !SetModState)
        {
            return;
        }
        uint16 Mask = 0;
        for (const FChordModifier& Modifier : ChordModifiers())
        {
            if (EnumHasAnyFlags(Modifiers, Modifier.Flag))
            {
                Mask |= Modifier.SdlMask;
            }
        }
        const uint16 State = GetModState();
        SetModState(bHeld ? (State | Mask) : (State & ~Mask));
#endif
    }
}

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

// CommonInput (an optional plugin, so reached by reflection) keeps one input type per local
// player. A gamepad key (drive.key Gamepad_*) switches it to Gamepad, which also swaps the Slate
// cursor user onto a faux cursor that a warp of the platform cursor does not move. A real mouse
// switches it back through FCommonInputPreprocessor, but that preprocessor drops a synthetic event
// unless the player's game viewport is in the Slate user's focus path (RefreshCurrentInputMethod,
// PIE) and the move has a non-zero delta. Board B-drive-click-dead-after-gamepad-key.
DEFINE_LOG_CATEGORY_STATIC(LogPinWrightDriveInput, Log, All);

namespace DriveInputCommonInput
{
    // ECommonInputType (CommonInputTypeEnum.h): MouseAndKeyboard = 0, Gamepad = 1, Touch = 2.
    constexpr uint8 MouseAndKeyboard = 0;
    constexpr uint8 Gamepad = 1;

    struct FReflected
    {
        UClass* SubsystemClass = nullptr;
        UFunction* GetType = nullptr;
        UFunction* SetType = nullptr;
    };

    // Resolved once CommonInput's class exists; retried while it does not.
    const FReflected& Reflected()
    {
        static FReflected Cached;
        if (!Cached.SubsystemClass)
        {
            UClass* SubsystemClass = FindObject<UClass>(nullptr, TEXT("/Script/CommonInput.CommonInputSubsystem"));
            UFunction* GetType = SubsystemClass ? SubsystemClass->FindFunctionByName(TEXT("GetCurrentInputType")) : nullptr;
            UFunction* SetType = SubsystemClass ? SubsystemClass->FindFunctionByName(TEXT("SetCurrentInputType")) : nullptr;
            // Each takes or returns exactly one ECommonInputType, so a uint8 is the whole parameter block.
            if (GetType && SetType && GetType->ParmsSize == sizeof(uint8) && SetType->ParmsSize == sizeof(uint8))
            {
                Cached = FReflected{ SubsystemClass, GetType, SetType };
            }
        }
        return Cached;
    }

    // The switch a real mouse event at ScreenPos makes: only for the local player that
    // FCommonInputPreprocessor would treat as the event's owner, i.e. whose game viewport the
    // pointer is over and whose (PIE-remapped) ControllerId is the mouse's Slate user. Gamepad goes
    // back to MouseAndKeyboard; Touch (mouse-for-touch) is left alone, as a real mouse leaves it.
    void ReturnGamepadPlayerToMouse(FSlateApplication& SlateApp, const FVector2D& ScreenPos)
    {
        const FReflected& R = Reflected();
        if (!R.SubsystemClass || !GEngine)
        {
            return;
        }
        const FWidgetPath Path = SlateApp.LocateWindowUnderMouse(ScreenPos, SlateApp.GetInteractiveTopLevelWindows());
        if (!Path.IsValid())
        {
            return;
        }
        const int32 MouseUser = SlateApp.GetUserIndexForMouse();
        for (const FWorldContext& Context : GEngine->GetWorldContexts())
        {
            const UWorld* World = Context.World();
            if (!Context.OwningGameInstance || !World || !World->IsGameWorld())
            {
                continue;
            }
            for (ULocalPlayer* Player : Context.OwningGameInstance->GetLocalPlayers())
            {
                UGameViewportClient* ViewportClient = Player ? Player->ViewportClient.Get() : nullptr;
                const TSharedPtr<SViewport> ViewportWidget = ViewportClient ? ViewportClient->GetGameViewportWidget() : nullptr;
                if (!ViewportWidget.IsValid() || !Path.ContainsWidget(ViewportWidget.Get()))
                {
                    continue;
                }
                int32 ControllerId = Player->GetControllerId();
                GEngine->RemapGamepadControllerIdForPIE(ViewportClient, ControllerId);
                UObject* Subsystem = Player->GetSubsystemBase(R.SubsystemClass);
                if (ControllerId != MouseUser || !Subsystem)
                {
                    continue;
                }
                uint8 Current = MouseAndKeyboard;
                Subsystem->ProcessEvent(R.GetType, &Current);
                if (Current == Gamepad)
                {
                    uint8 NewType = MouseAndKeyboard;
                    Subsystem->ProcessEvent(R.SetType, &NewType);
                    UE_LOG(LogPinWrightDriveInput, Log, TEXT("CommonInput input type of %s was Gamepad; switched to MouseAndKeyboard for mouse input over its viewport."),
                        *Player->GetName());
                }
            }
        }
    }
}

void FDriveInput::DispatchMouseMove(FSlateApplication& SlateApp, const FVector2D& ScreenPos)
{
    const FVector2D LastPos(SlateApp.GetLastCursorPos());

    // Keep the OS cursor in sync so later GetCursorPos()/GetLastCursorPos() agree.
    if (TSharedPtr<ICursor> Cursor = SlateApp.GetPlatformCursor())
    {
        Cursor->SetPosition(FMath::RoundToInt(ScreenPos.X), FMath::RoundToInt(ScreenPos.Y));
    }

    // Before the event, as CommonInput's preprocessor switches before routing.
    DriveInputCommonInput::ReturnGamepadPlayerToMouse(SlateApp, ScreenPos);

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
    const TSharedPtr<SWindow> Window = WindowUnderPoint(ScreenPos);
    return Window.IsValid() ? Window->GetNativeWindow() : nullptr;
}

TSharedPtr<SWindow> FDriveInput::WindowUnderPoint(const FVector2D& ScreenPos)
{
    if (!FSlateApplication::IsInitialized())
    {
        return nullptr;
    }

    FSlateApplication& SlateApp = FSlateApplication::Get();
    const FWidgetPath WidgetsUnderCursor = SlateApp.LocateWindowUnderMouse(
        ScreenPos, SlateApp.GetInteractiveTopLevelWindows(), /*bIgnoreEnabledStatus=*/false,
        SlateApp.GetUserIndexForMouse());
    return WidgetsUnderCursor.IsValid() ? TSharedPtr<SWindow>(WidgetsUnderCursor.GetWindow()) : nullptr;
}

namespace
{
    // The fallback half of FSlateApplication::LocateWindowUnderMouse: top-most first, child
    // windows before their parent, without consulting the platform's window-under-cursor.
    TSharedPtr<SWindow> TopWindowIn(FSlateApplication& SlateApp, const TArray<TSharedRef<SWindow>>& Windows,
        const FVector2D& ScreenPos, int32 UserIndex)
    {
        for (int32 Index = Windows.Num() - 1; Index >= 0; --Index)
        {
            const TSharedRef<SWindow>& Window = Windows[Index];
            if (!Window->IsVisible() || Window->IsWindowMinimized())
            {
                continue;
            }
            if (TSharedPtr<SWindow> Child = TopWindowIn(SlateApp, Window->GetChildWindows(), ScreenPos, UserIndex))
            {
                return Child;
            }
            // LocateWidgetInWindow's test (protected there): the window takes input at the point
            // and its hit-test grid names a widget under it.
            if (Window->AcceptsInput() && Window->IsScreenspaceMouseWithin(ScreenPos)
                && Window->GetHittestGrid().GetBubblePath(ScreenPos, SlateApp.GetCursorRadius(),
                    /*bIgnoreEnabledStatus=*/false, UserIndex).Num() > 0)
            {
                return Window;
            }
        }
        return nullptr;
    }
}

TSharedPtr<SWindow> FDriveInput::TopWindowAtPoint(const FVector2D& ScreenPos)
{
    if (!FSlateApplication::IsInitialized())
    {
        return nullptr;
    }

    FSlateApplication& SlateApp = FSlateApplication::Get();
    return TopWindowIn(SlateApp, SlateApp.GetInteractiveTopLevelWindows(), ScreenPos, SlateApp.GetUserIndexForMouse());
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
    SlateApp.ProcessMouseButtonDownEvent(NativeWindow, DownEvent);

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

namespace DriveInputHoverHold
{
    struct FHold
    {
        FIntPoint Point;
        TWeakPtr<SWindow> Window;
        bool bDuringPie = false;
        bool bPrevHandleInactive = false;
        FTSTicker::FDelegateHandle Ticker;
    };

    TOptional<FHold>& Active()
    {
        static TOptional<FHold> Hold;
        return Hold;
    }

    FIntPoint RoundPoint(const FVector2D& Pos)
    {
        return FIntPoint(FMath::RoundToInt(Pos.X), FMath::RoundToInt(Pos.Y));
    }

    bool IsPieRunning()
    {
        return GEditor && GEditor->PlayWorld != nullptr;
    }

    // Per frame: keep the hold while the platform cursor still sits on the hovered point, Slate
    // still routes that point to the hovered window, and PIE has neither started nor ended.
    bool Tick(float)
    {
        TOptional<FHold>& Hold = Active();
        if (!Hold.IsSet())
        {
            return false;
        }
        if (FSlateApplication::IsInitialized())
        {
            const TSharedPtr<ICursor> Cursor = FSlateApplication::Get().GetPlatformCursor();
            const TSharedPtr<SWindow> Window = Hold->Window.Pin();
            if (Cursor.IsValid() && RoundPoint(Cursor->GetPosition()) == Hold->Point
                && Window.IsValid() && FDriveInput::WindowUnderPoint(FVector2D(Hold->Point)) == Window
                && IsPieRunning() == Hold->bDuringPie)
            {
                return true;
            }
        }
        // Returning false unregisters this ticker; drop the handle so Release does not remove it.
        Hold->Ticker.Reset();
        FDriveInput::ReleaseHoverHold();
        return false;
    }
}

bool FDriveInput::HoverAt(const FVector2D& ScreenPos)
{
    if (!MoveTo(ScreenPos))
    {
        return false;
    }

    using namespace DriveInputHoverHold;
    FSlateApplication& SlateApp = FSlateApplication::Get();
    TOptional<FHold>& Hold = Active();
    if (!Hold.IsSet())
    {
        // MoveTo restored the flag, so this is the caller's value, not an injection scope's.
        Hold.Emplace();
        Hold->bPrevHandleInactive = SlateApp.GetHandleDeviceInputWhenApplicationNotActive();
        Hold->Ticker = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateStatic(&DriveInputHoverHold::Tick));
    }
    // Where the cursor actually landed, so a warp that missed by a pixel does not release at once.
    const TSharedPtr<ICursor> Cursor = SlateApp.GetPlatformCursor();
    Hold->Point = RoundPoint(Cursor.IsValid() ? Cursor->GetPosition() : ScreenPos);
    Hold->Window = WindowUnderPoint(FVector2D(Hold->Point));
    Hold->bDuringPie = IsPieRunning();
    SlateApp.SetHandleDeviceInputWhenApplicationNotActive(true);
    return true;
}

void FDriveInput::ReleaseHoverHold()
{
    TOptional<DriveInputHoverHold::FHold>& Hold = DriveInputHoverHold::Active();
    if (!Hold.IsSet())
    {
        return;
    }
    if (Hold->Ticker.IsValid())
    {
        FTSTicker::GetCoreTicker().RemoveTicker(Hold->Ticker);
    }
    if (FSlateApplication::IsInitialized())
    {
        FSlateApplication::Get().SetHandleDeviceInputWhenApplicationNotActive(Hold->bPrevHandleInactive);
    }
    Hold.Reset();
}

bool FDriveInput::IsHoverHeld()
{
    return DriveInputHoverHold::Active().IsSet();
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

bool FDriveInput::PressKey(const FKey& Key, EDriveModifierKeys Modifiers, EDriveKeyAction Action,
    bool* bOutPlatformModifiersHeld)
{
    // Delegate to the handled-reporting variant; existing callers only care that the
    // event was injected, not whether a widget consumed it.
    bool bDiscardHandled = false;
    return PressKeyReportingHandled(Key, Modifiers, Action, bDiscardHandled, bOutPlatformModifiersHeld);
}

bool FDriveInput::PressKeyReportingHandled(const FKey& Key, EDriveModifierKeys Modifiers,
    EDriveKeyAction Action, bool& bOutHandled, bool* bOutPlatformModifiersHeld)
{
    bOutHandled = false;
    if (bOutPlatformModifiersHeld)
    {
        *bOutPlatformModifiersHeld = false;
    }

    if (!FSlateApplication::IsInitialized() || !Key.IsValid())
    {
        return false;
    }

    FSlateApplication& SlateApp = FSlateApplication::Get();
    return PressKeyReportingHandled(SlateApp, Key, Modifiers, Action,
        SlateApp.GetInputDeviceIdForKeyboard(),
        static_cast<uint32>(SlateApp.GetUserIndexForKeyboard()), bOutHandled, bOutPlatformModifiersHeld);
}

bool FDriveInput::PressKeyReportingHandled(FSlateApplication& SlateApp, const FKey& Key,
    EDriveModifierKeys Modifiers, EDriveKeyAction Action,
    FInputDeviceId InputDevice, uint32 SlateUserIndex, bool& bOutHandled,
    bool* bOutPlatformModifiersHeld)
{
    bOutHandled = false;
    if (bOutPlatformModifiersHeld)
    {
        *bOutPlatformModifiersHeld = false;
    }
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

    // A modifier is a real chord, as a keyboard sends it: each modifier's own key-down before
    // the key and key-up after it, so handlers that read held-key state (a viewport's key map,
    // UPlayerInput, the platform modifier query) see it, not only the event's flags.
    using namespace DriveInputModifiers;
    auto SendModifierEdge = [&](const FKey& ModifierKey, EDriveModifierKeys HeldNow, bool bDown)
    {
        const uint32* ModKeyCodePtr = nullptr;
        const uint32* ModCharCodePtr = nullptr;
        FInputKeyManager::Get().GetCodesFromKey(ModifierKey, ModKeyCodePtr, ModCharCodePtr);
        FKeyEvent Event(ModifierKey, MakeModifierState(HeldNow), InputDevice, /*bIsRepeat*/ false,
            ModCharCodePtr ? *ModCharCodePtr : 0, ModKeyCodePtr ? *ModKeyCodePtr : 0,
            TOptional<int32>(static_cast<int32>(SlateUserIndex)));
        if (bDown)
        {
            SlateApp.ProcessKeyDownEvent(Event);
        }
        else
        {
            SlateApp.ProcessKeyUpEvent(Event);
        }
    };

    if (Action == EDriveKeyAction::Press || Action == EDriveKeyAction::Down)
    {
        SetPlatformModifiersHeld(Modifiers, true);
        EDriveModifierKeys Held = EDriveModifierKeys::None;
        for (const FChordModifier& Modifier : ChordModifiers())
        {
            if (EnumHasAnyFlags(Modifiers, Modifier.Flag) && Modifier.Key != Key)
            {
                Held |= Modifier.Flag;
                SendModifierEdge(Modifier.Key, Held, /*bDown*/ true);
            }
        }
        if (bOutPlatformModifiersHeld)
        {
            // Read back, never assumed: the platform application, not SDL, answers this query.
            const FModifierKeysState Platform = SlateApp.GetModifierKeys();
            *bOutPlatformModifiersHeld =
                (!EnumHasAnyFlags(Modifiers, EDriveModifierKeys::Shift) || Platform.IsShiftDown())
                && (!EnumHasAnyFlags(Modifiers, EDriveModifierKeys::Ctrl) || Platform.IsControlDown())
                && (!EnumHasAnyFlags(Modifiers, EDriveModifierKeys::Alt) || Platform.IsAltDown())
                && (!EnumHasAnyFlags(Modifiers, EDriveModifierKeys::Cmd) || Platform.IsCommandDown());
        }
        FKeyEvent DownEvent(Key, ModState, InputDevice, /*bIsRepeat*/ false,
            CharCode, KeyCode, TOptional<int32>(static_cast<int32>(SlateUserIndex)));
        bOutHandled |= SlateApp.ProcessKeyDownEvent(DownEvent);
    }
    if (Action == EDriveKeyAction::Press || Action == EDriveKeyAction::Up)
    {
        FKeyEvent UpEvent(Key, ModState, InputDevice, /*bIsRepeat*/ false,
            CharCode, KeyCode, TOptional<int32>(static_cast<int32>(SlateUserIndex)));
        bOutHandled |= SlateApp.ProcessKeyUpEvent(UpEvent);
        EDriveModifierKeys Held = Modifiers;
        for (int32 Index = ChordModifiers().Num() - 1; Index >= 0; --Index)
        {
            const FChordModifier& Modifier = ChordModifiers()[Index];
            if (EnumHasAnyFlags(Modifiers, Modifier.Flag) && Modifier.Key != Key)
            {
                Held &= ~Modifier.Flag;
                SendModifierEdge(Modifier.Key, Held, /*bDown*/ false);
            }
        }
        SetPlatformModifiersHeld(Modifiers, false);
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

bool FDriveInput::FocusForKeyboard(const TSharedRef<SWidget>& Target, const FVector2D& ScreenPos, FString& OutFocused)
{
    OutFocused.Reset();
    if (!FSlateApplication::IsInitialized())
    {
        return false;
    }

    // A wrapper (SEditableTextBox) forwards its focus to the inner SEditableText, so a focused
    // descendant counts as the target holding focus.
    const int32 KeyboardUser = FSlateApplication::Get().GetUserIndexForKeyboard();
    const auto HoldsFocus = [&Target, KeyboardUser]()
    {
        return Target->HasUserFocus(KeyboardUser).IsSet() || Target->HasUserFocusedDescendants(KeyboardUser);
    };
    if (HoldsFocus())
    {
        return true;
    }

    // The windows open before the click, so a popup the click opens can be told from one already up.
    FSlateApplication& SlateApp = FSlateApplication::Get();
    const TSharedPtr<SWindow> TargetWindow = SlateApp.FindWidgetWindow(Target);
    TArray<TSharedRef<SWindow>> WindowsBefore;
    SlateApp.GetAllVisibleWindowsOrdered(WindowsBefore);

    // A click that no widget on the path takes focus from (it lands on a non-focusable overlay)
    // leaves Slate focusing the leaf-most focusable widget under it, e.g. an SDockingTabStack.
    ClickAt(ScreenPos);
    if (HoldsFocus())
    {
        return true;
    }
    const TSharedPtr<SWidget> Focused = SlateApp.GetUserFocusedWidget(KeyboardUser);
    if (!Focused.IsValid())
    {
        return false;
    }
    // A combo or search box the click opened focuses a field in its popup, a child window of the
    // target's window; the keys a user types next go there, so that counts as the target's focus.
    const TSharedPtr<SWindow> FocusedWindow = SlateApp.FindWidgetWindow(Focused.ToSharedRef());
    if (TargetWindow.IsValid() && FocusedWindow.IsValid() && FocusedWindow != TargetWindow
        && FocusedWindow->IsDescendantOf(TargetWindow) && !WindowsBefore.Contains(FocusedWindow.ToSharedRef()))
    {
        return true;
    }
    OutFocused = Focused->ToString();
    return false;
}
