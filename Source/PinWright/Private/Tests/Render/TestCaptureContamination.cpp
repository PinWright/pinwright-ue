// Copyright (c) 2026 Alexander Penkin. MIT License.

// Tests for the two contamination facts a capture response has to disclose about a frame it
// already measured: game view being OFF, and a ShowFlag.* console variable forcing a show flag.
//
// WHAT THIS DEFENDS. Both are review hazards rather than outages: the capture succeeds, the PNG is
// a plausible picture, and every honesty field a reviewer is told to trust reads clean. A capture
// taken with game view off came back with the world-axis gizmo drawn into it, `gameView: false`
// published in the response and no warning anywhere -- while the neighbouring verb raised
// `overlayWarning` for a strictly milder condition. A capture taken with one ShowFlag.* cvar
// forced ON came back two-thirds covered by a full-screen debug visualization with `gameView:
// true`, `editorSprites` clean and a perfectly ordinary `meanLuminance`. Neither measurement was
// missing; only the warning was.
//
// WHY BOTH ARE ASSERTED WITHOUT A GPU. The disclosure is a pure function of FViewportCaptureOutput
// (MakeViewportInfoObject / MakeShowFlagOverrideInfoObject) and the survey is a pure read of the
// console registry, so neither needs a viewport, a world or a device. A capture test that cannot
// get a GPU takes a conditional-skip path and reports success WITHOUT running its assertions --
// board ticket B-test-skips-assertions-silently -- which is exactly what these two facts must not
// be defended by.
#include "Misc/AutomationTest.h"
#include "Handlers/Render/PreviewViewportCaptureUtils.h"
#include "Tests/TestSkipReporting.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/IConsoleManager.h"
#include "ShowFlags.h"

namespace
{
    // Prefixed for the same reason as every other helper in Tests/Render: anonymous namespaces in
    // one Unity translation unit merge, so a bare name a sibling also uses is a latent ODR clash.

    // Picks a probe flag off the LIVE registry instead of hardcoding one, for two reasons. The
    // flag set differs by engine version (VisualizeLightFunctionAtlas, the flag that produced the
    // reported defect, does not exist before UE 5.5), and a flag another agent has already forced
    // in this shared editor would make the before/after assertions meaningless. Restricted to the
    // Visualize* family and forced to 0 below, so the probe is inert: those flags are off by
    // default, and forcing one OFF changes no pixel while still being a genuine override.
    struct FPwContaminationProbeFlagSink
    {
        FString Chosen;

        void Consider(const FString& Name)
        {
            if (!Chosen.IsEmpty() || !Name.StartsWith(TEXT("Visualize")))
            {
                return;
            }
            const IConsoleVariable* CVar = IConsoleManager::Get().FindConsoleVariable(
                *FString::Printf(TEXT("ShowFlag.%s"), *Name));
            if (CVar && CVar->GetInt() == 2)
            {
                Chosen = Name;
            }
        }

        bool OnEngineShowFlag(uint32 /*Index*/, const FString& Name) { Consider(Name); return true; }
        bool OnCustomShowFlag(uint32 /*Index*/, const FString& Name) { Consider(Name); return true; }
    };

    const PinWrightRenderCapture::FForcedShowFlagOverride* PwContaminationFind(
        const TArray<PinWrightRenderCapture::FForcedShowFlagOverride>& Found, const FString& Name)
    {
        return Found.FindByPredicate(
            [&Name](const PinWrightRenderCapture::FForcedShowFlagOverride& Entry)
            {
                return Entry.Name == Name;
            });
    }
}

