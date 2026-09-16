// Copyright (c) 2026 Alexander Penkin. MIT License.

// WidgetBindHandler.cpp
// Unified widget.bind RPC handler replacing all widget.bind_* methods


#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/ErrorCodes.h"
#include "Handlers/UI/WidgetAuthoringUtils.h"
#include "Handlers/UI/WidgetBindingUtils.h"
#include "ScopedTransaction.h"
#include "PinWrightHelpers.h"

#include "WidgetBlueprint.h"
#include "Blueprint/WidgetTree.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/CompilerResultsLog.h"
#include "Logging/TokenizedMessage.h"
#include "UObject/UnrealType.h"

using namespace WidgetAuthoringHelpers;
using namespace WidgetBindingHelpers;

namespace
{
    // Names the spelling that WOULD have resolved, when the requested one is a near miss.
    FString BuildWidgetBindNameSteer(const FString& RequestedName,
        const TArray<FString>& PropertyNames, const TArray<FString>& EventNames)
    {
        // The dominant miss: the "Event" suffix dropped off a bindable event, e.g.
        // "OnMouseButtonDown" for UBorder::OnMouseButtonDownEvent.
        for (const FString& EventName : EventNames)
        {
            if (EventName.Equals(RequestedName + TEXT("Event"), ESearchCase::IgnoreCase))
            {
                return FString::Printf(TEXT(" Did you mean '%s'?"), *EventName);
            }
        }
        // The inverse: a property-binding delegate named in full, e.g. "TextDelegate" for "Text".
        for (const FString& PropertyName : PropertyNames)
        {
            if (RequestedName.Equals(PropertyName + TEXT("Delegate"), ESearchCase::IgnoreCase))
            {
                return FString::Printf(TEXT(" Did you mean '%s'?"), *PropertyName);
            }
        }
        return FString();
    }
}

