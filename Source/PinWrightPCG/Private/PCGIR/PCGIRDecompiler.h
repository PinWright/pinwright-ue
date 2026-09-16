// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"

class UPCGGraph;


struct FPCGIRDecompileResult
{
    bool bSuccess = false;
    FString PCGIRText;
    TArray<FString> Warnings;

    static FPCGIRDecompileResult MakeError(const FString& Message)
    {
        FPCGIRDecompileResult Result;
        Result.Warnings.Add(Message);
        return Result;
    }
};

struct FPCGIRDecompileOptions
{
    // Reserved for future use. The decompiler always emits subgraph nodes in
    // flattened form (referenced graph's path as a property; child nodes are
    // not inlined). If callers set this to true, the decompiler appends a
    // warning to Result.Warnings noting that the flag is deferred.
    bool bIncludeReferencedSubgraphs = false;
};

class FPCGIRDecompiler
{
public:
    static FPCGIRDecompileResult DecompileGraph(
        UPCGGraph* Graph,
        const FPCGIRDecompileOptions& Options = FPCGIRDecompileOptions());
};
