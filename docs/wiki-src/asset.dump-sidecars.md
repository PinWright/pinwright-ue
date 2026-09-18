# Asset dump sidecars

Typed JSON/text sidecars that `asset.dump` and `asset.dump_folder` emit beside `meta.json` and `properties.json`. Use this page to read dump output, add a per-type sidecar, or interpret `meta.json`'s `propertiesStatus` / `sidecarsEmitted`.

## Typed dump sidecars and widget preview screenshot

`asset.dump` / `asset.dump_folder` emit typed sidecars per asset class in addition to generic `properties.json`. The complete list (May 2026):

- `static_mesh.json` + `static_mesh.txt`, `texture.json` + `texture.txt`, `sound_wave.json` — emitted by the matching builders/text emitters in `Private/Handlers/Asset/`. Text sidecars are compact companions; the JSON files remain the structured dumps and source for `static_mesh.describe` / `texture.describe`.
- `level_sequence.json` — diff-stable; tickResolution/displayRate/playbackRange + tracks/sections/channels with key counts + bindings sorted by `FGuid::operator<` (integer-wise). `MovieSceneJsonUtils::MakeFrameRateObject` is the shared helper for tick-resolution serialization, reused by both `LevelSequenceDumpBuilder.cpp` and `WidgetAnimationJsonSerializer.cpp`.

**Level Sequence section references**

Section records retain the authored thing they play or control, not only range and channels. Built-in fields are:

- Skeletal animation sections: `animationPath`, `firstLoopStartFrameOffset`, `startFrameOffset`, `endFrameOffset`, `playRate`, `playRateType`, `bReverse`, `slotName`, `mirrorDataTablePath`, `bSkipAnimNotifiers`, and `bForceCustomMode`.
- Audio sections: `soundPath`, `attenuationSettingsPath`, `startFrameOffset`, `looping`, `playUntilFinished`, `suppressSubtitles`, and `overrideAttenuation`.
- Camera cuts: `cameraBindingId`; camera shakes: `shakeClassPath` and `playScale`; data layers and console-variable collections use ordered path arrays.
- Track-level material parameter collections use `materialParameterCollectionPath`.
- Asset-bearing optional MovieScene sections use `assetReferences` keyed by reflected property path. This keeps Control Rig, cache, media, subtitle, DMX, audio modulation, prestreaming, pose-search, animation-mixer, assembly, gameplay-cue, and lens references visible without linking every optional engine plugin.

