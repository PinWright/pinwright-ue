// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression coverage for B-worldpartition-mutations-false-success.
//
// These tests exercise the engine's real World Partition/data-layer mutation
// APIs and the production handlers against an active transient editor world.

#include "Compat/EngineVersionCompat.h"
#include "Misc/AutomationTest.h"
#include "Misc/Guid.h"
#include "Editor.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Handlers/ErrorCodes.h"
#include "UObject/Package.h"
#include "Tests/TestAssetTeardown.h"
#include "Tests/TestSkipReporting.h"
#include "Tests/TestUtils.h"
#include "Tests/TestWorldUtils.h"
#if __has_include("DataLayer/DataLayerEditorSubsystem.h")
#  include "DataLayer/DataLayerEditorSubsystem.h"
#elif __has_include("WorldPartition/DataLayer/DataLayerEditorSubsystem.h")
#  include "WorldPartition/DataLayer/DataLayerEditorSubsystem.h"
#endif
#include "WorldPartition/DataLayer/DataLayerManager.h"
#include "WorldPartition/DataLayer/DataLayerInstance.h"
#include "WorldPartition/DataLayer/DataLayerInstancePrivate.h"
#include "WorldPartition/DataLayer/WorldDataLayers.h"
// IDataLayerInstanceProvider and the external-data-layer instance type both arrive in UE 5.4;
// only the cleanup test's mixed removable/unremovable half uses them, and that half is compiled
// out below on 5.3.
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 4, 0)
#include "WorldPartition/DataLayer/DataLayerInstanceProviderInterface.h"
#include "WorldPartition/DataLayer/ExternalDataLayerInstance.h"
#endif

namespace WorldPartitionDataLayerMutationTests
{
    struct FTransientWorldPartitionFixture
    {
        ~FTransientWorldPartitionFixture()
        {
            if (GEditor && GEditor->GetEditorWorldContext().World() == World)
            {
                GEditor->GetEditorWorldContext().SetCurrentWorld(OriginalEditorWorld);
            }
            WorldGuard.Reset();
            if (!WorldObjectPath.IsEmpty())
            {
                PwTestAssetTeardown::DiscardCreatedAssetByObjectPath(WorldObjectPath);
            }
        }

        bool Initialize(FAutomationTestBase& Test)
        {
            FixtureGuid = FGuid::NewGuid().ToString(EGuidFormats::Digits);
            const FString FixtureName = FString::Printf(TEXT("PinWrightWorldPartition_%s"), *FixtureGuid);
            const FString PackageName = FString::Printf(TEXT("/Game/PinWright/Transient/%s"), *FixtureName);
            UPackage* Package = CreatePackage(*PackageName);
            if (!Test.TestNotNull(TEXT("transient World Partition package was created"), Package))
            {
                return false;
            }

            UWorld::InitializationValues InitializationValues = UWorld::InitializationValues()
                .CreatePhysicsScene(false)
                .ShouldSimulatePhysics(false)
                .EnableTraceCollision(true)
                .CreateNavigation(false)
                .CreateAISystem(false)
                .CreateWorldPartition(true);
            // FWorldInitializationValues::EnableWorldPartitionStreaming arrived in UE 5.4, with the
            // WorldInitializationValues.h split; on 5.3 there is no init-time lever and a World
            // Partition world is always created with streaming on. Nothing here plays the world,
            // so the difference is inert for a data-layer fixture.
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 4, 0)
            InitializationValues.EnableWorldPartitionStreaming(false);
#endif
            World = UWorld::CreateWorld(EWorldType::Editor, /*bInformEngineOfWorld=*/false,
                FName(*FixtureName), Package, /*bAddToRoot=*/true, ERHIFeatureLevel::Num, &InitializationValues);
            if (!Test.TestNotNull(TEXT("transient World Partition world was created"), World))
            {
                return false;
            }
            WorldObjectPath = World->GetPathName();
            WorldGuard = MakeUnique<FScopedTransientWorldGuard>(World);

