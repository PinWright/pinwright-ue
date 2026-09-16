# audio.synth

Sample-level procedural audio: a JSON recipe is rendered into an in-memory candidate buffer, iterated on against measured numbers, auditioned out loud, and exported as a USoundWave. Use this namespace to make a sound that does not exist yet; reach for `call("audio.authoring")` to wire a sound that already exists into a SoundCue, MetaSound or submix.

## Cold start

No preset library ships here, so the entry path is `call("audio.synth.describe_schema")` plus [`audio.synth.cookbook`](audio.synth.cookbook.md). The schema is the machine-readable grammar — generator/effect kinds, parameter types, units, ranges, required/default state, envelope/curve/modulation/normalize forms, caps, target metrics, and a validated example — while the cookbook supplies concrete starting values.

`describe_schema` is sectioned because the whole grammar does not fit one response. `section: "overview"` (the default) is the compact topology plus caps, defaults, strictness rules and the example; `generators`, `effects`, `envelopes` and `targets` are the drill-downs; `kind` narrows a generator or effect section to a single entry; `section: "all"` deliberately exceeds the inline threshold and comes back as a spill file.

## The recipe is a fixed topology

It is neither a free node graph nor a closed template enum:

```
recipe { version, seed, sampleRate, durationMs, layers[<=8], master, targets }
  layer  { startMs, gainDb, pan, generator, ampEnvelope[], pitchEnvelope[], modulation, fx[<=4] }
  master { fx[<=6], normalize { mode, target }, fadeInMs, fadeOutMs }
```

Six generator kinds (`osc`, `noise`, `modal`, `formant`, `granular`, `sample`) and fifteen effect kinds. Every generator is mono; the stereo mix bus and the layer's `pan` are the only stereo placement in the system, which is why `width` is rejected in a layer chain and accepted only on the master. Layer effects process the layer's mono signal before pan; master effects process the stereo bus.

Strictness is uniform and deliberate. An unknown JSON key, an unrecognised enum value, an out-of-range number, an over-cap array and a mismatched pair of `modal` mode arrays are all validation errors naming the exact field path (`layers[2].fx[1].kind`), never clamps and never a silent fallback to a plausible default. A clamped recipe would not round-trip, and a recipe that renders something other than what it names is the one defect an agent cannot see.

## Determinism, and what `seed` is for

The same recipe and `seed` render a byte-identical buffer in one session or after an editor restart. There is no clock, cycle counter, unseeded RNG, engine-global read, or layer-order dependence: each layer uses a substream hashed from the root seed and layer index, and granular grain N's jitter depends on N alone. Raising `densityHz` therefore inserts grains instead of re-rolling existing ones.

The practical consequence: `seed` is a reproducible variation knob, not a source of drift. Re-render one recipe under three seeds to get three takes of the same design, and quote the seed whenever you report a candidate. `audio.synth.variations` does that systematically, jittering named numeric paths and optionally the seed, and it is deliberately **not** an optimizer: it renders a set and returns a metric table beside the ids, and the choice is yours. `audio.synth.generate` and `audio.synth.patch` score recipe `targets` against requested analysis and report each as `met`, `missed` or `unscored`; neither verb searches parameter space or ranks sounds for you.

## Candidates are memory, not files

A render lands in the session candidate registry as an id. It is bounded by total bytes and count (one second of 48 kHz stereo float is about 384 KB) and reclaims the least-recently-*touched* candidate first; reads count as touches. Eviction includes its reason, distinguishing "evicted, re-render" from a bad id.

`audio.synth.list_candidates` publishes current byte usage beside the budget, so pressure is visible before eviction happens; `audio.synth.discard` frees space explicitly and reports per id what actually happened. Audio never becomes a loose file. `audio.synth.export` writes a USoundWave asset under `/Game` and then verifies it by decoding the created asset back through a different subsystem than the one that wrote it, comparing frames, rate, channels and an RMS/peak signature, so the confirmation is not a readback of a field the writer just set. Re-exporting the same candidate to the same path rewrites that wave's audio in place rather than creating a second asset, and keeps every property that is not the payload - SoundClass, attenuation, concurrency, submix and bus sends, modulation, loading behaviour, compression type, looping, volume. The only things this subsystem writes to disk are analyzer PNGs.

## The second render entry point: an existing MetaSound

`audio.synth.render_metasound` renders a `UMetaSoundSource` asset offline instead of a recipe, and lands its samples in the same candidate registry. That is the only way to *measure* a MetaSound rather than read its topology: `compile_metasound` reports `valid: true` for a graph whose three wave players all fire at once just as readily as for one staged 0.6 s apart, and `describe_metasound` / `decompile_metasound` can only show you the wires. Render it, then point `audio.analysis.analyze` at the candidate id and read the onset positions.