An unassigned reference is JSON `null`, not an omitted key; an unassigned array element is also `null`. Consumers must distinguish an explicitly unassigned asset from a field not present on that section class.
- `sound_cue.json` + `scir.txt` — structured walk of `FirstNode`/`ChildNodes` plus `AllNodes` orphans, deduped and max-child-clamped, with per-node CDO-diff properties, edges, and resolved `USoundNodeWavePlayer.SoundWave`; a node that owns an `editfixedsize` array parallel to `ChildNodes` (Random `Weights`, Mixer/Concatenator `InputVolume`, GroupControl `GroupSizes`) also carries `childValues`, pairing those entries with the child count they must match — the CDO diff hides a never-grown one entirely. SCIR is the compact text-IR companion used by `audio.authoring.decompile_sound_cue`, and it warns on both a mismatched per-child array and a Random node whose weights sum to zero.
- `metasound.json` — MetaSoundPatch / MetaSoundSource via `IMetaSoundDocumentInterface::GetConstDocument()`; emits `rootGraph` + nodes + edges + variables. Dispatched **above** the SoundWave check in `AssetDumpHandler.cpp` so MetaSoundSource is no longer misclassified as a SoundWave. When the document carries no default-page graph the sidecar is a stub instead: `skipped: true` + `skipReason: "METASOUND_GRAPH_UNAVAILABLE"` + `skipMessage`, keeping `assetKind` / `assetPath` / `rootGraph` and **omitting** `nodes`/`edges`/`variables` (an empty array would read as a genuinely empty graph). The dump joins UE 5.8's async document versioning first (`PinWright::MetaSound::WaitForDocumentVersioning`), so in practice the stub means a broken document, not an unmigrated one.
- `skeletal_mesh.json` — bounds, LODs, materials, physics asset and skeleton references, active sockets from `USkeletalMesh::GetActiveSocketList()`, and skeleton virtual bones. Live equivalent: `skeleton.describe_mesh`.
- `data_table.json` — emits `RowStruct` + 10-row preview with struct fields (`UDataTable` branch in `AssetDumpHandler.cpp`).
- `agir.txt` + `anim_graph.json` — emitted for AnimBlueprints alongside `bpir.txt`. Child AnimBPs whose `ParentClass` is another `ABP_*_C` (LinkedLayer overrides, demo-fork copies) emit a `delegates_to \`<parent classpath with _C suffix>\`` block instead of empty AGIR; `FAGIRDecompiler::Decompile`'s empty-`TopLevelGraphs` branch synthesizes the stub when `Cast<UAnimBlueprintGeneratedClass>(ParentClass)` succeeds. Non-AnimBP empty-graph case keeps the empty text + "Anim BP has no anim graphs." warning. Format matches the existing AGIR backtick-classpath style used by the `interfaces { implements \`<path>\` }` block.
- `btir.txt` — emitted for BehaviorTree assets and standalone Blackboard assets as compact BTIR text. Each graph-node line leads its field list with `nodeId: <guid>`, the id the `behavior_tree.*` authoring verbs address.
- `crir.txt` — emitted for `UControlRigBlueprint` assets via the CRIR registry registration in `Handlers/ControlRig/CRIRDecompileHandler.cpp`. Live equivalent: `controlrig.decompile_crir`.
- `map_references.json` — sidecar listing soft-uworld + referenced-asset summary.

`preview.png` is opt-in via `includeWidgetScreenshot=true` on `asset.dump` / `asset.dump_folder`. Capture uses `WidgetDesignerCaptureUtil` and `WidgetDesignerCaptureInternal.h`; its deterministic size is 400x200 plus `max_size` from the resolved preview, so pixels match across DPIs. Failures become diagnostics and never block the dump.

Uniform text-IR sidecars use the `UObject* -> { Text, Warnings, bSuccess }` contract and register through `Utils/IrSidecarRegistry.h` with `REGISTER_DECOMPILE_IR` beside the owning decompile handler. Current examples: MGIR (`material.decompile_mgir`), AGIR (`anim.decompile_agir`), SCIR (`audio.authoring.decompile_sound_cue`), BTIR (`behavior_tree.decompile`), MSIR (`audio.authoring.decompile_metasound`), NIR (`niagara.decompile_nir`), PCGIR (`pcg.decompile`), and CRIR (`controlrig.decompile_crir`).

`AssetDumpHandler::BuildAllFilesForAsset` dispatches registered IR sidecars after explicit asset-type branches, and baseline loading adds every registered filename to `FixedCanonical[]`. Add a uniform text-IR companion by registering its decompiler, not by editing the fixed asset-dump list. BPIR, JSON, XML, native structured sidecars, and binary artifacts stay explicit because they do not use that result shape; BPIR also carries graph headers and failure markers from `AssetDumpBuilder::BuildBpirText`.

## Adding a new per-type sidecar

`asset.dump` dispatch is one `else if` chain in the default-tail block: cast to `UFoo*`, call `<Foo>DumpBuilder::Build<Foo>Json(asset)`, then write with `AddJsonFile(DumpFileNames::<Foo>, ...)`. A new type requires four edits:

1. Create `Private/Handlers/Asset/<Foo>DumpBuilder.{h,cpp}` with `PINWRIGHT_API TSharedPtr<FJsonObject> Build<Foo>Json(const U<Foo>* asset)`, returning null for null input; mirror `StaticMeshDumpBuilder`.
2. Add a constant to the `DumpFileNames` namespace in `AssetDumpHandler.h`.
3. Add the dispatch branch in `BuildAllFilesForAsset` (`AssetDumpHandler.cpp`).
4. Add the filename to `FixedCanonical[]` inside `LoadBaselineDumpFiles` so diff-baseline lookups round-trip the sidecar.

