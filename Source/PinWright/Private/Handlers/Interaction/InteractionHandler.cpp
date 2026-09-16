// Copyright (c) 2026 Alexander Penkin. MIT License.

// Interaction System Handlers - Migrated to auto-registration pattern
#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/Blueprint/BlueprintHandlerUtils.h"
#include "PinWrightHelpers.h"
#include "Dom/JsonObject.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Editor.h"
#include "EngineUtils.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "UObject/SavePackage.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/SCS_Node.h"
#include "Components/ActorComponent.h"
#include "Components/SceneComponent.h"
#include "Components/SphereComponent.h"
#include "Components/BoxComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/TimelineComponent.h"
#include "GameFramework/Actor.h"
#include "Blueprint/UserWidget.h"
#include "Engine/DataTable.h"
#include "Misc/PackageName.h"

namespace
{
// ----------------------------------------------------------------------------
// Durable default-value baking for the configure_* Blueprint-asset handlers
// (trace / widget / door / switch / chest).
//
// These handlers scaffold gameplay member variables onto a Blueprint. Setting a
// value durably means writing it onto the variable's FBPVariableDescription.DefaultValue
// (the export-text string the Blueprint editor stores) BEFORE the single compile, so the
// compiler bakes it onto the class default object and it survives every future recompile.
// This is the engine-native FBlueprintEditorUtils::AddMemberVariable(BP, Name, Type,
// DefaultValue) path and mirrors the mechanism blueprint.add_variable uses. The pre-fix
// handlers added variables with no default (3-arg AddMemberVariable) and echoed the input
// args, so every configured value silently read back as the type's zero default. (The old
// door handler instead wrote the CDO directly before the recompile — fragile on a fresh BP
// where the property doesn't yet exist, and non-durable since the next recompile resets it.)
// ----------------------------------------------------------------------------

// Adds the member variable with DefaultValue baked in when absent, or updates the existing
// variable's DefaultValue in place. The following compile lands it on the CDO.
static void SetOrAddMemberVariableDefault(UBlueprint* Blueprint, const FName& VarName,
    const FEdGraphPinType& VarType, const FString& DefaultValueExportText)
{
    for (FBPVariableDescription& Var : Blueprint->NewVariables)
    {
        if (Var.VarName == VarName)
        {
            Var.DefaultValue = DefaultValueExportText;
            return;
        }
    }
    FBlueprintEditorUtils::AddMemberVariable(Blueprint, VarName, VarType, DefaultValueExportText);
}

// Reads a member variable's real post-compile value off the compiled CDO and writes it into
// Result under JsonField, so a configure_* response reports the value that actually landed
// rather than mirroring the request. No-op when the CDO or the property is missing (the field
// is then absent, making a value that failed to land visibly missing instead of faked).
static void EchoCompiledDefault(const TSharedPtr<FJsonObject>& Result, UObject* CDO,
    const TCHAR* PropertyName, const TCHAR* JsonField)
{
    if (!CDO)
    {
        return;
    }
    if (FProperty* Prop = FindFProperty<FProperty>(CDO->GetClass(), PropertyName))
    {
        if (const TSharedPtr<FJsonValue> Value = ExportPropertyToJsonValue(CDO, Prop))
        {
            Result->SetField(JsonField, Value);
        }
    }
}

// The export-text form a bool FBPVariableDescription.DefaultValue expects ("true"/"false").
static FString BoolDefaultText(bool bValue)
{
    return bValue ? TEXT("true") : TEXT("false");
}

// Marks the Blueprint structurally modified, compiles it once, and returns the freshly
// compiled class default object (the baked variable defaults land here). Returns null when the
// compile produced no GeneratedClass, which the configure_* handlers treat as a failed compile.
static UObject* CompileAndGetDefaultObject(UBlueprint* Blueprint,
    BlueprintHandlerUtils::FBlueprintCompileDiagnostics& OutCompileDiagnostics)
{
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
    OutCompileDiagnostics = BlueprintHandlerUtils::CompileBlueprintWithDiagnostics(Blueprint);
    return Blueprint->GeneratedClass ? Blueprint->GeneratedClass->GetDefaultObject() : nullptr;
}

// Finds the first SCS node whose component template is a USphereComponent — the single
// definition of "the interaction sphere" shared by the configure_interaction_trace writer
// and the get_interaction_info readback, so both target the same node and can't drift.
// Returns null (and leaves OutTemplate untouched) when the Blueprint has no such node.
static USCS_Node* FindInteractionSphereNode(UBlueprint* Blueprint, USphereComponent** OutTemplate = nullptr)
{
    if (!Blueprint || !Blueprint->SimpleConstructionScript)
    {
        return nullptr;
    }
    for (USCS_Node* Node : Blueprint->SimpleConstructionScript->GetAllNodes())
    {
        if (!Node || !Node->ComponentClass) continue;
        if (Node->ComponentClass->IsChildOf(USphereComponent::StaticClass()))
        {
            if (USphereComponent* SphereComp = Cast<USphereComponent>(Node->ComponentTemplate))
            {
                if (OutTemplate)
                {
                    *OutTemplate = SphereComp;
                }
                return Node;
            }
        }
    }
    return nullptr;
}

// ----------------------------------------------------------------------------
// get_interaction_info Blueprint-target readback.
//
// The blueprintPath branch historically emitted only {blueprintPath, blueprintName},
// so the one namespace-native "did my authoring land?" verb confirmed nothing for a
// Blueprint that the sibling create_*/configure_* verbs just wrote to (the rich
// readback lived only in the actorName branch). This reads the same state back off
// the compiled Blueprint — the member-variable defaults baked onto the CDO by the
// configure_* handlers, the interaction sphere on the SCS, and the four event
// dispatchers on the GeneratedClass. It reports that config under the SAME KEY NAMES
// as the actor branch (component/trace/widget/chest/events), but with a DIFFERENT,
// source-appropriate field set per key: the two targets persist config in different
// stores (CDO/SCS state here vs. namespaced actor tags there), so the field shapes
// are not — and cannot be — identical. Every field is emitted only when its source
// property/component actually exists (EchoCompiledDefault no-ops on a missing
// property), so a bare Blueprint reports nothing extra rather than faking zero defaults.
// ----------------------------------------------------------------------------
static void AddBlueprintInteractionReadback(const TSharedPtr<FJsonObject>& Result, UBlueprint* Blueprint)
{
    UClass* GeneratedClass = Blueprint ? Blueprint->GeneratedClass : nullptr;
    UObject* CDO = GeneratedClass ? GeneratedClass->GetDefaultObject() : nullptr;

    // Only add a sub-object when at least one of its fields was actually populated,
    // so an unconfigured Blueprint doesn't report empty {} sections.
    auto EmitIfPopulated = [&Result](const TCHAR* Section, const TSharedPtr<FJsonObject>& Sub)
    {
        if (Sub->Values.Num() > 0)
        {
            Result->SetObjectField(Section, Sub);
        }
    };

    // (a) Interaction sphere/component — read the sphere template + radius off the SCS via the
    // shared FindInteractionSphereNode locator that configure_interaction_trace also targets.
    {
        TSharedPtr<FJsonObject> Component = MakeShared<FJsonObject>();
        USphereComponent* SphereComp = nullptr;
        if (USCS_Node* Node = FindInteractionSphereNode(Blueprint, &SphereComp))
        {
            Component->SetStringField(TEXT("componentName"), Node->GetVariableName().ToString());
            Component->SetStringField(TEXT("componentClass"), Node->ComponentClass->GetName());
            Component->SetNumberField(TEXT("sphereRadius"), SphereComp->GetUnscaledSphereRadius());
        }
        EmitIfPopulated(TEXT("component"), Component);
    }

    // (b) Trace — TraceDistance/TraceType baked onto the CDO by configure_interaction_trace.
    {
        TSharedPtr<FJsonObject> Trace = MakeShared<FJsonObject>();
        EchoCompiledDefault(Trace, CDO, TEXT("TraceDistance"), TEXT("traceDistance"));
        EchoCompiledDefault(Trace, CDO, TEXT("TraceType"), TEXT("traceType"));
        EmitIfPopulated(TEXT("trace"), Trace);
    }

    // (c) Prompt widget — bShowOnHover/bShowPromptText/PromptTextFormat from configure_interaction_widget.
    {
        TSharedPtr<FJsonObject> Widget = MakeShared<FJsonObject>();
        EchoCompiledDefault(Widget, CDO, TEXT("bShowOnHover"), TEXT("showOnHover"));
        EchoCompiledDefault(Widget, CDO, TEXT("bShowPromptText"), TEXT("showPromptText"));
        EchoCompiledDefault(Widget, CDO, TEXT("PromptTextFormat"), TEXT("promptTextFormat"));
        EchoCompiledDefault(Widget, CDO, TEXT("InteractionWidgetClass"), TEXT("widgetClass"));
        EmitIfPopulated(TEXT("widget"), Widget);
    }

    // (d) Event dispatchers — the four OnInteraction* multicast delegates added by add_interaction_events.
    if (GeneratedClass)
    {
        static const TCHAR* const InteractionEventNames[] = {
            TEXT("OnInteractionStart"), TEXT("OnInteractionEnd"),
            TEXT("OnInteractableFound"), TEXT("OnInteractableLost")
        };
        TArray<TSharedPtr<FJsonValue>> Events;
        for (const TCHAR* EventName : InteractionEventNames)
        {
            if (FindFProperty<FMulticastDelegateProperty>(GeneratedClass, EventName))
            {
                Events.Add(MakeShared<FJsonValueString>(EventName));
            }
        }
        if (Events.Num() > 0)
        {
            Result->SetArrayField(TEXT("events"), Events);
        }
    }

    // (e) Chest/door gameplay props — bIsLocked/LidOpenAngle/OpenTime/LootTable (and the door's
    // OpenAngle) baked onto the CDO by configure_chest_properties / configure_door_properties.
    {
        TSharedPtr<FJsonObject> Chest = MakeShared<FJsonObject>();
        EchoCompiledDefault(Chest, CDO, TEXT("bIsLocked"), TEXT("locked"));
        EchoCompiledDefault(Chest, CDO, TEXT("LidOpenAngle"), TEXT("lidOpenAngle"));
        EchoCompiledDefault(Chest, CDO, TEXT("OpenAngle"), TEXT("openAngle"));
        EchoCompiledDefault(Chest, CDO, TEXT("OpenTime"), TEXT("openTime"));
        EchoCompiledDefault(Chest, CDO, TEXT("LootTable"), TEXT("lootTable"));
        EmitIfPopulated(TEXT("chest"), Chest);
    }
}
}

