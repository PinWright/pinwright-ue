// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "MGIR/MGIRDecompiler.h"


#include "IrCore/IrTextUtils.h"
#include "MGIR/MGIRDynamicInputs.h"
#include "MGIR/MGIRExpressionUtils.h"
#include "MGIR/MGIRHelpers.h"
#include "MGIR/MGIRMaterialAttributeUtils.h"
#include "MGIR/MGIRMaterialProperties.h"
#include "MGIR/MGIRSubstrateSugar.h"
#include "Engine/Texture.h"
#include "HAL/IConsoleManager.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpression.h"
#include "Materials/MaterialExpressionComposite.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Materials/MaterialExpressionConstant2Vector.h"
#include "Materials/MaterialExpressionConstant3Vector.h"
#include "Materials/MaterialExpressionConstant4Vector.h"
#include "Materials/MaterialExpressionCustomOutput.h"
#include "Materials/MaterialExpressionFunctionInput.h"
#include "Materials/MaterialExpressionFunctionOutput.h"
#include "Materials/MaterialExpressionMaterialAttributeLayers.h"
#include "Materials/MaterialExpressionMaterialFunctionCall.h"
#include "Materials/MaterialExpressionNamedReroute.h"
#include "Materials/MaterialExpressionParameter.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionSetMaterialAttributes.h"
#include "Materials/MaterialExpressionTextureBase.h"
#include "Materials/MaterialExpressionTextureSample.h"
#include "Materials/MaterialExpressionVectorParameter.h"
#include "Materials/MaterialFunction.h"
#include "Materials/MaterialFunctionInterface.h"
#include "UObject/Class.h"
#include "UObject/TextProperty.h"
#include "UObject/UnrealType.h"

namespace
{
struct FMGIREmitState
{
    TMap<const UMaterialExpression*, FString> Names;
    TSet<FString> UsedNames;
    int32 NextFallbackName = 0;

    FString NameFor(const UMaterialExpression* Expression)
    {
        if (!Expression)
        {
            return TEXT("null");
        }

        if (const FString* Existing = Names.Find(Expression))
        {
            return *Existing;
        }

        FString Candidate = MGIRHelpers::ExpressionHandleName(Expression->MaterialExpressionGuid);
        if (Candidate.IsEmpty())
        {
            Candidate = FIrTextUtils::FormatNumericLocalId(NextFallbackName++);
        }

        Candidate = MakeUniqueIdentifier(Candidate);
        Names.Add(Expression, Candidate);
        return Candidate;
    }

