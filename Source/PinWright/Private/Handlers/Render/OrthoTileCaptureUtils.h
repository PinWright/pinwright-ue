// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Handlers/Render/LandscapeGrassSettle.h" // FGrassBuildReport, held by value below
#include "Handlers/Render/PreviewViewportCaptureUtils.h" // FExposurePin, FViewModePin
#include "Handlers/Render/TileGridUtils.h"
// FEngineShowFlags is returned and taken BY VALUE below (a copy is the only way to compare a
// component's flags against the base it started from), so the complete type is required here.
#include "ShowFlags.h"
#include "UObject/StrongObjectPtr.h"

class UWorld;
class USceneCaptureComponent2D;
class UTextureRenderTarget2D;
class FJsonObject;

// The renderer half of render.capture_ortho_tiles: an offscreen ORTHOGRAPHIC scene capture of the
// editor world, one frame per tile, at a resolution fixed for the whole burst.
//
// WHY A SCENE CAPTURE AND NOT THE LEVEL VIEWPORT. Three measured reasons, none of them taste:
//
//  1. ARBITRARY POSE. FEditorViewportClient::CalcSceneView derives an orthographic view matrix
//     from ELevelViewportType, so the viewport path can only render the six cardinal poses and
//     quantises anything else (which is why PreviewViewportCaptureUtils has to report an
//     EffectiveRotation at all). A scene capture uses the component's own transform verbatim.
//
//  2. IT FIXES DISTANCE CULLING AT CAUSE. USceneCaptureComponent::bUpdateOrthoPlanes defaults
//     false, so SceneCaptureViewInfo::UpdateOrthoPlanes never runs
//     (Renderer/Private/SceneCaptureRendering.cpp:887) and the ~2.1e6 cm culling-origin pushback
//     that empties a wide orthographic viewport frame never happens. Measured at identical
//     framing: static meshes 0/16 -> 16/16 visible, HISM instances 0/64 -> 64/64. The whole
//     FViewDistanceSurvey / r.ViewDistanceScale apparatus PreviewViewportCaptureUtils needs is
//     unnecessary on this path, and this capture therefore never writes a global cvar.
//
//  3. IT WORKS HEADLESS. There is no Slate viewport, no back buffer and no resize, so the
//     documented FViewport::GetHitProxy assertion (`ProxyMap.Num() == TestSizeX * TestSizeY`,
//     which cost 66 actors of unsaved level state on 2026-08-13 when a capture verb varied its
//     resolution mid-session) is unreachable from here: nothing in this file touches a viewport.
//     The per-burst resolution is fixed anyway, structurally - see FOrthoTileCapture.
//
// WHAT IT DELIBERATELY DOES NOT USE. USceneCaptureComponent2D::bEnableOrthographicTiling is not
// the tiling this verb wants and cannot be used regardless: it requires CaptureSource to be a
// SceneColor source, and with a FinalColor source "tiling will be ignored and a Warning message
// will be logged" (SceneCaptureComponent2D.h:113-124). It also renders ONE image across n frames
// to beat the maximum render-target size, whereas this verb needs n separate georeferenced
// images. Tiles are produced by translating the camera per tile, from
// PinWrightTileGrid::ComputeTileCapturePlan.
//
// ---- THE NEAR PLANE IS ZERO, AND THAT IS THE SAME KNOB AS (2) ----
//
// BuildOrthoMatrix hardcodes `const float NearPlane = 0` (SceneCaptureRendering.cpp:546-560), so
// everything behind the camera PLANE is clipped - not merely unlit, gone. A camera at z=3000 over
// a map whose cliffs reach z=8000 renders the cliffs sliced off. The camera must therefore be
// pushed beyond the scene bounds along its view direction, and the depth coordinate it ended up
// at is reported rather than assumed. Turning bUpdateOrthoPlanes on would move the near plane for
// us and reintroduce the culling of (2): the two behaviours are the same switch, and this file
// chooses coverage.
namespace PinWrightOrthoTiles
{
    // ---------------------------------------------------------------------------------------
    // Burst ceilings
    // ---------------------------------------------------------------------------------------
    //
    // The hazard is not duration, it is an operation nobody can observe or cancel. This verb is
    // synchronous by construction: Ctx.StartJob invokes its bind delegate on the caller's own
    // stack (rpc-design.md section 9), so a ticket buys bookkeeping and not deferral, and
    // CaptureScene() plus FlushRenderingCommands cannot yield mid-tile anyway. A burst that
    // outlives the transport's 120 s response timeout is exactly the shape to refuse: the client
    // gives up, the handler keeps rendering, and the files land with nobody watching.
    //
    // Two ceilings, because tile COUNT and total PIXELS fail differently. Measured end to end on
    // this project: 64 tiles is ~7 s at 1024 px, ~25 s at 2048 px, ~2 min at 4096 px - i.e. cost
    // tracks pixels, not tiles, because 91-96% of per-tile time is PNG encode plus readback and
    // the renderer barely moves it.

