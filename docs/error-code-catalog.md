# Error Code Catalog

Generated inventory of every error code emitted from a handler tree — all 6
`Source/PinWright*/Private/Handlers/` roots — counting three spellings: raw
`SendError(TEXT("CODE"))`, bare `SendError("CODE", ...)`, and any reference to an
`ErrorCodes::ERR_*` constant, whichever sink it reaches (`FHandlerContext::SendError`,
`GeometryOps::FOpResult::Fail`/`FailIn`, or an out-parameter a caller sends later). Stable
reference for the error-code dedupe + `Handlers/ErrorCodes.h` registry migration (board
ticket `E-error-code-vocabulary-registry`).

Regenerate after any handler change with the snippet at the bottom of this file. Its root
discovery and both literal spellings mirror
`PinWright.core.error_codes.AllEmittedCodesAreRegistered`
(`Source/PinWright/Private/Tests/Core/TestErrorCodeRegistry.cpp`), so the catalog and that
contract test cover the same ground; the constant spelling is extra, since the test only
needs to see raw literals.

Emitting a code from a new verb: declare the `ERR_*` constant in `Handlers/ErrorCodes.h`
first, prefer an existing spelling over a new synonym, and put recovery information in the
structured payload — see [RPC design §7](rpc-design.md).

**Scope boundary: `Handlers/` roots only.** A callsite outside every `Private/Handlers` tree
is not counted, even for a code the table already lists. Concretely,
`GeometryOps::FOpResult::Fail(ErrorCodes::ERR_NO_UV_ELEMENTS, ...)` at
`Source/PinWrightGeometry/Private/Model/PwModelCompiler.cpp:1294` is a real fourth
`NO_UV_ELEMENTS` callsite and is deliberately excluded — the row reads 3 / 2. The same
boundary hides `Private/AudioGen/` (~350 constant references), `Private/Utils/`,
`Private/Transport/`, `Private/Dispatch/`, and the two loose handler files
`Private/PinWright_BlueprintHandlers_List.cpp` and `Private/PinWright_SCSHandlers.cpp`.
It also hides the 21 `MGIR_*` codes in `Private/MGIR/`, which are compiler diagnostic
strings rather than `SendError` literals and so are outside the scan on both counts.

**A callsite is one regex match**, and the constant pattern cannot tell an emit from a
mention: 8 comparisons (`ErrorCode == ErrorCodes::ERR_*`) and 4 schema-listing field names in
`Handlers/Audio/AudioSynthSchemaHandler.cpp` are counted as callsites here. 12 of 4608 — the
figures are an upper bound on emits, not an exact one.

Counts come from the scan itself, not from the rows below.

- **Unique codes:** 768 — one row each in `## Codes by frequency`.
- **Total callsites:** 4608 — sum of that table's `Callsites` column.
- **Handler files:** 287 of the 595 `.cpp`/`.h` files under the 6 roots emit at
  least one code. Do not sum the `Files` column instead: it is per-code and overlapping
  (one handler file emits many codes), so it double-counts to 1912.

Adding the codes only the two tables below carry gives 771 distinct codes file-wide. Quote
which of the two figures you mean.

These three totals are as of the scan below and were NOT recomputed for rows appended by hand
afterwards — `SAVE_DISK_STATE_DIVERGED` (`asset.save`,
board `B-asset-save-clobbers-out-of-band-change`) is one such row.

Scanned 2026-08-29 against the working tree at `2ba21649` (dirty). Source is edited concurrently;
re-run the snippet rather than trusting these totals to the day.

> **Scope widened 2026-08-19; the earlier staleness warning is resolved.** The previous snippet
> scanned only `Source/PinWright/Private/Handlers` and matched only `SendError(TEXT("..."))`.
> It was structurally blind to the five gated integration sub-modules (`geometry.*`, `pcg.*`,
> `chooser.*`, `pose_search.*`, the CommonUI verbs) and to every `ErrorCodes::ERR_*` callsite,
> and the table it produced — 556 unique / 3385 callsites — kept stale rows for codes that had
> moved to the sub-modules. Both the table and the snippet below now walk all 6 roots and
> count constants.

## Transport-emitted codes

Emitted from `Private/Transport/`, outside every `Handlers/` root, so the scan cannot count
those callsites. `EDITOR_BLOCKED_ON_MODAL` and `EDITOR_GAME_THREAD_STALLED` therefore have no
row at all; `EDITOR_NOT_READY` has one (3 / 2) covering only its `Handlers/UI/WidgetDesigner*`
references, not its transport emits. All three are declared in `Handlers/ErrorCodes.h`, but
nothing enforces that: `TestErrorCodeRegistry` collects only raw `SendError` literals under the
`Handlers/` roots, so a transport code emitted through a constant is invisible to it too.
Recorded here because regenerating the table would drop them:

| Code | Emitted by | Retryable |
|---|---|---|
| `EDITOR_NOT_READY` | `McpRequestCore::BuildPingResult` / the `tools/call` readiness gate, before startup completes | **yes** — a client is expected to poll |
| `EDITOR_BLOCKED_ON_MODAL` | the same two sites, when `ModalStateProbe` reports the game thread owned by a modal dialog past `UPinWrightSettings::ModalBlockedReportSeconds` | **no** — polling can never clear it; only a human dismissing the dialog or a process kill can. Kept distinct from `EDITOR_NOT_READY` precisely so a well-behaved client stops instead of burning its watchdog. See `docs/wiki-src/unattended.md`. |
| `EDITOR_GAME_THREAD_STALLED` | `McpRequestCore::BuildPingResult` ONLY — never the `tools/call` gate — when `ModalStateProbe`'s heartbeat has been stale past `UPinWrightSettings::GameThreadStallReportSeconds` (90 s default) | **yes** — a wedged handler may still return, and the queued request runs when it does. Distinct from both neighbours: `EDITOR_NOT_READY` means "still starting" and says nothing about what is wedged; `EDITOR_BLOCKED_ON_MODAL` is non-retryable and needs a human. Carries `inFlightMethod` / `inFlightRequestId` / `inFlightSeconds` when the thread stalled inside a dispatch. See `docs/wiki-src/unattended.md`. |

## Hand-annotated handler codes

Emitted from handlers, and — now that the scan counts `ErrorCodes::ERR_*` constants — most of
these also carry a row in the frequency table. Kept for the semantics a count cannot express.
Two never appear, because they are emitted outside every `Handlers/` root: `ASSET_IN_USE`
(`Utils/AssetCreatePolicy.cpp`) and `INVALID_PATTERN` (`Utils/NameMatchFilter.cpp`).

