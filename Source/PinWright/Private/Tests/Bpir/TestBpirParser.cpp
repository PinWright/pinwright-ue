// Copyright (c) 2026 Alexander Penkin. MIT License.

// TestBpirParser.cpp - Unit tests for FBpirParser (pure text parsing, no UE graph dependencies)

#include "Misc/AutomationTest.h"
#include "Compiler/BpirParser.h"
#include "Compiler/BpirTypes.h"
#include "Compiler/BpirTypeSpec.h"
#include "Compiler/BpirTypeSpecParser.h"
#include "Compiler/CompilerTypes.h"

// Helper: parse a single instruction inside an entry block, return the block
static bool ParseSingleInstruction(
    FAutomationTestBase& Test,
    const FString& InstructionLine,
    TArray<FBpirEntryBlock>& OutBlocks,
    TArray<FCompileError>& OutErrors)
{
    FString Code = FString::Printf(TEXT("entry event BeginPlay() {\n    %s\n}"), *InstructionLine);
    FBpirParser Parser;
    return Parser.Parse(Code, OutBlocks, OutErrors, /*bSkipReferenceValidation=*/ true);
}

// Helper: parse multiple instruction lines inside an entry block
static bool ParseMultipleInstructions(
    FAutomationTestBase& Test,
    const FString& BodyLines,
    TArray<FBpirEntryBlock>& OutBlocks,
    TArray<FCompileError>& OutErrors)
{
    FString Code = FString::Printf(TEXT("entry event BeginPlay() {\n%s\n}"), *BodyLines);
    FBpirParser Parser;
    return Parser.Parse(Code, OutBlocks, OutErrors);
}

// ============================================================================
// 1. ParseCall
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirParserParseCallTest,
    "PinWright.bpir.parser.ParseCall",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBpirParserParseCallTest::RunTest(const FString& Parameters)
{
    TArray<FBpirEntryBlock> Blocks;
    TArray<FCompileError> Errors;
    bool bOk = ParseSingleInstruction(*this, TEXT("call PrintString(InString: \"Hello\")"), Blocks, Errors);

    TestTrue(TEXT("Parse succeeded"), bOk);
    TestEqual(TEXT("1 block"), Blocks.Num(), 1);

    if (Blocks.Num() > 0)
    {
        TestTrue(TEXT("At least 1 instruction"), Blocks[0].Instructions.Num() >= 1);
        if (Blocks[0].Instructions.Num() >= 1)
        {
            const FBpirInstruction& Inst = Blocks[0].Instructions[0];
            TestEqual(TEXT("Opcode is Call"), Inst.Opcode, EBpirOpcode::Call);
            TestEqual(TEXT("FunctionName is PrintString"), Inst.FunctionName, TEXT("PrintString"));
            TestTrue(TEXT("Has at least 1 arg"), Inst.Args.Num() >= 1);
            if (Inst.Args.Num() >= 1)
            {
                TestEqual(TEXT("Arg PinName"), Inst.Args[0].PinName, TEXT("InString"));
                TestEqual(TEXT("Arg Value"), Inst.Args[0].Value, TEXT("\"Hello\""));
            }
        }
    }

    return true;
}

// ============================================================================
// 2. ParsePure
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirParserParsePureTest,
    "PinWright.bpir.parser.ParsePure",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBpirParserParsePureTest::RunTest(const FString& Parameters)
{
    TArray<FBpirEntryBlock> Blocks;
    TArray<FCompileError> Errors;
    bool bOk = ParseSingleInstruction(*this, TEXT("%x = pure GetActorLocation(Target: self)"), Blocks, Errors);

    TestTrue(TEXT("Parse succeeded"), bOk);
    TestEqual(TEXT("1 block"), Blocks.Num(), 1);

    if (Blocks.Num() > 0 && Blocks[0].Instructions.Num() >= 1)
    {
        const FBpirInstruction& Inst = Blocks[0].Instructions[0];
        TestEqual(TEXT("Opcode is Pure"), Inst.Opcode, EBpirOpcode::Pure);
        TestEqual(TEXT("ResultName is x"), Inst.ResultName, TEXT("x"));
        TestEqual(TEXT("FunctionName is GetActorLocation"), Inst.FunctionName, TEXT("GetActorLocation"));
        TestTrue(TEXT("Has at least 1 arg"), Inst.Args.Num() >= 1);
        if (Inst.Args.Num() >= 1)
        {
            TestEqual(TEXT("Arg PinName"), Inst.Args[0].PinName, TEXT("Target"));
            TestEqual(TEXT("Arg Value"), Inst.Args[0].Value, TEXT("self"));
        }
    }

    return true;
}

// ============================================================================
// 3. ParseLatent
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirParserParseLatentTest,
    "PinWright.bpir.parser.ParseLatent",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBpirParserParseLatentTest::RunTest(const FString& Parameters)
{
    TArray<FBpirEntryBlock> Blocks;
    TArray<FCompileError> Errors;
    bool bOk = ParseSingleInstruction(*this, TEXT("%d = latent Delay(Duration: 2.0) [completed -> @c]"), Blocks, Errors);

    TestTrue(TEXT("Parse succeeded"), bOk);
    TestEqual(TEXT("1 block"), Blocks.Num(), 1);

    if (Blocks.Num() > 0 && Blocks[0].Instructions.Num() >= 1)
    {
        const FBpirInstruction& Inst = Blocks[0].Instructions[0];
        TestEqual(TEXT("Opcode is Latent"), Inst.Opcode, EBpirOpcode::Latent);
        TestEqual(TEXT("ResultName is d"), Inst.ResultName, TEXT("d"));
        TestTrue(TEXT("Has exec targets"), Inst.ExecTargets.Num() >= 1);
        if (Inst.ExecTargets.Num() >= 1)
        {
            TestEqual(TEXT("ExecTarget PinName"), Inst.ExecTargets[0].PinName, TEXT("completed"));
            TestEqual(TEXT("ExecTarget Label"), Inst.ExecTargets[0].Label, TEXT("c"));
        }
    }

    return true;
}

// ============================================================================
// 4. ParseBranch
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirParserParseBranchTest,
    "PinWright.bpir.parser.ParseBranch",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBpirParserParseBranchTest::RunTest(const FString& Parameters)
{
    TArray<FBpirEntryBlock> Blocks;
    TArray<FCompileError> Errors;
    bool bOk = ParseSingleInstruction(*this, TEXT("%b = branch($cond) [true -> @t, false -> @f]"), Blocks, Errors);

    TestTrue(TEXT("Parse succeeded"), bOk);
    TestEqual(TEXT("1 block"), Blocks.Num(), 1);

    if (Blocks.Num() > 0 && Blocks[0].Instructions.Num() >= 1)
    {
        const FBpirInstruction& Inst = Blocks[0].Instructions[0];
        TestEqual(TEXT("Opcode is Branch"), Inst.Opcode, EBpirOpcode::Branch);
        TestEqual(TEXT("ResultName is b"), Inst.ResultName, TEXT("b"));

        // Verify exec targets
        TestEqual(TEXT("2 exec targets"), Inst.ExecTargets.Num(), 2);
        if (Inst.ExecTargets.Num() >= 2)
        {
            TestEqual(TEXT("First target PinName"), Inst.ExecTargets[0].PinName, TEXT("true"));
            TestEqual(TEXT("First target Label"), Inst.ExecTargets[0].Label, TEXT("t"));
            TestEqual(TEXT("Second target PinName"), Inst.ExecTargets[1].PinName, TEXT("false"));
            TestEqual(TEXT("Second target Label"), Inst.ExecTargets[1].Label, TEXT("f"));
        }
    }

    return true;
}

// ============================================================================
// 5. ParseForeach
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirParserParseForeachTest,
    "PinWright.bpir.parser.ParseForeach",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBpirParserParseForeachTest::RunTest(const FString& Parameters)
{
    TArray<FBpirEntryBlock> Blocks;
    TArray<FCompileError> Errors;
    bool bOk = ParseSingleInstruction(*this, TEXT("%loop = foreach($arr) [body -> @b, completed -> @c]"), Blocks, Errors);

    TestTrue(TEXT("Parse succeeded"), bOk);
    TestEqual(TEXT("1 block"), Blocks.Num(), 1);

    if (Blocks.Num() > 0 && Blocks[0].Instructions.Num() >= 1)
    {
        const FBpirInstruction& Inst = Blocks[0].Instructions[0];
        TestEqual(TEXT("Opcode is Foreach"), Inst.Opcode, EBpirOpcode::Foreach);
        TestEqual(TEXT("ResultName is loop"), Inst.ResultName, TEXT("loop"));

        TestEqual(TEXT("2 exec targets"), Inst.ExecTargets.Num(), 2);
        if (Inst.ExecTargets.Num() >= 2)
        {
            TestEqual(TEXT("body target"), Inst.ExecTargets[0].PinName, TEXT("body"));
            TestEqual(TEXT("body label"), Inst.ExecTargets[0].Label, TEXT("b"));
            TestEqual(TEXT("completed target"), Inst.ExecTargets[1].PinName, TEXT("completed"));
            TestEqual(TEXT("completed label"), Inst.ExecTargets[1].Label, TEXT("c"));
        }
    }

    return true;
}

// ============================================================================
// 6. ParseSet
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirParserParseSetTest,
    "PinWright.bpir.parser.ParseSet",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBpirParserParseSetTest::RunTest(const FString& Parameters)
{
    TArray<FBpirEntryBlock> Blocks;
    TArray<FCompileError> Errors;
    bool bOk = ParseSingleInstruction(*this, TEXT("set Speed = 100.0"), Blocks, Errors);

    TestTrue(TEXT("Parse succeeded"), bOk);
    TestEqual(TEXT("1 block"), Blocks.Num(), 1);

    if (Blocks.Num() > 0 && Blocks[0].Instructions.Num() >= 1)
    {
        const FBpirInstruction& Inst = Blocks[0].Instructions[0];
        TestEqual(TEXT("Opcode is Set"), Inst.Opcode, EBpirOpcode::Set);
        TestEqual(TEXT("FunctionName is Speed"), Inst.FunctionName, TEXT("Speed"));
        TestTrue(TEXT("Has 1 arg"), Inst.Args.Num() >= 1);
        if (Inst.Args.Num() >= 1)
        {
            TestEqual(TEXT("Arg Value"), Inst.Args[0].Value, TEXT("100.0"));
        }
    }

    return true;
}

// ============================================================================
// 7. ParseLabel
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirParserParseLabelTest,
    "PinWright.bpir.parser.ParseLabel",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBpirParserParseLabelTest::RunTest(const FString& Parameters)
{
    TArray<FBpirEntryBlock> Blocks;
    TArray<FCompileError> Errors;
    bool bOk = ParseSingleInstruction(*this, TEXT("@myLabel:"), Blocks, Errors);

    TestTrue(TEXT("Parse succeeded"), bOk);
    TestEqual(TEXT("1 block"), Blocks.Num(), 1);

    if (Blocks.Num() > 0 && Blocks[0].Instructions.Num() >= 1)
    {
        const FBpirInstruction& Inst = Blocks[0].Instructions[0];
        TestEqual(TEXT("Opcode is Label"), Inst.Opcode, EBpirOpcode::Label);
        TestEqual(TEXT("FunctionName is myLabel"), Inst.FunctionName, TEXT("myLabel"));
    }

    return true;
}

// ============================================================================
// 8. ParseExecGoto
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirParserParseExecGotoTest,
    "PinWright.bpir.parser.ParseExecGoto",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBpirParserParseExecGotoTest::RunTest(const FString& Parameters)
{
    TArray<FBpirEntryBlock> Blocks;
    TArray<FCompileError> Errors;
    bool bOk = ParseSingleInstruction(*this, TEXT("exec -> @done"), Blocks, Errors);

    TestTrue(TEXT("Parse succeeded"), bOk);
    TestEqual(TEXT("1 block"), Blocks.Num(), 1);

    if (Blocks.Num() > 0 && Blocks[0].Instructions.Num() >= 1)
    {
        const FBpirInstruction& Inst = Blocks[0].Instructions[0];
        TestEqual(TEXT("Opcode is ExecGoto"), Inst.Opcode, EBpirOpcode::ExecGoto);
        TestTrue(TEXT("Has 1 exec target"), Inst.ExecTargets.Num() >= 1);
        if (Inst.ExecTargets.Num() >= 1)
        {
            TestEqual(TEXT("ExecTarget Label is done"), Inst.ExecTargets[0].Label, TEXT("done"));
        }
    }

    return true;
}

// ============================================================================
// 9. ParseEnum
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirParserParseEnumTest,
    "PinWright.bpir.parser.ParseEnum",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBpirParserParseEnumTest::RunTest(const FString& Parameters)
{
    TArray<FBpirEntryBlock> Blocks;
    TArray<FCompileError> Errors;
    bool bOk = ParseSingleInstruction(*this, TEXT("%e = enum EType::Value"), Blocks, Errors);

    TestTrue(TEXT("Parse succeeded"), bOk);
    TestEqual(TEXT("1 block"), Blocks.Num(), 1);

    if (Blocks.Num() > 0 && Blocks[0].Instructions.Num() >= 1)
    {
        const FBpirInstruction& Inst = Blocks[0].Instructions[0];
        TestEqual(TEXT("Opcode is Enum"), Inst.Opcode, EBpirOpcode::Enum);
        TestEqual(TEXT("TypeArg is EType::Value"), Inst.TypeArg, TEXT("EType::Value"));
        TestEqual(TEXT("ResultName is e"), Inst.ResultName, TEXT("e"));
    }

    return true;
}

// ============================================================================
// 10. ParseCast
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirParserParseCastTest,
    "PinWright.bpir.parser.ParseCast",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBpirParserParseCastTest::RunTest(const FString& Parameters)
{
    TArray<FBpirEntryBlock> Blocks;
    TArray<FCompileError> Errors;
    bool bOk = ParseSingleInstruction(*this, TEXT("%c = cast<Character>(%obj) [success -> @ok, fail -> @no]"), Blocks, Errors);

    TestTrue(TEXT("Parse succeeded"), bOk);
    TestEqual(TEXT("1 block"), Blocks.Num(), 1);

    if (Blocks.Num() > 0 && Blocks[0].Instructions.Num() >= 1)
    {
        const FBpirInstruction& Inst = Blocks[0].Instructions[0];
        TestEqual(TEXT("Opcode is Cast"), Inst.Opcode, EBpirOpcode::Cast);
        TestEqual(TEXT("TypeArg is Character"), Inst.TypeArg, TEXT("Character"));
        TestEqual(TEXT("ResultName is c"), Inst.ResultName, TEXT("c"));

        // Verify args contain the object to cast
        TestTrue(TEXT("Has at least 1 arg"), Inst.Args.Num() >= 1);
        if (Inst.Args.Num() >= 1)
        {
            TestEqual(TEXT("Arg Value is %obj"), Inst.Args[0].Value, TEXT("%obj"));
        }

        // Verify exec targets
        TestEqual(TEXT("2 exec targets"), Inst.ExecTargets.Num(), 2);
        if (Inst.ExecTargets.Num() >= 2)
        {
            TestEqual(TEXT("success target"), Inst.ExecTargets[0].PinName, TEXT("success"));
            TestEqual(TEXT("success label"), Inst.ExecTargets[0].Label, TEXT("ok"));
            TestEqual(TEXT("fail target"), Inst.ExecTargets[1].PinName, TEXT("fail"));
            TestEqual(TEXT("fail label"), Inst.ExecTargets[1].Label, TEXT("no"));
        }
    }

    return true;
}

