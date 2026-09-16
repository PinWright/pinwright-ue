// Copyright (c) 2026 Alexander Penkin. MIT License.

// BpirDecompiler.h - Decompiles Blueprint graphs into BPIR text format

#pragma once

#include "CoreMinimal.h"

class UBlueprint;
class UEdGraph;
class UEdGraphNode;
class UEdGraphPin;
class FGraphWalker;
class FBpirTextEmitter;

DECLARE_LOG_CATEGORY_EXTERN(LogBpirDecompiler, Log, All);

// Severity tag on a decompiler warning. `Error` is reserved for decompiler-internal
// bookkeeping failures ("shouldn't normally happen" paths); `Warn` covers source-asset
// hygiene notes. The dump emitter (AssetDumpBuilder) maps these to `BPIR_ERROR:` and
// `BPIR_WARN:` marker prefixes so consumers can grep failures separately from notes.
enum class EBpirWarningSeverity : uint8 { Warn, Error };

struct FBpirWarning
{
    FString Text;
    EBpirWarningSeverity Severity = EBpirWarningSeverity::Warn;
};

// Why a decompile produced an empty BpirText. The dump-builder uses this to pick
// the appropriate `# (...)` marker comment so adjacent empty graphs in an asset
// dump no longer look identical when their reasons differ. `ZeroNodes` is set by
// the dump-builder (which has the Graph->Nodes.Num() check); the other reasons
// are set by the decompiler itself.
enum class EBpirEmptyReason : uint8
{
    NotEmpty,                    // Result.BpirText is non-empty
    ZeroNodes,                   // Graph->Nodes.Num() == 0 (set by AssetDumpBuilder)
    UnreachableGraph,            // Graph has nodes but no entry point the walker can seed from
};

// Result of a BPIR decompilation operation
struct FBpirDecompileResult
{
    bool bSuccess = false;
    FString BpirText;
    TArray<FBpirWarning> Warnings;
    EBpirEmptyReason EmptyReason = EBpirEmptyReason::NotEmpty;

    static FBpirDecompileResult MakeError(const FString& Message)
    {
        FBpirDecompileResult Result;
        Result.bSuccess = false;
        Result.Warnings.Add(FBpirWarning{Message, EBpirWarningSeverity::Error});
        return Result;
    }
};

/**
 * BPIR Decompiler.
 * Converts Blueprint graphs into BPIR (Blueprint Intermediate Representation) text.
 *
 * The output is a line-based IR that maps 1:1 to Blueprint graph nodes, using
 * SSA-like named values (%name), labeled blocks (@label), and explicit exec wiring.
 *
 * Usage:
 *   FBpirDecompiler Decompiler(MyBlueprint);
 *   FBpirDecompileResult Result = Decompiler.Decompile();
 */
class PINWRIGHT_API FBpirDecompiler
{
public:
    explicit FBpirDecompiler(UBlueprint* InTargetBlueprint);
    ~FBpirDecompiler();

    FBpirDecompiler(const FBpirDecompiler&) = delete;
    FBpirDecompiler& operator=(const FBpirDecompiler&) = delete;

    /** Decompile all graphs in the Blueprint. */
    FBpirDecompileResult Decompile();

    /** Decompile a specific graph by name. */
    FBpirDecompileResult DecompileGraph(const FString& GraphName);

    /** Decompile a specific function by name. */
    FBpirDecompileResult DecompileFunction(const FString& FunctionName);

    /** Decompile a specific macro by name. */
    FBpirDecompileResult DecompileMacro(const FString& MacroName);

    // Returns the BPIR literal token for an unwired pin's type-default:
    // nullptr / "" / false / 0 / 0.0 / None / sugared-struct / <unresolved>.
    // Public so emitters can route their defense-in-depth fallbacks through
    // the same dispatch table.
    static FString FormatPinDefaultLiteral(const UEdGraphPin* Pin);

private:
    // Extract the right-hand-side values from a flat `(Key=Val,Key=Val,...)`
    // ExportText struct literal, in source order. Used to feed the positional
    // sugar formatter (FVector/FRotator/FLinearColor). Flat split on `,` —
    // nested struct values containing literal commas are NOT supported, but
    // ExportText for the supported types never emits nested commas at this
    // level. Returns an empty array when Str is not surrounded by parens.
    static TArray<FString> ExtractStructComponentValues(const FString& StructLiteral);

    // Per-entry-point state used during decompilation
    struct FEntryState
    {
        int32 NextValueIndex = 0;
        int32 NextLabelIndex = 0;
        TMap<UEdGraphNode*, FString> NodeToValueName;
        TSet<UEdGraphNode*> VisitedNodes;
        TArray<FString> Lines;
        TArray<FBpirWarning> Warnings;

        // Queued labels: pairs of (label name, exec pin to walk from)
        TArray<TPair<FString, UEdGraphPin*>> PendingLabels;

        // Track which labels have been used to detect collisions
        TSet<FString> UsedLabels;

        // Map nodes to the label block they were first emitted under
        // Empty string means initial (pre-label) block
        TMap<UEdGraphNode*, FString> NodeToEmittedLabel;

        // Map nodes to the line index where they were emitted (for retroactive label insertion)
        TMap<UEdGraphNode*, int32> NodeToLineIndex;

        // The current label being walked
        FString CurrentWalkLabel;

        FString AllocValueName();
        FString AllocLabel(const FString& Preferred);
    };

    void DecompileGraphInternal(UEdGraph* Graph, TArray<FString>& OutEntryTexts, TArray<FBpirWarning>& OutWarnings, EBpirEmptyReason& OutEmptyReason);

    // Recursively flatten every UK2Node_Composite in `Graph` (and any nested composites
    // inside their BoundGraphs) by dissolving each composite into its inner nodes.
    // Operates in-place on `Graph`; the caller is responsible for cloning the input
    // graph first so the live editor graph is never mutated. Warnings are appended
    // to OutWarnings.
    void InlineCompositesInPlace(UEdGraph* Graph, TArray<FBpirWarning>& OutWarnings, int32 Depth = 0);

    // Walk an exec chain starting from an exec output pin, emitting BPIR lines
    void WalkExecChain(UEdGraphPin* ExecPin, FEntryState& State);

    // Backtrack from an input data pin to find and emit any unvisited pure nodes
    FString ResolveInputValue(UEdGraphPin* InputPin, FEntryState& State, int32 Depth = 0);

    // Emit all pure dependency nodes for an impure node's data inputs
    void EmitPureDependencies(UEdGraphNode* Node, FEntryState& State);

    // Append a BPIR body instruction for a concrete graph node and record its source line.
    void AppendNodeLine(FEntryState& State, UEdGraphNode* Node, const FString& Line);

    UBlueprint* TargetBlueprint;
    TUniquePtr<FGraphWalker> GraphWalker;
    TUniquePtr<FBpirTextEmitter> TextEmitter;

    // Un-cloned source graph's name for the current DecompileGraphInternal run. The
    // working graph may be a composite-flattening clone with an auto-suffixed name
    // (e.g. "Set Error_2"), so emit paths that surface the graph name use this instead
    // of GetGraph()->GetName().
    FString CurrentSourceGraphName;
};
