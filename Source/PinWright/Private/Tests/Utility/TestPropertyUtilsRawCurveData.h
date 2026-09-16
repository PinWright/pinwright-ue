// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Animation/AnimCurveTypes.h"
#include "UObject/Object.h"
#include "TestPropertyUtilsRawCurveData.generated.h"

UCLASS()
class UTestRawCurveDataHost : public UObject
{
    GENERATED_BODY()

public:
    UPROPERTY()
    FRawCurveTracks RawCurveData;
};
