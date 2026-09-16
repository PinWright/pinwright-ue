// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Containers/ArrayView.h"
// FWeakObjectPtr's definition, for FPreviewInstanceState's TWeakObjectPtr member. CoreMinimal.h
// brings the TWeakObjectPtr TEMPLATE but not the FWeakObjectPtr it stores, and instantiating one
// without this fails with "uses undefined struct 'FWeakObjectPtr'".
#include "UObject/WeakObjectPtr.h"
// The resolver contract. Read from the header on disk, not from §2.2 of the plan: the frozen prose
// is missing bCloseAfterCaptureProvided, FSubjectReleaseState/ProviderState and the whole §2.5 walk,
// and a provider written against the prose hand-rolls all three.
#include "Handlers/Render/CaptureSubject.h"

class FEditorViewportClient;
class UAnimationAsset;
class UDebugSkelMeshComponent;
class USkeletalMesh;
class USkeletalMeshComponent;
class UObject;

// THE ANIMATION-ASSET SUBJECT KIND: an animation, posed on its own preview mesh, in its own
// Persona-family editor. No level, no placed actor, no Level Sequence.
//
// It is the sibling of the skeletalMesh kind and deliberately not the same thing. There the MESH is
// the subject and an animation is an optional axis added to it; here the ANIMATION is the subject
// and the mesh is derived from it — which is the case §1a of the plan calls the animation-asset
// domain, and the one render.capture_animation_preview already serves.
//
// TWO PHASES, and the split is load-bearing. ResolveAnimationSubjectFacts settles everything the
// request alone decides — the animation, the preview mesh, the bounds, whether a time axis exists —
// and opens NOTHING. So a malformed request is refused before any editor state is mutated, and the
// bounds contract is measurable on a host with no realized preview viewport. The same split is in
// CaptureSubjectProviders_Mesh.h and for the same reasons.
//
// BOTH BOUNDS STRATEGIES LIVE HERE, AND THE KIND PICKS ONE.
// Decision 4 of the plan records that this domain has two CORRECT bounds implementations and that
// the resolver must express both rather than pick one:
//   * asset bounds — render.capture_animation_preview frames from USkeletalMesh::GetBounds()
//     (AnimationPreviewCaptureHandler.cpp:715), so the camera is identical at every instant and a
//     pose difference cannot be confused with a camera move. This is the DEFAULT for this kind and
//     is what BoundsSource == "assetBounds" means.
//   * sampled union — camera.animation_shots walks the frame plan once WITHOUT capturing, unions
//     the bounds across every sampled instant (AnimationShotsHandler.cpp:515-537), and holds that
//     union for the whole burst. Also camera-stable, because the union is computed once up front;
//     it is the right answer when the motion escapes the rest bounds — a kick, a swing, a rig that
//     translates. ComputeSampledPosedUnionBounds / ApplySampledUnionBounds are that implementation
//     for this kind, reported as BoundsSource == "sampledUnion".
// Acquire installs asset bounds. A verb that needs the union asks for it explicitly and the response
// says which one it got. The one thing that must never happen is bounds that change per instant.
namespace PinWrightCaptureSubjectAnimation
{
    using PinWrightCaptureSubject::ESubjectKind;
    using PinWrightCaptureSubject::FResolvedSubject;
    using PinWrightCaptureSubject::FSubjectRequest;
    using PinWrightCaptureSubject::FSubjectTimeSetter;

    // Spelled exactly as render.capture_animation_preview already ships it
    // (AnimationPreviewCaptureHandler.cpp:903) — a converged verb must not rename the field its
    // callers already match on. Same string as the skeletalMesh kind's, because it is the same
    // viewport.
    inline constexpr TCHAR CaptureSourcePersonaViewport[] = TEXT("personaPreviewViewport");
    inline constexpr TCHAR BoundsSourceAssetBounds[] = TEXT("assetBounds");
    inline constexpr TCHAR BoundsSourceSampledUnion[] = TEXT("sampledUnion");

    // ---- Preview mesh resolution -------------------------------------------------------------

    // Where the preview mesh came from. Worth reporting because a fallback mesh can have a
    // completely different silhouette from the one the animation was authored against, and a
    // reviewer looking at the wrong body reads that as an animation defect.
    enum class EPreviewMeshSource : uint8
    {
        None,
        AnimationAsset,   // UAnimationAsset::PreviewSkeletalMesh, set on the animation itself
        SkeletonPreview,  // USkeleton::PreviewSkeletalMesh, via GetAssetPreviewMesh
        CompatibleMesh,   // USkeleton::FindCompatibleMesh — the asset registry's first match
        RequestedMesh,    // the caller paired the animation with an explicit Skeletal Mesh path
    };
    const TCHAR* PreviewMeshSourceName(EPreviewMeshSource Source);

