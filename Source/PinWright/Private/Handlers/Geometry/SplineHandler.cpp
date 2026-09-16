// Copyright (c) 2026 Alexander Penkin. MIT License.

// Spline System Handlers - Migrated to auto-registration pattern
#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/ErrorCodes.h"
#include "Handlers/Geometry/SplineHelpers.h"
#include "PinWrightHelpers.h"
#include "Utils/DerivedStateReport.h"
#include "Utils/JsonBuilders.h"
#include "Dom/JsonObject.h"
#include "Misc/EngineVersionComparison.h"

#include "Editor.h"
#include "Engine/World.h"
#include "EngineUtils.h"

// Spline System includes
#include "Components/SplineComponent.h"
#include "Components/SplineMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Materials/MaterialInterface.h"
#include "GameFramework/Actor.h"

DEFINE_LOG_CATEGORY_STATIC(LogMcpSplineHandlersNew, Log, All);


// ============================================================================
// Static Helpers
// ============================================================================

namespace SplineHandlerHelpers
{

// Helper to get FVector from JSON object field
static FVector GetJsonVectorField(const TSharedPtr<FJsonObject>& Payload, const TCHAR* FieldName, const FVector& Default = FVector::ZeroVector)
{
    if (!Payload.IsValid()) return Default;
    const TSharedPtr<FJsonObject>* VecObj;
    if (Payload->TryGetObjectField(FieldName, VecObj) && VecObj->IsValid())
    {
        double X = Default.X, Y = Default.Y, Z = Default.Z;
        (*VecObj)->TryGetNumberField(TEXT("x"), X);
        (*VecObj)->TryGetNumberField(TEXT("y"), Y);
        (*VecObj)->TryGetNumberField(TEXT("z"), Z);
        return FVector(X, Y, Z);
    }
    return Default;
}

// Helper to get FRotator from JSON object field
static FRotator GetJsonRotatorField(const TSharedPtr<FJsonObject>& Payload, const TCHAR* FieldName, const FRotator& Default = FRotator::ZeroRotator)
{
    if (!Payload.IsValid()) return Default;
    const TSharedPtr<FJsonObject>* RotObj;
    if (Payload->TryGetObjectField(FieldName, RotObj) && RotObj->IsValid())
    {
        double Pitch = Default.Pitch, Yaw = Default.Yaw, Roll = Default.Roll;
        (*RotObj)->TryGetNumberField(TEXT("pitch"), Pitch);
        (*RotObj)->TryGetNumberField(TEXT("yaw"), Yaw);
        (*RotObj)->TryGetNumberField(TEXT("roll"), Roll);
        return FRotator(Pitch, Yaw, Roll);
    }
    return Default;
}

// Helper to find actor by name
static AActor* FindActorByName(UWorld* World, const FString& ActorName)
{
    if (!World || ActorName.IsEmpty()) return nullptr;

    for (TActorIterator<AActor> It(World); It; ++It)
    {
        if (It->GetActorLabel() == ActorName || It->GetName() == ActorName)
        {
            return *It;
        }
    }
    return nullptr;
}

// Helper to find spline component on actor
static USplineComponent* FindSplineComponent(AActor* Actor, const FString& ComponentName = TEXT(""))
{
    if (!Actor) return nullptr;

    TArray<USplineComponent*> SplineComponents;
    Actor->GetComponents<USplineComponent>(SplineComponents);

    if (SplineComponents.Num() == 0) return nullptr;

    if (!ComponentName.IsEmpty())
    {
        for (USplineComponent* Comp : SplineComponents)
        {
            if (Comp && Comp->GetName() == ComponentName)
            {
                return Comp;
            }
        }
        return nullptr;
    }

    return SplineComponents[0];
}

// Helper to find a spline-mesh component on actor. USplineMeshComponent derives
// from UStaticMeshComponent (NOT from USplineComponent), so FindSplineComponent
// can never return it — spline-mesh actors need their own lookup for readback.
static USplineMeshComponent* FindSplineMeshComponent(AActor* Actor, const FString& ComponentName = TEXT(""))
{
    if (!Actor) return nullptr;

    TArray<USplineMeshComponent*> MeshComponents;
    Actor->GetComponents<USplineMeshComponent>(MeshComponents);

    if (MeshComponents.Num() == 0) return nullptr;

    if (!ComponentName.IsEmpty())
    {
        for (USplineMeshComponent* Comp : MeshComponents)
        {
            if (Comp && Comp->GetName() == ComponentName)
            {
                return Comp;
            }
        }
        return nullptr;
    }

    return MeshComponents[0];
}

// Names the authoritative route for a spline whose point scale the engine re-derives.
//
// USplineComponent::AllowsSplinePointScaleEditing (SplineComponent.h:425) defaults to
// true and is overridden to false by spline types whose Scale curve is not the authority
// for what it appears to control. UWaterSplineComponent is one
// (WaterSplineComponent.h:54): UWaterSplineComponent::SynchronizeWaterProperties assigns
// Scale.X from UWaterSplineMetadata::RiverWidth and Scale.Y from Depth
// (WaterSplineComponent.cpp:231-246), and PostLoad calls it (:26-39). A Scale write
// therefore survives read-back and survives save, and is recomputed away on the next
// level load — the exact defect shape docs/rpc-design.md §5 is about.
//
// Returns the verb to send the caller to. Empty when the spline is a plain
// USplineComponent whose scale IS the authority (no refusal should be raised at all).
static void DescribeDerivedSplineScale(
    USplineComponent* SplineComp,
    FString& OutDerivedFrom,
    FString& OutAuthoritativeVerb,
    FString& OutExplanation)
{
    OutDerivedFrom = TEXT("the spline type's own metadata");
    OutAuthoritativeVerb.Reset();
    OutExplanation = TEXT(
        "This spline component reports AllowsSplinePointScaleEditing() == false: the "
        "engine re-derives the point scale from other state, so a scale write here is "
        "recomputed away on the next level load.");

    if (!SplineComp)
    {
        return;
    }

    // Resolved by reflection rather than by linking the Water plugin into this file: the
    // spline namespace deliberately carries no Water dependency (see CLAUDE.md, Module
    // Split), and the check has to keep working on a host where Water is disabled — there
    // FindObject returns null and the branch simply never fires.
    if (const UClass* WaterSplineClass =
            FindObject<UClass>(nullptr, TEXT("/Script/Water.WaterSplineComponent")))
    {
        if (SplineComp->IsA(WaterSplineClass))
        {
            OutDerivedFrom = TEXT("UWaterSplineMetadata::RiverWidth (Scale.X) and "
                                  "UWaterSplineMetadata::Depth (Scale.Y)");
            OutAuthoritativeVerb = TEXT("water.set_river_width_at_spline_point");
            OutExplanation = TEXT(
                "UE Water stores river width and depth in the water spline's metadata and "
                "re-derives Scale.X / Scale.Y from them in "
                "UWaterSplineComponent::SynchronizeWaterProperties, which PostLoad calls. "
                "Writing Scale is a write to a derived value: it reads back correctly, it "
                "saves, and it reverts to the metadata value on the next level load. Use "
                "water.set_river_width_at_spline_point (width) or "
                "water.set_river_depth_at_spline_point (depth) instead.");
        }
    }
}

// Helper to convert a spline-mesh forward axis enum to a string.
static FString SplineMeshAxisToString(ESplineMeshAxis::Type Axis)
{
    switch (Axis)
    {
        case ESplineMeshAxis::Y: return TEXT("Y");
        case ESplineMeshAxis::Z: return TEXT("Z");
        case ESplineMeshAxis::X:
        default: return TEXT("X");
    }
}

// Populate a JSON object with the readable state of a spline-mesh component:
// forward axis and the start/end position + tangent that define the deformation.
static void FillSplineMeshFields(const TSharedPtr<FJsonObject>& Obj, USplineMeshComponent* MeshComp)
{
    if (!Obj.IsValid() || !MeshComp) return;

    Obj->SetStringField(TEXT("componentName"), MeshComp->GetName());
    Obj->SetStringField(TEXT("componentClass"), TEXT("SplineMeshComponent"));
    Obj->SetStringField(TEXT("forwardAxis"), SplineMeshAxisToString(MeshComp->GetForwardAxis()));

    Obj->SetObjectField(TEXT("startPosition"), JsonBuilders::BuildVectorJson(MeshComp->GetStartPosition()));
    Obj->SetObjectField(TEXT("startTangent"), JsonBuilders::BuildVectorJson(MeshComp->GetStartTangent()));
    Obj->SetObjectField(TEXT("endPosition"), JsonBuilders::BuildVectorJson(MeshComp->GetEndPosition()));
    Obj->SetObjectField(TEXT("endTangent"), JsonBuilders::BuildVectorJson(MeshComp->GetEndTangent()));
}

} // namespace SplineHandlerHelpers


