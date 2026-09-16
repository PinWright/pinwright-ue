# bpir.examples.event-dispatcher

## Event Dispatcher

Bind, call, and handle an event dispatcher. Demonstrates multi-entry-block BPIR with dispatcher operations. See `call("bpir.instructions")` §2.10 for the full dispatcher reference (`call_dispatcher`, `bind_dispatcher`, `unbind_dispatcher`, `clear_dispatcher`, and external `Target:` targets).

```
entry event BeginPlay() {
    bind_dispatcher OnScoreChanged(target: self, event: @HandleScore)
    set Score = 0
}

entry custom_event HandleScore(int NewScore) {
    call PrintString(InString: "Score updated!")
}

entry custom_event AddPoints(int Points) {
    %new = call Add_IntInt(A: $Score, B: $Points)
    set Score = %new
    call_dispatcher OnScoreChanged(NewScore: $Score)
}
```

> **Tests:** Parser: `ParseCallDispatcher`, `ParseBindDispatcher` | Compiler: `BindDispatcher`, `CallDispatcher` | Decompiler: `DelegateNodeDecompile` | Round-trip: `Dispatcher`

_See also: call("bpir.examples") for the full index._
