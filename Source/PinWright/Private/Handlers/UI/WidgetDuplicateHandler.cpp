// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/UI/WidgetAuthoringUtils.h"
#include "ScopedTransaction.h"

#include "Blueprint/WidgetTree.h"
#include "Components/PanelWidget.h"
#include "Components/PanelSlot.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "WidgetBlueprint.h"
#include "Misc/EngineVersionComparison.h"

using namespace WidgetAuthoringHelpers;


namespace
{
    FString MakeWidgetNameKey(const FString& Name)
    {
        return Name.ToLower();
    }

    void SeedWidgetReservedNames(UWidgetBlueprint* WidgetBP, TSet<FString>& ReservedNames)
    {
        if (!WidgetBP || !WidgetBP->WidgetTree)
        {
            return;
        }

        WidgetBP->WidgetTree->ForEachWidget([&ReservedNames](UWidget* Widget)
        {
            if (Widget)
            {
                ReservedNames.Add(MakeWidgetNameKey(Widget->GetName()));
            }
        });
    }

    FString GenerateWidgetDuplicateName(UWidgetBlueprint* WidgetBP, UClass* WidgetClass,
        const TSet<FString>& ReservedNames, const FString& SourceName)
    {
        const FString BaseName = FString::Printf(TEXT("%s_Copy"), *SourceName);
        FString Candidate = MakeUniqueObjectName(WidgetBP->WidgetTree, WidgetClass, FName(*BaseName)).ToString();
        int32 Suffix = 1;
        while (ReservedNames.Contains(MakeWidgetNameKey(Candidate)))
        {
            const FString FallbackBase = FString::Printf(TEXT("%s_%d"), *BaseName, Suffix++);
            Candidate = MakeUniqueObjectName(WidgetBP->WidgetTree, WidgetClass, FName(*FallbackBase)).ToString();
        }
        return Candidate;
    }

    void AddMapping(TSharedPtr<FJsonObject> Mapping, UWidget* Source, UWidget* Target)
    {
        if (Mapping.IsValid() && Source && Target)
        {
            Mapping->SetStringField(Source->GetName(), Target->GetName());
        }
    }

    struct FWidgetDuplicateOptions
    {
        FString DesiredName;
        bool bDesiredNameIsExplicit = false;
        bool bDuplicateChildren = true;
        bool bCopyProperties = true;
        bool bCopySlot = true;
    };

    struct FWidgetDuplicateContext
    {
        TSharedPtr<FJsonObject> Mapping;
        TArray<FString>* Warnings = nullptr;
        TSet<FString> ReservedNames;
    };

