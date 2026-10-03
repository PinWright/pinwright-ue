// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Handlers/Render/PreviewViewportCaptureUtils.h"

class AActor;
class UClass;

// render.capture_actor_preview: one actor, framed to its own bounds, as an exact-size PNG, without
// placing anything in (or dirtying) the user's level.
//
// TWO SOURCES, ONE RENDERER. An actor CLASS is spawned RF_Transient into a private FPreviewScene
// world that the call creates and destroys; an existing ACTOR (editor level or a PIE world) is
// drawn in its own world. Either way the pixels come from the same ownerless
// FSceneCaptureProbe render.capture_mesh uses, with PRM_UseShowOnlyList restricted to the one
// actor, so the frame is the subject under its world's lights and nothing else. The coverage
// reference is the same draw with an EMPTY show-only list - the subject is never hidden, moved or
// otherwise touched, which is what lets the existing-actor path stay a pure read.
//
// Exported as a production boundary so the GPU test exercises the code the handler runs.
namespace PinWrightActorPreviewCapture
{
    struct FActorCaptureRequest
    {
        // Exactly one of the two. The class must be a concrete AActor subclass.
        UClass* ActorClass = nullptr;
        AActor* Actor = nullptr;
        PinWrightRenderCapture::FViewportCaptureRequest Capture;
        bool bUseOrbitPose = true;
        bool bAutoFrameOrthoWidth = true;
        float OrbitAzimuth = 0.0f;
        float OrbitElevation = 20.0f;
        float Padding = 1.25f;
        bool bMeasureCoverage = true;
        bool bWriteFile = true;
    };

    struct FActorCaptureOutput
    {
        FString ActorClass;
        FString ActorName;
        FString ActorLabel;
        FString ActorPath;
        // "preview" for a spawned class, else the actor's world: "editor" | "pie" | "game" | "other".
        FString World;
        bool bSpawnedTransient = false;
        // Measured after teardown: the transient instance no longer resolves. Meaningful only when
        // bSpawnedTransient.
        bool bTransientDestroyed = false;
        FVector BoundsOrigin = FVector::ZeroVector;
        double BoundsRadius = 0.0;
        PinWrightRenderCapture::FViewportCaptureRequest ResolvedRequest;
        // Carries the `viewport` block's fields; bPreviewSceneCaptureIncomplete is the sky-light
        // drain of a spawned class's private world.
        PinWrightRenderCapture::FViewportCaptureOutput Capture;
        TOptional<double> SubjectCoverage;
        TArray<uint8> PngData;
    };

    // "editor" | "pie" | "preview" | "game" | "other" for the world an actor lives in.
    FString DescribeWorld(const UWorld* World);

    bool CaptureActorPreview(const FActorCaptureRequest& Request, FActorCaptureOutput& OutCapture,
        FString& OutErrCode, FString& OutErrMsg);
}
