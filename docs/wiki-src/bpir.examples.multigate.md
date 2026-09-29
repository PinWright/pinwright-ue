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

MultiGate compiles to the engine's native `K2Node_MultiGate` node (it is not a StandardMacros graph). The gate cycles over every output pin, wired or not. `outputs: N` sets the output count; the node gets max(N, highest wired index + 1) outputs. The decompiler always emits `outputs:` first and names wired outputs as `Out0`, `Out1`, ...:

```
%mg = macro MultiGate(outputs: 4, Loop: true) [Out0 -> @out0]
```

> **Tests:** Parser: `ParseMacro` | Compiler: `MacroMultiGate`, `MacroMultiGateOutputCount` | Decompiler: `MacroMultiGate` | Round-trip: `MacroMultiGate`, `MacroMultiGateKeepsUnwiredOutputs`

_See also: call("bpir.examples") for the full index._
