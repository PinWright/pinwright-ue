// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Misc/AutomationTest.h"
#include "Tests/TestUtils.h"
#include "BpirGraphTestHelpers.h"

#include "Decompiler/BpirDecompiler.h"
#include "Handlers/Blueprint/BlueprintHandlerUtils.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "GameFramework/Actor.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "K2Node_Event.h"
#include "K2Node_CallFunction.h"
#include "K2Node_Composite.h"
#include "K2Node_Knot.h"
#include "K2Node_Tunnel.h"
#include "EdGraphSchema_K2.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphPin.h"
#include "Kismet/KismetSystemLibrary.h"
#include "UObject/Package.h"

namespace
{
    struct FCompositeQualifiedOrphanFixture
    {
        UBlueprint* BP = nullptr;
        UEdGraph* EventGraph = nullptr;
        UEdGraph* BoundGraph = nullptr;
        UK2Node_CallFunction* ReachableOuter = nullptr;
        UK2Node_CallFunction* InnerReachable = nullptr;
        UK2Node_CallFunction* InnerOrphan = nullptr;
        UK2Node_CallFunction* StandalonePure = nullptr;
        UK2Node_Knot* DisconnectedKnot = nullptr;
        bool bChainWired = false;
    };

    UEdGraphNode* FindNodeByGuid(UEdGraph* Graph, const FGuid& NodeGuid)
    {
        if (!Graph)
        {
            return nullptr;
        }

        for (UEdGraphNode* Node : Graph->Nodes)
        {
            if (Node && Node->NodeGuid == NodeGuid)
            {
                return Node;
            }
        }

        return nullptr;
    }

