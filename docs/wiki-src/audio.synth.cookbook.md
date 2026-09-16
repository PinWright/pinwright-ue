# audio.synth.cookbook

Concrete recipes for the four SFX families the `audio.synth` recipe grammar is built to cover, with starting values, target metric ranges, and the traps that cost the most iterations. Read `call("audio.synth.describe_schema")` for what is legal and this page for what is good.

## Read this part first

Five facts decide whether a first attempt fails on structure or on sound, and none is guessable from the grammar.

**`gainDb` is relative to six different references.** Generators share no output-level convention, so a layer's `gainDb` is a trim against whatever its generator hands the mix bus:

| generator | what 0 dB means |
|---|---|
| `osc` | Waveform amplitude, peak 1.0 by construction. A unison stack divides by voice count, so eight voices are not eight times louder. Predictable. |
| `noise` | The colour's own convention, RMS-referenced not peak-referenced. `brown` carries a make-up gain restoring white's RMS, so it matches in RMS and overshoots 1.0 in peak. |
| `modal` | Per mode: each resonator's input is divided by the peak of its own impulse response, so with the `impulse` exciter a mode's peak sample amplitude **is** its `modeGainsDb`. The bank's sum has no bound; many loud modes add. |
| `formant` | Peak-normalized to exactly 1.0 as the last step, because the bank's output swings tens of dB with vowel, `f0Hz` and `formantShift`. The only generator whose level is fixed regardless of its parameters. |
| `sample` | The source asset's own level, mono-collapsed. Whatever was recorded. |
| `granular` | The source's level divided by the mean overlap (`densityHz * grainMs / 1000`, floored at 1), so density is not a second volume knob. |

So the same `gainDb` on a `formant` layer and on a `sample` layer is not the same loudness, and nothing downstream repairs it. Absolute level is master `normalize`'s job; layer `gainDb` is for balance *between* layers, and you set it by rendering and reading `render.layers[].peakLinearPostGain`. That field is the maximum absolute sample across both stereo channels after gain and equal-power pan. The legacy `peakLinear` (also named `peakLinearPreGain`) is the pre-gain mono-layer peak after the generator, envelope and layer FX, so it does not move when only `gainDb` changes.

**Envelope times are layer-relative.** `ampEnvelope[].timeMs` and `pitchEnvelope[].timeMs` are measured from the layer's `startMs`, and the layer's span is `durationMs - startMs`. A point past that span is a validation error, not a clamp: a layer at `startMs: 400` in a 1000 ms recipe has a 600 ms span, so a breakpoint at 800 ms is rejected. Points must be non-decreasing. Omitting the envelope means no shaping at all, so the layer runs at unity for its whole span and clicks at both edges unless the generator decays on its own.

**`width` is master-only.** Every generator is mono and the mix bus owns stereo placement through the layer's `pan` (equal-power). A `width` effect in a layer chain is rejected by the parser and again by the renderer, since mid/side on a mono buffer would be a silent no-op.

**Caps are validation errors, not clamps.** 8 layers, 4 layer effects, 6 master effects, 1 to 64 envelope points, 1 to 32 modal modes, 1 to 60000 ms duration. Over any of them is an error naming the field path and the limit. That is what makes a recipe round-trip: parse then serialize returns the same recipe with defaults made explicit, so patching serialized output is safe.

**There is no automation on effect parameters.** No filter-cutoff envelope, no LFO you can point at an arbitrary knob. Only three things move over time: `ampEnvelope`, `pitchEnvelope`, and the layer's one `modulation` block (`fm`, `am` or `ring`). A moving pitch envelope on a `noise` layer is an error, not a no-op, because noise has no pitch for it to act on. For a *filtered sweep* use an `osc` layer with a pitch envelope, or three to four time-offset layers each with its own static filter cutoff (a stepped sweep is usually enough), or a `sample` / `granular` source that already contains the motion.

Four more surface only at render time: `filter` has no `mix` row and is always fully wet; `ringmod` publishes `rateHz` 0.1 to 20000 but the engine carrier only reaches 10 to 10000 Hz, and outside that it is rejected rather than clamped; `pitchshift` with `formantPreserve: true` is an `UNSUPPORTED_OPTION` error, since there is no phase vocoder behind it; and `normalize` in `lufs` mode needs roughly 500 ms of render before the loudness meter produces a result, so short one-shots must normalize by `peak` rather than silently degrading to it.

## The loop

