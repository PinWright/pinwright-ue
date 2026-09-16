// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Misc/AutomationTest.h"
#include "Catalog/WikiHandler.h"
#include "Dispatch/RpcDispatcher.h"
#include "Handlers/HandlerRegistration.h"
#include "Tests/Infra/WikiDocTestHelpers.h"

// The wiki renderer is invoked directly by the HTTP gateway when a request body
// omits its `params` field; these tests drive the same entry point.

// ============================================================================
// Resolve: rendering "runtime-uobject-inspection" returns the page body.
// Counterfactual: if the `Topic` arm in RenderPage's classification is reverted,
// classification falls through to `NotFound`, the response text starts with
// `# Not found:`, and the substring assertion fails.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWikiHandlerTopicPageResolvesTest,
    "PinWright.infra.wiki_handler.TopicPage.Resolves",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWikiHandlerTopicPageResolvesTest::RunTest(const FString& Parameters)
{
    FString Text;
    if (!WikiHandler::RenderPage(TEXT("runtime-uobject-inspection"), Text))
    {
        AddError(TEXT("Wiki renderer unavailable (no editor subsystem/dispatcher)"));
        return false;
    }

    TestTrue(TEXT("topic page body contains a known prelude marker"),
        Text.Contains(TEXT("Step 1: targeting the PIE world")));
    return true;
}

// ============================================================================
// Fuzzy: rendering "runtime-uo" lists the topic slug as a suggestion.
// Counterfactual: without the `Cache.SuggestionPool.Add(slug)` additions for
// topic slugs, `RankSuggestions` has no registry entry containing that
// substring and the assertion fails.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWikiHandlerTopicPageInFuzzyTest,
    "PinWright.infra.wiki_handler.TopicPage.FuzzySuggestion",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWikiHandlerTopicPageInFuzzyTest::RunTest(const FString& Parameters)
{
    FString Text;
    if (!WikiHandler::RenderPage(TEXT("runtime-uo"), Text))
    {
        AddError(TEXT("Wiki renderer unavailable (no editor subsystem/dispatcher)"));
        return false;
    }

    TestTrue(TEXT("fuzzy result mentions runtime-uobject-inspection"),
        Text.Contains(TEXT("runtime-uobject-inspection")));
    return true;
}

// ============================================================================
// Category not shadowed: rendering "blueprint" still renders the namespace index.
// Counterfactual: if discovery doesn't exclude slugs already in `CategoryNodes`,
// classification orders `Topic` before namespace classification and the
// topic-page render (which lacks `## Subgroups`) is returned; the assertion
// fails. This guards the namespace-not-shadowed acceptance criterion.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWikiHandlerCategoryWinsTest,
    "PinWright.infra.wiki_handler.TopicPage.CategoryNotShadowed",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWikiHandlerCategoryWinsTest::RunTest(const FString& Parameters)
{
    FString Text;
    if (!WikiHandler::RenderPage(TEXT("blueprint"), Text))
    {
        AddError(TEXT("Wiki renderer unavailable (no editor subsystem/dispatcher)"));
        return false;
    }

    const bool bHasNamespaceMarker =
        Text.Contains(TEXT("## Subgroups")) || Text.Contains(TEXT("## Methods"));
    TestTrue(TEXT("blueprint page renders as a namespace index, not a topic page"),
        bHasNamespaceMarker);
    return true;
}

// ============================================================================
// Underscore namespace resolves: rendering "gameplay_tags" (a real single-token
// namespace whose name legitimately contains an underscore) returns the
// namespace index, not a Not-Found page.
// Counterfactual: if RenderPage reverts to running every path through the
// unconditional underscore->dot collapse (NormalizeQuery) before ClassifyNode,
// "gameplay_tags" mangles to "gameplay.tags", which names no registered node, so
// classification falls through to NotFound, the body starts with "# Not found:",
// and the assertions below fail. This pins B-wiki-namespace-underscore-not-found.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWikiHandlerUnderscoreNamespaceResolvesTest,
    "PinWright.infra.wiki_handler.Namespace.UnderscoreResolves",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWikiHandlerUnderscoreNamespaceResolvesTest::RunTest(const FString& Parameters)
{
    FString Text;
    if (!WikiHandler::RenderPage(TEXT("gameplay_tags"), Text))
    {
        AddError(TEXT("Wiki renderer unavailable (no editor subsystem/dispatcher)"));
        return false;
    }

    TestFalse(TEXT("underscore namespace page is not a Not-Found page"),
        Text.Contains(TEXT("# Not found:")));
    TestTrue(TEXT("underscore namespace page renders the auto-generated method index"),
        Text.Contains(TEXT("## Methods")));
    return true;
}

// ============================================================================
// NormalizeSlug preserves a real underscore namespace verbatim so the transport's
// on-disk page lookup (game_framework.md) matches the slug the disk generator
// wrote, instead of probing the mangled game.framework.md and missing.
// Counterfactual: if NormalizeSlug reverts to the pure underscore->dot collapse,
// it returns "gameplay.tags" for "gameplay_tags" and the equality assertion fails.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWikiHandlerNormalizeSlugUnderscoreTest,
    "PinWright.infra.wiki_handler.NormalizeSlug.UnderscoreNamespacePreserved",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWikiHandlerNormalizeSlugUnderscoreTest::RunTest(const FString& Parameters)
{
    // A real underscore namespace must round-trip unchanged so the disk-path probe hits.
    TestEqual(TEXT("real underscore namespace slug is preserved verbatim"),
        WikiHandler::NormalizeSlug(TEXT("gameplay_tags")), FString(TEXT("gameplay_tags")));

    // A legacy flat tool name that does not resolve verbatim still collapses its
    // first underscore to a dot — the original heuristic is retained as the fallback.
    TestEqual(TEXT("unresolved flat name still collapses first underscore to dot"),
        WikiHandler::NormalizeSlug(TEXT("nonexistent_namespace_slug")),
        FString(TEXT("nonexistent.namespace_slug")));
    return true;
}

// ============================================================================
// add_switch method page documents that it produces a comparator
// MaterialExpressionIf (not a bool true/false switch) and states the selector
// convention. Rendering material.authoring.add_switch loads the `###
// material.authoring.add_switch` H3 overlay section via WikiOverlay::LoadMethodSection.
// Counterfactual: if the H3 overlay section in docs/wiki-src/material.authoring.md
// is reverted/removed, LoadMethodSection returns empty, the rendered page omits
// the "## Notes" block, and the MaterialExpressionIf / pin-convention assertions
// fail. This pins E-add-switch-is-comparator-if-undocumented.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWikiHandlerAddSwitchDocumentsIfTest,
    "PinWright.infra.wiki_handler.MethodPage.AddSwitchDocumentsIf",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWikiHandlerAddSwitchDocumentsIfTest::RunTest(const FString& Parameters)
{
    FString Text;
    if (!WikiHandler::RenderPage(TEXT("material.authoring.add_switch"), Text))
    {
        AddError(TEXT("Wiki renderer unavailable (no editor subsystem/dispatcher)"));
        return false;
    }

    TestFalse(TEXT("add_switch method page is not a Not-Found page"),
        Text.Contains(TEXT("# Not found:")));
    TestTrue(TEXT("add_switch page states it creates a MaterialExpressionIf comparator"),
        Text.Contains(TEXT("MaterialExpressionIf")));
    // The selector convention must be discoverable from the method page itself.
    TestTrue(TEXT("add_switch page documents the comparator selector pins"),
        Text.Contains(TEXT("AGreaterThanB")));
    return true;
}

// ============================================================================
// input.add_mapping method page's code Example uses the real required param key
// `contextPath`, matching the handler (RPC_PARAM_REQ("contextPath", ...)) and the
// auto-generated Parameters list. Rendering input.add_mapping loads the `###
// input.add_mapping` H3 overlay section from docs/wiki-src/input.md via
// WikiOverlay::LoadMethodSection.
// Counterfactual: if the example key in docs/wiki-src/input.md reverts to the
// non-existent `imcPath`, the rendered page's example contradicts its own
// Parameters list and a copy-pasted call fails with
// [MISSING_REQUIRED_PARAM] Missing required parameter 'contextPath'; the
// imcPath-absence assertion below fails. This pins E-add-mapping-example-wrong-param.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWikiHandlerAddMappingExampleParamTest,
    "PinWright.infra.wiki_handler.MethodPage.AddMappingExampleParam",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWikiHandlerAddMappingExampleParamTest::RunTest(const FString& Parameters)
{
    FString Text;
    if (!WikiHandler::RenderPage(TEXT("input.add_mapping"), Text))
    {
        AddError(TEXT("Wiki renderer unavailable (no editor subsystem/dispatcher)"));
        return false;
    }

    TestFalse(TEXT("add_mapping method page is not a Not-Found page"),
        Text.Contains(TEXT("# Not found:")));
    // The example must use the real required param key the handler reads.
    TestTrue(TEXT("add_mapping page example uses the contextPath key"),
        Text.Contains(TEXT("contextPath")));
    // The non-existent imcPath key must not appear anywhere on the page — a
    // copy-pasted example with imcPath errors MISSING_REQUIRED_PARAM 'contextPath'.
    TestFalse(TEXT("add_mapping page does not mention the bogus imcPath key"),
        Text.Contains(TEXT("imcPath")));
    return true;
}

