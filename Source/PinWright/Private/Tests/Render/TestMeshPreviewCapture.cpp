// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Misc/AutomationTest.h"

#include "Dom/JsonObject.h"
#include "DynamicRHI.h"
#include "Engine/StaticMesh.h"
#include "HAL/FileManager.h"
#include "Handlers/Render/MeshPreviewCaptureUtils.h"
#include "Misc/App.h"
#include "Tests/TestUtils.h"
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
        // B-capture-mesh-cold-first-frame-no-readiness-wait: the session waits on the mesh's slot
        // materials, not only its build, and the first shot is settled by measurement.
        Test.TestTrue(TEXT("readiness waited on the mesh build"),
            Output.Readiness.bWaitedForAssetCompilation);
        Test.TestTrue(TEXT("readiness asked the cube's slot material for its shader map"),
            Output.Readiness.bShaderMapChecked);
        Test.TestTrue(TEXT("the slot material's shader map is complete after the wait"),
            Output.Readiness.bShaderMapCompleteAfter);
        Test.TestTrue(TEXT("the first shot was compared against an identical redraw"),
            Output.Settle.bMeasured);
        Test.TestTrue(TEXT("a warm cube draws the same frame twice"), Output.Settle.bSettled);
        Test.TestTrue(TEXT("redrawRetries is a bounded count"),
            Output.Capture.RedrawRetries >= 0
                && Output.Capture.RedrawRetries
                    <= PinWrightMeshPreviewCapture::MaxSettleRedrawRetries);
        return true;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMeshPreviewCaptureCubePixelsTest,
    "PinWright.render.capture_mesh.TransientCubeHasNonFlatPixels",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMeshPreviewCaptureCubePixelsTest::RunTest(const FString& Parameters)
{
    if (PinWrightTestSkip::SkipIfRenderingUnavailable(*this)) { return true; }
    return RunCubeCaptureAssertions(*this, 1.0f);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMeshPreviewCaptureFiveCentimeterCubePixelsTest,
    "PinWright.render.capture_mesh.TransientFiveCentimeterCubeHasNonFlatPixels",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMeshPreviewCaptureFiveCentimeterCubePixelsTest::RunTest(const FString& Parameters)
{
    if (PinWrightTestSkip::SkipIfRenderingUnavailable(*this)) { return true; }
    return RunCubeCaptureAssertions(*this, 0.05f);
}

namespace TestMeshPreviewCaptureSettle
{
    // Feeds SettleFrame a scripted sequence of redraws: each entry is one frame filled with that
    // grey level.
    struct FScriptedRedraw
    {
        TArray<uint8> Levels;
        int32 Calls = 0;

        bool Draw(TArray<FColor>& OutPixels)
        {
            if (!Levels.IsValidIndex(Calls))
            {
                return false;
            }
            const uint8 Level = Levels[Calls++];
            OutPixels.Init(FColor(Level, Level, Level, 255), 16);
            return true;
        }
    };

    TArray<FColor> MakeFrame(uint8 Level)
    {
        TArray<FColor> Pixels;
        Pixels.Init(FColor(Level, Level, Level, 255), 16);
        return Pixels;
    }
}

// Pure logic: no GPU. Counterfactual: make SettleFrame return after one draw without comparing,
// or count the comparison draw as a retry, and the cold and stable cases below go red.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMeshPreviewCaptureSettleFrameTest,
    "PinWright.render.capture_mesh.SettleFrameCountsRedrawRetries",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMeshPreviewCaptureSettleFrameTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightMeshPreviewCapture;
    using TestMeshPreviewCaptureSettle::FScriptedRedraw;
    using TestMeshPreviewCaptureSettle::MakeFrame;

    {
        FScriptedRedraw Redraw{ { 100 } };
        TArray<FColor> Pixels = MakeFrame(100);
        int32 Retries = -1;
        FFrameSettleReport Report;
        TestTrue(TEXT("stable: settles"), SettleFrame([&](TArray<FColor>& Out)
            { return Redraw.Draw(Out); }, Pixels, MaxSettleRedrawRetries, Retries, Report));
        TestTrue(TEXT("stable: measured"), Report.bMeasured);
        TestTrue(TEXT("stable: settled"), Report.bSettled);
        TestEqual(TEXT("stable: the comparison draw is not a retry"), Retries, 0);
        TestEqual(TEXT("stable: one comparison draw"), Redraw.Calls, 1);
    }
    {
        // Cold first frame (black), then the finished picture twice.
        FScriptedRedraw Redraw{ { 200, 200 } };
        TArray<FColor> Pixels = MakeFrame(0);
        int32 Retries = -1;
        FFrameSettleReport Report;
        TestTrue(TEXT("cold: settles"), SettleFrame([&](TArray<FColor>& Out)
            { return Redraw.Draw(Out); }, Pixels, MaxSettleRedrawRetries, Retries, Report));
        TestTrue(TEXT("cold: settled on the retry"), Report.bSettled);
        TestEqual(TEXT("cold: one redraw retry"), Retries, 1);
        TestEqual(TEXT("cold: the returned frame is the settled one"), Pixels[0].R, (uint8)200);
    }
    {
        // Never agrees: bounded, and reported unsettled rather than looping.
        FScriptedRedraw Redraw{ { 50, 150, 250 } };
        TArray<FColor> Pixels = MakeFrame(0);
        int32 Retries = -1;
        FFrameSettleReport Report;
        TestTrue(TEXT("unsettled: returns"), SettleFrame([&](TArray<FColor>& Out)
            { return Redraw.Draw(Out); }, Pixels, MaxSettleRedrawRetries, Retries, Report));
        TestFalse(TEXT("unsettled: reported"), Report.bSettled);
        TestEqual(TEXT("unsettled: retries capped"), Retries, MaxSettleRedrawRetries);
        TestEqual(TEXT("unsettled: draws capped"), Redraw.Calls, MaxSettleRedrawRetries + 1);
        TestTrue(TEXT("unsettled: whole frame changed"), Report.ChangedPixelFraction > 0.99);

        FMeshCaptureOutput Output;
        Output.Settle = Report;
        Output.Capture.RedrawRetries = Retries;
        TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
        AddMeshFrameEvidenceFields(Output, Result);
        TestTrue(TEXT("unsettled: readiness published"), Result->HasField(TEXT("readiness")));
        TestFalse(TEXT("unsettled: frameSettled false"), Result->GetBoolField(TEXT("frameSettled")));
        TestTrue(TEXT("unsettled: frameWarning present"), Result->HasField(TEXT("frameWarning")));
    }
    {
        FScriptedRedraw Redraw;
        TArray<FColor> Pixels = MakeFrame(0);
        int32 Retries = -1;
        FFrameSettleReport Report;
        TestFalse(TEXT("a failed redraw fails the settle"), SettleFrame([&](TArray<FColor>& Out)
            { return Redraw.Draw(Out); }, Pixels, MaxSettleRedrawRetries, Retries, Report));
    }
    {
        FMeshCaptureOutput Output;
        TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
        AddMeshFrameEvidenceFields(Output, Result);
        TestFalse(TEXT("an unmeasured shot publishes no frameSettled"),
            Result->HasField(TEXT("frameSettled")));
        TestFalse(TEXT("an unmeasured shot publishes no frameWarning"),
            Result->HasField(TEXT("frameWarning")));
    }
    return true;
}

