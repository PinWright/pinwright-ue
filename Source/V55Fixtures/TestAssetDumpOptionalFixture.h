// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

// Reflected fixture exposing a TOptional<T> UPROPERTY (FOptionalProperty), which UHT
// can only reflect on UE 5.5+. This file lives in a sibling source directory that the
// module's Build.cs adds via ConditionalAddModuleDirectory only on 5.5+, so on 5.3/5.4
// it is never scanned by UHT (which forbids version #if around reflected types). The
// two OptInt tests that use it are themselves gated to 5.5+.
#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "TestAssetDumpOptionalFixture.generated.h"

UCLASS()
class UTestAssetDumpOptionalFixture : public UObject
{
    GENERATED_BODY()

public:
    UPROPERTY()
    TOptional<int32> OptInt;
};
