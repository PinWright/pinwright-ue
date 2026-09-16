# mrq

Drive Movie Render Queue — Unreal's offline render pipeline for cinematics — by building and
inspecting `UMoviePipelineExecutorJob` entries from `ULevelSequence` + map + optional preset,
running selected queue jobs under `UMoviePipelinePIEExecutor`, and enumerating available
`UMoviePipelinePrimaryConfig` presets.

Use this namespace after authoring a sequence via `sequencer`; for live-viewport capture without offline rendering, prefer `render` instead.

## Availability and naming

Conditional availability: the handler file `Handlers/MRQ/MRQHandler.cpp` is gated on `__has_include(<MoviePipelineQueueSubsystem.h>)` plus its sibling MRQ headers. When the engine's MovieRenderPipeline plugin is disabled in the target, all six methods are still registered but respond with `MRQ_NOT_AVAILABLE`: `create_job`, `run_jobs`, `list_jobs`, `remove_job`, `clear_queue` and `list_presets`. Builds opt the modules in through `TryAddConditionalModule` in `PinWright.Build.cs` for `MovieRenderPipelineCore` / `Editor` / `Settings` — there are no hard module dependencies to manage.

Important UE 5.6 naming: the primary config class is `UMoviePipelinePrimaryConfig`, NOT `UMoviePipelineMasterConfig`. The rename landed in UE 5.2 and there is no back-compat alias. Older docs and tutorials still reference the master-config name; trust the header on disk.

## `mrq.create_job` path validation

`mrq.create_job` validates `sequencePath`, `levelPath`, and an optional `presetPath` before it
looks up or mutates the shared queue. Each must be a valid long package/object path on a currently
mounted Unreal root; malformed or unmounted paths return `INVALID_PATH` and create no job. The
sequence is then resolved through the registry-backed asset resolver and must be an initialized
`ULevelSequence` with a `MovieScene`; an uninitialized sequence returns `SEQUENCE_INVALID` before
queue allocation. The map must resolve to a loaded `UWorld` map asset, including a transient in-memory map. Missing assets return `ASSET_NOT_FOUND`,
wrong types return `ASSET_WRONG_TYPE`, and neither case creates a job.
When package-only spellings are supplied, the queued job stores the resolver's canonical object
paths so MRQ receives the concrete sequence and map assets rather than package-only soft paths.

The preset's `UMoviePipelineOutputSetting::OutputDirectory` is also checked before allocation.
`{project_dir}` is expanded for the check, and a temporary file probe verifies that the nearest
existing ancestor is writable; nested output directories do not need to exist yet. An empty,
traversal, file-valued, file-ancestor, or unwritable output directory returns `INVALID_PATH` and
leaves the queue unchanged.

## Long-running pattern

`mrq.run_jobs` is the canonical example of the manual `StartJob` pattern in this plugin. The handler:

1. Validates queued jobs exist (`QUEUE_EMPTY` if not), validates `jobs` as distinct positional
   queue indices when supplied, and resolves `executorClass` before starting a ticket. An
   unloadable class returns `CLASS_NOT_FOUND`; an abstract class or a class outside the
   `UMoviePipelineExecutorBase` hierarchy returns `INVALID_EXECUTOR_CLASS`.
2. Calls `Ctx.StartJob(Args)` to allocate a ticket and immediately respond with `running` to the HTTP caller.
3. Creates the executor and binds `OnExecutorFinished` before starting it. With a `jobs` selection,
   it duplicates the queue, enables only the requested copies, removes unselected copies in
   reverse index order, and retains that transient queue until completion; the shared queue is
   never mutated, and temporary enabled state is restored if the executor finishes synchronously
   or fails to start.
4. Calls `QSS->RenderQueueInstanceWithExecutorInstance(SelectedQueue, Exec)` for a selection, or
   the shared queue when no selection is supplied. The selected queue is necessary because the UE
   5.8 PIE executor validates every job it receives, including disabled jobs.

The ticket result includes per-job exit status. A PIE executor that reports success without any
rendered output returns `success: false` with `errorCode: RENDER_NO_OUTPUT`. A job whose complete
decoded sample is near-uniform returns `RENDER_UNRENDERABLE_FRAMES` unless the caller explicitly
sets `allowUnrenderableFrames: true`; an executor without per-job artifact callbacks remains
explicitly unmeasured. See [system.job_status](system.md) for the polling pattern callers use to
watch the ticket.

## Shared queue and job management

