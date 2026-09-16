// Copyright (c) 2026 Alexander Penkin. MIT License.

// lighting.spawn_light — `shadowsEnabled` reports what will render, not what was written.
//
// The bug guarded here: the handler answered {success, actorName} and said nothing about shadows at
// all, so a caller that passed properties.castShadows:true got a success indistinguishable from a
// frame in which no dynamic shadow renders. While r.ShadowQuality is 0, EngineShowFlagOverride
// force-clears the DynamicShadows show flag for the whole frame (ShowFlags.cpp:490-496) — a test
// that sits above any per-light one, so the component's CastShadows bit is never consulted. The bit
// still reads back true, every write still succeeds, and BaseScalability.ini zeroes the cvar in
// [ShadowQuality@0]. Nothing readable off the light distinguishes "this light casts shadows" from
// "no light in the level casts shadows", so shadow bias and cascade values tuned against such a
// frame are tuned against the wrong renderer.
//
// The assertion is DIFFERENTIAL, not a presence check: the same call is run against the same
// renderer state at r.ShadowQuality 0 and at 3, and `shadowsEnabled` must differ between the two
// while `castShadows` reads true in both. A field echoed back from the request satisfies any
// single-run presence assertion, which is exactly how the sibling volumetric-fog defect survived
// its own tests.
//
// The test restores r.ShadowQuality at the priority it already had and destroys both probe lights:
// the cvar is process-global and the suite runs against whatever map the editor opened.

