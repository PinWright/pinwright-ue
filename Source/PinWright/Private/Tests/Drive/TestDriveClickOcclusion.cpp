// Copyright (c) 2026 Alexander Penkin. MIT License.

// drive.click must not report a benign settle outcome for a click that could never reach its
// target. Slate routes pointer input to whichever top-level window
// FSlateApplication::LocateWindowUnderMouse names for the point, which is not necessarily the
// target's own window: another top-level window stacked over it takes the click, and on Linux the
// platform's cached window-under-cursor keeps naming the window the pointer was over before the
// warp until SDL's enter event is pumped, so the first click into a different window is hit-tested
// in the old one. Both left the widget untouched while drive.click answered
// no_change_within_budget.
//
// Two live tests over uniquely-titled fixture windows, run across real engine frames so the
// platform can catch up with the warp exactly as it does for a real caller:
//
//  - OccludedTargetIsRefused (failure direction): a second window covers the target button, so the
//    click is refused with TARGET_OCCLUDED naming a window other than the target's, and the
//    button's OnClicked never fires.
//  - UncoveredTargetIsClicked (counterfactual): the same fixture without the cover is clicked
//    exactly once, so the routing gate does not refuse a reachable target.

#include "Misc/AutomationTest.h"

#include "Handlers/Drive/DriveEditorChrome.h"
#include "Handlers/Drive/DriveTypes.h"

#include "Tests/TestSkipReporting.h"
#include "Tests/TestUtils.h"

#include "Dom/JsonObject.h"
#include "Framework/Application/SlateApplication.h"
#include "Misc/Guid.h"
#include "Tests/AutomationCommon.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/SWindow.h"
#include "Widgets/Text/STextBlock.h"

namespace DriveClickOcclusionTest
{
    // One fixture: a target window holding a single button, optionally covered by a second window.
    struct FFixture
    {
        FString TargetTitle;
        FString CoverTitle;
        TSharedPtr<SWindow> TargetWindow;
        TSharedPtr<SWindow> CoverWindow;
        TSharedRef<int32> Clicks = MakeShared<int32>(0);
        FString ButtonHandle;
        TSharedRef<FTestResponseCapture> Capture = MakeShared<FTestResponseCapture>();
        double Deadline = 0.0;

        ~FFixture()
        {
            if (!FSlateApplication::IsInitialized())
            {
                return;
            }
            FSlateApplication& SlateApp = FSlateApplication::Get();
            if (CoverWindow.IsValid())
            {
                SlateApp.RequestDestroyWindow(CoverWindow.ToSharedRef());
            }
            if (TargetWindow.IsValid())
            {
                SlateApp.RequestDestroyWindow(TargetWindow.ToSharedRef());
            }
            // RequestDestroyWindow only queues; tick so the next test does not enumerate them.
            SlateApp.Tick(ESlateTickType::All);
        }
    };

    TSharedPtr<SWindow> MakeWindow(const FString& Title, const FVector2D& Position, const FVector2D& Size,
        const TSharedRef<SWidget>& Content)
    {
        TSharedRef<SWindow> Window = SNew(SWindow)
            .Title(FText::FromString(Title))
            .ScreenPosition(Position)
            .ClientSize(Size)
            .FocusWhenFirstShown(false)
            .SupportsMaximize(false)
            .SupportsMinimize(false);
        Window->SetContent(Content);
        FSlateApplication::Get().AddWindow(Window, /*bShowImmediately=*/true);
        return Window;
    }

