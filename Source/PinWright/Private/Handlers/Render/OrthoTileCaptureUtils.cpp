// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Handlers/Render/OrthoTileCaptureUtils.h"
#include "Handlers/ErrorCodes.h"
#include "Handlers/Render/ViewModeVocabulary.h"

#include "Camera/CameraTypes.h"
#include "Compat/EngineVersionCompat.h"
#include "Components/PrimitiveComponent.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/EngineTypes.h"
#include "Engine/Scene.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/World.h"
#include "HAL/PlatformTime.h"
#include "RenderingThread.h"
#include "TextureResource.h"
#include "UObject/UObjectIterator.h"
// GetTransientPackage() returns UPackage*, and NewObject's outer parameter needs the
// UPackage -> UObject conversion, which requires the complete type.
#include "UObject/Package.h"
#include "UnrealClient.h"

namespace PinWrightOrthoTiles
{
    namespace
    {
        // Fstop and ISO the pin holds constant. 4.0 is FPostProcessSettings' own shipped default
        // for DepthOfFieldFstop and 100 is a full-stop ISO, so the solved shutter speed is an
        // exact power of two for every whole EV100 - the round trip through the engine's log2 is
        // then exact rather than nearly exact.
        constexpr float PinFstop = 4.0f;
        constexpr float PinIso = 100.0f;

        // Tolerance on the per-tile camera readback, in centimetres and degrees. Tight enough that
        // a move which did not happen at all cannot pass (tiles are thousands of centimetres
        // apart), loose enough to survive the float round trip through the component transform.
        constexpr double CameraPoseToleranceCm = 0.5;
        // ORIENTATION tolerance, in radians, compared as a quaternion angular distance rather
        // than component-wise on the rotator. At pitch +/-90 the rotator is gimbal-locked and yaw
        // and roll trade freely: the engine stores a requested (P=-90, Y=0, R=-90) back as
        // (P=-90, Y=-90, R=0), which is the SAME orientation and a component-wise comparison
        // calls it a failed move. Comparing what the renderer actually derives - the basis - is
        // both correct and the only form that cannot false-positive on a top-down capture, which
        // is this verb's most common pose.
        constexpr double CameraPoseToleranceRad = 1.0e-3;

        // Tolerance on the EV100 read back off the component after a capture. Generous enough to
        // survive float rounding at EV100 +/-30 and far tighter than a third of a stop, so a
        // clamp or a lost override is a measured failure rather than a rounding excuse.
        constexpr float Ev100VerifyTolerance = 1.0e-3f;
    }

    // ---------------------------------------------------------------------------------------
    // Scene bounds
    // ---------------------------------------------------------------------------------------

    FDepthBoundsSurvey SurveyWorldDepthBounds(UWorld* World, PinWrightTileGrid::EWorldAxis DepthAxis)
    {
        FDepthBoundsSurvey Survey;
        if (!World)
        {
            return Survey;
        }

        FBox Accumulated(ForceInit);
        for (TObjectIterator<UPrimitiveComponent> It; It; ++It)
        {
            UPrimitiveComponent* Component = *It;
            if (!IsValid(Component) || Component->IsTemplate() || Component->GetWorld() != World)
            {
                continue;
            }
            if (!Component->IsRegistered() || !Component->IsVisible())
            {
                continue;
            }

            // USceneComponent::GetBounds() is 5.8+; the Bounds member it returns is public on
            // every supported engine, so read it directly.
            const FBox ComponentBox = Component->Bounds.GetBox();
            if (!ComponentBox.IsValid)
            {
                continue;
            }
            // World-scale bounds are not an extent (see UnboundedPrimitiveExtentCm). Counted, not
            // silently skipped.
            const FVector Extent = ComponentBox.GetExtent();
            if (Extent.X >= UnboundedPrimitiveExtentCm ||
                Extent.Y >= UnboundedPrimitiveExtentCm ||
                Extent.Z >= UnboundedPrimitiveExtentCm)
            {
                ++Survey.NumUnbounded;
                continue;
            }
            Accumulated += ComponentBox;
            ++Survey.NumPrimitives;
        }

        // bMeasured is the conjunction of "we looked" and "we found something with extent". An
        // empty world leaves it false, so the caller refuses to place a camera rather than
        // placing one at the origin and calling that a measurement.
        if (Survey.NumPrimitives == 0 || !Accumulated.IsValid)
        {
            return Survey;
        }

        Survey.bMeasured = true;
        Survey.BoundsMin = Accumulated.Min;
        Survey.BoundsMax = Accumulated.Max;
        Survey.DepthMin = PinWrightTileGrid::GetAxisValue(Accumulated.Min, DepthAxis);
        Survey.DepthMax = PinWrightTileGrid::GetAxisValue(Accumulated.Max, DepthAxis);
        return Survey;
    }

    // ---------------------------------------------------------------------------------------
    // Exposure
    // ---------------------------------------------------------------------------------------

    float PhysicalCameraEv100(float Fstop, float ShutterSpeed, float Iso)
    {
        // One copy of the engine's formula (PostProcessEyeAdaptation.cpp:526), including its
        // FMath::Max(1.f, ISO) clamp, so the value this file solves for and the value it verifies
        // against cannot drift from each other or from the renderer.
        return FMath::Log2(FMath::Square(Fstop) * ShutterSpeed * 100.0f / FMath::Max(1.0f, Iso));
    }

    void SolvePhysicalCameraForEv100(float Ev100, float& OutFstop, float& OutShutterSpeed, float& OutIso)
    {
        OutFstop = PinFstop;
        OutIso = PinIso;
        // EV100 = log2(Fstop^2 * Shutter * 100 / ISO) = log2(16 * Shutter) = 4 + log2(Shutter).
        OutShutterSpeed = FMath::Pow(2.0f, Ev100 - 4.0f);
    }

