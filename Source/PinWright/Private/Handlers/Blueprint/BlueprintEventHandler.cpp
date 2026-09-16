// Copyright (c) 2026 Alexander Penkin. MIT License.

// BlueprintEventHandler.cpp - Migrated from PinWright_BlueprintHandlers.cpp
// Blueprint event management: add_event, remove_event

#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/ErrorCodes.h"
#include "Handlers/Blueprint/BlueprintHandlerUtils.h"
#include "Handlers/Blueprint/BlueprintGraphHelpers.h"
#include "Compiler/BpirTypeSpec.h"
#include "Compiler/BpirTypeSpecParser.h"
#include "Decompiler/BpirInputKeyHelpers.h"
#include "Misc/ScopeExit.h"

#include "Kismet2/BlueprintEditorUtils.h"
#include "ScopedTransaction.h"
#include "GameFramework/Actor.h"

#include "K2Node_ComponentBoundEvent.h"
#include "K2Node_InputKey.h"
#if __has_include("K2Node_ActorBoundEvent.h")
#include "K2Node_ActorBoundEvent.h"
#define MCP_EVENT_HAS_ACTOR_BOUND_EVENT 1
#else
#define MCP_EVENT_HAS_ACTOR_BOUND_EVENT 0
#endif

using namespace BlueprintHandlerUtils;

namespace
{
    void ParseInputKeyRemoveEventName(const FString& EventName, FString& OutKeyIdentifier, bool& bOutHasSense, bool& bOutReleased)
    {
        OutKeyIdentifier = EventName.TrimStartAndEnd();
        bOutHasSense = false;
        bOutReleased = false;

        const FString PressedPrefix = TEXT("key_pressed ");
        const FString ReleasedPrefix = TEXT("key_released ");
        if (OutKeyIdentifier.StartsWith(PressedPrefix, ESearchCase::IgnoreCase))
        {
            bOutHasSense = true;
            bOutReleased = false;
            OutKeyIdentifier = OutKeyIdentifier.RightChop(PressedPrefix.Len()).TrimStartAndEnd();
        }
        else if (OutKeyIdentifier.StartsWith(ReleasedPrefix, ESearchCase::IgnoreCase))
        {
            bOutHasSense = true;
            bOutReleased = true;
            OutKeyIdentifier = OutKeyIdentifier.RightChop(ReleasedPrefix.Len()).TrimStartAndEnd();
        }

        if (OutKeyIdentifier.EndsWith(TEXT("()")))
        {
            OutKeyIdentifier.LeftChopInline(2);
            OutKeyIdentifier = OutKeyIdentifier.TrimStartAndEnd();
        }
    }
}

