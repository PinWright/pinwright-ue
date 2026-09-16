// Copyright (c) 2026 Alexander Penkin. MIT License.

// Tests for the scoped `viewMode` capture parameter: the vocabulary it accepts, the refusals it
// makes instead of rendering something else, and the applied-and-restored property on both of the
// viewport client's view-mode slots.
//
// WHAT THIS DEFENDS. Flipped faces and inside-out shells are invisible to every other signal a
// capture carries: a closed shell built inside-out renders IDENTICALLY to a correct one under
// normal lighting, and one shipped in this corpus and survived two investigations.
// VMI_FrontBackFace is the engine's instrument for exactly that, and it is the visual counterpart
// to the numeric health.signedVolume test. Reaching it must not cost the caller a persistent
// editor.set_view_mode that leaks into every later capture and into what the user is looking at.
//
// WHY MOST OF IT IS ASSERTED WITHOUT A VIEWPORT. A test that captures real pixels needs a GPU, and
// a capture test that cannot get one takes a conditional-skip path and reports success WITHOUT
// running its assertions - board ticket B-test-skips-assertions-silently, where
// PinWright.render.capture_asset_preview.PinnedCapturesAreIdentical skipped its only substantive
// assertions in 3 of 3 runs because another project's editor held the GPU, and the suite totals
// looked identical either way. So the vocabulary, the refusals and the report block are asserted
// as pure functions with nothing to skip, and only the two-slot restore needs a viewport client -
// which it drives directly, with no SceneViewport and no readback.
//
// THE FAILURE DIRECTION MATTERS. Asserting that a capture with viewMode succeeded proves nothing:
// that is precisely how camera.orbit_shots shipped a `viewMode` argument documented as
// "informational label echoed back in the result (does not change rendering)" and nobody noticed.
// Every test below asserts either that the mode REACHED the show flags the renderer reads, or that
// an unrenderable request was REFUSED.
#include "Misc/AutomationTest.h"
#include "Compat/EngineVersionCompat.h"
#include "Handlers/Render/CameraShotPlanUtils.h"
#include "Handlers/Render/PreviewViewportCaptureUtils.h"
#include "Handlers/Render/ViewModeVocabulary.h"
#include "Tests/TestUtils.h"
#include "Tests/TestSkipReporting.h"

#include "Dom/JsonObject.h"
#include "EditorViewportClient.h"
#include "Engine/EngineBaseTypes.h"
#include "IAssetViewport.h"
#include "LevelEditor.h"
#include "Misc/ScopeExit.h"
#include "Modules/ModuleManager.h"
#include "ShowFlags.h"
#include "UObject/Class.h"
#include "UObject/ReflectedTypeAccessors.h"

// The debug view mode the scoped-override assertions below are driven through.
//
// FrontBackFace is the mode the parameter was built for, and it is the subject wherever it
// exists - but its enumerator, its show flag and its key all arrived in UE 5.7. On older engines
// the same assertions run through ShaderComplexity, which is present on every supported engine
// and carries the two properties they actually need: it is not a lit mode, and ApplyViewMode
// gives it its own distinguishing show flag (ShowFlags.cpp:398), so "the mode was applied" stays
// measurable rather than assumed. The one test that is about FrontBackFace ITSELF - the spelling
// table - is version-gated instead, because there is no other mode whose spellings it could pin.
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 7, 0)
#define PW_TEST_SUBJECT_VIEW_MODE       VMI_FrontBackFace
#define PW_TEST_SUBJECT_VIEW_MODE_KEY   TEXT("FrontBackFace")
#define PW_TEST_SUBJECT_VIEW_MODE_SNAKE TEXT("front_back_face")
#define PW_TEST_SUBJECT_SHOW_FLAG(Flags) ((Flags).FrontBackFace)
#else
#define PW_TEST_SUBJECT_VIEW_MODE       VMI_ShaderComplexity
#define PW_TEST_SUBJECT_VIEW_MODE_KEY   TEXT("ShaderComplexity")
#define PW_TEST_SUBJECT_VIEW_MODE_SNAKE TEXT("shader_complexity")
#define PW_TEST_SUBJECT_SHOW_FLAG(Flags) ((Flags).ShaderComplexity)
#endif

namespace
{
    // Prefixed for the same reason as every other helper in Tests/Render: anonymous namespaces in
    // one Unity translation unit merge, so a bare name a sibling also uses is a latent ODR clash.
    TSharedPtr<FJsonObject> ViewModeOverridePayload(const TCHAR* Value)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        if (Value)
        {
            Payload->SetStringField(TEXT("viewMode"), Value);
        }
        return Payload;
    }

    // The active level-editor viewport client, or null when this run has no viewport. Same
    // resolution TestSetViewModeProjectionSlots uses, so the two behave alike.
    FEditorViewportClient* ViewModeOverrideTestClient()
    {
        if (!GEditor)
        {
            return nullptr;
        }
        FLevelEditorModule& LevelEditorModule =
            FModuleManager::GetModuleChecked<FLevelEditorModule>("LevelEditor");
        TSharedPtr<IAssetViewport> ActiveViewport = LevelEditorModule.GetFirstActiveViewport();
        if (!ActiveViewport.IsValid())
        {
            return nullptr;
        }
        return &ActiveViewport->GetAssetViewportClient();
    }
}

