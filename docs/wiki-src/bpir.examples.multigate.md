# bpir.examples.multigate

## MultiGate

Cycle through multiple output branches, optionally randomized or looping. See `call("bpir.instructions")` §2.4 for the `macro MultiGate(...)` invocation form alongside DoOnce, Gate, and FlipFlop.

```
entry custom_event OnCycle() {
    %mg = macro MultiGate(IsRandom: false, Loop: true) [0 -> @a, 1 -> @b, 2 -> @c]

@a:
    call PrintString(InString: "Phase A")

@b:
    call PrintString(InString: "Phase B")

@c:
    call PrintString(InString: "Phase C")
}
```

> **Tests:** Parser: `ParseMacro` | Compiler: `MacroMultiGate` | Decompiler: `MacroMultiGate` | Round-trip: `MacroMultiGate`

_See also: call("bpir.examples") for the full index._
