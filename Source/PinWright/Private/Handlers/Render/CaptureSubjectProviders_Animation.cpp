// Copyright (c) 2026 Alexander Penkin. MIT License.

// The ANIMATION-ASSET provider for the capture subject resolver: one animation, posed on its own
// preview mesh, in its own Persona-family editor.
//
// It registers itself at static init and edits no other file — that is the whole point of the
// registry (plan decision 3). Adding a subject kind adds a file; it does not touch a switch, a
// dispatcher or a verb.
//
// WHAT THIS FILE OWNS, and what it deliberately does not:
//
//  * It owns ACQUISITION — settle the animation and its preview mesh from the request, open the
//    editor through the ONE shared walk in CaptureSubject.h, find the preview component, freeze it,
//    and hand back a viewport client plus bounds plus a time setter. Every capture concern below
//    that (exposure pin, scoped view mode, sprite suppression, ORBIT SUPPRESSION, both viewport
//    transform slots, the aim verification, the blank/framing checks) already lives in
//    CaptureEditorViewportToPng and is not repeated here. render.capture_animation_preview
//    hand-rolls the orbit save/suppress/restore at :730-744 that the shared capture already performs
//    at PreviewViewportCaptureUtils.cpp:1431-1461 — that duplication is the motivating evidence for
//    this whole convergence, so reproducing it here would be self-defeating.
//  * It owns the RESTORE of everything it touched, on every exit path including the error paths.
//    The release state is installed BEFORE the editor is opened, so a failure three statements later
//    still closes what it opened.
//  * It does NOT solve a camera. FResolvedSubject carries bounds; how far back to stand is the
//    verb's decision (fov, padding, an explicit radius), and putting it here would fork the framing
//    maths a fifth time.
//  * It does NOT walk widget trees or cast toolkits. Both of those are single-copy in
//    CaptureSubject.h now, with the toolkit-name gate before the FAssetEditorToolkit cast and the
//    two-way widget verification before the SEditorViewport cast.
#include "Handlers/Render/CaptureSubjectProviders_Animation.h"

#include "Handlers/ErrorCodes.h"
// PersonaFamilyToolkitNames(): the same four toolkit names the skeletalMesh kind accepts, declared
// once by the mesh provider. Sharing the list is the point — two spellings of "the Persona family"
// is how one kind quietly starts accepting an editor the other refuses.
#include "Handlers/Render/CaptureSubjectProviders_Mesh.h"

#include "Animation/AnimationAsset.h"
#include "Animation/AnimSequenceBase.h"
#include "Animation/AnimSingleNodeInstance.h"
#include "Animation/DebugSkelMeshComponent.h"
#include "Animation/Skeleton.h"
#include "Components/SkeletalMeshComponent.h"
#include "Editor.h"
#include "EditorViewportClient.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/World.h"
// FPreviewScene: EditorViewportClient.h only forward-declares it, and GetPreviewScene()->GetWorld()
// needs the complete type.
#include "PreviewScene.h"
#include "Slate/SceneViewport.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "UObject/UObjectIterator.h"

// File-unique NAMED namespace, not an anonymous one: Unity merges translation units and the sibling
// Render/*.cpp files carry same-shaped helpers.
namespace PinWrightCaptureSubjectAnimation
{
    using namespace PinWrightCaptureSubject;

    namespace Internal
    {
        // Per-call state, type-erased into FResolvedSubject::ProviderState. Providers are registered
        // once at static init and FSubjectProvider::Release takes only FResolvedSubject&, so this is
        // the only place per-call state can live.
        struct FAnimationReleaseState : public FSubjectReleaseState
        {
            TWeakObjectPtr<UObject> Asset;
            // The three states, carried whole: absent closes only a window this call opened, an
            // explicit true closes one the caller already had open as well, false leaves it
            // (RenderHandler.cpp:395-412).
            bool bCloseAfterCapture = true;
            bool bCloseRequestedExplicitly = false;
            bool bWasAlreadyOpen = false;

            TWeakObjectPtr<UDebugSkelMeshComponent> PreviewComponent;
            // This call called EnablePreview and must undo it.
            bool bPreviewInstalled = false;
            FPreviewInstanceState SavedPreview;

            // The visibility flag the preview component carried when this call found it, saved
            // separately from FPreviewInstanceState because that struct restores the ANIMATION
            // (asset, position, playing, looping) and is applied only when bPreviewInstalled. A
            // hidden component has to be put back whether or not a preview was installed.
            bool bSubjectVisibilityCaptured = false;
            bool bSubjectWasVisible = true;
        };

