// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression coverage for B-nanite-rebuild-mesh-no-disk-write. The real registered
// asset.nanite_rebuild_mesh handler must persist by default. With save:false it must
// leave an existing .uasset byte-identical while reporting that stale disk revision.

#include "Misc/AutomationTest.h"

#include "Components/StaticMeshComponent.h"
#include "Containers/Ticker.h"
#include "Dom/JsonObject.h"
#include "Editor.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "Handlers/ErrorCodes.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Misc/Guid.h"
#include "StaticMeshCompiler.h"
#include "Tests/Assets/NaniteRebuildSaveTestUtils.h"
#include "Tests/Infra/ParamSpecTestHelpers.h"
#include "Tests/TestSkipReporting.h"
#include "Tests/TestUtils.h"
#include "Tests/TestWorldUtils.h"
#include "Utils/AssetCompilePump.h"

namespace
{
    AStaticMeshActor* SpawnNaniteConsumer(UWorld* World, UStaticMesh* Mesh)
    {
        if (!World || !Mesh)
        {
            return nullptr;
        }

        FActorSpawnParameters SpawnParams;
        SpawnParams.ObjectFlags = RF_Transient;
        AStaticMeshActor* Actor = World->SpawnActor<AStaticMeshActor>(
            AStaticMeshActor::StaticClass(), FVector::ZeroVector,
            FRotator::ZeroRotator, SpawnParams);
        if (Actor)
        {
            Actor->SetActorLabel(FString::Printf(TEXT("PW_NaniteConsumer_%s"),
                *FGuid::NewGuid().ToString(EGuidFormats::Digits)));
            Actor->GetStaticMeshComponent()->SetStaticMesh(Mesh);
        }
        return Actor;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNaniteRebuildMeshBoundedCompileWaitTest,
    "PinWright.asset.nanite_rebuild_mesh.BoundedCompileWait",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNaniteRebuildMeshBoundedCompileWaitTest::RunTest(const FString& /*Parameters*/)
{
    using namespace NaniteRebuildSaveTestUtils;

    TestTrue(TEXT("asset Nanite accepts a bounded timeout"),
        ParamSpecTestHelpers::IsParamAccepted(
            TEXT("asset.nanite_rebuild_mesh"), TEXT("timeoutSeconds")));

    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("No editor world is available for a live StaticMesh consumer fixture."));
        return true;
    }

