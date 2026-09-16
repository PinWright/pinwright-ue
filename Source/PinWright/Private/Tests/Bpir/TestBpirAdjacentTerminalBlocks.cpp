// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Misc/AutomationTest.h"
#include "CompilerTestUtils.h"
#include "BpirGraphTestHelpers.h"

#include "Compiler/BpirCompiler.h"
#include "Compiler/BpirParser.h"
#include "Compiler/BpirTypes.h"
#include "Compiler/CompilerTypes.h"
#include "Decompiler/BpirDecompiler.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_CallFunction.h"
#include "K2Node_FunctionResult.h"
#include "K2Node_IfThenElse.h"

using namespace CompilerTestUtils;

namespace
{
    UEdGraphNode* GetOnlyLinkedNode(UEdGraphPin* Pin)
    {
        return Pin && Pin->LinkedTo.Num() == 1 && Pin->LinkedTo[0]
            ? Pin->LinkedTo[0]->GetOwningNode()
            : nullptr;
    }

    UEdGraphPin* GetDefaultExecOutput(UEdGraphNode* Node)
    {
        return Node ? Node->FindPin(UEdGraphSchema_K2::PN_Then, EGPD_Output) : nullptr;
    }

    TArray<UK2Node_CallFunction*> CollectPrintStringCalls(UBlueprint* Blueprint)
    {
        TArray<UK2Node_CallFunction*> Calls;
        if (!Blueprint)
        {
            return Calls;
        }

        for (UEdGraph* Graph : Blueprint->UbergraphPages)
        {
            if (!Graph) continue;
            for (UEdGraphNode* Node : Graph->Nodes)
            {
                UK2Node_CallFunction* Call = Cast<UK2Node_CallFunction>(Node);
                if (Call && Call->FunctionReference.GetMemberName() == TEXT("PrintString"))
                {
                    Calls.Add(Call);
                }
            }
        }
        return Calls;
    }