// ============================================================================
// Spline Creation Handlers
// ============================================================================

REGISTER_RPC_HANDLER("spline.create_spline_actor", "spline", "Spawn a new actor with a spline component in the world",
    RPC_PARAMS(
        RPC_PARAM_OPT("actorName", "string", "Name for the spline actor"),
        RPC_PARAM_OPT("location", "object", "Spawn location {x, y, z}"),
        RPC_PARAM_OPT("rotation", "object", "Spawn rotation {pitch, yaw, roll}"),
        RPC_PARAM_OPT("bClosedLoop", "boolean", "Whether the spline forms a closed loop"),
        RPC_PARAM_OPT("splineType", "string", "Spline point type (Curve, Linear, Constant, etc.)"),
        RPC_PARAM_OPT("points", "array", "Initial spline points [{location:{x,y,z}}, ...]")
    ))
{
    FString ActorName = Ctx.GetString(TEXT("actorName"), TEXT("SplineActor"));
    FVector Location = SplineHandlerHelpers::GetJsonVectorField(Ctx.GetRawPayload(), TEXT("location"));
    FRotator Rotation = SplineHandlerHelpers::GetJsonRotatorField(Ctx.GetRawPayload(), TEXT("rotation"));
    bool bClosedLoop = Ctx.GetBool(TEXT("bClosedLoop"), false);
    FString SplineType = Ctx.GetString(TEXT("splineType"), TEXT("Curve"));

    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World)
    {
        Ctx.SendError(ErrorCodes::ERR_NO_WORLD, TEXT("No editor world available"));
        return true;
    }

    FActorSpawnParameters SpawnParams;
    SpawnParams.Name = *ActorName;
    SpawnParams.NameMode = FActorSpawnParameters::ESpawnActorNameMode::Requested;
    SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

    AActor* NewActor = World->SpawnActor<AActor>(AActor::StaticClass(), Location, Rotation, SpawnParams);
    if (!NewActor)
    {
        Ctx.SendError(ErrorCodes::ERR_SPAWN_FAILED, TEXT("Failed to spawn spline actor"));
        return true;
    }

    NewActor->SetActorLabel(*ActorName);

    USplineComponent* SplineComp = NewObject<USplineComponent>(NewActor, TEXT("SplineComponent"));
    if (!SplineComp)
    {
        NewActor->Destroy();
        Ctx.SendError(ErrorCodes::ERR_COMPONENT_FAILED, TEXT("Failed to create spline component"));
        return true;
    }

    SplineComp->RegisterComponent();
    NewActor->AddInstanceComponent(SplineComp);
    NewActor->SetRootComponent(SplineComp);

    SplineComp->SetClosedLoop(bClosedLoop);

    ESplinePointType::Type PointType = SplineHelpers::ParseSplinePointType(SplineType);
    for (int32 i = 0; i < SplineComp->GetNumberOfSplinePoints(); i++)
    {
        SplineComp->SetSplinePointType(i, PointType, true);
    }
    SplineComp->UpdateSpline();

    // Parse initial points if provided
    const TArray<TSharedPtr<FJsonValue>>* PointsArray = Ctx.GetArray(TEXT("points"));
    if (PointsArray)
    {
        SplineComp->ClearSplinePoints(false);
        for (int32 i = 0; i < PointsArray->Num(); i++)
        {
            const TSharedPtr<FJsonObject>* PointObj;
            if ((*PointsArray)[i]->TryGetObject(PointObj))
            {
                FVector PointLocation = SplineHandlerHelpers::GetJsonVectorField(*PointObj, TEXT("location"));
                SplineComp->AddSplinePoint(PointLocation, ESplineCoordinateSpace::Local, true);
                SplineComp->SetSplinePointType(i, PointType, false);
            }
        }
        SplineComp->UpdateSpline();
    }

    World->MarkPackageDirty();

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetNumberField(TEXT("pointCount"), SplineComp->GetNumberOfSplinePoints());
    Result->SetNumberField(TEXT("splineLength"), SplineComp->GetSplineLength());
    Result->SetBoolField(TEXT("closedLoop"), SplineComp->IsClosedLoop());

    // AddActorVerification owns actorName (= label) and actorPath (= the unique
    // object path); see Utils/AssetUtils.cpp. Writing those before it is dead.
    AddActorVerification(Result, NewActor);

    // After AddActorVerification, surface the collision/dedup signal (the unique
    // actorObjectName + nameWasDeduplicated flag) the verification helper omits.
    AddActorNameDeduplicationSignal(Result, NewActor, ActorName);

    Ctx.SendSuccess(Result);
    return true;
}

