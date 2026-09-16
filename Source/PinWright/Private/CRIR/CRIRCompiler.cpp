// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "CRIR/CRIRCompiler.h"

#include "CRIR/CRIRControlValueParser.h"
#include "CRIR/CRIRLayoutEngine.h"
#include "CRIR/CRIROpcodes.h"
#include "CRIR/CRIRParser.h"
#include "CRIR/CRIRPinResolver.h"
#include "CRIR/CRIRWireNaming.h"

#include "Utils/ControlRigBlueprintCompat.h"
#include "Utils/TransactionUtils.h"
#include "EdGraph/RigVMEdGraph.h"
#include "Rigs/RigHierarchy.h"
#include "Rigs/RigHierarchyController.h"
#include "Rigs/RigHierarchyDefines.h"
#include "Rigs/RigHierarchyElements.h"
#include "RigVMCore/RigVMDispatchFactory.h"
#include "RigVMCore/RigVMRegistry.h"
#include "RigVMModel/Nodes/RigVMCollapseNode.h"
#include "RigVMModel/Nodes/RigVMFunctionEntryNode.h"
#include "RigVMModel/Nodes/RigVMFunctionReferenceNode.h"
#include "RigVMModel/Nodes/RigVMFunctionReturnNode.h"
#include "RigVMModel/Nodes/RigVMLibraryNode.h"
#include "RigVMModel/Nodes/RigVMTemplateNode.h"
#include "RigVMModel/Nodes/RigVMUnitNode.h"
#include "RigVMModel/RigVMClient.h"
#include "RigVMModel/RigVMController.h"
#include "RigVMModel/RigVMFunctionLibrary.h"
#include "RigVMModel/RigVMGraph.h"
#include "RigVMModel/RigVMNode.h"
#include "RigVMModel/RigVMPin.h"

#include "IrCore/IrTextUtils.h"

#include "ScopedTransaction.h"
#include "Units/RigUnit.h"
#include "UObject/Class.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"
#include "Utils/AssetUtils.h"
#include "Utils/GuardedLoad.h"

#define LOCTEXT_NAMESPACE "CRIR"

namespace
{
using CRIRWireNaming::IsWireArgName;

bool SetControlRigAssetNotificationsSuspended(UControlRigBlueprint* Blueprint, bool bSuspend)
{
    if (!Blueprint)
    {
        return false;
    }

    if (UFunction* SuspendNotificationsFunction = Blueprint->FindFunction(TEXT("SuspendNotifications")))
    {
        struct FSuspendNotificationsParams
        {
            bool bSuspendNotifs;
        } Params{bSuspend};
        Blueprint->ProcessEvent(SuspendNotificationsFunction, &Params);
        return true;
    }

    return false;
}

// UE 5.8 exposes no public getter for the asset-wide notification state. The
// public refresh event is emitted synchronously only when SuspendNotifications
// transitions from suspended to resumed, so use that transition as the state
// probe and restore the caller's state immediately afterward.
bool BeginControlRigAssetNotificationSuspension(UControlRigBlueprint* Blueprint)
{
    if (!Blueprint)
    {
        return false;
    }

    UFunction* SuspendNotificationsFunction = Blueprint->FindFunction(
        TEXT("SuspendNotifications"));
    FPwRigVMEditorAssetInterfacePtr Asset = GetControlRigEditorAsset(Blueprint);
    if (!SuspendNotificationsFunction || !Asset)
    {
        return false;
    }

    bool bRefreshObserved = false;
    const FDelegateHandle RefreshHandle = Asset->OnRefreshEditor().AddLambda(
        [&bRefreshObserved](FPwRigVMEditorAssetInterfacePtr)
        {
            bRefreshObserved = true;
        });

    struct FSuspendNotificationsParams
    {
        bool bSuspendNotifs;
    } Params{false};
    Blueprint->ProcessEvent(SuspendNotificationsFunction, &Params);
    Asset->OnRefreshEditor().Remove(RefreshHandle);

    Params.bSuspendNotifs = true;
    Blueprint->ProcessEvent(SuspendNotificationsFunction, &Params);

    // No refresh means the caller was not suspended and this scope owns the
    // suspension. A refresh means the probe resumed caller-owned suspension;
    // the second call restored it, so Restore must leave it untouched.
    return !bRefreshObserved;
}

struct FControlRigTransactionalSnapshot
{
    TArray<URigVMGraph*> Graphs;
    TSet<URigVMNode*> ExistingNodes;
};

void FilterRigVMGraphsToCurrentClient(
    FRigVMClient* Client,
    TArray<URigVMGraph*>& Graphs)
{
    if (!Client)
    {
        Graphs.Reset();
        return;
    }

    TSet<URigVMGraph*> CurrentGraphs;
    for (URigVMGraph* Graph : Client->GetAllModels(
             /*bIncludeFunctionLibrary*/ true,
             /*bRecursive*/ true))
    {
        if (IsValid(Graph))
        {
            CurrentGraphs.Add(Graph);
        }
    }

    Graphs.RemoveAll(
        [&CurrentGraphs](URigVMGraph* Graph)
        {
            return !IsValid(Graph) || !CurrentGraphs.Contains(Graph);
        });
}

class FCRIRRollbackScope
{
public:
    FCRIRRollbackScope(
        UControlRigBlueprint* InBlueprint,
        const FControlRigTransactionalSnapshot& Snapshot)
        : RigVMBlueprint(InBlueprint ? static_cast<URigVMBlueprint*>(InBlueprint) : nullptr)
        , bPreviousAutoVMRecompile(RigVMBlueprint && RigVMBlueprint->GetAutoVMRecompile())
    {
        if (!RigVMBlueprint)
        {
            return;
        }

        RigVMBlueprint->SetAutoVMRecompile(false);
        bAssetNotificationsSuspended = BeginControlRigAssetNotificationSuspension(
            static_cast<UControlRigBlueprint*>(InBlueprint));

        FRigVMClient* Client = GetControlRigRigVMClient(InBlueprint);
        if (!Client)
        {
            return;
        }

        TArray<URigVMGraph*> GraphsToGuard = Client->GetAllModels(
            /*bIncludeFunctionLibrary*/ true,
            /*bRecursive*/ true);
        FilterRigVMGraphsToCurrentClient(Client, GraphsToGuard);
        for (URigVMGraph* Graph : Snapshot.Graphs)
        {
            if (IsValid(Graph) && GraphsToGuard.Contains(Graph))
            {
                GraphsToGuard.AddUnique(Graph);
            }
        }

        TSet<URigVMController*> GuardedControllers;
        for (URigVMGraph* Graph : GraphsToGuard)
        {
            URigVMController* Controller = IsValid(Graph) ? Client->GetController(Graph) : nullptr;
            if (Controller && !GuardedControllers.Contains(Controller))
            {
                GuardedControllers.Add(Controller);
                ControllerNotificationGuards.Add(
                    MakeUnique<FRigVMControllerNotifGuard>(Controller));
            }
        }
    }

    ~FCRIRRollbackScope()
    {
        Restore();
    }

    void Finish(const FControlRigTransactionalSnapshot& Snapshot)
    {
        if (bRestored || !RigVMBlueprint)
        {
            return;
        }

        FRigVMClient* Client = GetControlRigRigVMClient(
            static_cast<UControlRigBlueprint*>(RigVMBlueprint));
        if (Client)
        {
        TArray<URigVMGraph*> GraphsToPrune = Client->GetAllModels(
                /*bIncludeFunctionLibrary*/ true,
                /*bRecursive*/ true);
            FilterRigVMGraphsToCurrentClient(Client, GraphsToPrune);
            for (URigVMGraph* Graph : Snapshot.Graphs)
            {
                if (IsValid(Graph) && GraphsToPrune.Contains(Graph))
                {
                    GraphsToPrune.AddUnique(Graph);
                }
            }

            for (URigVMGraph* Graph : GraphsToPrune)
            {
                if (IsValid(Graph))
                {
                    if (URigVMController* Controller = Client->GetOrCreateController(Graph))
                    {
                        Controller->RemoveStaleNodes();
                    }
                }
            }
        }

        if (bPreviousAutoVMRecompile)
        {
            // Set the pending flag while auto-recompile is disabled. Restore then
            // performs one compile against the pruned, restored model.
            RigVMBlueprint->RequestAutoVMRecompilation();
        }
        Restore();
    }

private:
    void RebindEditorGraphNotifications()
    {
        if (!RigVMBlueprint)
        {
            return;
        }

        FPwRigVMEditorAssetInterfacePtr Asset =
            GetControlRigEditorAsset(static_cast<UControlRigBlueprint*>(RigVMBlueprint));
        if (!Asset)
        {
            return;
        }

        TArray<UEdGraph*> EdGraphs;
        GetControlRigEdGraphs(Asset, EdGraphs);
        for (UEdGraph* EdGraph : EdGraphs)
        {
            if (URigVMEdGraph* RigGraph = Cast<URigVMEdGraph>(EdGraph))
            {
                InitializeRigVMEdGraphFromAsset(RigGraph, Asset);
            }
        }
    }

    void Restore()
    {
        if (bRestored || !RigVMBlueprint)
        {
            return;
        }

        bRestored = true;
        ControllerNotificationGuards.Reset();
        RebindEditorGraphNotifications();
        if (bAssetNotificationsSuspended)
        {
            SetControlRigAssetNotificationsSuspended(
                static_cast<UControlRigBlueprint*>(RigVMBlueprint), false);
            bAssetNotificationsSuspended = false;
        }
        RigVMBlueprint->SetAutoVMRecompile(bPreviousAutoVMRecompile);
        if (bPreviousAutoVMRecompile)
        {
            RigVMBlueprint->RecompileVMIfRequired();
        }
    }

