// Copyright (c) 2026 Alexander Penkin. MIT License.

// Tests for the one derivation of an FDriveElement's live state: DriveElementFactory.
//
// Three properties are pinned, each of which the drive path got wrong when every element was
// stamped from the widget's OWN Slate attributes:
//
//  1. Published geometry is DESKTOP space. Slate stores a painted widget's cached/tick-space
//     geometry with the owning window's GetPositionInScreen() already appended, and FDriveInput
//     injects in desktop pixels, so the published rect must equal window origin + window-space
//     position exactly - neither the raw window-space value nor that value with the window
//     origin added a second time.
//  2. A child of a Collapsed ancestor reports visible:false with no geometry. Slate never writes
//     an ancestor's Collapsed state onto the child, and never arranges the subtree either, so the
//     child's own flag says "visible" while its stored rect is whatever the last frame that did
//     draw it left behind.
//  3. A child of a disabled ancestor reports enabled:false, for the same reason on the adjacent
//     flag - which is also how a UMG/CommonUI button disabled through SetIsEnabled(false) reads,
//     since that writes the flag onto the button's own widget, an ancestor of the Slate leaf.
//
// The three live tests drive the REAL editor-chrome walk (FDriveEditorChrome::BuildElementList)
// over a uniquely-titled fixture window placed at a non-zero desktop origin, so the ancestor
// threading in WalkAndCollect is exercised end to end rather than by calling the helper by hand.
// The fourth test calls the helper directly and needs neither a window nor a paint, so the
// visibility/enabled folding still has a non-skippable assertion on a host that cannot draw.

#include "Misc/AutomationTest.h"

#include "Handlers/Drive/DriveActionCommon.h"
#include "Handlers/Drive/DriveEditorChrome.h"
#include "Handlers/Drive/DriveElementFactory.h"
#include "Handlers/Drive/DriveTypes.h"

#include "Tests/TestSkipReporting.h"

#include "Framework/Application/SlateApplication.h"
#include "Layout/Visibility.h"
#include "Misc/Guid.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SWindow.h"
#include "Widgets/Text/STextBlock.h"

namespace DriveElementFactoryTest
{
    const TCHAR* VisibleLabel   = TEXT("PwElementFactoryVisibleLeaf");
    const TCHAR* CollapsedLabel = TEXT("PwElementFactoryCollapsedLeaf");
    const TCHAR* DisabledLabel  = TEXT("PwElementFactoryDisabledLeaf");

    // A shown, uniquely-titled window at a non-zero desktop origin holding three labelled leaves:
    // one plainly visible/enabled, one under a Collapsed border, one under a disabled border. Each
    // leaf keeps its OWN visibility Visible and its own enabled true, so any false the walk reports
    // can only have come from the ancestor fold.
    struct FFixtureWindow
    {
        explicit FFixtureWindow(FSlateApplication& InSlateApp)
            : SlateApp(InSlateApp)
            , Title(FString::Printf(TEXT("PW_ElementFactory_%s"),
                *FGuid::NewGuid().ToString(EGuidFormats::Digits)))
        {
            VisibleLeaf   = SNew(STextBlock).Text(FText::FromString(VisibleLabel));
            CollapsedLeaf = SNew(STextBlock).Text(FText::FromString(CollapsedLabel));
            DisabledLeaf  = SNew(STextBlock).Text(FText::FromString(DisabledLabel));

            CollapsedParent = SNew(SBorder)[CollapsedLeaf.ToSharedRef()];
            DisabledParent  = SNew(SBorder)[DisabledLeaf.ToSharedRef()];
            CollapsedParent->SetVisibility(EVisibility::Collapsed);
            DisabledParent->SetEnabled(false);

            Window = SNew(SWindow)
                .Title(FText::FromString(Title))
                // Deliberately off the desktop origin: the whole point of the geometry assertion
                // is that a window at (0,0) cannot tell window space from desktop space apart.
                .ScreenPosition(FVector2D(320.0f, 240.0f))
                .ClientSize(FVector2D(520.0f, 360.0f))
                .FocusWhenFirstShown(false)
                .SupportsMaximize(false)
                .SupportsMinimize(false);

            Window->SetContent(
                SNew(SVerticalBox)
                + SVerticalBox::Slot().AutoHeight()[SNew(SBorder)[VisibleLeaf.ToSharedRef()]]
                + SVerticalBox::Slot().AutoHeight()[CollapsedParent.ToSharedRef()]
                + SVerticalBox::Slot().AutoHeight()[DisabledParent.ToSharedRef()]);

            SlateApp.AddWindow(Window.ToSharedRef(), /*bShowImmediately=*/true);
            for (int32 Tick = 0; Tick < 8; ++Tick)
            {
                SlateApp.Tick(ESlateTickType::All);
            }
        }

