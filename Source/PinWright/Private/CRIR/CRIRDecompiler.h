// Copyright (c) 2026 Alexander Penkin. MIT License.

// CRIRDecompiler.h
//
// Walks a UControlRigBlueprint and produces canonical CRIR text via
// FCRIRTextEmitter. Mirrors AGIRDecompiler's result-struct shape, but the
// canonical block order is fixed: `rig_hierarchy` block first (always
// emitted, even when empty, so structure is unambiguous), then one
// `rig_graph "<ModelName>" { ... }` block per URigVMGraph, sorted by
// model name for byte-stable output across consecutive decompiles.
//
// Local-id allocation (%n0, %n1, ...) is the decompiler's job — the
// emitter is stateless. Nodes within a graph are sorted by UObject name
// before id assignment so two consecutive decompiles produce byte-equal
// text. Memory feedback_ir_logical_not_visual: round-trip guarantees
// logical equivalence only; node positions and unsupported node kinds
// are surfaced as `# TODO` comment lines plus warnings.

#pragma once

#include "CoreMinimal.h"

class UControlRigBlueprint;


struct FCRIRDecompileResult
{
    bool bSuccess = false;
    FString ErrorCode;
    FString ErrorMessage;
    FString AssetPath;
    FString CRIRText;
    TArray<FString> Warnings;
};

class PINWRIGHT_API FCRIRDecompiler
{
public:
    explicit FCRIRDecompiler(UControlRigBlueprint* InBlueprint);

    FCRIRDecompileResult Decompile();

private:
    UControlRigBlueprint* Blueprint = nullptr;
};
