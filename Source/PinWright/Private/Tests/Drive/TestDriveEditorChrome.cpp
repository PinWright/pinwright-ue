// Copyright (c) 2026 Alexander Penkin. MIT License.

// Tests for FDriveEditorChrome: a pure unit test for the window-rooted path / segment-name
// scheme over synthetic widgets, plus live tests over the editor's own windows. Unlike the game
// UMG surface, the editor's top-level windows ARE present during automation, so the live tests
// assert for real. They still guard the two "no UI at all" outcomes (Slate down / no windows) as
// a skip-with-warning so the suite stays safe under an unusual headless configuration, but assert
// structure/invariants - not exact widget names - to tolerate editor-layout variation.

#include "Misc/AutomationTest.h"

#include "Handlers/Drive/DriveEditorChrome.h"
#include "Handlers/Drive/DriveLiveResolver.h"
#include "Handlers/Drive/DriveTypes.h"

#include "Tests/TestSkipReporting.h"

#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

namespace
{
    // The element build over the active window, or a classification of why we can't: Slate down
    // or no visible windows is an (unusual, headless) skip; any other failure is a real error.
    enum class EEditorChromeBuildResult
    {
        Built,
        Skipped,
        Failed
    };

    bool IsAcceptedEditorChromeSkipCode(const FString& ErrorCode)
    {
        return ErrorCode == TEXT("SLATE_NOT_INITIALIZED")
            || ErrorCode == TEXT("NO_WINDOWS")
            || ErrorCode == TEXT("NO_ACTIVE_WINDOW");
    }

    EEditorChromeBuildResult BuildActiveWindowOrSkip(
        FAutomationTestBase& Test,
        TArray<FDriveElement>& OutElements,
        FString& OutWindowTitle)
    {
        FString ErrorCode;
        FString ErrorMessage;
        if (FDriveEditorChrome::BuildElementList(
                FDriveWindowSelector{}, OutElements, OutWindowTitle, ErrorCode, ErrorMessage))
        {
            return EEditorChromeBuildResult::Built;
        }

        if (IsAcceptedEditorChromeSkipCode(ErrorCode))
        {
            PinWrightTestSkip::SkipAssertions(Test, TEXT("no-editor-chrome"),
                FString::Printf(
                    TEXT("Skipping live editor-chrome assertions: %s - %s"), *ErrorCode, *ErrorMessage));
            return EEditorChromeBuildResult::Skipped;
        }

        Test.AddError(FString::Printf(
            TEXT("Unexpected editor-chrome element-list failure: %s - %s"), *ErrorCode, *ErrorMessage));
        return EEditorChromeBuildResult::Failed;
    }

    bool IsFiniteVec2EditorChrome(const FVector2D& Value)
    {
        return FMath::IsFinite(Value.X) && FMath::IsFinite(Value.Y);
    }
}

// ============================================================================
// Pure: window-rooted path / segment-name scheme over synthetic widgets (no window needed)
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDriveEditorChromePathSchemeTest,
    "PinWright.drive.editorchrome.PathScheme",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FDriveEditorChromePathSchemeTest::RunTest(const FString& Parameters)
{
    const TSharedRef<SButton> ButtonA = SNew(SButton);
    const TSharedRef<STextBlock> Text = SNew(STextBlock).Text(FText::FromString(TEXT("Hello")));

    // Two same-typed siblings plus a text block, so the sibling-index disambiguation has work to do.
    const TSharedRef<SButton> ButtonB = SNew(SButton);
    const TSharedRef<SVerticalBox> Box = SNew(SVerticalBox)
        + SVerticalBox::Slot()[ButtonA]
        + SVerticalBox::Slot()[ButtonB]
        + SVerticalBox::Slot()[Text];

    // Segment name falls back to the Slate type for non-UMG widgets.
    TestEqual(TEXT("segment name of a bare SButton is its Slate type"),
        FDriveEditorChrome::GetWidgetSegmentName(ButtonA), FString(TEXT("SButton")));
    TestEqual(TEXT("segment name of a bare STextBlock is its Slate type"),
        FDriveEditorChrome::GetWidgetSegmentName(Text), FString(TEXT("STextBlock")));

    // A root with no parent has a bare (un-indexed) path equal to its segment name.
    TestEqual(TEXT("root path is the bare segment name"),
        FDriveEditorChrome::BuildWidgetPath(Box), FString(TEXT("SVerticalBox")));

    const FString PathA = FDriveEditorChrome::BuildWidgetPath(ButtonA);
    const FString PathB = FDriveEditorChrome::BuildWidgetPath(ButtonB);
    const FString PathText = FDriveEditorChrome::BuildWidgetPath(Text);

    // Parent pointers are set when slots are attached (SlotBase::AttachWidget), so each path is
    // window-rooted: "<parent>/<child>[<index>]".
    TestEqual(TEXT("first button path is window-rooted and index 0"),
        PathA, FString(TEXT("SVerticalBox/SButton[0]")));
    TestEqual(TEXT("second button path is window-rooted and index 1"),
        PathB, FString(TEXT("SVerticalBox/SButton[1]")));
    TestEqual(TEXT("text block path is window-rooted and index 2"),
        PathText, FString(TEXT("SVerticalBox/STextBlock[2]")));

    // The core invariant: same-typed siblings get distinct handles.
    TestNotEqual(TEXT("same-typed siblings get distinct paths"), PathA, PathB);

    return true;
}

