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
struct FTestDataTableAtomicInner
{
    GENERATED_BODY()

    UPROPERTY()
    ETestDataTableAtomicMode Mode = ETestDataTableAtomicMode::Stable;
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

    // Enum literals nested in a container and a sub-struct, for the pre-converter enum check.
    UPROPERTY()
    TArray<ETestDataTableAtomicMode> Modes;

    UPROPERTY()
    FTestDataTableAtomicInner Inner;

    // Declared last: a malformed date string is only rejected by the converter's own parser,
    // after every earlier field has been written, which is the late failure set_row must roll back.
    UPROPERTY()
    FDateTime When;
};
