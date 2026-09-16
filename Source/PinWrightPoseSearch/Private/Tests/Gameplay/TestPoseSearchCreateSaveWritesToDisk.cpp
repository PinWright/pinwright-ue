// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression test for B-pose-search-create-save-no-disk-write.
//
// pose_search.create_schema / create_database / add_database_animation take a
// save param that defaults to true, return an AddAssetVerification block with
// existsAfter:true plus an explicit saved:true, and so imply the new
// UPoseSearchSchema / UPoseSearchDatabase .uasset is on disk. It is not. With
// save:true the create/mutate paths route through the shared mark-dirty-only
// helper McpSafeAssetSave (HandleCreateSchema at the direct :486 site;
// create_database / add_database_animation through the FinishPoseSearchAsset
// :159 chokepoint), which only MarkPackageDirty() + FAssetRegistryModule::
// AssetCreated() and returns true without writing any package to disk. The
// asset lives only in memory + the registry (hence existsAfter:true and the
// in-session asset.dump readback succeeding), and vanishes on a cold editor
// restart / git reset, while every call reports saved:true. The accepted
// sibling fixes (B-metasound / B-audio / B-niagara / B-material create-save)
// route the save through the real-save helper SaveAssetToDiskReportingPresence
// (forced SaveLoadedAsset + IFileManager::FileSize disk probe) and report an
// honest saved / pendingFlush; pose_search was left unfixed.
//
// This test drives the real registered pose_search.create_schema handler through
// the dispatcher with save:true against an in-code skeleton fixture (the proven
// approach in TestPoseSearchHandlers.cpp), then FileSize-probes the schema's
// package file on disk. Pre-fix McpSafeAssetSave writes nothing, so FileSize < 0
// and the disk-presence assertion fails — the red side. Post-fix (real force-save)
// the .uasset lands on disk and the assertion passes. The disk probe is the true
// differential; the saved:true field merely echoes the requested flag pre-fix and
// only becomes an honest disk-gated verdict post-fix.

#include "Misc/AutomationTest.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Compat/EngineVersionCompat.h"
#include "HAL/FileManager.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"
#include "Tests/TestUtils.h"

#if __has_include("PoseSearch/PoseSearchSchema.h") && __has_include("PoseSearch/PoseSearchFeatureChannel_Position.h")
#include "Animation/Skeleton.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Modules/ModuleManager.h"
#include "PoseSearch/PoseSearchFeatureChannel_Position.h"
#include "PoseSearch/PoseSearchSchema.h"
#include "ReferenceSkeleton.h"
#include "UObject/Package.h"
#define MCP_TEST_HAS_POSESEARCH 1
#else
#define MCP_TEST_HAS_POSESEARCH 0
#endif

#if MCP_TEST_HAS_POSESEARCH

namespace
{
// Distinct helper names (not the anonymous-namespace helpers in
// TestPoseSearchHandlers.cpp) so a Unity merge of the two sibling Gameplay TUs
// does not collide on an ODR duplicate.
FString MakeUniquePoseSearchSavePath(const TCHAR* Prefix)
{
    return FString::Printf(
        TEXT("/Game/PinWrightTests/%s_%s"),
        Prefix,
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));
}

USkeleton* BuildPoseSearchSaveTestSkeleton(const FString& PackagePath)
{
    UPackage* Package = CreatePackage(*PackagePath);
    USkeleton* Skeleton = NewObject<USkeleton>(
        Package,
        *FPackageName::GetLongPackageAssetName(PackagePath),
        RF_Public | RF_Standalone | RF_Transient);
    if (!Skeleton)
    {
        return nullptr;
    }

    {
        FReferenceSkeletonModifier Modifier(Skeleton);
        Modifier.Add(FMeshBoneInfo(FName(TEXT("root")), TEXT("root"), INDEX_NONE), FTransform::Identity);
    }

    Skeleton->AddToRoot();
    FAssetRegistryModule::AssetCreated(Skeleton);
    return Skeleton;
}
} // namespace

