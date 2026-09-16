# Engine-source research from the 2026-08 plugin-engineering pass

**Read this as research, not as a work order. Every patch it stages has already shipped.**

Salvaged verbatim (below the horizontal rule) from the host project's
`docs/plugin-engineering/pending-never-applied/RECONCILE_BEFORE_APPLY.md`, which was deleted
on 2026-08-16. That tree was gitignored in the host project, so this file is the only
surviving copy. It is kept here because `docs/README.md` in the host project rules that
plugin engineering notes belong in the plugin repo.

## Why the original was deleted

It presented eleven completed-but-unapplied patches and closed with *"ALL 11 PATCHES
COMPLETE. Nothing applied yet; plugin tree still clean at HEAD `21e068e5`."* That claim was
already false when written down and is now **122 commits stale** — `21e068e5` is 2026-08-10,
and the work landed on 2026-08-13:

| Subject | Commit |
|---|---|
| Stop modal dialogs from deadlocking the game thread during RPCs | `7ff9dcf3` |
| Render orthographic captures instead of returning blank images | `a70d6337` |
| Mark the level dirty from geometry verbs so mesh edits survive a save | `0f32bacc` |
| Refuse to duplicate a dynamic mesh the engine replaced with a placeholder | `ca23d1d6` |
| Reject landscape layer paints the material cannot satisfy instead of faking success | `6461f60d` |
| Return a job ticket from pcg.generate instead of holding the request open | `a4a44281` |

The tree misled three readers, each of whom mined the patch bodies for current state. A
`SUPERSEDED-BY` header was rejected as the remedy: the hazard is 800 KB of confident,
line-numbered hunks written against a tree that has since moved, and a one-line banner does
not stop a fourth reader from mining the body. The patch corpus was deleted; this file — the
only part that is engine-source research rather than apply instructions — was kept.

## Its own verification hooks are unreliable — do not re-run them literally

The document tells you to confirm application by counting marker strings. At least one gives
a **false negative**, and re-running it is a plausible source of one of the three misleadings:

- **"Rule P must appear exactly 5 times in `PostProcessHandler.cpp`"** — `grep -c "Rule P"`
  returns **0**. The mechanism is present exactly five times as
  `PinWright::MarkLevelActorModified(PPV);` at `Source/PinWright/Private/Handlers/Environment/PostProcessHandler.cpp:107,166,227,281,422`.
  The applier used different comment wording. The patch shipped; the hook was written against
  prose that never landed.
- The `NotifyMeshUpdated();` checksum, by contrast, **is** satisfied: exactly one remains
  tree-wide, as the document requires after applying.

Confirm against the code, never against a marker comment.

## What is still worth reading

The engine-source findings, which are version-pinned and were not superseded by shipping:

- `FSlateApplication::AddModalWindow` cancels under `GIsRunningUnattendedScript` on all six
  engine versions, with per-version line numbers (5.3–5.8).
- The `PromptForCheckoutAndSave` flag asymmetry: `GIsRunningUnattendedScript` takes a
  non-prompting save path (`FileHelpers.cpp:4659`) while `FApp::IsUnattended()` returns
  `PR_Cancelled` and saves nothing (`:4664`) — so bare `-unattended` on the visible path would
  silently break `editor.quit {save:true}`.
- Why `FCoreDelegates::ModalMessageDialog` is unsafe as a global intercept (single-cast, already
  bound by `UEditorEngine::Init`).
- Why orthographic capture ignored camera rotation (`EditorViewportClient.cpp:1341-1401`), with
  the blank-image test run that confirmed it.
- Several test traps: `SetActorLabel` comparing against `GetActorLabel(false)`, and
  `SpawnActorInActiveWorld` forking on run mode so a naive dirty test passes even with the fix
  reverted.

**Not verified:** whether every engine detail below was carried into `ScopedUnattendedRpc.h` /
`ModalStateProbe.cpp`. It was salvaged whole rather than pruned for that reason — treat the
line numbers as leads to re-check against the engine, not as current citations.

---

# Conflicts to resolve BEFORE applying staged patches