    UWidget* DuplicateWidgetTree(UWidgetBlueprint* WidgetBP, UWidget* SourceWidget,
        const FWidgetDuplicateOptions& Options, FWidgetDuplicateContext& DuplicateContext, FString& OutError)
    {
        if (!WidgetBP || !WidgetBP->WidgetTree || !SourceWidget)
        {
            OutError = TEXT("Invalid source widget");
            return nullptr;
        }

        const FString TargetName = Options.DesiredName.IsEmpty()
            ? GenerateWidgetDuplicateName(WidgetBP, SourceWidget->GetClass(), DuplicateContext.ReservedNames, SourceWidget->GetName())
            : Options.DesiredName;

        const FString TargetNameKey = MakeWidgetNameKey(TargetName);
        if (DuplicateContext.ReservedNames.Contains(TargetNameKey) ||
            (Options.bDesiredNameIsExplicit && FindWidgetByName(WidgetBP, TargetName)))
        {
            OutError = FString::Printf(TEXT("Widget with name '%s' already exists"), *TargetName);
            return nullptr;
        }
        DuplicateContext.ReservedNames.Add(TargetNameKey);

        WidgetBP->WidgetTree->Modify();
        FString ConstructionError;
        UWidget* NewWidget = ConstructWidgetForAuthoring(WidgetBP, SourceWidget->GetClass(), FName(*TargetName), &ConstructionError);
        if (!NewWidget)
        {
            OutError = ConstructionError.IsEmpty()
                ? FString::Printf(TEXT("Failed to construct widget '%s'"), *TargetName)
                : ConstructionError;
            DuplicateContext.ReservedNames.Remove(TargetNameKey);
            return nullptr;
        }

        NewWidget->Modify();
        if (Options.bCopyProperties)
        {
            CopyMatchingProperties(SourceWidget, NewWidget);
        }
        NewWidget->bIsVariable = SourceWidget->bIsVariable;
        AddMapping(DuplicateContext.Mapping, SourceWidget, NewWidget);

        UPanelWidget* SourcePanel = Cast<UPanelWidget>(SourceWidget);
        UPanelWidget* NewPanel = Cast<UPanelWidget>(NewWidget);
        if (Options.bDuplicateChildren && SourcePanel)
        {
            SourcePanel->Modify();
            if (!NewPanel)
            {
                if (DuplicateContext.Warnings)
                {
                    DuplicateContext.Warnings->Add(FString::Printf(TEXT("Widget '%s' is not a panel; children were not duplicated"), *NewWidget->GetName()));
                }
            }
            else
            {
                NewPanel->Modify();
                for (int32 ChildIndex = 0; ChildIndex < SourcePanel->GetChildrenCount(); ++ChildIndex)
                {
                    UWidget* SourceChild = SourcePanel->GetChildAt(ChildIndex);
                    if (!SourceChild)
                    {
                        continue;
                    }

                    FString ChildError;
                    FWidgetDuplicateOptions ChildOptions = Options;
                    ChildOptions.DesiredName.Reset();
                    ChildOptions.bDesiredNameIsExplicit = false;
                    UWidget* NewChild = DuplicateWidgetTree(WidgetBP, SourceChild, ChildOptions, DuplicateContext, ChildError);
                    if (!NewChild)
                    {
                        if (DuplicateContext.Warnings)
                        {
                            DuplicateContext.Warnings->Add(ChildError);
                        }
                        continue;
                    }

                    // UE 5.4/5.5: UPanelWidget::AddChild has no SlotTemplate overload;
                    // bCopySlot degrades to a default slot on those versions. The
                    // template-driven slot copy is available on UE 5.6+.
#if UE_VERSION_OLDER_THAN(5, 6, 0)
                    UPanelSlot* NewSlot = NewPanel->AddChild(NewChild);
#else
                    UPanelSlot* NewSlot = Options.bCopySlot
                        ? NewPanel->AddChild(NewChild, SourceChild->Slot)
                        : NewPanel->AddChild(NewChild);
#endif
                    if (!NewSlot)
                    {
                        if (DuplicateContext.Warnings)
                        {
                            DuplicateContext.Warnings->Add(FString::Printf(TEXT("Failed to attach duplicated child '%s' under '%s'"),
                                *NewChild->GetName(), *NewPanel->GetName()));
                        }
                        DiscardConstructedWidgetForAuthoring(WidgetBP, NewChild);
                        continue;
                    }
                    if (Options.bCopySlot && SourceChild->Slot && NewSlot)
                    {
                        NewSlot->Modify();
                        CopyMatchingProperties(SourceChild->Slot, NewSlot);
                    }
                }
            }
        }

        if (NewWidget->bIsVariable)
        {
            EnsureWidgetVariableGuid(WidgetBP, NewWidget->GetFName());
        }
        else
        {
            RemoveWidgetVariableGuid(WidgetBP, NewWidget->GetFName());
        }
        return NewWidget;
    }
}

