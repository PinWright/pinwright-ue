// Copyright (c) 2026 Alexander Penkin. MIT License.

// lighting.setup_light_shafts — `bloomEnabled` / `occlusionEnabled` report what will render, not
// what was written.
//
// The bug guarded here: a directional light's bEnableLightShaftBloom and bEnableLightShaftOcclusion
// are decorative whenever r.LightShaftQuality is 0, because
// FDeferredShadingSceneRenderer::RenderLightShaftOcclusion and ::RenderLightShaftBloom each open
// with ShouldRenderLightShafts(ViewFamily) — a test that sits ABOVE the loop over Scene->Lights, so
// the component flags are never even read. The flags still read back true, every write still
// succeeds, and BaseScalability.ini zeroes the cvar in [PostProcessQuality@0] and
// [PostProcessQuality@1]. Nothing on the light distinguishes "shafts are on" from "the pass is
// switched off", and unlike r.LightFunctionQuality / r.ShadowQuality this cvar has no
// EngineShowFlagOverride entry either, so the editor's LightShafts show flag stays lit.
//
// The assertion is DIFFERENTIAL, not a presence check: the same call is run against the same
// fixture at r.LightShaftQuality 0 and at 1, and the two runs must disagree about the reported state
// while componentFlags reads true in both. A field hardcoded to the written flag satisfies any
// single-run presence assertion — which is exactly how the sibling volumetric-fog defect survived
// its own two tests.
//
// The test restores r.LightShaftQuality at the priority it already had and destroys its own probe
// light: the cvar is process-global and the suite runs against whatever map the editor opened.