    // Hard tile-count ceiling for one call. 64 is the largest burst measured end to end above.
    constexpr int32 MaxTilesPerCall = 64;

    // Total output pixels for one call: 256 megapixels (e.g. 64 tiles at 2048 px, or 16 at
    // 4096 px). Derived from the measurement, not chosen: 64 x 4096 px = 1074 MP took ~120 s, so
    // ~0.112 s/MP, and 256 MP predicts ~29 s - about a quarter of the 120 s response timeout,
    // leaving room for a slower host before a burst starts outliving its own caller.
    constexpr int64 MaxTotalPixelsPerCall = 268435456;

    // The throughput the pixel ceiling is derived from, published so a caller can see the
    // arithmetic behind a refusal and so a re-measurement has something to disagree with.
    //
    // It is deliberately the WORST case seen, not the typical one. Re-measured 2026-08-18 through
    // this verb on the welcome map in a headless editor: 16 tiles at 1024 px (16.8 MP) took
    // 0.59 s and 4 tiles at 2048 px (16.8 MP) took 0.48 s - about 0.032 s/MP, ~3.5x faster than
    // this constant. Erring slow means the ceiling refuses a burst that would in fact have fit,
    // which costs a caller one extra call; erring fast means a burst outlives its own response
    // timeout, which costs a caller a render nobody is watching.
    //
    // Those two runs are also the evidence for putting the ceiling on PIXELS: identical pixel
    // counts, a 4x difference in tile count, and the same wall time to within 20%. Encode plus
    // readback was 83% of a steady-state 1024 px tile and 88% of a 2048 px one.
    constexpr double MeasuredSecondsPerMegapixel = 0.112;

    // Per-tile pixel size bounds. The lower bound keeps a tile large enough for the georeference
    // to mean anything; the upper is the engine's own render-target maximum
    // (UTextureRenderTarget2D::PostEditChangeProperty clamps to 16384) halved, since a square
    // tile at 8192 px is already 67 MP and only three of them fit the pixel ceiling.
    constexpr int32 MinTilePixels = 16;
    constexpr int32 MaxTilePixels = 8192;

    // How far beyond the surveyed scene bounds a derived camera is placed, in centimetres. Only
    // the SIGN and the fact that it clears the bounds matter to the near-plane-0 clip; the
    // magnitude is a default the caller can override.
    constexpr double DefaultCameraClearanceCm = 10000.0;

    // ---------------------------------------------------------------------------------------
    // Scene bounds along the depth axis
    // ---------------------------------------------------------------------------------------

    // What a camera placement needs to know about the world it is about to render.
    //
    // bMeasured defaults FALSE and every number defaults 0, so a survey that never ran cannot be
    // read as "the world is empty at the origin" - the difference decides whether the camera
    // placement is a measurement or a guess, and the verb refuses to guess.
    struct FDepthBoundsSurvey
    {
        bool bMeasured = false;
        // Primitives that contributed an extent to BoundsMin/BoundsMax.
        int32 NumPrimitives = 0;
        // Primitives skipped because their bounds reach world scale - see the function comment.
        // Reported rather than silently dropped: narrowing a measurement without saying so is how
        // a survey stops meaning what its name says.
        int32 NumUnbounded = 0;
        FVector BoundsMin = FVector::ZeroVector;
        FVector BoundsMax = FVector::ZeroVector;
        // BoundsMin/BoundsMax projected onto the capture's depth axis.
        double DepthMin = 0.0;
        double DepthMax = 0.0;
    };

