// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Handlers/HandlerRegistration.h"
#include "Handlers/ParamSpec.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/ErrorCodes.h"

#include "Handlers/Drive/DriveActionCommon.h"
#include "Handlers/Drive/DriveHandlerCommon.h"
#include "Handlers/Drive/DriveInput.h"
#include "Handlers/Drive/DriveLiveResolver.h"
#include "Handlers/Drive/DriveOsInput.h"
#include "Handlers/Drive/DriveWebHandlers.h"

#include "Dom/JsonObject.h"
#include "InputCoreTypes.h"

// Web surface: all six drive.* action verbs (click / type / scroll / drag / hover / key)
// branch to the asynchronous FDriveWebHandlers path (CEF DOM round-trip + the web settle
// model) when surface=web. On the game/editor surfaces they stay on the synchronous
// FDriveActionCommon::RunAction settle path below. Web drag is DOM-element-to-element, so
// surface=web requires a to_handle (the coordinate to_x/to_y release point is game/editor only).
//
// The six asynchronous drive.* ACTION verbs. Each parses only its own input
// params and hands the right injection lambda to FDriveActionCommon::RunAction,
// which owns the re-resolve / settle / async-resolve flow. All of them also
// accept the common settle params (stable_ticks, quiet_budget_ms,
// settle_budget_ms, timeout_ms), an optional wait_for condition, the observe mode,
// full_diff, and include_journal / mark_cap / journal_since — those are read
// inside RunAction, so they are listed here for discovery only.
//
// DEFAULT RESPONSE SHAPE (compact): { outcome, changed, settled, condition_met,
// elapsed_ms, ticks, diff: <summary>, journal? }. observe defaults to none so no
// full element list / screenshot rides back unasked, and diff defaults to the compact
// summary (counts + 15-handle samples). observe=list / list+screenshot re-adds the
// observation; full_diff=true swaps the summary for the complete handle lists.

// Reusable settle/observe param descriptors so each verb's RPC_PARAMS stays
// focused on its own input. Expands to a comma-separated FParamSpec list inside
// the variadic RPC_PARAMS(...). RunAction reads these from Ctx; they are declared
// here for discovery only. Includes the editor-chrome window selector
// (window_title/title, window_index/index) that RunAction -> ParseWindowSelector
// consumes, so the dispatcher's param allowlist accepts a windowed action verb.
#define DRIVE_COMMON_ACTION_PARAMS \
    RPC_PARAM_DEF("surface", "string", "Surface to act on: game | editor_chrome | web | auto (default auto -> game).", "auto"), \
    RPC_PARAM_OPT("instance_name", "string", "Live UMG root selector: substring-matched against the backing widget name."), \
    RPC_PARAM_OPT("root_index", "integer", "Live UMG root selector: the Nth root (0-based). Takes precedence over instance_name."), \
    DRIVE_WINDOW_SELECTOR_PARAMS, \
    RPC_PARAM_DEF("stable_ticks", "number", "Consecutive stable ticks required before the UI is considered settled (default 2).", "2"), \
    RPC_PARAM_DEF("quiet_budget_ms", "number", "Budget for the no-change phase before reporting a clean quiet, in ms (default 500).", "500"), \
    RPC_PARAM_DEF("settle_budget_ms", "number", "Budget for the overall settle phase, in ms (default 1500).", "1500"), \
    RPC_PARAM_DEF("timeout_ms", "number", "Timeout for an explicit wait_for condition, in ms (default 5000).", "5000"), \
    RPC_PARAM_OPT("wait_for", "object", "Optional condition to wait for after the action instead of settle-on-change."), \
    RPC_PARAM_DEF("observe", "string", "Observation detail in the response: none | list | list+screenshot (default none -> compact result with no full element list/screenshot).", "none"), \
    RPC_PARAM_DEF("full_diff", "boolean", "Return the full appeared/disappeared/changed handle lists instead of the compact diff summary (default false).", "false"), \
    RPC_PARAM_DEF("include_journal", "boolean", "Attach a top-level journal delta since journal_since (default false).", "false"), \
    RPC_PARAM_DEF("mark_cap", "number", "Max interactables given a visible mark in the observation screenshot (default 50).", "50"), \
    RPC_PARAM_DEF("journal_since", "number", "Cursor to read journal from when include_journal is set (default 0).", "0")