## APPLY ORDER (strict — dependencies are compile-level, not stylistic)
1. `patch_robustness.md`      — D1 is the CANONICAL spawn_batch `skipped` impl. Edit 3.6 MANDATORY with D3.
                                Edits 2.6+2.7 MUST go together. Edit 2.8 is optional + NOT additive.
2. `patch_spawn_material.md`  — MINUS its duplicate `skipped` hunk (dropped in favour of robustness D1).
3. `patch_ortho.md`           — adds `FViewportCaptureOutput::EffectiveRotation` (§2b) and
                                `ResolveOrthographicView`; deletes ViewProjectionUtils' mirrored
                                `ApplyCaptureCamera`. Touches ErrorCodes.h (registers
                                UNSUPPORTED_ORTHOGRAPHIC_ROTATION, which was never registered).
4. `patch_actor_labels.md`    — **HARD DEPENDENCY ON 3.** Its behind-camera guard consumes
                                `EffectiveRotation` from ortho §2b. Applying it without ortho is a LOUD
                                COMPILE ERROR (intentional, not a bug). Anchors are all post-ortho text.
                                Deliberately does NOT touch ErrorCodes.h (ortho owns that file).
5. `patch_mesh_roundtrip.md`  — NO COLLISIONS (verified). Its geometry edits are in `MeshOpsHandler.cpp`;
                                robustness D4-d/D4-e are in `PrimitiveHandler.cpp`/`SplineHandler.cpp`.
                                Its `geometry.md` anchors deliberately avoid D4-d's `### geometry.create_box`
                                block. Does NOT touch `ErrorCodes.h` (ortho owns it). No Build.cs change
                                (`PinWrightGeometry.Build.cs:54` already links GeometryScriptingCore).
                                WATCH: if labels changes label dedup, its idempotency test's
                                "exactly one actor" assertion may need adjusting.
6. `patch_geometry_modify.md` — applies AFTER robustness + mesh_roundtrip (they drift geometry lines).
                                Lives entirely in `Source/PinWrightGeometry/`. Adds 2 fns to the existing
                                `namespace GeometryUtils` in `GeometryUtils.h/.cpp` — ZERO new includes at
                                any call site. 48 of the sites are specified as a MECHANICAL TEXT-SWAP RULE
                                rather than line-numbered hunks, deliberately, so it survives line drift.
                                **REVIEW CHECKSUM: 49 `NotifyMeshUpdated();` exist today; exactly ONE must
                                remain after applying (the one inside the helper).** Verify this count.
                                ACCEPTED CORRECTION: its tests go in `Source/PinWrightGeometry/Private/Tests/
                                Geometry/`, NOT `Source/PinWright/Private/Tests/` as I briefed — per CLAUDE.md
                                typed tests moved with their handlers, and a test spawning an
                                ADynamicMeshActor cannot link in the main module. It is right, I was wrong.
7. `patch_wiki_level_building.md` — REWRITTEN (472 lines). C1 is RESOLVED: the false ortho workaround
                                is deleted. **Now ships TWO pages** — `level-blockout.md` (225 lines,
                                building) and `blockout-review.md` (120 lines, capture/framing/review
                                poses). Hunk B2 (`level.md` prelude) RETAINED and now links both — it is
                                REQUIRED for root-index reachability, do not drop it.
                                **APPLY NOTE: `patch_backlog.md` section 3b's `level-blockout.md` hunk will
                                find no matching OLD text — that is its own documented success case. SKIP
                                3b. Section 3a (`camera.md`) is unaffected and still applies.**
                                Page bodies are wrapped in FOUR-backtick fences because they contain
                                three-backtick code blocks.
8. `patch_dirty_mainmodule.md` — COMPLETE (1116 lines). Zero collisions (verified by grep against all
                                eight others — only prose mentions, no hunks). Order-independent.
                                New header `Handlers/Environment/EnvironmentDirtyUtils.h`, 5 files edited,
                                1 new test file. **VERIFICATION HOOK: "Rule P" must appear exactly 5 times
                                in `PostProcessHandler.cpp` after applying.**
                                CRITICAL DIFFERENCE FROM patch 6: here `Modify()` goes **BEFORE** the
                                mutation, because every mutation IS a UPROPERTY of the object being
                                Modify()-ed (post-hoc would snapshot the changed value). The geometry
                                patch calls it post-mutation because mesh edits touch no actor UPROPERTY.
                                Do not "harmonise" these two — the difference is deliberate and correct.