    URigVMBlueprint* RigVMBlueprint = nullptr;
    bool bPreviousAutoVMRecompile = false;
    bool bAssetNotificationsSuspended = false;
    bool bRestored = false;
    TArray<TUniquePtr<FRigVMControllerNotifGuard>> ControllerNotificationGuards;
};

FControlRigTransactionalSnapshot PrepareControlRigTransactionalSnapshot(UControlRigBlueprint* Blueprint)
{
    FControlRigTransactionalSnapshot Snapshot;
    TArray<UObject*> ObjectsToSnapshot;
    TSet<UObject*> SeenObjects;
    auto AddObject = [&ObjectsToSnapshot, &SeenObjects](UObject* Object)
    {
        if (Object && !SeenObjects.Contains(Object))
        {
            SeenObjects.Add(Object);
            ObjectsToSnapshot.Add(Object);
        }
    };

    AddObject(Blueprint);
    AddObject(GetControlRigHierarchy(Blueprint));

    FRigVMClient* Client = GetControlRigRigVMClient(Blueprint);
    if (Client)
    {
        for (URigVMGraph* Graph : Client->GetAllModels(
                  /*bIncludeFunctionLibrary*/ true,
                  /*bRecursive*/ true))
        {
            if (!IsValid(Graph))
            {
                continue;
            }

            Snapshot.Graphs.Add(Graph);

            for (URigVMNode* Node : Graph->GetNodes())
            {
                Snapshot.ExistingNodes.Add(Node);
            }

            // RigVM graph mutations are restored by the controller action
            // stack. Do not put graphs or graph-owned nodes, pins, links, or
            // contained graphs in the editor transaction: its property undo
            // runs before action-stack undo and can reintroduce dead entries
            // into Graph->Nodes, which RigVM then dereferences while importing.
        }
    }

#if UE_VERSION_OLDER_THAN(5, 7, 0)
    // The same hazard, one level up: FRigVMClient::Controllers is a transient UPROPERTY on the
    // blueprint, so snapshotting the blueprint puts the whole controller map into the editor
    // transaction. Removing a collapse or function node tears its contained graph's controller
    // down (RigVMController.cpp calls FRigVMClient::RemoveController), and the action-stack undo
    // then re-imports that node, allocating a FRESH contained graph at the same object path. The
    // property undo has meanwhile restored the map slot keyed by that path, still pointing at the
    // dead controller - and pre-5.7 FRigVMClient::GetController checks slot identity with no
    // garbage test (5.7 added one, which makes the revived slot harmless), so the next lookup
    // during the undo asserts "contains unexpected graph" and takes the editor down.
    //
    // Keep contained-graph slots out of the record: drop them before the snapshot, recreate them
    // straight after it, so the compile still runs with live controllers and the map the undo
    // restores carries only the top-level model slots, whose graphs the undo never reallocates.
    TArray<URigVMGraph*> ContainedGraphs;
    if (Client)
    {
        ContainedGraphs = Client->GetAllModels(/*bIncludeFunctionLibrary*/ true, /*bRecursive*/ true);
        const TArray<URigVMGraph*> TopLevelGraphs =
            Client->GetAllModels(/*bIncludeFunctionLibrary*/ true, /*bRecursive*/ false);
        ContainedGraphs.RemoveAll(
            [&TopLevelGraphs](URigVMGraph* Graph)
            {
                return !IsValid(Graph) || TopLevelGraphs.Contains(Graph);
            });
        for (URigVMGraph* Graph : ContainedGraphs)
        {
            (void)Client->RemoveController(Graph);
        }
    }
#endif

    for (UObject* Object : ObjectsToSnapshot)
    {
        PinWrightTransactionUtils::PrepareTransactionalSnapshot(Object);
    }

#if UE_VERSION_OLDER_THAN(5, 7, 0)
    for (URigVMGraph* Graph : ContainedGraphs)
    {
        if (IsValid(Graph))
        {
            (void)Client->GetOrCreateController(Graph);
        }
    }
#endif

    return Snapshot;
}

void RemoveNodesCreatedAfterSnapshot(
    UControlRigBlueprint* Blueprint,
    const FControlRigTransactionalSnapshot& Snapshot)
{
    FRigVMClient* Client = GetControlRigRigVMClient(Blueprint);
    if (!Client)
    {
        return;
    }

    // Newly created nodes did not exist when the transaction snapshot was
    // recorded. Inspect the current top-level models/function library too, as
    // the function library can be lazily materialized during a failed compile.
    // Remove them through the controller before applying the undo so
    // collapse/function teardown also clears contained graphs and controllers.
    TArray<URigVMGraph*> GraphsToInspect = Client->GetAllModels(
        /*bIncludeFunctionLibrary*/ true,
        /*bRecursive*/ false);
    FilterRigVMGraphsToCurrentClient(Client, GraphsToInspect);
    for (URigVMGraph* Graph : Snapshot.Graphs)
    {
        if (IsValid(Graph) && GraphsToInspect.Contains(Graph))
        {
            GraphsToInspect.AddUnique(Graph);
        }
    }
    for (URigVMGraph* Graph : GraphsToInspect)
    {
        URigVMController* Controller = IsValid(Graph) ? Client->GetController(Graph) : nullptr;
        if (!Controller)
        {
            continue;
        }

        FRigVMControllerNotifGuard NotificationGuard(Controller);
        const TArray<URigVMNode*> CurrentNodes = Graph->GetNodes();
        for (URigVMNode* Node : CurrentNodes)
        {
            if (Node && !Snapshot.ExistingNodes.Contains(Node))
            {
                Controller->RemoveNode(
                    Node,
                    /*bSetupUndoRedo*/ false,
                    /*bPrintPythonCommand*/ false);
            }
        }
    }
}

URigVMGraph* FindGraphByName(URigVMBlueprint* Blueprint, const FString& ModelName)
{
    if (!Blueprint)
    {
        return nullptr;
    }
    FRigVMClient* Client = GetControlRigRigVMClient(Blueprint);
    if (!Client)
    {
        return nullptr;
    }
    const TArray<URigVMGraph*> Models = Client->GetAllModels(/*bIncludeFunctionLibrary*/ false, /*bRecursive*/ false);

    // CRIRParser.cpp:526 documents that an absent rig_graph name means "use the
    // asset's first model". The decompiler may emit that form when the BP has
    // exactly one model; honor it here so round-trip compiles cleanly.
    if (ModelName.IsEmpty())
    {
        return Models.Num() > 0 ? Models[0] : nullptr;
    }
    for (URigVMGraph* Graph : Models)
    {
        if (Graph && Graph->GetGraphName() == ModelName)
        {
            return Graph;
        }
    }
    return nullptr;
}

// Replace-mode wipe. Use one action-backed controller operation so RigVM's
// serialized node snapshot is available when the surrounding editor
// transaction rolls back. Property undo alone cannot resurrect nodes that
// RemoveNodes has renamed and marked as garbage.
void ClearGraphForReplace(URigVMController* Controller, URigVMGraph* Graph)
{
    if (!Controller || !Graph)
    {
        return;
    }
    const TArray<URigVMNode*> Nodes = Graph->GetNodes();
    if (Nodes.Num() > 0)
    {
        Controller->RemoveNodes(
            Nodes,
            /*bSetupUndoRedo*/ true,
            /*bPrintPythonCommand*/ false);
    }
}

// Extend-mode reuse for singleton event nodes. Event units like
// RigUnit_BeginExecution ("Forwards Solve") carry CanOnlyExistOnce; the engine
// auto-creates one in every freshly-created Control Rig graph and refuses a
// second via AddUnitNode (RigVMController.cpp: "Event ... already exists in the
// graph", returns null). In Replace mode ClearGraphForReplace wipes the
// pre-existing node first, so re-adding succeeds; in Extend mode it survives,
// so the decompiled event unit collides. When that happens the existing node is
// exactly the one the CRIR text describes — find and reuse it instead of
// failing. Returns null for non-event structs or when no matching node exists.
URigVMNode* FindExistingEventNode(URigVMGraph* Graph, const FString& StructPath)
{
    if (!Graph || StructPath.IsEmpty())
    {
        return nullptr;
    }
    for (URigVMNode* Node : Graph->GetNodes())
    {
        URigVMUnitNode* UnitNode = Cast<URigVMUnitNode>(Node);
        if (!UnitNode || !UnitNode->IsEvent())
        {
            continue;
        }
        const UScriptStruct* Struct = UnitNode->GetScriptStruct();
        if (Struct && Struct->GetPathName() == StructPath)
        {
            return UnitNode;
        }
    }
    return nullptr;
}

FCRIRCompileResult CompileInstructionsIntoGraph(
    URigVMBlueprint* Blueprint,
    URigVMController* Controller,
    URigVMGraph* Graph,
    const TArray<FCRIRInstruction>& Instructions,
    const FCRIRCompileOptions& Options,
    int32& OutNodesCreated,
    TSet<URigVMNode*>& OutNodesNeedingLayout,
    TArray<FString>& OutWarnings,
    TArray<URigVMNode*>* OutTopLevelCreated = nullptr);

// Resolves the auto-created entry/return nodes inside a sub-graph and replays
// pin defaults from any `function_entry` / `function_return` instructions onto
// them. Never calls Add* on the interface nodes — they are owned by the
// engine's collapse/library scaffolding and cannot be re-created.
void ReconcileInterfaceNodes(
    URigVMBlueprint* Blueprint,
    URigVMGraph* InnerGraph,
    const TArray<FCRIRInstruction>& Instructions,
    TArray<FString>& OutWarnings)
{
    if (!Blueprint || !InnerGraph)
    {
        return;
    }
    FRigVMClient* Client = GetControlRigRigVMClient(Blueprint);
    URigVMController* InnerController = Client ? Client->GetController(InnerGraph) : nullptr;
    if (!InnerController)
    {
        return;
    }

    URigVMFunctionEntryNode* EntryNode = InnerGraph->GetEntryNode();
    URigVMFunctionReturnNode* ReturnNode = InnerGraph->GetReturnNode();

    for (const FCRIRInstruction& Inst : Instructions)
    {
        URigVMNode* TargetNode = nullptr;
        if (Inst.Opcode == ECRIROpcode::FunctionEntry)
        {
            TargetNode = EntryNode;
        }
        else if (Inst.Opcode == ECRIROpcode::FunctionReturn)
        {
            TargetNode = ReturnNode;
        }
        else
        {
            continue;
        }
        if (!TargetNode)
        {
            OutWarnings.Add(FString::Printf(
                TEXT("CRIR_INTERFACE_NODE_MISSING: %s (line %d): contained graph has no auto-created entry/return"),
                Inst.Opcode == ECRIROpcode::FunctionEntry ? TEXT("function_entry") : TEXT("function_return"),
                Inst.SourceLine));
            continue;
        }
        for (const FCRIRArg& Arg : Inst.Args)
        {
            if (Arg.Name.IsEmpty() || Arg.bIsLocalRef)
            {
                continue;
            }
            const FString PinPath = FString::Printf(TEXT("%s.%s"), *TargetNode->GetName(), *Arg.Name);
            if (!InnerController->SetPinDefaultValue(
                    PinPath,
                    Arg.RawText,
                    /*bResizeArrays*/ true,
                    /*bSetupUndoRedo*/ false,
                    /*bMergeUndoAction*/ false,
                    /*bPrintPythonCommand*/ false))
            {
                OutWarnings.Add(FString::Printf(
                    TEXT("CRIR_PIN_DEFAULT_FAILED: %s = %s (line %d)"),
                    *PinPath, *Arg.RawText, Inst.SourceLine));
            }
        }
    }
}

// TODO outer-return-only: CompileRigGraphBlock / CompileRigHierarchyBlock
// only consume the {bSuccess, ErrorCode, ErrorMessage} subset of the returned
// struct; the AssetPath / counts / Warnings fields belong on the outer Compile()
// return only. Refactor to a local FCRIRBlockOutcome to make the contract honest.
FCRIRCompileResult CompileRigGraphBlock(
    URigVMBlueprint* Blueprint,
    const FCRIREntryBlock& Block,
    const FCRIRCompileOptions& Options,
    int32& OutNodesCreated,
    TSet<URigVMNode*>& OutNodesNeedingLayout,
    URigVMGraph*& OutGraph,
    URigVMController*& Controller,
    TArray<FString>& OutWarnings)
{
    OutGraph = FindGraphByName(Blueprint, Block.Name);
    if (!OutGraph)
    {
        return FCRIRCompileResult::MakeError(
            TEXT("CRIR_MODEL_NOT_FOUND"),
            FString::Printf(TEXT("RigVM model '%s' not found on %s. Phase A does not auto-create models."),
                *Block.Name, *Blueprint->GetPathName()));
    }

    FRigVMClient* Client = GetControlRigRigVMClient(Blueprint);
    Controller = Client ? Client->GetController(OutGraph) : nullptr;
    if (!Controller)
    {
        return FCRIRCompileResult::MakeError(
            TEXT("CRIR_CONTROLLER_UNAVAILABLE"),
            FString::Printf(TEXT("No URigVMController available for model '%s'."), *Block.Name));
    }

    if (Options.Mode == ECRIRCompileMode::Replace)
    {
        ClearGraphForReplace(Controller, OutGraph);
    }

    return CompileInstructionsIntoGraph(
        Blueprint, Controller, OutGraph, Block.Instructions, Options,
        OutNodesCreated, OutNodesNeedingLayout, OutWarnings);
}

// Compiles a `rig_function "<Name>" { ... }` block. Adds the function to the
// blueprint's local library, then compiles the body into the resulting library
// node's contained graph via its dedicated controller.
FCRIRCompileResult CompileRigFunctionBlock(
    URigVMBlueprint* Blueprint,
    const FCRIREntryBlock& Block,
    const FCRIRCompileOptions& Options,
    int32& OutNodesCreated,
    TSet<URigVMNode*>& OutNodesNeedingLayout,
    TArray<FString>& OutWarnings)
{
    if (Block.Name.IsEmpty())
    {
        return FCRIRCompileResult::MakeError(
            TEXT("CRIR_BAD_BLOCK_HEADER"),
            TEXT("rig_function block requires a name"));
    }
    FRigVMClient* Client = GetControlRigRigVMClient(Blueprint);
    // On UE 5.3 a freshly created/loaded Control Rig Blueprint has no local function library
    // (and thus no controller) until first use; 5.4+ pre-creates it. GetOrCreate* returns the
    // existing objects on 5.4+ and lazily creates them on 5.3, so a CRIR text carrying a
    // rig_function block can compile into a brand-new target on every supported engine.
    URigVMFunctionLibrary* Library = Client ? Client->GetOrCreateFunctionLibrary(/*bSetupUndoRedo*/ false) : nullptr;
    if (!Library)
    {
        return FCRIRCompileResult::MakeError(
            TEXT("CRIR_FUNCTION_LIBRARY_UNAVAILABLE"),
            FString::Printf(TEXT("Local function library unavailable on %s."), *Blueprint->GetPathName()));
    }
    URigVMController* LibController = Client->GetOrCreateController(Library);
    if (!LibController)
    {
        return FCRIRCompileResult::MakeError(
            TEXT("CRIR_CONTROLLER_UNAVAILABLE"),
            TEXT("No URigVMController available for the function library."));
    }

    URigVMLibraryNode* FuncNode = LibController->AddFunctionToLibrary(
        FName(*Block.Name),
        /*bMutable*/ true,
        FVector2D::ZeroVector,
        /*bSetupUndoRedo*/ false,
        /*bPrintPythonCommand*/ false);
    if (!FuncNode)
    {
        return FCRIRCompileResult::MakeError(
            TEXT("CRIR_FUNCTION_CREATE_FAILED"),
            FString::Printf(TEXT("AddFunctionToLibrary('%s') returned null."), *Block.Name));
    }

    URigVMGraph* InnerGraph = FuncNode->GetContainedGraph();
    URigVMController* InnerController = InnerGraph ? Client->GetOrCreateController(InnerGraph) : nullptr;
    if (!InnerGraph || !InnerController)
    {
        return FCRIRCompileResult::MakeError(
            TEXT("CRIR_CONTROLLER_UNAVAILABLE"),
            FString::Printf(TEXT("Inner controller/graph unavailable for function '%s'."), *Block.Name));
    }

    FCRIRCompileResult InnerResult = CompileInstructionsIntoGraph(
        Blueprint, InnerController, InnerGraph, Block.Instructions, Options,
        OutNodesCreated, OutNodesNeedingLayout, OutWarnings);
    if (!InnerResult.bSuccess)
    {
        return InnerResult;
    }

    ReconcileInterfaceNodes(Blueprint, InnerGraph, Block.Instructions, OutWarnings);

    return FCRIRCompileResult{ true, FString(), FString(), Blueprint->GetPathName(), 0, 0, {} };
}

FCRIRCompileResult CompileInstructionsIntoGraph(
    URigVMBlueprint* Blueprint,
    URigVMController* Controller,
    URigVMGraph* Graph,
    const TArray<FCRIRInstruction>& Instructions,
    const FCRIRCompileOptions& Options,
    int32& OutNodesCreated,
    TSet<URigVMNode*>& OutNodesNeedingLayout,
    TArray<FString>& OutWarnings,
    TArray<URigVMNode*>* OutTopLevelCreated)
{
    FRigVMClient* Client = GetControlRigRigVMClient(Blueprint);
    TMap<FString, URigVMNode*> LocalIdMap;
    LocalIdMap.Reserve(Instructions.Num());

    // Pass A — node creation + literal-arg defaults. Wire args are deferred to
    // Pass B because their `%localId` references may point at later-declared
    // nodes (the parser's reference-validation flag tolerates this in
    // round-trip self-checks).
    for (const FCRIRInstruction& Inst : Instructions)
    {
        const FVector2D Position(Inst.Position.X, Inst.Position.Y);
        URigVMNode* CreatedNode = nullptr;
        bool bReusedExisting = false;

        switch (Inst.Opcode)
        {
        case ECRIROpcode::Unit:
        {
            if (Options.Mode == ECRIRCompileMode::Extend)
            {
                CreatedNode = FindExistingEventNode(Graph, Inst.StructPath);
                bReusedExisting = (CreatedNode != nullptr);
            }
            if (!CreatedNode)
            {
                CreatedNode = Controller->AddUnitNodeFromStructPath(
                    Inst.StructPath,
                    FRigUnit::GetMethodName(),
                    Position,
                    /*InNodeName*/ Inst.LocalId,
                    /*bSetupUndoRedo*/ false,
                    /*bPrintPythonCommand*/ false);
            }
            if (!CreatedNode)
            {
                // Extend mode replays a singleton event unit (e.g. the
                // auto-created "Forwards Solve" / RigUnit_BeginExecution) that
                // already lives in the target graph; the engine rejects the
                // duplicate. Reuse the existing node so wires resolve instead of
                // hard-failing the compile.
                CreatedNode = FindExistingEventNode(Graph, Inst.StructPath);
                bReusedExisting = (CreatedNode != nullptr);
            }
            if (!CreatedNode)
            {
                return FCRIRCompileResult::MakeError(
                    TEXT("CRIR_UNIT_CREATE_FAILED"),
                    FString::Printf(TEXT("Failed to create unit node from struct '%s' (line %d)."),
                        *Inst.StructPath, Inst.SourceLine));
            }
            break;
        }
        case ECRIROpcode::Var:
        {
            CreatedNode = Controller->AddVariableNode(
                FName(*Inst.VarName),
                Inst.VarType,
                /*InCPPTypeObject*/ nullptr,
                /*bIsGetter*/ true,
                Inst.VarDefault,
                Position,
                /*InNodeName*/ Inst.LocalId,
                /*bSetupUndoRedo*/ false,
                /*bPrintPythonCommand*/ false);
            if (!CreatedNode)
            {
                return FCRIRCompileResult::MakeError(
                    TEXT("CRIR_VAR_CREATE_FAILED"),
                    FString::Printf(TEXT("Failed to create variable node '%s' of type '%s' (line %d)."),
                        *Inst.VarName, *Inst.VarType, Inst.SourceLine));
            }
            break;
        }
        case ECRIROpcode::Reroute:
        {
            // CPPTypeObject is passed as NAME_None for scalar types; struct/enum
            // reroutes will need decompiler-side resolution later (deferred until
            // a real-world fixture hits that path).
            CreatedNode = Controller->AddFreeRerouteNode(
                Inst.VarType,
                /*InCPPTypeObjectPath*/ NAME_None,
                /*bIsConstant*/ false,
                /*InCustomWidgetName*/ NAME_None,
                /*InDefaultValue*/ Inst.VarDefault,
                Position,
                /*InNodeName*/ Inst.LocalId,
                /*bSetupUndoRedo*/ false);
            if (!CreatedNode)
            {
                return FCRIRCompileResult::MakeError(
                    TEXT("CRIR_REROUTE_CREATE_FAILED"),
                    FString::Printf(TEXT("Failed to create reroute node of type '%s' (line %d)."),
                        *Inst.VarType, Inst.SourceLine));
            }
            break;
        }
        case ECRIROpcode::Comment:
        {
            // Defaults mirror AddCommentNode's own defaults. Malformed
            // size/color silently fall back to defaults (matches the
            // pre-helper inline behavior — no error code is defined for them).
            FVector2D Size(400.f, 300.f);
            FLinearColor Color = FLinearColor::Black;
            TArray<double> Components;
            if (!Inst.VarType.IsEmpty() && FCRIRControlValueParser::TryParseDoubleTuple(Inst.VarType, 2, Components))
            {
                Size = FVector2D(Components[0], Components[1]);
            }
            if (!Inst.VarDefault.IsEmpty() && FCRIRControlValueParser::TryParseDoubleTuple(Inst.VarDefault, 4, Components))
            {
                Color = FLinearColor(Components[0], Components[1], Components[2], Components[3]);
            }
            CreatedNode = Controller->AddCommentNode(
                Inst.VarName,
                Position,
                Size,
                Color,
                /*InNodeName*/ Inst.LocalId,
                /*bSetupUndoRedo*/ false,
                /*bPrintPythonCommand*/ false);
            if (!CreatedNode)
            {
                return FCRIRCompileResult::MakeError(
                    TEXT("CRIR_COMMENT_CREATE_FAILED"),
                    FString::Printf(TEXT("Failed to create comment node (line %d)."), Inst.SourceLine));
            }
            break;
        }
        case ECRIROpcode::If:
        {
            CreatedNode = Controller->AddIfNode(
                Inst.StructPath,
                /*InCPPTypeObjectPath*/ NAME_None,
                Position,
                /*InNodeName*/ Inst.LocalId,
                /*bSetupUndoRedo*/ false,
                /*bPrintPythonCommand*/ false);
            if (!CreatedNode)
            {
                return FCRIRCompileResult::MakeError(
                    TEXT("CRIR_IF_CREATE_FAILED"),
                    FString::Printf(TEXT("Failed to create if node of type '%s' (line %d)."),
                        *Inst.StructPath, Inst.SourceLine));
            }
            break;
        }
        case ECRIROpcode::Select:
        {
            CreatedNode = Controller->AddSelectNode(
                Inst.StructPath,
                /*InCPPTypeObjectPath*/ NAME_None,
                Position,
                /*InNodeName*/ Inst.LocalId,
                /*bSetupUndoRedo*/ false,
                /*bPrintPythonCommand*/ false);
            if (!CreatedNode)
            {
                return FCRIRCompileResult::MakeError(
                    TEXT("CRIR_SELECT_CREATE_FAILED"),
                    FString::Printf(TEXT("Failed to create select node of type '%s' (line %d)."),
                        *Inst.StructPath, Inst.SourceLine));
            }
            break;
        }
        case ECRIROpcode::Enum:
        {
            CreatedNode = Controller->AddEnumNode(
                FName(*Inst.StructPath),
                Position,
                /*InNodeName*/ Inst.LocalId,
                /*bSetupUndoRedo*/ false,
                /*bPrintPythonCommand*/ false);
            if (!CreatedNode)
            {
                return FCRIRCompileResult::MakeError(
                    TEXT("CRIR_ENUM_CREATE_FAILED"),
                    FString::Printf(TEXT("Failed to create enum node for path '%s' (line %d)."),
                        *Inst.StructPath, Inst.SourceLine));
            }
            // Optional default — apply to the EnumValue pin.
            if (!Inst.VarDefault.IsEmpty())
            {
                const FString PinPath = FString::Printf(TEXT("%s.EnumValue"), *CreatedNode->GetName());
                if (!Controller->SetPinDefaultValue(
                        PinPath,
                        Inst.VarDefault,
                        /*bResizeArrays*/ true,
                        /*bSetupUndoRedo*/ false,
                        /*bMergeUndoAction*/ false,
                        /*bPrintPythonCommand*/ false))
                {
                    OutWarnings.Add(FString::Printf(
                        TEXT("CRIR_PIN_DEFAULT_FAILED: %s = %s (line %d)"),
                        *PinPath, *Inst.VarDefault, Inst.SourceLine));
                }
            }
            break;
        }
        case ECRIROpcode::InvokeEntry:
        {
            CreatedNode = Controller->AddInvokeEntryNode(
                FName(*Inst.VarName),
                Position,
                /*InNodeName*/ Inst.LocalId,
                /*bSetupUndoRedo*/ false,
                /*bPrintPythonCommand*/ false);
            if (!CreatedNode)
            {
                return FCRIRCompileResult::MakeError(
                    TEXT("CRIR_INVOKE_ENTRY_CREATE_FAILED"),
                    FString::Printf(TEXT("Failed to create invoke_entry node for '%s' (line %d)."),
                        *Inst.VarName, Inst.SourceLine));
            }
            break;
        }
        case ECRIROpcode::Template:
        {
            CreatedNode = Controller->AddTemplateNode(
                FName(*Inst.StructPath),
                Position,
                /*InNodeName*/ Inst.LocalId,
                /*bSetupUndoRedo*/ false,
                /*bPrintPythonCommand*/ false);
            if (!CreatedNode)
            {
                return FCRIRCompileResult::MakeError(
                    TEXT("CRIR_TEMPLATE_CREATE_FAILED"),
                    FString::Printf(TEXT("Failed to create template node with notation '%s' (line %d)."),
                        *Inst.StructPath, Inst.SourceLine));
            }
            break;
        }
        case ECRIROpcode::Dispatch:
        {
            // Dispatch nodes are created via AddTemplateNode keyed on the
            // factory's template notation. Per-factory helpers (AddBranchNode,
            // AddArrayNode, AddIfNode, …) ultimately route through the same
            // path; bypassing them keeps this branch uniform and avoids a
            // factory-name → helper switch.
            //
            // The decompiler emits the bare script struct name (e.g.
            // "RigVMDispatch_ArrayAdd") for diff-friendliness, but the registry
            // is keyed on the FactoryName which prepends "DISPATCH_" per
            // FRigVMDispatchFactory::GetFactoryName(). Try the literal token
            // first (round-trip from older dumps that may have included the
            // prefix), then fall back to the prefixed form.
            FRigVMRegistry& Registry = FRigVMRegistry::Get();
            const FRigVMDispatchFactory* Factory =
                Registry.FindDispatchFactory(FName(*Inst.StructPath));
            if (!Factory)
            {
                static const FString DispatchPrefix(TEXT("DISPATCH_"));
                const FString Prefixed = DispatchPrefix + Inst.StructPath;
                Factory = Registry.FindDispatchFactory(FName(*Prefixed));
            }
            if (!Factory)
            {
                return FCRIRCompileResult::MakeError(
                    TEXT("CRIR_DISPATCH_CREATE_FAILED"),
                    FString::Printf(TEXT("Dispatch factory '%s' not found in RigVM registry (line %d)."),
                        *Inst.StructPath, Inst.SourceLine));
            }
            CreatedNode = Controller->AddTemplateNode(
                Factory->GetTemplateNotation(),
                Position,
                /*InNodeName*/ Inst.LocalId,
                /*bSetupUndoRedo*/ false,
                /*bPrintPythonCommand*/ false);
            if (!CreatedNode)
            {
                return FCRIRCompileResult::MakeError(
                    TEXT("CRIR_DISPATCH_CREATE_FAILED"),
                    FString::Printf(TEXT("Failed to create dispatch node for factory '%s' (line %d)."),
                        *Inst.StructPath, Inst.SourceLine));
            }
            break;
        }
        case ECRIROpcode::Collapse:
        {
            // Materialize the inner Children into the parent graph first, then
            // call CollapseNodes to wrap them. The contained graph's
            // function_entry/function_return are auto-created by the engine;
            // their pin defaults are replayed via ReconcileInterfaceNodes.
            int32 InnerCreated = 0;
            TSet<URigVMNode*> InnerLayoutNodes;
            TArray<URigVMNode*> InnerCreatedNodes;
            FCRIRCompileResult ChildrenResult = CompileInstructionsIntoGraph(
                Blueprint, Controller, Graph, Inst.Children, Options,
                InnerCreated, InnerLayoutNodes, OutWarnings, &InnerCreatedNodes);
            if (!ChildrenResult.bSuccess)
            {
                return ChildrenResult;
            }
            TArray<FName> InnerNodeNames;
            InnerNodeNames.Reserve(InnerCreatedNodes.Num());
            for (URigVMNode* N : InnerCreatedNodes)
            {
                if (N) { InnerNodeNames.Add(N->GetFName()); }
            }
            URigVMCollapseNode* CollapseNode = Controller->CollapseNodes(
                InnerNodeNames,
                Inst.VarName,
                /*bSetupUndoRedo*/ false,
                /*bPrintPythonCommand*/ false,
                /*bIsAggregate*/ false);
            if (!CollapseNode)
            {
                return FCRIRCompileResult::MakeError(
                    TEXT("CRIR_COLLAPSE_CREATE_FAILED"),
                    FString::Printf(TEXT("CollapseNodes('%s') returned null (line %d)."),
                        *Inst.VarName, Inst.SourceLine));
            }
            CreatedNode = CollapseNode;
            // CollapseNodes derives the wrapper's position from the centroid of
            // the wrapped inner nodes — but those were placed at the inner-graph-
            // relative positions stored in the CRIR text, not their original
            // outer-graph positions. Restore the authored wrapper position so
            // round-trips are byte-stable.
            if (Inst.Position.bSet)
            {
                Controller->SetNodePosition(
                    CollapseNode,
                    Position,
                    /*bSetupUndoRedo*/ false,
                    /*bMergeUndoAction*/ false,
                    /*bPrintPythonCommand*/ false);
            }
            // Reconcile entry/return pin defaults inside the collapse body.
            ReconcileInterfaceNodes(Blueprint, CollapseNode->GetContainedGraph(), Inst.Children, OutWarnings);
            OutNodesCreated += InnerCreated;
            break;
        }
        case ECRIROpcode::FunctionRef:
        {
            // Token format: "Name" (same-asset) or "HostPath::Name" (external).
            FString Token = Inst.StructPath;
            int32 SepIdx = INDEX_NONE;
            const int32 LastColon = Token.Find(TEXT("::"), ESearchCase::CaseSensitive, ESearchDir::FromEnd);
            if (LastColon != INDEX_NONE)
            {
                SepIdx = LastColon;
            }
            if (SepIdx != INDEX_NONE)
            {
                const FString HostPath = Token.Left(SepIdx);
                const FString FuncName = Token.Mid(SepIdx + 2);
                CreatedNode = Controller->AddExternalFunctionReferenceNode(
                    HostPath,
                    FName(*FuncName),
                    Position,
                    /*InNodeName*/ Inst.LocalId,
                    /*bSetupUndoRedo*/ false,
                    /*bPrintPythonCommand*/ false);
                if (!CreatedNode)
                {
                    return FCRIRCompileResult::MakeError(
                        TEXT("CRIR_FUNCTION_REF_UNRESOLVED"),
                        FString::Printf(TEXT("AddExternalFunctionReferenceNode('%s::%s') returned null (line %d)."),
                            *HostPath, *FuncName, Inst.SourceLine));
                }
            }
            else
            {
                URigVMFunctionLibrary* Library = Client ? Client->GetFunctionLibrary() : nullptr;
                URigVMLibraryNode* LibNode = Library ? Library->FindFunction(FName(*Token)) : nullptr;
                if (!LibNode)
                {
                    return FCRIRCompileResult::MakeError(
                        TEXT("CRIR_FUNCTION_REF_UNRESOLVED"),
                        FString::Printf(TEXT("Local function '%s' not found in library (line %d)."),
                            *Token, Inst.SourceLine));
                }
                CreatedNode = Controller->AddFunctionReferenceNode(
                    LibNode,
                    Position,
                    /*InNodeName*/ Inst.LocalId,
                    /*bSetupUndoRedo*/ false,
                    /*bPrintPythonCommand*/ false);
                if (!CreatedNode)
                {
                    return FCRIRCompileResult::MakeError(
                        TEXT("CRIR_FUNCTION_REF_UNRESOLVED"),
                        FString::Printf(TEXT("AddFunctionReferenceNode('%s') returned null (line %d)."),
                            *Token, Inst.SourceLine));
                }
            }
            break;
        }
        case ECRIROpcode::FunctionEntry:
        case ECRIROpcode::FunctionReturn:
        {
            // Auto-created by the engine on every sub-graph; reconciled by the
            // caller via ReconcileInterfaceNodes after the collapse/library
            // body is in place. Skip here so we don't try to Add* them.
            continue;
        }
        case ECRIROpcode::ExposedPin:
        {
            // Declare one entry of the surrounding function's signature.
            // `AddExposedPin` is only legal on nested graphs (engine check at
            // RigVMController.cpp:13029). When CRIR is misused on a top-level
            // rig_graph the call returns NAME_None and we degrade to a warning.
            const ERigVMPinDirection Direction =
                (Inst.ExposedDirection == ECRIRExposedPinDirection::Output) ? ERigVMPinDirection::Output
                : (Inst.ExposedDirection == ECRIRExposedPinDirection::IO)    ? ERigVMPinDirection::IO
                : ERigVMPinDirection::Input;
            const FName Added = Controller->AddExposedPin(
                FName(*Inst.VarName),
                Direction,
                Inst.VarType,
                Inst.ExposedTypeObjectPath.IsEmpty() ? NAME_None : FName(*Inst.ExposedTypeObjectPath),
                Inst.VarDefault,
                /*bSetupUndoRedo*/ false,
                /*bPrintPythonCommand*/ false);
            if (Added == NAME_None)
            {
                OutWarnings.Add(FString::Printf(
                    TEXT("CRIR_EXPOSED_PIN_FAILED: '%s' (line %d) — AddExposedPin returned None"),
                    *Inst.VarName, Inst.SourceLine));
            }
            continue;
        }
        default:
            return FCRIRCompileResult::MakeError(
                TEXT("CRIR_UNSUPPORTED_OPCODE"),
                FString::Printf(TEXT("Unsupported CRIR opcode at line %d."), Inst.SourceLine));
        }

        // RigVMController may rename a node on collision; track by the actual
        // returned name so wire emission resolves to the live URigVMNode.
        // Comment nodes have no local id and don't participate in wire refs.
        if (!Inst.LocalId.IsEmpty())
        {
            LocalIdMap.Add(Inst.LocalId, CreatedNode);
        }
        // A reused pre-existing event node was not created this pass: keep its
        // own position, don't count it, and don't expose it to a wrapping
        // CollapseNodes call.
        if (!bReusedExisting)
        {
            if (!Inst.Position.bSet)
            {
                OutNodesNeedingLayout.Add(CreatedNode);
            }
            if (OutTopLevelCreated)
            {
                OutTopLevelCreated->Add(CreatedNode);
            }
            ++OutNodesCreated;
        }

        // Apply non-wire literal args as pin defaults. Wire args are skipped —
        // Pass B routes them through FCRIRPinResolver.
        for (const FCRIRArg& Arg : Inst.Args)
        {
            if (Arg.Name.IsEmpty() || IsWireArgName(Arg.Name))
            {
                continue;
            }
            const FString PinPath = FCRIRPinResolver::MakePinPath(CreatedNode, Arg.Name);
            if (!Controller->SetPinDefaultValue(
                    PinPath,
                    Arg.RawText,
                    /*bResizeArrays*/ true,
                    /*bSetupUndoRedo*/ false,
                    /*bMergeUndoAction*/ false,
                    /*bPrintPythonCommand*/ false))
            {
                OutWarnings.Add(FString::Printf(
                    TEXT("CRIR_PIN_DEFAULT_FAILED: %s = %s (line %d)"),
                    *PinPath, *Arg.RawText, Inst.SourceLine));
            }
        }
    }

    // Pass B — wire emission. Wire failures degrade to warnings (matching the
    // forgiving stance AGIR takes on link failures); the surrounding compile
    // still reports success and the round-trip diff surfaces missing edges.
    for (const FCRIRInstruction& Inst : Instructions)
    {
        URigVMNode* const* TargetPtr = LocalIdMap.Find(Inst.LocalId);
        URigVMNode* TargetNode = TargetPtr ? *TargetPtr : nullptr;
        if (!TargetNode)
        {
            continue;
        }
        for (const FCRIRArg& Arg : Inst.Args)
        {
            if (!Arg.bIsLocalRef || !IsWireArgName(Arg.Name))
            {
                continue;
            }
            FString WireError;
            if (!FCRIRPinResolver::ResolveAndConnect(Controller, TargetNode, Arg.Name, Arg, LocalIdMap, WireError))
            {
                OutWarnings.Add(FString::Printf(
                    TEXT("CRIR_WIRE_FAILED: %s on %s (line %d): %s"),
                    *Arg.Name, *Inst.LocalId, Inst.SourceLine, *WireError));
            }
        }
    }

    return FCRIRCompileResult{ true, FString(), FString(), Blueprint->GetPathName(), 0, 0, {} };
}

// Strips a single matching pair of " or ` from the ends of Value, returning
// the inner string. Returns Value unchanged when no matching pair is present.
FString UnquoteIfQuoted(const FString& Value)
{
    if (Value.Len() < 2)
    {
        return Value;
    }
    const TCHAR First = Value[0];
    const TCHAR Last = Value[Value.Len() - 1];
    if ((First == TEXT('"') && Last == TEXT('"'))
        || (First == TEXT('`') && Last == TEXT('`')))
    {
        return Value.Mid(1, Value.Len() - 2);
    }
    return Value;
}

// UEnum lookup keyed on the lowercase-snake-case rendering produced by the
// emitter's FormatEnumLowerSnake. Walks the enum's UEnum entries once, then
// matches by snake-cased name. Used for animation_type, primary_axis, etc.
template <typename TEnum>
bool TryResolveEnumLowerSnake(const FString& Lower, TEnum& OutValue)
{
    const UEnum* EnumObj = StaticEnum<TEnum>();
    if (!EnumObj)
    {
        return false;
    }
    for (int32 Idx = 0; Idx < EnumObj->NumEnums(); ++Idx)
    {
        const FString Name = EnumObj->GetNameStringByIndex(Idx);
        const FString Snake = FIrTextUtils::CamelToSnakeIdentifier(Name);
        if (Snake == Lower)
        {
            OutValue = static_cast<TEnum>(EnumObj->GetValueByIndex(Idx));
            return true;
        }
    }
    return false;
}

bool ParseBoolAttr(const FString& Raw, bool& Out)
{
    const FString Lower = Raw.ToLower();
    if (Lower == TEXT("true") || Lower == TEXT("1"))  { Out = true;  return true; }
    if (Lower == TEXT("false") || Lower == TEXT("0")) { Out = false; return true; }
    return false;
}

// Strip outer [] from a list attribute. Returns empty if not wrapped — caller
// treats this as "no values".
TArray<FString> SplitBracketList(const FString& Raw)
{
    FString Trimmed = Raw;
    Trimmed.TrimStartAndEndInline();
    if (!Trimmed.StartsWith(TEXT("[")) || !Trimmed.EndsWith(TEXT("]")))
    {
        return {};
    }
    const FString Inner = Trimmed.Mid(1, Trimmed.Len() - 2);
    TArray<FString> Parts;
    int32 Depth = 0;
    int32 Start = 0;
    for (int32 Idx = 0; Idx < Inner.Len(); ++Idx)
    {
        const TCHAR C = Inner[Idx];
        if (C == TEXT('(') || C == TEXT('[') || C == TEXT('{')) { ++Depth; }
        else if (C == TEXT(')') || C == TEXT(']') || C == TEXT('}')) { --Depth; }
        else if (C == TEXT(',') && Depth == 0)
        {
            Parts.Add(Inner.Mid(Start, Idx - Start).TrimStartAndEnd());
            Start = Idx + 1;
        }
    }
    if (Start <= Inner.Len())
    {
        Parts.Add(Inner.Mid(Start).TrimStartAndEnd());
    }
    Parts.RemoveAll([](const FString& S) { return S.IsEmpty(); });
    return Parts;
}

// Reserved top-level attribute names that the compiler consumes directly. The
// settings-walker skips these so they don't bleed into FRigControlSettings.
bool IsReservedControlAttr(const FString& Key)
{
    const FString L = Key.ToLower();
    return L == TEXT("type") || L == TEXT("value") || L == TEXT("shape")
        || L == TEXT("location") || L == TEXT("rotation") || L == TEXT("scale");
}

// Walks the element's Attributes map and writes each known sub-block key into
// OutSettings. Unknown keys flow into OutWarnings as CRIR_CONTROL_BAD_SUBBLOCK_KEY.
void ApplyControlSettings(
    const FCRIRElementInstruction& Elem,
    ERigControlType Type,
    FRigControlSettings& OutSettings,
    TArray<FString>& OutWarnings)
{
    for (const TPair<FString, FString>& Pair : Elem.Attributes)
    {
        const FString K = Pair.Key.ToLower();
        const FString& V = Pair.Value;
        if (IsReservedControlAttr(K))
        {
            continue;
        }
        bool bHandled = true;
        if (K == TEXT("animation_type"))
        {
            TryResolveEnumLowerSnake(V.ToLower(), OutSettings.AnimationType);
        }
        else if (K == TEXT("primary_axis"))
        {
            TryResolveEnumLowerSnake(V.ToLower(), OutSettings.PrimaryAxis);
        }
        else if (K == TEXT("display_name"))
        {
            OutSettings.DisplayName = FName(*UnquoteIfQuoted(V));
        }
        else if (K == TEXT("draw_limits"))
        {
            ParseBoolAttr(V, OutSettings.bDrawLimits);
        }
        else if (K == TEXT("shape_visible"))
        {
            ParseBoolAttr(V, OutSettings.bShapeVisible);
        }
        else if (K == TEXT("shape_visibility"))
        {
            TryResolveEnumLowerSnake(V.ToLower(), OutSettings.ShapeVisibility);
        }
        else if (K == TEXT("shape_name"))
        {
            OutSettings.ShapeName = FName(*UnquoteIfQuoted(V));
        }
        else if (K == TEXT("shape_color"))
        {
            TArray<double> RGBA;
            if (FCRIRControlValueParser::TryParseDoubleTuple(V, 4, RGBA))
            {
                OutSettings.ShapeColor = FLinearColor(
                    static_cast<float>(RGBA[0]),
                    static_cast<float>(RGBA[1]),
                    static_cast<float>(RGBA[2]),
                    static_cast<float>(RGBA[3]));
            }
        }
        else if (K == TEXT("is_transient_control"))
        {
            ParseBoolAttr(V, OutSettings.bIsTransientControl);
        }
        else if (K == TEXT("group_with_parent_control"))
        {
            ParseBoolAttr(V, OutSettings.bGroupWithParentControl);
        }
        else if (K == TEXT("restrict_space_switching"))
        {
            ParseBoolAttr(V, OutSettings.bRestrictSpaceSwitching);
        }
        else if (K == TEXT("use_preferred_rotation_order"))
        {
            ParseBoolAttr(V, OutSettings.bUsePreferredRotationOrder);
        }
        else if (K == TEXT("preferred_rotation_order"))
        {
            TryResolveEnumLowerSnake(V.ToLower(), OutSettings.PreferredRotationOrder);
        }
        else if (K == TEXT("filtered_channels"))
        {
            OutSettings.FilteredChannels.Reset();
            for (const FString& Tok : SplitBracketList(V))
            {
                ERigControlTransformChannel Ch;
                if (TryResolveEnumLowerSnake(Tok.ToLower(), Ch))
                {
                    OutSettings.FilteredChannels.Add(Ch);
                }
            }
        }
        else if (K == TEXT("driven_controls"))
        {
            OutSettings.DrivenControls.Reset();
            for (const FString& Tok : SplitBracketList(V))
            {
                OutSettings.DrivenControls.Add(FRigElementKey(FName(*Tok), ERigElementType::Control));
            }
        }
        else if (K == TEXT("limits"))
        {
            OutSettings.LimitEnabled.Reset();
            for (const FString& Tok : SplitBracketList(V))
            {
                TMap<FString, FString> KV;
                FCRIRControlValueParser::TryParseKeyValueTuple(Tok, KV);
                bool bMin = false, bMax = false;
                if (const FString* MinV = KV.Find(TEXT("min"))) { ParseBoolAttr(*MinV, bMin); }
                if (const FString* MaxV = KV.Find(TEXT("max"))) { ParseBoolAttr(*MaxV, bMax); }
                OutSettings.LimitEnabled.Add(FRigControlLimitEnabled(bMin, bMax));
            }
        }
        else if (K == TEXT("min") || K == TEXT("max"))
        {
            FRigControlValue Parsed;
            FString Err;
            if (FCRIRControlValueParser::Parse(V, Type, Parsed, Err))
            {
                if (K == TEXT("min")) { OutSettings.MinimumValue = Parsed; }
                else                  { OutSettings.MaximumValue = Parsed; }
            }
            else
            {
                OutWarnings.Add(FString::Printf(
                    TEXT("CRIR_CONTROL_BAD_SUBBLOCK_KEY: '%s' on '%s' (line %d): %s"),
                    *Pair.Key, *Elem.Name, Elem.SourceLine, *Err));
            }
        }
        else if (K == TEXT("control_enum"))
        {
            // `control_enum` needs no quoting, so V is raw CRIR source text - the dispatch
            // boundary sees only the opaque `text` param and cannot type this as a class ref.
            FString EnumRefusal;
            UEnum* Found = PinWrightGuardedLoad::LoadObjectChecked<UEnum>(V, &EnumRefusal);
            if (Found)
            {
                OutSettings.ControlEnum = Found;
            }
            else if (!EnumRefusal.IsEmpty())
            {
                OutWarnings.Add(FString::Printf(
                    TEXT("CRIR_CONTROL_BAD_SUBBLOCK_KEY: 'control_enum' on '%s' (line %d): %s"),
                    *Elem.Name, Elem.SourceLine, *EnumRefusal));
            }
        }
        else
        {
            bHandled = false;
        }
        if (!bHandled)
        {
            OutWarnings.Add(FString::Printf(
                TEXT("CRIR_CONTROL_BAD_SUBBLOCK_KEY: '%s' on '%s' (line %d)"),
                *Pair.Key, *Elem.Name, Elem.SourceLine));
        }
    }
}

// Resolve the existing element key for a CRIR element name, scanning the same
// element kinds CRIR supports. Returns true (with OutKey set) when a live
// element of that name already exists in the hierarchy. Used by the
// replace-mode clear-first / upsert step so recompiling a rig's own decompiled
// hierarchy reproduces it byte-for-byte instead of auto-renaming to `<name>_2`.
bool FindExistingHierarchyElement(URigHierarchy* Hierarchy, const FName& InName, FRigElementKey& OutKey)
{
    if (!Hierarchy)
    {
        return false;
    }
    const ERigElementType Candidates[] = {
        ERigElementType::Bone,
        ERigElementType::Null,
        ERigElementType::Control,
        ERigElementType::Curve,
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 4, 0)
        ERigElementType::Socket,
#endif
    };
    for (ERigElementType Type : Candidates)
    {
        const FRigElementKey Candidate(InName, Type);
        if (Hierarchy->Find(Candidate) != nullptr)
        {
            OutKey = Candidate;
            return true;
        }
    }
    return false;
}