// ============================================================================
// 11. ParseMakeStruct
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirParserParseMakeStructTest,
    "PinWright.bpir.parser.ParseMakeStruct",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBpirParserParseMakeStructTest::RunTest(const FString& Parameters)
{
    TArray<FBpirEntryBlock> Blocks;
    TArray<FCompileError> Errors;
    bool bOk = ParseSingleInstruction(*this, TEXT("%v = make<Vector>(X: 1.0)"), Blocks, Errors);

    TestTrue(TEXT("Parse succeeded"), bOk);
    TestEqual(TEXT("1 block"), Blocks.Num(), 1);

    if (Blocks.Num() > 0 && Blocks[0].Instructions.Num() >= 1)
    {
        const FBpirInstruction& Inst = Blocks[0].Instructions[0];
        TestEqual(TEXT("Opcode is MakeStruct"), Inst.Opcode, EBpirOpcode::MakeStruct);
        TestEqual(TEXT("TypeArg is Vector"), Inst.TypeArg, TEXT("Vector"));
        TestEqual(TEXT("ResultName is v"), Inst.ResultName, TEXT("v"));
        TestTrue(TEXT("Has at least 1 arg"), Inst.Args.Num() >= 1);
        if (Inst.Args.Num() >= 1)
        {
            TestEqual(TEXT("Arg PinName"), Inst.Args[0].PinName, TEXT("X"));
            TestEqual(TEXT("Arg Value"), Inst.Args[0].Value, TEXT("1.0"));
        }
    }

    return true;
}

// ============================================================================
// 12. ParseBreakStruct
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirParserParseBreakStructTest,
    "PinWright.bpir.parser.ParseBreakStruct",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBpirParserParseBreakStructTest::RunTest(const FString& Parameters)
{
    TArray<FBpirEntryBlock> Blocks;
    TArray<FCompileError> Errors;
    bool bOk = ParseSingleInstruction(*this, TEXT("%b = break<HitResult>(%hit)"), Blocks, Errors);

    TestTrue(TEXT("Parse succeeded"), bOk);
    TestEqual(TEXT("1 block"), Blocks.Num(), 1);

    if (Blocks.Num() > 0 && Blocks[0].Instructions.Num() >= 1)
    {
        const FBpirInstruction& Inst = Blocks[0].Instructions[0];
        TestEqual(TEXT("Opcode is BreakStruct"), Inst.Opcode, EBpirOpcode::BreakStruct);
        TestEqual(TEXT("TypeArg is HitResult"), Inst.TypeArg, TEXT("HitResult"));
        TestEqual(TEXT("ResultName is b"), Inst.ResultName, TEXT("b"));
        TestTrue(TEXT("Has at least 1 arg"), Inst.Args.Num() >= 1);
        if (Inst.Args.Num() >= 1)
        {
            TestEqual(TEXT("Arg Value is %hit"), Inst.Args[0].Value, TEXT("%hit"));
        }
    }

    return true;
}

// ============================================================================
// 13. ParseMacro
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirParserParseMacroTest,
    "PinWright.bpir.parser.ParseMacro",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBpirParserParseMacroTest::RunTest(const FString& Parameters)
{
    TArray<FBpirEntryBlock> Blocks;
    TArray<FCompileError> Errors;
    bool bOk = ParseSingleInstruction(*this, TEXT("%m = macro DoOnce() [completed -> @c]"), Blocks, Errors);

    TestTrue(TEXT("Parse succeeded"), bOk);
    TestEqual(TEXT("1 block"), Blocks.Num(), 1);

    if (Blocks.Num() > 0 && Blocks[0].Instructions.Num() >= 1)
    {
        const FBpirInstruction& Inst = Blocks[0].Instructions[0];
        TestEqual(TEXT("Opcode is Macro"), Inst.Opcode, EBpirOpcode::Macro);
        TestEqual(TEXT("FunctionName is DoOnce"), Inst.FunctionName, TEXT("DoOnce"));
        TestEqual(TEXT("ResultName is m"), Inst.ResultName, TEXT("m"));

        TestTrue(TEXT("Has exec targets"), Inst.ExecTargets.Num() >= 1);
        if (Inst.ExecTargets.Num() >= 1)
        {
            TestEqual(TEXT("ExecTarget PinName"), Inst.ExecTargets[0].PinName, TEXT("completed"));
            TestEqual(TEXT("ExecTarget Label"), Inst.ExecTargets[0].Label, TEXT("c"));
        }
    }

    return true;
}

// ============================================================================
// 14. ParseSelect
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirParserParseSelectTest,
    "PinWright.bpir.parser.ParseSelect",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBpirParserParseSelectTest::RunTest(const FString& Parameters)
{
    TArray<FBpirEntryBlock> Blocks;
    TArray<FCompileError> Errors;
    bool bOk = ParseSingleInstruction(*this,
        TEXT("%s = select(cond: true, true: \"A\", false: \"B\")"), Blocks, Errors);

    TestTrue(TEXT("Parse succeeded"), bOk);
    TestEqual(TEXT("1 block"), Blocks.Num(), 1);

    if (Blocks.Num() > 0 && Blocks[0].Instructions.Num() >= 1)
    {
        const FBpirInstruction& Inst = Blocks[0].Instructions[0];
        TestEqual(TEXT("Opcode is Select"), Inst.Opcode, EBpirOpcode::Select);
        TestEqual(TEXT("ResultName is s"), Inst.ResultName, TEXT("s"));
        TestEqual(TEXT("Has 3 args"), Inst.Args.Num(), 3);
    }

    return true;
}

// ============================================================================
// 15. NormalizePinName — tested indirectly via arg parsing with space in name
// NormalizePinName is private, so we test it through ParseArgs which calls it
// on pin names. "Array Element: $val" should produce PinName=="ArrayElement".
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirParserNormalizePinNameTest,
    "PinWright.bpir.parser.NormalizePinName",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBpirParserNormalizePinNameTest::RunTest(const FString& Parameters)
{
    TArray<FBpirEntryBlock> Blocks;
    TArray<FCompileError> Errors;
    bool bOk = ParseSingleInstruction(*this,
        TEXT("call SomeFunction(Array Element: $val)"), Blocks, Errors);

    TestTrue(TEXT("Parse succeeded"), bOk);
    TestEqual(TEXT("1 block"), Blocks.Num(), 1);

    if (Blocks.Num() > 0 && Blocks[0].Instructions.Num() >= 1)
    {
        const FBpirInstruction& Inst = Blocks[0].Instructions[0];
        TestTrue(TEXT("Has at least 1 arg"), Inst.Args.Num() >= 1);
        if (Inst.Args.Num() >= 1)
        {
            TestEqual(TEXT("Pin name normalized: spaces removed, next char capitalized"),
                Inst.Args[0].PinName, TEXT("ArrayElement"));
        }
    }

    return true;
}

// ============================================================================
// 18. BuildIndices — verify ValueIndex and LabelIndex are populated correctly
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirParserBuildIndicesTest,
    "PinWright.bpir.parser.BuildIndices",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBpirParserBuildIndicesTest::RunTest(const FString& Parameters)
{
    TArray<FBpirEntryBlock> Blocks;
    TArray<FCompileError> Errors;

    FString Body = TEXT(
        "    %x = pure IsValid(Object: self)\n"
        "    @lbl:\n"
        "    call PrintString(InString: \"test\")");
    bool bOk = ParseMultipleInstructions(*this, Body, Blocks, Errors);

    TestTrue(TEXT("Parse succeeded"), bOk);
    TestEqual(TEXT("1 block"), Blocks.Num(), 1);

    if (Blocks.Num() > 0)
    {
        const FBpirEntryBlock& Block = Blocks[0];

        // Verify ValueIndex contains %x
        const int32* XIdx = Block.ValueIndex.Find(TEXT("x"));
        TestTrue(TEXT("ValueIndex contains 'x'"), XIdx != nullptr);
        if (XIdx)
        {
            TestTrue(TEXT("ValueIndex['x'] is a valid instruction index"),
                Block.Instructions.IsValidIndex(*XIdx));
            if (Block.Instructions.IsValidIndex(*XIdx))
            {
                TestEqual(TEXT("ValueIndex['x'] points to Pure instruction"),
                    Block.Instructions[*XIdx].Opcode, EBpirOpcode::Pure);
            }
        }

        // Verify LabelIndex contains @lbl
        const int32* LblIdx = Block.LabelIndex.Find(TEXT("lbl"));
        TestTrue(TEXT("LabelIndex contains 'lbl'"), LblIdx != nullptr);
        if (LblIdx)
        {
            TestTrue(TEXT("LabelIndex['lbl'] is a valid instruction index"),
                Block.Instructions.IsValidIndex(*LblIdx));
            if (Block.Instructions.IsValidIndex(*LblIdx))
            {
                TestEqual(TEXT("LabelIndex['lbl'] points to Label instruction"),
                    Block.Instructions[*LblIdx].Opcode, EBpirOpcode::Label);
            }
        }

        // Verify the indices are different (they point to different instructions)
        if (XIdx && LblIdx)
        {
            TestNotEqual(TEXT("ValueIndex and LabelIndex point to different instructions"),
                *XIdx, *LblIdx);
        }
    }

    return true;
}

// ============================================================================
// 19. ParseSwitchInt
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirParserParseSwitchIntTest,
    "PinWright.bpir.parser.ParseSwitchInt",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBpirParserParseSwitchIntTest::RunTest(const FString& Parameters)
{
    TArray<FBpirEntryBlock> Blocks;
    TArray<FCompileError> Errors;
    bool bOk = ParseSingleInstruction(*this,
        TEXT("%s = switch_int(%v) [1 -> @a, 2 -> @b, default -> @d]"), Blocks, Errors);

    TestTrue(TEXT("Parse succeeded"), bOk);
    TestEqual(TEXT("1 block"), Blocks.Num(), 1);

    if (Blocks.Num() > 0 && Blocks[0].Instructions.Num() >= 1)
    {
        const FBpirInstruction& Inst = Blocks[0].Instructions[0];
        TestEqual(TEXT("Opcode is SwitchInt"), Inst.Opcode, EBpirOpcode::SwitchInt);
        TestEqual(TEXT("3 exec targets"), Inst.ExecTargets.Num(), 3);
    }

    return true;
}

// ============================================================================
// 20. ParseSwitchString
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirParserParseSwitchStringTest,
    "PinWright.bpir.parser.ParseSwitchString",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBpirParserParseSwitchStringTest::RunTest(const FString& Parameters)
{
    TArray<FBpirEntryBlock> Blocks;
    TArray<FCompileError> Errors;
    bool bOk = ParseSingleInstruction(*this,
        TEXT("%s = switch_string(%v) [\"hello\" -> @a, default -> @d]"), Blocks, Errors);

    TestTrue(TEXT("Parse succeeded"), bOk);
    TestEqual(TEXT("1 block"), Blocks.Num(), 1);

    if (Blocks.Num() > 0 && Blocks[0].Instructions.Num() >= 1)
    {
        const FBpirInstruction& Inst = Blocks[0].Instructions[0];
        TestEqual(TEXT("Opcode is SwitchString"), Inst.Opcode, EBpirOpcode::SwitchString);
        TestEqual(TEXT("2 exec targets"), Inst.ExecTargets.Num(), 2);
    }

    return true;
}

// ============================================================================
// 21. ParseSwitchEnum
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirParserParseSwitchEnumTest,
    "PinWright.bpir.parser.ParseSwitchEnum",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBpirParserParseSwitchEnumTest::RunTest(const FString& Parameters)
{
    TArray<FBpirEntryBlock> Blocks;
    TArray<FCompileError> Errors;
    bool bOk = ParseSingleInstruction(*this,
        TEXT("%s = switch_enum<EMyEnum>(%v) [Value1 -> @a]"), Blocks, Errors);

    TestTrue(TEXT("Parse succeeded"), bOk);
    TestEqual(TEXT("1 block"), Blocks.Num(), 1);

    if (Blocks.Num() > 0 && Blocks[0].Instructions.Num() >= 1)
    {
        const FBpirInstruction& Inst = Blocks[0].Instructions[0];
        TestEqual(TEXT("Opcode is SwitchEnum"), Inst.Opcode, EBpirOpcode::SwitchEnum);
        TestEqual(TEXT("TypeArg is EMyEnum"), Inst.TypeArg, TEXT("EMyEnum"));
    }

    return true;
}

// ============================================================================
// 22. ParseClearDispatcher
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirParserParseClearDispatcherTest,
    "PinWright.bpir.parser.ParseClearDispatcher",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBpirParserParseClearDispatcherTest::RunTest(const FString& Parameters)
{
    TArray<FBpirEntryBlock> Blocks;
    TArray<FCompileError> Errors;
    bool bOk = ParseSingleInstruction(*this,
        TEXT("clear_dispatcher MyDispatcher()"), Blocks, Errors);

    TestTrue(TEXT("Parse succeeded"), bOk);
    TestEqual(TEXT("1 block"), Blocks.Num(), 1);

    if (Blocks.Num() > 0 && Blocks[0].Instructions.Num() >= 1)
    {
        const FBpirInstruction& Inst = Blocks[0].Instructions[0];
        TestEqual(TEXT("Opcode is ClearDispatcher"), Inst.Opcode, EBpirOpcode::ClearDispatcher);
        TestEqual(TEXT("FunctionName is MyDispatcher"), Inst.FunctionName, TEXT("MyDispatcher"));
    }

    return true;
}

// ============================================================================
// 23. ParseCallWithExecTargets
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirParserParseCallWithExecTargetsTest,
    "PinWright.bpir.parser.ParseCallWithExecTargets",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBpirParserParseCallWithExecTargetsTest::RunTest(const FString& Parameters)
{
    TArray<FBpirEntryBlock> Blocks;
    TArray<FCompileError> Errors;
    bool bOk = ParseSingleInstruction(*this,
        TEXT("call Foo(A: 1) [OnDone -> @d, OnFail -> @f]"), Blocks, Errors);

    TestTrue(TEXT("Parse succeeded"), bOk);
    TestEqual(TEXT("1 block"), Blocks.Num(), 1);

    if (Blocks.Num() > 0 && Blocks[0].Instructions.Num() >= 1)
    {
        const FBpirInstruction& Inst = Blocks[0].Instructions[0];
        TestEqual(TEXT("Opcode is Call"), Inst.Opcode, EBpirOpcode::Call);
        TestEqual(TEXT("2 exec targets"), Inst.ExecTargets.Num(), 2);
        if (Inst.ExecTargets.Num() >= 2)
        {
            TestEqual(TEXT("First target PinName"), Inst.ExecTargets[0].PinName, TEXT("OnDone"));
            TestEqual(TEXT("First target Label"), Inst.ExecTargets[0].Label, TEXT("d"));
            TestEqual(TEXT("Second target PinName"), Inst.ExecTargets[1].PinName, TEXT("OnFail"));
            TestEqual(TEXT("Second target Label"), Inst.ExecTargets[1].Label, TEXT("f"));
        }
    }

    return true;
}