9. `patch_no_modals.md` — COMPLETE (1139 lines). **It REFUTES a claim in patch_backlog.md B0 — resolve
   in its favour, it cites six engine versions line by line:**
   - B0 §1.2 says raw Slate modals ignore `GIsRunningUnattendedScript` and that UE 5.5/5.6 "deadlock
     anyway". FALSE. `FSlateApplication::AddModalWindow` CANCELS the window under that global on ALL six
     versions (5.3:1990, 5.4:2004, 5.5:2030, 5.6:2032, 5.7:2098, 5.8:2134), and
     `GEditor->EditorAddModalWindow` routes through it (`EditorEngine.cpp:4172`).
     => B0-a alone closes the deadlock on 5.3-5.8. B0-b (referencer pre-flight) is still worth shipping
     for its typed `ASSET_IN_USE` contract, but it is NOT load-bearing for 5.5/5.6 safety as B0 claimed.
   - **THE FIX FOR THE HANG WE ACTUALLY HIT: `-AutoDeclinePackageRecovery`.** Parsed in
     `FPackageAutoSaver`'s ctor (`PackageAutoSaver.cpp:157` on 5.8), consumed at `:683`. Two references
     engine-wide — surgical and safe on a VISIBLE editor. `editor_start` currently passes NO flags on the
     visible path (`mcp_proxy.py:738-747`); `_HEADLESS_FLAGS` already has `-unattended`, which is why only
     visible editors were exposed to the Restore Packages modal.
   - **DO NOT put bare `-unattended` on the visible path.** `PromptForCheckoutAndSave` treats the two
     flags OPPOSITELY: `GIsRunningUnattendedScript` takes a non-prompting SAVE path
     (`FileHelpers.cpp:4659`), but `FApp::IsUnattended()` returns `PR_Cancelled` and **saves nothing**
     (`:4664`). Bare `-unattended` would silently break `editor.quit {save:true}`.
   - Global modal intercept via `FCoreDelegates::ModalMessageDialog` is **UNSAFE** — single-cast and
     already bound by `UEditorEngine::Init` (`EditorEngine.cpp:1291`); binding it from a plugin would
     replace the editor's own dialog renderer process-wide.
   - Watchdog seam is `FSlateApplication::GetOnModalLoopTickEvent()` (broadcast INSIDE the nested loop,
     `SlateApplication.cpp:2253`, uniform 5.3-5.8). `ping` is already served entirely on the I/O thread
     from three atomics, but the snapshot is stale-but-POSITIVE while blocked, and the completion-timeout
     sweep dies with the game thread — so the server's own 120s/300s timeouts never fire.
   - `FScopedSlowTask` is NOT in this bug class (`bSlowTaskWindow` exempts it and keeps the game thread
     running, `SlateApplication.cpp:2217`).
   - CANNOT be prevented: four `LaunchEngineLoop::PreInit` dialogs (`:6662`, `:6708`, `:6713`, `:6729`)
     use raw `FPlatformMisc::MessageBoxExt` before any plugin exists; `:6662` and `:6729` have NO guard.
     Also: an unblocked raw Slate modal returns an UNINITIALISED value, so "no deadlock" != "correct
     outcome" (`ObjectTools.cpp:937` picks overwrite-vs-cancel this way).
   - Collisions: `mcp_proxy.py` + its test with robustness D3 (different regions, re-read before editing);
     `PinWrightSettings` + `docs/wiki-src/system.md` with backlog B0-a (extends, does not duplicate).
