// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/ParamSpec.h"

#if defined(__has_include) && __has_include("PCGGraph.h")

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "UObject/UObjectGlobals.h"

#include "PCGGraph.h"
#include "PCGNode.h"
#include "PCGSettings.h"
#include "PCGPin.h"
#include "PCGEdge.h"
#include "Handlers/PCG/PCGHandlerHelpers.h"

REGISTER_RPC_HANDLER("pcg.inspect", "pcg",
    "Read-side dump of a UPCGGraph: nodes (with class/position/pin labels) and edges.",
    RPC_PARAMS(
        RPC_PARAM_REQ("graphPath", "path", "PCG graph asset path.")
    ))
{
    FString GraphPath;
    if (!Ctx.RequireString(TEXT("graphPath"), GraphPath)) return true;

    UPCGGraph* Graph = PinWrightPCG::LoadGraphOrError(Ctx, GraphPath);
    if (!Graph) return true;

    auto AppendPinLabels = [](const TArray<TObjectPtr<UPCGPin>>& Pins) -> TArray<TSharedPtr<FJsonValue>>
    {
        TArray<TSharedPtr<FJsonValue>> Out;
        Out.Reserve(Pins.Num());
        for (const UPCGPin* Pin : Pins)
        {
            if (!Pin) continue;
            Out.Add(MakeShared<FJsonValueString>(Pin->Properties.Label.ToString()));
        }
        return Out;
    };

    auto NodeToJson = [&AppendPinLabels](const UPCGNode* Node) -> TSharedPtr<FJsonObject>
    {
        TSharedPtr<FJsonObject> NodeObj = MakeShared<FJsonObject>();
        NodeObj->SetStringField(TEXT("id"), Node->GetName());
        if (const UPCGSettings* Settings = Node->GetSettings())
        {
            NodeObj->SetStringField(TEXT("settingsClass"), Settings->GetClass()->GetPathName());
        }
        else
        {
            NodeObj->SetStringField(TEXT("settingsClass"), FString());
        }
        int32 PX = 0, PY = 0;
        Node->GetNodePosition(PX, PY);
        NodeObj->SetNumberField(TEXT("x"), PX);
        NodeObj->SetNumberField(TEXT("y"), PY);
        NodeObj->SetArrayField(TEXT("inputPins"), AppendPinLabels(Node->GetInputPins()));
        NodeObj->SetArrayField(TEXT("outputPins"), AppendPinLabels(Node->GetOutputPins()));
        return NodeObj;
    };

    TArray<TSharedPtr<FJsonValue>> NodeValues;
    TArray<TSharedPtr<FJsonValue>> EdgeValues;

    // Single-pass traversal: emit node JSON and walk that node's outgoing edges
    // in the same iteration. The implicit input/output nodes are visited first
    // (so callers can reference them) and the input node's outgoing edges fold
    // into the same emit step.
    auto AppendNodeEdges = [&EdgeValues](const UPCGNode* Node)
    {
        for (const UPCGPin* OutputPin : Node->GetOutputPins())
        {
            if (!OutputPin) continue;
            for (const UPCGEdge* Edge : OutputPin->Edges)
            {
                if (!Edge || !Edge->InputPin || !Edge->OutputPin) continue;
                // Per PCGEdge.h: Edge::InputPin is the upstream (source/output)
                // pin; Edge::OutputPin is the downstream (dest/input) pin. Data
                // flows source-output -> dest-input, so from/fromPin come from
                // InputPin and to/toPin come from OutputPin.
                const UPCGNode* SrcNode = Edge->InputPin->Node;
                const UPCGNode* DstNode = Edge->OutputPin->Node;
                if (!SrcNode || !DstNode) continue;
                TSharedPtr<FJsonObject> EdgeObj = MakeShared<FJsonObject>();
                EdgeObj->SetStringField(TEXT("from"), SrcNode->GetName());
                EdgeObj->SetStringField(TEXT("fromPin"), Edge->InputPin->Properties.Label.ToString());
                EdgeObj->SetStringField(TEXT("to"), DstNode->GetName());
                EdgeObj->SetStringField(TEXT("toPin"), Edge->OutputPin->Properties.Label.ToString());
                EdgeValues.Add(MakeShared<FJsonValueObject>(EdgeObj));
            }
        }
    };

    if (UPCGNode* InputNode = Graph->GetInputNode())
    {
        NodeValues.Add(MakeShared<FJsonValueObject>(NodeToJson(InputNode)));
        AppendNodeEdges(InputNode);
    }
    if (UPCGNode* OutputNode = Graph->GetOutputNode())
    {
        // Output node has no outgoing edges to walk, but emit its node JSON.
        NodeValues.Add(MakeShared<FJsonValueObject>(NodeToJson(OutputNode)));
    }
    for (const UPCGNode* Node : Graph->GetNodes())
    {
        if (!Node) continue;
        NodeValues.Add(MakeShared<FJsonValueObject>(NodeToJson(Node)));
        AppendNodeEdges(Node);
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("graphPath"), Graph->GetPathName());
    Result->SetArrayField(TEXT("nodes"), NodeValues);
    Result->SetArrayField(TEXT("edges"), EdgeValues);
    Ctx.SendSuccess(Result);
    return true;
}

#endif // __has_include("PCGGraph.h")