// ============================================================================
// 24. ParseCallK2NodeClass
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirParserParseCallK2NodeClassTest,
    "PinWright.bpir.parser.ParseCallK2NodeClass",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBpirParserParseCallK2NodeClassTest::RunTest(const FString& Parameters)
{
    TArray<FBpirEntryBlock> Blocks;
    TArray<FCompileError> Errors;
    bool bOk = ParseSingleInstruction(*this,
        TEXT("call K2Node_CreateWidget(Class: WBP_HUD_C)"), Blocks, Errors);

    TestTrue(TEXT("Parse succeeded"), bOk);
    TestEqual(TEXT("1 block"), Blocks.Num(), 1);

    if (Blocks.Num() > 0 && Blocks[0].Instructions.Num() >= 1)
    {
        const FBpirInstruction& Inst = Blocks[0].Instructions[0];
        TestEqual(TEXT("Opcode is Call"), Inst.Opcode, EBpirOpcode::Call);
        TestEqual(TEXT("FunctionName is K2Node_CreateWidget"), Inst.FunctionName, TEXT("K2Node_CreateWidget"));
    }

    return true;
}

// ============================================================================
// 25. ParseGet
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirParserParseGetTest,
    "PinWright.bpir.parser.ParseGet",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBpirParserParseGetTest::RunTest(const FString& Parameters)
{
    TArray<FBpirEntryBlock> Blocks;
    TArray<FCompileError> Errors;
    bool bOk = ParseSingleInstruction(*this,
        TEXT("%v = get MyVar"), Blocks, Errors);

    TestTrue(TEXT("Parse succeeded"), bOk);
    TestEqual(TEXT("1 block"), Blocks.Num(), 1);

    if (Blocks.Num() > 0 && Blocks[0].Instructions.Num() >= 1)
    {
        const FBpirInstruction& Inst = Blocks[0].Instructions[0];
        TestEqual(TEXT("Opcode is Get"), Inst.Opcode, EBpirOpcode::Get);
        TestEqual(TEXT("FunctionName is MyVar"), Inst.FunctionName, TEXT("MyVar"));
    }

    return true;
}

// ============================================================================
// 26. ParseForeachBreak
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirParserParseForeachBreakTest,
    "PinWright.bpir.parser.ParseForeachBreak",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBpirParserParseForeachBreakTest::RunTest(const FString& Parameters)
{
    TArray<FBpirEntryBlock> Blocks;
    TArray<FCompileError> Errors;
    bool bOk = ParseSingleInstruction(*this,
        TEXT("%f = foreach_break($arr) [body -> @b, completed -> @c]"), Blocks, Errors);

    TestTrue(TEXT("Parse succeeded"), bOk);
    TestEqual(TEXT("1 block"), Blocks.Num(), 1);

    if (Blocks.Num() > 0 && Blocks[0].Instructions.Num() >= 1)
    {
        const FBpirInstruction& Inst = Blocks[0].Instructions[0];
        TestEqual(TEXT("Opcode is ForeachBreak"), Inst.Opcode, EBpirOpcode::ForeachBreak);
        TestEqual(TEXT("2 exec targets"), Inst.ExecTargets.Num(), 2);
    }

    return true;
}

// ============================================================================
// 27. ParseWhile
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirParserParseWhileTest,
    "PinWright.bpir.parser.ParseWhile",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBpirParserParseWhileTest::RunTest(const FString& Parameters)
{
    TArray<FBpirEntryBlock> Blocks;
    TArray<FCompileError> Errors;
    bool bOk = ParseSingleInstruction(*this,
        TEXT("%w = while(%cond) [body -> @b, completed -> @c]"), Blocks, Errors);

    TestTrue(TEXT("Parse succeeded"), bOk);
    TestEqual(TEXT("1 block"), Blocks.Num(), 1);

    if (Blocks.Num() > 0 && Blocks[0].Instructions.Num() >= 1)
    {
        const FBpirInstruction& Inst = Blocks[0].Instructions[0];
        TestEqual(TEXT("Opcode is While"), Inst.Opcode, EBpirOpcode::While);
        TestEqual(TEXT("2 exec targets"), Inst.ExecTargets.Num(), 2);
    }

    return true;
}

// ============================================================================
// 28. ParseSequence
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirParserParseSequenceTest,
    "PinWright.bpir.parser.ParseSequence",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBpirParserParseSequenceTest::RunTest(const FString& Parameters)
{
    TArray<FBpirEntryBlock> Blocks;
    TArray<FCompileError> Errors;
    bool bOk = ParseSingleInstruction(*this,
        TEXT("%s = sequence(3) [0 -> @a, 1 -> @b, 2 -> @c]"), Blocks, Errors);

    TestTrue(TEXT("Parse succeeded"), bOk);
    TestEqual(TEXT("1 block"), Blocks.Num(), 1);

    if (Blocks.Num() > 0 && Blocks[0].Instructions.Num() >= 1)
    {
        const FBpirInstruction& Inst = Blocks[0].Instructions[0];
        TestEqual(TEXT("Opcode is Sequence"), Inst.Opcode, EBpirOpcode::Sequence);
        TestEqual(TEXT("SequenceCount is 3"), Inst.SequenceCount, 3);
        TestEqual(TEXT("3 exec targets"), Inst.ExecTargets.Num(), 3);
    }

    return true;
}

// ============================================================================
// 29. ParseTimeline
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirParserParseTimelineTest,
    "PinWright.bpir.parser.ParseTimeline",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBpirParserParseTimelineTest::RunTest(const FString& Parameters)
{
    TArray<FBpirEntryBlock> Blocks;
    TArray<FCompileError> Errors;
    bool bOk = ParseSingleInstruction(*this,
        TEXT("%t = timeline MyTL() [update -> @u, finished -> @f]"), Blocks, Errors);

    TestTrue(TEXT("Parse succeeded"), bOk);
    TestEqual(TEXT("1 block"), Blocks.Num(), 1);

    if (Blocks.Num() > 0 && Blocks[0].Instructions.Num() >= 1)
    {
        const FBpirInstruction& Inst = Blocks[0].Instructions[0];
        TestEqual(TEXT("Opcode is Timeline"), Inst.Opcode, EBpirOpcode::Timeline);
        TestEqual(TEXT("FunctionName is MyTL"), Inst.FunctionName, TEXT("MyTL"));
    }

    return true;
}

// ============================================================================
// 30. ParseSelf
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirParserParseSelfTest,
    "PinWright.bpir.parser.ParseSelf",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBpirParserParseSelfTest::RunTest(const FString& Parameters)
{
    TArray<FBpirEntryBlock> Blocks;
    TArray<FCompileError> Errors;
    bool bOk = ParseSingleInstruction(*this,
        TEXT("%s = self"), Blocks, Errors);

    TestTrue(TEXT("Parse succeeded"), bOk);
    TestEqual(TEXT("1 block"), Blocks.Num(), 1);

    if (Blocks.Num() > 0 && Blocks[0].Instructions.Num() >= 1)
    {
        const FBpirInstruction& Inst = Blocks[0].Instructions[0];
        TestEqual(TEXT("Opcode is Self"), Inst.Opcode, EBpirOpcode::Self);
    }

    return true;
}

// ============================================================================
// 31. ParseMakeArray
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirParserParseMakeArrayTest,
    "PinWright.bpir.parser.ParseMakeArray",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBpirParserParseMakeArrayTest::RunTest(const FString& Parameters)
{
    TArray<FBpirEntryBlock> Blocks;
    TArray<FCompileError> Errors;
    bool bOk = ParseSingleInstruction(*this,
        TEXT("%a = make_array(0: 1, 1: 2)"), Blocks, Errors);

    TestTrue(TEXT("Parse succeeded"), bOk);
    TestEqual(TEXT("1 block"), Blocks.Num(), 1);

    if (Blocks.Num() > 0 && Blocks[0].Instructions.Num() >= 1)
    {
        const FBpirInstruction& Inst = Blocks[0].Instructions[0];
        TestEqual(TEXT("Opcode is MakeArray"), Inst.Opcode, EBpirOpcode::MakeArray);
        TestEqual(TEXT("Has 2 args"), Inst.Args.Num(), 2);
    }

    return true;
}

// ============================================================================
// 32. ParseReturn
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirParserParseReturnTest,
    "PinWright.bpir.parser.ParseReturn",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBpirParserParseReturnTest::RunTest(const FString& Parameters)
{
    TArray<FBpirEntryBlock> Blocks;
    TArray<FCompileError> Errors;
    bool bOk = ParseSingleInstruction(*this,
        TEXT("return %val"), Blocks, Errors);

    TestTrue(TEXT("Parse succeeded"), bOk);
    TestEqual(TEXT("1 block"), Blocks.Num(), 1);

    if (Blocks.Num() > 0 && Blocks[0].Instructions.Num() >= 1)
    {
        const FBpirInstruction& Inst = Blocks[0].Instructions[0];
        TestEqual(TEXT("Opcode is Return"), Inst.Opcode, EBpirOpcode::Return);
    }

    return true;
}

// ============================================================================
// 33. ParseComment
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirParserParseCommentTest,
    "PinWright.bpir.parser.ParseComment",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBpirParserParseCommentTest::RunTest(const FString& Parameters)
{
    TArray<FBpirEntryBlock> Blocks;
    TArray<FCompileError> Errors;
    bool bOk = ParseSingleInstruction(*this,
        TEXT("# This is a comment"), Blocks, Errors);

    TestTrue(TEXT("Parse succeeded"), bOk);
    TestEqual(TEXT("1 block"), Blocks.Num(), 1);

    if (Blocks.Num() > 0 && Blocks[0].Instructions.Num() >= 1)
    {
        const FBpirInstruction& Inst = Blocks[0].Instructions[0];
        TestEqual(TEXT("Opcode is Comment"), Inst.Opcode, EBpirOpcode::Comment);
    }

    return true;
}

// ============================================================================
// 34. ParseLatentTopLevel
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirParserParseLatentTopLevelTest,
    "PinWright.bpir.parser.ParseLatentTopLevel",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBpirParserParseLatentTopLevelTest::RunTest(const FString& Parameters)
{
    TArray<FBpirEntryBlock> Blocks;
    TArray<FCompileError> Errors;
    bool bOk = ParseSingleInstruction(*this,
        TEXT("latent Delay(Duration: 2.0) [completed -> @c]"), Blocks, Errors);

    TestTrue(TEXT("Parse succeeded"), bOk);
    TestEqual(TEXT("1 block"), Blocks.Num(), 1);

    if (Blocks.Num() > 0 && Blocks[0].Instructions.Num() >= 1)
    {
        const FBpirInstruction& Inst = Blocks[0].Instructions[0];
        TestEqual(TEXT("Opcode is Latent"), Inst.Opcode, EBpirOpcode::Latent);
        TestEqual(TEXT("FunctionName is Delay"), Inst.FunctionName, TEXT("Delay"));
    }

    return true;
}

// ============================================================================
// C2 - Entry Kind Parser Tests
// ============================================================================

// ============================================================================
// 35. ParseCustomEventEntry
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirParserParseCustomEventEntryTest,
    "PinWright.bpir.parser.ParseCustomEventEntry",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBpirParserParseCustomEventEntryTest::RunTest(const FString& Parameters)
{
    FString Code = TEXT("entry custom_event MyEvent(int Param) {\n    call PrintString(InString: \"test\")\n}");
    FBpirParser Parser;
    TArray<FBpirEntryBlock> Blocks;
    TArray<FCompileError> Errors;
    bool bOk = Parser.Parse(Code, Blocks, Errors, /*bSkipReferenceValidation=*/ true);

    TestTrue(TEXT("Parse succeeded"), bOk);
    TestTrue(TEXT("At least 1 block"), Blocks.Num() >= 1);

    if (Blocks.Num() > 0)
    {
        TestEqual(TEXT("Kind is CustomEvent"), Blocks[0].Kind, EBpirEntryKind::CustomEvent);
        TestEqual(TEXT("Name is MyEvent"), Blocks[0].Name, TEXT("MyEvent"));
        TestEqual(TEXT("1 param"), Blocks[0].Params.Num(), 1);
    }

    return true;
}

// ============================================================================
// 36. ParseFunctionEntry
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirParserParseFunctionEntryTest,
    "PinWright.bpir.parser.ParseFunctionEntry",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBpirParserParseFunctionEntryTest::RunTest(const FString& Parameters)
{
    FString Code = TEXT("entry function MyFunc(float X) -> bool @(1, 2) {\n    return true\n}");
    FBpirParser Parser;
    TArray<FBpirEntryBlock> Blocks;
    TArray<FCompileError> Errors;
    bool bOk = Parser.Parse(Code, Blocks, Errors, /*bSkipReferenceValidation=*/ true);

    TestTrue(TEXT("Parse succeeded"), bOk);
    TestTrue(TEXT("At least 1 block"), Blocks.Num() >= 1);

    if (Blocks.Num() > 0)
    {
        TestEqual(TEXT("Kind is Function"), Blocks[0].Kind, EBpirEntryKind::Function);
        TestEqual(TEXT("ReturnType is bool"), BpirTypeSpecParser::TypeSpecToBpirText(Blocks[0].ReturnType), FString(TEXT("bool")));
        TestTrue(TEXT("Entry has authored position"), Blocks[0].bHasAuthoredEntryPosition);
        TestEqual(TEXT("Entry authored X"), Blocks[0].AuthoredEntryPosition.X, 1.0);
        TestEqual(TEXT("Entry authored Y"), Blocks[0].AuthoredEntryPosition.Y, 2.0);
    }

    return true;
}

// ============================================================================
// 36B. ParseOverrideEntry
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirParserParseOverrideEntryTest,
    "PinWright.bpir.parser.ParseOverrideEntry",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBpirParserParseOverrideEntryTest::RunTest(const FString& Parameters)
{
    FString Code = TEXT("entry override CanJumpInternal() -> bool {\n    return true\n}");
    FBpirParser Parser;
    TArray<FBpirEntryBlock> Blocks;
    TArray<FCompileError> Errors;
    const bool bOk = Parser.Parse(Code, Blocks, Errors, /*bSkipReferenceValidation=*/ true);

    TestTrue(TEXT("Parse succeeded"), bOk);
    TestEqual(TEXT("Exactly 1 block"), Blocks.Num(), 1);

    if (Blocks.Num() == 1)
    {
        TestEqual(TEXT("Kind is Override"), Blocks[0].Kind, EBpirEntryKind::Override);
        TestEqual(TEXT("Name is CanJumpInternal"), Blocks[0].Name, TEXT("CanJumpInternal"));
        TestEqual(TEXT("ReturnType is bool"), BpirTypeSpecParser::TypeSpecToBpirText(Blocks[0].ReturnType), FString(TEXT("bool")));
    }

    return true;
}

