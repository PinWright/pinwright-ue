// Copyright (c) 2026 Alexander Penkin. MIT License.

// render.capture_annotated: the `subject` block, the `framing` verdict, and the frame statistics
// the verb used to compute and discard.
//
// A NEW file rather than assertions appended to TestAnnotatedCaptureHandlers.cpp: that file is the
// regression floor for this verb (including its assert-ABSENT cases) and has to keep passing
// unmodified, so nothing here may edit it.
//
// Three things are pinned:
//
//   1. NO RAW ERROR-CODE LITERALS. This verb emitted eight codes as bare TEXT("...") strings while
//      every other handler references the ErrorCodes::ERR_* constants. The existing global walk
//      PinWright.core.error_codes.AllEmittedCodesAreRegistered CANNOT catch that, and it is worth
//      being precise about why: it collects the literal from every SendError first argument and
//      fails only on a code with no matching ERR_* constant in Handlers/ErrorCodes.h. All eight of
//      these codes WERE registered - INVALID_ARGUMENT, CLASS_NOT_FOUND, EDITOR_NOT_AVAILABLE,
//      NO_EDITOR_WORLD, NO_ACTIVE_LEVEL_VIEWPORT, DECODE_FAILED, ENCODE_FAILED, SAVE_FAILED - so
//      that walk was green over this file the whole time the defect existed. What was missing is a
//      test of the SPELLING at the call site, which is what the first test below is.
//
//   2. THE STATS ARE NO LONGER DISCARDED. CaptureEditorViewportToPng fills FCaptureImageStats for
//      every capture; this verb published the annotated PNG's byte count and nothing about its
//      pixels. The test recomputes the statistics from the PNG on disk with the SAME exported
//      classifier the handler's numbers came from and requires them to agree.
//
//   3. `subject` AND `framing` ARE PRESENT ONLY WHEN A SUBJECT WAS RESOLVED. Both directions are
//      asserted, because a block emitted empty on every call is the failure mode the response-shape
//      contract already carries thirteen assert-absent cases against.
//
// NOT PINNED HERE: where the overlay ink lands. Test 2 below asserts only that an asset subject is
// accepted; a verb that accepted it and then projected the axes through the wrong viewport passes
// every assertion in this file while painting measured-looking overlays on coordinates the frame
// never had. That is asserted against a decoded PNG in TestAnnotatedAssetOverlay.cpp, and the
// "which client did the session move" half in TestViewProjectionTargets.cpp.
//
// EVERY capture in this file is 256x256. A capture size that VARIES within an editor session trips
// `Assertion failed: ProxyMap.Num() == TestSizeX * TestSizeY` in FViewport::GetHitProxy; a constant
// size is the property that has been proven safe, so a new test never introduces a fresh one.
#include "Misc/AutomationTest.h"
#include "Handlers/Render/PreviewViewportCaptureUtils.h"
#include "Tests/TestUtils.h"
#include "Tests/TestWorldUtils.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "ImageCore.h"
#include "ImageUtils.h"
#include "Interfaces/IPluginManager.h"
#include "Internationalization/Regex.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Tests/TestSkipReporting.h"

namespace
{
    // The typed, non-crashing exits render.capture_annotated may return when this host has no live
    // viewport to capture. Deliberately a LOCAL copy of the same list TestAnnotatedCaptureHandlers
    // keeps: that file is the frozen regression floor and must not be edited to export a helper.
    bool AnnotatedSubjectsIsTypedCaptureFailure(const FString& ErrorCode)
    {
        return ErrorCode == TEXT("NO_ACTIVE_LEVEL_VIEWPORT") ||
            ErrorCode == TEXT("NO_EDITOR_WORLD") ||
            ErrorCode == TEXT("EDITOR_NOT_AVAILABLE") ||
            ErrorCode == TEXT("CAPTURE_FAILED") ||
            ErrorCode == TEXT("PREVIEW_VIEWPORT_NOT_FOUND") ||
            ErrorCode == TEXT("UNSUPPORTED_ORTHOGRAPHIC_ROTATION") ||
            ErrorCode == TEXT("DECODE_FAILED") ||
            ErrorCode == TEXT("ENCODE_FAILED") ||
            ErrorCode == TEXT("SAVE_FAILED");
    }

    void AnnotatedSubjectsDeleteFileIfPresent(const FString& Path)
    {
        if (!Path.IsEmpty())
        {
            IFileManager::Get().Delete(*Path, false, true);
        }
    }