10. `patch_backlog.md` — COMPLETE (1170 lines). Findings that CHANGE other patches:
    - **B0's real trigger is `python.execute`, NOT a typed create verb.** `material.authoring.create_material`
      uses `CreatePackage`+`FactoryCreateNew` and CANNOT raise that dialog. The 3 modals were
      `UAssetToolsImpl::CanCreateAsset` (`AssetTools.cpp:4884->4899-4911->4920->4934`) reached via a user
      python script. So the same deadlock is reachable from ANY script -> the guard belongs at the
      DISPATCHER (`RpcDispatcher.cpp:326`, `FScopedUnattendedRpc`), not per-handler.
    - **UE VERSION MATRIX: 5.5/5.6 do NOT suppress** — raw `SMessageDialog::ShowModal()` with no
      `GIsRunningUnattendedScript` guard. 5.3/5.4 and 5.7/5.8 do. So the dispatcher guard ALONE does not
      close the bug class; the create-path referencer pre-flight is LOAD-BEARING, not belt-and-braces.
      `bOverwriteExisting` is 5.8-only and unusable portably.
    - **B6 is REFUTED — do NOT add the `IsCreatedByConstructionScript()` guard.** It returns true for SCS
      components too (`ActorComponent.cpp:994-997`), so the guard would reject nearly every `classPath`
      spawn. And the override DOES survive rerun: `FComponentInstanceDataCache` re-applies it
      (`ActorConstruction.cpp:927/960`) and `ShouldSkipProperty` skips only transient/non-CPF_Edit props;
      `OverrideMaterials` is neither. Ship the additive warning only.
    - **B5 deferral REFUTED — fix it.** The SCS material load consumes only the path string, so hoisting
      the pre-flight is free. Worse than "silent": on failure the node is already created AND the
      Blueprint is compiled and saved. Only genuinely breaking change in the set, deliberately narrow.
    - **B3: BOTH claims were right.** `level.save` IS ticketed (`LevelHandler.cpp:351`) but
      `HandlerContext.cpp:597-616` suppresses the ticket for streaming requests (the default for
      progress-capable MCP clients). `level.md` is the stale surface, not the behaviour.
    - **`patch_wiki_level_building.md:167` STILL contains the false ortho workaround** that C1 ordered
      removed. B4's hunks fix `camera.md:53`; the level-blockout page must be corrected too before it ships.
    - Two new error codes ship as STRING LITERALS; `ErrorCodes.h` untouched (ortho owns it) — flagged as a
      post-ortho follow-up to register them properly.
    - Re-triage: 1 FIX (R7 `foliage.add_instances` silently drops `locations` when `transforms` present —
      same silent-elision class), 7 KEEP DECLINED, 0 needing a user decision. R1's principled decline was
      preserved verbatim as the model for the table.
11. `patch_volume_dirty.md` — COMPLETE (56 KB). Corrections it made to MY brief:
    - `VolumeHandler.cpp` has **26 verbs, not ~12**; **25 of 26** have zero ceremony. The 3 `set_*` verbs
      mutate PRE-EXISTING volumes and are the WORSE defect — my brief only mentioned spawns.
    - `AreDynamicDataChangesAllowed` is **`protected`** — a handler cannot call it; the predicate must be
      spelled out. Its `bIgnoreStationary` defaults **true**, so only *Static* is blocked, and
      `USkyLightComponent` defaults Stationary (`SkyLightComponent.cpp:312`) — so the silent no-op fires
      only on author-set Static lights. Narrower than I implied.
    - Found the IDENTICAL defect in `environment.control.set_sun_intensity` (`EnvironmentHandler.cpp:616`),
      not in the brief. Fixed. `lighting.spawn_light` is safe (forces Movable `:216-217`),
      `spawn_sky_light` safe, fog and PPV not gated at all.
    - **`ErrorCodes.h` is a REGISTRY enforced by `TestErrorCodeRegistry`** — adding a code would force an
      edit to the file `patch_ortho.md` owns. This is why it chose the direct-UPROPERTY-write fix (d)
      over a typed error. Also rejected `SetMobility` because it runs two `FComponentReregisterContext`s
      and **invalidates the level's baked lighting**.
    - Reused `EnvironmentDirtyUtils.h` unchanged — no fourth helper. 25 volume verbs close at just
      **3 choke points** (`SpawnVolumeActor` x2 overloads, `BuildBoxBrushGeometry`) + the 3 `set_*` bodies.
    - **TEST TRAP, worse than the one patch 8 found:** `SetActorLabel` compares against
      `GetActorLabel(**false**)`, and `SpawnActor` calls `ClearActorLabel()` on fresh actors
      (`LevelActor.cpp:695-701`), so the label is EMPTY on spawn and ANY non-empty name dirties
      unconditionally. Every volume verb passes a non-empty `volumeName` (default `"TriggerVolume"`).
      => **No volume spawn verb can produce a failing dirty test on a classic map.** Guards are the
      label-free `set_*` verbs; the spawn test self-disables loudly rather than passing vacuously;
      `environment.spawn_reflection_capture` with `name` OMITTED is the one strictly discriminating case.
    - **Scope honesty:** the volume SPAWN defect is real only under OFPA/World Partition — on classic maps
      the accidental label dirty genuinely does dirty the level. Narrower than my brief implied.
    - Collision: `EnvironmentHandler.cpp` x `patch_dirty_mainmodule.md` C1/C2 — consumed by quoting the
      post-C1/C2 text as its OLD. Everything else is prose-only.

