// Copyright (c) 2026 Alexander Penkin. MIT License.

// Providers for the staticMesh and skeletalMesh subject kinds. Contract and rationale:
// CaptureSubjectProviders_Mesh.h.
//
// THE SHUTDOWN CRASH IS WHY RELEASE IS STRUCTURAL HERE TOO. An asset editor still open when the
// editor exits faults in ~FStaticMeshEditor (UE 5.8 StaticMeshEditor.cpp:271, docs/lessons.md:166).
// The per-call release state is installed BEFORE the editor is opened, so a failure anywhere after
// that point still reaches CloseAssetEditor when FResolvedSubject releases.

#include "Handlers/Render/CaptureSubjectProviders_Mesh.h"

#include "Handlers/ErrorCodes.h"
// FindPreviewMeshComponent: the ONE Persona preview-component walk. This file used to carry a
// third, count-free copy of it; see the note where that copy stood.
#include "Handlers/Render/CaptureSubjectProviders_Animation.h"

#include "Animation/AnimSequenceBase.h"
#include "Animation/AnimSingleNodeInstance.h"
#include "Animation/AnimationAsset.h"
#include "Animation/DebugSkelMeshComponent.h"
#include "Animation/Skeleton.h"
#include "Components/PrimitiveComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Editor.h"
#include "EditorViewportClient.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
// Back for the STATIC-mesh preview-component walk (FindStaticMeshPreviewComponent below). The
// Persona walk that these three once served moved to CaptureSubjectProviders_Animation.cpp and is
// still called from there rather than copied back; what returned is a different walk over a
// different component class, matched by asset identity.
#include "PreviewScene.h"
#include "UObject/UObjectIterator.h"
#include "Slate/SceneViewport.h"
#include "Subsystems/AssetEditorSubsystem.h"

// File-unique NAMED namespace, not an anonymous one: Unity merges translation units and the sibling
// Render/*.cpp files carry same-shaped helpers. The repo's convention for that collision is a named
// namespace per file.
namespace PinWrightCaptureSubjectMesh
{
    using namespace PinWrightCaptureSubject;

    namespace Internal
    {
        // Per-call state, type-erased into FResolvedSubject::ProviderState. Providers are
        // registered once at static init and the frozen FSubjectProvider::Release takes only
        // FResolvedSubject&, so this is the only place per-call state can live.
        struct FMeshReleaseState : public PinWrightCaptureSubject::FSubjectReleaseState
        {
            TWeakObjectPtr<UObject> Asset;
            // The three states, carried whole: absent closes only a window this call opened, an
            // explicit true closes one the caller already had open as well, false leaves it.
            // Dropping the distinction is what silently stopped the second capture of the same mesh
            // from cleaning up (RenderHandler.cpp:395-412).
            bool bCloseAfterCapture = true;
            bool bCloseRequestedExplicitly = false;
            bool bWasAlreadyOpen = false;

            // Everything this call changes on the Persona preview component, so it can be put back.
            // A review verb that leaves an artist's Persona tab scrubbed to a different frame with a
            // different animation loaded is not read-only in any sense that matters.
            TWeakObjectPtr<UDebugSkelMeshComponent> PreviewComponent;
            bool bPreviewInstalled = false;   // this call called EnablePreview and must undo it
            bool bPreviewOn = false;          // was the component in single-node preview mode at all
            bool bSingleNodeCaptured = false; // was there a single-node instance to read
            TWeakObjectPtr<UAnimationAsset> PreviousAnimation;
            float PreviousPosition = 0.0f;
            bool bPreviousPlaying = false;
            bool bPreviousLooping = true;

            // The ONE component the subject-coverage differential hides and shows again, plus the
            // visibility flag it carried when this call found it. Restored on release as well as
            // by the setter: a set that fails between the hide and the show would otherwise leave
            // an artist's tab holding an invisible asset, and "the mesh vanished" is a far more
            // expensive false symptom than a missing coverage number.
            TWeakObjectPtr<UPrimitiveComponent> SubjectComponent;
            bool bSubjectVisibilityCaptured = false;
            bool bSubjectWasVisible = true;
        };

