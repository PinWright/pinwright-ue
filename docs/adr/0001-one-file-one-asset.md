# One `.pwmodel` file produces exactly one asset

A `.pwmodel` file is one model, the way an `.obj` or `.stl` file is one model: no construct inside a
file emits a second `.uasset`. Parts, `materials`, `collision` and `lightmap` all describe the single
output mesh — parts merge into it and carry material assignment; they are not sub-assets. We chose
this because the alternative makes the compiler's contract unstateable: with N outputs, a failure
halfway through leaves some assets written and some not, "recompile this file" becomes a partial
overwrite of an unknown set, and the provenance stamp — one `SourcePath` per asset — stops being a
one-to-one relation.

## Considered options

- **One file, many assets** (a file could declare several models, or a `part` could opt out of the
  merge and bake standalone). Rejected: it buys terseness for authors and costs the all-or-nothing
  guarantee. It also forces a naming scheme for the extra outputs — derived from part names, from a
  block parameter, or from the output path plus a suffix — none of which survive the part being
  renamed.
- **One file, one asset** (chosen). "If anything failed, create nothing" is trivially true rather
  than something to engineer. `model.compile` takes one `outputPath` and returns one `assetPath`.

## Consequences accepted

- A rigged character becomes three or more files (mesh, skeleton, skin, animations), not one.
- `use <kind> from "<path>"` is therefore **mandatory infrastructure**, not a convenience — it is the
  only way a later file kind references a skeleton instead of inlining it. It is reserved in
  `pwmodel 0` for that reason.
- `FPwModelCompileResult::Parts` reports per-part triangle and vertex counts. Those are diagnostics,
  not asset manifests; no reader should treat a part as addressable content.
