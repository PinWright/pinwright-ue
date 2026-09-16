# bpir.instructions

BPIR instruction reference for calls, variables, control flow, macros, format text, timelines, structs, exec wiring, returns, dispatchers, literals, self, subsystems, and comments. See `call("bpir")` for the overview and `call("bpir.entry-points")` for entry declarations.

## 2.1 Function Calls

**Impure** (has exec pins, participates in exec chain):
```
call PrintString(InString: "Hello")              # void call
%hit = call LineTraceByChannel(Start: %s, End: %e)  # call with return
call SetVisibility(Target: $MyComponent, bNewVisibility: true)  # member call
```

**Pure** (no exec pins, data only — purity is auto-detected from UFunction flags):
```
%loc = call GetActorLocation(Target: self)
%sum = call Add_FloatFloat(A: $Health, B: 10.0)
%name = call GetObjectName(Object: $MyRef)
```

> **Float pins are doubles.** UE 5 BP "float" pins are 64-bit reals: prefer canonical
> `Add_DoubleDouble` over the 32-bit `Add_FloatFloat` (both compile). For an unknown
> pure UFunction, see `call("bpir.pure-impure")` for `search_api` and common utilities.

> **Deprecation:** `pure` remains an alias for `call`; purity is auto-detected from UFunction flags. Use `call`.

## Interface calls: `call` vs `message`

Both dispatch. Pick by what the target holds.

**`call` — the target is already an interface reference.** It emits `UK2Node_CallFunction` with an
interface-typed self pin, which dispatches to the implementer like any virtual call:

```
%c = cast<BPI_Damageable_C>(%owner) [success -> @have, fail -> @none]
@have:
    call ApplyDamage(Target: %c.AsBPIDamageable, Amount: 10.0)
```

**`message` — the target is a plain object that may or may not implement the interface.** It emits
`UK2Node_Message`, whose self pin is an ordinary object pin: the implements-check happens at
runtime and the call is skipped when it fails, so no cast and no failure branch are needed:

```
message BPI_Damageable_C::ApplyDamage(Target: %owner, Amount: 10.0)
```

The qualifier is the only thing that names the interface (the self pin is a plain object), so the
decompiler always emits it. Use the **generated-class** name for a Blueprint interface —
`BPI_Damageable_C`, `_C` included, because the stripped asset name does not resolve — and the plain
class name for a native one (`UserListEntry`). Unqualified `message Foo(...)` goes through the
normal resolution cascade and is refused with a compile error when the resolved function's owner is
not an interface, because such a node can never dispatch.

`Target:` is required, and survives decompile even when it is `self`: an unconnected message self
pin means "no receiver", not "self".

## Caller-Object Pin and the `Target:` Convention Collision

