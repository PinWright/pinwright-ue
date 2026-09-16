# bpir.examples.foreach-loop

## ForEach Loop

Iterate over an array variable with body and completed branches. See `call("bpir.instructions")` §2.3 for the full loop instruction reference (including `foreach_break` and `while`).

```
entry event BeginPlay() {
    %loop: object<Item> = foreach($Inventory) [body -> @body, completed -> @after]

@body:
    %name: string = call GetDisplayName(Object: %loop.ArrayElement)
    call PrintString(InString: %name)

@after:
    call PrintString(InString: "All items listed")
}
```

> **Tests:** Parser: `ParseForeach` | Compiler: `ForEachLoop` | Decompiler: `ForeachLoop` | Round-trip: `ForEach`

_See also: call("bpir.examples") for the full index._
