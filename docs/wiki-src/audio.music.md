# audio.music

Procedural music turns a score into one seamless PCM stem per track, exports the stems as `USoundWave` assets, then builds an interactive MetaSound Source for runtime layering. Use `audio.synth` for one-shot effects.

## The pipeline

```
score JSON  --render_stems-->  stem candidates  --export_stems-->  USoundWave assets  --build_interactive-->  MetaSound Source
   (schema)                    (in memory)                          (under /Game)                              (playable bed)
```

**Read the schema first.** No preset library ships: `call("audio.music.describe_schema")` plus this page is the cold-start path. Its sections are `overview` (topology, beat convention, caps, required fields), `rules`, `example`, `scales`, `roles`, and `modes`; `mode: "<name>"` opens one mode's full table.

**An instrument is a synth layer**, not a parallel model. A track's `instrument` uses the same `{generator, ampEnvelope, pitchEnvelope, modulation, fx}` object and validation as `call("audio.synth.describe_schema")`; new SFX generators are therefore available here. Because the score owns timing and mix, an instrument rejects `startMs` (use the note's `startBeat`), `gainDb`, and `pan`.

**One beat is one `timeSignature.denominator` note**, and `bpm` counts those per minute. A bar is `numerator` beats; there is no `4/denominator` correction. Thus 6/8 at 90 bpm has 666.67 ms eighth notes and 4 s bars. This is Quartz's convention, not MIDI's. Only denominators 2, 4, 8, 16, and 32 are accepted; `/1` is rejected rather than left unschedulable.

**Two caps apply.** Rendering accepts at most 16 tracks; the interactive graph accepts at most 8 stems because the engine's Audio Mixer node family tops out there. A 12-track score can export 12 assets but needs multiple Sources or pre-mixing. The schema publishes both limits.

**Stems use the shared candidate registry.** Candidate operations such as `call("audio.synth.audition")`, `call("audio.analysis.analyze")`, and `call("audio.synth.list_candidates")` also work on stems.

## Bare SoundWave loop metadata is inert

**Nothing at runtime reads a `USoundWave`'s loop metadata.** `FSoundWaveCuePoint` and `GetLoopRegions()` serialize and survive reload, but no decoder or mixer source consults them. Only the MetaSound Wave Player's `Loop` / `Loop Start` / `Loop Duration` pins honor loop points; a bed relying on wave metadata does not loop.

**Stems are seamless by construction.** `render_stems` renders past the loop point and wraps the overhang onto the head: `stem[i % LoopFrames] += stem[i]` for every `i >= LoopFrames`. No fade is applied; fading would duck the bed and erase the wrapped tail at each seam.

**`build_interactive` is the graph stage.** The Wave Player is the runtime that can loop a region. If a project already has a dynamic-music graph, stop after `export_stems`; the ordinary SoundWaves work with any MetaSound, SoundCue, or Quartz scheduler.

Exact PCM wrapping is necessary but not sufficient: block compression (Vorbis, Opus, ADPCM, Bink) pads to whole blocks and the decoder's first block ramps in, so it does not preserve a sample-exact seam. Keep a seamless stem PCM or give the Wave Player real loop points.

## Quartz drives it, and authoring stops at the asset

`build_interactive` produces an asset whose controls — `Intensity`, `Section`, `Play`, `Stop` — are **ordinary MetaSound parameters**. It does not produce a music system or promise every input is wired: use `graphInputs` and `warnings`. Scheduling is **core engine** Quartz; no plugin or `.uplugin` entry is needed:

- `UQuartzSubsystem::CreateNewClock` makes a clock and gives it a tempo and time signature. `FQuartzTimeSignature` is `{NumBeats, BeatType}` where `BeatType` *is* the denominator, which is why the score's beat convention matches it exactly.
- `UQuartzClockHandle::SubscribeToQuantizationEvent` fires a delegate on the bar, the beat, or any other `EQuartzCommandQuantization` boundary — the hook for changing `Intensity` on the downbeat instead of mid-phrase.
- `UAudioComponent::PlayQuantized` starts the component *on* a quantization boundary, which is how a second bed enters in time with the one already playing.

