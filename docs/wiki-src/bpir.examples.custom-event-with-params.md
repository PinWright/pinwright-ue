# bpir.examples.custom-event-with-params

## Custom Event with Parameters

Define a typed custom-event entry point; see `call("bpir.entry-points")` §1 for the `custom_event` declaration and `$ParamName` or bare-name parameter references.

```
entry custom_event ApplyDamage(float Amount, object<AActor> Source) {
    %new = call Subtract_FloatFloat(A: $Health, B: $Amount)
    set Health = %new
    %dead = call LessEqual_FloatFloat(A: $Health, B: 0.0)
    %b = branch(%dead) [true -> @die, false -> @end]

@die:
    call DestroyActor(Target: self)

@end:
}
```

> **Tests:** Parser: `ParseCustomEventEntry` | Compiler: `CustomEvent` | Decompiler: `CustomEvent` | Round-trip: `CustomEvent`
