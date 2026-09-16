// Copyright (c) 2026 Alexander Penkin. MIT License.

// GraphSelectionHandler.cpp - Get and delete selected Blueprint graph nodes

#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/ParamSpec.h"
#include "Dom/JsonObject.h"

#include "Editor.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "Engine/Blueprint.h"
#include "BlueprintEditor.h"
#include "ScopedTransaction.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "EdGraphNode_Comment.h"
#include "Misc/ScopedSlowTask.h"

static FBlueprintEditor* FindActiveBlueprintEditor()
{
    if (!GEditor)
    {
        return nullptr;
    }

    UAssetEditorSubsystem* AssetEditorSS = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();
    if (!AssetEditorSS)
    {
        return nullptr;
    }

    FBlueprintEditor* BestEditor = nullptr;
    double BestTime = 0.0;

    TArray<UObject*> EditedAssets = AssetEditorSS->GetAllEditedAssets();
    for (UObject* Asset : EditedAssets)
    {
        if (!Asset)
        {
            continue;
        }

        IAssetEditorInstance* EditorInstance = AssetEditorSS->FindEditorForAsset(Asset, false);
        if (!EditorInstance)
        {
            continue;
        }

        // RTTI is disabled (/GR-), so use GetEditorName() instead of dynamic_cast
        if (EditorInstance->GetEditorName() != FName("BlueprintEditor"))
        {
            continue;
        }
        FBlueprintEditor* BPEditor = static_cast<FBlueprintEditor*>(EditorInstance);
        if (!BPEditor)
        {
            continue;
        }

        const double ActivationTime = EditorInstance->GetLastActivationTime();
        if (!BestEditor || ActivationTime > BestTime)
        {
            BestEditor = BPEditor;
            BestTime = ActivationTime;
        }
    }

    return BestEditor;
}

// ---- editor.get_selected_graph_nodes ----
REGISTER_RPC_HANDLER("editor.get_selected_graph_nodes", "editor", "Return information about every node currently selected in the most recently activated Blueprint graph editor. Each entry has nodeId, title, className, position, and pin metadata. Errors NO_BLUEPRINT_EDITOR if no Blueprint editor window is open.",
    RPC_NO_PARAMS)
{
    if (!GEditor)
    {
        Ctx.SendError(TEXT("NO_EDITOR"), TEXT("Editor is not available"));
        return true;
    }

    FBlueprintEditor* BPEditor = FindActiveBlueprintEditor();
    if (!BPEditor)
    {
        Ctx.SendError(TEXT("NO_BLUEPRINT_EDITOR"), TEXT("No active Blueprint editor found"));
        return true;
    }

    const FGraphPanelSelectionSet& SelectedNodes = BPEditor->GetSelectedNodes();

    TArray<TSharedPtr<FJsonValue>> NodesArray;
    for (UObject* Obj : SelectedNodes)
    {
        UEdGraphNode* Node = Cast<UEdGraphNode>(Obj);
        if (!Node)
        {
            continue;
        }

        TSharedPtr<FJsonObject> NodeJson = MakeShared<FJsonObject>();
        NodeJson->SetStringField(TEXT("nodeId"), Node->NodeGuid.ToString());
        NodeJson->SetStringField(TEXT("title"), Node->GetNodeTitle(ENodeTitleType::FullTitle).ToString());
        NodeJson->SetStringField(TEXT("className"), Node->GetClass()->GetName());
        NodeJson->SetNumberField(TEXT("posX"), Node->NodePosX);
        NodeJson->SetNumberField(TEXT("posY"), Node->NodePosY);

        TArray<TSharedPtr<FJsonValue>> PinsArray;
        for (const UEdGraphPin* Pin : Node->Pins)
        {
            if (!Pin)
            {
                continue;
            }
            TSharedPtr<FJsonObject> PinJson = MakeShared<FJsonObject>();
            PinJson->SetStringField(TEXT("name"), Pin->PinName.ToString());
            PinJson->SetStringField(TEXT("type"), Pin->PinType.PinCategory.ToString());
            PinJson->SetStringField(TEXT("direction"), Pin->Direction == EGPD_Input ? TEXT("input") : TEXT("output"));
            PinJson->SetBoolField(TEXT("connected"), Pin->LinkedTo.Num() > 0);
            PinsArray.Add(MakeShared<FJsonValueObject>(PinJson));
        }
        NodeJson->SetArrayField(TEXT("pins"), PinsArray);

        NodesArray.Add(MakeShared<FJsonValueObject>(NodeJson));
    }

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetBoolField(TEXT("success"), true);
    Resp->SetNumberField(TEXT("count"), NodesArray.Num());
    Resp->SetArrayField(TEXT("nodes"), NodesArray);
    Ctx.SendSuccess(Resp);
    return true;
}

// ---- editor.delete_selected_graph_nodes ----
REGISTER_RPC_HANDLER("editor.delete_selected_graph_nodes", "editor", "Delete the currently selected nodes in the active Blueprint graph editor and mark the Blueprint dirty. Skips comment nodes and nodes the user is not allowed to delete; returns deletedCount. Wrapped in an undoable transaction.",
    RPC_NO_PARAMS)
{
    if (!GEditor)
    {
        Ctx.SendError(TEXT("NO_EDITOR"), TEXT("Editor is not available"));
        return true;
    }

    FBlueprintEditor* BPEditor = FindActiveBlueprintEditor();
    if (!BPEditor)
    {
        Ctx.SendError(TEXT("NO_BLUEPRINT_EDITOR"), TEXT("No active Blueprint editor found"));
        return true;
    }

    UBlueprint* Blueprint = BPEditor->GetBlueprintObj();
    if (!Blueprint)
    {
        Ctx.SendError(TEXT("NO_BLUEPRINT"), TEXT("No Blueprint associated with the active editor"));
        return true;
    }

    const FGraphPanelSelectionSet& SelectedNodes = BPEditor->GetSelectedNodes();
    if (SelectedNodes.Num() == 0)
    {
        TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
        Resp->SetBoolField(TEXT("success"), true);
        Resp->SetNumberField(TEXT("deletedCount"), 0);
        Resp->SetStringField(TEXT("note"), TEXT("No nodes were selected"));
        Ctx.SendSuccess(Resp);
        return true;
    }

    TArray<UEdGraphNode*> NodesToDelete;
    for (UObject* Obj : SelectedNodes)
    {
        UEdGraphNode* Node = Cast<UEdGraphNode>(Obj);
        if (!Node)
        {
            continue;
        }
        // Skip comment nodes
        if (Cast<UEdGraphNode_Comment>(Node))
        {
            continue;
        }
        if (Node->CanUserDeleteNode())
        {
            NodesToDelete.Add(Node);
        }
    }

    FScopedTransaction Transaction(FText::FromString(TEXT("Delete Selected Graph Nodes")));
    int32 DeletedCount = 0;
    for (UEdGraphNode* Node : NodesToDelete)
    {
        Node->Modify();
        Node->DestroyNode();
        DeletedCount++;
    }

    if (DeletedCount > 0)
    {
        FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
    }

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetBoolField(TEXT("success"), true);
    Resp->SetNumberField(TEXT("deletedCount"), DeletedCount);
    Ctx.SendSuccess(Resp);
    return true;
}