        bool MeasureEditorWasAlreadyOpen(UObject* Asset)
        {
            if (!Asset || !GEditor)
            {
                return false;
            }
            UAssetEditorSubsystem* Subsystem = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();
            return Subsystem && Subsystem->FindEditorForAsset(Asset, /*bFocusIfOpen=*/false) != nullptr;
        }
    }

    // ---------------------------------------------------------------------------------------
    // The Persona preview component

    UDebugSkelMeshComponent* FindPreviewMeshComponent(FEditorViewportClient& ViewportClient,
        USkeletalMesh* PreferredMesh, int32& OutCandidateCount)
    {
        OutCandidateCount = 0;
        FPreviewScene* PreviewScene = ViewportClient.GetPreviewScene();
        UWorld* PreviewWorld = PreviewScene ? PreviewScene->GetWorld() : nullptr;
        if (!PreviewWorld)
        {
            return nullptr;
        }

        UDebugSkelMeshComponent* FirstMatch = nullptr;
        UDebugSkelMeshComponent* PreferredMatch = nullptr;
        for (TObjectIterator<UDebugSkelMeshComponent> It; It; ++It)
        {
            UDebugSkelMeshComponent* Component = *It;
            // TObjectIterator also visits class default objects, and every other Persona instance in
            // the editor owns a preview component in its own preview world.
            if (!Component || Component->IsTemplate() || Component->GetWorld() != PreviewWorld)
            {
                continue;
            }
            // Every candidate is counted, so the walk does not stop at the preferred one: the count
            // is the whole point of reporting it, and a caller that learns "there were three" reads
            // its own picture differently from one told "there was one".
            ++OutCandidateCount;
            if (!FirstMatch)
            {
                FirstMatch = Component;
            }
            if (PreferredMesh && !PreferredMatch && Component->GetSkeletalMeshAsset() == PreferredMesh)
            {
                PreferredMatch = Component;
            }
        }
        return PreferredMatch ? PreferredMatch : FirstMatch;
    }

    // ---------------------------------------------------------------------------------------
    // Preview mesh resolution

    const TCHAR* PreviewMeshSourceName(EPreviewMeshSource Source)
    {
        switch (Source)
        {
        case EPreviewMeshSource::AnimationAsset:  return TEXT("animationAsset");
        case EPreviewMeshSource::SkeletonPreview: return TEXT("skeletonPreview");
        case EPreviewMeshSource::CompatibleMesh:  return TEXT("compatibleMesh");
        case EPreviewMeshSource::RequestedMesh:   return TEXT("requestedMesh");
        default:                                  return TEXT("none");
        }
    }

    USkeletalMesh* ResolvePreviewMesh(UAnimationAsset* AnimationAsset, EPreviewMeshSource& OutSource)
    {
        OutSource = EPreviewMeshSource::None;
        if (!AnimationAsset)
        {
            return nullptr;
        }

        // Step 1: the animation's own preview mesh. The `true` is passed for symmetry with the rest
        // of the engine's call sites; the header records that it is a no-op on 5.8.
        if (USkeletalMesh* OwnPreview = AnimationAsset->GetPreviewMesh(/*bFindIfNotSet=*/true))
        {
            OutSource = EPreviewMeshSource::AnimationAsset;
            return OwnPreview;
        }

        USkeleton* Skeleton = AnimationAsset->GetSkeleton();
        if (!Skeleton)
        {
            return nullptr;
        }

        // Step 2: the skeleton's preview mesh, through the accessor Persona itself uses. It
        // re-checks the asset first (already null here) and then the skeleton's, including the
        // IsCompiling guard that keeps a half-built mesh out of the preview (Skeleton.cpp:1382-1410).
        if (USkeletalMesh* SkeletonPreview = Skeleton->GetAssetPreviewMesh(AnimationAsset))
        {
            OutSource = EPreviewMeshSource::SkeletonPreview;
            return SkeletonPreview;
        }

        // Step 3, last resort: the first compatible mesh from the asset registry — what
        // USkeleton::GetPreviewMesh(true) reaches internally (Skeleton.cpp:1203-1212), minus its
        // side effect of writing the choice back onto somebody else's skeleton.
        if (USkeletalMesh* Compatible = Skeleton->FindCompatibleMesh())
        {
            OutSource = EPreviewMeshSource::CompatibleMesh;
            return Compatible;
        }
        return nullptr;
    }

