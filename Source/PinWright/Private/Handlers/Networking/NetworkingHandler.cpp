// Copyright (c) 2026 Alexander Penkin. MIT License.

// NetworkingHandler.cpp - Migrated from PinWright_NetworkingHandlers.cpp
// Networking & multiplayer: replication, RPCs, authority, relevancy, serialization, prediction

#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/ParamSpec.h"
#include "Handlers/Blueprint/BlueprintHandlerUtils.h"
#include "Handlers/Networking/MovementPredictionUtils.h"
#include "PinWrightGlobals.h"
#include "PinWrightHelpers.h"
#include "PinWrightSubsystem.h"
#include "Utils/ActorUtils.h"
#include "Dom/JsonObject.h"
#include "Compat/EngineVersionCompat.h"

#include "Editor.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "EditorAssetLibrary.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerController.h"
#include "Net/UnrealNetwork.h"
#include "UObject/UnrealType.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#include "K2Node_CallFunction.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "Handlers/Blueprint/BlueprintHandlerUtils.h"
#include "Handlers/Blueprint/BlueprintPathLoad.h"

DEFINE_LOG_CATEGORY_STATIC(LogMcpNetworkingHandlers, Log, All);

// ---------------------------------------------------------------------------
// Static helpers
// ---------------------------------------------------------------------------

namespace NetworkingHelpersNew
{
    // One shared body, in Handlers/Blueprint/BlueprintPathLoad.h. This file and
    // GameFrameworkHandler.cpp each carried a verbatim copy of it; the "//" refusal the merged
    // body now performs is the reason they must not, because a guard duplicated across two files
    // is a guard that has to be remembered twice. The using-declaration keeps the name a member of
    // this namespace, so the nineteen call sites below - which reach it through
    // `using namespace NetworkingHelpersNew` - are unchanged.
    using PinWrightBlueprintPathLoad::LoadBlueprintFromPath;

    // Resolve an actor by the same label-OR-internal-name-OR-path resolution semantics
    // the actor.* family uses (the bug that was reported), case-insensitive. Delegates to
    // McpActorUtils::FindActorByNameSimple — NOT the richer McpActorUtils::FindActorByName
    // (which adds PIE-world priority, a single-fuzzy-match fallback, and asset-path load):
    // Simple is the deliberate choice here because it honors the passed World, and every
    // networking call site passes GEditor->GetEditorWorldContext().World(), whereas the
    // full FindActorByName ignores its World arg and prioritizes GEditor->PlayWorld — which
    // would silently re-target these editor-world lookups into PIE. The old private body
    // matched GetName() ONLY (case-sensitive), which rejected the display label that
    // actor.spawn/actor.list accept AND that set_owner's own success verification echoes
    // back as actorName — so re-using the echoed value round-tripped to [NOT_FOUND]
    // (board E-networking-actorname-internal-name-only).
    static AActor* FindActorByName(UWorld* World, const FString& ActorName)
    {
        return McpActorUtils::FindActorByNameSimple(World, ActorName);
    }

    static ELifetimeCondition GetReplicationCondition(const FString& ConditionStr)
    {
        if (ConditionStr == TEXT("COND_None")) return COND_None;
        if (ConditionStr == TEXT("COND_InitialOnly")) return COND_InitialOnly;
        if (ConditionStr == TEXT("COND_OwnerOnly")) return COND_OwnerOnly;
        if (ConditionStr == TEXT("COND_SkipOwner")) return COND_SkipOwner;
        if (ConditionStr == TEXT("COND_SimulatedOnly")) return COND_SimulatedOnly;
        if (ConditionStr == TEXT("COND_AutonomousOnly")) return COND_AutonomousOnly;
        if (ConditionStr == TEXT("COND_SimulatedOrPhysics")) return COND_SimulatedOrPhysics;
        if (ConditionStr == TEXT("COND_InitialOrOwner")) return COND_InitialOrOwner;
        if (ConditionStr == TEXT("COND_Custom")) return COND_Custom;
        if (ConditionStr == TEXT("COND_ReplayOrOwner")) return COND_ReplayOrOwner;
        if (ConditionStr == TEXT("COND_ReplayOnly")) return COND_ReplayOnly;
        if (ConditionStr == TEXT("COND_SimulatedOnlyNoReplay")) return COND_SimulatedOnlyNoReplay;
        if (ConditionStr == TEXT("COND_SimulatedOrPhysicsNoReplay")) return COND_SimulatedOrPhysicsNoReplay;
        if (ConditionStr == TEXT("COND_SkipReplay")) return COND_SkipReplay;
        if (ConditionStr == TEXT("COND_Never")) return COND_Never;
        return COND_None;
    }

    static ENetDormancy GetNetDormancy(const FString& DormancyStr)
    {
        if (DormancyStr == TEXT("DORM_Never")) return DORM_Never;
        if (DormancyStr == TEXT("DORM_Awake")) return DORM_Awake;
        if (DormancyStr == TEXT("DORM_DormantAll")) return DORM_DormantAll;
        if (DormancyStr == TEXT("DORM_DormantPartial")) return DORM_DormantPartial;
        if (DormancyStr == TEXT("DORM_Initial")) return DORM_Initial;
        return DORM_Never;
    }

    static FString NetRoleToString(ENetRole Role)
    {
        switch (Role)
        {
            case ROLE_None: return TEXT("ROLE_None");
            case ROLE_SimulatedProxy: return TEXT("ROLE_SimulatedProxy");
            case ROLE_AutonomousProxy: return TEXT("ROLE_AutonomousProxy");
            case ROLE_Authority: return TEXT("ROLE_Authority");
            default: return TEXT("ROLE_Unknown");
        }
    }

    static FString NetDormancyToString(ENetDormancy Dormancy)
    {
        switch (Dormancy)
        {
            case DORM_Never: return TEXT("DORM_Never");
            case DORM_Awake: return TEXT("DORM_Awake");
            case DORM_DormantAll: return TEXT("DORM_DormantAll");
            case DORM_DormantPartial: return TEXT("DORM_DormantPartial");
            case DORM_Initial: return TEXT("DORM_Initial");
            default: return TEXT("DORM_Unknown");
        }
    }

    // Inverse of GetReplicationCondition — used by the get_networking_info reader to
    // round-trip the ELifetimeCondition that set_replication_condition writes into
    // FBPVariableDescription::ReplicationCondition. ELifetimeCondition is a reflected
    // UENUM(BlueprintType) (CoreNetTypes.h) and is not an enum class, so the engine
    // returns the bare COND_* enumerator name — mirroring the module's established
    // StaticEnum<T>()->GetNameStringByValue idiom and tracking any future COND_*.
    static FString ReplicationConditionToString(ELifetimeCondition Condition)
    {
        if (const UEnum* Enum = StaticEnum<ELifetimeCondition>())
        {
            const FString Name = Enum->GetNameStringByValue(static_cast<int64>(Condition));
            if (!Name.IsEmpty())
            {
                return Name;
            }
        }
        return TEXT("COND_None");
    }

    // Classify an RPC function from the net extra-flags stamped on its
    // UK2Node_FunctionEntry by create_rpc_function/set_rpc_reliability/configure_rpc_validation.
    // Returns Server/Client/NetMulticast, or empty when the graph carries no FUNC_Net.
    static FString NetFunctionTypeToString(int32 FunctionFlags)
    {
        if ((FunctionFlags & FUNC_Net) == 0) return FString();
        if (FunctionFlags & FUNC_NetServer) return TEXT("Server");
        if (FunctionFlags & FUNC_NetClient) return TEXT("Client");
        if (FunctionFlags & FUNC_NetMulticast) return TEXT("NetMulticast");
        return TEXT("Net");
    }

    static bool TryParseRpcType(const FString& RpcType, int32& OutDirectionFlag,
        FString& OutCanonicalType)
    {
        if (RpcType.Equals(TEXT("Server"), ESearchCase::IgnoreCase))
        {
            OutDirectionFlag = FUNC_NetServer;
            OutCanonicalType = TEXT("Server");
            return true;
        }
        if (RpcType.Equals(TEXT("Client"), ESearchCase::IgnoreCase))
        {
            OutDirectionFlag = FUNC_NetClient;
            OutCanonicalType = TEXT("Client");
            return true;
        }
        if (RpcType.Equals(TEXT("NetMulticast"), ESearchCase::IgnoreCase))
        {
            OutDirectionFlag = FUNC_NetMulticast;
            OutCanonicalType = TEXT("NetMulticast");
            return true;
        }
        return false;
    }