// ---------------------------------------------------------------------------------------------
// The vocabulary
// ---------------------------------------------------------------------------------------------

// The mode this parameter exists for, in the spelling the brief names it, resolving to the engine
// enumerator and carrying a show flag that can be measured afterwards.
//
// Version-gated as a whole rather than retargeted at PW_TEST_SUBJECT_VIEW_MODE: this test's
// subject IS the four FrontBackFace spellings and their canonical key, and VMI_FrontBackFace
// arrived in UE 5.7. There is no other mode whose spelling table it could pin instead.
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 7, 0)
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FViewModeVocabularyFrontBackFaceTest,
    "PinWright.render.capture_view_mode_override.FrontBackFaceResolvesAndIsMeasurable",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FViewModeVocabularyFrontBackFaceTest::RunTest(const FString& Parameters)
{
    // Snake case is the spelling the parameter is documented with; the other three are the same
    // request. If normalisation regressed, exactly one of these would fail.
    const TCHAR* Spellings[] = { TEXT("front_back_face"), TEXT("FrontBackFace"),
                                 TEXT("frontbackface"), TEXT("VMI_FrontBackFace") };
    for (const TCHAR* Spelling : Spellings)
    {
        const PinWrightViewModes::FViewModeResolution Resolution =
            PinWrightViewModes::Resolve(Spelling);
        if (!TestTrue(FString::Printf(TEXT("'%s' resolves"), Spelling), Resolution.IsValid()))
        {
            AddError(Resolution.ErrorMessage);
            continue;
        }
        TestEqual(FString::Printf(TEXT("'%s' is VMI_FrontBackFace"), Spelling),
            static_cast<int32>(Resolution.ViewMode), static_cast<int32>(VMI_FrontBackFace));
        TestEqual(FString::Printf(TEXT("'%s' reports the canonical key"), Spelling),
            Resolution.Key, FString(TEXT("FrontBackFace")));
    }

    // The measurable part. ApplyViewMode sets EngineShowFlags.FrontBackFace for this mode and for
    // no other (UE 5.8 Runtime/Engine/Private/ShowFlags.cpp:444), so this flag appearing in the
    // distinguishing set is what makes "the mode was applied" checkable rather than assumed.
    const TArray<FString> Flags = PinWrightViewModes::DistinguishingShowFlags(VMI_FrontBackFace);
    TestTrue(TEXT("FrontBackFace is distinguished by its own show flag"),
        Flags.Contains(TEXT("FrontBackFace")));
    return true;
}
#endif // VMI_FrontBackFace (UE 5.7+)

// The round trip that makes a response feed back into a request. A capture reports `viewModeKey`;
// if that spelling is not one the parser accepts, the reported key is decoration.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FViewModeVocabularyRoundTripTest,
    "PinWright.render.capture_view_mode_override.EveryReportedKeyIsAnAcceptedKey",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FViewModeVocabularyRoundTripTest::RunTest(const FString& Parameters)
{
    const TArray<FString> Keys = PinWrightViewModes::RenderableKeys();
    // A vocabulary that silently collapsed to nothing would pass every per-key assertion below,
    // so the size is asserted first. UE 5.8's EViewModeIndex has 48 assigned enumerators; after
    // the sentinels and the two Lit-identical entries there are well over twenty renderable ones.
    TestTrue(TEXT("the reflected vocabulary is populated"), Keys.Num() >= 20);

    for (const FString& Key : Keys)
    {
        const PinWrightViewModes::FViewModeResolution Resolution = PinWrightViewModes::Resolve(Key);
        // A companion mode resolves to a real enumerator but is refused without a target, which is
        // the correct behaviour and not a round-trip break: the KEY still has to map back.
        const bool bRoundTripped = Resolution.Status == PinWrightViewModes::EViewModeStatus::Ok
            || Resolution.Status == PinWrightViewModes::EViewModeStatus::NeedsCompanion
            || Resolution.Status == PinWrightViewModes::EViewModeStatus::Unavailable;
        if (!TestTrue(FString::Printf(TEXT("'%s' parses back to an enumerator"), *Key),
                bRoundTripped))
        {
            continue;
        }
        TestEqual(FString::Printf(TEXT("'%s' round-trips through GetKey"), *Key),
            PinWrightViewModes::GetKey(Resolution.ViewMode), Key);
    }
    return true;
}