    FString DescribeExposureBlockedByShowFlags(bool bLighting, bool bPostProcessing,
        const FString& RequestedViewModeKey)
    {
        const TCHAR* LightingText = bLighting ? TEXT("on") : TEXT("off");
        const TCHAR* PostText = bPostProcessing ? TEXT("on") : TEXT("off");
        if (RequestedViewModeKey.IsEmpty())
        {
            return FString::Printf(
                TEXT("the capture's show flags have Lighting=%s and PostProcessing=%s; ")
                TEXT("IsAutoExposureDebugMode (PostProcessEyeAdaptation.cpp:493-511) discards the ")
                TEXT("exposure override when either is off"),
                LightingText, PostText);
        }
        return FString::Printf(
            TEXT("view mode '%s' was requested for this burst, and it is what left the capture's ")
            TEXT("show flags at Lighting=%s / PostProcessing=%s; IsAutoExposureDebugMode ")
            TEXT("(PostProcessEyeAdaptation.cpp:493-511) discards the exposure override when either ")
            TEXT("is off. That is the requested mode doing what it says rather than a fault - but ")
            TEXT("the tiles still are not photometrically comparable with a pinned set, so drop ")
            TEXT("viewMode to get one."),
            *RequestedViewModeKey, LightingText, PostText);
    }

    // ---------------------------------------------------------------------------------------
    // The `viewMode` override
    // ---------------------------------------------------------------------------------------

