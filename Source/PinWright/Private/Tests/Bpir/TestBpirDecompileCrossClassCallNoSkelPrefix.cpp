// Copyright (c) 2026 Alexander Penkin. MIT License.

// TestBpirDecompileCrossClassCallNoSkelPrefix.cpp — regression test for
// B-bpir-decompile-skel-qualified-cross-class-calls.
//
// The decompiler emitted `call SKEL_<OtherClass>::Func(Target: $obj)` for a
// cross-Blueprint call (an explicit Target: pin to a DIFFERENT Blueprint's
// function) when the K2Node's bound UFunction was the SkeletonGeneratedClass
// copy. `ShouldQualifyFunctionName` (Decompiler/BpirTextEmitter.cpp) compared
// that SKEL BoundFunc against the GeneratedClass UFunction returned by
// `TargetClass->FindFunctionByName` with a raw pointer `!=`; the two are
// DISTINCT objects for the SAME authored function, so the predicate spuriously
// reported a name collision and over-qualified. `StripBPGeneratedClassSuffix`
// chops only `_C`, so the skeleton outer rendered as the bare,
// non-`ResolveUClass`-resolvable token `SKEL_<OtherClass>`, and feeding the
// decompiled text back into compile_bpir failed with
// "Unresolved class 'SKEL_<OtherClass>'".
//
// The fix collapses both the bound function and the cascade-found function to
// their GeneratedClass instance before the identity compare, so the SKEL-vs-GEN
// duplication no longer registers as a collision and the cross-class call emits
// its bare, round-trippable name. This is the NON-self twin of the DONE
// self-event fix (the IsSelfContext() short-circuit), which covered only self
// calls.
//
// Counterfactual: reverting the normalize-before-compare in
// ShouldQualifyFunctionName restores `return Found != BoundFunc`, the SKEL/GEN
// mismatch fires, GetFunctionDisplayName emits `SKEL_<TargetBP>::CrossFoo`, and
// the "no SKEL_ prefix" / "unqualified" assertions below fail.

#include "Misc/AutomationTest.h"

