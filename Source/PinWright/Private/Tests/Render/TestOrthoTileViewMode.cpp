// Copyright (c) 2026 Alexander Penkin. MIT License.

// The `viewMode` parameter on render.capture_ortho_tiles.
//
// THE DEFECT CLASS THESE TESTS EXIST FOR, in one sentence: a view-mode argument that is accepted,
// echoed back and renders an ordinary lit frame. camera.orbit_shots shipped exactly that
// (PreviewViewportCaptureUtils.cpp:1542-1545 records it), and a caller reading the response could
// not tell it from one that had applied. Every assertion below is therefore written against a
// MEASURED show flag or a MEASURED refusal code - never against "the call succeeded".
//
// TWO CLASSES OF TEST, kept apart the same way TestOrthoTileCapture.cpp keeps them apart.
//
// The deterministic ones never render and never need a world: applying a view mode to an
// FEngineShowFlags set, classifying a mode's reachability, and phrasing the exposure-blocked
// reason are all pure functions of the request. They carry the contract and they run identically
// on a headless commandlet.
//
// The live ones drive the real verb. They follow this suite's RHI-guard pattern - a failure must
// be TYPED, the branch taken is logged with AddInfo, and the deterministic half of each test has
// already asserted the same fact so a skipped pixel branch never leaves a test that asserted
// nothing.
#include "Misc/AutomationTest.h"
#include "Compat/EngineVersionCompat.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/HandlerRegistration.h"
#include "Handlers/ParamSpec.h"
#include "Handlers/Render/OrthoTileCaptureUtils.h"
#include "Handlers/Render/PreviewViewportCaptureUtils.h"
#include "Handlers/Render/ViewModeVocabulary.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Tests/TestUtils.h"

#include "Editor.h"
#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "Misc/Guid.h"
#include "ShowFlags.h"

namespace
{
    // Names are prefixed OrthoVm rather than Ortho because Unity merges translation units, and
    // TestOrthoTileCapture.cpp already owns OrthoVec / OrthoBasePayload / OrthoDeleteDirectory in
    // ITS anonymous namespace. Two anonymous-namespace symbols of the same name in one merged blob
    // is a redefinition, not an override.

