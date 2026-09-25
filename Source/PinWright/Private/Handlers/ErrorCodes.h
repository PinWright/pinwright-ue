// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"

// Central registry of error codes passed as the first argument to
// FHandlerContext::SendError(Code, Message). Each entry is the literal string
// the handler emits; callsites reference the ERR_<CODE> constant instead of a
// raw TEXT("...") so the vocabulary is greppable, IntelliSense-discoverable,
// and enforced by TestErrorCodeRegistry (which rejects any handler that passes
// a raw string literal not present in this header).
//
// Adding a code: declare a new ERR_<CODE> constant here first, then reference it
// from the handler. Do NOT introduce a raw TEXT("...") code at a callsite.
//
// REGISTERING A CODE HERE DOES NOT REQUIRE ADOPTING THE CONSTANT AT THE CALL SITE, and for some
// files it must not. TestErrorCodeRegistry enforces two separate rules: every emitted code has an
// entry here (this list), and a file that already cites ErrorCodes::ERR_ may not ALSO hand-spell a
// code. The second rule flips a file to "adopting" on its first ErrorCodes::ERR_ reference, so
// sprinkling one constant into a file that spells fifty codes by hand turns the other forty-nine
// into hard failures. The Niagara edit family is the standing case: it builds errors through
// FNiagaraEditError::Make(TEXT("CODE"), Message), its codes are registered below, and its call
// sites deliberately stay raw until someone converts a whole file at once.
//
// Semantic dupes still present (existing spellings retained so this header is a
// non-breaking registry; collapsing them is a separate breaking PR — see board
// ticket E-error-code-vocabulary-registry and docs/error-code-catalog.md):
//   - bad caller input:  INVALID_ARGUMENT / INVALID_PARAMS / INVALID_PARAM /
//                        INVALID_PARAMETER / INVALID_PAYLOAD
//   - missing field:     MISSING_PARAM / MISSING_PARAMETER / MISSING_PARAMETERS
//   - creation failure:  CREATE_FAILED / CREATION_FAILED / CREATION_ERROR
//   - wrong param type:  INVALID_PARAM_TYPE / INVALID_PARAMETER_TYPE
//   - delegate bind:     BIND_FAILED / BINDING_FAILED / BINDING_CREATION_FAILED

