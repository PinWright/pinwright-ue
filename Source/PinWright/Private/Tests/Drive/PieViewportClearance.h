// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

// Clears this editor's own regular windows off the level-viewport PIE for a drive test.
//
// The editor stacks regular windows of its own over the level viewport: a host plugin's welcome
// popup (BpGeneratorUltimate's 'ULTIMATE BLUEPRINT GENERATOR'), PinWright's 'PinWright Setup'
// screen, a floating tab restored from the machine-global layout ('Editor Preferences'). Slate
// routes pointer input at the PIE widget's center to whichever of them is on top, so the drive
// verbs correctly refuse with TARGET_OCCLUDED and a PIE input test measured nothing. The
// editor_chrome fixtures (click_occlusion, os_input) avoid this with their own top-most windows
// placed clear of every other window; PIE cannot move out of the level viewport, so this hides
// the windows over it instead and shows them again in Restore().
//
// Only FSlateApplication windows of this process of type Normal are touched, never another
// process's. Toasts (the notification manager re-shows them every frame), menus and tooltips
// are left alone, so the verbs' occlusion check stays the safety net for anything not cleared.
//
// Clear() also releases the game viewport's mouse capture, which PIE takes on start, by putting
// the player in Game-and-UI input mode: while the SViewport holds capture, every injected click,
// hover and focus lands on it and never on the UMG probe (the first live run of these tests).

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "Framework/Application/SlateApplication.h"
#include "Handlers/ErrorCodes.h"
#include "Misc/AutomationTest.h"
#include "Tests/TestUtils.h"
#include "Widgets/SViewport.h"
#include "Widgets/SWindow.h"

struct FPieViewportClearance
{
    TArray<TWeakPtr<SWindow>> Hidden;

    ~FPieViewportClearance() { Restore(); }

    // Hides every visible regular window of this editor that overlaps the PIE viewport (with a
    // margin for points just outside it), except the window holding the viewport and its parents.
    // Give the platform a few frames afterwards: Linux hides a window three frames later.
    void Clear()
    {
        UGameViewportClient* Client = GEngine ? GEngine->GameViewport.Get() : nullptr;
        const TSharedPtr<SViewport> Viewport = Client ? Client->GetGameViewportWidget() : nullptr;
        if (!Viewport.IsValid() || !FSlateApplication::IsInitialized())
        {
            return;
        }
        FSlateApplication& SlateApp = FSlateApplication::Get();
        const TSharedPtr<SWindow> Host = SlateApp.FindWidgetWindow(Viewport.ToSharedRef());
        const FGeometry& Geometry = Viewport->GetTickSpaceGeometry();
        const FSlateRect Rect = FSlateRect::FromPointAndExtent(
            Geometry.GetAbsolutePosition(), Geometry.GetAbsoluteSize()).ExtendBy(FMargin(16.0f));
        HideOver(SlateApp.GetInteractiveTopLevelWindows(), Host, Rect);

        // PIE starts with the game viewport holding the mouse (the level-viewport PIE focuses it
        // and DefaultViewportMouseCaptureMode captures on focus), so every pointer event goes to
        // the SViewport and no UMG widget in it is hit, exactly as for a real mouse until the game
        // shows a cursor for its UI. Put the player in the input mode a game showing clickable UI
        // uses; the local player's Slate operations release the capture this frame.
        UWorld* World = Client->GetWorld();
        if (APlayerController* Player = World ? World->GetFirstPlayerController() : nullptr)
        {
            Player->SetInputMode(FInputModeGameAndUI());
        }
    }

    // Shows the hidden windows again, parents first. A window destroyed meanwhile has no native
    // window left, and SWindow::ShowWindow then does nothing.
    void Restore()
    {
        if (FSlateApplication::IsInitialized())
        {
            for (const TWeakPtr<SWindow>& Weak : Hidden)
            {
                if (const TSharedPtr<SWindow> Window = Weak.Pin())
                {
                    Window->ShowWindow();
                }
            }
        }
        Hidden.Reset();
    }

    // After Clear(), a TARGET_OCCLUDED naming a regular window of this editor means Clear() missed
    // it: reported as a fixture error, not skipped. Returns true when it added that error.
    bool FailIfOwnWindowStillOccludes(FAutomationTestBase& Test, const FTestResponseCapture& Capture) const
    {
        FString Type;
        FString Title;
        if (Capture.bSuccess || Capture.ErrorCode != ErrorCodes::ERR_TARGET_OCCLUDED || !Capture.Result.IsValid()
            || !Capture.Result->TryGetStringField(TEXT("occluding_window_type"), Type) || Type != TEXT("Normal"))
        {
            return false;
        }
        Capture.Result->TryGetStringField(TEXT("occluding_window"), Title);
        Test.AddError(FString::Printf(
            TEXT("fixture: this editor's own window '%s' still covers the PIE viewport after the fixture hid %d window(s) over it: %s"),
            *Title, Hidden.Num(), *Capture.Message));
        return true;
    }

private:
    void HideOver(const TArray<TSharedRef<SWindow>>& Windows, const TSharedPtr<SWindow>& Host, const FSlateRect& Rect)
    {
        for (const TSharedRef<SWindow>& Window : Windows)
        {
            if (Window->IsVisible() && !Window->IsWindowMinimized() && Window->GetType() == EWindowType::Normal
                && !HoldsHost(Window, Host) && FSlateRect::DoRectanglesIntersect(Window->GetRectInScreen(), Rect))
            {
                Window->HideWindow();
                Hidden.Add(Window);
            }
            HideOver(Window->GetChildWindows(), Host, Rect);
        }
    }

    // The host window or one of its parents: hiding it would hide the viewport itself.
    static bool HoldsHost(const TSharedRef<SWindow>& Window, TSharedPtr<SWindow> Host)
    {
        for (; Host.IsValid(); Host = Host->GetParentWindow())
        {
            if (Host == Window)
            {
                return true;
            }
        }
        return false;
    }
};
