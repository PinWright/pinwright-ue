---
type: system
summary: "BPIR test coverage matrix: 329 tests across 13 files mapped by feature×layer (tokenizer, parser, compiler, decompiler, round-trip) plus node layout engine unit tests. Includes macro graph entry, multi-output return, multi-exit return, multi-dot set LHS parsing, enum defaults on generic byte pins, external target delegate binding, authored node positions, node layout formatting pipeline. Known gaps with remediation guidance."
date: 2026-05-01
tags: [bpir, testing, coverage, gaps]
---

# BPIR Test Coverage Matrix

Maps every BPIR feature to its test coverage across 5 layers: Tokenizer, Parser, Compiler, Decompiler, Round-Trip. Use this to find "where is the test for X?" and "what has no test?"

Total: **329 tests** across 13 files.

---

## 1. Test File Index

| File | Tests | Layer | Focus |
|------|------:|-------|-------|
| TestBpirTokenizer.cpp | 24 | Lexical | Token types, sigils, escapes, string literals, numbers, comments, angle brackets, brackets, colon, dispatcher keywords, switch variant keywords, foreach_break, entry, self, make_array, subsystem, asset reference paths, percent/dollar ref dot-pin notation |
| TestBpirParser.cpp | 70 | Syntax | All instruction opcodes, entry types, pin normalization, index building, error recovery, external property set (% and $ forms, multi-dot and single-dot LHS splitting, dollar-var regression), multi-entry, function params, call/bind dispatcher, unmatched bracket, exec clause errors, macro entry (simple, backtick, multi-exit, data+exec), return (named args, exit pin) |
| TestBpirExpression.cpp | 20 | Node | UK2Node_BpirExpression: pure/impure, pin management, declarations, error states, typed pins (softobject, softclass, interface, delegate, mcdelegate, wildcard, FRotator, FLinearColor, bool), multi-typed decls |
| TestBpirRoundTrip.cpp | 34 | Fidelity | Compile → decompile → recompile identity for all major features, macro graphs (simple, exec-only, backtick name), chained property access (%ref.Pin.Property), auto-BreakStruct (%ref.Member on struct return) |
| TestCompilerIntegration.cpp | 43 | E2E | All instruction types to graph, insertion API, multi-entry blocks, legacy brace syntax |
| TestCompilerAdvanced.cpp | 5 | E2E | Latent, cast, timeline, make/break struct |
| TestCompilerDispatchersOps.cpp | 10 | E2E | Enum literal, enum ref, make_array, call/bind/unbind dispatcher (with and without real delegates), enum defaults on generic byte pins (EqualEqual_ByteByte + ETraceTypeQuery), bind_dispatcher with external target (graceful failure) |
| TestCompilerErrors.cpp | 12 | E2E | Error detection, duplicate names/labels, rollback, malformed entry, empty block, forward refs, mismatched braces |
| TestCompilerMacrosEntryPoints.cpp | 7 | E2E | Macros (DoOnce, FlipFlop, Gate, MultiGate), construction entry, select, foreach_break |
| TestCompilerGapCoverage.cpp | 7 | E2E | Subsystem access, unbind_dispatcher, component_event, widget_event, key_pressed, key_released, comment instruction |
| TestCompilerResolvers.cpp | 36 | Unit | Function resolution (exact, alias, library search, conversions), pin resolution (register, literal, clear, type conversion for 20+ types incl. class, struct, enum wrappers, set/map gap docs) |
| TestDecompiler.cpp | 38 | E2E | Graph → text topology, entry point discovery, node classification, error cases, external property decompilation, loops, cast, select, switch_enum, macros, variables, return, make/break struct, make_array |
| TestBpirCompositeEntryName.cpp | 4 | E2E | Decompiler entry-signature naming on composite-bearing graphs: source graph name survives the clone+inline pre-pass (backtick-quoted spaced function name, `entry macro`, `entry override` keyword survival on child BPs), byte-identical repeated decompiles under unique-name-counter perturbation |
| TestBpirForwardReference.cpp | 2+ | E2E | Forward reference across entry blocks: function calling a later-declared function, custom_event calling a later-declared custom_event — verifies Phase 1 entry-creation + Phase 1.5 skeleton recompile enables cross-entry forward calls |
| TestBpirMultiBranchReturn.cpp | 2 | E2E + Fidelity | One `UK2Node_FunctionResult` per `return`: a two-branch multi-output function keeps each branch's own data, wires the branch arms to different nodes, orphans nothing, and decompiles to two named `return (...)` lines that recompile to the same shape (`B-bpir-second-return-branch-data-dropped`) |
| TestNodeLayout.cpp | 25 | Unit | Node layout engine: EstimateNodeSize, GetNodeBounds, BuildFormatXInfoMap (linear/branch/diamond), FormatX (spacing/cluster/grid), FNodeLayoutParameterFormatter (basic/consumer-only), FormatParameterNodes (empty/single/shared-pure), GetPinsOfSameHeight (linear/branch/empty), FormatY (same-row/stacking/collision/obstacles), ResetRelativeToAnchor, SnapToGrid (directional), end-to-end pipeline, kill-switch |