    FString AnnotatedHandlerSourcePath()
    {
        const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("PinWright"));
        if (!Plugin.IsValid())
        {
            return FString();
        }
        return Plugin->GetBaseDir()
            / TEXT("Source") / TEXT("PinWright") / TEXT("Private")
            / TEXT("Handlers") / TEXT("Render") / TEXT("AnnotatedCaptureHandler.cpp");
    }

    // Spawns a NON-transient cube actor. Transient actors are invisible to
    // UEditorActorSubsystem::GetAllLevelActors, which is what an actor subject resolves through, so
    // a transient fixture would make the verb look broken.
    AStaticMeshActor* AnnotatedSubjectsSpawnCube(UWorld* World, const FString& Label,
        const FVector& Location)
    {
        if (!World)
        {
            return nullptr;
        }
        UStaticMesh* CubeMesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
        if (!CubeMesh)
        {
            return nullptr;
        }
        FActorSpawnParameters SpawnParams; // deliberately NOT RF_Transient
        AStaticMeshActor* Actor = World->SpawnActor<AStaticMeshActor>(
            AStaticMeshActor::StaticClass(), Location, FRotator::ZeroRotator, SpawnParams);
        if (!Actor)
        {
            return nullptr;
        }
        Actor->GetStaticMeshComponent()->SetStaticMesh(CubeMesh);
        Actor->SetActorLabel(Label);
        return Actor;
    }
}

// ============================================================================
// 1. Every error code this file emits goes through an ErrorCodes::ERR_* constant.
//
// A SOURCE SCAN, in the shape TestErrorCodeRegistry.cpp already established, because the defect is
// in the spelling at the call site and nothing observable at runtime distinguishes
// SendError(TEXT("SAVE_FAILED"), ...) from SendError(ErrorCodes::ERR_SAVE_FAILED, ...) - the wire
// bytes are identical. That is exactly why it survived: it is invisible to every response-shape
// test, and it used to be invisible to the registry walk too - that walk fails only on a code with
// NO ERR_ constant, so eight hand-spelled but perfectly REGISTERED codes sat in this handler while
// the suite stayed green.
//
// That second gap is now closed generally by PinWright.core.error_codes.
// RegistryAdoptingFilesUseConstantsOnly, which holds every handler file that cites the registry -
// AnnotatedCaptureHandler.cpp among them - to zero hand-spelled codes. This test is kept as belt
// and braces: it names render.capture_annotated in its failure text and pins the verb's own
// contract independently of a tree-wide baseline that other work may edit.
//
// UNABLE TO FAIL IF the file could not be read, or if it contained no SendError call at all - a
// renamed or moved file would then "pass" by having nothing to find. Both are asserted first.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAnnotatedSubjectsNoRawErrorCodeLiteralsTest,
    "PinWright.render.capture_annotated.NoRawErrorCodeLiteralsRemain",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAnnotatedSubjectsNoRawErrorCodeLiteralsTest::RunTest(const FString& Parameters)
{
    const FString SourcePath = AnnotatedHandlerSourcePath();
    if (!TestFalse(TEXT("resolved the plugin source path"), SourcePath.IsEmpty()))
    {
        return false;
    }
    FString Contents;
    if (!TestTrue(FString::Printf(TEXT("AnnotatedCaptureHandler.cpp is readable at %s"), *SourcePath),
            FFileHelper::LoadFileToString(Contents, *SourcePath)))
    {
        return false;
    }

    // Preconditions, so a moved/emptied file cannot pass by being silent.
    const FRegexPattern AnySendErrorPattern(TEXT("SendError\\s*\\("));
    int32 SendErrorCount = 0;
    {
        FRegexMatcher Matcher(AnySendErrorPattern, Contents);
        while (Matcher.FindNext())
        {
            ++SendErrorCount;
        }
    }
    if (!TestTrue(TEXT("the handler still contains SendError call sites to check"), SendErrorCount > 0))
    {
        return false;
    }
    TestTrue(TEXT("the handler references the ErrorCodes registry at least once"),
        Contents.Contains(TEXT("ErrorCodes::ERR_")));

    // The decisive assertion, in both literal spellings the registry walk recognises.
    const FRegexPattern WrappedPattern(
        TEXT("SendError\\s*\\(\\s*TEXT\\(\\s*\"([A-Za-z_][A-Za-z0-9_]*)\"\\s*\\)"));
    const FRegexPattern BarePattern(TEXT("SendError\\s*\\(\\s*\"([A-Za-z_][A-Za-z0-9_]*)\"\\s*,"));
    TArray<FString> RawCodes;
    {
        FRegexMatcher Matcher(WrappedPattern, Contents);
        while (Matcher.FindNext())
        {
            RawCodes.AddUnique(Matcher.GetCaptureGroup(1));
        }
    }
    {
        FRegexMatcher Matcher(BarePattern, Contents);
        while (Matcher.FindNext())
        {
            RawCodes.AddUnique(Matcher.GetCaptureGroup(1));
        }
    }
    RawCodes.Sort();
    for (const FString& Code : RawCodes)
    {
        AddError(FString::Printf(
            TEXT("render.capture_annotated emits '%s' as a raw string literal. Use ")
            TEXT("ErrorCodes::ERR_%s instead - a registered code spelled by hand is still outside ")
            TEXT("the registry as far as grep, IntelliSense and any future rename are concerned."),
            *Code, *Code));
    }
    TestEqual(TEXT("no raw SendError code literals remain in AnnotatedCaptureHandler.cpp"),
        RawCodes.Num(), 0);
    return true;
}

