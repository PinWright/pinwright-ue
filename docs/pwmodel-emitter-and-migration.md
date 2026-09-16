---
type: system
summary: Plan for the .pwmodel emitter, the shared PwText core in the main PinWright module, the multi-line bracketed-list grammar change, the model.format verb, and the version/migration machinery
date: 2026-08-26
tags: [pwmodel, emitter, format]
---

# pwmodel emitter, shared text core, and version machinery

## Decisions

1. **The shared core lives in the main `PinWright` module, not in `PinWrightGeometry`.** `PinWrightGeometry` is a gated sub-module that depends on `PinWright` (`Source/PinWrightGeometry/PinWrightGeometry.Build.cs:66-70`); animation handlers live in the main module (`Source/PinWright/Private/Handlers/Animation/`). Dependency runs sub-module → main, never back. A core hosted in `Private/Model/` is unreachable from `.pwanim`. Target: `Source/PinWright/Public/PwText/` + `Source/PinWright/Private/PwText/`, `PINWRIGHT_API`, exactly the `FIrTextUtils` precedent (`Source/PinWright/Public/IrCore/IrTextUtils.h:19`) that `PwModelTokenizer.cpp:5` already includes across that boundary.
2. **Diagnostic codes are injected, not prefixed.** The shared tokenizer takes an `FPwLexCodes` struct of `const TCHAR*` supplied by the format. Runtime prefix-concatenation would move the four lexical code literals out of `Source/PinWright*/Private/Model/` and trip `TestPwModelDiagnosticCatalog.cpp`, whose scan roots are discovered as `Source/PinWright*/Private/Model/` (`:64-87`) and which fails on "registered but never emitted".
3. **Comments and blank lines are captured as a side-channel trivia array, not as tokens.** A new token type would have to be skipped at every `Peek`/`Check`/`Advance` site in a 2902-line parser; a parallel array keyed by line costs the parser nothing.
4. **Bracketed lists gain the right to span lines; tuples do not.** This is a grammar change and it must land *before* the emitter's layout, or the layout is designed twice. Justification is measured, not aesthetic (§3).
5. **Numbers are emitted shortest-round-trip (`%.15g` → `%.16g` → `%.17g`, first that re-reads bit-identically), never `FString::SanitizeFloat`.** `SanitizeFloat` formats through `%f` and trims (`String.cpp.inl:1247-1272`): it maps `1e-8` to `"0"` and `0.1234567` to `"0.123457"`.
6. **The emitter reorders parameters within a statement and nothing else.** Parameters live in a `TMap` (`PwModelAst.h:47`) so source order is already lost. Every other sequence is an ordered array whose order is load-bearing: material bindings *are* the asset's slot order (`PwModelAst.h:85-86`), parts merge in order, ops replay in order, collision elements are emitted in order. Sorting any of them is a semantic change wearing a formatter's clothes.
7. **`model.format` refuses a document that parsed with any Error-severity diagnostic**, and never changes the version number.
8. **The migrator's vocabulary half ships now; its version-branch half is a seam with nothing to put in it** (§7). That is a technical statement about there being one version, not a deferral.
9. **An emitter is not a decompiler.** Text → AST → text. There is no mesh → text and there never will be (`docs/pwmodel-design.md:21-23, 36-37, 210-212`). All formats here are export-only; nothing reads an existing asset back into source. The emitter's arrival must not be cited as evidence a decompiler is near.

---

## §0 What the brief got right, and what has moved

| Claim | Verdict | Evidence |
|---|---|---|
| No `model.format` verb | **True** | `ModelCompileHandler.cpp` registers exactly `model.compile:412`, `model.validate:481`, `model.describe_ops:546` |
| `pwmodel 0` carries no compatibility promise; no migration machinery | **True** | `PwModelParser.cpp:1345-1353` — `PwModelAcceptedVersions = TEXT("0")` and `PwModelIsAcceptedVersion` are file-static in an anonymous namespace, with a comment saying so; `docs/pwmodel-format.md:1336-1347` |
| Op table is `BuildOpTable()` at `:200-628`, **71 entries** | **Stale** | `BuildOpTable` is `PwModelParser.cpp:367-1105`. **74 entries** (68 part, 6 collision). 71 was correct at `HEAD` (`1ac056d9`); the uncommitted tree adds 3 |
| `frame_list` is a new **value type** | **Imprecise, and the imprecision matters** | It is a *parameter* type (`EPwModelParamType::FrameList`, `PwModelParser.h:60`). At the value level it is an ordinary `TupleList` (`PwModelAst.h:23`). The emitter therefore prints it from the value kind and cannot special-case it |
| Three new ops `sweep`, `extrude_along_spline`, `array_along_path` | **True, exactly those three** | Op-name set diff `HEAD` → working tree: +3, −0. Specs at `PwModelParser.cpp:966-1005` |
| "parameter widening across ~21 ops" | **Understated, and one-directional** | **25 ops widened**; `remesh_uniform` +17, `uv` +18, `simplify_mesh` +14. **3 ops narrowed** — `bridge` lost `subdivisions`, `recalculate_normals` lost `split_angle`, `trim` lost `keep_inside`. Removals are breaking and the brief did not mention them |
| `PWMODEL_BOOLEAN_NO_EFFECT` raised to an error that writes no asset | **True** | `PwModelDiagnostic.h:140-151` — "It aborts the part, so nothing is created", and the history note naming the twelve-example incident |
| `lightmap resolution=` | **True** | `PwModelParser.cpp:333-341`, range [4, 4096] |
| Tokenizer discards `#` comments, preserving line counting | **True** | `PwModelTokenizer.cpp:93-103` |
| Examples carry load-bearing multi-paragraph header comments | **True, and larger than stated** | 12 files, 2609 lines, **1651 full-line comments (63%)**. `spur_gear.pwmodel:1-40` derives the gear from ISO relations; `chess_rook.pwmodel:33-53` records a z-fighting bug and its fix in prose |