    // ---------------------------------------------------------------------------------------
    // What the request alone decides

    bool ResolveAnimationSubjectFacts(const FSubjectRequest& Request, FAnimationSubjectFacts& Out,
        FString& OutErrCode, FString& OutErrMsg)
    {
        if (Request.Kind != ESubjectKind::Animation)
        {
            OutErrCode = ErrorCodes::ERR_INVALID_ARGUMENT;
            OutErrMsg = FString::Printf(
                TEXT("subject.kind '%s' is not served by the animation provider, which handles ")
                TEXT("'animation' only."),
                ToWireName(Request.Kind));
            return false;
        }

        // Either spelling names the animation: `path` when the animation IS the subject, or
        // `animation` beside a Skeletal Mesh `path` when the caller is choosing the body as well.
        UObject* PrimaryAsset = nullptr;
        if (!Request.AssetPath.IsEmpty())
        {
            PrimaryAsset = LoadObject<UObject>(nullptr, *Request.AssetPath);
            if (!PrimaryAsset)
            {
                OutErrCode = ErrorCodes::ERR_ASSET_NOT_FOUND;
                OutErrMsg = FString::Printf(TEXT("Asset not found: %s"), *Request.AssetPath);
                return false;
            }
        }

        if (!Request.AnimationPath.IsEmpty())
        {
            Out.AnimationAsset = LoadObject<UAnimationAsset>(nullptr, *Request.AnimationPath);
            if (!Out.AnimationAsset)
            {
                OutErrCode = ErrorCodes::ERR_ANIMATION_NOT_FOUND;
                OutErrMsg = FString::Printf(TEXT("Animation asset not found: %s"),
                    *Request.AnimationPath);
                return false;
            }
        }
        if (!Out.AnimationAsset)
        {
            Out.AnimationAsset = Cast<UAnimationAsset>(PrimaryAsset);
        }
        if (!Out.AnimationAsset)
        {
            OutErrCode = ErrorCodes::ERR_UNSUPPORTED_ASSET_EDITOR;
            OutErrMsg = PrimaryAsset
                ? FString::Printf(
                    TEXT("subject.kind 'animation' needs an animation asset; '%s' is a %s. Name the ")
                    TEXT("animation in subject.animation, or use subject.kind 'skeletalMesh' to ")
                    TEXT("capture the mesh in its bind pose."),
                    *Request.AssetPath, *PrimaryAsset->GetClass()->GetName())
                : FString(TEXT("subject.kind 'animation' needs an animation asset in subject.path or ")
                          TEXT("subject.animation; neither was given."));
            return false;
        }

        // An explicitly named mesh wins: a caller pairing a mesh with an animation is telling us
        // which body to photograph, and silently substituting the animation's own preview mesh would
        // answer a question nobody asked.
        Out.PreviewMesh = Cast<USkeletalMesh>(PrimaryAsset);
        if (Out.PreviewMesh)
        {
            Out.PreviewMeshSource = EPreviewMeshSource::RequestedMesh;
        }
        else
        {
            Out.PreviewMesh = ResolvePreviewMesh(Out.AnimationAsset, Out.PreviewMeshSource);
        }
        if (!Out.PreviewMesh)
        {
            OutErrCode = ErrorCodes::ERR_SKELETAL_MESH_NOT_FOUND;
            OutErrMsg = FString::Printf(
                TEXT("Animation '%s' resolves to no preview mesh: it names none, its skeleton names ")
                TEXT("none, and the asset registry holds no compatible mesh. Set a preview mesh on ")
                TEXT("the animation or on the skeleton, or pass subject.path naming a Skeletal Mesh ")
                TEXT("with subject.animation naming this animation."),
                *Out.AnimationAsset->GetPathName());
            return false;
        }

        // A mismatched pairing produces a mesh posed by a skeleton it was not bound to — limbs in
        // the wrong places, and a picture that looks like a rigging defect. Refuse rather than
        // photograph it.
        if (Out.AnimationAsset->GetSkeleton() && Out.PreviewMesh->GetSkeleton() &&
            Out.AnimationAsset->GetSkeleton() != Out.PreviewMesh->GetSkeleton())
        {
            OutErrCode = ErrorCodes::ERR_SKELETON_MISMATCH;
            OutErrMsg = FString::Printf(
                TEXT("Animation '%s' targets skeleton '%s' but mesh '%s' is bound to '%s'."),
                *Out.AnimationAsset->GetPathName(),
                *Out.AnimationAsset->GetSkeleton()->GetPathName(),
                *Out.PreviewMesh->GetPathName(),
                *Out.PreviewMesh->GetSkeleton()->GetPathName());
            return false;
        }

        Out.EditorAsset = PrimaryAsset ? PrimaryAsset : static_cast<UObject*>(Out.AnimationAsset);

        if (!ComputeAssetBounds(*Out.PreviewMesh, Out.BoundsOrigin, Out.BoundsRadius))
        {
            OutErrCode = ErrorCodes::ERR_BOUNDS_EMPTY;
            OutErrMsg = FString::Printf(
                TEXT("Skeletal mesh '%s' reports empty bounds, so no camera can be framed on it."),
                *Out.PreviewMesh->GetPathName());
            return false;
        }
        Out.BoundsSource = BoundsSourceAssetBounds;
        Out.CaptureSource = CaptureSourcePersonaViewport;

        // An animation asset always carries a time axis; what varies is whether its length is
        // readable. A blend space or a montage whose rate does not resolve reports zero, and the
        // caller sees a zero-length range rather than a wrong one.
        Out.bTimeSupported = true;
        Out.TimeStartSeconds = 0.0;
        const UAnimSequenceBase* SequenceBase = Cast<UAnimSequenceBase>(Out.AnimationAsset);
        Out.TimeEndSeconds = SequenceBase ? static_cast<double>(SequenceBase->GetPlayLength()) : 0.0;
        return true;
    }