// save:true on pose_search.create_schema must persist the new UPoseSearchSchema
// .uasset to disk. Pre-fix the mark-dirty-only McpSafeAssetSave writes nothing, so
// the on-disk FileSize is < 0 and this test fails at the disk-presence assertion.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPoseSearchCreateSchemaSaveWritesToDiskTest,
    "PinWright.pose_search.CreateSchemaSaveWritesToDisk",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPoseSearchCreateSchemaSaveWritesToDiskTest::RunTest(const FString& Parameters)
{
    // The defect only manifests with the PoseSearch runtime module available; this
    // host enables the PoseSearch plugin (uproject), so a not-loaded module is a real
    // precondition failure, not a skip that would silently green the suite.
    if (!FModuleManager::Get().IsModuleLoaded(TEXT("PoseSearch")))
    {
        AddError(TEXT("PoseSearch module is not loaded; cannot reproduce the create_schema save-to-disk defect."));
        return false;
    }

    const FString SkeletonPackagePath = MakeUniquePoseSearchSavePath(TEXT("SK_PSSave"));
    const FString SchemaPackagePath = MakeUniquePoseSearchSavePath(TEXT("PSSchemaSave"));

    USkeleton* Skeleton = BuildPoseSearchSaveTestSkeleton(SkeletonPackagePath);
    ON_SCOPE_EXIT
    {
        if (Skeleton)
        {
            Skeleton->RemoveFromRoot();
        }
        CleanupTestAsset(SchemaPackagePath);
        CleanupTestAsset(SkeletonPackagePath);
    };

    TestNotNull(TEXT("in-code skeleton fixture created"), Skeleton);
    if (!Skeleton)
    {
        return false;
    }

    TSharedPtr<FJsonObject> PositionChannel = MakeShared<FJsonObject>();
    PositionChannel->SetStringField(TEXT("type"), TEXT("Position"));
    PositionChannel->SetStringField(TEXT("bone"), TEXT("root"));

    TArray<TSharedPtr<FJsonValue>> Channels;
    Channels.Add(MakeShared<FJsonValueObject>(PositionChannel));

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), SchemaPackagePath);
    Payload->SetStringField(TEXT("skeleton"), ToObjectPath(SkeletonPackagePath));
    Payload->SetArrayField(TEXT("channels"), Channels);
    Payload->SetBoolField(TEXT("save"), true);

    FTestResponseCapture Capture;
    const bool bFound = InvokeHandlerWithCapture(TEXT("pose_search.create_schema"), Payload, Capture);
    TestTrue(TEXT("pose_search.create_schema handler is registered"), bFound);
    TestTrue(TEXT("pose_search.create_schema responded"), Capture.bWasCalled);
    TestTrue(TEXT("pose_search.create_schema succeeded"), Capture.bSuccess);
    if (!bFound || !Capture.bSuccess || !Capture.Result.IsValid())
    {
        AddError(FString::Printf(TEXT("create_schema did not succeed: %s %s"),
            *Capture.ErrorCode, *Capture.Message));
        return false;
    }

    // Resolve the schema's on-disk package filename from the response assetPath
    // (AddAssetVerification overwrites it with the asset's resolved path).
    FString AssetPath;
    Capture.Result->TryGetStringField(TEXT("assetPath"), AssetPath);
    const FString PackageFilename = PackageFilenameFromAssetPath(AssetPath);
    TestFalse(TEXT("resolved a package filename from assetPath"), PackageFilename.IsEmpty());

    // Load-bearing differential assertion: save:true must land the schema .uasset on
    // disk. Pre-fix McpSafeAssetSave only marks the package dirty and never writes any
    // file, so FileSize < 0 and this fails; post-fix (real force-save) the file exists.
    const int64 OnDiskSize = IFileManager::Get().FileSize(*PackageFilename);
    TestTrue(
        *FString::Printf(TEXT("save:true persists the schema .uasset to disk (size=%lld at %s)"),
            OnDiskSize, *PackageFilename),
        OnDiskSize > 0);

    // Honest-reporting side: a save:true schema create must not report saved:true unless
    // the file actually reached disk. Pre-fix `saved` merely echoes the requested flag
    // (always true), so it cannot tell a real save from a dirty-only no-op — the disk
    // probe above is the genuine differential; this documents the contract.
    bool bSavedReported = false;
    Capture.Result->TryGetBoolField(TEXT("saved"), bSavedReported);
    TestTrue(TEXT("save:true reports saved:true"), bSavedReported);

    return true;
}