The queue returned by `UMoviePipelineQueueSubsystem::GetQueue()` is editor-global and persists for
the editor session. `mrq.create_job` appends one entry; it does not replace or clear earlier
entries. Its `queueSize` counts the whole queue, including jobs created by other calls or through
the MRQ editor UI. The response always carries `queuedJobs` for entries that existed before this
call, each with `index`, `jobName` and `sequencePath`; when that array is non-empty, `warnings[]`
states the count and warns that enabled entries among them may also be rendered by `mrq.run_jobs`
unless a selection is provided. Creation is refused with `MRQ_RENDER_IN_PROGRESS` while an
executor is active, so appending cannot race the executor's shared queue state.

Use `mrq.list_jobs` before acting on a shared queue. It returns `jobs[]` with the current positional
`index`, `jobName`, `enabled`, `sequencePath`, both `mapPath` and the backwards-compatible
`levelPath`, `presetPath`, `configurationPath`, the resolved `outputDirectory` and `fileNameFormat`,
and a `preflight` block read from the job's resolved configuration. These indices are positional,
not stable IDs, and can change after a removal.

By default `mrq.run_jobs` renders every enabled queue entry. Pass `jobs: [index, ...]` to render
only those entries; the handler renders a transient copy containing only the requested jobs,
preserving the shared queue's job settings and enabled state. An empty selection, duplicate index,
or out-of-range index is refused before a ticket is started. `mrq.remove_job` deletes one current
`jobIndex`; `mrq.clear_queue` deletes all entries. Both management verbs are refused while an
executor is active because the queue is shared state, and both return the resulting `queueSize`.

## What `mrq.create_job` tells you before the render is spent

Two defects lived on the queue path, and both were the ticket's shape one step earlier.

**An unloadable `presetPath` is now refused, not queued.** It used to be a soft failure: the job was queued with **no configuration at all**, a `UE_LOG` warning went to the editor log that no caller reads, and the response echoed `presetPath` byte-identically to the success case. The render then ran on the engine's CDO defaults — `Quality`/CRF 20, into a directory the caller never chose — for minutes, while the caller's only evidence said their preset applied. It now answers `MRQ_PRESET_NOT_LOADABLE` and queues **nothing**, so the shared queue is left exactly as it was. Refusal rather than a warning, for reasons a reviewer can check: this same handler already hard-errors `CLASS_NOT_FOUND` for an unloadable `executorClass`; an unconfigured job is a supported state *only* when the caller asked for it by omitting `presetPath`; and the cost is one-directional — refusing costs a corrected path now, proceeding costs minutes of render and a deliverable that leaves the tool. Omit `presetPath` to queue deliberately unconfigured; call `mrq.list_presets` to see what exists.

**The response now carries a `preflight` block**, read off the configuration the queued job actually holds after the preset was copied into it — never echoed from the request. That is also what replaces a `presetApplied` boolean: the disclosure shows the preset's own settings, so it cannot claim a preset took effect that did not.

| Field | Provenance |
|---|---|
| `resolution` — `width`, `height` | the resolved `UMoviePipelineOutputSetting` |
| `outputDirectory`, `fileNameFormat` | the same setting's format strings, published **unresolved** — MRQ expands `{project_dir}`, `{sequence_name}`, `{frame_number}` at render time, from state that does not exist at queue time |
| `frameRateOverride` | present only when the config overrides the sequence's rate (`bUseCustomFrameRate`); omitted rather than guessed, since the sequence is not loaded here |
| `outputs[]` | class path of every **enabled** file writer on the config. Empty means this job writes nothing |
| `encoderRequested` | the same reflection read-back `mrq.run_jobs` publishes, off the resolved video output; absent when no video output is configured |
| `warnings[]` | present only when non-empty |

Two warnings fire here, and both are cheaper here than after the render. A config with **no enabled output setting** will write no files at all while `mrq.run_jobs` still reports `success: true` for it. And a `rateControl` of `Quality` or `ConstantQP` is unbounded below — the same trap described in the next section, named before the minutes are spent rather than after.

**Not implemented: the `minBitsPerPixel` / `minBitrateMbps` param** the ticket also proposed. It was judged out of scope, and deliberately so: rewriting a caller's resolved config to `VariableBitRate` at a bitrate PinWright derived from a floor would make the plugin the one applying settings nobody asked for — this ticket's own defect, inverted. Disclosure plus the warning gives a caller everything needed to change one property on the preset asset themselves.