    // A bounds half-extent at or above this is not an extent, it is the engine's way of saying the
    // primitive has none: sky atmosphere, exponential height fog, unbound post-process volumes and
    // infinite-extent lights all report `UE_LARGE_HALF_WORLD_MAX` (4398046511104 cm - the entire
    // representable world, EngineDefines.h:41-42). Half of it is the cutoff, so a value merely
    // *near* world scale is caught too.
    //
    // MEASURED on this project's welcome map: of 346 registered visible primitives, 4 report
    // exactly UE_LARGE_HALF_WORLD_MAX, and an unfiltered survey therefore put the derived camera
    // plane at 4398046521104 cm - 4.4e12 cm above a level whose real content spans z in
    // [-1035, 1194]. Filtered, the same survey returns those real bounds and the camera lands at
    // 11194 cm. (What that absurd depth did to the pixels was never isolated: the map is close to
    // black from directly above in a headless editor on BOTH renderers, so the frames looked the
    // same either way. The camera placement is wrong regardless of whether it was also the reason
    // a frame looked wrong - do not read a rendering claim into this constant.)
    //
    // The sibling FViewDistanceSurvey hit the same class of value - a 7.6e12 cm bounding sphere on
    // the other map in this project - and records the same lesson: one primitive with no real
    // extent must not set a number derived on behalf of all of them.
    constexpr double UnboundedPrimitiveExtentCm = 2199023255552.0;

    // One pass over the REGISTERED, VISIBLE primitive components of World, accumulating the world
    // AABB of their bounding boxes. Mirrors SurveyViewDistances' iteration exactly (TObjectIterator
    // filtered by GetWorld(), skipping templates and invalid objects) so the two surveys cannot
    // disagree about which primitives are in the scene, and additionally drops the world-scale
    // bounds described above, counting them into FDepthBoundsSurvey::NumUnbounded.
    //
    // Uses the bounding BOX rather than the bounding sphere: the sphere radius over-reaches on a
    // long thin actor by up to its own half-length, and this number decides where a camera goes.
    FDepthBoundsSurvey SurveyWorldDepthBounds(UWorld* World, PinWrightTileGrid::EWorldAxis DepthAxis);

    // ---------------------------------------------------------------------------------------
    // Exposure
    // ---------------------------------------------------------------------------------------
    //
    // A scene capture cannot use the viewport pin. PreviewViewportCaptureUtils writes
    // FEditorViewportClient::ExposureSettings, which the renderer reads as
    // View.Family->ExposureSettings.bFixed - and a scene capture builds its OWN view family, in
    // which that struct is default-constructed and therefore never fixed. The pin has to travel
    // on the component's post-process settings instead.
    //
    // The route is AEM_Manual plus the physical-camera formula, because that is the only exposure
    // path whose value is fully determined by fields this code sets:
    //
    //     EV100 = log2(DepthOfFieldFstop^2 * CameraShutterSpeed * 100 / max(1, CameraISO))
    //     (Renderer/Private/PostProcess/PostProcessEyeAdaptation.cpp:524-530)
    //
    // Every term is overridden here, so the level's post-process volumes cannot move it. The
    // alternative - AutoExposureMinBrightness == AutoExposureMaxBrightness - is read as EV100
    // ONLY when the project's bExtendDefaultLuminanceRangeInAutoExposureSettings is on
    // (PostProcessEyeAdaptation.cpp:658-664), so it would make the meaning of `ev100` a property
    // of the host project's settings.
    //
    // THE PINNED VALUE IS NOT COMPARABLE WITH A VIEWPORT CAPTURE AT THE SAME EV100, and that is
    // an engine fact rather than a shortcoming here: the fixed-exposure branch multiplies by
    // kMiddleGrey (0.18) while the manual branch multiplies by 1.0 (:625), ~2.47 stops apart. It
    // does not matter, because captures are never compared across renderers anyway - viewport and
    // scene capture differ by 5.76% mean absolute error against a 1.30% viewport self-noise floor
    // - which is why every response and manifest records the renderer that produced it.
    struct FExposurePlan
    {
        // Echo of what the caller asked for.
        PinWrightRenderCapture::EExposureRequestMode Mode =
            PinWrightRenderCapture::EExposureRequestMode::Unset;
        float Ev100Requested = 0.0f;