Two further measurements that shape the design:

- **Model-level construct order is unanimous across all 12 examples**: `pwmodel → materials → part* → [lightmap] → collision`. Adopting it as canonical reorders nothing in the corpus.
- **Op coverage is 39 of 70 distinct op names (56%).** 31 op names appear in no example, including all three added today. This is the load-bearing fact for §8.

---

## §1 Scope, and where this sits in the dependency order

The emitter is upstream of the sibling sections. Concretely: the value-system agent's nested list kind, the `.pwanim` agent's whole document, and the skeleton/skin agent's new block types all land on the tokenizer/trivia/value/emitter surfaces defined here. **This section defines the shared core's shape.** Three commitments the siblings should plan against:

- **To the value-system agent:** the shared value type must carry `TArray<FPwTrivia> ElementTrivia` (per-element, indexed by element position). Without it, a comment written inside a wrapped multi-line list is unrepresentable and the only sound alternative is to reject it (§3.3). Also: whatever the nested kind's in-memory shape, the emitter needs a uniform recursive accessor (`NumElements()` / `ElementAt(i)`) rather than a switch per kind, or the line-breaking rule has to be written once per kind.
- **To the `.pwanim` agent:** do not write a tokenizer. Consume `FPwTokenizer` with your own `FPwLexCodes` and `FPwFormatProfile`. Your document's top-level construct set and canonical order are two entries in that profile.
- **To the skeleton/skin agent:** every new block type you add to the pwmodel AST must carry `Line` **and** `EndLine` (§4.1). A block with only an opening line has no upper bound for trivia attachment, and comments inside it are silently deleted.

---

## §2 The shared core: what is generic, what is per-format

### Generic — `Source/PinWright/Public/PwText/`

| File | Contents | Derived from |
|---|---|---|
| `PwToken.h` | `EPwTokenType`, `FPwToken` | `Model/PwModelToken.h`, verbatim rename |
| `PwTrivia.h` | `EPwTriviaKind { Comment, BlankLine }`, `FPwTrivia { Kind, Text, Line, Column, bOwnLine }` | new |
| `PwDiagnostic.h` | `EPwSeverity`, `FPwDiagnostic`, `FPwLexCodes` | `Model/PwModelDiagnostic.h:15-19, 216-319` |
| `PwTokenizer.h/.cpp` | `FPwTokenizer::Tokenize(Source, Codes, OutTokens, OutTrivia, OutDiagnostics)` | `Model/PwModelTokenizer.cpp` verbatim + trivia capture |
| `PwValue.h` | `EPwValueType`, `FPwValue` (owned jointly with the value-system agent) | `Model/PwModelAst.h:18-40` |
| `PwNumberFormat.h/.cpp` | `PwFormatNumber(double)` | new |
| `PwVersion.h` | `FPwVersionSpec { Keyword, MinReadable, Current, AcceptedText }`, `PwIsAcceptedVersion` | `PwModelParser.cpp:1345-1353`, lifted |
| `PwEmitter.h/.cpp` | trivia attachment, indentation, blank-line normalisation, value printing, list wrapping, statement/block printing, idempotence contract | new |
| `PwFormatProfile.h` | the per-format descriptor below | new |

### Per-format — supplied through `FPwFormatProfile`

- Format keyword and `FPwVersionSpec`.
- `FPwLexCodes` (Decision 2).
- **Parameter ordering oracle**: `TFunctionRef<const TArray<FString>*(FStringView StatementName, int32 ContextId)>` returning the declared parameter order. pwmodel wires this to `PwModelOpTable::Find(Name, Context)->Params`. One table, no second copy.
- **Top-level construct order**: an ordered list of construct kind ids. pwmodel: `use, materials, part, lightmap, collision, reserved`.
- **Layout exceptions.** pwmodel has two, and both are real grammar, not style:
  - The part header is the one place the grammar is not `keyword params` on a line — name, then params, then `{`, all on one line (`docs/pwmodel-format.md:158-163`; parser at `PwModelParser.cpp:2163-2190`).
  - `materials` and `complexity` use an assignment form `Name = "value"` with spaces, where ops use `key=value` without (`docs/pwmodel-format.md:85-87, 105`).
