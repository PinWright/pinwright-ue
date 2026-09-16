// Copyright (c) 2026 Alexander Penkin. MIT License.

// SCSHandler.cpp - Migrated from PinWright_SCSHandlers.cpp
// Blueprint Simple Construction Script (SCS) component management:
// get, add, remove, reparent, set transform, set property
//
// FSCSHandlers is self-contained and registered via auto-registration macro.
// This file registers JSON-RPC method entries that wrap FSCSHandlers static methods.

#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/Blueprint/BlueprintHandlerUtils.h"
#include "Handlers/Blueprint/BlueprintReinstancingGuard.h"
#include "Handlers/Geometry/SplineHelpers.h"
#include "PinWrightHelpers.h"
#include "PinWrightSubsystem.h"
#include "PinWright_SCSHandlers.h"
#include "Utils/ComponentReadFilter.h"
#include "ScopedTransaction.h"

#include "Engine/Blueprint.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "Components/SplineComponent.h"
#include "Kismet2/BlueprintEditorUtils.h"

// Helper: send a TSharedPtr<FJsonObject> result from FSCSHandlers as Ctx response
static void SendSCSResult(
    FHandlerContext& Ctx,
    const TSharedPtr<FJsonObject>& Result,
    const BlueprintReinstancingGuard::FLiveInstanceSurvey* ReinstancingSurvey = nullptr)
{
    if (!Result.IsValid())
    {
        Ctx.SendError(TEXT("INTERNAL_ERROR"), TEXT("SCS handler returned null result"));
        return;
    }

    bool bSuccess = false;
    Result->TryGetBoolField(TEXT("success"), bSuccess);

    if (bSuccess)
    {
        if (ReinstancingSurvey)
        {
            BlueprintReinstancingGuard::AddSurveyToJson(*ReinstancingSurvey, Result);
        }
        Ctx.SendSuccess(Result);
    }
    else
    {
        FString ErrorMsg;
        Result->TryGetStringField(TEXT("error"), ErrorMsg);
        FString ErrorCode;
        if (!Result->TryGetStringField(TEXT("errorCode"), ErrorCode))
            ErrorCode = TEXT("SCS_ERROR");
        Ctx.SendError(ErrorCode, ErrorMsg.IsEmpty() ? TEXT("SCS operation failed") : ErrorMsg);
    }
}

static bool PrepareSCSCompile(
    FHandlerContext& Ctx,
    const FString& BlueprintPath,
    const TCHAR* Verb,
    BlueprintReinstancingGuard::FLiveInstanceSurvey& OutSurvey)
{
    FString NormalizedPath;
    FString LoadError;
    UBlueprint* Blueprint = LoadBlueprintAsset(
        BlueprintPath, NormalizedPath, LoadError);
    if (!Blueprint)
    {
        Ctx.SendError(TEXT("ASSET_NOT_FOUND"), LoadError.IsEmpty()
            ? FString::Printf(TEXT("Blueprint asset not found at path: %s"), *BlueprintPath)
            : LoadError);
        return false;
    }

    OutSurvey = BlueprintReinstancingGuard::SurveyLiveInstances(Blueprint);
    return !BlueprintReinstancingGuard::RefuseIfLiveInstancesWouldBeReinstanced(
        Ctx, Blueprint, OutSurvey, Verb);
}

