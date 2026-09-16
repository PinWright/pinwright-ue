// Copyright (c) 2026 Alexander Penkin. MIT License.

// Tests for the capture-time exposure pin (the shared `exposure` parameter on every capture verb).
//
// WHAT THIS DEFENDS. Auto-exposure is a live scalar gain over the whole frame that re-balances
// between shots, so two captures of the same scene are not comparable and an edit that DID work
// is partly cancelled by the camera re-exposing -- it gets recorded as a no-op and reverted. That
// is the failure; "the verb returned success" was never in question and asserting it would prove
// nothing.
//
// The load-bearing properties, in the order this file asserts them:
//
//  1. OMITTED CHANGES NOTHING. The parameter is back-compatible or it is a regression dressed as
//     a feature. Asserted both on the parser (no `exposure` -> Unset, nothing to apply) and on a
//     live call (the viewport client's ExposureSettings are byte-identical afterwards).
//  2. A BAD REQUEST IS REFUSED, NOT DEGRADED. An unknown mode or a "fixed" without an ev100 must
//     error. Silently falling back to auto is precisely the uncomparable pair the parameter
//     exists to prevent, and the caller would never learn it happened (rpc-design.md section 3).
//  3. A PIN THAT DID NOT TAKE IS REPORTED. `pinned` is a measurement, and when it is false while
//     a pin was requested the response carries a warning naming the reason. This is the whole
//     contract: a frame that quietly used auto-exposure while the caller believes it is pinned is
//     worse than no parameter at all (rpc-design.md section 1).
//  4. THE VIEWPORT IS PUT BACK. A capture that leaves the editor's exposure moved silently
//     re-exposes every later capture in the session -- a new no-op generator rather than a fix.
//  5. THE PIN ACTUALLY GOVERNS PIXELS. Two captures with the SAME pin reproduce to within a
//     measured noise floor, and a capture with a DIFFERENT ev100 lands two orders of magnitude
//     past it. The second half is what makes the first non-vacuous: an inert pin would also
//     produce two matching frames. Note "reproduce", not "are identical" -- a pinned viewport
//     capture is NOT byte-stable (see the calibration comment on that test), so the pair is
//     compared within a tolerance and the frame is separately asserted to be lit.
//
// Two signals are easy to confuse, so they are asserted apart: `viewport.exposure`'s
// `adaptedReadbackPending` is a fact about the renderer's eye-adaptation readback (true on a fully
// warmed frame, and on every pinned capture), while `viewport.warmup.warmupWarning` is the measured
// verdict on whether these pixels had stopped changing. The first was once published as the second.
//
// Headless behaviour. Items 1-4 are pure functions of a request/response struct or of the
// registered handler's argument validation, so they run without an RHI or a viewport. Item 5
// drives a real capture and follows the RHI-guard pattern of TestAnnotatedCaptureHandlers.cpp: on
// failure the error code must be in the typed allow-list and the pixel assertions are skipped.
//
// EVERY SKIP IN THIS FILE ANNOUNCES ITSELF. Those guards are correct -- a host with no lit preview
// scene cannot show an exposure response -- but a skip that is silent is worse than no test: it
// reports a verdict about a property it never examined. Each early return therefore emits the
// `PINWRIGHT_ASSERTIONS_SKIPPED:` marker through PinWrightTestSkip::SkipAssertions, which
// `Content/Python/check_suite_log.py` reconciles as its own `skipped` outcome so that a run
// measuring nothing cannot read as COMPLETED_CLEAN. It is a WARNING, never a failure: making it
// fatal would recreate `B-tests-host-dependent-fixtures-hard-fail` on every headless host. Board
// ticket: `B-test-skips-assertions-silently`.
#include "Misc/AutomationTest.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/HandlerRegistration.h"
#include "Handlers/Render/PreviewViewportCaptureUtils.h"
#include "Tests/TestUtils.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "EditorViewportClient.h"
#include "Engine/EngineBaseTypes.h"
#include "HAL/FileManager.h"
#include "IAssetViewport.h"
#include "ImageCore.h"
#include "ImageUtils.h"
#include "LevelEditor.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Modules/ModuleManager.h"
#include "Tests/TestSkipReporting.h"

namespace
{
    using PinWrightRenderCapture::EExposureRequestMode;
    using PinWrightRenderCapture::FExposurePin;
    using PinWrightRenderCapture::FViewportCaptureOutput;

    // Names are deliberately prefixed: two anonymous namespaces in one Unity translation unit
    // merge, so a bare helper name that a sibling Tests/Render/*.cpp also uses is a latent
    // redefinition that adaptive unity hides until both files are committed (docs/lessons.md).

    // The typed, non-crashing exits a capture verb may return when no live viewport or RHI is
    // available. Anything outside this list is a real defect, not a headless host.
    bool ExposurePinIsTypedCaptureFailure(const FString& ErrorCode)
    {
        return ErrorCode == TEXT("NO_ACTIVE_LEVEL_VIEWPORT") ||
            ErrorCode == TEXT("NO_EDITOR_WORLD") ||
            ErrorCode == TEXT("EDITOR_NOT_AVAILABLE") ||
            ErrorCode == TEXT("CAPTURE_FAILED") ||
            ErrorCode == TEXT("PREVIEW_VIEWPORT_NOT_FOUND") ||
            ErrorCode == TEXT("UNSUPPORTED_ORTHOGRAPHIC_ROTATION") ||
            ErrorCode == TEXT("UNSUPPORTED_ASSET_EDITOR") ||
            ErrorCode == TEXT("ASSET_NOT_FOUND") ||
            ErrorCode == TEXT("BLANK_CAPTURE") ||
            ErrorCode == TEXT("ENCODE_FAILED") ||
            ErrorCode == TEXT("SAVE_FAILED") ||
            // The pin mechanism itself failing is a typed exit too. It must never be silent, but
            // on a host that cannot drive a viewport it is not a defect in this change.
            ErrorCode == TEXT("EXPOSURE_PIN_FAILED");
    }
    // Skips are reported through PinWrightTestSkip::SkipAssertions (Tests/TestSkipReporting.h),
    // which owns the wire literal and the three properties that make it work -- AddWarning rather
    // than UE_LOG, Warning verbosity rather than Info, and prefix-matched parsing. This file is
    // where board ticket `B-test-skips-assertions-silently` was found: its lit-mode gate routed
    // three consecutive runs to a NOT MEASURED path, all three reported green, and nothing
    // downstream could tell the two substantive assertions had never executed. The gate is right
    // -- asserting an exposure response against pixels that do not respond to exposure asserts
    // something the environment cannot show -- so the fix was to make the skip COUNTABLE, not to
    // fail. The full rationale now lives in that header.

    // The active Level Editor viewport client, or nullptr when there is none (headless host).
    // Used to read the editor's real exposure state around a call -- the restore assertion has to
    // measure the editor, not the response, or it is the capture grading its own homework.
    FEditorViewportClient* ExposurePinActiveLevelViewportClient()
    {
        FLevelEditorModule* LevelEditorModule =
            FModuleManager::GetModulePtr<FLevelEditorModule>(TEXT("LevelEditor"));
        if (!LevelEditorModule)
        {
            return nullptr;
        }
        TSharedPtr<IAssetViewport> ActiveViewport = LevelEditorModule->GetFirstActiveViewport();
        return ActiveViewport.IsValid() ? &ActiveViewport->GetAssetViewportClient() : nullptr;
    }

    void ExposurePinDeleteCaptureFile(const FTestResponseCapture& Capture)
    {
        FString Path;
        if (Capture.bSuccess && Capture.Result.IsValid() &&
            Capture.Result->TryGetStringField(TEXT("path"), Path) && !Path.IsEmpty())
        {
            IFileManager::Get().Delete(*Path, false, true);
        }
    }

    // The `viewport.exposure` sub-block of a successful capture response, or an invalid pointer.
    TSharedPtr<FJsonObject> ExposurePinGetExposureBlock(const FTestResponseCapture& Capture)
    {
        if (!Capture.bSuccess || !Capture.Result.IsValid())
        {
            return nullptr;
        }
        const TSharedPtr<FJsonObject>* Viewport = nullptr;
        if (!Capture.Result->TryGetObjectField(TEXT("viewport"), Viewport) || !Viewport ||
            !Viewport->IsValid())
        {
            return nullptr;
        }
        const TSharedPtr<FJsonObject>* Exposure = nullptr;
        if (!(*Viewport)->TryGetObjectField(TEXT("exposure"), Exposure) || !Exposure)
        {
            return nullptr;
        }
        return *Exposure;
    }

    // The `warmup` sub-block of a built viewport info object, or an invalid pointer. Warm-up is
    // measured on the frame and reported here; the exposure block carries no warm-up signal.
    TSharedPtr<FJsonObject> ExposurePinGetWarmupBlock(const TSharedPtr<FJsonObject>& Viewport)
    {
        if (!Viewport.IsValid())
        {
            return nullptr;
        }
        const TSharedPtr<FJsonObject>* Warmup = nullptr;
        if (!Viewport->TryGetObjectField(TEXT("warmup"), Warmup) || !Warmup)
        {
            return nullptr;
        }
        return *Warmup;
    }

    // A minimal capture output standing in for a real capture that requested a pin. Only the
    // exposure fields are filled, because MakeViewportInfoObject is a pure function of this
    // struct -- which is exactly why the reporting contract is testable without a viewport.
    FViewportCaptureOutput ExposurePinMakeCaptureOutput(bool bRequested, bool bPinned,
        const TCHAR* BlockedReason)
    {
        FViewportCaptureOutput Capture;
        Capture.ViewModeKey = TEXT("Lit");
        Capture.ViewMode = TEXT("display:Lit");
        Capture.ViewModeValue = static_cast<int32>(VMI_Lit);
        Capture.bLitViewMode = true;
        Capture.ExposureMode =
            bRequested ? EExposureRequestMode::Fixed : EExposureRequestMode::Unset;
        Capture.bExposurePinRequested = bRequested;
        Capture.bExposurePinned = bPinned;
        Capture.Ev100Requested = bRequested ? 9.5f : 0.0f;
        Capture.bExposureFixedApplied = bRequested;
        Capture.Ev100Applied = bRequested ? 9.5f : 0.0f;
        Capture.ExposurePinBlockedReason = BlockedReason;
        return Capture;
    }

    // Decoded pixels of a saved PNG. Empty on failure so a caller can treat it as "not checked"
    // rather than as an empty image that trivially matches another empty one.
    TArray<FColor> ExposurePinLoadPixels(const FString& PngPath)
    {
        TArray<FColor> Pixels;
        FImage Loaded;
        if (!FImageUtils::LoadImage(*PngPath, Loaded))
        {
            return Pixels;
        }
        Loaded.ChangeFormat(ERawImageFormat::BGRA8, EGammaSpace::sRGB);
        TArrayView64<FColor> View = Loaded.AsBGRA8();
        Pixels.Reserve(static_cast<int32>(View.Num()));
        for (const FColor& C : View)
        {
            Pixels.Add(C);
        }
        return Pixels;
    }

