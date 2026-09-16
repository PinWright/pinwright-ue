// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Handlers/Render/PreviewViewportCaptureUtils.h"

#include "Handlers/ErrorCodes.h"
#include "Handlers/Render/CaptureResolutionViewExtension.h"
#include "Handlers/Render/FlatRegionStats.h"
// The ONE view-mode vocabulary: key spelling, wire parsing, and the show-flag classification the
// scoped override below measures against. Shared with editor.set_view_mode so the two cannot
// disagree about what a mode name means.
#include "Handlers/Render/ViewModeVocabulary.h"
#include "Utils/JsonUtils.h"
#include "Utils/ScreenshotUtils.h"

#include "Compat/EngineVersionCompat.h"
#include "Dom/JsonObject.h"
#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/PrimitiveComponent.h"
#include "Engine/EngineBaseTypes.h"
#include "Engine/World.h"
#include "Editor.h"
#include "EditorViewportClient.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformTime.h"
#include "ImageUtils.h"
#include "LevelEditorViewport.h"
#include "Misc/FileHelper.h"
#include "Misc/ScopeExit.h"
#include "RenderingThread.h"
// EV100ToLuminance / LuminanceToEV100 -- the engine's OWN exposure<->EV100 conversion, public and
// inline, used here rather than re-derived so `ev100Equivalent` cannot drift from the renderer.
#include "RenderUtils.h"
#include "Rendering/SlateRenderer.h"
// FSceneViewStateInterface::GetLastEyeAdaptationExposure -- the renderer's own record of the
// exposure it last resolved for this viewport, used as the capture's drift signal.
#include "SceneManagement.h"
#include "SceneView.h"
#include "SceneUtils.h"
// FEngineShowFlags -- reached through FEditorViewportClient::EngineShowFlags, but included by name
// because this file reads and writes the BillboardSprites bit directly.
#include "ShowFlags.h"
#include "Slate/SceneViewport.h"
#include "UObject/UObjectIterator.h"
#include "UnrealClient.h"
// GetCachedScalabilityCVars() -- the struct the renderer actually reads its ViewDistanceScale
// from, as opposed to the cvar the capture writes.
#include "UnrealEngine.h"

namespace PinWrightRenderCapture
{
struct FCaptureActorLockState
{
    FLevelEditorViewportClient* Client = nullptr;
    FLevelViewportActorLock ActorLock;
    FLevelViewportActorLock CinematicActorLock;
};

namespace
{
    constexpr int32 MaxCaptureDimension = 16384;

    EAntiAliasingMethod EffectiveStillAntiAliasingMethod(
        const FEditorViewportClient& ViewportClient)
    {
        const FEngineShowFlags& Flags = ViewportClient.EngineShowFlags;
        const FStaticFeatureLevel FeatureLevel = ViewportClient.GetWorld()
            ? FStaticFeatureLevel(ViewportClient.GetWorld()->GetFeatureLevel())
            : FStaticFeatureLevel(GMaxRHIFeatureLevel);
        EAntiAliasingMethod Method = GetDefaultAntiAliasingMethod(FeatureLevel);
        if (!Flags.PostProcessing || !Flags.AntiAliasing)
        {
            return AAM_None;
        }
        if ((Method == AAM_TemporalAA || Method == AAM_TSR) && !Flags.TemporalAA)
        {
            return AAM_FXAA;
        }
        return Method;
    }

    bool IsValidProjectionMode(const FString& ProjectionMode)
    {
        return ProjectionMode == TEXT("perspective") || ProjectionMode == TEXT("orthographic");
    }

    struct FOrthoViewportTypeEntry
    {
        ELevelViewportType Type;
        FVector Forward;
        FRotator EffectiveRotation;
        const TCHAR* Name;
    };

    // Derived from the constant ViewRotationMatrix the engine builds per orthographic viewport
    // type in FEditorViewportClient::CalcSceneView (UE 5.8
    // Engine/Source/Editor/UnrealEd/Private/EditorViewportClient.cpp:1341-1401). UE uses the
    // row-vector convention, so matrix column 0 is screen right, column 1 screen up and column 2
    // the camera forward, all expressed in world space; EffectiveRotation is the FRotator with
    // exactly that basis, i.e. the pose the rendered pixels show. Cross-checked against the
    // perspective branch (:1253-1260), which yields right=+Y / up=+Z / forward=+X for
    // FRotator(0,0,0).
    //
    // The in-plane (roll/yaw) column of this table is engine-version-sensitive, and the split
    // below is measured rather than assumed: UE 5.6 rotated the Top/Bottom ViewRotationMatrices
    // 90 degrees in-plane. On 5.3-5.5 LVT_OrthoXY is diag(+X right, -Y up, -Z forward) - so world
    // +X runs RIGHT across the image and world +Y runs DOWN - while 5.6+ makes screen right -Y
    // and screen up -X, putting +X DOWN and +Y LEFT. LVT_OrthoNegativeXY moved with it. The four
    // side views are byte-identical on every supported engine and need no branch.
    // PinWright.spatial.view_projection.OrthoTopDownOrientation asserts the live basis against
    // this table on whatever engine it runs; if it fails, the matrices moved again and BOTH
    // this table and that test's per-version expectation need the new row.
    const FOrthoViewportTypeEntry GOrthoViewportTypes[] =
    {
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 6, 0)
        { LVT_OrthoXY,         FVector( 0.0,  0.0, -1.0), FRotator(-90.0f, 180.0f, 0.0f), TEXT("top")    },
        { LVT_OrthoNegativeXY, FVector( 0.0,  0.0,  1.0), FRotator( 90.0f,   0.0f, 0.0f), TEXT("bottom") },
#else
        { LVT_OrthoXY,         FVector( 0.0,  0.0, -1.0), FRotator(-90.0f, -90.0f, 0.0f), TEXT("top")    },
        { LVT_OrthoNegativeXY, FVector( 0.0,  0.0,  1.0), FRotator( 90.0f,  90.0f, 0.0f), TEXT("bottom") },
#endif
        { LVT_OrthoYZ,         FVector( 1.0,  0.0,  0.0), FRotator(  0.0f,   0.0f, 0.0f), TEXT("back")   },
        { LVT_OrthoNegativeYZ, FVector(-1.0,  0.0,  0.0), FRotator(  0.0f, 180.0f, 0.0f), TEXT("front")  },
        { LVT_OrthoNegativeXZ, FVector( 0.0,  1.0,  0.0), FRotator(  0.0f,  90.0f, 0.0f), TEXT("right")  },
        { LVT_OrthoXZ,         FVector( 0.0, -1.0,  0.0), FRotator(  0.0f, -90.0f, 0.0f), TEXT("left")   },
    };

    // Advance the editor by one frame FOR THE CAPTURE, without letting the capture drive the
    // editor's INTERACTIVE machinery. Two things this deliberately no longer does, each of which
    // put the plugin on the editor's hit-proxy path -- a buffer whose only purpose is to answer
    // "what primitive is under this pixel" and which no screenshot needs. Together they are
    // B-capture-open-level-hitproxy-colorrt-assert-kills-editor: a render-thread
    // `Assertion failed: ColorRT` (RHIResources.h:5395) that appErrors the whole shared editor.
    //
    // 1. IT DOES NOT PROCESS INPUT. This used to call SlateApp.PumpMessages() and tick with
    //    ESlateTickType::All. `All` adds the PlatformAndInput phase (the phase split is
    //    SlateApplication.cpp:1648-1667), whose FSlateUser::UpdateCursor() (:1725) runs a cursor
    //    query into the widget under the mouse -> FSceneViewport::OnCursorQuery
    //    (SceneViewport.cpp:604-622) -> FLevelEditorViewportClient::GetCursor
    //    (LevelEditorViewport.cpp:4727) -> FViewport::GetHitProxy -> GetRawHitProxyData. So a
    //    capture of the open level issued the editor's hit-proxy readback once per pump, on the
    //    viewport this call has just resized. PumpMessages() is worse than redundant: on Windows
    //    it DISPATCHES, and FWindowsApplication::DeferMessage only defers while
    //    GPumpingMessagesOutsideOfMainLoop (WindowsApplication.cpp:4190), so a real click that
    //    landed mid-capture ran ProcessClick against a viewport in the capture's transient state.
    //    Time|Widgets is everything a frame needs -- the viewport client still ticks and the
    //    widget tree still paints; queued OS input is simply delivered by the editor's own loop
    //    after the capture returns.
    //
    // 2. IT DOES NOT INVALIDATE HIT PROXIES. FEditorViewportClient::Invalidate() and
    //    FViewport::Invalidate() both default to invalidating the hit-proxy map ALONG WITH the
    //    display; the engine's own two-way split is Invalidate(_, /*bInvalidateHitProxies=*/false)
    //    / InvalidateDisplay(), commented "Invalidate only display pixels"
    //    (EditorViewportClient.cpp:6622-6637). A capture wants the FRAME redrawn, never the
    //    editor's pick buffer rebuilt -- and dirtying it is what ARMS the crash.
    //    FViewport::GetRawHitProxyData (UnrealClient.cpp:1923) returns its cache untouched while
    //    bHitProxiesCached holds (:1930-1941); only a dirty cache takes the regeneration branch,
    //    which builds an FRHIRenderPassInfo from HitProxyMap.GetRenderTargetTexture() (:1949-1960).
    //    That render target is released and rebuilt around a FlushRenderingCommands every time
    //    CaptureEditorViewportToPng calls SetFixedViewportSize (FSceneViewport::UpdateViewportRHI,
    //    SceneViewport.cpp:2136-2137 -> FViewport::ReleaseRHI -> HitProxyMap.Release(),
    //    UnrealClient.cpp:2261-2264), so a regeneration landing in that window reads a null colour
    //    target and asserts on the RENDERING thread. It is also why the ticket's reporters could
    //    only reproduce after a level.load or a spawn: those invalidate hit proxies too, and a
    //    hand-driven capture with seconds of think-time in front of it survived because the
    //    editor's own tick had already re-warmed the cache.
    //
    // The RESTORE side of a capture still invalidates hit proxies in full, and must: the pose,
    // show flags and viewport size all moved while we held it, so the editor's pick buffer really
    // is stale when it is handed back. That happens with no resize in flight, which is the whole
    // difference.
    void PumpViewport(FEditorViewportClient& ViewportClient, const TSharedPtr<FSceneViewport>& SceneViewport)
    {
        FSlateApplication& SlateApp = FSlateApplication::Get();
        SlateApp.Tick(ESlateTickType::TimeAndWidgets);

        if (UWorld* World = ViewportClient.GetWorld())
        {
            World->SendAllEndOfFrameUpdates();
        }

        ViewportClient.Invalidate(/*bInvalidateChildViews=*/true, /*bInvalidateHitProxies=*/false);
        if (SceneViewport.IsValid())
        {
            SceneViewport->InvalidateDisplay();
            SceneViewport->Draw();
        }

        if (FSlateRenderer* Renderer = SlateApp.GetRenderer())
        {
            Renderer->FlushCommands();
        }
        FlushRenderingCommands();
    }

    const TCHAR* ViewDistanceScaleCVarName = TEXT("r.ViewDistanceScale");

    // Force r.ViewDistanceScale for the life of the capture and put it back afterwards.
    //
    // Two engine details make this less trivial than Set/Set-back:
    //  1. The renderer does not read the cvar. It reads GetCachedScalabilityCVars().ViewDistanceScale,
    //     a cache refreshed by ScalabilityCVarsSinkCallback, which only runs from a console-variable
    //     SINK (UE 5.8 Runtime/Engine/Private/UnrealEngine.cpp:869, registered at :1275). Sinks fire
    //     from the engine tick -- which does NOT happen between a handler setting a cvar and the same
    //     handler drawing the viewport. Without the explicit CallAllConsoleVariableSinks() below, the
    //     override would be a no-op that reported success, which is the exact failure class this
    //     plugin has the longest list of.
    //  2. Setting at ECVF_SetByCode would leave the variable pinned at Code priority after the
    //     restore, silently outranking any later scalability change for the rest of the session. Both
    //     writes therefore go in at the priority the variable ALREADY has, which is always accepted
    //     (the engine's guard is >=, not >) and never raises it.
    class FScopedViewDistanceScale
    {
    public:
        explicit FScopedViewDistanceScale(float InNewScale)
        {
            CVar = IConsoleManager::Get().FindConsoleVariable(ViewDistanceScaleCVarName);
            if (!CVar)
            {
                return;
            }
            PreviousScale = CVar->GetFloat();
            AppliedScale = PreviousScale;
            if (InNewScale <= 0.0f || FMath::IsNearlyEqual(PreviousScale, InNewScale, UE_KINDA_SMALL_NUMBER))
            {
                // Nothing to do. Leaving CVar set but bApplied false keeps the read-back in
                // IsRestored() honest without writing anything.
                return;
            }
            SetBy = static_cast<EConsoleVariableFlags>(CVar->GetFlags() & ECVF_SetByMask);
            CVar->Set(InNewScale, SetBy);
            IConsoleManager::Get().CallAllConsoleVariableSinks();
            AppliedScale = CVar->GetFloat();
            bApplied = true;
        }

        ~FScopedViewDistanceScale()
        {
            if (!CVar || !bApplied)
            {
                return;
            }
            CVar->Set(PreviousScale, SetBy);
            IConsoleManager::Get().CallAllConsoleVariableSinks();
        }

        FScopedViewDistanceScale(const FScopedViewDistanceScale&) = delete;
        FScopedViewDistanceScale& operator=(const FScopedViewDistanceScale&) = delete;

        bool WasApplied() const { return bApplied; }
        float GetPreviousScale() const { return PreviousScale; }
        // The cvar READ BACK after the write, not the value that was requested. The two differ
        // if the engine clamped or refused it, and reporting the requested value would be the
        // capture claiming an override it did not get. The matching restore verdict cannot be
        // taken here (the object is still alive); the caller reads ReadViewDistanceScale() from
        // a scope guard declared before this one, so it runs after this destructor.
        float GetAppliedScale() const { return AppliedScale; }

    private:
        IConsoleVariable* CVar = nullptr;
        float PreviousScale = 1.0f;
        float AppliedScale = 1.0f;
        EConsoleVariableFlags SetBy = ECVF_SetByCode;
        bool bApplied = false;
    };

    // Applying the cvar is not enough for INSTANCED meshes, and this is the half of the override
    // that silently did nothing. A component's per-instance FAR cull distance is not re-read per
    // frame the way a primitive's MaxDrawDistance is: it is multiplied by the scalability scale
    // ONCE, when the primitive uniform buffer is built
    // (PrimitiveUniformShaderParameters.cpp: `DistanceMinMax.Y *=
    // GetCachedScalabilityCVars().ViewDistanceScale`, feeding InstanceDrawDistanceMinMaxSquared).
    // A proxy built before the override therefore keeps the OLD scale's cull radius for the whole
    // capture, so the forest the derivation was computed for stays culled while the response
    // reports overridden:true -- the exact silent-missing-content failure the derivation exists to
    // remove. Marking those components' render state dirty and flushing the deferred updates
    // rebuilds the buffers against the scale now in force.
    //
    // Only components that actually carry an end cull distance are touched: for every other
    // primitive the scale is applied at cull time and no rebuild is needed.
    void RebakeInstancedCullDistances(UWorld* World)
    {
        if (!World)
        {
            return;
        }
        bool bAnyDirtied = false;
        for (TObjectIterator<UInstancedStaticMeshComponent> It; It; ++It)
        {
            UInstancedStaticMeshComponent* Component = *It;
            if (!IsValid(Component) || Component->IsTemplate() || Component->GetWorld() != World ||
                !Component->IsRegistered())
            {
                continue;
            }
            int32 StartCull = 0;
            int32 EndCull = 0;
            Component->GetCullDistances(StartCull, EndCull);
            if (EndCull <= 0)
            {
                continue;
            }
            Component->MarkRenderStateDirty();
            bAnyDirtied = true;
        }
        if (bAnyDirtied)
        {
            // MarkRenderStateDirty only queues the recreate; without this the rebuilt buffers
            // would land a frame late, which for a single-frame capture means never.
            World->SendAllEndOfFrameUpdates();
        }
    }

    // Rebakes once the override is in force, and again after it is restored so the level is left
    // with the cull radii it had. Declared BEFORE FScopedViewDistanceScale at the call site so its
    // destructor runs AFTER the cvar has gone back.
    class FScopedInstancedCullRebake
    {
    public:
        explicit FScopedInstancedCullRebake(UWorld* InWorld) : World(InWorld) {}

        void Rebake()
        {
            bArmed = true;
            RebakeInstancedCullDistances(World);
        }

        ~FScopedInstancedCullRebake()
        {
            if (bArmed)
            {
                RebakeInstancedCullDistances(World);
            }
        }

        FScopedInstancedCullRebake(const FScopedInstancedCullRebake&) = delete;
        FScopedInstancedCullRebake& operator=(const FScopedInstancedCullRebake&) = delete;

    private:
        UWorld* World = nullptr;
        bool bArmed = false;
    };

    // Force the editor viewport's exposure override for the life of the capture and put it back.
    //
    // TWO WRITES, NOT ONE, AND THE SECOND IS THE LOAD-BEARING ONE.
    //
    //  1. `ExposureSettings = {bFixed:true, FixedEV100:N}` is the editor's own override slot
    //     (EditorViewportClient.h:1986). FEditorViewportClient::Draw copies it into the view
    //     family (EditorViewportClient.cpp:4876) and the renderer clamps min and max white-point
    //     luminance to one value derived from it (PostProcessEyeAdaptation.cpp:641 -> :516). That
    //     branch sits above the PostProcessVolume path in the same if/else chain, so the pin is
    //     immune to whatever the level's volumes carry.
    //
    //  2. `EngineShowFlags.EyeAdaptation = false`. Without it the pin is a TARGET, not a value:
    //     the eye-adaptation pass still blends toward it over frames, because ForceTarget
    //     (PostProcessEyeAdaptation.cpp:721) is only set for a camera cut, AEM_Manual, or an
    //     already-degenerate AutoExposureMin/MaxBrightness range -- none of which bFixed implies.
    //     Two captures taken back to back with the SAME pin would then differ, each one starting
    //     from where the previous left the adaptation, which defeats the entire point. With the
    //     show flag clear the tonemapper takes its exposure from a constant buffer built by
    //     GetEyeAdaptationFixedExposure (PostProcessTonemap.cpp:623-632), which under bFixed
    //     evaluates to exactly 1/CalculateFixedAutoExposure -- no temporal term, no scene
    //     dependence, identical on frame 1 and frame N. Clearing the flag alone would NOT pin
    //     anything (it just forces AEM_Manual); the two writes are only useful together, and
    //     bFixed still wins the if/else chain because it is tested first (:641 before :650).
    //
    // Restores both on every exit path including error returns. The restore is verified by a
    // read-back in the caller, not assumed here.
    class FScopedExposurePin
    {
    public:
        FScopedExposurePin(FEditorViewportClient& InClient, const FExposurePin& Pin)
            : Client(InClient)
        {
            PreviousSettings = Client.ExposureSettings;
            bPreviousEyeAdaptation = Client.EngineShowFlags.EyeAdaptation != 0;
            if (!Pin.WantsPin())
            {
                // Nothing is read or written. This is the omitted-parameter path and it must stay
                // byte-for-byte identical to the behaviour before this parameter existed.
                return;
            }
            Reassert(Pin.Ev100);
            // Display only, for the reason spelled out over PumpViewport: nothing inside a capture
            // picks, and a dirty hit-proxy cache is what arms the ColorRT assert. The destructor
            // below still invalidates in full -- that is the hand-back.
            Client.Invalidate(/*bInvalidateChildViews=*/true, /*bInvalidateHitProxies=*/false);
            bApplied = true;
        }

        ~FScopedExposurePin()
        {
            if (!bApplied)
            {
                return;
            }
            Client.ExposureSettings = PreviousSettings;
            Client.EngineShowFlags.SetEyeAdaptation(bPreviousEyeAdaptation);
            Client.Invalidate();
        }

        FScopedExposurePin(const FScopedExposurePin&) = delete;
        FScopedExposurePin& operator=(const FScopedExposurePin&) = delete;

        bool WasApplied() const { return bApplied; }
        const FExposureSettings& GetPreviousSettings() const { return PreviousSettings; }
        bool GetPreviousEyeAdaptation() const { return bPreviousEyeAdaptation; }
        void Reassert(float Ev100)
        {
            Client.ExposureSettings.bFixed = true;
            Client.ExposureSettings.FixedEV100 = Ev100;
            Client.EngineShowFlags.SetEyeAdaptation(false);
        }

    private:
        FEditorViewportClient& Client;
        FExposureSettings PreviousSettings;
        bool bPreviousEyeAdaptation = true;
        bool bApplied = false;
    };

    // Clear the editor icon sprites for the life of the capture and put the flag back.
    //
    // WHY THE FLAG AND NOT GAME VIEW. FEditorViewportClient::SetGameView is the editor's own
    // "hide editor-only decoration" lever (the G key) and it is the wrong tool for a capture that
    // has to leave the viewport as it found it. It SWAPS EngineShowFlags with LastEngineShowFlags
    // rather than setting anything (EditorViewportClient.cpp:7221-7252), and which set it treats
    // as "the game set" depends on whichever of the two already carries Game -- the plugin has
    // already measured that reusing a saved set can leave Splines on after a confirmed enable
    // (editor.set_game_view's overlayWarning). It also calls ShowWidget(false) and re-derives
    // SelectionOutline from user settings, neither of which a toggle back restores. A capture
    // guard cannot round-trip that; one bit can.
    //
    // And SetGameView(true) would not even do the job on its own terms: BillboardSprites is not
    // touched by FEngineShowFlags::Init at all, so it is ON in the game set as well. Game view
    // hides icons via FPrimitiveSceneProxy::IsShown's Editor/Game branch
    // (PrimitiveSceneProxy.cpp:1515-1545), which is a different mechanism with a different blast
    // radius. The flag is the narrow, restorable one.
    class FScopedEditorSpriteSuppression
    {
    public:
        FScopedEditorSpriteSuppression(FEditorViewportClient& InClient, bool bHide)
            : Client(InClient)
        {
            PreviousFlags = ReadEditorSpriteShowFlags(Client.EngineShowFlags);
            if (!bHide)
            {
                // Nothing is written. This is the omitted-parameter path and it must stay
                // byte-for-byte identical to the behaviour before this parameter existed.
                return;
            }
            SuppressEditorSpriteShowFlags(Client.EngineShowFlags);
            // Display only while the capture holds the viewport -- see PumpViewport.
            Client.Invalidate(/*bInvalidateChildViews=*/true, /*bInvalidateHitProxies=*/false);
            bApplied = true;
        }

        ~FScopedEditorSpriteSuppression()
        {
            if (!bApplied)
            {
                return;
            }
            RestoreEditorSpriteShowFlags(Client.EngineShowFlags, PreviousFlags);
            Client.Invalidate();
        }

        FScopedEditorSpriteSuppression(const FScopedEditorSpriteSuppression&) = delete;
        FScopedEditorSpriteSuppression& operator=(const FScopedEditorSpriteSuppression&) = delete;

        bool WasApplied() const { return bApplied; }
        const FEditorSpriteShowFlags& GetPreviousFlags() const { return PreviousFlags; }

    private:
        FEditorViewportClient& Client;
        FEditorSpriteShowFlags PreviousFlags;
        bool bApplied = false;
    };

    // Does the renderer APPLY a fixed-exposure override to a frame drawn in this state?
    //
    // The whole ExposureSettings chain is the `else` of IsAutoExposureDebugMode
    // (PostProcessEyeAdaptation.cpp:493-511 -> :639), so a debug or unlit frame writes the pin and
    // renders as if it were never there. Rather than copy that eleven-term predicate -- a second
    // copy of an engine rule is what silently drifts -- this tests the two conditions a capture
    // verb can actually put a viewport in, and defers the rest to the lit-ness classifier the
    // capture already computes and already reports.
    //
    // Conservative by construction: a mode outside VMI_Lit / VMI_PathTracing might still honour
    // the pin, and reporting `pinned:false` there costs one line of explanation, while a spurious
    // `pinned:true` is the exact defect this parameter exists to remove.
    bool ExposurePinGovernsFrame(const FEditorViewportClient& Client, bool bLitViewMode,
        FString& OutBlockedReason)
    {
        OutBlockedReason.Reset();
        if (!Client.EngineShowFlags.PostProcessing)
        {
            OutBlockedReason = TEXT("the PostProcessing show flag is off on this viewport, and "
                "IsAutoExposureDebugMode (PostProcessEyeAdaptation.cpp:511) short-circuits the "
                "whole exposure override on `!EngineShowFlags.PostProcessing`");
            return false;
        }
        if (!Client.EngineShowFlags.Lighting)
        {
            OutBlockedReason = TEXT("the Lighting show flag is off on this viewport, and "
                "IsAutoExposureDebugMode (PostProcessEyeAdaptation.cpp:497) short-circuits the "
                "exposure override on `!EngineShowFlags.Lighting`");
            return false;
        }
        if (!bLitViewMode)
        {
            OutBlockedReason = TEXT("the capture ran in a non-lit view mode, which the renderer "
                "treats as a debug view (IsAutoExposureDebugMode, "
                "PostProcessEyeAdaptation.cpp:493-511) and where the fixed-exposure override is "
                "not applied at all");
            return false;
        }
        return true;
    }

