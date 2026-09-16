// Copyright (c) 2026 Alexander Penkin. MIT License.

// NavigationHandler.cpp - Migrated from PinWright_NavigationHandlers.cpp
// Navigation system handlers: NavMesh configuration, nav modifiers, nav links, smart links

#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/Level/LevelBuildBinds.h"
#include "PinWrightHelpers.h"
#include "Utils/JsonBuilders.h"
#include "PinWrightSubsystem.h"
#include "Misc/EngineVersionComparison.h"

#include "Editor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Engine/Blueprint.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/SCS_Node.h"
#include "Kismet2/BlueprintEditorUtils.h"

// Navigation System includes
#include "NavigationSystem.h"
#include "NavMesh/RecastNavMesh.h"
#include "NavMesh/NavMeshBoundsVolume.h"
#include "NavModifierComponent.h"
#include "NavLinkCustomComponent.h"
#include "Navigation/NavLinkProxy.h"
#include "AI/NavigationSystemBase.h"
#include "NavAreas/NavArea.h"
#include "NavAreas/NavArea_Default.h"
#include "NavAreas/NavArea_Null.h"
#include "NavAreas/NavArea_Obstacle.h"

DEFINE_LOG_CATEGORY_STATIC(LogMcpNavigationHandlers, Log, All);

// Shared NO_NAVMESH remedy: both nav-config handlers (configure_nav_mesh_settings,
// set_nav_agent_properties) hit the same missing-RecastNavMesh condition and must
// give identical guidance, so the message lives here as one source of truth.
static const TCHAR* const NoNavMeshRemedyMsg = TEXT("No RecastNavMesh found in level. Create a NavMeshBoundsVolume first (volume.create_nav_mesh_bounds_volume) — it auto-registers the nav data.");

// Helper to get string field from JSON
static FString GetJsonStringFieldNav(const TSharedPtr<FJsonObject>& Payload, const TCHAR* FieldName, const FString& Default = TEXT(""))
{
    if (!Payload.IsValid()) return Default;
    FString Value;
    if (Payload->TryGetStringField(FieldName, Value)) { return Value; }
    return Default;
}

// Helper to get number field from JSON
static double GetJsonNumberFieldNav(const TSharedPtr<FJsonObject>& Payload, const TCHAR* FieldName, double Default = 0.0)
{
    if (!Payload.IsValid()) return Default;
    double Value;
    if (Payload->TryGetNumberField(FieldName, Value)) { return Value; }
    return Default;
}

// Helper to get bool field from JSON
static bool GetJsonBoolFieldNav(const TSharedPtr<FJsonObject>& Payload, const TCHAR* FieldName, bool Default = false)
{
    if (!Payload.IsValid()) return Default;
    bool Value;
    if (Payload->TryGetBoolField(FieldName, Value)) { return Value; }
    return Default;
}

// Helper to get FVector from JSON object field
static FVector GetJsonVectorFieldNav(const TSharedPtr<FJsonObject>& Payload, const TCHAR* FieldName, const FVector& Default = FVector::ZeroVector)
{
    if (!Payload.IsValid()) return Default;
    const TSharedPtr<FJsonObject>* VecObj;
    if (Payload->TryGetObjectField(FieldName, VecObj) && VecObj->IsValid())
    {
        return FVector(
            GetJsonNumberFieldNav(*VecObj, TEXT("x"), Default.X),
            GetJsonNumberFieldNav(*VecObj, TEXT("y"), Default.Y),
            GetJsonNumberFieldNav(*VecObj, TEXT("z"), Default.Z)
        );
    }
    return Default;
}

// Wire-string name for an ENavLinkDirection value, inverse of the BothWays-defaulting
// string->enum parse the configure/create/set-type handlers do. Single source of truth
// for the enum->string direction so an echoed direction can't drift from the parse set.
static const TCHAR* NavLinkDirectionToStringNav(ENavLinkDirection::Type Direction)
{
    switch (Direction)
    {
        case ENavLinkDirection::LeftToRight: return TEXT("LeftToRight");
        case ENavLinkDirection::RightToLeft: return TEXT("RightToLeft");
        default:                             return TEXT("BothWays");
    }
}

// Helper to get FRotator from JSON object field
static FRotator GetJsonRotatorFieldNav(const TSharedPtr<FJsonObject>& Payload, const TCHAR* FieldName, const FRotator& Default = FRotator::ZeroRotator)
{
    if (!Payload.IsValid()) return Default;
    const TSharedPtr<FJsonObject>* RotObj;
    if (Payload->TryGetObjectField(FieldName, RotObj) && RotObj->IsValid())
    {
        return FRotator(
            GetJsonNumberFieldNav(*RotObj, TEXT("pitch"), Default.Pitch),
            GetJsonNumberFieldNav(*RotObj, TEXT("yaw"), Default.Yaw),
            GetJsonNumberFieldNav(*RotObj, TEXT("roll"), Default.Roll)
        );
    }
    return Default;
}

// Helper to validate actor name
static bool IsValidActorNameNav(const FString& Name)
{
    if (Name.IsEmpty()) return false;
    if (Name.Contains(TEXT(".."))) return false;
    if (Name.Contains(TEXT("/")) || Name.Contains(TEXT("\\"))) return false;
    if (Name.Contains(TEXT(":"))) return false;
    return true;
}

// Helper to validate asset/class path
static bool IsValidNavigationPathNav(const FString& Path)
{
    if (Path.IsEmpty()) return false;
    return IsValidAssetPath(Path);
}

