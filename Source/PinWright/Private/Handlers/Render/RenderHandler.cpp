// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/ErrorCodes.h"
#include "Handlers/Material/MaterialShaderState.h"
#include "Handlers/PackagePathCompose.h"
#include "Handlers/Render/CameraShotPlanUtils.h"
#include "Handlers/Render/CaptureSubject.h"
#include "Handlers/Render/CaptureSubjectProviders_Mesh.h"
// The ONE kind-specific include in this file, and it buys a measurement nothing else can reach:
// MakeNiagaraSubjectDetailObject reads the tick delta, the tick count and the age the simulation
// actually got to off the resolved subject's provider state, and returns an invalid pointer for
// every other kind. That is an ask-the-provider call, not a switch on ESubjectKind - the verb
// still never branches on the kind to decide what to DO, only on whether the answer exists.
#include "Handlers/Render/CaptureSubjectProviders_Niagara.h"
#include "Handlers/Render/FlatRegionStats.h"
#include "Handlers/Render/MeshPreviewCaptureUtils.h"
#include "Handlers/Render/OpenLevelCapture.h"
#include "Handlers/Render/OrthoTileCaptureUtils.h"
#include "Handlers/Render/PoseListCapture.h"
#include "Handlers/Render/PreviewSceneRig.h"
#include "Handlers/Render/SubjectRegionStats.h"
#include "Handlers/Render/SubjectTimeSeries.h"
#include "Handlers/Render/PreviewViewportCaptureUtils.h"
#include "Handlers/Render/ViewModeVocabulary.h"
#include "Dispatch/SafePoint.h"
#include "PinWrightSubsystem.h"
#include "PinWrightHelpers.h"
#include "State/PluginState.h"
#include "Utils/AssetCompilePump.h"
#include "Utils/AssetUtils.h"
#include "Utils/CaptureReadinessGate.h"
#include "Utils/MeshRebuildRenderGuard.h"
#include "Utils/OnScreenMessageSurvey.h"
#include "Utils/ScreenshotUtils.h" // FlushBeforeReadback, the shared readback preamble

#include "AssetCompilingManager.h"
#include "Containers/Ticker.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/EngineBaseTypes.h"
#include "Engine/PostProcessVolume.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
// FMaterialParameterInfo et al. moved from the top-level MaterialTypes.h into
// Materials/MaterialParameters.h in UE 5.7; on 5.6 and earlier they live in
// MaterialTypes.h, which has no MaterialParameters.h (and is deprecated on 5.8).
#if __has_include("Materials/MaterialParameters.h")
#include "Materials/MaterialParameters.h"
#else
#include "MaterialTypes.h"
#endif
#include "AssetRegistry/AssetRegistryModule.h"
#include "EditorAssetLibrary.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture.h"
#include "UObject/Package.h"
#include "Compat/EngineVersionCompat.h"
#include "Runtime/Launch/Resources/Version.h"
#include "Editor.h"
#include "EditorViewportClient.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "IAssetViewport.h"
#include "Layout/ChildrenBase.h"
#include "LevelEditor.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Modules/ModuleManager.h"
#include "SEditorViewport.h"
#include "Slate/SceneViewport.h"
#include "Toolkits/AssetEditorToolkit.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Toolkits/IToolkitHost.h"
#include "UObject/StrongObjectPtr.h"
#include "Widgets/SWindow.h"
#include "Widgets/SWidget.h"

// ---- the asset-editor viewport walk is GONE FROM HERE, and that is the point ----
//
// This file used to carry its own copy: FindEditorViewportRecursive matched a widget on
// `GetTypeAsString().EndsWith("EditorViewport")` and then StaticCastSharedRef<SEditorViewport>'d
// it, and FindAssetEditorViewport static_cast<FAssetEditorToolkit*>'d an IAssetEditorInstance with
// no check that the toolkit is one. SEditorViewport carries no SLATE_DECLARE_WIDGET, so there is
// no runtime hierarchy check behind either cast: any widget whose type NAME merely ends in
// "EditorViewport" was undefined behaviour, and the suffix is also why the Niagara system viewport
// (SNiagaraSystemViewport, which really does derive from SEditorViewport) read as unreachable.
// Both are replaced by the one walk in CaptureSubject.cpp, which matches an exact allow-list of
// verified widget type names and refuses an unknown type with PREVIEW_VIEWPORT_NOT_FOUND naming it
// instead of casting it. Entries whose widget header is reachable from this module carry a
// compile-time TIsDerivedFrom static_assert; the rest are verified by a checked citation rather
// than by the compiler, so do not read the allow-list as uniformly compile-guarded in either
// direction. This verb reaches that walk through PinWrightCaptureSubject::Resolve rather than
// calling it directly.

namespace
{
    void AddCaptureFields(
        const PinWrightRenderCapture::FViewportCaptureOutput& Capture,
        const PinWrightRenderCapture::FViewportCaptureRequest& Request,
        TSharedPtr<FJsonObject>& Result)
    {
        Result->SetStringField(TEXT("path"), Capture.Path);
        Result->SetStringField(TEXT("filename"), Capture.Filename);
        Result->SetNumberField(TEXT("width"), Capture.Width);
        Result->SetNumberField(TEXT("height"), Capture.Height);
        Result->SetNumberField(TEXT("sizeBytes"), static_cast<double>(Capture.SizeBytes));
        Result->SetStringField(TEXT("projectionMode"), Request.ProjectionMode);
        // cameraLocation / cameraRotation are the pose the PIXELS show -- MEASURED off the
        // viewport client after the camera was applied, not echoed from the request -- so they
        // round-trip exactly into spatial.raycast_screen. Two things can separate them from the
        // request: an orthographic capture's in-plane orientation is quantised to the viewport
        // type the engine renders, and a viewport that ignored the pose outright reports where it
        // really was (see the `aim` block). The request is echoed alongside whenever they differ.
        Result->SetObjectField(TEXT("cameraLocation"),
            PinWrightRenderCapture::MakeVectorObject(Capture.EffectiveLocation));
        Result->SetObjectField(TEXT("cameraRotation"),
            PinWrightRenderCapture::MakeRotatorObject(Capture.EffectiveRotation));
        if (Capture.bOrthoRotationSnapped || !Capture.bCameraAimApplied)
        {
            Result->SetObjectField(TEXT("requestedRotation"),
                PinWrightRenderCapture::MakeRotatorObject(Request.Rotation));
        }
        if (!Capture.bCameraAimApplied)
        {
            Result->SetObjectField(TEXT("requestedLocation"),
                PinWrightRenderCapture::MakeVectorObject(Request.Location));
        }
        if (Request.ProjectionMode == TEXT("orthographic"))
        {
            Result->SetNumberField(TEXT("orthoWidth"), Request.OrthoWidth);
            if (!Capture.OrthoView.IsEmpty())
            {
                Result->SetStringField(TEXT("orthoView"), Capture.OrthoView);
            }
        }
        else
        {
            Result->SetNumberField(TEXT("fov"), Request.Fov);
        }
        Result->SetStringField(TEXT("renderer"), Capture.Renderer);
        Result->SetStringField(TEXT("mimeType"), TEXT("image/png"));

        TSharedPtr<FJsonObject> ImageStats = MakeShared<FJsonObject>();
        ImageStats->SetNumberField(TEXT("meanLuminance"), Capture.ImageStats.MeanLuminance);
        ImageStats->SetNumberField(TEXT("luminanceVariance"), Capture.ImageStats.LuminanceVariance);
        ImageStats->SetNumberField(TEXT("minLuminance"), Capture.ImageStats.MinLuminance);
        ImageStats->SetNumberField(TEXT("maxLuminance"), Capture.ImageStats.MaxLuminance);
        // The two numbers `blank` is made of, published beside the verdict so a caller can judge a
        // near-empty frame without inheriting this verb's threshold. `litPixelFraction` is the one
        // to compare across captures of the SAME scene at different sizes; `litPixelCount` is the
        // one that holds still for fixed-pixel editor overlays.
        ImageStats->SetNumberField(TEXT("litPixelCount"),
            static_cast<double>(Capture.ImageStats.LitPixelCount));
        ImageStats->SetNumberField(TEXT("litPixelFraction"), Capture.ImageStats.LitPixelFraction);
        ImageStats->SetNumberField(TEXT("litLuminanceThreshold"),
            PinWrightRenderCapture::BlankLitLuminanceThreshold);
        // How many 8-bit luminance levels these pixels resolve, and the per-level floor the
        // verdict was counted against. The numbers `crushed` / `blownOut` are made of, published
        // beside them for the same reason the lit-pixel pair is published beside `blank`.
        PinWrightRenderCapture::AddToneRangeStatsFields(Capture.ImageStats, ImageStats);
        Result->SetObjectField(TEXT("imageStats"), ImageStats);
        Result->SetBoolField(TEXT("blank"), Capture.ImageStats.bBlank);
        // Siblings of `blank`, at the same prominence, because they answer the question `blank`
        // was mistaken for. `blank` is "nothing was drawn into this frame"; these are "what was
        // drawn cannot be read" -- a frame crushed past the end of the scene's exposure range is
        // correctly rendered, correctly non-blank, and worthless (B-exposure-pin-black-frame).
        // Reported, never rejected: unlike a dead readback, a dark frame can be deliberate, and
        // the caller keeps the PNG either way.
        // Gated on a lit view mode inside the helper: a wireframe or unlit frame resolves two or
        // three tone levels because that is what it was asked to draw, and the classifier would
        // read that as a collapse. Not-applicable is reported, never omitted.
        PinWrightRenderCapture::AddToneRangeVerdictFields(Capture, Result);
        // The only block in this response that describes the SUBJECT rather than the frame.
        // `blank`, `meanLuminance`, `litPixelFraction` and the tone verdict are all correct
        // statements about the picture as a whole, and all four read healthy on a frame whose
        // asset rendered as a pure black silhouette inside a correctly lit environment -- the
        // backdrop dominates every one of them (B-capture-asset-preview-renders-foliage-black).
        // Reports `measured: false` with a reason on a capture that carried no subject bounds,
        // which is every level capture; see SubjectRegionStats.h.
        PinWrightSubjectRegion::AddSubjectRegionFields(Capture.SubjectRegion, Result);
        Result->SetNumberField(TEXT("redrawRetries"), Capture.RedrawRetries);

        Result->SetObjectField(TEXT("viewport"),
            PinWrightRenderCapture::MakeViewportInfoObject(Capture));
        // Unconditional, like the viewport block: a frame whose distant content was distance-culled
        // is pixel-for-pixel indistinguishable from a frame whose distant content is not in the
        // level, so the response has to say which state the renderer was in.
        Result->SetObjectField(TEXT("viewDistance"),
            PinWrightRenderCapture::MakeViewDistanceInfoObject(Capture));
    }

    void AddWorldFields(UWorld* ActiveWorld, UWorld* ViewportWorld, TSharedPtr<FJsonObject>& Result)
    {
        const auto AddWorld = [&Result](const TCHAR* Prefix, UWorld* World)
        {
            Result->SetStringField(FString::Printf(TEXT("%sWorld"), Prefix),
                World ? World->GetPathName() : FString());
            Result->SetStringField(FString::Printf(TEXT("%sWorldPackage"), Prefix),
                World && World->GetOutermost() ? World->GetOutermost()->GetName() : FString());
        };
        AddWorld(TEXT("active"), ActiveWorld);
        AddWorld(TEXT("viewport"), ViewportWorld);
        Result->SetBoolField(TEXT("worldMatches"), ActiveWorld && ViewportWorld == ActiveWorld);
    }
}

// ---- subject plumbing shared by this file's two capture verbs ----
//
// File-unique NAMED namespace, never anonymous: Unity merges these translation units and an
// anonymous helper here would collide with a same-named one in a sibling Render/*.cpp. Same
// reason CameraShotPlanUtils.h and PinWrightCameraFrameSubject (CameraFrameHandler.cpp) are named.
namespace PinWrightRenderSubject
{
    inline bool HasSubjectField(const TSharedPtr<FJsonObject>& Payload)
    {
        return Payload.IsValid() && Payload->HasField(TEXT("subject"));
    }

    // Which asset classes render.capture_asset_preview offers, decided HERE rather than in the
    // resolver on purpose: the registry knows how to ACQUIRE a kind, the verb decides which kinds
    // it publishes, and the refusal has to name the sibling verb by hand -- without that pointer an
    // agent told "Static Mesh only" concludes isolated skinned capture does not exist and goes off
    // to dirty a level instead (Tests/Render/TestAnimationCaptureHandlers.cpp asserts the pointer).
    //
    // Cross-module classes are reached by REFLECTION, not by link: USkeletalMesh and
    // UAnimationAsset are Engine and UNiagaraSystem is the Niagara module, and a FindObject on the
    // /Script path is this repo's standing pattern for exactly that (CLAUDE.md, UE version compat).
    // It also keeps this file free of a NiagaraSystem.h include for a single IsA test.
    inline bool AssetClassIsServed(UObject* Asset)
    {
        if (!Asset)
        {
            return false;
        }
        if (Asset->IsA<UStaticMesh>())
        {
            return true;
        }
        static const TCHAR* ServedClassPaths[] = {
            TEXT("/Script/Engine.SkeletalMesh"),
            TEXT("/Script/Engine.AnimationAsset"),
            TEXT("/Script/Niagara.NiagaraSystem"),
        };
        for (const TCHAR* ClassPath : ServedClassPaths)
        {
            if (const UClass* ServedClass = FindObject<UClass>(nullptr, ClassPath))
            {
                if (Asset->IsA(ServedClass))
                {
                    return true;
                }
            }
        }
        return false;
    }

    // ---- neutralise the shared parser's `previewScene` pin on the LEVEL-viewport path ----
    //
    // A NAMED function rather than two statements inlined into the handler body, because this is
    // the assertion that separates "the parameter is undeclared" from "the parameter is inert".
    // The dispatcher's unknown-param gate answers the first, on the wire only; only this clear
    // answers the second, on a direct invocation. A test can call the shared parser, call this,
    // and assert the pin is gone -- which an inline `Request.PreviewSceneRig = {}` buried in the
    // handler body cannot be asked to prove without a live Level Editor viewport and a GPU, and a
    // capture test that cannot get a GPU takes a conditional-skip path and reports success having
    // asserted nothing (board ticket B-test-skips-assertions-silently).
    //
    // Deliberately NOT `inline`, unlike its neighbours above: Tests/Render/
    // TestPreviewSceneRigRenderVerbs.cpp declares this signature and links against this
    // definition, and an inline definition is only guaranteed visible inside its own translation
    // unit -- which under Unity is a chunk boundary nobody controls.
    void ClearPreviewSceneRigForLevelViewport(PinWrightRenderCapture::FViewportCaptureRequest& Request)
    {
        Request.PreviewSceneRig = PinWrightPreviewSceneRig::FPreviewSceneRigPin{};
    }
}

// ---- render.create_render_target ----
REGISTER_RPC_HANDLER("render.create_render_target", "render", "Create a render target asset",
    RPC_PARAMS(
        RPC_PARAM_OPT("name", "string", "Name of the render target"),
        RPC_PARAM_OPT("width", "number", "Width in pixels (default 768)"),
        RPC_PARAM_OPT("height", "number", "Height in pixels (default 768)"),
        RPC_PARAM_OPT("format", "string", "Pixel format"),
        RPC_PARAM_OPT("packagePath", "path", "Package path (default /Game/RenderTargets)")
    ))
{
    FString Name = Ctx.GetString(TEXT("name"));
    int32 Width = Ctx.GetInt(TEXT("width"), PinWrightRenderCapture::DefaultCaptureEdge);
    int32 Height = Ctx.GetInt(TEXT("height"), PinWrightRenderCapture::DefaultCaptureEdge);
    FString FormatStr = Ctx.GetString(TEXT("format"));
    FString PackagePath = Ctx.GetString(TEXT("packagePath"), TEXT("/Game/RenderTargets"));

    FString AssetName = Name.IsEmpty() ? TEXT("NewRenderTarget") : Name;

    // COMPOSED AND CHECKED HERE, because the composition is what kills the process. `packagePath`
    // and `name` both arrive raw off the wire and were concatenated straight into CreatePackage,
    // which logs at Fatal for a name containing "//" (UObjectGlobals.cpp:1094-1096) and for one
    // that resolves to empty (:1118). Fatal is not compiled out in any configuration, so such a
    // call does not fail - it ends the editor PROCESS and every unsaved package in it, and no
    // post-call null check can catch it because nothing after the call runs. FString::operator/
    // does not save this: PathAppend only avoids doubling a slash it would add itself, so a
    // `name` of "a//b" reached the Fatal from either composition style (board
    // B-createpackage-unvalidated-paths-plugin-wide; mechanism measured on
    // B-foliage-add-type-name-with-slash-kills-the-editor). The trailing slash operator/ used to
    // absorb is trimmed first, so "/Game/RenderTargets/" is still an accepted packagePath.
    PackagePath.RemoveFromEnd(TEXT("/"));
    FString FullPath;
    FString PathError;
    if (!PinWrightComposeAssetPackagePath(PackagePath, AssetName, FullPath, PathError))
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
            FString::Printf(TEXT("%s Pass a bare asset name in 'name' and the destination folder "
                                 "in 'packagePath'."), *PathError));
        return true;
    }

    UPackage* Package = CreatePackage(*FullPath);
    UTextureRenderTarget2D* RT = NewObject<UTextureRenderTarget2D>(Package, UTextureRenderTarget2D::StaticClass(), FName(*AssetName), RF_Public | RF_Standalone);

    if (RT)
    {
        RT->InitAutoFormat(Width, Height);
        RT->UpdateResourceImmediate(true);
        RT->MarkPackageDirty();
        FAssetRegistryModule::AssetCreated(RT);

        TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
        Result->SetStringField(TEXT("assetPath"), RT->GetPathName());
        Ctx.SendSuccess(Result);
    }
    else
    {
        Ctx.SendError(ErrorCodes::ERR_CREATE_FAILED, TEXT("Failed to create render target."));
    }
    return true;
}