    // The renderer's last completed eye-adaptation readback for this viewport, or 0 when there is
    // none. Reached through the client's own FSceneViewStateReference rather than by building a
    // throwaway FSceneView: the value lives on the persistent view state, so the cheap path and
    // the expensive path would read the same number.
    // Non-const client: FSceneViewStateReference::GetReference (SceneTypes.h:75) is non-const.
    float ReadLastAdaptedExposure(FEditorViewportClient& Client)
    {
        if (const FSceneViewStateInterface* ViewState = Client.ViewState.GetReference())
        {
            return ViewState->GetLastEyeAdaptationExposure();
        }
        return 0.0f;
    }

    float ReadViewDistanceScale()
    {
        if (IConsoleVariable* CVar = IConsoleManager::Get().FindConsoleVariable(ViewDistanceScaleCVarName))
        {
            return CVar->GetFloat();
        }
        return 1.0f;
    }

    FString ViewportTypeName(ELevelViewportType ViewportType)
    {
        switch (ViewportType)
        {
        case LVT_OrthoXY: return TEXT("OrthoTop");
        case LVT_OrthoXZ: return TEXT("OrthoLeft");
        case LVT_OrthoYZ: return TEXT("OrthoBack");
        case LVT_Perspective: return TEXT("Perspective");
        case LVT_OrthoFreelook: return TEXT("OrthoFreelook");
        case LVT_OrthoNegativeXY: return TEXT("OrthoBottom");
        case LVT_OrthoNegativeXZ: return TEXT("OrthoRight");
        case LVT_OrthoNegativeYZ: return TEXT("OrthoFront");
        case LVT_None: return TEXT("None");
        default: return TEXT("Unknown");
        }
    }

    TSet<const FSceneViewport*>& ActiveViewportCaptureContexts()
    {
        static TSet<const FSceneViewport*> Contexts;
        return Contexts;
    }

    const TCHAR* QuantizationDitherCVarName =
        TEXT("r.BackbufferQuantizationDitheringOverride");

    // A bit depth no backbuffer has, so 1/(2^N - 1) lands ~5 orders of magnitude below one 8-bit
    // level and the grain the shader still adds cannot move a quantised channel.
    constexpr int32 QuantizationDitherSuppressedBitDepth = 24;

    // Suppress the tonemapper's backbuffer quantization dither for the life of the capture.
    //
    // WHY A CAPTURE HAS TO. The dither is not noise the renderer adds once: its offset is
    // `Halton(ViewState->GetOutputFrameIndex(8))` (PostProcessTonemap.cpp:753-759 feeding
    // PostProcessTonemap.usf:254, applied unconditionally at :600), so the pattern is a function of
    // HOW MANY FRAMES THIS VIEWPORT HAS DRAWN. Two shots of one unmoved pose therefore differ by
    // design -- measured on an eight-shot identical-pose set: a triangular +/-1 level on a THIRD of
    // every frame, and, because FXAA runs on the dithered LDR image and its edge decision flips
    // with it, up to 17 levels on individual high-contrast pixels. That is the failure the exposure
    // pin above exists to remove, arriving through a different term.
    //
    // The engine offers no off switch, only a bit-depth override, which is what this uses. Set at
    // the priority the variable already carries, for the reason spelled out over
    // FScopedViewDistanceScale -- SetByCode would outrank any later scalability change for the rest
    // of the session. No console-variable sink is involved (the renderer reads this one directly),
    // but the write does have to reach the RENDER thread before the draws it governs: the variable
    // is ECVF_RenderThreadSafe, so the propagation is deferred as a render command in order with
    // the capture's own (ConsoleManager.cpp:487-490), and the restore lands after them the same
    // way.
    class FScopedQuantizationDitherPin
    {
    public:
        FScopedQuantizationDitherPin()
        {
            CVar = IConsoleManager::Get().FindConsoleVariable(QuantizationDitherCVarName);
            if (!CVar)
            {
                return;
            }
            PreviousBitDepth = CVar->GetInt();
            if (PreviousBitDepth >= QuantizationDitherSuppressedBitDepth)
            {
                // Already past the point where the grain can move an 8-bit level. Leaving
                // bApplied false keeps the restore from writing a value nothing here set.
                return;
            }
            SetBy = static_cast<EConsoleVariableFlags>(CVar->GetFlags() & ECVF_SetByMask);
            CVar->Set(QuantizationDitherSuppressedBitDepth, SetBy);
            bApplied = CVar->GetInt() == QuantizationDitherSuppressedBitDepth;
        }

        ~FScopedQuantizationDitherPin()
        {
            if (!CVar || !bApplied)
            {
                return;
            }
            CVar->Set(PreviousBitDepth, SetBy);
        }

        FScopedQuantizationDitherPin(const FScopedQuantizationDitherPin&) = delete;
        FScopedQuantizationDitherPin& operator=(const FScopedQuantizationDitherPin&) = delete;

    private:
        IConsoleVariable* CVar = nullptr;
        int32 PreviousBitDepth = 0;
        EConsoleVariableFlags SetBy = ECVF_SetByCode;
        bool bApplied = false;
    };

}

struct FViewportCaptureSetContext::FImpl
{
    FImpl(FEditorViewportClient& InViewportClient,
        const TSharedPtr<FSceneViewport>& InSceneViewport,
        const FExposurePin& InExposure,
        FViewportCaptureRestoreReceipt& InRestoreReceipt)
        : ViewportClient(InViewportClient)
        , SceneViewport(InSceneViewport)
        , Exposure(InExposure)
        , RestoreReceipt(InRestoreReceipt)
        , OriginalSize(InSceneViewport.IsValid()
            ? InSceneViewport->GetSizeXY() : FIntPoint::ZeroValue)
        , bOriginalFixed(InSceneViewport.IsValid() && InSceneViewport->HasFixedSize())
    {
    }

    FEditorViewportClient& ViewportClient;
    TSharedPtr<FSceneViewport> SceneViewport;
    FExposurePin Exposure;
    FViewportCaptureRestoreReceipt& RestoreReceipt;
    FIntPoint OriginalSize = FIntPoint::ZeroValue;
    TUniquePtr<FScopedExposurePin> ExposureScope;
    TUniquePtr<FScopedQuantizationDitherPin> QuantizationDitherScope;
    TSharedPtr<FCaptureResolutionViewExtension, ESPMode::ThreadSafe> ResolutionScope;
    bool bOriginalFixed = false;
    bool bTemporalAntiAliasingBefore = false;
    bool bRegistered = false;
    bool bArmed = false;
};

FViewportCaptureSetContext::FViewportCaptureSetContext(
    FEditorViewportClient& ViewportClient,
    const TSharedPtr<FSceneViewport>& SceneViewport,
    const FExposurePin& Exposure,
    FViewportCaptureRestoreReceipt& OutRestoreReceipt)
    : Impl(MakeUnique<FImpl>(ViewportClient, SceneViewport, Exposure, OutRestoreReceipt))
{
    OutRestoreReceipt = FViewportCaptureRestoreReceipt();
    if (SceneViewport.IsValid()
        && !ActiveViewportCaptureContexts().Contains(SceneViewport.Get()))
    {
        ActiveViewportCaptureContexts().Add(SceneViewport.Get());
        Impl->bRegistered = true;
    }
}

FViewportCaptureSetContext::~FViewportCaptureSetContext()
{
    if (!Impl)
    {
        return;
    }

    FImpl& State = *Impl;
    if (State.bArmed)
    {
        const FExposureSettings ExposureBefore =
            State.ExposureScope->GetPreviousSettings();
        const bool bCanRestorePositiveSize =
            State.OriginalSize.X > 0 && State.OriginalSize.Y > 0;
        if (bCanRestorePositiveSize
            && (!State.SceneViewport->HasFixedSize()
                || State.SceneViewport->GetSizeXY() != State.OriginalSize))
        {
            // A positive SetFixedViewportSize draws synchronously. Keep exposure, TemporalAA and
            // the resolution extension pinned until that hand-back draw has completed.
            State.SceneViewport->SetFixedViewportSize(
                static_cast<uint32>(State.OriginalSize.X),
                static_cast<uint32>(State.OriginalSize.Y));
        }
        if (!State.bOriginalFixed && State.SceneViewport->HasFixedSize())
        {
            State.SceneViewport->SetFixedViewportSize(0, 0);
        }

        State.ResolutionScope.Reset();
        State.QuantizationDitherScope.Reset();
        State.ViewportClient.EngineShowFlags.SetTemporalAA(
            State.bTemporalAntiAliasingBefore);
        State.ExposureScope.Reset();

        State.RestoreReceipt.bExposureRestored =
            State.ViewportClient.ExposureSettings.bFixed == ExposureBefore.bFixed
            && FMath::IsNearlyEqual(State.ViewportClient.ExposureSettings.FixedEV100,
                ExposureBefore.FixedEV100, UE_KINDA_SMALL_NUMBER);
        State.RestoreReceipt.bTemporalAntiAliasingRestored =
            (State.ViewportClient.EngineShowFlags.TemporalAA != 0)
                == State.bTemporalAntiAliasingBefore;
        State.RestoreReceipt.bViewportSizeAndFixedStateRestored =
            State.SceneViewport->HasFixedSize() == State.bOriginalFixed
            && (!bCanRestorePositiveSize
                || State.SceneViewport->GetSizeXY() == State.OriginalSize);
    }

    if (State.bRegistered)
    {
        ActiveViewportCaptureContexts().Remove(State.SceneViewport.Get());
    }
}

bool FViewportCaptureSetContext::IsValid() const
{
    return Impl && Impl->bRegistered;
}

void FViewportCaptureSetContext::PrepareForDraw(uint32 Width, uint32 Height)
{
    if (!IsValid())
    {
        return;
    }

    FImpl& State = *Impl;
    if (!State.bArmed)
    {
        State.ExposureScope = MakeUnique<FScopedExposurePin>(
            State.ViewportClient, State.Exposure);
        State.bTemporalAntiAliasingBefore =
            State.ViewportClient.EngineShowFlags.TemporalAA != 0;
        State.ViewportClient.EngineShowFlags.SetTemporalAA(false);
        // Armed with the other render pins and held for the whole set, so every shot in it is
        // dithered identically rather than by its own frame index.
        State.QuantizationDitherScope = MakeUnique<FScopedQuantizationDitherPin>();
        State.ResolutionScope =
            FSceneViewExtensions::NewExtension<FCaptureResolutionViewExtension>(
                State.SceneViewport.Get());
        State.bArmed = true;
    }
    else
    {
        ReassertPins();
    }

    const FIntPoint RequestedSize(static_cast<int32>(Width), static_cast<int32>(Height));
    if (!State.SceneViewport->HasFixedSize()
        || State.SceneViewport->GetSizeXY() != RequestedSize)
    {
        State.SceneViewport->SetFixedViewportSize(Width, Height);
    }
}

void FViewportCaptureSetContext::ReassertPins()
{
    if (!Impl || !Impl->bArmed)
    {
        return;
    }

    if (Impl->Exposure.WantsPin())
    {
        Impl->ExposureScope->Reassert(Impl->Exposure.Ev100);
    }
    Impl->ViewportClient.EngineShowFlags.SetTemporalAA(false);
}

bool FViewportCaptureSetContext::PopulateFrameEvidence(
    FViewportCaptureOutput& OutCapture) const
{
    if (!Impl || !Impl->bArmed || !Impl->ExposureScope)
    {
        return false;
    }

    OutCapture.ExposureMode = Impl->Exposure.Mode;
    OutCapture.bExposurePinRequested = Impl->Exposure.WantsPin();
    OutCapture.Ev100Requested = Impl->Exposure.WantsPin() ? Impl->Exposure.Ev100 : 0.0f;
    OutCapture.bExposureFixedBefore = Impl->ExposureScope->GetPreviousSettings().bFixed;
    OutCapture.Ev100Before = Impl->ExposureScope->GetPreviousSettings().FixedEV100;
    OutCapture.bExposureFixedApplied = Impl->ViewportClient.ExposureSettings.bFixed;
    OutCapture.Ev100Applied = Impl->ViewportClient.ExposureSettings.FixedEV100;
    OutCapture.bTemporalAntiAliasingSuppressed =
        Impl->ViewportClient.EngineShowFlags.TemporalAA == 0;
    return Impl->ResolutionScope.IsValid()
        && Impl->ResolutionScope->PopulateCaptureOutput(OutCapture);
}

FCaptureImageStats CalculateCaptureImageStats(TConstArrayView<FColor> ColorData)
{
    FCaptureImageStats Stats;
    if (ColorData.IsEmpty())
    {
        Stats.bBlank = true;
        return Stats;
    }

    double Sum = 0.0;
    double SumSquares = 0.0;
    int64 LitPixels = 0;
    Stats.MinLuminance = 1.0;
    // Population per 8-bit luminance level, for the tone-range criterion. 256 bins because 256 is
    // what the PNG the caller receives can carry -- the range being measured is the range of the
    // ARTIFACT, not of the scene behind it.
    int64 ToneHistogram[256] = {};
    for (const FColor& Color : ColorData)
    {
        const double Luminance =
            (0.2126 * static_cast<double>(Color.R) +
             0.7152 * static_cast<double>(Color.G) +
             0.0722 * static_cast<double>(Color.B)) / 255.0;
        Sum += Luminance;
        SumSquares += Luminance * Luminance;
        Stats.MinLuminance = FMath::Min(Stats.MinLuminance, Luminance);
        Stats.MaxLuminance = FMath::Max(Stats.MaxLuminance, Luminance);
        LitPixels += (Luminance > BlankLitLuminanceThreshold) ? 1 : 0;
        ++ToneHistogram[FMath::Clamp(FMath::RoundToInt32(Luminance * 255.0), 0, 255)];
    }

    const double Count = static_cast<double>(ColorData.Num());
    Stats.MeanLuminance = Sum / Count;
    Stats.LuminanceVariance = FMath::Max(0.0, (SumSquares / Count) -
        (Stats.MeanLuminance * Stats.MeanLuminance));
    Stats.LitPixelCount = LitPixels;
    Stats.LitPixelFraction = static_cast<double>(LitPixels) / Count;

    // This is intentionally a black-frame diagnostic, not a generic "boring image" detector.
    // A valid flat-colour or dark scene is accepted unless nothing at all was drawn into it.
    // Deliberately black scenes remain available through allowBlank:true.
    //
    // The verdict is RESOLUTION-INVARIANT by construction, which the previous mean+variance form
    // was not (see the measurement table above BlankLitLuminanceThreshold in the header). Variance
    // scales with the SHARE of the frame that carries content, so a fixed-pixel-size editor overlay
    // contributed four times less variance at 1024 than at 512 and flipped the verdict on identical
    // content. Here the two regimes are covered by the statistic that is stable in each:
    //   - real scene content scales with the frame, so its lit FRACTION is what holds;
    //   - a fixed-pixel overlay does not, so its lit COUNT is what holds.
    // Admitting either as evidence leaves no content whose verdict moves with the capture size.
    //
    // Below ~128k pixels the fraction term is the more permissive of the two and carries small
    // frames; above it the count term does. Both floors are at least one pixel, so a frame in which
    // literally nothing is lit stays blank at every size.
    const int64 LitPixelFloor = FMath::Min<int64>(
        BlankMinLitPixels,
        FMath::Max<int64>(1, static_cast<int64>(FMath::CeilToDouble(BlankMinLitFraction * Count))));
    Stats.bBlank = Stats.MeanLuminance <= BlankMeanLuminance && Stats.LitPixelCount < LitPixelFloor;

    // --- tone range, a DIFFERENT question from `blank` (see the header for why it is separate) ---
    //
    // `blank` asks whether anything was drawn. This asks whether what was drawn can be read: how
    // many 8-bit luminance levels the frame actually resolves. The per-level floor admits either
    // a share of the frame or an absolute count, for exactly the reason the blank floor does --
    // scene content keeps its fraction as the frame grows, a fixed-pixel editor overlay keeps its
    // count, and admitting only one of the two makes the verdict move with the capture size.
    const int64 ToneLevelFloor = FMath::Min<int64>(
        ToneLevelMinPixels,
        FMath::Max<int64>(1, static_cast<int64>(FMath::CeilToDouble(ToneLevelMinFraction * Count))));
    Stats.ToneLevelMinPixelsUsed = ToneLevelFloor;
    for (const int64 LevelPopulation : ToneHistogram)
    {
        Stats.ToneLevelsUsed += (LevelPopulation >= ToneLevelFloor) ? 1 : 0;
    }
    const bool bToneCollapsed = Stats.ToneLevelsUsed < MinUsableToneLevels;
    // Mutually exclusive and jointly exhaustive over a collapse: the split only names which way
    // `ev100` has to move, and a collapsed frame always gets exactly one of the two labels.
    Stats.bCrushed = bToneCollapsed && Stats.MeanLuminance < CollapsedFrameDarkHalfMean;
    Stats.bBlownOut = bToneCollapsed && !Stats.bCrushed;
    // Last, so that every early return above leaves it false and no partial stats can be read as a
    // measurement.
    Stats.bStatsMeasured = true;
    return Stats;
}

void AddToneRangeStatsFields(const FCaptureImageStats& Stats,
    const TSharedPtr<FJsonObject>& ImageStats)
{
    if (!ImageStats.IsValid() || !Stats.bStatsMeasured)
    {
        // Omitted rather than zeroed. A `toneLevelsUsed: 0` on a frame nobody measured would read
        // as the most collapsed image possible, which is a verdict no one reached
        // (rpc-design.md section 4).
        return;
    }
    ImageStats->SetNumberField(TEXT("toneLevelsUsed"), Stats.ToneLevelsUsed);
    ImageStats->SetNumberField(TEXT("toneLevelMinPixels"),
        static_cast<double>(Stats.ToneLevelMinPixelsUsed));
}

FString MakeToneRangeWarning(const FCaptureImageStats& Stats)
{
    if (!Stats.bStatsMeasured || !(Stats.bCrushed || Stats.bBlownOut))
    {
        return FString();
    }
    // Names the measured count, the threshold, the mean, and the DIRECTION to move `ev100` -- the
    // exposure parameter runs backwards from brightness (a larger EV100 is a darker frame), which
    // has already sent one caller several stops the wrong way (see `ev100Equivalent`).
    return FString::Printf(
        TEXT("This frame carries no usable dynamic range: it resolves only %d of 256 luminance ")
        TEXT("levels (fewer than %d), at mean luminance %.6f, so it cannot show a gradient, a ")
        TEXT("shadow terminator or a material response whatever was rendered into it. `blank` is ")
        TEXT("false and correct -- pixels WERE drawn; `blank` catches a frame nothing was drawn ")
        TEXT("into, which is a different failure. The usual cause is an exposure past the end of ")
        TEXT("this scene's range: %s. The usable `ev100` band is scene-dependent, so pick one by ")
        TEXT("reading `imageStats.meanLuminance` back rather than by reusing a number from ")
        TEXT("elsewhere. A level count of %lld pixels was required for a level to count as ")
        TEXT("populated at this capture size."),
        Stats.ToneLevelsUsed,
        MinUsableToneLevels,
        Stats.MeanLuminance,
        Stats.bCrushed
            ? TEXT("the frame is crushed at the DARK end, so LOWER `ev100` (a smaller EV100 is a "
                   "brighter frame)")
            : TEXT("the frame is blown out at the BRIGHT end, so RAISE `ev100` (a larger EV100 is "
                   "a darker frame)"),
        static_cast<long long>(Stats.ToneLevelMinPixelsUsed));
}

void AddToneRangeVerdictFields(const FViewportCaptureOutput& Capture,
    const TSharedPtr<FJsonObject>& Result)
{
    if (!Result.IsValid() || !Capture.ImageStats.bStatsMeasured)
    {
        return;
    }
    // THE GATE. `crushed` / `blownOut` count populated 8-bit luminance levels and call fewer than
    // MinUsableToneLevels a collapse. That inference is only sound where the renderer was asked
    // for a continuous-tone image: a wireframe frame is two colours on a background, an unlit
    // frame is flat albedo, a complexity or collision visualisation is a small fixed palette --
    // each legitimately resolves two or three levels and would be labelled `crushed` at full
    // confidence. The `viewMode` parameter routes exactly those frames through here.
    //
    // The predicate is IsLitViewMode, reached through the lit-ness the capture already measured
    // and already reports as `viewport.lit`, NOT a second copy of the same question -- two
    // predicates for one question is what drifts. It is also the same term that makes the
    // exposure block's `pinnedFrameUsable` structurally safe: that field requires bExposurePinned,
    // and ExposurePinGovernsFrame refuses to pin a non-lit frame, so it can never reach a verdict
    // this one would have to suppress.
    Result->SetBoolField(TEXT("toneRangeApplicable"), Capture.bLitViewMode);
    if (!Capture.bLitViewMode)
    {
        // NOT-APPLICABLE AS A REAL ANSWER, not silence. Omitting `crushed` on an unlit frame reads
        // as "measured and fine" -- the same failure geometry.audit_static_meshes' per-check
        // tallies exist to prevent (MeshAuditUtils.h, `Applicable + NotApplicable ==
        // assetsExamined`). The bool above is the machine-readable half and this string the
        // explanation, the same pairing as `pinned` / `pinWarning` in the exposure block.
        Result->SetStringField(TEXT("toneRangeNotApplicable"), FString::Printf(
            TEXT("No tone-range verdict was reached for this frame: it was captured in view mode ")
            TEXT("'%s' (%s), which is not a lit mode. `crushed` and `blownOut` are derived from ")
            TEXT("how many of 256 luminance levels the pixels populate, and a wireframe, unlit or ")
            TEXT("debug-visualisation frame resolves very few by construction -- a two-tone frame ")
            TEXT("is what that mode was asked to draw, not evidence of a bad exposure. The count ")
            TEXT("itself is still reported as `imageStats.toneLevelsUsed`; only the verdict is ")
            TEXT("withheld. Capture without `viewMode`, or in 'Lit', if you need one."),
            *Capture.ViewModeKey, *Capture.ViewMode));
        return;
    }
    Result->SetBoolField(TEXT("crushed"), Capture.ImageStats.bCrushed);
    Result->SetBoolField(TEXT("blownOut"), Capture.ImageStats.bBlownOut);
    const FString RangeWarning = MakeToneRangeWarning(Capture.ImageStats);
    if (!RangeWarning.IsEmpty())
    {
        Result->SetStringField(TEXT("rangeWarning"), RangeWarning);
    }
}

float MaxAutoViewDistanceScaleFor(double MinCullDistance)
{
    if (MinCullDistance <= 0.0)
    {
        return MaxAutoViewDistanceScale;
    }
    // The int32 truncation bound from HierarchicalInstancedStaticMesh.cpp:1675, expressed as a
    // scale. The smallest cull distance in the scene is what reaches the bound last, so it is
    // the one the cap is computed against -- a larger cull distance simply never gets close.
    const double IntegerSafeScale = MaxScaledCullDistance / MinCullDistance;
    return static_cast<float>(FMath::Min(IntegerSafeScale, static_cast<double>(MaxAutoViewDistanceScale)));
}

float ReadEffectiveViewDistanceScale()
{
    return GetCachedScalabilityCVars().ViewDistanceScale;
}

FVector MeasureCaptureCullingOrigin(FEditorViewportClient& ViewportClient,
    const TSharedPtr<FSceneViewport>& SceneViewport, bool& bOutMeasured)
{
    bOutMeasured = false;
    const FVector CameraLocation = ViewportClient.GetViewLocation();
    if (!SceneViewport.IsValid() || ViewportClient.GetScene() == nullptr)
    {
        return CameraLocation;
    }

    // Same construction PinWrightViewProjection uses to rebuild a capture's FSceneView
    // (Handlers/Render/ViewProjectionUtils.cpp), deliberately: a second, differently-shaped view
    // family is how the measured origin drifts away from the one the capture actually renders.
    FVector CullingOrigin = CameraLocation;
    {
        FSceneViewFamilyContext ViewFamily(FSceneViewFamily::ConstructionValues(
            SceneViewport.Get(),
            ViewportClient.GetScene(),
            ViewportClient.EngineShowFlags)
            .SetRealtimeUpdate(ViewportClient.IsRealtime()));
        // The orthographic near-plane correction that moves the origin is gated on
        // `ViewFamily->ViewMode > VMI_Unlit` (EditorViewportClient.cpp:1407), so the family has to
        // carry the mode the capture will draw in or the measurement answers a different question
        // than the render asks.
        ViewFamily.ViewMode = ViewportClient.GetViewMode();
        if (const FSceneView* View = ViewportClient.CalcSceneView(&ViewFamily))
        {
            CullingOrigin = View->CullingOrigin;
            bOutMeasured = true;
        }
    }
    return CullingOrigin;
}

