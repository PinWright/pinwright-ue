---
type: reference
summary: "Normative reference for PinWright Skeleton (.pwskel): hierarchy, rig metadata, deterministic recompiles, validation, provenance, and diagnostics."
date: 2026-08-26
tags: [pwskel, skeleton, animation, file-format, diagnostics, examples]
---

# PinWright Skeleton (`.pwskel`) - format reference

A `.pwskel` file describes one Unreal skeleton hierarchy. It is source text, not an
asset path and not an animation. The compiler creates one `USkeleton` asset from the
hierarchy and preserves the source path as provenance.

## Version and document shape

The first non-comment, non-blank line must be:

```
pwskel 0
```

Version 0 has no compatibility promise. The document contains one nested `bone` hierarchy and
may also declare one preview mesh and curve metadata:

```
preview_mesh path="/Game/Rigs/SKM_Robot"

curve "MotionAmount" material=true morph_target=false max_lod=2 {
    linked_bone "root"
}

bone "name" [at=(x, y, z)] [rotate=(roll, pitch, yaw)] [scale=(x, y, z)]
    [retarget=animation|skeleton|animation_scaled|animation_relative|orient_and_scale] {
    bone "child" { }
}
```

There must be exactly one root bone. Children are written inside their parent's braces.
Bone names are string literals and must be unique across the complete hierarchy. A
document without a root is invalid.

A line break ends a statement — there is no statement terminator — so **a bone's `{` must
open on the header line**. A `{` on the next line is a new statement, not that bone's block,
and is refused with `PWSRC_BAD_BLOCK` naming the orphaned brace's line. Bone names are
compared case-insensitively, matching `FName`: `head` and `Head` collide.

Transforms are optional. `at` defaults to `(0, 0, 0)`, `rotate` defaults to
`(0, 0, 0)` degrees, in roll-pitch-yaw order, and `scale` defaults to `(1, 1, 1)`.
The transform is the bone's local transform relative to its parent. `retarget` defaults to
`animation` and writes that bone's translation retargeting mode.

`preview_mesh` takes a `USkeletalMesh` object path. The mesh must be bound to the generated
skeleton, or to a skeleton Unreal accepts as compatible. This usually means adding the line on a
second compile, after the mesh has been built against the first skeleton.

`curve` declares serialized `AnimCurveMetaData`. `material` and `morph_target` default to false;
`max_lod=-1` means every LOD and otherwise accepts 0 through 254. Each `linked_bone` must name a
bone declared in the same source. Curve metadata is authored rig data, not a cache: Unreal saves
it on the skeleton and exposes it for editing, so source must be able to reproduce it.

### Scale has a domain, and it is checked

`at` and `rotate` accept any value. `scale` does not, because it is the one header value whose
*domain* can turn a document the parser accepts into an asset nothing can pose.

**A vanishing axis is an error (`PWSKEL_DEGENERATE_SCALE`).** Any axis whose magnitude is at or
inside `UE_KINDA_SMALL_NUMBER` (1e-4) makes the bone's reference-pose transform singular:
every descendant collapses onto that bone's origin, and geometry skinned through the chain has
no volume. The threshold is an engine constant rather than a tuned one — 1e-4 is what
`FMath::IsNearlyZero` and `FVector::IsNearlyZero` use when a caller supplies no tolerance, so
at or under it the transform is singular at the precision the engine itself compares at; under
`UE_SMALL_NUMBER` (1e-8), `FTransform::GetSafeScaleReciprocal` substitutes a literal zero and
the inverse bind transform is undefined outright. It is an **error** and not a warning because
there is no reading of the source under which the resulting skeleton is usable.

Checking only exact zero would be worse than useless here: `scale=(0.00001, 1, 1)` collapses a
limb just as completely and would have reported nothing.

**A negative axis is a warning (`PWSKEL_MIRRORED_SCALE`).** It negates the determinant of the
bone's frame, so the handedness of that bone and of every descendant is flipped and skinned
geometry below it reads inside-out. It stays a warning because a mirrored chain is a legitimate
rig and the asset is well-formed — the compile still produces it. If the mirror was not
intended, negate the children's `at` offsets instead.

An axis that is both negative and vanishing is one mistake, not two: the singular reading is
the severe one, so that axis reports only the error.

## Recommended source placement

Keep a `.pwskel` directly beside the `.uasset` it generates, under the project's `Content/`
tree, with the same basename:

```text
Content/Characters/Hero/SK_Hero.uasset
Content/Characters/Hero/SK_Hero.pwskel
```

This is recommended, not mandatory. It makes the source-to-asset mapping obvious, makes an
orphaned source or an unstamped asset visible at a glance, and keeps an asset rename and its
source rename as one coherent operation. Existing layouts keep working by continuing to pass an
explicit `outputPath`.

Unreal's asset registry recognizes `.uasset` and `.umap`, not `.pwskel`, and typical Git LFS
rules are extension-based rather than path-based. A `.pwskel` under `Content/` therefore stays
plain, diffable text unless the project has a broader custom rule.