---

## 2. Feature Coverage

### 2.1 Instructions

| Feature | Tokenizer | Parser | Compiler | Decompiler | Round-Trip | Status |
|---------|-----------|--------|----------|------------|------------|--------|
| `call` (impure) | `KeywordTokenization` | `ParseCall`, `ParseCallWithExecTargets`, `ParseCallK2NodeClass` | `SimpleBeginPlay`, `MultipleBodyStatements`, `CompileCallWithMultiExec`, `CompileGenericK2Node` | `SimpleChain`, `GenericNodeSingleExec`, `GenericNodeMultiExec` | `SimpleCall`, `GenericNode` | Full |
| `pure` | `SigilTokens` | `ParsePure` | `SimpleBeginPlay` (inline) | `SimpleChain` (inline) | `SimpleCall` (inline) | Full |
| `latent` | --- | `ParseLatent`, `ParseLatentTopLevel` | `LatentDelay` | `LatentNode` | `Latent` | Full |
| `set` | --- | `ParseSet`, `InlineCommentStripping.Set` | `VariableDeclaration` | `VariableSet`, `VariableSetOutputGet` | `SetVariable` | Full |
| `get` | --- | `ParseGet`, `InlineCommentStripping.Get` | `GetOpcode` | `VariableGet` | `GetVariable` | Full |
| `branch` | `LabelRef` | `ParseBranch` | `IfElse`, `IfBraceOnSameLine`, `IfBraceOnNextLine`, `IfElseBraceOnSameLine`, `IfElseAllmanStyle`, `ElseIfChain`, `ElseIfWithoutFinalElse`, `NestedIfElse`, `DeeplyNestedControlFlow` | `BranchNode`, `Reconvergence` | `BranchReconverge` | Full |
| `foreach` | `LabelTokens` | `ParseForeach` | `ForEachLoop` | `ForeachLoop` | `ForEach` | Full |
| `foreach_break` | `ForeachBreakKeyword` | `ParseForeachBreak` | `ForeachBreak` | --- | `ForeachBreak` | Partial |
| `while` | --- | `ParseWhile` | `WhileLoop` | `WhileLoop` | `While` | Full |
| `switch` (generic) | --- | `ParseSwitchGeneric` | `SwitchStatement` | --- | --- | Partial |
| `switch_int` | --- | `ParseSwitchInt` | `CompileSwitchInt`, `switch_int.NonContiguousLabelsRejected` | `SwitchIntDecompile` | `SwitchInt`, `switch_int.CaseLabelsSurviveReconstruction`, `switch_int.OutOfOrderLabelsAreLaidOutAscending` | Full |
| `switch_string` | --- | `ParseSwitchString` | `CompileSwitchString` | `SwitchStringDecompile` | `SwitchString` | Full |
| `switch_enum` | `SwitchVariantKeywords` | `ParseSwitchEnum` | `CompileSwitchEnum` | `SwitchEnumDecompile` | `SwitchEnum` | Full |
| `sequence` | --- | `ParseSequence` | `SequenceBlock` | `SequenceNode` | `Sequence` | Full |
| `cast` | `CastAngleBrackets` | `ParseCast` | `CastNode` | `CastNode` | `Cast` | Full |
| `select` | --- | `ParseSelect` | `SelectNode` | `SelectNode` | `Select`, `SelectTrueFalseMapping`, `SelectEnumAllocatesAllOptions`, `SelectTextOptionLiteral` | Full |
| `macro DoOnce` | --- | `ParseMacro` | `MacroDoOnce` | --- | `MacroDoOnce` | Partial |
| `macro FlipFlop` | --- | `ParseMacro` | `MacroFlipFlop` | --- | `MacroFlipFlop` | Partial |
| `macro Gate` | --- | `ParseMacro` | `MacroGate` | `MacroGate` | `MacroGate` | Full |
| `macro MultiGate` | --- | `ParseMacro` | `MacroMultiGate` | `MacroMultiGate` | `MacroMultiGate` | Full |
| `timeline` | --- | `ParseTimeline` | `TimelineNode` | `Timeline` | `Timeline` | Full |
| `make_struct` | --- | `ParseMakeStruct` | `MakeStruct` | `MakeStruct` | `MakeStruct` | Full |
| `break_struct` | --- | `ParseBreakStruct` | `BreakStruct` | `BreakStructNode` | `BreakStruct` | Full |
| `make_array` | `MakeArrayKeyword` | `ParseMakeArray` | `MakeArray` | `MakeArrayNode` | `MakeArray`, `MakeArrayTextElementLiteral` | Full |
| `make_set` | --- | --- | --- | --- | --- | **Gap** |
| `make_map` | --- | --- | --- | --- | --- | **Gap** |
| `enum` literal | --- | `ParseEnum`, `InlineCommentStripping.Enum` | `EnumLiteral`, `EnumLiteralRef` | --- | `Enum` | Partial |
| `self` | `SelfKeyword` | `ParseSelf` | `GenericNode` (inline) | --- | `GenericNode` (inline) | Partial |
| `subsystem` | `SubsystemIdentifier` | `ParseSubsystem` | `SubsystemAccess` | --- | --- | Partial |
| `call_dispatcher` | `DispatcherKeywords` | `ParseCallDispatcher` | `CallDispatcher`, `CallDispatcher_WithDelegate` | --- | `Dispatcher` | Partial |
| `bind_dispatcher` | `DispatcherKeywords` | `ParseBindDispatcher` | `BindDispatcher`, `BindDispatcher_WithDelegate`, `BindDispatcher_ExternalTarget_NoCrash` | --- | `Dispatcher` | Partial |
| `unbind_dispatcher` | `DispatcherKeywords` | `UnbindDispatcher` | `UnbindDispatcher`, `UnbindDispatcher_WithDelegate`, `UnbindDispatcher_NoDelegate` | --- | --- | Partial |
| `clear_dispatcher` | `DispatcherKeywords` | `ParseClearDispatcher`, `ClearDispatcher` | `CompileClearDispatcher` | `DelegateNodeDecompile` | `ClearDispatcher` | Full |
| `return` | --- | `ParseReturn`, `ParseReturnVoid`, `InlineCommentStripping.Return` | `ReturnStatement` | `ReturnNode` | `Return` | Full |
| `return (named)` | --- | `ReturnNamedArgs` | `MultiBranchReturnKeepsBothBranches` | `MultiBranchReturn` | `MultiBranchReturn` | Full |
| `return` (one per branch) | --- | --- | `MultiBranchReturnKeepsBothBranches` | `MultiBranchReturn` | `MultiBranchReturn` | Full |
| `return [ExitPin]` | --- | `ReturnExitPinOnly`, `ReturnWithExitPin` | (macro context) | (macro context) | --- | Partial |
| `entry macro` | --- | `MacroEntry`, `MacroEntryBacktickName`, `MacroEntryMultiExit`, `MacroEntryDataAndExec` | (via SetupMacro) | (via decompile_macro) | `MacroSimple`, `MacroExecOnly`, `MacroBacktickName` | Full |
| `exec -> @label` | --- | `ParseExecGoto` | `MultipleSequentialControlFlow` | --- | --- | Partial |
| `comment` | `CommentToken` | `ParseComment` | `CommentInstruction` | --- | --- | Partial |
| external property set | `PercentRefDotPin`, `DollarRefDotProp` | `ParseExternalPropertySet`, `ParseExternalPropertySetDollar`, `SetExternalMultiDotLHS`, `SetExternalSingleDotLHS`, `SetDollarVarDotProp` | --- | `ExternalVariableGetThroughKnot`, `ExternalVariableSetThroughKnot` | --- | Partial |
| authored positions `@(x, y)` | --- | valid positions, quoted `@(`, labels, malformed markers, negative/whitespace variants, duplicate markers | fully positioned bodies, mixed-position rejection, zero-node rejection, helper rejection, per-entry isolation, insertion/body compile paths | exec-backed nodes, pure dependency nodes, transparent knot/reroute skip, current-coordinate suffixes | fully positioned decompile -> compile, generated layout coordinates, helper/zero-node diagnostics, multi-entry strictness | Full |

