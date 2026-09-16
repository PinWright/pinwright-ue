# bpir.entry-points

Entry-point declarations, authored node positions, and macro definitions for BPIR. See `call("bpir")` for the language overview.

## 1. Entry Points

Every code block starts with an `entry` declaration:

```
entry event BeginPlay() {
entry event Tick(float DeltaTime) {
entry construction ConstructionScript() {
entry custom_event OnDamageReceived(float Damage, object<AActor> Instigator) {
entry function CalculateDamage(float Base, float Mult) -> float {
entry function GetHealthAndName(object<AActor> Target) -> (float Health, string Name) {
entry macro ClampValue(float Value, float Min, float Max) -> (float Result) {
entry macro `Update Targets`(string TargetId) {
entry macro CheckValid(object<Object> Target) -> [IsValid -> @valid, IsNotValid -> @invalid] {
entry macro Validate(float X) -> (bool Result) [Pass -> @pass, Fail -> @fail] {
entry component_event BoxCollision.OnComponentBeginOverlap(object<AActor> OtherActor) {
entry widget_event StartButton.OnClicked() {
entry key_pressed SpaceBar() {
entry key_released SpaceBar() {
entry input_action /Game/Input/IA_Move.IA_Move() {
entry input_action /Game/Input/IA_Move.IA_Move() [Triggered -> @triggered, Completed -> @completed] {
```

For UMG event binding workflow, create or inspect named widgets with [`widget.import_xml`](widget.import_xml.md), [`widget.export_xml`](widget.export_xml.md), or [`widget.describe`](widget.describe.md), compile `entry widget_event ...`, then verify with [`widget.bind_event`](widget.bind_event.md).

**Custom event parameter references:** Parameters declared on a `custom_event` entry can be referenced in the body using either `$ParamName` (sigil form) or bare `ParamName` (no sigil). The decompiler always emits the `$ParamName` form. The bare name works because the value resolver falls back to the `PinResolver` for registered parameter pins when no `$`-prefixed variable matches. This applies to all entry types with parameters (events, functions, macros), not just custom events.

**Reference / const param modifiers:** Entry-signature parameters (`custom_event`, `function`, `macro`) accept a C++-style `const ` prefix and a `&` suffix on the type. The compiler strips the modifiers before type resolution and sets the corresponding flags on the emitted pin:

```
entry custom_event OnHit(const struct<FHitResult>& Hit) {
entry custom_event OnStatsUpdated(const struct<FPlayerStats>& Stats, bool bFull) {
entry function Mutate(struct<FVector>& InOutVec) -> bool {
entry function Apply(const object<AActor> Owner) {
```

Accepted forms per parameter: `const T& Name`, `T& Name`, and `const T Name`. Modifiers are supported on primitives, `struct<T>`, and `object<T>` alike. The parser is case-insensitive on `const`. `BpirTypeSpecParser::ParseTypeSpec` strips the tokens before building the `FBpirTypeSpec`, and `FCodePinResolver::ConvertTypeSpecToPinType` sets `FEdGraphPinType::bIsConst` + `bIsReference` from the spec, which the BP compiler maps to `CPF_ConstParm | CPF_ReferenceParm` on the emitted `UFunction` parameter.

**When these modifiers are required:** Any custom event whose signature must match a `DYNAMIC_MULTICAST_DELEGATE_*Param(..., const FStruct&, Name)` dispatcher. Without `const`+`&` on the `FStruct` param, `IsSignatureCompatibleWithScriptDelegate` rejects the binding at BP compile time — even though BPIR compilation itself succeeds — and the event becomes unbindable. The decompiler emits the modifiers whenever the source pin carries them, so round-trip fidelity is preserved.

**Cross-entry calls and forward references:** Any entry block can call a `custom_event`, `function`, or `macro` declared in another block, including one declared later, using `call CustomEventName(args)`. Phase 1 creates all entry points, Phase 1.5 regenerates the Blueprint skeleton so they are resolvable as UFunctions on `SkeletonGeneratedClass`, and Phase 2 emits the bodies; a forward `call MyFunc()` on line 2 can therefore target a function declared on line 50. `docs/bpir-compiler-internals.md` in the plugin folder covers this architecture in detail; it is a maintainer document, not a wiki page.

**Backtick-quoted names:** Any identifier containing spaces must be wrapped in backticks. This applies to both entry declarations and call sites:

- Entry declarations: `` entry macro `My Macro Name`(...) ``, `` entry function `Set Error`() ``
- Call sites: `` call `Set Error`(Target: Self, Error: "x") ``

The backticks are stripped during parsing; the underlying UFunction or graph is accessed by its unquoted name. The decompiler automatically emits backticks whenever a UFunction's display name contains a space.

**Space-stripped resolver fallback:** When resolving a function name, if an exact match fails, the resolver also tries a case-insensitive match after stripping all spaces from both the query and candidate names. This means `SetError` and `Set_Error` are accepted as aliases for `` `Set Error` `` when no ambiguity exists. The resolver errors cleanly if the space-stripped form matches more than one function.

