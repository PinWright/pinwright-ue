// Copyright (c) 2026 Alexander Penkin. MIT License.

// TestWidgetEventParamAliasLive.cpp
// Regression test for B-widget-event-param-name-mismatch (Phase 2 re-registration fix):
// Verifies that author-declared widget_event param aliases survive PinResolver->Clear()
// and remain resolvable during PreEmitVariableRefs in Phase 2.
//
// Counterfactual: if the RegisterEntryParamAliases call added after PinResolver->Clear()
// in Phase 2 is reverted, this test fails because the $MyCustomName alias is wiped by
// Clear() and PreEmitVariableRefs cannot resolve it, producing a compile error.

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
// Bpir.Compiler.WidgetEventParamAliasLive
//
// Creates a UUserWidget-based BP with a UCheckBox property "HudCheck" in the
// widget tree as a variable, compiles the skeleton, then compiles BPIR that
// uses a custom param alias "MyCustomName":
//
//   entry widget_event HudCheck.OnCheckStateChanged(enum<ECheckBoxState> MyCustomName) {
//       %b = call EqualEqual_ByteByte(A: $MyCustomName, B: 1)
//   }
//
// This specifically pins the Phase-2 alias re-registration fix: Phase 1
// (SetupWidgetEvent) registers the alias on the PinResolver, but Phase 2 then
// calls PinResolver->Clear() before PreEmitVariableRefs runs.  Without the fix,
// $MyCustomName is gone by the time it is needed.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirWidgetEventParamAliasLiveTest,
    "PinWright.bpir.compiler.WidgetEventParamAliasLive",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBpirWidgetEventParamAliasLiveTest::RunTest(const FString& Parameters)
{
    // Build a UUserWidget-based transient blueprint.
    UBlueprint* BP = CreateTransientTestBPWithParent(
        UUserWidget::StaticClass(), TEXT("WidgetEventParamAliasLiveBP"));
    TestNotNull(TEXT("Widget blueprint was created"), BP);
    if (!BP) return false;

    // Add a UCheckBox* member variable "HudCheck" so ComponentBoundEvent can
    // find the property by name on the skeleton class.
    FEdGraphPinType CheckBoxType;
    CheckBoxType.PinCategory = UEdGraphSchema_K2::PC_Object;
    CheckBoxType.PinSubCategoryObject = UCheckBox::StaticClass();
    FBlueprintEditorUtils::AddMemberVariable(BP, TEXT("HudCheck"), CheckBoxType);

    // Skeleton-compile so SkeletonGeneratedClass reflects the new property.
    FKismetEditorUtilities::CompileBlueprint(BP);

    // Compile BPIR using a custom param alias that differs from the UE delegate's
    // canonical pin name (bIsChecked).  This alias must survive Phase 2's
    // PinResolver->Clear() via the RegisterEntryParamAliases re-application.
    FBpirCompiler Compiler(BP);
    FCompileResult Result = Compiler.Compile(
        TEXT("entry widget_event HudCheck.OnCheckStateChanged(enum<ECheckBoxState> MyCustomName) {\n")
        TEXT("    %b = call EqualEqual_ByteByte(A: $MyCustomName, B: 1)\n")
        TEXT("}"));

    TestTrue(
        TEXT("Compile must succeed with custom widget-event param alias on live widget tree"),
        Result.bSuccess && Result.Errors.Num() == 0);

    if (!Result.bSuccess)
    {
        // Log the actual errors to aid diagnosis on failure.
        for (const FCompileError& Err : Result.Errors)
        {
            AddError(FString::Printf(TEXT("  L%d: %s"), Err.Line, *Err.Message));
        }
    }
    return true;
}