// ============================================================================
// 18.1 Interaction Component (Blueprint-based)
// ============================================================================

REGISTER_RPC_HANDLER("interaction.create_interaction_component", "interaction", "Add an interaction sphere component to a Blueprint",
    RPC_PARAMS(
        RPC_PARAM_REQ("blueprintPath", "path", "Path to the Blueprint asset"),
        RPC_PARAM_OPT("componentName", "string", "Name for the new component"),
        RPC_PARAM_OPT("traceDistance", "number", "Interaction trace distance / sphere radius")
    ))
{
    FString BlueprintPath = Ctx.GetString(TEXT("blueprintPath"));
    FString ComponentName = Ctx.GetString(TEXT("componentName"), TEXT("InteractionComponent"));

    FString ResolvedPath, LoadError;
    UBlueprint* Blueprint = LoadBlueprintAsset(BlueprintPath, ResolvedPath, LoadError);
    if (!Blueprint)
    {
        Ctx.SendError(TEXT("BLUEPRINT_NOT_FOUND"), LoadError);
        return true;
    }

    if (!Blueprint->SimpleConstructionScript)
    {
        Ctx.SendError(TEXT("INVALID_BP"), TEXT("Blueprint has no SimpleConstructionScript"));
        return true;
    }

    USCS_Node* Node = Blueprint->SimpleConstructionScript->CreateNode(USphereComponent::StaticClass(), *ComponentName);
    if (Node)
    {
        USphereComponent* Template = Cast<USphereComponent>(Node->ComponentTemplate);
        if (Template)
        {
            float TraceDistance = static_cast<float>(Ctx.GetNumber(TEXT("traceDistance"), 200.0));
            Template->SetSphereRadius(TraceDistance);
            Template->SetCollisionProfileName(TEXT("OverlapAll"));
            Template->SetGenerateOverlapEvents(true);
        }
        Blueprint->SimpleConstructionScript->AddNode(Node);
        FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
        McpSafeAssetSave(Blueprint);

        TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject());
        Result->SetBoolField(TEXT("componentAdded"), true);
        Result->SetStringField(TEXT("componentName"), ComponentName);
        AddAssetVerification(Result, Blueprint);
        Ctx.SendSuccess(Result);
    }
    else
    {
        Ctx.SendError(TEXT("COMPONENT_CREATE_FAILED"), TEXT("Failed to create interaction component"));
    }
    return true;
}

