// Copyright (c) 2026 Alexander Penkin. MIT License.

// PwModelDiagnostic.h - Diagnostic record for the .pwmodel compiler pipeline.
//
// The diagnostic record is shared with the other source formats. This header owns only
// the .pwmodel code registry; the source diagnostic type lives in PwSource.
#pragma once

#include "CoreMinimal.h"
#include "PwSource/PwDiagnostic.h"

// Registry of PWMODEL_* diagnostic codes, mirroring the Handlers/ErrorCodes.h idiom
// so the vocabulary is greppable and IntelliSense-discoverable rather than a set of
// raw TEXT("...") literals scattered across the pipeline. These are diagnostics of
// the source format, not dispatcher error codes - the model RPC verbs report them
// inside a result payload, so they are deliberately not ERR_* constants.
//
// Model-specific parser and compiler codes; shared lexical and structural codes live in PwSource.
namespace PwModelDiagnosticCodes
{
    // A model construct is unavailable in this .pwmodel version without pointing at a
    // different source format.
    // A reserved .pwmodel signpost belongs to another source format, such as .pwskel or
    // .pwanim, rather than being a construct this format can compile.
    inline constexpr TCHAR PWMODEL_CONSTRUCT_IN_WRONG_FORMAT[] = TEXT("PWMODEL_CONSTRUCT_IN_WRONG_FORMAT");
    // The document declares no `part`, so there is nothing to compile.
    inline constexpr TCHAR PWMODEL_NO_PARTS[] = TEXT("PWMODEL_NO_PARTS");
    // Two parts share a name.
    inline constexpr TCHAR PWMODEL_DUPLICATE_PART[] = TEXT("PWMODEL_DUPLICATE_PART");
    // A part with no ops. Warning: harmless, but always a mistake worth surfacing.
    inline constexpr TCHAR PWMODEL_EMPTY_PART[] = TEXT("PWMODEL_EMPTY_PART");
    // A part's or nested block's first op does not create geometry.
    inline constexpr TCHAR PWMODEL_PART_NEEDS_PRIMITIVE[] = TEXT("PWMODEL_PART_NEEDS_PRIMITIVE");
    // `color=` on a boolean op. A boolean produces FACES - the walls a cut opens, the tool's own
    // surface a union keeps - and those take `material=`, which the op now accepts. It produces no
    // VERTICES of its own to colour: every vertex in the result comes from one of the two operands
    // and already carries whatever colour that operand's generator gave it, so a scalar `color=`
    // here could only recolour geometry the author already coloured. The message names
    // `set_vertex_color` as the op that does mean that.
    inline constexpr TCHAR PWMODEL_MATERIAL_ON_BOOLEAN[] = TEXT("PWMODEL_MATERIAL_ON_BOOLEAN");
    // One op carries both `material="<Slot>"` and `material_id=<n>`: the named slot from the
    // model-wide table, and a raw index written straight into the triangles. Only one can be
    // honoured, and honouring either silently discards a value the author wrote - which is what
    // the compiler used to do, `material_id` winning with nothing said. The choice is the
    // author's, so this is an error rather than a warning with a documented precedence.
    inline constexpr TCHAR PWMODEL_MATERIAL_ID_CONFLICT[] = TEXT("PWMODEL_MATERIAL_ID_CONFLICT");

    // ---- Aiming a generator with from= / to= -------------------------------------
    //
    // Aiming replaces at= / rotate= and, on an op with an extent along its own local Z, the
    // parameter carrying that extent. Every code below refuses rather than repairs, for one
    // reason: each names an input from which TWO different placements can be derived, and a
    // compiler that picks one produces geometry the document does not describe while reporting
    // success. That is the failure this whole family exists to make impossible.