            if (!Test.TestNotNull(TEXT("transient world has a World Partition"), World->GetWorldPartition()) ||
                !Test.TestNotNull(TEXT("transient World Partition has a data-layer manager"), World->GetDataLayerManager()))
            {
                return false;
            }

            WorldDataLayers = AWorldDataLayers::Create(World);
            if (!Test.TestNotNull(TEXT("transient world has a WorldDataLayers actor"), WorldDataLayers))
            {
                return false;
            }
            World->SetWorldDataLayers(WorldDataLayers);
            if (!Test.TestTrue(TEXT("WorldDataLayers is attached to the transient world"),
                    World->GetWorldDataLayers() == WorldDataLayers))
            {
                return false;
            }

            DataLayer = WorldDataLayers->CreateDataLayer<UDataLayerInstancePrivate>();
            if (!Test.TestNotNull(TEXT("transient world data layer was created"), DataLayer))
            {
                return false;
            }
            DataLayerName = DataLayer->GetDataLayerShortName();
            if (!Test.TestTrue(TEXT("created data layer is present before mutation"),
                    WorldDataLayers->ContainsDataLayer(DataLayer)) ||
                !Test.TestTrue(TEXT("manager resolves the created data layer"),
                    World->GetDataLayerManager()->GetDataLayerInstance(DataLayer->GetAsset()) == DataLayer))
            {
                return false;
            }

            DataLayerSubsystem = GEditor->GetEditorSubsystem<UDataLayerEditorSubsystem>();
            if (!Test.TestNotNull(TEXT("editor data-layer subsystem was created"), DataLayerSubsystem))
            {
                return false;
            }

            OriginalEditorWorld = GEditor->GetEditorWorldContext().World();
            GEditor->GetEditorWorldContext().SetCurrentWorld(World);
            return Test.TestTrue(TEXT("transient fixture is the active editor world"),
                GEditor->GetEditorWorldContext().World() == World);
        }

        AActor* SpawnActor(FAutomationTestBase& Test, bool bCreateActorPackage)
        {
            FActorSpawnParameters SpawnParameters;
            SpawnParameters.Name = FName(*FString::Printf(TEXT("PinWrightDataLayerActor_%s_%d"),
                *FixtureGuid, SpawnedActorCount++));
            SpawnParameters.bCreateActorPackage = bCreateActorPackage;
            AActor* Actor = World->SpawnActor<AActor>(AActor::StaticClass(), FTransform::Identity, SpawnParameters);
            Test.TestNotNull(TEXT("GUID-labeled transient actor was created"), Actor);
            return Actor;
        }

        TSharedPtr<FJsonObject> MakeSetDataLayerPayload(const AActor* Actor) const
        {
            TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
            Payload->SetStringField(TEXT("actorPath"), Actor->GetPathName());
            Payload->SetStringField(TEXT("dataLayerName"), DataLayerName);
            return Payload;
        }