REGISTER_RPC_HANDLER("interaction.configure_interaction_trace", "interaction", "Configure interaction trace settings on a Blueprint",
    RPC_PARAMS(
        RPC_PARAM_REQ("blueprintPath", "path", "Path to the Blueprint asset"),
        RPC_PARAM_OPT("traceType", "string", "Trace type (sphere, box, etc.)"),
        RPC_PARAM_OPT("traceDistance", "number", "Trace distance"),
        RPC_PARAM_OPT("traceRadius", "number", "Trace radius")
    ))
{
    FString BlueprintPath = Ctx.GetString(TEXT("blueprintPath"));
    FString TraceType = Ctx.GetString(TEXT("traceType"), TEXT("sphere"));
    double TraceDistance = Ctx.GetNumber(TEXT("traceDistance"), 200.0);
    double TraceRadius = Ctx.GetNumber(TEXT("traceRadius"), 50.0);

    FString ResolvedPath, LoadError;
    UBlueprint* Blueprint = LoadBlueprintAsset(BlueprintPath, ResolvedPath, LoadError);
    if (!Blueprint)
    {
        Ctx.SendError(TEXT("BLUEPRINT_NOT_FOUND"), LoadError);
        return true;
    }

    bool bConfigured = false;

    USimpleConstructionScript* SCS = Blueprint->SimpleConstructionScript;
    if (SCS)
    {
        for (USCS_Node* Node : SCS->GetAllNodes())
        {
            if (!Node || !Node->ComponentClass) continue;

            if (Node->ComponentClass->IsChildOf(USphereComponent::StaticClass()))
            {
                USphereComponent* SphereComp = Cast<USphereComponent>(Node->ComponentTemplate);
                if (SphereComp)
                {
                    SphereComp->SetSphereRadius(static_cast<float>(TraceDistance));
                    SphereComp->SetCollisionProfileName(TEXT("OverlapAll"));
                    SphereComp->SetGenerateOverlapEvents(true);
                    bConfigured = true;
                }
            }
            else if (Node->ComponentClass->IsChildOf(UBoxComponent::StaticClass()))
            {
                UBoxComponent* BoxComp = Cast<UBoxComponent>(Node->ComponentTemplate);
                if (BoxComp)
                {
                    BoxComp->SetBoxExtent(FVector(static_cast<float>(TraceDistance), static_cast<float>(TraceRadius), static_cast<float>(TraceRadius)));
                    BoxComp->SetCollisionProfileName(TEXT("OverlapAll"));
                    BoxComp->SetGenerateOverlapEvents(true);
                    bConfigured = true;
                }
            }
        }
    }

    // Add trace configuration Blueprint variables
    FEdGraphPinType FloatType;
    FloatType.PinCategory = UEdGraphSchema_K2::PC_Real;
    FloatType.PinSubCategory = UEdGraphSchema_K2::PC_Float;

    FEdGraphPinType NameType;
    NameType.PinCategory = UEdGraphSchema_K2::PC_Name;

    // Bake the configured values durably onto the member-variable defaults; the compile below
    // lands them on the CDO. See SetOrAddMemberVariableDefault for the full rationale.
    SetOrAddMemberVariableDefault(Blueprint, TEXT("TraceDistance"), FloatType, FString::SanitizeFloat(TraceDistance));
    SetOrAddMemberVariableDefault(Blueprint, TEXT("TraceType"), NameType, TraceType);

    BlueprintHandlerUtils::FBlueprintCompileDiagnostics CompileDiagnostics;
    UObject* CDO = CompileAndGetDefaultObject(Blueprint, CompileDiagnostics);

    // Echo the values that actually landed on the compiled CDO — not a bare mirror of the request.
    TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject());
    EchoCompiledDefault(Result, CDO, TEXT("TraceType"), TEXT("traceType"));
    EchoCompiledDefault(Result, CDO, TEXT("TraceDistance"), TEXT("traceDistance"));
    // traceRadius is applied to the box-component extent (above), not a member variable, so it
    // has no CDO to read back — echo the requested value.
    Result->SetNumberField(TEXT("traceRadius"), TraceRadius);
    Result->SetBoolField(TEXT("configured"), bConfigured);
    BlueprintHandlerUtils::AddCompileDiagnosticsToJson(CompileDiagnostics, Result);

    McpSafeAssetSave(Blueprint);
    AddAssetVerification(Result, Blueprint);
    Ctx.SendSuccess(Result);
    return true;
}

