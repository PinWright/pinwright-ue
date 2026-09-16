// Copyright (c) 2026 Alexander Penkin. MIT License.

// TestBpirPhase0CascadesCreateDelegates.cpp
//
// Regression test for fix P0-4: BPIR Phase 0 (replace-mode wipe) must cascade-delete
// UK2Node_CreateDelegate nodes that live OUTSIDE the wiped subgraph but whose
// SelectedFunctionName points at a now-deleted entry (custom event, function, etc.).
//
// Root cause: CollectSubgraphNodes only walks exec-output pins and pure-upstream
// dependencies. It has no delegate-pin traversal, so CreateDelegate nodes that feed
// AddDelegate nodes inside the subgraph survive Phase 0 as orphans. On cold reload,
// FKismetCompilerContext::ReplaceConvertibleDelegates crashes on them because the
// referenced UFunction no longer exists.
//
// Fix: Phase 0 now collects WipedFunctionNames during the wipe loop, then calls
// BlueprintHandlerUtils::CascadeRemoveStaleCreateDelegates(TargetBlueprint,
// WipedFunctionNames) after the main deletion pass.
//
// Test flow:
//   1. Create an on-disk Blueprint (AActor parent).
//   2. Plant a UK2Node_CustomEvent named "TargetEvent" in EventGraph.
//   3. Plant a UK2Node_CreateDelegate in the same EventGraph with
//      SelectedFunctionName = "TargetEvent". Capture CreateDelegateGuid.
//   4. Invoke blueprint.compile_bpir with replace mode targeting "TargetEvent".
//   5. Assert compile_bpir succeeded (or at minimum did not return INTEGRITY_FAILURE).
//   6. Assert the CreateDelegate node is gone: GetNodeByGUID returns null.
//
// Counterfactual: if the CascadeRemoveStaleCreateDelegates call at Phase 0 end is
// reverted, Phase 0 only deletes the old K2Node_CustomEvent (and exec-reachable
// subgraph). The orphan K2Node_CreateDelegate with SelectedFunctionName="TargetEvent"
// survives. GetNodeByGUID returns the node and the TestNull assertion below fails.

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
#if defined(__has_include) && (__has_include("BlueprintGraph/K2Node_CreateDelegate.h") || __has_include("BlueprintGraph/Classes/K2Node_CreateDelegate.h") || __has_include("K2Node_CreateDelegate.h"))
#if __has_include("BlueprintGraph/K2Node_CreateDelegate.h")
#include "BlueprintGraph/K2Node_CreateDelegate.h"
#elif __has_include("BlueprintGraph/Classes/K2Node_CreateDelegate.h")
#include "BlueprintGraph/Classes/K2Node_CreateDelegate.h"
#else
#include "K2Node_CreateDelegate.h"
#endif

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirPhase0CascadesCreateDelegatesTest,
    "PinWright.blueprint.compile_bpir.Phase0CascadesCreateDelegates",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBpirPhase0CascadesCreateDelegatesTest::RunTest(const FString& Parameters)
{
    // On-disk path required: LoadBlueprintAsset (called by the handler) cannot see
    // transient-package assets.
    const FString AssetPath = FString::Printf(
        TEXT("/Game/__PW_GatewayTests/BpirPhase0Cascade_%s"),
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

    // Plant a custom event that BPIR Phase 0 will wipe in replace mode.
    UK2Node_CustomEvent* CustomEventNode = NewObject<UK2Node_CustomEvent>(EventGraph);
    CustomEventNode->CustomFunctionName = FName(TEXT("TargetEvent"));
    CustomEventNode->CreateNewGuid();
    EventGraph->AddNode(CustomEventNode, /*bFromUI=*/false, /*bSelectNewNode=*/false);
    // AllocateDefaultPins is required: compile_bpir triggers a skeleton recompile,
    // and UK2Node_Event::UpdateDelegatePin + UK2Node_CreateDelegate::GetDelegateSignature
    // both do FindPinChecked/check(Pin) on pins that only exist after pin allocation.
    CustomEventNode->AllocateDefaultPins();

    // Plant a CreateDelegate node OUTSIDE the custom event's exec subgraph.
    // Its SelectedFunctionName references the same event — this is the node that
    // CollectSubgraphNodes misses, and CascadeRemoveStaleCreateDelegates must remove.
    UK2Node_CreateDelegate* CreateDelegateNode = NewObject<UK2Node_CreateDelegate>(EventGraph);
    CreateDelegateNode->SelectedFunctionName = FName(TEXT("TargetEvent"));
    CreateDelegateNode->CreateNewGuid();
    EventGraph->AddNode(CreateDelegateNode, /*bFromUI=*/false, /*bSelectNewNode=*/false);
    CreateDelegateNode->AllocateDefaultPins();

    const FGuid CreateDelegateGuid = CreateDelegateNode->NodeGuid;

    if (!TestTrue(TEXT("CreateDelegate present before compile"),
            EventGraph->Nodes.Contains(CreateDelegateNode)))
    {
        PwTestAssetTeardown::DiscardCreatedAssetByObjectPath(ToObjectPath(AssetPath));
        return true;
    }

    // Invoke blueprint.compile_bpir in replace mode. The BPIR snippet redefines
    // TargetEvent with an empty body — enough to trigger Phase 0 deletion of the
    // existing custom event and (after fix P0-4) the orphan CreateDelegate.
    // Parameter names confirmed from BpirCompilerHandler.cpp: "assetPath", "code", "mode".
    const FString BpirCode = TEXT(
        "entry custom_event TargetEvent() {\n"
        "}\n"
    );

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), AssetPath);
    Payload->SetStringField(TEXT("code"), BpirCode);
    Payload->SetStringField(TEXT("mode"), TEXT("replace"));

    FTestResponseCapture Capture;
    const bool bFound = InvokeHandlerWithCapture(TEXT("blueprint.compile_bpir"), Payload, Capture);
    TestTrue(TEXT("Handler found"), bFound);
    TestTrue(TEXT("Response was sent"), Capture.bWasCalled);

    // The compile must not have returned INTEGRITY_FAILURE — that would indicate the
    // orphan CreateDelegate triggered the integrity gate rather than being cleaned up
    // by Phase 0c. Any other result (success or COMPILE_FAILED on empty body) is fine.
    if (!Capture.bSuccess)
    {
        TestFalse(TEXT("Error code is not INTEGRITY_FAILURE"),
            Capture.ErrorCode.Equals(TEXT("INTEGRITY_FAILURE"), ESearchCase::IgnoreCase));
    }

    // The CreateDelegate node must be gone — Phase 0c cascade-removed it.
    UEdGraphNode* FoundByGuid = FBlueprintEditorUtils::GetNodeByGUID(BP, CreateDelegateGuid);
    TestNull(TEXT("Orphan CreateDelegate was cascade-removed by Phase 0c (not found by GUID)"), FoundByGuid);

    PwTestAssetTeardown::DiscardCreatedAssetByObjectPath(ToObjectPath(AssetPath));
    return true;
}

#endif