        FString FixtureGuid;
        FString WorldObjectPath;
        FString DataLayerName;
        UWorld* World = nullptr;
        UWorld* OriginalEditorWorld = nullptr;
        AWorldDataLayers* WorldDataLayers = nullptr;
        UDataLayerInstancePrivate* DataLayer = nullptr;
        UDataLayerEditorSubsystem* DataLayerSubsystem = nullptr;
        TUniquePtr<FScopedTransientWorldGuard> WorldGuard;
        int32 SpawnedActorCount = 0;
    };
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWorldPartitionTransientDataLayerMutationTest,
    "PinWright.world_partition.mutations.TransientWorldDataLayerReadback",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWorldPartitionTransientDataLayerMutationTest::RunTest(const FString& Parameters)
{
    if (!GEditor)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("editor_context_required"),
            TEXT("AActor data-layer assignment uses the editor data-layer asset filter; no editor context is available."));
        return true;
    }

    WorldPartitionDataLayerMutationTests::FTransientWorldPartitionFixture Fixture;
    if (!Fixture.Initialize(*this))
    {
        return false;
    }

    AActor* Actor = Fixture.SpawnActor(*this, /*bCreateActorPackage=*/true);
    if (!Actor)
    {
        return false;
    }

    TArray<AActor*> Actors;
    Actors.Add(Actor);
    TArray<UDataLayerInstance*> Layers;
    Layers.Add(Fixture.DataLayer);

    const bool bAdded = Fixture.DataLayerSubsystem->AddActorsToDataLayers(Actors, Layers);
    TestTrue(TEXT("engine reports the actor data-layer assignment changed state"), bAdded);
    TestTrue(TEXT("actor data-layer membership is present after assignment"),
        Actor->ContainsDataLayer(Fixture.DataLayer));

    const bool bDuplicateAdd = Fixture.DataLayerSubsystem->AddActorsToDataLayers(Actors, Layers);
    TestFalse(TEXT("repeating the assignment reports no engine change"), bDuplicateAdd);
    TestTrue(TEXT("repeating the assignment preserves verified membership"),
        Actor->ContainsDataLayer(Fixture.DataLayer));

    const bool bRemovedFromActor = Fixture.DataLayerSubsystem->RemoveActorsFromDataLayers(Actors, Layers);
    TestTrue(TEXT("engine reports actor data-layer removal changed state"), bRemovedFromActor);
    TestFalse(TEXT("actor data-layer membership is absent after removal"),
        Actor->ContainsDataLayer(Fixture.DataLayer));

    const FName DataLayerInstanceName = MCP_DATA_LAYER_INSTANCE_FNAME(Fixture.DataLayer);
    const FString DataLayerPath = Fixture.DataLayer->GetPathName();
    Fixture.DataLayerSubsystem->DeleteDataLayer(Fixture.DataLayer);
    TestNull(TEXT("manager no longer resolves the removed data layer"),
        Fixture.World->GetDataLayerManager()->GetDataLayerInstance(DataLayerInstanceName));

    bool bFoundRemovedLayerByPath = false;
    Fixture.WorldDataLayers->MCP_FOR_EACH_DATA_LAYER_INSTANCE(
        [&DataLayerPath, &bFoundRemovedLayerByPath](UDataLayerInstance* LayerInstance)
        {
            if (LayerInstance && LayerInstance->GetPathName() == DataLayerPath)
            {
                bFoundRemovedLayerByPath = true;
                return false;
            }
            return true;
        });
    TestFalse(TEXT("post-removal enumeration confirms the stable instance is gone"),
        bFoundRemovedLayerByPath);

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWorldPartitionSetDataLayerReadbackContractTest,
    "PinWright.world_partition.set_datalayer.ReadbackContract",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWorldPartitionSetDataLayerReadbackContractTest::RunTest(const FString& Parameters)
{
    if (!GEditor)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("editor_context_required"),
            TEXT("The handler requires an editor world and editor data-layer subsystem."));
        return true;
    }

    WorldPartitionDataLayerMutationTests::FTransientWorldPartitionFixture Fixture;
    if (!Fixture.Initialize(*this))
    {
        return false;
    }

    AActor* Actor = Fixture.SpawnActor(*this, /*bCreateActorPackage=*/true);
    if (!Actor)
    {
        return false;
    }

    FTestResponseCapture InitialCapture;
    TestTrue(TEXT("initial set_datalayer handler invocation resolves"),
        InvokeHandlerWithCapture(TEXT("world_partition.set_datalayer"),
            Fixture.MakeSetDataLayerPayload(Actor), InitialCapture));
    TestTrue(TEXT("initial set_datalayer changes membership"), InitialCapture.bSuccess);
    TestTrue(TEXT("initial handler assignment is visible through actor readback"),
        Actor->ContainsDataLayer(Fixture.DataLayer));

    FTestResponseCapture DuplicateCapture;
    TestTrue(TEXT("duplicate set_datalayer handler invocation resolves"),
        InvokeHandlerWithCapture(TEXT("world_partition.set_datalayer"),
            Fixture.MakeSetDataLayerPayload(Actor), DuplicateCapture));
    TestTrue(TEXT("duplicate set_datalayer responds"), DuplicateCapture.bWasCalled);
    TestFalse(TEXT("duplicate set_datalayer is refused"), DuplicateCapture.bSuccess);
    TestEqual(TEXT("duplicate set_datalayer has a typed error"), DuplicateCapture.ErrorCode,
        FString(ErrorCodes::ERR_DATALAYER_ALREADY_ASSIGNED));
    if (!TestTrue(TEXT("duplicate refusal retains its result payload"), DuplicateCapture.Result.IsValid()))
    {
        return false;
    }

    FString DuplicateOutcome;
    FString DuplicateActorPath;
    bool bDuplicateAdded = true;
    bool bDuplicateAlreadyPresent = false;
    bool bDuplicateEngineChanged = true;
    bool bDuplicateVerified = false;
    TestTrue(TEXT("duplicate payload reports already_present"),
        DuplicateCapture.Result->TryGetStringField(TEXT("outcome"), DuplicateOutcome));
    TestEqual(TEXT("duplicate outcome"), DuplicateOutcome, FString(TEXT("already_present")));
    TestTrue(TEXT("duplicate payload reports added"),
        DuplicateCapture.Result->TryGetBoolField(TEXT("added"), bDuplicateAdded));
    TestFalse(TEXT("duplicate payload reports no addition"), bDuplicateAdded);
    TestTrue(TEXT("duplicate payload reports alreadyPresent"),
        DuplicateCapture.Result->TryGetBoolField(TEXT("alreadyPresent"), bDuplicateAlreadyPresent));
    TestTrue(TEXT("duplicate payload confirms prior membership"), bDuplicateAlreadyPresent);
    TestTrue(TEXT("duplicate payload reports engineChanged"),
        DuplicateCapture.Result->TryGetBoolField(TEXT("engineChanged"), bDuplicateEngineChanged));
    TestFalse(TEXT("duplicate payload reports no engine change"), bDuplicateEngineChanged);
    TestTrue(TEXT("duplicate payload reports verified"),
        DuplicateCapture.Result->TryGetBoolField(TEXT("verified"), bDuplicateVerified));
    TestTrue(TEXT("duplicate payload preserves verified membership"), bDuplicateVerified);
    TestTrue(TEXT("duplicate payload identifies the actor"),
        DuplicateCapture.Result->TryGetStringField(TEXT("actorPath"), DuplicateActorPath));
    TestEqual(TEXT("duplicate payload actor path"), DuplicateActorPath, Actor->GetPathName());

    AActor* IneligibleActor = Fixture.SpawnActor(*this, /*bCreateActorPackage=*/false);
    if (!IneligibleActor)
    {
        return false;
    }
    TestFalse(TEXT("mismatch fixture actor deliberately has no external actor package"),
        IneligibleActor->IsPackageExternal());

    FTestResponseCapture MismatchCapture;
    TestTrue(TEXT("ineligible set_datalayer handler invocation resolves"),
        InvokeHandlerWithCapture(TEXT("world_partition.set_datalayer"),
            Fixture.MakeSetDataLayerPayload(IneligibleActor), MismatchCapture));
    TestTrue(TEXT("ineligible set_datalayer responds"), MismatchCapture.bWasCalled);
    TestFalse(TEXT("ineligible set_datalayer is refused"), MismatchCapture.bSuccess);
    TestEqual(TEXT("readback mismatch has a typed verification error"), MismatchCapture.ErrorCode,
        FString(ErrorCodes::ERR_VERIFICATION_FAILED));
    if (!TestTrue(TEXT("verification refusal retains its failed result payload"), MismatchCapture.Result.IsValid()))
    {
        return false;
    }

    FString MismatchOutcome;
    FString MismatchActorPath;
    bool bMismatchAdded = true;
    bool bMismatchAlreadyPresent = true;
    bool bMismatchEngineChanged = true;
    bool bMismatchVerified = true;
    TestTrue(TEXT("verification payload reports rejected outcome"),
        MismatchCapture.Result->TryGetStringField(TEXT("outcome"), MismatchOutcome));
    TestEqual(TEXT("verification outcome"), MismatchOutcome, FString(TEXT("rejected")));
    TestTrue(TEXT("verification payload reports added"),
        MismatchCapture.Result->TryGetBoolField(TEXT("added"), bMismatchAdded));
    TestFalse(TEXT("verification payload reports no addition"), bMismatchAdded);
    TestTrue(TEXT("verification payload reports alreadyPresent"),
        MismatchCapture.Result->TryGetBoolField(TEXT("alreadyPresent"), bMismatchAlreadyPresent));
    TestFalse(TEXT("verification payload reports no prior membership"), bMismatchAlreadyPresent);
    TestTrue(TEXT("verification payload reports engineChanged"),
        MismatchCapture.Result->TryGetBoolField(TEXT("engineChanged"), bMismatchEngineChanged));
    TestFalse(TEXT("verification payload exposes the engine refusal"), bMismatchEngineChanged);
    TestTrue(TEXT("verification payload reports verified"),
        MismatchCapture.Result->TryGetBoolField(TEXT("verified"), bMismatchVerified));
    TestFalse(TEXT("verification payload exposes failed membership readback"), bMismatchVerified);
    TestTrue(TEXT("verification payload identifies the rejected actor"),
        MismatchCapture.Result->TryGetStringField(TEXT("actorPath"), MismatchActorPath));
    TestEqual(TEXT("verification payload actor path"), MismatchActorPath, IneligibleActor->GetPathName());
    TestFalse(TEXT("rejected actor remains outside the data layer"),
        IneligibleActor->ContainsDataLayer(Fixture.DataLayer));

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWorldPartitionCleanupDataLayerReadbackContractTest,
    "PinWright.world_partition.cleanup_invalid_datalayers.ReadbackContract",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWorldPartitionCleanupDataLayerReadbackContractTest::RunTest(const FString& Parameters)
{
    if (!GEditor)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("editor_context_required"),
            TEXT("The handler requires an editor world and editor data-layer subsystem."));
        return true;
    }

    WorldPartitionDataLayerMutationTests::FTransientWorldPartitionFixture Fixture;
    if (!Fixture.Initialize(*this))
    {
        return false;
    }
    TestNotNull(TEXT("zero-candidate fixture contains a valid data-layer asset"),
        Fixture.DataLayer->GetAsset());

    FTestResponseCapture Capture;
    TestTrue(TEXT("cleanup_invalid_datalayers handler invocation resolves"),
        InvokeHandlerWithCapture(TEXT("world_partition.cleanup_invalid_datalayers"),
            MakeShared<FJsonObject>(), Capture));
    TestTrue(TEXT("zero-candidate cleanup responds"), Capture.bWasCalled);
    TestFalse(TEXT("zero-candidate cleanup is refused"), Capture.bSuccess);
    TestEqual(TEXT("zero-candidate cleanup has a typed error"), Capture.ErrorCode,
        FString(ErrorCodes::ERR_NO_INVALID_DATALAYERS));
    if (!TestTrue(TEXT("zero-candidate refusal retains its result payload"), Capture.Result.IsValid()))
    {
        return false;
    }

    double CandidateCount = -1.0;
    double DeletedCount = -1.0;
    double FailedCount = -1.0;
    TestTrue(TEXT("cleanup payload reports candidateCount"),
        Capture.Result->TryGetNumberField(TEXT("candidateCount"), CandidateCount));
    TestEqual(TEXT("cleanup candidateCount"), CandidateCount, 0.0);
    TestTrue(TEXT("cleanup payload reports deletedCount"),
        Capture.Result->TryGetNumberField(TEXT("deletedCount"), DeletedCount));
    TestEqual(TEXT("cleanup deletedCount"), DeletedCount, 0.0);
    TestTrue(TEXT("cleanup payload reports failedCount"),
        Capture.Result->TryGetNumberField(TEXT("failedCount"), FailedCount));
    TestEqual(TEXT("cleanup failedCount"), FailedCount, 0.0);

    const TArray<TSharedPtr<FJsonValue>>* Deleted = nullptr;
    const TArray<TSharedPtr<FJsonValue>>* Failed = nullptr;
    TestTrue(TEXT("cleanup payload retains deleted array"),
        Capture.Result->TryGetArrayField(TEXT("deleted"), Deleted) && Deleted);
    TestTrue(TEXT("cleanup payload retains failed array"),
        Capture.Result->TryGetArrayField(TEXT("failed"), Failed) && Failed);
    if (Deleted && Failed)
    {
        TestEqual(TEXT("cleanup deleted array is empty"), Deleted->Num(), 0);
        TestEqual(TEXT("cleanup failed array is empty"), Failed->Num(), 0);
    }