### 2.2 Entry Types

| Entry Type | Parser | Compiler | Decompiler | Round-Trip | Status |
|------------|--------|----------|------------|------------|--------|
| `event` | (implicit in all) | `SimpleBeginPlay` + many | `SimpleChain` + many | `SimpleCall` + many | Full |
| `custom_event` | `ParseCustomEventEntry` | `CustomEvent`, `BareParamName` | `CustomEvent`, `CustomEventParam` | `CustomEventParam`, `BareParamName`, `CrossEntryCustomEventCall` | Full |
| `function` | `ParseFunctionEntry`, `ParseFunctionEntryWithParams`, `ParseCallWithReturn` | `NewFunction` | `MultipleEntryPoints` | `Function` | Full |
| `macro` | `MacroEntry`, `MacroEntryBacktickName`, `MacroEntryMultiExit`, `MacroEntryDataAndExec` | (via SetupMacro) | (via decompile_macro) | `MacroSimple`, `MacroExecOnly`, `MacroBacktickName` | Full |
| `construction` | `ParseConstructionEntry` | `ConstructionEntry` | --- | --- | Partial |
| `component_event` | `ParseComponentEventEntry` | `ComponentEventEntry` | --- | --- | Partial |
| `widget_event` | `ParseWidgetEventEntry` | `WidgetEventEntry` | --- | --- | Partial |
| `key_pressed` | `ParseKeyPressedEntry` | `KeyPressedEntry` | --- | --- | Partial |
| `key_released` | `ParseKeyReleasedEntry` | `KeyReleasedEntry` | --- | --- | Partial |

