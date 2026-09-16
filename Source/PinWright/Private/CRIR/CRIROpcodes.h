// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "IrCore/IrCompileDiagnostic.h"

// CRIR (Control Rig IR) node opcodes the compiler / decompiler round-trip.
// Event entry nodes (Forwards Solve, Backwards Solve, Construction) are
// themselves USTRUCT-backed `unit` instructions identified by struct path; no
// dedicated event opcode.
enum class ECRIROpcode : uint8
{
    Unit,           // `%localId = unit /Script/Path.RigUnit_X(arg=val, wire_in_X=%n.pin) [@(x,y)]`
    Var,            // `%localId = var Name Type [= literal] [@(x,y)]`
    Reroute,        // `%localId = reroute <Type> [= <literal>] [@(x,y)]`
    Comment,        // `comment "<text>" size=(w,h) color=(r,g,b,a) [@(x,y)]` — no LocalId.
    If,             // `%localId = if <Type> [@(x,y)]`
    Select,         // `%localId = select <Type> [@(x,y)]`
    Enum,           // `%localId = enum <EnumPath> [= <Value>] [@(x,y)]`
    InvokeEntry,    // `%localId = invoke_entry <EntryName> [@(x,y)]`
    Template,       // `%localId = template <Notation> [(args)] [@(x,y)]` — Notation stored in StructPath.
    Dispatch,       // `%localId = dispatch <FactoryStructName> [(args)] [@(x,y)]` — factory struct name stored in StructPath.
    Collapse,       // `%localId = collapse "<NodeName>" [@(x,y)] { ...body... }` — NodeName stored in VarName; body in Children.
    FunctionRef,    // `%localId = function_ref <FunctionName>` (same-asset) or `%localId = function_ref <HostPath>::<FunctionName>` (external). Token stored in StructPath.
    FunctionEntry,  // `function_entry [(args)] [@(x,y)]` — no LocalId. Args carry pin defaults on the auto-created entry node.
    FunctionReturn, // `function_return [(args)] [@(x,y)]` — no LocalId. Args carry pin defaults on the auto-created return node.
    ExposedPin,     // `exposed_pin <Name>: <direction> <CPPType> [object=<path>] [= <default>]` — no LocalId. Declares one entry of the function signature; only valid inside `rig_function` bodies.
};

// Direction token on an `exposed_pin` line, encoded as one of `input`, `output`,
// `io`. Mirrors ERigVMPinDirection but kept narrow (the other engine variants —
// Visible, Hidden, Invalid — are not exposable on a function signature).
enum class ECRIRExposedPinDirection : uint8
{
    Input,
    Output,
    IO,
};

// Top-level CRIR block kinds. RigGraph carries a single URigVMGraph model;
// RigHierarchy is a flat list of bone/null/control/socket elements with no
// model-name attribute. RigFunction is one named function-library entry; its
// body re-uses the same instruction shape as RigGraph and can recursively
// nest `collapse { ... }` sub-graph blocks.
enum class ECRIREntryKind : uint8
{
    RigGraph,
    RigHierarchy,
    RigFunction,
};

// Supported hierarchy element kinds. Reference / Connector / Physics are out
// of scope and emitted by the decompiler as `# TODO` comments rather than
// element instructions.
enum class ECRIRElementKind : uint8
{
    Bone,
    Null,
    Control,
    Socket,
    Curve,
};

// One argument on a `unit` or `var` instruction. Wire arguments
// (`wire_in_X=%nodeName.pinName`) decompose into `bIsLocalRef=true` plus the
// node / pin parts so the compiler can route them through
// URigVMController::AddLink without re-parsing the raw text. Literals and
// scalar value pins keep `RawText` only and `bIsLocalRef=false`.
struct FCRIRArg
{
    FString Name;
    FString RawText;
    bool bIsLocalRef = false;
    FString LocalRefNode;
    FString LocalRefPin;
};

