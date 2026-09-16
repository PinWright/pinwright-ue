// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression tests for B-bpir-break-struct-pin-not-named.
//
// FHitResult carries two near-synonym bone fields whose break pins are
// `HitBoneName` (the bone that was hit) and `BoneName` (the *tracing* component's
// bone, None for a line trace). A decompiled read that does not name the source pin
// makes the two indistinguishable, and a reviewer auditing a headshot test from the
// IR alone cannot tell which one feeds the comparison.
//
// An explicit `Break Hit Result` node always named its pin. The hole was the graph's
// *inline* break — a split struct pin — which the decompiler did not model at all:
// UEdGraphSchema_K2::SplitPin hides the struct pin and moves the wires onto sub-pins
// named "<ParentPinName>_<MemberName>", and every value reference was resolved from
// the sub-pin as if it were the node's value. For a variable read that collapsed both
// members onto the bare `$Var` (dropping the member outright); elsewhere it emitted
// the raw `Parent_Member` sub-pin name, which no fresh node carries, so the recompile
// could not find it either.
//
// Coverage here: the explicit break node (the ticket's literal scenario), a split
// variable read, and a split ReturnValue read — each with both bone pins wired to
// distinct consumers, asserting the two reads are textually distinct AND that the
// recompiled graph wires each consumer back to the member it named.

#include "Misc/AutomationTest.h"
#include "CompilerTestUtils.h"
#include "BpirGraphTestHelpers.h"

#include "Compiler/BpirCompiler.h"
#include "Compiler/CompilerTypes.h"
#include "Decompiler/BpirDecompiler.h"

#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Engine/HitResult.h"
#include "GameFramework/Actor.h"
#include "K2Node_CallFunction.h"
#include "K2Node_Event.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "Kismet/GameplayStatics.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Internationalization/Regex.h"

using namespace CompilerTestUtils;

// Uniquely prefixed so the unity build cannot collide these with another test TU's
// anonymous-namespace helpers.
namespace BpirStructBreakPinNamingTest
{
    const FName VarHitBone(TEXT("StoredHitBone"));
    const FName VarTraceBone(TEXT("StoredTraceBone"));

    void AddNameVariables(UBlueprint* BP)
    {
        FEdGraphPinType NameType;
        NameType.PinCategory = UEdGraphSchema_K2::PC_Name;
        FBlueprintEditorUtils::AddMemberVariable(BP, VarHitBone, NameType);
        FBlueprintEditorUtils::AddMemberVariable(BP, VarTraceBone, NameType);
        FKismetEditorUtilities::CompileBlueprint(BP);
    }

    UK2Node_VariableSet* SpawnSelfVariableSet(UEdGraph* Graph, const FName VarName)
    {
        UK2Node_VariableSet* Node = SpawnNode<UK2Node_VariableSet>(Graph);
        Node->VariableReference.SetSelfMember(VarName);
        Node->ReconstructNode();
        return Node;
    }

    UK2Node_CallFunction* SpawnGameplayStaticsCall(UEdGraph* Graph, const FName FunctionName)
    {
        UK2Node_CallFunction* Node = SpawnNode<UK2Node_CallFunction>(Graph);
        Node->FunctionReference.SetExternalMember(FunctionName, UGameplayStatics::StaticClass());
        Node->ReconstructNode();
        return Node;
    }

    // Wires SourcePin into the value pin of a `set VarName` node and chains it after
    // PrevExec. Returns the set node so the caller can chain the next one after it.
    UK2Node_VariableSet* WireMemberIntoSet(
        UEdGraph* Graph, UEdGraphNode* PrevExec, UEdGraphPin* SourcePin, const FName VarName)
    {
        if (!SourcePin)
        {
            return nullptr;
        }
        UK2Node_VariableSet* SetNode = SpawnSelfVariableSet(Graph, VarName);
        UEdGraphPin* ValuePin = SetNode->FindPin(VarName, EGPD_Input);
        if (!ValuePin)
        {
            return nullptr;
        }
        ValuePin->MakeLinkTo(SourcePin);
        WireThenToExec(PrevExec, SetNode);
        return SetNode;
    }

