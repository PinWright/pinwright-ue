// Copyright (c) 2026 Alexander Penkin. MIT License.

// PwAnimAst.h - Parse tree for the .pwanim source format.
//
// The AST is source-only.  It deliberately keeps the authored parameter maps and key
// statement order intact so the compiler can distinguish an omitted channel from an
// explicitly authored value and a future emitter can preserve the recipe.
#pragma once

#include "CoreMinimal.h"
#include "Misc/Optional.h"

#include "PwSource/PwDocument.h"
#include "PwSource/PwValue.h"

// `bone "name" [ease=…] { key … }`.
struct FPwAnimBone
{
    FString BoneName;
    TMap<FString, FPwValue> Header;
    TArray<FPwOp> Keys;

    int32 Line = 0;
    int32 Column = 0;
};

// `sync_marker "name" frame=N` is timeline state, so it lives beside the timebase rather
// than inside a bone block. Duplicate names are intentional: a marker label may occur at
// multiple phases of a loop.
struct FPwAnimSyncMarker
{
    FString MarkerName;
    int32 Frame = 0;
    int32 Line = 0;
    int32 Column = 0;
};

// One .pwanim file produces one animation asset.  The parser keeps all syntactically
// recovered uses and bones so diagnostics can report more than the first error; callers
// must not compile a document for which Parse returned false.
struct FPwAnimDocument
{
    FPwDocumentHeader Header;
    TArray<FPwUse> Uses;
    TOptional<FPwOp> Timebase;
    // Set means the source authored at least one marker statement. An unset value is
    // deliberately different from an empty replacement: old sources with no marker syntax
    // leave carried marker state alone during an in-place rebuild.
    TOptional<TArray<FPwAnimSyncMarker>> SyncMarkers;
    TArray<FPwAnimBone> Bones;
};

using FPwAnimationBone = FPwAnimBone;
using FPwAnimationDocument = FPwAnimDocument;
