// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

// The READ half of the mesh-rebuild render guard: which live components hold a scene proxy
// over a mesh asset's render data, and whether that asset is loaded at all.
//
// It sits in the main module beside FQuiesceScope in Utils/MeshRebuildRenderGuard.h because two
// modules need it and the module dependency only runs one way - PinWrightGeometry links PinWright,
// never the reverse. model.compile (geometry) and the main-module mesh mutators quiesce this set
// before rebuilding a mesh in place and refuse when they cannot; static_mesh.describe (main
// module) reports it, so a caller learns a rebuild would be refused from a read rather than from
// the refusal.
//
// Scanning only. Nothing here touches the render thread; the destroy / flush / recreate half
// stays with the guard. The main module already carries Niagara publicly, and Geometry declares
// Niagara explicitly because this shared header is included by its model tests/handlers.
//
// UStaticMeshComponents are matched to the requested mesh directly. Niagara components are
// matched through their assigned system's emitter renderer properties and mesh entries; a
// dynamic mesh binding is reported conservatively because its runtime object is not available
// from the asset graph alone.

#include "CoreMinimal.h"
#include "Compat/EngineVersionCompat.h"
#include "Components/ActorComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Containers/ArrayView.h"
#include "NiagaraComponent.h"
#include "NiagaraEmitter.h"
#include "NiagaraEmitterHandle.h"
#include "NiagaraMeshRendererProperties.h"
#include "NiagaraSystem.h"
#include "UObject/Class.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UObjectIterator.h"

namespace PinWrightMeshRebuild
{
    // Component classes whose scene proxy caches a mesh asset's render data. This path table is
    // retained for static_mesh.describe's diagnostic metadata; the actual rebuild scan below
    // uses concrete types so it can inspect the target mesh rather than treating every Niagara
    // component as a consumer.
    //
    // Resolved by class PATH so the metadata remains valid when Niagara is not loaded in an
    // editor session, even though PinWright's editor module already carries its Niagara module
    // dependency for the concrete matcher.
    inline const TCHAR* const StaleRenderStateComponentClassPaths[] =
    {
        TEXT("/Script/Niagara.NiagaraComponent"),
    };

    // The candidate classes that are actually loaded in this editor. A path that resolves to
    // nothing means the owning plugin is not present, so no component of that class exists.
    inline TArray<UClass*> ResolveStaleRenderStateComponentClasses()
    {
        TArray<UClass*> Classes;
        for (const TCHAR* ClassPath : StaleRenderStateComponentClassPaths)
        {
            if (UClass* Class = FindObject<UClass>(nullptr, ClassPath))
            {
                Classes.Add(Class);
            }
        }
        return Classes;
    }

    // A Niagara mesh renderer can either carry explicit static-mesh entries or bind its mesh
    // array/slot to a runtime parameter. An explicit entry is exact; a valid runtime binding may
    // reference the target and cannot be resolved from the authoring graph, so this predicate
    // deliberately reports "may reference" and keeps the candidate conservative.
    inline bool NiagaraMeshRendererMayReferenceTargetMesh(
        const UNiagaraMeshRendererProperties& Renderer, const TArray<UStaticMesh*>& TargetMeshes)
    {
        for (const FNiagaraMeshRendererMeshProperties& MeshProperties : Renderer.Meshes)
        {
            if (TargetMeshes.Contains(MeshProperties.Mesh.Get()) ||
                MeshProperties.MeshParameterBinding.ResolvedParameter.IsValid())
            {
                return true;
            }
        }

#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 6, 0)
        return Renderer.MeshesBinding.ResolvedParameter.IsValid();
#else
        // UNiagaraMeshRendererProperties gained the whole-array MeshesBinding in UE 5.6. Before
        // that a renderer can only bind a mesh per entry, which the loop above already tests, so
        // there is no further binding that could reference the target.
        return false;
#endif
    }

    inline bool NiagaraComponentMayReferenceTargetMesh(
        const UNiagaraComponent& Component, const TArray<UStaticMesh*>& TargetMeshes)
    {
        const UNiagaraSystem* System = Component.GetAsset();
        if (!System)
        {
            return false;
        }

        for (const FNiagaraEmitterHandle& EmitterHandle : System->GetEmitterHandles())
        {
            if (!EmitterHandle.GetIsEnabled())
            {
                continue;
            }

            bool bMayReferenceTarget = false;
            auto TestRenderer = [&TargetMeshes, &bMayReferenceTarget](
                const UNiagaraRendererProperties* Renderer)
            {
                const UNiagaraMeshRendererProperties* MeshRenderer =
                    Cast<UNiagaraMeshRendererProperties>(Renderer);
                if (MeshRenderer &&
                    NiagaraMeshRendererMayReferenceTargetMesh(*MeshRenderer, TargetMeshes))
                {
                    bMayReferenceTarget = true;
                }
            };

#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 7, 0)
            // 5.7+ enumerates renderers off the handle, which also reaches stateless emitters.
            EmitterHandle.ForEachEnabledRendererWithIndex(
                [&TestRenderer](const UNiagaraRendererProperties* Renderer, int32)
            {
                TestRenderer(Renderer);
            });
#else
            // Before 5.7 the handle has no renderer enumeration and every emitter is a standard
            // one, so the versioned emitter data is the complete renderer source.
            if (const FVersionedNiagaraEmitterData* EmitterData = EmitterHandle.GetEmitterData())
            {
                EmitterData->ForEachEnabledRenderer(TestRenderer);
            }
#endif
            if (bMayReferenceTarget)
            {
                return true;
            }
        }

        return false;
    }

    // Enumerate every live component the shared StaticMesh rebuild guard must cycle. Static mesh
    // components are matched to the requested meshes, while Niagara components are matched to
    // their actual system emitter mesh renderers. Components that are unregistered or have no
    // render state hold no scene proxy and are intentionally skipped.
    inline TArray<UActorComponent*> ScanForStaticMeshRebuildConsumers(
        const TArray<UStaticMesh*>& TargetMeshes)
    {
        TArray<UActorComponent*> Out;
        if (TargetMeshes.Num() == 0)
        {
            return Out;
        }

        for (TObjectIterator<UStaticMeshComponent> It; It; ++It)
        {
            UStaticMeshComponent* Component = *It;
            if (!Component || !Component->IsRegistered() || !Component->IsRenderStateCreated() ||
                !TargetMeshes.Contains(Component->GetStaticMesh()))
            {
                continue;
            }

            Out.Add(Component);
        }

        for (TObjectIterator<UNiagaraComponent> It; It; ++It)
        {
            UNiagaraComponent* Component = *It;
            if (Component && Component->IsRegistered() && Component->IsRenderStateCreated() &&
                NiagaraComponentMayReferenceTargetMesh(*Component, TargetMeshes))
            {
                Out.Add(Component);
            }
        }

        return Out;
    }

}