    static bool HasValidRpcDirection(int32 FunctionFlags)
    {
        if ((FunctionFlags & FUNC_Net) == 0)
        {
            return false;
        }

        const int32 DirectionCount = ((FunctionFlags & FUNC_NetServer) != 0 ? 1 : 0)
            + ((FunctionFlags & FUNC_NetClient) != 0 ? 1 : 0)
            + ((FunctionFlags & FUNC_NetMulticast) != 0 ? 1 : 0);
        if (DirectionCount != 1)
        {
            return false;
        }

        return true;
    }

    // Walk a Blueprint's authored function graphs and emit one record per RPC
    // (FUNC_Net) function: name, rpcType, reliable, and — the field no other
    // reader surfaces — withValidation (FUNC_NetValidate). Reads the exact same
    // UK2Node_FunctionEntry extra-flags the networking setters write.
    static TArray<TSharedPtr<FJsonValue>> BuildRpcFunctionsArray(UBlueprint* Blueprint)
    {
        TArray<TSharedPtr<FJsonValue>> Out;
        if (!Blueprint) return Out;

        for (UEdGraph* Graph : Blueprint->FunctionGraphs)
        {
            if (!Graph) continue;
            for (UEdGraphNode* Node : Graph->Nodes)
            {
                UK2Node_FunctionEntry* EntryNode = Cast<UK2Node_FunctionEntry>(Node);
                if (!EntryNode) continue;

                const int32 Flags = EntryNode->GetExtraFlags();
                const FString RpcType = NetFunctionTypeToString(Flags);
                if (RpcType.IsEmpty()) break; // not an RPC graph; stop at the entry node

                TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
                Entry->SetStringField(TEXT("name"), Graph->GetName());
                Entry->SetStringField(TEXT("rpcType"), RpcType);
                Entry->SetBoolField(TEXT("reliable"), (Flags & FUNC_NetReliable) != 0);
                Entry->SetBoolField(TEXT("withValidation"), (Flags & FUNC_NetValidate) != 0);
                Out.Add(MakeShared<FJsonValueObject>(Entry));
                break; // one entry node per graph
            }
        }
        return Out;
    }

    // Emit one record per replicated Blueprint variable (CPF_Net): name, replicated,
    // and — the fields no other reader surfaces — the RepNotify function NAME and the
    // replication CONDITION. Reads FBPVariableDescription::RepNotifyFunc /
    // ::ReplicationCondition that set_replicated_using/set_replication_condition write.
    static TArray<TSharedPtr<FJsonValue>> BuildReplicatedPropertiesArray(UBlueprint* Blueprint)
    {
        TArray<TSharedPtr<FJsonValue>> Out;
        if (!Blueprint) return Out;

        for (const FBPVariableDescription& VarDesc : Blueprint->NewVariables)
        {
            if ((VarDesc.PropertyFlags & CPF_Net) == 0) continue;

            TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
            Entry->SetStringField(TEXT("name"), VarDesc.VarName.ToString());
            Entry->SetBoolField(TEXT("replicated"), true);
            if (VarDesc.RepNotifyFunc != NAME_None)
            {
                Entry->SetStringField(TEXT("replicatedUsing"), VarDesc.RepNotifyFunc.ToString());
            }
            else
            {
                Entry->SetField(TEXT("replicatedUsing"), MakeShared<FJsonValueNull>());
            }
            Entry->SetStringField(TEXT("replicationCondition"),
                ReplicationConditionToString(VarDesc.ReplicationCondition));
            Out.Add(MakeShared<FJsonValueObject>(Entry));
        }
        return Out;
    }

    // A CPF_Net property only actually replicates if the OWNING ACTOR itself
    // replicates (bReplicates). Every verb that stamps CPF_Net on a property
    // (set_property_replicated, set_replication_condition, set_replicated_using,
    // add_network_prediction_data) shares this dependency: the property is
    // functionally inert until the actor is made to replicate. None of these
    // setters flips the actor-level CDO toggle on its own — auto-mutating a
    // persistent flag the caller never asked for would silently change the
    // verb's contract. Instead they surface the dependency uniformly via this
    // helper: report the actor's bReplicates state (bReplicatesEnabled) and,
    // when a property was just marked replicated on a non-replicating actor,
    // warn that it will not replicate until actor replication is enabled.
    // E-set-property-replicated-no-actor-replicates-flag.
    static void AddActorReplicatesDependency(TSharedPtr<FJsonObject>& Resp, UBlueprint* Blueprint,
        const FString& PropertyName, bool bMarkedReplicated)
    {
        if (!Resp.IsValid() || !Blueprint || !Blueprint->GeneratedClass) return;

        AActor* CDO = Cast<AActor>(Blueprint->GeneratedClass->GetDefaultObject());
        if (!CDO) return;

        const bool bActorReplicates = CDO->GetIsReplicated();
        Resp->SetBoolField(TEXT("bReplicatesEnabled"), bActorReplicates);
        if (bMarkedReplicated && !bActorReplicates)
        {
            Resp->SetStringField(TEXT("warning"),
                FString::Printf(TEXT("Property '%s' is marked replicated but the actor's bReplicates is still false, ")
                    TEXT("so it will NOT replicate. Enable actor replication with ")
                    TEXT("misc.set_replication {replicates:true}."),
                    *PropertyName));
        }
    }
}

// ---------------------------------------------------------------------------
// networking.set_property_replicated
// ---------------------------------------------------------------------------
REGISTER_RPC_HANDLER("networking.set_property_replicated", "networking", "Set a blueprint property as replicated or not",
    RPC_PARAMS(
        RPC_PARAM_REQ("blueprintPath", "path", "Path to the blueprint"),
        RPC_PARAM_REQ("propertyName", "string", "Name of the property to configure"),
        RPC_PARAM_OPT("replicated", "boolean", "Whether to replicate (default true)")
    ))
{
    using namespace NetworkingHelpersNew;

    FString BlueprintPath;
    if (!Ctx.RequireString(TEXT("blueprintPath"), BlueprintPath)) return true;
    FString PropertyName;
    if (!Ctx.RequireString(TEXT("propertyName"), PropertyName)) return true;
    bool bReplicated = Ctx.GetBool(TEXT("replicated"), true);

    UBlueprint* Blueprint = LoadBlueprintFromPath(BlueprintPath);
    if (!Blueprint) { Ctx.SendError(TEXT("NOT_FOUND"), TEXT("Blueprint not found")); return true; }

    bool bFound = false;
    for (FBPVariableDescription& VarDesc : Blueprint->NewVariables)
    {
        if (VarDesc.VarName == FName(*PropertyName))
        {
            if (bReplicated) VarDesc.PropertyFlags |= CPF_Net;
            else VarDesc.PropertyFlags &= ~CPF_Net;
            bFound = true;
            break;
        }
    }
    if (!bFound) { Ctx.SendError(TEXT("NOT_FOUND"), TEXT("Property not found in blueprint")); return true; }

    Blueprint->Modify();
    FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
    const BlueprintHandlerUtils::FBlueprintCompileDiagnostics Diagnostics =
        BlueprintHandlerUtils::CompileBlueprintWithDiagnostics(Blueprint);

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetBoolField(TEXT("success"), true);
    Resp->SetStringField(TEXT("message"), FString::Printf(TEXT("Property %s replication set to %s"), *PropertyName, bReplicated ? TEXT("true") : TEXT("false")));

    AddActorReplicatesDependency(Resp, Blueprint, PropertyName, bReplicated);
    BlueprintHandlerUtils::AddCompileDiagnosticsToJson(Diagnostics, Resp);

    AddAssetVerification(Resp, Blueprint);
    Ctx.SendSuccess(Resp);
    return true;
}

// ---------------------------------------------------------------------------
// networking.set_replication_condition
// ---------------------------------------------------------------------------
REGISTER_RPC_HANDLER("networking.set_replication_condition", "networking", "Set the replication condition for a blueprint property",
    RPC_PARAMS(
        RPC_PARAM_REQ("blueprintPath", "path", "Path to the blueprint"),
        RPC_PARAM_REQ("propertyName", "string", "Property name"),
        RPC_PARAM_REQ("condition", "string", "Replication condition (e.g. COND_OwnerOnly)")
    ))
{
    using namespace NetworkingHelpersNew;

    FString BlueprintPath, PropertyName, Condition;
    if (!Ctx.RequireString(TEXT("blueprintPath"), BlueprintPath)) return true;
    if (!Ctx.RequireString(TEXT("propertyName"), PropertyName)) return true;
    if (!Ctx.RequireString(TEXT("condition"), Condition)) return true;

    UBlueprint* Blueprint = LoadBlueprintFromPath(BlueprintPath);
    if (!Blueprint) { Ctx.SendError(TEXT("NOT_FOUND"), TEXT("Blueprint not found")); return true; }

    ELifetimeCondition LifetimeCondition = GetReplicationCondition(Condition);
    bool bFound = false;
    for (FBPVariableDescription& VarDesc : Blueprint->NewVariables)
    {
        if (VarDesc.VarName == FName(*PropertyName))
        {
            VarDesc.PropertyFlags |= CPF_Net;
            VarDesc.ReplicationCondition = LifetimeCondition;
            bFound = true;
            break;
        }
    }
    if (!bFound) { Ctx.SendError(TEXT("NOT_FOUND"), FString::Printf(TEXT("Property '%s' not found"), *PropertyName)); return true; }

    Blueprint->Modify();
    FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
    const BlueprintHandlerUtils::FBlueprintCompileDiagnostics Diagnostics =
        BlueprintHandlerUtils::CompileBlueprintWithDiagnostics(Blueprint);

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetBoolField(TEXT("success"), true);
    Resp->SetStringField(TEXT("message"), FString::Printf(TEXT("Replication condition set to %s"), *Condition));
    BlueprintHandlerUtils::AddCompileDiagnosticsToJson(Diagnostics, Resp);
    AddActorReplicatesDependency(Resp, Blueprint, PropertyName, /*bMarkedReplicated=*/true);
    AddAssetVerification(Resp, Blueprint);
    Ctx.SendSuccess(Resp);
    return true;
}