    TSharedPtr<FJsonObject> OrthoVmVec(double X, double Y, double Z)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetNumberField(TEXT("x"), X);
        Obj->SetNumberField(TEXT("y"), Y);
        Obj->SetNumberField(TEXT("z"), Z);
        return Obj;
    }

    // A payload that is valid except for whatever the caller overrides: a 4000 x 4000 cm top-down
    // box as one 64 px tile. Deliberately tiny - these tests are about show flags, not pixels, and
    // the burst cost should not gate them.
    TSharedPtr<FJsonObject> OrthoVmBasePayload()
    {
        TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
        P->SetObjectField(TEXT("worldMin"), OrthoVmVec(-2000.0, -2000.0, 0.0));
        P->SetObjectField(TEXT("worldMax"), OrthoVmVec(2000.0, 2000.0, 0.0));
        P->SetStringField(TEXT("axes"), TEXT("top_down_x_right_y_down"));
        P->SetNumberField(TEXT("exposure"), 11.0);
        P->SetNumberField(TEXT("cols"), 1.0);
        P->SetNumberField(TEXT("rows"), 1.0);
        P->SetNumberField(TEXT("tilePixels"), 64.0);
        P->SetBoolField(TEXT("overwrite"), true);
        // A headless editor draws this map close to black from above; blankness is not what these
        // tests measure and an ERR_BLANK_CAPTURE would hide the response they need to read.
        P->SetBoolField(TEXT("allowBlank"), true);
        P->SetStringField(TEXT("namePrefix"),
            FString::Printf(TEXT("pw_ortho_vm_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Short)));
        return P;
    }

    // The typed, non-crashing exits the verb may return when there is no RHI surface or no editor
    // world. Anything outside this list is a real defect, so the guard cannot swallow one. Kept
    // deliberately identical in spirit to TestOrthoTileCapture.cpp's list; a refusal that is NOT
    // here fails the test rather than being logged as a skip.
    bool OrthoVmIsTypedCaptureFailure(const FString& ErrorCode)
    {
        return ErrorCode == TEXT("EDITOR_NOT_AVAILABLE") ||
            ErrorCode == TEXT("NO_EDITOR_WORLD") ||
            ErrorCode == TEXT("SCENE_BOUNDS_NOT_MEASURED") ||
            ErrorCode == TEXT("SCENE_CAPTURE_FAILED") ||
            ErrorCode == TEXT("CAPTURE_CAMERA_NOT_APPLIED") ||
            ErrorCode == TEXT("RENDER_TARGET_CREATE_FAILED") ||
            ErrorCode == TEXT("READ_PIXELS_FAILED") ||
            ErrorCode == TEXT("BLANK_CAPTURE") ||
            ErrorCode == TEXT("ENCODE_FAILED") ||
            ErrorCode == TEXT("WRITE_FAILED");
    }

    void OrthoVmDeleteDirectory(const FString& Dir)
    {
        if (!Dir.IsEmpty())
        {
            IFileManager::Get().DeleteDirectory(*Dir, false, true);
        }
    }

    void OrthoVmCleanUp(const FTestResponseCapture& Capture)
    {
        FString OutputDir;
        if (Capture.Result.IsValid())
        {
            Capture.Result->TryGetStringField(TEXT("outputDir"), OutputDir);
        }
        OrthoVmDeleteDirectory(OutputDir);
    }

    // A pin read the way the verb reads it: off a payload, through the shared parser, with no
    // viewport client. Passing the payload rather than constructing the pin by hand is the point -
    // it is the production path, so a change in the parser's default reaches these tests.
    bool OrthoVmParsePin(const TCHAR* WireSpelling, PinWrightRenderCapture::FViewModePin& OutPin,
        FString& OutErrCode, FString& OutErrMsg)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        if (WireSpelling != nullptr)
        {
            Payload->SetStringField(TEXT("viewMode"), WireSpelling);
        }
        return PinWrightRenderCapture::ParseViewModePin(Payload, OutPin, OutErrCode, OutErrMsg);
    }

    // Index + name for every BUILT-IN engine show flag, so two flag sets can be diffed exhaustively
    // rather than over a hand-listed subset. Custom (plugin-registered) flags are skipped: nothing
    // on this path writes them, so they can only add noise.
    struct FOrthoVmFlagSink
    {
        TArray<TPair<uint32, FString>>* Out = nullptr;
        bool OnEngineShowFlag(uint32 InIndex, const FString& InName)
        {
            Out->Emplace(InIndex, InName);
            return true;
        }
        bool OnCustomShowFlag(uint32 /*InIndex*/, const FString& /*InName*/) { return true; }
    };

    const TArray<TPair<uint32, FString>>& OrthoVmAllFlagIndices()
    {
        static const TArray<TPair<uint32, FString>> Indices = []()
        {
            TArray<TPair<uint32, FString>> Built;
            FOrthoVmFlagSink Sink;
            Sink.Out = &Built;
            FEngineShowFlags::IterateAllFlags(Sink);
            return Built;
        }();
        return Indices;
    }

    // Every built-in flag whose value differs between two sets, by name.
    TArray<FString> OrthoVmDiffFlags(const FEngineShowFlags& A, const FEngineShowFlags& B)
    {
        TArray<FString> Different;
        for (const TPair<uint32, FString>& Flag : OrthoVmAllFlagIndices())
        {
            if (A.GetSingleFlag(Flag.Key) != B.GetSingleFlag(Flag.Key))
            {
                Different.Add(Flag.Value);
            }
        }
        return Different;
    }

    const TSharedPtr<FJsonObject>* OrthoVmGetObject(const TSharedPtr<FJsonObject>& Root, const TCHAR* Field)
    {
        const TSharedPtr<FJsonObject>* Out = nullptr;
        if (Root.IsValid() && Root->TryGetObjectField(Field, Out) && Out && (*Out).IsValid())
        {
            return Out;
        }
        return nullptr;
    }

    bool OrthoVmAnyWarningContains(const TSharedPtr<FJsonObject>& Result, const FString& Needle)
    {
        const TArray<TSharedPtr<FJsonValue>>* Warnings = nullptr;
        if (!Result.IsValid() || !Result->TryGetArrayField(TEXT("warnings"), Warnings) || !Warnings)
        {
            return false;
        }
        for (const TSharedPtr<FJsonValue>& Value : *Warnings)
        {
            FString Text;
            if (Value.IsValid() && Value->TryGetString(Text) && Text.Contains(Needle, ESearchCase::IgnoreCase))
            {
                return true;
            }
        }
        return false;
    }
}

// ============================================================================
// 1. The omitted parameter writes nothing
// ============================================================================

// A new optional parameter that moves a pixel on the path where it was not supplied is a silent
// behaviour change for every existing caller and every stored reference image. This asserts the
// omitted path against a reference built INDEPENDENTLY of the code under test -
// MakeBaseCaptureShowFlags() names the base and the one deliberate deviation - so "identical" is a
// comparison rather than a tautology.
//
// Counterfactual: call ApplyViewMode(VMI_Lit, ...) unconditionally instead of gating on
// WantsOverride() and this fails on PostProcessing, BSPTriangles, Brushes and Wireframe at once.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOrthoTilesOmittedViewModeLeavesShowFlagsUntouchedTest,
    "PinWright.render.capture_ortho_tiles.OmittedViewModeLeavesShowFlagsUntouched",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FOrthoTilesOmittedViewModeLeavesShowFlagsUntouchedTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightOrthoTiles;

    // ---- path A: the pin the PARSER produces for a payload with no `viewMode` field ----
    PinWrightRenderCapture::FViewModePin ParsedPin;
    FString ErrCode;
    FString ErrMsg;
    TestTrue(TEXT("a payload with no viewMode field parses cleanly"),
        OrthoVmParsePin(/*WireSpelling=*/nullptr, ParsedPin, ErrCode, ErrMsg));
    TestFalse(TEXT("and yields a pin that does not want an override"), ParsedPin.WantsOverride());

    FEngineShowFlags FromParsedPin = MakeBaseCaptureShowFlags();
    const FViewModePlan ParsedPlan = ApplyViewModeToCaptureShowFlags(ParsedPin, FromParsedPin);

    // ---- path B: a default-constructed pin, the contract every capture verb's field carries ----
    const PinWrightRenderCapture::FViewModePin DefaultPin;
    FEngineShowFlags FromDefaultPin = MakeBaseCaptureShowFlags();
    const FViewModePlan DefaultPlan = ApplyViewModeToCaptureShowFlags(DefaultPin, FromDefaultPin);

    // ---- and the reference the two are measured against ----
    const FEngineShowFlags Untouched = MakeBaseCaptureShowFlags();

    const TArray<FString> ParsedVsUntouched = OrthoVmDiffFlags(FromParsedPin, Untouched);
    TestEqual(FString::Printf(TEXT("the parsed omitted-parameter path writes no show flag (moved: %s)"),
            *FString::Join(ParsedVsUntouched, TEXT(", "))),
        ParsedVsUntouched.Num(), 0);

    const TArray<FString> DefaultVsUntouched = OrthoVmDiffFlags(FromDefaultPin, Untouched);
    TestEqual(FString::Printf(TEXT("the default-constructed pin writes no show flag (moved: %s)"),
            *FString::Join(DefaultVsUntouched, TEXT(", "))),
        DefaultVsUntouched.Num(), 0);

    const TArray<FString> ParsedVsDefault = OrthoVmDiffFlags(FromParsedPin, FromDefaultPin);
    TestEqual(FString::Printf(TEXT("the two omitted-parameter paths agree flag for flag (differ: %s)"),
            *FString::Join(ParsedVsDefault, TEXT(", "))),
        ParsedVsDefault.Num(), 0);

    // The report says "nothing was requested" rather than omitting the block, so a caller can tell
    // it from a request that did nothing.
    TestFalse(TEXT("the parsed plan reports no request"), ParsedPlan.bRequested);
    TestFalse(TEXT("the default plan reports no request"), DefaultPlan.bRequested);
    TestTrue(TEXT("a derived mode is reported even with no request"), !ParsedPlan.DerivedKey.IsEmpty());
    // NOT a formality, and it caught a live defect: UE 5.8's FEngineShowFlags::Init clears every
    // hidden Visualize* flag EXCEPT VisualizeMegaLights (ShowFlags.h:409-415), and FindViewMode
    // tests that one above every lit mode (ShowFlags.cpp:814) - so before MakeBaseCaptureShowFlags
    // cleared it, an ordinary burst with no `viewMode` reported derived:"VisualizeMegaLights".
    // Counterfactual: drop the SetVisualizeMegaLights(false) line from MakeBaseCaptureShowFlags and
    // this fails while every requested-mode test keeps passing, because ApplyViewMode writes that
    // flag itself (ShowFlags.cpp:409) and so only the omitted path can see it.
    TestEqual(TEXT("an unmodified capture derives back as Lit"),
        ParsedPlan.DerivedKey, PinWrightViewModes::GetKey(VMI_Lit));

    const TSharedPtr<FJsonObject> Info = MakeViewModeInfoObject(ParsedPlan);
    bool bRequestedField = true;
    TestTrue(TEXT("the viewMode block is emitted with no viewMode requested"),
        Info.IsValid() && Info->TryGetBoolField(TEXT("requested"), bRequestedField));
    TestFalse(TEXT("and reports requested:false"), bRequestedField);

    // ---- the live half: two real components, one through each path, diffed field for field ----
    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World)
    {
        AddError(TEXT("no editor world, so the component-level half could not be measured at all"));
        return true;
    }

    const PinWrightRenderCapture::FExposurePin NoExposure;
    FOrthoTileCapture FromParsed(World, 64, 64, NoExposure, ParsedPin);
    FOrthoTileCapture FromDefault(World, 64, 64, NoExposure, DefaultPin);
    if (!FromParsed.IsValid() || !FromDefault.IsValid())
    {
        AddInfo(FString::Printf(
            TEXT("BRANCH: a capture component could not be allocated (%s / %s) - the component-level "
                 "diff is SKIPPED, but the deterministic assertions above all ran."),
            *FromParsed.GetInitErrorCode(), *FromDefault.GetInitErrorCode()));
        // Asserted on this branch too: the failure must be typed, not a crash-adjacent blank.
        TestTrue(FString::Printf(TEXT("the parsed-path init failure is typed (got '%s')"),
                *FromParsed.GetInitErrorCode()),
            FromParsed.IsValid() || OrthoVmIsTypedCaptureFailure(FromParsed.GetInitErrorCode()));
        TestTrue(FString::Printf(TEXT("the default-path init failure is typed (got '%s')"),
                *FromDefault.GetInitErrorCode()),
            FromDefault.IsValid() || OrthoVmIsTypedCaptureFailure(FromDefault.GetInitErrorCode()));
        return true;
    }

    AddInfo(TEXT("BRANCH: both capture components allocated - the component-level diff is live."));
    const TSharedPtr<FJsonObject> ParsedFlags = FromParsed.DescribeShowFlags();
    const TSharedPtr<FJsonObject> DefaultFlags = FromDefault.DescribeShowFlags();
    TestTrue(TEXT("both components describe their show flags"),
        ParsedFlags.IsValid() && DefaultFlags.IsValid());
    if (ParsedFlags.IsValid() && DefaultFlags.IsValid())
    {
        for (const TPair<FString, TSharedPtr<FJsonValue>> Field : ParsedFlags->Values)
        {
            if (Field.Key == TEXT("viewMode"))
            {
                continue; // compared through the plan above; a nested object is not a scalar diff
            }
            const TSharedPtr<FJsonValue> Other = DefaultFlags->TryGetField(Field.Key);
            TestTrue(FString::Printf(TEXT("the default-pin component reports show flag '%s' too"), *Field.Key),
                Other.IsValid());
            if (Other.IsValid() && Field.Value.IsValid())
            {
                TestEqual(FString::Printf(TEXT("show flag '%s' matches across both paths"), *Field.Key),
                    Other->AsString(), Field.Value->AsString());
            }
        }

        // The regression floor for a bug this file's ordering fix closed:
        // USceneCaptureComponent::OnRegister calls UpdateShowFlags, which reassigns the WHOLE set
        // from the archetype - so a flag written before RegisterComponentWithWorld is silently
        // reverted. SetTemporalAA(false) used to be written there and was lost on every burst.
        // Counterfactual: move that line back above the register call and this fails.
        bool bTemporalAA = true;
        TestTrue(TEXT("the component reports its temporalAA flag"),
            ParsedFlags->TryGetBoolField(TEXT("temporalAA"), bTemporalAA));
        TestFalse(TEXT("temporalAA is off on the live component, not merely requested off"), bTemporalAA);

        // The SECOND flag written after registration, and the one whose loss is silent in the
        // pixels: VisualizeMegaLights renders nothing without a companion the scene-capture path
        // never sets, so nothing in the image would look wrong - only `derived` would say
        // VisualizeMegaLights over a Lit frame. Measured on the REGISTERED component, so this is
        // the ordering floor for it exactly as the line above is for TemporalAA.
        // Counterfactual: move either write back above RegisterComponentWithWorld and one of these
        // two fails, because OnRegister's UpdateShowFlags reassigns the whole set from the archetype.
        // The flag, the clear and the published field are all 5.8-only; older engines have no
        // VisualizeMegaLights bit for FindViewMode to read, so there is no ordering floor to pin.
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 8, 0)
        bool bVisualizeMegaLights = true;
        TestTrue(TEXT("the component reports its visualizeMegaLights flag"),
            ParsedFlags->TryGetBoolField(TEXT("visualizeMegaLights"), bVisualizeMegaLights));
        TestFalse(TEXT("visualizeMegaLights is off on the live component, though the engine's game "
                       "defaults ship it on"),
            bVisualizeMegaLights);
