// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Handlers/Render/ViewProjectionUtils.h"

#include "Editor.h"
#include "EditorViewportClient.h"
#include "IAssetViewport.h"
#include "LevelEditor.h"
#include "Compat/EngineVersionCompat.h"
#include "Modules/ModuleManager.h"
#include "SceneView.h"
#include "Slate/SceneViewport.h"

// Header locations for the Level Editor viewport accessors have been stable across UE 5.3-5.7,
// so no __has_include shims are needed here; the include guard is kept for future divergence.

namespace PinWrightViewProjection
{
using PinWrightRenderCapture::FViewportCaptureRequest;

// Named, not anonymous, so ViewProjectionUtils.h can forward-declare FScopedRebuiltView as
// FViewProjectionSession's pimpl. It is still defined only in this .cpp, and
// PinWrightViewProjection::Detail is qualified enough that a Unity-build TU merge cannot ODR-clash
// it - the same guard PinWrightAnnotatedCapture and FDriveSetOfMarkRenderer use for their own
// file-local helpers.
//
// The using-declaration moved OUT deliberately: an UNNAMED namespace injects its names into the
// enclosing scope and a named one does not, so DeprojectScreenToWorld's unqualified
// FViewportCaptureRequest parameter would stop resolving if it stayed inside.
namespace Detail
{

    // The pose is applied through PinWrightRenderCapture::ApplyCaptureCamera - the SAME function
    // the capture path uses, not a mirrored copy - so the view rebuilt below is pose-identical to
    // the frame a capture with the same request produced. This used to be a hand-kept duplicate;
    // any drift between the two made deprojected rays land on the wrong pixel.