// ---- navigation.configure_nav_mesh_settings ----
REGISTER_RPC_HANDLER("navigation.configure_nav_mesh_settings", "navigation", "Configure RecastNavMesh settings",
    RPC_PARAMS(
        RPC_PARAM_OPT("tileSizeUU", "number", "Tile size in unreal units"),
        RPC_PARAM_OPT("cellSize", "number", "Cell size for navmesh generation"),
        RPC_PARAM_OPT("cellHeight", "number", "Cell height for navmesh generation"),
        RPC_PARAM_OPT("agentStepHeight", "number", "Max step height for agent"),
        RPC_PARAM_OPT("minRegionArea", "number", "Minimum region area"),
        RPC_PARAM_OPT("mergeRegionSize", "number", "Merge region size"),
        RPC_PARAM_OPT("maxSimplificationError", "number", "Max simplification error")
    ))
{
    auto Payload = Ctx.GetRawPayload();

    FString BlueprintPath = GetJsonStringFieldNav(Payload, TEXT("blueprintPath"));
    if (!BlueprintPath.IsEmpty())
    {
        if (!IsValidNavigationPathNav(BlueprintPath))
        {
            Ctx.SendError(TEXT("SECURITY_VIOLATION"), TEXT("Invalid blueprintPath: must not contain path traversal (..) or invalid format"));
            return true;
        }
        UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *BlueprintPath);
        if (!Blueprint)
        {
            Ctx.SendError(TEXT("NOT_FOUND"), FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintPath));
            return true;
        }
    }

    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World)
    {
        Ctx.SendError(TEXT("NO_WORLD"), TEXT("No editor world available"));
        return true;
    }

    UNavigationSystemV1* NavSys = FNavigationSystem::GetCurrent<UNavigationSystemV1>(World);
    if (!NavSys)
    {
        Ctx.SendError(TEXT("NO_NAV_SYS"), TEXT("Navigation system not available"));
        return true;
    }

    ARecastNavMesh* NavMesh = Cast<ARecastNavMesh>(NavSys->GetDefaultNavDataInstance());
    if (!NavMesh)
    {
        Ctx.SendError(TEXT("NO_NAVMESH"), NoNavMeshRemedyMsg);
        return true;
    }

    bool bModified = false;

    if (Payload->HasField(TEXT("tileSizeUU")))
    {
        NavMesh->TileSizeUU = GetJsonNumberFieldNav(Payload, TEXT("tileSizeUU"), 1000.0f);
        bModified = true;
    }
    if (Payload->HasField(TEXT("minRegionArea")))
    {
        NavMesh->MinRegionArea = GetJsonNumberFieldNav(Payload, TEXT("minRegionArea"), 0.0f);
        bModified = true;
    }
    if (Payload->HasField(TEXT("mergeRegionSize")))
    {
        NavMesh->MergeRegionSize = GetJsonNumberFieldNav(Payload, TEXT("mergeRegionSize"), 400.0f);
        bModified = true;
    }
    if (Payload->HasField(TEXT("maxSimplificationError")))
    {
        NavMesh->MaxSimplificationError = GetJsonNumberFieldNav(Payload, TEXT("maxSimplificationError"), 1.3f);
        bModified = true;
    }

    if (Payload->HasField(TEXT("cellSize")) || Payload->HasField(TEXT("cellHeight")) || Payload->HasField(TEXT("agentStepHeight")))
    {
        FNavMeshResolutionParam& DefaultParams = NavMesh->NavMeshResolutionParams[(uint8)ENavigationDataResolution::Default];
        if (Payload->HasField(TEXT("cellSize")))
        {
            DefaultParams.CellSize = GetJsonNumberFieldNav(Payload, TEXT("cellSize"), 19.0f);
            bModified = true;
        }
        if (Payload->HasField(TEXT("cellHeight")))
        {
            DefaultParams.CellHeight = GetJsonNumberFieldNav(Payload, TEXT("cellHeight"), 10.0f);
            bModified = true;
        }
        if (Payload->HasField(TEXT("agentStepHeight")))
        {
            DefaultParams.AgentMaxStepHeight = GetJsonNumberFieldNav(Payload, TEXT("agentStepHeight"), 35.0f);
            bModified = true;
        }
    }

    if (bModified) { NavMesh->MarkPackageDirty(); }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("navMeshName"), NavMesh->GetName());
    Result->SetNumberField(TEXT("tileSizeUU"), NavMesh->TileSizeUU);
    Result->SetBoolField(TEXT("modified"), bModified);
    Result->SetBoolField(TEXT("navMeshPresent"), true);
    Result->SetStringField(TEXT("navMeshPath"), NavMesh->GetPathName());
    Result->SetStringField(TEXT("navMeshClass"), NavMesh->GetClass()->GetName());
    Result->SetBoolField(TEXT("existsAfter"), true);

    Ctx.SendSuccess(Result);
    return true;
}

// ---- navigation.set_nav_agent_properties ----
REGISTER_RPC_HANDLER("navigation.set_nav_agent_properties", "navigation", "Set nav agent properties (radius, height, slope, step height)",
    RPC_PARAMS(
        RPC_PARAM_OPT("agentRadius", "number", "Agent radius (default: 35)"),
        RPC_PARAM_OPT("agentHeight", "number", "Agent height (default: 144)"),
        RPC_PARAM_OPT("agentMaxSlope", "number", "Agent max walkable slope (default: 44)"),
        RPC_PARAM_OPT("agentStepHeight", "number", "Agent max step height (default: 35)")
    ))
{
    auto Payload = Ctx.GetRawPayload();

    FString BlueprintPath = GetJsonStringFieldNav(Payload, TEXT("blueprintPath"));
    if (!BlueprintPath.IsEmpty())
    {
        if (!IsValidNavigationPathNav(BlueprintPath))
        {
            Ctx.SendError(TEXT("SECURITY_VIOLATION"), TEXT("Invalid blueprintPath"));
            return true;
        }
        UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *BlueprintPath);
        if (!Blueprint)
        {
            Ctx.SendError(TEXT("NOT_FOUND"), FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintPath));
            return true;
        }
    }

    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World) { Ctx.SendError(TEXT("NO_WORLD"), TEXT("No editor world available")); return true; }

    UNavigationSystemV1* NavSys = FNavigationSystem::GetCurrent<UNavigationSystemV1>(World);
    if (!NavSys) { Ctx.SendError(TEXT("NO_NAV_SYS"), TEXT("Navigation system not available")); return true; }

    ARecastNavMesh* NavMesh = Cast<ARecastNavMesh>(NavSys->GetDefaultNavDataInstance());
    if (!NavMesh) { Ctx.SendError(TEXT("NO_NAVMESH"), NoNavMeshRemedyMsg); return true; }

    bool bModified = false;
    if (Payload->HasField(TEXT("agentRadius"))) { NavMesh->AgentRadius = GetJsonNumberFieldNav(Payload, TEXT("agentRadius"), 35.0f); bModified = true; }
    if (Payload->HasField(TEXT("agentHeight"))) { NavMesh->AgentHeight = GetJsonNumberFieldNav(Payload, TEXT("agentHeight"), 144.0f); bModified = true; }
    if (Payload->HasField(TEXT("agentMaxSlope"))) { NavMesh->AgentMaxSlope = GetJsonNumberFieldNav(Payload, TEXT("agentMaxSlope"), 44.0f); bModified = true; }
    if (Payload->HasField(TEXT("agentStepHeight")))
    {
        FNavMeshResolutionParam& DefaultParams = NavMesh->NavMeshResolutionParams[(uint8)ENavigationDataResolution::Default];
        DefaultParams.AgentMaxStepHeight = GetJsonNumberFieldNav(Payload, TEXT("agentStepHeight"), 35.0f);
        bModified = true;
    }
    if (bModified) { NavMesh->MarkPackageDirty(); }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetNumberField(TEXT("agentRadius"), NavMesh->AgentRadius);
    Result->SetNumberField(TEXT("agentHeight"), NavMesh->AgentHeight);
    Result->SetNumberField(TEXT("agentMaxSlope"), NavMesh->AgentMaxSlope);
    Result->SetBoolField(TEXT("navMeshPresent"), true);
    Result->SetStringField(TEXT("navMeshPath"), NavMesh->GetPathName());
    Result->SetBoolField(TEXT("existsAfter"), true);
    Ctx.SendSuccess(Result);
    return true;
}