REGISTER_RPC_HANDLER("interaction.configure_interaction_widget", "interaction", "Configure interaction widget on a Blueprint",
    RPC_PARAMS(
        RPC_PARAM_REQ("blueprintPath", "path", "Path to the Blueprint asset"),
        RPC_PARAM_OPT("widgetClass", "classref", "Widget class to use"),
        RPC_PARAM_OPT("showOnHover", "boolean", "Show widget on hover"),
        RPC_PARAM_OPT("showPromptText", "boolean", "Show prompt text"),
        RPC_PARAM_OPT("promptTextFormat", "string", "Format for prompt text")
    ))
{
    FString BlueprintPath = Ctx.GetString(TEXT("blueprintPath"));
    FString WidgetClass = Ctx.GetString(TEXT("widgetClass"));
    bool ShowOnHover = Ctx.GetBool(TEXT("showOnHover"), true);
    bool ShowPromptText = Ctx.GetBool(TEXT("showPromptText"), true);
    FString PromptTextFormat = Ctx.GetString(TEXT("promptTextFormat"), TEXT("Press {Key} to Interact"));

    FString ResolvedPath, LoadError;
    UBlueprint* Blueprint = LoadBlueprintAsset(BlueprintPath, ResolvedPath, LoadError);
    if (!Blueprint)
    {
        Ctx.SendError(TEXT("BLUEPRINT_NOT_FOUND"), LoadError);
        return true;
    }

    FEdGraphPinType BoolType;
    BoolType.PinCategory = UEdGraphSchema_K2::PC_Boolean;

    FEdGraphPinType StringType;
    StringType.PinCategory = UEdGraphSchema_K2::PC_String;

    // Bake the configured values durably onto the member-variable defaults; the compile below
    // lands them on the CDO. See SetOrAddMemberVariableDefault for the full rationale.
    SetOrAddMemberVariableDefault(Blueprint, TEXT("bShowOnHover"), BoolType, BoolDefaultText(ShowOnHover));
    SetOrAddMemberVariableDefault(Blueprint, TEXT("bShowPromptText"), BoolType, BoolDefaultText(ShowPromptText));
    SetOrAddMemberVariableDefault(Blueprint, TEXT("PromptTextFormat"), StringType, PromptTextFormat);

    // InteractionWidgetClass is a soft reference to the interaction prompt widget class. A
    // class / soft-class pin needs a meta-class (PinSubCategoryObject), or the Blueprint
    // compiler downgrades the property to a bare UObject reference; constrain it to UUserWidget.
    // (The full compiler-mechanism rationale lives in the regression test's header,
    // FInteractionConfigureWidgetClassMetaClassResolvedTest in TestInteractionHandlers.cpp.)
    // The default value is intentionally left unset here — this handler still only echoes
    // widgetClass below; durably landing it on the CDO is a separate value concern, not this
    // malformed-type fix.
    FEdGraphPinType SoftClassType;
    SoftClassType.PinCategory = UEdGraphSchema_K2::PC_SoftClass;
    SoftClassType.PinSubCategoryObject = UUserWidget::StaticClass();
    SetOrAddMemberVariableDefault(Blueprint, TEXT("InteractionWidgetClass"), SoftClassType, FString());

    BlueprintHandlerUtils::FBlueprintCompileDiagnostics CompileDiagnostics;
    UObject* CDO = CompileAndGetDefaultObject(Blueprint, CompileDiagnostics);

    // Echo the values that actually landed on the compiled CDO — not a bare mirror of the request.
    // widgetClass remains an echo of the requested class name; durably landing it on the
    // InteractionWidgetClass default is a separate value concern (see the note above).
    TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject());
    Result->SetStringField(TEXT("widgetClass"), WidgetClass);
    EchoCompiledDefault(Result, CDO, TEXT("bShowOnHover"), TEXT("showOnHover"));
    EchoCompiledDefault(Result, CDO, TEXT("bShowPromptText"), TEXT("showPromptText"));
    EchoCompiledDefault(Result, CDO, TEXT("PromptTextFormat"), TEXT("promptTextFormat"));
    Result->SetBoolField(TEXT("configured"), CDO != nullptr);
    Result->SetStringField(TEXT("blueprintPath"), BlueprintPath);
    BlueprintHandlerUtils::AddCompileDiagnosticsToJson(CompileDiagnostics, Result);

    McpSafeAssetSave(Blueprint);
    Ctx.SendSuccess(Result);
    return true;
}

