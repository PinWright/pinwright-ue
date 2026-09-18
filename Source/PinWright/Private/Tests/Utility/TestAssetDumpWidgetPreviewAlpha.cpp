// Copyright (c) 2026 Alexander Penkin. MIT License.

// asset.dump's widget aspect publishes what the opaque stamp destroyed.
//
// preview.png goes through the same WidgetDesignerCaptureUtil::CapturePreviewToPng that
// widget.screenshot_designer's preview branch uses, so it is stamped opaque before encoding and
// the widget's real transparency is gone from the file. asset.dump's `widgetPreview` block is
// the only surviving record.
//
// Tests/Utility/TestAssetDumpWidgetScreenshot.cpp already pins that the PNG is produced,
// skipped, and diagnosed. What is untested without this file is whether the alpha facts reach
// the caller, whether they are MEASURED rather than typed in, and whether the block stays away
// from every dump that captured no preview.
//
// Counterfactuals:
//   * Replace the two SetField calls in the `widgetPreview` block with literals and
//     ResponsePublishesMeasuredFacts fails: the fraction stops matching what an independent
//     CapturePreviewToPng measures on the same widget at the same MaxSize, and stops landing in
//     the band a half-covered canvas produces.
//   * Drop the `&PreviewAlpha` out-param from either BuildAllFilesForAsset call site and the
//     facts never reach FDumpSingleResult: the fraction reads 0.0 and bWidgetPreviewCaptured
//     stays false -- DumpResultCarriesTheFactsAcrossTheApiBoundary and
//     ResponsePublishesMeasuredFacts both fail.
//   * Emit the block unconditionally instead of gating on bWidgetPreviewCaptured and
//     BlockIsAbsentWhenTheAspectDidNotRun fails, because a dump that captured nothing would be
//     publishing opaqueStamped:false / a 0.0 fraction as if they were findings.

#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Tests/TestUtils.h"

#include "Tests/Widget/WidgetPreviewAlphaFixtures.h"
#include "Tests/Widget/WidgetTestFixtures.h"
#include "Handlers/Asset/AssetDumpHandler.h"
#include "Handlers/UI/WidgetDesignerCaptureUtil.h"

#include "HAL/FileManager.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Math/UnrealMathUtility.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "WidgetBlueprint.h"

namespace
{
    FString PWAdwpaMakeScratchRoot()
    {
        return FPaths::ConvertRelativePathToFull(FPaths::ProjectIntermediateDir())
            / TEXT("AssetDumpWidgetPreviewAlpha")
            / FGuid::NewGuid().ToString(EGuidFormats::Digits);
    }

    // asset.dump collapses a bare package path to its primary object, so the package path is
    // what the RPC takes; DumpSingleAsset is called with the explicit object path the way
    // TestAssetDumpWidgetScreenshot.cpp does.
    FString PWAdwpaMakeObjectPath(const FString& PackagePath)
    {
        return FString::Printf(TEXT("%s.%s"),
            *PackagePath, *FPackageName::GetLongPackageAssetName(PackagePath));
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDumpWidgetPreviewAlphaResponsePublishesMeasuredFactsTest,
    "PinWright.asset.dump.WidgetPreviewAlpha.ResponsePublishesMeasuredFacts",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FAssetDumpWidgetPreviewAlphaResponsePublishesMeasuredFactsTest::RunTest(const FString& Parameters)
{
    using namespace WidgetPreviewAlphaFixtures;

    const FString AssetPath = WidgetTestFixtures::MakeWidgetDesignerScreenshotAssetPath(
        TEXT("WBP_DumpAlphaFacts"));
    UWidgetBlueprint* WBP = WidgetTestFixtures::MakeWidgetDesignerScreenshotBlueprint(AssetPath);
    if (!TestNotNull(TEXT("widget blueprint allocated"), WBP))
    {
        return false;
    }

    const FString ScratchRoot = PWAdwpaMakeScratchRoot();
    ON_SCOPE_EXIT
    {
        IFileManager::Get().DeleteDirectory(*ScratchRoot, /*RequireExists=*/false, /*Tree=*/true);
        CloseAndCleanupWidget(WBP, AssetPath);
    };

    if (!TestNotNull(TEXT("opaque top-half fill added"), AddTopHalfOpaqueFill(WBP)))
    {
        return false;
    }
    FKismetEditorUtilities::CompileBlueprint(WBP);
    PinDesignerPreviewSize(WBP, FVector2D(PinnedDesignWidth, PinnedDesignHeight));

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), AssetPath);
    Payload->SetStringField(TEXT("outRoot"), ScratchRoot);
    Payload->SetBoolField(TEXT("includeWidgetScreenshot"), true);