// ---- navigation.rebuild_navigation ----
REGISTER_RPC_HANDLER("navigation.rebuild_navigation", "navigation", "Trigger full navigation rebuild",
    RPC_PARAMS(RPC_PARAM_OPT("blueprintPath", "path", "Optional Blueprint asset path to validate before rebuilding; omit to operate on the active world.")))
{
    auto Payload = Ctx.GetRawPayload();

    FString BlueprintPath = GetJsonStringFieldNav(Payload, TEXT("blueprintPath"));
    if (!BlueprintPath.IsEmpty())
    {
        if (!IsValidNavigationPathNav(BlueprintPath))
        {
            Ctx.SendError(TEXT("SECURITY_VIOLATION"), TEXT("Invalid blueprintPath"));
            return true;
        }
        UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *BlueprintPath);
        if (!Blueprint)
        {
            Ctx.SendError(TEXT("NOT_FOUND"), FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintPath));
            return true;
        }
    }

    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World) { Ctx.SendError(TEXT("NO_WORLD"), TEXT("No editor world available")); return true; }

    FJobBindArgs Args;
    Args.Method = TEXT("navigation.rebuild_navigation");
    Args.StartedPayload = MakeShared<FJsonObject>();

    Args.BindNativeDelegate =
        [World](FJobOnComplete OnComplete)
    {
        BindNavigationBuildCompletion(World)(OnComplete);
        if (UNavigationSystemV1* NavSys = UNavigationSystemV1::GetNavigationSystem(World))
        {
            NavSys->Build();
        }
    };
    Ctx.StartJob(Args);
    return true;
}

// ---- navigation.create_nav_modifier_component ----
REGISTER_RPC_HANDLER("navigation.create_nav_modifier_component", "navigation", "Add a NavModifierComponent to a blueprint",
    RPC_PARAMS(
        RPC_PARAM_REQ("blueprintPath", "path", "Path to the blueprint to modify"),
        RPC_PARAM_OPT("componentName", "string", "Name for the component (default: NavModifier)"),
        RPC_PARAM_OPT("areaClass", "classref", "NavArea class path"),
        RPC_PARAM_OPT("failsafeExtent", "object", "Failsafe extent {x,y,z}"),
        RPC_PARAM_OPT("save", "boolean", "Save blueprint after modification")
    ))
{
    auto Payload = Ctx.GetRawPayload();

    FString BlueprintPath = GetJsonStringFieldNav(Payload, TEXT("blueprintPath"));
    FString ComponentName = GetJsonStringFieldNav(Payload, TEXT("componentName"), TEXT("NavModifier"));
    FString AreaClassPath = GetJsonStringFieldNav(Payload, TEXT("areaClass"));
    FVector FailsafeExtent = GetJsonVectorFieldNav(Payload, TEXT("failsafeExtent"), FVector(100, 100, 100));

    if (BlueprintPath.IsEmpty()) { Ctx.SendError(TEXT("MISSING_PARAM"), TEXT("blueprintPath is required")); return true; }
    if (!IsValidNavigationPathNav(BlueprintPath)) { Ctx.SendError(TEXT("SECURITY_VIOLATION"), TEXT("Invalid blueprintPath")); return true; }
    if (!AreaClassPath.IsEmpty() && !IsValidNavigationPathNav(AreaClassPath)) { Ctx.SendError(TEXT("SECURITY_VIOLATION"), TEXT("Invalid areaClass")); return true; }

    UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *BlueprintPath);
    if (!Blueprint) { Ctx.SendError(TEXT("NOT_FOUND"), FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintPath)); return true; }

    USimpleConstructionScript* SCS = Blueprint->SimpleConstructionScript;
    if (!SCS) { Ctx.SendError(TEXT("INVALID_BP"), TEXT("Blueprint has no SimpleConstructionScript")); return true; }

    for (USCS_Node* Node : SCS->GetAllNodes())
    {
        if (Node && Node->GetVariableName().ToString() == ComponentName)
        {
            Ctx.SendError(TEXT("ALREADY_EXISTS"), FString::Printf(TEXT("Component '%s' already exists"), *ComponentName));
            return true;
        }
    }

    USCS_Node* NewNode = SCS->CreateNode(UNavModifierComponent::StaticClass(), *ComponentName);
    if (!NewNode) { Ctx.SendError(TEXT("CREATE_FAILED"), TEXT("Failed to create SCS node")); return true; }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    UNavModifierComponent* ModComp = Cast<UNavModifierComponent>(NewNode->ComponentTemplate);
    if (ModComp)
    {
        ModComp->FailsafeExtent = FailsafeExtent;
        if (!AreaClassPath.IsEmpty())
        {
            UClass* AreaClass = LoadClass<UNavArea>(nullptr, *AreaClassPath);
            if (AreaClass) { ModComp->AreaClass = AreaClass; }
        }
    }

    SCS->AddNode(NewNode);
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

    if (GetJsonBoolFieldNav(Payload, TEXT("save"), false)) { McpSafeAssetSave(Blueprint); }

    Result->SetStringField(TEXT("componentName"), ComponentName);
    Result->SetStringField(TEXT("blueprintPath"), BlueprintPath);
    Result->SetBoolField(TEXT("existsAfter"), true);
    // Echo the resolved AreaClass / FailsafeExtent that actually landed on the component
    // template, so the create call confirms inline which nav area was authored without a
    // follow-up scs.get / asset.dump readback. That readback is intentionally sparse
    // (CDO-diff, see B-component-diff-vs-class-cdo) and DROPS AreaClass whenever it equals
    // the UNavModifierComponent constructor default (NavArea_Null) — the common no-go-zone
    // value — making "omitted because == default" indistinguishable from "set silently
    // failed". Reading ModComp->AreaClass off the template (rather than echoing the raw
    // areaClass arg) captures the no-arg fallback-to-NavArea_Null case exactly, and mirrors
    // the additive-echo convention of the siblings set_nav_area_class (areaClass echo) and
    // configure_nav_link (resolved link-geometry echo).
    if (ModComp)
    {
        Result->SetStringField(TEXT("resolvedAreaClass"),
            ModComp->AreaClass ? ModComp->AreaClass->GetPathName() : FString());
        Result->SetObjectField(TEXT("failsafeExtent"),
            JsonBuilders::BuildVectorJson(ModComp->FailsafeExtent));
    }
    AddAssetVerification(Result, Blueprint);
    Ctx.SendSuccess(Result);
    return true;
}

