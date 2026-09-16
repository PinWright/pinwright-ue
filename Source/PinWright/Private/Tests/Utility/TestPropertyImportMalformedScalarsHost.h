// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "TestPropertyImportMalformedScalarsHost.generated.h"

USTRUCT()
struct FTestMalformedArrayStruct
{
    GENERATED_BODY()

    UPROPERTY()
    int32 FirstValue = 11;

    UPROPERTY()
    int32 SecondValue = 22;
};

UCLASS()
class UTestPropertyImportMalformedScalarsHost : public UObject
{
    GENERATED_BODY()

public:
    UPROPERTY()
    bool BoolValue = true;

    UPROPERTY()
    float FloatValue = 1.25f;

    UPROPERTY()
    float FixedFloatValues[2] = { 1.25f, 2.5f };

    UPROPERTY()
    double DoubleValue = 2.5;

    UPROPERTY()
    int32 IntValue = 123;

    UPROPERTY()
    int64 Int64Value = 456;

    UPROPERTY()
    uint16 UInt16Value = 7;

    UPROPERTY()
    int32 FixedIntValues[2] = { 11, 22 };

    UPROPERTY()
    TArray<int32> IntArray;

    UPROPERTY()
    TArray<FTestMalformedArrayStruct> StructArray;

    UPROPERTY()
    TMap<FString, int32> IntMap;

    UPROPERTY()
    TSet<float> FloatSet;
};