// ============================================================================
// input.create_input_action method page documents that the verb always creates a
// Boolean IA (valueType is not a create param) and that the Axis1D/Axis2D/Axis3D
// round-trip is `property.set` on the IA's reflected `ValueType` enum — NOT "the
// MCP cannot set value type" (the original ticket's false claim). Rendering
// input.create_input_action loads the `### input.create_input_action` H3 overlay
// section from docs/wiki-src/input.md via WikiOverlay::LoadMethodSection.
// Counterfactual: if that H3 section is reverted/removed, LoadMethodSection
// returns empty, the rendered page carries only the auto summary ("Create a new
// Enhanced Input UInputAction asset...") plus the name/path param list, and the
// overlay-exclusive markers asserted below disappear. The markers are chosen to
// live ONLY in the H3 overlay, not in the auto summary or any param description:
// `property.set`, `ValueType`, and `Axis2D` appear nowhere in InputHandler.cpp's
// create_input_action registration. The negative assertion guards against a
// regression to the reporter's false "this MCP cannot complete that round-trip"
// wording (which would mislead callers away from the working property.set route).
// This pins E-create-input-action-valuetype-undiscoverable.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWikiHandlerCreateInputActionDocumentsValueTypeTest,
    "PinWright.infra.wiki_handler.MethodPage.CreateInputActionDocumentsValueType",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWikiHandlerCreateInputActionDocumentsValueTypeTest::RunTest(const FString& Parameters)
{
    FString Text;
    if (!WikiHandler::RenderPage(TEXT("input.create_input_action"), Text))
    {
        AddError(TEXT("Wiki renderer unavailable (no editor subsystem/dispatcher)"));
        return false;
    }

    TestFalse(TEXT("input.create_input_action method page is not a Not-Found page"),
        Text.Contains(TEXT("# Not found:")));
    // The Boolean-default fact: an IA is always created Boolean. Overlay-exclusive.
    TestTrue(TEXT("create_input_action page states the IA is created Boolean"),
        Text.Contains(TEXT("Boolean")));
    // The working Axis-authoring round-trip the page must point at — overlay-exclusive
    // (property.set / ValueType / Axis2D appear in no create_input_action auto summary).
    TestTrue(TEXT("create_input_action page routes value-type authoring through property.set"),
        Text.Contains(TEXT("property.set")));
    TestTrue(TEXT("create_input_action page names the ValueType property to set"),
        Text.Contains(TEXT("ValueType")));
    TestTrue(TEXT("create_input_action page shows the Axis2D enum value"),
        Text.Contains(TEXT("Axis2D")));
    // The reworded ticket dropped the false claim; the page must NOT tell callers
    // the MCP cannot complete the value-type round-trip (it can, via property.set).
    TestFalse(TEXT("create_input_action page does not claim the MCP cannot set value type"),
        Text.Contains(TEXT("cannot complete that round-trip")));
    return true;
}

// ============================================================================
// editor.screenshot method page documents paired width/height exact-size support
// and discloses the sceneOnlyFallback capture mode. Rendering editor.screenshot
// loads the `### editor.screenshot` H3 overlay section from docs/wiki-src/editor.md
// via WikiOverlay::LoadMethodSection.
// Counterfactual: if that H3 section in docs/wiki-src/editor.md is reverted/removed,
// LoadMethodSection returns empty and the rendered page carries only the auto summary.
// The overlay-exclusive paired-input sentence and sceneOnlyFallback marker asserted
// below then disappear. This pins E-fixed-size-capture-discovery.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWikiHandlerScreenshotRedirectsFixedSizeTest,
    "PinWright.infra.wiki_handler.MethodPage.ScreenshotRedirectsFixedSize",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWikiHandlerScreenshotRedirectsFixedSizeTest::RunTest(const FString& Parameters)
{
    FString Text;
    if (!WikiHandler::RenderPage(TEXT("editor.screenshot"), Text))
    {
        AddError(TEXT("Wiki renderer unavailable (no editor subsystem/dispatcher)"));
        return false;
    }

    TestFalse(TEXT("editor.screenshot method page is not a Not-Found page"),
        Text.Contains(TEXT("# Not found:")));
    // The method itself supports paired width/height exact-size captures. Assert on
    // the overlay-only sentence rather than the auto summary's looser "exact-size"
    // wording, so reverting the overlay cannot leave a false pass.
    TestTrue(TEXT("editor.screenshot page documents paired width/height exact-size support"),
        Text.Contains(TEXT("supply both to render an exact size")));
    // The response's native-size scene-only fallback must be disclosed by its
    // captureMode vocabulary; this marker exists only in the overlay note.
    TestTrue(TEXT("editor.screenshot page discloses the sceneOnlyFallback capture mode"),
        Text.Contains(TEXT("sceneOnlyFallback")));
    return true;
}

// ============================================================================
// ai.get_ai_info method page states the controller readback is thin (only
// controllerClass) and routes perception/BB/BT verification to the real read-back
// verbs. RenderMethodPage emits exactly one overlay block for a method page: the
// `### ai.get_ai_info` H3 of docs/wiki-src/ai.md, loaded via
// WikiOverlay::LoadMethodSection under `## Notes`. (Namespace-page `##` sections are
// NOT pulled onto a method page.) All three markers asserted below therefore live in
// that single H3 body — the consolidated read-back route table.
// Counterfactual: if that H3 section in docs/wiki-src/ai.md is reverted/removed,
// LoadMethodSection returns empty, the rendered page carries only the auto summary
// ("Get information about AI assets (controllers, behavior trees, blackboards, EQS)")
// plus the param list, and the overlay-exclusive markers asserted below disappear.
// The markers are chosen to live ONLY in the H3 overlay, not in the auto summary or
// any param description: `SensesConfig`, `blueprint.scs.get`, and the
// `OBJECT_NOT_FOUND` precision note are nowhere in AIHandler.cpp's get_ai_info
// registration. This pins E-get-ai-info-no-perception-readback.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWikiHandlerGetAiInfoDocumentsThinReadbackTest,
    "PinWright.infra.wiki_handler.MethodPage.GetAiInfoDocumentsThinReadback",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWikiHandlerGetAiInfoDocumentsThinReadbackTest::RunTest(const FString& Parameters)
{
    FString Text;
    if (!WikiHandler::RenderPage(TEXT("ai.get_ai_info"), Text))
    {
        AddError(TEXT("Wiki renderer unavailable (no editor subsystem/dispatcher)"));
        return false;
    }

    TestFalse(TEXT("ai.get_ai_info method page is not a Not-Found page"),
        Text.Contains(TEXT("# Not found:")));
    // The page must steer agents to the perception read-back route: the perception
    // component's SensesConfig is read via blueprint.scs.get, not get_ai_info. Both
    // markers are overlay-exclusive — neither appears in the auto summary/param list.
    TestTrue(TEXT("get_ai_info page names the SensesConfig the readback omits"),
        Text.Contains(TEXT("SensesConfig")));
    TestTrue(TEXT("get_ai_info page routes perception readback to blueprint.scs.get"),
        Text.Contains(TEXT("blueprint.scs.get")));
    // The OBJECT_NOT_FOUND precision (property.get on the CDO :AIPerception subobject
    // path fails) lives only in the overlay; it is the call the reporter wasted.
    TestTrue(TEXT("get_ai_info page warns the CDO subobject route returns OBJECT_NOT_FOUND"),
        Text.Contains(TEXT("OBJECT_NOT_FOUND")));
    return true;
}

// ============================================================================
// property.list and property.set method pages point the property.* reflection
// route at the component-name discovery step: a property on a component is not
// reachable from the bare actor path, so the caller must read the real
// `…Component0` subobject name from actor.get_components first (or use the typed
// actor.get_component_property / actor.set_component_properties verbs). These
// notes live ONLY in the `### property.list` / `### property.set` H3 overlay
// sections of docs/wiki-src/property.md, loaded via WikiOverlay::LoadMethodSection.
// Counterfactual: if those H3 overlay notes are reverted/removed, LoadMethodSection
// drops them and the rendered method pages carry only the auto summaries
// ("List properties on a UObject..." / "Set a UPROPERTY value on a UObject...")
// plus param lists — neither auto summary names `actor.get_components` or the
// `Component0` default-subobject convention, so the assertions below fail. This
// pins E-property-route-no-component-path-discovery.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWikiHandlerPropertyComponentDiscoveryTest,
    "PinWright.infra.wiki_handler.MethodPage.PropertyComponentDiscovery",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWikiHandlerPropertyComponentDiscoveryTest::RunTest(const FString& Parameters)
{
    FString ListText;
    if (!WikiHandler::RenderPage(TEXT("property.list"), ListText))
    {
        AddError(TEXT("Wiki renderer unavailable (no editor subsystem/dispatcher)"));
        return false;
    }

    TestFalse(TEXT("property.list method page is not a Not-Found page"),
        ListText.Contains(TEXT("# Not found:")));
    // The property.* route must be steered to actor.get_components for the
    // component subobject name — overlay-exclusive (not in the auto summary).
    TestTrue(TEXT("property.list page routes component-property discovery to actor.get_components"),
        ListText.Contains(TEXT("actor.get_components")));
    // The UE default-subobject naming convention (…Component0, not …Component) is
    // the exact fact the reporter had to guess; it lives only in the overlay.
    TestTrue(TEXT("property.list page documents the Component0 default-subobject naming"),
        ListText.Contains(TEXT("Component0")));

    FString SetText;
    if (!WikiHandler::RenderPage(TEXT("property.set"), SetText))
    {
        AddError(TEXT("Wiki renderer unavailable (no editor subsystem/dispatcher)"));
        return false;
    }

    TestFalse(TEXT("property.set method page is not a Not-Found page"),
        SetText.Contains(TEXT("# Not found:")));
    TestTrue(TEXT("property.set page routes component-property discovery to actor.get_components"),
        SetText.Contains(TEXT("actor.get_components")));
    TestTrue(TEXT("property.set page documents the Component0 default-subobject naming"),
        SetText.Contains(TEXT("Component0")));
    // The typed verb that sidesteps the level-style path is named so the caller
    // can avoid the World-fallback trap entirely; overlay-exclusive.
    TestTrue(TEXT("property.set page names the typed actor.set_component_properties verb"),
        SetText.Contains(TEXT("actor.set_component_properties")));
    return true;
}

