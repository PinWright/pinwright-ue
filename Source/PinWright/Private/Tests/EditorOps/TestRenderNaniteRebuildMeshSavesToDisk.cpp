// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression coverage for B-nanite-rebuild-mesh-no-disk-write. The real registered
// render.nanite_rebuild_mesh job must place persistence evidence on its terminal result,
// while save:false must leave an existing .uasset byte-identical and report its stale size.

#include "Misc/AutomationTest.h"

#include "Async/TaskGraphInterfaces.h"
#include "Components/StaticMeshComponent.h"
#include "Containers/Ticker.h"
#include "Dom/JsonObject.h"
#include "Editor.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "StaticMeshCompiler.h"
#include "Dispatch/RpcDispatcher.h"
#include "State/JobRegistry.h"
#include "State/PluginState.h"
#include "Tests/Assets/NaniteRebuildSaveTestUtils.h"
#include "Tests/Infra/ParamSpecTestHelpers.h"
#include "Tests/TestSkipReporting.h"
#include "Tests/TestUtils.h"
#include "Tests/TestWorldUtils.h"
#include "Utils/AssetCompilePump.h"

namespace RenderNaniteRebuildMeshSaveTests
{
    AStaticMeshActor* SpawnRenderNaniteConsumer(UWorld* World, UStaticMesh* Mesh)
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
            Actor->SetActorLabel(FString::Printf(TEXT("PW_RenderNaniteConsumer_%s"),
                *FGuid::NewGuid().ToString(EGuidFormats::Digits)));
            Actor->GetStaticMeshComponent()->SetStaticMesh(Mesh);
        }
        return Actor;
    }

    bool InvokeForTicket(
        FAutomationTestBase& Test,
        const TSharedPtr<FJsonObject>& Payload,
        FString& OutTicketId)
    {
        FTestResponseCapture Capture;
        const bool bFound = InvokeHandlerWithCapture(
            TEXT("render.nanite_rebuild_mesh"), Payload, Capture);
        Test.TestTrue(TEXT("render.nanite_rebuild_mesh handler is registered"), bFound);
        Test.TestTrue(TEXT("render.nanite_rebuild_mesh kickoff responded"), Capture.bWasCalled);
        Test.TestTrue(TEXT("render.nanite_rebuild_mesh kickoff succeeded"), Capture.bSuccess);
        Test.TestTrue(TEXT("render.nanite_rebuild_mesh kickoff returned a result"),
            Capture.Result.IsValid());
        if (!bFound || !Capture.bWasCalled || !Capture.bSuccess || !Capture.Result.IsValid())
        {
            return false;
        }

        Test.TestFalse(TEXT("persistence fields are deferred to the terminal job result"),
            Capture.Result->HasField(TEXT("saveRequested")));

        Test.TestTrue(TEXT("render.nanite_rebuild_mesh kickoff carries ticket_id"),
            Capture.Result->TryGetStringField(TEXT("ticket_id"), OutTicketId)
                && !OutTicketId.IsEmpty());
        return !OutTicketId.IsEmpty();
    }

    bool InvokeAndWaitForTerminal(
        FAutomationTestBase& Test,
        const TSharedPtr<FJsonObject>& Payload,
        UStaticMesh* FixtureMesh,
        FJobTicket& OutTicket)
    {
        FString TicketId;
        if (!InvokeForTicket(Test, Payload, TicketId))
        {
            return false;
        }

        const double Deadline = FPlatformTime::Seconds() + 15.0;
        while (true)
        {
            FJobTicket Ticket;
            if (!FPluginState::Get().GetJobRegistry().Get(TicketId, Ticket))
            {
                Test.AddError(TEXT("render.nanite_rebuild_mesh ticket disappeared before completion."));
                return false;
            }
            if (Ticket.Status != TEXT("running"))
            {
                OutTicket = MoveTemp(Ticket);
                return true;
            }
            if (FPlatformTime::Seconds() > Deadline)
            {
                const EJobCancelResult CancelResult =
                    FPluginState::Get().GetJobRegistry().Cancel(TicketId);
                Test.TestTrue(TEXT("timed-out render job accepted cancellation"),
                    CancelResult == EJobCancelResult::Requested);
                if (FixtureMesh)
                {
                    FStaticMeshCompilingManager::Get().FinishCompilation({FixtureMesh});
                }
                Test.AddError(TEXT("render.nanite_rebuild_mesh did not finish within 15 seconds."));
                return false;
            }

            PinWright::AssetCompile::AdvanceOnGameThread();
            FTSTicker::GetCoreTicker().Tick(0.01f);
            FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);
            FPlatformProcess::Sleep(0.001f);
        }
    }

    bool TestSafePointContinuationSourceContract(FAutomationTestBase& Test)
    {
        const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("PinWright"));
        Test.TestTrue(TEXT("PinWright plugin is discoverable for source contract checks"),
            Plugin.IsValid());
        if (!Plugin.IsValid())
        {
            return false;
        }

        const FString SourcePath = Plugin->GetBaseDir()
            / TEXT("Source/PinWright/Private/Handlers/Render/RenderHandler.cpp");
        FString Source;
        Test.TestTrue(TEXT("RenderHandler.cpp loads for source contract checks"),
            FFileHelper::LoadFileToString(Source, *SourcePath));
        if (Source.IsEmpty())
        {
            return false;
        }

        const int32 NaniteStart = Source.Find(TEXT("// ---- render.nanite_rebuild_mesh ----"));
        const int32 NaniteEnd = Source.Find(
            TEXT("// ---- render.lumen_update_scene ----"), ESearchCase::CaseSensitive,
            ESearchDir::FromStart, NaniteStart);
        Test.TestTrue(TEXT("render.nanite_rebuild_mesh source region is bounded"),
            NaniteStart != INDEX_NONE && NaniteEnd > NaniteStart);
        if (NaniteStart == INDEX_NONE || NaniteEnd <= NaniteStart)
        {
            return false;
        }
        const FString NaniteSource = Source.Mid(NaniteStart, NaniteEnd - NaniteStart);

        const int32 SafePointStart = NaniteSource.Find(
            TEXT("DeferJobToSafePoint(Ctx, TEXT(\"render.nanite_rebuild_mesh\")"));
        const int32 BuildStart = NaniteSource.Find(TEXT("State->Start(MoveTemp(OnComplete))"));
        Test.TestTrue(TEXT("the Nanite job reserves a retained safe-point continuation"),
            SafePointStart != INDEX_NONE);
        Test.TestTrue(TEXT("the retained continuation owns the complete job start"),
            SafePointStart != INDEX_NONE && BuildStart > SafePointStart);
        Test.TestTrue(TEXT("the Nanite rebuild quiesces live render consumers"),
            NaniteSource.Contains(TEXT("PinWrightMeshRebuild::FQuiesceScope")));
        Test.TestTrue(TEXT("the compile wait advances through the bounded shared pump"),
            NaniteSource.Contains(TEXT("PinWright::AssetCompile::AdvanceOnGameThread()")));
        Test.TestTrue(TEXT("the compile wait rechecks its deadline after compilation clears"),
            NaniteSource.Contains(
                TEXT("return FPlatformTime::Seconds() < DeadlineSeconds;")));
        Test.TestTrue(TEXT("an invalid retained mesh resolves through the job completion path"),
            NaniteSource.Contains(TEXT("ResolveInvalidMesh();"))
                && NaniteSource.Contains(TEXT("ErrorCodes::ERR_INVALID_ASSET")));
        const int32 RetainedGuardStart = NaniteSource.Find(
            TEXT("class FRetainedCompileGuard"));
        const int32 StateStart = NaniteSource.Find(TEXT("class FState"));
        Test.TestTrue(TEXT("the post-timeout retained guard source region is bounded"),
            RetainedGuardStart != INDEX_NONE && StateStart > RetainedGuardStart);
        if (RetainedGuardStart != INDEX_NONE && StateStart > RetainedGuardStart)
        {
            const FString RetainedGuardSource = NaniteSource.Mid(
                RetainedGuardStart, StateStart - RetainedGuardStart);
            Test.TestFalse(TEXT("the post-timeout retained guard never pumps compilation"),
                RetainedGuardSource.Contains(
                    TEXT("PinWright::AssetCompile::AdvanceOnGameThread()")));
        }
        Test.TestFalse(TEXT("the Nanite job cannot escape through a raw GameThread marshal"),
            NaniteSource.Contains(TEXT("AsyncTask(ENamedThreads::GameThread")));
        Test.TestFalse(TEXT("the Nanite job has no unbounded StaticMesh compilation drain"),
            NaniteSource.Contains(TEXT("FinishCompilation("))
                || NaniteSource.Contains(TEXT("FinishCompilationForObjects(")));

        const FString SafePointPath = Plugin->GetBaseDir()
            / TEXT("Source/PinWright/Private/Dispatch/SafePoint.cpp");
        FString SafePointSource;
        Test.TestTrue(TEXT("SafePoint.cpp loads for source contract checks"),
            FFileHelper::LoadFileToString(SafePointSource, *SafePointPath));
        Test.TestTrue(TEXT("render.nanite_rebuild_mesh remains in the tick-unsafe table"),
            SafePointSource.Contains(TEXT("TEXT(\"render.nanite_rebuild_mesh\")")));
        return true;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRenderNaniteRebuildSafePointContinuationTest,
    "PinWright.render.nanite_rebuild_mesh.SafePointContinuation",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FRenderNaniteRebuildSafePointContinuationTest::RunTest(const FString& /*Parameters*/)
{
    using namespace NaniteRebuildSaveTestUtils;
    using namespace RenderNaniteRebuildMeshSaveTests;

    if (!RenderNaniteRebuildMeshSaveTests::TestSafePointContinuationSourceContract(*this))
    {
        return false;
    }

    TestTrue(TEXT("render Nanite accepts a bounded timeout"),
        ParamSpecTestHelpers::IsParamAccepted(
            TEXT("render.nanite_rebuild_mesh"), TEXT("timeoutSeconds")));
    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("No editor world is available for a live StaticMesh consumer fixture."));
        return true;
    }

    FScopedFixture Fixture(TEXT("Render"), TEXT("SafePointTimeout"));
    FScopedEditorWorldActorGuard WorldGuard;
    AStaticMeshActor* ConsumerActor = SpawnRenderNaniteConsumer(World, Fixture.Mesh);
    UStaticMeshComponent* Consumer = ConsumerActor
        ? ConsumerActor->GetStaticMeshComponent()
        : nullptr;
    if (!Fixture.Mesh || !Consumer || !Consumer->IsRenderStateCreated())
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("nanite-consumer-unavailable"),
            TEXT("The host could not create a render-state-bearing StaticMesh consumer."));
        return true;
    }

    FRpcHandlerFunc RenderHandler = nullptr;
    for (const FHandlerRegistration& Registration : FAutoRegisterHandler::GetPendingRegistrations())
    {
        if (Registration.MethodName == TEXT("render.nanite_rebuild_mesh"))
        {
            RenderHandler = Registration.Func;
            break;
        }
    }
    TestTrue(TEXT("render.nanite_rebuild_mesh production handler is registered"), RenderHandler != nullptr);
    if (!RenderHandler)
    {
        return false;
    }

    FRpcDispatcher Dispatcher;
    TSharedRef<FTestResponseCapture> Capture = MakeShared<FTestResponseCapture>();
    bool bQueuedRequestRan = false;
    Dispatcher.RegisterHandler(TEXT("test.render_nanite_wrapper"),
        [&Dispatcher, Capture, RenderHandler](
            const FString& RequestId, const FString&,
            const TSharedPtr<FJsonObject>& Payload) -> bool
        {
            FHandlerContext Ctx = FHandlerContext::MakeTestContextWithSharedCapture(
                RequestId, TEXT("render.nanite_rebuild_mesh"), Payload, Capture);
            Ctx.SetDispatcherForTesting(&Dispatcher);
            return RenderHandler(Ctx);
        });
    Dispatcher.RegisterHandler(TEXT("test.queued_after_nanite"),
        [&bQueuedRequestRan](
            const FString&, const FString&, const TSharedPtr<FJsonObject>&) -> bool
        {
            bQueuedRequestRan = true;
            return true;
        });

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), Fixture.Mesh->GetPathName());
    Payload->SetNumberField(TEXT("timeoutSeconds"), 0.01);

    {
        PinWright::AssetCompile::FScopedCompilePendingOverride PendingOverride(true);
        Dispatcher.ProcessRequest(
            TEXT("nanite-safe-point-id"), TEXT("test.render_nanite_wrapper"), Payload);

        FString TicketId;
        TestTrue(TEXT("Nanite job kickoff returns a ticket before deferred work"),
            Capture->bWasCalled && Capture->bSuccess && Capture->Result.IsValid()
                && Capture->Result->TryGetStringField(TEXT("ticket_id"), TicketId)
                && !TicketId.IsEmpty());
        TestTrue(TEXT("Nanite job retains the active request before its safe-point body"),
            Dispatcher.IsProcessingRequestForTesting());

        Dispatcher.ProcessRequest(
            TEXT("queued-after-nanite-id"), TEXT("test.queued_after_nanite"),
            MakeShared<FJsonObject>());
        Dispatcher.ProcessPendingRequests();
        TestFalse(TEXT("queued RPC cannot interleave before the Nanite job body"),
            bQueuedRequestRan);

        FTSTicker::GetCoreTicker().Tick(0.0f);

        // The override forces the bounded-wait timeout but does not keep the engine's real mesh
        // compilation alive. Require quiescence exactly while that real compilation is active;
        // a tiny fixture may already be terminal (and legitimately restored) before this point.
        const bool bCompilationStillRunning = Fixture.Mesh->IsCompiling();
        TestTrue(TEXT("timed-out render Nanite keeps its consumer quiesced until compilation terminates"),
            !bCompilationStillRunning || !Consumer->IsRenderStateCreated());
        TestFalse(TEXT("Nanite request scope closes after its safe-point body"),
            Dispatcher.IsProcessingRequestForTesting());
        TestTrue(TEXT("queued RPC runs only after the Nanite job body exits"),
            bQueuedRequestRan);
        FJobTicket Ticket;
        TestTrue(TEXT("failed Nanite job remains queryable"),
            FPluginState::Get().GetJobRegistry().Get(TicketId, Ticket));
        TestEqual(TEXT("timed-out fixture fails after the retained continuation runs"),
            Ticket.Status, FString(TEXT("failed")));
        TestTrue(TEXT("timed-out render Nanite job carries result details"),
            Ticket.Result.IsValid());
        if (Ticket.Result.IsValid())
        {
            bool bTimedOut = false;
            TestTrue(TEXT("render Nanite job reports timedOut:true"),
                Ticket.Result->TryGetBoolField(TEXT("timedOut"), bTimedOut) && bTimedOut);
            bool bSaveRequested = true;
            TestTrue(TEXT("render Nanite timeout reports no save attempt"),
                Ticket.Result->TryGetBoolField(TEXT("saveRequested"), bSaveRequested)
                    && !bSaveRequested);
        }
        TestTrue(TEXT("render Nanite timeout writes no package"),
            IFileManager::Get().FileSize(*PackageFilenameFromAssetPath(Fixture.PackagePath)) < 0);
    }
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
    FTSTicker::GetCoreTicker().Tick(0.1f);
    TestTrue(TEXT("render Nanite restores the consumer after compilation terminates"),
        Consumer->IsRenderStateCreated());
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRenderNaniteRebuildMeshSavesToDiskTest,
    "PinWright.render.nanite_rebuild_mesh.SavesToDisk",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FRenderNaniteRebuildMeshSavesToDiskTest::RunTest(const FString& Parameters)
{
    using namespace RenderNaniteRebuildMeshSaveTests;
    using namespace NaniteRebuildSaveTestUtils;

    TestTrue(TEXT("save is a declared parameter"),
        ParamSpecTestHelpers::IsParamAccepted(TEXT("render.nanite_rebuild_mesh"), TEXT("save")));
    FScopedFixture SavedFixture(TEXT("Render"), TEXT("Default"));
    TestNotNull(TEXT("default-save fixture StaticMesh created"), SavedFixture.Mesh);
    if (!SavedFixture.Mesh)
    {
        return false;
    }

    TSharedPtr<FJsonObject> SavedPayload = MakeShared<FJsonObject>();
    SavedPayload->SetStringField(TEXT("assetPath"), SavedFixture.Mesh->GetPathName());

    FJobTicket SavedTicket;
    if (!InvokeAndWaitForTerminal(*this, SavedPayload, SavedFixture.Mesh, SavedTicket))
    {
        return false;
    }
    TestEqual(TEXT("default-save job completed"), SavedTicket.Status, FString(TEXT("completed")));
    TestTrue(TEXT("default-save terminal result is present"), SavedTicket.Result.IsValid());
    if (SavedTicket.Status != TEXT("completed") || !SavedTicket.Result.IsValid())
    {
        return false;
    }

    bool bRebuilt = false;
    TestTrue(TEXT("default-save terminal result reports rebuilt:true"),
        ReadBool(SavedTicket.Result, TEXT("rebuilt"), bRebuilt) && bRebuilt);
    TestDurablePersistenceResult(*this, SavedTicket.Result, SavedFixture);

    FScopedFixture OptOutFixture(TEXT("Render"), TEXT("OptOut"));
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
    OptOutPayload->SetStringField(TEXT("assetPath"), OptOutFixture.Mesh->GetPathName());
    OptOutPayload->SetBoolField(TEXT("save"), false);

    FJobTicket OptOutTicket;
    if (!InvokeAndWaitForTerminal(*this, OptOutPayload, OptOutFixture.Mesh, OptOutTicket))
    {
        return false;
    }
    TestEqual(TEXT("save:false job completed"), OptOutTicket.Status, FString(TEXT("completed")));
    TestTrue(TEXT("save:false terminal result is present"), OptOutTicket.Result.IsValid());
    if (OptOutTicket.Status != TEXT("completed") || !OptOutTicket.Result.IsValid())
    {
        return false;
    }

    bRebuilt = false;
    TestTrue(TEXT("save:false terminal result reports rebuilt:true"),
        ReadBool(OptOutTicket.Result, TEXT("rebuilt"), bRebuilt) && bRebuilt);
    TestOptOutPersistenceResult(*this, OptOutTicket.Result, OptOutFixture, OptOutBaseline);

    FScopedFixture CancelFixture(TEXT("Render"), TEXT("Cancel"));
    TestNotNull(TEXT("cancellation fixture StaticMesh created"), CancelFixture.Mesh);
    if (!CancelFixture.Mesh)
    {
        return false;
    }
    if (CancelFixture.Mesh->IsCompiling())
    {
        FStaticMeshCompilingManager::Get().FinishCompilation({CancelFixture.Mesh});
    }

    TSharedPtr<FJsonObject> CancelPayload = MakeShared<FJsonObject>();
    CancelPayload->SetStringField(TEXT("assetPath"), CancelFixture.Mesh->GetPathName());
    FString CancelTicketId;
    if (!InvokeForTicket(*this, CancelPayload, CancelTicketId))
    {
        return false;
    }

    TSharedPtr<FJsonObject> JobCancelPayload = MakeShared<FJsonObject>();
    JobCancelPayload->SetStringField(TEXT("ticket_id"), CancelTicketId);
    FTestResponseCapture CancelCapture;
    const bool bCancelFound = InvokeHandlerWithCapture(
        TEXT("system.job_cancel"), JobCancelPayload, CancelCapture);
    TestTrue(TEXT("system.job_cancel handler is registered"), bCancelFound);
    TestTrue(TEXT("render Nanite cancellation responded"), CancelCapture.bWasCalled);
    TestTrue(TEXT("render Nanite cancellation succeeded"), CancelCapture.bSuccess);
    TestTrue(TEXT("render Nanite cancellation returned a result"), CancelCapture.Result.IsValid());
    if (!bCancelFound || !CancelCapture.bWasCalled || !CancelCapture.bSuccess
        || !CancelCapture.Result.IsValid())
    {
        return false;
    }

    bool bCancelled = false;
    TestTrue(TEXT("cancel response reports the PinWright job cancelled"),
        ReadBool(CancelCapture.Result, TEXT("cancelled"), bCancelled) && bCancelled);
    TestEqual(TEXT("cancel response reports a requested cancellation"),
        ReadString(CancelCapture.Result, TEXT("cancellation")), FString(TEXT("requested")));

    FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);
    PinWright::AssetCompile::AdvanceOnGameThread();
    FTSTicker::GetCoreTicker().Tick(0.01f);

    FJobTicket CancelledTicket;
    TestTrue(TEXT("cancelled render Nanite ticket remains queryable"),
        FPluginState::Get().GetJobRegistry().Get(CancelTicketId, CancelledTicket));
    TestEqual(TEXT("render Nanite ticket reaches cancelled terminal state"),
        CancelledTicket.Status, FString(TEXT("cancelled")));
    TestFalse(TEXT("render Nanite cancellation leaves no mesh compilation alive"),
        CancelFixture.Mesh->IsCompiling());
    TestTrue(TEXT("render Nanite cancellation writes no .uasset"),
        IFileManager::Get().FileSize(
            *PackageFilenameFromAssetPath(CancelFixture.PackagePath)) < 0);

    return true;
}
