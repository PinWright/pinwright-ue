// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DataTable.h"
#include "TestDataTableAtomicityFixture.generated.h"

UENUM()
enum class ETestDataTableAtomicMode : uint8
{
    Stable,
    Alternate
};

USTRUCT()
struct FTestDataTableAtomicRow : public FTableRowBase
{
    GENERATED_BODY()

    UPROPERTY()
    FString Label = TEXT("initial");

    UPROPERTY()
    int32 Score = 17;

    UPROPERTY()
    ETestDataTableAtomicMode Mode = ETestDataTableAtomicMode::Stable;
};
