# bpir.examples.macro-pure

## Macro Definition (Pure)

Define a reusable macro graph with data inputs and outputs. Pure macros contain only pure instructions -- no exec pins on the tunnel pair. See `call("bpir.entry-points")` §1c for macro-definition rules and `call("bpir.pure-impure")` §6 for the purity table.

```
entry macro ClampValue(float Value, float Min, float Max) -> (float Result) {
    %clamped = call FClamp(Value: $Value, Min: $Min, Max: $Max)
    return (Result: %clamped)
}
```

> **Tests:** Parser: `MacroEntry` | Compiler: (via SetupMacro) | Decompiler: (via decompile_macro) | Round-trip: `MacroSimple`

_See also: call("bpir.examples") for the full index._