REGISTER_RPC_HANDLER("spline.add_spline_point", "spline", "Add a point to a spline component on an actor",
    RPC_PARAMS(
        RPC_PARAM_REQ("actorName", "string", "Name of the actor with the spline"),
        RPC_PARAM_OPT("position", "object", "Point position {x, y, z}"),
        RPC_PARAM_OPT("index", "integer", "Index to insert at (-1 for end)"),
        RPC_PARAM_OPT("pointType", "string", "Spline point type")
    ))
{
    FString ActorName = Ctx.GetString(TEXT("actorName"));
    FVector Position = SplineHandlerHelpers::GetJsonVectorField(Ctx.GetRawPayload(), TEXT("position"));
    int32 Index = Ctx.GetInt(TEXT("index"), -1);
    FString PointType = Ctx.GetString(TEXT("pointType"), TEXT("Curve"));

    if (ActorName.IsEmpty())
    {
        Ctx.SendError(ErrorCodes::ERR_MISSING_PARAM, TEXT("actorName is required"));
        return true;
    }

    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World)
    {
        Ctx.SendError(ErrorCodes::ERR_NO_WORLD, TEXT("No editor world available"));
        return true;
    }

    AActor* Actor = SplineHandlerHelpers::FindActorByName(World, ActorName);
    if (!Actor)
    {
        Ctx.SendError(ErrorCodes::ERR_NOT_FOUND, FString::Printf(TEXT("Actor not found: %s"), *ActorName));
        return true;
    }

    USplineComponent* SplineComp = SplineHandlerHelpers::FindSplineComponent(Actor);
    if (!SplineComp)
    {
        Ctx.SendError(ErrorCodes::ERR_NO_SPLINE, TEXT("No spline component found on actor"));
        return true;
    }

    if (Index < 0 || Index >= SplineComp->GetNumberOfSplinePoints())
    {
        SplineComp->AddSplinePoint(Position, ESplineCoordinateSpace::Local, true);
        Index = SplineComp->GetNumberOfSplinePoints() - 1;
    }
    else
    {
        SplineComp->AddSplinePointAtIndex(Position, Index, ESplineCoordinateSpace::Local, true);
    }

    SplineComp->SetSplinePointType(Index, SplineHelpers::ParseSplinePointType(PointType), true);
    SplineComp->UpdateSpline();

    World->MarkPackageDirty();

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetNumberField(TEXT("pointIndex"), Index);
    Result->SetNumberField(TEXT("totalPoints"), SplineComp->GetNumberOfSplinePoints());

    AddActorVerification(Result, Actor);

    Ctx.SendSuccess(Result);
    return true;
}

REGISTER_RPC_HANDLER("spline.remove_spline_point", "spline", "Remove a point from a spline component",
    RPC_PARAMS(
        RPC_PARAM_REQ("actorName", "string", "Name of the actor with the spline"),
        RPC_PARAM_OPT("pointIndex", "integer", "Index of the point to remove")
    ))
{
    FString ActorName = Ctx.GetString(TEXT("actorName"));
    int32 PointIndex = Ctx.GetInt(TEXT("pointIndex"), 0);

    if (ActorName.IsEmpty())
    {
        Ctx.SendError(ErrorCodes::ERR_MISSING_PARAM, TEXT("actorName is required"));
        return true;
    }

    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World)
    {
        Ctx.SendError(ErrorCodes::ERR_NO_WORLD, TEXT("No editor world available"));
        return true;
    }

    AActor* Actor = SplineHandlerHelpers::FindActorByName(World, ActorName);
    if (!Actor)
    {
        Ctx.SendError(ErrorCodes::ERR_NOT_FOUND, FString::Printf(TEXT("Actor not found: %s"), *ActorName));
        return true;
    }

    USplineComponent* SplineComp = SplineHandlerHelpers::FindSplineComponent(Actor);
    if (!SplineComp)
    {
        Ctx.SendError(ErrorCodes::ERR_NO_SPLINE, TEXT("No spline component found on actor"));
        return true;
    }

    if (PointIndex < 0 || PointIndex >= SplineComp->GetNumberOfSplinePoints())
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_INDEX, FString::Printf(TEXT("Invalid point index: %d"), PointIndex));
        return true;
    }

    SplineComp->RemoveSplinePoint(PointIndex, true);
    SplineComp->UpdateSpline();

    World->MarkPackageDirty();

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetNumberField(TEXT("removedIndex"), PointIndex);
    Result->SetNumberField(TEXT("remainingPoints"), SplineComp->GetNumberOfSplinePoints());

    AddActorVerification(Result, Actor);

    Ctx.SendSuccess(Result);
    return true;
}

REGISTER_RPC_HANDLER("spline.set_spline_point_position", "spline", "Set the position of a spline point",
    RPC_PARAMS(
        RPC_PARAM_REQ("actorName", "string", "Name of the actor with the spline"),
        RPC_PARAM_OPT("pointIndex", "integer", "Index of the point"),
        RPC_PARAM_OPT("position", "object", "New position {x, y, z}")
    ))
{
    FString ActorName = Ctx.GetString(TEXT("actorName"));
    int32 PointIndex = Ctx.GetInt(TEXT("pointIndex"), 0);
    FVector Position = SplineHandlerHelpers::GetJsonVectorField(Ctx.GetRawPayload(), TEXT("position"));

    if (ActorName.IsEmpty())
    {
        Ctx.SendError(ErrorCodes::ERR_MISSING_PARAM, TEXT("actorName is required"));
        return true;
    }

    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World)
    {
        Ctx.SendError(ErrorCodes::ERR_NO_WORLD, TEXT("No editor world available"));
        return true;
    }

    AActor* Actor = SplineHandlerHelpers::FindActorByName(World, ActorName);
    if (!Actor)
    {
        Ctx.SendError(ErrorCodes::ERR_NOT_FOUND, FString::Printf(TEXT("Actor not found: %s"), *ActorName));
        return true;
    }

    USplineComponent* SplineComp = SplineHandlerHelpers::FindSplineComponent(Actor);
    if (!SplineComp)
    {
        Ctx.SendError(ErrorCodes::ERR_NO_SPLINE, TEXT("No spline component found on actor"));
        return true;
    }

    if (PointIndex < 0 || PointIndex >= SplineComp->GetNumberOfSplinePoints())
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_INDEX, FString::Printf(TEXT("Invalid point index: %d"), PointIndex));
        return true;
    }

    SplineComp->SetLocationAtSplinePoint(PointIndex, Position, ESplineCoordinateSpace::Local, true);
    SplineComp->UpdateSpline();

    World->MarkPackageDirty();

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetNumberField(TEXT("pointIndex"), PointIndex);

    AddActorVerification(Result, Actor);

    Ctx.SendSuccess(Result);
    return true;
}

