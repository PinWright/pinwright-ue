// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression tests for drive.observe visible_only (E-drive-observe-no-visible-filter). A CommonUI
// menu keeps its inactive screens in the widget tree, so interactables_only lists dozens of
// collapsed (visible:false) controls ahead of the on-screen ones in tree order, and max_elements /
// max_bytes then cut the list before any visible control. visible_only drops visible:false
// elements BEFORE the caps. The pure-filter test fails if the filter is removed or applied after
// the caps. The wire test routes drive.observe through the REAL dispatcher (so the param allowlist
// runs) over a fixture window holding a visible leaf and a leaf under a Collapsed parent: it fails
// if the param is not declared (UNKNOWN_PARAMS) or not threaded (the collapsed leaf comes back).

#include "Misc/AutomationTest.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Dispatch/RpcDispatcher.h"
#include "Framework/Application/SlateApplication.h"
#include "Handlers/Drive/DriveHandlerCommon.h"
#include "Handlers/Drive/DriveTypes.h"
#include "Layout/Visibility.h"
#include "Misc/Guid.h"
#include "Tests/Infra/DispatcherTestHelpers.h"
#include "Tests/TestSkipReporting.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SWindow.h"
#include "Widgets/Text/STextBlock.h"

namespace TestDriveObserveVisibleOnlyHelpers
{
    FDriveElement MakeElement(const FString& Handle, bool bVisible)
    {
        FDriveElement Element;
        Element.Handle = Handle;
        Element.bInteractable = true;
        Element.bVisible = bVisible;
        Element.bGeometryStale = !bVisible;
        return Element;
    }

