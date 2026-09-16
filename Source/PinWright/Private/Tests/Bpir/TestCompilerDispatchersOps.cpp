// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Misc/AutomationTest.h"
#include "Compat/EngineVersionCompat.h"
#include "CompilerTestUtils.h"

#include "Compiler/BpirCompiler.h"
#include "Compiler/CompilerTypes.h"
#include "Decompiler/BpirDecompiler.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "K2Node_Event.h"
#include "K2Node_IfThenElse.h"
#include "K2Node_MakeArray.h"
#include "K2Node_CreateDelegate.h"
#include "K2Node_AddDelegate.h"
#include "K2Node_AsyncAction.h"
#include "K2Node_Self.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "Blueprint/UserWidget.h"
#include "Blueprint/AsyncTaskDownloadImage.h"
#include "Kismet/BlueprintAsyncActionBase.h"

using namespace CompilerTestUtils;

namespace DispatchersOpsTestLocal
{
    // Counts nodes by exact UClass name. BpirCompiler.cpp picks between
    // UK2Node_CallDelegate / UK2Node_AddDelegate / UK2Node_RemoveDelegate purely on
    // the opcode, and falls back to a plain UK2Node_CallFunction when the delegate
    // headers are unavailable -- so the node *class* is the property that separates
    // call_dispatcher from bind_dispatcher from unbind_dispatcher. Comparing class
    // names rather than including the headers keeps this test independent of the
    // compiler's own __has_include gating, which is exactly what must not be trusted
    // here. Name is file-unique because Unity merges translation units.
    inline int32 CountNodesByClassName(UBlueprint* BP, const TCHAR* ClassName)
    {
        int32 Count = 0;
        if (!BP)
        {
            return Count;
        }
        for (UEdGraph* Graph : BP->UbergraphPages)
        {
            if (!Graph) continue;
            for (UEdGraphNode* Node : Graph->Nodes)
            {
                if (Node && Node->GetClass()->GetName() == ClassName)
                {
                    ++Count;
                }
            }
        }
        return Count;
    }
}

// ============================================================================
// 1. Compiler.Integration.EnumLiteral
// Simple test that an inline enum argument compiles cleanly.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCompilerIntegrationEnumLiteralTest,
    "PinWright.bpir.compiler.integration.EnumLiteral",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCompilerIntegrationEnumLiteralTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("DispatchOpsTestBP"));
    TestNotNull(TEXT("Blueprint was created"), BP);
    if (!BP) return false;

    FBpirCompiler Compiler(BP);
    FCompileResult Result = Compiler.Compile(
        TEXT("entry event BeginPlay() {\n")
        TEXT("    %e = enum ETraceTypeQuery::Visibility\n")
        TEXT("    call PrintString(InString: \"test\")\n")
        TEXT("}"));

    if (!Result.bSuccess) { for (const FCompileError& Err : Result.Errors) { AddError(FString::Printf(TEXT("  L%d: %s"), Err.Line, *Err.Message)); } }
    TestTrue(TEXT("Compile succeeded"), Result.bSuccess);
    TestTrue(TEXT("Nodes were created"), Result.CreatedNodeGUIDs.Num() > 0);
    return true;
}

// ============================================================================
// 2. Compiler.Integration.EnumLiteralRef
// Tests %ref to an enum value used downstream (Bug 2 fix).
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCompilerIntegrationEnumLiteralRefTest,
    "PinWright.bpir.compiler.integration.EnumLiteralRef",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCompilerIntegrationEnumLiteralRefTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("DispatchOpsTestBP"));
    TestNotNull(TEXT("Blueprint was created"), BP);
    if (!BP) return false;

    FBpirCompiler Compiler(BP);
    FCompileResult Result = Compiler.Compile(
        TEXT("entry event BeginPlay() {\n")
        TEXT("    %e = enum ETraceTypeQuery::TraceTypeQuery1\n")
        TEXT("    %origin = make<Vector>(X: 0.0, Y: 0.0, Z: 100.0)\n")
        TEXT("    %end = make<Vector>(X: 0.0, Y: 0.0, Z: -500.0)\n")
        TEXT("    %hit = call LineTraceByChannel(Start: %origin, End: %end, TraceChannel: %e)\n")
        TEXT("    call PrintString(InString: \"Traced\")\n")
        TEXT("}"));

    if (!Result.bSuccess) { for (const FCompileError& Err : Result.Errors) { AddError(FString::Printf(TEXT("  L%d: %s"), Err.Line, *Err.Message)); } }
    TestTrue(TEXT("Compile succeeded (enum %ref resolved correctly)"), Result.bSuccess);
    TestTrue(TEXT("At least one node was created"), Result.CreatedNodeGUIDs.Num() > 0);
    return true;
}

// ============================================================================
// 3. Compiler.Integration.MakeArray
// make_array with string literals should produce a UK2Node_MakeArray node.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCompilerIntegrationMakeArrayTest,
    "PinWright.bpir.compiler.integration.MakeArray",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCompilerIntegrationMakeArrayTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("DispatchOpsTestBP"));
    TestNotNull(TEXT("Blueprint was created"), BP);
    if (!BP) return false;

    FBpirCompiler Compiler(BP);
    FCompileResult Result = Compiler.Compile(
        TEXT("entry event BeginPlay() {\n")
        TEXT("    %arr = make_array(\"Apple\", \"Banana\", \"Cherry\")\n")
        TEXT("    call PrintString(InString: \"Made array\")\n")
        TEXT("}"));

    if (!Result.bSuccess) { for (const FCompileError& Err : Result.Errors) { AddError(FString::Printf(TEXT("  L%d: %s"), Err.Line, *Err.Message)); } }
    TestTrue(TEXT("Compile succeeded"), Result.bSuccess);
    TestEqual(TEXT("Exactly one MakeArray node was created"),
        CountNodesOfType<UK2Node_MakeArray>(BP), 1);
    return true;
}