FViewDistanceSurvey SurveyViewDistances(UWorld* World, const FVector& CullingOrigin)
{
    FViewDistanceSurvey Survey;
    Survey.CullingOrigin = CullingOrigin;
    if (!World)
    {
        return Survey;
    }
    Survey.bValid = true;

    // Read ONCE, outside the loop: it is a global that applies to every instanced component and
    // it is the only term in the instanced cull path that r.ViewDistanceScale cannot lift (see
    // FViewDistanceSurvey::FoliageMaxEndCullDistance). Absent on a host that has not loaded the
    // engine's foliage cvars, which reads as "disabled" -- the engine's own default.
    if (const IConsoleVariable* FoliageCeilingCVar =
            IConsoleManager::Get().FindConsoleVariable(TEXT("foliage.MaxEndCullDistance")))
    {
        const int32 Ceiling = FoliageCeilingCVar->GetInt();
        Survey.FoliageMaxEndCullDistance = (Ceiling > 0) ? static_cast<double>(Ceiling) : 0.0;
    }

    // One pass over the primitive components of THIS world. TObjectIterator walks every loaded
    // UObject of the class, so the world filter is what keeps other levels, the asset-preview
    // scenes and class default objects out of the numbers.
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

        ++Survey.NumPrimitives;

        // USceneComponent::GetBounds() is 5.8+; the Bounds member it returns is public on every
        // supported engine, so read it directly.
        const FBoxSphereBounds& Bounds = Component->Bounds;
        // Reach to the FAR edge of the bounding sphere. The renderer culls on the NEAR edge
        // (SceneVisibility.cpp:1024 compares ClosestDistSquared, and ComputeDistances at :722
        // subtracts the radius), so covering the far edge is strictly conservative -- the safe
        // direction for a number whose job is to stop content disappearing.
        const double CentreDistance = FVector::Dist(Bounds.Origin, CullingOrigin);
        const double ReachFromOrigin = CentreDistance + static_cast<double>(Bounds.SphereRadius);
        Survey.MaxPrimitiveDistance = FMath::Max(Survey.MaxPrimitiveDistance, ReachFromOrigin);
        // Near culling compares the FAR edge (SceneVisibility.cpp:1025 uses FurthestDistSquared),
        // so the primitive that limits how far the scale can be pushed is the one whose far edge
        // is closest.
        Survey.MinPrimitiveDistance = (Survey.MinPrimitiveDistance > 0.0)
            ? FMath::Min(Survey.MinPrimitiveDistance, ReachFromOrigin)
            : ReachFromOrigin;

        if (Component->MinDrawDistance > 0.0f)
        {
            Survey.MaxMinDrawDistance =
                FMath::Max(Survey.MaxMinDrawDistance, static_cast<double>(Component->MinDrawDistance));
        }

        // A CachedMaxDrawDistance of 0 means "never cull by distance"
        // (UE 5.8 Runtime/Engine/Classes/Components/PrimitiveComponent.h:344).
        double CullDistance = 0.0;
        if (Component->CachedMaxDrawDistance > 0.0f)
        {
            CullDistance = static_cast<double>(Component->CachedMaxDrawDistance);
        }

        // Instanced meshes cull their INSTANCES on a separate, per-component distance that the
        // primitive-level MaxDrawDistance says nothing about. This is the one that empties a
        // forest out of a wide top-down, so it is measured explicitly rather than inferred.
        if (const UInstancedStaticMeshComponent* Instanced = Cast<UInstancedStaticMeshComponent>(Component))
        {
            ++Survey.NumInstancedComponents;
            int32 StartCull = 0;
            int32 EndCull = 0;
            Instanced->GetCullDistances(StartCull, EndCull);
            double InstanceCull = (EndCull > 0) ? static_cast<double>(EndCull) : 0.0;

            // foliage.MaxEndCullDistance is applied AFTER the scale, so it is a ceiling on what
            // this component can EVER draw at, not a distance the scale can trade against. A
            // component that reaches past it is culled at every scale, so it is counted and
            // EXCLUDED rather than allowed to drive the derived scale toward a cap it can never
            // cash in. See FViewDistanceSurvey::FoliageMaxEndCullDistance for the arithmetic.
            //
            // HISM ONLY, which is narrower than "instanced". Both reads of the cvar sit inside
            // FHierarchicalStaticMeshSceneProxy (HierarchicalInstancedStaticMesh.cpp:1658 in
            // ::GetDynamicMeshElements and :1870), and that proxy is created only by
            // UHierarchicalInstancedStaticMeshComponent::CreateStaticMeshSceneProxy (:3004) --
            // a plain UInstancedStaticMeshComponent gets FInstancedStaticMeshSceneProxy
            // (InstancedStaticMesh.cpp:2600) and is never ceiling-clamped. Applying the ceiling to
            // one would EXCLUDE from RequiredScale a component the scale genuinely recovers, and
            // warn about a limit it does not have -- the same silent-missing-content failure this
            // whole derivation exists to remove, pointed the other way.
            if (Survey.FoliageMaxEndCullDistance > 0.0 &&
                Instanced->IsA<UHierarchicalInstancedStaticMeshComponent>())
            {
                if (ReachFromOrigin > Survey.FoliageMaxEndCullDistance)
                {
                    ++Survey.NumFoliageCeilingLimited;
                    InstanceCull = 0.0;
                }
                else if (InstanceCull > 0.0)
                {
                    // The engine's own FMath::Min(MaxEndCullDistance, EndCullDistance).
                    InstanceCull = FMath::Min(InstanceCull, Survey.FoliageMaxEndCullDistance);
                }
                // ...and the ceiling is deliberately NOT substituted for a MISSING end cull
                // distance. The engine's product is `EndCullDistance * MaxDrawDistanceScale`
                // (HierarchicalInstancedStaticMesh.cpp:1675) -- zero times any scale is zero, so
                // the ceiling then becomes the component's cull distance OUTRIGHT and no
                // r.ViewDistanceScale moves it. Such a component therefore has no requirement to
                // contribute: it is either already inside the ceiling (this branch, drawn at
                // every scale) or past it (the branch above, drawn at none). Substituting the
                // ceiling here instead manufactured a reach/ceiling ratio -- always <= 1, so
                // never a scale the derivation could use -- and fed the ceiling into
                // MinCullDistance, where it can clip the int32-truncation cap for a distance the
                // scale never multiplies.
            }

            if (InstanceCull > 0.0)
            {
                CullDistance = (CullDistance > 0.0) ? FMath::Min(CullDistance, InstanceCull) : InstanceCull;
            }
        }

        if (CullDistance > 0.0)
        {
            ++Survey.NumCulledPrimitives;
            Survey.MinCullDistance = (Survey.MinCullDistance > 0.0)
                ? FMath::Min(Survey.MinCullDistance, CullDistance)
                : CullDistance;
            // THIS primitive's reach against THIS primitive's cull distance. Pairing the global
            // max reach with the global min cull distance instead is what let one actor with an
            // enormous bounding sphere decide the scale for a forest that was never at risk.
            Survey.RequiredScale = FMath::Max(Survey.RequiredScale, ReachFromOrigin / CullDistance);
        }
    }

    return Survey;
}

float ComputeAutoViewDistanceScale(const FViewDistanceSurvey& Survey)
{
    // No survey, nothing culled, or a degenerate origin-inside-everything case: 1.0 means "do not
    // touch the cvar". Returning 1.0 rather than 0.0 keeps this a scale in every branch, so a
    // caller can multiply with it unconditionally.
    if (!Survey.bValid || Survey.MinCullDistance <= 0.0 || Survey.RequiredScale <= 0.0)
    {
        return 1.0f;
    }
    // "Already covered" is decided on the bare requirement, before the margin: a scene whose cull
    // distances exactly reach the frame must stay a no-op, or every correct capture in the
    // project starts moving a global cvar for nothing.
    if (Survey.RequiredScale <= 1.0)
    {
        return 1.0f;
    }
    const double Required = Survey.RequiredScale * AutoViewDistanceScaleMargin;

    double Cap = static_cast<double>(MaxAutoViewDistanceScaleFor(Survey.MinCullDistance));

    // A scale multiplies NEAR culling as well: SceneVisibility.cpp:999 computes
    // MinDrawDistanceSq = Square(MinDrawDistance * MaxDrawDistanceScale), and :1025 culls a
    // primitive whose FAR edge is inside that radius. Recovering distant foliage by making near
    // geometry vanish is not a fix, so on a map that uses MinDrawDistance the scale is bounded to
    // keep the closest primitive outside the scaled near radius. Most maps set none, and then
    // this bound does not exist.
    if (Survey.MaxMinDrawDistance > 0.0 && Survey.MinPrimitiveDistance > 0.0)
    {
        Cap = FMath::Min(Cap, Survey.MinPrimitiveDistance / Survey.MaxMinDrawDistance);
    }

    // A cap below 1.0 would turn a no-op into "cull more than before", which is worse than the
    // defect. Refuse to move the cvar at all in that case; the caller reports the conflict.
    if (Cap <= 1.0)
    {
        return 1.0f;
    }
    return static_cast<float>(FMath::Min(Required, Cap));
}

bool ResolveOrthographicView(const FRotator& RequestedRotation, FOrthographicViewResolution& OutResolution)
{
    OutResolution = FOrthographicViewResolution();

    const FVector Forward = RequestedRotation.Vector().GetSafeNormal();
    const float MinDot = FMath::Cos(FMath::DegreesToRadians(OrthoAxisToleranceDegrees));

    if (!Forward.IsNearlyZero())
    {
        for (const FOrthoViewportTypeEntry& Entry : GOrthoViewportTypes)
        {
            if (static_cast<float>(FVector::DotProduct(Forward, Entry.Forward)) >= MinDot)
            {
                OutResolution.ViewportType = Entry.Type;
                OutResolution.EffectiveRotation = Entry.EffectiveRotation;
                OutResolution.ViewName = Entry.Name;
                OutResolution.bAxisAligned = true;
                OutResolution.bRotationSnapped =
                    !Entry.EffectiveRotation.Equals(RequestedRotation.GetNormalized(), 0.01f);
                return true;
            }
        }
    }

    // No cardinal match. LVT_OrthoFreelook renders the same fixed view as LVT_OrthoYZ, i.e. the
    // FRotator(0,0,0) pose, so report that as the effective rotation rather than the request.
    OutResolution.ViewportType = LVT_OrthoFreelook;
    OutResolution.EffectiveRotation = FRotator::ZeroRotator;
    OutResolution.ViewName = TEXT("back");
    OutResolution.bAxisAligned = false;
    OutResolution.bRotationSnapped = true;
    return false;
}

float ComputeOrthoZoomForWorldWidth(const FEditorViewportClient& ViewportClient,
    const FViewport* Viewport, float WorldWidth)
{
    const float SafeWorldWidth = FMath::Max(WorldWidth, KINDA_SMALL_NUMBER);

    // No viewport to measure: fall back to the bare CAMERA_ZOOM_DIV relation, which is exact only
    // when r.Editor.AlignedOrthoZoom is 0.
    float Zoom = SafeWorldWidth * EditorOrthoZoomDivisor;

    if (Viewport)
    {
        const float SizeX = static_cast<float>(FMath::Max(Viewport->GetSizeXY().X, 1));
        const float CurrentZoom = ViewportClient.GetOrthoZoom();
        const float UnitsPerPixel = ViewportClient.GetOrthoUnitsPerPixel(Viewport);
        // GetOrthoUnitsPerPixel is exactly linear in OrthoZoom
        // ((Zoom / (SizeX * 15)) * ComputeOrthoZoomFactor(SizeX), EditorViewportClient.cpp:843-849),
        // so this ratio recovers the engine's zoom->world scale for THIS viewport width without
        // duplicating CAMERA_ZOOM_DIV or ComputeOrthoZoomFactor - both of which have moved between
        // engine versions, and the latter of which is CVar-gated (r.Editor.AlignedOrthoZoom
        // defaults to 1, making the scale width-dependent).
        const float UnitsPerPixelPerZoom =
            (CurrentZoom > KINDA_SMALL_NUMBER) ? (UnitsPerPixel / CurrentZoom) : 0.0f;
        if (UnitsPerPixelPerZoom > KINDA_SMALL_NUMBER)
        {
            Zoom = (SafeWorldWidth / SizeX) / UnitsPerPixelPerZoom;
        }
    }

    // FViewportCameraTransform::SetOrthoZoom ensures against these bounds (EditorViewportClient.h:254).
    return FMath::Clamp(Zoom, static_cast<float>(MIN_ORTHOZOOM), static_cast<float>(MAX_ORTHOZOOM));
}

float ComputeOrthoWorldWidthFromZoom(const FEditorViewportClient& ViewportClient,
    const FViewport* Viewport)
{
    if (!Viewport)
    {
        return ViewportClient.GetOrthoZoom() / EditorOrthoZoomDivisor;
    }
    const float SizeX = static_cast<float>(FMath::Max(Viewport->GetSizeXY().X, 1));
    return ViewportClient.GetOrthoUnitsPerPixel(Viewport) * SizeX;
}

void ApplyCaptureCamera(FEditorViewportClient& ViewportClient, const FViewport* Viewport,
    const FViewportCaptureRequest& Request)
{
    if (Request.ProjectionMode == TEXT("orthographic"))
    {
        if (!Request.bPreserveViewportType)
        {
            // The engine builds the orthographic view matrix from the viewport type and discards
            // the camera rotation entirely, so the requested rotation has to become a TYPE here or
            // it is silently dropped (which is what LVT_OrthoFreelook did for every pose).
            FOrthographicViewResolution Resolution;
            ResolveOrthographicView(Request.Rotation, Resolution);
            ViewportClient.SetViewportType(Resolution.ViewportType);
        }
        // Must follow SetViewportType: SetOrthoZoom and GetOrthoUnitsPerPixel both read
        // GetViewTransform(), which only resolves to the orthographic transform once the client
        // is non-perspective.
        ViewportClient.SetOrthoZoom(
            ComputeOrthoZoomForWorldWidth(ViewportClient, Viewport, Request.OrthoWidth));
    }
    else
    {
        ViewportClient.SetViewportType(LVT_Perspective);
        ViewportClient.FOVAngle = Request.Fov;
        ViewportClient.ViewFOV = Request.Fov;
        // MUST follow SetViewportType and precede the SetViewLocation / SetViewRotation below.
        // ToggleOrbitCamera converts through GetViewTransform(), which only resolves to the
        // perspective transform once the client is perspective; and the conversion WRITES both a
        // location and a rotation onto that transform, so a pose applied before it would simply be
        // overwritten. See the header for what orbit mode does to a requested pose and why the
        // orthographic branch above deliberately does not do this.
        ViewportClient.ToggleOrbitCamera(false);
    }
    ViewportClient.SetViewLocation(Request.Location);
    // Kept for orthographic captures too: the render ignores it, but it keeps the client's saved
    // camera state coherent and is what the perspective restore path expects.
    ViewportClient.SetViewRotation(Request.Rotation);
    // Display only. This runs immediately after SetFixedViewportSize, i.e. inside the window where
    // the viewport's hit-proxy render target has just been released and rebuilt -- see PumpViewport.
    ViewportClient.Invalidate(/*bInvalidateChildViews=*/true, /*bInvalidateHitProxies=*/false);
}

FEffectiveViewPose ResolveEffectiveViewPose(bool bOrbitCameraInForce,
    const FViewportCameraTransform& Transform)
{
    FEffectiveViewPose Pose;
    Pose.Location = Transform.GetLocation();
    Pose.Rotation = Transform.GetRotation();
    Pose.bFromOrbitCamera = bOrbitCameraInForce;
    if (!bOrbitCameraInForce)
    {
        return Pose;
    }

    // In orbit mode FEditorViewportClient::CalcViewRotationMatrix returns
    // FTranslationMatrix(Transform.GetLocation()) * Transform.ComputeOrbitMatrix()
    // (EditorViewportClient.cpp:7396-7400), and CalcSceneView pre-multiplies
    // FTranslationMatrix(-ViewOrigin) with ViewOrigin taken from that same location (:1113, :1141,
    // :1253). The two translations cancel exactly, leaving ComputeOrbitMatrix as the whole
    // world->view transform -- which is WHY the requested location contributes nothing to an orbit
    // view but its DISTANCE from the pivot. ComputeOrbitMatrix is called here rather than
    // transcribed, so its trailing FInverseRotationMatrix(FRotator(0,90,0)) and its pivot
    // arithmetic (:395-405) stay the engine's.
    //
    // The 90-degree axis swizzle CalcSceneView applies afterwards (:1255-1260) is deliberately NOT
    // applied: it converts UE world axes into the renderer's view axes and is identical on both
    // branches, so leaving it off is what makes the result an ordinary UE camera pose.
    const FMatrix ViewToWorld = Transform.ComputeOrbitMatrix().Inverse();
    Pose.Location = ViewToWorld.GetOrigin();
    Pose.Rotation = ViewToWorld.Rotator();
    return Pose;
}

FEffectiveViewPose MeasureEffectiveViewPose(const FEditorViewportClient& ViewportClient)
{
    // CalcSceneView's orbit branch lives inside its LVT_Perspective arm
    // (EditorViewportClient.cpp:1233-1253); the orthographic arm below it builds the view matrix
    // from the viewport type and never reads the orbit transform. Running the orbit matrix over an
    // orthographic client would therefore report a pose its pixels never had.
    return ResolveEffectiveViewPose(
        ViewportClient.bUsingOrbitCamera && ViewportClient.IsPerspective(),
        ViewportClient.GetViewTransform());
}

FBoundsFramingCheck EvaluateBoundsFraming(
    const FViewportCaptureRequest& Request,
    const FVector& CameraLocation,
    const FRotator& CameraRotation,
    const FVector& BoundsOrigin,
    double BoundsRadius)
{
    FBoundsFramingCheck Check;
    const FVector Forward = CameraRotation.Vector().GetSafeNormal();
    if (!(BoundsRadius > 0.0) || Forward.IsNearlyZero())
    {
        return Check;
    }
    Check.bEvaluated = true;

    const FVector ToCentre = BoundsOrigin - CameraLocation;
    const double CentreDistance = ToCentre.Size();
    if (CentreDistance <= BoundsRadius)
    {
        // The camera is INSIDE the bounding sphere. Nothing can be proven about the frame from a
        // sphere test here, and the conservative answer is the permissive one.
        return Check;
    }
    const double DepthAlongAxis = FVector::DotProduct(ToCentre, Forward);
    const FVector Lateral = ToCentre - (DepthAlongAxis * Forward);

    if (Request.ProjectionMode == TEXT("orthographic"))
    {
        // An orthographic frame is a box, so the circumscribing figure is the cylinder through its
        // corners: half-diagonal of orthoWidth x (orthoWidth * height/width). No behind-camera test
        // -- the editor's orthographic near plane sits far behind the camera, and content behind
        // the eye renders normally.
        Check.Units = TEXT("centimetres");
        const double HalfWidth = FMath::Max(static_cast<double>(Request.OrthoWidth), 0.0) * 0.5;
        const double AspectHeightOverWidth = (Request.Width > 0 && Request.Height > 0)
            ? (static_cast<double>(Request.Height) / static_cast<double>(Request.Width))
            : 1.0;
        const double HalfHeight = HalfWidth * AspectHeightOverWidth;
        Check.FrameLimit = FMath::Sqrt((HalfWidth * HalfWidth) + (HalfHeight * HalfHeight));
        Check.OffAxis = Lateral.Size() - BoundsRadius;
        Check.bBoundsInFrame = Check.OffAxis <= Check.FrameLimit;
        return Check;
    }

    Check.Units = TEXT("degrees");
    // Entirely behind the camera plane: no perspective projection can put it in frame.
    if (DepthAlongAxis + BoundsRadius <= 0.0)
    {
        Check.bBehindCamera = true;
        Check.bBoundsInFrame = false;
        Check.OffAxis = 180.0;
        Check.FrameLimit = 0.0;
        return Check;
    }

    // The cone that circumscribes the frustum. `Fov` is one axis of it and the engine chooses
    // WHICH axis from ULevelEditorViewportSettings::AspectRatioAxisConstraint, so the wider of the
    // two possible readings is taken and then widened again to the frame DIAGONAL. Over-estimating
    // the frame is what keeps a false verdict a fact rather than an approximation.
    const double HalfFovRadians =
        FMath::DegreesToRadians(FMath::Clamp(static_cast<double>(Request.Fov), 1.0, 170.0) * 0.5);
    const double Aspect = (Request.Width > 0 && Request.Height > 0)
        ? (static_cast<double>(Request.Width) / static_cast<double>(Request.Height))
        : 1.0;
    const double WidestAxisScale = FMath::Max(1.0, FMath::Max(Aspect, 1.0 / Aspect));
    const double HalfAxisTan = WidestAxisScale * FMath::Tan(HalfFovRadians);
    Check.FrameLimit = FMath::RadiansToDegrees(FMath::Atan(UE_DOUBLE_SQRT_2 * HalfAxisTan));

    // Nearest angular approach of the bounding sphere to the view axis: the angle to its centre
    // less the angle it subtends.
    const double CentreAngle = FMath::RadiansToDegrees(
        FMath::Acos(FMath::Clamp(DepthAlongAxis / CentreDistance, -1.0, 1.0)));
    const double AngularRadius = FMath::RadiansToDegrees(
        FMath::Asin(FMath::Clamp(BoundsRadius / CentreDistance, 0.0, 1.0)));
    Check.OffAxis = CentreAngle - AngularRadius;
    Check.bBoundsInFrame = Check.OffAxis <= Check.FrameLimit;
    return Check;
}

TSharedPtr<FJsonObject> MakeBoundsFramingObject(const FBoundsFramingCheck& Check)
{
    TSharedPtr<FJsonObject> Framing = MakeShared<FJsonObject>();
    Framing->SetBoolField(TEXT("evaluated"), Check.bEvaluated);
    if (!Check.bEvaluated)
    {
        return Framing;
    }
    Framing->SetBoolField(TEXT("boundsInFrame"), Check.bBoundsInFrame);
    Framing->SetBoolField(TEXT("behindCamera"), Check.bBehindCamera);
    Framing->SetNumberField(TEXT("offAxis"), Check.OffAxis);
    Framing->SetNumberField(TEXT("frameLimit"), Check.FrameLimit);
    Framing->SetStringField(TEXT("units"), Check.Units);
    if (!Check.bBoundsInFrame)
    {
        // Present IF AND ONLY IF the subject is provably absent, and it says what the frame
        // contains instead, because the failure it guards is an agent comparing two pictures of
        // the backdrop. `blank` cannot catch this: a lit backdrop is not a black frame.
        const FString Warning = Check.bBehindCamera
            ? FString(TEXT("The subject is entirely BEHIND the camera, so this frame shows none of it ")
                TEXT("-- only the backdrop. Such a frame is not blank and not black, so `blank` and ")
                TEXT("the luminance stats all look healthy; do not compare it with another capture as ")
                TEXT("if it showed the asset. Re-aim the camera at the asset's bounds."))
            : FString::Printf(
                TEXT("The subject's bounding sphere cannot project into this frame: its nearest ")
                TEXT("approach to the view axis is %.2f %s against a frame half-extent of %.2f. These ")
                TEXT("pixels are backdrop only. Such a frame is not blank and not black, so `blank` ")
                TEXT("and the luminance stats all look healthy; do not compare it with another capture ")
                TEXT("as if it showed the asset."),
                Check.OffAxis, *Check.Units, Check.FrameLimit);
        Framing->SetStringField(TEXT("framingWarning"), Warning);
    }
    return Framing;
}

const TCHAR* ExposureModeKey(EExposureRequestMode Mode)
{
    switch (Mode)
    {
    case EExposureRequestMode::Auto:  return TEXT("auto");
    case EExposureRequestMode::Fixed: return TEXT("fixed");
    case EExposureRequestMode::Unset: return TEXT("unset");
    }
    return TEXT("unset");
}

float ExposureLuminanceMax()
{
    // A transcription of LuminanceMaxFromLensAttenuation (UE 5.8
    // Runtime/Renderer/Private/PostProcess/PostProcessEyeAdaptation.cpp:376-389) reading the same
    // two cvars it reads, because that function lives in the Renderer module's private header. The
    // cvars are looked up on every call rather than cached in a static: both are runtime-settable
    // and a capture session that changes one must not keep converting against the old value.
    //
    // 0.78 is the ISO 12232:2006 saturation-speed constant, spelled exactly as the engine spells
    // it at :383. The 0.01 floor on the attenuation is the engine's too (:385) and is what keeps
    // a zeroed cvar from dividing by zero.
    constexpr float ISOSaturationSpeedConstant = 0.78f;
    constexpr float MinLensAttenuation = 0.01f;

    IConsoleManager& Console = IConsoleManager::Get();
    const IConsoleVariable* ExtendedRange = Console.FindConsoleVariable(
        TEXT("r.DefaultFeature.AutoExposure.ExtendDefaultLuminanceRange"), /*bTrackFrequentCalls=*/false);
    if (ExtendedRange == nullptr || ExtendedRange->GetInt() != 1)
    {
        // IsExtendLuminanceRangeEnabled() false: the engine hardcodes the scale to 1.0 so that
        // lighting is unitless (1.0 cd/m^2 becomes 1.0 at EV100 0).
        return 1.0f;
    }
    const IConsoleVariable* LensAttenuation =
        Console.FindConsoleVariable(TEXT("r.EyeAdaptation.LensAttenuation"), /*bTrackFrequentCalls=*/false);
    const float Attenuation = LensAttenuation != nullptr
        ? LensAttenuation->GetFloat()
        : ISOSaturationSpeedConstant;
    return ISOSaturationSpeedConstant / FMath::Max(Attenuation, MinLensAttenuation);
}

float Ev100ToExposureGain(float Ev100)
{
    return 1.0f / EV100ToLuminance(ExposureLuminanceMax(), Ev100);
}

