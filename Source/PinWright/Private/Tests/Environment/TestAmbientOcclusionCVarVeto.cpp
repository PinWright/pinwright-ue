// Copyright (c) 2026 Alexander Penkin. MIT License.

// lighting.set_ambient_occlusion — `enabled` reports what will render, not what was written.
//
// The bug guarded here: the handler derived `enabled` from the applied intensity alone
// (intensity > 0), so it answered `enabled: true` for a write that renders nothing. While
// r.AmbientOcclusionLevels is 0 the screen-space AO pass does not run at all —
// ShouldRenderScreenSpaceAmbientOcclusion requires FSSAOHelper::GetNumAmbientOcclusionLevels() != 0
// (CompositionLighting.cpp:81-95), a straight read of the cvar
// (PostProcessAmbientOcclusion.cpp:193-195) — so AmbientOcclusionIntensity, AmbientOcclusionRadius
// and every other AO field on the volume are decorative. The settings read back exactly as written,
// and BaseScalability.ini zeroes the cvar in [PostProcessQuality@0], so a low-post-process-quality
// editor renders no AO at all while every write here still succeeds.
//
// The assertion is DIFFERENTIAL, not a presence check: the same call is run against the same volume
// at r.AmbientOcclusionLevels 0 and at -1, and `enabled` must differ between the two while
// `intensity` — the write echo — is identical. The old derivation satisfies any single-run presence
// assertion, which is how the existing EchoesAppliedValues test passed against the defect.
//
// The test restores r.AmbientOcclusionLevels at the priority it already had, restores the AO
// settings of a volume it did not spawn, and lets the actor guard destroy one it did: the cvar is
// process-global and the suite runs against whatever map the editor opened.