- **Immutable-order arrays** (Decision 6), declared so the emitter cannot be asked to sort one.
- **Migration rule table** (§7).

Everything above the profile is format-agnostic; the profile is the only thing `.pwanim` writes.

---

## §3 Grammar change: bracketed lists may span lines

### 3.1 Why this is not optional

The grammar forbids it today: `docs/pwmodel-format.md:126` ("A tuple or list may not span lines") and the parser's two guards, `PwModelParser.cpp:1516-1520` (tuple) and `:1569-1573` (list).

Measured against the corpus: 2609 lines, longest **467 characters** (`amphora.pwmodel:41`), then 348, 221, 201, 186 — all five are `revolve profile=[…]` or equivalent. Every other non-comment line is under ~100. Git diffs are line-granular, so editing one control point in a 30-point profile presents as a 467-character whole-line replacement. That directly defeats the stated purpose — "a canonical formatter, so diffs stay stable, and a stable diff is the entire review mechanism for a git-tracked source" (`docs/pwmodel-design.md:204-205`).

The nested list-of-lists kind makes it strictly worse: a `loft` over 4 profiles × 20 points is ~1500 characters on one line. Landing that kind on top of a single-line-only list grammar ships an unreviewable construct on day one.

`pwmodel 0` carries no compatibility promise (`docs/pwmodel-format.md:1336-1338`), so this is the cheapest this change will ever be.

### 3.2 The change

- Tokenizer: **no change**. It already emits `Newline` tokens.
- `ParseValue`, `OpenBracket` arm (`PwModelParser.cpp:1567-1600`): replace the `Check(Newline) → error` guard with "skip `Newline` tokens" at three points — before an element, after a comma, before `]`.
- Tuple guard at `:1516-1520` **stays an error**. Tuples remain atomic; a point is one line.
- **Cost with a known remedy**: a missing `]` no longer terminates at end of line, so the error surfaces at EOF. Anchor the diagnostic to the `[` token's line, the way `PWSRC_UNCLOSED_BRACE` is anchored to the opening brace (`PwModelDiagnostic.h:55-57`). Add `PWMODEL_UNCLOSED_BRACKET` to the registry and to the `docs/pwmodel-format.md` diagnostics table in the same change, or `TestPwModelDiagnosticCatalog` fails in both directions.

### 3.3 Line-breaking policy

**The break decision is computed from the hypothetical single-line rendering, never from the input's current layout.** That is what makes it idempotent and independent of whether the input arrived wrapped.

```
width(statement_indent + "key=" + one_line_render(value)) <= 100  →  one line
otherwise                                                          →  wrapped
```

Wrapped form: `key=[` on the statement line, one element per line at `indent+1`, `]` at the statement's indent. Recursive: an inner list that fits on its line stays inline, otherwise the same rule applies one level deeper.

Budget of 100 is chosen from the corpus, not by taste: it leaves every existing non-list line byte-identical and breaks exactly the five offenders.

**Comments inside a wrapped list.** A `#` between two control points has no AST node to attach to — a tuple is not a node. Two sound options, in order of preference:

1. `FPwValue` carries `TArray<FPwTrivia> ElementTrivia` with an element index. The emitter re-emits each above its element, and the presence of any element trivia forces the wrapped form (a comment cannot be inline). This is the coordination request to the value-system agent in §1.
2. If (1) is refused, the parser must **reject** a comment inside a bracketed value with a diagnostic. It must not silently accept and drop it.

There is no third option. Accepting and dropping is the failure mode this whole section exists to prevent.

---

## §4 Emitter design

### 4.1 Trivia capture and attachment

**Capture.** `FPwTokenizer` gains an `OutTrivia` array. For `#`, record everything *after* the `#`, verbatim, trailing whitespace stripped, plus `Line`, `Column`, and `bOwnLine` (true when only whitespace precedes it on the line). For a blank line, record a `BlankLine` entry. Storing the payload verbatim is load-bearing: comment bodies carry indented ASCII tables and derivations (`spur_gear.pwmodel:11-17`, `chess_rook.pwmodel:9-12`). The emitter re-indents by rewriting the whitespace *before* the `#` and never touches a character after it.

**Prerequisite AST change.** `FPwModelPart`, `FPwModelOp` and `FPwModelCollision` record only their opening `Line` (`PwModelAst.h:51-52, 68-69, 106-107`). Add `EndLine` to each, plus a document-level `EndLine`. The parser already consumes the closing brace in `ForEachBlockEntry` and can record it in one line each. Without `EndLine`, trivia between a block's last child and its `}` has no upper bound: it either leaks into the next construct's leading block at the wrong indentation, or is dropped.

**Attachment** — pure line arithmetic, no heuristics:

| Bucket | Rule |
|---|---|
| Trailing | `Comment.Line == construct.Line && Comment.Column > construct.Column` |
| Leading | The maximal trivia run on lines strictly between the previous sibling's `EndLine` and this construct's `Line`. Internal blank lines within the run are **preserved** (they separate paragraphs — `chess_rook.pwmodel:33-53`); leading blanks of the run collapse to at most one |
| Block tail | Trivia between the last child's `EndLine` and the block's `EndLine` |
| Document prologue | Trivia on lines `< Doc.VersionLine` — this is where the multi-paragraph file headers live |
| Document epilogue | Trivia after the last construct's `EndLine` |

**Every trivia record must be consumed exactly once.** `checkf` in a check build; a diagnostic otherwise. The test in §6.2 catches it either way.

### 4.2 Canonical ordering

- **Model level**: the profile's order — `use, materials, part*, lightmap, collision, reserved`. Matches all 12 examples, so the corpus does not move.
- **Parts, ops, collision elements, material bindings**: source order, untouched (Decision 6).
- **Parameters within a statement**: op-table declaration order, with parameters the table does not declare impossible by construction (a clean parse rejects unknown parameters, and §5 refuses to format an unclean one).
- **Part header transform**: `PwModelOpTable::PartHeaderParams()` order — `at, rotate, scale` (`PwModelParser.cpp:355-365`).
- **Collision**: `complexity` first, then elements in source order.

**Measured cost**: 108 of 321 multi-parameter statements (33%) get their parameters reordered on the first format pass. The op table appends `at/rotate/scale`, then `material`, then `color` to every generator (`PwModelParser.cpp:143-153`), so those trail naturally — that part reads well. The bad case is `revolve`, whose declared order is `profile, angle, steps, capped`, moving a 400-character list to the front of the line. §3 dissolves this: the list becomes an indented block and `angle/steps/capped` sit above it. This is the second reason the grammar change must precede the emitter.

### 4.3 Number formatting

A float that round-trips is not the same as one that reads well, and the two requirements are separable — but only one of them is negotiable.

```
PwFormatNumber(double V):
  V is not finite            -> diagnostic; the grammar has no NaN/Inf literal
  V == 0.0                   -> "0"                 (also normalises -0.0)
  for P in {15, 16, 17}:
      S = Printf("%.*g", P, V)
      if (FCString::Atod(*S) == V) break            // bitwise
  assert S contains no ','                          // locale guard, see below
  normalise exponent: strip '+' and leading zeros   // "1e+06" -> "1e6"
```

- **Legibility**: `0.6` stays `0.6`, `107.5` stays `107.5`, `7.853982` stays `7.853982`. `%.17g` unconditionally would have written `0.59999999999999998` and reformatted essentially every line of the corpus.
- **Exponent form is legal.** The tokenizer accepts `e`/`E`, optional sign, digits (`PwModelTokenizer.cpp:159-191`), so `1e6` and `1e-05` are single Number tokens. `%g` only reaches exponent form below `1e-5` or above `1e15`, so every realistic modelling coordinate prints plain decimal.
- **Locale guard.** `FCString::Atod` is `_tcstod` on Windows (`MicrosoftPlatformString.h:169-172`) and `FString::Printf` routes to `vswprintf`; both are `LC_NUMERIC`-sensitive, and `docs/pwmodel-format.md:1245-1247` already records the parse-side hazard. They are sensitive *together*, so the round-trip check passes while the emitter writes `0,6` — which the tokenizer then lexes as two numbers with a comma between them, self-consistently and catastrophically. UE does not `setlocale` outside a save/restore pair in `ConsoleManager.cpp:69-82`, and `CharTest.cpp:36-43` asserts the process locale is `C`, but a host plugin can change it. One assertion covers it.
- Integers need no special case: `%.15g` of `48.0` is `48`.

### 4.4 Layout

- Indent: 4 spaces per depth. Matches every example and the doc.
- `key=value` with no spaces; `Name = "value"` with single spaces for `materials` entries and `complexity`. **No `=` alignment.** Alignment is a deterministic function of block contents so it would still be idempotent, but a one-line edit reflows N lines of diff, which is the opposite of the goal. Cost: 2 lines of `chess_rook.pwmodel` and the 4 aligned trailing comments in `origami_crane.pwmodel:149-152` lose their column alignment.
- Blank lines: at most one anywhere; exactly one before a model-level construct that carries a leading comment block. Idempotent by construction.
- Tuples: `(a, b, c)` — comma-space.
- Trailing newline: always exactly one.
- **Newline style: match the input file.** `.gitattributes` has no `.pwmodel` entry, so the examples fall under `* text=auto` and are checked out CRLF on Windows (verified: all 12 are 100% CRLF). `.gitattributes:6-9` states the rule directly — *"Scripted edits must therefore read a file's existing newlines rather than assume either style."* An emitter that normalises to LF turns every format into a whole-file diff. In `text` mode, default to `\n`; in `filePath` mode, detect and reproduce. Consider adding `*.pwmodel text` to `.gitattributes` in the same change, which makes the index shape explicit before the examples are committed.

