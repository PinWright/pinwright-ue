# bpir

A line-based IR for Blueprint graph logic.

## Why BPIR?

Blueprint graphs are **labeled directed graphs** — nodes connected by named exec/data pins. The C++ pseudocode forces a tree structure (nested braces) onto this, causing:

| Problem | C++ Pseudocode | BPIR |
|---------|---------------|------|
| Reconvergence | Implicit — compiler must infer where branches rejoin | Explicit — `exec -> @label` |
| Brace matching | 3 separate implementations, K&R vs Allman bugs | No braces — flat labeled blocks |
| Multi-output nodes | Mashed into `if`/`for`/`switch` keywords | Each output pin named in `[pin -> @label]` |
| Pure vs impure | Indistinguishable | Auto-detected from UFunction flags; `call` for all |
| Data wiring | Hidden nodes in `A->Foo(B->Bar())` | Every node named: `%name` references |
| Round-trip fidelity | Decompile loses node identity | Every instruction preserves identity |

## Core Principles

1. **SSA-like named values** — every node result gets a name (`%name`), referenced later
2. **Labeled blocks** — control flow via explicit `@label:` + `[pin -> @label]`
3. **Primary node mapping** — node-backed instructions own one visible primary node; helpers and zero-node instructions are explicit exceptions
4. **Line-based** — one instruction per line, keyword-first
5. **No braces for control flow** — blocks end at next label or `}`
6. **Exec auto-chaining** — sequential impure nodes auto-wire within a block

## Quick Reference

```
entry event BeginPlay() {
    call PrintString(InString: "Hello")
    %valid = call IsValid(Object: $MyRef)
    %b = branch(%valid) [true -> @ok, false -> @end]

@ok:
    set Score = 100
    call DoSomething(Target: $MyRef)

@end:
    call PrintString(InString: "Done")
}
```

Non-default Blueprint node enabled states are preserved with the bare
`disabled` and `devonly` suffixes on entry signatures and
instructions; the full grammar and position-marker ordering are documented in
`bpir.entry-points`.

Use `parent_call Class::Function(...)` when the graph contains a
`UK2Node_CallParentFunction`; ordinary `call` does not preserve that node
identity on a decompile/recompile.

## Sigil Summary

| Sigil | Meaning | Example |
|-------|---------|---------|
| `%name` | Node output (local value) | `%loc = call GetActorLocation()` |
| `%name.Pin` | Specific output pin | `%loop.ArrayElement` |
| `$name` | Blueprint variable or entry parameter (get) | `$Health`, `$Amount` |
| `@name` | Label (exec target) | `@then:`, `exec -> @done` |
| `self` | Self-reference | `call Jump(Target: self)` |

Struct members are addressed by dotted access on whatever value carries the struct —
`%n0.OutHit.BoneName`, `$Hit.HitBoneName`, `$Payload.Transform.Location.X`. A struct pin
that is **split** in the graph (the editor's inline struct break) uses a direction-specific
decompile form. A split **input** is rebuilt as a generated `make<Struct>(...)` value ahead of
the consumer, which receives the original parent pin name. A split **output** is addressed by
the same dotted member form and recompiles through an explicit break node when needed. The raw
`<ParentPin>_<Member>` input sub-pin name is not emitted as a consumer argument.

**A member segment is the pin's real name, never its display name**, and the two are not
interchangeable. `Break Hit Result` exposes `HitBoneName` (the bone that was hit,
`FHitResult::BoneName`) alongside `BoneName` (the *tracing* component's bone,
`FHitResult::MyBoneName`, `None` for a line trace); their display names — "Hit Bone Name"
and "Bone Name" — read as near-synonyms. Printing the pin name is what keeps the two reads
textually distinct and lets the compiler resolve each back to the member it names.

`$name` resolution is intentionally broader than "Blueprint member variable":
it first covers member variables and entry parameters, and can then fall back
to a prior `%name` local in the same block. This keeps decompiled or
LLM-authored aliases such as `%alias = $v` valid when `v` is a local binding,
not a class member. Compiler pre-emit must not create a standalone
`UK2Node_VariableGet` for non-member names; resolution goes through
`FCodePinResolver`, existing cached gets, and then `Block.ValueIndex`.
Negative Blueprint-variable probes should be cached per resolver graph so
local-heavy blocks do not repeatedly scan the Blueprint variable table.

## See also

- `call("bpir.entry-points")` — §1 Entry Points, §1b Authored Positions, §1c Macro Definitions
- `call("bpir.instructions")` — §2 Instructions (function calls, variables, control flow, macros, format text, timeline, structs, exec wiring, return, dispatchers, enum/array/set/map, self, subsystem, comments)
- `call("bpir.types")` — §3 Type System (literal formats, FText, optional empty pins, type-string resolution)
- `call("bpir.pure-impure")` — §6 Pure vs Impure table
- `call("bpir.errors")` — diagnostics and common pitfalls
- `call("bpir.examples")` — worked examples with test annotations
