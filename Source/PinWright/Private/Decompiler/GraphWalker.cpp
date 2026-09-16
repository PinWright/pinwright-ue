// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Decompiler/GraphWalker.h"
#include "Handlers/Blueprint/BlueprintHandlerUtils.h"
#include "Compiler/BpirSharedConstants.h"

#include "K2Node_Event.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#include "K2Node_InputKey.h"
#include "K2Node_ComponentBoundEvent.h"
#include "K2Node_IfThenElse.h"
#include "K2Node_MacroInstance.h"
#include "K2Node_Tunnel.h"
#include "K2Node_ExecutionSequence.h"
#include "K2Node_DynamicCast.h"
#include "K2Node_Switch.h"
#include "K2Node_Timeline.h"
#include "K2Node_CallFunction.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "EdGraphNode_Comment.h"
#include "K2Node_Knot.h"
#include "EdGraphSchema_K2.h"

#if __has_include("K2Node_InputAction.h")
#include "K2Node_InputAction.h"
#define MCP_HAS_INPUT_ACTION 1
#else
#define MCP_HAS_INPUT_ACTION 0
#endif
#if __has_include("K2Node_InputTouch.h")
#include "K2Node_InputTouch.h"
#define MCP_HAS_INPUT_TOUCH 1
#else
#define MCP_HAS_INPUT_TOUCH 0
#endif
#if __has_include("K2Node_ActorBoundEvent.h")
#include "K2Node_ActorBoundEvent.h"
#define MCP_HAS_ACTOR_BOUND_EVENT 1
#else
#define MCP_HAS_ACTOR_BOUND_EVENT 0
#endif
#if __has_include("K2Node_InputAxisEvent.h")
#include "K2Node_InputAxisEvent.h"
#define MCP_HAS_INPUT_AXIS_EVENT 1
#else
#define MCP_HAS_INPUT_AXIS_EVENT 0
#endif
#if __has_include("K2Node_InputAxisKeyEvent.h")
#include "K2Node_InputAxisKeyEvent.h"
#define MCP_HAS_INPUT_AXIS_KEY_EVENT 1
#else
#define MCP_HAS_INPUT_AXIS_KEY_EVENT 0
#endif

#if __has_include("K2Node_CallDelegate.h")
#include "K2Node_CallDelegate.h"
#include "K2Node_AddDelegate.h"
#include "K2Node_RemoveDelegate.h"
#include "K2Node_ClearDelegate.h"
#define MCP_HAS_DELEGATE_NODES 1
#else
#define MCP_HAS_DELEGATE_NODES 0
#endif
#include "EdGraph/EdGraph.h"

DEFINE_LOG_CATEGORY_STATIC(LogGraphWalker, Log, All);

namespace
{
    // Class -> semantics registry used by FGraphWalker::ClassifyNode.
    //
    // Lazily populated via a function-local static (C++11-thread-safe under MSVC).
    // Only contains arms whose dispatch is purely class-based; behaviorally-
    // conditional arms (UK2Node_MacroInstance macro-name dispatch,
    // UK2Node_Tunnel direction dispatch, UK2Node_CallFunction field-notify allowlist
    // and IsLatentNode check) remain as imperative pre-checks in ClassifyNode
    // so that ordering invariants are preserved (notably MacroInstance must be
    // matched before Tunnel because UK2Node_MacroInstance derives from
    // UK2Node_Tunnel).
    const TMap<UClass*, ENodeSemantics>& GetSemanticsRegistry()
    {
        static const TMap<UClass*, ENodeSemantics> Map = []
        {
            TMap<UClass*, ENodeSemantics> M;
            M.Add(UK2Node_IfThenElse::StaticClass(),       ENodeSemantics::Branch);
            M.Add(UK2Node_ExecutionSequence::StaticClass(), ENodeSemantics::Sequence);
            M.Add(UK2Node_DynamicCast::StaticClass(),      ENodeSemantics::Cast);
            M.Add(UK2Node_Timeline::StaticClass(),         ENodeSemantics::Timeline);
            M.Add(UK2Node_Switch::StaticClass(),           ENodeSemantics::Switch);
            M.Add(UK2Node_FunctionResult::StaticClass(),   ENodeSemantics::Return);
            M.Add(UK2Node_VariableSet::StaticClass(),      ENodeSemantics::VariableSet);
            M.Add(UK2Node_VariableGet::StaticClass(),      ENodeSemantics::VariableGet);
            M.Add(UEdGraphNode_Comment::StaticClass(),     ENodeSemantics::Comment);
            M.Add(UK2Node_Knot::StaticClass(),             ENodeSemantics::Knot);
#if MCP_HAS_DELEGATE_NODES
            M.Add(UK2Node_CallDelegate::StaticClass(),     ENodeSemantics::Dispatcher);
            M.Add(UK2Node_AddDelegate::StaticClass(),      ENodeSemantics::Dispatcher);
            M.Add(UK2Node_RemoveDelegate::StaticClass(),   ENodeSemantics::Dispatcher);
            M.Add(UK2Node_ClearDelegate::StaticClass(),    ENodeSemantics::Dispatcher);
#endif
            return M;
        }();
        return Map;
    }
}

