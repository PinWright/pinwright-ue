// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression test for B-bpir-override-param-not-resolvable.
//
// Before the fix, FBpirCompiler::SetupBuiltinEvent only registered output pins
// for ~5 hardcoded engine event names (ReceiveTick, ReceiveActorBegin/EndOverlap,
// ReceiveHit, ReceiveAnyDamage, ReceiveEndPlay). For arbitrary
// BlueprintImplementableEvent / BlueprintNativeEvent overrides authored as
// `entry event <Name>(...)`, the K2Node_Event was created with the parent
// UFUNCTION's output pins (via AllocateDefaultPins) but those pins were never
// registered with the PinResolver — so any body reference to a parameter
// (`$ParamName` or bare `ParamName`) failed with "variable not found".
//
// The fix replaces the hardcoded if/else cascade with a generic loop that walks
// all output non-exec / non-delegate pins on the event node and registers each.
//
// This test uses AActor::ReceivePointDamage — a BlueprintImplementableEvent NOT
// in the previously hardcoded list — and references its `Damage` (float) param
// from the body. Without the fix, `ReceivePointDamage` matches none of the
// hardcoded event names, the resolver has no `Damage` entry, and `set
// StoredDamage = $Damage` errors out with "variable 'Damage' not found".

#include "Misc/AutomationTest.h"
#include "CompilerTestUtils.h"

#include "Compiler/BpirCompiler.h"
#include "Compiler/CompilerTypes.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "GameFramework/Actor.h"
#include "Kismet2/BlueprintEditorUtils.h"


using namespace CompilerTestUtils;

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirOverrideEventParamResolutionTest,
    "PinWright.bpir.compiler.integration.NonHardcodedBIEParamResolves",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBpirOverrideEventParamResolutionTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("OverrideBIEParamBP"));
    TestNotNull(TEXT("Blueprint was created"), BP);
    if (!BP) return false;

    // Member float to receive `$Damage` via `set` — gives the body something
    // type-compatible to wire the resolved pin into.
    FEdGraphPinType FloatType;
    FloatType.PinCategory = UEdGraphSchema_K2::PC_Float;
    FBlueprintEditorUtils::AddMemberVariable(BP, TEXT("StoredDamage"), FloatType);

    // ReceivePointDamage is a BlueprintImplementableEvent on AActor. It is NOT
    // in the previously hardcoded special-case list in SetupBuiltinEvent, so
    // without the fix `$Damage` would not be registered with PinResolver and
    // the `set` would fail with a "variable 'Damage' not found" error.
    FBpirCompiler Compiler(BP);
    FCompileResult Result = Compiler.Compile(
        TEXT("entry event ReceivePointDamage() {\n")
        TEXT("    set StoredDamage = $Damage\n")
        TEXT("}"));

    if (!Result.bSuccess)
    {
        for (const FCompileError& Err : Result.Errors)
        {
            AddError(FString::Printf(TEXT("BPIR compile error  L%d: %s"), Err.Line, *Err.Message));
        }
    }
    TestTrue(TEXT("BPIR compile succeeded — non-hardcoded BIE param '$Damage' resolved"),
        Result.bSuccess);

    return Result.bSuccess;
}
