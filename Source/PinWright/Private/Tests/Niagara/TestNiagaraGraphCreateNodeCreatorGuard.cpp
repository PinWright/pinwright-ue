// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression guard for B-niagara-create-node-unfinalized-graph-node-creator-fatal /
// B-niagara-create-node-early-return-before-finalize-crash (one defect, filed twice).
//
// niagara.graph.create_node opened a stack FGraphNodeCreator<UEdGraphNode>, created the node, and
// only called Finalize() on the success path. Every rejection in between returned straight out, so
// ~FGraphNodeCreator's unconditional checkf(bPlaced) (EdGraph.h) fired an appError and terminated
// the editor — after the typed error response had already been delivered, which is why it read to
// the caller as a clean rejection followed by an unrelated transport drop.
//
// THIS TEST ASSERTS THE GUARD, NOT THE CRASH. An automation test cannot observe that appError: it
// would take its own suite host down with it. What it can assert is the state the guard produces —
// a well-formed typed error, and a graph that the rejection left exactly as it found it. Both
// rejection shapes named in the tickets are covered, because they fail differently:
//
//   Case 1  unsupported nodeClass  — must be refused BEFORE anything is constructed.
//   Case 2  supported class, rejected payload (INVALID_OP) — the node is necessarily built first
//           (Finalize() allocates pins from the applied fields), so this one proves the creator is
//           finalized on the error path and the node is then removed.
//   Case 3  accepted request — the handler is still reachable and the success path still finalizes.
//
// Case 2 is the reason case 1 alone is not enough: a fix that only hoists the class check would
// still abort here. Both cases assert an unchanged node count, so a fix that merely silences the
// assert while leaving an orphan node behind in the graph also fails.
//
// The existing PinWright.niagara.graph.create_node.PayloadApplicationAndErrorCodes test stayed
// green through the whole defect because it calls NiagaraGraphCreate::ApplyCreateNodePayload
// directly on a NewObject'd node — no FGraphNodeCreator is ever constructed on that path, so the
// destructor that does the killing is not under test. These cases go through the dispatcher
// against a real UNiagaraGraph instead.

#include "Misc/AutomationTest.h"
#include "Tests/TestUtils.h"

#include "Dom/JsonObject.h"
#include "EdGraphSchema_Niagara.h"
#include "Misc/Guid.h"
#include "Misc/ScopeExit.h"
#include "NiagaraGraph.h"
#include "NiagaraScript.h"
#include "NiagaraScriptSource.h"
#include "NiagaraSystem.h"
#include "UObject/Package.h"

namespace
{
    // Minimal synthetic fixture: a Niagara system whose system-spawn script carries a real
    // UNiagaraGraph, which is what create_node resolves through target {kind:"graph"}. Deliberately
    // built from nothing rather than from any particular content — the defect is in the handler's
    // construct-then-validate ordering, not in any asset.
    UNiagaraSystem* NewCreateNodeGuardSystem(FString& OutObjectPath, UNiagaraGraph*& OutGraph)
    {
        OutGraph = nullptr;

        const FString AssetName = FString::Printf(
            TEXT("NS_CreateNodeGuard_%s"),
            *FGuid::NewGuid().ToString(EGuidFormats::Digits));
        const FString PackageName = FString::Printf(TEXT("/Game/PinWrightTests/%s"), *AssetName);

        UPackage* Package = CreatePackage(*PackageName);
        UNiagaraSystem* System = NewObject<UNiagaraSystem>(
            Package,
            UNiagaraSystem::StaticClass(),
            *AssetName,
            RF_Public | RF_Standalone | RF_Transient);
        if (!System)
        {
            return nullptr;
        }
        System->AddToRoot();
        OutObjectPath = FString::Printf(TEXT("%s.%s"), *PackageName, *AssetName);

        // UNiagaraSystem::PostInitProperties creates the system scripts but does not wire a source
        // onto them (that happens in PostLoad), so the graph has to be attached by hand here.
        UNiagaraScript* SpawnScript = System->GetSystemSpawnScript();
        if (!SpawnScript)
        {
            return System;
        }

        UNiagaraScriptSource* Source = NewObject<UNiagaraScriptSource>(
            SpawnScript, TEXT("CreateNodeGuardSource"), RF_Transient | RF_Transactional);
        UNiagaraGraph* Graph = Source
            ? NewObject<UNiagaraGraph>(Source, TEXT("CreateNodeGuardGraph"), RF_Transient | RF_Transactional)
            : nullptr;
        if (!Graph)
        {
            return System;
        }

        // UNiagaraNode::AllocateDefaultPins reaches for the Niagara schema through the graph.
        Graph->Schema = UEdGraphSchema_Niagara::StaticClass();
        Source->NodeGraph = Graph;
        SpawnScript->SetLatestSource(Source);

        OutGraph = Graph;
        return System;
    }

