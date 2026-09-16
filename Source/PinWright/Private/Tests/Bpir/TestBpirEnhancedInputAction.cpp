// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "CompilerTestUtils.h"
#include "Tests/TestUtils.h"
#include "Tests/EnhancedInputTestUtils.h"

#include "Compiler/BpirCompiler.h"
#include "Compiler/BpirParser.h"
#include "Compiler/BpirTypes.h"
#include "Compiler/CompilerTypes.h"
#include "Decompiler/BpirDecompiler.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphPin.h"
#include "Engine/Blueprint.h"

using namespace CompilerTestUtils;

namespace
{
    static bool ParseInputActionSource(
        const FString& Source,
        TArray<FBpirEntryBlock>& OutBlocks,
        TArray<FCompileError>& OutErrors)
    {
        FBpirParser Parser;
        return Parser.Parse(Source, OutBlocks, OutErrors, /*bSkipReferenceValidation=*/true);
    }

    static void ReportCompileErrors(FAutomationTestBase& Test, const FCompileResult& Result)
    {
        for (const FCompileError& Error : Result.Errors)
        {
            Test.AddError(FString::Printf(TEXT("Compile L%d: %s"), Error.Line, *Error.Message));
        }
    }

}

using namespace EnhancedInputTestUtils;

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirEnhancedInputActionParserTest,
    "PinWright.BPIR.Parser.EnhancedInputAction",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBpirEnhancedInputActionParserTest::RunTest(const FString& Parameters)
{
    const FString ObjectPath = TEXT("/Game/Input/IA_Move.IA_Move");

    TArray<FBpirEntryBlock> Blocks;
    TArray<FCompileError> Errors;
    const bool bSimpleParsed = ParseInputActionSource(
        FString::Printf(TEXT("entry input_action %s() {\n}"), *ObjectPath),
        Blocks, Errors);
    TestTrue(TEXT("Simple input_action entry parses"), bSimpleParsed);
    TestEqual(TEXT("Simple input_action produces one block"), Blocks.Num(), 1);
    if (Blocks.Num() == 1)
    {
        TestEqual(TEXT("Entry kind is InputAction"), Blocks[0].Kind, EBpirEntryKind::InputAction);
        TestEqual(TEXT("Entry retains the exact object path"), Blocks[0].Name, ObjectPath);
        TestEqual(TEXT("Input action entry has no parameters"), Blocks[0].Params.Num(), 0);
        TestEqual(TEXT("Simple form has no explicit exec targets"), Blocks[0].EntryExecTargets.Num(), 0);
    }

    Blocks.Reset();
    Errors.Reset();
    const bool bMapParsed = ParseInputActionSource(
        FString::Printf(
            TEXT("entry input_action %s() [Triggered -> @triggered, Completed -> @completed] {\n")
            TEXT("@triggered:\n")
            TEXT("    call PrintString(InString: \"triggered\")\n")
            TEXT("@completed:\n")
            TEXT("    call PrintString(InString: \"completed\")\n")
            TEXT("}"),
            *ObjectPath),
        Blocks, Errors);
    TestTrue(TEXT("Mapped input_action entry parses"), bMapParsed);
    if (Blocks.Num() == 1)
    {
        TestEqual(TEXT("Mapped form retains two exec targets"), Blocks[0].EntryExecTargets.Num(), 2);
        if (Blocks[0].EntryExecTargets.Num() == 2)
        {
            TestEqual(TEXT("First event name is Triggered"),
                Blocks[0].EntryExecTargets[0].PinName, FString(TEXT("Triggered")));
            TestEqual(TEXT("First event label is triggered"),
                Blocks[0].EntryExecTargets[0].Label, FString(TEXT("triggered")));
            TestEqual(TEXT("Second event name is Completed"),
                Blocks[0].EntryExecTargets[1].PinName, FString(TEXT("Completed")));
            TestEqual(TEXT("Second event label is completed"),
                Blocks[0].EntryExecTargets[1].Label, FString(TEXT("completed")));
        }
    }

    auto TestRejected = [this](const FString& Description, const FString& Source)
    {
        TArray<FBpirEntryBlock> InvalidBlocks;
        TArray<FCompileError> InvalidErrors;
        TestFalse(*Description,
            ParseInputActionSource(Source, InvalidBlocks, InvalidErrors));
        TestTrue(*(Description + TEXT(" reports an error")), InvalidErrors.Num() > 0);
    };

    TestRejected(TEXT("Missing full object path is rejected"),
        TEXT("entry input_action IA_Move() {\n}"));
    TestRejected(TEXT("Input action parameters are rejected"),
        FString::Printf(TEXT("entry input_action %s(bool Value) {\n}"), *ObjectPath));
    TestRejected(TEXT("Unknown Enhanced Input event is rejected"),
        FString::Printf(
            TEXT("entry input_action %s() [Pressed -> @pressed] {\n@pressed:\n}"),
            *ObjectPath));
    TestRejected(TEXT("Duplicate Enhanced Input event is rejected"),
        FString::Printf(
            TEXT("entry input_action %s() [Triggered -> @first, Triggered -> @second] {\n")
            TEXT("@first:\n@second:\n}"),
            *ObjectPath));

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirEnhancedInputActionRoundTripTest,
    "PinWright.BPIR.RoundTrip.EnhancedInputAction",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBpirEnhancedInputActionRoundTripTest::RunTest(const FString& Parameters)
{
    const FString ActionPackagePath = FString::Printf(
        TEXT("/Game/__PW_GatewayTests/IA_BpirEnhancedInput_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    ON_SCOPE_EXIT
    {
        CleanupTestAsset(ActionPackagePath);
    };

    UInputAction* Action = CreateAxis2DInputAction(ActionPackagePath);
    if (!TestNotNull(TEXT("Axis2D UInputAction fixture created"), Action))
    {
        return true;
    }
    const FString ObjectPath = Action->GetPathName();

    UBlueprint* InvalidMappedBlueprint = CreateTransientTestBP(TEXT("EnhancedInputInvalidMapped"));
    if (!TestNotNull(TEXT("Invalid mapped Blueprint created"), InvalidMappedBlueprint)
        || !TestTrue(TEXT("Invalid mapped Blueprint has an event graph"),
            InvalidMappedBlueprint->UbergraphPages.Num() > 0))
    {
        return true;
    }
    const int32 InvalidNodeCountBefore = InvalidMappedBlueprint->UbergraphPages[0]->Nodes.Num();
    const FString InvalidMappedSource = FString::Printf(
        TEXT("entry input_action %s() [Triggered -> @triggered] {\n")
        TEXT("    call PrintString(InString: \"must not be emitted\")\n")
        TEXT("@triggered:\n")
        TEXT("    call PrintString(InString: \"valid branch\")\n")
        TEXT("}"),
        *ObjectPath);
    FBpirCompiler InvalidMappedCompiler(InvalidMappedBlueprint);
    const FCompileResult InvalidMappedCompile = InvalidMappedCompiler.Compile(InvalidMappedSource);
    TestFalse(TEXT("Mapped input_action rejects statements before the first label"),
        InvalidMappedCompile.bSuccess);
    bool bFoundPreLabelError = false;
    for (const FCompileError& Error : InvalidMappedCompile.Errors)
    {
        if (Error.Line == 2 && Error.Message.Contains(TEXT("put statements under a label")))
        {
            bFoundPreLabelError = true;
            break;
        }
    }
    TestTrue(TEXT("Pre-label error reports statement line and label guidance"),
        bFoundPreLabelError);
    TestEqual(TEXT("Invalid mapped input_action leaves graph node count unchanged"),
        InvalidMappedBlueprint->UbergraphPages[0]->Nodes.Num(), InvalidNodeCountBefore);
    TestEqual(TEXT("Invalid mapped input_action emits no Enhanced Input root"),
        CountEnhancedInputActionNodes(InvalidMappedBlueprint), 0);

    UBlueprint* SourceBlueprint = CreateTransientTestBP(TEXT("EnhancedInputSource"));
    if (!TestNotNull(TEXT("Source Blueprint created"), SourceBlueprint))
    {
        return true;
    }

    const FString SimpleSource = FString::Printf(
        TEXT("entry input_action %s() {\n")
        TEXT("    call PrintString(InString: \"triggered\")\n")
        TEXT("}"),
        *ObjectPath);
    FBpirCompiler SimpleCompiler(SourceBlueprint);
    const FCompileResult SimpleCompile = SimpleCompiler.Compile(SimpleSource);
    if (!SimpleCompile.bSuccess)
    {
        ReportCompileErrors(*this, SimpleCompile);
    }
    TestTrue(TEXT("Simple input_action BPIR compiles"), SimpleCompile.bSuccess);

    UEdGraphNode* SourceNode = FindEnhancedInputActionNode(SourceBlueprint);
    TestAxis2DNodeShape(*this, SourceNode, Action);
    if (SourceNode)
    {
        UEdGraphPin* Triggered = SourceNode->FindPin(TEXT("Triggered"), EGPD_Output);
        TestNotNull(TEXT("Triggered exec output exists"), Triggered);
        if (Triggered)
        {
            TestTrue(TEXT("Simple body is connected from Triggered"), Triggered->LinkedTo.Num() > 0);
        }
    }

    FBpirCompiler SimpleRetryCompiler(SourceBlueprint);
    const FCompileResult SimpleRetryCompile = SimpleRetryCompiler.Compile(
        SimpleSource, EBpirCompileMode::Replace);
    if (!SimpleRetryCompile.bSuccess)
    {
        ReportCompileErrors(*this, SimpleRetryCompile);
    }
    TestTrue(TEXT("Same input_action BPIR recompiles in upsert mode"), SimpleRetryCompile.bSuccess);
    TestEqual(TEXT("Upsert leaves exactly one Enhanced Input root"),
        CountEnhancedInputActionNodes(SourceBlueprint), 1);
    TestEqual(TEXT("Upsert leaves exactly one root bound to the action"),
        CountEnhancedInputActionNodes(SourceBlueprint, Action), 1);
    UEdGraphNode* RetriedSourceNode = FindEnhancedInputActionNode(SourceBlueprint);
    TestAxis2DNodeShape(*this, RetriedSourceNode, Action);

    FBpirDecompiler SimpleDecompiler(SourceBlueprint);
    const FBpirDecompileResult SimpleDecompile = SimpleDecompiler.Decompile();
    TestTrue(TEXT("Simple input_action decompiles"), SimpleDecompile.bSuccess);
    TestTrue(TEXT("Simple decompile retains exact input_action object path"),
        SimpleDecompile.BpirText.Contains(
            FString::Printf(TEXT("entry input_action %s()"), *ObjectPath)));
    TestFalse(TEXT("Simple decompile does not emit UnknownEntry"),
        SimpleDecompile.BpirText.Contains(TEXT("UnknownEntry")));

    UBlueprint* SimpleRoundTripBlueprint = CreateTransientTestBP(TEXT("EnhancedInputSimpleRoundTrip"));
    if (!TestNotNull(TEXT("Simple round-trip Blueprint created"), SimpleRoundTripBlueprint))
    {
        return true;
    }
    FBpirCompiler SimpleRoundTripCompiler(SimpleRoundTripBlueprint);
    const FCompileResult SimpleRoundTripCompile = SimpleRoundTripCompiler.Compile(SimpleDecompile.BpirText);
    if (!SimpleRoundTripCompile.bSuccess)
    {
        ReportCompileErrors(*this, SimpleRoundTripCompile);
    }
    TestTrue(TEXT("Decompiled simple BPIR recompiles"), SimpleRoundTripCompile.bSuccess);
    UEdGraphNode* SimpleRoundTripNode = FindEnhancedInputActionNode(SimpleRoundTripBlueprint);
    TestAxis2DNodeShape(*this, SimpleRoundTripNode, Action);
    TestTrue(TEXT("Simple round-trip Triggered chain is connected"),
        SimpleRoundTripNode
        && SimpleRoundTripNode->FindPin(TEXT("Triggered"), EGPD_Output)
        && SimpleRoundTripNode->FindPin(TEXT("Triggered"), EGPD_Output)->LinkedTo.Num() > 0);

    const FString MappedSource = FString::Printf(
        TEXT("entry input_action %s() [Triggered -> @triggered, Completed -> @completed] {\n")
        TEXT("@triggered:\n")
        TEXT("    call PrintString(InString: \"triggered\")\n")
        TEXT("@completed:\n")
        TEXT("    call PrintString(InString: \"completed\")\n")
        TEXT("}"),
        *ObjectPath);
    UBlueprint* MappedSourceBlueprint = CreateTransientTestBP(TEXT("EnhancedInputMappedSource"));
    if (!TestNotNull(TEXT("Mapped source Blueprint created"), MappedSourceBlueprint))
    {
        return true;
    }
    FBpirCompiler MappedCompiler(MappedSourceBlueprint);
    const FCompileResult MappedCompile = MappedCompiler.Compile(MappedSource);
    if (!MappedCompile.bSuccess)
    {
        ReportCompileErrors(*this, MappedCompile);
    }
    TestTrue(TEXT("Mapped input_action BPIR compiles"), MappedCompile.bSuccess);

    UEdGraphNode* MappedSourceNode = FindEnhancedInputActionNode(MappedSourceBlueprint);
    TestAxis2DNodeShape(*this, MappedSourceNode, Action);
    if (MappedSourceNode)
    {
        UEdGraphPin* Triggered = MappedSourceNode->FindPin(TEXT("Triggered"), EGPD_Output);
        UEdGraphPin* Completed = MappedSourceNode->FindPin(TEXT("Completed"), EGPD_Output);
        TestNotNull(TEXT("Mapped Triggered output exists"), Triggered);
        TestNotNull(TEXT("Mapped Completed output exists"), Completed);
        if (Triggered)
        {
            TestTrue(TEXT("Mapped Triggered output is connected"), Triggered->LinkedTo.Num() > 0);
        }
        if (Completed)
        {
            TestTrue(TEXT("Mapped Completed output is connected"), Completed->LinkedTo.Num() > 0);
        }
    }

    FBpirDecompiler MappedDecompiler(MappedSourceBlueprint);
    const FBpirDecompileResult MappedDecompile = MappedDecompiler.Decompile();
    TestTrue(TEXT("Mapped input_action decompiles"), MappedDecompile.bSuccess);
    const FString ExpectedMap = FString::Printf(
        TEXT("entry input_action %s() [Triggered -> @triggered, Completed -> @completed]"),
        *ObjectPath);
    TestTrue(TEXT("Decompiler preserves Triggered and Completed event roots"),
        MappedDecompile.BpirText.Contains(ExpectedMap));
    TestFalse(TEXT("Mapped decompile does not emit UnknownEntry"),
        MappedDecompile.BpirText.Contains(TEXT("UnknownEntry")));

    UBlueprint* RoundTripBlueprint = CreateTransientTestBP(TEXT("EnhancedInputRoundTrip"));
    if (!TestNotNull(TEXT("Round-trip Blueprint created"), RoundTripBlueprint))
    {
        return true;
    }
    FBpirCompiler RoundTripCompiler(RoundTripBlueprint);
    const FCompileResult RoundTripCompile = RoundTripCompiler.Compile(MappedDecompile.BpirText);
    if (!RoundTripCompile.bSuccess)
    {
        ReportCompileErrors(*this, RoundTripCompile);
    }
    TestTrue(TEXT("Decompiled mapped BPIR recompiles"), RoundTripCompile.bSuccess);

    UEdGraphNode* RoundTripNode = FindEnhancedInputActionNode(RoundTripBlueprint);
    TestAxis2DNodeShape(*this, RoundTripNode, Action);
    if (RoundTripNode)
    {
        UEdGraphPin* Triggered = RoundTripNode->FindPin(TEXT("Triggered"), EGPD_Output);
        UEdGraphPin* Completed = RoundTripNode->FindPin(TEXT("Completed"), EGPD_Output);
        TestTrue(TEXT("Round-trip Triggered chain is connected"),
            Triggered && Triggered->LinkedTo.Num() > 0);
        TestTrue(TEXT("Round-trip Completed chain is connected"),
            Completed && Completed->LinkedTo.Num() > 0);
    }

    return true;
}
