// Copyright (c) 2026 Alexander Penkin. MIT License.

// TestBpirPureBindingHoist.cpp - E-bpir-pure-nodes-in-branch-block. A pure node was bound
// inside the labeled block of its first consumer while a later block also read it, so the
// text claimed the value existed on one branch only. A pure binding read across blocks now
// sits at the top of the entry body.

#include "Misc/AutomationTest.h"
#include "CompilerTestUtils.h"

#include "Compiler/BpirCompiler.h"
#include "Compiler/CompilerTypes.h"
#include "Decompiler/BpirDecompiler.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_CallFunction.h"

namespace TestBpirPureBindingHoistHelpers
{
    const TCHAR* const SharedPureSource =
        TEXT("entry function HoistProbe(bool bFlag, int Base) {\n")
        TEXT("    %b = branch($bFlag) [true -> @then, false -> @merge]\n")
        TEXT("\n")
        TEXT("@then:\n")
        TEXT("    %s = pure Conv_IntToString(InInt: $Base)\n")
        TEXT("    call PrintString(InString: %s)\n")
        TEXT("    exec -> @merge\n")
        TEXT("\n")
        TEXT("@merge:\n")
        TEXT("    call PrintString(InString: %s)\n")
        TEXT("}\n");

    // Label of the block holding the first line that contains Needle ("" = entry block,
    // "<missing>" when no line matches).
    FString BlockOfLineContaining(const FString& Text, const FString& Needle)
    {
        TArray<FString> Lines;
        Text.ParseIntoArrayLines(Lines);
        FString Block;
        for (const FString& Line : Lines)
        {
            if (Line.StartsWith(TEXT("entry ")))
            {
                Block.Reset();
            }
            else if (Line.StartsWith(TEXT("@")) && Line.EndsWith(TEXT(":")))
            {
                Block = Line;
            }
            else if (Line.Contains(Needle))
            {
                return Block;
            }
        }
        return TEXT("<missing>");
    }

    TArray<UK2Node_CallFunction*> FindCalls(UBlueprint* BP, const FName FunctionName)
    {
        TArray<UK2Node_CallFunction*> Calls;
        for (UEdGraph* Graph : BP->FunctionGraphs)
        {
            for (UEdGraphNode* Node : Graph->Nodes)
            {
                UK2Node_CallFunction* Call = Cast<UK2Node_CallFunction>(Node);
                if (Call && Call->FunctionReference.GetMemberName() == FunctionName)
                {
                    Calls.Add(Call);
                }
            }
        }
        return Calls;
    }

    bool CompileInto(FAutomationTestBase& Test, UBlueprint* BP, const FString& Code)
    {
        FBpirCompiler Compiler(BP);
        const FCompileResult Result = Compiler.Compile(Code, EBpirCompileMode::Default);
        for (const FCompileError& Err : Result.Errors)
        {
            Test.AddError(FString::Printf(TEXT("Compile L%d: %s"), Err.Line, *Err.Message));
        }
        return Test.TestTrue(TEXT("BPIR compiles"), Result.bSuccess);
    }

    FString DecompileFunction(FAutomationTestBase& Test, UBlueprint* BP)
    {
        FBpirDecompiler Decompiler(BP);
        const FBpirDecompileResult Result = Decompiler.DecompileFunction(TEXT("HoistProbe"));
        Test.TestTrue(TEXT("Decompile succeeded"), Result.bSuccess);
        return Result.BpirText;
    }

    // Both PrintString calls read the one Conv_IntToString node.
    bool BothPrintsReadOneConversion(FAutomationTestBase& Test, UBlueprint* BP)
    {
        const TArray<UK2Node_CallFunction*> Conversions = FindCalls(BP, TEXT("Conv_IntToString"));
        const TArray<UK2Node_CallFunction*> Prints = FindCalls(BP, TEXT("PrintString"));
        if (!Test.TestEqual(TEXT("One conversion node"), Conversions.Num(), 1)
            || !Test.TestEqual(TEXT("Two PrintString nodes"), Prints.Num(), 2))
        {
            return false;
        }
        bool bAll = true;
        for (UK2Node_CallFunction* Print : Prints)
        {
            UEdGraphPin* InString = Print->FindPin(TEXT("InString"), EGPD_Input);
            bAll &= InString && InString->LinkedTo.Num() == 1
                && InString->LinkedTo[0]->GetOwningNode() == Conversions[0];
        }
        return Test.TestTrue(TEXT("Both PrintString calls read the shared conversion"), bAll);
    }
}

