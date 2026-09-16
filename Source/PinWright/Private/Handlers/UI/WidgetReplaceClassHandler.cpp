// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/UI/WidgetAuthoringUtils.h"
#include "ScopedTransaction.h"

#include "Blueprint/WidgetTree.h"
#include "WidgetBlueprint.h"
#include "Components/Widget.h"
#include "Components/PanelWidget.h"
#include "Kismet2/BlueprintEditorUtils.h"

using namespace WidgetAuthoringHelpers;


REGISTER_RPC_HANDLER("widget.replace_class", "widget",
    "Replace a widget's class while preserving its children and (for non-root) its parent slot.",
    RPC_PARAMS(
        RPC_PARAM_REQ("widgetPath", "path", "Path to the widget blueprint"),
        RPC_PARAM_REQ("targetName", "string", "Name of the widget to replace; may be the root"),
        RPC_PARAM_REQ("newType", "classref",
            "Target widget class name (e.g. 'Overlay', 'VerticalBox'). "
            "Panel subclasses preserve children; non-panel leaf swaps (e.g. Image->Border) are allowed but drop any children."),
        RPC_PARAM_DEF("preserveProperties", "bool",
            "Copy FProperty values from the old widget to the new one where types match.", "true")
    ))
{
    FString WidgetPath = Ctx.GetString(TEXT("widgetPath"));
    FString TargetName = Ctx.GetString(TEXT("targetName"));
    FString NewType = Ctx.GetString(TEXT("newType"));

    if (WidgetPath.IsEmpty() || TargetName.IsEmpty() || NewType.IsEmpty())
    {
        Ctx.SendError(TEXT("MISSING_PARAMETER"),
            TEXT("Missing required parameters: widgetPath, targetName, newType"));
        return true;
    }

    const bool bPreserveProperties = Ctx.GetBool(TEXT("preserveProperties"), true);

    UWidgetBlueprint* WidgetBP = LoadWidgetBlueprint(WidgetPath);
    if (!WidgetBP || !WidgetBP->WidgetTree)
    {
        Ctx.SendError(TEXT("NOT_FOUND"), TEXT("Widget blueprint not found"));
        return true;
    }

    UWidget* OldTarget = FindWidgetByName(WidgetBP, TargetName);
    if (!OldTarget)
    {
        Ctx.SendError(TEXT("NOT_FOUND"),
            FString::Printf(TEXT("Widget '%s' not found"), *TargetName));
        return true;
    }

    UClass* NewClass = ResolveWidgetClass(NewType);
    if (!NewClass)
    {
        Ctx.SendError(TEXT("INVALID_CLASS"),
            FString::Printf(TEXT("Widget class not found: %s"), *NewType));
        return true;
    }

    if (!NewClass->IsChildOf(UWidget::StaticClass()))
    {
        Ctx.SendError(TEXT("INVALID_CLASS"),
            FString::Printf(TEXT("Class is not a UWidget subclass: %s"), *NewType));
        return true;
    }

    const FString OldClassName = OldTarget->GetClass()->GetName();

    FScopedTransaction Transaction(FText::FromString(TEXT("MCP: widget.replace_class")));

    FString SwapError;
    bool bWasRoot = false;
    UWidget* NewWidget = ReplaceWidgetClass(WidgetBP, OldTarget, NewClass,
        bPreserveProperties, bWasRoot, &SwapError);
    if (!NewWidget)
    {
        Ctx.SendError(TEXT("REPLACE_FAILED"),
            SwapError.IsEmpty() ? TEXT("Failed to replace widget class") : SwapError);
        return true;
    }

    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WidgetBP);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetBoolField(TEXT("success"), true);
    Result->SetBoolField(TEXT("requiresCompile"), true);
    Result->SetStringField(TEXT("widgetPath"), WidgetPath);
    Result->SetStringField(TEXT("targetName"), TargetName);
    Result->SetStringField(TEXT("oldClass"), OldClassName);
    Result->SetStringField(TEXT("newClass"), NewClass->GetName());
    Result->SetBoolField(TEXT("wasRoot"), bWasRoot);

    Ctx.SendSuccess(Result);
    return true;
}
