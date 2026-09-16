// Copyright (c) 2026 Alexander Penkin. MIT License.

// ReflectionCaptureHandler.cpp
// environment.spawn_reflection_capture(shape: "Sphere"|"Box", ...)
//
// Shape-dispatched spawn of ASphereReflectionCapture / ABoxReflectionCapture.
// Shape-specific UPROPERTYs (InfluenceRadius on sphere, BoxTransitionDistance
// on box) resolve through FindPropertyCI on the derived component class, so
// the apply loop has no shape branch.

#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/ParamSpec.h"
#include "Handlers/Environment/EnvironmentSpawnHelpers.h"
#include "PinWrightHelpers.h"
#include "Utils/AssetUtils.h"
#include "Utils/PropertyUtils.h"
#include "Dom/JsonObject.h"

#include "Components/ReflectionCaptureComponent.h"
#include "Engine/BoxReflectionCapture.h"
#include "Engine/SphereReflectionCapture.h"
#include "Handlers/Environment/EnvironmentDirtyUtils.h"

// ---- environment.spawn_reflection_capture ----
REGISTER_RPC_HANDLER("environment.spawn_reflection_capture", "environment", "Spawn a Sphere or Box reflection capture actor and apply properties to its UReflectionCaptureComponent",
    RPC_PARAMS(
        RPC_PARAM_REQ("shape", "string", "Capture shape: 'Sphere' or 'Box'"),
        RPC_PARAM_OPT("name", "string", "Label for the spawned actor"),
        RPC_PARAM_OPT("location", "object", "Location {x, y, z}"),
        RPC_PARAM_OPT("rotation", "object", "Rotation {pitch, yaw, roll}"),
        RPC_PARAM_OPT("properties", "object", "Component properties (Brightness, InfluenceRadius for Sphere, BoxTransitionDistance for Box, etc.)")
    ))
{
    auto* Payload = Ctx.GetRawPayload().Get();

    const FString Shape = Ctx.GetString(TEXT("shape"));
    if (Shape.IsEmpty())
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("shape parameter is required ('Sphere' or 'Box')"));
        return true;
    }

    UClass* CaptureClass = nullptr;
    const FString LowerShape = Shape.ToLower();
    if (LowerShape == TEXT("sphere"))
    {
        CaptureClass = ASphereReflectionCapture::StaticClass();
    }
    else if (LowerShape == TEXT("box"))
    {
        CaptureClass = ABoxReflectionCapture::StaticClass();
    }
    else
    {
        Ctx.SendError(TEXT("INVALID_SHAPE"),
            FString::Printf(TEXT("Invalid shape: %s. Must be 'Sphere' or 'Box'."), *Shape));
        return true;
    }

    FVector Location;
    FRotator Rotation;
    ReadLocationRotationFromPayload(*Payload, Location, Rotation);

    const FString Name = Ctx.GetString(TEXT("name"));
    AActor* NewActor = SpawnActorInActiveWorld<AActor>(CaptureClass, Location, Rotation, Name);
    if (!NewActor)
    {
        Ctx.SendError(TEXT("SPAWN_FAILED"), TEXT("Failed to spawn reflection capture actor"));
        return true;
    }

    // SpawnActorInActiveWorld's unattended fork is a bare World->SpawnActor, which dirties
    // nothing without a transaction (LevelActor.cpp:735-739). The optional `name` gave an
    // accidental Actor-package dirty via SetActorLabel and never touched the level; with
    // `name` omitted the capture was silently dropped by level.save. Pre-mutation for the
    // property apply below.
    PinWright::MarkLevelActorSpawned(NewActor);

    // Both ASphereReflectionCapture and ABoxReflectionCapture expose typed
    // subclasses of UReflectionCaptureComponent; FindComponentByClass on the
    // base class resolves the derived component for property reflection.
    UReflectionCaptureComponent* Comp =
        NewActor->FindComponentByClass<UReflectionCaptureComponent>();
    if (!Comp)
    {
        Ctx.SendError(TEXT("COMPONENT_NOT_FOUND"),
            TEXT("Spawned actor missing UReflectionCaptureComponent"));
        return true;
    }

    TArray<TSharedPtr<FJsonValue>> RejectedProps;
    const TSharedPtr<FJsonObject>* Props = nullptr;
    Payload->TryGetObjectField(TEXT("properties"), Props);
    if (ApplyPropertiesJsonToComponent(Comp, Props ? *Props : TSharedPtr<FJsonObject>(), RejectedProps) > 0)
    {
        Comp->MarkRenderStateDirty();
    }

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetBoolField(TEXT("success"), true);
    Resp->SetStringField(TEXT("shape"), Shape);
    AddActorVerification(Resp, NewActor);
    AddChainableActorFields(Resp, NewActor);
    AddComponentVerification(Resp, Comp);
    if (RejectedProps.Num() > 0)
    {
        Resp->SetArrayField(TEXT("rejected"), RejectedProps);
    }
    Ctx.SendSuccess(Resp);
    return true;
}
