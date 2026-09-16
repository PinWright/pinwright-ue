# bpir.examples.format-text

## Format Text

Use `Format` with named placeholders for string interpolation.

```
entry event BeginPlay() {
    %txt = call Format(Format: "{Name} has {HP} HP", Name: $PlayerName, HP: $Health)
    call PrintString(InString: %txt)
}
```

Supported aliases: `Format`, `Format_Text`, `FormatText` (case-insensitive). The `Format:` argument is required. Each `{placeholder}` creates a wildcard input pin. See `call("bpir.instructions")` §2.5 for the full instruction reference.

> **Tests:** Compiler support added. Decompiler has `FormatArgsDefaultValues` for default-value argument formatting. Round-trip test pending.

_See also: call("bpir.examples") for the full index._
