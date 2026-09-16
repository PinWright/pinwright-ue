// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Compat/EngineVersionCompat.h"
#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/ErrorCodes.h"
#include "PinWrightSubsystem.h"
#include "PinWrightHelpers.h"

#include "Editor.h"
#include "LevelEditor.h"
#include "Subsystems/EditorActorSubsystem.h"
#include "WorldPartition/WorldPartition.h"

// WorldPartitionEditorLoaderAdapter is the canonical region-loading entry point on UE 5.4+.
#include "WorldPartition/WorldPartitionEditorLoaderAdapter.h"
#include "WorldPartition/LoaderAdapter/LoaderAdapterShape.h"

#include "WorldPartition/DataLayer/DataLayerSubsystem.h"

// DataLayerEditorSubsystem — tolerate both header locations seen across UE 5.4–5.7.
#if __has_include("DataLayer/DataLayerEditorSubsystem.h")
#  include "DataLayer/DataLayerEditorSubsystem.h"
#elif __has_include("WorldPartition/DataLayer/DataLayerEditorSubsystem.h")
#  include "WorldPartition/DataLayer/DataLayerEditorSubsystem.h"
#endif

#include "WorldPartition/DataLayer/DataLayerInstance.h"
#include "WorldPartition/DataLayer/DataLayerManager.h"
#include "WorldPartition/DataLayer/DataLayerAsset.h"
#include "WorldPartition/DataLayer/WorldDataLayers.h"

#include "Dom/JsonValue.h"
#include "UObject/Package.h"
#include "Misc/PackageName.h"
#include "Utils/PathUtils.h"
#include "Utils/AssetUtils.h"

// ---- world_partition.load_cells ----
REGISTER_RPC_HANDLER("world_partition.load_cells", "world_partition", "Load world partition cells in a region",
    RPC_PARAMS(
        RPC_PARAM_OPT("origin", "array", "Origin [x,y,z] of the region"),
        RPC_PARAM_OPT("extent", "array", "Extent [x,y,z] of the region")
    ))
{
    UWorld* World = GEditor->GetEditorWorldContext().World();
    if (!World)
    {
        Ctx.SendError(ErrorCodes::ERR_NO_WORLD, TEXT("No active editor world."));
        return true;
    }

    UWorldPartition* WorldPartition = World->GetWorldPartition();
    if (!WorldPartition)
    {
        Ctx.SendError(ErrorCodes::ERR_NOT_PARTITIONED, TEXT("World is not partitioned."));
        return true;
    }

    TSharedPtr<FJsonObject> Payload = Ctx.GetRawPayload();
    FVector Origin = FVector::ZeroVector;
    FVector Extent = FVector(25000.0f, 25000.0f, 25000.0f);

    const TArray<TSharedPtr<FJsonValue>>* OriginArr;
    if (Payload->TryGetArrayField(TEXT("origin"), OriginArr) && OriginArr && OriginArr->Num() >= 3)
    {
        Origin.X = (*OriginArr)[0]->AsNumber();
        Origin.Y = (*OriginArr)[1]->AsNumber();
        Origin.Z = (*OriginArr)[2]->AsNumber();
    }

    const TArray<TSharedPtr<FJsonValue>>* ExtentArr;
    if (Payload->TryGetArrayField(TEXT("extent"), ExtentArr) && ExtentArr && ExtentArr->Num() >= 3)
    {
        Extent.X = (*ExtentArr)[0]->AsNumber();
        Extent.Y = (*ExtentArr)[1]->AsNumber();
        Extent.Z = (*ExtentArr)[2]->AsNumber();
    }

    FBox Bounds(Origin - Extent, Origin + Extent);

    if (WorldPartition)
    {
        UWorldPartitionEditorLoaderAdapter* EditorLoaderAdapter = WorldPartition->CreateEditorLoaderAdapter<FLoaderAdapterShape>(World, Bounds, TEXT("MCP Loaded Region"));
        if (EditorLoaderAdapter && EditorLoaderAdapter->GetLoaderAdapter())
        {
            EditorLoaderAdapter->GetLoaderAdapter()->SetUserCreated(true);
            EditorLoaderAdapter->GetLoaderAdapter()->Load();
            TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
            Result->SetStringField(TEXT("action"), TEXT("load_region"));
            Result->SetStringField(TEXT("method"), TEXT("LoaderAdapter"));
            Result->SetBoolField(TEXT("requested"), true);
            Ctx.SendSuccess(Result);
            return true;
        }
    }

    Ctx.SendError(ErrorCodes::ERR_NOT_SUPPORTED, TEXT("WorldPartition region loading not supported or failed in this engine version."));
    return true;
}