// save:true on pose_search.create_database must persist the new UPoseSearchDatabase .uasset
// to disk. Unlike create_schema (which saves via a direct SaveAssetToDiskReportingPresence
// call), create_database routes its save through the FinishPoseSearchAsset chokepoint that
// add_database_animation shares. A regression that reverted that chokepoint to the
// mark-dirty-only McpSafeAssetSave (or mis-threaded its bool return) would leave the database
// silently non-persisting while every call still reports saved:true — the create_schema test
// would stay green. This test's on-disk FileSize probe is that missing differential: pre-fix
// FinishPoseSearchAsset writes nothing (FileSize < 0, red), post-fix the .uasset lands (green).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPoseSearchCreateDatabaseSaveWritesToDiskTest,
    "PinWright.pose_search.CreateDatabaseSaveWritesToDisk",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPoseSearchCreateDatabaseSaveWritesToDiskTest::RunTest(const FString& Parameters)
{
    if (!FModuleManager::Get().IsModuleLoaded(TEXT("PoseSearch")))
    {
        AddError(TEXT("PoseSearch module is not loaded; cannot reproduce the create_database save-to-disk defect."));
        return false;
    }

    const FString SkeletonPackagePath = MakeUniquePoseSearchSavePath(TEXT("SK_PSDbSave"));
    const FString SchemaPackagePath = MakeUniquePoseSearchSavePath(TEXT("PSSchemaDbSave"));
    const FString DatabasePackagePath = MakeUniquePoseSearchSavePath(TEXT("PSDbSave"));

    USkeleton* Skeleton = BuildPoseSearchSaveTestSkeleton(SkeletonPackagePath);
    ON_SCOPE_EXIT
    {
        if (Skeleton)
        {
            Skeleton->RemoveFromRoot();
        }
        CleanupTestAsset(DatabasePackagePath);
        CleanupTestAsset(SchemaPackagePath);
        CleanupTestAsset(SkeletonPackagePath);
    };

    TestNotNull(TEXT("in-code skeleton fixture created"), Skeleton);
    if (!Skeleton)
    {
        return false;
    }

    // Build a real, correctly-typed schema for the database to bind by driving the production
    // create_schema handler (the same handler the create_schema test covers). The database
    // only needs the schema resolvable in memory, so this holds pre-fix and post-fix.
    TSharedPtr<FJsonObject> PositionChannel = MakeShared<FJsonObject>();
    PositionChannel->SetStringField(TEXT("type"), TEXT("Position"));
    PositionChannel->SetStringField(TEXT("bone"), TEXT("root"));

    TArray<TSharedPtr<FJsonValue>> Channels;
    Channels.Add(MakeShared<FJsonValueObject>(PositionChannel));

    TSharedPtr<FJsonObject> SchemaPayload = MakeShared<FJsonObject>();
    SchemaPayload->SetStringField(TEXT("assetPath"), SchemaPackagePath);
    SchemaPayload->SetStringField(TEXT("skeleton"), ToObjectPath(SkeletonPackagePath));
    SchemaPayload->SetArrayField(TEXT("channels"), Channels);
    SchemaPayload->SetBoolField(TEXT("save"), true);

    FTestResponseCapture SchemaCapture;
    const bool bSchemaFound = InvokeHandlerWithCapture(TEXT("pose_search.create_schema"), SchemaPayload, SchemaCapture);
    TestTrue(TEXT("pose_search.create_schema handler is registered"), bSchemaFound);
    TestTrue(TEXT("schema fixture created"), SchemaCapture.bSuccess);
    if (!bSchemaFound || !SchemaCapture.bSuccess)
    {
        AddError(FString::Printf(TEXT("schema fixture create failed: %s %s"),
            *SchemaCapture.ErrorCode, *SchemaCapture.Message));
        return false;
    }

    // Drive the real create_database handler with save:true; its save flows through the
    // FinishPoseSearchAsset chokepoint under test.
    TSharedPtr<FJsonObject> DatabasePayload = MakeShared<FJsonObject>();
    DatabasePayload->SetStringField(TEXT("assetPath"), DatabasePackagePath);
    DatabasePayload->SetStringField(TEXT("schema"), ToObjectPath(SchemaPackagePath));
    DatabasePayload->SetBoolField(TEXT("save"), true);

    FTestResponseCapture Capture;
    const bool bFound = InvokeHandlerWithCapture(TEXT("pose_search.create_database"), DatabasePayload, Capture);
    TestTrue(TEXT("pose_search.create_database handler is registered"), bFound);
    TestTrue(TEXT("pose_search.create_database responded"), Capture.bWasCalled);
    TestTrue(TEXT("pose_search.create_database succeeded"), Capture.bSuccess);
    if (!bFound || !Capture.bSuccess || !Capture.Result.IsValid())
    {
        AddError(FString::Printf(TEXT("create_database did not succeed: %s %s"),
            *Capture.ErrorCode, *Capture.Message));
        return false;
    }

    FString AssetPath;
    Capture.Result->TryGetStringField(TEXT("assetPath"), AssetPath);
    const FString PackageFilename = PackageFilenameFromAssetPath(AssetPath);
    TestFalse(TEXT("resolved a package filename from assetPath"), PackageFilename.IsEmpty());

    // Load-bearing differential assertion: save:true must land the database .uasset on disk.
    // Pre-fix the FinishPoseSearchAsset chokepoint calls the mark-dirty-only McpSafeAssetSave
    // and writes no file, so FileSize < 0 and this fails; post-fix (real force-save) it exists.
    const int64 OnDiskSize = IFileManager::Get().FileSize(*PackageFilename);
    TestTrue(
        *FString::Printf(TEXT("save:true persists the database .uasset to disk (size=%lld at %s)"),
            OnDiskSize, *PackageFilename),
        OnDiskSize > 0);

    bool bSavedReported = false;
    Capture.Result->TryGetBoolField(TEXT("saved"), bSavedReported);
    TestTrue(TEXT("save:true reports saved:true"), bSavedReported);

    return true;
}

#endif // MCP_TEST_HAS_POSESEARCH
