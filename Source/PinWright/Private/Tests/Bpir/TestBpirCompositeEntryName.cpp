// Copyright (c) 2026 Alexander Penkin. MIT License.

// TestBpirCompositeEntryName.cpp
// Regression tests for entry-signature naming on composite-bearing graphs.
//
// DecompileGraphInternal flattens UK2Node_Composite nodes on a CLONE of the source
// graph (FEdGraphUtilities::CloneGraph). CloneGraph duplicates with DestName=NAME_None,
// so the clone gets an auto-generated sibling name from the per-outer unique-name
// counter ("EdGraph_N") instead of the source graph's name. The decompiler threads
// the SOURCE graph name into FBpirTextEmitter::EmitEntrySignature so function /
// override / macro entry signatures carry the real name.
//
// Counterfactual: if the source-name threading is removed (emitter falls back to
// EntryNode->GetGraph()->GetName() on the clone), every test below fails: entries
// come out as `entry function EdGraph_N(...)`, the `override` keyword is lost
// (parent-class lookup by the clone name misses), and repeated decompiles stop
// being byte-identical because each clone draws a fresh unique-name suffix.

#include "Misc/AutomationTest.h"
#include "CompilerTestUtils.h"

#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "GameFramework/Actor.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "K2Node_Composite.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_CallFunction.h"
#include "K2Node_Tunnel.h"
#include "EdGraphSchema_K2.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphPin.h"

#include "Decompiler/BpirDecompiler.h"

