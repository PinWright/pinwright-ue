// Copyright (c) 2026 Alexander Penkin. MIT License.

// BpirTypes.h - Core data structures for the Blueprint IR (BPIR) compiler/decompiler

#pragma once

#include "CoreMinimal.h"
#include "Compiler/BpirTypeSpec.h"

class UEdGraphNode;
class UEdGraphPin;

// All BPIR opcodes — one per instruction type
enum class EBpirOpcode : uint8
{
    Call,              // Impure function call
    Pure,              // Pure function call (no exec)
    Latent,            // Latent function call (has completion exec)
    Set,               // Variable set
    Get,               // Variable get (explicit)
    Branch,            // If-then-else
    Foreach,           // ForEachLoop macro
    ForeachBreak,      // ForEachLoopWithBreak macro
    While,             // WhileLoop macro
    Switch,            // Switch (enum/name/int/string)
    Sequence,          // Execution sequence
    Cast,              // Dynamic cast
    Select,            // Pure select (ternary)
    Macro,             // Named macro (DoOnce, FlipFlop, Gate, MultiGate)
    Timeline,          // Timeline node
    BreakStruct,       // Break struct into pins
    MakeStruct,        // Make struct from pins
    Self,              // Self reference
    Enum,              // Enum literal
    MakeArray,         // Make array from elements
    Subsystem,         // Get subsystem by type
    Return,            // Return from function
    End,               // Node-less end of the current exec chain
    ExecGoto,          // Explicit exec wire: exec -> @label
    Label,             // Label definition: @name:
    Comment,           // Comment: # text
    CallDispatcher,    // Call event dispatcher
    BindDispatcher,    // Bind to event dispatcher
    UnbindDispatcher,  // Unbind from event dispatcher
    SwitchInt,         // Switch on int
    SwitchString,      // Switch on string
    SwitchEnum,        // Switch on enum
    ClearDispatcher,   // Clear all bindings from event dispatcher
    FieldNotifySubscribe,   // Subscribe to FieldNotification (K2_AddFieldValueChangedDelegate)
    FieldNotifyUnsubscribe, // Unsubscribe from FieldNotification (K2_RemoveFieldValueChangedDelegate)
    Alias,             // Pure value alias: %tmp = $var or %tmp = %other (no node emitted)
};

// A named argument: PinName: Value
struct FBpirArg
{
    FString PinName;
    FString Value;     // Can be %ref, $var, literal, self, etc.
};

// An exec clause entry: PinName -> @Label[.TargetInputPinName]
struct FBpirExecTarget
{
    FString PinName;              // e.g. "true", "false", "body", "completed", "0", "1", "default"
    FString Label;                // Target label name (without @)
    FString TargetInputPinName;   // Optional: target node's input exec pin name; empty = first exec input
};

struct FBpirOpcodeTraits
{
    static bool IsImpure(EBpirOpcode Opcode)
    {
        switch (Opcode)
        {
        case EBpirOpcode::Call:
        case EBpirOpcode::Latent:
        case EBpirOpcode::Set:
        case EBpirOpcode::Branch:
        case EBpirOpcode::Foreach:
        case EBpirOpcode::ForeachBreak:
        case EBpirOpcode::While:
        case EBpirOpcode::Switch:
        case EBpirOpcode::SwitchInt:
        case EBpirOpcode::SwitchString:
        case EBpirOpcode::SwitchEnum:
        case EBpirOpcode::Sequence:
        case EBpirOpcode::Cast:
        case EBpirOpcode::Macro:
        case EBpirOpcode::Timeline:
        case EBpirOpcode::Return:
        case EBpirOpcode::ExecGoto:
        case EBpirOpcode::CallDispatcher:
        case EBpirOpcode::BindDispatcher:
        case EBpirOpcode::UnbindDispatcher:
        case EBpirOpcode::ClearDispatcher:
        case EBpirOpcode::FieldNotifySubscribe:
        case EBpirOpcode::FieldNotifyUnsubscribe:
            return true;
        case EBpirOpcode::Pure:
        case EBpirOpcode::Get:
        case EBpirOpcode::Select:
        case EBpirOpcode::BreakStruct:
        case EBpirOpcode::MakeStruct:
        case EBpirOpcode::Self:
        case EBpirOpcode::Enum:
        case EBpirOpcode::MakeArray:
        case EBpirOpcode::Subsystem:
        case EBpirOpcode::End:
        case EBpirOpcode::Label:
        case EBpirOpcode::Comment:
        case EBpirOpcode::Alias:
            return false;
        default:
            checkNoEntry();
            return false;
        }
    }
};

// Explicit node enabled-state markers carried by BPIR. The default Enabled
// value is intentionally not emitted; the two non-default states are the ones
// whose omission changes graph behaviour.
enum class EBpirNodeEnabledState : uint8
{
    Enabled,
    Disabled,
    DevelopmentOnly,
};

// A single parsed BPIR instruction
struct FBpirInstruction
{
    EBpirOpcode Opcode = EBpirOpcode::Call;
    FString ResultName;       // %name (without %) — empty for void calls
    FString FunctionName;     // Function/macro/event name
    FString TypeArg;          // For cast<Type>, make<Type>, break<Type>, enum Type::Value
    FString AliasRhs;         // For Alias: the RHS ref text (e.g. "$MyVar" or "%other")
    TArray<FBpirArg> Args;
    // Keyed by name so the shape-metadata replay can partition pre/post-AllocateDefaultPins entries and dedupe duplicates.
    TMap<FString, FString> NodeProps;
    TArray<FBpirExecTarget> ExecTargets;  // [pin -> @label] clauses
    int32 SourceLine = -1;    // 1-based line number for error reporting
    int32 SequenceCount = 0;  // For sequence(N) — number of outputs
    bool bHasAuthoredPosition = false;
    FVector2D AuthoredPosition = FVector2D::ZeroVector;

