// Copyright (c) 2026 Alexander Penkin. MIT License.

// Tests for the Win32 SendInput backend of drive.click / drive.hover os_input
// (Handlers/Drive/DriveOsInputWindows.cpp, F-drive-os-input-windows).
//
//  - VirtualDeskNormalization runs everywhere: the pure pixel -> 0..65535 mapping SendInput needs.
//  - Every Win32* test is compiled on Windows only. They were written on a Linux host and have NOT
//    been compiled or run yet. The live ones (Win32LiveClickActuates, Win32LiveHoverMovesPointer)
//    need a visible editor on an unlocked desktop; offscreen / nullrhi / commandlet runs skip them
//    with the reason FDriveOsInput::IsAvailable gives.

#include "Misc/AutomationTest.h"

#include "Handlers/Drive/DriveOsInput.h"

#if PLATFORM_WINDOWS
#include "Handlers/Drive/DriveEditorChrome.h"
#include "Handlers/Drive/DriveTypes.h"

#include "Tests/TestSkipReporting.h"
#include "Tests/TestUtils.h"

#include "Async/Async.h"
#include "Dom/JsonObject.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/Event.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Misc/Guid.h"
#include "Tests/AutomationCommon.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/SWindow.h"
#include "Widgets/Text/STextBlock.h"
#endif

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDriveOsInputVirtualDeskNormalizationTest,
    "PinWright.drive.os_input.VirtualDeskNormalization",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDriveOsInputVirtualDeskNormalizationTest::RunTest(const FString& Parameters)
{
    // The model the helper inverts: Windows places a MOUSEEVENTF_ABSOLUTE | VIRTUALDESK
    // coordinate n at Origin + (n * Extent) >> 16. Desktops: 1080p, 1440p, 1440p seen through
    // 150% scaling by a DPI-unaware process (1707), and two-monitor layouts whose secondary sits
    // left of the primary (negative origin).
    struct FDesk { int32 Origin; int32 Extent; };
    const FDesk Desks[] = { {0, 1920}, {0, 2560}, {0, 1707}, {-1920, 4480}, {-2560, 5120}, {0, 7680} };
    for (const FDesk& Desk : Desks)
    {
        int32 Misses = 0;
        int32 OutOfRange = 0;
        for (int32 Pixel = Desk.Origin; Pixel < Desk.Origin + Desk.Extent; ++Pixel)
        {
            const int32 N = FDriveOsInput::NormalizeVirtualDeskCoord(Pixel, Desk.Origin, Desk.Extent);
            OutOfRange += (N < 0 || N > 65535) ? 1 : 0;
            const int32 Back = Desk.Origin + static_cast<int32>((static_cast<int64>(N) * Desk.Extent) >> 16);
            Misses += Back != Pixel ? 1 : 0;
        }
        TestEqual(FString::Printf(TEXT("every pixel of a %d px desktop at origin %d lands on itself"), Desk.Extent, Desk.Origin), Misses, 0);
        TestEqual(FString::Printf(TEXT("... and stays in 0..65535 (%d px at %d)"), Desk.Extent, Desk.Origin), OutOfRange, 0);
    }

    TestEqual(TEXT("a pixel left of the desktop clamps to its first pixel"),
        FDriveOsInput::NormalizeVirtualDeskCoord(-5000, -1920, 4480), 0);
    TestEqual(TEXT("a pixel past the desktop clamps to its last pixel"),
        FDriveOsInput::NormalizeVirtualDeskCoord(10000, 0, 1920), FDriveOsInput::NormalizeVirtualDeskCoord(1919, 0, 1920));
    TestEqual(TEXT("an empty desktop yields 0"), FDriveOsInput::NormalizeVirtualDeskCoord(10, 0, 0), 0);

    // The naive Pixel * 65535 / Extent lands a pixel short almost everywhere; the round-trip
    // above is what tells the two apart.
    int32 NaiveMisses = 0;
    for (int32 Pixel = 0; Pixel < 1920; ++Pixel)
    {
        const int64 N = static_cast<int64>(Pixel) * 65535 / 1920;
        NaiveMisses += static_cast<int32>((N * 1920) >> 16) != Pixel ? 1 : 0;
    }
    TestTrue(TEXT("the naive mapping misses pixels, so the round-trip check can fail"), NaiveMisses > 0);
    return true;
}

#if PLATFORM_WINDOWS

namespace DriveOsInputWin32Test
{
    // A second thread holding the session's os_input lock, standing in for a peer editor. A
    // Win32 mutex is recursive for its owning thread, so the peer must be another thread.
    struct FPeerHoldsLock
    {
        FEvent* Acquired = FPlatformProcess::GetSynchEventFromPool(true);
        FEvent* Release = FPlatformProcess::GetSynchEventFromPool(true);
        bool bHeld = false;  // written before Acquired->Trigger(), read after its Wait()
        TFuture<void> Done;

