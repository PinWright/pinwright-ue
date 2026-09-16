// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/Texture.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpression.h"
// FMaterialParameterMetadata — the uniform group/sortPriority struct GetParameterValue() fills.
// It moved from the top-level MaterialTypes.h into Materials/MaterialParameters.h in UE 5.7;
// on 5.6 and earlier it lives in MaterialTypes.h, which has no MaterialParameters.h.
#if __has_include("Materials/MaterialParameters.h")
#include "Materials/MaterialParameters.h"
#else
#include "MaterialTypes.h"
#endif
#include "MaterialExpressionIO.h"
#include "Material/MaterialInputIterCompat.h"
#include "Material/MaterialPinNames.h"
#include "Materials/MaterialExpressionMaterialFunctionCall.h"
#include "Materials/MaterialExpressionParameter.h"
#include "Materials/MaterialExpressionTextureSample.h"
#include "Materials/MaterialFunction.h"
#include "UObject/UnrealType.h"
#include "Utils/PropertyUtils.h"


namespace MGIRExpressionUtils
{
struct FExpressionSortRecord
{
    UMaterialExpression* Expression = nullptr;
    int32 Depth = 0;
    FString StableKey;
};

inline FString StableExpressionKey(const UMaterialExpression* Expression)
{
    if (!Expression)
    {
        return FString();
    }

    if (Expression->MaterialExpressionGuid.IsValid())
    {
        return Expression->MaterialExpressionGuid.ToString(EGuidFormats::Digits);
    }

    return Expression->GetName();
}

inline void CopyMaterialExpressions(UMaterial* Material, TArray<UMaterialExpression*>& OutExpressions)
{
    if (!Material)
    {
        return;
    }

    for (UMaterialExpression* Expression : Material->GetExpressions())
    {
        if (Expression)
        {
            OutExpressions.Add(Expression);
        }
    }
}

inline void CopyFunctionExpressions(UMaterialFunction* Function, TArray<UMaterialExpression*>& OutExpressions)
{
    if (!Function)
    {
        return;
    }

    for (const TObjectPtr<UMaterialExpression>& Expression : Function->GetExpressions())
    {
        if (Expression)
        {
            OutExpressions.Add(Expression.Get());
        }
    }
}

inline int32 ComputeExpressionDepth(
    UMaterialExpression* Expression,
    const TSet<UMaterialExpression*>& ExpressionSet,
    TMap<UMaterialExpression*, int32>& Depths,
    TSet<UMaterialExpression*>& Visiting)
{
    if (!Expression)
    {
        return 0;
    }

    if (int32* ExistingDepth = Depths.Find(Expression))
    {
        return *ExistingDepth;
    }

    if (Visiting.Contains(Expression))
    {
        return 0;
    }

    Visiting.Add(Expression);

    int32 Depth = 0;
    ForEachExpressionInput(Expression, [&](FExpressionInput* Input, int32) -> bool
    {
        if (!Input || !Input->Expression || !ExpressionSet.Contains(Input->Expression))
        {
            return false;
        }
        Depth = FMath::Max(Depth, ComputeExpressionDepth(Input->Expression, ExpressionSet, Depths, Visiting) + 1);
        return false;
    });

    Visiting.Remove(Expression);
    Depths.Add(Expression, Depth);
    return Depth;
}

inline TArray<FExpressionSortRecord> BuildExpressionSortRecords(const TArray<UMaterialExpression*>& Expressions)
{
    TArray<FExpressionSortRecord> Records;
    TSet<UMaterialExpression*> ExpressionSet;
    for (UMaterialExpression* Expression : Expressions)
    {
        if (Expression)
        {
            ExpressionSet.Add(Expression);
        }
    }

    TMap<UMaterialExpression*, int32> Depths;
    TSet<UMaterialExpression*> Visiting;
    for (UMaterialExpression* Expression : Expressions)
    {
        if (Expression)
        {
            ComputeExpressionDepth(Expression, ExpressionSet, Depths, Visiting);
        }
    }

    Records.Reserve(ExpressionSet.Num());
    for (UMaterialExpression* Expression : Expressions)
    {
        if (!Expression)
        {
            continue;
        }

        FExpressionSortRecord Record;
        Record.Expression = Expression;
        Record.Depth = Depths.FindRef(Expression);
        Record.StableKey = StableExpressionKey(Expression);
        Records.Add(MoveTemp(Record));
    }

    Records.Sort([](const FExpressionSortRecord& A, const FExpressionSortRecord& B)
    {
        if (A.Depth != B.Depth)
        {
            return A.Depth < B.Depth;
        }

        return A.StableKey < B.StableKey;
    });

    return Records;
}

inline void SortExpressionsByDependency(TArray<UMaterialExpression*>& Expressions)
{
    TArray<FExpressionSortRecord> Records = BuildExpressionSortRecords(Expressions);
    Expressions.Reset(Records.Num());
    for (const FExpressionSortRecord& Record : Records)
    {
        Expressions.Add(Record.Expression);
    }
}

// Returns the reflected FExpressionInput property name for an exact input pointer. The property
// name is an implementation alias, so callers should keep it optional and preserve the display
// name separately (for example StaticSwitchParameter exposes True/False over A/B).
inline FString FindReflectedInputPropertyName(
    UMaterialExpression* Expr,
    const FExpressionInput* Input)
{
    if (!Expr || !Input)
    {
        return FString();
    }

    for (TFieldIterator<FProperty> It(Expr->GetClass()); It; ++It)
    {
        const FStructProperty* StructProperty = CastField<FStructProperty>(*It);
        if (!StructProperty
            || !StructProperty->Struct
            || StructProperty->Struct->GetFName() != FName(TEXT("ExpressionInput")))
        {
            continue;
        }

        for (int32 ArrayIndex = 0; ArrayIndex < StructProperty->ArrayDim; ++ArrayIndex)
        {
            if (StructProperty->ContainerPtrToValuePtr<FExpressionInput>(Expr, ArrayIndex) != Input)
            {
                continue;
            }

            FString PropertyName = It->GetName();
            if (StructProperty->ArrayDim > 1 && ArrayIndex > 0)
            {
                PropertyName += FString::Printf(TEXT("_%d"), ArrayIndex);
            }
            return PropertyName;
        }
    }

    return FString();
}

// Canonical per-input pin JSON shape ({name, optional internalName, connectedNodeId, outputIndex,
// mask, maskR/G/B/A}).
// Single source of truth shared by BuildExpressionDetailsJson (ordinary expression nodes) and the
// main-output-node readback (BuildMainNodeInputsJson) so the two payloads cannot drift.
// connectedNodeId is the empty string for an unwired input.
inline TSharedPtr<FJsonObject> BuildExpressionInputJson(
    const FString& Name,
    const FExpressionInput& In,
    const FString& InternalName = FString())
{
    TSharedPtr<FJsonObject> InputJson = MakeShared<FJsonObject>();
    InputJson->SetStringField(TEXT("name"), Name);
    if (!InternalName.IsEmpty() && !InternalName.Equals(Name, ESearchCase::IgnoreCase))
    {
        InputJson->SetStringField(TEXT("internalName"), InternalName);
    }
    InputJson->SetStringField(TEXT("connectedNodeId"),
        In.Expression ? In.Expression->MaterialExpressionGuid.ToString() : FString());
    InputJson->SetNumberField(TEXT("outputIndex"), In.OutputIndex);
    InputJson->SetNumberField(TEXT("mask"), In.Mask);
    InputJson->SetBoolField(TEXT("maskR"), In.MaskR != 0);
    InputJson->SetBoolField(TEXT("maskG"), In.MaskG != 0);
    InputJson->SetBoolField(TEXT("maskB"), In.MaskB != 0);
    InputJson->SetBoolField(TEXT("maskA"), In.MaskA != 0);
    return InputJson;
}

inline TSharedPtr<FJsonObject> BuildExpressionDetailsJson(UMaterialExpression* Expr)
{
    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    if (!Expr)
    {
        return Result;
    }

    Result->SetStringField(TEXT("nodeId"), Expr->MaterialExpressionGuid.ToString());
    Result->SetStringField(TEXT("nodeType"), Expr->GetClass()->GetName());
    Result->SetStringField(TEXT("nodeName"), Expr->GetName());
    Result->SetStringField(TEXT("desc"), Expr->Desc);
    Result->SetNumberField(TEXT("x"), Expr->MaterialExpressionEditorX);
    Result->SetNumberField(TEXT("y"), Expr->MaterialExpressionEditorY);

    UMaterialExpressionMaterialFunctionCall* FuncCall = Cast<UMaterialExpressionMaterialFunctionCall>(Expr);

    // Pin names come from PinWright::MaterialPinNames, not from the raw FName fields: both
    // FExpressionOutput::OutputName and UMaterialExpression::GetInputName are NAME_None for
    // plenty of real expressions, which stringifies to the literal "None" and leaves the
    // caller with no name to pass back to connect_nodes. See Material/MaterialPinNames.h for
    // the engine derivation being reproduced.
    const TArray<FString> InputNames = PinWright::MaterialPinNames::DeriveInputPinNames(Expr);
    const TArray<FString> OutputNames = PinWright::MaterialPinNames::DeriveOutputPinNames(Expr);

    TArray<TSharedPtr<FJsonValue>> Inputs;
    ForEachExpressionInput(Expr, [&](FExpressionInput* Input, int32 Index) -> bool
    {
        if (!Input)
        {
            return false;
        }

        const FString InternalName = FindReflectedInputPropertyName(Expr, Input);
        TSharedPtr<FJsonObject> InputJson = BuildExpressionInputJson(
            InputNames.IsValidIndex(Index) ? InputNames[Index] : FString(), *Input, InternalName);

        if (FuncCall && Index < FuncCall->FunctionInputs.Num())
        {
            InputJson->SetStringField(TEXT("rawName"),
                FuncCall->FunctionInputs[Index].Input.InputName.ToString());
        }

        Inputs.Add(MakeShared<FJsonValueObject>(InputJson));
        return false;
    });
    Result->SetArrayField(TEXT("inputs"), Inputs);

    TArray<TSharedPtr<FJsonValue>> Outputs;
    const TArray<FExpressionOutput>& ExprOutputs = Expr->GetOutputs();
    for (int32 OutIndex = 0; OutIndex < ExprOutputs.Num(); ++OutIndex)
    {
        const FExpressionOutput& Out = ExprOutputs[OutIndex];
        TSharedPtr<FJsonObject> OutputJson = MakeShared<FJsonObject>();
        // index is the value connect_nodes takes as sourceOutputIndex; name is what it
        // takes as sourcePin. Emitting both means neither has to be counted by position.
        OutputJson->SetNumberField(TEXT("index"), OutIndex);
        OutputJson->SetStringField(TEXT("name"),
            OutputNames.IsValidIndex(OutIndex) ? OutputNames[OutIndex] : FString());
        OutputJson->SetNumberField(TEXT("mask"), Out.Mask);
        OutputJson->SetBoolField(TEXT("maskR"), Out.MaskR != 0);
        OutputJson->SetBoolField(TEXT("maskG"), Out.MaskG != 0);
        OutputJson->SetBoolField(TEXT("maskB"), Out.MaskB != 0);
        OutputJson->SetBoolField(TEXT("maskA"), Out.MaskA != 0);
        Outputs.Add(MakeShared<FJsonValueObject>(OutputJson));
    }
    Result->SetArrayField(TEXT("outputs"), Outputs);

    if (Expr->HasAParameterName())
    {
        Result->SetStringField(TEXT("parameterName"), Expr->GetParameterName().ToString());

        // Group / SortPriority come off the engine's single uniform virtual, not a
        // Cast<UMaterialExpressionParameter>. Several parameter families declare their own
        // Group/SortPriority without deriving from UMaterialExpressionParameter — texture
        // sample, font sample, runtime virtual texture and sparse volume texture parameters —
        // so the cast dropped both fields for every one of them while HasAParameterName()
        // still emitted parameterName, leaving a payload that named a parameter it could not
        // describe. GetParameterValue covers all of them (UMaterialExpressionParameter's own
        // override fills the same two fields), and is what material.authoring's
        // get_material_info parameters[] entries are already built from.
        FMaterialParameterMetadata Meta;
        if (Expr->GetParameterValue(Meta))
        {
            Result->SetStringField(TEXT("group"), Meta.Group.ToString());
            Result->SetNumberField(TEXT("sortPriority"), Meta.SortPriority);
        }
    }

    TSharedPtr<FJsonObject> Properties = BuildClassPropertyJson(Expr, Expr->GetClass()->GetDefaultObject());
    if (Properties.IsValid() && Properties->Values.Num() > 0)
    {
        Result->SetObjectField(TEXT("properties"), Properties);
    }

    if (UMaterialExpressionTextureSample* TexExpr = Cast<UMaterialExpressionTextureSample>(Expr))
    {
        Result->SetStringField(TEXT("texturePath"),
            TexExpr->Texture ? TexExpr->Texture->GetPathName() : FString());
        Result->SetStringField(TEXT("samplerType"),
            UEnum::GetValueAsString(TexExpr->SamplerType));
    }

    return Result;
}
}
