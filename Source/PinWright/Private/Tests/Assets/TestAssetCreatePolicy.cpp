// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression coverage for B0-b (B-asset-create-modal-deadlock): AssetCreatePolicy is
// the non-interactive answer to "something already occupies the path this create verb
// wants to write". Before it, IAssetTools::CreateAsset walked into
// UAssetToolsImpl::CanCreateAsset, which raises three modals in a row (overwrite
// prompt -> ObjectTools reference-check prompt -> "referenced by other content"
// notice) and owns the game thread until a human clicks - a deadlock for every client.
//
// Every case runs inside an explicit FScopedUnattendedRpc and asserts
// GIsRunningUnattendedScript held for the duration. That is the machine-checkable
// proxy for "raised no dialog": a direct assertion is impossible because the
// automation harness runs -unattended, which suppresses FMessageDialog before any
// observable delegate fires.
//
// Counterfactual: revert the Resolve() call in
// material.authoring.create_material and the end-to-end idempotency test reports
// mode:"created" on the second call (having silently rebuilt the material) instead of
// mode:"updated_in_place".

#include "Misc/AutomationTest.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Components/StaticMeshComponent.h"
#include "CoreGlobals.h"
#include "Dispatch/RpcDispatcher.h"
#include "Dispatch/ScopedUnattendedRpc.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Engine/DataTable.h"
#include "Engine/Selection.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "Factories/MaterialInstanceConstantFactoryNew.h"
#include "Handlers/ErrorCodes.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"
#include "PinWrightHelpers.h"
#include "PinWrightSettings.h"
#include "Tests/Infra/DispatcherTestHelpers.h"
#include "Tests/TestUtils.h"
#include "Tests/TestWorldUtils.h"
#include "UObject/Package.h"
#include "Utils/AssetCreatePolicy.h"

// File-scope helper names carry the AssetCreatePolicyTest_ prefix: Unity merges
// translation units, so a plain UniquePath()/MakeMaterial() static would collide with
// a same-named static in another test file.

static FString AssetCreatePolicyTest_UniquePackagePath(const TCHAR* Prefix)
{
    return FString::Printf(TEXT("/Game/PinWrightTests/AssetCreatePolicy/%s_%s"),
        Prefix, *FGuid::NewGuid().ToString(EGuidFormats::Digits));
}

// A bare UMaterial in its own package - the shape a create verb would leave behind.
static UMaterial* AssetCreatePolicyTest_MakeMaterial(const FString& PackagePath)
{
    UPackage* Package = CreatePackage(*PackagePath);
    if (!Package)
    {
        return nullptr;
    }
    UMaterial* Material = NewObject<UMaterial>(
        Package, FName(*FPackageName::GetLongPackageAssetName(PackagePath)),
        RF_Public | RF_Standalone);
    if (Material)
    {
        Material->MarkPackageDirty();
        FAssetRegistryModule::AssetCreated(Material);
    }
    return Material;
}

// A UMaterialInstanceConstant parented to Parent - the referencer the policy must
// protect on the default path and report on the overwrite path.
static UMaterialInstanceConstant* AssetCreatePolicyTest_MakeInstance(
    const FString& PackagePath, UMaterial* Parent)
{
    UPackage* Package = CreatePackage(*PackagePath);
    if (!Package || !Parent)
    {
        return nullptr;
    }
    UMaterialInstanceConstantFactoryNew* Factory = NewObject<UMaterialInstanceConstantFactoryNew>();
    Factory->InitialParent = Parent;
    UMaterialInstanceConstant* Instance = Cast<UMaterialInstanceConstant>(
        Factory->FactoryCreateNew(UMaterialInstanceConstant::StaticClass(), Package,
            FName(*FPackageName::GetLongPackageAssetName(PackagePath)),
            RF_Public | RF_Standalone, nullptr, GWarn));
    if (Instance)
    {
        Instance->SetParentEditorOnly(Parent);
        Instance->PostEditChange();
        Instance->MarkPackageDirty();
        FAssetRegistryModule::AssetCreated(Instance);
    }
    return Instance;
}

