// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"

#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "GameFramework/Actor.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "K2Node_Event.h"
#include "K2Node_CallFunction.h"
#include "K2Node_MacroInstance.h"
#include "EdGraphSchema_K2.h"
#include "EdGraph/EdGraph.h"
#include "Kismet/KismetSystemLibrary.h"

namespace BpirGraphTestHelpers
{

    inline UBlueprint* CreateOrphanTestBlueprint()
    {
        // CreateBlueprint() asserts (FindObject<UBlueprint>(Outer, Name) == 0) when a
        // UBlueprint of this name already lives in the Outer. Dozens of orphan BPs from
        // earlier tests stay resident in the transient package (not yet GC'd), and a raw
        // FMath::Rand() draw (RAND_MAX 32767) repeats within a session, so it eventually
        // collides with a resident BP and crashes the suite. MakeUniqueObjectName queries
        // the Outer for a genuinely free "TestOrphanBP_<n>" slot -- robust regardless of GC
        // timing -- mirroring CompilerTestUtils::MakeUniqueTestBPName.
        const FName Name = MakeUniqueObjectName(
            GetTransientPackage(), UBlueprint::StaticClass(), FName(TEXT("TestOrphanBP")));
        return FKismetEditorUtilities::CreateBlueprint(
            AActor::StaticClass(),
            GetTransientPackage(),
            Name,
            BPTYPE_Normal,
            UBlueprint::StaticClass(),
            UBlueprintGeneratedClass::StaticClass(),
            FName(TEXT("PinWrightTests")));
    }

    template<typename T>
    T* AddNodeToGraph(UEdGraph* Graph)
    {
        T* Node = NewObject<T>(Graph);
        Node->CreateNewGuid();
        Node->PostPlacedNewNode();
        Node->AllocateDefaultPins();
        Graph->AddNode(Node, /*bFromUI=*/true, /*bSelectNewNode=*/false);
        return Node;
    }

    inline void WireExec(UEdGraphNode* Src, UEdGraphNode* Dst)
    {
        UEdGraphPin* ThenPin = Src->FindPin(UEdGraphSchema_K2::PN_Then,    EGPD_Output);
        UEdGraphPin* ExecPin = Dst->FindPin(UEdGraphSchema_K2::PN_Execute, EGPD_Input);
        if (!ThenPin)
        {
            for (UEdGraphPin* Pin : Src->Pins)
            {
                if (Pin && Pin->Direction == EGPD_Output)
                {
                    ThenPin = Pin;
                    break;
                }
            }
        }
        if (!ExecPin)
        {
            for (UEdGraphPin* Pin : Dst->Pins)
            {
                if (Pin && Pin->Direction == EGPD_Input)
                {
                    ExecPin = Pin;
                    break;
                }
            }
        }
        if (ThenPin && ExecPin)
        {
            if (const UEdGraphSchema* Schema = ThenPin->GetSchema())
            {
                Schema->TryCreateConnection(ThenPin, ExecPin);
            }
            else
            {
                ThenPin->MakeLinkTo(ExecPin);
            }
        }
    }

    // Returns the existing override-event node for EventName on EventGraph, or
    // creates one (SetExternalMember(EventName, OwnerClass) + bOverrideFunction +
    // ReconstructNode). Centralizes the Actor override-event authoring idiom so
    // per-event tests need not re-hand-roll it.
    inline UK2Node_Event* EnsureEventNode(
        UEdGraph* EventGraph, FName EventName, UClass* OwnerClass = AActor::StaticClass())
    {
        for (UEdGraphNode* Node : EventGraph->Nodes)
        {
            if (UK2Node_Event* EventNode = Cast<UK2Node_Event>(Node))
            {
                if (EventNode->EventReference.GetMemberName() == EventName)
                {
                    return EventNode;
                }
            }
        }
        UK2Node_Event* EventNode = AddNodeToGraph<UK2Node_Event>(EventGraph);
        EventNode->EventReference.SetExternalMember(EventName, OwnerClass);
        EventNode->bOverrideFunction = true;
        EventNode->ReconstructNode();
        return EventNode;
    }

