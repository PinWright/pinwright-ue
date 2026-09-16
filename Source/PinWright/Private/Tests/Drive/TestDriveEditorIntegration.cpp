// Copyright (c) 2026 Alexander Penkin. MIT License.

// Integration tests for the editor-chrome surface wired into the drive.* handlers:
// the new drive.list_windows discovery verb, drive.observe over surface=editor_chrome,
// and the surface-aware resolve path reached through drive.click. These invoke the
// registered handler bodies through the shared TestUtils helpers (no live PIE HUD).
// The editor's own top-level windows ARE present during automation, so the live
// assertions run for real, but they stay skip-tolerant (warn + pass) under the two
// "no UI at all" outcomes (Slate down / no windows) the same way the editorchrome unit
// tests do, and they assert structure/invariants rather than exact widget names.

#include "Misc/AutomationTest.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Handlers/Drive/DriveHandlerCommon.h"
#include "Handlers/Drive/DriveTypes.h"
#include "Tests/TestSkipReporting.h"
#include "Tests/TestUtils.h"

namespace
{
    // The window-resolution codes that mean "no editor UI to walk" — an unusual,
    // headless configuration the suite tolerates as a skip rather than a failure.
    bool IsAcceptedEditorIntSkipCode(const FString& ErrorCode)
    {
        return ErrorCode == TEXT("SLATE_NOT_INITIALIZED")
            || ErrorCode == TEXT("NO_WINDOWS")
            || ErrorCode == TEXT("NO_ACTIVE_WINDOW");
    }

    // A drive element carrying only the fields the observe filter cares about.
    FDriveElement MakeFilterElement(const FString& Handle, bool bInteractable)
    {
        FDriveElement Element;
        Element.Handle = Handle;
        Element.bInteractable = bInteractable;
        return Element;
    }
}

// ============================================================================
// drive.list_windows is registered and lists the live editor windows.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDriveEditorIntListWindowsTest,
    "PinWright.drive.editorint.ListWindowsRegisteredAndLive",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDriveEditorIntListWindowsTest::RunTest(const FString& Parameters)
{
    TestTrue(TEXT("drive.list_windows is registered"), IsHandlerRegistered(TEXT("drive.list_windows")));

    FTestResponseCapture Capture;
    const bool bFound = InvokeHandlerWithCapture(TEXT("drive.list_windows"), MakeShared<FJsonObject>(), Capture);

    TestTrue(TEXT("drive.list_windows handler found"), bFound);
    TestTrue(TEXT("drive.list_windows succeeds"), Capture.bSuccess);
    if (!Capture.bSuccess || !Capture.Result.IsValid())
    {
        return false;
    }

    double Count = -1.0;
    TestTrue(TEXT("response carries a count"), Capture.Result->TryGetNumberField(TEXT("count"), Count));

    const TArray<TSharedPtr<FJsonValue>>* Windows = nullptr;
    TestTrue(TEXT("response carries a windows array"), Capture.Result->TryGetArrayField(TEXT("windows"), Windows));
    if (!Windows)
    {
        return false;
    }

    TestEqual(TEXT("count matches the windows array length"), static_cast<int32>(Count), Windows->Num());

    if (Windows->Num() == 0)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-live-window"),
            TEXT("drive.list_windows returned no windows (Slate down / headless); skipping live assertions."));
        return true;
    }

    // Live: at least the main editor window. Each entry carries a type, an ordered
    // index matching its position, and a geometry.absolute block.
    for (int32 Index = 0; Index < Windows->Num(); ++Index)
    {
        const TSharedPtr<FJsonObject>* Entry = nullptr;
        if (!(*Windows)[Index].IsValid() || !(*Windows)[Index]->TryGetObject(Entry) || !Entry || !(*Entry).IsValid())
        {
            AddError(FString::Printf(TEXT("window entry %d is not a JSON object"), Index));
            continue;
        }

        FString Type;
        TestTrue(TEXT("window entry carries a type"), (*Entry)->TryGetStringField(TEXT("type"), Type));
        TestFalse(TEXT("window type string is populated"), Type.IsEmpty());

        double EntryIndex = -1.0;
        TestTrue(TEXT("window entry carries an index"), (*Entry)->TryGetNumberField(TEXT("index"), EntryIndex));
        TestEqual(TEXT("window index matches its ordered position"), static_cast<int32>(EntryIndex), Index);

        const TSharedPtr<FJsonObject>* Geometry = nullptr;
        TestTrue(TEXT("window entry carries a geometry object"),
            (*Entry)->TryGetObjectField(TEXT("geometry"), Geometry) && Geometry);
    }

    return true;
}

