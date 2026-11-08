// Copyright (c) 2026 Alexander Penkin. MIT License.

// editor.resize_window, editor.set_window_state, editor.screenshot_window: the game-safe half of
// the editor module's EditorWindowHandlers.cpp, which keeps editor.frame_graph. Explicit selectors
// use the shared FDriveEditorChrome::ResolveWindow rules; screenshot_window alone defaults an
// omitted selector to the main editor frame (the game viewport's window in a -game process).

#include "Handlers/Editor/EditorWindowHandlers.h"
#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/ParamSpec.h"
#include "Handlers/Drive/DriveEditorChrome.h"
#include "Handlers/Drive/DriveHandlerCommon.h"
#include "Dispatch/PinWrightWorld.h"
#include "Utils/ScreenshotUtils.h"
#include "Utils/RenderingAvailability.h"

#include "CoreGlobals.h"
#include "Dom/JsonObject.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Docking/TabManager.h"
#include "ImageUtils.h"
#include "Math/UnrealMathUtility.h"
#include "Misc/FileHelper.h"
#include "Rendering/SlateRenderer.h"
#include "Widgets/SWindow.h"

// Named (not anonymous) helper namespace: Unity merges .cpp files into one TU.
namespace EditorWindowHandlersRuntimeLocal
{
    // The three window-manager states as one canonical name, so editor.set_window_state can
    // publish what the window MEASURED as after the call in the same vocabulary the caller used
    // to ask for it. Minimized wins over maximized: Windows keeps the zoomed flag set on a
    // window it iconifies (::IsZoomed stays true under ::IsIconic), so testing maximized first
    // would report a minimized-from-maximized window as "maximized" - the exact misreport this
    // readback exists to prevent.
    FString DescribeEditorWindowStateName(bool bMinimized, bool bMaximized)
    {
        if (bMinimized)
        {
            return TEXT("minimized");
        }
        return bMaximized ? TEXT("maximized") : TEXT("restored");
    }
}

using namespace EditorWindowHandlersRuntimeLocal;
using namespace EditorWindowHandlers;

// Node/graph editors build their node widgets via a deferred SGraphPanel::Update() active
// timer, and bounds/desired-sizes are valid only after a SlatePrepass. Pump one full frame
// (and force-redraw the host window) so the panel paints before we query bounds or capture.
// Salvaged from the deleted BlueprintGraphScreenshotHandler::PumpGraphEditorSlate.
void EditorWindowHandlers::PumpEditorSlate(const TSharedPtr<SWindow>& HostWindow)
{
    FSlateApplication& SlateApp = FSlateApplication::Get();
    SlateApp.PumpMessages();
    SlateApp.Tick(ESlateTickType::All);
    if (HostWindow.IsValid())
    {
        SlateApp.ForceRedrawWindow(HostWindow.ToSharedRef());
    }
    if (FSlateRenderer* Renderer = SlateApp.GetRenderer())
    {
        Renderer->FlushCommands();
    }
}

// Parse the optional `aspect` param: either a "W:H" string (e.g. "16:9") or a bare positive
// float ratio (W/H). Sets bHasAspect + Ratio. Returns false with OutError only when aspect is
// present but malformed; an absent aspect returns true with bHasAspect=false. Salvaged from
// the deleted handler's aspect parsing.
bool EditorWindowHandlers::ParseAspectParam(const FHandlerContext& Ctx, bool& bHasAspect, double& Ratio, FString& OutError)
{
    bHasAspect = false;
    Ratio = 0.0;
    OutError.Reset();

    const TSharedPtr<FJsonObject>& Payload = Ctx.GetRawPayload();
    if (!Payload.IsValid())
    {
        return true;
    }
    const TSharedPtr<FJsonValue>* AspectField = Payload->Values.Find(TEXT("aspect"));
    if (!AspectField || !AspectField->IsValid())
    {
        return true;
    }

    const TSharedPtr<FJsonValue>& AspectVal = *AspectField;
    if (AspectVal->Type == EJson::String)
    {
        const FString AspectStr = AspectVal->AsString();
        FString WPart, HPart;
        if (AspectStr.Split(TEXT(":"), &WPart, &HPart))
        {
            const double W = FCString::Atod(*WPart.TrimStartAndEnd());
            const double H = FCString::Atod(*HPart.TrimStartAndEnd());
            if (W > 0.0 && H > 0.0)
            {
                Ratio = W / H;
                bHasAspect = true;
            }
        }
        else
        {
            const double R = FCString::Atod(*AspectStr.TrimStartAndEnd());
            if (R > 0.0)
            {
                Ratio = R;
                bHasAspect = true;
            }
        }
    }
    else if (AspectVal->Type == EJson::Number)
    {
        Ratio = AspectVal->AsNumber();
        bHasAspect = Ratio > 0.0;
    }

    if (!bHasAspect)
    {
        OutError = TEXT("aspect must be a positive 'W:H' string (e.g. '16:9') or a positive float ratio");
        return false;
    }
    return true;
}

