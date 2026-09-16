// Copyright (c) 2026 Alexander Penkin. MIT License.


#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/ParamSpec.h"
#include "Handlers/UI/WidgetAuthoringUtils.h"
#include "Handlers/UI/WidgetGeometryResolver.h"
#include "ScopedTransaction.h"

#include "Blueprint/WidgetTree.h"
#include "Components/Widget.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "WidgetBlueprint.h"
#include "WidgetBlueprintEditor.h"

using namespace WidgetAuthoringHelpers;

namespace
{
    bool ResolveWidget(FHandlerContext& Ctx, const FString& WidgetPath, const FString& WidgetName,
        UWidgetBlueprint*& OutWidgetBP, UWidget*& OutWidget)
    {
        OutWidgetBP = LoadWidgetBlueprint(WidgetPath);
        if (!OutWidgetBP || !OutWidgetBP->WidgetTree)
        {
            Ctx.SendError(TEXT("NOT_FOUND"), TEXT("Widget blueprint not found or has no WidgetTree"));
            return false;
        }

        OutWidget = FindWidgetByName(OutWidgetBP, WidgetName);
        if (!OutWidget)
        {
            Ctx.SendError(TEXT("NOT_FOUND"), FString::Printf(TEXT("Widget not found: %s"), *WidgetName));
            return false;
        }

        return true;
    }

    TSharedPtr<FJsonObject> MakeDesignerVisibilityResult(const FString& WidgetPath, const FString& WidgetName,
        UWidget* Widget)
    {
        TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
        Result->SetBoolField(TEXT("success"), true);
        Result->SetStringField(TEXT("widgetPath"), WidgetPath);
        Result->SetStringField(TEXT("widgetName"), WidgetName);
        Result->SetBoolField(TEXT("visible"), !Widget->bHiddenInDesigner);
        Result->SetBoolField(TEXT("hiddenInDesigner"), Widget->bHiddenInDesigner);
        Result->SetStringField(TEXT("visibility"), VisibilityToString(Widget->GetVisibility()));
        return Result;
    }

    bool UpdateOpenPreviewDesignerVisibility(UWidgetBlueprint* WidgetBP, UWidget* TemplateWidget, bool bHiddenInDesigner)
    {
        FWidgetBlueprintEditor* BPEditor = FWidgetGeometryResolver::FindWidgetBlueprintEditor(
            WidgetBP, false);
        if (!BPEditor)
        {
            return false;
        }

        UUserWidget* Preview = BPEditor->GetPreview();
        UWidget* PreviewWidget = Preview
            ? Preview->GetWidgetFromName(TemplateWidget->GetFName())
            : nullptr;
        if (!PreviewWidget)
        {
            return false;
        }

        PreviewWidget->Modify();
        PreviewWidget->bHiddenInDesigner = bHiddenInDesigner;
        BPEditor->InvalidatePreview(true);
        return true;
    }
}

REGISTER_RPC_HANDLER("widget.get_designer_visibility", "widget",
    "Get the editor-only Widget Blueprint hierarchy eye visibility for a widget-tree child.",
    RPC_PARAMS(
        RPC_PARAM_REQ("widgetPath", "path", "Path to the widget blueprint."),
        RPC_PARAM_REQ("widgetName", "string", "Name of the target widget.")
    ))
{
    FString WidgetPath;
    FString WidgetName;
    if (!Ctx.RequireString(TEXT("widgetPath"), WidgetPath))
    {
        return true;
    }
    if (!Ctx.RequireString(TEXT("widgetName"), WidgetName))
    {
        return true;
    }

    UWidgetBlueprint* WidgetBP = nullptr;
    UWidget* Widget = nullptr;
    if (!ResolveWidget(Ctx, WidgetPath, WidgetName, WidgetBP, Widget))
    {
        return true;
    }

    Ctx.SendSuccess(MakeDesignerVisibilityResult(WidgetPath, WidgetName, Widget));
    return true;
}

REGISTER_RPC_HANDLER("widget.set_designer_visibility", "widget",
    "Set the editor-only Widget Blueprint hierarchy eye visibility for a widget-tree child without changing runtime Visibility.",
    RPC_PARAMS(
        RPC_PARAM_REQ("widgetPath", "path", "Path to the widget blueprint."),
        RPC_PARAM_REQ("widgetName", "string", "Name of the target widget."),
        RPC_PARAM_REQ("visible", "boolean", "True to show the widget in the Designer, false to hide it.")
    ))
{
    FString WidgetPath;
    FString WidgetName;
    bool bVisible = true;
    if (!Ctx.RequireString(TEXT("widgetPath"), WidgetPath))
    {
        return true;
    }
    if (!Ctx.RequireString(TEXT("widgetName"), WidgetName))
    {
        return true;
    }
    if (!Ctx.RequireBool(TEXT("visible"), bVisible))
    {
        return true;
    }

    UWidgetBlueprint* WidgetBP = nullptr;
    UWidget* Widget = nullptr;
    if (!ResolveWidget(Ctx, WidgetPath, WidgetName, WidgetBP, Widget))
    {
        return true;
    }

    const bool bOldHiddenInDesigner = Widget->bHiddenInDesigner;
    const bool bOldVisible = !bOldHiddenInDesigner;
    const ESlateVisibility OldRuntimeVisibility = Widget->GetVisibility();
    const bool bNewHiddenInDesigner = !bVisible;
    bool bPreviewUpdated = false;

    if (bOldHiddenInDesigner != bNewHiddenInDesigner)
    {
        FScopedTransaction Transaction(FText::FromString(TEXT("MCP: widget.set_designer_visibility")));
        WidgetBP->Modify();
        WidgetBP->WidgetTree->Modify();
        Widget->Modify();

        Widget->bHiddenInDesigner = bNewHiddenInDesigner;
        FBlueprintEditorUtils::MarkBlueprintAsModified(WidgetBP);
        bPreviewUpdated = UpdateOpenPreviewDesignerVisibility(WidgetBP, Widget, bNewHiddenInDesigner);
    }

    const ESlateVisibility NewRuntimeVisibility = Widget->GetVisibility();

    TSharedPtr<FJsonObject> Result = MakeDesignerVisibilityResult(WidgetPath, WidgetName, Widget);
    Result->SetBoolField(TEXT("oldVisible"), bOldVisible);
    Result->SetBoolField(TEXT("newVisible"), !Widget->bHiddenInDesigner);
    Result->SetBoolField(TEXT("oldHiddenInDesigner"), bOldHiddenInDesigner);
    Result->SetBoolField(TEXT("newHiddenInDesigner"), Widget->bHiddenInDesigner);
    Result->SetBoolField(TEXT("runtimeVisibilityUnchanged"), OldRuntimeVisibility == NewRuntimeVisibility);
    Result->SetBoolField(TEXT("assetDirty"), WidgetBP->GetOutermost() && WidgetBP->GetOutermost()->IsDirty());
    Result->SetBoolField(TEXT("previewUpdated"), bPreviewUpdated);
    Ctx.SendSuccess(Result);
    return true;
}