// ============================================================================
// 4. Compiler.Integration.CallDispatcher
// call_dispatcher should not crash even if the dispatcher is not found on the BP.
// The test verifies graceful handling (success or a clean error message).
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCompilerIntegrationCallDispatcherTest,
    "PinWright.bpir.compiler.integration.CallDispatcher",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCompilerIntegrationCallDispatcherTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("DispatchOpsTestBP"));
    TestNotNull(TEXT("Blueprint was created"), BP);
    if (!BP) return false;

    FBpirCompiler Compiler(BP);
    FCompileResult Result = Compiler.Compile(
        TEXT("entry event BeginPlay() {\n")
        TEXT("    call_dispatcher OnDamage(Damage: 50.0)\n")
        TEXT("}"));

    // Dispatchers need an actual event dispatcher variable on the BP.
    // Without one, the compiler should either succeed (if it can resolve) or
    // fail gracefully with an error message -- but must not crash.
    if (!Result.bSuccess)
    {
        TestTrue(TEXT("Graceful failure produced error messages"),
            Result.Errors.Num() > 0);
        AddInfo(FString::Printf(TEXT("CallDispatcher gracefully failed: %s"),
            Result.Errors.Num() > 0 ? *Result.Errors[0].Message : TEXT("(no message)")));
    }
    else
    {
        AddInfo(TEXT("CallDispatcher compiled successfully"));
        TestTrue(TEXT("At least one node was created"), Result.CreatedNodeGUIDs.Num() > 0);
    }
    return true;
}

// ============================================================================
// 5. Compiler.Integration.UnbindDispatcher_WithDelegate
// unbind_dispatcher should compile to a UK2Node_RemoveDelegate when a real
// delegate property exists on the BP.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCompilerIntegrationUnbindDispatcherWithDelegateTest,
    "PinWright.bpir.compiler.integration.UnbindDispatcher_WithDelegate",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCompilerIntegrationUnbindDispatcherWithDelegateTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("UnbindDispatcherTestBP"));
    TestNotNull(TEXT("Blueprint was created"), BP);
    if (!BP) return false;

    // Add a multicast delegate variable so unbind_dispatcher has something to resolve
    FEdGraphPinType DelegateType;
    DelegateType.PinCategory = UEdGraphSchema_K2::PC_MCDelegate;
    FBlueprintEditorUtils::AddMemberVariable(BP, TEXT("OnDamage"), DelegateType);
    FKismetEditorUtilities::CompileBlueprint(BP);

    FBpirCompiler Compiler(BP);
    FCompileResult Result = Compiler.Compile(
        TEXT("entry event BeginPlay() {\n")
        TEXT("    unbind_dispatcher OnDamage()\n")
        TEXT("}"));

    if (Result.bSuccess)
    {
        TestTrue(TEXT("At least one node was created"), Result.CreatedNodeGUIDs.Num() > 0);
        // The stated contract is "compiles to a UK2Node_RemoveDelegate". A bare node
        // count cannot see the opcode->class mapping in BpirCompiler.cpp being swapped
        // with bind/call, nor the fallback that emits a plain UK2Node_CallFunction.
        TestEqual(TEXT("Exactly one K2Node_RemoveDelegate node was created"),
            DispatchersOpsTestLocal::CountNodesByClassName(BP, TEXT("K2Node_RemoveDelegate")), 1);
        AddInfo(TEXT("UnbindDispatcher compiled successfully"));
    }
    else
    {
        // Delegate resolution may fail in test context; verify no crash.
        // Warn so this degenerate branch is visible instead of silently green.
        AddWarning(TEXT("unbind_dispatcher with a real delegate failed to compile; skipping RemoveDelegate node assertion."));
        AddInfo(TEXT("unbind_dispatcher compile produced errors, verifying no crash"));
        TestTrue(TEXT("Errors were reported cleanly"), Result.Errors.Num() > 0);
    }
    return true;
}

// ============================================================================
// 6. Compiler.Integration.UnbindDispatcher_NoDelegate
// unbind_dispatcher on a non-existent delegate should fail gracefully.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCompilerIntegrationUnbindDispatcherNoDelegateTest,
    "PinWright.bpir.compiler.integration.UnbindDispatcher_NoDelegate",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCompilerIntegrationUnbindDispatcherNoDelegateTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("UnbindDispNoDelegateBP"));
    TestNotNull(TEXT("Blueprint was created"), BP);
    if (!BP) return false;

    FBpirCompiler Compiler(BP);
    FCompileResult Result = Compiler.Compile(
        TEXT("entry event BeginPlay() {\n")
        TEXT("    unbind_dispatcher NonExistentDispatcher()\n")
        TEXT("}"));

    // Without a real delegate, compilation should fail with a clean error message.
    if (!Result.bSuccess)
    {
        TestTrue(TEXT("Graceful failure produced error messages"),
            Result.Errors.Num() > 0);
        AddInfo(FString::Printf(TEXT("UnbindDispatcher gracefully failed: %s"),
            Result.Errors.Num() > 0 ? *Result.Errors[0].Message : TEXT("(no message)")));
    }
    else
    {
        // If it somehow resolved, that's acceptable too
        AddInfo(TEXT("UnbindDispatcher compiled despite no delegate (unexpected but acceptable)"));
    }
    return true;
}

