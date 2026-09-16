// Copyright (c) 2026 Alexander Penkin. MIT License.

// WidgetAddHandler.cpp
// Unified widget.add handler — replaces all 27 widget.add_* methods

#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/UI/WidgetAuthoringUtils.h"
#include "Handlers/UI/WidgetHandlerUtils.h"
#include "ScopedTransaction.h"

#include "Blueprint/WidgetTree.h"
#include "WidgetBlueprint.h"
#include "Components/Widget.h"
#include "Kismet2/BlueprintEditorUtils.h"

using namespace WidgetAuthoringHelpers;
using namespace WidgetHandlerUtils;


REGISTER_RPC_HANDLER("widget.add", "widget", "Add any widget type to a widget blueprint by class name",
    RPC_PARAMS(
        WidgetAssetPathParamReq(TEXT("Path to the widget blueprint")),
        RPC_PARAM_REQ("type", "classref", "Widget class name (e.g. TextBlock, Button, CanvasPanel, HorizontalBox)"),
        RPC_PARAM_REQ("name", "string", "Name for the new widget (aliases: widgetName, slotName)"),
        RPC_PARAM_OPT("widgetName", "string", "Alias for name"),
        RPC_PARAM_OPT("slotName", "string", "Alias for name"),
        RPC_PARAM_OPT("parentName", "string", "Name of parent panel widget (uses root if omitted)"),
        RPC_PARAM_OPT("placement", "object", "Placement object: {index}, {after}, or {before}. Defaults to appending to the parent.")
    ))
{
    FString WidgetPath = Ctx.GetStringFirstOf(WidgetAssetPathParamNames());
    if (WidgetPath.IsEmpty())
    {
        Ctx.SendError(TEXT("MISSING_PARAMETER"), TEXT("Missing required parameter: widgetPath"));
        return true;
    }

    FString Type = Ctx.GetString(TEXT("type"));
    if (Type.IsEmpty())
    {
        Ctx.SendError(TEXT("MISSING_PARAMETER"), TEXT("Missing required parameter: type"));
        return true;
    }

    FString Name = Ctx.GetStringFirstOf({TEXT("name"), TEXT("widgetName"), TEXT("slotName")});
    if (Name.IsEmpty())
    {
        Ctx.SendError(TEXT("MISSING_PARAMETER"), TEXT("Missing required parameter 'name' (also accepts: widgetName, slotName)"));
        return true;
    }

    FString ParentName = Ctx.GetString(TEXT("parentName"));
    TSharedPtr<FJsonObject> Placement = Ctx.GetObject(TEXT("placement"));

    UWidgetBlueprint* WidgetBP = LoadWidgetBlueprint(WidgetPath);
    if (!WidgetBP)
    {
        Ctx.SendError(TEXT("NOT_FOUND"), TEXT("Widget blueprint not found"));
        return true;
    }

    UClass* WidgetClass = ResolveWidgetClass(Type);
    if (!WidgetClass)
    {
        Ctx.SendError(TEXT("INVALID_CLASS"), FString::Printf(TEXT("Widget class not found: %s"), *Type));
        return true;
    }

    if (!WidgetClass->IsChildOf(UWidget::StaticClass()))
    {
        Ctx.SendError(TEXT("INVALID_CLASS"), FString::Printf(TEXT("Class is not a UWidget subclass: %s"), *Type));
        return true;
    }

    if (!WidgetBP->WidgetTree)
    {
        Ctx.SendError(TEXT("INVALID_STATE"), TEXT("Widget blueprint has no WidgetTree"));
        return true;
    }

    if (WidgetBP->WidgetTree->FindWidget(FName(*Name)))
    {
        Ctx.SendError(TEXT("DUPLICATE_NAME"), FString::Printf(TEXT("Widget with name '%s' already exists"), *Name));
        return true;
    }

    FScopedTransaction Transaction(FText::FromString(TEXT("MCP: widget.add")));
    FString ConstructionError;
    UWidget* Widget = ConstructWidgetForAuthoring(WidgetBP, WidgetClass, FName(*Name), &ConstructionError);
    if (!Widget)
    {
        Ctx.SendError(TEXT("CREATION_ERROR"), ConstructionError.IsEmpty()
            ? FString::Printf(TEXT("Failed to construct widget of type: %s"), *Type)
            : ConstructionError);
        return true;
    }

    FString AttachError;
    FString AttachErrorCode;
    int32 InsertIndex = INDEX_NONE;
    if (!AttachToParentOrRoot(WidgetBP, Widget, ParentName, Placement, &InsertIndex, &AttachErrorCode, &AttachError))
    {
        DiscardConstructedWidgetForAuthoring(WidgetBP, Widget);
        Ctx.SendError(AttachErrorCode.IsEmpty() ? FString(TEXT("ATTACH_FAILED")) : AttachErrorCode, AttachError);
        return true;
    }

    // Promote to a blueprint variable so BPIR/$Name resolution and graph references
    // see this widget after the next compile. WidgetXmlImportHandler does the same.
    Widget->bIsVariable = true;
    EnsureWidgetVariableGuid(WidgetBP, Widget->GetFName());
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WidgetBP);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetBoolField(TEXT("success"), true);
    Result->SetStringField(TEXT("message"), FString::Printf(TEXT("Added %s widget"), *Type));
    Result->SetStringField(TEXT("widgetName"), Widget->GetName());
    Result->SetStringField(TEXT("widgetClass"), WidgetClass->GetName());
    Result->SetNumberField(TEXT("insertIndex"), InsertIndex);
    Result->SetBoolField(TEXT("requiresCompile"), true);

    Ctx.SendSuccess(Result);
    return true;
}
