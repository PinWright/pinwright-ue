# bpir.examples.nested-control-flow

## Nested Control Flow

Branch inside a ForEach loop with validity checks -- demonstrates composing control flow constructs. See `call("bpir.instructions")` §2.3 for the individual `branch` and `foreach` references and §2.8 for label/exec-wiring rules.

```
entry event BeginPlay() {
    %b1 = branch($bEnabled) [true -> @enabled, false -> @end]

@enabled:
    %loop = foreach($Items) [body -> @each, completed -> @after_loop]

@each:
    %ok = call IsValid(Object: %loop.ArrayElement)
    %b2 = branch(%ok) [true -> @process, false -> @skip]

@process:
    call ProcessItem(Item: %loop.ArrayElement)

@skip:
    # auto-continues loop

@after_loop:
    call PrintString(InString: "All items processed")

@end:
}
```

> **Tests:** Parser: `ParseBranch`, `ParseForeach` | Compiler: `DeeplyNestedControlFlow` | Decompiler: _no dedicated test_ | Round-trip: _no dedicated test_

_See also: call("bpir.examples") for the full index._
