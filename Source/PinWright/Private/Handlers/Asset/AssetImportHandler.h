// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Utils/AssetImportPolicy.h"

class FJsonObject;

namespace PinWrightAssetImportHandler
{
    struct FRequest
    {
        FString SourcePath;
        FString SourceName;
        FString DestinationPath;
        FString DestinationName;
        FString RequestedAssetPath;
        bool bOverwrite = false;
    };

    struct FResult
    {
        bool bSuccess = false;
        FString ErrorCode;
        FString Message;
        TSharedPtr<FJsonObject> Data;
    };

    // Synchronous work owned by the registered asset.import handler. Production
    // calls it from the serialized safe-point continuation; focused tests call
    // the same path with request-local execution dependencies, including an
    // optional verifier that runs only after a successful reimport adapter.
    PINWRIGHT_API FResult Execute(
        const FRequest& Request,
        const AssetImportPolicy::FExecutionDependencies* Dependencies = nullptr);
}