    int32 CountBodyNodesWithExecInput(UBlueprint* Blueprint)
    {
        int32 Count = 0;
        if (!Blueprint)
        {
            return Count;
        }

        for (UEdGraph* Graph : Blueprint->UbergraphPages)
        {
            if (!Graph) continue;
            for (UEdGraphNode* Node : Graph->Nodes)
            {
                if (!Node) continue;
                for (UEdGraphPin* Pin : Node->Pins)
                {
                    if (Pin && Pin->Direction == EGPD_Input
                        && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
                    {
                        ++Count;
                        break;
                    }
                }
            }
        }
        return Count;
    }

    int32 CountInstructionLines(const FString& Text, const FString& Keyword, bool bExact)
    {
        TArray<FString> Lines;
        Text.ParseIntoArrayLines(Lines);
        int32 Count = 0;
        for (const FString& Line : Lines)
        {
            const FString Trimmed = Line.TrimStartAndEnd();
            if ((bExact && Trimmed == Keyword)
                || (!bExact && Trimmed.StartsWith(Keyword)))
            {
                ++Count;
            }
        }
        return Count;
    }

    const FBpirEntryBlock* FindBlockWithBranch(const TArray<FBpirEntryBlock>& Blocks)
    {
        for (const FBpirEntryBlock& Block : Blocks)
        {
            if (Block.Instructions.ContainsByPredicate([](const FBpirInstruction& Inst)
                {
                    return Inst.Opcode == EBpirOpcode::Branch;
                }))
            {
                return &Block;
            }
        }
        return nullptr;
    }

    bool LabelContainsCallThenEnd(const FBpirEntryBlock& Block, const FString& Label)
    {
        const int32* LabelIndex = Block.LabelIndex.Find(Label);
        if (!LabelIndex)
        {
            return false;
        }

        int32 InstructionIndex = *LabelIndex + 1;
        while (InstructionIndex < Block.Instructions.Num()
            && Block.Instructions[InstructionIndex].Opcode == EBpirOpcode::Comment)
        {
            ++InstructionIndex;
        }
        if (InstructionIndex >= Block.Instructions.Num()
            || Block.Instructions[InstructionIndex].Opcode != EBpirOpcode::Call)
        {
            return false;
        }

        ++InstructionIndex;
        while (InstructionIndex < Block.Instructions.Num()
            && Block.Instructions[InstructionIndex].Opcode == EBpirOpcode::Comment)
        {
            ++InstructionIndex;
        }
        return InstructionIndex < Block.Instructions.Num()
            && Block.Instructions[InstructionIndex].Opcode == EBpirOpcode::End;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FBpirAdjacentTerminalBlocksRoundTripTest,
    "PinWright.bpir.round_trip.AdjacentTerminalBlocksPreserveNoFallThrough",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBpirAdjacentTerminalBlocksRoundTripTest::RunTest(const FString& Parameters)
{
    UBlueprint* SourceBlueprint = CreateTransientTestBP(TEXT("AdjacentTerminalBlocksSourceBP"));
    TestNotNull(TEXT("Source Blueprint created"), SourceBlueprint);
    if (!SourceBlueprint || SourceBlueprint->UbergraphPages.IsEmpty()) return false;

    UEdGraph* SourceGraph = SourceBlueprint->UbergraphPages[0];
    UK2Node_Event* BeginPlay = BpirGraphTestHelpers::EnsureBeginPlayNode(SourceGraph);
    UK2Node_IfThenElse* SourceBranch =
        BpirGraphTestHelpers::AddNodeToGraph<UK2Node_IfThenElse>(SourceGraph);
    UK2Node_CallFunction* ThenCall = BpirGraphTestHelpers::AddPrintStringNode(SourceGraph);
    UK2Node_CallFunction* ElseCall = BpirGraphTestHelpers::AddPrintStringNode(SourceGraph);
    TestNotNull(TEXT("BeginPlay event exists"), BeginPlay);
    TestNotNull(TEXT("Source branch created"), SourceBranch);
    TestNotNull(TEXT("Then PrintString created"), ThenCall);
    TestNotNull(TEXT("Else PrintString created"), ElseCall);
    if (!BeginPlay || !SourceBranch || !ThenCall || !ElseCall) return false;

    TestTrue(TEXT("BeginPlay connects to source branch"), WireThenToExec(BeginPlay, SourceBranch));
    UEdGraphPin* ThenPin = SourceBranch->FindPin(UEdGraphSchema_K2::PN_Then, EGPD_Output);
    UEdGraphPin* ElsePin = SourceBranch->FindPin(UEdGraphSchema_K2::PN_Else, EGPD_Output);
    UEdGraphPin* ThenExecIn = ThenCall->FindPin(UEdGraphSchema_K2::PN_Execute, EGPD_Input);
    UEdGraphPin* ElseExecIn = ElseCall->FindPin(UEdGraphSchema_K2::PN_Execute, EGPD_Input);
    TestNotNull(TEXT("Branch then pin exists"), ThenPin);
    TestNotNull(TEXT("Branch else pin exists"), ElsePin);
    TestNotNull(TEXT("Then call exec input exists"), ThenExecIn);
    TestNotNull(TEXT("Else call exec input exists"), ElseExecIn);
    if (!ThenPin || !ElsePin || !ThenExecIn || !ElseExecIn) return false;

    ThenPin->MakeLinkTo(ThenExecIn);
    ElsePin->MakeLinkTo(ElseExecIn);

    TestEqual(TEXT("Source then arm has one link"), ThenPin->LinkedTo.Num(), 1);
    TestEqual(TEXT("Source else arm has one link"), ElsePin->LinkedTo.Num(), 1);
    TestTrue(TEXT("Source branch arms target distinct calls"),
        GetOnlyLinkedNode(ThenPin) != GetOnlyLinkedNode(ElsePin));
    UEdGraphPin* ThenExecOut = GetDefaultExecOutput(ThenCall);
    UEdGraphPin* ElseExecOut = GetDefaultExecOutput(ElseCall);
    TestNotNull(TEXT("Then call exec output exists"), ThenExecOut);
    TestNotNull(TEXT("Else call exec output exists"), ElseExecOut);
    if (!ThenExecOut || !ElseExecOut) return false;
    TestEqual(TEXT("Source then call terminates"), ThenExecOut->LinkedTo.Num(), 0);
    TestEqual(TEXT("Source else call terminates"), ElseExecOut->LinkedTo.Num(), 0);

    FBpirDecompiler Decompiler(SourceBlueprint);
    const FBpirDecompileResult DecompileResult = Decompiler.Decompile();
    TestTrue(TEXT("Decompile succeeds"), DecompileResult.bSuccess);
    if (!DecompileResult.bSuccess)
    {
        for (const FBpirWarning& Warning : DecompileResult.Warnings)
        {
            AddError(FString::Printf(TEXT("Decompile warning: %s"), *Warning.Text));
        }
        return false;
    }

    const FString& DecompiledText = DecompileResult.BpirText;
    TestEqual(TEXT("Each disconnected terminal emits one bare end"),
        CountInstructionLines(DecompiledText, TEXT("end"), /*bExact=*/true), 2);
    TestEqual(TEXT("Disconnected event terminals do not become returns"),
        CountInstructionLines(DecompiledText, TEXT("return"), /*bExact=*/false), 0);

    FBpirParser Parser;
    TArray<FBpirEntryBlock> ParsedBlocks;
    TArray<FCompileError> ParseErrors;
    const bool bParsed = Parser.Parse(DecompiledText, ParsedBlocks, ParseErrors);
    for (const FCompileError& Error : ParseErrors)
    {
        AddError(FString::Printf(TEXT("Parse L%d: %s"), Error.Line, *Error.Message));
    }
    TestTrue(TEXT("Production parser accepts decompiled end instructions"), bParsed);
    if (!bParsed) return false;

    const FBpirEntryBlock* ParsedBlock = FindBlockWithBranch(ParsedBlocks);
    TestNotNull(TEXT("Parsed output contains the source branch"), ParsedBlock);
    if (!ParsedBlock) return false;

    const FBpirInstruction* ParsedBranch = ParsedBlock->Instructions.FindByPredicate(
        [](const FBpirInstruction& Inst)
        {
            return Inst.Opcode == EBpirOpcode::Branch;
        });
    TestNotNull(TEXT("Parsed branch instruction found"), ParsedBranch);
    if (!ParsedBranch) return false;

    TestEqual(TEXT("Parsed branch exposes two target labels"), ParsedBranch->ExecTargets.Num(), 2);
    if (ParsedBranch->ExecTargets.Num() == 2)
    {
        TestTrue(TEXT("Parsed branch targets distinct labels"),
            ParsedBranch->ExecTargets[0].Label != ParsedBranch->ExecTargets[1].Label);
        for (const FBpirExecTarget& Target : ParsedBranch->ExecTargets)
        {
            TestTrue(*FString::Printf(TEXT("@%s contains call followed by end"), *Target.Label),
                LabelContainsCallThenEnd(*ParsedBlock, Target.Label));
        }
    }

    for (const FBpirInstruction& Inst : ParsedBlock->Instructions)
    {
        if (Inst.Opcode == EBpirOpcode::End)
        {
            TestFalse(TEXT("End is a node-less non-impure opcode"), Inst.IsImpure());
        }
    }

    FBpirParser InvalidParser;
    TArray<FBpirEntryBlock> InvalidBlocks;
    TArray<FCompileError> InvalidErrors;
    const bool bInvalidParsed = InvalidParser.Parse(
        TEXT("entry event BeginPlay() {\n    end anything\n}\n"),
        InvalidBlocks,
        InvalidErrors,
        /*bSkipReferenceValidation=*/true);
    TestFalse(TEXT("Production parser rejects arguments after end"), bInvalidParsed);
    TestTrue(TEXT("Invalid end reports its no-arguments contract"),
        ErrorsContain(InvalidErrors, TEXT("does not accept arguments")));

    UBlueprint* RecompiledBlueprint = CreateTransientTestBP(TEXT("AdjacentTerminalBlocksRecompiledBP"));
    TestNotNull(TEXT("Recompile target Blueprint created"), RecompiledBlueprint);
    if (!RecompiledBlueprint) return false;

    FBpirCompiler Compiler(RecompiledBlueprint);
    const FCompileResult CompileResult = Compiler.Compile(DecompiledText);
    for (const FCompileError& Error : CompileResult.Errors)
    {
        AddError(FString::Printf(TEXT("Recompile L%d: %s"), Error.Line, *Error.Message));
    }
    TestTrue(TEXT("Decompiled text recompiles"), CompileResult.bSuccess);
    if (!CompileResult.bSuccess) return false;

    UK2Node_IfThenElse* RecompiledBranch =
        CompilerTestUtils::FindNodeOfType<UK2Node_IfThenElse>(RecompiledBlueprint);
    TestNotNull(TEXT("Recompiled branch exists"), RecompiledBranch);
    if (!RecompiledBranch) return false;

    UEdGraphPin* RecompiledThenPin =
        RecompiledBranch->FindPin(UEdGraphSchema_K2::PN_Then, EGPD_Output);
    UEdGraphPin* RecompiledElsePin =
        RecompiledBranch->FindPin(UEdGraphSchema_K2::PN_Else, EGPD_Output);
    UEdGraphNode* RecompiledThenTarget = GetOnlyLinkedNode(RecompiledThenPin);
    UEdGraphNode* RecompiledElseTarget = GetOnlyLinkedNode(RecompiledElsePin);
    TestNotNull(TEXT("Recompiled then arm is wired"), RecompiledThenTarget);
    TestNotNull(TEXT("Recompiled else arm is wired"), RecompiledElseTarget);
    TestTrue(TEXT("Recompiled branch targets two distinct calls"),
        RecompiledThenTarget && RecompiledElseTarget && RecompiledThenTarget != RecompiledElseTarget);
    TestTrue(TEXT("Recompiled then target is PrintString"),
        RecompiledThenTarget && RecompiledThenTarget->IsA<UK2Node_CallFunction>());
    TestTrue(TEXT("Recompiled else target is PrintString"),
        RecompiledElseTarget && RecompiledElseTarget->IsA<UK2Node_CallFunction>());

    const TArray<UK2Node_CallFunction*> RecompiledCalls = CollectPrintStringCalls(RecompiledBlueprint);
    TestEqual(TEXT("Recompile creates exactly the two source calls"), RecompiledCalls.Num(), 2);
    for (UK2Node_CallFunction* Call : RecompiledCalls)
    {
        UEdGraphPin* ExecOutput = GetDefaultExecOutput(Call);
        TestNotNull(TEXT("Recompiled call has a default exec output"), ExecOutput);
        if (ExecOutput)
        {
            TestEqual(TEXT("Recompiled terminal call remains disconnected"), ExecOutput->LinkedTo.Num(), 0);
        }
    }

    TestEqual(TEXT("End creates no extra impure body node"),
        CountBodyNodesWithExecInput(RecompiledBlueprint), 3);
    TestNull(TEXT("End does not create a FunctionResult node"),
        CompilerTestUtils::FindNodeOfType<UK2Node_FunctionResult>(RecompiledBlueprint));

    const FString GotoAfterEndCode = TEXT(
        "entry event BeginPlay() {\n"
        "    call PrintString(InString: \"BeforeEnd\")\n"
        "    end\n"
        "    exec -> @target\n"
        "\n"
        "@target:\n"
        "    call PrintString(InString: \"AfterEndTarget\")\n"
        "    end\n"
        "}\n");

    FBpirParser GotoAfterEndParser;
    TArray<FBpirEntryBlock> GotoAfterEndBlocks;
    TArray<FCompileError> GotoAfterEndParseErrors;
    const bool bGotoAfterEndParsed = GotoAfterEndParser.Parse(
        GotoAfterEndCode, GotoAfterEndBlocks, GotoAfterEndParseErrors);
    for (const FCompileError& Error : GotoAfterEndParseErrors)
    {
        AddError(FString::Printf(TEXT("Goto-after-end parse L%d: %s"), Error.Line, *Error.Message));
    }
    TestTrue(TEXT("Production parser accepts goto after end"), bGotoAfterEndParsed);
    TestEqual(TEXT("Goto-after-end parse produces one entry block"),
        GotoAfterEndBlocks.Num(), 1);
    if (!bGotoAfterEndParsed || GotoAfterEndBlocks.Num() != 1) return false;

    bool bParsedEndThenGoto = false;
    const TArray<FBpirInstruction>& GotoAfterEndInstructions = GotoAfterEndBlocks[0].Instructions;
    for (int32 Index = 1; Index < GotoAfterEndInstructions.Num(); ++Index)
    {
        if (GotoAfterEndInstructions[Index - 1].Opcode == EBpirOpcode::End
            && GotoAfterEndInstructions[Index].Opcode == EBpirOpcode::ExecGoto)
        {
            bParsedEndThenGoto = true;
            break;
        }
    }
    TestTrue(TEXT("Parsed fixture contains adjacent end then exec-goto"), bParsedEndThenGoto);

    UBlueprint* GotoAfterEndBlueprint = CreateTransientTestBP(TEXT("GotoAfterEndBP"));
    TestNotNull(TEXT("Goto-after-end Blueprint created"), GotoAfterEndBlueprint);
    if (!GotoAfterEndBlueprint) return false;

    FBpirCompiler GotoAfterEndCompiler(GotoAfterEndBlueprint);
    const FCompileResult GotoAfterEndCompileResult = GotoAfterEndCompiler.Compile(GotoAfterEndCode);
    for (const FCompileError& Error : GotoAfterEndCompileResult.Errors)
    {
        AddError(FString::Printf(TEXT("Goto-after-end compile L%d: %s"), Error.Line, *Error.Message));
    }
    TestTrue(TEXT("Goto after end compiles as a source-less no-op"),
        GotoAfterEndCompileResult.bSuccess);
    if (!GotoAfterEndCompileResult.bSuccess) return false;

    const TArray<UK2Node_CallFunction*> GotoAfterEndCalls =
        CollectPrintStringCalls(GotoAfterEndBlueprint);
    TestEqual(TEXT("Goto-after-end compile creates both calls"), GotoAfterEndCalls.Num(), 2);
    UK2Node_CallFunction* BeforeEndCall = nullptr;
    UK2Node_CallFunction* AfterEndTargetCall = nullptr;
    for (UK2Node_CallFunction* Call : GotoAfterEndCalls)
    {
        UEdGraphPin* InStringPin = Call->FindPin(TEXT("InString"));
        if (!InStringPin)
        {
            InStringPin = Call->FindPin(TEXT("inString"));
        }
        if (!InStringPin)
        {
            continue;
        }
        if (InStringPin->DefaultValue.Contains(TEXT("BeforeEnd")))
        {
            BeforeEndCall = Call;
        }
        else if (InStringPin->DefaultValue.Contains(TEXT("AfterEndTarget")))
        {
            AfterEndTargetCall = Call;
        }
    }
    TestNotNull(TEXT("Call before end exists"), BeforeEndCall);
    TestNotNull(TEXT("Goto target call exists"), AfterEndTargetCall);
    if (!BeforeEndCall || !AfterEndTargetCall) return false;

    UEdGraphPin* BeforeEndExecOutput = GetDefaultExecOutput(BeforeEndCall);
    UEdGraphPin* AfterEndTargetExecInput =
        AfterEndTargetCall->FindPin(UEdGraphSchema_K2::PN_Execute, EGPD_Input);
    TestNotNull(TEXT("Call before end has an exec output"), BeforeEndExecOutput);
    TestNotNull(TEXT("Goto target call has an exec input"), AfterEndTargetExecInput);
    if (!BeforeEndExecOutput || !AfterEndTargetExecInput) return false;
    TestEqual(TEXT("Exec-goto cannot borrow the pre-end output"),
        BeforeEndExecOutput->LinkedTo.Num(), 0);
    TestEqual(TEXT("Goto target receives no pre-end link"),
        AfterEndTargetExecInput->LinkedTo.Num(), 0);

    // Counterfactuals: without decompiler emission the two text-count checks fail and
    // recompile auto-chains the first call into the second label; without parser support
    // recompile rejects `end`; without the ordinary WireExecPins state clear, the first
    // terminal relinks through label fall-through.
    // Without the ExecGoto backward-scan boundary, its source-less jump also relinks the
    // `BeforeEnd` call to `AfterEndTarget` across the terminator.
    return true;
}