// ---- navigation.set_nav_area_class ----
REGISTER_RPC_HANDLER("navigation.set_nav_area_class", "navigation", "Set the nav area class on a NavModifierComponent",
    RPC_PARAMS(
        RPC_PARAM_REQ("actorName", "string", "Name or label of the actor"),
        RPC_PARAM_REQ("areaClass", "classref", "NavArea class path to apply"),
        RPC_PARAM_OPT("componentName", "string", "Specific component name to modify")
    ))
{
    auto Payload = Ctx.GetRawPayload();

    FString ActorName = GetJsonStringFieldNav(Payload, TEXT("actorName"));
    FString ComponentName = GetJsonStringFieldNav(Payload, TEXT("componentName"));
    FString AreaClassPath = GetJsonStringFieldNav(Payload, TEXT("areaClass"));

    if (ActorName.IsEmpty() || AreaClassPath.IsEmpty()) { Ctx.SendError(TEXT("MISSING_PARAM"), TEXT("actorName and areaClass are required")); return true; }
    if (!IsValidActorNameNav(ActorName)) { Ctx.SendError(TEXT("SECURITY_VIOLATION"), TEXT("Invalid actorName")); return true; }
    if (!IsValidNavigationPathNav(AreaClassPath)) { Ctx.SendError(TEXT("SECURITY_VIOLATION"), TEXT("Invalid areaClass")); return true; }

    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World) { Ctx.SendError(TEXT("NO_WORLD"), TEXT("No editor world available")); return true; }

    AActor* TargetActor = nullptr;
    for (TActorIterator<AActor> It(World); It; ++It)
    {
        if (It->GetActorLabel() == ActorName || It->GetName() == ActorName) { TargetActor = *It; break; }
    }
    if (!TargetActor) { Ctx.SendError(TEXT("NOT_FOUND"), FString::Printf(TEXT("Actor not found: %s"), *ActorName)); return true; }

    UNavModifierComponent* ModComp = nullptr;
    TArray<UNavModifierComponent*> Components;
    TargetActor->GetComponents<UNavModifierComponent>(Components);

    if (!ComponentName.IsEmpty())
    {
        for (UNavModifierComponent* Comp : Components)
        {
            if (Comp && Comp->GetName() == ComponentName) { ModComp = Comp; break; }
        }
        if (!ModComp) { Ctx.SendError(TEXT("NO_COMPONENT"), FString::Printf(TEXT("NavModifierComponent '%s' not found on actor"), *ComponentName)); return true; }
    }
    else if (Components.Num() > 0)
    {
        ModComp = Components[0];
    }
    if (!ModComp) { Ctx.SendError(TEXT("NO_COMPONENT"), TEXT("No NavModifierComponent found on actor")); return true; }

    UClass* AreaClass = LoadClass<UNavArea>(nullptr, *AreaClassPath);
    if (!AreaClass) { Ctx.SendError(TEXT("INVALID_CLASS"), FString::Printf(TEXT("NavArea class not found: %s"), *AreaClassPath)); return true; }

    ModComp->SetAreaClass(AreaClass);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("actorName"), ActorName);
    Result->SetStringField(TEXT("areaClass"), AreaClassPath);
    AddActorVerification(Result, TargetActor);
    Ctx.SendSuccess(Result);
    return true;
}

