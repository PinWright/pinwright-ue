// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Misc/Guid.h"
#include "UObject/Object.h"
#include "TestPropertyUtilsMapStructKey.generated.h"

// Fixture UCLASS for TestPropertyUtilsMapStructKey.cpp. Hosts a TMap whose key
// is FGuid (a UStruct), exercising the non-{Str,Name,Int} fallback in
// PropertyUtils.cpp::ExportPropertyToJsonValue.
UCLASS()
class UTestMapStructKeyHost : public UObject
{
    GENERATED_BODY()

public:
    UPROPERTY()
    TMap<FGuid, int32> GuidToInt;
};
