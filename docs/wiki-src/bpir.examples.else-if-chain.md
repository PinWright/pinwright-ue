# bpir.examples.else-if-chain

## Else-If Chain

Multiple chained branches testing different conditions, converging at a single exit label.

```
entry event BeginPlay() {
    %gt75 = call Greater_FloatFloat(A: $Health, B: 75.0)
    %b1 = branch(%gt75) [true -> @high, false -> @check_mid]

@check_mid:
    %gt25 = call Greater_FloatFloat(A: $Health, B: 25.0)
    %b2 = branch(%gt25) [true -> @mid, false -> @low]

@high:
    call PrintString(InString: "Healthy")
    exec -> @done

@mid:
    call PrintString(InString: "Wounded")
    exec -> @done

@low:
    call PrintString(InString: "Critical")

@done:
    call PrintString(InString: "Status checked")
}
```

> **Tests:** Parser: `ParseBranch` | Compiler: `ElseIfChain`, `NestedIfElse` | Decompiler: `Reconvergence` | Round-trip: `BranchReconverge`

_See also: call("bpir.examples") for the full index._