    FString MakeUniqueIdentifier(const FString& RawName)
    {
        FString Sanitized;
        Sanitized.Reserve(RawName.Len());
        for (int32 Index = 0; Index < RawName.Len(); ++Index)
        {
            const TCHAR Ch = RawName[Index];
            const bool bValid = FChar::IsAlnum(Ch) || Ch == TEXT('_');
            Sanitized.AppendChar(bValid ? Ch : TEXT('_'));
        }

        if (Sanitized.IsEmpty() || FChar::IsDigit(Sanitized[0]))
        {
            Sanitized = TEXT("n") + Sanitized;
        }

        FString Candidate = Sanitized;
        int32 Suffix = 2;
        while (UsedNames.Contains(Candidate))
        {
            Candidate = FString::Printf(TEXT("%s_%d"), *Sanitized, Suffix++);
        }

        UsedNames.Add(Candidate);
        return Candidate;
    }
};

// Named through MGIRHelpers so the encoder sits beside the decoder the compiler reads with:
// the two are one escape set (`\\`, `\"`, `\n`, `\r`, `\t`, `\uNNNN`) and a literal written
// here that the compiler cannot read back is a silent round-trip failure.
FString Quote(const FString& Value)
{
    return MGIRHelpers::EncodeStringLiteral(Value);
}

FString Number(float Value)
{
    return FString::SanitizeFloat(Value);
}

FString PositionSuffix(const UMaterialExpression* Expression)
{
    return FIrTextUtils::FormatPositionSuffix(
        Expression ? Expression->MaterialExpressionEditorX : 0,
        Expression ? Expression->MaterialExpressionEditorY : 0);
}

FString ExpressionClassName(const UMaterialExpression* Expression)
{
    if (!Expression)
    {
        return TEXT("None");
    }

    return FIrTextUtils::FormatNameToken(Expression->GetClass()->GetPathName());
}

FString FunctionInputTypeName(EFunctionInputType Type)
{
    switch (Type)
    {
    case FunctionInput_Scalar:
        return TEXT("Float1");
    case FunctionInput_Vector2:
        return TEXT("Float2");
    case FunctionInput_Vector3:
        return TEXT("Float3");
    case FunctionInput_Vector4:
        return TEXT("Float4");
    case FunctionInput_Texture2D:
        return TEXT("Texture2D");
    case FunctionInput_MaterialAttributes:
        return TEXT("MaterialAttributes");
    case FunctionInput_StaticBool:
    case FunctionInput_Bool:
        return TEXT("Bool");
    case FunctionInput_Substrate:
        return TEXT("Substrate");
    default:
        return TEXT("Unknown");
    }
}

FString FormatInputRef(const FExpressionInput& Input, FMGIREmitState& State)
{
    if (!Input.Expression)
    {
        return FString();
    }

    FString Ref = TEXT("%") + State.NameFor(Input.Expression);
    if (Input.OutputIndex != 0)
    {
        Ref += FString::Printf(TEXT("[%d]"), Input.OutputIndex);
    }

    if (Input.Mask != 0)
    {
        FString Mask;
        if (Input.MaskR) { Mask += TEXT("r"); }
        if (Input.MaskG) { Mask += TEXT("g"); }
        if (Input.MaskB) { Mask += TEXT("b"); }
        if (Input.MaskA) { Mask += TEXT("a"); }
        if (!Mask.IsEmpty())
        {
            Ref += TEXT(".") + Mask;
        }
    }

    return Ref;
}

void AppendConnectedInputs(UMaterialExpression* Expression, FMGIREmitState& State, TArray<FString>& OutFields)
{
    if (!Expression)
    {
        return;
    }

    ForEachExpressionInput(Expression, [&](FExpressionInput* Input, int32 Index) -> bool
    {
        if (!Input || !Input->Expression)
        {
            return false;
        }

        FString InputName = Expression->GetInputName(Index).ToString();
        if (InputName.IsEmpty() || InputName == TEXT("None"))
        {
            InputName = FString::Printf(TEXT("Input%d"), Index);
        }

        OutFields.Add(FString::Printf(TEXT("%s: %s"), *InputName, *FormatInputRef(*Input, State)));
        return false;
    });
}

void AppendReflectedExpressionProperties(
    UMaterialExpression* Expression,
    const TSet<FName>& ExplicitProperties,
    TArray<FString>& OutFields)
{
    UObject* DefaultExpression = Expression ? Expression->GetClass()->GetDefaultObject() : nullptr;
    if (!Expression || !DefaultExpression)
    {
        return;
    }

    FReflectedFieldEmitOptions Options;
    Options.FieldSeparator = TEXT(": ");
    Options.bEmitArraysAsBracketList = true;
    FIrTextUtils::AppendReflectedFields(
        Expression->GetClass(),
        Expression,
        DefaultExpression,
        Expression,
        ExplicitProperties,
        [](const FStructProperty* StructProperty)
        {
            const FName StructName = StructProperty && StructProperty->Struct ? StructProperty->Struct->GetFName() : NAME_None;
            return StructName == FName(TEXT("ExpressionInput")) || StructName == FName(TEXT("MaterialAttributesInput"));
        },
        [](FName PropertyName)
        {
            return PropertyName == FName(TEXT("MaterialExpressionEditorX"))
                || PropertyName == FName(TEXT("MaterialExpressionEditorY"));
        },
        Options,
        OutFields);
}

void AppendExpressionProperties(UMaterialExpression* Expression, TArray<FString>& OutFields)
{
    if (!Expression)
    {
        return;
    }

    TSet<FName> ExplicitProperties;

    // A dynamic-input class's pin array is carried by the named pin arguments
    // AppendConnectedInputs has already emitted (see MGIRDynamicInputs), so reflecting the
    // backing array as well restates every name and connection in a struct-literal form the
    // compiler cannot read back - and did, until this suppression: a decompiled Custom node
    // emitted `Inputs: [(InputName="UV",Input=(Expression="..."))]` beside `UV: %n04C`.
    const FName DynamicInputArrayProperty =
        MGIRDynamicInputs::GetInputArrayPropertyName(Expression->GetClass());
    if (!DynamicInputArrayProperty.IsNone())
    {
        ExplicitProperties.Add(DynamicInputArrayProperty);
    }

    if (!Expression->Desc.IsEmpty())
    {
        OutFields.Add(FString::Printf(TEXT("Desc: %s"), *Quote(Expression->Desc)));
        ExplicitProperties.Add(FName(TEXT("Desc")));
    }

    if (UMaterialExpressionParameter* Parameter = Cast<UMaterialExpressionParameter>(Expression))
    {
        OutFields.Add(FString::Printf(TEXT("ParameterName: %s"), *Quote(Parameter->ParameterName.ToString())));
        ExplicitProperties.Add(FName(TEXT("ParameterName")));
        if (!Parameter->Group.IsNone())
        {
            OutFields.Add(FString::Printf(TEXT("Group: %s"), *Quote(Parameter->Group.ToString())));
            ExplicitProperties.Add(FName(TEXT("Group")));
        }
        OutFields.Add(FString::Printf(TEXT("SortPriority: %d"), Parameter->SortPriority));
        ExplicitProperties.Add(FName(TEXT("SortPriority")));
    }

    if (UMaterialExpressionFunctionInput* FunctionInput = Cast<UMaterialExpressionFunctionInput>(Expression))
    {
        OutFields.Add(FString::Printf(TEXT("InputName: %s"), *Quote(FunctionInput->InputName.ToString())));
        ExplicitProperties.Add(FName(TEXT("InputName")));
        OutFields.Add(FString::Printf(TEXT("InputType: %s"), *Quote(FunctionInputTypeName(FunctionInput->InputType.GetValue()))));
        ExplicitProperties.Add(FName(TEXT("InputType")));
        OutFields.Add(FString::Printf(TEXT("SortPriority: %d"), FunctionInput->SortPriority));
        ExplicitProperties.Add(FName(TEXT("SortPriority")));
        if (!FunctionInput->Description.IsEmpty())
        {
            OutFields.Add(FString::Printf(TEXT("Description: %s"), *Quote(FunctionInput->Description)));
            ExplicitProperties.Add(FName(TEXT("Description")));
        }
    }

    if (UMaterialExpressionScalarParameter* ScalarParameter = Cast<UMaterialExpressionScalarParameter>(Expression))
    {
        OutFields.Add(FString::Printf(TEXT("DefaultValue: %s"), *Number(ScalarParameter->DefaultValue)));
        ExplicitProperties.Add(FName(TEXT("DefaultValue")));
    }
    else if (UMaterialExpressionVectorParameter* VectorParameter = Cast<UMaterialExpressionVectorParameter>(Expression))
    {
        const FLinearColor& Value = VectorParameter->DefaultValue;
        OutFields.Add(FString::Printf(
            TEXT("DefaultValue: [%s, %s, %s, %s]"),
            *Number(Value.R),
            *Number(Value.G),
            *Number(Value.B),
            *Number(Value.A)));
        ExplicitProperties.Add(FName(TEXT("DefaultValue")));
    }

    if (UMaterialExpressionTextureBase* TextureExpression = Cast<UMaterialExpressionTextureBase>(Expression))
    {
        if (UTexture* Texture = TextureExpression->Texture.Get())
        {
            OutFields.Add(FString::Printf(TEXT("Texture: %s"), *Quote(Texture->GetPathName())));
            ExplicitProperties.Add(FName(TEXT("Texture")));
        }
    }

    if (UMaterialExpressionTextureSample* TextureSample = Cast<UMaterialExpressionTextureSample>(Expression))
    {
        OutFields.Add(FString::Printf(TEXT("ConstCoordinate: %d"), TextureSample->ConstCoordinate));
        ExplicitProperties.Add(FName(TEXT("ConstCoordinate")));
    }

    AppendReflectedExpressionProperties(Expression, ExplicitProperties, OutFields);
}

FString FormatFieldBlock(const TArray<FString>& Fields)
{
    return FIrTextUtils::FormatFieldList(Fields);
}

FString FormatAttributeList(const TArray<FString>& Attributes)
{
    TArray<FString> FormattedAttributes;
    FormattedAttributes.Reserve(Attributes.Num());
    for (const FString& Attribute : Attributes)
    {
        FormattedAttributes.Add(FIrTextUtils::IsBareNameToken(Attribute) ? Attribute : Quote(Attribute));
    }

    return TEXT("[") + FString::Join(FormattedAttributes, TEXT(",")) + TEXT("]");
}

FString FormatConstant(UMaterialExpression* Expression, FMGIREmitState& State)
{
    const FString Name = State.NameFor(Expression);

    if (UMaterialExpressionConstant* Constant = Cast<UMaterialExpressionConstant>(Expression))
    {
        return FString::Printf(TEXT("%%%s = constant Float1(%s)%s"), *Name, *Number(Constant->R), *PositionSuffix(Expression));
    }

    if (UMaterialExpressionConstant2Vector* Constant2 = Cast<UMaterialExpressionConstant2Vector>(Expression))
    {
        return FString::Printf(
            TEXT("%%%s = constant Float2(%s, %s)%s"),
            *Name,
            *Number(Constant2->R),
            *Number(Constant2->G),
            *PositionSuffix(Expression));
    }

    if (UMaterialExpressionConstant3Vector* Constant3 = Cast<UMaterialExpressionConstant3Vector>(Expression))
    {
        const FLinearColor& Value = Constant3->Constant;
        return FString::Printf(
            TEXT("%%%s = constant Float3(%s, %s, %s)%s"),
            *Name,
            *Number(Value.R),
            *Number(Value.G),
            *Number(Value.B),
            *PositionSuffix(Expression));
    }

    UMaterialExpressionConstant4Vector* Constant4 = CastChecked<UMaterialExpressionConstant4Vector>(Expression);
    const FLinearColor& Value = Constant4->Constant;
    return FString::Printf(
        TEXT("%%%s = constant Float4(%s, %s, %s, %s)%s"),
        *Name,
        *Number(Value.R),
        *Number(Value.G),
        *Number(Value.B),
        *Number(Value.A),
        *PositionSuffix(Expression));
}

bool IsConstantExpression(UMaterialExpression* Expression)
{
    return Expression
        && (Expression->IsA<UMaterialExpressionConstant>()
            || Expression->IsA<UMaterialExpressionConstant2Vector>()
            || Expression->IsA<UMaterialExpressionConstant3Vector>()
            || Expression->IsA<UMaterialExpressionConstant4Vector>());
}

FString FormatFunctionCall(UMaterialExpressionMaterialFunctionCall* FunctionCall, FMGIREmitState& State)
{
    TArray<FString> Fields;
    for (const FFunctionExpressionInput& FunctionInput : FunctionCall->FunctionInputs)
    {
        if (!FunctionInput.Input.Expression)
        {
            continue;
        }

        FString InputName = FunctionInput.ExpressionInput
            ? FunctionInput.ExpressionInput->InputName.ToString()
            : FunctionInput.Input.InputName.ToString();
        if (InputName.IsEmpty() || InputName == TEXT("None"))
        {
            InputName = TEXT("Input");
        }

        Fields.Add(FString::Printf(TEXT("%s: %s"), *InputName, *FormatInputRef(FunctionInput.Input, State)));
    }

    const FString FunctionPath = FunctionCall->MaterialFunction
        ? FunctionCall->MaterialFunction->GetPathName()
        : FString();

    return FString::Printf(
        TEXT("%%%s = function_call %s%s%s"),
        *State.NameFor(FunctionCall),
        *FIrTextUtils::FormatNameToken(FunctionPath),
        *FormatFieldBlock(Fields),
        *PositionSuffix(FunctionCall));
}

FString FormatNamedRerouteDeclaration(UMaterialExpressionNamedRerouteDeclaration* Declaration, FMGIREmitState& State)
{
    const FString DeclName = State.NameFor(Declaration);
    TArray<FString> Fields;
    const FString SourceRef = FormatInputRef(Declaration->Input, State);
    if (!SourceRef.IsEmpty())
    {
        Fields.Add(FString::Printf(TEXT("Source: %s"), *SourceRef));
    }
    if (!Declaration->Name.IsNone())
    {
        Fields.Add(FString::Printf(TEXT("DisplayName: %s"), *Quote(Declaration->Name.ToString())));
    }

    return FString::Printf(
        TEXT("%%%s = reroute %s%s%s"),
        *DeclName,
        *DeclName,
        *FormatFieldBlock(Fields),
        *PositionSuffix(Declaration));
}

FString FormatNamedRerouteUsage(UMaterialExpressionNamedRerouteUsage* Usage, FMGIREmitState& State)
{
    const FString DeclName = Usage->Declaration
        ? State.NameFor(Usage->Declaration)
        : TEXT("unbound");

    return FString::Printf(
        TEXT("%%%s = reroute %s()%s"),
        *State.NameFor(Usage),
        *DeclName,
        *PositionSuffix(Usage));
}

FString FormatLayerStack(UMaterialExpressionMaterialAttributeLayers* LayersExpression, FMGIREmitState& State)
{
    TArray<FString> Fields;
    if (LayersExpression->Input.Expression)
    {
        Fields.Add(FString::Printf(TEXT("Input: %s"), *FormatInputRef(LayersExpression->Input, State)));
    }

    const TArray<UMaterialFunctionInterface*>& Layers = LayersExpression->GetLayers();
    for (int32 Index = 0; Index < Layers.Num(); ++Index)
    {
        if (Layers[Index])
        {
            Fields.Add(FString::Printf(TEXT("Layer%d: %s"), Index, *Quote(Layers[Index]->GetPathName())));
        }
    }

    const TArray<UMaterialFunctionInterface*>& Blends = LayersExpression->GetBlends();
    for (int32 Index = 0; Index < Blends.Num(); ++Index)
    {
        if (Blends[Index])
        {
            Fields.Add(FString::Printf(TEXT("Blend%d: %s"), Index, *Quote(Blends[Index]->GetPathName())));
        }
    }

    return FString::Printf(
        TEXT("%%%s = layer_stack %s%s%s"),
        *State.NameFor(LayersExpression),
        *State.NameFor(LayersExpression),
        *FormatFieldBlock(Fields),
        *PositionSuffix(LayersExpression));
}

FString FormatCustomOutput(UMaterialExpressionCustomOutput* CustomOutput, FMGIREmitState& State)
{
    TArray<FString> Fields;
    AppendConnectedInputs(CustomOutput, State, Fields);
    return FString::Printf(
        TEXT("%%%s = call %s%s%s"),
        *State.NameFor(CustomOutput),
        *ExpressionClassName(CustomOutput),
        *FormatFieldBlock(Fields),
        *PositionSuffix(CustomOutput));
}

FString FormatSetMaterialAttributes(UMaterialExpressionSetMaterialAttributes* SetAttributes, FMGIREmitState& State)
{
    TArray<FString> Fields;
    Fields.Add(FString::Printf(
        TEXT("Attributes: %s"),
        *FormatAttributeList(FMGIRMaterialAttributeUtils::GetStableAttributeNames(SetAttributes))));

    for (int32 InputIndex = 0; InputIndex < SetAttributes->Inputs.Num(); ++InputIndex)
    {
        FExpressionInput* Input = SetAttributes->GetInput(InputIndex);
        if (!Input || !Input->Expression)
        {
            continue;
        }

        FString InputName;
        if (InputIndex == 0)
        {
            InputName = TEXT("MaterialAttributes");
        }
        else
        {
            InputName = SetAttributes->GetInputName(InputIndex).ToString();
        }

        Fields.Add(FString::Printf(TEXT("%s: %s"), *InputName, *FormatInputRef(*Input, State)));
    }

    if (!SetAttributes->Desc.IsEmpty())
    {
        Fields.Add(FString::Printf(TEXT("Desc: %s"), *Quote(SetAttributes->Desc)));
    }

    return FString::Printf(
        TEXT("%%%s = call %s%s%s"),
        *State.NameFor(SetAttributes),
        *ExpressionClassName(SetAttributes),
        *FormatFieldBlock(Fields),
        *PositionSuffix(SetAttributes));
}

FString FormatOrdinaryCall(UMaterialExpression* Expression, FMGIREmitState& State)
{
    TArray<FString> Fields;
    AppendConnectedInputs(Expression, State, Fields);
    AppendExpressionProperties(Expression, Fields);

    return FString::Printf(
        TEXT("%%%s = call %s%s%s"),
        *State.NameFor(Expression),
        *ExpressionClassName(Expression),
        *FormatFieldBlock(Fields),
        *PositionSuffix(Expression));
}

FString FormatSubstrateSugarCall(UMaterialExpression* Expression, FMGIREmitState& State, const FString& Alias)
{
    TArray<FString> Fields;
    AppendConnectedInputs(Expression, State, Fields);
    AppendExpressionProperties(Expression, Fields);

    return FString::Printf(
        TEXT("%%%s = %s%s%s"),
        *State.NameFor(Expression),
        *Alias,
        *FormatFieldBlock(Fields),
        *PositionSuffix(Expression));
}

FString FormatExpression(
    UMaterialExpression* Expression,
    FMGIREmitState& State,
    const FMGIRDecompileOptions& Options)
{
    if (IsConstantExpression(Expression))
    {
        return FormatConstant(Expression, State);
    }

    if (UMaterialExpressionMaterialFunctionCall* FunctionCall = Cast<UMaterialExpressionMaterialFunctionCall>(Expression))
    {
        return FormatFunctionCall(FunctionCall, State);
    }

    if (UMaterialExpressionNamedRerouteDeclaration* Declaration = Cast<UMaterialExpressionNamedRerouteDeclaration>(Expression))
    {
        return FormatNamedRerouteDeclaration(Declaration, State);
    }

    if (UMaterialExpressionNamedRerouteUsage* Usage = Cast<UMaterialExpressionNamedRerouteUsage>(Expression))
    {
        return FormatNamedRerouteUsage(Usage, State);
    }

    if (UMaterialExpressionMaterialAttributeLayers* LayersExpression = Cast<UMaterialExpressionMaterialAttributeLayers>(Expression))
    {
        return FormatLayerStack(LayersExpression, State);
    }

    if (UMaterialExpressionCustomOutput* CustomOutput = Cast<UMaterialExpressionCustomOutput>(Expression))
    {
        return FormatCustomOutput(CustomOutput, State);
    }

    if (UMaterialExpressionSetMaterialAttributes* SetAttributes = Cast<UMaterialExpressionSetMaterialAttributes>(Expression))
    {
        return FormatSetMaterialAttributes(SetAttributes, State);
    }

    if (Options.bEmitSubstrateSugar)
    {
        FString Alias;
        if (FMGIRSubstrateSugar::TryGetAliasForClassPath(Expression->GetClass()->GetPathName(), Alias))
        {
            return FormatSubstrateSugarCall(Expression, State, Alias);
        }
    }

    return FormatOrdinaryCall(Expression, State);
}

void SortExpressions(TArray<UMaterialExpression*>& Expressions)
{
    MGIRExpressionUtils::SortExpressionsByDependency(Expressions);
}

void AppendBodyLine(TArray<FString>& Lines, const FString& Line)
{
    Lines.Add(TEXT("    ") + Line);
}

void AppendExpressionLines(
    const TArray<UMaterialExpression*>& Expressions,
    FMGIREmitState& State,
    const FMGIRDecompileOptions& Options,
    TArray<FString>& Lines)
{
    // Composite ("Collapse Nodes") groups are flattened at decompile so the
    // produced MGIR round-trips through the compiler, which has no subgraph
    // opcode. Inner expressions reference their producers/consumers by GUID,
    // so external connections survive as through-wires automatically.
    for (UMaterialExpression* Expression : Expressions)
    {
        if (!Expression)
        {
            continue;
        }

        if (Expression->IsA<UMaterialExpressionComposite>())
        {
            continue;
        }

        if (Expression->IsA<UMaterialExpressionFunctionOutput>())
        {
            continue;
        }

        AppendBodyLine(Lines, FormatExpression(Expression, State, Options));
    }
}

void AppendMaterialRootOutputs(UMaterial* Material, FMGIREmitState& State, TArray<FString>& Lines)
{
    if (!Material || !Material->GetEditorOnlyData())
    {
        return;
    }

    UMaterialEditorOnlyData* EditorOnly = Material->GetEditorOnlyData();

    const TPair<const TCHAR*, FExpressionInput*> RootInputs[] =
    {
        { TEXT("BaseColor"), &EditorOnly->BaseColor },
        { TEXT("Metallic"), &EditorOnly->Metallic },
        { TEXT("Specular"), &EditorOnly->Specular },
        { TEXT("Roughness"), &EditorOnly->Roughness },
        { TEXT("Anisotropy"), &EditorOnly->Anisotropy },
        { TEXT("Normal"), &EditorOnly->Normal },
        { TEXT("Tangent"), &EditorOnly->Tangent },
        { TEXT("EmissiveColor"), &EditorOnly->EmissiveColor },
        { TEXT("Opacity"), &EditorOnly->Opacity },
        { TEXT("OpacityMask"), &EditorOnly->OpacityMask },
        { TEXT("WorldPositionOffset"), &EditorOnly->WorldPositionOffset },
        { TEXT("Displacement"), &EditorOnly->Displacement },
        { TEXT("SubsurfaceColor"), &EditorOnly->SubsurfaceColor },
        { TEXT("ClearCoat"), &EditorOnly->ClearCoat },
        { TEXT("ClearCoatRoughness"), &EditorOnly->ClearCoatRoughness },
        { TEXT("AmbientOcclusion"), &EditorOnly->AmbientOcclusion },
        { TEXT("Refraction"), &EditorOnly->Refraction },
        { TEXT("MaterialAttributes"), &EditorOnly->MaterialAttributes },
        { TEXT("PixelDepthOffset"), &EditorOnly->PixelDepthOffset },
        { TEXT("ShadingModel"), &EditorOnly->ShadingModelFromMaterialExpression },
        { TEXT("SurfaceThickness"), &EditorOnly->SurfaceThickness },
        { TEXT("FrontMaterial"), &EditorOnly->FrontMaterial },
    };

    for (const TPair<const TCHAR*, FExpressionInput*>& RootInput : RootInputs)
    {
        if (!RootInput.Value || !RootInput.Value->Expression)
        {
            continue;
        }

        AppendBodyLine(Lines, FString::Printf(
            TEXT("output %s: %s"),
            RootInput.Key,
            *FormatInputRef(*RootInput.Value, State)));
    }

    for (int32 Index = 0; Index < 8; ++Index)
    {
        FExpressionInput* Input = &EditorOnly->CustomizedUVs[Index];
        if (Input->Expression)
        {
            AppendBodyLine(Lines, FString::Printf(
                TEXT("output CustomizedUV%d: %s"),
                Index,
                *FormatInputRef(*Input, State)));
        }
    }
}

void AppendFunctionInputsAndOutputs(
    const TArray<UMaterialExpression*>& Expressions,
    FMGIREmitState& State,
    TArray<FString>& Lines)
{
    for (UMaterialExpression* Expression : Expressions)
    {
        if (UMaterialExpressionFunctionOutput* FunctionOutput = Cast<UMaterialExpressionFunctionOutput>(Expression))
        {
            if (FunctionOutput->A.Expression)
            {
                AppendBodyLine(Lines, FString::Printf(
                    TEXT("output %s: %s%s"),
                    *FunctionOutput->OutputName.ToString(),
                    *FormatInputRef(FunctionOutput->A, State),
                    *PositionSuffix(FunctionOutput)));
            }
            else
            {
                AppendBodyLine(Lines, FString::Printf(
                    TEXT("output %s: <unconnected>%s"),
                    *FunctionOutput->OutputName.ToString(),
                    *PositionSuffix(FunctionOutput)));
            }
        }
    }
}

FMGIRDecompileResult MakeSuccess(TArray<FString>&& Lines)
{
    FMGIRDecompileResult Result;
    Result.bSuccess = true;
    Result.MGIRText = FString::Join(Lines, TEXT("\n"));
    return Result;
}

bool ContainsSubstrateExpression(const TArray<UMaterialExpression*>& Expressions)
{
    for (const UMaterialExpression* Expression : Expressions)
    {
        if (Expression && FMGIRSubstrateSugar::IsSubstrateExpressionClassPath(Expression->GetClass()->GetPathName()))
        {
            return true;
        }
    }

    return false;
}

void AppendDisabledSubstrateWarningIfNeeded(
    const TArray<UMaterialExpression*>& Expressions,
    FMGIRDecompileResult& Result)
{
    IConsoleVariable* SubstrateCVar = IConsoleManager::Get().FindConsoleVariable(TEXT("r.Substrate"));
    if (!SubstrateCVar || SubstrateCVar->GetInt() != 0)
    {
        return;
    }

    if (!ContainsSubstrateExpression(Expressions))
    {
        return;
    }

    Result.Warnings.Add(TEXT("Material contains Substrate expressions but r.Substrate=0; MGIR decompile emitted the graph without compiling it."));
}

void CollectReferencedFunctions(
    UMaterialFunction* Function,
    TSet<UMaterialFunction*>& Visited,
    TArray<UMaterialFunction*>& OrderedFunctions);

void CollectReferencedFunctionInterface(
    UMaterialFunctionInterface* FunctionInterface,
    TSet<UMaterialFunction*>& Visited,
    TArray<UMaterialFunction*>& OrderedFunctions)
{
    if (!FunctionInterface)
    {
        return;
    }

    CollectReferencedFunctions(FunctionInterface->GetBaseFunction(), Visited, OrderedFunctions);
}

void CollectReferencedFunctions(
    UMaterialFunction* Function,
    TSet<UMaterialFunction*>& Visited,
    TArray<UMaterialFunction*>& OrderedFunctions)
{
    if (!Function || Visited.Contains(Function))
    {
        return;
    }

    Visited.Add(Function);
    OrderedFunctions.Add(Function);

    TArray<UMaterialExpression*> Expressions;
    MGIRExpressionUtils::CopyFunctionExpressions(Function, Expressions);
    for (UMaterialExpression* Expression : Expressions)
    {
        if (UMaterialExpressionMaterialFunctionCall* FunctionCall = Cast<UMaterialExpressionMaterialFunctionCall>(Expression))
        {
            CollectReferencedFunctionInterface(FunctionCall->MaterialFunction, Visited, OrderedFunctions);
        }

        if (UMaterialExpressionMaterialAttributeLayers* LayersExpression = Cast<UMaterialExpressionMaterialAttributeLayers>(Expression))
        {
            for (UMaterialFunctionInterface* Layer : LayersExpression->GetLayers())
            {
                CollectReferencedFunctionInterface(Layer, Visited, OrderedFunctions);
            }

            for (UMaterialFunctionInterface* Blend : LayersExpression->GetBlends())
            {
                CollectReferencedFunctionInterface(Blend, Visited, OrderedFunctions);
            }
        }
    }
}
}

