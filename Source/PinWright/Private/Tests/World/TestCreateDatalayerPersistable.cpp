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
//   - EchoesNonTransientPath invokes the handler, which short-circuits with
//     NOT_PARTITIONED (WorldPartitionHandler.cpp:111) on a non-partitioned world, so
//     it swaps in its own World Partition world fixture (GEditor->NewMap(true)) first.
//     It asserts the echoed dataLayerAssetPath string, which the handler computes
//     before NewObject, so it still does not observe the asset's actual outer.

#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Editor.h"
#include "Engine/World.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Tests/TestSkipReporting.h"
#include "Tests/TestUtils.h"
#include "Tests/TestWorldUtils.h"
#include "Utils/MapSwapDirtyWorldGuard.h"
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
// On a partitioned world the echoed dataLayerAssetPath must be a real /Game asset
// path, never a transient one. The test builds that world itself, so any error is a
// real failure rather than a skip.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWorldCreateDatalayerEchoesNonTransientPathTest,
    "PinWright.world_partition.create_datalayer.EchoesNonTransientPath",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWorldCreateDatalayerEchoesNonTransientPathTest::RunTest(const FString& Parameters)
{
    if (!GEditor)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("No GEditor; the partitioned fixture world cannot be created."));
        return true;
    }

    // The suite runs on a blank non-partitioned world (aa_suite_start), where the handler exits
    // NOT_PARTITIONED before it builds a package. Swap in a throwaway World Partition world behind
    // the shared pre-swap probe; MapGuard rebuilds the blank world on scope exit.
    FScopedEditorWorldMapGuard MapGuard;
    const PinWrightMapSwapGuard::FWorldSurvivorProbeResult Probe =
        PinWrightMapSwapGuard::ProbeResidentWorldSurvivors(
            FString(), /*bTransactionBufferWillBeCleared=*/false);
    if (Probe.bProbeUnavailable || Probe.IsBlocked())
    {
        const FString Reason = Probe.bProbeUnavailable
            ? Probe.UnavailableReason
            : PinWrightMapSwapGuard::DescribeSurvivorRefusal(Probe);
        PinWrightTestSkip::SkipAssertions(*this, TEXT("map-swap-refused"),
            FString::Printf(TEXT("The pre-swap guard would not clear a NewMap, so no partitioned "
                "fixture world was created: %s"), *Reason));
        return true;
    }
    UWorld* const PartitionedWorld = GEditor->NewMap(/*bIsPartitionedWorld=*/true);
    if (!TestTrue(TEXT("a World Partition fixture world is the active editor world"),
            PartitionedWorld && PartitionedWorld->IsPartitionedWorld()
                && GEditor->GetEditorWorldContext().World() == PartitionedWorld))
    {
        return false;
    }

    // Under the scratch root, so the asset the verb creates never lands in host content.
    const FString AssetFolder = FString::Printf(TEXT("/Game/PinWrightTests/DataLayers_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("dataLayerName"), TEXT("PersistenceProbeLayer"));
    Payload->SetStringField(TEXT("dataLayerAssetPath"), AssetFolder);

    FTestResponseCapture Capture;
    const bool bFound = InvokeHandlerWithCapture(TEXT("world_partition.create_datalayer"), Payload, Capture);
    TestTrue(TEXT("world_partition.create_datalayer handler is registered"), bFound);

    if (!bFound)
    {
        return false;
    }

    // The active world is the partitioned fixture, so every refusal is a defect in the verb.
    if (!TestTrue(*FString::Printf(TEXT("create_datalayer succeeded on a partitioned world (error %s: %s)"),
            *Capture.ErrorCode, *Capture.Message), Capture.bSuccess))
    {
        return false;
    }

    // The fixture world is new, so the handler's "already exists" branch (success with no
    // result object) cannot be legitimate here.
    TestFalse(TEXT("a fresh partitioned world has no pre-existing data layer"),
        Capture.Message.Contains(TEXT("already exists")));
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
    TestEqual(TEXT("the asset is created in the requested dataLayerAssetPath folder"),
        EchoedPath, AssetFolder / TEXT("PersistenceProbeLayer"));

    // Clean up the asset this success path created so the disposable host
    // baseline stays clean.
    CleanupTestAsset(EchoedPath);

    return true;
}