REGISTER_RPC_HANDLER("spline.set_spline_point_tangents", "spline", "Set the tangent vectors for a spline point",
    RPC_PARAMS(
        RPC_PARAM_REQ("actorName", "string", "Name of the actor with the spline"),
        RPC_PARAM_OPT("pointIndex", "integer", "Index of the point"),
        RPC_PARAM_OPT("arriveTangent", "object", "Arrive tangent vector {x, y, z}"),
        RPC_PARAM_OPT("leaveTangent", "object", "Leave tangent vector {x, y, z}; omit for a symmetric tangent (defaults to arriveTangent)")
    ))
{
    FString ActorName = Ctx.GetString(TEXT("actorName"));
    int32 PointIndex = Ctx.GetInt(TEXT("pointIndex"), 0);
    FVector ArriveTangent = SplineHandlerHelpers::GetJsonVectorField(Ctx.GetRawPayload(), TEXT("arriveTangent"));
    // An omitted leaveTangent means "same as arrive" (the symmetric case). Defaulting to
    // ArriveTangent here keeps that shorthand while still letting a caller author an explicit
    // zero leave tangent, which a ZeroVector default could not be told apart from absence.
    FVector LeaveTangent = SplineHandlerHelpers::GetJsonVectorField(Ctx.GetRawPayload(), TEXT("leaveTangent"), ArriveTangent);

    if (ActorName.IsEmpty())
    {
        Ctx.SendError(ErrorCodes::ERR_MISSING_PARAM, TEXT("actorName is required"));
        return true;
    }

    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World)
    {
        Ctx.SendError(ErrorCodes::ERR_NO_WORLD, TEXT("No editor world available"));
        return true;
    }

    AActor* Actor = SplineHandlerHelpers::FindActorByName(World, ActorName);
    if (!Actor)
    {
        Ctx.SendError(ErrorCodes::ERR_NOT_FOUND, FString::Printf(TEXT("Actor not found: %s"), *ActorName));
        return true;
    }

    USplineComponent* SplineComp = SplineHandlerHelpers::FindSplineComponent(Actor);
    if (!SplineComp)
    {
        Ctx.SendError(ErrorCodes::ERR_NO_SPLINE, TEXT("No spline component found on actor"));
        return true;
    }

    if (PointIndex < 0 || PointIndex >= SplineComp->GetNumberOfSplinePoints())
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_INDEX, FString::Printf(TEXT("Invalid point index: %d"), PointIndex));
        return true;
    }

    // A spline point carries INDEPENDENT arrive and leave tangents (FInterpCurvePoint's
    // ArriveTangent/LeaveTangent), so both are applied. CurveCustomTangent (= CIM_CurveUser) is
    // the point type that keeps authored tangents: any auto type (Curve / CurveClamped) makes
    // UpdateSpline recompute them from the neighbouring points, discarding what was set here.
    SplineComp->SetTangentsAtSplinePoint(PointIndex, ArriveTangent, LeaveTangent, ESplineCoordinateSpace::Local, false);
    SplineComp->SetSplinePointType(PointIndex, ESplinePointType::CurveCustomTangent, false);
    SplineComp->UpdateSpline();

    World->MarkPackageDirty();

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetNumberField(TEXT("pointIndex"), PointIndex);
    // Echo the applied tangents so a caller can confirm the leave tangent was not collapsed
    // onto the arrive tangent (the old single-tangent behaviour) without a separate readback.
    Result->SetObjectField(TEXT("arriveTangent"),
        JsonBuilders::BuildVectorJson(SplineComp->GetArriveTangentAtSplinePoint(PointIndex, ESplineCoordinateSpace::Local)));
    Result->SetObjectField(TEXT("leaveTangent"),
        JsonBuilders::BuildVectorJson(SplineComp->GetLeaveTangentAtSplinePoint(PointIndex, ESplineCoordinateSpace::Local)));

    AddActorVerification(Result, Actor);

    Ctx.SendSuccess(Result);
    return true;
}

REGISTER_RPC_HANDLER("spline.set_spline_point_rotation", "spline", "Set the rotation for a spline point",
    RPC_PARAMS(
        RPC_PARAM_REQ("actorName", "string", "Name of the actor with the spline"),
        RPC_PARAM_OPT("pointIndex", "integer", "Index of the point"),
        RPC_PARAM_OPT("pointRotation", "object", "Rotation {pitch, yaw, roll}")
    ))
{
    FString ActorName = Ctx.GetString(TEXT("actorName"));
    int32 PointIndex = Ctx.GetInt(TEXT("pointIndex"), 0);
    FRotator Rotation = SplineHandlerHelpers::GetJsonRotatorField(Ctx.GetRawPayload(), TEXT("pointRotation"));

    if (ActorName.IsEmpty())
    {
        Ctx.SendError(ErrorCodes::ERR_MISSING_PARAM, TEXT("actorName is required"));
        return true;
    }

    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World)
    {
        Ctx.SendError(ErrorCodes::ERR_NO_WORLD, TEXT("No editor world available"));
        return true;
    }

    AActor* Actor = SplineHandlerHelpers::FindActorByName(World, ActorName);
    if (!Actor)
    {
        Ctx.SendError(ErrorCodes::ERR_NOT_FOUND, FString::Printf(TEXT("Actor not found: %s"), *ActorName));
        return true;
    }

    USplineComponent* SplineComp = SplineHandlerHelpers::FindSplineComponent(Actor);
    if (!SplineComp)
    {
        Ctx.SendError(ErrorCodes::ERR_NO_SPLINE, TEXT("No spline component found on actor"));
        return true;
    }

    if (PointIndex < 0 || PointIndex >= SplineComp->GetNumberOfSplinePoints())
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_INDEX, FString::Printf(TEXT("Invalid point index: %d"), PointIndex));
        return true;
    }

    SplineComp->SetRotationAtSplinePoint(PointIndex, Rotation, ESplineCoordinateSpace::Local, true);
    SplineComp->UpdateSpline();

    World->MarkPackageDirty();

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetNumberField(TEXT("pointIndex"), PointIndex);

    AddActorVerification(Result, Actor);

    Ctx.SendSuccess(Result);
    return true;
}

REGISTER_RPC_HANDLER("spline.set_spline_point_scale", "spline",
    "Set the scale for a spline point. Refused with DERIVED_PROPERTY on spline types "
    "whose point scale the engine re-derives from other state (water splines: Scale.X is "
    "river width, Scale.Y is depth, both owned by the spline metadata) - such a write "
    "reads back, saves, and then reverts on the next level load. The error names the "
    "authoritative verb to use instead.",
    RPC_PARAMS(
        RPC_PARAM_REQ("actorName", "string", "Name of the actor with the spline"),
        RPC_PARAM_OPT("pointIndex", "integer", "Index of the point"),
        RPC_PARAM_OPT("pointScale", "object", "Scale vector {x, y, z}")
    ))
{
    FString ActorName = Ctx.GetString(TEXT("actorName"));
    int32 PointIndex = Ctx.GetInt(TEXT("pointIndex"), 0);
    FVector Scale = SplineHandlerHelpers::GetJsonVectorField(Ctx.GetRawPayload(), TEXT("pointScale"), FVector::OneVector);

    if (ActorName.IsEmpty())
    {
        Ctx.SendError(ErrorCodes::ERR_MISSING_PARAM, TEXT("actorName is required"));
        return true;
    }

    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World)
    {
        Ctx.SendError(ErrorCodes::ERR_NO_WORLD, TEXT("No editor world available"));
        return true;
    }

    AActor* Actor = SplineHandlerHelpers::FindActorByName(World, ActorName);
    if (!Actor)
    {
        Ctx.SendError(ErrorCodes::ERR_NOT_FOUND, FString::Printf(TEXT("Actor not found: %s"), *ActorName));
        return true;
    }

    USplineComponent* SplineComp = SplineHandlerHelpers::FindSplineComponent(Actor);
    if (!SplineComp)
    {
        Ctx.SendError(ErrorCodes::ERR_NO_SPLINE, TEXT("No spline component found on actor"));
        return true;
    }

    if (PointIndex < 0 || PointIndex >= SplineComp->GetNumberOfSplinePoints())
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_INDEX, FString::Printf(TEXT("Invalid point index: %d"), PointIndex));
        return true;
    }

    // Refuse before writing when the engine owns this value. The gate is the engine's
    // own declaration, not a hand-maintained type list, so a spline type that starts
    // deriving its scale in a later engine version is covered without a code change here
    // (docs/rpc-design.md §2: prefer a structural guarantee over discipline).
    if (!SplineComp->AllowsSplinePointScaleEditing())
    {
        FString DerivedFrom;
        FString AuthoritativeVerb;
        FString Explanation;
        SplineHandlerHelpers::DescribeDerivedSplineScale(
            SplineComp, DerivedFrom, AuthoritativeVerb, Explanation);

        TSharedPtr<FJsonObject> ErrorData = MakeShared<FJsonObject>();
        ErrorData->SetStringField(TEXT("actorName"), ActorName);
        ErrorData->SetStringField(TEXT("componentClass"), SplineComp->GetClass()->GetName());
        ErrorData->SetNumberField(TEXT("pointIndex"), PointIndex);
        PinWright::DerivedState::AddDerivedWriteReport(
            ErrorData,
            TEXT("spline point Scale"),
            DerivedFrom,
            AuthoritativeVerb,
            Explanation);

        Ctx.SendError(ErrorCodes::ERR_DERIVED_PROPERTY, Explanation, ErrorData);
        return true;
    }

    SplineComp->SetScaleAtSplinePoint(PointIndex, Scale, true);
    SplineComp->UpdateSpline();

    World->MarkPackageDirty();

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetNumberField(TEXT("pointIndex"), PointIndex);
    // Read the scale back off the component rather than echoing the request, so a write
    // the engine clamped or ignored cannot be reported as the value asked for.
    Result->SetObjectField(TEXT("pointScale"),
        JsonBuilders::BuildVectorJson(SplineComp->GetScaleAtSplinePoint(PointIndex)));

    AddActorVerification(Result, Actor);

    Ctx.SendSuccess(Result);
    return true;
}

