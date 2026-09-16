// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "MGIR/MGIRPinResolver.h"

#include "Compat/EngineVersionCompat.h"
#include "MGIR/MGIRHelpers.h"
#include "Material/MaterialExpressionFactory.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpression.h"
#include "MaterialExpressionIO.h"
#include "Material/MaterialInputIterCompat.h"
#include "Material/MaterialPinNames.h"

namespace
{
using MGIRHelpers::NormalizeSymbolName;

bool ApplyMaskText(const FString& MaskText, FMGIRPinReference& Reference)
{
    if (MaskText.IsEmpty())
    {
        return true;
    }

    Reference.bHasMask = true;
    Reference.MaskR = 0;
    Reference.MaskG = 0;
    Reference.MaskB = 0;
    Reference.MaskA = 0;

    for (int32 Index = 0; Index < MaskText.Len(); ++Index)
    {
        const TCHAR Channel = FChar::ToLower(MaskText[Index]);
        if (Channel == TEXT('r') || Channel == TEXT('x'))
        {
            Reference.MaskR = 1;
        }
        else if (Channel == TEXT('g') || Channel == TEXT('y'))
        {
            Reference.MaskG = 1;
        }
        else if (Channel == TEXT('b') || Channel == TEXT('z'))
        {
            Reference.MaskB = 1;
        }
        else if (Channel == TEXT('a') || Channel == TEXT('w'))
        {
            Reference.MaskA = 1;
        }
        else
        {
            return false;
        }
    }

    return true;
}

void CopyReferenceMask(const FMGIRPinReference& Reference, FMGIRResolvedPin& Pin)
{
    Pin.bHasMask = Reference.bHasMask;
    Pin.MaskR = Reference.MaskR;
    Pin.MaskG = Reference.MaskG;
    Pin.MaskB = Reference.MaskB;
    Pin.MaskA = Reference.MaskA;
}

void AddAvailableName(TArray<FString>& Names, const FString& Name)
{
    if (!Name.IsEmpty() && !Names.Contains(Name))
    {
        Names.Add(Name);
    }
}

TArray<FString> GetExpressionInputNames(UMaterialExpression* Expression)
{
    TArray<FString> Names;
    if (!Expression)
    {
        return Names;
    }

    // Derived names, not the raw FName: GetInputName is NAME_None on several classes, so the
    // suggestion list offered the literal "None" as a pin to try. Every name added here is one
    // FindExpressionInputByName resolves — see Material/MaterialPinNames.h.
    for (const FString& Name : PinWright::MaterialPinNames::DeriveInputPinNames(Expression))
    {
        AddAvailableName(Names, Name);
    }

    for (TFieldIterator<FProperty> It(Expression->GetClass()); It; ++It)
    {
        const FStructProperty* StructProperty = CastField<FStructProperty>(*It);
        if (StructProperty && StructProperty->Struct && StructProperty->Struct->GetFName() == FName(TEXT("ExpressionInput")))
        {
            AddAvailableName(Names, It->GetName());
        }
    }

    Names.Sort();
    return Names;
}

int32 EditDistance(const FString& A, const FString& B)
{
    const FString LowerA = A.ToLower();
    const FString LowerB = B.ToLower();

    TArray<int32> Previous;
    TArray<int32> Current;
    Previous.SetNum(LowerB.Len() + 1);
    Current.SetNum(LowerB.Len() + 1);

    for (int32 Index = 0; Index <= LowerB.Len(); ++Index)
    {
        Previous[Index] = Index;
    }

    for (int32 AIndex = 1; AIndex <= LowerA.Len(); ++AIndex)
    {
        Current[0] = AIndex;
        for (int32 BIndex = 1; BIndex <= LowerB.Len(); ++BIndex)
        {
            const int32 Cost = LowerA[AIndex - 1] == LowerB[BIndex - 1] ? 0 : 1;
            Current[BIndex] = FMath::Min3(
                Previous[BIndex] + 1,
                Current[BIndex - 1] + 1,
                Previous[BIndex - 1] + Cost);
        }
        Swap(Previous, Current);
    }

    return Previous[LowerB.Len()];
}

FString BestHint(const FString& Name, const TArray<FString>& AvailableNames)
{
    int32 BestDistance = MAX_int32;
    FString BestName;
    for (const FString& Candidate : AvailableNames)
    {
        const int32 Distance = EditDistance(Name, Candidate);
        if (Distance < BestDistance)
        {
            BestDistance = Distance;
            BestName = Candidate;
        }
    }

    return BestName;
}

FString FormatMissingInputMessage(const FString& InputName, const TArray<FString>& AvailableNames)
{
    FString Message = FString::Printf(TEXT("Input '%s' was not found."), *InputName);
    if (!AvailableNames.IsEmpty())
    {
        Message += FString::Printf(TEXT(" Available inputs: %s."), *FString::Join(AvailableNames, TEXT(", ")));
        const FString Hint = BestHint(InputName, AvailableNames);
        if (!Hint.IsEmpty())
        {
            Message += FString::Printf(TEXT(" Did you mean '%s'?"), *Hint);
        }
    }
    return Message;
}

// `CustomizedUV<n>` is index-bearing, so it cannot sit in the fixed name table below.
// The decompiler emits these lines; without this the round trip of any material using
// customized UVs failed the compile with MGIR_INPUT_NOT_FOUND.
bool TryResolveCustomizedUV(const FString& OutputName, EMaterialProperty& OutProperty)
{
    static const FString Prefix = TEXT("CustomizedUV");
    if (!OutputName.StartsWith(Prefix, ESearchCase::IgnoreCase))
    {
        return false;
    }

    const FString IndexText = OutputName.Mid(Prefix.Len());
    if (IndexText.IsEmpty() || !IndexText.IsNumeric())
    {
        return false;
    }

    const int32 Index = FCString::Atoi(*IndexText);
    if (Index < 0 || Index > (MP_CustomizedUVs7 - MP_CustomizedUVs0))
    {
        return false;
    }

    OutProperty = static_cast<EMaterialProperty>(MP_CustomizedUVs0 + Index);
    return true;
}

bool TryResolveMaterialProperty(const FString& OutputName, EMaterialProperty& OutProperty)
{
    static const TPair<const TCHAR*, EMaterialProperty> Properties[] = {
        { TEXT("EmissiveColor"), MP_EmissiveColor },
        { TEXT("Emissive"), MP_EmissiveColor },
        { TEXT("Opacity"), MP_Opacity },
        { TEXT("OpacityMask"), MP_OpacityMask },
        { TEXT("BaseColor"), MP_BaseColor },
        { TEXT("Diffuse"), MP_BaseColor },
        { TEXT("Metallic"), MP_Metallic },
        { TEXT("Specular"), MP_Specular },
        { TEXT("Roughness"), MP_Roughness },
        { TEXT("Anisotropy"), MP_Anisotropy },
        { TEXT("Normal"), MP_Normal },
        { TEXT("Tangent"), MP_Tangent },
        { TEXT("WorldPositionOffset"), MP_WorldPositionOffset },
        { TEXT("WPO"), MP_WorldPositionOffset },
        { TEXT("SubsurfaceColor"), MP_SubsurfaceColor },
        { TEXT("AmbientOcclusion"), MP_AmbientOcclusion },
        { TEXT("AO"), MP_AmbientOcclusion },
        { TEXT("Refraction"), MP_Refraction },
        { TEXT("PixelDepthOffset"), MP_PixelDepthOffset },
        { TEXT("MaterialAttributes"), MP_MaterialAttributes },
        { TEXT("FrontMaterial"), MP_FrontMaterial },
        { TEXT("SurfaceThickness"), MP_SurfaceThickness },
        { TEXT("Displacement"), MP_Displacement },
        // The engine stores the clear-coat pair and the shading-model pin in the generic
        // custom-data / shading-model slots, under names that do not match the slot names.
        // The decompiler emits all three; leaving them out here made a decompiled
        // clear-coat or from-expression-shading-model material fail its own recompile.
        { TEXT("ClearCoat"), MP_CustomData0 },
        { TEXT("ClearCoatRoughness"), MP_CustomData1 },
        { TEXT("ShadingModel"), MP_ShadingModel },
    };

    for (const TPair<const TCHAR*, EMaterialProperty>& Pair : Properties)
    {
        if (OutputName.Equals(Pair.Key, ESearchCase::IgnoreCase))
        {
            OutProperty = Pair.Value;
            return true;
        }
    }

    return TryResolveCustomizedUV(OutputName, OutProperty);
}

TArray<FString> GetMaterialOutputNames()
{
    return {
        TEXT("EmissiveColor"),
        TEXT("Opacity"),
        TEXT("OpacityMask"),
        TEXT("BaseColor"),
        TEXT("Metallic"),
        TEXT("Specular"),
        TEXT("Roughness"),
        TEXT("Anisotropy"),
        TEXT("Normal"),
        TEXT("Tangent"),
        TEXT("WorldPositionOffset"),
        TEXT("SubsurfaceColor"),
        TEXT("AmbientOcclusion"),
        TEXT("Refraction"),
        TEXT("PixelDepthOffset"),
        TEXT("MaterialAttributes"),
        TEXT("FrontMaterial"),
        TEXT("SurfaceThickness"),
        TEXT("Displacement"),
        TEXT("ClearCoat"),
        TEXT("ClearCoatRoughness"),
        TEXT("ShadingModel"),
        TEXT("CustomizedUV0..CustomizedUV7"),
    };
}
}

