// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "PCGIR/PCGIRDecompiler.h"

#if __has_include("PCGGraph.h")

#include "IrCore/IrTextUtils.h"
#include "PCGGraph.h"
#include "PCGNode.h"
#include "PCGPin.h"
#include "PCGEdge.h"
#include "PCGSettings.h"
#include "PCGSubgraph.h"
#include "UObject/Class.h"
#include "UObject/UnrealType.h"

namespace
{
    // Reserved local names for graph terminal nodes. Real per-node names use
    // sequential N1/N2/... to keep them grammar-uniform and stable across
    // edits.
    const TCHAR* kInputNodeName = TEXT("N_Input");
    const TCHAR* kOutputNodeName = TEXT("N_Output");

    FString FormatClassPath(const UClass* Class)
    {
        return Class ? Class->GetPathName() : FString(TEXT("None"));
    }

    // The MGIR/AGIR rejection callbacks reject struct types that carry their
    // own connection semantics (FExpressionInput, FPoseLink) so the reflected
    // walker doesn't redundantly dump connection state. PCG carries no such
    // input-struct concept on UPCGSettings — every settings property is a
    // plain reflected field — so the struct rejector is a no-op.
    bool RejectStructForPCG(const FStructProperty* /*StructProperty*/)
    {
        return false;
    }

    // Settings property names that are emitted elsewhere (e.g. by the
    // explicit subgraph block) or that don't belong in IR text (PCG's
    // bDebug/bEnabled UI toggles are layout-only — they don't change
    // graph semantics for the purpose of decompile diff).
    bool RejectPropertyNameForPCG(FName PropertyName)
    {
        static const FName SubgraphInstanceName(TEXT("SubgraphInstance"));
        static const FName SubgraphOverrideName(TEXT("SubgraphOverride"));
        return PropertyName == SubgraphInstanceName
            || PropertyName == SubgraphOverrideName;
    }

    void AppendPCGBodyLine(TArray<FString>& Lines, const FString& Line)
    {
        Lines.Add(TEXT("    ") + Line);
    }

    FString FormatPCGFieldBlock(const TArray<FString>& Fields)
    {
        if (Fields.Num() == 0)
        {
            return FString();
        }
        return TEXT(" {\n        ") + FString::Join(Fields, TEXT("\n        ")) + TEXT("\n    }");
    }

    FString FormatPinLabel(FName Label)
    {
        // Backtick-wrap labels that contain non-identifier characters; bare
        // identifiers pass through unchanged.
        return FIrTextUtils::FormatNameToken(Label.ToString());
    }

    void AppendSettingsProperties(
        UPCGSettings* Settings,
        TArray<FString>& OutFields)
    {
        if (!Settings)
        {
            return;
        }

        UClass* SettingsClass = Settings->GetClass();
        UObject* DefaultSettings = SettingsClass->GetDefaultObject();
        if (!DefaultSettings)
        {
            return;
        }

        FReflectedFieldEmitOptions Options;
        Options.FieldSeparator = TEXT(" = ");
        Options.bEmitArraysAsBracketList = true;

        FIrTextUtils::AppendReflectedFields(
            SettingsClass,
            Settings,
            DefaultSettings,
            Settings,
            /*ExplicitProperties*/ TSet<FName>(),
            &RejectStructForPCG,
            &RejectPropertyNameForPCG,
            Options,
            OutFields);
    }

    // PCG positions are stored as int32 via UPCGNode::PositionX/PositionY.
    // The plugin is editor-only so the data is always present; nodes that
    // haven't been positioned yet read as (0, 0).
    FString FormatNodePosition(UPCGNode* Node)
    {
        if (!Node)
        {
            return FIrTextUtils::FormatPositionSuffix(0, 0);
        }
        return FIrTextUtils::FormatPositionSuffix(Node->PositionX, Node->PositionY);
    }

    void EmitNode(
        UPCGNode* Node,
        const FString& NodeName,
        TArray<FString>& Lines,
        TArray<FString>& Warnings)
    {
        if (!Node)
        {
            return;
        }

        UPCGSettings* Settings = Node->GetSettings();
        const FString ClassPath = FormatClassPath(Settings ? Settings->GetClass() : nullptr);

        TArray<FString> Fields;

        // Special subgraph handling: emit the referenced graph's path as a
        // bare property and do not recurse into the child graph (matches the
        // MGIR composite-flatten policy from material.mgir).
        if (UPCGBaseSubgraphSettings* SubgraphSettings = Cast<UPCGBaseSubgraphSettings>(Settings))
        {
            if (UPCGGraph* Subgraph = SubgraphSettings->GetSubgraph())
            {
                Fields.Add(FString::Printf(
                    TEXT("subgraph = %s"),
                    *FIrTextUtils::Quote(Subgraph->GetPathName())));
            }
            else
            {
                Warnings.Add(FString::Printf(
                    TEXT("Subgraph settings node '%s' has a null Subgraph reference; emitting flat node with no subgraph property."),
                    *NodeName));
            }
        }

        AppendSettingsProperties(Settings, Fields);

        const FString Header = FString::Printf(
            TEXT("node %s = %s%s%s"),
            *NodeName,
            *FIrTextUtils::FormatNameToken(ClassPath),
            *FormatNodePosition(Node),
            *FormatPCGFieldBlock(Fields));
        AppendPCGBodyLine(Lines, Header);
    }