#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "Dom/JsonObject.h"
#include "Components/DirectionalLightComponent.h"
#include "Engine/DirectionalLight.h"
#include "Engine/World.h"
#include "Editor.h"
#include "HAL/IConsoleManager.h"
#include "Tests/TestSkipReporting.h"
#include "Tests/TestUtils.h"
#include "Tests/TestWorldUtils.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLightingSetupLightShaftsEnabledFollowsCVarVetoTest,
    "PinWright.lighting.setup_light_shafts.EnabledFollowsCVarVeto",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FLightingSetupLightShaftsEnabledFollowsCVarVetoTest::RunTest(const FString& Parameters)
{
    IConsoleVariable* LightShaftQualityCVar =
        IConsoleManager::Get().FindConsoleVariable(TEXT("r.LightShaftQuality"));
    if (!LightShaftQualityCVar)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("cvar-absent"),
            TEXT("r.LightShaftQuality is not in this host's console registry, so the renderer veto "
                 "this test drives cannot be produced."));
        return true;
    }

    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("no editor world, so the probe directional light has nowhere to spawn."));
        return true;
    }

    const int32 OriginalCVarValue = LightShaftQualityCVar->GetInt();
    // Write at the priority the variable ALREADY has (the engine's guard is >=, not >), so the test
    // never leaves r.LightShaftQuality pinned at Code priority outranking scalability for the rest
    // of the run — same rule as the sibling volumetric-fog veto test.
    const EConsoleVariableFlags CVarSetBy =
        static_cast<EConsoleVariableFlags>(LightShaftQualityCVar->GetFlags() & ECVF_SetByMask);

    // Destroys the probe light and restores the persistent level's dirty flag.
    FScopedEditorWorldActorGuard ActorGuard;

    // Declared after the guard so it runs BEFORE it.
    ON_SCOPE_EXIT
    {
        LightShaftQualityCVar->Set(OriginalCVarValue, CVarSetBy);
    };

    // A normal level actor, NOT RF_Transient: the verb walks
    // UEditorActorSubsystem::GetAllLevelActors, which filters transient actors out, so a transient
    // probe would be invisible and the miss would read as a verb defect.
    const FString ProbeLabel = TEXT("PinWrightLightShaftProbe");

    ADirectionalLight* Probe = World->SpawnActor<ADirectionalLight>(
        ADirectionalLight::StaticClass(), FVector(0.0f, 0.0f, 1000.0f), FRotator(-45.0f, 0.0f, 0.0f));
    if (!Probe)
    {
        AddError(TEXT("Failed to spawn the probe directional light."));
        return false;
    }
    Probe->SetActorLabel(ProbeLabel);

    UDirectionalLightComponent* ProbeComp = Probe->FindComponentByClass<UDirectionalLightComponent>();
    if (!ProbeComp)
    {
        AddError(TEXT("The probe directional light carries no UDirectionalLightComponent."));
        return false;
    }
    // The engine's light-shaft setters no-op on a registered Static-mobility component, and the verb
    // refuses that case outright. Movable keeps the probe on the path under test.
    ProbeComp->SetMobility(EComponentMobility::Movable);

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("actorName"), ProbeLabel);
    Payload->SetBoolField(TEXT("bloom"), true);
    Payload->SetBoolField(TEXT("occlusion"), true);

    // ---- Case 1: the veto is in force. This is the assertion an echoed flag could not fail.
    // Read the write back: if a higher-priority setter holds the cvar the premise of the whole test
    // is absent, and asserting on it would report a verb defect that is really a host one.
    LightShaftQualityCVar->Set(0, CVarSetBy);
    if (LightShaftQualityCVar->GetInt() != 0)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("cvar-pinned"),
            TEXT("r.LightShaftQuality would not take 0 on this host, so the renderer veto could not "
                 "be staged."));
        return true;
    }

    FTestResponseCapture Vetoed;
    TestTrue(TEXT("lighting.setup_light_shafts handler found (vetoed run)"),
        InvokeHandlerWithCapture(TEXT("lighting.setup_light_shafts"), Payload, Vetoed));
    TestTrue(TEXT("the component write still succeeds while the passes are switched off"),
        Vetoed.bSuccess);
    if (!Vetoed.bSuccess || !Vetoed.Result.IsValid())
    {
        return false;
    }

    bool bVetoedBloom = true;
    TestTrue(TEXT("the vetoed response carries bloomEnabled"),
        Vetoed.Result->TryGetBoolField(TEXT("bloomEnabled"), bVetoedBloom));
    TestFalse(TEXT("bloomEnabled is false while r.LightShaftQuality is 0"), bVetoedBloom);

    bool bVetoedOcclusion = true;
    TestTrue(TEXT("the vetoed response carries occlusionEnabled"),
        Vetoed.Result->TryGetBoolField(TEXT("occlusionEnabled"), bVetoedOcclusion));
    TestFalse(TEXT("occlusionEnabled is false while r.LightShaftQuality is 0"), bVetoedOcclusion);

    const TSharedPtr<FJsonObject>* VetoedFlags = nullptr;
    TestTrue(TEXT("the vetoed response carries componentFlags"),
        Vetoed.Result->TryGetObjectField(TEXT("componentFlags"), VetoedFlags));
    bool bVetoedFlagBloom = false;
    bool bVetoedFlagOcclusion = false;
    if (VetoedFlags && (*VetoedFlags).IsValid())
    {
        (*VetoedFlags)->TryGetBoolField(TEXT("bloom"), bVetoedFlagBloom);
        (*VetoedFlags)->TryGetBoolField(TEXT("occlusion"), bVetoedFlagOcclusion);
        TestTrue(TEXT("componentFlags.bloom reports the write that did happen"), bVetoedFlagBloom);
        TestTrue(TEXT("componentFlags.occlusion reports the write that did happen"),
            bVetoedFlagOcclusion);
    }

    const TSharedPtr<FJsonObject>* VetoedCVarInfo = nullptr;
    TestTrue(TEXT("the vetoed response carries lightShaftQualityCVar"),
        Vetoed.Result->TryGetObjectField(TEXT("lightShaftQualityCVar"), VetoedCVarInfo));
    if (VetoedCVarInfo && (*VetoedCVarInfo).IsValid())
    {
        FString CVarName;
        (*VetoedCVarInfo)->TryGetStringField(TEXT("cvar"), CVarName);
        TestEqual(TEXT("the reported cvar is r.LightShaftQuality"),
            CVarName, FString(TEXT("r.LightShaftQuality")));

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
    TestTrue(TEXT("a vetoed call names r.LightShaftQuality in cvarWarning"),
        VetoedWarning.Contains(TEXT("r.LightShaftQuality")));

    // ---- Case 2: same fixture, same call, veto lifted.
    LightShaftQualityCVar->Set(1, CVarSetBy);
    if (LightShaftQualityCVar->GetInt() == 0)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("cvar-pinned"),
            TEXT("r.LightShaftQuality would not take a non-zero value on this host, so the "
                 "veto-lifted half of the differential is absent."));
        return true;
    }

    FTestResponseCapture Running;
    TestTrue(TEXT("lighting.setup_light_shafts handler found (running run)"),
        InvokeHandlerWithCapture(TEXT("lighting.setup_light_shafts"), Payload, Running));
    TestTrue(TEXT("the verb succeeds with the passes running"), Running.bSuccess);
    if (!Running.bSuccess || !Running.Result.IsValid())
    {
        return false;
    }

    bool bRunningBloom = false;
    TestTrue(TEXT("the running response carries bloomEnabled"),
        Running.Result->TryGetBoolField(TEXT("bloomEnabled"), bRunningBloom));
    TestTrue(TEXT("bloomEnabled is true once r.LightShaftQuality is 1"), bRunningBloom);

    bool bRunningOcclusion = false;
    TestTrue(TEXT("the running response carries occlusionEnabled"),
        Running.Result->TryGetBoolField(TEXT("occlusionEnabled"), bRunningOcclusion));
    TestTrue(TEXT("occlusionEnabled is true once r.LightShaftQuality is 1"), bRunningOcclusion);

    TestFalse(TEXT("a running pass publishes no cvarWarning"),
        Running.Result->HasField(TEXT("cvarWarning")));

    const TSharedPtr<FJsonObject>* RunningFlags = nullptr;
    Running.Result->TryGetObjectField(TEXT("componentFlags"), RunningFlags);
    if (RunningFlags && (*RunningFlags).IsValid())
    {
        bool bRunningFlagBloom = false;
        (*RunningFlags)->TryGetBoolField(TEXT("bloom"), bRunningFlagBloom);
        TestTrue(TEXT("componentFlags.bloom is the same in both runs — only the cvar moved"),
            bRunningFlagBloom == bVetoedFlagBloom);
    }

    // The differential itself: a field echoed back from the component flag scores both runs alike.
    TestTrue(TEXT("bloomEnabled differs between the vetoed and the running run"),
        bVetoedBloom != bRunningBloom);
    TestTrue(TEXT("occlusionEnabled differs between the vetoed and the running run"),
        bVetoedOcclusion != bRunningOcclusion);

    return true;
}

// The other half of the contract: the verb must not write on a call that names nothing to write.
// A "setup" verb that quietly enables both flags when neither is asked for would publish a measured
// enabled state for a write the caller never requested.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLightingSetupLightShaftsRefusesEmptyRequestTest,
    "PinWright.lighting.setup_light_shafts.RefusesRequestWithNothingToWrite",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FLightingSetupLightShaftsRefusesEmptyRequestTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();

    FTestResponseCapture Capture;
    TestTrue(TEXT("lighting.setup_light_shafts handler found"),
        InvokeHandlerWithCapture(TEXT("lighting.setup_light_shafts"), Payload, Capture));
    TestFalse(TEXT("a call naming neither bloom nor occlusion is an error, not a success"),
        Capture.bSuccess);
    TestEqual(TEXT("the error code is INVALID_ARGUMENT"),
        Capture.ErrorCode, FString(TEXT("INVALID_ARGUMENT")));

    return true;
}