// ---- world_partition.create_datalayer ----
REGISTER_RPC_HANDLER("world_partition.create_datalayer", "world_partition", "Create a new data layer",
    RPC_PARAMS(
        RPC_PARAM_REQ("dataLayerName", "string", "Name for the new data layer")
    ))
{
    UWorld* World = GEditor->GetEditorWorldContext().World();
    if (!World) { Ctx.SendError(ErrorCodes::ERR_NO_WORLD, TEXT("No active editor world.")); return true; }
    UWorldPartition* WorldPartition = World->GetWorldPartition();
    if (!WorldPartition) { Ctx.SendError(ErrorCodes::ERR_NOT_PARTITIONED, TEXT("World is not partitioned.")); return true; }

    FString DataLayerName = Ctx.GetString(TEXT("dataLayerName"));
    if (DataLayerName.IsEmpty())
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_PARAMS, TEXT("Missing dataLayerName."));
        return true;
    }

    UDataLayerEditorSubsystem* DataLayerSubsystem = GEditor->GetEditorSubsystem<UDataLayerEditorSubsystem>();
    if (DataLayerSubsystem)
    {
        bool bExists = false;
        UWorldPartition* WP = World->GetWorldPartition();
        if (UDataLayerManager* DataLayerManager = WP ? WP->GetDataLayerManager() : nullptr)
        {
            DataLayerManager->ForEachDataLayerInstance([&](UDataLayerInstance* LayerInstance) {
                if (LayerInstance->GetDataLayerShortName() == DataLayerName || LayerInstance->GetDataLayerFullName() == DataLayerName)
                {
                    bExists = true;
                    return false;
                }
                return true;
            });
        }

        if (bExists)
        {
            Ctx.SendSuccess(FString::Printf(TEXT("DataLayer '%s' already exists."), *DataLayerName));
            return true;
        }

        // Create the UDataLayerAsset in a REAL on-disk package, not GetTransientPackage().
        // A transient-outer asset lives under /Engine/Transient and can never be
        // serialized, so the data layer and every set_datalayer assignment that
        // references it would dangle on level save/reload (silent persistence no-op).
        // Build a sanitized /Game-rooted package path and create the asset there with
        // RF_Standalone so it is a saveable content asset, mirroring the persistable
        // sibling level.structure.create_data_layer.
        FString FullAssetPath;
        FString PathError;
        if (!ValidateAssetCreationPath(TEXT("/Game/DataLayers"), DataLayerName, FullAssetPath, PathError))
        {
            Ctx.SendError(ErrorCodes::ERR_INVALID_PARAMS,
                FString::Printf(TEXT("Invalid data layer name/path: %s"), *PathError));
            return true;
        }

        UPackage* AssetPackage = CreatePackage(*FullAssetPath);
        if (!AssetPackage)
        {
            Ctx.SendError(ErrorCodes::ERR_PACKAGE_CREATION_FAILED,
                FString::Printf(TEXT("Failed to create package for DataLayerAsset at: %s"), *FullAssetPath));
            return true;
        }

        const FName AssetObjectName = FName(*FPackageName::GetShortName(FullAssetPath));
        UDataLayerAsset* NewAsset = NewObject<UDataLayerAsset>(AssetPackage, UDataLayerAsset::StaticClass(), AssetObjectName, RF_Public | RF_Standalone);
        UDataLayerInstance* NewLayer = nullptr;

        if (NewAsset)
        {
            // McpSafeAssetSave already marks the package dirty and notifies the
            // asset registry (FAssetRegistryModule::AssetCreated); no need to do
            // either explicitly. DataLayerSubsystem is non-null here (outer guard).
            McpSafeAssetSave(NewAsset);

            FDataLayerCreationParameters Params;
            Params.DataLayerAsset = NewAsset;
            Params.WorldDataLayers = World->GetWorldDataLayers();
            NewLayer = DataLayerSubsystem->CreateDataLayerInstance(Params);
        }

        if (NewLayer)
        {
            TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
            Result->SetStringField(TEXT("message"), FString::Printf(TEXT("DataLayer '%s' created."), *DataLayerName));
            Result->SetStringField(TEXT("dataLayerName"), DataLayerName);
            Result->SetStringField(TEXT("dataLayerAssetPath"), FullAssetPath);
            AddAssetVerification(Result, NewAsset);
            Ctx.SendSuccess(Result);
        }
        else
        {
            Ctx.SendError(ErrorCodes::ERR_CREATE_FAILED, TEXT("Failed to create DataLayer (Subsystem returned null)."));
        }
    }
    else
    {
        Ctx.SendError(ErrorCodes::ERR_SUBSYSTEM_NOT_FOUND, TEXT("DataLayerEditorSubsystem not found."));
    }
    return true;
}

