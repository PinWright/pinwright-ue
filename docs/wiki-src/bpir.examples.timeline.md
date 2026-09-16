# bpir.examples.timeline

## Timeline

Create a timeline with a float curve. Access track outputs via `%tl.Alpha`. See `call("bpir.instructions")` §2.6 for the `timeline` instruction reference.

```
entry event BeginPlay() {
    %tl = timeline FadeIn(
        Alpha: float_curve((0.0, 0.0), (2.0, 1.0))
    ) [update -> @tick, finished -> @done]

@tick:
    %scale = make<Vector>(X: %tl.Alpha, Y: %tl.Alpha, Z: %tl.Alpha)
    call SetActorScale3D(NewScale3D: %scale)

@done:
    call PrintString(InString: "Fade complete")
}
```

> **Tests:** Parser: `ParseTimeline` | Compiler: `TimelineNode` | Decompiler: `Timeline` | Round-trip: `Timeline`

_See also: call("bpir.examples") for the full index._
