// Copyright (c) 2026 Alexander Penkin. MIT License.

// WidgetHierarchyHandler.cpp
// Widget hierarchy manipulation: remove, rename, reparent.
// Analysis methods (get_widget_info, get_widget_tree, get_tree, get_widget_slot_info) have been
// consolidated into widget.describe (WidgetDescribeHandler.cpp).

#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/Blueprint/BlueprintHandlerUtils.h"
#include "Handlers/ErrorCodes.h"
#if WITH_DEV_AUTOMATION_TESTS
#include "Handlers/UI/WidgetHierarchyTestHooks.h"
#endif
#include "Handlers/UI/WidgetAuthoringUtils.h"
#include "Handlers/UI/WidgetHandlerUtils.h"
#include "ScopedTransaction.h"

#include "Blueprint/WidgetTree.h"
#include "WidgetBlueprint.h"
#include "Components/PanelWidget.h"
#include "Components/PanelSlot.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Misc/EngineVersionComparison.h"
#include "UObject/Package.h"
#if UE_VERSION_OLDER_THAN(5, 6, 0)
// 5.3-5.5 reparent: UPanelWidget::InsertChildAt has no SlotTemplate overload, so slot
// properties are carried across the move with UEngine::CopyPropertiesForUnrelatedObjects.
#include "Engine/Engine.h"
#endif
#include "K2Node_ComponentBoundEvent.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"

using namespace WidgetAuthoringHelpers;
using namespace WidgetHandlerUtils;

namespace
{
    // Gather the target widget's variable name and every descendant's variable
    // name in a single set — RemoveWidget cascades the tree on PanelWidgets, so
    // any bound event on a descendant is also left dangling after removal.
    static void CollectWidgetVariableNames(UWidget* Root, TSet<FName>& OutNames)
    {
        if (!Root) return;
        OutNames.Add(Root->GetFName());
        if (UPanelWidget* Panel = Cast<UPanelWidget>(Root))
        {
            for (int32 i = 0; i < Panel->GetChildrenCount(); ++i)
            {
                CollectWidgetVariableNames(Panel->GetChildAt(i), OutNames);
            }
        }
    }

