# bpir.examples.if-else

## If/Else with Reconvergence

Branch on a boolean variable, execute different paths, then reconverge at a shared label.

```
entry event BeginPlay() {
    %b = branch($bIsReady) [true -> @then, false -> @else]

@then:
    call PrintString(InString: "Ready!")
    exec -> @done

@else:
    call PrintString(InString: "Not ready")

@done:
    call PrintString(InString: "Finished")
}
```

> **Tests:** Parser: `ParseBranch` | Compiler: `IfElse` | Decompiler: `BranchNode`, `Reconvergence` | Round-trip: `BranchReconverge`

_See also: call("bpir.examples") for the full index._
