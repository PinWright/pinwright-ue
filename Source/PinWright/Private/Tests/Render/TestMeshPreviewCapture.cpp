// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Misc/AutomationTest.h"

#include "DynamicRHI.h"
#include "Handlers/Render/MeshPreviewCaptureUtils.h"
#include "Misc/App.h"
#include "Tests/TestSkipReporting.h"

namespace
{
    PinWrightMeshPreviewCapture::FMeshCaptureRequest MakeCubeRequest(float SubjectScale)
    {
        PinWrightMeshPreviewCapture::FMeshCaptureRequest Request;
        Request.AssetPath = TEXT("/Engine/BasicShapes/Cube.Cube");
        Request.Capture.Width = 128;
        Request.Capture.Height = 128;
        Request.Capture.Fov = 50.0f;
        Request.Capture.Exposure.Mode = PinWrightRenderCapture::EExposureRequestMode::Fixed;
        Request.Capture.Exposure.Ev100 = 0.0f;
        Request.Capture.PreviewSceneRig.bRequested = true;
        Request.Capture.PreviewSceneRig.bShowFloorProvided = true;
        Request.Capture.PreviewSceneRig.bShowFloor = false;
        Request.Capture.PreviewSceneRig.bShowEnvironmentProvided = true;
        Request.Capture.PreviewSceneRig.bShowEnvironment = false;
        Request.Capture.bRetainPixels = true;
        Request.SubjectScale = SubjectScale;
        Request.bWriteFile = false;
        return Request;
    }

    bool RunCubeCaptureAssertions(FAutomationTestBase& Test, float SubjectScale)
    {
        if (!FApp::CanEverRender() || !GDynamicRHI)
        {
            PinWrightTestSkip::SkipAssertions(Test, TEXT("no-gpu"),
                TEXT("This host has no rendering device, so scene-capture readback cannot run."));
            return true;
        }

        const PinWrightMeshPreviewCapture::FMeshCaptureRequest Request =
            MakeCubeRequest(SubjectScale);
        PinWrightMeshPreviewCapture::FMeshCaptureOutput Output;
        FString ErrorCode;
        FString ErrorMessage;
        const bool bCaptured = PinWrightMeshPreviewCapture::CaptureMeshToPng(
            Request, Output, ErrorCode, ErrorMessage);
        if (!Test.TestTrue(*FString::Printf(TEXT("capture succeeds: %s %s"),
                *ErrorCode, *ErrorMessage), bCaptured))
        {
            return false;
        }

        Test.TestEqual(TEXT("captured width"), Output.Capture.Width, 128);
        Test.TestEqual(TEXT("captured height"), Output.Capture.Height, 128);
        Test.TestEqual(TEXT("readback pixel count"), Output.Capture.Pixels.Num(), 128 * 128);
        Test.TestTrue(TEXT("PNG was encoded without a persistent file"), Output.PngData.Num() > 8);
        Test.TestTrue(TEXT("flat-region analysis measured the rendered pixels"),
            Output.FlatRegion.bMeasured);
        Test.TestTrue(TEXT("the frame contains non-flat pixels"),
            Output.FlatRegion.FlatBlockFraction < 1.0
                && Output.FlatRegion.LargestRegionFraction < 1.0);
        Test.TestTrue(TEXT("the cube changes luminance across the frame"),
            Output.Capture.ImageStats.LuminanceVariance > 0.0);
        Test.TestTrue(TEXT("the framed cube occupies a non-empty subset of the image"),
            Output.SubjectCoverage.IsSet()
                && Output.SubjectCoverage.GetValue() > 0.0
                && Output.SubjectCoverage.GetValue() < 1.0);
        return true;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMeshPreviewCaptureCubePixelsTest,
    "PinWright.render.capture_mesh.TransientCubeHasNonFlatPixels",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMeshPreviewCaptureCubePixelsTest::RunTest(const FString& Parameters)
{
    return RunCubeCaptureAssertions(*this, 1.0f);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMeshPreviewCaptureFiveCentimeterCubePixelsTest,
    "PinWright.render.capture_mesh.TransientFiveCentimeterCubeHasNonFlatPixels",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMeshPreviewCaptureFiveCentimeterCubePixelsTest::RunTest(const FString& Parameters)
{
    return RunCubeCaptureAssertions(*this, 0.05f);
}
