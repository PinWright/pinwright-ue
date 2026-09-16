---
type: system
summary: "Convergence of the eight capture verbs onto one viewport primitive plus a pluggable subject resolver, so every capture capability reaches every subject domain (world, actor, static mesh, skeletal mesh, animation, Niagara). Corrected reachability matrix, the FCaptureSubject contract, thirteen file-disjoint chunks for one wave, and what is structurally excluded."
date: 2026-08-21
tags: [capture, render, camera, viewport, niagara, subject-resolver]
---

# Capture convergence: one primitive, one subject resolver

Ordering below is **hard dependency only**, per [format-decisions.md](format-decisions.md). There are no
milestones. Where two chunks are independent, that is stated and they run together.

## Decisions

1. **The verbs stay; the subject becomes a parameter.** All eight capture verbs keep their registered
   names, their existing arguments and their existing response keys. Each grows one optional
   `subject` object (§2). Existing spellings — `actorName`, `assetPath`, `point`, `sequencePath` —
   remain accepted and are normalised into a `FCaptureSubject` internally, so no wire contract moves.
2. **The missing layer is acquisition, not capture.** `PinWrightRenderCapture::CaptureEditorViewportToPng`
   (`PreviewViewportCaptureUtils.h:1119-1127`) is already subject-agnostic and is already the single
   renderer for all seven viewport verbs. What is duplicated is the step *above* it: getting a
   viewport client, bounds, and a time setter for a named thing. That step exists in **four**
   independent copies today (§0, claim 6).
3. **Subject kinds register themselves; no central switch.** A provider is a static-init
   registration, exactly the `REGISTER_RPC_HANDLER` / `FAutoRegisterHandler::GetPendingRegistrations()`
   pattern the dispatcher already uses (`Dispatch/RpcDispatcher.cpp`). This is what makes the chunks
   in §5 file-disjoint: adding the Niagara kind creates one file and edits none.
4. **Bounds come from a static source, never from the posed/simulated subject.** Two correct
   implementations already exist and the plan keeps both, selected by the provider:
   *asset bounds* (`AnimationPreviewCaptureHandler.cpp:715`, `SkeletalMesh->GetBounds()`) and
   *union of sampled bounds* (`AnimationShotsHandler.cpp:515-537`). A Niagara subject uses a third:
   bounds pinned for the duration via `UNiagaraComponent::SetSystemFixedBounds`, because its dynamic
   bounds refresh only when the box is escaped or every 5 s (§2.4).
5. **No new error codes.** Every refusal the design needs already has a constant:
   `ERR_UNSUPPORTED_ASSET_EDITOR:1089`, `ERR_PREVIEW_VIEWPORT_NOT_FOUND:878`,
   `ERR_NO_ACTIVE_LEVEL_VIEWPORT:771`, `ERR_BOUNDS_EMPTY:224`, `ERR_ACTOR_NO_SKELETAL_MESH_COMPONENT:46`
   (`Handlers/ErrorCodes.h`). This keeps `ErrorCodes.h` and the regenerated
   `docs/error-code-catalog.md` out of every chunk's file list — the single largest source of
   cross-chunk conflict in this repo's history (`commit-grouping.md` § hard ordering facts).
6. **A capability a subject kind cannot support is a typed refusal, not silence.** "Every capability
   in every domain" is satisfied by a verb that answers, not by one that pretends. `camera.animation_shots`
   against a static mesh must return `ERR_UNSUPPORTED_ASSET_EDITOR` naming the missing time axis —
   today it returns `UNKNOWN_PARAMS` from the dispatcher's arg gate, which tells the caller nothing.
7. **`FCameraPose::SubjectTimeSeconds` is the time seam, and it already exists.** Declared and
   deliberately dead at `PoseListCapture.h:62-77`, with the three-kind layering written into its
   comment. The plan implements the driver it anticipates rather than inventing a second mechanism.
8. **`render.capture_ortho_tiles` does not join, and the reason is narrower than "it is a different
   renderer".** §4.1 states exactly which view-mode capabilities are unreachable on that path and
   which are in fact reachable — the overstated version of this exclusion would itself be a defect.
9. **Nothing in this plan claims particle reproducibility.** §4.2. Niagara determinism is off by
   default at all three scopes and is void under a variable tick delta; the verbs report what they
   did instead of promising repeatability.

---

## §0 Verification of the brief

Every row was read against the handler source on 2026-08-21. Line numbers are against a tree with an
in-flight crash fix (§7); re-read before cutting.

| Claim in the brief | Verdict | Evidence |
|---|---|---|
| `CaptureCameraPoses` is subject-agnostic; a preview scene and the level viewport are the same to it | **True** | `PoseListCapture.cpp` is 158 lines with zero references to `AActor`, `UWorld`, `ULevel` or `GEditor`; includes are `ErrorCodes.h`, `JsonObject.h`, `FileManager.h` only. Signature `PoseListCapture.h:143-149` |
| Only `camera.orbit_shots` calls it | **True** | Sole call site `CameraFrameHandler.cpp:593`. No test references it |
| `camera.animation_shots` serves the **animation-asset** domain | **False, and this is the brief's largest error** | It requires a *placed level actor* **and** a Level Sequence asset path, and scrubs through `ULevelSequenceEditorBlueprintLibrary::SetGlobalPosition` (`AnimationShotsHandler.cpp:133-160`, `Sequencer/SequencePlayheadUtils.h:94-122`). It is a **world/actor** verb. The animation-asset domain is served by `render.capture_animation_preview` alone |
| Animation bounds come from the mesh asset, never the posed component | **True of one verb, false of the other, and both are correct** | `render.capture_animation_preview` uses `SkeletalMesh->GetBounds()` (`AnimationPreviewCaptureHandler.cpp:715`) with the rationale at `:42-44`. `camera.animation_shots` unions `GetActorBounds` across every sampled instant in a no-capture pre-pass (`AnimationShotsHandler.cpp:515-537`), rationale at `:23-29`. Two solutions to one problem; the resolver must express both |
| `capture_asset_preview` publishes only `assetPath` | **False** | Six verb-specific keys: `framing` (a full block, `RenderHandler.cpp:495-503`), `assetPath`, `target`, `captureSource`, `assetEditorWasAlreadyOpen`, `assetEditorClosed` (`:504-508`), on top of `AddCaptureFields` |
| `capture_animation_preview` publishes `count`/`angles`/`views`/`frames` | **Half true** | `count` (`:891`) and `frames` (`:892`) exist. **`angles` and `views` do not exist as response keys.** Angles appear per shot as `shots[i].angle`; the view-plan size is published as `viewCount` (`:894`) |
| Two verbs still carry their own camera loops | **Understated — there are six independent call sites** | Own multi-shot loops: `AnimationShotsHandler.cpp:589-662`, `AnimationPreviewCaptureHandler.cpp:763-886`. Own single-shot request assembly: `CameraFrameHandler.cpp:147-192`, `RenderHandler.cpp:472` and `:613`, `AnnotatedCaptureHandler.cpp:365` |
| The animation handler independently diagnosed and fixed the orbit-camera defect | **True, and it is the plan's motivating evidence** | `AnimationPreviewCaptureHandler.cpp:730-744` hand-rolls the orbit save/suppress/restore that `CaptureEditorViewportToPng` already performs at `PreviewViewportCaptureUtils.cpp:1431-1461`. `CameraFrameHandler.cpp:525-532` records the same defect being found twice |
| The first capture into a fresh preview window is ~0.9 stop dark and `warmup.settled` returns true throughout | **True, and measured** | `docs/wiki-src/render.md:145-160`, commit `f07767c6`. `SM_Driftwood` mean 0.1987→0.3686, `SM_Amphora` 0.2491→0.3655, identical pinned `ev100: -1`, `settled: true` on all four with `meanLuminanceDelta` under 4e-4 |
| `minLuminance == 0` is a reliable positive test for a cold frame | **False as stated** | No document in the corpus proposes it as a test. It appears as *corroboration* for the ambient hypothesis on n=2 assets, beside `maxLuminance` being identical to the last digit and `litPixelFraction` moving 0.9966→1.0000. The doc names a different discriminator: "**`assetEditorWasAlreadyOpen` in the response is the field that separates a cold shot from a warm one**" (`render.md:164`). Promoting the corroboration to a detector is the error `rpc-design.md:66` prohibits |
| A batch primitive can absorb the warm-up for every caller | **True, and it already does — for one verb** | `PoseListCapture.cpp:73-103` takes a throwaway frame at `Poses[0]`, deletes it, reports `poseSet.warmupShotTaken`/`warmupShotDiscarded`. **None** of `capture_asset_preview`, `capture_animation_preview`, `capture_annotated`, `camera.frame_actor` takes one — and `capture_asset_preview` is precisely the verb that opens the window and captures in the same call |
| `capture_ortho_tiles` renders through `USceneCaptureComponent2D`, so `viewMode` cannot exist there | **True at the verb, but the stated reason is too broad** | Renderer confirmed: `OrthoTileCaptureUtils.cpp:165, 407, 419`. Zero hits for `viewMode`/`EViewModeIndex` in all five ortho files. But the component exposes a **public writable `ShowFlags`** (`SceneCaptureComponent.h:189`), and `FSceneViewFamily::ChooseDebugViewShaderMode` derives its mode from show flags alone (`SceneView.cpp:3239-3300`). See §4.1 for the exact split |
| Niagara determinism flags were not found where expected | **They exist, at three scopes, all defaulting off** | `UNiagaraSystem::bDeterminism` + `RandomSeed` (`NiagaraSystem.h:1073-1085`), `FVersionedNiagaraEmitterData::bDeterminism` + `RandomSeed` (`NiagaraEmitter.h:310-316`), `UNiagaraComponent::RandomSeedOffset` (`NiagaraComponent.h:96-103`). With system determinism off the seed is literally `FMath::Rand()` per reset (`NiagaraSystemInstance.cpp:894`). §4.2 |
| The Niagara asset-editor preview viewport is unreachable | **False — and this is the plan's cheapest win** | `FNiagaraSystemToolkit` and `SNiagaraSystemViewport` are indeed private and unexported. Irrelevant: `SNiagaraSystemViewport : public SEditorViewport` (`SNiagaraSystemViewport.h:28`), and `SEditorViewport::GetViewportClient()` / `GetSceneViewport()` are **public inline accessors in UnrealEd's public header** (`SEditorViewport.h:91, 96`). The existing widget-tree walk reaches it through the base class with no NiagaraEditor symbol. §2.5 |
| Niagara needs new module dependencies | **False** | `Niagara`, `NiagaraCore`, `NiagaraEditor` are already **public** hard dependencies (`PinWright.Build.cs:25`). 45 `niagara.*` verbs are registered. No `.Build.cs` edit is in this plan |
| The suite is red and a crash fix is in flight | **True** | Six modified files, all the `UViewModeUtils::GetViewModeDisplayName` fix. `Saved/Logs/pw_goal_suite.log` ended on `Array index out of bounds: 256 into an array of size 256` with 3423 started / 3421 passed / 1 failed — counts that cannot reconcile because the crashing test never completed. §7 |