namespace
{
    // Spawns a UK2Node_Composite in HostGraph with one exec input and one exec output,
    // an inner PrintString ("inner") wired entry-tunnel -> print -> exit-tunnel inside
    // its BoundGraph, then wires UpstreamExecOut -> composite -> DownstreamExecIn so it
    // sits mid-chain. Same construction sequence as BuildMidGraphCompositeFixture in
    // TestBpirCompositeNamePreserved.cpp. Returns nullptr on any failure.
    UK2Node_Composite* PlantMidChainComposite(
        UEdGraph* HostGraph, UEdGraphPin* UpstreamExecOut, UEdGraphPin* DownstreamExecIn)
    {
        if (!HostGraph || !UpstreamExecOut || !DownstreamExecIn)
        {
            return nullptr;
        }

        UK2Node_Composite* Composite = NewObject<UK2Node_Composite>(HostGraph);
        Composite->CreateNewGuid();
        Composite->PostPlacedNewNode();
        HostGraph->AddNode(Composite, /*bFromUI=*/true, /*bSelectNewNode=*/false);

        if (!Composite->BoundGraph || !Composite->InputSinkNode || !Composite->OutputSourceNode)
        {
            return nullptr;
        }

        FBlueprintEditorUtils::RenameGraph(Composite->BoundGraph, TEXT("MyBlock"));

        // Exec input + exec output so the composite sits mid-chain.
        FEdGraphPinType ExecPinType;
        ExecPinType.PinCategory = UEdGraphSchema_K2::PC_Exec;
        Composite->InputSinkNode->CreateUserDefinedPin(
            UEdGraphSchema_K2::PN_Execute, ExecPinType, EGPD_Output);
        Composite->OutputSourceNode->CreateUserDefinedPin(
            UEdGraphSchema_K2::PN_Then, ExecPinType, EGPD_Input);
        Composite->ReconstructNode();

        // Inner PrintString INSIDE BoundGraph: entry tunnel -> InnerPrint -> exit tunnel.
        UK2Node_CallFunction* InnerPrint =
            CompilerTestUtils::SpawnPrintStringCall(Composite->BoundGraph, 0, 0);
        if (!InnerPrint)
        {
            return nullptr;
        }

        UEdGraphPin* EntryThen = Composite->InputSinkNode->FindPin(
            UEdGraphSchema_K2::PN_Execute, EGPD_Output);
        UEdGraphPin* InnerExec = InnerPrint->FindPin(
            UEdGraphSchema_K2::PN_Execute, EGPD_Input);
        if (EntryThen && InnerExec)
        {
            EntryThen->MakeLinkTo(InnerExec);
        }

        UEdGraphPin* InnerThen = InnerPrint->FindPin(
            UEdGraphSchema_K2::PN_Then, EGPD_Output);
        UEdGraphPin* ExitExec = Composite->OutputSourceNode->FindPin(
            UEdGraphSchema_K2::PN_Then, EGPD_Input);
        if (InnerThen && ExitExec)
        {
            InnerThen->MakeLinkTo(ExitExec);
        }

        if (UEdGraphPin* InnerStringPin = InnerPrint->FindPin(TEXT("InString"), EGPD_Input))
        {
            InnerStringPin->DefaultValue = TEXT("inner");
        }

        // Wire upstream -> composite (first exec input) and
        // composite (first exec output) -> downstream.
        UEdGraphPin* CompositeInExec = nullptr;
        UEdGraphPin* CompositeOutExec = nullptr;
        for (UEdGraphPin* Pin : Composite->Pins)
        {
            if (Pin && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
            {
                if (Pin->Direction == EGPD_Input && !CompositeInExec)
                {
                    CompositeInExec = Pin;
                }
                else if (Pin->Direction == EGPD_Output && !CompositeOutExec)
                {
                    CompositeOutExec = Pin;
                }
            }
        }
        if (!CompositeInExec || !CompositeOutExec)
        {
            return nullptr;
        }
        UpstreamExecOut->MakeLinkTo(CompositeInExec);
        CompositeOutExec->MakeLinkTo(DownstreamExecIn);

        return Composite;
    }

    // Adds a function graph named FuncName on BP and plants a mid-chain composite:
    //   <exec chain tail>.then -> Composite(inner PrintString "inner") -> OuterPrint("outer")
    //
    // SignatureClass == nullptr creates a plain user function (bIsUserCreated=true).
    // A non-null SignatureClass routes through the editor's override-graph path
    // (bIsUserCreated=false + signature class); that path also auto-spawns a
    // CallParentFunction node already wired to the function entry, so the composite is
    // planted after the existing chain tail rather than directly after the entry node.
    bool AddCompositeFunctionGraph(UBlueprint* BP, const FName& FuncName, UClass* SignatureClass)
    {
        if (!BP)
        {
            return false;
        }

        UEdGraph* FuncGraph = FBlueprintEditorUtils::CreateNewGraph(
            BP, FuncName, UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
        if (!FuncGraph)
        {
            return false;
        }

        if (SignatureClass)
        {
            FBlueprintEditorUtils::AddFunctionGraph<UClass>(
                BP, FuncGraph, /*bIsUserCreated=*/false, SignatureClass);
        }
        else
        {
            FBlueprintEditorUtils::AddFunctionGraph<UClass>(
                BP, FuncGraph, /*bIsUserCreated=*/true, nullptr);
        }

        UK2Node_FunctionEntry* EntryNode =
            CompilerTestUtils::FindNodeOfType<UK2Node_FunctionEntry>(FuncGraph);
        if (!EntryNode)
        {
            return false;
        }

        // Walk to the tail of the existing exec chain (the entry node itself for a plain
        // user function; the auto-spawned CallParentFunction node for an override graph).
        UEdGraphNode* ChainTail = EntryNode;
        while (UEdGraphNode* Next = CompilerTestUtils::GetExecDownstream(ChainTail, FString()))
        {
            ChainTail = Next;
        }
        UEdGraphPin* TailThen = ChainTail->FindPin(UEdGraphSchema_K2::PN_Then, EGPD_Output);
        if (!TailThen)
        {
            return false;
        }

        UK2Node_CallFunction* OuterPrint =
            CompilerTestUtils::SpawnPrintStringCall(FuncGraph, 600, 0);
        if (!OuterPrint)
        {
            return false;
        }
        if (UEdGraphPin* OuterStringPin = OuterPrint->FindPin(TEXT("InString"), EGPD_Input))
        {
            OuterStringPin->DefaultValue = TEXT("outer");
        }
        UEdGraphPin* OuterExec = OuterPrint->FindPin(UEdGraphSchema_K2::PN_Execute, EGPD_Input);

        return PlantMidChainComposite(FuncGraph, TailThen, OuterExec) != nullptr;
    }
} // namespace

// ============================================================================
// 1. Function entry — a composite-bearing user function named "Set Error" must
//    decompile with the SOURCE graph name (backtick-quoted for the space), not
//    the flattening clone's auto-suffixed name.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirCompositeEntryFunctionNameTest,
    "PinWright.bpir.decompiler.CompositeEntryFunctionName",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBpirCompositeEntryFunctionNameTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CompilerTestUtils::CreateTransientTestBP(TEXT("CompositeEntryFuncBP"));
    TestNotNull(TEXT("Blueprint was created"), BP);
    if (!BP) return false;

    if (!AddCompositeFunctionGraph(BP, FName(TEXT("Set Error")), /*SignatureClass=*/nullptr))
    {
        AddError(TEXT("Failed to build 'Set Error' function graph with mid-chain composite"));
        return false;
    }

    FBpirDecompiler Decompiler(BP);
    FBpirDecompileResult Result = Decompiler.Decompile();

    TestTrue(TEXT("Decompile succeeded"), Result.bSuccess);

    // Composite body must have been inlined — proves the clone+inline path actually ran
    // (a missing composite would take the no-clone fast path and pass vacuously).
    TestTrue(TEXT("BPIR contains inlined composite body (\"inner\")"),
        Result.BpirText.Contains(TEXT("\"inner\"")));

    // Entry signature carries the source graph name, backtick-quoted for the space.
    TestTrue(TEXT("BPIR contains backtick-quoted `Set Error` entry name"),
        Result.BpirText.Contains(TEXT("`Set Error`")));

    // No clone auto-suffix leakage in either naming shape.
    TestFalse(TEXT("BPIR must NOT contain auto-suffixed 'Set Error_'"),
        Result.BpirText.Contains(TEXT("Set Error_")));
    TestFalse(TEXT("BPIR must NOT contain clone name 'EdGraph_'"),
        Result.BpirText.Contains(TEXT("EdGraph_")));
    return true;
}

// ============================================================================
// 2. Determinism — decompiling the same composite-bearing Blueprint twice must
//    produce byte-identical BPIR even after the unique-name allocator state is
//    perturbed between runs. Pre-fix, each decompile's flattening clone draws a
//    fresh "EdGraph_N" suffix that leaks into the entry signature, so the two
//    outputs differ.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirCompositeEntryNameDeterminismTest,
    "PinWright.bpir.decompiler.CompositeEntryNameDeterminism",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBpirCompositeEntryNameDeterminismTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CompilerTestUtils::CreateTransientTestBP(TEXT("CompositeEntryDetermBP"));
    TestNotNull(TEXT("Blueprint was created"), BP);
    if (!BP) return false;