// The readiness helper used to ask only a MATERIAL subject for its shader map; a mesh subject got
// its own build waited on and nothing else. Counterfactual: drop the mesh branches from
// WaitForThumbnailSubjectReadiness and shaderMapChecked goes false here.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMeshPreviewCaptureReadinessMaterialsTest,
    "PinWright.render.capture_mesh.ReadinessWaitsOnMeshMaterials",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMeshPreviewCaptureReadinessMaterialsTest::RunTest(const FString& Parameters)
{
    UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
    if (!TestNotNull(TEXT("the engine cube loads"), Cube))
    {
        return true;
    }
    if (!TestTrue(TEXT("precondition: the cube has a slot material"),
        Cube->GetStaticMaterials().Num() > 0
            && Cube->GetStaticMaterials()[0].MaterialInterface != nullptr))
    {
        return true;
    }
    PinWrightThumbnail::FThumbnailReadinessReport Report;
    PinWrightThumbnail::WaitForThumbnailSubjectReadiness(Cube, Report);
    TestTrue(TEXT("the mesh build was waited on"), Report.bWaitedForAssetCompilation);
    TestTrue(TEXT("the slot material's shader map was checked"), Report.bShaderMapChecked);
    TestTrue(TEXT("the slot material's shader map is complete after the wait"),
        Report.bShaderMapCompleteAfter);
    return true;
}

