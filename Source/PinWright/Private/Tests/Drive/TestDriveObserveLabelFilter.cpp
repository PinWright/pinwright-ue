// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression tests for drive.observe label_contains / filter / handle_contains
// (E-drive-observe-no-label-filter). Finding one control by its label used to mean observing the
// whole surface, letting the element list spill, and grepping the spill file. The filters keep
// only matching elements BEFORE the max_elements / max_bytes caps. The pure-filter test fails if
// the filter is removed, applied after the caps, or folds case ASCII-only (Cyrillic labels). The
// wire test routes drive.observe through the REAL dispatcher (so the param allowlist runs): it
// fails if label_contains / filter / handle_contains is not declared or not threaded.

#include "Misc/AutomationTest.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Dispatch/RpcDispatcher.h"
#include "Framework/Application/SlateApplication.h"
#include "Handlers/Drive/DriveHandlerCommon.h"
#include "Handlers/Drive/DriveTypes.h"
#include "Misc/Guid.h"
#include "Tests/Infra/DispatcherTestHelpers.h"
#include "Tests/TestSkipReporting.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SWindow.h"
#include "Widgets/Text/STextBlock.h"

namespace TestDriveObserveLabelFilterHelpers
{
    FDriveElement MakeElement(const FString& Handle, const FString& Label)
    {
        FDriveElement Element;
        Element.Handle = Handle;
        Element.Label = Label;
        Element.bInteractable = true;
        Element.bVisible = true;
        return Element;
    }

