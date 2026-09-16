# bpir.examples.do-once-flipflop

## DoOnce & FlipFlop

Standard Blueprint macros accessed via `macro` keyword. Multiple entry blocks in one BPIR program. See `call("bpir.instructions")` §2.4 for the full macro-invocation reference (DoOnce, Gate, FlipFlop, MultiGate).

```
entry event Tick(float DeltaTime) {
    %once = macro DoOnce() [completed -> @init]

@init:
    call PrintString(InString: "Initialized (once only)")
}

entry custom_event OnToggle() {
    %flip = macro FlipFlop() [A -> @on, B -> @off]

@on:
    call SetVisibility(Target: $MeshComp, bNewVisibility: true)

@off:
    call SetVisibility(Target: $MeshComp, bNewVisibility: false)
}
```

> **Tests:** Parser: `ParseMacro` | Compiler: `MacroDoOnce`, `MacroFlipFlop` | Decompiler: _no dedicated test_ | Round-trip: `MacroDoOnce`, `MacroFlipFlop`

_See also: call("bpir.examples") for the full index._