// The os_input opt-in, declared only on the two MOUSE verbs that implement it (drive.click,
// drive.hover) so the dispatcher's allowlist refuses it everywhere else — notably drive.key,
// whose XTEST key events were observed not to reach the editor's SDL window.
#define DRIVE_OS_INPUT_PARAM \
    RPC_PARAM_DEF("os_input", "boolean", \
        "Inject through the OS instead of Slate: real X11/XTEST pointer events, so the input " \
        "traverses SDL mouse confinement (LockOnCapture) and relative mode the way a human's " \
        "mouse does, and nothing forces the Slate inactive-input flag (default false). " \
        "Linux/X11 only — INVALID_ARGUMENT elsewhere or when no X display can be opened. " \
        "Mouse only; drive.key has no os_input. Blocks the editor for ~0.5s while the motion " \
        "path and button hold are paced. The response's input_path reports which path ran.", \
        "false")

namespace DriveActionHandlersLocal
{
    // Read the os_input opt-in and, when set, gate it on OS injection actually being
    // available on this host. Returns false AFTER sending the error when the caller asked
    // for a path this build/session cannot take — refusing up front rather than letting the
    // injection fail later as an opaque INPUT_FAILED.
    bool ResolveOsInput(FHandlerContext& Ctx, bool& bOutOsInput)
    {
        bOutOsInput = Ctx.GetBool(TEXT("os_input"), false);
        FString Error;
        if (bOutOsInput && !FDriveOsInput::IsAvailable(Error))
        {
            Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT, Error);
            return false;
        }
        return true;
    }

    // Parse a "+"/space/comma-separated modifier token list (shift/ctrl/alt/cmd).
    EDriveModifierKeys ParseModifiers(const FString& Spec)
    {
        EDriveModifierKeys Result = EDriveModifierKeys::None;
        if (Spec.IsEmpty())
        {
            return Result;
        }

        TArray<FString> Tokens;
        Spec.ParseIntoArray(Tokens, TEXT("+"), /*CullEmpty=*/true);
        if (Tokens.Num() <= 1)
        {
            // Fall back to whitespace/comma separators.
            Tokens.Reset();
            Spec.ParseIntoArrayWS(Tokens, TEXT(","), /*CullEmpty=*/true);
        }

        for (FString Token : Tokens)
        {
            Token.TrimStartAndEndInline();
            if (Token.Equals(TEXT("shift"), ESearchCase::IgnoreCase)) { Result |= EDriveModifierKeys::Shift; }
            else if (Token.Equals(TEXT("ctrl"), ESearchCase::IgnoreCase) || Token.Equals(TEXT("control"), ESearchCase::IgnoreCase)) { Result |= EDriveModifierKeys::Ctrl; }
            else if (Token.Equals(TEXT("alt"), ESearchCase::IgnoreCase)) { Result |= EDriveModifierKeys::Alt; }
            else if (Token.Equals(TEXT("cmd"), ESearchCase::IgnoreCase) || Token.Equals(TEXT("meta"), ESearchCase::IgnoreCase) || Token.Equals(TEXT("win"), ESearchCase::IgnoreCase)) { Result |= EDriveModifierKeys::Cmd; }
        }
        return Result;
    }

    EDriveKeyAction ParseKeyAction(const FString& Token)
    {
        if (Token.Equals(TEXT("down"), ESearchCase::IgnoreCase)) { return EDriveKeyAction::Down; }
        if (Token.Equals(TEXT("up"), ESearchCase::IgnoreCase))   { return EDriveKeyAction::Up; }
        return EDriveKeyAction::Press;
    }
}

using namespace DriveActionHandlersLocal;