    // Walk exec-outputs from EntryNode, collecting every reachable node. Used
    // to cascade-delete an entire bound-event subgraph so we don't leave a
    // partially-disconnected orphan chain behind.
    static void CollectBoundEventSubgraph(UEdGraphNode* EntryNode, TSet<UEdGraphNode*>& OutVisited)
    {
        if (!EntryNode) return;
        TArray<UEdGraphNode*> Queue;
        Queue.Add(EntryNode);
        while (Queue.Num() > 0)
        {
            UEdGraphNode* Current = Queue.Pop();
            if (!Current || OutVisited.Contains(Current)) continue;
            OutVisited.Add(Current);

            for (UEdGraphPin* Pin : Current->Pins)
            {
                if (!Pin) continue;
                if (Pin->Direction == EGPD_Output
                    && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
                {
                    for (UEdGraphPin* LinkedPin : Pin->LinkedTo)
                    {
                        if (!LinkedPin) continue;
                        if (UEdGraphNode* Next = LinkedPin->GetOwningNode())
                        {
                            if (!OutVisited.Contains(Next))
                            {
                                Queue.Add(Next);
                            }
                        }
                    }
                }
            }
        }
    }

    // Scan every ubergraph and function graph for K2Node_ComponentBoundEvent
    // nodes whose ComponentPropertyName is in RemovedNames, delete them and
    // their exec-reachable subgraphs. Returns how many entry nodes were removed.
    // OutCreateDelegatesRemoved receives the count of UK2Node_CreateDelegate nodes
    // that were cascade-deleted because they referenced one of the removed events.
    static int32 CascadeRemoveBoundEvents(UBlueprint* BP, const TSet<FName>& RemovedNames,
        int32& OutCreateDelegatesRemoved)
    {
        OutCreateDelegatesRemoved = 0;
        if (!BP || RemovedNames.Num() == 0) return 0;

        TArray<UEdGraph*> Graphs;
        Graphs.Append(BP->UbergraphPages);
        Graphs.Append(BP->FunctionGraphs);

        TArray<UK2Node_ComponentBoundEvent*> MatchingEntries;
        for (UEdGraph* Graph : Graphs)
        {
            if (!Graph) continue;
            for (UEdGraphNode* Node : Graph->Nodes)
            {
                UK2Node_ComponentBoundEvent* BoundEvent = Cast<UK2Node_ComponentBoundEvent>(Node);
                if (!BoundEvent) continue;
                if (RemovedNames.Contains(BoundEvent->ComponentPropertyName))
                {
                    MatchingEntries.Add(BoundEvent);
                }
            }
        }

        // Collect the mangled CustomFunctionName for each matching bound event BEFORE deletion.
        TSet<FName> BoundEventFunctionNames;
        for (UK2Node_ComponentBoundEvent* Entry : MatchingEntries)
        {
            if (Entry->CustomFunctionName != NAME_None)
                BoundEventFunctionNames.Add(Entry->CustomFunctionName);
        }

        TSet<UEdGraphNode*> NodesToDelete;
        for (UK2Node_ComponentBoundEvent* Entry : MatchingEntries)
        {
            CollectBoundEventSubgraph(Entry, NodesToDelete);
        }

        for (UEdGraphNode* Node : NodesToDelete)
        {
            if (!Node) continue;
            FBlueprintEditorUtils::RemoveNode(BP, Node, /*bDontRecompile=*/true);
        }

        OutCreateDelegatesRemoved =
            BlueprintHandlerUtils::CascadeRemoveStaleCreateDelegates(BP, BoundEventFunctionNames);

        return MatchingEntries.Num();
    }

    // Serialize broken-required-bind matches into a handler result: a stable ArrayKey
    // array of {name, type} (always present, empty when nothing broke) plus a
    // human-readable "warning" string only when at least one required bind was invalidated.
    // Action is the operation phrase for the warning (e.g. "Removing this widget").
    static void AddBrokenRequiredBindsToResult(const TArray<TPair<FName, FString>>& Broken,
        const TCHAR* ArrayKey, const TCHAR* Action, const TSharedPtr<FJsonObject>& Result)
    {
        TArray<TSharedPtr<FJsonValue>> BindsJson;
        TArray<FString> Descriptions;
        for (const TPair<FName, FString>& Bind : Broken)
        {
            TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
            Entry->SetStringField(TEXT("name"), Bind.Key.ToString());
            Entry->SetStringField(TEXT("type"), Bind.Value);
            BindsJson.Add(MakeShared<FJsonValueObject>(Entry));
            Descriptions.Add(FString::Printf(TEXT("\"%s\" (type %s)"), *Bind.Key.ToString(), *Bind.Value));
        }
        Result->SetArrayField(ArrayKey, BindsJson);

        if (Broken.Num() > 0)
        {
            Result->SetStringField(TEXT("warning"), FString::Printf(
                TEXT("%s invalidated required BindWidget target(s): %s. The next blueprint.compile will report ")
                TEXT("\"A required widget binding ... was not found.\" until a widget with the bound name is ")
                TEXT("restored or the C++ binding is made BindWidgetOptional."),
                Action, *FString::Join(Descriptions, TEXT(", "))));
        }
    }
}

// ---- widget.remove_widget ----
REGISTER_RPC_HANDLER("widget.remove_widget", "widget", "Delete a child widget from a UWidgetBlueprint's widget tree by name. Asset-side change; does not affect runtime instances already added to the viewport.",
    RPC_PARAMS(
        WidgetAssetPathParamReq(TEXT("Path to the widget blueprint")),
        RPC_PARAM_OPT("widgetName", "string", "Name of the widget to remove (canonical; aliases: name, slotName, widget_name, targetName)"),
        RPC_PARAM_OPT("name", "string", "Alias for widgetName"),
        RPC_PARAM_OPT("slotName", "string", "Alias for widgetName"),
        RPC_PARAM_OPT("widget_name", "string", "Alias for widgetName"),
        RPC_PARAM_OPT("targetName", "string", "Alias for widgetName"),
        RPC_PARAM_DEF("cleanupNewOrphans", "boolean", "Delete graph nodes that become orphaned by this deletion (default: true)", "true")
    ))
{
    FString WidgetPath = Ctx.GetStringFirstOf(WidgetAssetPathParamNames());
    FString SlotName = Ctx.GetStringFirstOf({TEXT("widgetName"), TEXT("name"), TEXT("slotName"), TEXT("widget_name"), TEXT("targetName")});
    const bool bCleanupNewOrphans = Ctx.GetBool(TEXT("cleanupNewOrphans"), true);

    if (WidgetPath.IsEmpty() || SlotName.IsEmpty())
    {
        Ctx.SendError(ErrorCodes::ERR_MISSING_PARAMETER, TEXT("Missing required parameters: widgetPath, widgetName (widgetName also accepts: name, slotName, widget_name, targetName)"));
        return true;
    }

    UWidgetBlueprint* WidgetBP = LoadWidgetBlueprint(WidgetPath);
    if (!WidgetBP || !WidgetBP->WidgetTree)
    {
        Ctx.SendError(ErrorCodes::ERR_NOT_FOUND, TEXT("Widget blueprint not found"));
        return true;
    }

    UWidget* TargetWidget = WidgetBP->WidgetTree->FindWidget(FName(*SlotName));
    if (!TargetWidget)
    {
        Ctx.SendError(ErrorCodes::ERR_NOT_FOUND, FString::Printf(TEXT("Widget '%s' not found"), *SlotName));
        return true;
    }

    FScopedTransaction Transaction(FText::FromString(TEXT("MCP: widget.remove_widget")));

    // Snapshot every descendant variable name before removal — RemoveWidget
    // cascades to children on PanelWidgets so their bound events also go stale.
    TSet<FName> RemovedNames;
    CollectWidgetVariableNames(TargetWidget, RemovedNames);
    const TSet<FGuid> OrphansBefore =
        BlueprintHandlerUtils::SnapshotBlueprintOrphanGuids(WidgetBP, true);

    WidgetBP->WidgetTree->RemoveWidget(TargetWidget);
    const int32 CompactedPanelSlots = CompactInvalidPanelSlots(WidgetBP, TargetWidget);

    // Cascade: purge K2Node_ComponentBoundEvent nodes (plus their exec chains)
    // that referenced any removed widget variable. Without this, the BP retains
    // "does not have a valid matching component" compile warnings and the
    // downstream chain can emit errors when it references other dead variables.
    int32 CascadedCreateDelegatesRemoved = 0;
    const int32 CascadedBoundEventsRemoved = CascadeRemoveBoundEvents(WidgetBP, RemovedNames,
        CascadedCreateDelegatesRemoved);

    const BlueprintHandlerUtils::FBlueprintOrphanDeltaCleanupResult OrphanCleanup =
        BlueprintHandlerUtils::CleanupNewBlueprintOrphans(WidgetBP, OrphansBefore, bCleanupNewOrphans, true);

    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WidgetBP);
    // Clean up orphaned GUIDs after compilation — doing it before MarkBlueprintAsStructurallyModified
    // causes an ensure in ValidateAndFixUpVariableGuids because the UWidget UObject is still alive
    // (outer is WidgetTree) but its GUID is already removed from the map.
    EnsureAllWidgetVariableGuids(WidgetBP);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetBoolField(TEXT("success"), true);
    Result->SetStringField(TEXT("widgetPath"), WidgetPath);
    Result->SetStringField(TEXT("removedWidget"), SlotName);
    Result->SetNumberField(TEXT("cascadedBoundEventsRemoved"), CascadedBoundEventsRemoved);
    Result->SetNumberField(TEXT("cascadedCreateDelegatesRemoved"), CascadedCreateDelegatesRemoved);
    Result->SetNumberField(TEXT("compactedPanelSlots"), CompactedPanelSlots);
    BlueprintHandlerUtils::AddOrphanDeltaCleanupResultToJson(OrphanCleanup, Result);

