// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Handlers/Drive/DriveTypes.h"

class SWidget;

// The single derivation of the live state fields an FDriveElement carries: effective
// visibility, effective enabled state, focus, and desktop-space geometry. Both Slate
// producers (FDriveLiveResolver for game UMG, FDriveEditorChrome for editor chrome) fill
// those five fields identically, so they share this one implementation and cannot drift
// again; each producer still owns the fields that genuinely differ between the surfaces
// (Handle, Path, Surface, label extraction).
//
// This is a named namespace, not an anonymous one: both producers live in the same unity
// TU cluster, where an unnamed-namespace helper of this name would be a redefinition.

namespace DriveElementFactory
{
    // Folded ancestor state carried DOWN a top-down widget walk.
    //
    // Slate stores visibility and enabled state as each widget's OWN attribute and folds the
    // parent chain in only at paint/hit-test time (EVisibility through the arrangement pass,
    // enabled through SWidget::ShouldBeEnabled(bParentEnabled)). Nothing writes an ancestor's
    // Collapsed or disabled state onto the child, so a walk that reads the child's attribute
    // alone reports a widget that is not drawn and cannot be pressed as visible and enabled.
    // The walk therefore threads the folded value instead of re-climbing GetParentWidget()
    // per node, which would re-walk the chain the walk is already descending.
    struct FAncestorState
    {
        // True while every ancestor above this point is itself drawn.
        bool bAncestorsVisible = true;
        // True while every ancestor above this point is itself enabled.
        bool bAncestorsEnabled = true;

        // The state that applies to Widget's children, given this state at Widget.
        FAncestorState ForChildrenOf(const TSharedRef<SWidget>& Widget) const;
    };

    // Fill the live state + geometry fields of OutElement from SlateWidget and the folded
    // ancestor state. Leaves Handle / Type / Label / Value / Path / Surface / MarkIndex alone.
    void FillStateAndGeometry(
        const TSharedRef<SWidget>& SlateWidget,
        const FAncestorState& Ancestors,
        FDriveElement& OutElement);
}