// ---- render.capture_mesh ----
REGISTER_RPC_HANDLER("render.capture_mesh", "render", "Capture a Static Mesh or Skeletal Mesh into a private transient preview scene as an exact-size PNG. Does not open an asset editor or use the active level world.",
    RPC_PARAMS(
        RPC_PARAM_REQ("assetPath", "path", "Static Mesh or Skeletal Mesh object path, e.g. /Engine/BasicShapes/Cube.Cube."),
        RPC_PARAM_OPT("filename", "filepath", "Output filename inside Saved/Screenshots/MeshCapture. The .png extension is appended if missing."),
        RPC_PARAM_OPT("width", "number", "Output width in pixels. Default 768."),
        RPC_PARAM_OPT("height", "number", "Output height in pixels. Default 768."),
        RPC_PARAM_OPT("location", "object", "Camera location {x,y,z}. Omit location and rotation to frame the mesh bounds."),
        RPC_PARAM_OPT("rotation", "object", "Camera rotation {pitch,yaw,roll}. Omit location and rotation to frame the mesh bounds."),
        RPC_PARAM_OPT("projectionMode", "string", "'perspective' (default) or 'orthographic'."),
        RPC_PARAM_OPT("fov", "number", "Perspective horizontal field of view in degrees. Default 50."),
        FParamSpec{TEXT("orthoWidth"), TEXT("number"),
            TEXT("Orthographic frame width in world centimetres. When omitted, it is fitted to the mesh bounds."),
            false, TEXT(""), TArray<FString>({TEXT("orthoWorldWidth")})},
        RPC_PARAM_OPT("exposure", "object|number", PINWRIGHT_EXPOSURE_PARAM_DESC),
        RPC_PARAM_OPT("viewMode", "string", PINWRIGHT_VIEW_MODE_PARAM_DESC),
        RPC_PARAM_OPT("previewScene", "object", PINWRIGHT_PREVIEW_SCENE_PARAM_DESC " This verb owns disposable local floor and environment components and never opens an asset editor."),
        RPC_PARAM_OPT("count", "integer", "Capture 1-24 views around the mesh, spread by `distribution`. Cannot be combined with views or an explicit camera pose."),
        RPC_PARAM_OPT("views", "string", "Canned view set. 'sides' captures front, back, left, right, top and bottom."),
        RPC_PARAM_OPT("elevation", "number", "Elevation for count views, in degrees. Default 20."),
        RPC_PARAM_DEF("distribution", "string", "How `count` spreads its shots. 'ring' (default) puts them at evenly-spaced azimuths on ONE horizontal circle at `elevation`, which never sees the top or the underside. 'sphere' uses a golden-angle (Fibonacci) spiral over the whole viewing sphere - deterministic, near-optimal even coverage for any N - and IGNORES `elevation`. Only affects `count`; `views` names its own poses.", "ring"),
        RPC_PARAM_OPT("seed", "number", "Jitter the shot distribution by a seeded azimuth offset. Omitted, output is deterministic (offset 0); supplied, output is STILL deterministic and the seed is echoed in shotDistribution so any set can be retaken exactly."),
        RPC_PARAM_DEF("measureCoverage", "boolean", "Draw once more with only the mesh hidden and report the fraction of pixels it changed. Defaults to true.", "true"),
        RPC_PARAM_DEF("padding", "number", "Bounds-fit margin multiplier. Default 1.25.", "1.25")
    ))
{
    const TSharedPtr<FJsonObject>& Payload = Ctx.GetRawPayload();
    const FString AssetPath = Ctx.GetString(TEXT("assetPath"));
    if (AssetPath.IsEmpty())
    {
        Ctx.SendError(ErrorCodes::ERR_MISSING_ASSET_PATH,
            TEXT("assetPath is required for render.capture_mesh"));
        return true;
    }

    PinWrightRenderCapture::FViewportCaptureRequest CaptureRequest;
    FString ErrorCode;
    FString ErrorMessage;
    if (!PinWrightRenderCapture::ParseOffscreenCaptureRequest(
        Payload, CaptureRequest, ErrorCode, ErrorMessage))
    {
        Ctx.SendError(ErrorCode, ErrorMessage);
        return true;
    }
    if (CaptureRequest.ViewMode.bRequested
        && !PinWrightOrthoTiles::IsViewModeReachableOnSceneCapture(
            CaptureRequest.ViewMode.ViewMode, ErrorCode, ErrorMessage))
    {
        Ctx.SendError(ErrorCode, ErrorMessage.Replace(
            TEXT("render.capture_ortho_tiles"), TEXT("render.capture_mesh")));
        return true;
    }

    const bool bHasCount = Payload.IsValid() && Payload->HasField(TEXT("count"));
    const bool bHasViews = Payload.IsValid() && Payload->HasField(TEXT("views"));
    const FString Views = Ctx.GetString(TEXT("views")).ToLower();
    if (bHasViews && Views != TEXT("sides"))
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
            TEXT("views must be 'sides' when supplied"));
        return true;
    }
    if ((bHasCount && bHasViews)
        || ((bHasCount || bHasViews)
            && (CaptureRequest.bLocationProvided || CaptureRequest.bRotationProvided)))
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
            TEXT("count and views are mutually exclusive, and neither can be combined with location or rotation"));
        return true;
    }

    const bool bDistributionProvided =
        Payload.IsValid() && Payload->HasField(TEXT("distribution"));
    const bool bSeedProvided = Payload.IsValid() && Payload->HasField(TEXT("seed"));
    const bool bElevationProvided = Payload.IsValid() && Payload->HasField(TEXT("elevation"));
    if (!bHasCount && !bHasViews
        && (bDistributionProvided || bSeedProvided || bElevationProvided))
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
            TEXT("distribution, seed and elevation require count or views"));
        return true;
    }

    const float Padding = static_cast<float>(Ctx.GetNumber(TEXT("padding"), 1.25));
    if (!FMath::IsFinite(Padding) || Padding <= 0.0f)
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
            TEXT("padding must be a finite number greater than zero"));
        return true;
    }

    PinWrightCameraFrame::EShotDistribution Distribution =
        PinWrightCameraFrame::EShotDistribution::Ring;
    const FString DistributionArg =
        Ctx.GetString(TEXT("distribution"), TEXT("ring")).ToLower();
    if (DistributionArg == TEXT("sphere"))
    {
        Distribution = PinWrightCameraFrame::EShotDistribution::Sphere;
    }
    else if (DistributionArg != TEXT("ring"))
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
            FString::Printf(TEXT("Unrecognised distribution '%s'. Valid: ring (default), sphere."),
                *DistributionArg));
        return true;
    }
    const int32 Seed = Ctx.GetInt(TEXT("seed"), 0);
    const double AzimuthOffsetDegrees = bSeedProvided
        ? PinWrightCameraFrame::SeedToAzimuthOffsetDegrees(Seed) : 0.0;
    float PlanElevation = 20.0f;
    if (bElevationProvided)
    {
        const double Elevation = Ctx.GetNumber(TEXT("elevation"), 20.0);
        if (!FMath::IsFinite(Elevation) || Elevation < -90.0 || Elevation > 90.0)
        {
            Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
                TEXT("elevation must be a finite number in [-90, 90]"));
            return true;
        }
        PlanElevation = static_cast<float>(Elevation);
    }

    TArray<PinWrightCameraFrame::FPlannedShot> Plans;
    if (bHasViews)
    {
        Plans = PinWrightCameraFrame::MakeSideViews(CaptureRequest.ProjectionMode);
    }
    else if (bHasCount)
    {
        const int32 Count = Ctx.GetInt(TEXT("count"), 1);
        if (Count < 1 || Count > PinWrightCameraFrame::GMaxOrbitShots)
        {
            Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
                FString::Printf(TEXT("count must be in [1, %d]"),
                    PinWrightCameraFrame::GMaxOrbitShots));
            return true;
        }
        Plans = Distribution == PinWrightCameraFrame::EShotDistribution::Sphere
            ? PinWrightCameraFrame::MakeSphereDistribution(
                Count, AzimuthOffsetDegrees, CaptureRequest.ProjectionMode)
            : PinWrightCameraFrame::MakeRingDistribution(
                Count, PlanElevation, AzimuthOffsetDegrees, CaptureRequest.ProjectionMode);
    }
    else
    {
        Plans.Add(PinWrightCameraFrame::FPlannedShot{
            0.0f, 20.0f, CaptureRequest.ProjectionMode });
    }

    const bool bMeasureCoverage = Ctx.GetBool(TEXT("measureCoverage"), true);
    constexpr int64 MaxMeshCaptureFramePixels = 16ll * 1024ll * 1024ll;
    constexpr int64 MaxMeshCaptureRequestPixels = 64ll * 1024ll * 1024ll;
    const int64 FramePixels = static_cast<int64>(CaptureRequest.Width)
        * static_cast<int64>(CaptureRequest.Height);
    const int64 RenderCount = static_cast<int64>(Plans.Num())
        * (bMeasureCoverage ? 2ll : 1ll);
    if (FramePixels > MaxMeshCaptureFramePixels
        || FramePixels * RenderCount > MaxMeshCaptureRequestPixels)
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
            FString::Printf(TEXT("mesh capture budget exceeded: %lld pixels per frame, %lld "
                "rendered pixels requested; limits are %lld and %lld"),
                FramePixels, FramePixels * RenderCount,
                MaxMeshCaptureFramePixels, MaxMeshCaptureRequestPixels));
        return true;
    }

    TUniquePtr<PinWrightMeshPreviewCapture::FMeshCaptureSession> Session =
        PinWrightMeshPreviewCapture::FMeshCaptureSession::Create(
            AssetPath, CaptureRequest.PreviewSceneRig, ErrorCode, ErrorMessage);
    if (!Session)
    {
        Ctx.SendError(ErrorCode, ErrorMessage);
        return true;
    }

    TArray<PinWrightMeshPreviewCapture::FMeshCaptureOutput> Captures;
    Captures.Reserve(Plans.Num());
    for (int32 Index = 0; Index < Plans.Num(); ++Index)
    {
        PinWrightMeshPreviewCapture::FMeshCaptureRequest Request;
        Request.AssetPath = AssetPath;
        Request.Capture = CaptureRequest;
        Request.bUseOrbitPose = !CaptureRequest.bLocationProvided
            && !CaptureRequest.bRotationProvided;
        Request.bAutoFrameOrthoWidth = !Payload.IsValid()
            || (!Payload->HasField(TEXT("orthoWidth"))
                && !Payload->HasField(TEXT("orthoWorldWidth")));
        Request.OrbitAzimuth = Plans[Index].Azimuth;
        Request.OrbitElevation = Plans[Index].Elevation;
        Request.Padding = Padding;
        Request.bMeasureCoverage = bMeasureCoverage;
        if (Plans.Num() > 1 && !CaptureRequest.Filename.IsEmpty())
        {
            Request.Capture.Filename = FString::Printf(TEXT("%s_%03d.png"),
                *FPaths::GetBaseFilename(CaptureRequest.Filename), Index + 1);
        }

        PinWrightMeshPreviewCapture::FMeshCaptureOutput Capture;
        if (!Session->Capture(Request, Capture, ErrorCode, ErrorMessage))
        {
            Ctx.SendError(ErrorCode, ErrorMessage);
            return true;
        }
        Captures.Add(MoveTemp(Capture));
    }

    // Disposal is part of the response contract, so destroy the RPC-owned preview scene before
    // serialising any `previewScene.disposed:true` receipt.
    Session.Reset();
    for (PinWrightMeshPreviewCapture::FMeshCaptureOutput& Capture : Captures)
    {
        Capture.Capture.bPreviewSceneRigDisposable = true;
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    AddCaptureFields(Captures[0].Capture, Captures[0].ResolvedRequest, Result);
    PinWrightFlatRegion::AddFlatRegionStatsFields(
        Captures[0].FlatRegion, Result->GetObjectField(TEXT("imageStats")));
    Result->SetStringField(TEXT("assetPath"), Captures[0].AssetPath);
    Result->SetStringField(TEXT("assetClass"), Captures[0].AssetClass);
    Result->SetBoolField(TEXT("assetEditorOpened"), false);
    Result->SetBoolField(TEXT("activeWorldUsed"), false);
    if (Captures[0].SubjectCoverage.IsSet())
    {
        Result->SetNumberField(TEXT("subjectCoverage"),
            Captures[0].SubjectCoverage.GetValue());
    }

    if (Captures.Num() > 1)
    {
        TArray<TSharedPtr<FJsonValue>> Shots;
        Shots.Reserve(Captures.Num());
        for (int32 Index = 0; Index < Captures.Num(); ++Index)
        {
            TSharedPtr<FJsonObject> Shot = MakeShared<FJsonObject>();
            AddCaptureFields(Captures[Index].Capture, Captures[Index].ResolvedRequest, Shot);
            PinWrightFlatRegion::AddFlatRegionStatsFields(
                Captures[Index].FlatRegion, Shot->GetObjectField(TEXT("imageStats")));
            if (Captures[Index].SubjectCoverage.IsSet())
            {
                Shot->SetNumberField(TEXT("subjectCoverage"),
                    Captures[Index].SubjectCoverage.GetValue());
            }
            TSharedPtr<FJsonObject> Angle = MakeShared<FJsonObject>();
            Angle->SetNumberField(TEXT("azimuth"), Plans[Index].Azimuth);
            Angle->SetNumberField(TEXT("elevation"), Plans[Index].Elevation);
            Shot->SetObjectField(TEXT("angle"), Angle);
            Shots.Add(MakeShared<FJsonValueObject>(Shot));
        }
        Result->SetNumberField(TEXT("count"), Captures.Num());
        Result->SetArrayField(TEXT("shots"), Shots);
    }
    if (bHasCount || bHasViews)
    {
        PinWrightCameraFrame::FShotDistributionPlan DistributionPlan;
        DistributionPlan.Distribution = Distribution;
        DistributionPlan.bSeeded = bSeedProvided;
        DistributionPlan.Seed = Seed;
        DistributionPlan.AzimuthOffsetDegrees = AzimuthOffsetDegrees;
        DistributionPlan.bAppliesToPlan = bHasCount;
        DistributionPlan.bElevationProvided = bElevationProvided;
        DistributionPlan.ElevationDegrees = PlanElevation;
        DistributionPlan.PlannedShots = Plans.Num();
        Result->SetObjectField(TEXT("shotDistribution"),
            PinWrightCameraFrame::MakeShotDistributionObject(DistributionPlan));
    }
    Ctx.SendSuccess(Result);
    return true;
}

