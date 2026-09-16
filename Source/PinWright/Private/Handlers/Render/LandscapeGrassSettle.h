// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"

class FJsonObject;
class UHierarchicalInstancedStaticMeshComponent;
class UWorld;

// ---- landscape grass is built around a CAMERA, not around a view matrix ----
//
// A capture applies its `location` / `rotation` to the editor viewport client and takes the
// pose back off it on scope exit, all inside one RPC. Landscape grass does not live on that
// path at all: `ULandscapeSubsystem::Tick` gathers camera locations from
// `World->ViewLocationsRenderedLastFrame` (UE 5.8
// Runtime/Landscape/Private/LandscapeSubsystem.cpp:733-739, including the engine's own comment
// about the editor often leaving it with no cameras, and the `OldCameras` fallback) and calls
// `Proxy->UpdateGrass(*Cameras, ...)` at :900 -- from the WORLD tick, which no capture runs.
// The capture's settle loop pumps Slate and the renderer and never ticks the world, and by the
// time the editor does tick, the entry pose is already back on the client.
//
// So a pose-driven capture photographed grass built around wherever the persistent viewport
// camera happened to be. Measured on a live editor, one map, one variable
// (B-capture-open-level-pose-params-photograph-stale-grass): the same pose captured twice
// through `render.capture_open_level` gave meanLuminance 0.6443 at 844 KB with no grass in
// frame, while `editor.set_camera` to that pose (which leaves the camera there long enough for
// a world tick to pick it up) followed by the identical capture gave 0.4796 at 1558 KB with a
// full carpet. Grass darkens the frame, so the lower mean is the one with more of it. Nothing
// in either response distinguished the two: both success, both non-blank, both settled.
//
// The fix drives the build explicitly instead of hoping for a tick.
// `ULandscapeSubsystem::RegenerateGrass(bFlushGrass, bForceSync, InOptionalCameraLocations)`
// (LandscapeSubsystem.h:120; the camera-locations branch is LandscapeSubsystem.cpp:633-635)
// takes the camera list as a parameter, so the capture's own eye position can be handed to it
// directly -- no viewport camera is moved for the grass, and none is left moved afterwards.
// With `bForceSync = true` the async builders are waited on inside the call
// (`ProcessAsyncGrassInstanceTasks` -> `FAsyncTask::EnsureCompletion`, LandscapeGrass.cpp:3352
// -> :3361), so the instances exist before the pixels are read rather than a few frames later.
namespace PinWrightCaptureGrass
{
    // What the landscape-grass build did for ONE capture pose. Every field is read off the
    // landscape proxies after the build, never assumed from the fact that the call was made.
    //
    // The pair that carries the whole point: `bSettled` with `InstancesAfter`. A caller looking
    // at bare ground needs to tell "this ground really has no grass on it" (settled, zero
    // instances) from "the grass for this pose had not finished building when the shutter fired"
    // (not settled) -- those are opposite conclusions about the same picture, and before this
    // block the response had no field that could separate them.
    struct FGrassBuildReport
    {
        // The world had a landscape subsystem and the walk below ran. False on a headless host
        // and on any path that never reached the grass step; separates "measured and there are
        // no landscapes" from "never looked", which `LandscapeProxies == 0` alone cannot.
        bool bMeasured = false;
        // Landscape proxies (ALandscape + ALandscapeStreamingProxy) in the captured world.
        // Zero is the ordinary answer for an asset-preview scene and for a level with no terrain.
        int32 LandscapeProxies = 0;
        // RegenerateGrass ran with CameraLocation as its one camera. False with LandscapeProxies
        // above zero means the grass in this frame was built around some OTHER camera.
        bool bBuiltForPose = false;
        // The eye position the grass was built around -- the capture's own MEASURED pose, not
        // the requested one, because an orbit-mode viewport can put the eye elsewhere.
        FVector CameraLocation = FVector::ZeroVector;
        // Grass components in the landscape foliage caches before and after the build, and the
        // instances they hold afterwards. `InstancesAfter` is the number that answers "is there
        // grass in this frame at all".
        int32 ComponentsBefore = 0;
        int32 ComponentsAfter = 0;
        int64 InstancesAfter = 0;
        // False when at least one cache entry could not be read at all, which makes `InstancesAfter`
        // a floor rather than a total. `instances` is then OMITTED from the response instead of
        // published: an incomplete sum emitted as a number is indistinguishable from bare ground,
        // and a count that cannot tell those apart is exactly the defect
        // B-capture-grass-instances-always-zero was filed for.
        bool bInstancesMeasured = false;
        // Cache entries whose grass component had already been destroyed when the count ran, so
        // whatever they were holding is unknowable now. The engine treats this state as real --
        // `UpdateGrass`'s own trim loop lists `!Used` (an expired Foliage pointer) as a reason to
        // drop an entry (LandscapeGrass.cpp:3286-3298), so the entry outlives the component until
        // the next update. Counted rather than silently added to the total as zero.
        int32 UnreadableComponents = 0;
        // Cache entries still flagged Pending, and async grass tasks still outstanding, AFTER
        // the forced build. Both should be zero after a synchronous regenerate; non-zero is the
        // measurement that says these pixels were taken over an unfinished build.
        int32 PendingComponents = 0;
        int32 PendingTasks = 0;
        // PendingComponents == 0 && PendingTasks == 0 after a build that ran. Never true on the
        // strength of the call having been made.
        bool bSettled = false;
        // `ALandscapeProxy::UpdateGrass` returns immediately when `GUndo != nullptr`
        // (LandscapeGrass.cpp:2854-2857), so a capture taken inside an open transaction cannot
        // build grass however it is asked. Reported rather than left to look like empty terrain.
        bool bBlockedByTransaction = false;
        // What the synchronous build cost, in milliseconds. Published because it is a real cost
        // this capture now pays on every level frame that has terrain in it.
        double BuildMs = 0.0;

