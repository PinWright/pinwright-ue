// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression tests for B-switch-int-labels-shift-on-load.
//
// UK2Node_SwitchInteger persists no per-case value: a case pin's *name* is the
// case value (UK2Node_Switch::GetExportTextForPin returns Pin->PinName), and
// nothing records which arm was authored for which label. So
// UK2Node_SwitchInteger::ReallocatePinsDuringReconstruction rebuilds the labels
// positionally -- it walks the exec output pins in pin order and renames each one
// to StartIndex, StartIndex + 1, ... That runs on compile-on-load
// (FBlueprintEditorUtils::ReconstructAllNodes under bIsRegeneratingOnLoad), on a
// StartIndex / bHasDefaultPin property change, and on a manual refresh -- but not
// on the in-editor compile that authored the node.
//
// The compiler used to create the case pins named exactly as authored, in text
// order, leaving StartIndex at its 0 default. Case labels [1, 2, 3] therefore
// came back as [0, 1, 2] after the next load, with every arm shifted by one and
// nothing logged at any point. The fix anchors StartIndex to the lowest label,
// lays the case pins out in ascending order, and rejects a label set that has no
// stable representation at all.
//
// Counterfactual -- a test that only compiles and counts nodes proves nothing
// here: the broken compile succeeded and produced exactly one SwitchInteger node.
// What these tests assert instead is the label -> arm mapping ACROSS a
// reconstruction, which is where the corruption happened:
//   * CaseLabelsSurviveReconstruction: before the fix the node reconstructs to
//     pins {0, 1, 2} -- pin "3" is gone and pin "1" runs the arm authored for 2.
//   * OutOfOrderLabelsAreLaidOutAscending: before the fix the pins were created
//     in text order [3, 1, 2] and reconstruct to [0, 1, 2], so every arm moves.
//   * NonContiguousLabelsRejected: before the fix this compiled clean and built a
//     node whose meaning changed on the next load.

#include "Misc/AutomationTest.h"

#include "CompilerTestUtils.h"

#include "Compiler/BpirCompiler.h"
#include "Compiler/CompilerTypes.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "K2Node_SwitchInteger.h"
#include "Kismet2/BlueprintEditorUtils.h"

using namespace CompilerTestUtils;

// Named (not anonymous) namespace: these files are merged by Unity builds, where
// two anonymous namespaces in different files become one and collide on name.
namespace PinWrightSwitchIntCaseLabelTest
{
    // Marker string carried by the PrintString call at the head of each case arm.
    // Reading it back through a case pin's link is how "which arm does this label
    // run?" is answered without depending on node ordering.
    const TCHAR* const ArmOne = TEXT("CASE_ARM_ONE");
    const TCHAR* const ArmTwo = TEXT("CASE_ARM_TWO");
    const TCHAR* const ArmThree = TEXT("CASE_ARM_THREE");

    // Case pin names in Pins order, excluding the always-present "Default" pin.
    // Order matters: the engine's renumber is positional over exactly this list.
    TArray<FString> GatherCaseLabels(const UK2Node_SwitchInteger* SwitchNode)
    {
        TArray<FString> Labels;
        if (!SwitchNode)
        {
            return Labels;
        }
        for (const UEdGraphPin* Pin : SwitchNode->Pins)
        {
            if (Pin
                && Pin->Direction == EGPD_Output
                && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec
                && Pin->PinName != TEXT("Default"))
            {
                Labels.Add(Pin->PinName.ToString());
            }
        }
        return Labels;
    }

    // Follows one case pin to the head of its arm and returns that arm's marker,
    // or an empty string when the pin is missing or unwired.
    FString ResolveArmMarker(UK2Node_SwitchInteger* SwitchNode, const TCHAR* CasePinName)
    {
        if (!SwitchNode)
        {
            return FString();
        }
        UEdGraphPin* CasePin = SwitchNode->FindPin(CasePinName, EGPD_Output);
        if (!CasePin || CasePin->LinkedTo.Num() == 0 || !CasePin->LinkedTo[0])
        {
            return FString();
        }
        UEdGraphNode* ArmHead = CasePin->LinkedTo[0]->GetOwningNode();
        UEdGraphPin* MessagePin = ArmHead ? ArmHead->FindPin(TEXT("InString"), EGPD_Input) : nullptr;
        return MessagePin ? MessagePin->DefaultValue : FString();
    }

