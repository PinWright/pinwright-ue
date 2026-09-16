// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Misc/AutomationTest.h"
#include "Compiler/CodeFunctionResolver.h"

#include "GameFramework/Actor.h"

// ============================================================================
// FCodeFunctionResolver — DisplayNameMetadata
// AActor::K2_GetComponentsByClass declares meta=(ScriptName="GetComponentsByClass").
// Authoring against the BP-visible "GetComponentsByClass" must resolve to the
// internal K2_GetComponentsByClass via the DisplayName/ScriptName meta pass.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCodeFunctionResolverDisplayNameMetadataTest,
    "PinWright.bpir.compiler.resolvers.function_resolver.DisplayNameMetadata",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCodeFunctionResolverDisplayNameMetadataTest::RunTest(const FString& Parameters)
{
    FCodeFunctionResolver Resolver;
    UFunction* Found = Resolver.ResolveFunction(AActor::StaticClass(), TEXT("GetComponentsByClass"));
    TestNotNull(TEXT("GetComponentsByClass resolves via ScriptName meta"), Found);
    if (Found)
    {
        TestEqual(TEXT("Resolved to internal K2_GetComponentsByClass"),
            Found->GetName(), TEXT("K2_GetComponentsByClass"));
    }
    return true;
}