        // ---- can any of the grass that exists be in THIS frame? ----
        //
        // The fields above answer "was grass made for this pose". They do not answer "will the
        // renderer draw it", and an ORTHOGRAPHIC capture is where those two come apart: a build
        // can report `builtForPose: true`, `settled: true` and millions of instances over a frame
        // that renders as bare ground, because grass instances are culled against
        // `InstanceEndCullDistance` (`LandscapeGrass.cpp:3220`, seeded from the grass variety's
        // `EndCullDistance`, default 10000 cm) measured from `FSceneView::CullingOrigin`
        // (`SceneView.cpp:844`) rather than from the camera. That is the whole of
        // B-ortho-capture-renders-no-landscape-grass: a plausible, well-formed, settled picture
        // of ground that is actually covered, with no field in the response that hinted otherwise.
        //
        // Filled only when a caller supplied the frame's culling origin. `bReachMeasured` false
        // means nobody asked, not that nothing is in range.
        bool bReachMeasured = false;
        // False when the culling origin handed in was a fallback (the capture could not build an
        // FSceneView) rather than a reading off the view the pixels are drawn from. The reach
        // numbers below are then about an assumed point, and say so.
        bool bCullingOriginMeasured = false;
        FVector CullingOrigin = FVector::ZeroVector;
        // Nearest approach from the culling origin to any grass component's bounds, in cm. The
        // number to compare against GrassCullDistance.
        double NearestComponentDistance = 0.0;
        // Smallest effective end-cull distance across the grass components that have one, in cm:
        // the component's own `InstanceEndCullDistance` times the r.ViewDistanceScale the renderer
        // will actually use (`HierarchicalInstancedStaticMesh.cpp:1672` folds the same product into
        // FinalCull). Zero when every component has culling disabled.
        double GrassCullDistance = 0.0;
        // Components, and the instances they hold, whose nearest bounds point is inside their own
        // effective end-cull distance of the culling origin. A component with culling disabled
        // (end-cull 0, which UpdateGrass sets when it is given no cameras) always counts as in
        // range. InstancesInReach == 0 while InstancesAfter > 0 is the empty-forest case.
        int32 ComponentsInReach = 0;
        int64 InstancesInReach = 0;
    };

    // Build the landscape grass of `World` around `CameraLocation` and report what happened.
    // Game thread only. A null world, or one with no landscape subsystem, returns an unmeasured
    // report; a world with no landscape proxies returns a measured, settled, empty one without
    // touching the subsystem.
    //
    // Does NOT move any viewport camera: the location is passed to RegenerateGrass as data.
    // Does NOT restore the previous grass -- grass components are RF_Transient
    // (LandscapeGrass.cpp:3159) and the editor's own amortised update rebuilds around the user's
    // camera on the next world tick, so a second forced build on the way out would double the
    // cost of every capture to undo something the engine undoes for free.
    FGrassBuildReport SettleGrassForCapturePose(UWorld* World, const FVector& CameraLocation);

