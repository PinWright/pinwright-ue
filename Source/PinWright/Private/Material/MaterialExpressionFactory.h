// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

class UMaterial;
class UMaterialExpression;
class UMaterialFunction;
struct FExpressionInput;


struct FCreateResult
{
    UMaterialExpression* Expression = nullptr;
    FString ErrorCode;
    FString ErrorMessage;

    bool IsSuccess() const { return Expression != nullptr; }
};

class FMaterialExpressionFactory
{
public:
    static FCreateResult Create(
        UMaterial* Material,
        UClass* ExpressionClass,
        const TSharedPtr<FJsonObject>& Properties,
        const FVector2D& Position);

    static FCreateResult Create(
        UMaterial* Material,
        const FString& ExpressionClassName,
        const TSharedPtr<FJsonObject>& Properties,
        const FVector2D& Position);

    static FCreateResult Create(
        UMaterialFunction* Function,
        UClass* ExpressionClass,
        const TSharedPtr<FJsonObject>& Properties,
        const FVector2D& Position);

    static FCreateResult Create(
        UMaterialFunction* Function,
        const FString& ExpressionClassName,
        const TSharedPtr<FJsonObject>& Properties,
        const FVector2D& Position);

    static UClass* ResolveExpressionClass(const FString& ExpressionClassName);
    static PINWRIGHT_API FExpressionInput* FindExpressionInputByName(
        UMaterialExpression* Expression,
        const FString& InputName,
        TArray<FString>* OutCandidateNames = nullptr);
    static void DiscardExpression(UMaterial* Material, UMaterialExpression* Expression);
    static void DiscardExpression(UMaterialFunction* Function, UMaterialExpression* Expression);
};
