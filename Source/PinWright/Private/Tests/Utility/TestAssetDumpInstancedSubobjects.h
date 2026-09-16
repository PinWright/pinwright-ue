// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Components/SceneComponent.h"
#include "UObject/Object.h"
#include "TestAssetDumpInstancedSubobjects.generated.h"

DECLARE_DYNAMIC_MULTICAST_DELEGATE(FTestPropertyUtilsEmptyMulticastDelegate);

USTRUCT()
struct FTestPropertyExportComponentContainer
{
    GENERATED_BODY()

    UPROPERTY()
    TObjectPtr<USceneComponent> PopulatedComponent = nullptr;

    UPROPERTY()
    TObjectPtr<USceneComponent> NullComponent = nullptr;

    UPROPERTY()
    TMap<FString, FString> MarkerShapedMap;

    UPROPERTY()
    TArray<FSoftObjectPath> UnsupportedNestedValues;
};

// Fixture for TestAssetDumpInstancedSubobjects.cpp. Contrasts explicitly owned
// top-level object expansion with path-shaped raw collection storage.
UCLASS()
class UTestInstancedPayload : public UObject
{
    GENERATED_BODY()
public:
    UPROPERTY(EditAnywhere, Category="Test")
    int32 ConfigInt = 0;

    UPROPERTY(EditAnywhere, Category="Test")
    FString ConfigName;

    UPROPERTY(EditAnywhere, Category="Test")
    int32 SparseEditableChanged = 0;

    UPROPERTY(EditAnywhere, Category="Test")
    int32 SparseEditableDefault = 17;

    UPROPERTY()
    int32 SparseHiddenChanged = 0;

    UPROPERTY(EditAnywhere, Transient, Category="Test")
    int32 SparseTransientChanged = 0;
};

UCLASS()
class UTestInstancedHost : public UObject
{
    GENERATED_BODY()
public:
    UPROPERTY(Instanced)
    TObjectPtr<UTestInstancedPayload> Payload;

    UPROPERTY(Instanced)
    TArray<TObjectPtr<UTestInstancedPayload>> PayloadArray;

    UPROPERTY()
    TObjectPtr<USceneComponent> OwnedComponentPointer;

    UPROPERTY()
    TObjectPtr<USceneComponent> ExternalComponentPointer;
};

UCLASS()
class UTestPropertyUtilsDelegateHost : public UObject
{
    GENERATED_BODY()
public:
    UPROPERTY(BlueprintAssignable, Category="Test")
    FTestPropertyUtilsEmptyMulticastDelegate OnInlineEvent;
};
