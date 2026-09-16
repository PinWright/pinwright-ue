// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Containers/UnrealString.h"
#include "EdGraph/EdGraphPin.h"
#include "Templates/UniquePtr.h"

enum class EIrTypeKind : uint8
{
    Void,
    Bool, Byte, Int, Int64, Float, Double, String, Name, Text, FieldPath,
    Struct, Object, SoftObject, Class, SoftClass, Enum, Interface,
    Delegate, McDelegate,
    Unresolved,
};

struct PINWRIGHT_API FIrTypeSpec
{
    EIrTypeKind Kind = EIrTypeKind::Unresolved;
    FName InnerName;
    // Second tagged-form identifier for arity-2 tagged kinds (currently only
    // Delegate/McDelegate, where InnerName carries the signature-owner hint
    // and SecondaryInnerName carries the UFunction signature name). NAME_None
    // for every other kind.
    FName SecondaryInnerName;
    EPinContainerType Container = EPinContainerType::None;
    TUniquePtr<FIrTypeSpec> ElementSpec;
    TUniquePtr<FIrTypeSpec> KeySpec;
    bool bIsConst = false;
    bool bIsReference = false;

    FIrTypeSpec() = default;

    FIrTypeSpec(const FIrTypeSpec& Other);
    FIrTypeSpec& operator=(const FIrTypeSpec& Other);
    FIrTypeSpec(FIrTypeSpec&&) = default;
    FIrTypeSpec& operator=(FIrTypeSpec&&) = default;

    bool IsVoid() const;
    bool IsEmpty() const;
    bool Equals(const FIrTypeSpec& Other) const;
};