// ============================================================================
// Game view is per-viewport state the capture does not own, and a capture drawn without it says so.
//
// The counterfactual: before this, a level capture published `gameView: false` beside a frame with
// editor chrome in it and returned success with no warning field of any kind.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCaptureContaminationGameViewTest,
    "PinWright.render.capture_contamination.GameViewOffOnALevelCaptureCarriesAWarning",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCaptureContaminationGameViewTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightRenderCapture;

    // A level-editor capture (no preview scene behind the client) drawn with game view off.
    {
        FViewportCaptureOutput Capture;
        Capture.bGameView = false;
        Capture.PreviewSceneRigDrawn.bSceneAvailable = false;
        const TSharedPtr<FJsonObject> Viewport = MakeViewportInfoObject(Capture);

        TestFalse(TEXT("the measurement itself is still published"),
            Viewport->GetBoolField(TEXT("gameView")));
        TestTrue(TEXT("a level capture drawn without game view carries a gameViewWarning"),
            Viewport->HasTypedField<EJson::String>(TEXT("gameViewWarning")));
        // The remedy has to name the verb that fixes it, because the whole failure mode is a
        // caller who already called it once and trusted the confirmation.
        TestTrue(TEXT("the warning names editor.set_game_view as the remedy"),
            Viewport->GetStringField(TEXT("gameViewWarning")).Contains(TEXT("editor.set_game_view")));
    }

    // The negative calibration case: game view on is the input every acceptance capture is meant
    // to run on, and it must read silent.
    {
        FViewportCaptureOutput Capture;
        Capture.bGameView = true;
        const TSharedPtr<FJsonObject> Viewport = MakeViewportInfoObject(Capture);
        TestFalse(TEXT("a capture drawn in game view carries no gameViewWarning"),
            Viewport->HasField(TEXT("gameViewWarning")));
    }

    // An asset-preview viewport is normally not in game view, has none of the chrome the warning
    // describes, and cannot be moved by editor.set_game_view -- so warning there would fire on
    // correct output with a remedy that does not apply.
    {
        FViewportCaptureOutput Capture;
        Capture.bGameView = false;
        Capture.PreviewSceneRigDrawn.bSceneAvailable = true;
        const TSharedPtr<FJsonObject> Viewport = MakeViewportInfoObject(Capture);
        TestFalse(TEXT("a preview-scene capture carries no gameViewWarning"),
            Viewport->HasField(TEXT("gameViewWarning")));
    }

    return true;
}