    // ---------------------------------------------------------------------------------------
    // Bounds

    bool ComputeAssetBounds(const USkeletalMesh& Mesh, FVector& OutOrigin, double& OutRadius)
    {
        const FBoxSphereBounds Bounds = Mesh.GetBounds();
        OutOrigin = Bounds.Origin;
        OutRadius = static_cast<double>(Bounds.SphereRadius);
        return OutRadius > 0.0 && !OutOrigin.ContainsNaN() && FMath::IsFinite(OutRadius);
    }

    FBox ComputePosedBoneBox(const USkeletalMeshComponent& Component)
    {
        FBox Box(ForceInit);
        const TArray<FTransform>& Transforms = Component.GetComponentSpaceTransforms();
        for (const FTransform& Transform : Transforms)
        {
            Box += Transform.GetLocation();
        }
        return Box;
    }

    bool ComputeSampledPosedUnionBounds(USkeletalMeshComponent& Component,
        const FSubjectTimeSetter& TimeSetter,
        TArrayView<const double> SampleTimesSeconds,
        FVector& OutOrigin, double& OutRadius,
        FString& OutErrCode, FString& OutErrMsg)
    {
        if (SampleTimesSeconds.Num() == 0)
        {
            OutErrCode = ErrorCodes::ERR_INVALID_ARGUMENT;
            OutErrMsg = TEXT("A sampled-union bounds pre-pass needs at least one sample time; with none ")
                        TEXT("there is nothing to union and the caller wants asset bounds.");
            return false;
        }
        if (!TimeSetter)
        {
            OutErrCode = ErrorCodes::ERR_UNSUPPORTED_ASSET_EDITOR;
            OutErrMsg = TEXT("A sampled-union bounds pre-pass needs a time setter, and this subject has ")
                        TEXT("no time axis to scrub.");
            return false;
        }

        FBox Union(ForceInit);
        for (const double TimeSeconds : SampleTimesSeconds)
        {
            // Refuse rather than union a stale pose: a union that silently skipped the instant it
            // could not reach describes a motion envelope the subject never had.
            if (!TimeSetter(TimeSeconds, OutErrCode, OutErrMsg))
            {
                return false;
            }

            const FTransform ComponentToWorld = Component.GetComponentTransform();
            Union += Component.CalcBounds(ComponentToWorld).GetBox();

            // The bone box is COMPONENT space; bring it into the same space as CalcBounds before
            // unioning. See the header: CalcBounds degenerates to asset bounds on a component that
            // is not rendering, so the bone box is what keeps this measurement pose-driven.
            const FBox BoneBox = ComputePosedBoneBox(Component);
            if (BoneBox.IsValid)
            {
                Union += BoneBox.TransformBy(ComponentToWorld);
            }
        }

        if (!Union.IsValid)
        {
            OutErrCode = ErrorCodes::ERR_BOUNDS_EMPTY;
            OutErrMsg = TEXT("The preview component reported no bounds at any sampled instant, so no ")
                        TEXT("camera can be framed on it.");
            return false;
        }

        OutOrigin = Union.GetCenter();
        // Extent size, matching camera.animation_shots' own union radius
        // (AnimationShotsHandler.cpp:534) so a set moving between the two verbs is framed the same.
        OutRadius = Union.GetExtent().Size();
        if (!(OutRadius > 0.0) || !FMath::IsFinite(OutRadius))
        {
            OutErrCode = ErrorCodes::ERR_BOUNDS_EMPTY;
            OutErrMsg = TEXT("The sampled bounds union has zero radius, so no camera can be framed on it.");
            return false;
        }
        return true;
    }