// ============================================================================
// audio.spawn_sound_at_location / audio.create_audio_component spawn a live
// UAudioComponent parented to the level AWorldSettings, which the actor.* read
// verbs cannot resolve (ACTOR_NOT_FOUND). The readback steer — "the owner is
// the unresolvable WorldSettings, so read the component back via the
// componentPath the response already returns, fed to system.inspect.inspect_object
// / property.get, not through actor.get_components" — lives ONLY in the
// `### audio.spawn_sound_at_location` / `### audio.create_audio_component` H3
// overlay sections of docs/wiki-src/audio.md, loaded via
// WikiOverlay::LoadMethodSection. Counterfactual: if those H3 sections are
// reverted/removed, LoadMethodSection drops them and the rendered method pages
// carry only the bare auto summaries ("Spawn a sound component at a world
// location" / "Create a UAudioComponent in the world...") plus param lists —
// neither names WorldSettings, componentPath, or the inspect_object readback,
// so the assertions below fail. This pins
// E-spawned-audio-component-not-actor-readable.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWikiHandlerAudioWorldSettingsReadbackTest,
    "PinWright.infra.wiki_handler.MethodPage.AudioWorldSettingsReadback",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWikiHandlerAudioWorldSettingsReadbackTest::RunTest(const FString& Parameters)
{
    // Both verbs spawn the UAudioComponent on the unresolvable WorldSettings and
    // share the same readback contract, so both method pages must carry the same
    // markers. Assert the identical set on each.
    for (const TCHAR* Method : {TEXT("audio.spawn_sound_at_location"), TEXT("audio.create_audio_component")})
    {
        FString Text;
        if (!WikiHandler::RenderPage(Method, Text))
        {
            AddError(TEXT("Wiki renderer unavailable (no editor subsystem/dispatcher)"));
            return false;
        }

        TestFalse(*FString::Printf(TEXT("%s method page is not a Not-Found page"), Method),
            Text.Contains(TEXT("# Not found:")));
        // The WorldSettings owner that the actor.* read verbs can't resolve — the
        // exact reason the actor route fails; overlay-exclusive (not in the auto summary).
        TestTrue(*FString::Printf(TEXT("%s page names the WorldSettings owner the actor route can't resolve"), Method),
            Text.Contains(TEXT("WorldSettings")));
        // The readback steer: use the returned componentPath, not the actor route.
        TestTrue(*FString::Printf(TEXT("%s page steers readback at the returned componentPath"), Method),
            Text.Contains(TEXT("componentPath")));
        TestTrue(*FString::Printf(TEXT("%s page points the readback at system.inspect.inspect_object"), Method),
            Text.Contains(TEXT("system.inspect.inspect_object")));
    }

    return true;
}

// ============================================================================
// audio.authoring.create_metasound method page's "Typical sequence" example uses
// the live camelCase param spellings every MetaSound handler actually reads
// (create takes name/path; the rest take assetPath/nodeClassName/sourceNodeId/...)
// and a real dotted registry className (UE.Sine.Audio) sourced from
// search_metasound_nodes — not the stale snake_case keys (asset_path/node_class/
// from_node) or the bare display name (WaveTableOscillator) the handler rejects.
// Rendering audio.authoring.create_metasound loads the `### audio.authoring.create_metasound`
// H3 overlay section from docs/wiki-src/audio.authoring.md via WikiOverlay::LoadMethodSection.
// Counterfactual: if that H3 example reverts to the snake_case node_class /
// WaveTableOscillator form, a copy-pasted create call fails MISSING_REQUIRED_PARAM
// 'name' and add_metasound_node fails NODE_CLASS_NOT_FOUND; the node_class-absence
// and nodeClassName-presence assertions below fail. This pins
// E-metasound-node-add-docs-misleading.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWikiHandlerCreateMetasoundExampleParamsTest,
    "PinWright.infra.wiki_handler.MethodPage.CreateMetasoundExampleParams",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWikiHandlerCreateMetasoundExampleParamsTest::RunTest(const FString& Parameters)
{
    FString Text;
    if (!WikiHandler::RenderPage(TEXT("audio.authoring.create_metasound"), Text))
    {
        AddError(TEXT("Wiki renderer unavailable (no editor subsystem/dispatcher)"));
        return false;
    }

    TestFalse(TEXT("create_metasound method page is not a Not-Found page"),
        Text.Contains(TEXT("# Not found:")));

    // The example must use the live param keys the handlers actually read, and not the
    // stale snake_case keys no handler reads (a copy-pasted call with `node_class:`
    // fails MISSING_NODE_TYPE and `asset_path:` is ignored). The `from_node:` connection
    // key is likewise stale.
    static const TCHAR* const LiveKeys[] = { TEXT("nodeClassName"), TEXT("assetPath") };
    static const TCHAR* const StaleKeys[] = {
        TEXT("node_class:"), TEXT("asset_path:"), TEXT("from_node:") };
    WikiDocTestHelpers::AssertExampleUsesParamKeys(*this, TEXT("create_metasound"), Text, LiveKeys, StaleKeys);

    // A real dotted registry className, sourced from search_metasound_nodes.
    TestTrue(TEXT("create_metasound page shows a real UE.<Name>.<Output> className"),
        Text.Contains(TEXT("UE.Sine.Audio")));
    TestTrue(TEXT("create_metasound page steers callers to search_metasound_nodes for classNames"),
        Text.Contains(TEXT("search_metasound_nodes")));
    return true;
}

// ============================================================================
// blueprint.scs.add_component method page warns that for a Blueprint with no
// native/inherited root, UE's scene-root validation promotes the FIRST scene
// component to the actor RootComponent and silently re-parents later root-level
// scene components under it — so "attach to root" is not "make an independent
// root." Rendering blueprint.scs.add_component loads the `### blueprint.scs.add_component`
// H3 overlay section from docs/wiki-src/blueprint.scs.md via WikiOverlay::LoadMethodSection.
// Counterfactual: if that H3 section is reverted/removed, LoadMethodSection
// returns empty, the rendered page carries only the auto summary ("Add a component
// template to a Blueprint's class-level SCS tree...") plus the param list (whose
// parentComponentName desc is just "omit (or empty) to attach as a child of the
// root." — no promotion warning), and the overlay-exclusive markers below
// disappear. The markers are chosen to live ONLY in the H3 overlay, not in the
// auto summary or any param description: `ValidateSceneRootNodes` and
// `RootComponent` appear nowhere in SCSHandler.cpp's add_component registration.
// This pins E-scs-add-component-root-promotion-undocumented.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWikiHandlerScsAddComponentDocumentsRootPromotionTest,
    "PinWright.infra.wiki_handler.MethodPage.ScsAddComponentDocumentsRootPromotion",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWikiHandlerScsAddComponentDocumentsRootPromotionTest::RunTest(const FString& Parameters)
{
    FString Text;
    if (!WikiHandler::RenderPage(TEXT("blueprint.scs.add_component"), Text))
    {
        AddError(TEXT("Wiki renderer unavailable (no editor subsystem/dispatcher)"));
        return false;
    }

    TestFalse(TEXT("blueprint.scs.add_component method page is not a Not-Found page"),
        Text.Contains(TEXT("# Not found:")));
    // The exact UE mechanism the reporter had to read engine source to confirm —
    // overlay-exclusive (not in the auto summary or the parentComponentName desc).
    TestTrue(TEXT("add_component page names UE's ValidateSceneRootNodes root-validation"),
        Text.Contains(TEXT("ValidateSceneRootNodes")));
    // The first-scene-component-becomes-RootComponent rule is the core fact.
    TestTrue(TEXT("add_component page states the first scene component becomes the RootComponent"),
        Text.Contains(TEXT("RootComponent")));
    return true;
}

// ============================================================================
// blueprint.scs.reparent_component method page warns that emptying newParentName
// on a scene component in a no-native-root Blueprint is typically a no-op: UE's
// scene-root validation re-promotes the existing first scene component to
// RootComponent at compile, and the handler also short-circuits the already-under-
// requested-parent case with "Component already under requested parent; no changes
// made." Rendering blueprint.scs.reparent_component loads the
// `### blueprint.scs.reparent_component` H3 overlay section from
// docs/wiki-src/blueprint.scs.md via WikiOverlay::LoadMethodSection.
// Counterfactual: if that H3 section is reverted/removed, LoadMethodSection returns
// empty, the rendered page carries only the auto summary ("Reparent a component in
// a Blueprint's SCS hierarchy") plus the param list (whose newParentName desc is
// just "New parent component name (empty for root)" — no no-op warning), and the
// overlay-exclusive markers below disappear. The markers live ONLY in the H3
// overlay: `ValidateSceneRootNodes` and the verbatim handler no-op message string
// appear nowhere in SCSHandler.cpp's reparent_component registration. This pins
// E-scs-add-component-root-promotion-undocumented.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWikiHandlerScsReparentDocumentsNoOpTest,
    "PinWright.infra.wiki_handler.MethodPage.ScsReparentDocumentsNoOp",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWikiHandlerScsReparentDocumentsNoOpTest::RunTest(const FString& Parameters)
{
    FString Text;
    if (!WikiHandler::RenderPage(TEXT("blueprint.scs.reparent_component"), Text))
    {
        AddError(TEXT("Wiki renderer unavailable (no editor subsystem/dispatcher)"));
        return false;
    }

    TestFalse(TEXT("blueprint.scs.reparent_component method page is not a Not-Found page"),
        Text.Contains(TEXT("# Not found:")));
    // The re-promotion mechanism that makes reparent-to-root a no-op — overlay-exclusive.
    TestTrue(TEXT("reparent_component page names UE's ValidateSceneRootNodes re-promotion"),
        Text.Contains(TEXT("ValidateSceneRootNodes")));
    // The handler's verbatim no-op message, quoted only in the overlay note.
    TestTrue(TEXT("reparent_component page quotes the no-changes-made no-op result"),
        Text.Contains(TEXT("no changes made")));
    return true;
}

