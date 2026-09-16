# bpir.errors

BPIR compiler diagnostics and common authoring pitfalls. See `call("bpir")` for the language overview.

## "Could not find target pin 'X' on node 'Y'" — includes pin list + best-guess

When BPIR call-site argument wiring fails because a named pin doesn't exist on the target node, the error message now includes:

1. The target node's actual input pin list (names the pins that *do* exist).
2. A "Did you mean 'Z'?" best-guess — computed by substring containment against the real pin names, with a length-delta tiebreak when multiple candidates match.

This applies to every node that flows through the generic wiring path in `WireDataPins` (`Private/Compiler/BpirCompiler.cpp`, `BuildMissingPinHint` helper), not just `UK2Node_CreateWidget`. Authors no longer need to decompile a reference Blueprint to discover the correct pin name — the error text itself tells you the available pins and the closest match to what you wrote.

Example: writing `call CreateWidget(Class: %cls, Owner: self)` when the correct pin is `OwningPlayer` produces an error listing `[Class, OwningPlayer, Outer]` and suggesting `"Did you mean 'OwningPlayer'?"`.

## Common Pitfalls

**Member functions need `Target:`** — Functions that belong to a specific class (not a library) require a `Target:` argument. If you get "Unresolved function", the function may exist on a component or actor class rather than a global library.

```
# WRONG — GetLocalPlayerGameTime is on UPlayerGameTimeComponent, not a library
%time = call GetLocalPlayerGameTime()

# RIGHT — pass the component as Target
%time = call GetLocalPlayerGameTime(Target: $GameTimeComponent)
```

**Pin names must match exactly** — Use the C++ parameter name, not the Blueprint display name.

```
# WRONG — the pin is named "Value", not "IntValue"
%txt = call Conv_IntToText(IntValue: 42)

# RIGHT
%txt = call Conv_IntToText(Value: 42)
```

**Format Text pins are self-wired** — The `Format` / `FormatText` instruction is handled by a dedicated K2Node handler, not the standard function resolution path. The handler creates the `UK2Node_FormatText` node and wires all pins itself (including dynamic `{placeholder}` argument pins). This is because `FindPin(const TCHAR*)` uses `FNAME_Find` internally, which returns `NAME_None` for dynamically created pin names that haven't been registered as FNames yet. The handler iterates `Node->Pins` directly instead. See `call("bpir.examples.format-text")` for the usage form.

**Cast results with `Target:`** — When calling a function on a cast result, pass the base `%ref` as Target. The compiler resolves the cast output type automatically from the primary output pin.

```
call DoSomething(Target: %cast)
```

**Chained property access (multi-dot)** — You can access properties on cast outputs or other object-typed pins using dot-chaining. The compiler auto-inserts VariableGet nodes for object properties and BreakStruct nodes for struct members at each level. Arbitrary depth is supported.

```
# Access a property on a cast result (auto-creates VariableGet)
%c = cast<MyClass>(%obj) [success -> @ok]
@ok:
call PrintString(InString: %c.AsMyClass.SomeProperty)

# Chain through struct members too
call DoSomething(Value: %c.AsMyClass.StructProp.X)
```

**Auto-BreakStruct for struct returns** — When a function returns a struct, you can access members directly with `%ref.Member` instead of writing an explicit `break<>` instruction. The compiler auto-inserts a BreakStruct node. See `call("bpir.examples.struct-make-break")` for the explicit form and `call("bpir.instructions")` §2.7 for the instruction reference.

```
# Explicit break (still works)
%v = call GetActorForwardVector(Target: self)
%b = break<Vector>(%v)
call PrintString(InString: %b.X)

# Implicit break (auto-inserted by compiler)
%v = call GetActorForwardVector(Target: self)
call PrintString(InString: %v.X)
```

## See also

- `call("bpir")` — language overview, sigils, quick reference
- `call("bpir.examples")` — worked examples with test annotations
- [`blueprint.insert_bpir_at_node`](blueprint.insert_bpir_at_node.md) — mid-flow graph insertion modes
- [Blueprint wiki](blueprint.md) — RPC entry points for compile/decompile/inspect
- [Blueprint graph wiki](blueprint.graph.md) — direct node creation, pin inspection, and graph analysis RPCs
- [Widget wiki](widget.md) — widget tree XML and UMG event binding RPCs
- `docs/bpir-compiler-internals.md` and `docs/bpir-test-matrix.md` in the plugin folder — parsing algorithm, design decisions, feature coverage and known gaps. Maintainer documents shipped beside the plugin, not wiki pages.