#endif

        // And the consequence, read off the LIVE component rather than off the reference set: the
        // mode the engine derives from the flags the renderer will actually see.
        const TSharedPtr<FJsonObject>* LiveViewMode = nullptr;
        if (TestTrue(TEXT("the component publishes its viewMode block"),
                ParsedFlags->TryGetObjectField(TEXT("viewMode"), LiveViewMode)
                    && LiveViewMode && (*LiveViewMode).IsValid()))
        {
            FString LiveDerived;
            TestTrue(TEXT("the live viewMode block carries a derived key"),
                (*LiveViewMode)->TryGetStringField(TEXT("derived"), LiveDerived));
            TestEqual(TEXT("the live component's flags derive back as Lit, not as a visualization mode"),
                LiveDerived, PinWrightViewModes::GetKey(VMI_Lit));
        }
    }
    return true;
}

// ============================================================================
// 2. Unlit actually clears the lighting flag
// ============================================================================

// The measured assertion, and the reason the implementation calls TWO engine functions rather than
// the one the design note originally named.
//
// ApplyViewMode alone does NOT clear Lighting for VMI_Unlit - it never calls SetLighting at all
// (UE 5.8 Runtime/Engine/Private/ShowFlags.cpp:292-446); the only thing it does for Unlit is
// SetPostProcessing(false). Lighting/Atmosphere/Fog are cleared by EngineShowFlagOverride
// (:530-585). Apply only the first and `viewMode:"unlit"` returns a LIT frame labelled unlit.
//
// Counterfactual: delete the EngineShowFlagOverride call in ApplyViewModeToCaptureShowFlags and
// the first assertion below fails while every "the call succeeded" style check still passes -
// which is precisely how camera.orbit_shots shipped an inert viewMode.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOrthoTilesUnlitActuallyClearsTheLightingFlagTest,
    "PinWright.render.capture_ortho_tiles.UnlitActuallyClearsTheLightingFlag",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FOrthoTilesUnlitActuallyClearsTheLightingFlagTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightOrthoTiles;

    PinWrightRenderCapture::FViewModePin Pin;
    FString ErrCode;
    FString ErrMsg;
    TestTrue(FString::Printf(TEXT("viewMode:\"unlit\" resolves (%s: %s)"), *ErrCode, *ErrMsg),
        OrthoVmParsePin(TEXT("unlit"), Pin, ErrCode, ErrMsg));
    TestTrue(TEXT("and yields a pin that wants an override"), Pin.WantsOverride());
    TestEqual(TEXT("the pin carries the canonical key"), Pin.Key, PinWrightViewModes::GetKey(VMI_Unlit));

    FEngineShowFlags Flags = MakeBaseCaptureShowFlags();
    TestTrue(TEXT("the base capture flag set starts LIT, so clearing it is a real change"),
        Flags.Lighting != 0);

    const FViewModePlan Plan = ApplyViewModeToCaptureShowFlags(Pin, Flags);

    // The measured flag, not the request.
    TestTrue(TEXT("unlit clears Lighting on the flags the renderer reads"), Flags.Lighting == 0);
    TestTrue(TEXT("unlit clears PostProcessing too, which is ApplyViewMode's own half"),
        Flags.PostProcessing == 0);

    // The reported key, gated on the measurement rather than echoed.
    TestTrue(TEXT("every show flag that distinguishes unlit from Lit read back"), Plan.bApplied);
    TestEqual(TEXT("the reported applied key is the canonical spelling of unlit"),
        Plan.AppliedKey, PinWrightViewModes::GetKey(VMI_Unlit));
    TestTrue(FString::Printf(TEXT("the reported applied key is 'unlit' (got '%s')"), *Plan.AppliedKey),
        Plan.AppliedKey.Equals(TEXT("unlit"), ESearchCase::IgnoreCase));

    // An INDEPENDENT second opinion: the engine's own FindViewMode reads VMI_Unlit back out of the
    // flags, and its final line is literally `EngineShowFlags.Lighting ? VMI_Lit : VMI_Unlit`
    // (ShowFlags.cpp:978). This cannot be satisfied by assigning a field anywhere in PinWright.
    TestEqual(TEXT("the engine's own FindViewMode derives Unlit back out of the flags"),
        Plan.DerivedKey, PinWrightViewModes::GetKey(VMI_Unlit));

    // The pinned fact that forces the second engine call to stay. If a future edit drops
    // EngineShowFlagOverride, this is the assertion that says why the frame went wrong.
    {
        FEngineShowFlags ApplyViewModeOnly = MakeBaseCaptureShowFlags();
        ApplyViewMode(VMI_Unlit, /*bPerspective=*/false, ApplyViewModeOnly);
        TestTrue(TEXT("ApplyViewMode ALONE leaves Lighting ON for unlit - the reason "
                      "EngineShowFlagOverride is also called"),
            ApplyViewModeOnly.Lighting != 0);
    }

    // ---- the live half ----
    TSharedPtr<FJsonObject> Payload = OrthoVmBasePayload();
    Payload->SetStringField(TEXT("viewMode"), TEXT("unlit"));
    FTestResponseCapture Capture;
    TestTrue(TEXT("render.capture_ortho_tiles handler found"),
        InvokeHandlerWithCapture(TEXT("render.capture_ortho_tiles"), Payload, Capture));
    TestTrue(TEXT("handler sent a response"), Capture.bWasCalled);

    if (Capture.bSuccess && Capture.Result.IsValid())
    {
        AddInfo(TEXT("BRANCH: the unlit orthographic capture RAN - response assertions are live."));
        const TSharedPtr<FJsonObject>* ShowFlags = OrthoVmGetObject(Capture.Result, TEXT("showFlags"));
        TestTrue(TEXT("a successful capture publishes a showFlags block"), ShowFlags != nullptr);
        if (ShowFlags)
        {
            bool bLighting = true;
            TestTrue(TEXT("the response reports the lighting show flag"),
                (*ShowFlags)->TryGetBoolField(TEXT("lighting"), bLighting));
            TestFalse(TEXT("the response's measured lighting flag is off under viewMode:unlit"), bLighting);

            const TSharedPtr<FJsonObject>* ViewModeBlock = OrthoVmGetObject(*ShowFlags, TEXT("viewMode"));
            TestTrue(TEXT("the showFlags block carries a viewMode sub-block"), ViewModeBlock != nullptr);
            if (ViewModeBlock)
            {
                FString Applied;
                TestTrue(TEXT("the viewMode sub-block reports an applied key"),
                    (*ViewModeBlock)->TryGetStringField(TEXT("applied"), Applied));
                TestTrue(FString::Printf(TEXT("the applied key is 'unlit' (got '%s')"), *Applied),
                    Applied.Equals(TEXT("unlit"), ESearchCase::IgnoreCase));
            }
        }
    }
    else
    {
        AddInfo(FString::Printf(
            TEXT("BRANCH: the unlit orthographic capture was REFUSED with %s - the response "
                 "assertions are SKIPPED, but every deterministic assertion above ran. Message: %s"),
            *Capture.ErrorCode, *Capture.Message));
        TestTrue(FString::Printf(TEXT("the refusal is a typed capture failure (got %s)"), *Capture.ErrorCode),
            OrthoVmIsTypedCaptureFailure(Capture.ErrorCode));
    }
    OrthoVmCleanUp(Capture);
    return true;
}

