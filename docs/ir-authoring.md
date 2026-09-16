---
type: guide
summary: "IR authoring contract for PinWright maintainers: ephemeral-IR invariant, IrCore text helpers, grammar target, diagnostics, sidecar/RPC dual-surface rule, test patterns, and BPIR/MGIR/AGIR examples."
date: 2026-09-03
tags: [ir, ircore, bpir, mgir, agir, asset-dump, diagnostics, testing]
---

# IR Authoring Guide

## Ephemeral IR Invariant

IR text is EPHEMERAL. BPIR, MGIR, AGIR, SCIR, BTIR, MSIR, NIR, PCGIR, CRIR, and any future IR exist only as:

1. Asset-dump sidecars that are fully rewritten on every re-dump.
2. Compile-input strings that are immediately converted into Unreal assets.

No user stores raw IR text as durable project data. Agents read fresh dumps; humans rarely touch the text directly. Therefore, do not write backwards-compatibility code, dual-accept parser windows, deprecation cycles, migrations, or upgrade utilities for old IR grammar. Grammar can change at any time when the emitter and parser move together. Tests do not need version compatibility; regenerate goldens alongside the grammar change.

This invariant comes before every local syntax preference. If a new IR author is about to keep an old syntax form "just in case", the answer is no.

Not an IR: **PinWright Model** (`.pwmodel`) is a durable, user-authored file format, not an IR, and nothing in this guide governs it. It has no decompiler, registers no sidecar, and is versioned with old parser paths retained. See [pwmodel-design.md](pwmodel-design.md) for the distinction and its consequences.

## IrCore Text Utilities

`FIrTextUtils` is the shared low-level text layer. Use it before adding IR-local quote, split, position, or property-filter helpers.

| Utility | Use when |
|---|---|
| `Quote` | Emitting a double-quoted string literal, including the surrounding quotes. |
| `EscapeString` | Escaping a string body before a caller adds its own delimiters. |
| `FormatNameToken` | Emitting a reflected name, graph name, pin name, field name, or path-like symbol that may need backticks. |
| `FormatPositionSuffix` | Appending the standard authored-position suffix, ` @(x, y)`. |
| `FormatFieldList` | Emitting parenthesized comma fields, including the empty `()` case. |
| `IsSafeReflectedProperty` | Filtering reflected UObject properties before emitting them into IR text; pass IR-specific struct/name reject predicates. |
| `StripTrailingComment` | Removing a `#` line comment while respecting quoted strings and backtick name tokens. |
| `SmartSplit` | Splitting comma, space, or field lists while ignoring delimiters inside quotes, names, parens, brackets, braces, and angle brackets. |
| `FindMatchingChar` | Finding a matching close delimiter while respecting nested delimiters, quotes, and backtick names. |
| `TryExtractPosition` | Parsing and removing an authored `@(x, y)` suffix from a line. |

Adjacent helpers such as `TryUnwrapNameToken`, `TryUnwrapStringLiteral`, `FindTopLevelDelimiterPositions`, `IsUnescapedQuote`, and `IsUnescapedBacktick` are also shared. Prefer wrapping them locally only when the wrapper documents an IR-specific policy difference.

## Default-Valued Properties: the zero-default rule

`FIrTextUtils::AppendReflectedFields` omits every property equal to its archetype/CDO default. That keeps blocks short but makes an absent line **ambiguous**: the reader cannot tell "at a default they would have to look up" from "false / 0 / unset", and every reader guesses the latter. The guess inverts on any property whose class default is not its type's zero value — `UNiagaraSpriteRendererProperties::bSubImageBlend` defaults to `true`, so its absence read as `false` and a bulk audit of 57 emitters produced 11 confidently-wrong results (`B-nir-renderer-omits-default-valued-properties`).

The rule that removes the ambiguity, `FReflectedFieldEmitOptions::bEmitNonZeroDefaults`:

- Omit a default-valued property **only** when that default is the type's zero value (`false`, `0`, empty string / name / text / array, null object, identity struct).
- A property at a class default that is *not* the zero value is emitted with a trailing ` @default` marker, formatted against a nullptr default so the whole value prints rather than an empty `"()"` diff.
- Overridden properties are emitted unmarked, keeping the archetype diff so struct sub-fields still suppress against per-field defaults (`B-decompile-struct-subfield-dropped`).