    int32 ExposurePinCountDifferingPixels(const TArray<FColor>& A, const TArray<FColor>& B)
    {
        if (A.Num() != B.Num())
        {
            return FMath::Max(A.Num(), B.Num());
        }
        int32 Differing = 0;
        for (int32 Index = 0; Index < A.Num(); ++Index)
        {
            if (A[Index] != B[Index])
            {
                ++Differing;
            }
        }
        return Differing;
    }

    // Mean absolute difference between two decoded frames, in 0-255 byte units, averaged over the
    // pixels and over the three colour channels. This is the magnitude measure the exposure
    // assertions run on; a differing-pixel COUNT cannot separate the two cases they have to tell
    // apart, because near-100% of pixels differ under both temporal jitter and a real exposure
    // change -- only the size of the difference separates them.
    //
    // Alpha is excluded deliberately: a capture's alpha channel is a separate contract (opaque vs.
    // 0) with its own tests, and folding it in would let an alpha-only change register as an
    // exposure change.
    double ExposurePinMeanAbsDifference(const TArray<FColor>& A, const TArray<FColor>& B)
    {
        if (A.Num() == 0 || A.Num() != B.Num())
        {
            // Not comparable. Return a value no tolerance accepts rather than a 0 that would read
            // as "these matched".
            return TNumericLimits<double>::Max();
        }
        double Sum = 0.0;
        for (int32 Index = 0; Index < A.Num(); ++Index)
        {
            Sum += FMath::Abs(static_cast<double>(A[Index].R) - static_cast<double>(B[Index].R));
            Sum += FMath::Abs(static_cast<double>(A[Index].G) - static_cast<double>(B[Index].G));
            Sum += FMath::Abs(static_cast<double>(A[Index].B) - static_cast<double>(B[Index].B));
        }
        return Sum / (static_cast<double>(A.Num()) * 3.0);
    }

    // Mean Rec.709 luminance in 0..1, computed exactly as CalculateCaptureImageStats does
    // (PreviewViewportCaptureUtils.cpp), so a number measured here is directly comparable with the
    // `imageStats.meanLuminance` a capture response publishes. Computed from the decoded PNG
    // rather than read out of the response: this has to be a property of the very pixels the
    // difference assertions ran on.
    double ExposurePinMeanLuminance(const TArray<FColor>& Pixels)
    {
        if (Pixels.Num() == 0)
        {
            return 0.0;
        }
        double Sum = 0.0;
        for (const FColor& Color : Pixels)
        {
            Sum += (0.2126 * static_cast<double>(Color.R) +
                    0.7152 * static_cast<double>(Color.G) +
                    0.0722 * static_cast<double>(Color.B)) / 255.0;
        }
        return Sum / static_cast<double>(Pixels.Num());
    }

    // A capture_open_level payload: small, orthographic top-down, blank frames accepted so a
    // dark or empty host map cannot turn an exposure assertion into a BLANK_CAPTURE failure.
    TSharedPtr<FJsonObject> ExposurePinMakeLevelPayload()
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetNumberField(TEXT("width"), 128);
        Payload->SetNumberField(TEXT("height"), 128);
        Payload->SetStringField(TEXT("projectionMode"), TEXT("orthographic"));
        Payload->SetNumberField(TEXT("orthoWidth"), 4000);
        Payload->SetBoolField(TEXT("allowBlank"), true);
        TSharedPtr<FJsonObject> Rotation = MakeShared<FJsonObject>();
        Rotation->SetNumberField(TEXT("pitch"), -90);
        Rotation->SetNumberField(TEXT("yaw"), 0);
        Rotation->SetNumberField(TEXT("roll"), 0);
        Payload->SetObjectField(TEXT("rotation"), Rotation);
        return Payload;
    }
}

// ============================================================================
// 1. The parameter contract: omitted changes nothing, malformed is refused.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCaptureExposurePinParamParsedTest,
    "PinWright.render.exposure_pin.ParamParsed",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCaptureExposurePinParamParsedTest::RunTest(const FString& Parameters)
{
    FString ErrorCode;
    FString ErrorMessage;

    // --- omitted: the back-compatibility guarantee, at the parser ---
    {
        PinWrightRenderCapture::FViewportCaptureRequest Request;
        TSharedPtr<FJsonObject> Omitted = MakeShared<FJsonObject>();
        TestTrue(TEXT("a payload without `exposure` parses"),
            PinWrightRenderCapture::ParseViewportCaptureRequest(Omitted, Request, ErrorCode, ErrorMessage));
        TestTrue(TEXT("omitted exposure is Unset"),
            Request.Exposure.Mode == EExposureRequestMode::Unset);
        TestFalse(TEXT("omitted exposure pins nothing"), Request.Exposure.WantsPin());
    }

    // --- bare-number shorthand ---
    {
        FExposurePin Pin;
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetNumberField(TEXT("exposure"), 12.5);
        TestTrue(TEXT("a bare number parses"),
            PinWrightRenderCapture::ParseExposurePin(Payload, Pin, ErrorCode, ErrorMessage));
        TestTrue(TEXT("a bare number means fixed"), Pin.Mode == EExposureRequestMode::Fixed);
        TestTrue(TEXT("a bare number pins"), Pin.WantsPin());
        TestEqual(TEXT("the bare number is the EV100"), Pin.Ev100, 12.5f);
    }

    // --- object form, fixed ---
    {
        FExposurePin Pin;
        TSharedPtr<FJsonObject> Inner = MakeShared<FJsonObject>();
        Inner->SetStringField(TEXT("mode"), TEXT("fixed"));
        Inner->SetNumberField(TEXT("ev100"), -3.25);
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetObjectField(TEXT("exposure"), Inner);
        TestTrue(TEXT("{mode:fixed, ev100} parses"),
            PinWrightRenderCapture::ParseExposurePin(Payload, Pin, ErrorCode, ErrorMessage));
        TestTrue(TEXT("mode fixed pins"), Pin.WantsPin());
        TestEqual(TEXT("ev100 round-trips, including a negative one"), Pin.Ev100, -3.25f);
    }

    // --- object form, explicit auto: understood, and NOT a pin ---
    {
        FExposurePin Pin;
        TSharedPtr<FJsonObject> Inner = MakeShared<FJsonObject>();
        Inner->SetStringField(TEXT("mode"), TEXT("AUTO"));
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetObjectField(TEXT("exposure"), Inner);
        TestTrue(TEXT("{mode:auto} parses, case-insensitively"),
            PinWrightRenderCapture::ParseExposurePin(Payload, Pin, ErrorCode, ErrorMessage));
        TestTrue(TEXT("auto is recorded as auto, not as unset"),
            Pin.Mode == EExposureRequestMode::Auto);
        TestFalse(TEXT("auto writes nothing"), Pin.WantsPin());
    }

    // --- the failure direction: every malformed shape must be REFUSED, never degraded to auto ---
    const auto ExpectRejected = [&](const TCHAR* What, const TSharedPtr<FJsonValue>& Value)
    {
        FExposurePin Pin;
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetField(TEXT("exposure"), Value);
        ErrorCode.Reset();
        TestFalse(FString::Printf(TEXT("%s is rejected"), What),
            PinWrightRenderCapture::ParseExposurePin(Payload, Pin, ErrorCode, ErrorMessage));
        TestEqual(FString::Printf(TEXT("%s is a typed INVALID_ARGUMENT"), What),
            ErrorCode, FString(TEXT("INVALID_ARGUMENT")));
        // The decisive half: a rejected request must not leave a pin behind that a caller could
        // mistake for "auto was chosen for me".
        TestFalse(FString::Printf(TEXT("%s leaves no pin"), What), Pin.WantsPin());
    };

    {
        TSharedPtr<FJsonObject> UnknownMode = MakeShared<FJsonObject>();
        UnknownMode->SetStringField(TEXT("mode"), TEXT("manual"));
        UnknownMode->SetNumberField(TEXT("ev100"), 10.0);
        ExpectRejected(TEXT("an unknown mode"), MakeShared<FJsonValueObject>(UnknownMode));
    }
    {
        // No safe default exists for an EV100, so "fixed" without one is an error rather than a
        // value this verb invents and then reports as though the caller chose it.
        TSharedPtr<FJsonObject> NoEv = MakeShared<FJsonObject>();
        NoEv->SetStringField(TEXT("mode"), TEXT("fixed"));
        ExpectRejected(TEXT("mode fixed without ev100"), MakeShared<FJsonValueObject>(NoEv));
    }
    {
        TSharedPtr<FJsonObject> Empty = MakeShared<FJsonObject>();
        ExpectRejected(TEXT("an empty exposure object"), MakeShared<FJsonValueObject>(Empty));
    }
    {
        // The contradiction. "auto" says leave auto-exposure running, `ev100` says pin at a
        // number, and no frame can be both. This used to be ACCEPTED with the ev100 silently
        // discarded -- so the caller got an auto-exposed frame while the request in hand named an
        // EV100, which is the uncomparable pair the parameter exists to prevent, arrived at by the
        // parameter itself. rpc-design.md section 3: there is no safe half to keep, so refuse.
        TSharedPtr<FJsonObject> AutoWithEv = MakeShared<FJsonObject>();
        AutoWithEv->SetStringField(TEXT("mode"), TEXT("auto"));
        AutoWithEv->SetNumberField(TEXT("ev100"), 5.0);
        ExpectRejected(TEXT("mode auto with an ev100"), MakeShared<FJsonValueObject>(AutoWithEv));
    }
    {
        // Case-insensitively, and for an ev100 of 0 -- which is a perfectly ordinary EV100 and
        // must not be read as "no ev100 was supplied" by a bHasEv100 that tested the value.
        TSharedPtr<FJsonObject> AutoWithZero = MakeShared<FJsonObject>();
        AutoWithZero->SetStringField(TEXT("mode"), TEXT("Auto"));
        AutoWithZero->SetNumberField(TEXT("ev100"), 0.0);
        ExpectRejected(TEXT("mode Auto with ev100 0"), MakeShared<FJsonValueObject>(AutoWithZero));
    }
    {
        // The refusal names BOTH halves, so the caller can see which two things collided rather
        // than being told one of them is invalid on its own (it is not).
        FExposurePin Pin;
        TSharedPtr<FJsonObject> Inner = MakeShared<FJsonObject>();
        Inner->SetStringField(TEXT("mode"), TEXT("auto"));
        Inner->SetNumberField(TEXT("ev100"), 5.0);
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetObjectField(TEXT("exposure"), Inner);
        ErrorCode.Reset();
        ErrorMessage.Reset();
        PinWrightRenderCapture::ParseExposurePin(Payload, Pin, ErrorCode, ErrorMessage);
        TestTrue(TEXT("the contradiction message names the mode"),
            ErrorMessage.Contains(TEXT("auto")));
        TestTrue(TEXT("the contradiction message names ev100"),
            ErrorMessage.Contains(TEXT("ev100")));
        TestTrue(TEXT("the contradiction message names the offending value"),
            ErrorMessage.Contains(TEXT("5")));
    }
    ExpectRejected(TEXT("a string"), MakeShared<FJsonValueString>(TEXT("fixed")));
    ExpectRejected(TEXT("a boolean"), MakeShared<FJsonValueBoolean>(true));
    ExpectRejected(TEXT("an out-of-range ev100"),
        MakeShared<FJsonValueNumber>(static_cast<double>(PinWrightRenderCapture::MaxExposureEv100) + 1.0));
    ExpectRejected(TEXT("an out-of-range negative ev100"),
        MakeShared<FJsonValueNumber>(static_cast<double>(PinWrightRenderCapture::MinExposureEv100) - 1.0));

    // A rejected `exposure` has to fail the WHOLE capture request, not be swallowed by the
    // surrounding parser -- otherwise the verb captures anyway, unpinned, and reports success.
    {
        PinWrightRenderCapture::FViewportCaptureRequest Request;
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        TSharedPtr<FJsonObject> Inner = MakeShared<FJsonObject>();
        Inner->SetStringField(TEXT("mode"), TEXT("nonsense"));
        Payload->SetObjectField(TEXT("exposure"), Inner);
        ErrorCode.Reset();
        TestFalse(TEXT("a bad exposure fails ParseViewportCaptureRequest as a whole"),
            PinWrightRenderCapture::ParseViewportCaptureRequest(Payload, Request, ErrorCode, ErrorMessage));
        TestEqual(TEXT("and it surfaces as INVALID_ARGUMENT"), ErrorCode,
            FString(TEXT("INVALID_ARGUMENT")));
    }
    return true;
}

