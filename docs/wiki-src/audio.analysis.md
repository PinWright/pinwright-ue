# audio.analysis

Measures audio and reports numbers an agent can act on: descriptor metrics, transient / partial / residual decomposition, A-versus-B comparison, a draft recipe fitted to a reference sound, and a folder-wide audit. Use this namespace to find out what a sound actually is; reach for `call("audio.synth")` to change it.

## It works on any SoundWave, not only on generated audio

The input is a USoundWave asset or synth candidate; **any** SoundWave qualifies, whether imported,
shipped, bounced, recorded, or generated here. The measurement path is not specific to plugin-
generated buffers, so it can describe a reference before synthesis.

**There is deliberately no file-reading path.** Import a loose file through the editor as a
SoundWave, then it uses the same decode seam as every other input. One seam keeps "what was
measured" and "what the project ships" on the same bytes; a second WAV path could let them drift.

Decoding a cold wave is not cheap: the payload is a blocking bulk-data load plus an Oodle unpack, and it is game-thread only. Measure once per wave and keep the numbers rather than re-measuring inside a loop.

## Absence is a measurement, and it is explicit

Every metric in the report is either a real measurement or is missing, with no third state. A family that could not be measured is omitted from the response entirely and its reason is listed under a top-level `unmeasured` object, so a report with nothing measurable serializes as a short block of reasons instead of a plausible wall of zeros. Within a measured family, a scalar the signal cannot define is omitted the same way.

The important cases are:

- A sustained tone has no `decayMs`; reporting 0 ms or the buffer length fabricates a value.
- Noise has no pitch. Below the estimator confidence floor, the whole pitch family is suppressed:
  a confident-looking 137 Hz read from a click is worse than no number.
- A band whose low edge is at or above Nyquist is omitted, not reported as 0.0. At an 8 kHz render
  rate there is no 8k-20k band to be empty.
- Digital silence is a **success**, not an error, and returns only the technical family. "This
  rendered, and what it rendered is nothing" differs from "this could not be analysed".

Degenerate inputs are answered where they are detected, in a fixed order: empty buffer, then non-finite samples, then silence. Non-finite before silence is load-bearing, because NaN compares false against every threshold and a NaN-filled buffer checked for silence first would be measured as silence and reported as valid audio.

## What is measured

Six families are reported. **Technical** survives even for silence: duration, rate, frame count,
peak, `peakDb` / `rmsDb`, clipped-sample count, signed `dcOffset`, zero-crossing rate, active
duration, and start/end discontinuities that predict playback or loop clicks. **Envelope** uses a
10 ms RMS block envelope: `onsetMs` (leading silence), `attackMs` (from onset, not sample 0),
`decayMs`, `tailMs` (a subset, not an addition), `temporalCentroidMs`, `crestDb`, and transient
count/onset list. **Loudness** is gated BS.1770. **Spectral** is time-averaged STFT data:
`centroidHz`, `rolloffHz`, `flatness`, `bandwidthHz`, a six-band power split with edges, spectral
flux, and strongest peaks. **Pitch** is `f0Hz`, confidence, motion class, and signed
`semitoneDelta`, so rising and falling differ. **Stereo** is correlation, width, mono compatibility,
and a named silent channel when present.

Deviations are signed or paired wherever they have two failure directions: `dcOffset` is signed rather than absolute, `stereoCorrelation` spans -1 to +1 so an anti-phase image cannot score like an in-phase one, and brightness is a six-way band split rather than one "brightness error" so too dark and too bright move different entries. Metrics whose only direction is "more" (clipped samples, discontinuity magnitude) are deliberately unsigned.

## Two detail levels, and a real size ceiling

The summary form is scalars only, no per-frame arrays, a handful of peaks, and is roughly 1.6 KB as the transport encodes it. The full-detail form adds the onset list, the pitch track, the flux series (each decimated, and flagged when decimated) and the analysis parameters, and may spill to a file. That is a deliberate trade, not a failure.

The MCP result carries the payload twice—escaped in `content[0].text` and verbatim in
`structuredContent`—for roughly 2.35x amplification against the spill gate. The practical inline
ceiling is therefore about 4,250 characters, not the threshold constant's 10,000. Ask for full
detail only when you need the series.

## Images are the perceptual channel

Waveform and spectrogram PNGs are the perceptual channel, with three enforced properties. Scales
are **fixed**, not fitted: otherwise a take 40 dB quieter can look identical and read as "no
change". Axes are **labelled**, so the answer can be "a tone at 1 kHz starting at 250 ms" rather
than "a bright horizontal line". The colormap is **viridis**, monotonic in luminance, so it does
not invent banding that reads as structure.

Images are the one thing here that lands on disk as files. Everything else is an asset or a response.

## Decomposition is shaped like the synthesizer, on purpose

Descriptor metrics answer "is this too bright?" but do not name a synthesis knob. Decomposition
does, using a model intentionally symmetric with `audio.synth`:

- **Transients** map to the noise layer and to the `modal` generator's `exciter`.
- **Partials** map one-to-one onto modal bank rows: a track's mean frequency feeds `modeFreqsHz`, and its fitted initial gain and -60 dB decay feed `modeGainsDb` and `modeDecaysMs`. A track whose amplitude never actually decays reports no fit at all rather than an invented decay time.
- **Residual** (everything the partials did not explain) maps to a filtered-noise layer, one log-spaced band per row with a simplified level envelope.

Harmonic/percussive separation distinguishes energy **horizontal** in the spectrogram from energy
**vertical**. It does not separate instruments or sources: a steadily ringing cymbal can be
harmonic, and bow noise can be percussive. "The drum track" is not a claim this measurement makes.

Cost is refused, never silently trimmed. An input past the duration limit is an error naming both the limit and the measurement, because cropping a 40 s input to 30 s and reporting the result as a decomposition of the input would describe a signal nobody passed.

## Reference to recipe

`audio.analysis.to_recipe` runs that decomposition and emits a draft `audio.synth` recipe. With no
preset library, it is the closest thing to a preset: a reference sound yields modal, transient,
and residual settings fitted to measurements rather than guessed.

Treat the result as a draft: unresolved modes are absent, and anything inexpressible (moving
formant, granular texture, stereo gesture) lands in the residual. The reference's character usually
survives better than its loudness. Follow [`audio.synth.cookbook`](audio.synth.cookbook.md): render,
measure, compare, and patch.

## Cross-cluster overlap

`call("audio.synth")` owns generation, iteration and the candidate registry; this namespace owns measurement and never renders. `call("audio.authoring")` owns SoundCue / MetaSound / SoundClass / submix asset authoring, and `call("audio")` owns runtime playback in a live world; neither measures. A folder audit here is a read-only sweep over existing assets and does not modify them.

## See also

- [`audio.synth.cookbook`](audio.synth.cookbook.md) for the generate / measure / patch loop and the target ranges each SFX family should be measured against.
- [`audio.synth`](audio.synth.md) for the recipe grammar these metrics are patched into.
- [`audio.authoring.describe_sound_wave`](audio.authoring.describe_sound_wave.md) for a SoundWave's asset-level metadata (compression, channels, duration) as opposed to its signal content.