// ============================================================================
// drive.click — move to an element's center and inject a mouse-button click.
// ============================================================================
REGISTER_RPC_HANDLER("drive.click", "drive",
    "Click a live UI element by handle (re-resolves the target, then settles before responding).",
    RPC_PARAMS(
        RPC_PARAM_REQ("handle", "string", "Handle of the element to click (from a drive.observe element)."),
        RPC_PARAM_DEF("button", "string", "Mouse button: left | right | middle (default left).", "left"),
        DRIVE_OS_INPUT_PARAM,
        DRIVE_COMMON_ACTION_PARAMS
    ))
{
    FString Handle;
    if (!Ctx.RequireString(TEXT("handle"), Handle)) return true;

    if (FDriveHandlerCommon::ResolveSurface(Ctx) == EDriveSurface::Web)
    {
        FDriveWebHandlers::ClickWeb(Ctx);
        return true;
    }

    bool bOsInput = false;
    if (!ResolveOsInput(Ctx, bOsInput)) return true;

    const EDriveMouseButton Button = FDriveInput::ParseMouseButton(Ctx.GetString(TEXT("button"), TEXT("left")));
    FDriveActionCommon::RunAction(Ctx, Handle,
        [Button, bOsInput](const FVector2D& Center)
        {
            if (bOsInput)
            {
                FString Error;
                return FDriveOsInput::ClickAt(Center, Button, Error);
            }
            return FDriveInput::ClickAt(Center, Button);
        },
        bOsInput ? TEXT("os_x11") : TEXT("slate"));
    return true;
}

// ============================================================================
// drive.hover — dispatch a real mouse-move to an element's center so hover fires.
// ============================================================================
REGISTER_RPC_HANDLER("drive.hover", "drive",
    "Hover a live UI element by handle (mouse-move to its center, then settle).",
    RPC_PARAMS(
        RPC_PARAM_REQ("handle", "string", "Handle of the element to hover."),
        DRIVE_OS_INPUT_PARAM,
        DRIVE_COMMON_ACTION_PARAMS
    ))
{
    FString Handle;
    if (!Ctx.RequireString(TEXT("handle"), Handle)) return true;

    if (FDriveHandlerCommon::ResolveSurface(Ctx) == EDriveSurface::Web)
    {
        FDriveWebHandlers::HoverWeb(Ctx);
        return true;
    }

    bool bOsInput = false;
    if (!ResolveOsInput(Ctx, bOsInput)) return true;

    FDriveActionCommon::RunAction(Ctx, Handle,
        [bOsInput](const FVector2D& Center)
        {
            if (bOsInput)
            {
                FString Error;
                return FDriveOsInput::MoveTo(Center, Error);
            }
            return FDriveInput::HoverAt(Center);
        },
        bOsInput ? TEXT("os_x11") : TEXT("slate"));
    return true;
}

// ============================================================================
// drive.scroll — inject a mouse-wheel event at an element's center.
// ============================================================================
REGISTER_RPC_HANDLER("drive.scroll", "drive",
    "Scroll over a live UI element by handle (mouse-wheel at its center, then settle).",
    RPC_PARAMS(
        RPC_PARAM_REQ("handle", "string", "Handle of the element to scroll over."),
        RPC_PARAM_DEF("delta", "number", "Wheel delta; positive scrolls up (default 1).", "1"),
        DRIVE_COMMON_ACTION_PARAMS
    ))
{
    FString Handle;
    if (!Ctx.RequireString(TEXT("handle"), Handle)) return true;

    if (FDriveHandlerCommon::ResolveSurface(Ctx) == EDriveSurface::Web)
    {
        FDriveWebHandlers::ScrollWeb(Ctx);
        return true;
    }

    const float Delta = static_cast<float>(Ctx.GetNumber(TEXT("delta"), 1.0));
    FDriveActionCommon::RunAction(Ctx, Handle,
        [Delta](const FVector2D& Center) { return FDriveInput::ScrollAt(Center, Delta); });
    return true;
}