// ============================================================================
// 37. ParseConstructionEntry
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirParserParseConstructionEntryTest,
    "PinWright.bpir.parser.ParseConstructionEntry",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBpirParserParseConstructionEntryTest::RunTest(const FString& Parameters)
{
    FString Code = TEXT("entry construction ConstructionScript() {\n    call PrintString(InString: \"built\")\n}");
    FBpirParser Parser;
    TArray<FBpirEntryBlock> Blocks;
    TArray<FCompileError> Errors;
    bool bOk = Parser.Parse(Code, Blocks, Errors, /*bSkipReferenceValidation=*/ true);

    TestTrue(TEXT("Parse succeeded"), bOk);
    TestTrue(TEXT("At least 1 block"), Blocks.Num() >= 1);

    if (Blocks.Num() > 0)
    {
        TestEqual(TEXT("Kind is Construction"), Blocks[0].Kind, EBpirEntryKind::Construction);
    }

    return true;
}

// ============================================================================
// 38. ParseComponentEventEntry
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirParserParseComponentEventEntryTest,
    "PinWright.bpir.parser.ParseComponentEventEntry",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBpirParserParseComponentEventEntryTest::RunTest(const FString& Parameters)
{
    FString Code = TEXT("entry component_event Box.OnOverlap(object<Actor> OtherActor) {\n    call PrintString(InString: \"overlap\")\n}");
    FBpirParser Parser;
    TArray<FBpirEntryBlock> Blocks;
    TArray<FCompileError> Errors;
    bool bOk = Parser.Parse(Code, Blocks, Errors, /*bSkipReferenceValidation=*/ true);

    TestTrue(TEXT("Parse succeeded"), bOk);
    TestTrue(TEXT("At least 1 block"), Blocks.Num() >= 1);

    if (Blocks.Num() > 0)
    {
        TestEqual(TEXT("Kind is ComponentEvent"), Blocks[0].Kind, EBpirEntryKind::ComponentEvent);
        TestEqual(TEXT("ComponentName is Box"), Blocks[0].ComponentName, TEXT("Box"));
    }

    return true;
}

// ============================================================================
// 39. ParseWidgetEventEntry
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirParserParseWidgetEventEntryTest,
    "PinWright.bpir.parser.ParseWidgetEventEntry",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBpirParserParseWidgetEventEntryTest::RunTest(const FString& Parameters)
{
    FString Code = TEXT("entry widget_event Button.OnClicked() {\n    call PrintString(InString: \"clicked\")\n}");
    FBpirParser Parser;
    TArray<FBpirEntryBlock> Blocks;
    TArray<FCompileError> Errors;
    bool bOk = Parser.Parse(Code, Blocks, Errors, /*bSkipReferenceValidation=*/ true);

    TestTrue(TEXT("Parse succeeded"), bOk);
    TestTrue(TEXT("At least 1 block"), Blocks.Num() >= 1);

    if (Blocks.Num() > 0)
    {
        TestEqual(TEXT("Kind is WidgetEvent"), Blocks[0].Kind, EBpirEntryKind::WidgetEvent);
        TestEqual(TEXT("ComponentName is Button"), Blocks[0].ComponentName, TEXT("Button"));
    }

    return true;
}

// ============================================================================
// 40. ParseKeyPressedEntry
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirParserParseKeyPressedEntryTest,
    "PinWright.bpir.parser.ParseKeyPressedEntry",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBpirParserParseKeyPressedEntryTest::RunTest(const FString& Parameters)
{
    FString Code = TEXT("entry key_pressed SpaceBar() {\n    call PrintString(InString: \"pressed\")\n}");
    FBpirParser Parser;
    TArray<FBpirEntryBlock> Blocks;
    TArray<FCompileError> Errors;
    bool bOk = Parser.Parse(Code, Blocks, Errors, /*bSkipReferenceValidation=*/ true);

    TestTrue(TEXT("Parse succeeded"), bOk);
    TestTrue(TEXT("At least 1 block"), Blocks.Num() >= 1);

    if (Blocks.Num() > 0)
    {
        TestEqual(TEXT("Kind is KeyPressed"), Blocks[0].Kind, EBpirEntryKind::KeyPressed);
        TestEqual(TEXT("Name is SpaceBar"), Blocks[0].Name, TEXT("SpaceBar"));
    }

    return true;
}

// ============================================================================
// 41. ParseKeyReleasedEntry
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirParserParseKeyReleasedEntryTest,
    "PinWright.bpir.parser.ParseKeyReleasedEntry",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBpirParserParseKeyReleasedEntryTest::RunTest(const FString& Parameters)
{
    FString Code = TEXT("entry key_released SpaceBar() {\n    call PrintString(InString: \"released\")\n}");
    FBpirParser Parser;
    TArray<FBpirEntryBlock> Blocks;
    TArray<FCompileError> Errors;
    bool bOk = Parser.Parse(Code, Blocks, Errors, /*bSkipReferenceValidation=*/ true);

    TestTrue(TEXT("Parse succeeded"), bOk);
    TestTrue(TEXT("At least 1 block"), Blocks.Num() >= 1);

    if (Blocks.Num() > 0)
    {
        TestEqual(TEXT("Kind is KeyReleased"), Blocks[0].Kind, EBpirEntryKind::KeyReleased);
        TestEqual(TEXT("Name is SpaceBar"), Blocks[0].Name, TEXT("SpaceBar"));
    }

    return true;
}

// ============================================================================
// InlineCommentStripping — set with inline comment
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirParserInlineCommentSetTest,
    "PinWright.bpir.parser.inline_comment_stripping.Set",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBpirParserInlineCommentSetTest::RunTest(const FString& Parameters)
{
    TArray<FBpirEntryBlock> Blocks;
    TArray<FCompileError> Errors;
    bool bOk = ParseSingleInstruction(*this, TEXT("set MyVar = 42 # comment"), Blocks, Errors);
    TestTrue(TEXT("Parse succeeded"), bOk);
    if (Blocks.Num() > 0 && Blocks[0].Instructions.Num() > 0)
    {
        const FBpirInstruction& Inst = Blocks[0].Instructions[0];
        TestEqual(TEXT("Opcode"), Inst.Opcode, EBpirOpcode::Set);
        TestEqual(TEXT("VarName"), Inst.FunctionName, TEXT("MyVar"));
        TestTrue(TEXT("Has arg"), Inst.Args.Num() > 0);
        if (Inst.Args.Num() > 0)
        {
            TestEqual(TEXT("Value is stripped"), Inst.Args[0].Value, TEXT("42"));
        }
    }
    return true;
}

// ============================================================================
// InlineCommentStripping — string preserved in set
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirParserInlineCommentStringTest,
    "PinWright.bpir.parser.inline_comment_stripping.StringPreserved",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBpirParserInlineCommentStringTest::RunTest(const FString& Parameters)
{
    TArray<FBpirEntryBlock> Blocks;
    TArray<FCompileError> Errors;
    bool bOk = ParseSingleInstruction(*this, TEXT("set MyVar = \"hello # world\" # comment"), Blocks, Errors);
    TestTrue(TEXT("Parse succeeded"), bOk);
    if (Blocks.Num() > 0 && Blocks[0].Instructions.Num() > 0)
    {
        const FBpirInstruction& Inst = Blocks[0].Instructions[0];
        TestTrue(TEXT("Has arg"), Inst.Args.Num() > 0);
        if (Inst.Args.Num() > 0)
        {
            TestEqual(TEXT("Value preserves string hash"), Inst.Args[0].Value, TEXT("\"hello # world\""));
        }
    }
    return true;
}

// ============================================================================
// InlineCommentStripping — return with inline comment
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirParserInlineCommentReturnTest,
    "PinWright.bpir.parser.inline_comment_stripping.Return",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBpirParserInlineCommentReturnTest::RunTest(const FString& Parameters)
{
    TArray<FBpirEntryBlock> Blocks;
    TArray<FCompileError> Errors;
    bool bOk = ParseSingleInstruction(*this, TEXT("return %result # comment"), Blocks, Errors);
    TestTrue(TEXT("Parse succeeded"), bOk);
    if (Blocks.Num() > 0 && Blocks[0].Instructions.Num() > 0)
    {
        const FBpirInstruction& Inst = Blocks[0].Instructions[0];
        TestEqual(TEXT("Opcode"), Inst.Opcode, EBpirOpcode::Return);
        TestTrue(TEXT("Has arg"), Inst.Args.Num() > 0);
        if (Inst.Args.Num() > 0)
        {
            TestEqual(TEXT("Value is stripped"), Inst.Args[0].Value, TEXT("%result"));
        }
    }
    return true;
}

// ============================================================================
// InlineCommentStripping — enum with inline comment
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirParserInlineCommentEnumTest,
    "PinWright.bpir.parser.inline_comment_stripping.Enum",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBpirParserInlineCommentEnumTest::RunTest(const FString& Parameters)
{
    TArray<FBpirEntryBlock> Blocks;
    TArray<FCompileError> Errors;
    bool bOk = ParseSingleInstruction(*this, TEXT("%val = enum EMyEnum::Value # comment"), Blocks, Errors);
    TestTrue(TEXT("Parse succeeded"), bOk);
    if (Blocks.Num() > 0 && Blocks[0].Instructions.Num() > 0)
    {
        const FBpirInstruction& Inst = Blocks[0].Instructions[0];
        TestEqual(TEXT("Opcode"), Inst.Opcode, EBpirOpcode::Enum);
        TestEqual(TEXT("TypeArg stripped"), Inst.TypeArg, TEXT("EMyEnum::Value"));
    }
    return true;
}

// ============================================================================
// InlineCommentStripping — get with inline comment
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirParserInlineCommentGetTest,
    "PinWright.bpir.parser.inline_comment_stripping.Get",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBpirParserInlineCommentGetTest::RunTest(const FString& Parameters)
{
    TArray<FBpirEntryBlock> Blocks;
    TArray<FCompileError> Errors;
    bool bOk = ParseSingleInstruction(*this, TEXT("%val = get MyVar # comment"), Blocks, Errors);
    TestTrue(TEXT("Parse succeeded"), bOk);
    if (Blocks.Num() > 0 && Blocks[0].Instructions.Num() > 0)
    {
        const FBpirInstruction& Inst = Blocks[0].Instructions[0];
        TestEqual(TEXT("Opcode"), Inst.Opcode, EBpirOpcode::Get);
        TestEqual(TEXT("FunctionName stripped"), Inst.FunctionName, TEXT("MyVar"));
    }
    return true;
}

// ============================================================================
// UnbindDispatcher — parses to UnbindDispatcher opcode
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirParserUnbindDispatcherTest,
    "PinWright.bpir.parser.UnbindDispatcher",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBpirParserUnbindDispatcherTest::RunTest(const FString& Parameters)
{
    TArray<FBpirEntryBlock> Blocks;
    TArray<FCompileError> Errors;
    bool bOk = ParseSingleInstruction(*this, TEXT("unbind_dispatcher OnMyEvent(Target: self)"), Blocks, Errors);
    TestTrue(TEXT("Parse succeeded"), bOk);
    if (Blocks.Num() > 0 && Blocks[0].Instructions.Num() > 0)
    {
        const FBpirInstruction& Inst = Blocks[0].Instructions[0];
        TestEqual(TEXT("Opcode"), Inst.Opcode, EBpirOpcode::UnbindDispatcher);
        TestEqual(TEXT("FunctionName"), Inst.FunctionName, TEXT("OnMyEvent"));
    }
    return true;
}

// ============================================================================
// ClearDispatcher — parses to ClearDispatcher opcode
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirParserClearDispatcherTest,
    "PinWright.bpir.parser.ClearDispatcher",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBpirParserClearDispatcherTest::RunTest(const FString& Parameters)
{
    TArray<FBpirEntryBlock> Blocks;
    TArray<FCompileError> Errors;
    bool bOk = ParseSingleInstruction(*this, TEXT("clear_dispatcher OnMyEvent(Target: self)"), Blocks, Errors);
    TestTrue(TEXT("Parse succeeded"), bOk);
    if (Blocks.Num() > 0 && Blocks[0].Instructions.Num() > 0)
    {
        const FBpirInstruction& Inst = Blocks[0].Instructions[0];
        TestEqual(TEXT("Opcode"), Inst.Opcode, EBpirOpcode::ClearDispatcher);
        TestEqual(TEXT("FunctionName"), Inst.FunctionName, TEXT("OnMyEvent"));
    }
    return true;
}

// ============================================================================
// UnmatchedBracket — error on unmatched [ in exec clause
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirParserUnmatchedBracketTest,
    "PinWright.bpir.parser.UnmatchedBracketError",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBpirParserUnmatchedBracketTest::RunTest(const FString& Parameters)
{
    TArray<FBpirEntryBlock> Blocks;
    TArray<FCompileError> Errors;
    bool bOk = ParseSingleInstruction(*this, TEXT("%n = branch(%cond) [true -> @then"), Blocks, Errors);
    TestFalse(TEXT("Parse should fail with unmatched bracket"), bOk);
    bool bFoundError = false;
    for (const FCompileError& Err : Errors)
    {
        if (Err.Message.Contains(TEXT("Unmatched")))
        {
            bFoundError = true;
            break;
        }
    }
    TestTrue(TEXT("Error about unmatched bracket"), bFoundError);
    return true;
}

// ============================================================================
// ExecWithoutArrow — error on exec without ->
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirParserExecWithoutArrowTest,
    "PinWright.bpir.parser.ExecWithoutArrowError",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBpirParserExecWithoutArrowTest::RunTest(const FString& Parameters)
{
    TArray<FBpirEntryBlock> Blocks;
    TArray<FCompileError> Errors;
    bool bOk = ParseSingleInstruction(*this, TEXT("exec @someLabel"), Blocks, Errors);
    TestFalse(TEXT("Parse should fail"), bOk);
    bool bFoundError = false;
    for (const FCompileError& Err : Errors)
    {
        if (Err.Message.Contains(TEXT("'->'")) || Err.Message.Contains(TEXT("arrow")))
        {
            bFoundError = true;
            break;
        }
    }
    TestTrue(TEXT("Error about missing ->"), bFoundError);
    return true;
}

// ============================================================================
// ExecClauseMissingArrow — error on exec clause entry without ->
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirParserExecClauseMissingArrowTest,
    "PinWright.bpir.parser.ExecClauseMissingArrow",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBpirParserExecClauseMissingArrowTest::RunTest(const FString& Parameters)
{
    TArray<FBpirEntryBlock> Blocks;
    TArray<FCompileError> Errors;
    bool bOk = ParseSingleInstruction(*this, TEXT("%n = branch(%cond) [true @then, false -> @else]"), Blocks, Errors);
    TestFalse(TEXT("Parse should fail"), bOk);
    bool bFoundError = false;
    for (const FCompileError& Err : Errors)
    {
        if (Err.Message.Contains(TEXT("'->'")) || Err.Message.Contains(TEXT("exec clause")))
        {
            bFoundError = true;
            break;
        }
    }
    TestTrue(TEXT("Error about missing -> in exec clause"), bFoundError);
    return true;
}

// ============================================================================
// 42. ParseSubsystem
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirParserParseSubsystemTest,
    "PinWright.bpir.parser.ParseSubsystem",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBpirParserParseSubsystemTest::RunTest(const FString& Parameters)
{
    TArray<FBpirEntryBlock> Blocks;
    TArray<FCompileError> Errors;
    bool bOk = ParseSingleInstruction(*this, TEXT("%ss = subsystem<AppMusicSubsystem>()"), Blocks, Errors);

    TestTrue(TEXT("Parse succeeded"), bOk);
    TestEqual(TEXT("1 block"), Blocks.Num(), 1);

    if (Blocks.Num() > 0 && Blocks[0].Instructions.Num() >= 1)
    {
        const FBpirInstruction& Inst = Blocks[0].Instructions[0];
        TestEqual(TEXT("Opcode is Subsystem"), Inst.Opcode, EBpirOpcode::Subsystem);
        TestEqual(TEXT("TypeArg is AppMusicSubsystem"), Inst.TypeArg, TEXT("AppMusicSubsystem"));
        TestEqual(TEXT("ResultName is ss"), Inst.ResultName, TEXT("ss"));
        TestEqual(TEXT("No args"), Inst.Args.Num(), 0);
    }

    return true;
}

