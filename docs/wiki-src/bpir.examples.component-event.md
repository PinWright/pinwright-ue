# bpir.examples.component-event

## Component Event

Bind to a component's delegate (e.g., overlap events) using `component_event`. See `call("bpir.entry-points")` §1 for the entry-kinds list, including `component_event ComponentName.DelegateName(...)`.

```
entry component_event TriggerBox.OnComponentBeginOverlap(object<AActor> OtherActor) {
    %valid = call IsValid(Object: $OtherActor)
    %b = branch(%valid) [true -> @process, false -> @end]

@process:
    call PrintString(InString: "Something entered the trigger!")

@end:
}
```

> **Tests:** Parser: `ParseComponentEventEntry` | Compiler: `ComponentEventEntry` | Decompiler: _no dedicated test_ | Round-trip: _no dedicated test_

_See also: call("bpir.examples") for the full index._
