// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "UObject/Object.h"
#include "UObject/FieldPath.h"
#include "UObject/SoftObjectPtr.h"
#include "TestAssetDumpObjectRefsFixture.generated.h"

// Fixture UCLASS exposing one UPROPERTY of every kind the property-serializer
// expansion in PropertyUtils.cpp now handles. Used only by
// TestAssetDumpObjectRefs.cpp; lives in the tests module.
UCLASS()
class UTestAssetDumpObjectRefsFixture : public UObject
{
    GENERATED_BODY()

public:
    UPROPERTY()
    TObjectPtr<UTexture2D> HardObjectRef;

    UPROPERTY()
    TSubclassOf<UObject> ClassRef;

    UPROPERTY()
    TArray<TObjectPtr<UTexture2D>> ObjectArray;

    UPROPERTY()
    TMap<FString, TObjectPtr<UTexture2D>> ObjectMap;

    UPROPERTY()
    TFieldPath<FProperty> FieldPathRef;

    UPROPERTY()
    TSoftObjectPtr<UWorld> MapRef;

    UPROPERTY()
    TArray<TSoftObjectPtr<UWorld>> MapList;

    UPROPERTY()
    TSoftObjectPtr<UTexture2D> NotAMap;
};
