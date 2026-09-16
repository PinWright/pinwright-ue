// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Misc/AutomationTest.h"
#include "CompilerTestUtils.h"
#include "BpirGraphTestHelpers.h"

#include "Compiler/BpirCompiler.h"
#include "Compiler/BpirParser.h"
#include "Compiler/BpirTypes.h"
#include "Compiler/CompilerTypes.h"
#include "Decompiler/BpirDecompiler.h"
#include "K2Node.h"
#include "K2Node_AssignmentStatement.h"
#include "K2Node_GetEnumeratorName.h"
#include "EdGraph/EdGraph.h"
#include "Internationalization/Regex.h"

using namespace CompilerTestUtils;
using BpirGraphTestHelpers::FindFirstNodeOfType;

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FBpirCallK2NodeRoundTripTest,
    "PinWright.bpir.round_trip.CallK2Node",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBpirCallK2NodeRoundTripTest::RunTest(const FString& Parameters)
{
    // Sub-test 1: round-trip a generic K2Node fallback through compile -> decompile -> recompile.
    // Try UK2Node_AssignmentStatement first. If it gets rejected at construction due to
    // wildcard-pin compile validation, fall back to UK2Node_GetEnumeratorName (fixed pins).
    {
        UBlueprint* BP = CreateTransientTestBP(TEXT("CallK2NodeRoundTripBP"));
        TestNotNull(TEXT("Blueprint created"), BP);
        if (!BP) return false;

        const FString AssignmentBpir = TEXT(
            "entry event BeginPlay() {\n"
            "    call K2Node_AssignmentStatement(Variable: self, Value: self)\n"
            "}"
        );

        FBpirCompiler Compiler(BP);
        FCompileResult CompileResult = Compiler.Compile(AssignmentBpir);
        FString DecompileNeedle = TEXT("call K2Node_AssignmentStatement(");

        const bool bAssignmentPlaced = CompileResult.bSuccess
            && FindFirstNodeOfType<UK2Node_AssignmentStatement>(BP) != nullptr;

        if (!bAssignmentPlaced)
        {
            BP = CreateTransientTestBP(TEXT("CallK2NodeRoundTripBPFallback"));
            TestNotNull(TEXT("Fallback Blueprint created"), BP);
            if (!BP) return false;

            const FString FallbackBpir = TEXT(
                "entry event BeginPlay() {\n"
                "    %name = call K2Node_GetEnumeratorName()\n"
                "}"
            );
            FBpirCompiler Compiler2(BP);
            FCompileResult R2 = Compiler2.Compile(FallbackBpir);
            if (!R2.bSuccess)
            {
                for (const FCompileError& Err : R2.Errors)
                {
                    AddError(FString::Printf(TEXT("Fallback compile L%d: %s"), Err.Line, *Err.Message));
                }
            }
            TestTrue(TEXT("Fallback compile of K2Node_GetEnumeratorName succeeded"), R2.bSuccess);
            TestNotNull(TEXT("K2Node_GetEnumeratorName placed in BP"), FindFirstNodeOfType<UK2Node_GetEnumeratorName>(BP));
            DecompileNeedle = TEXT("call K2Node_GetEnumeratorName(");
        }

        FBpirDecompiler Decompiler(BP);
        FBpirDecompileResult DecompileResult = Decompiler.Decompile();
        TestTrue(TEXT("Decompile succeeded"), DecompileResult.bSuccess);
        if (!DecompileResult.bSuccess)
        {
            for (const FBpirWarning& Warn : DecompileResult.Warnings)
            {
                AddError(FString::Printf(TEXT("Decompile warning: %s"), *Warn.Text));
            }
            return false;
        }

        TestTrue(
            FString::Printf(TEXT("Decompiled output contains '%s'"), *DecompileNeedle),
            DecompileResult.BpirText.Contains(DecompileNeedle));

        // Strip authored @(x, y) positions from the decompiled output so the recompile
        // runs in auto-layout mode. Authored-position mode requires every visible
        // node-backed instruction to carry @(x, y), but the decompile of `Value: self`
        // produces an implicit K2Node_Self helper that has no positioned instruction.
        // The round-trip exercise here is about call-K2Node syntax, not layout.
        FString StrippedBpir = DecompileResult.BpirText;
        {
            const FRegexPattern PositionPattern(TEXT("\\s*@\\(\\s*-?\\d+\\s*,\\s*-?\\d+\\s*\\)"));
            FRegexMatcher Matcher(PositionPattern, StrippedBpir);
            FString Result;
            int32 Cursor = 0;
            while (Matcher.FindNext())
            {
                const int32 Begin = Matcher.GetMatchBeginning();
                const int32 End = Matcher.GetMatchEnding();
                Result.Append(StrippedBpir.Mid(Cursor, Begin - Cursor));
                Cursor = End;
            }
            Result.Append(StrippedBpir.Mid(Cursor));
            StrippedBpir = MoveTemp(Result);
        }

        UBlueprint* BPRecompile = CreateTransientTestBP(TEXT("CallK2NodeRoundTripRecompileBP"));
        FBpirCompiler RecompileCompiler(BPRecompile);
        FCompileResult RecompileResult = RecompileCompiler.Compile(StrippedBpir);
        if (!RecompileResult.bSuccess)
        {
            for (const FCompileError& Err : RecompileResult.Errors)
            {
                AddError(FString::Printf(TEXT("Recompile L%d: %s"), Err.Line, *Err.Message));
            }
        }
        TestTrue(TEXT("Recompile of decompiled output succeeded"), RecompileResult.bSuccess);
    }

    // Sub-test 2: parser populates Inst.NodeProps when a `node_props { ... }` block
    // follows the args on a K2Node_-prefixed call. Counterfactual: if the node_props
    // slice in ParseCallInstruction is reverted, NodeProps stays empty and this fails.
    {
        const FString PropsBpir = TEXT(
            "entry event BeginPlay() {\n"
            "    call K2Node_AssignmentStatement(Variable: self, Value: self) node_props { bSomeBool: true }\n"
            "}"
        );

        FBpirParser Parser;
        TArray<FBpirEntryBlock> Blocks;
        TArray<FCompileError> ParseErrors;
        const bool bParsed = Parser.Parse(PropsBpir, Blocks, ParseErrors, /*bSkipReferenceValidation=*/true);
        if (!bParsed)
        {
            for (const FCompileError& Err : ParseErrors)
            {
                AddError(FString::Printf(TEXT("NodeProps parse L%d: %s"), Err.Line, *Err.Message));
            }
        }
        TestTrue(TEXT("Parse of call ... node_props { bSomeBool: true } succeeded"), bParsed);

        if (Blocks.Num() > 0 && Blocks[0].Instructions.Num() > 0)
        {
            const FBpirInstruction& Inst = Blocks[0].Instructions[0];
            TestEqual(TEXT("Inst.NodeProps has one entry"), Inst.NodeProps.Num(), 1);
            const FString* Found = Inst.NodeProps.Find(TEXT("bSomeBool"));
            TestNotNull(TEXT("NodeProps['bSomeBool'] present"), Found);
            if (Found)
            {
                TestEqual(TEXT("NodeProps['bSomeBool'] == true"), *Found, FString(TEXT("true")));
            }
        }
        else
        {
            AddError(TEXT("Expected at least one parsed instruction in NodeProps sub-test"));
        }
    }

    return true;
}
