// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Misc/AutomationTest.h"
#include "CompilerTestUtils.h"

#include "Compiler/BpirParser.h"
#include "Compiler/BpirTypeSpecParser.h"
#include "Compiler/CompilerTypes.h"
#include "Decompiler/BpirDecompiler.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_FunctionResult.h"

using namespace CompilerTestUtils;

namespace
{
    UEdGraph* FindSingleNamedOutputFunctionGraph(UBlueprint* Blueprint, const FString& Name)
    {
        if (!Blueprint)
        {
            return nullptr;
        }

        for (UEdGraph* Graph : Blueprint->FunctionGraphs)
        {
            if (Graph && Graph->GetName().Equals(Name, ESearchCase::IgnoreCase))
            {
                return Graph;
            }
        }
        return nullptr;
    }

    TArray<UEdGraphPin*> CollectSingleNamedOutputResultPins(UEdGraph* Graph)
    {
        TArray<UEdGraphPin*> Pins;
        UK2Node_FunctionResult* ResultNode = FindNodeOfType<UK2Node_FunctionResult>(Graph);
        if (!ResultNode)
        {
            return Pins;
        }

        for (UEdGraphPin* Pin : ResultNode->Pins)
        {
            if (Pin && Pin->Direction == EGPD_Input
                && Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec
                && !Pin->bHidden)
            {
                Pins.Add(Pin);
            }
        }
        return Pins;
    }

