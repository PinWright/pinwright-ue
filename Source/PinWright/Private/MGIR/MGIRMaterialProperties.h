// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"

class UMaterial;

// The material-level (non-graph) `UMaterial` state MGIR carries across a
// decompile -> compile round trip, as `property <Name>: <Value>` lines inside an
// `entry material` block.
//
// Before this existed the decompiler emitted the expression graph and NOTHING else, and
// the grammar had no syntax for these at all: a translucent, two-sided master decompiled
// and recompiled came back BLEND_Opaque / TwoSided=false with `blocksCompiled: 1` and no
// warning. The Opacity pin survived, wired into an opaque material, where the renderer
// discards it - a glass master silently rendering as a solid, with every field of the
// compile response reporting success.
//
// Names are the reflected UPROPERTY names, resolved through the property system rather
// than a hand-written mapping, so the MGIR token cannot drift from the engine field it
// writes. A carried property whose UPROPERTY no longer exists fails loudly at emit and at
// apply instead of silently dropping out of the round trip - the failure mode this module
// exists to remove.
namespace MGIRMaterialProperties
{
struct FResult
{
    FString ErrorCode;
    FString ErrorMessage;

    bool IsSuccess() const { return ErrorCode.IsEmpty(); }

    static FResult MakeError(const FString& InCode, const FString& InMessage)
    {
        FResult Result;
        Result.ErrorCode = InCode;
        Result.ErrorMessage = InMessage;
        return Result;
    }
};

// A validated `property` line, resolved against the spec table but not yet written.
// Parsing is separated from applying so a bad property name or value fails the whole
// block before the compiler has touched (and, in Append mode, emptied) the target asset.
struct FParsed
{
    int32 SpecIndex = INDEX_NONE;
    int64 EnumValue = 0;
    bool bBoolValue = false;
    float FloatValue = 0.0f;
    int32 IntValue = 0;
};

// Emits one `property <Name>: <Value>` line per carried property, unindented and in
// declaration order. Always emits every carried property, including ones sitting at
// their engine default: a document that omits a property leaves the target's existing
// value alone, so "absent" and "explicitly default" have to stay distinguishable.
TArray<FString> Emit(const UMaterial* Material);

FResult ParseProperty(const FString& Name, const FString& Value, FParsed& OutParsed);

FResult ApplyProperty(UMaterial* Material, const FParsed& Parsed);

// Run once after the last ApplyProperty on a material. `ShadingModels` (the bitfield the
// renderer actually reads) is derived from `ShadingModel`, not written alongside it, so a
// reflection write to `ShadingModel` alone leaves the material shading with the previous
// model. This is the same call UMaterial makes for itself on a property edit and on every
// shader recompile.
void FinalizeAppliedProperties(UMaterial* Material);

// The carried set, for diagnostics that have to name what MGIR does and does not preserve.
TArray<FString> GetCarriedPropertyNames();
}