### 4.5 Staying in lockstep with the op table

Single mechanism: the emitter reads parameter order through the profile's ordering oracle, which pwmodel wires to `PwModelOpTable::Find(...)->Params`. There is one table; `model.describe_ops` already emits it verbatim (`ModelCompileHandler.cpp:546`) and the parser validates against it (`PwModelParser.h:11-14`).

Drift is then only possible if the emitter can meet a parameter the table does not declare. That requires an unknown op or an unknown parameter, both of which are Error diagnostics — which §5 refuses to format. So drift is structurally excluded rather than tested for. The test in §6.3 pins it anyway, because "structurally excluded" is a claim.

---

## §5 The `model.format` verb

```
model.format
  text      string   inline source; exactly one of text | filePath   (mirrors model.validate:481)
  filePath  string   .pwmodel path; relative resolves against ProjectDir
  write     boolean  default false; only legal with filePath
```

Response: `formatted` (the text, always, so a caller can diff without writing), `changed` (bool), `diagnostics` (the parse's, including warnings), and in `write: true` mode `bytesWritten` plus whether the file on disk actually moved — not whether a write was attempted.

Rules, each earned from `docs/rpc-design.md`:

- **Refuses a document with any Error-severity diagnostic** (`PwModelDiagnosticsHaveError`, `PwModelDiagnostic.h:297`), reporting `ERR_MODEL_PARSE_FAILED` (already registered, `ErrorCodes.h:719`). Formatting a broken file must either drop the broken region or guess at it; both are silent data loss. Warnings format fine — a document using the deprecated `max_hulls=` still formats.
- **Never rewrites the version header.** It emits `Doc.Version`. A formatter that silently bumps a version is a formatter that silently changes semantics, and the diff cannot show why. Today `Current == 0 == every document's version`, so the two agree — which is precisely why the rule must be written now rather than discovered at the bump.
- **`write: true` compares bytes and reports honestly.** A no-op format must report `changed: false` and touch nothing. `docs/rpc-design.md:30, 37` — report what happened, and never a value the write path can fake.
- Reuses `ModelHandler_ResolveSourcePath` / `ModelHandler_LoadSourceFile` (`ModelCompileHandler.cpp:56, 103`) so path resolution cannot diverge from `model.compile`.
- Needs a `### model.format` H3 in `docs/wiki-src/model.md`, placed after the existing method sections; and `docs/wiki-src/model.authoring.md` should point at it as the pre-commit step.

---

## §6 Tests — and specifically what makes them able to fail

The design record names one test: `emit(parse(emit(parse(x)))) == emit(parse(x))`. **That test alone cannot fail for the defect that matters most.** A formatter that rounds every number to two decimals is *perfectly idempotent* — it converges after one pass. Idempotence pins textual stability; it says nothing about value fidelity. Two properties are needed, and only the second catches a lossy emitter.

### 6.1 Idempotence — `PinWright.Model.Emitter.FormattingIsAFixedPoint`

`emit(parse(emit(parse(x)))) == emit(parse(x))`, over the corpus of §6.5. Failure direction: a control case that asserts a deliberately unstable variant (e.g. an emitter that alternates blank-line policy) is *not* a fixed point, so the harness is shown to be able to detect non-convergence.

### 6.2 Comment conservation — `PinWright.Model.Emitter.NoCommentIsDropped`

The multiset of comment payloads in `emit(parse(x))` equals the multiset in `x`, compared after trailing-whitespace trimming and nothing else. This assertion is over the **input**, which the emitter did not produce, so it cannot be satisfied by the emitter agreeing with itself. Delete any comment and it fails; move one to the wrong block and the ordered variant of the same assertion fails. Assert order too, restricted to own-line comments, whose relative order is total.

### 6.3 Value fidelity — `PinWright.Model.Emitter.EveryValueSurvivesARoundTrip`

`parse(emit(parse(x)))` and `parse(x)` compared field by field over the whole AST: every `FPwValue`'s `Number` **bitwise**, every `Tuple`/`TupleList` element bitwise, every `Text` byte-identical, every op name, parameter key set, child order, part order, material binding order. This is the assertion the design record's single test omits, and it is the one that fails on a lossy number formatter.

### 6.4 Number formatter — `PinWright.Model.Emitter.NumbersRoundTripBitwise`

Adversarial table: `0.1, 0.3, 0.6, 1/3, 1e-5, 1e-8, 1e300, 1e-300, 2^53, 2^53+2, DBL_MIN, DBL_EPSILON, -0.0, 0.1234567`, plus the four hand-derived constants from `spur_gear.pwmodel:31-34` (`0.3639702, 0.3420201, 0.9396926, 7.853982`). Assert `Atod(PwFormatNumber(v)) == v` bitwise, and that the emitted text contains no `,`.

**The negative control is what makes this test evidence.** Assert in the same test that `FString::SanitizeFloat` *fails* the same table — it maps `1e-8` to `"0"` and `0.1234567` to `"0.123457"`. Without the control, a formatter that happened to be a no-op on all fourteen values would read green.

### 6.5 Corpus — table-driven, cannot go stale

Two sources, both generated:

1. **The 12 shipped examples.** Real comment density (63%), real long lists, real nesting.
2. **A synthesized document per op**, extending `ModelHandlersTest_BuildDocumentForOp` (`TestModelHandlers.cpp:344`). That helper today emits **only required parameters** (`:363-365`). For the emitter it must emit **every declared parameter**, the way the `paramSets` sibling already does (`:421-424`). A parameter the emitter cannot print then fails loudly instead of never being exercised. Because the corpus is derived from `PwModelOpTable::Get()`, an op added tomorrow is covered tomorrow — the same anti-drift idiom `model.describe_ops` uses.

Guard the empty denominator: assert the generated corpus is non-empty and its op count equals `PwModelOpTable::Get().Num()`. `docs/rpc-design.md:341` — a check that examined nothing must not print a pass.

### 6.6 The property none of these covers

Nothing above proves the *mesh* is unchanged. Value fidelity (§6.3) is the strongest available static proxy and it is genuinely strong, because the compiler reads values by name out of the AST. A compile-and-compare over the corpus would be stronger but inherits the reproducibility caveats already catalogued at `docs/pwmodel-format.md:1152-1250`. Recommendation: add one compile-parity test over 2-3 examples asserting triangle/vertex/slot/collision counts match before and after formatting, and state in its comment that it is a spot check, not the guarantee.

---

## §7 Version and migration machinery

### 7.1 Is a `0 → 1` migrator vacuous today? The version-branch half is. The vocabulary half is not.

The two halves have been conflated, and separating them is the whole answer.

**Version branching is genuinely vacuous.** There is exactly one accepted version (`PwModelParser.cpp:1350-1353`). A branch would be `if (Version == 0) { … }` with no `else`. Writing the `else` requires inventing a second grammar, which is maintaining a mechanism against a hypothesis — the format doc's own words at `docs/pwmodel-format.md:1345-1347`. What ships is the **seam**, not the branch: lift `PwModelAcceptedVersions` / `PwModelIsAcceptedVersion` out of the anonymous namespace into `FPwVersionSpec { Keyword, MinReadable, Current, AcceptedText }` in `PwText/PwVersion.h`, so that the day a bump happens there is one place to widen and it is shared across formats. `MinReadable == Current == 0` today; that is a fact, not a placeholder.

**Vocabulary migration is not vacuous, and has real inputs on disk today.** Three of them:

1. `max_hulls` → `max_hulls_per_component` is a live rename with a deprecation warning implemented ad hoc at `PwModelCollision.cpp:438-464`, and **two shipped examples still use the old spelling** (`crystal_cluster.pwmodel:155`, `ships_wheel.pwmodel:219`).
2. Three parameters were **removed** within version 0 — `bridge subdivisions`, `recalculate_normals split_angle`, `trim keep_inside`. A document written last week against them now hard-fails with `PWSRC_UNKNOWN_PARAM`. `ships_wheel.pwmodel:131-132` still documents one of them as a live-but-ignored parameter.
3. Three sibling sections in this same plan will change the vocabulary again.

So the machinery has work now. It is *intra*-version-0 compatibility, which is exactly the shape a format with no compatibility promise has.

### 7.2 What ships

**A rule table, applied to the AST, owned by the format profile.**

```cpp
enum class EPwMigrationKind : uint8 { RenameOp, RenameParam, DropParam, RewriteValue, RelocateStatement };

struct FPwMigrationRule
{
    int32              FromVersion;   // 0 today for every rule
    int32              ToVersion;     // 0 today for every rule
    EPwMigrationKind   Kind;
    const TCHAR*       OpName;        // nullptr = any
    const TCHAR*       Old;
    const TCHAR*       New;           // nullptr for DropParam
    const TCHAR*       Note;          // the exact text of the warning, one copy
};
```

Rules apply in ascending `(FromVersion, table order)`. Three rows exist on day one: the `max_hulls` rename and the three parameter drops.

Two properties this buys immediately:

- **The emitter can apply it.** `model.format` on `ships_wheel.pwmodel` rewrites `max_hulls=2` to `max_hulls_per_component=2` and the deprecation warning stops. That is a migrator, running today, on files that exist. It is exactly "parse with old, emit with new" — the design record's own definition (`docs/pwmodel-design.md:207`) — with the version integer held constant.
- **One place to look.** The `max_hulls` shim is currently 25 lines of policy inside the collision builder. Lifting it to a row means the next deprecation is a row rather than another 25 lines somewhere else.

**The rule that keeps this honest**: `model.format` applies rules only when asked. Default off; `migrate: true` opts in and the response reports each rule that fired. A formatter that silently renames a parameter is the same defect as one that silently bumps a version.

### 7.3 What "old parser paths are kept" costs after two or three bumps

This is the question the design record leaves open, and the answer determines which of two architectures gets built.

**Forked-parser reading** (copy `PwModelParser.cpp` per version): after three bumps, ~8700 lines of parser across three files, three op tables, and `model.describe_ops` must choose one to publish — which breaks its single-vocabulary property (`PwModelParser.h:11-14`) at the root. Every op added must be added to one table and *deliberately not* to two others. This is the rot, and it is not hypothetical: it is what the sentence literally proposes.

**Data-rule reading** (what this plan builds): the op table always describes `Current` only. Old names live in the migration table, never in a second op table. `describe_ops` stays single-valued. A bump costs one table block, N rule rows, and one test per rule.

The residue the rule table cannot absorb is *grammar*-shaped change — a new statement form, a new value kind, a changed brace structure. Those do need a live branch in the parser. Bound them explicitly:

> **Cap: at most two live grammar branches.** When a third would be needed, `MinReadable` advances and the oldest grammar is dropped — a document at that version must be run through the previous release's formatter first. State this in `docs/pwmodel-format.md § Version policy` **before** the first bump, because after the bump it reads as breaking a promise.

### 7.4 Per-format or shared?

**Shared**: the header statement shape (`<keyword> <int>` as the first non-trivia line), `FPwVersionSpec`, the accepted-version check, the missing/unsupported-version diagnostics, the rule-application engine, and the grammar-branch cap as a policy. **Per-format**: the keyword, the numbers, and the rule table. `.pwanim` starts at 0 with the same shape, and "what does a bump cost" is answered once for every format.

---

## §8 What must settle before `1` is justified

The user has held pwmodel at 0 through this plan, so this is the trigger list, not a request to bump.

1. **The vocabulary must stop moving.** It moved today: +3 ops, 25 widened, **3 narrowed**. A narrowing is breaking, and `docs/pwmodel-format.md:1338-1339` reserves the integer bump for exactly that. Three sibling sections in this plan will move it again.
2. **Coverage.** 39 of 70 op names are exercised by the corpus; 31 are not, including all three added today. "Real models have exercised it" (`docs/pwmodel-format.md:1338`) is measurable, and the measurement is currently 56%.
3. **The corpus must be clean under the current table.** Two examples still use the deprecated `max_hulls=`; one documents a parameter that no longer exists (`ships_wheel.pwmodel:131-132`); one carries an explanation of `max_hulls` behaviour that the current implementation contradicts (`crystal_cluster.pwmodel:149-152`). Re-validate all 12 and regenerate through the formatter.
4. **Severity churn must stop.** `PWMODEL_BOOLEAN_NO_EFFECT` became an Error yesterday and invalidated three of twelve examples in one commit (`1ac056d9`, "Fix the three .pwmodel examples the boolean traps broke"). A severity change that breaks a quarter of the corpus is the signature of a grammar still settling.
5. **The emitter must exist first** — which is now the plan, and is independently why the design record ranked it highest. Without it, the first migration is done by hand, and a migrator that has never been run on real input is a hypothesis.
6. **The grammar-branch cap (§7.3) must be written into the version policy** before the bump.

Recommended concrete trigger: op coverage ≥ 80% of the table across the example corpus, zero deprecation warnings on a full-corpus format pass, and no vocabulary narrowing for the duration of one release.

---

## §9 Work order, by hard dependency only

| # | Work | Blocked by | Blocks |
|---|---|---|---|
| 1 | Move tokenizer / token / diagnostic / value to `PwText/` under the main module; introduce `FPwLexCodes` populated from the existing `PWMODEL_*` constants; pure behaviour-preserving move | — | 2, and both sibling formats |
| 2 | Trivia capture in `FPwTokenizer` (`Comment`, `BlankLine`, `bOwnLine`) | 1 | 6 |
| 3 | `EndLine` on `FPwModelPart` / `FPwModelOp` / `FPwModelCollision` and the document | — | 6 |
| 4 | `PwFormatNumber` + §6.4 test with its negative control | 1 | 6 |
| 5 | Multi-line bracketed lists: parser guards at `:1569-1573`, `PWMODEL_UNCLOSED_BRACKET`, doc + catalog rows | 1 | 6, and the sibling nested value kind |
| 6 | `FPwEmitter`: trivia attachment, indentation, blank-line normalisation, value printing, list wrapping, statement/block printing | 2,3,4,5 | 7,9 |
| 7 | `FPwFormatProfile` for pwmodel: ordering oracle over `PwModelOpTable`, top-level order, the two layout exceptions, immutable-order declarations | 6 | 8,9 |
| 8 | `model.format` verb + wiki H3 + `docs/wiki-src/model.authoring.md` pointer | 7 | — |
| 9 | `FPwMigrationRule` table + application; three day-one rows; `migrate:` flag on `model.format` | 6,7 | — |
| 10 | `FPwVersionSpec` lifted out of the anonymous namespace into `PwText/PwVersion.h` | 1 | shared with `.pwanim` |
| 11 | Emitter test suite §6.1-6.6; widen `ModelHandlersTest_BuildDocumentForOp` to emit every parameter | 6,7 | — |
| 12 | Regenerate the 12 examples through the formatter; `*.pwmodel text` in `.gitattributes` | 8,9 | — |

Items 4 and 10 have no blocker and can start immediately. Item 1 is on the siblings' critical path even though the emitter could technically be written before it — doing it first means trivia (item 2) is added once rather than twice.

---

## §10 Risks

- **Item 1 is a move across a module boundary in a tree with 2183 uncommitted lines in `Private/Model/`.** Land it as a pure move with no behaviour change, verified by an unchanged test count, before anything else touches those files.
- **The catalog test is fragile to this move.** `TestPwModelDiagnosticCatalog.cpp:64-87` discovers scan roots as `Source/PinWright*/Private/Model/`. Decision 2 keeps every `PWMODEL_*` literal inside that tree. If a future change emits a pwmodel code from `PwText/`, the scan roots must widen in the same commit.
- **Item 5 changes the grammar.** Cheap now (no compatibility promise), expensive after a bump. It is sequenced before the emitter's layout for exactly this reason.
- **§4.2's 33% parameter reordering is a one-time, corpus-wide reflow.** Land item 12 as its own commit with no other changes, so the reflow is reviewable as a reflow.
- **Coordination dependency on the value-system agent** for `ElementTrivia` (§3.3). If that field is refused, §3.3 option 2 is the fallback and it must be decided before item 6, not discovered during it.

---

## Bugs found

1. **`TestModelHandlers.cpp:137-139` states a false fact about the tokenizer.** The comment justifies `FString::SanitizeFloat` over `%g` on the grounds that `%g` "would spell a large one as `1e+06`, which the tokenizer does not read as a number." The tokenizer reads it fine: `PwModelTokenizer.cpp:159-191` accepts `e`/`E`, an optional `+`/`-`, and digits, and `docs/pwmodel-format.md:118` documents exponents as part of the Number grammar. The choice of `SanitizeFloat` happens to be right for a different reason (`%g` at default precision 6 would truncate a large bound), but the stated reason is wrong and would mislead the next author into believing exponent literals are unsupported.

2. **`ships_wheel.pwmodel:131-132` documents a parameter that no longer exists.** It says "`recalculate_normals split_angle=` is accepted and ignored." The working tree removed `split_angle` from `recalculate_normals`, so that spelling is now `PWSRC_UNKNOWN_PARAM` — rejected, not ignored. The comment is the only place in the corpus that records this behaviour and it is now inverted.

3. **`crystal_cluster.pwmodel:149-152` contradicts the current implementation.** It reads "max_hulls is a CEILING THAT IS NOT HONOURED — the previous version asked for 12 and the compiled asset carried 66 elements". The current code explains the same observation correctly as a *per-connected-component* budget (`PwModelCollision.cpp:416-431`) and warns about it explicitly. The example's explanation reads as an engine defect where the behaviour is now understood and documented.

4. **Two shipped examples emit a deprecation warning on every compile.** `crystal_cluster.pwmodel:155` and `ships_wheel.pwmodel:219` use `auto ... max_hulls=`, which triggers the "old spelling ... Rename the parameter" warning at `PwModelCollision.cpp:458-463`. The example corpus is the reference documentation for the format; it should not demonstrate the deprecated spelling.

5. **Three op parameters were removed within version 0 with no deprecation path** — `bridge subdivisions`, `recalculate_normals split_angle`, `trim keep_inside`. Contrast `max_hulls`, which got a warning and continued acceptance. Documents written against last week's vocabulary now fail with `PWSRC_UNKNOWN_PARAM` and no message saying the parameter was removed rather than misspelled. Version 0 permits this; the inconsistency with the `max_hulls` treatment is the defect, and §7.2's `DropParam` rule kind is the fix.

6. **`.pwmodel` has no `.gitattributes` entry.** The 12 example files are untracked and currently 100% CRLF. Under `* text=auto` they will be normalised to LF in the index and checked out CRLF on Windows. Once `model.format` can write files, an emitter that assumes either style produces whole-file diffs on the other. `.gitattributes:6-9` already states the rule for scripted edits; the `.pwmodel` pin is missing.

7. **`FPwModelPart` / `FPwModelOp` / `FPwModelCollision` record no block end position.** Only the opening `Line`/`Column` (`PwModelAst.h:51-52, 68-69, 106-107`). This is invisible today because nothing needs block extents, but it means the AST cannot express "inside this block" as a line range — which any trivia-preserving consumer, and any future source-range diagnostic ("this part spans lines 32-58"), requires.

### Critical Files for Implementation
- Source/PinWrightGeometry/Private/Model/PwModelTokenizer.cpp
- Source/PinWrightGeometry/Private/Model/PwModelParser.cpp
- Source/PinWrightGeometry/Private/Model/PwModelAst.h
- Source/PinWrightGeometry/Private/Handlers/Model/ModelCompileHandler.cpp
- Source/PinWrightGeometry/Private/Tests/Model/TestModelHandlers.cpp