// ---- blueprint.scs.get ----
REGISTER_RPC_HANDLER("blueprint.scs.get", "blueprint", "Read the Simple Construction Script (SCS) component tree of a Blueprint class — the templates that get instanced on every spawned actor. Returns hierarchy + per-node class/transform/properties.",
    RPC_PARAMS(
        RPC_PARAM_REQ("blueprintPath", "path", "Blueprint asset path whose SCS tree is being read."),
        RPC_PARAM_OPT("nameMatch", "string", "Case-insensitive substring filter on component name. Snake_case name_match accepted."),
        RPC_PARAM_OPT("name_match", "string", "Snake_case alias for nameMatch."),
        RPC_PARAM_OPT("componentClass", "classref", "Component UClass name or path; matches that class and subclasses. Snake_case component_class accepted."),
        RPC_PARAM_OPT("component_class", "classref", "Snake_case alias for componentClass.")
    ))
{
    FString BlueprintPath = Ctx.GetString(TEXT("blueprintPath"));
    if (BlueprintPath.IsEmpty())
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("blueprintPath required"));
        return true;
    }

    FComponentReadFilter ComponentFilter;
    FString FilterErrorCode;
    FString FilterErrorMessage;
    if (!TryParseComponentReadFilter(Ctx.GetRawPayload(), ComponentFilter,
        FilterErrorCode, FilterErrorMessage))
    {
        Ctx.SendError(FilterErrorCode, FilterErrorMessage);
        return true;
    }

    TSharedPtr<FJsonObject> Result = FSCSHandlers::GetBlueprintSCS(BlueprintPath, ComponentFilter);
    SendSCSResult(Ctx, Result);
    return true;
}

// ---- blueprint.scs.add_component ----
REGISTER_RPC_HANDLER("blueprint.scs.add_component", "blueprint", "Add a component template to a Blueprint's class-level SCS tree (every spawned instance will get this component). For instance-only components on placed actors use actor.add_component instead.",
    RPC_PARAMS(
        RPC_PARAM_REQ("blueprintPath", "path", "Blueprint asset path."),
        RPC_PARAM_REQ("componentClass", "classref", "Short class name (e.g. 'StaticMeshComponent') or full /Script path; must derive from UActorComponent."),
        RPC_PARAM_REQ("componentName", "string", "Identifier used as both the component variable name and the SCS node name."),
        RPC_PARAM_OPT("parentComponentName", "string", "Existing SCS node name to attach under; omit (or empty) to attach as a child of the root."),
        RPC_PARAM_OPT("meshPath", "path", "Static or skeletal mesh asset path; assigned when the component class is a mesh component."),
        RPC_PARAM_OPT("materialPath", "path", "Material asset path; assigned to material slot 0 of the mesh component when applicable."),
        BlueprintReinstancingGuard::AllowReinstancingParam()
    ))
{
    FString BlueprintPath = Ctx.GetString(TEXT("blueprintPath"));
    FString ComponentClass = Ctx.GetString(TEXT("componentClass"));
    FString ComponentName = Ctx.GetString(TEXT("componentName"));
    FString ParentComponentName = Ctx.GetString(TEXT("parentComponentName"));
    FString MeshPath = Ctx.GetString(TEXT("meshPath"));
    FString MaterialPath = Ctx.GetString(TEXT("materialPath"));

    if (BlueprintPath.IsEmpty() || ComponentClass.IsEmpty() || ComponentName.IsEmpty())
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("blueprintPath, componentClass, and componentName are required"));
        return true;
    }

    BlueprintReinstancingGuard::FLiveInstanceSurvey ReinstancingSurvey;
    if (!PrepareSCSCompile(
            Ctx, BlueprintPath, TEXT("blueprint.scs.add_component"), ReinstancingSurvey))
    {
        return true;
    }

    {
        FScopedTransaction Transaction(FText::FromString(TEXT("MCP: blueprint.scs.add_component")));
        TSharedPtr<FJsonObject> Result = FSCSHandlers::AddSCSComponent(
            BlueprintPath, ComponentClass, ComponentName, ParentComponentName, MeshPath, MaterialPath);
        SendSCSResult(Ctx, Result, &ReinstancingSurvey);
    }
    return true;
}

