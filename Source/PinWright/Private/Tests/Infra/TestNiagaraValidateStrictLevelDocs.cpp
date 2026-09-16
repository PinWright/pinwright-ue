// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression test for E-niagara-validate-strict-empty-system-undocumented:
// niagara.validate accepts `level: basic | strict`, but the only doc surface for `level`
// was the bare param spec "Validation level: basic or strict. Defaults basic."
// (NiagaraInspectHandler.cpp:289) — it never stated that `level: strict` promotes three
// normally-`warning` structural codes to hard `error`. NormalizeValidationSeverity
// (NiagaraInspectHandler.cpp:99-109) escalates exactly NO_EMITTERS / DISABLED_EMITTER /
// NO_RENDERERS to "error" only under level==strict; those codes are raised as warnings in
// NiagaraDumpBuilder.cpp (NO_EMITTERS :1561, NO_RENDERERS :1581/:1646) and DISABLED_EMITTER
// via NiagaraInspectHandler.cpp:184. The escalation is intentional and locked by the
// behavior test FNiagaraValidateStrictNoEmittersErrorTest, so the correct lever is docs,
// not code.
//
// The fix adds a `### niagara.validate` H3 overlay section to docs/wiki-src/niagara.md
// documenting (a) the three strict-escalated codes and the warning->error promotion,
// (b) that they fire by design on a freshly-authored/empty system — a strict NO_EMITTERS
// on a `niagara.create_system` system (or NO_RENDERERS on a blank emitter) is inherent to
// the unfinished asset, so use the default `basic` level for an in-progress system — and
// (c) that the top-level errors/valid apply the normalization and are authoritative over
// the raw nested `compile.issues` severities.
//
// This renders through the live WikiHandler::RenderPage method-page path (the same entry
// the HTTP gateway serves doc requests from), surfacing the H3 via
// WikiOverlay::LoadMethodSection("niagara.validate") — NOT a copy of the overlay text.
// Every marker asserted below is overlay-exclusive: the auto method summary ("Validate a
// Niagara system, emitter, or script asset and return structured issues.") and the auto
// param specs name none of these codes, never say "warning"/"error", and never mention
// create_system or compile.issues — so reverting the H3 makes LoadMethodSection return
// empty, RenderMethodPage drops the `## Notes` block, and these assertions fail.
#include "Misc/AutomationTest.h"
#include "Tests/Infra/WikiDocTestHelpers.h"

// ============================================================================
// niagara.validate method page: the `### niagara.validate` H3 must document the
// strict-level severity escalation (the three promoted codes, the by-design
// empty-system trigger + basic-level steer, and the authoritative top-level layer).
// The H3 surfaces only when this method page is rendered directly (LoadMethodSection
// keys on the exact method name), so these markers live only in the overlay section.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNiagaraValidateStrictLevelDocTest,
    "PinWright.infra.wiki_handler.MethodPage.NiagaraValidateStrictLevel",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraValidateStrictLevelDocTest::RunTest(const FString& Parameters)
{
    FString Text;
    if (!WikiDocTestHelpers::RenderOrFail(*this, TEXT("niagara.validate"), Text))
    {
        return false;
    }

    // (a) The page names all three codes that level:strict escalates from warning to error.
    // The auto param spec ("Validation level: basic or strict. Defaults basic.") names none
    // of these, so they can only come from the H3 overlay section.
    TestTrue(TEXT("niagara.validate page names the strict-escalated NO_EMITTERS code"),
        Text.Contains(TEXT("NO_EMITTERS")));
    TestTrue(TEXT("niagara.validate page names the strict-escalated DISABLED_EMITTER code"),
        Text.Contains(TEXT("DISABLED_EMITTER")));
    TestTrue(TEXT("niagara.validate page names the strict-escalated NO_RENDERERS code"),
        Text.Contains(TEXT("NO_RENDERERS")));

    // (b) It documents the warning->error promotion direction (neither word is in the auto
    // summary or param specs, so this is overlay-exclusive).
    TestTrue(TEXT("niagara.validate page documents the warning->error strict promotion"),
        Text.Contains(TEXT("warning")) && Text.Contains(TEXT("error")));

    // (c) It steers agents to basic on a freshly-created / empty system, naming the
    // create_system origin of the guaranteed strict NO_EMITTERS error.
    TestTrue(TEXT("niagara.validate page ties the guaranteed empty-system error to create_system"),
        Text.Contains(TEXT("create_system")));

    // (d) It documents the authoritative-layer nuance: the top-level errors/valid win over
    // the raw nested compile.issues severities.
    TestTrue(TEXT("niagara.validate page notes the top-level severity is authoritative over compile.issues"),
        Text.Contains(TEXT("compile.issues")) && Text.Contains(TEXT("authoritative")));
    return true;
}

// ============================================================================
// niagara.validate method page, second overlay contract: the script-compile verdict.
//
// Regression cover for B-niagara-validate-green-while-scripts-ncs-error. The page used to
// discuss compile state only in terms of the null / NCS_Unknown "not compiled yet" state and
// told a caller to treat the top level as authoritative and ignore the nested block -- while
// NCS_Error appeared ONLY in the nested block, so a caller following the documented rule could
// not see a system whose scripts had failed to compile. The code fix promotes NCS_Error into
// the top-level verdict; this asserts the page says so, since a caller who does not know the
// promotion exists still has no reason to trust `valid` over a manual scan of compile.scripts.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNiagaraValidateScriptCompileDocTest,
    "PinWright.infra.wiki_handler.MethodPage.NiagaraValidateScriptCompile",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraValidateScriptCompileDocTest::RunTest(const FString& Parameters)
{
    FString Text;
    if (!WikiDocTestHelpers::RenderOrFail(*this, TEXT("niagara.validate"), Text))
    {
        return false;
    }

    // The failing state has a name, and the page uses it.
    TestTrue(TEXT("niagara.validate page names the NCS_Error compile status"),
        Text.Contains(TEXT("NCS_Error")));
    TestTrue(TEXT("niagara.validate page names the script-compile issue code"),
        Text.Contains(TEXT("NIAGARA_SCRIPT_COMPILE_ERROR")));

    // The three-state verdict field, so "nothing has compiled this yet" is not read as a pass.
    TestTrue(TEXT("niagara.validate page documents the scriptCompileCheck verdict field"),
        Text.Contains(TEXT("scriptCompileCheck")));
    TestTrue(TEXT("niagara.validate page documents the unverified script-compile verdict"),
        Text.Contains(TEXT("unverified")));

    // The pending-compile state and where waiting belongs.
    TestTrue(TEXT("niagara.validate page documents the pendingCompile field"),
        Text.Contains(TEXT("pendingCompile")));
    TestTrue(TEXT("niagara.validate page names the pending-compile issue code"),
        Text.Contains(TEXT("NIAGARA_COMPILE_PENDING")));

    // And why compile.valid is not the field to branch on.
    TestTrue(TEXT("niagara.validate page explains why compile.valid is not promoted"),
        Text.Contains(TEXT("compile.valid")));
    return true;
}