    bool ApplySampledUnionBounds(FResolvedSubject& Subject,
        USkeletalMeshComponent& Component,
        const FSubjectTimeSetter& TimeSetter,
        TArrayView<const double> SampleTimesSeconds,
        FString& OutErrCode, FString& OutErrMsg)
    {
        FVector Origin = FVector::ZeroVector;
        double Radius = 0.0;
        if (!ComputeSampledPosedUnionBounds(Component, TimeSetter, SampleTimesSeconds,
                Origin, Radius, OutErrCode, OutErrMsg))
        {
            return false;
        }
        Subject.BoundsOrigin = Origin;
        Subject.BoundsRadius = Radius;
        Subject.BoundsSource = BoundsSourceSampledUnion;
        return true;
    }

    // ---------------------------------------------------------------------------------------
    // Time axis

    bool FlushPoseRenderState(USkeletalMeshComponent& Component,
        FString& OutErrCode, FString& OutErrMsg)
    {
        if (!Component.IsRegistered() || !Component.IsRenderStateCreated())
        {
            OutErrCode = ErrorCodes::ERR_PREVIEW_NOT_FOUND;
            OutErrMsg = TEXT("The preview component has no live render state, so the posed image ")
                        TEXT("cannot be captured safely.");
            return false;
        }

        UWorld* World = Component.GetWorld();
        if (!World)
        {
            OutErrCode = ErrorCodes::ERR_PREVIEW_NOT_FOUND;
            OutErrMsg = TEXT("The preview component has no world to drain its deferred render ")
                        TEXT("update, so the posed image cannot be captured safely.");
            return false;
        }

        // MarkRenderDynamicDataDirty queues the skeletal render-proxy update for the world's end
        // of frame. The synchronous capture path pumps Slate and draws immediately, so it never
        // reaches the normal editor tick that would drain this queue. Send it now; the capture
        // utility's render-command flush then makes the queued proxy update visible to the PNG.
        Component.MarkRenderDynamicDataDirty();
        World->SendAllEndOfFrameUpdates();
        return true;
    }

    bool ScrubToTimeSeconds(USkeletalMeshComponent& Component, double TimeSeconds,
        FString& OutErrCode, FString& OutErrMsg)
    {
        if (!FMath::IsFinite(TimeSeconds))
        {
            OutErrCode = ErrorCodes::ERR_INVALID_ARGUMENT;
            OutErrMsg = TEXT("Subject time must be a finite number of seconds.");
            return false;
        }

        UAnimSingleNodeInstance* SingleNode = Component.GetSingleNodeInstance();
        if (!SingleNode)
        {
            OutErrCode = ErrorCodes::ERR_PREVIEW_NOT_FOUND;
            OutErrMsg = TEXT("The preview component has no single-node animation instance, so the ")
                        TEXT("animation cannot be frozen at a given time.");
            return false;
        }
        if (!SingleNode->GetAnimationAsset())
        {
            OutErrCode = ErrorCodes::ERR_UNSUPPORTED_ASSET_EDITOR;
            OutErrMsg = TEXT("The preview is showing a bind pose with no animation loaded, so this ")
                        TEXT("subject has no time axis: every instant would photograph the same pose. ")
                        TEXT("Name an animation to give it one.");
            return false;
        }

        // Order matters and each line is load-bearing; see the file header.
        SingleNode->SetPlaying(false);
        SingleNode->SetPosition(static_cast<float>(TimeSeconds), /*bFireNotifies=*/false);
        Component.TickAnimation(0.0f, /*bNeedsValidRootMotion=*/false);
        // A null tick function forces the non-threaded path, which is what makes the evaluation
        // complete before this call returns instead of one frame later.
        Component.RefreshBoneTransforms(nullptr);
        // RefreshBoneTransforms evaluates into the editable component-space buffer. Finalize is
        // still required when the normal component tick is bypassed: it flips that buffer to the
        // render-readable one, otherwise pose evidence changes while the PNG remains bind pose.
        Component.FinalizeBoneTransform();
        return FlushPoseRenderState(Component, OutErrCode, OutErrMsg);
    }

