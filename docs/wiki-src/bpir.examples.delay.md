# bpir.examples.delay

## Delay (Latent)

Latent node that pauses execution and resumes at a label after the duration. See `call("bpir.instructions")` §2.1 for the `latent` keyword and the `[completed -> @label]` exec-branch wiring.

```
entry event BeginPlay() {
    call PrintString(InString: "Starting countdown...")
    %d = latent Delay(Duration: 3.0) [completed -> @go]

@go:
    call PrintString(InString: "GO!")
}
```

> **Tests:** Parser: `ParseLatent`, `ParseLatentTopLevel` | Compiler: `LatentDelay` | Decompiler: `LatentNode` | Round-trip: `Latent`

_See also: call("bpir.examples") for the full index._