### 2.3 Type System

| Type | Tokenizer | Parser | Compiler | Decompiler | Status |
|------|-----------|--------|----------|------------|--------|
| `bool` | --- | --- | `PinResolver.ConvertCppType_Bool` | --- | Partial |
| `int` / `int32` | --- | --- | `PinResolver.ConvertCppType_Int` | --- | Partial |
| `int64` | --- | --- | `PinResolver.ConvertCppType_Int64` | --- | Partial |
| `float` / `double` | `NumberLiterals` | --- | `PinResolver.ConvertCppType_Float`, `PinResolver.ConvertCppType_Double` | --- | Partial |
| `string` | `StringEscape`, `DoubleBackslashBeforeQuote`, `TripleBackslash` | --- | `PinResolver.ConvertCppType_FString` | --- | Partial |
| `FName` | --- | --- | `PinResolver.ConvertCppType_FName` | --- | Partial |
| `FText` | --- | --- | `PinResolver.ConvertCppType_FText`, `PinResolver.SetTextDefaultRequiresIdentity`, `SelectTextOptionLiteral`, `MakeArrayTextElementLiteral` | `FTextLocalization`, `FTextStringTable` | Partial |
| `struct<T>` | --- | --- | `PinResolver.ConvertCppType_FVector`, `PinResolver.ConvertCppType_FTransform` | --- | Partial |
| `object<T>` | --- | `ParseEntryWithObjectParam` (in param) | `CastNode`, `PinResolver.ConvertCppType_Object`, `PinResolver.ConvertCppType_ObjectPointer` | --- | Partial |
| `array<T>` | --- | `ParseMakeArray` | `MakeArray`, `ForeachBreak`, `PinResolver.ConvertCppType_Array` | `MakeArrayTextElementLiteral` | Partial |
| `set<T>` | --- | --- | --- | --- | **Gap** |
| `map<K,V>` | --- | --- | --- | --- | **Gap** |
| `softobject<T>` | --- | --- | `PinResolver.ConvertCppType_SoftObject` | --- | Partial |
| `softclass<T>` | --- | --- | `PinResolver.ConvertCppType_SoftClass` | --- | Partial |
| `interface<T>` | --- | --- | `PinResolver.ConvertCppType_Interface` | --- | Partial |
| `delegate<T>` | --- | --- | `PinResolver.ConvertCppType_Delegate` | --- | Partial |
| `mcdelegate<T>` | --- | --- | `CompileClearDispatcher` (implicit), `PinResolver.ConvertCppType_MCDelegate` | `DelegateNodeDecompile` (implicit) | Partial |
| `wildcard` | --- | --- | `WildcardNoType` (expression-level) | --- | Partial |
| Asset ref literals | `AssetReferencePath` (tokenizer behavior doc) | --- | --- | --- | **Gap** |
| `FVector` literal | --- | --- | `PinResolver.ConvertCppType_FVector` | --- | Partial |
| `FRotator` literal | --- | --- | `PinResolver.ConvertCppType_FRotator` | --- | Partial |
| `FLinearColor` literal | --- | --- | `PinResolver.ConvertCppType_FLinearColor` | --- | Partial |
| class ref | --- | --- | `PinResolver.ConvertCppType_ClassRef` | --- | Partial |
| struct ref | --- | --- | `PinResolver.ConvertCppType_StructRef` | --- | Partial |
| enum ref | --- | --- | `PinResolver.ConvertCppType_EnumRef` | --- | Partial |

