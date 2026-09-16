// Copyright (c) 2026 Alexander Penkin. MIT License.

// TestBpirTargetClassPriority.cpp
// Regression tests for B-bpir-target-shadowed-by-self-class.
//
// Test 1 verifies Fix 1: when a call has an explicit Target: arg AND the
// function name collides with a member on the self-class hierarchy, the
// resolver picks the method on the target's class (not the self-class
// overload).
//
// Test 2 verifies Fix 2: the parser accepts `ClassName::MethodName` qualified
// syntax and the compiler resolves the method on the named class only,
// bypassing the cascade entirely.

#include "Misc/AutomationTest.h"
#include "CompilerTestUtils.h"
#include "TestBpirTargetClassPriorityFixture.h"

#include "Compiler/BpirCompiler.h"
#include "Compiler/CompilerTypes.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_CallFunction.h"
#include "Blueprint/UserWidget.h"
#include "Kismet/KismetSystemLibrary.h"

using namespace CompilerTestUtils;

// ============================================================================
// Test 1 — Target-class wins over self-class shadow
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirCallTargetClassWinsOverSelfClassShadowTest,
    "PinWright.bpir.compiler.target_class_priority.ShadowedByUUserWidget",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBpirCallTargetClassWinsOverSelfClassShadowTest::RunTest(const FString& Parameters)
{
    // Build a UUserWidget-parented transient blueprint so UUserWidget::SetPlaybackSpeed
    // (UWidgetAnimation*, float) is in the self-class function hierarchy.
    UBlueprint* BP = CreateTransientTestBPWithParent(
        UUserWidget::StaticClass(), TEXT("TargetClassPriorityShadowBP"));
    TestNotNull(TEXT("Widget blueprint was created"), BP);
    if (!BP) return false;

    // Add a member variable of the test subsystem type. BPIR can reach it via
    // $MySub, and the resolver will use the variable's class for Target-type
    // lookup.
    FEdGraphPinType SubType;
    SubType.PinCategory = UEdGraphSchema_K2::PC_Object;
    SubType.PinSubCategoryObject = UBpirTargetClassPriorityTestSubsystem::StaticClass();
    FBlueprintEditorUtils::AddMemberVariable(BP, TEXT("MySub"), SubType);

    FKismetEditorUtilities::CompileBlueprint(BP);

    // custom_event keeps the test independent of widget-specific entry kinds
    // (Construct/PreConstruct/Tick) while still exercising the cascade.
    static const TCHAR* BpirSource =
        TEXT("entry custom_event Trigger() {\n")
        TEXT("    call SetPlaybackSpeed(Target: $MySub, Speed: 0.5)\n")
        TEXT("}\n");

    FBpirCompiler Compiler(BP);
    FCompileResult Result = Compiler.Compile(BpirSource);
    TestTrue(TEXT("Compile succeeded"), Result.bSuccess);
    if (!Result.bSuccess)
    {
        for (const FCompileError& Err : Result.Errors)
        {
            AddError(FString::Printf(TEXT("Line %d: %s"), Err.Line, *Err.Message));
        }
        return false;
    }

    // Find the emitted CallFunction node for SetPlaybackSpeed.
    UK2Node_CallFunction* CallNode = FindCallFunctionBySubstring(BP, TEXT("SetPlaybackSpeed"));
    TestNotNull(TEXT("SetPlaybackSpeed CallFunction node was emitted"), CallNode);
    if (!CallNode) return false;

    const UFunction* Target = CallNode->GetTargetFunction();
    TestNotNull(TEXT("CallFunction has a bound target UFunction"), Target);
    if (!Target) return false;

    const UClass* OwningClass = Target->GetOuterUClass();
    TestEqual(
        TEXT("Bound function's owning class is the test subsystem (not the shadowing UUserWidget overload)"),
        OwningClass, static_cast<const UClass*>(UBpirTargetClassPriorityTestSubsystem::StaticClass()));
    TestNotEqual(
        TEXT("Bound function's owning class is NOT UUserWidget"),
        OwningClass, static_cast<const UClass*>(UUserWidget::StaticClass()));

    return true;
}

// ============================================================================
// Test 2 — Qualified ClassName::MethodName syntax routes to the named class
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirCallQualifiedClassMethodSyntaxTest,
    "PinWright.bpir.compiler.target_class_priority.QualifiedClassMethodSyntax",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBpirCallQualifiedClassMethodSyntaxTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("QualifiedClassMethodBP"));
    TestNotNull(TEXT("Blueprint was created"), BP);
    if (!BP) return false;

    static const TCHAR* BpirSource =
        TEXT("entry event BeginPlay() {\n")
        TEXT("    call UKismetSystemLibrary::PrintString(InString: \"X\")\n")
        TEXT("}\n");

    FBpirCompiler Compiler(BP);
    FCompileResult Result = Compiler.Compile(BpirSource);
    TestTrue(TEXT("Qualified-syntax compile succeeded"), Result.bSuccess);
    if (!Result.bSuccess)
    {
        for (const FCompileError& Err : Result.Errors)
        {
            AddError(FString::Printf(TEXT("Line %d: %s"), Err.Line, *Err.Message));
        }
        return false;
    }

    const int32 CallCount = CountNodesOfType<UK2Node_CallFunction>(BP);
    TestEqual(TEXT("Exactly one CallFunction node emitted"), CallCount, 1);

    UK2Node_CallFunction* CallNode = FindNodeOfType<UK2Node_CallFunction>(BP);
    TestNotNull(TEXT("CallFunction node located"), CallNode);
    if (!CallNode) return false;

    const UFunction* Target = CallNode->GetTargetFunction();
    TestNotNull(TEXT("CallFunction has a bound target UFunction"), Target);
    if (!Target) return false;

    TestEqual(
        TEXT("Qualified call binds to UKismetSystemLibrary::PrintString"),
        Target->GetOuterUClass(), UKismetSystemLibrary::StaticClass());

    return true;
}

// ============================================================================
// Test 2b — Qualified syntax with unresolvable class is a hard error
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirCallQualifiedSyntaxUnresolvedClassFailsTest,
    "PinWright.bpir.compiler.target_class_priority.QualifiedSyntaxUnresolvedClassFails",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBpirCallQualifiedSyntaxUnresolvedClassFailsTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("QualifiedClassUnresolvedBP"));
    TestNotNull(TEXT("Blueprint was created"), BP);
    if (!BP) return false;

    static const TCHAR* BpirSource =
        TEXT("entry event BeginPlay() {\n")
        TEXT("    call BogusClassNameForBpirTest::PrintString(InString: \"X\")\n")
        TEXT("}\n");

    FBpirCompiler Compiler(BP);
    FCompileResult Result = Compiler.Compile(BpirSource);
    TestFalse(TEXT("Compile fails when qualified class is unresolvable"), Result.bSuccess);
    TestTrue(
        TEXT("Error message mentions the unresolved class name"),
        ErrorsContain(Result.Errors, TEXT("BogusClassNameForBpirTest")));

    return true;
}