// ---- render.capture_asset_preview ----
REGISTER_RPC_HANDLER("render.capture_asset_preview", "render", "Open an asset editor preview viewport and capture it as an exact-size PNG without spawning the asset into the level. Serves every asset kind the capture-subject registry resolves - Static Mesh, Skeletal Mesh, animation asset and Niagara system - and captures either one framed still or a whole set (count, views:'sides', distribution:'sphere').",
    RPC_PARAMS(
        // OPTIONAL rather than required now that `subject` can stand in for it. The dispatcher's
        // required-param gate can only speak for ONE slot and there are two ways to name the
        // asset, so the "no asset at all" refusal moved into the body -- same INVALID_ARGUMENT
        // code the direct-invocation tests have always asserted, which the wire path used to
        // disagree with by answering MISSING_REQUIRED_PARAM.
        RPC_PARAM_OPT("assetPath", "path", "Asset object path, e.g. /Engine/BasicShapes/Cube.Cube. The legacy spelling of subject.path - pass this OR subject, not a different asset in each."),
        RPC_PARAM_OPT("subject", "object",
            "What to capture, as an object: {kind, path, animation, closeAfterCapture}. `kind` is one of "
            "staticMesh|skeletalMesh|animation|niagara and is INFERRED when omitted from the loaded asset's class; "
            "a payload naming two identifying keys is refused with both key names in the message. `animation` "
            "names an animation asset to load onto a skeletalMesh subject so it has a time axis. `world` and "
            "`actor` are REFUSED here - this verb captures an asset editor preview, so use "
            "render.capture_open_level or camera.frame_actor / camera.orbit_shots for those. The `subject` "
            "response block is present only when a subject was actually resolved."),
        RPC_PARAM_OPT("filename", "filepath", "Output filename inside Saved/Screenshots/AssetPreview. The .png extension is appended if missing. On a multi-shot call (count / views) it is used as the STEM and each shot appends its index and angles, because one name for N shots would leave one PNG on disk and N entries in the response."),
        RPC_PARAM_OPT("target", "string", "Capture target. v1 supports 'preview' only."),
        RPC_PARAM_OPT("width", "number", "Output width in pixels. Default 768."),
        RPC_PARAM_OPT("height", "number", "Output height in pixels. Default 768."),
        RPC_PARAM_OPT("location", "object", "Preview camera location {x, y, z}."),
        RPC_PARAM_OPT("rotation", "object", "Preview camera rotation {pitch, yaw, roll}."),
        RPC_PARAM_OPT("projectionMode", "string", "'perspective' (default) or 'orthographic'."),
        RPC_PARAM_OPT("fov", "number", "Perspective field of view in degrees. Default 50."),
        FParamSpec{TEXT("orthoWidth"), TEXT("number"),
            TEXT("Orthographic frame width in WORLD CENTIMETRES (the world span the image covers left to right). Default 2000. Alias: orthoWorldWidth."),
            false, TEXT("2000"), TArray<FString>({TEXT("orthoWorldWidth")})},
        RPC_PARAM_OPT("exposure", "object|number", PINWRIGHT_EXPOSURE_PARAM_DESC),
        RPC_PARAM_OPT("rejectBlank", "boolean", "Fail with BLANK_CAPTURE (after one redraw retry) instead of returning a near-uniform black frame. Defaults to FALSE, unlike render.capture_open_level where the gate is always on. It does NOT catch a frame crushed by an over-driven exposure pin: such a frame is drawn and non-blank -- read the `crushed` / `blownOut` verdicts and `imageStats.toneLevelsUsed` for that. Contradicts allowBlank:true, which is refused rather than silently disarming the gate."),
        // DECLARED as of the subject-convergence wave. It was read by the shared
        // ParseViewportCaptureRequest (PreviewViewportCaptureUtils.cpp:1279) and branched on by the
        // rejectBlank contradiction check below, while being absent from this list -- so the
        // dispatcher's unknown-param gate refused every wire call that carried it and the branch
        // was dead on the wire. Declaring it is what makes the contradiction refusal reachable by
        // the caller it was written for.
        RPC_PARAM_OPT("allowBlank", "boolean", "Accept an intentionally near-uniform black frame. On THIS verb the blank gate is off by default, so allowBlank changes nothing on its own; it exists so a caller can state the intent explicitly and so rejectBlank:true + allowBlank:true is REFUSED rather than one of them silently winning. It does not affect the crushed / blownOut verdicts, which are a different signal."),
        RPC_PARAM_DEF("measureCoverage", "boolean", "Draw every shot TWICE -- once with the subject hidden -- and report `subjectCoverage`, the fraction of the frame's pixels the subject actually changes. Defaults to TRUE. It is the only published signal that catches a frame containing NOTHING: over pure backdrop `boundsInFrame`, `blank` and `litPixelFraction` all read healthy, because geometrically the bounds are still in frame and the backdrop really is lit. Supported on EVERY subject kind this verb serves -- staticMesh, skeletalMesh, animation and niagara -- because the reference frame hides only the preview component carrying the asset you named, leaving the preview scene's floor, sky and lights in place; what changed between the two frames IS the subject. Costs one extra draw plus readback per shot -- roughly double the capture time for a set, and two full-size pixel buffers held at once -- which is the reason it can be turned off; pass false when you want the pictures and not the measurement. If the preview component cannot be identified the differential does not run and `subjectCoverage` is ABSENT -- never 0, which would be a measurement claim nobody made.", "true"),
        RPC_PARAM_OPT("viewMode", "string", PINWRIGHT_VIEW_MODE_PARAM_DESC),
        RPC_PARAM_OPT("previewScene", "object", PINWRIGHT_PREVIEW_SCENE_PARAM_DESC),
        RPC_PARAM_OPT("closeAfterCapture", "boolean", "Close the asset editor after the capture. Defaults to TRUE, which closes only a window this call opened; pass true explicitly to close one that was already open, or false to leave it open. The close is QUEUED onto the next editor tick rather than run inside this call - destroying an asset editor toolkit on the capture stack crashes the editor - so the response reports assetEditorClosed:false with assetEditorCloseDeferred:true, and the window is gone a tick later. With false, the window is kept only until the next capture opens one: at most one capture-opened asset editor is left open at a time."),
        RPC_PARAM_OPT("count", "integer", "Capture a SET of `count` shots orbiting the asset's bounds instead of one still. Spread by `distribution`. Cannot be combined with `views`, nor with an explicit location/rotation - those name ONE camera."),
        RPC_PARAM_OPT("views", "string", "Canned shot plan. 'sides' captures the six axis-aligned views (front/back/left/right/top/bottom), orthographic unless projectionMode says otherwise. Cannot be combined with count."),
        RPC_PARAM_OPT("elevation", "number", "Elevation in degrees above the horizon for `count` shots (default 30). THREE constructions cannot honour it, and each says so in shotDistribution -- `elevationIgnored`, `elevationIgnoredReason` and a warning naming the remedy -- rather than dropping it silently. (1) ORTHOGRAPHIC: an orthographic editor viewport renders only the six cardinal directions, because FEditorViewportClient::CalcSceneView builds the ortho view matrix from the viewport TYPE and ignores the camera rotation (UE 5.8 EditorViewportClient.cpp:1341-1401), so every shot is snapped to the nearest axis before the camera is placed -- |elevation| below 45 flattens to 0, 45 or more goes to +/-90, and THE DEFAULT OF 30 IS SUBJECT TO THIS TOO. Pass projectionMode:'perspective' to hold an elevation. (2) distribution:'sphere', which derives every elevation from the shot index. (3) `views`, which names its own poses. The value the plan was built from is published as shotDistribution.elevationRequestedDegrees on every ring plan, supplied or not, and what each camera actually got is in shots[].angle.elevation beside shots[].angle.requestedElevation."),
        RPC_PARAM_DEF("distribution", "string", "How `count` spreads its shots. 'ring' (default) puts them at evenly-spaced azimuths on ONE horizontal circle at `elevation`, which never sees the top or the underside. 'sphere' uses a golden-angle (Fibonacci) spiral over the whole viewing sphere - deterministic, near-optimal even coverage for any N - and IGNORES `elevation`. Only affects `count`; `views` names its own poses.", "ring"),
        RPC_PARAM_OPT("seed", "number", "Jitter the shot distribution by a seeded azimuth offset. Omitted, output is deterministic (offset 0); supplied, output is STILL deterministic and the seed is echoed in shotDistribution so any set can be retaken exactly."),
        RPC_PARAM_OPT("time", "number", "Single instant in SECONDS to drive the subject to before capturing. Shorthand for times:[t]. Refused with the subject's own typed reason when subject.timeSupported is false - a Static Mesh has no time axis, and a Skeletal Mesh has none until subject.animation names one."),
        RPC_PARAM_OPT("times", "array", "Instants in SECONDS, e.g. [0, 0.25, 0.5]. Each instant is captured from every camera the shot plan names, so the set is instants x cameras and the combined total is bounded by the same 24-shot ceiling as count/views. The subject is simulated or scrubbed ONCE PER INSTANT and held there while that instant's cameras fire, so the angles of one instant show the same moment - re-driving per shot would resimulate it, and a Niagara system reseeds on every reset. For a Niagara system the window the provider offers is published as subject.timeStartSeconds / subject.timeEndSeconds (0 to 2 s by default); an instant outside it is still simulated, it is just past what the provider calls the preview window. For a frame burst over an animation at the animation's own sampling rate, with per-instant pose-change proof, use render.capture_animation_preview instead."),
        PinWright::MaterialShaderState::AllowFallbackParamSpec()
    ))
{
    using namespace PinWrightCameraFrame;

    const TSharedPtr<FJsonObject>& Payload = Ctx.GetRawPayload();
    const bool bHasPayload = Payload.IsValid();
    const bool bAllowFallback = Ctx.GetBool(
        PinWright::MaterialShaderState::AllowFallbackParamName(), false);

    // ---- which asset, in either spelling ----
    // `assetPath` and `subject.path` name the same thing (plan §2.1), so they are collapsed here.
    // Read in the verb rather than left to ParseSubject because the three refusals that follow --
    // no path, an unloadable path, and a class this verb does not serve -- are the ones the
    // regression floor asserts by CODE, and all three have to happen before anything is opened.
    FString AssetPath = Ctx.GetString(TEXT("assetPath"));
    if (const TSharedPtr<FJsonObject> SubjectObject = Ctx.GetObject(TEXT("subject")))
    {
        FString SubjectPath;
        if (SubjectObject->TryGetStringField(TEXT("path"), SubjectPath) && !SubjectPath.IsEmpty())
        {
            AssetPath = SubjectPath;
        }
        // A LEVEL kind is refused HERE, ahead of the missing-path refusal below, because it is the
        // more specific answer: subject:{kind:"actor", name:"..."} carries no path, so the generic
        // "assetPath required" would be technically true and useless -- the caller does not need a
        // path, they need the other verb.
        FString SubjectKind;
        if (SubjectObject->TryGetStringField(TEXT("kind"), SubjectKind))
        {
            const FString KindKey = SubjectKind.ToLower();
            if (KindKey == TEXT("world") || KindKey == TEXT("actor"))
            {
                Ctx.SendError(ErrorCodes::ERR_UNSUPPORTED_ASSET_EDITOR,
                    FString::Printf(
                        TEXT("render.capture_asset_preview captures an asset editor preview, so ")
                        TEXT("subject.kind '%s' has no viewport here. Use render.capture_open_level for ")
                        TEXT("the open level, or camera.frame_actor / camera.orbit_shots for a placed ")
                        TEXT("actor."),
                        *KindKey));
                return true;
            }
        }
    }
    if (AssetPath.IsEmpty())
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
            TEXT("assetPath required (or subject: {path: \"/Game/...\"}, the same thing spelled the "
                 "new way). This verb captures an ASSET EDITOR preview: for the open level use "
                 "render.capture_open_level, and for a placed actor camera.frame_actor."));
        return true;
    }

    FString Target = Ctx.GetString(TEXT("target"), TEXT("preview")).ToLower();
    if (Target.IsEmpty())
    {
        Target = TEXT("preview");
    }
    if (Target != TEXT("preview"))
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT, TEXT("target must be 'preview'"));
        return true;
    }

    PinWrightRenderCapture::FViewportCaptureRequest Request;
    FString ErrorCode;
    FString ErrorMessage;
    if (!PinWrightRenderCapture::ParseViewportCaptureRequest(
        Ctx.GetRawPayload(), Request, ErrorCode, ErrorMessage))
    {
        Ctx.SendError(ErrorCode, ErrorMessage);
        return true;
    }

    // ---- two parameters the shared parser reads that this verb does NOT offer ----
    //
    // ParseViewportCaptureRequest is shared with render.capture_open_level and reads
    // `viewDistanceScale` (:1316) and `hideEditorSprites` (:1337) off any payload. Neither is in
    // this verb's RPC_PARAMS, so the dispatcher's unknown-param gate already refuses them on the
    // wire with UNKNOWN_PARAMS naming the valid list -- but a DIRECT handler invocation (every
    // automation test takes that path) still reaches the parser, so the fields are cleared here.
    // Behaviour, schema and the shared header's comment then agree on every path instead of three
    // ways, which is the actual defect: a parameter half-live on one path and refused on the other.
    //
    // WHY NEITHER IS OFFERED, measured against C:\UE_5.8\Engine rather than taken from the comment:
    //
    // `hideEditorSprites` clears EngineShowFlags.BillboardSprites, and exactly three component
    // classes gate their draw relevance on that flag -- UArrowComponent (ArrowComponent.cpp:170),
    // UBillboardComponent (BillboardComponent.cpp:214) and UMaterialBillboardComponent
    // (MaterialBillboardComponent.cpp:249). The scenes this verb captures contain none of them:
    // FPreviewScene registers only a UDirectionalLightComponent, a USkyLightComponent and a
    // ULineBatchComponent (PreviewScene.cpp:84-97) and FAdvancedPreviewScene adds a sky mesh, a
    // post-process component and a floor mesh (AdvancedPreviewScene.cpp:63-93), with NO SpawnActor
    // anywhere -- and both the Static Mesh editor viewport (SStaticMeshEditorViewport.cpp:143,
    // :489) and the Niagara system viewport (SNiagaraSystemViewport.cpp:871) build exactly that
    // scene and only AddComponent into it. Niagara's own renderers never read the flag either, so a
    // particle capture has nothing to gain and, more importantly, nothing to lose. The one partial
    // exception is Persona, whose preview world DOES hold actors -- AAnimationEditorPreviewActor
    // (PersonaToolkit.cpp:163, no sprite components) and, only after the artist toggles wind on in
    // the viewport, an AWindDirectionalSource whose editor-only UArrowComponent
    // (bTreatAsASprite=true) and UBillboardComponent do gate on the flag
    // (AnimationEditorPreviewScene.cpp:1121-1180). So the shared header's REASON ("no AActor
    // anywhere") is inaccurate for Persona while its CONCLUSION holds for every default preview:
    // the only thing the parameter could ever hide here is a user-toggled wind gizmo, never the
    // light bulbs / audio icons / player start the parameter documents.
    //
    // `viewDistanceScale` forces the GLOBAL r.ViewDistanceScale cvar for the duration of a capture
    // so distant LEVEL content is not culled out of the frame. A preview scene has no distance-
    // culled content to un-cull (its components carry no LDMaxDrawDistance), which is the same
    // reason FViewportCaptureRequest::bAutoViewDistanceScale is documented as capture_open_level
    // only. Offering it here would mutate editor-wide state for no picture change. Nothing is
    // hidden by refusing it: AddCaptureFields publishes the `viewDistance` block unconditionally,
    // so the response still reports whatever scale the frame was rendered at.
    Request.bHideEditorSprites = false;
    Request.ViewDistanceScale = 0.0f;
    Request.bViewDistanceScaleProvided = false;
    Request.bAutoViewDistanceScale = false;

    // ---- the blank gate is reachable from this verb, and it is OFF by default ----
    //
    // WHY IT IS REACHABLE AT ALL. Until now `bRejectBlankCapture` was set at exactly one site --
    // render.capture_open_level -- so this verb computed `imageStats` and `blank` and then ignored
    // its own verdict: a frame that DID trip the classifier still came back a clean success
    // (B-exposure-pin-black-frame, finding 3). Reporting a verdict a verb refuses to act on is the
    // gap; making it settable closes it.
    //
    // WHY IT IS NOT ON BY DEFAULT, unlike render.capture_open_level. That verb renders the user's
    // real level, where an all-black frame is always a failure. This one renders an
    // FAdvancedPreviewScene holding ONE asset, where a black frame can be the correct picture: an
    // unlit material, a Niagara system between bursts, or a mesh smaller than a pixel at the
    // requested framing. Arming the gate by default would turn every such caller's legitimate PNG
    // into a hard BLANK_CAPTURE without notice.
    //
    // WHAT THIS PARAGRAPH USED TO SAY, AND WHY IT IS GONE. It claimed the same preview scene
    // "captured from a headless -unattended editor is measured several times dimmer and sometimes
    // rendering black altogether". That is not true and never was: every reading behind it came
    // from three captures that all wrote to ONE file, because the auto-generated filename carried a
    // one-second timestamp and a capture takes ~60 ms. Fixed in commit bcc334e8; re-measured the
    // same day, the fixture reads meanLuminance 0.3644 in the automation commandlet and 0.3644
    // interactively. There is no dim mode and no black mode to defend against, so the gate's
    // default rests on the argument above instead.
    //
    // AND IT IS NOT THE FIX FOR THE CRUSHED FRAME. A frame crushed by an over-driven exposure pin
    // keeps the editor overlay's lit pixels, so it does not trip `bBlank` at all -- the tone-range
    // verdicts (`crushed` / `blownOut`) are what catch it, and they warn rather than reject
    // because a dark frame can be deliberate. The two signals are deliberately disjoint.
    const bool bRejectBlankRequested = Ctx.GetBool(TEXT("rejectBlank"), false);
    if (bRejectBlankRequested && Request.bAllowBlank)
    {
        // Refused rather than resolved by precedence: `allowBlank: true` would disarm the gate the
        // same call just asked for, and a caller who wrote both believes one of them is in force.
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
            TEXT("rejectBlank:true and allowBlank:true contradict each other: the first arms the "
                 "blank-frame gate and the second disarms it. Pass rejectBlank:true to fail on a "
                 "near-uniform black frame, or neither to accept one (the default for this verb). "
                 "Neither parameter catches a frame crushed by an over-driven exposure pin -- read "
                 "the `crushed` / `blownOut` verdicts for that."));
        return true;
    }
    Request.bRejectBlankCapture = bRejectBlankRequested;

    if (!GEditor)
    {
        Ctx.SendError(ErrorCodes::ERR_EDITOR_NOT_AVAILABLE, TEXT("Editor not available"));
        return true;
    }

    UObject* Asset = LoadObject<UObject>(nullptr, *AssetPath);
    if (!Asset)
    {
        Ctx.SendError(ErrorCodes::ERR_ASSET_NOT_FOUND, FString::Printf(TEXT("Asset not found: %s"), *AssetPath));
        return true;
    }
    if (!PinWrightRenderSubject::AssetClassIsServed(Asset))
    {
        // Name the sibling verbs. Without that pointer this rejection reads as "isolated preview
        // capture is impossible for this asset", which is what sent one review pass down the
        // spawn-into-a-scratch-level route and dirtied a shared map. The old wording said "Static
        // Mesh asset editors only", which is no longer true; what stays load-bearing -- and is
        // asserted by Tests/Render/TestAnimationCaptureHandlers.cpp -- is that the message still
        // names render.capture_animation_preview, the other half of a two-way pointer.
        //
        // A material or a texture gets its own clause for the same reason, and it is the one
        // refused class whose answer is a DIFFERENT namespace: asset.generate_thumbnail renders it
        // offscreen with no asset editor, no level and no viewport. Nothing else pointed there, so
        // the natural workaround was to bind the material to a mesh and capture THAT -- a served
        // kind, so it is accepted, and it drags the caller through this verb's asset-editor
        // open/close path for a picture that never needed one.
        const FString ThumbnailClause = (Asset->IsA<UMaterialInterface>() || Asset->IsA<UTexture>())
            ? FString(TEXT("For a material or a texture, asset.generate_thumbnail renders one "
                           "offscreen to a file and opens no asset editor. "))
            : FString();
        Ctx.SendError(ErrorCodes::ERR_UNSUPPORTED_ASSET_EDITOR,
            FString::Printf(
                TEXT("render.capture_asset_preview captures the asset editor preview of a Static Mesh, ")
                TEXT("Skeletal Mesh, animation asset or Niagara system ('%s' is a %s, which no capture ")
                TEXT("subject kind serves). %sFor a skinned asset reviewed as a frame burst over an ")
                TEXT("animation, render.capture_animation_preview drives the Persona preview viewport ")
                TEXT("directly. For a placed actor use camera.frame_actor or camera.orbit_shots, and for ")
                TEXT("the open level render.capture_open_level."),
                *AssetPath, *Asset->GetClass()->GetName(), *ThumbnailClause));
        return true;
    }

    const bool bDebugMaterialSubstitutionRequested = Request.ViewMode.WantsOverride() &&
        PinWrightRenderCapture::IsDebugMaterialSubstitutionMode(Request.ViewMode.ViewMode);
    bool bSubjectIsNanite = false;
    if (const UStaticMesh* SubjectMesh = Cast<UStaticMesh>(Asset))
    {
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 7, 0)
        bSubjectIsNanite = SubjectMesh->GetNaniteSettings().bEnabled;
#else
        bSubjectIsNanite = SubjectMesh->NaniteSettings.bEnabled;
