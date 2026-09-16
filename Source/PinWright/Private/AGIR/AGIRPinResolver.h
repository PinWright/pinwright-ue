// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "IrCore/IrPinResolverBase.h"


class UAnimGraphNode_Base;
class UEdGraphNode;

// AGIR pin reference — pose-link source identifier in AGIR text. Two flavours:
// `%nNN` for numeric AGIR-local ids (the primary form) and `%nodename` for
// symbolic ids. No mask / output-index syntax: anim pose links are scalar.
struct FAGIRPinReference
{
    FString SymbolName;   // The token after `%` (e.g. "n42" or "MyNode").
    bool bIsNumeric = false;
    int32 NumericId = INDEX_NONE;
};

class FAGIRPinResolver : protected TIrPinResolverBase<UEdGraphNode, void>
{
public:
    // Re-expose the shared error envelope publicly so callers can name the
    // WirePoseInput return type (protected inheritance hides the lookup/error
    // helpers but not this result type).
    using FWireResult = TIrPinResolverBase<UEdGraphNode, void>::FWireResult;

    // Parses `%n42` or `%nodename`. Returns false on malformed input.
    static bool ParseReference(FStringView Ref, FAGIRPinReference& Out);

    // Looks up the upstream node from a parsed reference. Symbol map is keyed
    // by the same string the emitter wrote (e.g. "n42"). Returns null if not found.
    static UEdGraphNode* ResolveReference(
        const FAGIRPinReference& Ref,
        const TMap<FString, UEdGraphNode*>& Symbols);

    // Creates a pose-link connection between an upstream node's output pin and
    // a downstream node's input pin. On failure FWireResult::ErrorCode is one of
    // AGIR_INVALID_PIN_REFERENCE / AGIR_SYMBOL_NOT_FOUND / AGIR_POSE_TYPE_MISMATCH
    // (schema rejected the pair) / AGIR_POSE_CONNECT_FAILED (schema accepted
    // but TryCreateConnection still failed). IsSuccess() is true on success.
    static FWireResult WirePoseInput(
        UAnimGraphNode_Base* DownstreamNode,
        FName InputPinName,
        UAnimGraphNode_Base* UpstreamNode,
        FName OutputPinName);
};