        ~FFixtureWindow()
        {
            SlateApp.RequestDestroyWindow(Window.ToSharedRef());
            // RequestDestroyWindow only QUEUES the destruction; without a tick the fixture window
            // is still enumerated (and can still be the active top-level window) when the next
            // test walks editor chrome.
            SlateApp.Tick(ESlateTickType::All);
        }

        FSlateApplication& SlateApp;
        FString Title;
        TSharedPtr<SWindow> Window;
        TSharedPtr<STextBlock> VisibleLeaf;
        TSharedPtr<STextBlock> CollapsedLeaf;
        TSharedPtr<STextBlock> DisabledLeaf;
        TSharedPtr<SBorder> CollapsedParent;
        TSharedPtr<SBorder> DisabledParent;
    };

    const FDriveElement* FindByLabel(const TArray<FDriveElement>& Elements, const TCHAR* Label)
    {
        for (const FDriveElement& Element : Elements)
        {
            if (Element.Label == Label)
            {
                return &Element;
            }
        }
        return nullptr;
    }

    // Build the element list over the fixture window. Returns false (already reported as a skip or
    // an error) when the walk could not run.
    bool BuildFixtureElements(
        FAutomationTestBase& Test,
        const FFixtureWindow& Fixture,
        TArray<FDriveElement>& OutElements)
    {
        FDriveWindowSelector Selector;
        Selector.Title = Fixture.Title;

        FString WindowTitle;
        FString ErrorCode;
        FString ErrorMessage;
        if (FDriveEditorChrome::BuildElementList(Selector, OutElements, WindowTitle, ErrorCode, ErrorMessage))
        {
            return true;
        }

        // The fixture window is ours and was just shown, so the only tolerable failures are the
        // "this host has no live Slate windows at all" ones.
        if (ErrorCode == TEXT("SLATE_NOT_INITIALIZED")
            || ErrorCode == TEXT("NO_WINDOWS")
            || ErrorCode == TEXT("WINDOW_NOT_FOUND"))
        {
            PinWrightTestSkip::SkipAssertions(Test, TEXT("fixture-window-not-enumerated"),
                FString::Printf(
                    TEXT("The fixture window '%s' could not be walked: %s - %s"),
                    *Fixture.Title, *ErrorCode, *ErrorMessage));
            return false;
        }

        Test.AddError(FString::Printf(
            TEXT("Unexpected element-list failure over the fixture window: %s - %s"),
            *ErrorCode, *ErrorMessage));
        return false;
    }

    bool RequireSlate(FAutomationTestBase& Test)
    {
        if (FSlateApplication::IsInitialized())
        {
            return true;
        }
        PinWrightTestSkip::SkipAssertions(Test, TEXT("slate_not_initialized"),
            TEXT("FSlateApplication is not initialized; the drive element fixture cannot be built."));
        return false;
    }
}