// ============================================================================
// Live: ListWindows finds at least the main editor window
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDriveEditorChromeListWindowsLiveTest,
    "PinWright.drive.editorchrome.ListWindowsLive",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FDriveEditorChromeListWindowsLiveTest::RunTest(const FString& Parameters)
{
    const TArray<FDriveWindowInfo> Windows = FDriveEditorChrome::ListWindows();

    if (Windows.Num() == 0)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-slate-windows"),
            TEXT("ListWindows returned no windows (Slate down / headless); skipping assertions."));
        return true;
    }

    TestTrue(TEXT("at least one top-level window is listed"), Windows.Num() >= 1);

    for (int32 Index = 0; Index < Windows.Num(); ++Index)
    {
        const FDriveWindowInfo& Info = Windows[Index];
        TestEqual(TEXT("window index matches its ordered position"), Info.Index, Index);
        TestFalse(TEXT("window type string is populated"), Info.Type.IsEmpty());
        TestTrue(TEXT("window absolute position is finite"), IsFiniteVec2EditorChrome(Info.AbsolutePosition));
        TestTrue(TEXT("window absolute size is finite"), IsFiniteVec2EditorChrome(Info.AbsoluteSize));
    }

    return true;
}

// ============================================================================
// Live: element-list builder shape and per-element invariants over the active window
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDriveEditorChromeElementListLiveTest,
    "PinWright.drive.editorchrome.ElementListLive",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FDriveEditorChromeElementListLiveTest::RunTest(const FString& Parameters)
{
    TArray<FDriveElement> Elements;
    FString WindowTitle;

    const EEditorChromeBuildResult Result = BuildActiveWindowOrSkip(*this, Elements, WindowTitle);
    if (Result != EEditorChromeBuildResult::Built)
    {
        return Result == EEditorChromeBuildResult::Skipped;
    }

    TestTrue(TEXT("active window yields a non-empty element list"), Elements.Num() > 0);

    bool bAnyInteractable = false;
    for (const FDriveElement& Element : Elements)
    {
        TestFalse(TEXT("every element carries a handle"), Element.Handle.IsEmpty());
        TestFalse(TEXT("every element carries a type"), Element.Type.IsEmpty());
        // Handle is the widget path for this surface; they are the same string.
        TestEqual(TEXT("handle equals path for editor chrome"), Element.Handle, Element.Path);
        TestEqual(TEXT("every element is on the EditorChrome surface"),
            static_cast<int32>(Element.Surface), static_cast<int32>(EDriveSurface::EditorChrome));
        TestTrue(TEXT("element absolute position is finite"), IsFiniteVec2EditorChrome(Element.AbsolutePosition));
        TestTrue(TEXT("element absolute size is finite"), IsFiniteVec2EditorChrome(Element.AbsoluteSize));
        // Included only when interactable OR text-bearing.
        TestTrue(TEXT("included element is interactable or label-bearing"),
            Element.bInteractable || !Element.Label.IsEmpty());

        bAnyInteractable |= Element.bInteractable;
    }

    TestTrue(TEXT("the active window exposes at least one interactable element"), bAnyInteractable);

    return true;
}

// ============================================================================
// Live: a handle from BuildElementList round-trips to Found; a bogus handle is NotFound
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDriveEditorChromeResolveHandleLiveTest,
    "PinWright.drive.editorchrome.ResolveHandleLive",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FDriveEditorChromeResolveHandleLiveTest::RunTest(const FString& Parameters)
{
    TArray<FDriveElement> Elements;
    FString WindowTitle;

    const EEditorChromeBuildResult Result = BuildActiveWindowOrSkip(*this, Elements, WindowTitle);
    if (Result != EEditorChromeBuildResult::Built)
    {
        return Result == EEditorChromeBuildResult::Skipped;
    }

    if (Elements.Num() == 0)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("fixture-did-not-populate"),
            TEXT("Active window has no elements; skipping handle round-trip."));
        return true;
    }

    // Re-resolve a known handle against the live tree. The same window is re-walked, so the
    // structure is identical and the indexed path resolves to exactly one widget.
    const FString KnownHandle = Elements[0].Handle;
    const FDriveResolveResult Resolved = FDriveEditorChrome::ResolveHandle(FDriveWindowSelector{}, KnownHandle);

    TestEqual(TEXT("known handle resolves to Found"),
        static_cast<int32>(Resolved.Status), static_cast<int32>(EDriveResolveStatus::Found));
    TestTrue(TEXT("resolved result carries a live widget"), Resolved.Widget.IsValid());
    TestEqual(TEXT("resolved element handle matches the request"), Resolved.Element.Handle, KnownHandle);
    TestEqual(TEXT("resolved element is on the EditorChrome surface"),
        static_cast<int32>(Resolved.Element.Surface), static_cast<int32>(EDriveSurface::EditorChrome));

    // A handle that cannot exist on any real widget; the window resolves, so the outcome is a
    // clean NotFound (not NoLiveUi, which is reserved for "couldn't reach a window to walk").
    const FString MissingHandle = TEXT("__pinwright_editor_chrome_no_such_handle__");
    const FDriveResolveResult Missing = FDriveEditorChrome::ResolveHandle(FDriveWindowSelector{}, MissingHandle);

    TestEqual(TEXT("missing handle resolves to NotFound"),
        static_cast<int32>(Missing.Status), static_cast<int32>(EDriveResolveStatus::NotFound));
    TestFalse(TEXT("NotFound result carries no live widget"), Missing.Widget.IsValid());

    return true;
}
