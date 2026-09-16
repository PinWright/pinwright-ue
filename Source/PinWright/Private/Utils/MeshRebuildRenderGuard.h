// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

// Shared guard for rebuilding a StaticMesh in place while the editor is still drawing it.
// UStaticMesh::Build reallocates FStaticMeshRenderData. Every live UStaticMeshComponent using the
// target, plus Niagara mesh consumers, must release its render state before the build and recreate
// it after the build and save have finished. The safe-point hop is part of this helper so every
// in-place rebuild uses the same ordering and refusal path.

#include "CoreMinimal.h"
#include "Containers/ArrayView.h"
#include "ComponentRecreateRenderStateContext.h"
#include "Components/ActorComponent.h"
#include "Dispatch/SafePoint.h"
#include "Engine/StaticMesh.h"
#include "Handlers/ErrorCodes.h"
#include "Misc/App.h"
#include "Misc/PackageName.h"
#include "RenderingThread.h"
#include "Utils/MeshRenderConsumerScan.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UObjectGlobals.h"

namespace PinWrightMeshRebuild
{
    // Destroys the render state of every component handed to it and recreates it on scope exit,
    // so the rebuild happens with no proxy holding the old render data. One UE context owns each
    // matched component for the whole callback; Build/PostEditChange then see the component
    // already detached and do not nest a second unregister/reregister cycle for it.
    class FQuiesceScope
    {
    public:
        explicit FQuiesceScope(TArrayView<UActorComponent* const> Components)
        {
            if (Components.Num() == 0)
            {
                return;
            }

            if (FApp::CanEverRender())
            {
                FlushRenderingCommands();
            }

            Contexts.Reserve(Components.Num());
            for (UActorComponent* Component : Components)
            {
                if (!IsValid(Component) || Component->IsUnreachable())
                {
                    NotQuiescedPathNames.Add(Component ? Component->GetPathName() : FString(TEXT("<null>")));
                    continue;
                }

                if (!Component->IsRegistered() || !Component->IsRenderStateCreated())
                {
                    continue;
                }

                RetainedComponents.Emplace(Component);
                Contexts.Emplace(Component);

                if (Component->IsRenderStateCreated())
                {
                    NotQuiescedPathNames.Add(Component->GetPathName());
                }
            }

            if (FApp::CanEverRender())
            {
                FlushRenderingCommands();
            }
        }

        FQuiesceScope(const FQuiesceScope&) = delete;
        FQuiesceScope& operator=(const FQuiesceScope&) = delete;

        ~FQuiesceScope() = default;

        int32 Num() const { return Contexts.Num(); }

        const TArray<FString>& NotQuiesced() const { return NotQuiescedPathNames; }

    private:
        // FComponentRecreateRenderStateContext keeps a raw component pointer. Declare the strong
        // roots first so reverse member destruction releases them only after the contexts restore
        // their render state on FQuiesceScope teardown.
        TArray<TStrongObjectPtr<UActorComponent>> RetainedComponents;
        TArray<FComponentRecreateRenderStateContext> Contexts;
        TArray<FString> NotQuiescedPathNames;
    };

    // Handlers accept both object paths (/Game/Mesh.Mesh) and package paths (/Game/Mesh). Resolve
    // the latter to the conventional asset object name so an occupied model.compile output is
    // guarded just like the object-path inputs used by the asset mutators.
    inline UStaticMesh* ResolveStaticMesh(const FString& AssetPath)
    {
        if (UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, *AssetPath))
        {
            return Mesh;
        }

        const FString PackagePath = FPackageName::ObjectPathToPackageName(AssetPath);
        const FString AssetName = FPackageName::GetShortName(PackagePath);
        if (PackagePath.IsEmpty() || AssetName.IsEmpty())
        {
            return nullptr;
        }