// ============================================================================
// 43. ParseSwitchGeneric — switch($val) [case -> @label]
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirParserParseSwitchGenericTest,
    "PinWright.bpir.parser.ParseSwitchGeneric",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBpirParserParseSwitchGenericTest::RunTest(const FString& Parameters)
{
    TArray<FBpirEntryBlock> Blocks;
    TArray<FCompileError> Errors;
    bool bOk = ParseSingleInstruction(*this,
        TEXT("%sw = switch($CurrentState) [Idle -> @idle, Active -> @active, default -> @def]"), Blocks, Errors);

    TestTrue(TEXT("Parse succeeded"), bOk);
    TestEqual(TEXT("1 block"), Blocks.Num(), 1);

    if (Blocks.Num() > 0 && Blocks[0].Instructions.Num() >= 1)
    {
        const FBpirInstruction& Inst = Blocks[0].Instructions[0];
        TestEqual(TEXT("Opcode is Switch"), Inst.Opcode, EBpirOpcode::Switch);
        TestEqual(TEXT("ResultName is sw"), Inst.ResultName, TEXT("sw"));
        TestEqual(TEXT("3 exec targets"), Inst.ExecTargets.Num(), 3);

        if (Inst.ExecTargets.Num() >= 3)
        {
            TestEqual(TEXT("First target PinName"), Inst.ExecTargets[0].PinName, TEXT("Idle"));
            TestEqual(TEXT("First target Label"), Inst.ExecTargets[0].Label, TEXT("idle"));
            TestEqual(TEXT("Second target PinName"), Inst.ExecTargets[1].PinName, TEXT("Active"));
            TestEqual(TEXT("Second target Label"), Inst.ExecTargets[1].Label, TEXT("active"));
            TestEqual(TEXT("Third target PinName"), Inst.ExecTargets[2].PinName, TEXT("default"));
            TestEqual(TEXT("Third target Label"), Inst.ExecTargets[2].Label, TEXT("def"));
        }
    }

    return true;
}

// ============================================================================
// 44. ParseCallDispatcher — call_dispatcher parses correctly
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirParserParseCallDispatcherTest,
    "PinWright.bpir.parser.ParseCallDispatcher",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBpirParserParseCallDispatcherTest::RunTest(const FString& Parameters)
{
    TArray<FBpirEntryBlock> Blocks;
    TArray<FCompileError> Errors;
    bool bOk = ParseSingleInstruction(*this,
        TEXT("call_dispatcher OnHealthChanged(NewHealth: $Health)"), Blocks, Errors);

    TestTrue(TEXT("Parse succeeded"), bOk);
    TestEqual(TEXT("1 block"), Blocks.Num(), 1);

    if (Blocks.Num() > 0 && Blocks[0].Instructions.Num() >= 1)
    {
        const FBpirInstruction& Inst = Blocks[0].Instructions[0];
        TestEqual(TEXT("Opcode is CallDispatcher"), Inst.Opcode, EBpirOpcode::CallDispatcher);
        TestEqual(TEXT("FunctionName is OnHealthChanged"), Inst.FunctionName, TEXT("OnHealthChanged"));
        TestTrue(TEXT("Has at least 1 arg"), Inst.Args.Num() >= 1);
        if (Inst.Args.Num() >= 1)
        {
            TestEqual(TEXT("Arg PinName"), Inst.Args[0].PinName, TEXT("NewHealth"));
            TestEqual(TEXT("Arg Value"), Inst.Args[0].Value, TEXT("$Health"));
        }
    }

    return true;
}

// ============================================================================
// 45. ParseBindDispatcher — bind_dispatcher parses correctly
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirParserParseBindDispatcherTest,
    "PinWright.bpir.parser.ParseBindDispatcher",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBpirParserParseBindDispatcherTest::RunTest(const FString& Parameters)
{
    TArray<FBpirEntryBlock> Blocks;
    TArray<FCompileError> Errors;
    bool bOk = ParseSingleInstruction(*this,
        TEXT("bind_dispatcher OnHealthChanged(target: self, event: @HandleHealth)"), Blocks, Errors);

    TestTrue(TEXT("Parse succeeded"), bOk);
    TestEqual(TEXT("1 block"), Blocks.Num(), 1);

    if (Blocks.Num() > 0 && Blocks[0].Instructions.Num() >= 1)
    {
        const FBpirInstruction& Inst = Blocks[0].Instructions[0];
        TestEqual(TEXT("Opcode is BindDispatcher"), Inst.Opcode, EBpirOpcode::BindDispatcher);
        TestEqual(TEXT("FunctionName is OnHealthChanged"), Inst.FunctionName, TEXT("OnHealthChanged"));
        TestTrue(TEXT("Has at least 2 args"), Inst.Args.Num() >= 2);
    }

    return true;
}

// ============================================================================
// 46. ParseExternalPropertySet — set %ref.PropertyName = value (single dot)
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirParserParseExternalPropertySetTest,
    "PinWright.bpir.parser.ParseExternalPropertySet",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBpirParserParseExternalPropertySetTest::RunTest(const FString& Parameters)
{
    TArray<FBpirEntryBlock> Blocks;
    TArray<FCompileError> Errors;
    bool bOk = ParseSingleInstruction(*this,
        TEXT("set %gi.SharpRiseCount = 0"), Blocks, Errors);

    TestTrue(TEXT("Parse succeeded"), bOk);
    TestEqual(TEXT("1 block"), Blocks.Num(), 1);

    if (Blocks.Num() > 0 && Blocks[0].Instructions.Num() >= 1)
    {
        const FBpirInstruction& Inst = Blocks[0].Instructions[0];
        TestEqual(TEXT("Opcode is Set"), Inst.Opcode, EBpirOpcode::Set);
        // External property: TypeArg stores target (%gi), FunctionName stores property
        TestEqual(TEXT("TypeArg is %gi"), Inst.TypeArg, TEXT("%gi"));
        TestEqual(TEXT("FunctionName is SharpRiseCount"), Inst.FunctionName, TEXT("SharpRiseCount"));
        TestTrue(TEXT("Has 1 arg"), Inst.Args.Num() >= 1);
        if (Inst.Args.Num() >= 1)
        {
            TestEqual(TEXT("Arg Value"), Inst.Args[0].Value, TEXT("0"));
        }
    }

    return true;
}

// ============================================================================
// 47. ParseExternalPropertySetDollar — set $Param.Health = 100
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirParserParseExternalPropertySetDollarTest,
    "PinWright.bpir.parser.ParseExternalPropertySetDollar",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBpirParserParseExternalPropertySetDollarTest::RunTest(const FString& Parameters)
{
    TArray<FBpirEntryBlock> Blocks;
    TArray<FCompileError> Errors;
    bool bOk = ParseSingleInstruction(*this,
        TEXT("set $Param.Health = 100"), Blocks, Errors);

    TestTrue(TEXT("Parse succeeded"), bOk);
    TestEqual(TEXT("1 block"), Blocks.Num(), 1);

    if (Blocks.Num() > 0 && Blocks[0].Instructions.Num() >= 1)
    {
        const FBpirInstruction& Inst = Blocks[0].Instructions[0];
        TestEqual(TEXT("Opcode is Set"), Inst.Opcode, EBpirOpcode::Set);
        TestEqual(TEXT("TypeArg is $Param"), Inst.TypeArg, TEXT("$Param"));
        TestEqual(TEXT("FunctionName is Health"), Inst.FunctionName, TEXT("Health"));
        TestTrue(TEXT("Has 1 arg"), Inst.Args.Num() >= 1);
        if (Inst.Args.Num() >= 1)
        {
            TestEqual(TEXT("Arg Value"), Inst.Args[0].Value, TEXT("100"));
        }
    }

    return true;
}

// ============================================================================
// 47a. SetExternalMultiDotLHS — set %ref.PinName.Property = value (multi-dot)
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirParserSetExternalMultiDotLHSTest,
    "PinWright.bpir.parser.SetExternalMultiDotLHS",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBpirParserSetExternalMultiDotLHSTest::RunTest(const FString& Parameters)
{
    TArray<FBpirEntryBlock> Blocks;
    TArray<FCompileError> Errors;
    bool bOk = ParseSingleInstruction(*this,
        TEXT("set %n7.AsBSampleGameInstance.MissionIsJustOpened = false"), Blocks, Errors);

    TestTrue(TEXT("Parse succeeded"), bOk);
    TestEqual(TEXT("1 block"), Blocks.Num(), 1);

    if (Blocks.Num() > 0 && Blocks[0].Instructions.Num() >= 1)
    {
        const FBpirInstruction& Inst = Blocks[0].Instructions[0];
        TestEqual(TEXT("Opcode is Set"), Inst.Opcode, EBpirOpcode::Set);
        // Multi-dot: last dot splits — TypeArg gets everything before, FunctionName gets the property
        TestEqual(TEXT("TypeArg is %n7.AsBSampleGameInstance"), Inst.TypeArg, TEXT("%n7.AsBSampleGameInstance"));
        TestEqual(TEXT("FunctionName is MissionIsJustOpened"), Inst.FunctionName, TEXT("MissionIsJustOpened"));
        TestTrue(TEXT("Has 1 arg"), Inst.Args.Num() >= 1);
        if (Inst.Args.Num() >= 1)
        {
            TestEqual(TEXT("Arg Value"), Inst.Args[0].Value, TEXT("false"));
        }
    }

    return true;
}

// ============================================================================
// 47b. SetExternalSingleDotLHS — set %ref.Prop = value (single-dot regression)
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirParserSetExternalSingleDotLHSTest,
    "PinWright.bpir.parser.SetExternalSingleDotLHS",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBpirParserSetExternalSingleDotLHSTest::RunTest(const FString& Parameters)
{
    TArray<FBpirEntryBlock> Blocks;
    TArray<FCompileError> Errors;
    bool bOk = ParseSingleInstruction(*this,
        TEXT("set %n7.Prop = true"), Blocks, Errors);

    TestTrue(TEXT("Parse succeeded"), bOk);
    TestEqual(TEXT("1 block"), Blocks.Num(), 1);

    if (Blocks.Num() > 0 && Blocks[0].Instructions.Num() >= 1)
    {
        const FBpirInstruction& Inst = Blocks[0].Instructions[0];
        TestEqual(TEXT("Opcode is Set"), Inst.Opcode, EBpirOpcode::Set);
        // Single dot: same behavior as before — %n7 is target, Prop is property
        TestEqual(TEXT("TypeArg is %n7"), Inst.TypeArg, TEXT("%n7"));
        TestEqual(TEXT("FunctionName is Prop"), Inst.FunctionName, TEXT("Prop"));
        TestTrue(TEXT("Has 1 arg"), Inst.Args.Num() >= 1);
        if (Inst.Args.Num() >= 1)
        {
            TestEqual(TEXT("Arg Value"), Inst.Args[0].Value, TEXT("true"));
        }
    }

    return true;
}

// ============================================================================
// 47c. SetDollarVarDotProp — set $target.Prop = value (dollar-var regression)
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirParserSetDollarVarDotPropTest,
    "PinWright.bpir.parser.SetDollarVarDotProp",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBpirParserSetDollarVarDotPropTest::RunTest(const FString& Parameters)
{
    TArray<FBpirEntryBlock> Blocks;
    TArray<FCompileError> Errors;
    bool bOk = ParseSingleInstruction(*this,
        TEXT("set $target.Prop = 42"), Blocks, Errors);

    TestTrue(TEXT("Parse succeeded"), bOk);
    TestEqual(TEXT("1 block"), Blocks.Num(), 1);

    if (Blocks.Num() > 0 && Blocks[0].Instructions.Num() >= 1)
    {
        const FBpirInstruction& Inst = Blocks[0].Instructions[0];
        TestEqual(TEXT("Opcode is Set"), Inst.Opcode, EBpirOpcode::Set);
        TestEqual(TEXT("TypeArg is $target"), Inst.TypeArg, TEXT("$target"));
        TestEqual(TEXT("FunctionName is Prop"), Inst.FunctionName, TEXT("Prop"));
        TestTrue(TEXT("Has 1 arg"), Inst.Args.Num() >= 1);
        if (Inst.Args.Num() >= 1)
        {
            TestEqual(TEXT("Arg Value"), Inst.Args[0].Value, TEXT("42"));
        }
    }

    return true;
}

// ============================================================================
// 48. ParseReturnVoid — return without a value
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirParserParseReturnVoidTest,
    "PinWright.bpir.parser.ParseReturnVoid",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBpirParserParseReturnVoidTest::RunTest(const FString& Parameters)
{
    TArray<FBpirEntryBlock> Blocks;
    TArray<FCompileError> Errors;
    bool bOk = ParseSingleInstruction(*this, TEXT("return"), Blocks, Errors);

    TestTrue(TEXT("Parse succeeded"), bOk);
    TestEqual(TEXT("1 block"), Blocks.Num(), 1);

    if (Blocks.Num() > 0 && Blocks[0].Instructions.Num() >= 1)
    {
        const FBpirInstruction& Inst = Blocks[0].Instructions[0];
        TestEqual(TEXT("Opcode is Return"), Inst.Opcode, EBpirOpcode::Return);
        TestEqual(TEXT("No args (void return)"), Inst.Args.Num(), 0);
    }

    return true;
}

// ============================================================================
// 49. ParseCallWithReturn — %result = call FuncName(...)
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirParserParseCallWithReturnTest,
    "PinWright.bpir.parser.ParseCallWithReturn",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBpirParserParseCallWithReturnTest::RunTest(const FString& Parameters)
{
    TArray<FBpirEntryBlock> Blocks;
    TArray<FCompileError> Errors;
    bool bOk = ParseSingleInstruction(*this,
        TEXT("%hit = call LineTraceByChannel(Start: %s, End: %e)"), Blocks, Errors);

    TestTrue(TEXT("Parse succeeded"), bOk);
    TestEqual(TEXT("1 block"), Blocks.Num(), 1);

    if (Blocks.Num() > 0 && Blocks[0].Instructions.Num() >= 1)
    {
        const FBpirInstruction& Inst = Blocks[0].Instructions[0];
        TestEqual(TEXT("Opcode is Call"), Inst.Opcode, EBpirOpcode::Call);
        TestEqual(TEXT("ResultName is hit"), Inst.ResultName, TEXT("hit"));
        TestEqual(TEXT("FunctionName"), Inst.FunctionName, TEXT("LineTraceByChannel"));
        TestEqual(TEXT("Has 2 args"), Inst.Args.Num(), 2);
        if (Inst.Args.Num() >= 2)
        {
            TestEqual(TEXT("Arg 0 PinName"), Inst.Args[0].PinName, TEXT("Start"));
            TestEqual(TEXT("Arg 0 Value"), Inst.Args[0].Value, TEXT("%s"));
            TestEqual(TEXT("Arg 1 PinName"), Inst.Args[1].PinName, TEXT("End"));
            TestEqual(TEXT("Arg 1 Value"), Inst.Args[1].Value, TEXT("%e"));
        }
    }

    return true;
}

