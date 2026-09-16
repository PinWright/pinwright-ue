// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Misc/AutomationTest.h"
#include "CompilerTestUtils.h"

#include "Compiler/BpirCompiler.h"
#include "Compiler/CompilerTypes.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphPin.h"
#include "K2Node_FormatText.h"

using namespace CompilerTestUtils;

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FBpirCallK2NodeShapeOrderingTest,
    "PinWright.bpir.compiler.k2node.ShapeOrdering",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBpirCallK2NodeShapeOrderingTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("CallK2NodeShapeOrderingBP"));
    TestNotNull(TEXT("Blueprint created"), BP);
    if (!BP) return false;

    // FormatText's Format pin is PC_Text and CoerceStringToPersistedFText demands
    // a localization identity, so the value must be NSLOCTEXT(...) (or equivalent).
    // The braces inside the source string drive PinNames; FormatText.PinDefaultValueChanged
    // populates PinNames from the parsed format pattern, then ReconstructNode re-runs
    // AllocateDefaultPins to materialize the wildcard argument pins.
    const FString Bpir = TEXT(
        "entry event BeginPlay() {\n"
        "    %r = call K2Node_FormatText() node_props { Format: NSLOCTEXT(\"Test\", \"Hello\", \"Hello {Name}, you have {Count} items\") }\n"
        "}"
    );

    FBpirCompiler Compiler(BP);
    FCompileResult CompileResult = Compiler.Compile(Bpir);
    if (!CompileResult.bSuccess)
    {
        for (const FCompileError& Err : CompileResult.Errors)
        {
            AddError(FString::Printf(TEXT("Compile L%d: %s"), Err.Line, *Err.Message));
        }
    }
    TestTrue(TEXT("Compile of FormatText with node_props { Format } succeeded"), CompileResult.bSuccess);
    if (!CompileResult.bSuccess)
    {
        return false;
    }

    UK2Node_FormatText* FormatNode = FindNodeOfType<UK2Node_FormatText>(BP);
    TestNotNull(TEXT("FormatText node placed in BP"), FormatNode);
    if (!FormatNode)
    {
        return false;
    }

    // Counterfactual: if ReplayGenericNodeProps applies all properties after
    // AllocateDefaultPins and skips PinDefaultValueChanged for FormatText, FindPin("Name")
    // and FindPin("Count") return null because PinNames stays empty and the wildcard
    // argument pins are never created.
    UEdGraphPin* NamePin = FormatNode->FindPin(TEXT("Name"));
    UEdGraphPin* CountPin = FormatNode->FindPin(TEXT("Count"));
    TestNotNull(TEXT("FormatText dynamic 'Name' argument pin exists"), NamePin);
    TestNotNull(TEXT("FormatText dynamic 'Count' argument pin exists"), CountPin);

    return true;
}
