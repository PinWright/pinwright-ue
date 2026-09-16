// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Containers/ArrayView.h"
#include "Handlers/Render/CaptureSubject.h"

class UAnimationAsset;
class UMeshComponent;
class UObject;
class USkeletalMesh;
class UStaticMesh;

// THE TWO MESH-ASSET SUBJECT KINDS: staticMesh and skeletalMesh.
//
// Both acquire an asset-editor preview viewport through the one walk in CaptureSubject.h, take
// bounds from the ASSET rather than from a posed component, and hand back a time setter. What
// separates them is the time axis: a Static Mesh has none and its setter refuses with a typed code
// naming exactly that (plan decision 6), while a Skeletal Mesh has one only when the request names
// an animation to load onto it.
//
// WHY THE FACTS STEP IS A SEPARATE, EXPORTED FUNCTION. ResolveMeshSubjectFacts decides everything
// that follows from the request alone — the asset class, the bounds, whether a time axis exists —
// and it opens nothing. Three things fall out of that split:
//   * A malformed request is refused before any editor state is mutated, which is the rule
//     Tests/Render/TestAnimationCaptureHandlers.cpp opens with.
//   * The bounds contract is testable on a host where no preview viewport can be realised --
//     a commandlet with no asset editor open, or a host whose GPU another process is holding.
//     RETRACTED 2026-08-21: this bullet used to justify itself with "FAdvancedPreviewScene is
//     not lit under UnrealEditor-Cmd -unattended and is bimodal across runs". That is false.
//     Commit bcc334e8 showed every reading behind it came from three captures overwriting ONE
//     file -- the auto-generated screenshot filename carries a one-second timestamp while a
//     capture takes ~60 ms -- and the fixture re-measures at meanLuminance 0.3644 both headless
//     and interactively. The split above stands on its own without that claim. See
//     docs/lessons.md (the retracted bullet, kept marked) and RenderHandler.cpp's
//     "WHAT THIS PARAGRAPH USED TO SAY, AND WHY IT IS GONE" comment.
//   * A caller that only needs to know whether a subject HAS a time axis pays for no window.
//
// BOUNDS COME FROM THE ASSET, ALWAYS. UStaticMesh::GetBounds() / USkeletalMesh::GetBounds() read
// the asset's own bounds, which do not move when the preview component is posed. Framing from the
// posed component would walk the camera under the subject between two instants of the same burst
// and destroy the comparison the burst exists to support (plan decision 4;
// AnimationPreviewCaptureHandler.cpp:42-44 records the same finding at its own call site).
namespace PinWrightCaptureSubjectMesh
{
    using PinWrightCaptureSubject::ESubjectKind;
    using PinWrightCaptureSubject::FResolvedSubject;
    using PinWrightCaptureSubject::FSubjectRequest;
    using PinWrightCaptureSubject::FSubjectTimeSetter;

    // `captureSource` for each kind, spelled exactly as the verbs already ship it:
    // RenderHandler.cpp:506 and AnimationPreviewCaptureHandler.cpp:903.
    inline constexpr TCHAR CaptureSourceStaticMeshEditor[] = TEXT("staticMeshEditorPreview");
    inline constexpr TCHAR CaptureSourcePersonaViewport[] = TEXT("personaPreviewViewport");
    // `boundsSource` for both kinds. The other four spellings belong to the other providers.
    inline constexpr TCHAR BoundsSourceAssetBounds[] = TEXT("assetBounds");

    // Toolkit names each kind will accept, for AcquireAssetEditorViewport's mandatory gate.
    TArrayView<const FName> StaticMeshToolkitNames();
    // The Persona family: SkeletalMeshEditor, AnimationEditor, SkeletonEditor,
    // AnimationBlueprintEditor. Matched by NAME rather than by casting to IHasPersonaToolkit,
    // because UE builds with RTTI off and a static downcast through those multiple-inheritance
    // hierarchies from IAssetEditorInstance* is not a safe thing to write.
    TArrayView<const FName> PersonaFamilyToolkitNames();
    bool IsPersonaFamilyToolkit(FName ToolkitName);

    // The typed no-time-axis refusal, so both the up-front rejection and the time setter spell it
    // identically. ERR_UNSUPPORTED_ASSET_EDITOR with a message naming the missing axis — never the
    // dispatcher's UNKNOWN_PARAMS, which tells the caller nothing (plan decision 6). SkeletonPath
    // is used only by the skeletalMesh wording and may be empty.
    bool RefuseNoTimeAxis(ESubjectKind Kind, const FString& AssetPath, const FString& SkeletonPath,
        FString& OutErrCode, FString& OutErrMsg);

    // Everything the request alone decides. Nothing here opens an asset editor, touches Slate, or
    // needs GEditor.
    struct FMeshSubjectFacts
    {
        UObject*         Asset = nullptr;
        UStaticMesh*     StaticMesh = nullptr;    // set for ESubjectKind::StaticMesh
        USkeletalMesh*   SkeletalMesh = nullptr;  // set for ESubjectKind::SkeletalMesh
        UAnimationAsset* AnimationAsset = nullptr;// set only when the request named one
        FString  SkeletonPath;                    // the mesh's skeleton, for the refusal wording

        FVector  BoundsOrigin = FVector::ZeroVector;
        double   BoundsRadius = 0.0;
        FString  BoundsSource;
        FString  CaptureSource;
        bool     bTimeSupported = false;
        double   TimeStartSeconds = 0.0;
        double   TimeEndSeconds = 0.0;
    };

    // Load the asset, gate its class, resolve any named animation, and measure the asset bounds.
    // Refuses with a typed code for: a missing path, a missing asset, a class the kind does not
    // serve, a missing animation, a skeleton mismatch, and — for a Static Mesh that was handed an
    // animation — the no-time-axis refusal above.
    bool ResolveMeshSubjectFacts(const FSubjectRequest& Request, FMeshSubjectFacts& Out,
        FString& OutErrCode, FString& OutErrMsg);

    // The registered Acquire for both kinds. Dispatches on Request.Kind; anything other than
    // StaticMesh / SkeletalMesh is refused rather than served, so a mis-registration is visible.
    bool AcquireMeshSubject(const FSubjectRequest& Request, FResolvedSubject& OutSubject,
        FSubjectTimeSetter& OutTimeSetter, FString& OutErrCode, FString& OutErrMsg);

    // The exact preview component acquired for this mesh subject. Valid until ReleaseMeshSubject;
    // null for non-mesh providers or an incomplete acquisition. Capture readiness uses it after
    // the draw to measure the rendered LOD and visible sections instead of guessing from the asset.
    UMeshComponent* GetResolvedMeshComponent(const FResolvedSubject& Subject);

    // The registered Release. Restores the Persona preview instance this call installed and applies
    // the three-state close rule through PinWrightCaptureSubject::CloseAssetEditor, writing the
    // MEASURED outcome into FResolvedSubject::bEditorClosed. Idempotent: it consumes ProviderState.
    void ReleaseMeshSubject(FResolvedSubject& Subject);
}
