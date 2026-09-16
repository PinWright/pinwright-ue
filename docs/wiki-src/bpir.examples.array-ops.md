# bpir.examples.array-ops

## Array Operations

Create arrays with `make_array` and call array utility functions. See `call("bpir.instructions")` §2.11 for `make_array` / `make_set` / `make_map` literals and the `Array_Get` accessor aliases (`.Item`, `.Result`, `.Value`).

```
entry event BeginPlay() {
    %arr = make_array("Apple", "Banana", "Cherry")
    call Array_Add(TargetArray: $Inventory, NewItem: "Sword")
    %len = call Array_Length(TargetArray: $Inventory)
    call PrintString(InString: %len)
}
```

> **Tests:** Parser: `ParseMakeArray` | Compiler: `MakeArray` | Decompiler: `MakeArrayNode` | Round-trip: `MakeArray`

_See also: call("bpir.examples") for the full index._
