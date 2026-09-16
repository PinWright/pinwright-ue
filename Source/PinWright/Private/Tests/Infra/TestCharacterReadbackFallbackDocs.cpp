// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression test for E-character-readback-fallback-undocumented:
// the `character` namespace's lone reader `get_character_info` now round-trips the
// nav-agent fields written by configure_nav_movement (NavAgentProps.AgentRadius/
// AgentHeight, bUseRVOAvoidance — emitted as navAgentRadius/navAgentHeight/
// avoidanceEnabled at CharacterHandler.cpp, mirroring the groundFriction/
// brakingDeceleration precedent from F-character-info-no-friction-braking), and
// surfaces the Footstep* variables written by configure_footstep_fx /
// map_surface_to_sound in the movementVariables array (its filter now matches the
// Footstep prefix alongside bIs/bCan/Speed/Movement).
//
// The `## Verifying movement settings` namespace section in docs/wiki-src/character.md
// documents the nav-agent fields as confirmed by get_character_info (no fallback) and
// lists the footstep variables by name, routing only their persisted DEFAULT VALUES
// to blueprint.inspect {includeProperties:true}.
//
// This exercises the live WikiHandler::RenderPage render path (the same entry the
// HTTP gateway uses for doc requests), not a copy of the overlay text. The base
// character.md prelude is a 2-sentence namespace blurb that names no method and no
// readback coverage, so every marker asserted below is overlay-exclusive: reverting
// the `## Verifying movement settings` section makes the renderer return the prelude
// only and these assertions fail.
#include "Misc/AutomationTest.h"
#include "Tests/Infra/WikiDocTestHelpers.h"

// ============================================================================
// Namespace page: the `## Verifying movement settings` section documents the
// get_character_info read-back coverage (nav-agent fields confirmed in-namespace;
// footstep variables listed by name, their defaults via blueprint.inspect). These
// markers live only in the namespace `##` section of docs/wiki-src/character.md.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCharacterReadbackFallbackNamespaceDocTest,
    "PinWright.infra.wiki_handler.Namespace.CharacterReadbackFallback",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCharacterReadbackFallbackNamespaceDocTest::RunTest(const FString& Parameters)
{
    FString Text;
    if (!WikiDocTestHelpers::RenderOrFail(*this, TEXT("character"), Text))
    {
        return false;
    }

    // The verifying-movement-settings section must exist on the namespace page.
    TestTrue(TEXT("character page carries a Verifying movement settings section"),
        Text.Contains(TEXT("Verifying movement settings")));

    // The footstep-FX variables get_character_info now surfaces by name in
    // movementVariables; their persisted DEFAULT VALUES come from blueprint.inspect.
    TestTrue(TEXT("character page names the footstep-FX variables get_character_info lists"),
        Text.Contains(TEXT("FootstepVolumeMultiplier")) && Text.Contains(TEXT("FootstepParticleScale")));
    TestTrue(TEXT("character page routes footstep-FX default values to blueprint.inspect includeProperties"),
        Text.Contains(TEXT("blueprint.inspect")) && Text.Contains(TEXT("includeProperties")));

    // The nav-agent fields are now CONFIRMED by get_character_info in-namespace
    // (emitted as navAgentRadius/navAgentHeight/avoidanceEnabled), no fallback.
    TestTrue(TEXT("character page names the nav-agent fields get_character_info confirms"),
        Text.Contains(TEXT("NavAgentProps.AgentRadius")) && Text.Contains(TEXT("avoidanceEnabled")));

    // groundFriction/brakingDeceleration are documented as CONFIRMED by the
    // reader (the F-character-info-no-friction-braking fix landed), not omitted.
    TestTrue(TEXT("character page documents groundFriction/brakingDeceleration as readable"),
        Text.Contains(TEXT("groundFriction")) && Text.Contains(TEXT("brakingDeceleration")));
    return true;
}
