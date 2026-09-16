// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Misc/AutomationTest.h"
#include "Catalog/WikiHandler.h"
#include "Dispatch/RpcDispatcher.h"
#include "Handlers/HandlerRegistration.h"
#include "Handlers/ParamSpec.h"

// The wiki cache is built lazily on the first RenderPage and keyed by the source
// dispatcher's registry generation. A handler registered after the cache is warm
// must become visible on the next render — this guards that invalidation path.

// ============================================================================
// PostInitRegistration: warming the cache, then registering a handler, then
// re-rendering shows the new method under "## Methods".
// Counterfactual: if RegisterHandler / AddAutoRegisteredForTesting does not bump
// RegistryGeneration, or GetWikiCache ignores the generation, the cache stays
// frozen, the "zz" page still renders as Not-Found, and the assertion fails.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWikiHandlerPostInitRegistrationTest,
    "PinWright.infra.wiki_handler.PostInitRegistration",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWikiHandlerPostInitRegistrationTest::RunTest(const FString& Parameters)
{
    FRpcDispatcher Dispatcher;
    Dispatcher.DrainAutoRegistrations(nullptr);

    // Warm the cache for this dispatcher at its current generation. No handler
    // lives under the "zz" namespace yet, so this renders as Not-Found.
    FString Before;
    if (!TestTrue(TEXT("warming render succeeds"),
            WikiHandler::RenderPage(Dispatcher, TEXT("zz"), Before)))
    {
        return false;
    }
    TestTrue(TEXT("zz is unknown before registration"),
        Before.StartsWith(TEXT("# Not found:")));

    // Simulate a post-init registrant landing in the registry the wiki reads.
    FHandlerRegistration Reg;
    Reg.MethodName = TEXT("zz.test_post_init");
    Reg.Category = TEXT("zz");
    Reg.Summary = TEXT("Post-init registered probe method");
    Dispatcher.AddAutoRegisteredForTesting(Reg);

    FString After;
    if (!TestTrue(TEXT("post-registration render succeeds"),
            WikiHandler::RenderPage(Dispatcher, TEXT("zz"), After)))
    {
        return false;
    }

    TestTrue(TEXT("zz now renders as a namespace page with a method list"),
        After.Contains(TEXT("## Methods")));
    TestTrue(TEXT("the post-init method appears on the page"),
        After.Contains(TEXT("zz.test_post_init")));

    // EnumerateAllSlugs shares the same cache; the new method must surface there too.
    TArray<FString> Slugs;
    if (!TestTrue(TEXT("slug enumeration succeeds"),
            WikiHandler::EnumerateAllSlugs(Dispatcher, Slugs)))
    {
        return false;
    }
    TestTrue(TEXT("slug list includes the post-init method"),
        Slugs.Contains(TEXT("zz.test_post_init")));

    return true;
}
