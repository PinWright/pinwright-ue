// Copyright (c) 2026 Alexander Penkin. MIT License.

// TestBpirSetAfterCreateWidget.cpp
// Regression test for E-bpir-set-on-createwidget-needs-cast:
// `set %ref.Prop = value` after `call K2Node_CreateWidget(Class: ...)` must compile
// without error.  Before the fix, CreateGenericK2Node never set ClassPin->DefaultObject,
// so GetClassToSpawn() returned null, GetAuthoritativePinClass fell back to UUserWidget,
// and the Set node had no input pin for TestInt.

#include "Misc/AutomationTest.h"
#include "CompilerTestUtils.h"

#include "Compiler/BpirCompiler.h"
#include "Compiler/CompilerTypes.h"
#include "Blueprint/UserWidget.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_VariableSet.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirSetAfterCreateWidgetTest,
    "PinWright.bpir.compiler.set.AfterCreateWidget",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBpirSetAfterCreateWidgetTest::RunTest(const FString& Parameters)
{
    using namespace CompilerTestUtils;

    // 1. Create a transient UUserWidget-based blueprint with an int32 variable "TestInt".
    UBlueprint* WidgetBP = CreateTransientTestBPWithParent(
        UUserWidget::StaticClass(), TEXT("BpirSetAfterCreateWidgetItem"));
    TestNotNull(TEXT("Widget blueprint was created"), WidgetBP);
    if (!WidgetBP) return false;

    // 2. Add an int32 member variable named "TestInt".
    FEdGraphPinType IntPinType;
    IntPinType.PinCategory = UEdGraphSchema_K2::PC_Int;
    FBlueprintEditorUtils::AddMemberVariable(WidgetBP, TEXT("TestInt"), IntPinType);

    // 3. Compile the widget BP so GeneratedClass is fully instantiated with TestInt.
    FKismetEditorUtilities::CompileBlueprint(WidgetBP);

    UClass* GenClass = WidgetBP->GeneratedClass;
    TestNotNull(TEXT("Widget GeneratedClass is valid after compile"), GenClass);
    if (!GenClass) return false;

    // Path used in the BPIR Class: argument (e.g. /Engine/Transient/Name.Name_C).
    FString GenClassPath = GenClass->GetPathName();
    AddInfo(FString::Printf(TEXT("Widget GeneratedClass path: %s"), *GenClassPath));

    // 4. Create a host blueprint for the BPIR body. Must be UUserWidget-based so
    //    `call GetOwningPlayer(Target: self)` resolves — GetOwningPlayer is a
    //    UUserWidget member, not present on AActor.
    UBlueprint* HostBP = CreateTransientTestBPWithParent(
        UUserWidget::StaticClass(), TEXT("BpirSetAfterCreateWidgetHost"));
    TestNotNull(TEXT("Host blueprint was created"), HostBP);
    if (!HostBP) return false;

    // 5. Compile BPIR: create widget, then set TestInt on the result directly.
    //    Uses `custom_event` for the entry — UUserWidget has no `BeginPlay`
    //    UFunction (its lifecycle is Construct/Destruct), and CreateEventNode
    //    correctly refuses to fabricate a phantom event referencing a missing
    //    parent function. The entry shape is incidental to this regression;
    //    the failure under test is the `set %item.Prop` resolver against a
    //    K2Node_CreateWidget output, which fires the same way under any entry.
    //    Mirrors the verified-live repro shape from
    //    E-bpir-set-on-createwidget-needs-cast history entry #5.
    FString BpirBody = FString::Printf(
        TEXT("entry custom_event T_setcw() {\n")
        TEXT("    %%p = call GetOwningPlayer(Target: self)\n")
        TEXT("    %%item = call K2Node_CreateWidget(Class: %s, OwningPlayer: %%p)\n")
        TEXT("    set %%item.TestInt = 42\n")
        TEXT("}"),
        *GenClassPath);

    FBpirCompiler Compiler(HostBP);
    FCompileResult Result = Compiler.Compile(BpirBody);

    if (!Result.bSuccess)
    {
        for (const FCompileError& Err : Result.Errors)
        {
            AddError(FString::Printf(TEXT("BPIR compile error  L%d: %s"), Err.Line, *Err.Message));
        }
    }

    TestTrue(TEXT("BPIR compiled successfully (set after K2Node_CreateWidget)"), Result.bSuccess);
    TestEqual(TEXT("No compile errors"), Result.Errors.Num(), 0);

    return true;
}
