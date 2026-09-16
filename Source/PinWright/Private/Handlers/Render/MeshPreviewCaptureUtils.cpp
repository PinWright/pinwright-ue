// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Handlers/Render/MeshPreviewCaptureUtils.h"

#include "Handlers/ErrorCodes.h"
#include "Handlers/Render/CameraShotPlanUtils.h"
#include "Handlers/Render/OrthoTileCaptureUtils.h"
#include "Handlers/Render/PreviewSceneRig.h"
#include "Handlers/Render/SceneCaptureProbeUtils.h"
#include "Handlers/Render/ViewModeVocabulary.h"
#include "Utils/AssetUtils.h"
#include "Utils/ScreenshotUtils.h"

#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "Compat/EngineVersionCompat.h"
#include "HAL/IConsoleManager.h"
#include "Misc/FileHelper.h"
#include "PreviewScene.h"
#include "SkinnedAssetCompiler.h"
#include "ShowFlags.h"
#include "StaticMeshCompiler.h"
#include "UObject/Package.h"

namespace PinWrightMeshPreviewCapture
{
namespace
{
    FPreviewScene::ConstructionValues MakeMeshPreviewSceneValues()
    {
        FPreviewScene::ConstructionValues Values;
        Values.SetCreatePhysicsScene(false).ShouldSimulatePhysics(false).SetTransactional(false);
        return Values;
    }

    void ConfigureMeshPreviewOutput(const FMeshCaptureRequest& Request,
        const PinWrightSceneCaptureProbe::FColorCaptureRequest& ColorRequest,
        const PinWrightSceneCaptureProbe::FColorCaptureMetadata& Metadata,
        PinWrightRenderCapture::FViewportCaptureOutput& Out)
    {
        const EViewModeIndex Mode = Request.Capture.ViewMode.bRequested
            ? Request.Capture.ViewMode.ViewMode : VMI_Lit;
        Out.Width = ColorRequest.Width;
        Out.Height = ColorRequest.Height;
        Out.RenderWidth = ColorRequest.Width;
        Out.RenderHeight = ColorRequest.Height;
        Out.RenderPrimaryResolutionFraction = 1.0;
        Out.RenderSecondaryResolutionFraction = 1.0;
        Out.RenderResolutionFraction = 1.0;
        Out.RenderScreenPercentage = 100;
        Out.RenderAntiAliasingMethod = TEXT("None");
        Out.RenderAntiAliasingMethodValue = 0;
        Out.bRenderResolutionPinned = true;
        Out.bTemporalAntiAliasingSuppressed = true;
        Out.Renderer = TEXT("sceneCapture2D");
        Out.ViewportType = TEXT("TransientPreviewScene");
        Out.ViewMode = PinWrightViewModes::GetDisplayName(Mode);
        Out.ViewModeKey = PinWrightRenderCapture::GetViewModeKey(Mode);
        Out.ViewModeValue = static_cast<int32>(Mode);
        Out.bViewModeOverrideRequested = Request.Capture.ViewMode.bRequested;
        Out.ViewModeRequestedKey = Request.Capture.ViewMode.Key;
        Out.ViewModeRequestedValue = static_cast<int32>(Request.Capture.ViewMode.ViewMode);
        Out.bViewModeApplied = Metadata.bViewModeApplied;
        Out.ViewModeShowFlags = Request.Capture.ViewMode.DistinguishingShowFlags;
        Out.ViewModeShowFlagMismatches = Metadata.ViewModeShowFlagMismatches;
        Out.bViewModeRestored = true;
        Out.bLitViewMode = PinWrightRenderCapture::IsLitViewMode(Mode);
        Out.bGameView = true;
        Out.EffectiveLocation = ColorRequest.Location;
        Out.EffectiveRotation = ColorRequest.Rotation;
        Out.bCameraAimApplied = true;
        Out.ExposureMode = Request.Capture.Exposure.Mode;
        Out.bExposurePinRequested = Request.Capture.Exposure.WantsPin();
        Out.bExposurePinned = Metadata.bExposurePinned;
        Out.Ev100Requested = Request.Capture.Exposure.Ev100;
        Out.bExposureFixedApplied = Metadata.bExposurePinned;
        Out.Ev100Applied = Metadata.ExposureEv100Applied;
        Out.bExposureRestored = true;
        Out.bPreviewSceneRigRequested = Request.Capture.PreviewSceneRig.WantsRig();
        Out.bSharedProfilesRestored = true;
        Out.bConfigFileUnchanged = true;
        Out.bWarmupMeasured = false;
    }
}

struct FMeshCaptureSession::FImpl
{
    FImpl()
        : PreviewScene(MakeMeshPreviewSceneValues())
        , Probe(PreviewScene.GetWorld())
    {
    }

