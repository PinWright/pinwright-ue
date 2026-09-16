// Copyright (c) 2026 Alexander Penkin. MIT License.

// PwSkelAst.h - Parse tree for the .pwskel source format.
//
// A .pwskel file contains one skeleton hierarchy and produces one USkeleton asset. The
// document deliberately has no asset-path field: the path-in-source form was removed when
// skeleton references became direct /Game/... asset paths, and the compiler owns its output
// path instead.
#pragma once

#include "CoreMinimal.h"

#include "PwSource/PwDocument.h"
#include "PwSource/PwValue.h"

// `bone "name" [at= rotate= scale=] { ... }`.
//
// Transform values remain in their source representation so the compiler can use the one
// shared PwValueRead::ReadTransformParams convention. Children are stored in declaration order;
// that order is the depth-first order used to build FReferenceSkeleton.
struct FPwBone
{
    FString Name;
    TMap<FString, FPwValue> Transform;
    TArray<FPwBone> Children;

    int32 Line = 0;
    int32 Column = 0;
};

struct FPwSkelPreviewMesh
{
    FString AssetPath;
    int32 Line = 0;
    int32 Column = 0;
};

// Curve metadata is authored rig data. It is not animation-sample output or a cache: Unreal
// serializes it on the skeleton and exposes it in the Skeleton editor.
struct FPwSkelCurveMetaData
{
    FString Name;
    bool bMaterial = false;
    bool bMorphTarget = false;
    int32 MaxLod = -1;
    TArray<FString> LinkedBones;
    int32 Line = 0;
    int32 Column = 0;
};

// One source file, one skeleton hierarchy. A valid document has exactly one root in Roots;
// malformed documents retain all parsed roots so the parser can report the complete error set.
struct FPwSkelDocument
{
    FPwDocumentHeader Header;
    TOptional<FPwSkelPreviewMesh> PreviewMesh;
    TArray<FPwSkelCurveMetaData> Curves;
    TArray<FPwBone> Roots;
};