    // Name of the output pin feeding `set VarName` in Blueprint, or empty when the
    // variable has no set node or its value pin is unwired. This is the assertion that
    // text distinctness alone cannot make: it proves the recompiled graph reads the
    // member the IR named, not merely that two different strings were printed.
    FString SourcePinNameForSet(UBlueprint* BP, const FName VarName)
    {
        for (UEdGraph* Graph : BP->UbergraphPages)
        {
            if (!Graph) continue;
            for (UEdGraphNode* Node : Graph->Nodes)
            {
                UK2Node_VariableSet* SetNode = ::Cast<UK2Node_VariableSet>(Node);
                if (!SetNode || SetNode->GetVarName() != VarName)
                {
                    continue;
                }
                UEdGraphPin* ValuePin = SetNode->FindPin(VarName, EGPD_Input);
                if (ValuePin && ValuePin->LinkedTo.Num() > 0 && ValuePin->LinkedTo[0])
                {
                    return ValuePin->LinkedTo[0]->PinName.ToString();
                }
            }
        }
        return FString();
    }

    // The single decompiled line assigning VarName, without its trailing `@(x, y)`.
    FString SetLineFor(const FString& BpirText, const FName VarName)
    {
        TArray<FString> Lines;
        BpirText.ParseIntoArrayLines(Lines, /*bCullEmpty=*/false);
        const FString Needle = FString::Printf(TEXT("set %s = "), *VarName.ToString());
        for (const FString& Line : Lines)
        {
            if (Line.Contains(Needle))
            {
                FString Trimmed = Line.TrimStartAndEnd();
                int32 PosIdx = INDEX_NONE;
                if (Trimmed.FindLastChar(TEXT('@'), PosIdx))
                {
                    Trimmed = Trimmed.Left(PosIdx).TrimEnd();
                }
                return Trimmed;
            }
        }
        return FString();
    }