// ============================================================================
// 2. An asset-domain subject is SERVED, not refused.
//
// WHAT THIS TEST USED TO ASSERT, and why it changed. It asserted a blanket
// UNSUPPORTED_ASSET_EDITOR for every asset kind, because the overlays are placed by
// PinWrightViewProjection::FViewProjectionSession and that session rebuilt its view from the active
// LEVEL EDITOR viewport by construction, taking no client argument: painting with it over an
// asset-editor preview frame would have put measured-looking axes, grid lines and boxes on pixels
// that frame never had. The session now takes an FViewProjectionTarget and the verb hands it the
// same client the capture rendered through, so the refusal has no reason left to exist and the
// test asserts its removal instead of being deleted.
//
// WHAT THIS TEST STILL PINS. Only the gate: an asset subject reaches the capture rather than being
// turned away in the argument-validation pass, and it publishes the `subject` block on the way out.
// It deliberately does NOT assert where the overlay ink landed - that needs a decoded PNG and a
// computed coordinate, and lives in TestAnnotatedAssetOverlay.cpp
// (PinWright.render.capture_annotated.AssetOverlayLandsOnTheComputedPixels), because a verb that
// accepted the subject and painted the axes in the wrong place would pass everything below.
//
// UNABLE TO FAIL IF it only asserted "the call did not return UNSUPPORTED_ASSET_EDITOR": the
// resolver itself returns that code for a genuinely unsupported toolkit, so the assertion is on
// the blanket refusal's own MESSAGE, which no other code path produces.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAnnotatedSubjectsAssetKindServedTest,
    "PinWright.render.capture_annotated.AssetSubjectIsServedNotRefused",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAnnotatedSubjectsAssetKindServedTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Subject = MakeShared<FJsonObject>();
    Subject->SetStringField(TEXT("kind"), TEXT("staticMesh"));
    Subject->SetStringField(TEXT("path"), TEXT("/Engine/BasicShapes/Cube.Cube"));

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetNumberField(TEXT("width"), 256.0);
    Payload->SetNumberField(TEXT("height"), 256.0);
    Payload->SetBoolField(TEXT("axes"), false);
    Payload->SetObjectField(TEXT("subject"), Subject);

    FTestResponseCapture Capture;
    TestTrue(TEXT("render.capture_annotated handler found"),
        InvokeHandlerWithCapture(TEXT("render.capture_annotated"), Payload, Capture));
    TestTrue(TEXT("handler sent a response"), Capture.bWasCalled);

    // The decisive assertion, and it holds on a headless host too: the old refusal was raised in
    // the argument-validation pass, so it did not need a viewport and its absence does not either.
    TestFalse(FString::Printf(
        TEXT("an asset subject is no longer refused outright (got '%s': %s)"),
        *Capture.ErrorCode, *Capture.Message),
        Capture.Message.Contains(TEXT("cannot annotate")));
    TestTrue(TEXT("an asset subject is a declared, accepted argument"),
        Capture.ErrorCode != TEXT("UNKNOWN_PARAMS"));

    FString CapturedPath;
    if (Capture.bSuccess && Capture.Result.IsValid())
    {
        Capture.Result->TryGetStringField(TEXT("path"), CapturedPath);
        const TSharedPtr<FJsonObject>* SubjectObj = nullptr;
        if (TestTrue(TEXT("a served asset subject publishes the subject block"),
                Capture.Result->TryGetObjectField(TEXT("subject"), SubjectObj) &&
                SubjectObj && (*SubjectObj).IsValid()))
        {
            FString Kind;
            (*SubjectObj)->TryGetStringField(TEXT("kind"), Kind);
            TestEqual(TEXT("subject.kind echoes the asset kind"), Kind, FString(TEXT("staticMesh")));
        }
        TestTrue(TEXT("a served asset subject publishes the framing block"),
            Capture.Result->HasField(TEXT("framing")));
    }
    else if (!Capture.bSuccess)
    {
        // A host that cannot open or render a static-mesh preview. The gate assertions above
        // already ran; only the response-shape half is unmeasured.
        TestTrue(FString::Printf(TEXT("capture failure is typed (got '%s')"), *Capture.ErrorCode),
            AnnotatedSubjectsIsTypedCaptureFailure(Capture.ErrorCode) ||
            Capture.ErrorCode == TEXT("UNSUPPORTED_ASSET_EDITOR") ||
            Capture.ErrorCode == TEXT("SUBSYSTEM_MISSING") ||
            Capture.ErrorCode == TEXT("OPEN_FAILED") ||
            Capture.ErrorCode == TEXT("ASSET_NOT_FOUND"));
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-preview-capture"),
            TEXT("the subject/framing ")
            TEXT("response-shape assertions need a successful capture. The refusal-is-gone ")
            TEXT("assertion above DID run."));
    }
    AnnotatedSubjectsDeleteFileIfPresent(CapturedPath);
    return true;
}

