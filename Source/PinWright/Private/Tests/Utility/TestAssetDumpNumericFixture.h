// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Engine/Texture2D.h"
#include "UObject/Object.h"
#include "UObject/LazyObjectPtr.h"
#include "UObject/WeakObjectPtr.h"
#include "TestAssetDumpNumericFixture.generated.h"

// Fixture UCLASS exposing UPROPERTYs of the previously-unsupported numeric and
// smart-pointer types. Drives the regression tests for the typed-encoder gap
// at PropertyUtils.cpp ~L442-510 and the CPF_Transient skip at the asset-dump
// walk site.
UCLASS()
class UTestAssetDumpNumericFixture : public UObject
{
    GENERATED_BODY()

public:
    UPROPERTY()
    uint32 U32 = 0;

    UPROPERTY()
    int8 I8 = 0;

    UPROPERTY()
    uint16 U16 = 0;

    UPROPERTY()
    uint64 U64 = 0;

    UPROPERTY()
    TWeakObjectPtr<UTexture2D> WeakRef;

    UPROPERTY()
    TLazyObjectPtr<UTexture2D> LazyRef;

    // Transient field — must be skipped by BuildClassPropertyJson.
    UPROPERTY(Transient)
    int32 TransientCounter = 0;

    // Non-transient field of identical type, present so we can assert the
    // transient-skip is selective rather than blanket.
    UPROPERTY()
    int32 PersistentCounter = 0;
};