// ============================================================================
// 50. ParseFunctionEntryWithParams — function with typed params and return type
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirParserParseFunctionEntryWithParamsTest,
    "PinWright.bpir.parser.ParseFunctionEntryWithParams",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBpirParserParseFunctionEntryWithParamsTest::RunTest(const FString& Parameters)
{
    FString Code = TEXT("entry function CalculateDamage(float Base, float Mult) -> float {\n    return %result\n}");
    FBpirParser Parser;
    TArray<FBpirEntryBlock> Blocks;
    TArray<FCompileError> Errors;
    bool bOk = Parser.Parse(Code, Blocks, Errors, /*bSkipReferenceValidation=*/ true);

    TestTrue(TEXT("Parse succeeded"), bOk);
    TestTrue(TEXT("At least 1 block"), Blocks.Num() >= 1);

    if (Blocks.Num() > 0)
    {
        TestEqual(TEXT("Kind is Function"), Blocks[0].Kind, EBpirEntryKind::Function);
        TestEqual(TEXT("Name is CalculateDamage"), Blocks[0].Name, TEXT("CalculateDamage"));
        TestEqual(TEXT("ReturnType is float"), BpirTypeSpecParser::TypeSpecToBpirText(Blocks[0].ReturnType), FString(TEXT("float")));
        TestEqual(TEXT("Has 2 params"), Blocks[0].Params.Num(), 2);
        if (Blocks[0].Params.Num() >= 2)
        {
            TestEqual(TEXT("Param 0 Type"), BpirTypeSpecParser::TypeSpecToBpirText(Blocks[0].Params[0].Type), FString(TEXT("float")));
            TestEqual(TEXT("Param 0 Name"), Blocks[0].Params[0].Name, TEXT("Base"));
            TestEqual(TEXT("Param 1 Type"), BpirTypeSpecParser::TypeSpecToBpirText(Blocks[0].Params[1].Type), FString(TEXT("float")));
            TestEqual(TEXT("Param 1 Name"), Blocks[0].Params[1].Name, TEXT("Mult"));
        }
    }

    return true;
}

// ============================================================================
// 51. ParseEntryWithObjectParam — object<ClassName> type in entry params
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirParserParseEntryWithObjectParamTest,
    "PinWright.bpir.parser.ParseEntryWithObjectParam",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBpirParserParseEntryWithObjectParamTest::RunTest(const FString& Parameters)
{
    FString Code = TEXT("entry custom_event OnDamage(float Damage, object<AActor> Instigator) {\n    call PrintString(InString: \"hit\")\n}");
    FBpirParser Parser;
    TArray<FBpirEntryBlock> Blocks;
    TArray<FCompileError> Errors;
    bool bOk = Parser.Parse(Code, Blocks, Errors, /*bSkipReferenceValidation=*/ true);

    TestTrue(TEXT("Parse succeeded"), bOk);
    TestTrue(TEXT("At least 1 block"), Blocks.Num() >= 1);

    if (Blocks.Num() > 0)
    {
        TestEqual(TEXT("Kind is CustomEvent"), Blocks[0].Kind, EBpirEntryKind::CustomEvent);
        TestEqual(TEXT("Has 2 params"), Blocks[0].Params.Num(), 2);
        if (Blocks[0].Params.Num() >= 2)
        {
            TestEqual(TEXT("Param 0 Type"), BpirTypeSpecParser::TypeSpecToBpirText(Blocks[0].Params[0].Type), FString(TEXT("float")));
            TestEqual(TEXT("Param 0 Name"), Blocks[0].Params[0].Name, TEXT("Damage"));
            TestEqual(TEXT("Param 1 Type"), BpirTypeSpecParser::TypeSpecToBpirText(Blocks[0].Params[1].Type), FString(TEXT("object<AActor>")));
            TestEqual(TEXT("Param 1 Name"), Blocks[0].Params[1].Name, TEXT("Instigator"));
        }
    }

    return true;
}

// ============================================================================
// 52. ParseMultipleEntries — two entry blocks in one BPIR program
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirParserParseMultipleEntriesTest,
    "PinWright.bpir.parser.ParseMultipleEntries",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBpirParserParseMultipleEntriesTest::RunTest(const FString& Parameters)
{
    FString Code = TEXT(
        "entry event BeginPlay() {\n"
        "    call PrintString(InString: \"hello\")\n"
        "}\n"
        "entry function Calc(float X) -> float {\n"
        "    return %result\n"
        "}");
    FBpirParser Parser;
    TArray<FBpirEntryBlock> Blocks;
    TArray<FCompileError> Errors;
    bool bOk = Parser.Parse(Code, Blocks, Errors, /*bSkipReferenceValidation=*/ true);

    TestTrue(TEXT("Parse succeeded"), bOk);
    TestEqual(TEXT("2 blocks"), Blocks.Num(), 2);

    if (Blocks.Num() >= 2)
    {
        TestEqual(TEXT("Block 0 is Event"), Blocks[0].Kind, EBpirEntryKind::Event);
        TestEqual(TEXT("Block 0 name"), Blocks[0].Name, TEXT("BeginPlay"));
        TestEqual(TEXT("Block 1 is Function"), Blocks[1].Kind, EBpirEntryKind::Function);
        TestEqual(TEXT("Block 1 name"), Blocks[1].Name, TEXT("Calc"));
    }

    return true;
}

// ============================================================================
// 53. MacroEntry — parse entry macro with input and output params
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirParserMacroEntryTest,
    "PinWright.bpir.parser.MacroEntry",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBpirParserMacroEntryTest::RunTest(const FString& Parameters)
{
    FString Code = TEXT(
        "entry macro SimpleMacro(float Value) -> (float Result) {\n"
        "    %clamped = pure FClamp(Value: $Value, Min: 0.0, Max: 1.0)\n"
        "    return (Result: %clamped)\n"
        "}");
    FBpirParser Parser;
    TArray<FBpirEntryBlock> Blocks;
    TArray<FCompileError> Errors;
    bool bOk = Parser.Parse(Code, Blocks, Errors, /*bSkipReferenceValidation=*/ true);

    TestTrue(TEXT("Parse succeeded"), bOk);
    TestTrue(TEXT("At least 1 block"), Blocks.Num() >= 1);

    if (Blocks.Num() > 0)
    {
        TestEqual(TEXT("Kind is Macro"), Blocks[0].Kind, EBpirEntryKind::Macro);
        TestEqual(TEXT("Name is SimpleMacro"), Blocks[0].Name, TEXT("SimpleMacro"));
        TestEqual(TEXT("Has 1 input param"), Blocks[0].Params.Num(), 1);
        if (Blocks[0].Params.Num() >= 1)
        {
            TestEqual(TEXT("Param 0 Type"), BpirTypeSpecParser::TypeSpecToBpirText(Blocks[0].Params[0].Type), FString(TEXT("float")));
            TestEqual(TEXT("Param 0 Name"), Blocks[0].Params[0].Name, TEXT("Value"));
        }
        TestEqual(TEXT("Has 1 output param"), Blocks[0].OutputParams.Num(), 1);
        if (Blocks[0].OutputParams.Num() >= 1)
        {
            TestEqual(TEXT("Output 0 Type"), BpirTypeSpecParser::TypeSpecToBpirText(Blocks[0].OutputParams[0].Type), FString(TEXT("float")));
            TestEqual(TEXT("Output 0 Name"), Blocks[0].OutputParams[0].Name, TEXT("Result"));
        }
    }

    return true;
}

// ============================================================================
// 54. MacroEntryBacktickName — macro with backtick-quoted name (spaces)
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirParserMacroEntryBacktickNameTest,
    "PinWright.bpir.parser.MacroEntryBacktickName",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBpirParserMacroEntryBacktickNameTest::RunTest(const FString& Parameters)
{
    FString Code = TEXT(
        "entry macro `Update Opponents`(string TrackId) {\n"
        "    call PrintString(InString: $TrackId)\n"
        "}");
    FBpirParser Parser;
    TArray<FBpirEntryBlock> Blocks;
    TArray<FCompileError> Errors;
    bool bOk = Parser.Parse(Code, Blocks, Errors, /*bSkipReferenceValidation=*/ true);

    TestTrue(TEXT("Parse succeeded"), bOk);
    TestTrue(TEXT("At least 1 block"), Blocks.Num() >= 1);

    if (Blocks.Num() > 0)
    {
        TestEqual(TEXT("Kind is Macro"), Blocks[0].Kind, EBpirEntryKind::Macro);
        TestEqual(TEXT("Name preserves spaces"), Blocks[0].Name, TEXT("Update Opponents"));
    }

    return true;
}

// ============================================================================
// 55. MacroEntryMultiExit — macro with multiple exec exit paths
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirParserMacroEntryMultiExitTest,
    "PinWright.bpir.parser.MacroEntryMultiExit",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBpirParserMacroEntryMultiExitTest::RunTest(const FString& Parameters)
{
    FString Code = TEXT(
        "entry macro `Check Valid`(object<Object> Target) -> [IsValid -> @valid, IsNotValid -> @invalid] {\n"
        "    call PrintString(InString: \"check\")\n"
        "}");
    FBpirParser Parser;
    TArray<FBpirEntryBlock> Blocks;
    TArray<FCompileError> Errors;
    bool bOk = Parser.Parse(Code, Blocks, Errors, /*bSkipReferenceValidation=*/ true);

    TestTrue(TEXT("Parse succeeded"), bOk);
    TestTrue(TEXT("At least 1 block"), Blocks.Num() >= 1);

    if (Blocks.Num() > 0)
    {
        TestEqual(TEXT("Kind is Macro"), Blocks[0].Kind, EBpirEntryKind::Macro);
        TestEqual(TEXT("Has 2 exec outputs"), Blocks[0].ExecOutputNames.Num(), 2);
        if (Blocks[0].ExecOutputNames.Num() >= 2)
        {
            TestEqual(TEXT("Exec output 0"), Blocks[0].ExecOutputNames[0], TEXT("IsValid"));
            TestEqual(TEXT("Exec output 1"), Blocks[0].ExecOutputNames[1], TEXT("IsNotValid"));
        }
        TestEqual(TEXT("No data outputs"), Blocks[0].OutputParams.Num(), 0);
    }

    return true;
}

// ============================================================================
// 56. MacroEntryDataAndExec — macro with both data outputs and exec exits
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirParserMacroEntryDataAndExecTest,
    "PinWright.bpir.parser.MacroEntryDataAndExec",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBpirParserMacroEntryDataAndExecTest::RunTest(const FString& Parameters)
{
    FString Code = TEXT(
        "entry macro Validate(float X) -> (bool Result) [Pass -> @pass, Fail -> @fail] {\n"
        "    return [Pass] (Result: true)\n"
        "}");
    FBpirParser Parser;
    TArray<FBpirEntryBlock> Blocks;
    TArray<FCompileError> Errors;
    bool bOk = Parser.Parse(Code, Blocks, Errors, /*bSkipReferenceValidation=*/ true);

    TestTrue(TEXT("Parse succeeded"), bOk);
    TestTrue(TEXT("At least 1 block"), Blocks.Num() >= 1);

    if (Blocks.Num() > 0)
    {
        TestEqual(TEXT("Kind is Macro"), Blocks[0].Kind, EBpirEntryKind::Macro);
        TestEqual(TEXT("Has 1 data output"), Blocks[0].OutputParams.Num(), 1);
        if (Blocks[0].OutputParams.Num() >= 1)
        {
            TestEqual(TEXT("Output 0 Type"), BpirTypeSpecParser::TypeSpecToBpirText(Blocks[0].OutputParams[0].Type), FString(TEXT("bool")));
            TestEqual(TEXT("Output 0 Name"), Blocks[0].OutputParams[0].Name, TEXT("Result"));
        }
        TestEqual(TEXT("Has 2 exec outputs"), Blocks[0].ExecOutputNames.Num(), 2);
        if (Blocks[0].ExecOutputNames.Num() >= 2)
        {
            TestEqual(TEXT("Exec output 0"), Blocks[0].ExecOutputNames[0], TEXT("Pass"));
            TestEqual(TEXT("Exec output 1"), Blocks[0].ExecOutputNames[1], TEXT("Fail"));
        }
    }

    return true;
}

// ============================================================================
// 57. ReturnNamedArgs — return with named output arguments
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirParserReturnNamedArgsTest,
    "PinWright.bpir.parser.ReturnNamedArgs",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBpirParserReturnNamedArgsTest::RunTest(const FString& Parameters)
{
    TArray<FBpirEntryBlock> Blocks;
    TArray<FCompileError> Errors;
    bool bOk = ParseSingleInstruction(*this, TEXT("return (OutputX: %val1, OutputY: %val2)"), Blocks, Errors);

    TestTrue(TEXT("Parse succeeded"), bOk);
    TestTrue(TEXT("At least 1 block"), Blocks.Num() >= 1);

    if (Blocks.Num() > 0)
    {
        TestTrue(TEXT("At least 1 instruction"), Blocks[0].Instructions.Num() >= 1);
        if (Blocks[0].Instructions.Num() >= 1)
        {
            const FBpirInstruction& Inst = Blocks[0].Instructions[0];
            TestEqual(TEXT("Opcode is Return"), Inst.Opcode, EBpirOpcode::Return);
            TestEqual(TEXT("Has 2 args"), Inst.Args.Num(), 2);
            if (Inst.Args.Num() >= 2)
            {
                TestEqual(TEXT("Arg 0 PinName"), Inst.Args[0].PinName, TEXT("OutputX"));
                TestEqual(TEXT("Arg 0 Value"), Inst.Args[0].Value, TEXT("%val1"));
                TestEqual(TEXT("Arg 1 PinName"), Inst.Args[1].PinName, TEXT("OutputY"));
                TestEqual(TEXT("Arg 1 Value"), Inst.Args[1].Value, TEXT("%val2"));
            }
        }
    }

    return true;
}

// ============================================================================
// 58. ReturnWithExitPin — return with exit pin name and data args
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirParserReturnWithExitPinTest,
    "PinWright.bpir.parser.ReturnWithExitPin",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBpirParserReturnWithExitPinTest::RunTest(const FString& Parameters)
{
    TArray<FBpirEntryBlock> Blocks;
    TArray<FCompileError> Errors;
    bool bOk = ParseSingleInstruction(*this, TEXT("return [IsValid] (Data: %val)"), Blocks, Errors);

    TestTrue(TEXT("Parse succeeded"), bOk);
    TestTrue(TEXT("At least 1 block"), Blocks.Num() >= 1);

    if (Blocks.Num() > 0)
    {
        TestTrue(TEXT("At least 1 instruction"), Blocks[0].Instructions.Num() >= 1);
        if (Blocks[0].Instructions.Num() >= 1)
        {
            const FBpirInstruction& Inst = Blocks[0].Instructions[0];
            TestEqual(TEXT("Opcode is Return"), Inst.Opcode, EBpirOpcode::Return);
            TestEqual(TEXT("TypeArg is IsValid"), Inst.TypeArg, TEXT("IsValid"));
            TestEqual(TEXT("Has 1 arg"), Inst.Args.Num(), 1);
            if (Inst.Args.Num() >= 1)
            {
                TestEqual(TEXT("Arg 0 PinName"), Inst.Args[0].PinName, TEXT("Data"));
                TestEqual(TEXT("Arg 0 Value"), Inst.Args[0].Value, TEXT("%val"));
            }
        }
    }

    return true;
}