    FSubjectTimeSetter MakeScrubTimeSetter(USkeletalMeshComponent& Component)
    {
        TWeakObjectPtr<USkeletalMeshComponent> WeakComponent(&Component);
        return [WeakComponent](double TimeSeconds, FString& OutErrCode, FString& OutErrMsg) -> bool
        {
            USkeletalMeshComponent* Live = WeakComponent.Get();
            if (!Live)
            {
                OutErrCode = ErrorCodes::ERR_PREVIEW_NOT_FOUND;
                OutErrMsg = TEXT("The preview component went away before the subject could be posed; ")
                            TEXT("the asset editor was probably closed mid-set.");
                return false;
            }
            return ScrubToTimeSeconds(*Live, TimeSeconds, OutErrCode, OutErrMsg);
        };
    }

    // ---------------------------------------------------------------------------------------
    // Preview instance state

    void FPreviewInstanceState::CaptureFrom(const UDebugSkelMeshComponent& Component)
    {
        *this = FPreviewInstanceState();
        bPreviewOn = Component.IsPreviewOn();
        if (UAnimSingleNodeInstance* Existing = Component.GetSingleNodeInstance())
        {
            bCaptured = true;
            Asset = Existing->GetAnimationAsset();
            Position = Existing->GetCurrentTime();
            bPlaying = Existing->IsPlaying();
            bLooping = Existing->IsLooping();
        }
    }

    void FPreviewInstanceState::RestoreTo(UDebugSkelMeshComponent& Component) const
    {
        UAnimationAsset* PreviousAsset = Asset.Get();
        Component.EnablePreview(bPreviewOn, PreviousAsset);
        if (!bCaptured)
        {
            return;
        }
        if (UAnimSingleNodeInstance* Restored = Component.GetSingleNodeInstance())
        {
            Restored->SetLooping(bLooping);
            Restored->SetPosition(Position, /*bFireNotifies=*/false);
            Restored->SetPlaying(bPlaying);
        }
    }

    // ---------------------------------------------------------------------------------------
    // Acquire