// ============================================================================
// 3. A deliberately unlit frame does not read as a fault
// ============================================================================

// An unlit capture cannot carry an exposure pin - IsAutoExposureDebugMode discards the override
// when Lighting or PostProcessing is off (PostProcessEyeAdaptation.cpp:493-511). The VERDICT is
// therefore the same whoever cleared the flag, and it stays `pinned: false`. The REASON is not:
// an unexplained lighting-off frame is a fault to chase, and a requested `unlit` is the request
// being honoured. Reporting the second in the first's words sends a caller hunting a defect they
// deliberately caused.
//
// THE CONTRAST IS THE TEST. Asserting only that the unlit message names the mode would pass on an
// implementation that named the mode in BOTH messages, or on one whose warning list happened to be
// empty. Both phrasings are produced here from the same production function and compared.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOrthoTilesUnlitDoesNotReadAsAFaultTest,
    "PinWright.render.capture_ortho_tiles.UnlitDoesNotReadAsAFault",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FOrthoTilesUnlitDoesNotReadAsAFaultTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightOrthoTiles;

    const FString UnlitKey = PinWrightViewModes::GetKey(VMI_Unlit);

    // The contrast case: the SAME flag state, with and without a requested mode.
    const FString WithoutMode = DescribeExposureBlockedByShowFlags(
        /*bLighting=*/false, /*bPostProcessing=*/true, /*RequestedViewModeKey=*/FString());
    const FString WithMode = DescribeExposureBlockedByShowFlags(
        /*bLighting=*/false, /*bPostProcessing=*/true, UnlitKey);

    TestTrue(TEXT("the un-parameterised reading DOES report the blocked pin - the contrast case is "
                  "not an empty message"),
        !WithoutMode.IsEmpty());
    TestFalse(TEXT("the un-parameterised reading names no view mode, because none was asked for"),
        WithoutMode.Contains(UnlitKey, ESearchCase::IgnoreCase));
    TestTrue(TEXT("both readings name the engine predicate that discards the override, so neither "
                  "loses the mechanism"),
        WithoutMode.Contains(TEXT("IsAutoExposureDebugMode")) && WithMode.Contains(TEXT("IsAutoExposureDebugMode")));

    TestTrue(FString::Printf(TEXT("the requested reading names the mode (got: %s)"), *WithMode),
        WithMode.Contains(UnlitKey, ESearchCase::IgnoreCase));
    TestTrue(TEXT("the requested reading says the mode caused it rather than reading as a fault"),
        WithMode.Contains(TEXT("was requested")) && WithMode.Contains(TEXT("rather than a fault")));
    TestNotEqual(TEXT("the two readings are different text, so the distinction is real"),
        WithMode, WithoutMode);

    // Both flags off, the other reachable state, so the phrasing is not accidentally tied to one.
    const FString BothOff = DescribeExposureBlockedByShowFlags(false, false, UnlitKey);
    TestTrue(TEXT("the requested reading names the mode with both flags off too"),
        BothOff.Contains(UnlitKey, ESearchCase::IgnoreCase));

    // ---- the live half ----
    TSharedPtr<FJsonObject> Payload = OrthoVmBasePayload();
    Payload->SetStringField(TEXT("viewMode"), TEXT("unlit"));
    FTestResponseCapture Capture;
    TestTrue(TEXT("render.capture_ortho_tiles handler found"),
        InvokeHandlerWithCapture(TEXT("render.capture_ortho_tiles"), Payload, Capture));
    TestTrue(TEXT("handler sent a response"), Capture.bWasCalled);

    if (Capture.bSuccess && Capture.Result.IsValid())
    {
        AddInfo(TEXT("BRANCH: the unlit orthographic capture RAN - warning assertions are live."));
        const TSharedPtr<FJsonObject>* Exposure = OrthoVmGetObject(Capture.Result, TEXT("exposure"));
        TestTrue(TEXT("a successful capture publishes an exposure block"), Exposure != nullptr);
        if (Exposure)
        {
            bool bPinned = true;
            TestTrue(TEXT("the response reports whether the pin governed the pixels"),
                (*Exposure)->TryGetBoolField(TEXT("pinned"), bPinned));
            // Honest either way: unlit clears PostProcessing, so the pin cannot govern.
            TestFalse(TEXT("an unlit burst honestly reports pinned:false rather than claiming a pin"),
                bPinned);
            FString PinWarning;
            TestTrue(TEXT("and says why"),
                (*Exposure)->TryGetStringField(TEXT("pinWarning"), PinWarning));
            TestTrue(FString::Printf(TEXT("the pin warning names the requested mode (got: %s)"), *PinWarning),
                PinWarning.Contains(UnlitKey, ESearchCase::IgnoreCase));
        }
        TestTrue(TEXT("the response's warning list names the requested mode as the cause"),
            OrthoVmAnyWarningContains(Capture.Result, UnlitKey));
    }
    else
    {
        AddInfo(FString::Printf(
            TEXT("BRANCH: the unlit orthographic capture was REFUSED with %s - the warning "
                 "assertions are SKIPPED, but the contrast pair above ran. Message: %s"),
            *Capture.ErrorCode, *Capture.Message));
        TestTrue(FString::Printf(TEXT("the refusal is a typed capture failure (got %s)"), *Capture.ErrorCode),
            OrthoVmIsTypedCaptureFailure(Capture.ErrorCode));
    }
    OrthoVmCleanUp(Capture);
    return true;
}