    // Three-arm switch_int source with the case labels supplied by the caller, so
    // the same body can be authored contiguous-from-one or shuffled.
    FString MakeThreeArmSource(const TCHAR* FirstLabel, const TCHAR* SecondLabel, const TCHAR* ThirdLabel)
    {
        return FString::Printf(TEXT(
            "entry event BeginPlay() {\n"
            "    %%s = switch_int(0) [%s -> @a, %s -> @b, %s -> @c, default -> @d]\n"
            "\n"
            "@a:\n"
            "    call PrintString(InString: \"%s\")\n"
            "    exec -> @d\n"
            "\n"
            "@b:\n"
            "    call PrintString(InString: \"%s\")\n"
            "    exec -> @d\n"
            "\n"
            "@c:\n"
            "    call PrintString(InString: \"%s\")\n"
            "    exec -> @d\n"
            "\n"
            "@d:\n"
            "}"),
            FirstLabel, SecondLabel, ThirdLabel, ArmOne, ArmTwo, ArmThree);
    }
}

// Alias rather than a using-directive: these helper names would otherwise be
// visible to every later file in the same Unity translation unit.
namespace SwInt = PinWrightSwitchIntCaseLabelTest;

// ----------------------------------------------------------------------------
// 1. Case labels that do not start at 0 must mean the same thing after the
//    reconstruction a load performs.
// ----------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FBpirSwitchIntCaseLabelsSurviveReconstructionTest,
    "PinWright.bpir.switch_int.CaseLabelsSurviveReconstruction",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBpirSwitchIntCaseLabelsSurviveReconstructionTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("SwitchIntCaseLabelBP"));
    TestNotNull(TEXT("Blueprint created"), BP);
    if (!BP) return false;

    FBpirCompiler Compiler(BP);
    FCompileResult Result = Compiler.Compile(SwInt::MakeThreeArmSource(TEXT("1"), TEXT("2"), TEXT("3")));
    if (!Result.bSuccess)
    {
        for (const FCompileError& Err : Result.Errors)
        {
            AddError(FString::Printf(TEXT("Compile L%d: %s"), Err.Line, *Err.Message));
        }
    }
    TestTrue(TEXT("Compile succeeded"), Result.bSuccess);
    if (!Result.bSuccess) return false;

    UK2Node_SwitchInteger* SwitchNode = FindNodeOfType<UK2Node_SwitchInteger>(BP);
    TestNotNull(TEXT("SwitchInteger node was created"), SwitchNode);
    if (!SwitchNode) return false;

    // StartIndex is the only thing that anchors the renumber. Left at 0 for
    // labels starting at 1, the reconstruction below shifts every arm.
    TestEqual(TEXT("StartIndex is anchored to the lowest case label"), SwitchNode->StartIndex, 1);

    const TArray<FString> LabelsBefore = SwInt::GatherCaseLabels(SwitchNode);
    TestEqual(TEXT("Three case pins before reconstruction"), LabelsBefore.Num(), 3);

    TestEqual(TEXT("Case 1 runs the first arm before reconstruction"),
        SwInt::ResolveArmMarker(SwitchNode, TEXT("1")), FString(SwInt::ArmOne));
    TestEqual(TEXT("Case 2 runs the second arm before reconstruction"),
        SwInt::ResolveArmMarker(SwitchNode, TEXT("2")), FString(SwInt::ArmTwo));
    TestEqual(TEXT("Case 3 runs the third arm before reconstruction"),
        SwInt::ResolveArmMarker(SwitchNode, TEXT("3")), FString(SwInt::ArmThree));

    // The load path. BlueprintCompilationManager takes exactly this branch for a
    // Blueprint regenerating on load, and it is what renames the case pins.
    FBlueprintEditorUtils::ReconstructAllNodes(BP);

    UK2Node_SwitchInteger* ReloadedNode = FindNodeOfType<UK2Node_SwitchInteger>(BP);
    TestNotNull(TEXT("SwitchInteger node survives reconstruction"), ReloadedNode);
    if (!ReloadedNode) return false;

    const TArray<FString> LabelsAfter = SwInt::GatherCaseLabels(ReloadedNode);
    TestEqual(TEXT("Still three case pins after reconstruction"), LabelsAfter.Num(), 3);
    TestEqual(TEXT("Case pin names are unchanged by reconstruction"),
        FString::Join(LabelsAfter, TEXT(",")), FString::Join(LabelsBefore, TEXT(",")));
    TestEqual(TEXT("Case pin names are still 1,2,3"),
        FString::Join(LabelsAfter, TEXT(",")), FString(TEXT("1,2,3")));

    // The assertion the ticket is about: same label, same arm.
    TestEqual(TEXT("Case 1 still runs the first arm after reconstruction"),
        SwInt::ResolveArmMarker(ReloadedNode, TEXT("1")), FString(SwInt::ArmOne));
    TestEqual(TEXT("Case 2 still runs the second arm after reconstruction"),
        SwInt::ResolveArmMarker(ReloadedNode, TEXT("2")), FString(SwInt::ArmTwo));
    TestEqual(TEXT("Case 3 still runs the third arm after reconstruction"),
        SwInt::ResolveArmMarker(ReloadedNode, TEXT("3")), FString(SwInt::ArmThree));

    return true;
}

