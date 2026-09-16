// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression tests for the editable-text value read-back fix
// (board: B-drive-editable-text-reads-hint-not-value).
//
// The defect: drive.observe/expect derived an element's Label from
// GetAccessibleText(EAccessibleType::Main), which the engine resolves to an editable widget's
// HINT text rather than its typed value (SEditableText::GetDefaultAccessibleText returns
// GetHintText()). So a field the caller had just typed into reported its placeholder, and
// text_equals/text_contains silently compared against that placeholder.
//
// The fix: DriveSlateTextValue::ReadEditableWidgetText reads the live value via GetText() (wired
// into both MakeElement copies as FDriveElement::Value), and FDriveConditionEval::TextEquals /
// TextContains prefer that Value over Label. These tests exercise the production reader over real
// in-code editable widgets built with a HINT set, plus the production condition evaluator, so a
// revert (dropping the GetText() read or the Value-preferring comparand) fails them. The fixtures
// are built entirely in code (no content asset needed) and touch no Lyra content.

#include "Misc/AutomationTest.h"

#include "Handlers/Drive/DriveConditionEval.h"
#include "Handlers/Drive/DriveSlateTextValue.h"
#include "Handlers/Drive/DriveTypes.h"

#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/Input/SEditableText.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Input/SMultiLineEditableTextBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/SMultiLineEditableText.h"
#include "Widgets/Text/STextBlock.h"

// ============================================================================
// ReadEditableWidgetText returns the TYPED value (GetText), never the hint, for
// every base editable Slate type; a non-editable widget reads empty.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDriveEditableTextValueReadTest,
    "PinWright.drive.editabletext.ReadValueNotHint",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FDriveEditableTextValueReadTest::RunTest(const FString& Parameters)
{
    const FText Hint = FText::FromString(TEXT("Search Assets"));
    const FText Typed = FText::FromString(TEXT("Cube"));

    // SEditableText: the exact widget the Content Browser search box wraps and the walk emits.
    {
        const TSharedRef<SEditableText> Editable =
            SNew(SEditableText).HintText(Hint).Text(Typed);
        const FString Value = DriveSlateTextValue::ReadEditableWidgetText(Editable);
        TestEqual(TEXT("SEditableText value is the typed text, not the hint"), Value, FString(TEXT("Cube")));
        TestNotEqual(TEXT("SEditableText value is not the hint"), Value, FString(TEXT("Search Assets")));
    }

    // SEditableTextBox (base of SSearchBox / SFilterSearchBox / SAssetSearchBox).
    {
        const TSharedRef<SEditableTextBox> EditableBox =
            SNew(SEditableTextBox).HintText(Hint).Text(Typed);
        TestEqual(TEXT("SEditableTextBox value is the typed text"),
            DriveSlateTextValue::ReadEditableWidgetText(EditableBox), FString(TEXT("Cube")));
    }

    // SMultiLineEditableText.
    {
        const TSharedRef<SMultiLineEditableText> MultiLine =
            SNew(SMultiLineEditableText).HintText(Hint).Text(Typed);
        TestEqual(TEXT("SMultiLineEditableText value is the typed text"),
            DriveSlateTextValue::ReadEditableWidgetText(MultiLine), FString(TEXT("Cube")));
    }

    // SMultiLineEditableTextBox.
    {
        const TSharedRef<SMultiLineEditableTextBox> MultiLineBox =
            SNew(SMultiLineEditableTextBox).HintText(Hint).Text(Typed);
        TestEqual(TEXT("SMultiLineEditableTextBox value is the typed text"),
            DriveSlateTextValue::ReadEditableWidgetText(MultiLineBox), FString(TEXT("Cube")));
    }

    // An empty editable field reads empty even when a hint is set (so an empty field is
    // distinguishable from a hint-only one - the reason Value is kept distinct from Label).
    {
        const TSharedRef<SEditableText> Empty = SNew(SEditableText).HintText(Hint);
        TestTrue(TEXT("empty editable field has empty value despite a hint"),
            DriveSlateTextValue::ReadEditableWidgetText(Empty).IsEmpty());
    }

    // A non-editable widget is not matched and reads empty.
    {
        const TSharedRef<STextBlock> Static = SNew(STextBlock).Text(FText::FromString(TEXT("Label")));
        TestTrue(TEXT("non-editable widget reads empty value"),
            DriveSlateTextValue::ReadEditableWidgetText(Static).IsEmpty());
    }

    return true;
}

