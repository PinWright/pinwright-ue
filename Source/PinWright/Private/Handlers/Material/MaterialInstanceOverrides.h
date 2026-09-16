// Copyright (c) 2026 Alexander Penkin. MIT License.

// Shared apply path for type-keyed material-instance parameter overrides.
// Header-only (inline) so both the material.authoring.* cluster
// (MaterialAuthoringHandler.cpp) and the legacy asset.* creator
// (AssetMaterialHandler.cpp) route override-application through ONE
// implementation across translation units — the convergence
// E-create-material-instance-duplicate-divergent-shape asks for. Mirrors the
// other header-only Material/ helpers (MaterialHandlerUtils.h, MaterialFinders.h).
#pragma once

#include "CoreMinimal.h"
#include "Compat/JsonKeyCompat.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/Texture.h"
#include "Materials/MaterialInstance.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Utils/GuardedLoad.h"
// FMaterialParameterInfo/Value/Metadata moved from the top-level MaterialTypes.h
// into Materials/MaterialParameters.h in UE 5.7; on 5.6 and earlier they live in MaterialTypes.h.
#if __has_include("Materials/MaterialParameters.h")
#include "Materials/MaterialParameters.h"
#else
#include "MaterialTypes.h"
#endif

// Apply a type-keyed override object — {scalar:{Name:num}, vector:{Name:{r,g,b,a}},
// texture:{Name:assetPath}, staticSwitch:{Name:bool}} — onto a freshly loaded or
// freshly created UMaterialInstanceConstant. Shared by the batch
// material.authoring.set_material_instance_parameters verb and the inline `parameters`
// slot on material.authoring.create_material_instance so both speak the SAME override shape
// (the one E-create-material-instance-duplicate-divergent-shape asks the create
// verbs to converge on). All sets are wrapped in a single
// FMaterialInstanceParameterUpdateContext so one PostEditChange / shader-permutation
// rebuild covers the whole batch (a per-set rebuild would be O(N) recompiles, and the
// static-switch set MUST go through the context — a direct Instance->Set... is
// discarded when the context dtor commits its snapshot). Caller is responsible for
// MarkPackageDirty / save and for the surrounding SendSuccess.
inline void ApplyMaterialInstanceParameterOverrides(
    UMaterialInstanceConstant* Instance,
    const TSharedPtr<FJsonObject>& ParamsObj,
    TArray<TSharedPtr<FJsonValue>>& Applied,
    TArray<TSharedPtr<FJsonValue>>& Failed)
{
    if (!Instance || !ParamsObj.IsValid()) return;

    auto MakeFailureEntry = [](const FString& Type, const FString& Name, const FString& Reason)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetStringField(TEXT("type"), Type);
        Obj->SetStringField(TEXT("name"), Name);
        Obj->SetStringField(TEXT("reason"), Reason);
        return MakeShared<FJsonValueObject>(Obj);
    };
    auto MakeAppliedEntry = [](const FString& Type, const FString& Name)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetStringField(TEXT("type"), Type);
        Obj->SetStringField(TEXT("name"), Name);
        return MakeShared<FJsonValueObject>(Obj);
    };

    FMaterialInstanceParameterUpdateContext UpdateCtx(Instance, EMaterialInstanceClearParameterFlag::None);

    const TSharedPtr<FJsonObject>* ScalarMap = nullptr;
    if (ParamsObj->TryGetObjectField(TEXT("scalar"), ScalarMap) && ScalarMap && (*ScalarMap).IsValid())
    {
        for (const auto& Pair : (*ScalarMap)->Values)
        {
            double NumVal = 0.0;
            if (Pair.Value.IsValid() && Pair.Value->TryGetNumber(NumVal))
            {
                Instance->SetScalarParameterValueEditorOnly(FMaterialParameterInfo(FName(*Pair.Key)), (float)NumVal);
                Applied.Add(MakeAppliedEntry(TEXT("scalar"), EARGCompat::JsonKeyToString(Pair.Key)));
            }
            else
            {
                Failed.Add(MakeFailureEntry(TEXT("scalar"), EARGCompat::JsonKeyToString(Pair.Key), TEXT("value is not a number")));
            }
        }
    }

    const TSharedPtr<FJsonObject>* VectorMap = nullptr;
    if (ParamsObj->TryGetObjectField(TEXT("vector"), VectorMap) && VectorMap && (*VectorMap).IsValid())
    {
        for (const auto& Pair : (*VectorMap)->Values)
        {
            if (Pair.Value.IsValid() && Pair.Value->Type == EJson::Object)
            {
                const TSharedPtr<FJsonObject> ColorObj = Pair.Value->AsObject();
                double R = 1.0, G = 1.0, B = 1.0, A = 1.0;
                ColorObj->TryGetNumberField(TEXT("r"), R);
                ColorObj->TryGetNumberField(TEXT("g"), G);
                ColorObj->TryGetNumberField(TEXT("b"), B);
                ColorObj->TryGetNumberField(TEXT("a"), A);
                Instance->SetVectorParameterValueEditorOnly(
                    FMaterialParameterInfo(FName(*Pair.Key)),
                    FLinearColor((float)R, (float)G, (float)B, (float)A));
                Applied.Add(MakeAppliedEntry(TEXT("vector"), EARGCompat::JsonKeyToString(Pair.Key)));
            }
            else
            {
                Failed.Add(MakeFailureEntry(TEXT("vector"), EARGCompat::JsonKeyToString(Pair.Key), TEXT("value is not an object {r,g,b,a}")));
            }
        }
    }

    const TSharedPtr<FJsonObject>* TextureMap = nullptr;
    if (ParamsObj->TryGetObjectField(TEXT("texture"), TextureMap) && TextureMap && (*TextureMap).IsValid())
    {
        for (const auto& Pair : (*TextureMap)->Values)
        {
            FString TexPath;
            if (Pair.Value.IsValid() && Pair.Value->TryGetString(TexPath))
            {
                // GUARDED, and this is the shape no key-based gate could ever reach: the path is
                // the VALUE half of a `texture: {ParamName: AssetPath}` map. The dispatch-boundary
                // type gate reads top-level params, and FParamSpec::NestedKeys names KEYS - so a
                // doubled slash here met nothing before this line. Board
                // B-nested-path-values-reach-createpackage-fatal.
                FString TexRefusal;
                UTexture* Tex = PinWrightGuardedLoad::LoadObjectChecked<UTexture>(TexPath, &TexRefusal);
                if (Tex)
                {
                    Instance->SetTextureParameterValueEditorOnly(FMaterialParameterInfo(FName(*Pair.Key)), Tex);
                    Applied.Add(MakeAppliedEntry(TEXT("texture"), EARGCompat::JsonKeyToString(Pair.Key)));
                }
                else
                {
                    Failed.Add(MakeFailureEntry(TEXT("texture"), EARGCompat::JsonKeyToString(Pair.Key),
                        TexRefusal.IsEmpty() ? TEXT("texture asset not found") : *TexRefusal));
                }
            }
            else
            {
                Failed.Add(MakeFailureEntry(TEXT("texture"), EARGCompat::JsonKeyToString(Pair.Key), TEXT("value is not a string asset path")));
            }
        }
    }

    const TSharedPtr<FJsonObject>* StaticSwitchMap = nullptr;
    if (ParamsObj->TryGetObjectField(TEXT("staticSwitch"), StaticSwitchMap) && StaticSwitchMap && (*StaticSwitchMap).IsValid())
    {
        for (const auto& Pair : (*StaticSwitchMap)->Values)
        {
            bool BoolVal = false;
            if (Pair.Value.IsValid() && Pair.Value->TryGetBool(BoolVal))
            {
                // Route through the update context's own static set; a direct
                // Instance->SetStaticSwitchParameterValueEditorOnly here is
                // discarded when the context dtor commits its snapshot.
                UpdateCtx.SetParameterValueEditorOnly(FMaterialParameterInfo(FName(*Pair.Key)), FMaterialParameterMetadata(FMaterialParameterValue(BoolVal)));
                Applied.Add(MakeAppliedEntry(TEXT("staticSwitch"), EARGCompat::JsonKeyToString(Pair.Key)));
            }
            else
            {
                Failed.Add(MakeFailureEntry(TEXT("staticSwitch"), EARGCompat::JsonKeyToString(Pair.Key), TEXT("value is not a boolean")));
            }
        }
    }
}