Two further measurements that shape the design:

- **`MakeViewportInfoObject` is already universal.** All eight verbs that own a viewport publish the
  `viewport` block from the one serializer (`ViewportHandler.cpp:752`, `AnimationPreviewCaptureHandler.cpp:930`,
  `AnimationShotsHandler.cpp:707`, `AnnotatedCaptureHandler.cpp:880`, `CameraFrameHandler.cpp:201, 642`,
  `RenderHandler.cpp:187`). So `viewport.aim`, `viewport.warmup` and `viewport.viewModeOverride`
  parity is **already achieved**; the brief's block-parity item reduces to `framing` (one verb) and
  `shotDistribution` (one verb).
- **The regression floor is thinner than it looks.** `camera.animation_shots`'s entire response-shape
  test block is unreachable — the fixture is a `StaticMeshActor` the verb always refuses, and the
  test file says so at `TestAnimationCaptureHandlers.cpp:405-409`. Two live exposure tests skip their
  only substantive assertions with `reason=fixture-not-lit`. The genuinely load-bearing floor is the
  **13 absence assertions** listed in §3.3.

---

## §1 Target matrix

### 1a. Capability × domain — the acceptance criterion

`✅` reachable today · `▲` gap, chunk id given · `⛔` structurally impossible, reason measured.

| Capability | world / level | placed actor | static mesh asset | skeletal mesh asset | animation asset | Niagara system |
|---|---|---|---|---|---|---|
| Single framed shot | ✅ `capture_open_level`, `capture_annotated` | ✅ `frame_actor` `:73` | ✅ `capture_asset_preview` `:251` | ▲ C3+C11 | ✅ `capture_animation_preview` `:214` | ▲ C5+C11 |
| Bounds-fit framing (camera solved from subject) | ▲ C2+C8 (level bounds) | ✅ `CameraFrameHandler.cpp:131-135` | ✅ `RenderHandler.cpp:369-385` | ▲ C3 | ✅ `AnimationPreviewCaptureHandler.cpp:715-718` | ▲ C5 |
| Orbit / multi-pose set | ✅ `orbit_shots` `:219` | ✅ `orbit_shots` | ▲ C3+C11 | ▲ C3+C11 | ✅ `capture_animation_preview` `:312` | ▲ C5+C11 |
| Six axis-aligned sides | ✅ `views:"sides"` `CameraFrameHandler.cpp:361-383` | ✅ same | ▲ C3+C11 | ▲ C3+C11 | ✅ `AnimationPreviewCaptureHandler.cpp:312` | ▲ C5+C11 |
| Sphere distribution + `seed` | ✅ `CameraFrameHandler.cpp:669-695` | ✅ same | ▲ C7+C11 | ▲ C7+C11 | ▲ C7+C10 | ▲ C7+C11 |
| Time series | ✅ `animation_shots` `:88` (needs a Level Sequence) | ✅ same | ⛔ no time axis — `PoseListCapture.h:68` "A static mesh ignores the time entirely". C3 emits the typed refusal | ⛔ same, unless an animation is named — then it is the animation-asset cell | ✅ `capture_animation_preview` `:763-886` | ✅ `capture_asset_preview` `time`/`times` — see §1c |
| Annotated overlays (axes/grid/bounds/labels) | ✅ `capture_annotated` `:173` | ✅ same (`bounds:[names]` `:487-503`) | ✅ **C16** | ✅ **C16** | ✅ **C16** | ✅ **C16** |
| Ortho tile mosaic | ✅ `capture_ortho_tiles` `:163` | ⛔ §4.1 | ⛔ §4.1 | ⛔ §4.1 | ⛔ §4.1 | ⛔ §4.1 |
| `viewMode` override, scoped + restored | ✅ 5 verbs | ✅ | ✅ `capture_asset_preview` | ▲ C11 | ▲ C10 (verb has no `viewMode` arg) | ▲ C11 |
| Exposure pin, read once per set | ✅ | ✅ | ✅ | ▲ C3 | ✅ | ▲ C5 |
| Warm-up throwaway frame | ✅ `orbit_shots` only | ✅ `orbit_shots` only | ▲ C6+C11 | ▲ C6+C11 | ▲ C6+C10 | ▲ C6+C11 |
| `framing` block (subject provably in frame) | ▲ C2+C11 | ▲ C2+C8 | ✅ `RenderHandler.cpp:495-503` | ▲ C3+C11 | ▲ C4+C10 | ▲ C5+C11 |
| `hideEditorSprites` | ✅ | ✅ | ✅ (undeclared — bug 1) | ▲ C11 | ▲ C10 | ▲ C11 |

### 1b. Verb × domain — the dispatch map

| Verb | world | actor | static mesh | skeletal mesh | animation | Niagara |
|---|---|---|---|---|---|---|
| `camera.frame_actor` | ▲ C8 | ✅ `:73` | ▲ C8 | ▲ C8 | ▲ C8 | ▲ C8 |
| `camera.orbit_shots` | ✅ `point`+`radius` `:471` | ✅ `:453` | ▲ C8 | ▲ C8 | ▲ C8 | ▲ C8 |
| `camera.animation_shots` | ▲ C9 (no actor named) | ✅ `:121` | ⛔ no time axis; typed refusal C9 | ⛔ same | ▲ C9 | ▲ C9 |
| `render.capture_open_level` | ✅ `:514` | ▲ C11 (no subject framing) | ⛔ the verb *is* the level viewport, by registration `:514` | ⛔ | ⛔ | ⛔ |
| `render.capture_asset_preview` | ⛔ not an asset | ⛔ not an asset | ✅ `:251` | ▲ C11 | ▲ C11 | ▲ C11 |
| `render.capture_animation_preview` | ⛔ Persona-only toolkit gate `:92-98` | ⛔ | ▲ C10 | ✅ `:376` | ✅ `:376` | ▲ C10 |
| `render.capture_annotated` | ✅ `:173` | ✅ `:487` | ✅ **C16** | ✅ **C16** | ✅ **C16** | ✅ **C16** |
| `render.capture_ortho_tiles` | ✅ `:163` | ⛔ §4.1 | ⛔ §4.1 | ⛔ §4.1 | ⛔ §4.1 | ⛔ §4.1 |

Every `⛔` above carries a measured reason in §4 or in the cell. No cell is marked `⛔` for
"unverified"; unverified would be `▲`.

### 1c. Resolutions recorded after the wave landed

**§1a and §1b contradicted each other twice, and both contradictions were resolved in favour of
§1a.** §1a assigns a capability to the verb that owns the domain; §1b assigns the same cell to
whichever verb page a chunk happened to be editing. Where they disagreed, the implementing chunk
followed §1a and emitted a typed refusal naming the other verb, which is Decision 6. §1b is the
stale half; read §1a as the acceptance criterion.