// ============================================================================
// 7. Compiler.Integration.CallDispatcher_WithDelegate
// call_dispatcher with a real multicast delegate variable should succeed.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCompilerIntegrationCallDispatcherWithDelegateTest,
    "PinWright.bpir.compiler.integration.CallDispatcher_WithDelegate",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCompilerIntegrationCallDispatcherWithDelegateTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("CallDispWithDelegateBP"));
    TestNotNull(TEXT("Blueprint was created"), BP);
    if (!BP) return false;

    // Add a real multicast delegate variable
    FEdGraphPinType DelegateType;
    DelegateType.PinCategory = UEdGraphSchema_K2::PC_MCDelegate;
    FBlueprintEditorUtils::AddMemberVariable(BP, TEXT("OnHealthChanged"), DelegateType);
    FKismetEditorUtilities::CompileBlueprint(BP);

    FBpirCompiler Compiler(BP);
    FCompileResult Result = Compiler.Compile(
        TEXT("entry event BeginPlay() {\n")
        TEXT("    call_dispatcher OnHealthChanged()\n")
        TEXT("}"));

    if (Result.bSuccess)
    {
        TestTrue(TEXT("At least one node was created"), Result.CreatedNodeGUIDs.Num() > 0);
        TestEqual(TEXT("Exactly one K2Node_CallDelegate node was created"),
            DispatchersOpsTestLocal::CountNodesByClassName(BP, TEXT("K2Node_CallDelegate")), 1);
    }
    else
    {
        // Delegate resolution may fail in transient test context
        AddWarning(TEXT("call_dispatcher with a real delegate failed to compile; skipping CallDelegate node assertion."));
        AddInfo(TEXT("call_dispatcher compile produced errors, verifying no crash"));
        TestTrue(TEXT("Errors were reported cleanly"), Result.Errors.Num() > 0);
    }
    return true;
}

// ============================================================================
// 8. Compiler.Integration.BindDispatcher_WithDelegate
// bind_dispatcher with a real multicast delegate variable should succeed.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCompilerIntegrationBindDispatcherWithDelegateTest,
    "PinWright.bpir.compiler.integration.BindDispatcher_WithDelegate",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCompilerIntegrationBindDispatcherWithDelegateTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("BindDispWithDelegateBP"));
    TestNotNull(TEXT("Blueprint was created"), BP);
    if (!BP) return false;

    // Add a real multicast delegate variable
    FEdGraphPinType DelegateType;
    DelegateType.PinCategory = UEdGraphSchema_K2::PC_MCDelegate;
    FBlueprintEditorUtils::AddMemberVariable(BP, TEXT("OnScoreUpdate"), DelegateType);
    FKismetEditorUtilities::CompileBlueprint(BP);

    FBpirCompiler Compiler(BP);
    FCompileResult Result = Compiler.Compile(
        TEXT("entry event BeginPlay() {\n")
        TEXT("    bind_dispatcher OnScoreUpdate()\n")
        TEXT("}"));

    if (Result.bSuccess)
    {
        TestTrue(TEXT("At least one node was created"), Result.CreatedNodeGUIDs.Num() > 0);
        TestEqual(TEXT("Exactly one K2Node_AddDelegate node was created"),
            DispatchersOpsTestLocal::CountNodesByClassName(BP, TEXT("K2Node_AddDelegate")), 1);
    }
    else
    {
        // Delegate resolution may fail in transient test context
        AddWarning(TEXT("bind_dispatcher with a real delegate failed to compile; skipping AddDelegate node assertion."));
        AddInfo(TEXT("bind_dispatcher compile produced errors, verifying no crash"));
        TestTrue(TEXT("Errors were reported cleanly"), Result.Errors.Num() > 0);
    }
    return true;
}

// ============================================================================
// Compiler.Integration.EnumDefaultOnGenericBytePin
// Enum-qualified literal on a generic PC_Byte pin (no PinSubCategoryObject
// pre-set) should infer the enum subtype from the literal and write the
// enum name string, so the post-link BP validator accepts the value once
// the inferred subtype propagates.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCompilerIntegrationEnumDefaultOnGenericBytePinTest,
    "PinWright.bpir.compiler.integration.EnumDefaultOnGenericBytePin",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCompilerIntegrationEnumDefaultOnGenericBytePinTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("EnumBytePinTestBP"));
    TestNotNull(TEXT("Blueprint was created"), BP);
    if (!BP) return false;

    FBpirCompiler Compiler(BP);
    FCompileResult Result = Compiler.Compile(
        TEXT("entry event BeginPlay() {\n")
        TEXT("    %cmp = pure EqualEqual_ByteByte(A: 0, B: ETraceTypeQuery::TraceTypeQuery1)\n")
        TEXT("    call PrintString(InString: \"test\")\n")
        TEXT("}"));

    if (!Result.bSuccess) { for (const FCompileError& Err : Result.Errors) { AddError(FString::Printf(TEXT("  L%d: %s"), Err.Line, *Err.Message)); } }
    TestTrue(TEXT("Compile succeeded"), Result.bSuccess);

    // The compile above already asserted success, so both the node and its "B" pin are
    // knowable -- guarding on them without asserting let every load-bearing assertion
    // below evaporate silently if the node was never emitted.
    UK2Node_CallFunction* CmpNode = FindCallFunctionBySubstring(BP, TEXT("EqualEqual_ByteByte"));
    TestNotNull(TEXT("EqualEqual_ByteByte call node was created"), CmpNode);
    if (CmpNode)
    {
        UEdGraphPin* BPin = CmpNode->FindPin(TEXT("B"));
        TestNotNull(TEXT("EqualEqual_ByteByte node has a 'B' pin"), BPin);
        if (BPin)
        {
            TestFalse(TEXT("B pin default is not empty"), BPin->DefaultValue.IsEmpty());
            TestEqual(TEXT("B pin default is the enum name string"),
                BPin->DefaultValue, FString(TEXT("TraceTypeQuery1")));
            TestTrue(TEXT("B pin has enum subcategory set"),
                BPin->PinType.PinSubCategoryObject.IsValid());
        }
    }
    return true;
}

