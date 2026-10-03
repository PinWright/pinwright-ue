# bpir.examples.key-input

## Key Input

Respond to a keyboard key press. See `call("bpir.entry-points")` §1 for the `key_pressed` / `key_released` entry kinds.

```
entry key_pressed SpaceBar() {
    call Jump(Target: self)
}
```

Modifier keys go inside the parens. This entry fires on Ctrl+Shift+S only, not on a plain S press:

```
entry key_pressed S(ctrl, shift) {
    call PrintString(InString: "save all")
}
```

During decompilation, `key_pressed` and `key_released` are distinct entry identities even though an InputKey node exposes both exec outputs. Body traversal starts from the named `Pressed` or `Released` pin that matches the emitted signature. UE allocates `Pressed` first, so using generic exec-output index zero for a `key_released` entry would emit the correct signature but silently drop its body. An InputKey node with both `Pressed` and `Released` wired decompiles as two entries, a `key_pressed` and then a `key_released` with the same key, each holding its own body. Compilation creates one InputKey node per BPIR entry, so a round trip turns that node into two equivalent single-pin nodes.

> **Tests:** Parser: `ParseKeyPressedEntry`, `input_key_modifiers.ParseCtrlJ` | Compiler: `KeyPressedEntry` | Decompiler: `KeyReleasedEntry`, `input_key_modifiers.DecompileEmitsModifiers` | Round-trip: `KeyReleasedEntry`, `input_key_modifiers.RoundTrip`, `input_key_dual_active.EmitsBothEntriesAndRoundTrips`

_See also: call("bpir.examples") for the full index._