    // Exactly one of `from` / `to` was written, or `up` was written with neither. An aim needs
    // both endpoints: one point plus a twist reference is not a direction.
    inline constexpr TCHAR PWMODEL_AIM_INCOMPLETE[] = TEXT("PWMODEL_AIM_INCOMPLETE");
    // `from`/`to` together with `at=` or `rotate=`. Both spell the op's placement, and aiming
    // computes exactly those two values, so honouring either would discard the other in silence.
    inline constexpr TCHAR PWMODEL_AIM_CONFLICT[] = TEXT("PWMODEL_AIM_CONFLICT");
    // `from` and `to` are the same point. A zero-length aim names no direction, so there is no
    // rotation to derive and no length to take.
    inline constexpr TCHAR PWMODEL_AIM_DEGENERATE[] = TEXT("PWMODEL_AIM_DEGENERATE");
    // `up` is parallel or antiparallel to the aim direction, or is zero-length. The twist
    // reference exists to pin the one degree of freedom aiming leaves free, and one lying on the
    // aim axis pins nothing. NOT raised for the DEFAULT up on a vertical aim - that case has a
    // defined fallback (roll = 0) and a vertical limb is ordinary; see
    // PwValueRead::MakeAimRotator for why an explicit up gets no such fallback.
    inline constexpr TCHAR PWMODEL_AIM_UP_PARALLEL[] = TEXT("PWMODEL_AIM_UP_PARALLEL");
    // The op's own extent parameter was written alongside `from`/`to`, which already supplies it
    // from the endpoint distance. `box size=` is the one spelling that is not a plain conflict:
    // its X and Y are still the author's, so only a non-zero Z conflicts and the remedy is to
    // write that Z as 0.
    inline constexpr TCHAR PWMODEL_AIM_EXTENT_CONFLICT[] = TEXT("PWMODEL_AIM_EXTENT_CONFLICT");
    // A `capsule` aimed across a distance no greater than 2*radius. `length` measures the
    // cylindrical section only, so the requested capsule would need a negative one; the two caps
    // alone already span more than the author asked the op to cover. Raised from the COMPILER
    // rather than the parser because the radius default lives in the generator's params struct,
    // which is the only place that number is true.
    inline constexpr TCHAR PWMODEL_AIM_TOO_SHORT[] = TEXT("PWMODEL_AIM_TOO_SHORT");
    // Warning: `from`/`to` on an op with no extent along its local Z - a sphere, a torus, a
    // plane. The midpoint and the direction are used and the DISTANCE is not, which is worth
    // saying: an author who wrote two endpoints reasonably expects the geometry to reach them.
    inline constexpr TCHAR PWMODEL_AIM_LENGTH_UNUSED[] = TEXT("PWMODEL_AIM_LENGTH_UNUSED");
    // Warning: `scale=` with a Z other than 1 on an aimed op whose extent came from the endpoint
    // distance. Scale bakes into the vertices AFTER the extent is set, so the geometry no longer
    // ends at `to` - it ends at scale.z times the distance. Legal and occasionally deliberate,
    // which is why it warns; the X and Y components shape the cross-section and are untouched.
    inline constexpr TCHAR PWMODEL_AIM_AXIAL_SCALE[] = TEXT("PWMODEL_AIM_AXIAL_SCALE");

    // `noise_deform magnitude_mode=relative` together with `apply_along_normal=false`. The two
    // ask for incompatible things: relative magnitude scales the displacement by the vertex's own
    // mean one-ring edge length, which is a SCALAR ruler, while apply_along_normal=false displaces
    // by the noise VECTOR - three decorrelated fields with no single length to scale. The op
    // refuses the pair; this raises it at the line instead of letting it arrive as a generic
    // PWMODEL_OP_FAILED with no position of its own.
    inline constexpr TCHAR PWMODEL_NOISE_MODE_CONFLICT[] = TEXT("PWMODEL_NOISE_MODE_CONFLICT");

    // ---- Parser: model-level blocks ----------------------------------------------

