// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/ParamSpec.h"

#if defined(__has_include) && __has_include("PCGGraph.h")

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "ScopedTransaction.h"
#include "UObject/Class.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

#include "PCGGraph.h"
#include "PCGNode.h"
#include "PCGSettings.h"
#include "PCGPin.h"
#include "PCGEdge.h"
#include "Handlers/PCG/PCGHandlerHelpers.h"

namespace PinWrightPCGAuthoring
{
    // FindPinByLabel stays local: only pcg.connect_pins uses it.
    const UPCGPin* FindPinByLabel(const TArray<TObjectPtr<UPCGPin>>& Pins, const FName& Label)
    {
        for (const UPCGPin* Pin : Pins)
        {
            if (Pin && Pin->Properties.Label == Label)
            {
                return Pin;
            }
        }
        return nullptr;
    }

    // Copy an input pin's upstream endpoints out by VALUE — (source node id, source pin
    // label) — rather than holding UPCGEdge pointers. UPCGPin::BreakAllIncompatibleEdges
    // unlinks and drops those edge objects, so a pointer snapshot taken before a write
    // cannot name what the write destroyed; a value snapshot can, and that naming is the
    // only thing that makes a displaced connection re-creatable by the caller.
    TArray<TPair<FString, FString>> SnapshotIncomingEdgeSources(const UPCGPin* InputPin)
    {
        TArray<TPair<FString, FString>> Sources;
        if (!InputPin)
        {
            return Sources;
        }

        for (const UPCGEdge* Edge : InputPin->Edges)
        {
            // Per PCGEdge.h, Edge::InputPin is the upstream (source output) pin.
            if (!Edge || !Edge->InputPin || !Edge->InputPin->Node)
            {
                continue;
            }
            Sources.Emplace(Edge->InputPin->Node->GetName(),
                Edge->InputPin->Properties.Label.ToString());
        }
        return Sources;
    }
}

REGISTER_RPC_HANDLER("pcg.add_node", "pcg",
    "Append a node to a UPCGGraph by settings-class path. x and y are required (no auto-layout).",
    RPC_PARAMS(
        RPC_PARAM_REQ("graphPath", "path", "PCG graph asset path."),
        RPC_PARAM_REQ("nodeClass", "classref", "UPCGSettings subclass path, e.g. /Script/PCG.PCGCreatePointsSettings."),
        RPC_PARAM_REQ("x", "integer", "Node X position."),
        RPC_PARAM_REQ("y", "integer", "Node Y position.")
    ))
{
    using namespace PinWrightPCG;
    using namespace PinWrightPCGAuthoring;

    FString GraphPath;
    if (!Ctx.RequireString(TEXT("graphPath"), GraphPath)) return true;

    FString NodeClassPath;
    if (!Ctx.RequireString(TEXT("nodeClass"), NodeClassPath)) return true;

    int32 X = 0;
    if (!Ctx.RequireInt(TEXT("x"), X)) return true;
    int32 Y = 0;
    if (!Ctx.RequireInt(TEXT("y"), Y)) return true;

    UPCGGraph* Graph = LoadGraphOrError(Ctx, GraphPath);
    if (!Graph) return true;

    UClass* SettingsClass = FindObject<UClass>(nullptr, *NodeClassPath);
    if (!SettingsClass)
    {
        SettingsClass = LoadClass<UPCGSettings>(nullptr, *NodeClassPath);
    }
    if (!SettingsClass && SendPluginDisabledForScriptPath(Ctx, NodeClassPath))
    {
        // A /Script/ path that does not resolve is either a typo or a disabled engine
        // plugin whose module was never loaded. Name the plugin when it is the latter,
        // rather than sending the caller hunting for a typo in a path that is correct.
        return true;
    }
    if (!SettingsClass || !SettingsClass->IsChildOf(UPCGSettings::StaticClass()))
    {
        Ctx.SendError(TEXT("CLASS_NOT_FOUND"),
            FString::Printf(TEXT("Could not resolve UPCGSettings subclass: %s"), *NodeClassPath));
        return true;
    }

    UPCGSettings* DefaultSettings = nullptr;
    UPCGNode* Node = Graph->AddNodeOfType(TSubclassOf<UPCGSettings>(SettingsClass), DefaultSettings);
    if (!Node)
    {
        Ctx.SendError(TEXT("ADD_NODE_FAILED"),
            FString::Printf(TEXT("AddNodeOfType returned null for class: %s"), *NodeClassPath));
        return true;
    }

    Node->SetNodePosition(X, Y);
    Graph->MarkPackageDirty();

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("nodeId"), Node->GetName());
    Result->SetStringField(TEXT("nodeClass"), NodeClassPath);
    Result->SetNumberField(TEXT("x"), X);
    Result->SetNumberField(TEXT("y"), Y);
    Ctx.SendSuccess(Result);
    return true;
}