Absence then means exactly one thing, and it is the thing readers already assume. This is a strict superset of the old output — nothing that was emitted before stops being emitted — so it is compile-direction safe: re-compiling an explicitly written default is a no-op.

Adoption status: **NIR only** (`NIRDecompiler::BuildReflectedFields`). NIR is decompile-only, so no parser had to learn the suffix. MGIR, AGIR, BTIR, SCIR, PCGIR still run with the flag off. Adopting it in an IR whose text feeds a compiler requires that compiler to strip the ` @default` suffix first, plus a golden-text refresh; do both in one change or not at all.

## Grammar Target

New IRs should use the harmonized grammar target below. Existing IRs may lag this target until the corresponding harmonization task lands, so check the current parser before changing tests.

Target grammar:

- Backtick-delimited name tokens for any non-bare identifier.
- Double-quoted strings with the standard escape set: `\\`, `\"`, `\n`, `\r`, `\t`, and `\uNNNN`.
- `#` line comments outside strings and backtick names.
- `@(x, y)` authored position suffixes.
- `:` as the field separator inside argument or property lists.
- Keyword-first, line-oriented instructions with an opcode enum behind the parser.

Illustrative target shape:

```text
entry example `/Game/Example/Asset` {
    %n0 = call `Node With Space`(Input: "value", DisplayName: `Unsafe?`) @(0, 0) # comment
    output Result: %n0
}
```

Current implementation notes:

- BPIR has its `FBpirGrammar` inside `Compiler/BpirTokenizer.cpp` and still has some parser-local compatibility helpers.
- MGIR and AGIR expose `FMGIRGrammar` / `FAGIRGrammar`, route many parser operations through `FIrTextUtils`, and keep IR-specific syntax in their parser/compiler files.
- The shared tokenizer handles words, numbers, strings, sigils, arrows, parens/brackets/angles, commas, colons, and `#` comments. Backtick name handling lives in `FIrTextUtils` and parser code, not as a universal tokenizer token.

## `IIrGrammar`

Every IR grammar should implement `IIrGrammar` with:

- An opcode enum, such as `EBpirOpcode`, `EMGIROpcode`, or `EAGIROpcode`.
- A keyword-to-opcode map used by `TryGetOpcode`.
- A syntax-keyword set used by `IsKeyword` for non-opcode words such as `entry`.
- An `OpcodeToText` helper when emitters or diagnostics need canonical keyword text.

The existing examples are:

- BPIR: `Compiler/BpirTokenizer.cpp` local `FBpirGrammar`, backed by `EBpirOpcode`.
- MGIR: `MGIR/MGIRGrammar.h/.cpp`, backed by `EMGIROpcode`.
- AGIR: `AGIR/AGIRGrammar.h/.cpp`, backed by `EAGIROpcode`.

Keep the grammar table as the single source of truth for accepted keywords. Do not duplicate opcode strings in parser branches, emitters, and diagnostics.

## `FIrTypeSpec`

Use `FIrTypeSpec` when a pin, parameter, port, return value, container element, delegate signature, or reflected connection carries structured type data. It records the type kind, inner names, secondary delegate signature names, container type, nested element/key specs, and const/reference modifiers.

Use a bare `FString` or `FName` only when the value is a symbol, graph name, asset path, property name, or class-expression token whose type is resolved by the destination node or asset domain.

Type parsing and emission should go through `IrTypeSpecParser` with an IR-specific `FIrTypeGrammar`. BPIR aliases this shared type model through `FBpirTypeSpec`; MGIR and future graph IRs should use the same parser when their ports need more than a raw name.

## Diagnostics

Current `FIrCompileDiagnostic` is intentionally small:

```cpp
struct FIrCompileDiagnostic
{
    int32 Line = -1;
    FString Message;
};
```