    inline constexpr TCHAR PWMODEL_DUPLICATE_MATERIALS[] = TEXT("PWMODEL_DUPLICATE_MATERIALS");
    inline constexpr TCHAR PWMODEL_DUPLICATE_COLLISION[] = TEXT("PWMODEL_DUPLICATE_COLLISION");
    inline constexpr TCHAR PWMODEL_DUPLICATE_LIGHTMAP[] = TEXT("PWMODEL_DUPLICATE_LIGHTMAP");
    // Two bindings for one slot name inside `materials`.
    inline constexpr TCHAR PWMODEL_DUPLICATE_SLOT[] = TEXT("PWMODEL_DUPLICATE_SLOT");
    // Warning: geometry tags a slot that `materials` does not bind. The asset gets an
    // empty slot rather than no slot, so this is recoverable and not an error.
    inline constexpr TCHAR PWMODEL_UNBOUND_MATERIAL[] = TEXT("PWMODEL_UNBOUND_MATERIAL");
    // Warning: `materials` binds a slot no geometry references, so the asset has no such slot
    // and the binding is dropped (PwModelCompiler.cpp CreateAsset copies a binding only when
    // SlotNames contains its slot). NOT raised for the implicit `Default` slot when any
    // untagged part-level generator allocates it - that binding IS applied, and reporting it as
    // dropped contradicted the asset the same compile had just written.
    inline constexpr TCHAR PWMODEL_UNUSED_MATERIAL[] = TEXT("PWMODEL_UNUSED_MATERIAL");
    // `collision` carries both explicit elements and an `auto` rule, or more than one
    // `auto` rule. Both spellings ask for two incompatible collision sources.
    inline constexpr TCHAR PWMODEL_COLLISION_CONFLICT[] = TEXT("PWMODEL_COLLISION_CONFLICT");
    // A `hull` body that cannot produce a hull: statically, an empty block or a
    // `convex points=` list with fewer than 4 points; at compile time, a body whose ops
    // produce an empty mesh.
    inline constexpr TCHAR PWMODEL_DEGENERATE_HULL[] = TEXT("PWMODEL_DEGENERATE_HULL");

    // ---- Compiler: geometry the parser cannot see --------------------------------
    //
    // Everything below needs a mesh to have been built, which is the line between the two
    // stages: a document the parser accepts is spelled correctly, so a diagnostic from
    // here is about geometry rather than about grammar.

    // ---- Compiler: skeleton and skin semantics ---------------------------------
    //
    // These are model-only conditions. The shared resolver owns the implementation, but
    // the wire code remains format-specific so .pwmodel and .pwanim can keep identical
    // suffixes without sharing a diagnostic prefix.
    inline constexpr TCHAR PWMODEL_SKELETON_NOT_AN_ASSET_PATH[] = TEXT("PWMODEL_SKELETON_NOT_AN_ASSET_PATH");
    inline constexpr TCHAR PWMODEL_SKELETON_NOT_FOUND[] = TEXT("PWMODEL_SKELETON_NOT_FOUND");
    inline constexpr TCHAR PWMODEL_SKELETON_WRONG_KIND[] = TEXT("PWMODEL_SKELETON_WRONG_KIND");
    inline constexpr TCHAR PWMODEL_SKELETON_HAS_NO_BONES[] = TEXT("PWMODEL_SKELETON_HAS_NO_BONES");
    inline constexpr TCHAR PWMODEL_SKIN_NO_SKELETON[] = TEXT("PWMODEL_SKIN_NO_SKELETON");
    inline constexpr TCHAR PWMODEL_SKIN_UNBOUND[] = TEXT("PWMODEL_SKIN_UNBOUND");
    inline constexpr TCHAR PWMODEL_SKIN_INCOMPLETE[] = TEXT("PWMODEL_SKIN_INCOMPLETE");
    inline constexpr TCHAR PWMODEL_SKIN_STALE[] = TEXT("PWMODEL_SKIN_STALE");
    inline constexpr TCHAR PWMODEL_BONE_NOT_FOUND[] = TEXT("PWMODEL_BONE_NOT_FOUND");
    inline constexpr TCHAR PWMODEL_COLLISION_ON_SKELETAL[] = TEXT("PWMODEL_COLLISION_ON_SKELETAL");
    inline constexpr TCHAR PWMODEL_LIGHTMAP_ON_SKELETAL[] = TEXT("PWMODEL_LIGHTMAP_ON_SKELETAL");