// ============================================================================
// 3. No subject argument => no `subject` block and no `framing` block.
//
// The absence half of the contract, and the reason it has a test: a block emitted empty (or zeroed)
// on every call is precisely the regression the verb's existing assert-absent cases exist to catch,
// and `subject` must not become the fourteenth one nobody knew about.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAnnotatedSubjectsOmittedWithoutArgumentTest,
    "PinWright.render.capture_annotated.SubjectAndFramingOmittedWithoutTheArgument",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAnnotatedSubjectsOmittedWithoutArgumentTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetNumberField(TEXT("width"), 256.0);
    Payload->SetNumberField(TEXT("height"), 256.0);
    Payload->SetBoolField(TEXT("axes"), true);

    FTestResponseCapture Capture;
    TestTrue(TEXT("render.capture_annotated handler found"),
        InvokeHandlerWithCapture(TEXT("render.capture_annotated"), Payload, Capture));
    TestTrue(TEXT("handler sent a response"), Capture.bWasCalled);

    FString CapturedPath;
    if (Capture.bSuccess && Capture.Result.IsValid())
    {
        Capture.Result->TryGetStringField(TEXT("path"), CapturedPath);

        // The decisive assertions.
        TestFalse(TEXT("no subject block without the argument"),
            Capture.Result->HasField(TEXT("subject")));
        TestFalse(TEXT("no framing block without the argument"),
            Capture.Result->HasField(TEXT("framing")));
        // ...while the frame statistics are unconditional, so absence here is a decision about the
        // subject and not a build without the whole feature.
        TestTrue(TEXT("imageStats is published regardless of the subject"),
            Capture.Result->HasField(TEXT("imageStats")));
        TestTrue(TEXT("blank is published regardless of the subject"),
            Capture.Result->HasField(TEXT("blank")));
    }
    else if (!Capture.bSuccess)
    {
        TestTrue(TEXT("capture failure is typed"),
            AnnotatedSubjectsIsTypedCaptureFailure(Capture.ErrorCode));
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-live-viewport"),
            TEXT("the response-shape ")
            TEXT("assertions need a successful capture."));
    }
    AnnotatedSubjectsDeleteFileIfPresent(CapturedPath);
    return true;
}