// ============================================================================
// 2. The honesty contract, asserted on the pure reporting function.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCaptureExposurePinFailedPinIsReportedTest,
    "PinWright.render.exposure_pin.FailedPinIsReported",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCaptureExposurePinFailedPinIsReportedTest::RunTest(const FString& Parameters)
{
    // The condition this whole parameter exists to stop being silent: the caller asked for a pin,
    // the frame came back, and the renderer never applied it.
    const TCHAR* Reason = TEXT("the Lighting show flag is off on this viewport");
    const TSharedPtr<FJsonObject> Viewport = PinWrightRenderCapture::MakeViewportInfoObject(
        ExposurePinMakeCaptureOutput(/*bRequested=*/true, /*bPinned=*/false, Reason));

    if (!TestTrue(TEXT("viewport block built"), Viewport.IsValid()))
    {
        return true;
    }
    const TSharedPtr<FJsonObject>* Exposure = nullptr;
    if (!TestTrue(TEXT("every capture carries an exposure block"),
            Viewport->TryGetObjectField(TEXT("exposure"), Exposure) && Exposure &&
            Exposure->IsValid()))
    {
        return true;
    }

    bool bPinRequested = false;
    TestTrue(TEXT("`pinRequested` is present"),
        (*Exposure)->TryGetBoolField(TEXT("pinRequested"), bPinRequested));
    TestTrue(TEXT("the request is echoed as requested"), bPinRequested);

    bool bPinned = true;
    TestTrue(TEXT("`pinned` is present"), (*Exposure)->TryGetBoolField(TEXT("pinned"), bPinned));
    TestFalse(TEXT("a pin the renderer ignored reports pinned=false"), bPinned);

    FString Warning;
    if (TestTrue(TEXT("a requested-but-ungoverned pin carries pinWarning"),
            (*Exposure)->TryGetStringField(TEXT("pinWarning"), Warning)))
    {
        // "the pin failed" is not actionable. The reason is what tells a caller which of its
        // frames to throw away and what to change before re-shooting.
        TestTrue(TEXT("the warning carries the measured reason"), Warning.Contains(Reason));
        TestTrue(TEXT("the warning states the consequence"),
            Warning.Contains(TEXT("auto-exposure")));
        TestTrue(TEXT("the warning names the remedy verb"),
            Warning.Contains(TEXT("editor.set_view_mode")));
    }

    // The other half: a pin that DID govern must not cry wolf, or the warning becomes noise and
    // stops being read.
    const TSharedPtr<FJsonObject> GovernedViewport = PinWrightRenderCapture::MakeViewportInfoObject(
        ExposurePinMakeCaptureOutput(/*bRequested=*/true, /*bPinned=*/true, TEXT("")));
    const TSharedPtr<FJsonObject>* Governed = nullptr;
    if (TestTrue(TEXT("governed capture has an exposure block"),
            GovernedViewport->TryGetObjectField(TEXT("exposure"), Governed) && Governed))
    {
        bool bGovernedPinned = false;
        (*Governed)->TryGetBoolField(TEXT("pinned"), bGovernedPinned);
        TestTrue(TEXT("a governed pin reports pinned=true"), bGovernedPinned);
        TestFalse(TEXT("a governed pin carries no pinWarning"),
            (*Governed)->HasField(TEXT("pinWarning")));
        double Ev100 = 0.0;
        TestTrue(TEXT("a governed pin reports the EV100 the pixels used"),
            (*Governed)->TryGetNumberField(TEXT("ev100"), Ev100));
        TestEqual(TEXT("the reported EV100 is the applied one"), Ev100, 9.5);
    }

    // And an omitted parameter reports itself as omitted, with no warning at all -- an unpinned
    // capture is the normal case and must not be noisy.
    const TSharedPtr<FJsonObject> UnsetViewport = PinWrightRenderCapture::MakeViewportInfoObject(
        ExposurePinMakeCaptureOutput(/*bRequested=*/false, /*bPinned=*/false, TEXT("")));
    const TSharedPtr<FJsonObject>* Unset = nullptr;
    if (TestTrue(TEXT("an unpinned capture still carries an exposure block"),
            UnsetViewport->TryGetObjectField(TEXT("exposure"), Unset) && Unset))
    {
        FString Mode;
        (*Unset)->TryGetStringField(TEXT("mode"), Mode);
        TestEqual(TEXT("an omitted parameter reports mode 'unset'"), Mode, FString(TEXT("unset")));
        bool bRequested = true;
        (*Unset)->TryGetBoolField(TEXT("pinRequested"), bRequested);
        TestFalse(TEXT("nobody asked for a pin"), bRequested);
        TestFalse(TEXT("an unpinned capture carries no pinWarning"),
            (*Unset)->HasField(TEXT("pinWarning")));
        // An unmeasured adapted exposure is OMITTED, never emitted as a zero that would read as
        // "the renderer resolved an exposure of 0".
        bool bAdaptedMeasured = true;
        TestTrue(TEXT("`adaptedMeasured` is present"),
            (*Unset)->TryGetBoolField(TEXT("adaptedMeasured"), bAdaptedMeasured));
        TestFalse(TEXT("nothing was measured on this synthetic output"), bAdaptedMeasured);
        TestFalse(TEXT("so no adapted number is published"), (*Unset)->HasField(TEXT("adapted")));
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCaptureExposurePinUnrestoredIsReportedTest,
    "PinWright.render.exposure_pin.UnrestoredExposureIsReported",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCaptureExposurePinUnrestoredIsReportedTest::RunTest(const FString& Parameters)
{
    FViewportCaptureOutput Capture =
        ExposurePinMakeCaptureOutput(/*bRequested=*/true, /*bPinned=*/true, TEXT(""));
    Capture.bExposureRestored = false;
    Capture.bExposureFixedBefore = false;
    Capture.Ev100Before = 1.0f;

    const TSharedPtr<FJsonObject> Exposure = PinWrightRenderCapture::MakeExposureInfoObject(Capture);
    bool bRestored = true;
    TestTrue(TEXT("`restored` is present on every capture"),
        Exposure->TryGetBoolField(TEXT("restored"), bRestored));
    TestFalse(TEXT("a capture that did not restore says so"), bRestored);
    FString Warning;
    if (TestTrue(TEXT("an unrestored capture carries restoreWarning"),
            Exposure->TryGetStringField(TEXT("restoreWarning"), Warning)))
    {
        TestTrue(TEXT("the warning states the session-wide consequence"),
            Warning.Contains(TEXT("session")));
    }

    // A restored capture stays quiet.
    Capture.bExposureRestored = true;
    const TSharedPtr<FJsonObject> Clean = PinWrightRenderCapture::MakeExposureInfoObject(Capture);
    TestFalse(TEXT("a restored capture carries no restoreWarning"),
        Clean->HasField(TEXT("restoreWarning")));
    return true;
}

// ============================================================================
// 2b. The units contract: a gain is not an EV100, and the response says which.
// ============================================================================

// WHAT THIS DEFENDS, AND THE FAILURE THAT PRODUCED IT. The renderer carries exposure as a linear
// GAIN; `ev100` is a log stop scale running the OTHER WAY. Both are plain positive-ish numbers, so
// a caller who feeds one where the other belongs gets a valid request, a successful pin, and a
// black frame -- with every field in the response agreeing that it worked. Measured 2026-08-19 on
// /Engine/BasicShapes/Cube: an auto capture resolved at gain 6.1011, i.e. EV100 -2.6091. Pinning at
// -2.6091 reproduced the auto frame to a decoded meanAbsDiff of 1.319 (inside the 1.456 auto-to-auto
// noise). Pinning at 6.1011 -- the gain, passed straight back as an EV100, which is the obvious
// reading of a field named `adapted` -- gave 152.882, a frame at meanLuminance 0.0117 against the
// auto shot's 0.6076.
//
// So the assertions here are about DIRECTION and MAGNITUDE of the conversion, not just that some
// conversion exists: a sign error or a dropped LuminanceMax passes any self-consistency check.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCaptureExposureGainEv100RoundTripTest,
    "PinWright.render.exposure_pin.ExposureGainAndEv100RoundTrip",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCaptureExposureGainEv100RoundTripTest::RunTest(const FString& Parameters)
{
    using PinWrightRenderCapture::Ev100ToExposureGain;
    using PinWrightRenderCapture::ExposureGainToEv100;
    using PinWrightRenderCapture::ExposureLuminanceMax;

    const float LuminanceMax = ExposureLuminanceMax();
    TestTrue(FString::Printf(TEXT("LuminanceMax is positive and finite (%g)"), LuminanceMax),
        LuminanceMax > 0.0f && FMath::IsFinite(LuminanceMax));

    // The two anchors that catch a dropped or inverted LuminanceMax. At EV100 0 the engine's
    // EV100ToLuminance is exactly LuminanceMax, so the gain is its reciprocal; one stop up halves
    // the gain because a HIGHER EV100 is a DARKER image.
    TestTrue(TEXT("gain at EV100 0 is 1/LuminanceMax"),
        FMath::IsNearlyEqual(Ev100ToExposureGain(0.0f), 1.0f / LuminanceMax, 1.0e-5f));
    TestTrue(TEXT("one stop of EV100 halves the gain"),
        FMath::IsNearlyEqual(Ev100ToExposureGain(1.0f), Ev100ToExposureGain(0.0f) * 0.5f, 1.0e-5f));

    // THE AXIS REVERSAL, stated as its own assertion because it is the half a round-trip test
    // cannot see: gain and EV100 are both self-consistent under a sign flip.
    TestTrue(TEXT("a HIGHER EV100 is a SMALLER gain (a darker frame)"),
        Ev100ToExposureGain(5.0f) < Ev100ToExposureGain(-5.0f));
    TestTrue(TEXT("a LARGER gain is a LOWER EV100"),
        ExposureGainToEv100(16.0f) < ExposureGainToEv100(1.0f));

    // Exact round trip across the whole accepted range, both directions.
    for (float Ev100 = PinWrightRenderCapture::MinExposureEv100;
         Ev100 <= PinWrightRenderCapture::MaxExposureEv100;
         Ev100 += 2.5f)
    {
        const float Gain = Ev100ToExposureGain(Ev100);
        TestTrue(FString::Printf(TEXT("EV100 %.2f -> gain %g -> EV100 round trips"), Ev100, Gain),
            FMath::IsNearlyEqual(ExposureGainToEv100(Gain), Ev100, 1.0e-3f));
    }

    // The field measurement above, asserted as a number. This is what makes the conversion a
    // statement about THIS renderer rather than about the identity 2^-x: a build whose
    // LuminanceMax stopped being 1.0 would round-trip perfectly and still fail here.
    TestTrue(FString::Printf(
        TEXT("the measured gain 6.1011 converts to EV100 -2.6091 (got %.4f)"),
        ExposureGainToEv100(6.1011f)),
        FMath::IsNearlyEqual(ExposureGainToEv100(6.1011f), -2.6091f, 0.01f));

    // A gain that was never measured must not come back as "EV100 0", which is a perfectly
    // ordinary exposure. Callers gate on `adaptedMeasured`; this only pins the encodable default.
    TestEqual(TEXT("a non-positive gain converts to 0 rather than -inf/NaN"),
        ExposureGainToEv100(0.0f), 0.0f);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCaptureExposureAdaptedReportsEv100EquivalentTest,
    "PinWright.render.exposure_pin.AdaptedIsReportedWithItsEv100Equivalent",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCaptureExposureAdaptedReportsEv100EquivalentTest::RunTest(const FString& Parameters)
{
    // An UNPINNED capture: the gain is the renderer's lagging readback, and the EV100 beside it is
    // the number the caller pins the next N shots at.
    FViewportCaptureOutput Auto =
        ExposurePinMakeCaptureOutput(/*bRequested=*/false, /*bPinned=*/false, TEXT(""));
    Auto.ExposureMode = EExposureRequestMode::Auto;
    Auto.AdaptedExposure = 6.1011f;
    Auto.bAdaptedExposureMeasured = true;
    Auto.bAdaptedExposureFromPin = false;

    const TSharedPtr<FJsonObject> AutoExposure = PinWrightRenderCapture::MakeExposureInfoObject(Auto);
    double Adapted = 0.0;
    double Ev100Equivalent = 0.0;
    FString AdaptedSource;
    if (TestTrue(TEXT("an auto capture publishes the gain, its EV100 and the source"),
            AutoExposure->TryGetNumberField(TEXT("adapted"), Adapted) &&
            AutoExposure->TryGetNumberField(TEXT("ev100Equivalent"), Ev100Equivalent) &&
            AutoExposure->TryGetStringField(TEXT("adaptedSource"), AdaptedSource)))
    {
        TestEqual(TEXT("`adapted` is the gain, unchanged"), Adapted, 6.1011, 1.0e-4);
        // The assertion that would have caught the defect: the two fields are DIFFERENT numbers.
        // Shipping only `adapted` made the response look like it already carried a reusable EV100.
        TestTrue(FString::Printf(
            TEXT("`ev100Equivalent` (%.4f) is not the gain (%.4f) -- different units"),
            Ev100Equivalent, Adapted),
            FMath::Abs(Ev100Equivalent - Adapted) > 1.0);
        TestEqual(TEXT("`ev100Equivalent` is the EV100 that reproduces that gain"),
            Ev100Equivalent, -2.6091, 0.01);
        TestEqual(TEXT("an auto capture's gain came from the lagging readback"),
            AdaptedSource, FString(TEXT("readback")));
    }

    // A PINNED capture: the readback is stale (it freezes when the EyeAdaptation show flag is
    // cleared), so the gain is derived from the EV100 in force instead -- and `ev100Equivalent`
    // must therefore come back EQUAL to `ev100`. A response where those two disagree is reporting
    // an exposure other than the one it pinned.
    FViewportCaptureOutput Pinned =
        ExposurePinMakeCaptureOutput(/*bRequested=*/true, /*bPinned=*/true, TEXT(""));
    Pinned.AdaptedExposure = PinWrightRenderCapture::Ev100ToExposureGain(Pinned.Ev100Applied);
    Pinned.bAdaptedExposureMeasured = true;
    Pinned.bAdaptedExposureFromPin = true;

    const TSharedPtr<FJsonObject> PinnedExposure =
        PinWrightRenderCapture::MakeExposureInfoObject(Pinned);
    double PinnedEv100 = 0.0;
    double PinnedEquivalent = 0.0;
    FString PinnedSource;
    if (TestTrue(TEXT("a pinned capture publishes ev100, ev100Equivalent and the source"),
            PinnedExposure->TryGetNumberField(TEXT("ev100"), PinnedEv100) &&
            PinnedExposure->TryGetNumberField(TEXT("ev100Equivalent"), PinnedEquivalent) &&
            PinnedExposure->TryGetStringField(TEXT("adaptedSource"), PinnedSource)))
    {
        TestEqual(TEXT("a pinned capture's ev100Equivalent is the EV100 it pinned at"),
            PinnedEquivalent, PinnedEv100, 1.0e-3);
        TestEqual(TEXT("a pinned capture's gain is derived, not read back"),
            PinnedSource, FString(TEXT("fixedPin")));
    }

    // And with nothing measured, neither number is published -- a 0 gain would convert to EV100 0,
    // which is an ordinary exposure and would read as a measurement.
    FViewportCaptureOutput Unmeasured =
        ExposurePinMakeCaptureOutput(/*bRequested=*/false, /*bPinned=*/false, TEXT(""));
    const TSharedPtr<FJsonObject> UnmeasuredExposure =
        PinWrightRenderCapture::MakeExposureInfoObject(Unmeasured);
    TestFalse(TEXT("no gain measured -> no `adapted`"),
        UnmeasuredExposure->HasField(TEXT("adapted")));
    TestFalse(TEXT("no gain measured -> no `ev100Equivalent`"),
        UnmeasuredExposure->HasField(TEXT("ev100Equivalent")));
    TestFalse(TEXT("no gain measured -> no `adaptedSource`"),
        UnmeasuredExposure->HasField(TEXT("adaptedSource")));
    // The readback note names one specific state, so a viewport that has a completed readback is
    // silent about it.
    TestFalse(TEXT("a completed readback publishes no adaptedReadbackPending"),
        UnmeasuredExposure->HasField(TEXT("adaptedReadbackPending")));

    // THE PENDING READBACK IS NOT A WARM-UP SIGNAL, and the exposure block no longer carries one.
    // `bAdaptedExposureReadbackPending` is true on every pinned capture -- clearing the
    // EyeAdaptation show flag stops the pass that refreshes the readback -- and it is true on
    // fully warmed frames (2 of 2 trials, 2026-08-19). It was published as a `warmupWarning` on
    // that basis and told callers to discard good frames. It is now the narrow fact it always was;
    // the warm-up verdict is measured on the pixels and reported at `viewport.warmup` (asserted
    // below). Do not move a warm-up warning back onto the exposure block.
    FViewportCaptureOutput Cold =
        ExposurePinMakeCaptureOutput(/*bRequested=*/true, /*bPinned=*/true, TEXT(""));
    Cold.AdaptedExposure = PinWrightRenderCapture::Ev100ToExposureGain(Cold.Ev100Applied);
    Cold.bAdaptedExposureMeasured = true;
    Cold.bAdaptedExposureFromPin = true;
    Cold.bAdaptedExposureReadbackPending = true;

    const TSharedPtr<FJsonObject> ColdExposure = PinWrightRenderCapture::MakeExposureInfoObject(Cold);
    FString ReadbackPending;
    if (TestTrue(TEXT("a capture with no completed readback carries adaptedReadbackPending"),
            ColdExposure->TryGetStringField(TEXT("adaptedReadbackPending"), ReadbackPending)))
    {
        // It has to hand the reader the signal that DOES answer "are these pixels finished", or it
        // gets read as the warm-up verdict it used to be mistaken for.
        TestTrue(TEXT("the note points at the measured warm-up signal"),
            ReadbackPending.Contains(TEXT("warmup.settled")));
    }
    TestFalse(TEXT("and it is not dressed up as a warm-up warning"),
        ColdExposure->HasField(TEXT("warmupWarning")));
    TestTrue(TEXT("the pinned gain is still reported alongside it"),
        ColdExposure->HasField(TEXT("adapted")) && ColdExposure->HasField(TEXT("ev100Equivalent")));

    // The warm-up verdict, on `viewport.warmup` where it is measured: the capture pumps extra
    // draws until the frame mean stops moving, and an unsettled frame is one that was still moving
    // when the budget ran out. A capture taken there is measurably darker than the identical
    // capture repeated (0.0506 vs 0.2437 meanLuminance at the same ev100 0, measured 2026-08-19
    // across an asset-editor close/reopen). It has to survive the PINNED branch -- the case a
    // caller most needs it in, because pinning is what stops auto-exposure from papering over the
    // dark frame.
    FViewportCaptureOutput Unsettled =
        ExposurePinMakeCaptureOutput(/*bRequested=*/true, /*bPinned=*/true, TEXT(""));
    Unsettled.bWarmupSettled = false;
    Unsettled.WarmupSettleRounds = 8;
    Unsettled.WarmupMeanLuminanceDelta = 0.012345;
    Unsettled.WarmupSettleMs = 640.0;

    const TSharedPtr<FJsonObject> UnsettledWarmup = ExposurePinGetWarmupBlock(
        PinWrightRenderCapture::MakeViewportInfoObject(Unsettled));
    if (TestTrue(TEXT("every capture carries a warmup block"), UnsettledWarmup.IsValid()))
    {
        bool bSettled = true;
        TestTrue(TEXT("`settled` is present"),
            UnsettledWarmup->TryGetBoolField(TEXT("settled"), bSettled));
        TestFalse(TEXT("a frame still moving when the budget ran out reports settled=false"),
            bSettled);
        FString WarmupWarning;
        if (TestTrue(TEXT("an unsettled frame carries warmupWarning"),
                UnsettledWarmup->TryGetStringField(TEXT("warmupWarning"), WarmupWarning)))
        {
            // Never a bare "warning": it has to say what to do, or it is noise that gets filtered
            // out -- and it has to carry the measurement rather than a fixed sentence, or it
            // cannot be told apart from a suspicion.
            TestTrue(TEXT("the warning names the remedy"),
                WarmupWarning.Contains(TEXT("Re-shoot")));
            TestTrue(TEXT("the warning carries the measured luminance delta"),
                WarmupWarning.Contains(TEXT("0.012345")));
        }
    }

    // The other half: a settled frame stays quiet, or the warning becomes noise and stops being
    // read. This is the assertion the retired exposure-block `warmupWarning` check could no longer
    // make -- that field is gone, so testing for its absence there proved nothing.
    FViewportCaptureOutput Settled =
        ExposurePinMakeCaptureOutput(/*bRequested=*/true, /*bPinned=*/true, TEXT(""));
    Settled.bWarmupSettled = true;
    Settled.WarmupSettleRounds = 2;
    Settled.WarmupMeanLuminanceDelta = 0.0;
    Settled.WarmupSettleMs = 90.0;

    const TSharedPtr<FJsonObject> SettledWarmup = ExposurePinGetWarmupBlock(
        PinWrightRenderCapture::MakeViewportInfoObject(Settled));
    if (TestTrue(TEXT("a settled capture carries a warmup block too"), SettledWarmup.IsValid()))
    {
        bool bSettled = false;
        SettledWarmup->TryGetBoolField(TEXT("settled"), bSettled);
        TestTrue(TEXT("a settled frame reports settled=true"), bSettled);
        TestFalse(TEXT("a settled frame carries no warmupWarning"),
            SettledWarmup->HasField(TEXT("warmupWarning")));
    }
    return true;
}

// ============================================================================
// 3. Live: the omitted parameter leaves the editor exactly as it found it.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCaptureOpenLevelOmittedExposureUnchangedTest,
    "PinWright.render.capture_open_level.OmittedExposureLeavesViewportUntouched",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCaptureOpenLevelOmittedExposureUnchangedTest::RunTest(const FString& Parameters)
{
    FEditorViewportClient* Client = ExposurePinActiveLevelViewportClient();
    const FExposureSettings Before = Client ? Client->ExposureSettings : FExposureSettings();

    FTestResponseCapture Capture;
    TestTrue(TEXT("render.capture_open_level handler found"),
        InvokeHandlerWithCapture(TEXT("render.capture_open_level"),
            ExposurePinMakeLevelPayload(), Capture));
    ON_SCOPE_EXIT
    {
        ExposurePinDeleteCaptureFile(Capture);
    };

    // Holds on a headless host too, and it is the back-compatibility guarantee itself: a capture
    // that names no exposure must not read, write or disturb the viewport's exposure state.
    if (Client)
    {
        TestEqual(TEXT("bFixed is untouched by a capture that named no exposure"),
            Client->ExposureSettings.bFixed, Before.bFixed);
        TestEqual(TEXT("FixedEV100 is untouched by a capture that named no exposure"),
            Client->ExposureSettings.FixedEV100, Before.FixedEV100);
    }

    if (!Capture.bSuccess)
    {
        TestTrue(FString::Printf(TEXT("failure is typed, not a crash: %s"), *Capture.ErrorCode),
            ExposurePinIsTypedCaptureFailure(Capture.ErrorCode));
        // The untouched-viewport half above did run; the response-shape half below did not.
        PinWrightTestSkip::SkipAssertions(*this, TEXT("capture-unavailable"),
            FString::Printf(TEXT("the capture returned %s, so the viewport.exposure response ")
                TEXT("assertions were skipped"), *Capture.ErrorCode));
        return true;
    }

    const TSharedPtr<FJsonObject> Exposure = ExposurePinGetExposureBlock(Capture);
    if (!TestTrue(TEXT("a successful capture reports viewport.exposure"), Exposure.IsValid()))
    {
        return true;
    }
    FString Mode;
    TestTrue(TEXT("the block names the mode"), Exposure->TryGetStringField(TEXT("mode"), Mode));
    TestEqual(TEXT("an omitted parameter reports 'unset'"), Mode, FString(TEXT("unset")));
    bool bPinRequested = true;
    Exposure->TryGetBoolField(TEXT("pinRequested"), bPinRequested);
    TestFalse(TEXT("no pin was requested"), bPinRequested);
    bool bPinned = true;
    Exposure->TryGetBoolField(TEXT("pinned"), bPinned);
    TestFalse(TEXT("and none was applied"), bPinned);
    bool bRestored = false;
    TestTrue(TEXT("the restore verdict is reported"),
        Exposure->TryGetBoolField(TEXT("restored"), bRestored));
    TestTrue(TEXT("nothing was moved, so it reports restored"), bRestored);
    return true;
}

// ============================================================================
// 4. Live: a pin is applied, reported, and put back.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCaptureOpenLevelPinnedExposureIsRestoredTest,
    "PinWright.render.capture_open_level.PinnedExposureIsRestored",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCaptureOpenLevelPinnedExposureIsRestoredTest::RunTest(const FString& Parameters)
{
    FEditorViewportClient* Client = ExposurePinActiveLevelViewportClient();
    const FExposureSettings Before = Client ? Client->ExposureSettings : FExposureSettings();
    const bool bEyeAdaptationBefore = Client ? (Client->EngineShowFlags.EyeAdaptation != 0) : true;

    TSharedPtr<FJsonObject> Payload = ExposurePinMakeLevelPayload();
    TSharedPtr<FJsonObject> Inner = MakeShared<FJsonObject>();
    Inner->SetStringField(TEXT("mode"), TEXT("fixed"));
    Inner->SetNumberField(TEXT("ev100"), 11.0);
    Payload->SetObjectField(TEXT("exposure"), Inner);

    FTestResponseCapture Capture;
    TestTrue(TEXT("render.capture_open_level handler found"),
        InvokeHandlerWithCapture(TEXT("render.capture_open_level"), Payload, Capture));
    ON_SCOPE_EXIT
    {
        ExposurePinDeleteCaptureFile(Capture);
    };

    // The decisive assertion, and it holds whether the capture succeeded, failed, or never
    // reached an RHI: a capture verb must not leave the editor's exposure mutated. Measured on
    // the editor, not read back out of the response.
    if (Client)
    {
        TestEqual(TEXT("bFixed is back where the call found it"),
            Client->ExposureSettings.bFixed, Before.bFixed);
        TestEqual(TEXT("FixedEV100 is back where the call found it"),
            Client->ExposureSettings.FixedEV100, Before.FixedEV100);
        // The pin clears the EyeAdaptation show flag to remove the temporal blend; that is part
        // of what has to be undone, and forgetting it would silently flatten every later capture.
        TestEqual(TEXT("the EyeAdaptation show flag is back where the call found it"),
            Client->EngineShowFlags.EyeAdaptation != 0, bEyeAdaptationBefore);
    }

    if (!Capture.bSuccess)
    {
        TestTrue(FString::Printf(TEXT("failure is typed, not a crash: %s"), *Capture.ErrorCode),
            ExposurePinIsTypedCaptureFailure(Capture.ErrorCode));
        // The restore half above did run; the pinned/pinWarning pairing below did not.
        PinWrightTestSkip::SkipAssertions(*this, TEXT("capture-unavailable"),
            FString::Printf(TEXT("the capture returned %s, so the pinned/pinWarning pairing ")
                TEXT("assertions were skipped"), *Capture.ErrorCode));
        return true;
    }

    const TSharedPtr<FJsonObject> Exposure = ExposurePinGetExposureBlock(Capture);
    if (!TestTrue(TEXT("a successful capture reports viewport.exposure"), Exposure.IsValid()))
    {
        return true;
    }
    FString Mode;
    Exposure->TryGetStringField(TEXT("mode"), Mode);
    TestEqual(TEXT("the block echoes the requested mode"), Mode, FString(TEXT("fixed")));
    bool bPinRequested = false;
    Exposure->TryGetBoolField(TEXT("pinRequested"), bPinRequested);
    TestTrue(TEXT("the request is recorded"), bPinRequested);
    double Ev100Requested = 0.0;
    TestTrue(TEXT("the requested EV100 is echoed"),
        Exposure->TryGetNumberField(TEXT("ev100Requested"), Ev100Requested));
    TestEqual(TEXT("the echoed EV100 is the one that was asked for"), Ev100Requested, 11.0);
    bool bRestored = false;
    Exposure->TryGetBoolField(TEXT("restored"), bRestored);
    TestTrue(TEXT("the response agrees the viewport was restored"), bRestored);

    // pinned is a MEASUREMENT, so it is allowed to be false here (a viewport left in a debug view
    // mode by earlier work does not apply the override). What is not allowed is being false and
    // silent -- assert the pairing rather than the value.
    bool bPinned = false;
    Exposure->TryGetBoolField(TEXT("pinned"), bPinned);
    if (bPinned)
    {
        double Ev100 = 0.0;
        TestTrue(TEXT("a governed pin publishes the EV100 in force"),
            Exposure->TryGetNumberField(TEXT("ev100"), Ev100));
        TestEqual(TEXT("the EV100 in force is the requested one"), Ev100, 11.0);
        TestFalse(TEXT("a governed pin carries no pinWarning"),
            Exposure->HasField(TEXT("pinWarning")));
    }
    else
    {
        TestTrue(TEXT("an ungoverned pin is never silent"),
            Exposure->HasField(TEXT("pinWarning")));
    }
    return true;
}

// ============================================================================
// 5. Live: the pin reproduces within the noise floor, and it is not inert.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCaptureAssetPreviewPinnedCapturesReproduceTest,
    "PinWright.render.capture_asset_preview.PinnedCapturesReproduceWithinTolerance",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCaptureAssetPreviewPinnedCapturesReproduceTest::RunTest(const FString& Parameters)
{
    // CALIBRATION. Every number below was measured on this exact fixture -- an FAdvancedPreviewScene
    // holding /Engine/BasicShapes/Cube, captured at 128x128 -- on 2026-08-18. Re-measure them if
    // the fixture changes; do not adjust them to make a run pass.
    //
    //   ev100:          -1        0        5        10       16
    //   meanLuminance:  0.3644   0.2466   0.0162   0.0068   0.0068
    //
    // The fixture's working range ends well before ev100 10. At 10 and 16 the frame crushes to
    // black and the only surviving pixels are exposure-independent post-tonemap editor overlays,
    // which are byte-identical -- so the original 10-vs-16 pair could not tell an inert pin from a
    // working one, and the same-pin half passed vacuously by comparing two black frames. -1 and 5
    // are both inside the range, and -1 vs 5 moves 16276/16384 px at a best-fit gain of ~4.0-5.1.
    //
    // TOLERANCE, and why identity is the wrong claim. Two back-to-back captures at the SAME ev100
    // of -1 differ in 11676/16384 px (best-fit gain 1.0000 -- so the exposure did not move; this is
    // temporal jitter in the renderer, not an exposure drift). Pinning exposure does not make a
    // viewport capture byte-stable, and asserting byte identity here only ever passed because both
    // frames were black.
    //
    // THE FIXTURE IS NOT BIMODAL, AND THE COMMANDLET IS NOT DARK. This block used to carry a
    // three-column "interactive / commandlet(lit) / commandlet(black)" table claiming the headless
    // suite rendered this scene at meanLuminance 0.0978 or 0.0078 with a signal of 23.127 or 0.000.
    // EVERY commandlet column was an artifact: all three captures were written to ONE file, because
    // the handler's auto-generated filename carried a one-second timestamp and a capture takes
    // ~60 ms. The numbers were a file compared with itself (hence the tell-tale `0.000 over
    // 0/16384 px` in every historical run) and the surviving reading was whichever shot won the
    // overwrite race. Fixed in PinWrightScreenshotUtils::MakeScreenshotFilename; the three shots
    // below now also name their own files, and the path-distinctness assertion further down is
    // what stops this from ever being measured that way again.
    //
    // Measured 2026-08-21 in the automation commandlet (`UnrealEditor-Cmd -unattended
    // -RenderOffscreen`, exactly how the suite runs), against the interactive column above:
    //
    //                            interactive       commandlet
    //   meanLuminance @ ev100 -1      0.3644           0.3644
    //   noise  (same ev100 twice)     0.88             0.844
    //   signal (ev100 -1 vs 5)       89.6             89.607
    //   px differing on the signal   16276/16384      16276/16384
    //
    // The two environments agree to three decimal places once the window is warmed. There is no
    // dim mode and no black mode to survive. (Taken WITHOUT the warm-up shot below, the commandlet
    // reads 0.3640 / 1.019 / 89.488 -- still the same fixture, just measured across the ambient's
    // arrival, which is the whole reason the warm-up shot exists.)
    //
    // So the band the constants have to separate is the real one: noise 0.844 against signal
    // 89.607, ~106x. 4.0 sits 4.7x above the measured noise and 22x below the measured signal.
    constexpr double ToleranceMeanAbsDiff = 4.0;
    // "Far above the tolerance", stated as a number rather than left to the reader. 2x the
    // tolerance leaves the measured signal 11x clear of the bar. It is deliberately NOT raised to
    // sit just under the signal: a bar set flush against one fixture's magnitude is what the
    // discarded 4x (32.0) attempt did, and the value of this half is that it is unmistakably
    // cleared by a real six-stop move, not that it is tight.
    constexpr double MinCrossEvMeanAbsDiff = ToleranceMeanAbsDiff * 2.0;
    // The lit-frame gate. It exists so the same-ev100 assertion can never again pass by comparing
    // two black frames -- a black frame does not respond to exposure, so both halves would be
    // vacuous. It is NOT expected to trip: the commandlet reads 0.3644 here, 12x above the gate.
    // If it does trip, something really has stopped rendering, and the skip marker says so. The
    // handler's own blank detector trips at 0.01, too low to serve as this gate.
    constexpr double MinLitMeanLuminance = 0.03;
    // The pair is shot at the brighter end because that is where the noise floor was measured.
    constexpr double LitEv100 = -1.0;
    constexpr double DarkerEv100 = 5.0;

    // The asset-preview viewport is the fixture of choice for a pixel-reproducibility assertion:
    // it is an isolated preview scene holding one static mesh, with no wind, no animation and no
    // streaming, so two identical requests differ only if the RENDER differed. It is also the
    // surface `lighting.set_exposure` could never pin at all -- that verb writes a
    // PostProcessVolume into the editor LEVEL, which a preview scene does not have.
    const auto CaptureAt = [&](double Ev100, const TCHAR* Filename, FTestResponseCapture& Out)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("assetPath"), TEXT("/Engine/BasicShapes/Cube.Cube"));
        // A DISTINCT filename per shot, named rather than generated. Left to the handler's
        // auto-name this test compared one file with itself: the generated name carried a
        // one-second timestamp, a capture takes ~60 ms, and all three shots landed on one path
        // (fixed in PinWrightScreenshotUtils::MakeScreenshotFilename). Naming them here means the
        // comparison no longer depends on that policy at all, and the path assertion below makes
        // a regression in it visible rather than silent.
        Payload->SetStringField(TEXT("filename"), Filename);
        Payload->SetNumberField(TEXT("width"), 128);
        Payload->SetNumberField(TEXT("height"), 128);
        TSharedPtr<FJsonObject> Inner = MakeShared<FJsonObject>();
        Inner->SetStringField(TEXT("mode"), TEXT("fixed"));
        Inner->SetNumberField(TEXT("ev100"), Ev100);
        Payload->SetObjectField(TEXT("exposure"), Inner);
        // Explicit, because the noise floor above was measured on shots into ONE preview viewport
        // and closeAfterCapture now defaults to true. Letting the default close the editor between
        // shots would make every capture here a first capture into a fresh viewport -- a different
        // fixture from the one the constants were calibrated against, and silently so.
        Payload->SetBoolField(TEXT("closeAfterCapture"), false);
        return InvokeHandlerWithCapture(TEXT("render.capture_asset_preview"), Payload, Out);
    };

    FTestResponseCapture Warmup;
    FTestResponseCapture First;
    FTestResponseCapture Second;
    FTestResponseCapture Darker;
    ON_SCOPE_EXIT
    {
        ExposurePinDeleteCaptureFile(Warmup);
        ExposurePinDeleteCaptureFile(First);
        ExposurePinDeleteCaptureFile(Second);
        ExposurePinDeleteCaptureFile(Darker);
    };

    // A THROWAWAY FIRST SHOT, and it is not optional. The capture that OPENS a preview window is
    // about a stop dark - the scene's ambient has not arrived yet (measured 2026-08-21 at identical
    // camera and identical ev100: mean 0.1987 then 0.3686 on SM_Driftwood, 0.2491 then 0.3655 on
    // SM_Amphora; documented in docs/wiki-src/render.md under "The first capture into a fresh
    // preview window is a stop dark"). Comparing a window-opening shot against a warm one is
    // comparing two different fixtures, and it would blow through the tolerance below on a
    // difference that is real product behaviour rather than a reproducibility failure.
    //
    // Until now this test got away with it only because the sibling test above happens to run
    // first, uses the same asset, and leaves its window open via closeAfterCapture:false - an
    // implicit fixture supplied by test ordering, which nothing enforces and a filtered run can
    // reorder. Warm the window here instead, and judge only the three shots that follow. The same
    // discipline the sibling test already applies explicitly.
    if (!TestTrue(TEXT("render.capture_asset_preview handler found"),
            CaptureAt(LitEv100, TEXT("pw_exposurepin_warm.png"), Warmup)))
    {
        return true;
    }
    CaptureAt(LitEv100, TEXT("pw_exposurepin_lit_a.png"), First);
    if (!First.bSuccess)
    {
        TestTrue(FString::Printf(TEXT("failure is typed, not a crash: %s"), *First.ErrorCode),
            ExposurePinIsTypedCaptureFailure(First.ErrorCode));
        PinWrightTestSkip::SkipAssertions(*this, TEXT("capture-unavailable"),
            FString::Printf(TEXT("the first capture returned %s, so no pixels exist to compare ")
                TEXT("and BOTH pixel assertions were skipped"), *First.ErrorCode));
        return true;
    }

    const TSharedPtr<FJsonObject> FirstExposure = ExposurePinGetExposureBlock(First);
    bool bPinned = false;
    if (FirstExposure.IsValid())
    {
        FirstExposure->TryGetBoolField(TEXT("pinned"), bPinned);
    }
    if (!bPinned)
    {
        // The response itself says these pixels were not pinned, so a reproducibility assertion
        // would be asserting something the verb never claimed. Skipping here is the same
        // discipline the RHI guard applies: assert the honest report, not the pixels it
        // disclaimed.
        TestTrue(TEXT("an unpinned preview capture explains itself"),
            FirstExposure.IsValid() && FirstExposure->HasField(TEXT("pinWarning")));
        PinWrightTestSkip::SkipAssertions(*this, TEXT("pin-not-applied"),
            TEXT("the response reports pinned=false, so a reproducibility assertion would be "
                "asserting something the verb never claimed; both pixel assertions were skipped"));
        return true;
    }

    CaptureAt(LitEv100, TEXT("pw_exposurepin_lit_b.png"), Second);
    CaptureAt(DarkerEv100, TEXT("pw_exposurepin_darker.png"), Darker);
    if (!Second.bSuccess || !Darker.bSuccess)
    {
        TestTrue(TEXT("a follow-up failure is typed"),
            ExposurePinIsTypedCaptureFailure(Second.bSuccess ? Darker.ErrorCode : Second.ErrorCode));
        PinWrightTestSkip::SkipAssertions(*this, TEXT("capture-unavailable"),
            FString::Printf(TEXT("a follow-up capture returned %s, so the pair could not be ")
                TEXT("formed and both pixel assertions were skipped"),
                Second.bSuccess ? *Darker.ErrorCode : *Second.ErrorCode));
        return true;
    }

    FString FirstPath;
    FString SecondPath;
    FString DarkerPath;
    First.Result->TryGetStringField(TEXT("path"), FirstPath);
    Second.Result->TryGetStringField(TEXT("path"), SecondPath);
    Darker.Result->TryGetStringField(TEXT("path"), DarkerPath);

    // THREE FILES, NOT ONE. This is a hard failure and never a skip, because its absence is what
    // let this test report green for weeks while measuring nothing. The handler's auto-generated
    // filename used to carry a one-second timestamp and nothing else; a capture takes ~60 ms, so
    // all three shots resolved to the same path, each write overwrote the last, and every response
    // still returned success with a `path`. Comparing a file with itself yields meanAbsDiff 0.000
    // over 0/16384 px -- which is exactly what every historical run of this test printed, on both
    // its measures, and it reads as a perfect pass on the same-pin half. Assert the fixture before
    // trusting a single pixel of it.
    TestTrue(TEXT("every capture reported a path"),
        !FirstPath.IsEmpty() && !SecondPath.IsEmpty() && !DarkerPath.IsEmpty());
    TestTrue(FString::Printf(
        TEXT("the three captures wrote three DIFFERENT files, so the comparisons below are ")
        TEXT("between distinct frames (first '%s', second '%s', darker '%s')"),
        *FPaths::GetCleanFilename(FirstPath), *FPaths::GetCleanFilename(SecondPath),
        *FPaths::GetCleanFilename(DarkerPath)),
        FirstPath != SecondPath && FirstPath != DarkerPath && SecondPath != DarkerPath);

    const TArray<FColor> FirstPixels = ExposurePinLoadPixels(FirstPath);
    const TArray<FColor> SecondPixels = ExposurePinLoadPixels(SecondPath);
    const TArray<FColor> DarkerPixels = ExposurePinLoadPixels(DarkerPath);
    if (FirstPixels.Num() == 0 || SecondPixels.Num() == 0 || DarkerPixels.Num() == 0)
    {
        // Undecodable output is a "not checked", never a pass.
        PinWrightTestSkip::SkipAssertions(*this, TEXT("undecodable-png"),
            TEXT("a capture PNG could not be decoded, so both pixel assertions were skipped"));
        return true;
    }

    const double LitMeanLuminance = ExposurePinMeanLuminance(FirstPixels);
    const double SamePinDifference = ExposurePinMeanAbsDifference(FirstPixels, SecondPixels);
    const double CrossPinDifference = ExposurePinMeanAbsDifference(FirstPixels, DarkerPixels);

    // Publish all three magnitudes on every run: a recalibration starts from the numbers this
    // host actually produced, not from a bare pass/fail.
    AddInfo(FString::Printf(
        TEXT("ev100 %.1f: meanLuminance %.4f. same-pin meanAbsDiff %.3f over %d/%d px; ")
        TEXT("ev100 %.1f vs %.1f meanAbsDiff %.3f over %d/%d px. tolerance %.1f."),
        LitEv100, LitMeanLuminance,
        SamePinDifference, ExposurePinCountDifferingPixels(FirstPixels, SecondPixels),
        FirstPixels.Num(),
        LitEv100, DarkerEv100,
        CrossPinDifference, ExposurePinCountDifferingPixels(FirstPixels, DarkerPixels),
        FirstPixels.Num(),
        ToleranceMeanAbsDiff));

    // The gate that keeps everything below honest. Two BLACK frames match each other trivially and
    // a black frame does not respond to exposure at all, so both assertions can be satisfied by a
    // fixture that stopped rendering -- which is exactly how this test passed while measuring
    // nothing. Establish that the pixels are lit BEFORE comparing them.
    //
    // WHERE THE FRAME IS BLACK, THIS IS A "NOT MEASURED", NOT A FAILURE -- and the distinction is
    // environmental, not a product defect. Measured 2026-08-18: the calibration table above was
    // taken in an INTERACTIVE editor, where ev100 -1 gives meanLuminance 0.3644. The automation
    // commandlet (`UnrealEditor-Cmd -unattended`, which is how the suite runs) renders the same
    // FAdvancedPreviewScene at meanLuminance 0.0078 -- ~47x darker -- and there ev100 -1 and
    // ev100 5 come back byte-identical, 0 of 16384 px differing across six stops. Exposure has no
    // measurable effect on those pixels at ANY ev100, so no choice of literals rescues the
    // comparison: the preview scene's lighting simply is not there headless. Asserting the
    // not-inert property against that environment would be asserting something the environment
    // cannot show, so the test reports what it saw and stops -- the same discipline as the
    // !bPinned and undecodable-PNG branches above.
    //
    // What stays assertable in the dark: the verb's `blank` flag must be DERIVED from the
    // statistics it published, not echoed from the request. That is gamma-free -- it reads only
    // the response's own numbers -- and it cannot be satisfied vacuously by blackness, because a
    // black frame is exactly the case the flag exists to call.
    //
    // Do NOT compare the decoded-PNG luminance above against the response's
    // imageStats.meanLuminance: they are in different gamma spaces. ExposurePinLoadPixels calls
    // ChangeFormat(BGRA8, sRGB) on the decode, while CalculateCaptureImageStats runs on the raw
    // framebuffer FColors, so the same dark frame reads 0.0078 decoded and 0.0977 reported. That
    // is a ~12x gap with no defect behind it. The difference measures above are unaffected --
    // both of their sides go through the identical decode -- but an ABSOLUTE luminance from this
    // helper is only ever comparable with another number from this helper.
    if (LitMeanLuminance < MinLitMeanLuminance)
    {
        // THE site board ticket `B-test-skips-assertions-silently` was filed against. Taking this
        // branch used to be indistinguishable downstream from passing: three consecutive runs took
        // it at meanLuminance 0.0078 and all three reported an unqualified green. The marker below
        // is what makes it countable -- see ExposurePinSkipAssertions for why it is a warning and
        // not a failure, and why it must not travel through UE_LOG.
        PinWrightTestSkip::SkipAssertions(*this, TEXT("fixture-not-lit"), FString::Printf(
            TEXT("the preview frame is not lit (meanLuminance %.4f < %.2f decoded), so BOTH ")
            TEXT("substantive assertions were skipped: same-pin < %.1f and cross-EV > %.1f. ")
            TEXT("NOT expected on any environment: the automation commandlet renders this ")
            TEXT("fixture at 0.3644, 12x above the gate, so a reading below it means something ")
            TEXT("stopped rendering rather than that the environment is dark."),
            LitMeanLuminance, MinLitMeanLuminance,
            ToleranceMeanAbsDiff, MinCrossEvMeanAbsDiff));

        const TSharedPtr<FJsonObject>* FirstStats = nullptr;
        if (TestTrue(TEXT("the capture reports imageStats for the frame it produced"),
                First.Result.IsValid() && First.Result->TryGetObjectField(TEXT("imageStats"), FirstStats)))
        {
            double ReportedMean = -1.0;
            double ReportedLitPixels = -1.0;
            double ReportedWidth = 0.0;
            double ReportedHeight = 0.0;
            bool bReportedBlank = false;
            if (TestTrue(TEXT("imageStats carries meanLuminance and litPixelCount"),
                    (*FirstStats)->TryGetNumberField(TEXT("meanLuminance"), ReportedMean) &&
                    (*FirstStats)->TryGetNumberField(TEXT("litPixelCount"), ReportedLitPixels)) &&
                TestTrue(TEXT("the capture reports the frame size the statistics were taken over"),
                    First.Result->TryGetNumberField(TEXT("width"), ReportedWidth) &&
                    First.Result->TryGetNumberField(TEXT("height"), ReportedHeight) &&
                    ReportedWidth > 0.0 && ReportedHeight > 0.0) &&
                TestTrue(TEXT("the capture reports a blank flag"),
                    First.Result->TryGetBoolField(TEXT("blank"), bReportedBlank)))
            {
                // The CURRENT criterion, recomputed from the published numbers -- a flag that
                // disagrees with the statistics beside it is a report about something other than
                // these pixels. Mirrors PreviewViewportCaptureUtils.cpp:497-500 exactly, including
                // the per-frame floor: the lit-pixel term is min(BlankMinLitPixels, a share of the
                // frame), so at this test's 128x128 the floor is 9 pixels, not 64.
                //
                // This used to recompute the RETIRED mean+variance form (mean <= 0.01 && variance
                // <= 0.0001). Variance was dropped because it is resolution-dependent -- identical
                // content flipped `blank` between 512 and 1024 px (the measurement table above
                // BlankLitLuminanceThreshold in PreviewViewportCaptureUtils.h) -- so on a crushed
                // frame the stale form predicted blank:true against the current criterion's
                // correct false, and this branch would have failed for the wrong reason.
                const int64 FramePixels =
                    static_cast<int64>(ReportedWidth) * static_cast<int64>(ReportedHeight);
                const int64 LitPixelFloor = FMath::Min<int64>(
                    PinWrightRenderCapture::BlankMinLitPixels,
                    FMath::Max<int64>(1, static_cast<int64>(FMath::CeilToDouble(
                        PinWrightRenderCapture::BlankMinLitFraction * FramePixels))));
                const bool bExpectedBlank =
                    ReportedMean <= PinWrightRenderCapture::BlankMeanLuminance &&
                    static_cast<int64>(ReportedLitPixels) < LitPixelFloor;
                TestTrue(FString::Printf(
                    TEXT("the blank flag is derived from the published statistics ")
                    TEXT("(blank %s, meanLuminance %.4f, litPixelCount %lld, floor %lld over ")
                    TEXT("%lld pixels)"),
                    bReportedBlank ? TEXT("true") : TEXT("false"), ReportedMean,
                    static_cast<int64>(ReportedLitPixels), LitPixelFloor, FramePixels),
                    bReportedBlank == bExpectedBlank);
            }
        }
        return true;
    }

    // THE property: the same pin reproduces. Not byte-for-byte -- a viewport capture carries a
    // temporal-jitter noise floor that pinning exposure does not remove -- but far inside a
    // tolerance that a real exposure change blows straight through. Auto-exposure re-balances
    // between shots, so this is the pair that would have drifted.
    TestTrue(FString::Printf(
        TEXT("two captures pinned at the same EV100 reproduce within tolerance ")
        TEXT("(meanAbsDiff %.3f < %.1f)"), SamePinDifference, ToleranceMeanAbsDiff),
        SamePinDifference < ToleranceMeanAbsDiff);

    // ...and the half that makes the above non-vacuous. An inert pin -- one written but never
    // applied to the pixels -- would land inside the same tolerance, so reproducibility alone
    // proves nothing. Six stops of EV100 must move the image an order of magnitude past it.
    TestTrue(FString::Printf(
        TEXT("a different EV100 moves the image far past the tolerance, so the pin is not inert ")
        TEXT("(meanAbsDiff %.3f > %.1f)"), CrossPinDifference, MinCrossEvMeanAbsDiff),
        CrossPinDifference > MinCrossEvMeanAbsDiff);

    // DIRECTION, which a magnitude measure cannot see. Mean-abs-difference is symmetric: it is the
    // same number whether the higher EV100 came back darker (correct) or brighter (an inverted
    // axis somewhere in the chain), so the assertion above passes either way. EV100 is a stop
    // scale on the exposure the camera is set to, so a HIGHER value must yield a DARKER frame --
    // the opposite convention from the linear gain the renderer carries internally, and the
    // confusion between the two is what `ev100Equivalent` exists to remove.
    const double DarkerMeanLuminance = ExposurePinMeanLuminance(DarkerPixels);
    TestTrue(FString::Printf(
        TEXT("the higher EV100 is the darker frame (ev100 %.1f -> meanLuminance %.4f, ")
        TEXT("ev100 %.1f -> %.4f)"),
        LitEv100, LitMeanLuminance, DarkerEv100, DarkerMeanLuminance),
        DarkerMeanLuminance < LitMeanLuminance);
    return true;
}

// ============================================================================
// 6. Live: auto once, then pin at what it reported. The advertised flow.
// ============================================================================

// The parameter's own rationale -- auto re-balances between shots, so unpinned captures are not
// comparable -- invites exactly one workflow: shoot once on auto, read the exposure it resolved,
// pin the rest there. That flow was broken by a units mismatch (see section 2b), and it broke
// SILENTLY: the pin succeeded, `pinned` was true, and the frame was black. This is the end-to-end
// assertion that the number the response hands back is the number the parameter accepts.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCaptureAssetPreviewAutoThenPinReproducesTest,
    "PinWright.render.capture_asset_preview.PinAtReportedEv100ReproducesTheAutoFrame",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCaptureAssetPreviewAutoThenPinReproducesTest::RunTest(const FString& Parameters)
{
    // CALIBRATION, measured 2026-08-19 in an interactive editor on the same fixture section 5
    // uses (/Engine/BasicShapes/Cube at 128x128), decoded meanAbsDiff over RGB in 0-255 units:
    //
    //   auto vs auto (the noise floor, and this fixture's eye adaptation is still moving)  1.456
    //   auto vs pin at the reported ev100Equivalent (-2.6091)                              1.319
    //   auto vs pin at the reported `adapted` gain (6.1011) fed back as an ev100         152.882
    //
    // The round trip lands INSIDE the auto-to-auto noise, which is the strongest form the claim
    // can take: the pinned frame is not merely close to the auto frame, it is closer than two auto
    // frames are to each other. The third row is the defect, and it is the reason the second
    // assertion below exists -- without it a conversion that returned the gain unchanged would
    // still pass the first.
    constexpr double ToleranceMeanAbsDiff = 4.0;
    constexpr double MinWrongUnitsMeanAbsDiff = 8.0;
    // Below this the fixture is not lit (the automation commandlet renders this preview scene
    // near-black; see the long note in section 5) and no exposure assertion can measure anything.
    constexpr double MinLitMeanLuminance = 0.03;
    // The gain and its EV100 must be far enough apart that "pinned at the wrong one" is visible.
    // Two stops is ~4x in linear light, well past the noise floor above.
    constexpr double MinUnitSeparationStops = 2.0;

    const auto CaptureWithExposure = [&](const TSharedPtr<FJsonObject>& ExposureArg,
        const TCHAR* Filename, FTestResponseCapture& Out)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("assetPath"), TEXT("/Engine/BasicShapes/Cube.Cube"));
        // Named, not auto-generated -- the same reason as the sibling test above: four shots into
        // one auto-generated name resolved to one file, and this test's "auto vs pin meanAbsDiff
        // 0.000" readings were a file compared with itself.
        Payload->SetStringField(TEXT("filename"), Filename);
        Payload->SetNumberField(TEXT("width"), 128);
        Payload->SetNumberField(TEXT("height"), 128);
        Payload->SetBoolField(TEXT("allowBlank"), true);
        if (ExposureArg.IsValid())
        {
            Payload->SetObjectField(TEXT("exposure"), ExposureArg);
        }
        // Explicit: this test's whole method is a warm-up shot followed by a reference shot into
        // the SAME preview viewport, and closeAfterCapture now defaults to true. Leaving it to the
        // default would close the editor between the two and make the warm-up shot warm nothing.
        Payload->SetBoolField(TEXT("closeAfterCapture"), false);
        return InvokeHandlerWithCapture(TEXT("render.capture_asset_preview"), Payload, Out);
    };
    const auto AutoExposureArg = []()
    {
        TSharedPtr<FJsonObject> Arg = MakeShared<FJsonObject>();
        Arg->SetStringField(TEXT("mode"), TEXT("auto"));
        return Arg;
    };
    const auto FixedExposureArg = [](double Ev100)
    {
        TSharedPtr<FJsonObject> Arg = MakeShared<FJsonObject>();
        Arg->SetStringField(TEXT("mode"), TEXT("fixed"));
        Arg->SetNumberField(TEXT("ev100"), Ev100);
        return Arg;
    };

    // Two auto shots, and the SECOND is the reference. `adapted` is a GPU->CPU readback that lags
    // the drawn frame, and this fixture's eye adaptation is still converging on the first capture
    // into a freshly opened preview (measured: gain 11.61 then 6.12 then 6.10, meanLuminance 0.308
    // then 0.609 then 0.608). Reading the exposure off a frame that was still moving would test
    // the adaptation's settling time, not the conversion.
    FTestResponseCapture Warmup;
    FTestResponseCapture Reference;
    FTestResponseCapture PinnedRight;
    FTestResponseCapture PinnedWrong;
    ON_SCOPE_EXIT
    {
        ExposurePinDeleteCaptureFile(Warmup);
        ExposurePinDeleteCaptureFile(Reference);
        ExposurePinDeleteCaptureFile(PinnedRight);
        ExposurePinDeleteCaptureFile(PinnedWrong);
    };

    if (!TestTrue(TEXT("render.capture_asset_preview handler found"),
            CaptureWithExposure(AutoExposureArg(), TEXT("pw_exposurepin_warmup.png"), Warmup)))
    {
        return true;
    }
    if (!Warmup.bSuccess)
    {
        TestTrue(FString::Printf(TEXT("failure is typed, not a crash: %s"), *Warmup.ErrorCode),
            ExposurePinIsTypedCaptureFailure(Warmup.ErrorCode));
        PinWrightTestSkip::SkipAssertions(*this, TEXT("capture-unavailable"),
            FString::Printf(TEXT("the warm-up capture returned %s, so the auto-then-pin round ")
                TEXT("trip was not exercised"), *Warmup.ErrorCode));
        return true;
    }
    CaptureWithExposure(AutoExposureArg(), TEXT("pw_exposurepin_reference.png"), Reference);
    if (!Reference.bSuccess)
    {
        TestTrue(TEXT("a follow-up failure is typed"),
            ExposurePinIsTypedCaptureFailure(Reference.ErrorCode));
        PinWrightTestSkip::SkipAssertions(*this, TEXT("capture-unavailable"),
            FString::Printf(TEXT("the reference capture returned %s, so the auto-then-pin round ")
                TEXT("trip was not exercised"), *Reference.ErrorCode));
        return true;
    }

    const TSharedPtr<FJsonObject> ReferenceExposure = ExposurePinGetExposureBlock(Reference);
    bool bAdaptedMeasured = false;
    double AdaptedGain = 0.0;
    double Ev100Equivalent = 0.0;
    if (!ReferenceExposure.IsValid() ||
        !ReferenceExposure->TryGetBoolField(TEXT("adaptedMeasured"), bAdaptedMeasured) ||
        !bAdaptedMeasured)
    {
        // No completed eye-adaptation readback on this host, so there is nothing to feed back.
        // A "not measured", never a pass by omission.
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-adapted-readback"),
            TEXT("the auto capture reported adaptedMeasured=false, so there was no reported "
                "EV100 to pin at and the auto-then-pin round trip was not exercised"));
        return true;
    }
    if (!TestTrue(TEXT("an auto capture publishes both the gain and its EV100 equivalent"),
            ReferenceExposure->TryGetNumberField(TEXT("adapted"), AdaptedGain) &&
            ReferenceExposure->TryGetNumberField(TEXT("ev100Equivalent"), Ev100Equivalent)))
    {
        return true;
    }
    AddInfo(FString::Printf(TEXT("auto resolved to gain %.4f = ev100Equivalent %.4f."),
        AdaptedGain, Ev100Equivalent));

    // Pin at what the response said, and -- for the non-vacuous half -- at the gain itself, which
    // is the mistake a caller makes when the response publishes only the gain.
    CaptureWithExposure(FixedExposureArg(Ev100Equivalent), TEXT("pw_exposurepin_right.png"),
        PinnedRight);
    CaptureWithExposure(FixedExposureArg(AdaptedGain), TEXT("pw_exposurepin_wrong.png"),
        PinnedWrong);
    if (!PinnedRight.bSuccess || !PinnedWrong.bSuccess)
    {
        TestTrue(TEXT("a follow-up failure is typed"), ExposurePinIsTypedCaptureFailure(
            PinnedRight.bSuccess ? PinnedWrong.ErrorCode : PinnedRight.ErrorCode));
        PinWrightTestSkip::SkipAssertions(*this, TEXT("capture-unavailable"),
            FString::Printf(TEXT("a pinned capture returned %s, so the auto-then-pin round trip ")
                TEXT("was not exercised"),
                PinnedRight.bSuccess ? *PinnedWrong.ErrorCode : *PinnedRight.ErrorCode));
        return true;
    }

    const TSharedPtr<FJsonObject> PinnedExposure = ExposurePinGetExposureBlock(PinnedRight);
    bool bPinned = false;
    if (PinnedExposure.IsValid())
    {
        PinnedExposure->TryGetBoolField(TEXT("pinned"), bPinned);
    }
    if (!bPinned)
    {
        TestTrue(TEXT("an unpinned preview capture explains itself"),
            PinnedExposure.IsValid() && PinnedExposure->HasField(TEXT("pinWarning")));
        PinWrightTestSkip::SkipAssertions(*this, TEXT("pin-not-applied"),
            TEXT("the response reports pinned=false, so the auto-then-pin round trip was not "
                "exercised"));
        return true;
    }
    // The pinned response must agree with itself: pinning at the reported EV100 has to report that
    // same EV100 back as its own equivalent, or the two fields are not on one scale.
    double PinnedEquivalent = 0.0;
    if (PinnedExposure->TryGetNumberField(TEXT("ev100Equivalent"), PinnedEquivalent))
    {
        TestEqual(TEXT("a capture pinned at the reported EV100 reports that EV100 back"),
            PinnedEquivalent, Ev100Equivalent, 0.01);
    }

    FString ReferencePath;
    FString RightPath;
    FString WrongPath;
    Reference.Result->TryGetStringField(TEXT("path"), ReferencePath);
    PinnedRight.Result->TryGetStringField(TEXT("path"), RightPath);
    PinnedWrong.Result->TryGetStringField(TEXT("path"), WrongPath);
    // Three distinct files, asserted before a pixel of them is trusted -- see the same assertion
    // in PinnedCapturesReproduceWithinTolerance for what its absence cost. This test's historical
    // "auto vs pin 0.000" readings were one file compared with itself.
    TestTrue(TEXT("every capture reported a path"),
        !ReferencePath.IsEmpty() && !RightPath.IsEmpty() && !WrongPath.IsEmpty());
    TestTrue(FString::Printf(
        TEXT("the three compared captures wrote three DIFFERENT files ")
        TEXT("(reference '%s', right '%s', wrong '%s')"),
        *FPaths::GetCleanFilename(ReferencePath), *FPaths::GetCleanFilename(RightPath),
        *FPaths::GetCleanFilename(WrongPath)),
        ReferencePath != RightPath && ReferencePath != WrongPath && RightPath != WrongPath);
    const TArray<FColor> ReferencePixels = ExposurePinLoadPixels(ReferencePath);
    const TArray<FColor> RightPixels = ExposurePinLoadPixels(RightPath);
    const TArray<FColor> WrongPixels = ExposurePinLoadPixels(WrongPath);
    if (ReferencePixels.Num() == 0 || RightPixels.Num() == 0 || WrongPixels.Num() == 0)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("undecodable-png"),
            TEXT("a capture PNG could not be decoded, so the auto-then-pin round trip was not "
                "exercised"));
        return true;
    }

    const double ReferenceMeanLuminance = ExposurePinMeanLuminance(ReferencePixels);
    const double RoundTripDifference = ExposurePinMeanAbsDifference(ReferencePixels, RightPixels);
    const double WrongUnitsDifference = ExposurePinMeanAbsDifference(ReferencePixels, WrongPixels);
    AddInfo(FString::Printf(
        TEXT("auto meanLuminance %.4f; auto vs pin@ev100Equivalent meanAbsDiff %.3f; ")
        TEXT("auto vs pin@adapted meanAbsDiff %.3f."),
        ReferenceMeanLuminance, RoundTripDifference, WrongUnitsDifference));

    if (ReferenceMeanLuminance < MinLitMeanLuminance)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("fixture-not-lit"), FString::Printf(
            TEXT("the preview frame is not lit (meanLuminance %.4f < %.2f decoded), so exposure ")
            TEXT("moves these pixels not at all and the round trip cannot be shown; the ")
            TEXT("round-trip and wrong-units assertions were skipped. NOT expected on any ")
            TEXT("environment: the automation commandlet renders this fixture's auto frame at ")
            TEXT("0.5849, so a reading below the gate means something stopped rendering."),
            ReferenceMeanLuminance, MinLitMeanLuminance));
        return true;
    }

    // THE property: the advertised flow reproduces the frame it was derived from.
    TestTrue(FString::Printf(
        TEXT("pinning at the reported ev100Equivalent reproduces the auto frame ")
        TEXT("(meanAbsDiff %.3f < %.1f)"), RoundTripDifference, ToleranceMeanAbsDiff),
        RoundTripDifference < ToleranceMeanAbsDiff);

    // ...and the half that makes it non-vacuous. If `ev100Equivalent` were the gain passed through
    // unchanged, the frame above WOULD be this one. Gated on the two actually being far enough
    // apart to tell apart: on a scene that happens to resolve near gain 1 they nearly coincide, and
    // asserting a difference there would be asserting noise.
    if (FMath::Abs(AdaptedGain - Ev100Equivalent) >= MinUnitSeparationStops)
    {
        TestTrue(FString::Printf(
            TEXT("pinning at the raw gain instead does NOT reproduce it, so the two fields are in ")
            TEXT("different units (meanAbsDiff %.3f > %.1f)"),
            WrongUnitsDifference, MinWrongUnitsMeanAbsDiff),
            WrongUnitsDifference > MinWrongUnitsMeanAbsDiff);
    }
    else
    {
        // A PARTIAL skip: the round-trip assertion above did run. The marker is still emitted --
        // "asserted one of two" is not "asserted both", and the count is what the tooling
        // reconciles.
        PinWrightTestSkip::SkipAssertions(*this, TEXT("units-too-close"), FString::Printf(
            TEXT("the gain (%.4f) and its EV100 (%.4f) are less than %.1f apart on this scene, ")
            TEXT("so a wrong-units pin is indistinguishable from the right one and the ")
            TEXT("non-vacuity assertion was skipped"),
            AdaptedGain, Ev100Equivalent, MinUnitSeparationStops));
    }
    return true;
}