        // The three physical-camera fields written onto the component, published so the pin is
        // auditable from the response alone.
        float Fstop = 0.0f;
        float ShutterSpeed = 0.0f;
        float Iso = 0.0f;

        // MEASURED: the EV100 recomputed from the component's settings AFTER the capture, using
        // the engine's own formula. Not an echo of Ev100Requested - a mistyped override or a
        // clamp anywhere in the chain shows up here as a different number.
        float Ev100Applied = 0.0f;
        bool bEv100Measured = false;

        // The conjunction of "a pin was requested", "the settings read back at the requested
        // EV100" and "this frame is one the renderer applies exposure to at all". False with
        // Mode == Fixed is the one state that makes the tiles non-comparable, and the verb warns
        // rather than reporting a pin it did not achieve.
        bool bPinned = false;
        // Why a requested pin does not govern the pixels. Empty when it does, or when none was
        // requested. Never a bare "failed".
        FString BlockedReason;
    };

    // Solve the physical-camera fields for an absolute EV100.
    //
    // Fstop is fixed at the engine's own default of 4.0 and ISO at 100, so the only free variable
    // is the shutter speed: EV100 = 4 + log2(ShutterSpeed). Fstop is OVERRIDDEN rather than left
    // to the blend chain even though 4.0 is the default, because an unoverridden term is one a
    // post-process volume in the level can move - and a term the caller cannot see moving is the
    // whole reason this is not a heuristic. Every EV100 in [-30, 30] lands on an exact power of
    // two, so the round trip through the engine's log2 is exact.
    void SolvePhysicalCameraForEv100(float Ev100, float& OutFstop, float& OutShutterSpeed, float& OutIso);

    // The engine's own EV100 formula, one copy, used both to solve and to verify.
    float PhysicalCameraEv100(float Fstop, float ShutterSpeed, float Iso);

    // Why a requested exposure pin does not govern a frame whose Lighting / PostProcessing show
    // flags are off, phrased against WHO turned them off.
    //
    // One definition for both readings, because they are different facts and a caller acts
    // differently on them: an unrequested lighting-off frame is a FAULT to chase (a profile, a
    // scene setting, a renderer state nobody asked for), while a `viewMode` that clears Lighting is
    // the request being honoured. Reporting the second in the first's words is how a deliberate
    // diagnostic capture reads as a broken one. Both still report `pinned: false`, because the pin
    // genuinely does not govern the pixels either way.
    //
    // RequestedViewModeKey empty means no view mode was asked for.
    FString DescribeExposureBlockedByShowFlags(bool bLighting, bool bPostProcessing,
        const FString& RequestedViewModeKey);

    // ---------------------------------------------------------------------------------------
    // The `viewMode` override on a SCENE CAPTURE, which is not the viewport override
    // ---------------------------------------------------------------------------------------
    //
    // WHAT IS DIFFERENT HERE. FScopedViewModeOverride takes an FEditorViewportClient&, writes both
    // of its view-mode slots and restores them. This path has no viewport and no slot: the mode
    // lives ONLY in FEngineShowFlags, which is a public writable member of the capture component,
    // and the component is created per call and discarded (see FOrthoTileCapture). So there is
    // nothing to restore and NO SCOPE GUARD IS WRITTEN - a guard that put back a value nobody else
    // can observe would be theatre, and the response omits `restored` rather than faking it.
    //
    // WHY THE FLAGS ARE ENOUGH TO MATTER. The component's flags become the view family's flags
    // verbatim: FSceneViewFamily::ConstructionValues(RenderTarget, Scene,
    // SceneCaptureComponent->ShowFlags) at UE 5.8 Renderer/Private/SceneCaptureRendering.cpp:905-909.
    // Whatever this writes is what the renderer reads.
    //
    // WHY BOTH ENGINE FUNCTIONS ARE CALLED, and this is the correction that matters most:
    // ApplyViewMode ALONE DOES NOT CLEAR `Lighting` FOR `unlit`. ApplyViewMode
    // (UE 5.8 Runtime/Engine/Private/ShowFlags.cpp:292-446) never calls SetLighting at all - the
    // only thing it does for VMI_Unlit is SetPostProcessing(false). Lighting, Atmosphere and Fog
    // are cleared by EngineShowFlagOverride (:530-585), which is what FEditorViewportClient::Draw
    // runs every frame after ApplyViewMode. Apply only the first and `viewMode: "unlit"` returns a
    // LIT frame labelled unlit - exactly the silent-wrong-picture failure this parameter exists to
    // prevent. Both are therefore run, in the engine's own order, the way
    // UGameViewportClient::Draw pairs them on a non-editor path
    // (Runtime/Engine/Private/GameViewportClient.cpp:1619-1620).
    //
    // ESFIM_Game rather than ESFIM_Editor is passed because the component's flags are a game set
    // (SceneCaptureComponent.cpp:169). The init mode gates exactly one line of
    // EngineShowFlagOverride - SetAudioRadius(false) - and nothing else in that function reads it.