// A bare UStaticMesh in its own unsaved package - the shape geometry.* / model.compile
// leaves behind. RenderData stays null, which is safe: FStaticMeshComponentHelper::
// CreateSceneProxy bails on a null GetRenderData() rather than asserting (UE 5.8
// StaticMeshComponentHelper.h:498), so a component can hold this mesh without rendering it.
static UStaticMesh* AssetCreatePolicyTest_MakeStaticMesh(const FString& PackagePath)
{
    UPackage* Package = CreatePackage(*PackagePath);
    if (!Package)
    {
        return nullptr;
    }
    return NewObject<UStaticMesh>(
        Package, FName(*FPackageName::GetLongPackageAssetName(PackagePath)),
        RF_Public | RF_Standalone);
}

// A REAL level actor (no RF_Transient) whose UStaticMeshComponent holds Mesh - the live
// in-memory referencer that refuses the overwrite delete. RF_Transient is deliberately not
// set: this fixture exists to be seen by the object-graph walk and by the level, and the
// plugin's testing rules call transient probes out as invisible to the level-actor paths.
// Pair with FScopedEditorWorldActorGuard, which destroys it on scope exit.
static AStaticMeshActor* AssetCreatePolicyTest_SpawnMeshHolder(
    UWorld* World, UStaticMesh* Mesh, const FString& Label)
{
    if (!World || !Mesh)
    {
        return nullptr;
    }
    AStaticMeshActor* Actor = World->SpawnActor<AStaticMeshActor>(
        AStaticMeshActor::StaticClass(), FVector::ZeroVector, FRotator::ZeroRotator);
    if (!Actor)
    {
        return nullptr;
    }
    Actor->GetStaticMeshComponent()->SetStaticMesh(Mesh);
    Actor->SetActorLabel(Label);
    return Actor;
}

// Forces the modal-suppression kill switch on for a test body and restores both it and
// the process-global afterwards, so an interactively-configured editor cannot make an
// assertion pass or fail for the wrong reason.
struct FAssetCreatePolicyTestSuppressionFixture
{
    UPinWrightSettings* Settings = GetMutableDefault<UPinWrightSettings>();
    bool bSavedSetting = false;
    bool bSavedGlobal = GIsRunningUnattendedScript;

    FAssetCreatePolicyTestSuppressionFixture()
    {
        PinWrightAutomationMode::ResetForTests();
        if (Settings)
        {
            bSavedSetting = Settings->bSuppressModalDialogsDuringRpc;
            Settings->bSuppressModalDialogsDuringRpc = true;
        }
        GIsRunningUnattendedScript = false;
    }

    ~FAssetCreatePolicyTestSuppressionFixture()
    {
        PinWrightAutomationMode::ResetForTests();
        if (Settings)
        {
            Settings->bSuppressModalDialogsDuringRpc = bSavedSetting;
        }
        GIsRunningUnattendedScript = bSavedGlobal;
    }
};

