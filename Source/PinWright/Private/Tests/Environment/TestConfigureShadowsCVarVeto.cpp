// Copyright (c) 2026 Alexander Penkin. MIT License.

// lighting.configure_shadows — `virtualShadowMaps` reports what r.Shadow.Virtual.Enable holds, not
// what the call asked for.
//
// The bug guarded here: the handler echoed the request field straight back beside success:true, so
// nothing it published was read off anything. Two paths drop the write while returning nothing.
// IConsoleManager::FindConsoleVariable is null on a host whose registry does not carry the name, and
// FConsoleVariableBase::CanChange admits a Set only when the incoming priority is >= the one already
// recorded on the variable — the verb writes at the default ECVF_SetByCode, so a console entry
// (ECVF_SetByConsole, the top of the ladder) made earlier in the session bounces it. In both cases
// the response described a shadow configuration that was never applied, which is worse than the two
// sibling defects the neighbouring veto tests guard: those at least wrote the component flag they
// reported.
//
// The assertion is DIFFERENTIAL, not a presence check: the same call is run against the same
// variable with the write refused and with it admitted, and `virtualShadowMaps` must differ between
// the two. A field echoed from the request scores both runs alike, which is exactly how the defect
// survived PinWright.lighting.configure_shadows.ValidParamsNoCrash.
//
// The test restores the cvar's value AND its whole flags word: the staging works by moving the
// recorded SetBy priority, and the variable is process-global.

