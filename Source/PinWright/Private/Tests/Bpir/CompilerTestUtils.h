// Copyright (c) 2026 Alexander Penkin. MIT License.

// Shared utilities for BPIR compiler/decompiler tests.
// Extracts duplicated helpers (CreateTransientTestBP, CountNodesOfType, etc.)
// that were copy-pasted across 6+ test files.
#pragma once

#include "CoreMinimal.h"

#include "Engine/Blueprint.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "K2Node_CallFunction.h"
#include "Kismet/KismetSystemLibrary.h"
#include "EdGraphSchema_K2.h"
#include "Compiler/CompilerTypes.h"
#include "Compiler/BpirCompiler.h"

namespace CompilerTestUtils
{
    // Picks a name under the transient package that no live UObject currently occupies.
    // CreateBlueprint() asserts when a UBlueprint already exists at the target name, and
    // prior test BPs can still be resident (not yet GC'd). MakeUniqueObjectName queries
    // the Outer for a free slot, so it is robust regardless of GC timing -- FMath::Rand()
    // is not, since it can repeat values within a session and collide with a resident BP.
    inline FName MakeUniqueTestBPName(const FString& NamePrefix)
    {
        return MakeUniqueObjectName(GetTransientPackage(), UBlueprint::StaticClass(), FName(*NamePrefix));
    }

    // Creates a minimal transient AActor-based Blueprint that is cleaned up by GC.
    // The NamePrefix is just a label; the actual object name is made unique per call.
    inline UBlueprint* CreateTransientTestBP(const FString& NamePrefix = TEXT("TestBP"))
    {
        return FKismetEditorUtilities::CreateBlueprint(
            AActor::StaticClass(),
            GetTransientPackage(),
            MakeUniqueTestBPName(NamePrefix),
            BPTYPE_Normal,
            UBlueprint::StaticClass(),
            UBlueprintGeneratedClass::StaticClass());
    }

    inline UBlueprint* CreateTransientTestBPWithParent(UClass* ParentClass, const FString& NamePrefix = TEXT("TestBP"))
    {
        if (!ParentClass)
        {
            return nullptr;
        }

        return FKismetEditorUtilities::CreateBlueprint(
            ParentClass,
            GetTransientPackage(),
            MakeUniqueTestBPName(NamePrefix),
            BPTYPE_Normal,
            UBlueprint::StaticClass(),
            UBlueprintGeneratedClass::StaticClass());
    }

    // Find a member variable on a Blueprint by exact name in NewVariables, or nullptr.
    inline const FBPVariableDescription* FindNewVariableByName(const UBlueprint* BP, const FString& VarName)
    {
        if (!BP)
        {
            return nullptr;
        }
        const FName Target(*VarName);
        for (const FBPVariableDescription& Var : BP->NewVariables)
        {
            if (Var.VarName == Target)
            {
                return &Var;
            }
        }
        return nullptr;
    }

    inline UEdGraph* FindMacroGraph(UBlueprint* Blueprint, const FString& Name)
    {
        if (!Blueprint)
        {
            return nullptr;
        }

        for (UEdGraph* Graph : Blueprint->MacroGraphs)
        {
            if (Graph && Graph->GetName().Equals(Name, ESearchCase::IgnoreCase))
            {
                return Graph;
            }
        }
        return nullptr;
    }

