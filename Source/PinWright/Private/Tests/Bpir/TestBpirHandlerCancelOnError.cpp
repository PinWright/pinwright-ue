// Copyright (c) 2026 Alexander Penkin. MIT License.

// TestBpirHandlerCancelOnError.cpp
//
// Regression test for B-bp-saved-state-corruption-mcp-edits (P0-3 + P1-6).
//
// P0-3: Every BPIR compile handler wraps its work in FScopedTransaction. On
//       failure, the handler must finalize the transaction and undo it before
//       SendError so partial mutations do not survive the structured error path.
//
// P1-6: In Replace mode, Phase 0 inside FBpirCompiler::Compile() deletes
//       existing entry nodes before re-creation (via
//       FBlueprintEditorUtils::RemoveNode, which calls Modify() on graph + node
//       before destruction). If Phase 1/2 then fail, those deletions must be
//       rolled back through the handler-level transaction — there is no
//       separate in-Compile rollback for Phase 0-pre deletions.
//
// Test flow:
//   1. Create an on-disk Blueprint (AActor parent).
//   2. Add a UK2Node_CustomEvent named "MyExistingEvent"; record its NodeGuid.
//   3. Submit blueprint.compile_bpir with code that:
//        - targets the same custom event (Replace mode deletes it in Phase 0)
//        - has a body that fails during emit: call to a non-existent function
//   4. Assert response error code == "COMPILE_FAILED".
//   5. Assert the original CustomEvent node is STILL present (NodeGuid still
//      resolves via FBlueprintEditorUtils::GetNodeByGUID).
//
// Counterfactual: without handler-level transaction rollback, the Phase 0-pre
// deletion stays applied; the original CustomEvent is gone after the handler
// returns; GetNodeByGUID returns null and assertion (5) fails.

#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Tests/TestAssetTeardown.h"
#include "Tests/TestUtils.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "GameFramework/Actor.h"
#include "EdGraph/EdGraph.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"
#include "K2Node_CustomEvent.h"


IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirHandlerCancelOnErrorTest,
    "PinWright.blueprint.compile_bpir.HandlerCancelOnError",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBpirHandlerCancelOnErrorTest::RunTest(const FString& Parameters)
{
    // On-disk path required: LoadBlueprintAsset (called by the handler) cannot
    // see transient-package assets.
    const FString AssetPath = FString::Printf(
        TEXT("/Game/__PW_GatewayTests/BpirHandlerCancelOnError_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));

    UPackage* Pkg = CreatePackage(*AssetPath);
    if (!TestNotNull(TEXT("Package created"), Pkg))
        return true;

    UBlueprint* BP = FKismetEditorUtilities::CreateBlueprint(
        AActor::StaticClass(), Pkg,
        FName(*FPackageName::GetLongPackageAssetName(AssetPath)),
        BPTYPE_Normal, UBlueprint::StaticClass(),
        UBlueprintGeneratedClass::StaticClass());

    if (!TestNotNull(TEXT("Blueprint created"), BP))
    {
        PwTestAssetTeardown::DiscardCreatedAssetByObjectPath(ToObjectPath(AssetPath));
        return true;
    }

    UEdGraph* EventGraph = FBlueprintEditorUtils::FindEventGraph(BP);
    if (!TestNotNull(TEXT("EventGraph exists"), EventGraph))
    {
        PwTestAssetTeardown::DiscardCreatedAssetByObjectPath(ToObjectPath(AssetPath));
        return true;
    }

    // Add an existing CustomEvent that Phase 0 (Replace mode) will delete.
    // RF_Transactional is required for editor undo to restore the node after
    // Phase 0's RemoveNode snapshot. Production paths (UEdGraph::CreateNode
    // / FEdGraphSchemaAction_NewNode) set this flag automatically; raw NewObject does
    // not, so the BPIR rollback contract only holds when callers
    // create nodes through the graph API. Emulate that here by setting the flag
    // explicitly on both the node and its graph.
    //
    // Initialization order mirrors UK2Node_CustomEvent::CreateFromFunction:
    // AddNode -> CreateNewGuid -> PostPlacedNewNode -> AllocateDefaultPins.
    // AllocateDefaultPins is mandatory: UK2Node_Event requires its OutputDelegate
    // pin to exist for the post-undo skeleton recompile path
    // (UpdateDelegatesInBlueprint -> UpdateDelegatePin -> FindPinChecked).
    // Without it, rolling the transaction back asserts inside the engine.
    UK2Node_CustomEvent* ExistingCE = NewObject<UK2Node_CustomEvent>(
        EventGraph, NAME_None, RF_Transactional);
    ExistingCE->CustomFunctionName = FName(TEXT("MyExistingEvent"));
    EventGraph->AddNode(ExistingCE, /*bFromUI=*/false, /*bSelectNewNode=*/false);
    ExistingCE->CreateNewGuid();
    ExistingCE->PostPlacedNewNode();
    ExistingCE->AllocateDefaultPins();

    const FGuid OriginalGuid = ExistingCE->NodeGuid;

    if (!TestTrue(TEXT("CustomEvent present before compile"), EventGraph->Nodes.Contains(ExistingCE)))
    {
        PwTestAssetTeardown::DiscardCreatedAssetByObjectPath(ToObjectPath(AssetPath));
        return true;
    }

    // BPIR body: target MyExistingEvent (triggers Phase 0 deletion) with a
    // body that calls a non-existent function, causing Phase 1/2 to fail with
    // COMPILE_FAILED.
    const FString BpirCode = TEXT(
        "entry custom_event MyExistingEvent() {\n"
        "    call ThisFunctionDoesNotExistAnywhere()\n"
        "}\n"
    );

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), AssetPath);
    Payload->SetStringField(TEXT("code"), BpirCode);
    // Explicit "replace" / "append" mode (same semantics) so Phase 0 pre-deletes
    // the existing CustomEvent before Phase 1 attempts re-creation.
    Payload->SetStringField(TEXT("mode"), TEXT("replace"));

    FTestResponseCapture Capture;
    const bool bFound = InvokeHandlerWithCapture(TEXT("blueprint.compile_bpir"), Payload, Capture);
    TestTrue(TEXT("Handler found"), bFound);

    // The compile must fail due to the unresolvable call target.
    TestTrue(TEXT("Response was sent"), Capture.bWasCalled);
    TestFalse(TEXT("Response is an error (not success)"), Capture.bSuccess);
    TestEqual(TEXT("Error code is COMPILE_FAILED"), Capture.ErrorCode, FString(TEXT("COMPILE_FAILED")));

    // The original CustomEvent must still be present — editor undo restores it
    // via the Modify() snapshot captured by RemoveNode during Phase 0.
    UEdGraphNode* RestoredNode = FBlueprintEditorUtils::GetNodeByGUID(BP, OriginalGuid);
    TestNotNull(TEXT("Original CustomEvent node restored after transaction rollback"), RestoredNode);

    if (RestoredNode)
    {
        UK2Node_CustomEvent* RestoredCE = Cast<UK2Node_CustomEvent>(RestoredNode);
        TestNotNull(TEXT("Restored node is a UK2Node_CustomEvent"), RestoredCE);
        if (RestoredCE)
        {
            TestEqual(TEXT("Restored CustomEvent has original name"),
                RestoredCE->CustomFunctionName.ToString(),
                FString(TEXT("MyExistingEvent")));
        }
    }

    PwTestAssetTeardown::DiscardCreatedAssetByObjectPath(ToObjectPath(AssetPath));
    return true;
}
