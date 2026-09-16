# bpir.examples.simple-beginplay

## Simple BeginPlay

Minimal BPIR: one entry point calls one function.

```
entry event BeginPlay() {
    call PrintString(InString: "Hello World")
}
```

> **Tests:** Parser: `ParseCall` | Compiler: `SimpleBeginPlay` | Decompiler: `SimpleChain` | Round-trip: `SimpleCall`

_See also: call("bpir.examples") for the full index._