    // Builds the fixture and resolves the button's drive handle. Returns null (already reported as
    // a skip) when this host cannot show and walk a Slate window.
    TSharedPtr<FFixture> BuildFixture(FAutomationTestBase& Test, bool bCovered)
    {
        if (!FSlateApplication::IsInitialized())
        {
            PinWrightTestSkip::SkipAssertions(Test, TEXT("slate_not_initialized"),
                TEXT("FSlateApplication is not initialized; the occlusion fixture cannot be built."));
            return nullptr;
        }

        TSharedPtr<FFixture> Fixture = MakeShared<FFixture>();
        const FString Guid = FGuid::NewGuid().ToString(EGuidFormats::Digits);
        Fixture->TargetTitle = FString::Printf(TEXT("PW_ClickOcclusion_Target_%s"), *Guid);
        Fixture->CoverTitle = FString::Printf(TEXT("PW_ClickOcclusion_Cover_%s"), *Guid);

        const TSharedRef<int32> Clicks = Fixture->Clicks;
        Fixture->TargetWindow = MakeWindow(Fixture->TargetTitle, FVector2D(420.0f, 320.0f), FVector2D(360.0f, 200.0f),
            SNew(SButton)
            .OnClicked_Lambda([Clicks]() { ++(*Clicks); return FReply::Handled(); })
            [
                SNew(STextBlock).Text(FText::FromString(TEXT("PwOcclusionTargetButton")))
            ]);
        if (bCovered)
        {
            // Larger than the target on every side, so the button's center is covered however the
            // platform frames the two windows.
            Fixture->CoverWindow = MakeWindow(Fixture->CoverTitle, FVector2D(380.0f, 280.0f), FVector2D(440.0f, 280.0f),
                SNew(SBorder)[SNew(STextBlock).Text(FText::FromString(TEXT("PwOcclusionCover")))]);
        }

        for (int32 Tick = 0; Tick < 8; ++Tick)
        {
            FSlateApplication::Get().Tick(ESlateTickType::All);
        }

        FDriveWindowSelector Selector;
        Selector.Title = Fixture->TargetTitle;
        TArray<FDriveElement> Elements;
        FString WindowTitle;
        FString ErrorCode;
        FString ErrorMessage;
        if (!FDriveEditorChrome::BuildElementList(Selector, Elements, WindowTitle, ErrorCode, ErrorMessage))
        {
            PinWrightTestSkip::SkipAssertions(Test, TEXT("fixture-window-not-enumerated"),
                FString::Printf(TEXT("The fixture window could not be walked: %s - %s"), *ErrorCode, *ErrorMessage));
            return nullptr;
        }
        for (const FDriveElement& Element : Elements)
        {
            // Skip the window's own title-bar buttons; the fixture button is the only other one.
            if (Element.Type == TEXT("SButton") && !Element.bGeometryStale
                && !Element.Handle.Contains(TEXT("SWindowTitleBar")))
            {
                Fixture->ButtonHandle = Element.Handle;
                break;
            }
        }
        if (Fixture->ButtonHandle.IsEmpty())
        {
            PinWrightTestSkip::SkipAssertions(Test, TEXT("fixture-button-not-painted"),
                TEXT("The fixture button was not listed with live geometry; this host did not paint the window."));
            return nullptr;
        }
        return Fixture;
    }

    // Starts drive.click on the fixture's button; the response arrives on a later frame.
    void StartClick(FAutomationTestBase& Test, FFixture& Fixture)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("handle"), Fixture.ButtonHandle);
        Payload->SetStringField(TEXT("surface"), TEXT("editor_chrome"));
        Payload->SetStringField(TEXT("window_title"), Fixture.TargetTitle);
        Test.TestTrue(TEXT("drive.click handler found"),
            InvokeHandlerWithSharedCapture(TEXT("drive.click"), Payload, Fixture.Capture));
        Fixture.Deadline = FPlatformTime::Seconds() + 10.0;
    }

    // True once the response arrived or the wait expired (reported as an error).
    bool ResponseArrived(FAutomationTestBase& Test, const FFixture& Fixture)
    {
        if (Fixture.Capture->bWasCalled)
        {
            return true;
        }
        if (FPlatformTime::Seconds() > Fixture.Deadline)
        {
            Test.AddError(TEXT("drive.click never answered within 10 s."));
            return true;
        }
        return false;
    }
}

