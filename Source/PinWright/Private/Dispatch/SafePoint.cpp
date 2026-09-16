// Copyright (c) 2026 Alexander Penkin. MIT License.

// Out-of-line half of Dispatch/SafePoint.h: the log category and the tick-unsafe
// method table. The table lives here rather than in the header so Unity builds
// materialise it exactly once and adding a verb does not recompile every handler.

#include "Dispatch/SafePoint.h"

DEFINE_LOG_CATEGORY(LogPinWrightSafePoint);

namespace PinWrightSafePoint
{
namespace
{
    // Every entry is a REGISTERED method name (the first argument of its
    // REGISTER_RPC_HANDLER), verified against the source. A typo here fails
    // silently - the verb simply stays ungated - which is what
    // PinWright.core.safe_point.TickUnsafeMethodsAreRegistered guards.
    //
    // Two hazard families, both fatal from inside UWorld::Tick:
    //
    // A. LEVEL / WORLD TEARDOWN -> CollectGarbage -> ~ULevel -> FreeTickTaskLevel,
    //    which asserts !LevelList.Contains(TickTaskLevel) (TickTaskManager.cpp:1990).
    //    Full chain evidence per verb is in Dispatch/SafePoint.h.
    //
    // B. RE-ENTRANT SLATE TICK / VIEWPORT DRAW / RENDER FLUSH from a handler body.
    //    The capture family calls PreviewViewportCaptureUtils::PumpViewport
    //    (SlateApp.Tick(ESlateTickType::TimeAndWidgets) + SceneViewport->Draw() +
    //    FlushRenderingCommands; the pump deliberately does NOT process platform
    //    input or invalidate hit proxies -- see the comment over PumpViewport) at
    //    least three times per capture from
    //    CaptureEditorViewportToPng (defined at :1589; the three pump sites are
    //    :2092, :2148 and :2231). Pumping Slate and drawing a
    //    viewport from inside the world tick re-enters the frame the engine is
    //    already in the middle of; FlushRenderingCommands from inside the tick's
    //    parallel-task wait is a documented stall/deadlock source. This family is
    //    NOT a LevelList problem, but bInTick is the correct gate for it too.
    //
    // NOT listed, deliberately:
    //   - level.load / editor.open_level / editor.open_asset: level.load carries
    //     its own in-handler RunAtSafePoint gate, which also covers the two
    //     cross-dispatch callers that reach it through
    //     FRpcDispatcher::DispatchMethod and therefore bypass this table.
    //     Listing it here as well would give one verb two gates.
    //   - render.create_render_target, render.attach_render_target_to_volume,
    //     render.lumen_update_scene, rendering.*: no draw, no flush, no teardown.
    //   - the audio.authoring.*_metasound* mutators (add_metasound_input / _node /
    //     _output, connect_metasound_nodes, set_metasound_default,
    //     set_metasound_node_input_default, add/remove/set_metasound_variable*): none
    //     of them is listed, and the new ones must stay consistent with that. They edit
    //     an in-memory FMetasoundFrontendDocument through the builder and then call
    //     McpSafeAssetSave, which only marks the package dirty (AssetUtils.h:47-60) — no
    //     synchronous package write, no ObjectTools delete, no CollectGarbage, no Slate
    //     pump. That is the whole difference from the two audio verbs that ARE listed
    //     under E: those create an asset and write the .uasset on the calling stack.
    const TCHAR* const GTickUnsafeMethodNames[] =
    {
        // --- A: level / world teardown ---------------------------------------
        // GEditor->NewMap(false) (LevelHandler.cpp:574) -> EditorDestroyWorld ->
        // Cleanse -> CollectGarbage.
        TEXT("level.create"),
        // UEditorLevelUtils::RemoveLevelFromWorld (LevelHandler.cpp:1252) ->
        // RemoveLevelsFromWorld -> GEditor->Cleanse (EditorLevelUtils.cpp:933).
        TEXT("level.remove_from_world"),
        // OutgoingWorld->DestroyWorld (LevelStructureHandler.cpp:389) +
        // GWorld reassignment (:399) on the world that may be mid-tick.
        TEXT("level.structure.create_level"),
        // GEditor->NewMap() (LightingHandler.cpp:921), same chain as level.create,
        // then spawns two lights into the freshly installed world.
        TEXT("lighting.create_lighting_enabled_level"),
        // CloseAllEditorsForAsset + ForceGarbageCollection + DeleteAsset in a loop
        // (AnimationHandler.cpp:266-279).
        TEXT("animation.cleanup"),
        // Overwrite publication replaces references and uses ObjectTools deletion,
        // whose engine path can synchronously collect garbage.
        TEXT("animation.setup_retargeting"),

        // --- B: re-entrant Slate tick / viewport draw / render flush ----------
        // McpSafeLevelSave flushes rendering and calls FEditorFileUtils::SaveLevel
        // synchronously from these always-deferred job continuations.
        TEXT("level.save"),
        TEXT("level.save_as"),
        TEXT("effect.advance_simulation"),
        TEXT("effect.step_and_capture"),
        TEXT("render.capture_open_level"),
        // Ownerless USceneCaptureComponent2D capture plus synchronous render-thread flush and
        // readback in a private preview world (MeshPreviewCaptureUtils.cpp).
        TEXT("render.capture_mesh"),
        TEXT("render.capture_asset_preview"),
        TEXT("render.capture_annotated"),
        TEXT("render.capture_animation_preview"),
        // FSceneCaptureProbe registers a USceneCaptureComponent2D into the live
        // editor world, CaptureScene()s and FlushRenderingCommands twice
        // (ZFightingHandler.cpp:380, SceneCaptureProbeUtils.cpp:201/206).
        TEXT("render.detect_z_fighting"),
        // Same hazard as detect_z_fighting and once per tile: FOrthoTileCapture
        // registers a USceneCaptureComponent2D into the live editor world and
        // calls CaptureScene() (which itself runs SendAllEndOfFrameUpdates)
        // followed by FlushRenderingCommands for every tile of the burst
        // (OrthoTileCaptureUtils.cpp CaptureTile).
        TEXT("render.capture_ortho_tiles"),
        TEXT("camera.frame_actor"),
        TEXT("camera.orbit_shots"),
        TEXT("camera.animation_shots"),
        TEXT("editor.screenshot"),
        TEXT("editor.screenshot_window"),
        TEXT("widget.screenshot_designer"),
        TEXT("ui.screenshot"),
        // ThumbnailTools::RenderThumbnail - an offscreen scene render on the
        // calling stack (AssetWorkflowHandler.cpp:716).
        TEXT("asset.generate_thumbnail"),
        // Not capture verbs, but the same synchronous Viewport->Draw(): both exist
        // to force a frame NOW so a following capture sees it, and agents call
        // them immediately before one.
        TEXT("editor.set_camera"),
        TEXT("editor.set_game_view"),

        // --- C: the arbitrary-payload entry points ---------------------------
        // These three are here for the OPPOSITE reason to everything above. A and
        // B are listed because their handler bodies are known to reach a specific
        // hazard. C is listed because its handler body can reach ANY of them and
        // the table cannot tell in advance: `open <map>` is the whole of the A
        // chain (UEditorEngine::Exec -> Map_Load, EditorServer.cpp:2343 ->
        // EditorDestroyWorld :2480 -> Cleanse EditorEngine.cpp:2763 ->
        // CollectGarbage :2859 -> ~ULevel -> FreeTickTaskLevel), and a Python
        // script can pump Slate or swap maps just as directly.
        //
        // They were previously excluded on the argument that a name-based table
        // cannot classify a payload-defined operation. That argument decides the
        // wrong way round: because the payload can be anything, the SAFE default
        // is to assume the worst, not to assume the best. The gate is
        // conservative by construction (SafePoint.h: bInTick strictly contains the
        // LevelList window), so listing them can only over-defer.
        //
        // The cost is bounded and paid only on the unsafe path: the dispatcher
        // gate is `IsTickUnsafeMethod && !IsSafeNow`, so a call that already
        // arrived outside UWorld::Tick runs inline exactly as before. On the
        // unsafe path it costs one UPinWrightSubsystem::Tick pass (0.1s period,
        // PinWrightSubsystem.cpp:208-212; the drain at :303 is unconditional, so
        // the very next pass picks it up).
        //
        // KNOWN GAP, same shape as level.load's: three cross-dispatch callers
        // reach system.console_command through FRpcDispatcher::DispatchMethod
        // (LevelHandler.cpp:545, :630, :687), which bypasses ProcessRequest and
        // therefore this table. All three are already safe - :545/:630 are inside
        // level.create, which is gated above, and :687 is level.stream's
        // PIE-only StreamLevel. Do not add an in-handler gate on top; that would
        // give one operation two gates.
        //
        // WHAT THIS DOES NOT FIX, stated so nobody reads more into it. Two
        // python.execute hazards are untouched by this gate, and NOTHING in the
        // plugin prevents either - they are documented in docs/wiki-src/python.md
        // and that is the only protection that exists:
        //   - CollectGarbage() running with a live Python frame ->
        //     FPythonScriptPlugin::OnPreGarbageCollect faults (board
        //     B-python-execute-reentrant-gc-crash, reached through
        //     MaterialEditingLibrary.recompile_material). That is a
        //     stack-CONTENTS problem, not a stack-POSITION one; the collect is
        //     legal wherever it runs, so moving the frame to the core ticker does
        //     nothing for it.
        //   - An unbounded loop wedging the game thread. The core ticker IS the
        //     game thread, so deferring changes only where the wedge starts.
        //     Detectable after 90s via the ping stall probe, not preventable.
        //     Since 2026-08-16 it is additionally *reportable*, but only with the
        //     script's cooperation: unreal.PinWrightProgressLibrary.report_progress
        //     called between iterations emits MCP progress to a streaming caller
        //     while the loop runs. That is not a fix for this hazard - a script
        //     that never calls it, or that is inside one long engine call, is as
        //     invisible as before, and nothing here can interrupt either.
        TEXT("python.execute"),
        TEXT("system.console_command"),
        TEXT("editor.console_command"),
        // python.callbacks belongs here for a narrower reason than its three neighbours: its
        // payload is NOT arbitrary - it runs only PinWright's own fixed scripts - but both of
        // its paths still enter the interpreter on the handler's stack (an import on the
        // first call, the unregister loop on every clear), and a Python frame on a stack
        // inside UWorld::Tick is the same shape as python.execute's. Listing it costs one
        // 0.1s core-ticker pass, and only when the call actually arrived mid-tick.
        TEXT("python.callbacks"),

        // --- D: editor audio-device work -------------------------------------
        // UEditorEngine::PlayPreviewSound -> ResetPreviewAudioComponent
        // (EditorEngine.cpp:2882-2976) destroys and re-creates the editor's shared
        // PreviewAudioComponent and pushes an active sound into the audio device on
        // the CALLING stack (UAudioComponent::Play -> FAudioDevice::AddNewActiveSound,
        // AudioComponent.cpp:922). In the editor the audio thread is the game thread -
        // stated by the engine itself at AudioComponent.cpp:926-928, which is why Play()
        // can see PlaybackComplete fire before it has even set bIsActive - so all of
        // that lands inside UWorld::Tick's own component tick when the request arrives
        // mid-frame, tearing down and re-registering an audio component while the world
        // is ticking audio components.
        // Deferring is free for this verb: the gate costs at most one 0.1s subsystem
        // tick and the sound is still playing when the response comes back, which is the
        // entire contract of an audition.
        TEXT("audio.synth.audition"),

        // USoundWave::PostEditChangeProperty for CompressionQuality reaches
        // UpdateAsset(), which frees resources and flushes audio rendering commands;
        // defer this setter so the derived-state refresh cannot run inside UWorld::Tick.
        TEXT("audio.authoring.set_sound_wave_properties"),

        // --- E: asset creation + package save --------------------------------
        // First entry of its kind, so the reasoning is spelled out. The handler body
        // runs three things that are all illegal mid-frame:
        //   1. AssetCreatePolicy::Resolve on an occupied path with overwrite:true
        //      deletes the sitting object through ObjectTools, which force-collects
        //      garbage - the same CollectGarbage -> ~ULevel -> FreeTickTaskLevel chain
        //      family A is listed for, reached from a different door.
        //   2. Audio::FSoundWavePCMWriter::SynchronouslyWriteSoundWave calls
        //      CreatePackage + NewObject on the calling stack (SampleBufferIO.cpp:367,
        //      370), and on a re-run that NewObject RECONSTRUCTS the existing USoundWave
        //      in place (UObjectGlobals.cpp:3516-3542) while the editor's audio device
        //      may still hold it.
        //   3. SaveAssetToDiskReportingPresence writes the .uasset synchronously.
        // Deferring costs at most one 0.1s UPinWrightSubsystem::Tick pass and nothing
        // about the response changes - the handler runs later on a safe stack with its
        // original FHandlerContext.
        TEXT("audio.authoring.create_sound_wave_from_pcm"),
        // Blueprint creators call CreatePackage and synchronously save the new asset.
        // The table cannot inspect conditional save flags, so every route stays off UWorld::Tick.
        TEXT("blueprint.create"),
        TEXT("widget.create_widget_blueprint"),
        TEXT("editor.create_utility_widget"),
        TEXT("pcg.create_graph"),
        // Same three hazards, same handler shape, different source of samples: this one
        // takes them from a session candidate instead of an inline JSON array, then runs
        // the identical AssetCreatePolicy::Resolve -> PwCreateSoundWaveAsset ->
        // SaveAssetToDiskReportingPresence sequence. It additionally decodes the created
        // wave back through USoundWave::GetImportedSoundWaveData, whose payload read blocks
        // on a bulk-data TFuture (SoundWave.cpp:1784) under the wave's
        // RawDataCriticalSection - another thing not to do from inside UWorld::Tick.
        // Table entry, never a hand-written gate, and never both.
        TEXT("audio.synth.export"),
        // Reaches E.1-E.3 through the same AssetCreatePolicy::Resolve ->
        // PwCreateSoundWaveAsset -> save -> GetImportedSoundWaveData sequence, but only
        // when the caller passes name+path; without them it writes nothing. Listed anyway,
        // for the reason section F states: the table cannot see which arguments a call will
        // carry, so the conservative listing is the only one that is always right, and a
        // call that arrives outside UWorld::Tick still runs inline.
        //
        // It carries a second hazard the others do not, and it is the reason this entry
        // would exist even without the asset write: the render drives
        // UMetaSoundSource::CreateSoundGenerator, which builds the graph operator on the
        // calling stack and momentarily forces au.MetaSound.EnableAsyncGeneratorBuilder to
        // 0 process-wide. Doing that from inside the world tick would hand a synchronous
        // build to any voice the audio device starts in the same frame.
        TEXT("audio.synth.render_metasound"),
        TEXT("physics.setup_physics_simulation"),
        TEXT("skeleton.create_physics_asset"),

        // The targeted asset writer always reaches SaveAssetToDiskReportingPresence and may
        // synchronously write a package; keep it off a UWorld::Tick stack like every save verb.
        TEXT("asset.save"),

        // These mutators conditionally write their packages through
        // SaveAssetToDiskReportingPresence. The table cannot inspect `save`, so even
        // save:false requests take the conservative between-frame route.
        TEXT("data_table.add_row"),
        TEXT("data_table.set_row"),
        TEXT("data_table.remove_row"),
        TEXT("data_table.set_row_struct"),
        TEXT("eqs.set_context_class"),

        // These asset mutators now conditionally write their package through the same
        // synchronous save helper. The dispatcher cannot inspect `save`, so both routes
        // stay off UWorld::Tick.
        TEXT("asset.set_metadata"),
        TEXT("asset.reset_instance_parameters"),
        TEXT("sequencer.bake_control_space"),

        // --- F: SoundWave decode + content-folder enumeration -----------------
        // The audio.analysis.* family. Every verb in it can be pointed at an
        // `assetPath` instead of a session candidate, and that path runs
        // PwDecodeSoundWave -> USoundWave::GetImportedSoundWaveData, whose
        // payload read BLOCKS on a bulk-data TFuture ("Will block." -
        // SoundWave.cpp:1784) under the wave's RawDataCriticalSection. That is
        // the same hazard audio.synth.export is listed for at E.3, reached
        // through a different door, and it is why all five are here rather than
        // only the ones that also do something else.
        //
        // The table cannot see which source a call will use, so the listing is
        // conservative by construction - exactly the argument section C makes
        // for the arbitrary-payload verbs. Because the source CAN be an asset,
        // the safe default is to assume it is; a call that arrived outside
        // UWorld::Tick still runs inline (the dispatcher gate is
        // `IsTickUnsafeMethod && !IsSafeNow`), and on the unsafe path it costs
        // one 0.1s UPinWrightSubsystem::Tick pass.
        //
        // audit_folder additionally walks the asset registry and calls
        // FAssetData::GetAsset() per row, i.e. a package LOAD per asset, up to
        // its page limit. Loading packages from inside the world tick is the
        // family-A hazard's mirror image and belongs off that stack too.
        //
        // These are read-only verbs: nothing here creates, saves or deletes an
        // asset, so the E hazards (overwrite-delete's forced GC, CreatePackage +
        // in-place NewObject, synchronous .uasset write) do not apply. The
        // blocking payload read and the package loads are the whole reason.
        TEXT("audio.analysis.analyze"),
        TEXT("audio.analysis.audit_folder"),
        TEXT("audio.analysis.compare"),
        TEXT("audio.analysis.decompose"),
        TEXT("audio.analysis.to_recipe"),

        // --- G: music asset creation + package save ---------------------------
        // The two audio.music verbs that write assets. Same hazards as E, reached
        // through the same doors, so they are listed for the same reasons rather
        // than on a family resemblance:
        //
        // audio.music.export_stems runs E.1/E.2/E.3 once PER STEM (up to
        // PwMusicLimits::MaxTracks = 16 of them in one call): AssetCreatePolicy::Resolve
        // with overwrite:true deletes the sitting object through ObjectTools, which
        // force-collects garbage; PwCreateSoundWaveAsset reaches
        // Audio::FSoundWavePCMWriter::SynchronouslyWriteSoundWave -> CreatePackage +
        // NewObject on the calling stack, and on a re-run that NewObject RECONSTRUCTS
        // the existing USoundWave in place (UObjectGlobals.cpp:3516-3542) while the
        // editor's audio device may still hold it; SaveAssetToDiskReportingPresence
        // writes each .uasset synchronously. It then decodes every written wave back
        // through USoundWave::GetImportedSoundWaveData, whose payload read BLOCKS on a
        // bulk-data TFuture ("Will block." - SoundWave.cpp:1784) under the wave's
        // RawDataCriticalSection - the F hazard, sixteen times over.
        //
        // audio.music.build_interactive creates a MetaSoundSource: the same
        // AssetCreatePolicy::Resolve (overwrite:true -> ObjectTools delete -> forced GC),
        // then PwBuildInteractiveMusicGraph builds the document and persists it. Note the
        // contrast with the audio.authoring.*_metasound* MUTATORS, which are deliberately
        // NOT listed: those edit an already-existing document and only mark the package
        // dirty. This verb CREATES the asset and writes it, which is exactly the E
        // distinction.
        //
        // Deferring costs at most one 0.1s UPinWrightSubsystem::Tick pass and nothing
        // about either response changes - the handler runs later on a safe stack with its
        // original FHandlerContext. Table entries, never hand-written gates, and never
        // both.
        //
        // NOT listed: audio.music.render_stems and audio.music.describe_schema. The
        // renderer only fills in-memory FPwAudioBuffers and registers session candidates
        // (no package, no asset, no save, no decode), and describe_schema reads static
        // spec tables. Listing them would over-defer for no hazard, and the table is
        // documentation as much as it is a gate.
        TEXT("audio.music.build_interactive"),
        TEXT("audio.music.export_stems"),

        // --- H: StaticMesh asset creation + a synchronous mesh build -----------
        // model.compile. Every E hazard, reached through GeometryAssetCreate.cpp
        // instead of the audio writers, plus one this table has not carried before:
        //   1. AssetCreatePolicy::Resolve with overwrite:true deletes the sitting
        //      object through ObjectTools, which force-collects garbage - the same
        //      CollectGarbage -> ~ULevel -> FreeTickTaskLevel chain family A is
        //      listed for (GeometryAssetCreate.cpp, the Resolve call above the
        //      provenance check).
        //   2. UE::AssetUtils::CreateStaticMeshAsset runs CreatePackage + NewObject
        //      on the calling stack, and on a recompile onto the same path it hands
        //      the util the occupant's own package so StaticAllocateObject
        //      RECONSTRUCTS the existing UStaticMesh in place (UObjectGlobals.cpp:3568)
        //      while components may still reference it.
        //   3. SaveAssetToDiskReportingPresence writes the .uasset synchronously.
        //   4. NEW for this family: UStaticMesh::PostEditChange builds the mesh, and
        //      the build releases and re-creates render resources - which means render
        //      fences and FlushRenderingCommands on the handler's own stack. The
        //      shipped log shows the recursion this produces from inside a frame:
        //      "FlushRenderingCommands called recursively! 2 calls on the stack."
        //
        // The stack that proves the position rather than argues it, from the shipped
        // 2026-08-19 crash log: FEngineLoop::Tick -> UUnrealEdEngine::Tick ->
        // UEditorEngine::Tick -> FTickableObjectBase::SimpleTickObjects ->
        // UMassEntityEditorSubsystem::Tick -> FTaskBase::WaitWithNamedThreadsSupport ->
        // FNamedTaskThread::ProcessTasksUntilQuit -> TGraphTask<FAsyncGraphTask> ->
        // FRpcDispatcher::ProcessRequest -> FPwModelCompiler::Compile. A THIRD-PARTY
        // subsystem's task wait pumped the game thread's named-thread queue and ran a
        // whole .pwmodel compile inside it.
        //
        // THAT GAP IS NOW CLOSED, and the stack is kept here because it is worth
        // recognising. IsSafeNow() used to read UWorld::bInTick only, and the editor's
        // tickables run BEFORE the editor world tick (EditorEngine.cpp:1933 against
        // :1967), so bInTick was false there and the gate let every listed method
        // through - measured three times in one day, twice with a listed verb (board
        // B-safepoint-tick-gate-inert-on-simpletickobjects-path). IsSafeNow() now also
        // reports unsafe while the game thread is draining one of its own named-thread
        // queues (PinWrightSafePoint::IsInsideNamedThreadPump, SafePoint.h), which is
        // exactly the stack above.
        //
        // What that costs EVERY family in this table, stated once here rather than
        // repeated per entry: a request that arrived over the transport is marshalled
        // with AsyncTask(ENamedThreads::GameThread, ...) and therefore always executes
        // from such a pump, so it now always takes the one-hop deferral instead of only
        // when a world happened to be ticking. One UPinWrightSubsystem::Tick pass, 0.1s
        // worst case, response path unchanged. Direct in-editor callers - automation
        // stacks, the core-ticker drain itself - are not in a pump and still run inline.
        //
        // NOT listed: model.validate and model.describe_ops. bValidateOnly short-circuits
        // before CreateAsset (PwModelCompiler.cpp Run, stage 5), so validate creates no
        // package, builds no UStaticMesh and saves nothing - it only allocates and
        // discards transient UDynamicMesh objects, which is legal mid-frame. describe_ops
        // reads static op tables. Listing either would over-defer for no hazard.
        //
        // NOT a concurrency fix, because there is no concurrency to fix: model.compile
        // runs only on the game thread (ProcessRequest marshals every off-thread request
        // there) and two compiles can never interleave, because a request arriving while
        // another is in flight hits the bProcessingRequest guard below and is queued.
        // What overlapping calls change is only WHERE in the frame each one lands.
        TEXT("model.compile"),
        // StaticMesh rebuild mutators use the shared render-consumer guard, which flushes
        // render commands while the mesh render data is being replaced.
        TEXT("asset.generate_lods"),
        TEXT("asset.nanite_rebuild_mesh"),
        TEXT("render.nanite_rebuild_mesh"),
        TEXT("static_mesh.bake_transform"),
        TEXT("geometry.set_lod_settings"),

        // --- I: master-material edit -> render-state recreation + render flush -
        // The three verbs that COMPLETE a unit of work on a master UMaterial. Each one
        // runs PinWright::MaterialConsumers::NotifyMasterMaterialChanged
        // (Handlers/Material/MaterialLandscapeConsumers.h), whose bare
        // `FMaterialUpdateContext UpdateContext;` takes EOptions::Default =
        // RecreateRenderStates | SyncWithRenderingThread (MaterialShared.h:3525). That
        // builds an FGlobalComponentRecreateRenderStateContext over the whole world and
        // calls FlushRenderingCommands in BOTH the constructor (MaterialShared.cpp:5026)
        // and the destructor (:5078, "Flush rendering commands even though we already did
        // so in the constructor"). RefreshLandscapeConsumers in the same header then calls
        // ALandscapeProxy::UpdateAllComponentMaterialInstances(true) once per consuming
        // landscape, which builds an FComponentRecreateRenderStateContext per landscape
        // component and can flush again per new combination MIC (LandscapeEdit.cpp:620).
        // The flush count per call therefore scales with the consuming landscapes and
        // their components rather than being a fixed two.
        //
        // That is family B's mechanism ("FlushRenderingCommands from inside the tick's
        // parallel-task wait is a documented stall/deadlock source") and family H.4's
        // ("the build releases and re-creates render resources - which means render fences
        // and FlushRenderingCommands on the handler's own stack"), reached through the
        // material door. The shipped evidence has H's shape too: two editor kills 90
        // seconds apart on 2026-08-16, EXCEPTION_ACCESS_VIOLATION on the RENDER thread
        // inside BeginReleaseResource's lambda (RenderingThread.cpp:1533) while draining
        // queued release commands, with the game thread parked in a render fence directly
        // below PinWright frames, each preceded by "FlushRenderingCommands called
        // recursively! 2 calls on the stack." 15 and 21 times in ~170 ms. Board:
        // B-compile-material-not-tick-gated.
        //
        // POSITION, not stack CONTENTS - the distinction family C draws for the Python GC
        // hazard, applied here. This is NOT shader-compiler re-entrancy:
        // MaterialCompileErrorCollector::WaitAndCollect's CacheShaders(Synchronous) plus
        // GShaderCompilingManager->FinishAllCompilation() is a legal game-thread drain
        // wherever it runs, and deferring does not shorten it by a millisecond. What this
        // gate moves off the engine's own frame is the render flush and the world-wide
        // render-state recreation. The length of the block, its missing job handle and the
        // error code the proxy reports while it runs are a separate defect on a separate
        // axis (board: B-compile-material-blocks-and-mislabels); this entry neither fixes
        // nor depends on it, and survives that rework because the update context runs
        // before any wait a job handle could carry.
        //
        // Table entries rather than in-handler gates, per the rule in SafePoint.h: none of
        // the three has a cross-dispatch caller (FRpcDispatcher::DispatchMethod is not
        // called with any of these names), and no handler body in Handlers/Material/ or
        // MGIR/ queues its own AsyncTask(ENamedThreads::GameThread, ...), so each body
        // runs entirely on the stack this gate decides.
        //
        // landscape.set_material reaches the same engine code, but remains deliberately
        // absent here because environment.build cross-dispatches to it outside ProcessRequest.
        // Its handler therefore uses the in-handler RunAtSafePoint route.
        TEXT("material.authoring.compile_material"),
        // The same PinWright::MaterialConsumers::ApplyMasterMaterialEdit call in the same
        // file: rewriting a LandscapeLayerBlend node changes the layer allocation the
        // landscape's combination MICs are keyed on, so it rebuilds every consumer exactly
        // as compile_material does.
        TEXT("material.authoring.configure_layer_blend"),
        // MGIR/MGIRCompiler.cpp FinalizeMaterial runs NotifyMasterMaterialChanged, then
        // UMaterial::ForceRecompileForRendering, then RefreshLandscapeConsumers - the same
        // two halves with a full master recompile between them, once per entry material in
        // the document.
        TEXT("material.compile_mgir"),

        // --- J: package eviction + whole-object-graph reference fixup ----------
        // asset.reload. Its entire job is UPackageTools::ReloadPackages, which is a
        // between-frames operation by construction and reaches THREE families at once
        // on the handler's own stack:
        //   A. TWO unconditional CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS) passes
        //      (PackageReload.cpp:821 per batch, :838 at the end) - the same
        //      CollectGarbage -> ~ULevel -> FreeTickTaskLevel chain family A is listed
        //      for, plus the Python-frame GC hazard family C names.
        //   B. FGlobalComponentReregisterContext over the whole editor
        //      (PackageTools.cpp:944) - render-state recreation and render flushes,
        //      the mechanism families B, H.4 and I are listed for - preceded by
        //      FlushAsyncLoading and GEditor->ResetAllSelectionSets().
        //   J. The one that is new here: ::ReloadPackages snapshots the ENTIRE object
        //      graph into a raw `TArray<UObject*> PotentialReferencers`
        //      (PackageReload.cpp:744; the slow whole-array path, because
        //      `PackageReload.EnableFastPath` defaults to false, PackageReload.cpp:19-23)
        //      and then serialises every entry through FReplaceObjectReferencesArchive
        //      (:774). Serialising a UFunction walks its script bytecode
        //      (UStruct::Serialize -> SerializeExpr -> FPropertyProxyArchive), so any
        //      object destroyed by work that interleaves with A's or B's pumps turns a
        //      later entry of that snapshot into a use-after-free. The shipped evidence
        //      is exactly that: EXCEPTION_ACCESS_VIOLATION at
        //      FPropertyProxyArchive::operator<< (PropertyProxyArchive.h:46, constructing
        //      a TFieldPath from a freed FField*), one frame under
        //      UFunction::Serialize / ReloadPackages, with the whole call arriving from
        //      TGraphTask<FAsyncGraphTask>::ExecuteTask ->
        //      FNamedTaskThread::ProcessTasksNamedThread ->
        //      UMassEntityEditorSubsystem::Tick -> FTickableObjectBase::SimpleTickObjects
        //      -> UEditorEngine::Tick. Board:
        //      B-asset-reload-blueprint-package-crash /
        //      B-asset-reload-access-violation-kills-editor (a shared-editor process kill
        //      that cost three attached streams their unsaved state).
        //
        // THE TABLE ENTRY ONLY WORKS BECAUSE THE HANDLER IS SYNCHRONOUS. asset.reload
        // used to hand its body to AsyncTask(ENamedThreads::GameThread, ...), which is the
        // landscape.set_material shape called out at the end of family I: the gate would
        // rule on the arrival stack while the real work landed on the very named-thread
        // queue the gate exists to get off - and, worse here, outside
        // FRpcDispatcher's bProcessingRequest reentrancy guard, so another agent's RPC
        // could be drained INTO the middle of the reload during one of A's or B's pumps.
        // That redundant marshal was removed with this entry (the dispatcher already
        // marshals every request to the game thread before invoking a handler), so the
        // deferred path now re-enters ProcessRequest from UPinWrightSubsystem::Tick and
        // runs the reload under that guard. Never re-add an AsyncTask there; the comment
        // at the top of the handler says so too.
        //
        // NOT listed, and not an oversight: asset.validate. It re-reads the ALREADY
        // RESIDENT UObject through LoadAsset - no eviction, no fixup pass, no GC, no
        // reregister context.
        TEXT("asset.reload"),

        // --- K: other synchronous graph/render rebuilds -----------------------
        // FixupReferencers loads and re-saves referencing packages, then deletes the
        // repaired redirectors. No cross-dispatch caller bypasses ProcessRequest.
        TEXT("asset.fixup_redirectors"),
        // Forced synchronous landscape-grass regeneration destroys and re-creates
        // instanced components. No cross-dispatch caller bypasses ProcessRequest.
        TEXT("landscape.flush_grass"),

        // --- L: Blueprint compile -> reinstancing queue flush -> live actors trashed --
        // FKismetEditorUtilities::CompileBlueprint flushes the compilation manager's
        // reinstancing queue (BlueprintCompilationManager.cpp:392 -> FlushReinstancingQueueImpl
        // at :2086), which walks every live instance through ReplaceInstancesOfClass_Inner and
        // ends, for actors, at World->EditorDestroyActor(OldActor, bShouldModifyLevel=true)
        // (KismetReinstanceUtilities.cpp:3011). Objects the CURRENT FRAME is ticking are
        // destroyed on the handler's own stack.
        //
        // That is fatal from inside the frame for a reason family A does not cover:
        // FTickTaskLevel cooks one TGraphTask<FTickFunctionTask> per enabled tick function at
        // StartFrame and the task holds a RAW FTickFunction* (TickTaskManager.cpp:284) which
        // DoTask dereferences (:334) into FTickFunction::ExecuteTick - a PURE_VIRTUAL
        // (EngineBaseTypes.h:524). Unregistering a tick function does NOT cancel an
        // already-cooked task (FTickTaskLevel::RemoveTickFunction, TickTaskManager.cpp:1807,
        // only edits the manager's lists), so the task runs against a destructed object.
        // Shipped evidence: `LowLevelFatalError [EngineBaseTypes.h:524] Pure virtual not
        // implemented ()` through TGraphTask<FTickFunctionTask>::ExecuteTask ->
        // FTickTaskSequencer::ReleaseTickGroup -> UWorld::Tick (LevelTick.cpp:1750) ->
        // UEditorEngine::Tick, 229 ms after "Compiling Blueprint '/Game/FPS/AI/BP_EnemyCharacter'",
        // with NO PIE running and no game code on the stack - one editor death taking seven
        // agent streams' in-flight work. Board: E-compile-reinstances-live-instances-no-guard.
        //
        // Table entries rather than in-handler gates, per the rule in SafePoint.h: none of these
        // verbs is reached through FRpcDispatcher::DispatchMethod, and none detaches its compile
        // into another GameThread task, so each runs entirely on the stack this gate decides.
        //
        // This gate fixes POSITION only - it moves the reinstancing pass off the engine's frame.
        // It does not make reinstancing invisible: the pass still destroys and re-creates placed
        // actors in whatever map is loaded, including another agent's. That half is the
        // reported, opt-in precondition in Handlers/Blueprint/BlueprintReinstancingGuard.h
        // (LIVE_INSTANCES_WOULD_BE_REINSTANCED / allowReinstancing), and the two are deliberately
        // separate: the gate cannot consent on the caller's behalf and the precondition cannot
        // move a stack.
        //
        TEXT("ai.assign_behavior_tree"),
        TEXT("ai.assign_blackboard"),
        TEXT("ai.run_behavior_tree"),
        TEXT("blueprint.add_dispatcher"),
        TEXT("blueprint.add_event"),
        TEXT("blueprint.add_function"),
        TEXT("blueprint.add_interface"),
        TEXT("blueprint.add_macro"),
        TEXT("blueprint.add_variable"),
        TEXT("blueprint.compile"),
        TEXT("blueprint.compile_bpir"),
        TEXT("blueprint.delete_unused_variables"),
        TEXT("blueprint.graph.delete_orphaned_nodes"),
        TEXT("blueprint.insert_bpir_at_node"),
        TEXT("blueprint.insert_bpir_before_node"),
        TEXT("blueprint.modify_scs"),
        TEXT("blueprint.remove_event"),
        TEXT("blueprint.remove_function"),
        TEXT("blueprint.remove_interface"),
        TEXT("blueprint.remove_variable"),
        TEXT("blueprint.rename_variable"),
        TEXT("blueprint.reparent"),
        TEXT("blueprint.scs.add_component"),
        TEXT("blueprint.scs.duplicate_component"),
        TEXT("blueprint.scs.remove_component"),
        TEXT("blueprint.scs.reparent_component"),
        TEXT("blueprint.scs.set_property"),
        TEXT("blueprint.scs.set_transform"),
        TEXT("blueprint.set_default"),
        TEXT("blueprint.set_function_settings"),
        TEXT("blueprint.set_variable_metadata"),
        TEXT("blueprint.set_variable_settings"),
        TEXT("game_framework.configure_game_rules"),
        TEXT("game_framework.configure_round_system"),
        TEXT("game_framework.configure_scoring_system"),
        TEXT("game_framework.configure_spawn_system"),
        TEXT("game_framework.configure_spectating"),
        TEXT("game_framework.configure_team_system"),
        TEXT("game_framework.set_respawn_rules"),
        TEXT("gas.add_attribute"),
        TEXT("gas.create_execution_calculation"),
        TEXT("interaction.add_interaction_events"),
        TEXT("interaction.configure_chest_properties"),
        TEXT("interaction.configure_door_properties"),
        TEXT("interaction.configure_interaction_trace"),
        TEXT("interaction.configure_interaction_widget"),
        TEXT("interaction.configure_switch_properties"),
        TEXT("networking.add_network_prediction_data"),
        TEXT("networking.configure_rpc_validation"),
        TEXT("networking.create_rpc_function"),
        TEXT("networking.set_autonomous_proxy"),
        TEXT("networking.set_property_replicated"),
        TEXT("networking.set_replicated_using"),
        TEXT("networking.set_replication_condition"),
        TEXT("networking.set_rpc_reliability"),
        TEXT("vehicle.create_wheel_asset"),
        TEXT("vehicle.remove_wheel_setup"),
        TEXT("vehicle.set_suspension"),
        TEXT("vehicle.set_wheel_asset_property"),
        TEXT("vehicle.set_wheel_setup"),
        TEXT("widget.bind_event"),
    };

    const TArray<FString>& BuildTickUnsafeMethodList()
    {
        static const TArray<FString> Methods = []()
        {
            TArray<FString> Built;
            Built.Reserve(static_cast<int32>(UE_ARRAY_COUNT(GTickUnsafeMethodNames)));
            for (const TCHAR* Name : GTickUnsafeMethodNames)
            {
                Built.Add(FString(Name));
            }
            Built.Sort();
            return Built;
        }();
        return Methods;
    }

    const TSet<FString>& TickUnsafeMethodSet()
    {
        static const TSet<FString> Methods(BuildTickUnsafeMethodList());
        return Methods;
    }
}

bool IsTickUnsafeMethod(const FString& Method)
{
    const FString& ExtraForTests = Detail::ExtraTickUnsafeMethodForTests();
    if (!ExtraForTests.IsEmpty() && ExtraForTests.Equals(Method, ESearchCase::CaseSensitive))
    {
        return true;
    }
    return TickUnsafeMethodSet().Contains(Method);
}

const TArray<FString>& GetTickUnsafeMethods()
{
    return BuildTickUnsafeMethodList();
}
}
