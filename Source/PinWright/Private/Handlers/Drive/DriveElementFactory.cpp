// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Handlers/Drive/DriveElementFactory.h"

#include "Layout/Clipping.h"
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
        //
        // A widget its clipping ancestors leave no pixel of is not drawn either, and the same
        // holds for everything under it.
        const FGeometry& Geometry = Widget->GetTickSpaceGeometry();
        Child.bAncestorsVisible = bAncestorsVisible && Widget->GetVisibility().IsVisible()
            && !ClipsOut(Geometry.GetRenderBoundingRect());

        // Mirrors SWidget::CalculateCullingAndClippingRules: ClipToBounds / ClipToBoundsAlways
        // intersect with the inherited clip, ClipToBoundsWithoutIntersecting replaces it. OnDemand
        // is left out (it clips only when the content overflows, which Slate decides at paint).
        // ponytail: the rects are render BOUNDING rects, so a rotated clipper over-reports what it
        // lets draw (never under-reports); exact clip quads only if rotated menus ever matter.
        Child.ClipRect = ClipRect;
        const EWidgetClipping Clipping = Widget->GetClipping();
        if (Child.bAncestorsVisible && Clipping == EWidgetClipping::ClipToBoundsWithoutIntersecting)
        {
            Child.ClipRect = Geometry.GetRenderBoundingRect();
        }
        else if (Child.bAncestorsVisible
            && (Clipping == EWidgetClipping::ClipToBounds || Clipping == EWidgetClipping::ClipToBoundsAlways))
        {
            Child.ClipRect = ClipRect.IsSet()
                ? ClipRect->IntersectionWith(Geometry.GetRenderBoundingRect())
                : Geometry.GetRenderBoundingRect();
        }

        // An SRetainerWidget paints its content into an SVirtualWindow rooted at (0,0), so the
        // geometry below it is retainer-local and cannot be compared with a desktop clip rect
        // (see FDriveElement::AbsolutePosition). Dropping the clip there only ever errs toward
        // the old visible:true.
        if (Widget->GetType() == TEXT("SRetainerWidget"))
        {
            Child.ClipRect.Reset();
        }

        // Mirrors SWidget::ShouldBeEnabled(bool InParentEnabled) == InParentEnabled && IsEnabled().
        // This is also what recovers a disabled UMG/CommonUI button: UWidget::SetIsEnabled writes
        // the flag onto the widget's OWN cached SWidget (UMG/Private/Components/Widget.cpp), which
        // is an ancestor of the inner Slate leaf the walk reports, never onto that leaf.
        Child.bAncestorsEnabled = bAncestorsEnabled && Widget->IsEnabled();

        return Child;
    }

    bool FAncestorState::ClipsOut(const FSlateRect& Rect) const
    {
        if (!ClipRect.IsSet())
        {
            return false;
        }
        bool bOverlapping = false;
        const FSlateRect Overlap = Rect.IntersectionWith(ClipRect.GetValue(), bOverlapping);
        // Touching the clip edge draws nothing; a zero-area rect (a widget never painted) is
        // judged by contact alone so it is not newly hidden by this check.
        return !bOverlapping || (Rect.GetArea() > 0.0f && Overlap.GetArea() <= 0.0f);
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
        //
        // A widget lying wholly outside a ClipToBounds ancestor (a closed dropdown translated out
        // of its clipping panel) draws nothing and takes no hover or click, so it is not visible
        // either; that is what stops the action gate aiming input at a rect nobody can see.
        //
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
        OutElement.bVisible = Ancestors.bAncestorsVisible && SlateWidget->GetVisibility().IsVisible()
            && !Ancestors.ClipsOut(DesktopGeometry.GetRenderBoundingRect());
        OutElement.bEnabled = Ancestors.bAncestorsEnabled && SlateWidget->IsEnabled();
        OutElement.bFocused = SlateWidget->HasAnyUserFocus().IsSet();

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