// The wire half: RenderHandler's one AddMeshFrameEvidenceFields call is the only thing that puts
// `readiness` / `frameSettled` on the response. Counterfactual: delete that call and the readiness
// and frameSettled asserts go red; settle on every shot instead of the first and shots[1] goes red.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMeshPreviewCaptureRpcFrameEvidenceTest,
    "PinWright.render.capture_mesh.RpcPublishesReadinessAndSettle",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMeshPreviewCaptureRpcFrameEvidenceTest::RunTest(const FString& Parameters)
{
    if (PinWrightTestSkip::SkipIfRenderingUnavailable(*this)) { return true; }

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), TEXT("/Engine/BasicShapes/Cube.Cube"));
    Payload->SetNumberField(TEXT("width"), 128);
    Payload->SetNumberField(TEXT("height"), 128);
    Payload->SetNumberField(TEXT("count"), 2);
    Payload->SetBoolField(TEXT("measureCoverage"), false);
    Payload->SetNumberField(TEXT("exposure"), 0);

    FTestResponseCapture Capture;
    if (!TestTrue(TEXT("render.capture_mesh handler found"),
            InvokeHandlerWithCapture(TEXT("render.capture_mesh"), Payload, Capture))
        || !TestTrue(*FString::Printf(TEXT("capture succeeds: %s %s"),
            *Capture.ErrorCode, *Capture.Message), Capture.bSuccess && Capture.Result.IsValid()))
    {
        return true;
    }
    const TSharedPtr<FJsonObject>& Result = Capture.Result;

    const TArray<TSharedPtr<FJsonValue>>* Shots = nullptr;
    if (Result->TryGetArrayField(TEXT("shots"), Shots))
    {
        for (const TSharedPtr<FJsonValue>& Shot : *Shots)
        {
            FString Path;
            if (Shot->AsObject().IsValid() && Shot->AsObject()->TryGetStringField(TEXT("path"), Path))
            {
                IFileManager::Get().Delete(*Path, false, false, true);
            }
        }
    }

    const TSharedPtr<FJsonObject>* Readiness = nullptr;
    TestTrue(TEXT("readiness is published"), Result->TryGetObjectField(TEXT("readiness"), Readiness));
    TestTrue(TEXT("readiness.shaderMapChecked"),
        Readiness && (*Readiness)->GetBoolField(TEXT("shaderMapChecked")));
    TestTrue(TEXT("frameSettled is published"), Result->HasField(TEXT("frameSettled")));

    if (!TestTrue(TEXT("two shots"), Shots && Shots->Num() == 2))
    {
        return true;
    }
    const int32 FirstRetries = (*Shots)[0]->AsObject()->GetIntegerField(TEXT("redrawRetries"));
    TestTrue(TEXT("shots[0].redrawRetries is 0 or 1"), FirstRetries == 0 || FirstRetries == 1);
    TestEqual(TEXT("shots[1].redrawRetries is 0: settle runs once per session"),
        (*Shots)[1]->AsObject()->GetIntegerField(TEXT("redrawRetries")), 0);
    return true;
}
