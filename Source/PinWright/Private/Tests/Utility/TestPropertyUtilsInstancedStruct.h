// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "Compat/InstancedStructCompat.h"
#include "TestPropertyUtilsInstancedStruct.generated.h"

// Fixture types for TestPropertyUtilsInstancedStruct.cpp. Exercises the
// FInstancedStruct type-erased recursion added to StructToJsonObject in
// PropertyExport.cpp: a wrapper whose payload (UScriptStruct* + heap block) is
// non-reflected must surface its inner struct's fields rather than emitting {}.

// Inner payload struct held by the FInstancedStruct wrappers below.
USTRUCT()
struct FTestInstancedStructPayload
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
class UTestInstancedStructHost : public UObject
{
    GENERATED_BODY()

public:
    // Single-valued case (mirrors UChooserTable::FallbackResult).
    UPROPERTY()
    FInstancedStruct SingleResult;

    // Array case (mirrors UChooserTable::ResultsStructs / ColumnsStructs).
    UPROPERTY()
    TArray<FInstancedStruct> ArrayResults;
};
