// Copyright (c) 2026 Alexander Penkin. MIT License.

// editor.dismiss_notifications - close the editor's notification toasts. Each toast is its own
// untitled top-most window, so a pointer action aimed under one is refused TARGET_OCCLUDED. The
// notification manager re-places and re-sizes those windows every frame, so editor.resize_window
// cannot clear one, and a persistent toast (no expiry, e.g. a setup warning) never leaves on its
// own. This fades each toast out the way its own expiry would, with a zero-length fade, then
// reports which windows Slate actually closed.

#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/ParamSpec.h"
#include "Handlers/ErrorCodes.h"

#include "Containers/Ticker.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Notifications/NotificationManager.h"
#include "HAL/PlatformTime.h"
#include "Layout/Children.h"
#include "Widgets/Notifications/SNotificationList.h"
#include "Widgets/SWindow.h"
#include "Widgets/Text/STextBlock.h"

namespace EditorNotificationHandlerLocal
{
    // Fade-out completes on the next core tick, the manager drops the window on its following
    // tick, and Slate destroys it on the one after; this only bounds a stuck editor.
    constexpr double CloseWaitSeconds = 3.0;

    // Every notification item under Widget. The concrete classes (SNotificationItemImpl,
    // SNotificationItemExternalImpl) are private to Slate, so they are matched by type name and
    // used through their public base, SNotificationItem.
    void FindItems(const TSharedRef<SWidget>& Widget, TArray<TSharedRef<SNotificationItem>>& Out)
    {
        if (Widget->GetTypeAsString().StartsWith(TEXT("SNotificationItem")))
        {
            Out.Add(StaticCastSharedRef<SNotificationItem>(Widget));
            return;
        }
        FChildren* Children = Widget->GetChildren();
        for (int32 Index = 0; Children && Index < Children->Num(); ++Index)
        {
            FindItems(Children->GetChildAt(Index), Out);
        }
    }

    // The toast's first non-empty text, so the caller can tell which toast was dismissed.
    FString FirstText(const TSharedRef<SWidget>& Widget)
    {
        if (Widget->GetType() == FName(TEXT("STextBlock")))
        {
            const FString Text = StaticCastSharedRef<STextBlock>(Widget)->GetText().ToString();
            if (!Text.IsEmpty())
            {
                return Text;
            }
        }
        FChildren* Children = Widget->GetChildren();
        for (int32 Index = 0; Children && Index < Children->Num(); ++Index)
        {
            const FString Text = FirstText(Children->GetChildAt(Index));
            if (!Text.IsEmpty())
            {
                return Text;
            }
        }
        return FString();
    }

    bool IsWindowOpen(const TWeakPtr<SWindow>& Weak)
    {
        const TSharedPtr<SWindow> Window = Weak.Pin();
        if (!Window.IsValid() || !FSlateApplication::IsInitialized())
        {
            return false;
        }
        TArray<TSharedRef<SWindow>> Visible;
        FSlateApplication::Get().GetAllVisibleWindowsOrdered(Visible);
        return Visible.Contains(Window.ToSharedRef());
    }

    struct FToast
    {
        TWeakPtr<SWindow> Window;
        FString Text;
    };
}

REGISTER_RPC_HANDLER("editor.dismiss_notifications", "editor",
    "Close every open editor notification toast (the untitled top-most windows of type Notification that "
    "drive.list_windows lists). They cover whatever is under them, a PIE viewport included, so a drive "
    "pointer action there is refused TARGET_OCCLUDED; editor.resize_window cannot clear one because the "
    "notification manager re-sizes and re-places it every frame. Fades each toast out with a zero-length "
    "fade and waits (up to 3 s) for Slate to close its window. Returns {dismissed, requested, "
    "notifications[{text, closed}]}, where closed is measured (the window left Slate's visible-window "
    "list), plus a warning when any window stayed open. NO_NOTIFICATIONS when none is open.",
    RPC_NO_PARAMS)
{
    using namespace EditorNotificationHandlerLocal;

    if (!FSlateApplication::IsInitialized())
    {
        Ctx.SendError(ErrorCodes::ERR_SLATE_NOT_INITIALIZED, TEXT("Slate application is not initialized"));
        return true;
    }

    TArray<TSharedRef<SWindow>> Windows;
    FSlateNotificationManager::Get().GetWindows(Windows);

    TArray<FToast> Toasts;
    for (const TSharedRef<SWindow>& Window : Windows)
    {
        if (!IsWindowOpen(Window))
        {
            continue;
        }
        Toasts.Add({ Window, FirstText(Window) });
        TArray<TSharedRef<SNotificationItem>> Items;
        FindItems(Window, Items);
        for (const TSharedRef<SNotificationItem>& Item : Items)
        {
            Item->SetFadeOutDuration(0.0f);
            Item->Fadeout();
        }
    }

    if (Toasts.IsEmpty())
    {
        Ctx.SendError(ErrorCodes::ERR_NO_NOTIFICATIONS,
            TEXT("No editor notification toast is open, so nothing was dismissed."));
        return true;
    }

    const TSharedRef<FAsyncResponseToken> Token = Ctx.MakeAsyncToken();
    const double Deadline = FPlatformTime::Seconds() + CloseWaitSeconds;
    FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
        [Token, Toasts, Deadline](float) -> bool
        {
            int32 Open = 0;
            for (const FToast& Toast : Toasts)
            {
                Open += IsWindowOpen(Toast.Window) ? 1 : 0;
            }
            if (Open > 0 && FPlatformTime::Seconds() < Deadline)
            {
                return true;
            }

            TArray<TSharedPtr<FJsonValue>> Entries;
            for (const FToast& Toast : Toasts)
            {
                TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
                Entry->SetStringField(TEXT("text"), Toast.Text);
                Entry->SetBoolField(TEXT("closed"), !IsWindowOpen(Toast.Window));
                Entries.Add(MakeShared<FJsonValueObject>(Entry));
            }
            TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
            Result->SetNumberField(TEXT("dismissed"), Toasts.Num() - Open);
            Result->SetNumberField(TEXT("requested"), Toasts.Num());
            Result->SetArrayField(TEXT("notifications"), Entries);
            if (Open > 0)
            {
                Result->SetStringField(TEXT("warning"), FString::Printf(
                    TEXT("%d of %d notification windows were still open after %.0f s."),
                    Open, Toasts.Num(), CloseWaitSeconds));
            }
            Token->SendSuccess(Result);
            return false;
        }));
    return true;
}
