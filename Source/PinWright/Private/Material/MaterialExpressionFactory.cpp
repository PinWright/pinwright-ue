// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Material/MaterialExpressionFactory.h"


#include "Handlers/ErrorCodes.h"
#include "PinWrightHelpers.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpression.h"
#include "Materials/MaterialExpressionAdd.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Materials/MaterialExpressionConstant3Vector.h"
#include "Materials/MaterialExpressionFunctionInput.h"
#include "Materials/MaterialExpressionFunctionOutput.h"
#include "Materials/MaterialExpressionMaterialFunctionCall.h"
#include "Materials/MaterialExpressionMultiply.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionTextureSample.h"
#include "Materials/MaterialExpressionVectorParameter.h"
#include "Materials/MaterialFunction.h"
#include "MaterialExpressionIO.h"
#include "Material/MaterialInputIterCompat.h"
#include "Material/MaterialPinNames.h"
#include "Utils/PropertyUtils.h"

namespace
{
FCreateResult MakeCreateError(const TCHAR* ErrorCode, const FString& ErrorMessage)
{
    FCreateResult Result;
    Result.ErrorCode = ErrorCode;
    Result.ErrorMessage = ErrorMessage;
    return Result;
}

FCreateResult MakeClassNotInstantiableError(const UClass* ExpressionClass)
{
    return MakeCreateError(ErrorCodes::ERR_CLASS_NOT_INSTANTIABLE,
        FString::Printf(TEXT("Expression class '%s' cannot be instantiated because it is abstract, deprecated, or obsolete/superseded by a newer class version."),
            *ExpressionClass->GetName()));
}

bool IsExpressionClassNotInstantiable(const UClass* ExpressionClass)
{
    return ExpressionClass
        && ExpressionClass->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists);
}
}

void FMaterialExpressionFactory::DiscardExpression(UMaterial* Material, UMaterialExpression* Expression)
{
    if (!Expression)
    {
        return;
    }

    if (Material && Material->GetEditorOnlyData())
    {
        Material->GetEditorOnlyData()->ExpressionCollection.Expressions.Remove(Expression);
    }

    Expression->Rename(nullptr, GetTransientPackage(), REN_DontCreateRedirectors | REN_NonTransactional);
    Expression->MarkAsGarbage();
}

void FMaterialExpressionFactory::DiscardExpression(UMaterialFunction* Function, UMaterialExpression* Expression)
{
    if (!Expression)
    {
        return;
    }

    if (Function && Function->GetEditorOnlyData())
    {
        Function->GetEditorOnlyData()->ExpressionCollection.Expressions.Remove(Expression);
    }

    Expression->Rename(nullptr, GetTransientPackage(), REN_DontCreateRedirectors | REN_NonTransactional);
    Expression->MarkAsGarbage();
}