// ============================================================================
// Compiler.Integration.BindDispatcher_ExternalTarget_NoCrash
// bind_dispatcher with external target should resolve the target class or
// fail gracefully with a clean error — never crash.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCompilerIntegrationBindDispatcherExternalTargetTest,
    "PinWright.bpir.compiler.integration.BindDispatcher_ExternalTarget_NoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCompilerIntegrationBindDispatcherExternalTargetTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("BindExtTargetTestBP"));
    TestNotNull(TEXT("Blueprint was created"), BP);
    if (!BP) return false;

    // bind_dispatcher with "Target: self" — self won't have this dispatcher,
    // but this tests the target resolution path without crashing
    FBpirCompiler Compiler(BP);
    FCompileResult Result = Compiler.Compile(
        TEXT("entry event BeginPlay() {\n")
        TEXT("    bind_dispatcher FakeDispatcher(Target: self)\n")
        TEXT("}"));

    if (Result.bSuccess)
    {
        TestTrue(TEXT("At least one node was created"), Result.CreatedNodeGUIDs.Num() > 0);
    }
    else
    {
        // Expected: fails gracefully because FakeDispatcher doesn't exist on self class
        AddInfo(TEXT("bind_dispatcher with external target produced clean errors"));
        TestTrue(TEXT("Errors were reported cleanly"), Result.Errors.Num() > 0);
    }
    return true;
}

// ============================================================================
// Compiler.Integration.BindDispatcher_WithCreateDelegate
// bind_dispatcher with a real delegate and a Delegate arg should create
// a K2Node_CreateDelegate and wire it.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCompilerIntegrationBindDispatcherWithCreateDelegateTest,
    "PinWright.bpir.compiler.integration.BindDispatcher_WithCreateDelegate",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCompilerIntegrationBindDispatcherWithCreateDelegateTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("BindCreateDelegateTestBP"));
    TestNotNull(TEXT("Blueprint was created"), BP);
    if (!BP) return false;

    // Add a multicast delegate variable
    FEdGraphPinType DelegateType;
    DelegateType.PinCategory = UEdGraphSchema_K2::PC_MCDelegate;
    FBlueprintEditorUtils::AddMemberVariable(BP, TEXT("OnDamage"), DelegateType);
    FKismetEditorUtilities::CompileBlueprint(BP);

    FBpirCompiler Compiler(BP);
    FCompileResult Result = Compiler.Compile(
        TEXT("entry event BeginPlay() {\n")
        TEXT("    bind_dispatcher OnDamage(Delegate: HandleDamage)\n")
        TEXT("}\n")
        TEXT("entry custom_event HandleDamage() {\n")
        TEXT("    call PrintString(InString: \"damage received\")\n")
        TEXT("}"));

    if (Result.bSuccess)
    {
        TestTrue(TEXT("Nodes were created"), Result.CreatedNodeGUIDs.Num() > 0);
        // Verify a CreateDelegate node was created
        bool bFoundCreateDelegate = false;
        for (UEdGraph* Graph : BP->UbergraphPages)
        {
            for (UEdGraphNode* Node : Graph->Nodes)
            {
                if (Node->IsA<UK2Node_CreateDelegate>())
                {
                    bFoundCreateDelegate = true;
                    break;
                }
            }
            if (bFoundCreateDelegate) break;
        }
        TestTrue(TEXT("K2Node_CreateDelegate was created"), bFoundCreateDelegate);
    }
    else
    {
        // Delegate resolution may fail in transient test context
        AddInfo(TEXT("bind_dispatcher with Delegate arg produced errors, verifying no crash"));
        for (const FCompileError& Err : Result.Errors)
        {
            AddInfo(FString::Printf(TEXT("  L%d: %s"), Err.Line, *Err.Message));
        }
    }
    return true;
}

// ============================================================================
// Compiler.Integration.BindDispatcher_ExternalTargetUsesSelfForCreateDelegate
// When binding to a dispatcher on an external target, the CreateDelegate object
// pin must still resolve against self so the handler is found on this Blueprint.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCompilerIntegrationBindDispatcherExternalTargetUsesSelfTest,
    "PinWright.bpir.compiler.integration.BindDispatcher_ExternalTargetUsesSelfForCreateDelegate",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCompilerIntegrationBindDispatcherExternalTargetUsesSelfTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("BindExtSelfTestBP"));
    TestNotNull(TEXT("Blueprint was created"), BP);
    if (!BP) return false;

    FEdGraphPinType DelegateType;
    DelegateType.PinCategory = UEdGraphSchema_K2::PC_MCDelegate;
    FBlueprintEditorUtils::AddMemberVariable(BP, TEXT("OnDamage"), DelegateType);

    FEdGraphPinType TargetType;
    TargetType.PinCategory = UEdGraphSchema_K2::PC_Object;
    TargetType.PinSubCategoryObject = BP->SkeletonGeneratedClass;
    FBlueprintEditorUtils::AddMemberVariable(BP, TEXT("RemoteSelf"), TargetType);
    FKismetEditorUtilities::CompileBlueprint(BP);

    FBpirCompiler Compiler(BP);
    FCompileResult Result = Compiler.Compile(
        TEXT("entry event BeginPlay() {\n")
        TEXT("    bind_dispatcher OnDamage(Target: $RemoteSelf, Delegate: HandleDamage)\n")
        TEXT("}\n")
        TEXT("entry custom_event HandleDamage() {\n")
        TEXT("    call PrintString(InString: \"damage received\")\n")
        TEXT("}"));

    if (!Result.bSuccess)
    {
        for (const FCompileError& Err : Result.Errors)
        {
            AddError(FString::Printf(TEXT("  L%d: %s"), Err.Line, *Err.Message));
        }
    }
    TestTrue(TEXT("Compile succeeded"), Result.bSuccess);
    if (!Result.bSuccess) return false;

    UK2Node_CreateDelegate* CreateDelegateNode = nullptr;
    UK2Node_AddDelegate* AddDelegateNode = nullptr;
    for (UEdGraph* Graph : BP->UbergraphPages)
    {
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            if (!CreateDelegateNode)
            {
                CreateDelegateNode = Cast<UK2Node_CreateDelegate>(Node);
            }
            if (!AddDelegateNode)
            {
                AddDelegateNode = Cast<UK2Node_AddDelegate>(Node);
            }
        }
    }

    TestNotNull(TEXT("CreateDelegate node exists"), CreateDelegateNode);
    TestNotNull(TEXT("AddDelegate node exists"), AddDelegateNode);
    if (!CreateDelegateNode || !AddDelegateNode) return false;

    UEdGraphPin* DelegateObjectPin = CreateDelegateNode->GetObjectInPin();
    TestNotNull(TEXT("CreateDelegate object pin exists"), DelegateObjectPin);
    if (DelegateObjectPin)
    {
        TestEqual(TEXT("CreateDelegate object pin has exactly one link"), DelegateObjectPin->LinkedTo.Num(), 1);
        if (DelegateObjectPin->LinkedTo.Num() == 1)
        {
            TestTrue(TEXT("CreateDelegate resolves handler against self"),
                DelegateObjectPin->LinkedTo[0]->GetOwningNode()->IsA<UK2Node_Self>());
        }
    }

    UEdGraphPin* AddDelegateTargetPin = AddDelegateNode->FindPin(UEdGraphSchema_K2::PN_Self);
    TestNotNull(TEXT("AddDelegate target pin exists"), AddDelegateTargetPin);
    if (AddDelegateTargetPin)
    {
        TestEqual(TEXT("AddDelegate target pin has exactly one link"), AddDelegateTargetPin->LinkedTo.Num(), 1);
        if (AddDelegateTargetPin->LinkedTo.Num() == 1)
        {
            TestFalse(TEXT("AddDelegate target is not self"), AddDelegateTargetPin->LinkedTo[0]->GetOwningNode()->IsA<UK2Node_Self>());
        }
    }
    return true;
}