    FEngineShowFlags MakeBaseCaptureShowFlags()
    {
        // ESFIM_Game is the component's own base (SceneCaptureComponent.cpp:169). The two
        // deviations below are this file's, and they are repeated here rather than shared with the
        // constructor on purpose: a reference that was PRODUCED BY the code under test cannot
        // catch that code writing something extra.
        FEngineShowFlags Flags(ESFIM_Game);
        Flags.SetTemporalAA(false);
        // UE 5.8 leaves VisualizeMegaLights ON in EVERY FEngineShowFlags. Init() memsets the whole
        // set to 1 and then clears the hidden visualization flags one at a time - VisualizeBuffer,
        // VisualizeNanite, VisualizeLumen, VisualizeVirtualShadowMap and the rest, ShowFlags.h:409-415
        // - and VisualizeMegaLights, added later, was never added to that list.
        //
        // It puts no MegaLights picture in the frame: MegaLights::GetVisualizeMode also needs
        // FSceneView::CurrentMegaLightsVisualizationMode (MegaLightsVisualize.cpp:54-73), which only
        // FEditorViewportClient ever writes (EditorViewportClient.cpp:4717) and a scene capture
        // leaves at NAME_None. What it does do is poison the READBACK: FindViewMode tests
        // VisualizeMegaLights fourth, above every lit mode (ShowFlags.cpp:814), so an untouched game
        // set derives back as VMI_VisualizeMegaLights and every burst that did not pass `viewMode`
        // reported `showFlags.viewMode.derived: "VisualizeMegaLights"` for a frame that was ordinary
        // Lit - the "accepted, echoed back, renders something else" failure this parameter exists to
        // prevent, inverted. Requesting any mode hid it, because ApplyViewMode writes the flag
        // unconditionally (ShowFlags.cpp:409); only the omitted path carried it.
        //
        // Cleared rather than special-cased in the derivation, so the component we own carries no
        // hidden visualization flag we did not ask for, and the second opinion stays the engine's
        // own FindViewMode rather than a PinWright reimplementation of it.
        // The flag itself only exists on 5.8+ (ShowFlagsValues.inl); older engines have no
        // VisualizeMegaLights entry for FindViewMode to read, so there is nothing to clear.
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 8, 0)
        Flags.SetVisualizeMegaLights(false);
#endif
        return Flags;
    }

    FViewModePlan ApplyViewModeToCaptureShowFlags(const PinWrightRenderCapture::FViewModePin& Pin,
        FEngineShowFlags& ShowFlags)
    {
        FViewModePlan Plan;
        Plan.bRequested = Pin.WantsOverride();
        if (Plan.bRequested)
        {
            Plan.RequestedKey = Pin.Key;
            Plan.RequestedValue = static_cast<int32>(Pin.ViewMode);
            Plan.DistinguishingShowFlags = Pin.DistinguishingShowFlags;

            // bPerspective=false: this capture is orthographic by construction. UE 5.8's
            // ApplyViewMode never reads the parameter (ShowFlags.cpp:292-446), but passing the
            // truth costs nothing and a future engine that starts reading it gets the right answer.
            ApplyViewMode(Pin.ViewMode, /*bPerspective=*/false, ShowFlags);
            // The second half, without which `unlit` leaves Lighting ON - see the header.
            EngineShowFlagOverride(ESFIM_Game, Pin.ViewMode, ShowFlags,
                /*bCanDisableTonemapper=*/false);

            // MEASURED, and measured AFTER the override rather than after ApplyViewMode: a flag
            // the override strips back is a flag the renderer will not see, and the caller needs
            // that named rather than reported as applied.
            Plan.ShowFlagMismatches = PinWrightViewModes::MeasureShowFlagMismatches(
                ShowFlags, Pin.ViewMode, /*bPerspective=*/false);
            Plan.bApplied = Plan.ShowFlagMismatches.Num() == 0;
            Plan.AppliedKey = Plan.bApplied ? Pin.Key : FString();
        }

        // Derived on BOTH paths, so a burst with no `viewMode` still says what the frame is.
        const EViewModeIndex Derived = FindViewMode(ShowFlags);
        Plan.bDerivedMeasured = true;
        Plan.DerivedValue = static_cast<int32>(Derived);
        Plan.DerivedKey = PinWrightViewModes::GetKey(Derived);
        return Plan;
    }

    bool IsViewModeReachableOnSceneCapture(EViewModeIndex ViewMode, FString& OutErrorCode,
        FString& OutErrorMessage)
    {
        OutErrorCode.Reset();
        OutErrorMessage.Reset();

        switch (ViewMode)
        {
        case VMI_VisualizeBuffer:
        case VMI_VisualizeSubstrate:
            // A BACKSTOP, not the first line of defence: this verb parses with no viewport client,
            // so PinWrightViewModes::Resolve already refuses all ten sub-visualisation modes with
            // VIEW_MODE_NEEDS_COMPANION before anything reaches here. It exists because that
            // refusal is a property of the VOCABULARY, and these two are additionally unreachable
            // as a property of the RENDERER - if the vocabulary ever stops classifying them as
            // companion modes, this still refuses rather than shipping a Lit frame.
            OutErrorCode = ErrorCodes::ERR_VIEW_MODE_NOT_RENDERABLE;
            OutErrorMessage = FString::Printf(
                TEXT("View mode '%s' cannot be produced by render.capture_ortho_tiles even with a ")
                TEXT("sub-visualisation target selected. Its picture is chosen by ")
                TEXT("FSceneViewFamily::ViewMode plus a ViewModeParam name, and the scene-capture ")
                TEXT("renderer writes NEITHER: the family constructor leaves ViewMode at VMI_Lit ")
                TEXT("(UE 5.8 Runtime/Engine/Private/SceneView.cpp:3054) and ")
                TEXT("Renderer/Private/SceneCaptureRendering.cpp assigns it nowhere, while the only ")
                TEXT("two consumers that read it - CustomDepthRendering.cpp:308 and ")
                TEXT("HairStrands/HairStrandsComposition.cpp:767-769 - test it against ")
                TEXT("VMI_VisualizeBuffer / VMI_VisualizeSubstrate exactly. Setting the show flag ")
                TEXT("alone renders a plausible near-Lit frame labelled as the mode. Take this ")
                TEXT("family through the viewport path instead: editor.set_view_mode plus ")
                TEXT("render.capture_open_level."),
                *PinWrightViewModes::GetKey(ViewMode));
            return false;

        case VMI_LightmapDensity:
        case VMI_LitLightmapDensity:
        case VMI_StationaryLightOverlap:
        case VMI_CollisionPawn:
        case VMI_CollisionVisibility:
            // These are refused as a SCOPE decision with an honest reason, and the reason is not
            // "the show flag cannot be set" - it can: the component's FEngineShowFlags become the
            // view family's flags verbatim (UE 5.8 Renderer/Private/SceneCaptureRendering.cpp
            // :905-909). What is missing is a MEASUREMENT that the flag produces the picture on
            // this renderer. Every mode in this family draws through ApplyViewModeOverrides
            // (Runtime/Engine/Private/PrimitiveDrawingUtils.cpp:1610-1626), which early-outs
            // entirely unless AllowDebugViewmodes() (RenderCore/Private/ShaderCore.cpp:579) and
            // then substitutes GEngine's editor debug materials per mesh; collision additionally
            // needs per-primitive collision proxies (StaticMeshSceneProxy.cpp:1633-1647). Nothing
            // in this project has ever rendered one of them through a scene capture, and a mode
            // that sets its flag and still draws a near-Lit picture is indistinguishable from one
            // that worked. Refusing by name is the only outcome a caller can act on.
            OutErrorCode = ErrorCodes::ERR_VIEW_MODE_NOT_RENDERABLE;
            OutErrorMessage = FString::Printf(
                TEXT("View mode '%s' is refused on render.capture_ortho_tiles rather than applied. ")
                TEXT("This verb renders through a transient USceneCaptureComponent2D, and this ")
                TEXT("family's picture is not decided by its show flag alone: it is drawn by ")
                TEXT("ApplyViewModeOverrides (UE 5.8 Runtime/Engine/Private/PrimitiveDrawingUtils.cpp")
                TEXT(":1610), which early-outs unless AllowDebugViewmodes() ")
                TEXT("(RenderCore/Private/ShaderCore.cpp:579) and substitutes GEngine's editor debug ")
                TEXT("materials per mesh - the editor viewport's own ApplyViewMode + ")
                TEXT("EngineShowFlagOverride pairing in FEditorViewportClient::Draw is what puts a ")
                TEXT("frame in front of that path, and it does not run here. Writing the flag anyway ")
                TEXT("would return a near-Lit frame labelled '%s', which no response field could ")
                TEXT("distinguish from a working one. Use editor.set_view_mode plus ")
                TEXT("render.capture_open_level for this family; the modes render.capture_ortho_tiles ")
                TEXT("does carry are the show-flag ones (unlit, wireframe, lighting_only, ")
                TEXT("detail_lighting, reflection_override, front_back_face, zebra, clay, ")
                TEXT("random_color, shader_complexity, quad_overdraw, lod_coloration, ")
                TEXT("hlod_coloration and the texture-accuracy modes)."),
                *PinWrightViewModes::GetKey(ViewMode), *PinWrightViewModes::GetKey(ViewMode));
            return false;

        default:
            return true;
        }
    }

    TSharedPtr<FJsonObject> MakeViewModeInfoObject(const FViewModePlan& Plan)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        // Unconditional, per the same rule the viewport capture path follows: "no mode was asked
        // for" and "a mode was asked for and did nothing" must be distinguishable from the
        // response alone.
        Obj->SetBoolField(TEXT("requested"), Plan.bRequested);
        // The engine's own read-back of the flags, present either way. See FViewModePlan for why
        // it must not be compared against `requested` as a verdict, and why the pair is guarded
        // rather than published as a zero that spells VMI_BrushWireframe.
        Obj->SetBoolField(TEXT("derivedMeasured"), Plan.bDerivedMeasured);
        if (Plan.bDerivedMeasured)
        {
            Obj->SetStringField(TEXT("derived"), Plan.DerivedKey);
            Obj->SetNumberField(TEXT("derivedValue"), Plan.DerivedValue);
        }
        if (!Plan.bRequested)
        {
            return Obj;
        }
        Obj->SetStringField(TEXT("requestedKey"), Plan.RequestedKey);
        Obj->SetNumberField(TEXT("requestedValue"), Plan.RequestedValue);
        // The measured verdict, and the key ONLY when it holds.
        Obj->SetStringField(TEXT("applied"), Plan.AppliedKey);
        Obj->SetBoolField(TEXT("flagsTook"), Plan.bApplied);

        TArray<TSharedPtr<FJsonValue>> Distinguishing;
        for (const FString& Name : Plan.DistinguishingShowFlags)
        {
            Distinguishing.Add(MakeShared<FJsonValueString>(Name));
        }
        Obj->SetArrayField(TEXT("distinguishingShowFlags"), Distinguishing);

        TArray<TSharedPtr<FJsonValue>> Mismatches;
        for (const FString& Name : Plan.ShowFlagMismatches)
        {
            Mismatches.Add(MakeShared<FJsonValueString>(Name));
        }
        // Emitted even when empty: an absent array reads as "not checked", and this is the field
        // that answers which show-flag families actually take on a GAME flag set.
        Obj->SetArrayField(TEXT("showFlagMismatches"), Mismatches);

        // There is no `restored` here and there deliberately never will be: the component is
        // created per call and discarded, so nothing outlives the burst to restore.
        Obj->SetBoolField(TEXT("scoped"), true);
        Obj->SetStringField(TEXT("scope"),
            TEXT("the transient capture component only - no viewport is written, so nothing is restored"));
        return Obj;
    }

    // ---------------------------------------------------------------------------------------
    // FOrthoTileCapture
    // ---------------------------------------------------------------------------------------

    FOrthoTileCapture::FOrthoTileCapture(UWorld* InWorld, int32 InPixelWidth, int32 InPixelHeight,
        const PinWrightRenderCapture::FExposurePin& InExposure,
        const PinWrightRenderCapture::FViewModePin& InViewMode)
        : World(InWorld)
        , PixelWidth(InPixelWidth)
        , PixelHeight(InPixelHeight)
    {
        Exposure.Mode = InExposure.Mode;
        Exposure.Ev100Requested = InExposure.Ev100;
        // Recorded before anything can fail, so a capture that never allocated a component still
        // reports what was asked for rather than reporting nothing.
        ViewMode.bRequested = InViewMode.WantsOverride();
        ViewMode.RequestedKey = InViewMode.Key;
        ViewMode.RequestedValue = static_cast<int32>(InViewMode.ViewMode);
        ViewMode.DistinguishingShowFlags = InViewMode.DistinguishingShowFlags;

        if (!InWorld)
        {
            InitErrorCode = ErrorCodes::ERR_NO_EDITOR_WORLD;
            InitErrorMessage = TEXT("No world to render");
            return;
        }
        if (PixelWidth < MinTilePixels || PixelHeight < MinTilePixels ||
            PixelWidth > MaxTilePixels || PixelHeight > MaxTilePixels)
        {
            InitErrorCode = ErrorCodes::ERR_INVALID_ARGUMENT;
            InitErrorMessage = FString::Printf(
                TEXT("tile pixel size %d x %d is outside [%d, %d]"),
                PixelWidth, PixelHeight, MinTilePixels, MaxTilePixels);
            return;
        }

        // Ownerless + transient + outered to the transient package. Nothing about this component
        // reaches the level, its actor list, its package dirty state or the outliner - which is
        // what lets a capture verb run against a map another agent is mid-edit on.
        USceneCaptureComponent2D* Component = NewObject<USceneCaptureComponent2D>(
            GetTransientPackage(), NAME_None, RF_Transient);
        if (!Component)
        {
            InitErrorCode = ErrorCodes::ERR_SCENE_CAPTURE_FAILED;
            InitErrorMessage = TEXT("Could not allocate the orthographic capture component");
            return;
        }

        // On-demand capture only. Both of these ship TRUE, and either one left on makes the
        // component re-render every frame for as long as it is registered.
        Component->bCaptureEveryFrame = false;
        Component->bCaptureOnMovement = false;
        // Moved once per tile. USceneComponent's constructor already defaults to Movable, and an
        // editor world exempts a static component from the move refusal anyway
        // (CheckStaticMobilityAndWarn, SceneComponent.cpp:3386-3390) - set explicitly so the
        // per-tile move cannot depend on either of those staying true.
        Component->SetMobility(EComponentMobility::Movable);
        // The opposite of what FSceneCaptureProbe wants, and for a reason that is about the
        // capture SOURCE. With bCaptureEveryFrame false, USceneCaptureComponent::GetViewState
        // allocates a view state ONLY when this is true (SceneCaptureComponent.cpp:421), and a
        // FinalColor capture runs the whole post-process chain - eye adaptation included - off
        // that state. The probe can leave it off because it reads G-buffer and depth sources,
        // which never touch it; a picture-producing path should not.
        //
        // Honest limit on that reasoning: this was turned on while chasing black frames during
        // bring-up, and it was NOT what made them stop being black (the map is close to black
        // from directly above in a headless editor, on the viewport renderer too). It is kept
        // because the engine's own requirement above is real, not because a measurement here
        // attributes anything to it.
        //
        // The cost is that temporal-history passes now carry state ACROSS tiles within a burst,
        // so tile N is not strictly independent of tile N-1. TemporalAA is switched off below for
        // the jitter half of that; the rest is disclosed rather than pretended away.
        Component->bAlwaysPersistRenderingState = true;
        // DM_Low is the lowest detail mode, so IsCulledByDetailMode() can never be true. A
        // detail-mode cull makes CaptureScene() a silent no-op that leaves the render target at
        // its clear colour and still returns normally.
        Component->DetailMode = EDetailMode::DM_Low;
        Component->ProjectionType = ECameraProjectionMode::Orthographic;
        // FinalColor, not SceneColor: these are pictures for a human and for image.compare, so
        // they want the full post-process chain including the tone curve. It is also what keeps
        // ViewFamily.EngineShowFlags.PostProcessing on (SceneCaptureRendering.cpp:751 clears it
        // only for the CaptureSceneColor path), which is what the exposure pin needs.
        Component->CaptureSource = SCS_FinalColorLDR;
        // PRM_LegacySceneCapture is the ZERO value of ESceneCapturePrimitiveRenderMode
        // (EngineTypes.h:60-68) and USceneCaptureComponent's constructor never assigns the
        // property, so an unset component silently takes the legacy path. Ask for the modern one
        // by name. (Set on the strength of the enum's own naming and prior measurement elsewhere
        // in this project, not on a before/after taken here - no frame in this file's bring-up
        // was shown to change because of it.)
        Component->PrimitiveRenderMode = ESceneCapturePrimitiveRenderMode::PRM_RenderScenePrimitives;
        // The two halves of the near-plane / distance-culling trade, set EXPLICITLY rather than
        // inherited: leaving bUpdateOrthoPlanes false keeps the culling origin at the camera
        // (16/16 static meshes and 64/64 HISM instances visible where the viewport path saw
        // 0/16 and 0/64), at the price of a near plane hardcoded to 0 that the caller's camera
        // depth has to clear. Both are archetype-defaulted values, and an archetype is exactly
        // the thing that can change under us.
        //
        // ENGINE VERSIONS (verified by grepping C:\UE_5.3 ... C:\UE_5.8):
        //   5.4 - 5.8  both fields exist (SceneCaptureComponent2D.h:68,72 on 5.8)
        //   5.3        NEITHER field exists, and 5.3's scene capture applies no ortho-plane
        //              correction at all - i.e. it behaves as though both were false, which is
        //              what these two lines ask for. A 5.3 backport deletes them and changes
        //              nothing else. Recorded in docs/engine-version-support.md.
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 4, 0)
        Component->bUpdateOrthoPlanes = false;
        Component->bUseCameraHeightAsViewTarget = false;
