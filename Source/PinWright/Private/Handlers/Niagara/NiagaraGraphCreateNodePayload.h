// Copyright (c) 2026 Alexander Penkin. MIT License.
// Helper for niagara.graph.create_node — extracted so tests can reach it directly.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Handlers/Niagara/NiagaraEditTypes.h"


class UClass;
class UNiagaraNode;

namespace NiagaraGraphCreate
{
    /**
     * Decide, from the UClass alone, whether niagara.graph.create_node v1 may construct this node.
     * Returns an empty FNiagaraEditError when it may; UNSUPPORTED_NODE_CLASS for a class outside
     * the v1 list, NODE_CREATE_FAILED for a class that cannot be instantiated at all.
     *
     * This must run BEFORE FGraphNodeCreator is constructed. ~FGraphNodeCreator is an
     * unconditional checkf(bPlaced) (EdGraph.h), so rejecting a request between CreateNode() and
     * Finalize() is an editor-terminating appError rather than an error response.
     */
    PINWRIGHT_API FNiagaraEditError ValidateCreateNodeClass(UClass* NodeClass);

    /**
     * Apply node-class-specific payload fields to a freshly created UNiagaraNode.
     * Returns an empty FNiagaraEditError on success; on failure HasError() is true
     * with Code and Message set.
     *
     * v1 handled classes: UNiagaraNodeOp, UNiagaraNodeInput, UNiagaraNodeOutput,
     * UNiagaraNodeCustomHlsl, UNiagaraNodeStaticSwitch, UNiagaraNodeIf,
     * UNiagaraNodeReroute, UNiagaraNodeConvert.
     * Any other class returns UNSUPPORTED_NODE_CLASS.
     */
    PINWRIGHT_API FNiagaraEditError ApplyCreateNodePayload(
        UNiagaraNode* Node,
        const TSharedPtr<FJsonObject>& Payload);
}