    // An extracted GeometryOps function returned !bSuccess. The message carries the op's
    // own ERR_* code and text, anchored to the op's source line - that anchoring is the
    // whole reason op failures come back as diagnostics rather than as a bare error.
    inline constexpr TCHAR PWMODEL_OP_FAILED[] = TEXT("PWMODEL_OP_FAILED");
    // Warning: `delete_vertex` named a vertex id that is INSIDE the mesh's id space and is no
    // longer a vertex. The op is skipped and the compile continues.
    //
    // It is not the author's mistake, and it used to abort the whole compile as
    // PWMODEL_OP_FAILED [INVALID_VERTEX]. FDynamicMesh3::RemoveTriangle drops the vertices its
    // triangle leaves isolated, so the natural pairing - cut a patch with `delete_triangle`,
    // then clean up the vertex it stranded - names a vertex the ENGINE has already removed, and
    // no document can see that it did. A no-op is what the author asked for and got.
    //
    // Deliberately NOT the answer for an id past MaxVertexID or below zero. Those were never
    // vertices of this mesh, nothing removed them, and they stay PWMODEL_OP_FAILED - which is
    // the whole point of splitting the two: a genuine bad index must not be swallowed by the
    // repair for the benign one. Sparse-but-present is the only case this covers, and
    // `id < MaxVertexID` is what distinguishes it (deletes leave the id space sparse rather
    // than renumbering, so an id that was ever valid stays inside the bound).
    inline constexpr TCHAR PWMODEL_VERTEX_ALREADY_REMOVED[] = TEXT("PWMODEL_VERTEX_ALREADY_REMOVED");
    // Warning: a compile stage ran and reported something non-fatal - a clamped dimension,
    // a bevel on a mesh with no polygroups, a collision flag combination that is built but
    // never queried, a lightmap index the build moved. One code rather than one per stage:
    // these all arrive as free text from FOpResult::Warnings and its siblings. This compiler
    // was their only consumer until the geometry.* wrappers were fixed to emit the same
    // strings as a `warnings` response array (GeometryOpWarnings.h); the two channels now
    // carry identical text for the same condition.
    inline constexpr TCHAR PWMODEL_STAGE_WARNING[] = TEXT("PWMODEL_STAGE_WARNING");
    // ERROR: a boolean completed and changed nothing, which is what the engine does when the
    // two meshes are disjoint. It aborts the part, so nothing is created.
    //
    // "Changed nothing" is TRIANGLE COUNT **AND** ENCLOSED VOLUME, both unmoved. The triangle
    // count alone is what FOpResult::bChanged carries, and it is not enough: a through-cut
    // turns a box into a smaller box, so a cut that removed 40% of the material left the count
    // at 12 and aborted the part with this code. The message that goes with it must not claim
    // the operands are disjoint either - it only ever tested that the target came back
    // unmoved, and the two are not the same statement. It names both operands' bounds and
    // their separation instead, and says which of the two cases it is looking at.
    //
    // Never an engine refusal: a boolean the engine declines fails FOpResult::bSuccess and is
    // reported as PWMODEL_OP_FAILED with the engine's own text, one branch earlier.
    //
    // Was a Warning until it shipped twelve green examples of visibly broken models -
    // pipe_junction's two `subtract` bores both reported "changed nothing" while model.compile
    // answered success:true for a pipe with no hole through it. The author asked for material
    // to be removed and none was, so the geometry that would reach the asset is not the
    // geometry the document describes, nothing downstream repairs it, and no reading of the
    // result is still correct. That is the line between this and the warnings around it, which
    // all leave a valid asset (PWMODEL_UV_CHANNEL_FILLED repairs the mesh,
    // PWMODEL_MATERIAL_ID_OUT_OF_RANGE pads the slot list).
    inline constexpr TCHAR PWMODEL_BOOLEAN_NO_EFFECT[] = TEXT("PWMODEL_BOOLEAN_NO_EFFECT");
    // A part carried no elements in a UV channel another part writes. Without a fill the
    // merged mesh has elements in the channel - so the bake's MeshHasUsableUVs guard does
    // not fire - while the silent part's triangles carry none, and the asset ships
    // untextured with no error. One code at two severities, because the reader needs the
    // same fact either way; which one is emitted says whether the fill succeeded:
    //   Warning: the fill ran, so the merged mesh is uniformly textured. Emitted ONCE per
    //     model, with up to three clauses naming every part and channel the compiler invented
    //     UVs for: FILLED (the part had no elements in a channel another part populates),
    //     PADDED (an op appended geometry with no UVs into a channel the part was already
    //     using - repaired at the append by FCompiler::ReconcileUVChannelsForAppend), and
    //     REPLACED (the channel covered only some of the part's triangles; a partial channel
    //     cannot be patched in place, so the WHOLE channel was reprojected and any authored
    //     UVs in it were lost). Only the last costs the author something they wrote, which is
    //     why the three are distinguished rather than pooled.
    //   Error: the fill could not run. EnsureMeshHasUVChannel refused to create the channel
    //     on that part (a mesh supports at most 8 UV channels, 0-7), so the untextured merge
    //     above is exactly what compiling on would produce. Emitted per offending
    //     part/channel and it fails the compile: nothing downstream repairs it.
    inline constexpr TCHAR PWMODEL_UV_CHANNEL_FILLED[] = TEXT("PWMODEL_UV_CHANNEL_FILLED");
    // Warning, or Error on the channel declared by `lightmap`: triangles from different parts
    // have positive-area UV overlap. Edge and point contact are valid and do not trigger it.
    // The compiler runs this after model-level UV layout, so a final clean atlas stays silent.
    inline constexpr TCHAR PWMODEL_UV_OVERLAP_ACROSS_PARTS[] = TEXT("PWMODEL_UV_OVERLAP_ACROSS_PARTS");
    // Error: `lightmap channel=N` names an absent, partially assigned, or topologically invalid
    // UV layer. The overlap check cannot make an honest claim about such a channel, and the
    // static mesh would otherwise retain a lightmap index whose triangles cannot all use it.
    inline constexpr TCHAR PWMODEL_LIGHTMAP_UV_INVALID[] = TEXT("PWMODEL_LIGHTMAP_UV_INVALID");
    // Warning: two ops in the same part (or the same nested block) produced geometry whose
    // bounding boxes interpenetrate, and the second was APPENDED onto the first rather than
    // unioned with it. Siblings in a block are appended, always - there is no implicit union -
    // so the overlap survives as buried interior faces and the next boolean is handed a
    // self-intersecting mesh. That is what left a white cross-shaped shard standing inside the
    // gothic window's trefoil: three overlapping circles appended, so the lobe partition walls
    // survived inside the opening. The fix is an explicit `union { }` around the second op only
    // when both solids use the same material slot; different-material overlaps must be moved
    // apart or left deliberate because boolean tools do not allocate a material slot.
    //
    // Warning and not an error, because the test is bounding boxes: two solids whose boxes
    // interpenetrate need not themselves overlap (petals arranged around a ring, an L of two
    // boxes about a corner). It is deliberately NOT raised for merely adjacent geometry - see
    // FCompiler::WarnOnUnunionedOverlap for the positive-thickness rule and why.
    inline constexpr TCHAR PWMODEL_UNUNIONED_OVERLAP[] = TEXT("PWMODEL_UNUNIONED_OVERLAP");
    // Warning: two whole PARTS produced geometry whose bounding boxes interpenetrate. The
    // sibling of PWMODEL_UNUNIONED_OVERLAP one level up, and a SEPARATE code because neither its
    // scope nor its remedy carries over:
    //
    //   - `union { }` is an op INSIDE a part. Two parts cannot be unioned at all without first
    //     being made one part, so the advice that ends the op-level message is not available
    //     here and repeating it would send the author looking for a spelling that does not exist.
    //   - Diagnostics collapse on (severity, code, part). Sharing the op-level code would fold a
    //     cross-part warning into whatever op-level group the same part already had, and the
    //     printed message is the group's FIRST - so the cross-part fact would vanish behind it.
    //
    // Raised ONCE per model, listing the pairs, rather than once per pair. Interpenetrating parts
    // are the normal way this format builds an organic model - a shape per lobe, a shape per limb
    // - so a per-pair warning would run to dozens on a correct document and train the author to
    // ignore the whole code. One entry naming the count and the first several pairs is the most
    // that can be said without becoming noise, and it is enough to catch the case this exists
    // for: a model whose parts were placed by arithmetic that drifted.
    inline constexpr TCHAR PWMODEL_UNUNIONED_OVERLAP_PARTS[] = TEXT("PWMODEL_UNUNIONED_OVERLAP_PARTS");
    // Warning: the merged mesh has boundary edges - it is an open shell, not a solid. Every
    // example model compiled clean while the compiler ran no such check, and one shipped with
    // 272 boundary edges. Warning rather than error because open IS the right answer for some
    // models (a card, a plane, a `procedural_mesh` + `append_buffers` surface); the compiler
    // cannot tell those from a subtract that broke through a wall, and only the author can.
    inline constexpr TCHAR PWMODEL_MESH_NOT_CLOSED[] = TEXT("PWMODEL_MESH_NOT_CLOSED");
    // Warning: the merged mesh carries near-zero-area triangles, bowtie vertices, or both.
    // Separate from PWMODEL_MESH_NOT_CLOSED and a rung below it: degenerates are common,
    // usually survive the static-mesh build's own welding, and shipped examples have carried
    // them (the current per-example reading is in docs/wiki-src/model.examples.md, which is the
    // one place a corpus count is kept). They matter as a TREND - a jump is the tell that
    // two boolean operands were placed to touch exactly rather than to overlap, which is the
    // one arrangement FMeshBoolean::SnapTolerance cannot resolve and which no PinWright surface
    // can reach to widen (the GeometryScript wrapper does not expose it). Overlap, do not touch.
    inline constexpr TCHAR PWMODEL_DEGENERATE_GEOMETRY[] = TEXT("PWMODEL_DEGENERATE_GEOMETRY");
    // Warning: one or more edge-connected components form a spatial island outside the largest
    // triangle-count island. The geometry remains valid and may be intentional; the compiler
    // reports exact nearest distance and honours only part-level allow_floating=true.
    inline constexpr TCHAR PWMODEL_FLOATING_COMPONENT[] = TEXT("PWMODEL_FLOATING_COMPONENT");
    // Warning: a single edge-connected shell's surface passes through ITSELF - a membrane
    // spanning a solid's interior, or two walls pushed through each other. It is the one fault
    // no other field of the health block can express, and it is invisible in a render.
    //
    // Both shapes of it defeat the documented gate `isClosed && signedVolume > 0` outright. A
    // membrane's two fans are oppositely wound, so their contributions to signedVolume cancel
    // EXACTLY and the number is the correct figure for the solid that was wanted; boundaryEdges
    // is 0, orientationConsistent is true, degenerates and bowties are 0. A pinch degrades
    // signedVolume SMOOTHLY - 62% and 25% of the analytic swept volume with every other field
    // green - and only flips sign long after the mesh stopped being a solid.
    //
    // Warning and not an error, on the same line as PWMODEL_MESH_NOT_CLOSED: the compiler cannot
    // tell a deliberately self-crossing decorative surface from a sweep that pinched, only the
    // author can, and a self-intersecting mesh still bakes and still renders.
    //
    // Counted PER SHELL, never across shells, and that restriction is what makes it usable.
    // Parts and sibling ops are APPENDED, never unioned, so two interpenetrating lobes are two
    // components - the normal way an organic model is built here, and already reported by
    // PWMODEL_UNUNIONED_OVERLAP / PWMODEL_UNUNIONED_OVERLAP_PARTS at bounding-box granularity.
    // Pooling them here would put this warning on a large fraction of correct documents and get
    // the whole code ignored.
    inline constexpr TCHAR PWMODEL_SELF_INTERSECTING_SURFACE[] = TEXT("PWMODEL_SELF_INTERSECTING_SURFACE");
    // Warning: a whole-mesh `extrude` whose `direction` opposes the surface's own facing
    // normal, which produces a slab that is inside out. The engine displaces the ORIGINAL
    // triangles by `direction * distance` keeping their authored winding, so they land on the
    // +direction face (OffsetMeshRegion.cpp:696-705), and reverses the stationary duplicate
    // (:734-744) - so outward requires `direction` parallel to the facing normal, and the
    // facing normal is the NEGATION of the right-hand rule because Unreal is left-handed
    // (VectorUtil::Normal returns (V2-V0) x (V1-V0), VectorUtil.h:80-87).
    //
    // Raised at the CALL SITE because nothing downstream can: the resulting shell is closed,
    // manifold, 0 boundary edges, and renders identically to a correct one. This is the
    // authoring mistake that shipped an inside-out example part for two releases.
    inline constexpr TCHAR PWMODEL_EXTRUDE_FACING_OPPOSED[] = TEXT("PWMODEL_EXTRUDE_FACING_OPPOSED");
    // Warning: the twin of the code above, one op over. A `revolve` whose finished sweep
    // encloses NEGATIVE volume, which is what a profile walked the wrong way round produces.
    // The generator sweeps the section keeping the winding the point order gives it, so in the
    // profile's own (x = radius, y = height) plane the section has to be traversed
    // COUNTER-CLOCKWISE - up the OUTER face, over the top, back down the INNER face, which is
    // the enclosed material staying on the left - or every triangle in the solid faces inwards.
    //
    // Raised at the CALL SITE for the same reason as the extrude twin: nothing downstream can.
    // The solid comes out closed, manifold, 0 boundary edges, orientationConsistent, with the
    // same triangle count and the same bounds as the correct one, and it renders identically
    // from every angle. Only signedVolume moves, and the model-wide sum averages one inverted
    // part away.
    //
    // Measured on the mesh the op produced rather than on the point list, so the closed-section
    // branch, a partial sweep with `capped` and the axis-capped lathe are all covered by one
    // test. Skipped on an OPEN result, where signed volume means nothing, and on a
    // negative-determinant `scale=`, which reverses the winding for a reason of its own and
    // would make this warning blame the wrong parameter.
    inline constexpr TCHAR PWMODEL_REVOLVE_PROFILE_REVERSED[] = TEXT("PWMODEL_REVOLVE_PROFILE_REVERSED");
    // Warning: an APPENDING modifier - `sweep`, `extrude_along_spline` - produced triangles whose
    // material slot cannot be derived from the geometry it was appended to, and the op carried no
    // `material=` to say what it should be. Two shapes, one code, because the reader has to act on
    // the same fact either way and the message says which case it is:
    //
    //   MIXED. The geometry the op extends carries more than one slot, so there is no single
    //     answer to inherit. The op takes the slot MOST of those triangles are on - deterministic,
    //     and the least surprising of the available guesses - and names the alternatives.
    //   NOTHING TO INHERIT. The op was handed an empty mesh (a `procedural_mesh` that appended
    //     nothing in front of it), so the new triangles keep the engine's material ID 0. That ID
    //     is NOT a neutral default: slots are a model-wide table in first-use order, so 0 is
    //     whichever slot the FIRST part in the document tagged, and the geometry ships in another
    //     part's material.
    //
    // Raised at the CALL SITE for the same reason as the two warnings above: nothing downstream
    // can. The mesh is valid, the slot count is unchanged - the op allocates no slot when it
    // inherits - so neither PWMODEL_MATERIAL_ID_OUT_OF_RANGE (which needs an ID past the end of
    // the table) nor PWMODEL_UNUSED_MATERIAL (which needs a binding nothing tags) can see it, and
    // the only place the choice appears is which section the new faces render in.
    //
    // NOT raised for the ordinary case this exists to make ordinary: a modifier appending into
    // geometry that speaks with one voice inherits that slot silently, which is what an author
    // means by putting a sweep in a part.
    inline constexpr TCHAR PWMODEL_MODIFIER_MATERIAL_AMBIGUOUS[] = TEXT("PWMODEL_MODIFIER_MATERIAL_AMBIGUOUS");