#endif
    }
    // These debug modes replace the subject's materials. Nanite is the measured exception: its
    // static relevance bypasses that substitution, so its real material still reaches the frame.
    const bool bCaptureUsesSubjectMaterials =
        !bDebugMaterialSubstitutionRequested || bSubjectIsNanite;

    // ---- the wire subject, normalised ----
    // ParseSubject owns the `subject` object, the legacy `assetPath` spelling and the
    // ambiguous-payload refusal; the verb owns which kinds it publishes. The class gate above ran
    // first so a class no kind serves gets this verb's message rather than the resolver's.
    PinWrightCaptureSubject::FSubjectRequest SubjectRequest;
    if (!PinWrightCaptureSubject::ParseSubject(Payload, SubjectRequest, ErrorCode, ErrorMessage))
    {
        Ctx.SendError(ErrorCode, ErrorMessage);
        return true;
    }
    if (SubjectRequest.Kind == PinWrightCaptureSubject::ESubjectKind::World ||
        SubjectRequest.Kind == PinWrightCaptureSubject::ESubjectKind::Actor)
    {
        // Reachable only through an explicit subject:{kind:"world"|"actor"}. Refused rather than
        // served, for the reason plan §4.3 gives about domain-named verbs: this one opens an ASSET
        // EDITOR, and answering a level question here would make the name lie.
        Ctx.SendError(ErrorCodes::ERR_UNSUPPORTED_ASSET_EDITOR,
            FString::Printf(
                TEXT("render.capture_asset_preview captures an asset editor preview, so subject.kind '%s' ")
                TEXT("has no viewport here. Use render.capture_open_level for the open level, or ")
                TEXT("camera.frame_actor / camera.orbit_shots for a placed actor."),
                PinWrightCaptureSubject::ToWireName(SubjectRequest.Kind)));
        return true;
    }
    // The path this verb resolved is authoritative for the response, so `subject.path` and the
    // legacy `assetPath` cannot disagree in what gets reported back.
    SubjectRequest.AssetPath = AssetPath;

    // ---- closeAfterCapture: three states, and the top-level spelling must not be swallowed ----
    //
    // The DEFAULT is "close". Leaving a real asset editor window open on every capture is not
    // something a caller would choose, and it is not merely untidy: an asset editor still open when
    // the editor exits crashes UE 5.8 during shutdown -- ~FStaticMeshEditor (StaticMeshEditor.cpp
    // :271) calls GEditor->GetEditorSubsystem<UImportSubsystem>()->OnAssetReimport.RemoveAll(this)
    // unguarded, long after the subsystems are gone, and faults reading 0x78 (docs/lessons.md).
    // A window the CALLER already had open is theirs, so the default closes only what this call
    // opened; an EXPLICIT closeAfterCapture:true closes it either way, because the old guard
    // silently did nothing on the second capture of the same mesh. All three states are carried by
    // FSubjectRequest::bCloseAfterCapture + bCloseAfterCaptureProvided and executed by the provider
    // (CaptureSubjectProviders_Mesh.cpp -> CloseAssetEditor), so this verb does not close anything
    // itself and reports what the provider measured.
    //
    // The one thing that IS this verb's business: `closeAfterCapture` is a TOP-LEVEL parameter here,
    // and ParseSubject reads its fields out of the nested `subject` object whenever one is present.
    // A caller writing {subject:{path:...}, closeAfterCapture:true} would otherwise have it silently
    // dropped -- a declared parameter that changes nothing, which is the exact defect class this
    // wave is clearing. The top-level spelling therefore overrides, and marks itself explicit.
    if (bHasPayload && Payload->HasField(TEXT("closeAfterCapture")))
    {
        SubjectRequest.bCloseAfterCapture = Ctx.GetBool(TEXT("closeAfterCapture"), true);
        SubjectRequest.bCloseAfterCaptureProvided = true;
    }

    // ---- shot plan: one still (the historical shape) or a whole set ----
    const bool bViewsProvided = bHasPayload && Payload->HasField(TEXT("views"));
    const FString Views = Ctx.GetString(TEXT("views")).ToLower();
    const bool bCountProvided = bHasPayload && Payload->HasField(TEXT("count"));
    // TWO booleans where there used to be one, and they are not interchangeable. `bCameraSet` is
    // what makes location/rotation contradictory - those name ONE camera and count/views name
    // several. A TIME SERIES does not: a fixed camera watching a system evolve is exactly the shot
    // a caller wants, so folding instants into that refusal would have banned the most useful
    // shape this change adds. `bMultiShot` is the RESPONSE shape - shots[] rather than one still -
    // and a series of instants is a set however many cameras it used.
    const bool bCameraSet = bViewsProvided || bCountProvided;
    const bool bProjectionProvided = bHasPayload && Payload->HasField(TEXT("projectionMode"));
    const bool bOrthoWidthProvided = bHasPayload &&
        (Payload->HasField(TEXT("orthoWidth")) || Payload->HasField(TEXT("orthoWorldWidth")));
    const float DefaultElevation = static_cast<float>(Ctx.GetNumber(TEXT("elevation"), 30.0));
    const bool bElevationProvided = bHasPayload && Payload->HasField(TEXT("elevation"));
    const bool bCallerSizeProvided = bHasPayload &&
        (Payload->HasField(TEXT("width")) || Payload->HasField(TEXT("height")));
    // The margin this verb has always framed with. Deliberately NOT camera.orbit_shots' 1.15: the
    // single-still framing below has shipped at 1.25 since the verb existed, and a set framed
    // tighter than the still it sits beside is a difference nobody asked for.
    constexpr float AssetPreviewPadding = 1.25f;

    // ---- the time axis: instants in SECONDS, parsed before anything is opened ----
    //
    // WHY SECONDS AND NOT FRAMES. render.capture_animation_preview counts in frames because an
    // animation asset carries a sampling rate to count them against. A Niagara system carries no
    // such rate and no authored duration at all - it is a simulation advanced by a tick delta - so
    // frames here would be a unit invented by this verb. Seconds are what every provider's setter
    // already takes (FSubjectTimeSetter's parameter is `double TimeSeconds`).
    //
    // Parsed HERE, beside the shot plan and ahead of the resolve, so a malformed `times` costs the
    // caller no asset-editor open.
    TArray<double> Instants;
    const bool bTimeProvided = bHasPayload && Payload->HasField(TEXT("time"));
    const bool bTimesProvided = bHasPayload && Payload->HasField(TEXT("times"));
    if (bTimeProvided && bTimesProvided)
    {
        // Refused rather than ranked, like every other two-spellings-of-one-thing payload on this
        // verb: a caller who wrote both believes one of them is in force.
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
            TEXT("time and times name the same axis: `time` is shorthand for a one-entry `times`. "
                 "Pass one of them, not both."));
        return true;
    }
    if (bTimesProvided)
    {
        const TArray<TSharedPtr<FJsonValue>>* TimesArray = nullptr;
        if (!Payload->TryGetArrayField(TEXT("times"), TimesArray) || TimesArray == nullptr)
        {
            Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
                TEXT("times must be an array of seconds, e.g. times:[0, 0.25, 0.5]. Use `time` for a "
                     "single instant."));
            return true;
        }
        Instants.Reserve(TimesArray->Num());
        for (int32 EntryIndex = 0; EntryIndex < TimesArray->Num(); ++EntryIndex)
        {
            const TSharedPtr<FJsonValue>& Entry = (*TimesArray)[EntryIndex];
            double Seconds = 0.0;
            if (!Entry.IsValid() || !Entry->TryGetNumber(Seconds))
            {
                Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
                    FString::Printf(
                        TEXT("times[%d] is not a number. Every entry is an instant in seconds."),
                        EntryIndex));
                return true;
            }
            // Non-finite is refused here rather than left to the provider: NaN would floor to a
            // garbage tick count inside AdvanceSimulation and photograph an arbitrary frame.
            if (!FMath::IsFinite(Seconds) || Seconds < 0.0)
            {
                Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
                    FString::Printf(
                        TEXT("times[%d] is %g. An instant must be a finite number of seconds at or "
                             "after 0 - no subject kind here has a state before its own start."),
                        EntryIndex, Seconds));
                return true;
            }
            Instants.Add(Seconds);
        }
        if (Instants.Num() == 0)
        {
            // An empty array is refused rather than treated as "no time axis": the caller wrote the
            // key, so silently capturing at whatever instant the editor was already showing would
            // answer a question they did not ask.
            Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
                TEXT("times is empty. Name at least one instant in seconds, or drop the key to "
                     "capture the subject wherever its preview already is."));
            return true;
        }
    }
    else if (bTimeProvided)
    {
        const double Seconds = Ctx.GetNumber(TEXT("time"), 0.0);
        if (!FMath::IsFinite(Seconds) || Seconds < 0.0)
        {
            Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
                FString::Printf(
                    TEXT("time is %g. An instant must be a finite number of seconds at or after 0."),
                    Seconds));
            return true;
        }
        Instants.Add(Seconds);
    }
    const bool bTimeAxis = Instants.Num() > 0;
    // A set in the response sense: shots[] rather than a single still. One instant on one camera
    // stays the historical single-still shape, so an existing caller's response does not grow an
    // array because they asked for one moment.
    const bool bMultiShot = bCameraSet || Instants.Num() > 1;

    if (bCameraSet && (Request.bLocationProvided || Request.bRotationProvided))
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
            TEXT("location/rotation name ONE camera and count/views name a SET; pass one or the "
                 "other. Drop location/rotation to orbit the asset's bounds, or drop count/views "
                 "to place a single free camera."));
        return true;
    }

    EShotDistribution Distribution = EShotDistribution::Ring;
    {
        const FString DistributionArg = Ctx.GetString(TEXT("distribution"), TEXT("ring")).ToLower();
        if (DistributionArg == TEXT("sphere"))
        {
            Distribution = EShotDistribution::Sphere;
        }
        else if (DistributionArg != TEXT("ring"))
        {
            // Rejected rather than defaulted: a typo that quietly becomes the default is
            // indistinguishable from the default having been asked for.
            Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
                FString::Printf(TEXT("Unrecognised distribution '%s'. Valid: ring (default), sphere."),
                    *DistributionArg));
            return true;
        }
    }
    const bool bSeedProvided = bHasPayload && Payload->HasField(TEXT("seed"));
    const int32 Seed = Ctx.GetInt(TEXT("seed"), 0);
    const double AzimuthOffsetDegrees = bSeedProvided ? SeedToAzimuthOffsetDegrees(Seed) : 0.0;

    TArray<FPlannedShot> Plan;
    if (bViewsProvided)
    {
        if (bCountProvided)
        {
            Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
                TEXT("views cannot be combined with count; pass one shot plan"));
            return true;
        }
        if (Views != TEXT("sides"))
        {
            Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
                TEXT("views must be 'sides' (the six axis-aligned views); use count for arbitrary poses"));
            return true;
        }
        // ONE sides table for every verb (CameraShotPlanUtils.h MakeSideViews), never a fourth
        // copy: a reordered `sides` set is six individually correct images that no longer line up
        // with the archived set they are compared against.
        Plan = MakeSideViews(bProjectionProvided ? Request.ProjectionMode : TEXT("orthographic"));
    }
    else if (bCountProvided)
    {
        const int32 Count = Ctx.GetInt(TEXT("count"), 4);
        if (Count < 1)
        {
            Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT, TEXT("count must be at least 1"));
            return true;
        }
        Plan = (Distribution == EShotDistribution::Sphere)
            ? MakeSphereDistribution(Count, AzimuthOffsetDegrees, Request.ProjectionMode)
            : MakeRingDistribution(Count, DefaultElevation, AzimuthOffsetDegrees, Request.ProjectionMode);
    }
    // The set is instants x cameras, so the ceiling is checked against the PRODUCT and the message
    // names both factors. Checked here rather than left to FPoseListCaptureRequest::MaxPoses,
    // because that one TRUNCATES and reports: a caller who asks for 5 instants of the six sides
    // would get 24 shots covering four instants and a fifth silently dropped, which reads as a
    // complete set of the wrong thing. A refusal that names 30 = 5 x 6 is actionable; a truncated
    // set is not.
    const int32 CameraCount = FMath::Max(Plan.Num(), 1);
    const int32 InstantCount = FMath::Max(Instants.Num(), 1);
    const int32 TotalShots = CameraCount * InstantCount;
    if (TotalShots > GMaxOrbitShots)
    {
        Ctx.SendError(ErrorCodes::ERR_TOO_MANY_SHOTS,
            bTimeAxis
                ? FString::Printf(
                    TEXT("Requested %d shots (%d instants x %d cameras) exceeds the maximum of %d. ")
                    TEXT("Drop instants or cameras; every instant is captured from every camera."),
                    TotalShots, InstantCount, CameraCount, GMaxOrbitShots)
                : FString::Printf(TEXT("Requested %d shots exceeds the maximum of %d"),
                    TotalShots, GMaxOrbitShots));
        return true;
    }

    // ---- resolve, capture, release, then report ----
    //
    // FResolvedSubject owns the provider's release discipline and is non-copyable; its destructor
    // releases if nothing did it first, so every early return below is covered. The success path
    // calls ReleaseSubject EXPLICITLY before building the response, because the release is what
    // closes the asset editor and sets bEditorClosed -- reading that field before the release would
    // report the request rather than the measurement, which is the whole point of publishing it.
    TSharedPtr<FJsonObject> Result;
    PinWright::MaterialShaderState::FCaptureReadiness MaterialReadiness;
    {
        PinWrightCaptureSubject::FResolvedSubject Resolved;
        // BOUND, not dropped. This is the only production call site that reaches the kind registry
        // for an asset subject, so it is the only place a Niagara provider's time setter can ever
        // surface: C5 shipped a complete driver (reset -> AdvanceSimulation -> hold at DesiredAge)
        // that no verb called, because this variable used to be named UnusedTimeSetter and went out
        // of scope. Without a `times` argument the behaviour is byte-identical to before - no pose
        // carries an instant, so the setter is never invoked and no kind sees a refusal.
        PinWrightCaptureSubject::FSubjectTimeSetter TimeSetter;
        if (!PinWrightCaptureSubject::Resolve(SubjectRequest, Resolved, TimeSetter,
                ErrorCode, ErrorMessage))
        {
            Ctx.SendError(ErrorCode, ErrorMessage);
            return true;
        }
        // Stated rather than assumed: the contract says these are never null on success, and a
        // provider that broke it would otherwise crash inside the capture util instead of naming
        // the subject it could not acquire.
        if (Resolved.ViewportClient == nullptr || !Resolved.SceneViewport.IsValid())
        {
            Ctx.SendError(ErrorCodes::ERR_PREVIEW_VIEWPORT_NOT_FOUND,
                FString::Printf(TEXT("The subject resolved but produced no usable preview viewport for "
                                     "'%s', so there is nothing to capture. This is a provider defect, "
                                     "not a bad argument."), *AssetPath));
            return true;
        }

        // ---- a time was asked for and this subject has no axis to put it on ----
        //
        // The refusal is TYPED and it is the PROVIDER'S OWN, harvested rather than written here.
        // Every provider installs a refusing setter on the no-axis path whose message names the
        // kind and the argument that would supply an axis (RefuseNoTimeAxis in
        // CaptureSubjectProviders_Mesh.cpp distinguishes "a Static Mesh has no time axis at all"
        // from "this Skeletal Mesh has none until subject.animation names one", and carries the
        // right error code for each: UNSUPPORTED_ASSET_EDITOR for the first, which is not
        // caller-fixable, and the same code with a fixable instruction for the second). Asking the
        // provider is what stops this verb owning a second, drifting copy of that table - and no
        // new error code is minted, which is the standing rule.
        //
        // Harvest-only: a refusing setter does no work, and the return value is deliberately
        // ignored. `timeSupported:false` is what the caller can see in the response, so the answer
        // must agree with it even if a provider contradicted its own flag.
        if (bTimeAxis && !Resolved.bTimeSupported)
        {
            FString TimeErrCode;
            FString TimeErrMsg;
            if (TimeSetter)
            {
                TimeSetter(Instants[0], TimeErrCode, TimeErrMsg);
            }
            if (TimeErrCode.IsEmpty())
            {
                TimeErrCode = ErrorCodes::ERR_UNSUPPORTED_ASSET_EDITOR;
            }
            if (TimeErrMsg.IsEmpty())
            {
                TimeErrMsg = FString::Printf(
                    TEXT("Subject kind '%s' reports no time axis for '%s', so there is no instant to "
                         "capture it at."),
                    PinWrightCaptureSubject::ToWireName(Resolved.Kind), *AssetPath);
            }
            Ctx.SendError(TimeErrCode, TimeErrMsg + TEXT(" ") + FString::Printf(
                TEXT("render.capture_asset_preview drives `time` / `times` only for a subject whose "
                     "`subject.timeSupported` reads true - a Niagara system always, a skeletalMesh or "
                     "animation subject once an animation is named. For a frame burst over an "
                     "animation use render.capture_animation_preview; for a placed actor driven by a "
                     "Level Sequence use camera.animation_shots.")));
            return true;
        }

        const FVector Center = Resolved.BoundsOrigin;
        const float Radius = FMath::Max(static_cast<float>(Resolved.BoundsRadius), 1.0f);
        // AUTO-FRAMING WITH NO MEASURED BOUNDS IS A GUESS, AND IT MUST SAY SO.
        //
        // Radius above floors at 1.0 so the fit-distance maths cannot divide by zero, and that
        // floor is exactly where a silent mis-frame comes from: a Niagara system whose only
        // emitters are GPU sims with no authored fixed bounds measures nothing
        // (MeasureAndPinBounds returns false, BoundsRadius stays 0), and the camera is then solved
        // 1.25 units from the origin - inside the effect, pointing at nothing. The provider already
        // publishes WHY through subject.boundsWarning; what was missing is the consequence, which
        // only this verb knows because only this verb solved a camera from it.
        //
        // Emitted ONLY when the guess was actually made: bounds were unmeasurable AND the caller
        // named no camera of their own. A caller who passed location/rotation framed it themselves
        // and needs no warning about a default that never ran.
        const bool bFramingIsAGuess = !(Resolved.BoundsRadius > 0.0) && !Request.bLocationProvided;

        PinWrightPoseCapture::FPoseListCaptureRequest PoseRequest;
        // Resolution resolved ONCE for the whole call and never re-read. A capture size that varies
        // within an editor session trips FViewport::GetHitProxy's
        // `ProxyMap.Num() == TestSizeX * TestSizeY` assertion, which has already cost 66 actors and
        // 125 emitters of unsaved level state on this project.
        PoseRequest.Width = Request.Width;
        PoseRequest.Height = Request.Height;
        PoseRequest.FilenamePrefix = FString::Printf(TEXT("%s_AssetPreview"), *Asset->GetName());
        PoseRequest.Subdirectory = TEXT("AssetPreview");
        PoseRequest.Exposure = Request.Exposure;
        PoseRequest.ViewMode = Request.ViewMode;
        // The scoped preview-scene rig, read ONCE for the whole call by the shared parser and
        // copied here for the same reason Exposure and ViewMode are: one wire spelling, one pin,
        // and every shot of a set lit identically. A rig re-read or re-applied per shot would make
        // two frames of one asset incomparable, which is the thing pinning it exists to prevent.
        PoseRequest.PreviewSceneRig = Request.PreviewSceneRig;
        // ---- the Niagara floor default ----
        //
        // A Niagara preview scene ships with a floor plane, and a particle system is normally
        // authored ABOUT the origin - so the floor cuts the effect in half, and for a low camera it
        // occludes it outright. Two captures of SimpleExplosion came back as pure floor with every
        // published signal green.
        //
        // Applied ONLY when the caller named no `previewScene.showFloor` of their own: someone who
        // asked for the floor wants the floor, including as a ground reference for a decal or an
        // impact effect. Nothing here overrides an explicit choice in either direction.
        //
        // WRITTEN ON THIS PER-CAPTURE PIN AND NOWHERE ELSE. Hiding the floor session-wide was tried
        // and reverted: the floor flag lives on a PROCESS-WIDE profile array that FScopedPreviewSceneRig
        // snapshots and restores per shot, and writing it outside that scope put `bShowFloor=false`
        // into a committed config file - the artist's own preview scene, changed permanently by a
        // read-only review verb. The scoped pin gives the same pixels and restores all three levels
        // (components, shared profile array, config) when the shot ends.
        if (Resolved.Kind == PinWrightCaptureSubject::ESubjectKind::Niagara &&
            !Request.PreviewSceneRig.bShowFloorProvided)
        {
            PoseRequest.PreviewSceneRig.bRequested = true;
            PoseRequest.PreviewSceneRig.bShowFloorProvided = true;
            PoseRequest.PreviewSceneRig.bShowFloor = false;
        }
        // Not offered on this verb; see the parser-neutralisation comment above for the engine
        // evidence. Written explicitly so the set-level state says so rather than inheriting it.
        PoseRequest.bHideEditorSprites = false;
        PoseRequest.bRejectBlankCapture = Request.bRejectBlankCapture;
        PoseRequest.bAllowBlank = Request.bAllowBlank;
        // This verb allows the same 24 as camera.orbit_shots rather than the primitive's default
        // of 8, so a `sides` set plus a sphere distribution both fit without silent shortening.
        PoseRequest.MaxPoses = GMaxOrbitShots;
        // Bounds from a STATIC source -- the asset's own, never the posed or simulated subject --
        // which is what makes the per-shot `framing` verdict mean "the subject left the frame"
        // rather than "the bounds moved".
        PoseRequest.BoundsOrigin = Resolved.BoundsOrigin;
        PoseRequest.BoundsRadius = Resolved.BoundsRadius;
        // bWarmupShot stays at the primitive's default of TRUE, and this is the verb it matters
        // most on: it is the one that OPENS the preview window and captures in the same call, and
        // the first capture into a fresh window is measurably about a stop dark (SM_Driftwood mean
        // 0.1987 then 0.3686 at an identical pinned ev100, docs/wiki-src/render.md). The throwaway
        // frame is deleted and reported through poseSet.warmupShotTaken, never assumed.

        // Parallel to PoseRequest.Poses on the multi-shot path: what each pose was ASKED for
        // before the orthographic axis snap, and whether the snap moved it.
        TArray<FPlannedShot> RequestedShots;
        TArray<bool> OrthoAxisSnapped;

        if (Plan.Num() == 0)
        {
            // ---- single still: the historical framing, unchanged ----
            // When the caller omits `location`, ParseViewportCaptureRequest defaults it to the
            // origin -- which sits inside the bounds and frames nothing. Frame to the subject's
            // bounds instead: pull back along -X by a distance derived from the bounding-sphere
            // radius and the (perspective) FOV, look at the centre, and nudge slightly up. The
            // caller's explicit location/rotation always win; this only fills the no-args path.
            // ComputeFitDistance(R, Fov, 1.25) is the same expression this block used inline
            // before the convergence -- max(R/tan(fov/2), R*1.5) * 1.25 -- so no existing caller's
            // pixels move; the only change is that the bounds now come from the resolved subject
            // instead of from a Cast<UStaticMesh>.
            //
            // Scope note: this default framing assumes a perspective projection. For
            // projectionMode="orthographic" the frame is governed by SetOrthoZoom(OrthoWidth), not
            // the camera distance, so a no-args orthographic default is not framed here -- callers
            // must pass orthoWidth/location.
            if (!Request.bLocationProvided)
            {
                const float Distance = ComputeFitDistance(Radius, Request.Fov, AssetPreviewPadding);
                const FVector CameraLocation = Center + FVector(-Distance, 0.0f, Radius * 0.35f);
                Request.Location = CameraLocation;
                if (!Request.bRotationProvided)
                {
                    Request.Rotation = (Center - CameraLocation).Rotation();
                }
            }

            PinWrightPoseCapture::FCameraPose Pose;
            Pose.ProjectionMode = Request.ProjectionMode;
            Pose.Fov = Request.Fov;
            Pose.OrthoWidth = Request.OrthoWidth;
            Pose.Location = Request.Location;
            Pose.Rotation = Request.Rotation;
            Pose.Filename = Request.Filename;
            PoseRequest.Poses.Add(MoveTemp(Pose));
        }
        else
        {
            const float Distance = ComputeFitDistance(Radius, Request.Fov, AssetPreviewPadding);
            // One stem for the whole set. Left to the util's auto-name, shots taken inside the same
            // second collide on one filename and overwrite each other on disk while the response
            // still lists every shot; the caller's `filename` becomes the stem so a named set stays
            // named.
            const FString Stem = Request.Filename.IsEmpty()
                ? FString::Printf(TEXT("%s_AssetPreview_%s"), *Asset->GetName(),
                    *FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S")))
                : FPaths::GetBaseFilename(Request.Filename);

            PoseRequest.Poses.Reserve(Plan.Num());
            RequestedShots.Reserve(Plan.Num());
            OrthoAxisSnapped.Reserve(Plan.Num());
            for (int32 ShotIndex = 0; ShotIndex < Plan.Num(); ++ShotIndex)
            {
                const FPlannedShot Shot = Plan[ShotIndex];
                // Orthographic shots must look along a cardinal world axis (see
                // SnapOrbitAnglesToOrthographicAxis). The sides table is already axis-aligned, so
                // this is a no-op there; it guards an orthographic count/distribution set.
                float ShotAzimuth = Shot.Azimuth;
                float ShotElevation = Shot.Elevation;
                bool bShotSnapped = false;
                if (Shot.ProjectionMode == TEXT("orthographic"))
                {
                    bShotSnapped = SnapOrbitAnglesToOrthographicAxis(ShotAzimuth, ShotElevation);
                }

                PinWrightPoseCapture::FCameraPose Pose;
                Pose.ProjectionMode = Shot.ProjectionMode;
                Pose.Fov = Request.Fov;
                Pose.Filename = FString::Printf(TEXT("%s_shot%02d_az%d_el%d.png"), *Stem, ShotIndex,
                    FMath::RoundToInt(ShotAzimuth), FMath::RoundToInt(ShotElevation));
                PlaceOrbitCamera(Center, ShotAzimuth, ShotElevation, Distance, Pose.Location, Pose.Rotation);
                if (Shot.ProjectionMode == TEXT("orthographic"))
                {
                    // An explicit orthoWidth still wins: the caller who set a world span meant it.
                    Pose.OrthoWidth = bOrthoWidthProvided
                        ? Request.OrthoWidth
                        : ComputeOrthoWorldWidth(Radius, AssetPreviewPadding, Request.Width, Request.Height);
                }

                PoseRequest.Poses.Add(MoveTemp(Pose));
                RequestedShots.Add(Shot);
                OrthoAxisSnapped.Add(bShotSnapped);
                Plan[ShotIndex].Azimuth = ShotAzimuth;
                Plan[ShotIndex].Elevation = ShotElevation;
            }
        }

        // ---- cross the camera list with the instants ----
        //
        // Poses are INSTANT-MAJOR: every camera of instant 0, then every camera of instant 1. Same
        // layout render.capture_animation_preview uses, for the same reason.
        //
        // THE INSTANT IS SET ON THE FIRST CAMERA OF EACH GROUP AND ON NO OTHER. That is the
        // load-bearing line of this whole change, and it is not an optimisation. The primitive
        // calls the setter exactly once for a pose that carries a time and leaves the subject
        // untouched for a pose that carries none (PoseListCapture.h:87-91), and the Niagara driver
        // ENDS in DesiredAge hold mode at the age it actually reached, so the simulation stays on
        // that frame while the rest of the group's cameras fire
        // (CaptureSubjectProviders_Niagara.h:256-260). Setting the time on every pose would instead
        // re-drive it, and AdvanceToTime resets first -- with system determinism off the instance
        // seed is FMath::Rand() on every reset (NiagaraSystemInstance.cpp:894), so the six sides of
        // "one instant" would be six DIFFERENT effects that merely share a number. One drive per
        // instant is what makes a multi-camera time series a set of one moment.
        //
        // The same holds, less dramatically, for a scrubbed animation subject: one scrub per
        // instant is cheaper and cannot land differently between angles.
        TArray<int32> PoseInstantIndex;
        TArray<int32> PoseCameraIndex;
        // What each drive of the subject actually did, MEASURED off the provider after the fact
        // rather than echoed from the request. Filled by the wrapper below, one entry per instant.
        struct FInstantRecord
        {
            double RequestedSeconds = 0.0;
            bool   bDriven = false;
            // The provider published a mechanism detail for this drive. False for every kind whose
            // provider has none to publish, which is not a failure - it is the absence of an
            // answer, and the response says nothing rather than zeroing it.
            bool   bMechanismRead = false;
            int32  TickCount = 0;
            double TickDeltaSeconds = 0.0;
            bool   bAgeMeasured = false;
            double AchievedAgeSeconds = 0.0;
            // Whether a simulation was actually running for this instant. Separate from bDriven:
            // bDriven says the setter was called and did not refuse, bSimulated says the ticks it
            // reports had an instance to run on.
            bool   bSimulated = false;
            // Which engine path swallowed the ticks, when bSimulated is false. Empty when the
            // simulation advanced, or when the provider had nothing to say.
            FString StallReason;
        };
        TArray<FInstantRecord> InstantRecords;
        int32 InstantDriveCursor = 0;

        if (bTimeAxis)
        {
            const TArray<PinWrightPoseCapture::FCameraPose> CameraPoses = PoseRequest.Poses;
            // One stem for the whole series. See SubjectTimeSeries.h on why the crossing rewrites
            // every filename rather than inheriting the single-still one.
            const FString TimeStem = Request.Filename.IsEmpty()
                ? FString::Printf(TEXT("%s_AssetPreview_%s"), *Asset->GetName(),
                    *FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S")))
                : FPaths::GetBaseFilename(Request.Filename);
            TArray<FString> CameraTags;
            CameraTags.Reserve(Plan.Num());
            for (const FPlannedShot& PlannedShot : Plan)
            {
                CameraTags.Add(FString::Printf(TEXT("_az%d_el%d"),
                    FMath::RoundToInt(PlannedShot.Azimuth), FMath::RoundToInt(PlannedShot.Elevation)));
            }

            PinWrightSubjectTimeSeries::CrossCamerasWithInstants(
                CameraPoses, Instants, TimeStem, CameraTags,
                PoseRequest.Poses, PoseInstantIndex, PoseCameraIndex);

            InstantRecords.SetNum(Instants.Num());
            for (int32 InstantIndex = 0; InstantIndex < Instants.Num(); ++InstantIndex)
            {
                InstantRecords[InstantIndex].RequestedSeconds = Instants[InstantIndex];
            }

            // The resolver's setter, wrapped ONLY to record what the drive achieved. The wrapper
            // adds no behaviour and swallows no failure: a refusal passes straight back out with
            // the provider's own code and message, which is what the primitive turns into the
            // set's failure.
            //
            // The instant is identified by a CURSOR, not by the time value handed in, because
            // times:[0.5, 0.5] is a legal request and matching on the value would credit both
            // drives to one record. The primitive guarantees one call per pose in pose order
            // (PoseListCapture.h:170-174), which is what makes the cursor correct.
            PoseRequest.SubjectTimeSetter =
                [&TimeSetter, &Resolved, &InstantRecords, &InstantDriveCursor](
                    double TimeSeconds, FString& OutErrCode, FString& OutErrMsg) -> bool
            {
                if (!TimeSetter || !TimeSetter(TimeSeconds, OutErrCode, OutErrMsg))
                {
                    if (OutErrCode.IsEmpty())
                    {
                        OutErrCode = ErrorCodes::ERR_UNSUPPORTED_ASSET_EDITOR;
                        OutErrMsg = FString::Printf(
                            TEXT("The resolved subject supplied no time setter, so %g s could not be "
                                 "reached. This is a provider defect, not a bad argument."),
                            TimeSeconds);
                    }
                    return false;
                }
                const int32 Index = InstantDriveCursor++;
                if (InstantRecords.IsValidIndex(Index))
                {
                    FInstantRecord& Record = InstantRecords[Index];
                    Record.bDriven = true;
                    // Ask the provider what it did. Returns an invalid pointer for every kind that
                    // is not Niagara, which is why nothing here switches on the kind.
                    if (const PinWrightCaptureSubjectNiagara::FNiagaraReleaseState* NiagaraState =
                            PinWrightCaptureSubjectNiagara::GetNiagaraReleaseState(Resolved))
                    {
                        Record.bMechanismRead = true;
                        Record.TickCount = NiagaraState->LastStep.TickCount;
                        Record.TickDeltaSeconds =
                            static_cast<double>(NiagaraState->LastStep.TickDeltaSeconds);
                        Record.bAgeMeasured = NiagaraState->LastStep.bAgeMeasured;
                        Record.AchievedAgeSeconds = NiagaraState->LastStep.AchievedAgeSeconds;
                        Record.bSimulated = NiagaraState->LastStep.bSimulated;
                        Record.StallReason = NiagaraState->LastStep.StallReason;
                    }
                }
                return true;
            };
        }

        // ---- the subject coverage differential ----
        //
        // THE ONE SIGNAL THAT CATCHES AN EMPTY FRAME. Over pure backdrop `boundsInFrame`, `blank`
        // and `litPixelFraction` all read healthy - the bounds genuinely ARE in frame and the
        // backdrop genuinely IS lit - so a capture containing nothing at all comes back as a clean
        // success on every other published field. Both recorded instances cost hours: a Niagara
        // system with nothing alive at the instant asked for, and a Static Mesh capture returning
        // uniform colour with blank:false, crushed:null, litPixelFraction:1.0, boundsInFrame:true.
        //
        // DEFAULTS ON, and the wire flag can only turn it OFF. Opt-IN was considered and rejected:
        // a signal that is off until somebody thinks to ask for it is off exactly when it is
        // needed, and nobody asks for this one before they have been fooled once.
        // The cost - one extra draw plus readback per shot, two full-size buffers at a time - is
        // real enough that a caller who wants pictures and not measurements must be able to say so,
        // which is what `measureCoverage:false` is; the default stays on for everyone who does not.
        //
        // ASKED and ABLE are separate on purpose, and the primitive takes the CONJUNCTION
        // (PoseListCapture.cpp:67). This flag is the asking; the PROVIDER'S bound setter is the
        // being able. A kind that binds nothing yields an ABSENT coverage number rather than a
        // zero, which would be a measurement claim nobody made.
        //
        // NO PER-KIND BRANCH HERE, AND THAT IS THE FIX. This verb used to build a Niagara-shaped
        // lambda behind a `GetNiagaraReleaseState` test, which made the verb the owner of a table
        // of which kinds can be hidden - and every kind missing from that table had
        // `subjectCoverage` structurally absent, forever, with nothing in the response saying the
        // difference was a capability gap rather than a measurement. Which components can be
        // hidden without hiding the backdrop is provider knowledge; a new provider now gains
        // coverage by binding FResolvedSubject::VisibilitySetter, not by editing a handler.
        PoseRequest.bMeasureSubjectCoverage = Ctx.GetBool(TEXT("measureCoverage"), true);
        PoseRequest.SubjectVisibilitySetter = Resolved.VisibilitySetter;

        PinWrightPoseCapture::FPoseListCaptureOutput PoseResult;
        if (!PinWrightPoseCapture::CaptureCameraPoses(*Resolved.ViewportClient, Resolved.SceneViewport,
                PoseRequest, PoseResult, ErrorCode, ErrorMessage))
        {
            Ctx.SendError(ErrorCode, ErrorMessage);
            return true;
        }
        if (PoseResult.Captures.Num() == 0)
        {
            Ctx.SendError(ErrorCodes::ERR_CAPTURE_FAILED,
                FString::Printf(TEXT("The pose list produced no captures for '%s'."), *AssetPath));
            return true;
        }

        Result = MakeShared<FJsonObject>();
        // The top-level frame fields stay exactly what they have always been -- the FIRST capture
        // of the call -- so a single-still caller sees the response it already reads, and a
        // multi-shot caller gets shots[] in addition rather than instead.
        AddCaptureFields(PoseResult.Captures[0], PoseResult.Requests[0], Result);
        // Did this frame contain the subject at all? `blank` cannot answer that -- it catches BLACK
        // frames, and the preview backdrop is brightly lit, so two captures of a mis-aimed camera
        // came back byte-identical pure backdrop with blank=false and litPixelFraction=1. Measured
        // by the primitive against the pose the RENDERER resolved to, so it also catches a viewport
        // that ignored the request. Emitted unconditionally here, as it always has been on this
        // verb: an asset subject always has bounds, so the block always has something to say.
        if (PoseResult.Framings.IsValidIndex(0))
        {
            TSharedPtr<FJsonObject> FramingObj =
                PinWrightRenderCapture::MakeBoundsFramingObject(PoseResult.Framings[0]);
            // The warning goes INSIDE the verdict it qualifies, not beside it: `evaluated:false`
            // on its own says a verdict was unreachable, and gives a caller no way to tell "no
            // bounds were measured, and the camera you are looking at was guessed" from "bounds
            // were fine, the check just did not run".
            if (bFramingIsAGuess)
            {
                FramingObj->SetStringField(TEXT("autoFramingWarning"), FString::Printf(
                    TEXT("No bounds could be measured for '%s', so the default camera was solved "
                         "from a 1 cm placeholder radius and is almost certainly framing nothing. "
                         "Read subject.boundsWarning for why the bounds are unmeasurable, then pass "
                         "an explicit location/rotation (or orthoWidth) to frame this subject."),
                    *AssetPath));
            }
            Result->SetObjectField(TEXT("framing"), FramingObj);
        }
        // The top-level frame fields describe capture 0, so its coverage belongs here too -- and it
        // is the number a single-still caller sees, since they get no `shots` array to read it
        // from. Writes nothing when no differential ran, exactly as it does per shot.
        PinWrightPoseCapture::AddPoseCoverageField(PoseResult, 0,
            PoseRequest.CoverageWarnFraction, Result);

        if (bMultiShot)
        {
            TArray<TSharedPtr<FJsonValue>> ShotsJson;
            ShotsJson.Reserve(PoseResult.Captures.Num());
            for (int32 ShotIndex = 0; ShotIndex < PoseResult.Captures.Num(); ++ShotIndex)
            {
                TSharedPtr<FJsonObject> ShotObj = MakeShared<FJsonObject>();
                // The camera this shot used. Identity with ShotIndex only while there is no time
                // axis; a time series repeats the whole camera list once per instant, so the angle
                // tables must be indexed by the CAMERA, never by the shot.
                const int32 CameraIndex = PoseCameraIndex.IsValidIndex(ShotIndex)
                    ? PoseCameraIndex[ShotIndex]
                    : ShotIndex;
                if (RequestedShots.IsValidIndex(CameraIndex) && Plan.IsValidIndex(CameraIndex))
                {
                    TSharedPtr<FJsonObject> AngleObj = MakeShared<FJsonObject>();
                    // All four unconditional, matching camera.orbit_shots: a caller who finds a
                    // defect in shot 5 of a sphere distribution can re-shoot that exact pose
                    // instead of reversing it out of a filename that carries rounded integers.
                    AngleObj->SetNumberField(TEXT("azimuth"), Plan[CameraIndex].Azimuth);
                    AngleObj->SetNumberField(TEXT("elevation"), Plan[CameraIndex].Elevation);
                    AngleObj->SetNumberField(TEXT("requestedAzimuth"), RequestedShots[CameraIndex].Azimuth);
                    AngleObj->SetNumberField(TEXT("requestedElevation"), RequestedShots[CameraIndex].Elevation);
                    ShotObj->SetObjectField(TEXT("angle"), AngleObj);
                    if (OrthoAxisSnapped.IsValidIndex(CameraIndex) && OrthoAxisSnapped[CameraIndex])
                    {
                        ShotObj->SetBoolField(TEXT("orthoAxisSnapped"), true);
                        ShotObj->SetNumberField(TEXT("requestedAzimuth"), RequestedShots[CameraIndex].Azimuth);
                        ShotObj->SetNumberField(TEXT("requestedElevation"), RequestedShots[CameraIndex].Elevation);
                    }
                }
                // Which instant this shot shows, on the shots that have one. Shots sharing an
                // `instantIndex` were captured from ONE drive of the subject and show the same
                // moment; shots with different indices do not. `time` is the instant the caller
                // ASKED for -- what the simulation actually reached is per-instant in subjectTime,
                // because a fixed tick delta cannot land on an arbitrary second.
                if (PoseInstantIndex.IsValidIndex(ShotIndex))
                {
                    const int32 InstantIndex = PoseInstantIndex[ShotIndex];
                    ShotObj->SetNumberField(TEXT("instantIndex"), InstantIndex);
                    if (Instants.IsValidIndex(InstantIndex))
                    {
                        ShotObj->SetNumberField(TEXT("time"), Instants[InstantIndex]);
                    }
                }
                AddShotFields(PoseResult.Captures[ShotIndex], PoseResult.Requests[ShotIndex], ShotObj);
                PinWrightPoseCapture::AddPoseFramingField(PoseResult, ShotIndex, ShotObj);
                // Beside `framing`, never inside it: the two answer different questions from
                // different evidence. `framing` is geometry and says whether the bounding sphere
                // could project into the frame; `subjectCoverage` is pixels and says how much of
                // this picture is actually the subject. A frame can pass the first and score 0.000
                // on the second, which is the whole reason the differential exists.
                PinWrightPoseCapture::AddPoseCoverageField(PoseResult, ShotIndex,
                    PoseRequest.CoverageWarnFraction, ShotObj);
                ShotsJson.Add(MakeShared<FJsonValueObject>(ShotObj));
            }
            Result->SetArrayField(TEXT("shots"), ShotsJson);
            Result->SetNumberField(TEXT("count"), ShotsJson.Num());
            if (bViewsProvided)
            {
                Result->SetStringField(TEXT("views"), Views);
            }
            // Only on a SET. Emitting it beside a single still would claim a distribution that
            // never applied to anything.
            // ---- did the elevation this plan was built from survive to the cameras? ----
            //
            // MEASURED off the two parallel angle tables, never deduced from `projectionMode`. The
            // orthographic snap only MOVES an elevation that is not already cardinal, and the
            // `sides` table is cardinal by construction - so deducing "orthographic, therefore
            // ignored" would fire `elevationIgnored` on a set the snap left untouched, which is the
            // same class of false statement in the other direction.
            int32 ElevationSnappedShots = 0;
            for (int32 CameraIndex = 0; CameraIndex < Plan.Num(); ++CameraIndex)
            {
                if (RequestedShots.IsValidIndex(CameraIndex) &&
                    !FMath::IsNearlyEqual(Plan[CameraIndex].Elevation,
                        RequestedShots[CameraIndex].Elevation, 0.01f))
                {
                    ++ElevationSnappedShots;
                }
            }

            FShotDistributionPlan DistributionPlan;
            DistributionPlan.Distribution = Distribution;
            DistributionPlan.bSeeded = bSeedProvided;
            DistributionPlan.Seed = Seed;
            DistributionPlan.AzimuthOffsetDegrees = AzimuthOffsetDegrees;
            DistributionPlan.bAppliesToPlan = bCountProvided;
            DistributionPlan.bElevationProvided = bElevationProvided;
            // The value the ring was BUILT from, which is the caller's when they gave one and this
            // verb's documented default of 30 when they did not. Publishing it is what makes a
            // rewritten default visible: `orthographic` used to answer pitch 0 and _el0 filenames
            // with `elevation` echoing null, so 30 -> 0 was indistinguishable from 30 holding.
            DistributionPlan.ElevationDegrees = DefaultElevation;
            DistributionPlan.ElevationSnappedShots = ElevationSnappedShots;
            DistributionPlan.PlannedShots = Plan.Num();
            Result->SetObjectField(TEXT("shotDistribution"),
                MakeShotDistributionObject(DistributionPlan));
        }

        // One vocabulary for every capture verb (CameraShotPlanUtils.h). This verb never applies
        // the multi-image budget -- it keeps its historical 1024 on every call shape, including
        // `views`, so a set and a still of the same asset are measured at the same
        // world-units-per-pixel.
        Result->SetStringField(TEXT("resolutionSource"),
            ResolveResolutionSource(bCallerSizeProvided, EResolutionSource::Default));
        // What the base primitive did with the pose list: how many poses were asked for, how many
        // captured, how many the bound dropped, and whether the throwaway warm-up frame was taken.
        Result->SetObjectField(TEXT("poseSet"),
            PinWrightPoseCapture::MakePoseSetInfoObject(PoseResult));

        // ---- the time axis, only when one was asked for ----
        //
        // CONDITIONAL, unlike viewport.previewScene next door. That block is unconditional on
        // purpose, so "not asked for" and "asked for and did nothing" stay distinguishable; here
        // the two are already distinguishable without it, because `subject.timeSupported` is
        // unconditional and says whether an axis exists at all. A subjectTime block on every call
        // would be the fourteenth assert-absent case for no gain.
        if (bTimeAxis)
        {
            TSharedPtr<FJsonObject> TimeObj = MakeShared<FJsonObject>();
            TimeObj->SetNumberField(TEXT("instantCount"), Instants.Num());
            TimeObj->SetNumberField(TEXT("camerasPerInstant"), CameraCount);
            // MEASURED, not derived: the primitive counts the drives it actually performed. A
            // number below instantCount means the set failed part-way and the shots that made it
            // back cover fewer instants than were asked for.
            TimeObj->SetNumberField(TEXT("subjectDrives"), PoseResult.SubjectTimesApplied);
            TArray<TSharedPtr<FJsonValue>> InstantsJson;
            InstantsJson.Reserve(InstantRecords.Num());
            for (const FInstantRecord& Record : InstantRecords)
            {
                TSharedPtr<FJsonObject> InstantObj = MakeShared<FJsonObject>();
                InstantObj->SetNumberField(TEXT("requestedSeconds"), Record.RequestedSeconds);
                // Unconditional inside the array: an instant that was never reached is exactly what
                // a reader needs to see, and its absence would look like a shorter series.
                InstantObj->SetBoolField(TEXT("driven"), Record.bDriven);
                if (Record.bMechanismRead)
                {
                    // How the instant was reached, which is the only thing that makes a particle
                    // frame interpretable: N whole fixed steps of this delta, floored - so a
                    // requested second that is not a multiple of the delta is NOT the age reached.
                    InstantObj->SetNumberField(TEXT("tickCount"), Record.TickCount);
                    InstantObj->SetNumberField(TEXT("tickDeltaSeconds"), Record.TickDeltaSeconds);
                    // What makes the two numbers above readable. UNiagaraComponent::AdvanceSimulation
                    // silently ticks nothing without a system instance controller, so `tickCount: 33`
                    // beside `simulated: false` means 33 ticks were ASKED FOR and none of them ran -
                    // and this instant's frames are not of the moment they claim.
                    InstantObj->SetBoolField(TEXT("simulated"), Record.bSimulated);
                }
                // Published unconditionally: without it, an absent achievedAgeSeconds below reads
                // exactly like a measured zero, and the two mean opposite things.
                InstantObj->SetBoolField(TEXT("ageMeasured"), Record.bAgeMeasured);
                if (Record.bAgeMeasured)
                {
                    // Read off the running simulation AFTER the ticks, never computed from them.
                    // Absent means no instance existed to ask, which is a different statement from
                    // "the age was 0".
                    InstantObj->SetNumberField(TEXT("achievedAgeSeconds"), Record.AchievedAgeSeconds);
                }
                if (!Record.StallReason.IsEmpty())
                {
                    // Names which engine path swallowed the ticks, so a caller reading this array
                    // does not have to fall back to subject.niagara to find out.
                    InstantObj->SetStringField(TEXT("simulationStalledReason"), Record.StallReason);
                }
                InstantsJson.Add(MakeShared<FJsonValueObject>(InstantObj));
            }
            TimeObj->SetArrayField(TEXT("instants"), InstantsJson);
            Result->SetObjectField(TEXT("subjectTime"), TimeObj);
        }

        Result->SetStringField(TEXT("assetPath"), AssetPath);
        Result->SetStringField(TEXT("target"), Target);
        // MEASURED by the provider rather than hardcoded per class: "staticMeshEditorPreview" for a
        // Static Mesh, "personaPreviewViewport" for a skinned subject, and so on.
        Result->SetStringField(TEXT("captureSource"), Resolved.CaptureSource);

        // ---- the Nanite blind spot of the four substitution-only debug modes ----
        //
        // Emitted here because this is the only place BOTH facts are in scope: which mode the call
        // asked for, and which asset is in front of the camera. The pairing is a false PASS rather
        // than a visible failure -- an inside-out Nanite mesh under front_back_face returns the
        // same pixels a correct one does -- so nothing downstream can recover it, and no existing
        // field changes value when it happens. See IsDebugMaterialSubstitutionMode for the engine
        // evidence and the measurement.
        //
        // A WARNING, NOT A REFUSAL, and deliberately so: `clay` and `zebra` on a Nanite mesh still
        // return a legitimate picture of the surface, just not one drawn through the debug
        // material, and `random_color` is merely uninformative rather than misleading. Only
        // front_back_face converts the gap into a wrong answer. Refusing all four would break
        // capturing Nanite content in the modes where the gap costs nothing.
        if (bDebugMaterialSubstitutionRequested)
        {
            if (bSubjectIsNanite)
            {
                Result->SetBoolField(TEXT("viewModeNaniteBlindSpot"), true);
                Result->SetStringField(TEXT("viewModeNaniteWarning"), FString::Printf(
                    TEXT("This capture ran in view mode '%s', which the engine draws ONLY by ")
                    TEXT("substituting a debug material per mesh batch in ApplyViewModeOverrides, ")
                    TEXT("and '%s' has Nanite ENABLED. Nanite geometry keeps static relevance and ")
                    TEXT("rasterises with its real materials, so the substitution never reaches it ")
                    TEXT("and the subject is drawn UNTINTED. For front_back_face that is ")
                    TEXT("indistinguishable from the correct-winding answer, so this frame cannot ")
                    TEXT("be read as a pass: an inverted Nanite mesh returns the same pixels as a ")
                    TEXT("correct one. viewport.viewModeOverride.applied reads true regardless and ")
                    TEXT("does not measure this. Disable Nanite on the asset for the diagnostic, ")
                    TEXT("or use geometry.audit_static_meshes / health.signedVolume, which do not ")
                    TEXT("depend on the render path."),
                    *PinWrightRenderCapture::GetViewModeKey(Request.ViewMode.ViewMode),
                    *AssetPath));
            }
        }

        // Read the component after the final draw, while the provider still owns it. This is the
        // only point where forced/predicted LOD and editor section visibility describe the frame
        // that was actually captured; asset slots alone include inactive LODs and hidden sections.
        if (UMeshComponent* MeshComponent =
                PinWrightCaptureSubjectMesh::GetResolvedMeshComponent(Resolved))
        {
            MaterialReadiness =
                PinWright::MaterialShaderState::ProbeCaptureComponent(MeshComponent);
        }
        else
        {
            MaterialReadiness = PinWright::MaterialShaderState::ProbeCaptureAsset(Asset);
        }

        // EXPLICIT, and it has to be here rather than at the end of the block: the release is what
        // closes the asset editor and measures bEditorClosed, so the three window fields below --
        // and the same pair inside the `subject` block -- would otherwise report the request
        // instead of what happened. Idempotent, so the destructor that follows is a no-op.
        PinWrightCaptureSubject::ReleaseSubject(Resolved);

        Result->SetBoolField(TEXT("assetEditorWasAlreadyOpen"), Resolved.bEditorWasAlreadyOpen);
        Result->SetBoolField(TEXT("assetEditorClosed"), Resolved.bEditorClosed);
        // THE THIRD WINDOW FIELD, and it is what makes the other two readable. The close is queued
        // onto the next core-ticker pass instead of running inside the release path, because
        // destroying an asset editor toolkit there is an editor-killing access violation
        // (CaptureSubject.h, ScheduleDeferredAssetEditorClose). So `assetEditorClosed` is false on
        // the normal close path -- honestly, the window really is still there as this answers --
        // and this field is the difference between "queued, gone next tick" and "left open".
        Result->SetBoolField(TEXT("assetEditorCloseDeferred"),
            PinWrightCaptureSubject::HasPendingDeferredAssetEditorClose(Resolved.AssetPath));
        const TSharedPtr<FJsonObject> SubjectInfo =
            PinWrightCaptureSubject::MakeSubjectInfoObject(Resolved);
        // The provider's own mechanism detail, hung under the kind that produced it.
        //
        // GATED ON A DRIVE HAVING HAPPENED, not merely on the kind. The block's `requestedTimeSeconds`
        // and `achievedAgeSeconds` describe ONE step -- the last one the setter ran -- so publishing
        // it after a call that asked for no instant would report a step that never happened. With a
        // series it describes the last instant, which the per-instant `subjectTime.instants` array
        // covers properly; this block is here for the determinism report, which is the honest
        // counterweight to reading a particle series as repeatable and has no per-instant variation
        // to lose. ReleaseSubject deliberately does not clear ProviderState, so reading it after
        // the release is defined.
        //
        // There is no `reproducible` field here in any spelling, in either direction, and there
        // must never be one: MakeNiagaraSubjectDetailObject holds that line and
        // PinWright.render.capture_subject_niagara.NoReproducibilityIsClaimed enforces it by
        // scanning the key set for the substring rather than for one field name.
        if (SubjectInfo.IsValid() && bTimeAxis && PoseResult.SubjectTimesApplied > 0)
        {
            if (const TSharedPtr<FJsonObject> NiagaraDetail =
                    PinWrightCaptureSubjectNiagara::MakeNiagaraSubjectDetailObject(Resolved))
            {
                SubjectInfo->SetObjectField(TEXT("niagara"), NiagaraDetail);
            }
        }
        Result->SetObjectField(TEXT("subject"), SubjectInfo);
    }

    if (!PinWright::MaterialShaderState::ApplyCaptureFallbackPolicy(
            Result, MaterialReadiness, bAllowFallback, bCaptureUsesSubjectMaterials))
    {
        Result->SetBoolField(TEXT("success"), false);
        Ctx.SendError(ErrorCodes::ERR_MATERIAL_FALLBACK,
            TEXT("An asset-preview material used the engine Default Material because shader "
                 "compilation failed or a rendered material slot was unassigned. "
                 "materialReadiness names the captured component's measured LOD/section scope and "
                 "the known failed or unassigned subject. Fix it, or pass "
                 "allowFallback:true to retain the image explicitly."),
            Result);
        return true;
    }

    Ctx.SendSuccess(Result);
    return true;
}

PinWrightOpenLevelCapture::FWorldMatchDecision
PinWrightOpenLevelCapture::EvaluateViewportWorldMatch(
    const UWorld* EditorWorld, const UWorld* ViewportWorld, bool bAllowPieWorld)
{
    FWorldMatchDecision Decision;
    const auto NormalizedMapName = [](const UWorld* World)
    {
        return World && World->GetOutermost()
            ? UWorld::RemovePIEPrefix(World->GetOutermost()->GetName())
            : FString();
    };

    Decision.EditorMapName = NormalizedMapName(EditorWorld);
    Decision.ViewportMapName = NormalizedMapName(ViewportWorld);
    Decision.bMapsMatch = !Decision.EditorMapName.IsEmpty()
        && !Decision.ViewportMapName.IsEmpty()
        && FName(*Decision.EditorMapName) == FName(*Decision.ViewportMapName);
    Decision.bViewportIsPieWorld = ViewportWorld
        && ViewportWorld->WorldType == EWorldType::PIE;

    if (EditorWorld && ViewportWorld == EditorWorld)
    {
        Decision.bAllowed = true;
        Decision.bMapsMatch = true;
        return Decision;
    }

    Decision.ErrorCode = ErrorCodes::ERR_VIEWPORT_WORLD_MISMATCH;
    if (!bAllowPieWorld)
    {
        Decision.Reason = TEXT("allowPieWorldRequired");
    }
    else if (!Decision.bViewportIsPieWorld)
    {
        Decision.Reason = TEXT("viewportWorldIsNotPie");
    }
    else if (!Decision.bMapsMatch)
    {
        Decision.Reason = TEXT("pieMapMismatch");
    }
    else
    {
        Decision.bAllowed = true;
        Decision.ErrorCode.Reset();
    }
    return Decision;
}

bool PinWrightOpenLevelCapture::Handle(FHandlerContext& Ctx)
{
    const PinWrightRenderCapture::FViewportCaptureHooks Hooks;
    const PinWrightOpenLevelCapture::FSuccessDecorator DecorateSuccess;
    return Handle(Ctx, Hooks, DecorateSuccess);
}

bool PinWrightOpenLevelCapture::Handle(FHandlerContext& Ctx,
    const PinWrightRenderCapture::FViewportCaptureHooks& Hooks,
    const PinWrightOpenLevelCapture::FSuccessDecorator& DecorateSuccess)
{
    PinWrightRenderCapture::FViewportCaptureRequest Request;
    FString ErrorCode;
    FString ErrorMessage;
    if (!PinWrightRenderCapture::ParseViewportCaptureRequest(
        Ctx.GetRawPayload(), Request, ErrorCode, ErrorMessage))
    {
        Ctx.SendError(ErrorCode, ErrorMessage);
        return true;
    }
    const bool bAllowPieWorld = Ctx.GetBool(TEXT("allowPieWorld"), false);

    // ---- the third parameter the shared parser reads that this verb does NOT offer ----
    //
    // ParseViewportCaptureRequest is shared with render.capture_asset_preview and
    // render.capture_annotated, so it fills Request.PreviewSceneRig off ANY payload carrying a
    // `render.capture_open_level` parses then clears `previewScene`: This verb declares no such parameter and could not honour one because it
    // drives the LIVE Level Editor viewport, which has no FPreviewScene behind it
    // (FEditorViewportClient::GetPreviewScene() returns null there), so a key-light / sky /
    // floor rig has nothing to write to and nothing to restore.
    //
    // On the wire the dispatcher's unknown-param gate already refuses the field with
    // UNKNOWN_PARAMS naming the valid list. But a DIRECT handler invocation -- the path every
    // automation test takes -- bypasses that gate and still reaches the parser, so the pin is
    // cleared here for exactly the reason `hideEditorSprites` and `viewDistanceScale` are
    // cleared in render.capture_asset_preview above: a parameter that is half-live on one path
    // and refused on the other is the defect, not the declaration. Behaviour, schema and the
    // shared parser then agree on every path instead of three ways.
    PinWrightRenderSubject::ClearPreviewSceneRigForLevelViewport(Request);

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

    FLevelEditorModule* LevelEditorModule = FModuleManager::GetModulePtr<FLevelEditorModule>(TEXT("LevelEditor"));
    if (!LevelEditorModule)
    {
        Ctx.SendError(ErrorCodes::ERR_NO_ACTIVE_LEVEL_VIEWPORT, TEXT("LevelEditor module is not loaded"));
        return true;
    }

    TSharedPtr<IAssetViewport> ActiveViewport = LevelEditorModule->GetFirstActiveViewport();
    if (!ActiveViewport.IsValid())
    {
        Ctx.SendError(ErrorCodes::ERR_NO_ACTIVE_LEVEL_VIEWPORT, TEXT("No active Level Editor viewport"));
        return true;
    }

    FEditorViewportClient& ViewportClient = ActiveViewport->GetAssetViewportClient();
    TSharedPtr<FSceneViewport> SceneViewport = ActiveViewport->GetSharedActiveViewport();
    if (!SceneViewport.IsValid())
    {
        Ctx.SendError(ErrorCodes::ERR_NO_ACTIVE_LEVEL_VIEWPORT, TEXT("Active Level Editor viewport has no SceneViewport"));
        return true;
    }

    UWorld* ViewportWorld = ViewportClient.GetWorld();
    const FWorldMatchDecision WorldMatch = EvaluateViewportWorldMatch(
        EditorWorld, ViewportWorld, bAllowPieWorld);
    if (!WorldMatch.bAllowed)
    {
        TSharedPtr<FJsonObject> Details = MakeShared<FJsonObject>();
        AddWorldFields(EditorWorld, ViewportWorld, Details);
        Details->SetBoolField(TEXT("allowPieWorld"), bAllowPieWorld);
        Details->SetBoolField(TEXT("viewportIsPieWorld"), WorldMatch.bViewportIsPieWorld);
        Details->SetBoolField(TEXT("pieWorldSameMap"), WorldMatch.bMapsMatch);
        Details->SetStringField(TEXT("editorMapName"), WorldMatch.EditorMapName);
        Details->SetStringField(TEXT("viewportMapName"), WorldMatch.ViewportMapName);
        Details->SetStringField(TEXT("reason"), WorldMatch.Reason);
        TSharedPtr<FJsonObject> Viewport = MakeShared<FJsonObject>();
        const ELevelViewportType ViewportType = ViewportClient.GetViewportType();
        Viewport->SetStringField(TEXT("type"),
            StaticEnum<ELevelViewportType>()->GetNameStringByValue(static_cast<int64>(ViewportType)));
        Viewport->SetNumberField(TEXT("typeValue"), static_cast<int32>(ViewportType));
        // Through the vocabulary, not the raw engine call - see PinWrightViewModes::GetDisplayName
        // for the two modes UViewModeUtils::GetViewModeDisplayName cannot name without ensuring.
        Viewport->SetStringField(TEXT("viewMode"),
            PinWrightViewModes::GetDisplayName(ViewportClient.GetViewMode()));
        Viewport->SetNumberField(TEXT("viewModeValue"), static_cast<int32>(ViewportClient.GetViewMode()));
        Viewport->SetBoolField(TEXT("gameView"), ViewportClient.IsInGameView());
        Viewport->SetBoolField(TEXT("realtime"), ViewportClient.IsRealtime());
        Details->SetObjectField(TEXT("viewport"), Viewport);
        Details->SetObjectField(TEXT("requestedCameraLocation"),
            PinWrightRenderCapture::MakeVectorObject(Request.Location));
        Details->SetObjectField(TEXT("requestedCameraRotation"),
            PinWrightRenderCapture::MakeRotatorObject(Request.Rotation));
        Details->SetStringField(TEXT("projectionMode"), Request.ProjectionMode);
        Ctx.SendError(WorldMatch.ErrorCode,
            TEXT("Active Level Editor viewport does not render the active editor world."), Details);
        return true;
    }

    Request.bRejectBlankCapture = true;
    // This is the only capture verb that renders a real level world, so it is the only one that
    // can survey cull distances. The util applies the derived scale to orthographic captures only
    // -- see the comment beside bAutoViewDistanceScale for why perspective is left alone.
    Request.bAutoViewDistanceScale = true;

    // ---- optional subject: a framing VERDICT, never a camera move ----
    //
    // This verb stays domain-named (plan §4.3): it captures the open level, always, and a `subject`
    // never changes which viewport it shoots through. What the subject adds is the one question the
    // response could not previously answer -- is the thing I am reviewing actually in this frame?
    // `blank` cannot answer it: an empty stretch of landscape is neither black nor blank, and two
    // captures of a mis-aimed camera come back non-blank and byte-identical. Asset kinds are
    // refused rather than served, because serving one would mean capturing something other than the
    // open level under a verb whose name says otherwise.
    //
    // The resolved subject lives in this block and its release runs at the block's end, before the
    // capture. That is correct HERE and only here: a world/actor subject resolves to the level
    // viewport this verb already holds and its bounds are a static sphere, so nothing the capture
    // needs outlives the release. An asset subject would not survive that ordering, which is a
    // second reason this verb refuses them.
    bool bSubjectResolved = false;
    FVector SubjectBoundsOrigin = FVector::ZeroVector;
    double SubjectBoundsRadius = 0.0;
    TSharedPtr<FJsonObject> SubjectBlock;
    if (PinWrightRenderSubject::HasSubjectField(Ctx.GetRawPayload()))
    {
        PinWrightCaptureSubject::FSubjectRequest SubjectRequest;
        if (!PinWrightCaptureSubject::ParseSubject(Ctx.GetRawPayload(), SubjectRequest,
                ErrorCode, ErrorMessage))
        {
            Ctx.SendError(ErrorCode, ErrorMessage);
            return true;
        }
        if (SubjectRequest.Kind != PinWrightCaptureSubject::ESubjectKind::World &&
            SubjectRequest.Kind != PinWrightCaptureSubject::ESubjectKind::Actor)
        {
            Ctx.SendError(ErrorCodes::ERR_UNSUPPORTED_ASSET_EDITOR,
                FString::Printf(
                    TEXT("render.capture_open_level captures the open level, so subject.kind '%s' has no ")
                    TEXT("frame here -- an asset is previewed in its own editor. Use ")
                    TEXT("render.capture_asset_preview for an asset, or subject.kind world|actor to ask ")
                    TEXT("whether something in THIS level is in frame."),
                    PinWrightCaptureSubject::ToWireName(SubjectRequest.Kind)));
            return true;
        }

        PinWrightCaptureSubject::FResolvedSubject Resolved;
        PinWrightCaptureSubject::FSubjectTimeSetter UnusedTimeSetter;
        if (!PinWrightCaptureSubject::Resolve(SubjectRequest, Resolved, UnusedTimeSetter,
                ErrorCode, ErrorMessage))
        {
            Ctx.SendError(ErrorCode, ErrorMessage);
            return true;
        }
        SubjectBoundsOrigin = Resolved.BoundsOrigin;
        SubjectBoundsRadius = Resolved.BoundsRadius;
        // Explicit rather than left to the destructor, so the release has provably run before the
        // capture below rather than at some point the reader has to derive from scope structure.
        // Nothing the capture needs outlives it: the bounds are a static sphere already copied out,
        // and a world/actor subject's viewport IS the level viewport this verb already holds.
        PinWrightCaptureSubject::ReleaseSubject(Resolved);
        SubjectBlock = PinWrightCaptureSubject::MakeSubjectInfoObject(Resolved);
        bSubjectResolved = true;
    }

    // ---- the readback preamble, composed onto whatever hooks the caller brought ----
    //
    // BeforeFinalFrame is the seam that runs AFTER the camera pose has been applied and after the
    // warm-up settle loop, and immediately before the final draw + pixel readback. That is the only
    // correct place for both halves: the shader compiles that decide whether this frame shows real
    // materials or the default material are the ones the NEW POSE queued, so a gate placed before
    // the pose would measure the wrong queue; and the flush is only worth anything against the
    // render/RHI work this capture itself just produced.
    //
    // The caller's own hook (effect.step_and_capture freezes and advances the world here) runs
    // FIRST and unchanged -- this composes, it does not replace.
    //
    // COST, stated rather than hidden: binding BeforeFinalFrame makes the shared capture run its
    // final-stage draw + readback, which an unhooked capture used to skip -- one extra frame and
    // one extra pixel read per capture. That is not incidental, it is the point: without it the
    // returned pixels would be the settle loop's LAST read, taken before the flush, and the flush
    // would mitigate nothing.
    const TSharedRef<PinWrightCaptureReadiness::FReadinessResult> Readiness =
        MakeShared<PinWrightCaptureReadiness::FReadinessResult>();
    PinWrightRenderCapture::FViewportCaptureHooks GatedHooks = Hooks;
    GatedHooks.BeforeFinalFrame =
        [&Hooks, Readiness](const PinWrightRenderCapture::FViewportCaptureOutput& SettledCapture,
            FString& OutErrorCode, FString& OutErrorMessage)
    {
        if (Hooks.BeforeFinalFrame
            && !Hooks.BeforeFinalFrame(SettledCapture, OutErrorCode, OutErrorMessage))
        {
            return false;
        }
        *Readiness = PinWrightCaptureReadiness::DrainBeforeReadback();
        if (Readiness->ShouldRefuseReadback())
        {
            OutErrorCode = ErrorCodes::ERR_CAPTURE_NOT_READY;
            OutErrorMessage = PinWrightCaptureReadiness::MakeRefusalMessage(*Readiness);
            return false;
        }
        Readiness->bReadbackFlushed = PinWrightScreenshotUtils::FlushBeforeReadback();
        return true;
    };

    PinWrightRenderCapture::FViewportCaptureOutput Capture;
    if (!PinWrightRenderCapture::CaptureEditorViewportToPng(
        ViewportClient,
        SceneViewport,
        Request,
        TEXT("OpenLevel"),
        TEXT("OpenLevel"),
        Capture,
        ErrorCode,
        ErrorMessage,
        &GatedHooks))
    {
        if (ErrorCode == ErrorCodes::ERR_BLANK_CAPTURE)
        {
            TSharedPtr<FJsonObject> Details = MakeShared<FJsonObject>();
            AddCaptureFields(Capture, Request, Details);
            AddWorldFields(EditorWorld, ViewportWorld, Details);
            Ctx.SendError(ErrorCode, ErrorMessage, Details);
        }
        else if (ErrorCode == ErrorCodes::ERR_CAPTURE_NOT_READY)
        {
            // The gate's own refusal, so the evidence it measured travels with it.
            TSharedPtr<FJsonObject> Details = MakeShared<FJsonObject>();
            PinWrightCaptureReadiness::AddReadinessFields(*Readiness, Details);
            Ctx.SendError(ErrorCode, ErrorMessage, Details);
        }
        else
        {
            Ctx.SendError(ErrorCode, ErrorMessage);
        }
        return true;
    }

    // The fifth contamination channel, surveyed on the same stack that took the pixels: text the
    // engine draws into the frame, which no show flag and no game view removes and which every
    // other honesty field reads clean through (Utils/OnScreenMessageSurvey.h).
    const PinWrightOnScreenMessages::FOnScreenMessageSurvey OnScreenMessages =
        PinWrightOnScreenMessages::Survey();

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    AddCaptureFields(Capture, Request, Result);
    {
        // Composed onto the block AddCaptureFields just built rather than inside
        // MakeViewportInfoObject: both readings are of live process state at capture time, not of
        // FViewportCaptureOutput, and that serializer is a pure function of the capture record.
        const TSharedPtr<FJsonObject>* ViewportBlock = nullptr;
        if (Result->TryGetObjectField(TEXT("viewport"), ViewportBlock) && ViewportBlock)
        {
            PinWrightCaptureReadiness::AddReadinessFields(*Readiness, *ViewportBlock);
            PinWrightOnScreenMessages::AddOnScreenMessageFields(OnScreenMessages, *ViewportBlock);
        }
    }
    // Present only when a subject was resolved. A `framing` block on a call that named nothing to
    // frame would be the fourteenth absence assertion nobody knew about, and an `evaluated: false`
    // block says less than no block at all.
    if (bSubjectResolved)
    {
        // Measured against the EFFECTIVE pose, not the requested one, so a viewport that ignored
        // the camera is caught rather than believed.
        const PinWrightRenderCapture::FBoundsFramingCheck Framing =
            PinWrightRenderCapture::EvaluateBoundsFraming(
                Request, Capture.EffectiveLocation, Capture.EffectiveRotation,
                SubjectBoundsOrigin, SubjectBoundsRadius);
        Result->SetObjectField(TEXT("framing"),
            PinWrightRenderCapture::MakeBoundsFramingObject(Framing));
        if (SubjectBlock.IsValid())
        {
            Result->SetObjectField(TEXT("subject"), SubjectBlock);
        }
    }
    Result->SetStringField(TEXT("levelPath"), ViewportWorld && ViewportWorld->GetOutermost()
        ? ViewportWorld->GetOutermost()->GetName()
        : FString());
    Result->SetStringField(TEXT("target"), TEXT("openLevel"));
    Result->SetStringField(TEXT("captureSource"), TEXT("levelEditorViewport"));
    Result->SetBoolField(TEXT("allowPieWorld"), bAllowPieWorld);
    Result->SetBoolField(TEXT("capturedPieWorld"), WorldMatch.bViewportIsPieWorld);
    Result->SetBoolField(TEXT("pieWorldSameMap"), WorldMatch.bMapsMatch);
    AddWorldFields(EditorWorld, ViewportWorld, Result);
    if (DecorateSuccess)
    {
        DecorateSuccess(Capture, Result);
    }
    Ctx.SendSuccess(Result);
    return true;
}

// ---- render.capture_open_level ----
REGISTER_RPC_HANDLER("render.capture_open_level", "render", "Capture the active opened Level Editor viewport from a caller-specified camera as an exact-size PNG.",
    RPC_PARAMS(
        RPC_PARAM_OPT("filename", "filepath", "Output filename inside Saved/Screenshots/OpenLevel. The .png extension is appended if missing."),
        RPC_PARAM_OPT("width", "number", "Output width in pixels. Default 768."),
        RPC_PARAM_OPT("height", "number", "Output height in pixels. Default 768."),
        RPC_PARAM_OPT("location", "object", "Level viewport camera location {x, y, z}."),
        RPC_PARAM_OPT("rotation", "object", "Level viewport camera rotation {pitch, yaw, roll}."),
        RPC_PARAM_OPT("projectionMode", "string", "'perspective' (default) or 'orthographic'. Orthographic requires a rotation that looks along a world axis (e.g. pitch -90 for top-down)."),
        RPC_PARAM_OPT("fov", "number", "Perspective field of view in degrees. Default 50."),
        RPC_PARAM_OPT("allowBlank", "boolean", "Accept an intentionally near-uniform black frame. Default false; otherwise the call retries one redraw then returns BLANK_CAPTURE."),
        RPC_PARAM_OPT("allowPieWorld", "boolean", "Permit an active ejected/simulate Level Editor viewport whose world is a PIE world for the same map as the editor world. Defaults to false; another-map PIE and every non-PIE mismatch remain VIEWPORT_WORLD_MISMATCH."),
        FParamSpec{TEXT("orthoWidth"), TEXT("number"),
            TEXT("Orthographic frame width in WORLD CENTIMETRES (the world span the image covers left to right). Default 2000. Alias: orthoWorldWidth."),
            false, TEXT("2000"), TArray<FString>({TEXT("orthoWorldWidth")})},
        RPC_PARAM_OPT("viewDistanceScale", "number", "Force r.ViewDistanceScale for this capture and restore it afterwards. Must be > 0. Omit on an orthographic capture to derive it from the scene so distant foliage is not culled; omit on a perspective capture to leave the cvar alone."),
        RPC_PARAM_OPT("exposure", "object|number", PINWRIGHT_EXPOSURE_PARAM_DESC),
        RPC_PARAM_OPT("hideEditorSprites", "boolean", PINWRIGHT_HIDE_EDITOR_SPRITES_PARAM_DESC),
        RPC_PARAM_OPT("viewMode", "string", PINWRIGHT_VIEW_MODE_PARAM_DESC),
        RPC_PARAM_OPT("subject", "object",
            "What this frame is supposed to CONTAIN, as an object: {kind, name, point, radius}. `kind` is "
            "world or actor -- an asset kind is refused, because this verb captures the open level by its "
            "own name and an asset lives in its own editor preview. It does NOT move the camera: pass "
            "location/rotation for that. What it buys is the `framing` block -- the verdict, measured "
            "against the pose the renderer resolved to, on whether the subject is provably out of frame. "
            "`blank` cannot answer that question: a frame of empty landscape is neither black nor blank. "
            "Both `framing` and the `subject` block are present only when a subject was resolved.")
    ))
{
    return PinWrightOpenLevelCapture::Handle(Ctx);
}

// ---- render.attach_render_target_to_volume ----
REGISTER_RPC_HANDLER("render.attach_render_target_to_volume", "render", "Attach a render target to a post process volume via material",
    RPC_PARAMS(
        RPC_PARAM_OPT("volumePath", "path", "Path to the post process volume actor"),
        RPC_PARAM_OPT("targetPath", "path", "Path to the render target asset"),
        RPC_PARAM_OPT("materialPath", "path", "Path to the base material"),
        RPC_PARAM_OPT("parameterName", "string", "Name of the texture parameter to set")
    ))
{
    FString VolumePath = Ctx.GetString(TEXT("volumePath"));
    FString TargetPath = Ctx.GetString(TEXT("targetPath"));

    APostProcessVolume* Volume = Cast<APostProcessVolume>(FindObject<AActor>(nullptr, *VolumePath));
    if (!Volume)
    {
        Ctx.SendError(ErrorCodes::ERR_ACTOR_NOT_FOUND, TEXT("Volume not found."));
        return true;
    }

    UTextureRenderTarget2D* RT = LoadObject<UTextureRenderTarget2D>(nullptr, *TargetPath);
    if (!RT)
    {
        Ctx.SendError(ErrorCodes::ERR_ASSET_NOT_FOUND, TEXT("Render target not found."));
        return true;
    }

    FString MaterialPath = Ctx.GetString(TEXT("materialPath"));
    FString ParamName = Ctx.GetString(TEXT("parameterName"));

    if (MaterialPath.IsEmpty() || ParamName.IsEmpty())
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT, TEXT("materialPath and parameterName required."));
        return true;
    }

    UMaterialInterface* BaseMat = LoadObject<UMaterialInterface>(nullptr, *MaterialPath);
    if (!BaseMat)
    {
        Ctx.SendError(ErrorCodes::ERR_ASSET_NOT_FOUND, TEXT("Base material not found."));
        return true;
    }

    // Validate that the material actually exposes a texture parameter named ParamName
    // before binding. UMaterialInstanceDynamic::SetTextureParameterValue silently stores
    // an override for an unknown parameter that the shader never samples, so without this
    // check the render target would never drive the volume yet the caller would be told
    // attached:true. Enumerating GetAllParameterInfoOfType(Texture) reads the material's
    // cached parameter map, so it correctly includes TextureSampleParameter2D subclasses
    // whose parameter name differs from the expression object name -- a
    // Cast<UMaterialExpressionParameter> scan would miss those.
    TArray<FMaterialParameterInfo> TextureParamInfos;
    TArray<FGuid> TextureParamGuids;
    BaseMat->GetAllParameterInfoOfType(EMaterialParameterType::Texture, TextureParamInfos, TextureParamGuids);

    bool bParamExists = false;
    TArray<FString> ValidTextureParams;
    ValidTextureParams.Reserve(TextureParamInfos.Num());
    for (const FMaterialParameterInfo& Info : TextureParamInfos)
    {
        const FString InfoName = Info.Name.ToString();
        ValidTextureParams.Add(InfoName);
        if (InfoName.Equals(ParamName, ESearchCase::IgnoreCase))
        {
            bParamExists = true;
        }
    }

    if (!bParamExists)
    {
        TSharedPtr<FJsonObject> ErrResult = MakeShared<FJsonObject>();
        ErrResult->SetStringField(TEXT("materialPath"), MaterialPath);
        ErrResult->SetStringField(TEXT("parameterName"), ParamName);
        TArray<TSharedPtr<FJsonValue>> ValidArr;
        ValidArr.Reserve(ValidTextureParams.Num());
        for (const FString& Name : ValidTextureParams)
        {
            ValidArr.Add(MakeShared<FJsonValueString>(Name));
        }
        ErrResult->SetArrayField(TEXT("validTextureParameters"), ValidArr);
        const FString ValidList = ValidTextureParams.Num() > 0
            ? FString::Join(ValidTextureParams, TEXT(", "))
            : TEXT("(material exposes no texture parameters)");
        Ctx.SendError(ErrorCodes::ERR_PARAMETER_NOT_FOUND,
            FString::Printf(TEXT("Material '%s' has no texture parameter named '%s'. Valid texture parameters: %s."),
                *MaterialPath, *ParamName, *ValidList),
            ErrResult);
        return true;
    }

    UMaterialInstanceDynamic* MID = UMaterialInstanceDynamic::Create(BaseMat, Volume);
    if (MID)
    {
        MID->SetTextureParameterValue(FName(*ParamName), RT);
        Volume->Settings.AddBlendable(MID, 1.0f);
        TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
        Result->SetStringField(TEXT("renderTarget"), TargetPath);
        Result->SetStringField(TEXT("materialPath"), MaterialPath);
        Result->SetStringField(TEXT("parameterName"), ParamName);
        Result->SetBoolField(TEXT("attached"), true);
        AddActorVerification(Result, Volume);
        Ctx.SendSuccess(Result);
    }
    else
    {
        Ctx.SendError(ErrorCodes::ERR_CREATE_FAILED, TEXT("Failed to create MID."));
    }
    return true;
}

