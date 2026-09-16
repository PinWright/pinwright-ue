// Copyright (c) 2026 Alexander Penkin. MIT License.

// CRIR node-kind coverage gate.
//
// Enumerates every UClass derived from URigVMNode at runtime, then partitions
// them into one of three sets: Covered (a CRIR.RoundTrip.<X> test exists),
// Uncovered (explicit decision with reason, e.g. WONTFIX'd), or structural
// intermediates (concrete in UCLASS terms but never instantiated user-facing).
// If a class falls into none of these, the engine has added a new leaf and the
// test fails loudly so the matrix is kept in sync.
//
// Counterfactual: if the URigVMVariableNode entry is removed from Covered or
// if a new concrete URigVMNode subclass appears in the engine without being
// added here, the test fails with the offending class names listed.
//
// `Covered` used to be a bare TSet whose "a round-trip test exists" claim was
// carried only in a trailing `//` comment — nothing checked it. Deleting
// TestCRIRComment.cpp left `RigVMCommentNode` in the set and the gate still
// passed, so the matrix could assert coverage that no longer existed. Each
// entry now names the covering test's C++ automation class and the gate asserts
// that class is actually registered with FAutomationTestFramework.
#include "Misc/AutomationTest.h"

#include "RigVMModel/RigVMNode.h"
#include "UObject/UObjectIterator.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCRIRNodeKindCoverageGate,
    "PinWright.CRIR.Coverage.NodeKindMatrix",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCRIRNodeKindCoverageGate::RunTest(const FString& Parameters)
{
    // Classes with a dedicated CRIR.RoundTrip.<X> test in Tests/Assets/, mapped
    // to the C++ automation class of the test that covers them. The class name
    // is the key FAutomationTestFramework registers each IMPLEMENT_SIMPLE_-
    // AUTOMATION_TEST instance under (the macro instantiates with TEXT(#TClass)),
    // so ContainsTest() below is a live check that the claimed test still exists.
    static const TMap<FName, FString> Covered = {
        { FName(TEXT("RigVMUnitNode")),              TEXT("FCRIRRoundTripForwardsSolveTest") },
        { FName(TEXT("RigVMTemplateNode")),          TEXT("FCRIRTemplate_RoundTrip") },
        { FName(TEXT("RigVMDispatchNode")),          TEXT("FCRIRIfSelect_RoundTrip") },
        { FName(TEXT("RigVMVariableNode")),          TEXT("FCRIRVariable_RoundTrip") },
        { FName(TEXT("RigVMCommentNode")),           TEXT("FCRIRComment_RoundTrip") },
        { FName(TEXT("RigVMRerouteNode")),           TEXT("FCRIRReroute_RoundTrip") },
        { FName(TEXT("RigVMEnumNode")),              TEXT("FCRIREnum_RoundTrip") },
        { FName(TEXT("RigVMInvokeEntryNode")),       TEXT("FCRIRInvokeEntry_RoundTrip") },
        { FName(TEXT("RigVMCollapseNode")),          TEXT("FCRIRCollapse_SingleLevel_RoundTrip") },
        { FName(TEXT("RigVMFunctionReferenceNode")), TEXT("FCRIRFunctionRef_LibraryAndRef_RoundTrip") },
        { FName(TEXT("RigVMAggregateNode")),         TEXT("FCRIRDecompiler_AggregateNode_RoundTripsAsCollapse_DegradedClass") },
        // Function entry/return nodes are auto-generated inside collapse and
        // function-reference contained graphs; covered indirectly by the
        // collapse and function-ref round-trip tests rather than standalone.
        { FName(TEXT("RigVMFunctionEntryNode")),     TEXT("FCRIRFunctionInterface_PinDefaults_Reconcile") },
        { FName(TEXT("RigVMFunctionReturnNode")),    TEXT("FCRIRFunctionInterface_PinDefaults_Reconcile") },
    };

    // Classes intentionally not covered, each with a reason.
    //
    // The If/Select/Branch/Array entries below are the engine-deprecated leaf
    // classes (UCLASS Deprecated specifier — C++ type is UDEPRECATED_*; the
    // reflected name strips the prefix). On URigVMController, AddIfNode /
    // AddSelectNode / AddBranchNode / AddArrayNode all build URigVMDispatchNode
    // instances via the RigVMDispatch_If / RigVMDispatch_SelectInt32 /
    // RigVMDispatch_Array* templates; the real round-trips for these surfaces
    // are covered by CRIR.RoundTrip.IfSelect and the dispatch-node arm of
    // CRIR.RoundTrip.TemplateAndDispatch. The deprecated classes themselves
    // only appear on 5.1-and-earlier assets that haven't been fixed up, and
    // CRIR's existing TODO arm in the decompiler handles any stray legacy data.
    static const TMap<FName, FString> Uncovered = {
        { FName(TEXT("RigVMParameterNode")),
          TEXT("Deprecated upstream node kind — WONTFIX per board entry F-crir-parameter-node-wontfix.") },
        { FName(TEXT("RigVMIfNode")),
          TEXT("Deprecated upstream node kind (UDEPRECATED_RigVMIfNode) — superseded by URigVMDispatchNode + RigVMDispatch_If; real round-trip covered by CRIR.RoundTrip.IfSelect. WONTFIX per board entry F-crir-deprecated-node-kinds-wontfix.") },
        { FName(TEXT("RigVMSelectNode")),
          TEXT("Deprecated upstream node kind (UDEPRECATED_RigVMSelectNode) — superseded by URigVMDispatchNode + RigVMDispatch_SelectInt32; real round-trip covered by CRIR.RoundTrip.IfSelect. WONTFIX per board entry F-crir-deprecated-node-kinds-wontfix.") },
        { FName(TEXT("RigVMBranchNode")),
          TEXT("Deprecated upstream node kind (UDEPRECATED_RigVMBranchNode) — superseded by control-flow on dispatch/unit nodes. WONTFIX per board entry F-crir-deprecated-node-kinds-wontfix.") },
        { FName(TEXT("RigVMArrayNode")),
          TEXT("Deprecated upstream node kind (UDEPRECATED_RigVMArrayNode) — superseded by URigVMDispatchNode + RigVMDispatch_Array* (e.g. ArrayAdd covered by CRIR.RoundTrip.TemplateAndDispatch). WONTFIX per board entry F-crir-deprecated-node-kinds-wontfix.") },
    };

    // Concrete-in-UCLASS-terms but never user-instantiated: these are the
    // intermediate bases for the leaf classes already covered above. The
    // RigVMLibraryNode hierarchy resolves to Collapse / FunctionReference /
    // Aggregate; the FunctionInterfaceNode hierarchy resolves to FunctionEntry
    // / FunctionReturn. Skipping is safe because any real round-trip surface
    // they expose lands in one of their leaf subclasses, which are tested.
    static const TSet<FName> StructuralBases = {
        FName(TEXT("RigVMLibraryNode")),
        FName(TEXT("RigVMFunctionInterfaceNode")),
    };

    UClass* RigVMNodeBase = URigVMNode::StaticClass();
    TArray<FString> Missing;
    for (TObjectIterator<UClass> It; It; ++It)
    {
        UClass* Cls = *It;
        if (Cls == RigVMNodeBase) { continue; }
        if (!Cls->IsChildOf(RigVMNodeBase)) { continue; }
        if (Cls->HasAnyClassFlags(CLASS_Abstract)) { continue; }
        const FName Name(*Cls->GetName());
        if (Covered.Contains(Name)) { continue; }
        if (Uncovered.Contains(Name)) { continue; }
        if (StructuralBases.Contains(Name)) { continue; }
        Missing.Add(Cls->GetName());
    }

    if (Missing.Num() > 0)
    {
        Missing.Sort();
        const FString Joined = FString::Join(Missing, TEXT(", "));
        AddError(FString::Printf(
            TEXT("CRIR node-kind matrix is incomplete. %d uncovered concrete URigVMNode subclass(es): %s. ")
            TEXT("Resolution: add a `CRIR.RoundTrip.<X>` test in Tests/Assets/, or add the class to the ")
            TEXT("Uncovered map in this file with a documented reason."),
            Missing.Num(), *Joined));
        return false;
    }

    // Second half of the gate: every `Covered` claim must name a test that is
    // actually registered. Without this the matrix is self-derived — it lists
    // the node kinds it decided are covered and then checks that list against
    // itself, so deleting a round-trip test file leaves the gate green.
    const FAutomationTestFramework& Framework = FAutomationTestFramework::Get();
    for (const TPair<FName, FString>& Entry : Covered)
    {
        if (!Framework.ContainsTest(Entry.Value))
        {
            AddError(FString::Printf(
                TEXT("CRIR node-kind matrix claims '%s' is covered by automation test class '%s', ")
                TEXT("but no such test is registered. Either the test was deleted/renamed (restore it, ")
                TEXT("or update this entry) or the node kind belongs in the Uncovered map with a reason."),
                *Entry.Key.ToString(), *Entry.Value));
        }
    }
    return true;
}