1. **`camera.animation_shots` × animation and × niagara.** §1b marks both `▲ C9`. §1a's *Time
   series* row puts the animation cell on `capture_animation_preview` and the Niagara cell on
   `▲ C5+C10` — both on the preview verbs. **C9 followed §1a**: `camera.animation_shots` accepts
   `world` and `actor` only, and every asset kind is `UNSUPPORTED_ASSET_EDITOR` naming
   `render.capture_animation_preview` (`AnimationShotsHandler.cpp:200-215`). Its time axis is a
   Level Sequence, which drives a *placed* actor; an asset open in its own editor has none.
2. **`render.capture_animation_preview` × static mesh and × niagara.** §1b marks both `▲ C10`.
   §1a routes a static-mesh still to `capture_asset_preview` and gives the static mesh no time axis
   at all. **C10 followed §1a**: the verb accepts `skeletalMesh` and `animation` only, and refuses
   `world`, `actor`, `staticMesh` and `niagara` with one `UNSUPPORTED_ASSET_EDITOR` naming
   `render.capture_asset_preview` and the `camera.*` verbs
   (`AnimationPreviewCaptureHandler.cpp:246-258`).

**Gap closed after the wave: §1a *Time series* × Niagara (`▲ C5+C10`) landed on
`render.capture_asset_preview`.** C5 shipped the driver — the Niagara provider publishes
`timeSupported: true`, `timeStartSeconds` / `timeEndSeconds` and a working time setter that resets
and runs `AdvanceSimulation` — and for a while **no verb bound it**: the only verb that reaches the
kind registry for an asset subject took the setter into a variable named `UnusedTimeSetter` and let
it fall out of scope, so `subject.timeSupported` advertised an axis no caller could reach.

Of the two candidate homes this section originally named, the second was taken: **a `time` /
`times` argument on `capture_asset_preview`, not a `niagara` branch on
`capture_animation_preview`.** Three reasons, in order of weight.

1. `capture_asset_preview` is the **only** production call site that reaches
   `PinWrightCaptureSubject::Resolve` for an asset subject, so it is the only place any provider's
   setter can surface at all. `capture_animation_preview` never calls the resolver — it walks
   Persona directly — so a branch there would have to duplicate the acquisition it deliberately
   does not own.
2. The sibling verb's whole payload counts in **frames at an animation's sampling rate**
   (`frames`, `frameStart`, `frameStep`), and a Niagara system carries no sampling rate and no
   authored duration. Frames there would be a unit invented for one kind.
3. Resolution 2 above already had `capture_animation_preview` refusing `niagara` *and naming
   `capture_asset_preview`*. Serving it there instead would have made the verb name, that refusal,
   and this section disagree.

The binding is generic, not Niagara-special: any kind whose provider reports `bTimeSupported` is
driven, so a `skeletalMesh` + `animation` subject gets a time series here too. §1a's *Time series*
row still assigns the **animation** cell to `capture_animation_preview`, which remains the richer
verb for that domain (frame units, per-instant pose-change proof); this is an additional route, not
a replacement, and the parameter docs say so.

One design decision worth carrying forward, because nothing in an image reveals it: **the subject
is driven once per instant, not once per pose.** `AdvanceToTime` resets before advancing and, with
system determinism off, the instance seed is `FMath::Rand()` on every reset — so a set that
re-drove per shot would make the six sides of one instant six different effects sharing a
timestamp. The crossing marks the instant on the first camera of each group only
(`Handlers/Render/SubjectTimeSeries.h`), and the driver's `DesiredAge` hold keeps the frame while
the rest of the group fires.

**The four `render.capture_annotated` asset cells are C16's, not C12's.** C12 could not reach them
and said so; C16 landed the overlay projection against any viewport (`bc951ea1`), which is what
made them reachable. Both tables above are corrected.

**There is no count of "five providers" — §2.3's table is the right shape.** Four provider `.cpp`
files (Level, Mesh, Animation, Niagara) register **six** kinds; Level and Mesh each register two.
Six is `SubjectKindCount` (`CaptureSubject.h:76`) and matches §2.3's six rows. Count kinds, not
files. The `boundsSource` vocabulary is likewise exactly six values — `levelBounds`, `point`,
`actorBounds`, `sampledUnion`, `assetBounds`, `pinnedFixedBounds` — verified against every string
literal the four providers assign.

---

## §2 The subject descriptor and the resolver

### 2.1 Wire shape

One optional object, additive on every verb. The existing spellings stay and normalise into it, so
no caller changes and no test moves.

```jsonc
"subject": {
  "kind":  "world" | "actor" | "staticMesh" | "skeletalMesh" | "animation" | "niagara",
  "path":  "/Game/…",          // asset kinds
  "name":  "SM_Rock_12",       // actor kind; ActorNameParamUtils spellings apply
  "point": {"x":0,"y":0,"z":0},// world kind, with radius
  "radius": 4000,              // world/point kind
  "animation": "/Game/…",      // skeletalMesh kind: which animation to load
  "closeAfterCapture": true    // asset kinds; existing default, existing semantics
}
```

`kind` is inferred when omitted, from exactly one present key, and an ambiguous payload is
`ERR_INVALID_ARGUMENT` naming both keys. Legacy normalisation, per verb:
`actorName`→`{kind:"actor"}`, `point`+`radius`→`{kind:"world"}`, `assetPath`→ kind from the loaded
`UClass`, `sequencePath` stays a separate time-source argument on `camera.animation_shots`.

An object-shaped parameter needs a nested-schema `### <verb>` overlay or it is undiscoverable —
this repo has already paid for that once (`Tests/Infra/TestFoliageNestedInputSchemaDocs.cpp`, the
`foliage.add_instances` silent-drop rule). That overlay is C13's deliverable, not optional.

### 2.2 The frozen C1 contract

Every chunk codes against these declarations. They are fixed here so the wave does not have to
serialise on C1 finishing first.

```cpp
// Handlers/Render/CaptureSubject.h — namespace PinWrightCaptureSubject
enum class ESubjectKind : uint8 { World, Actor, StaticMesh, SkeletalMesh, Animation, Niagara };

struct FSubjectRequest                 // parsed from the wire, before anything is opened
{
    ESubjectKind Kind = ESubjectKind::World;
    FString      AssetPath;
    FString      ActorName;
    FString      AnimationPath;
    FVector      Point = FVector::ZeroVector;
    float        Radius = 0.0f;        // <= 0 means "solve from bounds"
    bool         bCloseAfterCapture = true;
};

// What a capture needs and nothing more. No UObject, no asset editor, no Slate.
struct FResolvedSubject
{
    FEditorViewportClient*     ViewportClient = nullptr;   // never null on success
    TSharedPtr<FSceneViewport> SceneViewport;              // never invalid on success
    FVector  BoundsOrigin = FVector::ZeroVector;
    double   BoundsRadius = 0.0;       // 0 => EvaluateBoundsFraming reports bEvaluated=false
    bool     bTimeSupported = false;
    double   TimeStartSeconds = 0.0;
    double   TimeEndSeconds   = 0.0;
    FString  CaptureSource;            // "staticMeshEditorPreview" etc. — verbatim existing strings
    FString  BoundsSource;             // "assetBounds" | "sampledUnion" | "pinnedFixedBounds" | "actorBounds" | "point"
    bool     bEditorWasAlreadyOpen = false;
    bool     bEditorClosed = false;
    bool     bTimeReproducible = false;// FALSE for Niagara, always. §4.2
};

// Set the subject to one instant. Returns false and fills the error pair when the kind has no
// time axis; a caller that never asks for a time never sees the refusal.
using FSubjectTimeSetter = TFunction<bool(double TimeSeconds, FString& OutErrCode, FString& OutErrMsg)>;

struct FSubjectProvider
{
    ESubjectKind Kind;
    TFunction<bool(const FSubjectRequest&, FResolvedSubject&, FSubjectTimeSetter&,
                   FString& OutErrCode, FString& OutErrMsg)> Acquire;
    TFunction<void(FResolvedSubject&)> Release;   // runs on EVERY exit path, error paths included
};

bool ParseSubject(const TSharedPtr<FJsonObject>& Payload, FSubjectRequest& Out,
                  FString& OutErrCode, FString& OutErrMsg);
bool Resolve(const FSubjectRequest&, FResolvedSubject&, FSubjectTimeSetter&,
             FString& OutErrCode, FString& OutErrMsg);
TSharedPtr<FJsonObject> MakeSubjectInfoObject(const FResolvedSubject&);  // the `subject` response block
void RegisterProvider(const FSubjectProvider&);   // called from static init, one file per kind
```

`Resolve` returns an RAII-scoped handle in the implementation (the `Release` closure runs from a
`ON_SCOPE_EXIT`), matching the restore discipline `rpc-design.md:266` requires and
`CaptureEditorViewportToPng` already keeps at `PreviewViewportCaptureUtils.cpp:1431-1461`.