// ============================================================================
// Pure: the ancestor fold itself. No window, no paint, so this assertion runs on every host.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDriveElementFactoryAncestorFoldTest,
    "PinWright.drive.element_factory.AncestorStateFoldsVisibilityAndEnabled",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDriveElementFactoryAncestorFoldTest::RunTest(const FString& Parameters)
{
    using namespace DriveElementFactory;

    TSharedRef<STextBlock> Leaf = SNew(STextBlock).Text(FText::FromString(TEXT("leaf")));
    TSharedRef<SBorder> CollapsedParent = SNew(SBorder)[Leaf];
    CollapsedParent->SetVisibility(EVisibility::Collapsed);

    TSharedRef<STextBlock> Leaf2 = SNew(STextBlock).Text(FText::FromString(TEXT("leaf2")));
    TSharedRef<SBorder> DisabledParent = SNew(SBorder)[Leaf2];
    DisabledParent->SetEnabled(false);

    TSharedRef<STextBlock> Leaf3 = SNew(STextBlock).Text(FText::FromString(TEXT("leaf3")));
    TSharedRef<SBorder> SelfHitTestInvisibleParent = SNew(SBorder)[Leaf3];
    SelfHitTestInvisibleParent->SetVisibility(EVisibility::SelfHitTestInvisible);

    // The leaves themselves are untouched: every false below comes from the fold, not the leaf.
    TestTrue(TEXT("the collapsed parent's leaf is itself visible"), Leaf->GetVisibility().IsVisible());
    TestTrue(TEXT("the disabled parent's leaf is itself enabled"), Leaf2->IsEnabled());

    const FAncestorState Root;
    TestTrue(TEXT("a fresh ancestor state is visible"), Root.bAncestorsVisible);
    TestTrue(TEXT("a fresh ancestor state is enabled"), Root.bAncestorsEnabled);

    {
        FDriveElement Element;
        FillStateAndGeometry(Leaf, Root.ForChildrenOf(CollapsedParent), Element);
        TestFalse(TEXT("a child of a Collapsed ancestor is not visible"), Element.bVisible);
        TestTrue(TEXT("a child of a Collapsed ancestor is marked geometry-stale"), Element.bGeometryStale);
        TestTrue(TEXT("a stale element carries no position"), Element.AbsolutePosition.IsNearlyZero());
        TestTrue(TEXT("a stale element carries no size"), Element.AbsoluteSize.IsNearlyZero());
        // Zeroing is only safe because the shared action gate refuses first; the center an
        // ungated caller would compute from this element is desktop (0,0).
        TestFalse(TEXT("a stale element is not actionable"), FDriveActionCommon::IsActionable(Element));
    }

    {
        FDriveElement Element;
        FillStateAndGeometry(Leaf2, Root.ForChildrenOf(DisabledParent), Element);
        TestFalse(TEXT("a child of a disabled ancestor is not enabled"), Element.bEnabled);
        // Disabled is not hidden: the element stays visible, and so keeps live geometry. (The
        // geometry itself is asserted in the live test below; these synthetic widgets were never
        // painted, so there is nothing meaningful to measure here.)
        TestTrue(TEXT("a child of a disabled ancestor is still visible"), Element.bVisible);
    }

    {
        // The SelfHitTestInvisible allowance must survive the fold: those parents ARE drawn, so a
        // nested HUD label under one keeps reporting visible for verification reads.
        FDriveElement Element;
        FillStateAndGeometry(Leaf3, Root.ForChildrenOf(SelfHitTestInvisibleParent), Element);
        TestTrue(TEXT("a child of a SelfHitTestInvisible ancestor stays visible"), Element.bVisible);
    }

    {
        // The fold is cumulative: once an ancestor is collapsed, a visible ancestor below it
        // cannot restore visibility for the subtree.
        TSharedRef<SBorder> Inner = SNew(SBorder);
        const FAncestorState UnderCollapsed = Root.ForChildrenOf(CollapsedParent);
        const FAncestorState UnderInner = UnderCollapsed.ForChildrenOf(Inner);
        TestFalse(TEXT("a visible ancestor below a Collapsed one does not restore visibility"),
            UnderInner.bAncestorsVisible);
    }

    return true;
}

