// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

// Shared Slate-pump and designer-resolution helpers used by the widget.screenshot_designer
// RPC handler and the asset.dump preview-capture aspect. Header-only inline helpers so
// callers from different translation units share one implementation without an extra
// .cpp / module-level export.

#include "CoreMinimal.h"
#include "CoreGlobals.h"
#include "Application/ThrottleManager.h"
#include "Compat/EngineVersionCompat.h"
#include "BlueprintModes/WidgetBlueprintApplicationModes.h"
#include "Editor.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Docking/TabManager.h"
#include "HAL/PlatformProcess.h"
#include "Handlers/ErrorCodes.h"
#include "Handlers/Render/CaptureSubject.h"
#include "Handlers/UI/WidgetGeometryResolver.h"
#include "Misc/ScopeExit.h"
#include "Rendering/SlateRenderer.h"
#include "Selection.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Toolkits/IToolkitHost.h"
#include "WidgetBlueprint.h"
#include "WidgetBlueprintEditor.h"
#include "Widgets/SWidget.h"
#include "Widgets/SWindow.h"

namespace WidgetDesignerCaptureInternal
{
    inline constexpr int32 MaxSlateResolveAttempts = 12;

    // OPEN THE DESIGNER FOR A CAPTURE AND PUT THE EDITOR BACK THE WAY IT WAS FOUND.
    //
    // A Designer capture has to open the Widget Blueprint editor - the preview UUserWidget only
    // exists while the toolkit does. Leaving it open afterwards was this plugin's own bug: a
    // read-only capture verb manufactured the "asset editor open across a rebuild of its own
    // asset" precondition, and the next compile of that widget then took the whole editor process
    // down (board B-screenshot-designer-leaves-designer-open-compile-crash). The compile side is
    // fixed independently in Handlers/UI/WidgetDesignerCompileGuard.h; this closes the leak that
    // made a review loop walk into it by following the documentation.
    //
    // The three-state close rule and the deferred close are the render.capture_asset_preview
    // machinery, reused rather than re-spelled: absent closes only a window this call opened, an
    // explicit true closes one the caller already had open, false leaves it. The close is QUEUED
    // onto the core ticker, never run on this stack - destroying a toolkit inside a capture is its
    // own crash (PinWrightCaptureSubject::ScheduleDeferredAssetEditorClose).
    //
    // NESTING IS THE POINT. The screenshot handler opens the editor, then CapturePreviewToPng
    // opens it again through its own scope; the inner scope sees bWasAlreadyOpen and leaves the
    // window alone, so exactly one scope - the outermost, the one that actually opened it - owns
    // the close.
    struct FScopedDesignerAssetEditor
    {
        FScopedDesignerAssetEditor(
            UWidgetBlueprint* InBlueprint,
            bool bInCloseAfterCapture = true,
            bool bInCloseRequestedExplicitly = false)
            : Blueprint(InBlueprint)
            , bCloseAfterCapture(bInCloseAfterCapture)
            , bCloseRequestedExplicitly(bInCloseRequestedExplicitly)
        {
            UAssetEditorSubsystem* AssetEditorSubsystem = GEditor
                ? GEditor->GetEditorSubsystem<UAssetEditorSubsystem>()
                : nullptr;
            if (!AssetEditorSubsystem || !InBlueprint)
            {
                return;
            }
            bWasAlreadyOpen =
                AssetEditorSubsystem->FindEditorForAsset(InBlueprint, /*bFocusIfOpen=*/false) != nullptr;
            bOpenSucceeded = AssetEditorSubsystem->OpenEditorForAsset(InBlueprint);
        }

        ~FScopedDesignerAssetEditor()
        {
            Restore();
        }

        FScopedDesignerAssetEditor(const FScopedDesignerAssetEditor&) = delete;
        FScopedDesignerAssetEditor& operator=(const FScopedDesignerAssetEditor&) = delete;

        // OpenEditorForAsset succeeded (true whether it opened one or found one already open).
        bool IsOpen() const { return bOpenSucceeded; }

        // The caller already had this asset's editor open before the capture.
        bool WasAlreadyOpen() const { return bWasAlreadyOpen; }