// Fails unfixed: the conversion is bound under the true-branch label (its first consumer)
// although the merge block reads it too.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirPureBindingHoistTest,
    "PinWright.bpir.pure_binding_hoist.CrossBlockReadHoistsToEntryTop",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBpirPureBindingHoistTest::RunTest(const FString& Parameters)
{
    using namespace TestBpirPureBindingHoistHelpers;

    UBlueprint* BP = CompilerTestUtils::CreateTransientTestBP(TEXT("PureBindingHoistBP"));
    if (!TestNotNull(TEXT("Blueprint was created"), BP)) return false;
    if (!CompileInto(*this, BP, SharedPureSource)) return false;
    if (!BothPrintsReadOneConversion(*this, BP)) return false;

    const FString First = DecompileFunction(*this, BP);
    TestEqual(FString::Printf(TEXT("Shared pure binding sits in the entry block (text:\n%s)"), *First),
        BlockOfLineContaining(First, TEXT("Conv_IntToString")), FString());
    TestTrue(TEXT("Shared pure binding precedes the branch"),
        First.Find(TEXT("Conv_IntToString")) < First.Find(TEXT("branch(")));

    // Round trip: the hoisted text rebuilds the same single shared node and is stable.
    UBlueprint* RoundTripBP = CompilerTestUtils::CreateTransientTestBP(TEXT("PureBindingHoistRoundTripBP"));
    if (!TestNotNull(TEXT("Round-trip Blueprint was created"), RoundTripBP)) return false;
    if (!CompileInto(*this, RoundTripBP, First)) return false;
    BothPrintsReadOneConversion(*this, RoundTripBP);
    TestEqual(TEXT("Round-trip decompile is byte-identical"), DecompileFunction(*this, RoundTripBP), First);
    return true;
}

// Guard: a pure binding read only inside its own branch block stays there.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirPureBindingStaysInBlockTest,
    "PinWright.bpir.pure_binding_hoist.SameBlockReadStaysInPlace",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBpirPureBindingStaysInBlockTest::RunTest(const FString& Parameters)
{
    using namespace TestBpirPureBindingHoistHelpers;

    UBlueprint* BP = CompilerTestUtils::CreateTransientTestBP(TEXT("PureBindingStayBP"));
    if (!TestNotNull(TEXT("Blueprint was created"), BP)) return false;
    if (!CompileInto(*this, BP,
        TEXT("entry function HoistProbe(bool bFlag, int Base) {\n")
        TEXT("    %b = branch($bFlag) [true -> @then, false -> @merge]\n")
        TEXT("\n")
        TEXT("@then:\n")
        TEXT("    %s = pure Conv_IntToString(InInt: $Base)\n")
        TEXT("    call PrintString(InString: %s)\n")
        TEXT("    exec -> @merge\n")
        TEXT("\n")
        TEXT("@merge:\n")
        TEXT("    call PrintString(InString: \"tail\")\n")
        TEXT("}\n")))
    {
        return false;
    }

    const FString Text = DecompileFunction(*this, BP);
    const FString Block = BlockOfLineContaining(Text, TEXT("Conv_IntToString"));
    TestTrue(FString::Printf(TEXT("Single-block pure binding stays under its label (text:\n%s)"), *Text),
        Block.StartsWith(TEXT("@")));
    TestEqual(TEXT("It stays in the block of its only consumer"),
        Block, BlockOfLineContaining(Text, TEXT("PrintString(InString: %")));
    return true;
}
