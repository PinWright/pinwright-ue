// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression coverage for both asset.generate_lods defects:
// - the default save must write the generated LOD chain and expose the standard save-state shape;
// - a StaticMesh build must reach a terminal state before a row is counted as completed.
//
// Counterfactuals:
// - Restoring McpSafeAssetSave leaves no durable package and fails the saved-to-disk assertions.
// - Moving Batch.MarkSucceeded before the bounded compile wait/save gate fails the source-order
//   ratchet and allows a compiling row to be reported as completed.

#include "Misc/AutomationTest.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/StaticMesh.h"
#include "HAL/FileManager.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Tests/Assets/NaniteRebuildSaveTestUtils.h"
#include "Tests/Infra/ParamSpecTestHelpers.h"
#include "Tests/TestUtils.h"
#include "Utils/AssetCompilePump.h"

namespace GenerateLodsPersistenceAndCompileTests
{
    bool LoadGenerateLodsSource(FAutomationTestBase& Test, FString& OutBlock)
    {
        const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("PinWright"));
        if (!Test.TestTrue(TEXT("PinWright plugin is available for source contract checks"),
                Plugin.IsValid()))
        {
            return false;
        }

        FString Source;
        const FString SourcePath = Plugin->GetBaseDir()
            / TEXT("Source/PinWright/Private/Handlers/Asset/AssetWorkflowHandler.cpp");
        if (!Test.TestTrue(TEXT("AssetWorkflowHandler.cpp loads for source contract checks"),
                FFileHelper::LoadFileToString(Source, *SourcePath)))
        {
            return false;
        }

        Source = NeutralizeSourceText(Source);
        const FString Registration = TEXT("REGISTER_RPC_HANDLER(\"asset.generate_lods\"");
        const int32 Start = Source.Find(Registration);
        int32 End = Start == INDEX_NONE
            ? INDEX_NONE
            : Source.Find(TEXT("REGISTER_RPC_HANDLER("), ESearchCase::CaseSensitive,
                ESearchDir::FromStart, Start + Registration.Len());
        if (End == INDEX_NONE && Start != INDEX_NONE)
        {
            End = Source.Len();
        }

        if (!Test.TestTrue(TEXT("asset.generate_lods source region is bounded"),
                Start != INDEX_NONE && End > Start))
        {
            return false;
        }