bool FMGIRPinResolver::ParseReference(
    const FString& Text,
    FMGIRPinReference& OutReference,
    FString& OutErrorMessage)
{
    OutReference = FMGIRPinReference();

    FString Rest = Text.TrimStartAndEnd();
    if (!Rest.StartsWith(TEXT("%")))
    {
        OutErrorMessage = FString::Printf(TEXT("MGIR pin reference '%s' must start with %%."), *Text);
        return false;
    }
    Rest.RightChopInline(1, EAllowShrinking::No);

    FString MaskText;
    int32 DotIndex = INDEX_NONE;
    if (Rest.FindLastChar(TEXT('.'), DotIndex))
    {
        MaskText = Rest.Mid(DotIndex + 1);
        Rest.LeftInline(DotIndex, EAllowShrinking::No);
        if (!ApplyMaskText(MaskText, OutReference))
        {
            OutErrorMessage = FString::Printf(TEXT("Invalid MGIR channel mask '%s' in '%s'."), *MaskText, *Text);
            return false;
        }
    }

    int32 OpenBracketIndex = INDEX_NONE;
    if (Rest.FindLastChar(TEXT('['), OpenBracketIndex))
    {
        if (!Rest.EndsWith(TEXT("]")))
        {
            OutErrorMessage = FString::Printf(TEXT("Malformed MGIR output index in '%s'."), *Text);
            return false;
        }

        const FString IndexText = Rest.Mid(OpenBracketIndex + 1, Rest.Len() - OpenBracketIndex - 2);
        if (!LexTryParseString(OutReference.OutputIndex, *IndexText) || OutReference.OutputIndex < 0)
        {
            OutErrorMessage = FString::Printf(TEXT("Invalid MGIR output index '%s' in '%s'."), *IndexText, *Text);
            return false;
        }
        Rest.LeftInline(OpenBracketIndex, EAllowShrinking::No);
    }

    OutReference.SymbolName = Rest.TrimStartAndEnd();
    if (OutReference.SymbolName.IsEmpty())
    {
        OutErrorMessage = FString::Printf(TEXT("MGIR pin reference '%s' has an empty symbol name."), *Text);
        return false;
    }

    return true;
}