    FCompositeQualifiedOrphanFixture BuildCompositeQualifiedOrphanFixture()
    {
        FCompositeQualifiedOrphanFixture Fixture;
        Fixture.BP = BpirGraphTestHelpers::CreateOrphanTestBlueprint();
        if (!Fixture.BP || Fixture.BP->UbergraphPages.Num() == 0)
        {
            return Fixture;
        }

        Fixture.EventGraph = Fixture.BP->UbergraphPages[0];
        UK2Node_Event* BeginPlay = BpirGraphTestHelpers::EnsureBeginPlayNode(Fixture.EventGraph);
        Fixture.ReachableOuter = BpirGraphTestHelpers::AddPrintStringNode(Fixture.EventGraph);

        UK2Node_Composite* Composite = NewObject<UK2Node_Composite>(Fixture.EventGraph);
        Composite->CreateNewGuid();
        Composite->PostPlacedNewNode();
        Fixture.EventGraph->AddNode(Composite, /*bFromUI=*/true, /*bSelectNewNode=*/false);
        Fixture.BoundGraph = Composite->BoundGraph;
        if (!Fixture.BoundGraph || !Composite->InputSinkNode || !Composite->OutputSourceNode)
        {
            return Fixture;
        }

        FBlueprintEditorUtils::RenameGraph(Fixture.BoundGraph, TEXT("QualifiedOrphanGraph"));

        FEdGraphPinType ExecPinType;
        ExecPinType.PinCategory = UEdGraphSchema_K2::PC_Exec;
        Composite->InputSinkNode->CreateUserDefinedPin(
            UEdGraphSchema_K2::PN_Execute,
            ExecPinType,
            EGPD_Output);
        Composite->OutputSourceNode->CreateUserDefinedPin(
            UEdGraphSchema_K2::PN_Then,
            ExecPinType,
            EGPD_Input);
        Composite->ReconstructNode();

        Fixture.InnerOrphan = BpirGraphTestHelpers::AddPrintStringNode(Fixture.BoundGraph);
        Fixture.InnerReachable = BpirGraphTestHelpers::AddPrintStringNode(Fixture.BoundGraph);

        // Keep one disconnected pure call to prove the explicit standalone-pure policy
        // excludes the node from both orphan sets while BPIR preserves it in an entry body.
        Fixture.StandalonePure = NewObject<UK2Node_CallFunction>(Fixture.EventGraph);
        Fixture.StandalonePure->CreateNewGuid();
        Fixture.StandalonePure->FunctionReference.SetExternalMember(
            GET_FUNCTION_NAME_CHECKED(UKismetSystemLibrary, IsValid),
            UKismetSystemLibrary::StaticClass());
        Fixture.StandalonePure->PostPlacedNewNode();
        Fixture.StandalonePure->AllocateDefaultPins();
        Fixture.EventGraph->AddNode(
            Fixture.StandalonePure, /*bFromUI=*/true, /*bSelectNewNode=*/false);
        Fixture.StandalonePure->ReconstructNode();

        // A disconnected reroute knot is a data-only orphan candidate. BPIR cannot
        // preserve it as a standalone expression, so both consumers must report it.
        Fixture.DisconnectedKnot = BpirGraphTestHelpers::AddNodeToGraph<UK2Node_Knot>(Fixture.EventGraph);
        Fixture.DisconnectedKnot->ReconstructNode();

        // Intentionally reuse the reachable outer GUID. The old BPIR pass flattened the
        // composite and keyed reachability by GUID only, so this orphan looked reachable.
        if (Fixture.InnerOrphan && Fixture.ReachableOuter)
        {
            Fixture.InnerOrphan->NodeGuid = Fixture.ReachableOuter->NodeGuid;
        }

        UEdGraphPin* BeginPlayThen = BeginPlay->FindPin(
            UEdGraphSchema_K2::PN_Then, EGPD_Output);
        UEdGraphPin* CompositeInExec = nullptr;
        UEdGraphPin* CompositeOutExec = nullptr;
        for (UEdGraphPin* Pin : Composite->Pins)
        {
            if (!Pin || Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec)
            {
                continue;
            }
            if (Pin->Direction == EGPD_Input && !CompositeInExec)
            {
                CompositeInExec = Pin;
            }
            else if (Pin->Direction == EGPD_Output && !CompositeOutExec)
            {
                CompositeOutExec = Pin;
            }
        }
        UEdGraphPin* OuterExec = Fixture.ReachableOuter->FindPin(
            UEdGraphSchema_K2::PN_Execute, EGPD_Input);

        UEdGraphPin* InnerEntryExec = Composite->InputSinkNode->FindPin(
            UEdGraphSchema_K2::PN_Execute, EGPD_Output);
        UEdGraphPin* InnerReachableExec = Fixture.InnerReachable->FindPin(
            UEdGraphSchema_K2::PN_Execute, EGPD_Input);
        UEdGraphPin* InnerReachableThen = Fixture.InnerReachable->FindPin(
            UEdGraphSchema_K2::PN_Then, EGPD_Output);
        UEdGraphPin* InnerExitExec = Composite->OutputSourceNode->FindPin(
            UEdGraphSchema_K2::PN_Then, EGPD_Input);

        auto ConnectExecPins = [](UEdGraphPin* OutputPin, UEdGraphPin* InputPin)
        {
            if (!OutputPin || !InputPin)
            {
                return false;
            }
            bool bConnected = false;
            if (const UEdGraphSchema* Schema = OutputPin->GetSchema())
            {
                bConnected = Schema->TryCreateConnection(OutputPin, InputPin);
            }
            else
            {
                OutputPin->MakeLinkTo(InputPin);
                bConnected = true;
            }
            return bConnected
                && OutputPin->LinkedTo.Contains(InputPin)
                && InputPin->LinkedTo.Contains(OutputPin);
        };

        // One valid chain: BeginPlay -> composite -> inner -> exit -> outer. In
        // particular, the composite input has exactly one upstream exec connection.
        const bool bBeginPlayToComposite = ConnectExecPins(BeginPlayThen, CompositeInExec);
        const bool bCompositeToInner = ConnectExecPins(InnerEntryExec, InnerReachableExec);
        const bool bInnerToExit = ConnectExecPins(InnerReachableThen, InnerExitExec);
        const bool bCompositeToOuter = ConnectExecPins(CompositeOutExec, OuterExec);
        Fixture.bChainWired = bBeginPlayToComposite
            && bCompositeToInner
            && bInnerToExit
            && bCompositeToOuter;
        return Fixture;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirDecompilerSkipsKnotAndEmptyConstructionScriptTest,
    "PinWright.bpir.decompiler.SkipsKnotAndEmptyConstructionScript",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBpirDecompilerSkipsKnotAndEmptyConstructionScriptTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = BpirGraphTestHelpers::CreateOrphanTestBlueprint();
    if (!BP) { AddError(TEXT("Failed to create test Blueprint")); return false; }

    UEdGraph* EventGraph = (BP->UbergraphPages.Num() > 0) ? BP->UbergraphPages[0] : nullptr;
    if (!EventGraph) { AddError(TEXT("No EventGraph")); return false; }

    // Build: BeginPlay -> Knot -> PrintString
    UK2Node_Event* BeginPlayNode = BpirGraphTestHelpers::EnsureBeginPlayNode(EventGraph);

    UK2Node_Knot* KnotNode = BpirGraphTestHelpers::AddNodeToGraph<UK2Node_Knot>(EventGraph);
    KnotNode->ReconstructNode();
    BpirGraphTestHelpers::WireExec(BeginPlayNode, KnotNode);

    UK2Node_CallFunction* PrintNode = BpirGraphTestHelpers::AddNodeToGraph<UK2Node_CallFunction>(EventGraph);
    PrintNode->FunctionReference.SetExternalMember(
        GET_FUNCTION_NAME_CHECKED(UKismetSystemLibrary, PrintString),
        UKismetSystemLibrary::StaticClass());
    PrintNode->ReconstructNode();
    BpirGraphTestHelpers::WireExec(KnotNode, PrintNode);

    // The auto-created BP carries an empty UserConstructionScript::FunctionEntry which
    // previously triggered the second false positive — no extra setup is required.

    FBpirDecompiler Decompiler(BP);
    FBpirDecompileResult Result = Decompiler.Decompile();

    TestTrue(TEXT("Decompile succeeded"), Result.bSuccess);

    const FString OrphanWarningPrefix = TEXT("Orphaned node not reachable from any entry point");
    for (const FBpirWarning& Warning : Result.Warnings)
    {
        TestFalse(
            FString::Printf(TEXT("No orphan warning emitted (got: %s)"), *Warning.Text),
            Warning.Text.Contains(OrphanWarningPrefix));
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirDecompilerOrphanWarningIncludesNodeIdentityTest,
    "PinWright.bpir.decompiler.OrphanWarningIncludesNodeIdentity",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBpirDecompilerOrphanWarningIncludesNodeIdentityTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = BpirGraphTestHelpers::CreateOrphanTestBlueprint();
    if (!BP) { AddError(TEXT("Failed to create test Blueprint")); return false; }

    UEdGraph* EventGraph = (BP->UbergraphPages.Num() > 0) ? BP->UbergraphPages[0] : nullptr;
    if (!EventGraph) { AddError(TEXT("No EventGraph")); return false; }

    // Ensure an entry point exists so the graph has something reachable
    BpirGraphTestHelpers::EnsureBeginPlayNode(EventGraph);

    // Add two same-titled orphan nodes (both PrintString, no exec wiring to entry)
    UK2Node_CallFunction* NodeA = BpirGraphTestHelpers::AddNodeToGraph<UK2Node_CallFunction>(EventGraph);
    NodeA->FunctionReference.SetExternalMember(
        GET_FUNCTION_NAME_CHECKED(UKismetSystemLibrary, PrintString),
        UKismetSystemLibrary::StaticClass());
    NodeA->ReconstructNode();
    NodeA->NodePosX = 100;
    NodeA->NodePosY = 200;

    UK2Node_CallFunction* NodeB = BpirGraphTestHelpers::AddNodeToGraph<UK2Node_CallFunction>(EventGraph);
    NodeB->FunctionReference.SetExternalMember(
        GET_FUNCTION_NAME_CHECKED(UKismetSystemLibrary, PrintString),
        UKismetSystemLibrary::StaticClass());
    NodeB->ReconstructNode();
    NodeB->NodePosX = 300;
    NodeB->NodePosY = 400;

    // Neither node is wired to BeginPlay — both are orphans with identical titles
    FBpirDecompiler Decompiler(BP);
    FBpirDecompileResult Result = Decompiler.Decompile();

    const FString OrphanPrefix = TEXT("Orphaned node not reachable from any entry point");
    TArray<FString> OrphanWarnings;
    for (const FBpirWarning& Warning : Result.Warnings)
    {
        if (Warning.Text.Contains(OrphanPrefix))
        {
            OrphanWarnings.Add(Warning.Text);
            // Orphan warnings are source-asset hygiene notes, not decompiler-internal failures.
            TestEqual(TEXT("Orphan warning tagged Warn severity"),
                static_cast<int32>(Warning.Severity), static_cast<int32>(EBpirWarningSeverity::Warn));
        }
    }

    TestEqual(TEXT("Exactly two orphan warnings emitted"), OrphanWarnings.Num(), 2);
    if (OrphanWarnings.Num() < 2) { return false; }

    // Warnings must be textually distinct despite identical titles
    TestNotEqual(TEXT("Two same-titled orphan warnings must be textually distinct"),
        OrphanWarnings[0], OrphanWarnings[1]);

    // Each warning must contain its node's GUID, class name, title, and coordinates
    auto CheckWarningIdentity = [&](const FString& Warning, UK2Node_CallFunction* Node, const TCHAR* Label)
    {
        const FString Guid = Node->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens);
        const FString CoordStr = FString::Printf(TEXT("@(%d,%d)"), Node->NodePosX, Node->NodePosY);

        TestTrue(FString::Printf(TEXT("%s: warning contains GUID"), Label),
            Warning.Contains(Guid));
        TestTrue(FString::Printf(TEXT("%s: warning contains class name K2Node_CallFunction"), Label),
            Warning.Contains(TEXT("K2Node_CallFunction")));
        TestTrue(FString::Printf(TEXT("%s: warning contains title 'Print String'"), Label),
            Warning.Contains(TEXT("Print String")));
        TestTrue(FString::Printf(TEXT("%s: warning contains coordinates %s"), Label, *CoordStr),
            Warning.Contains(CoordStr));
    };

    // Match each warning to its node by GUID substring
    for (UK2Node_CallFunction* Node : {NodeA, NodeB})
    {
        const FString Guid = Node->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens);
        const FString* MatchedWarning = OrphanWarnings.FindByPredicate(
            [&](const FString& W) { return W.Contains(Guid); });

        if (!MatchedWarning)
        {
            AddError(FString::Printf(TEXT("No orphan warning found for node with GUID %s"), *Guid));
            continue;
        }
        CheckWarningIdentity(*MatchedWarning, Node,
            Node == NodeA ? TEXT("NodeA") : TEXT("NodeB"));
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirFinderAndDecompilerOrphanModelContractTest,
    "PinWright.bpir.decompiler.FinderAndDecompilerOrphanModelContract",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBpirFinderAndDecompilerOrphanModelContractTest::RunTest(const FString& Parameters)
{
    const FCompositeQualifiedOrphanFixture Fixture = BuildCompositeQualifiedOrphanFixture();
    if (!Fixture.BP || !Fixture.EventGraph || !Fixture.BoundGraph
        || !Fixture.ReachableOuter || !Fixture.InnerReachable || !Fixture.InnerOrphan
        || !Fixture.StandalonePure || !Fixture.DisconnectedKnot)
    {
        AddError(TEXT("Failed to build composite qualified-orphan fixture"));
        return false;
    }

    TestTrue(TEXT("fixture uses one valid BeginPlay-to-outer composite chain"),
        Fixture.bChainWired);
    if (!Fixture.bChainWired)
    {
        return false;
    }

    // The duplicated GUID is deliberate: graph identity must distinguish the reachable
    // outer node from the orphan in the nested authored graph.
    TestEqual(TEXT("fixture intentionally duplicates outer and inner GUID"),
        Fixture.InnerOrphan->NodeGuid, Fixture.ReachableOuter->NodeGuid);

    TArray<UEdGraph*> GraphFamily;
    BlueprintHandlerUtils::CollectBlueprintOrphanGraphFamily(Fixture.EventGraph, GraphFamily);
    const BlueprintHandlerUtils::FBlueprintOrphanReachability OrphanModel =
        BlueprintHandlerUtils::BuildBlueprintOrphanReachability(
            GraphFamily, /*bIncludeDataOnly=*/true);
    TestTrue(TEXT("graph-qualified model keeps reachable outer node live"),
        OrphanModel.IsExecReachable(Fixture.ReachableOuter));
    TestTrue(TEXT("graph-qualified model keeps the inner chain node live"),
        OrphanModel.IsExecReachable(Fixture.InnerReachable));
    TestFalse(TEXT("graph-qualified model keeps nested duplicate GUID orphaned"),
        OrphanModel.IsExecReachable(Fixture.InnerOrphan));
    TestFalse(TEXT("standalone pure node is not data-reachable"),
        OrphanModel.IsDataReachable(Fixture.StandalonePure));
    TestTrue(TEXT("standalone pure policy is explicit in the shared model"),
        OrphanModel.IsStandalonePure(Fixture.StandalonePure));
    TestFalse(TEXT("disconnected knot is not structural in the shared model"),
        OrphanModel.IsStructuralNode(Fixture.DisconnectedKnot));
    TestFalse(TEXT("disconnected knot is not a standalone pure preservation node"),
        OrphanModel.IsStandalonePure(Fixture.DisconnectedKnot));
    TestTrue(TEXT("disconnected knot is a data-only orphan"),
        OrphanModel.IsOrphan(Fixture.DisconnectedKnot, /*bIncludeDataOnly=*/true));

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), Fixture.BP->GetPathName());
    // A named graph selects its authored family, including the bound composite graph.
    Payload->SetStringField(TEXT("graphName"), Fixture.EventGraph->GetName());
    Payload->SetBoolField(TEXT("includeDataOnly"), true);