#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "Dom/JsonObject.h"
#include "Engine/World.h"
#include "Editor.h"
#include "HAL/IConsoleManager.h"
#include "Tests/TestSkipReporting.h"
#include "Tests/TestUtils.h"
#include "Tests/TestWorldUtils.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLightingSpawnLightShadowsEnabledFollowsCVarVetoTest,
    "PinWright.lighting.spawn_light.ShadowsEnabledFollowsCVarVeto",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FLightingSpawnLightShadowsEnabledFollowsCVarVetoTest::RunTest(const FString& Parameters)
{
    IConsoleVariable* ShadowQualityCVar =
        IConsoleManager::Get().FindConsoleVariable(TEXT("r.ShadowQuality"));
    if (!ShadowQualityCVar)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("cvar-absent"),
            TEXT("r.ShadowQuality is not in this host's console registry, so the renderer veto this "
                 "test drives cannot be produced."));
        return true;
    }

    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("no editor world, so the verb has nowhere to spawn its light."));
        return true;
    }

    const int32 OriginalCVarValue = ShadowQualityCVar->GetInt();
    // Write at the priority the variable ALREADY has (the engine's guard is >=, not >), so the test
    // never leaves r.ShadowQuality pinned at Code priority outranking scalability for the rest of
    // the run — same rule as the sibling volumetric-fog and light-shaft veto tests.
    const EConsoleVariableFlags CVarSetBy =
        static_cast<EConsoleVariableFlags>(ShadowQualityCVar->GetFlags() & ECVF_SetByMask);

    // Destroys both lights the verb spawns and restores the persistent level's dirty flag.
    FScopedEditorWorldActorGuard ActorGuard;

    // Declared after the guard so it runs BEFORE it.
    ON_SCOPE_EXIT
    {
        ShadowQualityCVar->Set(OriginalCVarValue, CVarSetBy);
    };

    TSharedPtr<FJsonObject> Props = MakeShared<FJsonObject>();
    Props->SetBoolField(TEXT("castShadows"), true);

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("lightType"), TEXT("point"));
    Payload->SetObjectField(TEXT("properties"), Props);

    // ---- Case 1: the veto is in force. This is the assertion the old response could not carry.
    // Read the write back: if a higher-priority setter holds the cvar the premise of the whole test
    // is absent, and asserting on it would report a verb defect that is really a host one.
    ShadowQualityCVar->Set(0, CVarSetBy);
    if (ShadowQualityCVar->GetInt() != 0)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("cvar-pinned"),
            TEXT("r.ShadowQuality would not take 0 on this host, so the renderer veto could not be "
                 "staged."));
        return true;
    }

    Payload->SetStringField(TEXT("name"), TEXT("PinWrightShadowVetoProbeVetoed"));

    FTestResponseCapture Vetoed;
    TestTrue(TEXT("lighting.spawn_light handler found (vetoed run)"),
        InvokeHandlerWithCapture(TEXT("lighting.spawn_light"), Payload, Vetoed));
    TestTrue(TEXT("the spawn still succeeds while dynamic shadows are switched off"),
        Vetoed.bSuccess);
    if (!Vetoed.bSuccess || !Vetoed.Result.IsValid())
    {
        return false;
    }

    bool bVetoedShadowsEnabled = true;
    TestTrue(TEXT("the vetoed response carries shadowsEnabled"),
        Vetoed.Result->TryGetBoolField(TEXT("shadowsEnabled"), bVetoedShadowsEnabled));
    TestFalse(TEXT("shadowsEnabled is false while r.ShadowQuality is 0"), bVetoedShadowsEnabled);

    bool bVetoedCastShadows = false;
    TestTrue(TEXT("the vetoed response carries castShadows"),
        Vetoed.Result->TryGetBoolField(TEXT("castShadows"), bVetoedCastShadows));
    TestTrue(TEXT("castShadows reports the write that did happen"), bVetoedCastShadows);

    const TSharedPtr<FJsonObject>* VetoedCVarInfo = nullptr;
    TestTrue(TEXT("the vetoed response carries shadowQualityCVar"),
        Vetoed.Result->TryGetObjectField(TEXT("shadowQualityCVar"), VetoedCVarInfo));
    if (VetoedCVarInfo && (*VetoedCVarInfo).IsValid())
    {
        FString CVarName;
        (*VetoedCVarInfo)->TryGetStringField(TEXT("cvar"), CVarName);
        TestEqual(TEXT("the reported cvar is r.ShadowQuality"),
            CVarName, FString(TEXT("r.ShadowQuality")));

        bool bFound = false;
        (*VetoedCVarInfo)->TryGetBoolField(TEXT("found"), bFound);
        TestTrue(TEXT("the cvar was measured, not assumed"), bFound);

        double MeasuredValue = -1.0;
        TestTrue(TEXT("the measured cvar value is published"),
            (*VetoedCVarInfo)->TryGetNumberField(TEXT("value"), MeasuredValue));
        TestEqual(TEXT("the measured cvar value is 0"), MeasuredValue, 0.0);
    }

    FString VetoedWarning;
    Vetoed.Result->TryGetStringField(TEXT("cvarWarning"), VetoedWarning);
    TestTrue(TEXT("a vetoed call names r.ShadowQuality in cvarWarning"),
        VetoedWarning.Contains(TEXT("r.ShadowQuality")));

    // ---- Case 2: same call, same renderer, veto lifted.
    ShadowQualityCVar->Set(3, CVarSetBy);
    if (ShadowQualityCVar->GetInt() <= 0)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("cvar-pinned"),
            TEXT("r.ShadowQuality would not take a value above 0 on this host, so the veto-lifted "
                 "half of the differential is absent."));
        return true;
    }

    Payload->SetStringField(TEXT("name"), TEXT("PinWrightShadowVetoProbeRunning"));

    FTestResponseCapture Running;
    TestTrue(TEXT("lighting.spawn_light handler found (running run)"),
        InvokeHandlerWithCapture(TEXT("lighting.spawn_light"), Payload, Running));
    TestTrue(TEXT("the verb succeeds with dynamic shadows running"), Running.bSuccess);
    if (!Running.bSuccess || !Running.Result.IsValid())
    {
        return false;
    }

    bool bRunningShadowsEnabled = false;
    TestTrue(TEXT("the running response carries shadowsEnabled"),
        Running.Result->TryGetBoolField(TEXT("shadowsEnabled"), bRunningShadowsEnabled));
    TestTrue(TEXT("shadowsEnabled is true once r.ShadowQuality is above 0"), bRunningShadowsEnabled);

    bool bRunningCastShadows = false;
    Running.Result->TryGetBoolField(TEXT("castShadows"), bRunningCastShadows);
    TestTrue(TEXT("castShadows is the same in both runs — only the cvar moved"),
        bRunningCastShadows == bVetoedCastShadows);

    TestFalse(TEXT("a running renderer publishes no cvarWarning"),
        Running.Result->HasField(TEXT("cvarWarning")));

    // The differential itself: a field echoed back from the request scores both runs alike.
    TestTrue(TEXT("shadowsEnabled differs between the vetoed and the running run"),
        bVetoedShadowsEnabled != bRunningShadowsEnabled);

    return true;
}
