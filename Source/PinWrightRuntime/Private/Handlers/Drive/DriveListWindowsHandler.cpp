// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Handlers/HandlerRegistration.h"
#include "Handlers/ParamSpec.h"
#include "Handlers/HandlerContext.h"

#include "Handlers/Drive/DriveEditorChrome.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

// drive.list_windows — enumerate the open top-level editor windows so an agent can
// discover and target editor chrome before observing/acting on it. No params: it
// returns FDriveEditorChrome::ListWindows() (the main frame, asset editors, dialogs,
// and transient popup/menu windows) with each window's title, type, ordered index,
// absolute screen-space geometry, and `maximized` state. The `index` of a window selects
// it via the `window_index` selector on the editor-chrome drive verbs; the `title` selects
// it via `window_title` (substring match). The `maximized` flag is the same signal
// editor.resize_window's WINDOW_MAXIMIZED gate reads, so a caller can tell from list_windows
// whether a window must be restored (editor.set_window_state {state:'restored'}) before
// resize_window will act. (No minimized flag: list_windows enumerates via
// GetAllVisibleWindowsOrdered, which excludes minimized windows, so a minimized window never
// appears here and the field could only ever read false.)
REGISTER_RPC_HANDLER("drive.list_windows", "drive",
    "List open top-level editor windows (title, type, geometry, index, maximized state) so an agent can target editor chrome.",
    RPC_NO_PARAMS)
{
    const TArray<FDriveWindowInfo> Windows = FDriveEditorChrome::ListWindows();

    TArray<TSharedPtr<FJsonValue>> WindowArray;
    WindowArray.Reserve(Windows.Num());
    for (const FDriveWindowInfo& Window : Windows)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetStringField(TEXT("title"), Window.Title);
        Obj->SetStringField(TEXT("type"), Window.Type);
        Obj->SetNumberField(TEXT("index"), Window.Index);

        // Native maximized state — mirrors editor.set_window_state's isMaximized readback so the
        // discovery verb agrees with editor.resize_window's WINDOW_MAXIMIZED gate.
        Obj->SetBoolField(TEXT("maximized"), Window.bMaximized);

        // Geometry mirrors the element shape: geometry.absolute = {x,y,w,h}.
        TSharedPtr<FJsonObject> Absolute = MakeShared<FJsonObject>();
        Absolute->SetNumberField(TEXT("x"), Window.AbsolutePosition.X);
        Absolute->SetNumberField(TEXT("y"), Window.AbsolutePosition.Y);
        Absolute->SetNumberField(TEXT("w"), Window.AbsoluteSize.X);
        Absolute->SetNumberField(TEXT("h"), Window.AbsoluteSize.Y);
        TSharedPtr<FJsonObject> Geometry = MakeShared<FJsonObject>();
        Geometry->SetObjectField(TEXT("absolute"), Absolute);
        Obj->SetObjectField(TEXT("geometry"), Geometry);

        WindowArray.Add(MakeShared<FJsonValueObject>(Obj));
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetArrayField(TEXT("windows"), WindowArray);
    Result->SetNumberField(TEXT("count"), Windows.Num());
    Ctx.SendSuccess(Result);
    return true;
}