    FScopedFixture Fixture(TEXT("Asset"), TEXT("Timeout"));
    FScopedEditorWorldActorGuard WorldGuard;
    AStaticMeshActor* ConsumerActor = SpawnNaniteConsumer(World, Fixture.Mesh);
    UStaticMeshComponent* Consumer = ConsumerActor
        ? ConsumerActor->GetStaticMeshComponent()
        : nullptr;
    if (!Fixture.Mesh || !Consumer || !Consumer->IsRenderStateCreated())
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("nanite-consumer-unavailable"),
            TEXT("The host could not create a render-state-bearing StaticMesh consumer."));
        return true;
    }

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("meshPath"), Fixture.Mesh->GetPathName());
    Payload->SetNumberField(TEXT("timeoutSeconds"), 0.01);

    FTestResponseCapture Capture;
    {
        PinWright::AssetCompile::FScopedCompilePendingOverride PendingOverride(true);
        TestTrue(TEXT("asset.nanite_rebuild_mesh timeout handler is registered"),
            InvokeHandlerWithCapture(
                TEXT("asset.nanite_rebuild_mesh"), Payload, Capture));
        // The override forces the bounded-wait timeout but does not keep the engine's real mesh
        // compilation alive. Require quiescence exactly while that real compilation is active;
        // a tiny fixture may already be terminal (and legitimately restored) before this point.
        const bool bCompilationStillRunning = Fixture.Mesh->IsCompiling();
        TestTrue(TEXT("timed-out Nanite rebuild keeps its consumer quiesced until compilation terminates"),
            !bCompilationStillRunning || !Consumer->IsRenderStateCreated());
    }

    TestTrue(TEXT("timed-out asset Nanite call responded"), Capture.bWasCalled);
    TestFalse(TEXT("timed-out asset Nanite call is not successful"), Capture.bSuccess);
    TestEqual(TEXT("timed-out asset Nanite call uses a typed error"),
        Capture.ErrorCode, FString(ErrorCodes::ERR_OPERATION_FAILED));
    TestTrue(TEXT("timed-out asset Nanite call returned details"), Capture.Result.IsValid());
    if (Capture.Result.IsValid())
    {
        bool bTimedOut = false;
        TestTrue(TEXT("asset Nanite reports timedOut:true"),
            Capture.Result->TryGetBoolField(TEXT("timedOut"), bTimedOut) && bTimedOut);
        bool bSaveRequested = true;
        TestTrue(TEXT("asset Nanite timeout reports no save attempt"),
            Capture.Result->TryGetBoolField(TEXT("saveRequested"), bSaveRequested)
                && !bSaveRequested);
        bool bSaved = true;
        TestTrue(TEXT("asset Nanite timeout reports saved:false"),
            Capture.Result->TryGetBoolField(TEXT("saved"), bSaved) && !bSaved);
    }
    TestTrue(TEXT("asset Nanite timeout writes no package"),
        IFileManager::Get().FileSize(*PackageFilenameFromAssetPath(Fixture.PackagePath)) < 0);

    const double SettleDeadline = FPlatformTime::Seconds() + 10.0;
    while (Fixture.Mesh->IsCompiling() && FPlatformTime::Seconds() < SettleDeadline)
    {
        PinWright::AssetCompile::AdvanceOnGameThread();
        FPlatformProcess::SleepNoStats(0.001f);
    }
    if (Fixture.Mesh->IsCompiling())
    {
        FStaticMeshCompilingManager::Get().FinishCompilation({Fixture.Mesh});
    }
    FTSTicker::GetCoreTicker().Tick(0.02f);
    TestTrue(TEXT("asset Nanite restores the consumer after compilation terminates"),
        Consumer->IsRenderStateCreated());
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNaniteRebuildMeshSavesToDiskTest,
    "PinWright.asset.nanite_rebuild_mesh.SavesToDisk",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNaniteRebuildMeshSavesToDiskTest::RunTest(const FString& Parameters)
{
    using namespace NaniteRebuildSaveTestUtils;

    TestTrue(TEXT("save is a declared parameter"),
        ParamSpecTestHelpers::IsParamAccepted(TEXT("asset.nanite_rebuild_mesh"), TEXT("save")));

    FScopedFixture SavedFixture(TEXT("Asset"), TEXT("Default"));
    TestNotNull(TEXT("default-save fixture StaticMesh created"), SavedFixture.Mesh);
    if (!SavedFixture.Mesh)
    {
        return false;
    }

    TSharedPtr<FJsonObject> SavedPayload = MakeShared<FJsonObject>();
    SavedPayload->SetStringField(TEXT("meshPath"), SavedFixture.Mesh->GetPathName());

    FTestResponseCapture SavedCapture;
    TestTrue(TEXT("asset.nanite_rebuild_mesh handler is registered"),
        InvokeHandlerWithCapture(TEXT("asset.nanite_rebuild_mesh"), SavedPayload, SavedCapture));
    TestTrue(TEXT("default-save call responded"), SavedCapture.bWasCalled);
    TestTrue(*FString::Printf(TEXT("default-save call succeeded (errorCode='%s')"),
        *SavedCapture.ErrorCode), SavedCapture.bSuccess);
    TestTrue(TEXT("default-save call returned a result"), SavedCapture.Result.IsValid());
    if (!SavedCapture.bSuccess || !SavedCapture.Result.IsValid())
    {
        return false;
    }

    bool bNaniteEnabled = false;
    TestTrue(TEXT("default-save result carries naniteEnabled:true"),
        ReadBool(SavedCapture.Result, TEXT("naniteEnabled"), bNaniteEnabled) && bNaniteEnabled);
    TestDurablePersistenceResult(*this, SavedCapture.Result, SavedFixture);

    FScopedFixture OptOutFixture(TEXT("Asset"), TEXT("OptOut"));
    TestNotNull(TEXT("save:false fixture StaticMesh created"), OptOutFixture.Mesh);
    if (!OptOutFixture.Mesh)
    {
        return false;
    }
    FPackageFileBaseline OptOutBaseline;
    if (!SaveFixtureBaseline(*this, OptOutFixture, OptOutBaseline))
    {
        return false;
    }

    TSharedPtr<FJsonObject> OptOutPayload = MakeShared<FJsonObject>();
    OptOutPayload->SetStringField(TEXT("meshPath"), OptOutFixture.Mesh->GetPathName());
    OptOutPayload->SetBoolField(TEXT("save"), false);

    FTestResponseCapture OptOutCapture;
    TestTrue(TEXT("asset.nanite_rebuild_mesh handler is registered for save:false"),
        InvokeHandlerWithCapture(TEXT("asset.nanite_rebuild_mesh"), OptOutPayload, OptOutCapture));
    TestTrue(TEXT("save:false call responded"), OptOutCapture.bWasCalled);
    TestTrue(*FString::Printf(TEXT("save:false call succeeded (errorCode='%s')"),
        *OptOutCapture.ErrorCode), OptOutCapture.bSuccess);
    TestTrue(TEXT("save:false call returned a result"), OptOutCapture.Result.IsValid());
    if (!OptOutCapture.bSuccess || !OptOutCapture.Result.IsValid())
    {
        return false;
    }

    bNaniteEnabled = false;
    TestTrue(TEXT("save:false result carries naniteEnabled:true"),
        ReadBool(OptOutCapture.Result, TEXT("naniteEnabled"), bNaniteEnabled) && bNaniteEnabled);
    TestOptOutPersistenceResult(*this, OptOutCapture.Result, OptOutFixture, OptOutBaseline);

    return true;
}
