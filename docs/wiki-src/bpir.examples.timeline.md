# bpir.examples.timeline

## Timeline

Create a looping timeline with a float track, a vector track and an event track. Read track outputs as `%tl.Alpha` / `%tl.Offset`; wire an event track's exec pin in the exec clause. See `call("bpir.instructions")` §2.6 for the settings, track forms and key syntax.

```
entry event BeginPlay() {
    %tl = timeline FadeIn(length: 2, loop: true, Alpha: float_curve((0, 0), (2, 1, cubic)), Offset: vector_curve(z((0, 0), (2, 100))), Beep: event_curve((1, 0))) [update -> @tick, Beep -> @beep]

@tick:
    %scale = make<Vector>(X: %tl.Alpha, Y: %tl.Alpha, Z: %tl.Alpha)
    call SetActorScale3D(NewScale3D: %scale)
    exec -> @done

@beep:
    call PrintString(InString: "Halfway")
    exec -> @done

@done:
}
```

The `timeline` instruction must stay on one line.

> **Tests:** Parser: `ParseTimeline` | Compiler: `TimelineNode` | Decompiler: `Timeline` | Round-trip: `Timeline`, `timeline_tracks.*`

_See also: call("bpir.examples") for the full index._