    // Returns the first node of type T in a specific graph, or nullptr.
    template<typename T>
    T* FindNodeOfType(const UEdGraph* Graph)
    {
        if (!Graph) return nullptr;
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            if (T* Typed = Cast<T>(Node))
            {
                return Typed;
            }
        }
        return nullptr;
    }

    // Returns the first node of type T anywhere in the Blueprint's UbergraphPages
    // or FunctionGraphs. Generic K2Node round-trip tests place nodes via BPIR which
    // routes through different graph kinds depending on whether the entry block is an
    // event or a function, so both arrays must be walked.
    template<typename T>
    T* FindNodeOfType(UBlueprint* BP)
    {
        if (!BP) return nullptr;
        for (UEdGraph* Graph : BP->UbergraphPages)
        {
            if (T* Found = FindNodeOfType<T>(Graph))
            {
                return Found;
            }
        }
        for (UEdGraph* Graph : BP->FunctionGraphs)
        {
            if (T* Found = FindNodeOfType<T>(Graph))
            {
                return Found;
            }
        }
        return nullptr;
    }

    // Counts all nodes of type T across every ubergraph page of the Blueprint.
    template<typename T>
    int32 CountNodesOfType(UBlueprint* BP)
    {
        int32 Count = 0;
        for (UEdGraph* Graph : BP->UbergraphPages)
        {
            for (UEdGraphNode* Node : Graph->Nodes)
            {
                if (Cast<T>(Node))
                {
                    Count++;
                }
            }
        }
        return Count;
    }

    // Returns the total node count across every ubergraph page.
    inline int32 CountAllEventGraphNodes(UBlueprint* BP)
    {
        int32 Count = 0;
        for (UEdGraph* Graph : BP->UbergraphPages)
        {
            Count += Graph->Nodes.Num();
        }
        return Count;
    }

    // Find a CallFunction node whose function name contains the given substring.
    inline UK2Node_CallFunction* FindCallFunctionBySubstring(UBlueprint* BP, const FString& Substring)
    {
        if (!BP) return nullptr;
        for (UEdGraph* Graph : BP->UbergraphPages)
        {
            for (UEdGraphNode* Node : Graph->Nodes)
            {
                UK2Node_CallFunction* CallNode = Cast<UK2Node_CallFunction>(Node);
                if (CallNode)
                {
                    FName FuncName = CallNode->FunctionReference.GetMemberName();
                    if (FuncName.ToString().Contains(Substring))
                    {
                        return CallNode;
                    }
                }
            }
        }
        return nullptr;
    }

    // Get the node connected to a specific exec output pin (by name).
    inline UEdGraphNode* GetExecDownstream(UEdGraphNode* Node, const FString& PinName)
    {
        if (!Node) return nullptr;
        for (UEdGraphPin* Pin : Node->Pins)
        {
            if (Pin && Pin->Direction == EGPD_Output
                && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec
                && (PinName.IsEmpty() || Pin->PinName.ToString() == PinName))
            {
                if (Pin->LinkedTo.Num() > 0)
                {
                    return Pin->LinkedTo[0]->GetOwningNode();
                }
            }
        }
        return nullptr;
    }

    // Create and register a node of type T in the graph.
    template<typename T>
    T* SpawnNode(UEdGraph* Graph, int32 PosX = 0, int32 PosY = 0)
    {
        T* Node = NewObject<T>(Graph);
        Node->CreateNewGuid();
        Node->PostPlacedNewNode();
        Node->AllocateDefaultPins();
        Node->NodePosX = PosX;
        Node->NodePosY = PosY;
        Graph->AddNode(Node, /*bFromUI=*/true, /*bSelectNewNode=*/false);
        return Node;
    }

    // Spawn a UK2Node_CallFunction targeting UKismetSystemLibrary::PrintString.
    // Must set FunctionReference + ReconstructNode() to get real exec-in/exec-out pins;
    // a bare-constructed UK2Node_CallFunction has no pins because AllocateDefaultPins
    // early-returns when the function reference is unset.
    inline UK2Node_CallFunction* SpawnPrintStringCall(UEdGraph* Graph, int32 PosX, int32 PosY)
    {
        UK2Node_CallFunction* Node = SpawnNode<UK2Node_CallFunction>(Graph, PosX, PosY);
        Node->FunctionReference.SetExternalMember(
            GET_FUNCTION_NAME_CHECKED(UKismetSystemLibrary, PrintString),
            UKismetSystemLibrary::StaticClass());
        Node->ReconstructNode();
        return Node;
    }

    // Wires the "then" output of Src to the "execute" input of Dst (standard linear exec flow).
    // Falls back to the first input exec pin for macro nodes (e.g. ForEachLoop uses "Exec" not "execute").
    inline bool WireThenToExec(UEdGraphNode* Src, UEdGraphNode* Dst)
    {
        UEdGraphPin* ThenPin = Src->FindPin(UEdGraphSchema_K2::PN_Then, EGPD_Output);
        UEdGraphPin* ExecPin = Dst->FindPin(UEdGraphSchema_K2::PN_Execute, EGPD_Input);
        if (!ExecPin)
        {
            ExecPin = GetDefault<UEdGraphSchema_K2>()->FindExecutionPin(*Dst, EGPD_Input);
        }
        if (ThenPin && ExecPin)
        {
            ThenPin->MakeLinkTo(ExecPin);
            return true;
        }
        return false;
    }

    // Returns true if any error message contains the given substring (case-insensitive).
    inline bool ErrorsContain(const TArray<FCompileError>& Errors, const FString& Substring)
    {
        for (const FCompileError& Err : Errors)
        {
            if (Err.Message.Contains(Substring, ESearchCase::IgnoreCase))
            {
                return true;
            }
        }
        return false;
    }
} // namespace CompilerTestUtils