        OutBlock = Source.Mid(Start, End - Start);
        return true;
    }

    bool LoadMeshRebuildGuardSource(FAutomationTestBase& Test, FString& OutSource)
    {
        const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("PinWright"));
        if (!Test.TestTrue(TEXT("PinWright plugin is available for guard ownership checks"),
                Plugin.IsValid()))
        {
            return false;
        }

        FString Source;
        const FString SourcePath = Plugin->GetBaseDir()
            / TEXT("Source/PinWright/Private/Utils/MeshRebuildRenderGuard.h");
        if (!Test.TestTrue(TEXT("MeshRebuildRenderGuard.h loads for ownership checks"),
                FFileHelper::LoadFileToString(Source, *SourcePath)))
        {
            return false;
        }

        OutSource = NeutralizeSourceText(Source);
        return true;
    }

    bool ReadFirstItem(const TSharedPtr<FJsonObject>& Result,
                       TSharedPtr<FJsonObject>& OutItem)
    {
        const TArray<TSharedPtr<FJsonValue>>* Items = nullptr;
        if (!Result.IsValid() || !Result->TryGetArrayField(TEXT("items"), Items)
            || !Items || Items->Num() == 0 || !(*Items)[0].IsValid())
        {
            return false;
        }

        OutItem = (*Items)[0]->AsObject();
        return OutItem.IsValid();
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGenerateLodsSavesToDiskTest,
    "PinWright.asset.generate_lods.SavesToDisk",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGenerateLodsSavesToDiskTest::RunTest(const FString& /*Parameters*/)
{
    using namespace GenerateLodsPersistenceAndCompileTests;
    using namespace NaniteRebuildSaveTestUtils;

    TestTrue(TEXT("asset.generate_lods accepts save"),
        ParamSpecTestHelpers::IsParamAccepted(TEXT("asset.generate_lods"), TEXT("save")));

    FScopedFixture Fixture(TEXT("GenerateLods"), TEXT("Saved"));
    TestNotNull(TEXT("generate_lods fixture StaticMesh created"), Fixture.Mesh);
    if (!Fixture.Mesh)
    {
        return false;
    }

    TSharedPtr<FJsonObject> OverCapPayload = MakeShared<FJsonObject>();
    OverCapPayload->SetStringField(TEXT("assetPath"), Fixture.Mesh->GetPathName());
    OverCapPayload->SetNumberField(TEXT("lodCount"), 9);
    TSharedRef<FTestResponseCapture> OverCapCapture = MakeShared<FTestResponseCapture>();
    TestTrue(TEXT("asset.generate_lods over-cap handler is registered"),
        InvokeHandlerWithSharedCapture(TEXT("asset.generate_lods"), OverCapPayload,
            OverCapCapture));
    PumpUntilCaptured(*OverCapCapture, 1.0);
    TestTrue(TEXT("asset.generate_lods over-cap request responded"), OverCapCapture->bWasCalled);
    TestFalse(TEXT("asset.generate_lods rejects an over-cap LOD count"),
        OverCapCapture->bSuccess);
    TestEqual(TEXT("asset.generate_lods over-cap error is INVALID_PARAMS"),
        OverCapCapture->ErrorCode, FString(TEXT("INVALID_PARAMS")));
    TestTrue(TEXT("asset.generate_lods over-cap error names the UE numeric cap"),
        OverCapCapture->Message.Contains(TEXT("8")));

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), Fixture.Mesh->GetPathName());
    Payload->SetNumberField(TEXT("lodCount"), 3);

    TSharedRef<FTestResponseCapture> Capture = MakeShared<FTestResponseCapture>();
    TestTrue(TEXT("asset.generate_lods handler is registered"),
        InvokeHandlerWithSharedCapture(TEXT("asset.generate_lods"), Payload, Capture));
    PumpUntilCaptured(*Capture, 5.0);
    TestTrue(TEXT("asset.generate_lods responded"), Capture->bWasCalled);
    TestTrue(*FString::Printf(TEXT("asset.generate_lods succeeded (errorCode='%s')"),
        *Capture->ErrorCode), Capture->bSuccess);
    TestTrue(TEXT("asset.generate_lods returned a result"), Capture->Result.IsValid());
    if (!Capture->bWasCalled || !Capture->bSuccess || !Capture->Result.IsValid())
    {
        return false;
    }

    double Processed = 0.0;
    TestTrue(TEXT("result reports processed"),
        Capture->Result->TryGetNumberField(TEXT("processed"), Processed));
    TestEqual(TEXT("one mesh was processed"), static_cast<int32>(Processed), 1);

    bool bTimedOut = true;
    TestTrue(TEXT("result reports timedOut"),
        Capture->Result->TryGetBoolField(TEXT("timedOut"), bTimedOut));
    TestFalse(TEXT("saved fixture did not time out"), bTimedOut);

    TestDurablePersistenceResult(*this, Capture->Result, Fixture);

    TSharedPtr<FJsonObject> Item;
    TestTrue(TEXT("result contains one per-mesh item"), ReadFirstItem(Capture->Result, Item));
    if (!Item.IsValid())
    {
        return false;
    }

    bool bItemOk = false;
    TestTrue(TEXT("saved fixture item is successful"),
        Item->TryGetBoolField(TEXT("ok"), bItemOk) && bItemOk);
    double ActualLodCount = 0.0;
    TestTrue(TEXT("saved fixture item reports actualLodCount"),
        Item->TryGetNumberField(TEXT("actualLodCount"), ActualLodCount));
    TestEqual(TEXT("saved fixture item reports three LODs"),
        static_cast<int32>(ActualLodCount), 3);
    bool bItemTimedOut = true;
    TestTrue(TEXT("saved fixture item reports timedOut:false"),
        Item->TryGetBoolField(TEXT("timedOut"), bItemTimedOut));
    TestFalse(TEXT("saved fixture item did not time out"), bItemTimedOut);
    TestEqual(TEXT("saved fixture item names its package"),
        Item->GetStringField(TEXT("package")), Fixture.PackagePath);

    const FString Filename = PackageFilenameFromAssetPath(Fixture.PackagePath);
    TestTrue(TEXT("generated fixture package exists on disk"),
        !Filename.IsEmpty() && IFileManager::Get().FileSize(*Filename) > 0);
    TestEqual(TEXT("in-memory fixture has the requested render LOD count"),
        Fixture.Mesh->GetNumLODs(), 3);

    // Regression for partial batches: one valid row must not promote a two-input request to
    // the single-input top-level persistence shape.
    FScopedFixture BatchFixture(TEXT("GenerateLods"), TEXT("PartialBatch"));
    TestNotNull(TEXT("partial-batch fixture StaticMesh created"), BatchFixture.Mesh);
    if (!BatchFixture.Mesh)
    {
        return false;
    }

    TArray<TSharedPtr<FJsonValue>> BatchInputs;
    BatchInputs.Add(MakeShared<FJsonValueString>(BatchFixture.Mesh->GetPathName()));
    BatchInputs.Add(MakeShared<FJsonValueString>(
        TEXT("/Game/PinWrightTests/SM_GenerateLods_MissingPartialBatch")));
    TSharedPtr<FJsonObject> PartialBatchPayload = MakeShared<FJsonObject>();
    PartialBatchPayload->SetArrayField(TEXT("assetPaths"), BatchInputs);
    PartialBatchPayload->SetNumberField(TEXT("lodCount"), 2);
    PartialBatchPayload->SetBoolField(TEXT("partial"), true);
    PartialBatchPayload->SetBoolField(TEXT("save"), false);

    TSharedRef<FTestResponseCapture> PartialBatchCapture = MakeShared<FTestResponseCapture>();
    TestTrue(TEXT("asset.generate_lods partial-batch handler is registered"),
        InvokeHandlerWithSharedCapture(TEXT("asset.generate_lods"), PartialBatchPayload,
            PartialBatchCapture));
    PumpUntilCaptured(*PartialBatchCapture, 5.0);
    TestTrue(TEXT("asset.generate_lods partial batch responded"),
        PartialBatchCapture->bWasCalled);
    TestTrue(TEXT("asset.generate_lods partial batch accepted one valid row"),
        PartialBatchCapture->bSuccess);
    TestTrue(TEXT("partial-batch result is present"), PartialBatchCapture->Result.IsValid());
    if (!PartialBatchCapture->bWasCalled || !PartialBatchCapture->Result.IsValid())
    {
        return false;
    }

    double Requested = 0.0;
    double Succeeded = 0.0;
    double Failed = 0.0;
    TestTrue(TEXT("partial-batch result reports original request count"),
        PartialBatchCapture->Result->TryGetNumberField(TEXT("requested"), Requested));
    TestTrue(TEXT("partial-batch result reports one succeeded row"),
        PartialBatchCapture->Result->TryGetNumberField(TEXT("succeeded"), Succeeded));
    TestTrue(TEXT("partial-batch result reports one failed row"),
        PartialBatchCapture->Result->TryGetNumberField(TEXT("failed"), Failed));
    TestEqual(TEXT("partial-batch requested count remains two"),
        static_cast<int32>(Requested), 2);
    TestEqual(TEXT("partial-batch succeeded count is one"),
        static_cast<int32>(Succeeded), 1);
    TestEqual(TEXT("partial-batch failed count is one"),
        static_cast<int32>(Failed), 1);
    TestFalse(TEXT("partial batch omits single-input top-level package"),
        PartialBatchCapture->Result->HasField(TEXT("package")));
    TestFalse(TEXT("partial batch omits single-input saveRequested"),
        PartialBatchCapture->Result->HasField(TEXT("saveRequested")));
    TestFalse(TEXT("partial batch omits single-input saved"),
        PartialBatchCapture->Result->HasField(TEXT("saved")));
    TestFalse(TEXT("partial batch omits single-input saveState"),
        PartialBatchCapture->Result->HasField(TEXT("saveState")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGenerateLodsNotReportedCompleteWhileCompilingTest,
    "PinWright.asset.generate_lods.NotReportedCompleteWhileCompiling",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGenerateLodsNotReportedCompleteWhileCompilingTest::RunTest(
    const FString& /*Parameters*/)
{
    using namespace GenerateLodsPersistenceAndCompileTests;
    using namespace NaniteRebuildSaveTestUtils;

    FString GenerateLodsSource;
    if (!LoadGenerateLodsSource(*this, GenerateLodsSource))
    {
        return false;
    }

    TestTrue(TEXT("generate_lods declares save"),
        GenerateLodsSource.Contains(TEXT("RPC_PARAM_DEF(\"save\", \"boolean\"")));
    TestTrue(TEXT("generate_lods declares a bounded timeout"),
        GenerateLodsSource.Contains(TEXT("RPC_PARAM_DEF(\"timeoutSeconds\", \"number\"")));
    TestTrue(TEXT("generate_lods enforces the UE StaticMesh LOD cap"),
        GenerateLodsSource.Contains(TEXT("MAX_STATIC_MESH_LODS"))
        && GenerateLodsSource.Contains(TEXT("must not exceed")));
    TestTrue(TEXT("generate_lods uses the shared AssetCompilePump"),
        GenerateLodsSource.Contains(TEXT("PinWright::AssetCompile::AdvanceOnGameThread()")));
    TestTrue(TEXT("generate_lods observes StaticMesh compilation"),
        GenerateLodsSource.Contains(TEXT("IsCompilingForBoundedWait(")));
    TestTrue(TEXT("generate_lods publishes timedOut"),
        GenerateLodsSource.Contains(TEXT("Item->SetBoolField(TEXT(\"timedOut\"), bTimedOut)")));
    TestTrue(TEXT("generate_lods uses durable save presence reporting"),
        GenerateLodsSource.Contains(TEXT("SaveAssetToDiskReportingPresence(")));
    TestTrue(TEXT("generate_lods uses the standard save-state report"),
        GenerateLodsSource.Contains(TEXT("AddAssetSaveReport(Item, bSave, bSavedToDisk, SaveState)")));
    const int32 TimeoutFieldIndex = GenerateLodsSource.Find(
        TEXT("Item->SetBoolField(TEXT(\"timedOut\"), bTimedOut)"));
    const int32 TimeoutSaveCallIndex = TimeoutFieldIndex == INDEX_NONE
        ? INDEX_NONE
        : GenerateLodsSource.Find(
            TEXT("AddAssetSaveReport(Item,"), ESearchCase::CaseSensitive,
            ESearchDir::FromStart, TimeoutFieldIndex);
    const int32 TimeoutSaveFalseIndex = TimeoutSaveCallIndex == INDEX_NONE
        ? INDEX_NONE
        : GenerateLodsSource.Find(
            TEXT("false"), ESearchCase::CaseSensitive,
            ESearchDir::FromStart, TimeoutSaveCallIndex);
    const int32 TimeoutSaveStateIndex = TimeoutSaveFalseIndex == INDEX_NONE
        ? INDEX_NONE
        : GenerateLodsSource.Find(
            TEXT("EAssetSaveState::NotRequested"), ESearchCase::CaseSensitive,
            ESearchDir::FromStart, TimeoutSaveFalseIndex);
    TestTrue(TEXT("generate_lods uses the explicit non-attempted timeout save state"),
        TimeoutFieldIndex != INDEX_NONE
        && TimeoutSaveCallIndex > TimeoutFieldIndex
        && TimeoutSaveFalseIndex > TimeoutSaveCallIndex
        && TimeoutSaveStateIndex > TimeoutSaveFalseIndex);
    TestFalse(TEXT("generate_lods does not use dirty-only McpSafeAssetSave"),
        GenerateLodsSource.Contains(TEXT("McpSafeAssetSave(")));
    TestFalse(TEXT("generate_lods does not use PostEditChange's unbounded compile path"),
        GenerateLodsSource.Contains(TEXT("Mesh->PostEditChange();")));
    TestFalse(TEXT("generate_lods does not call FinishCompilation before its bounded wait"),
        GenerateLodsSource.Contains(
            TEXT("FStaticMeshCompilingManager::Get().FinishCompilation(")));
    TestTrue(TEXT("generate_lods retains the render guard after a timeout"),
        GenerateLodsSource.Contains(TEXT("TimedOutMeshes.AddUnique(Mesh)"))
        && GenerateLodsSource.Contains(TEXT("RetainedCompileGuard->Start()")));
    TestTrue(TEXT("generate_lods gates success on compilation terminal state"),
        GenerateLodsSource.Contains(TEXT("bCompilationTerminal && ActualLodCount"))
        && GenerateLodsSource.Contains(TEXT("const int32 ActualLodCount = Mesh->GetNumLODs();")));
    TestFalse(TEXT("generate_lods does not use source-model count for render verification"),
        GenerateLodsSource.Contains(TEXT("GetNumSourceModels()")));

    FString GuardSource;
    if (!LoadMeshRebuildGuardSource(*this, GuardSource))
    {
        return false;
    }

    const int32 RetainedMemberIndex = GuardSource.Find(
        TEXT("TArray<TStrongObjectPtr<UActorComponent>> RetainedComponents;"));
    const int32 ContextMemberIndex = GuardSource.Find(
        TEXT("TArray<FComponentRecreateRenderStateContext> Contexts;"));
    const int32 RetainedPopulationIndex = GuardSource.Find(
        TEXT("RetainedComponents.Emplace(Component);"));
    const int32 ContextPopulationIndex = GuardSource.Find(
        TEXT("Contexts.Emplace(Component);"));
    TestTrue(TEXT("render guard retains every context component for its scope lifetime"),
        RetainedMemberIndex != INDEX_NONE
        && ContextMemberIndex > RetainedMemberIndex
        && RetainedPopulationIndex != INDEX_NONE
        && ContextPopulationIndex > RetainedPopulationIndex);

    const int32 DeadlineIndex = GenerateLodsSource.Find(
        TEXT("const double CompileDeadline = FPlatformTime::Seconds()"));
    const int32 SecondDeadlineIndex = GenerateLodsSource.Find(
        TEXT("const double CompileDeadline = FPlatformTime::Seconds()"),
        ESearchCase::CaseSensitive, ESearchDir::FromStart, DeadlineIndex + 1);
    const int32 GuardedCallIndex = GenerateLodsSource.Find(
        TEXT("RunGuardedStaticMeshRebuild("));
    TestTrue(TEXT("generate_lods uses one request-wide compile deadline"),
        DeadlineIndex != INDEX_NONE
        && SecondDeadlineIndex == INDEX_NONE
        && GuardedCallIndex > DeadlineIndex);
    const int32 ModifyIndex = GenerateLodsSource.Find(
        TEXT("Mesh->Modify();"), ESearchCase::CaseSensitive,
        ESearchDir::FromStart, DeadlineIndex);
    const int32 BuildIndex = GenerateLodsSource.Find(
        TEXT("Mesh->Build("), ESearchCase::CaseSensitive,
        ESearchDir::FromStart, ModifyIndex);
    const int32 CompileWaitCallIndex = GenerateLodsSource.Find(
        TEXT("bCompilationTerminal = WaitForCompilation("), ESearchCase::CaseSensitive,
        ESearchDir::FromStart, BuildIndex);
    const int32 TimeoutReportIndex = GenerateLodsSource.Find(
        TEXT("Item->SetBoolField(TEXT(\"timedOut\"), bTimedOut)"),
        ESearchCase::CaseSensitive, ESearchDir::FromStart, CompileWaitCallIndex);
    const int32 SaveIndex = GenerateLodsSource.Find(
        TEXT("SaveAssetToDiskReportingPresence("), ESearchCase::CaseSensitive,
        ESearchDir::FromStart, TimeoutReportIndex);
    const int32 RenderLodReadbackIndex = GenerateLodsSource.Find(
        TEXT("const int32 ActualLodCount = Mesh->GetNumLODs();"));
    const int32 RenderLodMismatchIndex = RenderLodReadbackIndex == INDEX_NONE
        ? INDEX_NONE
        : GenerateLodsSource.Find(
            TEXT("ActualLodCount != NumLODs"), ESearchCase::CaseSensitive,
            ESearchDir::FromStart, RenderLodReadbackIndex);
    const int32 SuccessIndex = GenerateLodsSource.Find(
        TEXT("Batch.MarkSucceeded("), ESearchCase::CaseSensitive,
        ESearchDir::FromStart, SaveIndex);
    TestTrue(TEXT("compile wait and save precede success accounting"),
        DeadlineIndex != INDEX_NONE
        && ModifyIndex > DeadlineIndex
        && BuildIndex > ModifyIndex
        && CompileWaitCallIndex > BuildIndex
        && TimeoutReportIndex > CompileWaitCallIndex
        && SaveIndex > TimeoutReportIndex
        && SuccessIndex > SaveIndex);
    TestTrue(TEXT("render LOD readback and mismatch failure precede any save"),
        RenderLodReadbackIndex != INDEX_NONE
        && RenderLodMismatchIndex > RenderLodReadbackIndex
        && SaveIndex > RenderLodMismatchIndex);

    TestTrue(TEXT("asset.generate_lods accepts timeoutSeconds"),
        ParamSpecTestHelpers::IsParamAccepted(
            TEXT("asset.generate_lods"), TEXT("timeoutSeconds")));

    FScopedFixture Fixture(TEXT("GenerateLods"), TEXT("Compile"));
    TestNotNull(TEXT("compile fixture StaticMesh created"), Fixture.Mesh);
    if (!Fixture.Mesh)
    {
        return false;
    }

    // Do not pre-build this fixture: UStaticMesh::Build synchronously drains an already-running
    // build before queuing the next one. The production handler must own the only build trigger so
    // its deadline and shared pump are the code exercised by this call.
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), Fixture.Mesh->GetPathName());
    Payload->SetNumberField(TEXT("lodCount"), 2);
    Payload->SetBoolField(TEXT("save"), false);
    Payload->SetNumberField(TEXT("timeoutSeconds"), 0.25);

    TSharedRef<FTestResponseCapture> Capture = MakeShared<FTestResponseCapture>();
    bool bHandlerFound = false;
    {
        // The override applies only to the post-Build bounded wait. Keep it active through the
        // latent response pump, then let its RAII destructor run before any retained cleanup tick.
        PinWright::AssetCompile::FScopedCompilePendingOverride PendingOverride(true);
        bHandlerFound = InvokeHandlerWithSharedCapture(
            TEXT("asset.generate_lods"), Payload, Capture);
        PumpUntilCaptured(*Capture, 5.0);
    }
    TestTrue(TEXT("asset.generate_lods compile handler is registered"), bHandlerFound);
    FTSTicker::GetCoreTicker().Tick(0.0f);
    TestTrue(TEXT("asset.generate_lods compile handler responded"), Capture->bWasCalled);

    TestTrue(TEXT("compile-order call returned a result"), Capture->Result.IsValid());
    if (!Capture->bWasCalled || !Capture->Result.IsValid())
    {
        return false;
    }

    bool bTimedOut = false;
    TestTrue(TEXT("compile-order result reports timedOut:true"),
        Capture->Result->TryGetBoolField(TEXT("timedOut"), bTimedOut) && bTimedOut);
    TestFalse(TEXT("a timed-out compile is not reported as handler success"),
        Capture->bSuccess);
    double Succeeded = 1.0;
    TestTrue(TEXT("timed-out result reports succeeded"),
        Capture->Result->TryGetNumberField(TEXT("succeeded"), Succeeded));
    TestEqual(TEXT("timed-out compile reports no succeeded rows"),
        static_cast<int32>(Succeeded), 0);

    TSharedPtr<FJsonObject> Item;
    TestTrue(TEXT("timed-out result includes the failed mesh row"),
        ReadFirstItem(Capture->Result, Item));
    if (!Item.IsValid())
    {
        return false;
    }
    bool bItemTimedOut = false;
    TestTrue(TEXT("timed-out mesh row reports timedOut:true"),
        Item->TryGetBoolField(TEXT("timedOut"), bItemTimedOut) && bItemTimedOut);
    TestFalse(TEXT("timed-out mesh row is not marked successful"),
        Item->GetBoolField(TEXT("ok")));
    bool bSaveRequested = true;
    TestTrue(TEXT("timed-out row reports no save attempt"),
        Item->TryGetBoolField(TEXT("saveRequested"), bSaveRequested) && !bSaveRequested);
    bool bSaved = true;
    TestTrue(TEXT("timed-out row reports saved:false"),
        Item->TryGetBoolField(TEXT("saved"), bSaved) && !bSaved);
    TestFalse(TEXT("timed-out row has no pendingFlush"),
        Item->HasField(TEXT("pendingFlush")));
    TestEqual(TEXT("timed-out row reports notRequested saveState"),
        Item->GetStringField(TEXT("saveState")), FString(TEXT("notRequested")));

    return true;
}
