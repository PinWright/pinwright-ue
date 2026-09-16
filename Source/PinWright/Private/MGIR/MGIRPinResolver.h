// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "IrCore/IrPinResolverBase.h"


class UMaterial;
class UMaterialExpression;
struct FExpressionInput;

struct FMGIRPinReference
{
    FString SymbolName;
    int32 OutputIndex = 0;
    bool bHasMask = false;
    int32 MaskR = 0;
    int32 MaskG = 0;
    int32 MaskB = 0;
    int32 MaskA = 0;
};

struct FMGIRResolvedPin
{
    UMaterialExpression* Expression = nullptr;
    int32 OutputIndex = 0;
    bool bHasMask = false;
    int32 MaskR = 0;
    int32 MaskG = 0;
    int32 MaskB = 0;
    int32 MaskA = 0;
};

using FMGIRWireResult = TIrPinResolverBase<UMaterialExpression, FMGIRResolvedPin>::FWireResult;

class FMGIRPinResolver : protected TIrPinResolverBase<UMaterialExpression, FMGIRResolvedPin>
{
public:
    static bool ParseReference(
        const FString& Text,
        FMGIRPinReference& OutReference,
        FString& OutErrorMessage);

    static FMGIRWireResult ResolveReference(
        const FString& Text,
        const TMap<FString, UMaterialExpression*>& Symbols,
        FMGIRResolvedPin& OutPin);

    static FMGIRWireResult WireExpressionInput(
        UMaterialExpression* TargetExpression,
        const FString& InputName,
        const FString& SourceReference,
        const TMap<FString, UMaterialExpression*>& Symbols);

    static FMGIRWireResult WireMaterialOutput(
        UMaterial* Material,
        const FString& OutputName,
        const FMGIRResolvedPin& Source);

    static void ApplyResolvedPin(FExpressionInput& Input, const FMGIRResolvedPin& Source);
};
