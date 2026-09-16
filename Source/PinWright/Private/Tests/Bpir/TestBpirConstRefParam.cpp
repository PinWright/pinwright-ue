// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression test for B-bpir-bind-dispatcher-external-target-local-event:
// BPIR custom_event listener parameters declared as `const struct<T>& Name` must
// emit a UFunction parameter property with both CPF_ConstParm and CPF_ReferenceParm
// set. Without this, any custom_event listener for a
// DYNAMIC_MULTICAST_DELEGATE_*Param dispatcher whose argument is declared as
// `const FStruct&` (the overwhelming majority of engine-defined delegates that
// pass non-primitive types) fails post-compile signature compatibility with
// "does not match the necessary signature".
//
// Counterfactual: if the `const` prefix + `&` suffix handling in
// `FCodePinResolver::ConvertCppTypeToPinType` is reverted, the emitted
// UFunction parameter has CPF_Parm only (missing the ref/const flags), and
// both HasAnyPropertyFlags assertions on CPF_ReferenceParm and CPF_ConstParm
// fail.

#include "Misc/AutomationTest.h"
#include "CompilerTestUtils.h"

#include "Compiler/BpirCompiler.h"
#include "Compiler/CompilerTypes.h"
#include "Engine/Blueprint.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "UObject/UnrealType.h"


using namespace CompilerTestUtils;

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirConstRefStructParamEmitsCorrectFlagsTest,
    "PinWright.bpir.compiler.integration.CustomEventConstRefStructParamEmitsCorrectFlags",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBpirConstRefStructParamEmitsCorrectFlagsTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("ConstRefParamTestBP"));
    TestNotNull(TEXT("Blueprint was created"), BP);
    if (!BP) return false;

    // Uses FVector (engine-provided, always available) so the test doesn't
    // depend on any project struct. The shape mirrors the field-repro failure:
    // a custom_event listener whose param matches a
    // DYNAMIC_MULTICAST_DELEGATE_OneParam(..., const FStruct&, Name) signature.
    FBpirCompiler Compiler(BP);
    FCompileResult Result = Compiler.Compile(
        TEXT("entry custom_event HandleVec(const struct<Vector>& V) {\n")
        TEXT("}"));

    if (!Result.bSuccess)
    {
        for (const FCompileError& Err : Result.Errors)
        {
            AddError(FString::Printf(TEXT("BPIR compile error  L%d: %s"), Err.Line, *Err.Message));
        }
    }
    TestTrue(TEXT("BPIR compile succeeded"), Result.bSuccess);
    if (!Result.bSuccess) return false;

    // Full compile so the UFunction lands on GeneratedClass with its final property flags.
    FKismetEditorUtilities::CompileBlueprint(BP);

    UFunction* Func = BP->GeneratedClass
        ? BP->GeneratedClass->FindFunctionByName(FName(TEXT("HandleVec")), EIncludeSuperFlag::ExcludeSuper)
        : nullptr;
    TestNotNull(TEXT("Custom event 'HandleVec' emitted as UFunction on GeneratedClass"), Func);
    if (!Func) return false;

    // Locate the 'V' parameter property and check its flags.
    FProperty* VParam = Func->FindPropertyByName(FName(TEXT("V")));
    TestNotNull(TEXT("Parameter 'V' exists on UFunction"), VParam);
    if (!VParam) return false;

    FStructProperty* StructParam = CastField<FStructProperty>(VParam);
    TestNotNull(TEXT("Parameter 'V' is FStructProperty"), StructParam);
    if (!StructParam) return false;

    // The actual assertions this test exists for.
    TestTrue(
        TEXT("Parameter 'V' has CPF_Parm (is function parameter)"),
        StructParam->HasAnyPropertyFlags(CPF_Parm));
    TestTrue(
        TEXT("Parameter 'V' has CPF_ReferenceParm (pass-by-ref)"),
        StructParam->HasAnyPropertyFlags(CPF_ReferenceParm));
    TestTrue(
        TEXT("Parameter 'V' has CPF_ConstParm (const)"),
        StructParam->HasAnyPropertyFlags(CPF_ConstParm));

    return true;
}
