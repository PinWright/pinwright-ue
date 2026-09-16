# bpir.pure-impure

Reference table for which BPIR instructions are pure vs impure, whether they have exec pins, and whether they auto-chain. See `call("bpir")` for the language overview, `call("bpir.instructions")` for instruction syntax.

## 6. Pure vs Impure

Bindings may carry an optional `: Type` annotation on the register (see the "Optional register-binding type annotation" subsection of `call("bpir.instructions")` §2.1). Examples below show the annotated form; un-annotated bindings remain valid.

| Syntax | Type | Exec Pins? | Auto-chains? |
|--------|------|-----------|-------------|
| `call Func(...)` | Auto-detected | If impure: Yes | If impure: Yes |
| `%n: bool = call Func(...)` | Auto-detected | If impure: Yes | If impure: Yes |
| `%n: float = pure Func(...)` | _(deprecated alias for `call`)_ | Auto-detected | Auto-detected |
| `set Var = ...` | Impure | Yes | Yes |
| `%n: int = get Var` | Pure | No | No |
| `%n = branch(...)` | Impure | Yes (multi) | No — uses `[...]` |
| `%n: float = select(...)` | Pure | No | No |
| `%n: float = break<Vector>(...)` | Pure | No | No |
| `%n: struct<Vector> = make<Vector>(...)` | Pure | No | No |
| `%n = self` | Pure | No | No |
| `%n: EWindowMode = enum EWindowMode::Fullscreen` | Pure | No | No |
| `%n: array<float> = make_array(...)` | Pure | No | No |
| `%n: set<int> = make_set(...)` | Pure (decompiler-only) | No | No |
| `%n: map<string, int> = make_map(...)` | Pure (decompiler-only) | No | No |
| `%n: int = foreach(...)` | Impure | Yes (multi) | No — uses `[...]` |
| `%n: int = foreach_break(...)` | Impure | Yes (multi) | No — uses `[...]` |
| `%n = while(...)` | Impure | Yes (multi) | No — uses `[...]` |
| `%n = switch_int(...)` | Impure | Yes (multi) | No — uses `[...]` |
| `%n = switch_string(...)` | Impure | Yes (multi) | No — uses `[...]` |
| `%n = switch_enum<EWindowMode>(...)` | Impure | Yes (multi) | No — uses `[...]` |
| `%n = sequence(...)` | Impure | Yes (multi) | No — uses `[...]` |
| `%c: object<Pawn> = cast<Pawn>(...)` | Impure | Yes (multi) | No — uses `[...]` |
| `%n = macro Name(...)` | Impure | Yes (multi) | No — uses `[...]` |
| `%n = timeline Name(...)` | Impure | Yes (multi) | No — uses `[...]` |
| `%h: object<MoveComponentToAction> = latent Func(...)` | Impure | Yes (multi) | No — uses `[...]` |
| `exec -> @label` | Impure | Yes | No — explicit wiring |
| `end` | Structural (non-impure) | No — node-less | No — terminates the current chain |
| `call_dispatcher Name(...)` | Impure | Yes | Yes |
| `bind_dispatcher Name(...)` | Impure | Yes | Yes |
| `unbind_dispatcher Name(...)` | Impure | Yes | Yes |
| `clear_dispatcher Name(...)` | Impure | Yes | Yes |
| `field_notify_subscribe Name(...)` | Impure | Yes | Yes |
| `field_notify_unsubscribe Name(...)` | Impure | Yes | Yes |
| `return` | Impure | Yes | No — terminal |
| `return (Name: val)` | Impure | Yes | No — terminal (named outputs) |
| `return [ExitPin]` | Impure | Yes | No — terminal (macro exit) |
| `return [ExitPin] (...)` | Impure | Yes | No — terminal (macro exit + data) |

## Discovering the concrete pure function name

The table classifies purity by *syntax shape*, not the Kismet UFunctions a pure
`call` targets. Common examples are `Add_FloatFloat` / `Subtract_DoubleDouble`
(math — see `call("bpir.instructions")` §2.1), `make<Vector>` / `break<HitResult>`
(struct ops — §2.7), `GetActorLocation`, and `GetObjectName`. For an unknown pure
utility (string concat, numeric/struct→string conversion, or clamp), **do not read
engine headers**: run
`blueprint.build_api_index(classFilter=["KismetStringLibrary","KismetMathLibrary","KismetSystemLibrary"])`,
then `blueprint.search_api("<verb>")` using the **plain verb** (`concat`, `append
string`, `clamp`, `to string`), not the C++ symbol. Put the returned UFunction name
directly in `call`. This is the `build_api_index` → `search_api` chain linked from
`blueprint.graph`; it discovers pure helpers within the RPC surface.

**Float pins are doubles.** UE 5 Blueprint "float" pins are 64-bit reals, so use
`<Op>_DoubleDouble` (`Add_DoubleDouble`, `Multiply_DoubleDouble`,
`Greater_DoubleDouble`) for canonical arithmetic/compare calls. The legacy
`<Op>_FloatFloat` spelling targets true 32-bit `float` pins in some examples. Both
compile; the decompiler normalizes `_FloatFloat` → `_DoubleDouble`. Prefer
`_DoubleDouble` for default float pins.

**High-frequency pure utilities** — the zero-discovery shortcut for the cases
that recur most; use `search_api` above for anything not listed:

| Intent | Pure `call` | Library |
|---|---|---|
| append two strings | `call Concat_StrStr(A: %a, B: %b)` | `KismetStringLibrary` |
| number → string | `call Conv_DoubleToString(InDouble: %n)` (int: `Conv_IntToString(InInt: %i)`) | `KismetStringLibrary` |
| struct → string | `call Conv_VectorToString(InVec: %v)` | `KismetStringLibrary` |
| clamp a real | `call FClamp(Value: %v, Min: 0.0, Max: 1.0)` | `KismetMathLibrary` |