// ============================================================================
// 4. The frame statistics are published, and they describe the frame that was rendered.
//
// HOW THIS IS MADE FALSIFIABLE WITHOUT THE BASE PNG. The verb deletes its intermediate base capture
// so a call leaves exactly one file behind, so the base frame cannot be read back directly. With
// EVERY overlay switched off (axes:false, labels:false, no grid, no bounds, no actorLabels) the
// paint pass writes not one pixel, and the saved annotated PNG therefore carries the base frame's
// pixels unchanged. Recomputing PinWrightRenderCapture::CalculateCaptureImageStats over that file -
// the same exported classifier the handler's own numbers came from - must reproduce them.
//
// UNABLE TO FAIL IF the frame were uniform: on a flat frame every statistic collapses to the same
// couple of values and the equality would hold for any implementation, including one that computed
// the stats from the annotated file, or from nothing. The variation precondition
// (maxLuminance > minLuminance) is therefore asserted FIRST, and when it does not hold the
// comparison is reported as not measured rather than as a pass.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAnnotatedSubjectsStatsNotDiscardedTest,
    "PinWright.render.capture_annotated.StatsAreNoLongerDiscarded",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAnnotatedSubjectsStatsNotDiscardedTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetNumberField(TEXT("width"), 256.0);
    Payload->SetNumberField(TEXT("height"), 256.0);
    // Every overlay off: the annotated PNG must then be the base frame, pixel for pixel.
    Payload->SetBoolField(TEXT("axes"), false);
    Payload->SetBoolField(TEXT("labels"), false);

    FTestResponseCapture Capture;
    TestTrue(TEXT("render.capture_annotated handler found"),
        InvokeHandlerWithCapture(TEXT("render.capture_annotated"), Payload, Capture));
    TestTrue(TEXT("handler sent a response"), Capture.bWasCalled);

    if (!Capture.bSuccess)
    {
        TestTrue(TEXT("capture failure is typed"),
            AnnotatedSubjectsIsTypedCaptureFailure(Capture.ErrorCode));
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-live-viewport"),
            TEXT("the statistics ")
            TEXT("comparison needs a rendered frame."));
        return true;
    }
    if (!TestTrue(TEXT("success result exists"), Capture.Result.IsValid()))
    {
        return true;
    }

    FString CapturedPath;
    Capture.Result->TryGetStringField(TEXT("path"), CapturedPath);

    // Structural half: present on every successful capture, whatever the pixels look like.
    const TSharedPtr<FJsonObject>* StatsObj = nullptr;
    const bool bHasStats = Capture.Result->TryGetObjectField(TEXT("imageStats"), StatsObj) &&
        StatsObj && (*StatsObj).IsValid();
    TestTrue(TEXT("response carries imageStats"), bHasStats);
    TestTrue(TEXT("response carries blank"), Capture.Result->HasField(TEXT("blank")));
    TestTrue(TEXT("response carries redrawRetries"), Capture.Result->HasField(TEXT("redrawRetries")));
    if (bHasStats)
    {
        const TCHAR* const RequiredStatsKeys[] = {
            TEXT("meanLuminance"), TEXT("luminanceVariance"), TEXT("minLuminance"),
            TEXT("maxLuminance"), TEXT("litPixelCount"), TEXT("litPixelFraction"),
            TEXT("litLuminanceThreshold") };
        for (const TCHAR* Key : RequiredStatsKeys)
        {
            TestTrue(FString::Printf(TEXT("imageStats carries %s"), Key),
                (*StatsObj)->HasField(FString(Key)));
        }
    }

    // Measured half.
    ON_SCOPE_EXIT { AnnotatedSubjectsDeleteFileIfPresent(CapturedPath); };
    if (!bHasStats || CapturedPath.IsEmpty() || !IFileManager::Get().FileExists(*CapturedPath))
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-frame-on-disk"),
            TEXT("cannot recompute the ")
            TEXT("statistics."));
        return true;
    }

    FImage Loaded;
    if (!FImageUtils::LoadImage(*CapturedPath, Loaded))
    {
        AddError(FString::Printf(TEXT("the annotated PNG at %s could not be decoded"), *CapturedPath));
        return true;
    }
    Loaded.ChangeFormat(ERawImageFormat::BGRA8, EGammaSpace::sRGB);
    TArrayView64<FColor> Pixels = Loaded.AsBGRA8();
    if (!TestTrue(TEXT("the annotated PNG decoded to pixels"), Pixels.Num() > 0))
    {
        return true;
    }
    const PinWrightRenderCapture::FCaptureImageStats Recomputed =
        PinWrightRenderCapture::CalculateCaptureImageStats(
            TConstArrayView<FColor>(Pixels.GetData(), static_cast<int32>(Pixels.Num())));

    // The precondition that makes the comparison evidence rather than arithmetic.
    if (Recomputed.MaxLuminance <= Recomputed.MinLuminance)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("uniform-frame"), FString::Printf(
            TEXT("min=%.6f max=%.6f, so an ")
            TEXT("equality between reported and recomputed statistics would hold for any ")
            TEXT("implementation and proves nothing."),
            Recomputed.MinLuminance, Recomputed.MaxLuminance));
        return true;
    }

    double ReportedMean = 0.0;
    double ReportedMin = 0.0;
    double ReportedMax = 0.0;
    double ReportedLitFraction = 0.0;
    double ReportedLitCount = 0.0;
    (*StatsObj)->TryGetNumberField(TEXT("meanLuminance"), ReportedMean);
    (*StatsObj)->TryGetNumberField(TEXT("minLuminance"), ReportedMin);
    (*StatsObj)->TryGetNumberField(TEXT("maxLuminance"), ReportedMax);
    (*StatsObj)->TryGetNumberField(TEXT("litPixelFraction"), ReportedLitFraction);
    (*StatsObj)->TryGetNumberField(TEXT("litPixelCount"), ReportedLitCount);

    // Lossless PNG both ways, so these are equalities, not approximations; the tolerance only
    // absorbs the JSON double round-trip.
    TestTrue(FString::Printf(
        TEXT("imageStats.meanLuminance matches the frame (reported %.9f, recomputed %.9f)"),
        ReportedMean, Recomputed.MeanLuminance),
        FMath::IsNearlyEqual(ReportedMean, Recomputed.MeanLuminance, 1.0e-6));
    TestTrue(FString::Printf(
        TEXT("imageStats.minLuminance matches the frame (reported %.9f, recomputed %.9f)"),
        ReportedMin, Recomputed.MinLuminance),
        FMath::IsNearlyEqual(ReportedMin, Recomputed.MinLuminance, 1.0e-6));
    TestTrue(FString::Printf(
        TEXT("imageStats.maxLuminance matches the frame (reported %.9f, recomputed %.9f)"),
        ReportedMax, Recomputed.MaxLuminance),
        FMath::IsNearlyEqual(ReportedMax, Recomputed.MaxLuminance, 1.0e-6));
    TestTrue(FString::Printf(
        TEXT("imageStats.litPixelFraction matches the frame (reported %.9f, recomputed %.9f)"),
        ReportedLitFraction, Recomputed.LitPixelFraction),
        FMath::IsNearlyEqual(ReportedLitFraction, Recomputed.LitPixelFraction, 1.0e-6));
    TestEqual(TEXT("imageStats.litPixelCount matches the frame"),
        static_cast<int64>(ReportedLitCount), Recomputed.LitPixelCount);

    bool bReportedBlank = true;
    Capture.Result->TryGetBoolField(TEXT("blank"), bReportedBlank);
    TestTrue(TEXT("blank matches the classifier's verdict for this frame"),
        bReportedBlank == Recomputed.bBlank);
    return true;
}