        FPeerHoldsLock()
        {
            Done = Async(EAsyncExecution::Thread, [this]()
            {
                const FDriveOsInput::FDisplayLock Lock(FDriveOsInput::DisplayLockPath(), FDriveOsInput::LockTimeoutSeconds);
                bHeld = Lock.IsHeld();
                Acquired->Trigger();
                Release->Wait();
            });
            Acquired->Wait(static_cast<uint32>(FDriveOsInput::LockTimeoutSeconds * 1000.0) + 1000);
        }

        ~FPeerHoldsLock()
        {
            Release->Trigger();
            Done.Wait();
            FPlatformProcess::ReturnSynchEventToPool(Acquired);
            FPlatformProcess::ReturnSynchEventToPool(Release);
        }
    };

    bool SkipIfUnavailable(FAutomationTestBase& Test)
    {
        FString Unavailable;
        if (FDriveOsInput::IsAvailable(Unavailable))
        {
            return false;
        }
        PinWrightTestSkip::SkipAssertions(Test, TEXT("os-input-unavailable"),
            FString::Printf(TEXT("os_input cannot inject on this host: %s"), *Unavailable));
        return true;
    }

    // A top-most window holding one button, placed clear of every window the editor shows.
    struct FFixture
    {
        FString Title;
        TSharedPtr<SWindow> Window;
        TSharedPtr<SButton> Button;
        TSharedRef<int32> Clicks = MakeShared<int32>(0);
        FString ButtonHandle;
        TSharedRef<FTestResponseCapture> Capture = MakeShared<FTestResponseCapture>();
        double Deadline = 0.0;

        ~FFixture()
        {
            if (FSlateApplication::IsInitialized() && Window.IsValid())
            {
                FSlateApplication::Get().RequestDestroyWindow(Window.ToSharedRef());
                FSlateApplication::Get().Tick(ESlateTickType::All);
            }
        }
    };