// The legacy spellings. These are the wire contract editor.set_view_mode shipped with, and a
// reflection-derived vocabulary that dropped one of them would break stored requests silently.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FViewModeVocabularyLegacyKeysTest,
    "PinWright.render.capture_view_mode_override.LegacyKeysStillResolve",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FViewModeVocabularyLegacyKeysTest::RunTest(const FString& Parameters)
{
    struct FExpect { const TCHAR* Spelling; EViewModeIndex Expected; };
    const FExpect Expectations[] = {
        { TEXT("lit"),                    VMI_Lit },
        { TEXT("unlit"),                  VMI_Unlit },
        // The long-standing quirk, preserved deliberately: "Wireframe" selects VMI_BrushWireframe
        // (the editor's "Wireframe only"), NOT the enumerator literally named VMI_Wireframe.
        { TEXT("wireframe"),              VMI_BrushWireframe },
        { TEXT("CSGWireframe"),           VMI_Wireframe },
        { TEXT("detaillighting"),         VMI_Lit_DetailLighting },
        { TEXT("lightingonly"),           VMI_LightingOnly },
        { TEXT("lightcomplexity"),        VMI_LightComplexity },
        { TEXT("shadercomplexity"),       VMI_ShaderComplexity },
        { TEXT("lightmapdensity"),        VMI_LightmapDensity },
        { TEXT("stationarylightoverlap"), VMI_StationaryLightOverlap },
        { TEXT("reflectionoverride"),     VMI_ReflectionOverride },
        { TEXT("collisionSimple"),        VMI_CollisionPawn },
        { TEXT("collisionComplex"),       VMI_CollisionVisibility },
        // Aliases the old parse chain carried.
        { TEXT("worldcollision"),         VMI_CollisionPawn },
        { TEXT("playercollision"),        VMI_CollisionPawn },
        { TEXT("precisecollision"),       VMI_CollisionVisibility },
        { TEXT("collisionvis"),           VMI_CollisionVisibility },
        // The five modes the widened vocabulary is being bought for. All three enumerators below
        // arrived in UE 5.7; the legacy spellings above them are what this test is actually about
        // and are present on every supported engine.
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 7, 0)
        { TEXT("zebra"),                  VMI_Zebra },
        { TEXT("random_color"),           VMI_RandomColor },
        { TEXT("clay"),                   VMI_Clay },
#endif
    };
    for (const FExpect& Expect : Expectations)
    {
        const PinWrightViewModes::FViewModeResolution Resolution =
            PinWrightViewModes::Resolve(Expect.Spelling);
        if (!TestTrue(FString::Printf(TEXT("'%s' resolves"), Expect.Spelling), Resolution.IsValid()))
        {
            AddError(Resolution.ErrorMessage);
            continue;
        }
        TestEqual(FString::Printf(TEXT("'%s' maps to the historical enumerator"), Expect.Spelling),
            static_cast<int32>(Resolution.ViewMode), static_cast<int32>(Expect.Expected));
    }
    return true;
}

// The refusals. Each of these would otherwise produce a plausible picture of the wrong thing,
// which is worse than an error.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FViewModeVocabularyRefusalsTest,
    "PinWright.render.capture_view_mode_override.UnrenderableModesAreRefusedSpecifically",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FViewModeVocabularyRefusalsTest::RunTest(const FString& Parameters)
{
    struct FRefusal { const TCHAR* Spelling; const TCHAR* ExpectedCode; };
    const FRefusal Refusals[] = {
        // Sentinels: no viewport can be put into these at all.
        { TEXT("VMI_Unknown"),            TEXT("UNKNOWN_VIEW_MODE") },
        { TEXT("VMI_Max"),                TEXT("UNKNOWN_VIEW_MODE") },
        // Not a spelling of anything.
        { TEXT("CollisionSomethingElse"), TEXT("UNKNOWN_VIEW_MODE") },
        // A menu grouping item, not a render mode: ApplyViewMode has no case for it, so it would
        // render exactly as Lit while the response called it GroupLODColoration.
        { TEXT("GroupLODColoration"),     TEXT("VIEW_MODE_NOT_RENDERABLE") },
        // Deprecated in 5.8 in favour of Lit + the MeshEdges show flag; likewise Lit-identical.
        // Only from 5.8: before that the enumerator carries no UMETA(Hidden) and ApplyViewMode
        // does have a case for it (5.6 Runtime/Engine/Private/ShowFlags.cpp:314 sets MeshEdges),
        // so on those engines it is a renderable mode and refusing it would be the bug.
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 8, 0)
        { TEXT("Lit_Wireframe"),          TEXT("UNKNOWN_VIEW_MODE") },
#endif
        // Needs a sub-visualisation the mode name does not carry. Refused rather than rendered as
        // whatever overview default the viewport happens to hold.
        { TEXT("VisualizeBuffer"),        TEXT("VIEW_MODE_NEEDS_COMPANION") },
        { TEXT("VisualizeLumen"),         TEXT("VIEW_MODE_NEEDS_COMPANION") },
    };
    for (const FRefusal& Refusal : Refusals)
    {
        const PinWrightViewModes::FViewModeResolution Resolution =
            PinWrightViewModes::Resolve(Refusal.Spelling);
        TestFalse(FString::Printf(TEXT("'%s' is refused"), Refusal.Spelling), Resolution.IsValid());
        TestEqual(FString::Printf(TEXT("'%s' refusal code"), Refusal.Spelling),
            Resolution.ErrorCode, FString(Refusal.ExpectedCode));
        // A refusal that does not say what is wrong sends the caller back to guessing.
        TestTrue(FString::Printf(TEXT("'%s' refusal explains itself"), Refusal.Spelling),
            Resolution.ErrorMessage.Len() > 40);
    }
    return true;
}

