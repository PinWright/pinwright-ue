// Copyright (c) 2026 Alexander Penkin. MIT License.

// Unit tests for post_process.* typed setters in PostProcessHandler.cpp.
//
// The bloom test directly guards the foot-gun the ticket attacks: it asserts
// BOTH the value (`BloomIntensity == 2.5f`) AND the override flag
// (`bOverride_BloomIntensity == true`). If a future refactor drops the
// `bOverride_BloomIntensity = true` flip but keeps the value write, the value
// assertion passes and the flag assertion fails — pinpointing the regression.
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "Dom/JsonObject.h"
#include "Engine/PostProcessVolume.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Editor.h"
#include "HAL/IConsoleManager.h"
#include "Tests/TestSkipReporting.h"
#include "Tests/TestUtils.h"
// FindUnboundPPV / TestPPVSetterEchoesAppliedValues live in TestUtils.h, shared by the
// bloom test below and the lighting.set_* echo tests.

// ---- post_process.set_bloom — bOverride flip + value write ----
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPostProcessSetBloomFlipsOverrideFlagTest,
    "PinWright.post_process.set_bloom.FlipsOverrideFlag",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPostProcessSetBloomFlipsOverrideFlagTest::RunTest(const FString& Parameters)
{
    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("Editor world not available — skipping post_process.set_bloom test"));
        return true;
    }

    // Track whether we (the test) spawned the PPV — only destroy in that case to
    // avoid clobbering a fixture left behind by a prior test or the level.
    const bool bPreExistingPPV = (FindUnboundPPV(World) != nullptr);

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetNumberField(TEXT("intensity"), 2.5);

    TestTrue(TEXT("post_process.set_bloom handler found"),
        InvokeHandler(TEXT("post_process.set_bloom"), Payload));

    APostProcessVolume* PPV = FindUnboundPPV(World);
    TestNotNull(TEXT("Unbound APostProcessVolume present after set_bloom"), PPV);
    if (!PPV) return false;

    ON_SCOPE_EXIT
    {
        if (!bPreExistingPPV && PPV)
        {
            PPV->Destroy();
        }
    };

    // Value assertion.
    TestEqual(TEXT("BloomIntensity round-trips"),
        PPV->Settings.BloomIntensity, 2.5f);

    // Foot-gun assertion: bOverride_BloomIntensity must be flipped to true.
    // If the handler ever stops flipping the bOverride flag (the exact regression
    // these typed setters guard against), this is the assertion that fires.
    TestTrue(TEXT("bOverride_BloomIntensity was flipped to true"),
        PPV->Settings.bOverride_BloomIntensity != 0);

    return true;
}