    // What was asked for and what was MEASURED off the flags afterwards. Nothing here is echoed
    // except RequestedKey itself; every other field is read back from the show flags the renderer
    // will consume.
    struct FViewModePlan
    {
        bool bRequested = false;
        FString RequestedKey;
        int32 RequestedValue = 0;

        // The mode the ENGINE's own FindViewMode (ShowFlags.cpp:800) derives back out of the
        // flags. An independent second opinion rather than a restatement: for `unlit` it lands on
        // VMI_Unlit only because the Lighting flag really is off (:978 is literally
        // `EngineShowFlags.Lighting ? VMI_Lit : VMI_Unlit`).
        //
        // It is NOT injective and must not be compared to RequestedKey as a pass/fail: FindViewMode
        // has no branch for ActorColoration, Clay-before-Zebra ordering wins ties, and several
        // modes that clear Lighting therefore derive back as "Unlit". Reported for reading; the
        // pass/fail fact is bApplied.
        //
        // bDerivedMeasured guards the pair because VMI_BrushWireframe is the ZERO value of
        // EViewModeIndex: a default-constructed plan whose flags were never built would otherwise
        // publish derivedValue 0 and read as a wireframe capture. A capture that failed before it
        // allocated a component says "unmeasured" instead.
        bool bDerivedMeasured = false;
        FString DerivedKey;
        int32 DerivedValue = 0;

        // EngineShowFlags names ApplyViewMode writes differently for this mode than for VMI_Lit,
        // and the subset that did NOT read back off the component afterwards. The second list is
        // the §4.3 "which families actually take on a game-set base" question answered by
        // measurement instead of by a table in a comment.
        TArray<FString> DistinguishingShowFlags;
        TArray<FString> ShowFlagMismatches;

        // Every distinguishing flag read back, so the mode reached the flags the renderer reads.
        // This is the fact a caller can trust; a response that only said the call succeeded is how
        // camera.orbit_shots shipped an inert `viewMode`.
        bool bApplied = false;
        // The key, and only when bApplied. Empty otherwise - never the requested key on a mode
        // whose flags did not take.
        FString AppliedKey;
    };

    // The show flags a fresh FOrthoTileCapture component carries with NO view mode requested:
    // FEngineShowFlags(ESFIM_Game) - the component's own base (SceneCaptureComponent.cpp:169) -
    // plus this file's deliberate deviations. Exists so "the omitted parameter writes nothing" can
    // be diffed against a reference instead of against the same code that produced it.
    FEngineShowFlags MakeBaseCaptureShowFlags();

    // Write a resolved pin into a capture component's flags and MEASURE the result. A pin with
    // bRequested false writes nothing at all and leaves the flags byte-identical, which is what
    // keeps existing pixels where they are.
    FViewModePlan ApplyViewModeToCaptureShowFlags(const PinWrightRenderCapture::FViewModePin& Pin,
        FEngineShowFlags& ShowFlags);

    // Can this mode's PICTURE be produced by a USceneCaptureComponent2D at all? False fills a
    // registered error code and a message naming the mechanism that is missing.
    //
    // This is a second gate BELOW PinWrightViewModes::Resolve, not a replacement for it. Resolve
    // answers "is this a renderable mode"; this answers "can THIS renderer draw it". The two
    // refusals carry different codes on purpose - a caller retries a Resolve refusal with a
    // different spelling and a reach refusal with a different verb.
    bool IsViewModeReachableOnSceneCapture(EViewModeIndex ViewMode, FString& OutErrorCode,
        FString& OutErrorMessage);