    // RAII: resolves the viewport - the caller's target, or the active Level Editor one when no
    // target was given - saves its camera + size, applies the requested pose + fixed size, and
    // rebuilds the FSceneView via CalcSceneView. Restores everything on destruction (fixed size ->
    // auto, camera fields, projection type, FOV/OrthoZoom), mirroring CaptureEditorViewportToPng's
    // save/apply/restore scope. GetView() is owned by the internal ViewFamily and is valid only
    // while this object is alive.
    //
    // NOTHING BELOW THE ACQUISITION IS LEVEL-SPECIFIC, which is why the target could be lifted in
    // at all: ApplyCaptureCamera takes an FEditorViewportClient& and a bare FViewport*
    // (PreviewViewportCaptureUtils.h), CalcSceneView is a virtual on the client, and
    // FEditorViewportClient::GetScene() returns GetWorld()->Scene - the preview world for a preview
    // client, the editor world for a level one (UE 5.8 EditorViewportClient.cpp:4633-4642). The
    // level viewport was never a requirement of the maths, only of the lookup.
    struct FScopedRebuiltView
    {
        FScopedRebuiltView(const FViewportCaptureRequest& Request, const FViewProjectionTarget& Target)
        {
            if (Target.IsExplicit())
            {
                // An explicit target is the caller stating which pixels this projection has to
                // agree with, so GEditor and the editor WORLD are deliberately not consulted on
                // this branch: a preview client draws its own transient world, and whether a level
                // is loaded says nothing about whether this view can be built.
                if (!Target.SceneViewport.IsValid())
                {
                    Error = TEXT("PREVIEW_VIEWPORT_NOT_FOUND: the supplied viewport client has no ")
                        TEXT("SceneViewport, so the view cannot be rebuilt at the captured size");
                    return;
                }
                Client = Target.Client;
                SceneViewport = Target.SceneViewport;
            }
            else if (!ResolveActiveLevelViewport())
            {
                return;
            }

            // Save originals BEFORE mutating so the restore in ~FScopedRebuiltView is exact. Same set
            // of fields CaptureEditorViewportToPng saves/restores.
            SavedLocation = Client->GetViewLocation();
            SavedRotation = Client->GetViewRotation();
            SavedViewportType = Client->GetViewportType();
            SavedFovAngle = Client->FOVAngle;
            SavedViewFov = Client->ViewFOV;
            SavedOrthoZoom = Client->GetOrthoZoom();
            // ApplyCaptureCamera turns the orbit camera off on the perspective path, because in
            // orbit mode SetViewLocation / SetViewRotation do not aim the view at all (see its
            // header comment). That is exactly as necessary here as it is for a capture -- a ray
            // deprojected through an orbit-mode view lands 90 degrees off the pixel it came from --
            // but the flag belongs to a live viewport the user is looking at (the level one, or an
            // asset editor's preview), so it has to go back.
            SavedLookAt = Client->GetLookAtLocation();
            bSavedOrbitCamera = Client->bUsingOrbitCamera;
            bRestore = true;

            // No capture could have produced an image from a tilted orthographic pose (the editor
            // derives its ortho view matrix from the viewport type), so refuse to invent a ray for
            // one. Same code and threshold as CaptureEditorViewportToPng, keeping
            // spatial.raycast_screen / spatial.place_on_surface exactly as strict as the capture
            // that is supposed to have produced the pixel.
            if (Request.ProjectionMode == TEXT("orthographic") && !Request.bPreserveViewportType)
            {
                PinWrightRenderCapture::FOrthographicViewResolution OrthoResolution;
                if (!PinWrightRenderCapture::ResolveOrthographicView(Request.Rotation, OrthoResolution))
                {
                    Error = FString::Printf(
                        TEXT("UNSUPPORTED_ORTHOGRAPHIC_ROTATION: orthographic poses must look along a world ")
                        TEXT("axis (within %.2f deg); rotation (pitch=%.2f, yaw=%.2f, roll=%.2f) is tilted."),
                        PinWrightRenderCapture::OrthoAxisToleranceDegrees,
                        Request.Rotation.Pitch, Request.Rotation.Yaw, Request.Rotation.Roll);
                    Client = nullptr;
                    return;
                }
            }

            // Aspect matters: disable any controlling-camera projection exactly as the pixel
            // capture does, then make the view rect match the captured dimensions.
            ProjectionAspectScope = MakeUnique<PinWrightRenderCapture::FScopedCaptureProjectionAspect>(
                *Client, Request.Width, Request.Height);

            // The view rect CalcSceneView reads from Viewport->GetSizeXY() must equal the captured
            // dimensions or the principal point (and thus every pixel) shifts.
            // SetFixedViewportSize -> ResizeViewport updates SizeXY synchronously (no Draw needed).
            SceneViewport->SetFixedViewportSize(Request.Width, Request.Height);
            PinWrightRenderCapture::ApplyCaptureCamera(*Client, SceneViewport.Get(), Request);

            // Build the same view family the engine builds for this client, then compute the view.
            // Realtime is immaterial to the projection matrices; set it from the client to be faithful.
            ViewFamily = MakeUnique<FSceneViewFamilyContext>(FSceneViewFamily::ConstructionValues(
                SceneViewport.Get(),
                Client->GetScene(),
                Client->EngineShowFlags)
                .SetRealtimeUpdate(Client->IsRealtime()));

            View = Client->CalcSceneView(ViewFamily.Get());
            if (!View)
            {
                Error = TEXT("VIEW_BUILD_FAILED: CalcSceneView returned null");
                // Leave bRestore=true so the destructor still reverts the applied pose/size.
            }
        }

        // The default target: the active Level Editor viewport, resolved exactly as this file has
        // always resolved it - same order, same four typed errors, same message text - so a call
        // that supplies no target is indistinguishable from a pre-target one.
        bool ResolveActiveLevelViewport()
        {
            if (!GEditor)
            {
                Error = TEXT("EDITOR_NOT_AVAILABLE: editor is not available");
                return false;
            }
            if (!GEditor->GetEditorWorldContext().World())
            {
                Error = TEXT("NO_EDITOR_WORLD: no active editor world");
                return false;
            }

            FLevelEditorModule* LevelEditorModule =
                FModuleManager::GetModulePtr<FLevelEditorModule>(TEXT("LevelEditor"));
            if (!LevelEditorModule)
            {
                Error = TEXT("NO_ACTIVE_LEVEL_VIEWPORT: LevelEditor module is not loaded");
                return false;
            }

            TSharedPtr<IAssetViewport> ActiveViewport = LevelEditorModule->GetFirstActiveViewport();
            if (!ActiveViewport.IsValid())
            {
                Error = TEXT("NO_ACTIVE_LEVEL_VIEWPORT: no active Level Editor viewport");
                return false;
            }

            Client = &ActiveViewport->GetAssetViewportClient();
            SceneViewport = ActiveViewport->GetSharedActiveViewport();
            if (!SceneViewport.IsValid())
            {
                Client = nullptr;
                Error = TEXT("NO_ACTIVE_LEVEL_VIEWPORT: active viewport has no SceneViewport");
                return false;
            }
            return true;
        }