// Every value the reflected enum carries, run through the vocabulary's display-name accessor.
//
// WHAT THIS DEFENDS, and it is not cosmetic. UViewModeUtils::GetViewModeDisplayName is a raw
// indexed read of a table sized VMI_Unknown + 1 (UE 5.8 Runtime/Engine/Private/ViewModeNames.cpp:10
// fills indices 0..VMI_Unknown; :262 indexes with no bound of its own), and this vocabulary walks
// StaticEnum<EViewModeIndex>(), which carries UHT's synthetic terminator at VMI_Unknown + 1. Calling
// the engine helper with it is a HARD ASSERT that takes the whole automation process down mid-run,
// not a test failure - one such run reported 3423 started / 3421 passed / 1 failed, counts that do
// not reconcile because the test that crashed never completed. Inside the bound the same helper
// ensures on any index the engine left empty, which today is VMI_VisualizeSubstrate (34) and
// VMI_VisualizeGroom (35), both of which this file offers as renderable modes.
//
// So: every reflected value, every one gets a NON-EMPTY name, and the run survives. Asserting
// non-emptiness is what makes it a real check - the crash-avoidance alone would be satisfied by
// returning nothing.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FViewModeDisplayNameIsSafeForEveryEnumValueTest,
    "PinWright.render.capture_view_mode_override.DisplayNameIsSafeForEveryEnumValue",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FViewModeDisplayNameIsSafeForEveryEnumValueTest::RunTest(const FString& Parameters)
{
    const UEnum* Enum = StaticEnum<EViewModeIndex>();
    if (!TestNotNull(TEXT("EViewModeIndex is reflected"), Enum))
    {
        return true;
    }
    // A walk that silently found nothing would pass every per-value assertion below.
    TestTrue(TEXT("the reflected enum is populated"), Enum->NumEnums() > 20);

    bool bSawOutOfTableValue = false;
    for (int32 Index = 0; Index < Enum->NumEnums(); ++Index)
    {
        const int64 Value = Enum->GetValueByIndex(Index);
        const EViewModeIndex ViewMode = static_cast<EViewModeIndex>(Value);
        if (Value > static_cast<int64>(VMI_Unknown))
        {
            bSawOutOfTableValue = true;
        }
        TestTrue(FString::Printf(TEXT("'%s' (%lld) has a display name"),
                *Enum->GetNameStringByIndex(Index), Value),
            !PinWrightViewModes::GetDisplayName(ViewMode).IsEmpty());
    }
    // The value that caused the crash has to still be in the walk, or this test stopped covering
    // it and would go green against a reintroduced raw call.
    TestTrue(TEXT("the walk still reaches a value past the engine's display-name table"),
        bSawOutOfTableValue);

    // The two modes the engine's own table has no branch for, named explicitly: if a later engine
    // fills them in, this keeps passing; if the fallback is removed, it fails here rather than
    // leaving an ensure to be noticed by eye in a log.
    TestTrue(TEXT("VMI_VisualizeSubstrate has a display name"),
        !PinWrightViewModes::GetDisplayName(VMI_VisualizeSubstrate).IsEmpty());
    TestTrue(TEXT("VMI_VisualizeGroom has a display name"),
        !PinWrightViewModes::GetDisplayName(VMI_VisualizeGroom).IsEmpty());

    // And the spelling that took the process down, end to end through the parser. It is a
    // sentinel either way, so the assertion is on the refusal, not on the mode.
    const PinWrightViewModes::FViewModeResolution MaxResolution =
        PinWrightViewModes::Resolve(TEXT("VMI_Max"));
    TestFalse(TEXT("'VMI_Max' is refused"), MaxResolution.IsValid());
    TestTrue(TEXT("'VMI_Max' resolves inside the engine's display-name table"),
        static_cast<int32>(MaxResolution.ViewMode) <= static_cast<int32>(VMI_Unknown));
    return true;
}

// The parse path the capture verbs actually run, including the do-nothing default. An omitted
// parameter must write nothing, or the omitted-parameter path is not the pre-existing behaviour.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FViewModePinParseTest,
    "PinWright.render.capture_view_mode_override.PinParsesAndDefaultsToNoOverride",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FViewModePinParseTest::RunTest(const FString& Parameters)
{
    PinWrightRenderCapture::FViewModePin Pin;
    FString ErrorCode;
    FString ErrorMessage;

    TestTrue(TEXT("an empty payload parses"), PinWrightRenderCapture::ParseViewModePin(
        ViewModeOverridePayload(nullptr), Pin, ErrorCode, ErrorMessage));
    TestFalse(TEXT("an omitted viewMode requests no override"), Pin.WantsOverride());

    TestTrue(TEXT("a snake_case mode name parses"), PinWrightRenderCapture::ParseViewModePin(
        ViewModeOverridePayload(PW_TEST_SUBJECT_VIEW_MODE_SNAKE), Pin, ErrorCode, ErrorMessage));
    TestTrue(TEXT("it requests an override"), Pin.WantsOverride());
    TestEqual(TEXT("the pin carries the enumerator"), static_cast<int32>(Pin.ViewMode),
        static_cast<int32>(PW_TEST_SUBJECT_VIEW_MODE));
    TestTrue(TEXT("the pin carries something to measure"), Pin.DistinguishingShowFlags.Num() > 0);

    TestFalse(TEXT("an unknown mode fails the parse"), PinWrightRenderCapture::ParseViewModePin(
        ViewModeOverridePayload(TEXT("not_a_view_mode")), Pin, ErrorCode, ErrorMessage));
    TestEqual(TEXT("with the registered code"), ErrorCode, FString(TEXT("UNKNOWN_VIEW_MODE")));
    TestFalse(TEXT("and a failed parse leaves no override on the pin"), Pin.WantsOverride());

    // A non-string viewMode has no do-nothing reading: the caller asked for a diagnostic view and
    // would otherwise get an ordinary capture that looks like the one they asked for.
    TSharedPtr<FJsonObject> Numeric = MakeShared<FJsonObject>();
    Numeric->SetNumberField(TEXT("viewMode"), 42.0);
    TestFalse(TEXT("a non-string viewMode fails rather than being ignored"),
        PinWrightRenderCapture::ParseViewModePin(Numeric, Pin, ErrorCode, ErrorMessage));
    return true;
}