// -----------------------------------------------------------------------------
// Same class, no overwrite -> UpdateInPlace: the SAME object comes back and its
// referencer survives. This is the idempotency contract; rebuilding the asset is what
// broke the referencing material instance before.
// -----------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetCreatePolicyUpdateInPlaceTest,
    "PinWright.assets.AssetCreatePolicy.UpdateInPlacePreservesExistingAndReferencers",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAssetCreatePolicyUpdateInPlaceTest::RunTest(const FString& /*Parameters*/)
{
    FAssetCreatePolicyTestSuppressionFixture Suppression;

    const FString MatPath = AssetCreatePolicyTest_UniquePackagePath(TEXT("M_Probe"));
    const FString MiPath = AssetCreatePolicyTest_UniquePackagePath(TEXT("MI_Probe"));
    ON_SCOPE_EXIT
    {
        CleanupTestAsset(MiPath);
        CleanupTestAsset(MatPath);
    };

    UMaterial* Material = AssetCreatePolicyTest_MakeMaterial(MatPath);
    if (!TestNotNull(TEXT("probe material created"), Material)) return false;
    UMaterialInstanceConstant* Instance = AssetCreatePolicyTest_MakeInstance(MiPath, Material);
    if (!TestNotNull(TEXT("probe material instance created"), Instance)) return false;

    {
        FScopedUnattendedRpc UnattendedScope;
        TestTrue(TEXT("GIsRunningUnattendedScript held for the whole resolution"),
            GIsRunningUnattendedScript);

        const AssetCreatePolicy::FResolution Resolution = AssetCreatePolicy::Resolve(
            MatPath, FPackageName::GetLongPackageAssetName(MatPath),
            UMaterial::StaticClass(), /*bOverwriteRequested=*/false);

        TestTrue(TEXT("action is UpdateInPlace"),
            Resolution.Action == AssetCreatePolicy::EAction::UpdateInPlace);
        TestTrue(TEXT("resolution reports an existing asset"), Resolution.bExistingFound);
        TestTrue(TEXT("the SAME UMaterial instance comes back, not a rebuilt one"),
            Resolution.Existing == Material);

        TSharedPtr<FJsonObject> Report = MakeShared<FJsonObject>();
        AssetCreatePolicy::AddCreateReport(Report, Resolution);
        bool bExistingField = false;
        Report->TryGetBoolField(TEXT("existing"), bExistingField);
        FString ModeField;
        Report->TryGetStringField(TEXT("mode"), ModeField);
        TestTrue(TEXT("report carries existing:true"), bExistingField);
        TestEqual(TEXT("report carries mode:updated_in_place"), ModeField, FString(TEXT("updated_in_place")));
    }

    TestTrue(TEXT("the material instance's Parent still resolves to the same material"),
        Instance->Parent == Material);
    return true;
}