    // ---- why an ORTHOGRAPHIC capture needs the forced build above more than a perspective one ----
    //
    // A perspective editor viewport eventually gets its grass built by the editor's own tick, so
    // before the forced build a perspective capture at the persistent camera's pose still showed
    // grass. An orthographic one never does, at any pose, because the editor excludes it from the
    // grass camera set by projection type. `UEditorEngine::UpdateSingleViewportClient` gates BOTH
    // camera feeds on `IsPerspective()`: the streaming-manager registration
    // (UE 5.8 Editor/UnrealEd/Private/EditorEngine.cpp:2613-2618) and the
    // `World->ViewLocationsRenderedLastFrame.Add(...)` (:2628-2634). The only other writer of that
    // array, `AddStreamingViewInfo` (Runtime/Engine/Private/UnrealClient.cpp:1771-1787), is called
    // solely from `UGameViewportClient::Draw` (GameViewportClient.cpp:1913), which no editor
    // viewport reaches. `ULandscapeSubsystem::Tick` feeds `UpdateGrass` from exactly those two
    // sources (LandscapeSubsystem.cpp:729-757, :891-901) and skips it entirely when neither
    // yields a camera -- and the default `grass.UseStreamingManagerForCameras` is 1
    // (LandscapeSubsystem.cpp:65-69), which is the branch with NO `OldCameras` fallback.
    //
    // So an orthographic view can never be a grass camera. Whatever grass an ortho frame contains
    // is a leftover of the last PERSPECTIVE view, and over ground no perspective view has visited
    // the frame is bare. Measured: an ortho capture at matched world coverage
    // (orthoWidth 6928 against fov 60 at Z 6000) returned meanLuminance 0.5161 with no grass where
    // the perspective frame returned 0.4796 with a full carpet -- grass darkens a frame, so the
    // higher number is the one with less of it. Distance culling was ruled out by measurement in
    // that session, not by argument (`cullingOriginPushback` 0 under `viewMode: "unlit"`, a derived
    // r.ViewDistanceScale of 2.29 live, and the grass still absent).
    // See B-ortho-capture-renders-no-landscape-grass.
    //
    // Measure how much of the grass that now exists can actually reach this frame, and fold the
    // answer into `InOutReport`. Game thread only. `CullingOrigin` must be the point the renderer
    // culls from -- `FSceneView::CullingOrigin`, which is NOT the camera for a lit editor
    // orthographic view -- and `bInCullingOriginMeasured` says whether it was read off a view or
    // fallen back to. `ViewDistanceScale` is the scale the renderer will use, read out of its own
    // scalability cache rather than out of the cvar. A report that was never measured, or a world
    // with no grass components, is left untouched.
    void MeasureGrassFrameReach(UWorld* World, const FVector& CullingOrigin,
        bool bInCullingOriginMeasured, float ViewDistanceScale, FGrassBuildReport& InOutReport);

    // ---- how many instances a grass component is actually holding ----
    //
    // NOT `GetInstanceCount()`. That accessor is `return PerInstanceSMData.Num();` (UE 5.8
    // Runtime/Engine/Private/InstancedStaticMesh.cpp:4844-4847, declared non-virtual at
    // Classes/Components/InstancedStaticMeshComponent.h:435 and not overridden by HISM), and
    // landscape grass never writes that array: `UGrassInstancedStaticMeshComponent::
    // AcceptPrebuiltTree` -- the ONLY way a grass component ever receives instances
    // (LandscapeGrass.cpp:3387) -- opens with `check(!PerInstanceSMData.Num())`
    // (Runtime/Foliage/Private/InstancedGrass.cpp:52) and hands the built buffer straight to the
    // render path. So the old accessor was a structural zero over arbitrarily much drawn grass:
    // 149 components carrying a dense carpet summed to 0 (B-capture-grass-instances-always-zero).
    //
    // The quantity grass DOES populate is `NumBuiltRenderInstances`, published through the public
    // virtual `UInstancedStaticMeshComponent::GetNumRenderInstances()`
    // ("Number of instances in the render-side instance buffer",
    // InstancedStaticMeshComponent.h:639) which the grass component overrides to return it
    // (Runtime/Foliage/Public/GrassInstancedStaticMeshComponent.h:22). That override exists from
    // UE 5.4 on; 5.3's grass component declares no such override, so there the virtual resolves to
    // the base spelling and reads zero. 5.3 therefore reads the public NumBuiltRenderInstances
    // member the base AcceptPrebuiltTree writes (HierarchicalInstancedStaticMesh.cpp:2889), which
    // is the same quantity the later override returns.
    //
    // The fallback is load-bearing rather than defensive: HISM's own override of that virtual is
    // `SortedInstances.Num()` (HierarchicalInstancedStaticMeshComponent.h:342), which is zero on a
    // hand-authored component whose cluster tree has not been built yet while `PerInstanceSMData`
    // already holds real instances. Neither number alone is right for both kinds of component.
    int32 GrassComponentInstanceCount(const UHierarchicalInstancedStaticMeshComponent& Foliage);

    // The `viewport.grass` response block. Pure function of the report, so the honesty contract
    // (when a warning appears, and what it says) is assertable without a level or an RHI.
    TSharedPtr<FJsonObject> MakeGrassBuildInfoObject(const FGrassBuildReport& Report);
}