| Code | Emitted by | Notes |
|---|---|---|
| `ASSET_ALREADY_EXISTS` | `AssetCreatePolicy::Resolve`, via every converted `*.create_*` verb; also `GeometryAssetCreate.cpp` (the single row the frequency table shows) | A **different** asset class occupies the target package path. Never silently replaced, regardless of `overwrite`. Distinct from the broader pre-existing `ALREADY_EXISTS`; error data names `existingClass`. The `GeometryAssetCreate` site reuses the code for a second condition: the target exists but carries no PinWright Model provenance stamp, or was generated from a different source path. **On that second condition the named remedy — `overwrite=true` — always works**, including on a referenced asset, because the flag grants permission without changing the mechanism: the occupant is rebuilt in place either way. Where the code means a class mismatch there is no remedy but renaming or deleting. |
| `ASSET_IN_USE` | `AssetCreatePolicy::Resolve`, when `overwrite:true` and the existing asset has referencers | Two branches, one code. **Registry branch:** on-disk packages reference it — pre-flight refusal, no delete is attempted, so the engine's reference-check modal is never reached. **In-memory branch:** nothing on disk references it, so the delete runs and `ObjectTools` refuses it because something live holds the object — typically a **level actor** spawned from the asset, which the registry cannot see. Error data always carries both pairs: `referencers` / `referencerCount` (on-disk packages, list capped at 25) and `referencingActors` / `referencingActorCount` (live actors as `Label (Class) in /Game/Maps/L_Foo`, list capped at 25); the pair that does not apply reads `[]` / `0`. **The remedy on both branches is to drop `overwrite`** — the same-path re-run then takes the update-in-place branch and keeps every reference intact. **Not reachable from `model.compile` / `geometry.convert_to_static_mesh`:** `GeometryAssetCreate.cpp` passes `bOverwriteRequested=false` unconditionally and reads its own `overwrite` flag as permission at the provenance gate instead, so a StaticMesh occupant is always rebuilt in place and no referencer count can refuse it. Passing the flag through here instead is what deadlocked those two gates against each other — this row's "drop `overwrite`" remedy and the provenance gate's "pass `overwrite=true`" each named the other, and on a referenced *and* unstamped asset neither worked. |
| `OVERWRITE_UNSAFE` | `asset.import` | The destination is occupied but the request cannot be proven to update one exact `UTexture2D` object in place without changing identity. The refusal happens before import or reimport. Error data names the requested path, source, existing path/class when resolvable, reason, all conservative prefix conflicts, and a capped sorted `referencers` list with uncapped `referencerCount`. Import under a new path, repoint known consumers, then delete the old asset through the unforced path; do not retry the same occupied request expecting automatic reference replacement. |
| `MESH_DUPLICATE_SUBSTITUTED` | `actor.duplicate` (`Handlers/Actor/LifecycleHandler.cpp`) | The engine replaced the duplicated dynamic mesh with a 12-triangle placeholder cube (`UDynamicMesh.cpp:568`) and reported success. The duplicate is destroyed and this is returned instead. Pass `allowPlaceholderMesh:true` to accept the cube with a warning. |
| `UNSUPPORTED_ORTHOGRAPHIC_ROTATION` | `render.capture_open_level` (`Handlers/Render/`) | Non-axis-aligned orthographic pose. See `docs/wiki-src/render.md`. |
| `BLANK_CAPTURE` | `render.capture_open_level` (`Handlers/Render/`) | The active level viewport remained near-uniform black after one forced redraw. Error data includes world, viewport, camera, and luminance diagnostics; `allowBlank:true` accepts intentional black frames. |
| `CAPTURE_NOT_READY` | `render.capture_open_level`, `editor.screenshot`, `ui.screenshot` (`Utils/CaptureReadinessGate.h`) | Shader compilation was still in flight after the bounded, pumping drain (20 s) that runs after the pose apply and settle and immediately before the readback, so the readback was refused. A material whose shader map has not landed renders as the **default material**, and such a frame reads settled, non-blank and clean on every other honesty field — so it is refused rather than shipped with a warning. Error data carries the `shadersCompiling` block: jobs at entry and remaining, asset compilations, pump rounds, drain and budget in ms. **Not a crash code**, and **a pending asset-compile queue never produces it**: the gate does not wait on textures/meshes/sound waves (that is the normal steady state after a map load) and discloses them through `viewport.shadersCompiling.assetCompilationWarning` instead. |
| `VIEWPORT_WORLD_MISMATCH` | `render.capture_open_level` (`Handlers/Render/`) | The selected active Level Editor viewport renders a different world than `GEditor->GetEditorWorldContext()`. No viewport state is changed; error data names both worlds and the current viewport state. |
| `INVALID_PATTERN` | `NameMatch::Parse` (`Utils/NameMatchFilter.cpp`), via `actor.list`, `system.inspect.list_objects`, `system.inspect.find_objects_by_class` | A `filter` that does not compile under `matchMode:"regex"`. Deliberately distinct from the `INVALID_ARGUMENT` family: ICU swallows regex compile errors, so an unrejected bad pattern silently matches nothing and the caller reads that as a genuine zero-result count. The same three verbs return `INVALID_MODE` for an unrecognised `matchMode` and `INVALID_ARGUMENT` for `matchMode`/`caseSensitive` supplied with no `filter`. See board ticket `B-actor-list-filter-case-mismatch`. |
| `MARK_DIRTY_REFUSED` | `asset.mark_dirty` (`Handlers/Asset/AssetMarkDirtyHandler.cpp`) | The package resolved but dirtying it is meaningless or suppressed: transient / PIE duplicate / cooked / native script package, or the editor is loading, transacting, cooking, or async-loading. Distinct from `PACKAGE_NOT_FOUND` (path never resolved to a loaded package). Message carries the printable reason; error data carries `package` + `isDirty:false`. |
| `NO_AI_CONTROLLER` | `ai.get_runtime_state` (`Handlers/AI/AIRuntimeStateHandler.cpp`) | The named actor resolved inside the PIE world, but no `AController` drives it — an unpossessed Pawn, or a plain actor. The remedy is to name the controller or possess the pawn. Distinct from `ACTOR_NOT_FOUND` (nothing matched the identifier at all) and from `NO_BRAIN_COMPONENT` (a controller is present and runs nothing). |
| `NO_BRAIN_COMPONENT` | `ai.get_runtime_state` (`Handlers/AI/AIRuntimeStateHandler.cpp`) | The controller carries **neither** a `UBrainComponent` **nor** a `UPathFollowingComponent`, so there is no running AI to report on. Refusing beats answering with three empty sections, which reads as "the AI is idle" rather than "there is no AI". A controller with one of the two is **answered**, not refused: an `AIController` driving a pawn with a bare `MoveTo` and no Behavior Tree is the case the verb exists for, and the missing half is reported as `present:false` with a reason. |
| `MESH_EMPTY` | `geometry.bake_ambient_occlusion` (`Handlers/Geometry/GeometryOps_Elements.cpp`) | The target mesh has no triangles, so there is nothing to cast rays against and nothing to write the result to. Refused rather than answered, because a bake on an empty mesh returns a flat, fully-exposed statistics block that is indistinguishable from a real bake that measured nothing — which is the exact failure this verb's statistics exist to expose. |
| `BAKE_FAILED` | `geometry.bake_ambient_occlusion` (`Handlers/Geometry/GeometryOps_Elements.cpp`) | The engine's `FMeshVertexBaker` returned no result image after `Bake()`. Nothing was written to the colour overlay. Not a caller error and not retryable with different parameters: every argument was already validated, so this is an engine-side failure of the baker itself. The repairs the verb makes before baking (freeing orphan colour elements, giving uncovered triangles normals) have already been applied when it fires, and are reported in the error's sibling success fields on a run that gets that far. |

## Codes by frequency