    inline UK2Node_Event* EnsureBeginPlayNode(UEdGraph* EventGraph)
    {
        return EnsureEventNode(EventGraph, TEXT("ReceiveBeginPlay"));
    }

    // Spawn a UK2Node_CallFunction targeting UKismetSystemLibrary::PrintString.
    // When bTransactional is true, the node is created with RF_Transactional and
    // added with bFromUI=false (suitable for tests that exercise undo/redo and
    // compile rollback semantics). The default path mirrors AddNodeToGraph: no
    // extra flags and bFromUI=true.
    inline UK2Node_CallFunction* AddPrintStringNode(UEdGraph* Graph, bool bTransactional = false)
    {
        if (!Graph)
        {
            return nullptr;
        }

        UK2Node_CallFunction* Node = bTransactional
            ? NewObject<UK2Node_CallFunction>(Graph, NAME_None, RF_Transactional)
            : NewObject<UK2Node_CallFunction>(Graph);
        Node->CreateNewGuid();
        Node->PostPlacedNewNode();
        if (!bTransactional)
        {
            Node->AllocateDefaultPins();
        }
        Node->FunctionReference.SetExternalMember(
            GET_FUNCTION_NAME_CHECKED(UKismetSystemLibrary, PrintString),
            UKismetSystemLibrary::StaticClass());
        Node->ReconstructNode();
        Graph->AddNode(Node, /*bFromUI=*/!bTransactional, /*bSelectNewNode=*/false);
        return Node;
    }

    // Returns the first node of type T found across UbergraphPages, FunctionGraphs,
    // and MacroGraphs of the Blueprint. Superset of CompilerTestUtils::FindNodeOfType
    // (which only walks Ubergraph + FunctionGraphs).
    template<typename T>
    inline T* FindFirstNodeOfType(UBlueprint* Blueprint)
    {
        if (!Blueprint)
        {
            return nullptr;
        }
        auto SearchGraphs = [](const auto& Graphs) -> T*
        {
            for (UEdGraph* Graph : Graphs)
            {
                if (!Graph) continue;
                for (UEdGraphNode* Node : Graph->Nodes)
                {
                    if (T* Typed = Cast<T>(Node))
                    {
                        return Typed;
                    }
                }
            }
            return nullptr;
        };
        if (T* Found = SearchGraphs(Blueprint->UbergraphPages)) return Found;
        if (T* Found = SearchGraphs(Blueprint->FunctionGraphs)) return Found;
        return SearchGraphs(Blueprint->MacroGraphs);
    }

    // Spawn a UK2Node_MacroInstance bound to /Engine StandardMacros::ForEachLoop.
    // X/Y default to 0/0; pass explicit positions when laying out a test graph.
    // Returns nullptr if StandardMacros cannot be loaded or ForEachLoop is missing.
    inline UK2Node_MacroInstance* SpawnForEachLoopMacro(UEdGraph* Graph, int32 X = 0, int32 Y = 0)
    {
        if (!Graph)
        {
            return nullptr;
        }
        UK2Node_MacroInstance* Node = NewObject<UK2Node_MacroInstance>(Graph);
        Node->CreateNewGuid();
        Node->PostPlacedNewNode();
        Node->AllocateDefaultPins();
        Node->NodePosX = X;
        Node->NodePosY = Y;
        Graph->AddNode(Node, /*bFromUI=*/true, /*bSelectNewNode=*/false);

        UBlueprint* MacroLib = LoadObject<UBlueprint>(
            nullptr,
            TEXT("/Engine/EditorBlueprintResources/StandardMacros.StandardMacros"));
        if (!MacroLib)
        {
            return nullptr;
        }

        UEdGraph* ForEachGraph = nullptr;
        for (UEdGraph* MacroGraph : MacroLib->MacroGraphs)
        {
            if (MacroGraph && MacroGraph->GetFName() == TEXT("ForEachLoop"))
            {
                ForEachGraph = MacroGraph;
                break;
            }
        }
        if (!ForEachGraph)
        {
            return nullptr;
        }

        Node->SetMacroGraph(ForEachGraph);
        Node->AllocateDefaultPins();
        Node->ReconstructNode();
        return Node;
    }

} // namespace BpirGraphTestHelpers