float ExposureGainToEv100(float ExposureGain)
{
    if (!(ExposureGain > 0.0f))
    {
        // No gain was measured. Returning 0 rather than -inf/NaN keeps the JSON encodable; the
        // caller must gate on `adaptedMeasured`, which is why this is not reported on its own.
        return 0.0f;
    }
    return LuminanceToEV100(ExposureLuminanceMax(), 1.0f / ExposureGain);
}

bool ParseExposurePin(const TSharedPtr<FJsonObject>& Payload, FExposurePin& OutPin,
    FString& OutErrorCode, FString& OutErrorMessage)
{
    OutPin = FExposurePin();
    OutErrorCode.Reset();
    OutErrorMessage.Reset();

    if (!Payload.IsValid() || !Payload->HasField(TEXT("exposure")))
    {
        return true;
    }

    const TSharedPtr<FJsonValue> Value = Payload->TryGetField(TEXT("exposure"));
    if (!Value.IsValid() || Value->Type == EJson::Null)
    {
        OutErrorCode = ErrorCodes::ERR_INVALID_ARGUMENT;
        OutErrorMessage = TEXT("exposure must be a number (EV100) or an object "
            "{mode:\"fixed\", ev100:<number>} or {mode:\"auto\"}; omit the field to leave auto-exposure alone");
        return false;
    }

    // Read the EV100 out of whichever shape carried it, then validate once. `bHasEv100` and not
    // "Ev100 != 0" because 0 is a perfectly ordinary EV100.
    bool bHasEv100 = false;
    double Ev100 = 0.0;
    FString Mode;

    if (Value->Type == EJson::Number)
    {
        // Bare-number shorthand: the one-token spelling of the only mode that does anything.
        Mode = TEXT("fixed");
        Ev100 = Value->AsNumber();
        bHasEv100 = true;
    }
    else if (Value->Type == EJson::Object)
    {
        const TSharedPtr<FJsonObject>& Object = Value->AsObject();
        if (!Object.IsValid())
        {
            OutErrorCode = ErrorCodes::ERR_INVALID_ARGUMENT;
            OutErrorMessage = TEXT("exposure object could not be read");
            return false;
        }
        Object->TryGetStringField(TEXT("mode"), Mode);
        bHasEv100 = Object->TryGetNumberField(TEXT("ev100"), Ev100);
        if (Mode.IsEmpty())
        {
            // An object carrying only an ev100 means "fixed" and nothing else; requiring the
            // caller to repeat it would be ceremony. An object carrying neither is a typo.
            if (!bHasEv100)
            {
                OutErrorCode = ErrorCodes::ERR_INVALID_ARGUMENT;
                OutErrorMessage = TEXT("exposure object needs a mode (\"auto\" or \"fixed\") or an "
                    "ev100 number; omit the field entirely to leave auto-exposure alone");
                return false;
            }
            Mode = TEXT("fixed");
        }
    }
    else
    {
        OutErrorCode = ErrorCodes::ERR_INVALID_ARGUMENT;
        OutErrorMessage = TEXT("exposure must be a number (EV100) or an object "
            "{mode:\"fixed\", ev100:<number>} or {mode:\"auto\"}");
        return false;
    }

    const FString ModeKey = Mode.ToLower();
    if (ModeKey == TEXT("auto"))
    {
        if (bHasEv100)
        {
            // The two halves of this request cancel: "auto" says leave auto-exposure running,
            // `ev100` says pin at a number. Accepting it and dropping the ev100 silently -- which
            // is what this did -- hands back an auto-exposed frame under a response that names the
            // ev100 the caller asked for, and the pin exists precisely so two frames can be
            // compared. Erroring on both halves at once is rpc-design.md section 3: there is no
            // safe half to keep, so the verb refuses instead of choosing one.
            OutErrorCode = ErrorCodes::ERR_INVALID_ARGUMENT;
            OutErrorMessage = FString::Printf(
                TEXT("exposure mode \"auto\" and ev100 %g contradict each other: \"auto\" leaves ")
                TEXT("auto-exposure running and ev100 pins the exposure, and no frame can be both. ")
                TEXT("Pass {mode:\"fixed\", ev100:%g} to pin at that EV100, or {mode:\"auto\"} with ")
                TEXT("no ev100 to leave auto-exposure alone."),
                Ev100, Ev100);
            return false;
        }
        // Explicit "do not pin". Renders exactly like an omitted parameter; the difference is
        // that the response can say the caller meant it.
        OutPin.Mode = EExposureRequestMode::Auto;
        return true;
    }
    if (ModeKey != TEXT("fixed"))
    {
        // An unrecognized mode errors rather than degrading to auto: a typo that silently means
        // "no pin" produces exactly the uncomparable pair the parameter exists to prevent
        // (rpc-design.md section 3).
        OutErrorCode = ErrorCodes::ERR_INVALID_ARGUMENT;
        OutErrorMessage = FString::Printf(
            TEXT("Unknown exposure mode '%s'. Valid modes are \"fixed\" (with ev100) and \"auto\"."),
            *Mode);
        return false;
    }

    if (!bHasEv100)
    {
        OutErrorCode = ErrorCodes::ERR_INVALID_ARGUMENT;
        OutErrorMessage = TEXT("exposure mode \"fixed\" needs an ev100 number. There is no safe "
            "default: any value this verb picked would be wrong for some scene and would be "
            "indistinguishable, in the response, from a value the caller chose.");
        return false;
    }

    if (!FMath::IsFinite(Ev100) ||
        Ev100 < static_cast<double>(MinExposureEv100) ||
        Ev100 > static_cast<double>(MaxExposureEv100))
    {
        OutErrorCode = ErrorCodes::ERR_INVALID_ARGUMENT;
        OutErrorMessage = FString::Printf(
            TEXT("exposure ev100 must be a finite value in [%.0f, %.0f]; got %g. The bound is wide "
                 "enough for anything physically meaningful, so a value outside it is a mistyped "
                 "magnitude that would render a black or blown frame while reporting a successful pin."),
            MinExposureEv100, MaxExposureEv100, Ev100);
        return false;
    }

    OutPin.Mode = EExposureRequestMode::Fixed;
    OutPin.Ev100 = static_cast<float>(Ev100);
    return true;
}

FEditorSpriteShowFlags ReadEditorSpriteShowFlags(const FEngineShowFlags& Flags)
{
    FEditorSpriteShowFlags State;
    State.bBillboardSprites = Flags.BillboardSprites != 0;
    return State;
}

FEditorSpriteShowFlags SuppressEditorSpriteShowFlags(FEngineShowFlags& Flags)
{
    const FEditorSpriteShowFlags Previous = ReadEditorSpriteShowFlags(Flags);
    Flags.SetBillboardSprites(false);
    return Previous;
}

void RestoreEditorSpriteShowFlags(FEngineShowFlags& Flags, const FEditorSpriteShowFlags& Previous)
{
    Flags.SetBillboardSprites(Previous.bBillboardSprites);
}

TArray<FForcedShowFlagOverride> SurveyForcedShowFlagOverrides()
{
    // The enumeration is the ENGINE's, not a hand-written list: FEngineShowFlags::IterateAllFlags
    // walks ShowFlagsValues.inl and then the custom flags registered by loaded modules, which is
    // the same walk FSystemSettings uses to register the cvars in the first place. A local table
    // would drift the moment an engine version or a plugin added a flag -- and the flag it missed
    // would be the one nobody thought to check.
    struct FForcedShowFlagSink
    {
        TArray<FForcedShowFlagOverride> Found;

        void Probe(const FString& Name)
        {
            const FString CVarName = FString::Printf(TEXT("ShowFlag.%s"), *Name);
            const IConsoleVariable* CVar =
                IConsoleManager::Get().FindConsoleVariable(*CVarName);
            if (!CVar)
            {
                // A flag whose cvar is not registered cannot be forced through this channel.
                // Silently skipped rather than reported as "not forced": the survey speaks only
                // about overrides it can actually see.
                return;
            }
            const int32 Value = CVar->GetInt();
            if (Value == 2)
            {
                // The documented default -- "do not override this showflag". The gate is the
                // VALUE and not the set-by priority, because a cvar deliberately written back to
                // 2 carries a raised priority and forces nothing; reporting it would be a warning
                // about the remedy.
                return;
            }
            FForcedShowFlagOverride& Entry = Found.AddDefaulted_GetRef();
            Entry.Name = Name;
            Entry.CVar = CVarName;
            Entry.Value = Value;
            // The engine's own spelling of the priority bits ("Console", "Code", ...), taken from
            // GetConsoleVariableSetByName rather than a local switch so a version that adds a
            // priority names it correctly instead of falling through to "unknown".
            Entry.SetBy = ::GetConsoleVariableSetByName(
                static_cast<EConsoleVariableFlags>(CVar->GetFlags() & ECVF_SetByMask));
        }

        bool OnEngineShowFlag(uint32 /*Index*/, const FString& Name)
        {
            Probe(Name);
            return true;
        }

        bool OnCustomShowFlag(uint32 /*Index*/, const FString& Name)
        {
            Probe(Name);
            return true;
        }
    };

    FForcedShowFlagSink Sink;
    FEngineShowFlags::IterateAllFlags(Sink);
    return MoveTemp(Sink.Found);
}

FScopedCaptureProjectionAspect::FScopedCaptureProjectionAspect(
    FEditorViewportClient& InClient, int32 Width, int32 Height)
    : Client(InClient)
    , PreviousAspectRatio(InClient.AspectRatio)
{
    Client.AspectRatio = ResolveCaptureProjectionAspect(Width, Height);

    // Only Level Editor clients can inherit a controlling camera. Find the exact registered
    // instance instead of casting an arbitrary asset-preview client. Clearing both lock slots and
    // refreshing is the engine's public path for dropping the independent FMinimalViewInfo.
    if (GEditor)
    {
        for (FLevelEditorViewportClient* LevelClient : GEditor->GetLevelViewportClients())
        {
            if (LevelClient == &InClient)
            {
                ActorLockState = MakeUnique<FCaptureActorLockState>();
                ActorLockState->Client = LevelClient;
                ActorLockState->ActorLock = LevelClient->GetActorLock();
                ActorLockState->CinematicActorLock = LevelClient->GetCinematicActorLock();
                LevelClient->SetActorLock(FLevelViewportActorLock::None);
                LevelClient->SetCinematicActorLock(FLevelViewportActorLock::None);
                LevelClient->UpdateViewForLockedActor();
                break;
            }
        }
    }
}

FScopedCaptureProjectionAspect::~FScopedCaptureProjectionAspect()
{
    Client.AspectRatio = PreviousAspectRatio;
    if (ActorLockState && ActorLockState->Client)
    {
        ActorLockState->Client->SetActorLock(ActorLockState->ActorLock);
        ActorLockState->Client->SetCinematicActorLock(ActorLockState->CinematicActorLock);
        ActorLockState->Client->UpdateViewForLockedActor();
    }
}

FScopedViewModeOverride::FScopedViewModeOverride(FEditorViewportClient& InClient,
    const FViewModePin& Pin)
    : Client(InClient)
{
    PreviousPersp = Client.GetPerspViewMode();
    PreviousOrtho = Client.GetOrthoViewMode();
    if (!Pin.WantsOverride())
    {
        // Nothing is written. This is the omitted-parameter path and it must stay byte-for-byte
        // identical to the behaviour before this parameter existed.
        return;
    }
    Client.SetViewModes(Pin.ViewMode, Pin.ViewMode);
    // Display only while the capture holds the viewport -- see PumpViewport.
    Client.Invalidate(/*bInvalidateChildViews=*/true, /*bInvalidateHitProxies=*/false);
    bApplied = true;
}

FScopedViewModeOverride::~FScopedViewModeOverride()
{
    if (!bApplied)
    {
        return;
    }
    Client.SetViewModes(PreviousPersp, PreviousOrtho);
    Client.Invalidate();
}

bool ParseHideEditorSprites(const TSharedPtr<FJsonObject>& Payload)
{
    // Deliberately permissive in one direction only: anything that is not an explicit true leaves
    // the viewport untouched. There is no error branch because there is no half-applied state to
    // report -- an unreadable value means the capture does exactly what it did before.
    return GetJsonBoolField(Payload, TEXT("hideEditorSprites"), false);
}

bool ParseViewModePin(const TSharedPtr<FJsonObject>& Payload, FViewModePin& OutPin,
    FString& OutErrorCode, FString& OutErrorMessage, const FEditorViewportClient* Client)
{
    OutPin = FViewModePin();
    OutErrorCode.Reset();
    OutErrorMessage.Reset();

    if (!Payload.IsValid() || !Payload->HasField(TEXT("viewMode")))
    {
        // The omitted-parameter path. Nothing is read and nothing will be written.
        return true;
    }

    FString Wire;
    if (!Payload->TryGetStringField(TEXT("viewMode"), Wire))
    {
        // Deliberately an error rather than "ignore it". `hideEditorSprites` can be permissive
        // because a false there means "do nothing", which is a legal outcome; a non-string
        // viewMode has no do-nothing reading -- the caller asked for a diagnostic view and would
        // otherwise get an ordinary capture that looks like the one they asked for.
        OutErrorCode = ErrorCodes::ERR_INVALID_ARGUMENT;
        OutErrorMessage = TEXT("viewMode must be a string; omit the field to capture in whatever "
                               "mode the viewport is already in.");
        return false;
    }

    const PinWrightViewModes::FViewModeResolution Resolution = Client
        ? PinWrightViewModes::ResolveForClient(Wire, *Client)
        : PinWrightViewModes::Resolve(Wire);
    if (!Resolution.IsValid())
    {
        OutErrorCode = Resolution.ErrorCode;
        OutErrorMessage = Resolution.ErrorMessage;
        return false;
    }

    OutPin.bRequested = true;
    OutPin.ViewMode = Resolution.ViewMode;
    OutPin.Key = Resolution.Key;
    OutPin.DistinguishingShowFlags = Resolution.DistinguishingShowFlags;
    OutPin.CompanionMode = Resolution.CompanionMode;
    return true;
}

bool ParseOffscreenCaptureRequest(
    const TSharedPtr<FJsonObject>& Payload,
    FViewportCaptureRequest& OutRequest,
    FString& OutErrorCode,
    FString& OutErrorMessage)
{
    OutErrorCode.Reset();
    OutErrorMessage.Reset();
    OutRequest.bAllowBlank = false;
    OutRequest.ViewDistanceScale = 0.0f;
    OutRequest.bViewDistanceScaleProvided = false;
    OutRequest.bHideEditorSprites = false;

    OutRequest.Filename = GetJsonStringField(Payload, TEXT("filename"));
    OutRequest.Width = GetJsonIntField(Payload, TEXT("width"), DefaultCaptureEdge);
    OutRequest.Height = GetJsonIntField(Payload, TEXT("height"), DefaultCaptureEdge);
    OutRequest.ProjectionMode = GetJsonStringField(Payload, TEXT("projectionMode"), TEXT("perspective")).ToLower();
    OutRequest.Location = ExtractVectorField(Payload, TEXT("location"), FVector::ZeroVector);
    OutRequest.Rotation = ExtractRotatorField(Payload, TEXT("rotation"), FRotator::ZeroRotator);
    // Record whether the caller actually supplied these (vs. the defaults above) so a
    // handler can fill the no-args framing path without re-reading the raw payload.
    OutRequest.bLocationProvided = Payload.IsValid() && Payload->HasField(TEXT("location"));
    OutRequest.bRotationProvided = Payload.IsValid() && Payload->HasField(TEXT("rotation"));
    OutRequest.Fov = static_cast<float>(GetJsonNumberField(Payload, TEXT("fov"), 50.0));
    // `orthoWidth` is WORLD CENTIMETRES (see FViewportCaptureRequest::OrthoWidth). `orthoWorldWidth`
    // is an accepted alias whose name states the unit; both mean the same thing.
    const bool bHasOrthoWorldWidthAlias = Payload.IsValid() && Payload->HasField(TEXT("orthoWorldWidth"));
    OutRequest.OrthoWidth = static_cast<float>(GetJsonNumberField(Payload,
        bHasOrthoWorldWidthAlias ? TEXT("orthoWorldWidth") : TEXT("orthoWidth"),
        static_cast<double>(DefaultOrthoWorldWidth)));

    if (OutRequest.Width <= 0 || OutRequest.Height <= 0 ||
        OutRequest.Width > MaxCaptureDimension || OutRequest.Height > MaxCaptureDimension)
    {
        OutErrorCode = ErrorCodes::ERR_INVALID_ARGUMENT;
        OutErrorMessage = FString::Printf(TEXT("width and height must be in (0, %d]"), MaxCaptureDimension);
        return false;
    }

    if (!IsValidProjectionMode(OutRequest.ProjectionMode))
    {
        OutErrorCode = ErrorCodes::ERR_INVALID_ARGUMENT;
        OutErrorMessage = TEXT("projectionMode must be 'perspective' or 'orthographic'");
        return false;
    }

    if (OutRequest.Fov <= 0.0f)
    {
        OutErrorCode = ErrorCodes::ERR_INVALID_ARGUMENT;
        OutErrorMessage = TEXT("fov must be greater than zero");
        return false;
    }

    if (OutRequest.OrthoWidth <= 0.0f)
    {
        OutErrorCode = ErrorCodes::ERR_INVALID_ARGUMENT;
        OutErrorMessage = TEXT("orthoWidth must be greater than zero (world centimetres)");
        return false;
    }

    if (!ParseExposurePin(Payload, OutRequest.Exposure, OutErrorCode, OutErrorMessage))
    {
        return false;
    }

    // No client here: this parser is called before the verb has resolved a viewport (and
    // render.capture_asset_preview has not opened one yet), so a mode whose sub-visualisation is
    // pre-selected is refused on this path. A verb that already holds a client can call
    // ParseViewModePin with it to let that case through.
    if (!ParseViewModePin(Payload, OutRequest.ViewMode, OutErrorCode, OutErrorMessage))
    {
        return false;
    }

    // Parsed HERE, in the shared parser, which means `render.capture_open_level` also reads it --
    // and that verb does not declare it. The verb-level `UNKNOWN_PARAMS` gate refuses the payload
    // before it ever reaches this function, but the pin is ALSO cleared on the level-viewport path
    // (RenderHandler.cpp, beside the existing viewDistanceScale / hideEditorSprites clears) so a
    // future caller of the parser cannot inherit a rig the verb never offered. Two independent
    // stops, because the previous wave shipped a verb that accepted three undeclared parameters
    // with only one of them in place.
    if (!PinWrightPreviewSceneRig::ParsePreviewSceneRigPin(Payload, OutRequest.PreviewSceneRig,
            OutErrorCode, OutErrorMessage))
    {
        return false;
    }

    return true;
}

bool ParseViewportCaptureRequest(
    const TSharedPtr<FJsonObject>& Payload,
    FViewportCaptureRequest& OutRequest,
    FString& OutErrorCode,
    FString& OutErrorMessage)
{
    if (!ParseOffscreenCaptureRequest(
            Payload, OutRequest, OutErrorCode, OutErrorMessage))
    {
        return false;
    }

    OutRequest.bAllowBlank = GetJsonBoolField(Payload, TEXT("allowBlank"), false);
    OutRequest.bViewDistanceScaleProvided =
        Payload.IsValid() && Payload->HasField(TEXT("viewDistanceScale"));
    if (OutRequest.bViewDistanceScaleProvided)
    {
        OutRequest.ViewDistanceScale =
            static_cast<float>(GetJsonNumberField(Payload, TEXT("viewDistanceScale"), 1.0));
        if (OutRequest.ViewDistanceScale <= 0.0f)
        {
            // Zero is rejected rather than read as "leave it alone" because zero is also a legal
            // r.ViewDistanceScale that culls everything -- the two readings are opposite and the
            // caller who typed it would not learn which one they got. Omit the field to opt out.
            OutErrorCode = ErrorCodes::ERR_INVALID_ARGUMENT;
            OutErrorMessage = TEXT("viewDistanceScale must be greater than zero; omit the field to leave r.ViewDistanceScale unchanged");
            return false;
        }
    }
    OutRequest.bHideEditorSprites = ParseHideEditorSprites(Payload);
    return true;
}

