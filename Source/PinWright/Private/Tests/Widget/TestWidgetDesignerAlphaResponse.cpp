// Copyright (c) 2026 Alexander Penkin. MIT License.

// widget.screenshot_designer publishes what the opaque stamp destroyed.
//
// The Designer preview capture is stamped opaque before encoding
// (WidgetDesignerCaptureUtil::StampOpaqueAndEncodePng). A widget's alpha 0 is real content --
// FWidgetRenderer draws onto a target cleared to FLinearColor::Transparent -- so the stamp
// destroys information that exists nowhere in the PNG at `path` afterwards. The response is the
// only place it survives, as `opaqueStamped` + `alphaZeroFraction`.
//
// These tests assert the RESPONSE, not the util. The util's own measurement is pinned by
// Tests/Widget/TestWidgetDesignerCaptureAlpha.cpp; what is untested without this file is
// whether the handler carries that measurement out to the caller at all, and whether it carries
// a MEASUREMENT rather than a literal somebody typed in.
//
// Counterfactuals:
//   * Replace the two SetField calls in WidgetDesignerScreenshotHandler.cpp's response with
//     hardcoded literals and PreviewResponsePublishesMeasuredAlphaFacts fails twice over: the
//     reported fraction stops matching the value an independent CapturePreviewToPng call
//     measures on the same widget at the same max_size, and it stops landing in the band a
//     half-covered canvas produces.
//   * Forget to copy Info.AlphaZeroFraction into the response -- the realistic slip, where the
//     local stays default-initialized -- and the fraction reads 0.0, outside the band.
//   * Delete the `if (bAlphaFactsMeasured)` gate so the fields publish on every branch, and
//     WindowResponseOmitsAlphaFacts fails: that branch stamps but never counts what it
//     overwrote, so any number it published would be one nobody measured.

#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Tests/TestUtils.h"

#include "Tests/Widget/WidgetPreviewAlphaFixtures.h"
#include "Tests/Widget/WidgetTestFixtures.h"
#include "Handlers/UI/WidgetDesignerCaptureUtil.h"