It needs no audio device, no PIE session and no editor world, and it runs the graph through the same `CreateSoundGenerator` path the audio mixer uses on playback — so what it measures is what the game hears. Two renders of one source plus one `audio.analysis.compare` is the whole "is the per-shot variation audible" check.

## Cross-cluster overlap

`call("audio.analysis")` measures buffers and reference sounds and owns the other half of the iteration loop. `call("audio.authoring")` owns SoundCue / MetaSound / SoundClass / SoundMix / submix asset authoring, which is what happens to a sound once it exists. `call("audio")` owns runtime playback and the mix stack in a live editor or PIE world. This namespace owns only sample-level synthesis and the candidates it produces.

## See also

- [`audio.synth.cookbook`](audio.synth.cookbook.md) for per-family recipes, starting values, target ranges, and the traps that bite recipe authors.
- [`audio.analysis`](audio.analysis.md) for the metrics you patch against and for turning a reference sound into a draft recipe.
- [`audio.authoring`](audio.authoring.md) for wiring the exported SoundWave into playable assets.

### audio.synth.describe_schema

The schema is generated from the parser/serializer spec tables, so its ranges are enforced and its defaults are materialized on parse. Serialized recipes are longer because optional values become explicit for round-tripping. Start with `section: "overview"`: it carries the caps, strictness rules, and validated example.

`kind` is validated, not ignored: a misspelled kind is rejected instead of silently returning the whole listing.

### audio.synth.audition

The editor preview device is a single shared slot. Starting an audition displaces whatever other editor preview was playing (a Content Browser preview, a previous audition), so the response reports a measured `previous` block plus `stopWith`, the exact call that undoes what this call did. Use it; do not guess at a stop call.

No PIE session and no editor world are needed, which is what makes this usable in an otherwise headless authoring loop. `assetPath`, `candidateId` and `stop` are mutually exclusive. `candidateId` needs the candidate registry to be present in the build and is rejected with `ASSET_NOT_FOUND` when it is not, so a plain id failure and a missing-subsystem failure do not look alike.

### audio.synth.generate

The workhorse. It renders, registers a candidate, and reports what the render *measured* - per-layer frames mixed plus the pre-gain mono-layer peak after the generator, envelope and layer FX (`peakLinear`, also named `peakLinearPreGain`) and the post-gain/equal-power-pan peak across both stereo layer channels (`peakLinearPostGain`), the samples the final clamp actually moved, and the level the normalizer keyed on. Use `render.layers[].peakLinearPostGain` for layer balance; the pre-gain number is deliberately retained as a generator diagnostic. Read `render.normalize.measured` before `render.normalize.inputDb`: the default is not a level.

When the recipe has `targets`, the response also carries `targets[]` rows with `metric`, optional flat inclusive `min`/`max` bounds, `tolerance: 0` (there is no extra comparison epsilon), the exact finite `measured` value when analysis produced that metric, `met` when it was measured, and `status` (`met`, `missed`, or `unscored`); `targetsMet` counts the rows whose status is `met`. Target misses are report data, not handler errors, and unavailable metrics stay `unscored` without a fabricated `measured` or `met` value.

`analyze` defaults on and returns the SUMMARY analysis - scalars only, no per-frame series. `plots` defaults to none; ask for `waveform`, `spectrogram` and/or `constantq` when you need to *see* the sound, and the response hands back file paths, not pixels. The images are the only thing this namespace writes to disk.

Re-sending an identical recipe converges onto the candidate it already produced and says `reused: true`, so a retry after a timeout cannot fill the registry with copies. That also means the seed is part of the identity: change it and you get a new candidate.

### audio.synth.patch

Iteration without losing the take you are comparing against. The patch applies to a fresh serialization of the source candidate's CANONICAL recipe - every optional value made explicit - and renders the result as a new candidate; the source is never touched, and `sourceStillResident` is a measured lookup rather than a promise, because adding a candidate can push another out under the byte budget.

Pointer paths address the canonical form, so fetch the recipe (or read a `generate` response) rather than guessing: `/layers/0/gainDb`, `/layers/0/generator/params/frequencyHz`, `/master/normalize/target`. All six RFC-6902 operations work. The two failure codes are different on purpose: `INVALID_PARAMS` means the patch document is wrong (fix the patch), `VERIFICATION_FAILED` means a `test` op did not hold (re-read the recipe). A patch whose result is not a legal recipe errors with the parser's field path and registers nothing.

### audio.synth.variations