// ============================================================================
// 4. Unreachable families are refused specifically
// ============================================================================

// A mode this renderer cannot draw must be a TYPED refusal naming the mode, never a silently
// ignored argument that returns a normally-lit frame. Two families are unreachable for two
// DIFFERENT reasons, so they must not collapse onto one generic code: a caller retries a
// needs-companion refusal by selecting a target, and a not-renderable refusal by changing verb.
//
// Counterfactual: return one code for both (or drop the reach gate and let the flag be written)
// and the codes-differ assertion fails.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOrthoTilesUnreachableFamiliesAreRefusedSpecificallyTest,
    "PinWright.render.capture_ortho_tiles.UnreachableFamiliesAreRefusedSpecifically",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FOrthoTilesUnreachableFamiliesAreRefusedSpecificallyTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightOrthoTiles;

    FString BufferCode;
    FString LightmapCode;

    {
        TSharedPtr<FJsonObject> Payload = OrthoVmBasePayload();
        Payload->SetStringField(TEXT("viewMode"), TEXT("VisualizeBuffer"));
        FTestResponseCapture Capture;
        TestTrue(TEXT("handler found (VisualizeBuffer)"),
            InvokeHandlerWithCapture(TEXT("render.capture_ortho_tiles"), Payload, Capture));
        TestFalse(TEXT("a sub-visualisation mode is refused, not rendered"), Capture.bSuccess);
        BufferCode = Capture.ErrorCode;
        TestEqual(TEXT("VisualizeBuffer is refused as needing a companion selection"),
            BufferCode, FString(TEXT("VIEW_MODE_NEEDS_COMPANION")));
        OrthoVmCleanUp(Capture);
    }

    {
        TSharedPtr<FJsonObject> Payload = OrthoVmBasePayload();
        Payload->SetStringField(TEXT("viewMode"), TEXT("LightmapDensity"));
        FTestResponseCapture Capture;
        TestTrue(TEXT("handler found (LightmapDensity)"),
            InvokeHandlerWithCapture(TEXT("render.capture_ortho_tiles"), Payload, Capture));
        TestFalse(TEXT("an editor-debug mode is refused, not rendered"), Capture.bSuccess);
        LightmapCode = Capture.ErrorCode;
        TestEqual(TEXT("LightmapDensity is refused as not renderable on this path"),
            LightmapCode, FString(TEXT("VIEW_MODE_NOT_RENDERABLE")));
        // The message must name the mechanism the caller has to go around, not merely say no.
        TestTrue(FString::Printf(TEXT("the refusal names the editor viewport's own show-flag override "
                                      "pairing (got: %s)"), *Capture.Message),
            Capture.Message.Contains(TEXT("EngineShowFlagOverride")));
        TestTrue(TEXT("the refusal names the mode it refused"),
            Capture.Message.Contains(TEXT("LightmapDensity")));
        TestTrue(TEXT("the refusal names the verb that CAN render this family"),
            Capture.Message.Contains(TEXT("render.capture_open_level")));
        OrthoVmCleanUp(Capture);
    }

    TestNotEqual(TEXT("the two families carry DIFFERENT refusal codes - one generic code for both "
                      "would tell a caller nothing about what to do next"),
        BufferCode, LightmapCode);

    // The reach gate itself, exercised directly. It is a backstop below the vocabulary's own
    // refusal, so a mode the parser already rejects still has to be rejected here.
    const EViewModeIndex Unreachable[] = {
        VMI_VisualizeBuffer, VMI_VisualizeSubstrate, VMI_LightmapDensity, VMI_LitLightmapDensity,
        VMI_StationaryLightOverlap, VMI_CollisionPawn, VMI_CollisionVisibility };
    for (const EViewModeIndex Mode : Unreachable)
    {
        FString Code;
        FString Message;
        TestFalse(FString::Printf(TEXT("'%s' is not reachable on a scene capture"),
                *PinWrightViewModes::GetKey(Mode)),
            IsViewModeReachableOnSceneCapture(Mode, Code, Message));
        TestEqual(FString::Printf(TEXT("'%s' carries a registered refusal code"),
                *PinWrightViewModes::GetKey(Mode)),
            Code, FString(TEXT("VIEW_MODE_NOT_RENDERABLE")));
        TestTrue(FString::Printf(TEXT("'%s' carries a message naming itself"),
                *PinWrightViewModes::GetKey(Mode)),
            Message.Contains(PinWrightViewModes::GetKey(Mode)));
    }

    // The other half of the gate: the show-flag family is NOT refused. Without this the test would
    // pass on an implementation that refused everything.
    // The last four enumerators arrived in UE 5.7, so the list they belong to is version-gated
    // rather than the whole assertion: the ten before them are present on every supported engine
    // and keep this half of the gate meaningful there.
    const EViewModeIndex Reachable[] = {
        VMI_Unlit, VMI_BrushWireframe, VMI_Wireframe, VMI_LightingOnly, VMI_Lit_DetailLighting,
        VMI_ReflectionOverride, VMI_ShaderComplexity, VMI_QuadOverdraw, VMI_LODColoration,
        VMI_HLODColoration
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 7, 0)
        , VMI_FrontBackFace, VMI_Zebra, VMI_Clay, VMI_RandomColor