// ============================================================================
// animation.authoring.set_transition_rules method page states the over-promise
// correction: the RPC sets only the four coarse transition fields and does NOT
// author the transition's rule expression (the bCanEnterTransition body that
// compares a variable like Speed). The handler
// (AnimationAuthoringHandler_AnimBlueprint.cpp set_transition_rules) writes only
// CrossfadeDuration / PriorityOrder / bAutomaticRuleBasedOnSequencePlayerInState
// / Bidirectional and never touches bCanEnterTransition, so the wiki's former
// "give each transition a boolean-evaluated condition" wording mis-sold a
// capability the API lacks. Rendering animation.authoring.set_transition_rules
// loads the `### animation.authoring.set_transition_rules` H3 overlay section
// from docs/wiki-src/animation.authoring.md via WikiOverlay::LoadMethodSection.
// Counterfactual: if that H3 section is reverted/removed (back to the bare auto
// summary "Update transition rules between states in a state machine"), the
// overlay-exclusive markers asserted below disappear and the assertions fail.
// The markers are chosen to live ONLY in the H3 overlay, not in the auto summary
// or any param description: `bCanEnterTransition`, `blueprint.graph` (the raw
// escape hatch), and the "proposed and deferred" note about the singular
// set_transition_rule helper (board-ticket IDs were stripped from wiki-src, which
// ships to Fab users) appear nowhere in the registration. This pins
// E-set-transition-rules-wiki-overstates-rule-authoring.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWikiHandlerSetTransitionRulesDocumentsNoRuleBodyTest,
    "PinWright.infra.wiki_handler.MethodPage.SetTransitionRulesDocumentsNoRuleBody",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWikiHandlerSetTransitionRulesDocumentsNoRuleBodyTest::RunTest(const FString& Parameters)
{
    FString Text;
    if (!WikiHandler::RenderPage(TEXT("animation.authoring.set_transition_rules"), Text))
    {
        AddError(TEXT("Wiki renderer unavailable (no editor subsystem/dispatcher)"));
        return false;
    }

    TestFalse(TEXT("set_transition_rules method page is not a Not-Found page"),
        Text.Contains(TEXT("# Not found:")));
    // The exact field the RPC does NOT author — the rule body that gates the
    // transition. Overlay-exclusive (not in the auto summary or any param desc).
    TestTrue(TEXT("set_transition_rules page names the bCanEnterTransition rule body it does not author"),
        Text.Contains(TEXT("bCanEnterTransition")));
    // The raw-graph escape hatch for authoring the rule expression today.
    TestTrue(TEXT("set_transition_rules page points at the raw blueprint.graph escape hatch"),
        Text.Contains(TEXT("blueprint.graph")));
    // The deferred-helper note so a reader knows the set_transition_rule (singular)
    // RPC is not yet available. Overlay-exclusive marker (ticket IDs are stripped).
    TestTrue(TEXT("set_transition_rules page notes the singular helper was proposed and deferred"),
        Text.Contains(TEXT("proposed and deferred")));
    // The former over-promise wording must be gone — the page must not claim the
    // RPC gives each transition a boolean-evaluated condition.
    TestFalse(TEXT("set_transition_rules page no longer claims it gives a boolean-evaluated condition"),
        Text.Contains(TEXT("boolean-evaluated condition")));
    return true;
}

// ============================================================================
// material.authoring namespace page documents the input pin type/dimension that
// is otherwise undiscoverable before compile: the "Common target pins" table now
// lists the Noise and Fresnel convenience-node pins, and a Limitations bullet
// states that Noise's Position is a float3, that connect_nodes does not
// type-check, and that a float2 must be promoted with AppendVector before wiring.
// All of these live in the prelude (above the first `### ` H3) of
// docs/wiki-src/material.authoring.md, so they render onto the namespace page via
// WikiOverlay::LoadGroupPrelude.
// Counterfactual: if the table rows and the Limitations bullet are
// reverted/removed, the prelude no longer carries these markers and the
// assertions below fail. The markers are chosen to live ONLY in the overlay, not
// in any auto-generated content: `FilterWidth` and `BaseReflectFractionIn` are
// pin names that appear nowhere in the Material handler registrations, and the
// float3-promotion prose is overlay-exclusive. This pins
// E-material-pin-input-type-undiscoverable.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWikiHandlerMaterialAuthoringDocumentsPinTypesTest,
    "PinWright.infra.wiki_handler.Namespace.MaterialAuthoringDocumentsPinTypes",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWikiHandlerMaterialAuthoringDocumentsPinTypesTest::RunTest(const FString& Parameters)
{
    FString Text;
    if (!WikiHandler::RenderPage(TEXT("material.authoring"), Text))
    {
        AddError(TEXT("Wiki renderer unavailable (no editor subsystem/dispatcher)"));
        return false;
    }

    TestFalse(TEXT("material.authoring namespace page is not a Not-Found page"),
        Text.Contains(TEXT("# Not found:")));
    // The Noise convenience-node pins must be discoverable from the pin table.
    // FilterWidth is overlay-exclusive — no Material handler registration names it.
    TestTrue(TEXT("material.authoring page lists the Noise FilterWidth pin"),
        Text.Contains(TEXT("FilterWidth")));
    // The Fresnel convenience-node pins must be discoverable too; the second
    // axis the ticket reported (Fresnel ExponentIn pin name was undiscoverable).
    TestTrue(TEXT("material.authoring page lists the Fresnel ExponentIn pin"),
        Text.Contains(TEXT("ExponentIn")));
    // BaseReflectFractionIn is overlay-exclusive — appears in no handler source.
    TestTrue(TEXT("material.authoring page lists the Fresnel BaseReflectFractionIn pin"),
        Text.Contains(TEXT("BaseReflectFractionIn")));
    // The Limitations bullet must state the float3 requirement on Noise Position
    // and the AppendVector promotion fix — the engine-source detour the docs
    // eliminate. These phrases live only in the new Limitations bullet.
    TestTrue(TEXT("material.authoring page states Noise Position is a float3"),
        Text.Contains(TEXT("float3")));
    TestTrue(TEXT("material.authoring page documents promoting a float2 with AppendVector"),
        Text.Contains(TEXT("AppendVector")));
    // The deferred-type-check framing must be explicit so callers know
    // connect_nodes accepting a mismatch is expected, not a bug.
    TestTrue(TEXT("material.authoring page states connect_nodes does not type-check"),
        Text.Contains(TEXT("does not type-check")));
    return true;
}

// ============================================================================
// material.authoring namespace page surfaces the one-call bulk MGIR alternative
// to the imperative add_*/connect_nodes drip. The capability (material.compile_mgir)
// ships and is documented on the PARENT `material` page, but the `material.authoring`
// sub-page an agent is steered to for normal material work did not cross-link it on
// the rendered namespace page. The fix adds (a) a Workflow note ("prefer one
// material.compile_mgir call for a large graph") and (b) a `material.mgir` link in
// the `## See also` block. Both live in `##` sections of
// docs/wiki-src/material.authoring.md, so they render onto the namespace page via
// FWikiOverlay.
// What this pins: the bulk-path pointer (the `material.compile_mgir` RPC name and
// the `material.mgir.md` cross-link) is present on the rendered namespace page. It
// guards the union, not each edit independently — the Workflow note (L18) alone
// carries both markers, so reverting only the See-also bullet still passes. The
// markers are overlay-exclusive on this namespace render: `material.compile_mgir`
// and `decompile_mgir` are registered under the `material` category, not
// `material.authoring`, so neither appears in this page's auto-generated
// `## Methods` index — the page surfaces them only because the overlay's `##`
// Workflow note / See-also block render via FWikiOverlay. Revert both the Workflow
// note and the See-also link and the markers vanish entirely. This pins
// E-material-mgir-bulk-path-undiscovered.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWikiHandlerMaterialAuthoringSurfacesBulkMgirTest,
    "PinWright.infra.wiki_handler.Namespace.MaterialAuthoringSurfacesBulkMgir",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWikiHandlerMaterialAuthoringSurfacesBulkMgirTest::RunTest(const FString& Parameters)
{
    FString Text;
    if (!WikiDocTestHelpers::RenderOrFail(*this, TEXT("material.authoring"), Text))
    {
        return false;
    }

    // The Workflow note must name the bulk-import RPC as the large-graph alternative.
    // compile_mgir is registered under the `material` category, so it is absent
    // from this page's auto `## Methods` index — it appears only via the overlay.
    TestTrue(TEXT("material.authoring page surfaces material.compile_mgir as the bulk path"),
        Text.Contains(TEXT("material.compile_mgir")));
    // The `## See also` block (namespace-page-visible) must cross-link the MGIR
    // topic page; the markdown link target is overlay-exclusive here.
    TestTrue(TEXT("material.authoring page cross-links the material.mgir topic page"),
        Text.Contains(TEXT("material.mgir.md")));
    return true;
}

// ============================================================================
// recorder namespace page documents the PRODUCER side that the recorder.* RPCs
// (a query-only surface) never expose: how a host project emits journal entries via
// FJournalRecorder + PW_JOURNAL_LOG, and the editor-only packaging constraint
// (the PinWrightRecorder module is "Type": "Editor", so a host runtime
// module must gate the dep with `if (Target.bBuildEditor)` and wrap calls in
// `#if WITH_EDITOR`). These live in the `## Producer side` / `## Editor-only
// packaging constraint` sections of docs/wiki-src/recorder.md, which render onto
// the namespace page via FWikiOverlay.
// Counterfactual: if docs/wiki-src/recorder.md is reverted/removed, the namespace
// page carries only the auto-generated method index for the recorder.* query verbs
// (list_sessions/describe_session/get_series/find_events/list_segments/query/...) —
// whose summaries name none of the markers below — so the assertions fail. The
// markers are overlay-exclusive: a grep of the recorder handler registrations
// finds PW_JOURNAL_LOG / bBuildEditor / WITH_EDITOR / LogVariable in ZERO files,
// and FJournalRecorder only inside RecorderLifecycle.cpp source (never in a
// REGISTER_RPC_HANDLER summary string). This pins F-recorder-producer-integration-guide.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWikiHandlerRecorderDocumentsProducerSideTest,
    "PinWright.infra.wiki_handler.Namespace.RecorderDocumentsProducerSide",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWikiHandlerRecorderDocumentsProducerSideTest::RunTest(const FString& Parameters)
{
    FString Text;
    if (!WikiHandler::RenderPage(TEXT("recorder"), Text))
    {
        AddError(TEXT("Wiki renderer unavailable (no editor subsystem/dispatcher)"));
        return false;
    }

    TestFalse(TEXT("recorder namespace page is not a Not-Found page"),
        Text.Contains(TEXT("# Not found:")));
    // The producer facade the query-only RPCs never mention — overlay-exclusive.
    TestTrue(TEXT("recorder page names the FJournalRecorder producer facade"),
        Text.Contains(TEXT("FJournalRecorder")));
    // The guard macro that gates the value expression on IsRecording().
    TestTrue(TEXT("recorder page documents the PW_JOURNAL_LOG guard macro"),
        Text.Contains(TEXT("PW_JOURNAL_LOG")));
    // The editor-only packaging recipe: the bBuildEditor-gated dependency and the
    // #if WITH_EDITOR call gating — the exact recipe the reporter derived by hand.
    TestTrue(TEXT("recorder page documents the bBuildEditor-gated dependency"),
        Text.Contains(TEXT("bBuildEditor")));
    TestTrue(TEXT("recorder page documents the WITH_EDITOR call gating"),
        Text.Contains(TEXT("WITH_EDITOR")));
    return true;
}

