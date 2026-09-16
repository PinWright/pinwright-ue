// Copyright (c) 2026 Alexander Penkin. MIT License.

// TestScopedTransactionApplyCancel.cpp
//
// Smoke test for the GUndo->Apply() + FScopedTransaction::Cancel() rollback
// pattern that BpirCompilerHandler will use to revert mutations on error paths.
//
// UE 5.6 contract (EditorTransaction.cpp:853): Apply may be called before
// Finalize to revert an object back to its prior state in the case that a
// transaction is canceled. Cancel() alone only discards the record without
// applying it -- so any Modify()-snapshotted state stays in its post-mutation
// form unless Apply() is invoked first.
//
// The riskiest unknown surfaced during the rollback investigation: Apply()
// asserts Inc==1||Inc==-1 (EditorTransaction.cpp:816). If a freshly-begun
// transaction has Inc==0, the assert trips. This test verifies the canonical
// "Apply then Cancel" sequence does not assert and does revert state.
//
// Counterfactual: if Apply asserts on default Inc, this test will fire the
// engine ensure/check before reaching the post-Apply assertions, signaling
// that the helper needs GUndo->SetInc(-1) (or a different rollback API).

#include "Misc/AutomationTest.h"

#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "Editor.h"
#include "Editor/Transactor.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "GameFramework/Actor.h"
#include "K2Node_CustomEvent.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/PackageName.h"
#include "ScopedTransaction.h"
#include "UObject/Package.h"
#include "Tests/TestSkipReporting.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FScopedTransactionApplyCancelTest,
    "PinWright.utils.scoped_transaction.ApplyThenCancelReverts",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FScopedTransactionApplyCancelTest::RunTest(const FString& Parameters)
{
    if (!GEditor || !GEditor->Trans)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-transactor"),
            TEXT("No editor transactor available; skipping (test must run under EditorContext)."));
        return true;
    }

    // Build a transient Blueprint with a UEdGraph -- mirrors the BPIR scenario
    // (handler removes a UK2Node_CustomEvent then needs to revert the removal).
    UPackage* TransientPkg = GetTransientPackage();
    const FName BPName = MakeUniqueObjectName(TransientPkg, UBlueprint::StaticClass(), TEXT("ScopedTxnTestBP"));
    UBlueprint* BP = FKismetEditorUtilities::CreateBlueprint(
        AActor::StaticClass(), TransientPkg, BPName,
        BPTYPE_Normal, UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());
    if (!TestNotNull(TEXT("CreateBlueprint succeeded"), BP)) return false;

    UEdGraph* EventGraph = FBlueprintEditorUtils::FindEventGraph(BP);
    if (!TestNotNull(TEXT("EventGraph exists"), EventGraph)) return false;

    // Pre-existing CustomEvent node we will delete inside the transaction and
    // expect to be restored after Apply().
    UK2Node_CustomEvent* OriginalNode = NewObject<UK2Node_CustomEvent>(
        EventGraph, NAME_None, RF_Transactional);
    OriginalNode->CustomFunctionName = TEXT("BeforeApply");
    OriginalNode->CreateNewGuid();
    OriginalNode->AllocateDefaultPins();
    EventGraph->AddNode(OriginalNode, /*bUserAction*/ false, /*bSelectNewNode*/ false);

    const FGuid OriginalGuid = OriginalNode->NodeGuid;
    const int32 NodeCountBefore = EventGraph->Nodes.Num();
    if (!TestTrue(TEXT("Pre-transaction node is in graph"), EventGraph->Nodes.Contains(OriginalNode))) return false;

    // ---- Open transaction, mutate, Apply, Cancel ----
    {
        FScopedTransaction Transaction(NSLOCTEXT("Test", "ApplyCancelSmoke", "ScopedTxn smoke"));

        if (!TestNotNull(TEXT("GUndo is set inside FScopedTransaction"), GUndo))
        {
            Transaction.Cancel();
            return false;
        }

        // Snapshot + delete the node. RemoveNode calls Graph->Modify() and
        // Node->Modify() before destruction -- the canonical pattern the BPIR
        // handler exercises.
        FBlueprintEditorUtils::RemoveNode(BP, OriginalNode, /*bDontRecompile*/ true);

        // Sanity: deletion landed.
        TestEqual(TEXT("Node count dropped during transaction"),
            EventGraph->Nodes.Num(), NodeCountBefore - 1);

        // The pivot: revert the snapshotted mutations, then discard the record.
        // If GUndo's Inc default is incompatible with Apply()'s assert, this
        // line will trip an engine check and the test will fail loudly here.
        GUndo->Apply();

        Transaction.Cancel();
    }

    // ---- Verify revert ----
    TestEqual(TEXT("Node count restored after Apply+Cancel"),
        EventGraph->Nodes.Num(), NodeCountBefore);

    UEdGraphNode* RestoredNode = FBlueprintEditorUtils::GetNodeByGUID(BP, OriginalGuid);
    TestNotNull(TEXT("Original CustomEvent (by GUID) restored after Apply+Cancel"), RestoredNode);
    if (RestoredNode)
    {
        UK2Node_CustomEvent* RestoredCE = Cast<UK2Node_CustomEvent>(RestoredNode);
        TestNotNull(TEXT("Restored node is still a UK2Node_CustomEvent"), RestoredCE);
        if (RestoredCE)
        {
            TestEqual(TEXT("Restored CustomEvent retained CustomFunctionName"),
                RestoredCE->CustomFunctionName, FName(TEXT("BeforeApply")));
        }
    }

    return true;
}
