---
type: system
summary: "BPIR compiler internals: two-phase compile architecture (Phase 0c stale-UFunction scrub after deletion sweeps, Phase 0-pre unconditional ComponentEvent/WidgetEvent upsert, entry creation, skeleton recompile, emit/wire), post-compile pipeline (full CompileBlueprint → RefreshBpirDelegateNodes → integrity gate), narrow-only integrity-gate checks (no orphan-UFunction heuristic), FBlockSetupState, SwitchToGraph state ordering (EmitMap/MacroExitTunnel restoration after graph switch), ResolveTargetClass (unified ValueResolver delegation), PreEmitVariableRefs, PendingExternalInjections consumption and cleanup pattern (InjectExternalVariable, three-exit cleanup in InsertCodeAfterNode/CompileBodyIntoGraph), function resolution cascade (7 steps, Step 4 handles both %ref and $var targets via ResolveTargetClass), type resolution pipeline (ConvertCppTypeToPinType for compiler vs MakePinType for RPC handlers, Wildcard-pin pitfall, const/ref param modifiers), bSkipWireDataPins, value resolution fallbacks (multi-dot property access, auto-BreakStruct), async action K2Node auto-configuration, delegate binding with external targets, K2Node_CreateDelegate four-step wiring order (AllocateDefaultPins, wire object input, SetFunction, HandleAnyChange), macro graph compilation, expression subgraph compilation, decompiler label allocation for multi-exit nodes (connected-pin-only constraint across Latent/Timeline/DoOnce-Gate-FlipFlop/Unknown paths), authored BPIR node positions, post-compile node layout engine (headless auto-formatting), entry function metadata round-trip (@meta/@flags decorators via EmitEntryDecoratorLines + ParseMetaDecorator/ParseFlagsDecorator), design decisions, error handling with atomic rollback."
date: 2026-06-05
tags: [bpir, compiler, parser, decompiler, internals, integrity-gate, ufunction-scrub, enum-subtype, k2node-classification, switch-enum, type-slot-resolution, cascade-coupling, skel-gen-duality, is-self-context, function-metadata, meta-flags-decorators]
---

# BPIR Compiler Internals

Internal documentation for contributors working on the BPIR compiler and decompiler. For BPIR syntax and usage, see the [Language Reference](wiki-src/bpir.md).

---

## 4. Two-Phase Compile Architecture

The `Compile()` method uses a two-phase architecture to support cross-entry custom event calls (e.g., calling a custom event defined in one entry block from another entry block in the same compile unit).

### Phase 0c — Stale UFunction Scrub After Deletion Sweeps

When Phase 0 of Replace/Append mode deletes `UK2Node_CustomEvent` nodes or function graphs (as part of the signature-conflict upsert or bound-event pre-pass), the deletion unhooks the node from the graph but does **not** remove the corresponding `UFunction` from `GeneratedClass`. The follow-up `FKismetEditorUtilities::CompileBlueprint(..., RegenerateSkeletonOnly)` at Phase 1.5 only rebuilds `SkeletonGeneratedClass` — it explicitly skips `FKismetCompilerContext::CompileClassLayout`, and with it `CleanAndSanitizeClass`, the only engine pass that clears `Class->Children` and `FuncMap`. As a result, orphan UFunctions survive on the generated class and crash `TFieldIterator<UFunction>` on cold reload.

To prevent this, Phase 0c runs `BlueprintHandlerUtils::ScrubStaleUFunctionsFromClass(BP, WipedFunctionNames)` after `CascadeRemoveStaleCreateDelegates`. For each wiped function name the scrub mirrors `FBlueprintEditorUtils::RemoveStaleFunctions` per-function:

1. Unlink the `UFunction` from the `Class->Children` linked list.
2. `Class->RemoveFunctionFromFunctionMap(Func)`.
3. `FLinkerLoad::InvalidateExport(Func)` so on-disk export refs become stale.
4. `Func->Rename(nullptr, GetTransientPackage())` to move it out of the class package.
5. After the per-function pass: `Class->ClearFunctionMapsCaches()` + `Class->Bind()` + `Class->StaticLink(true)`.

The engine's public `FBlueprintEditorUtils::RemoveStaleFunctions` is intentionally **not** used — it wipes all non-native UFunctions including user-authored ones. The scrub is targeted per name collected during the deletion sweep.

### Phase 0-pre — ComponentEvent / WidgetEvent Unconditional Upsert

Before Replace-mode's Phase 0 sweep runs, `Compile()` performs an unconditional pre-pass over `ComponentEvent` and `WidgetEvent` entry blocks. For each such block, the pass scans every `UEdGraph` in `UbergraphPages` looking for a `UK2Node_ComponentBoundEvent` whose `ComponentPropertyName` and `DelegatePropertyName` match the block's target. The name compare is space-stripped and case-insensitive — parity with `CodeNodeEmitter::CreateComponentEventNode`'s resolver (`Private/Compiler/CodeNodeEmitter.cpp:828-908`), so the pre-pass sees the same node the emit phase would create.

When a match is found, the pass deletes the bound-event entry node **and its full exec-reachable subgraph** before Phase 1 creates the replacement. This runs in every compile, regardless of `mode` — bound events are always upserted.

The rationale for diverging from `CustomEvent` (which the default Append mode preserves so a signature conflict can surface as an error) is two-fold:

1. Bound events have a **fixed delegate signature**. There's no user-authored signature to conflict with — the signature is dictated by the multicast delegate. So the "signature conflict" surface that makes `CustomEvent` duplicate-rejection useful doesn't exist.
2. Keeping both the old and new bound-event nodes produces **double-fire handlers** — the delegate broadcasts to every bound `UK2Node_ComponentBoundEvent` handler, and leaving the prior one wired doubles every event callback. This is visible to the user as "my button click runs the handler twice" and is hard to diagnose.

Given those two facts, upsert is the only correct semantic for bound events, which is why this pass is unconditional rather than gated on Replace mode.

**Implementation note:** the scan casts to `UK2Node_ComponentBoundEvent`, **not** `UK2Node_CustomEvent`. `UK2Node_ComponentBoundEvent` derives from `UK2Node_Event`, not `UK2Node_CustomEvent` — see `lessons.md` for a prior bug caused by this.

See the Phase 1 section below for how the new node is then created.

### Phase 1 — Entry Point Creation

Phase 1 iterates all entry blocks and calls `SetupEntryPoint()` for each. The results are cached in a `FBlockSetupState` struct per block:

- `EntryExecPin` — the exec output pin from the entry node
- `GraphContext` — the `UEdGraph*` active when the entry was created
- `EntryNode` — the entry node pointer (cached for pin re-registration in Phase 2)
- `bValid` — whether setup succeeded

### Phase 1.5 — Skeleton Recompile

If any custom events were created during Phase 1, the compiler triggers a `RegenerateSkeletonOnly` compile:

```cpp
FKismetEditorUtilities::CompileBlueprint(TargetBlueprint, EBlueprintCompileOptions::RegenerateSkeletonOnly);
```

This makes newly created custom events visible as `UFunction` objects on `SkeletonGeneratedClass`. Without this step, `call CustomEventName(...)` from another entry block would fail function resolution because the custom event's `UFunction` doesn't exist yet on `GeneratedClass`.

The skeleton recompile patches existing nodes in-place rather than destroying/recreating them, so the cached `EntryNode` pointers in `FBlockSetupState` remain valid.

### Phase 2 — Emit and Wire

Phase 2 iterates all blocks again, performing the standard scan/emit/wire passes for each. Before emitting, it re-registers entry node parameter pins that were lost when `PinResolver->Clear()` was called between blocks. The re-registration is generic — it iterates **all non-exec output pins** on the cached entry node:

```
for Pin in CachedEntryNode->Pins:
    if Pin.Direction == Output && Pin.Category != Exec && !Pin.Name.IsNone():
        PinResolver->RegisterVariable(Pin.Name, Pin)
```

This covers custom event params, function params, builtin event params (e.g., `DeltaSeconds` on `Tick`), component event params (e.g., `OtherActor` on `OnComponentBeginOverlap`), and macro input params — without requiring type-specific code for each entry kind.

### Function Resolution with SkeletonGeneratedClass

During Phase 2, the function resolution cascade (section 6) includes an additional step between self-class lookup and library search: if the initial `GeneratedClass` lookup fails and `CreatedCustomEvents.Num() > 0`, the compiler checks `SkeletonGeneratedClass`:

```
if (!Func && CreatedCustomEvents.Num() > 0 && TargetBlueprint->SkeletonGeneratedClass)
    Func = FunctionResolver->ResolveFunction(SkeletonGeneratedClass, FunctionName);
```

This is gated on `CreatedCustomEvents.Num() > 0` to avoid unnecessary `FindFunctionByName` walks when no custom events exist.

### ResolveTargetClass — Unified Reference Resolution

`ResolveTargetClass(TargetRef, Block)` determines the UClass for any target reference string. It delegates entirely to `ValueResolver->ResolveValue()`, which handles all reference formats: `$varName`, `%refName`, `%refName.PinName`, and chained property access. This means any new reference format supported by `ValueResolver` automatically works for external set targets and delegate binding targets without changes to `ResolveTargetClass` itself.

The method resolves the target pin, then extracts `UClass*` from `PinType.PinSubCategoryObject`. Returns `nullptr` with an error log if the reference is unresolvable or the pin type is not an object class.

### PreEmitVariableRefs

`PreEmitVariableRefs()` is a shared method that scans block instructions for `$var`, `self`, and external property references (`set %ref.Prop`, `set $Param.Prop`), pre-emitting the corresponding VariableGet/Self/ExternalGet nodes before the main emit loop. It is used by `Compile()`, `InsertCodeAfterNode()`, and `CompileBodyIntoGraph()`.

For `$Param.Prop`, the pre-emit path handles both object-typed and struct-typed targets. Object targets still emit an external `VariableGet`; struct targets delegate to `ResolveStructMemberThroughPin`, which auto-inserts `BreakStruct` / native-break nodes and caches the member pin under the same `ExternalGetCache` key shape.