#endif
        // Not the tiling this verb does. It is ignored for a FinalColor source anyway
        // (SceneCaptureComponent2D.h:113-124) and would render one image across n frames rather
        // than n georeferenced images, but an inherited true would silently offset every frame.
        Component->bEnableOrthographicTiling = false;
        Component->SetVisibility(true);

        Component->RegisterComponentWithWorld(InWorld);
        if (!Component->IsRegistered())
        {
            InitErrorCode = ErrorCodes::ERR_SCENE_CAPTURE_FAILED;
            InitErrorMessage = TEXT("Could not register the orthographic capture component with the world");
            return;
        }
        CaptureComponent.Reset(Component);

        // ---- SHOW FLAGS, WRITTEN AFTER REGISTRATION AND NEVER BEFORE ----
        //
        // USceneCaptureComponent::OnRegister calls UpdateShowFlags (UE 5.8
        // Runtime/Engine/Private/Components/SceneCaptureComponent.cpp:292, :435-451), whose FIRST
        // statement is `ShowFlags = Archetype->ShowFlags` - the whole set is reassigned from the
        // CDO and then the (here empty) ShowFlagSettings array is replayed over it. Every write
        // made before RegisterComponentWithWorld is therefore silently discarded, with no return
        // value and no log line to say so.
        //
        // This is not hypothetical: SetTemporalAA(false) used to sit above the register call and
        // was reverted on every single burst. The response reported `temporalAA: true` truthfully
        // the whole time - the report was measured, so it never lied; the intent was just never
        // reaching the renderer, and the two tiles of a mosaic that this flag exists to keep
        // comparable were being jittered independently after all.
        //
        // With a persistent view state the renderer would otherwise jitter the projection per
        // frame and blend against history, so two tiles of one mosaic would differ by their own
        // sub-pixel offsets at the seam. Off is what makes a burst comparable with itself.
        Component->ShowFlags.SetTemporalAA(false);

        // The second deviation from the game defaults, and the second flag that must be written
        // here rather than above the register call. UE 5.8's FEngineShowFlags::Init never clears
        // VisualizeMegaLights (ShowFlags.h:409-415 clears its every sibling), and FindViewMode
        // reads it above every lit mode (ShowFlags.cpp:814) - so a burst with no `viewMode` derived
        // back as VisualizeMegaLights. MakeBaseCaptureShowFlags carries the full reasoning.
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 8, 0)
        Component->ShowFlags.SetVisualizeMegaLights(false);