#endif
        };
    for (const EViewModeIndex Mode : Reachable)
    {
        FString Code;
        FString Message;
        TestTrue(FString::Printf(TEXT("'%s' IS reachable on a scene capture"),
                *PinWrightViewModes::GetKey(Mode)),
            IsViewModeReachableOnSceneCapture(Mode, Code, Message));
        TestTrue(FString::Printf(TEXT("'%s' carries no refusal code when it is reachable"),
                *PinWrightViewModes::GetKey(Mode)),
            Code.IsEmpty() && Message.IsEmpty());
    }
    return true;
}

// ============================================================================
// 5. The game-set base is reported, not assumed
// ============================================================================

// The open question this answers: the engine's ApplyViewMode is written against an EDITOR flag set
// and a capture component's flags start from the GAME set (SceneCaptureComponent.cpp:169), so
// whether a given family's flags actually take on this base was unknown. The answer must be a
// MEASUREMENT the response carries, not a table in a comment - a comment goes stale the first time
// the engine changes which flags a mode writes.
//
// So: every mode is applied to a real game-set flag copy, the flags are read back, and the plan's
// mismatch list is compared against an INDEPENDENT recomputation from the vocabulary. Whatever the
// measured answer is, it lands in the log via AddInfo and in the response via showFlagMismatches.
//
// Counterfactual: report a hardcoded empty mismatch array and the consistency assertion below
// fails as soon as any flag stops taking.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOrthoTilesGameFlagBaseIsReportedNotAssumedTest,
    "PinWright.render.capture_ortho_tiles.GameFlagBaseIsReportedNotAssumed",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FOrthoTilesGameFlagBaseIsReportedNotAssumedTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightOrthoTiles;

    // The show-flag-only family plus the whole ChooseDebugViewShaderMode family, which is the row
    // that was marked GAP: shader complexity, quad overdraw, LOD / HLOD coloration and the four
    // texture-accuracy modes are all selected from EngineShowFlags alone
    // (FSceneViewFamily::ChooseDebugViewShaderMode, UE 5.8 Runtime/Engine/Private/SceneView.cpp
    // :3239-3297), so if their flags take here they reach the renderer's own selector.
    const TCHAR* Spellings[] = {
        TEXT("unlit"), TEXT("wireframe"), TEXT("CSGWireframe"), TEXT("LightingOnly"),
        TEXT("DetailLighting"), TEXT("ReflectionOverride"), TEXT("front_back_face"),
        TEXT("zebra"), TEXT("clay"), TEXT("random_color"), TEXT("LightComplexity"),
        TEXT("ShaderComplexity"), TEXT("QuadOverdraw"), TEXT("ShaderComplexityWithQuadOverdraw"),
        TEXT("LODColoration"), TEXT("HLODColoration"), TEXT("PrimitiveDistanceAccuracy"),
        TEXT("MeshUVDensityAccuracy"), TEXT("MaterialTextureScaleAccuracy"),
        TEXT("RequiredTextureResolution") };

    int32 Measured = 0;
    for (const TCHAR* Spelling : Spellings)
    {
        PinWrightRenderCapture::FViewModePin Pin;
        FString ErrCode;
        FString ErrMsg;
        if (!OrthoVmParsePin(Spelling, Pin, ErrCode, ErrMsg) || !Pin.WantsOverride())
        {
            // Not a skip that hides an assertion: a mode this build refuses (a ray-tracing gate,
            // a future removal) is reported and moved past, and the counter below proves the loop
            // still measured a substantial set.
            AddInfo(FString::Printf(TEXT("viewMode '%s' is not offered on this build (%s) - not measured"),
                Spelling, *ErrCode));
            continue;
        }
        FString ReachCode;
        FString ReachMessage;
        if (!IsViewModeReachableOnSceneCapture(Pin.ViewMode, ReachCode, ReachMessage))
        {
            AddError(FString::Printf(
                TEXT("viewMode '%s' is in the show-flag family this verb claims to carry, but the "
                     "reach gate refuses it with %s"), Spelling, *ReachCode));
            continue;
        }

        FEngineShowFlags Flags = MakeBaseCaptureShowFlags();
        const FViewModePlan Plan = ApplyViewModeToCaptureShowFlags(Pin, Flags);
        ++Measured;

        // (a) the plan's list is the vocabulary's own measurement over the SAME flags, recomputed
        //     here rather than trusted.
        const TArray<FString> Independent = PinWrightViewModes::MeasureShowFlagMismatches(
            Flags, Pin.ViewMode, /*bPerspective=*/false);
        TestEqual(FString::Printf(TEXT("'%s': the reported mismatch count is the measured one"), Spelling),
            Plan.ShowFlagMismatches.Num(), Independent.Num());
        for (const FString& Name : Independent)
        {
            TestTrue(FString::Printf(TEXT("'%s': the report names mismatched flag '%s'"), Spelling, *Name),
                Plan.ShowFlagMismatches.Contains(Name));
        }

        // (b) bApplied is derived from that list, not asserted separately.
        TestTrue(FString::Printf(TEXT("'%s': applied is exactly 'no flag went missing'"), Spelling),
            Plan.bApplied == (Plan.ShowFlagMismatches.Num() == 0));
        TestTrue(FString::Printf(TEXT("'%s': the applied key is present exactly when applied"), Spelling),
            !Plan.AppliedKey.IsEmpty() == Plan.bApplied);

        // (c) THE ANSWER to the GAP row, asserted rather than logged: this family's distinguishing
        //     flags do survive on a GAME-initialised set. If a future engine starts stripping one,
        //     this fails and names it instead of a capture silently rendering near-Lit.
        TestTrue(FString::Printf(TEXT("'%s' takes on a game-set base (mismatched: %s)"),
                Spelling, *FString::Join(Plan.ShowFlagMismatches, TEXT(", "))),
            Plan.bApplied);
        TestTrue(FString::Printf(TEXT("'%s' distinguishes itself from Lit by at least one flag"), Spelling),
            Plan.DistinguishingShowFlags.Num() > 0);

        AddInfo(FString::Printf(TEXT("MEASURED '%s': %d distinguishing flag(s) [%s], %d did not take [%s], derived back as '%s'"),
            Spelling, Plan.DistinguishingShowFlags.Num(),
            *FString::Join(Plan.DistinguishingShowFlags, TEXT(", ")),
            Plan.ShowFlagMismatches.Num(), *FString::Join(Plan.ShowFlagMismatches, TEXT(", ")),
            *Plan.DerivedKey));

        // (d) the report is a field on the wire, not only an internal fact. Emitted even when
        //     empty: an absent array reads as "not checked".
        const TSharedPtr<FJsonObject> Info = MakeViewModeInfoObject(Plan);
        const TArray<TSharedPtr<FJsonValue>>* Mismatches = nullptr;
        TestTrue(FString::Printf(TEXT("'%s': the response block carries showFlagMismatches"), Spelling),
            Info.IsValid() && Info->TryGetArrayField(TEXT("showFlagMismatches"), Mismatches));
        const TArray<TSharedPtr<FJsonValue>>* Distinguishing = nullptr;
        TestTrue(FString::Printf(TEXT("'%s': the response block carries distinguishingShowFlags"), Spelling),
            Info.IsValid() && Info->TryGetArrayField(TEXT("distinguishingShowFlags"), Distinguishing));
    }

    // A loop that measured nothing is a green test that proved nothing. Half the table is the
    // floor: fewer than that and something is refusing modes this verb claims to carry.
    TestTrue(FString::Printf(TEXT("the walk measured a substantial set of modes (%d of %d)"),
            Measured, static_cast<int32>(UE_ARRAY_COUNT(Spellings))),
        Measured >= static_cast<int32>(UE_ARRAY_COUNT(Spellings)) / 2);
    return true;
}