| Code | Callsites | Files |
|---|---:|---:|
| `INVALID_ARGUMENT` | 797 | 131 |
| `INVALID_PARAMS` | 221 | 58 |
| `NOT_FOUND` | 197 | 38 |
| `ASSET_NOT_FOUND` | 161 | 68 |
| `NO_WORLD` | 94 | 14 |
| `MISSING_PARAM` | 93 | 12 |
| `EDITOR_NOT_AVAILABLE` | 78 | 32 |
| `CREATE_FAILED` | 74 | 28 |
| `INVALID_SEQUENCE` | 59 | 4 |
| `MISSING_PARAMETER` | 50 | 11 |
| `SPAWN_FAILED` | 50 | 16 |
| `SECURITY_VIOLATION` | 49 | 19 |
| `CREATION_FAILED` | 44 | 16 |
| `INVALID_PATH` | 43 | 19 |
| `ACTOR_NOT_FOUND` | 42 | 33 |
| `INVALID_PAYLOAD` | 41 | 8 |
| `PACKAGE_ERROR` | 40 | 13 |
| `GRAPH_NOT_FOUND` | 37 | 12 |
| `NODE_NOT_FOUND` | 37 | 15 |
| `SEQUENCE_NOT_FOUND` | 37 | 9 |
| `OPERATION_FAILED` | 36 | 3 |
| `BLUEPRINT_NOT_FOUND` | 35 | 13 |
| `INVALID_BLUEPRINT_PATH` | 35 | 14 |
| `CLASS_NOT_FOUND` | 34 | 25 |
| `UNSUPPORTED_ASSET_EDITOR` | 32 | 10 |
| `GAS_NOT_AVAILABLE` | 27 | 1 |
| `NO_EDITOR_WORLD` | 27 | 12 |
| `PROPERTY_NOT_FOUND` | 26 | 11 |
| `METASOUND_NOT_AVAILABLE` | 23 | 7 |
| `ANIM_BP_NOT_FOUND` | 21 | 2 |
| `ANIMGRAPH_MODULE_UNAVAILABLE` | 20 | 2 |
| `ASSET_CREATION_FAILED` | 20 | 7 |
| `EDITOR_WORLD_NOT_AVAILABLE` | 20 | 15 |
| `LOAD_FAILED` | 20 | 9 |
| `MESH_NOT_FOUND` | 20 | 15 |
| `MISSING_PATH` | 20 | 6 |
| `SKELETON_NOT_FOUND` | 20 | 6 |
| `UNSUPPORTED_ENGINE_VERSION` | 20 | 11 |
| `BINDING_NOT_FOUND` | 18 | 8 |
| `INVALID_TYPE` | 18 | 4 |
| `ANIMATION_INVALID` | 17 | 4 |
| `COMPONENT_NOT_FOUND` | 17 | 12 |
| `INVALID_INDEX` | 17 | 5 |
| `INVALID_ASSET_PATH` | 16 | 11 |
| `ANIMATION_NOT_FOUND` | 15 | 7 |
| `EDITOR_ACTOR_SUBSYSTEM_MISSING` | 15 | 8 |
| `INVALID_VALUE` | 15 | 4 |
| `NOT_SUPPORTED` | 15 | 6 |
| `INVALID_RECIPE` | 14 | 4 |
| `PREVIEW_VIEWPORT_NOT_FOUND` | 14 | 6 |
| `ALREADY_EXISTS` | 13 | 11 |
| `BONE_NOT_FOUND` | 13 | 6 |
| `INVALID_PROPERTY` | 13 | 5 |
| `MISSING_REQUIRED_PARAM` | 13 | 7 |
| `ASSET_DATA_INVALID` | 12 | 6 |
| `MISSING_NAME` | 12 | 3 |
| `PREVIEW_NOT_FOUND` | 12 | 4 |
| `INTERNAL_ERROR` | 11 | 7 |
| `MEMORY_PRESSURE` | 11 | 5 |
| `MISSING_PARAMETERS` | 11 | 2 |
| `NO_SPLINE` | 11 | 2 |
| `BINDING_CREATION_FAILED` | 10 | 2 |
| `BINDING_FAILED` | 10 | 4 |
| `CAPTURE_FAILED` | 10 | 7 |
| `ENCODE_FAILED` | 10 | 7 |
| `INVALID_LANDSCAPE` | 10 | 1 |
| `NOT_AVAILABLE` | 10 | 3 |
| `NO_ACTIVE_LEVEL_VIEWPORT` | 10 | 4 |
| `NO_EDITOR` | 10 | 4 |
| `OBJECT_NOT_FOUND` | 10 | 4 |
| `PIN_NOT_FOUND` | 10 | 8 |
| `WIDGET_NOT_FOUND` | 10 | 6 |
| `GRAPH_UNAVAILABLE` | 9 | 4 |
| `INVALID_PARAMETER` | 9 | 3 |
| `SUBSYSTEM_NOT_FOUND` | 9 | 5 |
| `UNSUPPORTED_OPERATION` | 9 | 3 |
| `VERIFICATION_FAILED` | 9 | 6 |
| `BLUEPRINT_BUSY` | 8 | 6 |
| `BOUNDS_EMPTY` | 8 | 4 |
| `FILE_NOT_FOUND` | 8 | 6 |
| `INVALID_GEOREFERENCE` | 8 | 3 |
| `INVALID_STATE` | 8 | 3 |
| `LEVEL_NOT_FOUND` | 8 | 3 |
| `SEQUENCE_INVALID` | 8 | 6 |
| `STRUCT_NOT_FOUND` | 8 | 3 |
| `TRACK_CREATION_FAILED` | 8 | 4 |
| `CREATION_ERROR` | 7 | 6 |
| `DUPLICATE_FAILED` | 7 | 6 |
| `EXECUTION_ERROR` | 7 | 4 |
| `INVALID_ASSET_TYPE` | 7 | 5 |
| `INVALID_CLASS` | 7 | 4 |
| `INVALID_GRAPH` | 7 | 2 |
| `INVALID_SURFACE_SPEC` | 7 | 3 |
| `METASOUND_FRONTEND_NOT_SUPPORTED` | 7 | 4 |
| `MONTAGE_NOT_FOUND` | 7 | 1 |
| `PARAMETER_NOT_FOUND` | 7 | 4 |
| `SAVE_FAILED` | 7 | 7 |
| `SKELETON_MISMATCH` | 7 | 6 |
| `CONVERSION_FAILED` | 6 | 4 |
| `CUE_NOT_FOUND` | 6 | 2 |
| `INSTANCE_INDEX_OUT_OF_RANGE` | 6 | 3 |
| `INVALID_PARENT` | 6 | 4 |
| `LANDSCAPE_NOT_FOUND` | 6 | 1 |
| `NO_COMPONENT` | 6 | 3 |
| `POLYGON_LIMIT_EXCEEDED` | 6 | 4 |
| `READ_PIXELS_FAILED` | 6 | 2 |
| `REPLACE_REFUSED` | 6 | 1 |
| `SCENE_CAPTURE_FAILED` | 6 | 4 |
| `SKELETAL_MESH_NOT_FOUND` | 6 | 4 |
| `SM_NOT_FOUND` | 6 | 1 |
| `TILE_OUT_OF_RANGE` | 6 | 3 |
| `TRACK_NOT_FOUND` | 6 | 2 |
| `UNSUPPORTED_ASSET` | 6 | 5 |
| `UNSUPPORTED_TYPE` | 6 | 2 |
| `VARIABLE_NOT_FOUND` | 6 | 3 |
| `VIEWPORT_NOT_AVAILABLE` | 6 | 3 |
| `WATER_PLUGIN_NOT_AVAILABLE` | 6 | 1 |
| `WORLD_NOT_FOUND` | 6 | 4 |
| `AMBIGUOUS_TARGET` | 5 | 1 |
| `ASSET_LOAD_FAILED` | 5 | 4 |
| `ATTACH_FAILED` | 5 | 4 |
| `ATTENUATION_NOT_FOUND` | 5 | 1 |
| `BOOLEAN_FAILED` | 5 | 2 |
| `CONDITION_INVALID` | 5 | 4 |
| `DECODE_FAILED` | 5 | 2 |
| `DIALOGUE_NOT_AVAILABLE` | 5 | 1 |
| `GROUND_NOT_MEASURED` | 5 | 2 |
| `INDEX_OUT_OF_RANGE` | 5 | 2 |
| `INPUT_NOT_FOUND` | 5 | 4 |
| `INTERFACE_NOT_FOUND` | 5 | 1 |
| `INVALID_BLUEPRINT` | 5 | 3 |
| `INVALID_INPUTS` | 5 | 1 |
| `INVALID_LOD` | 5 | 2 |
| `INVALID_MANIFEST` | 5 | 1 |
| `INVALID_MODE` | 5 | 5 |
| `INVALID_NODE_TYPE` | 5 | 2 |
| `INVALID_VERTEX` | 5 | 2 |
| `LANDSCAPE_NO_COMPONENTS` | 5 | 1 |
| `MOVIESCENE_UNAVAILABLE` | 5 | 1 |
| `NODE_TYPE_NOT_FOUND` | 5 | 2 |
| `PARAMETER_STORE_NOT_FOUND` | 5 | 2 |
| `PARSE_FAILED` | 5 | 2 |
| `PIN_CREATION_FAILED` | 5 | 2 |
| `PROPERTY_SET_FAILED` | 5 | 3 |
| `RENAME_FAILED` | 5 | 3 |
| `SUBSYSTEM_MISSING` | 5 | 4 |
| `SYSTEM_NOT_FOUND` | 5 | 1 |
| `TOO_MANY_SHOTS` | 5 | 4 |
| `UNSUPPORTED_ASSET_CLASS` | 5 | 3 |
| `WRITE_FAILED` | 5 | 4 |
| `ADD_FAILED` | 4 | 4 |
| `ADD_NODE_FAILED` | 4 | 4 |
| `AMBIGUOUS_ACTOR_NAME` | 4 | 4 |
| `AUDIO_EMPTY_BUFFER` | 4 | 4 |
| `AUDITION_FAILED` | 4 | 1 |
| `AUDIT_INVALID_PLAY_AREA_SPEC` | 4 | 1 |
| `AUDIT_SUPPORT_CHAIN_UNRESOLVED` | 4 | 1 |
| `BODY_NOT_FOUND` | 4 | 1 |
| `DATA_INTERFACE_NOT_FOUND` | 4 | 2 |
| `DEPRECATED_HANDLER` | 4 | 1 |
| `EXEC_FAILED` | 4 | 4 |
| `EXPORT_FAILED` | 4 | 4 |
| `GROUND_NOT_FOUND` | 4 | 1 |
| `IMPORT_FAILED` | 4 | 4 |
| `INVALID_BLEND_MODE` | 4 | 2 |
| `INVALID_GUID` | 4 | 3 |
| `INVALID_OBJECT` | 4 | 1 |
| `INVALID_PLACEMENT` | 4 | 3 |
| `INVALID_SEQUENCE_TYPE` | 4 | 2 |
| `INVALID_WATER_BODY_TYPE` | 4 | 1 |
| `METASOUND_DOCUMENT_INTERFACE_NOT_SUPPORTED` | 4 | 2 |
| `MISSING_MARKER_NAME` | 4 | 2 |
| `MODULE_NOT_FOUND` | 4 | 1 |
| `NOT_IMPLEMENTED` | 4 | 4 |
| `NOT_PARTITIONED` | 4 | 1 |
| `NO_ACTIVE_SESSION` | 4 | 1 |
| `PACKAGE_CREATION_FAILED` | 4 | 3 |
| `PACKAGE_NOT_FOUND` | 4 | 3 |
| `PARTIAL_GROUND_COVERAGE` | 4 | 2 |
| `PLUGIN_DISABLED` | 4 | 3 |
| `PYTHON_INIT_FAILED` | 4 | 2 |
| `SECTION_TYPE_MISMATCH` | 4 | 3 |
| `SLATE_NOT_INITIALIZED` | 4 | 1 |
| `SUBSYSTEM_NOT_AVAILABLE` | 4 | 1 |
| `UNKNOWN_OPERATION` | 4 | 4 |
| `WAVE_NOT_FOUND` | 4 | 1 |
| `WORLD_NOT_AVAILABLE` | 4 | 1 |
| `ACTOR_HAS_NO_BOUNDS` | 3 | 2 |
| `ASSET_ALREADY_EXISTS` | 3 | 3 |
| `ASSET_EXISTS` | 3 | 3 |
| `ASSET_IN_USE` | 3 | 2 |
| `AUDIT_AIRBORNE` | 3 | 1 |
| `AUDIT_BELOW_KILL_Z` | 3 | 1 |
| `BINDING_NOT_SKELETAL` | 3 | 2 |
| `BINDING_UNRESOLVED` | 3 | 1 |
| `BLANK_CAPTURE` | 3 | 3 |
| `EDITOR_NOT_READY` | 3 | 2 |
| `EDITOR_OPEN` | 3 | 2 |
| `FIELD_NOT_FOUND` | 3 | 1 |
| `FOLIAGE_ACTOR_FAILED` | 3 | 1 |
| `GROUND_HITS_ALL_REJECTED` | 3 | 2 |
| `INPUT_FAILED` | 3 | 3 |
| `INVALID_BONE_CONTROL_SPACE` | 3 | 1 |
| `INVALID_BP` | 3 | 3 |
| `INVALID_CONTEXT` | 3 | 1 |
| `INVALID_LAYERS` | 3 | 1 |
| `INVALID_OVERRIDE` | 3 | 1 |
| `INVALID_PARENT_CLASS` | 3 | 3 |
| `INVALID_PIN` | 3 | 2 |
| `INVALID_REFERENCED_ASSET` | 3 | 1 |
| `KEY_NOT_FOUND` | 3 | 2 |
| `LANDSCAPE_NO_HEIGHT_DATA` | 3 | 1 |
| `MATCH_COUNT_MISMATCH` | 3 | 2 |
| `MATERIAL_NOT_FOUND` | 3 | 3 |
| `MESH_AUDIT_READ_FAILED` | 3 | 1 |
| `MISSING_VALUE` | 3 | 3 |
| `MIX_NOT_FOUND` | 3 | 1 |
| `MRQ_NOT_AVAILABLE` | 3 | 1 |
| `NIAGARA_DATA_INTERFACE_MISMATCH` | 3 | 2 |
| `NODE_CREATE_FAILED` | 3 | 1 |
| `NOTIFY_STATE_NOT_FOUND` | 3 | 1 |
| `NO_LOD_MODELS` | 3 | 2 |
| `NO_UV_ELEMENTS` | 3 | 2 |
| `NO_VALID_ASSETS` | 3 | 1 |
| `NO_WATER_BODY_COMPONENT` | 3 | 1 |
| `OPEN_FAILED` | 3 | 3 |
| `PHYSICS_FAILED` | 3 | 1 |
| `QUERY_FAILED` | 3 | 1 |
| `SECTION_CREATION_FAILED` | 3 | 2 |
| `SET_DEFAULT_FAILED` | 3 | 3 |
| `SOCKET_NOT_FOUND` | 3 | 1 |
| `SOURCE_CONTROL_DISABLED` | 3 | 2 |
| `SOURCE_OUTPUT_PATH_NOT_DERIVABLE` | 3 | 3 |
| `STATIC_SWITCH_NOT_FOUND` | 3 | 1 |
| `TRACK_OP_FAILED` | 3 | 3 |
| `TYPE_NOT_FOUND` | 3 | 3 |
| `UNKNOWN_VIEW_MODE` | 3 | 1 |
| `UnsupportedNodeClass` | 3 | 1 |
| `VIEW_MODE_NOT_RENDERABLE` | 3 | 2 |
| `WORLD_PARTITION_NOT_ENABLED` | 3 | 1 |
| `WRONG_NODE_TYPE` | 3 | 3 |
| `ACTOR_NOT_GROUNDED` | 2 | 1 |
| `ACTOR_NO_SKELETAL_MESH_COMPONENT` | 2 | 2 |
| `ANIM_INVALID_SOURCE` | 2 | 1 |
| `APPLY_FAILED` | 2 | 2 |
| `ASSET_CLASS_MISMATCH` | 2 | 2 |
| `ASSET_COMPILING` | 2 | 2 |
| `ASSET_PLAYER_BASE_UNAVAILABLE` | 2 | 1 |
| `AUDIT_AT_WORLD_ORIGIN` | 2 | 1 |
| `AUDIT_BALANCED_ON_POINT` | 2 | 1 |
| `AUDIT_BELOW_SURFACE` | 2 | 1 |
| `AUDIT_DATA_VALIDATION` | 2 | 1 |
| `AUDIT_DEEPLY_EMBEDDED` | 2 | 1 |
| `AUDIT_DUPLICATE_TRANSFORM` | 2 | 1 |
| `AUDIT_EXTREME_SCALE` | 2 | 1 |
| `AUDIT_MISSING_MATERIAL` | 2 | 1 |
| `AUDIT_MISSING_MESH` | 2 | 1 |
| `AUDIT_NAN_TRANSFORM` | 2 | 1 |
| `AUDIT_NEGATIVE_SCALE` | 2 | 1 |
| `AUDIT_OUTSIDE_PLAY_AREA` | 2 | 1 |
| `AUDIT_OUTSIDE_WORLD_BOUNDS` | 2 | 1 |
| `AUDIT_UNKNOWN_CHECK` | 2 | 2 |
| `AUDIT_UNSUPPORTED_ASSEMBLY` | 2 | 1 |
| `AUDIT_ZERO_SCALE` | 2 | 1 |
| `AUTOMATION_ERROR` | 2 | 1 |
| `AXIS_MAPPING_UNRENDERABLE` | 2 | 1 |
| `BIND_FAILED` | 2 | 2 |
| `BLENDSPACE_NOT_FOUND` | 2 | 1 |
| `CAPTURE_ARRAY_NOT_FOUND` | 2 | 1 |
| `CLOTH_CREATE_UNSUPPORTED` | 2 | 1 |
| `COMPONENT_CREATE_FAILED` | 2 | 1 |
| `COMPONENT_FAILED` | 2 | 1 |
| `COMPOSITE_BOUNDARY_BROKEN` | 2 | 1 |
| `CONNECTION_FAILED` | 2 | 2 |
| `CONTROLLER_UNAVAILABLE` | 2 | 1 |
| `CONTROLRIG_TRACK_NOT_FOUND` | 2 | 1 |
| `CREATE_COMPONENT_FAILED` | 2 | 2 |
| `DATA_INTERFACE_EXISTS` | 2 | 1 |
| `DEPENDENCY_MISSING` | 2 | 1 |
| `DUPLICATE_NAME` | 2 | 2 |
| `EMITTER_DATA_MISSING` | 2 | 2 |
| `EMITTER_HANDLE_NOT_FOUND` | 2 | 1 |
| `ENUM_UPDATE_FAILED` | 2 | 1 |
| `EVENT_HANDLER_INVALID_INDEX` | 2 | 1 |
| `EVENT_NOT_FOUND` | 2 | 2 |
| `FUNCTION_NOT_FOUND` | 2 | 2 |
| `GAME_FEATURES_NOT_AVAILABLE` | 2 | 1 |
| `GAME_INSTANCE_NOT_FOUND` | 2 | 1 |
| `GAME_MODE_NOT_FOUND` | 2 | 1 |
| `GAME_STATE_NOT_FOUND` | 2 | 1 |
| `GRAPH_EDITOR_NOT_FOUND` | 2 | 1 |
| `HEIGHT_READ_FAILED` | 2 | 1 |
| `HOLDER_NOT_SEATABLE` | 2 | 1 |
| `IMMUTABLE_NODE` | 2 | 2 |
| `INCOMPATIBLE_DATA_INTERFACE` | 2 | 1 |
| `INTERFACE_ERROR` | 2 | 1 |
| `INTERFACE_NOT_REMOVABLE` | 2 | 1 |
| `INVALID_ASSET` | 2 | 1 |
| `INVALID_DATA_INTERFACE_CLASS` | 2 | 1 |
| `INVALID_GI_METHOD` | 2 | 2 |
| `INVALID_INTERFACE_CLASS` | 2 | 1 |
| `INVALID_JSON` | 2 | 1 |
| `INVALID_NAME` | 2 | 2 |
| `INVALID_NOISE_TYPE` | 2 | 1 |
| `INVALID_NORMAL_OVERLAY` | 2 | 1 |
| `INVALID_OPERATION` | 2 | 1 |
| `INVALID_PARAM_TYPE` | 2 | 2 |
| `INVALID_TAG` | 2 | 2 |
| `INVALID_TARGET` | 2 | 2 |
| `INVALID_XML` | 2 | 1 |
| `JOB_CANCEL_UNSUPPORTED` | 2 | 2 |
| `LANDSCAPE_SHAPE_AXIS_LOCKED` | 2 | 1 |
| `LANDSCAPE_SHAPE_FLAT_REGION` | 2 | 1 |
| `LANDSCAPE_SHAPE_NO_BOUNDARY` | 2 | 1 |
| `LANDSCAPE_SHAPE_REGION_TOO_SMALL` | 2 | 1 |
| `LANDSCAPE_SHAPE_STEPPED` | 2 | 1 |
| `LAYER_CREATION_FAILED` | 2 | 1 |
| `LEVEL_ALREADY_EXISTS` | 2 | 1 |
| `LEVEL_NOT_LOADED` | 2 | 2 |
| `LIVE_CODING_NOT_AVAILABLE` | 2 | 1 |
| `MERGE_FAILED` | 2 | 1 |
| `MERGE_NOT_POSSIBLE` | 2 | 1 |
| `MESH_AUDIT_Z_FIGHTING_UNRUNNABLE` | 2 | 1 |
| `MISSING_BONE_NAME` | 2 | 2 |
| `MISSING_FRAME` | 2 | 1 |
| `MISSING_ROW_STRUCT` | 2 | 1 |
| `MISSING_SECTION_NAME` | 2 | 1 |
| `MODEL_INVALID_SOURCE` | 2 | 1 |
| `MORPH_NOT_FOUND` | 2 | 1 |
| `MRQ_SUBSYSTEM_UNAVAILABLE` | 2 | 1 |
| `MSIR_DECOMPILE_FAILED` | 2 | 1 |
| `NOT_A_GAMEPLAY_EFFECT` | 2 | 1 |
| `NO_ASSETS_MATCHED` | 2 | 2 |
| `NO_BLUEPRINT_EDITOR` | 2 | 1 |
| `NO_GAME_INSTANCE` | 2 | 1 |
| `NO_KEYS_WRITTEN` | 2 | 1 |
| `NO_NAVMESH` | 2 | 1 |
| `NO_NAV_SYS` | 2 | 1 |
| `NO_VIEWPORT` | 2 | 1 |
| `OUTPUT_NOT_FOUND` | 2 | 1 |
| `OUT_OF_BOUNDS` | 2 | 1 |
| `OVERRIDE_CLEAR_FAILED` | 2 | 1 |
| `OVERWRITE_UNSAFE` | 2 | 1 |
| `PACKAGE_CREATE_FAILED` | 2 | 1 |
| `PACKAGE_FAILED` | 2 | 2 |
| `PARENT_NOT_FOUND` | 2 | 2 |
| `PHYSICS_ASSET_NOT_FOUND` | 2 | 1 |
| `PROPERTY_CONVERSION_FAILED` | 2 | 2 |
| `PROPERTY_EXPORT_FAILED` | 2 | 1 |
| `PYTHON_NOT_AVAILABLE` | 2 | 2 |
| `REMOVE_FAILED` | 2 | 2 |
| `RENDER_TARGET_CREATE_FAILED` | 2 | 2 |
| `REPLACE_FAILED` | 2 | 2 |
| `RESOLUTION_FAILED` | 2 | 1 |
| `RIG_CLASS_NOT_FOUND` | 2 | 2 |
| `ROW_NOT_FOUND` | 2 | 1 |
| `SCALABILITY_CVAR_USE_TYPED_VERB` | 2 | 2 |
| `SECTION_FAILED` | 2 | 2 |
| `SECTION_NOT_FOUND` | 2 | 2 |
| `SESSION_NOT_FOUND` | 2 | 2 |
| `SIMULATION_STAGE_CREATE_FAILED` | 2 | 1 |
| `SIMULATION_STAGE_INVALID_INDEX` | 2 | 1 |
| `SKELETON_HAS_NO_BONES` | 2 | 2 |
| `SOCKET_EXISTS` | 2 | 1 |
| `SOURCE_STATE_NOT_FOUND` | 2 | 2 |
| `SPAWNABLE_CREATION_FAILED` | 2 | 2 |
| `STATE_CREATE_FAILED` | 2 | 2 |
| `STATE_MACHINE_CREATE_FAILED` | 2 | 2 |
| `STRUCT_FIELD_ADD_FAILED` | 2 | 1 |
| `SUBMIX_NOT_AVAILABLE` | 2 | 1 |
| `SUN_NOT_FOUND` | 2 | 1 |
| `SURFACE_NOT_FOUND` | 2 | 1 |
| `SURFACE_NOT_SUPPORTED` | 2 | 1 |
| `SYSTEM_VIEW_MODEL_UNAVAILABLE` | 2 | 1 |
| `TARGET_AMBIGUOUS` | 2 | 2 |
| `TARGET_NOT_FOUND` | 2 | 2 |
| `TARGET_STATE_NOT_FOUND` | 2 | 2 |
| `TEMP_FILE_WRITE_FAILED` | 2 | 2 |
| `TESSELLATION_FAILED` | 2 | 1 |
| `TICKET_NOT_FOUND` | 2 | 1 |
| `TILE_BUDGET_EXCEEDED` | 2 | 2 |
| `TRACE_NOT_FOUND` | 2 | 1 |
| `TRACK_CREATE_FAILED` | 2 | 2 |
| `TRACK_NAME_NOT_APPLIED` | 2 | 1 |
| `TRANSITION_CREATE_FAILED` | 2 | 2 |
| `TRANSITION_NOT_FOUND` | 2 | 1 |
| `TREE_EMPTY` | 2 | 2 |
| `UNKNOWN_EFFECT` | 2 | 1 |
| `UNKNOWN_GENERATOR` | 2 | 1 |
| `UNKNOWN_QUALITY` | 2 | 2 |
| `UNKNOWN_TYPE` | 2 | 2 |
| `UNSUPPORTED_ARGUMENT` | 2 | 2 |
| `UNSUPPORTED_CHANNEL` | 2 | 2 |
| `UNSUPPORTED_COLUMN` | 2 | 1 |
| `UNSUPPORTED_PROPERTY` | 2 | 1 |
| `UNSUPPORTED_VALUE_TYPE` | 2 | 1 |
| `UV_GENERATION_FAILED` | 2 | 1 |
| `WEB_QUERY_FAILED` | 2 | 1 |
| `ACTION_FAILED` | 1 | 1 |
| `ACTOR_BURIED` | 1 | 1 |
| `ACTOR_LABEL_NOT_EDITABLE` | 1 | 1 |
| `ACTOR_LOAD_FAILED` | 1 | 1 |
| `ACTOR_LOCATION_LOCKED` | 1 | 1 |
| `ACTOR_SKELETAL_MESH_ASSET_NULL` | 1 | 1 |
| `ACTOR_SPAWN_FAILED` | 1 | 1 |
| `ADD_CAMERA_FAILED` | 1 | 1 |
| `ADD_PLAYER_FAILED` | 1 | 1 |
| `ADD_SOURCE_FAILED` | 1 | 1 |
| `AGIR_DECOMPILE_FAILED` | 1 | 1 |
| `AIMOFFSET_NOT_FOUND` | 1 | 1 |
| `ALIAS_CREATE_FAILED` | 1 | 1 |
| `AMBIGUOUS_EMITTER_HANDLE` | 1 | 1 |
| `AMBIGUOUS_INSTANCED_COMPONENT` | 1 | 1 |
| `AMBIGUOUS_NODE` | 1 | 1 |
| `AMBIGUOUS_SOURCE` | 1 | 1 |
| `ANIM_COMPILE_FAILED` | 1 | 1 |
| `ANIM_FILE_NOT_FOUND` | 1 | 1 |
| `ANIM_LOAD_FAILED` | 1 | 1 |
| `ANIM_PARSE_FAILED` | 1 | 1 |
| `ASSET_REGISTRY_UNAVAILABLE` | 1 | 1 |
| `ATTRIBUTE_NOT_FOUND` | 1 | 1 |
| `AUDIO_MULTICHANNEL_UNSUPPORTED` | 1 | 1 |
| `AUDIO_NON_FINITE_SAMPLES` | 1 | 1 |
| `AUDIT_TRACE_BUDGET_EXHAUSTED` | 1 | 1 |
| `BAKE_FAILED` | 1 | 1 |
| `BINDING_PARENT_NOT_SET` | 1 | 1 |
| `BLEND_CURVE_NOT_FOUND` | 1 | 1 |
| `BODY_SETUP_FAILED` | 1 | 1 |
| `BONE_EXISTS` | 1 | 1 |
| `BONE_MASK_NOT_FOUND` | 1 | 1 |
| `BONE_NOT_IN_SECTION` | 1 | 1 |
| `BOOKMARK_EMPTY` | 1 | 1 |
| `BOOKMARK_SET_FAILED` | 1 | 1 |
| `BPIR_REQUIRED` | 1 | 1 |
| `BTIR_ASSET_NOT_FOUND` | 1 | 1 |
| `BTIR_DECOMPILE_FAILED` | 1 | 1 |
| `BULK_DELETE_FAILED` | 1 | 1 |
| `BULK_RENAME_FAILED` | 1 | 1 |
| `CAMERA_LOAD_FAILED` | 1 | 1 |
| `CAMERA_NOT_BOUND` | 1 | 1 |
| `CANNOT_RELOAD_ACTIVE_LEVEL` | 1 | 1 |
| `CANNOT_REMOVE_ROOT` | 1 | 1 |
| `CAPTURE_CAMERA_NOT_APPLIED` | 1 | 1 |
| `CAST_FAILED` | 1 | 1 |
| `CDO_FAILED` | 1 | 1 |
| `CHAIN_MAP_NOT_APPLIED` | 1 | 1 |
| `CHAIN_NOT_ADDED` | 1 | 1 |
| `CHAIN_NOT_FOUND` | 1 | 1 |
| `CHECKOUT_FAILED` | 1 | 1 |
| `CLASS_MISMATCH` | 1 | 1 |
| `CLOTH_CREATE_FAILED` | 1 | 1 |
| `CLOTH_NAME_IN_USE` | 1 | 1 |
| `CLOTH_NOT_FOUND` | 1 | 1 |
| `COMPILE_FAILED` | 1 | 1 |
| `COMPONENT_CREATION_FAILED` | 1 | 1 |
| `COMPOSITE_NOT_FOUND` | 1 | 1 |
| `CONFIG_OPERATION_MISMATCH` | 1 | 1 |
| `CONFIG_READ_FAILED` | 1 | 1 |
| `CONFIG_TARGET_MISMATCH` | 1 | 1 |
| `CONNECTION_DISALLOWED` | 1 | 1 |
| `CONNECT_FAILED` | 1 | 1 |
| `CONSTRAINT_NOT_FOUND` | 1 | 1 |
| `CONSTRUCTION_FAILED` | 1 | 1 |
| `CONTROL_NOT_FOUND` | 1 | 1 |
| `CREATE_ASSET_FAILED` | 1 | 1 |
| `CREATE_NODE_FAILED` | 1 | 1 |
| `CREATE_TRACK_FAILED` | 1 | 1 |
| `CRIR_ASSET_NOT_FOUND` | 1 | 1 |
| `CURVE_ASSET_NOT_FOUND` | 1 | 1 |
| `CYCLE_DETECTED` | 1 | 1 |
| `DATALAYER_NOT_FOUND` | 1 | 1 |
| `DATA_INTERFACE_CLASS_NOT_FOUND` | 1 | 1 |
| `DECOMPILE_FAILED` | 1 | 1 |
| `DEFAULT_PROPERTY_NOT_FOUND` | 1 | 1 |
| `DELETE_FAILED` | 1 | 1 |
| `DERIVED_PROPERTY` | 1 | 1 |
| `DESTINATION_EXISTS` | 1 | 1 |
| `DESTINATION_FOLDER_NOT_FOUND` | 1 | 1 |
| `DETACH_FAILED` | 1 | 1 |
| `DUPLICATE_TIMELINE` | 1 | 1 |
| `EDGE_FAILED` | 1 | 1 |
| `EDGE_NOT_FOUND` | 1 | 1 |
| `EDITOR_IN_USE` | 1 | 1 |
| `EDITOR_NOT_FOUND` | 1 | 1 |
| `EDITOR_NOT_OPEN` | 1 | 1 |
| `EDITOR_SUBSYSTEM_MISSING` | 1 | 1 |
| `EFFECT_CLASS_NOT_FOUND` | 1 | 1 |
| `ELEMENT_NOT_FOUND` | 1 | 1 |
| `EMITTER_GRAPH_SOURCE_MISSING` | 1 | 1 |
| `EMITTER_ONLY_UNSUPPORTED` | 1 | 1 |
| `ENUM_NOT_FOUND` | 1 | 1 |
| `ENUM_NOT_RESOLVED` | 1 | 1 |
| `ENUM_VALUE_NOT_FOUND` | 1 | 1 |
| `EVAL_FAILED` | 1 | 1 |
| `EVENT_HANDLER_NOT_FOUND` | 1 | 1 |
| `EXECUTION_FAILED` | 1 | 1 |
| `EXPOSURE_PIN_FAILED` | 1 | 1 |
| `EXPRESSION_NOT_FOUND` | 1 | 1 |
| `FACTORY_FAILED` | 1 | 1 |
| `FACTORY_NOT_AVAILABLE` | 1 | 1 |
| `FOLIAGE_ACTOR_NOT_FOUND` | 1 | 1 |
| `GEOREFERENCE_MISMATCH` | 1 | 1 |
| `GRAPH_ERROR` | 1 | 1 |
| `GRID_TOO_DENSE` | 1 | 1 |
| `GROUND_SEAT_READBACK_MISMATCH` | 1 | 1 |
| `HOST_FAILED` | 1 | 1 |
| `IMAGE_GRID_MISMATCH` | 1 | 1 |
| `IMAGE_SIZE_MISMATCH` | 1 | 1 |
| `INCOMPATIBLE_CURVE_ASSET` | 1 | 1 |
| `INDEX_MALFORMED` | 1 | 1 |
| `INDEX_NOT_FOUND` | 1 | 1 |
| `INDEX_PARSE_ERROR` | 1 | 1 |
| `INPUT_ACTION_PROPERTY_NOT_FOUND` | 1 | 1 |
| `INSUFFICIENT_GROUND_CONTACT` | 1 | 1 |
| `INTERFACE_MUTATION_FAILED` | 1 | 1 |
| `INVALID_ACTOR_LABEL` | 1 | 1 |
| `INVALID_ANIM_NODE_CLASS` | 1 | 1 |
| `INVALID_BLUEPRINT_CANDIDATES` | 1 | 1 |
| `INVALID_BLUEPRINT_TYPE` | 1 | 1 |
| `INVALID_BONE_MODIFICATION_MODE` | 1 | 1 |
| `INVALID_CATEGORY` | 1 | 1 |
| `INVALID_CHILD_INDEX` | 1 | 1 |
| `INVALID_CLASS_TYPE` | 1 | 1 |
| `INVALID_COLLISION_COMPLEXITY` | 1 | 1 |
| `INVALID_COMPILE_FLAG` | 1 | 1 |
| `INVALID_DATATABLE` | 1 | 1 |
| `INVALID_DRAW_AS` | 1 | 1 |
| `INVALID_EFFECT_CLASS` | 1 | 1 |
| `INVALID_INHIBITION_POLICY` | 1 | 1 |
| `INVALID_INTERPOLATION_TYPE` | 1 | 1 |
| `INVALID_KEY` | 1 | 1 |
| `INVALID_KIND` | 1 | 1 |
| `INVALID_LIGHT_TYPE` | 1 | 1 |
| `INVALID_LOD_INDEX` | 1 | 1 |
| `INVALID_LOGIC_TYPE` | 1 | 1 |
| `INVALID_MATERIAL_INDEX` | 1 | 1 |
| `INVALID_OPERATIONS` | 1 | 1 |
| `INVALID_OPERATION_PAYLOAD` | 1 | 1 |
| `INVALID_OPERATION_TYPE` | 1 | 1 |
| `INVALID_PARAM` | 1 | 1 |
| `INVALID_PARAMETER_TYPE` | 1 | 1 |
| `INVALID_PARTITION_TYPE` | 1 | 1 |
| `INVALID_PROVIDER` | 1 | 1 |
| `INVALID_PRUNING_TYPE` | 1 | 1 |
| `INVALID_QUERY` | 1 | 1 |
| `INVALID_RESOLUTION_RULE` | 1 | 1 |
| `INVALID_ROW_STRUCT` | 1 | 1 |
| `INVALID_ROW_VALUES` | 1 | 1 |
| `INVALID_SAVE_FLAG` | 1 | 1 |
| `INVALID_SEGMENT_ANIMATION` | 1 | 1 |
| `INVALID_SHAPE` | 1 | 1 |
| `INVALID_SIMULATION_STAGE_CLASS` | 1 | 1 |
| `INVALID_SOUND_GROUP` | 1 | 1 |
| `INVALID_STAGE` | 1 | 1 |
| `INVALID_STRUCT` | 1 | 1 |
| `INVALID_SUBGRAPH_ASSET` | 1 | 1 |
| `INVALID_THRESHOLD` | 1 | 1 |
| `INVALID_TRANSFORM_PAYLOAD` | 1 | 1 |
| `INVALID_TRIANGLE` | 1 | 1 |
| `INVALID_VISIBILITY` | 1 | 1 |
| `INVALID_WHEEL_CLASS` | 1 | 1 |
| `JOB_NOT_RUNNING` | 1 | 1 |
| `LANDSCAPE_INVALID_TOOL_MODE` | 1 | 1 |
| `LANDSCAPE_MATERIAL_NO_LAYERS` | 1 | 1 |
| `LANDSCAPE_NO_MATERIAL` | 1 | 1 |
| `LANDSCAPE_ORPHANED_LAYER_WEIGHT` | 1 | 1 |
| `LANDSCAPE_SCULPT_NO_CHANGE` | 1 | 1 |
| `LAYER_NOT_FOUND` | 1 | 1 |
| `LEVEL_LOCKED` | 1 | 1 |
| `LEVEL_NOT_PERSISTED` | 1 | 1 |
| `LIVE_CODING_COMPILE_IN_PROGRESS` | 1 | 1 |
| `LIVE_CODING_NOT_ENABLED` | 1 | 1 |
| `MANAGER_NOT_FOUND` | 1 | 1 |
| `MARK_DIRTY_REFUSED` | 1 | 1 |
| `MERGE_TOOL_UNAVAILABLE` | 1 | 1 |
| `MESH_APPEND_FAILED` | 1 | 1 |
| `MESH_AUDIT_COMPONENT_UNKNOWN` | 1 | 1 |
| `MESH_AUDIT_DEGENERATE` | 1 | 1 |
| `MESH_AUDIT_EMPTY` | 1 | 1 |
| `MESH_AUDIT_FLOATING_COMPONENT` | 1 | 1 |
| `MESH_AUDIT_INCONSISTENT_WINDING` | 1 | 1 |
| `MESH_AUDIT_INVERTED` | 1 | 1 |
| `MESH_AUDIT_MIRRORED_BUILD_SCALE` | 1 | 1 |
| `MESH_AUDIT_NON_MANIFOLD` | 1 | 1 |
| `MESH_AUDIT_NOT_CLOSED` | 1 | 1 |
| `MESH_AUDIT_THIN_SHELL` | 1 | 1 |
| `MESH_AUDIT_UNLOADABLE` | 1 | 1 |
| `MESH_AUDIT_Z_FIGHTING` | 1 | 1 |
| `MESH_DUPLICATE_SUBSTITUTED` | 1 | 1 |
| `MESH_EMPTY` | 1 | 1 |
| `MESH_REBUILD_CONSUMER_NOT_QUIESCABLE` | 1 | 1 |
| `METASOUND_SEARCH_NOT_AVAILABLE` | 1 | 1 |
| `MGIR_DECOMPILE_FAILED` | 1 | 1 |
| `MISSING_ANIMATION_PATH` | 1 | 1 |
| `MISSING_ASSET_PATH` | 1 | 1 |
| `MISSING_BONES` | 1 | 1 |
| `MISSING_CACHE_NAME` | 1 | 1 |
| `MISSING_CHAINS` | 1 | 1 |
| `MISSING_CHAIN_NAME` | 1 | 1 |
| `MISSING_CONTROL` | 1 | 1 |
| `MISSING_CONTROLS` | 1 | 1 |
| `MISSING_CURVE_NAME` | 1 | 1 |
| `MISSING_NODE_TYPE` | 1 | 1 |
| `MISSING_SECTIONS` | 1 | 1 |
| `MISSING_SKELETON_PATH` | 1 | 1 |
| `MISSING_SLOT_NAME` | 1 | 1 |
| `MISSING_STATES` | 1 | 1 |
| `MISSING_STATE_MACHINE_NAME` | 1 | 1 |
| `MISSING_STATE_NAME` | 1 | 1 |
| `MODEL_COMPILE_FAILED` | 1 | 1 |
| `MODEL_FILE_NOT_FOUND` | 1 | 1 |
| `MODEL_PARSE_FAILED` | 1 | 1 |
| `MORPH_NOT_PERSISTED` | 1 | 1 |
| `MORPH_TARGET_NOT_FOUND` | 1 | 1 |
| `MRQ_JOB_ALLOCATION_FAILED` | 1 | 1 |
| `MRQ_PRESET_NOT_LOADABLE` | 1 | 1 |
| `MRQ_QUEUE_NULL` | 1 | 1 |
| `NIAGARA_DATA_INTERFACE_UNVERIFIED` | 1 | 1 |
| `NIAGARA_EMITTER_NOT_IN_SYSTEM_GRAPH` | 1 | 1 |
| `NIAGARA_ORPHAN_REMOVAL_UNSAFE` | 1 | 1 |
| `NIR_DECOMPILE_FAILED` | 1 | 1 |
| `NODES_NOT_FOUND` | 1 | 1 |
| `NODE_CLASS_NOT_FOUND` | 1 | 1 |
| `NODE_NOT_ASSET_PLAYER` | 1 | 1 |
| `NOTHING_TO_ANNOTATE` | 1 | 1 |
| `NOTHING_TO_UNDO` | 1 | 1 |
| `NOT_AN_ARRAY` | 1 | 1 |
| `NOT_AN_EFFECT_CALCULATION` | 1 | 1 |
| `NOT_A_GAMEPLAY_ABILITY` | 1 | 1 |
| `NOT_A_MAP` | 1 | 1 |
| `NOT_A_PAWN` | 1 | 1 |
| `NOT_A_SET` | 1 | 1 |
| `NOT_A_SPLINE` | 1 | 1 |
| `NOT_A_UTILITY_BLUEPRINT` | 1 | 1 |
| `NOT_A_UTILITY_WIDGET_BP` | 1 | 1 |
| `NOT_A_WHEEL_ASSET` | 1 | 1 |
| `NOT_IN_PIE` | 1 | 1 |
| `NOT_METASOUND` | 1 | 1 |
| `NOT_NIAGARA_ASSET` | 1 | 1 |
| `NOT_PERIODIC` | 1 | 1 |
| `NO_ACTIVE_WIDGET` | 1 | 1 |
| `NO_ACTORS_MATCHED` | 1 | 1 |
| `NO_BLUEPRINT` | 1 | 1 |
| `NO_CONTROLS_KEYED` | 1 | 1 |
| `NO_INSTANCED_COMPONENT` | 1 | 1 |
| `NO_NODE_SETTINGS` | 1 | 1 |
| `NO_PCG_GRAPH` | 1 | 1 |
| `NO_PLAYER_CONTROLLER` | 1 | 1 |
| `NO_SCS` | 1 | 1 |
| `NO_SKEL_MESH_COMP` | 1 | 1 |
| `NO_SKIN_WEIGHTS` | 1 | 1 |
| `NO_SMART_LINK` | 1 | 1 |
| `NO_SOURCE_WEIGHTS` | 1 | 1 |
| `NO_TRANSFORM_SECTION` | 1 | 1 |
| `NO_TRANSFORM_TRACK` | 1 | 1 |
| `NO_WORLD_SETTINGS` | 1 | 1 |
| `NULL_COMPONENT` | 1 | 1 |
| `OPERATION_NOT_SUPPORTED` | 1 | 1 |
| `OPERATION_SKIPPED` | 1 | 1 |
| `OUTPUT_FAILED` | 1 | 1 |
| `PANEL_NOT_REALIZED` | 1 | 1 |
| `PARAMETER_SET_FAILED` | 1 | 1 |
| `PARENT_CLASS_NOT_FOUND` | 1 | 1 |
| `PARENT_CYCLE` | 1 | 1 |
| `PARENT_REQUIRED` | 1 | 1 |
| `PARENT_SUBMIX_NOT_FOUND` | 1 | 1 |
| `PCGIR_DECOMPILE_FAILED` | 1 | 1 |
| `PIE_ACTIVE` | 1 | 1 |
| `PIE_STOP_FAILED` | 1 | 1 |
| `PIN_REMAP_INVALID` | 1 | 1 |
| `PIXEL_STATS_UNAVAILABLE` | 1 | 1 |
| `PLAYER_NOT_FOUND` | 1 | 1 |
| `PLUGIN_NOT_FOUND` | 1 | 1 |
| `POSSESS_FAILED` | 1 | 1 |
| `PRESET_NOT_APPLIED` | 1 | 1 |
| `PRESET_NOT_FOUND` | 1 | 1 |
| `PROFILE_NOT_FOUND` | 1 | 1 |
| `PROPERTY_NOT_INT` | 1 | 1 |
| `PROPERTY_NOT_SUPPORTED` | 1 | 1 |
| `PROPERTY_PATH_FAILED` | 1 | 1 |
| `PROPERTY_WRONG_TYPE` | 1 | 1 |
| `QUEUE_EMPTY` | 1 | 1 |
| `RECURSIVE_SUBGRAPH` | 1 | 1 |
| `RELOAD_FAILED` | 1 | 1 |
| `REVERB_NOT_AVAILABLE` | 1 | 1 |
| `REVERT_REQUIRES_UNLOADED_PACKAGE` | 1 | 1 |
| `ROOT_MISSING` | 1 | 1 |
| `ROWS_PRESENT_FORCE_REQUIRED` | 1 | 1 |
| `ROW_EXISTS` | 1 | 1 |
| `RUNTIME_ONLY_COMMAND` | 1 | 1 |
| `SAMPLE_REJECTED` | 1 | 1 |
| `SAVE_DISK_STATE_DIVERGED` | 1 | 1 |
| `SAVE_VERIFICATION_FAILED` | 1 | 1 |
| `SCENE_BOUNDS_NOT_MEASURED` | 1 | 1 |
| `SCHEMA_FINALIZE_FAILED` | 1 | 1 |
| `SCHEMA_NOT_SET` | 1 | 1 |
| `SCIR_DECOMPILE_FAILED` | 1 | 1 |
| `SCS_COMPONENT_NOT_FOUND` | 1 | 1 |
| `SCS_NOT_FOUND` | 1 | 1 |
| `SCS_OPERATION_FAILED` | 1 | 1 |
| `SCS_PARENT_NOT_FOUND` | 1 | 1 |
| `SCS_UNAVAILABLE` | 1 | 1 |
| `SEQUENCE_NOT_OPEN` | 1 | 1 |
| `SESSION_LOAD_FAILED` | 1 | 1 |
| `SET_FAILED` | 1 | 1 |
| `SET_PARENT_FAILED` | 1 | 1 |
| `SET_TIME_FAILED` | 1 | 1 |
| `SIMULATION_STAGE_CLASS_NOT_FOUND` | 1 | 1 |
| `SIMULATION_STAGE_NOT_FOUND` | 1 | 1 |
| `SKIN_WEIGHTS_INCOMPLETE` | 1 | 1 |
| `SKYLIGHT_NOT_FOUND` | 1 | 1 |
| `SNAPSHOT_NOT_FOUND` | 1 | 1 |
| `SOUNDWAVE_NOT_FOUND` | 1 | 1 |
| `SOUND_LOAD_FAILED` | 1 | 1 |
| `SOUND_WAVE_NOT_FOUND` | 1 | 1 |
| `SOURCE_EFFECT_NOT_AVAILABLE` | 1 | 1 |
| `SOURCE_NODE_NOT_FOUND` | 1 | 1 |
| `SOURCE_NOT_FOUND` | 1 | 1 |
| `SPEAKER_NOT_FOUND` | 1 | 1 |
| `SPLINE_COMPONENT_NOT_FOUND` | 1 | 1 |
| `SPLINE_NOT_FOUND` | 1 | 1 |
| `STACK_EMPTY` | 1 | 1 |
| `STATE_NOT_FOUND` | 1 | 1 |
| `STRUCT_FIELD_REMOVE_FAILED` | 1 | 1 |
| `SUBMIT_FAILED` | 1 | 1 |
| `SUBMIX_NOT_FOUND` | 1 | 1 |
| `SUB_SECTION_CREATE_FAILED` | 1 | 1 |
| `SUB_SECTION_NOT_FOUND` | 1 | 1 |
| `SURFACE_TRACE_MISSED` | 1 | 1 |
| `TANGENTS_FAILED` | 1 | 1 |
| `TARGET_CHANGED` | 1 | 1 |
| `TARGET_NODE_NOT_FOUND` | 1 | 1 |
| `TEXTURE_ERROR` | 1 | 1 |
| `TEXTURE_NOT_FOUND` | 1 | 1 |
| `THUMBNAIL_GENERATION_FAILED` | 1 | 1 |
| `TRANSFORM_MISMATCH` | 1 | 1 |
| `TREE_PROGRAMMATIC` | 1 | 1 |
| `TYPE_MISMATCH` | 1 | 1 |
| `UBT_NOT_FOUND` | 1 | 1 |
| `UNDO_NOT_REVERSIBLE` | 1 | 1 |
| `UNKNOWN_NODE_TYPE` | 1 | 1 |
| `UNKNOWN_STREAMING_METHOD` | 1 | 1 |
| `UNSAVED_CHANGES` | 1 | 1 |
| `UNSUPPORTED_BLUEPRINT` | 1 | 1 |
| `UNSUPPORTED_KEY_TYPE` | 1 | 1 |
| `UNSUPPORTED_OPTION` | 1 | 1 |
| `UNSUPPORTED_ORTHOGRAPHIC_ROTATION` | 1 | 1 |
| `UNSUPPORTED_PARAM_TYPE` | 1 | 1 |
| `UNSUPPORTED_RESULT` | 1 | 1 |
| `UNSUPPORTED_SHAPE` | 1 | 1 |
| `UNSUPPORTED_SOURCE_KIND` | 1 | 1 |
| `UNSUPPORTED_TARGET` | 1 | 1 |
| `UV_LAYER_ERROR` | 1 | 1 |
| `VALIDATION_FAILED` | 1 | 1 |
| `VARIABLE_FAILED` | 1 | 1 |
| `VBONE_NOT_FOUND` | 1 | 1 |
| `VIEWPORT_WORLD_MISMATCH` | 1 | 1 |
| `VIEW_MODE_NEEDS_COMPANION` | 1 | 1 |
| `VIEW_MODE_UNAVAILABLE` | 1 | 1 |
| `VIRTUAL_BONE_FAILED` | 1 | 1 |
| `VISIBILITY_MISMATCH` | 1 | 1 |
| `VOICE_NOT_FOUND` | 1 | 1 |
| `WEB_BROWSER_NOT_FOUND` | 1 | 1 |
| `WIDGET_BINDING_IS_MULTICAST_EVENT` | 1 | 1 |
| `WIDGET_BINDING_NAME_UNRESOLVED` | 1 | 1 |
| `WINDOW_MAXIMIZED` | 1 | 1 |
| `WINDOW_NOT_FOUND` | 1 | 1 |
| `WRONG_ACTOR_CLASS` | 1 | 1 |
| `WRONG_EXPRESSION_TYPE` | 1 | 1 |

