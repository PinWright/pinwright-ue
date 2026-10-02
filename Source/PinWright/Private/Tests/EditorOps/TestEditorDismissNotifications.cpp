// Copyright (c) 2026 Alexander Penkin. MIT License.

// editor.dismiss_notifications must actually close a notification toast that would never leave on
// its own. The fixture is a persistent toast (bFireAndForget=false, so it has no expiry); the verb
// must report it closed, and its window must be gone from Slate's visible windows, which is what
// the drive occlusion gate reads.

#include "Misc/AutomationTest.h"

#include "Tests/TestSkipReporting.h"
#include "Tests/TestUtils.h"

#include "Dom/JsonObject.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Notifications/NotificationManager.h"
#include "Misc/Guid.h"
#include "Tests/AutomationCommon.h"
#include "Widgets/Notifications/SNotificationList.h"
#include "Widgets/SWindow.h"

namespace EditorDismissNotificationsTest
{
    struct FFixture
    {
        FString Text;
        TSharedPtr<SNotificationItem> Item;
        TWeakPtr<SWindow> Window;
        TSharedRef<FTestResponseCapture> Capture = MakeShared<FTestResponseCapture>();
        bool bInvoked = false;
        double Deadline = 0.0;

        // A failed run must not leave the persistent toast over later tests.
        ~FFixture()
        {
            if (Item.IsValid())
            {
                Item->SetFadeOutDuration(0.0f);
                Item->Fadeout();
            }
        }
    };

    bool IsVisible(const TWeakPtr<SWindow>& Weak)
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
}

DEFINE_LATENT_AUTOMATION_COMMAND_ONE_PARAMETER(FEditorDismissNotificationsPoll, TFunction<bool()>, Poll);

bool FEditorDismissNotificationsPoll::Update()
{
    return Poll();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEditorDismissNotificationsClosesPersistentToastTest,
    "PinWright.editor.dismiss_notifications.ClosesPersistentToast",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FEditorDismissNotificationsClosesPersistentToastTest::RunTest(const FString& Parameters)
{
    using namespace EditorDismissNotificationsTest;

    if (!FSlateApplication::IsInitialized())
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("slate_not_initialized"),
            TEXT("FSlateApplication is not initialized; no notification window can be shown."));
        return true;
    }

    TSharedPtr<FFixture> Fixture = MakeShared<FFixture>();
    Fixture->Text = FString::Printf(TEXT("PW_DismissNotifications_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    FNotificationInfo Info(FText::FromString(Fixture->Text));
    Info.bFireAndForget = false;
    Fixture->Item = FSlateNotificationManager::Get().AddNotification(Info);
    if (!Fixture->Item.IsValid())
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("notifications-disabled"),
            TEXT("FSlateNotificationManager::AddNotification returned no item (Slate.bAllowNotifications is off)."));
        return true;
    }
    Fixture->Deadline = FPlatformTime::Seconds() + 5.0;

    ADD_LATENT_AUTOMATION_COMMAND(FEditorDismissNotificationsPoll([this, Fixture]() -> bool
    {
        if (!Fixture->bInvoked)
        {
            // Wait for the manager to show the toast's window.
            if (!Fixture->Window.IsValid())
            {
                Fixture->Window = FSlateApplication::Get().FindWidgetWindow(Fixture->Item.ToSharedRef());
            }
            if (!IsVisible(Fixture->Window))
            {
                if (FPlatformTime::Seconds() < Fixture->Deadline)
                {
                    return false;
                }
                PinWrightTestSkip::SkipAssertions(*this, TEXT("notification-window-not-shown"),
                    TEXT("The fixture toast's window never became visible on this host."));
                return true;
            }
            Fixture->bInvoked = true;
            Fixture->Deadline = FPlatformTime::Seconds() + 10.0;
            TestTrue(TEXT("editor.dismiss_notifications handler found"),
                InvokeHandlerWithSharedCapture(TEXT("editor.dismiss_notifications"), MakeShared<FJsonObject>(), Fixture->Capture));
            return false;
        }

        const FTestResponseCapture& Capture = *Fixture->Capture;
        if (!Capture.bWasCalled)
        {
            if (FPlatformTime::Seconds() < Fixture->Deadline)
            {
                return false;
            }
            AddError(TEXT("editor.dismiss_notifications never answered within 10 s."));
            return true;
        }

        TestTrue(FString::Printf(TEXT("the verb succeeds (error: %s %s)"), *Capture.ErrorCode, *Capture.Message),
            Capture.bSuccess);
        TestFalse(TEXT("the persistent toast's window is closed"), IsVisible(Fixture->Window));

        bool bReportedClosed = false;
        const TArray<TSharedPtr<FJsonValue>>* Entries = nullptr;
        if (Capture.Result.IsValid() && Capture.Result->TryGetArrayField(TEXT("notifications"), Entries))
        {
            for (const TSharedPtr<FJsonValue>& Value : *Entries)
            {
                const TSharedPtr<FJsonObject> Entry = Value->AsObject();
                bool bClosed = false;
                if (Entry.IsValid() && Entry->GetStringField(TEXT("text")) == Fixture->Text
                    && Entry->TryGetBoolField(TEXT("closed"), bClosed))
                {
                    bReportedClosed = bClosed;
                }
            }
        }
        TestTrue(TEXT("the response lists the toast by its text as closed"), bReportedClosed);
        return true;
    }));
    return true;
}