// ---- navigation.configure_nav_area_cost ----
REGISTER_RPC_HANDLER("navigation.configure_nav_area_cost", "navigation", "Configure nav area cost settings",
    RPC_PARAMS(
        RPC_PARAM_REQ("areaClass", "classref", "NavArea class path"),
        RPC_PARAM_OPT("areaCost", "number", "Default area cost (default: 1.0)"),
        RPC_PARAM_OPT("fixedAreaEnteringCost", "number", "Fixed area entering cost (read-only)")
    ))
{
    auto Payload = Ctx.GetRawPayload();

    FString AreaClassPath = GetJsonStringFieldNav(Payload, TEXT("areaClass"));
    double AreaCost = GetJsonNumberFieldNav(Payload, TEXT("areaCost"), 1.0);

    if (AreaClassPath.IsEmpty()) { Ctx.SendError(TEXT("MISSING_PARAM"), TEXT("areaClass is required")); return true; }
    if (!IsValidNavigationPathNav(AreaClassPath)) { Ctx.SendError(TEXT("SECURITY_VIOLATION"), TEXT("Invalid areaClass")); return true; }

    UClass* AreaClass = LoadClass<UNavArea>(nullptr, *AreaClassPath);
    if (!AreaClass) { Ctx.SendError(TEXT("INVALID_CLASS"), FString::Printf(TEXT("NavArea class not found: %s"), *AreaClassPath)); return true; }

    UNavArea* AreaCDO = AreaClass->GetDefaultObject<UNavArea>();
    if (!AreaCDO) { Ctx.SendError(TEXT("CDO_FAILED"), TEXT("Could not get NavArea CDO")); return true; }

    AreaCDO->DefaultCost = AreaCost;

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("areaClass"), AreaClassPath);
    Result->SetNumberField(TEXT("areaCost"), AreaCost);
    Result->SetNumberField(TEXT("fixedAreaEnteringCost"), AreaCDO->GetFixedAreaEnteringCost());
    Result->SetBoolField(TEXT("existsAfter"), true);

    FString Message = TEXT("Nav area cost configured");
    if (Payload->HasField(TEXT("fixedAreaEnteringCost")))
    {
        Message = TEXT("Nav area cost configured (note: fixedAreaEnteringCost is read-only and was not modified)");
        Result->SetBoolField(TEXT("fixedAreaEnteringCostIgnored"), true);
    }

    Ctx.SendSuccess(Result);
    return true;
}

// ---- navigation.create_nav_link_proxy ----
REGISTER_RPC_HANDLER("navigation.create_nav_link_proxy", "navigation", "Spawn a NavLinkProxy actor",
    RPC_PARAMS(
        RPC_PARAM_OPT("actorName", "string", "Name for the actor (default: NavLinkProxy)"),
        RPC_PARAM_REQ("location", "object", "Spawn location {x,y,z}"),
        RPC_PARAM_REQ("startPoint", "object", "Link start point {x,y,z}"),
        RPC_PARAM_REQ("endPoint", "object", "Link end point {x,y,z}"),
        RPC_PARAM_OPT("direction", "string", "Link direction: BothWays, LeftToRight, RightToLeft"),
        RPC_PARAM_OPT("rotation", "object", "Spawn rotation {pitch,yaw,roll}")
    ))
{
    auto Payload = Ctx.GetRawPayload();

    FString ActorName = GetJsonStringFieldNav(Payload, TEXT("actorName"), TEXT("NavLinkProxy"));
    FVector Location = GetJsonVectorFieldNav(Payload, TEXT("location"));
    FRotator Rotation = GetJsonRotatorFieldNav(Payload, TEXT("rotation"));
    FVector StartPoint = GetJsonVectorFieldNav(Payload, TEXT("startPoint"), FVector(-100, 0, 0));
    FVector EndPoint = GetJsonVectorFieldNav(Payload, TEXT("endPoint"), FVector(100, 0, 0));

    if (!Payload->HasField(TEXT("location"))) { Ctx.SendError(TEXT("MISSING_PARAM"), TEXT("location is required")); return true; }
    if (!Payload->HasField(TEXT("startPoint")) || !Payload->HasField(TEXT("endPoint")))
    {
        Ctx.SendError(TEXT("MISSING_PARAM"), TEXT("startPoint and endPoint are required to define the navigation link"));
        return true;
    }
    if (!IsValidActorNameNav(ActorName)) { Ctx.SendError(TEXT("SECURITY_VIOLATION"), TEXT("Invalid actorName")); return true; }

    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World) { Ctx.SendError(TEXT("NO_WORLD"), TEXT("No editor world available")); return true; }

    FActorSpawnParameters SpawnParams;
    SpawnParams.Name = *ActorName;
    SpawnParams.NameMode = FActorSpawnParameters::ESpawnActorNameMode::Requested;
    SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

    ANavLinkProxy* NavLink = World->SpawnActor<ANavLinkProxy>(Location, Rotation, SpawnParams);
    if (!NavLink) { Ctx.SendError(TEXT("SPAWN_FAILED"), TEXT("Failed to spawn NavLinkProxy")); return true; }

    NavLink->SetActorLabel(*ActorName);

    FNavigationLink NewLink;
    NewLink.Left = StartPoint;
    NewLink.Right = EndPoint;
    FString DirectionStr = GetJsonStringFieldNav(Payload, TEXT("direction"), TEXT("BothWays"));
    if (DirectionStr == TEXT("LeftToRight")) { NewLink.Direction = ENavLinkDirection::LeftToRight; }
    else if (DirectionStr == TEXT("RightToLeft")) { NewLink.Direction = ENavLinkDirection::RightToLeft; }
    else { NewLink.Direction = ENavLinkDirection::BothWays; }
    // ANavLinkProxy's constructor pre-seeds PointLinks[0] with a default link
    // (Left {0,-50,0}, Right {0,50,0}, SnapRadius 30, AreaClass NavArea_Default).
    // Replace that stray default rather than appending, so the caller's link is the
    // single PointLinks[0]. Otherwise the proxy carries a phantom duplicate and the
    // sibling RPCs disagree on which index is live: configure_nav_link edits
    // PointLinks[0] and the engine's simple->smart copy reads PointLinks[0], both of
    // which would target the default the caller never asked for. Mirrors the Num()==0
    // guard configure_nav_link already uses below.
    if (NavLink->PointLinks.Num() > 0) { NavLink->PointLinks[0] = NewLink; }
    else { NavLink->PointLinks.Add(NewLink); }

    World->MarkPackageDirty();

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("actorName"), NavLink->GetActorLabel());
    Result->SetStringField(TEXT("actorPath"), NavLink->GetPathName());
    AddActorVerification(Result, NavLink);
    Ctx.SendSuccess(Result);
    return true;
}