    FString StripAuthoredPositions(FString BpirText)
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

// ============================================================================
// The explicit `Break Hit Result` node — the ticket's literal scenario.
// Both bone outputs feed distinct variable sets; the IR must name each source pin
// and the recompiled graph must preserve which member fed which consumer.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirBreakHitResultBonePinsNamedTest,
    "PinWright.bpir.round_trip.BreakHitResultBonePinsNamed",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBpirBreakHitResultBonePinsNamedTest::RunTest(const FString& Parameters)
{
    using namespace BpirStructBreakPinNamingTest;

    UBlueprint* BP = CreateTransientTestBP(TEXT("BreakHitResultBonePinsBP"));
    TestNotNull(TEXT("Blueprint was created"), BP);
    if (!BP) return false;

    AddNameVariables(BP);

    UEdGraph* EventGraph = (BP->UbergraphPages.Num() > 0) ? BP->UbergraphPages[0] : nullptr;
    TestNotNull(TEXT("Event graph exists"), EventGraph);
    if (!EventGraph) return false;

    UK2Node_Event* EventNode =
        BpirGraphTestHelpers::EnsureEventNode(EventGraph, TEXT("ReceivePointDamage"));
    UEdGraphPin* HitInfoPin = EventNode ? EventNode->FindPin(TEXT("HitInfo"), EGPD_Output) : nullptr;
    TestNotNull(TEXT("PointDamage event exposes its HitInfo pin"), HitInfoPin);
    if (!HitInfoPin) return false;

    UK2Node_CallFunction* BreakNode = SpawnGameplayStaticsCall(EventGraph, TEXT("BreakHitResult"));
    UEdGraphPin* BreakInputPin = BreakNode->FindPin(TEXT("Hit"), EGPD_Input);
    TestNotNull(TEXT("BreakHitResult exposes its Hit input"), BreakInputPin);
    if (!BreakInputPin) return false;
    BreakInputPin->MakeLinkTo(HitInfoPin);

    UEdGraphPin* HitBonePin = BreakNode->FindPin(TEXT("HitBoneName"), EGPD_Output);
    UEdGraphPin* TraceBonePin = BreakNode->FindPin(TEXT("BoneName"), EGPD_Output);
    TestNotNull(TEXT("BreakHitResult exposes HitBoneName"), HitBonePin);
    TestNotNull(TEXT("BreakHitResult exposes BoneName"), TraceBonePin);
    if (!HitBonePin || !TraceBonePin) return false;

    UK2Node_VariableSet* FirstSet = WireMemberIntoSet(EventGraph, EventNode, HitBonePin, VarHitBone);
    TestNotNull(TEXT("HitBoneName consumer was wired"), FirstSet);
    if (!FirstSet) return false;
    TestNotNull(TEXT("BoneName consumer was wired"),
        WireMemberIntoSet(EventGraph, FirstSet, TraceBonePin, VarTraceBone));

    FBpirDecompiler Decompiler(BP);
    FBpirDecompileResult DecompileResult = Decompiler.Decompile();
    TestTrue(TEXT("Decompile succeeded"), DecompileResult.bSuccess);
    if (!DecompileResult.bSuccess) return false;

    const FString HitBoneLine = SetLineFor(DecompileResult.BpirText, VarHitBone);
    const FString TraceBoneLine = SetLineFor(DecompileResult.BpirText, VarTraceBone);
    TestTrue(TEXT("A set line was emitted for the HitBoneName read"), !HitBoneLine.IsEmpty());
    TestTrue(TEXT("A set line was emitted for the BoneName read"), !TraceBoneLine.IsEmpty());

    TestTrue(TEXT("HitBoneName read names its source pin"), HitBoneLine.EndsWith(TEXT(".HitBoneName")));
    TestTrue(TEXT("BoneName read names its source pin"), TraceBoneLine.EndsWith(TEXT(".BoneName")));
    TestFalse(TEXT("The BoneName read is not spelled as the HitBoneName read"),
        TraceBoneLine.EndsWith(TEXT(".HitBoneName")));

    UBlueprint* RecompileBP = CreateTransientTestBP(TEXT("BreakHitResultBonePinsRecompileBP"));
    TestNotNull(TEXT("Recompile Blueprint was created"), RecompileBP);
    if (!RecompileBP) return false;
    AddNameVariables(RecompileBP);

    FBpirCompiler RecompileCompiler(RecompileBP);
    FCompileResult RecompileResult =
        RecompileCompiler.Compile(StripAuthoredPositions(DecompileResult.BpirText));
    for (const FCompileError& Err : RecompileResult.Errors)
    {
        AddError(FString::Printf(TEXT("Recompile L%d: %s"), Err.Line, *Err.Message));
    }
    TestTrue(TEXT("Recompile of the decompiled break reads succeeded"), RecompileResult.bSuccess);
    if (!RecompileResult.bSuccess) return false;

    TestEqual(TEXT("Recompiled StoredHitBone still reads HitBoneName"),
        SourcePinNameForSet(RecompileBP, VarHitBone), FString(TEXT("HitBoneName")));
    TestEqual(TEXT("Recompiled StoredTraceBone still reads BoneName"),
        SourcePinNameForSet(RecompileBP, VarTraceBone), FString(TEXT("BoneName")));
    return true;
}

// ============================================================================
// Split struct pin on a variable read — the shape that actually collapsed.
// Both sub-pins used to resolve to the bare `$LastHit`, dropping the member.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirSplitVariableStructPinNamedTest,
    "PinWright.bpir.round_trip.SplitVariableStructPinNamed",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBpirSplitVariableStructPinNamedTest::RunTest(const FString& Parameters)
{
    using namespace BpirStructBreakPinNamingTest;

    UBlueprint* BP = CreateTransientTestBP(TEXT("SplitVariableStructPinBP"));
    TestNotNull(TEXT("Blueprint was created"), BP);
    if (!BP) return false;

    FEdGraphPinType HitResultType;
    HitResultType.PinCategory = UEdGraphSchema_K2::PC_Struct;
    HitResultType.PinSubCategoryObject = FHitResult::StaticStruct();
    FBlueprintEditorUtils::AddMemberVariable(BP, TEXT("LastHit"), HitResultType);
    AddNameVariables(BP);

    UEdGraph* EventGraph = (BP->UbergraphPages.Num() > 0) ? BP->UbergraphPages[0] : nullptr;
    TestNotNull(TEXT("Event graph exists"), EventGraph);
    if (!EventGraph) return false;

    UK2Node_Event* BeginPlay = BpirGraphTestHelpers::EnsureBeginPlayNode(EventGraph);

    UK2Node_VariableGet* GetNode = SpawnNode<UK2Node_VariableGet>(EventGraph);
    GetNode->VariableReference.SetSelfMember(TEXT("LastHit"));
    GetNode->ReconstructNode();

    UEdGraphPin* LastHitPin = GetNode->FindPin(TEXT("LastHit"), EGPD_Output);
    TestNotNull(TEXT("Variable get exposes its LastHit pin"), LastHitPin);
    if (!LastHitPin) return false;

    GetDefault<UEdGraphSchema_K2>()->SplitPin(LastHitPin, /*bNotify=*/false);

    UEdGraphPin* HitBonePin = GetNode->FindPin(TEXT("LastHit_HitBoneName"), EGPD_Output);
    UEdGraphPin* TraceBonePin = GetNode->FindPin(TEXT("LastHit_BoneName"), EGPD_Output);
    TestNotNull(TEXT("Split produced the HitBoneName sub-pin"), HitBonePin);
    TestNotNull(TEXT("Split produced the BoneName sub-pin"), TraceBonePin);
    if (!HitBonePin || !TraceBonePin) return false;

    UK2Node_VariableSet* FirstSet = WireMemberIntoSet(EventGraph, BeginPlay, HitBonePin, VarHitBone);
    TestNotNull(TEXT("HitBoneName sub-pin consumer was wired"), FirstSet);
    if (!FirstSet) return false;
    TestNotNull(TEXT("BoneName sub-pin consumer was wired"),
        WireMemberIntoSet(EventGraph, FirstSet, TraceBonePin, VarTraceBone));

    FBpirDecompiler Decompiler(BP);
    FBpirDecompileResult DecompileResult = Decompiler.Decompile();
    TestTrue(TEXT("Decompile succeeded"), DecompileResult.bSuccess);
    if (!DecompileResult.bSuccess) return false;

    const FString HitBoneLine = SetLineFor(DecompileResult.BpirText, VarHitBone);
    const FString TraceBoneLine = SetLineFor(DecompileResult.BpirText, VarTraceBone);

    TestEqual(TEXT("Split HitBoneName read names the member on the variable"),
        HitBoneLine, FString(TEXT("set StoredHitBone = $LastHit.HitBoneName")));
    TestEqual(TEXT("Split BoneName read names the member on the variable"),
        TraceBoneLine, FString(TEXT("set StoredTraceBone = $LastHit.BoneName")));
    TestFalse(TEXT("The raw split sub-pin name is not emitted"),
        DecompileResult.BpirText.Contains(TEXT("LastHit_")));

    UBlueprint* RecompileBP = CreateTransientTestBP(TEXT("SplitVariableStructPinRecompileBP"));
    TestNotNull(TEXT("Recompile Blueprint was created"), RecompileBP);
    if (!RecompileBP) return false;
    FBlueprintEditorUtils::AddMemberVariable(RecompileBP, TEXT("LastHit"), HitResultType);
    AddNameVariables(RecompileBP);

    FBpirCompiler RecompileCompiler(RecompileBP);
    FCompileResult RecompileResult =
        RecompileCompiler.Compile(StripAuthoredPositions(DecompileResult.BpirText));
    for (const FCompileError& Err : RecompileResult.Errors)
    {
        AddError(FString::Printf(TEXT("Recompile L%d: %s"), Err.Line, *Err.Message));
    }
    TestTrue(TEXT("Recompile of the decompiled split reads succeeded"), RecompileResult.bSuccess);
    if (!RecompileResult.bSuccess) return false;

    TestEqual(TEXT("Recompiled StoredHitBone reads the HitBoneName member"),
        SourcePinNameForSet(RecompileBP, VarHitBone), FString(TEXT("HitBoneName")));
    TestEqual(TEXT("Recompiled StoredTraceBone reads the BoneName member"),
        SourcePinNameForSet(RecompileBP, VarTraceBone), FString(TEXT("BoneName")));
    return true;
}

// ============================================================================
// Split struct pin on a call node's ReturnValue. The root pin collapses to the
// bare register, so the member is the whole suffix: `%n0.HitBoneName`. Before the
// fix this emitted `%n0.ReturnValue_HitBoneName`, a pin name no freshly created
// node carries, so the recompile could not resolve it back.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirSplitReturnValueStructPinNamedTest,
    "PinWright.bpir.round_trip.SplitReturnValueStructPinNamed",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBpirSplitReturnValueStructPinNamedTest::RunTest(const FString& Parameters)
{
    using namespace BpirStructBreakPinNamingTest;

    UBlueprint* BP = CreateTransientTestBP(TEXT("SplitReturnValueStructPinBP"));
    TestNotNull(TEXT("Blueprint was created"), BP);
    if (!BP) return false;

    AddNameVariables(BP);

    UEdGraph* EventGraph = (BP->UbergraphPages.Num() > 0) ? BP->UbergraphPages[0] : nullptr;
    TestNotNull(TEXT("Event graph exists"), EventGraph);
    if (!EventGraph) return false;

    UK2Node_Event* BeginPlay = BpirGraphTestHelpers::EnsureBeginPlayNode(EventGraph);

    UK2Node_CallFunction* MakeNode = SpawnGameplayStaticsCall(EventGraph, TEXT("MakeHitResult"));
    UEdGraphPin* ReturnPin = MakeNode->FindPin(UEdGraphSchema_K2::PN_ReturnValue, EGPD_Output);
    TestNotNull(TEXT("MakeHitResult exposes its ReturnValue pin"), ReturnPin);
    if (!ReturnPin) return false;

    GetDefault<UEdGraphSchema_K2>()->SplitPin(ReturnPin, /*bNotify=*/false);

    UEdGraphPin* HitBonePin = MakeNode->FindPin(TEXT("ReturnValue_HitBoneName"), EGPD_Output);
    UEdGraphPin* TraceBonePin = MakeNode->FindPin(TEXT("ReturnValue_BoneName"), EGPD_Output);
    TestNotNull(TEXT("Split produced the HitBoneName sub-pin"), HitBonePin);
    TestNotNull(TEXT("Split produced the BoneName sub-pin"), TraceBonePin);
    if (!HitBonePin || !TraceBonePin) return false;

    UK2Node_VariableSet* FirstSet = WireMemberIntoSet(EventGraph, BeginPlay, HitBonePin, VarHitBone);
    TestNotNull(TEXT("HitBoneName sub-pin consumer was wired"), FirstSet);
    if (!FirstSet) return false;
    TestNotNull(TEXT("BoneName sub-pin consumer was wired"),
        WireMemberIntoSet(EventGraph, FirstSet, TraceBonePin, VarTraceBone));

    FBpirDecompiler Decompiler(BP);
    FBpirDecompileResult DecompileResult = Decompiler.Decompile();
    TestTrue(TEXT("Decompile succeeded"), DecompileResult.bSuccess);
    if (!DecompileResult.bSuccess) return false;

    const FString HitBoneLine = SetLineFor(DecompileResult.BpirText, VarHitBone);
    const FString TraceBoneLine = SetLineFor(DecompileResult.BpirText, VarTraceBone);
    TestTrue(TEXT("Split HitBoneName read names its member"),
        HitBoneLine.EndsWith(TEXT(".HitBoneName")));
    TestTrue(TEXT("Split BoneName read names its member"),
        TraceBoneLine.EndsWith(TEXT(".BoneName")));
    TestFalse(TEXT("The BoneName read is not spelled as the HitBoneName read"),
        TraceBoneLine.EndsWith(TEXT(".HitBoneName")));
    TestFalse(TEXT("The raw split sub-pin name is not emitted"),
        DecompileResult.BpirText.Contains(TEXT("ReturnValue_")));
    // The split parent is hidden, so before the fix the type annotation named the
    // first sub-pin's type (bool) rather than the struct the node actually produces.
    TestTrue(TEXT("The register is annotated with the struct, not a member type"),
        DecompileResult.BpirText.Contains(TEXT(": struct<HitResult> = call ")));

    UBlueprint* RecompileBP = CreateTransientTestBP(TEXT("SplitReturnValueStructPinRecompileBP"));
    TestNotNull(TEXT("Recompile Blueprint was created"), RecompileBP);
    if (!RecompileBP) return false;
    AddNameVariables(RecompileBP);

    FBpirCompiler RecompileCompiler(RecompileBP);
    FCompileResult RecompileResult =
        RecompileCompiler.Compile(StripAuthoredPositions(DecompileResult.BpirText));
    for (const FCompileError& Err : RecompileResult.Errors)
    {
        AddError(FString::Printf(TEXT("Recompile L%d: %s"), Err.Line, *Err.Message));
    }
    TestTrue(TEXT("Recompile of the decompiled ReturnValue split reads succeeded"),
        RecompileResult.bSuccess);
    if (!RecompileResult.bSuccess) return false;

    TestEqual(TEXT("Recompiled StoredHitBone reads the HitBoneName member"),
        SourcePinNameForSet(RecompileBP, VarHitBone), FString(TEXT("HitBoneName")));
    TestEqual(TEXT("Recompiled StoredTraceBone reads the BoneName member"),
        SourcePinNameForSet(RecompileBP, VarTraceBone), FString(TEXT("BoneName")));
    return true;
}