// ---- lighting.set_ambient_occlusion — response echoes the applied AO values ----
// Guards board E-lighting-set-ao-exposure-no-echo: the setter must carry the
// intensity/radius it just wrote (and `enabled`, derived from the applied intensity)
// in its own success response, so a caller never needs a property.get round-trip on
// FPostProcessSettings to confirm the write. If a refactor reverts the echo back to
// {success, actorName} + verification only, the TryGetNumberField/TryGetBoolField
// assertions in the shared helper fire.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLightingSetAmbientOcclusionEchoesAppliedValuesTest,
    "PinWright.lighting.set_ambient_occlusion.EchoesAppliedValues",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FLightingSetAmbientOcclusionEchoesAppliedValuesTest::RunTest(const FString& Parameters)
{
    // `enabled` is MEASURED by the verb, not echoed: it is false whenever
    // r.AmbientOcclusionLevels is 0, because that cvar vetoes the whole screen-space AO pass
    // (see EnabledFollowsCVarVeto, which asserts exactly that). BaseScalability.ini zeroes the
    // cvar in [PostProcessQuality@0], so on a host that comes up at low post-process quality
    // the expectation below is about the host's scalability rather than the verb. Stage the
    // no-veto side explicitly so this test measures the echo it is named for.
    IConsoleVariable* AOLevelsCVar =
        IConsoleManager::Get().FindConsoleVariable(TEXT("r.AmbientOcclusionLevels"));
    // Write at the priority the variable ALREADY has (the engine's guard is >=, not >), so the
    // test never leaves the cvar pinned at Code priority outranking scalability for the rest of
    // the run - same rule as the sibling veto test.
    const int32 OriginalCVarValue = AOLevelsCVar ? AOLevelsCVar->GetInt() : 0;
    const EConsoleVariableFlags CVarSetBy = AOLevelsCVar
        ? static_cast<EConsoleVariableFlags>(AOLevelsCVar->GetFlags() & ECVF_SetByMask)
        : ECVF_SetByCode;
    ON_SCOPE_EXIT
    {
        if (AOLevelsCVar)
        {
            AOLevelsCVar->Set(OriginalCVarValue, CVarSetBy);
        }
    };
    if (AOLevelsCVar)
    {
        // -1 means "decide from the post-process settings", the only value that leaves the
        // applied intensity as the sole input to `enabled`.
        AOLevelsCVar->Set(-1, CVarSetBy);
        if (AOLevelsCVar->GetInt() == 0)
        {
            PinWrightTestSkip::SkipAssertions(*this, TEXT("cvar-pinned"),
                TEXT("r.AmbientOcclusionLevels is held at 0 by a higher-priority setter on this "
                     "host, so the AO pass is vetoed and `enabled` cannot be true for any write."));
            return true;
        }
    }

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetNumberField(TEXT("intensity"), 0.6);
    Payload->SetNumberField(TEXT("radius"), 120.0);

    // intensity/radius echoed inline, plus `enabled` (measured from the applied intensity with
    // the cvar veto lifted above: AO on iff intensity > 0, so 0.6 -> enabled:true).
    TestPPVSetterEchoesAppliedValues(*this, TEXT("lighting.set_ambient_occlusion"), Payload,
        { { TEXT("intensity"), 0.6f }, { TEXT("radius"), 120.0f } },
        { { TEXT("enabled"), true } });
    return true;
}

// ---- lighting.set_exposure — response echoes the applied exposure values ----
// Sibling guard for board E-lighting-set-ao-exposure-no-echo on the exposure setter:
// the minBrightness/maxBrightness/compensationValue it writes to the AutoExposure*
// fields must come back inline. Reverting the echo trips the assertions below.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLightingSetExposureEchoesAppliedValuesTest,
    "PinWright.lighting.set_exposure.EchoesAppliedValues",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FLightingSetExposureEchoesAppliedValuesTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetNumberField(TEXT("minBrightness"), 0.25);
    Payload->SetNumberField(TEXT("maxBrightness"), 4.0);
    Payload->SetNumberField(TEXT("compensationValue"), 1.5);

    TestPPVSetterEchoesAppliedValues(*this, TEXT("lighting.set_exposure"), Payload,
        { { TEXT("minBrightness"), 0.25f },
          { TEXT("maxBrightness"), 4.0f },
          { TEXT("compensationValue"), 1.5f } });
    return true;
}

// ---- post_process.set_anti_aliasing — CVar round-trip ----
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPostProcessSetAntiAliasingWritesScreenPercentageCVarTest,
    "PinWright.post_process.set_anti_aliasing.WritesScreenPercentageCVar",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPostProcessSetAntiAliasingWritesScreenPercentageCVarTest::RunTest(const FString& Parameters)
{
    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("Editor world not available — skipping anti_aliasing CVar test"));
        return true;
    }

    IConsoleVariable* ScreenPctCVar =
        IConsoleManager::Get().FindConsoleVariable(TEXT("r.ScreenPercentage"));
    TestNotNull(TEXT("r.ScreenPercentage CVar is registered"), ScreenPctCVar);
    if (!ScreenPctCVar) return false;

    const float OriginalScreenPct = ScreenPctCVar->GetFloat();
    const bool bPreExistingPPV = (FindUnboundPPV(World) != nullptr);

    APostProcessVolume* PPV = nullptr;

    ON_SCOPE_EXIT
    {
        // Always restore the CVar so we don't pollute other tests.
        if (ScreenPctCVar)
        {
            ScreenPctCVar->Set(OriginalScreenPct, ECVF_SetByCode);
        }
        if (!bPreExistingPPV && PPV)
        {
            PPV->Destroy();
        }
    };

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetNumberField(TEXT("screenPercentage"), 73.0);

    TestTrue(TEXT("post_process.set_anti_aliasing handler found"),
        InvokeHandler(TEXT("post_process.set_anti_aliasing"), Payload));

    PPV = FindUnboundPPV(World);
    TestEqual(TEXT("r.ScreenPercentage was written"),
        ScreenPctCVar->GetFloat(), 73.0f);

    return true;
}