FCRIRCompileResult CompileRigHierarchyBlock(
    UControlRigBlueprint* Blueprint,
    const FCRIREntryBlock& Block,
    const FCRIRCompileOptions& Options,
    TArray<FString>& OutWarnings)
{
    if (Block.Elements.Num() == 0)
    {
        return FCRIRCompileResult{ true, FString(), FString(), FString(), 0, 0, {} };
    }

    URigHierarchyController* Controller = Blueprint ? Blueprint->GetHierarchyController() : nullptr;
    if (!Controller)
    {
        return FCRIRCompileResult::MakeError(
            TEXT("CRIR_HIERARCHY_CONTROLLER_UNAVAILABLE"),
            TEXT("URigHierarchyController not available on the target Control Rig Blueprint."));
    }
    URigHierarchy* Hierarchy = GetControlRigHierarchy(Blueprint);
    const bool bReplaceMode = (Options.Mode == ECRIRCompileMode::Replace);

    // Track elements we add this pass so a child's `parent="<name>"` resolves
    // to the newly created bone/null/socket without round-tripping the
    // controller's name-correction logic (the controller may rename on
    // collision; we want the resolved key the controller actually returned).
    TMap<FString, FRigElementKey> AddedByName;
    AddedByName.Reserve(Block.Elements.Num());

    auto ResolveParentKey = [&Hierarchy, &AddedByName](
        const FString& ParentName,
        FRigElementKey& OutKey) -> bool
    {
        if (ParentName.IsEmpty())
        {
            OutKey = FRigElementKey();
            return true;
        }
        if (const FRigElementKey* Cached = AddedByName.Find(ParentName))
        {
            OutKey = *Cached;
            return true;
        }
        // Delegate to the shared name-to-element scan (single candidate list +
        // Socket version gate). Curve is among its candidates but is harmless
        // here: a curve cannot be a parent and is never referenced as one.
        return FindExistingHierarchyElement(Hierarchy, FName(*ParentName), OutKey);
    };

    for (const FCRIRElementInstruction& Elem : Block.Elements)
    {
        FRigElementKey ParentKey;
        if (!ResolveParentKey(Elem.Parent, ParentKey))
        {
            return FCRIRCompileResult::MakeError(
                TEXT("CRIR_HIERARCHY_BAD_PARENT"),
                FString::Printf(
                    TEXT("Parent '%s' for element '%s' (line %d) does not resolve to any bone/null/control/socket."),
                    *Elem.Parent, *Elem.Name, Elem.SourceLine));
        }

        // Decode optional transform attributes. Defaults match the
        // `Is*Default` predicates the decompiler uses to elide them.
        FVector Location = FVector::ZeroVector;
        FRotator Rotation = FRotator::ZeroRotator;
        FVector Scale = FVector::OneVector;

        TArray<double> XformComponents;
        if (const FString* LocAttr = Elem.Attributes.Find(TEXT("location")))
        {
            if (!FCRIRControlValueParser::TryParseDoubleTuple(*LocAttr, 3, XformComponents))
            {
                return FCRIRCompileResult::MakeError(
                    TEXT("CRIR_HIERARCHY_BAD_TRANSFORM"),
                    FString::Printf(
                        TEXT("Malformed location='%s' on element '%s' (line %d); expected (x,y,z)."),
                        **LocAttr, *Elem.Name, Elem.SourceLine));
            }
            Location = FVector(XformComponents[0], XformComponents[1], XformComponents[2]);
        }
        if (const FString* RotAttr = Elem.Attributes.Find(TEXT("rotation")))
        {
            if (!FCRIRControlValueParser::TryParseDoubleTuple(*RotAttr, 3, XformComponents))
            {
                return FCRIRCompileResult::MakeError(
                    TEXT("CRIR_HIERARCHY_BAD_TRANSFORM"),
                    FString::Printf(
                        TEXT("Malformed rotation='%s' on element '%s' (line %d); expected (pitch,yaw,roll)."),
                        **RotAttr, *Elem.Name, Elem.SourceLine));
            }
            Rotation = FRotator(XformComponents[0], XformComponents[1], XformComponents[2]);
        }
        if (const FString* ScaleAttr = Elem.Attributes.Find(TEXT("scale")))
        {
            if (!FCRIRControlValueParser::TryParseDoubleTuple(*ScaleAttr, 3, XformComponents))
            {
                return FCRIRCompileResult::MakeError(
                    TEXT("CRIR_HIERARCHY_BAD_TRANSFORM"),
                    FString::Printf(
                        TEXT("Malformed scale='%s' on element '%s' (line %d); expected (x,y,z)."),
                        **ScaleAttr, *Elem.Name, Elem.SourceLine));
            }
            Scale = FVector(XformComponents[0], XformComponents[1], XformComponents[2]);
        }

        const FTransform Xf(Rotation.Quaternion(), Location, Scale);
        const FName ElemFName(*Elem.Name);

        // Replace-mode clear-first / upsert: if an element of this name already
        // exists, remove it before the add so the controller re-uses the exact
        // name instead of GetSafeNewName-ing it to `<name>_2`. This makes
        // `mode=replace` actually replace the hierarchy (mirroring the graph
        // path's ClearGraphForReplace and the BPIR Phase 0 upsert precedent), so
        // recompiling a rig's own decompiled hierarchy reproduces it instead of
        // silently doubling every element. Done per-element rather than wiping
        // the whole hierarchy up front so an extend within the same text (a
        // newly-added child referencing a pre-existing parent) still resolves.
        if (bReplaceMode)
        {
            FRigElementKey ExistingKey;
            if (FindExistingHierarchyElement(Hierarchy, ElemFName, ExistingKey))
            {
                Controller->RemoveElement(ExistingKey, /*bSetupUndo*/ false, /*bPrintPythonCommand*/ false);
            }
        }

        FRigElementKey AddedKey;
        switch (Elem.Kind)
        {
        case ECRIRElementKind::Curve:
        {
            float CurveValue = 0.f;
            if (const FString* ValueAttr = Elem.Attributes.Find(TEXT("value")))
            {
                if (!LexTryParseString(CurveValue, **ValueAttr))
                {
                    return FCRIRCompileResult::MakeError(
                        TEXT("CRIR_CURVE_BAD_VALUE"),
                        FString::Printf(
                            TEXT("curve '%s' (line %d) has malformed value='%s'; expected a float."),
                            *Elem.Name, Elem.SourceLine, **ValueAttr));
                }
            }

            AddedKey = Controller->AddCurve(
                ElemFName,
                CurveValue,
                /*bSetupUndo*/ false,
                /*bPrintPythonCommand*/ false);
            if (Hierarchy && AddedKey.Type == ERigElementType::Curve)
            {
                Hierarchy->SetCurveValue(AddedKey, CurveValue, /*bSetupUndo*/ false);
            }
            break;
        }
        case ECRIRElementKind::Bone:
            AddedKey = Controller->AddBone(
                ElemFName, ParentKey, Xf,
                /*bTransformInGlobal*/ false,
                ERigBoneType::User,
                /*bSetupUndo*/ false,
                /*bPrintPythonCommand*/ false);
            break;
        case ECRIRElementKind::Null:
            AddedKey = Controller->AddNull(
                ElemFName, ParentKey, Xf,
                /*bTransformInGlobal*/ false,
                /*bSetupUndo*/ false,
                /*bPrintPythonCommand*/ false);
            break;
        case ECRIRElementKind::Socket:
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 4, 0)
            // URigHierarchyController::AddSocket (and the Socket element type) arrived in UE 5.4.
            AddedKey = Controller->AddSocket(
                ElemFName, ParentKey, Xf,
                /*bTransformInGlobal*/ false,
                FLinearColor::White,
                FString(),
                /*bSetupUndo*/ false,
                /*bPrintPythonCommand*/ false);
#else
            return FCRIRCompileResult::MakeError(
                TEXT("CRIR_SOCKET_UNSUPPORTED"),
                FString::Printf(TEXT("socket element '%s' (line %d) requires UE 5.4+ (ControlRig sockets)"),
                    *Elem.Name, Elem.SourceLine));
#endif
            break;
        case ECRIRElementKind::Control:
        {
            // type= is required; routes value parsing and dispatches to AddControl.
            const FString* TypeAttr = Elem.Attributes.Find(TEXT("type"));
            if (!TypeAttr)
            {
                return FCRIRCompileResult::MakeError(
                    TEXT("CRIR_CONTROL_BAD_TYPE"),
                    FString::Printf(TEXT("control '%s' (line %d) missing required type= attribute"),
                        *Elem.Name, Elem.SourceLine));
            }
            ERigControlType ControlType;
            if (!FCRIRControlValueParser::TryResolvePrefix(*TypeAttr, ControlType))
            {
                return FCRIRCompileResult::MakeError(
                    TEXT("CRIR_CONTROL_BAD_TYPE"),
                    FString::Printf(TEXT("control '%s' (line %d) has unknown type='%s'"),
                        *Elem.Name, Elem.SourceLine, **TypeAttr));
            }

            FRigControlValue ControlValue;
            if (const FString* ValueAttr = Elem.Attributes.Find(TEXT("value")))
            {
                FString Err;
                ECRIRControlValueParseError Cause;
                if (!FCRIRControlValueParser::ParseWithCause(*ValueAttr, ControlType, ControlValue, Err, Cause))
                {
                    const TCHAR* Code = (Cause == ECRIRControlValueParseError::PrefixTypeMismatch)
                        ? TEXT("CRIR_CONTROL_VALUE_TYPE_MISMATCH")
                        : TEXT("CRIR_CONTROL_BAD_VALUE");
                    return FCRIRCompileResult::MakeError(
                        Code,
                        FString::Printf(TEXT("control '%s' (line %d) value parse failed: %s"),
                            *Elem.Name, Elem.SourceLine, *Err));
                }
            }

            FTransform ShapeXf = FTransform::Identity;
            if (const FString* ShapeAttr = Elem.Attributes.Find(TEXT("shape")))
            {
                FRigControlValue ShapeVal;
                FString Err;
                if (!FCRIRControlValueParser::Parse(*ShapeAttr, ERigControlType::Transform, ShapeVal, Err))
                {
                    return FCRIRCompileResult::MakeError(
                        TEXT("CRIR_CONTROL_BAD_VALUE"),
                        FString::Printf(TEXT("control '%s' (line %d) shape parse failed: %s"),
                            *Elem.Name, Elem.SourceLine, *Err));
                }
                ShapeXf = ShapeVal.Get<FRigControlValue::FTransform_Float>().ToTransform();
            }

            FRigControlSettings Settings;
            Settings.ControlType = ControlType;
            ApplyControlSettings(Elem, ControlType, Settings, OutWarnings);

            AddedKey = Controller->AddControl(
                ElemFName, ParentKey, Settings, ControlValue,
                Xf, ShapeXf,
                /*bSetupUndo*/ false,
                /*bPrintPythonCommand*/ false);
#if UE_VERSION_OLDER_THAN(5, 6, 0)
            // 5.3-5.5: for a Rotator control, AddControl stores the value into PreferredEulerAngles
            // but then immediately recomputes PreferredEulerAngles from the (identity) pose transform,
            // discarding the authored rotation. (Fixed in 5.6, where SetControlValue also writes the
            // pose transform so the recompute round-trips.) Re-apply the value after AddControl so the
            // preferred angles hold the intended rotation, matching the 5.6+ result.
            if (ControlType == ERigControlType::Rotator
                && Elem.Attributes.Contains(TEXT("value"))
                && AddedKey.Type != ERigElementType::None
                && Hierarchy)
            {
                if (FRigControlElement* ControlElement = Hierarchy->Find<FRigControlElement>(AddedKey))
                {
                    // bFixEulerFlips=true selects the SetControlValue arm that writes the rotator
                    // straight into PreferredEulerAngles (SetRotator(GetRotatorFromControlValue(...))),
                    // bypassing the lossy transform→quaternion→euler recompute AddControl performs.
                    // The key-based overload defaults bFixEulerFlips=false (transform-only), which
                    // leaves the angles at AddControl's distorted recompute, so use the element overload.
                    Hierarchy->SetControlValue(ControlElement, ControlValue, ERigControlValueType::Initial,
                        /*bSetupUndo*/ false, /*bForce*/ false, /*bPrintPythonCommands*/ false, /*bFixEulerFlips*/ true);
                    Hierarchy->SetControlValue(ControlElement, ControlValue, ERigControlValueType::Current,
                        /*bSetupUndo*/ false, /*bForce*/ false, /*bPrintPythonCommands*/ false, /*bFixEulerFlips*/ true);
                }
            }
#endif
            break;
        }
        default:
            OutWarnings.Add(FString::Printf(
                TEXT("CRIR_HIERARCHY_UNSUPPORTED_KIND: element '%s' (line %d)"),
                *Elem.Name, Elem.SourceLine));
            continue;
        }

        if (AddedKey.Type == ERigElementType::None)
        {
            OutWarnings.Add(FString::Printf(
                TEXT("CRIR_HIERARCHY_ADD_FAILED: element '%s' (line %d) was not added"),
                *Elem.Name, Elem.SourceLine));
            continue;
        }

        // Surface a silent collision: if the controller's GetSafeNewName had to
        // rename the element (the returned key name differs from the requested
        // name), an element of this name already existed and was auto-renamed
        // (e.g. `<name>_2`) rather than upserted. In replace mode the clear-first
        // step above should have prevented this, so reaching here means a genuine
        // duplicate within the same text; in extend mode it is the documented
        // append behaviour. Either way it must not be silent — mirror the BPIR
        // duplicate-entry warning scan so a doubling becomes visible to callers.
        if (AddedKey.Name != ElemFName)
        {
            OutWarnings.Add(FString::Printf(
                TEXT("CRIR_HIERARCHY_DUPLICATE_ELEMENT: element '%s' (line %d) collided with an existing element and was auto-renamed to '%s'%s"),
                *Elem.Name, Elem.SourceLine, *AddedKey.Name.ToString(),
                bReplaceMode ? TEXT(" (duplicate name within the compiled text)") : TEXT(" (extend mode appended a same-named element)")));
        }

        AddedByName.Add(Elem.Name, AddedKey);
    }

    return FCRIRCompileResult{ true, FString(), FString(), FString(), 0, 0, {} };
}
} // namespace

