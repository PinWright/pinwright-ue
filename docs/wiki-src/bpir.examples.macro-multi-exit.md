# bpir.examples.macro-multi-exit

## Macro Definition (Multi-Exit)

Define a macro with multiple named exec exit paths. The caller sees each exit as a separate exec output pin on the macro instance node. See `call("bpir.entry-points")` §1c for the multi-exit declaration syntax and `call("bpir.instructions")` §2.9 for the `return [ExitPin]` variants.

```
entry macro CheckValid(object<Object> Target) -> [IsValid -> @valid, IsNotValid -> @invalid] {
    %ok = call IsValid(Object: $Target)
    %b = branch(%ok) [true -> @yes, false -> @no]

@yes:
    return [IsValid]

@no:
    return [IsNotValid]
}
```

> **Tests:** Parser: `MacroEntryMultiExit`, `MacroEntryDataAndExec` | Compiler: (via SetupMacro) | Decompiler: (via decompile_macro) | Round-trip: `MacroExecOnly`

_See also: call("bpir.examples") for the full index._