    // The mesh an animation asset should be previewed on, following Persona's own chain.
    //
    // UAnimationAsset::GetPreviewMesh(bFindIfNotSet) IGNORES ITS ARGUMENT on UE 5.8. The whole body
    // is PreviewSkeletalMesh.LoadSynchronous() plus a skeleton-compatibility nullify — there is no
    // bFindIfNotSet branch at all (Engine/Source/Runtime/Engine/Private/AnimationAsset.cpp:400-415).
    // Passing `true` and stopping there resolves an animation whose own preview mesh is unset to
    // nothing, which reads as "this animation has no mesh" on assets Persona opens without
    // complaint. render.capture_animation_preview:388-390 does exactly that and documents the
    // fallback it is not getting.
    //
    // The fallback lives on USkeleton, and only two of its accessors are reachable from a plugin:
    // USkeleton is UCLASS(..., MinimalAPI) (Skeleton.h:293) and its GetPreviewMesh overloads carry
    // no ENGINE_API of their own (:646-647), so a call to them links against nothing.
    // GetAssetPreviewMesh (:667) and FindCompatibleMesh (:670) do carry one, and together they are
    // the same chain GetPreviewMesh(true) walks internally (Skeleton.cpp:1191-1215) — minus its side
    // effect of writing the choice back onto the skeleton, which a read-only capture path should not
    // leave behind.
    USkeletalMesh* ResolvePreviewMesh(UAnimationAsset* AnimationAsset, EPreviewMeshSource& OutSource);

    // ---- What the request alone decides ------------------------------------------------------

    struct FAnimationSubjectFacts
    {
        // The asset whose editor is opened: whatever the caller named, so "the editor this call
        // opened" means the window they would recognise.
        UObject*         EditorAsset = nullptr;
        UAnimationAsset* AnimationAsset = nullptr;
        USkeletalMesh*   PreviewMesh = nullptr;
        EPreviewMeshSource PreviewMeshSource = EPreviewMeshSource::None;

        FVector  BoundsOrigin = FVector::ZeroVector;
        double   BoundsRadius = 0.0;
        FString  BoundsSource;
        FString  CaptureSource;
        bool     bTimeSupported = false;
        double   TimeStartSeconds = 0.0;
        double   TimeEndSeconds = 0.0;
    };

    // Load the animation, resolve its preview mesh, refuse a skeleton mismatch, and measure the
    // asset bounds. Opens no editor, touches no Slate, needs no GEditor.
    bool ResolveAnimationSubjectFacts(const FSubjectRequest& Request, FAnimationSubjectFacts& Out,
        FString& OutErrCode, FString& OutErrMsg);

    // ---- Bounds ------------------------------------------------------------------------------

    // Decision 4's default for this kind: bounds off the MESH ASSET, never off the posed component.
    // Returns false when the mesh reports a degenerate sphere, which the caller turns into
    // ERR_BOUNDS_EMPTY rather than framing a camera on a zero radius.
    bool ComputeAssetBounds(const USkeletalMesh& Mesh, FVector& OutOrigin, double& OutRadius);

    // Component-space AABB over the current pose's bone origins — the measurement that is
    // guaranteed to follow the pose.
    //
    // USkinnedMeshComponent::CalcBounds does NOT have that property: when the component is not
    // rendering (!ShouldRender()) or carries bComponentUseFixedSkelBounds it returns the skeletal
    // mesh asset's bounds transformed by the component (SkinnedMeshComponent.cpp:2250-2256). A union
    // built from CalcBounds alone therefore degenerates into asset bounds on any hidden component —
    // it looks like a measurement and is a constant.
    FBox ComputePosedBoneBox(const USkeletalMeshComponent& Component);

    // The second correct strategy: walk the sample times once WITHOUT capturing, union the posed
    // bounds, and hand back one bounds pair to hold for the whole set. Unions CalcBounds (what the
    // renderer culls and frames against) with ComputePosedBoneBox (what always follows the pose),
    // for the reason above.
    //
    // Leaves the component scrubbed to the LAST sample time; a caller that needs a specific instant
    // afterwards re-runs the time setter, exactly as a burst loop already does.
    bool ComputeSampledPosedUnionBounds(USkeletalMeshComponent& Component,
        const FSubjectTimeSetter& TimeSetter,
        TArrayView<const double> SampleTimesSeconds,
        FVector& OutOrigin, double& OutRadius,
        FString& OutErrCode, FString& OutErrMsg);

    // ComputeSampledPosedUnionBounds written into an already-resolved subject, including
    // BoundsSource = "sampledUnion". A union that cannot be computed leaves the subject exactly as
    // it was — its asset bounds — rather than zeroing it.
    bool ApplySampledUnionBounds(FResolvedSubject& Subject,
        USkeletalMeshComponent& Component,
        const FSubjectTimeSetter& TimeSetter,
        TArrayView<const double> SampleTimesSeconds,
        FString& OutErrCode, FString& OutErrMsg);

    // ---- Time axis ---------------------------------------------------------------------------