// ---------------------------------------------------------------------------
// networking.configure_net_update_frequency
// ---------------------------------------------------------------------------
REGISTER_RPC_HANDLER("networking.configure_net_update_frequency", "networking", "Configure network update frequency on a blueprint",
    RPC_PARAMS(
        RPC_PARAM_REQ("blueprintPath", "path", "Path to the blueprint"),
        RPC_PARAM_OPT("netUpdateFrequency", "number", "Net update frequency (default 100)"),
        RPC_PARAM_OPT("minNetUpdateFrequency", "number", "Minimum net update frequency (default 2)")
    ))
{
    using namespace NetworkingHelpersNew;

    FString BlueprintPath;
    if (!Ctx.RequireString(TEXT("blueprintPath"), BlueprintPath)) return true;
    double NetUpdateFrequency = Ctx.GetNumber(TEXT("netUpdateFrequency"), 100.0);
    double MinNetUpdateFrequency = Ctx.GetNumber(TEXT("minNetUpdateFrequency"), 2.0);

    UBlueprint* Blueprint = LoadBlueprintFromPath(BlueprintPath);
    if (!Blueprint) { Ctx.SendError(TEXT("NOT_FOUND"), TEXT("Blueprint not found")); return true; }

    AActor* CDO = Cast<AActor>(Blueprint->GeneratedClass->GetDefaultObject());
    if (CDO)
    {
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 5, 0)
        CDO->SetNetUpdateFrequency(static_cast<float>(NetUpdateFrequency));
        CDO->SetMinNetUpdateFrequency(static_cast<float>(MinNetUpdateFrequency));
#else
        CDO->NetUpdateFrequency = static_cast<float>(NetUpdateFrequency);
        CDO->MinNetUpdateFrequency = static_cast<float>(MinNetUpdateFrequency);
#endif
    }

    Blueprint->Modify();
    FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetBoolField(TEXT("success"), true);
    Resp->SetStringField(TEXT("message"), FString::Printf(TEXT("Net update frequency set to %.1f (min: %.1f)"), NetUpdateFrequency, MinNetUpdateFrequency));
    AddAssetVerification(Resp, Blueprint);
    Ctx.SendSuccess(Resp);
    return true;
}

// ---------------------------------------------------------------------------
// networking.configure_net_priority
// ---------------------------------------------------------------------------
REGISTER_RPC_HANDLER("networking.configure_net_priority", "networking", "Configure network priority on a blueprint",
    RPC_PARAMS(
        RPC_PARAM_REQ("blueprintPath", "path", "Path to the blueprint"),
        RPC_PARAM_OPT("netPriority", "number", "Net priority (default 1.0)")
    ))
{
    using namespace NetworkingHelpersNew;

    FString BlueprintPath;
    if (!Ctx.RequireString(TEXT("blueprintPath"), BlueprintPath)) return true;
    double NetPriority = Ctx.GetNumber(TEXT("netPriority"), 1.0);

    UBlueprint* Blueprint = LoadBlueprintFromPath(BlueprintPath);
    if (!Blueprint) { Ctx.SendError(TEXT("NOT_FOUND"), TEXT("Blueprint not found")); return true; }

    AActor* CDO = Cast<AActor>(Blueprint->GeneratedClass->GetDefaultObject());
    if (CDO) CDO->NetPriority = static_cast<float>(NetPriority);

    Blueprint->Modify();
    FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetBoolField(TEXT("success"), true);
    Resp->SetStringField(TEXT("message"), FString::Printf(TEXT("Net priority set to %.2f"), NetPriority));
    AddAssetVerification(Resp, Blueprint);
    Ctx.SendSuccess(Resp);
    return true;
}

// ---------------------------------------------------------------------------
// networking.set_net_dormancy
// ---------------------------------------------------------------------------
REGISTER_RPC_HANDLER("networking.set_net_dormancy", "networking", "Set network dormancy mode on a blueprint",
    RPC_PARAMS(
        RPC_PARAM_REQ("blueprintPath", "path", "Path to the blueprint"),
        RPC_PARAM_REQ("dormancy", "string", "Dormancy mode (e.g. DORM_Awake)")
    ))
{
    using namespace NetworkingHelpersNew;

    FString BlueprintPath, Dormancy;
    if (!Ctx.RequireString(TEXT("blueprintPath"), BlueprintPath)) return true;
    if (!Ctx.RequireString(TEXT("dormancy"), Dormancy)) return true;

    UBlueprint* Blueprint = LoadBlueprintFromPath(BlueprintPath);
    if (!Blueprint) { Ctx.SendError(TEXT("NOT_FOUND"), TEXT("Blueprint not found")); return true; }

    AActor* CDO = Cast<AActor>(Blueprint->GeneratedClass->GetDefaultObject());
    if (CDO) CDO->NetDormancy = GetNetDormancy(Dormancy);

    Blueprint->Modify();
    FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetBoolField(TEXT("success"), true);
    Resp->SetStringField(TEXT("message"), FString::Printf(TEXT("Net dormancy set to %s"), *Dormancy));
    AddAssetVerification(Resp, Blueprint);
    Ctx.SendSuccess(Resp);
    return true;
}