// -----------------------------------------------------------------------------
// Same class, overwrite requested, asset referenced -> Rejected ASSET_IN_USE with the
// referencer list. No delete is attempted, so the referencing instance is never
// orphaned and no engine prompt is reachable.
// -----------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetCreatePolicyOverwriteInUseTest,
    "PinWright.assets.AssetCreatePolicy.OverwriteRejectedWhenReferenced",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAssetCreatePolicyOverwriteInUseTest::RunTest(const FString& /*Parameters*/)
{
    FAssetCreatePolicyTestSuppressionFixture Suppression;

    const FString MatPath = AssetCreatePolicyTest_UniquePackagePath(TEXT("M_Probe"));
    const FString MiPath = AssetCreatePolicyTest_UniquePackagePath(TEXT("MI_Probe"));
    ON_SCOPE_EXIT
    {
        CleanupTestAsset(MiPath);
        CleanupTestAsset(MatPath);
    };

    UMaterial* Material = AssetCreatePolicyTest_MakeMaterial(MatPath);
    if (!TestNotNull(TEXT("probe material created"), Material)) return false;
    UMaterialInstanceConstant* Instance = AssetCreatePolicyTest_MakeInstance(MiPath, Material);
    if (!TestNotNull(TEXT("probe material instance created"), Instance)) return false;

    // The asset registry only learns MI -> M from a SAVED package's dependency data, so
    // both packages go to disk and the instance file is force-rescanned.
    TestTrue(TEXT("probe material saved to disk"),
        SaveAssetToDiskReportingPresence(Material, /*bForce=*/true));
    TestTrue(TEXT("probe material instance saved to disk"),
        SaveAssetToDiskReportingPresence(Instance, /*bForce=*/true));

    IAssetRegistry& Registry =
        FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
    const TArray<FString> ScanFiles = {
        FPackageName::LongPackageNameToFilename(MiPath, FPackageName::GetAssetPackageExtension())
    };
    Registry.ScanFilesSynchronous(ScanFiles, /*bForceRescan=*/true);

    {
        FScopedUnattendedRpc UnattendedScope;
        TestTrue(TEXT("GIsRunningUnattendedScript held for the whole resolution"),
            GIsRunningUnattendedScript);

        const AssetCreatePolicy::FResolution Resolution = AssetCreatePolicy::Resolve(
            MatPath, FPackageName::GetLongPackageAssetName(MatPath),
            UMaterial::StaticClass(), /*bOverwriteRequested=*/true);

        TestTrue(TEXT("action is Rejected"), Resolution.IsRejected());
        TestEqual(TEXT("error code is ASSET_IN_USE"),
            Resolution.ErrorCode, FString(ErrorCodes::ERR_ASSET_IN_USE));
        TestTrue(TEXT("referencerCount is at least 1"), Resolution.ReferencerCount >= 1);
        TestTrue(TEXT("referencers name the material instance's package"),
            Resolution.Referencers.Contains(MiPath));
        // The remedy has to be the one that works. Dropping overwrite takes the
        // UpdateInPlace branch and keeps every referencer; the message must lead with it.
        TestTrue(TEXT("the message leads with the drop-overwrite remedy"),
            Resolution.ErrorMessage.Contains(TEXT("Drop overwrite")));

        const TSharedPtr<FJsonObject> ErrorData = AssetCreatePolicy::MakeErrorData(Resolution);
        if (TestTrue(TEXT("rejection carries error data"), ErrorData.IsValid()))
        {
            const TArray<TSharedPtr<FJsonValue>>* RefArray = nullptr;
            TestTrue(TEXT("error data carries a referencers array"),
                ErrorData->TryGetArrayField(TEXT("referencers"), RefArray));
            double ReportedCount = 0.0;
            ErrorData->TryGetNumberField(TEXT("referencerCount"), ReportedCount);
            TestTrue(TEXT("error data carries referencerCount >= 1"), ReportedCount >= 1.0);

            // The actor pair is emitted on BOTH ASSET_IN_USE branches so the payload shape
            // does not shift under the caller. This branch never ran the object-graph walk,
            // so it reads [] / 0 rather than being absent.
            const TArray<TSharedPtr<FJsonValue>>* ActorArray = nullptr;
            if (TestTrue(TEXT("error data carries a referencingActors array on this branch too"),
                    ErrorData->TryGetArrayField(TEXT("referencingActors"), ActorArray))
                && ActorArray)
            {
                TestEqual(TEXT("no live level actor is claimed for a registry-only rejection"),
                    ActorArray->Num(), 0);
            }
            double ReportedActorCount = -1.0;
            ErrorData->TryGetNumberField(TEXT("referencingActorCount"), ReportedActorCount);
            TestEqual(TEXT("referencingActorCount is 0 on the registry branch"),
                ReportedActorCount, 0.0);
        }
    }

    // Nothing was deleted: the rejection happens before any delete is attempted.
    TestNotNull(TEXT("the referenced material still exists after the rejected overwrite"),
        FindObject<UMaterial>(nullptr, *ToObjectPath(MatPath)));
    return true;
}