    // Optional trailing `disabled` / `devonly` marker. A missing
    // marker means the compiler keeps the engine default (Enabled).
    EBpirNodeEnabledState EnabledState = EBpirNodeEnabledState::Enabled;
    bool bHasEnabledState = false;

    // `message Iface::Func(...)` — the Call opcode emits UK2Node_Message instead of
    // UK2Node_CallFunction. Kept as a flag on the Call opcode rather than a separate
    // opcode so the whole call lane (resolution cascade, arg wiring, return pin,
    // exec threading) is shared; only the node class differs.
    bool bInterfaceMessage = false;

    // `parent_call Class::Function(...)` emits UK2Node_CallParentFunction
    // instead of the generic UK2Node_CallFunction. Kept on the shared Call
    // opcode so function resolution and pin wiring remain identical.
    bool bParentCall = false;

    // Optional advisory type annotation parsed from "%name: Type = ..." binding syntax.
    // The parser records it but does not validate against the resolved pin type in v1.
    FBpirTypeSpec DeclaredResultType;
    bool bHasDeclaredResultType = false;

    // Is this instruction impure (has exec pins)?
    bool IsImpure() const
    {
        return FBpirOpcodeTraits::IsImpure(Opcode);
    }

    // Does this instruction have explicit exec targets (multi-output)?
    bool HasExecTargets() const { return ExecTargets.Num() > 0; }
};

// Emit-pass state for a single instruction — kept separate from FBpirInstruction
// so that the IR remains a pure data description.
struct FEmittedNodeInfo
{
    UEdGraphNode* Node = nullptr;
    UEdGraphPin* PrimaryOutputPin = nullptr;  // The default output data pin (ReturnValue)
    UEdGraphPin* ExecOutputPin = nullptr;     // The default exec output pin (for auto-chaining)
    bool bSkipWireDataPins = false;           // Set by handlers that wire their own pins (FormatText)
};

// Entry point kind
enum class EBpirEntryKind : uint8
{
    Event,            // entry event BeginPlay()
    CustomEvent,      // entry custom_event MyEvent(...)
    Override,         // entry override Foo(...)
    Function,         // entry function Foo(...) -> ReturnType
    Construction,     // entry construction ConstructionScript()
    ComponentEvent,   // entry component_event Box.OnOverlap(...)
    WidgetEvent,      // entry widget_event Button.OnClicked()
    KeyPressed,       // entry key_pressed SpaceBar()
    KeyReleased,      // entry key_released SpaceBar()
    InputAction,      // entry input_action /Game/Input/IA_Move.IA_Move()
    Macro,            // entry macro MacroName(inputs) -> (outputs)
};

// Carries `@meta(...)` / `@flags(...)` decorators parsed off the line above an
// `entry …` signature. Field-by-field contract is documented in the BPIR plan's
// "Decorator grammar contract" section; the compile chunk reads these names
// verbatim, so they must not drift.
struct FBpirEntryMetadata
{
    // @meta(...) — empty / default when not present
    FText Category;
    FText Tooltip;
    FText Keywords;
    FText CompactNodeTitle;
    FString DeprecationMessage;

    // @flags(...) bag — default values match engine defaults
    enum class EAccess : uint8 { Default, Public, Protected, Private };
    EAccess Access = EAccess::Default;        // Default = no access bit explicit
    bool bPure = false;
    bool bConst = false;
    bool bExec = false;
    bool bCallInEditor = false;
    bool bThreadSafe = false;
    bool bUnsafeDuringActorConstruction = false;
    bool bDeprecated = false;

    // True if @meta(...) appeared (regardless of whether any key was present)
    bool bMetaPresent = false;
    // True if @flags(...) appeared
    bool bFlagsPresent = false;
};

// A parsed entry block containing instructions
struct FBpirEntryBlock
{
    EBpirEntryKind Kind = EBpirEntryKind::Event;
    FString Name;                // Event/function name
    // For functions/overrides: return type. Default-constructed (IsEmpty()) means
    // "no -> clause present at all" — distinct from an explicit `-> void`.
    FBpirTypeSpec ReturnType;
    FString ComponentName;       // For component_event/widget_event: component/widget name
    bool bHasAuthoredEntryPosition = false;
    FVector2D AuthoredEntryPosition = FVector2D::ZeroVector;

    // Optional enabled-state marker on the entry signature. As with body
    // instructions, the default Enabled state is omitted from emitted BPIR.
    EBpirNodeEnabledState EnabledState = EBpirNodeEnabledState::Enabled;
    bool bHasEnabledState = false;

    // Parameters: array of {Type, Name}. Name stays a string; Type is the structured spec.
    struct FParam
    {
        FBpirTypeSpec Type;
        FString Name;
    };
    TArray<FParam> Params;

    // Output parameters (for macros): {Type, Name}
    TArray<FParam> OutputParams;

    // Exec output path names for multi-exit macros (e.g., ["IsValid", "IsNotValid"])
    TArray<FString> ExecOutputNames;

    // Optional named Enhanced Input event roots (e.g. Triggered -> @triggered).
    TArray<FBpirExecTarget> EntryExecTargets;

    // Instructions in this block
    TArray<FBpirInstruction> Instructions;

    // Label index: label name -> index into Instructions array
    TMap<FString, int32> LabelIndex;

    // Value index: %name -> index into Instructions array
    TMap<FString, int32> ValueIndex;

    // @meta(...) / @flags(...) decorators consumed off the line(s) above the
    // `entry …` signature. Default-constructed when no decorators were present.
    FBpirEntryMetadata Metadata;
};
