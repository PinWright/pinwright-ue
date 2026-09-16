// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression test for E-eqs-context-class-no-project-discovery-path:
// `eqs.set_context_class` accepts only the built-in context tokens
// (querier|item|navigationdata|blueprintbase) or a full UEnvQueryContext class
// path. A user-named "Player" context is none of those, and `blueprintbase`
// resolves the ABSTRACT EnvQueryContext_BlueprintBase base, not a usable named
// context. The discovery cross-link (asset.list) shipped via
// E-eqs-builtin-token-discovery #2, but it is necessary-but-not-sufficient when
// the project ships no context to discover: at that point the agent must AUTHOR
// one with blueprint.create {parent: EnvQueryContext_BlueprintBase} and pass its
// generated _C class path. That authoring fallback was undocumented on the EQS
// surface (there is no EQS-context-create verb; ai.add_eqs_context is a deprecated
// alias for this assign method). The fix appends the authoring-route sentence to
// the `### eqs.set_context_class` H3 overlay section in docs/wiki-src/eqs.md.
//
// This exercises the live WikiHandler::RenderPage render path (the same entry the
// HTTP gateway uses for doc requests), not a copy of the overlay text. The markers
// asserted below are overlay-exclusive: the auto-generated `eqs.set_context_class`
// summary and the bare `contextClass` param description name no blueprint.create
// authoring route, no EnvQueryContext_BlueprintBase parent, and no "deprecated
// alias" note — so the H3 section surfaces only when this method page is rendered
// directly, and reverting the overlay append makes these assertions fail.
#include "Misc/AutomationTest.h"
#include "Tests/Infra/WikiDocTestHelpers.h"

// ============================================================================
// Method page: the `### eqs.set_context_class` H3 documents the authoring fallback
// for when no project context exists — blueprint.create against the
// EnvQueryContext_BlueprintBase parent, passing the generated _C class path. The
// H3 overlay surfaces only when the method page is rendered directly (not on the
// namespace page), so these markers live only in the H3 section of
// docs/wiki-src/eqs.md.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEqsContextAuthoringDocTest,
    "PinWright.infra.wiki_handler.MethodPage.EqsContextAuthoringRoute",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FEqsContextAuthoringDocTest::RunTest(const FString& Parameters)
{
    FString Text;
    if (!WikiDocTestHelpers::RenderOrFail(*this, TEXT("eqs.set_context_class"), Text))
    {
        return false;
    }

    // The authoring route: blueprint.create against the EnvQueryContext_BlueprintBase
    // parent. Overlay-exclusive — the param description names neither.
    TestTrue(TEXT("set_context_class page documents the blueprint.create authoring route"),
        Text.Contains(TEXT("blueprint.create")));
    TestTrue(TEXT("set_context_class page names the EnvQueryContext_BlueprintBase parent for authoring"),
        Text.Contains(TEXT("EnvQueryContext_BlueprintBase")));

    // Pass the generated _C class path of the authored context back to this method.
    // Anchored to the overlay's exact phrase (the bare "_C" substring was too loose
    // — any class-path token or _Config/_Cache word would satisfy it).
    TestTrue(TEXT("set_context_class page steers to the generated _C class path of the authored context"),
        Text.Contains(TEXT("generated `_C` class path")));

    // There is no EQS-context-create verb: ai.add_eqs_context is a deprecated alias
    // for this assign method, so the author must use blueprint.create.
    TestTrue(TEXT("set_context_class page notes ai.add_eqs_context is a deprecated alias (no create verb)"),
        Text.Contains(TEXT("ai.add_eqs_context")) && Text.Contains(TEXT("deprecated alias")));

    return true;
}