    TArray<TSharedPtr<FJsonValue>> RemovedNamesJson;
    for (const FName& Name : RemovedNames)
    {
        RemovedNamesJson.Add(MakeShared<FJsonValueString>(Name.ToString()));
    }
    Result->SetArrayField(TEXT("removedWidgets"), RemovedNamesJson);

    // Advisory: if the removed widget (or any cascaded descendant) satisfied a required
    // meta=(BindWidget) on the parent class chain, surface it so the caller learns at
    // remove time rather than only when the next blueprint.compile fails. Removal itself
    // stays successful — this is a non-breaking advisory, never a refusal.
    TArray<TPair<FName, FString>> BrokenRequiredBinds;
    CollectBrokenRequiredBinds(WidgetBP, RemovedNames, BrokenRequiredBinds);
    AddBrokenRequiredBindsToResult(BrokenRequiredBinds, TEXT("removedRequiredBinds"),
        TEXT("Removing this widget"), Result);

    Ctx.SendSuccess(Result);
    return true;
}

// ---- widget.rename_widget ----
REGISTER_RPC_HANDLER("widget.rename_widget", "widget", "Rename a child widget within a UWidgetBlueprint's widget tree; validates the destination and verifies tree readback before updating references, but does not auto-update Blueprint graph references.",
    RPC_PARAMS(
        WidgetAssetPathParamReq(TEXT("Path to the widget blueprint")),
        RPC_PARAM_OPT("widgetName", "string", "Current name of the widget (canonical; aliases: name, slotName, widget_name, targetName)"),
        RPC_PARAM_OPT("name", "string", "Alias for widgetName"),
        RPC_PARAM_OPT("slotName", "string", "Alias for widgetName"),
        RPC_PARAM_OPT("widget_name", "string", "Alias for widgetName"),
        RPC_PARAM_OPT("targetName", "string", "Alias for widgetName"),
        RPC_PARAM_REQ("newName", "string", "New name for the widget")
    ))
{
    FString WidgetPath = Ctx.GetStringFirstOf(WidgetAssetPathParamNames());
    FString OldName = Ctx.GetStringFirstOf({TEXT("widgetName"), TEXT("name"), TEXT("slotName"), TEXT("widget_name"), TEXT("targetName")});
    FString NewName = Ctx.GetString(TEXT("newName"));

    if (WidgetPath.IsEmpty() || OldName.IsEmpty() || NewName.IsEmpty())
    {
        Ctx.SendError(ErrorCodes::ERR_MISSING_PARAMETER, TEXT("Missing required parameters: widgetPath, widgetName, newName (widgetName also accepts: name, slotName, widget_name, targetName)"));
        return true;
    }

    UWidgetBlueprint* WidgetBP = LoadWidgetBlueprint(WidgetPath);
    if (!WidgetBP || !WidgetBP->WidgetTree)
    {
        Ctx.SendError(ErrorCodes::ERR_NOT_FOUND, TEXT("Widget blueprint not found"));
        return true;
    }

    UWidget* TargetWidget = WidgetBP->WidgetTree->FindWidget(FName(*OldName));
    if (!TargetWidget)
    {
        Ctx.SendError(ErrorCodes::ERR_NOT_FOUND, FString::Printf(TEXT("Widget '%s' not found"), *OldName));
        return true;
    }

    FName OldFName = TargetWidget->GetFName();
    FText NameReason;
    if (!FName::IsValidXName(NewName, INVALID_OBJECTNAME_CHARACTERS, &NameReason))
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
            FString::Printf(TEXT("Invalid widget name '%s': %s"), *NewName, *NameReason.ToString()));
        return true;
    }

    const FName NewFName(*NewName);
    UWidget* ExistingWidget = nullptr;
    // FindWidget only walks the visible hierarchy. UObject::Rename, however,
    // checks every object directly outered to WidgetTree, including stale
    // source widgets left behind by an earlier failed edit. Scan the same
    // source set before Rename so an orphaned destination is reported instead
    // of reaching UObject's fatal duplicate-name path.
    WidgetBP->ForEachSourceWidget([&ExistingWidget, TargetWidget, NewFName](UWidget* SourceWidget)
    {
        if (!ExistingWidget && SourceWidget && SourceWidget != TargetWidget &&
            SourceWidget->GetFName() == NewFName)
        {
            ExistingWidget = SourceWidget;
        }
    });
    if (ExistingWidget)
    {
        Ctx.SendError(ErrorCodes::ERR_DESTINATION_EXISTS,
            FString::Printf(TEXT("Widget rename destination '%s' already exists"), *NewName));
        return true;
    }

    auto SendRenameSuccess = [&]()
    {
        TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
        Result->SetBoolField(TEXT("success"), true);
        Result->SetStringField(TEXT("widgetPath"), WidgetPath);
        Result->SetStringField(TEXT("oldName"), OldName);
        Result->SetStringField(TEXT("newName"), TargetWidget->GetName());

        // Advisory: renaming a widget off a required meta=(BindWidget) name breaks that bind
        // the same way removing it does (the bind matches by variable name). Only flag when the
        // name actually changed. Rename stays successful — this is a non-breaking advisory.
        TArray<TPair<FName, FString>> BrokenRequiredBinds;
        if (OldFName != TargetWidget->GetFName())
        {
            CollectBrokenRequiredBinds(WidgetBP, TSet<FName>{ OldFName }, BrokenRequiredBinds);
        }
        AddBrokenRequiredBindsToResult(BrokenRequiredBinds, TEXT("renamedRequiredBinds"),
            TEXT("Renaming this widget away from its bound name"), Result);

        Ctx.SendSuccess(Result);
        return true;
    };

    // UObject treats an FName-equivalent request as a successful no-op. Keep that path
    // transaction-free and report the name that is actually on the object.
    if (OldFName == NewFName)
    {
        return SendRenameSuccess();
    }

    UPackage* WidgetPackage = WidgetBP->GetOutermost();
    const bool bWasPackageDirty = WidgetPackage && WidgetPackage->IsDirty();
    FScopedTransaction Transaction(FText::FromString(TEXT("MCP: widget.rename_widget")));

    auto FailRename = [&](const FString& Message)
    {
        if (TargetWidget->GetFName() != OldFName)
        {
            TargetWidget->Rename(*OldFName.ToString(), nullptr,
                REN_DontCreateRedirectors | REN_NonTransactional);
        }
        Transaction.Cancel();
        if (WidgetPackage && !bWasPackageDirty)
        {
            WidgetPackage->SetDirtyFlag(false);
        }
        Ctx.SendError(ErrorCodes::ERR_RENAME_FAILED, Message);
        return true;
    };

    if (!TargetWidget->Rename(*NewName))
    {
        return FailRename(FString::Printf(TEXT("Widget '%s' could not be renamed to '%s'"),
            *OldName, *NewName));
    }

    // Verify the identity the tree exposes before touching GUID bookkeeping or marking the
    // blueprint modified. This also catches a rename that succeeds but is uniquified.