        // ---- THE PREVIEW COMPONENT THAT IS THE SUBJECT, MATCHED BY ASSET IDENTITY -------------
        //
        // NEVER "the first UStaticMeshComponent in the preview world". FAdvancedPreviewScene's
        // BACKDROP is made of static mesh components too - the floor plane and the sky sphere
        // (AdvancedPreviewScene.cpp:63-93) - and hiding one of those would put backdrop pixels
        // into the coverage differential and report a subject that is not there. Matching on the
        // asset the caller named is what makes the hidden thing provably the subject.
        //
        // Returns null rather than guessing when nothing matches: an unbound visibility setter
        // yields an ABSENT `subjectCoverage`, which is the honest answer, while a guessed
        // component yields a number that is wrong in a direction nobody can see.
        UStaticMeshComponent* FindStaticMeshPreviewComponent(FEditorViewportClient& ViewportClient,
            UStaticMesh* PreferredMesh)
        {
            FPreviewScene* PreviewScene = ViewportClient.GetPreviewScene();
            UWorld* PreviewWorld = PreviewScene ? PreviewScene->GetWorld() : nullptr;
            if (!PreviewWorld || !PreferredMesh)
            {
                return nullptr;
            }
            for (TObjectIterator<UStaticMeshComponent> It; It; ++It)
            {
                UStaticMeshComponent* Component = *It;
                // TObjectIterator also visits class default objects, and every other Static Mesh
                // editor in the process owns a preview component in its own preview world.
                if (!Component || Component->IsTemplate() || Component->GetWorld() != PreviewWorld)
                {
                    continue;
                }
                if (Component->GetStaticMesh() == PreferredMesh)
                {
                    return Component;
                }
            }
            return nullptr;
        }

        // Install the show/hide seam for a subject whose preview component has been identified.
        // A null component leaves the setter unbound, which reads as "this kind could not be
        // measured" rather than as a coverage of zero.
        void BindSubjectVisibility(FResolvedSubject& OutSubject, FMeshReleaseState& ReleaseState,
            UPrimitiveComponent* Component)
        {
            if (!Component)
            {
                return;
            }
            // The FLAG, not IsVisible(): IsVisible() folds in the parent chain, so restoring from
            // it could write a `true` the component never had.
            ReleaseState.SubjectComponent = Component;
            ReleaseState.bSubjectWasVisible = Component->GetVisibleFlag();
            ReleaseState.bSubjectVisibilityCaptured = true;

            const TWeakObjectPtr<UPrimitiveComponent> WeakComponent = Component;
            const bool bWasVisible = ReleaseState.bSubjectWasVisible;
            const FString AssetPath = OutSubject.AssetPath;
            OutSubject.VisibilitySetter = [WeakComponent, bWasVisible, AssetPath](
                bool bVisible, FString& OutErrCode, FString& OutErrMsg) -> bool
            {
                UPrimitiveComponent* Live = WeakComponent.Get();
                if (!Live)
                {
                    OutErrCode = ErrorCodes::ERR_PREVIEW_NOT_FOUND;
                    OutErrMsg = FString::Printf(
                        TEXT("The preview component for '%s' went away mid-capture, so its ")
                        TEXT("visibility could not be changed to measure subject coverage."),
                        *AssetPath);
                    return false;
                }
                // Restoring writes back what the component HAD, never a hardcoded true: a preview
                // component that arrived hidden must not be revealed by the act of measuring it.
                // Propagated to children because a preview mesh's own attached components draw
                // with it, and pixels left behind by a partial hide are counted as backdrop.
                Live->SetVisibility(bVisible && bWasVisible, /*bPropagateToChildren=*/true);
                return true;
            };
        }