`Target:` is BPIR's conventional caller-object argument for member calls. The compiler maps `Target: %obj` to Unreal's `PN_Self` pin (`UK2Node_CallFunction`'s self input); it is an alias, not a keyword, and any argument resolving to that pin is treated as the caller.

The collision occurs when a UFUNCTION declares a parameter literally named `Target` — common in enum-dispatching APIs such as `Begin(EReplayManualPlacementTarget Target, ...)`. `Target:` then resolves to that parameter, not the caller pin.

Use lowercase `self:` as the escape hatch. Pin names are case-insensitive, so `self: %obj` routes to the self pin and leaves `Target:` available for the UFUNCTION parameter.

**Wrong** — `Target:` is consumed by the parameter; `Target_1:` does not exist and produces an error:
```
call Begin(Target: %widget, Target_1: $enumValue, InitialTransform: $xform)
# → Could not find target pin 'Target_1' on node 'Begin'.
#   Available pins: self, Target, InitialTransform
```

**Right** — `self:` takes the caller-object, `Target:` takes the enum parameter:
```
call Begin(self: %widget, Target: $enumValue, InitialTransform: $xform)
```

> **Note:** Standardize on lowercase `self:`. `Self:` also works case-insensitively, but `self:` is the intent-signaling form; only `Target:` → `PN_Self` is explicitly documented in compiler source.

**Latent** (exec pin + completion callback) — see `call("bpir.examples.delay")`:
```
%d = latent Delay(Duration: 2.0) [completed -> @after]
%move = latent MoveComponentTo(Component: $Root, TargetLocation: %pos) [completed -> @arrived]
```

## Optional register-binding type annotation

Every `%name = ...` binding line accepts an optional `: Type` suffix on the register, mirroring Rust/TypeScript style:

```
%n0: bool = call IsValid(Object: %target)
%n1: object<MaterialInstanceDynamic> = call GetDynamicMaterial(Target: $ProgressBar)
%n2: double = call Subtract_DoubleDouble(A: $Value, B: $MinValue)
%c: object<Pawn> = cast<Pawn>(%obj) [success -> @ok, fail -> @nope]
%n4: EWindowMode = call GetFullscreenMode(Target: %n1)
```

The annotation describes the primary output pin. Its grammar matches entry parameters and return types; the parser sends it to `BpirTypeSpecParser::ParseTypeSpec`, the same parser used by `-> ReturnType`.

In v1 the annotation is **descriptive only**: the parser records `FBpirInstruction::DeclaredResultType` / `bHasDeclaredResultType` but does not validate it against the resolved pin, so a wrong hand-written type still compiles. This preserves old `compile_bpir` callsites and round-trip output, while uniform decompiler output makes `%nK` readable without opening the callee asset.

Exec-only / delegate / hidden outputs produce no annotation: lines like `%n0 = sequence(2) [0 -> @s0, 1 -> @s1]` and `%n1 = branch(%cond) [...]` stay un-annotated because their primary output is an exec pin.

## 2.2 Variables

```
set Health = 100.0                    # Set member variable
set bIsAlive = true                   # Set bool
set PlayerName = "Hero"               # Set string
%v = get MyVariable                   # Explicit get (usually just use $MyVariable inline)
```

**External property access** (set/get on another object):
```
set %gi.RetryCount = 0               # Set property on a node output object
set $Param.Health = 100              # Set property on a function parameter
set MyVar = $Param.RetryCount        # Read property from a parameter (in value position)
```

**Multi-dot references** — `%ref` and `$name` targets support arbitrary dot chains through object properties (`VariableGet`) and struct members (`BreakStruct`):
```
set %castResult.AsBMyGameInstance.MenuIsJustOpened = false
set %ref.PinName.Property = value
set $MyStructVar.Inner.Outer.Leaf = 42
%h = get $PlayerState.Stats.Health
```

The parser splits `set` targets on the **last** dot: `TypeArg` gets the object path and `FunctionName` the leaf. For get-side `$name.a.b.c`, `PreEmitVariableRefs` splits on the first dot and `ResolveChainFromPin` walks the remainder as it does for `%ref.Pin.Prop.Q`; single-dot `set $widget.Property = value` is unchanged.

The target can be `$paramName` (parameter, self/Blueprint variable, or `$` fallback to a prior `%` local) or `%refName` (node output), with optional intermediate pins. The compiler resolves each link's class from its pin type and emits the external VariableGet/VariableSet nodes.

## 2.3 Control Flow

**Branch:**
```
%b = branch($bIsReady) [true -> @then, false -> @else]
```

**ForEach loop** — see `call("bpir.examples.foreach-loop")` for a worked pattern:
```
%loop = foreach($MyArray) [body -> @body, completed -> @after]
# Access: %loop.ArrayElement, %loop.ArrayIndex
```

The decompiler prints the engine pin name, ``%loop.`Array Element` ``. Both spellings compile — a backtick-quoted pin segment in a `%ref.pin` value is accepted anywhere, matching the backtick convention for spaced label and pin names in exec targets (§2.8).

**ForEach with break:**
```
%loop = foreach_break($MyArray) [body -> @body, completed -> @after]
```

**While loop:**
```
%w = while($bContinue) [body -> @body, completed -> @after]
```

**Switch (on enum/name — generic):**
```
%sw = switch($CurrentState) [Idle -> @idle, Active -> @active, default -> @def]
```

**Switch on Int:**
```
%sw = switch_int($Count) [0 -> @zero, 1 -> @one, 2 -> @two, default -> @other]
```
Case labels must be plain decimal integers forming a **contiguous ascending run** (`[0,1,2]`,
`[3,4,5]`, `[-1,0,1]`); order in the text does not matter, but gaps and duplicates are rejected at
compile time. `UK2Node_SwitchInteger` stores no case values — the pin name *is* the case value, and
the engine renumbers the case pins to `StartIndex, StartIndex+1, ...` on every reconstruction — so a
gapped set has no representation that survives a reload. To dispatch on gapped values, use `branch`
or `switch_string`.

**Switch on String:**
```
%sw = switch_string($Command) ["attack" -> @atk, "defend" -> @def, default -> @unknown]
```

**Switch on Enum (typed):**
```
%sw = switch_enum<EWeaponType>($WeaponType) [Sword -> @sword, Bow -> @bow, default -> @other]
```

**Sequence:**
```
%seq = sequence(3) [0 -> @s0, 1 -> @s1, 2 -> @s2]
```

**Cast (statement form, impure)** — see `call("bpir.examples.cast-with-failure")` for a worked pattern:
```
%cast = cast<MyCharacter>(%pawn) [success -> @ok, fail -> @nope]
# Access: %cast.AsMyCharacter
```

**Cast (RHS-expression form, pure):** `cast<T>(...)` is accepted on the right of `set` (`set $TypedRef = cast<Actor>($Source)`). It compiles to a pure `UK2Node_DynamicCast` (`SetPurity(true)`): no exec branches, labels, or bool-success pin; failure returns null. Use it when the receiver can handle null, and the statement form when success must branch. Inner `$` / `%` references pre-emit normally.

```
set $TargetActor = cast<Actor>($SourceComponent)
set %typed = cast<MyCharacter>($Pawn)
```

Implementation: `BpirValueResolver::ResolveValue` routes `cast<...` through `ParseCastSyntax`, shared with `BpirCompiler::PreEmitVariableRefs`, then builds `UK2Node_DynamicCast` with `SetPurity(true)` (UE precedent: `EdGraphSchema_K2.cpp:2851`, `K2Node_DynamicCast::SetPurity`/`IsNodePure`).

Compiler gotcha: the cast source pin must be wired through the K2 schema (`TryCreateConnection`), not raw `MakeLinkTo`. A raw link preserves visible topology but skips `UK2Node_DynamicCast` pin notification, leaving the `Object` pin as a wildcard and causing the full Blueprint compile to fail with an undetermined Object pin.

**Select (pure ternary):**
```
%val = select(cond: $bIsDay, true: "Day", false: "Night")
```

> **Pin aliases:** The compiler accepts `cond`, `condition`, and `Index` for the condition pin of `select`, `branch`, and `switch` nodes. The decompiler always outputs `Index:`.

## 2.4 Macros (DoOnce, Gate, FlipFlop, MultiGate)

See `call("bpir.examples.do-once-flipflop")` and `call("bpir.examples.multigate")` for worked patterns.

```
%once = macro DoOnce() [completed -> @go]
%flip = macro FlipFlop() [A -> @pathA, B -> @pathB]
%gate = macro Gate(Open: true) [exit -> @through]
%mg = macro MultiGate(IsRandom: true) [0 -> @out0, 1 -> @out1, 2 -> @out2]
```

## 2.5 Format Text

Creates a `UK2Node_FormatText` pure node with dynamic argument pins from `{placeholder}` patterns. See `call("bpir.examples.format-text")` for a worked pattern.

```
%txt = call Format(Format: "{Name} has {HP} HP", Name: $PlayerName, HP: $Health)
```

**Aliases:** `Format`, `Format_Text`, `FormatText` (case-insensitive).

The `Format:` argument is required and must contain the format string. Each `{placeholder}` in the format string creates a wildcard input pin named after the placeholder. The output is an FText on the `Result` pin.

## 2.6 Timeline

See `call("bpir.examples.timeline")` for a worked pattern with track-output access.

```
%tl = timeline MyFadeTimeline(
    Alpha: float_curve((0.0, 0.0), (1.0, 1.0))
) [update -> @update, finished -> @finished]
```

## 2.7 Struct Operations

See `call("bpir.examples.struct-make-break")` for a worked pattern.

```
%broken = break<HitResult>(%hit.OutHit)        # Break struct
%vec = make<Vector>(X: 1.0, Y: 2.0, Z: 3.0)   # Make struct
# Access break results: %broken.Location, %broken.Normal, etc.
```

A **split** struct pin is the same operation authored inline on the node, and its decompile
form depends on direction. Split **inputs** become generated `make<Struct>(...)` values ahead
of the consumer, which receives the parent pin name. Split **outputs** use dotted member
references — `$Hit.HitBoneName`, `%n0.OutHit.BoneName`, `%n0.BoneName` when the split pin is
the node's `ReturnValue` — and explicit `break<T>` remains available for a break node. Each
segment is the member's real pin name — see `call("bpir")` for why the display name is not
usable here.

## 2.8 Labels & Exec Wiring

```
@then:                              # Define label
    call PrintString(InString: "Yes")
    exec -> @done                   # Explicit jump to label

@else:
    call PrintString(InString: "No")
    # Falls through to @done (next label, auto-chain)

@done:
    call PrintString(InString: "After branch")
```

**Rule:** Impure nodes auto-chain within a label block; use `exec -> @label` only for a non-adjacent jump. An adjacent label is a fall-through target only when the preceding segment has no terminator.

Use a bare `end` when the current exec chain has no successor. It creates no Blueprint node and accepts no arguments; it only prevents the preceding exec output from auto-chaining into a later instruction or adjacent label. This keeps sibling terminal blocks distinct:

```
%b = branch($bCondition) [true -> @yes, false -> @no]

@yes:
    call PrintString(InString: "Yes")
    end

@no:
    call PrintString(InString: "No")
    end
```

`end` is not `return`: `return` emits a real function-result node or targets a macro exit tunnel, while `end` only records a disconnected exec output.

It is also a hard boundary when the compiler resolves a label's first executable target or searches backward for the source of a later `exec ->`; neither lookup crosses `end`.

**Targeting a non-first input exec pin.** When the target node has ≥2 input exec pins (Gate, MultiGate, DoOnce, user macros with multiple entry tunnels), append `.PinName` to the label:

```
%g = call /Script/Engine.K2Node_Gate() [Open -> @gate.Open, Reset -> @gate.Reset]
```

Spaced label or pin names are wrapped in backticks, matching the existing convention for spaced macro and function names:

```
[Out -> @`my gate`.`Reset Pin`]
```

Lookup is case-insensitive; an omitted suffix wires to the first exec input. A missing named pin hard-fails.

This applies to ExecTargets entries (the `[output -> @label, ...]` clause). The `return [ExitPin]` form keeps its existing bracket syntax — that is a separate mechanism and unchanged.

## 2.9 Return

```
return %result                          # Return the conventional ReturnValue pin
return                                  # Void return (function or impure macro)
return (Health01: %value)               # Return one explicitly named function output
return (Result: %val, Name: %other)     # Named multi-output return (macro/multi-output function)
return [ExitPin]                        # Multi-exit macro: wire to named exec exit, no data
return [ExitPin] (Result: %val)         # Multi-exit macro: wire to named exec exit + data
```

In a function, `return` wires to `UK2Node_FunctionResult`; in a macro, to the exit tunnel. Bare `return value` targets the conventional `ReturnValue` pin. A named single result pairs a parenthesized entry output such as `-> (float Health01)` with `return (Health01: value)`. `[ExitPin]` selects a named exec input for multi-exit macros, and `(Name: value, ...)` wires data inputs on the exit tunnel or function result.

**A function may carry as many `return`s as it has exit paths.** Each `return` statement gets its own `UK2Node_FunctionResult` node with that statement's values wired into it — several result nodes per function graph are legal and the Blueprint compiler merges them, which is what a human authors when every branch ends in its own Return node. Distinct data per branch therefore survives:

```
entry function GetAimRay() -> (vector Origin, vector Direction) {
    %ok = call IsValid(Object: $Owner)
    %b = branch(%ok) [true -> @eyes, false -> @muzzle]

@eyes:
    %vp = call GetActorEyesViewPoint(Target: $Owner)
    %ef = pure Conv_RotatorToVector(InRot: %vp.OutRotation)
    return (Origin: %vp.OutLocation, Direction: %ef)

@muzzle:
    %ml = call K2_GetComponentLocation(Target: $Muzzle)
    %mr = call K2_GetComponentRotation(Target: $Muzzle)
    %mf = pure Conv_RotatorToVector(InRot: %mr)
    return (Origin: %ml, Direction: %mf)
}
```

**Macros are the exception:** a macro graph has exactly one exit tunnel whose data pins are shared by every exit, so two macro `return`s writing the same output pin overwrite each other. Split the exits (`return [ExitA]` / `return [ExitB]`) or compute the value once before returning.

## 2.10 Event Dispatchers

See `call("bpir.examples.event-dispatcher")` and `call("bpir.examples.clear-dispatcher")` for worked patterns.

```
call_dispatcher OnHealthChanged(NewHealth: $Health)
bind_dispatcher OnHealthChanged(target: self, event: @HandleHealthChanged)
unbind_dispatcher OnHealthChanged(target: self, event: @HandleHealthChanged)
clear_dispatcher OnHealthChanged(target: self)
```

**External targets:** `bind_dispatcher` and `unbind_dispatcher` accept `Target:` for dispatchers owned by another object. The compiler scans `Inst.Args`, resolves the class with `ResolveTargetClass`, and searches that class instead of self:

```
bind_dispatcher OnSomethingChanged(Target: %externalRef, event: @HandleIt)
```

## Field Notifications

`field_notify_subscribe` and `field_notify_unsubscribe` are the UE 5.4+ FieldNotification equivalent of `bind_dispatcher`. They emit `UK2Node_CallFunction` for `K2_AddFieldValueChangedDelegate` / `K2_RemoveFieldValueChangedDelegate` (`UFUNCTION(BlueprintCallable)` on `UWidget`), synthesize a local `UK2Node_CreateDelegate`, and set `FieldId` to `(FieldName="X")`.

```
field_notify_subscribe FieldName(event: @Handler)
field_notify_unsubscribe FieldName(event: @Handler)
```

- `FieldName` is the observed field (e.g. `Replay`), mapped to `FieldId` as `(FieldName="Replay")`.
- `event:` names a local custom event or function with signature `(Object: object, Field: FieldNotificationId)`.
- `target:` defaults to `self`; external targets may implement `INotifyFieldValueChanged` (including `UUserWidget`).

```
entry event Construct() { field_notify_subscribe Replay(event: @OnReplayChanged) }
entry event Destruct()  { field_notify_unsubscribe Replay(event: @OnReplayChanged) }
entry custom_event OnReplayChanged(Object: object, Field: FieldNotificationId) {}
```

## 2.11 Enum & Array/Set/Map Literals

See `call("bpir.examples.array-ops")` for a worked array pattern.

```
%e = enum ECollisionChannel::Visibility
%arr = make_array("A", "B", "C")
```

> **Strongly-typed enums (PC_Enum) vs legacy byte enums (PC_Byte):** UE 5 enums declared as `UENUM() enum class EFoo : uint8 { ... }` use `PC_Enum` on their pins, while legacy `UENUM() enum EFoo : uint8 { ... }` pins use `PC_Byte`. Both carry the `UEnum*` in `PinSubCategoryObject`. The BPIR decompiler must recognize **both** categories when emitting `enum EEnumName::ValueName` literals — checking only `PC_Byte` causes `PC_Enum` pins to fall through to raw integer emission (e.g., `B: 0` instead of `B: EWeaponState::Disarmed`). The fix is a guard like `if (PinType.PinCategory == PC_Byte || PinType.PinCategory == PC_Enum)` before looking up the `UEnum` subcategory object.
>
> **Compiler-side default value format:** `PC_Byte` pins with an enum subtype require a **numeric string** default (e.g., `"2"`), because that is what UE's pin serialization stores for byte-category pins. `PC_Enum` pins (strongly-typed enum class) require the **enum name string** (e.g., `"Collapsed"`). The BPIR compiler's `CodePinResolver::SetPinDefaultValue` branches on `PinCategory` to route correctly: for `PC_Byte` it calls `TryResolveEnumLiteralToValue` and stores the integer, for `PC_Enum` it calls `TryApplyEnumPinDefaultValue` which stores the name string. This distinction matters for operators like `EqualEqual_ByteByte` (PC_Byte) where passing an enum name string causes the BP schema to reject the default value.
>
> **PC_Byte with vs without UEnum subtype:** Not all `PC_Byte` pins carry an enum. `CodePinResolver::SetPinDefaultValue` distinguishes three cases: (1) `PC_Byte` with a `UEnum` in `PinSubCategoryObject` (e.g., `SetVisibility`'s `InVisibility` pin) — resolved as enum name strings via `TryApplyEnumPinDefaultValue`, (2) `PC_Byte` **without** a `UEnum` subtype (e.g., `EqualEqual_ByteByte`'s `B` pin) — stored as numeric strings, (3) `PC_Enum` — resolved as enum name strings. The check uses `Cast<UEnum>(Pin->PinType.PinSubCategoryObject.Get())` to distinguish cases 1 and 2. Without this check, bare byte pins would incorrectly attempt enum name resolution and fail.

**Decompiler-only** (emitted by the decompiler but not yet compilable):
```
%s = make_set("A", "B", "C")
%m = make_map("key1": "val1", "key2": "val2")
```

## `Array_Get` and accessor aliases

`call Array_Get(...)` (the BP `UK2Node_CallArrayFunction` / `UK2Node_GetArrayItem` node) returns one indexed element. Output names vary by UE version and node subclass, so BPIR accepts these case-insensitive aliases for the **sole non-exec output pin** when a literal name is absent:

| Alias | Resolves to |
|---|---|
| `.Item` | Sole non-exec output pin |
| `.Result` | Sole non-exec output pin |
| `.Value` | Sole non-exec output pin |

```
%el = call Array_Get(TargetArray: $MyArray, Index: 0)
%as = cast<MyActor>(%el.Item) [success -> @ok, fail -> @bad]
```

Aliases apply only when the literal pin name is missing; explicit names such as `%el.MyActor` still work. `foreach`'s `%loop.ArrayElement` is unaffected: `ForEachLoop` owns that stable pin and its notification.

**End-to-end wiring:** `call Array_Get(...)` followed by `cast<T>(%h.Item)` now compiles cleanly. Previously wildcard propagation lagged behind downstream wiring, so `TryCreateConnection` failed with `wiring data 'Item' -> 'Object'`. The compiler now calls `NotifyPinConnectionListChanged` after wiring `TargetArray`, resolving the element type before later instructions use the output.

## 2.12 Self Reference

```
%me = self
call DestroyActor(Target: self)
```

## 2.13 Subsystem Access

```
%ss = subsystem<AppMusicSubsystem>()
call NextTrack(Target: %ss)
%name = call GetCurrentTrackName(Target: %ss)
```

Auto-detects `UK2Node_GetSubsystem`, `UK2Node_GetEngineSubsystem`, `UK2Node_GetEditorSubsystem`, or `UK2Node_GetSubsystemFromPC` from the subsystem hierarchy. A `Target: %ref` typed node also supplies its class for function resolution.

## 2.14 Comments

```
# This is a comment — can attach to Comment nodes in BP
```

## See also

- `call("bpir.pure-impure")` for instruction purity, exec-pin, and auto-chaining classifications.