FCRIRCompileResult FCRIRCompiler::Compile(FStringView Text, const FCRIRCompileOptions& Options)
{
    TArray<FCRIREntryBlock> Blocks;
    TArray<FCRIRParseError> Errors;
    if (!FCRIRParser::Parse(Text, Blocks, Errors, /*bSkipReferenceValidation*/ false))
    {
        TArray<FString> Lines;
        Lines.Reserve(Errors.Num());
        for (const FCRIRParseError& Err : Errors)
        {
            Lines.Add(FString::Printf(TEXT("line %d: %s"), Err.Line, *Err.Message));
        }
        const FString Aggregate = Lines.Num() > 0 ? FString::Join(Lines, TEXT("; ")) : TEXT("Failed to parse CRIR text.");
        const FString Code = (Errors.Num() > 0 && !Errors[0].Code.IsEmpty()) ? Errors[0].Code : FString(TEXT("CRIR_PARSE_ERROR"));
        return FCRIRCompileResult::MakeError(Code, Aggregate);
    }

    if (Options.TargetAssetPath.IsEmpty())
    {
        return FCRIRCompileResult::MakeError(
            TEXT("CRIR_ASSET_NOT_FOUND"),
            TEXT("CRIR compile requires Options.TargetAssetPath."));
    }

    // Guarded as defence in depth: the wire param is typed `path` at the dispatch boundary, but
    // this compiler is also driven from tests and from internally-composed strings.
    FString TargetRefusal;
    UControlRigBlueprint* Blueprint = PinWrightGuardedLoad::LoadObjectChecked<UControlRigBlueprint>(
        Options.TargetAssetPath, &TargetRefusal);
    if (!Blueprint)
    {
        return FCRIRCompileResult::MakeError(
            TEXT("CRIR_ASSET_NOT_FOUND"),
            TargetRefusal.IsEmpty()
                ? FString::Printf(TEXT("Could not load UControlRigBlueprint at %s."), *Options.TargetAssetPath)
                : TargetRefusal);
    }

    UPackage* const Package = Blueprint->GetOutermost();
    const bool bPackageWasDirty = Package && Package->IsDirty();

    FScopedTransaction Transaction(LOCTEXT("CompileCRIR", "Compile CRIR"));
    const FControlRigTransactionalSnapshot Snapshot = PrepareControlRigTransactionalSnapshot(Blueprint);

    auto RollbackFailure = [Blueprint, &Transaction, &Snapshot, Package, bPackageWasDirty](FCRIRCompileResult Failure)
    {
        FCRIRRollbackScope RollbackScope(Blueprint, Snapshot);
        RemoveNodesCreatedAfterSnapshot(Blueprint, Snapshot);
        PinWrightTransactionUtils::ApplyAndCancelTransaction(Transaction);
        RollbackScope.Finish(Snapshot);
        if (Package && !bPackageWasDirty)
        {
            Package->SetDirtyFlag(false);
        }
        return Failure;
    };

    FCRIRCompileResult Result;
    Result.AssetPath = Blueprint->GetPathName();

    // Per-block layout state — keyed by the URigVMGraph the block targeted so
    // the layout pass routes nodes through the correct controller.
    struct FLayoutWork
    {
        URigVMGraph* Graph = nullptr;
        URigVMController* Controller = nullptr;
        TSet<URigVMNode*> Nodes;
    };
    TArray<FLayoutWork> LayoutWork;

    // Two-pass block ordering: hierarchy and function-library blocks compile
    // first so that any `function_ref <Name>` references emitted from rig_graph
    // blocks resolve against an already-populated library. The decompiler
    // emits rig_function blocks AFTER rig_graphs (for deterministic textual
    // output), so a single in-order pass would fail every round-trip that uses
    // local functions. Reordering at compile time also lets user-authored CRIR
    // declare functions in any position.
    auto CompileHierarchyOrFunction = [&](const FCRIREntryBlock& Block) -> FCRIRCompileResult
    {
        if (Block.Kind == ECRIREntryKind::RigHierarchy)
        {
            FCRIRCompileResult HierResult = CompileRigHierarchyBlock(Blueprint, Block, Options, Result.Warnings);
            if (HierResult.bSuccess)
            {
                ++Result.BlocksCompiled;
            }
            return HierResult;
        }
        // RigFunction
        int32 FnNodesCreated = 0;
        TSet<URigVMNode*> FnNodesNeedingLayout;
        FCRIRCompileResult FnResult = CompileRigFunctionBlock(
            Blueprint, Block, Options, FnNodesCreated, FnNodesNeedingLayout, Result.Warnings);
        if (FnResult.bSuccess)
        {
            Result.NodesCreated += FnNodesCreated;
            ++Result.BlocksCompiled;
        }
        return FnResult;
    };

    // Pass 1: hierarchy + rig_function blocks (in original textual order).
    for (const FCRIREntryBlock& Block : Blocks)
    {
        if (Block.Kind != ECRIREntryKind::RigHierarchy && Block.Kind != ECRIREntryKind::RigFunction)
        {
            continue;
        }
        FCRIRCompileResult Pass1Result = CompileHierarchyOrFunction(Block);
        if (!Pass1Result.bSuccess)
        {
            return RollbackFailure(MoveTemp(Pass1Result));
        }
    }

    // Pass 2: rig_graph blocks (in original textual order). function_ref
    // resolution against the local library now succeeds.
    for (const FCRIREntryBlock& Block : Blocks)
    {
        if (Block.Kind == ECRIREntryKind::RigHierarchy || Block.Kind == ECRIREntryKind::RigFunction)
        {
            continue;
        }

        if (Block.Kind != ECRIREntryKind::RigGraph)
        {
            return RollbackFailure(FCRIRCompileResult::MakeError(
                TEXT("CRIR_UNSUPPORTED_BLOCK"),
                FString::Printf(TEXT("Unsupported CRIR block kind at line %d."), Block.SourceLine)));
        }

        int32 BlockNodesCreated = 0;
        TSet<URigVMNode*> BlockNodesNeedingLayout;
        URigVMGraph* BlockGraph = nullptr;
        URigVMController* BlockController = nullptr;
        FCRIRCompileResult BlockResult = CompileRigGraphBlock(
            Blueprint, Block, Options, BlockNodesCreated, BlockNodesNeedingLayout,
            BlockGraph, BlockController, Result.Warnings);
        if (!BlockResult.bSuccess)
        {
            return RollbackFailure(MoveTemp(BlockResult));
        }

        Result.NodesCreated += BlockNodesCreated;
        ++Result.BlocksCompiled;

        if (Options.bRunLayout && BlockGraph && BlockController && BlockNodesNeedingLayout.Num() > 0)
        {
            FLayoutWork Work;
            Work.Graph = BlockGraph;
            Work.Controller = BlockController;
            Work.Nodes = MoveTemp(BlockNodesNeedingLayout);
            LayoutWork.Add(MoveTemp(Work));
        }
    }

    if (Options.bRunLayout)
    {
        for (const FLayoutWork& Work : LayoutWork)
        {
            FCRIRLayoutEngine::RunLayout(Work.Graph, Work.Nodes, Work.Controller);
        }
    }

    if (Options.bSave)
    {
        McpSafeAssetSave(Blueprint);
    }

    Result.bSuccess = true;
    return Result;
}

#undef LOCTEXT_NAMESPACE