// ---- blueprint.scs.remove_component ----
REGISTER_RPC_HANDLER("blueprint.scs.remove_component", "blueprint", "Remove a component from a Blueprint's SCS",
    RPC_PARAMS(
        RPC_PARAM_REQ("blueprintPath", "path", "Blueprint asset path"),
        RPC_PARAM_REQ("componentName", "string", "Component variable name to remove"),
        BlueprintReinstancingGuard::AllowReinstancingParam()
    ))
{
    FString BlueprintPath = Ctx.GetString(TEXT("blueprintPath"));
    FString ComponentName = Ctx.GetString(TEXT("componentName"));

    if (BlueprintPath.IsEmpty() || ComponentName.IsEmpty())
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("blueprintPath and componentName are required"));
        return true;
    }

    BlueprintReinstancingGuard::FLiveInstanceSurvey ReinstancingSurvey;
    if (!PrepareSCSCompile(
            Ctx, BlueprintPath, TEXT("blueprint.scs.remove_component"), ReinstancingSurvey))
    {
        return true;
    }

    {
        FScopedTransaction Transaction(FText::FromString(TEXT("MCP: blueprint.scs.remove_component")));
        TSharedPtr<FJsonObject> Result = FSCSHandlers::RemoveSCSComponent(BlueprintPath, ComponentName);
        SendSCSResult(Ctx, Result, &ReinstancingSurvey);
    }
    return true;
}

// ---- blueprint.scs.reparent_component ----
REGISTER_RPC_HANDLER("blueprint.scs.reparent_component", "blueprint", "Reparent a component in a Blueprint's SCS hierarchy",
    RPC_PARAMS(
        RPC_PARAM_REQ("blueprintPath", "path", "Blueprint asset path"),
        RPC_PARAM_REQ("componentName", "string", "Component variable name to reparent"),
        RPC_PARAM_OPT("newParentName", "string", "New parent component name (empty for root)"),
        BlueprintReinstancingGuard::AllowReinstancingParam()
    ))
{
    FString BlueprintPath = Ctx.GetString(TEXT("blueprintPath"));
    FString ComponentName = Ctx.GetString(TEXT("componentName"));
    FString NewParentName = Ctx.GetString(TEXT("newParentName"));

    if (BlueprintPath.IsEmpty() || ComponentName.IsEmpty())
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("blueprintPath and componentName are required"));
        return true;
    }

    BlueprintReinstancingGuard::FLiveInstanceSurvey ReinstancingSurvey;
    if (!PrepareSCSCompile(
            Ctx, BlueprintPath, TEXT("blueprint.scs.reparent_component"), ReinstancingSurvey))
    {
        return true;
    }

    {
        FScopedTransaction Transaction(FText::FromString(TEXT("MCP: blueprint.scs.reparent_component")));
        TSharedPtr<FJsonObject> Result = FSCSHandlers::ReparentSCSComponent(BlueprintPath, ComponentName, NewParentName);
        SendSCSResult(Ctx, Result, &ReinstancingSurvey);
    }
    return true;
}