#if WITH_DEV_AUTOMATION_TESTS
    const bool bForceReadbackFailure =
        PinWrightWidgetHierarchyTestHooks::ConsumeForcePostRenameReadbackFailure();
#else
    const bool bForceReadbackFailure = false;
#endif
    UWidget* NewIdentity = WidgetBP->WidgetTree->FindWidget(NewFName);
    UWidget* OldIdentity = WidgetBP->WidgetTree->FindWidget(OldFName);
    if (bForceReadbackFailure || NewIdentity != TargetWidget || OldIdentity != nullptr)
    {
        return FailRename(FString::Printf(
            TEXT("Widget rename '%s' to '%s' did not produce a unique tree identity"),
            *OldName, *NewName));
    }

    RemoveWidgetVariableGuid(WidgetBP, OldFName);
    EnsureWidgetVariableGuid(WidgetBP, TargetWidget->GetFName());
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WidgetBP);

    return SendRenameSuccess();
}

// ---- widget.reparent_widget ----
REGISTER_RPC_HANDLER("widget.reparent_widget", "widget", "Move a child widget to a different parent panel within the same UWidgetBlueprint, preserving the widget itself and its descendants.",
    RPC_PARAMS(
        WidgetAssetPathParamReq(TEXT("Path to the widget blueprint")),
        RPC_PARAM_OPT("widgetName", "string", "Name of the widget to move (canonical; aliases: name, slotName, widget_name, targetName)"),
        RPC_PARAM_OPT("name", "string", "Alias for widgetName"),
        RPC_PARAM_OPT("slotName", "string", "Alias for widgetName"),
        RPC_PARAM_OPT("widget_name", "string", "Alias for widgetName"),
        RPC_PARAM_OPT("targetName", "string", "Alias for widgetName"),
        RPC_PARAM_REQ("newParent", "string", "Name of the new parent panel widget"),
        RPC_PARAM_OPT("placement", "object", "Placement object: {index}, {after}, or {before}. Defaults to appending to the new parent.")
    ))
{
    FString WidgetPath = Ctx.GetStringFirstOf(WidgetAssetPathParamNames());
    FString SlotName = Ctx.GetStringFirstOf({TEXT("widgetName"), TEXT("name"), TEXT("slotName"), TEXT("widget_name"), TEXT("targetName")});
    FString NewParent = Ctx.GetString(TEXT("newParent"));

    if (WidgetPath.IsEmpty() || SlotName.IsEmpty() || NewParent.IsEmpty())
    {
        Ctx.SendError(ErrorCodes::ERR_MISSING_PARAMETER, TEXT("Missing required parameters: widgetPath, widgetName, newParent (widgetName also accepts: name, slotName, widget_name, targetName)"));
        return true;
    }

    UWidgetBlueprint* WidgetBP = LoadWidgetBlueprint(WidgetPath);
    if (!WidgetBP || !WidgetBP->WidgetTree)
    {
        Ctx.SendError(ErrorCodes::ERR_NOT_FOUND, TEXT("Widget blueprint not found"));
        return true;
    }

    UWidget* TargetWidget = WidgetBP->WidgetTree->FindWidget(FName(*SlotName));
    if (!TargetWidget)
    {
        Ctx.SendError(ErrorCodes::ERR_NOT_FOUND, FString::Printf(TEXT("Widget '%s' not found"), *SlotName));
        return true;
    }

    UPanelWidget* NewParentWidget = Cast<UPanelWidget>(WidgetBP->WidgetTree->FindWidget(FName(*NewParent)));
    if (!NewParentWidget)
    {
        Ctx.SendError(ErrorCodes::ERR_NOT_FOUND, FString::Printf(TEXT("New parent '%s' not found or not a panel"), *NewParent));
        return true;
    }

    if (TargetWidget == NewParentWidget || IsDescendantWidget(TargetWidget, NewParentWidget))
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_PARENT, TEXT("Cannot reparent a widget into itself or its descendant"));
        return true;
    }

    const TSharedPtr<FJsonObject> Placement = Ctx.GetObject(TEXT("placement"));
    FString PlacementPeerName;
    const bool bPeerPlacement = Placement.IsValid() &&
        (Placement->TryGetStringField(TEXT("before"), PlacementPeerName) ||
         Placement->TryGetStringField(TEXT("after"), PlacementPeerName));
    if (bPeerPlacement && PlacementPeerName.Equals(SlotName, ESearchCase::IgnoreCase))
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_PLACEMENT, TEXT("Cannot place a widget before or after itself"));
        return true;
    }

    FString PlacementError;
    const int32 RequestedInsertIndex = ResolveWidgetInsertIndex(WidgetBP, NewParentWidget, nullptr,
        Placement, PlacementError);
    if (!PlacementError.IsEmpty())
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_PLACEMENT, PlacementError);
        return true;
    }

    FScopedTransaction Transaction(FText::FromString(TEXT("MCP: widget.reparent_widget")));
    WidgetBP->Modify();
    WidgetBP->WidgetTree->Modify();
    NewParentWidget->Modify();
    int32 InsertIndex = RequestedInsertIndex;
    UPanelWidget* OldParent = TargetWidget->GetParent();
    const int32 OldIndex = OldParent ? OldParent->GetChildIndex(TargetWidget) : INDEX_NONE;
    UPanelSlot* OldSlot = TargetWidget->Slot;
    if (OldParent)
    {
        OldParent->Modify();
        if (OldParent == NewParentWidget && bPeerPlacement && OldIndex != INDEX_NONE && OldIndex < InsertIndex)
        {
            --InsertIndex;
        }
        OldParent->RemoveChild(TargetWidget);
    }
    InsertIndex = FMath::Clamp(InsertIndex, 0, NewParentWidget->GetChildrenCount());
    // UE 5.3-5.5: UPanelWidget::InsertChildAt has no SlotTemplate overload (added in 5.6),
    // so the freshly-inserted slot starts at its class defaults. To match the 5.6+ behavior
    // of carrying the old slot's properties across the move, copy the old slot's UPROPERTY
    // values onto the new slot when both are the same slot class (e.g. both UCanvasPanelSlot),
    // preserving Content/Parent which InsertChildAt already wired up.
