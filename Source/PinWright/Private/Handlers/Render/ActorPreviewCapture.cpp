// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Handlers/Render/ActorPreviewCapture.h"

#include "Handlers/ErrorCodes.h"
#include "Handlers/Render/CameraShotPlanUtils.h"
#include "Handlers/Render/FlatRegionStats.h"
#include "Handlers/Render/MeshPreviewCaptureUtils.h"
#include "Handlers/Render/PreviewSceneRig.h"
#include "Handlers/Render/SceneCaptureProbeUtils.h"
#include "Utils/PackageDirtyUtils.h"
#include "Utils/ScreenshotUtils.h"

#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "HAL/IConsoleManager.h"
#include "Misc/FileHelper.h"
#include "Misc/ScopeExit.h"
#include "PreviewScene.h"

namespace PinWrightActorPreviewCapture
{
namespace ActorPreviewCaptureLocal
{
    // FPreviewScene's destructor requests a process-wide full-purge GC when
    // r.ForceGCOnPreviewSceneExit is set. Suppressed for the destruction only.
    // ponytail: same 15 lines as BlueprintPreviewHandler.cpp's FScopedPreviewSceneGcSuppression;
    // hoist both into a shared header if a third copy appears.
    struct FScopedPreviewSceneGcSuppression
    {
        IConsoleVariable* Variable =
            IConsoleManager::Get().FindConsoleVariable(TEXT("r.ForceGCOnPreviewSceneExit"));
        int32 Previous = Variable ? Variable->GetInt() : 0;

        FScopedPreviewSceneGcSuppression()
        {
            if (Variable)
            {
                Variable->Set(0, ECVF_SetByCode);
            }
        }
        ~FScopedPreviewSceneGcSuppression()
        {
            if (Variable)
            {
                Variable->Set(Previous, ECVF_SetByCode);
            }
        }
    };