### 2.3 What each provider does

| Kind | Viewport client | Bounds | Time setter |
|---|---|---|---|
| `World` | `PinWrightCameraFrame::GetActiveLevelViewport` (`CameraShotPlanUtils.h:139-185`) | level bounds, or `point`+`radius` verbatim (`CameraFrameHandler.cpp:471-473`) | Level Sequence scrub, `SequencePlayheadUtils::ApplyPlayheadPosition` (`SequencePlayheadUtils.h:94-122`) — only when `sequencePath` is given |
| `Actor` | same | `Actor->GetActorBounds(false, Origin, Extent)`; union across sampled instants when a time plan exists (`AnimationShotsHandler.cpp:515-537`) | same |
| `StaticMesh` | asset-editor walk (§2.5), toolkit `StaticMeshEditor` | `StaticMesh->GetBounds()` | **none** — `bTimeSupported=false` |
| `SkeletalMesh` | asset-editor walk, Persona family (`AnimationPreviewCaptureHandler.cpp:92-98`) | `SkeletalMesh->GetBounds()` | `UAnimSingleNodeInstance::SetPosition` + `TickAnimation(0)` + `RefreshBoneTransforms(nullptr)` (`AnimationPreviewCaptureHandler.cpp:770-777`), only when an animation is named |
| `Animation` | same, via `AnimationAsset->GetPreviewMesh(true)` (`:376-390`) | preview **mesh asset** bounds, never the posed component | same |
| `Niagara` | asset-editor walk, toolkit `Niagara` | `SetSystemFixedBounds` pinned for the call (§2.4) | `AdvanceSimulation` / `SeekToDesiredAge` (§2.4) |

### 2.4 Niagara — engine facts, verified against `C:\UE_5.8\Engine`

`UNiagaraComponent` is `UCLASS(..., MinimalAPI)` (`NiagaraComponent.h:56-57`) — **there is no
class-level export**, so every member called must carry its own `NIAGARA_API`. These do:

```cpp
NIAGARA_API void SetAgeUpdateMode(ENiagaraAgeUpdateMode InAgeUpdateMode);   // :345
NIAGARA_API void SetDesiredAge(float InDesiredAge);                        // :353
NIAGARA_API void SeekToDesiredAge(float InDesiredAge);                     // :358
NIAGARA_API void SetSeekDelta(float InSeekDelta);                          // :372
NIAGARA_API void SetSystemFixedBounds(FBox LocalBounds);                   // :423
NIAGARA_API void AdvanceSimulation(int32 TickCount, float TickDeltaSeconds);        // :661
NIAGARA_API void AdvanceSimulationByTime(float SimulateTime, float TickDeltaSeconds); // :665
```

`enum class ENiagaraAgeUpdateMode : uint8 { TickDeltaTime, DesiredAge, DesiredAgeNoSeek }`
(`NiagaraCommon.h:185`). Do not write a numeric value for it anywhere.

**Use `AdvanceSimulation(N, FixedDt)`, not `SeekToDesiredAge`.** `AdvanceSimulation` forces solo mode
and runs `ManualTick` N times on the game thread, completing before it returns
(`NiagaraSystemInstance.cpp:987+`). `SeekToDesiredAge` is throttled by `MaxSimTime`, which defaults
to **33 ms** (`NiagaraComponent.cpp:651`), so a long seek is spread across frames the capture never
gives it — the frame would be photographed mid-seek. `AdvanceSimulationByTime` is
`AdvanceSimulation(FloorToInt(SimulateTime/TickDeltaSeconds), TickDeltaSeconds)` and is equivalent;
prefer the tick-count form so the delta is explicit in the response.

**Bounds grow, and they also go stale.** `UNiagaraComponent::CalcBounds` prefers `CurrLocalBounds`
over `SystemFixedBounds` (`NiagaraComponent.cpp:2433-2481`), and `CurrLocalBounds` is refreshed only
when the new box escapes the old one **or every 5 s**
(`MaxTimeBeforeForceUpdateTransform = 5.0f`, `NiagaraComponent.cpp:653, 1771-1780`). So bounds read
mid-simulation are correct on the grow side and up to five seconds stale on the shrink side. The
provider therefore pins bounds with `SetSystemFixedBounds` for the duration and restores with
`ClearSystemFixedBounds`, reporting `boundsSource: "pinnedFixedBounds"`.

**GPU emitters never get dynamic bounds at all** — `ENiagaraSimTarget::GPUComputeSim` falls back to
`FVersionedNiagaraEmitterData::GetDefaultFixedBounds()`, a hardcoded `FBox(FVector(-100), FVector(100))`
(`NiagaraEmitter.h:449`, `NiagaraEmitterInstanceImpl.cpp:960-995`). A GPU system with no authored
fixed bounds reports a 200 cm cube regardless of its real extent, so auto-framing will be wrong and
the response must say so rather than silently mis-frame: publish
`subject.boundsSource` and a `boundsWarning` when any emitter is GPU with no fixed bounds.

### 2.5 Asset-editor viewport acquisition — one walk, four copies today

The generic step already exists, four times over, and is asset-type-agnostic by construction: it
walks the toolkit's Slate tree for an `SEditorViewport` and reads the two public accessors.

- `RenderHandler.cpp:50-99` — anonymous namespace, `StaticMeshEditor`
- `AnimationPreviewCaptureHandler.cpp:100-151` — namespace `PinWrightAnimationPreview`, Persona family;
  its own comment at `:83-84` records that it is a copy made to dodge a Unity-build ODR collision
- `Handlers/Editor/EditorWindowHandlers.cpp:187` — "Adapted from RenderHandler::FindEditorViewportRecursive"
- `Handlers/UI/WidgetDesignerCaptureUtil.cpp:70` — a fourth, for UMG

The predicate is a **string type-name match**, `RenderHandler.cpp:52-55`:

```cpp
const FString WidgetType = Widget->GetTypeAsString();
if (WidgetType == TEXT("SEditorViewport") || WidgetType.EndsWith(TEXT("EditorViewport")))
{
    TSharedRef<SEditorViewport> EditorViewport = StaticCastSharedRef<SEditorViewport>(Widget);
```

**This is why Niagara looks unreachable and is not.** `SNiagaraSystemViewport` derives from
`SEditorViewport` (`SNiagaraSystemViewport.h:28`), which publishes
`TSharedPtr<FEditorViewportClient> GetViewportClient() const` (`SEditorViewport.h:91`) and
`TSharedPtr<FSceneViewport> GetSceneViewport()` (`:96`) as public inline accessors in **UnrealEd's
public header**. The toolkit's own client type and the `SNiagaraSystemViewport` symbol are never
needed. What blocks it is only the suffix: the widget is constructed as
`SNew(SNiagaraSystemViewport, …)` (`NiagaraSystemToolkitModeBase.cpp:497`), `SWidget::SetDebugInfo`
stores that literal unconditionally (`SWidget.cpp:1398-1400`), and `"SNiagaraSystemViewport"` does
not end in `"EditorViewport"`.

C1 therefore replaces the suffix heuristic with an **exact allow-list of verified widget type names**,
each entry justified by a checked `: public SEditorViewport` in the engine. A suffix match is not
merely incomplete, it is unsafe: `StaticCastSharedRef<SEditorViewport>` on any widget whose name
happens to end in `EditorViewport` is undefined behaviour, and `SEditorViewport` carries no
`SLATE_DECLARE_WIDGET` so there is no runtime hierarchy check to fall back on. Day-one entries,
each verified: `SStaticMeshEditorViewport`, the Persona viewport widgets currently matched by the
suffix, `SNiagaraSystemViewport`. Unknown names fall through to the existing suffix rule **with the
cast removed** — report `ERR_PREVIEW_VIEWPORT_NOT_FOUND` naming the widget type, so the next kind is
a one-line allow-list addition with an engine citation rather than a crash.

Toolkit gate, verified: `FNiagaraSystemToolkit::GetToolkitFName()` returns `FName("Niagara")`
(`NiagaraSystemToolkit.cpp:351-354`) and `FAssetEditorToolkit::GetEditorName()` returns
`GetToolkitFName()` (`AssetEditorToolkit.cpp:502-505`). `FNiagaraSystemToolkit` reaches
`FAssetEditorToolkit` through `FWorkflowCentricApplication`
(`WorkflowOrientedApp/WorkflowCentricApplication.h:19`), so the existing
`static_cast<FAssetEditorToolkit*>(EditorInstance)` → `GetToolkitHost()->GetParentWidget()` route is
valid for it.

The Niagara preview **component** is found the same way the Persona one is — iterate the viewport
client's own preview world (`FindPreviewMeshComponent`, `AnimationPreviewCaptureHandler.cpp:160-193`),
substituting `UNiagaraComponent` for `UDebugSkelMeshComponent`. `FNiagaraSystemViewModel::GetPreviewComponent()`
is public and exported (`NiagaraSystemViewModel.h:217`) but is **not** used here: it can hand back a
component belonging to a cached data-processing view model rather than the world the viewport draws.

