# bpir.examples.switch-string

## Switch on String

Route execution on a string value with quoted case labels.

```
entry custom_event HandleCommand(string Command) {
    %sw = switch_string($Command) ["start" -> @start, "stop" -> @stop, "reset" -> @reset, default -> @unknown]

@start:
    call BeginGame()
    exec -> @done

@stop:
    call EndGame()
    exec -> @done

@reset:
    call ResetGame()
    exec -> @done

@unknown:
    call PrintString(InString: "Unknown command")

@done:
}
```

> **Tests:** Parser: `ParseSwitchString` | Compiler: `CompileSwitchString` | Decompiler: `SwitchStringDecompile` | Round-trip: `SwitchString`

_See also: call("bpir.examples") for the full index._