// ---------------------------------------------------------------------------
// networking.create_rpc_function
// ---------------------------------------------------------------------------
REGISTER_RPC_HANDLER("networking.create_rpc_function", "networking", "Create a new RPC function on a blueprint, optionally with caller-specified parameter pins (e.g. a Server RPC that takes a damage amount)",
    RPC_PARAMS(
        RPC_PARAM_REQ("blueprintPath", "path", "Path to the blueprint"),
        RPC_PARAM_REQ("functionName", "string", "Name for the RPC function"),
        RPC_PARAM_REQ("rpcType", "string", "RPC type: Server, Client, or NetMulticast"),
        RPC_PARAM_OPT("reliable", "boolean", "Whether RPC is reliable (default true)"),
        RPC_PARAM_OPT("inputs", "array", "Array of {name, type} pin definitions for input parameters; type accepts the same tokens as blueprint.add_function's inputs."),
        RPC_PARAM_OPT("outputs", "array", "Reserved for signature validation; RPC return values are rejected with INVALID_RPC_CONFIGURATION.")
    ))
{
    using namespace NetworkingHelpersNew;

    FString BlueprintPath, FunctionName, RpcType;
    if (!Ctx.RequireString(TEXT("blueprintPath"), BlueprintPath)) return true;
    if (!Ctx.RequireString(TEXT("functionName"), FunctionName)) return true;
    if (!Ctx.RequireString(TEXT("rpcType"), RpcType)) return true;
    bool bReliable = Ctx.GetBool(TEXT("reliable"), true);

    int32 DirectionFlag = 0;
    FString CanonicalRpcType;
    if (!TryParseRpcType(RpcType, DirectionFlag, CanonicalRpcType))
    {
        Ctx.SendError(TEXT("INVALID_RPC_CONFIGURATION"),
            TEXT("rpcType must be Server, Client, or NetMulticast."));
        return true;
    }

    // Parameter pins are optional. Parse them with the same {name,type} parser
    // blueprint.add_function uses so the type vocabulary stays one source of truth.
    const TSharedPtr<FJsonObject>& Payload = Ctx.GetRawPayload();
    const TArray<TSharedPtr<FJsonValue>> Inputs =
        BlueprintHandlerUtils::ReadPinParamArrayField(Payload, TEXT("inputs"));
    const TArray<TSharedPtr<FJsonValue>> Outputs =
        BlueprintHandlerUtils::ReadPinParamArrayField(Payload, TEXT("outputs"));

    FString PinShapeError;
    if (!BlueprintHandlerUtils::ValidateNamedTypePinParamElements(
            Inputs, TEXT("inputs"), PinShapeError)
        || !BlueprintHandlerUtils::ValidateNamedTypePinParamElements(
            Outputs, TEXT("outputs"), PinShapeError))
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"), *PinShapeError);
        return true;
    }
    if (Outputs.Num() > 0)
    {
        Ctx.SendError(TEXT("INVALID_RPC_CONFIGURATION"),
            TEXT("RPC functions cannot declare output or return-value pins."));
        return true;
    }

    TArray<BlueprintHandlerUtils::FParsedPinParam> ParsedInputs;
    TArray<BlueprintHandlerUtils::FParsedPinParam> ParsedOutputs;
    FString ParamParseError;
    BlueprintHandlerUtils::ParseNamedTypePinParams(
        Inputs, ParsedInputs, BlueprintHandlerUtils::EParsedPinParamMode::AllowWildcardFallback,
        ParamParseError, TEXT("input param"));
    BlueprintHandlerUtils::ParseNamedTypePinParams(
        Outputs, ParsedOutputs, BlueprintHandlerUtils::EParsedPinParamMode::AllowWildcardFallback,
        ParamParseError, TEXT("output param"));

    // Reject any input/output token that would silently become a wildcard pin
    // (e.g. the documented-but-unsupported 'class:/Script/X.Y' form) instead of
    // returning success with a malformed pin that only fails at a later compile.
    // Mirrors blueprint.add_variable's loud TYPE_NOT_FOUND rejection.
    if (BlueprintHandlerUtils::RejectWildcardPinParams(Ctx, ParsedInputs, ParsedOutputs))
    {
        return true;
    }

    UBlueprint* Blueprint = LoadBlueprintFromPath(BlueprintPath);
    if (!Blueprint) { Ctx.SendError(TEXT("NOT_FOUND"), TEXT("Blueprint not found")); return true; }

    UEdGraph* NewGraph = FBlueprintEditorUtils::CreateNewGraph(
        Blueprint, FName(*FunctionName), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());

    if (NewGraph)
    {
        FBlueprintEditorUtils::AddFunctionGraph<UFunction>(Blueprint, NewGraph, false, static_cast<UFunction*>(nullptr));

        UK2Node_FunctionEntry* EntryNode = nullptr;
        UK2Node_FunctionResult* ResultNode = nullptr;
        for (UEdGraphNode* Node : NewGraph->Nodes)
        {
            if (UK2Node_FunctionEntry* Entry = Cast<UK2Node_FunctionEntry>(Node))
            {
                EntryNode = Entry;
            }
            else if (UK2Node_FunctionResult* Result = Cast<UK2Node_FunctionResult>(Node))
            {
                ResultNode = Result;
            }
            if (EntryNode && ResultNode) break;
        }

        if (!EntryNode)
        {
            FBlueprintEditorUtils::RemoveGraph(Blueprint, NewGraph);
            Ctx.SendError(TEXT("CREATE_FAILED"), TEXT("Function entry node was not created"));
            return true;
        }

        int32 NetFlags = FUNC_Net | DirectionFlag;
        if (bReliable) NetFlags |= FUNC_NetReliable;
        EntryNode->AddExtraFlags(NetFlags);

        FString PinCreationError;
        if (!BlueprintHandlerUtils::AddParsedPinParamsToNodes(
                EntryNode, ResultNode, ParsedInputs, ParsedOutputs,
                TEXT("networking.create_rpc_function"), PinCreationError))
        {
            FBlueprintEditorUtils::RemoveGraph(Blueprint, NewGraph);
            Ctx.SendError(TEXT("PIN_CREATION_FAILED"), *PinCreationError);
            return true;
        }

        Blueprint->Modify();
        FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
        const BlueprintHandlerUtils::FBlueprintCompileDiagnostics Diagnostics =
            BlueprintHandlerUtils::CompileBlueprintWithDiagnostics(Blueprint);

        TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
        Resp->SetBoolField(TEXT("success"), true);
        Resp->SetStringField(TEXT("functionName"), FunctionName);
        Resp->SetStringField(TEXT("rpcType"), CanonicalRpcType);
        Resp->SetBoolField(TEXT("reliable"), bReliable);
        if (Inputs.Num() > 0) Resp->SetArrayField(TEXT("inputs"), Inputs);
        if (Outputs.Num() > 0) Resp->SetArrayField(TEXT("outputs"), Outputs);
        Resp->SetStringField(TEXT("message"), FString::Printf(TEXT("Created %s RPC function: %s"), *CanonicalRpcType, *FunctionName));
        BlueprintHandlerUtils::AddCompileDiagnosticsToJson(Diagnostics, Resp);
        AddAssetVerification(Resp, Blueprint);
        Ctx.SendSuccess(Resp);
    }
    else
    {
        Ctx.SendError(TEXT("CREATE_FAILED"), TEXT("Failed to create function graph"));
    }
    return true;
}

// ---------------------------------------------------------------------------
// networking.configure_rpc_validation
// ---------------------------------------------------------------------------
REGISTER_RPC_HANDLER("networking.configure_rpc_validation", "networking", "Clear an unsupported validation flag from a Blueprint RPC; enabling validation is refused",
    RPC_PARAMS(
        RPC_PARAM_REQ("blueprintPath", "path", "Path to the blueprint"),
        RPC_PARAM_REQ("functionName", "string", "Name of the RPC function"),
        RPC_PARAM_OPT("withValidation", "boolean", "Must be false or omitted to clear validation; true returns UNSUPPORTED because Blueprint functions cannot supply a native _Validate thunk.")
    ))
{
    using namespace NetworkingHelpersNew;

    FString BlueprintPath, FunctionName;
    if (!Ctx.RequireString(TEXT("blueprintPath"), BlueprintPath)) return true;
    if (!Ctx.RequireString(TEXT("functionName"), FunctionName)) return true;
    bool bWithValidation = Ctx.GetBool(TEXT("withValidation"), false);

    UBlueprint* Blueprint = LoadBlueprintFromPath(BlueprintPath);
    if (!Blueprint) { Ctx.SendError(TEXT("NOT_FOUND"), TEXT("Blueprint not found")); return true; }

    UEdGraph* FuncGraph = nullptr;
    for (UEdGraph* Graph : Blueprint->FunctionGraphs)
    {
        if (Graph && Graph->GetFName() == FName(*FunctionName)) { FuncGraph = Graph; break; }
    }
    if (!FuncGraph) { Ctx.SendError(TEXT("NOT_FOUND"), FString::Printf(TEXT("Function '%s' not found"), *FunctionName)); return true; }

    UK2Node_FunctionEntry* EntryNode = nullptr;
    for (UEdGraphNode* Node : FuncGraph->Nodes)
    {
        if (UK2Node_FunctionEntry* Candidate = Cast<UK2Node_FunctionEntry>(Node))
        {
            EntryNode = Candidate;
            break;
        }
    }
    if (!EntryNode) { Ctx.SendError(TEXT("NOT_FOUND"), TEXT("Function entry node not found")); return true; }

    if (!HasValidRpcDirection(EntryNode->GetExtraFlags()))
    {
        Ctx.SendError(TEXT("INVALID_RPC_CONFIGURATION"),
            TEXT("RPC validation requires exactly one Server, Client, or NetMulticast direction."));
        return true;
    }
    if (bWithValidation)
    {
        Ctx.SendError(TEXT("UNSUPPORTED"),
            TEXT("Blueprint-authored RPC validation is unsupported because FUNC_NetValidate requires a native _Validate implementation."));
        return true;
    }

    EntryNode->ClearExtraFlags(FUNC_NetValidate);

    Blueprint->Modify();
    FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
    const BlueprintHandlerUtils::FBlueprintCompileDiagnostics Diagnostics =
        BlueprintHandlerUtils::CompileBlueprintWithDiagnostics(Blueprint);

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetBoolField(TEXT("success"), true);
    Resp->SetBoolField(TEXT("withValidation"), bWithValidation);
    BlueprintHandlerUtils::AddCompileDiagnosticsToJson(Diagnostics, Resp);
    AddAssetVerification(Resp, Blueprint);
    Ctx.SendSuccess(Resp);
    return true;
}