        // Apply the close rule now and report the MEASURED outcome: true only when the window is
        // gone as this returns. A queued close reports false, and
        // PinWrightCaptureSubject::HasPendingDeferredAssetEditorClose reports true beside it - the
        // same pair render.capture_asset_preview publishes as assetEditorClosed /
        // assetEditorCloseDeferred. Idempotent; the destructor calls it for every early-return
        // path so a refused capture leaks no window either.
        bool Restore()
        {
            if (bRestored)
            {
                return bClosed;
            }
            bRestored = true;
            bClosed = PinWrightCaptureSubject::CloseAssetEditor(
                Blueprint.Get(), bCloseAfterCapture, bCloseRequestedExplicitly, bWasAlreadyOpen);
            return bClosed;
        }

    private:
        TWeakObjectPtr<UWidgetBlueprint> Blueprint;
        bool bCloseAfterCapture = true;
        bool bCloseRequestedExplicitly = false;
        bool bWasAlreadyOpen = false;
        bool bOpenSucceeded = false;
        bool bRestored = false;
        bool bClosed = false;
    };

    // Keep the decision separate from the live editor query so automation can cover the
    // startup/package-loading branch without racing a real load. Designer mode may only
    // be ticked when package loading is idle and the typed element selection set exists.
    struct FDesignerCaptureReadinessState
    {
        bool bEditorLoadingPackage = false;
        bool bEditorSelectionSetAvailable = false;
    };

    inline bool IsDesignerCaptureReady(
        const FDesignerCaptureReadinessState& State,
        FString& OutError)
    {
        OutError.Reset();
        if (State.bEditorLoadingPackage || !State.bEditorSelectionSetAvailable)
        {
            OutError = ErrorCodes::ERR_EDITOR_NOT_READY;
            return false;
        }
        return true;
    }

    inline bool QueryDesignerCaptureReadiness(FString& OutError)
    {
        OutError.Reset();
        if (!GEditor)
        {
            OutError = ErrorCodes::ERR_EDITOR_NOT_AVAILABLE;
            return false;
        }

        USelection* SelectedActors = GEditor->GetSelectedActors();
        const bool bEditorSelectionSetAvailable =
            SelectedActors && SelectedActors->GetElementSelectionSet();

        FDesignerCaptureReadinessState State;
        State.bEditorLoadingPackage = MCP_IS_EDITOR_LOADING_PACKAGE;
        State.bEditorSelectionSetAvailable = bEditorSelectionSetAvailable;
        return IsDesignerCaptureReady(State, OutError);
    }

    inline TSharedPtr<SWindow> FindWidgetEditorHostWindow(FWidgetBlueprintEditor* WidgetEditor)
    {
        if (!WidgetEditor)
        {
            return nullptr;
        }

        TSharedRef<IToolkitHost> ToolkitHost = WidgetEditor->GetToolkitHost();
        TSharedRef<SWidget> ParentWidget = ToolkitHost->GetParentWidget();
        return FSlateApplication::Get().FindWidgetWindow(ParentWidget);
    }

    inline bool PumpDesignerSlate(
        const TSharedPtr<SWindow>& WindowToRedraw,
        FString* OutError = nullptr)
    {
        FString ReadinessError;
        if (!QueryDesignerCaptureReadiness(ReadinessError))
        {
            if (OutError)
            {
                *OutError = MoveTemp(ReadinessError);
            }
            return false;
        }

        if (OutError)
        {
            OutError->Reset();
        }

        FSlateApplication& SlateApp = FSlateApplication::Get();
        SlateApp.PumpMessages();
        SlateApp.Tick(ESlateTickType::All);
        if (WindowToRedraw.IsValid())
        {
            SlateApp.ForceRedrawWindow(WindowToRedraw.ToSharedRef());
        }
        if (FSlateRenderer* Renderer = SlateApp.GetRenderer())
        {
            Renderer->FlushCommands();
        }
        return true;
    }