FGraphWalker::FGraphWalker(UEdGraph* InGraph)
    : Graph(InGraph)
{
}

TArray<UEdGraphNode*> FGraphWalker::FindEntryPoints()
{
    TArray<UEdGraphNode*> EntryPoints;

    if (!Graph)
    {
        return EntryPoints;
    }

    // Collect entry nodes from this graph and all transitively nested subgraphs.
    // K2Node_Event placed inside a composite's BoundGraph is a real entry point —
    // the engine registers its tick/begin-play delegate regardless of nesting depth.
    BlueprintHandlerUtils::CollectEntryNodesRecursive(Graph, EntryPoints);

    // Sort by (GraphPathName, NodePosY) for deterministic ordering across nested scopes.
    // Two entry nodes from different child graphs with the same Y position must not float;
    // the graph path provides a stable secondary discriminator.
    EntryPoints.Sort([](const UEdGraphNode& A, const UEdGraphNode& B)
    {
        const FString PathA = A.GetGraph() ? A.GetGraph()->GetPathName() : FString();
        const FString PathB = B.GetGraph() ? B.GetGraph()->GetPathName() : FString();
        if (PathA != PathB)
        {
            return PathA < PathB;
        }
        return A.NodePosY < B.NodePosY;
    });

    return EntryPoints;
}

ENodeSemantics FGraphWalker::ClassifyNode(UEdGraphNode* Node)
{
    if (!Node)
    {
        return ENodeSemantics::Unknown;
    }

    // Pre-registry imperative dispatch for arms whose semantics depend on more
    // than the node's UClass.
    //
    // MacroInstance MUST be checked before Tunnel: UK2Node_MacroInstance derives
    // from UK2Node_Tunnel, so a registry-only walk would resolve macro instances
    // through the Tunnel ancestor and return TunnelEntry/Exit instead of the
    // correct macro-name-derived semantic.
    if (Node->IsA<UK2Node_MacroInstance>())
    {
        const FString MacroName = GetMacroName(Node);

        if (MacroName == BpirSharedConstants::MacroNames::ForEachLoop || MacroName == BpirSharedConstants::MacroNames::ForEachLoopWithBreak)
        {
            return ENodeSemantics::ForEach;
        }
        if (MacroName == BpirSharedConstants::MacroNames::While)
        {
            return ENodeSemantics::WhileLoop;
        }
        if (MacroName == TEXT("DoOnce"))
        {
            return ENodeSemantics::DoOnce;
        }
        if (MacroName == TEXT("Gate"))
        {
            return ENodeSemantics::Gate;
        }
        if (MacroName == TEXT("FlipFlop"))
        {
            return ENodeSemantics::FlipFlop;
        }

        return ENodeSemantics::MacroInstance;
    }

    // Tunnel nodes for macro graph entry/exit. Direction-dependent — cannot
    // be expressed as a flat class entry. Entry tunnels go through the shared
    // BlueprintHandlerUtils::IsMacroEntryTunnel predicate so all three call sites
    // (here, IsBlueprintEntryNode, BpirDecompiler entry-param resolver) agree.
    if (BlueprintHandlerUtils::IsMacroEntryTunnel(Node))
    {
        return ENodeSemantics::TunnelEntry;
    }
    if (UK2Node_Tunnel* Tunnel = Cast<UK2Node_Tunnel>(Node))
    {
        if (Tunnel->bCanHaveInputs && !Tunnel->bCanHaveOutputs)
            return ENodeSemantics::TunnelExit;
    }

    // CallFunction needs target-function inspection (field-notify allowlist) and
    // the IsLatentNode pin probe — neither expressible via the class registry.
    if (UK2Node_CallFunction* CallFn = Cast<UK2Node_CallFunction>(Node))
    {
        const UFunction* Func = CallFn->GetTargetFunction();
        if (Func)
        {
            const FName FuncName = Func->GetFName();
            if (FuncName == FName(BpirSharedConstants::FieldNotify::SubscribeFnName) ||
                FuncName == FName(BpirSharedConstants::FieldNotify::UnsubscribeFnName))
            {
                return ENodeSemantics::Dispatcher;
            }
        }
        return IsLatentNode(Node) ? ENodeSemantics::Latent : ENodeSemantics::FunctionCall;
    }

    // Class registry walk. Walks the inheritance chain starting at the node's
    // most-derived class so subclass entries win over base entries (matches the
    // first-match-wins semantics of the previous IsA<> chain). Examples:
    //   - UK2Node_SwitchInteger -> walks up to UK2Node_Switch -> Switch
    //   - Plugin-registered subclass of UK2Node_VariableGet -> VariableGet
    const TMap<UClass*, ENodeSemantics>& Registry = GetSemanticsRegistry();
    for (UClass* C = Node->GetClass(); C; C = C->GetSuperClass())
    {
        if (const ENodeSemantics* Found = Registry.Find(C))
        {
            return *Found;
        }
    }

    return ENodeSemantics::Unknown;
}

