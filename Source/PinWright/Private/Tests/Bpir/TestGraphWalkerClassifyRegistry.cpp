// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression tests for the FGraphWalker::ClassifyNode TMap<UClass*, ENodeSemantics>
// registry refactor (board ticket E-bpir-classify-node-registry).
//
// The refactor moved the bulk of the per-K2Node-class dispatch out of a manual
// if-chain into a class -> semantics registry walked via GetSuperClass().
//
// Two invariants this test pins down:
//   1. UK2Node_MacroInstance must be matched BEFORE UK2Node_Tunnel even though
//      MacroInstance derives from Tunnel. The pre-registry imperative branch in
//      ClassifyNode enforces this. If it is removed, the registry walk lands on
//      UK2Node_Tunnel via the inheritance chain and returns TunnelEntry instead
//      of the macro-name-derived semantic (e.g. ForEach for ForEachLoop).
//   2. The registry walk must traverse GetSuperClass() so subclasses inherit
//      their base-class entry. UK2Node_SwitchInteger derives from UK2Node_Switch
//      but is not registered directly; the walk has to resolve to Switch via
//      the parent. Replacing the walk with a flat Map.Find(GetClass()) breaks
//      this assertion.

#include "Misc/AutomationTest.h"
#include "BpirGraphTestHelpers.h"

#include "Decompiler/GraphWalker.h"
#include "Decompiler/DecompilerTypes.h"

#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "GameFramework/Actor.h"
#include "Kismet2/KismetEditorUtilities.h"

#include "EdGraph/EdGraph.h"
#include "K2Node_IfThenElse.h"
#include "K2Node_ExecutionSequence.h"
#include "K2Node_DynamicCast.h"
#include "K2Node_Switch.h"
#include "K2Node_SwitchInteger.h"
#include "K2Node_FunctionResult.h"
#include "K2Node_VariableSet.h"
#include "K2Node_VariableGet.h"
#include "K2Node_Knot.h"
#include "K2Node_MacroInstance.h"
#include "K2Node_CallFunction.h"
#include "EdGraphNode_Comment.h"
#include "Kismet/KismetSystemLibrary.h"
#include "UObject/Package.h"

namespace
{
    // Standard transient-Blueprint factory mirroring the helper used by other
    // tests in this folder.
    UBlueprint* CreateTransientBP()
    {
        const FName Name = *FString::Printf(TEXT("TestClassifyRegistryBP_%d"), FMath::Rand());
        return FKismetEditorUtilities::CreateBlueprint(
            AActor::StaticClass(),
            GetTransientPackage(),
            Name,
            BPTYPE_Normal,
            UBlueprint::StaticClass(),
            UBlueprintGeneratedClass::StaticClass(),
            FName(TEXT("PinWrightTests")));
    }

    template<typename T>
    T* AddNode(UEdGraph* Graph)
    {
        T* Node = NewObject<T>(Graph);
        Node->CreateNewGuid();
        Node->PostPlacedNewNode();
        Node->AllocateDefaultPins();
        Graph->AddNode(Node, /*bFromUI=*/true, /*bSelectNewNode=*/false);
        return Node;
    }

    using BpirGraphTestHelpers::SpawnForEachLoopMacro;
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGraphWalkerClassifyRegistryTest,
    "PinWright.Decompiler.GraphWalker.ClassifyRegistry",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGraphWalkerClassifyRegistryTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientBP();
    if (!BP || BP->UbergraphPages.Num() == 0)
    {
        AddError(TEXT("Failed to create transient blueprint with event graph"));
        return false;
    }

    UEdGraph* Graph = BP->UbergraphPages[0];
    FGraphWalker Walker(Graph);

    // ---- Pure registry entries ----
    UK2Node_IfThenElse* Branch = AddNode<UK2Node_IfThenElse>(Graph);
    TestEqual(TEXT("UK2Node_IfThenElse -> Branch"),
        static_cast<int32>(Walker.ClassifyNode(Branch)),
        static_cast<int32>(ENodeSemantics::Branch));

    UK2Node_ExecutionSequence* Seq = AddNode<UK2Node_ExecutionSequence>(Graph);
    TestEqual(TEXT("UK2Node_ExecutionSequence -> Sequence"),
        static_cast<int32>(Walker.ClassifyNode(Seq)),
        static_cast<int32>(ENodeSemantics::Sequence));