namespace ErrorCodes
{
    inline constexpr TCHAR ERR_ACTION_FAILED[]                              = TEXT("ACTION_FAILED");
    // spatial.ground_actors / spatial.verify_grounding: the actor is deeper into the surface
    // than the caller allowed. Distinct from ACTOR_NOT_GROUNDED (its opposite) so a caller can
    // tell a sunk prop from a floating one without parsing the message.
    inline constexpr TCHAR ERR_ACTOR_BURIED[]                               = TEXT("ACTOR_BURIED");
    // The actor has no primitive components, so there is no footprint to seat or measure.
    inline constexpr TCHAR ERR_ACTOR_HAS_NO_BOUNDS[]                        = TEXT("ACTOR_HAS_NO_BOUNDS");
    // The actor refuses label edits (AActor::IsActorLabelEditable is false) - typically an
    // actor inside a Level Instance that is not currently being edited.
    inline constexpr TCHAR ERR_ACTOR_LABEL_NOT_EDITABLE[]                   = TEXT("ACTOR_LABEL_NOT_EDITABLE");
    inline constexpr TCHAR ERR_ACTOR_LOAD_FAILED[]                          = TEXT("ACTOR_LOAD_FAILED");
    // AActor::IsLockLocation() is set; a placement verb refuses rather than silently overriding
    // a lock the user set in the editor.
    inline constexpr TCHAR ERR_ACTOR_LOCATION_LOCKED[]                      = TEXT("ACTOR_LOCATION_LOCKED");
    inline constexpr TCHAR ERR_ACTOR_NOT_FOUND[]                            = TEXT("ACTOR_NOT_FOUND");
    // Part of the actor floats above the surface by more than the caller allowed.
    inline constexpr TCHAR ERR_ACTOR_NOT_GROUNDED[]                         = TEXT("ACTOR_NOT_GROUNDED");
    inline constexpr TCHAR ERR_ACTOR_NO_SKELETAL_MESH_COMPONENT[]           = TEXT("ACTOR_NO_SKELETAL_MESH_COMPONENT");
    inline constexpr TCHAR ERR_ACTOR_SKELETAL_MESH_ASSET_NULL[]             = TEXT("ACTOR_SKELETAL_MESH_ASSET_NULL");
    inline constexpr TCHAR ERR_ACTOR_SPAWN_FAILED[]                         = TEXT("ACTOR_SPAWN_FAILED");
    inline constexpr TCHAR ERR_ADD_CAMERA_FAILED[]                          = TEXT("ADD_CAMERA_FAILED");
    inline constexpr TCHAR ERR_ADD_FAILED[]                                 = TEXT("ADD_FAILED");
    inline constexpr TCHAR ERR_ADD_NODE_FAILED[]                            = TEXT("ADD_NODE_FAILED");
    inline constexpr TCHAR ERR_ADD_PLAYER_FAILED[]                          = TEXT("ADD_PLAYER_FAILED");
    inline constexpr TCHAR ERR_ADD_SOURCE_FAILED[]                          = TEXT("ADD_SOURCE_FAILED");
    inline constexpr TCHAR ERR_AGIR_DECOMPILE_FAILED[]                      = TEXT("AGIR_DECOMPILE_FAILED");
    inline constexpr TCHAR ERR_AIMOFFSET_NOT_FOUND[]                        = TEXT("AIMOFFSET_NOT_FOUND");
    inline constexpr TCHAR ERR_ALIAS_CREATE_FAILED[]                        = TEXT("ALIAS_CREATE_FAILED");
    inline constexpr TCHAR ERR_ALREADY_EXISTS[]                             = TEXT("ALREADY_EXISTS");
    inline constexpr TCHAR ERR_ALREADY_PLAYING[]                            = TEXT("ALREADY_PLAYING");
    // An actorName matched more than one actor at the same precedence tier - almost always
    // a display label, which UE does not make unique. The verb refuses rather than acting
    // on whichever actor iterated first; the payload carries every candidate with its
    // unique internal object name so the caller can re-issue against one of them.
    inline constexpr TCHAR ERR_AMBIGUOUS_ACTOR_NAME[]                       = TEXT("AMBIGUOUS_ACTOR_NAME");
    inline constexpr TCHAR ERR_AMBIGUOUS_EMITTER_HANDLE[]                   = TEXT("AMBIGUOUS_EMITTER_HANDLE");
    // actor.get_instances / actor.set_instance_transforms / spatial.ground_instances: 'component'
    // was omitted on an actor carrying more than one instanced component, so there is no
    // unambiguous scatter to address. The verbs used to fall back to the component with the MOST
    // instances, which on a shared holder (AInstancedFoliageActor keeps one component per foliage
    // type for every caller in the level) is routinely another caller's scatter. The message names
    // every candidate with its instance count, so the refusal is the scoping surface rather than a
    // dead end.
    inline constexpr TCHAR ERR_AMBIGUOUS_INSTANCED_COMPONENT[]              = TEXT("AMBIGUOUS_INSTANCED_COMPONENT");
    inline constexpr TCHAR ERR_AMBIGUOUS_NODE[]                             = TEXT("AMBIGUOUS_NODE");
    inline constexpr TCHAR ERR_AMBIGUOUS_SESSION[]                          = TEXT("AMBIGUOUS_SESSION");
    inline constexpr TCHAR ERR_AMBIGUOUS_SOURCE[]                           = TEXT("AMBIGUOUS_SOURCE");
    inline constexpr TCHAR ERR_ANALYSIS_FAILED[]                            = TEXT("ANALYSIS_FAILED");
    inline constexpr TCHAR ERR_ANIMATION_INVALID[]                          = TEXT("ANIMATION_INVALID");
    inline constexpr TCHAR ERR_ANIMATION_NOT_FOUND[]                        = TEXT("ANIMATION_NOT_FOUND");
    inline constexpr TCHAR ERR_ANIMGRAPH_MODULE_UNAVAILABLE[]               = TEXT("ANIMGRAPH_MODULE_UNAVAILABLE");
    inline constexpr TCHAR ERR_ANIM_BP_NOT_FOUND[]                          = TEXT("ANIM_BP_NOT_FOUND");
    // anim.compile / anim.validate: the animation document parsed, but a later stage failed
    // (skeleton binding, baking, asset creation, or a refused overwrite). Kept apart from
    // ANIM_PARSE_FAILED so callers can distinguish malformed source from a valid document that
    // cannot be built; PWANIM_* diagnostics remain in the structured result either way.
    inline constexpr TCHAR ERR_ANIM_COMPILE_FAILED[]                        = TEXT("ANIM_COMPILE_FAILED");
    // No .pwanim file at the resolved filePath.
    inline constexpr TCHAR ERR_ANIM_FILE_NOT_FOUND[]                        = TEXT("ANIM_FILE_NOT_FOUND");
    // The source exists but is not usable text, or an inline validate request is empty.
    inline constexpr TCHAR ERR_ANIM_INVALID_SOURCE[]                        = TEXT("ANIM_INVALID_SOURCE");
    inline constexpr TCHAR ERR_ANIM_LOAD_FAILED[]                           = TEXT("ANIM_LOAD_FAILED");
    // The .pwanim source did not parse. Nothing was created.
    inline constexpr TCHAR ERR_ANIM_PARSE_FAILED[]                          = TEXT("ANIM_PARSE_FAILED");
    inline constexpr TCHAR ERR_APPLY_FAILED[]                               = TEXT("APPLY_FAILED");
    inline constexpr TCHAR ERR_ASC_NOT_FOUND[]                              = TEXT("ASC_NOT_FOUND");
    // Narrower than ERR_ALREADY_EXISTS: the create path must distinguish "a
    // DIFFERENT asset class already occupies this package path" (never silently
    // replaced, regardless of overwrite) from "the right class is there but is
    // still referenced" (ASSET_IN_USE). See Utils/AssetCreatePolicy.h.
    inline constexpr TCHAR ERR_ASSET_ALREADY_EXISTS[]                       = TEXT("ASSET_ALREADY_EXISTS");
    inline constexpr TCHAR ERR_ASSET_CLASS_MISMATCH[]                       = TEXT("ASSET_CLASS_MISMATCH");
    inline constexpr TCHAR ERR_ASSET_COMPILING[]                            = TEXT("ASSET_COMPILING");
    inline constexpr TCHAR ERR_ASSET_CREATION_FAILED[]                      = TEXT("ASSET_CREATION_FAILED");
    inline constexpr TCHAR ERR_ASSET_DATA_INVALID[]                         = TEXT("ASSET_DATA_INVALID");
    inline constexpr TCHAR ERR_ASSET_EXISTS[]                               = TEXT("ASSET_EXISTS");
    // overwrite:true refused because the existing asset still has referencers.
    // Two axes, both always present in the error data: referencers[] +
    // referencerCount are on-disk PACKAGES from the asset registry;
    // referencingActors[] + referencingActorCount are LIVE level actors found in
    // memory when the delete itself is refused (the everyday cause - a spawned
    // actor holding a just-compiled mesh, which the registry cannot see). Emitted
    // instead of attempting a delete, which is what raised three engine modals
    // before (AssetTools.cpp CanCreateAsset). See Utils/AssetCreatePolicy.h.
    inline constexpr TCHAR ERR_ASSET_IN_USE[]                               = TEXT("ASSET_IN_USE");
    inline constexpr TCHAR ERR_ASSET_LOAD_FAILED[]                          = TEXT("ASSET_LOAD_FAILED");
    inline constexpr TCHAR ERR_ASSET_NOT_FOUND[]                            = TEXT("ASSET_NOT_FOUND");
    inline constexpr TCHAR ERR_ASSET_WRONG_TYPE[]                           = TEXT("ASSET_WRONG_TYPE");
    inline constexpr TCHAR ERR_ASSET_PLAYER_BASE_UNAVAILABLE[]              = TEXT("ASSET_PLAYER_BASE_UNAVAILABLE");
    inline constexpr TCHAR ERR_ASSET_REGISTRY_UNAVAILABLE[]                 = TEXT("ASSET_REGISTRY_UNAVAILABLE");
    inline constexpr TCHAR ERR_ATTACH_FAILED[]                              = TEXT("ATTACH_FAILED");
    // ---- audio generation / analysis (Private/AudioGen) -------------------------------------
    // The audio verbs reuse DECODE_FAILED / ENCODE_FAILED / FILE_NOT_FOUND rather than adding
    // AUDIO_-prefixed synonyms of them; only failures those cannot express get a code here.
    //
    // The operation has no samples to work on - a zero-length render, a recipe whose layers
    // produced nothing, an analysis handed an unfilled buffer. An error rather than a
    // zero-length success: an empty render and a silent one are different outcomes.
    inline constexpr TCHAR ERR_AUDIO_EMPTY_BUFFER[]                         = TEXT("AUDIO_EMPTY_BUFFER");
    // The source carries more than two channels. FPwAudioBuffer is deinterleaved stereo, so a
    // 5.1/7.1 source has no lossless landing place; the verb refuses rather than dropping
    // channels the caller cannot see were dropped.
    inline constexpr TCHAR ERR_AUDIO_MULTICHANNEL_UNSUPPORTED[]             = TEXT("AUDIO_MULTICHANNEL_UNSUPPORTED");
    // The buffer carries NaN or infinity samples. Deliberately NOT collapsed into
    // AUDIO_EMPTY_BUFFER even though the remedy also lives upstream: one non-finite sample
    // poisons every sum, every FFT bin and every threshold comparison downstream, and - the
    // reason this has its own code - NaN compares false against every threshold, so a buffer
    // full of it measures as digital silence unless it is named first. The error payload
    // carries the count and the first offending frame, so the caller can find the generator or
    // effect that produced it rather than re-rendering blind.
    inline constexpr TCHAR ERR_AUDIO_NON_FINITE_SAMPLES[]                   = TEXT("AUDIO_NON_FINITE_SAMPLES");
    // The sound is generated at playback time and owns no stored samples, so there is nothing
    // to read back. Distinct from AUDIO_UNSUPPORTED_FORMAT: the asset is valid and readable as
    // an asset, it simply has no sample data to fetch.
    inline constexpr TCHAR ERR_AUDIO_PROCEDURAL_UNSUPPORTED[]               = TEXT("AUDIO_PROCEDURAL_UNSUPPORTED");
    // The container / codec / bit depth is one this verb cannot read or write on this host.
    inline constexpr TCHAR ERR_AUDIO_UNSUPPORTED_FORMAT[]                   = TEXT("AUDIO_UNSUPPORTED_FORMAT");
    // Editor preview playback could not be started, or started and produced no voice.
    inline constexpr TCHAR ERR_AUDITION_FAILED[]                            = TEXT("AUDITION_FAILED");
    // ---- level.audit ----------------------------------------------------------------------
    // One code per audit CHECK, so a caller can key on the code rather than parse a message,
    // and a finding's code is the same string whether it arrives as a per-actor row or (for
    // the whole-call rejections) as the RPC's own error. See Handlers/Level/LevelAuditUtils.h.
    // The actor's closest part floats above the stated surface, or nothing was under it at all.
    inline constexpr TCHAR ERR_AUDIT_AIRBORNE[]                             = TEXT("AUDIT_AIRBORNE");
    // A visible actor sits on the world origin - the default transform of an unplaced spawn.
    inline constexpr TCHAR ERR_AUDIT_AT_WORLD_ORIGIN[]                      = TEXT("AUDIT_AT_WORLD_ORIGIN");
    // A large actor touches the surface in too few footprint columns: a physically valid rest
    // that reads as a physics glitch. Distinct from AUDIT_AIRBORNE, which is its opposite.
    inline constexpr TCHAR ERR_AUDIT_BALANCED_ON_POINT[]                    = TEXT("AUDIT_BALANCED_ON_POINT");
    // The actor's bounds top is below AWorldSettings::KillZ.
    inline constexpr TCHAR ERR_AUDIT_BELOW_KILL_Z[]                         = TEXT("AUDIT_BELOW_KILL_Z");
    // Every sampled column has an accepted surface above the actor's HIGHEST point, i.e. none
    // of it is above ground. Deliberately not the same question as AUDIT_DEEPLY_EMBEDDED.
    inline constexpr TCHAR ERR_AUDIT_BELOW_SURFACE[]                        = TEXT("AUDIT_BELOW_SURFACE");
    // Republished from the engine's own AActor::IsDataValid for the actor.
    inline constexpr TCHAR ERR_AUDIT_DATA_VALIDATION[]                      = TEXT("AUDIT_DATA_VALIDATION");
    // More of the actor's height is under the surface than the caller allowed, while part of
    // it is still visible above. Deliberate bedding lives here, which is why the check is off
    // by default.
    inline constexpr TCHAR ERR_AUDIT_DEEPLY_EMBEDDED[]                      = TEXT("AUDIT_DEEPLY_EMBEDDED");
    inline constexpr TCHAR ERR_AUDIT_DUPLICATE_TRANSFORM[]                  = TEXT("AUDIT_DUPLICATE_TRANSFORM");
    inline constexpr TCHAR ERR_AUDIT_EXTREME_SCALE[]                        = TEXT("AUDIT_EXTREME_SCALE");
    // The `playArea` argument was absent, malformed, or resolved to no box. Never a fallback
    // to a different area - measuring against a box the caller did not ask for is worse than
    // refusing. Mirrors INVALID_SURFACE_SPEC.
    inline constexpr TCHAR ERR_AUDIT_INVALID_PLAY_AREA_SPEC[]               = TEXT("AUDIT_INVALID_PLAY_AREA_SPEC");
    inline constexpr TCHAR ERR_AUDIT_MISSING_MATERIAL[]                     = TEXT("AUDIT_MISSING_MATERIAL");
    inline constexpr TCHAR ERR_AUDIT_MISSING_MESH[]                         = TEXT("AUDIT_MISSING_MESH");
    inline constexpr TCHAR ERR_AUDIT_NAN_TRANSFORM[]                        = TEXT("AUDIT_NAN_TRANSFORM");
    inline constexpr TCHAR ERR_AUDIT_NEGATIVE_SCALE[]                       = TEXT("AUDIT_NEGATIVE_SCALE");
    inline constexpr TCHAR ERR_AUDIT_OUTSIDE_PLAY_AREA[]                    = TEXT("AUDIT_OUTSIDE_PLAY_AREA");
    inline constexpr TCHAR ERR_AUDIT_OUTSIDE_WORLD_BOUNDS[]                 = TEXT("AUDIT_OUTSIDE_WORLD_BOUNDS");
    // The actor's support chain left the audited set, cycled, or was deeper than the walk
    // limit. Reported as UNRUNNABLE, never as a pass: an unresolved chain is not a sound one.
    inline constexpr TCHAR ERR_AUDIT_SUPPORT_CHAIN_UNRESOLVED[]             = TEXT("AUDIT_SUPPORT_CHAIN_UNRESOLVED");
    // The per-call ground-measurement budget ran out before this actor. Reported per actor as
    // unrunnable rather than skipped, because a budget that silently stops measuring is the
    // same failure as a check that silently stops running.
    inline constexpr TCHAR ERR_AUDIT_TRACE_BUDGET_EXHAUSTED[]               = TEXT("AUDIT_TRACE_BUDGET_EXHAUSTED");
    // A `checks` / `excludeChecks` entry that names no registered check. An error rather than
    // a silent no-op: a typo that quietly ran nothing is indistinguishable from a clean level.
    inline constexpr TCHAR ERR_AUDIT_UNKNOWN_CHECK[]                        = TEXT("AUDIT_UNKNOWN_CHECK");
    // The actor rests on a stack whose own support ends in mid-air - the assembly-level defect
    // no per-actor contact check can see.
    inline constexpr TCHAR ERR_AUDIT_UNSUPPORTED_ASSEMBLY[]                 = TEXT("AUDIT_UNSUPPORTED_ASSEMBLY");
    // A scale axis is at or below the caller's epsilon. Per axis, unlike the engine map
    // check's product test (ActorEditor.cpp:1675), which cannot say which axis collapsed.
    inline constexpr TCHAR ERR_AUDIT_ZERO_SCALE[]                           = TEXT("AUDIT_ZERO_SCALE");
    inline constexpr TCHAR ERR_AUTOMATION_ERROR[]                           = TEXT("AUTOMATION_ERROR");
    inline constexpr TCHAR ERR_AUTOMATION_NOT_READY[]                       = TEXT("AUTOMATION_NOT_READY");
    // system.run_tests owns UE's process-global automation controller. A second caller must
    // wait for the current generation instead of replacing its filter or sharing completion.
    inline constexpr TCHAR ERR_AUTOMATION_RUN_IN_PROGRESS[]                 = TEXT("AUTOMATION_RUN_IN_PROGRESS");
    // render.capture_ortho_tiles: the requested screen-axis mapping does not survive the round
    // trip PinWrightTileGrid::TryMakeCameraRotationForAxisMapping runs - a camera rotation was
    // built from it and classifying that rotation back did not reproduce the mapping. Refused
    // rather than captured: the alternative is a mosaic that is mirrored or transposed against
    // its own georeference, which looks entirely plausible until a coordinate is read off it.
    inline constexpr TCHAR ERR_AXIS_MAPPING_UNRENDERABLE[]                  = TEXT("AXIS_MAPPING_UNRENDERABLE");
    inline constexpr TCHAR ERR_ATTENUATION_NOT_FOUND[]                      = TEXT("ATTENUATION_NOT_FOUND");
    inline constexpr TCHAR ERR_ATTRIBUTE_NOT_FOUND[]                        = TEXT("ATTRIBUTE_NOT_FOUND");
    inline constexpr TCHAR ERR_ATTRIBUTE_SET_NOT_FOUND[]                    = TEXT("ATTRIBUTE_SET_NOT_FOUND");
    // sequencer.bake_to_controlrig: the engine bake did not leave a Control Rig track on the
    // binding. Distinct from NO_KEYS_WRITTEN, which means the track exists and is empty.
    inline constexpr TCHAR ERR_BAKE_FAILED[]                                = TEXT("BAKE_FAILED");
    // Asset batch mutators preflight every requested row. The aggregate refusal and the
    // otherwise-valid rows held back by that refusal are separate states so callers can fix the
    // bad inputs and retry without mistaking untouched survivors for runtime failures.
    inline constexpr TCHAR ERR_BATCH_NOT_ATTEMPTED[]                        = TEXT("BATCH_NOT_ATTEMPTED");
    inline constexpr TCHAR ERR_BATCH_PREFLIGHT_FAILED[]                     = TEXT("BATCH_PREFLIGHT_FAILED");
    inline constexpr TCHAR ERR_BINDING_CREATION_FAILED[]                    = TEXT("BINDING_CREATION_FAILED");
    inline constexpr TCHAR ERR_BINDING_FAILED[]                             = TEXT("BINDING_FAILED");
    inline constexpr TCHAR ERR_BINDING_NOT_FOUND[]                          = TEXT("BINDING_NOT_FOUND");
    inline constexpr TCHAR ERR_BINDING_NOT_SKELETAL[]                       = TEXT("BINDING_NOT_SKELETAL");
    // sequencer component binding: the possessable was minted but FMovieScenePossessable::GetParent()
    // came back invalid (or names a possessable that is not in the movie scene). A component binding
    // whose parent link is missing resolves to NOTHING at playback - MovieSceneHelpers::GetResolutionContext
    // only substitutes the owning actor as the resolve context when GetParent() is valid - so the tracks
    // keyed to it silently drive nothing. Reported as a failure, never a warning; the verb rolls the
    // half-made binding back out of the sequence rather than leaving a placeholder.
    inline constexpr TCHAR ERR_BINDING_PARENT_NOT_SET[]                     = TEXT("BINDING_PARENT_NOT_SET");
    // sequencer component binding: the binding exists with a valid parent, but resolving it through the
    // runtime path (MovieSceneHelpers::GetBoundObjects over a transient shared playback state) did not
    // return the object the caller named. Distinct from BINDING_PARENT_NOT_SET so a caller can tell a
    // missing hierarchy link from a locator that resolves elsewhere.
    inline constexpr TCHAR ERR_BINDING_UNRESOLVED[]                         = TEXT("BINDING_UNRESOLVED");
    inline constexpr TCHAR ERR_BIND_FAILED[]                                = TEXT("BIND_FAILED");
    inline constexpr TCHAR ERR_BLANK_CAPTURE[]                              = TEXT("BLANK_CAPTURE");
    inline constexpr TCHAR ERR_BLENDSPACE_NOT_FOUND[]                       = TEXT("BLENDSPACE_NOT_FOUND");
    inline constexpr TCHAR ERR_BLEND_CURVE_NOT_FOUND[]                      = TEXT("BLEND_CURVE_NOT_FOUND");
    inline constexpr TCHAR ERR_BLUEPRINT_BUSY[]                             = TEXT("BLUEPRINT_BUSY");
    inline constexpr TCHAR ERR_BLUEPRINT_COMPILE_FAILED[]                   = TEXT("BLUEPRINT_COMPILE_FAILED");
    inline constexpr TCHAR ERR_BLUEPRINT_CREATE_FAILED[]                    = TEXT("BLUEPRINT_CREATE_FAILED");
    inline constexpr TCHAR ERR_BLUEPRINT_NOT_FOUND[]                        = TEXT("BLUEPRINT_NOT_FOUND");
    inline constexpr TCHAR ERR_BODY_NOT_FOUND[]                             = TEXT("BODY_NOT_FOUND");
    inline constexpr TCHAR ERR_BODY_SETUP_FAILED[]                          = TEXT("BODY_SETUP_FAILED");
    inline constexpr TCHAR ERR_BONE_EXISTS[]                                = TEXT("BONE_EXISTS");
    inline constexpr TCHAR ERR_BONE_MASK_NOT_FOUND[]                        = TEXT("BONE_MASK_NOT_FOUND");
    inline constexpr TCHAR ERR_BONE_NOT_FOUND[]                             = TEXT("BONE_NOT_FOUND");
    // Narrower than BONE_NOT_FOUND, and a different recovery. The bone exists on the mesh's
    // reference skeleton, but the SkeletalMesh SECTION that owns the requested vertex does not
    // list it in FSkelMeshSection::BoneMap, so no section-local influence slot exists for it and
    // one cannot be added without a re-chunk. BONE_NOT_FOUND means "use a different name";
    // this means "that bone cannot influence that vertex on this mesh as built". Emitted by
    // skeleton.set_vertex_weights. See Handlers/Animation/SkinWeightTransferUtils.h.
    inline constexpr TCHAR ERR_BONE_NOT_IN_SECTION[]                        = TEXT("BONE_NOT_IN_SECTION");
    inline constexpr TCHAR ERR_BOOKMARK_EMPTY[]                             = TEXT("BOOKMARK_EMPTY");
    inline constexpr TCHAR ERR_BOOKMARK_SET_FAILED[]                        = TEXT("BOOKMARK_SET_FAILED");
    inline constexpr TCHAR ERR_BOOLEAN_FAILED[]                             = TEXT("BOOLEAN_FAILED");
    inline constexpr TCHAR ERR_BOUNDS_EMPTY[]                               = TEXT("BOUNDS_EMPTY");
    inline constexpr TCHAR ERR_BPIR_REQUIRED[]                              = TEXT("BPIR_REQUIRED");
    inline constexpr TCHAR ERR_BTIR_ASSET_NOT_FOUND[]                       = TEXT("BTIR_ASSET_NOT_FOUND");
    inline constexpr TCHAR ERR_BTIR_DECOMPILE_FAILED[]                      = TEXT("BTIR_DECOMPILE_FAILED");
    inline constexpr TCHAR ERR_BULK_DELETE_FAILED[]                         = TEXT("BULK_DELETE_FAILED");
    inline constexpr TCHAR ERR_BULK_RENAME_FAILED[]                         = TEXT("BULK_RENAME_FAILED");
    inline constexpr TCHAR ERR_CALL_FAILED[]                                = TEXT("CALL_FAILED");
    inline constexpr TCHAR ERR_CAMERA_LOAD_FAILED[]                         = TEXT("CAMERA_LOAD_FAILED");
    inline constexpr TCHAR ERR_CAMERA_NOT_BOUND[]                           = TEXT("CAMERA_NOT_BOUND");
    // The candidate id WAS issued by this session's generated-candidate registry, and the
    // registry then reclaimed it to stay inside its byte/count budget. Distinct from
    // CANDIDATE_NOT_FOUND because the remedies diverge and an agent branches on the code
    // before it reads the payload: evicted means the id was real, so re-render it (and
    // discard candidates you are done with, or raise the budget); not-found means the id
    // itself is wrong. Collapsing the two is what makes an agent "fix" an id it got right.
    // The error payload carries `status`, the `removal` record and the live budget.
    inline constexpr TCHAR ERR_CANDIDATE_EVICTED[]                          = TEXT("CANDIDATE_EVICTED");
    // The candidate id names no entry in the session's generated-candidate registry and no
    // record of ever having been issued by it - i.e. the id is wrong. A candidate the
    // registry reclaimed reports CANDIDATE_EVICTED and an empty registry reports
    // NO_CANDIDATES; this code is only the genuine unknown id. Also used for a candidate the
    // CALLER discarded, with the payload's `status` reading "discarded": that is a
    // caller-side fact, and its remedy (re-render, or stop referring to it) is the same one
    // a wrong id gets. Distinct from ASSET_NOT_FOUND: a candidate is in-memory and
    // session-scoped, so the recovery is to re-generate, not to fix an asset path.
    inline constexpr TCHAR ERR_CANDIDATE_NOT_FOUND[]                        = TEXT("CANDIDATE_NOT_FOUND");
    inline constexpr TCHAR ERR_CANNOT_RELOAD_ACTIVE_LEVEL[]                 = TEXT("CANNOT_RELOAD_ACTIVE_LEVEL");
    inline constexpr TCHAR ERR_CANNOT_REMOVE_ROOT[]                         = TEXT("CANNOT_REMOVE_ROOT");
    inline constexpr TCHAR ERR_CAPTURE_ARRAY_NOT_FOUND[]                    = TEXT("CAPTURE_ARRAY_NOT_FOUND");
    // render.capture_ortho_tiles: the capture component's transform did not read back at the pose
    // the tile asked for, so the renderer would have drawn this tile from somewhere else. Refused
    // rather than captured - a burst rendered from one fixed pose still writes every file and
    // still reports per-tile world extents, so nothing downstream can tell it from a real mosaic.
    inline constexpr TCHAR ERR_CAPTURE_CAMERA_NOT_APPLIED[]                 = TEXT("CAPTURE_CAMERA_NOT_APPLIED");
    inline constexpr TCHAR ERR_CAPTURE_FAILED[]                             = TEXT("CAPTURE_FAILED");
    // editor.screenshot / ui.screenshot / render.capture_open_level: shader compilation was still
    // in flight after the bounded readiness drain, so the readback is refused rather than issued.
    // A material whose shader map has not landed renders as the DEFAULT material, and such a frame
    // reads settled, non-blank and clean on every other honesty field -- so it is refused rather
    // than shipped with a warning. Not a crash code: a pending asset-compile queue never produces
    // it, and the gate does not wait on one.
    inline constexpr TCHAR ERR_CAPTURE_NOT_READY[]                          = TEXT("CAPTURE_NOT_READY");
    inline constexpr TCHAR ERR_CAST_FAILED[]                                = TEXT("CAST_FAILED");
    inline constexpr TCHAR ERR_CDO_FAILED[]                                 = TEXT("CDO_FAILED");
    inline constexpr TCHAR ERR_CHAIN_MAP_NOT_APPLIED[]                      = TEXT("CHAIN_MAP_NOT_APPLIED");
    inline constexpr TCHAR ERR_CHAIN_NOT_ADDED[]                            = TEXT("CHAIN_NOT_ADDED");
    inline constexpr TCHAR ERR_CHAIN_NOT_FOUND[]                            = TEXT("CHAIN_NOT_FOUND");
    // niagara.set_curve_keys / niagara.get_curve_keys: the data interface IS a curve DI, but the
    // requested `channel` is not one of its members (or a scalar DI was given one at all). Distinct
    // from INCOMPATIBLE_DATA_INTERFACE, which means the object is not a curve DI in the first place.
    inline constexpr TCHAR ERR_CHANNEL_MISMATCH[]                           = TEXT("CHANNEL_MISMATCH");
    inline constexpr TCHAR ERR_CHECKOUT_FAILED[]                            = TEXT("CHECKOUT_FAILED");
    inline constexpr TCHAR ERR_CLASS_MISMATCH[]                             = TEXT("CLASS_MISMATCH");
    inline constexpr TCHAR ERR_CLASS_NOT_A_COMPONENT[]                      = TEXT("CLASS_NOT_A_COMPONENT");
    inline constexpr TCHAR ERR_CLASS_NOT_FOUND[]                            = TEXT("CLASS_NOT_FOUND");
    inline constexpr TCHAR ERR_CLASS_NOT_INSTANTIABLE[]                     = TEXT("CLASS_NOT_INSTANTIABLE");
    inline constexpr TCHAR ERR_CLOTH_CREATE_FAILED[]                        = TEXT("CLOTH_CREATE_FAILED");
    inline constexpr TCHAR ERR_CLOTH_CREATE_UNSUPPORTED[]                   = TEXT("CLOTH_CREATE_UNSUPPORTED");
    inline constexpr TCHAR ERR_CLOTH_NAME_IN_USE[]                          = TEXT("CLOTH_NAME_IN_USE");
    inline constexpr TCHAR ERR_CLOTH_NOT_FOUND[]                            = TEXT("CLOTH_NOT_FOUND");
    inline constexpr TCHAR ERR_COMMAND_BLOCKED[]                            = TEXT("COMMAND_BLOCKED");
    inline constexpr TCHAR ERR_COMMAND_FAILED[]                             = TEXT("COMMAND_FAILED");
    inline constexpr TCHAR ERR_COMPILE_FAILED[]                             = TEXT("COMPILE_FAILED");
    inline constexpr TCHAR ERR_COMPONENT_CREATE_FAILED[]                    = TEXT("COMPONENT_CREATE_FAILED");
    inline constexpr TCHAR ERR_COMPONENT_CREATION_FAILED[]                  = TEXT("COMPONENT_CREATION_FAILED");
    inline constexpr TCHAR ERR_COMPONENT_FAILED[]                           = TEXT("COMPONENT_FAILED");
    inline constexpr TCHAR ERR_COMPONENT_NOT_FOUND[]                        = TEXT("COMPONENT_NOT_FOUND");
    // blueprint.graph.delete_orphaned_nodes: the batch would have left a UK2Node_Composite
    // (collapsed graph, macro, math expression) with a null entry or exit tunnel back-pointer.
    // UK2Node_Composite::GetEntryNode()/GetExitNode() are check()-guarded, so that state is not a
    // recoverable error downstream — it is a fatal assert on the package's next load, and both
    // compile and save accept it silently. The deletions are rolled back and nothing is written,
    // so this code always means "the asset on disk is untouched".
    inline constexpr TCHAR ERR_COMPOSITE_BOUNDARY_BROKEN[]                  = TEXT("COMPOSITE_BOUNDARY_BROKEN");
    inline constexpr TCHAR ERR_COMPOSITE_NOT_FOUND[]                        = TEXT("COMPOSITE_NOT_FOUND");
    inline constexpr TCHAR ERR_CONDITION_INVALID[]                          = TEXT("CONDITION_INVALID");
    inline constexpr TCHAR ERR_CONFIG_OPERATION_MISMATCH[]                  = TEXT("CONFIG_OPERATION_MISMATCH");
    inline constexpr TCHAR ERR_CONFIG_READ_FAILED[]                          = TEXT("CONFIG_READ_FAILED");
    inline constexpr TCHAR ERR_CONFIG_TARGET_MISMATCH[]                      = TEXT("CONFIG_TARGET_MISMATCH");
    inline constexpr TCHAR ERR_CONNECTION_DISALLOWED[]                      = TEXT("CONNECTION_DISALLOWED");
    inline constexpr TCHAR ERR_CONNECTION_FAILED[]                          = TEXT("CONNECTION_FAILED");
    inline constexpr TCHAR ERR_CONNECT_FAILED[]                             = TEXT("CONNECT_FAILED");
    inline constexpr TCHAR ERR_CONSTRAINT_NOT_FOUND[]                       = TEXT("CONSTRAINT_NOT_FOUND");
    inline constexpr TCHAR ERR_CONSTRUCTION_FAILED[]                        = TEXT("CONSTRUCTION_FAILED");
    inline constexpr TCHAR ERR_CONTROLLER_UNAVAILABLE[]                     = TEXT("CONTROLLER_UNAVAILABLE");
    inline constexpr TCHAR ERR_CONTROLRIG_TRACK_NOT_FOUND[]                 = TEXT("CONTROLRIG_TRACK_NOT_FOUND");
    inline constexpr TCHAR ERR_CONTROL_NOT_FOUND[]                          = TEXT("CONTROL_NOT_FOUND");
    inline constexpr TCHAR ERR_CONVERSION_FAILED[]                          = TEXT("CONVERSION_FAILED");
    inline constexpr TCHAR ERR_CREATE_ASSET_FAILED[]                        = TEXT("CREATE_ASSET_FAILED");
    inline constexpr TCHAR ERR_CREATE_COMPONENT_FAILED[]                    = TEXT("CREATE_COMPONENT_FAILED");
    inline constexpr TCHAR ERR_CREATE_DYNAMIC_LIGHT_FAILED[]                = TEXT("CREATE_DYNAMIC_LIGHT_FAILED");
    inline constexpr TCHAR ERR_CREATE_FAILED[]                              = TEXT("CREATE_FAILED");
    inline constexpr TCHAR ERR_CREATE_NODE_FAILED[]                         = TEXT("CREATE_NODE_FAILED");
    inline constexpr TCHAR ERR_CREATE_TRACK_FAILED[]                        = TEXT("CREATE_TRACK_FAILED");
    inline constexpr TCHAR ERR_CREATION_ERROR[]                             = TEXT("CREATION_ERROR");
    inline constexpr TCHAR ERR_CREATION_FAILED[]                            = TEXT("CREATION_FAILED");
    inline constexpr TCHAR ERR_CRIR_ASSET_NOT_FOUND[]                       = TEXT("CRIR_ASSET_NOT_FOUND");
    inline constexpr TCHAR ERR_CUE_NOT_FOUND[]                              = TEXT("CUE_NOT_FOUND");
    inline constexpr TCHAR ERR_CURVE_ASSET_NOT_FOUND[]                      = TEXT("CURVE_ASSET_NOT_FOUND");
    inline constexpr TCHAR ERR_CVAR_NOT_FOUND[]                             = TEXT("CVAR_NOT_FOUND");
    inline constexpr TCHAR ERR_CYCLE_DETECTED[]                             = TEXT("CYCLE_DETECTED");
    inline constexpr TCHAR ERR_DATALAYER_ALREADY_ASSIGNED[]                 = TEXT("DATALAYER_ALREADY_ASSIGNED");
    inline constexpr TCHAR ERR_DATALAYER_NOT_FOUND[]                        = TEXT("DATALAYER_NOT_FOUND");
    inline constexpr TCHAR ERR_DATA_INTERFACE_CLASS_NOT_FOUND[]             = TEXT("DATA_INTERFACE_CLASS_NOT_FOUND");
    inline constexpr TCHAR ERR_DATA_INTERFACE_EXISTS[]                      = TEXT("DATA_INTERFACE_EXISTS");
    inline constexpr TCHAR ERR_DATA_INTERFACE_NOT_FOUND[]                   = TEXT("DATA_INTERFACE_NOT_FOUND");
    inline constexpr TCHAR ERR_DECODE_FAILED[]                              = TEXT("DECODE_FAILED");
    inline constexpr TCHAR ERR_DECOMPILE_FAILED[]                           = TEXT("DECOMPILE_FAILED");
    inline constexpr TCHAR ERR_DEFAULT_PROPERTY_NOT_FOUND[]                 = TEXT("DEFAULT_PROPERTY_NOT_FOUND");
    inline constexpr TCHAR ERR_DELETE_FAILED[]                              = TEXT("DELETE_FAILED");
    inline constexpr TCHAR ERR_DELETE_PARTIAL[]                             = TEXT("DELETE_PARTIAL");
    inline constexpr TCHAR ERR_DEPENDENCY_MISSING[]                         = TEXT("DEPENDENCY_MISSING");
    inline constexpr TCHAR ERR_DEPRECATED_HANDLER[]                         = TEXT("DEPRECATED_HANDLER");
    // The caller asked to write a value the engine RE-DERIVES from other state, so the
    // write would land, survive read-back, survive save, and be recomputed away on the
    // next load — invisible to every check short of a reload. Distinct from
    // UNSUPPORTED_PROPERTY ("this verb does not handle that property"): here the
    // property is real and writable, it is simply not the authority. The error payload
    // carries derivedWrite {property, derivedFrom, authoritativeVerb} so a caller can
    // retry through the setter that owns the value. See docs/rpc-design.md §5.
    inline constexpr TCHAR ERR_DERIVED_PROPERTY[]                           = TEXT("DERIVED_PROPERTY");
    inline constexpr TCHAR ERR_DESTINATION_EXISTS[]                         = TEXT("DESTINATION_EXISTS");
    // asset.move: destinationPath is neither an existing content folder to move into nor a
    // path inside one, so both readings of it are wrong and nothing was moved. Distinct from
    // ASSET_NOT_FOUND, which is about the SOURCE being missing: here the source is fine and the
    // folder the caller named does not exist. The verb refuses instead of picking a reading,
    // because the object-path reading silently renames the asset after the missing folder's last
    // segment and the folder reading would manufacture a content folder from a typo. The message
    // names the missing folder so the caller can create it with asset.create_folder and retry.
    inline constexpr TCHAR ERR_DESTINATION_FOLDER_NOT_FOUND[]                = TEXT("DESTINATION_FOLDER_NOT_FOUND");
    inline constexpr TCHAR ERR_DETACH_FAILED[]                              = TEXT("DETACH_FAILED");
    inline constexpr TCHAR ERR_DIALOGUE_NOT_AVAILABLE[]                     = TEXT("DIALOGUE_NOT_AVAILABLE");
    // level.load (and its aliases editor.open_level / editor.open_asset on a World): the map
    // being opened is ALREADY resident with unsaved changes. UEditorEngine::Map_Load must unload
    // that package to re-read the map, UPackageTools::UnloadPackages refuses to unload a dirty
    // package, and Map_Load then reaches an unconditional Fatal that kills the editor process.
    // The verb refuses instead, having changed nothing; see Utils/MapSwapDirtyWorldGuard.h.
    // The same code also covers the second, dirtiness-independent blocker on the same verbs
    // plus level.create: a DEAD world still resident after the pre-swap purge and collect,
    // which EditorDestroyWorld's own CheckForWorldGCLeaks would fatal on. That payload
    // carries survivingWorlds[] instead of the packageDirty/worldFound fields.
    inline constexpr TCHAR ERR_DIRTY_WORLD_BLOCKS_MAP_SWAP[]                 = TEXT("DIRTY_WORLD_BLOCKS_MAP_SWAP");
    inline constexpr TCHAR ERR_DUPLICATE_FAILED[]                           = TEXT("DUPLICATE_FAILED");
    inline constexpr TCHAR ERR_DUPLICATE_NAME[]                             = TEXT("DUPLICATE_NAME");
    inline constexpr TCHAR ERR_DUPLICATE_TIMELINE[]                         = TEXT("DUPLICATE_TIMELINE");
    inline constexpr TCHAR ERR_DYNAMIC_INPUT_NO_OUTPUT_TYPE[]               = TEXT("DYNAMIC_INPUT_NO_OUTPUT_TYPE");
    inline constexpr TCHAR ERR_DYNAMIC_INPUT_SET_FAILED[]                   = TEXT("DYNAMIC_INPUT_SET_FAILED");
    inline constexpr TCHAR ERR_EDGE_FAILED[]                                = TEXT("EDGE_FAILED");
    inline constexpr TCHAR ERR_EDGE_NOT_FOUND[]                             = TEXT("EDGE_NOT_FOUND");
    inline constexpr TCHAR ERR_EDITOR_ACTOR_SUBSYSTEM_MISSING[]             = TEXT("EDITOR_ACTOR_SUBSYSTEM_MISSING");
    // Deliberately NOT a variant of EDITOR_NOT_READY: that code is documented
    // retryable (BuildPingResult sets retryable:true beside it) and a well-behaved
    // client polls it, which is exactly the loop that burns a 600 s watchdog. A
    // modal owning the game thread is not retryable - only a human dismissing the
    // dialog, or killing the process, clears it.
    inline constexpr TCHAR ERR_EDITOR_BLOCKED_ON_MODAL[]                    = TEXT("EDITOR_BLOCKED_ON_MODAL");
    // The other half of the same symptom, and deliberately a THIRD code rather
    // than a reuse of either neighbour. Not EDITOR_NOT_READY: that means "still
    // starting", and an agent told that about a 90 s wedge learns nothing about
    // what is wedged. Not EDITOR_BLOCKED_ON_MODAL: that is non-retryable and
    // needs a human, whereas a stalled handler may simply return. Emitted only by
    // McpRequestCore::BuildPingResult, from the socket I/O thread, past
    // UPinWrightSettings::GameThreadStallReportSeconds. Retryable: yes.
    inline constexpr TCHAR ERR_EDITOR_GAME_THREAD_STALLED[]                  = TEXT("EDITOR_GAME_THREAD_STALLED");
    // The caller asserted `_expect_editor` and this process is not that editor, so the request
    // was refused BEFORE the handler ran. Emitted by the dispatcher's identity gate, not by any
    // handler. NOT retryable: the port is derived from the project path, so retrying reaches the
    // same wrong editor - the caller must aim at the endpoint its own editor bound, or stop the
    // rival. The payload carries `expected` and `actual` for exactly the asserted fields, because
    // a refusal that names only one side does not tell the caller which editor answered.
    // See Transport/EditorIdentity.h.
    inline constexpr TCHAR ERR_EDITOR_IDENTITY_MISMATCH[]                   = TEXT("EDITOR_IDENTITY_MISMATCH");
    // Terminal error of a job that was still running when editor.quit committed to
    // exiting and had no cancel hook: the process is going away, so the ticket (and any
    // client streaming it) gets a terminal state instead of waiting on a dead editor.
    inline constexpr TCHAR ERR_EDITOR_EXITING[]                             = TEXT("EDITOR_EXITING");
    // editor.quit only: another client has driven this editor within the in-use
    // window (EditorQuitPolicy::InUseWindowSeconds), so exiting would end a
    // session that is still in use. Sits beside UNSAVED_CHANGES as the second
    // thing quit refuses over, and takes the same shape - a refusal naming the
    // evidence, cleared by an explicit opt-in (force:true) rather than silently.
    // Retryable only in the sense that the window elapses: an editor nobody is
    // driving stops producing traffic and the same call then succeeds.
    inline constexpr TCHAR ERR_EDITOR_IN_USE[]                              = TEXT("EDITOR_IN_USE");
    inline constexpr TCHAR ERR_EDITOR_NOT_AVAILABLE[]                       = TEXT("EDITOR_NOT_AVAILABLE");
    inline constexpr TCHAR ERR_EDITOR_NOT_FOUND[]                           = TEXT("EDITOR_NOT_FOUND");
    inline constexpr TCHAR ERR_EDITOR_NOT_OPEN[]                            = TEXT("EDITOR_NOT_OPEN");
    inline constexpr TCHAR ERR_EDITOR_NOT_READY[]                           = TEXT("EDITOR_NOT_READY");
    inline constexpr TCHAR ERR_EDITOR_OPEN[]                                = TEXT("EDITOR_OPEN");
    inline constexpr TCHAR ERR_EDITOR_SUBSYSTEM_MISSING[]                   = TEXT("EDITOR_SUBSYSTEM_MISSING");
    inline constexpr TCHAR ERR_EDITOR_WORLD_NOT_AVAILABLE[]                 = TEXT("EDITOR_WORLD_NOT_AVAILABLE");
    inline constexpr TCHAR ERR_EFFECT_CLASS_NOT_FOUND[]                     = TEXT("EFFECT_CLASS_NOT_FOUND");
    inline constexpr TCHAR ERR_EFFECT_NOT_ACTIVE[]                          = TEXT("EFFECT_NOT_ACTIVE");
    inline constexpr TCHAR ERR_ELEMENT_NOT_FOUND[]                          = TEXT("ELEMENT_NOT_FOUND");
    inline constexpr TCHAR ERR_EMITTER_DATA_MISSING[]                       = TEXT("EMITTER_DATA_MISSING");
    inline constexpr TCHAR ERR_EMITTER_GRAPH_SOURCE_MISSING[]               = TEXT("EMITTER_GRAPH_SOURCE_MISSING");
    inline constexpr TCHAR ERR_EMITTER_HANDLE_NOT_FOUND[]                   = TEXT("EMITTER_HANDLE_NOT_FOUND");
    inline constexpr TCHAR ERR_EMITTER_NOT_FOUND[]                          = TEXT("EMITTER_NOT_FOUND");
    // niagara.add_emitter under its default inherit:true. UNiagaraSystem::AddEmitterHandle copies
    // the source emitter into the system and then either KEEPS the link back to it - the child's
    // VersionedParent / VersionedParentAtLastMerge, set by UNiagaraEmitter::CreateWithParentAndOwner
    // - or strips it when the source asset declares itself non-inheritable (bIsInheritable false,
    // NiagaraSystem.cpp:3023). Stripped means a frozen snapshot no later edit of the source asset
    // can ever reach, which the pre-fix verb reported as an ordinary success. Every stock Niagara
    // template and behaviour-example emitter loads that way, and asset.duplicate preserves it, so
    // "duplicate a template, author it, wire it up, keep editing it" hit this every time. The
    // handle is rolled back out of the system before this is sent. Two ways forward, both in the
    // message: pass inherit:false to take the snapshot deliberately, or set bIsInheritable on the
    // emitter asset (which is exactly what the editor's own emitter-creation wizard does to the
    // asset it makes from a template, NiagaraEmitterFactoryNew.cpp:183).
    inline constexpr TCHAR ERR_EMITTER_NOT_INHERITABLE[]                    = TEXT("EMITTER_NOT_INHERITABLE");
    // niagara.refresh_emitter: the named handle - or, with no `emitter` named, every handle in the
    // system - carries no parent, so there is nothing to merge from. A snapshot handle is not
    // refreshable at all; its only route back to the source asset is remove_emitter + add_emitter,
    // which discards any edit made to the system's own copy.
    inline constexpr TCHAR ERR_EMITTER_NOT_INHERITED[]                      = TEXT("EMITTER_NOT_INHERITED");
    inline constexpr TCHAR ERR_EMITTER_ONLY_UNSUPPORTED[]                   = TEXT("EMITTER_ONLY_UNSUPPORTED");
    inline constexpr TCHAR ERR_EMITTER_REQUIRED[]                           = TEXT("EMITTER_REQUIRED");
    inline constexpr TCHAR ERR_ENCODE_FAILED[]                              = TEXT("ENCODE_FAILED");
    inline constexpr TCHAR ERR_ENTRY_NODE_NOT_FOUND[]                       = TEXT("ENTRY_NODE_NOT_FOUND");
    inline constexpr TCHAR ERR_ENUM_NOT_FOUND[]                             = TEXT("ENUM_NOT_FOUND");
    inline constexpr TCHAR ERR_ENUM_NOT_RESOLVED[]                          = TEXT("ENUM_NOT_RESOLVED");
    inline constexpr TCHAR ERR_ENUM_UPDATE_FAILED[]                         = TEXT("ENUM_UPDATE_FAILED");
    inline constexpr TCHAR ERR_ENUM_VALUE_NOT_FOUND[]                       = TEXT("ENUM_VALUE_NOT_FOUND");
    inline constexpr TCHAR ERR_EVAL_FAILED[]                                = TEXT("EVAL_FAILED");
    inline constexpr TCHAR ERR_EVENT_HANDLER_INVALID_INDEX[]                = TEXT("EVENT_HANDLER_INVALID_INDEX");
    inline constexpr TCHAR ERR_EVENT_HANDLER_NOT_FOUND[]                    = TEXT("EVENT_HANDLER_NOT_FOUND");
    inline constexpr TCHAR ERR_EVENT_NOT_FOUND[]                            = TEXT("EVENT_NOT_FOUND");
    inline constexpr TCHAR ERR_EXECUTION_ERROR[]                            = TEXT("EXECUTION_ERROR");
    inline constexpr TCHAR ERR_EXECUTION_FAILED[]                           = TEXT("EXECUTION_FAILED");
    inline constexpr TCHAR ERR_EXEC_FAILED[]                                = TEXT("EXEC_FAILED");
    // A capture verb was asked to pin exposure and no finalized scene view observed the override.
    // Distinct from the softer "written but the render mode ignores it" case, which is
    // reported as viewport.exposure.pinned=false plus a pinWarning on an otherwise successful
    // capture — the frame is still usable for everything except comparison. Here the mechanism
    // itself failed, so no frame is returned rather than one that quietly used auto-exposure.
    inline constexpr TCHAR ERR_EXPOSURE_PIN_FAILED[]                        = TEXT("EXPOSURE_PIN_FAILED");
    inline constexpr TCHAR ERR_EXPORT_FAILED[]                              = TEXT("EXPORT_FAILED");
    inline constexpr TCHAR ERR_EXPORT_TIMED_OUT[]                           = TEXT("EXPORT_TIMED_OUT");
    inline constexpr TCHAR ERR_EXPRESSION_CREATION_FAILED[]                 = TEXT("EXPRESSION_CREATION_FAILED");
    inline constexpr TCHAR ERR_EXPRESSION_NOT_FOUND[]                       = TEXT("EXPRESSION_NOT_FOUND");
    inline constexpr TCHAR ERR_FACTORY_CREATION_FAILED[]                    = TEXT("FACTORY_CREATION_FAILED");
    inline constexpr TCHAR ERR_FACTORY_FAILED[]                             = TEXT("FACTORY_FAILED");
    inline constexpr TCHAR ERR_FACTORY_NOT_AVAILABLE[]                      = TEXT("FACTORY_NOT_AVAILABLE");
    inline constexpr TCHAR ERR_FIELD_NOT_FOUND[]                            = TEXT("FIELD_NOT_FOUND");
    inline constexpr TCHAR ERR_FILE_NOT_FOUND[]                             = TEXT("FILE_NOT_FOUND");
    inline constexpr TCHAR ERR_FIXED_SIZE_CAPTURE_UNAVAILABLE[]             = TEXT("FIXED_SIZE_CAPTURE_UNAVAILABLE");
    inline constexpr TCHAR ERR_FOLIAGE_ACTOR_FAILED[]                       = TEXT("FOLIAGE_ACTOR_FAILED");
    inline constexpr TCHAR ERR_FOLIAGE_ACTOR_NOT_FOUND[]                    = TEXT("FOLIAGE_ACTOR_NOT_FOUND");
    inline constexpr TCHAR ERR_FUNCTION_NOT_FOUND[]                         = TEXT("FUNCTION_NOT_FOUND");
    inline constexpr TCHAR ERR_GAME_FEATURES_NOT_AVAILABLE[]                = TEXT("GAME_FEATURES_NOT_AVAILABLE");
    inline constexpr TCHAR ERR_GAME_INSTANCE_NOT_FOUND[]                    = TEXT("GAME_INSTANCE_NOT_FOUND");
    inline constexpr TCHAR ERR_GAME_MODE_NOT_FOUND[]                        = TEXT("GAME_MODE_NOT_FOUND");
    inline constexpr TCHAR ERR_GAME_STATE_NOT_FOUND[]                       = TEXT("GAME_STATE_NOT_FOUND");
    inline constexpr TCHAR ERR_GAS_NOT_AVAILABLE[]                          = TEXT("GAS_NOT_AVAILABLE");
    // image.compare: both inputs carry a georeference and the two describe different ground
    // (axes, world extent, subdivision or depth plane). Distinct from IMAGE_SIZE_MISMATCH, which
    // is about pixels: two images CAN be the same ground at different resolutions, and the caller
    // acts differently on each - re-capture vs. re-georeference.
    inline constexpr TCHAR ERR_GEOREFERENCE_MISMATCH[]                      = TEXT("GEOREFERENCE_MISMATCH");
    inline constexpr TCHAR ERR_GRAPH_EDITOR_NOT_FOUND[]                     = TEXT("GRAPH_EDITOR_NOT_FOUND");
    inline constexpr TCHAR ERR_GRAPH_ERROR[]                                = TEXT("GRAPH_ERROR");
    inline constexpr TCHAR ERR_GRAPH_NOT_FOUND[]                            = TEXT("GRAPH_NOT_FOUND");
    inline constexpr TCHAR ERR_GRAPH_UNAVAILABLE[]                          = TEXT("GRAPH_UNAVAILABLE");
    // spatial.ground_actors / spatial.verify_grounding ground-probe outcomes. The three
    // "no ground" cases are deliberately separate because they demand different fixes:
    // something is in the way, nothing is there, or nothing was ever measured.
    //
    // GROUND_HITS_ALL_REJECTED means geometry WAS hit under every sampled column and the
    // surface filter refused all of it - the response names what it refused, which is how a
    // probe that landed on a collisionless fog card becomes visible instead of silent.
    inline constexpr TCHAR ERR_GROUND_HITS_ALL_REJECTED[]                   = TEXT("GROUND_HITS_ALL_REJECTED");
    inline constexpr TCHAR ERR_GROUND_NOT_FOUND[]                           = TEXT("GROUND_NOT_FOUND");
    // No measurement was taken at all (no world, no actor, no bounds). Never reported as a
    // pass: an unmeasured check is not a passed check.
    inline constexpr TCHAR ERR_GROUND_NOT_MEASURED[]                        = TEXT("GROUND_NOT_MEASURED");
    // The post-move readback disagrees with what the seat solve predicted. Catches the actor
    // that did not actually move and the ground answer that was not reproducible.
    inline constexpr TCHAR ERR_GROUND_SEAT_READBACK_MISMATCH[]              = TEXT("GROUND_SEAT_READBACK_MISMATCH");
    // image.annotate: the requested world grid spacing would draw more lines than
    // PinWrightImage::MaxGridLinesPerAxis across the frame. Refused rather than drawn: past
    // roughly one line per pixel the overlay IS the image, which is the buried-detail failure
    // that made low-opacity grids mandatory in the first place.
    inline constexpr TCHAR ERR_GRID_TOO_DENSE[]                             = TEXT("GRID_TOO_DENSE");
    inline constexpr TCHAR ERR_HEIGHT_READ_FAILED[]                         = TEXT("HEIGHT_READ_FAILED");
    // spatial.ground_actors / spatial.verify_grounding: the target actor is an ISM/HISM scatter
    // holder. Its world bounds are the union of EVERY instance, so its "footprint" is a whole
    // scatter and its "underside" is a surface no instance has - a verdict measured from them
    // describes nothing, and a move derived from them relocates every instance at once. Both
    // verbs refuse it with this code, and the refusal names the offending component.
    inline constexpr TCHAR ERR_HOLDER_NOT_SEATABLE[]                        = TEXT("HOLDER_NOT_SEATABLE");
    inline constexpr TCHAR ERR_HOST_FAILED[]                                = TEXT("HOST_FAILED");
    // image.*: the image's pixel dimensions do not divide into the georeference's cols x rows
    // whole tiles. A partial edge tile is unrepresentable (see Render/TileGridUtils.h) and
    // rounding one away moves the seam a reader measures across, so it is refused.
    inline constexpr TCHAR ERR_IMAGE_GRID_MISMATCH[]                        = TEXT("IMAGE_GRID_MISMATCH");
    // image.compare: the two images differ in pixel dimensions, so no pixel of one corresponds
    // to a pixel of the other and a difference composite would be meaningless.
    inline constexpr TCHAR ERR_IMAGE_SIZE_MISMATCH[]                        = TEXT("IMAGE_SIZE_MISMATCH");
    inline constexpr TCHAR ERR_IMMUTABLE_NODE[]                             = TEXT("IMMUTABLE_NODE");
    inline constexpr TCHAR ERR_IMPORT_FAILED[]                              = TEXT("IMPORT_FAILED");
    inline constexpr TCHAR ERR_INCOMPATIBLE_CURVE_ASSET[]                   = TEXT("INCOMPATIBLE_CURVE_ASSET");
    inline constexpr TCHAR ERR_INCOMPATIBLE_DATA_INTERFACE[]                = TEXT("INCOMPATIBLE_DATA_INTERFACE");
    inline constexpr TCHAR ERR_INCOMPATIBLE_SCRIPT_USAGE[]                  = TEXT("INCOMPATIBLE_SCRIPT_USAGE");
    inline constexpr TCHAR ERR_INCOMPATIBLE_STACK_GROUP[]                   = TEXT("INCOMPATIBLE_STACK_GROUP");
    inline constexpr TCHAR ERR_INDEX_MALFORMED[]                            = TEXT("INDEX_MALFORMED");
    inline constexpr TCHAR ERR_INDEX_NOT_FOUND[]                            = TEXT("INDEX_NOT_FOUND");
    inline constexpr TCHAR ERR_INDEX_OUT_OF_RANGE[]                         = TEXT("INDEX_OUT_OF_RANGE");
    inline constexpr TCHAR ERR_INDEX_PARSE_ERROR[]                          = TEXT("INDEX_PARSE_ERROR");
    inline constexpr TCHAR ERR_INPUT_ACTION_PROPERTY_NOT_FOUND[]            = TEXT("INPUT_ACTION_PROPERTY_NOT_FOUND");
    inline constexpr TCHAR ERR_INPUT_FAILED[]                               = TEXT("INPUT_FAILED");
    inline constexpr TCHAR ERR_INPUT_NOT_FOUND[]                            = TEXT("INPUT_NOT_FOUND");
    // spatial.verify_grounding: the actor touches the surface at fewer footprint columns than
    // required. One contact point on a wide actor is the balanced-boulder signature - it is a
    // physically valid rest and a visually wrong result, so it needs its own code.
    inline constexpr TCHAR ERR_INSUFFICIENT_GROUND_CONTACT[]                = TEXT("INSUFFICIENT_GROUND_CONTACT");
    inline constexpr TCHAR ERR_INTEGRITY_FAILURE[]                           = TEXT("INTEGRITY_FAILURE");
    // actor.get_instances / actor.set_instance_transforms / spatial.ground_instances: an
    // instance index outside [0, GetInstanceCount()). Instances are addressed positionally and a
    // re-scatter renumbers them, so a stale index is the expected failure and it must never be
    // clamped into a neighbour - the write verbs refuse the WHOLE batch on one bad index.
    inline constexpr TCHAR ERR_INSTANCE_INDEX_OUT_OF_RANGE[]                = TEXT("INSTANCE_INDEX_OUT_OF_RANGE");
    inline constexpr TCHAR ERR_INTERFACE_ERROR[]                            = TEXT("INTERFACE_ERROR");
    inline constexpr TCHAR ERR_INTERFACE_MUTATION_FAILED[]                  = TEXT("INTERFACE_MUTATION_FAILED");
    inline constexpr TCHAR ERR_INTERFACE_NOT_FOUND[]                        = TEXT("INTERFACE_NOT_FOUND");
    inline constexpr TCHAR ERR_INTERFACE_NOT_REMOVABLE[]                    = TEXT("INTERFACE_NOT_REMOVABLE");
    inline constexpr TCHAR ERR_INTERNAL_ERROR[]                             = TEXT("INTERNAL_ERROR");
    inline constexpr TCHAR ERR_INVALID_ANIM_NODE_CLASS[]                    = TEXT("INVALID_ANIM_NODE_CLASS");
    // UE's FActorEditorUtils::ValidateActorName rejected the requested display label, so
    // SetActorLabel left the previous label in place. Detected by reading the label back.
    inline constexpr TCHAR ERR_INVALID_ACTOR_LABEL[]                        = TEXT("INVALID_ACTOR_LABEL");
    inline constexpr TCHAR ERR_INVALID_ARGUMENT[]                           = TEXT("INVALID_ARGUMENT");
    inline constexpr TCHAR ERR_INVALID_ASSET[]                              = TEXT("INVALID_ASSET");
    inline constexpr TCHAR ERR_INVALID_ASSET_PATH[]                         = TEXT("INVALID_ASSET_PATH");
    inline constexpr TCHAR ERR_INVALID_ASSET_TYPE[]                         = TEXT("INVALID_ASSET_TYPE");
    inline constexpr TCHAR ERR_INVALID_BLEND_MODE[]                         = TEXT("INVALID_BLEND_MODE");
    inline constexpr TCHAR ERR_INVALID_BLUEPRINT[]                          = TEXT("INVALID_BLUEPRINT");
    inline constexpr TCHAR ERR_INVALID_BLUEPRINT_CANDIDATES[]               = TEXT("INVALID_BLUEPRINT_CANDIDATES");
    inline constexpr TCHAR ERR_INVALID_BLUEPRINT_PATH[]                     = TEXT("INVALID_BLUEPRINT_PATH");
    inline constexpr TCHAR ERR_INVALID_BLUEPRINT_TYPE[]                     = TEXT("INVALID_BLUEPRINT_TYPE");
    inline constexpr TCHAR ERR_INVALID_BONE_CONTROL_SPACE[]                 = TEXT("INVALID_BONE_CONTROL_SPACE");
    inline constexpr TCHAR ERR_INVALID_BONE_MODIFICATION_MODE[]             = TEXT("INVALID_BONE_MODIFICATION_MODE");
    inline constexpr TCHAR ERR_INVALID_BP[]                                 = TEXT("INVALID_BP");
    inline constexpr TCHAR ERR_INVALID_CATEGORY[]                           = TEXT("INVALID_CATEGORY");
    inline constexpr TCHAR ERR_INVALID_CHILD_INDEX[]                        = TEXT("INVALID_CHILD_INDEX");
    inline constexpr TCHAR ERR_INVALID_CLASS[]                              = TEXT("INVALID_CLASS");
    inline constexpr TCHAR ERR_INVALID_CLASS_TYPE[]                         = TEXT("INVALID_CLASS_TYPE");
    inline constexpr TCHAR ERR_INVALID_COLLISION_COMPLEXITY[]               = TEXT("INVALID_COLLISION_COMPLEXITY");
    inline constexpr TCHAR ERR_INVALID_COMPILE_FLAG[]                       = TEXT("INVALID_COMPILE_FLAG");
    inline constexpr TCHAR ERR_INVALID_CONTEXT[]                            = TEXT("INVALID_CONTEXT");
    inline constexpr TCHAR ERR_INVALID_DATATABLE[]                          = TEXT("INVALID_DATATABLE");
    inline constexpr TCHAR ERR_INVALID_DATA_INTERFACE_CLASS[]               = TEXT("INVALID_DATA_INTERFACE_CLASS");
    inline constexpr TCHAR ERR_INVALID_DRAW_AS[]                            = TEXT("INVALID_DRAW_AS");
    inline constexpr TCHAR ERR_INVALID_EFFECT_CLASS[]                       = TEXT("INVALID_EFFECT_CLASS");
    inline constexpr TCHAR ERR_INVALID_EXECUTOR_CLASS[]                     = TEXT("INVALID_EXECUTOR_CLASS");
    // image.*: the `georeference` object is missing, carries an unknown key, names an unknown
    // axes preset, or does not describe a usable tile grid. One code for the whole object because
    // the recovery is the same in every case - read image.md and re-issue the object - and the
    // message names the offending field.
    inline constexpr TCHAR ERR_INVALID_GEOREFERENCE[]                       = TEXT("INVALID_GEOREFERENCE");
    inline constexpr TCHAR ERR_INVALID_GI_METHOD[]                          = TEXT("INVALID_GI_METHOD");
    inline constexpr TCHAR ERR_INVALID_GRAPH[]                              = TEXT("INVALID_GRAPH");
    inline constexpr TCHAR ERR_INVALID_GUID[]                               = TEXT("INVALID_GUID");
    inline constexpr TCHAR ERR_INVALID_INDEX[]                              = TEXT("INVALID_INDEX");
    inline constexpr TCHAR ERR_INVALID_INHIBITION_POLICY[]                  = TEXT("INVALID_INHIBITION_POLICY");
    inline constexpr TCHAR ERR_INVALID_INPUTS[]                             = TEXT("INVALID_INPUTS");
    inline constexpr TCHAR ERR_INVALID_INPUT_VALUE[]                        = TEXT("INVALID_INPUT_VALUE");
    inline constexpr TCHAR ERR_INVALID_INTERFACE_CLASS[]                    = TEXT("INVALID_INTERFACE_CLASS");
    inline constexpr TCHAR ERR_INVALID_INTERPOLATION_TYPE[]                 = TEXT("INVALID_INTERPOLATION_TYPE");
    inline constexpr TCHAR ERR_INVALID_JSON[]                               = TEXT("INVALID_JSON");
    inline constexpr TCHAR ERR_INVALID_KEY[]                                = TEXT("INVALID_KEY");
    inline constexpr TCHAR ERR_INVALID_KIND[]                               = TEXT("INVALID_KIND");
    inline constexpr TCHAR ERR_INVALID_LANDSCAPE[]                          = TEXT("INVALID_LANDSCAPE");
    inline constexpr TCHAR ERR_INVALID_LAYERS[]                             = TEXT("INVALID_LAYERS");
    inline constexpr TCHAR ERR_INVALID_LIGHT_TYPE[]                         = TEXT("INVALID_LIGHT_TYPE");
    inline constexpr TCHAR ERR_INVALID_LOD[]                                = TEXT("INVALID_LOD");
    inline constexpr TCHAR ERR_INVALID_LOD_INDEX[]                          = TEXT("INVALID_LOD_INDEX");
    inline constexpr TCHAR ERR_INVALID_LOGIC_TYPE[]                         = TEXT("INVALID_LOGIC_TYPE");
    // image.*: a file passed as `georeferenceFrom` is not readable JSON, is not an image.tile
    // manifest, or does not carry the tile that was selected from it.
    inline constexpr TCHAR ERR_INVALID_MANIFEST[]                           = TEXT("INVALID_MANIFEST");
    inline constexpr TCHAR ERR_INVALID_MATERIAL_INDEX[]                     = TEXT("INVALID_MATERIAL_INDEX");
    inline constexpr TCHAR ERR_INVALID_MODE[]                               = TEXT("INVALID_MODE");
    inline constexpr TCHAR ERR_INVALID_NAME[]                               = TEXT("INVALID_NAME");
    inline constexpr TCHAR ERR_INVALID_NODE_TYPE[]                          = TEXT("INVALID_NODE_TYPE");
    inline constexpr TCHAR ERR_INVALID_NOISE_TYPE[]                         = TEXT("INVALID_NOISE_TYPE");
    // bend / twist / taper: the mesh's primary normal overlay is in a state the engine's space
    // deformers read without checking. Either the attribute set carries ZERO normal layers
    // (PrimaryNormals() is null and FMeshSpaceDeformerOp subclasses call MaxElementID() on it
    // unconditionally), or an allocated overlay element still has no parent vertex after the
    // op's own FreeUnusedElements() repair. Both are an out-of-bounds read in a shipping build,
    // not an assert - see the guard in GeometryOps_Modeling.cpp.
    inline constexpr TCHAR ERR_INVALID_NORMAL_OVERLAY[]                     = TEXT("INVALID_NORMAL_OVERLAY");
    inline constexpr TCHAR ERR_INVALID_OBJECT[]                             = TEXT("INVALID_OBJECT");
    inline constexpr TCHAR ERR_INVALID_OP[]                                 = TEXT("INVALID_OP");
    inline constexpr TCHAR ERR_INVALID_OPERATION[]                          = TEXT("INVALID_OPERATION");
    inline constexpr TCHAR ERR_INVALID_OPERATIONS[]                         = TEXT("INVALID_OPERATIONS");
    inline constexpr TCHAR ERR_INVALID_OPERATION_PAYLOAD[]                  = TEXT("INVALID_OPERATION_PAYLOAD");
    inline constexpr TCHAR ERR_INVALID_OPERATION_TYPE[]                     = TEXT("INVALID_OPERATION_TYPE");
    inline constexpr TCHAR ERR_INVALID_OVERRIDE[]                           = TEXT("INVALID_OVERRIDE");
    inline constexpr TCHAR ERR_INVALID_PARAM[]                              = TEXT("INVALID_PARAM");
    inline constexpr TCHAR ERR_INVALID_PARAMETER[]                          = TEXT("INVALID_PARAMETER");
    inline constexpr TCHAR ERR_INVALID_PARAMETER_TYPE[]                     = TEXT("INVALID_PARAMETER_TYPE");
    inline constexpr TCHAR ERR_INVALID_PARAMS[]                             = TEXT("INVALID_PARAMS");
    inline constexpr TCHAR ERR_INVALID_PARAM_TYPE[]                         = TEXT("INVALID_PARAM_TYPE");
    inline constexpr TCHAR ERR_INVALID_PARENT[]                             = TEXT("INVALID_PARENT");
    inline constexpr TCHAR ERR_INVALID_PARENT_CLASS[]                       = TEXT("INVALID_PARENT_CLASS");
    inline constexpr TCHAR ERR_INVALID_PARTITION_TYPE[]                     = TEXT("INVALID_PARTITION_TYPE");
    inline constexpr TCHAR ERR_INVALID_PATH[]                               = TEXT("INVALID_PATH");
    // A name-match pattern that does not compile under matchMode:"regex"
    // (Utils/NameMatchFilter.h). Deliberately distinct from the INVALID_ARGUMENT
    // family: ICU swallows regex compile errors, so an unrejected bad pattern
    // silently matches NOTHING and the caller reads that as a real zero-result
    // count.
    inline constexpr TCHAR ERR_INVALID_PATTERN[]                            = TEXT("INVALID_PATTERN");
    inline constexpr TCHAR ERR_INVALID_PAYLOAD[]                            = TEXT("INVALID_PAYLOAD");
    inline constexpr TCHAR ERR_INVALID_PIN[]                                = TEXT("INVALID_PIN");
    inline constexpr TCHAR ERR_INVALID_PIN_DEFAULT[]                        = TEXT("INVALID_PIN_DEFAULT");
    inline constexpr TCHAR ERR_INVALID_PLACEMENT[]                          = TEXT("INVALID_PLACEMENT");
    inline constexpr TCHAR ERR_INVALID_PROPERTY[]                           = TEXT("INVALID_PROPERTY");
    inline constexpr TCHAR ERR_INVALID_PROVIDER[]                           = TEXT("INVALID_PROVIDER");
    inline constexpr TCHAR ERR_INVALID_PRUNING_TYPE[]                       = TEXT("INVALID_PRUNING_TYPE");
    inline constexpr TCHAR ERR_INVALID_QUERY[]                              = TEXT("INVALID_QUERY");
    // A synth recipe document that does not parse, or parses into a structure the compiler
    // rejects (no layers, a layer with no generator, contradictory durations). Deliberately
    // not one of the INVALID_PARAM* family: the recipe is one argument whose INTERNAL shape is
    // wrong, so the error payload names the offending layer rather than the parameter.
    inline constexpr TCHAR ERR_INVALID_RECIPE[]                             = TEXT("INVALID_RECIPE");
    inline constexpr TCHAR ERR_INVALID_REFERENCED_ASSET[]                   = TEXT("INVALID_REFERENCED_ASSET");
    inline constexpr TCHAR ERR_INVALID_RENDERER_CLASS[]                     = TEXT("INVALID_RENDERER_CLASS");
    inline constexpr TCHAR ERR_INVALID_RESOLUTION_RULE[]                    = TEXT("INVALID_RESOLUTION_RULE");
    // networking RPC authoring/setters: the requested direction, return signature, reliability,
    // or validation flag cannot form a valid Server, Client, or NetMulticast RPC.
    inline constexpr TCHAR ERR_INVALID_RPC_CONFIGURATION[]                  = TEXT("INVALID_RPC_CONFIGURATION");
    inline constexpr TCHAR ERR_INVALID_ROW_STRUCT[]                         = TEXT("INVALID_ROW_STRUCT");
    inline constexpr TCHAR ERR_INVALID_ROW_VALUES[]                         = TEXT("INVALID_ROW_VALUES");
    inline constexpr TCHAR ERR_INVALID_SAVE_FLAG[]                          = TEXT("INVALID_SAVE_FLAG");
    inline constexpr TCHAR ERR_INVALID_SEGMENT_ANIMATION[]                  = TEXT("INVALID_SEGMENT_ANIMATION");
    inline constexpr TCHAR ERR_INVALID_SEQUENCE[]                           = TEXT("INVALID_SEQUENCE");
    inline constexpr TCHAR ERR_INVALID_SEQUENCE_TYPE[]                      = TEXT("INVALID_SEQUENCE_TYPE");
    inline constexpr TCHAR ERR_INVALID_SHAPE[]                              = TEXT("INVALID_SHAPE");
    inline constexpr TCHAR ERR_INVALID_SIMULATION_STAGE_CLASS[]             = TEXT("INVALID_SIMULATION_STAGE_CLASS");
    inline constexpr TCHAR ERR_INVALID_SOUND_GROUP[]                        = TEXT("INVALID_SOUND_GROUP");
    inline constexpr TCHAR ERR_INVALID_STACK[]                              = TEXT("INVALID_STACK");
    inline constexpr TCHAR ERR_INVALID_STAGE[]                              = TEXT("INVALID_STAGE");
    inline constexpr TCHAR ERR_INVALID_STATE[]                              = TEXT("INVALID_STATE");
    inline constexpr TCHAR ERR_INVALID_STRUCT[]                             = TEXT("INVALID_STRUCT");
    inline constexpr TCHAR ERR_INVALID_SUBGRAPH_ASSET[]                     = TEXT("INVALID_SUBGRAPH_ASSET");
    // spatial.ground_actors / spatial.verify_grounding: the required `surface` argument named
    // an unknown preset or was otherwise unusable. Rejected rather than defaulted - "which
    // surface did you mean" is the question every observed ground-placement failure got wrong,
    // so it is the one thing the verb will not guess at.
    inline constexpr TCHAR ERR_INVALID_SURFACE_SPEC[]                       = TEXT("INVALID_SURFACE_SPEC");
    inline constexpr TCHAR ERR_INVALID_TAG[]                                = TEXT("INVALID_TAG");
    inline constexpr TCHAR ERR_INVALID_TARGET[]                             = TEXT("INVALID_TARGET");
    inline constexpr TCHAR ERR_INVALID_TARGET_KIND[]                        = TEXT("INVALID_TARGET_KIND");
    inline constexpr TCHAR ERR_INVALID_TEXT_LOCALIZATION_IDENTITY[]         = TEXT("INVALID_TEXT_LOCALIZATION_IDENTITY");
    inline constexpr TCHAR ERR_INVALID_THRESHOLD[]                          = TEXT("INVALID_THRESHOLD");
    inline constexpr TCHAR ERR_INVALID_TRANSFORM_PAYLOAD[]                  = TEXT("INVALID_TRANSFORM_PAYLOAD");
    inline constexpr TCHAR ERR_INVALID_TRIANGLE[]                           = TEXT("INVALID_TRIANGLE");
    inline constexpr TCHAR ERR_INVALID_TYPE[]                               = TEXT("INVALID_TYPE");
    inline constexpr TCHAR ERR_INVALID_VALUE[]                              = TEXT("INVALID_VALUE");
    inline constexpr TCHAR ERR_INVALID_VERTEX[]                             = TEXT("INVALID_VERTEX");
    inline constexpr TCHAR ERR_INVALID_VISIBILITY[]                         = TEXT("INVALID_VISIBILITY");
    inline constexpr TCHAR ERR_INVALID_WATER_BODY_TYPE[]                    = TEXT("INVALID_WATER_BODY_TYPE");
    inline constexpr TCHAR ERR_INVALID_WHEEL_CLASS[]                        = TEXT("INVALID_WHEEL_CLASS");
    inline constexpr TCHAR ERR_INVALID_XML[]                                = TEXT("INVALID_XML");
    // system.job_cancel was asked to stop a job whose verb registered no cancel callback, so
    // NOTHING can stop the work. Deliberately an error and not a success-with-a-flag: cancelling
    // is what an agent does in response to a hang, and a success result there is read as "the
    // editor is idle now" while ~19,700 files keep being written. Distinct from
    // JOB_NOT_CANCELLABLE-style states below: the ticket is alive and running, the request is
    // well-formed, and the only thing missing is the capability.
    inline constexpr TCHAR ERR_JOB_CANCEL_UNSUPPORTED[]                     = TEXT("JOB_CANCEL_UNSUPPORTED");
    // The ticket exists but has already reached a terminal state (completed / failed / cancelled),
    // so there is nothing left to cancel. Distinct from TICKET_NOT_FOUND (no such ticket, or it
    // was evicted) because the recoveries differ: here system.job_status still has the result.
    inline constexpr TCHAR ERR_JOB_NOT_RUNNING[]                            = TEXT("JOB_NOT_RUNNING");
    inline constexpr TCHAR ERR_KEY_NOT_FOUND[]                              = TEXT("KEY_NOT_FOUND");
    // The landscape's material declares zero paintable target layers (no
    // LandscapeLayerBlend / LandscapeLayerWeight node, or one with no named layers), so
    // NO layer name can be painted on it. Distinct from LAYER_NOT_FOUND, whose recovery
    // is "use one of these other names": here there are none and the material itself
    // must be fixed. Error data carries materialPath + an empty availableLayers[].
    // landscape.sculpt: toolMode is not one of Raise / Lower / Flatten / Smooth. The verb
    // used to match the three it knew and let anything else fall through to a zero delta,
    // returning success with modifiedVertices 0 - so a typo was indistinguishable from a
    // stamp on already-correct terrain. Error data carries validToolModes[].
    inline constexpr TCHAR ERR_LANDSCAPE_INVALID_TOOL_MODE[]                = TEXT("LANDSCAPE_INVALID_TOOL_MODE");
    inline constexpr TCHAR ERR_LANDSCAPE_MATERIAL_NO_LAYERS[]               = TEXT("LANDSCAPE_MATERIAL_NO_LAYERS");
    inline constexpr TCHAR ERR_LANDSCAPE_NOT_FOUND[]                        = TEXT("LANDSCAPE_NOT_FOUND");
    inline constexpr TCHAR ERR_LANDSCAPE_NO_COMPONENTS[]                    = TEXT("LANDSCAPE_NO_COMPONENTS");
    // landscape.get_heights / landscape.audit_shape: the region resolved and the landscape has
    // components, but FLandscapeEditDataInterface::GetHeightData found none covering it and
    // filled the whole buffer by interpolation. Distinct from LANDSCAPE_NO_COMPONENTS (a hollow
    // actor, nothing anywhere) because the actor here is fine and only this region is unreadable.
    // Refused rather than answered: the fill is legal uint16 data, and the neutral value 32768
    // publishes as world Z 0, which reads exactly like flat ground at sea level.
    inline constexpr TCHAR ERR_LANDSCAPE_NO_HEIGHT_DATA[]                   = TEXT("LANDSCAPE_NO_HEIGHT_DATA");
    // No landscape material assigned at all. Separate from LANDSCAPE_MATERIAL_NO_LAYERS
    // because ALandscapeProxy::GetLandscapeMaterial() silently substitutes the engine
    // default surface material, so the two states are indistinguishable downstream and
    // have different recoveries (assign a material vs. add layers to the one you have).
    inline constexpr TCHAR ERR_LANDSCAPE_NO_MATERIAL[]                      = TEXT("LANDSCAPE_NO_MATERIAL");
    // landscape.create_procedural_terrain: the landscape carries weightmap allocations for a layer
    // that is NOT a registered target layer, so the edit-layer merge this paint triggers would
    // delete them from every component of every LOADED proxy - landscape-wide, independent of
    // `region` and of `strength`, and NOT reversible with editor.undo (the merge runs inside the
    // settle, after the paint's transaction has closed). Refused rather than warned because the
    // all-layers census cannot see the loss either: it enumerates the REGISTRATION side, so it
    // reports otherLayerTexelsLost 0 over a landscape-wide erase
    // (B-paint-erases-orphaned-layer). Error data carries orphanedLayers[] - layerName,
    // layerInfoPath, componentCount - plus the scan's component/proxy counts and the
    // loaded-proxies-only flag. Re-register the layer, or pass allowOrphanedLayerLoss=true to
    // accept the erasure deliberately. An allocation whose LayerInfo is NULL is reported in
    // warnings[] instead: nothing can sample or read it back, so refusing on it is obstruction.
    inline constexpr TCHAR ERR_LANDSCAPE_ORPHANED_LAYER_WEIGHT[]            = TEXT("LANDSCAPE_ORPHANED_LAYER_WEIGHT");
    // landscape.sculpt: the stroke would not change the stored height of a single vertex.
    // Refused rather than answered as a zero-item success (rpc-design.md §3), because a
    // brushRadius below one cell, a strength of 0, a Flatten already at its target and a
    // stroke that missed the landscape all look identical to a clean run otherwise. Error
    // data carries the full success-shaped diagnostic block - region, verticesInBrush,
    // plannedVertices and a `reason` naming which of those cases it was. Pass
    // allowNoChange=true when a no-op is a legitimate outcome you want to observe.
    inline constexpr TCHAR ERR_LANDSCAPE_SCULPT_NO_CHANGE[]                 = TEXT("LANDSCAPE_SCULPT_NO_CHANGE");
    // landscape.audit_shape findings. AXIS_LOCKED: too much of the band boundary runs along
    // cell edges - what a tier value written per cell from an axis-aligned region mask
    // produces, independent of how steep the faces are. STEPPED: the region's height rise is
    // carried by isolated single-sample risers between flat runs, i.e. a per-cell staircase
    // rather than a sculpted slope. The remaining three are UNRUNNABLE reasons, never
    // findings: an audit that could not measure must not read as one that passed.
    inline constexpr TCHAR ERR_LANDSCAPE_SHAPE_AXIS_LOCKED[]                = TEXT("LANDSCAPE_SHAPE_AXIS_LOCKED");
    // Every sample in the region sits at one height, so there is no boundary and no rise to
    // measure. Reported over an empty denominator rather than answered as 0.000.
    inline constexpr TCHAR ERR_LANDSCAPE_SHAPE_FLAT_REGION[]                = TEXT("LANDSCAPE_SHAPE_FLAT_REGION");
    // The region holds a boundary, but too little of one at this stride to be a measurement:
    // a fraction over a handful of chords is noise wearing a decimal point.
    inline constexpr TCHAR ERR_LANDSCAPE_SHAPE_NO_BOUNDARY[]                = TEXT("LANDSCAPE_SHAPE_NO_BOUNDARY");
    // The sampled region is smaller than one chord, so no chord could be formed at all.
    inline constexpr TCHAR ERR_LANDSCAPE_SHAPE_REGION_TOO_SMALL[]           = TEXT("LANDSCAPE_SHAPE_REGION_TOO_SMALL");
    inline constexpr TCHAR ERR_LANDSCAPE_SHAPE_STEPPED[]                    = TEXT("LANDSCAPE_SHAPE_STEPPED");
    inline constexpr TCHAR ERR_LAYER_CREATION_FAILED[]                      = TEXT("LAYER_CREATION_FAILED");
    // A named landscape/paint layer does not exist on the target. The message lists the
    // names that DO exist and the error data carries availableLayers[] +
    // availableLayerCount; emitted instead of the old "painted successfully" response for
    // a layer no material declares (B-create-procedural-terrain-paints-nothing).
    inline constexpr TCHAR ERR_LAYER_NOT_FOUND[]                            = TEXT("LAYER_NOT_FOUND");
    inline constexpr TCHAR ERR_LEVEL_ALREADY_EXISTS[]                       = TEXT("LEVEL_ALREADY_EXISTS");
    inline constexpr TCHAR ERR_LEVEL_LOCKED[]                               = TEXT("LEVEL_LOCKED");
    inline constexpr TCHAR ERR_LEVEL_NOT_FOUND[]                            = TEXT("LEVEL_NOT_FOUND");
    inline constexpr TCHAR ERR_LEVEL_NOT_LOADED[]                           = TEXT("LEVEL_NOT_LOADED");
    inline constexpr TCHAR ERR_LEVEL_NOT_PERSISTED[]                        = TEXT("LEVEL_NOT_PERSISTED");
    inline constexpr TCHAR ERR_LINK_NOT_FOUND[]                             = TEXT("LINK_NOT_FOUND");
    inline constexpr TCHAR ERR_LIVE_CODING_COMPILE_CANCELLED[]              = TEXT("LIVE_CODING_COMPILE_CANCELLED");
    inline constexpr TCHAR ERR_LIVE_CODING_COMPILE_FAILED[]                 = TEXT("LIVE_CODING_COMPILE_FAILED");
    inline constexpr TCHAR ERR_LIVE_CODING_COMPILE_IN_PROGRESS[]            = TEXT("LIVE_CODING_COMPILE_IN_PROGRESS");
    inline constexpr TCHAR ERR_LIVE_CODING_NOT_AVAILABLE[]                  = TEXT("LIVE_CODING_NOT_AVAILABLE");
    inline constexpr TCHAR ERR_LIVE_CODING_NOT_ENABLED[]                    = TEXT("LIVE_CODING_NOT_ENABLED");
    inline constexpr TCHAR ERR_LIVE_CODING_NOT_STARTED[]                    = TEXT("LIVE_CODING_NOT_STARTED");
    // A Blueprint compile verb refused because loaded worlds hold live instances of the class:
    // the compile flushes the reinstancing queue, which destroys and re-creates each of them and
    // dirties the owning level. Opt in with allowReinstancing=true. The error payload carries the
    // same `reinstanced` block the success path reports (Handlers/Blueprint/BlueprintReinstancingGuard.h).
    inline constexpr TCHAR ERR_LIVE_INSTANCES_WOULD_BE_REINSTANCED[]        = TEXT("LIVE_INSTANCES_WOULD_BE_REINSTANCED");
    inline constexpr TCHAR ERR_LOAD_FAILED[]                                = TEXT("LOAD_FAILED");
    inline constexpr TCHAR ERR_MALFORMED_JSON[]                             = TEXT("MALFORMED_JSON");
    inline constexpr TCHAR ERR_MANAGER_NOT_FOUND[]                          = TEXT("MANAGER_NOT_FOUND");
    // asset.mark_dirty resolved the package but dirtying it is meaningless or
    // suppressed (transient / PIE duplicate / cooked / native script package, or the
    // editor is loading, transacting, cooking or async-loading). Distinct from
    // PACKAGE_NOT_FOUND, which means the path never resolved to a loaded package. The
    // message carries the printable reason; error data carries package + isDirty:false.
    inline constexpr TCHAR ERR_MARK_DIRTY_REFUSED[]                         = TEXT("MARK_DIRTY_REFUSED");
    // spatial.ground_actors: a pattern selector (prefix/filter) matched a different number of
    // actors than the caller's expectedMatches. Refused BEFORE any actor is moved; the response
    // names what matched, so the caller can see which actors are not theirs.
    inline constexpr TCHAR ERR_MATCH_COUNT_MISMATCH[]                       = TEXT("MATCH_COUNT_MISMATCH");
    // A capture subject is known from material-resource state to be drawing the engine Default
    // Material. The caller must opt in with allowFallback:true to keep that image as evidence.
    inline constexpr TCHAR ERR_MATERIAL_FALLBACK[]                          = TEXT("MATERIAL_FALLBACK");
    inline constexpr TCHAR ERR_MATERIAL_NOT_FOUND[]                         = TEXT("MATERIAL_NOT_FOUND");
    inline constexpr TCHAR ERR_MEMORY_PRESSURE[]                            = TEXT("MEMORY_PRESSURE");
    inline constexpr TCHAR ERR_MERGE_FAILED[]                               = TEXT("MERGE_FAILED");
    inline constexpr TCHAR ERR_MERGE_NOT_POSSIBLE[]                         = TEXT("MERGE_NOT_POSSIBLE");
    inline constexpr TCHAR ERR_MERGE_TOOL_MISSING[]                         = TEXT("MERGE_TOOL_MISSING");
    inline constexpr TCHAR ERR_MERGE_TOOL_UNAVAILABLE[]                     = TEXT("MERGE_TOOL_UNAVAILABLE");
    // actor.duplicate refused to return a copy whose dynamic-mesh triangle count
    // does not match the source: the engine silently substitutes a 12-triangle
    // placeholder cube (UDynamicMesh.cpp:568) and still reports success. Pass
    // allowPlaceholderMesh:true to accept the cube with a warning instead.
    // geometry.append_buffers: the engine's AppendBuffersToMesh REFUSED one or more of the
    // caller's triangles - invalid indices, non-manifold topology, or a duplicate of a triangle
    // already on the mesh. It reports each refusal only into its UGeometryScriptDebug argument
    // and then skips that triangle and keeps going, so before this code existed the verb answered
    // success over a mesh that was silently missing geometry. The op restores the mesh to its
    // pre-append state before emitting this, so the failure is atomic.
    inline constexpr TCHAR ERR_MESH_APPEND_FAILED[]                         = TEXT("MESH_APPEND_FAILED");
    // model.compile: a mesh rebuild would run underneath a live component whose scene proxy still
    // caches the asset's render data, which aborts the render thread on the next frame. The
    // rebuild is refused and the blocking components are named.
    inline constexpr TCHAR ERR_MESH_REBUILD_CONSUMER_NOT_QUIESCABLE[]       = TEXT("MESH_REBUILD_CONSUMER_NOT_QUIESCABLE");
    // ---- geometry.audit_static_meshes -----------------------------------------------------
    // One code per sweep CHECK, following the level.audit convention above: a caller keys on
    // the code rather than parsing a message. These are FINDING codes and never RPC errors -
    // the sweep SendSuccesses with pass:false, because no PinWright verb turns a content
    // defect into a transport failure. See Handlers/Geometry/MeshAuditUtils.h.
    // Triangles below GeometryUtils::DegenerateAreaEpsilon. The static-mesh build welds most
    // of them away; a JUMP in the count is the tell that two boolean operands were placed to
    // touch exactly rather than to overlap.
    inline constexpr TCHAR ERR_MESH_AUDIT_DEGENERATE[]                      = TEXT("MESH_AUDIT_DEGENERATE");
    // The asset read back with zero triangles - a mesh that ships as nothing at all.
    inline constexpr TCHAR ERR_MESH_AUDIT_EMPTY[]                           = TEXT("MESH_AUDIT_EMPTY");
    // Interior edges whose two triangles traverse the shared edge the SAME way: part of the
    // mesh is wound against the rest. Independent of MESH_AUDIT_INVERTED, which a uniformly
    // inverted (and therefore perfectly consistent) shell trips instead.
    inline constexpr TCHAR ERR_MESH_AUDIT_INCONSISTENT_WINDING[]            = TEXT("MESH_AUDIT_INCONSISTENT_WINDING");
    // A closed shell whose signed volume is negative: it is inside out. Invisible to every
    // other measurement - counts, boundary edges, bowties and components all match a correct
    // mesh, and backface culling shows the camera whichever wall faces it.
    inline constexpr TCHAR ERR_MESH_AUDIT_INVERTED[]                        = TEXT("MESH_AUDIT_INVERTED");
    // A connected component is open, degenerate or has near-zero volume, so its winding could
    // not be answered. It is an unrunnable finding, never a clean component.
    inline constexpr TCHAR ERR_MESH_AUDIT_COMPONENT_UNKNOWN[]               = TEXT("MESH_AUDIT_COMPONENT_UNKNOWN");
    // The asset's Build Scale has an odd number of negative axes, so the build mirrors the
    // mesh. Names the CAUSE when MESH_AUDIT_INVERTED fires on triangles that are themselves fine.
    inline constexpr TCHAR ERR_MESH_AUDIT_MIRRORED_BUILD_SCALE[]            = TEXT("MESH_AUDIT_MIRRORED_BUILD_SCALE");
    // Bowtie vertices: two or more triangle fans meeting at one vertex.
    inline constexpr TCHAR ERR_MESH_AUDIT_NON_MANIFOLD[]                    = TEXT("MESH_AUDIT_NON_MANIFOLD");
    // Edges adjacent to exactly one triangle. Correct for a card, a plane or an authored
    // shell, which is why this is a warning and not an error.
    inline constexpr TCHAR ERR_MESH_AUDIT_NOT_CLOSED[]                      = TEXT("MESH_AUDIT_NOT_CLOSED");
    // The asset loaded but its requested LOD would not copy into a measurable mesh. An
    // UNRUNNABLE reason, not a defect: the sweep says it could not look rather than passing.
    inline constexpr TCHAR ERR_MESH_AUDIT_READ_FAILED[]                     = TEXT("MESH_AUDIT_READ_FAILED");
    // A closed mesh enclosing negligible volume against its own surface area - a sheet folded
    // back on itself, which passes both the inversion and the closedness checks.
    inline constexpr TCHAR ERR_MESH_AUDIT_THIN_SHELL[]                      = TEXT("MESH_AUDIT_THIN_SHELL");
    // Separate shells that neither touch nor come within the model-derived proximity tolerance.
    // This is a warning-only measurement because deliberately-floating ornaments are valid.
    inline constexpr TCHAR ERR_MESH_AUDIT_FLOATING_COMPONENT[]              = TEXT("MESH_AUDIT_FLOATING_COMPONENT");
    // Overlapping near-coplanar triangles from separate shells, reported as a depth-buffer risk.
    inline constexpr TCHAR ERR_MESH_AUDIT_Z_FIGHTING[]                      = TEXT("MESH_AUDIT_Z_FIGHTING");
    // The mesh had triangles, but none were finite, non-degenerate surfaces suitable for the check.
    inline constexpr TCHAR ERR_MESH_AUDIT_Z_FIGHTING_UNRUNNABLE[]           = TEXT("MESH_AUDIT_Z_FIGHTING_UNRUNNABLE");
    // The matched asset would not resolve to a UStaticMesh. An UNRUNNABLE reason.
    inline constexpr TCHAR ERR_MESH_AUDIT_UNLOADABLE[]                      = TEXT("MESH_AUDIT_UNLOADABLE");
    inline constexpr TCHAR ERR_MESH_DUPLICATE_SUBSTITUTED[]                 = TEXT("MESH_DUPLICATE_SUBSTITUTED");
    inline constexpr TCHAR ERR_MESH_EMPTY[]                                 = TEXT("MESH_EMPTY");
    inline constexpr TCHAR ERR_MESH_NOT_FOUND[]                             = TEXT("MESH_NOT_FOUND");
    inline constexpr TCHAR ERR_METASOUND_DOCUMENT_INTERFACE_NOT_SUPPORTED[] = TEXT("METASOUND_DOCUMENT_INTERFACE_NOT_SUPPORTED");
    inline constexpr TCHAR ERR_METASOUND_FRONTEND_NOT_SUPPORTED[]           = TEXT("METASOUND_FRONTEND_NOT_SUPPORTED");
    inline constexpr TCHAR ERR_METASOUND_NOT_AVAILABLE[]                    = TEXT("METASOUND_NOT_AVAILABLE");
    // The asset is a renderable MetaSound Source but its graph could not be turned into a
    // running operator on this stack: CreateSoundGenerator returned null (unresolvable
    // output format), the built operator exposes no whole audio block, or - the reason this
    // is not DECODE_FAILED or CREATION_FAILED - the async operator builder could not be
    // switched off, so the render would begin with a scheduling-dependent number of silent
    // blocks and would not reproduce. All three mean "the graph exists but did not run",
    // which is a different remedy from a bad path (ASSET_NOT_FOUND / NOT_METASOUND) and
    // from a graph that ran and produced nothing (AUDIO_EMPTY_BUFFER).
    inline constexpr TCHAR ERR_METASOUND_RENDER_FAILED[]                    = TEXT("METASOUND_RENDER_FAILED");
    inline constexpr TCHAR ERR_METASOUND_SEARCH_NOT_AVAILABLE[]             = TEXT("METASOUND_SEARCH_NOT_AVAILABLE");
    inline constexpr TCHAR ERR_MGIR_DECOMPILE_FAILED[]                      = TEXT("MGIR_DECOMPILE_FAILED");
    inline constexpr TCHAR ERR_MISSING_ANIMATION_PATH[]                     = TEXT("MISSING_ANIMATION_PATH");
    inline constexpr TCHAR ERR_MISSING_ASSET_PATH[]                         = TEXT("MISSING_ASSET_PATH");
    inline constexpr TCHAR ERR_MISSING_BONES[]                              = TEXT("MISSING_BONES");
    inline constexpr TCHAR ERR_MISSING_BONE_NAME[]                          = TEXT("MISSING_BONE_NAME");
    inline constexpr TCHAR ERR_MISSING_CACHE_NAME[]                         = TEXT("MISSING_CACHE_NAME");
    inline constexpr TCHAR ERR_MISSING_CHAINS[]                             = TEXT("MISSING_CHAINS");
    inline constexpr TCHAR ERR_MISSING_CHAIN_NAME[]                         = TEXT("MISSING_CHAIN_NAME");
    inline constexpr TCHAR ERR_MISSING_CONTROL[]                            = TEXT("MISSING_CONTROL");
    inline constexpr TCHAR ERR_MISSING_CONTROLS[]                           = TEXT("MISSING_CONTROLS");
    inline constexpr TCHAR ERR_MISSING_CURVE_NAME[]                         = TEXT("MISSING_CURVE_NAME");
    inline constexpr TCHAR ERR_MISSING_FRAME[]                              = TEXT("MISSING_FRAME");
    inline constexpr TCHAR ERR_MISSING_MARKER_NAME[]                        = TEXT("MISSING_MARKER_NAME");
    inline constexpr TCHAR ERR_MISSING_NAME[]                               = TEXT("MISSING_NAME");
    inline constexpr TCHAR ERR_MISSING_NODE_TYPE[]                          = TEXT("MISSING_NODE_TYPE");
    inline constexpr TCHAR ERR_MISSING_PARAM[]                              = TEXT("MISSING_PARAM");
    inline constexpr TCHAR ERR_MISSING_PARAMETER[]                          = TEXT("MISSING_PARAMETER");
    inline constexpr TCHAR ERR_MISSING_PARAMETERS[]                         = TEXT("MISSING_PARAMETERS");
    inline constexpr TCHAR ERR_MISSING_PATH[]                               = TEXT("MISSING_PATH");
    inline constexpr TCHAR ERR_MISSING_REQUIRED_PARAM[]                     = TEXT("MISSING_REQUIRED_PARAM");
    inline constexpr TCHAR ERR_MISSING_ROW_STRUCT[]                         = TEXT("MISSING_ROW_STRUCT");
    inline constexpr TCHAR ERR_MISSING_SECTIONS[]                           = TEXT("MISSING_SECTIONS");
    inline constexpr TCHAR ERR_MISSING_SECTION_NAME[]                       = TEXT("MISSING_SECTION_NAME");
    inline constexpr TCHAR ERR_MISSING_SKELETON_PATH[]                      = TEXT("MISSING_SKELETON_PATH");
    inline constexpr TCHAR ERR_MISSING_SLOT_NAME[]                          = TEXT("MISSING_SLOT_NAME");
    inline constexpr TCHAR ERR_MISSING_STATES[]                             = TEXT("MISSING_STATES");
    inline constexpr TCHAR ERR_MISSING_STATE_MACHINE_NAME[]                 = TEXT("MISSING_STATE_MACHINE_NAME");
    inline constexpr TCHAR ERR_MISSING_STATE_NAME[]                         = TEXT("MISSING_STATE_NAME");
    inline constexpr TCHAR ERR_MISSING_VALUE[]                              = TEXT("MISSING_VALUE");
    inline constexpr TCHAR ERR_MIX_NOT_FOUND[]                              = TEXT("MIX_NOT_FOUND");
    // model.compile / model.validate: the document parsed, but a later stage failed - an op
    // the engine rejected, a degenerate hull, a reserved construct, a refused overwrite. Kept
    // apart from MODEL_PARSE_FAILED so a caller can tell a malformed document from a
    // well-formed one that cannot be built; the line-anchored PWMODEL_* diagnostics ride in
    // the result's diagnostics[] either way.
    inline constexpr TCHAR ERR_MODEL_COMPILE_FAILED[]                       = TEXT("MODEL_COMPILE_FAILED");
    // No .pwmodel file at the resolved filePath. Relative paths resolve against the project
    // directory, so the message reports both what was asked for and what it resolved to.
    inline constexpr TCHAR ERR_MODEL_FILE_NOT_FOUND[]                       = TEXT("MODEL_FILE_NOT_FOUND");
    // The source exists but is not usable text: a file that would not load as text, or an
    // empty `text` on model.validate.
    inline constexpr TCHAR ERR_MODEL_INVALID_SOURCE[]                       = TEXT("MODEL_INVALID_SOURCE");
    // The .pwmodel source did not parse. Nothing was created.
    inline constexpr TCHAR ERR_MODEL_PARSE_FAILED[]                         = TEXT("MODEL_PARSE_FAILED");
    inline constexpr TCHAR ERR_MODULE_ADD_FAILED[]                          = TEXT("MODULE_ADD_FAILED");
    inline constexpr TCHAR ERR_MODULE_INDEX_INVALID[]                       = TEXT("MODULE_INDEX_INVALID");
    // The module resolved, but it declares no stack input under the requested `inputName`. The
    // message lists the module's available stack-input names (specialized handlers may narrow that
    // list to the input kind they support), so the usable spelling is discoverable.
    inline constexpr TCHAR ERR_MODULE_INPUT_NOT_FOUND[]                     = TEXT("MODULE_INPUT_NOT_FOUND");
    inline constexpr TCHAR ERR_MODULE_INPUT_OVERRIDE_LINKED[]               = TEXT("MODULE_INPUT_OVERRIDE_LINKED");
    inline constexpr TCHAR ERR_MODULE_MOVE_FAILED[]                         = TEXT("MODULE_MOVE_FAILED");
    inline constexpr TCHAR ERR_MODULE_NOT_FOUND[]                           = TEXT("MODULE_NOT_FOUND");
    // An owner-qualified module entry key ("<ownerName>:<nodeGuid>") was paired with a different
    // `emitter`. A bare `entryId` is a node guid that duplicated emitters share, so the two
    // halves disagreeing means the caller is about to edit an emitter it did not mean to.
    inline constexpr TCHAR ERR_MODULE_OWNER_MISMATCH[]                      = TEXT("MODULE_OWNER_MISMATCH");
    inline constexpr TCHAR ERR_MODULE_REMOVE_FAILED[]                       = TEXT("MODULE_REMOVE_FAILED");
    inline constexpr TCHAR ERR_MODULE_SCRIPT_NOT_FOUND[]                    = TEXT("MODULE_SCRIPT_NOT_FOUND");
    inline constexpr TCHAR ERR_MONTAGE_NOT_FOUND[]                          = TEXT("MONTAGE_NOT_FOUND");
    inline constexpr TCHAR ERR_MORPH_NOT_FOUND[]                            = TEXT("MORPH_NOT_FOUND");
    inline constexpr TCHAR ERR_MORPH_NOT_PERSISTED[]                        = TEXT("MORPH_NOT_PERSISTED");
    inline constexpr TCHAR ERR_MORPH_TARGET_NOT_FOUND[]                     = TEXT("MORPH_TARGET_NOT_FOUND");
    inline constexpr TCHAR ERR_MOVIESCENE_UNAVAILABLE[]                     = TEXT("MOVIESCENE_UNAVAILABLE");
    inline constexpr TCHAR ERR_MRQ_JOB_ALLOCATION_FAILED[]                  = TEXT("MRQ_JOB_ALLOCATION_FAILED");
    inline constexpr TCHAR ERR_MRQ_EXECUTOR_FAILED[]                         = TEXT("MRQ_EXECUTOR_FAILED");
    inline constexpr TCHAR ERR_MRQ_NOT_AVAILABLE[]                          = TEXT("MRQ_NOT_AVAILABLE");
    // mrq.create_job / run_jobs / remove_job / clear_queue: the shared editor queue is already
    // owned by an active executor, so mutating it or starting another render would race the
    // executor's live queue state.
    inline constexpr TCHAR ERR_MRQ_RENDER_IN_PROGRESS[]                    = TEXT("MRQ_RENDER_IN_PROGRESS");
    // mrq.create_job: the caller named a presetPath that would not load. Refused rather than
    // queued unconfigured, because the render is minutes long and the deliverable leaves the
    // tool: a job that silently fell back to the engine's CDO defaults renders at Quality/CRF 20
    // into a directory the caller did not choose, and the old response echoed presetPath either
    // way (B-mrq-render-result-omits-bitrate-and-size).
    inline constexpr TCHAR ERR_MRQ_PRESET_NOT_LOADABLE[]                    = TEXT("MRQ_PRESET_NOT_LOADABLE");
    inline constexpr TCHAR ERR_MRQ_QUEUE_NULL[]                             = TEXT("MRQ_QUEUE_NULL");
    inline constexpr TCHAR ERR_MRQ_SUBSYSTEM_UNAVAILABLE[]                  = TEXT("MRQ_SUBSYSTEM_UNAVAILABLE");
    inline constexpr TCHAR ERR_MSIR_DECOMPILE_FAILED[]                      = TEXT("MSIR_DECOMPILE_FAILED");
    inline constexpr TCHAR ERR_MUTE_FAILED[]                                = TEXT("MUTE_FAILED");
    // niagara.add_emitter / remove_emitter: the system's compiled data-interface count differs from
    // its resolved count. The next tick asserts inside the VectorVM (DataSetIdx < DataSets.Num())
    // on a concurrent worker - an appError that kills the editor - so the write path refuses
    // rather than saving. Root cause is compile:true + save:true on one call: the compile is
    // async, so the save persists an invalidated compile.
    //
    // niagara.validate reuses this spelling as a validation ISSUE code (an entry in its errors
    // array, not a wire error) for a system already in that state, so one fault has one name
    // whichever verb finds it. The remedy is niagara.list_orphan_data_interfaces /
    // niagara.remove_orphan_data_interfaces, or a recompile.
    //
    // sequencer.set_playhead (and the capture-subject providers that drive the same playhead
    // utils) refuse with it too, before opening or evaluating the sequence: moving the playhead
    // forces a world tick, which ticks every Niagara simulation in the level, not just the ones the
    // sequence binds. The message names each system, its live component count and the offending
    // scripts; niagara.audit_level reports the same set with owning actors.
    inline constexpr TCHAR ERR_NIAGARA_DATA_INTERFACE_MISMATCH[]            = TEXT("NIAGARA_DATA_INTERFACE_MISMATCH");
    // The counterpart of NIAGARA_DATA_INTERFACE_MISMATCH: nothing could be compared, because the
    // system has no resolved data-interface set yet (nothing has compiled it this session) or this
    // engine build does not expose one. NOT a pass and NOT a mismatch - an empty result under this
    // code is not evidence the system is sound. niagara.validate files it as a validation warning;
    // niagara.audit_level files it as the audit contract's UNRUNNABLE row, which makes pass false
    // whatever failOn says (Audit/AuditFramework.h). The remedy is niagara.compile, then re-ask.
    inline constexpr TCHAR ERR_NIAGARA_DATA_INTERFACE_UNVERIFIED[]          = TEXT("NIAGARA_DATA_INTERFACE_UNVERIFIED");
    // niagara.add_emitter / remove_emitter: the system graph's emitter-node rebuild could not
    // place the nodes, so one or more emitter handles would never be invoked - the handle exists,
    // compiles clean and validates clean, and spawns nothing. Refused before compile and before
    // save; the payload carries uninvokedEmitters and systemGraphReadable.
    inline constexpr TCHAR ERR_NIAGARA_EMITTER_NOT_IN_SYSTEM_GRAPH[]        = TEXT("NIAGARA_EMITTER_NOT_IN_SYSTEM_GRAPH");
    // niagara.remove_orphan_data_interfaces: dropping the orphan resolved data interfaces would
    // NOT bring a script's resolved count back to its compiled count, so the removal would leave
    // (or create) the very mismatch it exists to repair. Nothing is mutated; the payload carries
    // the offending scripts and both counts. Recompile the system instead.
    inline constexpr TCHAR ERR_NIAGARA_ORPHAN_REMOVAL_UNSAFE[]              = TEXT("NIAGARA_ORPHAN_REMOVAL_UNSAFE");
    // niagara.validate ISSUE codes (entries in its issues/errors/warnings arrays, not wire
    // errors), for a sprite/mesh renderer whose SubImageSize declares a frame grid the texture
    // it samples does not have. The sprite vertex factory remaps every UV into a
    // 1/X x 1/Y window unconditionally, so a wrong grid samples across cell boundaries and
    // renders clipped fragments and inter-cell gutter - with no compile error and nothing in
    // any other readback joining the declared grid to the assigned texture.
    //
    // _MISMATCH is an error when the disagreement was measured from pixels (the declared grid
    // has wholly-empty trailing columns or rows) and a warning when only a heuristic disagrees
    // (an NxM spelled in the texture's name, or dimensions that do not divide by the grid); the
    // message names which. _INDETERMINATE is a warning for a renderer whose texture could not be
    // resolved or read - never silence, which would read as "checked and fine".
    inline constexpr TCHAR ERR_NIAGARA_SUBUV_ATLAS_SIZE_INDETERMINATE[]     = TEXT("NIAGARA_SUBUV_ATLAS_SIZE_INDETERMINATE");
    inline constexpr TCHAR ERR_NIAGARA_SUBUV_ATLAS_SIZE_MISMATCH[]          = TEXT("NIAGARA_SUBUV_ATLAS_SIZE_MISMATCH");
    inline constexpr TCHAR ERR_NIR_DECOMPILE_FAILED[]                       = TEXT("NIR_DECOMPILE_FAILED");
    inline constexpr TCHAR ERR_NODES_NOT_FOUND[]                            = TEXT("NODES_NOT_FOUND");
    inline constexpr TCHAR ERR_NODE_CLASS_NOT_FOUND[]                       = TEXT("NODE_CLASS_NOT_FOUND");
    inline constexpr TCHAR ERR_NODE_CREATE_FAILED[]                         = TEXT("NODE_CREATE_FAILED");
    inline constexpr TCHAR ERR_NODE_CREATION_FAILED[]                       = TEXT("NODE_CREATION_FAILED");
    inline constexpr TCHAR ERR_NODE_NOT_ASSET_PLAYER[]                      = TEXT("NODE_NOT_ASSET_PLAYER");
    inline constexpr TCHAR ERR_NODE_NOT_FOUND[]                             = TEXT("NODE_NOT_FOUND");
    inline constexpr TCHAR ERR_NODE_TYPE_NOT_FOUND[]                        = TEXT("NODE_TYPE_NOT_FOUND");
    // image.annotate was given no grid, no crosshairs, no boxes and no labels. An empty mark set
    // is an error rather than a zero-item success (rpc-design.md §3): otherwise a typo in the
    // overlay key produces a byte-identical copy of the input reported as an annotation.
    inline constexpr TCHAR ERR_NOTHING_TO_ANNOTATE[]                        = TEXT("NOTHING_TO_ANNOTATE");
    inline constexpr TCHAR ERR_NOTHING_TO_UNDO[]                            = TEXT("NOTHING_TO_UNDO");
    inline constexpr TCHAR ERR_NOTIFY_STATE_NOT_FOUND[]                     = TEXT("NOTIFY_STATE_NOT_FOUND");
    inline constexpr TCHAR ERR_NOT_AN_ARRAY[]                               = TEXT("NOT_AN_ARRAY");
    inline constexpr TCHAR ERR_NOT_AN_EFFECT_CALCULATION[]                  = TEXT("NOT_AN_EFFECT_CALCULATION");
    inline constexpr TCHAR ERR_NOT_AVAILABLE[]                              = TEXT("NOT_AVAILABLE");
    inline constexpr TCHAR ERR_NOT_A_GAMEPLAY_ABILITY[]                     = TEXT("NOT_A_GAMEPLAY_ABILITY");
    inline constexpr TCHAR ERR_NOT_A_GAMEPLAY_EFFECT[]                      = TEXT("NOT_A_GAMEPLAY_EFFECT");
    inline constexpr TCHAR ERR_NOT_A_MAP[]                                  = TEXT("NOT_A_MAP");
    inline constexpr TCHAR ERR_NOT_A_PAWN[]                                 = TEXT("NOT_A_PAWN");
    inline constexpr TCHAR ERR_NOT_A_SET[]                                  = TEXT("NOT_A_SET");
    inline constexpr TCHAR ERR_NOT_A_SPLINE[]                               = TEXT("NOT_A_SPLINE");
    inline constexpr TCHAR ERR_NOT_A_UTILITY_BLUEPRINT[]                    = TEXT("NOT_A_UTILITY_BLUEPRINT");
    inline constexpr TCHAR ERR_NOT_A_UTILITY_WIDGET_BP[]                    = TEXT("NOT_A_UTILITY_WIDGET_BP");
    inline constexpr TCHAR ERR_NOT_A_WHEEL_ASSET[]                          = TEXT("NOT_A_WHEEL_ASSET");
    inline constexpr TCHAR ERR_NOT_DYNAMIC_INPUT_SCRIPT[]                   = TEXT("NOT_DYNAMIC_INPUT_SCRIPT");
    inline constexpr TCHAR ERR_NOT_FOUND[]                                  = TEXT("NOT_FOUND");
    inline constexpr TCHAR ERR_NOT_IMPLEMENTED[]                            = TEXT("NOT_IMPLEMENTED");
    inline constexpr TCHAR ERR_NOT_IN_PIE[]                                 = TEXT("NOT_IN_PIE");
    inline constexpr TCHAR ERR_NOT_METASOUND[]                              = TEXT("NOT_METASOUND");
    inline constexpr TCHAR ERR_NOT_NIAGARA_ASSET[]                          = TEXT("NOT_NIAGARA_ASSET");
    inline constexpr TCHAR ERR_NOT_PARTITIONED[]                            = TEXT("NOT_PARTITIONED");
    inline constexpr TCHAR ERR_NOT_PERIODIC[]                               = TEXT("NOT_PERIODIC");
    inline constexpr TCHAR ERR_NOT_PLAYING[]                                = TEXT("NOT_PLAYING");
    inline constexpr TCHAR ERR_NOT_SUPPORTED[]                              = TEXT("NOT_SUPPORTED");
    inline constexpr TCHAR ERR_NO_TESTS_MATCHED[]                           = TEXT("NO_TESTS_MATCHED");
    inline constexpr TCHAR ERR_NO_ACTIVE_GAME_WORLD[]                       = TEXT("NO_ACTIVE_GAME_WORLD");
    inline constexpr TCHAR ERR_NO_ACTIVE_LEVEL_VIEWPORT[]                   = TEXT("NO_ACTIVE_LEVEL_VIEWPORT");
    inline constexpr TCHAR ERR_NO_ACTIVE_SESSION[]                          = TEXT("NO_ACTIVE_SESSION");
    inline constexpr TCHAR ERR_NO_ACTIVE_WIDGET[]                           = TEXT("NO_ACTIVE_WIDGET");
    // A batch verb's actor selector (prefix / filter / names / selection) matched nothing. An
    // empty batch is an error, not a zero-item success: "0 of 0 placed" is how a typo in a
    // prefix passes for a clean run.
    inline constexpr TCHAR ERR_NO_ACTORS_MATCHED[]                          = TEXT("NO_ACTORS_MATCHED");
    // ai.get_runtime_state: the named actor exists in the PIE world but no AController drives it
    // - an unpossessed Pawn, or a plain actor. Distinct from NO_BRAIN_COMPONENT, which is about a
    // controller that IS there and runs no AI logic; the remedies differ (possess vs. assign a
    // brain), so collapsing them would send the caller after the wrong one.
    inline constexpr TCHAR ERR_NO_AI_CONTROLLER[]                           = TEXT("NO_AI_CONTROLLER");
    // The asset-side twin of NO_ACTORS_MATCHED: a batch verb's ASSET selector (a content folder
    // plus a class filter) enumerated nothing through the asset registry. Same argument, same
    // failure shape - a folder that matched nothing and a folder full of clean assets must not
    // produce the same-shaped answer, or a typo in a path passes for a clean sweep. Distinct from
    // ASSET_NOT_FOUND, which is about ONE named asset that is missing: here the caller named a
    // set, and the remedy is to fix the selector (folder, recursion flag) rather than one path.
    inline constexpr TCHAR ERR_NO_ASSETS_MATCHED[]                          = TEXT("NO_ASSETS_MATCHED");
    inline constexpr TCHAR ERR_NO_BLUEPRINT[]                               = TEXT("NO_BLUEPRINT");
    inline constexpr TCHAR ERR_NO_BLUEPRINT_EDITOR[]                        = TEXT("NO_BLUEPRINT_EDITOR");
    // ai.get_runtime_state: the controller carries neither a UBrainComponent nor a
    // UPathFollowingComponent, so there is no running AI to report on. Refusing beats answering
    // with three empty sections, which reads as "the AI is idle" rather than "there is no AI".
    inline constexpr TCHAR ERR_NO_BRAIN_COMPONENT[]                         = TEXT("NO_BRAIN_COMPONENT");
    // A candidate id was resolved against a generated-candidate registry that holds nothing
    // at all. Zero is not a small number (rpc-design.md §7): answering CANDIDATE_NOT_FOUND
    // here sends the caller off checking an id when the real state is that the session has
    // rendered nothing yet, so the remedy is to generate first.
    inline constexpr TCHAR ERR_NO_CANDIDATES[]                              = TEXT("NO_CANDIDATES");
    inline constexpr TCHAR ERR_NO_COMPONENT[]                               = TEXT("NO_COMPONENT");
    inline constexpr TCHAR ERR_NO_CONTROLS_KEYED[]                          = TEXT("NO_CONTROLS_KEYED");
    inline constexpr TCHAR ERR_NO_EDITOR[]                                  = TEXT("NO_EDITOR");
    inline constexpr TCHAR ERR_NO_EDITOR_WORLD[]                            = TEXT("NO_EDITOR_WORLD");
    inline constexpr TCHAR ERR_NO_GAME_INSTANCE[]                           = TEXT("NO_GAME_INSTANCE");
    inline constexpr TCHAR ERR_NO_INVALID_DATALAYERS[]                       = TEXT("NO_INVALID_DATALAYERS");
    // The actor carries no UInstancedStaticMeshComponent at all, so there are no instances to
    // read, write or seat. Distinct from COMPONENT_NOT_FOUND, which means a component WAS named
    // and did not resolve: the two send a caller to different fixes.
    inline constexpr TCHAR ERR_NO_INSTANCED_COMPONENT[]                     = TEXT("NO_INSTANCED_COMPONENT");
    // A bake/export ran, the engine reported success, and the measured result holds zero keys.
    // Emitted by sequencer.bake_to_controlrig and sequencer.export_anim_sequence instead of a
    // success carrying zeroes, so a bake that produced nothing can never read as one that worked.
    inline constexpr TCHAR ERR_NO_KEYS_WRITTEN[]                            = TEXT("NO_KEYS_WRITTEN");
    // sequencer.bake_control_space: the engine accepted the call, but the target control gained
    // zero transform/space keys. Existing keys are not evidence that this request baked anything.
    inline constexpr TCHAR ERR_NOTHING_BAKED[]                              = TEXT("NOTHING_BAKED");
    // sequencer.import_fbx: the engine returned success, but the MovieScene's measured track,
    // section, key, and signed-object state did not change. This names an accepted FBX that
    // matched/imported nothing, rather than conflating it with a parser/importer failure.
    inline constexpr TCHAR ERR_NOTHING_IMPORTED[]                           = TEXT("NOTHING_IMPORTED");
    inline constexpr TCHAR ERR_NO_LEVEL[]                                   = TEXT("NO_LEVEL");
    inline constexpr TCHAR ERR_NO_LOD_MODELS[]                              = TEXT("NO_LOD_MODELS");
    inline constexpr TCHAR ERR_NO_NAVMESH[]                                 = TEXT("NO_NAVMESH");
    inline constexpr TCHAR ERR_NO_NAV_SYS[]                                 = TEXT("NO_NAV_SYS");
    // pcg.set_node_property: the resolved UPCGNode carries no UPCGSettings object, so there is
    // nothing to write a reflected property on.
    inline constexpr TCHAR ERR_NO_NODE_SETTINGS[]                           = TEXT("NO_NODE_SETTINGS");
    inline constexpr TCHAR ERR_NO_PCG_GRAPH[]                               = TEXT("NO_PCG_GRAPH");
    inline constexpr TCHAR ERR_NO_PLAYER_CONTROLLER[]                       = TEXT("NO_PLAYER_CONTROLLER");
    inline constexpr TCHAR ERR_NO_SCS[]                                     = TEXT("NO_SCS");
    inline constexpr TCHAR ERR_NO_SKEL_MESH_COMP[]                          = TEXT("NO_SKEL_MESH_COMP");
    // "Nothing was ever bound." Distinct from SKIN_WEIGHTS_INCOMPLETE, which means a
    // binding exists but predates the geometry - different cause, different fix.
    inline constexpr TCHAR ERR_NO_SKIN_WEIGHTS[]                            = TEXT("NO_SKIN_WEIGHTS");
    inline constexpr TCHAR ERR_NO_SMART_LINK[]                              = TEXT("NO_SMART_LINK");
    inline constexpr TCHAR ERR_NO_SOURCE_WEIGHTS[]                          = TEXT("NO_SOURCE_WEIGHTS");
    inline constexpr TCHAR ERR_NO_SPLINE[]                                  = TEXT("NO_SPLINE");
    inline constexpr TCHAR ERR_NO_TIMING_DATA[]                             = TEXT("NO_TIMING_DATA");
    inline constexpr TCHAR ERR_NO_TRANSFORM_SECTION[]                       = TEXT("NO_TRANSFORM_SECTION");
    inline constexpr TCHAR ERR_NO_TRANSFORM_TRACK[]                         = TEXT("NO_TRANSFORM_TRACK");
    inline constexpr TCHAR ERR_NO_UV_ELEMENTS[]                             = TEXT("NO_UV_ELEMENTS");
    inline constexpr TCHAR ERR_NO_VALID_ASSETS[]                            = TEXT("NO_VALID_ASSETS");
    inline constexpr TCHAR ERR_NO_VIEWPORT[]                                = TEXT("NO_VIEWPORT");
    inline constexpr TCHAR ERR_NO_WATER_BODY_COMPONENT[]                    = TEXT("NO_WATER_BODY_COMPONENT");
    inline constexpr TCHAR ERR_NO_WORLD[]                                   = TEXT("NO_WORLD");
    inline constexpr TCHAR ERR_NO_WORLD_SETTINGS[]                          = TEXT("NO_WORLD_SETTINGS");
    inline constexpr TCHAR ERR_NULL_COMPONENT[]                             = TEXT("NULL_COMPONENT");
    inline constexpr TCHAR ERR_NULL_EXPRESSION[]                            = TEXT("NULL_EXPRESSION");
    inline constexpr TCHAR ERR_OBJECT_NOT_FOUND[]                           = TEXT("OBJECT_NOT_FOUND");
    inline constexpr TCHAR ERR_OPEN_FAILED[]                                = TEXT("OPEN_FAILED");
    inline constexpr TCHAR ERR_OPERATION_FAILED[]                           = TEXT("OPERATION_FAILED");
    inline constexpr TCHAR ERR_OPERATION_NOT_SUPPORTED[]                    = TEXT("OPERATION_NOT_SUPPORTED");
    inline constexpr TCHAR ERR_OPERATION_SKIPPED[]                          = TEXT("OPERATION_SKIPPED");
    inline constexpr TCHAR ERR_OUTPUT_FAILED[]                              = TEXT("OUTPUT_FAILED");
    inline constexpr TCHAR ERR_OUTPUT_NODE_NOT_FOUND[]                      = TEXT("OUTPUT_NODE_NOT_FOUND");
    inline constexpr TCHAR ERR_OUTPUT_NOT_FOUND[]                           = TEXT("OUTPUT_NOT_FOUND");
    inline constexpr TCHAR ERR_OUT_OF_BOUNDS[]                              = TEXT("OUT_OF_BOUNDS");
    inline constexpr TCHAR ERR_OVERRIDE_CLEAR_FAILED[]                      = TEXT("OVERRIDE_CLEAR_FAILED");
    inline constexpr TCHAR ERR_OVERWRITE_UNSAFE[]                           = TEXT("OVERWRITE_UNSAFE");
    inline constexpr TCHAR ERR_PACKAGE_CREATE_FAILED[]                      = TEXT("PACKAGE_CREATE_FAILED");
    inline constexpr TCHAR ERR_PACKAGE_CREATION_FAILED[]                    = TEXT("PACKAGE_CREATION_FAILED");
    inline constexpr TCHAR ERR_PACKAGE_ERROR[]                              = TEXT("PACKAGE_ERROR");
    inline constexpr TCHAR ERR_PACKAGE_FAILED[]                             = TEXT("PACKAGE_FAILED");
    inline constexpr TCHAR ERR_PACKAGE_NOT_FOUND[]                          = TEXT("PACKAGE_NOT_FOUND");
    inline constexpr TCHAR ERR_PANEL_NOT_REALIZED[]                         = TEXT("PANEL_NOT_REALIZED");
    inline constexpr TCHAR ERR_PARAMETER_EXISTS[]                           = TEXT("PARAMETER_EXISTS");
    inline constexpr TCHAR ERR_PARAMETER_NOT_FOUND[]                        = TEXT("PARAMETER_NOT_FOUND");
    inline constexpr TCHAR ERR_PARAMETER_REMOVE_FAILED[]                    = TEXT("PARAMETER_REMOVE_FAILED");
    inline constexpr TCHAR ERR_PARAMETER_SET_FAILED[]                       = TEXT("PARAMETER_SET_FAILED");
    inline constexpr TCHAR ERR_PARAMETER_STORE_NOT_FOUND[]                  = TEXT("PARAMETER_STORE_NOT_FOUND");
    inline constexpr TCHAR ERR_PARAMETER_TYPE_MISMATCH[]                    = TEXT("PARAMETER_TYPE_MISMATCH");
    inline constexpr TCHAR ERR_PARAM_FAILED[]                               = TEXT("PARAM_FAILED");
    // The WIRE axis of "wrong type", emitted only by FRpcDispatcher::ValidateHandlerParams: the
    // JSON shape of a declared parameter does not match its FParamSpec::Type, or it arrived as
    // JSON null. Deliberately NOT one of INVALID_PARAM_TYPE / INVALID_PARAMETER_TYPE /
    // PARAMETER_TYPE_MISMATCH above - every emitter of those three is a DOMAIN type check
    // (Niagara parameter types, material parameter types, PCG pin types) reached from inside a
    // handler body, and a caller must be able to tell "you sent an array where the verb declares
    // a string" from "this Niagara input cannot hold a float".
    inline constexpr TCHAR ERR_PARAM_TYPE_MISMATCH[]                        = TEXT("PARAM_TYPE_MISMATCH");
    inline constexpr TCHAR ERR_PARENT_CLASS_NOT_FOUND[]                     = TEXT("PARENT_CLASS_NOT_FOUND");
    inline constexpr TCHAR ERR_PARENT_CYCLE[]                               = TEXT("PARENT_CYCLE");
    inline constexpr TCHAR ERR_PARENT_NOT_FOUND[]                           = TEXT("PARENT_NOT_FOUND");
    inline constexpr TCHAR ERR_PARENT_REQUIRED[]                            = TEXT("PARENT_REQUIRED");
    inline constexpr TCHAR ERR_PARENT_SUBMIX_NOT_FOUND[]                    = TEXT("PARENT_SUBMIX_NOT_FOUND");
    inline constexpr TCHAR ERR_PARSE_FAILED[]                               = TEXT("PARSE_FAILED");
    // spatial.ground_actors / spatial.verify_grounding: too little of the actor's footprint has
    // ground under it - it overhangs a hole or the terrain edge. This is the code that catches
    // the prop placed beyond the map boundary, which a single centre-point probe cannot see.
    inline constexpr TCHAR ERR_PARTIAL_GROUND_COVERAGE[]                    = TEXT("PARTIAL_GROUND_COVERAGE");
    inline constexpr TCHAR ERR_PCGIR_DECOMPILE_FAILED[]                     = TEXT("PCGIR_DECOMPILE_FAILED");
    inline constexpr TCHAR ERR_PCG_COMPONENT_GONE[]                         = TEXT("PCG_COMPONENT_GONE");
    inline constexpr TCHAR ERR_PCG_GENERATION_CANCELLED[]                   = TEXT("PCG_GENERATION_CANCELLED");
    inline constexpr TCHAR ERR_PCG_GENERATION_ENDED_WITHOUT_OUTPUT[]        = TEXT("PCG_GENERATION_ENDED_WITHOUT_OUTPUT");
    inline constexpr TCHAR ERR_PCG_GENERATION_NOT_SCHEDULED[]               = TEXT("PCG_GENERATION_NOT_SCHEDULED");
    inline constexpr TCHAR ERR_PCG_GENERATION_TIMEOUT[]                     = TEXT("PCG_GENERATION_TIMEOUT");
    inline constexpr TCHAR ERR_PERCEPTION_COMPONENT_NOT_FOUND[]             = TEXT("PERCEPTION_COMPONENT_NOT_FOUND");
    inline constexpr TCHAR ERR_PHYSICS_ASSET_NOT_FOUND[]                    = TEXT("PHYSICS_ASSET_NOT_FOUND");
    inline constexpr TCHAR ERR_PHYSICS_FAILED[]                             = TEXT("PHYSICS_FAILED");
    inline constexpr TCHAR ERR_PIE_ACTIVE[]                                 = TEXT("PIE_ACTIVE");
    inline constexpr TCHAR ERR_PIE_NOT_ACTIVE[]                             = TEXT("PIE_NOT_ACTIVE");
    inline constexpr TCHAR ERR_PIE_START_CANCELLED[]                        = TEXT("PIE_START_CANCELLED");
    inline constexpr TCHAR ERR_PIE_START_FAILED[]                           = TEXT("PIE_START_FAILED");
    inline constexpr TCHAR ERR_PIE_START_IN_PROGRESS[]                      = TEXT("PIE_START_IN_PROGRESS");
    inline constexpr TCHAR ERR_PIE_STOP_FAILED[]                            = TEXT("PIE_STOP_FAILED");
    inline constexpr TCHAR ERR_PIE_STOP_IN_PROGRESS[]                       = TEXT("PIE_STOP_IN_PROGRESS");
    inline constexpr TCHAR ERR_PIN_CREATION_FAILED[]                        = TEXT("PIN_CREATION_FAILED");
    inline constexpr TCHAR ERR_PIN_NOT_FOUND[]                              = TEXT("PIN_NOT_FOUND");
    inline constexpr TCHAR ERR_PIN_REMAP_INVALID[]                          = TEXT("PIN_REMAP_INVALID");
    inline constexpr TCHAR ERR_PIXEL_STATS_UNAVAILABLE[]                    = TEXT("PIXEL_STATS_UNAVAILABLE");
    inline constexpr TCHAR ERR_PLAYER_NOT_FOUND[]                           = TEXT("PLAYER_NOT_FOUND");
    inline constexpr TCHAR ERR_PLAY_FAILED[]                                = TEXT("PLAY_FAILED");
    inline constexpr TCHAR ERR_PLUGIN_DISABLED[]                            = TEXT("PLUGIN_DISABLED");
    inline constexpr TCHAR ERR_PLUGIN_NOT_FOUND[]                           = TEXT("PLUGIN_NOT_FOUND");
    inline constexpr TCHAR ERR_POLYGON_LIMIT_EXCEEDED[]                     = TEXT("POLYGON_LIMIT_EXCEEDED");
    inline constexpr TCHAR ERR_POSSESS_FAILED[]                             = TEXT("POSSESS_FAILED");
    // audio.authoring.create_metasound_preset: the asset was created but its document does not
    // carry a preset template pointing at the requested parent, so it is a blank MetaSound
    // rather than a preset. Distinct from CREATE_FAILED (nothing was made at all) because the
    // object exists and looks fine — only the parent link is missing.
    inline constexpr TCHAR ERR_PRESET_NOT_APPLIED[]                         = TEXT("PRESET_NOT_APPLIED");
    inline constexpr TCHAR ERR_PRESET_NOT_FOUND[]                           = TEXT("PRESET_NOT_FOUND");
    inline constexpr TCHAR ERR_PREVIEW_NOT_FOUND[]                          = TEXT("PREVIEW_NOT_FOUND");
    inline constexpr TCHAR ERR_PREVIEW_VIEWPORT_NOT_FOUND[]                 = TEXT("PREVIEW_VIEWPORT_NOT_FOUND");
    inline constexpr TCHAR ERR_PROBE_CREATE_FAILED[]                        = TEXT("PROBE_CREATE_FAILED");
    inline constexpr TCHAR ERR_PROFILE_NOT_FOUND[]                          = TEXT("PROFILE_NOT_FOUND");
    inline constexpr TCHAR ERR_PROPERTY_CONVERSION_FAILED[]                 = TEXT("PROPERTY_CONVERSION_FAILED");
    inline constexpr TCHAR ERR_PROPERTY_EXPORT_FAILED[]                     = TEXT("PROPERTY_EXPORT_FAILED");
    inline constexpr TCHAR ERR_PROPERTY_NOT_FOUND[]                         = TEXT("PROPERTY_NOT_FOUND");
    inline constexpr TCHAR ERR_PROPERTY_NOT_INT[]                           = TEXT("PROPERTY_NOT_INT");
    inline constexpr TCHAR ERR_PROPERTY_NOT_SUPPORTED[]                     = TEXT("PROPERTY_NOT_SUPPORTED");
    inline constexpr TCHAR ERR_PROPERTY_PATH_FAILED[]                       = TEXT("PROPERTY_PATH_FAILED");
    inline constexpr TCHAR ERR_PROPERTY_SET_FAILED[]                        = TEXT("PROPERTY_SET_FAILED");
    inline constexpr TCHAR ERR_PROPERTY_WRONG_TYPE[]                        = TEXT("PROPERTY_WRONG_TYPE");
    // python.callbacks: the interpreter is up but the tracking shim
    // (Content/Python/pinwright_callbacks.py) would not install, so nothing can be listed
    // or cleared honestly. Distinct from PYTHON_INIT_FAILED, which is the interpreter.
    inline constexpr TCHAR ERR_PYTHON_CALLBACK_TRACKING_UNAVAILABLE[]       = TEXT("PYTHON_CALLBACK_TRACKING_UNAVAILABLE");
    inline constexpr TCHAR ERR_PYTHON_INIT_FAILED[]                         = TEXT("PYTHON_INIT_FAILED");
    inline constexpr TCHAR ERR_PYTHON_NOT_AVAILABLE[]                       = TEXT("PYTHON_NOT_AVAILABLE");
    inline constexpr TCHAR ERR_QUERY_FAILED[]                               = TEXT("QUERY_FAILED");
    inline constexpr TCHAR ERR_QUEUE_EMPTY[]                                = TEXT("QUEUE_EMPTY");
    inline constexpr TCHAR ERR_READ_PIXELS_FAILED[]                         = TEXT("READ_PIXELS_FAILED");
    inline constexpr TCHAR ERR_RECURSIVE_SUBGRAPH[]                         = TEXT("RECURSIVE_SUBGRAPH");
    inline constexpr TCHAR ERR_RELOAD_FAILED[]                              = TEXT("RELOAD_FAILED");
    inline constexpr TCHAR ERR_REMOVE_FAILED[]                              = TEXT("REMOVE_FAILED");
    inline constexpr TCHAR ERR_RENAME_FAILED[]                              = TEXT("RENAME_FAILED");
    inline constexpr TCHAR ERR_RENDERER_CLASS_NOT_FOUND[]                   = TEXT("RENDERER_CLASS_NOT_FOUND");
    inline constexpr TCHAR ERR_RENDERER_CREATE_FAILED[]                     = TEXT("RENDERER_CREATE_FAILED");
    inline constexpr TCHAR ERR_RENDERER_INDEX_INVALID[]                     = TEXT("RENDERER_INDEX_INVALID");
    inline constexpr TCHAR ERR_RENDERER_NOT_FOUND[]                         = TEXT("RENDERER_NOT_FOUND");
    inline constexpr TCHAR ERR_RENDER_NO_OUTPUT[]                           = TEXT("RENDER_NO_OUTPUT");
    inline constexpr TCHAR ERR_RENDER_TARGET_CREATE_FAILED[]                = TEXT("RENDER_TARGET_CREATE_FAILED");
    inline constexpr TCHAR ERR_RENDER_UNRENDERABLE_FRAMES[]                 = TEXT("RENDER_UNRENDERABLE_FRAMES");
    inline constexpr TCHAR ERR_REPLAY_RECORDING_FAILED[]                    = TEXT("REPLAY_RECORDING_FAILED");
    inline constexpr TCHAR ERR_REPLACE_FAILED[]                             = TEXT("REPLACE_FAILED");
    inline constexpr TCHAR ERR_REPLACE_REFUSED[]                            = TEXT("REPLACE_REFUSED");
    inline constexpr TCHAR ERR_RESOLUTION_FAILED[]                          = TEXT("RESOLUTION_FAILED");
    inline constexpr TCHAR ERR_REVERB_NOT_AVAILABLE[]                       = TEXT("REVERB_NOT_AVAILABLE");
    inline constexpr TCHAR ERR_REVERT_REQUIRES_UNLOADED_PACKAGE[]           = TEXT("REVERT_REQUIRES_UNLOADED_PACKAGE");
    inline constexpr TCHAR ERR_RIG_CLASS_NOT_FOUND[]                        = TEXT("RIG_CLASS_NOT_FOUND");
    inline constexpr TCHAR ERR_RIG_STATE_INVALID[]                          = TEXT("RIG_STATE_INVALID");
    inline constexpr TCHAR ERR_ROOT_MISSING[]                               = TEXT("ROOT_MISSING");
    inline constexpr TCHAR ERR_ROWS_PRESENT_FORCE_REQUIRED[]                = TEXT("ROWS_PRESENT_FORCE_REQUIRED");
    inline constexpr TCHAR ERR_ROW_EXISTS[]                                 = TEXT("ROW_EXISTS");
    inline constexpr TCHAR ERR_ROW_NOT_FOUND[]                              = TEXT("ROW_NOT_FOUND");
    inline constexpr TCHAR ERR_RUNTIME_ONLY_COMMAND[]                       = TEXT("RUNTIME_ONLY_COMMAND");
    inline constexpr TCHAR ERR_SAMPLE_REJECTED[]                            = TEXT("SAMPLE_REJECTED");
    // asset.save: the .uasset on disk changed since the package was loaded or last saved, so
    // writing the resident in-memory state would discard an out-of-band change (a git checkout,
    // an external revert, another tool's or another editor's write). Refused before any byte is
    // written; the payload's diskState block carries the measured on-disk saved hash and the
    // resident package's own. Overridable with overwriteDiskChanges:true — deliberately NOT by
    // force, which only bypasses the save throttle. AssetSaveHandler.cpp spells the literal by
    // hand on purpose: that file does not cite ErrorCodes::ERR_ and a first citation would flip
    // it to "adopting", failing TestErrorCodeRegistry on its hand-spelled ASSET_NOT_FOUND.
    inline constexpr TCHAR ERR_SAVE_DISK_STATE_DIVERGED[]                   = TEXT("SAVE_DISK_STATE_DIVERGED");
    inline constexpr TCHAR ERR_SAVE_FAILED[]                                = TEXT("SAVE_FAILED");
    inline constexpr TCHAR ERR_SAVE_VERIFICATION_FAILED[]                   = TEXT("SAVE_VERIFICATION_FAILED");
    // system.console_command / editor.console_command: the line sets an sg.* scalability group,
    // which pins it at ECVF_SetByConsole above the editor's own Scalability panel for the rest of
    // the session. Steers to performance.set_scalability, which writes at the panel's own
    // priority; `force: true` on either verb runs the line anyway. Both call sites spell the
    // literal by hand on purpose: neither file cites ErrorCodes::ERR_, and the first citation
    // would flip the whole file to "adopting" and fail its existing raw literals.
    inline constexpr TCHAR ERR_SCALABILITY_CVAR_USE_TYPED_VERB[]            = TEXT("SCALABILITY_CVAR_USE_TYPED_VERB");
    // render.capture_ortho_tiles: no camera depth was supplied and the world carries no visible
    // registered primitive with bounds, so there is nothing to place the orthographic camera
    // behind. The scene-capture ortho projection hardcodes its near plane to 0
    // (SceneCaptureRendering.cpp BuildOrthoMatrix), so a camera placed on a guess clips whatever
    // sits behind it - reported as unmeasured rather than defaulted to the origin.
    inline constexpr TCHAR ERR_SCENE_BOUNDS_NOT_MEASURED[]                  = TEXT("SCENE_BOUNDS_NOT_MEASURED");
    inline constexpr TCHAR ERR_SCENE_CAPTURE_FAILED[]                       = TEXT("SCENE_CAPTURE_FAILED");
    inline constexpr TCHAR ERR_SCHEMA_FINALIZE_FAILED[]                     = TEXT("SCHEMA_FINALIZE_FAILED");
    inline constexpr TCHAR ERR_SCHEMA_NOT_FOUND[]                           = TEXT("SCHEMA_NOT_FOUND");
    inline constexpr TCHAR ERR_SCHEMA_NOT_SET[]                             = TEXT("SCHEMA_NOT_SET");
    inline constexpr TCHAR ERR_SCIR_DECOMPILE_FAILED[]                      = TEXT("SCIR_DECOMPILE_FAILED");
    inline constexpr TCHAR ERR_SCS_COMPONENT_NOT_FOUND[]                    = TEXT("SCS_COMPONENT_NOT_FOUND");
    inline constexpr TCHAR ERR_SCS_NOT_FOUND[]                              = TEXT("SCS_NOT_FOUND");
    inline constexpr TCHAR ERR_SCS_OPERATION_FAILED[]                       = TEXT("SCS_OPERATION_FAILED");
    inline constexpr TCHAR ERR_SCS_PARENT_NOT_FOUND[]                       = TEXT("SCS_PARENT_NOT_FOUND");
    inline constexpr TCHAR ERR_SCS_UNAVAILABLE[]                            = TEXT("SCS_UNAVAILABLE");
    inline constexpr TCHAR ERR_SC_DISABLED[]                                = TEXT("SC_DISABLED");
    inline constexpr TCHAR ERR_SECTION_CREATION_FAILED[]                    = TEXT("SECTION_CREATION_FAILED");
    inline constexpr TCHAR ERR_SECTION_FAILED[]                             = TEXT("SECTION_FAILED");
    inline constexpr TCHAR ERR_SECTION_NOT_FOUND[]                          = TEXT("SECTION_NOT_FOUND");
    inline constexpr TCHAR ERR_SECTION_TYPE_MISMATCH[]                      = TEXT("SECTION_TYPE_MISMATCH");
    inline constexpr TCHAR ERR_SECURITY_VIOLATION[]                         = TEXT("SECURITY_VIOLATION");
    inline constexpr TCHAR ERR_SEQUENCE_INVALID[]                           = TEXT("SEQUENCE_INVALID");
    inline constexpr TCHAR ERR_SEQUENCE_NOT_FOUND[]                         = TEXT("SEQUENCE_NOT_FOUND");
    // The asset exists but is not the sequence currently open in Sequencer, and
    // the caller asked not to open it. Distinct from SEQUENCE_NOT_FOUND (no such
    // asset): the ULevelSequenceEditorBlueprintLibrary playhead APIs only ever
    // address the open sequence, so this is a precondition, not a lookup failure.
    inline constexpr TCHAR ERR_SEQUENCE_NOT_OPEN[]                          = TEXT("SEQUENCE_NOT_OPEN");
    inline constexpr TCHAR ERR_SERIALIZE_FAILED[]                           = TEXT("SERIALIZE_FAILED");
    inline constexpr TCHAR ERR_SESSION_LOAD_FAILED[]                        = TEXT("SESSION_LOAD_FAILED");
    inline constexpr TCHAR ERR_SESSION_NOT_FOUND[]                          = TEXT("SESSION_NOT_FOUND");
    inline constexpr TCHAR ERR_SET_DEFAULT_FAILED[]                         = TEXT("SET_DEFAULT_FAILED");
    inline constexpr TCHAR ERR_SET_FAILED[]                                 = TEXT("SET_FAILED");
    inline constexpr TCHAR ERR_SET_NIAGARA_PARAM_FAILED[]                   = TEXT("SET_NIAGARA_PARAM_FAILED");
    inline constexpr TCHAR ERR_SET_PARENT_FAILED[]                          = TEXT("SET_PARENT_FAILED");
    inline constexpr TCHAR ERR_SET_TIME_FAILED[]                            = TEXT("SET_TIME_FAILED");
    inline constexpr TCHAR ERR_SIMULATION_STAGE_CLASS_NOT_FOUND[]           = TEXT("SIMULATION_STAGE_CLASS_NOT_FOUND");
    inline constexpr TCHAR ERR_SIMULATION_STAGE_CREATE_FAILED[]             = TEXT("SIMULATION_STAGE_CREATE_FAILED");
    inline constexpr TCHAR ERR_SIMULATION_STAGE_INVALID_INDEX[]             = TEXT("SIMULATION_STAGE_INVALID_INDEX");
    inline constexpr TCHAR ERR_SIMULATION_STAGE_NOT_FOUND[]                 = TEXT("SIMULATION_STAGE_NOT_FOUND");
    inline constexpr TCHAR ERR_SKELETAL_MESH_NOT_FOUND[]                    = TEXT("SKELETAL_MESH_NOT_FOUND");
    inline constexpr TCHAR ERR_SKELETON_HAS_NO_BONES[]                      = TEXT("SKELETON_HAS_NO_BONES");
    inline constexpr TCHAR ERR_SKELETON_MISMATCH[]                          = TEXT("SKELETON_MISMATCH");
    inline constexpr TCHAR ERR_SKELETON_NOT_FOUND[]                         = TEXT("SKELETON_NOT_FOUND");
    // The ordering trap: bind_skin_weights ran, then geometry was appended, so the
    // vertices added afterwards carry a slot in the skin-weight attribute with zero
    // influences. Re-bind rather than treating it as a missing binding.
    inline constexpr TCHAR ERR_SKIN_WEIGHTS_INCOMPLETE[]                    = TEXT("SKIN_WEIGHTS_INCOMPLETE");
    inline constexpr TCHAR ERR_SKYLIGHT_NOT_FOUND[]                         = TEXT("SKYLIGHT_NOT_FOUND");
    inline constexpr TCHAR ERR_SLATE_NOT_INITIALIZED[]                      = TEXT("SLATE_NOT_INITIALIZED");
    inline constexpr TCHAR ERR_SM_NOT_FOUND[]                               = TEXT("SM_NOT_FOUND");
    inline constexpr TCHAR ERR_SNAPSHOT_NOT_FOUND[]                         = TEXT("SNAPSHOT_NOT_FOUND");
    inline constexpr TCHAR ERR_SOCKET_EXISTS[]                              = TEXT("SOCKET_EXISTS");
    inline constexpr TCHAR ERR_SOCKET_NOT_FOUND[]                           = TEXT("SOCKET_NOT_FOUND");
    inline constexpr TCHAR ERR_SOUNDWAVE_NOT_FOUND[]                        = TEXT("SOUNDWAVE_NOT_FOUND");
    inline constexpr TCHAR ERR_SOUND_LOAD_FAILED[]                          = TEXT("SOUND_LOAD_FAILED");
    inline constexpr TCHAR ERR_SOUND_WAVE_NOT_FOUND[]                       = TEXT("SOUND_WAVE_NOT_FOUND");
    inline constexpr TCHAR ERR_SOURCE_CONTROL_DISABLED[]                    = TEXT("SOURCE_CONTROL_DISABLED");
    inline constexpr TCHAR ERR_SOURCE_EFFECT_NOT_AVAILABLE[]                = TEXT("SOURCE_EFFECT_NOT_AVAILABLE");
    inline constexpr TCHAR ERR_SOURCE_NODE_NOT_FOUND[]                      = TEXT("SOURCE_NODE_NOT_FOUND");
    inline constexpr TCHAR ERR_SOURCE_NOT_FOUND[]                           = TEXT("SOURCE_NOT_FOUND");
    // A source compiler was asked to infer outputPath, but the source is not an unambiguous
    // <Project>/Content/<relative>/<basename>.<format> path. The recovery is to pass the
    // explicit outputPath; no fallback package or guessed mount is ever selected.
    inline constexpr TCHAR ERR_SOURCE_OUTPUT_PATH_NOT_DERIVABLE[]           = TEXT("SOURCE_OUTPUT_PATH_NOT_DERIVABLE");
    inline constexpr TCHAR ERR_SOURCE_STATE_NOT_FOUND[]                     = TEXT("SOURCE_STATE_NOT_FOUND");
    inline constexpr TCHAR ERR_SPAWNABLE_CREATION_FAILED[]                  = TEXT("SPAWNABLE_CREATION_FAILED");
    inline constexpr TCHAR ERR_SPAWN_FAILED[]                               = TEXT("SPAWN_FAILED");
    inline constexpr TCHAR ERR_SPEAKER_NOT_FOUND[]                          = TEXT("SPEAKER_NOT_FOUND");
    inline constexpr TCHAR ERR_SPLINE_COMPONENT_NOT_FOUND[]                 = TEXT("SPLINE_COMPONENT_NOT_FOUND");
    inline constexpr TCHAR ERR_SPLINE_NOT_FOUND[]                           = TEXT("SPLINE_NOT_FOUND");
    inline constexpr TCHAR ERR_SPLIT_SCREEN_ERROR[]                         = TEXT("SPLIT_SCREEN_ERROR");
    inline constexpr TCHAR ERR_STACK_EMPTY[]                                = TEXT("STACK_EMPTY");
    inline constexpr TCHAR ERR_STATE_CREATE_FAILED[]                        = TEXT("STATE_CREATE_FAILED");
    inline constexpr TCHAR ERR_STATE_FAILED[]                               = TEXT("STATE_FAILED");
    inline constexpr TCHAR ERR_STATE_INPUT_PIN_MISSING[]                    = TEXT("STATE_INPUT_PIN_MISSING");
    inline constexpr TCHAR ERR_STATE_MACHINE_CREATE_FAILED[]                = TEXT("STATE_MACHINE_CREATE_FAILED");
    inline constexpr TCHAR ERR_STATE_NOT_FOUND[]                            = TEXT("STATE_NOT_FOUND");
    inline constexpr TCHAR ERR_STATIC_SWITCH_NOT_FOUND[]                    = TEXT("STATIC_SWITCH_NOT_FOUND");
    // editor.step_frame: a previous step has not answered yet. The verb responds only after the
    // frame it asked for has run, and it re-points the process and Slate clocks for exactly that
    // frame, so overlapping steps would have to share one set of saved originals. Refused rather
    // than queued - a caller that fires two steps wants two frames, and would get one.
    inline constexpr TCHAR ERR_STEP_IN_PROGRESS[]                           = TEXT("STEP_IN_PROGRESS");
    // The short-time Fourier transform could not run: the FFT algorithm factory refused the
    // requested size, or the window is longer than the buffer. Never reported as an empty
    // spectrogram - an analysis that did not run is not an analysis that found nothing.
    inline constexpr TCHAR ERR_STFT_FAILED[]                                = TEXT("STFT_FAILED");
    inline constexpr TCHAR ERR_STOP_FAILED[]                                = TEXT("STOP_FAILED");
    inline constexpr TCHAR ERR_STRUCT_FIELD_ADD_FAILED[]                    = TEXT("STRUCT_FIELD_ADD_FAILED");
    inline constexpr TCHAR ERR_STRUCT_FIELD_REMOVE_FAILED[]                 = TEXT("STRUCT_FIELD_REMOVE_FAILED");
    inline constexpr TCHAR ERR_STRUCT_NOT_FOUND[]                           = TEXT("STRUCT_NOT_FOUND");
    inline constexpr TCHAR ERR_SUBMIT_FAILED[]                              = TEXT("SUBMIT_FAILED");
    inline constexpr TCHAR ERR_SUBMIX_NOT_AVAILABLE[]                       = TEXT("SUBMIX_NOT_AVAILABLE");
    inline constexpr TCHAR ERR_SUBMIX_NOT_FOUND[]                           = TEXT("SUBMIX_NOT_FOUND");
    inline constexpr TCHAR ERR_SUBSYSTEM_MISSING[]                          = TEXT("SUBSYSTEM_MISSING");
    inline constexpr TCHAR ERR_SUBSYSTEM_NOT_AVAILABLE[]                    = TEXT("SUBSYSTEM_NOT_AVAILABLE");
    inline constexpr TCHAR ERR_SUBSYSTEM_NOT_FOUND[]                        = TEXT("SUBSYSTEM_NOT_FOUND");
    inline constexpr TCHAR ERR_SUB_SECTION_CREATE_FAILED[]                  = TEXT("SUB_SECTION_CREATE_FAILED");
    inline constexpr TCHAR ERR_SUB_SECTION_NOT_FOUND[]                      = TEXT("SUB_SECTION_NOT_FOUND");
    inline constexpr TCHAR ERR_SUN_NOT_FOUND[]                              = TEXT("SUN_NOT_FOUND");
    inline constexpr TCHAR ERR_SURFACE_NOT_FOUND[]                          = TEXT("SURFACE_NOT_FOUND");
    inline constexpr TCHAR ERR_SURFACE_NOT_SUPPORTED[]                      = TEXT("SURFACE_NOT_SUPPORTED");
    // spatial.place_on_surface, worldPoint mode: the downward probe from the caller's own point
    // hit nothing, so no surface was ever measured. Distinct from SURFACE_NOT_FOUND (which the
    // screen / dropDown modes emit) because the recovery differs: those two mean "aim somewhere
    // else", this one additionally offers assumePointIsSurface for the deliberate case where the
    // caller really does want their own point treated as the surface. The verb used to take that
    // fallback silently and answer placed:true, which is how props ended up beyond the map edge.
    inline constexpr TCHAR ERR_SURFACE_TRACE_MISSED[]                       = TEXT("SURFACE_TRACE_MISSED");
    inline constexpr TCHAR ERR_SYSTEM_NOT_COMPILED[]                        = TEXT("SYSTEM_NOT_COMPILED");
    inline constexpr TCHAR ERR_SYSTEM_NOT_FOUND[]                           = TEXT("SYSTEM_NOT_FOUND");
    inline constexpr TCHAR ERR_SYSTEM_VIEW_MODEL_UNAVAILABLE[]              = TEXT("SYSTEM_VIEW_MODEL_UNAVAILABLE");
    inline constexpr TCHAR ERR_TESTS_FAILED[]                               = TEXT("TESTS_FAILED");
    inline constexpr TCHAR ERR_TESTS_SKIPPED[]                              = TEXT("TESTS_SKIPPED");
    inline constexpr TCHAR ERR_TEST_DISCOVERY_TIMEOUT[]                     = TEXT("TEST_DISCOVERY_TIMEOUT");
    // An isolated automation child exited without a terminal queue-drain marker or with counts
    // that do not reconcile. Zero recorded failures cannot turn that truncated run green.
    inline constexpr TCHAR ERR_TEST_RUN_INCOMPLETE[]                        = TEXT("TEST_RUN_INCOMPLETE");
    // An isolated system.run_tests child exceeded its bounded wall-clock budget and its owned
    // process tree was terminated. The result identifies the timed-out group for a scoped retry.
    inline constexpr TCHAR ERR_TEST_RUN_TIMEOUT[]                           = TEXT("TEST_RUN_TIMEOUT");
    // geometry.recompute_tangents: the engine's ComputeTangents refused. Reachable two ways, and
    // the first is ordinary rather than exotic - a mesh with no UV layer or no normal layer
    // ("TargetMesh is missing UV Set or Normals required to compute Tangents"), which is exactly
    // what geometry.import_obj produces, since AppendBuffersToMesh sets the UV layer count from
    // buffers that carry none. The second is MikkTSpace itself failing. Neither used to be
    // detectable: ComputeTangents returns TargetMesh on every path and reports only into its
    // UGeometryScriptDebug argument.
    inline constexpr TCHAR ERR_TANGENTS_FAILED[]                            = TEXT("TANGENTS_FAILED");
    inline constexpr TCHAR ERR_TARGET_AMBIGUOUS[]                           = TEXT("TARGET_AMBIGUOUS");
    inline constexpr TCHAR ERR_TARGET_CHANGED[]                             = TEXT("TARGET_CHANGED");
    inline constexpr TCHAR ERR_TARGET_NODE_NOT_FOUND[]                      = TEXT("TARGET_NODE_NOT_FOUND");
    inline constexpr TCHAR ERR_TARGET_NOT_FOUND[]                           = TEXT("TARGET_NOT_FOUND");
    inline constexpr TCHAR ERR_TARGET_STATE_NOT_FOUND[]                     = TEXT("TARGET_STATE_NOT_FOUND");
    inline constexpr TCHAR ERR_TEMP_FILE_WRITE_FAILED[]                     = TEXT("TEMP_FILE_WRITE_FAILED");
    // geometry.subdivide / geometry.poke: the engine's ApplyPNTessellation rejected the inputs
    // ("The inputs are invalid") or failed to compute ("Tessellation failed"). Both report only
    // into UGeometryScriptDebug, so before this code the verb answered success over a mesh it had
    // not subdivided.
    inline constexpr TCHAR ERR_TESSELLATION_FAILED[]                        = TEXT("TESSELLATION_FAILED");
    inline constexpr TCHAR ERR_TEXTURE_ERROR[]                              = TEXT("TEXTURE_ERROR");
    inline constexpr TCHAR ERR_TEXTURE_NOT_FOUND[]                          = TEXT("TEXTURE_NOT_FOUND");
    inline constexpr TCHAR ERR_THUMBNAIL_GENERATION_FAILED[]                = TEXT("THUMBNAIL_GENERATION_FAILED");
    inline constexpr TCHAR ERR_TICKET_NOT_FOUND[]                           = TEXT("TICKET_NOT_FOUND");
    // image.tile / render.capture_ortho_tiles: the requested grid is past a burst ceiling -
    // cols x rows over PinWrightImage::MaxTilesPerCall, or over
    // PinWrightOrthoTiles::MaxTilesPerCall / MaxTotalPixelsPerCall on the capture path, which
    // also caps total output PIXELS because that is what the time actually tracks. Every tile is
    // a separate render, encode and file write on the game thread with no job handle to cancel,
    // so an unbounded grid is an unobservable wedge that outlives its own caller. Cut the work
    // into several calls instead.
    inline constexpr TCHAR ERR_TILE_BUDGET_EXCEEDED[]                       = TEXT("TILE_BUDGET_EXCEEDED");
    // image.*: a tile index names a tile outside the georeference's cols x rows grid.
    inline constexpr TCHAR ERR_TILE_OUT_OF_RANGE[]                          = TEXT("TILE_OUT_OF_RANGE");
    inline constexpr TCHAR ERR_TIMEOUT[]                                    = TEXT("TIMEOUT");
    inline constexpr TCHAR ERR_TOO_MANY_SHOTS[]                             = TEXT("TOO_MANY_SHOTS");
    inline constexpr TCHAR ERR_TRACE_NOT_FOUND[]                            = TEXT("TRACE_NOT_FOUND");
    inline constexpr TCHAR ERR_TRACK_CREATE_FAILED[]                        = TEXT("TRACK_CREATE_FAILED");
    inline constexpr TCHAR ERR_TRACK_CREATION_FAILED[]                      = TEXT("TRACK_CREATION_FAILED");
    // sequencer.add_track was given a trackName it cannot store on the track: either the
    // resolved track class is outside the UMovieSceneNameableTrack subtree (nothing there holds
    // a display name), or the name failed readback after being written. Deliberately an error
    // rather than a success carrying the requested name: the previous behaviour echoed the
    // caller's trackName back unapplied, and six sequencer verbs then resolve tracks by that
    // string, so the response handed the agent an identifier guaranteed to answer
    // TRACK_NOT_FOUND on its very next call. Both branches leave the sequence unmodified.
    inline constexpr TCHAR ERR_TRACK_NAME_NOT_APPLIED[]                     = TEXT("TRACK_NAME_NOT_APPLIED");
    inline constexpr TCHAR ERR_TRACK_NOT_FOUND[]                            = TEXT("TRACK_NOT_FOUND");
    inline constexpr TCHAR ERR_TRACK_OP_FAILED[]                            = TEXT("TRACK_OP_FAILED");
    inline constexpr TCHAR ERR_TRANSFORM_MISMATCH[]                         = TEXT("TRANSFORM_MISMATCH");
    inline constexpr TCHAR ERR_TRANSITION_CREATE_FAILED[]                   = TEXT("TRANSITION_CREATE_FAILED");
    inline constexpr TCHAR ERR_TRANSITION_NOT_FOUND[]                       = TEXT("TRANSITION_NOT_FOUND");
    inline constexpr TCHAR ERR_TRAVEL_REFUSED[]                             = TEXT("TRAVEL_REFUSED");
    inline constexpr TCHAR ERR_TREE_EMPTY[]                                 = TEXT("TREE_EMPTY");
    inline constexpr TCHAR ERR_TREE_PROGRAMMATIC[]                          = TEXT("TREE_PROGRAMMATIC");
    inline constexpr TCHAR ERR_TYPE_MISMATCH[]                              = TEXT("TYPE_MISMATCH");
    inline constexpr TCHAR ERR_TYPE_NOT_FOUND[]                             = TEXT("TYPE_NOT_FOUND");
    inline constexpr TCHAR ERR_UBT_NOT_FOUND[]                              = TEXT("UBT_NOT_FOUND");
    inline constexpr TCHAR ERR_UNDO_NOT_REVERSIBLE[]                        = TEXT("UNDO_NOT_REVERSIBLE");
    // A recipe named an audio effect the registry does not carry. An error rather than a
    // skipped layer: a typo'd effect name that silently passes the dry signal through is
    // indistinguishable from an effect that had no audible result.
    inline constexpr TCHAR ERR_UNKNOWN_EFFECT[]                             = TEXT("UNKNOWN_EFFECT");
    // A recipe named a generator the registry does not carry. Same reasoning as
    // UNKNOWN_EFFECT, one step earlier: an unrecognized generator renders silence, which
    // reads as a working pipeline that produced a quiet sound.
    inline constexpr TCHAR ERR_UNKNOWN_GENERATOR[]                          = TEXT("UNKNOWN_GENERATOR");
    // One level below UNKNOWN_PARAMS, and emitted only by FRpcDispatcher::ValidateHandlerParams:
    // the caller sent a key inside an object/array parameter that DECLARES its nested schema
    // (FParamSpec::NestedKeys) and does not accept that key. A separate code from UNKNOWN_PARAMS
    // because the two need different fixes - one is a top-level parameter name the caller can look
    // up in the wiki's parameter list, the other is a key inside a value whose accepted set the
    // refusal message has to carry. A parameter that declares no nested schema emits neither.
    inline constexpr TCHAR ERR_UNKNOWN_NESTED_PARAMS[]                      = TEXT("UNKNOWN_NESTED_PARAMS");
    inline constexpr TCHAR ERR_UNKNOWN_NODE_TYPE[]                          = TEXT("UNKNOWN_NODE_TYPE");
    inline constexpr TCHAR ERR_UNKNOWN_OPERATION[]                          = TEXT("UNKNOWN_OPERATION");
    inline constexpr TCHAR ERR_UNKNOWN_QUALITY[]                            = TEXT("UNKNOWN_QUALITY");
    inline constexpr TCHAR ERR_UNKNOWN_STREAMING_METHOD[]                   = TEXT("UNKNOWN_STREAMING_METHOD");
    inline constexpr TCHAR ERR_UNKNOWN_TYPE[]                               = TEXT("UNKNOWN_TYPE");
    inline constexpr TCHAR ERR_UNKNOWN_VIEW_MODE[]                          = TEXT("UNKNOWN_VIEW_MODE");
    inline constexpr TCHAR ERR_UNSAVED_CHANGES[]                            = TEXT("UNSAVED_CHANGES");
    inline constexpr TCHAR ERR_UNSUPPORTED[]                                = TEXT("UNSUPPORTED");
    inline constexpr TCHAR ERR_UNSUPPORTED_ARGUMENT[]                       = TEXT("UNSUPPORTED_ARGUMENT");
    inline constexpr TCHAR ERR_UNSUPPORTED_ASSET[]                          = TEXT("UNSUPPORTED_ASSET");
    inline constexpr TCHAR ERR_UNSUPPORTED_ASSET_CLASS[]                    = TEXT("UNSUPPORTED_ASSET_CLASS");
    inline constexpr TCHAR ERR_UNSUPPORTED_ASSET_EDITOR[]                   = TEXT("UNSUPPORTED_ASSET_EDITOR");
    inline constexpr TCHAR ERR_UNSUPPORTED_BLUEPRINT[]                      = TEXT("UNSUPPORTED_BLUEPRINT");
    inline constexpr TCHAR ERR_UNSUPPORTED_CHANNEL[]                        = TEXT("UNSUPPORTED_CHANNEL");
    inline constexpr TCHAR ERR_UNSUPPORTED_COLUMN[]                         = TEXT("UNSUPPORTED_COLUMN");
    inline constexpr TCHAR ERR_UNSUPPORTED_ENGINE_VERSION[]                 = TEXT("UNSUPPORTED_ENGINE_VERSION");
    inline constexpr TCHAR ERR_UNSUPPORTED_INPUT_VALUE[]                    = TEXT("UNSUPPORTED_INPUT_VALUE");
    inline constexpr TCHAR ERR_UNSUPPORTED_KEY_TYPE[]                       = TEXT("UNSUPPORTED_KEY_TYPE");
    inline constexpr TCHAR ERR_UNSUPPORTED_NODE[]                           = TEXT("UNSUPPORTED_NODE");
    inline constexpr TCHAR ERR_UNSUPPORTED_NODE_CLASS[]                     = TEXT("UNSUPPORTED_NODE_CLASS");
    inline constexpr TCHAR ERR_UNSUPPORTED_OPERATION[]                      = TEXT("UNSUPPORTED_OPERATION");
    inline constexpr TCHAR ERR_UNSUPPORTED_OPTION[]                         = TEXT("UNSUPPORTED_OPTION");
    inline constexpr TCHAR ERR_UNSUPPORTED_ORTHOGRAPHIC_ROTATION[]          = TEXT("UNSUPPORTED_ORTHOGRAPHIC_ROTATION");
    inline constexpr TCHAR ERR_UNSUPPORTED_PARAM_TYPE[]                     = TEXT("UNSUPPORTED_PARAM_TYPE");
    inline constexpr TCHAR ERR_UNSUPPORTED_PROPERTY[]                       = TEXT("UNSUPPORTED_PROPERTY");
    inline constexpr TCHAR ERR_UNSUPPORTED_RESULT[]                         = TEXT("UNSUPPORTED_RESULT");
    inline constexpr TCHAR ERR_UNSUPPORTED_SCOPE[]                          = TEXT("UNSUPPORTED_SCOPE");
    inline constexpr TCHAR ERR_UNSUPPORTED_SHAPE[]                          = TEXT("UNSUPPORTED_SHAPE");
    inline constexpr TCHAR ERR_UNSUPPORTED_SOURCE_KIND[]                    = TEXT("UNSUPPORTED_SOURCE_KIND");
    inline constexpr TCHAR ERR_UNSUPPORTED_TARGET[]                         = TEXT("UNSUPPORTED_TARGET");
    inline constexpr TCHAR ERR_UNSUPPORTED_TYPE[]                           = TEXT("UNSUPPORTED_TYPE");
    inline constexpr TCHAR ERR_UNSUPPORTED_VALUE_TYPE[]                     = TEXT("UNSUPPORTED_VALUE_TYPE");
    inline constexpr TCHAR ERR_UNSUPPORTED_VERSION[]                        = TEXT("UNSUPPORTED_VERSION");
    inline constexpr TCHAR ERR_USE_ASSET_IMPORT[]                           = TEXT("USE_ASSET_IMPORT");
    // geometry.unwrap_uv / geometry.auto_uv (and the .pwmodel xatlas / patch_builder uv ops): the
    // engine's automatic UV generator refused. XAtlas rejects a non-compact mesh outright ("Try
    // calling CompactMesh"), and both generators report an outright generation failure; all of it
    // travels only through UGeometryScriptDebug, so the verbs used to answer success having
    // written no UV elements at all. Distinct from NO_UV_ELEMENTS, which is this module's own
    // post-check on the result, and from UV_LAYER_ERROR, which is about the layer, not the
    // generator.
    inline constexpr TCHAR ERR_UV_GENERATION_FAILED[]                       = TEXT("UV_GENERATION_FAILED");
    inline constexpr TCHAR ERR_UV_LAYER_ERROR[]                             = TEXT("UV_LAYER_ERROR");
    inline constexpr TCHAR ERR_UnsupportedNodeClass[]                       = TEXT("UnsupportedNodeClass");
    inline constexpr TCHAR ERR_VALIDATION_FAILED[]                          = TEXT("VALIDATION_FAILED");
    inline constexpr TCHAR ERR_VARIABLE_FAILED[]                            = TEXT("VARIABLE_FAILED");
    inline constexpr TCHAR ERR_VARIABLE_NOT_FOUND[]                         = TEXT("VARIABLE_NOT_FOUND");
    inline constexpr TCHAR ERR_VBONE_NOT_FOUND[]                            = TEXT("VBONE_NOT_FOUND");
    inline constexpr TCHAR ERR_VERIFICATION_FAILED[]                        = TEXT("VERIFICATION_FAILED");
    inline constexpr TCHAR ERR_VIEWPORT_NOT_AVAILABLE[]                     = TEXT("VIEWPORT_NOT_AVAILABLE");
    inline constexpr TCHAR ERR_VIEWPORT_RESTORE_FAILED[]                    = TEXT("VIEWPORT_RESTORE_FAILED");
    inline constexpr TCHAR ERR_VIEWPORT_WORLD_MISMATCH[]                    = TEXT("VIEWPORT_WORLD_MISMATCH");
    // A view mode whose picture is chosen by a separate sub-visualisation (VisualizeBuffer,
    // VisualizeLumen, RayTracingDebug, ...) was requested with none selected on the viewport.
    // An error rather than an applied mode: the bare mode renders its overview default, which
    // looks like a real answer to the request that was made.
    inline constexpr TCHAR ERR_VIEW_MODE_NEEDS_COMPANION[]                  = TEXT("VIEW_MODE_NEEDS_COMPANION");
    // The named mode sets no show flag that Lit does not, so a capture in it would be a Lit
    // capture with a different label (VMI_GroupLODColoration, the deprecated VMI_Lit_Wireframe).
    inline constexpr TCHAR ERR_VIEW_MODE_NOT_RENDERABLE[]                   = TEXT("VIEW_MODE_NOT_RENDERABLE");
    // The engine's own EngineShowFlagOverride strips the flag this mode depends on before the
    // frame is drawn (ray tracing disabled, for the ray-tracing modes). Refused rather than set,
    // because setting it renders a plausible near-Lit picture that is not the requested mode.
    inline constexpr TCHAR ERR_VIEW_MODE_UNAVAILABLE[]                      = TEXT("VIEW_MODE_UNAVAILABLE");
    inline constexpr TCHAR ERR_VIRTUAL_BONE_FAILED[]                        = TEXT("VIRTUAL_BONE_FAILED");
    inline constexpr TCHAR ERR_VISIBILITY_MISMATCH[]                        = TEXT("VISIBILITY_MISMATCH");
    inline constexpr TCHAR ERR_VOICE_CHAT_ERROR[]                           = TEXT("VOICE_CHAT_ERROR");
    inline constexpr TCHAR ERR_VOICE_NOT_FOUND[]                            = TEXT("VOICE_NOT_FOUND");
    inline constexpr TCHAR ERR_WATER_PLUGIN_NOT_AVAILABLE[]                 = TEXT("WATER_PLUGIN_NOT_AVAILABLE");
    inline constexpr TCHAR ERR_WAVE_NOT_FOUND[]                             = TEXT("WAVE_NOT_FOUND");
    inline constexpr TCHAR ERR_WEB_BROWSER_NOT_FOUND[]                      = TEXT("WEB_BROWSER_NOT_FOUND");
    inline constexpr TCHAR ERR_WEB_QUERY_FAILED[]                           = TEXT("WEB_QUERY_FAILED");
    // widget.bind: propertyName reaches neither of UMG's two lookups ("<name>Delegate", then the
    // name verbatim as a bindable event), so a record written under it would resolve to nothing at
    // runtime and be indistinguishable from a live binding in widget.export_xml. The payload
    // carries bindableProperties / bindableEvents for the widget's class.
    inline constexpr TCHAR ERR_WIDGET_BINDING_NAME_UNRESOLVED[]             = TEXT("WIDGET_BINDING_NAME_UNRESOLVED");
    // widget.bind: the name IS a real event, but a multicast one (UButton::OnClicked and friends).
    // The Bindings array only carries single-cast FDelegateProperty; distinct from
    // WIDGET_BINDING_NAME_UNRESOLVED so a caller can route to blueprint.compile_bpir without
    // parsing the message.
    inline constexpr TCHAR ERR_WIDGET_BINDING_IS_MULTICAST_EVENT[]          = TEXT("WIDGET_BINDING_IS_MULTICAST_EVENT");
    inline constexpr TCHAR ERR_WIDGET_NOT_FOUND[]                           = TEXT("WIDGET_NOT_FOUND");
    inline constexpr TCHAR ERR_WINDOW_MAXIMIZED[]                           = TEXT("WINDOW_MAXIMIZED");
    // editor.set_window_state: refuses to minimize a window while an automation session is
    // running (GIsAutomationTesting). A minimized window stops rendering, so every profiling and
    // render number taken afterwards is a frozen last value that reads as a scene regression.
    inline constexpr TCHAR ERR_WINDOW_MINIMIZE_REFUSED[]                    = TEXT("WINDOW_MINIMIZE_REFUSED");
    inline constexpr TCHAR ERR_WINDOW_NOT_FOUND[]                           = TEXT("WINDOW_NOT_FOUND");
    // editor.set_window_state: the selector DID match a window, but it carries no native platform
    // window, so SWindow::Restore/Maximize/Minimize are no-ops on it. Distinct from NO_WINDOWS /
    // WINDOW_NOT_FOUND so a caller can tell "nothing matched" from "matched, but unchangeable".
    inline constexpr TCHAR ERR_WINDOW_STATE_NOT_CHANGEABLE[]                = TEXT("WINDOW_STATE_NOT_CHANGEABLE");
    inline constexpr TCHAR ERR_WORLD_MISMATCH[]                             = TEXT("WORLD_MISMATCH");
    inline constexpr TCHAR ERR_WORLD_NOT_AVAILABLE[]                        = TEXT("WORLD_NOT_AVAILABLE");
    inline constexpr TCHAR ERR_WORLD_NOT_FOUND[]                            = TEXT("WORLD_NOT_FOUND");
    inline constexpr TCHAR ERR_WORLD_PARTITION_NOT_ENABLED[]                = TEXT("WORLD_PARTITION_NOT_ENABLED");
    inline constexpr TCHAR ERR_WRITE_FAILED[]                               = TEXT("WRITE_FAILED");
    inline constexpr TCHAR ERR_WRONG_ACTOR_CLASS[]                          = TEXT("WRONG_ACTOR_CLASS");
    inline constexpr TCHAR ERR_WRONG_EXPRESSION_TYPE[]                      = TEXT("WRONG_EXPRESSION_TYPE");
    inline constexpr TCHAR ERR_WRONG_NODE_TYPE[]                            = TEXT("WRONG_NODE_TYPE");
}