TSharedPtr<FJsonObject> EditorWindowHandlers::MakePointObject(const FVector2f& P)
{
    TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
    Obj->SetNumberField(TEXT("x"), P.X);
    Obj->SetNumberField(TEXT("y"), P.Y);
    return Obj;
}

bool EditorWindowHandlers::ResolveScreenshotWindow(
    const FDriveWindowSelector& Selector,
    TSharedPtr<SWindow>& OutWindow,
    FString& OutTitle,
    FString& OutErrorCode,
    FString& OutErrorMessage)
{
    OutWindow.Reset();
    OutTitle.Reset();
    OutErrorCode.Reset();
    OutErrorMessage.Reset();

    if (!FSlateApplication::IsInitialized())
    {
        OutErrorCode = TEXT("SLATE_NOT_INITIALIZED");
        OutErrorMessage = TEXT("Slate application is not initialized");
        return false;
    }

    if (!Selector.Title.IsEmpty() || Selector.Index.IsSet())
    {
        return FDriveEditorChrome::ResolveWindow(
            Selector, OutWindow, OutTitle, OutErrorCode, OutErrorMessage);
    }

    const bool bGameProcess = PinWright::IsGameProcess();
    if (bGameProcess)
    {
        // A game process has no main frame: the window its game viewport renders into.
        if (GEngine && GEngine->GameViewport)
        {
            OutWindow = GEngine->GameViewport->GetWindow();
        }
    }
    else
    {
        // The MainFrame module publishes its root window here (MainFrameHandler), which is the
        // window IMainFrameModule::GetParentWindow returns; read without linking MainFrame.
        OutWindow = FGlobalTabmanager::Get()->GetRootWindow();
    }
    if (!OutWindow.IsValid() || !OutWindow->IsVisible() || OutWindow->IsWindowMinimized())
    {
        OutWindow.Reset();
        OutErrorCode = TEXT("WINDOW_NOT_FOUND");
        OutErrorMessage = bGameProcess
            ? TEXT("The game viewport window is unavailable, hidden, or minimized")
            : TEXT("The main editor frame is unavailable, hidden, or minimized");
        return false;
    }

    OutTitle = OutWindow->GetTitle().ToString();
    return true;
}

void EditorWindowHandlers::SetScreenshotWindowIdentityFields(
    FJsonObject& Result, const FString& WindowTitle, EWindowType WindowType)
{
    Result.SetStringField(TEXT("windowTitle"), WindowTitle);
    Result.SetStringField(TEXT("windowType"), FDriveEditorChrome::WindowTypeToString(WindowType));
}