Cast-order gotchas: `UMaterialInstanceConstant` does NOT derive from `UMaterial` (`UMaterialInstance : UMaterialInterface`), so MIC needs a separate arm. `UAnimMontage` does NOT derive from `UAnimSequence` (`UAnimCompositeBase : UAnimSequenceBase`), so either order is safe. Verify the engine header before placing a branch.

## Recent schema additions and removals

`meta.json` no longer emits `dumpedAt` (it churned ~30k unchanged files), `assetType` (hardcoded `"UObject"` for ~92% of assets), or `packageFlags` (empty on 30k assets). Unchanged re-dumps now remain stable.
- For `UObjectRedirector` assets, `meta.json` now includes a `redirectsTo` field with the resolved target asset path (via `ResolveRedirectorTarget` in `AssetDumpBuilder.cpp`). The empty `properties.json` callout above still applies — `DestinationObject` is non-reflected, so `properties.json` stays empty; cache consumers should read `meta.json.redirectsTo` instead.
- Per-asset skip stubs still create a folder with only `{ assetPath, className, skipped: true, skipReason: "<ErrorCode>" }` in `meta.json` (`AssetDumpErrorCodes`). Completion carries `assetCount`, `queued`, `dumped`, `unchanged`, and `skipCount`; folder jobs preserve `assetCount == dumped + unchanged + skipCount`. Cache consumers MUST branch on `meta.json.skipped` before opening sidecars, and `ReconcileMirrorSubtree` overwrites stubs after a successful dump.
- `properties.json` for `TSoftClassPtr` properties strips the trailing space that used to appear in `type` strings (217+ files).
- `properties.json` for instanced sub-objects recurses when `CPF_PersistentInstance` or `CPF_InstancedReference` is set, emitting inline UObject state instead of opaque `:Foo_0` paths. A thread-local context tracks cycles and depth (max 3); revisits/cap hits emit `{ "_kind": "cycle", "object": "..." }` / `{ "_kind": "max_depth", "object": "..." }` under `"object"`.
- `properties.json` struct-array elements (e.g. `TextureParameterValues`) now expand into JSON arrays of objects via a shared `StructToJsonObject` helper instead of escaped `ExportText` strings.
- MGIR: engine-generated empty materials/functions now emit `# no expression graph` inside the otherwise-bare ``entry material `/Game/Path/Asset.Asset` { }`` body, distinguishing "intentionally empty" from "decompile failed"; the parser tolerates `#` comments.
- `tree.xml` user-widget tags no longer leak the `_C` suffix; element names are stripped to the user-facing class name.
- `tree.xml` no longer emits redundant `Geom.source="off"` on every node; the sentinel is suppressed on non-root elements.

`dumpSchemaVersion` 6 (May 2026) adds five additive `meta.json` fields and `ASSET_FILE_MISSING`, reducing class-name branching for routine checks:

- `kind` (always present): coarse runtime classification — `Widget` / `Actor` / `Component` / `AnimInstance` / `Interface` / `FunctionLibrary` / `MacroLibrary` / `Object`. Blueprint classification uses `BlueprintType` plus `IsChildOf` on `GeneratedClass` (falling back to `ParentClass` when null); other assets use `Asset->GetClass()`. This replaces parentClass denylists for widget BPs whose C++ parent lacks `UserWidget` (for example, `CustomHUDLayout` and `EditorUtilityWidget`).
- `blueprintType` (Blueprint assets only): raw `UBlueprint::BlueprintType` enum string (`Normal` / `MacroLibrary` / `Interface` / `FunctionLibrary` / `Const` / `LevelScript`). Omitted on non-Blueprint assets. Reuses `AssetDumpHandler::BlueprintTypeToStatusString` so the meta and `propertiesStatus.blueprintType` surfaces stay in lockstep.
- `sidecarsEmitted` (always present except on skip stubs): alphabetically-sorted files written beside `meta.json`. Intersect it with the canonical sidecar registry to expose missing handlers; a class producing only `meta.json` + `properties.json` is now visible as data.
- `propertiesStatus` for non-Blueprint and `UObjectRedirector` assets: `{ status: "n/a", reason: "non_blueprint_asset" }`. Pairs with the existing Blueprint values so a single `meta.propertiesStatus?.status` check works uniformly across Material, Texture, StaticMesh, SoundCue, Niagara, redirector, and every other asset kind without branching on `className`.
- `compileStateAvailable` + `compileStateReason` were added for NiagaraSystem / NiagaraEmitter in schema 6, then removed in schema 9 because they describe live editor state, not authored content. Use `niagara.inspect` / `niagara.validate`.
- `ASSET_FILE_MISSING` comes from `DumpSingleAsset` when `FPackageName::DoesPackageExist` fails before `LoadObject`. It separates orphan stubs/baker residue/content-pack leftovers (case 1 — "ignore, was never going to load") from genuine `ASSET_LOAD_FAILED` (case 2 — "investigate, real load failure"); a skip stub may carry it in `skipReason`.

