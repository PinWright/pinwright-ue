// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Widgets/SWidget.h"
#include "Widgets/Input/SEditableText.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Input/SMultiLineEditableTextBox.h"
#include "Widgets/Text/SMultiLineEditableText.h"

// Shared, header-only read-back of an editable Slate widget's *live typed value*.
//
// Why this exists: ExtractLabel (in both DriveEditorChrome.cpp and DriveLiveResolver.cpp)
// derives an element's Label from GetAccessibleText(EAccessibleType::Main). For an editable
// input the engine resolves that accessible text to the widget's HINT/placeholder, not the
// text the user typed (SEditableText::GetDefaultAccessibleText returns GetHintText()). So the
// typed value was never surfaced by observe/expect and text_equals/text_contains could not see
// it. This helper reads the actual value via GetText() so drive.observe can expose a distinct
// `value` and the text conditions can verify what was typed.
//
// It is inline in a uniquely-named namespace (not anonymous) so the single definition is shared
// by both TUs that include it without an ODR clash when Unity merges translation units, the same
// way the plugin's other shared-helper clusters avoid the merged-unit redefinition problem.
//
// Dispatch is by exact leaf type (SWidget::GetType()): the base Slate editable widgets all live
// in the Slate module the plugin already links, but they do NOT share a common GetText() base
// (SEditableText / SMultiLineEditableText derive from SWidget; the *Box variants derive from
// SBorder), so a static_cast keyed on the confirmed exact type is used rather than a virtual
// call. Wrapper widgets such as SSearchBox / SFilterSearchBox / SAssetSearchBox are intentionally
// NOT matched here: the live walk emits their inner SEditableText as its own element, so reading
// GetText() on that inner SEditableText already covers the search-box case (the ticket's own
// repro handle ends in .../SEditableText[0]). Non-editable widgets return an empty string.
namespace DriveSlateTextValue
{
    // Live typed value of an editable widget, or empty for a non-editable widget. Only the exact
    // base editable leaf types are read; a matched type is downcast (safe because GetType() is the
    // registered leaf type, so the object truly is that type).
    inline FString ReadEditableWidgetText(const TSharedRef<SWidget>& Widget)
    {
        const FName Type = Widget->GetType();

        if (Type == TEXT("SEditableText"))
        {
            return static_cast<const SEditableText&>(Widget.Get()).GetText().ToString();
        }
        if (Type == TEXT("SEditableTextBox"))
        {
            return static_cast<const SEditableTextBox&>(Widget.Get()).GetText().ToString();
        }
        if (Type == TEXT("SMultiLineEditableText"))
        {
            return static_cast<const SMultiLineEditableText&>(Widget.Get()).GetText().ToString();
        }
        if (Type == TEXT("SMultiLineEditableTextBox"))
        {
            return static_cast<const SMultiLineEditableTextBox&>(Widget.Get()).GetText().ToString();
        }

        return FString();
    }
}