**Named outputs:** Both `function` and `macro` entries support named outputs via `-> (type Name, ...)`, including a one-item list such as `-> (float Health01)`. Bare `-> type` is the compact function shorthand for the conventional `ReturnValue` pin. The decompiler uses the parenthesized form whenever a sole output has another name, so that name survives recompilation.

**Implemented interface graphs:** Aggregate `blueprint.decompile`, named `blueprint.decompile {graphName}`, and `blueprint.decompile_function` include graphs owned by `ImplementedInterfaces`. They are emitted as `entry override`, not as new local `entry function` declarations; recompiling that text reuses the interface-owned graph and does not create an ordinary duplicate function graph.

**Enhanced Input action entries:** `entry input_action` requires the full object path, including the object suffix: `/Game/Input/IA_Move.IA_Move`. The simple form starts its body from the `Triggered` output. Use an explicit entry exec map when more event outputs have bodies; supported names are `Triggered`, `Started`, `Ongoing`, `Canceled`, and `Completed`:

```bpir
entry input_action /Game/Input/IA_Move.IA_Move() [Triggered -> @triggered, Completed -> @completed] {
@triggered:
    call PrintString(InString: "moving")

@completed:
    call PrintString(InString: "stopped")
}
```

With an explicit event map, every executable statement must appear under a label. Comments before the first label remain allowed, but an executable statement there is rejected before graph mutation. The diagnostic identifies the statement's source line and says `put statements under a label`, instead of creating a disconnected node.

The action asset is bound before the node allocates pins, so data outputs match the asset's value type. For example, an Axis2D action exposes `$ActionValue` as `FVector2D`; `$ElapsedSeconds`, `$TriggeredSeconds`, and `$InputAction` are also available when the engine node exposes them. Decompilation preserves the exact action object path and emits the exec map whenever more than the single `Triggered` chain is connected.

## `compile_bpir` mode parameter

`blueprint.compile_bpir` accepts an optional `mode` string:

| Value | Behaviour |
|-------|-----------|
| `"append"` (default) | **Upsert** — existing entry nodes with matching signatures are deleted before the new ones are created. Retrying a timed-out compile is safe; no duplicate nodes accumulate. |
| `"replace"` | Alias for `"append"` — identical semantics, preserved for backward compatibility with callers that set it explicitly. |
| `"extend"` | **Append to existing body** — Phase 0 deletion is skipped. For `entry override`, the new body's first instruction is wired to the terminal exec-output pin of the existing override's exec chain. Falls back to `"append"` behaviour when no existing override is found (creates fresh). Errors with `COMPILE_FAILED` if the existing exec chain has forked control flow (branch/sequence nodes). **Scope note:** currently applies to `entry override` only; other entry kinds (`event`, `custom_event`, `function`, `macro`) receive the same Phase 0 skip but do not perform the terminal-splice — they create a fresh entry as if in `"append"` mode. |

Both `"append"` and `"replace"` modes were harmonised in the idempotency fix (Apr 2026). Before that fix, `"append"` did **not** pre-clean existing entries, so retrying a timed-out RPC created duplicates.

## 1b. Authored Node Positions

An entry signature or any visible node-backed BPIR instruction can end with an authored graph coordinate:

```
entry event BeginPlay() {
    call PrintString(InString: "Hello") @(240, 0)
    %valid = call IsValid(Object: self) @(520, 0)
    %branch = branch(%valid) [true -> @ok, false -> @done] @(800, 0)

@ok:
    call PrintString(InString: "Valid") @(1080, -96)

@done:
    call PrintString(InString: "Done") @(1080, 96)
}
```

`@(x, y)` sets absolute `NodePosX` / `NodePosY` coordinates. On an entry signature it positions the entry node itself; on a body instruction it positions that instruction's primary emitted node. The marker is quote-aware, so strings such as `"literal @("` are treated as string contents, not position syntax. BPIR labels such as `@ok` keep their normal control-flow meaning.

Entry positions appear after the complete signature, including return declarations:

```bpir
entry custom_event PositionedEntry() @(16, 1902) {
    call PrintString(InString: "Body") @(316, 1902)
}

entry function Measure() -> float @(1, 2) {
    return 1.0 @(300, 2)
}
```

Entry positions are independent from body placement mode. A positioned entry does not require every body instruction to be positioned, and a manually positioned body does not require the entry signature to carry a coordinate.

Layout mode is selected independently for each entry body:

| Body shape | Result |
|------------|--------|
| No primary-node instructions have `@(x, y)` | The post-compile layout engine formats the whole created body. Implicit helper/generated nodes are allowed. |
| Every primary-node instruction has `@(x, y)` | Authored coordinates are preserved for the primary nodes. Implicit visible helper/generated nodes are rejected. |
| Some, but not all, primary-node instructions have `@(x, y)` | Compile error. Add `@(x, y)` to every visible node-backed instruction in that entry body, or remove all positions from the body to use auto-layout. |