    // Switch to designer mode + invoke the SlatePreview tab. RefreshPreview() is invoked
    // only when there is no preview UUserWidget yet — calling it on an existing preview
    // re-destroys the UUserWidget via OnPreviewNeedsRecreation, which races SDesignerView::Tick
    // and prevents TakeWidget from ever populating the cached Slate widget.
    inline TSharedPtr<SWindow> OpenAndInvokeDesigner(
        FWidgetBlueprintEditor* WidgetEditor,
        FString* OutError = nullptr)
    {
        if (OutError)
        {
            OutError->Reset();
        }

        if (!WidgetEditor)
        {
            return nullptr;
        }

        FString ReadinessError;
        if (!QueryDesignerCaptureReadiness(ReadinessError))
        {
            if (OutError)
            {
                *OutError = MoveTemp(ReadinessError);
            }
            return nullptr;
        }

        WidgetEditor->SetCurrentMode(FWidgetBlueprintApplicationModes::DesignerMode);
        if (TSharedPtr<FTabManager> TabManager = WidgetEditor->GetToolkitHost()->GetTabManager())
        {
            TabManager->TryInvokeTab(FTabId(FName(TEXT("SlatePreview"))));
        }
        if (WidgetEditor->GetPreview() == nullptr)
        {
            WidgetEditor->RefreshPreview();
        }

        TSharedPtr<SWindow> HostWindow = FindWidgetEditorHostWindow(WidgetEditor);
        if (!PumpDesignerSlate(HostWindow, OutError))
        {
            return nullptr;
        }
        return HostWindow;
    }

    inline TSharedPtr<SWindow> FindWidgetEditorHostWindowWithRetry(
        FWidgetBlueprintEditor* WidgetEditor,
        FString* OutError = nullptr)
    {
        if (OutError)
        {
            OutError->Reset();
        }

        TSharedPtr<SWindow> HostWindow;
        for (int32 Attempt = 0; Attempt < MaxSlateResolveAttempts; ++Attempt)
        {
            HostWindow = FindWidgetEditorHostWindow(WidgetEditor);
            if (HostWindow.IsValid())
            {
                return HostWindow;
            }
            if (Attempt + 1 >= MaxSlateResolveAttempts)
            {
                break;
            }
            if (!PumpDesignerSlate(nullptr, OutError))
            {
                return nullptr;
            }
        }
        return HostWindow;
    }

    inline bool ResolveDesignerPreviewTargetWithRetry(
        UWidgetBlueprint* WidgetBlueprint,
        FWidgetBlueprintEditor* WidgetEditor,
        FWidgetDesignerPreviewTarget& OutTarget,
        FString& OutError,
        bool bDesignerAlreadyOpen = false)
    {
        OutError.Reset();
        TSharedPtr<SWindow> HostWindow = bDesignerAlreadyOpen
            ? FindWidgetEditorHostWindow(WidgetEditor)
            : OpenAndInvokeDesigner(WidgetEditor, &OutError);
        if (!OutError.IsEmpty())
        {
            return false;
        }

        // Forcibly disable Slate throttling during the retry loop. ThrottleManager.h
        // documents DisableThrottle as the supported primitive for "interactive actions
        // that require multiple refreshes & world ticks" — exactly our case, where each
        // Tick must traverse widgets so SDesignerView::Tick can call TakeWidget.
        // DisableThrottle is reference-counted, so the matching DisableThrottle(false)
        // in ON_SCOPE_EXIT must run exactly once whether we succeed or fall through.
        FSlateThrottleManager::Get().DisableThrottle(true);
        ON_SCOPE_EXIT
        {
            FSlateThrottleManager::Get().DisableThrottle(false);
        };

        for (int32 Attempt = 0; Attempt < MaxSlateResolveAttempts; ++Attempt)
        {
            if (!QueryDesignerCaptureReadiness(OutError))
            {
                return false;
            }

            if (FWidgetGeometryResolver::ResolveDesignerPreviewTarget(
                WidgetBlueprint, true, OutTarget, &OutError))
            {
                return true;
            }
            if (Attempt + 1 >= MaxSlateResolveAttempts)
            {
                break;
            }
            // Pump only — never RefreshPreview here. Rebuilding mid-retry re-nulls
            // SDesignerView::PreviewWidget before Slate has a chance to call TakeWidget.
            if (!PumpDesignerSlate(HostWindow, &OutError))
            {
                return false;
            }
            // Yield 5ms so OS-level paint/activation messages dispatch between Slate ticks;
            // without this, back-to-back PumpMessages can starve native window callbacks on Win32.
            FPlatformProcess::Sleep(0.005f);
        }
        return false;
    }
}