bool CaptureEditorViewportToPng(
    FEditorViewportClient& ViewportClient,
    const TSharedPtr<FSceneViewport>& SceneViewport,
    const FViewportCaptureRequest& Request,
    const FString& DefaultPrefix,
    const FString& Subdirectory,
    FViewportCaptureOutput& OutCapture,
    FString& OutErrorCode,
    FString& OutErrorMessage,
    const FViewportCaptureHooks* Hooks)
{
    OutErrorCode.Reset();
    OutErrorMessage.Reset();

    if (!SceneViewport.IsValid())
    {
        OutErrorCode = ErrorCodes::ERR_PREVIEW_VIEWPORT_NOT_FOUND;
        OutErrorMessage = TEXT("Editor preview viewport is not available");
        return false;
    }

    FViewportCaptureRestoreReceipt DirectRestoreReceipt;
    const bool bOwnCaptureSetContext = Request.ViewportCaptureSetContext == nullptr;
    ON_SCOPE_EXIT
    {
        if (bOwnCaptureSetContext)
        {
            OutCapture.bExposureRestored = DirectRestoreReceipt.bExposureRestored;
        }
    };
    TUniquePtr<FViewportCaptureSetContext> OwnedCaptureSetContext;
    FViewportCaptureSetContext* CaptureSetContext = Request.ViewportCaptureSetContext;
    if (!CaptureSetContext)
    {
        OwnedCaptureSetContext = MakeUnique<FViewportCaptureSetContext>(
            ViewportClient, SceneViewport, Request.Exposure, DirectRestoreReceipt);
        CaptureSetContext = OwnedCaptureSetContext.Get();
    }
    if (!CaptureSetContext->IsValid())
    {
        OutErrorCode = ErrorCodes::ERR_CAPTURE_FAILED;
        OutErrorMessage = TEXT("The editor preview viewport is already owned by another capture context");
        return false;
    }

    OutCapture.EffectiveRotation = Request.Rotation;
    OutCapture.OrthoView.Reset();
    OutCapture.bOrthoRotationSnapped = false;
    OutCapture.Width = Request.Width;
    OutCapture.Height = Request.Height;
    OutCapture.Renderer = TEXT("sceneViewportReadPixels");

    if (Request.ProjectionMode == TEXT("orthographic") && !Request.bPreserveViewportType)
    {
        // Narrowed guard. An editor viewport builds its orthographic view matrix from the viewport
        // type, so the six cardinal directions are renderable and nothing else is; a tilted
        // orthographic pose has no viewport type and would silently render as something else.
        // (A SceneCapture2D-based offscreen renderer is the right path for arbitrary rotations.)
        FOrthographicViewResolution Resolution;
        if (!ResolveOrthographicView(Request.Rotation, Resolution))
        {
            OutErrorCode = ErrorCodes::ERR_UNSUPPORTED_ORTHOGRAPHIC_ROTATION;
            OutErrorMessage = FString::Printf(
                TEXT("Orthographic capture needs a camera direction along a world axis (within %.2f deg): ")
                TEXT("pitch -90 (top), pitch +90 (bottom), or pitch 0 with yaw 0 / 90 / 180 / -90. ")
                TEXT("Requested rotation (pitch=%.2f, yaw=%.2f, roll=%.2f) is tilted; the editor viewport ")
                TEXT("derives its orthographic view matrix from the viewport type, not from an arbitrary ")
                TEXT("rotation. Use projectionMode 'perspective' for a tilted view."),
                OrthoAxisToleranceDegrees, Request.Rotation.Pitch, Request.Rotation.Yaw, Request.Rotation.Roll);
            return false;
        }
        OutCapture.EffectiveRotation = Resolution.EffectiveRotation;
        OutCapture.OrthoView = Resolution.ViewName;
        OutCapture.bOrthoRotationSnapped = Resolution.bRotationSnapped;
    }

    const FVector OriginalLocation = ViewportClient.GetViewLocation();
    const FRotator OriginalRotation = ViewportClient.GetViewRotation();
    const ELevelViewportType OriginalViewportType = ViewportClient.GetViewportType();
    const float OriginalFovAngle = ViewportClient.FOVAngle;
    const float OriginalViewFov = ViewportClient.ViewFOV;
    const float OriginalOrthoZoom = ViewportClient.GetOrthoZoom();
    // The orbit pivot. Nothing on this path writes it, but ToggleOrbitCamera's conversion is
    // DEFINED in terms of it (the eye sits at LookAt plus |ViewLocation - LookAt|), so it is
    // captured with the rest of the pose rather than trusted to have held still.
    const FVector OriginalLookAt = ViewportClient.GetLookAtLocation();
    const bool bOrbitCameraAtEntry = ViewportClient.bUsingOrbitCamera;
    OutCapture.bOrbitCameraAtEntry = bOrbitCameraAtEntry;
    // Assigned after ApplyCaptureCamera, read by the scope guard below. Declared here because the
    // guard is installed before the apply.
    bool bOrbitCameraSuppressed = false;

    // Declared before the restore guard so it is destroyed after the pose/viewport restore. The
    // captured projection is governed by Request.Width/Height even when this viewport entered the
    // call locked to a camera actor with its own aspect ratio.
    FScopedCaptureProjectionAspect ProjectionAspectScope(
        ViewportClient, Request.Width, Request.Height);

    // Asset-editor preview viewports (e.g. the Static Mesh editor) are NOT realtime
    // by default: FEditorViewportClient::IsRealtime() returns false unless a realtime
    // override or RealTimeUntilFrameNumber is set. Without realtime, the manually
    // driven offscreen Draw() does not reliably composite the 3D scene pass into the
    // slate readback target -- only the HUD/canvas pass (the world-axis gizmo) lands,
    // producing a white frame with a faint gizmo stub. Push a temporary realtime
    // override (and request a few realtime frames) so the scene actually renders into
    // the captured FSceneViewport, then remove the override on scope-exit. This is the
    // engine's intended mechanism for forcing a non-realtime editor viewport to render.
    // Paired Add/Remove must use the same FText so RemoveRealtimeOverride matches it.
    const FText RealtimeOverrideName = NSLOCTEXT("PinWright", "CaptureRealtimeOverride", "PinWright Viewport Capture");
    ViewportClient.AddRealtimeOverride(true, RealtimeOverrideName);

    ON_SCOPE_EXIT
    {
        // Orbit goes back FIRST, and while the client is still in the capture's perspective type:
        // ToggleOrbitCamera converts through GetViewTransform(), so re-enabling has to see the same
        // transform ApplyCaptureCamera disabled it on, and the conversion REWRITES both location
        // and rotation -- a pose restored before it would be overwritten by the conversion. With
        // the pivot unmoved and the pose restored after it, the orbit state is exactly what it was.
        if (bOrbitCameraSuppressed)
        {
            ViewportClient.ToggleOrbitCamera(true);
            // Only on this branch. SetLookAtLocation writes through GetViewTransform() too, so on
            // a capture that changed the projection type it would put the entry pivot into the
            // wrong one of the two camera transforms; bOrbitCameraSuppressed is true only for a
            // perspective capture, which is the transform the pivot was read from.
            ViewportClient.SetLookAtLocation(OriginalLookAt, /*bRecalculateView=*/false);
        }
        ViewportClient.SetViewLocation(OriginalLocation);
        ViewportClient.SetViewRotation(OriginalRotation);
        ViewportClient.SetViewportType(OriginalViewportType);
        ViewportClient.FOVAngle = OriginalFovAngle;
        ViewportClient.ViewFOV = OriginalViewFov;
        if (OriginalOrthoZoom > 0.0f)
        {
            ViewportClient.SetOrthoZoom(OriginalOrthoZoom);
        }
        // Pop our realtime override. bCheckMissingOverride=false: the user could have
        // mutated overrides between Add and Remove, so do not assert if it is gone.
        ViewportClient.RemoveRealtimeOverride(RealtimeOverrideName, /*bCheckMissingOverride=*/false);
        // This is the per-frame hand-back. The set context restores viewport size only after the
        // final frame, while its render pins still cover any synchronous restore draw.
        ViewportClient.Invalidate();
    };

    // The pose has to be on the client BEFORE the view-distance survey: the survey measures every
    // distance from the point the renderer will cull against, and that point is read off the
    // FSceneView this pose produces. Nothing here draws -- the first pump is still below the
    // FScopedViewDistanceScale, which is what the sink ordering actually requires.
    //
    // Arm exposure, TemporalAA suppression, and native render resolution BEFORE sizing. UE 5.8
    // FSceneViewport::ResizeViewport draws synchronously (SceneViewport.cpp:1988-1995), so sizing
    // first would feed one unpinned frame into persistent render history on every shot.
    CaptureSetContext->PrepareForDraw(Request.Width, Request.Height);
    ApplyCaptureCamera(ViewportClient, SceneViewport.Get(), Request);
    bOrbitCameraSuppressed = bOrbitCameraAtEntry && !ViewportClient.bUsingOrbitCamera;
    OutCapture.bOrbitCameraSuppressed = bOrbitCameraSuppressed;

    // ---- where the camera ACTUALLY ended up ----
    // Measured off the client rather than echoed from the request, for the same reason the
    // exposure and show-flag blocks are: a viewport can ignore a write. This one did, silently,
    // for every perspective capture through an asset-editor preview -- orbit mode routed the pose
    // through the orbit pivot and the response reported the REQUEST as the pose the pixels showed.
    // An opposed-camera pair is exactly the comparison that cannot survive that, and it is the
    // primary way geometry defects get diagnosed.
    {
        const FEffectiveViewPose EffectivePose = MeasureEffectiveViewPose(ViewportClient);
        OutCapture.EffectiveLocation = EffectivePose.Location;
        // The orthographic EffectiveRotation was already resolved above from the viewport type,
        // which is what the engine renders from; the client's rotation is not consulted there and
        // must not overwrite it.
        if (ViewportClient.IsPerspective())
        {
            OutCapture.EffectiveRotation = EffectivePose.Rotation;
            const FVector RequestedForward = Request.Rotation.Vector().GetSafeNormal();
            const FVector EffectiveForward = EffectivePose.Rotation.Vector().GetSafeNormal();
            OutCapture.CameraAimErrorDegrees = FMath::RadiansToDegrees(FMath::Acos(
                FMath::Clamp(FVector::DotProduct(RequestedForward, EffectiveForward), -1.0, 1.0)));
            OutCapture.CameraAimLocationErrorCm =
                FVector::Dist(Request.Location, EffectivePose.Location);
            OutCapture.bCameraAimApplied =
                OutCapture.CameraAimErrorDegrees <= AimToleranceDegrees &&
                OutCapture.CameraAimLocationErrorCm <= AimToleranceCentimetres;
        }
        else
        {
            // An orthographic capture's in-plane orientation is quantised by design and already
            // reported through bOrthoRotationSnapped; only the eye position is a real aim here.
            OutCapture.CameraAimLocationErrorCm =
                FVector::Dist(Request.Location, EffectivePose.Location);
            OutCapture.bCameraAimApplied =
                OutCapture.CameraAimLocationErrorCm <= AimToleranceCentimetres;
        }
    }

    // ---- scoped view-mode override ----
    // Applied AFTER ApplyCaptureCamera so the viewport type is final: SetViewModes re-applies the
    // show flags for whichever slot is active (EditorViewportClient.cpp:6583-6590), and doing it
    // before the projection could change would apply them for the wrong one. Applied BEFORE
    // everything below, because CapturedViewMode is read a few lines down and the exposure pin's
    // own verdict depends on it -- a front_back_face frame is not lit, and `pinned` has to say so.
    OutCapture.bViewModeOverrideRequested = Request.ViewMode.WantsOverride();
    OutCapture.ViewModeRequestedKey = Request.ViewMode.Key;
    OutCapture.ViewModeRequestedValue = static_cast<int32>(Request.ViewMode.ViewMode);
    OutCapture.ViewModeShowFlags = Request.ViewMode.DistinguishingShowFlags;
    OutCapture.ViewModeCompanion = Request.ViewMode.CompanionMode;

    // Declared BEFORE the scope object so it destructs AFTER it -- the restore verdict has to be
    // read once BOTH slots are already back. A capture that leaves a debug mode on the viewport
    // changes what the USER is looking at, not just the next capture, which is the entire reason
    // this parameter exists instead of a call to editor.set_view_mode.
    ON_SCOPE_EXIT
    {
        OutCapture.ViewModePerspAfterKey = GetViewModeKey(ViewportClient.GetPerspViewMode());
        OutCapture.ViewModeOrthoAfterKey = GetViewModeKey(ViewportClient.GetOrthoViewMode());
        OutCapture.bViewModeRestored =
            OutCapture.ViewModePerspAfterKey == OutCapture.ViewModePerspBeforeKey &&
            OutCapture.ViewModeOrthoAfterKey == OutCapture.ViewModeOrthoBeforeKey;
    };
    ON_SCOPE_EXIT
    {
        // SetViewModes restores an entire show-flag preset. Reassert the set-owned pins after that
        // per-frame scope closes so they remain in force between consecutive shots.
        CaptureSetContext->ReassertPins();
    };

    FScopedViewModeOverride ViewModeScope(ViewportClient, Request.ViewMode);
    OutCapture.ViewModePerspBeforeKey = GetViewModeKey(ViewModeScope.GetPreviousPersp());
    OutCapture.ViewModeOrthoBeforeKey = GetViewModeKey(ViewModeScope.GetPreviousOrtho());
    if (ViewModeScope.WasApplied())
    {
        // Read off the client, not echoed. Two independent measurements, because they can fail
        // separately: the SLOTS say the assignment landed on both projections, and the SHOW FLAGS
        // say the mode reached the state the renderer actually reads. A response that only
        // asserted the call succeeded is how camera.orbit_shots' `viewMode` shipped inert.
        OutCapture.bViewModeApplied =
            ViewportClient.GetPerspViewMode() == Request.ViewMode.ViewMode &&
            ViewportClient.GetOrthoViewMode() == Request.ViewMode.ViewMode;
        OutCapture.ViewModeShowFlagMismatches = PinWrightViewModes::MeasureShowFlagMismatches(
            ViewportClient.EngineShowFlags, Request.ViewMode.ViewMode, ViewportClient.IsPerspective());
        OutCapture.bViewModeApplied =
            OutCapture.bViewModeApplied && OutCapture.ViewModeShowFlagMismatches.Num() == 0;
    }

    // SetViewModes can rewrite TemporalAA and EyeAdaptation. Put the set-owned pins back before
    // the first explicit capture draw and leave them armed between frames.
    CaptureSetContext->ReassertPins();
    CaptureSetContext->PopulateFrameEvidence(OutCapture);
    const EAntiAliasingMethod AntiAliasingMethod =
        EffectiveStillAntiAliasingMethod(ViewportClient);
    OutCapture.RenderAntiAliasingMethodValue = static_cast<int32>(AntiAliasingMethod);
    OutCapture.RenderAntiAliasingMethod = GetShortAntiAliasingName(AntiAliasingMethod);

    // ---- the preview-scene rig ----
    //
    // Resolved BEFORE the first pump, for the same reason the exposure pin and the sprite flag
    // are: the rig has to be on the scene before anything draws, or the first frames are lit by
    // the rig this call was supposed to replace.
    //
    // REFUSED, NOT IGNORED, when the viewport cannot carry it. A rig that silently does nothing
    // returns a frame lit by a setup the caller believes they chose -- the same defect class as
    // camera.orbit_shots' inert `viewMode`, and there is no field that could distinguish the two
    // after the fact.
    OutCapture.bPreviewSceneRigRequested = Request.PreviewSceneRig.WantsRig();
    TOptional<PinWrightPreviewSceneRig::FSharedProfileSnapshot> OwnedRigSnapshot;
    TUniquePtr<PinWrightPreviewSceneRig::FScopedPreviewSceneRig> OwnedRigScope;
    ON_SCOPE_EXIT
    {
        if (!OwnedRigScope)
        {
            return;
        }
        OwnedRigScope.Reset();
        OutCapture.PreviewSceneRigAfter = PinWrightPreviewSceneRig::MeasureRig(ViewportClient);
        OutCapture.bPreviewSceneRigRestored = PinWrightPreviewSceneRig::RigReportsMatch(
            OutCapture.PreviewSceneRigAfter, OutCapture.PreviewSceneRigBefore);

        if (OwnedRigSnapshot.IsSet())
        {
            const PinWrightPreviewSceneRig::FSharedProfileSnapshot& Snapshot =
                OwnedRigSnapshot.GetValue();
            if (UAssetViewerSettings* RigSettings = UAssetViewerSettings::Get())
            {
                OutCapture.bSharedProfilesRestored = PinWrightPreviewSceneRig::SharedProfilesMatch(
                    RigSettings->Profiles, Snapshot.Profiles);
            }
            OutCapture.ConfigFileDigest = PinWrightPreviewSceneRig::DigestFile(
                PinWrightPreviewSceneRig::PreviewSceneConfigFilePath());
            OutCapture.bConfigFileUnchanged =
                OutCapture.ConfigFileDigest == Snapshot.ConfigDigest;
        }
    };

    if (Request.bPreviewSceneRigAlreadyScoped)
    {
        OutCapture.PreviewSceneRigBefore = Request.PreviewSceneRigAtSetEntry;
        OutCapture.bPreviewSceneCaptureUpdated =
            Request.bPreviewSceneCaptureUpdatedAtSetEntry;
        OutCapture.bPreviewSceneCaptureIncomplete =
            Request.bPreviewSceneCaptureIncompleteAtSetEntry;
    }
    else
    {
        if (!PinWrightPreviewSceneRig::CanApplyRig(ViewportClient, Request.PreviewSceneRig,
                OutErrorCode, OutErrorMessage))
        {
            return false;
        }
        OwnedRigSnapshot = PinWrightPreviewSceneRig::CaptureSharedProfiles();
        OwnedRigScope = MakeUnique<PinWrightPreviewSceneRig::FScopedPreviewSceneRig>(
            ViewportClient, Request.PreviewSceneRig);
        OutCapture.PreviewSceneRigBefore = OwnedRigScope->GetPreviousRig();
        // The guard drove the sky/reflection capture drain in its constructor, which is BEFORE the
        // first pump below -- the whole point, since the settle loop's own pump cannot drive it.
        OutCapture.bPreviewSceneCaptureUpdated = OwnedRigScope->CaptureContentsUpdated();
        OutCapture.bPreviewSceneCaptureIncomplete = OwnedRigScope->CaptureContentsIncomplete();
    }
    // MEASURED off the live scene after the write, never echoed. This is the block that says what
    // rig these pixels were actually drawn under -- including on the omitted-parameter path, where
    // it is the editor's own rig and is exactly the fact that made two machines' captures
    // incomparable with nothing in the response to show it.
    OutCapture.PreviewSceneRigDrawn = PinWrightPreviewSceneRig::MeasureRig(ViewportClient);
    if (Request.PreviewSceneRig.WantsRig())
    {
        OutCapture.bPreviewSceneRigApplied =
            PinWrightPreviewSceneRig::RigMatchesRequest(
                Request.PreviewSceneRig, OutCapture.PreviewSceneRigDrawn);
    }

    // ---- editor sprite suppression ----
    // Resolved BEFORE the first pump, for the same reason the exposure pin below is: the flag has
    // to be on the client before anything draws, or the first frames carry the icons the caller
    // asked to remove.
    OutCapture.bHideEditorSpritesRequested = Request.bHideEditorSprites;

    // Declared BEFORE the scope object so it destructs AFTER it -- the restore verdict has to be
    // read once the flag is already back. A capture that leaves BillboardSprites cleared changes
    // every later frame the USER sees in that viewport, not just the next capture, so it is
    // measured rather than assumed.
    ON_SCOPE_EXIT
    {
        OutCapture.bEditorSpritesRestored =
            (ReadEditorSpriteShowFlags(ViewportClient.EngineShowFlags).bBillboardSprites ==
                OutCapture.bBillboardSpritesBefore);
    };

    FScopedEditorSpriteSuppression EditorSpriteScope(ViewportClient, Request.bHideEditorSprites);
    OutCapture.bBillboardSpritesBefore = EditorSpriteScope.GetPreviousFlags().bBillboardSprites;
    // Read off the client, not echoed from the request: a viewport a human already had in game
    // view, or with the flag cleared by hand, renders sprite-free whether or not this call asked
    // for it, and a caller comparing two frames needs the state the pixels were drawn in.
    OutCapture.bBillboardSpritesApplied = ViewportClient.EngineShowFlags.BillboardSprites != 0;

    // ---- exposure pin ----
    // Resolved BEFORE the first pump, for the same reason the view-distance scale is: the override
    // has to be on the client before anything draws, or the first frames render at the exposure
    // the pin was supposed to replace.
    //
    // The view mode is read HERE rather than in the reporting block below because whether the
    // renderer honours the pin depends on it (ExposurePinGovernsFrame). Reading it after
    // ApplyCaptureCamera is what makes it the mode the pixels will be drawn in.
    const EViewModeIndex CapturedViewMode = ViewportClient.GetViewMode();
    // Request, entry, and live read-back fields were populated by the set context above. It stays
    // alive through every frame; the final restore receipt is copied only after the set closes.

    // Does the renderer's fixed-exposure branch govern these pixels? Asked once, for ANY viewport
    // carrying bFixed -- including one a human left pinned through the editor's EV100 control --
    // rather than only for a pin this call wrote. Two things downstream need the same answer: the
    // reported `pinned` (only meaningful when a pin was requested) and which source the reported
    // exposure gain may come from (below), and computing it twice is how those two drift apart.
    FString FixedExposureBlockedReason;
    const bool bFixedExposureGovernsFrame = OutCapture.bExposureFixedApplied &&
        ExposurePinGovernsFrame(ViewportClient, IsLitViewMode(CapturedViewMode),
            FixedExposureBlockedReason);

    if (Request.Exposure.WantsPin())
    {
        if (!OutCapture.bExposureFixedApplied ||
            !FMath::IsNearlyEqual(OutCapture.Ev100Applied, Request.Exposure.Ev100, UE_KINDA_SMALL_NUMBER))
        {
            // The write did not stick. This is a broken mechanism rather than a view-mode caveat,
            // and the caller asked for a guarantee this frame cannot carry -- so no frame is
            // returned. The set context restores everything it owns on this error path.
            OutErrorCode = ErrorCodes::ERR_EXPOSURE_PIN_FAILED;
            OutErrorMessage = FString::Printf(
                TEXT("Requested an exposure pin at EV100 %.4f, but the viewport client read back "
                     "bFixed=%s / FixedEV100=%.4f immediately after the write. The pin did not take, "
                     "so this frame would have used auto-exposure while reporting otherwise."),
                Request.Exposure.Ev100,
                OutCapture.bExposureFixedApplied ? TEXT("true") : TEXT("false"),
                OutCapture.Ev100Applied);
            return false;
        }
        // Written and stuck; the measurement of whether the RENDERER applies it to a frame drawn
        // in this state was taken above. This is the half that a read-back of our own write cannot
        // establish.
        OutCapture.bExposurePinned = bFixedExposureGovernsFrame;
        OutCapture.ExposurePinBlockedReason = FixedExposureBlockedReason;
    }

    // ---- view-distance override (task #96: a wide ortho drops distant foliage) ----
    // Resolve the scale BEFORE the first pump: the sink that publishes it into the renderer's
    // scalability cache must run before anything draws, or the first frames render culled.
    float RequestedViewDistanceScale = 0.0f;
    OutCapture.ViewDistanceScaleSource = TEXT("none");
    if (Request.bViewDistanceScaleProvided)
    {
        RequestedViewDistanceScale = Request.ViewDistanceScale;
        OutCapture.ViewDistanceScaleSource = TEXT("caller");
    }
    else if (Request.bAutoViewDistanceScale && Request.ProjectionMode == TEXT("orthographic"))
    {
        // Orthographic only, deliberately. An ortho frame's coverage is set by orthoWidth and does
        // not shrink as the camera pulls back, so its corners routinely sit outside every authored
        // cull distance -- that is the defect. A perspective frame's coverage and its cull distances
        // scale together, it does not have the same failure by construction, and auto-enabling it
        // there would move every perspective measurement already recorded against this verb. Pass
        // viewDistanceScale explicitly to override a perspective shot.
        bool bOriginMeasured = false;
        const FVector CullingOrigin =
            MeasureCaptureCullingOrigin(ViewportClient, SceneViewport, bOriginMeasured);
        OutCapture.ViewDistanceSurvey = SurveyViewDistances(ViewportClient.GetWorld(), CullingOrigin);
        OutCapture.ViewDistanceSurvey.bCullingOriginMeasured = bOriginMeasured;
        OutCapture.ViewDistanceSurvey.CullingOriginPushback =
            FVector::Dist(CullingOrigin, ViewportClient.GetViewLocation());

        const float AutoScale = ComputeAutoViewDistanceScale(OutCapture.ViewDistanceSurvey);
        if (AutoScale > 1.0f)
        {
            RequestedViewDistanceScale = AutoScale;
            OutCapture.ViewDistanceScaleSource = TEXT("auto");
            // Name WHICH cap bound. "the scale was clipped" is not actionable; "the int32 bound in
            // the foliage path clipped it" tells a caller to narrow orthoWidth and tile instead.
            const double Required =
                OutCapture.ViewDistanceSurvey.RequiredScale * AutoViewDistanceScaleMargin;
            if (Required > static_cast<double>(AutoScale) + UE_KINDA_SMALL_NUMBER)
            {
                OutCapture.bViewDistanceScaleClamped = true;
                const double NearCap =
                    (OutCapture.ViewDistanceSurvey.MaxMinDrawDistance > 0.0 &&
                     OutCapture.ViewDistanceSurvey.MinPrimitiveDistance > 0.0)
                        ? OutCapture.ViewDistanceSurvey.MinPrimitiveDistance /
                              OutCapture.ViewDistanceSurvey.MaxMinDrawDistance
                        : TNumericLimits<double>::Max();
                if (NearCap <= static_cast<double>(AutoScale) + UE_KINDA_SMALL_NUMBER)
                {
                    OutCapture.ViewDistanceClampReason = TEXT("nearCull");
                }
                else if (AutoScale >= MaxAutoViewDistanceScale - UE_KINDA_SMALL_NUMBER)
                {
                    OutCapture.ViewDistanceClampReason = TEXT("absolute");
                }
                else
                {
                    OutCapture.ViewDistanceClampReason = TEXT("int32");
                }
            }
        }
    }

    // Declared BEFORE the scope object so it destructs AFTER it: the verdict has to be read once
    // the cvar is already back. "Restored" is a read-back, not an assumption -- a capture that
    // leaves a global rendering setting moved makes every later capture in the session
    // incomparable, and that is worth reporting rather than trusting.
    ON_SCOPE_EXIT
    {
        const float ScaleAfterRestore = ReadViewDistanceScale();
        OutCapture.bViewDistanceScaleRestored =
            FMath::IsNearlyEqual(ScaleAfterRestore, OutCapture.ViewDistanceScaleBefore, UE_KINDA_SMALL_NUMBER);
    };

    FScopedInstancedCullRebake InstancedCullRebake(ViewportClient.GetWorld());
    FScopedViewDistanceScale ViewDistanceScope(RequestedViewDistanceScale);
    OutCapture.ViewDistanceScaleBefore = ViewDistanceScope.GetPreviousScale();
    OutCapture.ViewDistanceScaleApplied = ViewDistanceScope.GetAppliedScale();
    OutCapture.bViewDistanceScaleOverridden = ViewDistanceScope.WasApplied();
    if (!ViewDistanceScope.WasApplied())
    {
        OutCapture.ViewDistanceScaleSource = TEXT("none");
        OutCapture.bViewDistanceScaleClamped = false;
        OutCapture.ViewDistanceClampReason.Reset();
    }

    // What the RENDERER will read, taken from its scalability cache AFTER the scope has written
    // the cvar and run the sinks -- not from the cvar, and not assumed equal to it. Two things
    // can separate the two: r.ViewDistanceScale.ApplySecondaryScale folds SecondaryScale into the
    // cached value (UnrealEngine.cpp:905-906), and the cache refreshes only on a sink. Comparing
    // this against what the survey asked for is the only check here that the write path cannot
    // fake -- everything else is the capture grading its own homework.
    OutCapture.ViewDistanceScaleEffective = ReadEffectiveViewDistanceScale();
    if (ViewDistanceScope.WasApplied())
    {
        InstancedCullRebake.Rebake();
    }
    OutCapture.bViewDistanceCoverageSufficient =
        !OutCapture.ViewDistanceSurvey.bValid ||
        OutCapture.ViewDistanceSurvey.RequiredScale <= 0.0 ||
        static_cast<double>(OutCapture.ViewDistanceScaleEffective) >= OutCapture.ViewDistanceSurvey.RequiredScale;

    OutCapture.ViewportTypeValue = static_cast<int32>(ViewportClient.GetViewportType());
    OutCapture.ViewportType = ViewportTypeName(ViewportClient.GetViewportType());
    // CapturedViewMode was read AFTER ApplyCaptureCamera in the exposure block above (the pin's
    // verdict needs it), so what is reported is the mode the pixels are about to be rendered in
    // rather than a pre-setup snapshot.
    OutCapture.ViewModeValue = static_cast<int32>(CapturedViewMode);
    // Through the vocabulary, not the raw engine call: UViewModeUtils::GetViewModeDisplayName
    // fires an ensure for VMI_VisualizeSubstrate and VMI_VisualizeGroom, both of which a viewport
    // can be sitting in when a capture is taken (see PinWrightViewModes::GetDisplayName).
    OutCapture.ViewMode = PinWrightViewModes::GetDisplayName(CapturedViewMode);
    OutCapture.ViewModeKey = GetViewModeKey(CapturedViewMode);
    OutCapture.bLitViewMode = IsLitViewMode(CapturedViewMode);
    OutCapture.bGameView = ViewportClient.IsInGameView();
    OutCapture.bRealtime = ViewportClient.IsRealtime();
    // Surveyed here, beside the flags the client reports, because a forced ShowFlag.* cvar is
    // applied to the VIEW and never to the client -- there is nothing on ViewportClient to read it
    // off, and every field in this block would otherwise read clean on a frame two-thirds covered
    // by a debug visualization. Taken before the draws below, so it describes the state the pixels
    // were rendered under rather than whatever the process reached afterwards.
    OutCapture.ForcedShowFlags = SurveyForcedShowFlagOverrides();
    OutCapture.bShowFlagOverridesMeasured = true;
    // The other half of the same question, and the half the forced-cvar survey cannot answer: the
    // viewport's OWN overlay flags. Read here, off the client that is about to draw, and taken at
    // the same point for the same reason -- `gameView` a line above is not an answer about
    // overlays, because SetGameView can reuse a saved flag set that still carries Splines. This is
    // the read that puts the spline line into the RESPONSE instead of leaving it only in the
    // pixels, where it has twice nearly been reported as authored map content.
    OutCapture.OverlayShowFlags = PinWrightReadGameViewOverlayFlags(ViewportClient.EngineShowFlags);
    OutCapture.bOverlayShowFlagsMeasured = true;

    // ---- landscape grass, settled against the pose these pixels will show ----
    //
    // BEFORE the first pump, for the same reason the exposure pin and the show-flag survey are:
    // grass built after the draw is grass that is not in the frame. AFTER ApplyCaptureCamera and
    // after the effective pose was measured, because the location handed to the build is the
    // MEASURED eye (an orbit-mode viewport puts it somewhere other than the request).
    //
    // The pose parameters alone never reached this build. Landscape grass is keyed on CAMERA
    // LOCATIONS collected on the WORLD tick, which no capture runs, and the entry pose is back on
    // the client before the editor next ticks -- so a pose-driven capture photographed grass built
    // around the persistent viewport camera and said nothing about it. See LandscapeGrassSettle.h
    // for the measurements. Nothing here moves a viewport camera: the location goes to
    // RegenerateGrass as data.
    OutCapture.GrassBuild = PinWrightCaptureGrass::SettleGrassForCapturePose(
        ViewportClient.GetWorld(), OutCapture.EffectiveLocation);

    // ---- and whether any of that grass can reach THIS frame ----
    //
    // The build above answers "was grass made for this pose"; it does not answer "will the
    // renderer draw it", and an ORTHOGRAPHIC frame is where those come apart. Grass instances are
    // culled against InstanceEndCullDistance measured from FSceneView::CullingOrigin, which for a
    // lit editor ortho view is not the camera -- so a settled build of millions of instances can
    // render as bare ground with every field in the block above reading clean. That is
    // B-ortho-capture-renders-no-landscape-grass, and this is the measurement that ticket asked
    // for: "report whether landscape grass components exist in the scene and how many of their
    // instances are in the frustum, so the next investigator can tell 'not built' from 'built and
    // not drawn' without guessing."
    //
    // Only when there is grass to measure, so a level with no terrain pays nothing. The origin is
    // reused from the view-distance survey where that ran; otherwise one is measured here, off the
    // same FSceneView the capture is about to draw.
    if (OutCapture.GrassBuild.ComponentsAfter > 0)
    {
        FVector GrassCullingOrigin = OutCapture.EffectiveLocation;
        bool bGrassCullingOriginMeasured = false;
        if (OutCapture.ViewDistanceSurvey.bValid && OutCapture.ViewDistanceSurvey.bCullingOriginMeasured)
        {
            GrassCullingOrigin = OutCapture.ViewDistanceSurvey.CullingOrigin;
            bGrassCullingOriginMeasured = true;
        }
        else
        {
            GrassCullingOrigin =
                MeasureCaptureCullingOrigin(ViewportClient, SceneViewport, bGrassCullingOriginMeasured);
        }
        PinWrightCaptureGrass::MeasureGrassFrameReach(ViewportClient.GetWorld(), GrassCullingOrigin,
            bGrassCullingOriginMeasured, OutCapture.ViewDistanceScaleEffective, OutCapture.GrassBuild);
    }

    if (Hooks && Hooks->BeforeWarmup &&
        !Hooks->BeforeWarmup(OutErrorCode, OutErrorMessage))
    {
        return false;
    }

    // The realtime override above already forces IsRealtime() true for the whole
    // capture scope, so the scene render -- not just the canvas/HUD pass -- composites
    // across every pump. RequestRealTimeFrames is a one-time belt-and-suspenders arm
    // of RealTimeUntilFrameNumber covering the first frames; no need to re-arm it each
    // iteration (the override is the single source of realtime state for the loop).
    ViewportClient.RequestRealTimeFrames(2);
    for (int32 Attempt = 0; Attempt < 3; ++Attempt)
    {
        PumpViewport(ViewportClient, SceneViewport);
    }

    TArray<FColor> ColorData;
    auto ReadViewport = [&]() -> bool
    {
        ColorData.Reset();
        if (!SceneViewport->ReadPixels(ColorData) ||
            ColorData.Num() != Request.Width * Request.Height)
        {
            return false;
        }
        PinWrightScreenshotUtils::ForceOpaqueAlpha(ColorData);
        OutCapture.ImageStats = CalculateCaptureImageStats(ColorData);
        // Assigned inside ReadViewport rather than after it, so a redraw retry replaces the
        // retained buffer instead of leaving the first, rejected read behind.
        if (Request.bRetainPixels)
        {
            OutCapture.Pixels = ColorData;
        }
        return true;
    };

    if (!ReadViewport())
    {
        OutErrorCode = ErrorCodes::ERR_CAPTURE_FAILED;
        OutErrorMessage = TEXT("Failed to read pixels from editor viewport");
        return false;
    }

    // --- warm-up settle ---
    //
    // Three pumps is not a convergence test, it is a guess, and on a viewport that has just been
    // created it is the wrong one: the capture that OPENED the Static Mesh editor read
    // meanLuminance 0.0506 where the identical pinned capture a call later read 0.2437 (measured
    // 2026-08-19, /Engine/BasicShapes/Cube, ev100 0, 512x512). 2.26 stops, at a pinned exposure
    // whose entire purpose is to make two captures comparable, and auto-exposure hides it by
    // re-exposing -- so it corrupts exactly the workflow the pin exists for.
    //
    // Why this and not the eye-adaptation readback: that flag was tried as the detector and is not
    // one. Reopening the asset editor in a separate call and then capturing gives a fully warmed
    // frame with the readback still pending, twice out of two -- it tracks "this viewport is
    // young", not "this frame is unfinished", so gating on it would discard good captures.
    //
    // Pumping until the frame mean stops moving measures the thing that actually varies. A settled
    // viewport pays one extra pump and one extra ReadPixels, which is the irreducible cost of
    // observing that a frame did not move; an unsettled one pays up to MaxWarmupSettleRounds or
    // MaxWarmupSettleMs, whichever binds first, and then returns the frame it has with
    // bWarmupSettled false rather than hanging. Every capture publishes the round count, the
    // residual delta and the milliseconds, so the cost is a measurement in each response rather
    // than an assumption made here.
    {
        const double SettleStartSeconds = FPlatformTime::Seconds();
        double PreviousMean = OutCapture.ImageStats.MeanLuminance;
        TArray<FColor> PreviousPixels = MoveTemp(ColorData);
        while (OutCapture.WarmupSettleRounds < MaxWarmupSettleRounds)
        {
            PumpViewport(ViewportClient, SceneViewport);
            if (!ReadViewport())
            {
                OutErrorCode = ErrorCodes::ERR_CAPTURE_FAILED;
                OutErrorMessage = TEXT("Failed to read pixels from editor viewport while waiting "
                    "for the frame to settle");
                return false;
            }
            ++OutCapture.WarmupSettleRounds;

            const double Delta = FMath::Abs(OutCapture.ImageStats.MeanLuminance - PreviousMean);
            OutCapture.WarmupMeanLuminanceDelta = Delta;
            PreviousMean = OutCapture.ImageStats.MeanLuminance;
            const PinWrightFlatRegion::FFrameDifferenceStats PixelDelta =
                PinWrightFlatRegion::MeasureFrameDifference(
                    PreviousPixels, ColorData, OutCapture.WarmupChannelThreshold);
            OutCapture.bWarmupPixelChangeMeasured = PixelDelta.bMeasured;
            OutCapture.WarmupMeanAbsDelta = PixelDelta.MeanAbsDelta;
            OutCapture.WarmupMaxDelta = PixelDelta.MaxDelta;
            OutCapture.WarmupChangedPixelFraction = PixelDelta.ChangedPixelFraction;

            // Absolute OR relative: the absolute term carries a dark frame, where a relative
            // tolerance on a near-zero mean is noise, and the relative term carries a bright one.
            const double Tolerance = FMath::Max(WarmupSettleAbsTolerance,
                WarmupSettleRelTolerance * OutCapture.ImageStats.MeanLuminance);
            if (Delta <= Tolerance)
            {
                OutCapture.bWarmupSettled = true;
                break;
            }
            if ((FPlatformTime::Seconds() - SettleStartSeconds) * 1000.0 >= MaxWarmupSettleMs)
            {
                break;
            }
            if (OutCapture.WarmupSettleRounds < MaxWarmupSettleRounds)
            {
                PreviousPixels = MoveTemp(ColorData);
            }
        }
        OutCapture.WarmupSettleMs = (FPlatformTime::Seconds() - SettleStartSeconds) * 1000.0;
    }

    // The exposure gain these pixels were drawn with, taken after the frames have been drawn.
    //
    // THE READBACK IS NOT A SOURCE FOR A PINNED FRAME. Clearing the EyeAdaptation show flag stops
    // the scene-luminance pass that refreshes GetLastEyeAdaptationExposure, so the readback does
    // not go to zero as an earlier version of this comment assumed -- it FREEZES at whatever the
    // last auto-exposed frame left behind. Measured 2026-08-19 on a preview viewport pinned in
    // turn at EV100 0, 4.34 and -1.90: 3.1218, 3.1219, 3.1070. One stale number, reported for
    // three frames six stops apart, and the caller's only handle on "what exposure did I get".
    //
    // On the fixed branch the gain is not a measurement at all. The renderer computes it in
    // closed form from FixedEV100 (GetEyeAdaptationFixedExposure, PostProcessEyeAdaptation.cpp
    // :810-825, over the bFixed branch of GetEyeAdaptationScalarParameters at :639-647), so
    // Ev100ToExposureGain reproduces exactly the number the tonemapper used -- with no readback
    // latency, and no dependence on how many frames were pumped.
    //
    // Gated on the pin GOVERNING the frame, not merely being written: in a debug/unlit view mode
    // the whole override chain is skipped (ExposurePinGovernsFrame) and the pixels really did use
    // the auto path, so there the readback is the honest answer.
    const float ReadbackGain = ReadLastAdaptedExposure(ViewportClient);
    // A zero readback means the renderer has not completed one for this viewport yet, which is a
    // statement about the VIEWPORT rather than about the exposure, and it survives the pinned
    // branch below.
    //
    // It is NOT a warm-up detector, though it was published as one. Measured 2026-08-19: reopening
    // the Static Mesh editor in a separate call and then capturing gives a fully warmed frame
    // (meanLuminance 0.2437, 0.2436 vs 0.0506 for open-and-capture in one call) with this flag
    // still true both times. It tracks how young the viewport's view state is, not whether the
    // frame finished. `warmup` (the settle block above) is the measurement that answers that.
    const bool bReadbackPending = !(ReadbackGain > 0.0f);
    OutCapture.bAdaptedExposureReadbackPending = bReadbackPending;

    if (OutCapture.bExposureFixedApplied && bFixedExposureGovernsFrame)
    {
        OutCapture.AdaptedExposure = Ev100ToExposureGain(OutCapture.Ev100Applied);
        OutCapture.bAdaptedExposureMeasured = true;
        OutCapture.bAdaptedExposureFromPin = true;
    }
    else
    {
        // Zero means there is no completed readback. Reported as unmeasured rather than as a zero,
        // so it cannot be read as "the exposure was 0" (rpc-design.md section 4).
        OutCapture.AdaptedExposure = ReadbackGain;
        OutCapture.bAdaptedExposureMeasured = !bReadbackPending;
    }

    bool bFinalStageStarted = false;
    const auto FinishFinalStage = [&]()
    {
        if (bFinalStageStarted && Hooks && Hooks->AfterFinalFrame)
        {
            Hooks->AfterFinalFrame();
        }
        bFinalStageStarted = false;
    };
    ON_SCOPE_EXIT
    {
        FinishFinalStage();
    };
    if (Hooks && Hooks->BeforeFinalFrame)
    {
        bFinalStageStarted = true;
        if (!Hooks->BeforeFinalFrame(OutCapture, OutErrorCode, OutErrorMessage))
        {
            return false;
        }

        PumpViewport(ViewportClient, SceneViewport);
        if (!ReadViewport())
        {
            OutErrorCode = ErrorCodes::ERR_CAPTURE_FAILED;
            OutErrorMessage = TEXT("Failed to read pixels from editor viewport after the final capture stage");
            return false;
        }
    }

    if (Request.bRejectBlankCapture && !Request.bAllowBlank && OutCapture.ImageStats.bBlank)
    {
        // One bounded redraw retry. Re-arming realtime before the draw covers a viewport that
        // returned a stale/cleared backbuffer on the first read without creating an unbounded
        // retry loop that can hang automation.
        OutCapture.RedrawRetries = 1;
        ViewportClient.RequestRealTimeFrames(2);
        PumpViewport(ViewportClient, SceneViewport);
        if (!ReadViewport())
        {
            OutErrorCode = ErrorCodes::ERR_CAPTURE_FAILED;
            OutErrorMessage = TEXT("Failed to read pixels from editor viewport after redraw retry");
            return false;
        }
        if (OutCapture.ImageStats.bBlank)
        {
            OutErrorCode = ErrorCodes::ERR_BLANK_CAPTURE;
            OutErrorMessage = FString::Printf(
                TEXT("Editor viewport capture remained near-uniform black after one redraw retry: ")
                TEXT("%lld of %d pixels are above luminance %.2f (%.6f of the frame), and the frame ")
                TEXT("mean is %.6f. Both figures are what the verdict is made of, and neither ")
                TEXT("depends on the capture size -- re-shooting at a different width/height will ")
                TEXT("give the same answer. (luminanceVariance=%.8f, minLuminance=%.6f, ")
                TEXT("maxLuminance=%.6f.) Verify the viewport/world and camera state, or pass ")
                TEXT("allowBlank:true for an intentional black frame."),
                static_cast<long long>(OutCapture.ImageStats.LitPixelCount),
                Request.Width * Request.Height,
                BlankLitLuminanceThreshold,
                OutCapture.ImageStats.LitPixelFraction,
                OutCapture.ImageStats.MeanLuminance,
                OutCapture.ImageStats.LuminanceVariance,
                OutCapture.ImageStats.MinLuminance,
                OutCapture.ImageStats.MaxLuminance);
            return false;
        }
    }

    // Pixel acquisition, including the bounded blank retry, is complete. Release any scoped
    // world-time freeze before image analysis, encoding, and disk I/O; the scope guard above still
    // covers every early return while a final-stage hook is armed.
    FinishFinalStage();

    if (!CaptureSetContext->PopulateFrameEvidence(OutCapture))
    {
        OutErrorCode = ErrorCodes::ERR_CAPTURE_FAILED;
        OutErrorMessage = TEXT("The editor viewport drew without exposing its effective render resolution");
        return false;
    }

    // --- the SUBJECT's own luminance, as opposed to the frame's ---
    //
    // Taken HERE, once, on the settled buffer: after the warm-up settle and after the blank-redraw
    // retry, so it describes the pixels that reach the caller rather than an intermediate frame,
    // and not inside ReadViewport, which runs once per settle round. Before the PNG encode, for
    // the same reason bRetainPixels retains before it -- what is measured is what ReadPixels
    // returned, not a decoded PNG in a different gamma space.
    //
    // Costs one pass over the frame, and only when the caller supplied bounds. A request without
    // them reports `measured: false` and the reason, which is the honest answer for a verb that
    // has no subject (B-capture-asset-preview-renders-foliage-black).
    {
        PinWrightSubjectRegion::FSubjectRegionView RegionView;
        // The EFFECTIVE pose, never the requested one, for exactly the reason
        // EvaluateBoundsFraming uses it: an orbit-mode preview viewport derives its own eye and a
        // region measured off the request would be measured off pixels that were never rendered.
        RegionView.CameraLocation = OutCapture.EffectiveLocation;
        RegionView.CameraRotation = OutCapture.EffectiveRotation;
        RegionView.Width = Request.Width;
        RegionView.Height = Request.Height;
        RegionView.bOrthographic = Request.ProjectionMode == TEXT("orthographic");
        RegionView.Fov = static_cast<double>(Request.Fov);
        RegionView.OrthoWidth = static_cast<double>(Request.OrthoWidth);
        RegionView.BoundsOrigin = Request.BoundsOrigin;
        RegionView.BoundsRadius = Request.BoundsRadius;
        // The exposure the pixels were ACTUALLY drawn at, read back off the client -- not
        // Request.Exposure, because a viewport left pinned by the editor's own EV100 control
        // renders fixed without this call asking for anything, and that caller is the one who
        // most needs the silhouette warning to say so. Only the warning reads these; the
        // published numbers stay in `viewport.exposure`.
        RegionView.bFixedExposure = OutCapture.bExposureFixedApplied;
        RegionView.Ev100 = static_cast<double>(OutCapture.Ev100Applied);
        OutCapture.SubjectRegion = PinWrightSubjectRegion::MeasureSubjectRegion(
            ColorData, RegionView, BlankLitLuminanceThreshold);
    }

    // The editor back buffer carries alpha 0 over every SCENE pixel -- only Slate-composited editor
    // overlays (the world-axis gizmo, the ortho scale bar) land with alpha 255 -- so encoding the
    // read-back alpha produced a PNG that was 99.97% transparent. Consumers that composite alpha
    // showed a blank frame with just the gizmo and scale bar on their own page background, while any
    // consumer that re-encoded the image without an alpha channel (e.g. a large capture downsampled
    // to JPEG) showed the very same file rendering fine. That size-dependent split is what made this
    // read as "horizontal orthographic renders no geometry" while the bigger perspective and top-down
    // shots looked correct: the geometry was always in the RGB channels
    // (B-horizontal-orthographic-views-render-no-geometry).
    TArray64<uint8> PngData;
    FImageUtils::PNGCompressImageArray(Request.Width, Request.Height,
        TArrayView64<const FColor>(ColorData.GetData(), ColorData.Num()), PngData);
    if (PngData.Num() == 0)
    {
        OutErrorCode = ErrorCodes::ERR_ENCODE_FAILED;
        OutErrorMessage = TEXT("Failed to encode editor viewport capture as PNG");
        return false;
    }

    FString Filename;
    const FString OutputPath = PinWrightScreenshotUtils::MakeScreenshotOutputPath(
        Request.Filename, DefaultPrefix, Subdirectory, Filename);
    if (!FFileHelper::SaveArrayToFile(PngData, *OutputPath))
    {
        OutErrorCode = ErrorCodes::ERR_SAVE_FAILED;
        OutErrorMessage = FString::Printf(TEXT("Failed to save viewport capture: %s"), *OutputPath);
        return false;
    }

    OutCapture.Path = OutputPath;
    OutCapture.Filename = Filename;
    OutCapture.SizeBytes = PngData.Num();
    return true;
}

