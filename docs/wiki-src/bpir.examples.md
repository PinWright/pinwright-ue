# bpir.examples

Companion to `call("bpir")`. Each example demonstrates a specific BPIR pattern with the exact syntax accepted by the compiler.

## Index

Use these for few-shot learning, developer onboarding, and as copy-paste templates when authoring BPIR code. Every example includes a test annotation showing which automated tests cover that pattern. Tests span four categories: **Parser** (syntax parsing), **Compiler** (graph node emission), **Decompiler** (graph-to-BPIR), and **Round-trip** (compile then decompile).

| # | Example | Slug |
|---|---------|------|
| 1 | Simple BeginPlay | `call("bpir.examples.simple-beginplay")` |
| 2 | If/Else with Reconvergence | `call("bpir.examples.if-else")` |
| 3 | Else-If Chain | `call("bpir.examples.else-if-chain")` |
| 4 | ForEach Loop | `call("bpir.examples.foreach-loop")` |
| 5 | Switch on Enum | `call("bpir.examples.switch-enum")` |
| 6 | Sequence | `call("bpir.examples.sequence")` |
| 7 | Cast with Failure | `call("bpir.examples.cast-with-failure")` |
| 8 | Delay (Latent) | `call("bpir.examples.delay")` |
| 9 | Timeline | `call("bpir.examples.timeline")` |
| 10 | DoOnce & FlipFlop | `call("bpir.examples.do-once-flipflop")` |
| 11 | Custom Event with Parameters | `call("bpir.examples.custom-event-with-params")` |
| 12 | Function with Return Value | `call("bpir.examples.function-with-return")` |
| 13 | Component Event | `call("bpir.examples.component-event")` |
| 14 | Widget Event | `call("bpir.examples.widget-event")` |
| 15 | Key Input | `call("bpir.examples.key-input")` |
| 16 | Event Dispatcher | `call("bpir.examples.event-dispatcher")` |
| 17 | Switch on Int | `call("bpir.examples.switch-int")` |
| 18 | Switch on String | `call("bpir.examples.switch-string")` |
| 19 | Switch on Enum (Typed) | `call("bpir.examples.switch-enum-typed")` |
| 20 | Clear Dispatcher | `call("bpir.examples.clear-dispatcher")` |
| 21 | Array Operations | `call("bpir.examples.array-ops")` |
| 22 | Struct Make/Break | `call("bpir.examples.struct-make-break")` |
| 23 | IsValid Pattern | `call("bpir.examples.isvalid-pattern")` |
| 24 | Nested Control Flow | `call("bpir.examples.nested-control-flow")` |
| 25 | Construction Script | `call("bpir.examples.construction-script")` |
| 26 | MultiGate | `call("bpir.examples.multigate")` |
| 27 | Format Text | `call("bpir.examples.format-text")` |
| 28 | Macro Definition (Pure) | `call("bpir.examples.macro-pure")` |
| 29 | Macro Definition (Multi-Exit) | `call("bpir.examples.macro-multi-exit")` |
| 30 | Async K2Node Call with Exec Branches | `call("bpir.examples.async-k2node")` |
| 31 | Cross-Widget Handler Discovery | `call("bpir.examples.cross-widget-handler")` |
| 32 | Backtick-Quoted Function Names with Spaces | `call("bpir.examples.backtick-quoted-names")` |

## Test Coverage Summary