    if (!AddCompositeFunctionGraph(BP, FName(TEXT("Set Error")), /*SignatureClass=*/nullptr))
    {
        AddError(TEXT("Failed to build 'Set Error' function graph with mid-chain composite"));
        return false;
    }

    FBpirDecompiler FirstDecompiler(BP);
    FBpirDecompileResult First = FirstDecompiler.Decompile();
    TestTrue(TEXT("First decompile succeeded"), First.bSuccess);

    // Perturb the unique-name allocator state that names composite-flattening clones.
    // CloneGraph duplicates with DestName=NAME_None, so a clone is named by
    // MakeUniqueObjectName(BlueprintOuter, UEdGraph::StaticClass()) — a per-outer,
    // per-class suffix counter ("EdGraph_N"). Both throwaway siblings below shift the
    // suffix a fresh clone would receive, so any clone-name leakage into BPIR text
    // surfaces as a byte diff between the two decompiles:
    //  - a default-named sibling UEdGraph advances the per-outer "EdGraph_N" counter;
    //  - a sibling occupying the next "Set Error_N" slot guards the base-name-derived
    //    naming shape as well. CreateNewGraph with the literal duplicate name is not
    //    usable for this (it ensure-fails and renames the ORIGINAL graph aside,
    //    destroying the fixture), so the name is made unique against the same base
    //    first. Neither sibling is registered in FunctionGraphs, so Decompile() never
    //    visits them.
    UEdGraph* SerialBumpGraph = NewObject<UEdGraph>(BP);
    TestNotNull(TEXT("Class-serial perturbation graph created"), SerialBumpGraph);
    const FName SuffixSlotName = MakeUniqueObjectName(
        BP, UEdGraph::StaticClass(), FName(TEXT("Set Error")));
    UEdGraph* SuffixSlotGraph = FBlueprintEditorUtils::CreateNewGraph(
        BP, SuffixSlotName, UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
    TestNotNull(TEXT("Suffix-slot perturbation graph created"), SuffixSlotGraph);

    FBpirDecompiler SecondDecompiler(BP);
    FBpirDecompileResult Second = SecondDecompiler.Decompile();
    TestTrue(TEXT("Second decompile succeeded"), Second.bSuccess);

    TestTrue(TEXT("First decompile contains unsuffixed `Set Error` entry name"),
        First.BpirText.Contains(TEXT("`Set Error`")));
    TestTrue(TEXT("Second decompile contains unsuffixed `Set Error` entry name"),
        Second.BpirText.Contains(TEXT("`Set Error`")));
    TestEqual(TEXT("Decompiled BPIR is identical across repeated decompiles"),
        Second.BpirText, First.BpirText);
    // TestEqual on FString compares case-insensitively; pin down byte-identity too.
    TestTrue(TEXT("Decompiled BPIR is byte-identical (case-sensitive) across repeated decompiles"),
        Second.BpirText.Equals(First.BpirText, ESearchCase::CaseSensitive));
    return true;
}

// ============================================================================
// 3. Macro entry — a composite-bearing macro graph must decompile with
//    `entry macro <SourceName>`, not the flattening clone's auto-suffixed name.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirCompositeEntryMacroNameTest,
    "PinWright.bpir.decompiler.CompositeEntryMacroName",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBpirCompositeEntryMacroNameTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CompilerTestUtils::CreateTransientTestBP(TEXT("CompositeEntryMacroBP"));
    TestNotNull(TEXT("Blueprint was created"), BP);
    if (!BP) return false;

