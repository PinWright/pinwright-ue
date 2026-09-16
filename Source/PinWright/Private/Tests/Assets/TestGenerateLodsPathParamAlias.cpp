// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression tests for E-generate-lods-landscapepath-misnomer.
// asset.generate_lods generates LODs on StaticMesh assets, but its single-mesh slot
// was historically named `landscapePath` (a self-admitted misnomer) with no convention-
// conforming alias. A caller reasoning from the codebase-wide assetPath/assetPaths
// convention who sent `assetPath` hit a hard UNKNOWN_PARAMS reject from the dispatcher.
// The fix makes `assetPath` the canonical single-mesh key, adds `meshPath` plus the legacy
// `landscapePath` as aliases (ParamAliasUtils::MakeAliasParamSpec), and reads the value via
// FHandlerContext::GetStringFirstOf so any of the three resolves end-to-end.
//
// Two layers of coverage:
//  - Static registration check: the registered single-mesh spec is canonical `assetPath`,
//    optional, and carries the `meshPath` and `landscapePath` aliases. Exercises the
//    production FHandlerRegistration/FParamSpec set. asset.generate_lods is async, so this
//    static check is the primary guarantee that the alias machinery is wired up.
//  - Body-side resolution: drive the production resolver
//    AssetPathParamUtils::ResolveGenerateLodsSingleMesh against an FHandlerContext carrying
//    each candidate key (assetPath / meshPath / landscapePath) and assert it reads the value
//    back via GetStringFirstOf. This proves the alias reaches the handler body, not just the
//    registry. It deliberately stops short of dispatching asset.generate_lods end-to-end:
//    that handler is async (MakeAsyncToken) with no synchronous response, so a dispatcher
//    round-trip in the sync test harness trips the missing-response guard's once-per-session
//    ensure() for asset.generate_lods — corrupting the shared ensure state that the separate
//    core.dispatcher.AutoRegNoResponseGuard test relies on. The resolver-unit assertion
//    covers the same end-to-end concern (the body resolves the slot) without that side effect.
// Counterfactual: reverting the fix renames the canonical spec back to `landscapePath`, so
//   `assetPath` is no longer a known param (static check fails) and ResolveGenerateLodsSingleMesh
//   no longer exists / no longer reads `assetPath` (the body-side check fails to compile/resolve).
#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/ParamSpec.h"
#include "Handlers/Asset/AssetPathParamUtils.h"
#include "Tests/Infra/ParamSpecTestHelpers.h"

using ParamSpecTestHelpers::FindParamSpec;

// 1. Static: asset.generate_lods registers its single-mesh slot canonically as `assetPath`
//    (optional), with `meshPath` and the legacy `landscapePath` as aliases. The old
//    misnomer-only `landscapePath` spec must no longer exist as a standalone canonical param.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGenerateLodsDeclaresAssetPathAliasTest,
    "PinWright.asset.aliases.GenerateLodsDeclaresAssetPathAlias",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGenerateLodsDeclaresAssetPathAliasTest::RunTest(const FString& Parameters)
{
    const FParamSpec* Spec = FindParamSpec(TEXT("asset.generate_lods"), TEXT("assetPath"));
    if (!TestNotNull(TEXT("asset.generate_lods declares a canonical assetPath param"), Spec))
    {
        return false;
    }

    TestFalse(TEXT("asset.generate_lods assetPath is optional"), Spec->bRequired);
    TestTrue(TEXT("asset.generate_lods assetPath carries the 'meshPath' alias"),
        Spec->Aliases.Contains(TEXT("meshPath")));
    TestTrue(TEXT("asset.generate_lods assetPath carries the legacy 'landscapePath' alias"),
        Spec->Aliases.Contains(TEXT("landscapePath")));

    // The misnomer must be demoted to an alias, not registered as its own canonical param.
    const FParamSpec* LegacySpec = FindParamSpec(TEXT("asset.generate_lods"), TEXT("landscapePath"));
    TestNull(TEXT("asset.generate_lods no longer registers landscapePath as a canonical param"),
        LegacySpec);

    return true;
}

// 2. Body-side resolution: the production resolver the handler uses to read its single-mesh
//    slot must return the value for each of assetPath / meshPath / landscapePath. This is the
//    end-to-end concern (the body honors the alias, not just the registry) proven at the unit
//    level so it never trips the async missing-response guard (see file header).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGenerateLodsResolvesSingleMeshAliasTest,
    "PinWright.asset.aliases.GenerateLodsResolvesSingleMeshAlias",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGenerateLodsResolvesSingleMeshAliasTest::RunTest(const FString& Parameters)
{
    // The same value sent under each candidate wire key must resolve identically: the body
    // reads via GetStringFirstOf(GenerateLodsSingleMeshKeys()), canonical first.
    const FString Expected = TEXT("/Game/Meshes/SM_Probe");

    for (const FString& Key : AssetPathParamUtils::GenerateLodsSingleMeshKeys())
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(Key, Expected);

        FHandlerContext Ctx = FHandlerContext::MakeTestContext(
            TEXT("req-generate-lods-alias"), TEXT("asset.generate_lods"), Payload);

        TestEqual(*FString::Printf(TEXT("ResolveGenerateLodsSingleMesh reads '%s'"), *Key),
            AssetPathParamUtils::ResolveGenerateLodsSingleMesh(Ctx), Expected);
    }

    // Absent slot resolves to empty (the slot is optional; the batch assetPaths slot may be
    // used instead), so the handler — not the resolver — decides whether absence is an error.
    {
        FHandlerContext Ctx = FHandlerContext::MakeTestContext(
            TEXT("req-generate-lods-empty"), TEXT("asset.generate_lods"), MakeShared<FJsonObject>());
        TestEqual(TEXT("ResolveGenerateLodsSingleMesh returns empty when no key is present"),
            AssetPathParamUtils::ResolveGenerateLodsSingleMesh(Ctx), FString());
    }

    return true;
}