    // The `viewMode` sub-block of the response's / manifest's `showFlags` object.
    TSharedPtr<FJsonObject> MakeViewModeInfoObject(const FViewModePlan& Plan);

    // ---------------------------------------------------------------------------------------
    // The capture
    // ---------------------------------------------------------------------------------------

    // Per-tile cost, in milliseconds, split where the cost actually is.
    struct FTileTiming
    {
        double RenderMs = 0.0;   // CaptureScene() + FlushRenderingCommands
        double ReadbackMs = 0.0; // render target -> CPU FColor
        double TotalMs = 0.0;    // the two above; encode/write is timed by the caller
    };

    // Where the camera ACTUALLY was when the tile was drawn, read back off the component after the
    // move rather than echoed from the plan.
    //
    // This exists because the alternative failed silently and cost most of this verb's bring-up.
    // FScene::UpdateSceneCaptureContents reads the pose from
    // CaptureComponent->GetComponentToWorld() (SceneCaptureRendering.cpp:1180), so a move that
    // does not land renders every tile of the burst from the same wrong place - and every tile
    // still writes a file, still reports timings, and still says nothing. Sixteen tiles of one
    // frame is not distinguishable from a dark level by any field the response otherwise has.
    struct FTileCameraPose
    {
        FVector Location = FVector::ZeroVector;
        FRotator Rotation = FRotator::ZeroRotator;
        // The readback matched what was requested. False is a hard failure, not a warning: every
        // world coordinate in the georeference is a lie about pixels drawn from somewhere else.
        bool bMatchesRequest = false;
    };

    // Owns the transient capture component and the render target for the lifetime of ONE request.
    //
    // Construct once per burst and reuse for every tile. Two properties are structural rather than
    // conventional:
    //
    //  * NOTHING REACHES THE LEVEL. The component is created ownerless, RF_Transient, outered to
    //    the transient package, and registered with RegisterComponentWithWorld - the mechanism
    //    FPreviewScene::AddComponent uses. No actor is spawned, so the level's actor array, the
    //    outliner, World Partition and the package dirty flag are all untouched. This is the same
    //    lifetime FSceneCaptureProbe uses for render.detect_z_fighting; that probe is hardcoded to
    //    Perspective, which is the only part not shared.
    //
    //  * THE RESOLUTION CANNOT VARY WITHIN A BURST. Width and height are constructor arguments and
    //    the render target is allocated once from them; CaptureTile takes no size at all. A grid
    //    has one tile pixel size by construction (PinWrightTileGrid::FTileGrid), so a partial edge
    //    tile at a different size is unrepresentable upstream too.
    class FOrthoTileCapture
    {
    public:
        // Allocates the component and the render target. Check IsValid() before use; the failure
        // reason is available from GetInitError().
        //
        // A default-constructed InViewMode writes NOTHING to the show flags, so an omitted
        // `viewMode` leaves this capture byte-for-byte what it was before the parameter existed.
        FOrthoTileCapture(UWorld* InWorld, int32 InPixelWidth, int32 InPixelHeight,
            const PinWrightRenderCapture::FExposurePin& InExposure,
            const PinWrightRenderCapture::FViewModePin& InViewMode);
        ~FOrthoTileCapture();

        FOrthoTileCapture(const FOrthoTileCapture&) = delete;
        FOrthoTileCapture& operator=(const FOrthoTileCapture&) = delete;

        bool IsValid() const;
        const FString& GetInitErrorCode() const { return InitErrorCode; }
        const FString& GetInitErrorMessage() const { return InitErrorMessage; }

        // Render one tile and read it back as BGRA8 sRGB, row-major from the TOP-LEFT pixel -
        // the layout PinWrightImage::FBitmap and PinWrightScreenshotUtils::EncodeBitmapToPng both
        // expect, so no swizzle happens anywhere on this path.
        //
        // Alpha is NOT stamped here. The caller must run PinWrightScreenshotUtils::IsBlankReadback
        // BEFORE ForceOpaqueAlpha, because the stamp turns a detectably-empty all-zero readback
        // into a plausible opaque black frame.
        //
        // Synchronous and game-thread only: CaptureScene() sends end-of-frame updates and renders
        // on the calling stack, and the readback flushes the rendering thread.
        bool CaptureTile(const PinWrightTileGrid::FTileCapturePlan& Plan, const FRotator& Rotation,
            TArray<FColor>& OutPixels, FTileTiming& OutTiming,
            PinWrightRenderCapture::FCaptureImageStats& OutStats, FTileCameraPose& OutPose,
            FString& OutErrorCode, FString& OutErrorMessage);

