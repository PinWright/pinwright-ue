// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "TestPropertyUtilsArrayStructElement.generated.h"

// Fixture types for TestPropertyUtilsArrayStructElement.cpp. Exercises the
// FStructProperty inner-element branch added to the FArrayProperty loop in
// PropertyUtils.cpp::ExportPropertyToJsonValue.
USTRUCT()
struct FTestArrayStructElementInner
{
    GENERATED_BODY()

    UPROPERTY()
    int32 IntField = 0;

    UPROPERTY()
    FString StringField;

    UPROPERTY()
    bool BoolField = false;
};

UCLASS()
class UTestArrayStructElementHost : public UObject
{
    GENERATED_BODY()

public:
    UPROPERTY()
    TArray<FTestArrayStructElementInner> Items;
};
