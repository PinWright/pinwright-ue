// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Misc/AutomationTest.h"

#include "Handlers/Render/CaptureSubject.h"
#include "Handlers/Render/CaptureSubjectProviders_Mesh.h"
#include "Handlers/Render/FlatRegionStats.h"
#include "Handlers/Render/PoseListCapture.h"
#include "Tests/TestAssetTeardown.h"
#include "Tests/TestSkipReporting.h"
#include "Utils/ScreenshotUtils.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Compat/EngineVersionCompat.h"
#include "Dom/JsonObject.h"
#include "Editor.h"
#include "EditorViewportClient.h"
#include "Engine/StaticMesh.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/App.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "RHI.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "UObject/Package.h"

namespace PoseListCaptureStabilityTestLocal
{
    const TCHAR* const CubePath = TEXT("/Engine/BasicShapes/Cube.Cube");

    struct FPairDiagnostics
    {
        int32 MaxDelta = 0;
        int32 MaxX = -1;
        int32 MaxY = -1;
        TCHAR MaxChannel = TEXT('r');
        double ChangedFraction = 0.0;
        int32 ChangedMinX = -1;
        int32 ChangedMinY = -1;
        int32 ChangedMaxX = -1;
        int32 ChangedMaxY = -1;
    };

    const TCHAR* BoolText(bool bValue)
    {
        return bValue ? TEXT("true") : TEXT("false");
    }

    FString ConsoleVariableValue(const TCHAR* Name)
    {
        const IConsoleVariable* Variable = IConsoleManager::Get().FindConsoleVariable(Name);
        return Variable ? Variable->GetString() : FString(TEXT("<unregistered>"));
    }

    FPairDiagnostics ScanPair(const TArray<FColor>& Before, const TArray<FColor>& After,
        int32 Width, int32 Height)
    {
        FPairDiagnostics Diagnostics;
        const int32 ExpectedPixels = Width > 0 && Height > 0 ? Width * Height : 0;
        const int32 PixelCount = FMath::Min(ExpectedPixels,
            FMath::Min(Before.Num(), After.Num()));
        if (PixelCount <= 0)
        {
            return Diagnostics;
        }

        int32 ChangedPixels = 0;
        int32 MinX = Width;
        int32 MinY = Height;
        int32 MaxX = -1;
        int32 MaxY = -1;
        const TCHAR Channels[3] = { TEXT('r'), TEXT('g'), TEXT('b') };
        for (int32 PixelIndex = 0; PixelIndex < PixelCount; ++PixelIndex)
        {
            const FColor& A = Before[PixelIndex];
            const FColor& B = After[PixelIndex];
            const int32 Deltas[3] =
            {
                FMath::Abs(static_cast<int32>(B.R) - static_cast<int32>(A.R)),
                FMath::Abs(static_cast<int32>(B.G) - static_cast<int32>(A.G)),
                FMath::Abs(static_cast<int32>(B.B) - static_cast<int32>(A.B))
            };
            const int32 X = PixelIndex % Width;
            const int32 Y = PixelIndex / Width;
            if (Deltas[0] != 0 || Deltas[1] != 0 || Deltas[2] != 0)
            {
                ++ChangedPixels;
                MinX = FMath::Min(MinX, X);
                MinY = FMath::Min(MinY, Y);
                MaxX = FMath::Max(MaxX, X);
                MaxY = FMath::Max(MaxY, Y);
            }
            for (int32 ChannelIndex = 0; ChannelIndex < 3; ++ChannelIndex)
            {
                // Strictly greater preserves the first pixel and r/g/b channel in scan order on
                // ties, so two runs with the same buffers name the same diagnostic location.
                if (Deltas[ChannelIndex] > Diagnostics.MaxDelta)
                {
                    Diagnostics.MaxDelta = Deltas[ChannelIndex];
                    Diagnostics.MaxX = X;
                    Diagnostics.MaxY = Y;
                    Diagnostics.MaxChannel = Channels[ChannelIndex];
                }
            }
        }

        Diagnostics.ChangedFraction = static_cast<double>(ChangedPixels)
            / static_cast<double>(PixelCount);
        if (ChangedPixels > 0)
        {
            Diagnostics.ChangedMinX = MinX;
            Diagnostics.ChangedMinY = MinY;
            Diagnostics.ChangedMaxX = MaxX;
            Diagnostics.ChangedMaxY = MaxY;
        }
        return Diagnostics;
    }

