// Copyright (c) 2026 Alexander Penkin. MIT License.

// PwModelAst.h - Parse tree for the .pwmodel source format.
//
// Plain data: no engine types, no UObject, no world. The parser produces this and
// the compiler consumes it, so both halves are unit-testable without an editor
// world - which is the whole reason the tokenizer and parser were split out of the
// compile RPC.
//
// There is deliberately no per-part "bake" flag: one source file compiles to
// exactly one asset (docs/adr/0001-one-file-one-asset.md), every part merges into
// the single output mesh, and nothing opts out.
#pragma once

#include "CoreMinimal.h"
#include "Misc/Optional.h"
#include "PwSource/PwDocument.h"
#include "PwSource/PwValue.h"

// `part <name> [bone= at= rotate= scale=] { … }` - a named sub-region of the single
// output mesh, carrying material assignment. Its transform maps part-local space
// to mesh space and is applied once, after the part's ops have run.
struct FPwModelPart
{
    FString Name;

    // A String literal naming the reference-skeleton bone this part is rigidly bound to.
    // Empty means the part has no rigid binding in the source document.
    FString BoneBinding;

    // Specific opt-out for an intentionally floating part. The compiler still measures and
    // reports it; this only suppresses the warning for this part, never the whole check.
    bool bAllowFloating = false;

    // The header's at / rotate / scale, unresolved. Absent keys mean "not specified",
    // which is not the same as an identity value supplied explicitly.
    TMap<FString, FPwValue> Transform;

    TArray<FPwOp> Ops;

    int32 Line = 0;
    int32 Column = 0;
};

// The slot every UNTAGGED piece of part-level geometry lands in, allocated lazily by
// the compiler (PwModelCompiler.cpp RunGenerator) the first time a generator runs
// without `material=`. It is a real, bindable slot name: `materials { Default = "…" }`
// binds it and the binding reaches the asset.
//
// Shared here rather than spelled twice because the parser has to know it too. It did
// not, and PWMODEL_UNUSED_MATERIAL told authors "Slot 'Default' is bound but no geometry
// tags it; the binding is dropped" about a binding the compiler was applying - a
// diagnostic that contradicted the asset it had just written.
inline constexpr TCHAR PwModelDefaultSlotName[] = TEXT("Default");

// `materials { Slot = "/Game/…" }`. An ordered array rather than a map: the block is
// reported and diagnosed in source order, and a map would lose both that and the
// position needed to report a duplicate slot.
//
// Declaration order is NOT the slot order of the output asset. Slots are allocated in
// model-wide FIRST-USE order by PwModelCompiler.cpp ResolveSlot - the index follows the
// tags, not this block. The implicit `Default` is the one exception: it is rotated to the
// END of the table (PwModelCompiler.cpp MoveImplicitDefaultSlotLast), so untagged geometry
// appearing anywhere cannot renumber a slot the author's tags placed.
struct FPwModelMaterialBinding
{
    FString Slot;
    FString AssetPath;

    int32 Line = 0;
    int32 Column = 0;
};

// `collision { … }` - at most one per model. Complexity plus either explicit
// elements or one `auto` rule; the parser records both and the compiler rejects
// the combination with PWMODEL_COLLISION_CONFLICT.
struct FPwModelCollision
{
    // `complexity = <flag>` as an Identifier value; Type == None when omitted.
    FPwValue Complexity;

    // box / sphere / capsule / hull / convex / auto, in source order.
    TArray<FPwOp> Elements;

    int32 Line = 0;
    int32 Column = 0;
};

// `skeleton { … }` and `animation <name> { … }` are parsed and brace-checked, then
// rejected with PWMODEL_CONSTRUCT_IN_WRONG_FORMAT; each signposts its own format.
// The diagnostic identifies the reserved keyword, so only the keyword, the optional name
// and the position are retained. The body is discarded.
struct FPwModelReservedBlock
{
    FString Keyword;
    FString Name;   // `animation <name>`; empty for skeleton

    int32 Line = 0;
    int32 Column = 0;
};

// One file, one asset.
struct FPwModelDocument
{
    FPwDocumentHeader Header;

    TArray<FPwUse> Uses;
    TArray<FPwModelPart> Parts;

    TArray<FPwModelMaterialBinding> Materials;
    TOptional<FPwModelCollision> Collision;

    // `uv_layout channel=N [texture_resolution=M]` statements, retained in source
    // order. Multiple statements are allowed, but each channel may appear once.
    TArray<FPwOp> UVLayouts;

    // `lightmap channel=N` as an op node so it keeps its position and reads the same
    // way as every other `name key=value` statement. It is model-level rather than an
    // op because LightMapCoordinateIndex is per-asset.
    TOptional<FPwOp> Lightmap;

    // `skin { smooth ... }` is a model-level rule. The outer node preserves the block's
    // source position and its children carry the typed smooth parameters for the compiler.
    TOptional<FPwOp> Skin;

    TArray<FPwModelReservedBlock> ReservedBlocks;
};