| # | Example | Parser | Compiler | Decompiler | Round-trip |
|---|---------|--------|----------|------------|------------|
| 1 | Simple BeginPlay | `ParseCall` | `SimpleBeginPlay` | `SimpleChain` | `SimpleCall` |
| 2 | If/Else Reconvergence | `ParseBranch` | `IfElse` | `BranchNode` | `BranchReconverge` |
| 3 | Else-If Chain | `ParseBranch` | `ElseIfChain` | `Reconvergence` | `BranchReconverge` |
| 4 | ForEach Loop | `ParseForeach` | `ForEachLoop` | `ForeachLoop` | `ForEach` |
| 5 | Switch on Enum | `ParseSwitchEnum` | `CompileSwitchEnum` | `SwitchEnumDecompile` | `SwitchEnum` |
| 6 | Sequence | `ParseSequence` | `SequenceBlock` | `SequenceNode` | `Sequence` |
| 7 | Cast with Failure | `ParseCast` | `CastNode` | `CastNode` | `Cast` |
| 8 | Delay (Latent) | `ParseLatent` | `LatentDelay` | `LatentNode` | `Latent` |
| 9 | Timeline | `ParseTimeline` | `TimelineNode` | `Timeline` | `Timeline` |
| 10 | DoOnce & FlipFlop | `ParseMacro` | `MacroDoOnce`, `MacroFlipFlop` | -- | `MacroDoOnce`, `MacroFlipFlop` |
| 11 | Custom Event | `ParseCustomEventEntry` | `CustomEvent` | `CustomEvent` | `CustomEvent` |
| 12 | Function + Return | `ParseFunctionEntry` | `NewFunction`, `SelectNode` | `MultipleEntryPoints`, `ReturnNode` | `Function`, `Select`, `Return` |
| 13 | Component Event | `ParseComponentEventEntry` | `ComponentEventEntry` | -- | -- |
| 14 | Widget Event | `ParseWidgetEventEntry` | `WidgetEventEntry` | -- | -- |
| 15 | Key Input | `ParseKeyPressedEntry` | `KeyPressedEntry` | `KeyReleasedEntry` | `KeyReleasedEntry` |
| 16 | Event Dispatcher | `ParseCallDispatcher`, `ParseBindDispatcher` | `BindDispatcher`, `CallDispatcher` | `DelegateNodeDecompile` | `Dispatcher` |
| 17 | Switch on Int | `ParseSwitchInt` | `CompileSwitchInt` | `SwitchIntDecompile` | `SwitchInt` |
| 18 | Switch on String | `ParseSwitchString` | `CompileSwitchString` | `SwitchStringDecompile` | `SwitchString` |
| 19 | Switch on Enum (Typed) | `ParseSwitchEnum` | `CompileSwitchEnum` | `SwitchEnumDecompile` | `SwitchEnum` |
| 20 | Clear Dispatcher | `ParseClearDispatcher` | `CompileClearDispatcher` | `DelegateNodeDecompile` | `ClearDispatcher` |
| 21 | Array Operations | `ParseMakeArray` | `MakeArray` | `MakeArrayNode` | `MakeArray` |
| 22 | Struct Make/Break | `ParseMakeStruct`, `ParseBreakStruct` | `MakeStruct`, `BreakStruct` | `MakeStruct`, `BreakStructNode` | `MakeStruct`, `BreakStruct` |
| 23 | IsValid Pattern | `ParseBranch` | `IfElse` | `BranchNode` | `BranchReconverge` |
| 24 | Nested Control Flow | `ParseBranch`, `ParseForeach` | `DeeplyNestedControlFlow` | -- | -- |
| 25 | Construction Script | `ParseConstructionEntry` | `ConstructionEntry` | -- | -- |
| 26 | MultiGate | `ParseMacro` | `MacroMultiGate` | `MacroMultiGate` | `MacroMultiGate` |
| 27 | Format Text | -- | `FormatText` (via K2Node handler) | `FormatArgsDefaultValues` (partial) | -- |
| 28 | Macro Definition (Pure) | `MacroEntry` | (SetupMacro) | (decompile_macro) | `MacroSimple` |
| 29 | Macro Definition (Multi-Exit) | `MacroEntryMultiExit` | (SetupMacro) | (decompile_macro) | `MacroExecOnly` |
| 30 | Async K2Node Call with Exec Branches | -- | -- | -- | -- |
| 31 | Cross-Widget Handler Discovery | -- | -- | -- | -- |
| 32 | Backtick-Quoted Function Names | -- | -- | -- | -- |

**Coverage:** Examples 1–29 all have compiler coverage. Examples 30–32 are documented BP wiring patterns without dedicated tests; the Format Text round-trip is pending. Example #20 (Clear Dispatcher) also has decompiler coverage through `DelegateNodeDecompile`.

## See also

- `call("bpir")` — BPIR language reference landing page
- `call("bpir.instructions")` — instruction syntax (call, branch, foreach, switch, cast, latent, timeline, macro, dispatcher operations)