    FTestResponseCapture Capture;
    TestTrue(TEXT("asset.dump handler found"),
        InvokeHandlerWithCapture(TEXT("asset.dump"), Payload, Capture));
    TestTrue(FString::Printf(TEXT("dump succeeds (error='%s')"), *Capture.ErrorCode),
        Capture.bSuccess);
    if (!Capture.bSuccess || !Capture.Result.IsValid())
    {
        return false;
    }

    const TSharedPtr<FJsonObject>* WidgetPreview = nullptr;
    TestTrue(TEXT("response carries a widgetPreview block"),
        Capture.Result->TryGetObjectField(TEXT("widgetPreview"), WidgetPreview)
            && WidgetPreview != nullptr && WidgetPreview->IsValid());
    if (!WidgetPreview || !WidgetPreview->IsValid())
    {
        return false;
    }

    TestEqual(TEXT("widgetPreview names the aspect file it is about"),
        (*WidgetPreview)->GetStringField(TEXT("file")), FString(DumpFileNames::WidgetPreviewPng));

    // HasField, not a getter: TryGetBoolField on an absent field yields false, which cannot be
    // told apart from a field present and false.
    TestTrue(TEXT("widgetPreview carries opaqueStamped"),
        (*WidgetPreview)->HasField(TEXT("opaqueStamped")));
    TestTrue(TEXT("widgetPreview carries alphaZeroFraction"),
        (*WidgetPreview)->HasField(TEXT("alphaZeroFraction")));
    if (!(*WidgetPreview)->HasField(TEXT("opaqueStamped"))
        || !(*WidgetPreview)->HasField(TEXT("alphaZeroFraction")))
    {
        return false;
    }

    bool bReportedStamped = false;
    (*WidgetPreview)->TryGetBoolField(TEXT("opaqueStamped"), bReportedStamped);
    TestTrue(TEXT("the dumped preview reports itself stamped opaque"), bReportedStamped);

    double ReportedFraction = -1.0;
    TestTrue(TEXT("alphaZeroFraction is a number"),
        (*WidgetPreview)->TryGetNumberField(TEXT("alphaZeroFraction"), ReportedFraction));

    // A band, not a bare `> 0`. The fixture leaves exactly half the canvas at the render
    // target's transparent clear, so an honest measurement lands near 0.5; a hardcoded 0.0 or
    // 1.0 both fall outside, and on a uniformly transparent fixture neither could be told from
    // a measurement.
    TestTrue(
        FString::Printf(
            TEXT("alphaZeroFraction is measured over a half-covered canvas (got %.6f, expected ")
            TEXT("%.2f..%.2f). A reading near 1.0 means the opaque fill did not render -- that ")
            TEXT("is a fixture defect, not a handler defect."),
            ReportedFraction, HalfCoveredFractionMin, HalfCoveredFractionMax),
        IsHalfCoveredFraction(ReportedFraction));

