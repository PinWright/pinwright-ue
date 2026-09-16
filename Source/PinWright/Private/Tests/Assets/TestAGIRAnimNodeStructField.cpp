// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression test for B-agir-alphaboolblend-empty-struct-not-reimportable.
//
// The AGIR emitter (FIrTextUtils::FormatReflectedPropertyValue) quotes every
// struct export — so a default FInputAlphaBoolBlend, whose ExportText collapses
// to `()`, is emitted as the quoted literal `"()"`. The compile-side reflective
// writer WriteAnimNodeFieldByName fed that raw quoted text straight into
// FProperty::ImportText, which for a struct expects to begin with `(` and
// rejects the leading `"` — so the field failed re-import ("ImportText failed
// for 'AlphaBoolBlend' ...") and was silently dropped, breaking the decompile ->
// compile round-trip-lossless guarantee. Its two sibling reflective writers
// (WriteUObjectFieldByName, TryWriteEditorClassField) already unquote first;
// this one was the lone outlier. The fix adds the same unquote step.
//
// This test exercises WriteAnimNodeFieldByName directly (production code) on a
// transient UAnimGraphNode_ApplyAdditive — no AnimBP/asset fixture needed,
// because the field resolver works purely by reflection over the node's
// FAnimNode_* struct property. Counterfactual: reverting the unquote in
// WriteAnimNodeFieldByName makes the empty `"()"` write return a non-empty
// "ImportText failed" error and fails the first assertion.
#include "Misc/AutomationTest.h"


#include "AnimGraphNode_ApplyAdditive.h"
#include "AnimNodes/AnimNode_ApplyAdditive.h"
#include "Animation/InputScaleBias.h"
#include "Handlers/Animation/AnimGraphConstructionUtils.h"
#include "UObject/Package.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAGIRAnimNodeStructFieldReimportTest,
    "PinWright.AGIR.AnimNodeField.QuotedStructReimports",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAGIRAnimNodeStructFieldReimportTest::RunTest(const FString& Parameters)
{
    UAnimGraphNode_ApplyAdditive* Node = NewObject<UAnimGraphNode_ApplyAdditive>(GetTransientPackage());
    if (!TestNotNull(TEXT("ApplyAdditive editor node created"), Node))
    {
        return true;
    }

    // 1) The exact decompiler output for a default AlphaBoolBlend: a quoted
    //    empty-struct literal. This is the reported lossy-field case. Before the
    //    fix this returned the non-empty "ImportText failed for 'AlphaBoolBlend'"
    //    error and the field dropped on round-trip.
    const FString EmptyError = AnimGraphConstructionUtils::WriteAnimNodeFieldByName(
        Node, FName(TEXT("AlphaBoolBlend")), TEXT("\"()\""));
    TestEqual(
        TEXT("quoted empty struct literal \"()\" re-imports without error"),
        EmptyError, FString());

    // 2) A quoted NON-default struct body must also re-import (the same quote
    //    asymmetry would drop ANY quoted struct field, not just the empty one),
    //    and its editable members must actually land.
    const FString NonDefaultError = AnimGraphConstructionUtils::WriteAnimNodeFieldByName(
        Node, FName(TEXT("AlphaBoolBlend")), TEXT("\"(BlendInTime=0.250000,BlendOutTime=0.500000)\""));
    TestEqual(
        TEXT("quoted non-default struct body re-imports without error"),
        NonDefaultError, FString());

    if (FAnimNode_ApplyAdditive* RuntimeNode = &Node->Node)
    {
        TestEqual(TEXT("BlendInTime imported from quoted struct body"),
            RuntimeNode->AlphaBoolBlend.BlendInTime, 0.25f);
        TestEqual(TEXT("BlendOutTime imported from quoted struct body"),
            RuntimeNode->AlphaBoolBlend.BlendOutTime, 0.5f);
    }

    // 3) A field that does not exist still resolves to a clear error (guards
    //    against the unquote masking the resolver's own diagnostics).
    const FString MissingError = AnimGraphConstructionUtils::WriteAnimNodeFieldByName(
        Node, FName(TEXT("NoSuchField")), TEXT("\"()\""));
    TestFalse(TEXT("missing field still reports an error"), MissingError.IsEmpty());

    return true;
}
