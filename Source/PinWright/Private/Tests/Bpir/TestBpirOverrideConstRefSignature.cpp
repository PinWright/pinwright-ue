// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression test for B-bpir-override-param-not-resolvable (secondary half).
//
// `BlueprintHandlerUtils::GetFunctionSignatureDescriptors` was checking
// `CPF_OutParm` without filtering `CPF_ConstParm`. UE's CPF_ReferenceParm doc
// explicitly states CPF_OutParm is set for ALL reference params — even
// `const T&` — so const-ref inputs were sorted into OutOutputs instead of
// OutInputs. This caused `entry override` of any UFUNCTION whose parent has a
// `const T&` param (the canonical BlueprintImplementableEvent payload pattern)
// to fail signature validation in FBpirCompiler::SetupOverride with
// "Override '<Name>' does not match the parent signature: Input count mismatch:
// expected 1, actual 0" — the parent's const-ref input was misreported as an
// output, dropping the actual-input count to zero.
//
// Test target: AActor::ReceiveHit, which takes `const FHitResult& Hit` as its
// last parameter alongside several non-ref inputs. Pre-fix, ReceiveHit
// introspects as 7 inputs / 1 output (Hit moved to outputs). Post-fix, it
// introspects as 8 inputs / 0 outputs.
//
// Counterfactual: if the `&& !CPF_ConstParm` check is removed from the
// classifier, `Hit` falls into OutOutputs; ActualInputs.Num() drops to 7 and
// ActualOutputs.Num() rises to 1, both assertions below fail.

#include "Misc/AutomationTest.h"

#include "Handlers/Blueprint/BlueprintHandlerUtils.h"
#include "GameFramework/Actor.h"
#include "UObject/Class.h"


IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirOverrideConstRefSignatureClassificationTest,
    "PinWright.bpir.compiler.integration.ConstRefParamClassifiedAsInput",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBpirOverrideConstRefSignatureClassificationTest::RunTest(const FString& Parameters)
{
    UFunction* ReceiveHit = AActor::StaticClass()->FindFunctionByName(FName(TEXT("ReceiveHit")));
    TestNotNull(TEXT("AActor::ReceiveHit UFunction exists"), ReceiveHit);
    if (!ReceiveHit) return false;

    TArray<BlueprintHandlerUtils::FNamedPinTypeDescriptor> Inputs;
    TArray<BlueprintHandlerUtils::FNamedPinTypeDescriptor> Outputs;
    FString Error;

    const bool bOk = BlueprintHandlerUtils::GetFunctionSignatureDescriptors(
        ReceiveHit, Inputs, Outputs, Error);

    if (!bOk)
    {
        AddError(FString::Printf(TEXT("GetFunctionSignatureDescriptors failed: %s"), *Error));
        return false;
    }

    // ReceiveHit signature (AActor.h in UE 5.6):
    //   void ReceiveHit(UPrimitiveComponent* MyComp, AActor* Other,
    //                   UPrimitiveComponent* OtherComp, bool bSelfMoved,
    //                   FVector HitLocation, FVector HitNormal,
    //                   FVector NormalImpulse, const FHitResult& Hit)
    // All 8 are inputs; the const-ref Hit must NOT be classified as output.
    TestEqual(TEXT("ReceiveHit input count is 8 (const-ref Hit is input, not output)"),
        Inputs.Num(), 8);
    TestEqual(TEXT("ReceiveHit output count is 0 (no return value, no out params)"),
        Outputs.Num(), 0);

    // Locate Hit specifically and assert it's in Inputs.
    bool bFoundHitInInputs = false;
    for (const auto& Desc : Inputs)
    {
        if (Desc.Name.Equals(TEXT("Hit"), ESearchCase::IgnoreCase))
        {
            bFoundHitInInputs = true;
            break;
        }
    }
    TestTrue(TEXT("ReceiveHit's 'Hit' (const FHitResult&) param is classified as input"),
        bFoundHitInInputs);

    return true;
}
