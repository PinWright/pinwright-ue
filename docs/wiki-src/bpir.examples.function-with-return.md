# bpir.examples.function-with-return

## Function with Return Value

Define a Blueprint function with input parameters and a return value. See `call("bpir.entry-points")` §1 for the `entry function` declaration (including multi-output `-> (type Name, ...)`) and `call("bpir.instructions")` §2.9 for the `return` variants.

```
entry function GetDamageMultiplier(float BaseDamage, bool bIsCritical) -> float {
    %crit: float = select(cond: $bIsCritical, true: 2.0, false: 1.0)
    %result: float = call Multiply_FloatFloat(A: $BaseDamage, B: %crit)
    return %result
}
```

> **Tests:** Parser: `ParseFunctionEntry`, `ParseReturn`, `ParseSelect` | Compiler: `NewFunction`, `SelectNode`, `ReturnStatement` | Decompiler: `MultipleEntryPoints`, `ReturnNode` | Round-trip: `Function`, `Select`, `Return`

_See also: call("bpir.examples") for the full index._
