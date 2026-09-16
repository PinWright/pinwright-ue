// Copyright (c) 2026 Alexander Penkin. MIT License.

// BpirSubgraphCompiler.h - Compiles BPIR with header declarations into a collapsed subgraph

#pragma once

#include "CoreMinimal.h"
#include "Compiler/CompilerTypes.h"
#include "Compiler/BpirTypeSpec.h"

class UBlueprint;
class UEdGraph;
class UK2Node_Tunnel;

DECLARE_LOG_CATEGORY_EXTERN(LogBpirSubgraphCompiler, Log, All);

// Parsed declaration from the BPIR header section (before ---)
struct FBpirExpressionDecl
{
    FString Name;
    // Structured BPIR type spec. Default-constructed (IsEmpty()) means "wildcard"
    // because no type clause was authored on this declaration.
    FBpirTypeSpec TypeSpec;
    FString DefaultValue;  // Optional default for inputs
    bool bIsInput = true;
};

// Orchestrates BPIR compilation into a UK2Node_Composite's collapsed subgraph.
// Parses header declarations, configures tunnel pins, delegates body compilation
// to FBpirCompiler::CompileBodyIntoGraph, and wires outputs to exit tunnel.
class FBpirSubgraphCompiler
{
public:
    FBpirSubgraphCompiler(UBlueprint* InBlueprint, UEdGraph* InBoundGraph,
                          UK2Node_Tunnel* InEntryTunnel, UK2Node_Tunnel* InExitTunnel);

    // Compile full BPIR text (header + body) into the bound subgraph.
    FCompileResult CompileIntoSubgraph(const FString& FullBpirText);

private:
    // Split text at --- separator into header lines and body text
    void SplitHeaderAndBody(const FString& FullText, TArray<FString>& OutHeaderLines, FString& OutBody);

    // Parse input/output declarations from header lines
    bool ParseDeclarations(const TArray<FString>& HeaderLines, TArray<FBpirExpressionDecl>& OutDecls, TArray<FCompileError>& OutErrors);

    // Auto-discover $var references in body when no header is present
    void AutoDiscoverInputs(const FString& Body, TArray<FBpirExpressionDecl>& OutDecls);

    // Scan body for impure opcodes
    static bool DetectImpure(const FString& Body);

    UBlueprint* Blueprint;
    UEdGraph* BoundGraph;
    UK2Node_Tunnel* EntryTunnel;
    UK2Node_Tunnel* ExitTunnel;
};