// ---- editor.resize_window ----
REGISTER_RPC_HANDLER("editor.resize_window", "editor",
    "Resize a top-level editor window's CLIENT area (chrome excluded). Target the window with the shared "
    "window selector. Provide width/height (px) and/or an aspect ratio; pass logical=true to treat the "
    "sizes as DPI-independent (multiplied by the window DPI scale for the physical resize). A maximized "
    "window errors WINDOW_MAXIMIZED (restore it first via editor.set_window_state {state:'restored'}).",
    RPC_PARAMS(
        DRIVE_WINDOW_SELECTOR_PARAMS,
        RPC_PARAM_OPT("width", "integer", "Target client width in pixels. Combine with height, or with aspect to derive height."),
        RPC_PARAM_OPT("height", "integer", "Target client height in pixels. Combine with width, or with aspect to derive width."),
        RPC_PARAM_OPT("aspect", "string",
            "Target aspect ratio (a 'W:H' string like '16:9' or a bare float W/H). The missing dimension is derived "
            "from the provided one (or from the current width when neither width nor height is given)."),
        RPC_PARAM_DEF("logical", "boolean",
            "When true, width/height are DPI-independent logical pixels and are multiplied by the window DPI scale "
            "for the physical resize. Default false (sizes are physical pixels).", "false")
    ))
{
    if (!FSlateApplication::IsInitialized())
    {
        Ctx.SendError(TEXT("SLATE_NOT_INITIALIZED"), TEXT("Slate application is not initialized"));
        return true;
    }

    TSharedPtr<SWindow> Window;
    FString WindowTitle;
    FString ErrorCode;
    FString ErrorMessage;
    if (!FDriveEditorChrome::ResolveWindow(
            FDriveHandlerCommon::ParseWindowSelector(Ctx), Window, WindowTitle, ErrorCode, ErrorMessage))
    {
        Ctx.SendError(ErrorCode, ErrorMessage);
        return true;
    }

    const bool bLogical = Ctx.GetBool(TEXT("logical"), false);
    const TOptional<int32> WidthOpt = Ctx.GetIntFirstOf({ TEXT("width") });
    const TOptional<int32> HeightOpt = Ctx.GetIntFirstOf({ TEXT("height") });

    bool bHasAspect = false;
    double AspectRatio = 0.0;
    FString AspectError;
    if (!ParseAspectParam(Ctx, bHasAspect, AspectRatio, AspectError))
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"), AspectError);
        return true;
    }

    if (!WidthOpt.IsSet() && !HeightOpt.IsSet() && !bHasAspect)
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("Provide width and/or height, or an aspect ratio"));
        return true;
    }

    const bool bWasMaximized = Window->IsWindowMaximized();
    if (bWasMaximized)
    {
        Ctx.SendError(TEXT("WINDOW_MAXIMIZED"),
            FString::Printf(TEXT("Window '%s' is maximized and will not visibly resize; restore it first via "
                "editor.set_window_state {state:'restored'}, then retry"), *WindowTitle));
        return true;
    }

    const float Dpi = FMath::Max(Window->GetDPIScaleFactor(), KINDA_SMALL_NUMBER);
    const FVector2f CurrentClient = Window->GetClientSizeInScreen();
    // Current client size expressed in the caller's chosen space (logical or physical).
    const double CurW = bLogical ? static_cast<double>(CurrentClient.X) / Dpi : static_cast<double>(CurrentClient.X);
    const double CurH = bLogical ? static_cast<double>(CurrentClient.Y) / Dpi : static_cast<double>(CurrentClient.Y);

    double ReqW = 0.0;
    double ReqH = 0.0;
    if (bHasAspect)
    {
        if (WidthOpt.IsSet())
        {
            ReqW = WidthOpt.GetValue();
            ReqH = ReqW / AspectRatio;
        }
        else if (HeightOpt.IsSet())
        {
            ReqH = HeightOpt.GetValue();
            ReqW = ReqH * AspectRatio;
        }
        else
        {
            ReqW = CurW;
            ReqH = ReqW / AspectRatio;
        }
    }
    else
    {
        ReqW = WidthOpt.IsSet() ? static_cast<double>(WidthOpt.GetValue()) : CurW;
        ReqH = HeightOpt.IsSet() ? static_cast<double>(HeightOpt.GetValue()) : CurH;
    }

    if (ReqW < 1.0 || ReqH < 1.0)
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("Resolved client size must be at least 1x1 px"));
        return true;
    }

    // SWindow::Resize takes the DPI-scaled (physical) client size.
    const double PhysW = bLogical ? ReqW * Dpi : ReqW;
    const double PhysH = bLogical ? ReqH * Dpi : ReqH;

    Window->Resize(FVector2f(static_cast<float>(PhysW), static_cast<float>(PhysH)));
    PumpEditorSlate(Window);

    const FVector2f NewClient = Window->GetClientSizeInScreen();
    const FVector2f NewWindow = Window->GetSizeInScreen();
    const FVector2f NewPos = Window->GetPositionInScreen();

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("windowTitle"), WindowTitle);
    Result->SetObjectField(TEXT("requestedClientSize"), MakePointObject(FVector2f(static_cast<float>(PhysW), static_cast<float>(PhysH))));
    Result->SetObjectField(TEXT("clientSize"), MakePointObject(NewClient));
    Result->SetObjectField(TEXT("windowSize"), MakePointObject(NewWindow));
    Result->SetObjectField(TEXT("position"), MakePointObject(NewPos));
    Result->SetNumberField(TEXT("dpiScale"), Dpi);
    Result->SetBoolField(TEXT("wasMaximized"), bWasMaximized);
    // The window manager, or Slate itself, may keep a different size (a notification toast is
    // re-sized to its content every frame); say so rather than echo the request as done.
    if (FMath::Abs(NewClient.X - PhysW) > 1.0 || FMath::Abs(NewClient.Y - PhysH) > 1.0)
    {
        Result->SetStringField(TEXT("warning"), FString::Printf(
            TEXT("Requested client size %.0fx%.0f but the window measured %.0fx%.0f afterwards; the resize did not take.%s"),
            PhysW, PhysH, NewClient.X, NewClient.Y,
            Window->GetType() == EWindowType::Notification
                ? TEXT(" Notification toasts size themselves; close them with editor.dismiss_notifications.")
                : TEXT("")));
    }
    Ctx.SendSuccess(Result);
    return true;
}