Schema v8 (July 2026) makes unchanged public sidecars byte-identical and the dump root git-friendly:

- `meta.json` no longer emits `pluginVersion` or `dumpSchemaVersion` — both rewrote the ~30k-file tree on every release. Versioning now lives only in private per-asset `.dumpcache.json` (`dumper.pluginVersion` and `aspectVersions`); the `meta.json` aspect version bumped 4 → 5. The `dumpSchemaVersion` notes above remain history for older dumps.
- Niagara sidecars no longer emit `changeId` — it mirrored `UNiagaraGraph::GetChangeID()`, an `FGuid` regenerated on PostLoad resync, so it churned on every editor session without any authored change.
- `anim_graph.json` `pages[]`: the interface-layer tail is now sorted by `GetPathName` (previously TSet pointer order, which varied run-to-run).
- The writer seeds four create-if-missing, never-overwritten root files: `.gitignore` (ignores `.dumpcache.json` and `*.tmp`), `.gitattributes` (`* -text`), and identical `CLAUDE.md` / `AGENTS.md` guidance (refresh commands, stale generated content, and whole-side merge conflict resolution such as `git merge -X theirs` followed by a sweep). The two guidance files stay tracked; only `.dumpcache.json` / `*.tmp` are ignored. Users may customize or delete any of the four; deletion regenerates stock content. This keeps volatile `.dumpcache.json` fields (`engineVersion` with per-hotfix CL, `pluginVersion`, `diskSize`, `packageSavedHash`) out of content-only diffs.

Schema v9 (July 2026) separates authored mirrors from live diagnostics and makes baseline replacement content-aware:

- `meta.json` removes Niagara `compileStateAvailable` / `compileStateReason`; `niagara_compile.json` and `nir.txt` omit live compile readiness, validity, recompile flags, and deferred-on-load markers. Runtime compile state remains on `niagara.inspect` / `niagara.validate`.
- Rapid-iteration parameters byte-identical to persisted module-source defaults are omitted; authored overrides and unresolved entries remain.
- `properties.json` omits transient, duplicate-transient, deprecated, skip-serialization, and exact known derived-cache fields. Set values and SoundCue concurrency paths are deterministically sorted.
- Dump writes compare exact UTF-8/binary bytes, preserve unchanged mtimes, stage changes transactionally, retain `.dumpcache.json`, and prune stale sidecars only after successful commit.
- Texture reads refuse temporary async-compilation stand-ins. Direct calls return `ASSET_COMPILING`; folder jobs defer and retry a few dozen entries later, then report `ASSET_COMPILE_TIMEOUT` after 120 seconds with no compilation progress anywhere, preserving a prior dump when one exists.

## Blueprint meta synthesis when GeneratedClass is null

Blueprint assets whose `GeneratedClass` is null (for example, unloaded or partly-broken BPs) used to degrade to `className="Blueprint"` with empty `parentClass`. `BuildMetaJson` now synthesises `className` as `<AssetName>_C` from the BP asset name.

