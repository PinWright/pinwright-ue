// Copyright (c) 2026 Alexander Penkin. MIT License.

// ZFightingHandler.cpp — render.detect_z_fighting.
//
// The one capture verb whose deliverable is a NUMBER rather than a picture. See
// ZFightingAnalysis.h for what the test measures and, just as importantly, what it cannot
// see; see SceneCaptureProbeUtils.h for why it renders offscreen instead of through the
// Level Editor viewport every other capture verb drives.

#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/ParamSpec.h"
#include "Handlers/ErrorCodes.h"
#include "Handlers/Render/CaptureRendererNames.h"
#include "Handlers/Render/PreviewViewportCaptureUtils.h"
#include "Handlers/Render/SceneCaptureProbeUtils.h"
#include "Handlers/Render/ZFightingAnalysis.h"
#include "Utils/JsonUtils.h"
#include "Utils/ScreenshotUtils.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "EditorViewportClient.h"
#include "Engine/Engine.h"
#include "Engine/RendererSettings.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "HAL/FileManager.h"
#include "IAssetViewport.h"
#include "LevelEditor.h"
#include "Misc/FileHelper.h"
#include "Modules/ModuleManager.h"

// Names from PinWrightZFighting are qualified throughout rather than pulled in with a
// using-directive: this module builds with Unity enabled, where a file-scope using-directive
// leaks into every translation unit merged after it in the same blob.
namespace
{
    constexpr int32 MaxNamedActorsPerRegion = 4;

    struct FActorBoundsEntry
    {
        FString Name;
        FString Label;
        FBox Bounds = FBox(ForceInit);
        double Volume = 0.0;
    };

    void CollectActorBounds(UWorld* World, TArray<FActorBoundsEntry>& OutEntries)
    {
        OutEntries.Reset();
        if (!World)
        {
            return;
        }
        for (TActorIterator<AActor> It(World); It; ++It)
        {
            AActor* Actor = *It;
            if (!Actor)
            {
                continue;
            }
            FVector Origin = FVector::ZeroVector;
            FVector Extent = FVector::ZeroVector;
            // bOnlyCollidingComponents=false: this asks "what is drawn here", and a
            // collisionless mesh is exactly the kind of thing that ends up coplanar with
            // something else. Restricting to colliding components would omit the actors most
            // likely to be responsible.
            Actor->GetActorBounds(false, Origin, Extent);
            if (Extent.IsNearlyZero())
            {
                continue;
            }
            FActorBoundsEntry Entry;
            Entry.Name = Actor->GetName();
            Entry.Label = Actor->GetActorLabel();
            Entry.Bounds = FBox(Origin - Extent, Origin + Extent);
            Entry.Volume = Entry.Bounds.GetVolume();
            OutEntries.Add(MoveTemp(Entry));
        }
    }

    // Actors whose render bounds contain the point, smallest first. Bounds containment rather
    // than a line trace on purpose: a trace resolves against COLLISION, and a mesh with no
    // collision — a very common half of a z-fighting pair — is invisible to it, so a trace
    // would confidently name the wrong actor. Bounds over-report instead, which is the safe
    // direction for a hint field.
    void NameActorsAtPoint(const TArray<FActorBoundsEntry>& Entries, const FVector& Point,
        TArray<FString>& OutNames)
    {
        OutNames.Reset();
        TArray<const FActorBoundsEntry*> Matches;
        for (const FActorBoundsEntry& Entry : Entries)
        {
            if (Entry.Bounds.ExpandBy(2.0).IsInsideOrOn(Point))
            {
                Matches.Add(&Entry);
            }
        }
        Matches.Sort([](const FActorBoundsEntry& Lhs, const FActorBoundsEntry& Rhs)
        {
            return Lhs.Volume < Rhs.Volume;
        });
        const int32 Keep = FMath::Min(Matches.Num(), MaxNamedActorsPerRegion);
        for (int32 Index = 0; Index < Keep; ++Index)
        {
            OutNames.Add(Matches[Index]->Label.IsEmpty() ? Matches[Index]->Name : Matches[Index]->Label);
        }
    }