// ---- editor.set_window_state ----
REGISTER_RPC_HANDLER("editor.set_window_state", "editor",
    "Set a top-level editor window's maximize/minimize/restore state. Target the window with the shared "
    "window selector. state: 'restored' (aka 'normal' - un-maximize AND un-minimize to a normal framed "
    "window), 'maximized', or 'minimized'. This is the callable recovery for editor.resize_window's "
    "WINDOW_MAXIMIZED error: set state='restored' first, then resize (the editor commonly launches "
    "maximized). Unlike every other window verb this one CAN target a minimized window - state='restored' "
    "against a minimized editor is the in-band recovery, and needs no selector when the whole editor is "
    "minimized. Reports the before/after maximized+minimized flags, the MEASURED resulting state read back "
    "after the call (with a warning when it differs from the requested one), and the resulting client size.",
    RPC_PARAMS(
        DRIVE_WINDOW_SELECTOR_PARAMS,
        RPC_PARAM_REQ("state", "string",
            "Target window state: 'restored' (or 'normal' - un-maximize and un-minimize to a normal "
            "framed window), 'maximized', or 'minimized'.")
    ))
{
    if (!FSlateApplication::IsInitialized())
    {
        Ctx.SendError(TEXT("SLATE_NOT_INITIALIZED"), TEXT("Slate application is not initialized"));
        return true;
    }

    FString StateStr;
    if (!Ctx.RequireString(TEXT("state"), StateStr))
    {
        return true;
    }
    const FString State = StateStr.TrimStartAndEnd().ToLower();

    enum class EDesiredWindowState { Restored, Maximized, Minimized };
    EDesiredWindowState Desired;
    if (State == TEXT("restored") || State == TEXT("restore") || State == TEXT("normal"))
    {
        Desired = EDesiredWindowState::Restored;
    }
    else if (State == TEXT("maximized") || State == TEXT("maximize"))
    {
        Desired = EDesiredWindowState::Maximized;
    }
    else if (State == TEXT("minimized") || State == TEXT("minimize"))
    {
        Desired = EDesiredWindowState::Minimized;
    }
    else
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"),
            TEXT("state must be one of: 'restored' (or 'normal'), 'maximized', 'minimized'"));
        return true;
    }

    // Minimizing the editor is the one direction of this verb that can poison an in-flight
    // measurement: a minimized window stops rendering, so a profile taken against it reads a
    // frozen last value in every column (a hard ~333 ms frame cap, DrawCalls 0, a bit-identical
    // GameThreadTime) with nothing out of range to flag it. GIsAutomationTesting spans the whole
    // automation session (AutomationTest.cpp:1218/:1226, not just one test), so refuse rather
    // than hand a running suite a silently-wrong scene cost. Restore and maximize are unaffected;
    // the recovery direction must never be gated.
    if (Desired == EDesiredWindowState::Minimized && GIsAutomationTesting)
    {
        Ctx.SendError(TEXT("WINDOW_MINIMIZE_REFUSED"),
            TEXT("Refusing to minimize a window while an automation session is running: a minimized "
                 "window stops rendering, and every profiling/render number taken afterwards reports a "
                 "frozen last value that looks like a scene regression. Re-issue after the suite ends."));
        return true;
    }

    TSharedPtr<SWindow> Window;
    FString WindowTitle;
    FString ErrorCode;
    FString ErrorMessage;
    // The ONLY caller that opts into minimized-tolerant resolution. Every other consumer of the
    // shared selector (drive observe/act/capture, editor.frame_graph, editor.resize_window,
    // editor.screenshot_window) keeps the visible-only set: they read or act on a window's
    // contents, which a minimized window does not have. This verb acts on the window's own
    // window-manager state, so a minimized window is exactly the target it exists to fix.
    if (!FDriveEditorChrome::ResolveWindow(
            FDriveHandlerCommon::ParseWindowSelector(Ctx), Window, WindowTitle, ErrorCode, ErrorMessage,
            /*bIncludeMinimizedWindows=*/true))
    {
        Ctx.SendError(ErrorCode, ErrorMessage);
        return true;
    }

    // A window matched, so NO_WINDOWS / WINDOW_NOT_FOUND are no longer honest answers - but a
    // window with no native platform window (a virtual/offscreen SWindow) makes SWindow::Restore /
    // Maximize / Minimize literal no-ops (SWindow.cpp:1754-1776 each guard on NativeWindow), and
    // reporting a readback from one is a fake success. Refuse with a code that says which of the
    // two failures happened: matched, but its state cannot be changed.
    if (!Window->GetNativeWindow().IsValid())
    {
        Ctx.SendError(TEXT("WINDOW_STATE_NOT_CHANGEABLE"),
            FString::Printf(
                TEXT("Window '%s' matched the selector but has no native platform window, so its "
                     "maximize/minimize/restore state cannot be changed"), *WindowTitle));
        return true;
    }

    const bool bWasMaximized = Window->IsWindowMaximized();
    const bool bWasMinimized = Window->IsWindowMinimized();

    switch (Desired)
    {
    case EDesiredWindowState::Restored:  Window->Restore();  break;
    case EDesiredWindowState::Maximized: Window->Maximize(); break;
    case EDesiredWindowState::Minimized: Window->Minimize(); break;
    }
    // Let the native window-mode change settle before reading back the resulting state/size.
    PumpEditorSlate(Window);

    const bool bIsMaximized = Window->IsWindowMaximized();
    const bool bIsMinimized = Window->IsWindowMinimized();
    const FVector2f Client = Window->GetClientSizeInScreen();

    // What the window ACTUALLY is now, read back from the native window - not what was asked for.
    // The window manager is free to decline (an offscreen host under -RenderOffscreen routinely
    // does), and a response that echoed only the request would report that decline as a success.
    const FString MeasuredState = DescribeEditorWindowStateName(bIsMinimized, bIsMaximized);
    const FString DesiredState = DescribeEditorWindowStateName(
        Desired == EDesiredWindowState::Minimized, Desired == EDesiredWindowState::Maximized);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("windowTitle"), WindowTitle);
    Result->SetStringField(TEXT("requestedState"), State);
    Result->SetStringField(TEXT("state"), MeasuredState);
    Result->SetBoolField(TEXT("stateMatchesRequest"), MeasuredState == DesiredState);
    Result->SetBoolField(TEXT("wasMaximized"), bWasMaximized);
    Result->SetBoolField(TEXT("wasMinimized"), bWasMinimized);
    Result->SetBoolField(TEXT("isMaximized"), bIsMaximized);
    Result->SetBoolField(TEXT("isMinimized"), bIsMinimized);
    Result->SetObjectField(TEXT("clientSize"), MakePointObject(Client));
    if (MeasuredState != DesiredState)
    {
        Result->SetStringField(TEXT("warning"), FString::Printf(
            TEXT("Requested state '%s' but the window measured '%s' afterwards; the window manager "
                 "declined the change (common for an offscreen window under -RenderOffscreen)"),
            *DesiredState, *MeasuredState));
    }
    if (bIsMinimized)
    {
        // The window is left in the one state that makes every other verb refuse it and every
        // render/profiling number meaningless. Carry the exact call that undoes it, so the
        // recovery never has to be discovered from outside the RPC surface again.
        Result->SetStringField(TEXT("recovery"), FString::Printf(
            TEXT("editor.set_window_state {\"window_title\": \"%s\", \"state\": \"restored\"}"),
            *WindowTitle));
    }
    Ctx.SendSuccess(Result);
    return true;
}