// ---- render.nanite_rebuild_mesh ----
namespace RenderNaniteRebuildMeshJob
{
    constexpr double DefaultCompilationTimeoutSeconds = 120.0;
    constexpr double MinCompilationTimeoutSeconds = 0.0;
    constexpr double MaxCompilationTimeoutSeconds = 120.0;

    class FRetainedCompileGuard : public TSharedFromThis<FRetainedCompileGuard>
    {
    public:
        FRetainedCompileGuard(
            TSharedPtr<PinWrightMeshRebuild::FQuiesceScope> InQuiesce,
            UStaticMesh* InMesh)
            : Quiesce(MoveTemp(InQuiesce))
            , Mesh(InMesh)
        {
        }

        void Start()
        {
            const TSharedRef<FRetainedCompileGuard> Self = AsShared();
            FTSTicker::GetCoreTicker().AddTicker(
                FTickerDelegate::CreateLambda([Self](float /*DeltaTime*/)
                {
                    if (Self->Mesh.IsValid()
                        && PinWright::AssetCompile::IsCompilingForBoundedWait(
                            Self->Mesh.Get(), /*bAllowTestOverride=*/true))
                    {
                        // The editor's normal asset-compilation tick owns progress here. This
                        // watcher only retains the render guard; it never pumps engine work after
                        // the job's retained request scope has ended.
                        return true;
                    }

                    Self->Quiesce.Reset();
                    Self->Mesh.Reset();
                    return false;
                }),
                0.05f);
        }