    // Independent cross-check at the MaxSize the widget aspect hardcodes, so the control
    // rasterises the same preview at the same pixel count and counts the same pre-stamp alpha.
    // A literal in the response would have to coincide with a freshly measured double to 1e-3.
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
    TestEqual(
        FString::Printf(
            TEXT("widgetPreview.alphaZeroFraction equals an independently measured one ")
            TEXT("(response=%.6f, control=%.6f)"),
            ReportedFraction, ControlInfo.AlphaZeroFraction),
        ReportedFraction, ControlInfo.AlphaZeroFraction, 1e-3);

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDumpWidgetPreviewAlphaBlockIsAbsentTest,
    "PinWright.asset.dump.WidgetPreviewAlpha.BlockIsAbsentWhenTheAspectDidNotRun",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FAssetDumpWidgetPreviewAlphaBlockIsAbsentTest::RunTest(const FString& Parameters)
{
    using namespace WidgetPreviewAlphaFixtures;

    const FString AssetPath = WidgetTestFixtures::MakeWidgetDesignerScreenshotAssetPath(
        TEXT("WBP_DumpAlphaFactsAbsent"));
    UWidgetBlueprint* WBP = WidgetTestFixtures::MakeWidgetDesignerScreenshotBlueprint(AssetPath);
    if (!TestNotNull(TEXT("widget blueprint allocated"), WBP))
    {
        return false;
    }

    const FString ScratchRoot = PWAdwpaMakeScratchRoot();
    ON_SCOPE_EXIT
    {
        IFileManager::Get().DeleteDirectory(*ScratchRoot, /*RequireExists=*/false, /*Tree=*/true);
        CloseAndCleanupWidget(WBP, AssetPath);
    };

    if (!TestNotNull(TEXT("preview label added"), WidgetTestFixtures::AddSizedPreviewLabel(WBP)))
    {
        return false;
    }
    FKismetEditorUtilities::CompileBlueprint(WBP);

    // The SAME widget the positive test captures, dumped with the aspect off. The block must
    // key off "a preview was captured", not off "this asset is a widget" -- otherwise a caller
    // reading opaqueStamped:false would conclude the dumper had stopped stamping.
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), AssetPath);
    Payload->SetStringField(TEXT("outRoot"), ScratchRoot);
    Payload->SetBoolField(TEXT("includeWidgetScreenshot"), false);

    FTestResponseCapture Capture;
    TestTrue(TEXT("asset.dump handler found"),
        InvokeHandlerWithCapture(TEXT("asset.dump"), Payload, Capture));
    TestTrue(FString::Printf(TEXT("dump succeeds (error='%s')"), *Capture.ErrorCode),
        Capture.bSuccess);
    if (!Capture.bSuccess || !Capture.Result.IsValid())
    {
        return false;
    }

    // Confirms the dump really ran, so the absence below is "the aspect did not run" rather
    // than "nothing ran at all".
    const TArray<TSharedPtr<FJsonValue>>* WrittenPaths = nullptr;
    TestTrue(TEXT("the dump wrote files"),
        Capture.Result->TryGetArrayField(TEXT("writtenPaths"), WrittenPaths)
            && WrittenPaths != nullptr && WrittenPaths->Num() > 0);

    TestFalse(TEXT("no widgetPreview block when the screenshot aspect is off"),
        Capture.Result->HasField(TEXT("widgetPreview")));

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDumpWidgetPreviewAlphaApiBoundaryTest,
    "PinWright.asset.dump.WidgetPreviewAlpha.DumpResultCarriesTheFactsAcrossTheApiBoundary",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FAssetDumpWidgetPreviewAlphaApiBoundaryTest::RunTest(const FString& Parameters)
{
    using namespace WidgetPreviewAlphaFixtures;

    const FString AssetPath = WidgetTestFixtures::MakeWidgetDesignerScreenshotAssetPath(
        TEXT("WBP_DumpAlphaBoundary"));
    UWidgetBlueprint* WBP = WidgetTestFixtures::MakeWidgetDesignerScreenshotBlueprint(AssetPath);
    if (!TestNotNull(TEXT("widget blueprint allocated"), WBP))
    {
        return false;
    }

    const FString ScratchRoot = PWAdwpaMakeScratchRoot();
    ON_SCOPE_EXIT
    {
        IFileManager::Get().DeleteDirectory(*ScratchRoot, /*RequireExists=*/false, /*Tree=*/true);
        CloseAndCleanupWidget(WBP, AssetPath);
    };

    if (!TestNotNull(TEXT("opaque top-half fill added"), AddTopHalfOpaqueFill(WBP)))
    {
        return false;
    }
    FKismetEditorUtilities::CompileBlueprint(WBP);
    PinDesignerPreviewSize(WBP, FVector2D(PinnedDesignWidth, PinnedDesignHeight));

    const FString ObjectPath = PWAdwpaMakeObjectPath(AssetPath);

    // FDumpSingleResult is PINWRIGHT_API and is the boundary the async folder sweep crosses
    // too, so the facts have to live there rather than only inside the synchronous handler.
    AssetDumpHandler::FDumpSingleResult WithAspect =
        AssetDumpHandler::DumpSingleAsset(ObjectPath, ScratchRoot, /*bDiff=*/false,
            /*bIncludeWidgetScreenshot=*/true);
    TestTrue(FString::Printf(TEXT("dump with the aspect succeeds (error='%s')"),
            *WithAspect.ErrorCode),
        WithAspect.ErrorCode.IsEmpty());
    TestTrue(TEXT("the aspect reports itself captured"), WithAspect.bWidgetPreviewCaptured);
    TestTrue(TEXT("the captured preview reports itself stamped"),
        WithAspect.bWidgetPreviewOpaqueStamped);
    TestTrue(
        FString::Printf(
            TEXT("WidgetPreviewAlphaZeroFraction is measured over a half-covered canvas ")
            TEXT("(got %.6f, expected %.2f..%.2f)"),
            WithAspect.WidgetPreviewAlphaZeroFraction,
            HalfCoveredFractionMin, HalfCoveredFractionMax),
        IsHalfCoveredFraction(WithAspect.WidgetPreviewAlphaZeroFraction));

    AssetDumpHandler::FDumpSingleResult WithoutAspect =
        AssetDumpHandler::DumpSingleAsset(ObjectPath, ScratchRoot, /*bDiff=*/false,
            /*bIncludeWidgetScreenshot=*/false);
    TestTrue(FString::Printf(TEXT("dump without the aspect succeeds (error='%s')"),
            *WithoutAspect.ErrorCode),
        WithoutAspect.ErrorCode.IsEmpty());
    // The gate, not the number: a 0.0 fraction is the struct's default and says nothing on its
    // own. bWidgetPreviewCaptured is what tells a reader the difference between "measured, and
    // none of it was transparent" and "never measured".
    TestFalse(TEXT("no capture is reported when the aspect is off"),
        WithoutAspect.bWidgetPreviewCaptured);

    return true;
}