// ---- navigation.configure_nav_link ----
REGISTER_RPC_HANDLER("navigation.configure_nav_link", "navigation", "Configure an existing NavLinkProxy",
    RPC_PARAMS(
        RPC_PARAM_REQ("actorName", "string", "Name/label of the NavLinkProxy actor"),
        RPC_PARAM_OPT("startPoint", "object", "Link start point {x,y,z}"),
        RPC_PARAM_OPT("endPoint", "object", "Link end point {x,y,z}"),
        RPC_PARAM_OPT("direction", "string", "Link direction: BothWays, LeftToRight, RightToLeft"),
        RPC_PARAM_OPT("snapRadius", "number", "Snap radius for the link")
    ))
{
    auto Payload = Ctx.GetRawPayload();
    FString ActorName = GetJsonStringFieldNav(Payload, TEXT("actorName"));
    if (ActorName.IsEmpty()) { Ctx.SendError(TEXT("MISSING_PARAM"), TEXT("actorName is required")); return true; }
    if (!IsValidActorNameNav(ActorName)) { Ctx.SendError(TEXT("SECURITY_VIOLATION"), TEXT("Invalid actorName")); return true; }

    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World) { Ctx.SendError(TEXT("NO_WORLD"), TEXT("No editor world available")); return true; }

    ANavLinkProxy* NavLink = nullptr;
    for (TActorIterator<ANavLinkProxy> It(World); It; ++It)
    {
        if (It->GetActorLabel() == ActorName || It->GetName() == ActorName) { NavLink = *It; break; }
    }
    if (!NavLink) { Ctx.SendError(TEXT("NOT_FOUND"), FString::Printf(TEXT("NavLinkProxy not found: %s"), *ActorName)); return true; }

    bool bModified = false;
    if (Payload->HasField(TEXT("startPoint")) || Payload->HasField(TEXT("endPoint")))
    {
        if (NavLink->PointLinks.Num() == 0) { NavLink->PointLinks.Add(FNavigationLink()); }
        FNavigationLink& Link = NavLink->PointLinks[0];
        if (Payload->HasField(TEXT("startPoint"))) { Link.Left = GetJsonVectorFieldNav(Payload, TEXT("startPoint")); bModified = true; }
        if (Payload->HasField(TEXT("endPoint"))) { Link.Right = GetJsonVectorFieldNav(Payload, TEXT("endPoint")); bModified = true; }
        if (Payload->HasField(TEXT("direction")))
        {
            FString DirectionStr = GetJsonStringFieldNav(Payload, TEXT("direction"), TEXT("BothWays"));
            if (DirectionStr == TEXT("LeftToRight")) { Link.Direction = ENavLinkDirection::LeftToRight; }
            else if (DirectionStr == TEXT("RightToLeft")) { Link.Direction = ENavLinkDirection::RightToLeft; }
            else { Link.Direction = ENavLinkDirection::BothWays; }
            bModified = true;
        }
        if (Payload->HasField(TEXT("snapRadius"))) { Link.SnapRadius = GetJsonNumberFieldNav(Payload, TEXT("snapRadius"), 30.0f); bModified = true; }
    }
    if (bModified) { World->MarkPackageDirty(); }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("actorName"), ActorName);
    Result->SetBoolField(TEXT("modified"), bModified);
    // Echo the resolved link geometry that landed on the edited PointLinks[0], so the
    // re-tune is confirmed inline without a follow-up actor.describe readback (which
    // overflows the display limit on a NavLinkProxy and spills to file). Mirrors the
    // additive-echo convention of the sibling navigation.set_nav_area_class, which
    // echoes its applied areaClass. Read straight off the link after the write so the
    // values reflect what is actually stored, not the raw payload.
    if (NavLink->PointLinks.Num() > 0)
    {
        const FNavigationLink& AppliedLink = NavLink->PointLinks[0];
        Result->SetObjectField(TEXT("startPoint"), JsonBuilders::BuildVectorJson(AppliedLink.Left));
        Result->SetObjectField(TEXT("endPoint"), JsonBuilders::BuildVectorJson(AppliedLink.Right));
        Result->SetStringField(TEXT("direction"), NavLinkDirectionToStringNav(AppliedLink.Direction));
        Result->SetNumberField(TEXT("snapRadius"), AppliedLink.SnapRadius);
    }
    AddActorVerification(Result, NavLink);
    Ctx.SendSuccess(Result);
    return true;
}

// ---- navigation.set_nav_link_type ----
REGISTER_RPC_HANDLER("navigation.set_nav_link_type", "navigation", "Set NavLink type to simple or smart",
    RPC_PARAMS(
        RPC_PARAM_REQ("actorName", "string", "Name/label of the NavLinkProxy actor"),
        RPC_PARAM_OPT("linkType", "string", "Link type: simple or smart (default: simple)")
    ))
{
    auto Payload = Ctx.GetRawPayload();
    FString ActorName = GetJsonStringFieldNav(Payload, TEXT("actorName"));
    FString LinkType = GetJsonStringFieldNav(Payload, TEXT("linkType"), TEXT("simple"));
    if (ActorName.IsEmpty()) { Ctx.SendError(TEXT("MISSING_PARAM"), TEXT("actorName is required")); return true; }
    if (!IsValidActorNameNav(ActorName)) { Ctx.SendError(TEXT("SECURITY_VIOLATION"), TEXT("Invalid actorName")); return true; }

    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World) { Ctx.SendError(TEXT("NO_WORLD"), TEXT("No editor world available")); return true; }

    ANavLinkProxy* NavLink = nullptr;
    for (TActorIterator<ANavLinkProxy> It(World); It; ++It)
    {
        if (It->GetActorLabel() == ActorName || It->GetName() == ActorName) { NavLink = *It; break; }
    }
    if (!NavLink) { Ctx.SendError(TEXT("NOT_FOUND"), FString::Printf(TEXT("NavLinkProxy not found: %s"), *ActorName)); return true; }

    bool bSmartLink = (LinkType == TEXT("smart"));
    NavLink->bSmartLinkIsRelevant = bSmartLink;
    if (bSmartLink)
    {
        UNavLinkCustomComponent* SmartComp = NavLink->GetSmartLinkComp();
        if (SmartComp) { SmartComp->SetEnabled(true); }
    }
    World->MarkPackageDirty();

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("actorName"), ActorName);
    Result->SetStringField(TEXT("linkType"), LinkType);
    Result->SetBoolField(TEXT("bSmartLinkIsRelevant"), NavLink->bSmartLinkIsRelevant);
    AddActorVerification(Result, NavLink);
    Ctx.SendSuccess(Result);
    return true;
}