// ----------------------------------------------------------------------------
// 2. Case labels written out of order must be laid out ascending, because the
//    engine's renumber is positional and would otherwise reassign the arms.
// ----------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FBpirSwitchIntOutOfOrderLabelsTest,
    "PinWright.bpir.switch_int.OutOfOrderLabelsAreLaidOutAscending",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBpirSwitchIntOutOfOrderLabelsTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("SwitchIntOutOfOrderLabelBP"));
    TestNotNull(TEXT("Blueprint created"), BP);
    if (!BP) return false;

    // Authored 3, 1, 2 -- the arm markers still follow the @a/@b/@c blocks, so
    // case 3 runs arm one, case 1 runs arm two, case 2 runs arm three.
    FBpirCompiler Compiler(BP);
    FCompileResult Result = Compiler.Compile(SwInt::MakeThreeArmSource(TEXT("3"), TEXT("1"), TEXT("2")));
    if (!Result.bSuccess)
    {
        for (const FCompileError& Err : Result.Errors)
        {
            AddError(FString::Printf(TEXT("Compile L%d: %s"), Err.Line, *Err.Message));
        }
    }
    TestTrue(TEXT("Compile succeeded"), Result.bSuccess);
    if (!Result.bSuccess) return false;

    UK2Node_SwitchInteger* SwitchNode = FindNodeOfType<UK2Node_SwitchInteger>(BP);
    TestNotNull(TEXT("SwitchInteger node was created"), SwitchNode);
    if (!SwitchNode) return false;

    TestEqual(TEXT("StartIndex is anchored to the lowest case label"), SwitchNode->StartIndex, 1);
    TestEqual(TEXT("Case pins are laid out in ascending order, not text order"),
        FString::Join(SwInt::GatherCaseLabels(SwitchNode), TEXT(",")), FString(TEXT("1,2,3")));

    FBlueprintEditorUtils::ReconstructAllNodes(BP);

    UK2Node_SwitchInteger* ReloadedNode = FindNodeOfType<UK2Node_SwitchInteger>(BP);
    TestNotNull(TEXT("SwitchInteger node survives reconstruction"), ReloadedNode);
    if (!ReloadedNode) return false;

    TestEqual(TEXT("Case pin order is unchanged by reconstruction"),
        FString::Join(SwInt::GatherCaseLabels(ReloadedNode), TEXT(",")), FString(TEXT("1,2,3")));
    TestEqual(TEXT("Case 3 still runs the arm it was authored with"),
        SwInt::ResolveArmMarker(ReloadedNode, TEXT("3")), FString(SwInt::ArmOne));
    TestEqual(TEXT("Case 1 still runs the arm it was authored with"),
        SwInt::ResolveArmMarker(ReloadedNode, TEXT("1")), FString(SwInt::ArmTwo));
    TestEqual(TEXT("Case 2 still runs the arm it was authored with"),
        SwInt::ResolveArmMarker(ReloadedNode, TEXT("2")), FString(SwInt::ArmThree));

    return true;
}