REGISTER_RPC_HANDLER("interaction.add_interaction_events", "interaction", "Add interaction event dispatchers to a Blueprint",
    RPC_PARAMS(
        RPC_PARAM_REQ("blueprintPath", "path", "Path to the Blueprint asset")
    ))
{
    FString BlueprintPath = Ctx.GetString(TEXT("blueprintPath"));

    FString ResolvedPath, LoadError;
    UBlueprint* Blueprint = LoadBlueprintAsset(BlueprintPath, ResolvedPath, LoadError);
    if (!Blueprint)
    {
        Ctx.SendError(TEXT("BLUEPRINT_NOT_FOUND"), LoadError);
        return true;
    }

    TArray<FString> EventNames = {
        TEXT("OnInteractionStart"),
        TEXT("OnInteractionEnd"),
        TEXT("OnInteractableFound"),
        TEXT("OnInteractableLost")
    };

    TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject());
    TArray<TSharedPtr<FJsonValue>> AddedEvents;

    // The four interaction events are parameterless multicast dispatchers.
    const TArray<BlueprintHandlerUtils::FParsedPinParam> NoSignatureParams;
    int32 AddedCount = 0;

    for (const FString& EventName : EventNames)
    {
        const FName EventFName(*EventName);

        // Create each dispatcher with a real delegate signature graph so the compiled
        // FMulticastDelegateProperty carries a backing <Name>__DelegateSignature UFunction.
        // Adding only a PC_MCDelegate member variable (as this handler did before) left
        // SignatureFunction null, which warns "No SignatureFunction in MulticastDelegateProperty
        // '<name>'" on the next compile and cannot be bound. Shared recipe with
        // blueprint.add_dispatcher — its AddMemberVariable step already reports an existing
        // member as MemberExists, so we switch on the helper result instead of pre-scanning
        // NewVariables by hand (which also missed inherited/other-scope name collisions).
        const BlueprintHandlerUtils::EAddDispatcherResult AddResult =
            BlueprintHandlerUtils::AddDispatcherWithSignatureGraph(Blueprint, EventFName, NoSignatureParams);
        switch (AddResult)
        {
        case BlueprintHandlerUtils::EAddDispatcherResult::Success:
            ++AddedCount;
            AddedEvents.Add(MakeShareable(new FJsonValueString(EventName)));
            break;
        case BlueprintHandlerUtils::EAddDispatcherResult::MemberExists:
            AddedEvents.Add(MakeShareable(new FJsonValueString(EventName + TEXT(" (exists)"))));
            break;
        default:
            AddedEvents.Add(MakeShareable(new FJsonValueString(EventName + TEXT(" (failed)"))));
            break;
        }
    }

    Result->SetArrayField(TEXT("eventsAdded"), AddedEvents);
    Result->SetStringField(TEXT("blueprintPath"), BlueprintPath);
    Result->SetNumberField(TEXT("eventCount"), AddedCount);

    // Only compile + re-save when a dispatcher was actually created; re-running on a Blueprint
    // that already has all four events is an idempotent no-op. Compiling here (not just marking
    // structurally modified) is what lands the freshly added multicast-delegate member variables
    // onto the GeneratedClass as FMulticastDelegateProperty entries — the same single-compile step
    // the sibling configure_* handlers run via CompileAndGetDefaultObject. Without it the
    // dispatchers live only in NewVariables/DelegateSignatureGraphs until some later recompile, so
    // the events don't yet exist on the compiled class and get_interaction_info's readback (which
    // reflects over GeneratedClass) reports none.
    if (AddedCount > 0)
    {
        BlueprintHandlerUtils::FBlueprintCompileDiagnostics CompileDiagnostics;
        CompileAndGetDefaultObject(Blueprint, CompileDiagnostics);
        BlueprintHandlerUtils::AddCompileDiagnosticsToJson(CompileDiagnostics, Result);
        McpSafeAssetSave(Blueprint);
    }
    Ctx.SendSuccess(Result);
    return true;
}

// ============================================================================
// 18.2 Interactables
// ============================================================================