    TArray<FColor> MakeHalfChangedFrame(bool bChanged)
    {
        TArray<FColor> Pixels;
        Pixels.Init(FColor(100, 100, 100, 255), 16 * 16);
        if (bChanged)
        {
            for (int32 Y = 8; Y < 16; ++Y)
            {
                for (int32 X = 0; X < 16; ++X)
                {
                    Pixels[Y * 16 + X] = FColor(120, 120, 120, 255);
                }
            }
        }
        return Pixels;
    }

    FString PoseListSourcePath()
    {
        const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("PinWright"));
        return Plugin.IsValid()
            ? Plugin->GetBaseDir() / TEXT("Source/PinWright/Private/Handlers/Render/PoseListCapture.cpp")
            : FString();
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPixelConvergenceDetectsLocalChangeTest,
    "PinWright.render.capture.PixelConvergenceDetectsLocalChange",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPixelConvergenceDetectsLocalChangeTest::RunTest(const FString& Parameters)
{
    const TArray<FColor> Before =
        PoseListCaptureStabilityTestLocal::MakeHalfChangedFrame(false);
    const TArray<FColor> After =
        PoseListCaptureStabilityTestLocal::MakeHalfChangedFrame(true);
    const PinWrightFlatRegion::FFrameDifferenceStats Difference =
        PinWrightFlatRegion::MeasureFrameDifference(Before, After, 1);
    TestTrue(TEXT("same-sized buffers are measured"), Difference.bMeasured);
    TestEqual(TEXT("a half-frame filled-face change has mean delta ten"),
        Difference.MeanAbsDelta, 10.0);
    TestEqual(TEXT("the largest channel delta is retained"), Difference.MaxDelta, 20);
    TestEqual(TEXT("half the pixels exceed the one-level floor"),
        Difference.ChangedPixelFraction, 0.5);

    const PinWrightFlatRegion::FFlatRegionStats Region =
        PinWrightFlatRegion::MeasureLargestFlatRegion(After, 16, 16);
    TestTrue(TEXT("the changed fixture has measurable spatial regions"), Region.bMeasured);
    TestEqual(TEXT("the filled-face fixture's largest flat region is half the frame"),
        Region.LargestRegionFraction, 0.5);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPoseRepeatabilityEvidenceTest,
    "PinWright.render.pose_list.PoseRepeatabilityEvidence",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPoseRepeatabilityEvidenceTest::RunTest(const FString& Parameters)
{
    PinWrightPoseCapture::FPoseListCaptureRequest Request;
    Request.Poses.SetNum(2);
    Request.bWarmupShot = true;
    Request.bMeasurePoseRepeatability = true;
    Request.bMeasureSubjectCoverage = true;
    Request.SubjectVisibilitySetter = [](bool /*bVisible*/, FString& /*OutErrorCode*/,
                                          FString& /*OutErrorMessage*/)
    {
        return true;
    };

    int32 CaptureCalls = 0;
    TArray<FString> DisposableRequestedFilenames;
    const PinWrightPoseCapture::FPoseFrameCapturer Capturer =
        [&CaptureCalls, &DisposableRequestedFilenames](
            const PinWrightRenderCapture::FViewportCaptureRequest& Frame,
            bool bWarmupFrame,
            PinWrightRenderCapture::FViewportCaptureOutput& OutCapture,
            FString& OutErrorCode,
            FString& OutErrorMessage)
        {
            ++CaptureCalls;
            if (bWarmupFrame)
            {
                DisposableRequestedFilenames.Add(Frame.Filename);
            }
            OutCapture.Width = 16;
            OutCapture.Height = 16;
            if (Frame.bRetainPixels)
            {
                const bool bControl = bWarmupFrame && CaptureCalls == 6;
                OutCapture.Pixels =
                    PoseListCaptureStabilityTestLocal::MakeHalfChangedFrame(bControl);
            }
            return true;
        };

    PinWrightPoseCapture::FPoseListCaptureOutput Output;
    FString ErrorCode;
    FString ErrorMessage;
    if (!TestTrue(TEXT("the injected pose set captures"),
            PinWrightPoseCapture::RunPoseListCapture(
                Request, Capturer, Output, ErrorCode, ErrorMessage)))
    {
        return false;
    }
    TestEqual(TEXT("warm-up, two references, two real shots, and one control were drawn"),
        CaptureCalls, 6);
    TestEqual(TEXT("all disposable captures were observed"),
        DisposableRequestedFilenames.Num(), 4);
    if (DisposableRequestedFilenames.Num() == 4)
    {
        TestFalse(TEXT("the warm-up carries its unique discard filename"),
            DisposableRequestedFilenames[0].IsEmpty());
        for (int32 Index = 1; Index < DisposableRequestedFilenames.Num(); ++Index)
        {
            TestTrue(TEXT("reference and control captures use collision-checked generated names"),
                DisposableRequestedFilenames[Index].IsEmpty());
        }
    }
    TestTrue(TEXT("the control shot was taken"), Output.bPoseRepeatabilityControlShotTaken);
    TestTrue(TEXT("the control comparison was measured"), Output.bPoseRepeatabilityMeasured);
    TestEqual(TEXT("the set reports mean absolute delta"),
        Output.PoseRepeatabilityMeanAbsDelta, 10.0);
    TestEqual(TEXT("the set reports max delta"), Output.PoseRepeatabilityMaxDelta, 20);
    TestEqual(TEXT("the set reports changed-pixel fraction"),
        Output.PoseRepeatabilityChangedPixelFraction, 0.5);
    TestEqual(TEXT("temporary pose-0 pixels are released"), Output.Captures[0].Pixels.Num(), 0);

    const TSharedPtr<FJsonObject> PoseSet =
        PinWrightPoseCapture::MakePoseSetInfoObject(Output);
    const TSharedPtr<FJsonObject> Repeatability =
        PoseSet->GetObjectField(TEXT("poseRepeatability"));
    TestTrue(TEXT("poseSet publishes the measured control"),
        Repeatability->GetBoolField(TEXT("measured")));
    TestEqual(TEXT("poseSet publishes the exact max delta"),
        Repeatability->GetIntegerField(TEXT("maxDelta")), 20);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPreviewRigScopedOnceTest,
    "PinWright.render.pose_list.PreviewRigScopedOnce",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPreviewRigScopedOnceTest::RunTest(const FString& Parameters)
{
    const FString Path = PoseListCaptureStabilityTestLocal::PoseListSourcePath();
    FString Source;
    if (!TestTrue(TEXT("the pose-list source is readable"), !Path.IsEmpty()
            && FFileHelper::LoadFileToString(Source, *Path)))
    {
        return false;
    }

    const int32 WrapperAt = Source.Find(TEXT("bool CaptureCameraPoses("),
        ESearchCase::CaseSensitive);
    const int32 RigScopeAt = Source.Find(TEXT("FScopedPreviewSceneRig RigScope"),
        ESearchCase::CaseSensitive, ESearchDir::FromStart, WrapperAt);
    const int32 RunSetAt = Source.Find(TEXT("bCaptureSucceeded = RunPoseListCapture("),
        ESearchCase::CaseSensitive, ESearchDir::FromStart, RigScopeAt);
    const int32 ScopedHandoffAt = Source.Find(
        TEXT("ScopedRequest.bPreviewSceneRigAlreadyScoped = true"),
        ESearchCase::CaseSensitive, ESearchDir::FromStart, RigScopeAt);
    TestTrue(TEXT("the rig scope begins inside CaptureCameraPoses"),
        WrapperAt != INDEX_NONE && RigScopeAt > WrapperAt);
    TestTrue(TEXT("the rig scope is constructed before the set runs"),
        RunSetAt > RigScopeAt);
    TestTrue(TEXT("individual frames are marked as already scoped"),
        ScopedHandoffAt > RigScopeAt && ScopedHandoffAt < RunSetAt);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEightIdenticalPosesAdjacentMaxDeltaTest,
    "PinWright.render.pose_list.EightIdenticalPosesAdjacentMaxDelta",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FEightIdenticalPosesAdjacentMaxDeltaTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightCaptureSubject;
    using namespace PoseListCaptureStabilityTestLocal;

    if (!FApp::CanEverRender() || !GDynamicRHI)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-rhi"),
            TEXT("The host cannot create rendered frames or has no dynamic RHI."));
        return true;
    }
    if (!TestTrue(TEXT("Slate is initialized for the preview capture"),
            FSlateApplication::IsInitialized()))
    {
        return false;
    }
    if (!TestNotNull(TEXT("GEditor is available for the preview capture"), GEditor))
    {
        return false;
    }

    UStaticMesh* SourceCube = LoadObject<UStaticMesh>(nullptr,
        PoseListCaptureStabilityTestLocal::CubePath);
    UAssetEditorSubsystem* AssetEditors =
        GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();
    if (!TestNotNull(TEXT("the engine Cube source fixture is available"), SourceCube)
        || !TestNotNull(TEXT("the AssetEditorSubsystem is available"), AssetEditors))
    {
        return false;
    }

    const FString MeshPackagePath = FString::Printf(
        TEXT("/Game/__PW_GatewayTests/PWPoseRepeatability_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    UPackage* MeshPackage = CreatePackage(*MeshPackagePath);
    if (!TestNotNull(TEXT("the unique preview mesh package is created"), MeshPackage))
    {
        return false;
    }
    UStaticMesh* TestMesh = DuplicateObject<UStaticMesh>(SourceCube, MeshPackage,
        FName(*FPackageName::GetLongPackageAssetName(MeshPackagePath)));
    if (!TestNotNull(TEXT("the unique preview mesh fixture is created"), TestMesh))
    {
        MeshPackage->SetFlags(RF_Transient);
        FAssetRegistryModule::PackageDeleted(MeshPackage);
        MeshPackage->ClearFlags(RF_Standalone | RF_Public);
        MeshPackage->SetDirtyFlag(false);
        MeshPackage->MarkAsGarbage();
        return false;
    }
    FAssetRegistryModule::AssetCreated(TestMesh);
    const FString MeshObjectPath = TestMesh->GetPathName();

    FAssetEditorViewportAcquisition Acquisition;
    PinWrightPoseCapture::FPoseListCaptureOutput Output;
    ON_SCOPE_EXIT
    {
        for (const PinWrightRenderCapture::FViewportCaptureOutput& Capture : Output.Captures)
        {
            if (!Capture.Path.IsEmpty())
            {
                IFileManager::Get().Delete(*Capture.Path, /*RequireExists=*/false,
                    /*EvenReadOnly=*/true, /*Quiet=*/true);
            }
        }
        Acquisition.ViewportClient = nullptr;
        Acquisition.SceneViewport.Reset();
        Acquisition.ViewportWidget.Reset();
        AssetEditors->CloseAllEditorsForAsset(TestMesh);
        PwTestAssetTeardown::DiscardCreatedAssetByObjectPath(MeshObjectPath);
    };

    FString ErrorCode;
    FString ErrorMessage;
    const bool bAcquired = AcquireAssetEditorViewport(TestMesh,
        PinWrightCaptureSubjectMesh::StaticMeshToolkitNames(), Acquisition,
        ErrorCode, ErrorMessage);
    if (!TestTrue(
            FString::Printf(TEXT("the unique preview mesh viewport is acquired (%s: %s)"),
                *ErrorCode, *ErrorMessage), bAcquired))
    {
        AddError(FString::Printf(TEXT("Preview viewport acquisition failed (%s: %s)."),
            *ErrorCode, *ErrorMessage));
        return false;
    }
    if (!TestFalse(TEXT("the GUID-named preview mesh editor was not already open"),
            Acquisition.bWasAlreadyOpen))
    {
        return false;
    }
    if (!Acquisition.ViewportClient || !Acquisition.SceneViewport.IsValid())
    {
        AddError(TEXT("Preview mesh acquisition succeeded without a usable client and viewport."));
        return false;
    }

    FEditorViewportClient& Client = *Acquisition.ViewportClient;
    PinWrightPoseCapture::FPoseListCaptureRequest Request;
    Request.Width = 96;
    Request.Height = 96;
    Request.FilenamePrefix = TEXT("PoseSetAdjacentRepeatability");
    Request.Subdirectory = TEXT("PinWrightTests");
    Request.bRetainPixels = true;
    Request.bRejectBlankCapture = true;
    Request.Exposure.Mode = PinWrightRenderCapture::EExposureRequestMode::Fixed;
    Request.Exposure.Ev100 = 0.0f;
    Request.ViewMode.bRequested = true;
    Request.ViewMode.ViewMode = VMI_Lit;
    Request.ViewMode.Key = TEXT("lit");
    Request.PreviewSceneRig.bRequested = true;
    Request.PreviewSceneRig.bKeyAimProvided = true;
    Request.PreviewSceneRig.KeyAzimuthDegrees = 112.5;
    Request.PreviewSceneRig.KeyElevationDegrees = 40.0;
    Request.PreviewSceneRig.bKeyIntensityProvided = true;
    Request.PreviewSceneRig.KeyIntensity = 3.0;
    Request.PreviewSceneRig.bSkyIntensityProvided = true;
    Request.PreviewSceneRig.SkyIntensity = 1.0;
    Request.PreviewSceneRig.bShowFloorProvided = true;
    Request.PreviewSceneRig.bShowFloor = true;
    Request.PreviewSceneRig.bShowEnvironmentProvided = true;
    Request.PreviewSceneRig.bShowEnvironment = true;

    PinWrightPoseCapture::FCameraPose Pose;
    Pose.Location = Client.GetViewLocation();
    Pose.Rotation = Client.GetViewRotation();
    Pose.Fov = Client.ViewFOV;
    const FString RunId = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    for (int32 Index = 0; Index < 8; ++Index)
    {
        Pose.Filename = FString::Printf(TEXT("PoseSetAdjacent_%s_%02d.png"),
            *RunId, Index);
        Request.Poses.Add(Pose);
    }

    const bool bCaptureSucceeded = PinWrightPoseCapture::CaptureCameraPoses(
        Client, Acquisition.SceneViewport, Request, Output, ErrorCode, ErrorMessage);
    const FString DiagnosticDirectory = FPaths::Combine(FPaths::ProjectSavedDir(),
        TEXT("PinWrightTests"), TEXT("pose_list_stability"), RunId);
    IFileManager& FileManager = IFileManager::Get();
    const bool bDiagnosticDirectoryReady = FileManager.DirectoryExists(*DiagnosticDirectory)
        || FileManager.MakeDirectory(*DiagnosticDirectory, /*Tree=*/true);
    TestTrue(FString::Printf(TEXT("diagnostic directory is writable: %s"),
        *DiagnosticDirectory), bDiagnosticDirectoryReady);

    const int32 DiagnosticCaptureCount = FMath::Min(Output.Captures.Num(), 8);
    for (int32 Index = 0; Index < DiagnosticCaptureCount; ++Index)
    {
        const PinWrightRenderCapture::FViewportCaptureOutput& Capture = Output.Captures[Index];
        const FString DiagnosticPath = DiagnosticDirectory
            / FString::Printf(TEXT("shot_%d.png"), Index);
        const int64 ExpectedPixelCount = static_cast<int64>(Capture.Width) * Capture.Height;
        const bool bValidBuffer = Capture.Width > 0 && Capture.Height > 0
            && ExpectedPixelCount <= MAX_int32 && Capture.Pixels.Num() == ExpectedPixelCount;
        TArray<uint8> PngBytes;
        const bool bEncoded = bValidBuffer
            && PinWrightScreenshotUtils::EncodeBitmapToPng(
                Capture.Width, Capture.Height, Capture.Pixels, PngBytes);
        const bool bWritten = bEncoded
            && FFileHelper::SaveArrayToFile(PngBytes, *DiagnosticPath);
        TestTrue(FString::Printf(TEXT("diagnostic shot %d has valid dimensions and pixels"),
            Index), bValidBuffer);
        TestTrue(FString::Printf(TEXT("diagnostic shot %d encodes as PNG"), Index), bEncoded);
        TestTrue(FString::Printf(TEXT("diagnostic shot %d is written"), Index), bWritten);
        UE_LOG(LogTemp, Display, TEXT("PoseListStability shot=%d path=%s written=%s"),
            Index, *DiagnosticPath, BoolText(bWritten));
    }

    UE_LOG(LogTemp, Display,
        TEXT("PoseListStability eyeAdaptationCvars ")
        TEXT("r.EyeAdaptation.PreExposureOverride=%s ")
        TEXT("r.EyeAdaptation.MethodOverride=%s ")
        TEXT("r.EyeAdaptation.LensAttenuation=%s ")
        TEXT("r.EyeAdaptation.CachedLightingPreExposure=%s"),
        *ConsoleVariableValue(TEXT("r.EyeAdaptation.PreExposureOverride")),
        *ConsoleVariableValue(TEXT("r.EyeAdaptation.MethodOverride")),
        *ConsoleVariableValue(TEXT("r.EyeAdaptation.LensAttenuation")),
        *ConsoleVariableValue(TEXT("r.EyeAdaptation.CachedLightingPreExposure")));
    // FEditorViewportClient::GetShowWidget() arrived in UE 5.4; on 5.3 the bShowWidget it reads is
    // protected with no accessor, so this one diagnostic field is unreadable there.
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 4, 0)
    const TCHAR* const ShowWidgetText = BoolText(Client.GetShowWidget());
#else
    const TCHAR* const ShowWidgetText = TEXT("unavailable");
#endif
    UE_LOG(LogTemp, Display,
        TEXT("PoseListStability viewportHud drawAxes=%s drawAxesGame=%s showWidget=%s showStats=%s"),
        BoolText(Client.bDrawAxes), BoolText(Client.bDrawAxesGame),
        ShowWidgetText, BoolText(Client.ShouldShowStats()));

    for (int32 Index = 0; Index < DiagnosticCaptureCount; ++Index)
    {
        const PinWrightRenderCapture::FViewportCaptureOutput& Capture = Output.Captures[Index];
        const FGameViewOverlayShowFlags& Overlays = Capture.OverlayShowFlags;
        // CaptureCameraPoses constructs the set context and forwards its pointer to every frame;
        // there is no separate output boolean for that internal production path.
        UE_LOG(LogTemp, Display,
            TEXT("PoseListStability capture=%d output=%dx%d render=%dx%d ")
            TEXT("resolutionPinned=%s primaryFraction=%.9f secondaryFraction=%.9f ")
            TEXT("resolutionFraction=%.9f screenPercentage=%d exposurePinned=%s ")
            TEXT("ev100Requested=%.9f ev100Applied=%.9f taaSuppressed=%s aa=%s ")
            TEXT("setScopedPinPath=true gameView=%s realtime=%s overlayMeasured=%s ")
            TEXT("splines=%s billboardSprites=%s selection=%s selectionOutline=%s grid=%s ")
            TEXT("volumes=%s lightRadius=%s audioRadius=%s modeWidgets=%s navigation=%s game=%s ")
            TEXT("hideEditorSpritesRequested=%s billboardSpritesApplied=%s forcedShowFlags=%d"),
            Index, Capture.Width, Capture.Height, Capture.RenderWidth, Capture.RenderHeight,
            BoolText(Capture.bRenderResolutionPinned), Capture.RenderPrimaryResolutionFraction,
            Capture.RenderSecondaryResolutionFraction, Capture.RenderResolutionFraction,
            Capture.RenderScreenPercentage, BoolText(Capture.bExposurePinned),
            Capture.Ev100Requested, Capture.Ev100Applied,
            BoolText(Capture.bTemporalAntiAliasingSuppressed), *Capture.RenderAntiAliasingMethod,
            BoolText(Capture.bGameView), BoolText(Capture.bRealtime),
            BoolText(Capture.bOverlayShowFlagsMeasured), BoolText(Overlays.bSplines),
            BoolText(Overlays.bBillboardSprites), BoolText(Overlays.bSelection),
            BoolText(Overlays.bSelectionOutline), BoolText(Overlays.bGrid),
            BoolText(Overlays.bVolumes), BoolText(Overlays.bLightRadius),
            BoolText(Overlays.bAudioRadius), BoolText(Overlays.bModeWidgets),
            BoolText(Overlays.bNavigation), BoolText(Overlays.bGame),
            BoolText(Capture.bHideEditorSpritesRequested),
            BoolText(Capture.bBillboardSpritesApplied), Capture.ForcedShowFlags.Num());
    }

    for (int32 Index = 1; Index < DiagnosticCaptureCount; ++Index)
    {
        const PinWrightRenderCapture::FViewportCaptureOutput& Before =
            Output.Captures[Index - 1];
        const PinWrightRenderCapture::FViewportCaptureOutput& After = Output.Captures[Index];
        const int64 ExpectedPixelCount = static_cast<int64>(Before.Width) * Before.Height;
        const bool bValidPair = Before.Width > 0 && Before.Height > 0
            && Before.Width == After.Width && Before.Height == After.Height
            && ExpectedPixelCount <= MAX_int32
            && Before.Pixels.Num() == ExpectedPixelCount
            && After.Pixels.Num() == ExpectedPixelCount;
        const FPairDiagnostics Diagnostics = bValidPair
            ? ScanPair(Before.Pixels, After.Pixels, Before.Width, Before.Height)
            : FPairDiagnostics();
        UE_LOG(LogTemp, Display,
            TEXT("PoseListStability pair=%d-%d maxDelta=%d at=(%d,%d) channel=%c ")
            TEXT("changedFraction=%.9f bbox=(%d,%d)-(%d,%d)"),
            Index - 1, Index, Diagnostics.MaxDelta, Diagnostics.MaxX, Diagnostics.MaxY,
            Diagnostics.MaxChannel, Diagnostics.ChangedFraction,
            Diagnostics.ChangedMinX, Diagnostics.ChangedMinY,
            Diagnostics.ChangedMaxX, Diagnostics.ChangedMaxY);
        TestTrue(FString::Printf(TEXT("diagnostic pair %d-%d has compatible retained buffers"),
            Index - 1, Index), bValidPair);
    }

    if (!TestTrue(TEXT("the production eight-pose capture succeeds"), bCaptureSucceeded))
    {
        AddError(FString::Printf(TEXT("Eight-pose capture failed (%s: %s)."),
            *ErrorCode, *ErrorMessage));
        return false;
    }
    if (!TestEqual(TEXT("all eight requested poses are captured"), Output.Captures.Num(), 8))
    {
        return false;
    }

    UE_LOG(LogTemp, Display,
        TEXT("PoseListStability assertionRoi=(35,35)-(59,64) size=25x30"));
    for (int32 Index = 0; Index < Output.Captures.Num(); ++Index)
    {
        const PinWrightRenderCapture::FViewportCaptureOutput& Capture = Output.Captures[Index];
        TestTrue(FString::Printf(TEXT("frame %d reports native-resolution pinning"), Index),
            Capture.bRenderResolutionPinned);
        TestTrue(FString::Printf(TEXT("frame %d reports TemporalAA suppression"), Index),
            Capture.bTemporalAntiAliasingSuppressed);
        TestTrue(FString::Printf(TEXT("frame %d reports fixed exposure"), Index),
            Capture.bExposurePinned);
        TestFalse(FString::Printf(TEXT("frame %d contains lit preview content"), Index),
            Capture.ImageStats.bBlank);
        TestEqual(FString::Printf(TEXT("frame %d retains every pixel"), Index),
            Capture.Pixels.Num(), Request.Width * Request.Height);
    }

    // The empirical allowance covers sparse frame-indexed 8-bit/quantization-like variation:
    // one observed pixel reached 3, plus 1 LSB margin, still far below the 14-22 LSB cycle.
    constexpr int32 SubjectRoiMaxDelta = 4;
    for (int32 Index = 1; Index < Output.Captures.Num(); ++Index)
    {
        const PinWrightRenderCapture::FViewportCaptureOutput& Before =
            Output.Captures[Index - 1];
        const PinWrightRenderCapture::FViewportCaptureOutput& After = Output.Captures[Index];
        const bool bValidDimensions = Before.Width == 96 && Before.Height == 96
            && After.Width == 96 && After.Height == 96
            && Before.Pixels.Num() == 96 * 96 && After.Pixels.Num() == 96 * 96;
        if (!TestTrue(FString::Printf(
                TEXT("adjacent pair %d-%d has the expected 96x96 retained frames"),
                Index - 1, Index), bValidDimensions))
        {
            continue;
        }

        TArray<FColor> BeforeSubjectPixels;
        TArray<FColor> AfterSubjectPixels;
        BeforeSubjectPixels.Reserve(25 * 30);
        AfterSubjectPixels.Reserve(25 * 30);
        for (int32 Y = 35; Y < 65; ++Y)
        {
            for (int32 X = 35; X < 60; ++X)
            {
                const int32 PixelIndex = Y * 96 + X;
                BeforeSubjectPixels.Add(Before.Pixels[PixelIndex]);
                AfterSubjectPixels.Add(After.Pixels[PixelIndex]);
            }
        }

        // Full-frame diagnostics retain sparse non-subject background-edge changes; this
        // assertion targets the stationary subject region.
        const PinWrightFlatRegion::FFrameDifferenceStats Difference =
            PinWrightFlatRegion::MeasureFrameDifference(
                BeforeSubjectPixels, AfterSubjectPixels, SubjectRoiMaxDelta);
        TestTrue(FString::Printf(TEXT("adjacent pair %d-%d is measured"), Index - 1, Index),
            Difference.bMeasured);
        TestTrue(FString::Printf(
                TEXT("adjacent pair %d-%d max channel delta is <= %d (was %d)"),
                Index - 1, Index, SubjectRoiMaxDelta, Difference.MaxDelta),
            Difference.MaxDelta <= SubjectRoiMaxDelta);
    }
    return true;
}