    void EmitEdgesForNode(
        UPCGNode* Node,
        const TMap<UPCGNode*, FString>& NodeNames,
        TArray<FString>& Lines,
        TArray<FString>& Warnings)
    {
        if (!Node)
        {
            return;
        }

        // Walk every output pin's edges so each edge is reported exactly
        // once. UPCGEdge::InputPin is the upstream (source/output) pin;
        // UPCGEdge::OutputPin is the downstream (dest/input) pin.
        for (UPCGPin* OutputPin : Node->GetOutputPins())
        {
            if (!OutputPin)
            {
                continue;
            }

            for (UPCGEdge* Edge : OutputPin->Edges)
            {
                if (!Edge)
                {
                    continue;
                }

                UPCGPin* SourcePin = Edge->InputPin;
                UPCGPin* DestPin = Edge->OutputPin;
                if (!SourcePin || !DestPin || !SourcePin->Node || !DestPin->Node)
                {
                    Warnings.Add(TEXT("Skipping dangling PCG edge with null pin or owner."));
                    continue;
                }

                const FString* SourceName = NodeNames.Find(SourcePin->Node);
                const FString* DestName = NodeNames.Find(DestPin->Node);
                if (!SourceName || !DestName)
                {
                    Warnings.Add(TEXT("Skipping PCG edge referencing a node not registered in this graph walk."));
                    continue;
                }

                AppendPCGBodyLine(Lines, FString::Printf(
                    TEXT("connect %s.%s -> %s.%s"),
                    **SourceName,
                    *FormatPinLabel(SourcePin->Properties.Label),
                    **DestName,
                    *FormatPinLabel(DestPin->Properties.Label)));
            }
        }
    }
}

FPCGIRDecompileResult FPCGIRDecompiler::DecompileGraph(
    UPCGGraph* Graph,
    const FPCGIRDecompileOptions& Options)
{
    if (!Graph)
    {
        return FPCGIRDecompileResult::MakeError(TEXT("DecompileGraph: Graph is null."));
    }

    FPCGIRDecompileResult Result;
    if (Options.bIncludeReferencedSubgraphs)
    {
        // includeReferencedSubgraphs is reserved on the public surface for
        // forward compatibility but the decompiler always flattens; surface
        // a warning so callers that opt in see what's happening.
        Result.Warnings.Add(TEXT("includeReferencedSubgraphs is reserved for future use; subgraphs are always flattened in the current decompile direction."));
    }

    TArray<FString> Lines;
    Lines.Add(FString::Printf(
        TEXT("entry pcg %s {"),
        *FIrTextUtils::FormatNameToken(Graph->GetPathName())));

    const int32 BodyStart = Lines.Num();

    // Collect terminal nodes plus the per-graph node array. GetInputNode /
    // GetOutputNode return the well-known terminals that aren't part of the
    // regular Nodes array; emit them with reserved names so connect
    // statements can address them grammar-uniformly.
    TMap<UPCGNode*, FString> NodeNames;
    TArray<UPCGNode*> EmissionOrder;

    if (UPCGNode* InputNode = Graph->GetInputNode())
    {
        NodeNames.Add(InputNode, kInputNodeName);
        EmissionOrder.Add(InputNode);
    }
    if (UPCGNode* OutputNode = Graph->GetOutputNode())
    {
        if (!NodeNames.Contains(OutputNode))
        {
            NodeNames.Add(OutputNode, kOutputNodeName);
            EmissionOrder.Add(OutputNode);
        }
    }

    int32 SequentialIndex = 1;
    for (UPCGNode* Node : Graph->GetNodes())
    {
        if (!Node || NodeNames.Contains(Node))
        {
            continue;
        }
        const FString Name = FString::Printf(TEXT("N%d"), SequentialIndex++);
        NodeNames.Add(Node, Name);
        EmissionOrder.Add(Node);
    }

    for (UPCGNode* Node : EmissionOrder)
    {
        EmitNode(Node, NodeNames.FindChecked(Node), Lines, Result.Warnings);
    }

    for (UPCGNode* Node : EmissionOrder)
    {
        EmitEdgesForNode(Node, NodeNames, Lines, Result.Warnings);
    }

    if (Lines.Num() == BodyStart)
    {
        // Disambiguate intentional empty body from failed extraction.
        AppendPCGBodyLine(Lines, TEXT("# no nodes"));
    }

    Lines.Add(TEXT("}"));

    Result.bSuccess = true;
    Result.PCGIRText = FString::Join(Lines, TEXT("\n"));
    return Result;
}

#else // !__has_include("PCGGraph.h")

FPCGIRDecompileResult FPCGIRDecompiler::DecompileGraph(
    UPCGGraph* /*Graph*/,
    const FPCGIRDecompileOptions& /*Options*/)
{
    return FPCGIRDecompileResult::MakeError(TEXT("PCG plugin not available; PCGIR decompile is unsupported in this build."));
}

#endif // __has_include("PCGGraph.h")