// ============================================================================
// recorder.integration topic page resolves (auto-enrolled into TopicNodes from
// docs/wiki-src/recorder.integration.md by the wiki discovery walk, because its
// slug does not collide with a registered Category) and carries the full host-side
// producer recipe: the producer API table, the Case-A/Case-B editor-only linking,
// and the OPTIONAL runtime-proxy aside (demoted per the reworded ticket scope, not
// presented as required content).
// Counterfactual: if docs/wiki-src/recorder.integration.md is reverted/removed, the
// slug is no longer enrolled, RenderPage("recorder.integration") classifies as
// NotFound, the body starts with "# Not found:", and the assertions fail. The
// proxy aside is asserted to be explicitly optional so the page does not regress
// into gold-plating the generic UE proxy pattern as a core deliverable.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWikiHandlerRecorderIntegrationTopicResolvesTest,
    "PinWright.infra.wiki_handler.TopicPage.RecorderIntegrationResolves",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWikiHandlerRecorderIntegrationTopicResolvesTest::RunTest(const FString& Parameters)
{
    FString Text;
    if (!WikiHandler::RenderPage(TEXT("recorder.integration"), Text))
    {
        AddError(TEXT("Wiki renderer unavailable (no editor subsystem/dispatcher)"));
        return false;
    }

    TestFalse(TEXT("recorder.integration topic page is not a Not-Found page"),
        Text.Contains(TEXT("# Not found:")));
    // The producer call the host actually writes, and the editor-only dependency gate.
    TestTrue(TEXT("recorder.integration page names the LogVariable producer call"),
        Text.Contains(TEXT("LogVariable")));
    TestTrue(TEXT("recorder.integration page documents the bBuildEditor-gated dependency"),
        Text.Contains(TEXT("bBuildEditor")));
    // The runtime-proxy pattern must be present as an OPTIONAL aside (the reworded
    // scope demotes it from a core deliverable to an optional convenience).
    TestTrue(TEXT("recorder.integration page frames the runtime proxy pattern as optional"),
        Text.Contains(TEXT("Optional")) && Text.Contains(TEXT("proxy")));
    return true;
}

// ============================================================================
// widget.set_animation_loop / widget.set_animation_speed are always-NOT_SUPPORTED
// verbs (loop count and playback speed are runtime-only UUserWidget::PlayAnimation()
// arguments, not serialized UWidgetAnimation properties). Their auto-generated
// per-method pages must NOT over-advertise persistable loop/speed authoring: the
// registration summary itself self-documents the guard (prefixed "Returns
// NOT_SUPPORTED: ...", stating the fixed outcome in the registration summary
// so it renders into the method page), so the
// caveat renders into both the `## Methods` index and the per-method page; and a
// `### widget.set_animation_loop` / `### widget.set_animation_speed` H3 overlay
// section in docs/wiki-src/widget.md adds a per-method Notes block with the
// runtime-PlayAnimation guidance.
// Counterfactual: revert the summary prefix in WidgetAnimationHandler.cpp and the
// "NOT_SUPPORTED" assertion fails (the summary renders verbatim into the method
// page); revert/remove the `### widget.set_animation_*` H3 sections in
// docs/wiki-src/widget.md and the overlay-exclusive "fails loudly" marker fails
// (LoadMethodSection returns empty, so the per-method Notes block disappears). The
// pre-existing loop/speed caveat at widget.md:497 sits under the
// `### widget.export_animations_json` H3 (it was under a `### widget animation JSON`
// orphan heading that reached no page at all until that heading was retargeted), so
// it renders onto widget.export_animations_json and never reaches these per-method
// pages — it cannot mask a revert here. This pins
// E-widget-anim-loop-speed-phantom-authoring-verbs.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWikiHandlerWidgetAnimationLoopSpeedDocumentsRuntimeOnlyTest,
    "PinWright.infra.wiki_handler.MethodPage.WidgetAnimationLoopSpeedDocumentsRuntimeOnly",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWikiHandlerWidgetAnimationLoopSpeedDocumentsRuntimeOnlyTest::RunTest(const FString& Parameters)
{
    // Both verbs share the same always-NOT_SUPPORTED contract, so both method
    // pages must carry the same four guarantees. Assert the identical set on each.
    for (const TCHAR* Method : {TEXT("widget.set_animation_loop"), TEXT("widget.set_animation_speed")})
    {
        FString Text;
        if (!WikiHandler::RenderPage(Method, Text))
        {
            AddError(TEXT("Wiki renderer unavailable (no editor subsystem/dispatcher)"));
            return false;
        }

        TestFalse(*FString::Printf(TEXT("%s method page is not a Not-Found page"), Method),
            Text.Contains(TEXT("# Not found:")));
        // The self-documenting summary must surface the always-error guard on the
        // page the agent reads when calling the method directly. Reverting the
        // "Returns NOT_SUPPORTED: ..." summary prefix drops this token.
        TestTrue(*FString::Printf(TEXT("%s page states the call returns NOT_SUPPORTED"), Method),
            Text.Contains(TEXT("NOT_SUPPORTED")));
        // The runtime PlayAnimation redirect must be on the page itself.
        TestTrue(*FString::Printf(TEXT("%s page points callers at PlayAnimation"), Method),
            Text.Contains(TEXT("PlayAnimation")));
        // Overlay-exclusive marker: "fails loudly" appears only in the
        // `### widget.set_animation_*` H3 overlay block, not in the handler
        // summary, so it fails iff that per-method Notes section is reverted.
        TestTrue(*FString::Printf(TEXT("%s page renders the per-method overlay Notes block"), Method),
            Text.Contains(TEXT("fails loudly")));
    }

    return true;
}

// ============================================================================
// actor.list method page documents that its single narrowing param `filter` is a
// name/label substring match, NOT a class filter, and routes a class-shaped intent
// ("list every PostProcessVolume") to actor.find_by_class — and the actor namespace
// page disambiguates the three actor.* read queries (name vs class vs tag) by
// narrowing intent. The `### actor.list` H3 surfaces on the method page via
// WikiOverlay::LoadMethodSection, and the new `**Narrowing intent**` bullet lives in
// the `## Cross-cluster overlap` section of docs/wiki-src/actor.md, which renders
// onto the namespace page via FWikiOverlay.
// Counterfactual: if the `### actor.list` H3 and the narrowing-intent bullet are
// reverted/removed, the actor.list method page carries only its auto summary
// ("Enumerate every actor in the resolved world ... prefer system.inspect.list_objects")
// plus the `filter`/`world` param list, and the namespace page's read-only-audit
// bullet only maps each query to its system.inspect.* twin. The markers asserted
// below are overlay-exclusive — a grep of the handler sources finds "not a class
// filter", "coincidentally", and "Narrowing intent" in ZERO REGISTER_RPC_HANDLER
// summaries — so the assertions fail iff the overlay edit is reverted. (Bare
// `actor.find_by_class` is NOT overlay-exclusive on the namespace page — it appears
// in the auto `## Methods` index and the find_by_class registration summary — so the
// namespace assertion keys off "Narrowing intent"/"coincidentally", not that token.)
// This pins E-actor-list-no-class-filter.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWikiHandlerActorListDocumentsNameNotClassFilterTest,
    "PinWright.infra.wiki_handler.MethodPage.ActorListDocumentsNameNotClassFilter",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWikiHandlerActorListDocumentsNameNotClassFilterTest::RunTest(const FString& Parameters)
{
    // 1) The actor.list method page must state filter is name-only and route class
    //    intent to actor.find_by_class.
    FString MethodText;
    if (!WikiHandler::RenderPage(TEXT("actor.list"), MethodText))
    {
        AddError(TEXT("Wiki renderer unavailable (no editor subsystem/dispatcher)"));
        return false;
    }

    TestFalse(TEXT("actor.list method page is not a Not-Found page"),
        MethodText.Contains(TEXT("# Not found:")));
    // The core fact — filter is a name/label substring, not a class filter.
    // Overlay-exclusive (no handler summary says "not a class filter").
    TestTrue(TEXT("actor.list page states filter is not a class filter"),
        MethodText.Contains(TEXT("not a class filter")));
    // The footgun warning that the default-label match is only coincidental.
    // Overlay-exclusive.
    TestTrue(TEXT("actor.list page warns the default-label match is only coincidental"),
        MethodText.Contains(TEXT("coincidentally")));
    // The redirect to the real class filter must be on the page the caller reads.
    TestTrue(TEXT("actor.list page routes class intent to actor.find_by_class"),
        MethodText.Contains(TEXT("actor.find_by_class")));

    // 2) The actor namespace page must disambiguate the three actor.* queries by
    //    narrowing intent. "Narrowing intent" lives only in the new bullet — the
    //    bare actor.find_by_class token is not overlay-exclusive here (it is in the
    //    auto method index), so assert on the overlay-exclusive bullet marker.
    FString NamespaceText;
    if (!WikiHandler::RenderPage(TEXT("actor"), NamespaceText))
    {
        AddError(TEXT("Wiki renderer unavailable (no editor subsystem/dispatcher)"));
        return false;
    }

    TestFalse(TEXT("actor namespace page is not a Not-Found page"),
        NamespaceText.Contains(TEXT("# Not found:")));
    TestTrue(TEXT("actor namespace page disambiguates the actor.* queries by narrowing intent"),
        NamespaceText.Contains(TEXT("Narrowing intent")));
    // The footgun is also stated in the namespace cross-cluster bullet; overlay-exclusive.
    TestTrue(TEXT("actor namespace page warns the filter class-match is only coincidental"),
        NamespaceText.Contains(TEXT("coincidentally")));
    return true;
}

