// Copyright (c) 2026 Alexander Penkin. MIT License.

// lighting.spawn_sky_light — the written intensity is not the effective one.
//
// This is the RESCALING variant of the scalability-cvar family, not the veto one, and it is the
// harder shape to notice: nothing fails, nothing is switched off, and the component reads back
// exactly what was asked for. FSkyLightSceneProxy::GetEffectiveLightColor returns
// LightColor * GSkylightIntensityMultiplier (SkyLightComponent.cpp:85-91, 231-234), so the renderer
// never sees the written intensity, and BaseScalability.ini sets r.SkylightIntensityMultiplier to
// 0.8 in [GlobalIlluminationQuality@0]. The bug guarded here: the handler answered
// {success, actorName} and said nothing about intensity at all, so a caller had no way to learn
// that the scene is lit at a fraction of the value it just wrote.
//
// The assertion is DIFFERENTIAL: the same call is run against the same fixture at multiplier 0.5 and
// at 1.0, and `effectiveIntensity` must differ between the two while `intensity` — the component
// read-back — is identical. A field echoed back from the request scores both runs alike, which is
// precisely the defect.
//
// The test restores r.SkylightIntensityMultiplier at the priority it already had and destroys both
// probe sky lights: the cvar is process-global and the suite runs against whatever map the editor
// opened.