DEFINE_LATENT_AUTOMATION_COMMAND_ONE_PARAMETER(FDriveClickOcclusionPoll, TFunction<bool()>, Poll);

bool FDriveClickOcclusionPoll::Update()
{
    return Poll();
}

// ============================================================================
// Failure direction: a covered target is refused, and nothing behind the cover is clicked.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDriveClickOccludedTargetIsRefusedTest,
    "PinWright.drive.click_occlusion.OccludedTargetIsRefused",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDriveClickOccludedTargetIsRefusedTest::RunTest(const FString& Parameters)
{
    using namespace DriveClickOcclusionTest;

    TSharedPtr<FFixture> Fixture = BuildFixture(*this, /*bCovered=*/true);
    if (!Fixture.IsValid())
    {
        return true;
    }

    StartClick(*this, *Fixture);
    ADD_LATENT_AUTOMATION_COMMAND(FDriveClickOcclusionPoll([this, Fixture]() -> bool
    {
        if (!ResponseArrived(*this, *Fixture))
        {
            return false;
        }
        const FTestResponseCapture& Capture = *Fixture->Capture;
        TestFalse(TEXT("a click on a covered target is not reported as a success"), Capture.bSuccess);
        TestEqual(TEXT("the refusal is TARGET_OCCLUDED"), Capture.ErrorCode, FString(TEXT("TARGET_OCCLUDED")));
        TestEqual(TEXT("the covered button never received the click"), *Fixture->Clicks, 0);

        // The window named is whichever one the routing picked (the cover, or a window this host
        // stacks above both); what must hold is that it is not the target's own.
        FString Occluder;
        if (Capture.Result.IsValid() && Capture.Result->TryGetStringField(TEXT("occluding_window"), Occluder))
        {
            TestNotEqual(TEXT("the occluding window is not the target's own"), Occluder, Fixture->TargetTitle);
        }
        return true;
    }));
    return true;
}

// ============================================================================
// Counterfactual: the same target without the cover is clicked exactly once.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDriveClickUncoveredTargetIsClickedTest,
    "PinWright.drive.click_occlusion.UncoveredTargetIsClicked",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDriveClickUncoveredTargetIsClickedTest::RunTest(const FString& Parameters)
{
    using namespace DriveClickOcclusionTest;

    TSharedPtr<FFixture> Fixture = BuildFixture(*this, /*bCovered=*/false);
    if (!Fixture.IsValid())
    {
        return true;
    }

    StartClick(*this, *Fixture);
    ADD_LATENT_AUTOMATION_COMMAND(FDriveClickOcclusionPoll([this, Fixture]() -> bool
    {
        if (!ResponseArrived(*this, *Fixture))
        {
            return false;
        }
        const FTestResponseCapture& Capture = *Fixture->Capture;
        FString Occluder;
        if (!Capture.bSuccess && Capture.ErrorCode == TEXT("TARGET_OCCLUDED")
            && Capture.Result.IsValid() && Capture.Result->TryGetStringField(TEXT("occluding_window"), Occluder)
            && !Occluder.Contains(TEXT("PW_ClickOcclusion_")))
        {
            // A window this test does not own (the host's window manager stacked the unfocused
            // fixture under it) really does cover the button, so the refusal is correct here.
            PinWrightTestSkip::SkipAssertions(*this, TEXT("fixture-window-stacked-under-host-window"),
                FString::Printf(TEXT("The uncovered fixture sits under host window '%s'."), *Occluder));
            return true;
        }
        TestTrue(FString::Printf(TEXT("the uncovered click succeeds (error: %s %s)"), *Capture.ErrorCode, *Capture.Message),
            Capture.bSuccess);
        TestEqual(TEXT("the button received exactly one click"), *Fixture->Clicks, 1);
        return true;
    }));
    return true;
}