// ============================================================================
// A forced ShowFlag.* cvar is surveyed off the live registry and reported.
//
// The counterfactual: before this, nothing in the plugin read those cvars, `showFlagOverrides` did
// not exist, and a frame with a full-screen debug pass burned into it was reported with every
// field green.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCaptureContaminationShowFlagCVarTest,
    "PinWright.render.capture_contamination.ForcedShowFlagCVarIsSurveyedAndWarned",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCaptureContaminationShowFlagCVarTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightRenderCapture;

    // ---- the survey, against the real console registry ----
    FPwContaminationProbeFlagSink Probe;
    FEngineShowFlags::IterateAllFlags(Probe);

    if (Probe.Chosen.IsEmpty())
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no_unforced_visualize_showflag"),
            TEXT("no ShowFlag.Visualize* console variable on this host reads its default of 2, so "
                 "there is no inert probe to force and restore"));
    }
    else
    {
        const FString CVarName = FString::Printf(TEXT("ShowFlag.%s"), *Probe.Chosen);
        IConsoleVariable* CVar = IConsoleManager::Get().FindConsoleVariable(*CVarName);
        // The sink only selects names whose cvar it already resolved, so a null here is a broken
        // registry -- a hard error rather than a condition to skip on, and asserted rather than
        // checked so a failure does not take the suite host down with it.
        if (TestNotNull(TEXT("the probe flag's console variable resolves"), CVar))
        {
            TestNull(TEXT("the probe flag is not forced before this test touches it"),
                PwContaminationFind(SurveyForcedShowFlagOverrides(), Probe.Chosen));

            // Written at the priority the variable ALREADY carries, which the engine's >= guard
            // accepts, so this test cannot leave a ShowFlag.* cvar pinned above where it found it
            // -- the same rule FScopedViewDistanceScale follows for r.ViewDistanceScale.
            const EConsoleVariableFlags SetBy =
                static_cast<EConsoleVariableFlags>(CVar->GetFlags() & ECVF_SetByMask);
            CVar->Set(0, SetBy);

            {
                const TArray<FForcedShowFlagOverride> Found = SurveyForcedShowFlagOverrides();
                const FForcedShowFlagOverride* Entry = PwContaminationFind(Found, Probe.Chosen);
                if (TestNotNull(TEXT("a forced ShowFlag.* cvar is found by the survey"), Entry))
                {
                    TestEqual(TEXT("the survey reports the forced VALUE, not a bare boolean"),
                        Entry->Value, 0);
                    TestEqual(TEXT("the survey names the cvar the remedy is spelled against"),
                        Entry->CVar, CVarName);
                    TestTrue(TEXT("the survey names the priority that set it"),
                        !Entry->SetBy.IsEmpty());
                }
            }

            CVar->Set(2, SetBy);

            TestNull(TEXT("restoring the default removes the entry, so the survey reads the cvar "
                          "and not a latch"),
                PwContaminationFind(SurveyForcedShowFlagOverrides(), Probe.Chosen));
        }
    }

    // ---- the reporting, as a pure function of the capture output ----

    // Surveyed, nothing forced. This is the reading every capture in a clean editor produces, and
    // it must be silent.
    {
        FViewportCaptureOutput Capture;
        Capture.bShowFlagOverridesMeasured = true;
        const TSharedPtr<FJsonObject> Block = MakeShowFlagOverrideInfoObject(Capture);
        TestTrue(TEXT("measured is true once the survey has run"),
            Block->GetBoolField(TEXT("measured")));
        TestEqual(TEXT("nothing forced means an empty list"),
            Block->GetArrayField(TEXT("forced")).Num(), 0);
        TestFalse(TEXT("a clean survey carries no overrideWarning"),
            Block->HasField(TEXT("overrideWarning")));
    }

    // Never surveyed. An empty list alone would read identically to the clean case above, which is
    // the absence assertion `measured` exists to prevent.
    {
        FViewportCaptureOutput Capture;
        const TSharedPtr<FJsonObject> Block = MakeShowFlagOverrideInfoObject(Capture);
        TestFalse(TEXT("measured is false when the survey never ran"),
            Block->GetBoolField(TEXT("measured")));
    }

    // One flag forced ON: the reported case, and the one that produced a two-thirds occluded
    // acceptance capture with every other field green.
    {
        FViewportCaptureOutput Capture;
        Capture.bShowFlagOverridesMeasured = true;
        FForcedShowFlagOverride& Forced = Capture.ForcedShowFlags.AddDefaulted_GetRef();
        Forced.Name = TEXT("VisualizeProbe");
        Forced.CVar = TEXT("ShowFlag.VisualizeProbe");
        Forced.Value = 1;
        Forced.SetBy = TEXT("Console");

        const TSharedPtr<FJsonObject> Block = MakeShowFlagOverrideInfoObject(Capture);
        const TArray<TSharedPtr<FJsonValue>>& Rows = Block->GetArrayField(TEXT("forced"));
        if (TestEqual(TEXT("the forced flag is listed"), Rows.Num(), 1))
        {
            const TSharedPtr<FJsonObject> Row = Rows[0]->AsObject();
            TestEqual(TEXT("the row names the flag"),
                Row->GetStringField(TEXT("name")), FString(TEXT("VisualizeProbe")));
            TestEqual(TEXT("the row spells the direction as a word, not as 0/1 alone"),
                Row->GetStringField(TEXT("direction")), FString(TEXT("on")));
            TestEqual(TEXT("the row carries the priority that set it"),
                Row->GetStringField(TEXT("setBy")), FString(TEXT("Console")));
        }
        TestTrue(TEXT("a forced flag raises an overrideWarning"),
            Block->HasTypedField<EJson::String>(TEXT("overrideWarning")));
        // Naming the cvar is the whole remedy: restoring it is one console command, and the entire
        // cost of the reported incident was discovering which flag to name.
        TestTrue(TEXT("the warning names the cvar to restore"),
            Block->GetStringField(TEXT("overrideWarning")).Contains(TEXT("ShowFlag.VisualizeProbe")));
    }

    // A flag forced OFF is the other direction of the same contamination -- the frame is missing a
    // rendering feature rather than carrying an extra pass -- and must not be reported as "on".
    {
        FViewportCaptureOutput Capture;
        Capture.bShowFlagOverridesMeasured = true;
        FForcedShowFlagOverride& Forced = Capture.ForcedShowFlags.AddDefaulted_GetRef();
        Forced.Name = TEXT("Probe");
        Forced.CVar = TEXT("ShowFlag.Probe");
        Forced.Value = 0;
        Forced.SetBy = TEXT("Code");

        const TSharedPtr<FJsonObject> Block = MakeShowFlagOverrideInfoObject(Capture);
        const TArray<TSharedPtr<FJsonValue>>& Rows = Block->GetArrayField(TEXT("forced"));
        if (TestEqual(TEXT("the forced-off flag is listed"), Rows.Num(), 1))
        {
            TestEqual(TEXT("a value of 0 reads as off"),
                Rows[0]->AsObject()->GetStringField(TEXT("direction")), FString(TEXT("off")));
        }
        TestTrue(TEXT("a flag forced off raises an overrideWarning too"),
            Block->HasTypedField<EJson::String>(TEXT("overrideWarning")));
    }

    // And the block is reachable from the shared viewport block every capture verb routes through,
    // so camera.orbit_shots and render.capture_annotated report it without knowing it exists.
    {
        FViewportCaptureOutput Capture;
        const TSharedPtr<FJsonObject> Viewport = MakeViewportInfoObject(Capture);
        TestTrue(TEXT("the viewport block carries showFlagOverrides"),
            Viewport->HasTypedField<EJson::Object>(TEXT("showFlagOverrides")));
    }

    return true;
}