    bool AcquireAnimationSubject(const FSubjectRequest& Request, FResolvedSubject& OutSubject,
        FSubjectTimeSetter& OutTimeSetter, FString& OutErrCode, FString& OutErrMsg)
    {
        FAnimationSubjectFacts Facts;
        const bool bFactsResolved = ResolveAnimationSubjectFacts(Request, Facts, OutErrCode, OutErrMsg);

        // Installed on EVERY path, including the ones that return false below: a caller holding a
        // setter must never get silence from it. This one is replaced by the real scrubber once a
        // preview component exists.
        {
            const FString AssetPath = Request.AssetPath.IsEmpty() ? Request.AnimationPath : Request.AssetPath;
            OutTimeSetter = [AssetPath](double, FString& SetterErrCode, FString& SetterErrMsg) -> bool
            {
                SetterErrCode = ErrorCodes::ERR_PREVIEW_NOT_FOUND;
                SetterErrMsg = FString::Printf(
                    TEXT("The animation subject '%s' was never acquired, so there is no preview to pose ")
                    TEXT("at an instant."),
                    AssetPath.IsEmpty() ? TEXT("(unnamed)") : *AssetPath);
                return false;
            };
        }

        if (!bFactsResolved)
        {
            return false;
        }

        OutSubject.Kind = ESubjectKind::Animation;
        OutSubject.AssetPath = Request.AssetPath.IsEmpty() ? Request.AnimationPath : Request.AssetPath;
        OutSubject.BoundsOrigin = Facts.BoundsOrigin;
        OutSubject.BoundsRadius = Facts.BoundsRadius;
        OutSubject.BoundsSource = Facts.BoundsSource;
        OutSubject.CaptureSource = Facts.CaptureSource;
        OutSubject.bTimeSupported = Facts.bTimeSupported;
        OutSubject.TimeStartSeconds = Facts.TimeStartSeconds;
        OutSubject.TimeEndSeconds = Facts.TimeEndSeconds;
        // Scrubbing IS reproducible: the same position produces the same pose, with no simulation and
        // no tick-delta dependence. This is exactly the claim §4.2 refuses to make for Niagara, and
        // the reason it is safe to make here.
        OutSubject.bTimeReproducible = true;

        if (!GEditor)
        {
            OutErrCode = ErrorCodes::ERR_EDITOR_NOT_AVAILABLE;
            OutErrMsg = TEXT("Editor not available");
            return false;
        }

        // Installed BEFORE the editor is opened, so a failure anywhere below still reaches
        // CloseAssetEditor when the subject releases. bWasAlreadyOpen is measured here rather than
        // read back from the acquisition, because it has to be known even when the acquisition fails
        // — and after the open it is unknowable.
        TSharedRef<Internal::FAnimationReleaseState> ReleaseState =
            MakeShared<Internal::FAnimationReleaseState>();
        ReleaseState->Asset = Facts.EditorAsset;
        ReleaseState->bCloseAfterCapture = Request.bCloseAfterCapture;
        ReleaseState->bCloseRequestedExplicitly = Request.bCloseAfterCaptureProvided;
        ReleaseState->bWasAlreadyOpen = Internal::MeasureEditorWasAlreadyOpen(Facts.EditorAsset);
        OutSubject.ProviderState = ReleaseState;
        OutSubject.bEditorWasAlreadyOpen = ReleaseState->bWasAlreadyOpen;

        FAssetEditorViewportAcquisition Acquisition;
        if (!AcquireAssetEditorViewport(Facts.EditorAsset,
                PinWrightCaptureSubjectMesh::PersonaFamilyToolkitNames(),
                Acquisition, OutErrCode, OutErrMsg))
        {
            return false;
        }
        OutSubject.ViewportClient = Acquisition.ViewportClient;
        OutSubject.SceneViewport = Acquisition.SceneViewport;

        int32 PreviewComponentCount = 0;
        UDebugSkelMeshComponent* PreviewComponent =
            Acquisition.ViewportClient
                ? FindPreviewMeshComponent(*Acquisition.ViewportClient, Facts.PreviewMesh,
                    PreviewComponentCount)
                : nullptr;
        if (!PreviewComponent)
        {
            OutErrCode = ErrorCodes::ERR_SKELETAL_MESH_NOT_FOUND;
            OutErrMsg = FString::Printf(
                TEXT("The '%s' editor for '%s' has no preview mesh component, so the animation cannot ")
                TEXT("be posed. Set a preview mesh on the skeleton first."),
                *Acquisition.ToolkitName.ToString(), *OutSubject.AssetPath);
            return false;
        }

        // ---- Freeze the preview, saving everything this call touches.
        ReleaseState->PreviewComponent = PreviewComponent;
        ReleaseState->SavedPreview.CaptureFrom(*PreviewComponent);

        // ---- the subject-coverage seam ----
        //
        // Bound by the PROVIDER, like every other kind: which components can be hidden without
        // hiding the backdrop is provider knowledge, and a verb that kept a table of it gave every
        // unlisted kind a structurally absent `subjectCoverage`. The FLAG is saved rather than
        // IsVisible(), which folds in the parent chain and could restore a `true` the component
        // never had.
        ReleaseState->bSubjectWasVisible = PreviewComponent->GetVisibleFlag();
        ReleaseState->bSubjectVisibilityCaptured = true;
        {
            const TWeakObjectPtr<UDebugSkelMeshComponent> WeakVisibilityComponent = PreviewComponent;
            const bool bWasVisible = ReleaseState->bSubjectWasVisible;
            const FString VisibilityAssetPath = Request.AssetPath;
            OutSubject.VisibilitySetter = [WeakVisibilityComponent, bWasVisible, VisibilityAssetPath](
                bool bVisible, FString& OutVisErrCode, FString& OutVisErrMsg) -> bool
            {
                UDebugSkelMeshComponent* Live = WeakVisibilityComponent.Get();
                if (!Live)
                {
                    OutVisErrCode = ErrorCodes::ERR_PREVIEW_NOT_FOUND;
                    OutVisErrMsg = FString::Printf(
                        TEXT("The preview component for '%s' went away mid-capture, so its ")
                        TEXT("visibility could not be changed to measure subject coverage."),
                        *VisibilityAssetPath);
                    return false;
                }
                Live->SetVisibility(bVisible && bWasVisible, /*bPropagateToChildren=*/true);
                return true;
            };
        }

        // bEnable is unconditionally true: EnablePreview installs the single-node preview instance
        // and hands it the asset. Passing false instead would leave whatever instance the editor
        // already had running (an Animation Blueprint, say), which is not deterministic.
        PreviewComponent->EnablePreview(true, Facts.AnimationAsset);
        ReleaseState->bPreviewInstalled = true;

        UAnimSingleNodeInstance* SingleNode = PreviewComponent->GetSingleNodeInstance();
        if (!SingleNode)
        {
            OutErrCode = ErrorCodes::ERR_PREVIEW_NOT_FOUND;
            OutErrMsg = FString::Printf(
                TEXT("Preview component for '%s' has no single-node animation instance, so the ")
                TEXT("animation cannot be frozen at a given time and a burst would not be ")
                TEXT("deterministic."),
                *Facts.AnimationAsset->GetPathName());
            return false;
        }
        // Order matters: EnablePreview -> SetAnimationAsset restarts playback, so stopping has to
        // come after it and before any position is written. A playing preview would sit at a
        // different time in each of the Slate pumps a capture performs.
        SingleNode->SetPlaying(false);

        OutTimeSetter = MakeScrubTimeSetter(*PreviewComponent);
        return true;
    }