// ---- navigation.create_smart_link ----
REGISTER_RPC_HANDLER("navigation.create_smart_link", "navigation", "Spawn a smart NavLinkProxy with custom link component",
    RPC_PARAMS(
        RPC_PARAM_OPT("actorName", "string", "Name for the actor (default: SmartNavLink)"),
        RPC_PARAM_REQ("location", "object", "Spawn location {x,y,z}"),
        RPC_PARAM_REQ("startPoint", "object", "Link start point {x,y,z}"),
        RPC_PARAM_REQ("endPoint", "object", "Link end point {x,y,z}"),
        RPC_PARAM_OPT("direction", "string", "Link direction: BothWays, LeftToRight, RightToLeft"),
        RPC_PARAM_OPT("rotation", "object", "Spawn rotation {pitch,yaw,roll}")
    ))
{
    auto Payload = Ctx.GetRawPayload();
    FString ActorName = GetJsonStringFieldNav(Payload, TEXT("actorName"), TEXT("SmartNavLink"));
    FVector Location = GetJsonVectorFieldNav(Payload, TEXT("location"));
    FRotator Rotation = GetJsonRotatorFieldNav(Payload, TEXT("rotation"));
    FVector StartPoint = GetJsonVectorFieldNav(Payload, TEXT("startPoint"), FVector(-100, 0, 0));
    FVector EndPoint = GetJsonVectorFieldNav(Payload, TEXT("endPoint"), FVector(100, 0, 0));

    if (!Payload->HasField(TEXT("location"))) { Ctx.SendError(TEXT("MISSING_PARAM"), TEXT("location is required")); return true; }
    if (!Payload->HasField(TEXT("startPoint")) || !Payload->HasField(TEXT("endPoint")))
    {
        Ctx.SendError(TEXT("MISSING_PARAM"), TEXT("startPoint and endPoint are required to define the navigation link"));
        return true;
    }
    if (!IsValidActorNameNav(ActorName)) { Ctx.SendError(TEXT("SECURITY_VIOLATION"), TEXT("Invalid actorName")); return true; }

    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World) { Ctx.SendError(TEXT("NO_WORLD"), TEXT("No editor world available")); return true; }

    FActorSpawnParameters SpawnParams;
    SpawnParams.Name = *ActorName;
    SpawnParams.NameMode = FActorSpawnParameters::ESpawnActorNameMode::Requested;
    SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

    ANavLinkProxy* NavLink = World->SpawnActor<ANavLinkProxy>(Location, Rotation, SpawnParams);
    if (!NavLink) { Ctx.SendError(TEXT("SPAWN_FAILED"), TEXT("Failed to spawn NavLinkProxy")); return true; }

    NavLink->SetActorLabel(*ActorName);
    NavLink->bSmartLinkIsRelevant = true;

    UNavLinkCustomComponent* SmartComp = NavLink->GetSmartLinkComp();
    if (SmartComp)
    {
        FString DirectionStr = GetJsonStringFieldNav(Payload, TEXT("direction"), TEXT("BothWays"));
        ENavLinkDirection::Type Direction = ENavLinkDirection::BothWays;
        if (DirectionStr == TEXT("LeftToRight")) { Direction = ENavLinkDirection::LeftToRight; }
        else if (DirectionStr == TEXT("RightToLeft")) { Direction = ENavLinkDirection::RightToLeft; }
        SmartComp->SetLinkData(StartPoint, EndPoint, Direction);
        SmartComp->SetEnabled(true);
    }

    World->MarkPackageDirty();

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("actorName"), NavLink->GetActorLabel());
    Result->SetStringField(TEXT("actorPath"), NavLink->GetPathName());
    Result->SetBoolField(TEXT("bSmartLinkIsRelevant"), true);
    AddActorVerification(Result, NavLink);
    Ctx.SendSuccess(Result);
    return true;
}