    TSharedPtr<FFixture> BuildFixture(FAutomationTestBase& Test)
    {
        TSharedPtr<FFixture> Fixture = MakeShared<FFixture>();
        Fixture->Title = FString::Printf(TEXT("PW_OsInputWin32_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));

        TArray<TSharedRef<SWindow>> Visible;
        FSlateApplication::Get().GetAllVisibleWindowsOrdered(Visible);
        double ClearX = 420.0;
        for (const TSharedRef<SWindow>& Other : Visible)
        {
            ClearX = FMath::Max(ClearX, FVector2D(Other->GetPositionInScreen()).X + FVector2D(Other->GetSizeInScreen()).X + 80.0);
        }

        const TSharedRef<int32> Clicks = Fixture->Clicks;
        Fixture->Window = SNew(SWindow)
            .Title(FText::FromString(Fixture->Title))
            .ScreenPosition(FVector2D(ClearX, 320.0))
            .ClientSize(FVector2D(360.0, 200.0))
            .FocusWhenFirstShown(false)
            .IsTopmostWindow(true)
            .SupportsMaximize(false)
            .SupportsMinimize(false)
            [
                SAssignNew(Fixture->Button, SButton)
                .OnClicked_Lambda([Clicks]() { ++(*Clicks); return FReply::Handled(); })
                [
                    SNew(STextBlock).Text(FText::FromString(TEXT("PwOsInputWin32Button")))
                ]
            ];
        FSlateApplication::Get().AddWindow(Fixture->Window.ToSharedRef(), /*bShowImmediately=*/true);
        for (int32 Tick = 0; Tick < 8; ++Tick)
        {
            FSlateApplication::Get().Tick(ESlateTickType::All);
        }

        FDriveWindowSelector Selector;
        Selector.Title = Fixture->Title;
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
            if (Element.Type == TEXT("SButton") && !Element.bGeometryStale && !Element.Handle.Contains(TEXT("SWindowTitleBar")))
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

    void Start(FAutomationTestBase& Test, FFixture& Fixture, const TCHAR* Method)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("handle"), Fixture.ButtonHandle);
        Payload->SetBoolField(TEXT("os_input"), true);
        Payload->SetStringField(TEXT("surface"), TEXT("editor_chrome"));
        Payload->SetStringField(TEXT("window_title"), Fixture.Title);
        Test.TestTrue(FString::Printf(TEXT("%s handler found"), Method),
            InvokeHandlerWithSharedCapture(Method, Payload, Fixture.Capture));
        Fixture.Deadline = FPlatformTime::Seconds() + 15.0;
    }

    bool ResponseArrived(FAutomationTestBase& Test, const FFixture& Fixture)
    {
        if (Fixture.Capture->bWasCalled)
        {
            return true;
        }
        if (FPlatformTime::Seconds() > Fixture.Deadline)
        {
            Test.AddError(TEXT("the os_input action never answered within 15 s."));
            return true;
        }
        return false;
    }
}

DEFINE_LATENT_AUTOMATION_COMMAND_ONE_PARAMETER(FDriveOsInputWin32Poll, TFunction<bool()>, Poll);

bool FDriveOsInputWin32Poll::Update()
{
    return Poll();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDriveOsInputWin32LockExcludesTest,
    "PinWright.drive.os_input.Win32LockExcludes",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDriveOsInputWin32LockExcludesTest::RunTest(const FString& Parameters)
{
    using namespace DriveOsInputWin32Test;
    TestEqual(TEXT("the lock is the session-wide named mutex"), FDriveOsInput::DisplayLockPath(), FString(TEXT("Local\\PinWright-os-input")));
    {
        FPeerHoldsLock Peer;
        if (!TestTrue(TEXT("the stand-in peer holds the lock"), Peer.bHeld))
        {
            return false;
        }
        const double Start = FPlatformTime::Seconds();
        const FDriveOsInput::FDisplayLock Second(FDriveOsInput::DisplayLockPath(), 0.2);
        const double Waited = FPlatformTime::Seconds() - Start;
        TestFalse(TEXT("a second acquirer does not get it while the peer holds it"), Second.IsHeld());
        TestTrue(TEXT("... and reports a timeout, not a failure"), Second.bTimedOut);
        TestTrue(TEXT("... after waiting out its timeout"), Waited >= 0.19);
        TestTrue(TEXT("... but not much longer"), Waited < 2.0);
    }
    const FDriveOsInput::FDisplayLock Third(FDriveOsInput::DisplayLockPath(), 0.2);
    TestTrue(TEXT("the lock is free again once the peer releases it"), Third.IsHeld());
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDriveOsInputWin32ClickWaitsForLockTest,
    "PinWright.drive.os_input.Win32ClickWaitsForLock",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDriveOsInputWin32ClickWaitsForLockTest::RunTest(const FString& Parameters)
{
    // A click while a peer holds the lock refuses with OS_INPUT_BUSY after the bounded wait and
    // moves nothing. Costs LockTimeoutSeconds.
    using namespace DriveOsInputWin32Test;
    if (SkipIfUnavailable(*this))
    {
        return true;
    }
    FIntPoint Before(-1, -1);
    FDriveOsInput::IsPointerAt(FIntPoint(-100000, -100000), Before);

    FDriveInjectFailure Failure;
    bool bClicked = true;
    {
        FPeerHoldsLock Peer;
        if (!TestTrue(TEXT("the stand-in peer holds the lock"), Peer.bHeld))
        {
            return false;
        }
        bClicked = FDriveOsInput::ClickAt(FVector2D(Before.X + 40, Before.Y + 40), EDriveMouseButton::Left, Failure);
    }
    FIntPoint After(-1, -1);
    FDriveOsInput::IsPointerAt(FIntPoint(-100000, -100000), After);

    TestFalse(TEXT("the click is refused"), bClicked);
    TestEqual(TEXT("... with OS_INPUT_BUSY"), Failure.Code, FString(TEXT("OS_INPUT_BUSY")));
    TestTrue(TEXT("... naming the lock"), Failure.Details.IsValid() && Failure.Details->HasField(TEXT("lock_file")));
    TestEqual(TEXT("... and the pointer was not moved"), After, Before);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDriveOsInputWin32ForeignPointRefusedTest,
    "PinWright.drive.os_input.Win32ForeignPointRefused",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDriveOsInputWin32ForeignPointRefusedTest::RunTest(const FString& Parameters)
{
    // A point no window of this editor owns (off every monitor): the ownership gate names it
    // foreign, and a click there refuses with TARGET_OCCLUDED before anything moves.
    using namespace DriveOsInputWin32Test;
    if (SkipIfUnavailable(*this))
    {
        return true;
    }
    const FVector2D Nowhere(-100000.0, -100000.0);
    FDriveOsInput::FForeignWindow Foreign;
    TestTrue(TEXT("a point outside every window of this editor is foreign"), FDriveOsInput::FindForeignWindowAt(Nowhere, Foreign));
    TestNotEqual(TEXT("... and not owned by this process"), Foreign.Pid, static_cast<uint32>(FPlatformProcess::GetCurrentProcessId()));

    FIntPoint Before(-1, -1);
    FDriveOsInput::IsPointerAt(FIntPoint(-100000, -100000), Before);
    FDriveInjectFailure Failure;
    TestFalse(TEXT("a click there is refused"), FDriveOsInput::ClickAt(Nowhere, EDriveMouseButton::Left, Failure));
    TestEqual(TEXT("... with TARGET_OCCLUDED"), Failure.Code, FString(TEXT("TARGET_OCCLUDED")));
    TestTrue(TEXT("... naming the owner's pid"), Failure.Details.IsValid() && Failure.Details->HasField(TEXT("occluding_pid")));
    FIntPoint After(-1, -1);
    FDriveOsInput::IsPointerAt(FIntPoint(-100000, -100000), After);
    TestEqual(TEXT("... and the pointer was not moved"), After, Before);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDriveOsInputWin32PrePressPointerCheckTest,
    "PinWright.drive.os_input.Win32PrePressPointerCheck",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDriveOsInputWin32PrePressPointerCheckTest::RunTest(const FString& Parameters)
{
    // GetCursorPos backs ClickAt's last check before the press. Moves and presses nothing.
    using namespace DriveOsInputWin32Test;
    if (SkipIfUnavailable(*this))
    {
        return true;
    }
    FIntPoint Pointer(-100000, -100000);
    TestFalse(TEXT("a pointer that is not on the target refuses the press"),
        FDriveOsInput::IsPointerAt(FIntPoint(-100000, -100000), Pointer));
    TestNotEqual(TEXT("... and reports where the pointer really is"), Pointer, FIntPoint(-100000, -100000));
    FIntPoint Again(-1, -1);
    TestTrue(TEXT("a pointer on the target lets the press through"), FDriveOsInput::IsPointerAt(Pointer, Again));
    TestEqual(TEXT("... at the same point"), Again, Pointer);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDriveOsInputWin32LiveClickActuatesTest,
    "PinWright.drive.os_input.Win32LiveClickActuates",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDriveOsInputWin32LiveClickActuatesTest::RunTest(const FString& Parameters)
{
    // drive.click os_input:true on a real button: the SendInput path runs (input_path os_win32)
    // and the button's OnClicked fires exactly once. Moves the real pointer and takes the
    // foreground for the editor.
    using namespace DriveOsInputWin32Test;
    if (SkipIfUnavailable(*this))
    {
        return true;
    }
    TSharedPtr<FFixture> Fixture = BuildFixture(*this);
    if (!Fixture.IsValid())
    {
        return true;
    }
    Start(*this, *Fixture, TEXT("drive.click"));
    ADD_LATENT_AUTOMATION_COMMAND(FDriveOsInputWin32Poll([this, Fixture]() -> bool
    {
        if (!ResponseArrived(*this, *Fixture))
        {
            return false;
        }
        const FTestResponseCapture& Capture = *Fixture->Capture;
        TestTrue(FString::Printf(TEXT("the os_input click succeeds (error: %s %s)"), *Capture.ErrorCode, *Capture.Message), Capture.bSuccess);
        FString InputPath;
        TestTrue(TEXT("the response reports input_path"),
            Capture.Result.IsValid() && Capture.Result->TryGetStringField(TEXT("input_path"), InputPath));
        TestEqual(TEXT("... as the Win32 SendInput path"), InputPath, FString(TEXT("os_win32")));
        TestEqual(TEXT("the button received exactly one click"), *Fixture->Clicks, 1);
        return true;
    }));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDriveOsInputWin32LiveHoverMovesPointerTest,
    "PinWright.drive.os_input.Win32LiveHoverMovesPointer",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDriveOsInputWin32LiveHoverMovesPointerTest::RunTest(const FString& Parameters)
{
    // drive.hover os_input:true moves the real pointer onto the button: input_path os_win32, the
    // button reports hovered, and nothing was clicked.
    using namespace DriveOsInputWin32Test;
    if (SkipIfUnavailable(*this))
    {
        return true;
    }
    TSharedPtr<FFixture> Fixture = BuildFixture(*this);
    if (!Fixture.IsValid())
    {
        return true;
    }
    TestFalse(TEXT("the button is not hovered before the hover"), Fixture->Button->IsHovered());
    Start(*this, *Fixture, TEXT("drive.hover"));
    ADD_LATENT_AUTOMATION_COMMAND(FDriveOsInputWin32Poll([this, Fixture]() -> bool
    {
        if (!ResponseArrived(*this, *Fixture))
        {
            return false;
        }
        const FTestResponseCapture& Capture = *Fixture->Capture;
        TestTrue(FString::Printf(TEXT("the os_input hover succeeds (error: %s %s)"), *Capture.ErrorCode, *Capture.Message), Capture.bSuccess);
        FString InputPath;
        TestTrue(TEXT("the response reports input_path"),
            Capture.Result.IsValid() && Capture.Result->TryGetStringField(TEXT("input_path"), InputPath));
        TestEqual(TEXT("... as the Win32 SendInput path"), InputPath, FString(TEXT("os_win32")));
        TestTrue(TEXT("the real pointer reached the button"), Fixture->Button->IsHovered());
        TestEqual(TEXT("a hover clicks nothing"), *Fixture->Clicks, 0);
        return true;
    }));
    return true;
}

#endif // PLATFORM_WINDOWS