    // Pose the component at one instant, synchronously, on the game thread.
    //
    // SetPosition ALONE POSES NOTHING. UAnimSingleNodeInstance::SetPosition only writes the proxy's
    // current time; the pose does not move until something ticks the anim instance and refreshes the
    // bone transforms. Both are driven here rather than left to the preview scene's own tick, so the
    // pose is correct before the first Slate pump instead of one tick later — the capture path pumps
    // three times per shot, and a preview left playing would be at three different instants.
    //
    // The refusal codes follow the split CaptureSubject.h documents on FSubjectTimeSetter:
    // ERR_INVALID_ARGUMENT for a non-finite instant (caller-fixable), ERR_UNSUPPORTED_ASSET_EDITOR
    // when the preview holds no animation and therefore has no axis at all,
    // ERR_PREVIEW_NOT_FOUND when the component or its instance has gone away.
    bool ScrubToTimeSeconds(USkeletalMeshComponent& Component, double TimeSeconds,
        FString& OutErrCode, FString& OutErrMsg);

    // The preview viewport is driven synchronously by Slate in this plugin, not by a normal world
    // tick. Drain the preview world's deferred render updates after the game-thread pose is
    // finalized, or the render proxy can still draw the previous pose.
    bool FlushPoseRenderState(USkeletalMeshComponent& Component,
        FString& OutErrCode, FString& OutErrMsg);

    // ScrubToTimeSeconds as the resolver's time seam (FCameraPose::SubjectTimeSeconds, declared and
    // deliberately dead at PoseListCapture.h:62-77, whose comment writes out this exact three-kind
    // layering: a static mesh ignores the time, an animation SCRUBS to it, a Niagara system
    // SIMULATES to it). Holds the component weakly, so a preview torn down mid-set refuses with a
    // typed code instead of dereferencing a stale pointer.
    FSubjectTimeSetter MakeScrubTimeSetter(USkeletalMeshComponent& Component);

    // ---- Preview instance state --------------------------------------------------------------

    // Everything this kind changes on the preview component, so it can be put back. A review verb
    // that leaves an artist's Persona tab scrubbed to a different frame with a different animation
    // loaded is not read-only in any sense that matters. Field-for-field the state
    // AnimationPreviewCaptureHandler.cpp:196-210 already saves; a struct rather than loose fields on
    // the release state so the restore can be exercised without an asset editor.
    struct FPreviewInstanceState
    {
        // A single-node instance existed at entry, so its position/play/loop state is meaningful.
        bool bCaptured = false;
        // Whether the component was in single-node PREVIEW mode at all, as distinct from which asset
        // was loaded. An Animation Blueprint editor's component normally is not, and restoring it as
        // "preview on with a null asset" leaves the artist looking at a reference pose where their
        // graph used to be running.
        bool bPreviewOn = false;
        TWeakObjectPtr<UAnimationAsset> Asset;
        float Position = 0.0f;
        bool bPlaying = false;
        bool bLooping = true;

        void CaptureFrom(const UDebugSkelMeshComponent& Component);
        // Restores unconditionally, not only when a previous instance was captured: this kind turns
        // preview mode ON, so a component that had none must be put back to having none.
        void RestoreTo(UDebugSkelMeshComponent& Component) const;
    };

    // ---- The Persona preview component -------------------------------------------------------

    // The preview component, found through the viewport client's own preview scene rather than
    // through IPersonaToolkit: no downcast, no Persona header, and it works for every editor in the
    // family without knowing which one is open.
    //
    // OutCandidateCount reports how many matched, because Persona explicitly anticipates more than
    // one (FAnimationEditorPreviewScene::GetAllPreviewMeshComponents) and a host that adds its own
    // would otherwise silently change which mesh got photographed. The count is the reason this
    // walks every candidate instead of returning at the first preferred match.
    //
    // Exported rather than file-private because render.capture_animation_preview calls it too
    // (AnimationPreviewCaptureHandler.cpp:728): the verb and this provider had byte-identical copies,
    // and two copies of one rule is how the two stop agreeing about which mesh got photographed.
    // The skeletalMesh provider carried a third, file-private and count-free copy; it now calls
    // this one (CaptureSubjectProviders_Mesh.cpp). That copy returned at the first mesh match
    // rather than finishing the walk, which produced the same component for every input but no
    // count -- so nothing about which mesh gets photographed changed when it went.
    UDebugSkelMeshComponent* FindPreviewMeshComponent(FEditorViewportClient& ViewportClient,
        USkeletalMesh* PreferredMesh, int32& OutCandidateCount);

    // ---- The registered provider -------------------------------------------------------------

    // Opens the Persona editor through PinWrightCaptureSubject::AcquireAssetEditorViewport — the one
    // shared §2.5 walk, with its toolkit-name gate and its verified widget match — and never its own
    // copy of it.
    bool AcquireAnimationSubject(const FSubjectRequest& Request, FResolvedSubject& OutSubject,
        FSubjectTimeSetter& OutTimeSetter, FString& OutErrCode, FString& OutErrMsg);

    // Restores the preview instance this call installed and applies the three-state close rule
    // through PinWrightCaptureSubject::CloseAssetEditor, writing the MEASURED outcome into
    // FResolvedSubject::bEditorClosed. Idempotent: it consumes ProviderState.
    void ReleaseAnimationSubject(FResolvedSubject& Subject);
}