        // The preview-component lookup that used to live here is now
        // PinWrightCaptureSubjectAnimation::FindPreviewMeshComponent. This was the THIRD copy of
        // one walk; the other two had already collapsed onto that one. It differed from the
        // survivor in exactly two ways, neither of which changes which component is returned:
        //   * it returned at the first mesh match instead of finishing the walk, and
        //   * it reported no candidate count.
        // For every input the two agree -- first preferred match if there is one, otherwise the
        // first candidate in the same world, otherwise null -- because the survivor only ever
        // takes the FIRST preferred match it sees (`!PreferredMatch` guards the assignment) and
        // seeds FirstMatch from the first candidate regardless. Finishing the walk is what buys
        // the count, which Persona needs because it explicitly anticipates more than one preview
        // component (FAnimationEditorPreviewScene::GetAllPreviewMeshComponents).

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
    // Toolkit gates

    TArrayView<const FName> StaticMeshToolkitNames()
    {
        // FStaticMeshEditor::GetToolkitFName() (Editor/StaticMeshEditor/Private/StaticMeshEditor.cpp).
        static const FName Names[] = { FName(TEXT("StaticMeshEditor")) };
        return TArrayView<const FName>(Names, UE_ARRAY_COUNT(Names));
    }

    TArrayView<const FName> PersonaFamilyToolkitNames()
    {
        static const FName Names[] = {
            FName(TEXT("SkeletalMeshEditor")),
            FName(TEXT("AnimationEditor")),
            FName(TEXT("SkeletonEditor")),
            FName(TEXT("AnimationBlueprintEditor"))
        };
        return TArrayView<const FName>(Names, UE_ARRAY_COUNT(Names));
    }

    bool IsPersonaFamilyToolkit(FName ToolkitName)
    {
        return PersonaFamilyToolkitNames().Contains(ToolkitName);
    }

    // ---------------------------------------------------------------------------------------
    // The typed no-time-axis refusal

    bool RefuseNoTimeAxis(ESubjectKind Kind, const FString& AssetPath, const FString& SkeletonPath,
        FString& OutErrCode, FString& OutErrMsg)
    {
        OutErrCode = ErrorCodes::ERR_UNSUPPORTED_ASSET_EDITOR;
        if (Kind == ESubjectKind::SkeletalMesh)
        {
            OutErrMsg = FString::Printf(
                TEXT("Skeletal Mesh '%s' was resolved without an animation, so it has no time axis to ")
                TEXT("scrub and can only be captured in its bind pose. Pass subject.animation naming an ")
                TEXT("animation asset bound to skeleton '%s' to give this subject a time axis."),
                *AssetPath,
                SkeletonPath.IsEmpty() ? TEXT("(none)") : *SkeletonPath);
            return false;
        }
        OutErrMsg = FString::Printf(
            TEXT("A Static Mesh has no time axis: '%s' holds no animation and cannot be posed at an ")
            TEXT("instant, so a time series over it is not a thing that exists. Capture it as a single ")
            TEXT("still, or name a subject that carries a time axis — a skeletalMesh with an ")
            TEXT("animation, an animation asset, or a placed actor driven by a Level Sequence."),
            *AssetPath);
        return false;
    }

    // ---------------------------------------------------------------------------------------
    // What the request alone decides

