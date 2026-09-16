# bpir.examples.switch-int

## Switch on Int

```
entry event BeginPlay() {
    %sw = switch_int($WaveNumber) [0 -> @tutorial, 1 -> @easy, 2 -> @hard, default -> @boss]

@tutorial:
    call PrintString(InString: "Tutorial wave")
    exec -> @done

@easy:
    call PrintString(InString: "Easy wave")
    exec -> @done

@hard:
    call PrintString(InString: "Hard wave")
    exec -> @done

@boss:
    call PrintString(InString: "Boss wave")

@done:
    call StartWave()
}
```

> **Constraint:** case labels must be a contiguous ascending run of plain decimal integers. `[1, 5, 9]` is rejected — the engine renumbers `switch_int` case pins positionally from the node's `StartIndex` on load, so a gapped set would change which arm each case runs.

> **Tests:** Parser: `ParseSwitchInt` | Compiler: `CompileSwitchInt` | Decompiler: `SwitchIntDecompile` | Round-trip: `SwitchInt` | Reconstruction: `switch_int.CaseLabelsSurviveReconstruction`, `switch_int.OutOfOrderLabelsAreLaidOutAscending`, `switch_int.NonContiguousLabelsRejected`
