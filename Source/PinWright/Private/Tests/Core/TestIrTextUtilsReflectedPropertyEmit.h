// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Templates/SubclassOf.h"
#include "UObject/Object.h"
#include "UObject/SoftObjectPtr.h"
#include "TestIrTextUtilsReflectedPropertyEmit.generated.h"

UENUM()
enum class EIrTextUtilsReflectedEnum : uint8
{
    First,
    Second
};

UENUM()
enum EIrTextUtilsReflectedByteEnum
{
    IrTextUtilsReflectedByteZero,
    IrTextUtilsReflectedByteOne
};

UCLASS()
class UTestIrTextUtilsReflectedObject : public UObject
{
    GENERATED_BODY()
};

UCLASS()
class UTestIrTextUtilsReflectedPropertyHost : public UObject
{
    GENERATED_BODY()

public:
    UPROPERTY(EditAnywhere, Category = "Test")
    bool BoolValue = false;

    UPROPERTY(EditAnywhere, Category = "Test")
    int32 IntValue = 0;

    UPROPERTY(EditAnywhere, Category = "Test")
    float FloatValue = 0.0f;

    UPROPERTY(EditAnywhere, Category = "Test")
    EIrTextUtilsReflectedEnum EnumValue = EIrTextUtilsReflectedEnum::First;

    UPROPERTY(EditAnywhere, Category = "Test")
    TEnumAsByte<EIrTextUtilsReflectedByteEnum> ByteEnumValue = IrTextUtilsReflectedByteZero;

    UPROPERTY(EditAnywhere, Category = "Test")
    uint8 RawByteValue = 0;

    UPROPERTY(EditAnywhere, Category = "Test")
    UObject* ObjectValue = nullptr;

    UPROPERTY(EditAnywhere, Category = "Test")
    TSubclassOf<UObject> ClassValue;

    UPROPERTY(EditAnywhere, Category = "Test")
    TSoftObjectPtr<UObject> SoftObjectValue;

    UPROPERTY(EditAnywhere, Category = "Test")
    TSoftClassPtr<UObject> SoftClassValue;

    UPROPERTY(EditAnywhere, Category = "Test")
    FString StringValue;

    UPROPERTY(EditAnywhere, Category = "Test")
    FName NameValue;

    UPROPERTY(EditAnywhere, Category = "Test")
    FText TextValue;

    UPROPERTY(EditAnywhere, Category = "Test")
    TArray<FString> StringArray;

    UPROPERTY(EditAnywhere, Category = "Test")
    int32 AlphaSortValue = 0;

    UPROPERTY(EditAnywhere, Category = "Test")
    int32 DefaultIdenticalValue = 0;

    UPROPERTY(EditAnywhere, Category = "Test")
    int32 ExplicitValue = 0;

    UPROPERTY(EditAnywhere, Category = "Test")
    int32 RejectedByNameValue = 0;

    UPROPERTY(EditAnywhere, Category = "Test")
    FVector VectorValue = FVector::ZeroVector;

    UPROPERTY(EditAnywhere, Category = "Test")
    int32 ZetaSortValue = 0;
};

// Fixture for B-decompile-struct-subfield-dropped: a struct whose sub-fields have
// NON-zero CDO defaults. Exporting it with a nullptr default (the bug) compares
// each sub-field against its type's zero-value instead of the archetype default,
// so a non-default value equal to zero is dropped and a value left at its
// non-zero default is spuriously emitted.
USTRUCT()
struct FIrTextUtilsReflectedSubStruct
{
    GENERATED_BODY()

    // Mirrors PCG's bRandomizedPruning (real default true). Driven to false (its
    // type's zero-value) it must still be emitted; a nullptr default drops it.
    UPROPERTY(EditAnywhere, Category = "Test")
    bool bDefaultTrueFlag = true;

    // Non-zero default: left at its default it must be suppressed, not spuriously
    // emitted as if overridden.
    UPROPERTY(EditAnywhere, Category = "Test")
    int32 DefaultedInt = 5;
};

UCLASS()
class UTestIrTextUtilsReflectedStructDefaultHost : public UObject
{
    GENERATED_BODY()

public:
    UPROPERTY(EditAnywhere, Category = "Test")
    FIrTextUtilsReflectedSubStruct SubStructValue;
};
