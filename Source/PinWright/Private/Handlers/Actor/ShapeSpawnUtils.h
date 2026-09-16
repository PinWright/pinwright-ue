// Copyright (c) 2026 Alexander Penkin. MIT License.

// ShapeSpawnUtils.h - Shared helpers for the actor.spawn_shape / actor.spawn_batch
// level-prototyping verbs: BasicShapes shape-token resolution, editor-world lookup,
// and the small spawn primitives (static mesh, skeletal mesh, arbitrary class).
//
// Kept as inline functions in a named namespace (not an anonymous namespace) so both
// handler translation units share one definition without an ODR / redefinition error
// when Unity merges them into a single TU. Mirrors SpawnHandler.cpp's file-local
// ResolveSpawnWorld / SpawnActorInWorld, which are in an anonymous namespace and so
// are not reachable across TUs.
#pragma once

#include "CoreMinimal.h"
#include "Editor.h"
#include "Animation/SkeletalMeshActor.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "GameFramework/Actor.h"
#include "Engine/World.h"

namespace ShapeSpawnUtils
{
    // Resolves the world for placing prototyping actors: the active PIE world when one
    // is running, else the editor world. Mirrors SpawnHandler's ResolveSpawnWorld.
    inline UWorld* ResolveEditorWorld()
    {
        if (!GEditor)
        {
            return nullptr;
        }
        if (GEditor->PlayWorld)
        {
            return GEditor->PlayWorld;
        }
        return GEditor->GetEditorWorldContext().World();
    }

    // Maps a case-insensitive shape token to its /Engine/BasicShapes asset path.
    // Accepts CUBE / SPHERE / CYLINDER / CONE / PLANE. Returns false (OutMeshPath
    // cleared) for an unrecognized token.
    inline bool ResolveShapeMeshPath(const FString& Shape, FString& OutMeshPath)
    {
        const FString Key = Shape.TrimStartAndEnd().ToUpper();
        const TCHAR* Name = nullptr;
        if (Key == TEXT("CUBE"))          { Name = TEXT("Cube"); }
        else if (Key == TEXT("SPHERE"))   { Name = TEXT("Sphere"); }
        else if (Key == TEXT("CYLINDER")) { Name = TEXT("Cylinder"); }
        else if (Key == TEXT("CONE"))     { Name = TEXT("Cone"); }
        else if (Key == TEXT("PLANE"))    { Name = TEXT("Plane"); }

        if (!Name)
        {
            OutMeshPath.Empty();
            return false;
        }
        OutMeshPath = FString::Printf(TEXT("/Engine/BasicShapes/%s.%s"), Name, Name);
        return true;
    }

    // Comma-separated list of accepted shape tokens, for INVALID_PARAMS messages.
    inline const FString& ValidShapesCsv()
    {
        static const FString Csv = TEXT("CUBE, SPHERE, CYLINDER, CONE, PLANE");
        return Csv;
    }

    // Spawns an AStaticMeshActor carrying Mesh at Transform, labeled Label. Uses
    // TeleportPhysics so the initial transform is not interpolated, sets Movable
    // mobility, and marks render state dirty. Mirrors actor.spawn's static-mesh path.
    inline AActor* SpawnStaticMeshActor(UWorld* World, UStaticMesh* Mesh,
                                        const FTransform& Transform, const FString& Label)
    {
        if (!World || !Mesh)
        {
            return nullptr;
        }
        FActorSpawnParameters SpawnParams;
        SpawnParams.SpawnCollisionHandlingOverride =
            ESpawnActorCollisionHandlingMethod::AdjustIfPossibleButAlwaysSpawn;
        AStaticMeshActor* Actor = World->SpawnActor<AStaticMeshActor>(
            AStaticMeshActor::StaticClass(), Transform.GetLocation(), Transform.Rotator(),
            SpawnParams);
        if (!Actor)
        {
            return nullptr;
        }
        Actor->SetActorTransform(Transform, false, nullptr, ETeleportType::TeleportPhysics);
        if (UStaticMeshComponent* MeshComp = Actor->GetStaticMeshComponent())
        {
            MeshComp->SetMobility(EComponentMobility::Movable);
            MeshComp->SetStaticMesh(Mesh);
            MeshComp->MarkRenderStateDirty();
        }
        if (!Label.IsEmpty())
        {
            Actor->SetActorLabel(Label);
        }
        return Actor;
    }

    // Companion to SpawnStaticMeshActor for a meshPath that resolves to a skeletal mesh.
    inline AActor* SpawnSkeletalMeshActor(UWorld* World, USkeletalMesh* Mesh,
                                          const FTransform& Transform, const FString& Label)
    {
        if (!World || !Mesh)
        {
            return nullptr;
        }
        FActorSpawnParameters SpawnParams;
        SpawnParams.SpawnCollisionHandlingOverride =
            ESpawnActorCollisionHandlingMethod::AdjustIfPossibleButAlwaysSpawn;
        ASkeletalMeshActor* Actor = World->SpawnActor<ASkeletalMeshActor>(
            ASkeletalMeshActor::StaticClass(), Transform.GetLocation(), Transform.Rotator(),
            SpawnParams);
        if (!Actor)
        {
            return nullptr;
        }
        Actor->SetActorTransform(Transform, false, nullptr, ETeleportType::TeleportPhysics);
        if (USkeletalMeshComponent* MeshComp = Actor->GetSkeletalMeshComponent())
        {
            MeshComp->SetMobility(EComponentMobility::Movable);
            MeshComp->SetSkeletalMesh(Mesh);
            MeshComp->MarkRenderStateDirty();
        }
        if (!Label.IsEmpty())
        {
            Actor->SetActorLabel(Label);
        }
        return Actor;
    }

    // Spawns an instance of an arbitrary actor class at Transform, labeled Label.
    inline AActor* SpawnActorOfClass(UWorld* World, UClass* Class,
                                     const FTransform& Transform, const FString& Label)
    {
        if (!World || !Class)
        {
            return nullptr;
        }
        FActorSpawnParameters SpawnParams;
        SpawnParams.SpawnCollisionHandlingOverride =
            ESpawnActorCollisionHandlingMethod::AdjustIfPossibleButAlwaysSpawn;
        AActor* Actor = World->SpawnActor(Class, &Transform, SpawnParams);
        if (!Actor)
        {
            return nullptr;
        }
        Actor->SetActorTransform(Transform, false, nullptr, ETeleportType::TeleportPhysics);
        if (!Label.IsEmpty())
        {
            Actor->SetActorLabel(Label);
        }
        return Actor;
    }
}