REGISTER_RPC_HANDLER("interaction.configure_door_properties", "interaction", "Configure properties on a door Blueprint",
    RPC_PARAMS(
        RPC_PARAM_REQ("doorPath", "path", "Path to the door Blueprint"),
        RPC_PARAM_OPT("openAngle", "number", "Door open angle in degrees"),
        RPC_PARAM_OPT("openTime", "number", "Door open time in seconds"),
        RPC_PARAM_OPT("locked", "boolean", "Whether the door is locked")
    ))
{
    FString DoorPath = Ctx.GetString(TEXT("doorPath"));
    double OpenAngle = Ctx.GetNumber(TEXT("openAngle"), 90.0);
    double OpenTime = Ctx.GetNumber(TEXT("openTime"), 0.5);
    bool Locked = Ctx.GetBool(TEXT("locked"), false);

    FString ResolvedPath, LoadError;
    UBlueprint* Blueprint = LoadBlueprintAsset(DoorPath, ResolvedPath, LoadError);
    if (!Blueprint)
    {
        Ctx.SendError(TEXT("BLUEPRINT_NOT_FOUND"), LoadError);
        return true;
    }

    FEdGraphPinType FloatType;
    FloatType.PinCategory = UEdGraphSchema_K2::PC_Real;
    FloatType.PinSubCategory = UEdGraphSchema_K2::PC_Float;

    FEdGraphPinType BoolType;
    BoolType.PinCategory = UEdGraphSchema_K2::PC_Boolean;

    // Bake each configured value durably onto the member-variable default; the compile below
    // lands it on the CDO. See SetOrAddMemberVariableDefault for the full rationale (including
    // why the old direct-CDO-write was fragile). bIsOpen carries no configured input, so it
    // stays at its zero default.
    SetOrAddMemberVariableDefault(Blueprint, TEXT("OpenAngle"), FloatType, FString::SanitizeFloat(OpenAngle));
    SetOrAddMemberVariableDefault(Blueprint, TEXT("OpenTime"), FloatType, FString::SanitizeFloat(OpenTime));
    SetOrAddMemberVariableDefault(Blueprint, TEXT("bIsLocked"), BoolType, BoolDefaultText(Locked));
    SetOrAddMemberVariableDefault(Blueprint, TEXT("bIsOpen"), BoolType, FString());

    BlueprintHandlerUtils::FBlueprintCompileDiagnostics CompileDiagnostics;
    UObject* CDO = CompileAndGetDefaultObject(Blueprint, CompileDiagnostics);

    // Echo the values that actually landed on the compiled CDO — not a bare mirror of the request.
    TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject());
    EchoCompiledDefault(Result, CDO, TEXT("OpenAngle"), TEXT("openAngle"));
    EchoCompiledDefault(Result, CDO, TEXT("OpenTime"), TEXT("openTime"));
    EchoCompiledDefault(Result, CDO, TEXT("bIsLocked"), TEXT("locked"));
    Result->SetBoolField(TEXT("configured"), CDO != nullptr);
    Result->SetStringField(TEXT("doorPath"), DoorPath);
    BlueprintHandlerUtils::AddCompileDiagnosticsToJson(CompileDiagnostics, Result);

    McpSafeAssetSave(Blueprint);
    Ctx.SendSuccess(Result);
    return true;
}
REGISTER_RPC_HANDLER("interaction.configure_switch_properties", "interaction", "Configure properties on a switch Blueprint",
    RPC_PARAMS(
        RPC_PARAM_REQ("switchPath", "path", "Path to the switch Blueprint"),
        RPC_PARAM_OPT("switchType", "string", "Type of switch"),
        RPC_PARAM_OPT("canToggle", "boolean", "Whether the switch can be toggled"),
        RPC_PARAM_OPT("resetTime", "number", "Time before switch resets")
    ))
{
    FString SwitchPath = Ctx.GetString(TEXT("switchPath"));
    FString SwitchType = Ctx.GetString(TEXT("switchType"), TEXT("button"));
    bool CanToggle = Ctx.GetBool(TEXT("canToggle"), true);
    double ResetTime = Ctx.GetNumber(TEXT("resetTime"), 0.0);

    FString ResolvedPath, LoadError;
    UBlueprint* Blueprint = LoadBlueprintAsset(SwitchPath, ResolvedPath, LoadError);
    if (!Blueprint)
    {
        Ctx.SendError(TEXT("BLUEPRINT_NOT_FOUND"), LoadError);
        return true;
    }

    FEdGraphPinType NameType;
    NameType.PinCategory = UEdGraphSchema_K2::PC_Name;

    FEdGraphPinType BoolType;
    BoolType.PinCategory = UEdGraphSchema_K2::PC_Boolean;

    FEdGraphPinType FloatType;
    FloatType.PinCategory = UEdGraphSchema_K2::PC_Real;
    FloatType.PinSubCategory = UEdGraphSchema_K2::PC_Float;

    // Bake each configured value durably onto the member-variable default; the compile below
    // lands it on the CDO. See SetOrAddMemberVariableDefault for the full rationale.
    // bIsActivated carries no configured input, so it stays at its zero default.
    SetOrAddMemberVariableDefault(Blueprint, TEXT("SwitchType"), NameType, SwitchType);
    SetOrAddMemberVariableDefault(Blueprint, TEXT("bCanToggle"), BoolType, BoolDefaultText(CanToggle));
    SetOrAddMemberVariableDefault(Blueprint, TEXT("bIsActivated"), BoolType, FString());
    SetOrAddMemberVariableDefault(Blueprint, TEXT("ResetTime"), FloatType, FString::SanitizeFloat(ResetTime));

    BlueprintHandlerUtils::FBlueprintCompileDiagnostics CompileDiagnostics;
    UObject* CDO = CompileAndGetDefaultObject(Blueprint, CompileDiagnostics);

    // Echo the values that actually landed on the compiled CDO — not a bare mirror of the request.
    TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject());
    EchoCompiledDefault(Result, CDO, TEXT("SwitchType"), TEXT("switchType"));
    EchoCompiledDefault(Result, CDO, TEXT("bCanToggle"), TEXT("canToggle"));
    EchoCompiledDefault(Result, CDO, TEXT("ResetTime"), TEXT("resetTime"));
    Result->SetBoolField(TEXT("configured"), CDO != nullptr);
    Result->SetStringField(TEXT("switchPath"), SwitchPath);
    BlueprintHandlerUtils::AddCompileDiagnosticsToJson(CompileDiagnostics, Result);

    McpSafeAssetSave(Blueprint);
    Ctx.SendSuccess(Result);
    return true;
}
REGISTER_RPC_HANDLER("interaction.configure_chest_properties", "interaction", "Configure properties on a chest Blueprint",
    RPC_PARAMS(
        RPC_PARAM_REQ("chestPath", "path", "Path to the chest Blueprint"),
        RPC_PARAM_OPT("locked", "boolean", "Whether the chest is locked"),
        RPC_PARAM_OPT("openAngle", "number", "Lid open angle in degrees"),
        RPC_PARAM_OPT("openTime", "number", "Lid open time in seconds"),
        RPC_PARAM_OPT("lootTablePath", "path", "Path to loot table asset")
    ))
{
    FString ChestPath = Ctx.GetString(TEXT("chestPath"));
    bool Locked = Ctx.GetBool(TEXT("locked"), false);
    double OpenAngle = Ctx.GetNumber(TEXT("openAngle"), 90.0);
    double OpenTime = Ctx.GetNumber(TEXT("openTime"), 0.5);
    FString LootTablePath = Ctx.GetString(TEXT("lootTablePath"));

    FString ResolvedPath, LoadError;
    UBlueprint* Blueprint = LoadBlueprintAsset(ChestPath, ResolvedPath, LoadError);
    if (!Blueprint)
    {
        Ctx.SendError(TEXT("BLUEPRINT_NOT_FOUND"), LoadError);
        return true;
    }

    FEdGraphPinType BoolType;
    BoolType.PinCategory = UEdGraphSchema_K2::PC_Boolean;

    FEdGraphPinType FloatType;
    FloatType.PinCategory = UEdGraphSchema_K2::PC_Real;
    FloatType.PinSubCategory = UEdGraphSchema_K2::PC_Float;

    // LootTable is a soft reference to the chest's loot-table asset. Like a class / soft-class
    // pin, a soft-OBJECT pin needs a meta-class (PinSubCategoryObject) or the Blueprint compiler
    // silently defaults the property's PropertyClass to UObject (KismetCompilerMisc's PC_Object /
    // PC_Interface / PC_SoftObject branch defaults a null sub-type to UObject::StaticClass() with
    // NO warning logged, unlike the PC_Class / PC_SoftClass branch), degrading LootTable to a
    // bare TSoftObjectPtr<UObject> that can hold any asset instead of the intended loot table.
    // Constrain it to UDataTable — the canonical loot-table asset type. (Regression-asserted in
    // FInteractionConfigureChestDefaultsLandOnCdoTest in TestInteractionHandlers.cpp.)
    FEdGraphPinType SoftObjectType;
    SoftObjectType.PinCategory = UEdGraphSchema_K2::PC_SoftObject;
    SoftObjectType.PinSubCategoryObject = UDataTable::StaticClass();

    // Bake each configured value durably onto the member-variable default; the compile below
    // lands it on the CDO. See SetOrAddMemberVariableDefault for the full rationale.
    // bIsOpen carries no configured input, so it stays at its zero default.
    SetOrAddMemberVariableDefault(Blueprint, TEXT("bIsLocked"), BoolType, BoolDefaultText(Locked));
    SetOrAddMemberVariableDefault(Blueprint, TEXT("bIsOpen"), BoolType, FString());
    SetOrAddMemberVariableDefault(Blueprint, TEXT("LidOpenAngle"), FloatType, FString::SanitizeFloat(OpenAngle));
    SetOrAddMemberVariableDefault(Blueprint, TEXT("OpenTime"), FloatType, FString::SanitizeFloat(OpenTime));
    SetOrAddMemberVariableDefault(Blueprint, TEXT("LootTable"), SoftObjectType, LootTablePath);

    BlueprintHandlerUtils::FBlueprintCompileDiagnostics CompileDiagnostics;
    UObject* CDO = CompileAndGetDefaultObject(Blueprint, CompileDiagnostics);

    // Echo the values that actually landed on the compiled CDO — not a bare mirror of the
    // request — so a value that failed to bake is visibly absent rather than faked.
    TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject());
    EchoCompiledDefault(Result, CDO, TEXT("bIsLocked"), TEXT("locked"));
    EchoCompiledDefault(Result, CDO, TEXT("LidOpenAngle"), TEXT("openAngle"));
    EchoCompiledDefault(Result, CDO, TEXT("OpenTime"), TEXT("openTime"));
    if (!LootTablePath.IsEmpty())
    {
        EchoCompiledDefault(Result, CDO, TEXT("LootTable"), TEXT("lootTablePath"));
    }
    Result->SetBoolField(TEXT("configured"), CDO != nullptr);
    Result->SetStringField(TEXT("chestPath"), ChestPath);
    BlueprintHandlerUtils::AddCompileDiagnosticsToJson(CompileDiagnostics, Result);

    McpSafeAssetSave(Blueprint);
    Ctx.SendSuccess(Result);
    return true;
}
REGISTER_RPC_HANDLER("interaction.add_destruction_component", "interaction", "Add a destruction component with health variables to a Blueprint",
    RPC_PARAMS(
        RPC_PARAM_REQ("blueprintPath", "path", "Path to the Blueprint asset"),
        RPC_PARAM_OPT("componentName", "string", "Name for the destruction component")
    ))
{
    FString BlueprintPath = Ctx.GetString(TEXT("blueprintPath"));
    FString ComponentName = Ctx.GetString(TEXT("componentName"), TEXT("DestructionComponent"));

    FString ResolvedPath, LoadError;
    UBlueprint* Blueprint = LoadBlueprintAsset(BlueprintPath, ResolvedPath, LoadError);
    if (!Blueprint)
    {
        Ctx.SendError(TEXT("BLUEPRINT_NOT_FOUND"), LoadError);
        return true;
    }

    USimpleConstructionScript* SCS = Blueprint->SimpleConstructionScript;
    if (!SCS)
    {
        Ctx.SendError(TEXT("NO_SCS"), TEXT("Blueprint has no SimpleConstructionScript"));
        return true;
    }

    USCS_Node* Node = SCS->CreateNode(USceneComponent::StaticClass(), *ComponentName);
    if (Node)
    {
        SCS->AddNode(Node);

        FEdGraphPinType BoolType;
        BoolType.PinCategory = UEdGraphSchema_K2::PC_Boolean;

        FEdGraphPinType FloatType;
        FloatType.PinCategory = UEdGraphSchema_K2::PC_Real;
        FloatType.PinSubCategory = UEdGraphSchema_K2::PC_Float;

        FEdGraphPinType IntType;
        IntType.PinCategory = UEdGraphSchema_K2::PC_Int;

        // Add Health variable
        bool bHealthExists = false;
        for (FBPVariableDescription& Var : Blueprint->NewVariables)
        {
            if (Var.VarName == TEXT("Health")) { bHealthExists = true; break; }
        }
        if (!bHealthExists)
        {
            FBlueprintEditorUtils::AddMemberVariable(Blueprint, TEXT("Health"), FloatType);
        }

        // Add MaxHealth variable
        bool bMaxHealthExists = false;
        for (FBPVariableDescription& Var : Blueprint->NewVariables)
        {
            if (Var.VarName == TEXT("MaxHealth")) { bMaxHealthExists = true; break; }
        }
        if (!bMaxHealthExists)
        {
            FBlueprintEditorUtils::AddMemberVariable(Blueprint, TEXT("MaxHealth"), FloatType);
        }

        // Add bIsDestroyed variable
        bool bDestroyedExists = false;
        for (FBPVariableDescription& Var : Blueprint->NewVariables)
        {
            if (Var.VarName == TEXT("bIsDestroyed")) { bDestroyedExists = true; break; }
        }
        if (!bDestroyedExists)
        {
            FBlueprintEditorUtils::AddMemberVariable(Blueprint, TEXT("bIsDestroyed"), BoolType);
        }

        // Add DestructionStage variable
        bool bStageExists = false;
        for (FBPVariableDescription& Var : Blueprint->NewVariables)
        {
            if (Var.VarName == TEXT("DestructionStage")) { bStageExists = true; break; }
        }
        if (!bStageExists)
        {
            FBlueprintEditorUtils::AddMemberVariable(Blueprint, TEXT("DestructionStage"), IntType);
        }

        FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
        McpSafeAssetSave(Blueprint);

        TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject());
        Result->SetBoolField(TEXT("componentAdded"), true);
        Result->SetStringField(TEXT("componentName"), ComponentName);
        Result->SetStringField(TEXT("blueprintPath"), BlueprintPath);

        TArray<TSharedPtr<FJsonValue>> AddedVars;
        AddedVars.Add(MakeShareable(new FJsonValueString(TEXT("Health"))));
        AddedVars.Add(MakeShareable(new FJsonValueString(TEXT("MaxHealth"))));
        AddedVars.Add(MakeShareable(new FJsonValueString(TEXT("bIsDestroyed"))));
        AddedVars.Add(MakeShareable(new FJsonValueString(TEXT("DestructionStage"))));
        Result->SetArrayField(TEXT("variablesAdded"), AddedVars);

        Ctx.SendSuccess(Result);
    }
    else
    {
        Ctx.SendError(TEXT("COMPONENT_CREATE_FAILED"), TEXT("Failed to create destruction component"));
    }
    return true;
}

