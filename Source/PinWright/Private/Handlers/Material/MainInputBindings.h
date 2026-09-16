// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once
#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpression.h"
#include "MaterialExpressionIO.h"
#include "MGIR/MGIRExpressionUtils.h"

namespace PinWright::Material
{

struct FMainInputBinding
{
    const TCHAR* Name;
    FExpressionInput* (*Get)(UMaterialEditorOnlyData*);
};

inline const FMainInputBinding* GetMainInputBindings(int32& OutNum)
{
    static const FMainInputBinding Table[] = {
        { TEXT("BaseColor"),                        [](UMaterialEditorOnlyData* D) -> FExpressionInput* { return &D->BaseColor; } },
        { TEXT("Metallic"),                         [](UMaterialEditorOnlyData* D) -> FExpressionInput* { return &D->Metallic; } },
        { TEXT("Specular"),                         [](UMaterialEditorOnlyData* D) -> FExpressionInput* { return &D->Specular; } },
        { TEXT("Roughness"),                        [](UMaterialEditorOnlyData* D) -> FExpressionInput* { return &D->Roughness; } },
        { TEXT("Anisotropy"),                       [](UMaterialEditorOnlyData* D) -> FExpressionInput* { return &D->Anisotropy; } },
        { TEXT("Normal"),                           [](UMaterialEditorOnlyData* D) -> FExpressionInput* { return &D->Normal; } },
        { TEXT("Tangent"),                          [](UMaterialEditorOnlyData* D) -> FExpressionInput* { return &D->Tangent; } },
        { TEXT("EmissiveColor"),                    [](UMaterialEditorOnlyData* D) -> FExpressionInput* { return &D->EmissiveColor; } },
        { TEXT("Opacity"),                          [](UMaterialEditorOnlyData* D) -> FExpressionInput* { return &D->Opacity; } },
        { TEXT("OpacityMask"),                      [](UMaterialEditorOnlyData* D) -> FExpressionInput* { return &D->OpacityMask; } },
        { TEXT("WorldPositionOffset"),              [](UMaterialEditorOnlyData* D) -> FExpressionInput* { return &D->WorldPositionOffset; } },
        { TEXT("Displacement"),                     [](UMaterialEditorOnlyData* D) -> FExpressionInput* { return &D->Displacement; } },
        { TEXT("SubsurfaceColor"),                  [](UMaterialEditorOnlyData* D) -> FExpressionInput* { return &D->SubsurfaceColor; } },
        { TEXT("ClearCoat"),                        [](UMaterialEditorOnlyData* D) -> FExpressionInput* { return &D->ClearCoat; } },
        { TEXT("ClearCoatRoughness"),               [](UMaterialEditorOnlyData* D) -> FExpressionInput* { return &D->ClearCoatRoughness; } },
        { TEXT("AmbientOcclusion"),                 [](UMaterialEditorOnlyData* D) -> FExpressionInput* { return &D->AmbientOcclusion; } },
        { TEXT("Refraction"),                       [](UMaterialEditorOnlyData* D) -> FExpressionInput* { return &D->Refraction; } },
        { TEXT("MaterialAttributes"),               [](UMaterialEditorOnlyData* D) -> FExpressionInput* { return &D->MaterialAttributes; } },
        { TEXT("PixelDepthOffset"),                 [](UMaterialEditorOnlyData* D) -> FExpressionInput* { return &D->PixelDepthOffset; } },
        { TEXT("ShadingModelFromMaterialExpression"), [](UMaterialEditorOnlyData* D) -> FExpressionInput* { return &D->ShadingModelFromMaterialExpression; } },
        { TEXT("SurfaceThickness"),                 [](UMaterialEditorOnlyData* D) -> FExpressionInput* { return &D->SurfaceThickness; } },
        { TEXT("FrontMaterial"),                    [](UMaterialEditorOnlyData* D) -> FExpressionInput* { return &D->FrontMaterial; } },
    };
    OutNum = sizeof(Table) / sizeof(Table[0]);
    return Table;
}

inline FExpressionInput* ResolveMainInput(UMaterialEditorOnlyData* Data, const FString& InputName)
{
    if (!Data) return nullptr;
    static const FString CustomizedPrefix = TEXT("CustomizedUVs[");
    if (InputName.StartsWith(CustomizedPrefix, ESearchCase::IgnoreCase) && InputName.EndsWith(TEXT("]")))
    {
        const FString Inner = InputName.Mid(CustomizedPrefix.Len(), InputName.Len() - CustomizedPrefix.Len() - 1);
        int32 Index = -1;
        if (Inner.IsNumeric() && LexTryParseString(Index, *Inner) && Index >= 0 && Index < UE_ARRAY_COUNT(Data->CustomizedUVs))
        {
            return &Data->CustomizedUVs[Index];
        }
        return nullptr;
    }
    int32 Num = 0;
    const FMainInputBinding* Bindings = GetMainInputBindings(Num);
    for (int32 i = 0; i < Num; ++i)
    {
        if (InputName.Equals(Bindings[i].Name, ESearchCase::IgnoreCase))
        {
            return Bindings[i].Get(Data);
        }
    }
    return nullptr;
}

inline FString GetValidMainInputNames()
{
    int32 Num = 0;
    const FMainInputBinding* Bindings = GetMainInputBindings(Num);
    TArray<FString> Names;
    Names.Reserve(Num + 1);
    for (int32 i = 0; i < Num; ++i)
    {
        Names.Add(Bindings[i].Name);
    }
    Names.Add(TEXT("CustomizedUVs[0..7]"));
    return FString::Join(Names, TEXT(", "));
}

// Human-readable name for the material's (first) shading model. get_material_info / create_material
// accept a shadingModel string but the readback never surfaced it; this closes that asymmetry.
inline FString GetShadingModelString(const UMaterial* Material)
{
    if (!Material)
    {
        return FString();
    }
    const FMaterialShadingModelField Models = Material->GetShadingModels();
    if (!Models.IsValid())
    {
        // A material with no shading model set (e.g. MaterialAttributes-driven) — report
        // nothing rather than asserting in GetFirstShadingModel's check(IsValid()).
        return TEXT("None");
    }
    const EMaterialShadingModel Model = Models.GetFirstShadingModel();
    switch (Model)
    {
    case MSM_Unlit:               return TEXT("Unlit");
    case MSM_DefaultLit:          return TEXT("DefaultLit");
    case MSM_Subsurface:          return TEXT("Subsurface");
    case MSM_PreintegratedSkin:   return TEXT("PreintegratedSkin");
    case MSM_ClearCoat:           return TEXT("ClearCoat");
    case MSM_SubsurfaceProfile:   return TEXT("SubsurfaceProfile");
    case MSM_TwoSidedFoliage:     return TEXT("TwoSidedFoliage");
    case MSM_Hair:                return TEXT("Hair");
    case MSM_Cloth:               return TEXT("Cloth");
    case MSM_Eye:                 return TEXT("Eye");
    case MSM_SingleLayerWater:    return TEXT("SingleLayerWater");
    case MSM_ThinTranslucent:     return TEXT("ThinTranslucent");
    default:                      return TEXT("Unknown");
    }
}

// Build the JSON array of the main material node's input pins. Each entry uses the same per-input
// shape BuildExpressionDetailsJson emits for ordinary expression nodes (via the shared
// MGIRExpressionUtils::BuildExpressionInputJson), but unlike that function — which lists every pin,
// including unwired ones — this surfaces ONLY wired inputs, plus an entry for every connected
// CustomizedUVs[N] slot, so the caller can read back "what feeds BaseColor/EmissiveColor/..."
// without decompiling the graph.
inline TArray<TSharedPtr<FJsonValue>> BuildMainNodeInputsJson(UMaterialEditorOnlyData* Data)
{
    TArray<TSharedPtr<FJsonValue>> Inputs;
    if (!Data)
    {
        return Inputs;
    }

    auto AppendInput = [&Inputs](const FString& Name, const FExpressionInput& In)
    {
        if (!In.Expression)
        {
            return; // Only surface wired inputs, matching what a graph readback would show.
        }
        Inputs.Add(MakeShared<FJsonValueObject>(MGIRExpressionUtils::BuildExpressionInputJson(Name, In)));
    };

    int32 Num = 0;
    const FMainInputBinding* Bindings = GetMainInputBindings(Num);
    for (int32 i = 0; i < Num; ++i)
    {
        AppendInput(Bindings[i].Name, *Bindings[i].Get(Data));
    }

    for (int32 i = 0; i < UE_ARRAY_COUNT(Data->CustomizedUVs); ++i)
    {
        AppendInput(FString::Printf(TEXT("CustomizedUVs[%d]"), i), Data->CustomizedUVs[i]);
    }

    return Inputs;
}

// Build a node-detail-style payload for the main material output node. The main node is not a
// UMaterialExpression, so FindExpressionByIdOrName can never resolve it; the write RPCs
// (connect_nodes / break_connections) already accept the documented "Main"/empty sentinel, and
// this lets the read RPCs (get_material_node_details, material.graph.get_node_details) answer the
// matching "is BaseColor/EmissiveColor wired?" question with the same vocabulary.
inline TSharedPtr<FJsonObject> BuildMainNodeDetailsJson(UMaterial* Material)
{
    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    if (!Material)
    {
        return Result;
    }

    Result->SetStringField(TEXT("nodeId"), TEXT("Main"));
    Result->SetStringField(TEXT("nodeType"), TEXT("MainMaterialOutput"));
    Result->SetStringField(TEXT("nodeName"), TEXT("Main"));
    Result->SetBoolField(TEXT("isMainOutput"), true);
    Result->SetStringField(TEXT("shadingModel"), GetShadingModelString(Material));
    Result->SetArrayField(TEXT("inputs"), BuildMainNodeInputsJson(Material->GetEditorOnlyData()));
    Result->SetArrayField(TEXT("outputs"), TArray<TSharedPtr<FJsonValue>>());
    return Result;
}

} // namespace