// ============================================================================
// Live: published geometry is desktop space (window origin included exactly once).
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDriveElementFactoryDesktopGeometryTest,
    "PinWright.drive.element_factory.GeometryIsDesktopSpace",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDriveElementFactoryDesktopGeometryTest::RunTest(const FString& Parameters)
{
    using namespace DriveElementFactoryTest;

    if (!RequireSlate(*this))
    {
        return true;
    }

    FFixtureWindow Fixture(FSlateApplication::Get());

    const FVector2D WindowOrigin(Fixture.Window->GetPositionInScreen());
    if (WindowOrigin.IsNearlyZero())
    {
        // At the desktop origin window space and desktop space are numerically identical, so the
        // assertion below could not distinguish them and a pass would mean nothing.
        PinWrightTestSkip::SkipAssertions(*this, TEXT("fixture-window-at-desktop-origin"),
            FString::Printf(TEXT("The fixture window landed at (%.1f, %.1f); a non-zero origin is ")
                TEXT("required to tell window space from desktop space."), WindowOrigin.X, WindowOrigin.Y));
        return true;
    }

    const FVector2D WindowSpacePos(Fixture.VisibleLeaf->GetPaintSpaceGeometry().GetAbsolutePosition());
    const FVector2D WindowSpaceSize(Fixture.VisibleLeaf->GetPaintSpaceGeometry().GetAbsoluteSize());
    if (WindowSpaceSize.IsNearlyZero())
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("fixture-window-never-painted"),
            TEXT("The fixture window's leaf has no arranged geometry; this host did not paint it."));
        return true;
    }

    TArray<FDriveElement> Elements;
    if (!BuildFixtureElements(*this, Fixture, Elements))
    {
        return true;
    }

    const FDriveElement* Visible = FindByLabel(Elements, VisibleLabel);
    if (!Visible)
    {
        AddError(FString::Printf(TEXT("The fixture's visible leaf '%s' was not emitted by the walk."),
            VisibleLabel));
        return false;
    }

    // Slate's own definition of the value the walk publishes:
    //   PersistentState.DesktopGeometry == AllottedGeometry + WindowToDesktopTransform,
    // where WindowToDesktopTransform is the owning window's GetPositionInScreen(). Publishing the
    // raw window-space value fails this by the window origin; adding the origin on top of the
    // already-desktop value fails it by the same amount in the other direction.
    const FVector2D Expected = WindowOrigin + WindowSpacePos;
    TestTrue(*FString::Printf(
        TEXT("published X is desktop space: got %.1f, expected %.1f (window origin %.1f + window-space %.1f)"),
        Visible->AbsolutePosition.X, Expected.X, WindowOrigin.X, WindowSpacePos.X),
        FMath::IsNearlyEqual(Visible->AbsolutePosition.X, Expected.X, 1.0));
    TestTrue(*FString::Printf(
        TEXT("published Y is desktop space: got %.1f, expected %.1f (window origin %.1f + window-space %.1f)"),
        Visible->AbsolutePosition.Y, Expected.Y, WindowOrigin.Y, WindowSpacePos.Y),
        FMath::IsNearlyEqual(Visible->AbsolutePosition.Y, Expected.Y, 1.0));

    TestFalse(TEXT("a painted, visible element is not marked geometry-stale"), Visible->bGeometryStale);
    TestTrue(TEXT("a painted, visible element carries a non-zero size"),
        !Visible->AbsoluteSize.IsNearlyZero());

    return true;
}