    UK2Node_DynamicCast* Cast = AddNode<UK2Node_DynamicCast>(Graph);
    TestEqual(TEXT("UK2Node_DynamicCast -> Cast"),
        static_cast<int32>(Walker.ClassifyNode(Cast)),
        static_cast<int32>(ENodeSemantics::Cast));

    UK2Node_FunctionResult* Ret = AddNode<UK2Node_FunctionResult>(Graph);
    TestEqual(TEXT("UK2Node_FunctionResult -> Return"),
        static_cast<int32>(Walker.ClassifyNode(Ret)),
        static_cast<int32>(ENodeSemantics::Return));

    UK2Node_VariableSet* VarSet = AddNode<UK2Node_VariableSet>(Graph);
    TestEqual(TEXT("UK2Node_VariableSet -> VariableSet"),
        static_cast<int32>(Walker.ClassifyNode(VarSet)),
        static_cast<int32>(ENodeSemantics::VariableSet));

    UK2Node_VariableGet* VarGet = AddNode<UK2Node_VariableGet>(Graph);
    TestEqual(TEXT("UK2Node_VariableGet -> VariableGet"),
        static_cast<int32>(Walker.ClassifyNode(VarGet)),
        static_cast<int32>(ENodeSemantics::VariableGet));

    UK2Node_Knot* Knot = AddNode<UK2Node_Knot>(Graph);
    TestEqual(TEXT("UK2Node_Knot -> Knot"),
        static_cast<int32>(Walker.ClassifyNode(Knot)),
        static_cast<int32>(ENodeSemantics::Knot));

    UEdGraphNode_Comment* Comment = AddNode<UEdGraphNode_Comment>(Graph);
    TestEqual(TEXT("UEdGraphNode_Comment -> Comment"),
        static_cast<int32>(Walker.ClassifyNode(Comment)),
        static_cast<int32>(ENodeSemantics::Comment));

    // ---- Subclass walk: UK2Node_SwitchInteger -> UK2Node_Switch -> Switch ----
    // UK2Node_SwitchInteger is not registered directly. The registry walk has
    // to resolve it via GetSuperClass(). If the walk is replaced by a flat
    // Map.Find(GetClass()), this returns Unknown and the assertion fails.
    UK2Node_SwitchInteger* SwitchInt = AddNode<UK2Node_SwitchInteger>(Graph);
    TestEqual(TEXT("UK2Node_SwitchInteger (subclass walk) -> Switch"),
        static_cast<int32>(Walker.ClassifyNode(SwitchInt)),
        static_cast<int32>(ENodeSemantics::Switch));

    // ---- Pre-registry CallFunction arm: non-latent function -> FunctionCall ----
    UK2Node_CallFunction* PrintCall = AddNode<UK2Node_CallFunction>(Graph);
    PrintCall->FunctionReference.SetExternalMember(
        GET_FUNCTION_NAME_CHECKED(UKismetSystemLibrary, PrintString),
        UKismetSystemLibrary::StaticClass());
    PrintCall->ReconstructNode();
    TestEqual(TEXT("Non-latent UK2Node_CallFunction -> FunctionCall"),
        static_cast<int32>(Walker.ClassifyNode(PrintCall)),
        static_cast<int32>(ENodeSemantics::FunctionCall));

    // ---- Ordering invariant: MacroInstance bound to ForEachLoop -> ForEach ----
    // UK2Node_MacroInstance derives from UK2Node_Tunnel. If the explicit
    // MacroInstance pre-check is removed and dispatch falls through to the
    // registry walk + the Tunnel pre-check, the macro instance would be
    // matched as TunnelEntry/TunnelExit (or Unknown) instead of ForEach.
    UK2Node_MacroInstance* ForEach = SpawnForEachLoopMacro(Graph);
    if (ForEach)
    {
        TestEqual(TEXT("UK2Node_MacroInstance(ForEachLoop) -> ForEach (ordering)"),
            static_cast<int32>(Walker.ClassifyNode(ForEach)),
            static_cast<int32>(ENodeSemantics::ForEach));
    }
    else
    {
        AddWarning(TEXT("Unable to load /Engine StandardMacros::ForEachLoop; ordering assertion skipped"));
    }

    // ---- Null guard ----
    TestEqual(TEXT("nullptr -> Unknown"),
        static_cast<int32>(Walker.ClassifyNode(nullptr)),
        static_cast<int32>(ENodeSemantics::Unknown));
    return true;
}