// ============================================================================
// actor.find_by_name method page resolves the self-contradicting-help trap: the
// verb keys its sole required param `name`, but the summary and the `name` param
// help both call the *value* a "substring", so a caller reaches for the key
// `substring` and eats `[MISSING_REQUIRED_PARAM] Missing required parameter 'name'`.
// Two edits close the round-trip and both render onto the actor.find_by_name method
// page: (1) the `name` param help is reworded to lead with the key (renders into the
// page's auto-generated Parameters list), and (2) an `### actor.find_by_name` H3 in
// docs/wiki-src/actor.md surfaces via WikiOverlay::LoadMethodSection.
// Counterfactual: if the param help reverts to the bare "Substring to match
// against ..." form, the auto Parameters list drops the "param key is 'name'" steer
// and that assertion fails; if the `### actor.find_by_name` H3 is reverted/removed,
// LoadMethodSection returns empty and the overlay-exclusive "most natural wrong
// guess" marker disappears. Both markers are chosen to live in exactly one edit each
// — a grep of the handler sources finds neither "param key is 'name'" nor "most
// natural wrong guess" in any other registration summary or overlay — so each
// assertion fails iff its half of the fix is reverted. This pins
// E-actor-find-by-name-substring-key-self-contradicts.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWikiHandlerFindByNameDocumentsNameKeyTest,
    "PinWright.infra.wiki_handler.MethodPage.FindByNameDocumentsNameKey",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWikiHandlerFindByNameDocumentsNameKeyTest::RunTest(const FString& Parameters)
{
    FString MethodText;
    if (!WikiDocTestHelpers::RenderOrFail(*this, TEXT("actor.find_by_name"), MethodText))
    {
        return false;
    }

    // 1) The reworded `name` param help leads with the key so the read-the-help path
    //    no longer plants `substring` as the obvious key. Renders into the auto
    //    Parameters list; "param key is 'name'" is in no other param/summary.
    TestTrue(TEXT("actor.find_by_name param help leads with the 'name' key"),
        MethodText.Contains(TEXT("param key is 'name'")));
    // 2) The `### actor.find_by_name` H3 overlay must name the substring->key trap.
    //    "most natural wrong guess" is overlay-exclusive (no handler summary uses it).
    TestTrue(TEXT("actor.find_by_name page documents the substring wrong-guess trap"),
        MethodText.Contains(TEXT("most natural wrong guess")));
    return true;
}

// ============================================================================
// The actor namespace page documents how to resolve an ambiguous `actorName` when
// display labels collide: labels are not unique (placed copies share one), the
// resolver also accepts the unique internal object name, and the collision-safe key
// is the object-name leaf returned in the `path` field of find_by_class/find_by_tag.
// This lives in the `## actorName resolution & colliding labels` section of
// docs/wiki-src/actor.md, which renders onto the namespace page via FWikiOverlay.
// The same collision-safe steer is appended to the shared `actorName` parameter help
// on the per-actor verbs, so it also renders into each method page's auto-generated
// Parameters list (asserted on actor.add_tag below).
// Counterfactual: if the `## actorName resolution & colliding labels` section is
// reverted/removed, the namespace page no longer carries the overlay-exclusive
// markers below ("not guaranteed unique" / "actorName resolution") and the namespace
// assertions fail; if the param-help note is reverted, the actor.add_tag method
// page's Parameters list drops "collision-safe key" and that assertion fails. The
// markers are chosen to live ONLY in the new edits — a grep of the handler
// registrations and the rest of actor.md finds "not guaranteed unique" and
// "collision-safe key" in ZERO other content. This pins
// E-actor-name-resolution-label-collision.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWikiHandlerActorNameLabelCollisionDocumentedTest,
    "PinWright.infra.wiki_handler.MethodPage.ActorNameLabelCollisionDocumented",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWikiHandlerActorNameLabelCollisionDocumentedTest::RunTest(const FString& Parameters)
{
    // 1) The actor namespace page must document the colliding-label resolution model.
    FString NamespaceText;
    if (!WikiHandler::RenderPage(TEXT("actor"), NamespaceText))
    {
        AddError(TEXT("Wiki renderer unavailable (no editor subsystem/dispatcher)"));
        return false;
    }

    TestFalse(TEXT("actor namespace page is not a Not-Found page"),
        NamespaceText.Contains(TEXT("# Not found:")));
    // The section heading is overlay-exclusive (appears in no handler summary).
    TestTrue(TEXT("actor page has the actorName resolution section"),
        NamespaceText.Contains(TEXT("actorName resolution")));
    // The core fact — display labels are NOT unique — is overlay-exclusive.
    TestTrue(TEXT("actor page states display labels are not guaranteed unique"),
        NamespaceText.Contains(TEXT("not guaranteed unique")));
    // The collision-safe disambiguator: the unique internal object name / object-name leaf.
    TestTrue(TEXT("actor page names the unique internal object name as the safe key"),
        NamespaceText.Contains(TEXT("internal object name")));
    TestTrue(TEXT("actor page points at the object-name leaf in the find query path field"),
        NamespaceText.Contains(TEXT("object-name leaf")));
    // The resolver no longer picks a winner among colliding labels, so the page must
    // document the refusal and the payload the caller recovers from - not merely warn that
    // labels collide. Without this, the page could drift back to describing a silent
    // first-match resolution and still pass every assertion above.
    TestTrue(TEXT("actor page names the AMBIGUOUS_ACTOR_NAME refusal"),
        NamespaceText.Contains(TEXT("AMBIGUOUS_ACTOR_NAME")));
    TestTrue(TEXT("actor page tells the caller the error lists candidates"),
        NamespaceText.Contains(TEXT("candidate")));

    // 2) The collision-safe steer must also reach the point of use: the shared
    //    actorName param help renders into the actor.add_tag method page's
    //    auto-generated Parameters list. "collision-safe key" is the overlay-exclusive
    //    marker added to that param description.
    FString MethodText;
    if (!WikiHandler::RenderPage(TEXT("actor.add_tag"), MethodText))
    {
        AddError(TEXT("Wiki renderer unavailable (no editor subsystem/dispatcher)"));
        return false;
    }

    TestFalse(TEXT("actor.add_tag method page is not a Not-Found page"),
        MethodText.Contains(TEXT("# Not found:")));
    TestTrue(TEXT("actor.add_tag actorName param help flags the collision-safe internal object name"),
        MethodText.Contains(TEXT("collision-safe key")));
    return true;
}

// ============================================================================
// The asset namespace page reconciles the three overlapping list/search verbs
// (asset.list / asset.search / asset.search_assets) and states that a bare
// `asset.find` is NOT a verb, so an agent listing assets of a class does not
// reach for asset.search (which hard-errors MISSING_REQUIRED_PARAM 'query') or
// guess the phantom asset.find. The reconciliation lives in the `## Cross-cluster
// overlap` "List / search verbs" bullet of docs/wiki-src/asset.md (renders onto
// the asset namespace page via FWikiOverlay), and the `### asset.search` /
// `### asset.search_assets` H3 sections surface on each method page via
// WikiOverlay::LoadMethodSection.
// Counterfactual: if those overlay edits are reverted/removed, the asset
// namespace page carries only the pre-existing cross-cluster bullets plus the
// auto `## Methods` index, and the asset.search / asset.search_assets method
// pages carry only their bare auto summaries ("Search for assets by name using
// substring or wildcard matching" / "Search for assets using filters") plus the
// param lists. The markers asserted below are overlay-exclusive — a grep of the
// Asset handler registrations finds "is not a verb", "[MISSING_REQUIRED_PARAM]",
// and "no-name path" in ZERO REGISTER_RPC_HANDLER summaries — so the assertions
// fail iff the overlay edits are reverted. (The bare verb tokens asset.list /
// asset.search_assets are NOT overlay-exclusive on the namespace page — they
// appear in the auto `## Methods` index — so the namespace assertion keys off the
// overlay-exclusive "is not a verb" prose, not those tokens.) This pins
// E-asset-search-vs-search-assets-overlap.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWikiHandlerAssetSearchVerbsReconciledTest,
    "PinWright.infra.wiki_handler.MethodPage.AssetSearchVerbsReconciled",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWikiHandlerAssetSearchVerbsReconciledTest::RunTest(const FString& Parameters)
{
    // 1) The asset namespace page must reconcile the three list/search verbs and
    //    state asset.find is not a verb (see header block for the overlay-
    //    exclusivity / counterfactual rationale that makes this assertion load-bearing).
    FString NamespaceText;
    if (!WikiHandler::RenderPage(TEXT("asset"), NamespaceText))
    {
        AddError(TEXT("Wiki renderer unavailable (no editor subsystem/dispatcher)"));
        return false;
    }

    TestFalse(TEXT("asset namespace page is not a Not-Found page"),
        NamespaceText.Contains(TEXT("# Not found:")));
    // The phantom-verb warning the reporter twice tripped over (asset.find).
    TestTrue(TEXT("asset namespace page states asset.find is not a verb"),
        NamespaceText.Contains(TEXT("is not a verb")));
    // The class-only-list route must name the already-documented asset.list path
    // so the caller is not steered only at the two search* siblings.
    TestTrue(TEXT("asset namespace page routes a class-only list at asset.list filter.class"),
        NamespaceText.Contains(TEXT("asset.list { filter: { class:")));

    // 2) The asset.search method page must state query is required (via the
    //    [MISSING_REQUIRED_PARAM] error-code marker) and redirect a class-only list
    //    at asset.search_assets.
    FString SearchText;
    if (!WikiHandler::RenderPage(TEXT("asset.search"), SearchText))
    {
        AddError(TEXT("Wiki renderer unavailable (no editor subsystem/dispatcher)"));
        return false;
    }

    TestFalse(TEXT("asset.search method page is not a Not-Found page"),
        SearchText.Contains(TEXT("# Not found:")));
    TestTrue(TEXT("asset.search page documents the MISSING_REQUIRED_PARAM 'query' error for a name-less call"),
        SearchText.Contains(TEXT("[MISSING_REQUIRED_PARAM]")));
    TestTrue(TEXT("asset.search page redirects a class-only list at asset.search_assets"),
        SearchText.Contains(TEXT("asset.search_assets")));

    // 3) The asset.search_assets method page must frame itself as the no-name
    //    class/path list and give the class-list recipe.
    FString SearchAssetsText;
    if (!WikiHandler::RenderPage(TEXT("asset.search_assets"), SearchAssetsText))
    {
        AddError(TEXT("Wiki renderer unavailable (no editor subsystem/dispatcher)"));
        return false;
    }

    TestFalse(TEXT("asset.search_assets method page is not a Not-Found page"),
        SearchAssetsText.Contains(TEXT("# Not found:")));
    TestTrue(TEXT("asset.search_assets page frames itself as the no-name list path"),
        SearchAssetsText.Contains(TEXT("no-name path")));
    TestTrue(TEXT("asset.search_assets page gives the class-list recipe"),
        SearchAssetsText.Contains(TEXT("classNames:[\"SoundWave\"]")));
    return true;
}