    TSharedPtr<FJsonObject> MakeCreateNodeParams(const FString& AssetPath, const TCHAR* NodeClass)
    {
        TSharedPtr<FJsonObject> Target = MakeShared<FJsonObject>();
        Target->SetStringField(TEXT("kind"), TEXT("graph"));
        Target->SetStringField(TEXT("scriptUsage"), TEXT("SystemSpawn"));

        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("assetPath"), AssetPath);
        Params->SetObjectField(TEXT("target"), Target);
        Params->SetStringField(TEXT("nodeClass"), NodeClass);
        Params->SetNumberField(TEXT("x"), 0.0);
        Params->SetNumberField(TEXT("y"), 0.0);
        return Params;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FNiagaraGraphCreateNodeCreatorGuardTest,
    "PinWright.niagara.graph.create_node_guard.RejectionLeavesGraphUnchanged",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraGraphCreateNodeCreatorGuardTest::RunTest(const FString& Parameters)
{
    FString ObjectPath;
    UNiagaraGraph* Graph = nullptr;
    UNiagaraSystem* System = NewCreateNodeGuardSystem(ObjectPath, Graph);

    TestNotNull(TEXT("fixture Niagara system created"), System);
    if (!System)
    {
        return false;
    }

    ON_SCOPE_EXIT
    {
        System->RemoveFromRoot();
    };

    TestNotNull(TEXT("fixture system-spawn graph attached"), Graph);
    if (!Graph)
    {
        return false;
    }

    const int32 BaselineNodes = Graph->Nodes.Num();

    // -----------------------------------------------------------------------
    // 1. Unsupported nodeClass. UNiagaraNodeAssignment resolves fine (it is a UNiagaraNode
    //    subclass) but is outside the v1 list, which is the exact repro from both tickets.
    //    The rejection must arrive with no node constructed at all.
    // -----------------------------------------------------------------------
    {
        FTestResponseCapture Capture;
        const bool bFound = InvokeHandlerWithCapture(
            TEXT("niagara.graph.create_node"),
            MakeCreateNodeParams(ObjectPath, TEXT("NiagaraNodeAssignment")),
            Capture);

        TestTrue(TEXT("niagara.graph.create_node handler found"), bFound);
        TestTrue(TEXT("unsupported nodeClass sent a response"), Capture.bWasCalled);
        TestFalse(TEXT("unsupported nodeClass is not a success"), Capture.bSuccess);
        TestEqual(TEXT("unsupported nodeClass error code"),
            Capture.ErrorCode, FString(TEXT("UNSUPPORTED_NODE_CLASS")));
        TestEqual(TEXT("unsupported nodeClass added no node to the graph"),
            Graph->Nodes.Num(), BaselineNodes);
    }

    // -----------------------------------------------------------------------
    // 2. Supported class, rejected payload. This is the path that still runs inside the
    //    creator's lifetime, so it is the one that proves Finalize() is unconditional: the
    //    node is built, the payload is refused, and the handler must still finalize and then
    //    remove the node rather than returning through the destructor.
    // -----------------------------------------------------------------------
    {
        TSharedPtr<FJsonObject> Params = MakeCreateNodeParams(ObjectPath, TEXT("NiagaraNodeOp"));
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("opName"), TEXT("DoesNotExist_XYZ_NotARealOp"));
        Params->SetObjectField(TEXT("payload"), Payload);

        FTestResponseCapture Capture;
        InvokeHandlerWithCapture(TEXT("niagara.graph.create_node"), Params, Capture);

        TestTrue(TEXT("rejected payload sent a response"), Capture.bWasCalled);
        TestFalse(TEXT("rejected payload is not a success"), Capture.bSuccess);
        TestEqual(TEXT("rejected payload error code"),
            Capture.ErrorCode, FString(TEXT("INVALID_OP")));
        TestEqual(TEXT("rejected payload left no node behind in the graph"),
            Graph->Nodes.Num(), BaselineNodes);
    }

    // -----------------------------------------------------------------------
    // 3. The verb is still reachable and the accepted path still finalizes. UNiagaraNodeReroute
    //    takes no payload, so this asserts the restructured creator block, not payload handling.
    // -----------------------------------------------------------------------
    {
        FTestResponseCapture Capture;
        InvokeHandlerWithCapture(
            TEXT("niagara.graph.create_node"),
            MakeCreateNodeParams(ObjectPath, TEXT("NiagaraNodeReroute")),
            Capture);

        TestTrue(TEXT("accepted request sent a response"), Capture.bWasCalled);
        TestTrue(TEXT("accepted request succeeded"), Capture.bSuccess);
        if (!Capture.bSuccess)
        {
            AddError(FString::Printf(TEXT("niagara.graph.create_node failed on a supported class: %s — %s"),
                *Capture.ErrorCode, *Capture.Message));
            return false;
        }

        TestEqual(TEXT("accepted request added exactly one node"),
            Graph->Nodes.Num(), BaselineNodes + 1);

        FString NodeId;
        TestTrue(TEXT("accepted request reports a nodeId"),
            Capture.Result.IsValid() && Capture.Result->TryGetStringField(TEXT("nodeId"), NodeId));
        // Finalize() is what assigns the GUID, so an empty nodeId would mean the node was never
        // finalized even though the call reported success.
        TestFalse(TEXT("reported nodeId is not empty"), NodeId.IsEmpty());
    }

    return true;
}
