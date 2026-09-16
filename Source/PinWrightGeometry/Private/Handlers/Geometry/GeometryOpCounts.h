// Copyright (c) 2026 Alexander Penkin. MIT License.

// GeometryOpCounts.h - the post-op vertexCount/triangleCount echo, shared by every geometry RPC
// wrapper that reports what the mesh became.
//
// Why a header and not two anonymous-namespace copies: this module builds with bUseUnity = true,
// so two file-local helpers of the same name in MeshOpsHandler.cpp and GeometryTransformHandler.cpp
// become an ODR redefinition the moment Unity merges those TUs. The copies dodged that by
// prefixing one of them with its file name, which compiles but leaves two byte-identical bodies
// free to drift - and this one has exactly one correctness property worth not drifting (see
// below). CLAUDE.md's build note prescribes the other fix for the same ODR problem: consolidate
// the helper into a per-cluster NAMED-namespace header, which is what this file is. It sits
// beside GeometryOpWarnings.h, the sibling header that turns FOpResult::Warnings into response
// JSON; both exist so an FOpResult reaches the wire the same way from every wrapper.
//
// The property: the counts come off the OP RESULT, never from a second read of the mesh. The op
// already sampled both after it ran, and a re-read can only disagree with the numbers the op
// reported (it also costs a second traversal). GeometryUtils::SetMeshCountFields is the re-read
// form and stays for callers that have a mesh but no FOpResult.
#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Handlers/Geometry/GeometryOps.h"

namespace GeometryOps
{
    // Same keys and same emission order as GeometryUtils::SetMeshCountFields, so a wrapper that
    // switches between them produces byte-identical JSON.
    inline void SetMeshCountFieldsFromOp(
        const TSharedPtr<FJsonObject>& Result,
        const FOpResult& Op)
    {
        if (!Result.IsValid())
        {
            return;
        }
        Result->SetNumberField(TEXT("vertexCount"), Op.VerticesAfter);
        Result->SetNumberField(TEXT("triangleCount"), Op.TrianglesAfter);
    }
}