// ============================================================================
// Utility
// ============================================================================

REGISTER_RPC_HANDLER("interaction.get_interaction_info", "interaction", "Get info about an interactable Blueprint or actor",
    RPC_PARAMS(
        RPC_PARAM_OPT("blueprintPath", "path", "Path to a Blueprint asset"),
        RPC_PARAM_OPT("actorName", "string", "Name of an actor in the world")
    ))
{
    FString BlueprintPath = Ctx.GetString(TEXT("blueprintPath"));
    FString ActorName = Ctx.GetString(TEXT("actorName"));
    TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject());

    if (!BlueprintPath.IsEmpty())
    {
        FString ResolvedPath, LoadError;
        UBlueprint* Blueprint = LoadBlueprintAsset(BlueprintPath, ResolvedPath, LoadError);
        if (Blueprint)
        {
            Result->SetStringField(TEXT("blueprintPath"), BlueprintPath);
            Result->SetStringField(TEXT("blueprintName"), Blueprint->GetName());

            // Read back the interaction config the create_*/configure_* verbs wrote to this
            // Blueprint (sphere/trace/widget/events/chest), so this verb confirms the authoring
            // for a Blueprint target as the actor branch does — under the same key names, but with
            // a source-appropriate field set (the two targets persist config in different stores;
            // see AddBlueprintInteractionReadback). Each field is emitted only when configured.
            AddBlueprintInteractionReadback(Result, Blueprint);
        }
    }

    if (!ActorName.IsEmpty())
    {
        UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
        if (World)
        {
            AActor* FoundActor = McpActorUtils::FindActorByNameSimple(World, ActorName);
            if (FoundActor)
            {
                Result->SetStringField(TEXT("actorName"), FoundActor->GetName());
                Result->SetStringField(TEXT("actorClass"), FoundActor->GetClass()->GetName());
            }
        }
    }

    Ctx.SendSuccess(Result);
    return true;
}