// ============================================================================
// drive.type — focus an element (click its center) then type a string into it.
// ============================================================================
REGISTER_RPC_HANDLER("drive.type", "drive",
    "Type text into a live UI element by handle (clicks to focus, then types, then settles).",
    RPC_PARAMS(
        RPC_PARAM_REQ("handle", "string", "Handle of the input element to type into."),
        RPC_PARAM_REQ("text", "string", "Text to type one character at a time."),
        DRIVE_COMMON_ACTION_PARAMS
    ))
{
    FString Handle;
    if (!Ctx.RequireString(TEXT("handle"), Handle)) return true;
    FString Text;
    if (!Ctx.RequireString(TEXT("text"), Text)) return true;

    if (FDriveHandlerCommon::ResolveSurface(Ctx) == EDriveSurface::Web)
    {
        FDriveWebHandlers::TypeWeb(Ctx);
        return true;
    }

    FDriveActionCommon::RunAction(Ctx, Handle,
        [Text](const FVector2D& Center)
        {
            FDriveInput::ClickAt(Center);
            return FDriveInput::TypeString(Text);
        });
    return true;
}

// ============================================================================
// drive.key — inject a key event (optionally focusing an element first).
// ============================================================================
REGISTER_RPC_HANDLER("drive.key", "drive",
    "Inject a key event to the focused widget; with a handle, clicks it to focus first.",
    RPC_PARAMS(
        RPC_PARAM_REQ("key", "string", "Key name to inject (e.g. Enter, Escape, A, SpaceBar)."),
        RPC_PARAM_OPT("handle", "string", "Optional element to click for focus before the key."),
        RPC_PARAM_OPT("modifiers", "string", "Modifier keys to hold, e.g. \"ctrl+shift\" (shift/ctrl/alt/cmd)."),
        RPC_PARAM_DEF("action", "string", "Key edge: press | down | up (default press).", "press"),
        DRIVE_COMMON_ACTION_PARAMS
    ))
{
    FString KeyName;
    if (!Ctx.RequireString(TEXT("key"), KeyName)) return true;

    if (FDriveHandlerCommon::ResolveSurface(Ctx) == EDriveSurface::Web)
    {
        // Web key names are DOM key strings (e.g. "ArrowDown"), not FKeys, so branch before the
        // native FKey validation below; the bridge maps the key DOM-side.
        FDriveWebHandlers::KeyWeb(Ctx);
        return true;
    }

    const FKey Key(*KeyName);
    if (!EKeys::GetKeyDetails(Key).IsValid())
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_KEY,
            FString::Printf(TEXT("Unknown key name '%s'."), *KeyName));
        return true;
    }

    const EDriveModifierKeys Modifiers = ParseModifiers(Ctx.GetString(TEXT("modifiers")));
    const EDriveKeyAction Action = ParseKeyAction(Ctx.GetString(TEXT("action"), TEXT("press")));

    // Handle is optional: when present we click to focus, when absent the key goes
    // to the currently focused widget. RunAction is told whether to expect a target.
    const FString Handle = Ctx.GetString(TEXT("handle"));
    const bool bHasHandle = !Handle.IsEmpty();

    FDriveActionCommon::RunAction(Ctx, Handle,
        [Key, Modifiers, Action, bHasHandle](const FVector2D& Center)
        {
            if (bHasHandle)
            {
                FDriveInput::ClickAt(Center);
            }
            return FDriveInput::PressKey(Key, Modifiers, Action);
        });
    return true;
}

