// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression test for F-bpir-composite-keyword (inline-at-decompile pivot).
//
// Composites are dissolved during decompile by the InlineCompositesInPlace pre-pass
// in BpirDecompiler.cpp, so BPIR text never carries a `K2Node_Composite` token nor
// a `composite ` keyword. The composite's inner body (PrintString here) appears in
// the parent stream as if there were no composite at all. After recompile the
// resulting graph contains zero UK2Node_Composite nodes — the inline is the
// canonical representation, not a transitional shape.
//
// Counterfactual: if InlineCompositesInPlace is removed (decompile sees the live
// composite again), BPIR text reintroduces `K2Node_Composite` from the generic
// fallback and the recompiled graph re-creates UK2Node_Composite — both
// assertions below fail.

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
    // Mid-graph composite layout:
    //
    //   EventGraph
    //   ├─ Event BeginPlay
    //   ├─ K2Node_Composite ("MyBlock")
    //   │  └─ BoundGraph ("MyBlock")
    //   │     ├─ Entry tunnel  -> InnerPrint.execute
    //   │     └─ InnerPrint (PrintString "inner") -> Exit tunnel
    //   └─ OuterPrint (PrintString "outer")
    //
    //   Wiring: BeginPlay.then -> Composite.input_exec -> OuterPrint.execute
    //
    // The composite is mid-chain (not an entry-point holder), so the pre-pass
    // must dissolve it into the parent EventGraph and the inner PrintString
    // must surface in BPIR text as a top-level instruction.

    struct FMidGraphCompositeFixture
    {
        UBlueprint* BP = nullptr;
        UEdGraph* EventGraph = nullptr;
        UK2Node_Event* BeginPlayEvent = nullptr;
        UK2Node_Composite* CompositeNode = nullptr;
        UEdGraph* BoundGraph = nullptr;
        UK2Node_CallFunction* InnerPrint = nullptr;
        UK2Node_CallFunction* OuterPrint = nullptr;
    };

    FMidGraphCompositeFixture BuildMidGraphCompositeFixture()
    {
        FMidGraphCompositeFixture F;

        const FName Name = *FString::Printf(TEXT("TestMidCompositeBP_%s"),
            *FGuid::NewGuid().ToString(EGuidFormats::DigitsLower).Left(8));
        F.BP = FKismetEditorUtilities::CreateBlueprint(
            AActor::StaticClass(),
            GetTransientPackage(),
            Name,
            BPTYPE_Normal,
            UBlueprint::StaticClass(),
            UBlueprintGeneratedClass::StaticClass(),
            FName(TEXT("PinWrightTests")));

        if (!F.BP || F.BP->UbergraphPages.Num() == 0)
        {
            return F;
        }

        F.EventGraph = F.BP->UbergraphPages[0];

        // Event BeginPlay in EventGraph (entry lives outside the composite).
        F.BeginPlayEvent = NewObject<UK2Node_Event>(F.EventGraph);
        F.BeginPlayEvent->CreateNewGuid();
        F.BeginPlayEvent->EventReference.SetExternalMember(
            TEXT("ReceiveBeginPlay"), AActor::StaticClass());
        F.BeginPlayEvent->bOverrideFunction = true;
        F.BeginPlayEvent->PostPlacedNewNode();
        F.BeginPlayEvent->AllocateDefaultPins();
        F.EventGraph->AddNode(F.BeginPlayEvent, /*bFromUI=*/true, /*bSelectNewNode=*/false);
        F.BeginPlayEvent->ReconstructNode();

        // Mid-graph composite.
        F.CompositeNode = NewObject<UK2Node_Composite>(F.EventGraph);
        F.CompositeNode->CreateNewGuid();
        F.CompositeNode->PostPlacedNewNode();
        F.EventGraph->AddNode(F.CompositeNode, /*bFromUI=*/true, /*bSelectNewNode=*/false);

        F.BoundGraph = F.CompositeNode->BoundGraph;
        if (!F.BoundGraph || !F.CompositeNode->InputSinkNode || !F.CompositeNode->OutputSourceNode)
        {
            return F;
        }

        FBlueprintEditorUtils::RenameGraph(F.BoundGraph, TEXT("MyBlock"));

        // Give the composite an exec input + exec output so it sits mid-chain.
        FEdGraphPinType ExecPinType;
        ExecPinType.PinCategory = UEdGraphSchema_K2::PC_Exec;
        F.CompositeNode->InputSinkNode->CreateUserDefinedPin(
            UEdGraphSchema_K2::PN_Execute,
            ExecPinType,
            EGPD_Output);
        F.CompositeNode->OutputSourceNode->CreateUserDefinedPin(
            UEdGraphSchema_K2::PN_Then,
            ExecPinType,
            EGPD_Input);
        F.CompositeNode->ReconstructNode();

        // Inner PrintString INSIDE BoundGraph: entry -> InnerPrint -> exit.
        F.InnerPrint = CompilerTestUtils::SpawnPrintStringCall(F.BoundGraph, 0, 0);
        if (F.InnerPrint)
        {
            UEdGraphPin* EntryThen = F.CompositeNode->InputSinkNode->FindPin(
                UEdGraphSchema_K2::PN_Execute, EGPD_Output);
            UEdGraphPin* InnerExec = F.InnerPrint->FindPin(
                UEdGraphSchema_K2::PN_Execute, EGPD_Input);
            if (EntryThen && InnerExec)
            {
                EntryThen->MakeLinkTo(InnerExec);
            }

            UEdGraphPin* InnerThen = F.InnerPrint->FindPin(
                UEdGraphSchema_K2::PN_Then, EGPD_Output);
            UEdGraphPin* ExitExec = F.CompositeNode->OutputSourceNode->FindPin(
                UEdGraphSchema_K2::PN_Then, EGPD_Input);
            if (InnerThen && ExitExec)
            {
                InnerThen->MakeLinkTo(ExitExec);
            }

            UEdGraphPin* InnerStringPin = F.InnerPrint->FindPin(TEXT("InString"), EGPD_Input);
            if (InnerStringPin)
            {
                InnerStringPin->DefaultValue = TEXT("inner");
            }
        }

        // Outer PrintString in EventGraph, downstream of the composite.
        F.OuterPrint = CompilerTestUtils::SpawnPrintStringCall(F.EventGraph, 0, 0);
        if (F.OuterPrint)
        {
            UEdGraphPin* OuterStringPin = F.OuterPrint->FindPin(TEXT("InString"), EGPD_Input);
            if (OuterStringPin)
            {
                OuterStringPin->DefaultValue = TEXT("outer");
            }
        }

        // Wire BeginPlay.then -> Composite (first exec input).
        UEdGraphPin* BeginPlayThen = F.BeginPlayEvent->FindPin(
            UEdGraphSchema_K2::PN_Then, EGPD_Output);
        UEdGraphPin* CompositeInExec = nullptr;
        for (UEdGraphPin* Pin : F.CompositeNode->Pins)
        {
            if (Pin && Pin->Direction == EGPD_Input
                && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
            {
                CompositeInExec = Pin;
                break;
            }
        }
        if (BeginPlayThen && CompositeInExec)
        {
            BeginPlayThen->MakeLinkTo(CompositeInExec);
        }

        // Wire Composite (first exec output) -> OuterPrint.execute.
        UEdGraphPin* CompositeOutExec = nullptr;
        for (UEdGraphPin* Pin : F.CompositeNode->Pins)
        {
            if (Pin && Pin->Direction == EGPD_Output
                && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
            {
                CompositeOutExec = Pin;
                break;
            }
        }
        UEdGraphPin* OuterPrintExec = F.OuterPrint ? F.OuterPrint->FindPin(
            UEdGraphSchema_K2::PN_Execute, EGPD_Input) : nullptr;
        if (CompositeOutExec && OuterPrintExec)
        {
            CompositeOutExec->MakeLinkTo(OuterPrintExec);
        }

        return F;
    }
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirCompositeNamePreservedTest,
    "PinWright.bpir.decompiler.CompositeNamePreserved",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBpirCompositeNamePreservedTest::RunTest(const FString& Parameters)
{
    FMidGraphCompositeFixture F = BuildMidGraphCompositeFixture();
    if (!F.BP)
    {
        AddError(TEXT("Failed to build mid-graph composite fixture"));
        return false;
    }
    if (!F.BoundGraph || !F.InnerPrint || !F.OuterPrint)
    {
        AddError(TEXT("Composite BoundGraph / inner / outer PrintString missing"));
        return false;
    }

    FBpirDecompiler Decompiler(F.BP);
    FBpirDecompileResult Result = Decompiler.Decompile();

    TestTrue(TEXT("Decompile succeeded"), Result.bSuccess);

    // Inner PrintString from inside the composite must surface in the parent stream.
    TestTrue(
        TEXT("BPIR text contains inner PrintString call from inlined composite body"),
        Result.BpirText.Contains(TEXT("PrintString")) && Result.BpirText.Contains(TEXT("\"inner\"")));

    // Outer PrintString downstream of the composite stays in the parent stream.
    TestTrue(
        TEXT("BPIR text contains outer PrintString downstream of inlined composite"),
        Result.BpirText.Contains(TEXT("\"outer\"")));

    // No composite identity remains in BPIR text — neither the engine class token
    // nor a `composite ` keyword (trailing space avoids matching unrelated tokens).
    TestFalse(
        TEXT("BPIR text must NOT contain 'K2Node_Composite' after inline pre-pass"),
        Result.BpirText.Contains(TEXT("K2Node_Composite")));
    TestFalse(
        TEXT("BPIR text must NOT contain 'composite ' keyword after inline pre-pass"),
        Result.BpirText.Contains(TEXT("composite ")));

    // Recompile and verify zero composite nodes remain in the resulting graphs.
    UBlueprint* RecompileBP = CompilerTestUtils::CreateTransientTestBP(TEXT("CompositeInlineRecompileBP"));
    if (!RecompileBP)
    {
        AddError(TEXT("Failed to create blueprint for recompile sub-test"));
        return false;
    }
    FBpirCompiler Recompiler(RecompileBP);
    FCompileResult Recompile = Recompiler.Compile(Result.BpirText);
    TestTrue(TEXT("Recompile of decompiled BPIR succeeded"), Recompile.bSuccess);

    int32 RemainingComposites = 0;
    for (UEdGraph* G : RecompileBP->UbergraphPages)
    {
        if (!G) continue;
        for (UEdGraphNode* N : G->Nodes)
        {
            if (Cast<UK2Node_Composite>(N)) { ++RemainingComposites; }
        }
    }
    TestEqual(TEXT("Recompiled graph contains zero UK2Node_Composite nodes"), RemainingComposites, 0);
    return true;
}