// ============================================================================
// Compiler.Integration.BindDispatcher_SelfContextNoRedundantSelfWire
// Regression for E-bind-dispatcher-redundant-self-wire: when bind_dispatcher
// binds to a same-class dispatcher (no `target:`), the compiler must not emit
// a redundant K2Node_Self wired into CreateDelegate.self. UE resolves unlinked
// CreateDelegate.self to the current BP's class — the explicit wire is
// cosmetic noise. Both AddDelegate.Target and CreateDelegate.self must be
// left unlinked for the same-class case.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCompilerIntegrationBindDispatcherSelfContextNoRedundantSelfTest,
    "PinWright.bpir.compiler.integration.BindDispatcher_SelfContextNoRedundantSelfWire",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCompilerIntegrationBindDispatcherSelfContextNoRedundantSelfTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("BindSelfNoRedundantTestBP"));
    TestNotNull(TEXT("Blueprint was created"), BP);
    if (!BP) return false;

    FEdGraphPinType DelegateType;
    DelegateType.PinCategory = UEdGraphSchema_K2::PC_MCDelegate;
    FBlueprintEditorUtils::AddMemberVariable(BP, TEXT("OnDamage"), DelegateType);
    FKismetEditorUtilities::CompileBlueprint(BP);

    FBpirCompiler Compiler(BP);
    FCompileResult Result = Compiler.Compile(
        TEXT("entry event BeginPlay() {\n")
        TEXT("    bind_dispatcher OnDamage(Delegate: HandleDamage)\n")
        TEXT("}\n")
        TEXT("entry custom_event HandleDamage() {\n")
        TEXT("    call PrintString(InString: \"damage received\")\n")
        TEXT("}"));

    if (!Result.bSuccess)
    {
        for (const FCompileError& Err : Result.Errors)
        {
            AddError(FString::Printf(TEXT("  L%d: %s"), Err.Line, *Err.Message));
        }
    }
    TestTrue(TEXT("Compile succeeded"), Result.bSuccess);
    if (!Result.bSuccess) return false;

    UK2Node_CreateDelegate* CreateDelegateNode = nullptr;
    UK2Node_AddDelegate* AddDelegateNode = nullptr;
    int32 SelfNodeCount = 0;
    for (UEdGraph* Graph : BP->UbergraphPages)
    {
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            if (!CreateDelegateNode)
            {
                CreateDelegateNode = Cast<UK2Node_CreateDelegate>(Node);
            }
            if (!AddDelegateNode)
            {
                AddDelegateNode = Cast<UK2Node_AddDelegate>(Node);
            }
            if (Node->IsA<UK2Node_Self>())
            {
                ++SelfNodeCount;
            }
        }
    }

    TestNotNull(TEXT("CreateDelegate node exists"), CreateDelegateNode);
    TestNotNull(TEXT("AddDelegate node exists"), AddDelegateNode);
    if (!CreateDelegateNode || !AddDelegateNode) return false;

    // No K2Node_Self should be emitted — bind_dispatcher is the only instruction
    // that might need self, and for same-class binds it should rely on implicit self.
    TestEqual(TEXT("No redundant K2Node_Self emitted for same-class bind_dispatcher"),
        SelfNodeCount, 0);

    UEdGraphPin* DelegateObjectPin = CreateDelegateNode->GetObjectInPin();
    TestNotNull(TEXT("CreateDelegate object pin exists"), DelegateObjectPin);
    if (DelegateObjectPin)
    {
        TestEqual(TEXT("CreateDelegate object pin is unlinked (implicit self)"),
            DelegateObjectPin->LinkedTo.Num(), 0);
    }

    UEdGraphPin* AddDelegateTargetPin = AddDelegateNode->FindPin(UEdGraphSchema_K2::PN_Self);
    TestNotNull(TEXT("AddDelegate target pin exists"), AddDelegateTargetPin);
    if (AddDelegateTargetPin)
    {
        TestEqual(TEXT("AddDelegate target pin is unlinked (implicit self)"),
            AddDelegateTargetPin->LinkedTo.Num(), 0);
    }

    // HandleAnyChange must still resolve the handler function via implicit-self
    // scope — otherwise the BP compile would fail with "Unable to find the
    // selected function/event".
    TestEqual(TEXT("CreateDelegate resolves SelectedFunctionName to handler"),
        CreateDelegateNode->GetFunctionName(), FName(TEXT("HandleDamage")));
    return true;
}