REGISTER_RPC_HANDLER("spline.set_spline_type", "spline", "Set the spline point type for one or all points",
    RPC_PARAMS(
        RPC_PARAM_REQ("actorName", "string", "Name of the actor with the spline"),
        RPC_PARAM_OPT("splineType", "string", "Spline point type (Curve, Linear, Constant, etc.)"),
        RPC_PARAM_OPT("pointIndex", "integer", "Index of specific point (-1 for all)")
    ))
{
    FString ActorName = Ctx.GetString(TEXT("actorName"));
    FString SplineType = Ctx.GetString(TEXT("splineType"), TEXT("Curve"));
    int32 PointIndex = Ctx.GetInt(TEXT("pointIndex"), -1);

    if (ActorName.IsEmpty())
    {
        Ctx.SendError(ErrorCodes::ERR_MISSING_PARAM, TEXT("actorName is required"));
        return true;
    }

    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World)
    {
        Ctx.SendError(ErrorCodes::ERR_NO_WORLD, TEXT("No editor world available"));
        return true;
    }

    AActor* Actor = SplineHandlerHelpers::FindActorByName(World, ActorName);
    if (!Actor)
    {
        Ctx.SendError(ErrorCodes::ERR_NOT_FOUND, FString::Printf(TEXT("Actor not found: %s"), *ActorName));
        return true;
    }

    USplineComponent* SplineComp = SplineHandlerHelpers::FindSplineComponent(Actor);
    if (!SplineComp)
    {
        Ctx.SendError(ErrorCodes::ERR_NO_SPLINE, TEXT("No spline component found on actor"));
        return true;
    }

    ESplinePointType::Type PointType = SplineHelpers::ParseSplinePointType(SplineType);

    if (PointIndex >= 0)
    {
        if (PointIndex >= SplineComp->GetNumberOfSplinePoints())
        {
            Ctx.SendError(ErrorCodes::ERR_INVALID_INDEX, FString::Printf(TEXT("Invalid point index: %d"), PointIndex));
            return true;
        }
        SplineComp->SetSplinePointType(PointIndex, PointType, true);
    }
    else
    {
        for (int32 i = 0; i < SplineComp->GetNumberOfSplinePoints(); i++)
        {
            SplineComp->SetSplinePointType(i, PointType, false);
        }
    }

    SplineComp->UpdateSpline();
    World->MarkPackageDirty();

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("splineType"), SplineType);
    Result->SetNumberField(TEXT("pointsAffected"), PointIndex >= 0 ? 1 : SplineComp->GetNumberOfSplinePoints());

    AddActorVerification(Result, Actor);

    Ctx.SendSuccess(Result);
    return true;
}

// ============================================================================
// Spline Mesh Handlers
// ============================================================================

REGISTER_RPC_HANDLER("spline.set_spline_mesh_asset", "spline", "Set the static mesh on a SplineMeshComponent",
    RPC_PARAMS(
        RPC_PARAM_REQ("actorName", "string", "Name of the actor"),
        RPC_PARAM_REQ("meshPath", "string", "Path to the static mesh asset"),
        RPC_PARAM_OPT("componentName", "string", "Name of the SplineMeshComponent")
    ))
{
    FString ActorName = Ctx.GetString(TEXT("actorName"));
    FString ComponentName = Ctx.GetString(TEXT("componentName"));
    FString MeshPath = Ctx.GetString(TEXT("meshPath"));

    if (ActorName.IsEmpty() || MeshPath.IsEmpty())
    {
        Ctx.SendError(ErrorCodes::ERR_MISSING_PARAM, TEXT("actorName and meshPath are required"));
        return true;
    }

    // SECURITY: Validate meshPath
    FString SafeMeshPath = SanitizeProjectRelativePath(MeshPath);
    if (SafeMeshPath.IsEmpty())
    {
        Ctx.SendError(ErrorCodes::ERR_SECURITY_VIOLATION,
            FString::Printf(TEXT("Invalid or unsafe meshPath: %s. Path must be relative to project (e.g., /Game/...)"), *MeshPath));
        return true;
    }

    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World)
    {
        Ctx.SendError(ErrorCodes::ERR_NO_WORLD, TEXT("No editor world available"));
        return true;
    }

    AActor* Actor = SplineHandlerHelpers::FindActorByName(World, ActorName);
    if (!Actor)
    {
        Ctx.SendError(ErrorCodes::ERR_NOT_FOUND, FString::Printf(TEXT("Actor not found: %s"), *ActorName));
        return true;
    }

    TArray<USplineMeshComponent*> MeshComponents;
    Actor->GetComponents<USplineMeshComponent>(MeshComponents);

    USplineMeshComponent* TargetComp = nullptr;
    if (!ComponentName.IsEmpty())
    {
        for (USplineMeshComponent* Comp : MeshComponents)
        {
            if (Comp && Comp->GetName() == ComponentName)
            {
                TargetComp = Comp;
                break;
            }
        }
    }
    else if (MeshComponents.Num() > 0)
    {
        TargetComp = MeshComponents[0];
    }

    if (!TargetComp)
    {
        Ctx.SendError(ErrorCodes::ERR_NO_COMPONENT, TEXT("No SplineMeshComponent found on actor"));
        return true;
    }

    UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, *SafeMeshPath);
    if (!Mesh)
    {
        Ctx.SendError(ErrorCodes::ERR_MESH_NOT_FOUND, FString::Printf(TEXT("Mesh not found: %s"), *SafeMeshPath));
        return true;
    }

    TargetComp->SetStaticMesh(Mesh);
    World->MarkPackageDirty();

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("actorName"), ActorName);
    Result->SetStringField(TEXT("meshPath"), SafeMeshPath);

    AddActorVerification(Result, Actor);

    Ctx.SendSuccess(Result);
    return true;
}