#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "Dom/JsonObject.h"
#include "Engine/World.h"
#include "Editor.h"
#include "HAL/IConsoleManager.h"
#include "Tests/TestSkipReporting.h"
#include "Tests/TestUtils.h"
#include "Tests/TestWorldUtils.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLightingSpawnSkyLightEffectiveIntensityFollowsCVarScaleTest,
    "PinWright.lighting.spawn_sky_light.EffectiveIntensityFollowsCVarScale",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FLightingSpawnSkyLightEffectiveIntensityFollowsCVarScaleTest::RunTest(const FString& Parameters)
{
    IConsoleVariable* MultiplierCVar =
        IConsoleManager::Get().FindConsoleVariable(TEXT("r.SkylightIntensityMultiplier"));
    if (!MultiplierCVar)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("cvar-absent"),
            TEXT("r.SkylightIntensityMultiplier is not in this host's console registry, so the "
                 "renderer rescale this test drives cannot be produced."));
        return true;
    }

    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("no editor world, so the verb has nowhere to spawn its sky light."));
        return true;
    }

    const float OriginalCVarValue = MultiplierCVar->GetFloat();
    // Write at the priority the variable ALREADY has (the engine's guard is >=, not >), so the test
    // never leaves r.SkylightIntensityMultiplier pinned at Code priority outranking scalability for
    // the rest of the run — same rule as the sibling volumetric-fog and light-shaft veto tests.
    const EConsoleVariableFlags CVarSetBy =
        static_cast<EConsoleVariableFlags>(MultiplierCVar->GetFlags() & ECVF_SetByMask);

    // Destroys both sky lights the verb spawns and restores the persistent level's dirty flag.
    FScopedEditorWorldActorGuard ActorGuard;

    // Declared after the guard so it runs BEFORE it.
    ON_SCOPE_EXIT
    {
        MultiplierCVar->Set(OriginalCVarValue, CVarSetBy);
    };

    const double RequestedIntensity = 2.0;

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetNumberField(TEXT("intensity"), RequestedIntensity);

    // ---- Case 1: the renderer rescales. This is the assertion the old response could not carry.
    // Read the write back: if a higher-priority setter holds the cvar the premise of the whole test
    // is absent, and asserting on it would report a verb defect that is really a host one.
    MultiplierCVar->Set(0.5f, CVarSetBy);
    if (!FMath::IsNearlyEqual(MultiplierCVar->GetFloat(), 0.5f))
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("cvar-pinned"),
            TEXT("r.SkylightIntensityMultiplier would not take 0.5 on this host, so the renderer "
                 "rescale could not be staged."));
        return true;
    }

    Payload->SetStringField(TEXT("name"), TEXT("PinWrightSkyScaleProbeScaled"));

    FTestResponseCapture Scaled;
    TestTrue(TEXT("lighting.spawn_sky_light handler found (scaled run)"),
        InvokeHandlerWithCapture(TEXT("lighting.spawn_sky_light"), Payload, Scaled));
    TestTrue(TEXT("the intensity write still succeeds while the renderer rescales it"),
        Scaled.bSuccess);
    if (!Scaled.bSuccess || !Scaled.Result.IsValid())
    {
        return false;
    }

    double ScaledIntensity = 0.0;
    TestTrue(TEXT("the scaled response carries intensity"),
        Scaled.Result->TryGetNumberField(TEXT("intensity"), ScaledIntensity));
    TestEqual(TEXT("intensity reports the write that did happen"),
        (float)ScaledIntensity, (float)RequestedIntensity);

    double ScaledEffective = 0.0;
    TestTrue(TEXT("the scaled response carries effectiveIntensity"),
        Scaled.Result->TryGetNumberField(TEXT("effectiveIntensity"), ScaledEffective));
    TestEqual(TEXT("effectiveIntensity is the written intensity times the measured multiplier"),
        (float)ScaledEffective, (float)(RequestedIntensity * 0.5));

    const TSharedPtr<FJsonObject>* ScaledCVarInfo = nullptr;
    TestTrue(TEXT("the scaled response carries skylightIntensityMultiplierCVar"),
        Scaled.Result->TryGetObjectField(TEXT("skylightIntensityMultiplierCVar"), ScaledCVarInfo));
    if (ScaledCVarInfo && (*ScaledCVarInfo).IsValid())
    {
        FString CVarName;
        (*ScaledCVarInfo)->TryGetStringField(TEXT("cvar"), CVarName);
        TestEqual(TEXT("the reported cvar is r.SkylightIntensityMultiplier"),
            CVarName, FString(TEXT("r.SkylightIntensityMultiplier")));

        bool bFound = false;
        (*ScaledCVarInfo)->TryGetBoolField(TEXT("found"), bFound);
        TestTrue(TEXT("the cvar was measured, not assumed"), bFound);

        double MeasuredValue = 0.0;
        TestTrue(TEXT("the measured cvar value is published"),
            (*ScaledCVarInfo)->TryGetNumberField(TEXT("value"), MeasuredValue));
        TestEqual(TEXT("the measured cvar value is 0.5"), (float)MeasuredValue, 0.5f);
    }

    FString ScaledWarning;
    Scaled.Result->TryGetStringField(TEXT("cvarWarning"), ScaledWarning);
    TestTrue(TEXT("a rescaled call names r.SkylightIntensityMultiplier in cvarWarning"),
        ScaledWarning.Contains(TEXT("r.SkylightIntensityMultiplier")));

    // ---- Case 2: same call, same fixture, scale restored to 1.
    MultiplierCVar->Set(1.0f, CVarSetBy);
    if (!FMath::IsNearlyEqual(MultiplierCVar->GetFloat(), 1.0f))
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("cvar-pinned"),
            TEXT("r.SkylightIntensityMultiplier would not take 1.0 on this host, so the unscaled "
                 "half of the differential is absent."));
        return true;
    }

    Payload->SetStringField(TEXT("name"), TEXT("PinWrightSkyScaleProbeUnscaled"));

    FTestResponseCapture Unscaled;
    TestTrue(TEXT("lighting.spawn_sky_light handler found (unscaled run)"),
        InvokeHandlerWithCapture(TEXT("lighting.spawn_sky_light"), Payload, Unscaled));
    TestTrue(TEXT("the verb succeeds with the scale at 1"), Unscaled.bSuccess);
    if (!Unscaled.bSuccess || !Unscaled.Result.IsValid())
    {
        return false;
    }

    double UnscaledIntensity = 0.0;
    Unscaled.Result->TryGetNumberField(TEXT("intensity"), UnscaledIntensity);
    TestEqual(TEXT("intensity is the same in both runs — only the cvar moved"),
        (float)UnscaledIntensity, (float)ScaledIntensity);

    double UnscaledEffective = 0.0;
    TestTrue(TEXT("the unscaled response carries effectiveIntensity"),
        Unscaled.Result->TryGetNumberField(TEXT("effectiveIntensity"), UnscaledEffective));
    TestEqual(TEXT("effectiveIntensity equals the written intensity once the scale is 1"),
        (float)UnscaledEffective, (float)RequestedIntensity);

    TestFalse(TEXT("an unscaled renderer publishes no cvarWarning"),
        Unscaled.Result->HasField(TEXT("cvarWarning")));

    // The differential itself: a field echoed back from the request scores both runs alike.
    TestTrue(TEXT("effectiveIntensity differs between the scaled and the unscaled run"),
        !FMath::IsNearlyEqual((float)ScaledEffective, (float)UnscaledEffective));

    return true;
}