1. `call("audio.synth.generate", { recipe })` renders into a session candidate and returns its id, the render report (per-layer frames and pre/post-gain peaks, clamped samples, normalizer input and applied gain) and, by default, the summary analysis. An identical recipe converges back onto the candidate it already produced.
2. Read the analysis. Numbers first (peak, crest, attack, decay, centroid, flatness, band split), then a `plots` PNG when the numbers do not explain what the user is reacting to.
3. `call("audio.synth.patch", { candidateId, patch })` with RFC-6902 operations against the **canonical** recipe (every optional value made explicit, so `/layers/0/generator/params/exciterMs` exists even if you never wrote it). The source candidate is untouched and the result is a new candidate, so both takes stay comparable. A patch whose result does not parse errors with the parser's field path and creates nothing.
4. Repeat. Audition only when a human is in the loop: the editor preview slot is shared, and starting one displaces whatever else was previewing.

`targets` never changes the render. When analysis is requested, the response carries one `targets[]` row per inclusive metric range with `metric`, optional flat `min`/`max`, the exact finite `measured` value when that metric was available, `tolerance: 0` (no extra epsilon beyond the range), `met` only when measured, and `status` (`met`, `missed`, or `unscored`); `targetsMet` counts only `met` rows. A metric the signal cannot express (a decay time on a sustained tone) is `unscored`, never a failure against zero, and has no fabricated `measured` or `met` field. With `analyze: false`, target rows remain explicitly `unscored` because no measurement was requested.

When the user already owns a sound they like, start from `audio.analysis.to_recipe` rather than from scratch: it fits the reference and emits a draft recipe, which with no preset library in this system is the nearest thing to a preset there is.

## Impacts and destruction

The `modal` generator is the whole family: a bank of exponentially decaying resonators, one per entry of three same-length arrays, and **those three arrays are the material**.

**Frequency ratios choose the object class.** Small-integer ratios (1, 2, 3, 4, 5) read as a struck bar or a plucked string: pitched, musical, one clear note. Free-free bar ratios (1, 2.756, 5.404, 8.933, 13.34) read as struck metal, where a pitch is audible but does not sit in a key. Ratios sharing no small-integer relation read as a bell: a real bell's partials sit near 0.5, 1.0, 1.2, 1.5, 2.0 of its strike note, and the 0.5 hum plus the 1.2 minor-third tierce are what separate it from a gong. Dense, slightly irregular frequencies with no dominant partial read as a plate or a sheet.

**Decay times choose the material.** Under about 120 ms reads as wood, plastic or a dull thunk; 300 to 800 ms as ceramic or glass; 1 to 6 seconds as metal, with a bell needing its low modes to outlast the high ones several times over. Give higher modes shorter decays in every case: equal decays across the bank sound synthetic, because real damping rises with frequency.

**Gains choose brightness.** With the `impulse` exciter a mode's peak sample amplitude is exactly its `modeGainsDb`, so the array is a literal spectrum rather than a suggestion. A metal impact wants its upper modes within 15 to 20 dB of the fundamental; a dull thunk wants them 25 dB or more down.

**The exciter is a spectral decision, not a loudness one.** All three are normalized to agree with a unit impulse in the band they share and differ only in what they do above it.

- `impulse` is one sample of 1.0: flat spectrum, every mode excited equally, the brightest and most synthetic option. `exciterMs` is unused, because a single sample has no length.
- `strike` is a raised-cosine mallet pulse of `exciterMs` scaled to unit area, i.e. unit momentum: identical low-frequency drive to the impulse, top rolled off. The first spectral null sits at 1 kHz for a 2 ms strike and at 200 Hz for a 10 ms one. That is the hard-hammer versus soft-beater difference, and it is how you dull a hit without touching `modeGainsDb`.
- `noise` is a windowed white burst at unit energy: the impulse's average level, with per-mode scatter. That scatter is what makes repeated hits sound like repeated hits instead of one hit played twice, so use it for anything that fires more than once.

A modal layer usually needs **no `ampEnvelope`**: the mode decays are the envelope. Add a short master `fadeOutMs` anyway, so a still-ringing tail does not click at the buffer end.

A struck metal bar, ready to render:

```json
{
  "durationMs": 2200,
  "seed": 3,
  "layers": [
    {
      "gainDb": -6,
      "generator": {
        "kind": "modal",
        "params": {
          "modeFreqsHz":  [850, 2343, 4593, 7593, 11340],
          "modeGainsDb":  [0, -4, -9, -14, -20],
          "modeDecaysMs": [1800, 1300, 900, 600, 350],
          "exciter": "impulse"
        }
      }
    },
    {
      "gainDb": -14,
      "generator": { "kind": "noise", "params": { "color": "white", "lowCutHz": 2000, "highCutHz": 16000 } },
      "ampEnvelope": [
        { "timeMs": 0, "value": 1, "curve": "exp" },
        { "timeMs": 25, "value": 0 }
      ]
    }
  ],
  "master": {
    "fx": [ { "kind": "reverb", "params": { "decayMs": 900, "mix": 0.12, "dampingHz": 6000 } } ],
    "normalize": { "mode": "peak", "target": -1 },
    "fadeOutMs": 20
  },
  "targets": {
    "peakDb": { "min": -1.5, "max": -0.5 }, "attackMs": { "max": 5 },
    "decayMs": { "min": 900, "max": 2200 }, "centroidHz": { "min": 2000, "max": 5000 },
    "flatness": { "max": 0.15 }, "crestDb": { "min": 14 }
  }
}
```

The second layer is the contact noise: without it the modal bank starts too cleanly and reads as a synthesizer. 20 to 30 ms of band-limited noise, 8 to 15 dB under the bank, is enough.

Variants from the same shape:

- **Wooden thunk.** `modeFreqsHz [180, 411, 690, 1123]`, `modeGainsDb [0, -6, -11, -17]`, `modeDecaysMs [90, 62, 40, 26]`, `exciter: "strike"`, `exciterMs: 8`. Contact noise `lowCutHz: 20, highCutHz: 1500`, 18 ms. Master reverb `decayMs: 350, mix: 0.08`. Targets `decayMs 60-160`, `centroidHz 400-1200`, `flatness 0.10-0.35`, `crestDb 10-18`.
- **Glass.** `modeFreqsHz [2400, 3810, 5220, 7050]`, `modeGainsDb [0, -5, -10, -16]`, `modeDecaysMs [700, 520, 380, 240]`, `exciter: "impulse"`. Targets `centroidHz 3000-6000`, `decayMs 300-800`, `flatness < 0.12`.
- **Bell.** `modeFreqsHz [220, 440, 528, 660, 880, 1320]`, `modeGainsDb [-6, 0, -3, -5, -8, -14]`, `modeDecaysMs [6000, 4500, 3200, 2600, 1800, 1100]`, `durationMs: 7000`. The 220 Hz hum under the 440 strike note is the entry that makes it a bell.
- **Destruction (many hits).** Three to five modal layers at different `startMs` (0, 40, 95, 170, 260 ms), `exciter: "noise"` so no two hits are identical, the frequency array scaled per layer (0.94, 1.07, 1.13), and decreasing `gainDb`. Add a `granular` debris layer when a source wave exists.

## Explosions and weapons

No new generator: this family is **layering plus envelopes plus FX**, and the recipe's job is to keep four bands from fighting. One layer each for sub thump, body, crack and tail, with different attack times.

- **Sub thump.** `osc` sine at 45 to 60 Hz with a `pitchEnvelope` falling 12 to 16 semitones over the first 200 to 400 ms; the falling pitch is what makes a sine read as impact rather than as a test tone. `ampEnvelope` 0 to 1 in 20 to 40 ms (not instant: an instant sub attack is a click, not weight), then to 0 over 500 to 900 ms. `distort` `type: "soft", drive: 3` adds the harmonics that let the thump survive on speakers with no bottom octave.
- **Body.** `noise` `color: "brown"`, `lowCutHz: 20`, `highCutHz: 1200`, attacking in 3 to 5 ms and decaying 700 to 1200 ms on an `exp` curve. A layer `filter` `lowpass` at 500 to 900 Hz, `resonance: 1`, sets distance: lower cutoff is further away.
- **Crack.** `noise` `white` or `blue`, `lowCutHz: 800`, `highCutHz: 18000`, 0 to 1 in 1 ms then to 0 by 80 to 120 ms. This is the transient the ear locates the event by; when the explosion sounds soft, this layer is too quiet or too slow, not the body. `distort` `hard`, `drive: 15-20`, `mix: 0.6` for a weapon; no distortion for a distant blast.
- **Tail.** The master `reverb` carries it: `decayMs 2000-3500`, `mix 0.30-0.45`, `dampingHz 3000-5000` distant; `decayMs 600-1200`, `mix 0.10-0.18`, `dampingHz 8000` close. A `granular` or `noise` debris layer starting at 150 to 300 ms with a 1.5 to 3 s decay sells rubble.

Distance moves five things at once: distant means little or no crack, a body cutoff under 700 Hz, a longer wetter reverb with heavier damping, a slower attack, and a lower centroid. Close is the opposite of every one. Do not make distance with volume alone; the analysis will show nothing else moved.

