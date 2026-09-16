// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Handlers/Drive/DriveElementFactory.h"

#include "Layout/Geometry.h"
#include "Layout/Visibility.h"
#include "Widgets/SWidget.h"

namespace DriveElementFactory
{
    FAncestorState FAncestorState::ForChildrenOf(const TSharedRef<SWidget>& Widget) const
    {
        FAncestorState Child;

        // Mirrors the engine's own recurrence for this quantity, FSlateInvalidationWidgetVisibility
        // (SlateCore/Public/FastUpdate/WidgetProxy.h): a child's ancestors count as visible only
        // when the whole chain above it, this widget included, is drawn. Collapsed and Hidden are
        // the only EVisibility values whose IsVisible() is false, so a HitTestInvisible or
        // SelfHitTestInvisible parent still leaves its children visible - which is what keeps the
        // SelfHitTestInvisible allowance below meaningful for nested HUD labels.
        Child.bAncestorsVisible = bAncestorsVisible && Widget->GetVisibility().IsVisible();

        // Mirrors SWidget::ShouldBeEnabled(bool InParentEnabled) == InParentEnabled && IsEnabled().
        // This is also what recovers a disabled UMG/CommonUI button: UWidget::SetIsEnabled writes
        // the flag onto the widget's OWN cached SWidget (UMG/Private/Components/Widget.cpp), which
        // is an ancestor of the inner Slate leaf the walk reports, never onto that leaf.
        Child.bAncestorsEnabled = bAncestorsEnabled && Widget->IsEnabled();

        return Child;
    }

    void FillStateAndGeometry(
        const TSharedRef<SWidget>& SlateWidget,
        const FAncestorState& Ancestors,
        FDriveElement& OutElement)
    {
        // The widget's own bit follows EVisibility::IsVisible() ("is drawn") rather than
        // == Visible, so that SelfHitTestInvisible status text (a common case for non-clickable
        // HUD labels) still reports visible for verification reads. The ancestor term is what
        // makes the published value EFFECTIVE rather than a local attribute read.
        OutElement.bVisible = Ancestors.bAncestorsVisible && SlateWidget->GetVisibility().IsVisible();
        OutElement.bEnabled = Ancestors.bAncestorsEnabled && SlateWidget->IsEnabled();
        OutElement.bFocused = SlateWidget->HasAnyUserFocus().IsSet();

        // DESKTOP space, and already so: GetTickSpaceGeometry() is the non-deprecated spelling of
        // what GetCachedGeometry() forwards to (SlateCore/Private/Widgets/SWidget.cpp:1163-1171,
        // both returning PersistentState.DesktopGeometry), and SWidget::Paint stores that as the
        // window-space AllottedGeometry with the owning window's GetPositionInScreen() already
        // appended (SWidget.cpp:1494-1495, offset supplied by SWindow::PaintWindow's FPaintArgs,
        // SWindow.cpp:2131; FSlateInvalidationRoot::AdjustWidgetsDesktopGeometry re-applies it
        // when the window moves). So no GetPositionInScreen() term may be added here - that would
        // double-offset every published rect. GetPaintSpaceGeometry() is NOT interchangeable: it
        // returns the raw window-space AllottedGeometry, which is the one coordinate FDriveInput
        // must never be handed.
        const FGeometry& DesktopGeometry = SlateWidget->GetTickSpaceGeometry();

        // Slate skips arranging and painting a subtree an ancestor collapsed or hid, so a widget
        // that is not drawn keeps whatever rect the last frame that DID draw it left behind -
        // real-looking, plausible, and wrong. Publishing those numbers is what let the action gate
        // aim a click at whatever occupies that space now, so they are zeroed and flagged instead
        // of carried forward.
        OutElement.bGeometryStale = !OutElement.bVisible;
        if (OutElement.bGeometryStale)
        {
            OutElement.AbsolutePosition = FVector2D::ZeroVector;
            OutElement.AbsoluteSize = FVector2D::ZeroVector;
        }
        else
        {
            OutElement.AbsolutePosition = FVector2D(DesktopGeometry.GetAbsolutePosition());
            OutElement.AbsoluteSize = FVector2D(DesktopGeometry.GetAbsoluteSize());
        }
    }
}