// ============================================================================
// drive.observe with surface=editor_chrome returns an element list for the
// active editor window.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDriveEditorIntObserveTest,
    "PinWright.drive.editorint.ObserveEditorChromeActiveWindow",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDriveEditorIntObserveTest::RunTest(const FString& Parameters)
{
    bSuppressLogErrors = true;
    bSuppressLogWarnings = true;

    // screenshot=false keeps the test to the element list (the window-capture path is
    // best-effort and exercised elsewhere); no window selector selects the active window.
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("surface"), TEXT("editor_chrome"));
    Payload->SetBoolField(TEXT("screenshot"), false);

    FTestResponseCapture Capture;
    const bool bFound = InvokeHandlerWithCapture(TEXT("drive.observe"), Payload, Capture);

    TestTrue(TEXT("drive.observe handler found"), bFound);
    TestTrue(TEXT("drive.observe sent a response"), Capture.bWasCalled);

    // A failure is only acceptable as one of the "no editor UI" skip codes.
    if (!Capture.bSuccess)
    {
        if (IsAcceptedEditorIntSkipCode(Capture.ErrorCode))
        {
            PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-chrome"),
                FString::Printf(
                    TEXT("Skipping live editor-chrome observe: %s"), *Capture.ErrorCode));
            return true;
        }
        AddError(FString::Printf(
            TEXT("drive.observe editor_chrome failed unexpectedly: %s"), *Capture.ErrorCode));
        return false;
    }

    if (!Capture.Result.IsValid())
    {
        AddError(TEXT("drive.observe succeeded but carried no result object"));
        return false;
    }

    FString SurfaceStr;
    TestTrue(TEXT("observation carries a surface"), Capture.Result->TryGetStringField(TEXT("surface"), SurfaceStr));
    TestEqual(TEXT("observation surface is editor_chrome"), SurfaceStr, FString(TEXT("editor_chrome")));

    const TArray<TSharedPtr<FJsonValue>>* Elements = nullptr;
    TestTrue(TEXT("observation carries an elements array"),
        Capture.Result->TryGetArrayField(TEXT("elements"), Elements));
    if (!Elements)
    {
        return false;
    }
    TestTrue(TEXT("active editor window yields a non-empty element list"), Elements->Num() > 0);

    // Every element should be stamped with the editor-chrome surface.
    for (const TSharedPtr<FJsonValue>& Value : *Elements)
    {
        const TSharedPtr<FJsonObject>* Entry = nullptr;
        if (Value.IsValid() && Value->TryGetObject(Entry) && Entry && (*Entry).IsValid())
        {
            FString ElemSurface;
            if ((*Entry)->TryGetStringField(TEXT("surface"), ElemSurface))
            {
                TestEqual(TEXT("element surface is editor_chrome"), ElemSurface, FString(TEXT("editor_chrome")));
            }
        }
    }

    return true;
}