        ~FScopedRebuiltView()
        {
            // Free the view family (and its FSceneView) before reverting the viewport size, so no
            // render command references the viewport RHI across the restore-time resize flush.
            View = nullptr;
            ViewFamily.Reset();

            if (bRestore && Client)
            {
                if (SceneViewport.IsValid())
                {
                    SceneViewport->SetFixedViewportSize(0, 0);
                }
                // Orbit first, and before the pose: ToggleOrbitCamera rewrites both the location
                // and the rotation from the pivot, so a pose put back ahead of it is discarded.
                // Same ordering, and the same reason, as CaptureEditorViewportToPng's guard.
                if (bSavedOrbitCamera && !Client->bUsingOrbitCamera)
                {
                    Client->ToggleOrbitCamera(true);
                    // Only on this branch, for the same reason as in CaptureEditorViewportToPng:
                    // SetLookAtLocation writes through GetViewTransform(), and orbit was only ever
                    // suppressed on the perspective one.
                    Client->SetLookAtLocation(SavedLookAt, /*bRecalculateView=*/false);
                }
                Client->SetViewLocation(SavedLocation);
                Client->SetViewRotation(SavedRotation);
                Client->SetViewportType(SavedViewportType);
                Client->FOVAngle = SavedFovAngle;
                Client->ViewFOV = SavedViewFov;
                if (SavedOrthoZoom > 0.0f)
                {
                    Client->SetOrthoZoom(SavedOrthoZoom);
                }
                ProjectionAspectScope.Reset();
                Client->Invalidate();
            }
        }

        FScopedRebuiltView(const FScopedRebuiltView&) = delete;
        FScopedRebuiltView& operator=(const FScopedRebuiltView&) = delete;

        bool IsValid() const { return View != nullptr; }
        FSceneView* GetView() const { return View; }
        const FString& GetError() const { return Error; }

    private:
        FEditorViewportClient* Client = nullptr;
        TSharedPtr<FSceneViewport> SceneViewport;
        TUniquePtr<FSceneViewFamilyContext> ViewFamily;
        TUniquePtr<PinWrightRenderCapture::FScopedCaptureProjectionAspect> ProjectionAspectScope;
        FSceneView* View = nullptr; // owned by ViewFamily
        FString Error;