    void ReportSingleNamedOutputErrors(
        FAutomationTestBase& Test,
        const FString& Phase,
        const TArray<FCompileError>& Errors)
    {
        for (const FCompileError& Error : Errors)
        {
            Test.AddError(FString::Printf(TEXT("%s L%d: %s"), *Phase, Error.Line, *Error.Message));
        }
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirParserSingleNamedOutputDistinctionTest,
    "PinWright.bpir.parser.SingleNamedOutputDistinction",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBpirParserSingleNamedOutputDistinctionTest::RunTest(const FString& Parameters)
{
    TArray<FBpirEntryBlock> NamedBlocks;
    TArray<FCompileError> NamedErrors;
    FBpirParser NamedParser;
    const bool bNamedParsed = NamedParser.Parse(
        TEXT("entry function GetHealth() -> (float Health01) {}"),
        NamedBlocks,
        NamedErrors,
        /*bSkipReferenceValidation=*/true);

    if (!bNamedParsed)
    {
        ReportSingleNamedOutputErrors(*this, TEXT("Named parse"), NamedErrors);
    }
    TestTrue(TEXT("Parenthesized single named output parses"), bNamedParsed);
    TestEqual(TEXT("Named parse produces one entry"), NamedBlocks.Num(), 1);
    if (NamedBlocks.Num() == 1)
    {
        const FBpirEntryBlock& NamedBlock = NamedBlocks[0];
        TestTrue(TEXT("Named output does not populate ReturnType"), NamedBlock.ReturnType.IsEmpty());
        TestEqual(TEXT("Named output populates one OutputParams entry"), NamedBlock.OutputParams.Num(), 1);
        if (NamedBlock.OutputParams.Num() == 1)
        {
            TestEqual(TEXT("Named output preserves its name"), NamedBlock.OutputParams[0].Name, TEXT("Health01"));
            TestEqual(TEXT("Named output preserves its type"),
                BpirTypeSpecParser::TypeSpecToBpirText(NamedBlock.OutputParams[0].Type),
                FString(TEXT("float")));
        }
    }

    TArray<FBpirEntryBlock> ShorthandBlocks;
    TArray<FCompileError> ShorthandErrors;
    FBpirParser ShorthandParser;
    const bool bShorthandParsed = ShorthandParser.Parse(
        TEXT("entry function GetHealth() -> float {}"),
        ShorthandBlocks,
        ShorthandErrors,
        /*bSkipReferenceValidation=*/true);

    if (!bShorthandParsed)
    {
        ReportSingleNamedOutputErrors(*this, TEXT("Shorthand parse"), ShorthandErrors);
    }
    TestTrue(TEXT("Bare single-output shorthand parses"), bShorthandParsed);
    TestEqual(TEXT("Shorthand parse produces one entry"), ShorthandBlocks.Num(), 1);
    if (ShorthandBlocks.Num() == 1)
    {
        const FBpirEntryBlock& ShorthandBlock = ShorthandBlocks[0];
        TestEqual(TEXT("Shorthand populates ReturnType"),
            BpirTypeSpecParser::TypeSpecToBpirText(ShorthandBlock.ReturnType),
            FString(TEXT("float")));
        TestEqual(TEXT("Shorthand does not populate OutputParams"), ShorthandBlock.OutputParams.Num(), 0);
    }

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirSingleNamedOutputRoundTripTest,
    "PinWright.bpir.round_trip.SingleNamedOutputPreserved",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBpirSingleNamedOutputRoundTripTest::RunTest(const FString& Parameters)
{
    static const TCHAR* NamedSource =
        TEXT("entry function GetHealth() -> (float Health01) {\n")
        TEXT("    return (Health01: 0.5)\n")
        TEXT("}\n");

    UBlueprint* NamedBlueprint = CreateTransientTestBP(TEXT("SingleNamedOutputBP"));
    TestNotNull(TEXT("Named-output Blueprint created"), NamedBlueprint);
    if (!NamedBlueprint) return false;

    FBpirCompiler NamedCompiler(NamedBlueprint);
    FCompileResult NamedCompile = NamedCompiler.Compile(NamedSource);
    if (!NamedCompile.bSuccess)
    {
        ReportSingleNamedOutputErrors(*this, TEXT("Named compile"), NamedCompile.Errors);
    }
    TestTrue(TEXT("Named-output function compiles"), NamedCompile.bSuccess);
    if (!NamedCompile.bSuccess) return false;

    UEdGraph* NamedGraph = FindSingleNamedOutputFunctionGraph(NamedBlueprint, TEXT("GetHealth"));
    TestNotNull(TEXT("Named-output function graph exists"), NamedGraph);
    if (!NamedGraph) return false;

    TArray<UEdGraphPin*> OriginalPins = CollectSingleNamedOutputResultPins(NamedGraph);
    TestEqual(TEXT("Original graph has one result data pin"), OriginalPins.Num(), 1);
    if (OriginalPins.Num() == 1)
    {
        TestEqual(TEXT("Compiler preserves the authored output name"), OriginalPins[0]->PinName, FName(TEXT("Health01")));
    }

    FBpirDecompiler NamedDecompiler(NamedBlueprint);
    FBpirDecompileResult NamedDecompile = NamedDecompiler.DecompileFunction(TEXT("GetHealth"));
    TestTrue(TEXT("Named-output function decompiles"), NamedDecompile.bSuccess);
    if (!NamedDecompile.bSuccess) return false;

    TestTrue(TEXT("Decompiler emits the parenthesized named form"),
        NamedDecompile.BpirText.Contains(TEXT("-> (float Health01)")));
    TestFalse(TEXT("Decompiler does not collapse the named output to shorthand"),
        NamedDecompile.BpirText.Contains(TEXT("-> float")));

    UBlueprint* RecompiledBlueprint = CreateTransientTestBP(TEXT("SingleNamedOutputRecompiledBP"));
    TestNotNull(TEXT("Recompile target Blueprint created"), RecompiledBlueprint);
    if (!RecompiledBlueprint) return false;

    FBpirCompiler Recompiler(RecompiledBlueprint);
    FCompileResult Recompile = Recompiler.Compile(NamedDecompile.BpirText);
    if (!Recompile.bSuccess)
    {
        ReportSingleNamedOutputErrors(*this, TEXT("Named recompile"), Recompile.Errors);
    }
    TestTrue(TEXT("Decompiled named-output function recompiles"), Recompile.bSuccess);
    if (!Recompile.bSuccess) return false;

    UEdGraph* RecompiledGraph = FindSingleNamedOutputFunctionGraph(RecompiledBlueprint, TEXT("GetHealth"));
    TestNotNull(TEXT("Recompiled function graph exists"), RecompiledGraph);
    if (!RecompiledGraph) return false;

    TArray<UEdGraphPin*> RecompiledPins = CollectSingleNamedOutputResultPins(RecompiledGraph);
    TestEqual(TEXT("Recompiled graph has one result data pin"), RecompiledPins.Num(), 1);
    if (RecompiledPins.Num() == 1)
    {
        UEdGraphPin* RecompiledPin = RecompiledPins[0];
        TestEqual(TEXT("Named output survives decompile and recompile"), RecompiledPin->PinName, FName(TEXT("Health01")));
        TestEqual(TEXT("Recompiled named output remains a real pin"),
            RecompiledPin->PinType.PinCategory, UEdGraphSchema_K2::PC_Real);
        TestEqual(TEXT("Recompiled named output remains float"),
            RecompiledPin->PinType.PinSubCategory, UEdGraphSchema_K2::PC_Float);
    }

    static const TCHAR* ConventionalSource =
        TEXT("entry function GetValue() -> float {\n")
        TEXT("    return 0.25\n")
        TEXT("}\n");

    UBlueprint* ConventionalBlueprint = CreateTransientTestBP(TEXT("ConventionalReturnValueBP"));
    TestNotNull(TEXT("Conventional-return Blueprint created"), ConventionalBlueprint);
    if (!ConventionalBlueprint) return false;

    FBpirCompiler ConventionalCompiler(ConventionalBlueprint);
    FCompileResult ConventionalCompile = ConventionalCompiler.Compile(ConventionalSource);
    if (!ConventionalCompile.bSuccess)
    {
        ReportSingleNamedOutputErrors(*this, TEXT("Conventional compile"), ConventionalCompile.Errors);
    }
    TestTrue(TEXT("Bare ReturnValue shorthand still compiles"), ConventionalCompile.bSuccess);
    if (!ConventionalCompile.bSuccess) return false;

    UEdGraph* ConventionalGraph = FindSingleNamedOutputFunctionGraph(ConventionalBlueprint, TEXT("GetValue"));
    TArray<UEdGraphPin*> ConventionalPins = CollectSingleNamedOutputResultPins(ConventionalGraph);
    TestEqual(TEXT("Conventional graph has one result data pin"), ConventionalPins.Num(), 1);
    if (ConventionalPins.Num() == 1)
    {
        TestEqual(TEXT("Bare shorthand creates the conventional ReturnValue pin"),
            ConventionalPins[0]->PinName, UEdGraphSchema_K2::PN_ReturnValue);
    }

    FBpirDecompiler ConventionalDecompiler(ConventionalBlueprint);
    FBpirDecompileResult ConventionalDecompile = ConventionalDecompiler.DecompileFunction(TEXT("GetValue"));
    TestTrue(TEXT("Conventional-return function decompiles"), ConventionalDecompile.bSuccess);
    if (!ConventionalDecompile.bSuccess) return false;

    TestTrue(TEXT("Conventional ReturnValue keeps compact shorthand"),
        ConventionalDecompile.BpirText.Contains(TEXT("-> float")));
    TestFalse(TEXT("Conventional ReturnValue is not expanded to the named form"),
        ConventionalDecompile.BpirText.Contains(TEXT("-> (float ReturnValue)")));

    return true;
}