TSharedPtr<FJsonObject> MakeVectorObject(const FVector& Value)
{
    TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
    Object->SetNumberField(TEXT("x"), Value.X);
    Object->SetNumberField(TEXT("y"), Value.Y);
    Object->SetNumberField(TEXT("z"), Value.Z);
    return Object;
}

TSharedPtr<FJsonObject> MakeRotatorObject(const FRotator& Value)
{
    TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
    Object->SetNumberField(TEXT("pitch"), Value.Pitch);
    Object->SetNumberField(TEXT("yaw"), Value.Yaw);
    Object->SetNumberField(TEXT("roll"), Value.Roll);
    return Object;
}

TSharedPtr<FJsonObject> MakeViewDistanceInfoObject(const FViewportCaptureOutput& Capture)
{
    TSharedPtr<FJsonObject> Info = MakeShared<FJsonObject>();
    Info->SetStringField(TEXT("cvar"), TEXT("r.ViewDistanceScale"));
    Info->SetNumberField(TEXT("scaleBefore"), Capture.ViewDistanceScaleBefore);
    Info->SetNumberField(TEXT("scaleApplied"), Capture.ViewDistanceScaleApplied);
    Info->SetBoolField(TEXT("overridden"), Capture.bViewDistanceScaleOverridden);
    Info->SetBoolField(TEXT("restored"), Capture.bViewDistanceScaleRestored);
    Info->SetStringField(TEXT("source"), Capture.ViewDistanceScaleSource);
    Info->SetNumberField(TEXT("scaleEffective"), Capture.ViewDistanceScaleEffective);
    Info->SetBoolField(TEXT("coverageSufficient"), Capture.bViewDistanceCoverageSufficient);
    // MACHINE-READABLE half of the clamp disclosure, unconditional so its absence is never the
    // signal. `viewDistanceWarning` below is the prose that explains it; a caller that only reads
    // strings would have to pattern-match one to learn that the derived scale was clipped, which
    // is the same "looks like a clean override" failure this block exists to close. Same
    // bool-plus-explanation pairing as `toneRangeApplicable` / `toneRangeNotApplicable` and
    // `pinned` / `pinWarning` elsewhere in this response.
    Info->SetBoolField(TEXT("clamped"), Capture.bViewDistanceScaleClamped);
    if (Capture.bViewDistanceScaleClamped)
    {
        Info->SetStringField(TEXT("clampReason"), Capture.ViewDistanceClampReason);
    }

    if (Capture.ViewDistanceSurvey.bValid)
    {
        TSharedPtr<FJsonObject> Survey = MakeShared<FJsonObject>();
        Survey->SetNumberField(TEXT("minCullDistance"), Capture.ViewDistanceSurvey.MinCullDistance);
        Survey->SetNumberField(TEXT("maxPrimitiveDistance"), Capture.ViewDistanceSurvey.MaxPrimitiveDistance);
        Survey->SetNumberField(TEXT("minPrimitiveDistance"), Capture.ViewDistanceSurvey.MinPrimitiveDistance);
        Survey->SetNumberField(TEXT("requiredScale"), Capture.ViewDistanceSurvey.RequiredScale);
        Survey->SetNumberField(TEXT("maxMinDrawDistance"), Capture.ViewDistanceSurvey.MaxMinDrawDistance);
        Survey->SetNumberField(TEXT("primitives"), Capture.ViewDistanceSurvey.NumPrimitives);
        Survey->SetNumberField(TEXT("distanceCulledPrimitives"), Capture.ViewDistanceSurvey.NumCulledPrimitives);
        Survey->SetNumberField(TEXT("instancedComponents"), Capture.ViewDistanceSurvey.NumInstancedComponents);
        Survey->SetNumberField(TEXT("foliageMaxEndCullDistance"),
            Capture.ViewDistanceSurvey.FoliageMaxEndCullDistance);
        Survey->SetNumberField(TEXT("foliageCeilingLimited"),
            Capture.ViewDistanceSurvey.NumFoliageCeilingLimited);
        // The distances above are measured from HERE, not from the camera. Reported so a reader
        // can see the orthographic pushback instead of concluding the numbers are wrong.
        Survey->SetObjectField(TEXT("cullingOrigin"), MakeVectorObject(Capture.ViewDistanceSurvey.CullingOrigin));
        Survey->SetBoolField(TEXT("cullingOriginMeasured"), Capture.ViewDistanceSurvey.bCullingOriginMeasured);
        Survey->SetNumberField(TEXT("cullingOriginPushback"), Capture.ViewDistanceSurvey.CullingOriginPushback);
        Info->SetObjectField(TEXT("survey"), Survey);
    }

    if (!Capture.ViewDistanceSurvey.bCullingOriginMeasured && Capture.ViewDistanceSurvey.bValid &&
        Capture.ViewDistanceScaleSource == TEXT("auto"))
    {
        Info->SetStringField(TEXT("cullingOriginWarning"),
            TEXT("The culling origin could not be measured off a built FSceneView, so the survey ")
            TEXT("fell back to the camera location. An orthographic view's culling origin sits up ")
            TEXT("to 2097152 cm behind the camera, so the derived scale is a lower bound here and ")
            TEXT("distant geometry may still be culled."));
    }

    if (Capture.ViewDistanceSurvey.NumFoliageCeilingLimited > 0)
    {
        // The one shortfall r.ViewDistanceScale cannot close, so it is reported whatever the scale
        // did. The engine clamps the instanced end-cull distance AFTER multiplying it by the scale,
        // which means these components are culled at the ceiling at every scale -- including the
        // one this capture just derived and applied "successfully".
        Info->SetStringField(TEXT("foliageCeilingWarning"), FString::Printf(
            TEXT("foliage.MaxEndCullDistance is set to %.0f cm, and %d instanced component(s) in ")
            TEXT("this level reach further than that from the culling origin (farthest %.0f cm). ")
            TEXT("The engine clamps the instanced cull distance AFTER multiplying it by ")
            TEXT("r.ViewDistanceScale (HierarchicalInstancedStaticMesh.cpp:1675-1686), so NO value ")
            TEXT("of viewDistanceScale draws those instances -- they are missing from these pixels ")
            TEXT("regardless of what this capture applied. Raise or clear foliage.MaxEndCullDistance, ")
            TEXT("or narrow orthoWidth so the frame stays inside the ceiling."),
            Capture.ViewDistanceSurvey.FoliageMaxEndCullDistance,
            Capture.ViewDistanceSurvey.NumFoliageCeilingLimited,
            Capture.ViewDistanceSurvey.MaxPrimitiveDistance));
    }

    if (Capture.bViewDistanceScaleClamped)
    {
        // Says what is still wrong with the frame rather than reporting a clean override. The
        // alternative -- a silently clipped scale -- produces a picture that is better than before
        // and still missing content, which is the hardest kind of result to notice. The cap that
        // bound is named because the three have different remedies.
        const FString CapExplanation =
            Capture.ViewDistanceClampReason == TEXT("nearCull")
                ? FString::Printf(
                      TEXT("a primitive in this level sets MinDrawDistance %.0f cm and the scale ")
                      TEXT("multiplies near culling too, so pushing it further would delete near ")
                      TEXT("geometry instead"),
                      Capture.ViewDistanceSurvey.MaxMinDrawDistance)
            : Capture.ViewDistanceClampReason == TEXT("int32")
                ? FString::Printf(
                      TEXT("the engine truncates the scaled foliage cull distance into an int32 ")
                      TEXT("(HierarchicalInstancedStaticMesh.cpp:1675), which bounds the product ")
                      TEXT("at %.0f cm"),
                      MaxScaledCullDistance)
                : FString::Printf(
                      TEXT("the derived scale reached the absolute ceiling of %.0f"),
                      MaxAutoViewDistanceScale);

        Info->SetStringField(TEXT("viewDistanceWarning"), FString::Printf(
            TEXT("This frame needed a view-distance scale of %.1f but only %.1f was applied: %s. ")
            TEXT("Primitives whose own cull distance times %.1f falls short of their %.0f cm reach ")
            TEXT("from the culling origin are STILL distance-culled in these pixels. Narrow ")
            TEXT("orthoWidth and capture in tiles, or pass viewDistanceScale explicitly."),
            Capture.ViewDistanceSurvey.RequiredScale * AutoViewDistanceScaleMargin,
            Capture.ViewDistanceScaleApplied,
            *CapExplanation,
            Capture.ViewDistanceScaleApplied,
            Capture.ViewDistanceSurvey.MaxPrimitiveDistance));
    }

    if (!Capture.bViewDistanceCoverageSufficient && Capture.ViewDistanceScaleSource != TEXT("none"))
    {
        // Read-back disagreement, as opposed to a cap the derivation knew about. The usual cause
        // is r.ViewDistanceScale.SecondaryScale silently multiplying the write down.
        Info->SetStringField(TEXT("coverageWarning"), FString::Printf(
            TEXT("The renderer's scalability cache reports an effective view-distance scale of ")
            TEXT("%.4f while this scene needs %.4f, so distant geometry is still culled in these ")
            TEXT("pixels. If scaleApplied (%.4f) differs from scaleEffective, check ")
            TEXT("r.ViewDistanceScale.ApplySecondaryScale / r.ViewDistanceScale.SecondaryScale."),
            Capture.ViewDistanceScaleEffective,
            Capture.ViewDistanceSurvey.RequiredScale,
            Capture.ViewDistanceScaleApplied));
    }

    if (!Capture.bViewDistanceScaleRestored)
    {
        Info->SetStringField(TEXT("restoreWarning"), FString::Printf(
            TEXT("r.ViewDistanceScale did not read back as %.4f after the capture. This session's ")
            TEXT("later captures are no longer comparable with earlier ones until it is reset."),
            Capture.ViewDistanceScaleBefore));
    }
    return Info;
}

