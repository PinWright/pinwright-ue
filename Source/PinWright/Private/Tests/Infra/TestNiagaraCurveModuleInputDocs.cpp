// Copyright (c) 2026 Alexander Penkin. MIT License.

// Doc contract for B-niagara-set-curve-keys-unreachable-module-input-di.
//
// The code fix teaches niagara.set_curve_keys / niagara.get_curve_keys / niagara.bind_curve_asset a
// second addressing form (`entryId` + `inputName`), because no parameter store holds a stack
// module's curve data interface. A caller who does not know that form exists still cannot author an
// over-life ramp — three reporters cycled every `scope` + `parameterName` spelling before filing —
// so the capability is only half-delivered without the page saying which form reaches which object.
//
// These render through the live WikiHandler::RenderPage method-page path via
// WikiOverlay::LoadMethodSection, not a copy of the overlay text. Every marker asserted below is
// overlay-exclusive: the auto method summary and the auto param specs never mention the parameter
// stores holding no module curve, never name valueMode, and never explain the override-creation
// rule — so deleting the H3 makes LoadMethodSection return empty, RenderMethodPage drops the
// `## Notes` block, and these assertions fail.

#include "Misc/AutomationTest.h"
#include "Tests/Infra/WikiDocTestHelpers.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNiagaraSetCurveKeysModuleInputDocTest,
    "PinWright.infra.wiki_handler.MethodPage.NiagaraSetCurveKeysModuleInput",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraSetCurveKeysModuleInputDocTest::RunTest(const FString& Parameters)
{
    FString Text;
    if (!WikiDocTestHelpers::RenderOrFail(*this, TEXT("niagara.set_curve_keys"), Text))
    {
        return false;
    }

    // (a) Both addressing forms are named, with the module-input keys spelled as sent.
    TestTrue(TEXT("set_curve_keys page names the module-input entryId key"),
        Text.Contains(TEXT("entryId")));
    TestTrue(TEXT("set_curve_keys page names the module-input inputName key"),
        Text.Contains(TEXT("inputName")));

    // (b) The reason the store form cannot reach a stack curve — the fact three reporters had to
    // discover by exhausting spellings.
    TestTrue(TEXT("set_curve_keys page states no parameter store holds a stack module's curve"),
        Text.Contains(TEXT("parameter store")));
    TestTrue(TEXT("set_curve_keys page names a stock module whose curve needs the module-input form"),
        Text.Contains(TEXT("ScaleSpriteSize")));

    // (c) The override-creation rule, which is what keeps a write off the shared module asset.
    TestTrue(TEXT("set_curve_keys page documents the createdOverride report"),
        Text.Contains(TEXT("createdOverride")));
    TestTrue(TEXT("set_curve_keys page explains the default-valued input creates its own override"),
        Text.Contains(TEXT("valueMode")));

    // (d) The refusal a caller will actually hit, named so it can be searched.
    TestTrue(TEXT("set_curve_keys page names the unknown-input refusal code"),
        Text.Contains(TEXT("MODULE_INPUT_NOT_FOUND")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNiagaraGetCurveKeysDocTest,
    "PinWright.infra.wiki_handler.MethodPage.NiagaraGetCurveKeys",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraGetCurveKeysDocTest::RunTest(const FString& Parameters)
{
    FString Text;
    if (!WikiDocTestHelpers::RenderOrFail(*this, TEXT("niagara.get_curve_keys"), Text))
    {
        return false;
    }

    // The read gap this verb closes: four of the ticket's nine encounters were blocked by the
    // absence of ANY surface emitting curve samples, so the page has to say this is the one.
    TestTrue(TEXT("get_curve_keys page states the other read surfaces do not emit curve samples"),
        Text.Contains(TEXT("nir.txt")) || Text.Contains(TEXT("decompile_nir")));
    TestTrue(TEXT("get_curve_keys page distinguishes the override object from the module script default"),
        Text.Contains(TEXT("valueMode")) && Text.Contains(TEXT("default")));
    return true;
}