    bool ResolveMeshSubjectFacts(const FSubjectRequest& Request, FMeshSubjectFacts& Out,
        FString& OutErrCode, FString& OutErrMsg)
    {
        if (Request.Kind != ESubjectKind::StaticMesh && Request.Kind != ESubjectKind::SkeletalMesh)
        {
            OutErrCode = ErrorCodes::ERR_INVALID_ARGUMENT;
            OutErrMsg = FString::Printf(
                TEXT("subject.kind '%s' is not served by the mesh providers, which handle "
                     "'staticMesh' and 'skeletalMesh' only."),
                ToWireName(Request.Kind));
            return false;
        }
        if (Request.AssetPath.IsEmpty())
        {
            OutErrCode = ErrorCodes::ERR_INVALID_ARGUMENT;
            OutErrMsg = FString::Printf(TEXT("subject.path is required for subject.kind '%s'"),
                ToWireName(Request.Kind));
            return false;
        }

        Out.Asset = LoadObject<UObject>(nullptr, *Request.AssetPath);
        if (!Out.Asset)
        {
            OutErrCode = ErrorCodes::ERR_ASSET_NOT_FOUND;
            OutErrMsg = FString::Printf(TEXT("Asset not found: %s"), *Request.AssetPath);
            return false;
        }

        if (Request.Kind == ESubjectKind::StaticMesh)
        {
            Out.StaticMesh = Cast<UStaticMesh>(Out.Asset);
            if (!Out.StaticMesh)
            {
                OutErrCode = ErrorCodes::ERR_UNSUPPORTED_ASSET_EDITOR;
                OutErrMsg = FString::Printf(
                    TEXT("subject.kind 'staticMesh' takes a Static Mesh ('%s' is a %s). Use ")
                    TEXT("subject.kind 'skeletalMesh' for a skinned mesh or 'animation' for an ")
                    TEXT("animation asset."),
                    *Request.AssetPath, *Out.Asset->GetClass()->GetName());
                return false;
            }

            // Refused BEFORE anything is opened. Naming an animation is how a caller asks a subject
            // for a time axis, and on a Static Mesh that is wrong from the request alone — so the
            // caller gets the typed answer without a window being opened, focused and closed first.
            if (!Request.AnimationPath.IsEmpty())
            {
                return RefuseNoTimeAxis(ESubjectKind::StaticMesh, Request.AssetPath, FString(),
                    OutErrCode, OutErrMsg);
            }

            // `subject.radius` is deliberately NOT consulted: §2.1 of the plan scopes radius to the
            // world/point kinds, and an asset kind's extent is the asset's own bounds. Honouring it
            // here would report a radius the mesh does not have.
            const FBoxSphereBounds AssetBounds = Out.StaticMesh->GetBounds();
            Out.BoundsOrigin = AssetBounds.Origin;
            Out.BoundsRadius = static_cast<double>(AssetBounds.SphereRadius);
            Out.BoundsSource = BoundsSourceAssetBounds;
            Out.CaptureSource = CaptureSourceStaticMeshEditor;
            Out.bTimeSupported = false;
            Out.TimeStartSeconds = 0.0;
            Out.TimeEndSeconds = 0.0;
            return true;
        }

        Out.SkeletalMesh = Cast<USkeletalMesh>(Out.Asset);
        if (!Out.SkeletalMesh)
        {
            OutErrCode = ErrorCodes::ERR_UNSUPPORTED_ASSET_EDITOR;
            OutErrMsg = FString::Printf(
                TEXT("subject.kind 'skeletalMesh' takes a Skeletal Mesh ('%s' is a %s). Use ")
                TEXT("subject.kind 'animation' for an animation asset — it resolves its own preview ")
                TEXT("mesh — or 'staticMesh' for a Static Mesh."),
                *Request.AssetPath, *Out.Asset->GetClass()->GetName());
            return false;
        }
        if (const USkeleton* MeshSkeleton = Out.SkeletalMesh->GetSkeleton())
        {
            Out.SkeletonPath = MeshSkeleton->GetPathName();
        }

        // The animation is optional, and it is what decides whether this subject has a time axis.
        if (!Request.AnimationPath.IsEmpty())
        {
            Out.AnimationAsset = LoadObject<UAnimationAsset>(nullptr, *Request.AnimationPath);
            if (!Out.AnimationAsset)
            {
                OutErrCode = ErrorCodes::ERR_ANIMATION_NOT_FOUND;
                OutErrMsg = FString::Printf(TEXT("Animation asset not found: %s"), *Request.AnimationPath);
                return false;
            }
            // A mismatched pairing produces a mesh posed by a skeleton it was not bound to — limbs
            // in the wrong places, and a picture that reads as a rigging defect. Refuse rather than
            // photograph it.
            if (Out.AnimationAsset->GetSkeleton() && Out.SkeletalMesh->GetSkeleton() &&
                Out.AnimationAsset->GetSkeleton() != Out.SkeletalMesh->GetSkeleton())
            {
                OutErrCode = ErrorCodes::ERR_SKELETON_MISMATCH;
                OutErrMsg = FString::Printf(
                    TEXT("Animation '%s' targets skeleton '%s' but mesh '%s' is bound to '%s'."),
                    *Out.AnimationAsset->GetPathName(),
                    *Out.AnimationAsset->GetSkeleton()->GetPathName(),
                    *Out.SkeletalMesh->GetPathName(),
                    *Out.SkeletalMesh->GetSkeleton()->GetPathName());
                return false;
            }
        }

        const FBoxSphereBounds AssetBounds = Out.SkeletalMesh->GetBounds();
        Out.BoundsOrigin = AssetBounds.Origin;
        Out.BoundsRadius = static_cast<double>(AssetBounds.SphereRadius);
        Out.BoundsSource = BoundsSourceAssetBounds;
        Out.CaptureSource = CaptureSourcePersonaViewport;
        Out.bTimeSupported = Out.AnimationAsset != nullptr;
        Out.TimeStartSeconds = 0.0;
        const UAnimSequenceBase* SequenceBase = Cast<UAnimSequenceBase>(Out.AnimationAsset);
        Out.TimeEndSeconds = SequenceBase ? static_cast<double>(SequenceBase->GetPlayLength()) : 0.0;
        return true;
    }

