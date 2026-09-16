// Copyright (c) 2026 Alexander Penkin. MIT License.

// GeometryTarget.cpp - see GeometryTarget.h for why this is one named namespace rather than
// thirteen anonymous-namespace copies.
#include "Handlers/Geometry/GeometryTarget.h"

#include "Handlers/ErrorCodes.h"
#include "Handlers/Actor/ActorNameParamUtils.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/Geometry/GeometryUtils.h"

#include "Components/DynamicMeshComponent.h"
#include "DynamicMeshActor.h"
#include "Editor.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Subsystems/EditorActorSubsystem.h"
#include "UDynamicMesh.h"

namespace GeometryTarget
{

McpActorUtils::FActorResolution ResolveMeshActor(UWorld* World, const FString& Identifier)
{
    if (!IsValid(World) || Identifier.IsEmpty())
    {
        return McpActorUtils::FActorResolution();
    }

    // Check the unfiltered exact-identity resolver first only to preserve an explicitly
    // requested non-mesh object path/name. If the caller named a non-mesh actor by exact
    // identity, do not reinterpret that same string as a DynamicMeshActor's display label;
    // otherwise a class-filtered pass would silently act on the wrong actor. The fallback is
    // also exact-identity-only: geometry deliberately has no label-substring tier.
    const McpActorUtils::FActorResolution AnyResolution =
        McpActorUtils::ResolveActorFiltered(World, Identifier,
            [](AActor*) { return true; }, McpActorUtils::EActorResolvePolicy::ExactIdentity);
    if ((AnyResolution.IsResolved() || AnyResolution.IsAmbiguous()) &&
        (AnyResolution.MatchedBy == McpActorUtils::EActorMatchKind::ObjectPath ||
            AnyResolution.MatchedBy == McpActorUtils::EActorMatchKind::ObjectName))
    {
        return AnyResolution;
    }

    return McpActorUtils::ResolveActorFiltered(World, Identifier,
        [](AActor* Actor)
        {
            return Actor && Actor->IsA(ADynamicMeshActor::StaticClass());
        }, McpActorUtils::EActorResolvePolicy::ExactIdentity);
}

McpActorUtils::FActorResolution ResolveAnyActor(UWorld* World, const FString& Identifier)
{
    return IsValid(World) && !Identifier.IsEmpty()
        ? McpActorUtils::ResolveActorFiltered(World, Identifier,
            [](AActor*) { return true; }, McpActorUtils::EActorResolvePolicy::ExactIdentity)
        : McpActorUtils::FActorResolution();
}

ADynamicMeshActor* FindMeshActor(UWorld* World, const FString& Identifier)
{
    const McpActorUtils::FActorResolution Resolution = ResolveMeshActor(World, Identifier);
    return Resolution.IsResolved() ? Cast<ADynamicMeshActor>(Resolution.Actor) : nullptr;
}

AActor* FindAnyActor(UWorld* World, const FString& Identifier)
{
    const McpActorUtils::FActorResolution Resolution = ResolveAnyActor(World, Identifier);
    return Resolution.IsResolved() ? Resolution.Actor : nullptr;
}

bool Find(
    UWorld* World,
    const FString& Identifier,
    FGeometryTarget& Out,
    McpActorUtils::FActorResolution* OutResolution)
{
    Out = FGeometryTarget();
    const McpActorUtils::FActorResolution Resolution = ResolveMeshActor(World, Identifier);
    if (OutResolution)
    {
        *OutResolution = Resolution;
    }

    Out.Actor = Resolution.IsResolved() ? Cast<ADynamicMeshActor>(Resolution.Actor) : nullptr;
    if (!Out.Actor)
    {
        return false;
    }

    Out.Component = Out.Actor->GetDynamicMeshComponent();
    Out.Mesh = Out.Component ? Out.Component->GetDynamicMesh() : nullptr;
    return Out.Mesh != nullptr;
}

bool ResolveOrSendError(
    const FHandlerContext& Ctx,
    const FString& ActorName,
    FGeometryTarget& Out,
    const FResolveOptions& Options)
{
    Out = FGeometryTarget();
    if (ActorName.IsEmpty())
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT, TEXT("actorName required"));
        return false;
    }

    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!IsValid(World))
    {
        Ctx.SendError(Options.NoWorldCode, Options.NoWorldMessage);
        return false;
    }

    const McpActorUtils::FActorResolution Resolution = ResolveMeshActor(World, ActorName);
    if (Resolution.IsAmbiguous())
    {
        ActorNameParamUtils::SendAmbiguousActorError(Ctx, ActorName, Resolution);
        return false;
    }

    Out.Actor = Resolution.IsResolved() ? Cast<ADynamicMeshActor>(Resolution.Actor) : nullptr;
    if (!Out.Actor)
    {
        Ctx.SendError(ErrorCodes::ERR_ACTOR_NOT_FOUND, Options.Role
            ? FString::Printf(TEXT("%s actor not found: %s"), Options.Role, *ActorName)
            : FString::Printf(TEXT("Actor not found: %s"), *ActorName));
        return false;
    }

    Out.Component = Out.Actor->GetDynamicMeshComponent();
    if (!Out.Component && Options.bReportMissingComponent)
    {
        Ctx.SendError(ErrorCodes::ERR_COMPONENT_NOT_FOUND, TEXT("DynamicMeshComponent not found on actor"));
        return false;
    }

    Out.Mesh = Out.Component ? Out.Component->GetDynamicMesh() : nullptr;
    if (!Out.Mesh)
    {
        Ctx.SendError(ErrorCodes::ERR_MESH_NOT_FOUND, TEXT("DynamicMesh not available"));
        return false;
    }

    return true;
}