    // The affected pixel nearest the region centroid. A region is often a ring or an arc, so
    // its centroid can land on a clean pixel whose depth belongs to a different surface
    // entirely; sampling depth there would report a world position nowhere near the seam.
    bool FindRepresentativePixel(const TArray<uint8>& Mask, int32 Width,
        const PinWrightZFighting::FRegion& Region, int32& OutX, int32& OutY)
    {
        const int32 CentroidX = FMath::Clamp(FMath::RoundToInt(Region.CentroidX), Region.MinX, Region.MaxX);
        const int32 CentroidY = FMath::Clamp(FMath::RoundToInt(Region.CentroidY), Region.MinY, Region.MaxY);
        if (PinWrightZFighting::IsAffected(Mask[CentroidY * Width + CentroidX]))
        {
            OutX = CentroidX;
            OutY = CentroidY;
            return true;
        }

        double BestDistanceSquared = TNumericLimits<double>::Max();
        bool bFound = false;
        for (int32 Y = Region.MinY; Y <= Region.MaxY; ++Y)
        {
            for (int32 X = Region.MinX; X <= Region.MaxX; ++X)
            {
                if (!PinWrightZFighting::IsAffected(Mask[Y * Width + X]))
                {
                    continue;
                }
                const double DeltaX = X - Region.CentroidX;
                const double DeltaY = Y - Region.CentroidY;
                const double DistanceSquared = DeltaX * DeltaX + DeltaY * DeltaY;
                if (DistanceSquared < BestDistanceSquared)
                {
                    BestDistanceSquared = DistanceSquared;
                    OutX = X;
                    OutY = Y;
                    bFound = true;
                }
            }
        }
        return bFound;
    }

    bool ResolveChannel(const FString& Name, ESceneCaptureSource& OutSource)
    {
        const FString Lower = Name.ToLower();
        if (Lower == TEXT("basecolor"))
        {
            OutSource = SCS_BaseColor;
            return true;
        }
        if (Lower == TEXT("normal"))
        {
            OutSource = SCS_Normal;
            return true;
        }
        return false;
    }
}