        bool bRestore = false;
        FVector SavedLocation = FVector::ZeroVector;
        FRotator SavedRotation = FRotator::ZeroRotator;
        ELevelViewportType SavedViewportType = LVT_Perspective;
        float SavedFovAngle = 90.0f;
        float SavedViewFov = 90.0f;
        float SavedOrthoZoom = 0.0f;
        FVector SavedLookAt = FVector::ZeroVector;
        bool bSavedOrbitCamera = false;
    };
}

bool DeprojectScreenToWorld(
    const FViewportCaptureRequest& View,
    float PixelX,
    float PixelY,
    FVector& OutWorldOrigin,
    FVector& OutWorldDir,
    FString& OutErr,
    const FViewProjectionTarget& Target)
{
    OutErr.Reset();

    Detail::FScopedRebuiltView Scoped(View, Target);
    if (!Scoped.IsValid())
    {
        OutErr = Scoped.GetError();
        return false;
    }

    // DeprojectFVector2D internally uses the view's UnscaledViewRect and InvViewProjection matrix,
    // treating ScreenPos with a top-left origin (see FSceneView::DeprojectScreenToWorld). The center
    // pixel maps to camera forward; both perspective and orthographic views are handled.
    Scoped.GetView()->DeprojectFVector2D(FVector2D(PixelX, PixelY), OutWorldOrigin, OutWorldDir);
    return true;
}

FViewProjectionSession::FViewProjectionSession(const FViewportCaptureRequest& View)
    : Scoped(MakeUnique<Detail::FScopedRebuiltView>(View, FViewProjectionTarget()))
{
}

FViewProjectionSession::FViewProjectionSession(const FViewportCaptureRequest& View,
    const FViewProjectionTarget& Target)
    : Scoped(MakeUnique<Detail::FScopedRebuiltView>(View, Target))
{
}

// Out-of-line so the header can hold a TUniquePtr to the incomplete Detail::FScopedRebuiltView.
FViewProjectionSession::~FViewProjectionSession() = default;

bool FViewProjectionSession::IsValid() const
{
    return Scoped.IsValid() && Scoped->IsValid();
}

const FString& FViewProjectionSession::GetError() const
{
    static const FString EmptyError;
    return Scoped.IsValid() ? Scoped->GetError() : EmptyError;
}

bool FViewProjectionSession::Project(
    const FVector& WorldPoint,
    float& OutPixelX,
    float& OutPixelY,
    bool& OutBehindCamera) const
{
    OutBehindCamera = false;

    if (!IsValid())
    {
        return false;
    }

    const FSceneView* SceneView = Scoped->GetView();
    FVector2D ScreenPos = FVector2D::ZeroVector;
    // bShouldCalcOutsideViewPosition=true so AABB corners behind/outside the frustum still yield a
    // pixel for overlay clipping; ProjectWorldToScreen returns false when the point is behind the
    // camera plane (W <= 0), which we surface as OutBehindCamera without failing the call.
#if UE_VERSION_OLDER_THAN(5, 4, 0)
    // UE 5.3: ProjectWorldToScreen has no bShouldCalcOutsideViewPosition parameter (added in 5.4).
    // Replicate that overload's behind-camera behavior: project against |W| so the point still
    // yields a pixel, and report in-front-ness from the sign of W (mirrors the 5.4 engine math).
    bool bInFront = false;
    {
        const FIntRect& ViewRect = SceneView->UnscaledViewRect;
        const FPlane Projected = SceneView->ViewMatrices.GetViewProjectionMatrix()
            .TransformFVector4(FVector4(WorldPoint, 1.0));
        bInFront = Projected.W > 0.0f;
        const float RHW = 1.0f / FMath::Abs(Projected.W);
        const float NormalizedX = (Projected.X * RHW / 2.0f) + 0.5f;
        const float NormalizedY = 1.0f - (Projected.Y * RHW / 2.0f) - 0.5f;
        ScreenPos = FVector2D(
            NormalizedX * static_cast<float>(ViewRect.Width()) + static_cast<float>(ViewRect.Min.X),
            NormalizedY * static_cast<float>(ViewRect.Height()) + static_cast<float>(ViewRect.Min.Y));
    }
#else
    const bool bInFront = FSceneView::ProjectWorldToScreen(
        WorldPoint,
        SceneView->UnscaledViewRect,
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 8, 0)
        // UE 5.8 renamed FViewMatrices::GetViewProjectionMatrix() to GetWorldToClip() and
        // deprecated the old spelling (C4996); GetWorldToClip does not exist pre-5.8.
        SceneView->ViewMatrices.GetWorldToClip(),
#else
        SceneView->ViewMatrices.GetViewProjectionMatrix(),
#endif
        ScreenPos,
        /*bShouldCalcOutsideViewPosition=*/true);
#endif

    OutBehindCamera = !bInFront;
    OutPixelX = static_cast<float>(ScreenPos.X);
    OutPixelY = static_cast<float>(ScreenPos.Y);
    return true;
}

bool ProjectWorldToScreen(
    const FViewportCaptureRequest& View,
    const FVector& WorldPoint,
    float& OutPixelX,
    float& OutPixelY,
    bool& OutBehindCamera,
    FString& OutErr,
    const FViewProjectionTarget& Target)
{
    OutErr.Reset();
    OutBehindCamera = false;

    // One-point convenience wrapper: build a session, project, tear it down. There is deliberately
    // only ONE projection implementation (FViewProjectionSession::Project) for the same reason the
    // ortho patch deleted this file's mirrored copy of ApplyCaptureCamera - a second copy is how
    // overlay pixels silently drift off the frame they are supposed to annotate.
    //
    // Callers projecting more than a couple of points must open the session themselves; see the
    // cost model in ViewProjectionUtils.h.
    const FViewProjectionSession Session(View, Target);
    if (!Session.IsValid())
    {
        OutErr = Session.GetError();
        return false;
    }
    return Session.Project(WorldPoint, OutPixelX, OutPixelY, OutBehindCamera);
}
}