// ---------------------------------------------------------------------------------------------
// The report block
// ---------------------------------------------------------------------------------------------

// The block has to distinguish "no override was asked for" from "an override did nothing", which
// an absent block cannot. It also has to WARN when the restore did not read back, because that
// capture changed the editor window the user is looking at.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FViewModeOverrideReportTest,
    "PinWright.render.capture_view_mode_override.ReportSaysWhatWasMeasured",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FViewModeOverrideReportTest::RunTest(const FString& Parameters)
{
    // No override requested: the block still exists and says so.
    {
        PinWrightRenderCapture::FViewportCaptureOutput Capture;
        Capture.ViewModePerspBeforeKey = TEXT("Lit");
        Capture.ViewModeOrthoBeforeKey = TEXT("Lit");
        Capture.ViewModePerspAfterKey = TEXT("Lit");
        Capture.ViewModeOrthoAfterKey = TEXT("Lit");
        const TSharedPtr<FJsonObject> Block =
            PinWrightRenderCapture::MakeViewModeOverrideInfoObject(Capture);
        bool bRequested = true;
        TestTrue(TEXT("`requested` is present"), Block->TryGetBoolField(TEXT("requested"), bRequested));
        TestFalse(TEXT("an omitted parameter reports requested:false"), bRequested);
        TestFalse(TEXT("and carries no restoreWarning"), Block->HasField(TEXT("restoreWarning")));
    }

    // Applied and restored: both slots back where they were, nothing to warn about.
    {
        PinWrightRenderCapture::FViewportCaptureOutput Capture;
        Capture.bViewModeOverrideRequested = true;
        Capture.ViewModeRequestedKey = PW_TEST_SUBJECT_VIEW_MODE_KEY;
        Capture.ViewModeRequestedValue = static_cast<int32>(PW_TEST_SUBJECT_VIEW_MODE);
        Capture.ViewModeShowFlags = { PW_TEST_SUBJECT_VIEW_MODE_KEY };
        Capture.bViewModeApplied = true;
        Capture.bViewModeRestored = true;
        Capture.ViewModePerspBeforeKey = TEXT("Lit");
        Capture.ViewModeOrthoBeforeKey = TEXT("Unlit");
        Capture.ViewModePerspAfterKey = TEXT("Lit");
        Capture.ViewModeOrthoAfterKey = TEXT("Unlit");
        const TSharedPtr<FJsonObject> Block =
            PinWrightRenderCapture::MakeViewModeOverrideInfoObject(Capture);
        bool bApplied = false;
        Block->TryGetBoolField(TEXT("applied"), bApplied);
        TestTrue(TEXT("applied:true"), bApplied);
        TestFalse(TEXT("no applyWarning on a clean apply"), Block->HasField(TEXT("applyWarning")));
        TestFalse(TEXT("no restoreWarning on a clean restore"),
            Block->HasField(TEXT("restoreWarning")));

        // BOTH slots are reported, before and after. One number cannot describe the restore.
        const TSharedPtr<FJsonObject>* Previous = nullptr;
        if (TestTrue(TEXT("`previous` is present"),
                Block->TryGetObjectField(TEXT("previous"), Previous) && Previous))
        {
            FString Persp;
            FString Ortho;
            (*Previous)->TryGetStringField(TEXT("perspective"), Persp);
            (*Previous)->TryGetStringField(TEXT("orthographic"), Ortho);
            TestEqual(TEXT("previous perspective slot"), Persp, FString(TEXT("Lit")));
            TestEqual(TEXT("previous orthographic slot"), Ortho, FString(TEXT("Unlit")));
        }
    }

    // Not restored: the warning must fire and must name both slots, because a capture that leaves
    // a debug mode on the viewport changes what the user sees, not just the next capture.
    {
        PinWrightRenderCapture::FViewportCaptureOutput Capture;
        Capture.bViewModeOverrideRequested = true;
        Capture.ViewModeRequestedKey = PW_TEST_SUBJECT_VIEW_MODE_KEY;
        Capture.bViewModeApplied = true;
        Capture.bViewModeRestored = false;
        Capture.ViewModePerspBeforeKey = TEXT("Lit");
        Capture.ViewModeOrthoBeforeKey = TEXT("Lit");
        Capture.ViewModePerspAfterKey = PW_TEST_SUBJECT_VIEW_MODE_KEY;
        Capture.ViewModeOrthoAfterKey = PW_TEST_SUBJECT_VIEW_MODE_KEY;
        const TSharedPtr<FJsonObject> Block =
            PinWrightRenderCapture::MakeViewModeOverrideInfoObject(Capture);
        FString Warning;
        if (TestTrue(TEXT("a failed restore warns"),
                Block->TryGetStringField(TEXT("restoreWarning"), Warning)))
        {
            TestTrue(TEXT("the warning names the mode left behind"),
                Warning.Contains(PW_TEST_SUBJECT_VIEW_MODE_KEY));
        }
    }

    // Applied:false with a show-flag mismatch: the exact shape of the defect this closes, where a
    // parameter looks like it worked because the call succeeded.
    {
        PinWrightRenderCapture::FViewportCaptureOutput Capture;
        Capture.bViewModeOverrideRequested = true;
        Capture.ViewModeRequestedKey = PW_TEST_SUBJECT_VIEW_MODE_KEY;
        Capture.ViewModeShowFlags = { PW_TEST_SUBJECT_VIEW_MODE_KEY, TEXT("Lighting") };
        Capture.ViewModeShowFlagMismatches = { PW_TEST_SUBJECT_VIEW_MODE_KEY };
        Capture.bViewModeApplied = false;
        Capture.bViewModeRestored = true;
        const TSharedPtr<FJsonObject> Block =
            PinWrightRenderCapture::MakeViewModeOverrideInfoObject(Capture);
        FString Warning;
        TestTrue(TEXT("a mode that did not take warns"),
            Block->TryGetStringField(TEXT("applyWarning"), Warning));
        TestTrue(TEXT("the mismatching flags are listed"),
            Block->HasField(TEXT("showFlagMismatches")));
    }
    return true;
}

