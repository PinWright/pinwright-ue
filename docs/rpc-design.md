---
type: guide
summary: "Design rules for new PinWright RPC verbs: report only what happened, structural guarantees over discipline, required parameters, independent verification, the three levels of persistence (including derived-value writes and consumer refresh), batching and job handles, tick safety, error codes, failure-direction tests, wiki rendering limits, keeping the advertised engine range true, refusing to let a run that measured nothing report as one that measured, declaring what a parameter IS (path / classref / filepath) rather than only its JSON shape, and guarding the load itself where the boundary cannot see the value (nested values, IR source text). Living doc."
date: 2026-09-03
tags: [rpc, handlers, design, verification, persistence, derived-state, jobs, cancellation, error-codes, tick-safety, testing, engine-compat, paths, param-types, nested-params, ir-compilers]
---

# Designing a new RPC verb

Every rule below comes from a defect that shipped in this plugin. Attributions are kept so a rule can be argued with on its evidence rather than on taste.

Scope: interface and contract decisions for a new verb. Layer mechanics (dispatcher, `FHandlerContext`, auto-registration, `REGISTER_RPC_HANDLER`) are in [arch.md](arch.md); one-off engine quirks go to [lessons.md](lessons.md).

The end-user counterpart is `docs/wiki-src/level-review.evidence-and-provenance.md`, which states §4 (verification the write path cannot fake), §5 (the three levels of persistence) and §6 (both failure directions) as rules for agents *using* these verbs to build a level. Keep the two consistent — a rule that changes here usually changes there — but do not merge them: this page is about the verb's interface, that one is about trusting its output.

**This page is living.** When a verb ships a hard-won design lesson, add it here — the rule plus the incident that produced it, in the section it belongs to. A lesson that only shapes one call site belongs in `lessons.md`; a lesson that would shape the *next* verb's interface belongs here.

---

## Before you ship a verb

- [ ] Every boolean in the response is computed from a measurement, not a literal. Grep the diff for `SetBoolField(..., true)`.
- [ ] Failure is the default: every early-out and every default-constructed result reports failure.
- [ ] Every parameter with no correct default is required. Unknown preset/enum values error instead of falling back. No per-verb `MissingRequiredParam` test — `infra.contract.RequiredParamGate.EveryVerb` covers the declaration (§3).
- [ ] Every registry/asset query whose results the verb then mutates or deletes is scoped by default to what the caller named, the wide scope is a named opt-in, and the response lists what was touched outside the request (§3).
- [ ] Every alternate spelling the body reads is declared in that parameter's `FParamSpec` `Aliases`. The dispatcher's unknown-parameter gate runs before the handler, so an alias that lives only in prose or in a `GetStringFirstOf` list is rejected with `UNKNOWN_PARAMS` and the body never sees it.
- [ ] Verification reads something the write path cannot fake.
- [ ] Persistence claims match reality — mark-dirty ≠ saved ≠ survives reload.
- [ ] The property being written is the authority, not something the engine re-derives from other state on load. If it is derived, the verb refuses and names the authoritative verb.
- [ ] Consumers that cache data derived from the edited asset were refreshed, and the coverage is reported as a measurement.
- [ ] Measurements are signed or paired, so the two failure directions cannot score alike.
- [ ] It is a batch verb, not a caller loop. An empty match set is an error, not a zero-item success.
- [ ] Long operations use `Ctx.StartJob` with a real completion hook; if cancellation is impossible, the verb says so instead of faking it.
- [ ] Tick-unsafe work is gated through `Dispatch/SafePoint.h` — table entry preferred over a hand-written gate.
- [ ] Global state the verb changed is restored before it returns.
- [ ] A read verb leaves every package's dirty flag exactly as it found it — preserve, not clear (§11). A "Get"-named engine API is not automatically const.
- [ ] Every emitted error code is declared in `Handlers/ErrorCodes.h`, and recovery information is in the structured payload.
- [ ] Every engine symbol the verb newly depends on exists on the lowest advertised engine — or is guarded, or the narrowing is recorded in [engine-version-support.md](engine-version-support.md).
- [ ] Tests assert the failure direction, not only success.
- [ ] If the result is visible, it was **looked at** — not only measured. The verb makes the artifact cheap to obtain and characterises it rather than asserting a property of it. A check that examined nothing does not report a pass.
- [ ] Every test whose assertions can be conditionally stepped over emits `PINWRIGHT_ASSERTIONS_SKIPPED` on that path, so the suite verdict can refuse to read the run as clean (§17). A skip with no marker is invisible downstream.
- [ ] An **audit** verb derives `pass` through `PinWrightAudit::FVerdict::DerivePass` and takes its check table, `ParseCheckId` and `passRule` from `Audit/AuditFramework.h` — never a private copy (§18). A malformed argument is an RPC error before the sweep, not an `unrunnable` row.
- [ ] Wiki overlay: every `##` section sits above the first `###`; method sections are the bare dotted name.
- [ ] A verb opening a **new top-level namespace** has a `docs/wiki-src/maturity.json` entry (`core` | `experimental` | `internal`). Without one the namespace renders `(unclassified)` and `PinWright.infra.wiki_handler.Maturity.EveryRegisteredNamespaceIsClassified` fails naming it.

---

## 1. Report only what happened

The dominant defect class in this plugin: a verb reporting success for work it did not do. An agent cannot see the editor; the response *is* the world as far as it is concerned, so a false success is not a cosmetic bug — it is the agent proceeding on a false belief and building more work on top of it.

Shipped instances:

- `McpSafeAssetSave` returned a hardcoded `true` for any non-null pointer and never wrote a `.uasset`. Every `SetBoolField("saved", McpSafeAssetSave(X))` published a constant under a measurement name.
- `existsAfter` was written as the literal `true` beside a real probe emitting the same field name — two fields agreeing with each other while both disagreed with the disk.
- `system.job_cancel` returned `{cancelled: true}` for any ticket in `running`. Only 5 of 20 ticketed verbs register a cancel hook; the other 15 have no mechanism at all. (Re-counted 2026-08-16 by enumerating every `Ctx.StartJob` call site against every `SetCancelCallback` one: the hooks are `pcg.generate`, `asset.dump`, `asset.dump_folder`, `localization.gather`, `localization.compile`. The earlier "4 of 19" collapsed the two localization verbs, which share a `StartJob` path; the count of uncancellable verbs was right both times, which is why nobody caught it.) One recorded run wrote ~19,700 files after a "successful" cancel — and cancelling is exactly what an agent does *in response to a hang*.
- A placement verb invented a surface at the caller's own point on a trace miss and returned `placed: true`.
- A track verb returned an identifier that resolved nowhere.
- An event verb echoed failed pins back as created.

Rules:

- A response field named for an outcome must be derived from an observation of that outcome.
- When a capability genuinely does not exist, the verb reports its absence (`ERR_JOB_CANCEL_UNSUPPORTED`). Faking the capability is strictly worse than not having it.
- Do not fake-success a method that cannot run on the current engine either — that is what `Ctx.SendUnsupportedEngineVersion` is for.
- **A write that lands in one of several engine state slots must name the slot, measured.** `FEditorViewportClient` keeps `PerspViewModeIndex` and `OrthoViewModeIndex` separately and `SetViewMode` writes only the one matching the current projection (UE 5.8 `EditorViewportClient.cpp:6460-6485`). `editor.set_view_mode` wrote it honestly and returned `{"success":true,"viewMode":"Lit"}` — true about the write, silent about the slot — so every orthographic capture kept rendering the previous mode. This is the hardest sub-class to spot: the verb is not lying, it is answering a narrower question than the caller asked. The fix is both halves — write every slot the caller could plausibly mean (`projection` defaults to `"both"`), *and* report each slot's mode read back off the client, plus a `previous` block so the change is restorable.
- **When a family has two renderers, the response must name which one drew the pixels.** Every `render.capture_*` verb but one reads the live editor viewport's back buffer; `render.capture_ortho_tiles` renders an offscreen `USceneCaptureComponent2D`. They are two instruments, not two paths to one number: at identical framing they differ by **5.76% mean absolute error against a 1.30% viewport self-noise floor**, and fitting an ideal tone LUT between them only closes it to 3.28% (scene capture's own frame-to-frame spread is 0.12-0.54%). A comparison handed one frame from each would report the instrument as a content change, with nothing in either response to notice it by. So the capture records `renderer` in the response *and* in the manifest it writes, where a later `image.compare` can refuse the pair. The rule generalises past renderers: any measurement whose value depends on which mechanism produced it must carry the mechanism, or the two become silently interchangeable one call downstream.
- **A warning must be derived from the thing it warns about, not from something that correlates with it.** A capture published `viewport.exposure.warmupWarning` — "these pixels are ~1.8 stops dark, do not compare this frame against anything" — whenever the renderer had no completed eye-adaptation readback for the viewport, on the strength of that flag being true on the dark frame in 4 of 4 trials. It is not a detector: reopening the asset editor in a *separate* call and then capturing gives a fully warmed frame (`meanLuminance` 0.2437 and 0.2436 against 0.0506 for open-and-capture in one call) with the flag still true both times, because the flag tracks how young the view state is and, separately, is true of *every* pinned capture — pinning clears the show flag that refreshes the readback. So the warning fired on correct output and told the caller to throw it away. Correlation observed in a handful of trials is a hypothesis, not a measurement; if the response is going to make a claim about the pixels, measure the pixels. The fix pumps extra frames until the frame mean stops moving and reports `warmup {settled, settleRounds, meanLuminanceDelta, settleMs}`, and the readback flag was renamed to the narrow fact it actually is (`adaptedReadbackPending`). Corollary for the bound: a convergence wait that runs on every call must publish its own cost — the settled path costs one extra draw plus one readback and says so in `settleMs`, so nobody has to take "it is probably cheap" on trust.
- **A value the verb silently replaced is an unreported outcome, and the report has to be in the response.** `geometry.*` grew a full clamp-and-warn layer: every segment count below its engine floor is raised, and the op records "segments clamped from 1 to 3" on `FOpResult::Warnings`. The ops-layer tests asserted the array was populated and passed, and **not one RPC wrapper read the field** — the only consumer was the `.pwmodel` compiler. So a caller passing `segments: 1` received a bare success and never learned the engine had used 3, for the entire life of the clamps. The generalisation is a shape to sweep for, not a geometry detail: a producer of diagnostics with no consumer at the boundary the caller can see is invisible, and tests written one layer below that boundary go green while the contract is broken. Two rules follow. **Test at the layer the contract lives on** — a claim about the response is proved by driving the dispatcher, never by asserting the struct the wrapper receives. **And pick one shape for the optional field and keep it**: `warnings` is emitted only when non-empty (the plugin's dominant convention — `actor.*`, `material.*`, `render.*`, `landscape.*` all gate on `Num() > 0`; only the compile/decompile report families emit it unconditionally, because there the report is the payload), which is also what makes the addition byte-identical for every call that trips no clamp.
- **When a verb echoes a parameter it also clamps, the clamp must run before the echo is read.** Two `geometry.create_*` verbs echoed `numSteps` straight off the request while the engine floored it, so `numSteps: 0` came back as `numSteps: 0` for a staircase built with 1 step — not an omission but a false statement about the geometry. The structural fix is the op taking its params by non-const reference and normalising them in place, so the wrapper's echo reads the effective value and there is no second copy of the clamp to rot beside it (§2).
- **A verb whose coverage is partial must enumerate what it does not cover.** `editor.set_game_view` swaps one show-flag set and suppresses component visualizers; it does not govern editor-mode render passes or debug-drawn geometry, and its clearing of `EngineShowFlags.Splines` is conditional on which flag set `SetGameView` decides to reuse (`EditorViewportClient.cpp:7229-7240`). A confirmed `gameViewEnabled:true` therefore sat beside a water spline drawing down a river that a reviewer nearly logged as mid-channel foam. The verb now publishes the measured overlay flags, a `notGovernedByGameView` list, and a warning when it measures the contradiction itself. Reporting the gap is the fix; silently repairing it would be an unannounced global mutation (§11).
- **An empty result and an unanswerable question must not look the same.** `asset.get_dependencies` normalised its `assetPath` with `FPackageName::ObjectPathToPackageName` — a string operation that returns its input unchanged when there is no `.` — and never checked that the package existed. A typo'd, unmounted or already-deleted path therefore reached the registry as a node with no edges and came back `{"dependencies": []}` with `isError: false`, which is byte-for-byte the shape of a genuine "nothing references this". The verb sits on the delete path, so what a caller read before removing an asset was "safe to proceed". The rule is narrower than "validate input": **for a read verb an unresolvable target is an error (`ASSET_NOT_FOUND`), and the empty result is reserved for a question that was actually asked.** The response also echoes what the path resolved to (`packageName`), so an empty list reads as a measurement of a named package rather than a possible typo. Corollary: when several verbs take the same path slot, the accept/reject decision belongs in ONE resolver (§2) — the four asset dependency/reference verbs had three different answers for the same string, and one of them depended on whether the package happened to be loaded.
- **A cap on the output is not a cap on the work.** `asset.find_by_tag` must LOAD every candidate to read its UMetaData, and it broke out of the walk on `matches >= limit`. A tag nothing carries never reaches that cap, so the queries that return nothing were exactly the ones that loaded every asset under `/Game`, synchronously, on the game thread — and the comment sitting on the loop asserted the opposite ("the cap therefore bounds the SCAN as well as the output"), which is how it survived review. When the per-item cost is real work rather than a row copy, **the scan needs its own budget, defaulted and clamped independently of the result cap**, and the response must name which bound ended the walk (`stopReason: complete | limit | scanLimit`) so a short answer is never mistaken for an exhaustive one. A comment claiming a bound is not a bound: assert the walk length in a test that fails if it runs long.
- **An accepted-and-ignored parameter is a confirmation, and it is the worst shape in this section.** A wrong result can be caught by looking at the artifact. A parameter echoed back unchanged cannot: the response is the only channel the caller has, and a reply carrying `textureResolution: 2048` is byte-identical whether the value reached an engine call or was dropped on the floor. Four verbs shipped this at once — `quadrangulate` and `remesh_voxel` hardcoded a `TrisBefore/2` density and never read their size control, `pack_uv_islands` echoed a resolution no engine call received, `bridge` echoed a `subdivisions` it could not apply. **Note what did NOT catch them: the dispatcher's `UNKNOWN_PARAMS` gate.** That gate rejects a param the spec does not declare, which is the opposite failure; a param that IS declared and never read sails through it, and no other mechanism looks. So there are exactly three honest outcomes for a knob, and the choice must be deliberate: **implement** it, **remove it from the declared surface** so the existing gate refuses it in band, or **rename the verb** to what it does. Removing it is a real fix, not a cop-out — it converts a silent lie into an immediate error — but only removal does that; leaving it declared while documenting that it does nothing leaves the echo intact.
- **A verb that creates a RELATIONSHIP must name the relationship, and a parameter named like a reference must not silently produce a copy.** `niagara.add_emitter` takes an `emitterPath`, and `UNiagaraSystem::AddEmitterHandle` gives the system either a child of that asset (parent link kept, later edits mergeable) or a frozen photocopy of it (parent stripped, because the source asset declares itself non-inheritable) — with **nothing in the response distinguishing the two**. The field names steered the reader wrong on their own: a parameter called `emitterPath` echoed back verbatim reads as "the system now uses that asset". Three separate sessions wired emitters, kept editing them, and shipped systems running the pre-edit content; `compile` said `completed`, `asset.save` said `saved`, `validate level:"strict"` said `valid: true, errors: []`, and the on-disk `.uasset` grew on every re-save. The only disagreeing signal was a structural read nobody had a reason to make. Three rules follow. **Publish the relationship as a measured field** (`emitterSource: "inherited" | "snapshot"` plus the parent path, read off the created object's own parent pointer — never re-derived from the engine's rule, which is spelled differently on every engine version). **Make the editor's model the default and the lossy variant a named opt-in** (`inherit` defaults true; `inherit: false` is the editor's own *Remove Parent Emitter*), and **refuse rather than silently downgrade** when the default cannot be honoured. **And give the relationship a readback and a repair verb** — `niagara.inspect` reports `parent { inherited, path, synchronized }`, `niagara.validate` raises `EMITTER_PARENT_STALE`, `niagara.refresh_emitter` merges. The repair must be non-destructive where the lossy one is not: remove+add also picks up the parent's changes and throws away every edit made to the system's own copy, which left one reporter with no route at all mid-iteration.
- **Where the engine documents a precondition and does not check it, validate at the input — a post-condition cannot see the damage.** Geometry Script's capped extrude and sweep take any three-or-more points and cap by `TriangulateSimplePolygon`, whose ear clipper, finding no ear, "treats the current vertex as an ear" rather than failing. A self-intersecting cross-section therefore yields the full N-2 cap triangles, writes nothing to the debug channel, and returns success with a cap made of overlapping self-cancelling geometry. Every post-condition that suggests itself fails: the cap reuses the end-section vertices so the mesh is still topologically CLOSED and an open-boundary count reads clean, and an enclosed-volume check has no reference value to compare against. The input is the only place the defect is still visible. Corollary that cost two review rounds on that ticket: **validate the actual precondition, not a proxy for it.** The precondition is *simple*, not *convex* — the ear test handles concave ears deliberately — so a guard that refused non-convex cross-sections would reject valid work while still passing the self-intersecting ones, which is a worse contract than the one it replaced. Assert the accepting direction with known-good concave inputs, or the guard's own tests cannot tell the two apart.
- **An input verb must name its destination and verify delivery at the RECEIVER — ambient focus is not a destination.** `editor.simulate_input` key events called `FSlateApplication::ProcessKeyDownEvent`, threw the handled bool away, and answered `{success:true, message:"Key down: W"}`. Slate routes a key along the focus path of the keyboard user, so with the editor focused on any of its own panels the event never entered the running game: a reviewer held `W` and `LeftShift` on a possessed pawn across two sessions and measured zero velocity, an unchanged location, and a sprint bool that stays `false` — then could not tell a broken key binding in the game under test from a broken verb, because both answer identically. Three rules. **Resolve the destination explicitly** (the PIE world's `UGameViewportClient`, its local player and controller) and put it on the focus path before dispatching, rather than inheriting whatever holds focus. **Report who received it** — world, player controller, pawn, and which route carried the event — so the caller can see the destination was the one they meant. **And derive success from the receiver's own state, not from the injection call returning:** the delivery is confirmed by the target `UPlayerInput`'s queued-event count for that key moving, which is observable in the same call; `APlayerController::IsInputKeyDown` is not, since `bDown` is assigned during the next `ProcessInputStack` and reads `false` for one more frame. Generalises past input: whenever a verb hands work to a subsystem that routes by ambient state (focus, selection, "current" viewport or world), the ambient state is an input to resolve and report, never a destination to assume. Both the focused-Slate event and the direct `UGameViewportClient::InputKey` fallback carry the selected local player's input device; the fallback applies the same pre/post queued-edge check rather than becoming fire-and-forget success. These fields prove transport receipt, route handling, and PlayerInput edge registration separately; they do **not** prove that an Enhanced Input action or pawn callback executed, which requires a live game-side observation.

For game-targeted `editor.simulate_input`, the authoritative contract is stricter: inject through
the resolved PIE `UGameViewportClient`, retain the response across the following player-input tick,
and set `deliveredToGame:true` only when the exact queued event id appears in that same
`UPlayerInput`'s processed event counts with the requested pressed/released state. `handled` is only
the immediate viewport return. `consumingRoute` names `player_input` or the point before it where
delivery stopped; handled-but-not-delivered is `INPUT_FAILED`. Explicit game/world targeting with
no PIE returns `PIE_NOT_ACTIVE`. Slate handling, including a global preprocessor, can never satisfy
this game-delivery contract.

## 2. Prefer structural guarantees over discipline

Rules get forgotten; types do not. Whenever a rule in this document could instead be a compile error or an unrepresentable state, make it one. Worked examples from this codebase:

- **Change the return type.** `McpSafeAssetSave` was changed from `bool` to `void`. A whole family of "reported saved, wrote nothing" defects became one compile-error sweep, and the misuse became unwritable.
- **Make the parameter required and leading.** A parameter with no default argument makes a missed call site a compile error instead of a silent misbehaviour (`GroundPlacement::MeasureContact` takes its `FGroundSurfaceSpec` positionally, not as a defaulted tail argument).
- **Make success a conjunction of independently established facts.** `FGroundSeatResult::IsSeated()` is `AppliedTransform.IsSet() && Contact.bPass`. The `TOptional<FTransform>` is set on exactly one line, immediately after the only `SetActorTransform` call, and cleared again on revert — so a path that forgets to move an actor cannot report that it did.
- **Make the zero value a failure.** `EGroundSeatStatus::NotAttempted` is 0, so a default-constructed result is a failure. There is no default-constructible success.
- **One writer for pass/fail.** `GroundPlacement::EvaluateContact()` is the *only* code that assigns `bPass` / `FailReasonCode` / `FailReason`, and both the mutating verb (`spatial.ground_actors`) and the checking verb (`spatial.verify_grounding`) call it. They cannot disagree about what "seated" means.
- **Collapse the duplicated write sites first, then add the capability once.** Adding an optional parameter to a family whose write step is copy-pasted N times means either N branches or a capability that works on some siblings and silently not on others. Sequencer had six binding-creation sites, four of them byte-identical `AddPossessable` + `BindPossessableObject` pairs (a fifth and sixth were `AddSpawnable`, a genuinely different call). They were collapsed into one `SequencerBindingUtils::BindActor` **before** the component path was added beside it as `BindComponent`, so there is now one place a binding is minted and one place the verification lives. Note the other half of the rule: do not unify what only *looks* the same — the two `AddSpawnable` sites take an object template rather than a binding reference, and folding them in behind a flag would have bought a third code path inside the helper to save two lines at the call sites. Do not add the parameter to verbs where it has no meaning either (`add_camera` binds a camera it just spawned); a capability offered where it cannot apply is interface noise the caller has to test to disprove.

## 3. When there is no safe default, require the parameter

A convenient default that is wrong in the field is worse than a required argument.

`spatial.ground_actors` makes its `surface` argument mandatory because neither trace mode is safe on real content: with `traceComplex: false` a collision-boxed VFX card becomes "the ground"; with `traceComplex: true` a collisionless HISM foliage mesh blocks, because complex collision for a static mesh is its render triangle soup. Measured on this project: **268 of 930** downward `traceComplex` probes were blocked by foliage instead of the landscape.

Corollaries:

- An unrecognized preset name **errors** and leaves the spec untouched (`GroundPlacement::ParseSurfacePreset` returns false), so an unknown preset can never silently degrade into "trace everything".
- There is no "unset default that quietly means everything". If the caller must answer a question the verb cannot answer for them, ask it in the schema.
- **Scope every registry query whose result set you then MUTATE, and make the wide scope a named opt-in.** A query API's unset field means *unconstrained*, not *unspecified* — `FARFilter` with only `ClassPaths` set is "every asset of this class in every mounted content root", and a `packagePaths`/`classNames`/`tags` field left empty reads the same way. That is harmless for a read and unbounded for a write. `asset.bulk_delete` ran exactly that filter and handed the result to a fix-up that **deletes** what it collects: a call naming a handful of assets in one folder irreversibly deleted 4 unrelated host packages and rewrote 13 more, with nothing in the response saying so (`fixupScope`, board `B-tests-destroy-host-assets`, backlog D-72). Rules that fall out, each one a thing that went wrong:
  - **Derive the default scope from what the caller named** — the package folders of the assets in the request — rather than leaving it unset. The caller's own arguments are the only bound the verb can honestly infer.
  - **Choose recursion deliberately and write down why.** `bRecursivePaths: true` on the folder of one deleted asset re-widens to everything beneath it; for an asset at the top of a content root that is the project again, under a narrower-looking spelling.
  - **A scope that resolves to nothing must match nothing.** An empty `PackagePaths` array is "every path", so the narrowest possible request silently becomes the widest — the same defect through the door built to close it. Pin it to an impossible value and test that the empty case matches zero.
  - **Name the wide scope** (`fixupScope: "project"`) and reject anything unrecognised *before* the destructive step, so a typo costs the caller nothing.
  - **Report what was touched outside the request, always, empty included.** A count cannot say *which* packages went. `redirectorsDeletedOutsideScope: []` is the verb stating it checked; an omitted field is indistinguishable from not looking (§1).
  - **Put the scope decision in one shared helper** so sibling verbs cannot drift about what "inside the requested set" means — and use the same helper for the filter and for the collateral test, or the two will disagree (a string-prefix "inside" check folds `/Game/FooOther` into `/Game/Foo`; §2).
- An empty match set from a selector is an error (`NO_ACTORS_MATCHED`), not a zero-item success — a typo in a prefix otherwise looks identical to a clean run.
- **Do not write a per-verb `MissingRequiredParam` test.** Enforcement is not in the handler body — it is `FRpcDispatcher::ValidateHandlerParams`, and `PinWright.infra.contract.RequiredParamGate.EveryVerb` already drives it for every registration and every required `FParamSpec`, including verbs that do not exist yet. Declaring the param IS the coverage. 532 hand-written copies of that assertion existed across 26 files, and 481 of them called `Reg.Func(Ctx)` directly through `InvokeHandler`, bypassing the very gate they claimed to test — green with every `RPC_PARAM_REQ` deleted, with `bRequired` inverted, or with the body gutted to `return true;`. Copies also have to be *remembered* per verb, and were not: the walk that replaced them covers 958 verbs and 1664 required slots, roughly three times what the copies reached. Write a per-verb test only for a requirement the *body* enforces (e.g. "exactly one of `location` or `path`"), which the gate cannot see.
- **The declared `Type` is enforced too, and only in the lossy direction.** `ValidateHandlerParams` also reads `FParamSpec::Type` (and a typed alias's own `FParamAliasSpec::Type`) and refuses `PARAM_TYPE_MISMATCH` for the shapes UE's `FJsonValue` accessors would silently coerce to `""` / `0` / `false` with only an editor-only `LogJson` line: array/object where a scalar is declared, a non-numeric string where `number` is, a string `FCString::ToBool` reads as false where `boolean` is, a scalar where `array`/`object` is — **and JSON `null` for any declared type, `any` included**, because `FJsonObject::HasField` returns true for `EJson::Null` and a serializer that emits nulls for unset optionals otherwise turns "I did not set this" into "I explicitly set this to the destructive value". The lossless coercions are deliberately kept: `"limit": "100"`, `"force": "true"`, a number where `string` is declared. **One measured carve-out:** an `object` slot also accepts an array, because 205 vector-shaped slots across 36 handler files declare `object` while `ExtractVectorField` reads `{x,y,z}` *or* `[x,y,z]` — correct those declarations to `object|array` verb by verb before tightening that row. Consequences for a new verb: **declare the type honestly, because it is now a wire contract** — a slot that really accepts two shapes must say so (`array|string`), a mis-declared slot becomes a live rejection, and a verb no longer needs to hand-roll its own shape check (`system.run_tests`' `HasTypedField` guards, `asset.search`'s `classFilter` parse). Grammar and accepted-shape table: `Source/PinWright/Private/Handlers/ParamTypeCheck.h`. Board `B-param-type-never-validated`.
- **A key nested inside an object/array parameter is validated only if you declare it.** The first three passes of `ValidateHandlerParams` walk `Params->Values` one level deep, so until a parameter declares its nested schema, a key inside it is checked by **nothing** — 317 of 1,220 verbs carry that surface, and a caller sending a plausible-but-unread nested key got `success` with the input silently dropped (`material.authoring.create_material_instance` answered `applied:[]` *and* `failed:[]`; `render.capture_annotated` echoed back a `grid` it had ignored). Populate `FParamSpec::NestedKeys` — `RPC_PARAM_OPT_NESTED("grid", "object", "…", TEXT("spacing"), TEXT("extent"))` — and a fourth pass refuses `UNKNOWN_NESTED_PARAMS` for every other key, naming the offending dotted path and listing the accepted set. **Three rules on adoption, and each one is a way to get this wrong.** (1) **Empty means undeclared, not empty-set**, so the default is unchanged behaviour — refusing undeclared keys across all 317 would turn a documentation gap into a live rejection for a large fraction of real callers. (2) **Declare only what the verb actually READS**, because the allow-list is now the promise: a key listed but unread is an accepted-and-discarded input again, wearing a gate. (3) **Adopt only where the set is genuinely closed, and update the description in the same commit** — closing an object is a compatibility break, and a map keyed by caller-chosen names (`set_material_instance_parameters`' `scalar`/`vector` maps) has no closed set to declare and must never adopt. One level only: the level below a declared one has its own schema and inventing it is how a gate starts refusing shapes nobody wrote down. Mechanism: `Source/PinWright/Private/Handlers/NestedParamKeyCheck.h`; the adopter list is ratcheted by `PinWright.infra.dispatcher.NestedParamKeyGate.AdoptionSetIsRatcheted`. Board `B-declared-param-guard-blind-to-nested-keys`.
- **Keep deep schema validation explicit.** Recursive document parsers and per-verb nested adapters use the shared `RejectUnknownKeys` collector in `Utils/JsonUtils.h`, choosing first-error or sorted-all-error output so their established path and message contracts stay intact: image parsers keep case-sensitive `AllSorted` reporting, while music and synth parsers keep case-insensitive `First` reporting and attach the first miss to its recursive field path. This is separate from `FParamSpec::NestedKeys`, which deliberately checks one level only. `NestedKeysMatchTheirParameterDescriptions` compares every production adopter against the immediate keys in the first documented outer `{...}` schema, case-insensitively and in both directions; it does not infer schemas for non-adopters. `DeclaredParamsHaveSourceUse` covers the cheap inverse top-level direction by blanking complete `RPC_PARAM_*` declarations before collecting production literals, with the deliberately unreachable `gas.set_ability_input:abilitySetPath` as its single ratcheted exception.

## 4. Verification must measure something the write path cannot fake

Repeatedly here, the tool and its own check agreed with each other and disagreed with reality:

- Skin weights written to an alternate profile and read back from that same profile.
- A save "verified" by an existence check that was structurally blind to the failure the write path produces: the `.uasset` from the *previous* save satisfies a bare existence probe, so a throttle-skipped save was indistinguishable from a real one. The fix is a freshness gate — capture timestamp/size and the package dirty flag *before* the save (`SaveAssetToDiskReportingPresence`).
- A position check that opened an animation sequence before measuring, so it could only observe the state the fix controlled.

Rules:

- The check should run through a different subsystem, or a re-probe of the world, or a reload — not a readback of the value the writer just set.
- Re-measure **after** the mutation and derive the verdict from the second measurement (`SeatActor` measures, solves, moves, then measures again and evaluates *that*).
- Carry a readback tolerance where you can: `MaxSeatErrorCm` fails when the actor did not actually move, when the world answer was not reproducible, and when something else shifted underneath.
- Report what the check could not see. A fallback path that silently changes what the numbers mean must surface a flag (`bUsedBoundsPlaneFallback`), and an unmeasured report must omit its numbers rather than emit zeros that read as measurements (`FGroundContactReport::bMeasured`).
- **A verb that creates a node is verified on the node's class, not on "something was created".** `configure_layer_blend` created one `UMaterialExpressionScalarParameter` per requested layer and answered `layerCount: N` with N `nodeIds`. Every step succeeded, every count matched the request, and the material declared **zero** target layers — the engine harvests them by calling `GetLandscapeLayerNames` on each expression, which `ScalarParameter` does not implement. A response that echoes the request back cannot fail. Assert the produced type, and report the count the *consumer* reads: this verb now returns `targetLayers[]` from `UE::Landscape::RetrieveTargetLayerNamesFromMaterial`, the same accessor `ALandscapeProxy::RetrieveTargetLayerNamesFromMaterials` harvests through, so a node that exists but declares nothing reports empty.
- **State handed to an engine object must be read back OFF THAT OBJECT, because that is what the engine will read.** `render.capture_ortho_tiles` moves one scene-capture component per tile and renders. The renderer takes the pose from `CaptureComponent->GetComponentToWorld()` (`SceneCaptureRendering.cpp:1180`) - not from the request - so a move that failed would render every tile of a 64-tile burst from one place while every tile still wrote a file, still reported its own world extent and still timed itself. Nothing downstream could tell that from a real mosaic. The verb therefore reads the component's location and orientation back before rendering and refuses (`CAPTURE_CAMERA_NOT_APPLIED`) when they disagree with the plan. Two details worth carrying: **compare the invariant the engine uses, not the representation you sent** - the first version compared `FRotator` component-wise and failed on every top-down capture, because at pitch ±90 the rotator is gimbal-locked and the engine stores a requested `(P=-90, Y=0, R=-90)` back as `(P=-90, Y=-90, R=0)`, the same orientation; comparing quaternion angular distance is both correct and immune to that. And **the check earns its place even though the pose turned out to be fine** - it is what converted "the frames are black, and I have no idea which of five suspects it is" into a one-line answer.
- **An all-zero test is not an emptiness test.** `IsBlankReadback` requires all four channels of every pixel to be exactly 0, which is the right test for "the surface was never drawn". It says nothing about a frame the renderer *did* draw, over nothing: tonemapper dither over black scene colour is not all-zero. A 16-tile burst that was visually black reported `blank: false` for all 16. Publish a magnitude beside the boolean - mean/min/max luminance per item and for the batch - and let the reader judge; the two fields answer different questions and only the second one catches a frame drawn over nothing. Reuse the classifier the sibling verbs already publish (`CalculateCaptureImageStats`) rather than adding a second idea of "empty".
- **A threshold over sampled data must be invariant to the sampling resolution, or it is a statement about the sampling and not about the content.** The same classifier's `bBlank` was `mean <= 0.01 && variance <= 0.0001`, and *variance measures the share of the frame that carries content*. A fixed-pixel-size editor overlay (world-axis gizmo, stats text) is a smaller share of a bigger frame, so one camera in empty space scored `blank:false` at 256 and 512 and `blank:true` at 1024 and 2048 — and `render.capture_open_level` rejects a blank capture, so raising `width` to get more detail converted a working call into a hard `BLANK_CAPTURE`. The trap is that the criterion looks scale-free: mean is, variance is not. Two rules come out of it. **Find which statistic is stable in each regime the input has, and admit each as evidence** — real scene content scales with the frame and keeps its lit *fraction*; a fixed-pixel overlay does not and keeps its lit *count*; a criterion built on either alone flips on the other, and one built on both flips on neither. And **prove the invariance in the test rather than reasoning it**: the fixture set carries both regimes at five sizes, and asserts as a separate check that the *previous* criterion would have flipped on at least one of them, because a test whose fixtures never flip under the old rule cannot tell the fix from the bug.
- **When a write creates a relationship, the relationship is the thing to verify — and the verb must refuse without it, not warn.** A Sequencer component binding is a possessable whose *parent* is the owning actor's binding. That link is not metadata: `MovieSceneHelpers::GetResolutionContext` (`MovieSceneCommonHelpers.cpp:1272-1297`) substitutes the resolved parent object as the locator's resolution context **only** when `GetParent().IsValid() && AreParentContextsSignificant()`, so a component possessable without it resolves against the world, finds nothing, and every track keyed to it drives nothing — with the GUID valid, the track present, `get_bindings` listing it, and no error anywhere. The interface consequences: (1) the parent is **read back off the sequence** and published beside the child GUID, never echoed from what the create call was asked to do; (2) the new GUID is resolved through the *runtime* resolution path (`MovieSceneHelpers::GetBoundObjects` over a transient shared playback state) and compared against the object the caller named; (3) failing either is an error, not a warning, and the half-made binding — including a parent binding the engine minted on the way — is removed, because a binding that resolves to nothing is exactly the placeholder a later `add_transform_track` will happily key onto. **A readback that only asks "does the row exist" cannot see this class of defect at all**; the question has to be "does it resolve, and to what".
- **Reach for the engine's composite operation before hand-rolling its steps, and let access modifiers be evidence rather than obstacles.** The obvious component implementation mirrors the actor path — `AddPossessable` then `BindPossessableObject(Guid, Component, EditorWorld)` — and produces a binding that passes every readback and animates nothing, because it skips `SetParent` and binds against the wrong context. `ULevelSequence::FindOrAddBinding` (`LevelSequence.cpp:865-941`) already does the whole path, including reusing an existing actor binding. It is `protected`, which reads as a closed door and is not one: it is reached through the public base declaration `UMovieSceneSequence::CreatePossessable`, since C++ checks member access against the *static* type of the call expression. Before concluding an engine operation is unreachable headlessly, check whether a public base declaration already dispatches to it — the alternative here was re-deriving four engine steps, one of which is invisible when omitted.
- **A comparison over rendered pixels takes a tolerance and publishes a magnitude — never an equality check.** Pinning exposure removes the drift between two captures; it does not make them byte-stable. Two back-to-back `render.capture_asset_preview` shots at the same `ev100`, on a preview scene with nothing animating, differ in **11676 of 16384 px, mean absolute difference 0.88/255, max 34, best-fit gain 1.0000** (measured 2026-08-18). An `identical: true` over a stochastic measurement is a field that reads false on correct output, and a differing-pixel *count* is no better at ~71% churn from noise alone. The exposure-pin test (then named `render.capture_asset_preview.PinnedCapturesAreIdentical`, now `…PinnedCapturesReproduceWithinTolerance`) asserted byte identity and passed only because both its frames were black — the same defect from the other side, a threshold with neither end measured. So: the verb reports mean/max absolute difference and takes the threshold as a parameter, and whoever sets a threshold measures **both** ends — the noise floor and a known-real change — because one measured side cannot show that it discriminates. On that fixture the real change (six stops of EV100) scores 89.6, ~100× the floor. Measure both ends **in the environment the assertion will run in**: the same scene captured headless (`-unattended`, how the suite runs) is several times dimmer and scores 23.1 for that same change, so a bar set at 32 from the interactive signal was green interactively and red in CI — a threshold is only calibrated against the environment it was measured in.
- **An "in-place update" that reconstructs the object is a full property reset, and a contents-only verification is blind to it.** `audio.synth.export` routed its rewrite through `FSoundWavePCMWriter::SynchronouslyWriteSoundWave`, whose asset path is `NewObject<USoundWave>` with the same name (`SampleBufferIO.cpp:369`) - which re-runs the constructor over the existing allocation, so every `UPROPERTY` returns to its CDO default. The audio was byte-correct and the decode-back comparison passed, so the verb answered `verification.pass:true` while the wave lost its `SoundClassObject` and `AttenuationSettings` - an asset that escapes every SoundMix, duck and class volume and plays at full level at any distance, with nothing in the editor flagging it. Four encounters shipped ~30 unrouted waves before a caller diffed the `.uasset` bytes; every response along the way said `pass:true, saved:true`. Three rules come out of it. **Write onto the existing object instead of re-creating it** - preservation by construction beats a copy-back list that is one engine version away from being incomplete, and the engine's own reimport-over-an-existing-asset path (`SoundFactory.cpp:657-763`, the `bUseExistingSettings` branch) is the model for which fields a payload rewrite legitimately owns. **Name that field set once and use it twice** - as the write's assignment list and as the verification's exclusion list - so a field the write forgets to declare fails the verb loudly rather than vanishing quietly. And **verify the whole object, not the payload**: the check here snapshots every non-payload property as exported text before the write and diffs after, which is generic enough to catch the next field nobody thought to name, and its verdict is folded into `verification.pass` so the loss is an error rather than a footnote.

- **A convergence check on the frame mean cannot see a local cycle — test the subject region, or repeat the pose and diff.** The preview-capture settle loop reports `warmup.settled: true, settleRounds: 1` off a whole-frame mean-luminance delta. Measured 2026-09-02 on a stationary camera: twelve captures of the *same* pose returned a period-4 lighting cycle of ±14/255 on the subject's shadowed face, while whole-frame mean luminance moved **0.001** across that same set — so every frame was certified converged and no two were the same picture. A statistic aggregated over the frame is dominated by the part of the frame nothing is happening in. Where a verb offers a convergence verdict, compute it over the region the caller named (the subject, the measured bounds) as well as the frame, and publish the cheapest falsifier there is: **shoot the same pose twice and difference them.** A settle test that cannot fail on a repeated pose is not measuring settling. `B-preview-capture-lighting-cycles-per-shot`.
- **A report of the rig applied is not a report of the rig in the pixels.** Preview-scene captures read their lighting rig back off the live scene and publish it, which is the right shape — and it is still not evidence about the frame, because no editor tick runs inside the capture call, so work the engine defers (a sky-light capture update) never executes while the response is being assembled. Measured: a freshly opened preview window renders near-black (mean 0.591, min 0.004); ten seconds later the same call, same pose, renders the converged ambient (mean 0.686, min 0.117) — and the `previewScene` block is **byte-identical** across that change. Two rules follow. A block describing state the verb applied must not be readable as a description of the output; name what it is measured off (the scene) and, where the gap is real, publish a field that distinguishes applied from *in effect*. And a verb that depends on deferred engine work must either drive that work or report that it could not — a settle loop whose pump cannot run the thing it is waiting for certifies the wait instead of the result. `B-preview-rig-first-capture-stale-sky`.

## 5. Persistence has three levels — only the third is proof

1. **Readback** — the value is in memory. Proves nothing about durability.
2. **Save reported success** — still not proof. A bare transform set from Python does not dirty the package at all, so the save writes nothing.
3. **A genuine reload** — the only proof. A value has survived read-back *and* save and still reverted on load, because the engine re-derives it from spline metadata.

Note that `open_level` on an already-open map does not reload; it will not catch this.

Wire contract, so callers can tell the levels apart:

- `McpSafeAssetSave` only marks dirty (the immediate write is the documented bulkdata-corruption vector on UE 5.7+). Report it with `AddMarkDirtySaveReport`, which measures via `IsAssetPersistedToDisk` — normally `saved:false, pendingFlush:true`, which is the truth the old constant hid.
- Flows that promise disk persistence use `SaveAssetToDiskReportingPresence` + `AddAssetSaveReport`.
- `AddAssetVerification` emits the measured triple `existsAfter` / `existsOnDisk` / `pendingSave`. Keep `{saveRequested, saved, pendingFlush}` identical across both save families so a caller never has to know which one ran.
- **A `saved:false` that does not say WHICH kind of false it is loses work.** `pendingFlush` marks "requested and not durable" and is emitted for three unrelated situations with three different remedies: the edit is deferred (flush it), the write failed (flushing will not help), the package can never be written (nothing will). `SaveAssetToDiskReportingPresence` had already measured the difference — it captures file size, timestamp and the pre-save dirty flag before the save — and threw it away by returning a `bool`; `model.compile` then reported an intermittent, unexplained `saved:false` on a path where the throttle was bypassed entirely, so the obvious suspect was also the wrong one. Pass `EAssetSaveState` (`Utils/AssetSaveState.h`) out of the save helper and into `AddAssetSaveReport`, which adds `saveState` + `saveDetail` without touching the three existing fields. Two rules generalise: **a measurement that already exists must reach the wire rather than being collapsed at the last step**, and **a not-durable report must name the remedy, because "not saved" and "not saved, and retrying will not help" are different instructions.** The state table is documented once, in `docs/wiki-src/safe-mutation-save.md`.
- **Log the values the verdict was computed from, not the adjective.** The freshness check's warning said "stamp+size unchanged" and printed neither, so a false negative (the file did move; the probe did not see it) was indistinguishable from a genuinely failed write, and the next occurrence was as undiagnosable as the last. Print every probe — engine outcome, size before/after, timestamp before/after, pre-save dirty flag, resolved filename — on every not-durable branch.

### 5a. A write to a value the engine re-derives is doomed, and only a reload can see it

The third level exists because of writes like this, so name the shape: **the caller asked for property A, but the engine recomputes A from authoritative state B.** The write lands, reads back at the requested number, saves into the package, and is recomputed away in `PostLoad`. Every check short of a genuine reload scores it as a success — and `open_level` on the already-open map does not reload.

Shipped instance: `spline.set_spline_point_scale` on a UE Water spline. `UWaterSplineComponent::SynchronizeWaterProperties` assigns `Scale.X` from `UWaterSplineMetadata::RiverWidth` and `Scale.Y` from `Depth` (UE 5.8 `WaterSplineComponent.cpp:231-246`), and `PostLoad` calls it (`:26-39`), as do `PostDuplicate` and every `PostEditChangeProperty`. A river width was set to 3490.6, read back, saved and committed — and was 4800 on the next load.

Rules:

- **Ask the engine, do not maintain a type list.** The engine usually declares the fact itself and the declaration is the right gate: `USplineComponent::AllowsSplinePointScaleEditing()` defaults to true and `UWaterSplineComponent` overrides it to false precisely because its scale is derived (`SplineComponent.h:425`, `WaterSplineComponent.h:54`). Gating on the predicate covers a spline type that starts deriving its scale in a later engine version, with no code change. Other engine tells: `UCLASS(HideFunctions = (SetFieldOfView))`, a `BlueprintSetter` on the property, a `PostEditChangeProperty` that logs "changing X directly is unsupported".
- **Refuse rather than forward when forwarding is only right for some inputs.** Forwarding a water `Scale.X` write to `SetRiverWidthAtSplineInputKey` is exact for rivers, but `RiverWidth` is editable on rivers only while `Scale.X` is synchronized from it on *every* water body type — a forwarded lake write would store a value that controls nothing and report it as a width change.
- **A refusal must name a verb that exists.** If the authoritative setter has no verb, ship it in the same change; an unreachable remedy is a dead end with extra steps (§7).
- **Emit the reason as structure, not prose.** `PinWright::DerivedState::AddDerivedWriteReport` puts `derivedWrite {property, derivedFrom, authoritativeVerb, survivesReload:false, explanation}` in the error payload, which is what survives the oversize-spill rewrite.
- **The authoritative verb then measures the derived value.** `water.set_river_width_at_spline_point` writes the metadata, synchronizes, and reports per point both the `stored` metadata value and the `derivedScale` the engine computed. The handler never assigns `derivedScale`, so it is the half of the check the write path cannot fake (§4).

The generalisation is worth sweeping for: anything the engine recomputes from other state on load is a candidate. Confirmed siblings found by that sweep and not yet fixed — `navigation.set_nav_agent_properties` writing `ARecastNavMesh::AgentRadius`, which `SetConfig` restores from `NavDataConfig` at every registration; `misc.set_camera_fov` on a CineCamera, whose `RecalcDerivedData` overwrites `FieldOfView` from the clamped focal length in `PostLoad`.

### 5b. A durable edit can still reach nothing: report consumer coverage, measured

The mirror image, and it wears the same disguise. The asset edit *is* durable — but consumers that cache data derived from it were never told, so a downstream measurement scores the edit as a no-op while every verb reports success.

`material.authoring.compile_material` did `PreEditChange(nullptr)` + `PostEditChange()` and stopped. Two caches survive that, both silently: `UMaterial::PostEditChangePropertyInternal` creates no `FMaterialUpdateContext` (`Material.cpp:5350`), and that destructor is the only code that recaches dependent material instances' static permutations (`MaterialShared.cpp:5049`); and `ALandscapeProxy::MaterialInstanceConstantMap` caches combination materials no update context can see. Measured: forcing a lane gate to 0 in a terrain master — which must erase the whole painted road network — moved a fixed frame **1.81%**, the noise floor; the same edit after a material round-trip moved it **17.19%**.

- **Do the refresh if the API is reachable; report it if it is not.** `ALandscapeProxy::UpdateAllComponentMaterialInstances` is `LANDSCAPE_API`, so `compile_material` calls it rather than teaching callers a round-trip.
- **Do not assume a fix generalises across triggers.** Commit `11fe111a` made `landscape.set_material` rebuild the MICs by naming `LandscapeMaterial` in a real `FPropertyChangedEvent`. That mechanism is triggered by an *assignment*; a graph edit to the master performs none, so it never fired. The effect transferred, the trigger did not — check which half of a prior fix you are reusing.
- **Coverage is a measurement, not a count of calls issued.** `FConsumerRefreshReport` counts landscapes whose component MICs changed *object identity* before vs. after, because `UpdateMaterialInstances_Internal` allocates a fresh `ULandscapeMaterialInstanceConstant` per component per rebuild (`LandscapeEdit.cpp:715`). An unchanged pointer set proves the rebuild did not run.
- **`complete` is the conjunction of "we looked" and "we got them all."** `bMeasured` defaults false and the counters default 0, so a path that never enumerated consumers cannot report clean coverage. Keep "rebuilt" and "had nothing to rebuild" as separate counters — collapsing them lets an empty consumer score as a refreshed one.
- **A partial refresh is not a plain success.** When coverage is incomplete the verb emits a `warnings[]` entry naming what was missed and the remedy, beside the still-true `compileSucceeded`.
- **Fixing one entry point is not fixing the defect — enumerate every door into the same state.** The `compile_material` fix above was recorded as closing this bug. It closed one of the ways to reach it. `material.compile_mgir` — the bulk path the wiki *recommends over* the imperative one — reached the same master through its own `FinalizeMaterial` and did strictly less than the pre-fix `compile_material` had: a bare `PreEditChange(nullptr)` + `PostEditChange()` + `ForceRecompileForRendering()`, no update context, no landscape rebuild, and then it **saved the asset**, so the durable artifact was correct and the screen was not. Measured on the same terrain and camera: the identical MGIR document moved the frame **0.79%** before the fix and **93.70%** after. Before calling a defect of this class closed, grep for every call site that reaches the same engine state — here, every `PostEditChange()` on a `UMaterial` — and account for each one as fixed, deliberately-not-fixed, or unreachable.
- **When "fix it everywhere" would be pathological, draw the line on a principle and publish it.** A full landscape component-MIC rebuild costs 64 fresh `UObject`s plus permutation jobs, so firing it inside every incremental `add_*`/`connect_nodes` call would make batch authoring unusable. The line drawn: **verbs that complete a unit of work push and report `consumerRefresh`; verbs that are one step inside a unit of work do not** — and the namespace page names both lists, so the caller learns the rule instead of discovering it from a render that did not change.
- **A frame-delta control must be immune to auto-exposure, or it is measuring the camera.** Eye adaptation is a scalar gain on the whole frame, so a *luminance-shaped* control ("force the gate to 0", "make it darker") can be silently compensated away — a sibling workstream nearly filed a false plugin bug on exactly that. Two defences, both cheap: pick a control that changes **hue** (wire pure magenta into `BaseColor`) and measure **per channel**, because no scalar gain maps magenta to terrain colours; and fit the least-squares best-fit gain between the two frames and divide it out — a difference that is only exposure collapses to zero, while a real content change *survives* and typically grows (93.82% → 99.44% here, because no single gain can reconcile the frames). Report the gain alongside the delta: a no-op pair whose best-fit gain is ~1.000 proves the exposure never moved, so it had nothing to hide.
- **Give the pairing one name so the next verb cannot forget half of it.** `PinWright::MaterialConsumers::ApplyMasterMaterialEdit` is the update-context-scoped `PreEditChange`/`PostEditChange` *and* the measured landscape rebuild. A verb that completes a master edit calls that, never the bare `PostEditChange()`; the bare pair is what shipped this defect twice.
- **The class recurs on every private cache, so enumerate the caches rather than fixing the verb you were shown.** Third occurrence, on the landscape *grass instance* cache: `ALandscapeProxy::FoliageCache.CachedGrassComps` is keyed on the grass type pointer and the variety count and on nothing inside an `FGrassVariety`, so a `GrassDensity`/`GrassMesh` edit leaves every key equal to itself. Measured at one fixed pose: `meanLuminance` 0.4444 → 0.4439 (the noise floor) for the edit, → 0.4287 after a flush. No material fix touched it — a different cache reached by a different verb family. `PinWright::GrassConsumers::ApplyGrassTypeEdit` is the named pairing, and the seam it hangs off is the generic reflected mutators' change notification, because that is where the edit actually happens. **An engine version can also take the compensation away underneath you:** UE 5.3's `ULandscapeGrassType::PostEditChangeProperty` flushed the consumers itself and 5.4 stopped, so a verb that was correct on one engine became a silent no-op on the next with no source change of its own.
- **An invalidation and a destruction empty the same cache, so a test that asserts the cache is empty proves neither — assert what SURVIVED.** The grass fix above shipped destructive (commit `d8f1bc32`) and its own acceptance tests could not see it. `PinWright::GrassConsumers::RefreshGrassConsumers` was modelled on the console command with the measured evidence behind it, `grass.FlushCache`, and copied its *arguments* along with its mechanism: that command takes `ALandscapeProxy::FlushGrassComponents`' defaulted `bFlushGrassMaps=true`, which on top of dropping the HISM clusters calls `ULandscapeComponent::RemoveGrassMap()` on every component (5.8 `LandscapeGrass.cpp:2726-2736` → `:1233-1239`, one statement installing a freshly allocated empty `FLandscapeComponentGrassData`). The instance invalidation the fix actually needed is the branch's **unconditional** half (`FoliageCache.ClearCache()` at `:2695`), so the destructive argument bought nothing and cost the per-component density maps — data rebuilt only by the editor's amortised, camera-driven grass-map builder, and serialised into the landscape package (`Landscape.cpp:1114`), so a save while they are empty makes the loss durable. Measured live: 136 grass components → **0**, unrecovered by camera moves, by `grass.Enable 0/1` or over minutes. Four rules. **Read the flag the engine passes at the equivalent moment, not the one the debug command passes** — when UE's own grass-map builder detects a changed grass type it comments *"this invalidates foliage instances but not the grass maps"* and routes through `ULandscapeSubsystem::RemoveGrassInstances`, which spells out `/*bFlushGrassMaps = */false` (`LandscapeGrassMapsBuilder.cpp:500/504/579`, `LandscapeSubsystem.cpp:609`); a console command exists to debug, and its defaults are chosen for a bigger hammer than a verb wants. **A count of what went away is not coverage** — the response published `subObjectsRefreshed: 136`, which was a count of destroyed clusters presented as a measure of refresh. **Publish a survival measurement beside the invalidation measurement**, taken on both sides inside the same call (`grassMaps.componentsHoldingMapsBefore/After`), and emit the failure field only when it is non-zero so its presence is the signal. **And when a refresh hangs off a generic seam, its blast radius is every verb through that seam** — this one sat on the reflected mutators' change notification, so the destruction moved from a console string a caller opted into onto every `property.set` / `property.reset` / `container.*` write to a grass type, with no opt-in anywhere.

### 5c. A reflection write to a property the object shadows is corruption, not a write

The nearest consumer of a property can be the object itself. Three UE 5.8 properties are mirrored into a private cached copy that only the engine's own setter updates, and the engine `ensure`s the moment the two diverge: `UStaticMeshComponent::StaticMesh`/`KnownStaticMesh` (`StaticMeshComponent.cpp:746`), `USkinnedMeshComponent::SkinnedAsset`/`KnownSkinnedAsset` (`SkinnedMeshComponent.cpp:6052`), `UTexture::CompositeTexture`/`KnownCompositeTexture` (`Texture.cpp:1235`). A generic `FProperty` store sets one half. The component then renders, streams and collides as the *old* asset while the package serializes the new one — and `actor.set_component_properties` returned `applied:["StaticMesh"]` over exactly that state on a live foliage HISM.

- **Enumerate the class from the engine, not from the bug report.** `rg "without a call to Notify"` over `Engine/Source` returns the complete list in seconds; the report named one. Any generic property-writing verb inherits all of it.
- **Route to the typed setter, not to `PostEditChangeProperty`.** The setter is the superset (`SetStaticMesh` also does PSO precache, physics state, streaming, bounds, navigation) and cannot be mis-shaped. Synthesising the *chain* form is worse than useless here: `UInstancedStaticMeshComponent::PostEditChangeChainProperty` dereferences `PropertyChain.GetActiveMemberNode()` with no null check (`InstancedStaticMesh.cpp:5638`), so a hand-built empty chain crashes the editor.
- **Then verify by reading the component back (§4).** `SetStaticMesh` returns `false` both for "already this mesh" and for "refused"; only the component's state separates them.
- **Decide "clear" off the request, not off the stored value.** `FObjectProperty` performs no class check of its own, so a wrong-class path either stores a mismatched pointer or lands as null depending on configuration — and null read back as "the caller wanted it empty" silently *erases* the mesh. Only the documented `""`/`"None"`/`"null"`/JSON-null sentinels may clear.
- **Detection is component-type-dependent; corruption is not.** The ensure fires immediately on an ISM/HISM because `UInstancedStaticMeshComponent::UpdateBounds` reads through `GetStaticMesh()` (`InstancedStaticMesh.cpp:3146`). The identical raw write to a plain `UStaticMeshComponent` was silent in the same session. Do not scope a fix of this class by where the log line appeared.

### 5d. A reflection store is not a write — the property's effect usually lives in the notification

§5c is the special case where the object shadows the property. This is the general one, and it is much larger: for a whole class of engine properties **the field is not where the behaviour is**. `UWaterBodyComponent::WaterMaterial` is inert until `PostEditChangeProperty` reaches `UpdateMaterialInstances` → `CreateOrUpdateWaterMID` (`WaterBodyComponent.cpp:1368` → `:1252` → `:1019` → `:1057`). `UPrimitiveComponent::LDMaxDrawDistance` is inert until the same hook copies it into `CachedMaxDrawDistance`, the value the renderer culls on (`PrimitiveComponent.cpp:1552-1555`, `:1628`). A raw `FProperty` store runs none of it, and the failure is perfectly disguised: the write lands, the read-back is exact, `MarkPackageDirty` records a change, and nothing moves. Measured: a 2x water `Scattering` change pushed through `actor.set_component_properties` rendered **pixel-identical** (mean luma 0.4493 → 0.4493); the same change through Python `set_editor_property` — which notifies via `FPropertyAccessUtil` (`PropertyAccessUtil.cpp:599`, `:795`) — rebuilt the MID and moved it to 0.4743.

- **A generic writer that takes `void*` cannot notify, so the call site must.** `ApplyJsonValueToProperty(void* TargetContainer, …)` (`PropertyImport.h:10`) never holds the `UObject`. That signature is why ~20 verbs across the tree store without notifying — the omission is structural, not an oversight at any one site.
- **Fire the non-chain form, and only the non-chain form.** `FPropertyChangedEvent(Property, EPropertyChangeType::ValueSet)` sets `MemberProperty = Property` (`UnrealType.h:6977-6985`), which is the shape every name-matched engine branch reads. The chain form is actively unsafe to synthesize (§5c). `PinWright::NotifyPropertyChanged` (`Utils/PropertyChangeNotify.h`) is that one call; use it rather than open-coding a fifth copy.
- **A bare `PostEditChange()` is not the notification.** It builds an *empty* event, so `MemberPropertyName == NAME_None` and every `GET_MEMBER_NAME_CHECKED` branch is skipped — for *any* assignment. That is the same defect §5b shipped twice on landscapes, and it is what `property.set` and the whole `container.*` family still do (`UtilityPropertyHandler.cpp:1129` and siblings).
- **Notify without `PreEditChange`, deliberately.** `UActorComponent::PreEditChange` unregisters the component and calls `FlushRenderingCommands` (`ActorComponent.cpp:1327`, `:1336-1339`) — unacceptable in loop-driven verbs, which is why `EnvironmentDirtyUtils.h:14-16` rejected the pair. It is also the *only* thing that populates `EditReregisterContexts`, and that is the sole trigger for `ConsolidatedPostEditChange` rerunning the owner's construction scripts (`:1437-1446`). Skipping it therefore buys the notification with no flush, no re-registration, and no chance of the target being destroyed by its own notification.
- **Report `notified` beside `applied`, because they are different facts.** "The value is in the field" and "the class's change hook ran" fail independently, and only the second one moves a derived-state property. Engine-setter paths appear in `applied` only — the setter is the superset.
- **Prove it on the cheapest member of the class, not on the one from the bug report.** The water repro needs the Water plugin, a map and a camera. `CachedMaxDrawDistance == LDMaxDrawDistance` after the verb is the same assertion, RHI-free and deterministic, because nothing else in the engine copies one to the other on a property write (`Tests/Actor/TestSetComponentPropertiesNotifies.cpp`).

## 6. Measure both failure directions, and measure assemblies

- **A one-sided measure hides half the failures.** A ground check that measured clearance upward scored a sunken object identically to a perfectly seated one; 620 buried actors went unnoticed. Use a signed measure or paired bounds — `maxGapCm` *and* `penetrationCm`, from the same probe.
- **Per-item checks miss assemblies.** Items resting on each other each pass individually while the whole cluster floats. If a verb can be asked about a set, it should be able to answer about the set.
- **Sample the shape, not a point.** A pivot is not the mesh's lowest point and the lowest point is not a plane; a single ray at the pivot cannot tell a slope from a step. The grid exists for that reason, and `coverage` exists to say when half the footprint is over nothing.
- **A warning threshold needs a NEGATIVE calibration case, and the metric must read zero on the verb's intended input.** The obvious metric for `geometry.spherify`'s new hazard warning was the radial scale spread it produces — it separated the two reported failures (1.99, 1.84) from the reported clean case (1.58) cleanly. It is wrong: a *perfect cube* at `factor=1.0`, which is the op's whole purpose, scores √3 = 1.73 on it, so the warning would have shouted loudest at correct usage. The shipped metric, `factor · (Emax/Emin − 1)`, is identically **0** for a cube at every factor and for every mesh at factor 0, and it is aspect-monotone. Pick the metric by asking what it reads on the input the verb exists to handle, not only by whether it separates the cases someone happened to report. Then write the negative case into a test — that test, not the positive one, is what makes the threshold mean anything.
- **Compute a threshold from geometry, not from the vertices you happened to visit.** The same metric sampled over vertices flips on segment *parity*: 5 vertices per box edge puts one exactly at the face centre where the extreme scale falls (measured 1.5170, matching the closed form), 6 does not (1.4149). A vertex-sampled threshold would therefore fire or not fire on an even/odd change that alters no shape. Drive the threshold from the closed form and *report* the measured range alongside it.

## 7. Errors carry recovery, and every code is registered

- **Name the way out.** The good pattern already here: rejecting an unknown argument field names the offending field(s), the valid fields, *and* the wiki root index path. Every error result whose method is known also carries a `Docs:` line plus a top-level `docs: {page, wiki}` field pointing at the exact method page.
- **Put recovery in the structured payload.** Discovered in testing: when a result sets `structuredContent`, text blocks may not be forwarded to the client — anything that exists only as prose can be lost. The structured `docs` field survives the oversize-spill rewrite for exactly this reason.
- **Register the code.** A raw `SendError(TEXT("..."))` literal with no matching `ERR_*` constant in `Handlers/ErrorCodes.h` fails `PinWright.core.error_codes.AllEmittedCodesAreRegistered`. That exact mistake has cost suite runs. Declare the constant first, then reference it.
- Prefer an existing spelling over a new synonym — the header already carries semantic duplicates (`INVALID_ARGUMENT` / `INVALID_PARAMS` / `INVALID_PARAM` / …) that cannot be collapsed without a breaking change.
- **A code handed to a shared resolver as a non-first argument is invisible to the registry scan.** `TestErrorCodeRegistry`'s four emission patterns all anchor on the code literal sitting immediately inside the call parens (`SendError(TEXT("X")`, `MakeError(TEXT("X"),`), so `ResolveExpressionOrSendError(Ctx, Resolution, Id, TEXT("NOT_FOUND"), Msg)` matches none of them and that emission drops out of `AllEmittedCodesAreRegistered` coverage — silently, and in the safe direction only as long as the code is registered anyway. Nothing fails today; know that moving a literal into a helper's later parameter removes it from the scan rather than converting it. **The file-level rule still bites:** the moment a file cites `ErrorCodes::ERR_` even once, `RegistryAdoptingFilesUseConstantsOnly` holds *every* other code in that file to the constant form. A shared header that gains one constant reference must convert all of its literals in the same change.
- Distinguish opposite failures with distinct codes when a caller would act differently: `ACTOR_BURIED` vs `ACTOR_NOT_GROUNDED` means nobody has to parse a message.
- **Zero is not a small number — order the checks so it cannot be reported as one.** `verify_grounding` shipped its threshold tests in declaration order, so an actor touching *nothing* fell into the "fewer contact points than required" branch and came back `INSUFFICIENT_GROUND_CONTACT` ("balanced on too few points") when it was floating 150 cm in the air. The code was registered and the message was well-formed; it just pointed the caller at reseating an actor that needed dropping. When a threshold check and an existence check can both match, test existence first — and answer it there rather than falling through, so a caller that disabled the later check still cannot score the empty case as a pass.
- **Two guards whose messages name each other are a deadlock, and no single message can detect it.** If guard A refuses `X` and says "drop X" while guard B refuses the absence of `X` and says "pass X", the state where both fire is reachable by no argument at all. That is almost always one flag carrying two axes: `model.compile`'s `overwrite` was both permission (replace content this source did not generate) and mechanism (delete-then-recreate versus rebuild-in-place), which made the *first* compile onto any referenced, unstamped asset impossible in both directions. Split the axes rather than rewording either message, and test the intersection — the guards had a test each and the combination had none. Full write-up in `docs/lessons.md`, "Two guards that each name the other as the remedy".

## 8. Batch the work; never make the caller loop

A per-actor Python loop over ~1660 actors (≈5100 calls at roughly a second each) wedged an editor for **168+ minutes**. It was uncancellable and unobservable: even the one status call that would have said whether the work so far was worth saving needed the same blocked game thread.

**Get the cause right, because the wrong cause argues against the fix.** That incident is routinely retold as "a per-actor Python loop blocks the game thread for hours". It does not, and the number is not close. Measured 2026-08-16 on this project's map: the same per-actor sweep — `get_components_by_class` over **all 3283 actors** — run *inside one* `python.execute` completes in **0.1 s**. The engine work was never the cost. The 168 minutes was ~5100 **round trips**, at roughly a second each, essentially all of it transport and dispatch overhead outside the editor.

The advice is unchanged — batch it into one call — but the reason a reader carries away matters: someone who believes the game thread is the bottleneck will conclude that batching just moves the same wedge inside one call and will avoid batching too. It does the opposite. Round-trip count is the thing to minimise; a loop that is ruinous as N calls is usually trivial as one.

- A sweep over a level belongs in C++ behind **one** call, with selection by `actors` / `prefix` / `filter` / `selection`.
- Design the verb so the interesting cross-item question (coverage, spread, the whole cluster) is answerable, because that is the reason it is one call and not N.
- **When the unit of work has a shape, take the shape as a parameter.** Round-trip count is not the only thing a one-per-call verb costs; it also decides what the caller is able to express. `landscape.sculpt` stamps one circular brush at one world location, so an author with a *curve* to draw has no way to say so: the traced path becomes N independent stamps, and the continuity between them — the thing that makes the edge read as one cliff rather than a row of scallops — falls in the gaps between the calls, where no per-call parameter can reach it. Offering the primitive one grain finer than the caller's unit of work silently pushes the interesting half of the problem back to them. Ask what the caller is actually drawing, and take *that* — a polyline, a region, a profile — as one argument.
- Any write-side verb must be **idempotent**. The transport implements a response-only timeout: when it fires, the handler keeps running and its work commits, so a client retry must converge to the same final state rather than accumulate.

## 9. Long operations need a job handle — and the cost is bimodal

- With an engine completion hook, `Ctx.StartJob` is roughly **15 lines**. The per-operation hook table (and its footguns — multicast double-fire, dynamic delegates that reject `AddLambda`, poll-only operations) is in [ue-async-completion-delegates.md](ue-async-completion-delegates.md).
- Without one, genuine cancellation means chunking a work loop: **200–500 lines**. That cost is a legitimate reason not to support cancellation — it is never a reason to claim it worked. Register a cancel callback only when something can actually stop; the registry answers `Unsupported` otherwise.
- **When a verb must stay synchronous, bound it on the cost driver and derive the bound from the response timeout.** `render.capture_ortho_tiles` renders N tiles inline; `CaptureScene()` plus `FlushRenderingCommands` cannot yield mid-tile, so no ticket makes it observable. Two things follow. First, the ceiling goes on **pixels**, not tiles: 91-96% of per-tile cost is PNG encode plus readback, so 64 tiles is ~7 s at 1024 px and ~2 min at 4096 px, and a tile-count-only ceiling passes the two-minute case. Second, the number is derived rather than chosen - the measured 0.112 s/MP against the transport's 120 s response timeout gives 256 MP at ~29 s, roughly a quarter of it. **The failure being refused is not "slow", it is "the handler outlives its caller"**: the transport's timeout is response-only, so when it fires the work keeps running and commits with nobody watching. State the predicted cost in the refusal so the caller can see the arithmetic instead of guessing at the limit.
- `Ctx.StartJob` is **not** a deferral primitive: `FHandlerContext::StartJob` invokes the bind delegate synchronously, so a job whose bind delegate does its work inline still runs on the caller's stack.

## 9a. Progress is a wire contract, not a log line — and it is only possible where the work yields

Long-running verbs report progress as MCP `notifications/progress` frames on the caller's open SSE stream (`wiki-src/mcp-transport.md`). The mechanism was complete and the payload was not: **the feature was notionally present and functionally absent**, which is §1's defect class one layer down — nothing lied, but nothing arrived either.

Three defects, all in the ~25 lines of `UPinWrightSubsystem::HandleJobEvent`, all found by reading the MCP spec against the code rather than by any test failing:

- **The token was invented.** The transport parsed the caller's `params._meta.progressToken` and used it only as a boolean gate, then emitted the server's *ticket id* in its place. The spec admits only tokens "provided in an active request", so every frame the server had ever sent was uncorrelatable by a conforming client. `FPendingCompletion` now retains the token and `FSocketHttpServer::GetProgressToken` hands it back. **Parsing a protocol field and discarding it is worse than not parsing it** — the gate passes, so everything downstream looks wired.
- **The number stopped increasing.** No verb set a `progress` value anywhere, so every frame fell back to the ticket's progress-array length — which is ring-trimmed to 50. Long jobs, the only ones that need progress, counted to 50 and then reported 50 forever, against a spec MUST. **A monotonic counter must not be derived from a bounded buffer**; `FJobTicket::ProgressSeq` is never trimmed and is what the wire value comes from now.
- **There was no denominator.** `total` was never emitted, so no client could draw a bar. It is now published when the reporter knows one and **omitted when it does not** — the §1 rule applied to an absent measurement: a zero total is not "unknown", it renders as finished.

Rules this leaves:

- **One writer for the published number.** `FJobRegistry::RecordProgress` computes the `progress` value once, so the JSONL monitor line and the SSE frame cannot disagree — the §2 pattern that made `EvaluateContact` the only writer of `bPass`.
- **When a protocol rule and §1 collide, §1 wins — and the seam gets documented.** MCP says `progress` MUST *increase* with every notification. A long job legitimately stalls (`asset.dump_folder` sits flat in "waiting for async compilation"), so obeying that literally means incrementing a number for work that did not happen — a value named for an outcome nobody observed, which is the exact defect in §1. The ruling: the value **never decreases**, and a backwards reporter is **held** at its previous value rather than advanced past it. A repeated number is the truth and the `message` says why. The hold is disclosed in the audit line (`reportedProgress`, `progressHeldAtPrevious`) because a backwards count is a reporter bug worth finding, not a normal condition.
- **Check the throttle when you add progress.** `ProgressEventMinIntervalMs` defaulted to **60000** — one event per minute — which would have swallowed any new per-item reporting and produced a bug report against the wrong layer. It is now 1000, and streamed progress bypasses it entirely.

**The honest limit, which belongs in the verb's documentation and not only in its code: progress can only be emitted between units of work, never inside one.** Every handler runs on the game thread, so a verb reports only when control returns to it. A single synchronous engine call — `rename_directory` loading 237 assets, one mesh build, one package save — yields nothing until it returns, and no amount of transport work changes that. This is exactly why the Content Browser shows a progress bar where an RPC cannot: it is *inside* the operation, driving it item by item. A verb whose work is one atomic engine call should say it cannot report progress rather than emitting two frames around a five-minute silence.

The corollary is the useful half: **the chunkable case is fully serviceable**, including work PinWright did not write. `unreal.PinWrightProgressLibrary.report_progress` lets a user script inside `python.execute` emit frames from its own loop — which is precisely the shape of the 168-minute wedge in §8. What makes that work is non-obvious enough to record: `FSocketHttpServer::WriteStreamFrame` only *enqueues* bytes onto a queue the transport's I/O thread drains, so a frame reported mid-script reaches the socket without the game thread returning to the tick loop.

- **A verb that becomes streaming must not change what non-streaming callers receive.** `python.execute` allocates a ticket only when the request already qualified for SSE; a plain-JSON caller gets the identical body it always did. Turning a synchronous verb into a ticket for everyone is a breaking change dressed as a feature.
- **Progress is not cancellation and must not imply it.** Nothing here can interrupt a running script; §9's ruling stands unchanged.

## 10. Tick-unsafe work must be gated

Destroying a level inside `UWorld::Tick` trips `check(!LevelList.Contains(TickTaskLevel))` and crashes. The dispatcher marshals off-thread requests with `AsyncTask(ENamedThreads::GameThread, …)`, and the game thread drains that queue from *inside* the world tick — so whether a handler lands mid-frame depends purely on when the HTTP thread enqueued it. That is why the crash looks intermittent with a single client.

- Use the shared gate in `Dispatch/SafePoint.h`. Hand-copying the gate into one verb is exactly how four sibling level/lighting verbs and the whole capture family were missed.
- **Prefer the method table** (`IsTickUnsafeMethod`): a one-line change, no response-path surgery, and the handler runs later on a safe stack with its *original* `FHandlerContext`. Use the in-handler `RunAtSafePoint` route only when a cross-dispatch caller can arrive on an ungated stack (`level.load`, reached through `editor.open_level` / `editor.open_asset`), since `FRpcDispatcher::DispatchMethod` bypasses the table. Never give one operation both gates.
- Arbitrary-payload verbs (`python.execute`, `system.console_command`, `editor.console_command`) **are** in the table, and the reasoning is worth internalising: because the payload can be anything, the safe default is to assume the worst. The gate is conservative by construction, so listing them can only over-defer.
- Know the limit: the gate fixes stack *position*, not stack *contents*. A `CollectGarbage()` with a live Python frame faults wherever it runs, and an unbounded loop wedges the game thread from the core ticker just as well. Neither is prevented by anything in the plugin; documentation is the only protection.
- Do not defer with another `AsyncTask(ENamedThreads::GameThread, …)` — that is the very queue drained mid-frame. A handler-local task also returns from the handler before its body runs, so `ProcessRequest` clears `bProcessingRequest`; the continuation then executes outside both the request's reentrancy guard and the method-table check that already happened. Remove a redundant marshal, or use in-handler `RunAtSafePoint` when cross-dispatch makes the method table insufficient. For a dispatcher-owned context, `RunAtSafePoint` keeps the originating request scope active across its ticker hop, so a second RPC stays queued until the continuation returns; do not replace that route with a naked `DeferToSafePoint` call.

## 11. Do not mutate global state without restoring it

A capture verb that permanently changed cull distances made every later measurement in the session irreproducible — including measurements taken by unrelated verbs. Anything a verb changes for the duration of its own work (view settings, quality knobs, editor preferences, selection) is restored on every exit path, including error paths. Prefer values the verb computes itself over per-user editor preferences: a preference-derived value makes the same batch RPC produce different results on two machines.

**When the mutation is the point and must persist, return the prior value.** `editor.set_game_view` and `editor.set_view_mode` deliberately do not self-restore — a capture burst needs the setting to survive the call. But their prior values were unreadable from the verb, so an agent could not leave the editor as it found it and every burst inherited whatever the last one left. Both now return a `previous` block. A verb that persists a change owes the caller the value needed to undo it; "does not restore" and "cannot be restored" are different contracts and only the first is acceptable.

**The package dirty flag is global state, and a read verb must leave it exactly as it found it.** `landscape.get_heights` — whose own registration string reads "Read back a landscape's heightmap over a region" — took a host project's `editor.list_dirty_packages` from 0 to 1 on a fresh boot, with one 5x5 read and no other RPC in between. The cause is that the engine "read" it calls is an **edit** interface underneath: `FLandscapeEditDataInterface::GetHeightData` reaches `FLandscapeTextureDataInfo`, whose constructor calls `Texture->Modify(bShouldDirtyPackage)` with the interface's `bShouldDirtyPackage` defaulting to **true** (`LandscapeEditInterface.cpp:4028-4043`, `:88-90`), and the heightmap texture is outered to the landscape actor — so a pure read marked the MAP package dirty. Passing `bUploadTextureChangesToGPU=false` does not make such an interface read-only; it only gates the GPU upload. Look for the API's own dirty opt-out (`SetShouldDirtyPackage(false)` here) rather than assuming a `Get` prefix means const.

Why this outranks a cosmetic flag: **the dirty flag is the only evidence a project has that a write landed.** Saves are gated on it (`save_asset` defaults to `only_if_is_dirty=true`) and an in-memory read-back cannot fail, because `load_asset` returns the object already resident. A read that dirties makes "the package is dirty" stop discriminating between a successful apply and a no-op apply that happened to follow a measurement, and it makes every later `save_all` rewrite files nothing changed — plus an unsaved-changes prompt on quit, which is the moment real work gets discarded by mistake.

The contract is **preserve, not clear**: a read that runs on an already-dirty package must leave it dirty, or it silently throws away someone else's pending edit. Remove the cause where you can find it; `PinWright::PackageDirty::FScopedPackageDirtyRestore` (`Utils/PackageDirtyUtils.h`) captures and restores the flags around the read as the structural backstop, and logs a Warning naming the package when it actually had to restore one — so a surviving cause is reported rather than masked. Test **both** starting states: a clean-stays-clean assertion alone is also passed by a verb that unconditionally clears.

## 12. Test the failure direction

- **Before you write error handling on a return value, confirm the API reports failure through it.** Every GeometryScript entry point returns its `TargetMesh` on *every* path, error paths included, and reports the actual failure into a `UGeometryScriptDebug*` out-parameter. `GeometryOps::Boolean`/`Trim`/`Mirror` passed `nullptr` there and gated their error codes on `if (!ResultMesh)` — a condition only a null *input* can produce, which the op already rejected a line earlier. Result: **all four `geometry.boolean_*` verbs reported success on a boolean the engine had refused**, for as long as they had existed, with an error path that read like real handling. The generalisation is not about geometry: when an engine API takes a diagnostic out-parameter, grep your call sites for `nullptr` in that argument before trusting anything downstream of it.
- **A failure test that trips your OWN guard proves nothing about the layer you wrap.** The boolean family's failure tests asserted `ExpectedCode` on null-handle inputs — which the ops reject before reaching the engine — so they were green throughout the defect above and would have stayed green through any regression of it. A failure test has to make the *wrapped layer* fail. Ask what real input makes the engine refuse (here: a `Subtract` whose tool encloses the target, so the result is empty), drive that, and assert the caller receives the error. Pair it with a control that flips one option and turns the same call green, or the test cannot distinguish "the fix works" from "the fixture is broken".
- **Reviving a dead error path exposes every side effect sequenced before the failure check — audit them in the same change.** `geometry.boolean_*` destroys the tool actor when `keepTool: false`, unconditionally, and that was safe only because `BOOLEAN_FAILED` could not fire. The moment it could, a refused boolean deleted the caller's cutter and then reported failure: tool gone, no cut, nothing to retry with. Destroying an input is a **commit** and belongs on the success path next to the dirty-flag/notify commit. The general rule: when you make a check reachable, re-read everything downstream of it that used to be unreachable-by-implication, and ask of each side effect whether it is a commit or a cleanup. Commits go after the check; only cleanup of things the op itself allocated goes before it.
- **When the side effect happens INSIDE the failing call, the fix is a rollback, not a reordering.** The rule above assumes the commit is a separate statement you can move below the check. `AppendBuffersToMesh` breaks that assumption: it appends every vertex, then validates each triangle, and reports a refusal from inside the same `EditMesh` lambda that has already mutated the mesh. There is no ordering that makes `geometry.append_buffers` atomic, so the op snapshots the target and restores it on failure. Before choosing to pay that copy, check whether a targeted undo is possible - here it is not, because `FDynamicMesh3::AppendVertex` reuses ids from the free list, so the appended vertices are not a contiguous tail you can identify after the fact. And check whether the copy is avoidable in the common case: an empty target rolls back with `Reset()`, which is most calls. **Where rollback is genuinely impossible, the failure message must say what state the mesh is in** - `geometry.subdivide` names the iteration it failed on and `geometry.poke` says the face offset was already applied, rather than implying the mesh was left alone.
- **An engine that reports per element, not per call, needs a bounded summary - a verbatim join is a denial of service.** `AppendBuffersToMesh` calls `AppendError` once per refused triangle. Joining those verbatim turns a 40k-face malformed OBJ into a multi-megabyte string in one JSON error field. `FGeometryScriptDebugSink::ErrorSummary()` deduplicates and counts instead (`"...Non-Manifold Mesh Topology (x39412)"`), which is lossless here only because the engine builds those messages from `LOCTEXT` constants with nothing interpolated into them. Pair it with `ErrorCount()`: the caller needs the number of things refused, and `HasError()` cannot give it. Ask "does this API report once per call or once per element?" before forwarding its text.
- **A silent success is not always the absence of an error - sometimes the engine raises one and continues anyway.** `AutoGeneratePatchBuilderMeshUVs` reports "Requested Polygroup Layer does not exist" and then generates UVs with no group constraint; `ComputeTangents` warns that Standard MikkT is unsupported and silently computes Fast MikkT instead. Reading `HasError()` as "the operation did not happen" is wrong for the first, and reading a warning as ignorable is wrong for the second. Decide per site which of the two an engine message is, and say so in the message you forward - PinWright reports the PatchBuilder case as a failure (the caller asked for a constraint and did not get it) while noting that UVs may still have been written.
- **Before wiring a diagnostic out-parameter, check the failure is reachable - and say so when it is not.** Of 161 GeometryScript call sites in `PinWrightGeometry`, most raise nothing but a null-input error the plugin already rejects one line earlier. Wiring those adds a check that cannot fire and invites a test that cannot fail. `docs/geometry-debug-sink-sweep.md` carries the verdict per site so the next sweep does not re-derive it; `geometry.subdivide` is wired with **no** test and a comment explaining that `FPNTriangles` has no reachable failure on UE 5.8. Stating that an untestable site is untestable is the deliverable; a green test over it is worse than nothing.
- **A test that drives a real engine failure must declare the engine's log line as expected, or it fails on its own success.** `UE::Geometry::MakeScriptError` writes `UE_LOG(LogGeometry, Error, ...)` **before** it checks whether a `UGeometryScriptDebug` was even passed, so a test that provokes an engine refusal emits a genuine Error line - and UE's automation framework fails any test with an uncaptured Error. The boolean family's two dispatcher tests passed in one suite run and failed in the next on byte-identical code, with every assertion green and the captured log line as the only failure; whether the framework captures it is environment-dependent, which is worse than either outcome because it reads as a flake rather than a rule. Declare it: `AddExpectedErrorPlain(TEXT("<the engine's text>"), EAutomationExpectedErrorFlags::Contains, /*Occurrences=*/0)`. **`Occurrences = 0`**, meaning "any number including none" - a fixed count fails in the other direction on the runs where nothing is captured. Use the `Plain` variant: the engine's messages contain `.` and other regex metacharacters, and the non-plain overload treats the pattern as a regex by default.
- A test asserting that a verb returns success proves almost nothing here. Assert that it **fails** when the underlying operation fails — every assertion in `TestJobCancelHonesty.cpp` is written so it breaks if the verb goes back to answering success.
- Regression tests must call the same production helper the handler uses. A test that re-implements the fix as a local lambda still passes after reverting the production code.
- Derive expected test counts independently. A harness that quit early once reported zero failures over a run that skipped **1,646** tests. `started == success + fail`, and the started count must match the expected suite total — see the Testing section of `CLAUDE.md`.
- **A presence assertion cannot catch a constant.** `remesh_uniform`'s regression test asserted the response carries a non-zero achieved `triangleCount` — which a handler ignoring the request and remeshing to a fixed density satisfies perfectly, because a constant is still non-zero. Same shape for a colour verb reporting `verticesModified`: the number was `EditMesh.VertexCount()` unconditionally, and every test that checked it was positive passed. **For any parameter that shapes an output, the test is differential: run the same fixture at two values and require the outputs to differ in the requested direction.** Assert the ORDER, not the value, where the engine only approximates the request — a uniform remesh targets an edge length derived from a triangle budget, so demanding a specific count buys flakiness for no extra coverage. And assert the accepting direction too: a pair of zeroes is also "equal", so check both runs produced geometry before comparing them.
- **A fixture that never enters the defect's state cannot test for it.** The only coverage on `set_vertex_color set_all` was create-box, paint, assert `hasColors` — on a mesh that never grows. The defect was that geometry appended AFTER the first paint stayed unreachable, which sits entirely outside that counterfactual, so the test was green for the whole life of the bug. When the defect is about a sequence, the fixture has to perform the sequence, and the mid-sequence state should be asserted as a **known-bad control**: after the append and before the second paint, the new triangle must genuinely carry no colour element. Without that check, an engine change that started colouring appended triangles would make the final assertion pass while measuring nothing.


## 13. Wiki rendering constraints that silently hide documentation

Both of these fail silently, and both have eaten shipped content:

- **Rendering stops at the first `### ` line.** Any `##` section written after the first `###` is invisible on the rendered page. This swallowed ~26.5 KB across five topic pages, including the only inbound pointer to the customer-facing support page. Put every `##` editorial section above the first `###`, and prefer bold labels to `###` on topic pages.
- **A `###` heading reaches its method page only if it is exactly the bare dotted method name.** Anything else (a decorated title, a partial name) leaves the section stranded.

A third silent failure lived in the same renderer and is now closed: **the maturity map used to fail open.** `docs/wiki-src/maturity.json` classifies each top-level namespace `core` | `experimental` | `internal`; a namespace with no entry rendered *bare*, and bare is exactly what `core` renders — the root-index legend still says "unmarked namespaces are core". So a brand-new or forgotten namespace advertised itself as solid primary surface, and a test asserted that as correct. An unmapped namespace now renders `(unclassified)` on the root index and `Stability: unclassified` on its page, and `Maturity.EveryRegisteredNamespaceIsClassified` fails the suite naming the offender. The general rule: **a default that means "no information" must never coincide with the value that means "most trusted".** See `B-maturity-unmapped-namespace-fails-open`.

Fuller authoring rules — prelude length, the auto-generated `## Methods` index you must not duplicate, topic-page extraction, the ~20,000-char soft budget — are in the Wiki Authoring Constraints section of `CLAUDE.md`.

## 14. A verb that needs a newer engine than the plugin advertises is a contract defect

The advertised engine range is a promise the whole plugin makes, and one new call can break it for every verb at once. Nothing at runtime can report that — the build simply fails on the consumer's engine — so the check belongs at authoring time, where it is one grep.

Shipped instance: `0fe35187` landed `ALandscapeProxy::RetrieveAllLandscapeMaterials` (5.8-only) and `UWaterBodyRiverComponent::{Get,Set}River{Width,Depth}AtSplineInputKey` (5.6+) unguarded, in a changeset carrying zero `UE_VERSION_*` macros against 221 elsewhere in the plugin, while `CLAUDE.md` advertised UE 5.3–5.8 in three places. Both calls were right for the target engine; the defect was the untouched range claim.

- **The two absences have different remedies — do not confuse them.** *Runtime* absence (the plugin compiled, the feature is not in this editor) is `Ctx.SendUnsupportedEngineVersion`, §1. *Compile-time* absence has no runtime remedy at all; reaching for the error code does not address it.
- **Establish a symbol's first version before using one you met in the current headers.** All six engines are installed at `C:\UE_5.3` … `C:\UE_5.8` on the dev host. The sibling call on the next line is not evidence: `UpdateAllComponentMaterialInstances` is on all six, `RetrieveAllLandscapeMaterials` on one.

## 15. Speak the identity the user sees, and refuse the one that does not identify

An actor has two names. The **display label** (`GetActorLabel()`) is what the World Outliner shows and what renaming in the outliner changes; it is editor-only, mutable, and **not unique**. The **internal object name** (`GetName()`) is unique within the level and effectively immutable. A user reading the outliner and a script resolving by name can therefore mean different actors while both look correct.

Measured on this project's map: **3137 of 3137** actors have an engine-generated internal name (`StaticMeshActor_N`, `Emitter_N`, …), **0** have a label equal to their internal name, and every naming convention the level uses (`TW_`/`CP_`/`R`/`D`/`TOP`/`MID`) lives entirely on the label. The label is not a nicety here — it is the only identifier carrying meaning. So verbs must accept it, report it, and be able to set it.

But a non-unique key cannot be resolved by guessing. Rules:

- **Resolve by precedence tiers, evaluating each tier across every actor before falling to the next.** `McpActorUtils::ResolveActor` tries object path, then internal object name, then exact label, then label substring. The two unique tiers resolve deterministically; the two label tiers can match several actors. Evaluating a whole tier before the next is what makes the answer independent of iteration order.
- **An identifier matching several actors is a structured error, never a pick.** `AMBIGUOUS_ACTOR_NAME` carries every candidate as `{label, name, path, class}` so the caller can re-issue against a unique internal name without a second round trip. Choosing `Candidates[0]` is the §1 defect in its purest form: success reported for work done to something other than what the caller named.
- **Where a helper cannot report a structured error, it must return nothing rather than a guess.** `FindActorByName` has ~100 call sites that only have `nullptr` to work with; it now returns `nullptr` on ambiguity. Downgrading a silent wrong-actor mutation to an honest miss is the right trade at every one of those call sites.
- **This is not an actor rule.** Any slot that accepts a *human* key beside a unique one has the same hazard, and the same answer. A material `nodeId` accepts a GUID, an object name, a path, or a **parameter name** — and a parameter name is legally shared by several expression nodes (a triplanar material samples one texture parameter on two or three projection planes). `set_texture_sample_texture` repointed the first match and returned plain success; the graph stayed legal, compiled clean, and `get_material_node_details` resolved the same way, so no in-band signal could contradict it. The shared resolver now returns every match, all eight verbs that take a `nodeId`/`expressionId` refuse with `AMBIGUOUS_NODE` + candidates, and the pointer-returning helper returns `nullptr` on ambiguity for the same reason `FindActorByName` does. When you add an identity slot, ask which of its accepted spellings is non-unique before the first caller finds out.
- **Every per-actor row carries both identities.** A row with only a label cannot be fed back into a lookup; a row with only an internal name is unreadable against the editor. `AddActorVerification` emits `actorLabel` **and** `actorObjectName`; query rows emit `label` **and** `objectName` beside whatever `name` historically meant. Do not repurpose an existing key's meaning to fix this — add the missing one.
- **Do not confuse "no match" with "not specific enough".** They have different recoveries (fix the typo vs. pass a unique name), so batch verbs bucket them separately: `missing[]` and `ambiguous[]`, never both under `missing[]`.
- **A response-shape family is defined by what a verb DOES, not by what it is named.** Every verb that spawns an actor owes the caller `class` plus the `AddActorVerification` block. Enumerating that family by the `create_` prefix missed `geometry.revolve`, `geometry.import_obj` and `geometry.import_stl` twice — once when the convention was written and again when a sweep was run to enforce it — because all three spawn and none is spelled `create_`. Enumerate by the call the verb makes (`GeometryTarget::Spawn`), and make the test's verb table that enumeration, so the next member added without the shape fails rather than joining quietly.

Shipped instances, all found together:

- `FindActorByName` scanned actors and `break`-ed on the first one matching label OR name OR path. Two actors labelled `PWLabelProbe` (spawned by `actor.spawn_shape`, which itself sets only a label) resolved to whichever the level iterated first. `actor.set_folder` on that label moved **one** of the two and returned `{"updatedCount":1}` — a success naming a folder move the caller could not have predicted the target of.
- The ambiguity warning that did exist fired only on the *fuzzy* substring path; the exact-label collision — the common case — was entirely silent.
- `actor.find_by_class`, `actor.find_by_tag`, `actor.set_folder`, `actor.spawn*`, `actor.duplicate` and `level.audit` all emitted the label under a key spelled `name` and never emitted `GetName()`, so their output could not be fed back into a lookup that resolves deterministically.
- There was **no verb to set a label at all**. `actor.set_label` closes that; it routes to `SetActorLabel` (or `SetActorLabelUnique` on `unique=true`) and reads the label back off the actor, because `SetActorLabel` drops a label failing `FActorEditorUtils::ValidateActorName` with only a log warning — echoing the request would report a rename that never happened (§4).

**`AActor::SetActorLabel` does not uniquify.** It validates, compares and assigns (`Engine/Private/ActorEditor.cpp:1291`); only `FActorLabelUtilities::SetActorLabelUnique` appends a suffix (`EditorEngine.cpp:6579`). This repo believed the opposite in a test comment, which is how duplicate labels were assumed unreachable. Both halves are now pinned by tests.

**Documenting a hazard is not fixing it.** `docs/wiki-src/actor.md` had described the collision and told callers to pass the internal object name instead — accurate, and still shipped alongside a resolver that silently guessed. Per §2, a rule the caller must remember should be a structural guarantee instead; the doc now describes a resolver that refuses rather than a convention the caller must uphold.
- **A plugin-presence gate is not a version gate.** `#if MCP_HAS_WATER` compiles out when the Water plugin is disabled and says nothing about a Water API added in 5.6. Nor is `WITH_EDITORONLY_DATA`, which is always 1 for this plugin's editor-only modules.
- **Shipping unguarded is allowed when it is a decision; then write it down.** Add a row to [engine-version-support.md](engine-version-support.md) — symbol, call site, verbs, first version, substitute — *and* a per-version note at the call site in the shape of `SkeletalMeshAssetIOHandler.cpp:35-71`. That matrix is what turns a backport into a copy-paste; the difference between debt and a defect is whether the next person has to re-derive it.
- **Move the range claim in the same commit that breaks it.** A document advertising a range the code cannot build is §1's defect class — a claim asserted instead of measured — and it costs the reader a failed build rather than a wrong number.

## 16. Vision verification is mandatory; numbers are never sufficient on their own

**If the verb has a visible result, no metric may carry the claim alone.** Capture it, look at it, and say what you saw. "The numbers pass" reports the measurement, not the work — and a report with no image and no visual description is unverified by construction, including one relayed from somewhere else. Ask what it looked like rather than forwarding a green.

The failure is always the same shape and it is never an arithmetic mistake: **a real number, correctly computed, measuring the wrong quantity** — very often the specification the author supplied, so it confirms compliance rather than correctness. A metric that restates your own configuration cannot fail, and one that cannot fail is not evidence. Every row below was right about what it measured:

| Reported | Read green while | Why it could not fail |
|---|---|---|
| `blank: false` on all 16 tiles | every tile was visually black | all-four-channels-zero is the right test for "never drawn", and says nothing about a frame drawn over nothing |
| `pinned: true` | the frame was crushed to black | a predicate over viewport state, not over the pixels it produced |
| `…PinnedCapturesAreIdentical` passing | its substantive assertions never executed | a conditional skip reports success, and the started/succeeded totals cannot see it (`check_suite_log` now can, but only for skips that emit the marker — see §17) |
| `modifiedVertices: N` from `landscape.sculpt` | the stamp moved terrain by nothing | it is the size of the clamped rectangle the brush covered, not a count of vertices whose height changed |
| a suite with `Result={Fail}` = 0 | 610 of 3780 tests had run | absence of failure is not presence of testing; the run was a killed commandlet |
| a parse gate exiting 0 | it had been pointed at a path holding nothing | no input, therefore no findings, therefore success |
| `side_fill_frac` 1.36, `side_gap_frac` 0.026 | the mesh rendered as a smooth balloon | both measure **area against a fixed envelope**, and a convex blob fills an envelope better than the correct shape does — the defect was the *outline*, which no area measure reaches |

So:

- **Compare against the reference, not against the spec.** If a person looking at the pair cannot say "those are the same thing", it is not green whatever the scalars say.
- **Publish a magnitude beside every boolean**, and characterise the artifact rather than asserting a property of it — §1 and §4 carry the worked cases, including why a pinned pair still needs a tolerance rather than an equality check.
- **A verb whose result is visible should make the artifact cheap to obtain**, and must never report success over a frame it has not characterised.
- **A check that examined nothing must not print a pass.** Guard the empty denominator explicitly and report the scope beside the verdict; `0 ERROR` has to mean "I looked, and it was fine", never "I looked at nothing".
- **A metric written for a defect class does not cover that class — name the QUANTITY, and check the new metric against the old defect *and* the new one.** This is the hardest row in the table above, because it is the one where the author had already been burned and had already acted. `side_fill_frac` / `side_gap_frac` were added the same day, deliberately, after a tree passed aspect, bounding box, projected area and uniform-scale checks while rendering as seven separated discs with the trunk showing between every pair. Those two are honest, independently recomputed, and they caught that tree — 0.4350 → 0.8535 and 0.4143 → 0.0131. Hours later a second mesh passed **both** and rendered as a smooth balloon on a stick. `fill` went 1.3604 → 1.2428 across the fix: **the new metric moved the wrong way on the correct change.**

  The reason is that "density inside the outline" and "shape of the outline" are different quantities, and the first was adopted believing it covered the second. A third measure — projected area over the area of its own **convex hull** — read **−0.0003** on the balloon and 0.1685 on the fix; −0.0003 is a literally convex outline, and it was the only number that could tell them apart. The generalisable mechanism is worth more than the number: **a form that is star-shaped about an axis has a silhouette equal to its radial maximum over the sweep, so any modulation of the surface moves the surface and never the outline.** That mesh measured 25–70% radial spread on every ring while its silhouette ran 91 → 252 → 89 uu, a smooth curve. No shading or material change could have rescued it, and reaching for one would have burned the pass.

  Two rules fall out. **Write down which quantity a new metric measures, in the units of the defect, before adopting it** — had "area against an envelope" been written next to "the outline is wrong", the gap would have been visible on paper. And **run a new metric against the defect it was written for *and* against the next one that looks like it**; a metric validated on a single case is calibrated on one end only, exactly as §4 requires of a pixel threshold.
- Numbers remain right for the invisible — counts, extents, suite totals, bytes written — and even there prefer a measure the write path cannot fake (§4).


## 17. A run that measured nothing must not report as one that measured

**This is the universal form of §16's last two bullets, and it applies to every verdict a verb, a
gate or a checker emits — not only to visible results.** A pass has two claims folded into it:
*something was measured*, and *it was fine*. Almost every silent false green comes from the first
claim being empty while the second is trivially true, because "nothing was measured" and "nothing
was wrong" produce the same zero.

Four shapes, all recorded here:

- **Zero denominator.** A parse gate pointed at an empty directory exits 0. No input → no findings
  → success.
- **The queue never drained.** A killed suite commandlet leaves a log with zero failures, no crash
  dump and no fatal banner, so every failure-grepping check passes it. 610 of 3780 and 762 of 3778
  were both quoted as green.
- **The counter counted the wrong thing.** `found=0` is falsy, so `if found and finished < found`
  never fires; a terminal marker over zero tests then classifies clean.
- **The assertions were stepped over.** A test that cannot measure its fixture returns success
  after skipping its assertions and lands *inside* the success total. `succeeded=4241` contained
  one such test, and nothing downstream could see it.

The rules that fall out:

- **A verdict must be able to fail on emptiness, not only on badness.** Guard the empty
  denominator explicitly. `0 ERROR` has to mean "I looked, and it was fine", never "I looked at
  nothing" — and `N passed` has to mean N assertions ran.
- **Emit a marker at the point of the skip, and gate on it downstream.** The skip itself is often
  correct: a test asserting an exposure response against pixels that cannot respond would assert
  nothing meaningful, and making it a hard failure just makes the suite red on host conditions.
  What is not acceptable is the skip being invisible. In this repo the contract is
  `PINWRIGHT_ASSERTIONS_SKIPPED` (see `CLAUDE.md` § Testing), matched by **prefix** because the
  engine appends ` [file(line)]` — and a skip that forgets the marker is still invisible.
- **The marker is a WIRE FORMAT consumed by `Content/Python/check_suite_log.py`**, which greps it,
  counts it and refuses to call such a run `COMPLETED_CLEAN`. Change the literal in both places or
  in neither.
- **Emit it with `AddWarning` — never `AddInfo`, never `UE_LOG`.** No document said this, which is
  exactly why 20 sites drifted to `AddInfo`. `AddInfo` events are not
  written to the automation log at all, so a marker emitted through one is invisible rather than
  merely quiet: 20 such markers sat in `Tests/Render/` unseen by any log-based checker, and the
  measurement that found them is that a 4242-test log carried exactly 8484 = 4242 × 2
  `LogAutomationController: Display:` lines (Started and Completed only) and zero `NOT MEASURED`.
  `UE_LOG` is wrong for the opposite reason: `bElevateLogWarningsToErrors` turns a log warning
  into a test failure, which is the hard-fail regression the skip mechanism exists to avoid.
- **A skip is its own outcome, not a pass and not a failure.** `COMPLETED_WITH_SKIPS` /
  `EDITOR_TESTS_SKIPPED` exists so a caller can tell "nothing is red" from "this run measured what
  a clean run measures". Collapsing it into either neighbour loses exactly the fact worth knowing.
- **Put the empty-measurement count next to the reassuring one.** `succeeded=4241 skipped=1` is
  the whole fix; a skip count in a footnote is a skip count nobody reads.
- **When preparation and verdict are separate, only the checker owns the verdict.** `editor_prepare_tests` is the canonical command-returning planner. It accepts one mandatory, non-empty `filter`, runs the live PinWright precondition first, and reports `EDITOR_ALREADY_RUNNING` for a detected editor or `not_probed` when the live probe is unavailable. It does not claim that an unavailable probe proves the editor is stopped.
  The planner resolves the project's `EngineAssociation`, returns the matching `UnrealEditor-Cmd` executable, and returns a `COMMAND_READY` object with the launch executable and `argv`, checker executable and `argv`, project path, and explicit absolute `logPath`. It returns immediately and does not compile, launch, wait, kill, sample CPU or IO, apply timeout overrides, or return a test verdict.
  The caller runs the returned launch executable with the returned launch `argv`, then runs the returned checker executable and `argv` for `check_suite_log.py` with the exact same `logPath`. The launch contract uses `-ExecCmds="Automation RunTests <filter>,Quit"`, `-TestExit="Automation Test Queue Empty"`, and `-Abslog=<same absolute logPath>`, plus `-unattended`, `-RunningUnattendedScript`, `-nopause`, `-nocefaccelpaint`, `-ddc=InstalledNoZenLocalFallback`, and `-log`. It uses a real RHI and does not add `-NullRHI`.
  `check_suite_log.py` remains the sole fail-closed classification path. It keeps one ordered ladder for empty evidence, incomplete runs, failures, skipped assertions, and clean runs. `PINWRIGHT_ASSERTIONS_SKIPPED` remains a distinct `COMPLETED_WITH_SKIPS` outcome, not a clean result.
  Two entry points may share parsing helpers, but preparation must not duplicate the checker's rules, and neither preparation nor a caller's process exit may replace the checker's observation-based verdict.
---

## 18. An audit verb has one contract, and it lives in `Audit/AuditFramework.h`

An **audit** is a verb that returns a verdict about content it did not write: `level.audit`,
`geometry.audit_static_meshes`, `landscape.audit_shape`, `skeleton.audit_skin_weights`. They are
§17 applied to a shipped wire surface, so the rule is not restated per verb — it is
`PinWrightAudit::FVerdict::DerivePass` in `Source/PinWright/Private/Audit/AuditFramework.h`, and
every audit derives its `pass` through it. The header is header-only and under the main module's
`Private/`, which is why `PinWrightGeometry` can include it: that module already adds
`Source/PinWright/Private` to its `PrivateIncludePaths`, so no export macro and no link edge.

**The universal rule.** *An audit reports `pass: false` with structured findings; it never reports
a content defect as an RPC error. And unmeasured is never a pass.* Those are one rule, not two: a
verb that errors out on bad content teaches callers to treat its errors as content facts, and then
a genuine "I could not measure this" arrives through the same channel and reads as a verdict.

**The verdict, in full:**

```
pass = no finding at or above failOn
       AND zero unrunnable checks
       AND the sweep was not truncated
```

`failOn` (`error` | `any` | `none`) moves the **severity bar and nothing else**. It cannot reach
the unrunnable term and it cannot reach the truncation term. `failOn: "none"` means "no *finding*
should fail me"; it must never be able to mean "measure nothing and call it clean". An audit with
nothing to truncate — `landscape.audit_shape` refuses an oversized region rather than measuring
part of it — leaves `bTruncated` false rather than writing a second rule.

**What every audit shares, and therefore does not re-declare:**

- **`ESeverity` / `EFindingStatus`** and their wire spellings (`error` / `warning`,
  `flagged` / `unrunnable`). A caller branches on those strings; two audits spelling the same
  concept differently is a wire defect.
- **The check-descriptor table.** Each audit keeps its own `FCheckInfo`, because the extra columns
  are genuinely per-audit (`bNeedsSurface`, `bNeedsClosed`). What is shared is the shape every one
  of them has — `.Check`, `.Id`, `.bDefaultOn` — and the operations over it: `ParseCheckId`,
  `CheckInfo`, `CheckBit` / `HasCheck`, `DefaultCheckMask` / `AllCheckMask` / `MaskWhere`, and
  `ValidCheckIdList`.
- **`ParseCheckId`'s semantics.** It returns false for an unknown id and **leaves the out-param
  untouched**, and every caller turns that into an error rather than skipping the entry. A typo in
  `checks` that silently ran nothing is indistinguishable from a subject that passed every check —
  the §17 false green, one argument earlier. `ValidCheckIdList` derives the rejection message from
  the table, so a check added to the table cannot go unmentioned in the error that rejects its typo.
- **`PassRuleText`**, which composes the sentence the response publishes as `passRule`. Publishing
  the rule is not decoration: a pass rule the caller infers from the numbers is a pass rule two
  readers infer differently.

**Report what could NOT be checked, per subject, in buckets that sum.** Every audit reports each
subject under one of `flagged` / `clean` / `unrunnable` / `not-applicable` (plus `ignored` where
ignore rules exist), and the buckets sum to the number of subjects examined. A subject that falls
out of every bucket is exactly how a check silently stops running, and the sum is what makes that
detectable. `not-applicable` is **not** `unrunnable`: the first means the check has no meaning for
this subject (a mesh check on a light, `inverted` on an open surface), the second means it applies
and could not be evaluated.

**An argument error is not a finding.** This is the boundary that is easiest to get wrong, because
folding a bad argument into the report *looks* like honesty. It is the opposite: a caller passing
`/Game/A/SM_X` where `geometry.audit_static_meshes` wanted `/Game/A/SM_X.SM_X` got seven
`unrunnable` rows coded `MESH_AUDIT_UNLOADABLE` and `pass: false` against a healthy mesh — an
argument-form mistake wearing a content defect's costume, and a caller acting on it re-authors a
mesh that was fine (`B-mesh-audit-package-path-reads-as-broken-asset`). Three facts, three answers:

| the input | the answer |
| --- | --- |
| not a well-formed identifier at all | an RPC error (`INVALID_ARGUMENT`) **before the sweep**, naming the expected form. It never enters the findings structure. |
| well formed, names nothing | `unrunnable` with its own code (`ASSET_NOT_FOUND`) — genuinely unmeasured, so it still fails `pass`, but distinguishable from a broken subject. |
| well formed, resolves | measured. |

Normalise where the shorter form is unambiguous (`PathUtils::NormalizeToObjectPath`) rather than
refusing it — refusing teaches a rule the rest of the surface does not follow. And **never drop**
an entry that did not resolve: silently shrinking the requested set is how a sweep reports a clean
verdict over subjects it never saw, which is §17 again.

**An empty match set is an error, not a zero-item success** (§8). A folder that matched nothing and
a folder full of correct content must not produce the same-shaped answer.

---

## 19. A service that cannot serve must say so loudly, and must retract what it advertised

§1 says report only what happened. The transport-level form of it is stronger, because a broken
transport cannot answer the very call that would report the breakage: **when the surface itself is
down, every honest signal has to travel out-of-band, and nothing that is still readable may keep
claiming the surface is up.**

Two earned failures, both from `B-failed-bind-no-retry`, both of them "fail open" wearing a
success costume:

- **Failing terminally on the first attempt.** The listener port was bound exactly once. A
  replacement editor booting before a dead one released the socket lost the bind, logged one line,
  and served nothing for the rest of the process lifetime — measured at 4 h 15 min in one session.
  Everything else about the editor worked, so it read as "the plugin is broken" rather than "this
  port is busy". A failure that can clear itself by waiting must be **retried on a bounded
  schedule** (`Public/Transport/BindRetryPolicy.h`); one that cannot — no socket subsystem, an OS
  reserved range — must be terminal *immediately* and say which it was, because burning a
  ten-minute budget on a condition waiting cannot fix is its own kind of dishonesty.
- **Leaving a stale advertisement in place.** `Saved/PinWright/gateway-port` was written on a
  successful bind and never retracted, on a "last-known-good survives failures" policy. That policy
  is only defensible while something still honours the claim. Combined with never retrying, it
  pointed every caller at a port from an earlier session — and the stdio proxy re-resolves the
  endpoint from that file *per call*, so a live editor that knew it was not serving went on aiming
  clients at a dead endpoint. **The state a client reads to find you is part of your interface: it
  must be retracted when it stops being true, not just refreshed when it is.**

**The retraction rule, which is the part that is easy to get wrong.** The obvious fix — delete the
file whenever our bind fails — breaks the normal case. A second editor of the same project loses
the bind *precisely because* the first one holds that port and is serving on it; there the file is
correct, and deleting it takes down the instance that works on behalf of the one that does not.

> Retract a shared claim only on **proof** that it is false, never on the mere fact that *you* are
> not the one honouring it. Ownership is unknowable after a crash; liveness is observable.

`Transport/PortAdvertisement.h` settles it with one probe — is anything listening on the advertised
port — and every inconclusive answer (no socket subsystem, a timeout) reads as *listening*, so an
ambiguous probe can never delete a working endpoint. The verdict itself is a pure function of
(is anything advertised, is anything listening); the impurity is confined to the probe. That split
is what makes it testable: a test that could only observe this through two real editors contending
for one port would be skipped on every host, and skipped is exactly how the first half of the
defect survived review.

**A publish that fails is not a detail.** Publication is half of serving. A bound server whose port
never reached the file is unreachable by every client and, in the log, looks identical to a lost
bind. `WritePortFile`'s return value is therefore load-bearing: false means "not serving yet",
which is an Error and a retry on the ticker, not a discarded bool.

**Failure-direction tests here run both ways** (§12, §17). One fails if a dead advertisement is
left standing; a second fails if a live one is deleted. Only the pair pins the behaviour — either
alone is satisfied by a one-line change that recreates the other defect.

---

## 20. A verb that ends a process owns the state it hands to teardown

`editor.quit` is the shape: its whole job is a *clean* shutdown, so a crash during shutdown is
not a side effect of the verb, it is the verb failing at the one thing it does. And it fails
expensively — a crashed exit loses whatever was unsaved and arms the next launch's restore-packages
modal, so one bad quit can wedge the following session too.

Two faults on this path, and neither was fixable where it faulted:

- **PIE live at exit** — `UEditorEngine::EndPlayMap()` gets driven from a viewport-layout destructor
  *during* `FSlateApplication::Shutdown()`, and reaches a `UAssetEditorSubsystem` that shutdown has
  already torn down.
- **An asset editor open at exit** — the toolkit is destroyed by Slate window teardown from inside
  `FEngineLoop::Exit()`, and `~FStaticMeshEditor` runs an unguarded `RemoveAll()` against a
  subsystem that is gone (`StaticMeshEditor.cpp:271` on 5.8), faulting on a small member offset.

Both are engine code we do not own, at a point in shutdown where no guard of ours runs.

> **What shutdown does is decided long before the teardown that crashes. The only lever a verb has
> is the state it hands over, so preconditions are the fix — not a guard at the fault, and never a
> try/catch or a sleep around it.**

So `editor.quit` ends PIE and closes every open asset editor *while the subsystems their destructors
reach for are still alive*, which runs those same destructors on a healthy editor. The generalisable
part is the shape, not the two cases: **a verb that hands control to an engine teardown path must
first put the engine into the state that path assumes**, and must decide, per precondition, whether
failing to reach it is fatal.

**Those two answers differ here, deliberately.** PIE that will not stop is a *refusal* to exit
(`PIE_STOP_FAILED`, no exit scheduled) because the crash is certain. A toolkit that refuses to close
is *reported* (`assetEditorsRemaining`) and the exit proceeds, because the crash is likely rather
than certain and an editor that could never be shut down is the worse failure. Choose consciously;
what is not acceptable is a success reply followed by a crash, which is what both cases did before.

**Ordering is also a §12 problem, and the obvious test does not cover it.** The first test written
for this asserted `OpenCount == 0` — it never opened an asset editor, so it exercised only the
nothing-was-open path and stayed green for the entire life of the crash it was filed against. A
teardown-ordering test has to *establish the precondition* (open a toolkit, on an engine asset so it
runs on any consumer's project), then assert the verb cleared it. Anything less is green before and
after.
## 21. An accepted parameter is a promise, and two verbs sharing a concept must share its semantics

A parameter that is declared, accepted without complaint, and then not honoured is worse than one
that does not exist. A caller who passes an unknown key gets `UNKNOWN_PARAMS` and corrects
themselves; a caller who passes `caseSensitive: true` and is silently ignored receives **positive
confirmation** of a filter that never ran, and acts on the result. That is §1 with the falsehood
moved from the response body into the request contract: the verb reports a filtered set, and the
set is not filtered.

There are exactly three honest outcomes for a declared parameter. **Implement it.** **Reject it**
with a typed error naming the accepted values. Or **re-document the verb** to what it truly does.
Accepted-and-ignored is not one of them, and neither is "documented as unsupported but still
accepted" — a caller reads the response, not the page.

Shipped instances, all of them silent:

- `blueprint.references` used `caseSensitive` only to decide whether both operands were lowercased
  before comparing. That is sufficient under `exactTarget`, which pinned
  `Equals(ESearchCase::CaseSensitive)`, and a no-op on the default substring path, because
  `FString::Contains` already defaults to `ESearchCase::IgnoreCase`. The flag was dead on the path
  callers use, on both of the verb's filters.
- `actor.find_by_tag` tested `matchType == "contains"` and let every other value fall into the
  `else`, which ran exact `FName` equality. A typo, or a token borrowed from a sibling verb, came
  back as a well-formed, plausible, silently **narrower** answer that reads as "nothing is tagged
  that way".
- `asset.search`'s `classFilterMode` did the same in the other direction: every unrecognised value
  fell through to the `exact` branch, and the echoed mode agreed with the caller while a different
  comparison had run.

Rules that follow:

- **Never let an implicit `ESearchCase` decide a documented behaviour.** `FString::Equals` defaults
  to `CaseSensitive` while `Contains` and `StartsWith` default to `IgnoreCase`, so a mode set built
  on the defaults disagrees with itself across its own modes. Pass it explicitly at every
  comparison. `NameMatch::FFilter` (`Utils/NameMatchFilter.h`) exists so this is one decision, not
  one per verb; reuse the struct even where the wire shape must differ.
- **An unrecognised enum value is an error, not a mode to guess at.** The dispatcher's
  `UNKNOWN_PARAMS` guard cannot help here — the key is declared and only its *value* is
  unvalidated — so the verb owns the check. Reject with `INVALID_MODE` (or `INVALID_ARGUMENT`)
  **enumerating the accepted tokens and quoting what was sent**; a refusal a caller cannot act on
  costs a second wrong guess.
- **A modifier with nothing to modify is also an error.** `caseSensitive` or `matchMode` supplied
  with no pattern returns the entire unfiltered set while looking like a filtered answer. That is
  the same false confidence one step earlier, and `NameMatch::Parse` refuses it.
- **Echo the resolved semantics, in their canonical spelling.** Every one of these defects was
  undetectable from the response: nothing in it said which comparison produced the rows. The echo
  is what turns a future regression into a visible contradiction instead of a plausible number,
  and it is what lets a caller distinguish "no matches" from "your mode was ignored".

The sibling half of the rule: **two verbs implementing the same concept must implement it the same
way, and the page must say which way.** `asset.list`'s class filter was case-sensitive while
`asset.search`'s was case-insensitive — the same conceptual filter, opposite behaviour, neither
documented. The user-visible failure is asymmetric and quiet: a mis-cased `filter.class` returns an
**empty** `asset.list` page, which reads as "no such assets exist" rather than "your filter's case
was wrong", and the identical string works in the sibling. Whichever behaviour is right, they have
to agree; a divergence is its own defect even before you decide which side is correct.

When you converge two verbs, pick the side by evidence rather than by which is easier to change.
Here `asset.list` moved, on three: it is what the sibling already did, what `NameMatch::FFilter`
defaults to plugin-wide, and what `asset.list` **itself** already did one step earlier, since
`ResolveUClass` resolves the class name case-insensitively when it builds the `FARFilter` — the
case-sensitive post-filter contradicted the verb's own front half. Then make the convergence
testable: the parity test asserts the two verbs' answers agree, so the next divergence fails there
instead of in a caller's empty page.

**A shared helper that runs after the caller must not overwrite what the caller wrote.**
`WriteMeasuredAssetVerification` wrote `assetPath` unconditionally, and `AddAssetVerification` runs
after the handler has built its payload — so 50 of 322 call sites had their own `assetPath`
silently replaced by the bare package path, about 35 of them downgrading a correct object path.
Which ordering a verb got was decided by accident: of three sibling asset-creating verbs, the one
that happened to assign *after* the helper passed its test and the other two did not. Per §2 the
fix is structural, not a remembered ordering — but note that blanket "don't overwrite a populated
field" is the wrong structure here, because some of those call sites echo a caller-supplied request
string rather than a resolved handle, and preserving that trades a silent downgrade for a silent
echo of unverified input. The rule that holds is checkable: **keep the caller's value when it
denotes the same object, replace it otherwise, and report the replacement** (`requestedAssetPath`,
`assetPathSubstituted`). Emit the unambiguous form under its own key as well (`packageName`) — and
pick that key by what the rest of the surface already means by it, or the fix reintroduces the
divergence one field over. `packagePath` was unavailable precisely because `asset.list` rows
already use it for the containing folder.

## 22. A response budget must measure what the reader pays, and a capability the verb decides is a capability that silently does not exist

Three defects found together in one verb (`render.capture_asset_preview`, 2026-08-23). They are
separate lessons that happen to share a response.

**A size gate must measure the string a reader actually reads — once, in the encoding the wire
uses.** `MarkOversizedToolResult` compared the display threshold against
`SerializeJsonObject(ToolResult)`, and that string is not what anyone reads: it was
*pretty-printed* (UE's `TJsonWriterFactory<>` defaults to `TPrettyJsonPrintPolicy` — tab indent,
CRLF — while the transport has always written the body condensed), it counted the payload *twice*
(MCP asks a tool returning `structuredContent` to also return an equivalent text block, so the same
object rides in both and a reader sees whichever their client renders), and the two compounded
because escaping the pretty copy into a JSON string costs two characters per character of
whitespace. Measured on 147 real spilled responses: a 4,618-character payload reached the gate as
**14,228** (3.1×) and **92 of 92** single-still captures spilled to disk — each costing an extra
file read on the verb the mandatory vision-verification loop runs dozens of times per asset, and
not one of them a real overflow. The generalisation: **a threshold on "how much does this cost the
caller" must be applied to the artefact the caller consumes, not to a serialization convenient to
the code.** And when the gate publishes its number, publish both — `characters` (what was measured)
beside `fileCharacters` (what is on disk) — because a single number under two meanings is how the
first one drifted. Corollary that bit twice here: **a constant derived from a measurement factor
must cite the factor** (`ModelCompileHandler.cpp` sized its diagnostic limit from "about 2.13× its
own serialized length", correctly, and is now conservative by a further 2.4× because the factor
moved underneath it).

**A capability table that lives in the verb makes every kind missing from it a structural
absence.** `subjectCoverage` — the one signal that catches a frame containing no subject — was
bound by the handler behind a `GetNiagaraReleaseState` test, so the *verb* owned the list of
subject kinds that can be hidden. Every other kind returned `subjectCoverage: null` for the life of
the feature, in a shape a caller could not distinguish from "the measurement was switched off". It
cost two agents hours on a Static Mesh capture that returned uniform colour with no subject in
frame while reporting `blank:false`, `crushed:null`, `litPixelFraction:1.0`, `boundsInFrame:true` —
every published health signal green over an empty picture. The fix is §2-shaped: the capability
moved onto `FResolvedSubject` as a setter each *provider* binds, so a new kind gains coverage by
binding it rather than by someone remembering to edit a handler. Note the reasoning that kept the
gap alive, because it is the reusable part: the branch was commented "a Static Mesh IS the preview
scene's content and cannot be hidden without hiding the thing the capture is of" — but hiding the
thing being captured is exactly what a *reference frame* is. **A plausible sentence on a branch is
not a measurement**, and the comment outlived every chance to check it.

**A parameter the engine cannot honour must say so — and the DEFAULT is subject to the same
rule.** §21 covers the accepted-and-ignored parameter. What this adds is the case where the caller
passed nothing at all: `elevation` defaults to 30, and under `projectionMode:"orthographic"` the
snap to a cardinal axis rewrote it to 0 with `shotDistribution` silent, so a caller who never
touched the argument still had a documented default replaced without notice. A report gated on
"did the caller supply one" would still have said nothing. **Gate the report on whether the value
survived, measured, not on whether the caller wrote it.** Two further details generalise: publish
the value the plan was *built from* (`elevationRequestedDegrees`) or "30 held" and "30 became 0"
are the same response; and when several causes could each swallow a parameter, publish *which*
(`elevationIgnoredReason`) — the remedy differed for all three here, so one bare boolean sends the
caller to change the wrong argument.


---

## 23. Declare what a parameter IS, not just its JSON shape — and `filepath` is not `path`

`FParamSpec::Type` is the only place the dispatcher can learn what a value means, and until this
rule landed it could only say `string`. That is not enough for a path, because a path has a
property no other string has: **a doubled slash in one kills the editor process.**
`CreatePackage` logs at `Fatal` when its package name contains `//`
(`CoreUObject/Private/UObject/UObjectGlobals.cpp:1094-1096`). `Fatal` is not compiled out in any
configuration — it ends the process and every unsaved package in it, and no `if (!Result)` after
the call is ever reached. `CreatePackage` is not the only door and in most files not even the
first one: `StaticLoadObjectInternal` calls `ResolveName2(..., Create=true)`, which calls
`CreatePackage` on the partial name, so **any load on unvalidated caller text is the same kill**,
at ~385 sites. One such string already took a live editor down with unsaved work in it.

**Three declared types, and choosing between them is a per-parameter reading of the verb.**

| Declare | For | Doubled-slash rule |
|---|---|---|
| `path` | an asset / package / object path — `/Game/A/B`, `/Game/A/B.B`, `/Game/A/B.B:C` | **refused** |
| `classref` | a class reference — a bare short name (`PointLight`), `/Script/UMG.UserWidget`, a Blueprint asset path or its `_C` generated-class path, a plugin mount | **refused** |
| `filepath` | a path on **disk** — `filePath`, `outputPath`, `sourcePath`, `destinationPath` | **none, deliberately** |

All three accept exactly the JSON shapes `string` accepts, so retyping an existing `string`
declaration changes no shape verdict. They differ from `string` only in the rule column, and from
each other only there.

**`filepath` carries no rule because a UNC path normalises to `//server/share`.** Typing a disk
path `path` starts refusing valid input from callers who were working. This is the one place a
find-replace across the registry is actively wrong, and no lint can catch it: the linter knows
names, and `filePath` typed `path` satisfies it exactly as `filepath` would. Read the verb.

**`Contains("//")` is the whole rule, and nothing wider is correct.**
`FPackageName::IsValidLongPackageName` refuses a leading-slash-less short name *and* refuses `.`
(it is in `INVALID_LONGPACKAGE_CHARACTERS`, `NameTypes.h:197`). Using it on a class reference
would refuse `PointLight`, `/Script/UMG.UserWidget` and `/Game/BP/BP_X.BP_X_C` — roughly half the
shapes `ClassUtils::ResolveUClass` documents, across ~45 verbs. `classref` exists precisely so a
class slot gets the lethal rule and no other. The lethality does not depend on a leading slash or
on a dot: `ResolveName2` returns immediately with no delimiter, and `StaticLoadObjectInternal`
re-enters itself with `InName + "." + GetShortName(InName)`, so `LoadObject(nullptr,
TEXT("A//B"))` is an editor kill too.

**An array of paths is `path|array`, and the rule reaches its string elements** (the refusal names
the index). Note the trade-off before spelling it: a union widens the *shape* the slot accepts, so
a slot declared plain `array` starts accepting a bare string as well. Where that matters, leave it
`array` — element typing is a separate ticket.

**Where the refusal sits.** `FRpcDispatcher::ValidateHandlerParams` runs
missing-required → unknown-name → declared-type → **path-separator** → nested-key. The path rule is
below the shape pass because it is about the *content* of a value that is already the right shape —
telling a caller who sent `assetPath` as an object that "it contains //" is not an answer they can
act on. It is above the nested-key pass because it is the only refusal in the chain that prevents
process death rather than a wrong answer. It reuses `INVALID_ARGUMENT`, and the message **quotes
the offending value**, which is a contract other tests assert on, not a nicety.

**What this does not cover, and never will.** The gate sees only the top level. A path nested one
level down — `foliageTypes[].meshPath`, `nodes[].texturePath`, `stems[].assetPath`,
`captures[].attribute` — has no declared type of its own, and `FParamSpec::NestedKeys` (an untyped
allow-list of KEYS) cannot be given one: the material case puts the path in the value half
(`texture: {ParamName: AssetPath}`), where a key-shaped rule cannot reach it. A class reference
inside IR source text is worse — it is a substring of `text`, which must stay typed `string`. Both
are closed at the point of use instead; see §24. Do not approximate either with a name heuristic.

**The ratchet.** `PinWright.infra.contract.PathParamTypes.PathShapedParamsDeclareAPathType` scans
the handler trees and fails on any parameter whose name ends in `path`/`paths` that declares a
non-path type outside its baseline. A new verb spelling `RPC_PARAM_REQ("assetPath", "string", ...)`
goes red there, and nowhere else — `string` and `path` are byte-identical on every payload without
a `//`, so no response-shape test can tell them apart.


---

## 24. When the boundary cannot see the value, guard the load — `PinWrightGuardedLoad::LoadObjectChecked`

§23 is the right layer for a top-level parameter and the wrong one for two input shapes, both
confirmed reachers rather than hypotheses:

- **A nested value.** A path inside an array element, or in the VALUE half of a map. It is not a
  parameter, so it has no `Type` to declare.
- **IR source text.** An AGIR / BTIR / CRIR / BPIR / MGIR / MSIR / NIR / SCIR class or asset
  reference is a substring of `text`. Typing `text` as `path` is nonsense; typing it `classref`
  refuses every valid program. And no IR tokenizer filters `/` — the comment character is `#`, and
  `Unquote` returns its input verbatim — so `class=/Game//X.X_C` arrives at the load intact.

**The rule: a load whose path did not arrive as a top-level `path`/`classref` parameter goes
through `PinWrightGuardedLoad::LoadObjectChecked` (`Utils/GuardedLoad.h`).** It is a drop-in for
`LoadObject<T>(nullptr, *Path)` — same result for every string that was ever going to resolve,
`nullptr` instead of a dead editor for the one that was not — and its optional `OutRefusal`
out-param lets a call site say *why* rather than folding the refusal into its own not-found
message. Fill your existing error from it; do not invent a second vocabulary.

**A guard keyed on where the lethal call happens cannot be bypassed by input shape**, because it
never looks at the input's shape. That also covers the third case nobody declares: a string this
plugin composed itself.

This does **not** license retyping a `path` parameter back to `string`. The boundary refuses the
request with the value quoted, before any work happens; this refuses one load, deep inside a verb
that may already have created something. Defence in depth, in that order.

**Sanitizing is not an option.** A caller who wrote `//` asked for a package that does not exist;
collapsing it silently makes the verb report success about a *different* asset.

**The ratchet.** `PinWright.infra.contract.LoadGuard.IrCompilersLoadThroughTheGuard` holds every IR
compiler tree at **zero** unguarded loads — a load argument that is a string literal is exempt,
decided by reading the arguments rather than by naming the site.
`PinWright.infra.contract.LoadGuard.NestedValueFilesDoNotRegrow` holds the confirmed nested-value
files to a measured count, so a new nested reacher cannot land quietly in a file that already has
top-level-parameter loads. Both are source scans: `CreatePackage`'s `Fatal` means a test that drove
an unguarded site would end the suite host instead of reporting a red.
