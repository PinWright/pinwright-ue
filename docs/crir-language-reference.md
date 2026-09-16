---
type: guide
summary: "CRIR language reference: text IR for Control Rig graphs, rig hierarchy, and function library. Top-level blocks (rig_graph, rig_function, rig_hierarchy), nested rig_subgraph blocks, full RigVM node opcode coverage (unit/var/reroute/comment/if/select/enum/invoke_entry/template/dispatch/collapse/function_ref/function_entry/function_return), hierarchy element kinds with typed control-value grammar and flat curve values."
date: 2026-05-18
tags: [crir, compiler, controlrig, rigvm, ir, roundtrip, dispatch-node, control-rig, function-library, control-settings, variable-node, curve]
---

# Control Rig IR (CRIR) — Language Reference

## What is CRIR?

CRIR is a text-based intermediate representation for Unreal Control Rig assets. It captures the RigVM execution graph, the per-asset function library, nested collapse / sub-graph contents, and the rig element hierarchy in a single document that round-trips losslessly through `controlrig.compile_crir` and `controlrig.decompile_crir`. Wave 2 added the full RigVM node opcode set plus mutating `control` element authoring with typed-prefix value grammar; hierarchy curves are flat named float elements.

CRIR sits alongside [BPIR](wiki-src/bpir.md) (Blueprint), MGIR (Material), AGIR (AnimGraph), and the other ephemeral IRs documented in [IR authoring](ir-authoring.md). Like its siblings, CRIR text exists only as compile input and asset-dump output — never as durable project data.

---

## Why CRIR?

| Problem | Without CRIR | With CRIR |
|---------|--------------|-----------|
| Diff/review of Control Rig changes | Binary `.uasset` diff, opaque to reviewers | Stable text diff, line-by-line review |
| Scripted bulk authoring (rebind a pin across N rigs) | Open each asset in editor, manual edit | Decompile → text-edit → recompile, scriptable |
| Version-control history | Asset history is unreadable in `git log -p` | CRIR sidecar in asset-dump cache is greppable |
| Regression test fixtures | Snapshot of a `.uasset` is fragile across engine updates | CRIR text is stable across engine updates as long as opcodes match |

The same text is what `asset.dump_folder` writes to `<dump-root>/<rig-path>/crir.txt` (default root `<ProjectSavedDir>/PinWright/asset-dumps`) and what `controlrig.compile_crir` consumes — one canonical surface.

---

## Core principles

