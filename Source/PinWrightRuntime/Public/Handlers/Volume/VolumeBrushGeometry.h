// Copyright (c) 2026 Alexander Penkin. MIT License.

// VolumeBrushGeometry.h - Brush-model construction for ABrush-derived volume actors.
//
// Lifted out of VolumeHandler.cpp, where it reached only the volume.* verbs, so the SPAWN
// paths in other namespaces reach the same builder instead of re-deriving it:
// foliage.create_procedural (AProceduralFoliageVolume) and actor.spawn (any AVolume
// subclass, APCGVolume among them) both landed brush actors with a null Brush UModel —
// bounds extent (0,0,0), no collision, every containment query answering empty, and
// success reported. B-spawned-volumes-have-no-brush-geometry.
//
// Named namespace + inline functions, matching Handlers/Environment/EnvironmentDirtyUtils.h:
// the plugin's handlers share one module with Unity enabled, where same-named
// anonymous-namespace helpers collide across merged translation units.

#pragma once

#include "CoreMinimal.h"
#include "BSPOps.h"
#include "Builders/CubeBuilder.h"
#include "Components/BrushComponent.h"
#include "Engine/Brush.h"
#include "Engine/Polys.h"
#include "Handlers/Environment/EnvironmentDirtyUtils.h"
#include "Model.h"
// UPackage must be complete here: GetTransientPackage() returns UPackage*, and a
// translation unit that has only the forward declaration cannot convert it to the
// UObject* NewObject takes (strict-includes build, reached through FoliageHandler.cpp).
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

namespace VolumeBrushGeometry
{
    // The box the editor itself gives a volume dragged in from the place-actors panel:
    // UActorFactoryBoxVolume builds with a default-constructed UCubeBuilder, whose X/Y/Z
    // are 200 uu of FULL size (EditorBrushBuilder.cpp:395-417), i.e. a 100 uu half-extent.
    inline constexpr float DefaultBoxSize = 200.0f;

    // Builds box geometry into a brush volume, initializing the brush UModel first.
    //
    // A freshly World->SpawnActor<ABrush-derived>() volume has a null Brush UModel, so
    // UEditorBrushBuilder::EndBrush early-returns success without writing any geometry
    // (Engine: EditorBrushBuilder.cpp — `if (Brush == nullptr) return true;`). The result
    // is a phantom volume that reports success but has no geometry and no collision.
    // This mirrors the engine's own UActorFactory::CreateBrushForVolumeActor: initialize
    // Brush (UModel) + Polys, wire BrushComponent->Brush, run the builder, then
    // csgPrepMovingBrush to build the collision model and refresh nav data.
    //
    // BoxSize is FULL size on each axis, not a half-extent — UCubeBuilder::X/Y/Z are the
    // cube's dimensions. Callers holding a half-extent go through CreateBoxBrushForVolume.
    inline bool BuildBoxBrushGeometry(ABrush* Volume, const FVector& BoxSize)
    {
        if (!Volume || !Volume->GetBrushComponent())
        {
            return false;
        }

        // Pre-mutation: everything below is a UPROPERTY write on the actor (PolyFlags,
        // Brush, BrushBuilder) or on its BrushComponent (Brush). On the spawn path this is
        // a redundant second Modify() after MarkLevelActorSpawned — idempotent and cheap.
        // On the `volume.set_volume_extent` / `volume.set_volume_bounds` paths it is the
        // ONLY thing that dirties anything at all.
        PinWright::MarkLevelActorModified(Volume, Volume->GetBrushComponent());

        Volume->PreEditChange(nullptr);

        // Match the volume's own transient/transactional flags for the new model objects.
        const EObjectFlags ObjectFlags = Volume->GetFlags() & (RF_Transient | RF_Transactional);

        Volume->PolyFlags = 0;
        Volume->Brush = NewObject<UModel>(Volume, NAME_None, ObjectFlags);
        Volume->Brush->Initialize(nullptr, true);
        Volume->Brush->Polys = NewObject<UPolys>(Volume->Brush, NAME_None, ObjectFlags);
        Volume->GetBrushComponent()->Brush = Volume->Brush;

        UCubeBuilder* CubeBuilder = NewObject<UCubeBuilder>(GetTransientPackage());
        CubeBuilder->X = BoxSize.X;
        CubeBuilder->Y = BoxSize.Y;
        CubeBuilder->Z = BoxSize.Z;
        Volume->BrushBuilder = DuplicateObject<UBrushBuilder>(CubeBuilder, Volume);

        CubeBuilder->Build(Volume->GetWorld(), Volume);

        // Build the BSP/simple collision model and refresh navigation from the new polys.
        FBSPOps::csgPrepMovingBrush(Volume);

        Volume->PostEditChange();

        return true;
    }

    inline bool CreateBoxBrushForVolume(ABrush* Volume, const FVector& Extent)
    {
        return BuildBoxBrushGeometry(Volume, FVector(Extent.X * 2.0f, Extent.Y * 2.0f, Extent.Z * 2.0f));
    }

    inline bool CreateSphereBrushForVolume(ABrush* Volume, float Radius)
    {
        return BuildBoxBrushGeometry(Volume, FVector(Radius * 2.0f, Radius * 2.0f, Radius * 2.0f));
    }

    inline bool CreateCapsuleBrushForVolume(ABrush* Volume, float Radius, float HalfHeight)
    {
        return BuildBoxBrushGeometry(Volume, FVector(Radius * 2.0f, Radius * 2.0f, HalfHeight * 2.0f));
    }
}