// ---------------------------------------------------------------------------
// networking.set_rpc_reliability
// ---------------------------------------------------------------------------
REGISTER_RPC_HANDLER("networking.set_rpc_reliability", "networking", "Set the reliability of an RPC function",
    RPC_PARAMS(
        RPC_PARAM_REQ("blueprintPath", "path", "Path to the blueprint"),
        RPC_PARAM_REQ("functionName", "string", "Name of the RPC function"),
        RPC_PARAM_OPT("reliable", "boolean", "Reliable (default true)")
    ))
{
    using namespace NetworkingHelpersNew;

    FString BlueprintPath, FunctionName;
    if (!Ctx.RequireString(TEXT("blueprintPath"), BlueprintPath)) return true;
    if (!Ctx.RequireString(TEXT("functionName"), FunctionName)) return true;
    bool bReliable = Ctx.GetBool(TEXT("reliable"), true);

    UBlueprint* Blueprint = LoadBlueprintFromPath(BlueprintPath);
    if (!Blueprint) { Ctx.SendError(TEXT("NOT_FOUND"), TEXT("Blueprint not found")); return true; }

    UEdGraph* FuncGraph = nullptr;
    for (UEdGraph* Graph : Blueprint->FunctionGraphs)
    {
        if (Graph && Graph->GetFName() == FName(*FunctionName)) { FuncGraph = Graph; break; }
    }
    if (!FuncGraph) { Ctx.SendError(TEXT("NOT_FOUND"), FString::Printf(TEXT("Function '%s' not found"), *FunctionName)); return true; }

    UK2Node_FunctionEntry* EntryNode = nullptr;
    for (UEdGraphNode* Node : FuncGraph->Nodes)
    {
        if (UK2Node_FunctionEntry* Candidate = Cast<UK2Node_FunctionEntry>(Node))
        {
            EntryNode = Candidate;
            break;
        }
    }
    if (!EntryNode) { Ctx.SendError(TEXT("NOT_FOUND"), TEXT("Function entry node not found")); return true; }

    if (!HasValidRpcDirection(EntryNode->GetExtraFlags()))
    {
        Ctx.SendError(TEXT("INVALID_RPC_CONFIGURATION"),
            TEXT("RPC reliability requires exactly one Server, Client, or NetMulticast direction."));
        return true;
    }
    if ((EntryNode->GetExtraFlags() & FUNC_NetValidate) != 0)
    {
        Ctx.SendError(TEXT("INVALID_RPC_CONFIGURATION"),
            TEXT("RPC reliability cannot be changed while the Blueprint function carries unsupported FUNC_NetValidate."));
        return true;
    }

    if (bReliable) EntryNode->AddExtraFlags(FUNC_NetReliable);
    else EntryNode->ClearExtraFlags(FUNC_NetReliable);

    Blueprint->Modify();
    FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
    const BlueprintHandlerUtils::FBlueprintCompileDiagnostics Diagnostics =
        BlueprintHandlerUtils::CompileBlueprintWithDiagnostics(Blueprint);

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetBoolField(TEXT("success"), true);
    Resp->SetBoolField(TEXT("reliable"), bReliable);
    BlueprintHandlerUtils::AddCompileDiagnosticsToJson(Diagnostics, Resp);
    AddAssetVerification(Resp, Blueprint);
    Ctx.SendSuccess(Resp);
    return true;
}

// ---------------------------------------------------------------------------
// networking.set_owner
// ---------------------------------------------------------------------------
REGISTER_RPC_HANDLER("networking.set_owner", "networking", "Set or clear the owner of an actor in the world",
    RPC_PARAMS(
        RPC_PARAM_REQ("actorName", "string", "Name of the actor"),
        RPC_PARAM_OPT("ownerActorName", "string", "Name of the owner actor (empty to clear)")
    ))
{
    using namespace NetworkingHelpersNew;

    FString ActorName;
    if (!Ctx.RequireString(TEXT("actorName"), ActorName)) return true;
    FString OwnerActorName = Ctx.GetString(TEXT("ownerActorName"));

    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World) { Ctx.SendError(TEXT("NO_WORLD"), TEXT("No world available")); return true; }

    AActor* Actor = FindActorByName(World, ActorName);
    if (!Actor) { Ctx.SendError(TEXT("NOT_FOUND"), TEXT("Actor not found")); return true; }

    AActor* Owner = nullptr;
    if (!OwnerActorName.IsEmpty()) Owner = FindActorByName(World, OwnerActorName);

    Actor->SetOwner(Owner);

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetBoolField(TEXT("success"), true);
    Resp->SetStringField(TEXT("message"), Owner ? FString::Printf(TEXT("Set owner of %s to %s"), *ActorName, *OwnerActorName) : FString::Printf(TEXT("Cleared owner of %s"), *ActorName));
    AddActorVerification(Resp, Actor);
    Ctx.SendSuccess(Resp);
    return true;
}

// ---------------------------------------------------------------------------
// networking.set_autonomous_proxy
// ---------------------------------------------------------------------------
REGISTER_RPC_HANDLER("networking.set_autonomous_proxy", "networking", "Configure autonomous proxy replication on replicated properties",
    RPC_PARAMS(
        RPC_PARAM_REQ("blueprintPath", "path", "Path to the blueprint"),
        RPC_PARAM_OPT("isAutonomousProxy", "boolean", "Enable autonomous proxy condition (default true)")
    ))
{
    using namespace NetworkingHelpersNew;

    FString BlueprintPath;
    if (!Ctx.RequireString(TEXT("blueprintPath"), BlueprintPath)) return true;
    bool bIsAutonomousProxy = Ctx.GetBool(TEXT("isAutonomousProxy"), true);

    UBlueprint* Blueprint = LoadBlueprintFromPath(BlueprintPath);
    if (!Blueprint) { Ctx.SendError(TEXT("NOT_FOUND"), TEXT("Blueprint not found")); return true; }

    bool bAnyModified = false;
    for (FBPVariableDescription& VarDesc : Blueprint->NewVariables)
    {
        if ((VarDesc.PropertyFlags & CPF_Net) != 0)
        {
            VarDesc.ReplicationCondition = bIsAutonomousProxy ? COND_AutonomousOnly : COND_None;
            bAnyModified = true;
        }
    }

    BlueprintHandlerUtils::FBlueprintCompileDiagnostics Diagnostics;
    if (bAnyModified)
    {
        Blueprint->Modify();
        FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
        Diagnostics = BlueprintHandlerUtils::CompileBlueprintWithDiagnostics(Blueprint);
    }

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetBoolField(TEXT("success"), true);
    Resp->SetBoolField(TEXT("isAutonomousProxy"), bIsAutonomousProxy);
    if (bAnyModified)
    {
        BlueprintHandlerUtils::AddCompileDiagnosticsToJson(Diagnostics, Resp);
    }
    AddAssetVerification(Resp, Blueprint);
    Ctx.SendSuccess(Resp);
    return true;
}

// ---------------------------------------------------------------------------
// networking.check_has_authority
// ---------------------------------------------------------------------------
REGISTER_RPC_HANDLER("networking.check_has_authority", "networking", "Check if an actor has authority",
    RPC_PARAMS(
        RPC_PARAM_REQ("actorName", "string", "Name of the actor to check")
    ))
{
    using namespace NetworkingHelpersNew;

    FString ActorName;
    if (!Ctx.RequireString(TEXT("actorName"), ActorName)) return true;

    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World) { Ctx.SendError(TEXT("NO_WORLD"), TEXT("No world available")); return true; }

    AActor* Actor = FindActorByName(World, ActorName);
    if (!Actor) { Ctx.SendError(TEXT("NOT_FOUND"), TEXT("Actor not found")); return true; }

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetBoolField(TEXT("success"), true);
    Resp->SetBoolField(TEXT("hasAuthority"), Actor->HasAuthority());
    Resp->SetStringField(TEXT("role"), NetRoleToString(Actor->GetLocalRole()));
    Ctx.SendSuccess(Resp);
    return true;
}

// ---------------------------------------------------------------------------
// networking.check_is_locally_controlled
// ---------------------------------------------------------------------------
REGISTER_RPC_HANDLER("networking.check_is_locally_controlled", "networking", "Check if an actor/pawn is locally controlled",
    RPC_PARAMS(
        RPC_PARAM_REQ("actorName", "string", "Name of the actor to check")
    ))
{
    using namespace NetworkingHelpersNew;

    FString ActorName;
    if (!Ctx.RequireString(TEXT("actorName"), ActorName)) return true;

    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World) { Ctx.SendError(TEXT("NO_WORLD"), TEXT("No world available")); return true; }

    AActor* Actor = FindActorByName(World, ActorName);
    if (!Actor) { Ctx.SendError(TEXT("NOT_FOUND"), TEXT("Actor not found")); return true; }

    bool bIsLocallyControlled = false;
    bool bIsLocalController = false;
    APawn* Pawn = Cast<APawn>(Actor);
    if (Pawn)
    {
        bIsLocallyControlled = Pawn->IsLocallyControlled();
        APlayerController* PC = Cast<APlayerController>(Pawn->GetController());
        bIsLocalController = PC ? PC->IsLocalController() : false;
    }

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetBoolField(TEXT("success"), true);
    Resp->SetBoolField(TEXT("isLocallyControlled"), bIsLocallyControlled);
    Resp->SetBoolField(TEXT("isLocalController"), bIsLocalController);
    Ctx.SendSuccess(Resp);
    return true;
}

