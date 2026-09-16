# Response budget: what spills, and why

Capture responses are large, and a response over the display threshold is written to disk and replaced by a reference the caller then has to read. This page says which call shapes fit, what the gate measures, and what a spill reference publishes.

A tool result larger than the display threshold (10,000 characters by default) is written to `Saved/PinWright/HttpResponses/` and replaced by a short reference, costing the caller an extra file read. **Which number is compared against that threshold used to be wrong in three compounding ways**, and the correction is worth knowing because it decides whether your capture stays inline:

1. The wrapper was measured **pretty-printed** — tab indent and CRLF line ends — while the transport has always written the wire body condensed.
2. The payload was counted **twice**: MCP asks a tool returning `structuredContent` to also return an equivalent text block for clients that do not read structured output, so the same object rides in both. A reader sees whichever one their client renders — never both.
3. Escaping that pretty copy into a JSON string cost **two characters per character** of whitespace nobody asked for.

Measured on 147 real spilled responses: a capture whose payload was **4,618** characters reached the gate as **14,228** (3.1×), and **92 of 92** single-still captures spilled — on the verb the mandatory vision-verification loop runs dozens of times per asset. None was a real overflow. The gate now measures **one condensed copy of the payload a reader sees**. A spill reference publishes both numbers: `file.characters` is what was measured, `file.fileCharacters` is what is on disk (larger — the whole wrapper, pretty-printed, payload twice).

Two response-shape savings land with it. `viewport.previewScene` stops emitting `previous` / `afterRestore` / `restore` when no rig was requested and the restore was clean — with nothing changed they restate the drawn rig and announce a restoration of nothing (1,114 characters). See [`render.preview-scene-rig`](render.preview-scene-rig.md).

**`views: "sides"` is the one call shape that still spills, and that is a decision rather than an oversight.** Its payload measures ~10.9 KB against the 10 KB budget, of which **6.0 KB is six per-shot measurement blocks** — `imageStats`, `framing`, the placed pose. The only ways to fit it are to cut measurements or to take fewer than six views; the first is how a shaped response gets mistaken for a complete one, and the second is not `sides`. So a six-view review call is expected to spill **once per asset**, while every other shape of every capture verb now stays inline. If you want the review inline, take the views in two calls, or pass `measureCoverage: false` and accept losing the one signal that catches an empty frame.