REGISTER_RPC_HANDLER("pcg.connect_pins", "pcg",
    "Wire a source output pin to a target input pin in a UPCGGraph. `connected` is measured "
    "(the target pin's edges are read back after the write), not echoed; fromNode/fromPin/toNode/"
    "toPin echo the request. A target pin that does not accept multiple connections DESTROYS what "
    "was already wired to it — every Procedural Vegetation node's In pin is one — so "
    "`replacedExistingEdge` and `replacedEdges[]` name every edge this call destroyed "
    "(fromNode/fromPin/toNode/toPin each, enough to restore it), and `warnings[]` carries one line "
    "per loss. The write runs in an undo transaction, so the displaced edge is also recoverable "
    "with editor undo.",
    RPC_PARAMS(
        RPC_PARAM_REQ("graphPath", "path", "PCG graph asset path."),
        RPC_PARAM_REQ("fromNode", "string", "Source node id (GetName())."),
        RPC_PARAM_REQ("fromPin", "string", "Source output pin label."),
        RPC_PARAM_REQ("toNode", "string", "Destination node id (GetName())."),
        RPC_PARAM_REQ("toPin", "string", "Destination input pin label.")
    ))
{
    using namespace PinWrightPCG;
    using namespace PinWrightPCGAuthoring;

    FString GraphPath;
    if (!Ctx.RequireString(TEXT("graphPath"), GraphPath)) return true;

    FString FromNodeName;
    if (!Ctx.RequireString(TEXT("fromNode"), FromNodeName)) return true;
    FString FromPinName;
    if (!Ctx.RequireString(TEXT("fromPin"), FromPinName)) return true;
    FString ToNodeName;
    if (!Ctx.RequireString(TEXT("toNode"), ToNodeName)) return true;
    FString ToPinName;
    if (!Ctx.RequireString(TEXT("toPin"), ToPinName)) return true;

    UPCGGraph* Graph = LoadGraphOrError(Ctx, GraphPath);
    if (!Graph) return true;

    UPCGNode* FromNode = FindNodeByNameIncludingImplicit(Graph, FromNodeName);
    UPCGNode* ToNode = FindNodeByNameIncludingImplicit(Graph, ToNodeName);
    if (!FromNode || !ToNode)
    {
        Ctx.SendError(TEXT("NODE_NOT_FOUND"),
            FString::Printf(TEXT("Could not find node(s): from='%s' to='%s'"),
                *FromNodeName, *ToNodeName));
        return true;
    }

    const FName FromPinFName(*FromPinName);
    const FName ToPinFName(*ToPinName);
    const UPCGPin* ToPinBefore = FindPinByLabel(ToNode->GetInputPins(), ToPinFName);
    if (!FindPinByLabel(FromNode->GetOutputPins(), FromPinFName) || !ToPinBefore)
    {
        Ctx.SendError(TEXT("PIN_NOT_FOUND"),
            FString::Printf(TEXT("Pin missing: fromPin='%s' (output on '%s') or toPin='%s' (input on '%s')"),
                *FromPinName, *FromNodeName, *ToPinName, *ToNodeName));
        return true;
    }

    // Wiring a target pin that does not allow multiple connections makes the engine break
    // whatever was already on it (UPCGGraph::AddLabeledEdge -> UPCGPin::BreakAllIncompatibleEdges),
    // and the new edge is always the survivor because it is appended first. Snapshot the
    // pin's upstream endpoints before the write so the response can name what the write cost.
    const TArray<TPair<FString, FString>> IncomingBefore = SnapshotIncomingEdgeSources(ToPinBefore);

    // AddLabeledEdge, not the AddEdge wrapper: the wrapper discards the engine's
    // "the To pin removed other edges" bool. Kept as a cross-check against the measurement
    // below rather than published directly — it is also true for a type-incompatibility
    // break, and false on early-out paths this handler has already excluded.
    //
    // Undo: BreakAllIncompatibleEdges calls Modify() on both pins, but Modify() only records
    // into an ACTIVE transaction. Without this scope the displaced edge is unrecoverable.
    bool bEngineReportedBrokenEdges = false;
    {
        FScopedTransaction Transaction(NSLOCTEXT("PinWright", "PcgConnectPins", "PCG Connect Pins"));
        Graph->Modify();
        bEngineReportedBrokenEdges = Graph->AddLabeledEdge(FromNode, FromPinFName, ToNode, ToPinFName);
    }
    Graph->MarkPackageDirty();

    // Measured, not assumed: re-resolve the target pin and read its edges back.
    const UPCGPin* ToPinAfter = FindPinByLabel(ToNode->GetInputPins(), ToPinFName);
    const TArray<TPair<FString, FString>> IncomingAfter = SnapshotIncomingEdgeSources(ToPinAfter);

    const bool bConnected = IncomingAfter.Contains(TPair<FString, FString>(FromNodeName, FromPinName));

    TArray<TPair<FString, FString>> ReplacedSources;
    for (const TPair<FString, FString>& Before : IncomingBefore)
    {
        if (!IncomingAfter.Contains(Before))
        {
            ReplacedSources.Add(Before);
        }
    }

    TArray<TSharedPtr<FJsonValue>> ReplacedValues;
    TArray<TSharedPtr<FJsonValue>> Warnings;
    for (const TPair<FString, FString>& Replaced : ReplacedSources)
    {
        // Every field a caller needs to restore the connection with a second connect_pins.
        TSharedPtr<FJsonObject> ReplacedObj = MakeShared<FJsonObject>();
        ReplacedObj->SetStringField(TEXT("fromNode"), Replaced.Key);
        ReplacedObj->SetStringField(TEXT("fromPin"), Replaced.Value);
        ReplacedObj->SetStringField(TEXT("toNode"), ToNodeName);
        ReplacedObj->SetStringField(TEXT("toPin"), ToPinName);
        ReplacedValues.Add(MakeShared<FJsonValueObject>(ReplacedObj));

        Warnings.Add(MakeShared<FJsonValueString>(FString::Printf(
            TEXT("Connecting '%s'.'%s' -> '%s'.'%s' DESTROYED the existing edge '%s'.'%s' -> '%s'.'%s'; "
                 "'%s' does not accept multiple connections. Restore it with connect_pins if it was wanted, "
                 "or undo this call in the editor."),
            *FromNodeName, *FromPinName, *ToNodeName, *ToPinName,
            *Replaced.Key, *Replaced.Value, *ToNodeName, *ToPinName,
            *ToPinName)));
    }

    if (!bConnected)
    {
        Warnings.Add(MakeShared<FJsonValueString>(FString::Printf(
            TEXT("Read-back found no edge '%s'.'%s' -> '%s'.'%s' after the write; the connection did not take."),
            *FromNodeName, *FromPinName, *ToNodeName, *ToPinName)));
    }

    if (bEngineReportedBrokenEdges != (ReplacedSources.Num() > 0))
    {
        // The two disagree only if the read-back and UPCGGraph::AddLabeledEdge saw different
        // graphs; say so rather than letting the measurement pass as uncontested.
        Warnings.Add(MakeShared<FJsonValueString>(FString::Printf(
            TEXT("Disagreement: AddLabeledEdge reported brokeOtherEdges=%s but the read-back found %d replaced edge(s). "
                 "replacedEdges is the measured value."),
            bEngineReportedBrokenEdges ? TEXT("true") : TEXT("false"), ReplacedSources.Num())));
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    // Measured: read back off the target pin after the write, not a literal.
    Result->SetBoolField(TEXT("connected"), bConnected);
    Result->SetBoolField(TEXT("replacedExistingEdge"), ReplacedSources.Num() > 0);
    // Measured, and empty when nothing was displaced. Each entry is a full edge spec.
    Result->SetArrayField(TEXT("replacedEdges"), ReplacedValues);
    // The request, echoed — these four are the caller's own input, not a read-back.
    Result->SetStringField(TEXT("fromNode"), FromNodeName);
    Result->SetStringField(TEXT("fromPin"), FromPinName);
    Result->SetStringField(TEXT("toNode"), ToNodeName);
    Result->SetStringField(TEXT("toPin"), ToPinName);
    if (Warnings.Num() > 0)
    {
        Result->SetArrayField(TEXT("warnings"), Warnings);
    }
    Ctx.SendSuccess(Result);
    return true;
}

REGISTER_RPC_HANDLER("pcg.remove_node", "pcg",
    "Delete a node from a UPCGGraph (and its incident edges). Refuses to delete input/output.",
    RPC_PARAMS(
        RPC_PARAM_REQ("graphPath", "path", "PCG graph asset path."),
        RPC_PARAM_REQ("nodeId", "string", "Node id (GetName()).")
    ))
{
    using namespace PinWrightPCG;
    using namespace PinWrightPCGAuthoring;

    FString GraphPath;
    if (!Ctx.RequireString(TEXT("graphPath"), GraphPath)) return true;

    FString NodeId;
    if (!Ctx.RequireString(TEXT("nodeId"), NodeId)) return true;

    UPCGGraph* Graph = LoadGraphOrError(Ctx, GraphPath);
    if (!Graph) return true;

    UPCGNode* Node = FindNodeByNameIncludingImplicit(Graph, NodeId);
    if (!Node)
    {
        Ctx.SendError(TEXT("NODE_NOT_FOUND"),
            FString::Printf(TEXT("Could not find node: %s"), *NodeId));
        return true;
    }

    if (Node == Graph->GetInputNode() || Node == Graph->GetOutputNode())
    {
        Ctx.SendError(TEXT("IMMUTABLE_NODE"),
            FString::Printf(TEXT("Cannot delete implicit input/output node: %s"), *NodeId));
        return true;
    }

    Graph->RemoveNode(Node);
    Graph->MarkPackageDirty();

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("removed"), NodeId);
    Ctx.SendSuccess(Result);
    return true;
}

#endif // __has_include("PCGGraph.h")