// Delegates to the ONE vocabulary (Handlers/Render/ViewModeVocabulary.h), which derives the key
// from StaticEnum<EViewModeIndex>() plus five published overrides. This used to be a hand-written
// switch over ~40 enumerators kept in step with editor.set_view_mode's parse chain by a test; the
// two are now the same code and cannot drift at all. An enumerator the engine adds gets a key here
// with no edit.
FString GetViewModeKey(EViewModeIndex ViewMode)
{
    return PinWrightViewModes::GetKey(ViewMode);
}

bool IsLitViewMode(EViewModeIndex ViewMode)
{
    // VMI_PathTracing is included because it is a fully shaded, fully lit result -- a stricter
    // reference render than VMI_Lit, not a debug view. Nothing else qualifies; see the header for
    // why the list is deliberately this short.
    return ViewMode == VMI_Lit || ViewMode == VMI_PathTracing;
}

bool IsDebugMaterialSubstitutionMode(EViewModeIndex ViewMode)
{
    // The four modes ApplyViewModeOverrides draws by swapping the mesh batch's material for one of
    // GEngine's debug materials (PrimitiveDrawingUtils.cpp:1750-1805). Listed rather than computed
    // because the property that matters is WHERE the engine implements the mode, which no show
    // flag exposes: each of these four is a material swap and nothing else, so each one inherits
    // the reachability of that single call site. See the header for the Nanite consequence.
    //
    // All four enumerators arrived in UE 5.7; on older engines there is no mode the engine draws
    // this way, so no mode qualifies.
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 7, 0)
    return ViewMode == VMI_FrontBackFace
        || ViewMode == VMI_Clay
        || ViewMode == VMI_Zebra
        || ViewMode == VMI_RandomColor;
#else
    (void)ViewMode;
    return false;
#endif
}

// The `exposure` sub-block of every capture's `viewport` block. Unconditional, for the same
// reason `lit` is: a frame the camera re-exposed is pixel-for-pixel indistinguishable from a
// frame whose content changed, so the response has to say which one the caller is holding.
TSharedPtr<FJsonObject> MakeExposureInfoObject(const FViewportCaptureOutput& Capture)
{
    TSharedPtr<FJsonObject> Exposure = MakeShared<FJsonObject>();
    Exposure->SetStringField(TEXT("mode"), ExposureModeKey(Capture.ExposureMode));
    Exposure->SetBoolField(TEXT("pinRequested"), Capture.bExposurePinRequested);
    // `pinned` is a predicate over VIEWPORT STATE and its meaning is deliberately unchanged: it
    // says the override was applied and the renderer would honour it. It has never said anything
    // about the pixels, and a pin driven past the end of the scene's range satisfies it exactly
    // while returning a frame crushed to black (B-exposure-pin-black-frame). The frame question
    // gets its own field below rather than being smuggled into this one -- callers branch on
    // `pinned` today, and quietly narrowing what it means would break them silently.
    Exposure->SetBoolField(TEXT("pinned"), Capture.bExposurePinned);
    if (Capture.bExposurePinRequested && Capture.bExposurePinned && Capture.ImageStats.bStatsMeasured)
    {
        // MEASURED ON THE RETURNED PIXELS, not on the configuration that produced them: the pin
        // took effect AND the frame it produced resolves enough luminance levels to be read. Only
        // emitted when there is a pinned frame to judge -- with no pin requested there is nothing
        // to say, and with a pin requested but not applied `pinWarning` already reports that these
        // pixels are auto-exposed and uncomparable.
        Exposure->SetBoolField(TEXT("pinnedFrameUsable"),
            !(Capture.ImageStats.bCrushed || Capture.ImageStats.bBlownOut));
    }
    // Whether the pixels were drawn with a fixed exposure at all -- true for a viewport a human
    // left pinned through the editor's EV100 control even when this call asked for nothing.
    Exposure->SetBoolField(TEXT("fixed"), Capture.bExposureFixedApplied);
    if (Capture.bExposureFixedApplied)
    {
        Exposure->SetNumberField(TEXT("ev100"), Capture.Ev100Applied);
    }
    if (Capture.bExposurePinRequested)
    {
        Exposure->SetNumberField(TEXT("ev100Requested"), Capture.Ev100Requested);
    }
    Exposure->SetBoolField(TEXT("restored"), Capture.bExposureRestored);
    // Omitted rather than zeroed when there is no completed readback: a 0 here would read as "the
    // renderer resolved an exposure of zero", which is a measurement nobody took.
    Exposure->SetBoolField(TEXT("adaptedMeasured"), Capture.bAdaptedExposureMeasured);
    if (Capture.bAdaptedExposureMeasured)
    {
        // `adapted` is a linear GAIN and `ev100Equivalent` is the same exposure as a log stop.
        // Both are published because they answer different questions and neither substitutes for
        // the other: the gain is what the renderer multiplied scene colour by (and what changes
        // between two unpinned shots), the EV100 is what `ev100` accepts. They run in OPPOSITE
        // directions -- a bigger gain is a BRIGHTER image, a bigger EV100 a darker one -- which is
        // why shipping only the gain sent a caller 5.6 stops the wrong way. `ev100Equivalent` is
        // emitted next to it so the reusable number is never the one a reader has to derive.
        Exposure->SetNumberField(TEXT("adapted"), Capture.AdaptedExposure);
        Exposure->SetNumberField(TEXT("ev100Equivalent"),
            ExposureGainToEv100(Capture.AdaptedExposure));
        // Exact (derived from the EV100 the frame was pinned at) vs lagging (a GPU->CPU readback
        // from a few frames back). A caller comparing two unpinned shots needs to know it is
        // holding the lagging kind.
        Exposure->SetStringField(TEXT("adaptedSource"),
            Capture.bAdaptedExposureFromPin ? TEXT("fixedPin") : TEXT("readback"));
    }
    if (Capture.bAdaptedExposureReadbackPending)
    {
        // Emitted only when true, and worded as the narrow fact it is. It used to be a
        // `warmupWarning` telling the caller to discard the frame, on the strength of correlating
        // with an unfinished one; it fires on a fully warmed frame too (2 of 2 trials, 2026-08-19),
        // so it is not evidence about the pixels and no longer speaks about them. `warmup.settled`
        // is what answers that question, measured on the frame itself.
        Exposure->SetStringField(TEXT("adaptedReadbackPending"),
            TEXT("The renderer has not completed an eye-adaptation readback for this viewport, so "
                 "no measured `adapted` gain is available from it. Common on a viewport whose view "
                 "state is young -- a just-opened asset editor -- and also on any pinned capture, "
                 "where clearing the EyeAdaptation show flag stops the pass that refreshes it. It "
                 "says nothing about whether these pixels are finished: read `warmup.settled` for "
                 "that."));
    }

    if (Capture.bExposurePinRequested && !Capture.bExposurePinned)
    {
        // THE reason this parameter exists. The pin was written, the viewport carries it, and the
        // renderer ignores it for a frame in this state -- so these pixels used auto-exposure and
        // are not comparable with anything. Saying so is the whole contract; returning the frame
        // quietly would put the caller back exactly where it started.
        Exposure->SetStringField(TEXT("pinWarning"), FString::Printf(
            TEXT("An exposure pin at EV100 %.4f was requested and did NOT govern these pixels: %s. "
                 "The frame was rendered with auto-exposure, so it cannot be compared against "
                 "another capture. Capture in view mode 'Lit' with the PostProcessing and Lighting "
                 "show flags on (editor.set_view_mode viewMode:\"Lit\") and pin again."),
            Capture.Ev100Requested,
            Capture.ExposurePinBlockedReason.IsEmpty()
                ? TEXT("the renderer does not apply the fixed-exposure override to a frame in this state")
                : *Capture.ExposurePinBlockedReason));
    }

    if (Capture.bExposurePinRequested && Capture.bExposurePinned &&
        Capture.ImageStats.bStatsMeasured &&
        (Capture.ImageStats.bCrushed || Capture.ImageStats.bBlownOut))
    {
        // THE case this ticket is about. Every other field in this block is true and the aggregate
        // is misleading: the pin was asked for, the pin was applied, the renderer honoured it, and
        // the frame is unreadable. Nothing above fires -- `pinWarning` covers only the inverse
        // (pin requested, renderer ignored it) -- so without this line the caller is handed
        // `pinned: true` and no signal at all.
        Exposure->SetStringField(TEXT("pinRangeWarning"), FString::Printf(
            TEXT("The exposure pin at EV100 %.4f WAS applied and DID govern these pixels, and the ")
            TEXT("frame it produced is unusable: %s Read `pinned` for what it says -- the override ")
            TEXT("was applied to the viewport -- and `pinnedFrameUsable` for whether the resulting ")
            TEXT("image can be read; they are different questions and this capture answers them ")
            TEXT("differently. Re-shoot at a different `ev100`; a comparison set built on this ")
            TEXT("frame compares black against black."),
            Capture.Ev100Requested,
            *MakeToneRangeWarning(Capture.ImageStats)));
    }

    if (!Capture.bExposureRestored)
    {
        Exposure->SetStringField(TEXT("restoreWarning"), FString::Printf(
            TEXT("The viewport's exposure did not read back as it was before this capture "
                 "(fixed=%s, ev100=%.4f). Every later capture in this session is exposed "
                 "differently from the earlier ones until it is reset."),
            Capture.bExposureFixedBefore ? TEXT("true") : TEXT("false"),
            Capture.Ev100Before));
    }
    return Exposure;
}

// The `editorSprites` sub-block of every capture's `viewport` block. Unconditional, for the same
// reason `lit` is: a light-bulb icon drawn over a wall is pixel-for-pixel indistinguishable from
// something standing in front of that wall, so the response has to say which viewport state these
// pixels came from rather than leaving the reader to guess.
TSharedPtr<FJsonObject> MakeEditorSpriteInfoObject(const FViewportCaptureOutput& Capture)
{
    TSharedPtr<FJsonObject> Sprites = MakeShared<FJsonObject>();
    Sprites->SetBoolField(TEXT("hideRequested"), Capture.bHideEditorSpritesRequested);
    // `visible` is the question a caller actually has, answered off the flag the pixels were drawn
    // with rather than off the request -- true here means icons could be in this frame whether or
    // not anybody asked for them.
    Sprites->SetBoolField(TEXT("visible"), Capture.bBillboardSpritesApplied);
    Sprites->SetBoolField(TEXT("billboardSpritesBefore"), Capture.bBillboardSpritesBefore);
    Sprites->SetBoolField(TEXT("billboardSprites"), Capture.bBillboardSpritesApplied);
    Sprites->SetBoolField(TEXT("restored"), Capture.bEditorSpritesRestored);

    if (Capture.bHideEditorSpritesRequested && Capture.bBillboardSpritesApplied)
    {
        // The write did not stick. Nothing in the current code path can produce this, which is
        // exactly why it is reported: a silent no-op is the failure class this whole reporting
        // block exists to close, and a future viewport type that re-derives its show flags per
        // frame would land here rather than returning an icon-covered frame as a clean success.
        Sprites->SetStringField(TEXT("hideWarning"),
            TEXT("hideEditorSprites was requested and EngineShowFlags.BillboardSprites is still "
                 "set on this viewport, so editor icon sprites (light bulbs, audio icons, the "
                 "directional-light arrow) may be in these pixels."));
    }

    if (!Capture.bEditorSpritesRestored)
    {
        Sprites->SetStringField(TEXT("restoreWarning"), FString::Printf(
            TEXT("EngineShowFlags.BillboardSprites did not read back as it was before this capture "
                 "(before=%s). This capture left the viewport's show flags moved, so the editor "
                 "window itself, and every later capture from it, differ until it is reset."),
            Capture.bBillboardSpritesBefore ? TEXT("true") : TEXT("false")));
    }
    return Sprites;
}

// The `showFlagOverrides` sub-block of every capture's `viewport` block. Unconditional, for the
// same reason `editorSprites` is: a forced ShowFlag.* console variable either paints a full-screen
// debug pass over these pixels or removes a whole rendering feature from them, and the result is a
// plausible picture of a scene that is not the one under review. `measured` is published beside
// the list so "surveyed, nothing forced" and "never surveyed" are different answers.
TSharedPtr<FJsonObject> MakeShowFlagOverrideInfoObject(const FViewportCaptureOutput& Capture)
{
    TSharedPtr<FJsonObject> Block = MakeShared<FJsonObject>();
    Block->SetBoolField(TEXT("measured"), Capture.bShowFlagOverridesMeasured);

    TArray<TSharedPtr<FJsonValue>> Forced;
    FString Named;
    for (const FForcedShowFlagOverride& Override : Capture.ForcedShowFlags)
    {
        TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
        Row->SetStringField(TEXT("name"), Override.Name);
        Row->SetStringField(TEXT("cvar"), Override.CVar);
        Row->SetNumberField(TEXT("value"), Override.Value);
        // The direction as a word beside the raw number, because 0 and 1 read as false and true
        // to everything that is not this cvar family, where they mean force-off and force-on and
        // the value that means "leave it alone" is 2.
        Row->SetStringField(TEXT("direction"), Override.Value == 0 ? TEXT("off") : TEXT("on"));
        Row->SetStringField(TEXT("setBy"), Override.SetBy);
        Forced.Add(MakeShared<FJsonValueObject>(Row));

        if (!Named.IsEmpty())
        {
            Named += TEXT(", ");
        }
        Named += FString::Printf(TEXT("%s %d (set by %s)"),
            *Override.CVar, Override.Value, *Override.SetBy);
    }
    Block->SetArrayField(TEXT("forced"), Forced);

    if (Capture.ForcedShowFlags.Num() > 0)
    {
        Block->SetStringField(TEXT("overrideWarning"), FString::Printf(
            TEXT("%d ShowFlag.* console variable(s) were forcing a show flag while these pixels ")
            TEXT("were drawn: %s. That channel is neither the viewport's own show flags nor the ")
            TEXT("editor billboards: the override is ORed over the view's flags after this ")
            TEXT("viewport's are copied, so it is PROCESS-GLOBAL, game view does not clear it, ")
            TEXT("and `gameView`, `editorSprites` and the tone statistics all read clean on the ")
            TEXT("resulting frame. A Visualize* flag forced ON draws a full-screen debug pass ")
            TEXT("over the scene; any flag forced OFF removes that feature from it. Restore the ")
            TEXT("engine default with system.console_command \"<cvar> 2\" and re-shoot before ")
            TEXT("treating this frame as a picture of the level."),
            Capture.ForcedShowFlags.Num(), *Named));
    }
    return Block;
}

// The `viewModeOverride` sub-block of every capture's `viewport` block. Unconditional, so an
// absent override and an override that did nothing are distinguishable -- which they were not for
// camera.orbit_shots' `viewMode`, an argument that changed nothing and echoed itself back for
// long enough to be documented as informational.
TSharedPtr<FJsonObject> MakeViewModeOverrideInfoObject(const FViewportCaptureOutput& Capture)
{
    TSharedPtr<FJsonObject> Override = MakeShared<FJsonObject>();
    Override->SetBoolField(TEXT("requested"), Capture.bViewModeOverrideRequested);
    Override->SetBoolField(TEXT("applied"), Capture.bViewModeApplied);
    Override->SetBoolField(TEXT("restored"), Capture.bViewModeRestored);

    // Both slots, before and after, always. A viewport client keeps separate perspective and
    // orthographic view modes, so one number cannot describe the restore.
    TSharedPtr<FJsonObject> Before = MakeShared<FJsonObject>();
    Before->SetStringField(TEXT("perspective"), Capture.ViewModePerspBeforeKey);
    Before->SetStringField(TEXT("orthographic"), Capture.ViewModeOrthoBeforeKey);
    Override->SetObjectField(TEXT("previous"), Before);

    TSharedPtr<FJsonObject> After = MakeShared<FJsonObject>();
    After->SetStringField(TEXT("perspective"), Capture.ViewModePerspAfterKey);
    After->SetStringField(TEXT("orthographic"), Capture.ViewModeOrthoAfterKey);
    Override->SetObjectField(TEXT("afterRestore"), After);

    if (Capture.bViewModeOverrideRequested)
    {
        Override->SetStringField(TEXT("requestedViewMode"), Capture.ViewModeRequestedKey);
        Override->SetNumberField(TEXT("requestedViewModeValue"), Capture.ViewModeRequestedValue);

        // The show flags this mode writes differently from Lit, and which of them failed to read
        // back. This is the check that cannot be satisfied by assigning a field: the flags are
        // what FEditorViewportClient::Draw hands the renderer.
        TArray<TSharedPtr<FJsonValue>> Flags;
        for (const FString& Name : Capture.ViewModeShowFlags)
        {
            Flags.Add(MakeShared<FJsonValueString>(Name));
        }
        Override->SetArrayField(TEXT("showFlagsChecked"), Flags);

        if (Capture.ViewModeShowFlagMismatches.Num() > 0)
        {
            TArray<TSharedPtr<FJsonValue>> Mismatches;
            for (const FString& Name : Capture.ViewModeShowFlagMismatches)
            {
                Mismatches.Add(MakeShared<FJsonValueString>(Name));
            }
            Override->SetArrayField(TEXT("showFlagMismatches"), Mismatches);
        }

        if (!Capture.ViewModeCompanion.IsEmpty())
        {
            // The same mode with a different sub-visualisation is a different picture, so the one
            // in force is named rather than left implicit.
            Override->SetStringField(TEXT("companionMode"), Capture.ViewModeCompanion);
        }

        if (!Capture.bViewModeApplied)
        {
            Override->SetStringField(TEXT("applyWarning"), FString::Printf(
                TEXT("viewMode '%s' was requested and did not fully take on this viewport: the ")
                TEXT("slots read back perspective='%s' / orthographic='%s' and %d of the %d show ")
                TEXT("flags that distinguish this mode from Lit did not match. These pixels are ")
                TEXT("NOT in the mode that was asked for -- compare viewModeValue against ")
                TEXT("requestedViewModeValue and re-shoot rather than reading this frame as a ")
                TEXT("diagnostic view."),
                *Capture.ViewModeRequestedKey, *Capture.ViewModePerspAfterKey,
                *Capture.ViewModeOrthoAfterKey, Capture.ViewModeShowFlagMismatches.Num(),
                Capture.ViewModeShowFlags.Num()));
        }
    }

    if (!Capture.bViewModeRestored)
    {
        Override->SetStringField(TEXT("restoreWarning"), FString::Printf(
            TEXT("The viewport's view-mode slots did not read back as they were before this ")
            TEXT("capture (perspective %s -> %s, orthographic %s -> %s). This capture left the ")
            TEXT("editor window itself in a different mode, so what the user sees and every later ")
            TEXT("capture from this viewport are affected until it is reset."),
            *Capture.ViewModePerspBeforeKey, *Capture.ViewModePerspAfterKey,
            *Capture.ViewModeOrthoBeforeKey, *Capture.ViewModeOrthoAfterKey));
    }
    return Override;
}

// One rig, as JSON. Shared by the top-level block and by `previous` / `afterRestore`, so the three
// cannot drift into describing different things -- which is exactly what a hand-rolled second copy
// does the first time a field is added.
TSharedPtr<FJsonObject> MakeOnePreviewSceneRigObject(
    const PinWrightPreviewSceneRig::FPreviewSceneRigReport& Rig)
{
    TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();

    TSharedPtr<FJsonObject> Key = MakeShared<FJsonObject>();
    TSharedPtr<FJsonObject> Rotation = MakeShared<FJsonObject>();
    Rotation->SetNumberField(TEXT("pitch"), Rig.KeyRotation.Pitch);
    Rotation->SetNumberField(TEXT("yaw"), Rig.KeyRotation.Yaw);
    Rotation->SetNumberField(TEXT("roll"), Rig.KeyRotation.Roll);
    // BOTH the raw FRotator and the derived arrival pair, so nobody redoes the trig -- and so a
    // caller who has one can check the other. The rotator is the engine's own value; azimuth and
    // elevation are where the light ARRIVES from, which is the negation of the +X axis the
    // rotator describes.
    Key->SetObjectField(TEXT("rotation"), Rotation);
    Key->SetNumberField(TEXT("azimuth"), Rig.KeyAzimuthDegrees);
    Key->SetNumberField(TEXT("elevation"), Rig.KeyElevationDegrees);
    Key->SetNumberField(TEXT("intensity"), Rig.KeyIntensity);
    Key->SetStringField(TEXT("color"), FString::Printf(TEXT("#%02X%02X%02X"),
        Rig.KeyColor.R, Rig.KeyColor.G, Rig.KeyColor.B));
    // The same colour the renderer works in. ULightComponentBase stores an sRGB FColor and the
    // shading path uses its linear form, so publishing only one of the two invites a caller to
    // compare an sRGB triple against a linear one and conclude the light changed when it did not.
    TSharedPtr<FJsonObject> Linear = MakeShared<FJsonObject>();
    const FLinearColor KeyLinear = FLinearColor(Rig.KeyColor);
    Linear->SetNumberField(TEXT("r"), KeyLinear.R);
    Linear->SetNumberField(TEXT("g"), KeyLinear.G);
    Linear->SetNumberField(TEXT("b"), KeyLinear.B);
    Key->SetObjectField(TEXT("colorLinear"), Linear);
    Out->SetObjectField(TEXT("key"), Key);

    TSharedPtr<FJsonObject> Sky = MakeShared<FJsonObject>();
    Sky->SetNumberField(TEXT("intensity"), Rig.SkyIntensity);
    Sky->SetBoolField(TEXT("visible"), Rig.bSkyVisible);
    if (!Rig.SkyCubemapPath.IsEmpty())
    {
        // The single biggest reason two machines' captures of the same asset differ, and the one
        // thing the plugin's own docs never mentioned: the ambient light comes from this cubemap.
        Sky->SetStringField(TEXT("cubemap"), Rig.SkyCubemapPath);
    }
    Out->SetObjectField(TEXT("sky"), Sky);

    if (Rig.bAdvancedScene || Rig.bBackdropVisibilityMeasured)
    {
        // A bare scene normally has no floor/environment measurement. The transient mesh rig is
        // the exception: it owns equivalent per-call components without touching the shared
        // FAdvancedPreviewScene profile, and marks that measurement explicitly.
        Out->SetBoolField(TEXT("showFloor"), Rig.bShowFloor);
        Out->SetBoolField(TEXT("showEnvironment"), Rig.bShowEnvironment);
        Out->SetBoolField(TEXT("rotateLightingRig"), Rig.bRotateLightingRig);
    }

    TSharedPtr<FJsonObject> ShowFlags = MakeShared<FJsonObject>();
    ShowFlags->SetBoolField(TEXT("postProcessing"), Rig.bPostProcessingShowFlag);
    ShowFlags->SetBoolField(TEXT("tonemapper"), Rig.bTonemapperShowFlag);
    ShowFlags->SetBoolField(TEXT("eyeAdaptation"), Rig.bEyeAdaptationShowFlag);
    Out->SetObjectField(TEXT("showFlags"), ShowFlags);
    return Out;
}