## ALL 11 PATCHES COMPLETE. Nothing applied yet; plugin tree still clean at HEAD 21e068e5.

## CORRECTIONS TO MY OWN BRIEFINGS (agent was right, I was wrong)
- `LightingHandler.cpp` is **7 of 9** verbs, not 9 — `setup_global_illumination` and `configure_shadows`
  are CVar-only and touch no UObject.
- `PostProcessHandler.cpp` is **5 of 6** + resolver — `set_anti_aliasing` is CVar-only.
- Post-process settings are **NOT component-resident**. `FPostProcessSettings Settings` is a UPROPERTY on
  `APostProcessVolume` itself (`PostProcessVolume.h:27-28`); the renderer samples the volume list per
  frame. PPV verbs need ACTOR ceremony only — no `MarkRenderStateDirty`.
- **`bOverride_*` is NOT a defect.** All 28 value writes across the 8 PPV-touching verbs already flip
  their paired override on the preceding line. My information was stale. (The foot-gun does remain
  reachable via generic `property.set` — unchanged.)
- Two ACCIDENTAL-DIRTY paths mask this bug and make the obvious test useless: `SetActorLabel` calls
  `Modify(true)` but only when the label actually changes and never on the level; and
  `SpawnActorInActiveWorld` forks on run mode (interactive -> `UEditorEngine::AddActor` ->
  `MarkPackageDirty`; unattended/NullRHI -> bare `World->SpawnActor` -> nothing). So `lighting.spawn_light`
  PASSES a naive dirty test even with the fix reverted. Tests must use `setup_volumetric_fog` and
  `set_bloom` instead.

Overlaps already resolved: robustness + spawn_material both touch `Handlers/Actor/SpawnBatchHandler.cpp`
and `docs/wiki-src/actor.md`. ortho + labels both touch `ViewProjectionUtils.*` and
`AnnotatedCaptureHandler.cpp` — labels was authored against post-ortho text, so order 3-then-4 is required.
Re-read every file before editing; later hunks may not match verbatim after earlier edits.

## C1 — `camera.frame_actor` / `camera.orbit_shots` ortho: DOC AGENT IS WRONG (probably)

`patch_wiki_level_building.md` documents these as the **verified workaround** for top-down ortho,
citing that they "compute their own look-at pose and pass `bRejectArbitraryOrthographicRotation=false`".
That claim comes from the wiki text.

The ORTHO agent contradicts it from ENGINE SOURCE:
`FEditorViewportClient::CalcSceneView` does NOT read camera rotation for orthographic views —
`ViewInitOptions.ViewRotationMatrix` is hard-coded per `ELevelViewportType`
(`C:/UE_5.8/Engine/Source/Editor/UnrealEd/Private/EditorViewportClient.cpp:1341-1401`);
only `ViewOrigin` comes from the camera. `LVT_OrthoFreelook` (what the plugin sets,
`PreviewViewportCaptureUtils.cpp:62`) gets a matrix byte-identical to `LVT_OrthoYZ` = pose (0,0,0).
=> `SetViewRotation` is silently discarded in ortho; the canonical "top/front/side" orbit set renders
the SAME +X side view three times, and the subject is pushed off-frame because ortho centres on ViewOrigin.
The in-repo comment at `CameraFrameHandler.cpp:291-294` asserting otherwise is wrong.

**Source-based analysis beats doc-repetition. Do NOT ship the doc page claiming this workaround works.**