### 2.4 Error Handling

| Scenario | Compiler | Status |
|----------|----------|--------|
| Duplicate `%name` | `DuplicateValueName` | Full |
| Duplicate `@label` | `DuplicateLabel` | Full |
| Unknown `%ref` | `UnknownPercentRef` | Full |
| Unknown `@label` | `UnknownLabelRef` | Full |
| Atomic rollback | `AtomicRollback` | Full |
| Malformed entry | `MalformedEntry` | Full |
| Empty block | `EmptyBlock` | Full |
| Unknown function | `InvalidFunction` | Full |
| Forward `%ref` | `ForwardReference` | Full |
| Duplicate entry name | `DuplicateEntryName` | Full |
| Invalid opcode keyword | `InvalidOpcodeKeyword` | Full |
| Mismatched braces | `MismatchedBraces` | Full |
| Rollback preserves existing | `RollbackPreservesExistingNodes` | Full |
| Unrecognized type | `PinResolver.ConvertCppType_Unrecognized` | Full |

### 2.5 Graph Operations

| Operation | Compiler | Status |
|-----------|----------|--------|
| Insert after node | `InsertCodeAfterNode`, `InsertCodeAfterNode_ExecChain` | Full |
| Insert after specific pin | `InsertAfterNode_SpecificPin`, `InsertAfterNode_BadPinName` | Full |
| Insert before node | `InsertBeforeNode`, `InsertBeforeNode_EventNode` | Full |
| Undo compile | `UndoCompile` | Full |
| Multi-entry blocks | `CompileMultiEntryBlocks` | Full |
| External var get (knot) | `ExternalVariableGetThroughKnot` | Full |
| External var set (knot) | `ExternalVariableSetThroughKnot` | Full |

### 2.6 Node Layout Engine

25 unit tests in `TestNodeLayout.cpp` covering the post-compile headless auto-formatting pipeline. All under `PinWright.Bpir.NodeLayout.*` test ID prefix.