Targets, distant explosion: `peakDb -1.5..-0.5`, `crestDb 8-14`, `attackMs 15-60`, `decayMs 1200-3000`, `centroidHz 200-900`, `flatness 0.35-0.70`, `lufs -18..-12`. Close weapon: `crestDb 14-22`, `attackMs { max: 5 }`, `decayMs 300-900`, `centroidHz 900-2500`, `clippedSamples { max: 0 }`. Watch `clippedSamples` here specifically: four layers peaking together is the usual route to a clipped bus, and the fix is layer `gainDb` balance, not a lower `normalize` target.

## Creatures and voices

The `formant` generator is a classical source-filter model: a Rosenberg glottal pulse through a parallel bank of five vowel formants. **`f0Hz` and `formantShift` are independent**, and that independence is the entire point.

`f0Hz` is pitch, how fast the folds vibrate. `formantShift` is body size, the length of the vocal tract. The shipped vowel table is a bass voice, the longest human tract, so `formantShift` above 1.0 walks up through smaller human voices and below 1.0 keeps going into creature territory: the same vowel at the same pitch becomes a mouse or a bull purely by moving it. A pitch shifter cannot do that, because it drags the formants along and produces chipmunks. Use `pitchEnvelope` freely; it multiplies `f0Hz` and never touches the bank.

`voicing` and `breathiness` set the source mix of pulse versus aspiration noise, and both at 0 is silence and is rejected. `breathiness` also drives cycle-to-cycle jitter and shimmer, 0.4% / 0.25 dB at 0 up to 2.5% / 2.5 dB at 1. That wobble is most of what separates a living throat from a synthesizer, so a growl wants `breathiness` at 0.4 or above even when it should not sound breathy.

**Be honest about what this generator cannot do.** There is no subharmonic or diplophonic mode, so a growl comes out *rough* but never *torn*: grain and irregularity, not the period-doubled tearing of a real roar. And there is one static vowel per layer with no interpolation, so anything past roughly a second is a held note rather than a cry with a shape. Both are structural, not tuning problems, and neither is worth iterating against.

Work around them by layering. For a torn roar: a `formant` layer for the voiced core, a `noise` layer band-passed 300 to 3000 Hz with its own irregular envelope for the tear, and `distort` `type: "tube"`, `drive: 8-15`, `mix: 0.5` on the formant layer for the grunge the model omits. For a vowel that moves: two or three `formant` layers on different vowels with overlapping `startMs` and crossfading `ampEnvelope`s (three 300 ms layers with 100 ms overlaps read as one 700 ms utterance). Past that, synthesis is the wrong tool: run `sample` or `granular` over a recorded vocalization and treat `audio.synth` as the processor rather than the source.

A large creature growl:

```json
{
  "durationMs": 900,
  "seed": 11,
  "layers": [
    {
      "gainDb": -4,
      "generator": {
        "kind": "formant",
        "params": { "f0Hz": 72, "vowel": "o", "formantShift": 0.62, "voicing": 0.85, "breathiness": 0.45 }
      },
      "ampEnvelope": [
        { "timeMs": 0,   "value": 0,    "curve": "exp" },
        { "timeMs": 90,  "value": 1,    "curve": "linear" },
        { "timeMs": 620, "value": 0.85, "curve": "exp" },
        { "timeMs": 900, "value": 0 }
      ],
      "pitchEnvelope": [
        { "timeMs": 0, "semitones": 2 }, { "timeMs": 900, "semitones": -3 }
      ],
      "fx": [
        { "kind": "distort", "params": { "type": "tube", "drive": 9, "mix": 0.45 } },
        { "kind": "filter",  "params": { "type": "lowpass", "cutoffHz": 4500, "resonance": 0.9 } }
      ]
    },
    {
      "gainDb": -18,
      "generator": { "kind": "noise", "params": { "color": "pink", "lowCutHz": 300, "highCutHz": 3000 } },
      "ampEnvelope": [
        { "timeMs": 0, "value": 0 }, { "timeMs": 120, "value": 0.7, "curve": "scurve" },
        { "timeMs": 700, "value": 0.4, "curve": "exp" }, { "timeMs": 900, "value": 0 }
      ]
    }
  ],
  "master": {
    "fx": [ { "kind": "eq", "params": { "lowGainDb": 4, "midGainDb": 0, "highGainDb": -3, "lowHz": 180 } } ],
    "normalize": { "mode": "peak", "target": -1.5 },
    "fadeInMs": 5,
    "fadeOutMs": 25
  },
  "targets": {
    "peakDb": { "min": -2, "max": -1 }, "centroidHz": { "min": 400, "max": 1400 },
    "flatness": { "max": 0.40 }, "crestDb": { "min": 6, "max": 14 },
    "attackMs": { "min": 40, "max": 150 }
  }
}
```