    // Warning: a BOOLEAN produced faces whose material slot could not be derived. The boolean
    // twin of PWMODEL_MODIFIER_MATERIAL_AMBIGUOUS above, and the same two shapes:
    //
    //   MIXED. The geometry the boolean was applied to carries more than one slot, so untagged
    //     block geometry - and the walls a `subtract` opens, which ARE that geometry - have no
    //     single slot to inherit. The op takes the slot MOST of the target is on and names the
    //     alternatives. Write `material=` on the boolean, or on the generator inside its block.
    //   NOTHING TO INHERIT. The boolean was applied to an empty mesh, so its faces keep material
    //     ID 0 - which is not a neutral default but whichever slot the FIRST part in the document
    //     tagged.
    //
    // A separate code from the modifier one because the REMEDY is different: a boolean's new faces
    // can be named on the op itself or on any generator inside its block, and neither spelling
    // exists for a sweep.
    //
    // NOT raised when the target speaks with one voice, which is the ordinary document: the walls
    // a cut opens in a single-material part join that material silently.
    inline constexpr TCHAR PWMODEL_BOOLEAN_MATERIAL_AMBIGUOUS[] = TEXT("PWMODEL_BOOLEAN_MATERIAL_AMBIGUOUS");

    // Warning: a `material=` written on a boolean op, or on a generator inside its block, opened a
    // slot that NO triangle of the result carries - so the tag did nothing and the asset gains an
    // empty section.
    //
    // The realistic source is a TOOL SOLID THE OPERATION DISCARDS ENTIRELY - most often one that
    // misses the target, in a block whose other solids do meet it, so the op works and only that
    // one tool is dead. `PWMODEL_BOOLEAN_NO_EFFECT` cannot see it: the boolean as a whole did
    // change the geometry.
    //
    // NOT `trim`, despite the shape of the op suggesting it. GeometryOps::Trim maps `keep_inside`
    // onto Subtract / Intersection and dispatches the same ApplyMeshBoolean the other three
    // booleans use (GeometryOps_Boolean.cpp), so a trim's tool surface BECOMES the cut face and a
    // tag on it lands on real geometry. An ordinary `union` or `subtract` tool leaves faces behind
    // too, so this stays quiet on all of them.
    //
    // Distinct from PWMODEL_UNUSED_MATERIAL, which is the parser's report of a `materials { }`
    // BINDING no tag names. This is the opposite direction - a tag no geometry carries - and only
    // the finished mesh can see it, which is why it is raised here.
    inline constexpr TCHAR PWMODEL_BOOLEAN_MATERIAL_UNUSED[] = TEXT("PWMODEL_BOOLEAN_MATERIAL_UNUSED");

