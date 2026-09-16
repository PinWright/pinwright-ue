# bpir.examples.isvalid-pattern

## IsValid Pattern

Check validity before accessing an object. See `call("bpir.errors")` "Member functions need `Target:`" when `IsValid` returns a typed object whose methods you then call.

```
entry event BeginPlay() {
    %owner: object<Actor> = call GetOwner(Target: self)
    %ok: bool = call IsValid(Object: %owner)
    %b = branch(%ok) [true -> @valid, false -> @end]

@valid:
    %loc: struct<Vector> = call GetActorLocation(Target: %owner)
    call PrintString(InString: %loc)

@end:
}
```

> **Tests:** Parser: `ParseBranch`, `ParsePure` | Compiler: `IfElse` | Decompiler: `BranchNode` | Round-trip: `BranchReconverge`

_See also: call("bpir.examples") for the full index._