| Component | Tests | Coverage |
|-----------|-------|----------|
| EstimateNodeSize | `EstimateSize` | Width/height finite, respects minimums |
| GetNodeBounds | `Bounds` | Bounding rect from node position + estimated size |
| BuildFormatXInfoMap | `FormatX.LinearChain`, `FormatX.Branch`, `FormatX.Diamond` | Linear chain, branch topology, diamond reconvergence |
| FormatX | `FormatX.LinearSpacing`, `FormatX.ClusterBounds`, `FormatX.GridAlignment` | Inter-column spacing, cluster-extended bounds, grid snap |
| FNodeLayoutParameterFormatter | `ParameterFormatter.Basic`, `ParameterFormatter.ConsumerOnly` | Pure-node column layout, consumer-only (no pures) case |
| FormatParameterNodes | `FormatParameterNodes.Empty`, `FormatParameterNodes.SingleConsumer`, `FormatParameterNodes.SharedPure` | Empty pool, single consumer with pures, first-consumer-wins shared-pure rule |
| GetPinsOfSameHeight | `SameRow.Linear`, `SameRow.Branch`, `SameRow.EmptyMap` | Linear chain same-row marking, branch first-child marking, empty-map edge case |
| FormatY | `FormatY.LinearSameRow`, `FormatY.BranchStacking`, `FormatY.CollisionNudge`, `FormatY.ExternalObstacle` | Same-row Y inheritance, sibling stacking, collision avoidance, external obstacle avoidance |
| ResetRelativeToAnchor | `AnchorReset` | Anchor node returns to pre-format position |
| SnapToGrid | `GridSnap`, `DirectionalSnap` | Basic grid alignment, directional rounding (Floor/Ceil/Round) |
| End-to-end | `Engine.EndToEnd` | Full pipeline on a multi-node graph |
| Kill switch | `Engine.KillSwitch` | `bEnableBpirLayoutPass=false` bypasses layout entirely |

### 2.7 Authored-position coverage

`@(x, y)` coverage spans parser, compiler, decompiler, and round-trip tests:

| Layer | Coverage |
|-------|----------|
| Parser | Valid suffixes, negative coordinates, whitespace variants (`@(10,20)` / `@(10, 20)`), quoted strings containing `@(`, BPIR labels like `@foo`, malformed suffixes, multiple markers, and comment-only lines. |
| Compiler | No-position bodies keep auto-layout and implicit helpers; fully positioned bodies preserve primary-node coordinates; mixed primary-node positions fail per entry body; zero-primary-node positions fail; authored-position bodies reject implicit visible helpers; insert/body compile paths share the same layout rules. |
| Decompiler | Exec-backed and pure dependency nodes emit current graph coordinates; transparent knots/reroutes stay omitted; event/custom event, function return, macro/tunnel, branch/switch, delegate, make/break struct, and other explicit node-backed lines use the centralized suffix path. |
| Round-trip | Fully positioned body decompile -> compile, unpositioned body with generated layout coordinates, multi-entry per-body strictness, helper behavior in auto-layout vs authored-position mode, explicit helper BPIR line round-trip where supported, malformed and mixed-position diagnostics. |
| Diagnostics | Manual-placement failures assert both the reason and the remediation: add `@(x, y)` to every visible node-backed instruction, remove all positions to use auto-layout, or make an implicit helper explicit with its own positioned BPIR line. |

Remaining unsupported helper-position cases are intentional v1 scope: implicit visible helpers are allowed only in unpositioned auto-layout bodies. Authored-position bodies must either avoid implicit helpers or represent them with explicit BPIR instructions where the language already supports that helper as a node-backed line.

---

## 3. Known Gaps

Verified by auditing all 13 test files against the BPIR spec feature list.

### Instructions with no or minimal coverage

1. **`make_set`** — No test at any layer (parser, compiler, decompiler, or round-trip). Spec lists as decompiler-only output but no emission path is tested. Needs at minimum a parser test and a decompiler emission test.
2. **`make_map`** — Same as `make_set`. Zero test coverage across all layers.
3. **`call_dispatcher` / `bind_dispatcher`** — Parser tests added (`ParseCallDispatcher`, `ParseBindDispatcher`). Compiler and round-trip tests exist but rely on graceful-failure paths because transient test BPs lack real delegate types. Coverage is fragile; no decompiler test.
4. **`switch` (generic)** — Parser test added (`ParseSwitchGeneric`). Compiler test `SwitchStatement` exists. No decompiler or round-trip test.
5. **`exec -> @label`** — Compiler test `MultipleSequentialControlFlow` exists. No decompiler or round-trip test.
6. **`comment` instruction** — Compiler test `CommentInstruction` added. No decompiler or round-trip test.

### Entry types with compiler-only or parser-only coverage