    // ---------------------------------------------------------------------------------------
    // Acquire

    bool AcquireMeshSubject(const FSubjectRequest& Request, FResolvedSubject& OutSubject,
        FSubjectTimeSetter& OutTimeSetter, FString& OutErrCode, FString& OutErrMsg)
    {
        FMeshSubjectFacts Facts;
        const bool bFactsResolved = ResolveMeshSubjectFacts(Request, Facts, OutErrCode, OutErrMsg);

        // Installed on EVERY path, including the ones that return false below: a caller holding a
        // setter must never get silence from it. Resolve() would install its own generic refusal if
        // this were left empty, but the kind-specific wording is what tells the caller which
        // argument would supply an axis.
        {
            const ESubjectKind Kind = Request.Kind;
            const FString AssetPath = Request.AssetPath;
            const FString SkeletonPath = Facts.SkeletonPath;
            OutTimeSetter = [Kind, AssetPath, SkeletonPath](double,
                FString& SetterErrCode, FString& SetterErrMsg) -> bool
            {
                return RefuseNoTimeAxis(Kind, AssetPath, SkeletonPath, SetterErrCode, SetterErrMsg);
            };
        }

        if (!bFactsResolved)
        {
            return false;
        }

        OutSubject.Kind = Request.Kind;
        OutSubject.AssetPath = Request.AssetPath;
        OutSubject.BoundsOrigin = Facts.BoundsOrigin;
        OutSubject.BoundsRadius = Facts.BoundsRadius;
        OutSubject.BoundsSource = Facts.BoundsSource;
        OutSubject.CaptureSource = Facts.CaptureSource;
        OutSubject.bTimeSupported = Facts.bTimeSupported;
        OutSubject.TimeStartSeconds = Facts.TimeStartSeconds;
        OutSubject.TimeEndSeconds = Facts.TimeEndSeconds;
        // A mesh pose is a pure function of the instant asked for, so two captures at the same time
        // are the same picture. Contrast the Niagara kind, which is hardcoded false because its
        // simulation is seeded from FMath::Rand() on every reset (plan §4.2).
        OutSubject.bTimeReproducible = true;

        if (!GEditor)
        {
            OutErrCode = ErrorCodes::ERR_EDITOR_NOT_AVAILABLE;
            OutErrMsg = TEXT("Editor not available");
            return false;
        }

        // Installed BEFORE the editor is opened, so a failure anywhere below still reaches
        // CloseAssetEditor when the subject releases. bWasAlreadyOpen is measured here rather than
        // read back from the acquisition, because it has to be known even when the acquisition
        // fails — and after the open it is unknowable.
        TSharedRef<Internal::FMeshReleaseState> ReleaseState = MakeShared<Internal::FMeshReleaseState>();
        ReleaseState->Asset = Facts.Asset;
        ReleaseState->bCloseAfterCapture = Request.bCloseAfterCapture;
        ReleaseState->bCloseRequestedExplicitly = Request.bCloseAfterCaptureProvided;
        ReleaseState->bWasAlreadyOpen = Internal::MeasureEditorWasAlreadyOpen(Facts.Asset);
        OutSubject.ProviderState = ReleaseState;
        OutSubject.bEditorWasAlreadyOpen = ReleaseState->bWasAlreadyOpen;

        const TArrayView<const FName> AcceptedToolkits = (Request.Kind == ESubjectKind::StaticMesh)
            ? StaticMeshToolkitNames()
            : PersonaFamilyToolkitNames();

        FAssetEditorViewportAcquisition Acquisition;
        if (!AcquireAssetEditorViewport(Facts.Asset, AcceptedToolkits, Acquisition,
            OutErrCode, OutErrMsg))
        {
            return false;
        }
        OutSubject.ViewportClient = Acquisition.ViewportClient;
        OutSubject.SceneViewport = Acquisition.SceneViewport;

        if (Request.Kind == ESubjectKind::StaticMesh)
        {
            // Nothing to POSE and no time axis -- the refusing time setter installed above stands
            // -- but there IS something to hide: the preview component carrying this asset.
            //
            // WHAT THIS REPLACES, AND WHY THE OLD REASONING WAS WRONG. The coverage differential
            // was bound for the Niagara kind alone, on the argument that "a Static Mesh IS the
            // preview scene's only content and cannot be hidden without hiding the thing the
            // capture is of". Hiding the thing being captured is exactly what a REFERENCE frame
            // is: the differential wants the same pose WITHOUT the subject so it can subtract it.
            // The preview scene keeps its floor, sky and lights while the mesh is hidden, so the
            // reference is a backdrop-only frame and what changed between the two IS the mesh.
            //
            // The cost of that mistake was measured on this project the day this was written: a
            // capture came back uniform colour with no subject in frame while reporting
            // blank:false, crushed:null, litPixelFraction:1.0 and boundsInFrame:true. Every
            // published health signal read green -- boundsInFrame because the bounds genuinely
            // were in frame, litPixelFraction because the backdrop genuinely was lit -- and the
            // one field that answers "is the subject in this picture" was structurally absent for
            // this kind. Two agents spent hours on it with the image in front of them.
            Internal::BindSubjectVisibility(OutSubject, *ReleaseState,
                Acquisition.ViewportClient
                    ? Internal::FindStaticMeshPreviewComponent(*Acquisition.ViewportClient,
                          Facts.StaticMesh)
                    : nullptr);
            return true;
        }

        // Discarded, and named so that is visible: this kind photographs whichever preview
        // component the walk prefers and has no response field to publish a candidate count in.
        // render.capture_animation_preview does publish it, which is why the shared function
        // reports it rather than hiding it.
        int32 UnusedPreviewComponentCount = 0;
        UDebugSkelMeshComponent* PreviewComponent =
            Acquisition.ViewportClient
                ? PinWrightCaptureSubjectAnimation::FindPreviewMeshComponent(
                      *Acquisition.ViewportClient, Facts.SkeletalMesh, UnusedPreviewComponentCount)
                : nullptr;
        if (!PreviewComponent)
        {
            OutErrCode = ErrorCodes::ERR_SKELETAL_MESH_NOT_FOUND;
            OutErrMsg = FString::Printf(
                TEXT("The '%s' editor for '%s' has no preview mesh component, so the subject cannot ")
                TEXT("be posed. Set a preview mesh on the skeleton first."),
                *Acquisition.ToolkitName.ToString(), *Request.AssetPath);
            return false;
        }

        // ---- Freeze the preview, saving everything this call touches.
        ReleaseState->PreviewComponent = PreviewComponent;
        ReleaseState->bPreviewOn = PreviewComponent->IsPreviewOn();
        // Same seam as the Static Mesh branch above, and for the same reason: a skinned subject is
        // no less capable of being absent from its own frame. The component found here is the one
        // the walk already preferred by asset, so the hidden thing is provably the subject rather
        // than some other Persona preview in the process.
        Internal::BindSubjectVisibility(OutSubject, *ReleaseState, PreviewComponent);
        if (UAnimSingleNodeInstance* Existing = PreviewComponent->GetSingleNodeInstance())
        {
            ReleaseState->bSingleNodeCaptured = true;
            ReleaseState->PreviousAnimation = Existing->GetAnimationAsset();
            ReleaseState->PreviousPosition = Existing->GetCurrentTime();
            ReleaseState->bPreviousPlaying = Existing->IsPlaying();
            ReleaseState->bPreviousLooping = Existing->IsLooping();
        }

        // bEnable is unconditionally true, including when no animation was named: EnablePreview
        // installs the single-node preview instance and hands it the asset, and a NULL asset is how
        // the engine's own preview shows the reference pose. Passing false instead would leave
        // whatever instance the editor already had running (an Animation Blueprint, say), which is
        // neither the bind pose nor deterministic.
        PreviewComponent->EnablePreview(true, Facts.AnimationAsset);
        ReleaseState->bPreviewInstalled = true;

        UAnimSingleNodeInstance* SingleNode = PreviewComponent->GetSingleNodeInstance();
        if (Facts.AnimationAsset && !SingleNode)
        {
            OutErrCode = ErrorCodes::ERR_PREVIEW_NOT_FOUND;
            OutErrMsg = FString::Printf(
                TEXT("Preview component for '%s' has no single-node animation instance, so the ")
                TEXT("animation cannot be frozen at a given time and a burst would not be ")
                TEXT("deterministic."),
                *Request.AssetPath);
            return false;
        }
        if (SingleNode)
        {
            // Order matters: EnablePreview -> SetAnimationAsset restarts playback, so stopping has
            // to come after it and before any position is written. A playing preview would sit at a
            // different time in each of the Slate pumps a capture performs, and a burst would
            // photograph three unrelated instants per shot.
            SingleNode->SetPlaying(false);
        }

        if (Facts.AnimationAsset)
        {
            const TWeakObjectPtr<UDebugSkelMeshComponent> WeakComponent = PreviewComponent;
            const FString AssetPath = Request.AssetPath;
            OutTimeSetter = [WeakComponent, AssetPath](double TimeSeconds,
                FString& SetterErrCode, FString& SetterErrMsg) -> bool
            {
                UDebugSkelMeshComponent* Component = WeakComponent.Get();
                UAnimSingleNodeInstance* Instance = Component ? Component->GetSingleNodeInstance() : nullptr;
                if (!Component || !Instance)
                {
                    SetterErrCode = ErrorCodes::ERR_PREVIEW_NOT_FOUND;
                    SetterErrMsg = FString::Printf(
                        TEXT("The preview component for '%s' went away before the subject could be ")
                        TEXT("posed; the asset editor was probably closed mid-set."),
                        *AssetPath);
                    return false;
                }
                // SetPosition only writes the clock. These two turn it into a pose: TickAnimation
                // runs the anim instance at the new time, and RefreshBoneTransforms(nullptr)
                // evaluates the skeleton synchronously on the game thread — a null tick function is
                // what forces the non-threaded path.
                Instance->SetPosition(static_cast<float>(TimeSeconds), /*bFireNotifies=*/false);
                Component->TickAnimation(0.0f, /*bNeedsValidRootMotion=*/false);
                Component->RefreshBoneTransforms(nullptr);
                // RefreshBoneTransforms evaluates into the editable component-space buffer.
                // Finalize flips it to the render-readable buffer when the normal component tick
                // is bypassed, keeping the rendered pose consistent with pose evidence.
                Component->FinalizeBoneTransform();
                Component->MarkRenderDynamicDataDirty();
                return true;
            };
        }

        return true;
    }

