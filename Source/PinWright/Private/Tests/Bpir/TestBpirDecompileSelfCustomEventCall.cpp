// Copyright (c) 2026 Alexander Penkin. MIT License.

// TestBpirDecompileSelfCustomEventCall.cpp — regression test for
// B-bpir-decompile-emits-skel-class-prefix-on-self-event-calls.
//
// The decompiler emitted `call SKEL_<Class>::MyEvent_Event()` for self
// custom-event calls when the K2Node's FunctionReference resolved to the
// SkeletonGeneratedClass UFunction (which is what the BPIR resolver hands back
// after Phase 1.5 RegenerateSkeletonOnly — only the SKEL class has the freshly
// regenerated UFunction at that point). The fix is in
// `Decompiler/BpirTextEmitter.cpp::ShouldQualifyFunctionName`: short-circuit
// to `return false` when `Node->FunctionReference.IsSelfContext()` is true.
// Self-context calls never need class qualification regardless of which
// UFunction copy (SKEL vs GEN) the resolver returned.
//
// This test seeds a K2Node_CallFunction with the SKEL UFunction post full
// compile (so both SKEL and GEN UFunction copies exist), mirroring the BPIR
// emit-time state where the resolver returned the SkeletonGeneratedClass copy,
// and asserts the decompiled text contains the bare `call MyEvent_Event(`
// form and no `SKEL_` substring anywhere.
//
// Counterfactual: with both UFunction copies present post-full-compile,
// reverting the production `if (Node->FunctionReference.IsSelfContext()) return false;`
// line in `BpirTextEmitter.cpp::ShouldQualifyFunctionName` causes the cascade
// to compare `BPClass->FindFunctionByName("MyEvent_Event")` (returns the GEN
// UFunction) against `GetTargetFunction()` (returns the SKEL UFunction, because
// the K2Node's FunctionReference was seeded from `SkeletonGeneratedClass`).
// The `Found != BoundFunc` mismatch fires, qualification kicks in,
// `GetFunctionDisplayName` emits `SKEL_TestOrphanBP_<N>::MyEvent_Event()`, and
// the `SKEL_` substring check fails.

#include "Misc/AutomationTest.h"

#include "BpirGraphTestHelpers.h"
#include "Decompiler/BpirDecompiler.h"
#include "EdGraph/EdGraph.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "K2Node_CallFunction.h"
#include "K2Node_CustomEvent.h"
#include "Kismet2/KismetEditorUtilities.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirDecompileSelfCustomEventCallTest,
    "PinWright.bpir.decompiler.SelfCustomEventCallNoSkelPrefix",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBpirDecompileSelfCustomEventCallTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = BpirGraphTestHelpers::CreateOrphanTestBlueprint();
    if (!TestNotNull(TEXT("Test Blueprint created"), BP))
    {
        return false;
    }

    UEdGraph* EventGraph = (BP->UbergraphPages.Num() > 0) ? BP->UbergraphPages[0] : nullptr;
    if (!TestNotNull(TEXT("Test Blueprint has UbergraphPage"), EventGraph))
    {
        return false;
    }

    // Plant the self custom event.
    UK2Node_CustomEvent* CustomEvent =
        BpirGraphTestHelpers::AddNodeToGraph<UK2Node_CustomEvent>(EventGraph);
    CustomEvent->CustomFunctionName = FName(TEXT("MyEvent_Event"));
    CustomEvent->AllocateDefaultPins();
    CustomEvent->ReconstructNode();

    // Full compile so both `SkeletonGeneratedClass` AND `GeneratedClass` hold a
    // `MyEvent_Event` UFunction. The bug-path comparison in
    // `BpirTextEmitter.cpp::ShouldQualifyFunctionName` needs both copies present:
    // it does `BPClass->FindFunctionByName(FuncFName)` against
    // `OwningBP->GeneratedClass` and compares the result against
    // `Node->GetTargetFunction()` — only when both lookups return distinct
    // UFunction objects (one GEN, one SKEL) does the `Found != BoundFunc`
    // mismatch fire and qualification kick in.
    FKismetEditorUtilities::CompileBlueprint(BP);

    UFunction* SkelUFunc = BP->SkeletonGeneratedClass
        ? BP->SkeletonGeneratedClass->FindFunctionByName(FName(TEXT("MyEvent_Event")))
        : nullptr;
    if (!TestNotNull(TEXT("SkeletonGeneratedClass holds MyEvent_Event UFunction"), SkelUFunc))
    {
        return false;
    }
    UFunction* GenUFunc = BP->GeneratedClass
        ? BP->GeneratedClass->FindFunctionByName(FName(TEXT("MyEvent_Event")))
        : nullptr;
    if (!TestNotNull(TEXT("GeneratedClass holds MyEvent_Event UFunction"), GenUFunc))
    {
        return false;
    }
    // The mismatch path requires the two UFunction objects to be distinct.
    if (!TestNotEqual(TEXT("SKEL and GEN UFunctions are distinct objects"),
            (const UFunction*)SkelUFunc, (const UFunction*)GenUFunc))
    {
        return false;
    }

    // Plant the call site and seed it with the SKEL UFunction, mirroring what
    // CodeNodeEmitter::SetFromFunction(Func) does at BPIR emit time when the
    // resolver returned the SkeletonGeneratedClass copy. SetFromFunction copies
    // the UFunction's owning class into the K2Node's FunctionReference, so the
    // CallNode resolves through `SkeletonGeneratedClass` at decompile time —
    // `GetTargetFunction()` hands back the SKEL UFunction while the
    // production cascade's `BPClass->FindFunctionByName` returns the GEN copy.
    UK2Node_CallFunction* CallNode =
        BpirGraphTestHelpers::AddNodeToGraph<UK2Node_CallFunction>(EventGraph);
    CallNode->SetFromFunction(SkelUFunc);
    CallNode->ReconstructNode();

    // Wire BeginPlay -> CallNode so the decompiler walks the call site as a
    // reachable body instruction. BpirGraphTestHelpers::EnsureBeginPlayNode
    // returns an existing or newly added BeginPlay event node.
    UK2Node_Event* BeginPlayNode = BpirGraphTestHelpers::EnsureBeginPlayNode(EventGraph);
    BpirGraphTestHelpers::WireExec(BeginPlayNode, CallNode);

    // Run the decompiler — this exercises the real `FBpirDecompiler` /
    // `BpirTextEmitter.cpp::ShouldQualifyFunctionName` production path.
    FBpirDecompiler Decompiler(BP);
    FBpirDecompileResult Result = Decompiler.Decompile();

    TestTrue(TEXT("decompile succeeded"), Result.bSuccess);
    AddInfo(FString::Printf(TEXT("Decompiled BPIR:\n%s"), *Result.BpirText));

    TestTrue(TEXT("bare call form present"),
        Result.BpirText.Contains(TEXT("call MyEvent_Event(")));
    TestFalse(TEXT("SKEL prefix must not appear anywhere"),
        Result.BpirText.Contains(TEXT("SKEL_")));

    return true;
}
