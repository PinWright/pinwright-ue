// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"


class UMaterial;
class UMaterialExpression;
class UMaterialExpressionNamedRerouteDeclaration;
class UMaterialFunction;

struct FMGIRExpressionSpec
{
    FString Name;
    FString ExpressionClassName;
    UClass* ExpressionClass = nullptr;
    TSharedPtr<FJsonObject> Properties;
    FVector2D Position = FVector2D::ZeroVector;
};

struct FMGIRConstantSpec
{
    FString Name;
    TArray<double> Values;
    FVector2D Position = FVector2D::ZeroVector;
};

struct FMGIRFunctionCallSpec
{
    FString Name;
    FString FunctionPath;
    FVector2D Position = FVector2D::ZeroVector;
};

struct FMGIRRerouteDeclarationSpec
{
    FString Name;
    FString SourceReference;
    FVector2D Position = FVector2D::ZeroVector;
};

struct FMGIRRerouteUsageSpec
{
    FString Name;
    FString DeclarationName;
    FVector2D Position = FVector2D::ZeroVector;
};

struct FMGIRLayerStackSpec
{
    FString Name;
    TArray<FString> LayerFunctionPaths;
    TArray<FString> BlendFunctionPaths;
    FString InputReference;
    FVector2D Position = FVector2D::ZeroVector;
};

struct FMGIRConnectionSpec
{
    FString TargetName;
    FString InputName;
    FString SourceReference;
};

struct FMGIRRootOutputSpec
{
    FString OutputName;
    FString SourceReference;
    FVector2D Position = FVector2D::ZeroVector;
};

struct FMGIREmitResult
{
    UMaterialExpression* Expression = nullptr;
    FString ErrorCode;
    FString ErrorMessage;

    bool IsSuccess() const { return Expression != nullptr && ErrorCode.IsEmpty(); }
};

class FMGIRExpressionEmitter
{
public:
    explicit FMGIRExpressionEmitter(UMaterial* InMaterial);
    explicit FMGIRExpressionEmitter(UMaterialFunction* InFunction);

    FMGIREmitResult EmitExpression(const FMGIRExpressionSpec& Spec);
    FMGIREmitResult EmitConstant(const FMGIRConstantSpec& Spec);
    FMGIREmitResult EmitFunctionCall(const FMGIRFunctionCallSpec& Spec);
    FMGIREmitResult EmitNamedRerouteDeclaration(const FMGIRRerouteDeclarationSpec& Spec);
    FMGIREmitResult EmitNamedRerouteUsage(const FMGIRRerouteUsageSpec& Spec);
    FMGIREmitResult EmitLayerStack(const FMGIRLayerStackSpec& Spec);

    FMGIREmitResult WireExpressionInput(const FMGIRConnectionSpec& Spec);
    FMGIREmitResult WireRootOutput(const FMGIRRootOutputSpec& Spec);

    UMaterialExpression* FindSymbol(const FString& Name) const;

    // Extend mode only. Indexes the expressions ALREADY in the target graph under the
    // handle names the decompiler issues for them, and arms the refusal in
    // ValidateNewSymbol. Every Emit* creates unconditionally and there is no update path,
    // so without this a document naming an existing handle silently produced a second
    // expression and reported success. Call after any graph clear, before emitting.
    void GuardExistingExpressionHandles();

private:
    UMaterial* Material = nullptr;
    UMaterialFunction* Function = nullptr;
    TMap<FString, UMaterialExpression*> Symbols;
    TMap<FString, UMaterialExpressionNamedRerouteDeclaration*> RerouteDeclarations;

    // Handle name -> expression already present in the target graph when compilation began.
    // Empty unless GuardExistingExpressionHandles armed it, which keeps Append untouched.
    TMap<FString, UMaterialExpression*> PreExistingHandles;
    bool bGuardExistingHandles = false;

    FMGIREmitResult RegisterExpression(const FString& Name, UMaterialExpression* Expression);
    bool ValidateNewSymbol(const FString& Name, FMGIREmitResult& OutError) const;
    FMGIREmitResult MakeError(const TCHAR* ErrorCode, const FString& ErrorMessage) const;
};