### `$name.a.b.c` chained access — unified with `%ref.Pin.Prop`

Multi-segment chained property access on struct-typed entry/local refs (`$name.a.b.c`) compiles by reusing the same helper that already walks `%ref.Pin.Prop.Q` chains. The fix unified both paths through a shared `ResolveChainFromPin` helper extracted from the inner loop of `ResolveChainedPropertyAccess`:

- `FBpirCompiler::PreEmitVariableRefs` (`BpirCompiler.cpp:~1974`) still splits `$name.rest` on the first dot only. It passes `rest` (possibly dotted) as a single `Property` string to `FBpirValueResolver::PreEmitExternalGet`. The BPIR source itself is not rewritten.
- `FBpirValueResolver::PreEmitExternalGet` (`BpirValueResolver.cpp:~575`) detects a multi-segment `Property`, splits it on `.`, resolves segment-0 against the target pin as before (struct via `ResolveStructMemberThroughPin`, object via the external-variable-get path), then folds the remainder through `ResolveChainFromPin(StartPin, RemainingChain)`.
- `ResolveChainFromPin` (`BpirValueResolver.cpp:~801`) is the shared helper. It now serves both the `$name.a.b.c` path and the existing `%ref.Pin.Prop.Q` path. Behavior on the `%ref` path is unchanged — the extraction is pure.
- `ExternalGetCache` is keyed on the full dotted `Property` string (matching the lookup already done in `ResolveValue` at ~line 59), so cached pins are reused across duplicate `$name.a.b.c` references in the same compile.

Pinned by `FBpirDollar_ChainedStructFieldAccess_Test` in `TestBpirDollarStructFieldAccess.cpp`.

### PendingExternalInjections — Consumption and Cleanup

`PendingExternalInjections` is a `TMap<FString, UEdGraphPin*>` that holds externally-provided pin references injected via `InjectExternalVariable()`. Before the emit phase, each pending injection is consumed by calling `ValueResolver->InjectCachedVariable(Key, Pin)`, which registers the pin as a `$VarName` in the value resolver.

The injection consumption pattern (for-each over `PendingExternalInjections` calling `InjectCachedVariable`) appears in both `InsertCodeAfterNode()` and `CompileBodyIntoGraph()`. After consumption, `PendingExternalInjections.Empty()` must be called on **all exit paths** — parse failure, rollback (accumulated errors), and success. Without this cleanup, a latent double-injection bug occurs when reusing a compiler instance: leftover entries from a failed call are re-consumed on the next call, injecting stale pin references into an unrelated compilation.

`CompileBodyIntoGraph()` has the same three-exit cleanup pattern. The `Compile()` method does not use `PendingExternalInjections` directly — its external variable injection goes through `FBpirSubgraphCompiler` which calls `InjectExternalVariable()` per input declaration.

### Post-Compile Pipeline — Full Compile, Delegate Refresh, Integrity Gate

After all Phase-2 emit/wire work succeeds, `BpirCompilerHandler.cpp` runs a fixed three-step tail on every BPIR entry point (`blueprint.compile_bpir`, `blueprint.insert_bpir_at_node`, `blueprint.insert_bpir_before_node`). The ordering is load-bearing — steps depend on artifacts produced by the previous step:

1. **Full compile** — `FKismetEditorUtilities::CompileBlueprint(BP, EBlueprintCompileOptions::None)`. This runs `FKismetCompilerContext::CompileClassLayout`, which invokes `CleanAndSanitizeClass` and rebuilds both `SkeletonGeneratedClass` and `GeneratedClass` from scratch. UFunction layout becomes authoritative; any canonical GUID shifts on pre-existing delegate targets settle here.

2. **Delegate node GUID refresh** — `BlueprintHandlerUtils::RefreshBpirDelegateNodes(BP, Result.CreatedNodeGUIDs)`. This iterates **every** `UK2Node_CreateDelegate` in the BP — not just the nodes created in this transaction — because the full compile at step 1 can shift canonical function GUIDs on pre-existing delegates that referenced functions in the same class. For each delegate node the refresh invalidates `SelectedFunctionGuid` and re-runs `HandleAnyChange` against the now-final class, forcing the node to re-resolve `SelectedFunctionName` against the settled UFunction layout.

3. **Integrity gate** — `ValidateBlueprintGraphIntegrity(BP)`. See below.

This tail runs once per successful BPIR compile. If the full compile reports BP-level errors, those are surfaced via `UBlueprint::Status` and `FCompilerResultsLog`; BPIR compilation itself having emitted successfully does not mask them.

### Integrity Gate — Narrow Sanity Checks Only

`ValidateBlueprintGraphIntegrity` asserts four classes of invariant that the engine would never legitimately emit:

| Check | What it catches |
|-------|-----------------|
| `UK2Node_CreateDelegate::IsValid` + canonical-GUID equality | Stale `SelectedFunctionGuid` / dangling function reference |
| `UK2Node_ComponentBoundEvent` component + delegate property resolution | Missing component variable or renamed delegate property |
| Pin `LinkedTo` null / cross-graph walk | Dangling pin links, cross-graph pin connections |
| `Class->Children` null-UFunction + invalid-FName check | Directly corrupt UFunction entries on the class |

The gate **does NOT** check whether each UFunction on the class has a backing graph / custom-event / parent / interface. That speculative heuristic was removed because the engine synthesizes UFunctions on the class for many node types, and enumerating every legitimate pattern from outside the engine is unsustainable. Known examples include:

- `UK2Node_AsyncAction` proxy callbacks — `BeforePush_<NodeGuid>`, `AfterPush_<GUID>`, `OnResult_<GUID>`, etc. (one UFunction per exec-output delegate pin).
- `UK2Node_ComponentBoundEvent` delegate signatures — `BndEvt__*__DelegateSignature`.
- The main ubergraph entry — `ExecuteUbergraph_<BPName>`.
- Input event signatures — `InpActEvt_<ActionName>`, `InpAxisEvt_<AxisName>`.
- Timeline update / finished callbacks.

Every new K2Node that synthesizes class-layer metadata would need to be added to a whitelist — a losing fight from outside the engine. The plugin instead trusts UE's own `FKismetEditorUtilities::CompileBlueprint` status for class-layer validity, and corruption prevention lives at the source (Phase 0c scrub + delegate-node GUID refresh) rather than in a post-hoc detector. This simplification was forced by a false positive on async-action proxy callbacks, which are legitimate engine-generated UFunctions.

---

## 5. Parsing Algorithm

### Line-by-line dispatch

```
for each line:
    strip whitespace
    skip blank / comment-only lines

    match first token:
        'entry'              -> parse entry declaration
        '}'                  -> end current block
        '@' + name + ':'     -> label definition
        '%' + name + '='     -> node declaration with named output
        'set'                -> variable set
        'return'             -> return instruction
        'end'                -> node-less exec-chain terminator
        'call'               -> void impure call
        'latent'             -> void latent call (rare — usually has %name)
        'exec'               -> explicit exec wiring
        'call_dispatcher'    -> dispatcher call
        'bind_dispatcher'    -> dispatcher bind
        'unbind_dispatcher'  -> dispatcher unbind
        'clear_dispatcher'   -> dispatcher clear (unbind all)
        'switch_int'         -> switch on integer
        'switch_string'      -> switch on string
        'switch_enum'        -> switch on typed enum
        '#'                  -> comment
        else                 -> error
```

### Three-pass compilation

**Pass 1 — Scan:** Parse lines into instruction objects. Build `@label` → line index table. Build `%name` → instruction table. Validate all references exist.

**Pass 2 — Emit:** Create K2Nodes for node-backed instructions and store node pointers in the value table. Structural `end` emits no node and clears the placement pass's carried exec pin.

**Pass 3 — Wire:** Resolve `%name` / `$var` / literal references → wire data pins. Resolve `@label` references → wire exec pins. Auto-chain sequential impure nodes within each label block; `end` clears `LastExecOutput` without creating a wire.

### Exec auto-chaining rules

Within a label block (between `@label:` markers), impure nodes auto-chain: each impure node's default exec output connects to the next impure node's exec input. Explicit `exec -> @label` or `[pin -> @label]` overrides this.

Cross-label fall-through: a label block that does NOT end in a terminator falls through to the **next adjacent label** — the block's trailing exec output auto-chains into the first impure node of the following segment. This is the documented `bpir.instructions` §2.8 "auto-chain to next label" idiom (`exec -> @label` is only required when jumping to a NON-adjacent label). `WireExecPins` Step 2 carries each segment's trailing `LastExecOutput` through `FallThroughExec` to emit this edge.

Fall-through does NOT happen (the trailing exec output is null, so no edge is emitted) when the preceding block ends in a terminator:
- After a node with `[exec_clause]` (multi-output branch/switch nodes handle their own wiring)
- After an explicit `exec -> @label` / `[pin -> @label]` jump
- After bare `end`, a node-less marker for a disconnected exec output
- After `return`

---

## 6. Function Resolution Cascade

When the compiler encounters a `call` or `pure` instruction, it must resolve the function name to a `UFunction*`. The resolution follows a cascade, stopping at the first match.

### Step 0: Qualified `ClassName::MethodName` Short-Circuit

When the parser observed a `ClassName::MethodName` form, the class is stashed in `FBpirInstruction::TypeArg` and the method in `FunctionName`. The compiler resolves `TypeArg` via `ResolveUClass` and looks up the method on that class (plus `SkeletonGeneratedClass` mirroring Step 1b for BP-generated classes). If either lookup fails, compile errors out — an explicit `Class::` qualification is an explicit demand and never falls through to other cascade steps. When the qualified path succeeds, all subsequent steps are skipped.

### Step 4 (when `Target:` is explicit): Target-Type-Aware Lookup First