REGISTER_RPC_HANDLER("widget.bind", "widget",
    "Bind a widget property or bindable event to a handler function on the widget blueprint. "
    "propertyName must be a name UMG resolves: a property stem whose class has a '<stem>Delegate' "
    "delegate, or a bindable event delegate verbatim. Multicast events (OnClicked and friends) are "
    "not property bindings - create those with blueprint.compile_bpir 'entry widget_event ...'.",
    RPC_PARAMS(
        RPC_PARAM_REQ("widgetPath", "path", "Path to the widget blueprint"),
        RPC_PARAM_REQ("widgetName", "string", "Name of the target widget"),
        RPC_PARAM_REQ("propertyName", "string", "Bindable property stem (Text, Visibility, ToolTipText) or bindable event delegate (OnMouseButtonDownEvent)"),
        RPC_PARAM_REQ("functionName", "string", "Name of binding function")
    ))
{
    FString WidgetPath, WidgetName, PropertyName, FunctionName;
    if (!Ctx.RequireString(TEXT("widgetPath"), WidgetPath)) return true;
    if (!Ctx.RequireString(TEXT("widgetName"), WidgetName)) return true;
    if (!Ctx.RequireString(TEXT("propertyName"), PropertyName)) return true;
    if (!Ctx.RequireString(TEXT("functionName"), FunctionName)) return true;

    UWidgetBlueprint* WidgetBP = LoadWidgetBlueprint(WidgetPath);
    if (!WidgetBP || !WidgetBP->WidgetTree)
    {
        Ctx.SendError(ErrorCodes::ERR_NOT_FOUND, TEXT("Widget blueprint not found"));
        return true;
    }

    UWidget* TargetWidget = FindWidgetByName(WidgetBP, WidgetName);
    if (!TargetWidget)
    {
        Ctx.SendError(ErrorCodes::ERR_NOT_FOUND, FString::Printf(TEXT("Widget '%s' not found"), *WidgetName));
        return true;
    }

    UClass* WidgetClass = TargetWidget->GetClass();
    const FString WidgetClassName = WidgetClass->GetName();

    // Resolve before writing anything. A name that reaches neither of UMG's two lookups binds to
    // nothing at runtime, and neither the widget compiler nor widget.export_xml can tell the dead
    // record apart from a live one, so the only place it is still visible is here.
    FWidgetBindingTarget Target;
    if (!ResolveWidgetBindingTarget(WidgetClass, PropertyName, Target))
    {
        TArray<FString> BindableProperties, BindableEvents;
        CollectBindableNames(WidgetClass, BindableProperties, BindableEvents);

        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("widgetName"), WidgetName);
        Payload->SetStringField(TEXT("widgetClass"), WidgetClassName);
        Payload->SetArrayField(TEXT("bindableProperties"), EmitStringArray(BindableProperties));
        Payload->SetArrayField(TEXT("bindableEvents"), EmitStringArray(BindableEvents));

        // A multicast delegate is a real event, just not one the Bindings array can carry:
        // UWidgetBlueprintGeneratedClass::InitializeBindingsStatic only resolves FDelegateProperty.
        if (FindFProperty<FMulticastDelegateProperty>(WidgetClass, *PropertyName))
        {
            Ctx.SendError(ErrorCodes::ERR_WIDGET_BINDING_IS_MULTICAST_EVENT,
                FString::Printf(
                    TEXT("'%s' on %s is a multicast event, not a property binding. widget.bind writes ")
                    TEXT("the UWidgetBlueprint Bindings array, which UMG resolves to single-cast ")
                    TEXT("delegates only. Create it with blueprint.compile_bpir: ")
                    TEXT("entry widget_event %s.%s() { ... }"),
                    *PropertyName, *WidgetClassName, *WidgetName, *PropertyName),
                Payload);
            return true;
        }

        Ctx.SendError(ErrorCodes::ERR_WIDGET_BINDING_NAME_UNRESOLVED,
            FString::Printf(
                TEXT("'%s' is not bindable on %s. UMG looks up '%sDelegate' then '%s'; neither is a ")
                TEXT("bindable delegate on this class.%s bindableProperties / bindableEvents list the ")
                TEXT("names that resolve."),
                *PropertyName, *WidgetClassName, *PropertyName, *PropertyName,
                *BuildWidgetBindNameSteer(PropertyName, BindableProperties, BindableEvents)),
            Payload);
        return true;
    }

    const FString DelegateName = Target.DelegateProperty->GetName();

    // No bIsVariable gate here, deliberately. UWidgetBlueprintGeneratedClass::InitializeBindingsStatic
    // resolves ObjectName through the generated class's object-property map, but the widget compiler
    // generates a hidden variable for any widget a binding names - bShouldGenerateVariable is
    // `bIsVariable || IsA<UNamedSlot>() || Bindings.ContainsByPredicate(ObjectName == GetName())`
    // (WidgetBlueprintCompiler.cpp, present unchanged in 5.3-5.8). So writing the record is what
    // creates the property the runtime lookup needs; refusing a non-variable widget would break
    // widget.import_xml's IsVariable="false" output for no gain.

    FScopedTransaction Transaction(FText::FromString(TEXT("MCP: widget.bind")));

    bool bFunctionCreated = false;
    FString Error;
    if (!EnsureFunctionGraph(WidgetBP, Target.DelegateProperty->SignatureFunction, FunctionName,
            /*bPureFunction=*/!Target.bIsEvent, bFunctionCreated, Error))
    {
        Ctx.SendError(ErrorCodes::ERR_BINDING_FAILED, Error);
        return true;
    }

    // Regenerates the skeleton so the handler function exists on SkeletonGeneratedClass carrying its
    // pure flag - both the guid lookup and the validity check below read it off that class.
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WidgetBP);

    const FDelegateEditorBinding Candidate =
        MakeFunctionBinding(WidgetBP, WidgetName, Target.PropertyName, FunctionName);

    // The engine's own check rather than a re-derivation of it: it repeats the delegate lookup,
    // resolves FunctionName on the generated class, and compares signatures (and purity, for a
    // property binding). Nothing on the write path can make this true.
    FCompilerResultsLog BindingLog;
    BindingLog.bSilentMode = true;
    const bool bResolves = Candidate.IsBindingValid(WidgetBP->SkeletonGeneratedClass, WidgetBP, BindingLog);
    if (!bResolves)
    {
        TArray<TSharedPtr<FJsonValue>> CompilerMessages;
        for (const TSharedRef<FTokenizedMessage>& Message : BindingLog.Messages)
        {
            CompilerMessages.Add(MakeShared<FJsonValueString>(Message->ToText().ToString()));
        }

        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("delegateProperty"), DelegateName);
        Payload->SetBoolField(TEXT("requiresPureFunction"), !Target.bIsEvent);
        Payload->SetBoolField(TEXT("functionCreated"), bFunctionCreated);
        Payload->SetBoolField(TEXT("bindingCreated"), false);
        Payload->SetArrayField(TEXT("compilerMessages"), CompilerMessages);

        Ctx.SendError(ErrorCodes::ERR_BINDING_FAILED,
            FString::Printf(
                TEXT("'%s' resolves to %s::%s, but function '%s' does not satisfy it%s. ")
                TEXT("No binding was written."),
                *PropertyName, *WidgetClassName, *DelegateName, *FunctionName,
                Target.bIsEvent ? TEXT("") : TEXT(" (a property binding needs a pure function returning the property type)")),
            Payload);
        return true;
    }

    bool bBindingCreated = false;
    if (!UpsertWidgetBinding(WidgetBP, Candidate, bBindingCreated, Error))
    {
        Ctx.SendError(ErrorCodes::ERR_BINDING_FAILED, Error);
        return true;
    }

    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WidgetBP);
    McpSafeAssetSave(WidgetBP);

    // Read back the record that actually landed in the array rather than restating the candidate.
    const int32 StoredIndex = WidgetBP->Bindings.IndexOfByKey(Candidate);
    const bool bBindingStored = StoredIndex != INDEX_NONE
        && WidgetBP->Bindings[StoredIndex].FunctionName == Candidate.FunctionName;
    if (!bBindingStored)
    {
        Ctx.SendError(ErrorCodes::ERR_BINDING_FAILED,
            FString::Printf(TEXT("Binding %s.%s -> '%s' is absent from the blueprint's Bindings array ")
                            TEXT("after the write."),
                *WidgetName, *Target.PropertyName.ToString(), *FunctionName));
        return true;
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetBoolField(TEXT("success"), bBindingStored);
    Result->SetStringField(TEXT("widgetName"), WidgetName);
    Result->SetStringField(TEXT("widgetClass"), WidgetClassName);
    Result->SetStringField(TEXT("propertyName"), Target.PropertyName.ToString());
    Result->SetStringField(TEXT("delegateProperty"), DelegateName);
    Result->SetStringField(TEXT("bindingType"), Target.bIsEvent ? TEXT("event") : TEXT("property"));
    Result->SetStringField(TEXT("functionName"), FunctionName);
    Result->SetBoolField(TEXT("functionCreated"), bFunctionCreated);
    Result->SetBoolField(TEXT("bindingCreated"), bBindingCreated);
    Result->SetBoolField(TEXT("bindingStored"), bBindingStored);
    Result->SetBoolField(TEXT("resolves"), bResolves);
    Ctx.SendSuccess(Result);
    return true;
}