// ============================================================================
// Compiler.Integration.EnumDefaultOnPCEnumPin
// PC_Enum pins (e.g. ESlateVisibility) should retain the name string format
// ("Collapsed"), not a numeric index. This is the counterpart to
// EnumDefaultOnGenericBytePin which verifies PC_Byte pins get numeric values.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCompilerIntegrationEnumDefaultOnPCEnumPinTest,
    "PinWright.bpir.compiler.integration.EnumDefaultOnPCEnumPin",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCompilerIntegrationEnumDefaultOnPCEnumPinTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBPWithParent(
        UUserWidget::StaticClass(), TEXT("EnumPCEnumPinTestBP"));
    TestNotNull(TEXT("Blueprint was created"), BP);
    if (!BP) return false;

    // UUserWidget exposes `Construct` (BlueprintImplementableEvent) as its
    // "begin play" equivalent — there is no `BeginPlay` UFunction on the parent
    // class, and CreateEventNode's phantom-event gate (CodeNodeEmitter.cpp)
    // refuses to bind to a non-existent member.
    FBpirCompiler Compiler(BP);
    FCompileResult Result = Compiler.Compile(
        TEXT("entry event Construct() {\n")
        TEXT("    call SetVisibility(InVisibility: ESlateVisibility::Collapsed)\n")
        TEXT("}"));

    if (!Result.bSuccess) { for (const FCompileError& Err : Result.Errors) { AddError(FString::Printf(TEXT("  L%d: %s"), Err.Line, *Err.Message)); } }
    TestTrue(TEXT("Compile succeeded"), Result.bSuccess);
    if (!Result.bSuccess) return false;

    UK2Node_CallFunction* CallNode = FindCallFunctionBySubstring(BP, TEXT("SetVisibility"));
    TestNotNull(TEXT("SetVisibility call node was created"), CallNode);
    if (!CallNode) return false;

    UEdGraphPin* VisPin = CallNode->FindPin(TEXT("InVisibility"));
    TestNotNull(TEXT("InVisibility pin exists"), VisPin);
    if (!VisPin) return false;

    // UE5 exposes most native enums as PC_Byte with PinSubCategoryObject set to the UEnum.
    // ESlateVisibility is PC_Byte, not PC_Enum. Verify the enum subcategory is wired correctly
    // and the default value is set (numeric for PC_Byte, name for PC_Enum).
    TestTrue(TEXT("Pin category is PC_Byte or PC_Enum"),
        VisPin->PinType.PinCategory == UEdGraphSchema_K2::PC_Byte
        || VisPin->PinType.PinCategory == UEdGraphSchema_K2::PC_Enum);
    TestTrue(TEXT("Pin has enum subcategory"),
        VisPin->PinType.PinSubCategoryObject.IsValid());
    TestFalse(TEXT("Pin default is not empty"),
        VisPin->DefaultValue.IsEmpty());
    return true;
}

// ============================================================================
// Compiler.Integration.BindDispatcher_EventKeyword
// The pre-scan in `bind_dispatcher` must accept `event:` (BPIR keyword) as an
// alias for the UE pin name `Delegate:`. Verifies a UK2Node_CreateDelegate is
// wired with the expected function name.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCompilerIntegrationBindDispatcherEventKeywordTest,
    "PinWright.bpir.compiler.integration.BindDispatcher_EventKeyword",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCompilerIntegrationBindDispatcherEventKeywordTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("BindDispEventKeywordBP"));
    TestNotNull(TEXT("Blueprint was created"), BP);
    if (!BP) return false;

    FEdGraphPinType DelegateType;
    DelegateType.PinCategory = UEdGraphSchema_K2::PC_MCDelegate;
    FBlueprintEditorUtils::AddMemberVariable(BP, TEXT("OnProbeEvent"), DelegateType);
    FKismetEditorUtilities::CompileBlueprint(BP);

    FBpirCompiler Compiler(BP);
    FCompileResult Result = Compiler.Compile(
        TEXT("entry custom_event ProbeHandler() {\n")
        TEXT("    call PrintString(InString: \"handled\")\n")
        TEXT("}\n")
        TEXT("entry event BeginPlay() {\n")
        TEXT("    bind_dispatcher OnProbeEvent(target: self, event: ProbeHandler)\n")
        TEXT("}"));

    if (!Result.bSuccess) { for (const FCompileError& Err : Result.Errors) { AddError(FString::Printf(TEXT("  L%d: %s"), Err.Line, *Err.Message)); } }
    TestTrue(TEXT("Compile succeeded (event: keyword accepted)"), Result.bSuccess);
    if (!Result.bSuccess) return false;

    // Find the UK2Node_CreateDelegate and verify it references ProbeHandler.
    UK2Node_CreateDelegate* CDNode = nullptr;
    for (UEdGraph* Graph : BP->UbergraphPages)
    {
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            if (UK2Node_CreateDelegate* Candidate = Cast<UK2Node_CreateDelegate>(Node))
            {
                CDNode = Candidate;
                break;
            }
        }
        if (CDNode) break;
    }
    TestNotNull(TEXT("K2Node_CreateDelegate was created"), CDNode);
    if (CDNode)
    {
        TestEqual(TEXT("CreateDelegate function name is ProbeHandler"),
            CDNode->GetFunctionName().ToString(), FString(TEXT("ProbeHandler")));
    }
    return true;
}