// ---- render.detect_z_fighting ----
REGISTER_RPC_HANDLER("render.detect_z_fighting", "render",
    "Detect z-fighting in the current level from one camera and return an affected-pixel count, a pass/fail verdict against a fraction threshold, the responsible screen regions and actors, and a mask PNG.",
    RPC_PARAMS(
        RPC_PARAM_OPT("location", "object", "Camera location {x, y, z}. Defaults to the active Level Editor viewport camera."),
        RPC_PARAM_OPT("rotation", "object", "Camera rotation {pitch, yaw, roll}. Defaults to the active Level Editor viewport camera."),
        RPC_PARAM_DEF("fov", "number", "Horizontal field of view in degrees. Defaults to the viewport's FOV when the camera came from the viewport, otherwise 50.", "50"),
        RPC_PARAM_DEF("width", "number", "Analysis width in pixels. Default 768; a measured coplanar seam retained all 8 flagged regions at 768 versus 1920, while a separated control stayed clean at both sizes.", "768"),
        RPC_PARAM_DEF("height", "number", "Analysis height in pixels. See width.", "768"),
        RPC_PARAM_DEF("nearPlane", "number", "Base near clipping plane in world centimetres.", "10"),
        RPC_PARAM_DEF("perturbation", "string", "Which clipping plane is perturbed between the two renders: 'nearPlane' (default) or 'farPlane'. Use 'farPlane' when Nanite geometry produces scattered false positives.", "nearPlane"),
        RPC_PARAM_DEF("nearPlaneRatio", "number", "perturbation='nearPlane' only: the second render's near plane as a multiple of the first. Must not be 1 or a power of two.", "3"),
        RPC_PARAM_DEF("farPlane", "number", "perturbation='farPlane' only: the second render's finite far plane in world centimetres. Must sit beyond everything in frame.", "1000000"),
        RPC_PARAM_DEF("channels", "array", "Surface-identity channels to compare: any of 'baseColor', 'normal'.", "['baseColor','normal']"),
        RPC_PARAM_DEF("channelThreshold", "number", "Per-channel absolute difference that counts as a surface swap.", "0.02"),
        RPC_PARAM_DEF("minFraction", "number", "Pass/fail gate: the analysis fails when affectedFraction exceeds this.", "0.00002"),
        RPC_PARAM_DEF("maxRegions", "number", "How many clustered regions to report, largest first.", "8"),
        RPC_PARAM_DEF("mask", "boolean", "Write the per-pixel mask PNG.", "true"),
        RPC_PARAM_OPT("filename", "filepath", "Output filename for the mask inside Saved/Screenshots/ZFighting. The .png extension is appended if missing.")
    ))
{
    if (!GEditor)
    {
        Ctx.SendError(ErrorCodes::ERR_EDITOR_NOT_AVAILABLE, TEXT("Editor not available"));
        return true;
    }
    UWorld* EditorWorld = GEditor->GetEditorWorldContext().World();
    if (!EditorWorld)
    {
        Ctx.SendError(ErrorCodes::ERR_NO_EDITOR_WORLD, TEXT("No active editor world"));
        return true;
    }

    const TSharedPtr<FJsonObject>& Payload = Ctx.GetRawPayload();
    const bool bLocationProvided = Payload.IsValid() && Payload->HasField(TEXT("location"));
    const bool bRotationProvided = Payload.IsValid() && Payload->HasField(TEXT("rotation"));
    const bool bFovProvided = Payload.IsValid() && Payload->HasField(TEXT("fov"));

    FVector CameraLocation = ExtractVectorField(Payload, TEXT("location"), FVector::ZeroVector);
    FRotator CameraRotation = ExtractRotatorField(Payload, TEXT("rotation"), FRotator::ZeroRotator);
    float Fov = static_cast<float>(GetJsonNumberField(Payload, TEXT("fov"), 50.0));
    FString CameraSource = TEXT("caller");

    // "Check what I am looking at" is the common case, so an omitted pose adopts the
    // viewport camera. Unlike the viewport capture verbs this only READS the viewport
    // camera — it never moves it, never resizes it, and never reads its pixels — so it
    // leaves nothing for a concurrent agent to trip over and still works when the viewport
    // itself could not be captured.
    if (!bLocationProvided || !bRotationProvided)
    {
        FLevelEditorModule* LevelEditorModule =
            FModuleManager::GetModulePtr<FLevelEditorModule>(TEXT("LevelEditor"));
        TSharedPtr<IAssetViewport> ActiveViewport =
            LevelEditorModule ? LevelEditorModule->GetFirstActiveViewport() : nullptr;
        if (!ActiveViewport.IsValid())
        {
            Ctx.SendError(ErrorCodes::ERR_NO_ACTIVE_LEVEL_VIEWPORT,
                TEXT("No active Level Editor viewport to take the camera from; pass location and rotation explicitly"));
            return true;
        }
        FEditorViewportClient& ViewportClient = ActiveViewport->GetAssetViewportClient();
        if (!bLocationProvided)
        {
            CameraLocation = ViewportClient.GetViewLocation();
        }
        if (!bRotationProvided)
        {
            CameraRotation = ViewportClient.GetViewRotation();
        }
        if (!bFovProvided && ViewportClient.ViewFOV > 0.0f)
        {
            Fov = ViewportClient.ViewFOV;
        }
        CameraSource = TEXT("viewport");
    }

    const int32 Width = Ctx.GetInt(TEXT("width"), PinWrightZFighting::DefaultAnalysisLongEdge);
    const int32 Height = Ctx.GetInt(TEXT("height"), PinWrightZFighting::DefaultAnalysisLongEdge);
    const float NearPlane = static_cast<float>(GetJsonNumberField(Payload, TEXT("nearPlane"),
        PinWrightSceneCaptureProbe::DefaultNearClippingPlane));
    const FString Perturbation = GetJsonStringField(Payload, TEXT("perturbation"), TEXT("nearPlane")).ToLower();
    const double NearPlaneRatio = GetJsonNumberField(Payload, TEXT("nearPlaneRatio"),
        PinWrightZFighting::DefaultNearPlaneRatio);
    const double FarPlane = GetJsonNumberField(Payload, TEXT("farPlane"), 1000000.0);
    const double ChannelThreshold = GetJsonNumberField(Payload, TEXT("channelThreshold"),
        PinWrightZFighting::DefaultChannelThreshold);
    const double MinFraction = GetJsonNumberField(Payload, TEXT("minFraction"),
        PinWrightZFighting::DefaultMinFraction);
    const int32 MaxRegions = Ctx.GetInt(TEXT("maxRegions"), PinWrightZFighting::DefaultMaxRegions);
    const bool bWriteMask = Ctx.GetBool(TEXT("mask"), true);
    const FString Filename = GetJsonStringField(Payload, TEXT("filename"));

    if (Width <= 0 || Height <= 0
        || Width > PinWrightSceneCaptureProbe::MaxProbeDimension
        || Height > PinWrightSceneCaptureProbe::MaxProbeDimension)
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
            FString::Printf(TEXT("width and height must be in (0, %d]"),
                PinWrightSceneCaptureProbe::MaxProbeDimension));
        return true;
    }
    if (Fov <= 0.0f || Fov >= 180.0f)
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT, TEXT("fov must be in (0, 180)"));
        return true;
    }
    if (NearPlane <= 0.0f)
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
            TEXT("nearPlane must be greater than zero (world centimetres)"));
        return true;
    }
    if (ChannelThreshold < 0.0)
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT, TEXT("channelThreshold must not be negative"));
        return true;
    }
    if (MinFraction < 0.0 || MinFraction > 1.0)
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT, TEXT("minFraction must be in [0, 1]"));
        return true;
    }
    if (MaxRegions < 0 || MaxRegions > PinWrightZFighting::MaxAllowedRegions)
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
            FString::Printf(TEXT("maxRegions must be in [0, %d]"), PinWrightZFighting::MaxAllowedRegions));
        return true;
    }

    const bool bPerturbNear = Perturbation == TEXT("nearplane");
    const bool bPerturbFar = Perturbation == TEXT("farplane");
    if (!bPerturbNear && !bPerturbFar)
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
            TEXT("perturbation must be 'nearPlane' or 'farPlane'"));
        return true;
    }

    // The single most important input validation in this verb. Scaling the near plane by an
    // exact power of two scales every stored depth by that same power of two with no change
    // of rounding anywhere, so no depth comparison in the frame can change its outcome and
    // the analysis is guaranteed to report zero affected pixels — a clean pass on an
    // arbitrarily broken scene. Rejecting the input is the only honest response; silently
    // running it would ship the exact false negative this verb exists to prevent.
    if (bPerturbNear && PinWrightZFighting::IsPowerOfTwoRatio(NearPlaneRatio))
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
            FString::Printf(TEXT("nearPlaneRatio %g is a power of two, which perturbs nothing: under the ")
                TEXT("infinite-far reversed-Z projection a scene capture builds, scaling the near plane by a ")
                TEXT("power of two scales every depth value exactly, so no depth comparison can change and the ")
                TEXT("analysis would report zero affected pixels on any scene. Use a non-power-of-two ratio such as 3."),
                NearPlaneRatio));
        return true;
    }
    if (bPerturbNear && !(NearPlaneRatio > 0.0))
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT, TEXT("nearPlaneRatio must be greater than zero"));
        return true;
    }
    if (bPerturbFar && FarPlane <= static_cast<double>(NearPlane))
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
            TEXT("farPlane must be greater than nearPlane (world centimetres)"));
        return true;
    }

    // Channels.
    TArray<FString> ChannelNames;
    const TArray<TSharedPtr<FJsonValue>>* ChannelArray = nullptr;
    if (Payload.IsValid() && Payload->TryGetArrayField(TEXT("channels"), ChannelArray) && ChannelArray)
    {
        for (const TSharedPtr<FJsonValue>& Value : *ChannelArray)
        {
            FString Name;
            if (Value.IsValid() && Value->TryGetString(Name) && !Name.IsEmpty())
            {
                ChannelNames.AddUnique(Name);
            }
        }
        if (ChannelNames.Num() == 0)
        {
            Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
                TEXT("channels must contain at least one of 'baseColor', 'normal'"));
            return true;
        }
    }
    else
    {
        // Both by default. Base colour alone misses a duplicate that was rotated slightly
        // rather than translated (same albedo, different normal); normal alone misses two
        // genuinely coplanar surfaces wearing different materials. Together they identify a
        // surface well enough that a swap between two visibly different surfaces is caught.
        ChannelNames.Add(TEXT("baseColor"));
        ChannelNames.Add(TEXT("normal"));
    }

    TArray<ESceneCaptureSource> ChannelSources;
    for (const FString& Name : ChannelNames)
    {
        ESceneCaptureSource Source = SCS_BaseColor;
        if (!ResolveChannel(Name, Source))
        {
            Ctx.SendError(ErrorCodes::ERR_UNSUPPORTED_CHANNEL,
                FString::Printf(TEXT("Unsupported channel '%s'; expected 'baseColor' or 'normal'"), *Name));
            return true;
        }
        ChannelSources.Add(Source);
    }

    // ---- Render ----

    PinWrightSceneCaptureProbe::FSceneCaptureProbe Probe(EditorWorld);
    if (!Probe.IsValid())
    {
        Ctx.SendError(ErrorCodes::ERR_SCENE_CAPTURE_FAILED,
            TEXT("Could not create the offscreen scene capture probe in the editor world"));
        return true;
    }

    const double PerturbedNearPlane = bPerturbNear
        ? static_cast<double>(NearPlane) * NearPlaneRatio
        : static_cast<double>(NearPlane);

    PinWrightSceneCaptureProbe::FProbeRequest BaseRequest;
    BaseRequest.Location = CameraLocation;
    BaseRequest.Rotation = CameraRotation;
    BaseRequest.Fov = Fov;
    BaseRequest.Width = Width;
    BaseRequest.Height = Height;
    BaseRequest.NearClippingPlane = NearPlane;
    BaseRequest.FarClippingPlane = 0.0f;

    PinWrightSceneCaptureProbe::FProbeRequest PerturbedRequest = BaseRequest;
    if (bPerturbNear)
    {
        PerturbedRequest.NearClippingPlane = static_cast<float>(PerturbedNearPlane);
    }
    else
    {
        PerturbedRequest.FarClippingPlane = static_cast<float>(FarPlane);
    }

    const int64 PixelCount = static_cast<int64>(Width) * static_cast<int64>(Height);
    TArray<uint8> Mask;
    Mask.SetNumZeroed(static_cast<int32>(PixelCount));

    TArray<FLinearColor> CaptureA;
    TArray<FLinearColor> CaptureB;
    TArray<int32> PerChannelFlagged;
    FString ErrorCode;
    FString ErrorMessage;

    for (int32 ChannelIndex = 0; ChannelIndex < ChannelSources.Num(); ++ChannelIndex)
    {
        BaseRequest.Source = ChannelSources[ChannelIndex];
        PerturbedRequest.Source = ChannelSources[ChannelIndex];

        if (!Probe.Capture(BaseRequest, CaptureA, ErrorCode, ErrorMessage)
            || !Probe.Capture(PerturbedRequest, CaptureB, ErrorCode, ErrorMessage))
        {
            Ctx.SendError(ErrorCode, ErrorMessage);
            return true;
        }

        PerChannelFlagged.Add(PinWrightZFighting::AccumulateChannelDifference(
            CaptureA, CaptureB, ChannelThreshold, Mask));
    }

    // Depth, at the UNPERTURBED configuration, used only to decide which pixels the
    // perturbation itself invalidated and to place the reported regions in the world. It is
    // deliberately not differenced: see the ZFightingAnalysis.h header for why a depth
    // difference cannot separate a z-fight from its own quantisation noise.
    TArray<FLinearColor> Depth;
    PinWrightSceneCaptureProbe::FProbeRequest DepthRequest = BaseRequest;
    DepthRequest.Source = SCS_SceneDepth;
    if (!Probe.Capture(DepthRequest, Depth, ErrorCode, ErrorMessage))
    {
        Ctx.SendError(ErrorCode, ErrorMessage);
        return true;
    }

    const double ExclusionMin = bPerturbNear ? PerturbedNearPlane : static_cast<double>(NearPlane);
    const double ExclusionMax = bPerturbFar ? FarPlane : 0.0;
    const int32 ExcludedPixels = PinWrightZFighting::ApplyDepthRangeExclusion(
        Depth, ExclusionMin, ExclusionMax, Mask);

    const int32 AffectedPixels = PinWrightZFighting::CountAffected(Mask);
    const int64 AnalyzedPixels = PixelCount - ExcludedPixels;
    const double AffectedFraction = AnalyzedPixels > 0
        ? static_cast<double>(AffectedPixels) / static_cast<double>(AnalyzedPixels)
        : 0.0;

    // ---- Regions ----

    TArray<PinWrightZFighting::FRegion> Regions;
    int32 TotalRegions = 0;
    PinWrightZFighting::FindRegions(Mask, Width, Height, MaxRegions, Regions, TotalRegions);

    TArray<FActorBoundsEntry> ActorBounds;
    if (Regions.Num() > 0)
    {
        CollectActorBounds(EditorWorld, ActorBounds);
    }

    TArray<TSharedPtr<FJsonValue>> RegionValues;
    for (int32 Index = 0; Index < Regions.Num(); ++Index)
    {
        const PinWrightZFighting::FRegion& Region = Regions[Index];
        TSharedPtr<FJsonObject> RegionObject = MakeShared<FJsonObject>();
        RegionObject->SetNumberField(TEXT("index"), Index);
        RegionObject->SetNumberField(TEXT("x"), Region.MinX);
        RegionObject->SetNumberField(TEXT("y"), Region.MinY);
        RegionObject->SetNumberField(TEXT("width"), Region.MaxX - Region.MinX + 1);
        RegionObject->SetNumberField(TEXT("height"), Region.MaxY - Region.MinY + 1);
        RegionObject->SetNumberField(TEXT("pixels"), Region.Pixels);
        RegionObject->SetNumberField(TEXT("centroidX"), Region.CentroidX);
        RegionObject->SetNumberField(TEXT("centroidY"), Region.CentroidY);

        int32 SampleX = 0;
        int32 SampleY = 0;
        if (FindRepresentativePixel(Mask, Width, Region, SampleX, SampleY))
        {
            const double SampleDepth = static_cast<double>(Depth[SampleY * Width + SampleX].R);
            RegionObject->SetNumberField(TEXT("depth"), SampleDepth);
            const FVector Ray = PinWrightSceneCaptureProbe::PixelToCameraRay(
                CameraRotation, Fov, Width, Height, SampleX, SampleY);
            const FVector WorldPoint = CameraLocation + Ray * SampleDepth;
            RegionObject->SetObjectField(TEXT("worldLocation"),
                PinWrightRenderCapture::MakeVectorObject(WorldPoint));

            TArray<FString> ActorNames;
            NameActorsAtPoint(ActorBounds, WorldPoint, ActorNames);
            TArray<TSharedPtr<FJsonValue>> ActorValues;
            for (const FString& ActorName : ActorNames)
            {
                ActorValues.Add(MakeShared<FJsonValueString>(ActorName));
            }
            RegionObject->SetArrayField(TEXT("actors"), ActorValues);
        }

        RegionValues.Add(MakeShared<FJsonValueObject>(RegionObject));
    }

    // ---- Mask image ----

    FString MaskPath;
    FString MaskFilename;
    FIntPoint MaskSize(0, 0);
    if (bWriteMask)
    {
        MaskSize = PinWrightZFighting::ResolveMaskSize(Width, Height,
            PinWrightZFighting::DefaultMaskLongEdge);
        TArray<FColor> MaskBitmap;
        PinWrightZFighting::BuildMaskBitmap(Mask, Width, Height, MaskSize.X, MaskSize.Y, MaskBitmap);

        TArray<uint8> PngData;
        if (!PinWrightScreenshotUtils::EncodeBitmapToPng(MaskSize.X, MaskSize.Y, MaskBitmap, PngData))
        {
            Ctx.SendError(ErrorCodes::ERR_ENCODE_FAILED, TEXT("Could not encode the z-fighting mask as PNG"));
            return true;
        }
        MaskPath = PinWrightScreenshotUtils::MakeScreenshotOutputPath(
            Filename, TEXT("ZFighting"), TEXT("ZFighting"), MaskFilename);
        if (!FFileHelper::SaveArrayToFile(PngData, *MaskPath))
        {
            Ctx.SendError(ErrorCodes::ERR_SAVE_FAILED,
                FString::Printf(TEXT("Could not write the z-fighting mask to %s"), *MaskPath));
            return true;
        }
    }

    // ---- Response ----

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("analysis"), TEXT("zFighting"));

    // A frame with nothing measurable in it must never read as a clean frame. Failing here
    // rather than reporting pass:true with affectedPixels:0 is the whole difference between
    // "checked and found nothing" and "could not check".
    const bool bNothingMeasured = AnalyzedPixels <= 0;
    const bool bPass = !bNothingMeasured && AffectedFraction <= MinFraction;
    Result->SetBoolField(TEXT("pass"), bPass);
    if (bNothingMeasured)
    {
        Result->SetStringField(TEXT("warning"),
            TEXT("Every pixel was excluded, so nothing was measured and this result is not evidence of a clean frame. "
                 "The camera is probably inside or against geometry, or the whole frame is empty."));
    }

    Result->SetNumberField(TEXT("affectedPixels"), AffectedPixels);
    Result->SetNumberField(TEXT("affectedFraction"), AffectedFraction);
    Result->SetNumberField(TEXT("analyzedPixels"), static_cast<double>(AnalyzedPixels));
    Result->SetNumberField(TEXT("totalPixels"), static_cast<double>(PixelCount));
    Result->SetNumberField(TEXT("clipExcludedPixels"), ExcludedPixels);
    Result->SetNumberField(TEXT("minFraction"), MinFraction);
    Result->SetNumberField(TEXT("analysisWidth"), Width);
    Result->SetNumberField(TEXT("analysisHeight"), Height);

    // The analysis resolution belongs in the response because a result taken at a lower one
    // is not comparable with a result taken at the default, and a caller diffing two runs
    // must be able to see that rather than infer it.
    if (FMath::Max(Width, Height) < PinWrightZFighting::DefaultAnalysisLongEdge)
    {
        Result->SetStringField(TEXT("resolutionWarning"),
            FString::Printf(TEXT("Analysis ran at %dx%d, below the %d long-edge default. A z-fighting seam is a thin ")
                TEXT("sliver whose screen width scales with resolution, so seams visible at the default size can vanish ")
                TEXT("here and produce a false pass."),
                Width, Height, PinWrightZFighting::DefaultAnalysisLongEdge));
    }

    Result->SetStringField(TEXT("perturbation"), bPerturbNear ? TEXT("nearPlane") : TEXT("farPlane"));
    Result->SetNumberField(TEXT("nearPlane"), NearPlane);
    if (bPerturbNear)
    {
        Result->SetNumberField(TEXT("nearPlaneRatio"), NearPlaneRatio);
        Result->SetNumberField(TEXT("perturbedNearPlane"), PerturbedNearPlane);
    }
    else
    {
        Result->SetNumberField(TEXT("farPlane"), FarPlane);
    }

    TArray<TSharedPtr<FJsonValue>> ChannelValues;
    for (int32 Index = 0; Index < ChannelNames.Num(); ++Index)
    {
        TSharedPtr<FJsonObject> ChannelObject = MakeShared<FJsonObject>();
        ChannelObject->SetStringField(TEXT("channel"), ChannelNames[Index]);
        ChannelObject->SetNumberField(TEXT("flaggedPixels"),
            PerChannelFlagged.IsValidIndex(Index) ? PerChannelFlagged[Index] : 0);
        ChannelValues.Add(MakeShared<FJsonValueObject>(ChannelObject));
    }
    Result->SetArrayField(TEXT("channels"), ChannelValues);
    Result->SetNumberField(TEXT("channelThreshold"), ChannelThreshold);

    // SCS_BaseColor and SCS_Normal are G-buffer reads and exist only under deferred shading;
    // the renderer silently substitutes SCS_SceneColorHDR for both under forward shading
    // (Renderer/Private/SceneCaptureRendering.cpp). The analysis still functions there, but
    // it is then comparing LIT COLOUR, which reacts to lighting as well as to surface
    // identity, so the caller has to be told rather than left to wonder.
    if (const URendererSettings* RendererSettings = GetDefault<URendererSettings>())
    {
        if (RendererSettings->bForwardShading)
        {
            Result->SetBoolField(TEXT("forwardShading"), true);
            Result->SetStringField(TEXT("shadingNote"),
                TEXT("This project uses forward shading, where the renderer substitutes lit scene colour for the "
                     "baseColor and normal G-buffer channels. The comparison still detects surface swaps but also "
                     "reacts to lighting differences, so raise channelThreshold if results look noisy."));
        }
    }

    Result->SetArrayField(TEXT("regions"), RegionValues);
    Result->SetNumberField(TEXT("regionsReported"), Regions.Num());
    Result->SetNumberField(TEXT("regionsFound"), TotalRegions);

    if (bWriteMask)
    {
        Result->SetStringField(TEXT("maskPath"), MaskPath);
        Result->SetStringField(TEXT("maskFilename"), MaskFilename);
        Result->SetNumberField(TEXT("maskWidth"), MaskSize.X);
        Result->SetNumberField(TEXT("maskHeight"), MaskSize.Y);
        Result->SetStringField(TEXT("mimeType"), TEXT("image/png"));
    }

    Result->SetObjectField(TEXT("cameraLocation"), PinWrightRenderCapture::MakeVectorObject(CameraLocation));
    Result->SetObjectField(TEXT("cameraRotation"), PinWrightRenderCapture::MakeRotatorObject(CameraRotation));
    Result->SetNumberField(TEXT("fov"), Fov);
    Result->SetStringField(TEXT("cameraSource"), CameraSource);
    Result->SetStringField(TEXT("projectionMode"), TEXT("perspective"));
    Result->SetStringField(TEXT("renderer"), PinWrightCaptureRenderer::SceneCapture2D);
    Result->SetNumberField(TEXT("sceneRenders"), Probe.GetRenderCount());
    Result->SetStringField(TEXT("levelPath"),
        EditorWorld->GetOutermost() ? EditorWorld->GetOutermost()->GetName() : FString());

    Ctx.SendSuccess(Result);
    return true;
}
