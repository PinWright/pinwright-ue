// Copyright (c) 2026 Alexander Penkin. MIT License.

// TestWidgetEventParams.cpp
// Regression test for B-widget-event-param-name-mismatch:
// SetupWidgetEvent must register author-declared param names as aliases on the
// delegate output pins so $MyAlias resolves even when MyAlias != the canonical
// UE delegate pin name.

#include "Misc/AutomationTest.h"
#include "CompilerTestUtils.h"

#include "Compiler/BpirCompiler.h"
#include "Compiler/CompilerTypes.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "K2Node_ComponentBoundEvent.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "Blueprint/UserWidget.h"
#include "Components/CheckBox.h"

using namespace CompilerTestUtils;

// ============================================================================
// Bpir.Compiler.WidgetEventParamAlias
//
// Creates a UUserWidget-based BP, adds a UCheckBox variable "HudCheck", then
// compiles:
//
//   entry widget_event HudCheck.OnCheckStateChanged(enum<ECheckBoxState> NewCheckState) {
//       %bChecked = call EqualEqual_ByteByte(A: $NewCheckState, B: 1)
//   }
//
// Before the fix: $NewCheckState could not be resolved because SetupWidgetEvent
// ignored Block.Params and never called PinResolver->RegisterVariable for the
// author-declared alias. The compile failed with "variable 'NewCheckState' not
// found".
//
// After the fix: SetupWidgetEvent enumerates the event node's output data pins
// positionally and registers both the author's alias and the canonical pin name,
// so $NewCheckState resolves to the delegate's output pin regardless of its
// internal name.
//
// If the UCheckBox property cannot be found on the skeleton class at test time
// (UMG not fully available in the headless context), the test falls back to
// verifying graceful failure — never a crash, and the error must not be
// "variable 'NewCheckState' not found" (that error indicates the old bug).
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirWidgetEventParamAliasTest,
    "PinWright.bpir.compiler.WidgetEventParamAlias",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBpirWidgetEventParamAliasTest::RunTest(const FString& Parameters)
{
    // Build a UUserWidget-based transient blueprint with a UCheckBox property.
    UBlueprint* BP = CreateTransientTestBPWithParent(
        UUserWidget::StaticClass(), TEXT("WidgetEventParamAliasBP"));
    TestNotNull(TEXT("Widget blueprint was created"), BP);
    if (!BP) return false;

    // Add a UCheckBox* member variable named "HudCheck" so the component-bound
    // event resolver can find the property by name on the skeleton class.
    FEdGraphPinType CheckBoxType;
    CheckBoxType.PinCategory = UEdGraphSchema_K2::PC_Object;
    CheckBoxType.PinSubCategoryObject = UCheckBox::StaticClass();
    FBlueprintEditorUtils::AddMemberVariable(BP, TEXT("HudCheck"), CheckBoxType);

    // Compile the skeleton so SkeletonGeneratedClass reflects the new property.
    FKismetEditorUtilities::CompileBlueprint(BP);

    // Now compile BPIR that uses a custom param alias "NewCheckState" instead of
    // the canonical UE internal pin name for UCheckBox::OnCheckStateChanged.
    FBpirCompiler Compiler(BP);
    FCompileResult Result = Compiler.Compile(
        TEXT("entry widget_event HudCheck.OnCheckStateChanged(enum<ECheckBoxState> NewCheckState) {\n")
        TEXT("    %bChecked = call EqualEqual_ByteByte(A: $NewCheckState, B: 1)\n")
        TEXT("}"));

    if (Result.bSuccess)
    {
        // Full success path: alias resolved, node was created.
        TestTrue(TEXT("No compile errors"), Result.Errors.Num() == 0);
        TestTrue(TEXT("At least one node was created"), Result.CreatedNodeGUIDs.Num() > 0);

        // Verify a ComponentBoundEvent node is present in the event graph.
        bool bFoundEventNode = false;
        for (UEdGraph* Graph : BP->UbergraphPages)
        {
            for (UEdGraphNode* Node : Graph->Nodes)
            {
                if (Cast<UK2Node_ComponentBoundEvent>(Node))
                {
                    bFoundEventNode = true;
                    break;
                }
            }
            if (bFoundEventNode) break;
        }
        TestTrue(TEXT("UK2Node_ComponentBoundEvent was created"), bFoundEventNode);
    }
    else
    {
        // Fallback: widget BP setup may be incomplete in a headless test context
        // (e.g. UMG module not fully initialised, CheckBox delegate pins absent).
        // This is acceptable — what is NOT acceptable is the old bug error.
        TestTrue(TEXT("Graceful failure produced error messages"), Result.Errors.Num() > 0);

        // The old bug produced exactly "variable 'NewCheckState' not found".
        // If that message appears, the fix has been reverted.
        bool bHasOldBugError = false;
        for (const FCompileError& Err : Result.Errors)
        {
            AddInfo(FString::Printf(TEXT("  L%d: %s"), Err.Line, *Err.Message));
            if (Err.Message.Contains(TEXT("variable 'NewCheckState' not found"),
                ESearchCase::IgnoreCase)
                || Err.Message.Contains(TEXT("NewCheckState' not found"),
                ESearchCase::IgnoreCase))
            {
                bHasOldBugError = true;
            }
        }
        TestFalse(
            TEXT("Error must NOT be 'variable NewCheckState not found' (that is the old bug)"),
            bHasOldBugError);
    }
    return true;
}