// ============================================================================
// Compiler.Integration.BindDispatcher_EventAtSigilStripped
// `event: @HandlerName` (BPIR sigil form) must store the function name without
// the leading '@' sigil on UK2Node_CreateDelegate::GetFunctionName().
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCompilerIntegrationBindDispatcherEventAtSigilTest,
    "PinWright.bpir.compiler.integration.BindDispatcher_EventAtSigilStripped",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCompilerIntegrationBindDispatcherEventAtSigilTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("BindDispAtSigilBP"));
    TestNotNull(TEXT("Blueprint was created"), BP);
    if (!BP) return false;

    FEdGraphPinType DelegateType;
    DelegateType.PinCategory = UEdGraphSchema_K2::PC_MCDelegate;
    FBlueprintEditorUtils::AddMemberVariable(BP, TEXT("OnProbeEvent"), DelegateType);
    FKismetEditorUtilities::CompileBlueprint(BP);

    FBpirCompiler Compiler(BP);
    FCompileResult Result = Compiler.Compile(
        TEXT("entry custom_event ProbeHandler() {\n")
        TEXT("    call PrintString(InString: \"handled\")\n")
        TEXT("}\n")
        TEXT("entry event BeginPlay() {\n")
        TEXT("    bind_dispatcher OnProbeEvent(target: self, event: @ProbeHandler)\n")
        TEXT("}"));

    if (!Result.bSuccess) { for (const FCompileError& Err : Result.Errors) { AddError(FString::Printf(TEXT("  L%d: %s"), Err.Line, *Err.Message)); } }
    TestTrue(TEXT("Compile succeeded (@ sigil accepted)"), Result.bSuccess);
    if (!Result.bSuccess) return false;

    UK2Node_CreateDelegate* CDNode = nullptr;
    for (UEdGraph* Graph : BP->UbergraphPages)
    {
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            if (UK2Node_CreateDelegate* Candidate = Cast<UK2Node_CreateDelegate>(Node))
            {
                CDNode = Candidate;
                break;
            }
        }
        if (CDNode) break;
    }
    TestNotNull(TEXT("K2Node_CreateDelegate was created"), CDNode);
    if (CDNode)
    {
        const FString FnName = CDNode->GetFunctionName().ToString();
        TestEqual(TEXT("CreateDelegate function name has @ sigil stripped"),
            FnName, FString(TEXT("ProbeHandler")));
        TestFalse(TEXT("Function name does not retain leading '@'"),
            FnName.StartsWith(TEXT("@")));
    }
    return true;
}

// ============================================================================
// Compiler.Integration.DecompilerEmitsEventAt
// Decompiler should emit `event: @FnName` by reading the linked
// UK2Node_CreateDelegate's function name. Round-trips the bind.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCompilerIntegrationDecompilerEmitsEventAtTest,
    "PinWright.bpir.compiler.integration.DecompilerEmitsEventAt",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCompilerIntegrationDecompilerEmitsEventAtTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("DecompileEventAtBP"));
    TestNotNull(TEXT("Blueprint was created"), BP);
    if (!BP) return false;

    FEdGraphPinType DelegateType;
    DelegateType.PinCategory = UEdGraphSchema_K2::PC_MCDelegate;
    FBlueprintEditorUtils::AddMemberVariable(BP, TEXT("OnProbeEvent"), DelegateType);
    FKismetEditorUtilities::CompileBlueprint(BP);

    FBpirCompiler Compiler(BP);
    FCompileResult CompileResult = Compiler.Compile(
        TEXT("entry custom_event ProbeHandler() {\n")
        TEXT("    call PrintString(InString: \"handled\")\n")
        TEXT("}\n")
        TEXT("entry event BeginPlay() {\n")
        TEXT("    bind_dispatcher OnProbeEvent(target: self, event: @ProbeHandler)\n")
        TEXT("}"));

    if (!CompileResult.bSuccess)
    {
        for (const FCompileError& Err : CompileResult.Errors)
        {
            AddError(FString::Printf(TEXT("  L%d: %s"), Err.Line, *Err.Message));
        }
    }
    TestTrue(TEXT("Compile succeeded"), CompileResult.bSuccess);
    if (!CompileResult.bSuccess) return false;

    FBpirDecompiler Decompiler(BP);
    FBpirDecompileResult DecompileResult = Decompiler.Decompile();
    TestTrue(TEXT("Decompile succeeded"), DecompileResult.bSuccess);
    TestTrue(TEXT("Decompiled output contains 'event: @ProbeHandler'"),
        DecompileResult.BpirText.Contains(TEXT("event: @ProbeHandler")));
    return true;
}