Size variants on the same layer: a small chittering creature is `f0Hz: 320`, `formantShift: 1.7`, `vowel: "i"`, `durationMs: 140`, pitch envelope rising 5 semitones. A mid-size predator is `f0Hz: 140`, `formantShift: 0.95`, `vowel: "a"`. A very large one is `f0Hz: 50`, `formantShift: 0.52` (0.5 is the floor) and needs the EQ low shelf to keep the fundamental audible.

Check `pitch.f0Hz` and its confidence in the analysis. When pitch was suppressed for low confidence the layer carries too much noise and reads as a hiss rather than a voice: lower `breathiness`, lower the noise layer's `gainDb`, or both.

## Spells, magic and UI

`granular` for texture, `osc` sweeps for motion, the FX chain for the sense of a place. There is no "magic" generator; magic is a source with no obvious real-world identity plus motion too smooth or too fast to be physical.

**Granular** is the texture engine. `grainMs` 20 to 60 with `densityHz` 40 to 120 gives a continuous shimmer; `grainMs` 5 to 15 with `densityHz` 8 to 25 gives sparse particles you can hear individually. Sweep the read head with `positionStart` / `positionEnd` (0.1 to 0.9 across the layer), `positionJitter` 0.15 to 0.35 so it does not sound like a tape scrub, `pitchJitterCents` 200 to 600 to spread it into a cloud, and `reverseChance` 0.25 to 0.5 for the unplaceable direction. Raising `densityHz` changes texture without changing level.

**Sweeps** are `osc` plus a `pitchEnvelope`, the one family where a large pitch envelope is the point. Rising 24 to 36 semitones over 300 to 600 ms reads as a charge-up; falling reads as dissipation or a portal closing. `waveform: "triangle"` for clean, `"saw"` under a lowpass for force. `unison: 4` with `unisonSpreadCents: 25` thickens a sweep without a second layer and without raising the level.

**The FX chain does the rest.** `delay` at `timeMs` 140 to 260, `feedback` 0.35 to 0.55 gives the repeating tail that reads as arcane, and `dampingHz` 4000 to 6000 makes each repeat duller than the last, which is what stops it sounding mechanical. `reverb` at `decayMs` 1500 to 3000, `mix` 0.25 to 0.40 places it in a large space. `phaser` (`rateHz` 0.3-1.2, `depth` 0.6, `stages` 6) or `chorus` (`rateHz` 0.4, `depthMs` 8, `voices` 4) keeps a sustained magic sound from feeling static. `pitchshift` at `semitones: 12`, `mix: 0.3` adds sparkle; leave `formantPreserve` at its `false` default, since `true` is rejected.

Order is where authors lose time: distortion and filtering belong on the layer chain, where they hear the dry layer, while reverb, delay and width belong on the master, where they hear the finished mix. Four effects is the layer cap and six the master cap, so a five-effect layer chain is a validation error, not a slow render.

**UI sounds are the same family at a twentieth of the length.** A click is `osc` sine or triangle at 900 to 1600 Hz with a 1 ms `exp` attack and a 30 to 60 ms decay, plus a `noise` layer high-passed at 4 kHz lasting 8 to 15 ms at 12 dB down. A confirm is two such layers a fifth apart, the second starting 60 ms later; an error is the same two a semitone apart, together. Keep the master chain nearly empty (`reverb` at `mix: 0.05`, `decayMs: 250` at most): an audible tail makes an interface feel slow.

Targets, magic one-shot: `attackMs 5-40`, `decayMs 800-2500`, `centroidHz 1500-5000`, `flatness 0.20-0.50`, `crestDb 8-16`, `stereoCorrelation { min: 0.2, max: 0.9 }` for a wide but mono-compatible image. UI: `durationMs { max: 250 }`, `attackMs { max: 3 }`, `peakDb { min: -3, max: -1 }`, `crestDb 10-18`, `clippedSamples { max: 0 }`.

Width is the last step: place layers with `pan` first (three at -0.4, 0, +0.4 are already wide), then add a master `width` of 1.2 to 1.6 if it still needs it. Check `stereoCorrelation` afterwards; below about 0.1 the sound partially cancels when a player's setup folds to mono.

## See also

- [`audio.synth`](audio.synth.md) for the namespace, the candidate registry, and how determinism and `seed` work.
- [`audio.analysis`](audio.analysis.md) for what each metric means, why a missing metric is missing, and the reference-to-recipe path.
- [`audio.authoring`](audio.authoring.md) for turning the exported SoundWave into a SoundCue, MetaSound or attenuated source.