    FPreviewScene PreviewScene;
    TUniquePtr<PinWrightPreviewSceneRig::FScopedPreviewSceneRig> Rig;
    PinWrightSceneCaptureProbe::FSceneCaptureProbe Probe;
    UMeshComponent* MeshComponent = nullptr;
    FBoxSphereBounds Bounds;
    FString AssetPath;
    FString AssetClass;
};

FMeshCaptureSession::FMeshCaptureSession(TUniquePtr<FImpl>&& InImpl)
    : Impl(MoveTemp(InImpl))
{
}

FMeshCaptureSession::~FMeshCaptureSession()
{
    check(IsInGameThread());
    IConsoleVariable* ForceGc = IConsoleManager::Get().FindConsoleVariable(
        TEXT("r.ForceGCOnPreviewSceneExit"));
    if (ForceGc)
    {
        // FPreviewScene otherwise schedules a process-wide full-purge GC from its destructor.
        // render.capture_mesh is tick-unsafe and handlers execute serially on the game thread, so
        // no other RPC or preview-scene destructor can overlap this exact destruction scope.
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 8, 0)
        const EConsoleVariableFlags Priority = static_cast<EConsoleVariableFlags>(
            ForceGc->GetFlags() & ECVF_SetByMask);
        TGuardConsoleVariable<int32> DisablePreviewSceneGc(ForceGc, 0, Priority);
#elif UE_VERSION_NEWER_THAN_OR_EQUAL(5, 7, 0)
        // The guard took no priority before UE 5.8; it sets and restores at ECVF_SetByCode, which
        // outranks this cvar's registration priority, so the suppression still lands.
        TGuardConsoleVariable<int32> DisablePreviewSceneGc(ForceGc, 0);
#elif UE_VERSION_NEWER_THAN_OR_EQUAL(5, 6, 0)
        // On UE 5.6 the guard has no IConsoleVariable* constructor either, only the by-name
        // one - which resolves to the very variable this scope already found.
        TGuardConsoleVariable<int32> DisablePreviewSceneGc(
            TEXT("r.ForceGCOnPreviewSceneExit"), 0);
#else
        // TGuardConsoleVariable itself arrived in UE 5.6 (HAL/IConsoleManager.h). Save and
        // restore by hand at ECVF_SetByCode - the same value and priority the later guard uses.
        struct FScopedForceGcSuppression
        {
            explicit FScopedForceGcSuppression(IConsoleVariable* InVariable)
                : Variable(InVariable), PreviousValue(InVariable->GetInt())
            {
                Variable->Set(0, ECVF_SetByCode);
            }
            ~FScopedForceGcSuppression() { Variable->Set(PreviousValue, ECVF_SetByCode); }

            IConsoleVariable* Variable;
            int32 PreviousValue;
        };
        FScopedForceGcSuppression DisablePreviewSceneGc(ForceGc);
#endif
        Impl.Reset();
        return;
    }
    Impl.Reset();
}

