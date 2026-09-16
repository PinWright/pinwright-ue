// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "TestPropertyExportNoiseFiltering.generated.h"

USTRUCT()
struct FPropertyExportNoiseFixture
{
    GENERATED_BODY()

    UPROPERTY()
    int32 StableValue = 11;

    // Same name as UMovieSceneSignedObject's derived GUID, but a different owner.
    UPROPERTY()
    int32 Signature = 12;

    UPROPERTY(Transient)
    int32 TransientValue = 13;

    // UHT only permits DuplicateTransient on class members. The automation
    // fixture adds CPF_DuplicateTransient to this reflected field at runtime.
    UPROPERTY()
    int32 DuplicateTransientValue = 14;

    UPROPERTY(SkipSerialization)
    int32 SkipSerializationValue = 15;

    UPROPERTY()
    int32 DeprecatedValue_DEPRECATED = 16;
};

UCLASS()
class UTestPropertyExportNoiseHost : public UObject
{
    GENERATED_BODY()

public:
    UPROPERTY()
    FPropertyExportNoiseFixture Value;
};
