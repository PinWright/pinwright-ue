# anim

The `anim` namespace contains two unrelated text families. Choose by the source format and the asset type you are targeting.

## AGIR: Animation Blueprint graphs

- `anim.compile_agir` accepts AGIR text and compiles it into a target `UAnimBlueprint`; `anim.decompile_agir` reads one and emits AGIR text.
- This family edits Animation Blueprint graph structure, not animation-sequence key data.
- AGIR compilation is atomic after the target loads: an emission error restores the authored graphs, implemented interfaces, and the package's prior dirty state in both Replace and Extend modes.

## `.pwanim`: animation sequences

- `anim.compile` reads one `.pwanim` file and creates or rebuilds exactly one `UAnimSequence`; `anim.validate` parses and bakes one without writing an asset.
- It accepts inline `text` or `filePath` for validation.
- `anim.describe_ops` reports the `.pwanim` operation and parameter vocabulary.
- The normative format reference is `docs/pwanim-format.md`.

`anim.compile` is NOT the general form of `anim.compile_agir`: use the former for a `.pwanim` file and animation sequence, and the latter for AGIR text and Animation Blueprint graphs. Calling one in place of the other is the wrong operation.

`.pwanim` is sparse: a key may omit `at`, `rotate`, or `scale`. The compiler fills an omitted channel from the target bone's reference pose, never identity. Zero-fill would teleport a keyed non-root bone to its parent's origin and can pass without an error at any layer. See `docs/pwanim-format.md` for the grammar, baking rules, and diagnostics.

## Keep `.pwanim` beside the generated asset

The recommended layout is `Content/<Rel>/<Name>.pwanim` directly beside
`Content/<Rel>/<Name>.uasset`: it makes the mapping obvious, exposes an orphaned source or
unstamped asset, and keeps renames together. Unreal's asset registry recognizes `.uasset` and
`.umap`, not `.pwanim`; typical Git LFS rules are extension-based, so the source remains plain
diffable text under `Content/`.

Following that layout makes `outputPath` optional:
`<ProjectDir>/Content/<Rel>/<Name>.pwanim` derives `/Game/<Rel>/<Name>`. Otherwise pass the
argument explicitly. A non-derivable omission fails with `SOURCE_OUTPUT_PATH_NOT_DERIVABLE`,
names the reason and never guesses. Existing layouts remain supported, and derivation never
bypasses the normal provenance or `overwrite` refusal.

A generated sequence is source-owned. Its provenance stamp records the semantic state from the last
compile. If typed verbs or the Animation Sequence editor changed tracks, timeline, loop, skeleton,
markers, curves, notifies, or retarget source, and the new source would discard that change,
`anim.compile` stops with `PWSRC_RECOMPILE_UNMANAGED_STATE` before resetting the model.
`overwrite: true` permits the loss and returns the same code as a warning. Omitted state is never
silently copied forward; no `sync_marker` statements means an empty marker set.

The guard also runs before taking over an unstamped sequence or one stamped from another source. It
names every live value the incoming source changes or omits. Without `overwrite: true`, the refusal
carries `PWANIM_ASSET_PROVENANCE_CONFLICT`, names the current source when stamped, and includes the
state-loss list; with the flag, takeover succeeds and the list remains a warning.

Validation and compilation resolve the same live skeleton generation. When a skeleton hierarchy
is recompiled, loaded animation assets are refreshed in that editor session. If an animation
compile fails after it starts writing, the previous sequence and package-dirty state are restored,
so a later unrelated save cannot commit a partial compile.

## AGIR sidecar registration

`asset.dump` writes the same AGIR decompile output to `agir.txt`; AGIR is registered as a uniform text-IR sidecar beside the decompile handler, while `anim_graph.json` remains an explicit structured sidecar.

## AGIR text-form field-name conventions

AGIR text emission uses reflection CamelCase keys, not snake_case or invented aliases. Live decompile output is the source of truth. Early draft sketches that proposed `instance_class=`, `layer_name=`, `cache_name=`, or `BlendPose_<i>` were wrong against the actual emitter.

Verified correct forms (UE 5.6):