BPIR extends it as `FCompileError`; MGIR and AGIR parse errors extend it with a `Code` string. Today there is no shared severity enum, column/range type, or source-span object. When a compiler needs structured RPC output, map the current line/message/code shape into that response without pretending the shared diagnostic already has richer fields.

Use this distinction consistently:

- Hard errors stop parsing or asset mutation. Examples: malformed syntax, unresolved required asset, invalid opcode, or a compile action that cannot produce a valid asset.
- Lossy-roundtrip warnings keep the operation successful but disclose omitted or normalized state. Examples: unsupported visual-only layout, skipped comment boxes, omitted optional editor-only data, or decompile-only graph families.

If shared diagnostics gain severity or source ranges later, make that a real `FIrCompileDiagnostic` change and update all IRs together. Do not invent parallel per-IR severity shapes.

## File Layout

Prefer a self-contained IR directory under `Private/<IR>/`:

```text
Private/<IR>/
  <IR>Opcodes.h
  <IR>Grammar.h/.cpp
  <IR>Parser.h/.cpp
  <IR>Decompiler.h/.cpp
  <IR>TextEmitter.h/.cpp
  <IR>Compiler.h/.cpp
  <IR>Compiler_<Family>.cpp
```

Handlers stay under the owning domain:

```text
Private/Handlers/<Domain>/<IR>DecompileHandler.cpp
Private/Handlers/<Domain>/<IR>CompileHandler.cpp
```

Asset-dump registration stays in `Private/Handlers/Asset/AssetDumpHandler.h/.cpp`.

Existing layouts:

- BPIR is older and split between `Private/Compiler/` and `Private/Decompiler/`.
- MGIR is under `Private/MGIR/`.
- AGIR is under `Private/AGIR/`, with per-family compiler files for distinct anim-node shapes.

For new IRs, copy the MGIR/AGIR layout unless the domain already has a stronger convention.

## Asset-Dump Sidecar Integration

Every IR must have an asset-dump sidecar. The sidecar is the batch-inspection surface agents read after `asset.dump` or `asset.dump_folder`.

Implementation checklist (new IRs — registry route):