REGISTER_RPC_HANDLER("spline.configure_spline_mesh_axis", "spline", "Set the forward axis on a SplineMeshComponent",
    RPC_PARAMS(
        RPC_PARAM_REQ("actorName", "string", "Name of the actor"),
        RPC_PARAM_OPT("componentName", "string", "Name of the SplineMeshComponent"),
        RPC_PARAM_OPT("forwardAxis", "string", "Forward axis (X, Y, Z)")
    ))
{
    FString ActorName = Ctx.GetString(TEXT("actorName"));
    FString ComponentName = Ctx.GetString(TEXT("componentName"));
    FString ForwardAxis = Ctx.GetString(TEXT("forwardAxis"), TEXT("X"));

    if (ActorName.IsEmpty())
    {
        Ctx.SendError(ErrorCodes::ERR_MISSING_PARAM, TEXT("actorName is required"));
        return true;
    }

    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World)
    {
        Ctx.SendError(ErrorCodes::ERR_NO_WORLD, TEXT("No editor world available"));
        return true;
    }

    AActor* Actor = SplineHandlerHelpers::FindActorByName(World, ActorName);
    if (!Actor)
    {
        Ctx.SendError(ErrorCodes::ERR_NOT_FOUND, FString::Printf(TEXT("Actor not found: %s"), *ActorName));
        return true;
    }

    TArray<USplineMeshComponent*> MeshComponents;
    Actor->GetComponents<USplineMeshComponent>(MeshComponents);

    USplineMeshComponent* TargetComp = nullptr;
    if (!ComponentName.IsEmpty())
    {
        for (USplineMeshComponent* Comp : MeshComponents)
        {
            if (Comp && Comp->GetName() == ComponentName) { TargetComp = Comp; break; }
        }
    }
    else if (MeshComponents.Num() > 0)
    {
        TargetComp = MeshComponents[0];
    }

    if (!TargetComp)
    {
        Ctx.SendError(ErrorCodes::ERR_NO_COMPONENT, TEXT("No SplineMeshComponent found on actor"));
        return true;
    }

    ESplineMeshAxis::Type Axis = ESplineMeshAxis::X;
    if (ForwardAxis == TEXT("Y")) Axis = ESplineMeshAxis::Y;
    else if (ForwardAxis == TEXT("Z")) Axis = ESplineMeshAxis::Z;

    TargetComp->SetForwardAxis(Axis);
    World->MarkPackageDirty();

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("forwardAxis"), ForwardAxis);

    AddActorVerification(Result, Actor);

    Ctx.SendSuccess(Result);
    return true;
}

REGISTER_RPC_HANDLER("spline.set_spline_mesh_material", "spline", "Set a material on a SplineMeshComponent",
    RPC_PARAMS(
        RPC_PARAM_REQ("actorName", "string", "Name of the actor"),
        RPC_PARAM_REQ("materialPath", "string", "Path to the material asset"),
        RPC_PARAM_OPT("componentName", "string", "Name of the SplineMeshComponent"),
        RPC_PARAM_OPT("materialIndex", "integer", "Material slot index")
    ))
{
    FString ActorName = Ctx.GetString(TEXT("actorName"));
    FString ComponentName = Ctx.GetString(TEXT("componentName"));
    FString MaterialPath = Ctx.GetString(TEXT("materialPath"));
    int32 MaterialIndex = Ctx.GetInt(TEXT("materialIndex"), 0);

    if (ActorName.IsEmpty() || MaterialPath.IsEmpty())
    {
        Ctx.SendError(ErrorCodes::ERR_MISSING_PARAM, TEXT("actorName and materialPath are required"));
        return true;
    }

    // SECURITY: Validate materialPath
    FString SafeMaterialPath = SanitizeProjectRelativePath(MaterialPath);
    if (SafeMaterialPath.IsEmpty())
    {
        Ctx.SendError(ErrorCodes::ERR_SECURITY_VIOLATION,
            FString::Printf(TEXT("Invalid or unsafe materialPath: %s. Path must be relative to project (e.g., /Game/...)"), *MaterialPath));
        return true;
    }

    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World)
    {
        Ctx.SendError(ErrorCodes::ERR_NO_WORLD, TEXT("No editor world available"));
        return true;
    }

    AActor* Actor = SplineHandlerHelpers::FindActorByName(World, ActorName);
    if (!Actor)
    {
        Ctx.SendError(ErrorCodes::ERR_NOT_FOUND, FString::Printf(TEXT("Actor not found: %s"), *ActorName));
        return true;
    }

    TArray<USplineMeshComponent*> MeshComponents;
    Actor->GetComponents<USplineMeshComponent>(MeshComponents);

    USplineMeshComponent* TargetComp = nullptr;
    if (!ComponentName.IsEmpty())
    {
        for (USplineMeshComponent* Comp : MeshComponents)
        {
            if (Comp && Comp->GetName() == ComponentName) { TargetComp = Comp; break; }
        }
    }
    else if (MeshComponents.Num() > 0)
    {
        TargetComp = MeshComponents[0];
    }

    if (!TargetComp)
    {
        Ctx.SendError(ErrorCodes::ERR_NO_COMPONENT, TEXT("No SplineMeshComponent found on actor"));
        return true;
    }

    UMaterialInterface* Material = LoadObject<UMaterialInterface>(nullptr, *SafeMaterialPath);
    if (!Material)
    {
        Ctx.SendError(ErrorCodes::ERR_MATERIAL_NOT_FOUND, FString::Printf(TEXT("Material not found: %s"), *SafeMaterialPath));
        return true;
    }

    TargetComp->SetMaterial(MaterialIndex, Material);
    World->MarkPackageDirty();

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("materialPath"), SafeMaterialPath);
    Result->SetNumberField(TEXT("materialIndex"), MaterialIndex);

    AddActorVerification(Result, Actor);
    AddComponentVerification(Result, TargetComp);

    Ctx.SendSuccess(Result);
    return true;
}

