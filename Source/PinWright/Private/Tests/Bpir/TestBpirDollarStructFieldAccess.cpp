// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression test for the remaining B-bpir-override-param-not-resolvable hole.
//
// After the event-pin registration and const-ref signature fixes landed,
// `$StructParam.Field` on entry params was still broken because
// FBpirValueResolver::PreEmitExternalGet only handled object-typed `$target.Property`
// references. Struct-typed targets hit the hard UClass cast, never populated
// ExternalGetCache, and later failed in ResolveValue with:
//   "External property reference '$HitInfo.bBlockingHit' not found"
//
// This test exercises both struct access shapes through the `$...` path:
// - `$HitLocation.X`         -> FVector native-break path (BreakVector helper)
// - `$HitInfo.bBlockingHit`  -> FHitResult native-break path (BreakHitResult helper)
// Both structs are flagged HasNativeBreak in their USTRUCT metadata, so the
// resolver routes them through the curated helper UFunction instead of
// UK2Node_BreakStruct.  FHitResult in particular has zero BlueprintVisible
// UPROPERTYs — the only working break path is the BreakHitResult helper.

#include "Misc/AutomationTest.h"
#include "CompilerTestUtils.h"

#include "Compiler/BpirCompiler.h"
#include "Compiler/CompilerTypes.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "GameFramework/Actor.h"
#include "K2Node_CallFunction.h"
#include "Kismet2/BlueprintEditorUtils.h"


using namespace CompilerTestUtils;

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirDollarStructFieldAccessTest,
    "PinWright.bpir.compiler.integration.DollarStructFieldAccessResolves",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBpirDollarStructFieldAccessTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("DollarStructFieldBP"));
    TestNotNull(TEXT("Blueprint was created"), BP);
    if (!BP)
    {
        return false;
    }

    FEdGraphPinType FloatType;
    FloatType.PinCategory = UEdGraphSchema_K2::PC_Real;
    FloatType.PinSubCategory = UEdGraphSchema_K2::PC_Float;
    FBlueprintEditorUtils::AddMemberVariable(BP, TEXT("StoredX"), FloatType);

    FEdGraphPinType BoolType;
    BoolType.PinCategory = UEdGraphSchema_K2::PC_Boolean;
    FBlueprintEditorUtils::AddMemberVariable(BP, TEXT("StoredBlockingHit"), BoolType);

    FBpirCompiler Compiler(BP);
    FCompileResult Result = Compiler.Compile(
        TEXT("entry event ReceivePointDamage() {\n")
        TEXT("    set StoredX = $HitLocation.X\n")
        TEXT("    set StoredBlockingHit = $HitInfo.bBlockingHit\n")
        TEXT("}"));

    if (!Result.bSuccess)
    {
        for (const FCompileError& Err : Result.Errors)
        {
            AddError(FString::Printf(TEXT("BPIR compile error  L%d: %s"), Err.Line, *Err.Message));
        }
        return false;
    }

    TestTrue(TEXT("BPIR compile succeeded for $struct.field access on event params"), Result.bSuccess);
    TestEqual(TEXT("No compile errors"), Result.Errors.Num(), 0);
    TestNotNull(TEXT("BreakHitResult call node was created for FHitResult member access"),
        FindCallFunctionBySubstring(BP, TEXT("BreakHitResult")));
    TestNotNull(TEXT("BreakVector call node was created for FVector member access"),
        FindCallFunctionBySubstring(BP, TEXT("BreakVector")));
    return true;
}

// Regression test for E-bpir-dollar-chained-struct-field: `$HitInfo.Location.X` is a
// 2-dot chain through FHitResult (struct) -> Location (FVector struct) -> X (float).
// PreEmitVariableRefs splits only on the first dot, handing
// ("HitInfo", "Location.X") to PreEmitExternalGet. Before the fix,
// ResolveStructMemberThroughPin tried to find a member literally named "Location.X"
// on FHitResult and failed. After the fix, PreEmitExternalGet splits the remainder
// and folds it through ResolveChainFromPin, producing two struct-break operations
// (BreakHitResult helper for FHitResult, BreakVector helper for FVector).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirDollar_ChainedStructFieldAccess_Test,
    "PinWright.bpir.dollar.ChainedStructFieldAccess",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBpirDollar_ChainedStructFieldAccess_Test::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("DollarChainedStructFieldBP"));
    TestNotNull(TEXT("Blueprint was created"), BP);
    if (!BP)
    {
        return false;
    }

    FEdGraphPinType FloatType;
    FloatType.PinCategory = UEdGraphSchema_K2::PC_Real;
    FloatType.PinSubCategory = UEdGraphSchema_K2::PC_Float;
    FBlueprintEditorUtils::AddMemberVariable(BP, TEXT("StoredX"), FloatType);

    FBpirCompiler Compiler(BP);
    FCompileResult Result = Compiler.Compile(
        TEXT("entry event ReceivePointDamage() {\n")
        TEXT("    set StoredX = $HitInfo.Location.X\n")
        TEXT("}"));

    if (!Result.bSuccess)
    {
        for (const FCompileError& Err : Result.Errors)
        {
            AddError(FString::Printf(TEXT("BPIR compile error  L%d: %s"), Err.Line, *Err.Message));
        }
        return false;
    }

    TestTrue(TEXT("BPIR compile succeeded for chained $struct.inner.field access"), Result.bSuccess);
    TestEqual(TEXT("No compile errors"), Result.Errors.Num(), 0);

    // FHitResult has HasNativeBreak → BreakHitResult CallFunction for Location extraction.
    // FVector has HasNativeBreak → BreakVector CallFunction for X extraction.
    // Both should be emitted when the chain walks through two struct boundaries.
    TestNotNull(TEXT("BreakHitResult call node was created for FHitResult -> Location"),
        FindCallFunctionBySubstring(BP, TEXT("BreakHitResult")));
    TestNotNull(TEXT("BreakVector call node was created for FVector -> X"),
        FindCallFunctionBySubstring(BP, TEXT("BreakVector")));
    return true;
}