// A non-lit mode the CALLER asked for must not be reported with the standing "the viewport was
// left this way, call editor.set_view_mode to fix it" remedy: the premise is false and the remedy
// is the persistent verb this parameter exists to avoid.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FViewModeOverrideWarningIsScopedTest,
    "PinWright.render.capture_view_mode_override.RequestedDebugModeWarnsAboutItself",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FViewModeOverrideWarningIsScopedTest::RunTest(const FString& Parameters)
{
    PinWrightRenderCapture::FViewportCaptureOutput Capture;
    Capture.ViewportType = TEXT("perspective");
    Capture.ViewModeValue = static_cast<int32>(PW_TEST_SUBJECT_VIEW_MODE);
    Capture.ViewModeKey = PinWrightRenderCapture::GetViewModeKey(PW_TEST_SUBJECT_VIEW_MODE);
    Capture.ViewMode = TEXT("Front/Back Face");
    Capture.bLitViewMode = false;
    Capture.bViewModeOverrideRequested = true;
    Capture.bViewModeApplied = true;
    Capture.bViewModeRestored = true;
    Capture.ViewModeRequestedKey = PW_TEST_SUBJECT_VIEW_MODE_KEY;

    const TSharedPtr<FJsonObject> Viewport =
        PinWrightRenderCapture::MakeViewportInfoObject(Capture);
    FString Warning;
    if (TestTrue(TEXT("a non-lit capture still warns"),
            Viewport->TryGetStringField(TEXT("viewModeWarning"), Warning)))
    {
        TestTrue(TEXT("the warning names the mode"), Warning.Contains(PW_TEST_SUBJECT_VIEW_MODE_KEY));
        // The whole point of the scoped parameter: the caller is NOT told to go and run the
        // persistent verb, and is NOT told the viewport was left in a debug mode.
        TestFalse(TEXT("it does not send the caller to editor.set_view_mode"),
            Warning.Contains(TEXT("editor.set_view_mode")));
        TestTrue(TEXT("it says the mode was requested by this call"),
            Warning.Contains(TEXT("this call")));
    }
    return true;
}

// ---------------------------------------------------------------------------------------------
// The two-slot restore, on a real viewport client
// ---------------------------------------------------------------------------------------------

