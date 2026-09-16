// Copyright (c) 2026 Alexander Penkin. MIT License.

// Verifies that nested composites (composite-inside-composite) are flattened
// recursively by InlineCompositesInPlace in BpirDecompiler.cpp. Both inner-most
// PrintStrings must surface in the parent BPIR stream and the recompiled graph
// must contain zero UK2Node_Composite nodes at any depth.
//
// Counterfactual: if the inline pass becomes top-level only (no recursion into
// BoundGraph before flattening), the outer composite dissolves but the inner
// composite survives — `K2Node_Composite` reappears in BPIR text and the
// recompile node-count assertion fails.

#include "Misc/AutomationTest.h"
#include "Tests/TestUtils.h"
#include "CompilerTestUtils.h"

#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "GameFramework/Actor.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "K2Node_Composite.h"
#include "K2Node_Event.h"
#include "K2Node_CallFunction.h"
#include "K2Node_Tunnel.h"
#include "EdGraphSchema_K2.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphPin.h"
#include "Kismet/KismetSystemLibrary.h"
#include "UObject/Package.h"

#include "Compiler/BpirCompiler.h"
#include "Compiler/CompilerTypes.h"
#include "Decompiler/BpirDecompiler.h"

namespace
{
    // Adds an exec input + exec output to a freshly placed K2Node_Composite so it
    // can sit mid-chain. Returns the in/out exec pins on the outer composite.
    void GiveCompositeExecPassthrough(
        UK2Node_Composite* Composite,
        UEdGraphPin*& OutInExec,
        UEdGraphPin*& OutOutExec)
    {
        OutInExec = nullptr;
        OutOutExec = nullptr;
        if (!Composite || !Composite->InputSinkNode || !Composite->OutputSourceNode)
        {
            return;
        }

        FEdGraphPinType ExecPinType;
        ExecPinType.PinCategory = UEdGraphSchema_K2::PC_Exec;
        Composite->InputSinkNode->CreateUserDefinedPin(
            UEdGraphSchema_K2::PN_Execute, ExecPinType, EGPD_Output);
        Composite->OutputSourceNode->CreateUserDefinedPin(
            UEdGraphSchema_K2::PN_Then, ExecPinType, EGPD_Input);
        Composite->ReconstructNode();

        for (UEdGraphPin* Pin : Composite->Pins)
        {
            if (!Pin || Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec) continue;
            if (Pin->Direction == EGPD_Input && !OutInExec)   { OutInExec  = Pin; }
            if (Pin->Direction == EGPD_Output && !OutOutExec) { OutOutExec = Pin; }
        }
    }
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirCompositeInlineNestedTest,
    "PinWright.bpir.decompiler.CompositeInlineNested",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBpirCompositeInlineNestedTest::RunTest(const FString& Parameters)
{
    // Layout:
    //   EventGraph
    //   └─ Event BeginPlay -> OuterComposite -> (downstream chain ends)
    //      OuterComposite.BoundGraph ("Outer"):
    //         entry -> OuterPrint("outer") -> InnerComposite -> exit
    //         InnerComposite.BoundGraph ("Inner"):
    //            entry -> InnerPrint("inner") -> exit

    const FName Name = *FString::Printf(TEXT("TestNestedCompositeBP_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::DigitsLower).Left(8));
    UBlueprint* BP = FKismetEditorUtilities::CreateBlueprint(
        AActor::StaticClass(),
        GetTransientPackage(),
        Name,
        BPTYPE_Normal,
        UBlueprint::StaticClass(),
        UBlueprintGeneratedClass::StaticClass(),
        FName(TEXT("PinWrightTests")));
    if (!BP || BP->UbergraphPages.Num() == 0)
    {
        AddError(TEXT("Failed to create nested-composite test blueprint"));
        return false;
    }

    UEdGraph* EventGraph = BP->UbergraphPages[0];

    UK2Node_Event* BeginPlay = NewObject<UK2Node_Event>(EventGraph);
    BeginPlay->CreateNewGuid();
    BeginPlay->EventReference.SetExternalMember(TEXT("ReceiveBeginPlay"), AActor::StaticClass());
    BeginPlay->bOverrideFunction = true;
    BeginPlay->PostPlacedNewNode();
    BeginPlay->AllocateDefaultPins();
    EventGraph->AddNode(BeginPlay, /*bFromUI=*/true, /*bSelectNewNode=*/false);
    BeginPlay->ReconstructNode();

    UK2Node_Composite* OuterComposite = NewObject<UK2Node_Composite>(EventGraph);
    OuterComposite->CreateNewGuid();
    OuterComposite->PostPlacedNewNode();
    EventGraph->AddNode(OuterComposite, /*bFromUI=*/true, /*bSelectNewNode=*/false);
    if (!OuterComposite->BoundGraph)
    {
        AddError(TEXT("Outer composite missing BoundGraph"));
        return false;
    }
    FBlueprintEditorUtils::RenameGraph(OuterComposite->BoundGraph, TEXT("Outer"));

    UEdGraphPin* OuterInExec = nullptr;
    UEdGraphPin* OuterOutExec = nullptr;
    GiveCompositeExecPassthrough(OuterComposite, OuterInExec, OuterOutExec);

    UEdGraph* OuterBound = OuterComposite->BoundGraph;
    UK2Node_CallFunction* OuterPrint = CompilerTestUtils::SpawnPrintStringCall(OuterBound, 0, 0);
    if (UEdGraphPin* SP = OuterPrint ? OuterPrint->FindPin(TEXT("InString"), EGPD_Input) : nullptr)
    {
        SP->DefaultValue = TEXT("outer");
    }

    // Inner composite, placed inside OuterComposite's BoundGraph.
    UK2Node_Composite* InnerComposite = NewObject<UK2Node_Composite>(OuterBound);
    InnerComposite->CreateNewGuid();
    InnerComposite->PostPlacedNewNode();
    OuterBound->AddNode(InnerComposite, /*bFromUI=*/true, /*bSelectNewNode=*/false);
    if (!InnerComposite->BoundGraph)
    {
        AddError(TEXT("Inner composite missing BoundGraph"));
        return false;
    }
    FBlueprintEditorUtils::RenameGraph(InnerComposite->BoundGraph, TEXT("Inner"));

    UEdGraphPin* InnerInExec = nullptr;
    UEdGraphPin* InnerOutExec = nullptr;
    GiveCompositeExecPassthrough(InnerComposite, InnerInExec, InnerOutExec);

    UEdGraph* InnerBound = InnerComposite->BoundGraph;
    UK2Node_CallFunction* InnerPrint = CompilerTestUtils::SpawnPrintStringCall(InnerBound, 0, 0);
    if (UEdGraphPin* SP = InnerPrint ? InnerPrint->FindPin(TEXT("InString"), EGPD_Input) : nullptr)
    {
        SP->DefaultValue = TEXT("inner");
    }

    // Wire InnerComposite body: entry.exec -> InnerPrint.execute -> InnerPrint.then -> exit.then
    UEdGraphPin* InnerEntryThen = InnerComposite->InputSinkNode->FindPin(
        UEdGraphSchema_K2::PN_Execute, EGPD_Output);
    UEdGraphPin* InnerExitExec = InnerComposite->OutputSourceNode->FindPin(
        UEdGraphSchema_K2::PN_Then, EGPD_Input);
    UEdGraphPin* InnerPrintExec = InnerPrint ? InnerPrint->FindPin(
        UEdGraphSchema_K2::PN_Execute, EGPD_Input) : nullptr;
    UEdGraphPin* InnerPrintThen = InnerPrint ? InnerPrint->FindPin(
        UEdGraphSchema_K2::PN_Then, EGPD_Output) : nullptr;
    if (InnerEntryThen && InnerPrintExec) { InnerEntryThen->MakeLinkTo(InnerPrintExec); }
    if (InnerPrintThen && InnerExitExec)  { InnerPrintThen->MakeLinkTo(InnerExitExec); }

    // Wire OuterComposite body: entry.exec -> OuterPrint.execute -> OuterPrint.then -> InnerComposite.in
    //                            InnerComposite.out -> exit.then
    UEdGraphPin* OuterEntryThen = OuterComposite->InputSinkNode->FindPin(
        UEdGraphSchema_K2::PN_Execute, EGPD_Output);
    UEdGraphPin* OuterExitExec = OuterComposite->OutputSourceNode->FindPin(
        UEdGraphSchema_K2::PN_Then, EGPD_Input);
    UEdGraphPin* OuterPrintExec = OuterPrint ? OuterPrint->FindPin(
        UEdGraphSchema_K2::PN_Execute, EGPD_Input) : nullptr;
    UEdGraphPin* OuterPrintThen = OuterPrint ? OuterPrint->FindPin(
        UEdGraphSchema_K2::PN_Then, EGPD_Output) : nullptr;
    if (OuterEntryThen && OuterPrintExec) { OuterEntryThen->MakeLinkTo(OuterPrintExec); }
    if (OuterPrintThen && InnerInExec)    { OuterPrintThen->MakeLinkTo(InnerInExec); }
    if (InnerOutExec && OuterExitExec)    { InnerOutExec->MakeLinkTo(OuterExitExec); }

    // Wire BeginPlay.then -> OuterComposite.in.
    UEdGraphPin* BeginPlayThen = BeginPlay->FindPin(UEdGraphSchema_K2::PN_Then, EGPD_Output);
    if (BeginPlayThen && OuterInExec)
    {
        BeginPlayThen->MakeLinkTo(OuterInExec);
    }

    FBpirDecompiler Decompiler(BP);
    FBpirDecompileResult Result = Decompiler.Decompile();
    TestTrue(TEXT("Decompile of nested-composite BP succeeded"), Result.bSuccess);

    // Both inner-most PrintStrings surface flat at the parent stream.
    TestTrue(
        TEXT("BPIR text contains outer PrintString from outer composite body"),
        Result.BpirText.Contains(TEXT("\"outer\"")));
    TestTrue(
        TEXT("BPIR text contains inner PrintString from doubly-nested composite body"),
        Result.BpirText.Contains(TEXT("\"inner\"")));

    // Neither composite shape survives in BPIR text at any depth.
    TestFalse(
        TEXT("BPIR text must NOT contain 'K2Node_Composite' at any nesting depth"),
        Result.BpirText.Contains(TEXT("K2Node_Composite")));
    TestFalse(
        TEXT("BPIR text must NOT contain 'composite ' keyword at any nesting depth"),
        Result.BpirText.Contains(TEXT("composite ")));
    // Tunnel boundary nodes from the inner composites must not leak as raw text either.
    TestFalse(
        TEXT("BPIR text must NOT contain 'K2Node_Tunnel' from collapsed boundary nodes"),
        Result.BpirText.Contains(TEXT("K2Node_Tunnel")));

    // Recompile and verify zero composite nodes remain.
    UBlueprint* RecompileBP = CompilerTestUtils::CreateTransientTestBP(TEXT("NestedCompositeRecompileBP"));
    if (!RecompileBP)
    {
        AddError(TEXT("Failed to create recompile blueprint"));
        return false;
    }
    FBpirCompiler Recompiler(RecompileBP);
    FCompileResult Recompile = Recompiler.Compile(Result.BpirText);
    TestTrue(TEXT("Recompile of decompiled nested-composite BPIR succeeded"), Recompile.bSuccess);

    int32 RemainingComposites = 0;
    for (UEdGraph* G : RecompileBP->UbergraphPages)
    {
        if (!G) continue;
        for (UEdGraphNode* N : G->Nodes)
        {
            if (Cast<UK2Node_Composite>(N)) { ++RemainingComposites; }
        }
    }
    TestEqual(TEXT("Recompiled graph contains zero UK2Node_Composite nodes (any depth)"),
        RemainingComposites, 0);
    return true;
}