---

## §3 Block parity

### 3.1 Already universal — do not re-plumb

`viewport` and every sub-block (`aim`, `warmup`, `exposure`, `editorSprites`, `viewModeOverride`,
`viewModeWarning`) come from one serializer, `MakeViewportInfoObject`
(`PreviewViewportCaptureUtils.cpp:2345-2444`), and all eight viewport-owning verbs already call it.

### 3.2 The real gaps

| Block | Emitted by | Missing from | Chunk |
|---|---|---|---|
| `framing` | `capture_asset_preview` only (`RenderHandler.cpp:495-503`) | all seven others | C8–C12, each from `FResolvedSubject::Bounds*` |
| `shotDistribution` | `orbit_shots` only (`CameraFrameHandler.cpp:669-695`) | `animation_shots`, `capture_animation_preview` | C7 lifts the planner, C9/C10 adopt |
| `poseSet` (warm-up + truncation) | `orbit_shots` only (`PoseListCapture.cpp:131-157`) | all seven others | C6 + adopters |
| `imageStats` full set (`litPixelCount`, `litPixelFraction`, `litLuminanceThreshold`, `toneLevelsUsed`, `toneLevelMinPixels`) | `AddCaptureFields` (`RenderHandler.cpp:101-193`) | every per-shot object via `AddShotFields` (`CameraShotPlanUtils.h:190-244`) — the drift its own header comment predicts at `PreviewViewportCaptureUtils.h:1108-1110` | C7 |
| `imageStats` / `blank` at all | — | `capture_annotated` computes them and discards them | C12 |
| `subject` (new) | — | all | C1 defines, all adopt |

### 3.3 What must not move

Thirteen **absence** assertions are the strictest part of the contract and the easiest thing a
refactor breaks — a zeroed field emitted where the code used to omit it fails them. Assert-absent
today: `viewModeWarning` on a lit capture (`TestCaptureViewModeReporting.cpp:110-111`);
`restoreWarning` (`TestCaptureViewModeOverride.cpp:367, 390`), `applyWarning` (`:388`),
`showFlagMismatches` (`:445`); `pinWarning` (`TestCaptureExposurePin.cpp:557, 579, 1012`),
`adapted` (`:586, 764`), `ev100Equivalent` (`:766`), `adaptedSource` (`:768`),
`adaptedReadbackPending` (`:772`), `warmupWarning` (`:799, 858`), `restoreWarning` (`:620`);
`framingWarning` when `boundsInFrame` is true (`TestCaptureCameraAim.cpp:438-439`);
`actorLabels` and `overlays.actorLabels` when the argument is omitted
(`TestAnnotatedCaptureHandlers.cpp`).

**Every new block follows the same rule: present only when it has something to say.** A `subject`
block on a verb the caller gave no subject to is omitted, not emitted empty.

---

## §4 What cannot converge — technical, not scheduling

### 4.1 `render.capture_ortho_tiles`

It renders through `USceneCaptureComponent2D` + a render target
(`OrthoTileCaptureUtils.cpp:165, 407, 419`), not an `FEditorViewportClient`. It has no `viewMode`
argument and reports none: zero hits for `viewMode`, `EViewModeIndex` or `ViewModeVocabulary` across
all five ortho/probe files. Its manifest publishes `renderer: "sceneCapture2D"` precisely so a
comparison can refuse a cross-renderer pair — measured at **5.76 % mean absolute error against a
1.30 % viewport self-noise floor** (`docs/wiki-src/render.md:162-176`).

The honest split, because the broad version of this claim is itself wrong:

| View-mode capability | On the scene-capture path | Why |
|---|---|---|
| Show-flag-only modes (unlit, wireframe, lighting off, foliage off) | **reachable** | `FEngineShowFlags ShowFlags` is a public writable member (`SceneCaptureComponent.h:189`); PinWright already writes it (`OrthoTileCaptureUtils.cpp:239`) |
| `ChooseDebugViewShaderMode` family — shader complexity, quad overdraw, LOD/HLOD coloration, texture density | **reachable in principle**, unimplemented | derived from show flags alone, `SceneView.cpp:3239-3300`, never reads `Family->ViewMode` |
| Buffer visualization, Substrate visualization | **unreachable** | needs `FSceneViewFamily::ViewMode` **and** `ViewModeParam(FName)`; the capture renderer writes neither (`grep -c ViewMode SceneCaptureRendering.cpp` = 0; `ViewMode` initialised to `VMI_Lit` at `SceneView.cpp:3054`) |
| Lightmap density, stationary-light overlap, lit wireframe, collision | **unreachable** | applied via `ApplyViewMode` + `EngineShowFlagOverride(ESFIM_Editor, …)`; neither is called from any capture path, and the component's flags start at `ESFIM_Game` (`SceneCaptureComponent.cpp:168-169`) |
| Two-slot persp/ortho mode with scoped restore | **not applicable** | no persistent view state on a component |

So the exclusion is: **the `viewMode` vocabulary's application and measurement scope cannot cross**
(`FScopedViewModeOverride`, `ResolveForClient`, the two-slot restore, `EngineShowFlagOverride`,
`ViewModeParam`), while the *name → show-flag resolution* layer already is viewport-free by its own
design statement (`ViewModeVocabulary.h:6-8`, `DistinguishingShowFlags` at `:148` takes a bare
`FEngineShowFlags`). Sharing that layer with the ortho verb is a legitimate future item; it is **not**
in this plan, because it changes the ortho verb's pixels and this plan changes none.