#endif

        // ---- the view-mode override, applied ONCE for the whole burst ----
        //
        // Same structural reason as the exposure pin below: one component and one flag set for the
        // burst, so "every tile was drawn in the same mode" is a property of the object rather
        // than a loop invariant somebody has to maintain. An unrequested pin writes nothing.
        ViewMode = ApplyViewModeToCaptureShowFlags(InViewMode, Component->ShowFlags);

        // ---- the exposure pin, written once for the whole burst ----
        //
        // Written here rather than per tile precisely so it CANNOT vary between tiles: there is
        // one component and one settings block for the burst, so "all tiles share one pin" is
        // structural rather than a loop invariant somebody has to maintain.
        if (InExposure.WantsPin())
        {
            SolvePhysicalCameraForEv100(InExposure.Ev100, Exposure.Fstop, Exposure.ShutterSpeed, Exposure.Iso);

            FPostProcessSettings& Settings = Component->PostProcessSettings;
            Settings.bOverride_AutoExposureMethod = true;
            Settings.AutoExposureMethod = AEM_Manual;
            Settings.bOverride_AutoExposureApplyPhysicalCameraExposure = true;
            Settings.AutoExposureApplyPhysicalCameraExposure = 1;
            Settings.bOverride_DepthOfFieldFstop = true;
            Settings.DepthOfFieldFstop = Exposure.Fstop;
            Settings.bOverride_CameraShutterSpeed = true;
            Settings.CameraShutterSpeed = Exposure.ShutterSpeed;
            Settings.bOverride_CameraISO = true;
            Settings.CameraISO = Exposure.Iso;
            // AEM_Manual does NOT reset the exposure compensation terms
            // (PostProcessEyeAdaptation.cpp:618-651 applies them on that branch too), so a level
            // whose post-process volume carries a bias would shift every tile by it. Both are
            // pinned to their neutral values.
            Settings.bOverride_AutoExposureBias = true;
            Settings.AutoExposureBias = 0.0f;
            Settings.bOverride_AutoExposureBiasCurve = true;
            Settings.AutoExposureBiasCurve = nullptr;
            // The component's own settings only reach the frame at a non-zero blend weight.
            Component->PostProcessBlendWeight = 1.0f;
        }
        else
        {
            Exposure.BlockedReason = TEXT("no pin was requested");
        }

        // ---- render target, allocated ONCE for the burst ----
        UTextureRenderTarget2D* Target = NewObject<UTextureRenderTarget2D>(
            GetTransientPackage(), NAME_None, RF_Transient);
        if (!Target)
        {
            InitErrorCode = ErrorCodes::ERR_RENDER_TARGET_CREATE_FAILED;
            InitErrorMessage = TEXT("Could not allocate the orthographic capture render target");
            return;
        }
        // RTF_RGBA8_SRGB, measured: a non-sRGB target of the same bit depth came back 27.8% off
        // the reference. bForceLinearGamma is the flag that decides it (TextureRenderTarget2D.cpp
        // :253 pairs the two), and it defaults TRUE on a fresh render target, so both are set.
        Target->RenderTargetFormat = RTF_RGBA8_SRGB;
        Target->bForceLinearGamma = false;
        Target->SRGB = true;
        Target->ClearColor = FLinearColor::Black;
        Target->bAutoGenerateMips = false;
        Target->InitAutoFormat(static_cast<uint32>(PixelWidth), static_cast<uint32>(PixelHeight));
        Target->UpdateResourceImmediate(/*bClearRenderTarget=*/true);
        RenderTarget.Reset(Target);
        Component->TextureTarget = Target;
    }

    FOrthoTileCapture::~FOrthoTileCapture()
    {
        if (USceneCaptureComponent2D* Component = CaptureComponent.Get())
        {
            Component->TextureTarget = nullptr;
            if (Component->IsRegistered())
            {
                Component->UnregisterComponent();
            }
        }
        if (UTextureRenderTarget2D* Target = RenderTarget.Get())
        {
            Target->ReleaseResource();
        }
        // Both handles are released here and collected by the next ordinary GC. This path never
        // calls CollectGarbage(): a synchronous collect from inside an RPC is the reentrancy
        // crash recorded as B-python-execute-reentrant-gc-crash.
        CaptureComponent.Reset();
        RenderTarget.Reset();
    }

    bool FOrthoTileCapture::IsValid() const
    {
        return World.IsValid() && CaptureComponent.IsValid() && CaptureComponent->IsRegistered()
            && RenderTarget.IsValid();
    }

    bool FOrthoTileCapture::CaptureTile(const PinWrightTileGrid::FTileCapturePlan& Plan,
        const FRotator& Rotation, TArray<FColor>& OutPixels, FTileTiming& OutTiming,
        PinWrightRenderCapture::FCaptureImageStats& OutStats, FTileCameraPose& OutPose,
        FString& OutErrorCode, FString& OutErrorMessage)
    {
        OutPixels.Reset();
        OutTiming = FTileTiming();
        OutStats = PinWrightRenderCapture::FCaptureImageStats();
        OutPose = FTileCameraPose();
        OutErrorCode.Reset();
        OutErrorMessage.Reset();

        if (!IsValid())
        {
            OutErrorCode = InitErrorCode.IsEmpty() ? FString(ErrorCodes::ERR_SCENE_CAPTURE_FAILED) : InitErrorCode;
            OutErrorMessage = InitErrorMessage.IsEmpty()
                ? TEXT("Orthographic capture is not initialised") : InitErrorMessage;
            return false;
        }
        if (Plan.PixelWidth != PixelWidth || Plan.PixelHeight != PixelHeight)
        {
            // Structural, not defensive: the render target was sized once for the burst, and a
            // tile of a different size would be read back through the wrong stride. The engine
            // check() this protects against is documented in the header.
            OutErrorCode = ErrorCodes::ERR_INVALID_ARGUMENT;
            OutErrorMessage = FString::Printf(
                TEXT("tile (%d,%d) asks for %d x %d px but this burst is fixed at %d x %d px"),
                Plan.Tile.Col, Plan.Tile.Row, Plan.PixelWidth, Plan.PixelHeight, PixelWidth, PixelHeight);
            return false;
        }
        if (!(Plan.OrthoWidth > 0.0f))
        {
            OutErrorCode = ErrorCodes::ERR_INVALID_ARGUMENT;
            OutErrorMessage = FString::Printf(TEXT("tile (%d,%d) has a non-positive orthoWidth (%f cm)"),
                Plan.Tile.Col, Plan.Tile.Row, Plan.OrthoWidth);
            return false;
        }

        USceneCaptureComponent2D* Component = CaptureComponent.Get();
        Component->OrthoWidth = Plan.OrthoWidth;
        Component->SetWorldLocationAndRotation(Plan.CameraLocation, Rotation);

        // Read the pose back off the component BEFORE rendering, and refuse if the move did not
        // land. GetComponentToWorld() is exactly what the renderer will read
        // (SceneCaptureRendering.cpp:1180), so this checks the value the pixels will be drawn
        // from rather than the value that was requested - the two are different facts and only
        // the first one is about this frame.
        OutPose.Location = Component->GetComponentLocation();
        OutPose.Rotation = Component->GetComponentRotation();
        OutPose.bMatchesRequest =
            OutPose.Location.Equals(Plan.CameraLocation, CameraPoseToleranceCm) &&
            Component->GetComponentQuat().AngularDistance(Rotation.Quaternion()) <= CameraPoseToleranceRad;
        if (!OutPose.bMatchesRequest)
        {
            OutErrorCode = ErrorCodes::ERR_CAPTURE_CAMERA_NOT_APPLIED;
            OutErrorMessage = FString::Printf(
                TEXT("Tile (%d,%d) asked the capture component to move to %s / %s, and it read back at %s / %s. ")
                TEXT("The renderer takes the pose from the component transform, so every tile would have been drawn ")
                TEXT("from the same wrong place while still reporting per-tile world extents."),
                Plan.Tile.Col, Plan.Tile.Row,
                *Plan.CameraLocation.ToString(), *Rotation.ToString(),
                *OutPose.Location.ToString(), *OutPose.Rotation.ToString());
            return false;
        }

        // ---- landscape grass for THIS tile, before its pixels exist ----
        //
        // Per tile, not once per burst: grass is built within a cull-distance band of a camera
        // point, so a burst settled at tile (0,0) would still render bare ground at (3,3). Placed
        // after the pose read-back so the location handed to the build is the one the renderer
        // will draw from, and before RenderStart so the build cost is not billed as render time.
        // Nothing here moves a camera; the location goes to RegenerateGrass as data, and the grass
        // components it creates are RF_Transient and rebuilt around the user's own camera on the
        // next editor tick. See LandscapeGrassSettle.h and the class comment in the header.
        GrassBuild = PinWrightCaptureGrass::SettleGrassForCapturePose(World.Get(), OutPose.Location);
        // The scene-capture ortho path is not the editor viewport path: BuildOrthoMatrix hardcodes
        // a near plane of 0, so there is no near-plane correction moving the culling origin off the
        // camera unless bUpdateOrthoPlanes is set on the component. The camera location is
        // therefore the origin used here, and it is published as UNMEASURED -- it is derived from
        // the component's configuration rather than read off an FSceneView, and the response has
        // to keep those apart.
        PinWrightCaptureGrass::MeasureGrassFrameReach(World.Get(), OutPose.Location,
            /*bInCullingOriginMeasured=*/false,
            PinWrightRenderCapture::ReadEffectiveViewDistanceScale(), GrassBuild);
        GrassBuildTotalMs += GrassBuild.BuildMs;
        if (GrassBuild.bMeasured && GrassBuild.LandscapeProxies > 0 && !GrassBuild.bSettled)
        {
            bAllTilesGrassSettled = false;
        }

        const double RenderStart = FPlatformTime::Seconds();
        // Synchronous: CaptureScene sends end-of-frame updates, builds a scene render and
        // executes it before returning. CaptureSceneDeferred would only queue the work, to be
        // drained when a main view family renders - which in a headless or idle editor may never
        // happen, giving a clean success over an unwritten render target.
        Component->CaptureScene();
        FlushRenderingCommands();
        const double ReadbackStart = FPlatformTime::Seconds();
        ++RenderCount;

        FTextureRenderTargetResource* Resource = RenderTarget->GameThread_GetRenderTargetResource();
        if (!Resource)
        {
            OutErrorCode = ErrorCodes::ERR_READ_PIXELS_FAILED;
            OutErrorMessage = TEXT("Orthographic capture render target has no render resource");
            return false;
        }
        if (!Resource->ReadPixels(OutPixels))
        {
            OutErrorCode = ErrorCodes::ERR_READ_PIXELS_FAILED;
            OutErrorMessage = FString::Printf(TEXT("Could not read tile (%d,%d) back to the CPU"),
                Plan.Tile.Col, Plan.Tile.Row);
            return false;
        }

        const int64 Expected = static_cast<int64>(PixelWidth) * static_cast<int64>(PixelHeight);
        if (static_cast<int64>(OutPixels.Num()) != Expected)
        {
            const int32 Got = OutPixels.Num();
            OutPixels.Reset();
            OutErrorCode = ErrorCodes::ERR_READ_PIXELS_FAILED;
            OutErrorMessage = FString::Printf(
                TEXT("Tile (%d,%d) readback returned %d pixels, expected %lld"),
                Plan.Tile.Col, Plan.Tile.Row, Got, Expected);
            return false;
        }

        // The luminance classifier the viewport capture path already publishes, run on these
        // pixels too. It exists because the all-zero blank test is not enough: a frame the
        // renderer filled with tonemapper dither over nothing is NOT all-zero, so it passes that
        // test while showing nothing - which is exactly how a whole 16-tile burst of black tiles
        // reported `blank: false` during this verb's own bring-up. One classifier for both
        // renderers, so neither can drift into its own idea of "empty".
        OutStats = PinWrightRenderCapture::CalculateCaptureImageStats(OutPixels);

        const double Now = FPlatformTime::Seconds();
        OutTiming.RenderMs = (ReadbackStart - RenderStart) * 1000.0;
        OutTiming.ReadbackMs = (Now - ReadbackStart) * 1000.0;
        OutTiming.TotalMs = (Now - RenderStart) * 1000.0;

        // ---- the exposure verdict, measured after the pixels exist ----
        //
        // Read off the component's settings rather than echoed from the request, and recomputed
        // through the engine's own formula rather than compared field by field: a lost override
        // or a clamp anywhere in the chain lands as a different EV100 here, which is the fact the
        // caller needs. Recomputed every tile because a silent divergence part-way through a
        // burst is exactly the failure a mosaic cannot survive.
        const FPostProcessSettings& Settings = Component->PostProcessSettings;
        Exposure.Ev100Applied = PhysicalCameraEv100(
            Settings.DepthOfFieldFstop, Settings.CameraShutterSpeed, Settings.CameraISO);
        Exposure.bEv100Measured = true;
        if (Exposure.Mode != PinWrightRenderCapture::EExposureRequestMode::Fixed)
        {
            Exposure.bPinned = false;
        }
        else if (Settings.AutoExposureMethod != AEM_Manual ||
            Settings.AutoExposureApplyPhysicalCameraExposure == 0)
        {
            Exposure.bPinned = false;
            Exposure.BlockedReason = TEXT("the component's metering mode is not Manual with physical "
                "camera exposure applied, so CalculateManualAutoExposure "
                "(PostProcessEyeAdaptation.cpp:519-532) is not the branch that set the exposure");
        }
        else if (!FMath::IsNearlyEqual(Exposure.Ev100Applied, Exposure.Ev100Requested, Ev100VerifyTolerance))
        {
            Exposure.bPinned = false;
            Exposure.BlockedReason = FString::Printf(
                TEXT("the physical-camera settings on the component evaluate to EV100 %.4f, not the "
                     "requested %.4f (f/%.3f, shutter 1/%g s, ISO %.1f)"),
                Exposure.Ev100Applied, Exposure.Ev100Requested,
                Settings.DepthOfFieldFstop, Settings.CameraShutterSpeed, Settings.CameraISO);
        }
        else if (!Component->ShowFlags.Lighting || !Component->ShowFlags.PostProcessing)
        {
            // IsAutoExposureDebugMode (PostProcessEyeAdaptation.cpp:493-511) short-circuits the
            // whole exposure chain on either of these, and a frame that ignored the pin is not
            // comparable with one that honoured it.
            //
            // The VERDICT is the same whoever cleared the flags - the pin does not govern these
            // pixels either way - but the REASON is not. A requested `unlit` clears Lighting on
            // purpose (EngineShowFlagOverride, ShowFlags.cpp:530-585), and reporting that in the
            // words of an unexplained lighting-off frame sends a caller hunting a fault they
            // deliberately caused. The mode is named instead; see
            // DescribeExposureBlockedByShowFlags.
            Exposure.bPinned = false;
            Exposure.BlockedReason = DescribeExposureBlockedByShowFlags(
                Component->ShowFlags.Lighting != 0,
                Component->ShowFlags.PostProcessing != 0,
                ViewMode.bRequested ? ViewMode.RequestedKey : FString());
        }
        else
        {
            Exposure.bPinned = true;
            Exposure.BlockedReason.Reset();
        }

        return true;
    }

    bool FOrthoTileCapture::MeasureUpdateOrthoPlanes() const
    {
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 4, 0)
        const USceneCaptureComponent2D* Component = CaptureComponent.Get();
        return Component != nullptr && Component->bUpdateOrthoPlanes;