This namespace defines the music asset; the game decides when layers are audible. Setting `Intensity` from C++ or Blueprint works immediately but changes wherever the playhead is; Quartz quantization makes the transition land deliberately.

The stems remain plain SoundWaves: use them in another MetaSound, or put one in a Wave Player with `Loop` enabled and skip this verb.

## An inexact loop is reported, not hidden

A loop must be a whole number of frames. `bars x beats x 60000/bpm x sampleRate/1000` often is not; the verb reports that instead of silently rounding a click into the result.

Every render therefore reports three things together:

| field | meaning |
|---|---|
| `loop.exact` | whether the loop lands on a whole frame at all |
| `loop.residualFrames` | the **signed** distance from the unrounded length to the rounded one |
| `loop.nearestExactBpm` | the tempo that *would* be exact at this bar count and sample rate |

`residualFrames` is signed so long and short errors differ. `nearestExactBpm` is recovery guidance, usually within a hundredth of the requested bpm, for a sample-exact seam.

Exactness belongs to the whole score, not the tempo: 140 bpm in 4/4 at 48 kHz is inexact for one bar but exact for seven. Measure each score.

## Cross-cluster overlap

`call("audio.synth")` owns one-shot recipes and the shared candidate registry; `call("audio.analysis")` owns metrics, decomposition, and reference comparison; `call("audio.authoring")` owns general MetaSound/SoundCue graph authoring; `call("audio")` owns live playback and mixing. This namespace owns only score → stems → bed.

## See also

- [`audio.synth`](audio.synth.md) for the generator and effect grammar an instrument is written in, and for the candidate registry's eviction rules.
- [`audio.synth.cookbook`](audio.synth.cookbook.md) for concrete generator choices and starting values — an instrument is a layer, so the cookbook's advice applies verbatim.
- [`audio.authoring`](audio.authoring.md) for editing the generated MetaSound Source further, and for wiring the stems into anything else.

### audio.music.describe_schema

The grammar comes from the parser/serializer's static tables, so documented ranges and defaults are live: ranges error at that boundary, while defaults are materialized on parse and make echoed scores longer.

Read `section: "overview"` first: it has topology, beat convention, caps, and required fields. `section: "rules"` adds defaults, strictness, and the seamless-loop contract. `section: "example"` is a worked score; `exampleValidated` is the live parser verdict.

`modes` returns digests (name, summary, required parameters) because four full tables exceed the response ceiling. Pass `mode: "evolving_pad"` for one complete table. `section: "all"` deliberately exceeds the inline threshold and returns a spill file.

Strictness is uniform: an unknown scale, key root, track role, generation mode, or object key is rejected with the valid set; nothing is clamped. Errors name a JSON path such as `tracks[2].notes[17].degree` for the patch/resubmit cycle.

### audio.music.render_stems

The renderer is a job: a score may contain 16 tracks of 512 notes, and it creates one instrument instance *per note*. The response is a ticket whose table lands in `system.job_status`. No cancel callback exists; `system.job_cancel` returns `JOB_CANCEL_UNSUPPORTED`. The score is fully parsed **before** ticket creation, so malformed input returns a real error code and parser field path rather than a bare job failure.

Read `exact`, signed `residualFrames`, and `nearestExactBpm` first; an inexact loop is reported for you to decide.

Read `dropped` in each stem row. It counts notes that contributed nothing, from the mixer's return rather than an assumption; dropped, quiet, and rendered notes can otherwise look the same in a success-only report. Common causes are a pitch outside `formant`'s roughly 2 kHz `f0Hz` ceiling, or off-tonic notes on a `noise` percussion track, which the generator rejects rather than detuning silently.

`peakDb` is after track gain/pan and **may exceed 0 dBFS**. Nothing normalizes: the six generators have no shared output-level convention or correct absolute target. Trim with `tracks[].gainDb` and re-render.