REGISTER_RPC_HANDLER("spline.create_spline_mesh_actor", "spline", "Spawn a new actor with a SplineMeshComponent",
    RPC_PARAMS(
        RPC_PARAM_OPT("actorName", "string", "Name for the new actor"),
        RPC_PARAM_OPT("componentName", "string", "Name for the SplineMeshComponent"),
        RPC_PARAM_OPT("meshPath", "string", "Path to a static mesh asset"),
        RPC_PARAM_OPT("forwardAxis", "string", "Forward axis (X, Y, Z)"),
        RPC_PARAM_OPT("location", "object", "Spawn location {x, y, z}"),
        RPC_PARAM_OPT("rotation", "object", "Spawn rotation {pitch, yaw, roll}")
    ))
{
    FString ActorName = Ctx.GetString(TEXT("actorName"), TEXT("SplineMeshActor"));
    FString ComponentName = Ctx.GetString(TEXT("componentName"), TEXT("SplineMesh"));
    FString MeshPath = Ctx.GetString(TEXT("meshPath"));
    FString ForwardAxis = Ctx.GetString(TEXT("forwardAxis"), TEXT("X"));
    FVector Location = SplineHandlerHelpers::GetJsonVectorField(Ctx.GetRawPayload(), TEXT("location"));
    FRotator Rotation = SplineHandlerHelpers::GetJsonRotatorField(Ctx.GetRawPayload(), TEXT("rotation"));

    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World)
    {
        Ctx.SendError(ErrorCodes::ERR_NO_WORLD, TEXT("No editor world available"));
        return true;
    }

    // SECURITY: Validate meshPath if provided
    FString SafeMeshPath;
    if (!MeshPath.IsEmpty())
    {
        SafeMeshPath = SanitizeProjectRelativePath(MeshPath);
        if (SafeMeshPath.IsEmpty())
        {
            Ctx.SendError(ErrorCodes::ERR_SECURITY_VIOLATION,
                FString::Printf(TEXT("Invalid or unsafe meshPath: %s. Path must be relative to project (e.g., /Game/...)"), *MeshPath));
            return true;
        }
    }

    FActorSpawnParameters SpawnParams;
    SpawnParams.Name = *ActorName;
    SpawnParams.NameMode = FActorSpawnParameters::ESpawnActorNameMode::Requested;
    SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

    AActor* NewActor = World->SpawnActor<AActor>(AActor::StaticClass(), Location, Rotation, SpawnParams);
    if (!NewActor)
    {
        Ctx.SendError(ErrorCodes::ERR_SPAWN_FAILED, TEXT("Failed to spawn spline mesh actor"));
        return true;
    }

    NewActor->SetActorLabel(*ActorName);

    USplineMeshComponent* SplineMeshComp = NewObject<USplineMeshComponent>(NewActor, *ComponentName);
    if (!SplineMeshComp)
    {
        NewActor->Destroy();
        Ctx.SendError(ErrorCodes::ERR_COMPONENT_FAILED, TEXT("Failed to create SplineMeshComponent"));
        return true;
    }

    SplineMeshComp->RegisterComponent();
    NewActor->AddInstanceComponent(SplineMeshComp);
    NewActor->SetRootComponent(SplineMeshComp);

    if (!SafeMeshPath.IsEmpty())
    {
        UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, *SafeMeshPath);
        if (!Mesh)
        {
            NewActor->Destroy();
            Ctx.SendError(ErrorCodes::ERR_MESH_NOT_FOUND, FString::Printf(TEXT("Mesh not found: %s"), *SafeMeshPath));
            return true;
        }
        SplineMeshComp->SetStaticMesh(Mesh);
    }

    if (SplineMeshComp->GetMaterial(0) == nullptr)
    {
        UMaterialInterface* FallbackMaterial = McpLoadMaterialWithFallback(TEXT(""), true);
        if (FallbackMaterial)
        {
            SplineMeshComp->SetMaterial(0, FallbackMaterial);
        }
    }

    ESplineMeshAxis::Type Axis = ESplineMeshAxis::X;
    if (ForwardAxis == TEXT("Y")) Axis = ESplineMeshAxis::Y;
    else if (ForwardAxis == TEXT("Z")) Axis = ESplineMeshAxis::Z;
    SplineMeshComp->SetForwardAxis(Axis);

    SplineMeshComp->SetStartAndEnd(FVector::ZeroVector, FVector(100, 0, 0),
                                    FVector(500, 0, 0), FVector(-100, 0, 0));

    World->MarkPackageDirty();

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("componentName"), ComponentName);

    // AddActorVerification owns actorName (= label) and actorPath (= the unique
    // object path); see Utils/AssetUtils.cpp. Writing those before it is dead.
    AddActorVerification(Result, NewActor);
    AddComponentVerification(Result, SplineMeshComp);

    // After AddActorVerification, surface the collision/dedup signal (the unique
    // actorObjectName + nameWasDeduplicated flag) the verification helper omits.
    AddActorNameDeduplicationSignal(Result, NewActor, ActorName);

    Ctx.SendSuccess(Result);
    return true;
}

// ============================================================================
// Mesh Scattering Handlers
// ============================================================================

REGISTER_RPC_HANDLER("spline.scatter_meshes_along_spline", "spline", "Scatter static meshes along a spline. Creates floor(length/spacing)+1 SEPARATE UStaticMeshComponents attached to the spline component (NOT an InstancedStaticMeshComponent). spline.get_splines_info reports spline geometry only and will not list them; verify from meshComponents/meshesCreated here, or with actor.get_components.",
    RPC_PARAMS(
        RPC_PARAM_REQ("actorName", "string", "Name of the actor with the spline"),
        RPC_PARAM_REQ("meshPath", "string", "Path to the static mesh to scatter"),
        RPC_PARAM_OPT("spacing", "number", "Distance between mesh instances"),
        RPC_PARAM_OPT("alignToSpline", "boolean", "Whether to align meshes to spline direction")
    ))
{
    FString ActorName = Ctx.GetString(TEXT("actorName"));
    FString MeshPath = Ctx.GetString(TEXT("meshPath"));
    double Spacing = Ctx.GetNumber(TEXT("spacing"), 100.0);
    bool bAlignToSpline = Ctx.GetBool(TEXT("alignToSpline"), true);

    FString SafeMeshPath = SanitizeProjectRelativePath(MeshPath);
    if (SafeMeshPath.IsEmpty())
    {
        Ctx.SendError(ErrorCodes::ERR_SECURITY_VIOLATION,
            FString::Printf(TEXT("Invalid or unsafe meshPath: %s. Path must be relative to project (e.g., /Game/...)"), *MeshPath));
        return true;
    }

    if (Spacing <= 0.0)
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_PARAM, TEXT("spacing must be greater than 0"));
        return true;
    }

    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World)
    {
        Ctx.SendError(ErrorCodes::ERR_NO_WORLD, TEXT("No editor world available"));
        return true;
    }

    AActor* Actor = SplineHandlerHelpers::FindActorByName(World, ActorName);
    if (!Actor)
    {
        Ctx.SendError(ErrorCodes::ERR_NOT_FOUND, FString::Printf(TEXT("Actor not found: %s"), *ActorName));
        return true;
    }

    USplineComponent* SplineComp = SplineHandlerHelpers::FindSplineComponent(Actor);
    if (!SplineComp)
    {
        Ctx.SendError(ErrorCodes::ERR_NO_SPLINE, TEXT("No spline component found on actor"));
        return true;
    }

    UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, *SafeMeshPath);
    if (!Mesh)
    {
        Ctx.SendError(ErrorCodes::ERR_MESH_NOT_FOUND, FString::Printf(TEXT("Mesh not found: %s"), *SafeMeshPath));
        return true;
    }

    float SplineLength = SplineComp->GetSplineLength();
    int32 MeshCount = FMath::FloorToInt(SplineLength / Spacing);

    TArray<FString> CreatedMeshes;

    for (int32 i = 0; i <= MeshCount; i++)
    {
        float Distance = i * Spacing;
        FVector SpawnLocation = SplineComp->GetLocationAtDistanceAlongSpline(Distance, ESplineCoordinateSpace::World);
        FRotator SpawnRotation = bAlignToSpline
            ? SplineComp->GetRotationAtDistanceAlongSpline(Distance, ESplineCoordinateSpace::World)
            : FRotator::ZeroRotator;

        UStaticMeshComponent* MeshComp = NewObject<UStaticMeshComponent>(Actor);
        if (MeshComp)
        {
            MeshComp->SetStaticMesh(Mesh);
            MeshComp->SetWorldLocationAndRotation(SpawnLocation, SpawnRotation);
            MeshComp->RegisterComponent();
            Actor->AddInstanceComponent(MeshComp);
            MeshComp->AttachToComponent(SplineComp, FAttachmentTransformRules::KeepWorldTransform);
            CreatedMeshes.Add(MeshComp->GetName());
        }
    }

    World->MarkPackageDirty();

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetNumberField(TEXT("meshesCreated"), CreatedMeshes.Num());
    Result->SetNumberField(TEXT("splineLength"), SplineLength);
    Result->SetNumberField(TEXT("spacing"), Spacing);
    // The created component names were already collected and then thrown away, leaving
    // the caller with a bare count and no handle - and spline.get_splines_info cannot
    // see these components at all. Return the identities, capped with the same
    // truncated vocabulary the bounded list handlers use so a long spline cannot blow
    // the response budget.
    {
        constexpr int32 MaxMeshComponentDetail = 64;
        TArray<TSharedPtr<FJsonValue>> MeshComponentArray;
        for (const FString& ComponentName : CreatedMeshes)
        {
            if (MeshComponentArray.Num() >= MaxMeshComponentDetail)
            {
                break;
            }
            MeshComponentArray.Add(MakeShared<FJsonValueString>(ComponentName));
        }
        Result->SetArrayField(TEXT("meshComponents"), MeshComponentArray);
        Result->SetBoolField(TEXT("meshComponentsTruncated"),
            MeshComponentArray.Num() < CreatedMeshes.Num());
    }

    AddActorVerification(Result, Actor);

    Ctx.SendSuccess(Result);
    return true;
}

