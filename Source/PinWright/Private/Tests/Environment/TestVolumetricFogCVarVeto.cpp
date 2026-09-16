// Copyright (c) 2026 Alexander Penkin. MIT License.

// lighting.setup_volumetric_fog — `enabled` reports what will render, not what was written.
//
// The bug guarded here: the handler answered `enabled: true` unconditionally, because the
// field was the literal `true` next to a component write that always succeeds. While
// r.VolumetricFog is 0 the volumetric pass does not run at all, so bEnableVolumetricFog,
// VolumetricFogDistance, VolumetricFogAlbedo and every light's VolumetricScatteringIntensity
// are decorative — and nothing readable off the actor distinguishes that state from a
// working one, so fog values tuned against such a frame are tuned against the wrong renderer.
//
// The assertion is DIFFERENTIAL, not a presence check: the same call is run against the same
// fixture at r.VolumetricFog 0 and at 1, and `enabled` must differ between the two. A
// hardcoded `true` satisfies any single-run presence assertion, which is exactly how the
// defect survived the existing tests on this verb.
//
// The test restores r.VolumetricFog, the fog component's two written fields and the package
// dirty flag: it runs against the host project's real open level, and the cvar is
// process-global.

#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "Dom/JsonObject.h"
#include "Components/ExponentialHeightFogComponent.h"
#include "Engine/ExponentialHeightFog.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Editor.h"
#include "HAL/IConsoleManager.h"
#include "UObject/Package.h"
#include "Tests/TestUtils.h"
#include "Tests/TestWorldUtils.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLightingSetupVolumetricFogEnabledFollowsCVarVetoTest,
    "PinWright.lighting.setup_volumetric_fog.EnabledFollowsCVarVeto",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FLightingSetupVolumetricFogEnabledFollowsCVarVetoTest::RunTest(const FString& Parameters)
{
    IConsoleVariable* VolumetricFogCVar =
        IConsoleManager::Get().FindConsoleVariable(TEXT("r.VolumetricFog"));
    if (!VolumetricFogCVar)
    {
        AddWarning(TEXT("PINWRIGHT_ASSERTIONS_SKIPPED: r.VolumetricFog is not in this host's ")
                   TEXT("console registry, so the renderer veto this test drives cannot be produced."));
        return true;
    }

    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World)
    {
        AddWarning(TEXT("PINWRIGHT_ASSERTIONS_SKIPPED: no editor world, so the verb has nowhere ")
                   TEXT("to find or spawn an ExponentialHeightFog."));
        return true;
    }

    const int32 OriginalCVarValue = VolumetricFogCVar->GetInt();
    // Write at the priority the variable ALREADY has (the engine's guard is >=, not >), so the
    // test never leaves r.VolumetricFog pinned at Code priority outranking scalability for the
    // rest of the run — same rule as FScopedViewDistanceScale in PreviewViewportCaptureUtils.cpp.
    const EConsoleVariableFlags CVarSetBy =
        static_cast<EConsoleVariableFlags>(VolumetricFogCVar->GetFlags() & ECVF_SetByMask);

    AExponentialHeightFog* ExistingFog = nullptr;
    for (TActorIterator<AExponentialHeightFog> It(World); It; ++It)
    {
        ExistingFog = *It;
        break;
    }

    bool bOriginalEnableFlag = false;
    float OriginalDistance = 0.0f;
    UPackage* FogPackage = nullptr;
    bool bFogPackageWasDirty = false;
    if (ExistingFog && ExistingFog->GetComponent())
    {
        bOriginalEnableFlag = ExistingFog->GetComponent()->bEnableVolumetricFog;
        OriginalDistance = ExistingFog->GetComponent()->VolumetricFogDistance;
        FogPackage = ExistingFog->GetPackage();
        bFogPackageWasDirty = FogPackage ? FogPackage->IsDirty() : false;
    }

    // Destroys a fog actor the verb spawns on a map that has none, and restores the
    // persistent level's dirty flag last.
    FScopedEditorWorldActorGuard ActorGuard;

    // Declared after the guard so it runs BEFORE it: the component fields go back first,
    // then the guard settles the level's dirty flag over the top.
    ON_SCOPE_EXIT
    {
        VolumetricFogCVar->Set(OriginalCVarValue, CVarSetBy);
        if (IsValid(ExistingFog) && ExistingFog->GetComponent())
        {
            ExistingFog->GetComponent()->bEnableVolumetricFog = bOriginalEnableFlag;
            ExistingFog->GetComponent()->VolumetricFogDistance = OriginalDistance;
            ExistingFog->GetComponent()->MarkRenderStateDirty();
        }
        if (FogPackage)
        {
            FogPackage->SetDirtyFlag(bFogPackageWasDirty);
        }
    };

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetNumberField(TEXT("viewDistance"), 6000.0);

    // ---- Case 1: the veto is in force. This is the assertion the old literal could not fail.
    // Read the write back: if a higher-priority setter holds the cvar the premise of the whole
    // test is absent, and asserting on it would report a verb defect that is really a host one.
    VolumetricFogCVar->Set(0, CVarSetBy);
    if (VolumetricFogCVar->GetInt() != 0)
    {
        AddWarning(TEXT("PINWRIGHT_ASSERTIONS_SKIPPED: r.VolumetricFog would not take 0 on this ")
                   TEXT("host, so the renderer veto could not be staged."));
        return true;
    }

    FTestResponseCapture Vetoed;
    TestTrue(TEXT("lighting.setup_volumetric_fog handler found (vetoed run)"),
        InvokeHandlerWithCapture(TEXT("lighting.setup_volumetric_fog"), Payload, Vetoed));
    TestTrue(TEXT("the component write still succeeds while the pass is switched off"),
        Vetoed.bSuccess);
    if (!Vetoed.bSuccess || !Vetoed.Result.IsValid())
    {
        return false;
    }

    bool bVetoedEnabled = true;
    TestTrue(TEXT("the vetoed response carries enabled"),
        Vetoed.Result->TryGetBoolField(TEXT("enabled"), bVetoedEnabled));
    TestFalse(TEXT("enabled is false while r.VolumetricFog is 0"), bVetoedEnabled);

    bool bVetoedComponentFlag = false;
    TestTrue(TEXT("the vetoed response carries componentFlag"),
        Vetoed.Result->TryGetBoolField(TEXT("componentFlag"), bVetoedComponentFlag));
    TestTrue(TEXT("componentFlag reports the write that did happen"), bVetoedComponentFlag);

    const TSharedPtr<FJsonObject>* VetoedCVarInfo = nullptr;
    TestTrue(TEXT("the vetoed response carries volumetricFogCVar"),
        Vetoed.Result->TryGetObjectField(TEXT("volumetricFogCVar"), VetoedCVarInfo));
    if (VetoedCVarInfo && (*VetoedCVarInfo).IsValid())
    {
        FString CVarName;
        (*VetoedCVarInfo)->TryGetStringField(TEXT("cvar"), CVarName);
        TestEqual(TEXT("the reported cvar is r.VolumetricFog"),
            CVarName, FString(TEXT("r.VolumetricFog")));

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
    TestTrue(TEXT("a vetoed call names r.VolumetricFog in cvarWarning"),
        VetoedWarning.Contains(TEXT("r.VolumetricFog")));

    // ---- Case 2: same fixture, same call, veto lifted.
    VolumetricFogCVar->Set(1, CVarSetBy);
    if (VolumetricFogCVar->GetInt() == 0)
    {
        AddWarning(TEXT("PINWRIGHT_ASSERTIONS_SKIPPED: r.VolumetricFog would not take a non-zero ")
                   TEXT("value on this host, so the veto-lifted half of the differential is absent."));
        return true;
    }

    FTestResponseCapture Running;
    TestTrue(TEXT("lighting.setup_volumetric_fog handler found (running run)"),
        InvokeHandlerWithCapture(TEXT("lighting.setup_volumetric_fog"), Payload, Running));
    TestTrue(TEXT("the verb succeeds with the pass running"), Running.bSuccess);
    if (!Running.bSuccess || !Running.Result.IsValid())
    {
        return false;
    }

    bool bRunningEnabled = false;
    TestTrue(TEXT("the running response carries enabled"),
        Running.Result->TryGetBoolField(TEXT("enabled"), bRunningEnabled));
    TestTrue(TEXT("enabled is true once r.VolumetricFog is 1"), bRunningEnabled);
    TestFalse(TEXT("a running pass publishes no cvarWarning"),
        Running.Result->HasField(TEXT("cvarWarning")));

    // The differential itself: a field echoed back from the request scores both runs alike.
    TestTrue(TEXT("enabled differs between the vetoed and the running run"),
        bVetoedEnabled != bRunningEnabled);

    return true;
}