TUniquePtr<FMeshCaptureSession> FMeshCaptureSession::Create(const FString& AssetPath,
    const PinWrightPreviewSceneRig::FPreviewSceneRigPin& RigPin,
    FString& OutErrCode, FString& OutErrMsg, float SubjectScale)
{
    OutErrCode.Reset();
    OutErrMsg.Reset();
    if (!IsInGameThread())
    {
        OutErrCode = ErrorCodes::ERR_SCENE_CAPTURE_FAILED;
        OutErrMsg = TEXT("Mesh preview capture must run on the game thread");
        return nullptr;
    }
    if (!FMath::IsFinite(SubjectScale) || SubjectScale <= 0.0f)
    {
        OutErrCode = ErrorCodes::ERR_INVALID_ARGUMENT;
        OutErrMsg = TEXT("subject scale must be finite and greater than zero");
        return nullptr;
    }

    const FResolvedAsset Resolved = ResolveAsset(AssetPath, /*bLoadObject=*/true);
    if (!Resolved.Object)
    {
        OutErrCode = ErrorCodes::ERR_ASSET_NOT_FOUND;
        OutErrMsg = Resolved.ErrorMessage.IsEmpty()
            ? FString::Printf(TEXT("Mesh asset not found: %s"), *AssetPath)
            : Resolved.ErrorMessage;
        return nullptr;
    }

    UStaticMesh* StaticMesh = Cast<UStaticMesh>(Resolved.Object);
    USkeletalMesh* SkeletalMesh = Cast<USkeletalMesh>(Resolved.Object);
    if (!StaticMesh && !SkeletalMesh)
    {
        OutErrCode = ErrorCodes::ERR_UNSUPPORTED_ASSET_CLASS;
        OutErrMsg = FString::Printf(
            TEXT("render.capture_mesh supports UStaticMesh and USkeletalMesh assets; %s is %s"),
            *AssetPath, *Resolved.Object->GetClass()->GetName());
        return nullptr;
    }

    UMeshComponent* MeshComponent = nullptr;
    FBoxSphereBounds MeshBounds(ForceInit);
    FString AssetClass;
    if (StaticMesh)
    {
        FStaticMeshCompilingManager::Get().FinishCompilation({ StaticMesh });
        UStaticMeshComponent* Component = NewObject<UStaticMeshComponent>(
            GetTransientPackage(), NAME_None, RF_Transient);
        if (Component)
        {
            Component->SetStaticMesh(StaticMesh);
        }
        MeshComponent = Component;
        MeshBounds = StaticMesh->GetBounds();
        AssetClass = TEXT("StaticMesh");
    }
    else
    {
        FSkinnedAssetCompilingManager::Get().FinishCompilation({ SkeletalMesh });
        USkeletalMeshComponent* Component = NewObject<USkeletalMeshComponent>(
            GetTransientPackage(), NAME_None, RF_Transient);
        if (Component)
        {
            Component->SetSkeletalMeshAsset(SkeletalMesh);
        }
        MeshComponent = Component;
        MeshBounds = SkeletalMesh->GetBounds();
        AssetClass = TEXT("SkeletalMesh");
    }
    if (!MeshComponent)
    {
        OutErrCode = ErrorCodes::ERR_CREATE_FAILED;
        OutErrMsg = TEXT("Could not allocate the transient mesh component");
        return nullptr;
    }

    const FTransform SubjectTransform(FQuat::Identity, FVector::ZeroVector,
        FVector(SubjectScale));
    MeshBounds = MeshBounds.TransformBy(SubjectTransform);
    if (MeshBounds.ContainsNaN() || MeshBounds.SphereRadius <= 0.0)
    {
        OutErrCode = ErrorCodes::ERR_BOUNDS_EMPTY;
        OutErrMsg = FString::Printf(
            TEXT("Mesh '%s' reports no finite, non-empty bounds after compilation"),
            *Resolved.Object->GetPathName());
        return nullptr;
    }

    // From this point onward the session owns the preview scene. Every failure therefore reaches
    // FMeshCaptureSession's scoped r.ForceGCOnPreviewSceneExit suppression before destruction.
    TUniquePtr<FMeshCaptureSession> Session(
        new FMeshCaptureSession(MakeUnique<FImpl>()));
    FImpl& NewImpl = *Session->Impl;
    if (!NewImpl.Probe.IsValid())
    {
        OutErrCode = ErrorCodes::ERR_SCENE_CAPTURE_FAILED;
        OutErrMsg = TEXT("Could not initialise the transient scene capture component");
        return nullptr;
    }

    NewImpl.AssetPath = Resolved.Object->GetPathName();
    NewImpl.AssetClass = MoveTemp(AssetClass);
    NewImpl.MeshComponent = MeshComponent;
    NewImpl.Bounds = MeshBounds;
    NewImpl.MeshComponent->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    NewImpl.PreviewScene.AddComponent(NewImpl.MeshComponent, SubjectTransform);
    const float BoundsRadius = FMath::Max(
        static_cast<float>(NewImpl.Bounds.SphereRadius), 1.0f);
    NewImpl.Rig = MakeUnique<PinWrightPreviewSceneRig::FScopedPreviewSceneRig>(
        NewImpl.PreviewScene, NewImpl.Bounds.Origin, BoundsRadius, RigPin,
        OutErrCode, OutErrMsg);
    if (!NewImpl.Rig->IsValid())
    {
        return nullptr;
    }
    return Session;
}

