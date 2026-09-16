// Copyright (c) 2026 Alexander Penkin. MIT License.

// Concrete data asset class for MCP inventory/item operations
#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "McpGenericDataAsset.generated.h"

/**
 * Concrete data asset class for MCP inventory/item operations.
 * Both UDataAsset and UPrimaryDataAsset are abstract in UE5,
 * so we need a concrete wrapper that can be instantiated.
 */
UCLASS(BlueprintType)
class PINWRIGHT_API UMcpGenericDataAsset : public UDataAsset
{
    GENERATED_BODY()

public:
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "MCP Data")
    FString ItemName;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "MCP Data")
    FString Description;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "MCP Data")
    TMap<FString, FString> Properties;
};