// Optional `@(x,y)` position annotation. Distinct struct (rather than
// AGIR's `bHasPosition + FVector2D`) so the parser can emit a fully-zero
// FCRIRPosition with bSet=false without dragging in FVector2D in this header.
struct FCRIRPosition
{
    float X = 0.f;
    float Y = 0.f;
    bool bSet = false;
};

// One instruction inside a `rig_graph { ... }` block. Most opcodes are flat;
// `Collapse` carries a recursive Children array for its `rig_subgraph` body.
struct FCRIRInstruction
{
    ECRIROpcode Opcode = ECRIROpcode::Unit;
    FString LocalId;            // %nNN local id (without leading %).

    // Unit form fields.
    FString StructPath;         // Full UStruct path, e.g. /Script/ControlRig.RigUnit_BeginExecution.

    // Var form fields. Also re-used by `ExposedPin`: VarName=pin name, VarType=
    // CPPType, VarDefault=default literal (or empty). ExposedPin additionally
    // uses ExposedDirection and ExposedTypeObjectPath below.
    FString VarName;
    FString VarType;            // Raw type spec text (parsed via IrTypeSpecParser by the compiler).
    FString VarDefault;         // Optional literal RHS after `=`; empty when absent.

    // ExposedPin form fields.
    ECRIRExposedPinDirection ExposedDirection = ECRIRExposedPinDirection::Input;
    FString ExposedTypeObjectPath;  // Optional path to CPPTypeObject (script struct / enum / class) — empty for primitive types.

    TArray<FCRIRArg> Args;
    FCRIRPosition Position;
    int32 SourceLine = -1;

    // Populated for `Collapse` only — the recursive body of the rig_subgraph.
    TArray<FCRIRInstruction> Children;
};

// One element inside a `rig_hierarchy { ... }` block. Phase A stores all
// per-element knobs (location/rotation/scale, control settings, etc.) as raw
// strings keyed by attribute name; typed decoding (FRigControlSettings,
// FRigControlValue) is the compiler's job in Phase B and is out of scope here.
struct FCRIRElementInstruction
{
    ECRIRElementKind Kind = ECRIRElementKind::Bone;
    FString Name;
    FString Parent;
    TMap<FString, FString> Attributes;
    int32 SourceLine = -1;
};

struct FCRIREntryBlock
{
    ECRIREntryKind Kind = ECRIREntryKind::RigGraph;
    FString Name;                               // RigVMGraph model name; empty for RigHierarchy.
    TArray<FCRIRInstruction> Instructions;      // Populated only for RigGraph blocks.
    TArray<FCRIRElementInstruction> Elements;   // Populated only for RigHierarchy blocks.
    int32 SourceLine = -1;
};

struct FCRIRParseError : public FIrCompileDiagnostic
{
    FString Code;

    FCRIRParseError() = default;
    FCRIRParseError(int32 InLine, const FString& InMessage, const FString& InCode = FString())
        : FIrCompileDiagnostic(InLine, InMessage)
        , Code(InCode)
    {
    }

    // Single-line human-readable form, e.g. `[CRIR_UNDEFINED_REF] line 7: ...`
    // (the code is omitted when empty). The one canonical format for callers
    // that surface parse errors, so diagnostics don't drift across the module.
    FString ToString() const
    {
        return Code.IsEmpty()
            ? FString::Printf(TEXT("line %d: %s"), Line, *Message)
            : FString::Printf(TEXT("[%s] line %d: %s"), *Code, Line, *Message);
    }
};

// Joins parse errors into one `; `-separated string using FCRIRParseError::ToString.
inline FString JoinCRIRParseErrors(const TArray<FCRIRParseError>& Errors)
{
    TArray<FString> Lines;
    Lines.Reserve(Errors.Num());
    for (const FCRIRParseError& Err : Errors)
    {
        Lines.Add(Err.ToString());
    }
    return FString::Join(Lines, TEXT("; "));
}