        // The exposure plan, with bPinned and Ev100Applied filled in from the component after the
        // first capture. Meaningful only once CaptureTile has run at least once.
        const FExposurePlan& GetExposurePlan() const { return Exposure; }

        int32 GetRenderCount() const { return RenderCount; }

        // Read back off the component rather than restated. This is the switch that decides
        // whether the ~2.1e6 cm culling-origin pushback is applied - i.e. whether distant
        // geometry is in these pixels at all - and it is also what would move the near plane off
        // 0. A response that printed a literal here would keep saying "false" after an archetype
        // change flipped it. Returns true only when the component exists and has it set.
        bool MeasureUpdateOrthoPlanes() const;

        // The component's show-flag state, measured off the component rather than restated. The
        // flags start from FEngineShowFlags(ESFIM_Game) (Engine/Private/Components/
        // SceneCaptureComponent.cpp:169), so no editor-only grid, gizmo, billboard, sprite or
        // spline overlay is in these pixels - which is what makes the frame comparable with a
        // downloaded reference image instead of with the editor's chrome.
        //
        // Carries the `viewMode` sub-block, which is emitted even when no mode was requested so
        // that "nothing was asked for" and "a mode was asked for and did nothing" stay
        // distinguishable.
        TSharedPtr<FJsonObject> DescribeShowFlags() const;

        // What the requested view mode did to the flags, measured at construction. Meaningful on a
        // capture whose component failed to allocate too: the request is still reported.
        const FViewModePlan& GetViewModePlan() const { return ViewMode; }

        // ---- landscape grass, built per tile ----
        //
        // This verb rendered ZERO landscape grass over ground the perspective capture fills with
        // it (B-ortho-capture-renders-no-landscape-grass), and it did so silently: valid,
        // non-blank, settled tiles of ground that is actually covered. The cause is not culling.
        // Grass is BUILT around camera locations that only a PERSPECTIVE editor viewport ever
        // supplies -- `UEditorEngine::UpdateSingleViewportClient` gates both feeds on
        // `IsPerspective()` (UE 5.8 EditorEngine.cpp:2613-2618, :2628-2634), and the only other
        // writer runs solely from `UGameViewportClient::Draw` (GameViewportClient.cpp:1913). A
        // `USceneCaptureComponent2D` is neither, so a tile burst can never be a grass camera at
        // all: it photographs whatever the editor's own viewport last built, over ground that
        // viewport may never have visited. See LandscapeGrassSettle.h.
        //
        // The report is the LAST tile's measurement (the tile whose pixels were read most
        // recently), with the build cost summed over the burst and the settle verdict ANDed --
        // three facts that mean different things and are kept apart rather than averaged.
        const PinWrightCaptureGrass::FGrassBuildReport& GetGrassBuild() const { return GrassBuild; }
        double GetGrassBuildTotalMs() const { return GrassBuildTotalMs; }
        // False when ANY tile's grass build was still outstanding when that tile was rendered.
        // Meaningful only once a tile with terrain in it has been captured.
        bool AllTilesGrassSettled() const { return bAllTilesGrassSettled; }

    private:
        TWeakObjectPtr<UWorld> World;
        TStrongObjectPtr<USceneCaptureComponent2D> CaptureComponent;
        TStrongObjectPtr<UTextureRenderTarget2D> RenderTarget;
        int32 PixelWidth = 0;
        int32 PixelHeight = 0;
        int32 RenderCount = 0;
        FExposurePlan Exposure;
        FViewModePlan ViewMode;
        PinWrightCaptureGrass::FGrassBuildReport GrassBuild;
        double GrassBuildTotalMs = 0.0;
        bool bAllTilesGrassSettled = true;
        FString InitErrorCode;
        FString InitErrorMessage;
    };
}