#include "HAL/FileManager.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Math/UnrealMathUtility.h"
#include "Misc/ScopeExit.h"
#include "WidgetBlueprint.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWidgetDesignerPreviewResponsePublishesMeasuredAlphaFactsTest,
    "PinWright.widget.screenshot_designer.PreviewResponsePublishesMeasuredAlphaFacts",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWidgetDesignerPreviewResponsePublishesMeasuredAlphaFactsTest::RunTest(const FString& Parameters)
{
    using namespace WidgetPreviewAlphaFixtures;

    const FString WidgetPath = WidgetTestFixtures::MakeWidgetDesignerScreenshotAssetPath(
        TEXT("WBP_AlphaResponsePreview"));
    UWidgetBlueprint* WBP = WidgetTestFixtures::MakeWidgetDesignerScreenshotBlueprint(WidgetPath);
    if (!TestNotNull(TEXT("widget blueprint allocated"), WBP))
    {
        return false;
    }

    FString ScreenshotPath;
    ON_SCOPE_EXIT
    {
        if (!ScreenshotPath.IsEmpty())
        {
            IFileManager::Get().Delete(*ScreenshotPath);
        }
        CloseAndCleanupWidget(WBP, WidgetPath);
    };

    if (!TestNotNull(TEXT("opaque top-half fill added"), AddTopHalfOpaqueFill(WBP)))
    {
        return false;
    }
    FKismetEditorUtilities::CompileBlueprint(WBP);
    PinDesignerPreviewSize(WBP, FVector2D(PinnedDesignWidth, PinnedDesignHeight));

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("widgetPath"), WidgetPath);
    Payload->SetStringField(TEXT("target"), TEXT("preview"));
    Payload->SetNumberField(TEXT("max_size"), PinnedMaxSize);

    FTestResponseCapture Capture;
    TestTrue(TEXT("widget.screenshot_designer handler found"),
        InvokeHandlerWithCapture(TEXT("widget.screenshot_designer"), Payload, Capture));
    TestTrue(FString::Printf(TEXT("preview capture succeeds (error='%s')"), *Capture.ErrorCode),
        Capture.bSuccess);
    if (!Capture.bSuccess || !Capture.Result.IsValid())
    {
        return false;
    }
    Capture.Result->TryGetStringField(TEXT("path"), ScreenshotPath);

    // Presence checked with HasField rather than a getter: TryGetBoolField on an absent field
    // yields false, which is indistinguishable from a field present and false.
    TestTrue(TEXT("preview response carries opaqueStamped"),
        Capture.Result->HasField(TEXT("opaqueStamped")));
    TestTrue(TEXT("preview response carries alphaZeroFraction"),
        Capture.Result->HasField(TEXT("alphaZeroFraction")));
    if (!Capture.Result->HasField(TEXT("opaqueStamped"))
        || !Capture.Result->HasField(TEXT("alphaZeroFraction")))
    {
        return false;
    }

    bool bReportedStamped = false;
    TestTrue(TEXT("opaqueStamped is a boolean"),
        Capture.Result->TryGetBoolField(TEXT("opaqueStamped"), bReportedStamped));
    TestTrue(TEXT("the preview capture reports itself stamped opaque"), bReportedStamped);

    double ReportedFraction = -1.0;
    TestTrue(TEXT("alphaZeroFraction is a number"),
        Capture.Result->TryGetNumberField(TEXT("alphaZeroFraction"), ReportedFraction));

    // A band, not a bare `> 0`: the fixture leaves exactly half the canvas at the render
    // target's transparent clear, so an honest measurement lands near 0.5. A `> 0` assertion
    // would also pass on a hardcoded 1.0, and on a uniformly transparent fixture it could not
    // tell a measurement from a guess at all.
    TestTrue(
        FString::Printf(
            TEXT("alphaZeroFraction is measured over a half-covered canvas (got %.6f, expected ")
            TEXT("%.2f..%.2f). A reading near 1.0 means the opaque fill did not render -- that ")
            TEXT("is a fixture defect, not a handler defect."),
            ReportedFraction, HalfCoveredFractionMin, HalfCoveredFractionMax),
        IsHalfCoveredFraction(ReportedFraction));

    // Independent cross-check. CapturePreviewToPng is called directly at the same max_size, so
    // it rasterises the same preview at the same pixel count and counts the same pre-stamp
    // alpha. The response must carry THAT number. This is what a hardcoded literal cannot
    // survive: it would have to coincide with a freshly measured double to 1e-3.
    TArray<uint8> ControlPng;
    FString ControlError;
    WidgetDesignerCaptureUtil::FCaptureInfo ControlInfo;
    const bool bControlCaptured = WidgetDesignerCaptureUtil::CapturePreviewToPng(
        WBP, PinnedMaxSize, ControlPng, ControlError, &ControlInfo);
    TestTrue(FString::Printf(TEXT("control capture succeeds (error='%s')"), *ControlError),
        bControlCaptured);
    if (!bControlCaptured)
    {
        return false;
    }
    TestTrue(TEXT("control capture reports itself stamped"), ControlInfo.bOpaqueStamped);
    TestEqual(
        FString::Printf(
            TEXT("response alphaZeroFraction equals an independently measured one ")
            TEXT("(response=%.6f, control=%.6f)"),
            ReportedFraction, ControlInfo.AlphaZeroFraction),
        ReportedFraction, ControlInfo.AlphaZeroFraction, 1e-3);

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWidgetDesignerWindowResponseOmitsAlphaFactsTest,
    "PinWright.widget.screenshot_designer.WindowResponseOmitsAlphaFacts",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWidgetDesignerWindowResponseOmitsAlphaFactsTest::RunTest(const FString& Parameters)
{
    using namespace WidgetPreviewAlphaFixtures;

    const FString WidgetPath = WidgetTestFixtures::MakeWidgetDesignerScreenshotAssetPath(
        TEXT("WBP_AlphaResponseWindow"));
    UWidgetBlueprint* WBP = WidgetTestFixtures::MakeWidgetDesignerScreenshotBlueprint(WidgetPath);
    if (!TestNotNull(TEXT("widget blueprint allocated"), WBP))
    {
        return false;
    }

    FString ScreenshotPath;
    ON_SCOPE_EXIT
    {
        if (!ScreenshotPath.IsEmpty())
        {
            IFileManager::Get().Delete(*ScreenshotPath);
        }
        CloseAndCleanupWidget(WBP, WidgetPath);
    };

    if (!TestNotNull(TEXT("preview label added"), WidgetTestFixtures::AddSizedPreviewLabel(WBP)))
    {
        return false;
    }
    FKismetEditorUtilities::CompileBlueprint(WBP);

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("widgetPath"), WidgetPath);
    Payload->SetStringField(TEXT("target"), TEXT("window"));

    FTestResponseCapture Capture;
    TestTrue(TEXT("widget.screenshot_designer handler found"),
        InvokeHandlerWithCapture(TEXT("widget.screenshot_designer"), Payload, Capture));
    TestTrue(FString::Printf(TEXT("window capture succeeds (error='%s')"), *Capture.ErrorCode),
        Capture.bSuccess);
    if (!Capture.bSuccess || !Capture.Result.IsValid())
    {
        return false;
    }
    Capture.Result->TryGetStringField(TEXT("path"), ScreenshotPath);

    // Confirms this really is the window branch, so the two absences below are being asserted
    // about the path that takes no pre-stamp measurement.
    TestEqual(TEXT("capture source is the editor window"),
        Capture.Result->GetStringField(TEXT("captureSource")), FString(TEXT("editorWindow")));

    // BOTH fields, and by HasField. The window branch does stamp (ForceOpaqueAlpha on the Slate
    // back-buffer readback) but never counts the alpha it overwrote, so there is no fraction to
    // publish and no honest way to publish the flag without one. Checking only alphaZeroFraction
    // would pass a regression that emitted a bare opaqueStamped; checking with GetNumberField
    // would pass anything, because a missing number reads as 0.
    TestFalse(TEXT("window response does not carry opaqueStamped"),
        Capture.Result->HasField(TEXT("opaqueStamped")));
    TestFalse(TEXT("window response does not carry alphaZeroFraction"),
        Capture.Result->HasField(TEXT("alphaZeroFraction")));

    return true;
}