// ============================================================================
// drive.drag — press at one element's center, drag to a second point, release.
// ============================================================================
REGISTER_RPC_HANDLER("drive.drag", "drive",
    "Drag from one live UI element to a second element or absolute point, then settle.",
    RPC_PARAMS(
        RPC_PARAM_REQ("handle", "string", "Handle of the element to drag from (the press point)."),
        RPC_PARAM_OPT("to_handle", "string", "Handle of the element to drag to; its center is the release point."),
        RPC_PARAM_OPT("to_x", "number", "Absolute screen X of the release point (used with to_y when to_handle is absent)."),
        RPC_PARAM_OPT("to_y", "number", "Absolute screen Y of the release point (used with to_x when to_handle is absent)."),
        RPC_PARAM_DEF("duration_ms", "number", "Drag duration spreading the interpolated moves, in ms (default 200).", "200"),
        DRIVE_COMMON_ACTION_PARAMS
    ))
{
    FString FromHandle;
    if (!Ctx.RequireString(TEXT("handle"), FromHandle)) return true;

    if (FDriveHandlerCommon::ResolveSurface(Ctx) == EDriveSurface::Web)
    {
        // Web drag is DOM-element-to-element; DragWeb parses to_handle itself (to_x/to_y are
        // game/editor-only) and rejects a missing to_handle with INVALID_ARGUMENT.
        FDriveWebHandlers::DragWeb(Ctx);
        return true;
    }

    // Resolve the release point: a to_handle's live center, or literal to_x/to_y.
    FVector2D ToPoint = FVector2D::ZeroVector;
    const FString ToHandle = Ctx.GetString(TEXT("to_handle"));
    const TSharedPtr<FJsonObject>& Payload = Ctx.GetRawPayload();
    const bool bHasToCoords = Payload.IsValid()
        && Payload->HasField(TEXT("to_x")) && Payload->HasField(TEXT("to_y"));

    if (!ToHandle.IsEmpty())
    {
        const FDriveRootSelector Selector = FDriveHandlerCommon::ParseRootSelector(Ctx);
        const FDriveResolveResult Resolved = FDriveLiveResolver::ResolveHandle(Selector, ToHandle);
        if (Resolved.Status != EDriveResolveStatus::Found)
        {
            const TCHAR* Code = Resolved.Status == EDriveResolveStatus::Ambiguous
                ? ErrorCodes::ERR_TARGET_AMBIGUOUS
                : (Resolved.Status == EDriveResolveStatus::NotFound
                    ? ErrorCodes::ERR_TARGET_NOT_FOUND
                    : nullptr);
            // NoLiveUi carries the resolver's own specific code/message.
            Ctx.SendError(Code ? FString(Code) : Resolved.ErrorCode,
                Resolved.ErrorMessage.IsEmpty()
                    ? FString::Printf(TEXT("Could not resolve to_handle '%s'."), *ToHandle)
                    : Resolved.ErrorMessage);
            return true;
        }

        // The release target must pass the same gate as the press target (RunAction applies it
        // below). Without it a stale element's zeroed rect makes "its center" desktop (0,0), so
        // the drag would silently release in the corner of the screen instead of refusing.
        if (!FDriveActionCommon::IsActionable(Resolved.Element))
        {
            Ctx.SendError(ErrorCodes::ERR_TARGET_CHANGED,
                FString::Printf(
                    TEXT("Release target '%s' is no longer actionable (visible=%s, enabled=%s, geometry_stale=%s)."),
                    *ToHandle,
                    Resolved.Element.bVisible ? TEXT("true") : TEXT("false"),
                    Resolved.Element.bEnabled ? TEXT("true") : TEXT("false"),
                    Resolved.Element.bGeometryStale ? TEXT("true") : TEXT("false")));
            return true;
        }

        ToPoint = Resolved.Element.AbsolutePosition + Resolved.Element.AbsoluteSize * 0.5;
    }
    else if (bHasToCoords)
    {
        ToPoint = FVector2D(Ctx.GetNumber(TEXT("to_x")), Ctx.GetNumber(TEXT("to_y")));
    }
    else
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
            TEXT("drive.drag requires a release point: pass to_handle, or both to_x and to_y."));
        return true;
    }

    const int32 DurationMs = Ctx.GetInt(TEXT("duration_ms"), 200);
    FDriveActionCommon::RunAction(Ctx, FromHandle,
        [ToPoint, DurationMs](const FVector2D& From) { return FDriveInput::DragFromTo(From, ToPoint, DurationMs); });
    return true;
}

#undef DRIVE_OS_INPUT_PARAM
#undef DRIVE_COMMON_ACTION_PARAMS