FExpressionInput* FMaterialExpressionFactory::FindExpressionInputByName(
    UMaterialExpression* Expression,
    const FString& InputName,
    TArray<FString>* OutCandidateNames)
{
    if (OutCandidateNames)
    {
        OutCandidateNames->Reset();
    }
    if (!Expression)
    {
        return nullptr;
    }

    // Pair each reflected property name with its exact cached input pointer. Some expressions use
    // a different display vocabulary (StaticSwitchParameter exposes A/B as True/False), and both
    // names must resolve to the same field rather than relying on their text or index to agree.
    TMap<FExpressionInput*, FString> InternalNames;
    for (TFieldIterator<FProperty> It(Expression->GetClass()); It; ++It)
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
            FString InternalName = It->GetName();
            if (StructProperty->ArrayDim > 1 && ArrayIndex > 0)
            {
                InternalName += FString::Printf(TEXT("_%d"), ArrayIndex);
            }
            InternalNames.Add(
                StructProperty->ContainerPtrToValuePtr<FExpressionInput>(Expression, ArrayIndex),
                MoveTemp(InternalName));
        }
    }

    const TArray<FString> DisplayNames = PinWright::MaterialPinNames::DeriveInputPinNames(Expression);
    FExpressionInput* Found = nullptr;
    UMaterialExpressionMaterialFunctionCall* FuncCall = Cast<UMaterialExpressionMaterialFunctionCall>(Expression);
    ForEachExpressionInput(Expression, [&](FExpressionInput* Input, int32 Index) -> bool
    {
        if (!Input)
        {
            return false;
        }

        TArray<FString, TInlineAllocator<3>> Aliases;
        if (DisplayNames.IsValidIndex(Index))
        {
            Aliases.Add(DisplayNames[Index]);
        }
        if (FuncCall)
        {
            Aliases.Add(FuncCall->GetInputNameWithType(Index, /*bWithType=*/false).ToString());
        }
        if (const FString* InternalName = InternalNames.Find(Input))
        {
            Aliases.Add(*InternalName);
        }

        for (const FString& Alias : Aliases)
        {
            if (Alias.IsEmpty() || Alias == TEXT("None"))
            {
                continue;
            }
            if (OutCandidateNames && !OutCandidateNames->ContainsByPredicate(
                [&Alias](const FString& Existing)
                {
                    return Existing.Equals(Alias, ESearchCase::IgnoreCase);
                }))
            {
                OutCandidateNames->Add(Alias);
            }
            if (!Found && Alias.Equals(InputName, ESearchCase::IgnoreCase))
            {
                Found = Input;
            }
        }
        return false;
    });
    if (Found)
    {
        return Found;
    }

    // Last-resort compatibility path for reflected inputs that the expression's cached/visible
    // input iterator omits (for example TextureSample.TextureObject when bShowTextureInputPin is
    // false). Keep this after the display/internal alias scan so an alias that resolves through
    // the richer map retains its existing pointer and candidate ordering.
    for (TFieldIterator<FProperty> It(Expression->GetClass()); It; ++It)
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
            FString InternalName = It->GetName();
            if (StructProperty->ArrayDim > 1 && ArrayIndex > 0)
            {
                InternalName += FString::Printf(TEXT("_%d"), ArrayIndex);
            }

            if (OutCandidateNames && !OutCandidateNames->ContainsByPredicate(
                [&InternalName](const FString& Existing)
                {
                    return Existing.Equals(InternalName, ESearchCase::IgnoreCase);
                }))
            {
                OutCandidateNames->Add(InternalName);
            }

            if (InternalName.Equals(InputName, ESearchCase::IgnoreCase))
            {
                return StructProperty->ContainerPtrToValuePtr<FExpressionInput>(Expression, ArrayIndex);
            }
        }
    }

    return nullptr;
}

UClass* FMaterialExpressionFactory::ResolveExpressionClass(const FString& ExpressionClassName)
{
    if (ExpressionClassName == TEXT("TextureSample"))
    {
        return UMaterialExpressionTextureSample::StaticClass();
    }
    if (ExpressionClassName == TEXT("VectorParameter") || ExpressionClassName == TEXT("ConstantVectorParameter"))
    {
        return UMaterialExpressionVectorParameter::StaticClass();
    }
    if (ExpressionClassName == TEXT("ScalarParameter") || ExpressionClassName == TEXT("ConstantScalarParameter"))
    {
        return UMaterialExpressionScalarParameter::StaticClass();
    }
    if (ExpressionClassName == TEXT("Add"))
    {
        return UMaterialExpressionAdd::StaticClass();
    }
    if (ExpressionClassName == TEXT("Multiply"))
    {
        return UMaterialExpressionMultiply::StaticClass();
    }
    if (ExpressionClassName == TEXT("FunctionInput"))
    {
        return UMaterialExpressionFunctionInput::StaticClass();
    }
    if (ExpressionClassName == TEXT("FunctionOutput"))
    {
        return UMaterialExpressionFunctionOutput::StaticClass();
    }
    if (ExpressionClassName == TEXT("Constant") || ExpressionClassName == TEXT("Float") || ExpressionClassName == TEXT("Scalar"))
    {
        return UMaterialExpressionConstant::StaticClass();
    }
    if (ExpressionClassName == TEXT("Constant3Vector") || ExpressionClassName == TEXT("ConstantVector") ||
        ExpressionClassName == TEXT("Color") || ExpressionClassName == TEXT("Vector3"))
    {
        return UMaterialExpressionConstant3Vector::StaticClass();
    }

    if (UClass* ExpressionClass = ResolveClassByName(ExpressionClassName))
    {
        if (ExpressionClass->IsChildOf(UMaterialExpression::StaticClass()))
        {
            return ExpressionClass;
        }
    }

    const FString PrefixedName = FString::Printf(TEXT("MaterialExpression%s"), *ExpressionClassName);
    if (UClass* ExpressionClass = ResolveClassByName(PrefixedName))
    {
        if (ExpressionClass->IsChildOf(UMaterialExpression::StaticClass()))
        {
            return ExpressionClass;
        }
    }

    return nullptr;
}