// ---- post_process.set_anti_aliasing — invalid `method` token is rejected, not a silent success ----
// Guards board B-set-aa-invalid-method-silent-noop: an unrecognized documented-enum
// token (e.g. "TAAU") must fail loud with INVALID_PARAMS rather than returning a clean
// {success:true} while leaving r.AntiAliasingMethod untouched. The three cases together
// pin the exact contract the fix establishes: (1) an invalid token errors with the right
// code and mutates nothing; (2) a valid token still succeeds, echoes back, and writes the
// CVar (proving no over-rejection); (3) omitting `method` entirely still succeeds (the
// optional-param path the fix must preserve). Reverting the validate-before-mutate guard
// turns case 1 back into a silent success and fails the bSuccess/ErrorCode assertions.
// The handler writes only CVars (no PostProcessVolume / world), so no editor world is
// needed and the fixture is built entirely in-code.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPostProcessSetAntiAliasingRejectsInvalidMethodTest,
    "PinWright.post_process.set_anti_aliasing.RejectsInvalidMethod",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPostProcessSetAntiAliasingRejectsInvalidMethodTest::RunTest(const FString& Parameters)
{
    IConsoleVariable* AACVar =
        IConsoleManager::Get().FindConsoleVariable(TEXT("r.AntiAliasingMethod"));
    TestNotNull(TEXT("r.AntiAliasingMethod CVar is registered"), AACVar);
    if (!AACVar) return false;

    const int32 OriginalAA = AACVar->GetInt();
    ON_SCOPE_EXIT
    {
        // Restore the CVar so the valid-token write below doesn't pollute other tests.
        AACVar->Set(OriginalAA, ECVF_SetByCode);
    };

    // Case 1: an unrecognized token must fail loud, not fake-succeed, and must not write the CVar.
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("method"), TEXT("TAAU"));

        FTestResponseCapture Capture;
        TestTrue(TEXT("set_anti_aliasing handler registered"),
            InvokeHandlerWithCapture(TEXT("post_process.set_anti_aliasing"), Payload, Capture));
        TestFalse(TEXT("invalid method is rejected, not a silent success"), Capture.bSuccess);
        TestEqual(TEXT("invalid method error code is INVALID_PARAMS"),
            Capture.ErrorCode, FString(TEXT("INVALID_PARAMS")));
        TestEqual(TEXT("r.AntiAliasingMethod is untouched after a rejected call"),
            AACVar->GetInt(), OriginalAA);
    }

    // Case 2: a valid token still succeeds, echoes it back, and writes the CVar (no over-rejection).
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("method"), TEXT("TSR"));

        FTestResponseCapture Capture;
        TestTrue(TEXT("set_anti_aliasing handler registered (valid)"),
            InvokeHandlerWithCapture(TEXT("post_process.set_anti_aliasing"), Payload, Capture));
        TestTrue(TEXT("valid method succeeds"), Capture.bSuccess);
        if (Capture.bSuccess && Capture.Result.IsValid())
        {
            FString Echoed;
            TestTrue(TEXT("valid method echoes antiAliasingMethod"),
                Capture.Result->TryGetStringField(TEXT("antiAliasingMethod"), Echoed));
            TestEqual(TEXT("echoed antiAliasingMethod is TSR"), Echoed, FString(TEXT("TSR")));
        }
        TestEqual(TEXT("r.AntiAliasingMethod written to TSR (4)"), AACVar->GetInt(), 4);
    }

    // Case 3: omitting `method` entirely must still succeed — the optional-param path the
    // fix must preserve (reject only a method that is present-but-unresolved, never absent).
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        FTestResponseCapture Capture;
        TestTrue(TEXT("set_anti_aliasing handler registered (no method)"),
            InvokeHandlerWithCapture(TEXT("post_process.set_anti_aliasing"), Payload, Capture));
        TestTrue(TEXT("omitting method still succeeds"), Capture.bSuccess);
    }

    return true;
}