bool FGraphWalker::IsLatentNode(UEdGraphNode* Node)
{
    UK2Node_CallFunction* CallNode = Cast<UK2Node_CallFunction>(Node);
    if (!CallNode)
    {
        return false;
    }

    // Check for a LatentInfo pin, which is the canonical latent marker
    for (UEdGraphPin* Pin : CallNode->Pins)
    {
        if (Pin && Pin->PinName == TEXT("LatentInfo"))
        {
            return true;
        }
    }

    UFunction* Function = CallNode->GetTargetFunction();
    if (!Function)
    {
        return false;
    }

    // FUNC_BlueprintAuthorityOnly alone does not make a node latent,
    // but some latent nodes also carry this flag — check metadata first.
    if (Function->HasMetaData(TEXT("Latent")))
    {
        return true;
    }

    return false;
}

UEdGraphPin* FGraphWalker::GetExecOutputPin(UEdGraphNode* Node, int32 Index)
{
    if (!Node)
    {
        return nullptr;
    }

    int32 Found = 0;
    for (UEdGraphPin* Pin : Node->Pins)
    {
        if (Pin
            && Pin->Direction == EGPD_Output
            && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
        {
            if (Found == Index)
            {
                return Pin;
            }
            ++Found;
        }
    }

    return nullptr;
}

TArray<UEdGraphPin*> FGraphWalker::GetAllExecOutputPins(UEdGraphNode* Node)
{
    TArray<UEdGraphPin*> Result;

    if (!Node)
    {
        return Result;
    }

    for (UEdGraphPin* Pin : Node->Pins)
    {
        if (Pin
            && Pin->Direction == EGPD_Output
            && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
        {
            Result.Add(Pin);
        }
    }

    return Result;
}

UEdGraphPin* FGraphWalker::GetCompletionPin(UEdGraphNode* Node)
{
    if (!Node)
    {
        return nullptr;
    }

    // Preferred completion pin names for latent nodes, in priority order
    static const FName CompletionNames[] = {
        TEXT("Completed"),
        TEXT("OnFinished"),
        TEXT("Then"),
    };

    for (const FName& Name : CompletionNames)
    {
        for (UEdGraphPin* Pin : Node->Pins)
        {
            if (Pin
                && Pin->Direction == EGPD_Output
                && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec
                && Pin->PinName == Name)
            {
                return Pin;
            }
        }
    }

    // Fall back to the first exec output that isn't the primary trigger output
    UEdGraphPin* FirstExec = nullptr;
    for (UEdGraphPin* Pin : Node->Pins)
    {
        if (Pin
            && Pin->Direction == EGPD_Output
            && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
        {
            if (!FirstExec)
            {
                FirstExec = Pin;
            }
            else
            {
                // Return the second exec pin — the first is often the main flow-through
                return Pin;
            }
        }
    }

    return FirstExec;
}

FSwitchInfo FGraphWalker::AnalyzeSwitch(UEdGraphNode* SwitchNode)
{
    FSwitchInfo Info;

    if (!SwitchNode)
    {
        return Info;
    }

    for (UEdGraphPin* Pin : SwitchNode->Pins)
    {
        if (!Pin || Pin->Direction != EGPD_Output)
        {
            continue;
        }

        if (Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec)
        {
            continue;
        }

        if (Pin->PinName == TEXT("Default"))
        {
            Info.DefaultPin = Pin;
        }
        else
        {
            Info.Cases.Add({ Pin->PinName.ToString(), Pin });
        }
    }

    return Info;
}

FString FGraphWalker::GetMacroName(UEdGraphNode* MacroNode)
{
    UK2Node_MacroInstance* Macro = Cast<UK2Node_MacroInstance>(MacroNode);
    if (!Macro)
    {
        return FString();
    }

    UEdGraph* MacroGraph = Macro->GetMacroGraph();
    if (!MacroGraph)
    {
        return FString();
    }

    return MacroGraph->GetName();
}
