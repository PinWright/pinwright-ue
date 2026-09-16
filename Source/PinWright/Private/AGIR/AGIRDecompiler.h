// Copyright (c) 2026 Alexander Penkin. MIT License.

// AGIRDecompiler.h
//
// Drives `FAGIRTextEmitter` over a `UAnimBlueprint`'s anim graphs and anim
// layer interface override graphs. Mirrors `FMGIRDecompiler` shape: result
// struct is `{ bSuccess, AGIRText, Warnings }`, top-level entries are sorted
// alphabetically and prefixed with `# ==== Graph: <Name> (<Kind>) ====`
// headers (matching `AssetDumpBuilder::BuildBpirText`).
//
// Walker filters `Blueprint->FunctionGraphs` to graphs whose schema derives
// from `UAnimationGraphSchema` and whose class is exactly `UAnimationGraph`
// (not state-machine / transition / blend-space sub-classes — those are
// reached recursively from inside the emitter's `EmitStateMachine`). It also
// walks `Blueprint->ImplementedInterfaces[i].Graphs` plus their child graphs
// for anim layer interface override pose graphs.

#pragma once

#include "CoreMinimal.h"

class UAnimBlueprint;


struct FAGIRDecompileResult
{
    bool bSuccess = false;
    FString AGIRText;
    TArray<FString> Warnings;

    static FAGIRDecompileResult MakeError(const FString& Message)
    {
        FAGIRDecompileResult Result;
        Result.Warnings.Add(Message);
        return Result;
    }
};

class PINWRIGHT_API FAGIRDecompiler
{
public:
    explicit FAGIRDecompiler(UAnimBlueprint* InAnimBP);

    // Emits all anim graphs + anim layer interface override graphs in the BP.
    FAGIRDecompileResult Decompile();

private:
    UAnimBlueprint* AnimBlueprint = nullptr;
};
