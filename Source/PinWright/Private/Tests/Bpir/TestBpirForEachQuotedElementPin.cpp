// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Misc/AutomationTest.h"
#include "CompilerTestUtils.h"

#include "Compiler/BpirCompiler.h"
#include "Compiler/CompilerTypes.h"
#include "Decompiler/BpirDecompiler.h"
#include "EdGraphSchema_K2.h"
#include "Internationalization/Regex.h"
#include "K2Node_MacroInstance.h"
#include "Kismet2/BlueprintEditorUtils.h"

using namespace CompilerTestUtils;

namespace
{
    void AddForEachQuotedTestArray(UBlueprint* BP)
    {
        FEdGraphPinType ArrayType;
        ArrayType.PinCategory = UEdGraphSchema_K2::PC_String;
        ArrayType.ContainerType = EPinContainerType::Array;
        FBlueprintEditorUtils::AddMemberVariable(BP, TEXT("TestArray"), ArrayType);
    }

    // The macro instance's element output is literally named "Array Element";
    // space-insensitive matching keeps the probe working if a future engine
    // version renames it to the spaceless spelling.
    UEdGraphPin* FindForEachQuotedElementPin(UK2Node_MacroInstance* MacroNode)
    {
        if (!MacroNode)
        {
            return nullptr;
        }
        for (UEdGraphPin* Pin : MacroNode->Pins)
        {
            if (Pin && Pin->Direction == EGPD_Output
                && Pin->PinName.ToString().Replace(TEXT(" "), TEXT("")).Equals(TEXT("ArrayElement"), ESearchCase::IgnoreCase))
            {
                return Pin;
            }
        }
        return nullptr;
    }

    // Strips authored @(x, y) positions so the recompile runs in auto-layout mode:
    // authored-position mode requires every node-backed instruction to carry a
    // position, and the decompile of `$TestArray` produces an implicit VariableGet
    // helper that has no positioned instruction of its own.
    FString StripForEachQuotedPositions(const FString& BpirText)
    {
        const FRegexPattern PositionPattern(TEXT("\\s*@\\(\\s*-?\\d+\\s*,\\s*-?\\d+\\s*\\)"));
        FRegexMatcher Matcher(PositionPattern, BpirText);
        FString Result;
        int32 Cursor = 0;
        while (Matcher.FindNext())
        {
            const int32 Begin = Matcher.GetMatchBeginning();
            const int32 End = Matcher.GetMatchEnding();
            Result.Append(BpirText.Mid(Cursor, Begin - Cursor));
            Cursor = End;
        }
        Result.Append(BpirText.Mid(Cursor));
        return Result;
    }
}

// A backtick-quoted pin segment is the language's spelling for a spaced pin name,
// and it must resolve the same pin as the bare `%loop.ArrayElement` spelling.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FBpirForEachQuotedElementPinAccessTest,
    "PinWright.bpir.compiler.integration.ForEachQuotedElementPinAccess",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBpirForEachQuotedElementPinAccessTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("ForEachQuotedElementPinBP"));
    TestNotNull(TEXT("Blueprint was created"), BP);
    if (!BP) return false;

    AddForEachQuotedTestArray(BP);

    FBpirCompiler Compiler(BP);
    FCompileResult Result = Compiler.Compile(
        TEXT("entry event BeginPlay() {\n")
        TEXT("    %loop = foreach(Array: $TestArray) [body -> @body, completed -> @done]\n")
        TEXT("\n")
        TEXT("@body:\n")
        TEXT("    call PrintString(InString: %loop.`Array Element`)\n")
        TEXT("\n")
        TEXT("@done:\n")
        TEXT("}"));

    if (!Result.bSuccess)
    {
        for (const FCompileError& Err : Result.Errors)
        {
            AddError(FString::Printf(TEXT("  L%d: %s"), Err.Line, *Err.Message));
        }
    }
    TestTrue(TEXT("Compile with backtick-quoted element pin succeeded"), Result.bSuccess);
    if (!Result.bSuccess) return false;

    UK2Node_MacroInstance* LoopNode = FindNodeOfType<UK2Node_MacroInstance>(BP);
    TestNotNull(TEXT("ForEach macro node found"), LoopNode);
    if (!LoopNode) return false;

    UEdGraphPin* ElementPin = FindForEachQuotedElementPin(LoopNode);
    TestNotNull(TEXT("ForEach macro exposes an Array Element output pin"), ElementPin);
    if (!ElementPin) return false;

    TestTrue(TEXT("Array Element output pin is wired into the loop body"), ElementPin->LinkedTo.Num() > 0);

    return true;
}

// The decompiler prints a spaced pin name in backticks, so its own output for a
// ForEach body that consumes the element must compile again unedited.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FBpirForEachElementPinRecompilesTest,
    "PinWright.bpir.round_trip.ForEachElementPinRecompiles",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBpirForEachElementPinRecompilesTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("ForEachElementPinRecompileSourceBP"));
    TestNotNull(TEXT("Source Blueprint was created"), BP);
    if (!BP) return false;

    AddForEachQuotedTestArray(BP);

    FBpirCompiler Compiler(BP);
    FCompileResult CompileResult = Compiler.Compile(
        TEXT("entry event BeginPlay() {\n")
        TEXT("    %loop = foreach(Array: $TestArray) [body -> @body, completed -> @done]\n")
        TEXT("\n")
        TEXT("@body:\n")
        TEXT("    call PrintString(InString: %loop.ArrayElement)\n")
        TEXT("\n")
        TEXT("@done:\n")
        TEXT("}"));

    if (!CompileResult.bSuccess)
    {
        for (const FCompileError& Err : CompileResult.Errors)
        {
            AddError(FString::Printf(TEXT("Initial compile L%d: %s"), Err.Line, *Err.Message));
        }
    }
    TestTrue(TEXT("Initial compile succeeded"), CompileResult.bSuccess);
    if (!CompileResult.bSuccess) return false;

    FBpirDecompiler Decompiler(BP);
    FBpirDecompileResult DecompileResult = Decompiler.Decompile();
    TestTrue(TEXT("Decompile succeeded"), DecompileResult.bSuccess);
    if (!DecompileResult.bSuccess)
    {
        for (const FBpirWarning& Warning : DecompileResult.Warnings)
        {
            AddError(FString::Printf(TEXT("Decompile warning: %s"), *Warning.Text));
        }
        return false;
    }

    TestTrue(TEXT("Decompiled BPIR spells the element pin as `Array Element`"),
        DecompileResult.BpirText.Contains(TEXT(".`Array Element`")));

    UBlueprint* RecompileBP = CreateTransientTestBP(TEXT("ForEachElementPinRecompileTargetBP"));
    TestNotNull(TEXT("Recompile Blueprint was created"), RecompileBP);
    if (!RecompileBP) return false;

    AddForEachQuotedTestArray(RecompileBP);

    FBpirCompiler RecompileCompiler(RecompileBP);
    FCompileResult RecompileResult = RecompileCompiler.Compile(StripForEachQuotedPositions(DecompileResult.BpirText));
    if (!RecompileResult.bSuccess)
    {
        for (const FCompileError& Err : RecompileResult.Errors)
        {
            AddError(FString::Printf(TEXT("Recompile L%d: %s"), Err.Line, *Err.Message));
        }
    }
    TestTrue(TEXT("Recompile of decompiled ForEach element access succeeded"), RecompileResult.bSuccess);

    return true;
}