// ---- blueprint.scs.set_transform ----
REGISTER_RPC_HANDLER("blueprint.scs.set_transform", "blueprint", "Set transform of a component in a Blueprint's SCS",
    RPC_PARAMS(
        RPC_PARAM_REQ("blueprintPath", "path", "Blueprint asset path"),
        RPC_PARAM_REQ("componentName", "string", "Component variable name"),
        RPC_PARAM_OPT("location", "array", "Location [x, y, z]"),
        RPC_PARAM_OPT("rotation", "array", "Rotation [pitch, yaw, roll]"),
        RPC_PARAM_OPT("scale", "array", "Scale [x, y, z]"),
        BlueprintReinstancingGuard::AllowReinstancingParam()
    ))
{
    FString BlueprintPath = Ctx.GetString(TEXT("blueprintPath"));
    FString ComponentName = Ctx.GetString(TEXT("componentName"));

    if (BlueprintPath.IsEmpty() || ComponentName.IsEmpty())
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("blueprintPath and componentName are required"));
        return true;
    }

    BlueprintReinstancingGuard::FLiveInstanceSurvey ReinstancingSurvey;
    if (!PrepareSCSCompile(
            Ctx, BlueprintPath, TEXT("blueprint.scs.set_transform"), ReinstancingSurvey))
    {
        return true;
    }

    // Build a transform data object from the raw payload
    // The original SetSCSComponentTransform expects location/rotation/scale as arrays
    TSharedPtr<FJsonObject> TransformData = MakeShared<FJsonObject>();
    auto* Payload = Ctx.GetRawPayload().Get();

    const TArray<TSharedPtr<FJsonValue>>* LocArray;
    if (Payload->TryGetArrayField(TEXT("location"), LocArray))
        TransformData->SetArrayField(TEXT("location"), *LocArray);

    const TArray<TSharedPtr<FJsonValue>>* RotArray;
    if (Payload->TryGetArrayField(TEXT("rotation"), RotArray))
        TransformData->SetArrayField(TEXT("rotation"), *RotArray);

    const TArray<TSharedPtr<FJsonValue>>* ScaleArray;
    if (Payload->TryGetArrayField(TEXT("scale"), ScaleArray))
        TransformData->SetArrayField(TEXT("scale"), *ScaleArray);

    {
        FScopedTransaction Transaction(FText::FromString(TEXT("MCP: blueprint.scs.set_transform")));
        TSharedPtr<FJsonObject> Result = FSCSHandlers::SetSCSComponentTransform(BlueprintPath, ComponentName, TransformData);
        SendSCSResult(Ctx, Result, &ReinstancingSurvey);
    }
    return true;
}

// ---- blueprint.scs.set_property ----
REGISTER_RPC_HANDLER("blueprint.scs.set_property", "blueprint", "Set a UPROPERTY on an SCS component template (class-level default), so every newly spawned instance picks it up. For per-instance edits on placed actors use actor.set_component_properties instead.",
    RPC_PARAMS(
        RPC_PARAM_REQ("blueprintPath", "path", "Blueprint asset path."),
        RPC_PARAM_REQ("componentName", "string", "SCS node / component variable name."),
        RPC_PARAM_REQ("propertyName", "string", "UPROPERTY name; supports dot-paths for nested struct members (e.g. 'RelativeLocation.X')."),
        RPC_PARAM_REQ("propertyValue", "any", "JSON value to assign; type must match the property (number, string, bool, object/array for structs and containers)."),
        BlueprintReinstancingGuard::AllowReinstancingParam()
    ))
{
    FString BlueprintPath = Ctx.GetString(TEXT("blueprintPath"));
    FString ComponentName = Ctx.GetString(TEXT("componentName"));
    FString PropertyName = Ctx.GetString(TEXT("propertyName"));

    if (BlueprintPath.IsEmpty() || ComponentName.IsEmpty() || PropertyName.IsEmpty())
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("blueprintPath, componentName, and propertyName are required"));
        return true;
    }

    BlueprintReinstancingGuard::FLiveInstanceSurvey ReinstancingSurvey;
    if (!PrepareSCSCompile(
            Ctx, BlueprintPath, TEXT("blueprint.scs.set_property"), ReinstancingSurvey))
    {
        return true;
    }

    // Extract propertyValue as a raw FJsonValue from payload
    auto* Payload = Ctx.GetRawPayload().Get();
    TSharedPtr<FJsonValue> PropertyValue;
    if (Payload->HasField(TEXT("propertyValue")))
        PropertyValue = Payload->TryGetField(TEXT("propertyValue"));

    {
        FScopedTransaction Transaction(FText::FromString(TEXT("MCP: blueprint.scs.set_property")));
        TSharedPtr<FJsonObject> Result = FSCSHandlers::SetSCSComponentProperty(
            BlueprintPath, ComponentName, PropertyName, PropertyValue);
        SendSCSResult(Ctx, Result, &ReinstancingSurvey);
    }
    return true;
}

// ---- blueprint.scs.set_spline_points ----
// The spline point-type string<->enum mapping is shared with the actor-scoped spline.*
// handlers via Handlers/Geometry/SplineHelpers.h (SplineHelpers::ParseSplinePointType /
// SplinePointTypeToString).