// ----------------------------------------------------------------------------
// 3. A label set the engine cannot represent must be rejected loudly rather than
//    compiled into a node that changes meaning on the next load.
// ----------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FBpirSwitchIntNonContiguousLabelsRejectedTest,
    "PinWright.bpir.switch_int.NonContiguousLabelsRejected",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBpirSwitchIntNonContiguousLabelsRejectedTest::RunTest(const FString& Parameters)
{
    // Gapped labels: no StartIndex reproduces 1, 5, 9 under the positional
    // renumber, so there is nothing to compile that would still mean this.
    {
        UBlueprint* BP = CreateTransientTestBP(TEXT("SwitchIntGappedLabelBP"));
        TestNotNull(TEXT("Blueprint created"), BP);
        if (!BP) return false;

        FBpirCompiler Compiler(BP);
        FCompileResult Result = Compiler.Compile(SwInt::MakeThreeArmSource(TEXT("1"), TEXT("5"), TEXT("9")));

        TestFalse(TEXT("Gapped case labels are rejected"), Result.bSuccess);
        TestTrue(TEXT("The error names the contiguity constraint"),
            ErrorsContain(Result.Errors, TEXT("contiguous")));
        TestNull(TEXT("No SwitchInteger node is left behind"),
            FindNodeOfType<UK2Node_SwitchInteger>(BP));
    }

    // Duplicate labels: two pins would share a name, and only the first would
    // ever be found -- the second arm becomes unreachable with nothing logged.
    {
        UBlueprint* BP = CreateTransientTestBP(TEXT("SwitchIntDuplicateLabelBP"));
        TestNotNull(TEXT("Blueprint created"), BP);
        if (!BP) return false;

        FBpirCompiler Compiler(BP);
        FCompileResult Result = Compiler.Compile(SwInt::MakeThreeArmSource(TEXT("1"), TEXT("1"), TEXT("2")));

        TestFalse(TEXT("Duplicate case labels are rejected"), Result.bSuccess);
        TestTrue(TEXT("The error names the duplicate"),
            ErrorsContain(Result.Errors, TEXT("used twice")));
        TestNull(TEXT("No SwitchInteger node is left behind"),
            FindNodeOfType<UK2Node_SwitchInteger>(BP));
    }

    // A non-decimal label would be rewritten to canonical form by the engine, so
    // the pin the compiler wired is not the pin that survives.
    {
        UBlueprint* BP = CreateTransientTestBP(TEXT("SwitchIntNonDecimalLabelBP"));
        TestNotNull(TEXT("Blueprint created"), BP);
        if (!BP) return false;

        FBpirCompiler Compiler(BP);
        FCompileResult Result = Compiler.Compile(SwInt::MakeThreeArmSource(TEXT("01"), TEXT("2"), TEXT("3")));

        TestFalse(TEXT("A non-canonical decimal case label is rejected"), Result.bSuccess);
        TestTrue(TEXT("The error names the decimal-integer constraint"),
            ErrorsContain(Result.Errors, TEXT("plain decimal integer")));
        TestNull(TEXT("No SwitchInteger node is left behind"),
            FindNodeOfType<UK2Node_SwitchInteger>(BP));
    }

    return true;
}