FMGIRDecompileResult FMGIRDecompiler::DecompileMaterial(
    UMaterial* Material,
    const FMGIRDecompileOptions& Options)
{
    if (!Material)
    {
        return FMGIRDecompileResult::MakeError(TEXT("DecompileMaterial: Material is null."));
    }

    TArray<UMaterialExpression*> Expressions;
    MGIRExpressionUtils::CopyMaterialExpressions(Material, Expressions);
    SortExpressions(Expressions);

    FMGIREmitState State;
    TArray<FString> Lines;
    Lines.Add(FString::Printf(TEXT("entry material %s {"), *FIrTextUtils::FormatNameToken(Material->GetPathName())));

    // Material-level state (blend mode, shading model, two-sided, domain, translucency
    // lighting mode, ...) comes first, above the graph. Emitted BEFORE BodyStart is taken so
    // the "# no expression graph" marker still means "no graph", not "no content" - a
    // material with properties and no expressions must keep saying so.
    for (const FString& PropertyLine : MGIRMaterialProperties::Emit(Material))
    {
        AppendBodyLine(Lines, PropertyLine);
    }

    const int32 BodyStart = Lines.Num();
    AppendExpressionLines(Expressions, State, Options, Lines);
    AppendMaterialRootOutputs(Material, State, Lines);
    if (Lines.Num() == BodyStart)
    {
        // Disambiguate intentional empty body from failed extraction (no graph vs broken dump).
        AppendBodyLine(Lines, TEXT("# no expression graph"));
    }
    Lines.Add(TEXT("}"));

    FMGIRDecompileResult Result = MakeSuccess(MoveTemp(Lines));
    AppendDisabledSubstrateWarningIfNeeded(Expressions, Result);
    return Result;
}