// ---- blueprint.add_event ----
REGISTER_RPC_HANDLER("blueprint.add_event", "blueprint", "Add an event entry node to a Blueprint's EventGraph. eventType picks between built-in lifecycle events (BeginPlay, Tick, etc.) and user-defined CustomEvents. The returned parameters[] is read back off the created node's pins, not echoed from the request, and a pin that could not be created is reported as PIN_CREATION_FAILED rather than as success. Wire downstream logic via blueprint.compile_bpir or blueprint.graph.create_node + connect_pins.",
    RPC_PARAMS(
        BlueprintPathParamReq(TEXT("path"), TEXT("path"), TEXT("Blueprint asset path.")),
        RPC_PARAM_OPT("eventType", "string", "'custom' for a CustomEvent, or a built-in name like 'BeginPlay' / 'Tick' / 'EndPlay'. Built-in events with matching signatures are deduped."),
        RPC_PARAM_OPT("customEventName", "string", "Identifier for the new node when eventType='custom'."),
        RPC_PARAM_OPT("parameters", "array", "Array of {name, type} pin definitions. Custom events only - supplying it with a built-in eventType is rejected with UNSUPPORTED_ARGUMENT instead of being dropped. A type that does not resolve to a concrete pin is rejected with TYPE_NOT_FOUND before anything is created, rather than becoming a wildcard pin."),
        RPC_PARAM_REQ("x", "number", "Event-graph canvas X coordinate (required to avoid stacking nodes at origin)."),
        RPC_PARAM_REQ("y", "number", "Event-graph canvas Y coordinate.")
    ))
{
    FString Path = ResolveBlueprintPath(Ctx);
    if (Path.IsEmpty())
    {
        Ctx.SendError(TEXT("INVALID_BLUEPRINT_PATH"), TEXT("blueprint.add_event requires a blueprint path."));
        return true;
    }

    // Validate position up front so failures cannot dirty the Blueprint by leaving
    // a freshly-created EventGraph attached after AddUbergraphPage().
    // Note: the legacy `location: {x, y}` object alias is intentionally removed —
    // position is now a required top-level contract per the mandatory-position sweep.
    double EventPosXD = 0.0, EventPosYD = 0.0;
    if (!Ctx.RequireNumber(TEXT("x"), EventPosXD)) return true;
    if (!Ctx.RequireNumber(TEXT("y"), EventPosYD)) return true;
    int32 EventPosX = static_cast<int32>(EventPosXD);
    int32 EventPosY = static_cast<int32>(EventPosYD);

    const TSharedPtr<FJsonObject>& Payload = Ctx.GetRawPayload();

    FString EventType;
    Payload->TryGetStringField(TEXT("eventType"), EventType);
    FString CustomName;
    Payload->TryGetStringField(TEXT("customEventName"), CustomName);
    const TArray<TSharedPtr<FJsonValue>>* ParamsField = nullptr;
    Payload->TryGetArrayField(TEXT("parameters"), ParamsField);
    TArray<TSharedPtr<FJsonValue>> Params =
        (ParamsField && ParamsField->Num() > 0)
            ? *ParamsField
            : TArray<TSharedPtr<FJsonValue>>();

    const FString FinalType = EventType.IsEmpty() ? TEXT("custom") : EventType;
    const bool bIsCustomEvent = FinalType.Equals(TEXT("custom"), ESearchCase::IgnoreCase);

    // Validate the requested pins BEFORE anything is created, for the same reason the position
    // check above runs up front: a rejection must leave the Blueprint untouched. Mirrors
    // BlueprintFunctionHandler.cpp:150-163.
    //
    // Only custom events get user-defined pins - the authoring block below is inside
    // `if (bIsCustomEvent)`. A `parameters` array on a built-in event was silently dropped from
    // the graph and then echoed back in the response and written to the registry, so the caller
    // was told about pins that exist nowhere.
    if (Params.Num() > 0 && !bIsCustomEvent)
    {
        Ctx.SendError(ErrorCodes::ERR_UNSUPPORTED_ARGUMENT, FString::Printf(
            TEXT("'parameters' applies only to eventType 'custom'; a built-in event node ('%s') ")
            TEXT("takes its signature from the overridden function and cannot carry user-defined ")
            TEXT("pins. Nothing was added. Drop 'parameters', or use eventType 'custom'."),
            *FinalType));
        return true;
    }

    // Parse once, here. AddUserDefinedPin falls back to a PC_Wildcard pin when the spec does not
    // resolve and still returns true, so an unresolvable type used to produce a malformed pin
    // that nothing reported - it was not even counted as a failure. Refuse it up front with the
    // shared scan blueprint.add_function / networking.create_rpc_function already use, and hand
    // the SAME parsed specs to the authoring loop so the thing validated is the thing written.
    TArray<FParsedPinParam> ParsedParams;
    FString ParamParseError;
    ParseNamedTypePinParams(Params, ParsedParams, EParsedPinParamMode::AllowWildcardFallback,
        ParamParseError, TEXT("event parameter"));

    // ParseNamedTypePinParams skips any entry that is not a JSON object, without a word. A
    // shorter output than input therefore means entries were dropped, which the caller has no
    // other way to learn.
    if (ParsedParams.Num() != Params.Num())
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT, FString::Printf(
            TEXT("'parameters' has %d entries but only %d of them are objects. Every entry must ")
            TEXT("be a {name, type} object; the rest would have been dropped silently. Nothing ")
            TEXT("was added."),
            Params.Num(), ParsedParams.Num()));
        return true;
    }
    // Named before typed: an unnamed pin cannot be created (AddUserDefinedPin rejects an empty
    // name) and the type-resolution scan below would blame the type for it.
    for (const FParsedPinParam& Param : ParsedParams)
    {
        if (Param.Name.IsEmpty())
        {
            Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT, FString::Printf(
                TEXT("An entry of 'parameters' has an empty 'name' (type '%s'). A pin cannot be ")
                TEXT("created without a name. Nothing was added."),
                *Param.OriginalType));
            return true;
        }
    }
    FString WildcardError;
    if (FindFirstPinParamWildcardFallback(ParsedParams, TEXT("event parameter"), WildcardError))
    {
        Ctx.SendError(ErrorCodes::ERR_TYPE_NOT_FOUND, WildcardError);
        return true;
    }

    if (FPluginState::Get().Blueprints().IsBusy(Path))
    {
        Ctx.SendError(TEXT("BLUEPRINT_BUSY"), TEXT("Blueprint is busy"));
        return true;
    }

    FPluginState::Get().Blueprints().MarkBusy(Path);
    ON_SCOPE_EXIT
    {
        if (FPluginState::Get().Blueprints().IsBusy(Path))
            FPluginState::Get().Blueprints().ClearBusy(Path);
    };

    auto* Subsystem = Ctx.GetSubsystem();
    FString Normalized, LoadErr;
    UBlueprint* BP = LoadBlueprintAsset(Path, Normalized, LoadErr);
    const FString RegistryKey = !Normalized.IsEmpty() ? Normalized : Path;
    if (!BP)
    {
        Ctx.SendError(TEXT("BLUEPRINT_NOT_FOUND"),
            LoadErr.IsEmpty() ? TEXT("Failed to load blueprint") : *LoadErr);
        return true;
    }

    // Find or create event graph
    UEdGraph* EventGraph = FBlueprintEditorUtils::FindEventGraph(BP);
    if (!EventGraph)
    {
        EventGraph = FBlueprintEditorUtils::CreateNewGraph(
            BP, TEXT("EventGraph"), UEdGraph::StaticClass(),
            UEdGraphSchema_K2::StaticClass());
        if (EventGraph)
            FBlueprintEditorUtils::AddUbergraphPage(BP, EventGraph);
    }

    if (!EventGraph)
    {
        Ctx.SendError(TEXT("GRAPH_UNAVAILABLE"), TEXT("Failed to create event graph"));
        return true;
    }

    FScopedTransaction Transaction(FText::FromString(TEXT("MCP: blueprint.add_event")));

    FName EventName;
    UK2Node_CustomEvent* CustomEventNode = nullptr;
    // Requested pins that are not on the node when the dust settles. Hoisted to handler scope
    // because it now decides the response: it used to be a block-local that went to a UE_LOG at
    // the bottom of the parameter loop and nowhere else, so the caller was told success while
    // the pins it was about to connect did not exist.
    TArray<FString> FailedParams;

    if (bIsCustomEvent)
    {
        EventName = CustomName.IsEmpty()
            ? FName(*FString::Printf(TEXT("Event_%s"), *FGuid::NewGuid().ToString()))
            : FName(*CustomName);

        // Check if custom event already exists
        for (UEdGraphNode* Node : EventGraph->Nodes)
        {
            if (UK2Node_CustomEvent* ExistingNode = Cast<UK2Node_CustomEvent>(Node))
            {
                if (ExistingNode->CustomFunctionName == EventName)
                {
                    CustomEventNode = ExistingNode;
                    break;
                }
            }
        }

        if (!CustomEventNode)
        {
            EventGraph->Modify();
            FGraphNodeCreator<UK2Node_CustomEvent> NodeCreator(*EventGraph);
            CustomEventNode = NodeCreator.CreateNode();
            CustomEventNode->CustomFunctionName = EventName;
            CustomEventNode->NodePosX = EventPosX;
            CustomEventNode->NodePosY = EventPosY;
            NodeCreator.Finalize();
            CustomEventNode->AllocateDefaultPins();
        }
        else
        {
            CustomEventNode->NodePosX = EventPosX;
            CustomEventNode->NodePosY = EventPosY;
        }

        // Handle parameters for custom events. ParsedParams carries the specs the up-front gate
        // accepted, so no type can silently degrade to a wildcard pin here any more; what remains
        // is a pin the node itself refused (empty name, duplicate name) or one lost to
        // ReconstructNode, and both of those now travel out in the response.
        if (CustomEventNode && ParsedParams.Num() > 0)
        {
            CustomEventNode->Modify();
            TSet<FString> FailedNames;
            for (const FParsedPinParam& Param : ParsedParams)
            {
                if (!AddUserDefinedPin(CustomEventNode, Param.Name, Param.Spec, EGPD_Output))
                {
                    FailedNames.Add(Param.Name);
                    FailedParams.Add(FString::Printf(TEXT("%s (%s): CreateUserDefinedPin refused it"),
                        *Param.OriginalName, *Param.OriginalType));
                }
            }
            CustomEventNode->ReconstructNode();
            // Verify pins after reconstruct. AddUserDefinedPin trims the name before creating the
            // pin, so Param.Name (already trimmed by ParseNamedTypePinParams) is the name to look
            // up - the old code searched with the raw, untrimmed request string.
            for (const FParsedPinParam& Param : ParsedParams)
            {
                if (Param.Name.IsEmpty() || FailedNames.Contains(Param.Name))
                {
                    continue;
                }
                if (!CustomEventNode->FindPin(*Param.Name))
                {
                    FailedNames.Add(Param.Name);
                    FailedParams.Add(FString::Printf(TEXT("%s (%s): lost after ReconstructNode"),
                        *Param.OriginalName, *Param.OriginalType));
                }
            }
            if (FailedParams.Num() > 0)
            {
                UE_LOG(LogPinWrightSubsystem, Warning, TEXT("blueprint_add_event: failed to create parameters: %s"),
                    *FString::Join(FailedParams, TEXT(", ")));
            }
        }
    }
    else
    {
        // Standard event logic
        FString TargetEventName = FinalType;
        // Class-aware shorthand: only apply the AActor Receive-prefix mapping when
        // the parent is an AActor subclass AND the literal name does not already
        // resolve on the parent. UUserWidget::Tick must stay literal "Tick".
        static const TMap<FString, FString> EventNameAliases = {
            {TEXT("BeginPlay"), TEXT("ReceiveBeginPlay")},
            {TEXT("Tick"), TEXT("ReceiveTick")},
            {TEXT("EndPlay"), TEXT("ReceiveEndPlay")},
        };

        if (BP && BP->ParentClass)
        {
            const bool bLiteralResolves = BP->ParentClass->FindFunctionByName(FName(*TargetEventName)) != nullptr;
            const bool bIsActorSubclass = BP->ParentClass->IsChildOf(AActor::StaticClass());
            if (!bLiteralResolves && bIsActorSubclass)
            {
                if (const FString* Alias = EventNameAliases.Find(TargetEventName))
                    TargetEventName = *Alias;
            }
        }

        EventName = FName(*TargetEventName);

        UClass* TargetClass = nullptr;
        UFunction* EventFunc = nullptr;

        // Search hierarchy
        UClass* SearchClass = BP->ParentClass;
        while (SearchClass && !EventFunc)
        {
            EventFunc = SearchClass->FindFunctionByName(
                *TargetEventName, EIncludeSuperFlag::ExcludeSuper);
            if (EventFunc)
            {
                TargetClass = SearchClass;
                break;
            }
            SearchClass = SearchClass->GetSuperClass();
        }

        if (!EventFunc)
        {
            Ctx.SendError(TEXT("EVENT_NOT_FOUND"),
                *FString::Printf(TEXT("Could not find event '%s' (resolved to '%s') in parent class."),
                    *FinalType, *TargetEventName));
            return true;
        }

        // Check if node already exists
        bool bExists = false;
        for (UEdGraphNode* Node : EventGraph->Nodes)
        {
            if (UK2Node_Event* EventNode = Cast<UK2Node_Event>(Node))
            {
                if (EventNode->EventReference.GetMemberName() == EventFunc->GetFName())
                {
                    bExists = true;
                    break;
                }
            }
        }

        if (!bExists)
        {
            EventGraph->Modify();
            FGraphNodeCreator<UK2Node_Event> NodeCreator(*EventGraph);
            UK2Node_Event* EventNode = NodeCreator.CreateNode();
            EventNode->EventReference.SetFromField<UFunction>(EventFunc, false);
            EventNode->bOverrideFunction = true;
            EventNode->NodePosX = EventPosX;
            EventNode->NodePosY = EventPosY;
            NodeCreator.Finalize();
        }
    }

    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);
    const FBlueprintCompileDiagnostics CompileDiagnostics =
        CompileBlueprintWithDiagnostics(BP);
    // WasSavePersisted, not the raw call: the throttle skip and the transient-package
    // early-out used to return true, so a second edit inside the 0.5s window reported
    // saved:true with the change still only in memory.
    const bool bSaved = WasSavePersisted(SaveLoadedAssetThrottled(BP));

    // MEASURED parameter pins, read off the node's UserDefinedPins through the same
    // CollectEventPins reader blueprint.get's graph snapshot uses (BlueprintHandlerUtils.cpp,
    // CollectBlueprintEvents -> CollectEventPins). The registry and the response below both take
    // THIS array. Neither may take the caller's request array: writing the request into the
    // registry is what let blueprint.get re-report pins that were never created, so the write
    // path and the readback corroborated each other while both disagreed with the graph.
    // Empty for a built-in event node, which has no user-defined pins by construction.
    TArray<TSharedPtr<FJsonValue>> MeasuredParams;
    CollectEventPins(CustomEventNode, MeasuredParams);

    // Update registry
    TSharedPtr<FJsonObject> Entry = EnsureBlueprintEntry(RegistryKey);
    TArray<TSharedPtr<FJsonValue>> Events =
        Entry->HasField(TEXT("events"))
            ? Entry->GetArrayField(TEXT("events"))
            : TArray<TSharedPtr<FJsonValue>>();
    bool bFound = false;
    for (const TSharedPtr<FJsonValue>& Item : Events)
    {
        if (!Item.IsValid() || Item->Type != EJson::Object) continue;
        const TSharedPtr<FJsonObject> Obj = Item->AsObject();
        if (Obj.IsValid())
        {
            FString Existing;
            if (Obj->TryGetStringField(TEXT("name"), Existing) &&
                Existing.Equals(EventName.ToString(), ESearchCase::IgnoreCase))
            {
                Obj->SetStringField(TEXT("eventType"), FinalType);
                if (MeasuredParams.Num() > 0) Obj->SetArrayField(TEXT("parameters"), MeasuredParams);
                else Obj->RemoveField(TEXT("parameters"));
                bFound = true;
                break;
            }
        }
    }

    if (!bFound)
    {
        TSharedPtr<FJsonObject> Rec = MakeShared<FJsonObject>();
        Rec->SetStringField(TEXT("name"), EventName.ToString());
        Rec->SetStringField(TEXT("eventType"), FinalType);
        if (MeasuredParams.Num() > 0) Rec->SetArrayField(TEXT("parameters"), MeasuredParams);
        Events.Add(MakeShared<FJsonValueObject>(Rec));
    }

    Entry->SetArrayField(TEXT("events"), Events);

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetStringField(TEXT("blueprintPath"), RegistryKey);
    Resp->SetStringField(TEXT("eventName"), EventName.ToString());
    Resp->SetStringField(TEXT("eventType"), FinalType);
    Resp->SetBoolField(TEXT("saved"), bSaved);
    // The pins that ARE on the node, not the pins that were asked for.
    if (MeasuredParams.Num() > 0) Resp->SetArrayField(TEXT("parameters"), MeasuredParams);
    AddAssetVerification(Resp, BP);
    AddCompileDiagnosticsToJson(CompileDiagnostics, Resp);

    if (FailedParams.Num() > 0)
    {
        // The event node exists but the graph does not match the request, so this is not a
        // success. The old code hardcoded success:true here and echoed the request array as
        // `parameters`, which sent the agent on to connect_pins against pins that were never
        // created. The result object rides along on the error so the caller can see which pins
        // DO exist without a second round trip.
        Resp->SetBoolField(TEXT("success"), false);
        TArray<TSharedPtr<FJsonValue>> FailedJson;
        for (const FString& Failure : FailedParams)
        {
            FailedJson.Add(MakeShared<FJsonValueString>(Failure));
        }
        Resp->SetArrayField(TEXT("failedParameters"), FailedJson);
        Ctx.SendError(ErrorCodes::ERR_PIN_CREATION_FAILED, FString::Printf(
            TEXT("Event '%s' was created but %d requested parameter pin(s) were not: %s. ")
            TEXT("The 'parameters' field of this response lists the pins that actually exist on ")
            TEXT("the node; do not connect to any pin outside it."),
            *EventName.ToString(), FailedParams.Num(), *FString::Join(FailedParams, TEXT("; "))),
            Resp);
        return true;
    }

    Resp->SetBoolField(TEXT("success"), true);
    Ctx.SendSuccess(Resp);
    return true;
}