REGISTER_RPC_HANDLER("widget.duplicate", "widget", "Duplicate a widget-tree child, optionally copying its subtree, authored properties, slot data, and placement.",
    RPC_PARAMS(
        RPC_PARAM_REQ("widgetPath", "path", "Path to the widget blueprint."),
        RPC_PARAM_OPT("sourceName", "string", "Name of the source widget to duplicate."),
        RPC_PARAM_OPT("widgetName", "string", "Alias for sourceName."),
        RPC_PARAM_REQ("name", "string", "Name for the duplicated root widget."),
        RPC_PARAM_OPT("parentName", "string", "Target parent panel; defaults to the source widget's current parent."),
        RPC_PARAM_OPT("placement", "object", "Placement object: {index}, {after}, or {before}. Defaults to after source when duplicating under the same parent."),
        RPC_PARAM_OPT("duplicateChildren", "boolean", "Whether to recursively duplicate panel children. Defaults true."),
        RPC_PARAM_OPT("copyProperties", "boolean", "Whether to copy safe editable widget properties. Defaults true."),
        RPC_PARAM_OPT("copySlot", "boolean", "Whether to copy compatible slot properties. Defaults true.")
    ))
{
    FString WidgetPath;
    if (!Ctx.RequireString(TEXT("widgetPath"), WidgetPath))
    {
        return true;
    }

    const FString SourceName = Ctx.GetStringFirstOf({TEXT("sourceName"), TEXT("widgetName")});
    if (SourceName.IsEmpty())
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("sourceName is required"));
        return true;
    }

    FString DuplicateName;
    if (!Ctx.RequireString(TEXT("name"), DuplicateName))
    {
        return true;
    }

    UWidgetBlueprint* WidgetBP = LoadWidgetBlueprint(WidgetPath);
    if (!WidgetBP || !WidgetBP->WidgetTree)
    {
        Ctx.SendError(TEXT("NOT_FOUND"), TEXT("Widget blueprint not found or has no WidgetTree"));
        return true;
    }

    UWidget* SourceWidget = FindWidgetByName(WidgetBP, SourceName);
    if (!SourceWidget)
    {
        Ctx.SendError(TEXT("NOT_FOUND"), FString::Printf(TEXT("Source widget not found: %s"), *SourceName));
        return true;
    }

    UPanelWidget* TargetParent = nullptr;
    const FString ParentName = Ctx.GetString(TEXT("parentName"));
    if (!ParentName.IsEmpty())
    {
        UWidget* ParentWidget = FindWidgetByName(WidgetBP, ParentName);
        TargetParent = Cast<UPanelWidget>(ParentWidget);
        if (!TargetParent)
        {
            Ctx.SendError(TEXT("INVALID_PARENT"), FString::Printf(TEXT("Parent widget '%s' is not a panel"), *ParentName));
            return true;
        }
    }
    else
    {
        TargetParent = SourceWidget->GetParent();
    }

    if (!TargetParent)
    {
        Ctx.SendError(TEXT("INVALID_PARENT"), TEXT("Source widget has no panel parent; provide parentName"));
        return true;
    }

    if (SourceWidget == TargetParent || IsDescendantWidget(SourceWidget, TargetParent))
    {
        Ctx.SendError(TEXT("INVALID_PARENT"), TEXT("Cannot duplicate a widget into itself or its descendant"));
        return true;
    }

    FString PlacementError;
    const int32 InsertIndex = ResolveWidgetInsertIndex(WidgetBP, TargetParent, SourceWidget, Ctx.GetObject(TEXT("placement")), PlacementError);
    if (!PlacementError.IsEmpty())
    {
        Ctx.SendError(TEXT("INVALID_PLACEMENT"), PlacementError);
        return true;
    }

    const bool bDuplicateChildren = Ctx.GetBool(TEXT("duplicateChildren"), true);
    const bool bCopyProperties = Ctx.GetBool(TEXT("copyProperties"), true);
    const bool bCopySlot = Ctx.GetBool(TEXT("copySlot"), true);

    FScopedTransaction Transaction(FText::FromString(TEXT("MCP: widget.duplicate")));
    WidgetBP->Modify();
    WidgetBP->WidgetTree->Modify();
    TargetParent->Modify();
    if (UPanelWidget* SourcePanel = Cast<UPanelWidget>(SourceWidget))
    {
        SourcePanel->Modify();
    }
    if (UPanelWidget* SourceParent = SourceWidget->GetParent())
    {
        SourceParent->Modify();
    }

    TSharedPtr<FJsonObject> Mapping = MakeShared<FJsonObject>();
    TArray<FString> Warnings;
    FString DuplicateError;
    FWidgetDuplicateOptions DuplicateOptions;
    DuplicateOptions.DesiredName = DuplicateName;
    DuplicateOptions.bDesiredNameIsExplicit = true;
    DuplicateOptions.bDuplicateChildren = bDuplicateChildren;
    DuplicateOptions.bCopyProperties = bCopyProperties;
    DuplicateOptions.bCopySlot = bCopySlot;
    FWidgetDuplicateContext DuplicateContext;
    DuplicateContext.Mapping = Mapping;
    DuplicateContext.Warnings = &Warnings;
    SeedWidgetReservedNames(WidgetBP, DuplicateContext.ReservedNames);
    UWidget* NewWidget = DuplicateWidgetTree(WidgetBP, SourceWidget, DuplicateOptions, DuplicateContext, DuplicateError);
    if (!NewWidget)
    {
        Ctx.SendError(TEXT("DUPLICATE_FAILED"), DuplicateError);
        return true;
    }

    // UE 5.4/5.5: UPanelWidget::InsertChildAt has no SlotTemplate overload; the slot
    // template is reused only on UE 5.6+. The slot-property copy below (gated on
    // bCopySlot) still runs on every version as a back-stop.
#if UE_VERSION_OLDER_THAN(5, 6, 0)
    UPanelSlot* NewSlot = TargetParent->InsertChildAt(InsertIndex, NewWidget);
#else
    UPanelSlot* NewSlot = bCopySlot
        ? TargetParent->InsertChildAt(InsertIndex, NewWidget, SourceWidget->Slot)
        : TargetParent->InsertChildAt(InsertIndex, NewWidget);
#endif
    if (!NewSlot)
    {
        DiscardConstructedWidgetForAuthoring(WidgetBP, NewWidget);
        Ctx.SendError(TEXT("ATTACH_FAILED"), TEXT("Failed to attach duplicated widget to target parent"));
        return true;
    }
    if (bCopySlot && SourceWidget->Slot)
    {
        NewSlot->Modify();
        CopyMatchingProperties(SourceWidget->Slot, NewSlot);
    }

    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WidgetBP);

    TArray<TSharedPtr<FJsonValue>> WarningValues;
    for (const FString& Warning : Warnings)
    {
        WarningValues.Add(MakeShared<FJsonValueString>(Warning));
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetBoolField(TEXT("success"), true);
    Result->SetStringField(TEXT("sourceName"), SourceWidget->GetName());
    Result->SetStringField(TEXT("widgetName"), NewWidget->GetName());
    Result->SetStringField(TEXT("widgetClass"), NewWidget->GetClass()->GetName());
    Result->SetObjectField(TEXT("mapping"), Mapping);
    Result->SetArrayField(TEXT("warnings"), WarningValues);
    Result->SetBoolField(TEXT("compileRequired"), true);
    Result->SetBoolField(TEXT("saveRequired"), true);
    Ctx.SendSuccess(Result);
    return true;
}