    UMeshComponent* GetResolvedMeshComponent(const FResolvedSubject& Subject)
    {
        if (!Subject.ProviderState.IsValid() ||
            (Subject.Kind != ESubjectKind::StaticMesh &&
             Subject.Kind != ESubjectKind::SkeletalMesh))
        {
            return nullptr;
        }
        const TSharedPtr<Internal::FMeshReleaseState> ReleaseState =
            StaticCastSharedPtr<Internal::FMeshReleaseState>(Subject.ProviderState);
        return ReleaseState.IsValid()
            ? Cast<UMeshComponent>(ReleaseState->SubjectComponent.Get())
            : nullptr;
    }

    // ---------------------------------------------------------------------------------------
    // Release

    void ReleaseMeshSubject(FResolvedSubject& Subject)
    {
        if (!Subject.ProviderState.IsValid())
        {
            return;
        }
        // Consumed, so a second release is a no-op without needing a separate flag. The cast is
        // safe by pairing: this runs only on subjects the mesh providers resolved.
        const TSharedPtr<Internal::FMeshReleaseState> ReleaseState =
            StaticCastSharedPtr<Internal::FMeshReleaseState>(Subject.ProviderState);
        Subject.ProviderState.Reset();
        if (!ReleaseState.IsValid())
        {
            return;
        }

        // FIRST, and unconditionally: put the subject back on screen. The coverage differential
        // restores visibility itself on every normal path, but a set that failed BETWEEN the hide
        // and the show would otherwise leave the artist's tab holding an invisible asset -- a far
        // more expensive false symptom than the missing coverage number that failure already
        // costs. Writing the SAVED flag rather than true keeps a component that arrived hidden
        // hidden.
        if (ReleaseState->bSubjectVisibilityCaptured)
        {
            if (UPrimitiveComponent* SubjectComponent = ReleaseState->SubjectComponent.Get())
            {
                SubjectComponent->SetVisibility(ReleaseState->bSubjectWasVisible,
                    /*bPropagateToChildren=*/true);
            }
        }

        if (UDebugSkelMeshComponent* PreviewComponent = ReleaseState->PreviewComponent.Get())
        {
            if (ReleaseState->bPreviewInstalled)
            {
                // Unconditional, not only when a previous single-node instance was captured: this
                // call turned preview mode ON, so a component that had none must be put back to
                // having none — otherwise an Animation Blueprint editor is left showing a reference
                // pose where the artist's graph used to be running.
                PreviewComponent->EnablePreview(ReleaseState->bPreviewOn,
                    ReleaseState->PreviousAnimation.Get());
                if (ReleaseState->bSingleNodeCaptured)
                {
                    if (UAnimSingleNodeInstance* Restored = PreviewComponent->GetSingleNodeInstance())
                    {
                        Restored->SetLooping(ReleaseState->bPreviousLooping);
                        Restored->SetPosition(ReleaseState->PreviousPosition, /*bFireNotifies=*/false);
                        Restored->SetPlaying(ReleaseState->bPreviousPlaying);
                    }
                }
            }
        }

        // Dropped before the window closes, so a caller that kept the FResolvedSubject cannot read
        // a client that belongs to a destroyed asset editor.
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
        //
        // EXPECT FALSE ON THE NORMAL CLOSE PATH, and it is not a regression. CloseAssetEditor no
        // longer destroys the toolkit here - running that destructor chain from this release faults
        // with an access violation and takes the editor down (this exact call site is one of the
        // two crash frames on board B-capture-asset-preview-no-safe-close-mode). It queues the
        // close onto the next core-ticker pass instead, so the window really is still open as this
        // returns. `assetEditorCloseDeferred` in the response is what says so.
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
        struct FAutoRegisterMeshSubjectProviders
        {
            FAutoRegisterMeshSubjectProviders()
            {
                RegisterKind(ESubjectKind::StaticMesh);
                RegisterKind(ESubjectKind::SkeletalMesh);
            }

            static void RegisterKind(ESubjectKind Kind)
            {
                PinWrightCaptureSubject::FSubjectProvider Provider;
                Provider.Kind = Kind;
                Provider.Acquire = &AcquireMeshSubject;
                Provider.Release = &ReleaseMeshSubject;
                PinWrightCaptureSubject::RegisterProvider(Provider);
            }
        };

        // `static`, matching REGISTER_RPC_HANDLER's own AutoReg object: the registration is a side
        // effect of construction, and the name is file-unique so Unity cannot merge two.
        static const FAutoRegisterMeshSubjectProviders GAutoRegisterMeshSubjectProviders;
    }
}