    private:
        TSharedPtr<PinWrightMeshRebuild::FQuiesceScope> Quiesce;
        TStrongObjectPtr<UStaticMesh> Mesh;
    };

    class FState
    {
    public:
        FState(FString InAssetPath, const bool bInSave, const double InTimeoutSeconds)
            : AssetPath(MoveTemp(InAssetPath))
            , bSave(bInSave)
            , TimeoutSeconds(InTimeoutSeconds)
        {
        }

        void Start(FJobOnComplete InOnComplete)
        {
            if (bResolved)
            {
                return;
            }
            OnComplete = MoveTemp(InOnComplete);
            DeadlineSeconds = FPlatformTime::Seconds() + TimeoutSeconds;

            UStaticMesh* LoadedMesh = PinWrightMeshRebuild::ResolveStaticMesh(AssetPath);
            if (!LoadedMesh)
            {
                Resolve(false, nullptr, ErrorCodes::ERR_ASSET_NOT_FOUND);
                return;
            }
            StaticMesh.Reset(LoadedMesh);

            const TArray<UStaticMesh*> RebuildMeshes = {LoadedMesh};
            const TArray<UActorComponent*> RenderConsumers =
                PinWrightMeshRebuild::ScanForStaticMeshRebuildConsumers(RebuildMeshes);
            Quiesce = MakeShared<PinWrightMeshRebuild::FQuiesceScope>(RenderConsumers);
            if (Quiesce->NotQuiesced().Num() > 0)
            {
                TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
                Result->SetStringField(TEXT("assetPath"), AssetPath);
                Result->SetBoolField(TEXT("rebuilt"), false);
                Result->SetStringField(TEXT("errorCode"),
                    ErrorCodes::ERR_MESH_REBUILD_CONSUMER_NOT_QUIESCABLE);
                AddAssetSaveReport(
                    Result, bSave, /*bSavedToDisk=*/false, EAssetSaveState::Failed);
                Resolve(false, Result, FString::Printf(
                    TEXT("%s: refusing to rebuild StaticMesh '%s' because %d live component(s) "
                         "could not be quiesced (%s)"),
                    ErrorCodes::ERR_MESH_REBUILD_CONSUMER_NOT_QUIESCABLE,
                    *AssetPath, Quiesce->NotQuiesced().Num(),
                    *FString::Join(Quiesce->NotQuiesced(), TEXT(", "))));
                return;
            }

#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 7, 0)
            FMeshNaniteSettings Settings = StaticMesh->GetNaniteSettings();
            Settings.bEnabled = true;
            StaticMesh->SetNaniteSettings(Settings);
#else
            StaticMesh->NaniteSettings.bEnabled = true;
#endif

            StaticMesh->Build(/*bSilent=*/true);
            StaticMesh->MarkPackageDirty();

            if (!WaitForCompilation())
            {
                if (!StaticMesh.IsValid() || !IsValid(StaticMesh.Get()))
                {
                    ResolveInvalidMesh();
                    return;
                }

                RetainGuardUntilCompilationStops(/*bForceRetention=*/true);
                TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
                Result->SetStringField(TEXT("assetPath"), AssetPath);
                Result->SetBoolField(TEXT("rebuilt"), false);
                Result->SetBoolField(TEXT("timedOut"), true);
                Result->SetNumberField(TEXT("timeoutSeconds"), TimeoutSeconds);
                AddAssetSaveReport(
                    Result, /*bSaveRequested=*/false, /*bSavedToDisk=*/false,
                    EAssetSaveState::NotRequested);
                Resolve(false, Result, FString::Printf(
                    TEXT("%s: StaticMesh '%s' was still compiling after %.2f seconds; "
                         "no Nanite save was attempted"),
                    ErrorCodes::ERR_OPERATION_FAILED, *AssetPath, TimeoutSeconds));
                return;
            }

            CompleteAfterCompilation();
        }

