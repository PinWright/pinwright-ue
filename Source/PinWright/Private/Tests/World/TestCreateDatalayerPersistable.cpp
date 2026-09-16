// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression tests for B-create-datalayer-transient-asset-not-persistable.
//
// world_partition.create_datalayer used to construct the UDataLayerAsset with
// GetTransientPackage() as its outer, so the asset resolved under
// /Engine/Transient and could never be serialized: the data layer and every
// set_datalayer assignment that referenced it dangled on level save/reload
// (a silent persistence no-op despite the success return). The fix routes the
// asset through a real /Game-rooted package via ValidateAssetCreationPath +
// CreatePackage with RF_Standalone, mirroring the persistable sibling
// level.structure.create_data_layer.
//
// COVERAGE LIMIT - read before trusting this file as a regression guard.
// Neither test below observes the reverted defect, and an earlier version of this
// header claimed otherwise:
//
//   - BuildsPersistablePackagePath calls ValidateAssetCreationPath directly with a
//     folder string supplied BY THE TEST. That helper is a generic path builder with
//     no data-layer knowledge, and the handler is never invoked. Reverting
//     WorldPartitionHandler.cpp's NewObject outer to GetTransientPackage() leaves
//     this test passing untouched - it pins the helper's behaviour, not the fix.
//   - EchoesNonTransientPath does invoke the handler, but the handler short-circuits
//     with NOT_PARTITIONED (WorldPartitionHandler.cpp:108) before it ever builds a
//     package, and a headless automation world is not partitioned. The echo
//     assertions therefore do not run in the environment the suite executes in.
//
// Observing the real contract requires a World Partition world fixture: only with
// WorldPartition present does the handler reach ValidateAssetCreationPath +
// CreatePackage + NewObject(AssetPackage, ...) and echo a path worth asserting on.
// Until such a fixture exists, EchoesNonTransientPath at least fails on an
// UNEXPECTED error and reports its non-coverage loudly rather than passing silently.

#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Misc/PackageName.h"
#include "Tests/TestSkipReporting.h"
#include "Tests/TestUtils.h"
#include "Utils/PathUtils.h"

// ============================================================================
// The package path the fixed handler builds for a data layer must be a real
// /Game-rooted mount path, never the transient package. This is the exact call
// world_partition.create_datalayer now makes before CreatePackage().
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWorldCreateDatalayerBuildsPersistablePackagePathTest,
    "PinWright.world_partition.create_datalayer.BuildsPersistablePackagePath",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWorldCreateDatalayerBuildsPersistablePackagePathTest::RunTest(const FString& Parameters)
{
    // Mirror the handler's call: folder "/Game/DataLayers", name = the requested layer.
    const FString DataLayerName = TEXT("ReplayProbeLayer");
    FString FullAssetPath;
    FString PathError;
    const bool bBuilt = ValidateAssetCreationPath(TEXT("/Game/DataLayers"), DataLayerName, FullAssetPath, PathError);

    TestTrue(TEXT("ValidateAssetCreationPath succeeds for a data layer name"), bBuilt);
    TestEqual(TEXT("no path error"), PathError, FString());

    // The asset must live in a real content mount, NOT /Engine/Transient.
    TestTrue(TEXT("asset package path is a valid registered mount point"),
        IsValidMountPoint(FullAssetPath));
    TestTrue(TEXT("asset package path is under /Game (a saveable project content root)"),
        FullAssetPath.StartsWith(TEXT("/Game/")));
    TestFalse(TEXT("asset package path is NOT the transient package"),
        FullAssetPath.StartsWith(TEXT("/Engine/Transient")));

    // The asset object name must be the layer name, so a downstream save lands a
    // resolvable /Game/...DataLayers/<name>.<name> asset rather than a transient ref.
    TestEqual(TEXT("asset short name is the data layer name"),
        FPackageName::GetShortName(FullAssetPath), DataLayerName);

    return true;
}

// ============================================================================
// When the handler reaches its success path (a partitioned world is available),
// the echoed dataLayerAssetPath must be a real /Game asset path, never a
// transient one.
//
// A headless automation world is normally not partitioned, so the handler exits
// early and the echo cannot be inspected. That skip is now EXPLICIT and its reason
// is PINNED: only NO_WORLD / NOT_PARTITIONED count as "no partitioned world to test
// against". Any other error - a renamed param, a broken package path, a failed
// NewObject - is a real failure rather than a silent pass, which is what the
// previous unconditional `if (Capture.bSuccess)` guard turned every error into.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWorldCreateDatalayerEchoesNonTransientPathTest,
    "PinWright.world_partition.create_datalayer.EchoesNonTransientPath",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWorldCreateDatalayerEchoesNonTransientPathTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("dataLayerName"), TEXT("PersistenceProbeLayer"));

    FTestResponseCapture Capture;
    const bool bFound = InvokeHandlerWithCapture(TEXT("world_partition.create_datalayer"), Payload, Capture);
    TestTrue(TEXT("world_partition.create_datalayer handler is registered"), bFound);

    if (!bFound)
    {
        return false;
    }

    // The only two errors that mean "this host has no partitioned world to test
    // against". Anything else is a genuine defect in the verb.
    if (!Capture.bSuccess)
    {
        const bool bNoPartitionedWorld =
            Capture.ErrorCode == TEXT("NOT_PARTITIONED") || Capture.ErrorCode == TEXT("NO_WORLD");
        TestTrue(*FString::Printf(
            TEXT("create_datalayer failed with an unexpected error: %s (%s)"),
            *Capture.ErrorCode, *Capture.Message), bNoPartitionedWorld);
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-partitioned-world"),
            FString::Printf(
                TEXT("No partitioned editor world (%s); skipping the "
                     "create_datalayer.EchoesNonTransientPath echo assertions. The persistable-path "
                     "contract is NOT covered on this host - see the COVERAGE LIMIT note at the top "
                     "of this file."), *Capture.ErrorCode));
        return true;
    }

    // The handler also returns success WITHOUT creating anything when the layer already
    // exists (WorldPartitionHandler.cpp, "already exists" branch) - that response carries
    // a message and no result object, so there is no path to assert on. A previous run on
    // this host leaves the layer behind, so this is reachable and must not be a failure.
    if (Capture.Message.Contains(TEXT("already exists")))
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("datalayer-already-exists"),
            TEXT("Data layer already existed; skipping the echo assertions. "
                 "Nothing was created, so there is no dataLayerAssetPath to inspect."));
        return true;
    }

    // Reaching here means a partitioned world IS present and the layer was newly created,
    // so the handler ran the package-building path the fix installed and the echo must
    // be assertable.
    if (!TestTrue(TEXT("successful create_datalayer carries a result object"), Capture.Result.IsValid()))
    {
        return false;
    }

    FString EchoedPath;
    if (!TestTrue(TEXT("successful create_datalayer echoes a non-empty dataLayerAssetPath"),
            Capture.Result->TryGetStringField(TEXT("dataLayerAssetPath"), EchoedPath)
                && !EchoedPath.IsEmpty()))
    {
        return false;
    }

    TestFalse(TEXT("echoed dataLayerAssetPath is NOT the transient package"),
        EchoedPath.StartsWith(TEXT("/Engine/Transient")));
    TestTrue(TEXT("echoed dataLayerAssetPath is a real content asset path"),
        EchoedPath.StartsWith(TEXT("/Game/")));

    // Clean up the asset this success path created so the disposable host
    // baseline stays clean.
    CleanupTestAsset(EchoedPath);

    return true;
}