// ---- world_partition.set_datalayer ----
REGISTER_RPC_HANDLER("world_partition.set_datalayer", "world_partition", "Assign an actor to a data layer",
    RPC_PARAMS(
        RPC_PARAM_REQ("actorPath", "path", "Path or label of the actor"),
        RPC_PARAM_REQ("dataLayerName", "string", "Name of the data layer")
    ))
{
    UWorld* World = GEditor->GetEditorWorldContext().World();
    if (!World) { Ctx.SendError(ErrorCodes::ERR_NO_WORLD, TEXT("No active editor world.")); return true; }
    UWorldPartition* WorldPartition = World->GetWorldPartition();
    if (!WorldPartition) { Ctx.SendError(ErrorCodes::ERR_NOT_PARTITIONED, TEXT("World is not partitioned.")); return true; }

    FString ActorPath = Ctx.GetString(TEXT("actorPath"));
    FString DataLayerName = Ctx.GetString(TEXT("dataLayerName"));

    AActor* Actor = FindObject<AActor>(nullptr, *ActorPath);
    if (!Actor)
    {
        if (UEditorActorSubsystem* ActorSS = GEditor->GetEditorSubsystem<UEditorActorSubsystem>())
        {
            TArray<AActor*> AllActors = ActorSS->GetAllLevelActors();
            for (AActor* A : AllActors)
            {
                if (A && A->GetActorLabel().Equals(ActorPath, ESearchCase::IgnoreCase))
                {
                    Actor = A;
                    break;
                }
            }
        }
    }

    if (!Actor)
    {
        Ctx.SendError(ErrorCodes::ERR_ACTOR_NOT_FOUND, FString::Printf(TEXT("Actor not found: %s"), *ActorPath));
        return true;
    }

    UDataLayerEditorSubsystem* DataLayerSubsystem = GEditor->GetEditorSubsystem<UDataLayerEditorSubsystem>();
    if (DataLayerSubsystem)
    {
        UDataLayerInstance* TargetLayer = nullptr;

        if (UDataLayerManager* DataLayerManager = WorldPartition->GetDataLayerManager())
        {
            DataLayerManager->ForEachDataLayerInstance([&](UDataLayerInstance* LayerInstance) {
                if (LayerInstance->GetDataLayerShortName() == DataLayerName || LayerInstance->GetDataLayerFullName() == DataLayerName)
                {
                    TargetLayer = LayerInstance;
                    return false;
                }
                return true;
            });
        }

        if (TargetLayer)
        {
            TArray<AActor*> Actors;
            Actors.Add(Actor);
            TArray<UDataLayerInstance*> Layers;
            Layers.Add(TargetLayer);

            const bool bWasPresent = Actor->ContainsDataLayer(TargetLayer);
#if UE_VERSION_OLDER_THAN(5, 6, 0)
            // UE 5.6 added IsPackageExternal() to AActor::SupportsDataLayerType, so from 5.6 on the
            // engine itself refuses an actor that has no external actor package. Older engines
            // accept the assignment and leave behind a membership the actor's own package cannot
            // carry, so apply the same precondition here and let the read-back below report it.
            const bool bEligible = Actor->IsPackageExternal();
#else
            constexpr bool bEligible = true;
#endif
            const bool bEngineChanged = bEligible && DataLayerSubsystem->AddActorsToDataLayers(Actors, Layers);
            const bool bPresentAfter = Actor->ContainsDataLayer(TargetLayer);
            const bool bAdded = !bWasPresent && bPresentAfter;

            TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
            Result->SetStringField(TEXT("dataLayerName"), DataLayerName);
            Result->SetStringField(TEXT("outcome"), bPresentAfter
                ? (bAdded ? TEXT("added") : TEXT("already_present"))
                : TEXT("rejected"));
            Result->SetBoolField(TEXT("added"), bAdded);
            Result->SetBoolField(TEXT("alreadyPresent"), bPresentAfter && bWasPresent);
            Result->SetBoolField(TEXT("engineChanged"), bEngineChanged);
            Result->SetBoolField(TEXT("verified"), bPresentAfter);
            AddActorVerification(Result, Actor);

            if (bWasPresent && bPresentAfter)
            {
                Ctx.SendError(ErrorCodes::ERR_DATALAYER_ALREADY_ASSIGNED,
                    FString::Printf(TEXT("Data Layer '%s' is already assigned to actor '%s'; no membership change was made."),
                        *DataLayerName, *Actor->GetActorLabel()), Result);
            }
            else if (!bEngineChanged || !bAdded)
            {
                Ctx.SendError(ErrorCodes::ERR_VERIFICATION_FAILED,
                    FString::Printf(TEXT("Data Layer '%s' did not produce a verified new membership on actor '%s'; "
                                         "the engine rejected the assignment or its result contradicted the read-back."),
                        *DataLayerName, *Actor->GetActorLabel()), Result);
            }
            else
            {
                Ctx.SendSuccess(Result);
            }
        }
        else
        {
            Ctx.SendError(ErrorCodes::ERR_DATALAYER_NOT_FOUND, FString::Printf(TEXT("DataLayer '%s' not found."), *DataLayerName));
        }
    }
    else
    {
        Ctx.SendError(ErrorCodes::ERR_SUBSYSTEM_NOT_FOUND, TEXT("DataLayerEditorSubsystem not found."));
    }
    return true;
}