// THE REGRESSION TEST. Set both slots to two DIFFERENT known modes, run the scope with a third,
// and assert both slots come back. Without the restore both slots hold the override; without the
// two-slot save one of them does. Needs a viewport client but no SceneViewport, no GPU and no
// readback, so it cannot go quiet the way a pixel test can.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FViewModeOverrideRestoresBothSlotsTest,
    "PinWright.render.capture_view_mode_override.ScopeRestoresBothProjectionSlots",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FViewModeOverrideRestoresBothSlotsTest::RunTest(const FString& Parameters)
{
    FEditorViewportClient* Client = ViewModeOverrideTestClient();
    if (!Client)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-level-viewport"),
            TEXT("Skipped: no active level viewport, so neither view-mode slot is readable."));
        return true;
    }

    const EViewModeIndex EntryPersp = Client->GetPerspViewMode();
    const EViewModeIndex EntryOrtho = Client->GetOrthoViewMode();
    ON_SCOPE_EXIT
    {
        // Global viewport state this test changed, put back on every exit path including a failed
        // assertion - the same discipline the code under test is asserting.
        Client->SetViewModes(EntryPersp, EntryOrtho);
    };

    // TWO DIFFERENT modes, deliberately. If the slots started equal, a scope that saved only one
    // and wrote it to both would still "restore" correctly and the test would pass.
    Client->SetViewModes(VMI_Unlit, VMI_Lit);
    if (!TestEqual(TEXT("the perspective slot starts at Unlit"),
            static_cast<int32>(Client->GetPerspViewMode()), static_cast<int32>(VMI_Unlit)))
    {
        return true;
    }
    TestEqual(TEXT("the orthographic slot starts at Lit"),
        static_cast<int32>(Client->GetOrthoViewMode()), static_cast<int32>(VMI_Lit));

    PinWrightRenderCapture::FViewModePin Pin;
    Pin.bRequested = true;
    Pin.ViewMode = PW_TEST_SUBJECT_VIEW_MODE;
    Pin.Key = PW_TEST_SUBJECT_VIEW_MODE_KEY;
    Pin.DistinguishingShowFlags = PinWrightViewModes::DistinguishingShowFlags(PW_TEST_SUBJECT_VIEW_MODE);

    {
        PinWrightRenderCapture::FScopedViewModeOverride Scope(*Client, Pin);
        TestTrue(TEXT("the scope applied"), Scope.WasApplied());

        // APPLIED, not echoed - both slots, because a mode set only for perspective leaves every
        // orthographic capture rendering the old one.
        TestEqual(TEXT("the perspective slot holds the override"),
            static_cast<int32>(Client->GetPerspViewMode()),
            static_cast<int32>(PW_TEST_SUBJECT_VIEW_MODE));
        TestEqual(TEXT("the orthographic slot holds the override"),
            static_cast<int32>(Client->GetOrthoViewMode()),
            static_cast<int32>(PW_TEST_SUBJECT_VIEW_MODE));

        // The measurement the slot fields cannot fake: the show flags the renderer reads.
        const TArray<FString> Mismatches = PinWrightViewModes::MeasureShowFlagMismatches(
            Client->EngineShowFlags, PW_TEST_SUBJECT_VIEW_MODE, Client->IsPerspective());
        TestEqual(TEXT("every distinguishing show flag reached the client"), Mismatches.Num(), 0);
        TestTrue(TEXT("the subject mode's own show flag is set while the scope is open"),
            PW_TEST_SUBJECT_SHOW_FLAG(Client->EngineShowFlags) != 0);
    }

    // The contract. Both slots, independently.
    TestEqual(TEXT("the perspective slot was restored"),
        static_cast<int32>(Client->GetPerspViewMode()), static_cast<int32>(VMI_Unlit));
    TestEqual(TEXT("the orthographic slot was restored"),
        static_cast<int32>(Client->GetOrthoViewMode()), static_cast<int32>(VMI_Lit));
    TestTrue(TEXT("the subject mode's show flag was cleared with it"),
        PW_TEST_SUBJECT_SHOW_FLAG(Client->EngineShowFlags) == 0);
    return true;
}

// The omitted-parameter path writes NOTHING. Without this, a default-constructed pin that quietly
// wrote VMI_Lit would move every existing caller's pixels.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FViewModeOverrideOmittedWritesNothingTest,
    "PinWright.render.capture_view_mode_override.OmittedParameterWritesNothing",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FViewModeOverrideOmittedWritesNothingTest::RunTest(const FString& Parameters)
{
    FEditorViewportClient* Client = ViewModeOverrideTestClient();
    if (!Client)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-level-viewport"),
            TEXT("Skipped: no active level viewport, so neither view-mode slot is readable."));
        return true;
    }

    const EViewModeIndex EntryPersp = Client->GetPerspViewMode();
    const EViewModeIndex EntryOrtho = Client->GetOrthoViewMode();
    ON_SCOPE_EXIT
    {
        Client->SetViewModes(EntryPersp, EntryOrtho);
    };

    Client->SetViewModes(VMI_Unlit, VMI_ShaderComplexity);
    {
        const PinWrightRenderCapture::FViewModePin Unset;
        PinWrightRenderCapture::FScopedViewModeOverride Scope(*Client, Unset);
        TestFalse(TEXT("an unset pin applies nothing"), Scope.WasApplied());
        TestEqual(TEXT("the perspective slot is untouched inside the scope"),
            static_cast<int32>(Client->GetPerspViewMode()), static_cast<int32>(VMI_Unlit));
        TestEqual(TEXT("the orthographic slot is untouched inside the scope"),
            static_cast<int32>(Client->GetOrthoViewMode()),
            static_cast<int32>(VMI_ShaderComplexity));
    }
    TestEqual(TEXT("the perspective slot is untouched after the scope"),
        static_cast<int32>(Client->GetPerspViewMode()), static_cast<int32>(VMI_Unlit));
    TestEqual(TEXT("the orthographic slot is untouched after the scope"),
        static_cast<int32>(Client->GetOrthoViewMode()), static_cast<int32>(VMI_ShaderComplexity));
    return true;
}

// ---------------------------------------------------------------------------------------------
// camera.orbit_shots shot distribution
// ---------------------------------------------------------------------------------------------