#else
        // 5.3 applies no ortho-plane correction at all, so the measurement is a constant false
        // rather than a read of a field that does not exist.
        return false;
#endif
    }

    TSharedPtr<FJsonObject> FOrthoTileCapture::DescribeShowFlags() const
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        // "game" is the FEngineShowFlags constructor argument the component ships with
        // (SceneCaptureComponent.cpp:169), not an editor set. Named so a reader can tell at a
        // glance that no editor chrome is in these pixels.
        Obj->SetStringField(TEXT("source"), TEXT("game"));
        const USceneCaptureComponent2D* Component = CaptureComponent.Get();
        Obj->SetBoolField(TEXT("measured"), Component != nullptr);
        // Emitted ABOVE the null-component early return, so a capture that never allocated still
        // reports what mode was asked for. Unconditional for the same reason the viewport path's
        // block is: "nothing was requested" and "something was requested and did nothing" are
        // different answers and a caller acts differently on them.
        Obj->SetObjectField(TEXT("viewMode"), MakeViewModeInfoObject(ViewMode));
        if (!Component)
        {
            return Obj;
        }
        // The flags a reference comparison actually turns on: whether the frame carries lighting
        // and post-processing at all, and whether any editor-only overlay could be in it.
        Obj->SetBoolField(TEXT("lighting"), Component->ShowFlags.Lighting != 0);
        Obj->SetBoolField(TEXT("postProcessing"), Component->ShowFlags.PostProcessing != 0);
        Obj->SetBoolField(TEXT("atmosphere"), Component->ShowFlags.Atmosphere != 0);
        Obj->SetBoolField(TEXT("fog"), Component->ShowFlags.Fog != 0);
        Obj->SetBoolField(TEXT("landscape"), Component->ShowFlags.Landscape != 0);
        Obj->SetBoolField(TEXT("instancedFoliage"), Component->ShowFlags.InstancedFoliage != 0);
        Obj->SetBoolField(TEXT("translucency"), Component->ShowFlags.Translucency != 0);
        Obj->SetBoolField(TEXT("grid"), Component->ShowFlags.Grid != 0);
        Obj->SetBoolField(TEXT("splines"), Component->ShowFlags.Splines != 0);
        Obj->SetBoolField(TEXT("billboardSprites"), Component->ShowFlags.BillboardSprites != 0);
        Obj->SetBoolField(TEXT("selection"), Component->ShowFlags.Selection != 0);
        Obj->SetBoolField(TEXT("temporalAA"), Component->ShowFlags.TemporalAA != 0);
        Obj->SetBoolField(TEXT("motionBlur"), Component->ShowFlags.MotionBlur != 0);
        // Published because the engine ships it ON in every FEngineShowFlags and it is what
        // `viewMode.derived` reads first: a true here is the tell that the clear in the constructor
        // was lost, and `derived` will be saying VisualizeMegaLights over a Lit frame.
        // The flag only exists on 5.8+; the field is omitted on older engines rather than
        // reported as a constant false, so a reader can tell "off" from "not a thing here".
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 8, 0)
        Obj->SetBoolField(TEXT("visualizeMegaLights"), Component->ShowFlags.VisualizeMegaLights != 0);
#endif
        return Obj;
    }
}
