// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"

class UMaterialExpressionSetMaterialAttributes;

namespace FMGIRMaterialAttributeUtils
{
    FString GetStableAttributeName(const FGuid& AttributeID);
    TArray<FString> GetStableAttributeNames(const UMaterialExpressionSetMaterialAttributes* Expression);
    bool TryResolveAttributeID(
        const FString& AttributeNameOrGuid,
        const UMaterialExpressionSetMaterialAttributes* Expression,
        FGuid& OutAttributeID);
    bool ApplyAttributeNames(
        UMaterialExpressionSetMaterialAttributes* Expression,
        const TArray<FString>& AttributeNames,
        FString& OutError);
    void RebuildInputsFromAttributeSetTypes(UMaterialExpressionSetMaterialAttributes* Expression);
    bool TryGetWireInputName(
        const UMaterialExpressionSetMaterialAttributes* Expression,
        const FString& StableAttributeName,
        FString& OutInputName);
}