// ============================================================================
// FilterObservationElements (pure): interactables_only drops static elements;
// max_elements caps the list and reports the dropped count via OutOmittedCount.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDriveFilterObservationElementsTest,
    "PinWright.drive.editorint.FilterObservationElements",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDriveFilterObservationElementsTest::RunTest(const FString& Parameters)
{
    auto BuildSet = []()
    {
        TArray<FDriveElement> Elements;
        Elements.Add(MakeFilterElement(TEXT("btn_0"), true));
        Elements.Add(MakeFilterElement(TEXT("txt_0"), false));
        Elements.Add(MakeFilterElement(TEXT("btn_1"), true));
        Elements.Add(MakeFilterElement(TEXT("txt_1"), false));
        Elements.Add(MakeFilterElement(TEXT("btn_2"), true));
        return Elements;
    };

    // interactables_only keeps only the 3 interactable elements; no cap, so no omission.
    {
        TArray<FDriveElement> Elements = BuildSet();
        int32 Omitted = -1;
        FDriveHandlerCommon::FilterObservationElements(Elements, /*bInteractablesOnly=*/true, /*MaxElements=*/0, /*MaxBytes=*/0, Omitted);
        TestEqual(TEXT("interactables_only keeps 3 of 5"), Elements.Num(), 3);
        TestEqual(TEXT("no omission without a cap"), Omitted, 0);
        for (const FDriveElement& Element : Elements)
        {
            TestTrue(TEXT("every kept element is interactable"), Element.bInteractable);
        }
    }

    // max_elements caps the list and reports exactly how many were dropped.
    {
        TArray<FDriveElement> Elements = BuildSet();
        int32 Omitted = -1;
        FDriveHandlerCommon::FilterObservationElements(Elements, /*bInteractablesOnly=*/false, /*MaxElements=*/2, /*MaxBytes=*/0, Omitted);
        TestEqual(TEXT("max_elements caps the list at 2"), Elements.Num(), 2);
        TestEqual(TEXT("omitted reports the 3 dropped"), Omitted, 3);
        TestEqual(TEXT("cap preserves the leading elements"), Elements[0].Handle, FString(TEXT("btn_0")));
    }

    // Both together: filter first, then cap; omission counts only the cap drops.
    {
        TArray<FDriveElement> Elements = BuildSet();
        int32 Omitted = -1;
        FDriveHandlerCommon::FilterObservationElements(Elements, /*bInteractablesOnly=*/true, /*MaxElements=*/2, /*MaxBytes=*/0, Omitted);
        TestEqual(TEXT("filter then cap leaves 2"), Elements.Num(), 2);
        TestEqual(TEXT("omitted counts only the cap drop (3 interactables -> 2)"), Omitted, 1);
    }

    // A cap at or above the count is a no-op (0 = unlimited too).
    {
        TArray<FDriveElement> Elements = BuildSet();
        int32 Omitted = -1;
        FDriveHandlerCommon::FilterObservationElements(Elements, /*bInteractablesOnly=*/false, /*MaxElements=*/0, /*MaxBytes=*/0, Omitted);
        TestEqual(TEXT("max_elements 0 is unlimited"), Elements.Num(), 5);
        TestEqual(TEXT("nothing omitted"), Omitted, 0);
    }

    return true;
}

// ============================================================================
// drive.observe editor_chrome interactables_only=true returns only interactable
// elements (cuts a verbose window list down to its actionable widgets).
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDriveEditorIntObserveInteractablesOnlyTest,
    "PinWright.drive.editorint.ObserveInteractablesOnly",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDriveEditorIntObserveInteractablesOnlyTest::RunTest(const FString& Parameters)
{
    bSuppressLogErrors = true;
    bSuppressLogWarnings = true;

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("surface"), TEXT("editor_chrome"));
    Payload->SetBoolField(TEXT("screenshot"), false);
    Payload->SetBoolField(TEXT("interactables_only"), true);

    FTestResponseCapture Capture;
    const bool bFound = InvokeHandlerWithCapture(TEXT("drive.observe"), Payload, Capture);
    TestTrue(TEXT("drive.observe handler found"), bFound);

    if (!Capture.bSuccess)
    {
        if (IsAcceptedEditorIntSkipCode(Capture.ErrorCode))
        {
            PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-chrome"),
                FString::Printf(TEXT("Skipping live editor-chrome observe: %s"), *Capture.ErrorCode));
            return true;
        }
        AddError(FString::Printf(TEXT("drive.observe editor_chrome failed unexpectedly: %s"), *Capture.ErrorCode));
        return false;
    }

    const TArray<TSharedPtr<FJsonValue>>* Elements = nullptr;
    if (!Capture.Result.IsValid() || !Capture.Result->TryGetArrayField(TEXT("elements"), Elements) || !Elements)
    {
        AddError(TEXT("drive.observe succeeded but carried no elements array"));
        return false;
    }

    // Every returned element must be interactable (static text/labels are dropped).
    for (const TSharedPtr<FJsonValue>& Value : *Elements)
    {
        const TSharedPtr<FJsonObject>* Entry = nullptr;
        if (Value.IsValid() && Value->TryGetObject(Entry) && Entry && (*Entry).IsValid())
        {
            bool bInteractable = false;
            (*Entry)->TryGetBoolField(TEXT("interactable"), bInteractable);
            TestTrue(TEXT("interactables_only element is interactable"), bInteractable);
        }
    }

    return true;
}