// Read a local-space point location from a JSON entry that is either {x,y,z} directly
// or {location:{x,y,z}} (matching spline.create_spline_actor's points format).
static bool SCSReadSplinePointLocation(const TSharedPtr<FJsonValue>& Value, FVector& OutLocation)
{
    const TSharedPtr<FJsonObject>* PointObj = nullptr;
    if (!Value.IsValid() || !Value->TryGetObject(PointObj) || !PointObj || !(*PointObj).IsValid())
    {
        return false;
    }
    // Route both accepted shapes through the shared Utils/JsonUtils readers so this handler
    // stays consistent with the rest of the plugin's JSON->FVector extraction. The nested
    // {location:{...}} form reuses ExtractVectorField (which also accepts X/Y/Z casing and the
    // [x,y,z] array shape); the bare {x,y,z} form reads the point object directly via the
    // shared number helper (ExtractVectorField keys off a field name, so it can't cover it).
    if ((*PointObj)->HasField(TEXT("location")))
    {
        OutLocation = ExtractVectorField(*PointObj, TEXT("location"), FVector::ZeroVector);
        return true;
    }
    OutLocation = FVector(
        GetJsonNumberField(*PointObj, TEXT("x")),
        GetJsonNumberField(*PointObj, TEXT("y")),
        GetJsonNumberField(*PointObj, TEXT("z")));
    return true;
}