// ---------------------------------------------------------------------------
// networking.configure_net_cull_distance
// ---------------------------------------------------------------------------
REGISTER_RPC_HANDLER("networking.configure_net_cull_distance", "networking", "Configure network cull distance on a blueprint",
    RPC_PARAMS(
        RPC_PARAM_REQ("blueprintPath", "path", "Path to the blueprint"),
        RPC_PARAM_OPT("netCullDistanceSquared", "number", "Squared cull distance (default 225000000)"),
        RPC_PARAM_OPT("useOwnerNetRelevancy", "boolean", "Use owner net relevancy")
    ))
{
    using namespace NetworkingHelpersNew;

    FString BlueprintPath;
    if (!Ctx.RequireString(TEXT("blueprintPath"), BlueprintPath)) return true;
    double NetCullDistanceSquared = Ctx.GetNumber(TEXT("netCullDistanceSquared"), 225000000.0);
    bool bUseOwnerNetRelevancy = Ctx.GetBool(TEXT("useOwnerNetRelevancy"), false);

    UBlueprint* Blueprint = LoadBlueprintFromPath(BlueprintPath);
    if (!Blueprint) { Ctx.SendError(TEXT("NOT_FOUND"), TEXT("Blueprint not found")); return true; }

    AActor* CDO = Cast<AActor>(Blueprint->GeneratedClass->GetDefaultObject());
    if (CDO)
    {
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 5, 0)
        CDO->SetNetCullDistanceSquared(static_cast<float>(NetCullDistanceSquared));
#else
        CDO->NetCullDistanceSquared = static_cast<float>(NetCullDistanceSquared);
#endif
        CDO->bNetUseOwnerRelevancy = bUseOwnerNetRelevancy;
    }

    Blueprint->Modify();
    FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetBoolField(TEXT("success"), true);
    Resp->SetStringField(TEXT("message"), FString::Printf(TEXT("Net cull distance squared set to %.0f"), NetCullDistanceSquared));
    AddAssetVerification(Resp, Blueprint);
    Ctx.SendSuccess(Resp);
    return true;
}

// ---------------------------------------------------------------------------
// networking.set_always_relevant
// ---------------------------------------------------------------------------
REGISTER_RPC_HANDLER("networking.set_always_relevant", "networking", "Set an actor blueprint as always relevant",
    RPC_PARAMS(
        RPC_PARAM_REQ("blueprintPath", "path", "Path to the blueprint"),
        RPC_PARAM_OPT("alwaysRelevant", "boolean", "Always relevant (default true)")
    ))
{
    using namespace NetworkingHelpersNew;

    FString BlueprintPath;
    if (!Ctx.RequireString(TEXT("blueprintPath"), BlueprintPath)) return true;
    bool bAlwaysRelevant = Ctx.GetBool(TEXT("alwaysRelevant"), true);

    UBlueprint* Blueprint = LoadBlueprintFromPath(BlueprintPath);
    if (!Blueprint) { Ctx.SendError(TEXT("NOT_FOUND"), TEXT("Blueprint not found")); return true; }

    AActor* CDO = Cast<AActor>(Blueprint->GeneratedClass->GetDefaultObject());
    if (CDO) CDO->bAlwaysRelevant = bAlwaysRelevant;

    Blueprint->Modify();
    FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetBoolField(TEXT("success"), true);
    Resp->SetStringField(TEXT("message"), FString::Printf(TEXT("Always relevant set to %s"), bAlwaysRelevant ? TEXT("true") : TEXT("false")));
    AddAssetVerification(Resp, Blueprint);
    Ctx.SendSuccess(Resp);
    return true;
}

// ---------------------------------------------------------------------------
// networking.set_only_relevant_to_owner
// ---------------------------------------------------------------------------
REGISTER_RPC_HANDLER("networking.set_only_relevant_to_owner", "networking", "Set an actor blueprint as only relevant to its owner",
    RPC_PARAMS(
        RPC_PARAM_REQ("blueprintPath", "path", "Path to the blueprint"),
        RPC_PARAM_OPT("onlyRelevantToOwner", "boolean", "Only relevant to owner (default true)")
    ))
{
    using namespace NetworkingHelpersNew;

    FString BlueprintPath;
    if (!Ctx.RequireString(TEXT("blueprintPath"), BlueprintPath)) return true;
    bool bOnlyRelevantToOwner = Ctx.GetBool(TEXT("onlyRelevantToOwner"), true);

    UBlueprint* Blueprint = LoadBlueprintFromPath(BlueprintPath);
    if (!Blueprint) { Ctx.SendError(TEXT("NOT_FOUND"), TEXT("Blueprint not found")); return true; }

    AActor* CDO = Cast<AActor>(Blueprint->GeneratedClass->GetDefaultObject());
    if (CDO) CDO->bOnlyRelevantToOwner = bOnlyRelevantToOwner;

    Blueprint->Modify();
    FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetBoolField(TEXT("success"), true);
    Resp->SetStringField(TEXT("message"), FString::Printf(TEXT("Only relevant to owner set to %s"), bOnlyRelevantToOwner ? TEXT("true") : TEXT("false")));
    AddAssetVerification(Resp, Blueprint);
    Ctx.SendSuccess(Resp);
    return true;
}

// ---------------------------------------------------------------------------
// networking.set_replicated_using
// ---------------------------------------------------------------------------
REGISTER_RPC_HANDLER("networking.set_replicated_using", "networking", "Set RepNotify function for a replicated property",
    RPC_PARAMS(
        RPC_PARAM_REQ("blueprintPath", "path", "Path to the blueprint"),
        RPC_PARAM_REQ("propertyName", "string", "Name of the replicated property"),
        RPC_PARAM_REQ("repNotifyFunc", "string", "Name of the RepNotify function")
    ))
{
    using namespace NetworkingHelpersNew;

    FString BlueprintPath, PropertyName, RepNotifyFunc;
    if (!Ctx.RequireString(TEXT("blueprintPath"), BlueprintPath)) return true;
    if (!Ctx.RequireString(TEXT("propertyName"), PropertyName)) return true;
    if (!Ctx.RequireString(TEXT("repNotifyFunc"), RepNotifyFunc)) return true;

    UBlueprint* Blueprint = LoadBlueprintFromPath(BlueprintPath);
    if (!Blueprint) { Ctx.SendError(TEXT("NOT_FOUND"), TEXT("Blueprint not found")); return true; }

    bool bFound = false;
    for (FBPVariableDescription& VarDesc : Blueprint->NewVariables)
    {
        if (VarDesc.VarName == FName(*PropertyName))
        {
            VarDesc.PropertyFlags |= CPF_Net | CPF_RepNotify;
            VarDesc.RepNotifyFunc = FName(*RepNotifyFunc);
            bFound = true;
            break;
        }
    }
    if (!bFound) { Ctx.SendError(TEXT("NOT_FOUND"), FString::Printf(TEXT("Property '%s' not found"), *PropertyName)); return true; }

    Blueprint->Modify();
    FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
    const BlueprintHandlerUtils::FBlueprintCompileDiagnostics Diagnostics =
        BlueprintHandlerUtils::CompileBlueprintWithDiagnostics(Blueprint);

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetBoolField(TEXT("success"), true);
    Resp->SetStringField(TEXT("message"), FString::Printf(TEXT("ReplicatedUsing set to %s for property %s"), *RepNotifyFunc, *PropertyName));
    AddActorReplicatesDependency(Resp, Blueprint, PropertyName, /*bMarkedReplicated=*/true);
    BlueprintHandlerUtils::AddCompileDiagnosticsToJson(Diagnostics, Resp);
    AddAssetVerification(Resp, Blueprint);
    Ctx.SendSuccess(Resp);
    return true;
}