#if UE_VERSION_OLDER_THAN(5, 6, 0)
    UPanelSlot* NewSlot = NewParentWidget->InsertChildAt(InsertIndex, TargetWidget);
    if (!NewSlot)
    {
        if (OldParent)
        {
            OldParent->InsertChildAt(FMath::Clamp(OldIndex, 0, OldParent->GetChildrenCount()), TargetWidget);
        }
        Ctx.SendError(ErrorCodes::ERR_ATTACH_FAILED, TEXT("Failed to attach widget to new parent"));
        return true;
    }
    if (OldSlot && NewSlot->GetClass() == OldSlot->GetClass())
    {
        UWidget* const KeepContent = NewSlot->Content;
        UPanelWidget* const KeepParent = NewSlot->Parent;
        UEngine::FCopyPropertiesForUnrelatedObjectsParams CopyParams;
        CopyParams.bNotifyObjectReplacement = false;
        UEngine::CopyPropertiesForUnrelatedObjects(OldSlot, NewSlot, CopyParams);
        NewSlot->Content = KeepContent;
        NewSlot->Parent = KeepParent;
        NewSlot->SynchronizeProperties();
    }
#else
    if (!NewParentWidget->InsertChildAt(InsertIndex, TargetWidget, OldSlot))
    {
        if (OldParent)
        {
            OldParent->InsertChildAt(FMath::Clamp(OldIndex, 0, OldParent->GetChildrenCount()), TargetWidget, OldSlot);
        }
        Ctx.SendError(ErrorCodes::ERR_ATTACH_FAILED, TEXT("Failed to attach widget to new parent"));
        return true;
    }
#endif

    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WidgetBP);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetBoolField(TEXT("success"), true);
    Result->SetStringField(TEXT("widgetPath"), WidgetPath);
    Result->SetStringField(TEXT("widget"), SlotName);
    Result->SetStringField(TEXT("newParent"), NewParent);
    Result->SetNumberField(TEXT("insertIndex"), InsertIndex);

    Ctx.SendSuccess(Result);
    return true;
}