The wrapped overhang is each note's voice length plus effect tail, but a tail is counted only when `ampEnvelope` **ends at exactly 0**. A non-zero final value or absent envelope never silences the generator, so extra frames would be more note, not decay. A `convolve` tail equals the impulse response length, unavailable without loading the asset, so it is truncated at voice end. For a reverb cut at the loop, end the amp envelope at 0 or shorten the tail.

A stem candidate is audio, not a recipe. It shares `audio.synth`'s registry, so `audio.synth.audition`, `audio.synth.export`, and analysis verbs accept its id; `audio.synth.patch` does not, because the stem has no recipe. Patch the score and re-render.

Re-sending an identical score reuses only if every prior candidate id is re-resolved through the registry and still resident: the response says `reused: true` and adds none. A reuse omits per-stem counts because it measured nothing; the creating render reported them. One eviction makes the whole set miss and re-render, avoiding a falsely complete partially-evicted result.

`mixdown: true` registers one extra candidate containing the unity-gain sum of all stems; it is off by default because it costs another full registry buffer.

### audio.music.export_stems

Exports one stem candidate per track to a `USoundWave`, naming assets from track names with optional `prefix`. There is no file export; the result is an asset path plus frames, rate, and channels.

A job writes up to 16 waves and decodes each for verification. Candidate ids and names are resolved/validated **before** the ticket, preserving distinct codes: `CANDIDATE_EVICTED` means re-render, `NO_CANDIDATES` means none has been generated, and `CANDIDATE_NOT_FOUND` means a wrong id. `system.job_cancel` returns `JOB_CANCEL_UNSUPPORTED`.

Verification decodes through `USoundWave::GetImportedSoundWaveData`, independently parses RIFF, and compares frames, sample rate, channels, and an RMS/peak signature with the candidate buffer; it does not merely read fields the writer set. A failed stem row reports its worst delta, the job fails with counts, and the candidate is **not** marked exported.

Re-runs rewrite the audio in place rather than create `_1` siblings; `updatedInPlace` counts occupied paths. The rewrite keeps every property that is not the payload - SoundClass, attenuation, concurrency, submix and bus sends, modulation, loading behaviour, compression type, looping, volume, sound group - and refreshes only what the old audio determined: duration, format, cue points, channel layout, timecode. A rewrite that moves anything else fails that stem's row with `VERIFICATION_FAILED` naming the properties, rather than reporting a verified stem.

Names are validated, not sanitized: a name the sanitizer would rewrite is rejected. Case-insensitive duplicates are rejected because `Pad` and `pad` are one Windows file.

Leave the waves PCM if they must loop seamlessly. See the compression caveat above.

### audio.music.build_interactive

Builds stem assets into one interactive MetaSound Source. It is not a job, but is tick-unsafe (asset creation plus package save), so the safe-point gate may run it a tick later; the response is unchanged.

**At most 8 stems.** The Audio Mixer node family registers 2 to 8 inputs per channel layout and nothing wider. A 9-to-16-track score can render/export but is refused *here*, not silently pre-mixed; split it across Sources or mix before export.

Every `assetPath` loads before creation, so an unresolvable path errors with **no asset behind**. This precedes occupancy resolution because `overwrite: true` deletes the target; discovering a bad stem later would destroy a good asset for an impossible build.

Per stem, only `assetPath` is required. `inputName` defaults to `Stem<index>`; duplicates error because one layer would be uncontrollable. `loopStartSeconds` and `loopDurationSeconds` default to 0, which the Wave Player reads as *loop the whole asset*—correct for a `render_stems` stem. Set them for a region of a longer wave. `intensityThreshold` defaults to 0 (audible at any intensity) and must be 0..1.

The response reports measurements: `nodesAdded` and `connectionsMade` are document node/edge deltas, not call counts; `graphInputs` is what the finished graph exposes; `warnings` includes exposed-but-unconsumed inputs. `measured` is false when nothing was produced, so clean counts cannot describe a no-op. Persistence is checked on the created asset, not inferred from a true return.

Read `graphInputs` for ordinary MetaSound parameters to drive; schedule writes on a Quartz clock when they must land on a bar (see above).
