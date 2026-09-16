# bpir.examples.sequence

## Sequence

Execute multiple branches in order using indexed output pins.

```
entry event BeginPlay() {
    %seq = sequence(3) [0 -> @init, 1 -> @spawn, 2 -> @notify]

@init:
    set Health = 100.0
    set bIsAlive = true

@spawn:
    %loc = call GetActorLocation(Target: self)
    call SpawnEffect(Location: %loc)

@notify:
    call PrintString(InString: "Setup complete")
}
```

> **Tests:** Parser: `ParseSequence` | Compiler: `SequenceBlock` | Decompiler: `SequenceNode` | Round-trip: `Sequence`

_See also: call("bpir.examples") for the full index._