TSharedPtr<FJsonObject> MakePreviewSceneRigInfoObject(const FViewportCaptureOutput& Capture)
{
    TSharedPtr<FJsonObject> Block = MakeShared<FJsonObject>();
    const PinWrightPreviewSceneRig::FPreviewSceneRigReport& Drawn = Capture.PreviewSceneRigDrawn;

    Block->SetBoolField(TEXT("sceneAvailable"), Drawn.bSceneAvailable);
    if (!Drawn.bSceneAvailable)
    {
        // The only field present on a level-viewport verb. Emitting an empty `profileName` here
        // instead would be one more absence nobody knew to assert on: a reader cannot tell an
        // empty string from a profile whose name is empty.
        //
        // The one exception is a rig that was REQUESTED against a scene-less viewport, which
        // cannot happen through the capture path (CanApplyRig refuses it with
        // UNSUPPORTED_ASSET_EDITOR before the guard is built) but is reported rather than dropped
        // if some future caller reaches it, because a silently discarded rig is the exact defect
        // this parameter exists to remove.
        if (Capture.bPreviewSceneRigRequested)
        {
            Block->SetBoolField(TEXT("requested"), true);
            Block->SetBoolField(TEXT("applied"), false);
            Block->SetStringField(TEXT("rigWarning"),
                TEXT("A previewScene rig was requested for a viewport with no preview scene, so "
                     "nothing was applied and these pixels are lit by the level's own lighting."));
        }
        return Block;
    }

    Block->SetBoolField(TEXT("advancedScene"), Drawn.bAdvancedScene);
    if (Drawn.bAdvancedScene)
    {
        // THE FIELD THAT MAKES TWO MACHINES' CAPTURES COMPARABLE -- or provably not. The profile
        // index is per-user (UEditorPerProjectUserSettings::AssetViewerProfileIndex), so two
        // developers running the same call can be on different rigs and neither response used to
        // say so.
        Block->SetStringField(TEXT("profileName"), Drawn.ProfileName);
        Block->SetNumberField(TEXT("profileIndex"), Drawn.ProfileIndex);
    }

    // The rig the PIXELS WERE DRAWN UNDER, flattened into the top level so the common question --
    // "what lit this frame" -- is answered without walking into a sub-object.
    const TSharedPtr<FJsonObject> DrawnObject = MakeOnePreviewSceneRigObject(Drawn);
    for (const TPair<FString, TSharedPtr<FJsonValue>> Field : DrawnObject->Values)
    {
        Block->SetField(Field.Key, Field.Value);
    }

    // The canonical triple. `requested` false with a fully populated rig above is the ordinary
    // case and means "this is the editor's own rig"; `requested` true with `applied` false is the
    // one condition that makes the frame non-comparable with what the caller asked for.
    Block->SetBoolField(TEXT("requested"), Capture.bPreviewSceneRigRequested);
    Block->SetBoolField(TEXT("applied"), Capture.bPreviewSceneRigApplied);
    // THE OTHER HALF OF `applied`, and the reason `applied` on its own was read as more than it
    // says. `applied` is a true statement about the WRITES; reaching the pixels ALSO needs the
    // sky/reflection capture queue drained, and that queue is emptied only by an editor tick no
    // capture runs. This field says the drain was DRIVEN -- necessary, and on its own not
    // sufficient, which is what `captureIncomplete` below exists to say. Published beside
    // `applied` so the two are read together rather than one standing in for the other.
    Block->SetBoolField(TEXT("captureUpdated"), Capture.bPreviewSceneCaptureUpdated);
    if (Capture.bPreviewSceneRigDisposable)
    {
        Block->SetBoolField(TEXT("disposed"), true);
    }
    else
    {
        Block->SetBoolField(TEXT("restored"), Capture.bPreviewSceneRigRestored);
    }

    // ---- THE BEFORE/AFTER MIRRORS AND THE RESTORE LEDGER: WHEN THERE WAS A CHANGE TO REPORT ---
    //
    // These three describe A CHANGE THIS CALL MADE AND PUT BACK. When no rig was requested,
    // nothing was changed: `previous` and `afterRestore` are then byte-for-byte the drawn rig
    // already flattened into this block above, and `restore` reports a restore of nothing. That is
    // 1,114 characters of a capture response restating a fact three times and announcing an
    // outcome that did not occur - rpc-design.md section 1, in its quietest form.
    //
    // MEASURED: on 92 real single-still capture responses the whole `previewScene` block ran
    // 1,676-1,755 characters, and this triple was two thirds of it.
    //
    // EMITTED WHENEVER THERE IS SOMETHING TO SAY, which is a wider condition than "requested":
    // a restore that FAILED must publish both sides and the ledger even if the rig was applied by
    // a verb-internal default rather than by the caller, because that is exactly when a caller
    // needs to see what was left behind. The `rigWarning` / `restoreWarning` strings below are
    // unaffected either way - they are gated on their own measured conditions, not on this one -
    // so the failure paths lose nothing.
    //
    // What is NOT conditional: `requested`, `applied`, `restored` above, and the whole measured
    // drawn rig. "What lit these pixels" is the question this block exists to answer, and it is
    // answered on every capture from every verb, unchanged.
    const bool bRestoreLedgerIsInteresting = !Capture.bPreviewSceneRigDisposable && (
        Capture.bPreviewSceneRigRequested ||
        !Capture.bPreviewSceneRigRestored ||
        !Capture.bSharedProfilesRestored ||
        !Capture.bConfigFileUnchanged);
    if (bRestoreLedgerIsInteresting)
    {
        // Both sides, so the restore is checkable by the CALLER and not only by us.
        Block->SetObjectField(TEXT("previous"),
            MakeOnePreviewSceneRigObject(Capture.PreviewSceneRigBefore));
        Block->SetObjectField(TEXT("afterRestore"),
            MakeOnePreviewSceneRigObject(Capture.PreviewSceneRigAfter));

        TSharedPtr<FJsonObject> Restore = MakeShared<FJsonObject>();
        Restore->SetBoolField(TEXT("profilesRestored"), Capture.bSharedProfilesRestored);
        Restore->SetBoolField(TEXT("configFileUnchanged"), Capture.bConfigFileUnchanged);
        // Published rather than kept private so a caller can compare it across two calls itself.
        // Empty means the file does not exist on this host, which is a legal state and not a
        // failed read.
        Restore->SetStringField(TEXT("configFileDigest"), Capture.ConfigFileDigest);
        Block->SetObjectField(TEXT("restore"), Restore);
    }

    if (Drawn.bRotateLightingRig)
    {
        // NO RESTORE CAN FIX THIS, which is why it is a warning rather than something the guard
        // corrects. FAdvancedPreviewScene::Tick re-aims the key light every frame and writes the
        // new rotation into the shared profile (AdvancedPreviewScene.cpp:285-294), so the rig
        // moved WHILE these pixels were being drawn and will have moved again by the next capture.
        Block->SetStringField(TEXT("rotateLightingRigWarning"),
            TEXT("This preview scene has 'Rotate Sky and Directional Lighting' on, so its key "
                 "light moves every frame and writes its new rotation into the shared profile. "
                 "This capture is not reproducible and is not comparable with another capture of "
                 "the same subject, whatever exposure or rig was pinned. Turn the profile's "
                 "rotation off in the editor's Preview Scene Settings before taking a comparison "
                 "set."));
    }

    if (Capture.bPreviewSceneRigRequested && !Capture.bPreviewSceneRigApplied)
    {
        // THE reason `applied` is measured instead of assigned. The rig was written and the scene
        // does not read back carrying it, so these pixels are lit by something else while the
        // request says otherwise.
        Block->SetStringField(TEXT("rigWarning"),
            TEXT("A previewScene rig was requested and the scene did NOT read back carrying it. "
                 "These pixels were lit by a different rig than the request describes, so they "
                 "cannot be compared against another capture taken with the same request. Read "
                 "the key/sky fields in this block -- they are the MEASURED rig -- rather than "
                 "the request."));
    }

    if (!Capture.bPreviewSceneCaptureUpdated)
    {
        // GATED ON THE DRAIN, NOT ON THE WARM-UP. `warmup.settled` cannot carry this: its pump is
        // the capture's own, which runs no editor tick, so it cannot advance the work it would
        // have to observe -- a frame lit by a stale sky capture is a finished frame of the wrong
        // thing and converges immediately and legitimately. A capture that reported
        // `settled: true` on round 1 with a mean-luminance delta 166x inside tolerance was
        // measured delivering exactly that.
        Block->SetStringField(TEXT("skyCaptureWarning"),
            TEXT("The sky-light and reflection captures for this preview scene were NOT updated "
                 "for this capture, so these pixels are lit by whatever the last editor frame "
                 "left in the capture queue rather than by the rig reported above. `applied` "
                 "describes the writes, not the pixels, and `warmup.settled` cannot see this -- "
                 "the settle loop's pump runs no editor tick, so it converges on a stable wrong "
                 "frame. This is reported, not guessed: the update is driven explicitly on every "
                 "preview-scene capture and did not run here, which happens when the preview "
                 "world has no renderer scene. Do not compare this frame against another "
                 "capture's lighting."));
    }
    else if (Capture.bPreviewSceneCaptureIncomplete)
    {
        // OMITTED, NOT FALSE, on the healthy path -- the block's standing rule, and here it also
        // keeps the field from being read as a measurement on the branch above, where the drain
        // never ran and there is nothing to have been left over from it.
        Block->SetBoolField(TEXT("captureIncomplete"), true);
        Block->SetStringField(TEXT("captureIncompleteWarning"),
            TEXT("The sky/reflection capture update was driven for this capture and a sky "
                 "capture was STILL queued when it returned, so the lighting in these pixels "
                 "may be the previous capture's. The engine defers a capture it cannot finish "
                 "while shaders, textures or meshes are async-compiling and will not retry it "
                 "for 5 seconds, which is why the first capture after any asset load can land "
                 "here; nothing this call can do shortens that wait. Re-shoot once compilation "
                 "has settled. NOTE THE ONE DIRECTION THIS CAN BE WRONG IN: the engine's queue "
                 "is process-wide, so a sky light in another editor window can raise this on a "
                 "capture whose own lighting is fine. It cannot be silent while THIS scene's "
                 "capture is pending."));
    }

    if (!Capture.bPreviewSceneRigDisposable &&
        (!Capture.bPreviewSceneRigRestored || !Capture.bSharedProfilesRestored ||
            !Capture.bConfigFileUnchanged))
    {
        // Three separate failures, one field, each named explicitly, because their blast radii are
        // different: a moved component affects later captures from this viewport, a moved shared
        // profile affects every open asset editor in the process, and a moved config file is a
        // source-controlled diff.
        Block->SetStringField(TEXT("restoreWarning"), FString::Printf(
            TEXT("This capture did not fully restore the preview-scene state it changed: light "
                 "components restored=%s, shared UAssetViewerSettings::Profiles array "
                 "restored=%s, committed Config/DefaultEditor.ini unchanged=%s. The profile array "
                 "is process-wide, so a false there re-lights every open asset editor; the config "
                 "file is under source control, so a false there is a working-tree diff. Note "
                 "that a Preview Scene Settings tab closing in ANOTHER window during this capture "
                 "can produce the last one on its own -- this reports it rather than claiming a "
                 "guarantee it cannot make."),
            Capture.bPreviewSceneRigRestored ? TEXT("true") : TEXT("false"),
            Capture.bSharedProfilesRestored ? TEXT("true") : TEXT("false"),
            Capture.bConfigFileUnchanged ? TEXT("true") : TEXT("false")));
    }
    return Block;
}

TSharedPtr<FJsonObject> MakeViewportInfoObject(const FViewportCaptureOutput& Capture)
{
    TSharedPtr<FJsonObject> Viewport = MakeShared<FJsonObject>();
    Viewport->SetStringField(TEXT("type"), Capture.ViewportType);
    Viewport->SetNumberField(TEXT("typeValue"), Capture.ViewportTypeValue);
    Viewport->SetStringField(TEXT("viewMode"), Capture.ViewMode);
    Viewport->SetStringField(TEXT("viewModeKey"), Capture.ViewModeKey);
    Viewport->SetNumberField(TEXT("viewModeValue"), Capture.ViewModeValue);
    Viewport->SetBoolField(TEXT("lit"), Capture.bLitViewMode);
    Viewport->SetBoolField(TEXT("gameView"), Capture.bGameView);
    Viewport->SetBoolField(TEXT("realtime"), Capture.bRealtime);
    Viewport->SetObjectField(TEXT("exposure"), MakeExposureInfoObject(Capture));
    Viewport->SetObjectField(TEXT("editorSprites"), MakeEditorSpriteInfoObject(Capture));
    Viewport->SetObjectField(TEXT("showFlagOverrides"), MakeShowFlagOverrideInfoObject(Capture));
    Viewport->SetObjectField(TEXT("viewModeOverride"), MakeViewModeOverrideInfoObject(Capture));
    Viewport->SetObjectField(TEXT("previewScene"), MakePreviewSceneRigInfoObject(Capture));
    {
        TSharedPtr<FJsonObject> Render = MakeShared<FJsonObject>();
        Render->SetNumberField(TEXT("width"), Capture.RenderWidth);
        Render->SetNumberField(TEXT("height"), Capture.RenderHeight);
        Render->SetNumberField(TEXT("primaryResolutionFraction"),
            Capture.RenderPrimaryResolutionFraction);
        Render->SetNumberField(TEXT("secondaryResolutionFraction"),
            Capture.RenderSecondaryResolutionFraction);
        Render->SetNumberField(TEXT("resolutionFraction"), Capture.RenderResolutionFraction);
        Render->SetNumberField(TEXT("screenPercentage"), Capture.RenderScreenPercentage);
        Render->SetStringField(TEXT("antiAliasingMethod"), Capture.RenderAntiAliasingMethod);
        Render->SetNumberField(TEXT("antiAliasingMethodValue"),
            Capture.RenderAntiAliasingMethodValue);
        Render->SetBoolField(TEXT("resolutionPinned"), Capture.bRenderResolutionPinned);
        Render->SetBoolField(TEXT("upscaled"), Capture.bRenderUpscaled);
        Render->SetBoolField(TEXT("temporalAntiAliasingSuppressed"),
            Capture.bTemporalAntiAliasingSuppressed);
        Viewport->SetObjectField(TEXT("render"), Render);
    }
    // Unconditional, and MEASURED off the landscape proxies after the build. `settled` with
    // `instances` is the pair that separates "this ground really is bare" from "the grass for
    // this pose had not finished building" -- two opposite readings of one picture that the
    // response could not tell apart before (B-capture-open-level-pose-params-photograph-stale-grass).
    Viewport->SetObjectField(TEXT("grass"),
        PinWrightCaptureGrass::MakeGrassBuildInfoObject(Capture.GrassBuild));

    // ---- game view, and the fact that nobody in this process owns it ----
    //
    // `gameView` has been published beside these pixels since the block existed, and a capture
    // that measured it FALSE still returned success with editor chrome in the frame and said
    // nothing -- while the neighbouring verb raised `overlayWarning` for a strictly milder
    // condition. The measurement was never the gap; the warning was.
    //
    // Gated on the viewport having no preview scene, i.e. on the level-editor family
    // (render.capture_open_level, camera.*, the level path of render.capture_annotated). An asset
    // preview viewport is normally NOT in game view, has none of the chrome this warns about, and
    // has no lever to change it -- editor.set_game_view drives the level viewport -- so warning
    // there would fire on correct output and hand the reader a remedy that does not apply.
    if (!Capture.bGameView && !Capture.PreviewSceneRigDrawn.bSceneAvailable)
    {
        Viewport->SetStringField(TEXT("gameViewWarning"),
            TEXT("Game view was OFF while these pixels were drawn, so this frame can carry editor "
                 "chrome that is not in the level: the world-axis gizmo, component visualizers "
                 "(spline handles, tangent arrows, light radii) and the grid, selection and "
                 "selection-outline passes. `editorSprites` does not cover any of them -- it "
                 "governs billboard icons only and reports clean here. Game view is per-VIEWPORT "
                 "state that this capture does not set and does not own, so in an editor shared "
                 "with other agents it can be turned off between an editor.set_game_view call and "
                 "this capture, however the earlier call was confirmed. Re-assert "
                 "editor.set_game_view {enabled: true} immediately before the shot and read this "
                 "field off THIS response rather than trusting that confirmation."));
    }

    // ---- overlay show flags, measured off the client that drew these pixels ----
    //
    // Unconditional and MEASURED, for the reason `editorSprites` is, and the reason is sharper
    // here: a spline's white dashed line down a river bank is pixel-for-pixel indistinguishable
    // from foam, and it has twice come close to being written up as authored map content off a
    // capture whose response said `gameView: true` and nothing else. `gameView` is not the answer
    // to "are overlays in this frame" -- SetGameView reuses the current flags as the game set when
    // EngineShowFlags.Game is already true (EditorViewportClient.cpp:7229-7240), so the flags it
    // is supposed to clear can survive it. `measured` separates "read off the client and nothing
    // was on" from "never read".
    {
        TSharedPtr<FJsonObject> Overlays = MakeShared<FJsonObject>();
        Overlays->SetBoolField(TEXT("measured"), Capture.bOverlayShowFlagsMeasured);
        PinWrightAddGameViewOverlayFlags(Overlays, Capture.OverlayShowFlags);

        const FString VisibleOverlays =
            PinWrightDescribeVisibleGameViewOverlays(Capture.OverlayShowFlags);
        if (Capture.bOverlayShowFlagsMeasured && Capture.bGameView && !VisibleOverlays.IsEmpty())
        {
            // The contradiction, spelled out where the reader of these pixels actually looks. Only
            // on the game-view branch: with game view off, `gameViewWarning` above already says
            // the frame carries editor chrome, and repeating it here would be noise.
            Overlays->SetStringField(TEXT("overlayWarning"), FString::Printf(
                TEXT("Game view was ON while these pixels were drawn and these overlay show flags "
                     "were still set: %s. Editor overlays are therefore IN this frame. The one "
                     "that reads as content: `splines` draws a line along every water body and "
                     "landscape spline -- a white dashed line down each bank of a river is that "
                     "overlay, not foam and not authored geometry. Toggle editor.set_game_view "
                     "off and on again to force a fresh game flag set, or clear the flag on the "
                     "viewport, and re-shoot before reading these pixels as shipped content."),
                *VisibleOverlays));
        }
        Viewport->SetObjectField(TEXT("overlayShowFlags"), Overlays);
    }

    // ---- aim ----
    // Unconditional, and measured. The defect this exists for returned a perfectly valid PNG of
    // the WRONG SIDE of the subject and reported the requested pose back as if it had been used.
    {
        TSharedPtr<FJsonObject> Aim = MakeShared<FJsonObject>();
        Aim->SetBoolField(TEXT("applied"), Capture.bCameraAimApplied);
        Aim->SetNumberField(TEXT("rotationErrorDegrees"), Capture.CameraAimErrorDegrees);
        Aim->SetNumberField(TEXT("locationErrorCm"), Capture.CameraAimLocationErrorCm);
        // Orbit mode is the mechanism that used to swallow a requested pose; reported both as
        // "the viewport was orbiting when this call arrived" and "this call had to turn it off".
        Aim->SetBoolField(TEXT("orbitCameraAtEntry"), Capture.bOrbitCameraAtEntry);
        Aim->SetBoolField(TEXT("orbitCameraSuppressed"), Capture.bOrbitCameraSuppressed);
        if (!Capture.bCameraAimApplied)
        {
            Aim->SetStringField(TEXT("aimWarning"), FString::Printf(
                TEXT("The camera did not go where this call asked: the rendered view direction is ")
                TEXT("%.2f deg off the requested rotation and the eye is %.2f cm from the requested ")
                TEXT("location. These pixels show a different part of the world than the request ")
                TEXT("describes, so they cannot be paired with another capture as an opposed or ")
                TEXT("matched view. `cameraRotation` and `cameraLocation` in this response are the ")
                TEXT("MEASURED pose -- use those, not the request."),
                Capture.CameraAimErrorDegrees, Capture.CameraAimLocationErrorCm));
        }
        Viewport->SetObjectField(TEXT("aim"), Aim);
    }

    // Emitted only by backends that ran the settle loop. `settled` is the conjunction of
    // "the frame mean moved less than the tolerance between two consecutive draws" -- there is no
    // literal here and no path that reports settled without having observed it.
    if (Capture.bWarmupMeasured)
    {
        TSharedPtr<FJsonObject> Warmup = MakeShared<FJsonObject>();
        Warmup->SetBoolField(TEXT("settled"), Capture.bWarmupSettled);
        Warmup->SetNumberField(TEXT("settleRounds"), Capture.WarmupSettleRounds);
        Warmup->SetNumberField(TEXT("meanLuminanceDelta"), Capture.WarmupMeanLuminanceDelta);
        Warmup->SetBoolField(TEXT("pixelChangeMeasured"), Capture.bWarmupPixelChangeMeasured);
        Warmup->SetNumberField(TEXT("meanAbsDelta"), Capture.WarmupMeanAbsDelta);
        Warmup->SetNumberField(TEXT("maxDelta"), Capture.WarmupMaxDelta);
        Warmup->SetNumberField(TEXT("changedPixelFraction"),
            Capture.WarmupChangedPixelFraction);
        Warmup->SetNumberField(TEXT("channelThreshold"), Capture.WarmupChannelThreshold);
        Warmup->SetNumberField(TEXT("settleMs"), Capture.WarmupSettleMs);
        if (!Capture.bWarmupSettled)
        {
            // The actionable case, and the one the old readback-based warning got wrong in both
            // directions. Says what was measured, not what is suspected.
            Warmup->SetStringField(TEXT("warmupWarning"), FString::Printf(
                TEXT("The frame was still changing when the settle budget ran out: its mean ")
                TEXT("luminance moved by %.6f over the last of %d extra draws (%.1f ms). These ")
                TEXT("pixels are mid-warm-up and are not comparable with another capture -- a ")
                TEXT("freshly opened preview viewport has been measured 2.26 stops dark in this ")
                TEXT("state, at a pinned exposure. Re-shoot into the same viewport and use the ")
                TEXT("later frame."),
                Capture.WarmupMeanLuminanceDelta, Capture.WarmupSettleRounds,
                Capture.WarmupSettleMs));
        }
        Viewport->SetObjectField(TEXT("warmup"), Warmup);
    }

    if (!Capture.bLitViewMode && Capture.bViewModeOverrideRequested && Capture.bViewModeApplied)
    {
        // The non-lit mode is what THIS call asked for, so the standing warning's remedy ("call
        // editor.set_view_mode with Lit") is wrong here and its premise ("a viewport left in a
        // debug mode stays that way") is the opposite of what happened. Still unconditional and
        // still says the frame cannot carry a material/lighting decision -- that part is true
        // however the mode was chosen -- but it names the scoped mechanism instead of the
        // persistent one.
        Viewport->SetStringField(TEXT("viewModeWarning"), FString::Printf(
            TEXT("Captured in view mode '%s' (%s) because this call asked for it via the viewMode ")
            TEXT("parameter, not because the viewport was left that way. This frame is a ")
            TEXT("diagnostic view and does not show materials and lighting as rendered, so it ")
            TEXT("cannot be used to verify material, lighting or colour work -- take a second ")
            TEXT("capture without viewMode for that. The viewport's previous mode was restored ")
            TEXT("(viewport.viewModeOverride.restored says so), so nothing leaked into later ")
            TEXT("captures."),
            *Capture.ViewModeKey, *Capture.ViewMode));
    }
    else if (!Capture.bLitViewMode)
    {
        // Names the mode in both spellings and states the consequence, because the failure this
        // guards against is an agent accepting a confident-looking frame that shows none of the
        // work it was sent to check. The remedy is spelled out with the exact key to pass back.
        Viewport->SetStringField(TEXT("viewModeWarning"), FString::Printf(
            TEXT("Captured in view mode '%s' (%s), not Lit. This frame does not show materials and ")
            TEXT("lighting as rendered, so it cannot be used to verify material, lighting or colour ")
            TEXT("work. Call editor.set_view_mode with viewMode:\"Lit\" and capture again; note that ")
            TEXT("set_view_mode does not restore itself, so a viewport left in a debug mode by ")
            TEXT("earlier work stays that way for every later capture."),
            *Capture.ViewModeKey, *Capture.ViewMode));
    }
    return Viewport;
}
}