FMGIRWireResult FMGIRPinResolver::ResolveReference(
    const FString& Text,
    const TMap<FString, UMaterialExpression*>& Symbols,
    FMGIRResolvedPin& OutPin)
{
    FMGIRPinReference Reference;
    FString ErrorMessage;
    if (!ParseReference(Text, Reference, ErrorMessage))
    {
        return MakeError(TEXT("MGIR_INVALID_PIN_REFERENCE"), ErrorMessage);
    }

    UMaterialExpression* Expression = LookupSymbol(Symbols, NormalizeSymbolName(Reference.SymbolName));
    if (!Expression)
    {
        return MakeError(TEXT("MGIR_SYMBOL_NOT_FOUND"),
            FString::Printf(TEXT("MGIR symbol '%%%s' was not found."), *Reference.SymbolName));
    }

    OutPin.Expression = Expression;
    OutPin.OutputIndex = Reference.OutputIndex;
    CopyReferenceMask(Reference, OutPin);
    return FMGIRWireResult();
}

void FMGIRPinResolver::ApplyResolvedPin(FExpressionInput& Input, const FMGIRResolvedPin& Source)
{
    Input.Expression = Source.Expression;
    Input.OutputIndex = Source.OutputIndex;
    if (Source.bHasMask)
    {
        Input.Mask = 1;
        Input.MaskR = Source.MaskR;
        Input.MaskG = Source.MaskG;
        Input.MaskB = Source.MaskB;
        Input.MaskA = Source.MaskA;
    }
    else
    {
        Input.Mask = 0;
        Input.MaskR = 0;
        Input.MaskG = 0;
        Input.MaskB = 0;
        Input.MaskA = 0;
    }
}