// ---- navigation.configure_smart_link_behavior ----
REGISTER_RPC_HANDLER("navigation.configure_smart_link_behavior", "navigation", "Configure smart link behavior settings",
    RPC_PARAMS(
        RPC_PARAM_REQ("actorName", "string", "Name/label of the NavLinkProxy actor"),
        RPC_PARAM_OPT("linkEnabled", "boolean", "Enable/disable the smart link"),
        RPC_PARAM_OPT("enabledAreaClass", "classref", "Area class when link is enabled"),
        RPC_PARAM_OPT("disabledAreaClass", "classref", "Area class when link is disabled"),
        RPC_PARAM_OPT("broadcastRadius", "number", "Broadcast radius"),
        RPC_PARAM_OPT("broadcastInterval", "number", "Broadcast interval"),
        RPC_PARAM_OPT("bCreateBoxObstacle", "boolean", "Create box obstacle"),
        RPC_PARAM_OPT("obstacleAreaClass", "classref", "Obstacle area class"),
        RPC_PARAM_OPT("obstacleExtent", "object", "Obstacle extent {x,y,z}"),
        RPC_PARAM_OPT("obstacleOffset", "object", "Obstacle offset {x,y,z}")
    ))
{
    auto Payload = Ctx.GetRawPayload();
    FString ActorName = GetJsonStringFieldNav(Payload, TEXT("actorName"));
    if (ActorName.IsEmpty()) { Ctx.SendError(TEXT("MISSING_PARAM"), TEXT("actorName is required")); return true; }
    if (!IsValidActorNameNav(ActorName)) { Ctx.SendError(TEXT("SECURITY_VIOLATION"), TEXT("Invalid actorName")); return true; }

    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World) { Ctx.SendError(TEXT("NO_WORLD"), TEXT("No editor world available")); return true; }

    ANavLinkProxy* NavLink = nullptr;
    for (TActorIterator<ANavLinkProxy> It(World); It; ++It)
    {
        if (It->GetActorLabel() == ActorName || It->GetName() == ActorName) { NavLink = *It; break; }
    }
    if (!NavLink) { Ctx.SendError(TEXT("NOT_FOUND"), FString::Printf(TEXT("NavLinkProxy not found: %s"), *ActorName)); return true; }

    UNavLinkCustomComponent* SmartComp = NavLink->GetSmartLinkComp();
    if (!SmartComp) { Ctx.SendError(TEXT("NO_SMART_LINK"), TEXT("NavLinkProxy has no smart link component")); return true; }

    bool bModified = false;

    if (Payload->HasField(TEXT("linkEnabled")))
    {
        SmartComp->SetEnabled(GetJsonBoolFieldNav(Payload, TEXT("linkEnabled"), true));
        bModified = true;
    }
    if (Payload->HasField(TEXT("enabledAreaClass")))
    {
        FString AreaClassPath = GetJsonStringFieldNav(Payload, TEXT("enabledAreaClass"));
        UClass* AreaClass = LoadClass<UNavArea>(nullptr, *AreaClassPath);
        if (AreaClass) { SmartComp->SetEnabledArea(AreaClass); bModified = true; }
    }
    if (Payload->HasField(TEXT("disabledAreaClass")))
    {
        FString AreaClassPath = GetJsonStringFieldNav(Payload, TEXT("disabledAreaClass"));
        UClass* AreaClass = LoadClass<UNavArea>(nullptr, *AreaClassPath);
        if (AreaClass) { SmartComp->SetDisabledArea(AreaClass); bModified = true; }
    }
    if (Payload->HasField(TEXT("broadcastRadius")) || Payload->HasField(TEXT("broadcastInterval")))
    {
        float Radius = GetJsonNumberFieldNav(Payload, TEXT("broadcastRadius"), 1000.0f);
        float Interval = GetJsonNumberFieldNav(Payload, TEXT("broadcastInterval"), 0.0f);
        SmartComp->SetBroadcastData(Radius, ECC_Pawn, Interval);
        bModified = true;
    }
    if (GetJsonBoolFieldNav(Payload, TEXT("bCreateBoxObstacle"), false))
    {
        FString ObstacleAreaPath = GetJsonStringFieldNav(Payload, TEXT("obstacleAreaClass"), TEXT("/Script/NavigationSystem.NavArea_Null"));
        UClass* ObstacleArea = LoadClass<UNavArea>(nullptr, *ObstacleAreaPath);
        FVector Extent = GetJsonVectorFieldNav(Payload, TEXT("obstacleExtent"), FVector(100, 100, 100));
        FVector Offset = GetJsonVectorFieldNav(Payload, TEXT("obstacleOffset"));
        if (ObstacleArea) { SmartComp->AddNavigationObstacle(ObstacleArea, Extent, Offset); bModified = true; }
    }
    if (bModified) { World->MarkPackageDirty(); }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("actorName"), ActorName);
    Result->SetBoolField(TEXT("linkEnabled"), SmartComp->IsEnabled());
    Result->SetBoolField(TEXT("modified"), bModified);
    AddActorVerification(Result, NavLink);
    Ctx.SendSuccess(Result);
    return true;
}

// ---- navigation.get_navigation_info ----
REGISTER_RPC_HANDLER("navigation.get_navigation_info", "navigation", "Get navigation system info and status",
    RPC_PARAMS(RPC_PARAM_OPT("blueprintPath", "path", "Optional Blueprint asset path to validate; omit to report the active world's navigation system.")))
{
    auto Payload = Ctx.GetRawPayload();

    FString BlueprintPath = GetJsonStringFieldNav(Payload, TEXT("blueprintPath"));
    if (!BlueprintPath.IsEmpty())
    {
        if (!IsValidNavigationPathNav(BlueprintPath))
        {
            Ctx.SendError(TEXT("SECURITY_VIOLATION"), TEXT("Invalid blueprintPath"));
            return true;
        }
        UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *BlueprintPath);
        if (!Blueprint)
        {
            Ctx.SendError(TEXT("NOT_FOUND"), FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintPath));
            return true;
        }
    }

    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World) { Ctx.SendError(TEXT("NO_WORLD"), TEXT("No editor world available")); return true; }

    UNavigationSystemV1* NavSys = FNavigationSystem::GetCurrent<UNavigationSystemV1>(World);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    TSharedPtr<FJsonObject> NavInfo = MakeShared<FJsonObject>();

    if (NavSys)
    {
        ARecastNavMesh* NavMesh = Cast<ARecastNavMesh>(NavSys->GetDefaultNavDataInstance());
        if (NavMesh)
        {
            NavInfo->SetNumberField(TEXT("agentRadius"), NavMesh->AgentRadius);
            NavInfo->SetNumberField(TEXT("agentHeight"), NavMesh->AgentHeight);
            NavInfo->SetNumberField(TEXT("agentMaxSlope"), NavMesh->AgentMaxSlope);
            NavInfo->SetNumberField(TEXT("tileSizeUU"), NavMesh->TileSizeUU);
            const FNavMeshResolutionParam& DefaultParams = NavMesh->NavMeshResolutionParams[(uint8)ENavigationDataResolution::Default];
            NavInfo->SetNumberField(TEXT("cellSize"), DefaultParams.CellSize);
            NavInfo->SetNumberField(TEXT("cellHeight"), DefaultParams.CellHeight);
            NavInfo->SetNumberField(TEXT("agentStepHeight"), DefaultParams.AgentMaxStepHeight);
        }
        NavInfo->SetBoolField(TEXT("isNavigationBuildInProgress"), NavSys->IsNavigationBuildInProgress());
    }

    int32 NavLinkCount = 0;
    for (TActorIterator<ANavLinkProxy> It(World); It; ++It) { NavLinkCount++; }
    NavInfo->SetNumberField(TEXT("navLinkCount"), NavLinkCount);

    int32 BoundsVolumeCount = 0;
    for (TActorIterator<ANavMeshBoundsVolume> It(World); It; ++It) { BoundsVolumeCount++; }
    NavInfo->SetNumberField(TEXT("boundsVolumes"), BoundsVolumeCount);

    Result->SetObjectField(TEXT("navMeshInfo"), NavInfo);
    Ctx.SendSuccess(Result);
    return true;
}