// ============================================================================
// animation.authoring.create_montage method page's "Typical sequence" example uses
// the live camelCase param keys every montage handler actually reads — add_montage_slot
// (assetPath/animationPath/slotName), add_montage_section (assetPath/sectionName/startTime),
// set_section_timing (assetPath/sectionName/startTime), set_blend_in/out
// (assetPath/blendTime) — not the stale snake_case keys (asset_path/slot_name/
// section_name/start_time/blend_time), the documented-but-renamed `time:` (the param
// is `startTime`), or the phantom `end_time` (set_section_timing has NO endTime param
// at all — verified: a grep of AnimationAuthoringHandler_Sequence.cpp for end_time/endTime
// is empty). Rendering animation.authoring.create_montage loads the
// `### animation.authoring.create_montage` H3 overlay section from
// docs/wiki-src/animation.authoring.md via WikiOverlay::LoadMethodSection.
// Counterfactual: if that H3 example reverts to the snake_case asset_path/slot_name/
// start_time/end_time form, a copy-pasted call hard-rejects — set_section_timing
// {start_time, end_time} fails [UNKNOWN_PARAMS] (the dispatcher's strict known-param
// gate rejects keys not in the spec Name+Aliases set, and no montage spec declares a
// snake_case alias), and create_montage with snake_case + no `name` fails
// [MISSING_REQUIRED_PARAM] 'name'. The snake_case-key-absence assertions below fail
// when the example is reverted. This pins E-montage-wiki-example-snake-case-params.
// (The live-vs-stale `<key>:` / case-sensitive matching convention lives once in
// WikiDocTestHelpers::AssertExampleUsesParamKeys.)
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWikiHandlerCreateMontageExampleParamsTest,
    "PinWright.infra.wiki_handler.MethodPage.CreateMontageExampleParams",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWikiHandlerCreateMontageExampleParamsTest::RunTest(const FString& Parameters)
{
    FString Text;
    if (!WikiDocTestHelpers::RenderOrFail(*this, TEXT("animation.authoring.create_montage"), Text))
    {
        return false;
    }

    // The example must use the live camelCase param keys the handlers actually read,
    // and not the stale snake_case keys no montage handler reads (a copy-pasted call
    // with them hard-rejects [UNKNOWN_PARAMS]).
    static const TCHAR* const LiveKeys[] = {
        TEXT("assetPath"), TEXT("animationPath"), TEXT("sectionName"),
        TEXT("startTime"), TEXT("blendTime") };
    static const TCHAR* const StaleKeys[] = {
        TEXT("asset_path:"), TEXT("slot_name:"), TEXT("section_name:"),
        TEXT("start_time:"), TEXT("blend_time:"),
        // The phantom end_time param does not exist on set_section_timing — the
        // example must not document it (copy-paste fails [UNKNOWN_PARAMS]).
        TEXT("end_time:") };
    WikiDocTestHelpers::AssertExampleUsesParamKeys(*this, TEXT("create_montage"), Text, LiveKeys, StaleKeys);

    // The phantom camelCase `endTime:` must not be documented either, but `endTime:`
    // is a substring of the legitimate `blendTime:` key on the set_blend_in/out lines.
    // Assert it appears only as the tail of `blendTime:`, never on its own.
    WikiDocTestHelpers::AssertForbiddenKeyOnlyAsSuffix(*this, TEXT("create_montage"), Text,
        TEXT("endTime:"), TEXT("blendTime:"));
    return true;
}

// ============================================================================
// All four ui.activatable_* method pages document that `host` is keyed on the
// LIVE runtime instance name — the auto-suffixed `WBP_…_C_<n>` `widgetName` that
// ui.create_hud returns — NOT the widget blueprint asset/class name the caller
// authored. FindStackInPie matches `host` against `It->GetName()` over live
// UUserWidget instances (UiActivatableStackHandler.cpp:26), so the asset name
// yields HOST_NOT_FOUND. Each method page's overlay H3
// (### ui.activatable_push / _pop / list_stack_widgets / get_active_widget in
// docs/wiki-src/ui.md) carries this note, loaded via WikiOverlay::LoadMethodSection.
// Counterfactual: if those H3 sections (and the host-naming note added to the
// pre-existing ### ui.activatable_push section) are reverted/removed,
// LoadMethodSection returns empty (pop/list/get_active had no section at all),
// the rendered pages carry only the auto summaries ("Pop the active (or named)
// activatable widget off a named stack." etc.) plus the param list — whose `host`
// desc is only "Name of the host UUserWidget instance that owns the stack", naming
// neither ui.create_hud, widgetName, nor the _C_<n> form. The markers asserted
// below are overlay-exclusive: a grep of UiActivatableStackHandler.cpp's
// registrations finds neither `ui.create_hud`, `widgetName`, nor `HOST_NOT_FOUND`
// in any summary or param description (HOST_NOT_FOUND appears only as a runtime
// error-string literal, never in a registration). This pins
// E-activatable-host-runtime-instance-name-undocumented.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWikiHandlerActivatableHostInstanceNameDocumentedTest,
    "PinWright.infra.wiki_handler.MethodPage.ActivatableHostInstanceNameDocumented",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWikiHandlerActivatableHostInstanceNameDocumentedTest::RunTest(const FString& Parameters)
{
    // The host-naming note must be present on EVERY one of the four methods that
    // take `host` — the gap applies to all four, not just push.
    for (const TCHAR* Method : {
        TEXT("ui.activatable_push"), TEXT("ui.activatable_pop"),
        TEXT("ui.list_stack_widgets"), TEXT("ui.get_active_widget") })
    {
        FString Text;
        if (!WikiDocTestHelpers::RenderOrFail(*this, Method, Text))
        {
            return false;
        }

        // The linkage the reporter had to discover by trial-and-error: `host` is the
        // `widgetName` ui.create_hud returns. Both tokens are overlay-exclusive — the
        // `host` param desc is only "Name of the host UUserWidget instance that owns
        // the stack" and names neither ui.create_hud nor widgetName.
        TestTrue(*FString::Printf(TEXT("%s page links host to the ui.create_hud return value"), Method),
            Text.Contains(TEXT("ui.create_hud")));
        TestTrue(*FString::Printf(TEXT("%s page names the widgetName host must match"), Method),
            Text.Contains(TEXT("widgetName")));
        // The auto-suffixed live-instance form `WBP_…_C_<n>`, so the caller knows it
        // is NOT the asset/class name. Overlay-exclusive.
        TestTrue(*FString::Printf(TEXT("%s page shows the _C_<n> runtime-instance form"), Method),
            Text.Contains(TEXT("_C_<n>")));
        // The error a wrong (asset-name) host produces, so the page maps the symptom
        // back to the fix. HOST_NOT_FOUND is overlay-exclusive on the rendered page —
        // it is only a runtime error-string literal in the handler, never a summary.
        TestTrue(*FString::Printf(TEXT("%s page names the HOST_NOT_FOUND symptom"), Method),
            Text.Contains(TEXT("HOST_NOT_FOUND")));
    }
    return true;
}

// ============================================================================
// Root index carries maturity-tier markers: non-core namespaces are flagged
// inline right after the backticked name (`material` (experimental),
// `pipeline` (internal)), core namespaces stay unmarked, and the intro carries
// the one-line legend explaining the markers. Tiers come from
// docs/wiki-src/maturity.json, loaded into the wiki cache and rendered by
// RenderRootNamespaceEntry / RenderRoot.
// Counterfactual: if the maturity map is no longer loaded into the cache (or
// RenderRootNamespaceEntry drops its Tier parameter), every entry renders bare,
// the (experimental)/(internal) markers and the legend line disappear, and the
// assertions fail. This pins E-wiki-maturity-tiers.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWikiHandlerRootIndexMaturityMarkersTest,
    "PinWright.infra.wiki_handler.Maturity.RootIndexMarkers",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWikiHandlerRootIndexMaturityMarkersTest::RunTest(const FString& Parameters)
{
    FString Text;
    if (!WikiHandler::RenderPage(TEXT(""), Text))
    {
        AddError(TEXT("Wiki renderer unavailable (no editor subsystem/dispatcher)"));
        return false;
    }

    // Non-core tiers are flagged inline right after the backticked name.
    TestTrue(TEXT("root index marks material as experimental"),
        Text.Contains(TEXT("`material` (experimental)")));
    TestTrue(TEXT("root index marks pipeline as internal"),
        Text.Contains(TEXT("`pipeline` (internal)")));
    // Core namespaces stay unmarked — no parenthesized tier after the name.
    TestTrue(TEXT("root index lists the blueprint entry"),
        Text.Contains(TEXT("- `blueprint`")));
    TestFalse(TEXT("root index does not mark the core blueprint namespace"),
        Text.Contains(TEXT("- `blueprint` (")));
    // The one-line legend explaining the markers renders in the intro.
    TestTrue(TEXT("root index carries the maturity legend"),
        Text.Contains(TEXT("unmarked namespaces are core")));
    // The legend must also explain the fail-closed marker, or a reader who meets
    // `(unclassified)` on an entry has no way to tell what it means.
    TestTrue(TEXT("maturity legend explains the unclassified marker"),
        Text.Contains(TEXT("(unclassified)")));
    return true;
}