    TSet<FString> FinderOrphanKeys;
    FTestResponseCapture Capture;
    const bool bFound = InvokeHandlerWithCapture(
        TEXT("blueprint.graph.find_orphaned_nodes"), Payload, Capture);
    TestTrue(TEXT("find_orphaned_nodes handler found"), bFound);
    TestTrue(TEXT("find_orphaned_nodes responded"), Capture.bWasCalled);
    TestTrue(TEXT("find_orphaned_nodes succeeded"), Capture.bSuccess);

    const FString InnerGuid = Fixture.InnerOrphan->NodeGuid.ToString();
    const FString InnerWarningGuid =
        Fixture.InnerOrphan->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens);
    const FString StandaloneGuid = Fixture.StandalonePure->NodeGuid.ToString();
    const FString StandaloneWarningGuid =
        Fixture.StandalonePure->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens);
    const FString KnotGuid = Fixture.DisconnectedKnot->NodeGuid.ToString();
    const FString KnotWarningGuid =
        Fixture.DisconnectedKnot->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens);
    const FString BoundGraphName = Fixture.BoundGraph->GetName();
    const TArray<TSharedPtr<FJsonValue>>* OrphanedNodes = nullptr;
    double OrphanedCount = -1.0;
    if (Capture.Result.IsValid())
    {
        TestTrue(TEXT("handler response carries orphanedCount"),
            Capture.Result->TryGetNumberField(TEXT("orphanedCount"), OrphanedCount));
        TestEqual(TEXT("finder reports the nested impure orphan and disconnected knot"),
            OrphanedCount, 2.0);
        TestTrue(TEXT("handler response carries orphanedNodes"),
            Capture.Result->TryGetArrayField(TEXT("orphanedNodes"), OrphanedNodes));
    }
    else
    {
        AddError(TEXT("No result JSON returned from find_orphaned_nodes"));
    }

    bool bFinderFoundInnerOrphan = false;
    bool bFinderFoundStandalonePure = false;
    bool bFinderFoundDisconnectedKnot = false;
    if (OrphanedNodes)
    {
        for (const TSharedPtr<FJsonValue>& Value : *OrphanedNodes)
        {
            const TSharedPtr<FJsonObject> Entry = Value.IsValid() ? Value->AsObject() : nullptr;
            if (!Entry.IsValid())
            {
                continue;
            }

            FString NodeId;
            bool bHasExecPins = false;
            if (!Entry->TryGetStringField(TEXT("nodeId"), NodeId)
                || !Entry->TryGetBoolField(TEXT("hasExecPins"), bHasExecPins))
            {
                continue;
            }

            FString GraphName;
            TestTrue(TEXT("finder orphan carries graphName"),
                Entry->TryGetStringField(TEXT("graphName"), GraphName));
            FinderOrphanKeys.Add(GraphName + TEXT("|") + NodeId);
            if (NodeId.Equals(InnerGuid, ESearchCase::IgnoreCase))
            {
                TestEqual(TEXT("finder reports the authored nested graph"), GraphName, BoundGraphName);
                TestTrue(TEXT("nested orphan reports exec pins"), bHasExecPins);
                bFinderFoundInnerOrphan = true;
            }
            else if (NodeId.Equals(StandaloneGuid, ESearchCase::IgnoreCase))
            {
                TestEqual(TEXT("finder reports standalone pure in the selected root graph"),
                    GraphName, Fixture.EventGraph->GetName());
                TestFalse(TEXT("standalone pure orphan reports no exec pins"), bHasExecPins);
                bFinderFoundStandalonePure = true;
            }
            else if (NodeId.Equals(KnotGuid, ESearchCase::IgnoreCase))
            {
                bFinderFoundDisconnectedKnot = true;
            }
        }
    }
    TestTrue(TEXT("finder includes the nested orphan GUID"), bFinderFoundInnerOrphan);
    TestFalse(TEXT("finder excludes the shared standalone pure GUID"), bFinderFoundStandalonePure);
    TestTrue(TEXT("finder includes the disconnected data-only knot GUID"), bFinderFoundDisconnectedKnot);

    FBpirDecompiler Decompiler(Fixture.BP);
    const FBpirDecompileResult Result =
        Decompiler.DecompileGraph(Fixture.EventGraph->GetName());
    TestTrue(TEXT("BPIR decompilation succeeds"), Result.bSuccess);

    const FString OrphanWarningPrefix = TEXT("Orphaned node not reachable from any entry point");
    TArray<const FBpirWarning*> OrphanWarnings;
    for (const FBpirWarning& Warning : Result.Warnings)
    {
        if (Warning.Text.Contains(OrphanWarningPrefix))
        {
            OrphanWarnings.Add(&Warning);
        }
    }

    // Counterfactuals: the old flattened WorkingGraph pass keyed reachability by GUID,
    // so the reachable outer PrintString suppressed this nested orphan's warning. BPIR
    // warning text deliberately uses hyphenated GUIDs, while finder JSON uses the default
    // compact format; matching either warning with the compact form is a false negative.
    TestEqual(TEXT("BPIR warns for the same nested orphan and disconnected knot"),
        OrphanWarnings.Num(), 2);
    TSet<FString> BpirOrphanKeys;
    bool bBpirFoundInnerOrphan = false;
    bool bBpirFoundStandalonePure = false;
    bool bBpirFoundDisconnectedKnot = false;
    for (const FBpirWarning* Warning : OrphanWarnings)
    {
        if (!Warning)
        {
            continue;
        }
        TestEqual(TEXT("BPIR orphan warning uses Warn severity"),
            static_cast<int32>(Warning->Severity), static_cast<int32>(EBpirWarningSeverity::Warn));
        if (Warning->Text.Contains(InnerWarningGuid))
        {
            bBpirFoundInnerOrphan = true;
            BpirOrphanKeys.Add(BoundGraphName + TEXT("|") + InnerGuid);
            TestTrue(TEXT("BPIR warning names the authored nested graph"),
                Warning->Text.Contains(BoundGraphName));
        }
        if (Warning->Text.Contains(StandaloneWarningGuid))
        {
            bBpirFoundStandalonePure = true;
            BpirOrphanKeys.Add(Fixture.EventGraph->GetName() + TEXT("|") + StandaloneGuid);
        }
        if (Warning->Text.Contains(KnotWarningGuid))
        {
            bBpirFoundDisconnectedKnot = true;
            BpirOrphanKeys.Add(Fixture.EventGraph->GetName() + TEXT("|") + KnotGuid);
        }
    }
    TestTrue(TEXT("BPIR warning includes the nested orphan GUID"), bBpirFoundInnerOrphan);
    TestFalse(TEXT("BPIR excludes the shared standalone pure GUID"), bBpirFoundStandalonePure);
    TestTrue(TEXT("BPIR warning includes the disconnected data-only knot GUID"),
        bBpirFoundDisconnectedKnot);
    TestEqual(TEXT("finder and BPIR produce the same orphan-set size"),
        BpirOrphanKeys.Num(), FinderOrphanKeys.Num());
    for (const FString& OrphanKey : FinderOrphanKeys)
    {
        TestTrue(
            FString::Printf(TEXT("BPIR orphan set contains finder key %s"), *OrphanKey),
            BpirOrphanKeys.Contains(OrphanKey));
    }
    TestTrue(TEXT("BPIR preserves the standalone pure call in the entry body"),
        Result.BpirText.Contains(TEXT("IsValid")));

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintGraphDeleteNamedNestedScopeTest,
    "PinWright.blueprint.graph.delete_orphaned_nodes.NamedGraphNestedScopeOptIn",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBlueprintGraphDeleteNamedNestedScopeTest::RunTest(const FString& Parameters)
{
    const FCompositeQualifiedOrphanFixture Fixture = BuildCompositeQualifiedOrphanFixture();
    if (!Fixture.BP || !Fixture.EventGraph || !Fixture.BoundGraph || !Fixture.InnerOrphan)
    {
        AddError(TEXT("Failed to build composite qualified-orphan fixture"));
        return false;
    }

    TestTrue(TEXT("fixture uses one valid BeginPlay-to-outer composite chain"),
        Fixture.bChainWired);
    if (!Fixture.bChainWired)
    {
        return false;
    }

    const FGuid InnerOrphanGuid = Fixture.InnerOrphan->NodeGuid;
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), Fixture.BP->GetPathName());
    Payload->SetStringField(TEXT("graphName"), Fixture.EventGraph->GetName());
    Payload->SetBoolField(TEXT("includeDataOnly"), true);

    FTestResponseCapture DefaultCapture;
    const bool bDefaultFound = InvokeHandlerWithCapture(
        TEXT("blueprint.graph.delete_orphaned_nodes"), Payload, DefaultCapture);
    TestTrue(TEXT("default named delete handler found"), bDefaultFound);
    TestTrue(TEXT("default named delete responded"), DefaultCapture.bWasCalled);
    TestTrue(TEXT("default named delete succeeded"), DefaultCapture.bSuccess);
    if (DefaultCapture.Result.IsValid())
    {
        double NestedGraphsSkipped = -1.0;
        double DeletedCount = -1.0;
        TestTrue(TEXT("default named delete reports nestedGraphsSkipped"),
            DefaultCapture.Result->TryGetNumberField(TEXT("nestedGraphsSkipped"), NestedGraphsSkipped));
        TestTrue(TEXT("default named delete skips the nested graph"), NestedGraphsSkipped >= 1.0);
        TestTrue(TEXT("default named delete reports deletedCount"),
            DefaultCapture.Result->TryGetNumberField(TEXT("deletedCount"), DeletedCount));
        TestEqual(TEXT("default named delete removes only the root-graph knot"), DeletedCount, 1.0);
    }
    else
    {
        AddError(TEXT("No result JSON returned from default named delete"));
    }
    TestTrue(TEXT("nested orphan survives default named delete"),
        FindNodeByGuid(Fixture.BoundGraph, InnerOrphanGuid) != nullptr);

    Payload->SetBoolField(TEXT("includeNestedGraphs"), true);
    FTestResponseCapture NestedCapture;
    const bool bNestedFound = InvokeHandlerWithCapture(
        TEXT("blueprint.graph.delete_orphaned_nodes"), Payload, NestedCapture);
    TestTrue(TEXT("opt-in named delete handler found"), bNestedFound);
    TestTrue(TEXT("opt-in named delete responded"), NestedCapture.bWasCalled);
    TestTrue(TEXT("opt-in named delete succeeded"), NestedCapture.bSuccess);
    if (NestedCapture.Result.IsValid())
    {
        double NestedGraphsSkipped = -1.0;
        double DeletedCount = -1.0;
        TestTrue(TEXT("opt-in named delete reports nestedGraphsSkipped"),
            NestedCapture.Result->TryGetNumberField(TEXT("nestedGraphsSkipped"), NestedGraphsSkipped));
        TestEqual(TEXT("opt-in named delete skips no nested graphs"), NestedGraphsSkipped, 0.0);
        TestTrue(TEXT("opt-in named delete reports deletedCount"),
            NestedCapture.Result->TryGetNumberField(TEXT("deletedCount"), DeletedCount));
        TestEqual(TEXT("opt-in named delete removes the nested orphan"), DeletedCount, 1.0);
    }
    else
    {
        AddError(TEXT("No result JSON returned from opt-in named delete"));
    }
    TestTrue(TEXT("opt-in named delete removes the nested orphan"),
        FindNodeByGuid(Fixture.BoundGraph, InnerOrphanGuid) == nullptr);

    return true;
}