FMGIRDecompileResult FMGIRDecompiler::DecompileFunction(
    UMaterialFunction* Function,
    const FMGIRDecompileOptions& Options)
{
    if (!Function)
    {
        return FMGIRDecompileResult::MakeError(TEXT("DecompileFunction: Function is null."));
    }

    TArray<UMaterialExpression*> Expressions;
    MGIRExpressionUtils::CopyFunctionExpressions(Function, Expressions);
    SortExpressions(Expressions);

    FMGIREmitState State;
    TArray<FString> Lines;
    Lines.Add(FString::Printf(TEXT("entry function %s {"), *FIrTextUtils::FormatNameToken(Function->GetPathName())));
    const int32 BodyStart = Lines.Num();
    AppendExpressionLines(Expressions, State, Options, Lines);
    AppendFunctionInputsAndOutputs(Expressions, State, Lines);
    if (Lines.Num() == BodyStart)
    {
        // Disambiguate intentional empty body from failed extraction (no graph vs broken dump).
        AppendBodyLine(Lines, TEXT("# no expression graph"));
    }
    Lines.Add(TEXT("}"));

    FMGIRDecompileResult Result = MakeSuccess(MoveTemp(Lines));
    AppendDisabledSubstrateWarningIfNeeded(Expressions, Result);
    return Result;
}

