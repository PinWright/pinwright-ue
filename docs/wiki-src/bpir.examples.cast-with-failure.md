# bpir.examples.cast-with-failure

## Cast with Failure

Dynamic cast with success and failure branches. Access the cast result via `%cast.AsMyCharacter`. See `call("bpir.instructions")` §2.3 for the cast statement form and the pure RHS-expression form (`set %typed = cast<T>(%src)`).

```
entry event BeginPlay() {
    %pawn: object<Pawn> = call GetPlayerPawn(PlayerIndex: 0)
    %cast: object<MyCharacter> = cast<MyCharacter>(%pawn) [success -> @ok, fail -> @fail]

@ok:
    call DoAbility(Target: %cast.AsMyCharacter)
    exec -> @done

@fail:
    call PrintString(InString: "Wrong character class")

@done:
}
```

> **Tests:** Parser: `ParseCast` | Compiler: `CastNode` | Decompiler: `CastNode` | Round-trip: `Cast`

_See also: call("bpir.examples") for the full index._