        void Cancel()
        {
            if (bResolved)
            {
                return;
            }
            bResolved = true;
            RetainGuardUntilCompilationStops(/*bForceRetention=*/false);
            Quiesce.Reset();
            StaticMesh.Reset();
            OnComplete = FJobOnComplete();
        }

        void FailBeforeStart(FJobOnComplete InOnComplete)
        {
            if (bResolved)
            {
                return;
            }
            bResolved = true;
            if (InOnComplete)
            {
                InOnComplete(false, nullptr, FString::Printf(
                    TEXT("%s: dispatcher/request context ended before the deferred Nanite rebuild began"),
                    ErrorCodes::ERR_OPERATION_FAILED));
            }
        }

    private:
        bool WaitForCompilation()
        {
            while (PinWright::AssetCompile::IsCompilingForBoundedWait(
                StaticMesh.Get(), /*bAllowTestOverride=*/true))
            {
                if (!StaticMesh.IsValid() || !IsValid(StaticMesh.Get())
                    || FPlatformTime::Seconds() >= DeadlineSeconds)
                {
                    return false;
                }

                PinWright::AssetCompile::AdvanceOnGameThread();
                if (!PinWright::AssetCompile::IsCompilingForBoundedWait(
                    StaticMesh.Get(), /*bAllowTestOverride=*/true))
                {
                    return FPlatformTime::Seconds() < DeadlineSeconds;
                }

                const double RemainingSeconds = DeadlineSeconds - FPlatformTime::Seconds();
                if (RemainingSeconds <= 0.0)
                {
                    return false;
                }
                FPlatformProcess::SleepNoStats(static_cast<float>(
                    FMath::Min(RemainingSeconds, 0.01)));
            }
            return FPlatformTime::Seconds() < DeadlineSeconds;
        }

