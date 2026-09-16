// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "PinWrightAssetImportReferenceActor.generated.h"

class UTexture2D;

UCLASS(Transient)
class APinWrightAssetImportReferenceActor : public AActor
{
    GENERATED_BODY()

public:
    APinWrightAssetImportReferenceActor();

    UPROPERTY()
    TObjectPtr<UTexture2D> HardReference;

    UPROPERTY()
    TSoftObjectPtr<UTexture2D> SoftReference;

    UPROPERTY()
    TArray<TObjectPtr<UObject>> HardReferences;

    UPROPERTY()
    TArray<TSoftObjectPtr<UObject>> SoftReferences;
};