// ============================================================================
// TextEquals / TextContains verify against the typed Value when present, and
// report it as value="..."; a hint-only element (Value empty, Label = hint)
// still compares against Label so nothing regresses.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDriveEditableTextConditionTest,
    "PinWright.drive.editabletext.ConditionPrefersValue",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FDriveEditableTextConditionTest::RunTest(const FString& Parameters)
{
    // The search box AFTER typing "Cube": Label is still the hint (accessible text), Value is the
    // typed text. This is exactly the state the ticket's repro produced.
    FDriveElement Field;
    Field.Handle = TEXT("SAssetSearchBox/.../SEditableText[0]");
    Field.Type = TEXT("SEditableText");
    Field.Label = TEXT("Search Assets"); // hint, as ExtractLabel/accessible text yields
    Field.Value = TEXT("Cube");          // typed value, as the fix now reads

    TArray<FDriveElement> Elements;
    Elements.Add(Field);

    auto EvalText = [&Elements](EDriveConditionType Type, const FString& Expected) -> FDriveConditionResult
    {
        FDriveCondition C;
        C.Type = Type;
        C.Target = TEXT("SAssetSearchBox/.../SEditableText[0]");
        C.ExpectedText = Expected;
        return FDriveConditionEval::Evaluate(C, Elements, FDriveJournalDelta());
    };

    // text_equals against the typed value is MET (pre-fix it compared the hint and failed).
    {
        const FDriveConditionResult R = EvalText(EDriveConditionType::TextEquals, TEXT("Cube"));
        TestTrue(TEXT("text_equals matches the typed value"), R.bMet);
        TestEqual(TEXT("actual reports the value, not the label"), R.Actual, TEXT("value=\"Cube\""));
    }

    // text_equals against the hint is NOT met - the condition no longer sees the placeholder.
    {
        const FDriveConditionResult R = EvalText(EDriveConditionType::TextEquals, TEXT("Search Assets"));
        TestFalse(TEXT("text_equals no longer matches the hint"), R.bMet);
    }

    // text_contains against a substring of the typed value is MET.
    {
        const FDriveConditionResult R = EvalText(EDriveConditionType::TextContains, TEXT("ub"));
        TestTrue(TEXT("text_contains matches a substring of the typed value"), R.bMet);
        TestEqual(TEXT("contains actual reports the value"), R.Actual, TEXT("value=\"Cube\""));
    }

    // Fallback: an element with only a Label (empty Value, e.g. a static status text, or an
    // empty editable field showing just its hint) still compares against Label and reports it.
    {
        FDriveElement HintOnly;
        HintOnly.Handle = TEXT("status");
        HintOnly.Type = TEXT("STextBlock");
        HintOnly.Label = TEXT("Ready");
        // Value intentionally empty.

        TArray<FDriveElement> LabelElements;
        LabelElements.Add(HintOnly);

        FDriveCondition C;
        C.Type = EDriveConditionType::TextEquals;
        C.Target = TEXT("status");
        C.ExpectedText = TEXT("Ready");
        const FDriveConditionResult R = FDriveConditionEval::Evaluate(C, LabelElements, FDriveJournalDelta());
        TestTrue(TEXT("empty-value element still matches on label"), R.bMet);
        TestEqual(TEXT("empty-value element reports label, not value"), R.Actual, TEXT("label=\"Ready\""));
    }

    return true;
}