// -----------------------------------------------------------------------------
// Same class, overwrite requested, NO on-disk referencer, but a live level actor holds the
// object -> the delete is refused in memory and the rejection NAMES that actor.
//
// This is the model.compile iterate-render-iterate loop: compile a mesh, place it in the
// level, recompile with overwrite:true. The asset registry sees nothing (an unsaved level
// reference is not on disk to index), so the branch above passes and ObjectTools refuses the
// delete instead. The message used to answer that with "Close any editor holding it", which
// is doubly wrong - DeleteSingleObject has already closed every asset editor for the object
// by then (UE 5.8 ObjectTools.cpp:3470) - and it never named the actor that was actually
// holding it. Two agents lost a cycle hunting for an asset editor that was not open.
// -----------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetCreatePolicyOverwriteNamesActorTest,
    "PinWright.assets.AssetCreatePolicy.OverwriteRejectionNamesTheReferencingLevelActor",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAssetCreatePolicyOverwriteNamesActorTest::RunTest(const FString& /*Parameters*/)
{
    FAssetCreatePolicyTestSuppressionFixture Suppression;

    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World)
    {
        AddError(TEXT("No editor world: the live-actor referencer case cannot be exercised."));
        return false;
    }

    const FString MeshPath = AssetCreatePolicyTest_UniquePackagePath(TEXT("SM_Probe"));
    // Declared BEFORE the world guard so it runs AFTER it: the actor must be destroyed
    // before the mesh it references is force-deleted.
    ON_SCOPE_EXIT { CleanupTestAsset(MeshPath); };
    FScopedEditorWorldActorGuard WorldGuard;

    UStaticMesh* Mesh = AssetCreatePolicyTest_MakeStaticMesh(MeshPath);
    if (!TestNotNull(TEXT("probe static mesh created"), Mesh)) return false;

    // Trailing 'X': FActorLabelUtilities strips a trailing NUMBER before uniquifying, so a
    // GUID ending in digits would come back incremented instead of intact.
    const FString ActorLabel = FString::Printf(TEXT("PWCreatePolicyHolder_%sX"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    AStaticMeshActor* Holder = AssetCreatePolicyTest_SpawnMeshHolder(World, Mesh, ActorLabel);
    if (!TestNotNull(TEXT("probe level actor spawned"), Holder)) return false;
    if (!TestTrue(TEXT("the probe actor's component actually holds the probe mesh"),
            Holder->GetStaticMeshComponent()->GetStaticMesh() == Mesh))
    {
        return false;
    }

    {
        FScopedUnattendedRpc UnattendedScope;
        TestTrue(TEXT("GIsRunningUnattendedScript held for the whole resolution"),
            GIsRunningUnattendedScript);

        const AssetCreatePolicy::FResolution Resolution = AssetCreatePolicy::Resolve(
            MeshPath, FPackageName::GetLongPackageAssetName(MeshPath),
            UStaticMesh::StaticClass(), /*bOverwriteRequested=*/true);

        TestTrue(TEXT("action is Rejected"), Resolution.IsRejected());
        TestEqual(TEXT("error code is still ASSET_IN_USE - no new code was invented"),
            Resolution.ErrorCode, FString(ErrorCodes::ERR_ASSET_IN_USE));

        // The registry branch cannot explain this rejection: nothing on disk references the
        // mesh. That is exactly why the old message left the caller with no lead to follow.
        TestEqual(TEXT("no on-disk package referencer was found"), Resolution.ReferencerCount, 0);

        TestTrue(TEXT("at least one live level actor is reported"),
            Resolution.ReferencingActorCount >= 1);
        bool bListNamesHolder = false;
        bool bListNamesClass = false;
        for (const FString& Entry : Resolution.ReferencingActors)
        {
            bListNamesHolder |= Entry.Contains(ActorLabel);
            bListNamesClass |= Entry.Contains(TEXT("StaticMeshActor"));
        }
        // The referencer the graph walk finds is the UStaticMeshComponent; it must be
        // reported as its OWNING ACTOR, which is what a caller can act on.
        TestTrue(TEXT("referencingActors names the spawned probe actor by label"), bListNamesHolder);
        TestTrue(TEXT("referencingActors carries the actor's class, not the component's"),
            bListNamesClass);

        TestTrue(TEXT("the error message names the actor holding the asset"),
            Resolution.ErrorMessage.Contains(ActorLabel));
        TestTrue(TEXT("the message leads with the drop-overwrite remedy"),
            Resolution.ErrorMessage.Contains(TEXT("Drop overwrite")));
        TestFalse(TEXT("the message no longer sends the caller hunting for an asset editor"),
            Resolution.ErrorMessage.Contains(TEXT("Close any editor holding it")));

        const TSharedPtr<FJsonObject> ErrorData = AssetCreatePolicy::MakeErrorData(Resolution);
        if (TestTrue(TEXT("rejection carries error data"), ErrorData.IsValid()))
        {
            const TArray<TSharedPtr<FJsonValue>>* ActorArray = nullptr;
            if (TestTrue(TEXT("error data carries a referencingActors array"),
                    ErrorData->TryGetArrayField(TEXT("referencingActors"), ActorArray))
                && ActorArray)
            {
                bool bPayloadNamesHolder = false;
                for (const TSharedPtr<FJsonValue>& Value : *ActorArray)
                {
                    bPayloadNamesHolder |= Value.IsValid() && Value->AsString().Contains(ActorLabel);
                }
                TestTrue(TEXT("the payload's referencingActors names the probe actor"),
                    bPayloadNamesHolder);
            }
            double ReportedActorCount = 0.0;
            ErrorData->TryGetNumberField(TEXT("referencingActorCount"), ReportedActorCount);
            TestTrue(TEXT("error data carries referencingActorCount >= 1"), ReportedActorCount >= 1.0);

            // The on-disk list stays a separate, still-present field so a caller can tell the
            // two kinds of referencer apart.
            const TArray<TSharedPtr<FJsonValue>>* PackageArray = nullptr;
            if (TestTrue(TEXT("error data still carries the package referencers array"),
                    ErrorData->TryGetArrayField(TEXT("referencers"), PackageArray))
                && PackageArray)
            {
                TestEqual(TEXT("the package referencer list is empty for this rejection"),
                    PackageArray->Num(), 0);
            }
        }
    }

    // DeleteSingleObject re-selects an object whose delete it refused. Drop it so no later
    // test inherits a stale content-browser selection.
    if (GEditor)
    {
        GEditor->GetSelectedObjects()->Deselect(Mesh);
    }

    // The refusal left everything exactly as it was: nothing deleted, nothing repointed.
    TestNotNull(TEXT("the referenced mesh still exists after the refused overwrite"),
        FindObject<UStaticMesh>(nullptr, *ToObjectPath(MeshPath)));
    TestTrue(TEXT("the probe actor still points at the same mesh"),
        Holder->GetStaticMeshComponent()->GetStaticMesh() == Mesh);
    return true;
}

// -----------------------------------------------------------------------------
// A different asset class occupies the path -> Rejected ASSET_ALREADY_EXISTS naming
// the class, whatever `overwrite` says. A create verb must never silently replace an
// asset of another type.
// -----------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetCreatePolicyClassMismatchTest,
    "PinWright.assets.AssetCreatePolicy.WrongClassRejectedEvenWithOverwrite",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAssetCreatePolicyClassMismatchTest::RunTest(const FString& /*Parameters*/)
{
    FAssetCreatePolicyTestSuppressionFixture Suppression;

    const FString TablePath = AssetCreatePolicyTest_UniquePackagePath(TEXT("DT_Squatter"));
    ON_SCOPE_EXIT { CleanupTestAsset(TablePath); };

    UPackage* Package = CreatePackage(*TablePath);
    if (!TestNotNull(TEXT("squatter package created"), Package)) return false;
    UDataTable* Table = NewObject<UDataTable>(
        Package, FName(*FPackageName::GetLongPackageAssetName(TablePath)),
        RF_Public | RF_Standalone);
    if (!TestNotNull(TEXT("squatter DataTable created"), Table)) return false;
    Table->RowStruct = FTableRowBase::StaticStruct();
    Table->MarkPackageDirty();
    FAssetRegistryModule::AssetCreated(Table);

    const FString AssetName = FPackageName::GetLongPackageAssetName(TablePath);

    FScopedUnattendedRpc UnattendedScope;
    TestTrue(TEXT("GIsRunningUnattendedScript held for the whole resolution"),
        GIsRunningUnattendedScript);

    for (const bool bOverwrite : {false, true})
    {
        const AssetCreatePolicy::FResolution Resolution = AssetCreatePolicy::Resolve(
            TablePath, AssetName, UMaterial::StaticClass(), bOverwrite);

        TestTrue(*FString::Printf(TEXT("action is Rejected (overwrite=%s)"),
                     bOverwrite ? TEXT("true") : TEXT("false")),
            Resolution.IsRejected());
        TestEqual(*FString::Printf(TEXT("error code is ASSET_ALREADY_EXISTS (overwrite=%s)"),
                      bOverwrite ? TEXT("true") : TEXT("false")),
            Resolution.ErrorCode, FString(ErrorCodes::ERR_ASSET_ALREADY_EXISTS));
        TestTrue(TEXT("the message names the class actually occupying the path"),
            Resolution.ErrorMessage.Contains(TEXT("DataTable")));
        TestTrue(TEXT("the squatting asset is never handed back as an update target"),
            Resolution.Existing == nullptr);
    }

    // Nothing was deleted on the overwrite pass either.
    TestNotNull(TEXT("the squatting DataTable still exists"),
        FindObject<UDataTable>(nullptr, *ToObjectPath(TablePath)));
    return true;
}

// -----------------------------------------------------------------------------
// End to end through the dispatcher: material.authoring.create_material is idempotent
// and reports the additive existing / mode fields on the wire.
// -----------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetCreatePolicyCreateMaterialIdempotentTest,
    "PinWright.assets.AssetCreatePolicy.CreateMaterialIsIdempotent",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAssetCreatePolicyCreateMaterialIdempotentTest::RunTest(const FString& /*Parameters*/)
{
    FAssetCreatePolicyTestSuppressionFixture Suppression;

    const FString Folder = TEXT("/Game/PinWrightTests/AssetCreatePolicy");
    const FString AssetName = FString::Printf(TEXT("M_Idempotent_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    const FString PackagePath = FString::Printf(TEXT("%s/%s"), *Folder, *AssetName);
    ON_SCOPE_EXIT { CleanupTestAsset(PackagePath); };

    FRpcDispatcher Dispatcher;
    DispatcherTestHelpers::FSinkPtr Sink;
    DispatcherTestHelpers::MakeDispatcher(Sink, Dispatcher);

    auto MakeParams = [](const FString& InName, const FString& InFolder)
    {
        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("name"), InName);
        Params->SetStringField(TEXT("path"), InFolder);
        // save:false keeps the fixture in memory; the policy resolves in-memory objects
        // too, which is exactly the case the old DoesAssetExist pre-check missed.
        Params->SetBoolField(TEXT("save"), false);
        return Params;
    };

    bool bSuccess = false;
    FString ErrorCode;
    TSharedPtr<FJsonObject> FirstResult;
    DispatcherTestHelpers::Dispatch(Dispatcher, Sink,
        TEXT("material.authoring.create_material"), TEXT("req-create-material-1"),
        MakeParams(AssetName, Folder), bSuccess, FirstResult, ErrorCode);

    TestTrue(FString::Printf(TEXT("first create succeeded (err='%s')"), *ErrorCode), bSuccess);
    if (!bSuccess || !FirstResult.IsValid()) return false;

    bool bFirstExisting = true;
    FirstResult->TryGetBoolField(TEXT("existing"), bFirstExisting);
    FString FirstMode;
    FirstResult->TryGetStringField(TEXT("mode"), FirstMode);
    FString FirstAssetPath;
    FirstResult->TryGetStringField(TEXT("assetPath"), FirstAssetPath);
    TestFalse(TEXT("first create reports existing:false"), bFirstExisting);
    TestEqual(TEXT("first create reports mode:created"), FirstMode, FString(TEXT("created")));

    bool bSecondSuccess = false;
    FString SecondErrorCode;
    TSharedPtr<FJsonObject> SecondResult;
    DispatcherTestHelpers::Dispatch(Dispatcher, Sink,
        TEXT("material.authoring.create_material"), TEXT("req-create-material-2"),
        MakeParams(AssetName, Folder), bSecondSuccess, SecondResult, SecondErrorCode);

    // Pre-fix this second call reached CanCreateAsset and raised three modals; the
    // contract now is a clean idempotent success.
    TestTrue(FString::Printf(TEXT("second create succeeded (err='%s')"), *SecondErrorCode),
        bSecondSuccess);
    if (!bSecondSuccess || !SecondResult.IsValid()) return false;

    bool bSecondExisting = false;
    SecondResult->TryGetBoolField(TEXT("existing"), bSecondExisting);
    FString SecondMode;
    SecondResult->TryGetStringField(TEXT("mode"), SecondMode);
    FString SecondAssetPath;
    SecondResult->TryGetStringField(TEXT("assetPath"), SecondAssetPath);
    TestTrue(TEXT("second create reports existing:true"), bSecondExisting);
    TestEqual(TEXT("second create reports mode:updated_in_place"),
        SecondMode, FString(TEXT("updated_in_place")));
    TestEqual(TEXT("both calls report the same assetPath"), SecondAssetPath, FirstAssetPath);

    return true;
}
