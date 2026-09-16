---
type: reference
summary: "Normative reference for PinWright Animation (.pwanim): compiled-skeleton references, timebase and keys, easing, dense baking, loop seams, RPC surfaces, and diagnostic catalog."
date: 2026-08-23
tags: [pwanim, animation, skeleton, file-format, diagnostics, examples]
---

# PinWright Animation (`.pwanim`) - format reference

A `.pwanim` file describes animation keys against one compiled Unreal skeleton. It
does not embed a skeleton or read a skeleton source file. The `use skeleton from`
path must resolve to a mounted `USkeleton` asset.

## Version and document shape

The first non-comment, non-blank line must be:

```
pwanim 0
```

The document contains exactly one skeleton reference, exactly one timebase, and one
or more bone blocks:

```
use skeleton from "/Game/Rigs/Robot"
timebase rate=(30, 1) frames=30 loop=true
sync_marker "L" frame=0
sync_marker "R" frame=15
sync_marker "L" frame=30

bone "upper_arm" ease=ease_in_out {
    key frame=0 rotate=(0, 0, -20)
    key frame=15 rotate=(0, 0, 20) ease=linear
    key frame=30 rotate=(0, 0, -20)
}
```

A line break ends a statement — there is no statement terminator — so **a bone's `{` must
open on the header line**. A `{` on the next line is a new statement, not that bone's block,
and is refused with `PWSRC_BAD_BLOCK` naming the orphaned brace's line.

The asset path is a direct Unreal object path. It is not a relative source path and
does not use extension sniffing. `rate=(numerator, denominator)` is the frame rate;
`frames` is the inclusive final frame. `loop=true` both requests a loop seam check and
sets the compiled `UAnimSequenceBase.bLoop` default to true; `loop=false` suppresses
the seam check and writes a one-shot default. Asset players can still override that
default at playback time.

Each key requires an integer `frame`. `at`, `rotate`, and `scale` are optional vector3
channels. Rotation uses roll-pitch-yaw degrees. Bone names must exist in the referenced
skeleton. Keys within one bone must have unique, ascending frame numbers. A bone block
must contain at least one key.

Sync markers are timeline state and therefore use a top-level statement beside `timebase`:
`sync_marker "name" frame=N`. Their frame is an integer in the inclusive range `0` through
`frames`; a value past the final frame is an error, never a clamp. Marker names must be
non-empty strings that can become Unreal `FName` labels; they do not need to be declared on
the skeleton. Repeating a name at different frames is legal (and repeating it at the same
frame is preserved too), because one label can describe multiple phases of a loop.

The complete set of marker statements replaces the sequence's authored marker set on every
compile. No marker statement means an empty set. This makes the source authoritative and avoids
an omitted live marker being silently re-timed or carried into the generated asset.

The allowed easing values are `step`, `linear`, `ease_in`, `ease_out`, and
`ease_in_out`. A bone-level `ease` supplies the default; a key-level value applies to
the segment beginning at that key. A final key ease has no following segment and is
reported as a warning.

## Recommended source placement

Keep a `.pwanim` directly beside the `.uasset` it generates, under the project's `Content/`
tree, with the same basename:

```text
Content/Characters/Hero/Anim/AS_Hero_Walk.uasset
Content/Characters/Hero/Anim/AS_Hero_Walk.pwanim
```

This is recommended, not mandatory. It makes the source-to-asset mapping obvious, makes an
orphaned source or an unstamped asset visible at a glance, and keeps a rename of the asset and
its source as one coherent operation. Existing layouts keep working by passing an explicit
`outputPath`.

Unreal's asset registry recognizes `.uasset` and `.umap`, not `.pwanim`, and typical Git LFS
rules are extension-based rather than path-based. The source therefore stays plain, diffable
text under `Content/` unless the project has a broader custom rule.

When `outputPath` is omitted, `<ProjectDir>/Content/<Rel>/<Name>.pwanim` derives exactly
`/Game/<Rel>/<Name>`. A source outside that tree, with the wrong extension, or mapping to an
invalid package path is refused with `SOURCE_OUTPUT_PATH_NOT_DERIVABLE`; the message names the
reason and tells the caller to pass an explicit `outputPath`. No default is guessed. The normal
provenance and `overwrite` guard runs after derivation, so a derived target stamped from another
source still refuses unless `overwrite=true`.

## Baking and output

Sparse keys are baked to every frame from 0 through `frames`, inclusive. Missing
channels are filled from the referenced skeleton's reference pose, never from an
identity transform. Translation and scale interpolate component-wise. Rotation uses
quaternion spherical interpolation. A looping animation must meet the reference pose
at its loop seam within the format tolerance; otherwise validation reports the seam
diagnostic. Compiled sequences use Unreal's `Linear` `UAnimSequence.Interpolation`
default because interpolation mode is not a `.pwanim` channel.

`anim.validate` accepts exactly one of `text` or `filePath`. It validates and bakes the
animation without creating an asset. `anim.compile` requires `filePath`; `outputPath` is
optional only for the mapping above. Inline `text` is refused for asset creation. It creates one `UAnimSequence`, reports the
resolved skeleton and baked frame/key counts, and honors the save, overwrite, and
provenance rules of the handler. `anim.describe_ops` reports the key, timebase, bone, and
sync-marker parameter vocabulary.

The canonical worked source is
[`Examples/pwanim/robot_arm_wave.pwanim`](../Examples/pwanim/robot_arm_wave.pwanim).

## Diagnostics

Shared lexical, value, and generic source-shape failures use `PWSRC_*` codes:

| Code | Meaning |
|---|---|
| `PWSRC_UNEXPECTED_CHARACTER` | A character cannot begin a source token. |
| `PWSRC_UNTERMINATED_STRING` | A quoted string reaches end of input without closing. |
| `PWSRC_INVALID_STRING` | A string literal has invalid contents. |
| `PWSRC_INVALID_NUMBER` | A numeric literal is malformed. |
| `PWSRC_MISSING_VERSION` | The required format header is absent. |
| `PWSRC_UNSUPPORTED_VERSION` | The header version is not supported. |
| `PWSRC_UNEXPECTED_TOKEN` | A token is not valid at the current grammar position. |
| `PWSRC_UNCLOSED_BRACE` | A block was opened but not closed. |
| `PWSRC_UNKNOWN_OP` | An operation name is not in the format vocabulary. |
| `PWSRC_UNKNOWN_PARAM` | A parameter is not accepted by the current operation. |
| `PWSRC_DUPLICATE_PARAM` | A parameter was supplied more than once. |
| `PWSRC_MISSING_PARAM` | A required parameter is absent. |
| `PWSRC_BAD_TUPLE_ARITY` | A tuple has the wrong number of values. |
| `PWSRC_BAD_VALUE` | A value has the wrong type or is outside its allowed domain. |
| `PWSRC_BAD_BLOCK` | A block is not valid for the current construct. |
| `PWSRC_RECOMPILE_UNMANAGED_STATE` | Recompile or takeover would discard live state not reproduced by the incoming source. Error by default; warning when `overwrite=true` explicitly permits the loss. |

Animation-specific diagnostics are:

| Code | Meaning |
|---|---|
| `PWANIM_WRONG_FORMAT` | The source is not a valid animation document. |
| `PWANIM_MISSING_TIMEBASE` | No timebase was declared. |
| `PWANIM_DUPLICATE_TIMEBASE` | More than one timebase was declared. |
| `PWANIM_MISSING_SKELETON` | No skeleton reference was declared. |
| `PWANIM_DUPLICATE_SKELETON` | More than one skeleton reference was declared. |
| `PWANIM_UNSUPPORTED_USE_KIND` | The `use` kind is not `skeleton`. |
| `PWANIM_SKELETON_NOT_AN_ASSET_PATH` | The skeleton reference is not a direct asset path. |
| `PWANIM_SKELETON_NOT_FOUND` | The referenced skeleton asset could not be found. |
| `PWANIM_SKELETON_WRONG_KIND` | The referenced asset is not a `USkeleton`. |
| `PWANIM_SKELETON_HAS_NO_BONES` | The referenced skeleton has no bones. |
| `PWANIM_NO_BONES` | The document contains no bone blocks. |
| `PWANIM_EMPTY_BONE` | A bone block contains no keys. |
| `PWANIM_DUPLICATE_BONE` | A bone name occurs more than once. |
| `PWANIM_DUPLICATE_KEY` | A bone has two keys at the same frame. |
| `PWANIM_KEYS_OUT_OF_ORDER` | Keys in a bone are not strictly ascending. |
| `PWANIM_TRAILING_EASE` | The final key declares ease without a following segment. |
| `PWANIM_LOOP_SEAM` | A looping animation does not meet its seam tolerance. |
| `PWANIM_UNKNOWN_BONE` | A bone block is absent from the referenced skeleton. |
| `PWANIM_ASSET_PROVENANCE_CONFLICT` | The destination is owned by another source, or has no source stamp. The message names the current source when available; pass `overwrite=true` only when the takeover is intended. |

## Rebuilding an existing animation in place

`anim.compile` rebuilds an occupied `UAnimSequence` in place so referencers keep working, but it
does not silently preserve unrelated live state. The provenance stamp records the prior generated
state. If tracks, timebase, loop setting, skeleton, markers, curves, notifies, or retarget source
changed outside source and the new source would discard that change, compilation stops with
`PWSRC_RECOMPILE_UNMANAGED_STATE` before the data model is reset.

The same guard runs when the destination is stamped from a different source or has no stamp. A
different source cannot use the old source's baseline as proof, so every current value that differs
from or is omitted by the incoming source is named. Without `overwrite=true`, the existing
ownership refusal includes that list and the asset stays unchanged. With `overwrite=true`, the
takeover succeeds and the list is returned as a warning. A source-path relocation with identical
state has no state-loss warning, but still needs the normal takeover permission.

The in-place write is atomic at the compiler boundary. If track creation, key writing, marker
replacement, or final readback fails, the prior sequence and its package-dirty state are restored.
A failed compile therefore cannot leave a partial model waiting to be saved by an unrelated later
operation.

This is an error by default. `overwrite=true` explicitly permits the loss and emits the same code
as a warning. The deterministic rebuild clears notifies and `RetargetSource`, replaces all sync
markers, and regenerates tracks, timebase, skeleton, and loop from source. Float and transform
curves are not yet authorable in `.pwanim`; they are therefore protected by the guard and cleared
only after explicit overwrite permission.

## Held poses and key counts

A track that holds one pose for the whole animation — the single-key form, or any bone whose keys
all carry the same value — is stored by the engine with **one** key rather than `frames + 1`, and
that is a correct write, not a short one. It evaluates to the authored pose at every frame.

## Related pages

- [PinWright Skeleton format](pwskel-format.md) defines the referenced hierarchy.
- [PinWright Model format](pwmodel-format.md) can produce a skeletal mesh from that hierarchy.
- [Robot arm animation example](../Examples/pwanim/robot_arm_wave.pwanim) is a generic source example.