## What the terminal result says about the artifact

`mrq.run_jobs` used to resolve with `{"success": true}` and nothing else — not even the path of the file it wrote. A 1080p60 cinematic that encoded at 1.16 Mbps and shipped visibly banded passed every check a caller could run (resolution, frame count, duration, container magic) because the one number that separated it from its 21.24 Mbps re-render was never published (`B-mrq-render-result-omits-bitrate-and-size`).

The result now carries a `jobs` array, one entry per rendered job:

| Field | Provenance |
|---|---|
| `outputFiles[]` — `path`, `renderPass`, `exists`, `fileSizeBytes` | paths from `FMoviePipelineOutputData`; `exists`/`fileSizeBytes` **stat'd** per file |
| `outputFileCount`, `measuredFileCount`, `totalFileSizeBytes` | measured; `totalFileSizeBytes` omitted when nothing was stat'd |
| `frameCount`, `frameRate`, `durationSeconds` | the shot's own `WorkMetrics` / `CachedFrameRate`, not the sequence asset |
| `resolution` | the resolved `UMoviePipelineOutputSetting` |
| `overallBitrateBps` = `totalFileSizeBytes * 8 / durationSeconds`, `bitsPerPixel` = bitrate / (w·h·fps) | derived from a measured size; **not demuxed** from the container |
| `encoderRequested` — `class`, `rateControl`, `constantRateFactor`, `averageBitrateMbps`, `maxBitrateMbps` | read by reflection off the resolved video output setting — a **request**, not an outcome |
| `shots[]` — `name`, `state` | each shot's own `ShotInfo.State` at the moment it reported its work finished |
| `warnings[]` | present only when non-empty |

Two rules the shape encodes. **Anything unmeasurable is omitted, never zeroed** — a path the pipeline reported but that is not on disk gets `exists: false` and no `fileSizeBytes`, and a render with no frame count publishes no `durationSeconds` or `overallBitrateBps`. And **measured is named plainly while requested is named `encoderRequested`**: on the engine's shipped default (`Quality`, CRF 20) the encoder settings place no lower bound on the achieved bitrate at all, because the Media Foundation quality branch sets an encode QP and never a mean or max bitrate.

Two warnings are worth acting on. An encode below **0.04 bits/pixel/frame** (4.98 Mbps at 1080p60, 1.24 Mbps at 720p30, 19.9 Mbps at 4K30) is flagged as implausible — the delivered bad file measured 0.0093 bpp. And a `rateControl` of `Quality` or `ConstantQP` is flagged as unbounded below, which is the trap on fog, night, smoke, underwater and other smooth-gradient shots; set the video output to `VariableBitRate` with an explicit `AverageBitrateInMbps` for those.

`jobs` is **absent** — with `artifactWarning` in its place — when a non-PIE `executorClass` was requested, since `OnIndividualJobWorkFinished` is declared on `UMoviePipelinePIEExecutor` alone. An empty array would read as "the render wrote nothing", which is a different claim from "nothing was measured".

## What the terminal result says about the PICTURE

Everything in the table above is a property of the **file**, and that was the next defect. Four 3840x2160 PNGs of 8.8 MB each, all `exists: true`, all within 0.8 % of each other in size, at 8.53 bits/pixel — comfortably above the 0.04 bits/pixel floor — came back `jobSucceeded: true` with no warnings. The lower 47 % of every one of them was a flat pale-grey void where the ground should have been (`B-mrq-run-jobs-succeeds-on-unrenderable-frames`). A large flat region compresses well, but 8 million pixels of PNG keeps the byte count plausible, so no size-derived number can see it.

**`jobSucceeded` means "the pipeline ran to completion and wrote its files".** It is not a verdict on the pixels and never was. The verdict on the pixels is this:

| Field | Provenance |
|---|---|
| `imageAnalyzed` on each `outputFiles[]` entry | present on **every** entry, including the ones that were not opened — a video container, a file that was not on disk, a frame outside the sample. `imageNotAnalyzedReason` names which |
| `imageStats` — `meanLuminance`, `luminanceVariance`, `minLuminance`, `maxLuminance`, `litPixelCount`, `litPixelFraction`, `litLuminanceThreshold`, `toneLevelsUsed`, `toneLevelMinPixels`, `meanRed`, `meanGreen`, `meanBlue`, `redVariance`, `greenVariance`, `blueVariance` | luminance fields use the same `PinWrightRenderCapture::CalculateCaptureImageStats` the `render.capture_*` verbs publish; channel means and population variances are measured in normalized 0–1 RGB over the same decoded pixels |
| `imageStats.flatRegionFraction`, `flatRegionLevel`, `flatRegionBounds{minX,minY,maxX,maxY}`, `flatBlockFraction` | the **spatial** measure — the largest 4-connected region of low-local-variance blocks, as a share of the frame, with its representative luminance and bounding box in normalised top-left-origin coordinates |
| `blank`, `crushed`, `blownOut` | the shipped whole-frame verdicts, meaning exactly what they mean on `render.capture_open_level` |
| `suspect`, `suspectReasons[]` (`FLAT_REGION` / `BLANK` / `CRUSHED` / `BLOWN_OUT`) | the one field an automated acceptance pipeline can gate on |
| `uniformColor`, `unrenderable` | per decoded frame; true when the variance of every RGB channel is at most `(2/255)^2`, allowing small dither while rejecting a black, magenta, or other single-colour frame |
| `framesAnalyzed`, `framesSuspect`, `framesUnrenderable`, `allAnalyzedFramesUnrenderable` | per job; published even when zero, because "no frame was opened" must be readable |

**Why a spatial measure and not another aggregate.** On a frame that is half good picture and half void every whole-frame statistic stays healthy — the good half carries a normal mean, a normal variance and dozens of tone levels — so `blank`, `crushed` and `blownOut` are all false *and correct*. Only "how much of this frame is one contiguous low-variance field" changes. Each block is classified by local luminance standard deviation rather than min/max equality, so sparse dither does not fragment the reported shallow-gradient void. Regions stay anchored to a bounded band around their seed block's luminance, which stops an ordinary long sky gradient from chaining across the frame.

**Partial suspect content warns; a uniformly unrenderable sample refuses.** A large flat region can be a legitimate sky, backdrop, title card, or letterbox bar, so that observation remains a warning and `flatRegionBounds` lets the caller judge it. When every decoded sample from any job has near-zero variance in all three colour channels, however, the terminal ticket fails with `RENDER_UNRENDERABLE_FRAMES`; the failed result keeps `jobs[]`, `outputFiles[]`, and every measured statistic. Use `allowUnrenderableFrames: true` only when a uniform render is intentional; the result then succeeds but keeps `unrenderableFramesDetected: true` and adds `unrenderableFramesWarning`.

**It does not name the cause, and cannot.** An unconverged Nanite / virtual-shadow-map stream, geometry that never loaded into the PIE world, a GPU timeout mid-accumulation and a matte backdrop all write the same pixels. The one causal discrimination that comes free is published instead: **where in the render the suspect frames fall**. Every measured frame suspect ⇒ not first-frame convergence, and more warm-up will not fix it. Only the earliest frames suspect ⇒ that is exactly what an unconverged start looks like.

**Sampled, not exhaustive.** At most **8** frames are decoded per job — evenly spaced, first and last always included — because opening every frame of a long render costs game-thread time after the caller has already waited minutes. Whenever fewer frames were opened than were written, a warning says so explicitly: `framesSuspect` then describes the sample, not the render.

## What the executor itself reported

`executorErrors[]` — `fatal`, `message`, `jobName` — carries everything broadcast on `UMoviePipelineExecutorBase::OnExecutorErrored`, which nothing used to publish. This is not redundant with `success`: `OnExecutorFinishedImpl` broadcasts `!bAnyJobHadFatalError`, and that flag is set only on the **fatal** branch. A non-fatal executor error therefore leaves `success: true` and used to reach the caller nowhere at all — it went to the editor log, which no RPC caller reads. When that combination happens, `executorWarning` names it.

Per job, `shots[]` carries each shot's `name` and its pipeline `state` (`Uninitialized` / `WarmingUp` / `MotionBlur` / `Rendering` / `CoolingDown` / `Finished`) at the moment it reported its work finished. A shot that is not `Finished` did not produce every frame it was asked for, and a warning says so — the frame count, the file list and `jobSucceeded` all describe a partial render in that case.

## See also

- [`sequencer`](sequencer.md) — Movie Render Queue consumes `ULevelSequence` assets authored through this namespace.
- [`system`](system.md) — `system.job_status` for polling the `mrq.run_jobs` ticket to completion.
- [`showcase-video`](showcase-video.md) — producing footage meant to be watched: the shared-queue trap, the silent AAC track, and PNG masters over a direct encode.