// ============================================================================
// Utility Handlers
// ============================================================================

REGISTER_RPC_HANDLER("spline.get_splines_info", "spline", "Get information about spline components on actors in the world",
    RPC_PARAMS(
        RPC_PARAM_OPT("actorName", "string", "Specific actor name (omit to list all spline actors)")
    ))
{
    FString ActorName = Ctx.GetString(TEXT("actorName"));

    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World)
    {
        Ctx.SendError(ErrorCodes::ERR_NO_WORLD, TEXT("No editor world available"));
        return true;
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();

    if (!ActorName.IsEmpty())
    {
        AActor* Actor = SplineHandlerHelpers::FindActorByName(World, ActorName);
        if (!Actor)
        {
            Ctx.SendError(ErrorCodes::ERR_NOT_FOUND, FString::Printf(TEXT("Actor not found: %s"), *ActorName));
            return true;
        }

        USplineComponent* SplineComp = SplineHandlerHelpers::FindSplineComponent(Actor);
        if (!SplineComp)
        {
            // No USplineComponent — fall back to a USplineMeshComponent (created by
            // spline.create_spline_mesh_actor). It is not a USplineComponent subclass,
            // so report its spline-mesh state rather than the misleading NO_SPLINE.
            if (USplineMeshComponent* MeshComp = SplineHandlerHelpers::FindSplineMeshComponent(Actor))
            {
                Result->SetStringField(TEXT("actorName"), ActorName);
                Result->SetBoolField(TEXT("isSplineMesh"), true);
                SplineHandlerHelpers::FillSplineMeshFields(Result, MeshComp);
                Ctx.SendSuccess(Result);
                return true;
            }

            Ctx.SendError(ErrorCodes::ERR_NO_SPLINE, TEXT("No spline component found on actor"));
            return true;
        }

        Result->SetStringField(TEXT("actorName"), ActorName);
        Result->SetNumberField(TEXT("pointCount"), SplineComp->GetNumberOfSplinePoints());
        Result->SetNumberField(TEXT("splineLength"), SplineComp->GetSplineLength());
        Result->SetBoolField(TEXT("closedLoop"), SplineComp->IsClosedLoop());

        TArray<TSharedPtr<FJsonValue>> PointsArray;
        for (int32 i = 0; i < SplineComp->GetNumberOfSplinePoints(); i++)
        {
            TSharedPtr<FJsonObject> PointObj = MakeShared<FJsonObject>();
            FVector Loc = SplineComp->GetLocationAtSplinePoint(i, ESplineCoordinateSpace::Local);

            PointObj->SetNumberField(TEXT("index"), i);

            TSharedPtr<FJsonObject> LocObj = MakeShared<FJsonObject>();
            LocObj->SetNumberField(TEXT("x"), Loc.X);
            LocObj->SetNumberField(TEXT("y"), Loc.Y);
            LocObj->SetNumberField(TEXT("z"), Loc.Z);
            PointObj->SetObjectField(TEXT("location"), LocObj);

            PointObj->SetStringField(TEXT("type"), SplineHelpers::SplinePointTypeToString(SplineComp->GetSplinePointType(i)));

            PointsArray.Add(MakeShared<FJsonValueObject>(PointObj));
        }
        Result->SetArrayField(TEXT("points"), PointsArray);
    }
    else
    {
        TArray<TSharedPtr<FJsonValue>> SplinesArray;
        for (TActorIterator<AActor> It(World); It; ++It)
        {
            AActor* Actor = *It;
            TArray<USplineComponent*> SplineComponents;
            Actor->GetComponents<USplineComponent>(SplineComponents);

            if (SplineComponents.Num() > 0)
            {
                TSharedPtr<FJsonObject> ActorObj = MakeShared<FJsonObject>();
                ActorObj->SetStringField(TEXT("actorName"), Actor->GetActorLabel());
                ActorObj->SetNumberField(TEXT("splineComponentCount"), SplineComponents.Num());

                if (SplineComponents[0])
                {
                    ActorObj->SetNumberField(TEXT("pointCount"), SplineComponents[0]->GetNumberOfSplinePoints());
                    ActorObj->SetNumberField(TEXT("splineLength"), SplineComponents[0]->GetSplineLength());
                }

                SplinesArray.Add(MakeShared<FJsonValueObject>(ActorObj));
            }
            else
            {
                // Spline-mesh-only actors (no USplineComponent) would otherwise be
                // silently dropped from the listing — include them with their state.
                TArray<USplineMeshComponent*> MeshComponents;
                Actor->GetComponents<USplineMeshComponent>(MeshComponents);
                if (MeshComponents.Num() > 0 && MeshComponents[0])
                {
                    TSharedPtr<FJsonObject> ActorObj = MakeShared<FJsonObject>();
                    ActorObj->SetStringField(TEXT("actorName"), Actor->GetActorLabel());
                    ActorObj->SetBoolField(TEXT("isSplineMesh"), true);
                    ActorObj->SetNumberField(TEXT("splineMeshComponentCount"), MeshComponents.Num());
                    SplineHandlerHelpers::FillSplineMeshFields(ActorObj, MeshComponents[0]);
                    SplinesArray.Add(MakeShared<FJsonValueObject>(ActorObj));
                }
            }
        }
        Result->SetArrayField(TEXT("splines"), SplinesArray);
        Result->SetNumberField(TEXT("totalSplineActors"), SplinesArray.Num());
    }

    Ctx.SendSuccess(Result);
    return true;
}