    bool CaptureInWorld(UWorld* World, AActor* Actor, const FActorCaptureRequest& Request,
        FActorCaptureOutput& Out, FString& OutErrCode, FString& OutErrMsg)
    {
        const FBox Box = Actor->GetComponentsBoundingBox(/*bNonColliding=*/true,
            /*bIncludeFromChildActors=*/true);
        const FBoxSphereBounds Bounds(Box);
        if (!Box.IsValid || Bounds.ContainsNaN() || Bounds.SphereRadius <= 0.0)
        {
            OutErrCode = ErrorCodes::ERR_BOUNDS_EMPTY;
            OutErrMsg = FString::Printf(
                TEXT("Actor '%s' (%s) has no finite, non-empty component bounds to frame; it has no ")
                TEXT("renderable primitive."), *Actor->GetName(), *Actor->GetClass()->GetName());
            return false;
        }
        Out.BoundsOrigin = Bounds.Origin;
        Out.BoundsRadius = Bounds.SphereRadius;

        PinWrightSceneCaptureProbe::FSceneCaptureProbe Probe(World);
        if (!Probe.IsValid())
        {
            OutErrCode = ErrorCodes::ERR_SCENE_CAPTURE_FAILED;
            OutErrMsg = TEXT("Could not initialise the transient scene capture component");
            return false;
        }

        const float Radius = FMath::Max(static_cast<float>(Bounds.SphereRadius), 1.0f);
        PinWrightSceneCaptureProbe::FColorCaptureRequest Color;
        Color.Width = Request.Capture.Width;
        Color.Height = Request.Capture.Height;
        Color.Fov = Request.Capture.Fov;
        Color.bOrthographic = Request.Capture.ProjectionMode == TEXT("orthographic");
        Color.OrthoWidth = Request.Capture.OrthoWidth;
        if (Color.bOrthographic && Request.bAutoFrameOrthoWidth)
        {
            Color.OrthoWidth = PinWrightCameraFrame::ComputeOrthoWorldWidth(
                Radius, Request.Padding, Color.Width, Color.Height);
        }
        Color.bFixedExposure = Request.Capture.Exposure.WantsPin();
        Color.ExposureEv100 = Request.Capture.Exposure.Ev100;
        Color.bViewModeRequested = Request.Capture.ViewMode.bRequested;
        Color.ViewMode = Request.Capture.ViewMode.ViewMode;
        Color.ViewModeKey = Request.Capture.ViewMode.Key;
        Color.DistinguishingShowFlags = Request.Capture.ViewMode.DistinguishingShowFlags;
        Color.Location = Request.Capture.Location;
        Color.Rotation = Request.Capture.Rotation;
        if (Request.bUseOrbitPose)
        {
            PinWrightCameraFrame::PlaceOrbitCamera(Bounds.Origin, Request.OrbitAzimuth,
                Request.OrbitElevation,
                PinWrightCameraFrame::ComputeFitDistance(Radius, Color.Fov, Request.Padding),
                Color.Location, Color.Rotation);
        }
        const float CenterDepth = static_cast<float>(FVector::DotProduct(
            Bounds.Origin - Color.Location, Color.Rotation.Vector()));
        Color.NearClippingPlane = FMath::Max(1.0f, CenterDepth - Radius * 1.1f);
        Color.bShowOnlyActors = true;
        // The engine expands a show-only actor through its own components only, so a Child Actor
        // Component's actor (framed above via bIncludeFromChildActors) must be listed itself.
        Color.ShowOnlyActors = { Actor };
        Actor->GetAllChildActors(Color.ShowOnlyActors, /*bIncludeDescendants=*/true);

        Out.ResolvedRequest = Request.Capture;
        Out.ResolvedRequest.Location = Color.Location;
        Out.ResolvedRequest.Rotation = Color.Rotation;
        Out.ResolvedRequest.OrthoWidth = Color.OrthoWidth;

        PinWrightSceneCaptureProbe::FColorCaptureMetadata Metadata;
        if (!Probe.CaptureColor(Color, Out.Capture.Pixels, Metadata, OutErrCode, OutErrMsg))
        {
            return false;
        }
        // The same `viewport` fields render.capture_mesh publishes for this probe, so a view mode
        // that did not take (or left show flags wrong) is reported, not returned as a clean frame.
        PinWrightMeshPreviewCapture::ConfigureMeshPreviewOutput(
            Request.Capture, Color, Metadata, Out.Capture);
        Out.Capture.ViewportType = Out.bSpawnedTransient
            ? TEXT("TransientPreviewScene") : TEXT("ActorWorld");

        if (Request.bMeasureCoverage)
        {
            // Same lights, same camera, no primitive at all: the pixels that differ ARE the actor.
            PinWrightSceneCaptureProbe::FColorCaptureRequest Reference = Color;
            Reference.ShowOnlyActors.Reset();
            TArray<FColor> ReferencePixels;
            PinWrightSceneCaptureProbe::FColorCaptureMetadata ReferenceMetadata;
            if (!Probe.CaptureColor(Reference, ReferencePixels, ReferenceMetadata,
                    OutErrCode, OutErrMsg))
            {
                return false;
            }
            const PinWrightFlatRegion::FFrameDifferenceStats Difference =
                PinWrightFlatRegion::MeasureFrameDifference(ReferencePixels, Out.Capture.Pixels, 8);
            if (Difference.bMeasured)
            {
                Out.SubjectCoverage = Difference.ChangedPixelFraction;
            }
        }

        Out.Capture.ImageStats = PinWrightRenderCapture::CalculateCaptureImageStats(Out.Capture.Pixels);
        if (!PinWrightScreenshotUtils::EncodeBitmapToPng(Color.Width, Color.Height,
                Out.Capture.Pixels, Out.PngData))
        {
            OutErrCode = ErrorCodes::ERR_WRITE_FAILED;
            OutErrMsg = TEXT("Could not encode the actor capture as PNG");
            return false;
        }
        Out.Capture.SizeBytes = Out.PngData.Num();
        if (Request.bWriteFile)
        {
            Out.Capture.Path = PinWrightScreenshotUtils::MakeScreenshotOutputPath(
                Request.Capture.Filename, TEXT("ActorPreview"), TEXT("ActorPreview"),
                Out.Capture.Filename);
            if (!FFileHelper::SaveArrayToFile(Out.PngData, *Out.Capture.Path))
            {
                OutErrCode = ErrorCodes::ERR_SAVE_FAILED;
                OutErrMsg = FString::Printf(TEXT("Could not write PNG: %s"), *Out.Capture.Path);
                return false;
            }
            Out.PngData.Reset();
        }
        if (!Request.Capture.bRetainPixels)
        {
            Out.Capture.Pixels.Reset();
        }
        return true;
    }