// ---------------------------------------------------------------------------
// networking.configure_client_prediction
// ---------------------------------------------------------------------------
REGISTER_RPC_HANDLER("networking.configure_client_prediction", "networking", "Configure client-side prediction on a character blueprint",
    RPC_PARAMS(
        RPC_PARAM_REQ("blueprintPath", "path", "Path to the character blueprint"),
        RPC_PARAM_OPT("enablePrediction", "boolean", "Enable prediction (default true)"),
        RPC_PARAM_OPT("predictionThreshold", "number", "Prediction threshold (default 0.1)")
    ))
{
    using namespace NetworkingHelpersNew;

    FString BlueprintPath;
    if (!Ctx.RequireString(TEXT("blueprintPath"), BlueprintPath)) return true;
    bool bEnablePrediction = Ctx.GetBool(TEXT("enablePrediction"), true);
    double PredictionThreshold = Ctx.GetNumber(TEXT("predictionThreshold"), 0.1);

    UBlueprint* Blueprint = LoadBlueprintFromPath(BlueprintPath);
    if (!Blueprint) { Ctx.SendError(TEXT("NOT_FOUND"), TEXT("Blueprint not found")); return true; }

    ACharacter* CharacterCDO = Cast<ACharacter>(Blueprint->GeneratedClass->GetDefaultObject());
    if (CharacterCDO && CharacterCDO->GetCharacterMovement())
    {
        UCharacterMovementComponent* CMC = CharacterCDO->GetCharacterMovement();
        if (bEnablePrediction)
        {
            CMC->bNetworkAlwaysReplicateTransformUpdateTimestamp = true;
            CMC->NetworkSimulatedSmoothLocationTime = static_cast<float>(PredictionThreshold);
        }
        else
        {
            CMC->bNetworkAlwaysReplicateTransformUpdateTimestamp = false;
        }
    }

    Blueprint->Modify();
    FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetBoolField(TEXT("success"), true);
    Resp->SetBoolField(TEXT("enablePrediction"), bEnablePrediction);
    Resp->SetNumberField(TEXT("predictionThreshold"), PredictionThreshold);
    AddAssetVerification(Resp, Blueprint);
    Ctx.SendSuccess(Resp);
    return true;
}

// ---------------------------------------------------------------------------
// networking.configure_server_correction
// ---------------------------------------------------------------------------
REGISTER_RPC_HANDLER("networking.configure_server_correction", "networking", "Configure server correction smoothing on a character blueprint",
    RPC_PARAMS(
        RPC_PARAM_REQ("blueprintPath", "path", "Path to the character blueprint"),
        RPC_PARAM_OPT("correctionThreshold", "number", "Correction threshold (default 1.0)"),
        RPC_PARAM_OPT("smoothingRate", "number", "Smoothing rate (default 0.5)")
    ))
{
    using namespace NetworkingHelpersNew;

    FString BlueprintPath;
    if (!Ctx.RequireString(TEXT("blueprintPath"), BlueprintPath)) return true;
    double CorrectionThreshold = Ctx.GetNumber(TEXT("correctionThreshold"), 1.0);
    double SmoothingRate = Ctx.GetNumber(TEXT("smoothingRate"), 0.5);

    UBlueprint* Blueprint = LoadBlueprintFromPath(BlueprintPath);
    if (!Blueprint) { Ctx.SendError(TEXT("NOT_FOUND"), TEXT("Blueprint not found")); return true; }

    ACharacter* CharacterCDO = Cast<ACharacter>(Blueprint->GeneratedClass->GetDefaultObject());
    if (CharacterCDO && CharacterCDO->GetCharacterMovement())
    {
        UCharacterMovementComponent* CMC = CharacterCDO->GetCharacterMovement();
        CMC->NetworkSimulatedSmoothLocationTime = static_cast<float>(SmoothingRate);
        CMC->NetworkSimulatedSmoothRotationTime = static_cast<float>(SmoothingRate);
        CMC->ListenServerNetworkSimulatedSmoothLocationTime = static_cast<float>(SmoothingRate);
        CMC->ListenServerNetworkSimulatedSmoothRotationTime = static_cast<float>(SmoothingRate);
    }

    Blueprint->Modify();
    FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetBoolField(TEXT("success"), true);
    Resp->SetNumberField(TEXT("correctionThreshold"), CorrectionThreshold);
    Resp->SetNumberField(TEXT("smoothingRate"), SmoothingRate);
    AddAssetVerification(Resp, Blueprint);
    Ctx.SendSuccess(Resp);
    return true;
}

// ---------------------------------------------------------------------------
// networking.add_network_prediction_data
// ---------------------------------------------------------------------------
REGISTER_RPC_HANDLER("networking.add_network_prediction_data", "networking", "Add a replicated variable for network prediction data",
    RPC_PARAMS(
        RPC_PARAM_REQ("blueprintPath", "path", "Path to the blueprint"),
        RPC_PARAM_REQ("dataType", "string", "Data type: Transform, Vector, Rotator, or Float"),
        RPC_PARAM_OPT("variableName", "string", "Optional variable name")
    ))
{
    using namespace NetworkingHelpersNew;

    FString BlueprintPath, DataType;
    if (!Ctx.RequireString(TEXT("blueprintPath"), BlueprintPath)) return true;
    if (!Ctx.RequireString(TEXT("dataType"), DataType)) return true;
    FString VariableName = Ctx.GetString(TEXT("variableName"));

    UBlueprint* Blueprint = LoadBlueprintFromPath(BlueprintPath);
    if (!Blueprint) { Ctx.SendError(TEXT("NOT_FOUND"), TEXT("Blueprint not found")); return true; }

    FString VarName = VariableName.IsEmpty() ? FString::Printf(TEXT("PredictionData_%s"), *DataType) : VariableName;

    FEdGraphPinType PinType;
    PinType.PinCategory = UEdGraphSchema_K2::PC_Struct;

    if (DataType == TEXT("Transform")) PinType.PinSubCategoryObject = TBaseStructure<FTransform>::Get();
    else if (DataType == TEXT("Vector")) PinType.PinSubCategoryObject = TBaseStructure<FVector>::Get();
    else if (DataType == TEXT("Rotator")) PinType.PinSubCategoryObject = TBaseStructure<FRotator>::Get();
    else { PinType.PinCategory = UEdGraphSchema_K2::PC_Real; PinType.PinSubCategory = UEdGraphSchema_K2::PC_Float; }

    bool bSuccess = FBlueprintEditorUtils::AddMemberVariable(Blueprint, FName(*VarName), PinType);

    if (bSuccess)
    {
        for (FBPVariableDescription& VarDesc : Blueprint->NewVariables)
        {
            if (VarDesc.VarName == FName(*VarName))
            {
                VarDesc.PropertyFlags |= CPF_Net;
                VarDesc.ReplicationCondition = COND_AutonomousOnly;
                break;
            }
        }
    }

    Blueprint->Modify();
    FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
    const BlueprintHandlerUtils::FBlueprintCompileDiagnostics Diagnostics =
        BlueprintHandlerUtils::CompileBlueprintWithDiagnostics(Blueprint);

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetBoolField(TEXT("success"), bSuccess);
    Resp->SetStringField(TEXT("variableName"), VarName);
    Resp->SetStringField(TEXT("dataType"), DataType);
    AddActorReplicatesDependency(Resp, Blueprint, VarName, /*bMarkedReplicated=*/bSuccess);
    BlueprintHandlerUtils::AddCompileDiagnosticsToJson(Diagnostics, Resp);
    AddAssetVerification(Resp, Blueprint);
    Ctx.SendSuccess(Resp);
    return true;
}

// ---------------------------------------------------------------------------
// networking.configure_movement_prediction
// ---------------------------------------------------------------------------
REGISTER_RPC_HANDLER("networking.configure_movement_prediction", "networking", "Configure movement prediction smoothing on a character blueprint",
    RPC_PARAMS(
        RPC_PARAM_REQ("blueprintPath", "path", "Path to the character blueprint"),
        RPC_PARAM_OPT("networkSmoothingMode", "string", "Disabled, Linear, or Exponential (case-insensitive; default Exponential)"),
        RPC_PARAM_OPT("networkMaxSmoothUpdateDistance", "number", "Max smooth update distance (default 256)"),
        RPC_PARAM_OPT("networkNoSmoothUpdateDistance", "number", "No smooth update distance (default 384)")
    ))
{
    using namespace NetworkingHelpersNew;

    FString BlueprintPath;
    if (!Ctx.RequireString(TEXT("blueprintPath"), BlueprintPath)) return true;
    const FString NetworkSmoothingModeName =
        Ctx.GetString(TEXT("networkSmoothingMode"), TEXT("Exponential"));
    ENetworkSmoothingMode NetworkSmoothingMode = ENetworkSmoothingMode::Exponential;
    if (!PinWrightMovementPrediction::TryParseNetworkSmoothingMode(
            NetworkSmoothingModeName, NetworkSmoothingMode))
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"),
            TEXT("networkSmoothingMode must be one of: Disabled, Linear, Exponential"));
        return true;
    }
    double NetworkMaxSmoothUpdateDistance = Ctx.GetNumber(TEXT("networkMaxSmoothUpdateDistance"), 256.0);
    double NetworkNoSmoothUpdateDistance = Ctx.GetNumber(TEXT("networkNoSmoothUpdateDistance"), 384.0);

    UBlueprint* Blueprint = LoadBlueprintFromPath(BlueprintPath);
    if (!Blueprint) { Ctx.SendError(TEXT("NOT_FOUND"), TEXT("Blueprint not found")); return true; }

    ACharacter* CharacterCDO = Cast<ACharacter>(Blueprint->GeneratedClass->GetDefaultObject());
    if (CharacterCDO && CharacterCDO->GetCharacterMovement())
    {
        UCharacterMovementComponent* CMC = CharacterCDO->GetCharacterMovement();
        PinWrightMovementPrediction::ApplyMovementPredictionSettings(*CMC, NetworkSmoothingMode,
            static_cast<float>(NetworkMaxSmoothUpdateDistance),
            static_cast<float>(NetworkNoSmoothUpdateDistance));
    }

    Blueprint->Modify();
    FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetBoolField(TEXT("success"), true);
    Resp->SetStringField(TEXT("message"), TEXT("Movement prediction configured"));
    AddAssetVerification(Resp, Blueprint);
    Ctx.SendSuccess(Resp);
    return true;
}