// ---- blueprint.remove_event ----
REGISTER_RPC_HANDLER("blueprint.remove_event", "blueprint", "Remove an event from a Blueprint",
    RPC_PARAMS(
        BlueprintPathParamReq(TEXT("path"), TEXT("path"), TEXT("Blueprint asset path")),
        RPC_PARAM_REQ("eventName", "string", "Name of the event to remove"),
        RPC_PARAM_OPT("componentName", "string", "Optional: when a blueprint has multiple events bound to different components sharing the same delegate signature, pass the owning component's property name (e.g. `BT_MyTracks`) to target just that one."),
        RPC_PARAM_OPT("nodeId", "string", "Optional: a specific K2Node_ComponentBoundEvent node GUID to target unambiguously. Takes precedence over componentName."),
        RPC_PARAM_DEF("cleanupNewOrphans", "boolean", "Delete nodes that become orphaned by this deletion (default: true)", "true")
    ))
{
    FString Path = ResolveBlueprintPath(Ctx);
    if (Path.IsEmpty())
    {
        Ctx.SendError(TEXT("INVALID_BLUEPRINT_PATH"), TEXT("blueprint.remove_event requires a blueprint path."));
        return true;
    }

    const TSharedPtr<FJsonObject>& Payload = Ctx.GetRawPayload();
    FString EventName;
    Payload->TryGetStringField(TEXT("eventName"), EventName);
    if (EventName.IsEmpty())
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("eventName required"));
        return true;
    }

    FString ComponentName;
    FString NodeId;
    Payload->TryGetStringField(TEXT("componentName"), ComponentName);
    Payload->TryGetStringField(TEXT("nodeId"), NodeId);
    const bool bCleanupNewOrphans = Ctx.GetBool(TEXT("cleanupNewOrphans"), true);

    auto* Subsystem = Ctx.GetSubsystem();
    FString NormPath;
    const FString RegistryPath =
        (FindBlueprintNormalizedPath(Path, NormPath) && !NormPath.IsEmpty())
            ? NormPath
            : Path;

    // Look up event in registry
    TSharedPtr<FJsonObject> Entry = EnsureBlueprintEntry(RegistryPath);
    TArray<TSharedPtr<FJsonValue>> Events =
        Entry->HasField(TEXT("events"))
            ? Entry->GetArrayField(TEXT("events"))
            : TArray<TSharedPtr<FJsonValue>>();
    int32 FoundIdx = INDEX_NONE;
    for (int32 i = 0; i < Events.Num(); ++i)
    {
        const TSharedPtr<FJsonValue>& V = Events[i];
        if (!V.IsValid() || V->Type != EJson::Object) continue;
        const TSharedPtr<FJsonObject> Obj = V->AsObject();
        FString CandidateName;
        if (Obj->TryGetStringField(TEXT("name"), CandidateName) &&
            CandidateName.Equals(EventName, ESearchCase::IgnoreCase))
        {
            FoundIdx = i;
            break;
        }
    }

    FScopedTransaction Transaction(FText::FromString(TEXT("MCP: blueprint.remove_event")));

    // --- Remove from graph (works for events created by any path) ---
    bool bRemovedFromGraph = false;
    int32 TotalRemovedNodeCount = 0;
    int32 TotalCascadedCreateDelegatesRemoved = 0;
    bool bHasOrphanCleanup = false;
    BlueprintHandlerUtils::FBlueprintOrphanDeltaCleanupResult OrphanCleanup;
    FBlueprintCompileDiagnostics CompileDiagnostics;
    bool bCompileAttempted = false;
    {
        FString NormalizedRemove, RemoveLoadErr;
        UBlueprint* RemoveBlueprint =
            LoadBlueprintAsset(RegistryPath, NormalizedRemove, RemoveLoadErr);
        if (RemoveBlueprint)
        {
            const TSet<FGuid> OrphansBefore =
                BlueprintHandlerUtils::SnapshotBlueprintOrphanGuids(RemoveBlueprint, true);

            TArray<UEdGraphNode*> EventRootNodes;
            FString InputKeyIdentifier;
            bool bInputKeyHasSense = false;
            bool bInputKeyReleased = false;
            ParseInputKeyRemoveEventName(EventName, InputKeyIdentifier, bInputKeyHasSense, bInputKeyReleased);

            // Disambiguator: nodeId wins when set; otherwise componentName filters
            // ComponentBoundEvent candidates (branches without a component ignore it).
            auto PassesDisambiguator = [&](const FGuid& NodeGuid, const FName& ComponentProp)
            {
                if (!NodeId.IsEmpty())
                {
                    return BlueprintGraphHelpers::NodeGuidMatchesId(NodeGuid, NodeId);
                }
                if (!ComponentName.IsEmpty() && ComponentProp != NAME_None)
                {
                    return ComponentProp.ToString().Equals(ComponentName, ESearchCase::IgnoreCase);
                }
                return true;
            };

            // Search UbergraphPages for custom events and standard event overrides
            for (UEdGraph* Graph : RemoveBlueprint->UbergraphPages)
            {
                for (UEdGraphNode* Node : Graph->Nodes)
                {
                    if (UK2Node_InputKey* InputKeyNode = Cast<UK2Node_InputKey>(Node))
                    {
                        const bool bSenseMatches =
                            !bInputKeyHasSense
                            || FBpirInputKeyHelpers::IsInputKeyExecPinActive(InputKeyNode, bInputKeyReleased);
                        if (FBpirInputKeyHelpers::DoesInputKeyMatchBpirIdentifier(InputKeyNode, InputKeyIdentifier)
                            && bSenseMatches
                            && PassesDisambiguator(InputKeyNode->NodeGuid, NAME_None))
                        {
                            EventRootNodes.Add(InputKeyNode);
                        }
                    }
                    else if (UK2Node_CustomEvent* CustomEvent = Cast<UK2Node_CustomEvent>(Node))
                    {
                        if (CustomEvent->CustomFunctionName.ToString().Equals(
                                EventName, ESearchCase::IgnoreCase)
                            && PassesDisambiguator(CustomEvent->NodeGuid, NAME_None))
                        {
                            EventRootNodes.Add(CustomEvent);
                        }
                    }
                    else if (UK2Node_ComponentBoundEvent* CompBoundEvent =
                                 Cast<UK2Node_ComponentBoundEvent>(Node))
                    {
                        // Match by any representation the caller may supply:
                        // 1. Full display title e.g. "On Clicked (RenameButton)"
                        // 2. Rebuilt "DelegateDisplay (ComponentProperty)" form
                        // 3. Bare DelegatePropertyName FName
                        // 4. Mangled CustomFunctionName (BndEvt__ form)
                        // Note: ComponentPropertyName read is safe even when the source
                        //       component has been removed — FName stores the string.
                        const FString FullTitle =
                            CompBoundEvent->GetNodeTitle(ENodeTitleType::FullTitle).ToString();
                        const FString DelegateDisplay =
                            CompBoundEvent->GetTargetDelegateDisplayName().ToString();
                        const FString ComponentPropName =
                            CompBoundEvent->ComponentPropertyName.ToString();
                        const FString RebuiltTitle =
                            FString::Printf(TEXT("%s (%s)"), *DelegateDisplay, *ComponentPropName);
                        const FString BareDelegateName =
                            CompBoundEvent->DelegatePropertyName.ToString();
                        const FString MangledName =
                            CompBoundEvent->CustomFunctionName.ToString();

                        if ((FullTitle.Equals(EventName, ESearchCase::IgnoreCase)
                                || RebuiltTitle.Equals(EventName, ESearchCase::IgnoreCase)
                                || BareDelegateName.Equals(EventName, ESearchCase::IgnoreCase)
                                || MangledName.Equals(EventName, ESearchCase::IgnoreCase))
                            && PassesDisambiguator(CompBoundEvent->NodeGuid, CompBoundEvent->ComponentPropertyName))
                        {
                            EventRootNodes.Add(CompBoundEvent);
                        }
                    }
#if MCP_EVENT_HAS_ACTOR_BOUND_EVENT
                    else if (UK2Node_ActorBoundEvent* ActorBoundEvent =
                                 Cast<UK2Node_ActorBoundEvent>(Node))
                    {
                        // Match by full display title or bare DelegatePropertyName
                        const FString FullTitle =
                            ActorBoundEvent->GetNodeTitle(ENodeTitleType::FullTitle).ToString();
                        const FString BareDelegateName =
                            ActorBoundEvent->DelegatePropertyName.ToString();

                        if ((FullTitle.Equals(EventName, ESearchCase::IgnoreCase)
                                || BareDelegateName.Equals(EventName, ESearchCase::IgnoreCase))
                            && PassesDisambiguator(ActorBoundEvent->NodeGuid, NAME_None))
                        {
                            EventRootNodes.Add(ActorBoundEvent);
                        }
                    }
#endif // MCP_EVENT_HAS_ACTOR_BOUND_EVENT
                    else if (UK2Node_Event* EventNode = Cast<UK2Node_Event>(Node))
                    {
                        if (EventNode->EventReference.GetMemberName().ToString().Equals(
                                EventName, ESearchCase::IgnoreCase)
                            && PassesDisambiguator(EventNode->NodeGuid, NAME_None))
                        {
                            EventRootNodes.Add(EventNode);
                        }
                    }
                }
            }

            // Search FunctionGraphs for override functions (e.g. GetContentPanel)
            if (EventRootNodes.Num() == 0)
            {
                for (UEdGraph* Graph : RemoveBlueprint->FunctionGraphs)
                {
                    if (!Graph->GetFName().ToString().Equals(
                            EventName, ESearchCase::IgnoreCase))
                        continue;
                    for (UEdGraphNode* Node : Graph->Nodes)
                    {
                        if (UK2Node_FunctionEntry* EntryNode = Cast<UK2Node_FunctionEntry>(Node))
                        {
                            EventRootNodes.Add(EntryNode);
                        }
                    }
                }
            }

            // Collect downstream exec chain body nodes for cleanup
            TSet<UEdGraphNode*> AllNodesToRemove;
            CollectExecChainNodes(EventRootNodes, AllNodesToRemove);

            // Capture function names from event ROOT nodes BEFORE removal — after
            // RemoveNode the node pointers may be invalid.
            TSet<FName> RemovedFunctionNames;
            for (UEdGraphNode* RootNode : EventRootNodes)
            {
                const FName Name = GetEffectiveFunctionNameForRemoval(RootNode);
                if (Name != NAME_None)
                {
                    RemovedFunctionNames.Add(Name);
                }
            }

            TotalRemovedNodeCount = AllNodesToRemove.Num();
            for (UEdGraphNode* Node : AllNodesToRemove)
                FBlueprintEditorUtils::RemoveNode(RemoveBlueprint, Node, true);

            TotalCascadedCreateDelegatesRemoved =
                CascadeRemoveStaleCreateDelegates(RemoveBlueprint, RemovedFunctionNames);

            OrphanCleanup = BlueprintHandlerUtils::CleanupNewBlueprintOrphans(
                RemoveBlueprint, OrphansBefore, bCleanupNewOrphans, true);
            bHasOrphanCleanup = true;

            if (TotalRemovedNodeCount > 0)
            {
                bRemovedFromGraph = true;
                FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(RemoveBlueprint);
                CompileDiagnostics = CompileBlueprintWithDiagnostics(RemoveBlueprint);
                bCompileAttempted = true;
                SaveLoadedAssetThrottled(RemoveBlueprint);
            }
        }
    }

    // --- Remove from registry ---
    if (FoundIdx != INDEX_NONE)
    {
        Events.RemoveAt(FoundIdx);
        Entry->SetArrayField(TEXT("events"), Events);
    }

    // If neither registry nor graph had the event, treat as idempotent no-op
    if (FoundIdx == INDEX_NONE && !bRemovedFromGraph)
    {
        TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
        Resp->SetStringField(TEXT("eventName"), EventName);
        Resp->SetStringField(TEXT("blueprintPath"), Path);
        Resp->SetNumberField(TEXT("removedNodeCount"), 0);
        Resp->SetStringField(TEXT("note"),
            TEXT("Event not present; treated as removed (idempotent)."));
        Ctx.SendSuccess(Resp);
        return true;
    }

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetStringField(TEXT("eventName"), EventName);
    Resp->SetStringField(TEXT("blueprintPath"), RegistryPath);
    Resp->SetNumberField(TEXT("removedNodeCount"), TotalRemovedNodeCount);
    Resp->SetNumberField(TEXT("cascadedCreateDelegatesRemoved"), TotalCascadedCreateDelegatesRemoved);
    if (bHasOrphanCleanup)
    {
        BlueprintHandlerUtils::AddOrphanDeltaCleanupResultToJson(OrphanCleanup, Resp);
    }
    if (bCompileAttempted)
    {
        AddCompileDiagnosticsToJson(CompileDiagnostics, Resp);
    }
    Ctx.SendSuccess(Resp);
    return true;
}