// ============================================================================
// 59. ReturnExitPinOnly — return with only an exit pin name, no data
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirParserReturnExitPinOnlyTest,
    "PinWright.bpir.parser.ReturnExitPinOnly",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBpirParserReturnExitPinOnlyTest::RunTest(const FString& Parameters)
{
    TArray<FBpirEntryBlock> Blocks;
    TArray<FCompileError> Errors;
    bool bOk = ParseSingleInstruction(*this, TEXT("return [IsNotValid]"), Blocks, Errors);

    TestTrue(TEXT("Parse succeeded"), bOk);
    TestTrue(TEXT("At least 1 block"), Blocks.Num() >= 1);

    if (Blocks.Num() > 0)
    {
        TestTrue(TEXT("At least 1 instruction"), Blocks[0].Instructions.Num() >= 1);
        if (Blocks[0].Instructions.Num() >= 1)
        {
            const FBpirInstruction& Inst = Blocks[0].Instructions[0];
            TestEqual(TEXT("Opcode is Return"), Inst.Opcode, EBpirOpcode::Return);
            TestEqual(TEXT("TypeArg is IsNotValid"), Inst.TypeArg, TEXT("IsNotValid"));
            TestEqual(TEXT("Has 0 args"), Inst.Args.Num(), 0);
        }
    }

    return true;
}

// ============================================================================
// 60. ParseForeachTopLevel — bare foreach(...) without %name = prefix
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirParserParseForeachTopLevelTest,
    "PinWright.bpir.parser.ParseForeachTopLevel",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBpirParserParseForeachTopLevelTest::RunTest(const FString& Parameters)
{
    TArray<FBpirEntryBlock> Blocks;
    TArray<FCompileError> Errors;
    bool bOk = ParseSingleInstruction(*this,
        TEXT("foreach(Array: $items) [body -> @b, completed -> @done]"), Blocks, Errors);

    TestTrue(TEXT("Parse succeeded"), bOk);
    TestEqual(TEXT("1 block"), Blocks.Num(), 1);

    if (Blocks.Num() > 0 && Blocks[0].Instructions.Num() >= 1)
    {
        const FBpirInstruction& Inst = Blocks[0].Instructions[0];
        TestEqual(TEXT("Opcode is Foreach"), Inst.Opcode, EBpirOpcode::Foreach);
        TestTrue(TEXT("ResultName is empty"), Inst.ResultName.IsEmpty());

        TestEqual(TEXT("2 exec targets"), Inst.ExecTargets.Num(), 2);
        if (Inst.ExecTargets.Num() >= 2)
        {
            TestEqual(TEXT("body target PinName"), Inst.ExecTargets[0].PinName, TEXT("body"));
            TestEqual(TEXT("body target Label"), Inst.ExecTargets[0].Label, TEXT("b"));
            TestEqual(TEXT("completed target PinName"), Inst.ExecTargets[1].PinName, TEXT("completed"));
            TestEqual(TEXT("completed target Label"), Inst.ExecTargets[1].Label, TEXT("done"));
        }
    }

    return true;
}

// ============================================================================
// 61. AliasDollar — %tmp = $MyVar parses as Alias with AliasRhs == "$MyVar"
// Pre-fix: "Expected keyword after '='" parse error.
// Post-fix: Opcode == Alias, ResultName == "tmp", AliasRhs == "$MyVar".
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirParserAliasDollarTest,
    "PinWright.bpir.parser.AliasDollar",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBpirParserAliasDollarTest::RunTest(const FString& Parameters)
{
    TArray<FBpirEntryBlock> Blocks;
    TArray<FCompileError> Errors;
    bool bOk = ParseSingleInstruction(*this, TEXT("%tmp = $MyVar"), Blocks, Errors);

    // Pre-fix: parse fails with "Expected keyword after '='"
    TestTrue(TEXT("Parse succeeded"), bOk);
    TestEqual(TEXT("1 block"), Blocks.Num(), 1);

    if (Blocks.Num() > 0 && Blocks[0].Instructions.Num() >= 1)
    {
        const FBpirInstruction& Inst = Blocks[0].Instructions[0];
        TestEqual(TEXT("Opcode is Alias"), Inst.Opcode, EBpirOpcode::Alias);
        TestEqual(TEXT("ResultName is tmp"), Inst.ResultName, TEXT("tmp"));
        TestEqual(TEXT("AliasRhs is $MyVar"), Inst.AliasRhs, TEXT("$MyVar"));
        // Alias has no function name, no args, no exec targets
        TestTrue(TEXT("FunctionName is empty"), Inst.FunctionName.IsEmpty());
        TestEqual(TEXT("No args"), Inst.Args.Num(), 0);
    }

    return true;
}

// ============================================================================
// 62. AliasPercent — %alias = %other parses as Alias with AliasRhs == "%other"
// Pre-fix: "Expected keyword after '='" parse error.
// Post-fix: Opcode == Alias, ResultName == "alias", AliasRhs == "%other".
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirParserAliasPercentTest,
    "PinWright.bpir.parser.AliasPercent",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBpirParserAliasPercentTest::RunTest(const FString& Parameters)
{
    TArray<FBpirEntryBlock> Blocks;
    TArray<FCompileError> Errors;
    bool bOk = ParseSingleInstruction(*this, TEXT("%alias = %other"), Blocks, Errors);

    // Pre-fix: parse fails with "Expected keyword after '='"
    TestTrue(TEXT("Parse succeeded"), bOk);
    TestEqual(TEXT("1 block"), Blocks.Num(), 1);

    if (Blocks.Num() > 0 && Blocks[0].Instructions.Num() >= 1)
    {
        const FBpirInstruction& Inst = Blocks[0].Instructions[0];
        TestEqual(TEXT("Opcode is Alias"), Inst.Opcode, EBpirOpcode::Alias);
        TestEqual(TEXT("ResultName is alias"), Inst.ResultName, TEXT("alias"));
        TestEqual(TEXT("AliasRhs is %other"), Inst.AliasRhs, TEXT("%other"));
        TestTrue(TEXT("FunctionName is empty"), Inst.FunctionName.IsEmpty());
        TestEqual(TEXT("No args"), Inst.Args.Num(), 0);
    }

    return true;
}

// ============================================================================
// 63. AliasDotSuffixRejected — %x = $MyVar.Field must FAIL to parse
// Inline member access is not supported; user must use break<T> instead.
// This verifies the dot-suffix guard in the parser is preserved after the fix.
// Pre-fix: also fails, but for the wrong reason ("Expected keyword after '='").
// Post-fix: falls through to the keyword-dispatch path and also fails, but
//           the observable contract is the same — parse returns false.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirParserAliasDotSuffixRejectedTest,
    "PinWright.bpir.parser.AliasDotSuffixRejected",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBpirParserAliasDotSuffixRejectedTest::RunTest(const FString& Parameters)
{
    TArray<FBpirEntryBlock> Blocks;
    TArray<FCompileError> Errors;
    bool bOk = ParseSingleInstruction(*this, TEXT("%x = $MyVar.Field"), Blocks, Errors);

    // Inline member access via alias is not supported — parse must fail.
    TestFalse(TEXT("Parse should fail for dot-suffix alias"), bOk);
    TestTrue(TEXT("At least one error reported"), Errors.Num() > 0);

    return true;
}

// ============================================================================
// 64. RejectAtColonEventName — `entry event @Widget:Event(...)` must FAIL.
//
// The silent-degrade bug (B-bpir-widget-event-at-syntax-silent-degrade):
// the parser used to accept `@RenameButton:OnClicked` as a plain event name,
// producing a K2Node_Event (unbound) instead of a K2Node_ComponentBoundEvent.
// The correct form is `entry widget_event Widget.OnClicked()`.
//
// Counterfactual: if the validation added to BpirParser.cpp::ParseEntryLine
// around lines 568-578 is reverted, this test fails because the parser accepts
// `@RenameButton:OnClicked` as a plain event name and returns bOk=true with
// zero errors — matching the ticket's silent-degrade bug.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirParserRejectAtColonEventNameTest,
    "PinWright.bpir.parser.RejectAtColonEventName",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBpirParserRejectAtColonEventNameTest::RunTest(const FString& Parameters)
{
    // --- Negative case: @Widget:Event form must be rejected ---
    {
        FBpirParser Parser;
        TArray<FBpirEntryBlock> Blocks;
        TArray<FCompileError> Errors;
        bool bOk = Parser.Parse(TEXT("entry event @RenameButton:OnClicked() {\n}"), Blocks, Errors, /*bSkipReferenceValidation=*/ true);

        TestFalse(TEXT("Parse should fail for @Widget:Event syntax"), bOk);
        TestTrue(TEXT("At least one error emitted"), Errors.Num() > 0);
        if (Errors.Num() > 0)
        {
            TestTrue(TEXT("Error message mentions widget_event hint"), Errors[0].Message.Contains(TEXT("widget_event")));
        }
    }

    // --- Positive case: entry widget_event Widget.OnClicked() must still succeed ---
    {
        FBpirParser Parser;
        TArray<FBpirEntryBlock> Blocks;
        TArray<FCompileError> Errors;
        bool bOk = Parser.Parse(TEXT("entry widget_event RenameButton.OnClicked()\n{\n}"), Blocks, Errors, /*bSkipReferenceValidation=*/ true);

        TestTrue(TEXT("Parse should succeed for widget_event form"), bOk);
        TestTrue(TEXT("At least one block parsed"), Blocks.Num() >= 1);
        if (Blocks.Num() > 0)
        {
            TestEqual(TEXT("Kind is WidgetEvent"), Blocks[0].Kind, EBpirEntryKind::WidgetEvent);
            TestEqual(TEXT("ComponentName is RenameButton"), Blocks[0].ComponentName, TEXT("RenameButton"));
            TestEqual(TEXT("Name is OnClicked"), Blocks[0].Name, TEXT("OnClicked"));
        }
    }

    return true;
}

// ============================================================================
// 65. AuthoredPositionValid — BPIR instructions carry explicit node positions
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirParserAuthoredPositionValidTest,
    "PinWright.bpir.parser.AuthoredPositionValid",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBpirParserAuthoredPositionValidTest::RunTest(const FString& Parameters)
{
    TArray<FBpirEntryBlock> Blocks;
    TArray<FCompileError> Errors;
    bool bOk = ParseSingleInstruction(*this, TEXT("call PrintString(InString: \"Hello\") @(0, 0)"), Blocks, Errors);

    TestTrue(TEXT("Parse succeeded"), bOk);
    TestEqual(TEXT("1 block"), Blocks.Num(), 1);

    if (Blocks.Num() > 0 && Blocks[0].Instructions.Num() >= 1)
    {
        const FBpirInstruction& Inst = Blocks[0].Instructions[0];
        TestTrue(TEXT("Has authored position"), Inst.bHasAuthoredPosition);
        TestEqual(TEXT("Authored position"), Inst.AuthoredPosition, FVector2D(0.0, 0.0));
        TestEqual(TEXT("Opcode is Call"), Inst.Opcode, EBpirOpcode::Call);
    }

    return true;
}

// ============================================================================
// 66. AuthoredPositionWhitespaceAndNegative — accepts compact and negative coords
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirParserAuthoredPositionWhitespaceAndNegativeTest,
    "PinWright.bpir.parser.AuthoredPositionWhitespaceAndNegative",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBpirParserAuthoredPositionWhitespaceAndNegativeTest::RunTest(const FString& Parameters)
{
    {
        TArray<FBpirEntryBlock> Blocks;
        TArray<FCompileError> Errors;
        bool bOk = ParseSingleInstruction(*this, TEXT("call PrintString(InString: \"A\") @(10,20)"), Blocks, Errors);

        TestTrue(TEXT("Parse compact position succeeded"), bOk);
        if (Blocks.Num() > 0 && Blocks[0].Instructions.Num() >= 1)
        {
            const FBpirInstruction& Inst = Blocks[0].Instructions[0];
            TestTrue(TEXT("Compact position present"), Inst.bHasAuthoredPosition);
            TestEqual(TEXT("Compact position"), Inst.AuthoredPosition, FVector2D(10.0, 20.0));
        }
    }

    {
        TArray<FBpirEntryBlock> Blocks;
        TArray<FCompileError> Errors;
        bool bOk = ParseSingleInstruction(*this,
            TEXT("branch($cond) [true -> @then, false -> @else] @(-10, 20)"), Blocks, Errors);

        TestTrue(TEXT("Parse negative position after exec suffix succeeded"), bOk);
        if (Blocks.Num() > 0 && Blocks[0].Instructions.Num() >= 1)
        {
            const FBpirInstruction& Inst = Blocks[0].Instructions[0];
            TestTrue(TEXT("Negative position present"), Inst.bHasAuthoredPosition);
            TestEqual(TEXT("Negative position"), Inst.AuthoredPosition, FVector2D(-10.0, 20.0));
            TestEqual(TEXT("Exec targets preserved"), Inst.ExecTargets.Num(), 2);
        }
    }

    return true;
}

// ============================================================================
// 67. AuthoredPositionIgnoresQuotedMarkerAndLabels — quote-aware @ handling
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirParserAuthoredPositionIgnoresQuotedMarkerAndLabelsTest,
    "PinWright.bpir.parser.AuthoredPositionIgnoresQuotedMarkerAndLabels",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBpirParserAuthoredPositionIgnoresQuotedMarkerAndLabelsTest::RunTest(const FString& Parameters)
{
    {
        TArray<FBpirEntryBlock> Blocks;
        TArray<FCompileError> Errors;
        bool bOk = ParseSingleInstruction(*this, TEXT("call PrintString(InString: \"quoted @(1, 2)\")"), Blocks, Errors);

        TestTrue(TEXT("Parse quoted marker succeeded"), bOk);
        if (Blocks.Num() > 0 && Blocks[0].Instructions.Num() >= 1)
        {
            const FBpirInstruction& Inst = Blocks[0].Instructions[0];
            TestFalse(TEXT("Quoted marker is not position"), Inst.bHasAuthoredPosition);
            TestTrue(TEXT("Quoted call has arg"), Inst.Args.Num() >= 1);
            if (Inst.Args.Num() >= 1)
            {
                TestEqual(TEXT("Quoted arg preserved"), Inst.Args[0].Value, TEXT("\"quoted @(1, 2)\""));
            }
        }
    }

    {
        TArray<FBpirEntryBlock> Blocks;
        TArray<FCompileError> Errors;
        bool bOk = ParseSingleInstruction(*this, TEXT("exec -> @done"), Blocks, Errors);

        TestTrue(TEXT("Parse label ref succeeded"), bOk);
        if (Blocks.Num() > 0 && Blocks[0].Instructions.Num() >= 1)
        {
            const FBpirInstruction& Inst = Blocks[0].Instructions[0];
            TestFalse(TEXT("Label ref is not position"), Inst.bHasAuthoredPosition);
            TestTrue(TEXT("Exec target present"), Inst.ExecTargets.Num() >= 1);
            if (Inst.ExecTargets.Num() >= 1)
            {
                TestEqual(TEXT("Exec target label"), Inst.ExecTargets[0].Label, TEXT("done"));
            }
        }
    }

    {
        TArray<FBpirEntryBlock> Blocks;
        TArray<FCompileError> Errors;
        bool bOk = ParseSingleInstruction(*this, TEXT("call PrintString(InString: \"A\") # comment @(1, 2)"), Blocks, Errors);

        TestTrue(TEXT("Parse inline comment marker succeeded"), bOk);
        if (Blocks.Num() > 0 && Blocks[0].Instructions.Num() >= 1)
        {
            const FBpirInstruction& Inst = Blocks[0].Instructions[0];
            TestFalse(TEXT("Inline comment marker is not position"), Inst.bHasAuthoredPosition);
        }
    }

    return true;
}