    // ---------------------------------------------------------------------------------------
    // Release

    void ReleaseAnimationSubject(FResolvedSubject& Subject)
    {
        if (!Subject.ProviderState.IsValid())
        {
            return;
        }
        // Consumed, so a second release is a no-op without needing a separate flag. The cast is safe
        // by pairing: this runs only on subjects this provider resolved.
        const TSharedPtr<Internal::FAnimationReleaseState> ReleaseState =
            StaticCastSharedPtr<Internal::FAnimationReleaseState>(Subject.ProviderState);
        Subject.ProviderState.Reset();
        if (!ReleaseState.IsValid())
        {
            return;
        }

        // FIRST, and independent of bPreviewInstalled: put the subject back on screen. The coverage
        // differential restores visibility itself on every normal path, but a set that failed
        // BETWEEN the hide and the show would otherwise leave the artist's tab holding an invisible
        // asset -- a far more expensive false symptom than the coverage number that failure already
        // costs. Writing the SAVED flag rather than true keeps a component that arrived hidden hidden.
        if (ReleaseState->bSubjectVisibilityCaptured)
        {
            if (UDebugSkelMeshComponent* PreviewComponent = ReleaseState->PreviewComponent.Get())
            {
                PreviewComponent->SetVisibility(ReleaseState->bSubjectWasVisible,
                    /*bPropagateToChildren=*/true);
            }
        }

        if (ReleaseState->bPreviewInstalled)
        {
            if (UDebugSkelMeshComponent* PreviewComponent = ReleaseState->PreviewComponent.Get())
            {
                ReleaseState->SavedPreview.RestoreTo(*PreviewComponent);
            }
        }

        // Dropped before the window closes, so a caller that kept the FResolvedSubject cannot read a
        // client that belongs to a destroyed asset editor.
        Subject.ViewportClient = nullptr;
        Subject.SceneViewport.Reset();

        UObject* Asset = ReleaseState->Asset.Get();
        if (!Asset)
        {
            return;
        }
        // The three-state rule and the read-back are both inside CloseAssetEditor, which returns the
        // MEASURED outcome: an asset editor can veto its own close, and a close reported but not
        // performed leaves the shutdown-crash precondition in place while the response says the
        // window is gone.
        Subject.bEditorClosed = PinWrightCaptureSubject::CloseAssetEditor(
            Asset,
            ReleaseState->bCloseAfterCapture,
            ReleaseState->bCloseRequestedExplicitly,
            ReleaseState->bWasAlreadyOpen);
    }

    namespace Internal
    {
        // Static-init registration, one file per kind, exactly the shape the dispatcher's own
        // REGISTER_RPC_HANDLER uses. Adding a kind creates a file and edits none.
        struct FAutoRegisterAnimationSubjectProvider
        {
            FAutoRegisterAnimationSubjectProvider()
            {
                FSubjectProvider Provider;
                Provider.Kind = ESubjectKind::Animation;
                Provider.Acquire = &AcquireAnimationSubject;
                Provider.Release = &ReleaseAnimationSubject;
                RegisterProvider(Provider);
            }
        };

        // `static`, matching REGISTER_RPC_HANDLER's own AutoReg object: the registration is a side
        // effect of construction, and the name is file-unique so Unity cannot merge two.
        static const FAutoRegisterAnimationSubjectProvider GAutoRegisterAnimationSubjectProvider;
    }
}
