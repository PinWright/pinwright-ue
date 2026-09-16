// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression coverage for B-insert-bpir-rollback-leaves-anchor-exec-severed.
// A valid insert into a wired exec pin must restore that original edge when a
// whole-Blueprint compile fails because BPIR emitted an invalid node.

#include "Misc/AutomationTest.h"
#include "Tests/TestUtils.h"
#include "BpirGraphTestHelpers.h"
#include "Handlers/Blueprint/BlueprintHandlerUtils.h"
#include "Utils/AssetUtils.h"

#include "Dom/JsonObject.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "GameFramework/Actor.h"
#include "K2Node_CallFunction.h"
#include "K2Node_CustomEvent.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"
#include "UObject/Package.h"

namespace BpirInsertRollbackAnchorExecTest
{
    static UEdGraphPin* FindExecOutput(UEdGraphNode* Node)
    {
        return Node
            ? Node->FindPin(UEdGraphSchema_K2::PN_Then, EGPD_Output)
            : nullptr;
    }

    static UEdGraphPin* FindExecInput(UEdGraphNode* Node)
    {
        return Node
            ? Node->FindPin(UEdGraphSchema_K2::PN_Execute, EGPD_Input)
            : nullptr;
    }

    static int32 CountLinksTo(const UEdGraphPin* Source, const UEdGraphPin* Target)
    {
        int32 Count = 0;
        if (!Source || !Target)
        {
            return Count;
        }

        for (const UEdGraphPin* LinkedPin : Source->LinkedTo)
        {
            if (LinkedPin == Target)
            {
                ++Count;
            }
        }
        return Count;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirInsertRollbackAnchorExecTest,
    "PinWright.blueprint.insert_bpir_at_node.RollbackAnchorExec",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBpirInsertRollbackAnchorExecTest::RunTest(const FString& Parameters)
{
    using namespace BpirInsertRollbackAnchorExecTest;

    const FString AssetPath = FString::Printf(
        TEXT("/Game/PinWrightTests/BpirInsertRollbackAnchorExec_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));

    UPackage* Package = CreatePackage(*AssetPath);
    if (!TestNotNull(TEXT("Package created"), Package))
    {
        return true;
    }

    UBlueprint* BP = FKismetEditorUtilities::CreateBlueprint(
        AActor::StaticClass(),
        Package,
        FName(*FPackageName::GetLongPackageAssetName(AssetPath)),
        BPTYPE_Normal,
        UBlueprint::StaticClass(),
        UBlueprintGeneratedClass::StaticClass());

    if (!TestNotNull(TEXT("Blueprint created"), BP))
    {
        CleanupTestAsset(AssetPath);
        return true;
    }

    ON_SCOPE_EXIT
    {
        if (UPackage* BlueprintPackage = BP->GetOutermost())
        {
            BlueprintPackage->SetDirtyFlag(false);
        }
        CleanupTestAsset(AssetPath);
    };

    BP->SetFlags(RF_Transactional);

    UEdGraph* EventGraph = FBlueprintEditorUtils::FindEventGraph(BP);
    if (!TestNotNull(TEXT("Event graph exists"), EventGraph))
    {
        return true;
    }
    EventGraph->SetFlags(RF_Transactional);

    UK2Node_CustomEvent* Anchor = NewObject<UK2Node_CustomEvent>(
        EventGraph, NAME_None, RF_Transactional);
    Anchor->CustomFunctionName = FName(TEXT("InsertRollbackAnchor"));
    Anchor->CreateNewGuid();
    Anchor->PostPlacedNewNode();
    Anchor->AllocateDefaultPins();
    EventGraph->AddNode(Anchor, /*bFromUI=*/false, /*bSelectNewNode=*/false);

    UK2Node_CallFunction* DownstreamNode = BpirGraphTestHelpers::AddPrintStringNode(
        EventGraph, /*bTransactional=*/true);
    TestNotNull(TEXT("Downstream PrintString node created"), DownstreamNode);
    if (!DownstreamNode)
    {
        return true;
    }

    UEdGraphPin* AnchorThen = FindExecOutput(Anchor);
    UEdGraphPin* DownstreamExec = FindExecInput(DownstreamNode);
    TestNotNull(TEXT("Anchor exec output exists"), AnchorThen);
    TestNotNull(TEXT("Downstream exec input exists"), DownstreamExec);
    if (!AnchorThen || !DownstreamExec)
    {
        return true;
    }

    const UEdGraphSchema* Schema = AnchorThen->GetSchema();
    const bool bConnected = Schema
        ? Schema->TryCreateConnection(AnchorThen, DownstreamExec)
        : (AnchorThen->MakeLinkTo(DownstreamExec), true);
    TestTrue(TEXT("Original anchor exec link created"), bConnected);
    TestEqual(TEXT("Original anchor output has one link"), AnchorThen->LinkedTo.Num(), 1);
    TestEqual(TEXT("Original downstream input has one link"), DownstreamExec->LinkedTo.Num(), 1);

    const FGuid AnchorGuid = Anchor->NodeGuid;
    const FGuid DownstreamGuid = DownstreamNode->NodeGuid;

    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);
    const BlueprintHandlerUtils::FBlueprintCompileDiagnostics BaselineDiagnostics =
        BlueprintHandlerUtils::CompileBlueprintWithDiagnostics(BP);
    TestTrue(TEXT("Baseline Blueprint compiles successfully"), BaselineDiagnostics.bCompiled);
    TestEqual(TEXT("Baseline Blueprint compile has no errors"),
        BaselineDiagnostics.Errors.Num(), 0);
    if (!BaselineDiagnostics.bCompiled)
    {
        return true;
    }

    // Full compilation can reconstruct pins, so establish the durable baseline
    // from GUID-resolved nodes rather than retaining pre-compile pin pointers.
    UEdGraphNode* BaselineAnchor = FBlueprintEditorUtils::GetNodeByGUID(BP, AnchorGuid);
    UEdGraphNode* BaselineDownstream = FBlueprintEditorUtils::GetNodeByGUID(BP, DownstreamGuid);
    TestNotNull(TEXT("Baseline anchor resolves after compile"), BaselineAnchor);
    TestNotNull(TEXT("Baseline downstream resolves after compile"), BaselineDownstream);
    if (!BaselineAnchor || !BaselineDownstream)
    {
        return true;
    }

    UEdGraphPin* BaselineAnchorThen = FindExecOutput(BaselineAnchor);
    UEdGraphPin* BaselineDownstreamExec = FindExecInput(BaselineDownstream);
    TestNotNull(TEXT("Baseline anchor exec output exists"), BaselineAnchorThen);
    TestNotNull(TEXT("Baseline downstream exec input exists"), BaselineDownstreamExec);
    if (!BaselineAnchorThen || !BaselineDownstreamExec)
    {
        return true;
    }

    TestEqual(TEXT("Baseline anchor links to downstream exactly once"),
        CountLinksTo(BaselineAnchorThen, BaselineDownstreamExec), 1);
    TestEqual(TEXT("Baseline downstream links back to anchor exactly once"),
        CountLinksTo(BaselineDownstreamExec, BaselineAnchorThen), 1);

    UEdGraph* BaselineGraph = BaselineAnchor->GetGraph();
    if (!TestNotNull(TEXT("Baseline graph resolves"), BaselineGraph))
    {
        return true;
    }
    const int32 BaselineNodeCount = BaselineGraph->Nodes.Num();

    // Save a real, valid baseline asset so the handler exercises normal content
    // loading. The generic call below is emitted successfully, then fails only
    // during the handler's final whole-Blueprint compile.
    BP->MarkPackageDirty();
    const bool bBaselineSaved = SaveAssetToDiskReportingPresence(BP, /*bForce=*/true);
    TestTrue(TEXT("Valid baseline Blueprint saved to disk"), bBaselineSaved);
    if (!bBaselineSaved)
    {
        return true;
    }

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), AssetPath);
    Payload->SetStringField(TEXT("nodeId"), AnchorGuid.ToString());
    Payload->SetStringField(TEXT("execPin"), TEXT("then"));
    Payload->SetStringField(TEXT("code"),
        TEXT("call K2Node_CallFunction()\n"));

    FTestResponseCapture Capture;
    TestTrue(TEXT("insert_bpir_at_node handler found"),
        InvokeHandlerWithCapture(TEXT("blueprint.insert_bpir_at_node"), Payload, Capture));
    TestTrue(TEXT("Response was sent"), Capture.bWasCalled);
    TestFalse(TEXT("Response is a structured error"), Capture.bSuccess);
    TestEqual(TEXT("Error code is BLUEPRINT_COMPILE_FAILED"),
        Capture.ErrorCode,
        FString(TEXT("BLUEPRINT_COMPILE_FAILED")));

    TArray<FGuid> InsertedNodeGuids;
    if (TestNotNull(TEXT("Compile failure retains its handler payload"), Capture.Result.Get()))
    {
        const TArray<TSharedPtr<FJsonValue>>* CreatedNodes = nullptr;
        TestTrue(TEXT("Compile failure payload contains createdNodes"),
            Capture.Result->TryGetArrayField(TEXT("createdNodes"), CreatedNodes));
        if (CreatedNodes)
        {
            TestTrue(TEXT("BPIR parsed and inserted at least one node before compile failed"),
                CreatedNodes->Num() > 0);
            for (const TSharedPtr<FJsonValue>& CreatedNode : *CreatedNodes)
            {
                FGuid CreatedGuid;
                const bool bGuidParsed = CreatedNode.IsValid()
                    && FGuid::Parse(CreatedNode->AsString(), CreatedGuid);
                TestTrue(TEXT("Created node id parses"), bGuidParsed);
                if (bGuidParsed)
                {
                    InsertedNodeGuids.Add(CreatedGuid);
                }
            }
        }
    }

    UEdGraphNode* RestoredAnchor = FBlueprintEditorUtils::GetNodeByGUID(BP, AnchorGuid);
    UEdGraphNode* RestoredDownstream = FBlueprintEditorUtils::GetNodeByGUID(BP, DownstreamGuid);
    TestNotNull(TEXT("Anchor still resolves after rollback"), RestoredAnchor);
    TestNotNull(TEXT("Downstream node still resolves after rollback"), RestoredDownstream);
    if (!RestoredAnchor || !RestoredDownstream)
    {
        return true;
    }

    UEdGraphPin* RestoredAnchorThen = FindExecOutput(RestoredAnchor);
    UEdGraphPin* RestoredDownstreamExec = FindExecInput(RestoredDownstream);
    TestNotNull(TEXT("Restored anchor exec output exists"), RestoredAnchorThen);
    TestNotNull(TEXT("Restored downstream exec input exists"), RestoredDownstreamExec);
    if (!RestoredAnchorThen || !RestoredDownstreamExec)
    {
        return true;
    }

    TestEqual(TEXT("Restored anchor output has exactly one link"),
        RestoredAnchorThen->LinkedTo.Num(), 1);
    TestEqual(TEXT("Restored downstream input has exactly one link"),
        RestoredDownstreamExec->LinkedTo.Num(), 1);
    TestEqual(TEXT("Restored anchor links to downstream exactly once"),
        CountLinksTo(RestoredAnchorThen, RestoredDownstreamExec), 1);
    TestEqual(TEXT("Restored downstream links back to anchor exactly once"),
        CountLinksTo(RestoredDownstreamExec, RestoredAnchorThen), 1);

    UEdGraph* RestoredGraph = RestoredAnchor->GetGraph();
    if (TestNotNull(TEXT("Restored graph resolves"), RestoredGraph))
    {
        TestEqual(TEXT("Rollback restores the baseline node count"),
            RestoredGraph->Nodes.Num(), BaselineNodeCount);
    }
    for (const FGuid& InsertedNodeGuid : InsertedNodeGuids)
    {
        TestNull(TEXT("Rollback removes every node inserted before compile failure"),
            FBlueprintEditorUtils::GetNodeByGUID(BP, InsertedNodeGuid));
    }

    return true;
}
