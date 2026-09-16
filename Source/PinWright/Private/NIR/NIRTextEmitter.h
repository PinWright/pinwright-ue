// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"

class UEdGraphNode;
class UEdGraphPin;
class UNiagaraGraph;
class UNiagaraNode;
class UNiagaraScript;
enum class ENiagaraScriptUsage : uint8;
struct FNiagaraTypeDefinition;

// FNIRTextEmitter is the line-level output buffer used by the NIR decompiler. It owns
// an indent counter and a TStringBuilder<> output buffer, plus an optional warnings
// sink the v1b/v1c code paths route diagnostics through. The orchestrator builds one
// emitter per FNIRResult and threads it through EmitSystem / EmitEmitterStandalone /
// EmitScriptGraphScope so all line emission flows through a single indent state.
struct PINWRIGHT_API FNIRTextEmitter
{
    explicit FNIRTextEmitter(TArray<FString>* InOutWarnings = nullptr)
        : OutWarnings(InOutWarnings)
    {
    }

    // Append a single fully-formed line at the current indent. Newline is appended automatically.
    void AppendLine(FStringView Line);

    // Append the current-indent whitespace prefix (callers compose the rest of the line themselves).
    void AppendIndent();

    // Open an indented brace scope. Emits "<Header> {" at the current indent, then increments depth.
    void EnterScope(FStringView Header);

    // Close the most recent EnterScope. Decrements depth, then emits "}" at the new indent.
    void ExitScope();

    // Record a non-fatal diagnostic into the result's Warnings array (if attached).
    void Warn(FStringView Message);

    // Return the accumulated buffer as a string (used by BuildNiagaraIrText to fill FNIRResult.Text).
    FString ToString();

    int32 GetIndentDepth() const { return IndentDepth; }
    void SetIndentDepth(int32 Depth) { IndentDepth = FMath::Max(0, Depth); }

private:
    TStringBuilder<4096> Builder;
    int32 IndentDepth = 0;
    TArray<FString>* OutWarnings = nullptr;
};

namespace NIRTextEmitter
{
    // Shared formatting helpers used by both the system/emitter shell (v1a) and the
    // script-graph body emission (v1c). All wrap FIrTextUtils equivalents with NIR-local
    // conventions (FName overloads, version-suffix dispatch, etc.).

    FString Quote(FStringView Value);

    FString FormatNameToken(FName Name);
    FString FormatNameToken(FStringView Name);

    FString FormatPositionSuffix(int32 X, int32 Y);
    FString FormatPositionSuffix(const UEdGraphNode* Node);

    // Resolve "@vMajor.Minor" for a versioned UNiagaraScript reference. Returns empty if
    // VersionGuid is zero or no matching published version exists on the script.
    FString FormatVersionSuffix(const FGuid& VersionGuid, const UNiagaraScript* Script);

    // Map ENiagaraScriptUsage to the NIR-grammar usage token used in "graph <Usage> { ... }"
    // and the dataflow output emitter. Returns "Unknown" for unrecognised usages.
    FString FormatUsageName(ENiagaraScriptUsage Usage);

    // Format an FName-shaped parameter handle as the "$Namespace.Name" SSA-style parameter
    // reference used by EmitInputValueExpr and the ParameterMapGet/Set emitters. Falls back
    // to "$Name" if the handle has no namespace component.
    FString FormatParameterRef(FName ParameterHandleName);

    // Format a Niagara type-definition as its NIR type-name token (e.g. "float", "Vector",
    // "Position"). Returns "Unknown" for invalid type defs so coverage gaps stay visible.
    FString FormatTypeName(const FNiagaraTypeDefinition& Type);

    // Format a pin's Niagara type via UEdGraphSchema_Niagara::PinToTypeDefinition, falling
    // back to explicit struct/class/enum pin metadata when present. Returns "Unknown" for
    // null pins or pins with no resolvable Niagara/object type metadata.
    FString FormatPinType(const UEdGraphPin* Pin);
}

// Recursive value-expression emitter for module input override chains and graph-node input
// pins. Returns the formatted RHS string; warnings (including the depth-32 recursion-limit
// marker) are routed through Out.
FString EmitInputValueExpr(UEdGraphPin* InputPin, FNIRTextEmitter& Out, int32 Depth);

// Format an input-pin RHS as the SSA-style "%upstream.OutputPinName" local reference when the
// pin is linked, or the literal pin default when it's not. The "%name" form mirrors BPIR /
// AGIR local-value refs.
FString FormatPinValueRef(UEdGraphPin* Pin);

// Per-family node emitters. Each tries to handle Node by its concrete class and returns true on
// match, false otherwise (so EmitGraphBody can fall through to the next family / unknown-node
// fallback). Implementations live in NIRGraphEmitter_Dataflow.cpp / _Control.cpp / _Util.cpp.
bool NIRGraphEmit_Dataflow(UNiagaraNode* Node, FNIRTextEmitter& Out);
bool NIRGraphEmit_Control(UNiagaraNode* Node, FNIRTextEmitter& Out);
bool NIRGraphEmit_Util(UNiagaraNode* Node, FNIRTextEmitter& Out);

// Walk every node on Graph in order, dispatching each to the three family emitters above.
// Unhandled node classes emit "# unknown-node <ClassName> @(x, y)" and a matching warning.
void EmitGraphBody(UNiagaraGraph* Graph, FNIRTextEmitter& Out);

// Open "graph <UsageName> { ... }", call EmitGraphBody, close. Resolves the graph and usage
// directly from Script.
void EmitScriptGraphScope(const UNiagaraScript* Script, FNIRTextEmitter& Out);