    // The ticket's shape: hidden screens first in tree order, the on-screen controls last.
    TArray<FDriveElement> HiddenFirst()
    {
        TArray<FDriveElement> Elements;
        for (int32 Index = 0; Index < 6; ++Index)
        {
            Elements.Add(MakeElement(FString::Printf(TEXT("W_CreateUser/Field_%d"), Index), false));
        }
        Elements.Add(MakeElement(TEXT("W_SchoolNameLogin/NameBox"), true));
        Elements.Add(MakeElement(TEXT("W_SchoolNameLogin/OkButton"), true));
        return Elements;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDriveObserveVisibleOnlyFilterTest,
    "PinWright.drive.observe.VisibleOnlyKeepsOnScreenUnderCaps",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDriveObserveVisibleOnlyFilterTest::RunTest(const FString& Parameters)
{
    using namespace TestDriveObserveVisibleOnlyHelpers;

    // Precondition: without the filter the cap keeps only hidden rows (the reported bug).
    {
        TArray<FDriveElement> Elements = HiddenFirst();
        int32 Omitted = -1;
        FDriveHandlerCommon::FilterObservationElements(Elements, /*bInteractablesOnly=*/true,
            /*MaxElements=*/2, /*MaxBytes=*/0, Omitted);
        if (!TestEqual(TEXT("fixture: unfiltered cap keeps 2"), Elements.Num(), 2))
        {
            return false;
        }
        TestFalse(TEXT("fixture: unfiltered cap keeps a hidden row first"), Elements[0].bVisible);
    }

    // visible_only + max_elements: the two on-screen controls survive the cap.
    {
        TArray<FDriveElement> Elements = HiddenFirst();
        int32 Omitted = -1;
        FDriveHandlerCommon::FilterObservationElements(Elements, /*bInteractablesOnly=*/true,
            /*MaxElements=*/2, /*MaxBytes=*/0, Omitted, /*bVisibleOnly=*/true);
        if (!TestEqual(TEXT("visible_only keeps both visible rows"), Elements.Num(), 2))
        {
            return false;
        }
        TestEqual(TEXT("first kept row is the visible name box"), Elements[0].Handle, FString(TEXT("W_SchoolNameLogin/NameBox")));
        TestEqual(TEXT("second kept row is the visible OK button"), Elements[1].Handle, FString(TEXT("W_SchoolNameLogin/OkButton")));
        TestEqual(TEXT("the filter drop is not counted as a cap omission"), Omitted, 0);
    }

    // visible_only + max_bytes: a budget for ~1 element still returns a visible one, not a hidden one.
    {
        TArray<FDriveElement> Elements = HiddenFirst();
        int32 Omitted = -1;
        FDriveHandlerCommon::FilterObservationElements(Elements, /*bInteractablesOnly=*/false,
            /*MaxElements=*/0, /*MaxBytes=*/1, Omitted, /*bVisibleOnly=*/true);
        if (!TestEqual(TEXT("max_bytes keeps one row"), Elements.Num(), 1))
        {
            return false;
        }
        TestTrue(TEXT("the row max_bytes keeps is visible"), Elements[0].bVisible);
        TestEqual(TEXT("max_bytes omits only the other visible row"), Omitted, 1);
    }

    // Default (false) is a no-op: hidden rows stay listed for widget_present/count checks.
    {
        TArray<FDriveElement> Elements = HiddenFirst();
        int32 Omitted = -1;
        FDriveHandlerCommon::FilterObservationElements(Elements, false, 0, 0, Omitted);
        TestEqual(TEXT("default keeps hidden rows"), Elements.Num(), 8);
    }

    return true;
}

namespace TestDriveObserveVisibleOnlyHelpers
{
    const TCHAR* VisibleLeafLabel = TEXT("PwVisibleOnlyShownLeaf");
    const TCHAR* HiddenLeafLabel = TEXT("PwVisibleOnlyCollapsedLeaf");

    // A shown, uniquely-titled window with one plain leaf and one leaf under a Collapsed border
    // (the leaf's own visibility stays Visible, so visible:false can only come from the fold).
    struct FFixtureWindow
    {
        explicit FFixtureWindow(FSlateApplication& InSlateApp)
            : SlateApp(InSlateApp)
            , Title(FString::Printf(TEXT("PW_ObserveVisibleOnly_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits)))
        {
            TSharedRef<SBorder> CollapsedParent = SNew(SBorder)[SNew(STextBlock).Text(FText::FromString(HiddenLeafLabel))];
            CollapsedParent->SetVisibility(EVisibility::Collapsed);

            Window = SNew(SWindow)
                .Title(FText::FromString(Title))
                .ScreenPosition(FVector2D(300.0f, 220.0f))
                .ClientSize(FVector2D(420.0f, 240.0f))
                .FocusWhenFirstShown(false)
                .SupportsMaximize(false)
                .SupportsMinimize(false);
            Window->SetContent(
                SNew(SVerticalBox)
                + SVerticalBox::Slot().AutoHeight()[SNew(STextBlock).Text(FText::FromString(VisibleLeafLabel))]
                + SVerticalBox::Slot().AutoHeight()[CollapsedParent]);

            SlateApp.AddWindow(Window.ToSharedRef(), /*bShowImmediately=*/true);
            for (int32 Tick = 0; Tick < 8; ++Tick)
            {
                SlateApp.Tick(ESlateTickType::All);
            }
        }

        ~FFixtureWindow()
        {
            SlateApp.RequestDestroyWindow(Window.ToSharedRef());
            // RequestDestroyWindow only queues; tick so the next test does not enumerate it.
            SlateApp.Tick(ESlateTickType::All);
        }

        FSlateApplication& SlateApp;
        FString Title;
        TSharedPtr<SWindow> Window;
    };

    // Observe the fixture window through the real dispatcher. Returns the elements' label ->
    // visible map (AND-folded per label); false when the call failed (OutErrorCode set).
    bool ObserveFixture(const FString& Title, bool bVisibleOnly, TMap<FString, bool>& OutVisibleByLabel,
        int32& OutCount, FString& OutErrorCode, FString& OutMessage)
    {
        DispatcherTestHelpers::FSinkPtr Sink;
        FRpcDispatcher Dispatcher;
        DispatcherTestHelpers::MakeDispatcher(Sink, Dispatcher);

        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("surface"), TEXT("editor_chrome"));
        Payload->SetStringField(TEXT("window_title"), Title);
        Payload->SetBoolField(TEXT("screenshot"), false);
        if (bVisibleOnly)
        {
            Payload->SetBoolField(TEXT("visible_only"), true);
        }

        bool bSuccess = false;
        TSharedPtr<FJsonObject> Result;
        DispatcherTestHelpers::Dispatch(Dispatcher, Sink, TEXT("drive.observe"), TEXT("req-visible-only"),
            Payload, bSuccess, Result, OutErrorCode);
        OutMessage = Sink->Message;
        if (!Sink->bWasCalled)
        {
            OutErrorCode = TEXT("NO_SYNCHRONOUS_RESPONSE");
            return false;
        }
        const TArray<TSharedPtr<FJsonValue>>* Elements = nullptr;
        if (!bSuccess || !Result.IsValid() || !Result->TryGetArrayField(TEXT("elements"), Elements) || !Elements)
        {
            return false;
        }
        OutCount = Elements->Num();
        for (const TSharedPtr<FJsonValue>& Value : *Elements)
        {
            const TSharedPtr<FJsonObject>* Entry = nullptr;
            if (Value.IsValid() && Value->TryGetObject(Entry) && Entry && (*Entry).IsValid())
            {
                FString Label;
                bool bVisible = false;
                (*Entry)->TryGetStringField(TEXT("label"), Label);
                (*Entry)->TryGetBoolField(TEXT("visible"), bVisible);
                // Labels can repeat (e.g. unlabelled containers): fold so one hidden row is never masked.
                bool& bAllVisible = OutVisibleByLabel.FindOrAdd(Label, true);
                bAllVisible = bAllVisible && bVisible;
            }
        }
        return true;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDriveObserveVisibleOnlyDispatchTest,
    "PinWright.drive.observe.VisibleOnlyParamDropsHiddenEditorChrome",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDriveObserveVisibleOnlyDispatchTest::RunTest(const FString& Parameters)
{
    using namespace TestDriveObserveVisibleOnlyHelpers;

    if (!FSlateApplication::IsInitialized())
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("slate_not_initialized"),
            TEXT("FSlateApplication is not initialized; the visible_only fixture window cannot be built."));
        return true;
    }
    FFixtureWindow Fixture(FSlateApplication::Get());

    // Precondition: unfiltered, the fixture lists the visible leaf AND the collapsed leaf
    // (visible:false). Without this the filtered assertions below could pass vacuously.
    TMap<FString, bool> Unfiltered;
    int32 UnfilteredCount = 0;
    FString ErrorCode;
    FString Message;
    if (!ObserveFixture(Fixture.Title, false, Unfiltered, UnfilteredCount, ErrorCode, Message))
    {
        if (ErrorCode == TEXT("SLATE_NOT_INITIALIZED") || ErrorCode == TEXT("NO_WINDOWS")
            || ErrorCode == TEXT("WINDOW_NOT_FOUND"))
        {
            PinWrightTestSkip::SkipAssertions(*this, TEXT("fixture-window-not-enumerated"),
                FString::Printf(TEXT("Fixture window '%s' could not be observed: %s - %s"), *Fixture.Title, *ErrorCode, *Message));
            return true;
        }
        AddError(FString::Printf(TEXT("Unfiltered drive.observe of the fixture failed: %s - %s"), *ErrorCode, *Message));
        return false;
    }
    const bool* UnfilteredHidden = Unfiltered.Find(HiddenLeafLabel);
    const bool* UnfilteredShown = Unfiltered.Find(VisibleLeafLabel);
    if (!TestNotNull(TEXT("fixture: unfiltered observe lists the collapsed leaf"), UnfilteredHidden)
        || !TestNotNull(TEXT("fixture: unfiltered observe lists the visible leaf"), UnfilteredShown))
    {
        return false;
    }
    TestFalse(TEXT("fixture: collapsed leaf reads visible:false"), *UnfilteredHidden);
    TestTrue(TEXT("fixture: visible leaf reads visible:true"), *UnfilteredShown);

    // visible_only through the dispatcher: accepted (not UNKNOWN_PARAMS), non-empty, all visible,
    // the visible leaf kept and the collapsed leaf dropped.
    TMap<FString, bool> Filtered;
    int32 FilteredCount = 0;
    if (!ObserveFixture(Fixture.Title, true, Filtered, FilteredCount, ErrorCode, Message))
    {
        AddError(FString::Printf(TEXT("drive.observe visible_only failed (UNKNOWN_PARAMS = not declared): %s - %s"), *ErrorCode, *Message));
        return false;
    }
    TestTrue(TEXT("visible_only result is non-empty"), FilteredCount > 0);
    TestTrue(TEXT("visible_only keeps the visible leaf"), Filtered.Contains(VisibleLeafLabel));
    TestFalse(TEXT("visible_only drops the collapsed leaf"), Filtered.Contains(HiddenLeafLabel));
    for (const TPair<FString, bool>& Pair : Filtered)
    {
        TestTrue(*FString::Printf(TEXT("visible_only element '%s' is visible"), *Pair.Key), Pair.Value);
    }
    TestTrue(TEXT("visible_only returned fewer elements than the unfiltered observe"), FilteredCount < UnfilteredCount);

    return true;
}
