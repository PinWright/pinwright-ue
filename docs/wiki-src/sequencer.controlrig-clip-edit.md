# sequencer.controlrig-clip-edit

Edit an existing AnimSequence through a Control Rig and export the result as a new clip, without modifying the source asset. Every step is a PinWright verb; nothing links the export back into the sequence.

## The chain

1. `sequencer.create` a scratch Level Sequence.
2. Bind a live skeletal-mesh actor carrying the clip's skeleton: `sequencer.add_actor` on an actor already in the level (a SkeletalMesh asset cannot be added as a spawnable yet).
3. `sequencer.add_animation_track` with the source clip (`animSequencePath`) on that binding.
4. `sequencer.bake_to_controlrig` on the binding. The engine evaluates the clip into an FK Control Rig track (one control per bone, named `<bone>_CONTROL`) and disables the skeletal animation track. Check `keysWritten`.
5. `sequencer.list_controls` to get the control names and types.
6. Edit: `sequencer.set_control_keys` (many controls x many frames per call, `space: local` for channel values, `space: world` for world transforms), `sequencer.pin_controls` to hold an effector in place, `sequencer.get_control_values` to read poses before and after.
7. `sequencer.export_anim_sequence` with a new `outAssetPath`. It bakes the evaluated binding into a new AnimSequence and reports `boneTrackCount` / `boneKeysWritten` read back off the written asset.

The source AnimSequence is only read: the bake samples it through the sequence, and the export writes a different asset. `PinWright.Sequencer.ControlRigKeys.ClipEditWorkflowLeavesSourceUnchanged` runs this chain on mannequin content and asserts the source `.uasset` is byte-identical and its package not dirtied afterwards.

## Things that bite

- **The bake is destructive on the sequence.** It removes any existing Control Rig track on the binding (`replacedControlRigTracks`). Bake once, then edit.
- **FK world keys need the parent chain.** World space poses the rig from every control's channels at that frame and runs the FK forward solve; keys are solved in request order, so key a parent before its children in one call.
- **World space is refused, not guessed,** for an additive rig or a binding moved by a transform/attach track (`CONTROL_WORLD_SPACE_UNAVAILABLE`). Use `space: local` there.
- **A held pose exports as one key per bone track.** That is the sequencer data model, not a truncated export.
