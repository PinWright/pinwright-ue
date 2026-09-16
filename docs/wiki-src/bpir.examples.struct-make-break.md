# bpir.examples.struct-make-break

## Struct Make/Break

Construct structs with `make<Type>` and decompose them with `break<Type>`. Access fields via dot notation. See `call("bpir.instructions")` §2.7 for the instruction reference and `call("bpir.errors")` for the auto-BreakStruct shortcut on struct-returning calls.

```
entry event BeginPlay() {
    %origin: struct<Vector> = make<Vector>(X: 0.0, Y: 0.0, Z: 100.0)
    %end: struct<Vector> = make<Vector>(X: 0.0, Y: 0.0, Z: -500.0)
    %hit: bool = call LineTraceByChannel(Start: %origin, End: %end, TraceChannel: enum ETraceTypeQuery::Visibility)
    %result: struct<Vector> = break<HitResult>(%hit.OutHit)
    call PrintString(InString: %result.BoneName)
}
```

> **Tests:** Parser: `ParseMakeStruct`, `ParseBreakStruct` | Compiler: `MakeStruct`, `BreakStruct` | Decompiler: `MakeStruct`, `BreakStructNode` | Round-trip: `MakeStruct`, `BreakStruct`

_See also: call("bpir.examples") for the full index._