#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "Dom/JsonObject.h"
#include "Engine/PostProcessVolume.h"
#include "Engine/World.h"
#include "Editor.h"
#include "HAL/IConsoleManager.h"
#include "Tests/TestSkipReporting.h"
#include "Tests/TestUtils.h"
#include "Tests/TestWorldUtils.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLightingSetAmbientOcclusionEnabledFollowsCVarVetoTest,
    "PinWright.lighting.set_ambient_occlusion.EnabledFollowsCVarVeto",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FLightingSetAmbientOcclusionEnabledFollowsCVarVetoTest::RunTest(const FString& Parameters)
{
    IConsoleVariable* AOLevelsCVar =
        IConsoleManager::Get().FindConsoleVariable(TEXT("r.AmbientOcclusionLevels"));
    if (!AOLevelsCVar)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("cvar-absent"),
            TEXT("r.AmbientOcclusionLevels is not in this host's console registry, so the renderer "
                 "veto this test drives cannot be produced."));
        return true;
    }

    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("no editor world, so the verb has nowhere to find or spawn a PostProcessVolume."));
        return true;
    }

    const int32 OriginalCVarValue = AOLevelsCVar->GetInt();
    // Write at the priority the variable ALREADY has (the engine's guard is >=, not >), so the test
    // never leaves r.AmbientOcclusionLevels pinned at Code priority outranking scalability for the
    // rest of the run — same rule as the sibling volumetric-fog and light-shaft veto tests.
    const EConsoleVariableFlags CVarSetBy =
        static_cast<EConsoleVariableFlags>(AOLevelsCVar->GetFlags() & ECVF_SetByMask);

    // The verb writes into the level's unbound PPV if there is one. Record its AO state so a volume
    // this test did not spawn goes back exactly as it was found; one the verb spawns is destroyed by
    // the actor guard instead.
    APostProcessVolume* ExistingPPV = FindUnboundPPV(World);
    float OriginalAOIntensity = 0.0f;
    float OriginalAORadius = 0.0f;
    bool bOriginalOverrideIntensity = false;
    bool bOriginalOverrideRadius = false;
    if (ExistingPPV)
    {
        OriginalAOIntensity = ExistingPPV->Settings.AmbientOcclusionIntensity;
        OriginalAORadius = ExistingPPV->Settings.AmbientOcclusionRadius;
        bOriginalOverrideIntensity = ExistingPPV->Settings.bOverride_AmbientOcclusionIntensity != 0;
        bOriginalOverrideRadius = ExistingPPV->Settings.bOverride_AmbientOcclusionRadius != 0;
    }

    // Destroys a PPV the verb spawns on a map that has none, and restores the persistent level's
    // dirty flag last.
    FScopedEditorWorldActorGuard ActorGuard;

    // Declared after the guard so it runs BEFORE it: the settings go back first, then the guard
    // settles the level's dirty flag over the top.
    ON_SCOPE_EXIT
    {
        AOLevelsCVar->Set(OriginalCVarValue, CVarSetBy);
        if (IsValid(ExistingPPV))
        {
            ExistingPPV->Settings.AmbientOcclusionIntensity = OriginalAOIntensity;
            ExistingPPV->Settings.AmbientOcclusionRadius = OriginalAORadius;
            ExistingPPV->Settings.bOverride_AmbientOcclusionIntensity = bOriginalOverrideIntensity;
            ExistingPPV->Settings.bOverride_AmbientOcclusionRadius = bOriginalOverrideRadius;
        }
    };

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetNumberField(TEXT("intensity"), 0.6);
    Payload->SetNumberField(TEXT("radius"), 120.0);

    // ---- Case 1: the veto is in force. This is the assertion the old derivation could not fail.
    // Read the write back: if a higher-priority setter holds the cvar the premise of the whole test
    // is absent, and asserting on it would report a verb defect that is really a host one.
    AOLevelsCVar->Set(0, CVarSetBy);
    if (AOLevelsCVar->GetInt() != 0)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("cvar-pinned"),
            TEXT("r.AmbientOcclusionLevels would not take 0 on this host, so the renderer veto "
                 "could not be staged."));
        return true;
    }

    FTestResponseCapture Vetoed;
    TestTrue(TEXT("lighting.set_ambient_occlusion handler found (vetoed run)"),
        InvokeHandlerWithCapture(TEXT("lighting.set_ambient_occlusion"), Payload, Vetoed));
    TestTrue(TEXT("the settings write still succeeds while the AO pass is switched off"),
        Vetoed.bSuccess);
    if (!Vetoed.bSuccess || !Vetoed.Result.IsValid())
    {
        return false;
    }

    bool bVetoedEnabled = true;
    TestTrue(TEXT("the vetoed response carries enabled"),
        Vetoed.Result->TryGetBoolField(TEXT("enabled"), bVetoedEnabled));
    TestFalse(TEXT("enabled is false while r.AmbientOcclusionLevels is 0"), bVetoedEnabled);

    double VetoedIntensity = 0.0;
    TestTrue(TEXT("the vetoed response carries intensity"),
        Vetoed.Result->TryGetNumberField(TEXT("intensity"), VetoedIntensity));
    TestEqual(TEXT("intensity reports the write that did happen"), (float)VetoedIntensity, 0.6f);

    const TSharedPtr<FJsonObject>* VetoedCVarInfo = nullptr;
    TestTrue(TEXT("the vetoed response carries ambientOcclusionLevelsCVar"),
        Vetoed.Result->TryGetObjectField(TEXT("ambientOcclusionLevelsCVar"), VetoedCVarInfo));
    if (VetoedCVarInfo && (*VetoedCVarInfo).IsValid())
    {
        FString CVarName;
        (*VetoedCVarInfo)->TryGetStringField(TEXT("cvar"), CVarName);
        TestEqual(TEXT("the reported cvar is r.AmbientOcclusionLevels"),
            CVarName, FString(TEXT("r.AmbientOcclusionLevels")));

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
    TestTrue(TEXT("a vetoed call names r.AmbientOcclusionLevels in cvarWarning"),
        VetoedWarning.Contains(TEXT("r.AmbientOcclusionLevels")));

    // ---- Case 2: same fixture, same call, veto lifted. -1 is the engine default: "decide from the
    // post-process settings", which is what BaseScalability.ini restores at PostProcessQuality@1.
    AOLevelsCVar->Set(-1, CVarSetBy);
    if (AOLevelsCVar->GetInt() == 0)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("cvar-pinned"),
            TEXT("r.AmbientOcclusionLevels would not take a non-zero value on this host, so the "
                 "veto-lifted half of the differential is absent."));
        return true;
    }

    FTestResponseCapture Running;
    TestTrue(TEXT("lighting.set_ambient_occlusion handler found (running run)"),
        InvokeHandlerWithCapture(TEXT("lighting.set_ambient_occlusion"), Payload, Running));
    TestTrue(TEXT("the verb succeeds with the AO pass running"), Running.bSuccess);
    if (!Running.bSuccess || !Running.Result.IsValid())
    {
        return false;
    }

    bool bRunningEnabled = false;
    TestTrue(TEXT("the running response carries enabled"),
        Running.Result->TryGetBoolField(TEXT("enabled"), bRunningEnabled));
    TestTrue(TEXT("enabled is true once r.AmbientOcclusionLevels is non-zero"), bRunningEnabled);

    double RunningIntensity = 0.0;
    Running.Result->TryGetNumberField(TEXT("intensity"), RunningIntensity);
    TestEqual(TEXT("intensity is the same in both runs — only the cvar moved"),
        (float)RunningIntensity, (float)VetoedIntensity);

    TestFalse(TEXT("a running pass publishes no cvarWarning"),
        Running.Result->HasField(TEXT("cvarWarning")));

    // The differential itself: a field derived from the written intensity alone scores both runs
    // alike.
    TestTrue(TEXT("enabled differs between the vetoed and the running run"),
        bVetoedEnabled != bRunningEnabled);

    return true;
}