## Regeneration

One-shot scan, no committed script — run inline from the plugin root. Comments are stripped
first, so a code named only in a comment is not counted. An `ErrorCodes::ERR_*` name with no
matching declaration raises `KeyError` instead of being counted under its own name: the
registry is meant to be complete, so that failure is the point.

```python
# Pipe into the engine's bundled interpreter from the plugin root -- never uv, and never
# install anything into it. Stdlib only:
#   & "$env:UE_ROOT\Engine\Binaries\ThirdParty\Python3\Win64\python.exe" -
import os, re, glob, collections
HEADER = "Source/PinWright/Private/Handlers/ErrorCodes.h"
consts = dict(re.findall(r'ERR_([A-Za-z0-9_]+)\s*\[\s*\]\s*=\s*TEXT\(\s*"([A-Za-z_][A-Za-z0-9_]*)"\s*\)',
                         open(HEADER, encoding='utf-8', errors='replace').read()))
pats = [re.compile(r'SendError\s*\(\s*TEXT\(\s*"([A-Za-z_][A-Za-z0-9_]*)"\s*\)'),
        re.compile(r'SendError\s*\(\s*"([A-Za-z_][A-Za-z0-9_]*)"\s*,'),
        re.compile(r'ErrorCodes::ERR_([A-Za-z0-9_]+)')]
comment = re.compile(r'/\*.*?\*/|//[^\n]*', re.S)
counts, files = collections.Counter(), collections.defaultdict(set)
for root in sorted(glob.glob("Source/PinWright*/Private/Handlers")):
    for dp, _, ns in os.walk(root):
        if os.sep + "Tests" + os.sep in dp + os.sep: continue
        for n in ns:
            if not n.endswith((".cpp", ".h")): continue
            path = os.path.join(dp, n).replace("\\", "/")
            if path == HEADER: continue
            txt = comment.sub("", open(path, encoding='utf-8', errors='replace').read())
            for i, p in enumerate(pats):
                for m in p.finditer(txt):
                    c = consts[m.group(1)] if i == 2 else m.group(1)
                    counts[c] += 1; files[c].add(path)
for c, n in sorted(counts.items(), key=lambda kv: (-kv[1], kv[0])):
    print("| `%s` | %d | %d |" % (c, n, len(files[c])))
print(len(counts), sum(counts.values()), len(set().union(*files.values())))
```

The trailing line prints unique codes, total callsites, and distinct emitting files — the
three figures in the bullets at the top.