A sweep, not a search. It renders N seeded perturbations and hands back a table of ids and metrics; nothing scores, ranks or selects, because the judgement about which take sounds right is the one thing a metric cannot make for you. Audition or export the id you pick.

`mutations` names numeric pointer paths and a jitter (`relative` as a fraction of the current value, `absolute` as a delta in its own unit); `varySeed` re-rolls every noise, grain and jitter decision and is usable on its own for takes of one unchanged design. A sweep with neither is refused rather than returning N copies of the base recipe. A variation whose perturbation walks a parameter outside its schema range fails as one row, with its code, and the rest of the sweep still lands.

It answers with a job ticket and renders afterwards - poll `system.job_status` with the `ticket_id` - and it registers no cancel callback, so `system.job_cancel` will tell you `JOB_CANCEL_UNSUPPORTED` rather than pretend to stop it. Every argument is checked before the ticket is issued, so a mistyped pointer is a plain error, not a failed job.

### audio.synth.export

The only way audio leaves this namespace, and it leaves as a `USoundWave` asset - there is no file export anywhere in the subsystem. After writing, the verb decodes the created asset back through `USoundWave::GetImportedSoundWaveData` and compares frames, sample rate, channel count and a per-channel RMS/peak signature against the candidate's buffer. That parse path is a different subsystem from the writer, so `verification.pass` is a real comparison rather than a readback of what was just set; the per-channel deltas are published either way, so a failure says by how much.

`save: false` marks the package dirty and says so (`saved: false`, `pendingFlush: true`) instead of claiming durability. Re-exporting the same candidate to the same path rewrites that wave's audio in place and converges - which is what makes a retry safe. The rewrite keeps every property that is not the payload (SoundClass, attenuation, concurrency, submix and bus sends, modulation, loading behaviour, compression type, looping, volume, sound group) and refreshes only what the old audio determined: duration, format, cue points, channel layout, timecode. `verification.propertiesPreserved` measures that half - it compares every non-payload `UPROPERTY` as exported text before and after, and a rewrite that moved one fails the call with `VERIFICATION_FAILED` and names it in `verification.changedProperties` rather than answering `pass: true`. The response also reports `routing.soundClass` / `routing.attenuationSettings` as they stand after the write. A wave this verb *creates* has neither, because nothing has set them - an unrouted wave escapes every SoundMix, duck and class volume and plays at full level at any distance, and nothing in the editor flags it, so assign them with two `property.set` calls (`SoundClassObject`, `AttenuationSettings`) plus `asset.save` before shipping it.

### audio.synth.render_metasound

The one verb that turns a built MetaSound graph into samples. Everything else in `audio.authoring` describes the graph you just wrote; this measures what it does. `durationSeconds` is required and is a CEILING, not a length: a one-shot that finishes earlier stops there and `render.finishedAtSeconds` says when, while a source still producing at the ceiling reports `render.truncated: true` so you know you are looking at a prefix.

Read `render.requestedSampleRate` against `render.effectiveSampleRate` before quoting a duration — an asset carrying a `SampleRateOverride` runs its operator at its own rate, and the buffer carries the effective one. `render.sourceChannels` is the graph's output width (1 for a Mono source); the top-level `channels` is always 2, because the buffer is deinterleaved stereo and a mono graph is duplicated into both sides. A source resolving to more than two channels is refused with `AUDIO_MULTICHANNEL_UNSUPPORTED` rather than silently folded.

`inputs` overrides graph inputs for this render only — nothing is written back to the asset. A JSON number becomes a **Float**, which is what most MetaSound inputs are; an Int32, or any type JSON cannot distinguish, needs `{"type":"int","value":3}`. MetaSound validates overrides against the graph's own vertices and drops what it does not recognise *silently*, so the response measures the outcome: `inputs.applied` and `inputs.rejected` are name sets, and a name you expected under `applied` appearing under `rejected` means the override never reached the render.

A graph that fails to build is not an error here. The engine substitutes a no-op operator, so it comes back as a success whose report reads `finished: true` at the first block with `analysis.technical.digitalSilence` set — visible and nameable, which a `METASOUND_RENDER_FAILED` would not be. `METASOUND_RENDER_FAILED` means something else: the graph never ran at all, most often because `au.MetaSound.EnableAsyncGeneratorBuilder` could not be driven to 0 and the render would otherwise have begun with a scheduling-dependent number of silent blocks.

`name` + `path` are optional and go together. They write the samples as a `USoundWave` through the same writer `audio.synth.export` uses, behind the same non-modal overwrite gate, and verify it by decoding the created asset back. Omitting them costs nothing: the `candidateId` already feeds `audio.analysis.*`, `audio.synth.audition` and `audio.synth.export`.