Omitting `outputPath` activates the convention:
`<ProjectDir>/Content/<Rel>/<Name>.pwskel` derives `/Game/<Rel>/<Name>`. A source outside that
tree, with the wrong extension, or mapping to an invalid package path is refused with
`SOURCE_OUTPUT_PATH_NOT_DERIVABLE`; the message names the reason and the explicit `outputPath`
argument to pass. Nothing is guessed. The derived target then goes through the same provenance
and `overwrite` checks as an explicit target, including refusal of an asset stamped from another
source unless `overwrite=true`.

## RPC surfaces

`skeleton.validate` accepts exactly one of `text` or `filePath`. It parses and validates
the source without creating an asset.

`skeleton.compile` requires `filePath`; `outputPath` is optional only for the mapping above. Inline `text` is refused for
asset creation. The output is a `USkeleton`; the response reports the asset path,
bone count, whether an existing asset was updated in place, whether it was saved, and
whether a save remains pending. A source file may update its own stamped asset in place only when
the live asset still matches the previous generated baseline, or when the new source now describes
the live change. An unstamped or different source requires `overwrite=true` before replacement.

`skeleton.describe_ops` reports the `bone` vocabulary and its `at`, `rotate`, and
`scale` parameters. The canonical worked source is
[`Examples/pwskel/robot_arm.pwskel`](../Examples/pwskel/robot_arm.pwskel).

## Recompile safety and fields outside the format

The provenance stamp stores the source path and the semantic state produced by the last compile.
On a same-source recompile, the compiler compares that baseline with both the live asset and the
new source. If live state changed outside source and the new source would not reproduce it,
compilation stops with `PWSRC_RECOMPILE_UNMANAGED_STATE`. The asset is untouched.

On a takeover from a different source, or from an unstamped asset, the incoming source cannot trust
the old baseline. The guard therefore names every current value that the incoming source changes or
omits. Without `overwrite=true`, that list is included in the existing ownership refusal. With
`overwrite=true`, the takeover succeeds and the same code is emitted as a warning.

This is an **error**, because a warning would still destroy data while returning success.
`overwrite=true` is the deliberate escape hatch: the rebuild proceeds, emits the same code as a
warning, and clears state the source does not own. The compiler never carries omitted live state
forward, because that would make output depend on edit history rather than source.

The source owns the hierarchy, preview mesh, per-bone translation retargeting modes, and curve
metadata. It does not currently express sockets, virtual bones, retarget sources, blend profiles,
slot groups, compatible skeletons, additional preview meshes, notify names, or preview
attachments. Add those through their normal authoring surfaces only when they are intentionally
temporary, or expect to pass `overwrite=true` to discard them on the next source rebuild.

Writing `socket "..."` or `virtual_bone "..."` in a `.pwskel` is refused with
`PWSRC_UNEXPECTED_TOKEN`, and the refusal names the verb that owns the construct rather than
only saying it is not allowed — an ordinary unknown keyword still gets the generic message
and its spelling suggestion.

A successful compile is measured from the resulting asset, not inferred from completed write
calls. The reference hierarchy, local transforms, retargeting table, and source-owned metadata
must all read back as described. A hierarchy change also refreshes loaded animation assets in the
same editor session, so validation and compilation do not observe different skeleton generations.

## Diagnostics

Lexical, value, and generic source-shape failures use the shared `PWSRC_*` codes. The
skeleton-specific registry carries what only a skeleton can mean — hierarchy shape, the
domain of a bone transform, and what a rebuild left behind:

| Code | Meaning |
|---|---|
| `PWSKEL_NO_BONES` | The document contains no root bone declaration. |
| `PWSKEL_MULTIPLE_ROOTS` | More than one top-level bone was declared. |
| `PWSKEL_DUPLICATE_BONE` | A bone name occurs more than once in the hierarchy. |
| `PWSKEL_DEGENERATE_SCALE` | A `scale` axis is at or inside 1e-4, making the bone's reference-pose transform singular. Error. |
| `PWSKEL_MIRRORED_SCALE` | A `scale` axis is negative, flipping the handedness of the bone and every descendant. Warning. |
| `PWSKEL_DUPLICATE_PREVIEW_MESH` | More than one `preview_mesh` declaration was supplied. |
| `PWSKEL_DUPLICATE_CURVE` | A curve metadata name occurs more than once. |
| `PWSKEL_UNKNOWN_LINKED_BONE` | Curve metadata links a bone absent from this source. |
| `PWSKEL_ASSET_POSTCONDITION_FAILED` | The compiled asset did not read back the hierarchy or metadata described by source. No success is reported. |

The shared source catalog is:

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

## Related pages

- [PinWright Animation format](pwanim-format.md) uses a compiled `USkeleton`.
- [PinWright Model format](pwmodel-format.md) can use a compiled skeleton for skinning.
- [Robot arm skeleton example](../Examples/pwskel/robot_arm.pwskel) is a generic source example.