1. Add a canonical filename such as `<ir>.txt` to `DumpFileNames` in `AssetDumpHandler.h`.
2. Add the filename to the canonical diff/baseline list in `AssetDumpHandler.cpp`.
3. Register the dual-surface builder via `REGISTER_DECOMPILE_IR(SpecName, FileName, ClassThunk, BuildFn, Priority)` in `Private/Utils/IrSidecarRegistry.h` from the matching `Handlers/<Domain>/<IR>DecompileHandler.cpp`. One registration wires both the sidecar drain (`AssetDumpHandler::AddRegisteredIrSidecarFiles`) and the live RPC to the same `BuildFn` — no dispatch-branch edit in `AssetDumpHandler.cpp` is required. See [arch.md](arch.md#ir-sidecar-registry-dual-surface-pattern) for the registry mechanism; SCIR, BTIR, MSIR, and NIR are current consumers.
4. Emit the sidecar only when the builder succeeds and returns non-empty text.
5. If the IR aspect fails, record an aspect diagnostic and keep the rest of the asset dump successful.
6. If the whole asset fails to dump, rely on the existing skip-stub behavior: the folder contains `meta.json` with `skipped: true` and `skipReason`.

BPIR, MGIR, and AGIR predate the registry and still ship through explicit dispatch branches in `BuildAllFilesForAsset`. Do not copy that pattern for new IRs — go through the registry.

Current `meta.json` does not carry a generic `aspects` array. File presence is the aspect declaration for IR text. Add an IR-specific status field only when consumers need to distinguish meaningful empty, unsupported, and error states that filename presence cannot express; follow the small `propertiesStatus` shape (`status`, optional `reason`) rather than embedding IR text in metadata.

Document the sidecar in [wiki-src/asset.md](wiki-src/asset.md) when the schema becomes public-facing.

## Dump And RPC Dual Surface

Every IR text output must ship both surfaces:

- Dump sidecar: `<ir>.txt` through `asset.dump` / `asset.dump_folder`.
- Live RPC: `<namespace>.decompile_<ir>` under `Private/Handlers/<Domain>/`.

Both surfaces must call one shared builder/decompiler path. The dump branch and RPC handler are thin adapters around that path.

New live decompile RPCs should expose the shared builder text as `ir` with a `warnings` array; adding `text` as an alias is acceptable when matching older decompile RPC conventions. Do not create an IR-specific field name unless an existing public RPC already owns that shape.

Preferred shape:

```cpp
struct FIrBuildResult
{
    bool bSuccess = false;
    FString Text;
    TArray<FString> Warnings;
};

FIrBuildResult BuildXxxIrText(UAssetType* Asset, const FXxxIrBuildOptions& Options);
```

If output needs to vary between dump and RPC, route that through an options struct. Do not create two emitters, two walkers, or two normalization paths. Drift between dump and live read is a bug because agents use the dump cache for broad reasoning and the RPC for read-after-mutate truth.

Current examples are close but not perfectly uniform: BPIR has `AssetDumpBuilder::BuildBpirText` for dump formatting plus live `blueprint.decompile`; MGIR and AGIR call their decompilers from both asset-dump and live handlers via explicit dispatch branches in `AssetDumpHandler.cpp`; SCS text is a dump-only companion to the existing `blueprint.scs.get` JSON surface. SCIR, BTIR, MSIR, and NIR use the newer `REGISTER_DECOMPILE_IR` macro (see [arch.md](arch.md#ir-sidecar-registry-dual-surface-pattern)) so one declaration wires both surfaces to the same builder. New IRs should use the registry and make the shared builder explicit from the start.

## Testing Patterns

Pick the smallest test shape that matches the IR maturity.

| Pattern | Use for | Counterfactual |
|---|---|---|
| Full round-trip | Mature mutable IRs where compile and decompile are both contractual, as with BPIR. | Reverting compiler or decompiler normalization changes the second decompile or breaks compile. |
| Golden decompile | Stable decompile text where compile coverage is partial or expensive. | Reverting emitter order, quoting, or field formatting changes the snapshot. |
| Decompile-only | Read-first or immutable native graphs where compile is unsupported or intentionally partial, as with early AGIR graph families. | Reverting the walker or emitter drops expected nodes, fields, or warnings. |
| Asset-dump parity | Any IR sidecar with a live RPC. | Reverting the shared builder causes dump text and RPC text to diverge. |

Test locations follow the current tree:

- BPIR compiler/decompiler behavior: `Private/Tests/Bpir/`.
- Asset and sidecar behavior: `Private/Tests/Assets/` or `Private/Tests/Utility/`.
- IR-local fixtures for new shared helpers: `Private/Tests/IrCore/`.

Do not add backwards-compatibility tests for old grammar. Replace the fixture or golden when the grammar changes.

## Existing IR Examples

- [BPIR language reference](wiki-src/bpir.md) - full bidirectional Blueprint graph IR syntax.
- [BPIR examples](wiki-src/bpir.examples.md) - snippets by node kind and workflow.
- [BPIR compiler internals](bpir-compiler-internals.md) - compiler, decompiler, type, and rollback internals.
- [CRIR language reference](crir-language-reference.md) - Control Rig IR (Phase A): RigVM graph + decompile-only rig hierarchy.
- [material.mgir wiki overlay](wiki-src/material.mgir.md) - MGIR live workflow and decompile/compile surface.
- [controlrig wiki overlay](wiki-src/controlrig.md) - CRIR live workflow surface for `UControlRigBlueprint`.
- [asset wiki overlay](wiki-src/asset.md) - asset dump sidecar schemas, including `bpir.txt`, `mgir.txt`, `agir.txt`, and `crir.txt`.
- [animation.authoring wiki overlay](wiki-src/animation.authoring.md) - animation authoring surface adjacent to AGIR.
- [AGIR source](../Source/PinWright/Private/AGIR/) - current AGIR parser, emitter, compiler, and decompiler implementation.
- [CRIR source](../Source/PinWright/Private/CRIR/) - current CRIR parser, emitter, compiler, decompiler, and pin resolver.

AGIR does not yet have a dedicated maintainer reference page. Until it does, source plus the animation authoring overlay are the authoritative cross-link targets.
