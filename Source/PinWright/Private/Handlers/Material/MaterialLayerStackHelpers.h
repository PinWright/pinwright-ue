// Copyright (c) 2026 Alexander Penkin. MIT License.

// MaterialLayerStackHelpers.h
// Single source of truth for the parallel-array maintenance that
// UMaterialExpressionMaterialAttributeLayers requires when its Layers/Blends
// slots are populated. Used by both the MGIR emitter (EmitLayerStack) and the
// imperative `material.authoring.set_material_layer_stack` RPC so the two
// authoring paths cannot drift.
//
// The DefaultLayers struct holds parallel arrays:
//   Layers, EditorOnly.LayerNames, LayerStates, LayerGuids, LayerLinkStates,
//   RestrictToLayerRelatives  (one entry per layer)
//   Blends, EditorOnly.RestrictToBlendRelatives  (one entry per blend; length
//   must equal Layers.Num() - 1)
// Forgetting any one parallel array leaves the layer-stack node in an invalid
// state that fails to compile and confuses the material editor UI.

#pragma once

#include "CoreMinimal.h"
#include "Materials/MaterialExpressionMaterialAttributeLayers.h"
#include "Materials/MaterialFunctionInterface.h"
// FMaterialParameterInfo et al. moved from the top-level MaterialTypes.h into
// Materials/MaterialParameters.h in UE 5.7; on 5.6 and earlier they live in
// MaterialTypes.h, which has no MaterialParameters.h (and is deprecated on 5.8).
#if __has_include("Materials/MaterialParameters.h")
#include "Materials/MaterialParameters.h"
#else
#include "MaterialTypes.h"
#endif

// Empties DefaultLayers and repopulates every parallel array from the supplied
// Layers/Blends, then calls RebuildLayerGraph(false). Caller is responsible
// for any subsequent PostEditChange/MarkPackageDirty on the owning material.
//
// Caller-provided invariant: Blends.Num() == Layers.Num() - 1 when Layers is
// non-empty. Helper does not enforce this — both call sites already validate
// upstream and surface a domain-specific error.
inline void ApplyMaterialLayerStack(
    UMaterialExpressionMaterialAttributeLayers* LayersExpression,
    const TArray<UMaterialFunctionInterface*>& Layers,
    const TArray<UMaterialFunctionInterface*>& Blends)
{
    if (!LayersExpression) return;

    LayersExpression->DefaultLayers.Empty();
    for (UMaterialFunctionInterface* Layer : Layers)
    {
        LayersExpression->DefaultLayers.Layers.Add(Layer);
        LayersExpression->DefaultLayers.EditorOnly.LayerNames.Add(
            FText::FromString(Layer ? Layer->GetName() : FString()));
        LayersExpression->DefaultLayers.EditorOnly.LayerStates.Add(true);
        LayersExpression->DefaultLayers.EditorOnly.LayerGuids.Add(FGuid::NewGuid());
        LayersExpression->DefaultLayers.EditorOnly.LayerLinkStates.Add(EMaterialLayerLinkState::NotFromParent);
        LayersExpression->DefaultLayers.EditorOnly.RestrictToLayerRelatives.Add(false);
    }

    for (UMaterialFunctionInterface* Blend : Blends)
    {
        LayersExpression->DefaultLayers.Blends.Add(Blend);
        LayersExpression->DefaultLayers.EditorOnly.RestrictToBlendRelatives.Add(false);
    }

    LayersExpression->RebuildLayerGraph(false);
}
