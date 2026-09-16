# bpir.examples.clear-dispatcher

## Clear Dispatcher

Unbind all delegates from a dispatcher on EndPlay. See `call("bpir.instructions")` §2.10 for `clear_dispatcher` and related dispatcher operations.

```
entry event BeginPlay() {
    bind_dispatcher OnScoreChanged(target: self, event: @HandleScore)
}

entry custom_event HandleScore(int NewScore) {
    call PrintString(InString: "Score changed!")
}

entry event EndPlay(enum EEndPlayReason::Type EndPlayReason) {
    clear_dispatcher OnScoreChanged(target: self)
}
```

> **Tests:** Parser: `ParseClearDispatcher`, `ClearDispatcher` | Compiler: `CompileClearDispatcher` | Decompiler: `DelegateNodeDecompile` | Round-trip: `ClearDispatcher`

_See also: call("bpir.examples") for the full index._
