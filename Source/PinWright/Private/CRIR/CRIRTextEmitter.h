// Copyright (c) 2026 Alexander Penkin. MIT License.

// CRIRTextEmitter.h
//
// Pure text-formatting helpers for CRIR (Control Rig IR). The decompiler
// (CRIRDecompiler, Wave 2F) calls these to assemble a `rig_graph "<Name>" { ... }`
// or `rig_hierarchy { ... }` block one line at a time. Stateless: no graph
// walking happens here — the caller resolves all references and passes
// already-stringified arg payloads.

#pragma once

#include "CoreMinimal.h"
#include "CRIR/CRIROpcodes.h"
#include "Rigs/RigHierarchyDefines.h"

struct FRigControlSettings;
struct FRigControlValue;

class FCRIRTextEmitter
{
public:
    // Top-level block headers / footers. RigGraph carries the model name; the
    // emitter quotes it via FIrTextUtils::Quote unconditionally so embedded
    // spaces / specials round-trip cleanly.
    static FString EmitRigGraphHeader(const FString& ModelName);
    static FString EmitRigHierarchyHeader();
    static FString EmitRigFunctionHeader(const FString& FunctionName);
    static FString EmitSubgraphHeader(const FString& NodeName);
    static FString EmitBlockFooter();

    // Per-instruction lines. Position is appended as ` @(x,y)` when bSet=true.
    // Indentation is the caller's responsibility — these helpers return the
    // raw line without the leading 4-space block indent.
    static FString EmitUnit(
        const FString& LocalId,
        const FString& StructPath,
        const TArray<FCRIRArg>& Args,
        const FCRIRPosition& Position);

    static FString EmitVar(
        const FString& LocalId,
        const FString& VarName,
        const FString& VarType,
        const FString& VarDefault,
        const FCRIRPosition& Position);

    // Wave-1 extended kinds. Each line uses the same `@(x,y)` position suffix
    // convention as unit/var. Type / value strings are passed pre-formatted so
    // the emitter remains a pure formatter — type / enum resolution belongs in
    // the decompiler. Optional `Args` array carries trailing `(arg=val, ...)`
    // wire and pin-default arg list (same shape as unit's arg list).
    static FString EmitReroute(
        const FString& LocalId,
        const FString& CPPType,
        const FString& DefaultValue,
        const TArray<FCRIRArg>& Args,
        const FCRIRPosition& Position);

    static FString EmitComment(
        const FString& Text,
        const FString& Size,
        const FString& Color,
        const FCRIRPosition& Position);

    static FString EmitIf(
        const FString& LocalId,
        const FString& CPPType,
        const TArray<FCRIRArg>& Args,
        const FCRIRPosition& Position);

    static FString EmitSelect(
        const FString& LocalId,
        const FString& CPPType,
        const TArray<FCRIRArg>& Args,
        const FCRIRPosition& Position);

    static FString EmitEnum(
        const FString& LocalId,
        const FString& EnumObjectPath,
        const FString& Value,
        const FCRIRPosition& Position);

    static FString EmitInvokeEntry(
        const FString& LocalId,
        const FString& EntryName,
        const TArray<FCRIRArg>& Args,
        const FCRIRPosition& Position);

    static FString EmitTemplate(
        const FString& LocalId,
        const FString& Notation,
        const TArray<FCRIRArg>& Args,
        const FCRIRPosition& Position);

    static FString EmitDispatch(
        const FString& LocalId,
        const FString& FactoryStructName,
        const TArray<FCRIRArg>& Args,
        const FCRIRPosition& Position);

    // Emits the head line of a `%localId = collapse "NodeName" [@(x,y)] {`
    // block. The recursive body and the trailing `}` are emitted by the caller.
    static FString EmitCollapseHeader(
        const FString& LocalId,
        const FString& NodeName,
        const FCRIRPosition& Position);

    // Same-asset reference: HostPath is empty -> emits `function_ref Name`.
    // External reference: HostPath non-empty -> emits `function_ref HostPath::Name`.
    static FString EmitFunctionRef(
        const FString& LocalId,
        const FString& FunctionName,
        const FString& HostPath,
        const TArray<FCRIRArg>& Args,
        const FCRIRPosition& Position);

    static FString EmitFunctionEntry(
        const TArray<FCRIRArg>& Args,
        const FCRIRPosition& Position);

    static FString EmitFunctionReturn(
        const TArray<FCRIRArg>& Args,
        const FCRIRPosition& Position);

    // Emit one entry of a function's exposed-pin signature. Format:
    // `exposed_pin <Name>: <direction> <CPPType> [object=<path>] [= <default>]`.
    // The direction token is `input` / `output` / `io`. ObjectPath / Default
    // clauses are omitted when their arg is empty.
    static FString EmitExposedPin(
        const FString& PinName,
        ECRIRExposedPinDirection Direction,
        const FString& CPPType,
        const FString& CPPTypeObjectPath,
        const FString& DefaultValue);

    static FString EmitElement(
        ECRIRElementKind Kind,
        const FString& Name,
        const FString& Parent,
        const TArray<TPair<FString, FString>>& Attributes);

    // Control-element multi-line emitter (ticket 4 / F-crir-control-mutation-write).
    // Top-level line carries `parent="..." type=... value=...` plus optional
    // `shape=...` and offset transform attrs; the sub-block `{ ... }` is
    // emitted only when SubBlockBodyOrEmpty is non-empty.
    static FString EmitControlElement(
        const FString& Name,
        const FString& Parent,
        ERigControlType Type,
        const FString& ValueLiteral,
        const FString& ShapeLiteralOrEmpty,
        const TArray<TPair<FString, FString>>& OffsetTransformAttrs,
        const FString& SubBlockBodyOrEmpty);

    // Format a control value using the typed-function-prefix grammar.
    static FString FormatControlValue(ERigControlType Type, const FRigControlValue& Value);

    // Emit the sub-block body for control settings. Returns empty string when
    // every settings field equals the default for `Type` (caller suppresses
    // the `{ ... }` block in that case).
    static FString FormatControlSettingsSubBlock(const FRigControlSettings& Settings, ERigControlType Type);

    // Static helpers exposed for tests / the decompiler's own per-call needs.
    static FString MakeLocalId(int32 Counter);
    static FString FormatLocalRef(const FString& Node, const FString& Pin);

    // Tuple grammar shared with CRIRDecompiler for `location=`/`rotation=`/
    // `scale=` element-line attributes and for the transform value bodies.
    static FString FormatVector(const FVector& V);
    static FString FormatRotator(const FRotator& R);

    // Indents every line of `Body` by 4 spaces. Used by callers assembling the
    // outer block from an inner per-instruction sequence.
    static FString IndentBlockBody(const FString& Body);
};
