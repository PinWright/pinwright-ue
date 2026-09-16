# bpir.examples.key-input

## Key Input

Respond to a keyboard key press. See `call("bpir.entry-points")` §1 for the `key_pressed` / `key_released` entry kinds.

```
entry key_pressed SpaceBar() {
    call Jump(Target: self)
}
```

During decompilation, `key_pressed` and `key_released` are distinct entry identities even though an InputKey node exposes both exec outputs. Body traversal starts from the named `Pressed` or `Released` pin that matches the emitted signature. UE allocates `Pressed` first, so using generic exec-output index zero for a `key_released` entry would emit the correct signature but silently drop its body. Compilation currently creates one InputKey node per logical BPIR entry; sharing one node across both pins would require a broader node-and-pin entry representation.

> **Tests:** Parser: `ParseKeyPressedEntry` | Compiler: `KeyPressedEntry` | Decompiler: `KeyReleasedEntry` | Round-trip: `KeyReleasedEntry`

_See also: call("bpir.examples") for the full index._