        void CompleteAfterCompilation()
        {
            if (bResolved)
            {
                return;
            }
            if (!StaticMesh.IsValid() || !IsValid(StaticMesh.Get()))
            {
                ResolveInvalidMesh();
                return;
            }

            UStaticMesh* Mesh = StaticMesh.Get();
            FString PackageName = Mesh->GetOutermost()->GetName();
            int64 SizeBytes = 0;
            bool bSavedToDisk = false;
            EAssetSaveState SaveState = EAssetSaveState::NotRequested;
            if (bSave)
            {
                bSavedToDisk = SaveAssetToDiskReportingPresence(
                    Mesh, /*bForce=*/true, &PackageName, &SizeBytes, &SaveState);
            }
            else
            {
                FString PackageFilename;
                if (FPackageName::TryConvertLongPackageNameToFilename(
                        PackageName, PackageFilename, FPackageName::GetAssetPackageExtension()))
                {
                    SizeBytes = FMath::Max<int64>(IFileManager::Get().FileSize(*PackageFilename), 0);
                }
            }

            TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
            Result->SetStringField(TEXT("assetPath"), AssetPath);
            Result->SetBoolField(TEXT("naniteEnabled"), true);
            Result->SetBoolField(TEXT("rebuilt"), true);
            Result->SetStringField(TEXT("package"), PackageName);
            AddAssetSaveSizeReport(Result, SizeBytes, bSavedToDisk);
            AddAssetSaveReport(Result, bSave, bSavedToDisk, SaveState);
            Resolve(true, Result, FString());
        }

        void Resolve(const bool bSuccess, TSharedPtr<FJsonObject> Result, FString Error)
        {
            if (bResolved)
            {
                return;
            }
            bResolved = true;

            FJobOnComplete Completion = MoveTemp(OnComplete);
            if (Completion)
            {
                Completion(bSuccess, MoveTemp(Result), MoveTemp(Error));
            }
            Quiesce.Reset();
            StaticMesh.Reset();
        }

        void ResolveInvalidMesh()
        {
            TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
            Result->SetStringField(TEXT("assetPath"), AssetPath);
            Result->SetBoolField(TEXT("rebuilt"), false);
            Result->SetStringField(TEXT("errorCode"), ErrorCodes::ERR_INVALID_ASSET);
            AddAssetSaveReport(Result, /*bSaveRequested=*/false,
                /*bSavedToDisk=*/false, EAssetSaveState::Failed);
            Resolve(false, Result, FString::Printf(
                TEXT("%s: retained StaticMesh '%s' became invalid before completion"),
                ErrorCodes::ERR_INVALID_ASSET, *AssetPath));
        }

        void RetainGuardUntilCompilationStops(const bool bForceRetention)
        {
            if (!StaticMesh.IsValid())
            {
                return;
            }

            if (StaticMesh->IsCompiling())
            {
                UObject* MeshObject = StaticMesh.Get();
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 6, 0)
                FAssetCompilingManager::Get().MarkCompilationAsCanceled({MeshObject});
#endif
            }
            if (bForceRetention || StaticMesh->IsCompiling())
            {
                MakeShared<FRetainedCompileGuard>(MoveTemp(Quiesce), StaticMesh.Get())->Start();
            }
        }

        FString AssetPath;
        bool bSave = true;
        bool bResolved = false;
        double TimeoutSeconds = DefaultCompilationTimeoutSeconds;
        double DeadlineSeconds = 0.0;
        FJobOnComplete OnComplete;
        TSharedPtr<PinWrightMeshRebuild::FQuiesceScope> Quiesce;
        TStrongObjectPtr<UStaticMesh> StaticMesh;
    };
}

REGISTER_RPC_HANDLER("render.nanite_rebuild_mesh", "render", "Enable Nanite and rebuild a static mesh",
    RPC_PARAMS(
        RPC_PARAM_REQ("assetPath", "path", "Path to the static mesh asset"),
        RPC_PARAM_OPT("save", "boolean", "Persist the rebuilt StaticMesh to disk (default true)."),
        RPC_PARAM_DEF("timeoutSeconds", "number", "Maximum time to wait for StaticMesh compilation (0-120 seconds). A timeout does not save.", "120")
    ))
{
    FString AssetPath = Ctx.GetString(TEXT("assetPath"));
    if (AssetPath.IsEmpty())
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT, TEXT("assetPath required."));
        return true;
    }
    const bool bSave = Ctx.GetBool(TEXT("save"), true);
    double TimeoutSeconds = RenderNaniteRebuildMeshJob::DefaultCompilationTimeoutSeconds;
    if (Ctx.GetRawPayload()->HasField(TEXT("timeoutSeconds")))
    {
        if (!Ctx.RequireNumber(TEXT("timeoutSeconds"), TimeoutSeconds))
        {
            return true;
        }
        if (!FMath::IsFinite(TimeoutSeconds)
            || TimeoutSeconds < RenderNaniteRebuildMeshJob::MinCompilationTimeoutSeconds
            || TimeoutSeconds > RenderNaniteRebuildMeshJob::MaxCompilationTimeoutSeconds)
        {
            Ctx.SendError(ErrorCodes::ERR_INVALID_PARAMS,
                FString::Printf(TEXT("timeoutSeconds must be between %.2f and %.0f."),
                    RenderNaniteRebuildMeshJob::MinCompilationTimeoutSeconds,
                    RenderNaniteRebuildMeshJob::MaxCompilationTimeoutSeconds));
            return true;
        }
    }

    TSharedRef<RenderNaniteRebuildMeshJob::FState> State =
        MakeShared<RenderNaniteRebuildMeshJob::FState>(AssetPath, bSave, TimeoutSeconds);

    FJobBindArgs Args;
    Args.Method = TEXT("render.nanite_rebuild_mesh");
    Args.StartedPayload = MakeShared<FJsonObject>();
    Args.StartedPayload->SetStringField(TEXT("assetPath"), AssetPath);

    Args.BindNativeDelegate =
        [Ctx, State](FJobOnComplete OnComplete)
    {
        PinWrightSafePoint::DeferJobToSafePoint(Ctx, TEXT("render.nanite_rebuild_mesh"),
            [State, OnComplete]() mutable
            {
                State->Start(MoveTemp(OnComplete));
            },
            [State, OnComplete]() mutable
            {
                State->FailBeforeStart(MoveTemp(OnComplete));
            });
    };

    const FString TicketId = Ctx.StartJob(Args);
    TWeakPtr<RenderNaniteRebuildMeshJob::FState> WeakState(State);
    FPluginState::Get().GetJobRegistry().SetCancelCallback(
        TicketId,
        [WeakState]()
        {
            if (const TSharedPtr<RenderNaniteRebuildMeshJob::FState> Pinned = WeakState.Pin())
            {
                Pinned->Cancel();
            }
        });
    return true;
}

// ---- render.lumen_update_scene ----
REGISTER_RPC_HANDLER("render.lumen_update_scene", "render", "Trigger a Lumen scene recapture",
    RPC_NO_PARAMS)
{
    if (GEditor)
    {
        UWorld* World = GEditor->GetEditorWorldContext().World();
        if (World)
        {
            // r.LumenScene.SurfaceCache.Reset 1 is the real one-shot Lumen
            // recapture lever: the renderer resets all atlases/captured cards on
            // the next frame and auto-clears the cvar back to 0
            // (Engine/Source/Runtime/Renderer/Private/Lumen/LumenSceneRendering.cpp).
            // The former r.Lumen.Scene.Recapture does not exist in UE 5.x, so Exec
            // returned false and the handler reported a recapture that never ran.
            // Honor the Exec bool and fail loud on rejection, matching system.console_command.
            const TCHAR* Command = TEXT("r.LumenScene.SurfaceCache.Reset 1");
            const bool bOk = GEngine->Exec(World, Command);
            if (!bOk)
            {
                Ctx.SendError(ErrorCodes::ERR_EXEC_FAILED, TEXT("Command not executed"));
                return true;
            }
            // The !bOk early return above already encoded the false case as
            // EXEC_FAILED, so reaching here means bOk is true.
            TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
            Result->SetStringField(TEXT("action"), TEXT("lumen_update_scene"));
            Result->SetStringField(TEXT("command"), Command);
            Result->SetBoolField(TEXT("executed"), true);
            Ctx.SendSuccess(Result);
            return true;
        }
    }
    Ctx.SendError(ErrorCodes::ERR_EXECUTION_FAILED, TEXT("Could not execute command (no world context)."));
    return true;
}