    UEdGraph* MacroGraph = FBlueprintEditorUtils::CreateNewGraph(
        BP, FName(TEXT("MyMacro")), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
    TestNotNull(TEXT("Macro graph created"), MacroGraph);
    if (!MacroGraph) return false;

    FBlueprintEditorUtils::AddMacroGraph(BP, MacroGraph,
        /*bIsUserCreated=*/true, static_cast<UClass*>(nullptr));

    // Locate auto-created tunnel pair (predicate matches GraphWalker entry detection).
    UK2Node_Tunnel* EntryTunnel = nullptr;
    UK2Node_Tunnel* ExitTunnel = nullptr;
    for (UEdGraphNode* Node : MacroGraph->Nodes)
    {
        UK2Node_Tunnel* Tunnel = Cast<UK2Node_Tunnel>(Node);
        if (!Tunnel) continue;
        if (Tunnel->bCanHaveOutputs && !Tunnel->bCanHaveInputs)
        {
            EntryTunnel = Tunnel;
        }
        else if (Tunnel->bCanHaveInputs && !Tunnel->bCanHaveOutputs)
        {
            ExitTunnel = Tunnel;
        }
    }
    if (!EntryTunnel || !ExitTunnel)
    {
        AddError(TEXT("Failed to find macro tunnel pair"));
        return false;
    }

    // Exec pins on the tunnel pair so the composite can sit mid-chain between them.
    FEdGraphPinType ExecPinType;
    ExecPinType.PinCategory = UEdGraphSchema_K2::PC_Exec;
    UEdGraphPin* MacroIn = EntryTunnel->CreateUserDefinedPin(
        FName(TEXT("In")), ExecPinType, EGPD_Output);
    UEdGraphPin* MacroOut = ExitTunnel->CreateUserDefinedPin(
        FName(TEXT("Out")), ExecPinType, EGPD_Input);

    if (!PlantMidChainComposite(MacroGraph, MacroIn, MacroOut))
    {
        AddError(TEXT("Failed to plant mid-chain composite in macro graph"));
        return false;
    }

    FBpirDecompiler Decompiler(BP);
    FBpirDecompileResult Result = Decompiler.DecompileMacro(TEXT("MyMacro"));

    TestTrue(TEXT("DecompileMacro succeeded"), Result.bSuccess);
    TestTrue(TEXT("BPIR contains inlined composite body (\"inner\")"),
        Result.BpirText.Contains(TEXT("\"inner\"")));
    TestTrue(TEXT("Entry signature carries the unsuffixed macro name"),
        Result.BpirText.Contains(TEXT("entry macro MyMacro(")));
    TestFalse(TEXT("BPIR must NOT contain auto-suffixed 'MyMacro_'"),
        Result.BpirText.Contains(TEXT("MyMacro_")));
    TestFalse(TEXT("BPIR must NOT contain clone name 'EdGraph_'"),
        Result.BpirText.Contains(TEXT("EdGraph_")));
    return true;
}

// ============================================================================
// 4. Override entry — a composite-bearing override graph in a child Blueprint
//    must decompile as `entry override DoThing`. Pre-fix, the emitter looks up
//    the parent class by the clone's auto-suffixed name, which misses, silently
//    downgrading the entry to `entry function EdGraph_N(...)`.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirCompositeEntryOverrideNameTest,
    "PinWright.bpir.decompiler.CompositeEntryOverrideName",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBpirCompositeEntryOverrideNameTest::RunTest(const FString& Parameters)
{
    // Parent BP declares a callable user function "DoThing"; CompileBlueprint puts the
    // UFunction on the generated class so the child can genuinely override it.
    UBlueprint* ParentBP = CompilerTestUtils::CreateTransientTestBP(TEXT("CompositeOvrParentBP"));
    TestNotNull(TEXT("Parent blueprint was created"), ParentBP);
    if (!ParentBP) return false;

    UEdGraph* ParentFuncGraph = FBlueprintEditorUtils::CreateNewGraph(
        ParentBP, FName(TEXT("DoThing")), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
    TestNotNull(TEXT("Parent function graph created"), ParentFuncGraph);
    if (!ParentFuncGraph) return false;
    FBlueprintEditorUtils::AddFunctionGraph<UClass>(
        ParentBP, ParentFuncGraph, /*bIsUserCreated=*/true, nullptr);

    FKismetEditorUtilities::CompileBlueprint(ParentBP);
    UClass* ParentClass = ParentBP->GeneratedClass;
    if (!ParentClass || !ParentClass->FindFunctionByName(FName(TEXT("DoThing"))))
    {
        AddError(TEXT("Parent generated class does not carry UFunction 'DoThing' after compile"));
        return false;
    }

    UBlueprint* ChildBP = CompilerTestUtils::CreateTransientTestBPWithParent(
        ParentClass, TEXT("CompositeOvrChildBP"));
    TestNotNull(TEXT("Child blueprint was created"), ChildBP);
    if (!ChildBP) return false;

    // Override graph via the editor's own path (bIsUserCreated=false + parent signature
    // class): CreateFunctionGraphTerminators binds the entry to the parent's "DoThing"
    // and auto-spawns a wired CallParentFunction node; the composite lands after it.
    if (!AddCompositeFunctionGraph(ChildBP, FName(TEXT("DoThing")), ParentClass))
    {
        AddError(TEXT("Failed to build 'DoThing' override graph with mid-chain composite"));
        return false;
    }

    FBpirDecompiler Decompiler(ChildBP);
    FBpirDecompileResult Result = Decompiler.DecompileFunction(TEXT("DoThing"));

    TestTrue(TEXT("DecompileFunction succeeded"), Result.bSuccess);
    TestTrue(TEXT("BPIR contains inlined composite body (\"inner\")"),
        Result.BpirText.Contains(TEXT("\"inner\"")));
    TestTrue(TEXT("Entry keeps the override keyword and the source name"),
        Result.BpirText.Contains(TEXT("entry override DoThing")));
    TestFalse(TEXT("Override entry must NOT degrade to 'entry function'"),
        Result.BpirText.Contains(TEXT("entry function ")));
    TestFalse(TEXT("BPIR must NOT contain auto-suffixed 'DoThing_'"),
        Result.BpirText.Contains(TEXT("DoThing_")));
    TestFalse(TEXT("BPIR must NOT contain clone name 'EdGraph_'"),
        Result.BpirText.Contains(TEXT("EdGraph_")));
    return true;
}
