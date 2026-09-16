// Copyright (c) 2026 Alexander Penkin. MIT License.

// AGIRCompiler.h
//
// Compiles AGIR text back into a `UAnimBlueprint`'s anim graphs and anim
// layer interface override graphs. Mirrors `FMGIRCompiler` shape.
//
// Phase 3 scope: state machines + anim layer interface override graphs
// round-trip. Blend-space-graph, blend-space-sample-graph, layered-blend
// pose subgraphs and custom-transition subgraphs return
// `AGIR_SUBGRAPH_NOT_SUPPORTED` (decompile-only). Implicit AnimBP creation
// is not supported (`UAnimBlueprint` requires a `USkeleton` not carried in
// AGIR text); missing target asset is reported as `AGIR_TARGET_NOT_FOUND`.
//
// This header ships the skeleton (types, options, top-level `Compile` entry).
// Per-family block compile bodies are filled in by Wave 7 chunks.

#pragma once

#include "CoreMinimal.h"

// Replace clears the existing AnimGraph + linked anim layer interface
// implementation graphs before emitting; Extend appends to whatever already
// exists on the target.
enum class EAGIRCompileMode : uint8
{
    Replace,
    Extend,
};

struct FAGIRCompileOptions
{
    EAGIRCompileMode Mode = EAGIRCompileMode::Replace;
    FString Context;
    bool bRunLayout = true;
    bool bSave = false;
};

struct FAGIRCompileResult
{
    bool bSuccess = false;
    FString ErrorCode;
    FString ErrorMessage;
    FString AssetPath;
    int32 BlocksCompiled = 0;
    int32 NodesCreated = 0;
    TArray<FString> Warnings;

    // Optional human-facing diagnostic steer surfaced alongside an error (relayed
    // by anim.compile_agir as a `hint` field). Used to attribute failures whose
    // raw message is locally true but globally misleading — e.g. a pose-symbol
    // resolution failure that may actually be a decompiler/compiler round-trip
    // gap rather than user-authored bad AGIR. Empty when there is nothing to add.
    FString Hint;

    static FAGIRCompileResult MakeError(const FString& InCode, const FString& InMessage)
    {
        FAGIRCompileResult Result;
        Result.ErrorCode = InCode;
        Result.ErrorMessage = InMessage;
        return Result;
    }

    FAGIRCompileResult& WithHint(const FString& InHint)
    {
        Hint = InHint;
        return *this;
    }
};

class PINWRIGHT_API FAGIRCompiler
{
public:
    static FAGIRCompileResult Compile(FStringView Code, const FAGIRCompileOptions& Options);
};