#if UE_VERSION_OLDER_THAN(5, 4, 0)
    // The mixed removable/unremovable half needs an instance the engine refuses to remove, and on
    // 5.3 no such instance exists: UDataLayerInstance::CanBeRemoved, UExternalDataLayerInstance and
    // IDataLayerInstanceProvider all arrive in UE 5.4. The handler's partial-failure branch is
    // compiled out on 5.3 for the same reason (Handlers/World/WorldPartitionHandler.cpp), so there
    // is nothing here to assert. Marked rather than silently green.
    PinWrightTestSkip::SkipAssertions(*this, TEXT("unremovable-data-layer-requires-5.4"),
        TEXT("UDataLayerInstance::CanBeRemoved and UExternalDataLayerInstance arrived in UE 5.4; "
             "on 5.3 no data layer instance can be engine-refused, so ERR_DELETE_PARTIAL is "
             "unreachable."));
    return true;
#else
    const FName RemovableName(*FString::Printf(TEXT("RemovableInvalidDataLayer_%s"), *Fixture.FixtureGuid));
    UDataLayerInstance* RemovableInvalid = NewObject<UDataLayerInstance>(
        Fixture.WorldDataLayers, UDataLayerInstance::StaticClass(), RemovableName, RF_Transactional);
    const FName UnremovableName(*FString::Printf(TEXT("UnremovableInvalidDataLayer_%s"), *Fixture.FixtureGuid));
    UExternalDataLayerInstance* UnremovableInvalid = NewObject<UExternalDataLayerInstance>(
        Fixture.WorldDataLayers, UExternalDataLayerInstance::StaticClass(), UnremovableName, RF_Transactional);
    if (!TestNotNull(TEXT("removable missing-asset data layer was created"), RemovableInvalid) ||
        !TestNotNull(TEXT("unremovable missing-asset data layer was created"), UnremovableInvalid))
    {
        return false;
    }
    IDataLayerInstanceProvider* DataLayerProvider =
        static_cast<IDataLayerInstanceProvider*>(Fixture.WorldDataLayers);
    DataLayerProvider->GetDataLayerInstances().Add(RemovableInvalid);
    DataLayerProvider->GetDataLayerInstances().Add(UnremovableInvalid);
    TestNull(TEXT("removable cleanup candidate has no asset"), RemovableInvalid->GetAsset());
    TestNull(TEXT("unremovable cleanup candidate has no asset"), UnremovableInvalid->GetAsset());
    TestTrue(TEXT("first cleanup candidate is removable"), RemovableInvalid->CanBeRemoved());
    TestFalse(TEXT("external cleanup candidate is refused by the engine"), UnremovableInvalid->CanBeRemoved());

    const FString RemovablePath = RemovableInvalid->GetPathName();
    const FString UnremovablePath = UnremovableInvalid->GetPathName();
    FTestResponseCapture PartialCapture;
    TestTrue(TEXT("mixed cleanup handler invocation resolves"),
        InvokeHandlerWithCapture(TEXT("world_partition.cleanup_invalid_datalayers"),
            MakeShared<FJsonObject>(), PartialCapture));
    TestTrue(TEXT("mixed cleanup responds"), PartialCapture.bWasCalled);
    TestFalse(TEXT("mixed cleanup is not reported as full success"), PartialCapture.bSuccess);
    TestEqual(TEXT("mixed cleanup has a typed partial error"), PartialCapture.ErrorCode,
        FString(ErrorCodes::ERR_DELETE_PARTIAL));
    if (!TestTrue(TEXT("mixed cleanup retains its result payload"), PartialCapture.Result.IsValid()))
    {
        return false;
    }

    CandidateCount = -1.0;
    DeletedCount = -1.0;
    FailedCount = -1.0;
    PartialCapture.Result->TryGetNumberField(TEXT("candidateCount"), CandidateCount);
    PartialCapture.Result->TryGetNumberField(TEXT("deletedCount"), DeletedCount);
    PartialCapture.Result->TryGetNumberField(TEXT("failedCount"), FailedCount);
    TestEqual(TEXT("mixed cleanup candidateCount"), CandidateCount, 2.0);
    TestEqual(TEXT("mixed cleanup deletedCount"), DeletedCount, 1.0);
    TestEqual(TEXT("mixed cleanup failedCount"), FailedCount, 1.0);

    const TSharedPtr<FJsonObject> DeletedEntry = JsonArrayFindObjectByStringField(
        PartialCapture.Result, TEXT("deleted"), TEXT("dataLayerPath"), RemovablePath);
    const TSharedPtr<FJsonObject> FailedEntry = JsonArrayFindObjectByStringField(
        PartialCapture.Result, TEXT("failed"), TEXT("dataLayerPath"), UnremovablePath);
    TestTrue(TEXT("mixed cleanup returns the verified deleted entry"), DeletedEntry.IsValid());
    TestTrue(TEXT("mixed cleanup returns the engine-refused failed entry"), FailedEntry.IsValid());
    if (DeletedEntry.IsValid() && FailedEntry.IsValid())
    {
        bool bDeletedVerified = false;
        bool bFailedVerified = true;
        bool bFailedAssetMissing = false;
        FString FailureReason;
        TestTrue(TEXT("deleted entry carries verified"),
            DeletedEntry->TryGetBoolField(TEXT("verified"), bDeletedVerified));
        TestTrue(TEXT("deleted entry is verified"), bDeletedVerified);
        TestTrue(TEXT("failed entry carries verified"),
            FailedEntry->TryGetBoolField(TEXT("verified"), bFailedVerified));
        TestFalse(TEXT("failed entry is not verified"), bFailedVerified);
        TestTrue(TEXT("failed entry carries assetMissing"),
            FailedEntry->TryGetBoolField(TEXT("assetMissing"), bFailedAssetMissing));
        TestTrue(TEXT("failed entry confirms the missing asset"), bFailedAssetMissing);
        TestTrue(TEXT("failed entry carries a reason"),
            FailedEntry->TryGetStringField(TEXT("reason"), FailureReason));
        TestFalse(TEXT("failed entry reason is non-empty"), FailureReason.IsEmpty());
    }

    bool bRemovableStillPresent = false;
    bool bUnremovableStillPresent = false;
    Fixture.WorldDataLayers->ForEachDataLayerInstance(
        [&](UDataLayerInstance* LayerInstance)
        {
            if (LayerInstance)
            {
                bRemovableStillPresent |= LayerInstance->GetPathName() == RemovablePath;
                bUnremovableStillPresent |= LayerInstance->GetPathName() == UnremovablePath;
            }
            return true;
        });
    TestFalse(TEXT("mixed cleanup readback confirms the removable instance is gone"),
        bRemovableStillPresent);
    TestTrue(TEXT("mixed cleanup readback confirms the refused instance remains"),
        bUnremovableStillPresent);

    return true;
#endif // UE_VERSION_OLDER_THAN(5, 4, 0)
}