bool FMeshCaptureSession::Capture(const FMeshCaptureRequest& Request,
    FMeshCaptureOutput& OutCapture, FString& OutErrCode, FString& OutErrMsg)
{
    OutCapture = FMeshCaptureOutput();
    OutErrCode.Reset();
    OutErrMsg.Reset();
    if (!Impl)
    {
        OutErrCode = ErrorCodes::ERR_SCENE_CAPTURE_FAILED;
        OutErrMsg = TEXT("Mesh capture session is not initialised");
        return false;
    }
    OutCapture.AssetPath = Impl->AssetPath;
    OutCapture.AssetClass = Impl->AssetClass;

    const float BoundsRadius = FMath::Max(static_cast<float>(Impl->Bounds.SphereRadius), 1.0f);
    PinWrightSceneCaptureProbe::FColorCaptureRequest ColorRequest;
    ColorRequest.Width = Request.Capture.Width;
    ColorRequest.Height = Request.Capture.Height;
    ColorRequest.Fov = Request.Capture.Fov;
    ColorRequest.bOrthographic = Request.Capture.ProjectionMode == TEXT("orthographic");
    ColorRequest.OrthoWidth = Request.Capture.OrthoWidth;
    if (ColorRequest.bOrthographic && Request.bAutoFrameOrthoWidth)
    {
        ColorRequest.OrthoWidth = PinWrightCameraFrame::ComputeOrthoWorldWidth(
            BoundsRadius, Request.Padding, ColorRequest.Width, ColorRequest.Height);
    }
    ColorRequest.bFixedExposure = Request.Capture.Exposure.WantsPin();
    ColorRequest.ExposureEv100 = Request.Capture.Exposure.Ev100;
    ColorRequest.bViewModeRequested = Request.Capture.ViewMode.bRequested;
    ColorRequest.ViewMode = Request.Capture.ViewMode.ViewMode;
    ColorRequest.ViewModeKey = Request.Capture.ViewMode.Key;
    ColorRequest.DistinguishingShowFlags = Request.Capture.ViewMode.DistinguishingShowFlags;
    ColorRequest.Location = Request.Capture.Location;
    ColorRequest.Rotation = Request.Capture.Rotation;
    if (Request.bUseOrbitPose)
    {
        const float Distance = PinWrightCameraFrame::ComputeFitDistance(
            BoundsRadius, ColorRequest.Fov, Request.Padding);
        PinWrightCameraFrame::PlaceOrbitCamera(Impl->Bounds.Origin, Request.OrbitAzimuth,
            Request.OrbitElevation, Distance, ColorRequest.Location, ColorRequest.Rotation);
    }
    const float BoundsCenterDepth = static_cast<float>(FVector::DotProduct(
        Impl->Bounds.Origin - ColorRequest.Location, ColorRequest.Rotation.Vector()));
    ColorRequest.NearClippingPlane = FMath::Max(
        1.0f, BoundsCenterDepth - BoundsRadius * 1.1f);
    OutCapture.ResolvedRequest = Request.Capture;
    OutCapture.ResolvedRequest.Location = ColorRequest.Location;
    OutCapture.ResolvedRequest.Rotation = ColorRequest.Rotation;
    OutCapture.ResolvedRequest.OrthoWidth = ColorRequest.OrthoWidth;

    PinWrightSceneCaptureProbe::FColorCaptureMetadata Metadata;
    if (!Impl->Probe.CaptureColor(ColorRequest, OutCapture.Capture.Pixels, Metadata,
        OutErrCode, OutErrMsg))
    {
        return false;
    }
    if (Request.bMeasureCoverage)
    {
        Impl->MeshComponent->SetVisibility(false, true);
        TArray<FColor> ReferencePixels;
        PinWrightSceneCaptureProbe::FColorCaptureMetadata ReferenceMetadata;
        const bool bReferenceCaptured = Impl->Probe.CaptureColor(
            ColorRequest, ReferencePixels, ReferenceMetadata, OutErrCode, OutErrMsg);
        Impl->MeshComponent->SetVisibility(true, true);
        if (!bReferenceCaptured)
        {
            return false;
        }
        const PinWrightFlatRegion::FFrameDifferenceStats Difference =
            PinWrightFlatRegion::MeasureFrameDifference(
                ReferencePixels, OutCapture.Capture.Pixels, 8);
        if (Difference.bMeasured)
        {
            OutCapture.SubjectCoverage = Difference.ChangedPixelFraction;
        }
    }

    const FEngineShowFlags* DrawnShowFlags = Impl->Probe.GetShowFlags();
    FEngineShowFlags FallbackShowFlags = PinWrightOrthoTiles::MakeBaseCaptureShowFlags();
    OutCapture.Capture.PreviewSceneRigBefore = Impl->Rig->GetPreviousRig();
    OutCapture.Capture.PreviewSceneRigDrawn = Impl->Rig->Measure(
        DrawnShowFlags ? *DrawnShowFlags : FallbackShowFlags);
    OutCapture.Capture.bPreviewSceneCaptureUpdated = Impl->Rig->CaptureContentsUpdated();
    OutCapture.Capture.bPreviewSceneCaptureIncomplete = Impl->Rig->CaptureContentsIncomplete();
    if (Request.Capture.PreviewSceneRig.WantsRig())
    {
        OutCapture.Capture.bPreviewSceneRigApplied =
            PinWrightPreviewSceneRig::RigMatchesRequest(
                Request.Capture.PreviewSceneRig, OutCapture.Capture.PreviewSceneRigDrawn);
    }

    ConfigureMeshPreviewOutput(Request, ColorRequest, Metadata, OutCapture.Capture);
    OutCapture.Capture.ImageStats = PinWrightRenderCapture::CalculateCaptureImageStats(
        OutCapture.Capture.Pixels);
    OutCapture.FlatRegion = PinWrightFlatRegion::MeasureLargestFlatRegion(
        OutCapture.Capture.Pixels, ColorRequest.Width, ColorRequest.Height);
    if (!PinWrightScreenshotUtils::EncodeBitmapToPng(ColorRequest.Width, ColorRequest.Height,
        OutCapture.Capture.Pixels, OutCapture.PngData))
    {
        OutErrCode = ErrorCodes::ERR_WRITE_FAILED;
        OutErrMsg = TEXT("Could not encode the transient mesh capture as PNG");
        return false;
    }

    OutCapture.Capture.SizeBytes = OutCapture.PngData.Num();
    if (Request.bWriteFile)
    {
        OutCapture.Capture.Path = PinWrightScreenshotUtils::MakeScreenshotOutputPath(
            Request.Capture.Filename, TEXT("MeshCapture"), TEXT("MeshCapture"),
            OutCapture.Capture.Filename);
        if (!FFileHelper::SaveArrayToFile(OutCapture.PngData, *OutCapture.Capture.Path))
        {
            OutErrCode = ErrorCodes::ERR_SAVE_FAILED;
            OutErrMsg = FString::Printf(TEXT("Could not write PNG: %s"),
                *OutCapture.Capture.Path);
            return false;
        }
        OutCapture.PngData.Reset();
    }
    if (!Request.Capture.bRetainPixels)
    {
        OutCapture.Capture.Pixels.Reset();
    }
    return true;
}

bool CaptureMeshToPng(const FMeshCaptureRequest& Request, FMeshCaptureOutput& OutCapture,
    FString& OutErrCode, FString& OutErrMsg)
{
    TUniquePtr<FMeshCaptureSession> Session = FMeshCaptureSession::Create(
        Request.AssetPath, Request.Capture.PreviewSceneRig, OutErrCode, OutErrMsg,
        Request.SubjectScale);
    const bool bCaptured = Session
        && Session->Capture(Request, OutCapture, OutErrCode, OutErrMsg);
    Session.Reset();
    OutCapture.Capture.bPreviewSceneRigDisposable = bCaptured;
    return bCaptured;
}
}