When the unqualified call has an explicit `Target:` argument and the target's class can be extracted from a `%ref` pin type (with `BpirDeclaredReturnTypes` fallback for wildcard `ReturnValue` pins) or via `ResolveTargetClass` for `$var` targets, the compiler runs the target-class lookup **before** Step 1. This mirrors UE's BP node-creation scoping: the source-pin's type fixes the function-picker scope, so a method that matches the wired target's class wins over a same-named method on the self-class hierarchy (e.g. `URaceAnalyzerSubsystem::SetPlaybackSpeed(float)` over `UUserWidget::SetPlaybackSpeed(UWidgetAnimation*, float)` when the BPIR is compiled in a Widget Blueprint). Self-class is reached only when the target-class lookup returns null.

The cascade below describes the fallback chain that runs when Step 0 doesn't apply and either no `Target:` is present or the target-class lookup returned null.

### Step 1: Self-Class Lookup

Search the Blueprint's generated class (and its parent chain) for a function matching the name. This catches member functions, inherited functions, and functions defined in the BP itself.

### Step 1b: Skeleton Class Lookup (Cross-Entry Custom Events)

If step 1 fails and custom events were created during Phase 1 of the two-phase compile (see section 4), search `SkeletonGeneratedClass` for a matching function. This is how `call CustomEventName(...)` resolves to a custom event defined in another entry block within the same compile unit. Gated on `CreatedCustomEvents.Num() > 0` to avoid unnecessary function walks.

### Step 2: Blueprint Function Library Scan

Iterate all loaded `UBlueprintFunctionLibrary` subclasses and search their static functions. This is how global utilities like `PrintString`, `MakeVector`, `Conv_IntToText`, etc. are found.

### Step 3: Broad Search (Non-Library Static Functions)

If steps 1–2 fail, perform a broad search across **all** loaded classes for static `BlueprintCallable` functions that are NOT on `UBlueprintFunctionLibrary` subclasses. This catches functions like `UGameplayStatics::GetGameInstance`, `UWidgetLayoutLibrary::GetMousePositionOnViewport`, and other static functions on non-library classes that take a WorldContext parameter.

The broad search uses a **lazy `TMap<FString, UFunction*>` cache** (`BroadFunctionCache`) built on first miss. The cache maps lowercase function names to `UFunction*` for all functions matching `FUNC_Static | FUNC_BlueprintCallable` (using `HasAllFunctionFlags`, not `HasAnyFunctionFlags` — the distinction matters because `HasAnyFunctionFlags` is OR semantics while `HasAllFunctionFlags` requires both flags set). The cache is built once per compilation session and reused for subsequent lookups.

### Step 4: Target-Type-Aware Lookup (fallback position)

This is the same logic described above as "Step 4 (when `Target:` is explicit): Target-Type-Aware Lookup First" — it is the SAME implementation, just reached as a fallback only when `Target:` extraction returned null on the first attempt (e.g. target arg missing, or the source pin had no class type and `BpirDeclaredReturnTypes` had no entry). In practice, when `Target:` is present and resolves, this step runs ahead of Step 1; when it doesn't resolve, control falls into Step 1 → Step 1b → Step 2 → Step 3 → Step 6 in order.

The two target forms remain:

- **`%ref` target** — The target references a named node output (e.g., `%ss` from `subsystem<MusicPlayerSubsystem>()`). The resolver reads the pin type from the node's output pin and searches that class.
- **`$var` target** — The target is a blueprint variable (e.g., `$MyDoorRef`). The resolver calls `ResolveTargetClass` to determine the variable's type from the blueprint's property list, then searches that class.

This is how `call NextTrack(Target: %ss)` resolves to `UMusicPlayerSubsystem::NextTrack`, and equally how `call Open(Target: $DoorRef)` resolves to a method on the door actor's class.

### Step 5: FormatText K2Node Handler

