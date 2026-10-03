# bpir.examples.struct-make-break

## Struct Make/Break

Construct structs with `make<Type>` and decompose them with `break<Type>`. Access fields via dot notation. See `call("bpir.instructions")` §2.7 for the instruction reference and `call("bpir.errors")` for the auto-BreakStruct shortcut on struct-returning calls.

`break<Type>` builds a Break Struct node for structs **without** a native break function (`Margin` and most project structs). For a struct whose type names a native break function (`HasNativeBreak`: `HitResult`, `Rotator`, `Vector`, `Transform`, ...) it emits that function instead — `break<HitResult>(%h)` is `call BreakHitResult(Hit: %h)`, `break<Rotator>(%r)` is `call BreakRotator(InRot: %r)` — so members are the function's output pins. Dotted member access straight off the struct pin (`%hit.OutHit.BoneName`) routes through the same function.

```
entry event BeginPlay() {
    %origin: struct<Vector> = make<Vector>(X: 0.0, Y: 0.0, Z: 100.0)
    %end: struct<Vector> = make<Vector>(X: 0.0, Y: 0.0, Z: -500.0)
    %channel = enum ETraceTypeQuery::TraceTypeQuery1
    %hit: bool = call LineTraceByChannel(Start: %origin, End: %end, TraceChannel: %channel)
    %b = call BreakHitResult(Hit: %hit.OutHit)
    call PrintString(InString: %b.BoneName)
    %margin: struct<Margin> = make<Margin>(Left: 1.0, Top: 2.0, Right: 3.0, Bottom: 4.0)
    %m = break<Margin>(%margin)
    call PrintString(InString: %m.Left)
}
```

`TraceTypeQuery1` is the `Visibility` trace channel. `BreakHitResult` outputs are its parameter names (`BoneName`, `HitBoneName`, `Location`, `ImpactPoint`, `HitActor`, ...).

> **Tests:** This exact block is compiled by `PinWright.infra.wiki_handler.Topic.BpirStructMakeBreakExampleCompiles` | Parser: `ParseMakeStruct`, `ParseBreakStruct` | Compiler: `MakeStruct`, `BreakStruct` (`break<Margin>`) | Decompiler: `MakeStruct`, `BreakStructNode` | Round-trip: `MakeStruct`, `BreakStruct` (`break<Margin>`)

_See also: call("bpir.examples") for the full index._