// ============================================================================
// 5. An actor subject publishes `subject` and a `framing` verdict with bounds behind it.
//
// The positive half of test 3. `framing.evaluated` is the field that separates "measured" from
// "there were no bounds to measure against", so asserting it true is what makes the block worth
// emitting; a block that always reported evaluated:false would satisfy a mere presence check.
//
// UNABLE TO FAIL IF the fixture had no extent - a zero-radius subject yields evaluated:false by
// construction. The fixture is the engine unit cube and its bounds radius is asserted non-zero
// before the verb is called.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAnnotatedSubjectsActorSubjectFramingTest,
    "PinWright.render.capture_annotated.ActorSubjectPublishesSubjectAndFraming",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAnnotatedSubjectsActorSubjectFramingTest::RunTest(const FString& Parameters)
{
    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("there is no editor world to spawn the fixture into"));
        return true;
    }

    FScopedEditorWorldActorGuard ActorGuard;
    const FString Label = FString::Printf(TEXT("PW_AnnotatedSubject_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    AStaticMeshActor* Actor = AnnotatedSubjectsSpawnCube(World, Label, FVector::ZeroVector);
    if (!Actor)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-cube-fixture"),
            TEXT("the engine cube fixture could not be spawned"));
        return true;
    }
    const FString ResolvedName = Actor->GetActorLabel();

    // The precondition that keeps `evaluated:true` from being free.
    FVector FixtureOrigin = FVector::ZeroVector;
    FVector FixtureExtent = FVector::ZeroVector;
    Actor->GetActorBounds(false, FixtureOrigin, FixtureExtent);
    if (!TestTrue(TEXT("the fixture has non-zero bounds to frame against"),
            FixtureExtent.GetMax() > 1.0))
    {
        return true;
    }

    TSharedPtr<FJsonObject> Subject = MakeShared<FJsonObject>();
    Subject->SetStringField(TEXT("kind"), TEXT("actor"));
    Subject->SetStringField(TEXT("name"), ResolvedName);

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    // Fixed capture size, matching every other capture in this suite: a size that varies within a
    // session trips FViewport::GetHitProxy's ProxyMap assertion.
    Payload->SetNumberField(TEXT("width"), 256.0);
    Payload->SetNumberField(TEXT("height"), 256.0);
    Payload->SetStringField(TEXT("projectionMode"), TEXT("perspective"));
    Payload->SetNumberField(TEXT("fov"), 60.0);
    TSharedPtr<FJsonObject> Loc = MakeShared<FJsonObject>();
    Loc->SetNumberField(TEXT("x"), -500.0);
    Loc->SetNumberField(TEXT("y"), -500.0);
    Loc->SetNumberField(TEXT("z"), 400.0);
    Payload->SetObjectField(TEXT("location"), Loc);
    TSharedPtr<FJsonObject> Rot = MakeShared<FJsonObject>();
    Rot->SetNumberField(TEXT("pitch"), -30.0);
    Rot->SetNumberField(TEXT("yaw"), 45.0);
    Rot->SetNumberField(TEXT("roll"), 0.0);
    Payload->SetObjectField(TEXT("rotation"), Rot);
    Payload->SetBoolField(TEXT("axes"), true);
    Payload->SetObjectField(TEXT("subject"), Subject);

    FTestResponseCapture Capture;
    TestTrue(TEXT("render.capture_annotated handler found"),
        InvokeHandlerWithCapture(TEXT("render.capture_annotated"), Payload, Capture));
    TestTrue(TEXT("handler sent a response"), Capture.bWasCalled);

    FString CapturedPath;
    if (!Capture.bSuccess)
    {
        // An actor subject must never turn a working capture into a param error.
        TestNotEqual(TEXT("an actor subject is a declared, accepted argument"),
            Capture.ErrorCode, FString(TEXT("UNKNOWN_PARAMS")));
        TestTrue(TEXT("capture failure is typed"),
            AnnotatedSubjectsIsTypedCaptureFailure(Capture.ErrorCode));
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-live-viewport"),
            TEXT("the subject/framing ")
            TEXT("assertions need a successful capture."));
        return true;
    }
    if (!TestTrue(TEXT("success result exists"), Capture.Result.IsValid()))
    {
        return true;
    }
    Capture.Result->TryGetStringField(TEXT("path"), CapturedPath);
    ON_SCOPE_EXIT { AnnotatedSubjectsDeleteFileIfPresent(CapturedPath); };

    TestTrue(TEXT("a resolved subject publishes the subject block"),
        Capture.Result->HasField(TEXT("subject")));

    const TSharedPtr<FJsonObject>* FramingObj = nullptr;
    if (!TestTrue(TEXT("a resolved subject publishes the framing block"),
            Capture.Result->TryGetObjectField(TEXT("framing"), FramingObj) &&
            FramingObj && (*FramingObj).IsValid()))
    {
        return true;
    }
    bool bEvaluated = false;
    (*FramingObj)->TryGetBoolField(TEXT("evaluated"), bEvaluated);
    TestTrue(TEXT("framing was actually evaluated against the subject's bounds"), bEvaluated);
    if (bEvaluated)
    {
        TestTrue(TEXT("framing carries its verdict"), (*FramingObj)->HasField(TEXT("boundsInFrame")));
        TestTrue(TEXT("framing carries the numbers behind the verdict"),
            (*FramingObj)->HasField(TEXT("offAxis")) && (*FramingObj)->HasField(TEXT("frameLimit")));
        bool bInFrame = false;
        (*FramingObj)->TryGetBoolField(TEXT("boundsInFrame"), bInFrame);
        // The assert-absent rule this block inherits: framingWarning exists only when the subject
        // is provably out of frame.
        if (bInFrame)
        {
            TestFalse(TEXT("no framingWarning when the subject is in frame"),
                (*FramingObj)->HasField(TEXT("framingWarning")));
        }
    }
    return true;
}