ADynamicMeshActor* Spawn(
    const FHandlerContext& Ctx,
    UDynamicMesh* DynMesh,
    const FTransform& Transform,
    const FString& Label,
    const FSpawnOptions& Options)
{
    if (!GEditor)
    {
        if (DynMesh) DynMesh->MarkAsGarbage();
        Ctx.SendError(ErrorCodes::ERR_EDITOR_NOT_AVAILABLE, TEXT("Editor not available"));
        return nullptr;
    }

    UWorld* SpawnWorld = GEditor->GetEditorWorldContext().World();
    if (!SpawnWorld && Options.bFallBackToActorSubsystemWorld)
    {
        if (UEditorActorSubsystem* ActorSS = GEditor->GetEditorSubsystem<UEditorActorSubsystem>())
        {
            SpawnWorld = ActorSS->GetWorld();
        }
    }
    if (!IsValid(SpawnWorld))
    {
        if (DynMesh) DynMesh->MarkAsGarbage();
        Ctx.SendError(ErrorCodes::ERR_EDITOR_WORLD_NOT_AVAILABLE, TEXT("Editor world not available"));
        return nullptr;
    }

    const FVector SpawnLocation = Transform.GetLocation();
    const FRotator SpawnRotation = Transform.Rotator();
    FActorSpawnParameters SpawnParams;
    SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
    // Templated SpawnActor rather than the UClass* overload + Cast: the class was already the
    // fixed ADynamicMeshActor::StaticClass(), so the cast could only ever fail on a spawn that
    // did not happen - and its false branch skipped SetDynamicMesh silently, handing back an
    // actor with no mesh and no error.
    ADynamicMeshActor* NewActor = SpawnWorld->SpawnActor<ADynamicMeshActor>(
        SpawnLocation, SpawnRotation, SpawnParams);
    if (!NewActor)
    {
        if (DynMesh) DynMesh->MarkAsGarbage();
        Ctx.SendError(ErrorCodes::ERR_SPAWN_FAILED, TEXT("Failed to spawn DynamicMeshActor"));
        return nullptr;
    }

    NewActor->SetActorLabel(Label);
    NewActor->SetActorScale3D(Transform.GetScale3D());

    if (UDynamicMeshComponent* DMComp = NewActor->GetDynamicMeshComponent())
    {
        DMComp->SetDynamicMesh(DynMesh);
    }

    GeometryUtils::MarkGeometryActorSpawned(NewActor);

    return NewActor;
}

} // namespace GeometryTarget
