// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "MGIR/MGIRDynamicInputs.h"

#include "Materials/MaterialExpression.h"
#include "Materials/MaterialExpressionCustom.h"

FName MGIRDynamicInputs::GetInputArrayPropertyName(const UClass* ExpressionClass)
{
    if (ExpressionClass && ExpressionClass->IsChildOf(UMaterialExpressionCustom::StaticClass()))
    {
        return GET_MEMBER_NAME_CHECKED(UMaterialExpressionCustom, Inputs);
    }

    return NAME_None;
}

bool MGIRDynamicInputs::ApplyDeclaredInputNames(
    UMaterialExpression* Expression,
    const TArray<FString>& InputNames,
    FString& OutError)
{
    UMaterialExpressionCustom* Custom = Cast<UMaterialExpressionCustom>(Expression);
    if (!Custom)
    {
        OutError = TEXT("Expression does not declare its input pins from call arguments.");
        return false;
    }

    TArray<FString> DeclaredNames;
    DeclaredNames.Reserve(InputNames.Num());
    for (const FString& InputName : InputNames)
    {
        const FString Trimmed = InputName.TrimStartAndEnd();
        if (Trimmed.IsEmpty())
        {
            OutError = TEXT("Custom input names cannot be empty; an unnamed input is skipped by the material translator.");
            return false;
        }

        const bool bDuplicate = DeclaredNames.ContainsByPredicate(
            [&Trimmed](const FString& Existing)
            {
                return Existing.Equals(Trimmed, ESearchCase::IgnoreCase);
            });
        if (bDuplicate)
        {
            OutError = FString::Printf(
                TEXT("Duplicate Custom input name '%s'; each input becomes one HLSL function parameter, so the names must be distinct."),
                *Trimmed);
            return false;
        }

        DeclaredNames.Add(Trimmed);
    }

    // Reset rather than append: the class default seeds ONE input whose name is empty, and
    // UMaterialExpressionCustom::Compile skips every unnamed input. Keeping it would leave a
    // pin the editor draws, a document can wire, and the generated shader never receives.
    Custom->Inputs.Reset(DeclaredNames.Num());
    for (const FString& DeclaredName : DeclaredNames)
    {
        FCustomInput& Input = Custom->Inputs.AddDefaulted_GetRef();
        Input.InputName = FName(*DeclaredName);
    }

    return true;
}