1. **Line-based.** One opcode per line, keyword-first. No multi-line statements except for brace-bounded blocks.
2. **Brace-bounded blocks.** `rig_graph` and `rig_hierarchy` use `{ ... }`; nesting is shallow (one level inside the document).
3. **SSA-style local ids inside graph blocks.** Each node gets a `%localId` valid only inside its enclosing `rig_graph`. Referenced from later opcodes as `%localId.PinName`.
4. **Struct-path-keyed unit identity.** `unit` opcodes reference RigVM units by their full struct path (`/Script/ControlRig.RigUnit_BeginExecution`), not by short display name. This is the same identity the engine uses internally and survives renames as long as the struct path is stable.
5. **Logical-equivalence-only round-trip.** CRIR round-trips guarantee logical equivalence only — comments and node colors are dropped on decompile. Node positions are preserved via the `@(x, y)` suffix. See [Round-trip semantics](#round-trip-semantics).

---

## Quick reference

A complete CRIR document with both block kinds:

```crir
rig_hierarchy {
    bone "root" parent="" location=(0.0, 0.0, 0.0) rotation=(0.0, 0.0, 0.0) scale=(1.0, 1.0, 1.0)
    bone "spine_01" parent="root" location=(0.0, 0.0, 12.0) rotation=(0.0, 0.0, 0.0) scale=(1.0, 1.0, 1.0)
    control "spine_ctrl" parent="root" shape="Circle_Thick" location=(0.0, 0.0, 12.0) rotation=(0.0, 0.0, 0.0) scale=(1.0, 1.0, 1.0)
    curve IKBlend value=0.75
}

rig_graph "RigVMModel" {
    %fwd = unit /Script/ControlRig.RigUnit_BeginExecution() @(-400, 0)
    %get = unit /Script/ControlRig.RigUnit_GetControlTransform(Control="spine_ctrl", Space=GlobalSpace) @(-150, 0)
    %set = unit /Script/ControlRig.RigUnit_SetBoneTransform(
        Bone="spine_01",
        Space=GlobalSpace,
        Weight=1.0,
        wire_in_ExecutePin=%fwd.ExecutePin,
        wire_in_Transform=%get.Transform
    ) @(150, 0)
}
```

The example shows the typical shape: a `rig_hierarchy` block listing supported hierarchy element kinds, followed by one `rig_graph` per RigVM model, with `unit` opcodes wired by `wire_in_*` arguments.

---

## Top-level blocks

### `rig_graph "<ModelName>" { ... }`

One block per `URigVMGraph` model on the asset. The default model name produced by the engine is `"RigVMModel"`; rigs that declare additional models (function libraries, sub-graphs explicitly added by the rigger) decompile to one `rig_graph` block per model. The model name is required and must be quoted.

Inside a `rig_graph` block, every line is one of:

- A `unit` instruction that creates a `URigVMUnitNode`.
- A `var` instruction that creates a `URigVMVariableNode`.
- A `# ` line comment, which the parser tolerates and discards.

Empty lines inside a `rig_graph` block are also tolerated.

### `rig_function "<Name>" { ... }`

One block per `URigVMLibraryNode` on the asset's function library (`URigVMFunctionLibrary` accessed via `Blueprint->GetRigVMClient()->GetLocalFunctionLibrary()`). The library is per-asset (not per-graph) and extends `URigVMGraph`; its top-level children are the function nodes themselves. The decompiler emits `rig_function` blocks alphabetically sorted by name, AFTER all `rig_graph` blocks. Inside, function bodies use the same instruction grammar as `rig_graph` plus `function_entry` / `function_return` opcodes for the entry and return nodes.

Compile path: get the function library's controller via `Client->GetControllerForGraph(Lib)`, call `AddFunctionToLibrary(FName, /*bMutable*/true, ZeroVector, false, false)` to materialize the library node, then get the body controller via `Client->GetControllerForGraph(FuncNode->GetContainedGraph())` to compile inner instructions into the function body.

### `rig_hierarchy { ... }`

Holds the rig element list — bones, nulls, controls, sockets, and curves. Unlike `rig_graph`, the block has no name; a Control Rig has exactly one `URigHierarchy`.

Wave 2 added compile-side `control` element authoring with full `FRigControlSettings` + `FRigControlValue` serialization. Curve support adds flat `value=` floats; hierarchy curves are `FRigCurveElement` values, not keyed `FRichCurve` animation curves. Controls and curves now round-trip alongside `bone`, `null`, and `socket`. See [`control`](#control) for the typed-prefix value grammar and sub-block field set.

An empty `rig_hierarchy {}` block is accepted and ignored on compile.

### `rig_subgraph "<Name>" { ... }`

Nested inside any `rig_graph`, `rig_function`, or another `rig_subgraph` body. Materializes the contained graph of a `URigVMCollapseNode` or `URigVMFunctionReferenceNode`. The block name matches the containing node's identifier. The parser tracks the enclosing scope through a `TArray<FParseFrame>` stack (ported from `AGIRParser:666-714`) so that `%localId` references resolve against the immediately enclosing graph, not the outermost `rig_graph`.

---

## Node opcodes (inside `rig_graph`)

CRIR recognizes the following node opcodes: `unit`, `var`, `reroute`, `comment`, `if`, `select`, `enum`, `invoke_entry`, `template`, `dispatch`, `collapse`, `function_ref`, `function_entry`, `function_return`. All RigVM node kinds are now covered. The catch-all decompiler fallback for unknown subclasses still emits `# TODO unsupported node kind: <ClassName>` and the compiler errors with `CRIR_UNSUPPORTED_OPCODE` on encounter.

**Decompiler dispatch order (derived-first).** The classifier must dispatch `URigVMNode` subclasses from most-derived to most-general because `URigVMUnitNode` and `URigVMDispatchNode` both inherit `URigVMTemplateNode`. The correct emission order is: `unit` (URigVMUnitNode) → `if` / `select` (URigVMDispatchNode special cases) → general `dispatch` (URigVMDispatchNode) → bare `template` (URigVMTemplateNode) → `var` / `reroute` / `enum` / `invoke_entry` → `collapse` (URigVMCollapseNode — also catches `URigVMAggregateNode` which inherits from CollapseNode) → `function_ref` → `function_entry` / `function_return` → catch-all TODO. `URigVMAggregateNode` rides the `collapse` arm via `Cast<URigVMCollapseNode>` (gated by `UE_RIGVM_AGGREGATE_NODES_ENABLED`, default `1` in 5.6). Aggregate class identity is documented-loss on round-trip — the inner unit nodes carry all runtime behavior.

### `unit`

Creates a `URigVMUnitNode` — the RigVM equivalent of a USTRUCT-backed function call. Event entry nodes (Forwards Solve, Backwards Solve, Construction) are also `unit` opcodes; their identity is carried by the struct path.

```crir
%localId = unit <struct-path>(<arg>, <arg>, ...) [@(x, y)]
```

The struct path is the full `/Script/<Module>.<StructName>` form. Examples in common use:

- `/Script/ControlRig.RigUnit_BeginExecution` — Forwards Solve event entry
- `/Script/ControlRig.RigUnit_InverseExecution` — Backwards Solve event entry
- `/Script/ControlRig.RigUnit_PrepareForExecution` — Construction event entry
- `/Script/ControlRig.RigUnit_GetBoneTransform`
- `/Script/ControlRig.RigUnit_SetBoneTransform`
- `/Script/ControlRig.RigUnit_GetControlTransform`

Three argument forms are accepted, each occupying one comma-separated slot:

| Form | Effect |
|---|---|
| `<PinName>=<value>` | Set the default value on `<PinName>`. The value is parsed by the same literal grammar as BPIR (numbers, quoted strings, enum identifiers, struct literals like `(X=1.0, Y=2.0, Z=3.0)`). |
| `wire_in_<PinName>=%sourceId.OutputPin` | Create a link from the named output pin of `%sourceId` into the input pin `<PinName>` on this node. |
| `wire_out_<PinName>=%targetId.InputPin` | Create a link from the output pin `<PinName>` on this node into the named input pin of `%targetId`. |

`@(x, y)` is an optional position annotation (see [Position annotation](#position-annotation)).

**Examples — value-default arg vs. wired-input arg:**

```crir
# Value-default: Weight pin gets a literal float
%set = unit /Script/ControlRig.RigUnit_SetBoneTransform(
    Bone="spine_01",
    Weight=1.0
)

# Wired-input: the exec pin is linked from %fwd's exec output
%set = unit /Script/ControlRig.RigUnit_SetBoneTransform(
    Bone="spine_01",
    wire_in_ExecutePin=%fwd.ExecutePin
)
```

### `var`

Creates a `URigVMVariableNode` — a graph-local read of a variable declared on the rig.

```crir
%localId = var <Name> <Type> [= <literal>]
```

`<Type>` follows the BPIR type grammar (`bool`, `float`, `double`, `int`, `FString`, `FVector`, `FTransform`, `FName`, etc.). The optional `= <literal>` provides the default value when the variable is not yet bound; if omitted, the variable's existing default on the rig is used.

```crir
%speed = var WalkSpeed float = 6.0
%loc = var SpawnLocation FVector
```

**Round-trip requirement — variable must pre-exist on the target BP.** The `var` opcode only creates the get-variable node; it does NOT register a member variable on the destination Control Rig Blueprint. `URigVMController::AddVariableNode` (engine, `RigVMController.cpp:1871`) looks up the variable via `GetVariableByName` and aborts compile with `"Cannot add variable '<name>' with type '<type>' - variable does not exist."` when no matching member exists. Callers must seed the member variable on the target BP out-of-band before compile (e.g. `FBlueprintEditorUtils::AddMemberVariable`); this applies symmetrically when building source BPs programmatically for fixtures. CRIR has no member-variable declaration block — the grammar covers nodes only, not the rig's variable schema. See `Tests/Assets/TestCRIRVariableNode.cpp` for the canonical fixture pattern (pre-register `MyBool` on both source and target before `AddVariableNode`).

### `reroute`

Creates a `URigVMRerouteNode` — a graph-organization pass-through with a single typed `Value` pin used as both input and output.

```crir
%localId = reroute <Type> [= <literal>] [(arg=val, ...)] [@(x, y)]
```

The optional `= <literal>` provides the constant value when the reroute has no incoming wire (the engine calls this `IsLiteral()`). The optional trailing arg list carries `wire_in_Value=%src.pin` for the wired pass-through case.

```crir
%route = reroute float = 1.0 @(120, 40)
%route = reroute float (wire_in_Value=%fwd.Weight) @(120, 40)
```

### `comment`

Creates a `URigVMCommentNode` — a purely cosmetic colored grouping box with author text. Comments have no incoming wires and no `%localId` prefix.

```crir
comment "<text>" [size=(w, h)] [color=(r, g, b, a)] [@(x, y)]
```

```crir
comment "TODO: collapse this section" size=(300, 200) color=(0.2, 0.2, 0.2, 0.6) @(-100, -100)
```

### `if`

Creates a `URigVMDispatchNode` running the `RigVMDispatch_If` factory — picks between two values based on a `Condition` bool.

```crir
%localId = if <CPPType> [(arg=val, ...)] [@(x, y)]
```

`<CPPType>` is the result-pin type (`float`, `int`, etc.). Args wire `Condition`, `True`, `False`, and `Result`.

```crir
%pick = if float (Condition=true, True=1.0, False=0.0) @(0, 0)
```

### `select`

Creates a `URigVMDispatchNode` running the `RigVMDispatch_SelectInt32` factory — picks from a value array based on an integer `Index`.

```crir
%localId = select <CPPType> [(arg=val, ...)] [@(x, y)]
```

```crir
%pick = select int (Index=0, Values=(10,20,30)) @(0, 0)
```

> **UE 5.6 implementation note (applies to `if` and `select`):** `URigVMController::AddIfNode` and `AddSelectNode` do NOT produce `URigVMIfNode` / `URigVMSelectNode` — those classes are `UDEPRECATED_*` in 5.6. They produce `URigVMDispatchNode` instances dispatched through factories `FRigVMDispatch_If` and `FRigVMDispatch_SelectInt32` (and other Select variants). The CRIR decompiler discriminates these via `Cast<URigVMDispatchNode>` + `Factory->GetScriptStruct()->GetName()` matching the constants `kRigVMDispatchIfName` (`RigVMDispatch_If`) / `kRigVMDispatchSelectPrefix` (`RigVMDispatch_Select*`). Anything that casts to the deprecated `URigVMIfNode` / `URigVMSelectNode` in UE 5.6 will silently miss every real If/Select instance. Same general pattern as `URigVMUnitNode : URigVMTemplateNode` — the runtime classes consolidated into TemplateNode/DispatchNode while keeping the convenience `Add*` APIs.

### `enum`

Creates a `URigVMEnumNode` — a constant enum value source.

```crir
%localId = enum <EnumObjectPath> [= <Value>] [@(x, y)]
```

```crir
%axis = enum /Script/Engine.EAxis = X @(0, 0)
```

### `invoke_entry`

Creates a `URigVMInvokeEntryNode` — calls a named entry point on the same Control Rig from inside another entry.

```crir
%localId = invoke_entry <EntryName> [(arg=val, ...)] [@(x, y)]
```

```crir
%inv = invoke_entry SubRoutine (wire_in_ExecuteContext=%fwd.ExecuteContext) @(300, 0)
```

### `template`

Creates a bare `URigVMTemplateNode` — a polymorphic node bound to a template notation but not yet specialized to a unit struct or dispatch factory.

```crir
%localId = template <Notation> [(arg=val, ...)] [@(x, y)]
```

`<Notation>` is the template's notation string (the engine identity used by `URigVMController::AddTemplateNode`). The decompiler emits this form only for nodes whose runtime class is exactly `URigVMTemplateNode` — `URigVMUnitNode` and `URigVMDispatchNode` instances render as `unit` / `dispatch` (or `if` / `select`) respectively.

### `dispatch`

Creates a `URigVMDispatchNode` outside the `if` / `select` special cases.

```crir
%localId = dispatch <FactoryStructName> [(arg=val, ...)] [@(x, y)]
```

`<FactoryStructName>` is the dispatch factory struct name without `/Script/` prefix (e.g. `RigVMDispatch_ArrayAdd`). The `if` / `select` forms are syntactic sugar for the `RigVMDispatch_If` and `RigVMDispatch_Select*` factories; everything else uses the bare `dispatch` opcode.

### `collapse`

Creates a `URigVMCollapseNode` — a sub-graph wrapper.

```crir
%localId = collapse "<Name>" [@(x, y)] {
    <inner instructions>
    ...
}
```

The trailing `{ ... }` block is a `rig_subgraph "<Name>"` body that holds the collapsed inner graph. The compile path materializes child instructions as nodes in the PARENT graph first, then calls `Controller->CollapseNodes(NodeNames, Name, false, false, false)` to wrap them. There is no reverse "create-empty-collapse-then-add-children" API.

### `function_ref`

Creates a `URigVMFunctionReferenceNode` — a call site referencing a function defined in this asset's library or another asset's library.

```crir
%localId = function_ref <Name> [(arg=val, ...)] [@(x, y)]
%localId = function_ref <HostPath>::<Name> [(arg=val, ...)] [@(x, y)]
```

The bare-`Name` form references a function in the same asset's library (matched against a `rig_function "<Name>"` block in the same document). The `<HostPath>::<Name>` form references a function on a different Control Rig asset, with `<HostPath>` the full asset path of the host. Inner pin defaults follow the standard `wire_in_*` / `wire_out_*` and `<PinName>=<value>` arg grammar.

### `function_entry` and `function_return`

Materialize a function body's entry and return nodes. They appear only inside `rig_function` and `rig_subgraph` blocks.

```crir
function_entry [@(x, y)]
function_return [@(x, y)]
```

These opcodes have no `%localId` because their identity is fixed by position — every function body has exactly one entry and one return node. `URigVMFunctionEntryNode` and `URigVMFunctionReturnNode` have private constructors with `friend class URigVMController`; no `Add*` RPC exists. After `CollapseNodes` or `AddFunctionToLibrary` auto-creates them, the compiler locates them via `Graph->GetEntryNode()` / `GetReturnNode()` and replays pin defaults via `SetPinDefaultValue`. The compiler errors if a body block contains a duplicate `function_entry` or `function_return`.

### `exposed_pin`

Declares one entry of a function's signature. Emitted by the decompiler before `function_entry` for each pin on a `URigVMLibraryNode` (excluding the auto-managed execute-context pin). Only meaningful inside `rig_function` bodies — `URigVMController::AddExposedPin` rejects top-level graphs.

```crir
exposed_pin <Name>: <input|output|io> <CPPType> [object=<TypeObjectPath>] [= <DefaultLiteral>]
```

| Token | Meaning |
|---|---|
| `<Name>` | Pin name as it appears on the library node and on call-site `function_ref` nodes. |
| direction | Engine `ERigVMPinDirection`. `input` flows the value into the body; `output` flows it out; `io` mirrors. |
| `<CPPType>` | Plain CPPType string passed to `AddExposedPin` (e.g. `int32`, `float`, `FVector`). |
| `object=<path>` | Optional `CPPTypeObjectPath` for struct / enum / class types. Omit for primitives. |
| `= <DefaultLiteral>` | Optional initial default value. Mirrored onto the auto-created entry node by `RefreshFunctionPins`. |

The compiler calls `URigVMController::AddExposedPin(...)` once per `exposed_pin` line; this implicitly scaffolds the entry/return nodes the first time it's called. Subsequent `function_entry` / `function_return` args land on those pins via `SetPinDefaultValue`. Authored entry-node defaults on `Output`-direction pins survive a round-trip because the decompiler emits the raw `GetDefaultValue()` string regardless of its override predicate (`PinHasAuthoredDefaultOverride`, `CRIRDecompiler.cpp`) — that predicate short-circuits for output pins through `CanProvideDefaultValue`. It is deliberately not the engine's `URigVMPin::HasDefaultValueOverride()`, which returns false for every pin whenever the shipped-off `RigVM.EnablePinOverrides` CVar is off; see `docs/lessons.md`.

---

## Element opcodes (inside `rig_hierarchy`)

CRIR supports `bone`, `null`, `control`, `socket`, and `curve`. `reference`, `connector`, and physics element kinds are still emitted as TODO comments.

### `bone`

```crir
bone "<Name>" parent="<ParentName>" location=(x, y, z) rotation=(p, y, r) scale=(x, y, z)
```

| Attribute | Meaning |
|---|---|
| `Name` | Bone name. |
| `parent` | Parent element name. Empty (`""`) means root — no parent. |
| `location` | Initial local-space translation as `(x, y, z)` floats. |
| `rotation` | Initial local-space rotation as Euler `(pitch, yaw, roll)` floats in degrees. |
| `scale` | Initial local-space scale as `(x, y, z)` floats. |

### `null`

```crir
null "<Name>" parent="<ParentName>" location=(x, y, z) rotation=(p, y, r) scale=(x, y, z)
```

Same attribute set as `bone`. Nulls are non-deforming transform anchors used for control parenting and IK targets.

### `control`

```crir
control "<Name>" parent="<ParentName>" type=<type-token> value=<type-token>(<value-args>) [<settings-attr>=<value> ...] [{
    <sub-block-key>=<value>
    ...
}]
```

Wave 2 added compile-side `control` authoring. The opcode now carries a `type=` token, a `value=` literal using the typed-prefix value grammar below, optional inline settings attributes, and an optional trailing `{ ... }` sub-block of FRigControlSettings fields. The sub-block is recognized through the same `FParseFrame` stack used for nested graphs, routed by `FParseFrame::ElementAttributes`.

**Typed-prefix value grammar.** `ERigControlType` has 11 variants (`Rigs/RigHierarchyDefines.h:194-207`). The leading token on `value=` selects the variant, and the parenthesized args follow per-type rules:

| Type token | Storage | Args |
|---|---|---|
| `bool` | `bool` | `(true)` / `(false)` |
| `float` | `float` | `(1.5)` |
| `int` | `int32` | `(7)` |
| `vector2d` | `FVector3f` (XY) | `(0.0, 0.0)` |
| `position` | `FVector3f` | `(0.0, 0.0, 0.0)` |
| `scale` | `FVector3f` | `(1.0, 1.0, 1.0)` |
| `scale_float` | `FVector3f` (uniform X) | `(1.0)` |
| `rotator` | `FVector3f` (PYR) | `(p=0.0, y=0.0, r=0.0)` |
| `transform` | `FTransform_Float` | `(loc=(...), rot=(p=,y=,r=), scale=(...))` |
| `transform_no_scale` | `FTransformNoScale_Float` | `(loc=(...), rot=(p=,y=,r=))` |
| `euler_transform` | `FEulerTransform_Float` | `(loc=(...), rot=(p=,y=,r=), scale=(...))` |

Low-arity types use positional args (e.g. `position(1.0, 2.0, 3.0)`); compound types use keyword args (e.g. `transform(loc=(0,0,0), rot=(p=0,y=0,r=0), scale=(1,1,1))`). `FRigControlValue` is a templated `Set<T>(T)` over a 32-float storage blob — use static `FRigControlValue::Make<T>(value)` as the constructor entry; it is NOT a tagged union with per-variant typed setter overloads.

**FRigControlSettings sub-block (~20 fields, alphabetically sorted, defaults elided).** The trailing `{ ... }` carries the FRigControlSettings serializable fields. Fields with default values are elided on decompile, so the typical sub-block is small. The full set:

`AnimationType`, `bDrawLimits`, `bGroupWithParentControl`, `bIsTransientControl`, `bRestrictSpaceSwitching`, `bShapeVisible`, `bUsePreferredRotationOrder`, `ControlEnum`, `Customization`, `DisplayName`, `DrivenControls`, `FilteredChannels`, `LimitEnabled` (positional `TArray<FRigControlLimitEnabled>`, sized by `SetupLimitArrayForType` per type), `MaximumValue`, `MinimumValue`, `PreferredRotationOrder`, `PrimaryAxis`, `ShapeColor`, `ShapeName`, `ShapeVisibility`.

`ControlType` is set from the opcode's `type=` token (not the sub-block). **Load-bearing:** `URigHierarchyController::AddControl` copies `Settings` as-is via `NewElement->Settings = InSettings`; it never derives `Settings.ControlType` from the value or any other source. Callers MUST set `Settings.ControlType = <type>` explicitly before calling `AddControl`, even if the `FRigControlValue` already encodes the type. Verified at `RigHierarchyController.cpp:480-556`.

**Excluded fields** (do not serialize): `ShapeTransform` is transient — it flows only via `AddControl`'s `InShapeTransform` parameter and never round-trips through `FRigControlSettings` serialization. `bIsCurve` is transient. `PreviouslyDrivenControls` has no `UPROPERTY`. `bAnimatable_DEPRECATED` and `bShapeEnabled_DEPRECATED` are deprecated. Note that `SecondaryAxis` does NOT exist — only `PrimaryAxis`. The "~40 fields" count from earlier specs included `FRigControlValueStorage`'s 32 internal floats plus deprecated/transient — actual serializable surface is ~20.

### `curve`

```crir
curve "<Name>" value=<float>
```

Control Rig hierarchy curves are `FRigCurveElement` float values. They are not keyed animation `FRichCurve` tracks, so CRIR does not expose `default`, `evaluationMode`, or `keys` fields. The decompiler reads `URigHierarchy::GetCurveValue` and emits the current value as `value=...`; compile defaults a missing `value=` to `0.0` and creates the curve with `URigHierarchyController::AddCurve`.

### `socket`

```crir
socket "<Name>" parent="<ParentName>" location=(x, y, z) rotation=(p, y, r) scale=(x, y, z)
```

Sockets attach external content (meshes, niagara emitters) to a rig point. Same attribute shape as `bone` and `null`.

> **Out of scope:** `reference`, `connector`, and physics element kinds (`physics_body`, `physics_constraint`) are not emitted. The decompiler emits `# TODO unsupported element kind: <ElementType>` for these.

---

## Pin wiring grammar

Pin links between nodes inside a `rig_graph` are expressed as `wire_in_*` and `wire_out_*` arguments on the `unit` opcode. Both directions are accepted; the decompiler always emits the `wire_in_*` form (sink-side) for determinism.

**Direction:**

- `wire_in_<PinName>=%source.OutputPin` — link `source.OutputPin → this.PinName` (data or exec flows into this node).
- `wire_out_<PinName>=%target.InputPin` — link `this.PinName → target.InputPin` (data or exec flows out of this node into the target).

**Pin paths:**

Pin path references inside CRIR text use the local node id and the pin's name relative to the node. Sub-pins (struct members exposed as separate pins) are dotted: `%get.Transform.Translation`. CRIR does not use full graph-path pin references (`/RigVMModel/Node.Pin`) — the local-id form is sufficient because pin references are resolved within the enclosing `rig_graph` block.

**The exec pin is named `ExecutePin`, and that is what the decompiler emits.** `FRigUnitMutable`'s exec property is `FRigVMExecutePin ExecutePin` (`RigUnit.h`), so every mutable unit node carries a pin of that name and decompiles to `wire_in_ExecutePin=%<src>.ExecutePin`. The legacy `ExecuteContext` spelling is still **accepted on input**: `URigVMNode::FindPin` falls back to `FindExecutePin()` for either name (`RigVMNode.cpp`). Do not assert on the literal `ExecuteContext` — hand-authored CRIR using it compiles fine, so the mismatch only surfaces after a decompile. `invoke_entry` is the exception: `URigVMInvokeEntryNode` synthesises its exec pin from `FRigVMStruct::ExecuteContextName`, so there both spellings name the same thing and `ExecuteContext` is what round-trips.

**Worked example — both directions of the same link:**

```crir
# Equivalent — sink-side (canonical, decompiler output):
%set = unit /Script/ControlRig.RigUnit_SetBoneTransform(
    wire_in_ExecutePin=%fwd.ExecutePin
)

# Equivalent — source-side (accepted on input):
%fwd = unit /Script/ControlRig.RigUnit_BeginExecution(
    wire_out_ExecutePin=%set.ExecutePin
)
```

Both forms produce the same link in the resulting `URigVMGraph`. Authors writing CRIR by hand can pick whichever direction reads more naturally; round-tripping through decompile normalizes to `wire_in_*`.

The compiler errors with `CRIR_UNRESOLVED_PIN` if `%source` is not declared earlier in the block, or if the named pin does not exist on the resolved node.

---

## Position annotation

```crir
%n0 = unit /Script/ControlRig.RigUnit_BeginExecution() @(-400, 0)
```

`@(x, y)` is an optional integer-coordinate suffix on any `unit` or `var` line inside a `rig_graph` block. It sets the node's editor-canvas position. Coordinates are in graph-space pixels matching the editor.

When `compile_crir` runs with `runLayout=true` (the default), nodes missing `@(x, y)` are placed by the post-compile auto-layout pass. When `runLayout=false`, missing positions default to `(0, 0)`, which usually produces a stack of overlapping nodes — set positions explicitly or leave layout enabled.

The position annotation is **emitted on decompile** for every node, carrying whatever `URigVMNode::GetPosition` returns. This preserves authored layout across a round-trip — recompiling the decompiled text places nodes back at the same coordinates and `runLayout` becomes a no-op for nodes that already have explicit positions.

---

## Compile options (`controlrig.compile_crir`)

| Param | Type | Default | Purpose |
|---|---|---|---|
| `text` | string | required | CRIR document text. |
| `context` | string | required | Target Control Rig Blueprint asset path (e.g. `/Game/Rigs/CR_Hero`). Must resolve to a `UControlRigBlueprint`. |
| `mode` | `replace` \| `extend` | `replace` | `replace` clears existing `URigVMUnitNode` / `URigVMVariableNode` entries in each named model before adding the CRIR's nodes. `extend` appends without clearing. |
| `runLayout` | bool | `true` | Auto-position nodes that lack an `@(x, y)` annotation after compile. |
| `save` | bool | `false` | Mark the asset dirty and save it once compile finishes. |

**Returns:** `{ mode, assetPath, blocksCompiled, nodesCreated, warnings[] }`.

`blocksCompiled` is the count of top-level CRIR blocks consumed. `nodesCreated` counts graph node instructions; hierarchy elements are created through `URigHierarchyController` and do not contribute to `nodesCreated`.

---

## Decompile (`controlrig.decompile_crir`)

| Param | Type | Default | Purpose |
|---|---|---|---|
| `assetPath` | string | required | Control Rig Blueprint asset to decompile. |

**Returns:** `{ assetPath, text, warnings[] }`.

The same code path also writes the asset-dump sidecar `crir.txt` into the asset's dump dir under the configured dump root whenever `asset.dump` or `asset.dump_folder` covers a Control Rig asset, so agents can read CRIR from the cache without invoking `controlrig.decompile_crir` directly. Prefer the cache where possible and fall back to a live decompile if the cache is stale.

`warnings[]` lists any `# TODO unsupported node kind: ...` or `# TODO unsupported element kind: ...` lines emitted into the text, surfaced as structured warnings for tooling.

---

## Round-trip semantics

CRIR guarantees **logical equivalence only**. Two specific properties hold:

1. **Decompile is deterministic.** Two consecutive `controlrig.decompile_crir` calls on the same asset produce byte-equal text. Block ordering, instruction ordering inside a block, and argument ordering on a `unit` line are all stable.
2. **Decompile → compile → decompile is byte-equal.** The standard round-trip test compiles the decompiled text into a fresh Control Rig asset and decompiles that, expecting byte equality. This is the authoritative correctness test for CRIR.

What is **not** preserved across a round-trip:

- **Comments.** `# ...` lines in CRIR input are tolerated by the parser but are not stored in any way that survives a decompile.
- **Node colors.** RigVM node tinting authored in the editor is dropped.

Authored node positions **are** preserved across the round-trip via the `@(x, y)` suffix.

This matches the project's general IR rule: round-trips guarantee logical equivalence only; visual fidelity is out of scope.

---

## Errors

| Code | Meaning |
|---|---|
| `CRIR_PARSE_ERROR` | Grammar or tokenization failure. Includes line number and a single-line excerpt of the offending input. |
| `CRIR_UNSUPPORTED_OPCODE` | Input contains a node opcode outside CRIR's supported set. The error names the offending opcode or struct path. |
| `CRIR_HIERARCHY_BAD_PARENT` | A hierarchy element's `parent=` does not resolve to an existing or previously added element. |
| `CRIR_HIERARCHY_BAD_TRANSFORM` | A hierarchy element has malformed `location=`, `rotation=`, or `scale=` tuple text. |
| `CRIR_CONTROL_BAD_TYPE` / `CRIR_CONTROL_BAD_VALUE` | A `control` element has an invalid `type=` token or typed-prefix `value=` literal. |
| `CRIR_CURVE_BAD_VALUE` | A `curve` element has malformed `value=` text; the value must parse as a float. |
| `CRIR_UNRESOLVED_PIN` | A `wire_in_X=%name.pin` or `wire_out_X=%name.pin` references a `%name` not declared earlier in the block, or a pin that does not exist on the resolved node. |
| `CRIR_ASSET_NOT_FOUND` | `context` does not resolve to a `UControlRigBlueprint`. |

All errors are surfaced through the standard `IrCompileDiagnostic` channel: structured for RPC callers, formatted with line/column context for human readers.

---

## See also

- [BPIR language reference](wiki-src/bpir.md) — sibling IR for Blueprint graphs; CRIR mirrors its sigil conventions and authored-position grammar.
- [IR authoring guide](ir-authoring.md) — shared IR contract, ephemerality invariant, and `IrCore` text helpers used by CRIR.
- [BPIR compiler internals](bpir-compiler-internals.md) — implementation patterns (two-phase compile, pin resolver, layout engine) that CRIR's compiler reuses where applicable.
- Board ticket `F-control-rig-ir-language` — the originating spec and Phase A implementation tracker.