    // The ticket's shape: a long menu with the wanted button last in tree order.
    TArray<FDriveElement> MenuWithTargetLast()
    {
        TArray<FDriveElement> Elements;
        for (int32 Index = 0; Index < 6; ++Index)
        {
            Elements.Add(MakeElement(FString::Printf(TEXT("W_Menu/Row_%d"), Index), FString::Printf(TEXT("Трасса %d"), Index)));
        }
        Elements.Add(MakeElement(TEXT("W_Menu/CreateButton"), TEXT("СОЗДАТЬ")));
        return Elements;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDriveObserveLabelFilterTest,
    "PinWright.drive.observe.LabelContainsKeepsMatchUnderCaps",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDriveObserveLabelFilterTest::RunTest(const FString& Parameters)
{
    using namespace TestDriveObserveLabelFilterHelpers;

    // Precondition: without the filter a cap of 1 keeps the first row, not the wanted button.
    {
        TArray<FDriveElement> Elements = MenuWithTargetLast();
        int32 Omitted = -1;
        FDriveHandlerCommon::FilterObservationElements(Elements, false, /*MaxElements=*/1, /*MaxBytes=*/0, Omitted);
        if (!TestEqual(TEXT("fixture: unfiltered cap keeps 1"), Elements.Num(), 1))
        {
            return false;
        }
        TestNotEqual(TEXT("fixture: unfiltered cap misses the button"), Elements[0].Handle, FString(TEXT("W_Menu/CreateButton")));
    }

    // Lower-case Cyrillic needle matches the upper-case label, before the caps.
    {
        TArray<FDriveElement> Elements = MenuWithTargetLast();
        int32 Omitted = -1;
        FDriveHandlerCommon::FilterObservationElements(Elements, false, /*MaxElements=*/1, /*MaxBytes=*/1, Omitted,
            /*bVisibleOnly=*/false, /*LabelContains=*/TEXT("создать"));
        if (!TestEqual(TEXT("label_contains keeps exactly the button"), Elements.Num(), 1))
        {
            return false;
        }
        TestEqual(TEXT("label_contains keeps the button"), Elements[0].Handle, FString(TEXT("W_Menu/CreateButton")));
        TestEqual(TEXT("the filter drop is not counted as a cap omission"), Omitted, 0);
    }

    // handle_contains matches the widget-tree path case-insensitively; both filters AND together.
    {
        TArray<FDriveElement> Elements = MenuWithTargetLast();
        int32 Omitted = -1;
        FDriveHandlerCommon::FilterObservationElements(Elements, false, 0, 0, Omitted, false, FString(), TEXT("row_"));
        TestEqual(TEXT("handle_contains keeps the six rows"), Elements.Num(), 6);

        Elements = MenuWithTargetLast();
        FDriveHandlerCommon::FilterObservationElements(Elements, false, 0, 0, Omitted, false, TEXT("трасса"), TEXT("Row_3"));
        if (TestEqual(TEXT("label_contains + handle_contains keep one row"), Elements.Num(), 1))
        {
            TestEqual(TEXT("both filters keep Row_3"), Elements[0].Handle, FString(TEXT("W_Menu/Row_3")));
        }

        Elements = MenuWithTargetLast();
        FDriveHandlerCommon::FilterObservationElements(Elements, false, 0, 0, Omitted, false, TEXT("no such label"));
        TestEqual(TEXT("a non-matching label returns an empty list"), Elements.Num(), 0);
    }

    // Empty filters are a no-op.
    {
        TArray<FDriveElement> Elements = MenuWithTargetLast();
        int32 Omitted = -1;
        FDriveHandlerCommon::FilterObservationElements(Elements, false, 0, 0, Omitted, false, FString(), FString());
        TestEqual(TEXT("empty filters keep every row"), Elements.Num(), 7);
    }

    return true;
}

namespace TestDriveObserveLabelFilterHelpers
{
    const TCHAR* WantedLabel = TEXT("PwLabelFilterWantedLeaf");
    const TCHAR* OtherLabel = TEXT("PwLabelFilterOtherLeaf");

    // A shown, uniquely-titled window with two plain text leaves.
    struct FFixtureWindow
    {
        explicit FFixtureWindow(FSlateApplication& InSlateApp)
            : SlateApp(InSlateApp)
            , Title(FString::Printf(TEXT("PW_ObserveLabelFilter_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits)))
        {
            Window = SNew(SWindow)
                .Title(FText::FromString(Title))
                .ScreenPosition(FVector2D(300.0f, 220.0f))
                .ClientSize(FVector2D(420.0f, 240.0f))
                .FocusWhenFirstShown(false)
                .SupportsMaximize(false)
                .SupportsMinimize(false);
            Window->SetContent(
                SNew(SVerticalBox)
                + SVerticalBox::Slot().AutoHeight()[SNew(STextBlock).Text(FText::FromString(OtherLabel))]
                + SVerticalBox::Slot().AutoHeight()[SNew(STextBlock).Text(FText::FromString(WantedLabel))]);

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

    // Observe the fixture window through the real dispatcher with an optional string param.
    // Returns the element labels; false when the call failed (OutErrorCode set).
    bool ObserveFixture(const FString& Title, const FString& ParamName, const FString& ParamValue,
        TArray<FString>& OutLabels, FString& OutErrorCode, FString& OutMessage)
    {
        DispatcherTestHelpers::FSinkPtr Sink;
        FRpcDispatcher Dispatcher;
        DispatcherTestHelpers::MakeDispatcher(Sink, Dispatcher);

        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("surface"), TEXT("editor_chrome"));
        Payload->SetStringField(TEXT("window_title"), Title);
        Payload->SetBoolField(TEXT("screenshot"), false);
        if (!ParamName.IsEmpty())
        {
            Payload->SetStringField(ParamName, ParamValue);
        }

        bool bSuccess = false;
        TSharedPtr<FJsonObject> Result;
        DispatcherTestHelpers::Dispatch(Dispatcher, Sink, TEXT("drive.observe"), TEXT("req-label-filter"),
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
        for (const TSharedPtr<FJsonValue>& Value : *Elements)
        {
            const TSharedPtr<FJsonObject>* Entry = nullptr;
            if (Value.IsValid() && Value->TryGetObject(Entry) && Entry && (*Entry).IsValid())
            {
                FString Label;
                (*Entry)->TryGetStringField(TEXT("label"), Label);
                OutLabels.Add(Label);
            }
        }
        return true;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDriveObserveLabelFilterDispatchTest,
    "PinWright.drive.observe.LabelContainsParamFiltersEditorChrome",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDriveObserveLabelFilterDispatchTest::RunTest(const FString& Parameters)
{
    using namespace TestDriveObserveLabelFilterHelpers;

    if (!FSlateApplication::IsInitialized())
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("slate_not_initialized"),
            TEXT("FSlateApplication is not initialized; the label filter fixture window cannot be built."));
        return true;
    }
    FFixtureWindow Fixture(FSlateApplication::Get());

    // Precondition: unfiltered, the fixture lists both leaves plus other rows (the window chrome),
    // so the filtered assertions below cannot pass vacuously.
    TArray<FString> Unfiltered;
    FString ErrorCode;
    FString Message;
    if (!ObserveFixture(Fixture.Title, FString(), FString(), Unfiltered, ErrorCode, Message))
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
    if (!TestTrue(TEXT("fixture: unfiltered observe lists the wanted leaf"), Unfiltered.Contains(WantedLabel))
        || !TestTrue(TEXT("fixture: unfiltered observe lists the other leaf"), Unfiltered.Contains(OtherLabel)))
    {
        return false;
    }

    // label_contains, its filter alias, and handle_contains are each accepted through the
    // dispatcher (not UNKNOWN_PARAMS) and threaded to the filter (a non-matching handle fragment
    // empties the otherwise non-empty list).
    struct FCase { const TCHAR* Param; FString Value; bool bExpectOnlyWanted; };
    const FCase Cases[] = {
        { TEXT("label_contains"), TEXT("pwlabelfilterwanted"), true },
        { TEXT("filter"), TEXT("PWLABELFILTERWANTED"), true },
        { TEXT("handle_contains"), TEXT("no-such-handle-fragment"), false },
    };
    for (const FCase& Case : Cases)
    {
        TArray<FString> Filtered;
        if (!ObserveFixture(Fixture.Title, Case.Param, Case.Value, Filtered, ErrorCode, Message))
        {
            AddError(FString::Printf(TEXT("drive.observe %s failed (UNKNOWN_PARAMS = not declared): %s - %s"), Case.Param, *ErrorCode, *Message));
            continue;
        }
        if (Case.bExpectOnlyWanted)
        {
            TestEqual(*FString::Printf(TEXT("%s keeps one element"), Case.Param), Filtered.Num(), 1);
            TestTrue(*FString::Printf(TEXT("%s keeps the wanted leaf"), Case.Param), Filtered.Contains(WantedLabel));
        }
        else
        {
            TestEqual(*FString::Printf(TEXT("%s with no match returns no elements"), Case.Param), Filtered.Num(), 0);
        }
    }

    return true;
}
