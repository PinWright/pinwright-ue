// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "TestPropertyListContainerCppType.generated.h"

// Fixture for TestPropertyListContainerCppType.cpp. Declares container UPROPERTYs
// (TMap / TArray / TSet) plus a scalar, so the regression test can assert that
// property.list emits a templated cppType ("TMap<FName,FString>"), not the bare
// container token ("TMap") that the no-arg GetCPPType() drops the parameters from.
UCLASS()
class UTestPropertyListContainerCppTypeHost : public UObject
{
    GENERATED_BODY()

public:
    UPROPERTY()
    TMap<FName, FString> NameToString;

    UPROPERTY()
    TArray<FVector> Points;

    UPROPERTY()
    TSet<FName> Tags;

    UPROPERTY()
    float Scalar = 0.f;
};