// ---- editor.screenshot_window ----
REGISTER_RPC_HANDLER("editor.screenshot_window", "editor",
    "Capture a full top-level editor window (including its chrome) to a PNG under Saved/Screenshots/EditorWindow. "
    "With no selector, captures the exact main editor frame (in a -game process, the game viewport's window); explicit selectors use the shared window resolver. "
    "Returns windowTitle and windowType for the captured window. Uses FSlateApplication::TakeScreenshot rooted "
    "at the window widget with alpha forced opaque.",
    RPC_PARAMS(
        RPC_PARAM_OPT("window_title", "string",
            "Visible top-level window-title substring. With no non-empty title or index, captures the main editor frame."),
        RPC_PARAM_OPT("title", "string", "Alias for window_title; window_title wins when both are non-empty."),
        RPC_PARAM_OPT("window_index", "integer",
            "Nth visible top-level window (0-based, see drive.list_windows). Takes precedence over window_title."),
        RPC_PARAM_OPT("index", "integer", "Alias for window_index."),
        RPC_PARAM_OPT("filename", "filepath",
            "Output filename inside Saved/Screenshots/EditorWindow. The .png extension is appended if missing.")
    ))
{
    if (!PinWrightRendering::RequireRenderer(Ctx))
    {
        return true;
    }

    TSharedPtr<SWindow> Window;
    FString WindowTitle;
    FString ErrorCode;
    FString ErrorMessage;
    const FDriveWindowSelector Selector = FDriveHandlerCommon::ParseWindowSelector(Ctx);
    if (!EditorWindowHandlers::ResolveScreenshotWindow(
            Selector, Window, WindowTitle, ErrorCode, ErrorMessage))
    {
        Ctx.SendError(ErrorCode, ErrorMessage);
        return true;
    }

    TArray<FColor> Bitmap;
    FIntVector SizeVec;
    // Never FSlateApplication::TakeScreenshot directly: a window the pass does not draw leaves the
    // renderer armed with a pointer to this frame. Contract in ScreenshotUtils.h.
    const bool bCaptured = PinWrightScreenshotUtils::TakeSlateScreenshot(
        Window.ToSharedRef(), Bitmap, SizeVec);
    if (!bCaptured || SizeVec.X <= 0 || SizeVec.Y <= 0 || Bitmap.Num() == 0)
    {
        Ctx.SendError(TEXT("CAPTURE_FAILED"),
            FString::Printf(TEXT("Failed to capture window '%s'"), *WindowTitle));
        return true;
    }

    // Force alpha opaque (Slate may leave it non-255); contract in ScreenshotUtils.h.
    PinWrightScreenshotUtils::ForceOpaqueAlpha(Bitmap);

    TArray64<uint8> PngData;
    FImageUtils::PNGCompressImageArray(SizeVec.X, SizeVec.Y,
        TArrayView64<const FColor>(Bitmap.GetData(), Bitmap.Num()), PngData);
    if (PngData.Num() == 0)
    {
        Ctx.SendError(TEXT("ENCODE_FAILED"), TEXT("Failed to encode editor window screenshot as PNG"));
        return true;
    }

    TArray<uint8> PngBytes;
    PngBytes.Reset(PngData.Num());
    PngBytes.Append(PngData.GetData(), PngData.Num());

    FString Filename;
    const FString OutputPath = PinWrightScreenshotUtils::MakeScreenshotOutputPath(
        Ctx.GetString(TEXT("filename")),
        TEXT("EditorWindow"),
        TEXT("EditorWindow"),
        Filename);
    if (!FFileHelper::SaveArrayToFile(PngBytes, *OutputPath))
    {
        Ctx.SendError(TEXT("SAVE_FAILED"),
            FString::Printf(TEXT("Failed to save editor window screenshot: %s"), *OutputPath));
        return true;
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("path"), OutputPath);
    Result->SetStringField(TEXT("filename"), Filename);
    Result->SetNumberField(TEXT("width"), SizeVec.X);
    Result->SetNumberField(TEXT("height"), SizeVec.Y);
    Result->SetNumberField(TEXT("sizeBytes"), PngBytes.Num());
    EditorWindowHandlers::SetScreenshotWindowIdentityFields(
        *Result, WindowTitle, Window->GetType());
    Result->SetStringField(TEXT("mimeType"), TEXT("image/png"));
    Ctx.SendSuccess(Result);
    return true;
}