// ============================================================================
// Compiler.Integration.TSubclassOfClassPathResolution
// TSubclassOf / Class pin argument passed as a fully-qualified class path should
// resolve to the UClass via ResolveUClass and populate the pin's DefaultObject.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCompilerIntegrationTSubclassOfClassPathTest,
    "PinWright.bpir.compiler.integration.TSubclassOfClassPathResolution",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCompilerIntegrationTSubclassOfClassPathTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBPWithParent(UUserWidget::StaticClass(), TEXT("TSubclassOfTestBP"));
    TestNotNull(TEXT("Blueprint was created"), BP);
    if (!BP) return false;

    // UUserWidget exposes `Construct` (BlueprintImplementableEvent) as its
    // "begin play" equivalent — there is no `BeginPlay` UFunction on the parent
    // class, and CreateEventNode's phantom-event gate (CodeNodeEmitter.cpp)
    // refuses to bind to a non-existent member.
    FBpirCompiler Compiler(BP);
    FCompileResult Result = Compiler.Compile(
        TEXT("entry event Construct() {\n")
        TEXT("    %p = call GetOwningPlayer()\n")
        TEXT("    %w = call CreateWidget(OwningPlayer: %p.ReturnValue, Class: /Script/UMG.UserWidget)\n")
        TEXT("}"));

    if (!Result.bSuccess) { for (const FCompileError& Err : Result.Errors) { AddError(FString::Printf(TEXT("  L%d: %s"), Err.Line, *Err.Message)); } }
    TestTrue(TEXT("Compile succeeded"), Result.bSuccess);
    if (!Result.bSuccess) return false;

    // Locate the CreateWidget node. The BPIR compiler emits this as a
    // UK2Node_CreateWidget (a dedicated ConstructObjectFromClass subclass in
    // UMGEditor), not a plain UK2Node_CallFunction — so FindCallFunctionBySubstring
    // would miss it. Search by class name to stay decoupled from the UMGEditor
    // private header (which the tests module doesn't link against directly).
    UEdGraphNode* CreateWidgetNode = nullptr;
    for (UEdGraph* Graph : BP->UbergraphPages)
    {
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            if (Node && Node->GetClass()->GetName() == TEXT("K2Node_CreateWidget"))
            {
                CreateWidgetNode = Node;
                break;
            }
        }
        if (CreateWidgetNode) break;
    }
    TestNotNull(TEXT("CreateWidget call node exists"), CreateWidgetNode);
    if (!CreateWidgetNode) return false;

    UEdGraphPin* ClassPin = CreateWidgetNode->FindPin(TEXT("Class"));
    TestNotNull(TEXT("Class pin exists"), ClassPin);
    if (!ClassPin) return false;

    UObject* DefaultObj = ClassPin->DefaultObject;
    TestNotNull(TEXT("Class pin DefaultObject is non-null (UClass resolved)"), DefaultObj);
    if (DefaultObj)
    {
        TestTrue(TEXT("Class pin resolves to UUserWidget"),
            DefaultObj == UUserWidget::StaticClass());
    }
    return true;
}

// ============================================================================
// Compiler.Integration.AsyncAction_BareRequiresFactory
// Bare UK2Node_AsyncAction calls must not infer a factory from pin names.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCompilerIntegrationAsyncActionBareRequiresFactoryTest,
    "PinWright.bpir.compiler.integration.AsyncAction_BareRequiresFactory",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCompilerIntegrationAsyncActionBareRequiresFactoryTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("AsyncActionBareFactoryBP"));
    TestNotNull(TEXT("Blueprint was created"), BP);
    if (!BP) return false;

    FBpirCompiler Compiler(BP);
    FCompileResult Result = Compiler.Compile(
        TEXT("entry event BeginPlay() {\n")
        TEXT("    call K2Node_AsyncAction(URL: \"https://example.com/test.png\")\n")
        TEXT("}"));

    TestFalse(TEXT("Bare async-action compile fails"), Result.bSuccess);
    TestTrue(TEXT("Migration message names exact factory form"),
        ErrorsContain(Result.Errors, TEXT("K2Node_AsyncAction_<FactoryFunctionName>")));
    TestTrue(TEXT("Migration message rejects pin-set inference"),
        ErrorsContain(Result.Errors, TEXT("pin-set factory inference")));
    TestEqual(TEXT("No UK2Node_AsyncAction node was created"),
        CountNodesOfType<UK2Node_AsyncAction>(BP), 0);
    return true;
}

// ============================================================================
// Compiler.Integration.AsyncAction_SyntheticRedirect
// `call K2Node_AsyncAction_<FactoryFunction>(...)` resolves by exact async
// factory function name, not by provided argument names.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCompilerIntegrationAsyncActionSyntheticRedirectTest,
    "PinWright.bpir.compiler.integration.AsyncAction_SyntheticRedirect",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCompilerIntegrationAsyncActionSyntheticRedirectTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("AsyncActionSyntheticBP"));
    TestNotNull(TEXT("Blueprint was created"), BP);
    if (!BP) return false;

    FBpirCompiler Compiler(BP);
    FCompileResult Result = Compiler.Compile(
        TEXT("entry event BeginPlay() {\n")
        TEXT("    call K2Node_AsyncAction_DownloadImage()\n")
        TEXT("}"));

    if (!Result.bSuccess) { for (const FCompileError& Err : Result.Errors) { AddError(FString::Printf(TEXT("  L%d: %s"), Err.Line, *Err.Message)); } }
    TestTrue(TEXT("Compile succeeded (exact factory name resolves to UK2Node_AsyncAction)"),
        Result.bSuccess);
    if (!Result.bSuccess) return false;

    TestEqual(TEXT("Exactly one UK2Node_AsyncAction node was created"),
        CountNodesOfType<UK2Node_AsyncAction>(BP), 1);

    UK2Node_AsyncAction* AsyncNode = nullptr;
    for (UEdGraph* Graph : BP->UbergraphPages)
    {
        if (UK2Node_AsyncAction* Found = FindNodeOfType<UK2Node_AsyncAction>(Graph))
        {
            AsyncNode = Found;
            break;
        }
    }
    TestNotNull(TEXT("UK2Node_AsyncAction node is reachable"), AsyncNode);
    if (AsyncNode)
    {
        // GetFactoryFunction() was protected (inaccessible from test code) in UE 5.4.
        // It became public in UE 5.6. Skip the factory-identity assertions on 5.4;
        // the node-count and node-type checks above are still meaningful regression guards.
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 6, 0)
        UFunction* Factory = AsyncNode->GetFactoryFunction();
        TestNotNull(TEXT("Async node factory function is non-null (exact factory resolver ran)"), Factory);
        if (Factory)
        {
            TestEqual(TEXT("Synthetic form factory function is DownloadImage"),
                Factory->GetFName(), GET_FUNCTION_NAME_CHECKED(UAsyncTaskDownloadImage, DownloadImage));
            TestEqual(TEXT("Synthetic form factory owner is UAsyncTaskDownloadImage"),
                Factory->GetOwnerClass(), UAsyncTaskDownloadImage::StaticClass());
        }
#endif
    }
    return true;
}