7. **`construction`** — Parser + compiler test. No decompiler or round-trip test.
8. **`component_event`** — Parser + compiler test (`ComponentEventEntry`). No decompiler or round-trip test.
9. **`widget_event`** — Parser + compiler test (`WidgetEventEntry`). No decompiler or round-trip test.
10. **`key_pressed`** — Parser + compiler test (`KeyPressedEntry`). No decompiler or round-trip test.
11. **`key_released`** — Parser + compiler test (`KeyReleasedEntry`). No decompiler or round-trip test.

### Type system gaps

12. **`set<T>` type** — `ConvertCppTypeToPinType` does not handle `set<T>`. Gap-documenting test added (`ConvertCppType_SetNotImplemented`). Implementation needed in `CodePinResolver.cpp`.
13. **`map<K,V>` type** — `ConvertCppTypeToPinType` does not handle `map<K,V>`. Gap-documenting test added (`ConvertCppType_MapNotImplemented`). Implementation needed in `CodePinResolver.cpp`.
14. **Asset reference literals** (`/Game/Path/To/Asset.Asset`) — Tokenizer behavior documented (`AssetReferencePath`), but no parser, compiler, or decompiler test. Asset refs are not compilable as inline literals; a parser-level test for the literal format is the minimum needed.

### Recently closed gaps

- **Custom event parameter round-trip** — Decompiler now emits `$ParamName` for entry node parameter pins (was `?`). Compiler accepts both `$ParamName` and bare `ParamName`. Tests: `Bpir.Decompiler.CustomEventParam`, `Bpir.RoundTrip.CustomEventParam`, `Bpir.RoundTrip.BareParamName`.
- **Cross-entry custom event calls** — Compiler now supports `call CustomEventName(...)` from another entry block via skeleton recompile. Test: `Bpir.RoundTrip.CrossEntryCustomEventCall`.
- **Forward entry references** — Compiler supports calling a function or custom event declared *later* in the same compile body. Phase 1 creates all entries before Phase 2 emits any bodies; Phase 1.5 skeleton recompile makes them UFunctions-resolvable. Tests: `Bpir.Compiler.ForwardReference.FunctionCallsLaterFunction`, `Bpir.Compiler.ForwardReference.CustomEventCallsLaterCustomEvent`.
- **Pure node fallback name sanitization** — Decompiler strips parentheses from display-name fallback to prevent uncompilable names like `Equal_(Enum)`.
- **Multi-dot set LHS parsing** — Parser now uses `FindLastChar('.')` to split external property set targets, correctly handling `%ref.PinName.Property` (splits at last dot). Tests: `Bpir.Parser.SetExternalMultiDotLHS`, `Bpir.Parser.SetExternalSingleDotLHS`, `Bpir.Parser.SetDollarVarDotProp`.
- **Enum defaults: PC_Byte vs PC_Enum routing** — Compiler branches on `PinCategory` when applying enum literal defaults. `PC_Byte` pins (e.g., `EqualEqual_ByteByte.B`) resolve the enum literal to a **numeric string** (`"2"`); `PC_Enum` pins (e.g., `SetVisibility.InVisibility`) apply the **enum name string** (`"Collapsed"`). Test: `Bpir.Compiler.EnumDefaultOnGenericBytePin` covers `ETraceTypeQuery` on a byte pin; `TestCompilerDispatchersOps` also tests `ESlateVisibility` on PC_Enum pins.
- **Delegate binding with external targets** — `bind_dispatcher` supports a `Target:` arg to bind to dispatchers on external objects. Graceful failure when delegate not found on target class. Test: `Bpir.Compiler.BindDispatcher_ExternalTarget_NoCrash`.
- **`switch_int` case labels across a reconstruction** — `UK2Node_SwitchInteger` stores no case values (the pin *name* is the value) and renumbers its case pins positionally to `StartIndex, StartIndex+1, ...` in `ReallocatePinsDuringReconstruction`, which runs on compile-on-load but not on the authoring compile. The compiler now anchors `StartIndex` to the lowest label, lays the case pins out ascending, and rejects gapped / duplicated / non-canonical label sets. Tests: `Bpir.switch_int.CaseLabelsSurviveReconstruction`, `Bpir.switch_int.OutOfOrderLabelsAreLaidOutAscending`, `Bpir.switch_int.NonContiguousLabelsRejected`. **Why the old coverage missed it:** `CompileSwitchInt` and `round_trip.SwitchInt` both used the failing shape `[1 -> …, 2 -> …]` and asserted only `bSuccess` plus "a SwitchInteger node exists" — neither reconstructed, so neither could see the labels shift. A `switch_int` test that does not reconstruct proves nothing about case identity. Board ticket `B-switch-int-labels-shift-on-load`.