// ============================================================================
// 68. AuthoredPositionLabelsAndComments — labels can carry positions, comments do not
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirParserAuthoredPositionLabelsAndCommentsTest,
    "PinWright.bpir.parser.AuthoredPositionLabelsAndComments",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBpirParserAuthoredPositionLabelsAndCommentsTest::RunTest(const FString& Parameters)
{
    {
        TArray<FBpirEntryBlock> Blocks;
        TArray<FCompileError> Errors;
        bool bOk = ParseSingleInstruction(*this, TEXT("@done: @(30, 40)"), Blocks, Errors);

        TestTrue(TEXT("Parse positioned label succeeded"), bOk);
        if (Blocks.Num() > 0 && Blocks[0].Instructions.Num() >= 1)
        {
            const FBpirInstruction& Inst = Blocks[0].Instructions[0];
            TestEqual(TEXT("Opcode is Label"), Inst.Opcode, EBpirOpcode::Label);
            TestTrue(TEXT("Label position present"), Inst.bHasAuthoredPosition);
            TestEqual(TEXT("Label position"), Inst.AuthoredPosition, FVector2D(30.0, 40.0));
        }
    }

    {
        FBpirParser Parser;
        TArray<FBpirEntryBlock> Blocks;
        TArray<FCompileError> Errors;
        bool bOk = Parser.Parse(TEXT("entry event BeginPlay() {\n# comment @(1, 2)\n}"), Blocks, Errors, /*bSkipReferenceValidation=*/ true);

        TestTrue(TEXT("Parse comment succeeded"), bOk);
        if (Blocks.Num() > 0 && Blocks[0].Instructions.Num() >= 1)
        {
            const FBpirInstruction& Inst = Blocks[0].Instructions[0];
            TestEqual(TEXT("Opcode is Comment"), Inst.Opcode, EBpirOpcode::Comment);
            TestFalse(TEXT("Comment has no authored position"), Inst.bHasAuthoredPosition);
        }
    }

    return true;
}

// ============================================================================
// 69. AuthoredPositionMalformed — malformed position suffixes fail clearly
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirParserAuthoredPositionMalformedTest,
    "PinWright.bpir.parser.AuthoredPositionMalformed",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBpirParserAuthoredPositionMalformedTest::RunTest(const FString& Parameters)
{
    const TCHAR* BadLines[] = {
        TEXT("call PrintString(InString: \"A\") @(10"),
        TEXT("call PrintString(InString: \"A\") @(10, nope)"),
        TEXT("call PrintString(InString: \"A\") @(10, 20) @(30, 40)"),
        TEXT("branch($cond) @(10, 20) [true -> @done]")
    };

    for (const TCHAR* BadLine : BadLines)
    {
        TArray<FBpirEntryBlock> Blocks;
        TArray<FCompileError> Errors;
        bool bOk = ParseSingleInstruction(*this, BadLine, Blocks, Errors);

        TestFalse(FString::Printf(TEXT("Parse should fail: %s"), BadLine), bOk);
        TestTrue(TEXT("Position error reported"), Errors.Num() > 0);
        if (Errors.Num() > 0)
        {
            TestTrue(TEXT("Error mentions position marker"), Errors[0].Message.Contains(TEXT("Position marker")) || Errors[0].Message.Contains(TEXT("position marker")));
        }
    }

    return true;
}

// ============================================================================
// 70. AcceptsOptionalTypeAnnotation — parser accepts "%name: Type = ..."
// register-binding type annotations on call/branch/cast/foreach forms and
// records them on FBpirInstruction without erroring on type mismatch.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirParserAcceptsOptionalTypeAnnotationTest,
    "PinWright.bpir.parser.AcceptsOptionalTypeAnnotation",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBpirParserAcceptsOptionalTypeAnnotationTest::RunTest(const FString& Parameters)
{
    // Annotated call: %n: bool = pure ...
    {
        TArray<FBpirEntryBlock> Blocks;
        TArray<FCompileError> Errors;
        const bool bOk = ParseSingleInstruction(*this,
            TEXT("%n: bool = pure IsValid(self)"), Blocks, Errors);
        TestTrue(TEXT("Annotated pure call parses"), bOk);
        if (Blocks.Num() > 0 && Blocks[0].Instructions.Num() >= 1)
        {
            const FBpirInstruction& Inst = Blocks[0].Instructions[0];
            TestEqual(TEXT("ResultName stripped of annotation"), Inst.ResultName, TEXT("n"));
            TestTrue(TEXT("bHasDeclaredResultType set on annotated call"), Inst.bHasDeclaredResultType);
        }
    }

    // Annotated branch: %b: bool = branch(...)
    {
        TArray<FBpirEntryBlock> Blocks;
        TArray<FCompileError> Errors;
        const bool bOk = ParseMultipleInstructions(*this,
            TEXT("    %c = pure IsValid(self)\n    %b: bool = branch(%c) [true -> @done]\n@done:\n    call PrintString(InString: \"x\")\n"),
            Blocks, Errors);
        TestTrue(TEXT("Annotated branch parses"), bOk);
        if (Blocks.Num() > 0)
        {
            const FBpirInstruction* BranchInst = nullptr;
            for (const FBpirInstruction& I : Blocks[0].Instructions)
            {
                if (I.Opcode == EBpirOpcode::Branch) { BranchInst = &I; break; }
            }
            TestNotNull(TEXT("Found Branch instruction"), BranchInst);
            if (BranchInst)
            {
                TestTrue(TEXT("bHasDeclaredResultType set on annotated branch"), BranchInst->bHasDeclaredResultType);
            }
        }
    }

    // Annotated cast: %c: object<Pawn> = cast<Pawn>(...)
    {
        TArray<FBpirEntryBlock> Blocks;
        TArray<FCompileError> Errors;
        const bool bOk = ParseMultipleInstructions(*this,
            TEXT("    %o = pure GetOwner(self)\n    %c: object<Pawn> = cast<Pawn>(%o) [success -> @ok]\n@ok:\n    call PrintString(InString: \"ok\")\n"),
            Blocks, Errors);
        TestTrue(TEXT("Annotated cast parses"), bOk);
        if (Blocks.Num() > 0)
        {
            const FBpirInstruction* CastInst = nullptr;
            for (const FBpirInstruction& I : Blocks[0].Instructions)
            {
                if (I.Opcode == EBpirOpcode::Cast) { CastInst = &I; break; }
            }
            TestNotNull(TEXT("Found Cast instruction"), CastInst);
            if (CastInst)
            {
                TestTrue(TEXT("bHasDeclaredResultType set on annotated cast"), CastInst->bHasDeclaredResultType);
            }
        }
    }

    // Annotated foreach: %loop: int = foreach(...). The element type is int per
    // the loop's primary output; the annotation is parse-and-record only.
    {
        TArray<FBpirEntryBlock> Blocks;
        TArray<FCompileError> Errors;
        const bool bOk = ParseMultipleInstructions(*this,
            TEXT("    %arr = pure MakeArray()\n    %loop: int = foreach(%arr) [body -> @body, completed -> @done]\n@body:\n@done:\n    call PrintString(InString: \"done\")\n"),
            Blocks, Errors);
        TestTrue(TEXT("Annotated foreach parses"), bOk);
        TestTrue(TEXT("At least one block produced"), Blocks.Num() > 0);
        const FBpirInstruction* LoopInst = nullptr;
        if (Blocks.Num() > 0)
        {
            for (const FBpirInstruction& I : Blocks[0].Instructions)
            {
                if (I.Opcode == EBpirOpcode::Foreach || I.Opcode == EBpirOpcode::ForeachBreak)
                {
                    LoopInst = &I;
                    break;
                }
            }
        }
        TestNotNull(TEXT("Found foreach instruction"), LoopInst);
        if (LoopInst)
        {
            TestTrue(TEXT("bHasDeclaredResultType set on annotated foreach"), LoopInst->bHasDeclaredResultType);
        }
    }

    // Deliberately wrong-typed annotation (advisory semantics): parser must accept
    // and record without producing an error. v1 is parse-and-record only.
    {
        TArray<FBpirEntryBlock> Blocks;
        TArray<FCompileError> Errors;
        const bool bOk = ParseSingleInstruction(*this,
            TEXT("%n: object<Pawn> = pure IsValid(self)"), Blocks, Errors);
        TestTrue(TEXT("Wrong-typed annotation does not block parse"), bOk);
        if (Blocks.Num() > 0 && Blocks[0].Instructions.Num() >= 1)
        {
            const FBpirInstruction& Inst = Blocks[0].Instructions[0];
            TestTrue(TEXT("bHasDeclaredResultType set even when type disagrees with call"),
                Inst.bHasDeclaredResultType);
        }
    }

    // Un-annotated line: bHasDeclaredResultType must be false.
    {
        TArray<FBpirEntryBlock> Blocks;
        TArray<FCompileError> Errors;
        const bool bOk = ParseSingleInstruction(*this,
            TEXT("%n = pure IsValid(self)"), Blocks, Errors);
        TestTrue(TEXT("Un-annotated call parses"), bOk);
        if (Blocks.Num() > 0 && Blocks[0].Instructions.Num() >= 1)
        {
            const FBpirInstruction& Inst = Blocks[0].Instructions[0];
            TestFalse(TEXT("bHasDeclaredResultType is false on un-annotated call"),
                Inst.bHasDeclaredResultType);
        }
    }

    return true;
}

// ============================================================================
// Entry metadata decorators: @meta(...) / @flags(...)
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirParserEntryMetaCategoryTest,
    "PinWright.bpir.parser.EntryMetadata.MetaCategory",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBpirParserEntryMetaCategoryTest::RunTest(const FString& Parameters)
{
    const FString Code = TEXT(
        "@meta(Category=\"Scoring\")\n"
        "entry function Foo() {\n"
        "}\n");

    FBpirParser Parser;
    TArray<FBpirEntryBlock> Blocks;
    TArray<FCompileError> Errors;
    const bool bOk = Parser.Parse(Code, Blocks, Errors);

    TestTrue(TEXT("Parse succeeded"), bOk);
    TestEqual(TEXT("One block"), Blocks.Num(), 1);
    if (Blocks.Num() > 0)
    {
        TestTrue(TEXT("bMetaPresent true"), Blocks[0].Metadata.bMetaPresent);
        TestFalse(TEXT("bFlagsPresent false"), Blocks[0].Metadata.bFlagsPresent);
        TestEqual(TEXT("Category text"), Blocks[0].Metadata.Category.ToString(), FString(TEXT("Scoring")));
    }

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirParserEntryFlagsPurePublicTest,
    "PinWright.bpir.parser.EntryMetadata.FlagsPurePublic",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBpirParserEntryFlagsPurePublicTest::RunTest(const FString& Parameters)
{
    const FString Code = TEXT(
        "@flags(Pure, Public)\n"
        "entry function Foo() {\n"
        "}\n");

    FBpirParser Parser;
    TArray<FBpirEntryBlock> Blocks;
    TArray<FCompileError> Errors;
    const bool bOk = Parser.Parse(Code, Blocks, Errors);

    TestTrue(TEXT("Parse succeeded"), bOk);
    TestEqual(TEXT("One block"), Blocks.Num(), 1);
    if (Blocks.Num() > 0)
    {
        TestTrue(TEXT("bFlagsPresent true"), Blocks[0].Metadata.bFlagsPresent);
        TestTrue(TEXT("bPure set"), Blocks[0].Metadata.bPure);
        TestEqual(TEXT("Access is Public"),
            (int32)Blocks[0].Metadata.Access, (int32)FBpirEntryMetadata::EAccess::Public);
    }

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirParserEntryFlagsConflictingAccessTest,
    "PinWright.bpir.parser.EntryMetadata.FlagsConflictingAccess",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBpirParserEntryFlagsConflictingAccessTest::RunTest(const FString& Parameters)
{
    const FString Code = TEXT(
        "@flags(Public, Private)\n"
        "entry function Foo() {\n"
        "}\n");

    FBpirParser Parser;
    TArray<FBpirEntryBlock> Blocks;
    TArray<FCompileError> Errors;
    const bool bOk = Parser.Parse(Code, Blocks, Errors);

    TestFalse(TEXT("Parse rejects conflicting access"), bOk);
    TestTrue(TEXT("At least one error"), Errors.Num() > 0);

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirParserEntryMetaUnknownKeyTest,
    "PinWright.bpir.parser.EntryMetadata.MetaUnknownKey",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBpirParserEntryMetaUnknownKeyTest::RunTest(const FString& Parameters)
{
    const FString Code = TEXT(
        "@meta(Unknown=\"X\")\n"
        "entry function Foo() {\n"
        "}\n");

    FBpirParser Parser;
    TArray<FBpirEntryBlock> Blocks;
    TArray<FCompileError> Errors;
    const bool bOk = Parser.Parse(Code, Blocks, Errors);

    TestFalse(TEXT("Parse rejects unknown @meta key"), bOk);
    TestTrue(TEXT("At least one error"), Errors.Num() > 0);
    if (Errors.Num() > 0)
    {
        TestTrue(TEXT("Error mentions Unknown"), Errors[0].Message.Contains(TEXT("Unknown")));
        TestTrue(TEXT("Error suggests valid keys"), Errors[0].Message.Contains(TEXT("Valid keys are:")));
    }

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirParserEntryMacroRejectsFlagsTest,
    "PinWright.bpir.parser.EntryMetadata.MacroRejectsFlags",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBpirParserEntryMacroRejectsFlagsTest::RunTest(const FString& Parameters)
{
    const FString Code = TEXT(
        "@flags(Pure)\n"
        "entry macro Foo() {\n"
        "}\n");

    FBpirParser Parser;
    TArray<FBpirEntryBlock> Blocks;
    TArray<FCompileError> Errors;
    const bool bOk = Parser.Parse(Code, Blocks, Errors);

    TestFalse(TEXT("Parse rejects @flags on macro"), bOk);
    TestTrue(TEXT("At least one error"), Errors.Num() > 0);
    bool bSawMacroError = false;
    for (const FCompileError& E : Errors)
    {
        if (E.Message.Contains(TEXT("Macros do not accept @flags")))
        {
            bSawMacroError = true;
            break;
        }
    }
    TestTrue(TEXT("Error explains macros reject @flags"), bSawMacroError);

    return true;
}