Mixed bodies fail:

```
entry event BeginPlay() {
    call PrintString(InString: "Pinned") @(240, 0)
    call PrintString(InString: "Missing position")
}
# Compile error: manual-placement mode requires every visible node-backed instruction in this entry body to have @(x, y).
# Fix: add @(x, y) to the second call, or remove all positions from this body to use auto-layout.
```

Authored-position bodies also reject implicit visible helpers because those helpers have no BPIR line to carry a coordinate:

```
entry function UsesStruct(struct<FHitResult> Hit) -> float {
    %x = call Conv_DoubleToFloat(InDouble: $Hit.Location.X) @(300, 0)
    return %x @(620, 0)
}
# Compile error: authored-position mode would create an implicit visible helper node.
# Fix: remove all positions for auto-layout, or make the helper explicit with its own positioned BPIR line.
```

`@(x, y)` on a zero-primary-node instruction is also invalid:

```
entry event BeginPlay() {
    exec -> @done @(240, 0)

@done:
    call PrintString(InString: "Done") @(520, 0)
}
# Compile error: @(x, y) only applies to visible node-backed BPIR instructions.
# Fix: remove the position marker from the zero-node instruction, or attach positions only to instructions that emit visible primary nodes.
```

Comment-only lines and transparent reroutes/knots do not carry BPIR position suffixes in v1. Decompilation emits current graph coordinates for node-backed BPIR lines, so manually moved nodes round-trip back into editable `@(x, y)` suffixes.

## 1b.1 Node Enabled State

BPIR preserves the Blueprint node enabled state when it is not the default. A
`disabled` marker means the node is compiled out. A `devonly` marker
means the node is enabled only in development builds, matching UE's
`ENodeEnabledState::DevelopmentOnly` state. The marker applies to an entry
signature or to the instruction's primary emitted node:

```bpir
entry event Tick() disabled @(0, 0) {
    call PrintString(InString: "development") disabled @(320, 0)
}

entry event BeginPlay() {
    call PrintString(InString: "editor only") devonly @(320, 160)
}
```

Calls to a Blueprint's direct parent preserve their node class with the
`parent_call Class::Function(...)` keyword. The class qualifier is required in
decompiler output so recompilation targets the same parent implementation; the
entry or instruction marker can be appended as usual:

```bpir
entry event BeginPlay() {
    parent_call Actor::ReceiveBeginPlay() disabled
}
```

The marker is a bare suffix and is written immediately before the optional
`@(x, y)` position marker. The parser also accepts it after the position marker
for hand-authored input. `enabled` is accepted as an explicit reset to the
default state, but the decompiler omits it. Entries and instructions without a
marker compile as enabled, preserving the historic BPIR form.

## 1c. Macro Definitions

Macro graphs are defined with `entry macro`. Unlike functions, macros compile to `UK2Node_Tunnel` entry/exit pairs (not `UK2Node_FunctionEntry` / `UK2Node_FunctionResult`). Macros can be pure (no exec pins) or impure, can have multiple data outputs, and can have multiple named exec exit paths.

## Pure macro (data outputs only)

```
entry macro ClampValue(float Value, float Min, float Max) -> (float Result) {
    %clamped = call FClamp(Value: $Value, Min: $Min, Max: $Max)
    return (Result: %clamped)
}
```

## Impure macro (exec-only, no data outputs)

```
entry macro LogAndContinue(string Message) {
    call PrintString(InString: $Message)
}
```

## Multi-exit macro (named exec exit paths)

Use `[ExitName -> @label, ...]` in the entry declaration to define multiple exec output pins on the exit tunnel:

```
entry macro CheckValid(object<Object> Target) -> [IsValid -> @valid, IsNotValid -> @invalid] {
    %ok = call IsValid(Object: $Target)
    %b = branch(%ok) [true -> @yes, false -> @no]

@yes:
    return [IsValid]

@no:
    return [IsNotValid]
}
```

## Combined data + exec outputs

A macro can have both named data outputs and named exec exit paths:

```
entry macro Validate(float X) -> (bool Result) [Pass -> @pass, Fail -> @fail] {
    %gt = call Greater_FloatFloat(A: $X, B: 0.0)
    %b = branch(%gt) [true -> @ok, false -> @bad]

@ok:
    return [Pass] (Result: true)

@bad:
    return [Fail] (Result: false)
}
```

## Return variants in macros

| Syntax | Meaning |
|--------|---------|
| `return (Name: value, ...)` | Wire named data outputs on exit tunnel |
| `return [ExitPin]` | Wire to a specific named exec exit pin (no data) |
| `return [ExitPin] (Name: value, ...)` | Wire to a specific exec exit + named data outputs |
| `return` | Wire to default exec output (impure macros) |