Do NOT fall through to `ParentClass->GetName()`: both `GeneratedClass` and `ParentClass` can be null on the same asset, so that proposed fix would null-deref. Synthesis uses the asset name only.

For Blueprint-derived assets, `meta.json.className` remains `<AssetName>_C` (the GeneratedClass name). Filter subtypes with `parentClass` (for example `/Script/Engine.AnimInstance` or `/Script/UMG.UserWidget`); synthesized entries leave it empty when unknown.

## BP-added SCS components don't live on the CDO

`properties.json` recursion through `UActorComponent*` fields must distinguish **inherited native-CDS components** from **BP-added SCS components**.

- **Inherited native-CDS components** (created by `CreateDefaultSubobject` in C++ constructors) populate the CDO field, so `FObjectProperty::GetObjectPropertyValue_InContainer(CDO)` returns the template.
- **BP-added SCS components** (`USimpleConstructionScript::Nodes`) **do not** put their template on the BPGC CDO. The same property read returns `nullptr` because `USCS_Node::ExecuteNodeOnActor` populates the field only when constructing an instance.

To recover an SCS template, `PropertyUtils.cpp`'s `FObjectProperty` branch (`ResolveSCSComponentTemplate`) casts `Container->GetClass()` to `UBlueprintGeneratedClass`, walks `GetSuperClass()`, and calls `FindSCSNode(Property->GetFName())` on each class with a `SimpleConstructionScript`. A hit returns `Node->GetActualComponentTemplate(LeafBPGC)`, so `UInheritableComponentHandler` leaf overrides win over parent SCS defaults.

**Critical name distinction:** BPGC `FObjectProperty->GetFName()` matches `USCS_Node::InternalVariableName` (`Box`), while the template outer is `<VarName>_GEN_VARIABLE` (`Box_GEN_VARIABLE`). Use `FindSCSNode(Property->GetFName())`, **not** `FindComponentTemplateByName`, which expects the suffix; the old lookup silently missed BP-added components on derived BPGCs.

Pinned by a regression test. See also [`blueprint.scs`](blueprint.scs.md) for the live `blueprint.scs.get` surface that already understood SCS-vs-CDO and was the structural reference for the dumper fix.

## TMap key serialization fallback

`PropertyUtils.cpp` map-key serialization handles `FString` / `FName` / integer keys directly and uses `KeyProp->ExportTextItem_Direct(...)` for the fallback path. That covers struct keys, `FGuid`, `FSoftObjectPath`, `int64`, and other non-{Str,Name,Int} key types so they serialize as canonical ExportText strings instead of the historical `key_N` placeholder. Byte-enum keys emit the resolved enum name.

Note on a near-miss false positive: `UMaterial::ParameterOverviewExpansion` looks like a struct-key map at a glance but is actually `TMap<FString,bool>`. The engine editor (`SMaterialLayersFunctionsTree.cpp`) manufactures the keys as concatenated `Index+Association+Name` strings and stores them verbatim. The dumper emits those FString keys faithfully — do not file it as struct-key mangling.

## BPIR identifier quoting in dumps

BPIR identifier tokens are backtick-quoted whenever the UE name contains any non-identifier character, not just spaces. A common case is a Blueprint boolean display name ending in `?` (for example `Mode01?` or `Raw Value?`): the decompiler emits `` `Mode01?` `` / `` $`Raw Value?` `` / `` bool `Mode01?` `` instead of raw `?`, which used to break regex consumers. The parser unwraps these tokens on the param/arg side, so round-trips stay clean.

This is a tightening of the existing rule documented in `call("bpir.types")`: "Any identifier containing spaces must be wrapped in backticks." In practice the decompiler quotes on any character that the BPIR identifier lexer would otherwise refuse — spaces, `?`, and anything that would force re-tokenization.

## See also

- [`asset`](asset.md) — the `asset.dump` / `asset.dump_folder` verbs that write these sidecars.
- [`asset.dump-quickstart`](asset.dump-quickstart.md) — dump roots, freshness markers, and the first-sweep workflow.
- [`asset-audit`](asset-audit.md) — the repeatable evidence-cache workflow these sidecars feed.