    // Every part ran, and the merged mesh has no triangles.
    inline constexpr TCHAR PWMODEL_EMPTY_MESH[] = TEXT("PWMODEL_EMPTY_MESH");
    // The merged mesh exceeds GEOM_MAX_TRIANGLES_PER_DYNAMIC_MESH.
    inline constexpr TCHAR PWMODEL_MESH_TOO_LARGE[] = TEXT("PWMODEL_MESH_TOO_LARGE");
    // A vertex position is NaN or infinite. Checked before creation because the value
    // survives the bake and turns up later as a mesh with an infinite bounding box.
    inline constexpr TCHAR PWMODEL_INVALID_GEOMETRY[] = TEXT("PWMODEL_INVALID_GEOMETRY");
    // Warning: a triangle carries a material ID past the end of the model-wide slot list,
    // which only `append_buffers material_id=` can produce. Slots are padded so the asset
    // stays valid rather than referencing a slot that does not exist.
    inline constexpr TCHAR PWMODEL_MATERIAL_ID_OUT_OF_RANGE[] = TEXT("PWMODEL_MATERIAL_ID_OUT_OF_RANGE");
    // Warning: an untagged generator allocated the implicit `Default` slot in a document whose
    // `materials` block binds every slot the author wrote and does not bind `Default`. That
    // geometry ships on the engine default material rather than on anything the author named.
    //
    // It no longer reports a renumbering: the slot is placed LAST, after every slot a tag names
    // (PwModelCompiler.cpp MoveImplicitDefaultSlotLast), so nothing declared moves. The index is
    // still named, because referencers address sections by index and have to stay off it.
    //
    // Raised from the compiler rather than the parser because the INDEX is the fact that matters
    // and only the finished slot table carries it. Not raised for a document with no `materials`
    // block, which is the ordinary untagged case, nor for one that binds `Default` itself, nor
    // for one where a part-level op tags `material="Default"` by hand, nor for one where no
    // declared slot reached the asset at all - PWMODEL_UNUSED_MATERIAL already reports that per
    // binding.
    inline constexpr TCHAR PWMODEL_IMPLICIT_DEFAULT_SLOT[] = TEXT("PWMODEL_IMPLICIT_DEFAULT_SLOT");
    // Collision construction failed without a more specific code of its own.
    inline constexpr TCHAR PWMODEL_COLLISION_FAILED[] = TEXT("PWMODEL_COLLISION_FAILED");
    // The single CreateStaticMesh call failed. The message carries the creator's ERR_*
    // code, so an ASSET_ALREADY_EXISTS refusal reads as a refusal rather than as a crash.
    inline constexpr TCHAR PWMODEL_ASSET_CREATE_FAILED[] = TEXT("PWMODEL_ASSET_CREATE_FAILED");
}