- **Interface message calls (`message Interface::Function(...)`)** — `UK2Node_Message` derives from `UK2Node_CallFunction`, so it decompiled as an indistinguishable plain `call Foo(...)` and recompiled into a plain `UK2Node_CallFunction`: a silent node-class change, and the reason a correct message node looked like a missing one for two rounds on `B-bpir-interface-call-never-dispatches`. `message` is now a keyword on the Call opcode (`FBpirInstruction::bInterfaceMessage`), always class-qualified on emit, refused when the resolved function's owner is not an interface, and its `Target:` survives even when it is `self`. Test: `bpir.round_trip.InterfaceMessage` (compile → node class, decompile → keyword + qualifier + target, recompile → node class, plus the non-interface refusal).
- **Parent function calls (`parent_call Class::Function(...)`)** — `UK2Node_CallParentFunction` now has a dedicated BPIR keyword and compiler lane, preserving its authoritative parent class and enabled-state marker through saved-asset compile → decompile → recompile → reload coverage in `bpir.round_trip.EnabledState`.

### Decompiler/round-trip remaining gaps

15. **`foreach_break`** — No decompiler test (compiler + round-trip exist).
16. **`macro DoOnce` / `macro FlipFlop`** — No decompiler test (compiler + round-trip exist).
17. **`subsystem<T>()`** — Compiler test added (`SubsystemAccess`). Still missing decompiler and round-trip.
18. **`unbind_dispatcher`** — Parser + compiler tests exist. Still missing decompiler and round-trip.
19. **External property access** (`set %ref.Prop`, `set $Param.Prop`) — Parser tests added (`ParseExternalPropertySet`, `ParseExternalPropertySetDollar`). Decompiler-only coverage through knot tests. No compiler or round-trip test.
20. **Format Text** (`Format` node) — Compiler support added via dedicated K2Node handler (aliases: `Format`, `Format_Text`, `FormatText`). Decompiler has `FormatArgsDefaultValues` for default-value formatting. Round-trip test still pending.
21. **`self` decompiler** — No decompiler or round-trip test specifically for `self` references.
22. **`enum` literal decompiler** — Compiler + round-trip exist. No dedicated decompiler test.
23. **`select` with a literal index** — A `select(Index: <literal>)` leaves `UK2Node_Select`'s Index pin at `PC_Wildcard` (a literal creates no connection, and nothing types the pin), so the Blueprint compile fails with "The type of Index is undetermined". Only a variable/parameter index is covered; the option-pin side is covered by `SelectTextOptionLiteral`. Tracked separately from `B-bpir-select-literal-text-lost`.
24. **`make_array` with no type source** — the BPIR compiler types a MakeArray's element pins from a `%name: array<T> =` annotation, or from an element literal carrying a real FText localization identity (`MakeArrayTextElementLiteral`); otherwise the engine types them only when the array output is wired to an already-typed consumer. A bare `make_array("a", "b")` has none of the three, so its pins stay `PC_Wildcard` and the resulting graph is untyped — the same shape as gap 23. No test asserts what the Blueprint compile then does with it; `MakeArray` only asserts the BPIR round-trip.

---

## See also

- [BPIR Language Reference](wiki-src/bpir.md) — instruction syntax, types, sigils
- [BPIR Examples](wiki-src/bpir.examples.md) — code examples with test annotations
- [BPIR Compiler Internals](bpir-compiler-internals.md) — parsing algorithm, design decisions
- [Blueprint wiki insertion sections](wiki-src/blueprint.md#blueprintinsert_bpir_at_node) — mid-flow graph insertion modes
- [Architecture](arch.md) — plugin architecture reference