What the ortho path already shares, and should keep sharing: the blank classifier and opaque-alpha
stamp (`PinWrightScreenshotUtils::IsBlankReadback` / `ForceOpaqueAlpha`,
`OrthoTileCaptureHandler.cpp:574, 578`), the luminance classifier itself
(`CalculateCaptureImageStats`, called at `OrthoTileCaptureUtils.cpp:445` with the comment "One
classifier for both renderers, so neither can drift into its own idea of 'empty'"), and the
`exposure` wire vocabulary's parser. The *exposure mechanism* is not shared and cannot be: the
viewport writes `FEditorViewportClient::ExposureSettings`, the capture writes `FPostProcessSettings`
with `AEM_Manual`, and `OrthoTileCaptureUtils.h:187-192` records that the two are not numerically
comparable at the same EV100 — about 2.47 stops apart, because one branch multiplies by
`kMiddleGrey` 0.18 and the other by 1.0.

### 4.2 Niagara reproducibility — reported, never promised

Determinism exists at three scopes and **all three default off**:

- `UNiagaraSystem::bDeterminism = false` + `RandomSeed = 0` (`NiagaraSystem.h:1073-1085`). With it
  off, the instance seed is literally `FMath::Rand()` on every reset
  (`NiagaraSystemInstance.cpp:894`).
- `FVersionedNiagaraEmitterData::bDeterminism = false` + `RandomSeed` (`NiagaraEmitter.h:310-316`).
  Its own comment scopes the guarantee: results repeat "**as long as delta time is not variable**",
  and "any changes to the emitter's individual scripts will adjust the results".
- `UNiagaraComponent::RandomSeedOffset` (`NiagaraComponent.h:96-103`), whose comment warns that
  setting it non-deterministically "has the potential to break determinism of the entire system",
  and which latches at activate/reset only.

None of them covers GPU emitters (no GPU determinism knob exists anywhere in the Niagara source) or
data interfaces that read world state. Therefore:

**The plan makes no reproducibility claim for particle captures and adds no `reproducible: true`
field.** `FResolvedSubject::bTimeReproducible` is hardcoded `false` for the Niagara kind. The
response reports what was done — the tick delta used, the tick count, and the measured
`system.bDeterminism` / emitter determinism flags — and a `reproducibilityWarning` naming which of
the three scopes is off. This is the same discipline the `auto` collision decomposer investigation
arrived at after a full pass on a non-deterministic subsystem that returned 21 hulls on one compile
and 14 on the next: report the non-determinism, do not average it away.

### 4.3 Not in scope, and why

`render.capture_open_level` and `render.capture_ortho_tiles` are **domain-named verbs** — their
registration strings define the subject (`RenderHandler.cpp:514`, `OrthoTileCaptureHandler.cpp:163`).
Giving them a `subject` would make the name lie. Their capabilities reach other domains through
`camera.orbit_shots` and `render.capture_asset_preview`, which is what §1a measures.

---

## §5 Work — one wave of thirteen file-disjoint chunks

No chunk edits a file another chunk edits. Every chunk that needs a new test writes a **new** test
file rather than appending to a shared one, for the same reason.

**The tree does not compile until the whole wave lands.** C8–C12 call the §2.2 declarations before
C1's file exists on their agent's disk; that is why the contract is frozen in this document. Build
and suite verification is a wave-end step, not a per-chunk step — the convention this repo already
follows.

### C1 — resolver core and provider registry
**Create** `Source/PinWright/Private/Handlers/Render/CaptureSubject.h`, `CaptureSubject.cpp`,
`Source/PinWright/Private/Tests/Render/TestCaptureSubjectResolver.cpp`. **Modify** nothing.
Steps: implement §2.2 verbatim; the static-init `RegisterProvider` table; `ParseSubject` with
kind inference and the ambiguous-payload refusal; `MakeSubjectInfoObject`; the shared asset-editor
walk of §2.5 with the **exact-name allow-list** replacing the unchecked suffix cast; RAII release.
Acceptance:
- `PinWright.render.capture_subject.AmbiguousPayloadIsRefused` — `{assetPath, actorName}` together
  returns `INVALID_ARGUMENT` whose message contains **both** key names. *Unable to fail if* it
  asserted only that parsing failed; a typo also fails. Assert both substrings.
- `…UnknownWidgetTypeIsRefusedNotCast` — a widget named `"SFakeEditorViewport"` that does not derive
  from `SEditorViewport` yields `PREVIEW_VIEWPORT_NOT_FOUND` naming the type, and no cast occurs.
  *Unable to fail if* the fixture derived from `SEditorViewport`; it must not.
- `…AllowListEntriesAreRealEditorViewports` — every allow-list entry compiles a
  `static_assert(TIsDerivedFrom<T, SEditorViewport>::Value)`. Build-time, so it cannot rot silently.
- `…ReleaseRunsOnTheErrorPath` — a provider whose `Acquire` fails after opening an editor still has
  `Release` invoked exactly once. Assert a counter, both directions.

### C2 — world and actor providers
**Create** `Handlers/Render/CaptureSubjectProviders_Level.cpp`, `Tests/Render/TestCaptureSubjectLevel.cpp`.
Steps: `World` and `Actor` providers over `GetActiveLevelViewport`; level-bounds fit for `World`
when no `point` is given; `point`+`radius` path preserved verbatim; sampled-union bounds when the
caller supplies a time plan; the Level Sequence time setter.
Acceptance: `…WorldBoundsFitFramesTheLevel` asserts `BoundsRadius > 0` and `BoundsSource == "levelBounds"`;
`…PointWithoutRadiusIsRefused` reproduces the existing message at `CameraFrameHandler.cpp:466-468`
character-for-character; `…SampledUnionExceedsAnySingleInstant` asserts the union radius is strictly
greater than the max single-instant radius on a fixture that moves — *unable to fail if* the fixture
were static, so the test asserts the movement precondition first.

### C3 — static-mesh and skeletal-mesh providers
**Create** `Handlers/Render/CaptureSubjectProviders_Mesh.cpp`, `Tests/Render/TestCaptureSubjectMesh.cpp`.
Steps: both kinds over the C1 walk; bounds from `GetBounds()`; `bTimeSupported=false` for
`StaticMesh`; the decision-6 typed refusal naming the missing time axis; `closeAfterCapture`
semantics carried over unchanged from `RenderHandler.cpp:395-441` — including that an **explicit**
`true` closes a window the caller already had open.
Acceptance: `…StaticMeshHasNoTimeAxis` asserts `UNSUPPORTED_ASSET_EDITOR` and that the message
contains `"time"`, not `UNKNOWN_PARAMS`; `…SkeletalMeshBoundsComeFromTheAsset` asserts
`BoundsSource == "assetBounds"` **and** that the value equals `SkeletalMesh->GetBounds().SphereRadius`
read at assert time from the fixture, not a literal.

### C4 — animation-asset provider
**Create** `Handlers/Render/CaptureSubjectProviders_Animation.cpp`, `Tests/Render/TestCaptureSubjectAnimation.cpp`.
Steps: resolve the preview mesh via `GetPreviewMesh(true)`; **bounds from the mesh asset**; the
scrub time setter (`SetPosition` → `TickAnimation(0)` → `RefreshBoneTransforms(nullptr)`); full
save/restore of `FPreviewInstanceState` (`AnimationPreviewCaptureHandler.cpp:196-210`).
Acceptance: `…PosedBoundsAreNotUsed` — scrub to two instants whose posed component bounds differ by
more than 10 %, assert `BoundsRadius` is **bit-identical** across both. *Unable to fail if* the
fixture's pose barely moved; assert the posed-bounds difference as a precondition first. This is the
single most valuable test in the wave: it pins decision 4 against the exact regression that
motivated it.

### C5 — Niagara provider
**Create** `Handlers/Render/CaptureSubjectProviders_Niagara.cpp`, `Tests/Render/TestCaptureSubjectNiagara.cpp`.
**No `.Build.cs` change** — `Niagara`/`NiagaraCore`/`NiagaraEditor` are already public dependencies
(`PinWright.Build.cs:25`).
Steps: toolkit gate on `GetEditorName() == FName("Niagara")`; component found by iterating the
viewport client's preview world; bounds pinned with `SetSystemFixedBounds` and cleared on release;
time setter via `AdvanceSimulation(N, FixedDt)` from a reset, with the tick delta echoed;
`bTimeReproducible = false` unconditionally; the GPU-no-fixed-bounds `boundsWarning`.
Acceptance:
- `…AdvancesByTickCountNotWallClock` asserts the component's age after the setter equals
  `N * FixedDt` within one delta, read off `GetDesiredAge()` — not off the value passed in.
- `…GpuEmitterWithoutFixedBoundsWarns` asserts the warning fires and names the emitter.
  *Unable to fail if* the fixture were CPU-only; assert `SimTarget == GPUComputeSim` first.
- `…NoReproducibilityIsClaimed` — grep-style assertion that the response carries no `reproducible`
  field and does carry `reproducibilityWarning` when `bDeterminism` is false. Both directions.

### C6 — pose-list primitive: time driver, framing, per-set reporting
**Create** `Tests/Render/TestPoseListCaptureSubjectTime.cpp`.
**Modify** `Handlers/Render/PoseListCapture.h`, `PoseListCapture.cpp`.
Steps: implement `FCameraPose::SubjectTimeSeconds` by taking an `FSubjectTimeSetter` on
`FPoseListCaptureRequest` and calling it before each pose; add `BoundsOrigin`/`BoundsRadius` to the
request and emit `framing` per capture from `EvaluateBoundsFraming` against the **measured** pose;
keep `bWarmupShot` default `true` and keep the warm-up excluded from `Captures`.
Acceptance: `…TimeSetterRunsOncePerPose` counts invocations and asserts it equals `Poses.Num()`
(**not** `+1` — the warm-up reuses `Poses[0]` and must not re-drive time);
`…FramingIsMeasuredAgainstTheEffectivePose` asserts the framing verdict changes when the viewport
refuses the requested aim while the request is unchanged.

### C7 — shot planning: one sides table, one distribution, full per-shot stats
**Create** `Tests/Render/TestShotPlanSides.cpp`. **Modify** `Handlers/Render/CameraShotPlanUtils.h`.
Steps: lift the six-sides table — duplicated verbatim at `CameraFrameHandler.cpp:361-383`,
`AnimationShotsHandler.cpp:197-220`, `AnimationPreviewCaptureHandler.cpp:312` — into one
`MakeSideViews(const FString& ProjectionMode)`; expose `MakeShotDistributionObject` so
`shotDistribution` is emittable by any verb; add the missing `litPixelCount`, `litPixelFraction`,
`litLuminanceThreshold` and `AddToneRangeStatsFields` call to `AddShotFields`; unify the
`resolutionSource` vocabulary, which is currently three different word sets across three verbs.
Additive only — adopters compile unchanged.
Acceptance: `…SidesTableIsOneTable` asserts the six azimuth/elevation pairs equal the values at
`CameraFrameHandler.cpp:361-383` **read as literals in the test**, so a silent reordering fails;
`…PerShotStatsCarryTheLitPair` asserts all five new keys present with the same values
`AddCaptureFields` would produce for the same `FCaptureImageStats`.

### C8 — `camera.frame_actor` + `camera.orbit_shots`
**Create** `Tests/Render/TestCameraFrameSubjects.cpp`. **Modify** `Handlers/Render/CameraFrameHandler.cpp`.
Steps: both verbs take `subject`; `actorName`/`point` normalise into it; `frame_actor` routes through
`CaptureCameraPoses` with a one-pose list so it inherits the warm-up; both emit `framing` and
`subject`. **`PoseRequest.MaxPoses = GMaxOrbitShots` must survive** (`:541-544`) — dropping that line
silently shortens every set from 24 to the primitive's default 8.
Acceptance: existing `TestCameraFrameHandlers.cpp` passes unmodified — `path`, `width`, `height`,
`actorName`, `count`, `shots[]`, `resolutionSource`, per-shot `orthoAxisSnapped`/`orthoView`/`orthoWidth`;
`…OrbitSetStillAllowsTwentyFour` asserts a 24-shot request returns 24 with
`poseSet.posesTruncated == 0`; `…AssetSubjectProducesSixSides` asserts a static-mesh subject with
`views:"sides"` returns 6 shots whose `angle` pairs are the C7 table.

### C9 — `camera.animation_shots`
**Create** `Tests/Render/TestAnimationShotsSubjects.cpp`. **Modify** `Handlers/Render/AnimationShotsHandler.cpp`.
Steps: replace the nested loop at `:589-662` with one `CaptureCameraPoses` call carrying the
`FSubjectTimeSetter`; take `subject`; add the `viewMode` argument it uniquely lacks; emit
`shotDistribution`, `framing`, `poseSet`, `subject`; keep the sampled-union bounds and the
resolve-once resolution rule (`:23-29`) intact.
Acceptance: `…BurstHoldsOneCameraAcrossInstants` asserts every shot's `cameraLocation` is identical
across frame indices at a fixed view index — *unable to fail if* the fixture does not move, so the
test asserts a non-zero `poseDeltaFromFirst` first; `…PartialSetSurvivesAFailingShot` asserts that a
shot failing mid-burst returns the shots captured so far, which the hand-rolled loop could not do
(`:628-634` errors out and discards them).

### C10 — `render.capture_animation_preview`
**Create** `Tests/Render/TestAnimationPreviewSubjects.cpp`. **Modify** `Handlers/Render/AnimationPreviewCaptureHandler.cpp`.
Steps: replace the nested loop at `:763-886` and the redundant outer orbit save/restore at `:730-744`
with one `CaptureCameraPoses` call; take `subject`; add `viewMode`, `hideEditorSprites`,
`distribution`/`seed`; emit `framing`, `shotDistribution`, `poseSet`, `subject`; publish the
`viewport` block **per shot** as well as at top level — today it comes only from `LastCapture` (`:929-931`),
so per-shot aim and warm-up verdicts are unavailable.
Acceptance: `…ColdFirstFrameIsDiscarded` asserts `poseSet.warmupShotTaken == true` and that no file
named `*_warmup.png` survives; `…AnglesAndViewsAreNotInvented` asserts the response has **no**
`angles` and **no** `views` key (they never existed — see §0) while `count`, `frames` and `viewCount`
keep their current meanings.

### C11 — `render.capture_asset_preview` + `render.capture_open_level`
**Create** `Tests/Render/TestAssetPreviewSubjects.cpp`. **Modify** `Handlers/Render/RenderHandler.cpp`.
**Gated on the in-flight crash fix landing — see §7.**
Steps: `capture_asset_preview` takes `subject` and accepts every asset kind the registry serves,
replacing the `Cast<UStaticMesh>` gate at `:346-359` and the `StaticMeshEditor` name gate at `:449-454`
with a resolver call; route it through `CaptureCameraPoses` so it gains orbit, sides, distribution
and the warm-up frame; declare the three undeclared-but-live parameters (bug 1); update the
registration string, which currently says "Static Mesh" (`:251`).
Acceptance: existing `TestCaptureExposurePin.cpp` and `TestCaptureBlankCriterion.cpp` pass unmodified;
`…SkeletalMeshAssetGetsSixSides` returns 6 shots with `captureSource` naming the Persona preview;
`…NiagaraAssetIsCaptured` returns a non-blank frame with `subject.kind == "niagara"` — and, per
`rpc-design.md:341`, the chunk's report must contain a **description of what the frame looked like**,
not only its statistics; `…UndeclaredParamsAreDeclared` walks the verb's `FParamSpec` list and
asserts `allowBlank`, `viewDistanceScale` and `hideEditorSprites` are present.

### C12 — `render.capture_annotated`
**Create** `Tests/Render/TestAnnotatedSubjects.cpp`. **Modify** `Handlers/Render/AnnotatedCaptureHandler.cpp`.
Steps: take `subject` so overlays can be drawn over an asset preview; add the discarded `imageStats`,
`blank`, `crushed`/`blownOut`/`rangeWarning`, `redrawRetries` and `framing`; replace the eight raw
`TEXT("…")` error literals with the `ErrorCodes::ERR_*` constants (bug 3).
Acceptance: existing `TestAnnotatedCaptureHandlers.cpp` passes unmodified, **including its
assert-absent cases**; `…StatsAreNoLongerDiscarded` asserts `imageStats.meanLuminance` equals the
value the base capture computed; `…EveryErrorCodeIsRegistered` — the existing
`PinWright.core.error_codes.AllEmittedCodesAreRegistered` walk now covers this file.

### C13 — documentation
**Modify** `docs/wiki-src/camera.md`, `docs/wiki-src/render.md`, `docs/index.md`, `docs/tags.md`.
Sole owner of all four. Any chunk wanting a doc line files it here.
Steps: `### <verb>` H3 overlays documenting the nested `subject` schema for every verb that takes one —
required, or the object shape is undiscoverable, per `TestFoliageNestedInputSchemaDocs.cpp`; fix
`camera.md:11` and `:29`, which both say "Both verbs" in a namespace with three; add the missing
response-field documentation for `camera.animation_shots` and `render.capture_animation_preview`,
whose pages document **zero** response fields today; correct the `render.md:19-27` `viewport` table,
which omits `exposure`, `warmup`, `viewModeOverride`, `aim` and `editorSprites`; state plainly that
particle captures carry no reproducibility promise (§4.2).
Acceptance: `PinWright.core.docs_schema.EveryMaintainerDocIsIndexed` passes;
`PinWright.infra.wiki_src.SourcePagesFollowRenderingRules` passes — every `##` sits above the first
`###`; a new `TestCaptureSubjectSchemaDocs.cpp` (owned by C13) asserts on overlay-**exclusive**
phrasing only, never on bare parameter names the generator already renders.

### Ordering

Only two orderings are real. Everything else runs together.

```
in-flight crash fix + green full suite ──> C11        (same file, RenderHandler.cpp)
C1 contract frozen in §2.2             ──> C8..C12    (compile-time only; the wave lands together)
```

C1–C7 have no blocker among themselves and can all start immediately. C13 depends on nothing and can
be written from this document.

---

## §6 Shared-file ownership

| File | Owner | Rule |
|---|---|---|
| `Handlers/ErrorCodes.h` | **nobody** | Decision 5: no new codes. If a chunk believes it needs one, it stops and the plan is amended — it does not add one |
| `docs/error-code-catalog.md` | nobody | Follows from the above |
| `PinWright.Build.cs` | nobody | Verified unnecessary (`:25`) |
| `Dispatch/SafePoint.cpp` | nobody | All eight verbs are already listed (`:75-91`); no verb is added or renamed |
| `Handlers/Render/CameraShotPlanUtils.h` | **C7** | Header-only, additive. Four `.cpp` files include it and none needs editing for C7's additions |
| `docs/index.md`, `docs/tags.md`, `docs/wiki-src/camera.md`, `docs/wiki-src/render.md` | **C13** | Append-only for anyone else; re-read before editing |
| `Tests/Render/TestAnimationCaptureHandlers.cpp`, `TestCameraFrameHandlers.cpp`, `TestAnnotatedCaptureHandlers.cpp`, `TestCaptureExposurePin.cpp`, `TestCaptureViewModeOverride.cpp`, `TestCaptureBlankCriterion.cpp`, `TestCaptureCameraAim.cpp` | **nobody** | These are the regression floor. They must pass **unmodified**. A chunk that needs a new assertion writes a new file |
| `PreviewViewportCaptureUtils.h/.cpp`, `ViewModeVocabulary.h/.cpp` | **nobody in this wave** | Held by the in-flight crash fix (§7). No chunk here needs them: every function it calls is already exported |

Other agents edit this checkout concurrently. Re-read any file before editing and never revert a
change you did not author.

---

## §7 Preconditions

**This wave starts only after a green full suite.** Not a filtered run.

The state at the time of writing: six files carry an uncommitted `UViewModeUtils::GetViewModeDisplayName`
crash fix — an out-of-bounds `Array index out of bounds: 256 into an array of size 256` reached by
`Resolve("VMI_Max")` landing on UHT's synthetic terminator, plus two `ensure`s on
`VMI_VisualizeSubstrate` (34) and `VMI_VisualizeGroom` (35), which the vocabulary offers as
renderable but which have no display-name branch. `Saved/Logs/pw_goal_suite.log` reports
3423 started / 3421 passed / 1 failed — counts that cannot reconcile, because the crashing test never
completed and the process died mid-run.

Two things must be true before dispatch, and a filtered run establishes neither:

1. **A full-suite run with reconciling counts.** The last clean baseline is 4051/4051/0 at `39a3eb2b`,
   22 commits back. `pw_fix_check.log` shows 131/131/0 but is a `PinWright.render` filter.
   *Absence of failure is not presence of testing* — `rpc-design.md:341`, and the killed-commandlet
   row in its own table.
2. **The one genuine failure demonstrated fixed, not skipped.**
   `PinWright.render.capture_asset_preview.PinnedCapturesReproduceWithinTolerance` failed with
   `meanAbsDiff 89.134` against a tolerance of 4.0; it now reports `Result={Success}` **only because**
   it printed `PINWRIGHT_ASSERTIONS_SKIPPED … reason=fixture-not-lit`, so both substantive assertions
   were skipped. That is board ticket `B-test-skips-assertions-silently` firing, and it is the same
   failure mode this plan's acceptance criteria are written against: a green that examined nothing.

---

## §8 Risks

- **The regression floor is thinner than the test count suggests.** `camera.animation_shots`'s
  response-shape block is unreachable (`TestAnimationCaptureHandlers.cpp:400-409`) and two exposure
  tests self-skip. C9 changes a verb with, in practice, no shape coverage at all. C9's own new tests
  are the only thing standing between that refactor and a silent contract change — write them first.
- **~~Asset-preview captures cannot be pixel-asserted in the suite.~~ RETRACTED 2026-08-21.** This
  risk said `FAdvancedPreviewScene` is not lit under `UnrealEditor-Cmd -unattended`, bimodally
  across runs: the same fixture rendering `meanLuminance` 0.0978 on one run and 0.0078 on another,
  where six stops of `ev100` produce byte-identical frames. It was believed and acted on for weeks
  and shaped C3/C4/C5/C10/C11. It is **wrong**. Commit `bcc334e8` ("Fix the view-mode crash and the
  capture test that never compared two files") showed the auto-generated screenshot filename
  carries a **one-second** timestamp while a capture takes ~60 ms, so the three captures behind
  those numbers overwrote each other and every "comparison" was one file against itself.
  Re-measured the same day: `meanLuminance` is **0.3644 headless and 0.3644 interactive**. There is
  no dim mode and no black mode. Kept rather than deleted because the chunks above were designed
  around it. See the bullet marked `RETRACTED 2026-08-21` in `docs/lessons.md` for the withdrawn
  text verbatim. **What survives:** an assertion comparing two captures must pass an explicit
  `filename` **or** assert the two returned `path` values differ, otherwise it cannot fail; and an
  unlit frame is still reported as **not measured**, never as a pass — that rule was always right,
  just for a different reason.
- **Every new preview-opening path inherits the shutdown crash.** An asset editor left open when the
  editor exits faults in `~FStaticMeshEditor` at `StaticMeshEditor.cpp:271`
  (`docs/lessons.md:166`). `closeAfterCapture` defaults to `true` and its three-state semantics
  (`RenderHandler.cpp:395-412`) must be carried into every provider verbatim, including that an
  explicit `true` closes a window the caller already had open.
- **Capture size must not vary within a session.** A varying capture size trips
  `Assertion failed: ProxyMap.Num() == TestSizeX * TestSizeY` in `FViewport::GetHitProxy` — the
  incident cost 66 actors and 125 emitters of unsaved level state. Constant size is proven safe over
  48 cycles. `camera.animation_shots` resolves resolution once for exactly this reason
  (`AnimationShotsHandler.cpp:23-29`); every converted verb must keep that property.
- **C11 collides with the crash fix by file.** `RenderHandler.cpp` is the only overlap in the wave,
  and it is unavoidable: that file holds two of the eight verbs. §7's gate is the mitigation.
- **The `subject` block must not become the fourteenth absence assertion nobody knew about.** Emit it
  only when a subject was resolved, and document that rule in C13 rather than leaving it to be
  discovered from a diff.

---

## Bugs found

1. **`render.capture_asset_preview` accepts three undeclared parameters.** `allowBlank`,
   `viewDistanceScale` and `hideEditorSprites` are read by the shared
   `ParseViewportCaptureRequest` (`PreviewViewportCaptureUtils.cpp:1279, 1316, 1337`) and the handler
   explicitly branches on `allowBlank` at `RenderHandler.cpp:320` — but none is in the verb's
   `RPC_PARAMS` list (`:253-268`). One of them, `hideEditorSprites`, is documented in the shared
   header as deliberately *not* offered on preview verbs (`PreviewViewportCaptureUtils.h:29-31`). So
   the schema, the header comment and the behaviour disagree three ways.
2. **Two unchecked downcasts, duplicated in two files.** `static_cast<FAssetEditorToolkit*>(EditorInstance)`
   (`RenderHandler.cpp:90`, `AnimationPreviewCaptureHandler.cpp:143`) with no check that the toolkit
   is one, and `StaticCastSharedRef<SEditorViewport>(Widget)` off a `GetTypeAsString().EndsWith("EditorViewport")`
   match (`RenderHandler.cpp:55`, `AnimationPreviewCaptureHandler.cpp:105`). `SEditorViewport` has no
   `SLATE_DECLARE_WIDGET`, so there is no runtime hierarchy check to justify the cast — any widget
   whose type name merely ends in `EditorViewport` is undefined behaviour. C1 removes both.
3. **`render.capture_annotated` uses eight raw error-code string literals** — `TEXT("INVALID_ARGUMENT")`
   `:227, 301`, `TEXT("CLASS_NOT_FOUND")` `:313`, `TEXT("EDITOR_NOT_AVAILABLE")` `:325`,
   `TEXT("NO_EDITOR_WORLD")` `:331`, `TEXT("NO_ACTIVE_LEVEL_VIEWPORT")` `:339, 345, 352`,
   `TEXT("DECODE_FAILED")` `:379, 390`, `TEXT("ENCODE_FAILED")` `:812`, `TEXT("SAVE_FAILED")` `:833` —
   instead of the `ErrorCodes::ERR_*` constants used everywhere else. `PinWrightCameraFrame::GetActiveLevelViewport`
   (`CameraShotPlanUtils.h:139-185`) has the same defect for three codes.
4. **`render.capture_annotated` computes `imageStats` and `blank` and throws them away.**
   `CaptureEditorViewportToPng` fills them; the hand-built response at `:842-903` omits `imageStats`,
   `blank`, `crushed`, `blownOut`, `rangeWarning`, `redrawRetries` and `framing` entirely. It reports
   the annotated PNG's byte count and nothing about its pixels — on the one verb whose entire purpose
   is a frame a human will read.
5. **The `renderer` vocabulary has two spellings for one renderer.** `OrthoTileCaptureHandler.cpp:744, 835`
   writes `"sceneCapture2D"`; `ZFightingHandler.cpp:638` writes `"sceneCaptureComponent2D"`. That
   field exists so a comparison can refuse a cross-renderer pair
   (`docs/wiki-src/render.md:170`); two spellings for the same path defeat exactly that check.
6. **`resolutionSource` has three different vocabularies across three verbs.** `orbit_shots`:
   `"caller"|"budget"|"default"`. `animation_shots`: `"caller"|"burstBudget"|"singleStill"`.
   `frame_actor`: no such field at all. Same concept, same namespace, three answers.
7. **`warmupWarning` is emitted by two unrelated producers with different meanings** —
   `PoseListCapture.cpp:151` ("the throwaway PNG could not be deleted", inside `poseSet`) and
   `PreviewViewportCaptureUtils.cpp:2399` ("the frame never settled", inside `viewport.warmup`).
   Different parents, so no JSON clash, but the string is ambiguous to anyone grepping for it and to
   any caller that flattens the response.
8. **`camera.md` describes a two-verb namespace that has had three verbs for some time.** `:11` and
   `:29` both open "Both verbs"; the same page's generated `## Methods` index at `:70` lists three,
   and `:47` discusses `camera.animation_shots` by name. The "Live viewport requirement" and view-mode
   sections are therefore silently untrue for one third of the namespace.
9. **Two verbs document zero response fields.** `camera.animation_shots.md` and
   `render.capture_animation_preview.md` end at the parameter list with no `## Notes` section, while
   `render.md:37` promises behaviour for both by name. The only surviving record of
   `poseSampled` / `poseChanged` / `frames` / per-shot `imageStats` is the unreachable test block in
   `TestAnimationCaptureHandlers.cpp:405-409`.
10. **`render.md:19-27`'s `viewport` field table omits five of its own sub-blocks** — `exposure`,
    `warmup`, `viewModeOverride`, `aim`, `editorSprites`. All five are described elsewhere on the same
    page and all five are asserted by tests; the table that presents itself as the field list is the
    one place they are missing.

### Critical Files for Implementation
- `Source/PinWright/Private/Handlers/Render/PoseListCapture.h`
- `Source/PinWright/Private/Handlers/Render/PreviewViewportCaptureUtils.h`
- `Source/PinWright/Private/Handlers/Render/CameraShotPlanUtils.h`
- `Source/PinWright/Private/Handlers/Render/RenderHandler.cpp`
- `Source/PinWright/Private/Handlers/Render/AnimationPreviewCaptureHandler.cpp`
- `Source/PinWright/Private/Handlers/Render/AnimationShotsHandler.cpp`
- `Source/PinWright/Private/Handlers/Render/CameraFrameHandler.cpp`
- `Source/PinWright/Private/Handlers/Render/AnnotatedCaptureHandler.cpp`