### RESOLVED 2026-08-12 — TEST RUN, ORTHO AGENT CONFIRMED. Doc claim is FALSE.

Ran `camera.orbit_shots {point:{x:0,y:0,z:0}, radius:40000, width:1024, height:1024}` on the built terrain.
Results:
- shot00 (az45/el30, PERSPECTIVE) -> renders the scene CORRECTLY: green east half, grey west half,
  blue river between them. 563 KB.
- shot01 (az0/el0, ortho)  -> **BLANK WHITE**. 28 KB.
- shot02 (az90/el0, ortho) -> **BLANK WHITE**. 21 KB.
- shot03 (az0/el90, ortho "TOP") -> **BLANK WHITE**. 20 KB.

The scene renders fine in perspective from the same call, so the blankness is ORTHO-SPECIFIC, not a
scene/lighting problem. Reported `orthoWidth` was 1380000 — absurd under either conversion rule, which
also corroborates the ortho agent's finding that the ortho zoom math is wrong.

CONSEQUENCES:
1. The wiki's claim that `camera.frame_actor`/`camera.orbit_shots` are a working ortho workaround is
   FALSE and must not ship. Fix `docs/wiki-src/camera.md` too, not just the new page.
2. The new `level-blockout` page must say: top-down ORTHOGRAPHIC capture does not work in any surface
   today; use the perspective top-down recipe. Remove the camera.* workaround recommendation entirely.
3. This is stronger evidence than the ortho agent had (it argued from source; we have blank PNGs).
   Add it to the board entry as the reproduction.
4. After the ortho patch is applied, RE-RUN this exact test — it is the acceptance test for the fix.

## C2 — `level.save`: docs say async ticket, EMPIRICALLY it returned synchronously

Doc agent (from wiki): "`level.save` takes no parameters and is an async ticket job, not a blocking save."
Foundation agent (ran it): returned `{"saved":true,"levelPath":...}` directly — no ticket, no
`system.job_status` poll needed.

Both may be true (fast path returns inline), or the wiki is stale. VERIFY by calling `level.save {}`
and inspecting the raw response. Whichever is true, the DOC PAGE MUST MATCH REALITY.
If the wiki is stale, that is a doc bug worth fixing in `docs/wiki-src/level.md` in the same commit series.

## C3 — `BasicShapeMaterial` params: unsourced in PinWright docs, but WE VERIFIED THEM

Doc agent correctly flagged that `/Engine/BasicShapes/BasicShapeMaterial`'s `Color` vector param /
`Roughness` scalar / absent emissive appear nowhere in the PinWright wiki — it is engine knowledge.
RESOLUTION: it IS verified, just not by the wiki. The foundation build created 12 MaterialInstanceConstants
parented to it, set `Color` + `Roughness`, and read all 12 back successfully; a binary scan of the uasset
also showed exactly one VectorParameter `Color` and one ScalarParameter `Roughness`.
=> The claim is safe to ship. Keep it.

## C4 — My briefing had several WRONG arg names (doc agent corrected them; fix the playbook too)
- `material.authoring.create_material_instance`: params are `name` (required) + `path` (folder,
  default `/Game/Materials`); `assetPath` is an ALIAS OF `name`, not a separate arg.
- `spatial.verify_placement`: first arg is `actor`, NOT `actorName`. `expect` also supports an `on` key.
- `level.create` has NO `save` arg.
- `render.capture_open_level` has no boolean `orthographic` — it is `projectionMode`.
- (Correct as I had them: `static_mesh.set_material` -> `materialIndex`; `actor.set_folder` -> `folderPath`
   while `actor.spawn_batch` -> `folder`.)

## C5 — Doc page structural constraint
Topic pages must NOT use `###` — rendering STOPS at the first one (`docs/wiki-src/README.md:50`).
The page uses `##` only. Preserve this when editing the page later.

## C6 — Root index does NOT auto-list guide pages
Generated `index.md` renders only namespace preludes + `## Support`. Existing guides are reachable from
the root only because a namespace prelude links them. So hunk B2 (`docs/wiki-src/level.md` prelude link)
is REQUIRED for discoverability — not optional polish. Do not drop it.