FCreateResult FMaterialExpressionFactory::Create(
    UMaterial* Material,
    const FString& ExpressionClassName,
    const TSharedPtr<FJsonObject>& Properties,
    const FVector2D& Position)
{
    UClass* ExpressionClass = ResolveExpressionClass(ExpressionClassName);
    if (!ExpressionClass)
    {
        return MakeCreateError(TEXT("CLASS_NOT_FOUND"),
            FString::Printf(TEXT("Unknown expression class: %s. Try using the full class name like 'MaterialExpressionAdd' or 'Add'."),
                *ExpressionClassName));
    }

    return Create(Material, ExpressionClass, Properties, Position);
}

FCreateResult FMaterialExpressionFactory::Create(
    UMaterial* Material,
    UClass* ExpressionClass,
    const TSharedPtr<FJsonObject>& Properties,
    const FVector2D& Position)
{
    if (!Material)
    {
        return MakeCreateError(TEXT("INVALID_ARGUMENT"), TEXT("Material is required."));
    }
    if (!ExpressionClass || !ExpressionClass->IsChildOf(UMaterialExpression::StaticClass()))
    {
        return MakeCreateError(TEXT("CLASS_NOT_FOUND"), TEXT("Expression class must derive from UMaterialExpression."));
    }
    if (IsExpressionClassNotInstantiable(ExpressionClass))
    {
        return MakeClassNotInstantiableError(ExpressionClass);
    }

    TMap<FString, FProperty*> ResolvedProperties;
    if (Properties.IsValid())
    {
        for (const TPair<FString, TSharedPtr<FJsonValue>> Pair : Properties->Values)
        {
            FProperty* Property = FindPropertyCI(ExpressionClass, Pair.Key);
            if (!Property)
            {
                return MakeCreateError(TEXT("PROPERTY_NOT_FOUND"),
                    FString::Printf(TEXT("Property '%s' was not found on %s."),
                        *Pair.Key, *ExpressionClass->GetName()));
            }
            ResolvedProperties.Add(Pair.Key, Property);
        }
    }

    UMaterialExpression* NewExpr = NewObject<UMaterialExpression>(
        Material, ExpressionClass, NAME_None, RF_Transactional);
    if (!NewExpr)
    {
        return MakeCreateError(TEXT("CREATE_FAILED"), TEXT("Failed to create expression."));
    }

    // Owning-material back-pointer. UMaterialExpression::PostEditChangeProperty forwards an
    // expression-level edit to the material only when this is set; left null, a later write to any
    // property of this node (property.set, a typed setter, a hand edit) lands on the expression and
    // the material never recompiles, so the change silently never appears. Outering to the material
    // is not enough — the engine reads this field, not GetOuter(). Assigned exactly as
    // UMaterialEditingLibrary::CreateMaterialExpressionEx does.
    NewExpr->Material = Material;

    NewExpr->MaterialExpressionEditorX = static_cast<int32>(Position.X);
    NewExpr->MaterialExpressionEditorY = static_cast<int32>(Position.Y);

    if (Properties.IsValid())
    {
        for (const TPair<FString, TSharedPtr<FJsonValue>> Pair : Properties->Values)
        {
            FProperty* Property = ResolvedProperties.FindRef(Pair.Key);

            FString Error;
            if (!ApplyJsonValueToProperty(NewExpr, Property, Pair.Value, Error))
            {
                DiscardExpression(Material, NewExpr);
                return MakeCreateError(TEXT("PROPERTY_TYPE_MISMATCH"),
                    FString::Printf(TEXT("Failed to set property '%s' on %s: %s"),
                        *Pair.Key, *ExpressionClass->GetName(), *Error));
            }
        }
    }

    if (UMaterialExpressionFunctionOutput* Output = Cast<UMaterialExpressionFunctionOutput>(NewExpr))
    {
        Output->ConditionallyGenerateId(false);
    }
    else if (UMaterialExpressionFunctionInput* FunctionInput = Cast<UMaterialExpressionFunctionInput>(NewExpr))
    {
        // Same persistent-identity contract as the output above, and the half that was missed:
        // a caller caches FFunctionExpressionInput::ExpressionInputId and re-links by it in
        // UpdateFromFunctionResource, so an uninitialised Id is minted fresh by PostLoad on every
        // load and the caller's wire into this pin is dropped without a compile error.
        FunctionInput->ConditionallyGenerateId(false);
    }

    if (Material->GetEditorOnlyData())
    {
        Material->GetEditorOnlyData()->ExpressionCollection.Expressions.Add(NewExpr);
    }

    FCreateResult Result;
    Result.Expression = NewExpr;
    return Result;
}

