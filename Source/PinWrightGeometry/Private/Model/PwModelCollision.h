// Copyright (c) 2026 Alexander Penkin. MIT License.

// PwModelCollision.h - Builds an FKAggregateGeom from a parsed .pwmodel `collision` block.
//
// This layer writes NO asset and touches NO UBodySetup: it returns a value that
// GeometryAssetCreate writes into the deferred gap before the StaticMesh's single build
// (FStaticMeshCreateSpec::SimpleCollision). That is the whole reason it is a separate
// translation unit - collision patched onto an already-built asset is what forced the
// InvalidatePhysicsData / recook dance in CollisionHelpers.h, and none of it is needed
// when the aggregate geometry exists before the first build.
//
// `hull { … }` bodies need geometry ops executed, which lives a layer up. Rather than
// depending on the ops layer - and on a merged-mesh pipeline that does not exist yet at
// this level - the caller passes a callback. That also makes every hull case unit-testable
// with a stub that fabricates a mesh, with no compiler in the loop.
#pragma once

#include "CoreMinimal.h"
#include "Templates/Function.h"

#include "BodySetupEnums.h"                // ECollisionTraceFlag
#include "PhysicsEngine/AggregateGeom.h"   // FKAggregateGeom and its FK*Elem members

#include "Model/PwModelAst.h"

class UDynamicMesh;

// Declared rather than included: GeometryScript/CollisionFunctions.h is a heavy header and the
// only thing needed here is the enum's name. The underlying type is part of its declaration
// (CollisionFunctions.h:17), so it must be repeated exactly.
enum class EGeometryScriptCollisionGenerationMethod : uint8;

struct FPwModelCollisionResult
{
    bool bSuccess = false;

    // A PwModelDiagnosticCodes PWMODEL_* value; empty on success.
    FString ErrorCode;
    FString ErrorMessage;

    // Source position of the failing entry, so the compiler can anchor its diagnostic on the
    // line the author wrote rather than on the collision block as a whole. -1 when the failure
    // has no position.
    int32 ErrorLine = -1;
    int32 ErrorColumn = -1;

    // Non-fatal notes. The compiler folds these into its diagnostic list; nothing here fails
    // a compile.
    TArray<FString> Warnings;

    FKAggregateGeom Geom;
    ECollisionTraceFlag Trace = ECollisionTraceFlag::CTF_UseDefault;

    int32 ElementsWritten = 0;
};

// Executes one `hull { … }` body's ops into a transient mesh. Returns false and fills
// OutErrors when the body cannot be built.
using FBuildMeshFromOps = TFunctionRef<bool(const TArray<FPwOp>& Ops,
                                            UDynamicMesh* Out,
                                            TArray<FString>& OutErrors)>;

// Builds the aggregate geometry and trace flag for one model.
//
// MergedMesh is the source for `auto` generation only - explicit elements never read it, and
// it may be null when the block declares none. BuildOps is called once per `hull` entry.
FPwModelCollisionResult BuildCollision(const FPwModelCollision& CollisionBlock,
                                       UDynamicMesh* MergedMesh,
                                       FBuildMeshFromOps BuildOps);

// The `complexity` and `auto method=` vocabularies are DERIVED from their engine enums rather
// than hand-written, so a value added to ECollisionTraceFlag or to
// EGeometryScriptCollisionGenerationMethod cannot silently fall through a four-way branch into
// a wrong default. Exposed so a caller can list the accepted spellings in a diagnostic without
// re-deriving them.
namespace PwModelCollisionNames
{
    // snake_case of ECollisionTraceFlag minus its CTF_Use prefix: simple_and_complex,
    // simple_as_complex, complex_as_simple, use_default.
    const TArray<FString>& ComplexityNames();
    bool TryParseComplexity(const FString& Name, ECollisionTraceFlag& OutFlag);

    // snake_case of EGeometryScriptCollisionGenerationMethod: aligned_boxes … level_sets.
    const TArray<FString>& AutoMethodNames();
    bool TryParseAutoMethod(const FString& Name, EGeometryScriptCollisionGenerationMethod& OutMethod);
}