FMGIRWireResult FMGIRPinResolver::WireExpressionInput(
    UMaterialExpression* TargetExpression,
    const FString& InputName,
    const FString& SourceReference,
    const TMap<FString, UMaterialExpression*>& Symbols)
{
    if (!TargetExpression)
    {
        return MakeError(TEXT("MGIR_INVALID_TARGET"), TEXT("Target expression is required."));
    }

    FMGIRResolvedPin Source;
    FMGIRWireResult ResolveResult = ResolveReference(SourceReference, Symbols, Source);
    if (!ResolveResult.IsSuccess())
    {
        return ResolveResult;
    }

    FExpressionInput* Input = FMaterialExpressionFactory::FindExpressionInputByName(TargetExpression, InputName);
    if (!Input)
    {
        return MakeError(TEXT("MGIR_INPUT_NOT_FOUND"),
            FormatMissingInputMessage(InputName, GetExpressionInputNames(TargetExpression)));
    }

    ApplyResolvedPin(*Input, Source);
    return FMGIRWireResult();
}

FMGIRWireResult FMGIRPinResolver::WireMaterialOutput(
    UMaterial* Material,
    const FString& OutputName,
    const FMGIRResolvedPin& Source)
{
    if (!Material)
    {
        return MakeError(TEXT("MGIR_INVALID_TARGET"), TEXT("Material is required."));
    }

    EMaterialProperty Property = MP_MAX;
    if (!TryResolveMaterialProperty(OutputName, Property))
    {
        return MakeError(TEXT("MGIR_INPUT_NOT_FOUND"),
            FormatMissingInputMessage(OutputName, GetMaterialOutputNames()));
    }

    FExpressionInput* Input = Material->GetExpressionInputForProperty(Property);
    if (!Input)
    {
        return MakeError(TEXT("MGIR_INPUT_NOT_FOUND"),
            FString::Printf(TEXT("Material output '%s' is not available on this material."), *OutputName));
    }

    ApplyResolvedPin(*Input, Source);
    return FMGIRWireResult();
}