// The property `ring` fails and `sphere` is being bought: N shots that are actually spread over
// the sphere rather than sampling one horizontal circle.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FShotDistributionSphereCoversTheSphereTest,
    "PinWright.camera.orbit_shots.SphereDistributionSpreadsOverElevation",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FShotDistributionSphereCoversTheSphereTest::RunTest(const FString& Parameters)
{
    constexpr int32 Count = 8;
    const TArray<PinWrightCameraFrame::FPlannedShot> Sphere =
        PinWrightCameraFrame::MakeSphereDistribution(Count, 0.0, TEXT("perspective"));
    TestEqual(TEXT("sphere produces the requested number of shots"), Sphere.Num(), Count);

    // The point of the mode. A ring set has ONE elevation; if this passed for a ring set the mode
    // would be buying nothing.
    TSet<int32> Elevations;
    for (const PinWrightCameraFrame::FPlannedShot& Shot : Sphere)
    {
        Elevations.Add(FMath::RoundToInt(Shot.Elevation));
    }
    TestTrue(TEXT("sphere shots do not share one elevation"), Elevations.Num() > 1);

    // Distinct poses. A construction that clumped - which is what a random mode does - would fail
    // here, and coverage is the only thing the mode is asked for.
    TSet<FString> Poses;
    for (const PinWrightCameraFrame::FPlannedShot& Shot : Sphere)
    {
        Poses.Add(FString::Printf(TEXT("%.3f_%.3f"), Shot.Azimuth, Shot.Elevation));
    }
    TestEqual(TEXT("every sphere pose is distinct"), Poses.Num(), Count);

    // It must reach both hemispheres, because the underside is exactly where the defects this is
    // for have hidden.
    bool bAbove = false;
    bool bBelow = false;
    for (const PinWrightCameraFrame::FPlannedShot& Shot : Sphere)
    {
        bAbove |= Shot.Elevation > 5.0f;
        bBelow |= Shot.Elevation < -5.0f;
    }
    TestTrue(TEXT("sphere reaches above the horizon"), bAbove);
    TestTrue(TEXT("sphere reaches below the horizon"), bBelow);

    // Every azimuth stays in [0, 360) so the value can be fed straight back through `angles`.
    for (const PinWrightCameraFrame::FPlannedShot& Shot : Sphere)
    {
        TestTrue(TEXT("azimuth is normalised"), Shot.Azimuth >= 0.0f && Shot.Azimuth < 360.0f);
    }
    return true;
}

// `ring` is the default, and its poses must be byte-identical to the expression it replaced, or
// every archived comparison shot moves.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FShotDistributionRingIsUnchangedTest,
    "PinWright.camera.orbit_shots.RingDistributionMatchesThePreviousExpression",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FShotDistributionRingIsUnchangedTest::RunTest(const FString& Parameters)
{
    for (int32 Count = 1; Count <= 12; ++Count)
    {
        const TArray<PinWrightCameraFrame::FPlannedShot> Ring =
            PinWrightCameraFrame::MakeRingDistribution(Count, 30.0f, 0.0, TEXT("perspective"));
        TestEqual(TEXT("ring produces the requested number of shots"), Ring.Num(), Count);
        for (int32 Index = 0; Index < Count; ++Index)
        {
            // The literal expression camera.orbit_shots used before the distribution parameter
            // existed, in single precision - a double computation narrowed afterwards can differ
            // by one ulp, and one ulp of degrees is a different camera and a different PNG.
            const float Expected = (360.0f * Index) / static_cast<float>(Count);
            TestEqual(FString::Printf(TEXT("ring azimuth %d/%d"), Index, Count),
                Ring[Index].Azimuth, Expected);
            TestEqual(TEXT("ring elevation is the fixed one"), Ring[Index].Elevation, 30.0f);
        }
    }
    return true;
}

// The seed contract: deterministic without one, deterministic WITH one, and different seeds give
// different sets. Without this a "jitter" parameter is indistinguishable from an unreproducible
// random mode.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FShotDistributionSeedIsReproducibleTest,
    "PinWright.camera.orbit_shots.SeededDistributionIsReproducible",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FShotDistributionSeedIsReproducibleTest::RunTest(const FString& Parameters)
{
    const double OffsetA = PinWrightCameraFrame::SeedToAzimuthOffsetDegrees(1234);
    const double OffsetB = PinWrightCameraFrame::SeedToAzimuthOffsetDegrees(1234);
    TestEqual(TEXT("the same seed gives the same offset"), OffsetA, OffsetB);
    TestNotEqual(TEXT("a different seed gives a different offset"), OffsetA,
        PinWrightCameraFrame::SeedToAzimuthOffsetDegrees(4321));

    const TArray<PinWrightCameraFrame::FPlannedShot> First =
        PinWrightCameraFrame::MakeSphereDistribution(6, OffsetA, TEXT("perspective"));
    const TArray<PinWrightCameraFrame::FPlannedShot> Second =
        PinWrightCameraFrame::MakeSphereDistribution(6, OffsetB, TEXT("perspective"));
    if (TestEqual(TEXT("both runs produce the same count"), First.Num(), Second.Num()))
    {
        for (int32 Index = 0; Index < First.Num(); ++Index)
        {
            TestEqual(TEXT("seeded azimuth reproduces"), First[Index].Azimuth,
                Second[Index].Azimuth);
            TestEqual(TEXT("seeded elevation reproduces"), First[Index].Elevation,
                Second[Index].Elevation);
        }
    }

    // No seed means offset zero, which is the unjittered construction - not "some other random
    // set". This is what makes an omitted seed a contract rather than an absence.
    const TArray<PinWrightCameraFrame::FPlannedShot> Unseeded =
        PinWrightCameraFrame::MakeSphereDistribution(6, 0.0, TEXT("perspective"));
    TestEqual(TEXT("the unseeded first azimuth is the construction's own"), Unseeded[0].Azimuth,
        0.0f);
    return true;
}