FCreateResult FMaterialExpressionFactory::Create(
    UMaterialFunction* Function,
    const FString& ExpressionClassName,
    const TSharedPtr<FJsonObject>& Properties,
    const FVector2D& Position)
{
    UClass* ExpressionClass = ResolveExpressionClass(ExpressionClassName);
    if (!ExpressionClass)
    {
        return MakeCreateError(TEXT("CLASS_NOT_FOUND"),
            FString::Printf(TEXT("Unknown expression class: %s. Try using the full class name like 'MaterialExpressionAdd' or 'Add'."),
                *ExpressionClassName));
    }

    return Create(Function, ExpressionClass, Properties, Position);
}

FCreateResult FMaterialExpressionFactory::Create(
    UMaterialFunction* Function,
    UClass* ExpressionClass,
    const TSharedPtr<FJsonObject>& Properties,
    const FVector2D& Position)
{
    if (!Function)
    {
        return MakeCreateError(TEXT("INVALID_ARGUMENT"), TEXT("Material function is required."));
    }
    if (!ExpressionClass || !ExpressionClass->IsChildOf(UMaterialExpression::StaticClass()))
    {
        return MakeCreateError(TEXT("CLASS_NOT_FOUND"), TEXT("Expression class must derive from UMaterialExpression."));
    }
    if (IsExpressionClassNotInstantiable(ExpressionClass))
    {
        return MakeClassNotInstantiableError(ExpressionClass);
    }

    TMap<FString, FProperty*> ResolvedProperties;
    if (Properties.IsValid())
    {
        for (const TPair<FString, TSharedPtr<FJsonValue>> Pair : Properties->Values)
        {
            FProperty* Property = FindPropertyCI(ExpressionClass, Pair.Key);
            if (!Property)
            {
                return MakeCreateError(TEXT("PROPERTY_NOT_FOUND"),
                    FString::Printf(TEXT("Property '%s' was not found on %s."),
                        *Pair.Key, *ExpressionClass->GetName()));
            }
            ResolvedProperties.Add(Pair.Key, Property);
        }
    }

    UMaterialExpression* NewExpr = NewObject<UMaterialExpression>(
        Function, ExpressionClass, NAME_None, RF_Transactional);
    if (!NewExpr)
    {
        return MakeCreateError(TEXT("CREATE_FAILED"), TEXT("Failed to create expression."));
    }

    // Function-graph counterpart of the Material back-pointer set in the UMaterial overload above:
    // UMaterialExpression::PostEditChangeProperty falls through to Function->PostEditChangeProperty
    // when Material is null, so a function-owned expression needs Function to forward its edits.
    // Material stays null — that is the pair the material editor itself writes back when it saves a
    // function graph, and parameter-name validation / named-reroute lookup key off the same pair.
    NewExpr->Function = Function;

    NewExpr->MaterialExpressionEditorX = static_cast<int32>(Position.X);
    NewExpr->MaterialExpressionEditorY = static_cast<int32>(Position.Y);

    if (Properties.IsValid())
    {
        for (const TPair<FString, TSharedPtr<FJsonValue>> Pair : Properties->Values)
        {
            FProperty* Property = ResolvedProperties.FindRef(Pair.Key);

            FString Error;
            if (!ApplyJsonValueToProperty(NewExpr, Property, Pair.Value, Error))
            {
                DiscardExpression(Function, NewExpr);
                return MakeCreateError(TEXT("PROPERTY_TYPE_MISMATCH"),
                    FString::Printf(TEXT("Failed to set property '%s' on %s: %s"),
                        *Pair.Key, *ExpressionClass->GetName(), *Error));
            }
        }
    }

    if (UMaterialExpressionFunctionOutput* Output = Cast<UMaterialExpressionFunctionOutput>(NewExpr))
    {
        Output->ConditionallyGenerateId(false);
    }
    else if (UMaterialExpressionFunctionInput* FunctionInput = Cast<UMaterialExpressionFunctionInput>(NewExpr))
    {
        // See the UMaterial overload above: an input pin without a persistent Id disconnects
        // every already-saved caller of this function on its next load.
        FunctionInput->ConditionallyGenerateId(false);
    }

    if (Function->GetEditorOnlyData())
    {
        Function->GetEditorOnlyData()->ExpressionCollection.Expressions.Add(NewExpr);
    }

    FCreateResult Result;
    Result.Expression = NewExpr;
    return Result;
}