#include "BpirGraphTestHelpers.h"
#include "Decompiler/BpirTextEmitter.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "K2Node_CallFunction.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_DynamicCast.h"
#include "Kismet2/KismetEditorUtilities.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirDecompileCrossClassCallNoSkelPrefixTest,
    "PinWright.bpir.decompiler.CrossClassCallNoSkelPrefix",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBpirDecompileCrossClassCallNoSkelPrefixTest::RunTest(const FString& Parameters)
{
    // --- Target Blueprint: owns the cross-class function `CrossFoo`. ---
    UBlueprint* TargetBP = BpirGraphTestHelpers::CreateOrphanTestBlueprint();
    if (!TestNotNull(TEXT("Target Blueprint created"), TargetBP))
    {
        return false;
    }
    UEdGraph* TargetGraph = (TargetBP->UbergraphPages.Num() > 0) ? TargetBP->UbergraphPages[0] : nullptr;
    if (!TestNotNull(TEXT("Target Blueprint has UbergraphPage"), TargetGraph))
    {
        return false;
    }
    UK2Node_CustomEvent* CrossEvent =
        BpirGraphTestHelpers::AddNodeToGraph<UK2Node_CustomEvent>(TargetGraph);
    CrossEvent->CustomFunctionName = FName(TEXT("CrossFoo"));
    CrossEvent->AllocateDefaultPins();
    CrossEvent->ReconstructNode();

    // Full compile so BOTH SkeletonGeneratedClass and GeneratedClass hold a
    // distinct `CrossFoo` UFunction — the SKEL/GEN duplication the bug rides on.
    FKismetEditorUtilities::CompileBlueprint(TargetBP);

    UClass* TargetSkelClass = TargetBP->SkeletonGeneratedClass;
    UClass* TargetGenClass = TargetBP->GeneratedClass;
    if (!TestNotNull(TEXT("Target has SkeletonGeneratedClass"), TargetSkelClass) ||
        !TestNotNull(TEXT("Target has GeneratedClass"), TargetGenClass))
    {
        return false;
    }
    UFunction* SkelFoo = TargetSkelClass->FindFunctionByName(FName(TEXT("CrossFoo")));
    UFunction* GenFoo = TargetGenClass->FindFunctionByName(FName(TEXT("CrossFoo")));
    if (!TestNotNull(TEXT("SKEL CrossFoo exists"), SkelFoo) ||
        !TestNotNull(TEXT("GEN CrossFoo exists"), GenFoo))
    {
        return false;
    }
    // The mismatch path requires the two UFunction objects to be distinct.
    if (!TestNotEqual(TEXT("SKEL and GEN CrossFoo are distinct objects"),
            (const UFunction*)SkelFoo, (const UFunction*)GenFoo))
    {
        return false;
    }

    // --- Self Blueprint: hosts the cross-class call site. ---
    UBlueprint* SelfBP = BpirGraphTestHelpers::CreateOrphanTestBlueprint();
    if (!TestNotNull(TEXT("Self Blueprint created"), SelfBP))
    {
        return false;
    }
    UEdGraph* SelfGraph = (SelfBP->UbergraphPages.Num() > 0) ? SelfBP->UbergraphPages[0] : nullptr;
    if (!TestNotNull(TEXT("Self Blueprint has UbergraphPage"), SelfGraph))
    {
        return false;
    }

    // A DynamicCast node gives us an output pin typed as the Target's
    // GeneratedClass — the pin `ShouldQualifyFunctionName` reads to derive
    // TargetClass. TargetType must be set BEFORE AllocateDefaultPins so the
    // result pin carries the class (mirrors FCodeNodeEmitter::CreateCastNode).
    UK2Node_DynamicCast* CastNode = NewObject<UK2Node_DynamicCast>(SelfGraph);
    CastNode->TargetType = TargetGenClass;
    CastNode->CreateNewGuid();
    CastNode->PostPlacedNewNode();
    CastNode->AllocateDefaultPins();
    SelfGraph->AddNode(CastNode, /*bFromUI=*/false, /*bSelectNewNode=*/false);
    UEdGraphPin* CastResultPin = CastNode->GetCastResultPin();
    if (!TestNotNull(TEXT("Cast result pin exists"), CastResultPin))
    {
        return false;
    }
    if (!TestTrue(TEXT("Cast result pin is typed as the Target GeneratedClass"),
            CastResultPin->PinType.PinSubCategoryObject.Get() == TargetGenClass))
    {
        return false;
    }

    // The cross-class call, bound to the Target's SKELETON CrossFoo — this is
    // what CodeNodeEmitter::SetFromFunction reproduces at BPIR emit time when the
    // resolver returned the SkeletonGeneratedClass copy.
    UK2Node_CallFunction* CallNode =
        BpirGraphTestHelpers::AddNodeToGraph<UK2Node_CallFunction>(SelfGraph);
    CallNode->SetFromFunction(SkelFoo);
    CallNode->ReconstructNode();

    // Fixture-validity gates: the bug only exists on the NON-self path with a
    // SKELETON-class bound function. If either is false the scenario didn't
    // reproduce and a green result would be vacuous.
    if (!TestFalse(TEXT("Call is NOT self-context (cross-class)"),
            CallNode->FunctionReference.IsSelfContext()))
    {
        return false;
    }
    const UFunction* BoundFunc = CallNode->GetTargetFunction();
    if (!TestNotNull(TEXT("Call has a bound target UFunction"), BoundFunc))
    {
        return false;
    }
    if (!TestTrue(TEXT("Bound function is the SKELETON CrossFoo instance"),
            BoundFunc->GetOuterUClass() == TargetSkelClass))
    {
        return false;
    }

    // Link the Target-typed cast result into the call's self/target pin so the
    // predicate resolves TargetClass = Target GeneratedClass. Raw MakeLinkTo
    // avoids schema rejection of the SKEL-vs-GEN pin-type pairing.
    UEdGraphPin* SelfPin = CallNode->FindPin(UEdGraphSchema_K2::PN_Self, EGPD_Input);
    if (!TestNotNull(TEXT("Call has a self/target pin"), SelfPin))
    {
        return false;
    }
    CastResultPin->MakeLinkTo(SelfPin);

    // Exercise the real production emit path.
    FBpirTextEmitter Emitter;
    const FString DisplayName = Emitter.GetFunctionDisplayName(CallNode);
    AddInfo(FString::Printf(TEXT("GetFunctionDisplayName -> %s"), *DisplayName));

    TestFalse(TEXT("No SKEL_ prefix in the emitted call token"),
        DisplayName.Contains(TEXT("SKEL_")));
    TestFalse(TEXT("Cross-class call is emitted unqualified (no `::`)"),
        DisplayName.Contains(TEXT("::")));
    TestTrue(TEXT("Bare CrossFoo function name is emitted"),
        DisplayName.Contains(TEXT("CrossFoo")));

    return true;
}