If the function name matches `Format`, `Format_Text`, or `FormatText` (case-insensitive), the compiler dispatches to a dedicated handler that creates a `UK2Node_FormatText` node instead of a `UK2Node_CallFunction`. See [bSkipWireDataPins](#6b-bskipwiredatapins-pattern) below.

### Step 6: K2Node Fallback

Search for K2Node classes by name pattern (e.g., `UK2Node_<FunctionName>`). This catches specialized nodes like `UK2Node_SpawnActorFromClass`.

If all 7 steps fail, the compiler reports an error with diagnostic context: what class was searched, which steps were attempted, and suggestions for fixing the issue (e.g., "check that the function is BlueprintCallable").

### Cascade ↔ Decompiler Probe Coupling (Load-Bearing)

The decompiler's qualification probe `ShouldQualifyFunctionName` (`Private/Decompiler/BpirTextEmitter.cpp`) re-implements the cascade above to decide when a call site must emit `Class::Method` form instead of bare `Method` on round-trip. The probe walks the same step ordering — target-class-aware lookup, self-class, skeleton class, library scan, broad-static — and concludes "qualify" only when an unqualified emission would resolve to a different `UFunction` on recompile than the one currently bound to the node.

This is a deliberate duplication of resolution logic across two files rather than a shared policy module. The full shared-policy extraction was considered and rejected as over-engineering: the cascade is small enough that mirrored code is cheaper than the abstraction, and a bug in a shared policy module would silently break both sides simultaneously. Mirrored code at least fails loudly on round-trip when the two drift.

To keep the two in lockstep, both sites carry bidirectional `CASCADE-COUPLING NOTE` comments: the cascade body in `BpirCompiler.cpp` (above the Step 0 qualified short-circuit) points at the decompiler probe by file path; the probe `ShouldQualifyFunctionName` in `BpirTextEmitter.cpp` points back at the compiler cascade. Any change to step ordering or the addition of a new cascade step **must** update both sites in the same change. The symptom of desync is silent: round-trip emits either over- or under-qualified call lines that recompile to a different function than the original Blueprint, and existing round-trip tests miss it whenever both sides drop the same arm symmetrically (see the "symmetric loss" lesson in [lessons.md](lessons.md)).

### SKEL/GEN UFunction Duality and the `IsSelfContext` Invariant

The engine maintains **parallel `UFunction` copies** on `SkeletonGeneratedClass` and `GeneratedClass` for the same logical Blueprint function. The two pointers are never `==`-equal even though they describe the same logical method. This duality is the root of a class of decompiler emission bugs that look like compiler-side state corruption but live entirely in `ShouldQualifyFunctionName`.

Key engine-side facts:

- `UK2Node_CallFunction::GetTargetFunction()` resolves through `SkeletonGeneratedClass` because `UK2Node::GetBlueprintClassFromNode()` prefers `SkeletonGeneratedClass` over `GeneratedClass`. So nodes mid-edit (and many nodes post-compile) return a `UFunction*` whose `GetOuterUClass()` is the skeleton class.
- `BPClass->FindFunctionByName(Name)` on `GeneratedClass` returns a **different `UFunction*`** that points at the same logical method on the generated class.
- `FMemberReference::SetGivenSelfScope` zeroes `MemberParent` for self calls regardless of whether the SKEL or GEN `UFunction` was passed to `SetFromFunction`. The `ClassGeneratedBy` equality clause in `SetGivenSelfScope` (`MemberReference.cpp`) catches both class pointers, so any persisted-state-side fix attempting to "prefer GenFunc over SkelFunc" on the node is structurally inert — the persisted `MemberParent` is already null in both cases.

The decompiler probe's qualification decision must therefore not be driven by `UFunction*` pointer equality between `Node->GetTargetFunction()` and `BPClass->FindFunctionByName(Name)` — that comparison is always unequal for self calls (one returns the skel copy, the other the gen copy), which previously caused `ShouldQualifyFunctionName` to mis-fire and emit bogus `SKEL_<Class>::Method()` qualifiers on self custom-event calls.

The correct guard is an `if (Node->FunctionReference.IsSelfContext()) return false;` short-circuit at the top of `ShouldQualifyFunctionName`. `FMemberReference::IsSelfContext()` is the canonical engine-side answer to "is this a call on self?", driven by the persisted `MemberParent == nullptr` check that `SetGivenSelfScope` establishes. BPIR never qualifies self calls — there is no need to probe pointer equality for self-context call sites.

**Decompile-time bug vs persisted-state bug.** This is a decompile-time *emission* issue: the persisted node state is correct, the GEN/SKEL pointer mismatch is a property of the live engine class layout, and the fix lives in the probe at emission time. Do **not** conflate it with the persisted-skeleton-pointer pathology described in `B-bp-saved-state-corruption-mcp-edits` for `K2Node_CreateDelegate` — that ticket is about delegate-node state surviving across compiles with a stale SKEL pointer baked in. The two have similar symptoms (`SKEL_` strings reaching the user) but disjoint root causes; a "prefer GenFunc over SkelFunc when binding a node" patch in `BpirCompiler.cpp` does not fix the decompiler emission and was reverted at `BpirCompiler.cpp:5293-5308`.

Pinned by `B-bpir-decompile-emits-skel-class-prefix-on-self-event-calls.md`.

---

## 6b. bSkipWireDataPins Pattern

Some K2Nodes create their pins dynamically at runtime (not from a `UFunction` signature). For these nodes, the standard `WireDataPins` pass cannot find pins by name because `FindPin(const TCHAR*)` internally calls `FName(InPinName, FNAME_Find)`, which returns `NAME_None` if the FName string hasn't been registered yet. Dynamically created pins may use FNames that don't exist in the global name table until the node allocates them.

The `bSkipWireDataPins` flag on `FEmittedNode` tells the compiler to skip the standard data pin wiring pass for that node. Instead, the node's `EmitInstruction` handler is responsible for wiring all pins itself, typically by iterating `Node->Pins` directly and matching by display name or pin category rather than using `FindPin`.

**Current users:**
- **FormatText** (`UK2Node_FormatText`) — The format string creates wildcard argument pins named after `{placeholder}` patterns. These pin names are not registered as FNames until the node's `PinNames` array is populated. The FormatText handler wires the format string first (to trigger pin creation), then iterates `Node->Pins` to match argument names.

This pattern should be used for any future K2Node integration where pins are dynamically generated and cannot be found via `FindPin(const TCHAR*)`.

---

## 6c. Value Resolution Fallbacks (Multi-Dot and Auto-BreakStruct)

When Pass 3 (Wire) resolves a `%ref.PinName` reference, `ResolvePercentRefPin` first attempts a direct pin lookup on the source node. If the pin is not found, two fallback paths activate:

### Fallback 1: Multi-dot chained property access

For references like `%ref.Pin.Property` or `%ref.Pin.Struct.Member`, the resolver splits `PinName` on dots and walks the chain iteratively:

1. **Resolve first segment** — Find the pin matching the first dot-segment on the source node (e.g., `AsMyClass` on a cast node).
2. **Walk remaining segments** — For each subsequent segment:
   - If the current pin is an **object or interface type**, create an `ExternalVariableGet` node (property access on the object) and advance to its output pin.
   - If the current pin is a **struct type**, create a `BreakStruct` node and find the member pin by name.
3. **Arbitrary depth** — The chain can be any length: `%ref.CastOutput.StructProp.X` resolves through a VariableGet for the object property, then a BreakStruct for the struct member.

### Fallback 2: Struct ReturnValue auto-BreakStruct

When a single-segment `PinName` (no dots) doesn't match any pin on the source node, but the node has a struct-typed `ReturnValue` pin, the resolver auto-inserts a `BreakStruct` and looks for the member on the broken struct. This enables `%v.X` where `%v` is a function returning `FVector` — the compiler auto-inserts `Break Vector` and wires to its `X` output.

### Shared infrastructure

Both fallback paths use `ResolveStructMemberThroughPin` to handle BreakStruct creation. This helper checks the `AutoBreakStructCache` (keyed by source struct pin) to reuse existing BreakStruct nodes — multiple references to different members of the same struct share one BreakStruct node. Multi-dot chains use `AutoPropertyGetCache` (keyed by node pointer + pin path string) to cache intermediate VariableGet nodes.

`FindOutputPinByName` does two-pass lookup: exact case-insensitive match first, then space-normalized fallback (e.g., `ArrayElement` matches `Array Element`).

Both caches are cleared in `SetGraph()` when the compiler switches to a new graph context.

---

## 6d. Async Action K2Node Factory Identity

`UK2Node_AsyncAction` is a generic K2 node that must be configured with a specific async factory function before pins are allocated. BPIR represents that configured identity as:

```bpir
call K2Node_AsyncAction_<FactoryFunctionName>(...)
```

Two lanes share the entry-point `FCodeNodeEmitter::CreateAsyncActionNode(FactoryFunc, ExecPin, NodeClass=nullptr)`:

- **Generic lane** (`NodeClass == nullptr`): owner class lacks `HasDedicatedAsyncNode` meta. The node is spawned as the bare `UK2Node_AsyncAction` base; `InitializeProxyFromFunction` populates `ProxyFactoryFunctionName` / `ProxyFactoryClass` / `ProxyClass` so the base's `AllocateDefaultPins` produces correct delegate exec outputs from the factory's static delegate signature.
- **Dedicated-subclass lane** (`NodeClass != nullptr`): owner class carries `HasDedicatedAsyncNode` meta and a matching `UK2Node_AsyncAction_<FactoryFunctionName>` subclass exists (e.g. `UAsyncAction_ListenForGameplayMessage` → `UK2Node_AsyncAction_ListenForGameplayMessages`). The node is spawned as the dedicated subclass so its `AllocateDefaultPins` override materializes dynamic pins (e.g. `Payload` whose type depends on a `PayloadType` argument). `InitializeProxyFromFunction` is still the contract entry-point — the dedicated subclasses inherit it from `UK2Node_BaseAsyncTask`.

The dedicated subclass is selected by `FCodeNodeEmitter::ResolveDedicatedAsyncActionSubclass(UFunction*)`, a sibling probe to `IsAsyncActionFactory`. It is invoked *before* the generic-lane predicate at all three call sites in `BpirCompiler.cpp` (named-factory dispatch, explicit-K2-class path, shared lambda). Resolution is hybrid: `FindFirstObjectSafe<UClass>` by Epic's naming convention first (O(1)), then `TObjectIterator<UClass>` filtered to `UK2Node_AsyncAction` subclasses (O(n)) as a fallback for plugins that don't follow the convention exactly.

`IsAsyncActionFactory`'s `HasDedicatedAsyncNode` opt-out remains load-bearing for the generic lane — it ensures dedicated-subclass factories never spawn a bare `UK2Node_AsyncAction`. The explicit-K2-class branch in `EmitGenericK2NodeInstruction` recovers the factory rejected by that opt-out via a direct `UBlueprintAsyncActionBase`-filtered iterator.

Compile path:

1. Strip `K2Node_AsyncAction_`.
2. Resolve the factory: try `ResolveAsyncActionFactoryByName` (generic-lane) first; if that fails because of the dedicated-node opt-out, walk `UBlueprintAsyncActionBase` subclasses directly.
3. If no match exists, fail with a deterministic error.
4. If multiple factories have the same function name, fail with a deterministic ambiguity list.
5. Probe `ResolveDedicatedAsyncActionSubclass(FactoryFunc)`; route to `CreateAsyncActionNode` with the resolved `UClass*` (or nullptr for the generic lane).
6. `CreateAsyncActionNode` calls `UK2Node_AsyncAction::InitializeProxyFromFunction(UFunction*)` before `AllocateDefaultPins()` on whichever subclass was spawned.

Bare `call K2Node_AsyncAction(...)` is rejected with a migration message. The compiler no longer infers factories from argument pin names because pin sets can collide and hidden/default pins make the heuristic unstable across engine or plugin changes.

Decompile path reads `UK2Node_BaseAsyncTask::GetFactoryFunction()` and emits the exact `K2Node_AsyncAction_<FactoryFunctionName>` identity. Configured async actions do not go through the unknown-node fallback.

---

## 6e. Type Resolution Pipeline

Pin types are constructed in two separate converters used in different call paths. Understanding which converter is active prevents silent type mismatches.

### ConvertCppTypeToPinType (CodePinResolver.cpp)

Used by the **BPIR compiler** when setting up custom event parameters (`SetupCustomEvent`) and when resolving pin types during `compile_bpir`. Accepts **BPIR shorthand type names** (`string`, `name`, `text`, `int`, `float`, `bool`, `vector`, `rotator`, `transform`, `object`, `class`, `enum`, `struct`, plus array/map/set wrappers) and C++ primitive names. For UE 5.x, `float` maps to `PC_Real + SubCategoryObject = float` (not the legacy `PC_Float`).

### MakePinType (BlueprintHandlerUtils.cpp)

Used by **RPC handlers** that directly manipulate Blueprint graph pins (e.g., `blueprint_add_event`, `blueprint_add_function`). Accepts C++ class names (`FString`, `FName`, `FText`, etc.) as well as short names. In its original form this converter used legacy `PC_Float` instead of the UE 5.x `PC_Real` + subcategory form; this was corrected when the type mismatch was discovered.

### Why two converters matter

If you add a new type to one converter but not the other, the type works in one path and silently falls back to `PC_Wildcard` (Wildcard pins) in the other. **Wildcard pins are dropped by `ReconstructNode`** — the Blueprint compiler silently removes them, making the pin disappear without any compile error. Always update both converters when adding type support. See the [lessons page](lessons.md) for the corrective rule.

### Angle-bracket forms are compiler-only; NormalizeParamType bridges the gap

`ConvertCppTypeToPinType` recognizes angle-bracket type forms (`enum<T>`, `struct<T>`, `object<T>`, `class<T>`, `interface<T>`, `softobject<T>`, `softclass<T>`) via `ParseAngleBracketType`. `MakePinType` has **no** angle-bracket handling — it only resolves bare identifiers plus a fixed primitive table, relying on `FindFirstObjectSafe<UEnum>` / `ResolveClassByName` as fallbacks for bare names.

Because entry-signature params (function inputs, return types, custom-event params, macro I/O) go through `MakePinType`, an `enum<EMyEnum>` entry-signature type would silently resolve to `PC_Wildcard`. The decompiler, however, always emits the `enum<T>` form (consistent with body syntax). The bridge is `BpirParser.cpp::NormalizeParamType`, which rewrites entry-signature `enum<T>` and `enum T` to bare `T` before `MakePinType` runs — letting the `FindFirstObjectSafe<UEnum>` fallback resolve it.

When adding a new angle-bracket type keyword, either (a) add the same angle-bracket branch to `MakePinType`, or (b) extend `NormalizeParamType` to rewrite the new form to something `MakePinType` already accepts. See `call("bpir.types")` for the user-facing statement of this invariant.

---

## 6f. Delegate Binding with External Targets

The dispatcher instruction handlers (`bind_dispatcher`, `unbind_dispatcher`, `call_dispatcher`, `clear_dispatcher`) create delegate nodes (`UK2Node_AssignDelegate`, `UK2Node_RemoveDelegate`, `UK2Node_CallDelegate`, `UK2Node_ClearDelegate`). By default they search for the delegate property on `TargetBlueprint->GeneratedClass` (self context).

The `bind_dispatcher` and `unbind_dispatcher` handlers now support external targets via a `Target` argument. The handler scans `Inst.Args` for an argument named `Target` (case-insensitive). If found, it calls `ResolveTargetClass(Arg.Value, Block)` to determine the external class, then uses `SearchClass->FindPropertyByName(DelegateName)` on the resolved class instead of self. The `bSelfContext` flag is set to `false` when an external target is used, which affects how `SetFromProperty` configures the delegate node's context pin.

This leverages the unified `ResolveTargetClass` (section 4), so any reference format works: `$varName`, `%refName`, `%refName.PinName`, or multi-dot chains. If the external target class does not have the named delegate property, the handler logs a warning and returns `false` (graceful failure), which triggers atomic rollback.

### K2Node_CreateDelegate Wiring Order

`bind_dispatcher` emits a `UK2Node_CreateDelegate` whose `SelectedFunctionName` must resolve to a real `FunctionReference` at BP compile time — not just a stored string. `UK2Node_CreateDelegate::HandleAnyChange()` is what performs that resolution: it reads the currently-wired object input pin, builds a scope from that pin's class, and looks up `SelectedFunctionName` in the scope to populate the internal `FunctionReference`.

This imposes a strict wiring order for the compiler:

1. `AllocateDefaultPins()` — creates `self` (object input), `Function` (delegate output), etc.
2. **Wire the object input pin** — connect either the `Self` node (self-context bind) or the external target pin (`Target: %ref` / `Target: $var`).
3. `SetFunction(FunctionName)` — stores the function name string.
4. `HandleAnyChange()` — **required**. With the object pin now wired, this call walks the scope and resolves the stored name into a real `FunctionReference`.

If `HandleAnyChange()` is skipped (or called before the object pin is wired), the node keeps only the name string. BP compilation then fails with `"Unable to find the selected function/event"` even though BPIR compilation reported success. This was a real bug in the `bind_dispatcher` handler: the function name was set without a post-wiring `HandleAnyChange()` call, so delegate binding silently produced uncompilable graphs. The same pattern applies any time a `K2Node_CreateDelegate` is constructed programmatically — the four steps above must run in order.

---

## 6g. PC_Byte Enum Subtype Propagation

`PC_Byte` pins on byte-comparison functions like `EqualEqual_ByteByte` / `NotEqual_ByteByte` acquire their enum subtype (`PinSubCategoryObject`) only **after** a link is made. UE propagates the subtype from the linked source pin during `NotifyPinConnectionListChanged`. At BPIR compile time, when `CodePinResolver::SetPinDefaultValue` runs on a literal like `EDoorState::Closed`, the pin's `PinSubCategoryObject` is still null because no source pin has been wired yet.

For enum-qualified literals (`Enum::Value` form), the resolver must therefore infer the enum from the literal itself and write the **name string** (e.g. `"NotAvailable"`), not the numeric index. After the link is later made, UE's post-link validator inherits the enum subtype on the pin and rejects numeric values — only the name form survives the validation pass.

The fix in `Private/Compiler/CodePinResolver.cpp` removes the `!bHadOriginalEnumSubtype` short-circuit on enum-qualified literals: the enum-aware branch always runs when the literal has the `Enum::Value` shape, regardless of whether the pin already carries a subtype. The numeric-fallback branch survives only as last-resort for truly untyped byte pins (rare in practice).

## 6h. Decompile Recognizers and K2Node Classification

Several engine K2Node classes that represent function-call semantics do **not** derive from `UK2Node_CallFunction` and are therefore missed by the generic CallFunction recognizer in `BpirTextEmitter::EmitPureNode`. The default fall-through emits a synthetic title-based identifier (e.g. `Get_<TypeName>` built from `GetNodeTitle`) that the BPIR compiler cannot parse.

Subsystem getters are the canonical example: `UK2Node_GetSubsystem` and its three subclasses (`UK2Node_GetSubsystemFromPC`, `UK2Node_GetEngineSubsystem`, `UK2Node_GetEditorSubsystem`) all derive directly from `UK2Node` and require an explicit recognizer that casts to the `UK2Node_GetSubsystem` base (the base catches all four subclasses). The recognizer emits the documented `subsystem<T>()` keyword form documented in `call("bpir.instructions")` (subsystem opcode).

**Implementation gotcha:** `UK2Node_GetSubsystem::CustomClass` is a `protected` UPROPERTY on the base class. To read the configured subsystem type from outside the class, walk the result pin: `GetResultPin()->PinType.PinSubCategoryObject` is populated by `Initialize()` and is the public-facing source of truth.

The same pattern applies to other engine K2Nodes that wrap function-call semantics without inheriting from `UK2Node_CallFunction` — async actions (`UK2Node_BaseAsyncTask`, handled separately via section 6d), component-bound events, and `UK2Node_CreateDelegate`. Add explicit recognizers before the generic CallFunction fallback.

## 6i. SwitchEnum Default Arm Lowering

`UK2Node_SwitchEnum` does **not** expose a `Default` exec pin, unlike its `UK2Node_Switch` base class (and the `SwitchInt`/`SwitchString`/`SwitchName` siblings). The engine synthesizes "default" behavior by leaving unused enum values' exec outputs unwired — at runtime, hitting an unwired value is a no-op.

The BPIR compiler's switch lowering mirrors this. When an authored BPIR `default -> @label` arm is present on a `switch_enum<T>(...)`, the compiler must:

1. Enumerate `EnumType->NumEnums()`.
2. Skip the auto-generated `_MAX` sentinel and any entries tagged `Hidden`.
3. Skip entries already wired by explicit arms.
4. For each remaining value, wire that value's exec output pin to the default arm's target label.

This is implemented in `WireSwitchEnumDefaultArm` (file-local helper in `BpirCompiler.cpp`). The existing `FindExecOutputPin` "default" branch still works for `SwitchInt`/`SwitchString`/`SwitchName` because they DO expose a real `Default` pin — only `SwitchEnum` needs the value-enumeration expansion.

## 6j. Type-Slot Resolution for Angle-Bracket Keywords

BPIR angle-bracket type slots (`subsystem<T>`, `cast<T>`, `make<T>`, `break<T>`) must route through the unified resolvers in `Private/Utils/ClassUtils.cpp`:

| Resolver | Slot kinds | What it handles |
|----------|------------|-----------------|
| `ResolveUClass` | `subsystem<T>`, `cast<T>`, object slots | Strip-or-prepend prefix variations (`U`/`A`/`F`/`I`), content-mount probing, `_C` suffix for BP classes |
| `ResolveUScriptStruct` | `make<T>`, `break<T>` | Same prefix handling for struct names |
| `ResolveUEnum` | `enum<T>` | 3-tier: full-path load → fast short-name lookup → `TObjectIterator` fallback for plugin/game modules |

Rolling your own resolution with `FindFirstObjectSafe<UClass>(*ShortName)` plus a single-direction prepend retry silently rejects canonical C++ identifiers — e.g. `UMusicPlayerSubsystem` prepended becomes `UUMusicPlayerSubsystem` and never resolves. The cast and `switch_enum` opcode arms in `BpirCompiler.cpp` were already routed through the unified resolvers; the `subsystem`, `make`, and `break` arms were brought to parity in the same fix.

When adding a new angle-bracket type keyword, route the type-slot lookup through the appropriate `ResolveU*` helper rather than calling `FindFirstObjectSafe` / `FindObject` directly. See `call("bpir.types")` for the user-facing surface of these keywords.

## 6k. Entry Function Metadata Round-Trip (`@meta` / `@flags`)

BPIR round-trips function-level metadata and Blueprint function flags — it is **not** a missing feature. The decorators are emitted as `@meta(...)` and `@flags(...)` lines placed immediately **before** the `entry function` (or `entry event`) signature; the parser folds the same lines back into the entry's `FBpirEntryMetadata` on recompile.

**Emit path** — `FBpirTextEmitter::EmitEntryDecoratorLines(UEdGraphNode*)` (`Private/Decompiler/BpirTextEmitter.cpp`), called from `BpirDecompiler.cpp` just before `EmitEntrySignature`. It reads metadata from whichever entry node it sees:

- `UK2Node_FunctionEntry` — `MetaData` (an `FKismetUserDeclaredFunctionMetadata`) for `Category` / `Tooltip` / `Keywords` / `CompactNodeTitle` / `DeprecationMessage` / `bCallInEditor` / `bThreadSafe` / `bUnsafeDuringActorConstruction` / `bDeprecated`; the `EFunctionFlags` bag comes from `GetExtraFlags()`.
- `UK2Node_CustomEvent` — same `@meta` fields via `GetUserDefinedMetaData()`, but `DeprecationMessage` / `bIsDeprecated` / `bCallInEditor` are read from the node's own public UPROPERTYs (the editor-set source of truth), and the flag bag is `CustomEvent->FunctionFlags`.
- `UK2Node_Tunnel` (macro entry tunnel only) — `@meta` fields only. Macros have no `EFunctionFlags` bag, so `@flags(...)` is intentionally never emitted on `entry macro` lines (and the parser rejects it there).

`FUNC_Native` is stripped on the emit side to mirror `UK2Node_FunctionEntry::SetExtraFlags`, so emission never produces a flag identifier the parse-back would drop. `@meta` is omitted when every field is empty; `@flags` is omitted for the default state (Public + nothing else).

**`@flags(...)` identifiers** — access specifier (`Public` / `Protected` / `Private`), `BlueprintPure` (from `FUNC_BlueprintPure`), `Const` (`FUNC_Const`), `Exec` (`FUNC_Exec`), plus the boolean meta flags `CallInEditor` and `ThreadSafe`. `BlueprintCallable` and `Deprecated` are likewise round-tripped.

**Parse path** — `FBpirParser::ParseMetaDecorator` and `FBpirParser::ParseFlagsDecorator` (`Private/Compiler/BpirParser.cpp`) accumulate into a `PendingMetadata` (`FBpirEntryMetadata`) that attaches to the next entry declaration. On compile, `BpirCompiler.cpp` applies it: `ApplyFlagsDecoratorToFunctionFlags` recomputes the `EFunctionFlags` word (clearing `FUNC_AccessSpecifiers | FUNC_BlueprintPure | FUNC_Const | FUNC_Exec | FUNC_Native` first, then ORing the decorator's flags), and `ApplyMetaDecoratorToKismetMetadata` / `ApplyFlagsDecoratorToKismetMetadata` write the `FKismetUserDeclaredFunctionMetadata` fields back. Round-trip is covered by `Tests/Bpir/TestBpirFunctionMetadata.cpp` and `Tests/Bpir/TestBpirFunctionMetadataEmit.cpp`.

---

## 7. Design Decisions

1. **Label scope** — Labels are scoped per entry block. Different entry blocks can reuse the same label names (e.g., `@then`). No cross-entry wiring.

2. **No inline pure expressions** — Every authored primary node is its own line. `call Foo(X: call Bar())` is NOT valid. Use `%tmp = call Bar()` then `call Foo(X: %tmp)`. Helper/generated nodes and zero-node instructions are exceptions to the primary-node line model.

3. **Pin name defaults** — `%name` alone defaults to the `ReturnValue` pin. Use `%name.PinName` for other outputs (e.g., `%loop.ArrayElement`, `%cast.AsMyCharacter`).

4. **Pin name normalization** — Pin names with spaces use camelCase: `Array Element` → `ArrayElement`. The compiler accepts both forms.

### Node-type specifics

- **StandardMacros library loading** — For ForEachLoop / WhileLoop / Sequence-style macros, load `/Engine/EditorBlueprintResources/StandardMacros.StandardMacros` as a `UBlueprint` (NOT as a `UEdGraph`), then iterate `MacroLib->MacroGraphs` and match `GetFName() == TEXT("ForEachLoop")` (or other macro name) to get the underlying `UEdGraph*`. `FCodeNodeEmitter::GetStandardMacrosLibrary` + `FindMacroGraph` already encapsulate this — reuse those helpers rather than reloading the library.

- **`UK2Node_ExecutionSequence`** uses lowercase-underscored internal pin names: `then_0`, `then_1`, `then_2`, … The display name in the Blueprint editor is `Then 0` / `Then 1` / `Then 2`, but all code (e.g. `WireNamedOutputToExec`, `FindPin`) must use the internal FName. The name is built by `UK2Node_ExecutionSequence::GetPinNameGivenIndex` at `C:/UE_5.6/Engine/Source/Editor/BlueprintGraph/Private/K2Node_ExecutionSequence.cpp:274-277` as `*FString::Printf(TEXT("%s_%d"), *UEdGraphSchema_K2::PN_Then.ToString(), Index)`. `then_0` is the primary same-row child per the FormatY same-row-marking rule; subsequent `then_N` stack below the primary with shared X.

---

## 7a. Return in Function Context — One Result Node Per Return

`EmitInstruction`'s `EBpirOpcode::Return` case gives **every `return` statement its own
`UK2Node_FunctionResult`**. Several result nodes per function graph are legal — the engine's
`FKCHandler_FunctionResult` merges them, `UK2Node_FunctionResult::SyncWithPrimaryResultNode` keeps
their pin sets identical, and `CanUserDeleteNode` explicitly permits the extras — and it is the
shape a human authors when each branch ends in its own Return node.

Selection order for a `return`:

1. Scan `CurrentGraph->Nodes` for a `UK2Node_FunctionResult` that is **not already claimed by an
   earlier `return` in this block** and whose exec-in has no links.
2. Failing that, `FCodeNodeEmitter::CreateFunctionResult` makes a new one. The engine's
   `PostPlacedNewNode` → `SyncWithPrimaryResultNode` copies the primary node's user-defined pins and
   reconstructs, so `CreateFunctionResult` calls `AllocateDefaultPins()` **only when the node still
   has no pins** — calling it after that reconstruction adds a second `execute` exec pin (the
   engine's user-pin loop is `FindPin`-guarded, its exec `CreatePin` is not).

**The claim check cannot use exec links.** Exec wiring is Pass 3b, which runs after every node in the
block is emitted, so at emit time every result node's exec-in is still empty. `EmitMap` is reset per
block and only `return` stores a `UK2Node_FunctionResult` in it, so it is the authoritative record of
which result nodes this block has already taken. Sharing one node across returns was
`B-bpir-second-return-branch-data-dropped`: both branch execs landed on it, its single-link data
inputs kept only the last branch's values, the earlier branch's producers were orphaned, and the
compile reported success.

`WireDataPins` carries the matching tripwire: a `return` whose target pin is already fed by a node
**this compile created** is a hard error rather than a silent re-link. Links that pre-date the
compile (Extend mode splicing into an already-authored result node) are left alone, and macro
returns are excluded — a macro graph has exactly one exit tunnel whose data pins every exit shares.

On the decompile side, `FBpirTextEmitter::EmitReturn` picks its form from the result node's data-pin
**count**, not from how many pins happen to be wired: one data pin emits `return %v`, more than one
emits the named `return (Name: %v, ...)` form. `return %a, %b` is not parseable BPIR — the parser
reads the whole comma list as a single value reference — and the positional form always lands on the
node's first data pin, so a partially wired multi-output result node needs the names.

---

## 7b. Macro Graph Compilation

Macro graphs use a separate setup path from functions. The entry point dispatch (`EBpirEntryKind::Macro`) calls `SetupMacro()` instead of the function setup path.

### SetupMacro flow

1. **Create graph** — `FBlueprintEditorUtils::AddMacroGraph()` creates a new `UEdGraph` and automatically calls `CreateMacroGraphTerminators()`, which spawns the `UK2Node_Tunnel` entry/exit pair.

2. **Find tunnels** — `SetupMacro` iterates the auto-created nodes to find the entry tunnel (`bCanHaveOutputs == true`) and exit tunnel (`bCanHaveInputs == true`). It does NOT create tunnels itself — doing so would cause duplicates since `AddMacroGraph` already creates them.

3. **Determine pure vs impure** — Scans `Block.Instructions` for any impure opcode. If all instructions are pure, the entry tunnel's exec pin is left null (`EntryExecPin = nullptr`). The main loop explicitly allows `EntryExecPin == nullptr` when `Block.Kind == EBpirEntryKind::Macro`.

4. **Configure tunnel pins** — Pin direction is mirrored relative to the graph:
   - Entry tunnel: `EGPD_Output` pins = macro input parameters (data flows out of entry into the graph)
   - Exit tunnel: `EGPD_Input` pins = macro output parameters (data flows into exit from the graph)

5. **Multi-exit exec pins** — For macros with `ExecOutputNames` (the `[ExitA -> @a, ExitB -> @b]` syntax), named exec input pins are created on the exit tunnel. Each maps to a distinct exec output on the macro instance node when the macro is used.

6. **SwitchToGraph** — A helper that updates all graph-dependent compiler state in one call: sets `CurrentGraph`, resets `bCurrentGraphIsMacro = false` and `CurrentMacroExitTunnel = nullptr`. After `SetupMacro` finds the tunnels, it sets `bCurrentGraphIsMacro = true` and caches `CurrentMacroExitTunnel` for O(1) access during return wiring.

### Return in macro context

When the compiler encounters a `return` instruction and `bCurrentGraphIsMacro` is true:

- **Default return** — wires to the cached `CurrentMacroExitTunnel`'s default exec input pin.
- **Named data return** (`return (Name: value, ...)`) — wires data pins to the corresponding input pins on the exit tunnel.
- **Multi-exit return** (`return [ExitPin]`) — uses `Inst.TypeArg` to find the specific named exec input pin on the exit tunnel via `GetExecInputPin()`, then wires to it.
- **Combined** (`return [ExitPin] (Name: value, ...)`) — wires both exec and data pins.

This avoids creating `UK2Node_FunctionResult` nodes, which are invalid inside macro graphs.

### Graph lifecycle

After `SetupMacro` completes and the macro body is compiled, the compiler calls `SwitchToGraph(SavedGraph)` to restore the previous graph context (typically the ubergraph). This allows multi-entry compilation where some entries are macros and others are event/function graphs.

---

## 4b. SwitchToGraph State Ordering and FBlockSetupState Restoration

`SwitchToGraph()` clears all graph-dependent compiler state in one call: it sets `CurrentGraph`, calls `ValueResolver->SetGraph()` (which resets `EmitMap` and the auto-property/auto-break caches), and resets `CurrentMacroExitTunnel` to `nullptr` and `bCurrentGraphIsMacro` to `false`.

In the two-phase architecture, Phase 2 must call `SwitchToGraph()` to restore each block's graph context before emitting instructions. This creates a critical ordering requirement: **state set before `SwitchToGraph()` is wiped by the call.** Specifically:

1. **EmitMap must be set AFTER SwitchToGraph, not before.** `ValueResolver->SetGraph()` (called inside `SwitchToGraph`) clears the `EmitMap`. If Phase 2 calls `SetEmitMap()` and then `SwitchToGraph()`, the emit map is immediately destroyed. The correct order is `SwitchToGraph()` first, then `SetEmitMap()`.

2. **CurrentMacroExitTunnel must be restored from FBlockSetupState AFTER SwitchToGraph.** `SwitchToGraph()` resets `CurrentMacroExitTunnel = nullptr`. For macro entry blocks, the exit tunnel pointer was found during Phase 1 and must be saved in `FBlockSetupState::MacroExitTunnel`, then restored after the Phase 2 graph switch.

The `FBlockSetupState` struct bridges Phase 1 and Phase 2 by saving per-block state that would otherwise be lost:

| Field | Purpose |
|-------|---------|
| `EntryExecPin` | Exec output pin from the entry node |
| `GraphContext` | The `UEdGraph*` active when the entry was created |
| `EntryNode` | Entry node pointer (for pin re-registration in Phase 2) |
| `MacroExitTunnel` | Exit tunnel for macro blocks (nullptr for non-macros) |
| `bValid` | Whether Phase 1 setup succeeded |

When Phase 2 processes a macro block, the restoration sequence is:

```
SwitchToGraph(BlockState.GraphContext);       // clears graph-dependent state
SetEmitMap(...);                              // re-establish emit map AFTER switch
CurrentMacroExitTunnel = BlockState.MacroExitTunnel;  // restore AFTER switch
bCurrentGraphIsMacro = (MacroExitTunnel != nullptr);
```

Violating this ordering causes silent failures: macro `return` instructions find `CurrentMacroExitTunnel == nullptr` and either crash or incorrectly create `UK2Node_FunctionResult` nodes (which are invalid in macro graphs).

---

## 7c. BPIR Expression Subgraph Compilation

`UK2Node_BpirExpression` is a composite node that owns a `BoundGraph` containing a tunnel pair (entry/exit). The user writes BPIR text with an optional header declaring inputs/outputs, and `FBpirSubgraphCompiler` compiles the body into the subgraph. This is distinct from macro compilation (section 7b) — expressions live inside an existing graph as collapsed nodes, not as standalone macro graphs.

### Compilation flow (FBpirSubgraphCompiler)

1. **Split header and body** — `SplitHeaderAndBody()` splits on a `---` separator. Lines before it are `input`/`output` declarations; lines after are the BPIR body. If no `---` is present, the entire text is body, and `AutoDiscoverInputs()` scans for `$VarName` references not backed by blueprint variables.

2. **Parse declarations** — `ParseDeclarations()` reads `input Name: Type` and `output Name: Type` lines. Inputs become `EGPD_Output` pins on the entry tunnel (data flows out into the subgraph); outputs become `EGPD_Input` pins on the exit tunnel (data flows in from the subgraph).

3. **Detect impure** — `DetectImpure()` scans the body for impure opcode keywords. If any are found, exec pins (`execute`) are created on both tunnels.

4. **SetExitTunnel** — Before compiling the body, the subgraph compiler calls `Compiler.SetExitTunnel(ExitTunnel)`. This sets `bCurrentGraphIsMacro = true` and `CurrentMacroExitTunnel = ExitTunnel` on `FBpirCompiler`, reusing the same macro-context return path described in section 7b.

5. **Inject external variables** — For each declared input, `InjectExternalVariable(Name, TunnelPin)` registers the entry tunnel's output pin as a `$VarName` in the value resolver. The body can then reference `$Name` to read the input.

6. **CompileBodyIntoGraph** — `FBpirCompiler::CompileBodyIntoGraph()` runs the standard three-pass compilation (scan/emit/wire) into `BoundGraph`, starting execution from the entry exec pin (if impure).

7. **Wire outputs** — After compilation, the subgraph compiler re-parses the body to build the `ValueIndex`, then wires each declared output's `%ref` node to the corresponding exit tunnel input pin. Wildcard output pins adopt the type from the source pin.

8. **Wire exit exec** — If impure, the last exec output from the compiled chain is connected to the exit tunnel's exec input.

### Why SetExitTunnel is required

Without `SetExitTunnel()`, the compiler treats `return` as a function context return and creates a `UK2Node_FunctionResult`. Function result nodes are invalid inside a composite subgraph — they belong in function graphs and expect a `ReturnValue` pin declared by the function signature. In a subgraph, there is no function signature, so the result node has no data pins. This causes a `"Could not find target pin ReturnValue"` error during wiring.

`SetExitTunnel()` makes the compiler treat the subgraph identically to a macro: `return` wires to the cached `CurrentMacroExitTunnel` instead of spawning a `UK2Node_FunctionResult`. Data return pins (`return (Name: value)`) wire to the exit tunnel's named input pins.

### BoundGraph naming with MakeUniqueObjectName

`UK2Node_BpirExpression::PostPlacedNewNode()` renames `BoundGraph` using `MakeUniqueObjectName()`:

```cpp
const FName UniqueName = MakeUniqueObjectName(
    GetOuter(), UEdGraph::StaticClass(), TEXT("BpirExpressionGraph"));
BoundGraph->Rename(*UniqueName.ToString(), GetOuter());
```

This is necessary because multiple `UK2Node_BpirExpression` instances can coexist in the same blueprint. Without unique names, the default graph name collides and triggers a fatal `UObject::Rename` assert. The `MakeUniqueObjectName` call appends a numeric suffix (e.g., `BpirExpressionGraph_0`, `BpirExpressionGraph_1`).

### Limitations

- **`return VALUE` requires a declared output** — The body cannot `return (Name: value)` unless `output Name: Type` was declared in the header. Without a header output declaration, no corresponding pin exists on the exit tunnel, and the wiring silently fails.
- **No multi-exit exec** — Unlike macros, expression subgraphs do not support `ExecOutputNames` / `return [ExitPin]`. There is a single exit exec pin named `execute`.
- **Reentrancy guard** — `UK2Node_BpirExpression::bIsRebuilding` is a static flag that prevents recursive `RebuildFromBpir()` calls during `ReconstructNode` or `PostEditUndo`.

---

## 8. Error Handling & Atomic Transactions

### Parser errors

| Condition | Severity | Behavior |
|-----------|----------|----------|
| Missing closing `}` for entry block | Error | Abort — was previously auto-closed silently |
| Unmatched parentheses in `cast`, `make`/`break`, `subsystem`, `sequence`, `make_array` | Error | Abort — was previously accepted silently |
| Empty entry name | Error | Abort |
| Missing dot in `component_event` / `widget_event` name | Error | Abort |
| Exec label without `@` prefix | Error | Abort |
| Sub-parser return value indicates failure | Error | Abort — return values were previously discarded |
| Unknown line keyword | Error | Abort |

### Compiler errors

| Condition | Severity | Behavior |
|-----------|----------|----------|
| Unknown `%name` reference | Error | Abort — undefined value |
| Unknown `@label` reference | Error | Abort — undefined label |
| Unknown `$variable` | Warning | Emit VariableGet node anyway (may exist at runtime) |
| Duplicate `%name` | Error | Abort — redefined value |
| Duplicate `@label` | Error | Abort — redefined label |
| Unresolved function | Error | Abort — function not found |
| Type mismatch on pin | Warning | Attempt connection, let UE validate |
| Missing required pin arg | Warning | Leave pin at default |
| Forward `%name` reference | Error | Abort — referencing a result defined on a later line |
| `TryCreateConnection` failure (any of ~10 wire call sites) | Error | Accumulated into `AccumulatedErrors`; triggers rollback |
| `EmitInstruction` failure in `InsertCodeAfterNode` or `CompileBodyIntoGraph` | Error | Abort — return value was previously discarded |
| `WireDataPins` or `WireExecPins` failure | Error | Propagated to caller — `WireExecPins` previously always returned true |

### Decompiler warnings

| Condition | Severity | Behavior |
|-----------|----------|----------|
| Impure node not reachable from any entry point | Warning | Reported in post-walk orphan-detection pass |
| `LinkedTo[0]` is null on an exec pin | Warning | Null-checked before dereference (previously could crash) |
| Exec pin has more than one connection (`LinkedTo.Num() > 1`) | Warning | All connections reported; first is used |
| `"?"` returned from value resolver | Warning | Reported (was previously silent) |
| Cycle detected in pure-dependency traversal | Guard | Node added to `VisitedNodes` before recursive call (prevents infinite recursion) |

Atomic transaction: if ANY error occurs during compilation, all created nodes are deleted (rollback).

> **Note:** `$variable` references do not trigger rollback. They are resolved at the value-resolver level (Pass 3), not during parsing. An unknown `$variable` emits a warning and creates a VariableGet node regardless.

---

## 9. Decompiler Label Allocation for Multi-Exit Nodes

The decompiler's multi-exit node handlers (Branch, Switch, Cast, Loop, Latent, Timeline, MacroInstance) emit `@label:` definitions for each exec output pin. A critical constraint governs label allocation: **labels must only be allocated for exec output pins that are actually connected** (`LinkedTo.Num() > 0`).

### The problem

If a label is added to `LabelMap` for an unconnected exec pin, the label name appears in the emitted instruction text (e.g., `[false -> @else]`), but the corresponding `@label:` definition is never emitted. This is because `PendingLabels` — the mechanism that emits label definitions when the next connected node is visited — is skipped for pins with no connections. The recompiler then fails with `"Undefined label reference"` because the label is referenced but never defined.

### The pattern

The correct decompilation pattern for multi-exit nodes is:

1. **Find all exec output pins** on the node.
2. **Check connectivity** — for each pin, test `Pin->LinkedTo.Num() > 0`.
3. **Conditionally allocate labels** — only call `AllocateLabel()` and add to `LabelMap` for connected pins.
4. **Build instruction text** — only include `[pin -> @label]` clauses for pins that have labels.
5. **Emit the instruction** — the text references only labels that will have corresponding `@label:` definitions.

This applies uniformly to all multi-exit handlers. An unconnected `false` branch on a Branch node, an unused `default` case on a Switch, or an unconnected `fail` pin on a Cast node should all produce no label reference in the decompiled output.

### Round-trip implication

This constraint is essential for round-trip fidelity (decompile → recompile). If the decompiler emits a label reference for an unconnected pin, the recompiler creates a label reference with no definition, causing a compile error. The original Blueprint had no connection on that pin, so the recompiled version should also have no connection — which means no label should exist.

### Call-site locations for LabelMap building

Every decompiler path that builds `LabelMap` from multi-exec output pins must enforce the `LinkedTo.Num() > 0` skip. These are the call sites in `Private/Decompiler/BpirDecompiler.cpp` (as of this revision):

| Path | Line | Node class |
|------|------|-----------|
| Latent | ~833 | `UK2Node_BaseAsyncTask` and subclasses |
| Timeline | ~886 | `UK2Node_Timeline` |
| DoOnce / Gate / FlipFlop | ~921 | `UK2Node_MacroInstance` (standard macros) |
| Unknown (generic fallback) | ~1172 | Any multi-exec node not matched by a specific handler |

The Branch, Switch, Cast, Loop, and specific MacroInstance paths already follow the pattern via shared helpers.

**Invariant for new paths:** any new decompiler path that builds `LabelMap` from multi-exec outputs must follow the same connected-pin-only pattern. The canonical test is a round-trip recompile of the decompiler output — emitting a stray label for a disconnected pin produces a label reference in the output text with no matching block body, which BPIR recompile rejects with `"Undefined label reference"`.

---

## 10. What Gets Eliminated

### From compiler (~800 LOC removed)
- `FindMatchingClosingBrace` — no braces
- `ExtractScopedBlock` — labels define blocks
- `CountBraceDepthDelta` — no brace depth
- `PendingReconvergePins` — explicit `exec -> @label`
- Recursive `ProcessIfStatement` / `ProcessForLoop` / `ProcessWhileLoop` / `ProcessSwitchStatement` / `ProcessSequenceStatement` — flat label dispatch

### From decompiler (~300 LOC removed)
- `AnalyzeBranch` / `CollectReachableNodes` — emit labels from graph
- Indent tracking — cosmetic only
- Recursive `DecompileChain` branching — flat node enumeration

---

## Post-Compile Node Layout Engine

After a successful compile (no accumulated errors), the compiler runs a layout pass over every graph it touched. The pass repositions created nodes so they don't overlap and follow a readable left-to-right execution flow. It runs headless — no open graph editor required.

### Authored-position mode

BPIR can carry `@(x, y)` metadata on node-backed instructions. The compiler chooses layout mode per entry body after instruction emission, when it knows which instructions produced primary visible nodes:

| Body mode | Selection rule | Layout behavior |
|-----------|----------------|-----------------|
| Auto-layout | No primary emitted instruction in the body has an authored position | The existing layout pass formats the full created-node pool. Implicit helper/generated nodes are allowed and layout-managed. |
| Authored-position | Every primary emitted instruction in the body has an authored position | Primary nodes receive the authored `NodePosX` / `NodePosY` and are excluded from post-compile movement. Implicit visible helper/generated nodes are rejected. |
| Invalid mixed mode | Only some primary emitted instructions in the body have authored positions | Compilation fails with a manual-placement diagnostic for that entry body. |

Positions are absolute graph coordinates for the instruction's primary emitted node only. They do not apply to transparent reroutes/knots, comment-only lines, or instructions that emit no primary node. A zero-primary-node instruction with `@(x, y)` is a compile error, because there is no visible node to receive the coordinate.

The authored-position restriction on helpers is deliberate v1 scope. Auto-generated visible helpers, such as implicit `BreakStruct` nodes created from dotted struct access, do not have a BPIR instruction line where the author can supply coordinates. In authored-position mode the compile fails and tells the author to add missing positions to every visible node-backed instruction, remove positions from the body to use auto-layout, or make the helper explicit with its own positioned BPIR line.

Decompilation appends ` @(x, y)` centrally for node-backed BPIR lines using the current node `NodePosX` / `NodePosY`, so manually moved nodes become editable coordinates in the emitted BPIR text.

### When it runs

`RunLayoutPass()` is called at three points, always on the success path only (after the error-check / rollback block, before `NodeCreationStack().Add()`):

| Method | Anchor | Scope |
|--------|--------|-------|
| `Compile()` | First event/entry/tunnel in created set | Main graph + `CreatedFunctionGraphs` + `CreatedMacroGraphs` |
| `InsertCodeAfterNode()` | The insertion-point node (frozen) | Insertion-point's graph; existing nodes = frozen obstacles |
| `CompileBodyIntoGraph()` | Owner of `EntryExecPin` | Target graph |

`InsertCodeBeforeNode()` delegates to `InsertCodeAfterNode` and inherits its layout call.

#### Anchor fallback — upstream walk for insert mode

`RunLayoutPass` picks the layout anchor from the created-node set. In insert mode (`compile_bpir` wires a new Call into a pre-existing Event that is **not** in `CreatedGUIDs`), none of the created nodes is an event/entry/tunnel. When that happens, the pass walks upstream along input exec pins starting from `Pool[0]` to find the true exec root — even if that root is in the `Obstacles` set. On match, the upstream root is promoted from `Obstacles` into `Pool` and used as the Anchor. This preserves the same-row invariant for inserted chains (the chain lands on the same Y as the existing event, rather than the first created node anchoring itself and dragging downstream siblings off-row).

Undo remains safe across this promotion: UE transactions record `NodePosX`/`NodePosY` changes on any `Modify()`-ed node regardless of which transaction created that node, so repositioning an obstacle still round-trips through undo. BlueprintAssist does not do this walk — it relies on caller-supplied `NodeToKeepStill`. See the [Blueprint wiki insertion sections](wiki-src/blueprint.md#blueprintinsert_bpir_at_node) for the insert-mode surface.

### Pipeline phases (in order)

1. **Save anchor position** — `FIntPoint(Anchor->NodePosX, Anchor->NodePosY)` captured before any repositioning.
2. **BuildFormatXInfoMap** — dual-stack output/input alternating BFS over exec pins. Builds a parent/child tree (`FFormatXInfo`) with parent-priority resolution (prefer larger NodePosX for output direction, smaller for input). Cycle-safe. Capped at `TraversalIterationCap` (default 10,000).
3. **FormatX pass 1** (`bUseClusterBounds=false`) — BFS through the info tree, assigning each node's X via `GetChildX(parent, child, direction)`. Root X untouched.
4. **FormatParameterNodes** — for each impure node with pure data-pin ancestors, instantiate `FNodeLayoutParameterFormatter`. It arranges pure nodes in a left-side column (right edge aligned to consumer's left minus `PinPadX`, stacked vertically with `IntraParameterPadY`). Registers the union rect (consumer + pures) as that consumer's "cluster bounds". First-consumer-wins rule for shared pures (BFS order determines priority).
5. **FormatX pass 2** (`bUseClusterBounds=true`) — rerun with cluster-extended parent bounds so parameter subtrees don't collide with the next impure column.
6. **GetPinsOfSameHeight** — marks the first exec-output child of each parent as `bSameRowAsParent=true`. FormatY will keep these at parent Y.
7. **FormatY** — DFS pre-order placement (`FormatY_Recursive`). Same-row child inherits parent Y; non-same-row siblings stack below with `NodePadY` gap. After each placement, a collision loop jumps the node below any overlapping already-placed node (cluster bounds used). Capped at `CollisionIterationCap` (default 30). **Critical invariant:** the collision loop MUST skip the child's direct parent — the same-row child necessarily overlaps its parent's cluster rect (the parent's pure-input column extends downward into the child's row), and treating the parent as a blocker pushes the same-row child down, producing a diagonal stair-step across what should be a linear exec chain. The guard is a single early-exit in the overlap test:

    ```cpp
    // Inside FormatY_Recursive's TryBlock / overlap predicate:
    if (Other == Info->LinkFromParent.GetFromNode()) return false;
    ```

    This matches BlueprintAssist's `EdGraphFormatter::FormatY_Recursive` pattern (`Plugins/BlueprintAssist/Source/BlueprintAssist/Private/BlueprintAssistFormatters/EdGraphFormatter.cpp:1120-1123`). FormatX pass 2 places siblings far enough apart in X that no non-parent ancestor's cluster can reach a same-row grandchild's X range, so direct-parent-skip alone is sufficient — BA does **not** use a slide-right mechanism, and neither do we.

8. **ResetRelativeToAnchor** — translate entire pool by `SavedPos - CurrentAnchorPos` so the anchor lands back at its pre-format coordinates.
9. **SnapToGrid** — directional rounding: Floor for input-direction children, Ceil for output-direction, Round for root. Y always Round. Grid = `InternalGridPx` (default 8).

**Pipeline order in `FNodeLayoutEngine::Format` (current):** FormatX pass 1 → FormatParameterNodes → FormatX pass 2 → GetPinsOfSameHeight → FormatY → `ResolveConsumerClusterOverlaps` → ResetRelativeToAnchor → SnapToGrid. `ResolveConsumerClusterOverlaps` is a BFS-order secondary pass that translates each consumer cluster down past any already-placed sibling clusters it overlaps. Because placed consumers are never moved by later ones, the pass caches `GetCurrentClusterBounds` results in a parallel `TArray<FSlateRect>` alongside `PlacedConsumers`; without that cache the pass is O(N²) in Slate text measurements (see `lessons.md`).

### Size estimation

Node sizes are estimated from `UEdGraphNode` data alone (no Slate widget required):

- **Title width**: measured via `FSlateFontMeasure` using `FAppStyle::Get().GetFontStyle("Graph.Node.NodeTitle")`.
- **Pin label widths**: measured with `"Graph.Node.PinName"` font. Input and output columns paired row-by-row.
- **Height**: `HeaderHeightPx + max(inputPins, outputPins) × PinRowHeightPx + footer`.
- **Fallback**: when `FSlateApplication` is uninitialized (e.g. `-NullRHI` commandlet), falls back to char-count heuristic (8 px/char title, 7 px/char pin labels). Logs once at verbose level.

### Settings

`UBpirLayoutSettings` (Project Settings → Plugins → BPIR Layout):

| Setting | Default | Purpose |
|---------|---------|---------|
| `bEnableBpirLayoutPass` | true | Kill switch — false disables the pass entirely |
| `PinRowHeightPx` | 22 | Height per pin row |
| `HeaderHeightPx` | 44 | Node header height |
| `HorizontalPaddingPx` | 24 | Interior horizontal padding |
| `NodePadX` | 80 | Gap between adjacent columns |
| `NodePadY` | 48 | Gap between sibling nodes |
| `PinPadX` | 32 | Gap between pure-node column and consumer |
| `IntraParameterPadY` | 16 | Gap between stacked pure nodes |
| `InternalGridPx` | 8 | Directional grid alignment |
| `CollisionIterationCap` | 30 | Max nudge iterations per node |
| `TraversalIterationCap` | 10000 | Max BFS iterations |

### Key files

| File | Role |
|------|------|
| `Private/Compiler/NodeLayoutEngine.h` | All types and function declarations |
| `Private/Compiler/NodeLayoutEngine.cpp` | Engine, FormatX, FormatY, size estimation, grid snap |
| `Private/Compiler/NodeLayoutParameterFormatter.h/.cpp` | Pure-node column formatter |
| `Public/BpirLayoutSettings.h` | UDeveloperSettings subclass |
| `Private/Compiler/BpirCompiler.cpp` | `RunLayoutPass()` static helper + 3 call sites |

### Limitations

- No knot node insertion (long wires accepted as-is).
- No comment-box auto-padding (comments still use `FinalizeCommentBoxes` bounding-box math).
- No helixing parameter style (only left-side column).
- Only operates on nodes in `CreatedNodeGUIDs` — user-authored nodes are never repositioned.
- No `SimpleRelativeFormatting` fast path — always runs full pipeline.

---

## See also

- [BPIR Language Reference](wiki-src/bpir.md) — instruction syntax, types, sigils
- [BPIR Examples](wiki-src/bpir.examples.md) — code examples with test annotations
- [Blueprint wiki insertion sections](wiki-src/blueprint.md#blueprintinsert_bpir_at_node) — mid-flow graph insertion modes
- [BPIR Test Matrix](bpir-test-matrix.md) — feature coverage and known gaps
- [Architecture](arch.md) — plugin architecture reference
- [Lessons](lessons.md) — corrective patterns including type converter pitfalls