// ============================================================================
// drive.observe editor_chrome max_elements caps the elements array and reports
// the dropped count via omitted_count (no silent truncation).
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDriveEditorIntObserveMaxElementsTest,
    "PinWright.drive.editorint.ObserveMaxElementsOmittedCount",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDriveEditorIntObserveMaxElementsTest::RunTest(const FString& Parameters)
{
    bSuppressLogErrors = true;
    bSuppressLogWarnings = true;

    // Baseline: the unfiltered element count for the active window.
    TSharedPtr<FJsonObject> BasePayload = MakeShared<FJsonObject>();
    BasePayload->SetStringField(TEXT("surface"), TEXT("editor_chrome"));
    BasePayload->SetBoolField(TEXT("screenshot"), false);

    FTestResponseCapture BaseCapture;
    InvokeHandlerWithCapture(TEXT("drive.observe"), BasePayload, BaseCapture);
    if (!BaseCapture.bSuccess)
    {
        if (IsAcceptedEditorIntSkipCode(BaseCapture.ErrorCode))
        {
            PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-chrome"),
                FString::Printf(TEXT("Skipping live editor-chrome observe: %s"), *BaseCapture.ErrorCode));
            return true;
        }
        AddError(FString::Printf(TEXT("baseline observe failed: %s"), *BaseCapture.ErrorCode));
        return false;
    }

    const TArray<TSharedPtr<FJsonValue>>* BaseElements = nullptr;
    if (!BaseCapture.Result.IsValid() || !BaseCapture.Result->TryGetArrayField(TEXT("elements"), BaseElements) || !BaseElements)
    {
        AddError(TEXT("baseline observe carried no elements array"));
        return false;
    }
    const int32 Total = BaseElements->Num();
    if (Total <= 1)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("too-few-elements-to-truncate"),
            TEXT("Active editor window has <= 1 element; skipping max_elements truncation assertion."));
        return true;
    }

    // Cap at 1: the list must shorten and omitted_count must account for the rest.
    TSharedPtr<FJsonObject> CapPayload = MakeShared<FJsonObject>();
    CapPayload->SetStringField(TEXT("surface"), TEXT("editor_chrome"));
    CapPayload->SetBoolField(TEXT("screenshot"), false);
    CapPayload->SetNumberField(TEXT("max_elements"), 1);

    FTestResponseCapture CapCapture;
    InvokeHandlerWithCapture(TEXT("drive.observe"), CapPayload, CapCapture);
    if (!CapCapture.bSuccess || !CapCapture.Result.IsValid())
    {
        AddError(TEXT("capped observe did not succeed"));
        return false;
    }

    const TArray<TSharedPtr<FJsonValue>>* CapElements = nullptr;
    TestTrue(TEXT("capped observe carries an elements array"),
        CapCapture.Result->TryGetArrayField(TEXT("elements"), CapElements) && CapElements != nullptr);
    if (CapElements)
    {
        TestTrue(TEXT("capped element count respects max_elements"), CapElements->Num() <= 1);
    }

    double OmittedCount = -1.0;
    TestTrue(TEXT("capped observe reports omitted_count"),
        CapCapture.Result->TryGetNumberField(TEXT("omitted_count"), OmittedCount));
    TestTrue(TEXT("omitted_count is positive when the cap dropped elements"), OmittedCount > 0.0);

    return true;
}

// ============================================================================
// The surface-aware resolve path (drive.click -> ResolveForSurface) returns a
// clean coded error for a bogus editor-chrome handle, never a crash or a fake
// success.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDriveEditorIntResolveBogusHandleTest,
    "PinWright.drive.editorint.ResolveBogusEditorHandleCodedError",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDriveEditorIntResolveBogusHandleTest::RunTest(const FString& Parameters)
{
    bSuppressLogErrors = true;
    bSuppressLogWarnings = true;

    // A handle that cannot exist on any real widget, routed to the editor-chrome
    // surface. The synchronous re-resolve fails before any input is injected, so the
    // handler answers with a coded error (TARGET_NOT_FOUND when a window resolved, or a
    // window-resolution code when there is no editor window to walk).
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("surface"), TEXT("editor_chrome"));
    Payload->SetStringField(TEXT("handle"), TEXT("__pinwright_editor_chrome_no_such_handle__"));

    FTestResponseCapture Capture;
    const bool bFound = InvokeHandlerWithCapture(TEXT("drive.click"), Payload, Capture);

    TestTrue(TEXT("drive.click handler found"), bFound);
    TestTrue(TEXT("drive.click sent a response"), Capture.bWasCalled);
    TestFalse(TEXT("a bogus editor handle is not a success"), Capture.bSuccess);
    TestFalse(TEXT("the failure carries a non-empty error code"), Capture.ErrorCode.IsEmpty());

    return true;
}