#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "Dom/JsonObject.h"
#include "HAL/IConsoleManager.h"
#include "Tests/TestSkipReporting.h"
#include "Tests/TestUtils.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLightingConfigureShadowsFollowsCVarVetoTest,
    "PinWright.lighting.configure_shadows.VirtualShadowMapsFollowsCVarVeto",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FLightingConfigureShadowsFollowsCVarVetoTest::RunTest(const FString& Parameters)
{
    IConsoleVariable* ShadowCVar =
        IConsoleManager::Get().FindConsoleVariable(TEXT("r.Shadow.Virtual.Enable"));
    if (!ShadowCVar)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("cvar-absent"),
            TEXT("r.Shadow.Virtual.Enable is not in this host's console registry, so the refused "
                 "write this test drives cannot be staged."));
        return true;
    }

    // The refused Set logs a LogConsoleManager warning ("...was ignored as it is lower priority..."),
    // and the automation framework elevates a captured log warning to a test error by default
    // (UAutomationControllerSettings::bElevateLogWarningsToErrors). Declaring it expected keeps the
    // run green on the very path this test exists to exercise. Negative occurrence = optional: how
    // many times the engine logs a refusal is an engine detail, and the assertions below do the real
    // verification.
    AddExpectedMessagePlain(TEXT("was ignored as it is lower priority"),
        ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, -1);

    const int32 OriginalValue = ShadowCVar->GetInt();
    const EConsoleVariableFlags OriginalFlags = ShadowCVar->GetFlags();

    ON_SCOPE_EXIT
    {
        // ECVF_SetByConsole is the top of the priority ladder, so this write is admitted whatever
        // the test left recorded; SetFlags then puts the priority back where it was.
        ShadowCVar->Set(OriginalValue, ECVF_SetByConsole);
        ShadowCVar->SetFlags(OriginalFlags);
    };

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetBoolField(TEXT("virtualShadowMaps"), true);

    // ---- Case 1: the write is refused. This is the assertion an echoed field could not fail.
    // Pin the variable OFF at ECVF_SetByConsole — exactly what "r.Shadow.Virtual.Enable 0" typed
    // into the editor console leaves behind — so the handler's ECVF_SetByCode write bounces.
    ShadowCVar->Set(0, ECVF_SetByConsole);
    if (ShadowCVar->GetInt() != 0)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("cvar-pinned"),
            TEXT("r.Shadow.Virtual.Enable would not take 0 even at ECVF_SetByConsole on this host, "
                 "so the refused write could not be staged."));
        return true;
    }

    FTestResponseCapture Refused;
    TestTrue(TEXT("lighting.configure_shadows handler found (refused run)"),
        InvokeHandlerWithCapture(TEXT("lighting.configure_shadows"), Payload, Refused));
    TestTrue(TEXT("the verb still succeeds when the write is refused"), Refused.bSuccess);
    if (!Refused.bSuccess || !Refused.Result.IsValid())
    {
        return false;
    }

    bool bRefusedReport = true;
    TestTrue(TEXT("the refused response carries virtualShadowMaps"),
        Refused.Result->TryGetBoolField(TEXT("virtualShadowMaps"), bRefusedReport));
    TestFalse(TEXT("virtualShadowMaps reports the cvar, not the request, when the write is refused"),
        bRefusedReport);

    bool bRefusedRequest = false;
    TestTrue(TEXT("the refused response carries requested"),
        Refused.Result->TryGetBoolField(TEXT("requested"), bRefusedRequest));
    TestTrue(TEXT("requested carries what the call asked for"), bRefusedRequest);

    const TSharedPtr<FJsonObject>* RefusedCVarInfo = nullptr;
    TestTrue(TEXT("the refused response carries shadowVirtualEnableCVar"),
        Refused.Result->TryGetObjectField(TEXT("shadowVirtualEnableCVar"), RefusedCVarInfo));
    if (RefusedCVarInfo && (*RefusedCVarInfo).IsValid())
    {
        FString CVarName;
        (*RefusedCVarInfo)->TryGetStringField(TEXT("cvar"), CVarName);
        TestEqual(TEXT("the reported cvar is r.Shadow.Virtual.Enable"),
            CVarName, FString(TEXT("r.Shadow.Virtual.Enable")));

        bool bFound = false;
        (*RefusedCVarInfo)->TryGetBoolField(TEXT("found"), bFound);
        TestTrue(TEXT("the cvar was measured, not assumed"), bFound);

        double MeasuredValue = -1.0;
        TestTrue(TEXT("the measured cvar value is published"),
            (*RefusedCVarInfo)->TryGetNumberField(TEXT("value"), MeasuredValue));
        TestEqual(TEXT("the measured cvar value is 0"), MeasuredValue, 0.0);
    }

    FString RefusedWarning;
    Refused.Result->TryGetStringField(TEXT("cvarWarning"), RefusedWarning);
    TestTrue(TEXT("a refused write names r.Shadow.Virtual.Enable in cvarWarning"),
        RefusedWarning.Contains(TEXT("r.Shadow.Virtual.Enable")));

    // ---- Case 2: same call, same variable, the write admitted.
    // Drop the recorded priority below ECVF_SetByCode rather than restoring OriginalFlags: on a host
    // that arrived at this test with a console entry already holding the variable, restoring would
    // stage a second refusal and the differential would collapse into two identical runs.
    const uint32 PreservedFlags =
        static_cast<uint32>(OriginalFlags) & ~static_cast<uint32>(ECVF_SetByMask);
    ShadowCVar->SetFlags(static_cast<EConsoleVariableFlags>(
        PreservedFlags | static_cast<uint32>(ECVF_SetByScalability)));

    FTestResponseCapture Admitted;
    TestTrue(TEXT("lighting.configure_shadows handler found (admitted run)"),
        InvokeHandlerWithCapture(TEXT("lighting.configure_shadows"), Payload, Admitted));
    TestTrue(TEXT("the verb succeeds when the write lands"), Admitted.bSuccess);
    if (!Admitted.bSuccess || !Admitted.Result.IsValid())
    {
        return false;
    }

    // Read the write back before asserting on the response: if the variable would not take 1 even at
    // scalability priority the premise of this half is absent, and asserting on it would report a
    // verb defect that is really a host one.
    if (ShadowCVar->GetInt() == 0)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("cvar-pinned"),
            TEXT("r.Shadow.Virtual.Enable would not take 1 on this host once the recorded priority "
                 "was dropped, so the admitted half of the differential is absent."));
        return true;
    }

    bool bAdmittedReport = false;
    TestTrue(TEXT("the admitted response carries virtualShadowMaps"),
        Admitted.Result->TryGetBoolField(TEXT("virtualShadowMaps"), bAdmittedReport));
    TestTrue(TEXT("virtualShadowMaps is true once the write lands"), bAdmittedReport);
    TestFalse(TEXT("an applied write publishes no cvarWarning"),
        Admitted.Result->HasField(TEXT("cvarWarning")));

    // The differential itself: a field echoed back from the request scores both runs alike.
    TestTrue(TEXT("virtualShadowMaps differs between the refused and the admitted run"),
        bRefusedReport != bAdmittedReport);

    return true;
}