// ============================================================================
// Live: a child of a Collapsed ancestor reads visible:false with zeroed geometry.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDriveElementFactoryCollapsedAncestorTest,
    "PinWright.drive.element_factory.CollapsedAncestorHidesChild",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDriveElementFactoryCollapsedAncestorTest::RunTest(const FString& Parameters)
{
    using namespace DriveElementFactoryTest;

    if (!RequireSlate(*this))
    {
        return true;
    }

    FFixtureWindow Fixture(FSlateApplication::Get());

    TArray<FDriveElement> Elements;
    if (!BuildFixtureElements(*this, Fixture, Elements))
    {
        return true;
    }

    const FDriveElement* Collapsed = FindByLabel(Elements, CollapsedLabel);
    const FDriveElement* Visible = FindByLabel(Elements, VisibleLabel);
    if (!Collapsed || !Visible)
    {
        AddError(TEXT("The fixture's leaves were not both emitted by the walk; a subtree under a ")
            TEXT("Collapsed ancestor must still be listed, only flagged."));
        return false;
    }

    // Anti-vacuous: the leaf's own visibility is untouched, so a reported false can only come from
    // the ancestor fold - and the sibling under a plain parent must still read true, so the fix is
    // not "report everything hidden".
    TestTrue(TEXT("the collapsed leaf's own visibility is still Visible"),
        Fixture.CollapsedLeaf->GetVisibility().IsVisible());
    TestTrue(TEXT("the control leaf under a plain ancestor still reads visible"), Visible->bVisible);

    TestFalse(TEXT("a child of a Collapsed ancestor reads visible:false"), Collapsed->bVisible);
    TestTrue(TEXT("its geometry is marked stale"), Collapsed->bGeometryStale);
    TestTrue(TEXT("its position is zeroed, not last frame's"), Collapsed->AbsolutePosition.IsNearlyZero());
    TestTrue(TEXT("its size is zeroed, not last frame's"), Collapsed->AbsoluteSize.IsNearlyZero());

    // The action gate that both the press target (FDriveActionCommon::RunAction) and
    // drive.drag's to_handle release target run. Zeroing the rect is only safe because this
    // refuses first: the center an ungated caller would compute from a stale element is
    // literally desktop (0,0), so an accepted collapsed to_handle would drag to the corner of
    // the screen and report a benign settle outcome.
    const FVector2D StaleCenter = Collapsed->AbsolutePosition + Collapsed->AbsoluteSize * 0.5;
    TestTrue(TEXT("an ungated stale target's computed center is desktop (0,0)"),
        StaleCenter.IsNearlyZero());
    TestFalse(TEXT("a stale element is refused by the action gate"),
        FDriveActionCommon::IsActionable(*Collapsed));
    TestTrue(TEXT("the live control element passes the action gate"),
        FDriveActionCommon::IsActionable(*Visible));

    return true;
}

// ============================================================================
// Live: a child of a disabled ancestor reads enabled:false.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDriveElementFactoryDisabledAncestorTest,
    "PinWright.drive.element_factory.DisabledAncestorDisablesChild",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDriveElementFactoryDisabledAncestorTest::RunTest(const FString& Parameters)
{
    using namespace DriveElementFactoryTest;

    if (!RequireSlate(*this))
    {
        return true;
    }

    FFixtureWindow Fixture(FSlateApplication::Get());

    TArray<FDriveElement> Elements;
    if (!BuildFixtureElements(*this, Fixture, Elements))
    {
        return true;
    }

    const FDriveElement* Disabled = FindByLabel(Elements, DisabledLabel);
    const FDriveElement* Visible = FindByLabel(Elements, VisibleLabel);
    if (!Disabled || !Visible)
    {
        AddError(TEXT("The fixture's leaves were not both emitted by the walk."));
        return false;
    }

    // Anti-vacuous, both directions: the leaf's own enabled flag is untouched (Slate never writes a
    // parent's disabled state down), and the control leaf must still read enabled.
    TestTrue(TEXT("the disabled leaf's own IsEnabled() is still true"), Fixture.DisabledLeaf->IsEnabled());
    TestTrue(TEXT("the control leaf under an enabled ancestor still reads enabled"), Visible->bEnabled);

    TestFalse(TEXT("a child of a disabled ancestor reads enabled:false"), Disabled->bEnabled);
    // Disabled is not hidden: it stays listed, visible, and keeps live (non-stale) geometry, so a
    // caller can still find it, assert on it, and read where it sits.
    TestTrue(TEXT("a disabled child is still reported visible"), Disabled->bVisible);
    TestFalse(TEXT("a disabled child's geometry is not marked stale"), Disabled->bGeometryStale);
    TestFalse(TEXT("a disabled child is refused by the action gate"),
        FDriveActionCommon::IsActionable(*Disabled));

    return true;
}
