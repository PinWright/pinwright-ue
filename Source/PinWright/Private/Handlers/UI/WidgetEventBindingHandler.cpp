// Copyright (c) 2026 Alexander Penkin. MIT License.


#include "Handlers/HandlerRegistration.h"
#include "Handlers/ParamSpec.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/Blueprint/BlueprintHandlerUtils.h"
#include "Handlers/UI/WidgetAuthoringUtils.h"

#include "K2Node_ComponentBoundEvent.h"
#include "WidgetBlueprint.h"
#include "EdGraph/EdGraph.h"
#include "UObject/UnrealType.h"

using namespace WidgetAuthoringHelpers;

REGISTER_RPC_HANDLER("widget.bind_event", "widget",
    "Verify/refresh an existing widget event binding. Create the BndEvt node first via compile_bpir with 'entry widget_event <WidgetName>.<EventName>() { ... }'",
    RPC_PARAMS(
        RPC_PARAM_REQ("blueprintPath", "path", "Widget blueprint asset path"),
        RPC_PARAM_REQ("widgetName", "string", "Widget variable name (e.g. AcceptButton)"),
        RPC_PARAM_REQ("eventName", "string", "Delegate name (e.g. OnClicked)")
    ))
{
    FString BlueprintPath, WidgetName, EventName;
    if (!Ctx.RequireString(TEXT("blueprintPath"), BlueprintPath)) return true;
    if (!Ctx.RequireString(TEXT("widgetName"), WidgetName)) return true;
    if (!Ctx.RequireString(TEXT("eventName"), EventName)) return true;

    UWidgetBlueprint* WidgetBP = LoadWidgetBlueprint(BlueprintPath);
    if (!WidgetBP)
    {
        Ctx.SendError(TEXT("NOT_FOUND"), TEXT("Widget blueprint not found"));
        return true;
    }

    UWidget* Widget = FindWidgetByName(WidgetBP, WidgetName);
    if (!Widget)
    {
        Ctx.SendError(TEXT("WIDGET_NOT_FOUND"),
            FString::Printf(TEXT("Widget '%s' not found in blueprint"), *WidgetName));
        return true;
    }

    UK2Node_ComponentBoundEvent* FoundNode = nullptr;
    for (UEdGraph* Graph : WidgetBP->UbergraphPages)
    {
        if (!Graph) continue;
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            UK2Node_ComponentBoundEvent* CompEvent = Cast<UK2Node_ComponentBoundEvent>(Node);
            if (CompEvent
                && CompEvent->ComponentPropertyName == FName(*WidgetName)
                && CompEvent->DelegatePropertyName == FName(*EventName))
            {
                FoundNode = CompEvent;
                break;
            }
        }
        if (FoundNode) break;
    }

    if (!FoundNode)
    {
        Ctx.SendError(TEXT("BPIR_REQUIRED"),
            FString::Printf(
                TEXT("No BndEvt node for %s.%s on %s. This tool only verifies existing bindings. "
                     "To create one, call compile_bpir with body \"entry widget_event %s.%s() { ... }\" first."),
                *WidgetName, *EventName, *BlueprintPath, *WidgetName, *EventName));
        return true;
    }

    // Compile only if needed — compilation is expensive
    BlueprintHandlerUtils::FBlueprintCompileDiagnostics CompileDiagnostics;
    bool bCompileAttempted = false;
    if (WidgetBP->Status != BS_UpToDate)
    {
        CompileDiagnostics = BlueprintHandlerUtils::CompileBlueprintWithDiagnostics(WidgetBP);
        bCompileAttempted = true;
    }

    bool bDelegateValid = WidgetBP->GeneratedClass
        && FindFProperty<FObjectProperty>(WidgetBP->GeneratedClass, FoundNode->ComponentPropertyName)
        && FoundNode->GetTargetDelegateProperty() != nullptr;

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("widgetName"), WidgetName);
    Result->SetStringField(TEXT("eventName"), EventName);
    Result->SetStringField(TEXT("customFunctionName"), FoundNode->CustomFunctionName.ToString());
    Result->SetBoolField(TEXT("isDelegateValid"), bDelegateValid);
    Result->SetStringField(TEXT("nodeId"), FoundNode->NodeGuid.ToString());
    if (bCompileAttempted)
    {
        BlueprintHandlerUtils::AddCompileDiagnosticsToJson(CompileDiagnostics, Result);
    }
    Ctx.SendSuccess(Result);
    return true;
}