- Parenthesized field lists use `Key: value`, e.g. `InstanceClass: ...`, `Layer: ...`, and `BlendPoses_<i>: %blend_space_0`; top-level `=` inside those lists is invalid.
- Head attributes outside parenthesized field lists remain `key=value`: cached-pose save/use still writes `name=` on save and `source=` on use; the runtime property the value lands in is `NameOfCache`.
- Linked-anim does NOT emit pose-link children from AGIR text. Compile reconstructs the node with `InstanceClass` + `Layer` + `ReconstructNode()` and skips input-pose wiring.
- Local ids use per-node-family mnemonics such as `%save_cached_pose_0`, `%blend_space_0`, and `%use_cached_pose_0`; definitions and pose-link references must use the same emitted token.

When writing a new AGIR compile-side handler, decompile a hand-authored example with `anim.decompile_agir` first and mirror its exact keys before believing any spec doc.

## AGIR data-pin bindings

A data pin (`X`, `Y`, `Alpha`, `BlendWeights_0`, ...) can be *driven* rather than left at a literal, and AGIR expresses both engine mechanisms. They are different mechanisms, so they have different spellings and each round-trips back to its own form:

| form | means | engine state |
| --- | --- | --- |
| `X: $Direction` | a `K2Node_VariableGet` for the AnimBP member `Direction`, wired into the `X` pin | a real node in the AnimGraph, visible to `blueprint.graph.get_nodes` |
| `Alpha: bind Speed` | a property-access (fast-path) binding | an entry in the node's `PropertyBindings`; no node in the graph |
| `Alpha: bind fn GetAlpha` | the same, bound to a function | `PropertyBindings` entry with `Type=Function` |

`$Name` is the same sigil BPIR uses for a member read, deliberately. `bind` takes a dotted path (`bind Struct.Member`). Both work on any anim-node family (`call`, `blend_space`, `layered_blend`, `save_cached_pose`, ...) and on skeletal-control `Alpha` pins.

Two rules follow from a pin being bound:

- **The literal is not emitted for a bound pin.** A bound pin's value in the runtime `FAnimNode_*` struct is overwritten every frame, so printing it beside the binding would print a dead number under the same key. `Alpha: bind CrouchAlpha` therefore reads as "driven"; a bare `Alpha: 0.0` reads as "not driven, sitting at 0" — the distinction the decompiler used to erase.
- **Compile creates the pin if it is hidden.** A property that is not currently exposed as a pin is exposed (the engine's `SetPinVisibility`, which reconstructs the node) before the binding is applied; no separate `animation.authoring.set_anim_graph_pin_exposed` call is needed.

A wired pin whose driver is neither of these (a function-call node, a math node, a getter on another object) is not representable. The decompile reports it as an `AGIR_PIN_BINDING_NOT_REPRESENTABLE` warning naming the node, the pin and the driver, rather than emitting nothing — so an incomplete text is distinguishable from a complete-and-empty one. A `$Name` whose variable does not exist on the target AnimBP, or that cannot be connected, comes back as an `AGIR_FIELD_WRITE` warning; nothing is silently dropped in either direction.

## AGIR cliff completion (round-trip families)

AGIR compile/decompile is feature-complete across seven cliff families that previously round-tripped lossy or rejected on recompile:

- `blend_space` - sample-graph children round-trip via inlined compile (engine's `CompileBlockIntoGraph` is anonymous-namespace, so the body is inlined). Class resolution reads `class=` arg with `UAnimGraphNode_BlendSpaceGraph` as default; do NOT use `Inst.SymbolName` (it is the display-name token, not a class path).
- `layered_blend`, `linked_anim`, `linked_input_pose` - compile-only cliffs (decompile already worked).
- `save_cached_pose` / `use_cached_pose` - cross-graph cache-name table is editor-side, AnimBP-wide, resolved during `EarlyValidation`. AGIR compile sets editor strings + weak ptr; engine completes runtime wiring at compile time.
- `custom_transition` (`logic_type=2`) - compile-only; engine's `CreateCustomTransitionGraph` is protected and its property-event trigger fires `PostEditChangeProperty` deep in synthetic construction, so the body is inlined.
- `state_alias` - added later.
- Interface-implemented functions (`linked_anim` referencing a non-class function like `FullBodyAdditives`) - added as a top-level `interfaces { implements <classpath> }` block. Compile pre-pass calls `FBlueprintEditorUtils::ImplementNewInterface` for each manifest entry BEFORE `BuildLayerGraphMap` runs, otherwise decompiled-then-recompiled AnimBPs fail with `AGIR_TARGET_NOT_FOUND`.

Node creation uses `AnimGraphConstructionUtils::CreateAnimNode` (wraps `FGraphNodeCreator`) - extensions auto-register, so do not hand-roll `FGraphNodeCreator` per cliff. Apply `ApplyNodeGuidIfPresent` on every created node including sample-graph children, and include `"guid"` in IgnoredKeys for the dispatch loop.

## Round-trip limitations and how to diagnose a round-trip failure

The canonical AGIR loop is decompile -> edit text -> recompile -> diff. It relies on the decompiler's output being losslessly re-consumable by the compiler. Most of it is (see the cliff families above), but a few cases are not yet lossless, and the failure surface can look like a user error when it is actually a tool gap.

**Diagnostic rule:** If `anim.compile_agir` rejects (or warns on) the unmodified output of `anim.decompile_agir`, that is a tool round-trip gap, not your error. Do not rewrite your AGIR or re-read your input. Check this list first, then report the issue against the AGIR decompiler/compiler. You should never need to read the plugin's C++ (`AGIRTextEmitter` / `AGIRParser` / `AGIRCompiler`) to use the tool.

When a pose-symbol resolution fails, `anim.compile_agir` surfaces a `hint` field alongside the `AGIR_SYMBOL_NOT_FOUND` error steering you here. That hint fires for any unresolved `%` pose reference, so its presence does not by itself prove a tool bug, but it does on unmodified decompiler output.

Known not-yet-lossless cases:

- **Empty-struct fields, e.g. `AlphaBoolBlend: "()"`.** The decompiler emits an at-default struct as the empty-parenthesis literal `"()"`; feeding that back warns `ImportText failed for AlphaBoolBlend` and silently drops the field (the surrounding graph still compiles). A decompile -> recompile -> re-decompile -> diff review will show a spurious diff on the field or see it vanish. This is a known limitation.
- **Historical, now fixed: wired data pins were invisible in both directions.** `anim.compile_agir` accepted `X: $Direction` and fed it to `ImportText`, and `anim.decompile_agir` never printed a data-pin binding at all — so a correct AnimGraph and one whose inputs were never connected produced byte-identical AGIR, with `warnings: []` both times. Reading a locomotion blend space or a skeletal-control `Alpha` at face value gave a confident wrong answer about working content. Both sides now round-trip the two binding forms above.
- **Historical, now fixed: single top-level state machine `output %state_machine_0`.** An AnimBP whose AnimGraph output was a bare state machine once failed verbatim recompile with `AGIR_SYMBOL_NOT_FOUND` on `%state_machine_0` (the emitter pre-allocated the token but never wrote the binding). Fixed by registering the name-token machine under the emitter's positional id. It remains here as the canonical example of the failure shape this rule covers.

## BlendSample.SampleName does not exist on UE 5.6

Do not reach for `FBlendSample::SampleName` when emitting blend-space sample-graph names. That field does not exist on UE 5.6 (a reviewer suggested it; it was wrong against the engine source). The AGIR decompiler uses `GetName()` on the sample graph itself. The cost is that user-renamed samples drift from their original name in the round-trip; the upside is that the code compiles against the current engine.

If round-trip name fidelity becomes a hard requirement, sample names need to be plumbed through a different engine API or stored out-of-band in AGIR text, but `SampleName` is not the field to use.

## AGIR top-level constructs (interfaces, state_alias)

AGIR supports two top-level constructs from the May 2026 dump-completeness pass:

- `interfaces { implements ... }` - a top-level block listing implemented anim-graph interfaces and their function declarations. Empty when the AnimBP has no interface implementations; populated for assets that inherit anim function contracts.
- `state_alias` opcode - emitted by state-machine state-alias nodes with their `aliases` list sorted alphabetically for stable diffs. Replaces an earlier `SYMBOL_NOT_FOUND` decompile failure on AnimBPs containing alias nodes.

Both features are sprint-verified end-to-end against a real `agir.txt` round-trip, not just the decompile path. They survive AGIR compile -> save -> decompile. See the AGIR cliff completion section above for the compile-side handler details.

## See also

- [`asset`](asset.md) for asset dump sidecar registration and diff-baseline behavior.
- [`controlrig`](controlrig.md) for CRIR - the sibling text IR for `UControlRigBlueprint` RigVM graphs.
