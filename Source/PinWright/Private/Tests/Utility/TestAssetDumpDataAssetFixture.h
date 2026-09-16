// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "Engine/DataTable.h"
#include "TestAssetDumpDataAssetFixture.generated.h"

UCLASS()
class UTestAssetDumpPrimaryDataAsset : public UPrimaryDataAsset
{
    GENERATED_BODY()

public:
    UPROPERTY(EditAnywhere, Category = "Asset Dump")
    FString SentinelText;

    UPROPERTY(EditAnywhere, Category = "Asset Dump")
    int32 DefaultNumber = 7;
};

USTRUCT()
struct FTestAssetDumpDataTableRow : public FTableRowBase
{
    GENERATED_BODY()

    UPROPERTY()
    FString Label;

    UPROPERTY()
    int32 Score = 0;
};
