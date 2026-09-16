// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "TestObjectArrayImportFixture.generated.h"

// Fixture UCLASS for TestObjectArrayImportFailLoud.cpp — hosts a reflected object
// array (an FArrayProperty whose Inner is an FObjectProperty over base UObject).
// Mirrors the reported repro's shape (UInputAction::Modifiers, a
// TArray<TObjectPtr<UInputModifier>>) without pulling in EnhancedInput or an
// on-disk asset, so the array-inner FObjectProperty import branch can be driven
// directly. Kept on a transient UObject so no engine CDO or on-disk asset is mutated.
UCLASS()
class UTestObjectArrayImportHost : public UObject
{
    GENERATED_BODY()

public:
    // TArray<UObject*>: the FArrayProperty->FObjectProperty inner branch under test.
    UPROPERTY()
    TArray<TObjectPtr<UObject>> ObjectList;
};