    void DescribeActor(const AActor* Actor, FActorCaptureOutput& Out)
    {
        Out.ActorClass = Actor->GetClass()->GetPathName();
        Out.ActorName = Actor->GetName();
        Out.ActorLabel = Actor->GetActorLabel();
        Out.ActorPath = Actor->GetPathName();
    }
}

FString DescribeWorld(const UWorld* World)
{
    if (!World)
    {
        return TEXT("other");
    }
    switch (World->WorldType)
    {
    case EWorldType::Editor:        return TEXT("editor");
    case EWorldType::PIE:           return TEXT("pie");
    case EWorldType::EditorPreview: return TEXT("preview");
    case EWorldType::Game:          return TEXT("game");
    default:                        return TEXT("other");
    }
}

bool CaptureActorPreview(const FActorCaptureRequest& Request, FActorCaptureOutput& OutCapture,
    FString& OutErrCode, FString& OutErrMsg)
{
    using namespace ActorPreviewCaptureLocal;
    OutCapture = FActorCaptureOutput();
    OutErrCode.Reset();
    OutErrMsg.Reset();
    check(IsInGameThread());

    if ((Request.ActorClass == nullptr) == (Request.Actor == nullptr))
    {
        OutErrCode = ErrorCodes::ERR_INVALID_ARGUMENT;
        OutErrMsg = TEXT("Pass exactly one of an actor class (spawned transient) or an existing actor.");
        return false;
    }

    if (AActor* Existing = Request.Actor)
    {
        UWorld* World = Existing->GetWorld();
        if (!World)
        {
            OutErrCode = ErrorCodes::ERR_ACTOR_NOT_FOUND;
            OutErrMsg = FString::Printf(TEXT("Actor '%s' is not in a world."), *Existing->GetName());
            return false;
        }
        DescribeActor(Existing, OutCapture);
        OutCapture.World = DescribeWorld(World);
        // A read: whatever the probe's registration does to the level, the map's dirty flag is
        // left exactly as it was found (rpc-design.md §11).
        PinWright::PackageDirty::FScopedPackageDirtyRestore DirtyRestore;
        DirtyRestore.Capture(Existing);
        DirtyRestore.Capture(World->PersistentLevel);
        return CaptureInWorld(World, Existing, Request, OutCapture, OutErrCode, OutErrMsg);
    }

    UClass* Class = Request.ActorClass;
    if (!Class->IsChildOf(AActor::StaticClass())
        || Class->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists))
    {
        OutErrCode = ErrorCodes::ERR_CLASS_NOT_INSTANTIABLE;
        OutErrMsg = FString::Printf(TEXT("'%s' is not a concrete Actor class."), *Class->GetPathName());
        return false;
    }

    FPreviewScene::ConstructionValues Values;
    Values.SetCreatePhysicsScene(false).ShouldSimulatePhysics(false).SetTransactional(false);
    TUniquePtr<FPreviewScene> Scene = MakeUnique<FPreviewScene>(Values);
    UWorld* World = Scene->GetWorld();
    AActor* Spawned = nullptr;
    TWeakObjectPtr<AActor> WeakSpawned;
    // EVERY exit destroys the instance and then the private world, and the response's
    // `transientDestroyed` is read off the weak handle afterwards rather than assumed.
    ON_SCOPE_EXIT
    {
        if (IsValid(Spawned) && World)
        {
            World->EditorDestroyActor(Spawned, /*bShouldModifyLevel=*/false);
        }
        Spawned = nullptr;
        {
            FScopedPreviewSceneGcSuppression SuppressGc;
            Scene.Reset();
        }
        OutCapture.bTransientDestroyed = OutCapture.bSpawnedTransient && !WeakSpawned.IsValid();
    };
    if (!World)
    {
        OutErrCode = ErrorCodes::ERR_ACTOR_SPAWN_FAILED;
        OutErrMsg = TEXT("Could not create the private preview world.");
        return false;
    }

    FActorSpawnParameters Params;
    Params.bTemporaryEditorActor = true;
    Params.bHideFromSceneOutliner = true;
    Params.bNoFail = true;
    // Transient and not transactional: never saved, never dirties a package, never in undo.
    Params.ObjectFlags = RF_Transient;
    Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
    Params.OverrideLevel = World->PersistentLevel;
    const FTransform SpawnTransform = FTransform::Identity;
    Spawned = World->SpawnActor(Class, &SpawnTransform, Params);
    WeakSpawned = Spawned;
    if (!Spawned)
    {
        OutErrCode = ErrorCodes::ERR_ACTOR_SPAWN_FAILED;
        OutErrMsg = FString::Printf(TEXT("SpawnActor returned null for %s."), *Class->GetPathName());
        return false;
    }
    OutCapture.bSpawnedTransient = true;
    DescribeActor(Spawned, OutCapture);
    OutCapture.World = TEXT("preview");

    // The sky light only recaptures inside an editor tick; a capture runs none (PreviewSceneRig.h).
    PinWrightPreviewSceneRig::UpdatePreviewSceneCaptures(
        Scene.Get(), OutCapture.Capture.bPreviewSceneCaptureIncomplete);
    return CaptureInWorld(World, Spawned, Request, OutCapture, OutErrCode, OutErrMsg);
}
}