// ---- world_partition.cleanup_invalid_datalayers ----
REGISTER_RPC_HANDLER("world_partition.cleanup_invalid_datalayers", "world_partition", "Remove data layer instances with missing assets",
    RPC_NO_PARAMS)
{
    UWorld* World = GEditor->GetEditorWorldContext().World();
    if (!World) { Ctx.SendError(ErrorCodes::ERR_NO_WORLD, TEXT("No active editor world.")); return true; }
    UWorldPartition* WorldPartition = World->GetWorldPartition();
    if (!WorldPartition) { Ctx.SendError(ErrorCodes::ERR_NOT_PARTITIONED, TEXT("World is not partitioned.")); return true; }

    UDataLayerEditorSubsystem* DataLayerSubsystem = GEditor->GetEditorSubsystem<UDataLayerEditorSubsystem>();
    if (!DataLayerSubsystem)
    {
        Ctx.SendError(ErrorCodes::ERR_SUBSYSTEM_NOT_FOUND, TEXT("DataLayerEditorSubsystem not found."));
        return true;
    }

    UDataLayerManager* DataLayerManager = WorldPartition ? WorldPartition->GetDataLayerManager() : nullptr;
    if (!DataLayerManager)
    {
        Ctx.SendError(ErrorCodes::ERR_MANAGER_NOT_FOUND, TEXT("DataLayerManager not found."));
        return true;
    }

    TArray<UDataLayerInstance*> InvalidInstances;
    DataLayerManager->ForEachDataLayerInstance([&](UDataLayerInstance* LayerInstance) {
        if (LayerInstance && !LayerInstance->GetAsset())
        {
            InvalidInstances.Add(LayerInstance);
        }
        return true;
    });

    auto MakeInstanceResult = [](const FString& DataLayerName, const FString& DataLayerFullName,
        const FString& InstancePath, bool bVerified, const FString& Reason)
    {
        TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetStringField(TEXT("dataLayerName"), DataLayerName);
        Entry->SetStringField(TEXT("dataLayerFullName"), DataLayerFullName);
        Entry->SetStringField(TEXT("dataLayerPath"), InstancePath);
        Entry->SetBoolField(TEXT("assetMissing"), true);
        Entry->SetBoolField(TEXT("verified"), bVerified);
        if (!Reason.IsEmpty())
        {
            Entry->SetStringField(TEXT("reason"), Reason);
        }
        return Entry;
    };

    TArray<TSharedPtr<FJsonValue>> Deleted;
    TArray<TSharedPtr<FJsonValue>> Failed;
    for (UDataLayerInstance* InvalidInstance : InvalidInstances)
    {
        const FString InstancePath = InvalidInstance->GetPathName();
        const FString DataLayerName = InvalidInstance->GetDataLayerShortName();
        const FString DataLayerFullName = InvalidInstance->GetDataLayerFullName();

        // UDataLayerInstance::CanBeRemoved arrived in UE 5.4 with the external-data-layer
        // instances that are the only thing that answers false to it; on 5.3 no data layer
        // instance is unremovable, which is exactly what the 5.4 base implementation returns.
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 4, 0)
        if (!InvalidInstance->CanBeRemoved())
        {
            Failed.Add(MakeShared<FJsonValueObject>(MakeInstanceResult(DataLayerName, DataLayerFullName, InstancePath,
                false, TEXT("The engine reports this data layer instance cannot be removed."))));
            continue;
        }
#endif

        DataLayerSubsystem->DeleteDataLayer(InvalidInstance);

        bool bStillPresent = false;
        DataLayerManager->ForEachDataLayerInstance([&](UDataLayerInstance* LayerInstance) {
            if (LayerInstance && LayerInstance->GetPathName() == InstancePath)
            {
                bStillPresent = true;
                return false;
            }
            return true;
        });

        if (bStillPresent)
        {
            Failed.Add(MakeShared<FJsonValueObject>(MakeInstanceResult(DataLayerName, DataLayerFullName, InstancePath,
                false, TEXT("DeleteDataLayer returned but the instance remains in the data layer manager."))));
        }
        else
        {
            Deleted.Add(MakeShared<FJsonValueObject>(MakeInstanceResult(DataLayerName, DataLayerFullName, InstancePath,
                true, FString())));
        }
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetArrayField(TEXT("deleted"), Deleted);
    Result->SetArrayField(TEXT("failed"), Failed);
    Result->SetNumberField(TEXT("candidateCount"), InvalidInstances.Num());
    Result->SetNumberField(TEXT("deletedCount"), Deleted.Num());
    Result->SetNumberField(TEXT("failedCount"), Failed.Num());
    Result->SetStringField(TEXT("message"), FString::Printf(
        TEXT("Processed %d invalid Data Layer Instance(s): %d deleted, %d failed."),
        InvalidInstances.Num(), Deleted.Num(), Failed.Num()));

    if (InvalidInstances.IsEmpty())
    {
        Ctx.SendError(ErrorCodes::ERR_NO_INVALID_DATALAYERS,
            TEXT("No invalid Data Layer Instances were found; cleanup made no changes."), Result);
    }
    else if (Failed.Num() > 0)
    {
        const TCHAR* ErrorCode = Deleted.Num() > 0
            ? ErrorCodes::ERR_DELETE_PARTIAL
            : ErrorCodes::ERR_DELETE_FAILED;
        Ctx.SendError(ErrorCode,
            FString::Printf(TEXT("Data Layer cleanup verified %d deletion(s) and %d failure(s); see deleted[] and failed[]."),
                Deleted.Num(), Failed.Num()), Result);
    }
    else
    {
        Ctx.SendSuccess(Result);
    }
    return true;
}
