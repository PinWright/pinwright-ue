// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "CRIR/CRIROpcodes.h"

// Line-oriented parser for CRIR (Control Rig IR) text. Produces a flat list of
// FCRIREntryBlock — one per `rig_graph "<ModelName>" { ... }` or
// `rig_hierarchy { ... }` top-level block. Pure text processing; no
// UControlRig / URigVMGraph dependencies.
//
// `bSkipReferenceValidation` suppresses the cross-reference check that every
// `%name` referenced inside a `wire_in_*=%node.pin` arg resolves to a `%name`
// declared somewhere in the same block. The check is two-pass and tolerates
// forward references (a wire whose source `%name` is declared on a later line),
// because the decompiler emits a graph's nodes in UObject-name order — not
// exec/declaration order — so a wire source can be emitted after its sink. The
// flag exists only for callers that re-parse intermediate fixup states which
// may briefly contain genuinely-dangling refs.
class PINWRIGHT_API FCRIRParser
{
public:
    static bool Parse(
        FStringView Code,
        TArray<FCRIREntryBlock>& OutBlocks,
        TArray<FCRIRParseError>& OutErrors,
        bool bSkipReferenceValidation = false);
};