// ---------------------------------------------------------------------------
// networking.configure_replicated_movement
// ---------------------------------------------------------------------------
REGISTER_RPC_HANDLER("networking.configure_replicated_movement", "networking", "Enable or disable movement replication on a blueprint",
    RPC_PARAMS(
        RPC_PARAM_REQ("blueprintPath", "path", "Path to the blueprint"),
        RPC_PARAM_OPT("replicateMovement", "boolean", "Replicate movement (default true)")
    ))
{
    using namespace NetworkingHelpersNew;

    FString BlueprintPath;
    if (!Ctx.RequireString(TEXT("blueprintPath"), BlueprintPath)) return true;
    bool bReplicateMovement = Ctx.GetBool(TEXT("replicateMovement"), true);

    UBlueprint* Blueprint = LoadBlueprintFromPath(BlueprintPath);
    if (!Blueprint) { Ctx.SendError(TEXT("NOT_FOUND"), TEXT("Blueprint not found")); return true; }

    AActor* CDO = Cast<AActor>(Blueprint->GeneratedClass->GetDefaultObject());
    if (CDO) CDO->SetReplicatingMovement(bReplicateMovement);

    Blueprint->Modify();
    FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetBoolField(TEXT("success"), true);
    Resp->SetStringField(TEXT("message"), FString::Printf(TEXT("Replicate movement set to %s"), bReplicateMovement ? TEXT("true") : TEXT("false")));
    AddAssetVerification(Resp, Blueprint);
    Ctx.SendSuccess(Resp);
    return true;
}

// ---------------------------------------------------------------------------
// networking.get_networking_info
// ---------------------------------------------------------------------------
REGISTER_RPC_HANDLER("networking.get_networking_info", "networking", "Get networking information for a blueprint or actor",
    RPC_PARAMS(
        RPC_PARAM_OPT("blueprintPath", "path", "Path to a blueprint"),
        RPC_PARAM_OPT("actorName", "string", "Name of an actor in the world")
    ))
{
    using namespace NetworkingHelpersNew;

    FString BlueprintPath = Ctx.GetString(TEXT("blueprintPath"));
    FString ActorName = Ctx.GetString(TEXT("actorName"));

    TSharedPtr<FJsonObject> NetworkingInfo = MakeShared<FJsonObject>();

    if (!BlueprintPath.IsEmpty())
    {
        UBlueprint* Blueprint = LoadBlueprintFromPath(BlueprintPath);
        if (!Blueprint) { Ctx.SendError(TEXT("NOT_FOUND"), TEXT("Blueprint not found")); return true; }

        AActor* CDO = Cast<AActor>(Blueprint->GeneratedClass->GetDefaultObject());
        if (CDO)
        {
            NetworkingInfo->SetBoolField(TEXT("bReplicates"), CDO->GetIsReplicated());
            NetworkingInfo->SetBoolField(TEXT("bAlwaysRelevant"), CDO->bAlwaysRelevant);
            NetworkingInfo->SetBoolField(TEXT("bOnlyRelevantToOwner"), CDO->bOnlyRelevantToOwner);
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 5, 0)
            NetworkingInfo->SetNumberField(TEXT("netUpdateFrequency"), CDO->GetNetUpdateFrequency());
            NetworkingInfo->SetNumberField(TEXT("minNetUpdateFrequency"), CDO->GetMinNetUpdateFrequency());
            NetworkingInfo->SetNumberField(TEXT("netCullDistanceSquared"), CDO->GetNetCullDistanceSquared());
#else
            NetworkingInfo->SetNumberField(TEXT("netUpdateFrequency"), CDO->NetUpdateFrequency);
            NetworkingInfo->SetNumberField(TEXT("minNetUpdateFrequency"), CDO->MinNetUpdateFrequency);
            NetworkingInfo->SetNumberField(TEXT("netCullDistanceSquared"), CDO->NetCullDistanceSquared);
#endif
            NetworkingInfo->SetNumberField(TEXT("netPriority"), CDO->NetPriority);
            NetworkingInfo->SetStringField(TEXT("netDormancy"), NetDormancyToString(CDO->NetDormancy));
        }

        // Per-RPC and per-property replication detail — the round-trip readback for
        // the networking setters. withValidation (FUNC_NetValidate), the RepNotify
        // function NAME, and the replication CONDITION are surfaced by no other
        // reader (system.inspect.inspect_class decodes rpcType/reliable/replicated
        // and RepNotify-presence but not these three).
        NetworkingInfo->SetArrayField(TEXT("rpcFunctions"), BuildRpcFunctionsArray(Blueprint));
        NetworkingInfo->SetArrayField(TEXT("replicatedProperties"), BuildReplicatedPropertiesArray(Blueprint));
    }
    else if (!ActorName.IsEmpty())
    {
        UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
        if (!World) { Ctx.SendError(TEXT("NO_WORLD"), TEXT("No world available")); return true; }

        AActor* Actor = FindActorByName(World, ActorName);
        if (!Actor) { Ctx.SendError(TEXT("NOT_FOUND"), TEXT("Actor not found")); return true; }

        NetworkingInfo->SetBoolField(TEXT("bReplicates"), Actor->GetIsReplicated());
        NetworkingInfo->SetBoolField(TEXT("bAlwaysRelevant"), Actor->bAlwaysRelevant);
        NetworkingInfo->SetBoolField(TEXT("bOnlyRelevantToOwner"), Actor->bOnlyRelevantToOwner);
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 5, 0)
        NetworkingInfo->SetNumberField(TEXT("netUpdateFrequency"), Actor->GetNetUpdateFrequency());
        NetworkingInfo->SetNumberField(TEXT("minNetUpdateFrequency"), Actor->GetMinNetUpdateFrequency());
        NetworkingInfo->SetNumberField(TEXT("netCullDistanceSquared"), Actor->GetNetCullDistanceSquared());
#else
        NetworkingInfo->SetNumberField(TEXT("netUpdateFrequency"), Actor->NetUpdateFrequency);
        NetworkingInfo->SetNumberField(TEXT("minNetUpdateFrequency"), Actor->MinNetUpdateFrequency);
        NetworkingInfo->SetNumberField(TEXT("netCullDistanceSquared"), Actor->NetCullDistanceSquared);
#endif
        NetworkingInfo->SetNumberField(TEXT("netPriority"), Actor->NetPriority);
        NetworkingInfo->SetStringField(TEXT("netDormancy"), NetDormancyToString(Actor->NetDormancy));
        NetworkingInfo->SetStringField(TEXT("role"), NetRoleToString(Actor->GetLocalRole()));
        NetworkingInfo->SetStringField(TEXT("remoteRole"), NetRoleToString(Actor->GetRemoteRole()));
        NetworkingInfo->SetBoolField(TEXT("hasAuthority"), Actor->HasAuthority());

        // Owner round-trip for the actor-instance setters (set_owner / actor.attach /
        // actor.detach all write AActor::Owner via SetOwner). set_owner reports the
        // new owner only in its transient message string, so this is the only
        // structured readback of the owner change. null when there is no owner.
        AActor* Owner = Actor->GetOwner();
        if (Owner)
        {
            NetworkingInfo->SetStringField(TEXT("owner"), Owner->GetPathName());
            NetworkingInfo->SetStringField(TEXT("ownerName"), Owner->GetName());
        }
        else
        {
            NetworkingInfo->SetField(TEXT("owner"), MakeShared<FJsonValueNull>());
            NetworkingInfo->SetField(TEXT("ownerName"), MakeShared<FJsonValueNull>());
        }
    }
    else
    {
        Ctx.SendError(TEXT("INVALID_PARAMS"), TEXT("Must provide either blueprintPath or actorName"));
        return true;
    }

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetBoolField(TEXT("success"), true);
    Resp->SetObjectField(TEXT("networkingInfo"), NetworkingInfo);
    Ctx.SendSuccess(Resp);
    return true;
}
