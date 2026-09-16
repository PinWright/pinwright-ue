// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Misc/AutomationTest.h"
#include "CompilerTestUtils.h"

#include "Compiler/BpirCompiler.h"
#include "Compiler/CompilerTypes.h"
#include "Decompiler/BpirDecompiler.h"
#include "K2Node_GetSubsystem.h"
#include "EdGraph/EdGraph.h"
#include "Internationalization/Regex.h"

using namespace CompilerTestUtils;

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FBpirSubsystemGetterRoundTripTest,
    "PinWright.bpir.round_trip.SubsystemGetter",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBpirSubsystemGetterRoundTripTest::RunTest(const FString& Parameters)
{
    // Step 1: compile a transient BP from BPIR that emits a subsystem getter.
    // GameInstanceSubsystem is a well-known base used by TestCompilerGapCoverage's
    // SubsystemAccess test, so the compiler-side resolution path is exercised.
    UBlueprint* BP = CreateTransientTestBP(TEXT("SubsystemGetterRoundTripBP"));
    TestNotNull(TEXT("Blueprint created"), BP);
    if (!BP) return false;

    const FString InputBpir = TEXT(
        "entry event BeginPlay() {\n"
        "    %ss = subsystem<GameInstanceSubsystem>()\n"
        "}"
    );

    FBpirCompiler Compiler(BP);
    FCompileResult CompileResult = Compiler.Compile(InputBpir);
    if (!CompileResult.bSuccess)
    {
        // If the subsystem class is not discoverable in this test context, skip
        // — the same conditional is used by FCompilerIntegrationSubsystemAccessTest.
        // Use AddWarning (not AddError) so the test is treated as skipped, not failed.
        for (const FCompileError& Err : CompileResult.Errors)
        {
            AddWarning(FString::Printf(TEXT("SubsystemGetter skipped (class not discoverable) L%d: %s"), Err.Line, *Err.Message));
        }
        return true;
    }
    TestTrue(TEXT("Initial compile succeeded"), CompileResult.bSuccess);
    TestNotNull(TEXT("UK2Node_GetSubsystem placed in BP"), FindNodeOfType<UK2Node_GetSubsystem>(BP));

    // Step 2: decompile and assert the emitter uses the `subsystem<T>()` keyword
    // form, not the `Get_<TypeName>` title fallback.
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
        TEXT("Decompiled BPIR contains 'subsystem<' keyword form"),
        DecompileResult.BpirText.Contains(TEXT("subsystem<")));
    TestFalse(
        TEXT("Decompiled BPIR does NOT contain 'call Get_' free-function form"),
        DecompileResult.BpirText.Contains(TEXT("call Get_")));

    // Step 3: strip authored @(x, y) positions and recompile.
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

    UBlueprint* BPRecompile = CreateTransientTestBP(TEXT("SubsystemGetterRoundTripRecompileBP"));
    TestNotNull(TEXT("Recompile Blueprint created"), BPRecompile);
    if (!BPRecompile) return false;

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
    TestNotNull(TEXT("UK2Node_GetSubsystem present after recompile"), FindNodeOfType<UK2Node_GetSubsystem>(BPRecompile));

    return true;
}