REGISTER_RPC_HANDLER("blueprint.scs.set_spline_points", "blueprint", "Author the point data of a USplineComponent that lives in a Blueprint's SCS (class template): replace all spline points, set their point type, optionally set the closed-loop flag, then rebuild the spline (UpdateSpline). Pairs with blueprint.scs.add_component, which creates the SplineComponent but cannot set point positions/types. For a spline on a placed actor use the actor-scoped spline.* verbs instead.",
    RPC_PARAMS(
        RPC_PARAM_REQ("blueprintPath", "path", "Blueprint asset path whose SCS holds the spline component."),
        RPC_PARAM_REQ("componentName", "string", "SCS node / component variable name of the USplineComponent template."),
        RPC_PARAM_REQ("points", "array", "Ordered spline points; each entry is {x,y,z} or {location:{x,y,z}} in the component's local space. Replaces all existing points."),
        RPC_PARAM_OPT("pointType", "string", "Point type applied to every point (Linear, Curve, Constant, CurveClamped, CurveCustomTangent). Default Curve."),
        RPC_PARAM_OPT("closedLoop", "boolean", "When present, sets the spline's closed-loop flag."),
        RPC_PARAM_OPT("save", "boolean", "Save the Blueprint after authoring (default false).")
    ))
{
    FString BlueprintPath = Ctx.GetString(TEXT("blueprintPath"));
    FString ComponentName = Ctx.GetString(TEXT("componentName"));
    FString PointTypeStr = Ctx.GetString(TEXT("pointType"), TEXT("Curve"));

    if (BlueprintPath.IsEmpty() || ComponentName.IsEmpty())
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("blueprintPath and componentName are required"));
        return true;
    }

    const TArray<TSharedPtr<FJsonValue>>* PointsArray = Ctx.GetArray(TEXT("points"));
    if (!PointsArray)
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("points array is required"));
        return true;
    }

    // Resolve via the shared LoadBlueprintAsset helper (like every sibling blueprint.scs.*
    // handler) instead of a raw LoadObject on the package path: it finds in-memory/transient
    // and unsaved Blueprints (FindObject on the full object path) and normalizes the path,
    // whereas LoadObject on a dotless package path misses them.
    FString NormalizedPath;
    FString LoadError;
    UBlueprint* Blueprint = LoadBlueprintAsset(BlueprintPath, NormalizedPath, LoadError);
    if (!Blueprint)
    {
        Ctx.SendError(TEXT("NOT_FOUND"),
            LoadError.IsEmpty()
                ? FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintPath)
                : LoadError);
        return true;
    }

    USimpleConstructionScript* SCS = Blueprint->SimpleConstructionScript;
    if (!SCS)
    {
        Ctx.SendError(TEXT("INVALID_BP"), TEXT("Blueprint has no SimpleConstructionScript"));
        return true;
    }

    // Reuse the canonical case-insensitive SCS lookup (Utils/AssetUtils) instead of
    // re-scanning GetAllNodes() by hand.
    USplineComponent* SplineComp = nullptr;
    if (USCS_Node* Node = FindScsNodeByName(SCS, ComponentName))
    {
        SplineComp = Cast<USplineComponent>(Node->ComponentTemplate);
    }

    if (!SplineComp)
    {
        Ctx.SendError(TEXT("NOT_A_SPLINE"),
            FString::Printf(TEXT("No USplineComponent template named '%s' in the SCS of %s"),
                *ComponentName, *BlueprintPath));
        return true;
    }

    const ESplinePointType::Type PointType = SplineHelpers::ParseSplinePointType(PointTypeStr);

    // Wrap the class-template mutation in a transaction so it is undoable, matching the
    // sibling blueprint.scs.* handlers (add_component/remove_component/reparent_component/
    // set_transform/set_property).
    {
        FScopedTransaction Transaction(FText::FromString(TEXT("MCP: blueprint.scs.set_spline_points")));
        SplineComp->Modify();

        // Replace all points in a single pass, deferring the recompute until every point + type
        // is set. AddedCount (rather than the PointsArray index) keeps the spline index correct
        // when SCSReadSplinePointLocation skips an unparseable entry.
        SplineComp->ClearSplinePoints(false);
        int32 AddedCount = 0;
        for (const TSharedPtr<FJsonValue>& PointValue : *PointsArray)
        {
            FVector Location;
            if (SCSReadSplinePointLocation(PointValue, Location))
            {
                SplineComp->AddSplinePoint(Location, ESplineCoordinateSpace::Local, false);
                SplineComp->SetSplinePointType(AddedCount++, PointType, false);
            }
        }

        const TSharedPtr<FJsonObject>& Payload = Ctx.GetRawPayload();
        if (Payload.IsValid() && Payload->HasField(TEXT("closedLoop")))
        {
            SplineComp->SetClosedLoop(Ctx.GetBool(TEXT("closedLoop"), false));
        }

        // Rebuild the interp curves / reparam tables from the authored points — the recompute
        // blueprint.scs.set_property could never perform (it sets a UPROPERTY without UpdateSpline).
        SplineComp->UpdateSpline();

        FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
    }

    // Mark-dirty only (Blueprint). saved is reported below from AddMarkDirtySaveReport,
    // which measures; it used to be McpSafeAssetSave's constant-true return.
    const bool bSaveRequested = Ctx.GetBool(TEXT("save"), false);
    if (bSaveRequested)
    {
        McpSafeAssetSave(Blueprint);
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("componentName"), ComponentName);
    Result->SetStringField(TEXT("blueprintPath"), BlueprintPath);
    Result->SetNumberField(TEXT("pointCount"), SplineComp->GetNumberOfSplinePoints());
    Result->SetNumberField(TEXT("splineLength"), SplineComp->GetSplineLength());
    Result->SetBoolField(TEXT("closedLoop"), SplineComp->IsClosedLoop());
    // Report the point type actually applied (the resolved enum), not the raw request string,
    // so a miscased/unrecognized input (coerced to Curve) doesn't misreport what was authored.
    Result->SetStringField(TEXT("pointType"), SplineHelpers::SplinePointTypeToString(PointType));
    AddMarkDirtySaveReport(Result, Blueprint, bSaveRequested);
    AddAssetVerification(Result, Blueprint);
    Ctx.SendSuccess(Result);
    return true;
}