// ============================================================================
// Namespace pages carry a Stability line for every namespace — mapped ones name
// their tier, unmapped ones say `unclassified` (covered by
// Maturity.UnmappedNamespaceRendersUnclassified). The lookup keys on the FIRST
// dotted segment so nested namespace pages inherit their top-level tier
// (material.authoring inherits material's experimental). Rendered by
// RenderNamespaceHeader via ResolveTier over the cache's maturity map.
// Counterfactual: if RenderNamespaceHeader stops emitting the Stability line
// (or looks up the full dotted path instead of the first segment, which has no
// map entry for nested pages), the lines below disappear and the assertions
// fail. This pins E-wiki-maturity-tiers.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWikiHandlerNamespacePageStabilityLineTest,
    "PinWright.infra.wiki_handler.Maturity.NamespaceStabilityLine",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWikiHandlerNamespacePageStabilityLineTest::RunTest(const FString& Parameters)
{
    // One namespace per tier, plus a nested page inheriting its top-level tier.
    struct FCase { const TCHAR* Path; const TCHAR* Marker; };
    for (const FCase& Case : {
        FCase{ TEXT("blueprint"), TEXT("Stability: core") },
        FCase{ TEXT("material"), TEXT("Stability: experimental") },
        FCase{ TEXT("pipeline"), TEXT("Stability: internal") },
        FCase{ TEXT("material.authoring"), TEXT("Stability: experimental") } })
    {
        FString Text;
        if (!WikiDocTestHelpers::RenderOrFail(*this, Case.Path, Text))
        {
            return false;
        }
        TestTrue(*FString::Printf(TEXT("%s page carries '%s'"), Case.Path, Case.Marker),
            Text.Contains(Case.Marker));
    }

    // The experimental wording must state the meaning, not just the tier name.
    FString MaterialText;
    if (!WikiDocTestHelpers::RenderOrFail(*this, TEXT("material"), MaterialText))
    {
        return false;
    }
    TestTrue(TEXT("experimental Stability line explains the tier"),
        MaterialText.Contains(TEXT("still changing")));
    return true;
}

// ============================================================================
// The maturity map fails CLOSED. A namespace absent from maturity.json renders
// as `unclassified` — the least-trusted tier — on BOTH surfaces: a
// `(unclassified)` marker on its root index entry and a
// `Stability: unclassified` line on its namespace page. Uses a fixture
// dispatcher with a probe namespace no maturity.json key matches.
//
// This test previously asserted the opposite (`UnmappedNamespaceUnmarked`): that
// an unmapped namespace rendered bare. Bare is exactly what a `core` namespace
// renders, and the root-index legend says "unmarked namespaces are core" — so
// the old behaviour made a forgotten or brand-new namespace advertise itself as
// solid primary surface, and this test locked that in. See board ticket
// B-maturity-unmapped-namespace-fails-open.
//
// Counterfactual: revert ResolveTier to returning the raw map lookup (empty when
// absent) and the probe entry loses both the marker and the Stability line, so
// both positive assertions fail. This also pins E-wiki-maturity-tiers.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWikiHandlerUnmappedNamespaceUnclassifiedTest,
    "PinWright.infra.wiki_handler.Maturity.UnmappedNamespaceRendersUnclassified",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWikiHandlerUnmappedNamespaceUnclassifiedTest::RunTest(const FString& Parameters)
{
    FRpcDispatcher Dispatcher;
    Dispatcher.DrainAutoRegistrations(nullptr);

    FHandlerRegistration Reg;
    Reg.MethodName = TEXT("zz_maturity_probe.noop");
    Reg.Category = TEXT("zz_maturity_probe");
    Reg.Summary = TEXT("Maturity-map probe method");
    Dispatcher.AddAutoRegisteredForTesting(Reg);

    FString Root;
    if (!TestTrue(TEXT("root render succeeds"),
            WikiHandler::RenderPage(Dispatcher, TEXT(""), Root)))
    {
        return false;
    }
    TestTrue(TEXT("root index lists the probe namespace"),
        Root.Contains(TEXT("`zz_maturity_probe`")));
    TestTrue(TEXT("unmapped probe namespace is marked unclassified, not left bare"),
        Root.Contains(TEXT("`zz_maturity_probe` (unclassified)")));

    FString Page;
    if (!TestTrue(TEXT("namespace render succeeds"),
            WikiHandler::RenderPage(Dispatcher, TEXT("zz_maturity_probe"), Page)))
    {
        return false;
    }
    TestTrue(TEXT("unmapped namespace page carries an unclassified Stability line"),
        Page.Contains(TEXT("Stability: unclassified")));
    // The line must say what the tier means, not just name it — an agent reading
    // the page has to learn that this is weaker than experimental.
    TestTrue(TEXT("unclassified Stability line explains the tier"),
        Page.Contains(TEXT("less stable than experimental")));
    return true;
}

// ============================================================================
// Authoring-time gate: EVERY namespace the live editor registers is classified
// in docs/wiki-src/maturity.json. Adding a namespace without a maturity entry
// turns the suite red and names the namespace, which is the point — the
// fail-closed render (above) keeps the omission honest at call time, but the
// omission itself is a docs defect that should be caught before it ships.
//
// Asserts against the rendered root index rather than re-reading maturity.json,
// so it exercises the same path an agent sees; the marker only appears when
// ResolveTier fell back.
// Counterfactual: drop a namespace's entry from maturity.json and this fails
// naming that namespace. This pins B-maturity-unmapped-namespace-fails-open.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWikiHandlerEveryNamespaceClassifiedTest,
    "PinWright.infra.wiki_handler.Maturity.EveryRegisteredNamespaceIsClassified",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWikiHandlerEveryNamespaceClassifiedTest::RunTest(const FString& Parameters)
{
    FString Root;
    if (!WikiHandler::RenderPage(TEXT(""), Root))
    {
        AddError(TEXT("Wiki renderer unavailable (no editor subsystem/dispatcher)"));
        return false;
    }

    TArray<FString> Lines;
    Root.ParseIntoArrayLines(Lines, /*InCullEmpty=*/false);

    TArray<FString> Unclassified;
    for (const FString& Line : Lines)
    {
        // Root namespace entries render as: - `<slug>` (<tier>) — <intro>
        if (!Line.StartsWith(TEXT("- `")) || !Line.Contains(TEXT("` (unclassified)")))
        {
            continue;
        }
        const int32 Close = Line.Find(TEXT("` (unclassified)"));
        Unclassified.Add(Line.Mid(3, Close - 3));
    }

    if (Unclassified.Num() > 0)
    {
        AddError(FString::Printf(
            TEXT("%d registered namespace(s) have no docs/wiki-src/maturity.json entry and render as ")
            TEXT("'unclassified': %s. Classify each as core, experimental or internal."),
            Unclassified.Num(), *FString::Join(Unclassified, TEXT(", "))));
        return false;
    }
    return true;
}

// ============================================================================
// Root index carries a "## Task guides" index of the standalone guide pages, so a
// user who does not already know a page exists can find it. The list is derived
// from the same TopicNodes enrolment that makes those pages navigable
// (RenderGuideIndex / GuideSlugs in WikiHandler.cpp), not from a hand-maintained
// table, and `workflows` — itself the index of guides — leads it.
// Counterfactual: before this section existed, a standalone guide was reachable
// via call("<slug>") but appeared on the root index only if some namespace prelude
// happened to link it; the heading, the workflows-first ordering, and the
// per-guide entries below are all absent and every assertion fails. Deleting a
// wiki-src guide page or adding one changes the list without touching this test —
// which is the property being pinned.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWikiHandlerRootIndexListsGuidesTest,
    "PinWright.infra.wiki_handler.RootIndex.ListsGuidePages",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWikiHandlerRootIndexListsGuidesTest::RunTest(const FString& Parameters)
{
    FString Root;
    if (!WikiHandler::RenderPage(TEXT(""), Root))
    {
        AddError(TEXT("Wiki renderer unavailable (no editor subsystem/dispatcher)"));
        return false;
    }

    FString Section;
    if (!TestTrue(TEXT("root index carries the '## Task guides' section"),
            WikiDocTestHelpers::ExtractSection(Root, TEXT("Task guides"), Section)))
    {
        return false;
    }

    TArray<TPair<FString, FString>> Entries;
    WikiDocTestHelpers::ParseSlugBullets(Section, Entries);
    if (!TestTrue(TEXT("guide section lists at least one page"), Entries.Num() > 0))
    {
        return false;
    }

    // workflows is the index of guides, so it leads the list.
    TestEqual(TEXT("workflows leads the guide list"), Entries[0].Key, FString(TEXT("workflows")));

    for (const TPair<FString, FString>& Entry : Entries)
    {
        // Namespace and method pages have their own indexes and must not be
        // duplicated here. Every dotted slug is either a nested namespace, a
        // method, or a namespace-owned topic linked from its parent page.
        TestFalse(*FString::Printf(TEXT("guide entry '%s' is not a namespace/method page"), *Entry.Key),
            Entry.Key.Contains(TEXT(".")));

        // Compact by design: one line per page, not the page's prelude. The root's
        // byte budget is already spent on the namespace entries below.
        TestTrue(*FString::Printf(TEXT("guide entry '%s' stays within the one-line budget"), *Entry.Key),
            Entry.Value.Len() <= 220);

        if (Entry.Value.IsEmpty())
        {
            continue; // page with no prose prelude renders as a bare slug
        }

        // The summary is derived from the page, not written here: its opening must
        // appear verbatim on the page it describes. A 30-char probe stays inside the
        // page's first source line (these overlays write a paragraph per line) while
        // still being specific to that page, and survives the clause/length trims
        // that only ever cut the tail.
        FString GuidePage;
        if (!WikiDocTestHelpers::RenderOrFail(*this, *Entry.Key, GuidePage))
        {
            return false;
        }
        const FString Probe = Entry.Value.Left(FMath::Min(30, Entry.Value.Len()));
        TestTrue(*FString::Printf(TEXT("guide summary for '%s' is derived from the page itself"), *Entry.Key),
            GuidePage.Contains(Probe));
    }

    // The namespace index stays separate — a registered namespace is never enrolled
    // as a topic, so it must not appear as a guide entry.
    for (const TCHAR* Namespace : { TEXT("blueprint"), TEXT("actor"), TEXT("material") })
    {
        TestFalse(*FString::Printf(TEXT("guide section does not list the %s namespace"), Namespace),
            Section.Contains(FString::Printf(TEXT("- `%s`"), Namespace)));
    }
    return true;
}
