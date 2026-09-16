// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Handlers/HandlerRegistration.h"
#include "Handlers/ParamSpec.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/ErrorCodes.h"

#include "Utils/JsonBuilders.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/Engine.h"
#include "Engine/EngineBaseTypes.h"
#include "Engine/GameViewportClient.h"
#include "Engine/World.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Application/SlateUser.h"
#include "GameFramework/PlayerController.h"
#include "Types/ReflectionMetadata.h"
#include "Widgets/SViewport.h"
#include "Widgets/SWidget.h"

// File-local helpers live in a uniquely-named namespace (not an anonymous one): the
// plugin's Unity build merges translation units, and a distinct namespace keeps these
// from colliding with same-named helpers in sibling drive units.
namespace DriveInputStateLocal
{
    // {type, debug_path} for a widget, or JSON null when nothing holds the slot. Null is
    // written explicitly rather than omitting the key, so "nothing has capture/focus" is a
    // reported fact instead of an absent field the caller has to interpret.
    void SetWidgetField(const TSharedPtr<FJsonObject>& Obj, const TCHAR* Field, const TSharedPtr<SWidget>& Widget)
    {
        if (!Widget.IsValid())
        {
            Obj->SetField(Field, MakeShared<FJsonValueNull>());
            return;
        }

        TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetStringField(TEXT("type"), Widget->GetTypeAsString());
        Entry->SetStringField(TEXT("debug_path"), FReflectionMetaData::GetWidgetDebugInfo(Widget.Get()));
        Obj->SetObjectField(Field, Entry);
    }

    // The shared stringifier: strips the "EEnum::" scope so the wire carries the bare
    // member name (CapturePermanently / LockOnCapture) rather than a raw integer.
    using JsonBuilders::EnumMemberName;
}

using namespace DriveInputStateLocal;

// drive.input_state — read-only snapshot of WHO OWNS THE MOUSE right now. The action verbs
// report what settled after an injection; this reports the input-routing state that decided
// where the injection could go in the first place: whether Slate considers the app active,
// which widget holds cursor capture and keyboard focus, whether that is the game viewport,
// and the viewport's capture / lock / cursor policy. It is the read a mouse-capture bug is
// diagnosed with (and the read that distinguishes the Slate injection path from os_input,
// which deliberately leaves slate_active alone).
//
// It takes drive.observe's surface / root selectors for symmetry, but ignores them: the
// state reported here is per-application and per-game-viewport, not per-UMG-root.
REGISTER_RPC_HANDLER("drive.input_state", "drive",
    "Read the live mouse-capture and focus state: Slate active, cursor captor, focused widget, game-viewport capture/lock modes, and OS cursor position.",
    RPC_PARAMS(
        RPC_PARAM_DEF("surface", "string", "Accepted for symmetry with drive.observe and ignored: the reported state is per-application / per-game-viewport, not per-surface.", "auto"),
        RPC_PARAM_OPT("instance_name", "string", "Accepted for symmetry with drive.observe and ignored."),
        RPC_PARAM_OPT("instanceName", "string", "Alias for instance_name."),
        RPC_PARAM_OPT("root_index", "integer", "Accepted for symmetry with drive.observe and ignored."),
        RPC_PARAM_OPT("rootIndex", "integer", "Alias for root_index.")
    ))
{
    if (!FSlateApplication::IsInitialized())
    {
        Ctx.SendError(ErrorCodes::ERR_SLATE_NOT_INITIALIZED,
            TEXT("Slate application is not initialized."));
        return true;
    }

    FSlateApplication& SlateApp = FSlateApplication::Get();
    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();

    // The flag FDriveInput's Slate path temporarily forces and os_input never touches.
    Result->SetBoolField(TEXT("slate_active"), SlateApp.IsActive());

    TSharedPtr<SWidget> Captor;
    TSharedPtr<SWidget> Focused;
    if (TSharedPtr<FSlateUser> CursorUser = SlateApp.GetCursorUser())
    {
        Captor = CursorUser->GetCursorCaptor();
        Focused = CursorUser->GetFocusedWidget();
    }
    SetWidgetField(Result, TEXT("cursor_captor"), Captor);
    SetWidgetField(Result, TEXT("focused_widget"), Focused);

    // The game viewport is optional: with no PIE session running these read null rather
    // than failing, so the verb stays usable for "is anything holding the mouse?" outside PIE.
    UGameViewportClient* ViewportClient = GEngine ? GEngine->GameViewport : nullptr;
    TSharedPtr<SViewport> ViewportWidget = ViewportClient ? ViewportClient->GetGameViewportWidget() : nullptr;

    if (ViewportWidget.IsValid())
    {
        // HasMouseCapture is true for the viewport OR a descendant; is_captor is the exact
        // identity test. They disagree precisely when a child widget stole the capture.
        Result->SetBoolField(TEXT("viewport_has_mouse_capture"), ViewportWidget->HasMouseCapture());
        Result->SetBoolField(TEXT("viewport_is_captor"), Captor.Get() == ViewportWidget.Get());
        Result->SetBoolField(TEXT("viewport_has_focus"), ViewportWidget->HasAnyUserFocus().IsSet());
    }
    else
    {
        Result->SetField(TEXT("viewport_has_mouse_capture"), MakeShared<FJsonValueNull>());
        Result->SetField(TEXT("viewport_is_captor"), MakeShared<FJsonValueNull>());
        Result->SetField(TEXT("viewport_has_focus"), MakeShared<FJsonValueNull>());
    }

    if (ViewportClient)
    {
        Result->SetStringField(TEXT("mouse_capture_mode"),
            EnumMemberName(StaticEnum<EMouseCaptureMode>(), static_cast<int64>(ViewportClient->GetMouseCaptureMode())));
        Result->SetStringField(TEXT("mouse_lock_mode"),
            EnumMemberName(StaticEnum<EMouseLockMode>(), static_cast<int64>(ViewportClient->GetMouseLockMode())));
        Result->SetBoolField(TEXT("hide_cursor_during_capture"), ViewportClient->HideCursorDuringCapture());
    }
    else
    {
        Result->SetField(TEXT("mouse_capture_mode"), MakeShared<FJsonValueNull>());
        Result->SetField(TEXT("mouse_lock_mode"), MakeShared<FJsonValueNull>());
        Result->SetField(TEXT("hide_cursor_during_capture"), MakeShared<FJsonValueNull>());
    }

    APlayerController* PlayerController = nullptr;
    if (ViewportClient)
    {
        if (UWorld* World = ViewportClient->GetWorld())
        {
            PlayerController = World->GetFirstPlayerController();
        }
    }
    if (PlayerController)
    {
        Result->SetBoolField(TEXT("show_mouse_cursor"), PlayerController->bShowMouseCursor);
    }
    else
    {
        Result->SetField(TEXT("show_mouse_cursor"), MakeShared<FJsonValueNull>());
    }

    // Absolute desktop pixels — the same space an element's geometry.absolute reports and
    // the space os_input aims at, so a caller can verify where the pointer actually landed.
    const FVector2D CursorPos(SlateApp.GetCursorPos());
    TSharedPtr<FJsonObject> OsCursor = MakeShared<FJsonObject>();
    OsCursor->SetNumberField(TEXT("x"), CursorPos.X);
    OsCursor->SetNumberField(TEXT("y"), CursorPos.Y);
    Result->SetObjectField(TEXT("os_cursor"), OsCursor);

    Ctx.SendSuccess(Result);
    return true;
}