FMGIRDecompileResult FMGIRDecompiler::DecompileMaterialWithReferences(
    UMaterial* Material,
    const FMGIRDecompileOptions& Options)
{
    FMGIRDecompileResult MaterialResult = DecompileMaterial(Material, Options);
    if (!MaterialResult.bSuccess)
    {
        return MaterialResult;
    }

    TSet<UMaterialFunction*> Visited;
    TArray<UMaterialFunction*> OrderedFunctions;
    TArray<UMaterialExpression*> Expressions;
    MGIRExpressionUtils::CopyMaterialExpressions(Material, Expressions);
    for (UMaterialExpression* Expression : Expressions)
    {
        if (UMaterialExpressionMaterialFunctionCall* FunctionCall = Cast<UMaterialExpressionMaterialFunctionCall>(Expression))
        {
            CollectReferencedFunctionInterface(FunctionCall->MaterialFunction, Visited, OrderedFunctions);
        }

        if (UMaterialExpressionMaterialAttributeLayers* LayersExpression = Cast<UMaterialExpressionMaterialAttributeLayers>(Expression))
        {
            for (UMaterialFunctionInterface* Layer : LayersExpression->GetLayers())
            {
                CollectReferencedFunctionInterface(Layer, Visited, OrderedFunctions);
            }

            for (UMaterialFunctionInterface* Blend : LayersExpression->GetBlends())
            {
                CollectReferencedFunctionInterface(Blend, Visited, OrderedFunctions);
            }
        }
    }

    TArray<FString> Blocks;
    Blocks.Add(MaterialResult.MGIRText);
    for (UMaterialFunction* Function : OrderedFunctions)
    {
        FMGIRDecompileResult FunctionResult = DecompileFunction(Function, Options);
        MaterialResult.Warnings.Append(FunctionResult.Warnings);
        if (FunctionResult.bSuccess)
        {
            Blocks.Add(FunctionResult.MGIRText);
        }
    }

    MaterialResult.MGIRText = FString::Join(Blocks, TEXT("\n\n"));
    return MaterialResult;
}