        const FString ObjectPath = PackagePath + TEXT(".") + AssetName;
        return LoadObject<UStaticMesh>(nullptr, *ObjectPath);
    }

    using FStaticMeshRebuildWork = TFunction<void(
        const PinWrightSafePoint::FSafePointResponder&, const TArray<UStaticMesh*>&)>;

    // Resolve the requested meshes at the safe point, quiesce every live target-mesh component and
    // stale Niagara consumer, then run the complete caller operation while the scope is alive.
    // The caller's work must include Build/PostEditChange, compilation drains, and saving; the
    // scope restores render state only after that callback returns. Invalid paths are passed
    // through as an empty mesh list so batch callers can preserve their existing skip semantics.
    inline bool RunGuardedStaticMeshRebuild(const FHandlerContext& Ctx, const TCHAR* Reason,
                                            const TArray<FString>& AssetPaths,
                                            FStaticMeshRebuildWork Work)
    {
        return PinWrightSafePoint::RunAtSafePoint(Ctx, Reason,
            [Paths = AssetPaths, Work = MoveTemp(Work)](
                const PinWrightSafePoint::FSafePointResponder& Responder) mutable
        {
            TArray<UStaticMesh*> Meshes;
            Meshes.Reserve(Paths.Num());
            for (const FString& Path : Paths)
            {
                if (UStaticMesh* Mesh = ResolveStaticMesh(Path))
                {
                    Meshes.Add(Mesh);
                }
            }

            if (Meshes.Num() == 0)
            {
                Work(Responder, Meshes);
                return;
            }

            const TArray<UActorComponent*> RenderConsumers =
                PinWrightMeshRebuild::ScanForStaticMeshRebuildConsumers(Meshes);
            FQuiesceScope Quiesce(RenderConsumers);

            if (Quiesce.NotQuiesced().Num() > 0)
            {
                Responder.SendError(ErrorCodes::ERR_MESH_REBUILD_CONSUMER_NOT_QUIESCABLE,
                    FString::Printf(
                        TEXT("Refusing to rebuild StaticMesh '%s': %d live component(s) still "
                              "cache its render data after the guard tried to quiesce them (%s). "
                              "Nothing was built or saved. Delete or deactivate those components, "
                              "then retry."),
                        *Meshes[0]->GetPathName(), Quiesce.NotQuiesced().Num(),
                        *FString::Join(Quiesce.NotQuiesced(), TEXT(", "))));
                return;
            }

            Work(Responder, Meshes);
        });
    }

    // Batch callers that report one outcome per original input need the null slots retained when
    // a mesh disappears between preflight and the safe-point callback. The tag makes this an
    // additive overload: existing callers keep the filtered TArray<UStaticMesh*> contract above.
    struct FPreserveInputSlotsTag
    {
    };
    inline constexpr FPreserveInputSlotsTag PreserveInputSlots;

    struct FPreservedStaticMeshBatch
    {
        TArray<UStaticMesh*> Meshes;
        TSharedPtr<FQuiesceScope> Quiesce;
        bool bCanRebuild = true;
        FString ErrorCode;
        FString Error;
    };

    using FPreservedStaticMeshRebuildWork = TFunction<void(
        const PinWrightSafePoint::FSafePointResponder&, const FPreservedStaticMeshBatch&)>;

    inline bool RunGuardedStaticMeshRebuild(const FHandlerContext& Ctx, const TCHAR* Reason,
                                            const TArray<FString>& AssetPaths,
                                            FPreserveInputSlotsTag,
                                            FPreservedStaticMeshRebuildWork Work)
    {
        return PinWrightSafePoint::RunAtSafePoint(Ctx, Reason,
            [Paths = AssetPaths, Work = MoveTemp(Work)](
                const PinWrightSafePoint::FSafePointResponder& Responder) mutable
        {
            FPreservedStaticMeshBatch Batch;
            Batch.Meshes.Reserve(Paths.Num());

            TArray<UStaticMesh*> ResolvedMeshes;
            ResolvedMeshes.Reserve(Paths.Num());
            for (const FString& Path : Paths)
            {
                UStaticMesh* Mesh = ResolveStaticMesh(Path);
                Batch.Meshes.Add(Mesh);
                if (Mesh)
                {
                    ResolvedMeshes.AddUnique(Mesh);
                }
            }

            if (ResolvedMeshes.Num() == 0)
            {
                Work(Responder, Batch);
                return;
            }

            const TArray<UActorComponent*> RenderConsumers =
                PinWrightMeshRebuild::ScanForStaticMeshRebuildConsumers(ResolvedMeshes);
            Batch.Quiesce = MakeShared<FQuiesceScope>(RenderConsumers);

            if (Batch.Quiesce->NotQuiesced().Num() > 0)
            {
                Batch.bCanRebuild = false;
                Batch.ErrorCode = ErrorCodes::ERR_MESH_REBUILD_CONSUMER_NOT_QUIESCABLE;
                Batch.Error = FString::Printf(
                    TEXT("Refusing to rebuild StaticMesh '%s': %d live component(s) still "
                         "cache its render data after the guard tried to quiesce them (%s). "
                         "Nothing was built or saved. Delete or deactivate those components, "
                         "then retry."),
                    *ResolvedMeshes[0]->GetPathName(), Batch.Quiesce->NotQuiesced().Num(),
                    *FString::Join(Batch.Quiesce->NotQuiesced(), TEXT(", ")));
            }

            // Even a guard refusal reaches the batch callback. It must not rebuild when
            // bCanRebuild is false; it owns the per-input failures and aggregate response.
            Work(Responder, Batch);
        });
    }
}
