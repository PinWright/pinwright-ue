// Copyright (c) 2026 Alexander Penkin. MIT License.

// Response-level coverage for the non-blocking Niagara compile route and the shared persistence
// report emitted by Niagara mutation finalization.
#include "Misc/AutomationTest.h"

#include "Handlers/HandlerRegistration.h"
#include "Handlers/Niagara/NiagaraEditTypes.h"
#include "Tests/TestSkipReporting.h"
#include "Tests/TestUtils.h"

#include "Interfaces/IPluginManager.h"
#include "Misc/FileHelper.h"
#include "NiagaraEmitter.h"
#include "NiagaraSystem.h"
#include "UObject/Package.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FNiagaraCompileStatusNonBlockingProbeContractTest,
    "PinWright.niagara.CompileStatus.NonBlockingProbeContract",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraCompileStatusNonBlockingProbeContractTest::RunTest(const FString& Parameters)
{
    TestTrue(TEXT("niagara.compile is registered"), IsHandlerRegistered(TEXT("niagara.compile")));
    TestTrue(TEXT("niagara.compile_status is registered"), IsHandlerRegistered(TEXT("niagara.compile_status")));
    TestFalse(TEXT("niagara.mark_dirty remains absent"), IsHandlerRegistered(TEXT("niagara.mark_dirty")));

    UNiagaraSystem* System = NewObject<UNiagaraSystem>(GetTransientPackage());
    if (!System)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("niagara-compile-status-fixture-failed"),
            TEXT("NewObject returned null; non-blocking handler assertions were not exercised."));
        return true;
    }
    System->AddToRoot();
    const FString AssetPath = System->GetPathName();

    // wait:false is the shared-editor-safe route. This source-less fixture cannot issue a real
    // compile, which makes it a deterministic check that the response does not invent one.
    {
        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("assetPath"), AssetPath);
        Params->SetBoolField(TEXT("wait"), false);

        FTestResponseCapture Capture;
        TestTrue(TEXT("niagara.compile handler found"),
            InvokeHandlerWithCapture(TEXT("niagara.compile"), Params, Capture));
        TestTrue(TEXT("wait:false succeeds"), Capture.bSuccess);
        if (Capture.bSuccess && Capture.Result.IsValid())
        {
            bool bRequested = true;
            bool bCompiled = true;
            bool bCompleted = true;
            bool bWaited = true;
            bool bOutstanding = true;
            bool bTimedOut = true;
            double TimeoutSeconds = 0.0;
            const TArray<TSharedPtr<FJsonValue>>* StillCompiling = nullptr;
            FString Status;
            TestTrue(TEXT("requested is present"), Capture.Result->TryGetBoolField(TEXT("requested"), bRequested));
            TestFalse(TEXT("no engine request was issued"), bRequested);
            TestTrue(TEXT("compiled is present"), Capture.Result->TryGetBoolField(TEXT("compiled"), bCompiled));
            TestFalse(TEXT("no-op request is not completed"), bCompiled);
            TestTrue(TEXT("completed is present"), Capture.Result->TryGetBoolField(TEXT("completed"), bCompleted));
            TestFalse(TEXT("completed agrees with compiled"), bCompleted);
            TestTrue(TEXT("waited is present"), Capture.Result->TryGetBoolField(TEXT("waited"), bWaited));
            TestFalse(TEXT("wait:false never enters the blocking wait"), bWaited);
            TestTrue(TEXT("outstanding state is present"),
                Capture.Result->TryGetBoolField(TEXT("outstandingCompilationRequests"), bOutstanding));
            TestFalse(TEXT("fixture has no outstanding compile"), bOutstanding);
            TestTrue(TEXT("status is present"), Capture.Result->TryGetStringField(TEXT("status"), Status));
            TestEqual(TEXT("no-op status is honest"), Status, FString(TEXT("notRequested")));
            TestTrue(TEXT("timedOut is present"), Capture.Result->TryGetBoolField(TEXT("timedOut"), bTimedOut));
            TestFalse(TEXT("wait:false did not time out"), bTimedOut);
            TestTrue(TEXT("timeoutSeconds is present"),
                Capture.Result->TryGetNumberField(TEXT("timeoutSeconds"), TimeoutSeconds));
            TestEqual(TEXT("default wait budget is 60 seconds"), TimeoutSeconds, 60.0);
            TestTrue(TEXT("stillCompiling is present"),
                Capture.Result->TryGetArrayField(TEXT("stillCompiling"), StillCompiling));
            TestTrue(TEXT("quiet fixture has no still-compiling systems"),
                StillCompiling && StillCompiling->IsEmpty());
        }
    }

    for (const double InvalidTimeout : {0.0, 60.01})
    {
        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("assetPath"), AssetPath);
        Params->SetNumberField(TEXT("timeoutSeconds"), InvalidTimeout);
        FTestResponseCapture Capture;
        InvokeHandlerWithCapture(TEXT("niagara.compile"), Params, Capture);
        TestFalse(FString::Printf(TEXT("timeout %.2f is rejected"), InvalidTimeout), Capture.bSuccess);
        TestEqual(TEXT("invalid timeout uses INVALID_PARAMS"),
            Capture.ErrorCode, FString(TEXT("INVALID_PARAMS")));
    }

    {
        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("assetPath"), AssetPath);
        Params->SetStringField(TEXT("timeoutSeconds"), TEXT("fast"));
        FTestResponseCapture Capture;
        InvokeHandlerWithCapture(TEXT("niagara.compile"), Params, Capture);
        TestFalse(TEXT("non-number timeout is rejected"), Capture.bSuccess);
        TestEqual(TEXT("non-number timeout uses INVALID_PARAMS"),
            Capture.ErrorCode, FString(TEXT("INVALID_PARAMS")));
    }

    {
        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("assetPath"), AssetPath);

        FTestResponseCapture Capture;
        TestTrue(TEXT("niagara.compile_status handler found"),
            InvokeHandlerWithCapture(TEXT("niagara.compile_status"), Params, Capture));
        TestTrue(TEXT("compile_status succeeds"), Capture.bSuccess);
        if (Capture.bSuccess && Capture.Result.IsValid())
        {
            bool bCompleted = true;
            bool bSuccessful = true;
            bool bOutstanding = true;
            bool bIncludesGpuShaders = false;
            bool bCpuPending = true;
            bool bQueueObserved = false;
            double AffectedSystemCount = 0.0;
            FString Status;
            FString ScriptCheck;
            FString QueueScope;
            TestTrue(TEXT("completed is present"), Capture.Result->TryGetBoolField(TEXT("completed"), bCompleted));
            TestFalse(TEXT("unknown scripts are not completed"), bCompleted);
            TestTrue(TEXT("successful is present"), Capture.Result->TryGetBoolField(TEXT("successful"), bSuccessful));
            TestFalse(TEXT("unverified compile is not successful"), bSuccessful);
            TestTrue(TEXT("outstanding state is present"),
                Capture.Result->TryGetBoolField(TEXT("outstandingCompilationRequests"), bOutstanding));
            TestFalse(TEXT("status probe does not start work"), bOutstanding);
            TestTrue(TEXT("outstanding scope includes GPU shaders"),
                Capture.Result->TryGetBoolField(TEXT("outstandingIncludesGpuShaders"), bIncludesGpuShaders));
            TestTrue(TEXT("GPU inclusion is explicit"), bIncludesGpuShaders);
            // The separate measurement: scope is inclusive, but this fixture has no emitters, so
            // no GPU work can exist inside it.
            bool bHasGpuSimulation = true;
            TestTrue(TEXT("GPU simulation state is present"),
                Capture.Result->TryGetBoolField(TEXT("hasGpuSimulation"), bHasGpuSimulation));
            TestFalse(TEXT("a system with no GPU emitter has no GPU simulation"), bHasGpuSimulation);
            TestTrue(TEXT("CPU pending state is present"),
                Capture.Result->TryGetBoolField(TEXT("cpuScriptCompilationPending"), bCpuPending));
            TestFalse(TEXT("fixture has no CPU compile pending"), bCpuPending);
            TestTrue(TEXT("queue-observation state is present"),
                Capture.Result->TryGetBoolField(TEXT("compileQueueObserved"), bQueueObserved));
            TestTrue(TEXT("a direct system queue is observable"), bQueueObserved);
            TestTrue(TEXT("queue scope is present"),
                Capture.Result->TryGetStringField(TEXT("compileQueueScope"), QueueScope));
            TestEqual(TEXT("direct-system queue scope is explicit"), QueueScope, FString(TEXT("system")));
            TestTrue(TEXT("affected-system count is present"),
                Capture.Result->TryGetNumberField(TEXT("affectedSystemCount"), AffectedSystemCount));
            TestEqual(TEXT("the direct target is the one affected system"), AffectedSystemCount, 1.0);
            TestTrue(TEXT("status is present"), Capture.Result->TryGetStringField(TEXT("status"), Status));
            TestEqual(TEXT("unknown script status stays unverified"), Status, FString(TEXT("unverified")));
            TestTrue(TEXT("scriptCompileCheck is present"),
                Capture.Result->TryGetStringField(TEXT("scriptCompileCheck"), ScriptCheck));
            TestEqual(TEXT("script verdict is unverified"), ScriptCheck, FString(TEXT("unverified")));
        }
    }

    System->RemoveFromRoot();

    // A standalone emitter has no compile queue of its own. Even if its cached script statuses
    // look terminal, the probe must stay unverified when no loaded system using it can be observed.
    UNiagaraEmitter* Emitter = NewObject<UNiagaraEmitter>(GetTransientPackage());
    if (Emitter)
    {
        Emitter->AddToRoot();
        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("assetPath"), Emitter->GetPathName());
        FTestResponseCapture Capture;
        TestTrue(TEXT("emitter compile_status handler found"),
            InvokeHandlerWithCapture(TEXT("niagara.compile_status"), Params, Capture));
        TestTrue(TEXT("emitter compile_status succeeds"), Capture.bSuccess);
        if (Capture.bSuccess && Capture.Result.IsValid())
        {
            bool bCompleted = true;
            bool bSuccessful = true;
            bool bQueueObserved = true;
            double AffectedSystemCount = 1.0;
            FString Status;
            FString QueueScope;
            Capture.Result->TryGetBoolField(TEXT("completed"), bCompleted);
            Capture.Result->TryGetBoolField(TEXT("successful"), bSuccessful);
            Capture.Result->TryGetBoolField(TEXT("compileQueueObserved"), bQueueObserved);
            Capture.Result->TryGetNumberField(TEXT("affectedSystemCount"), AffectedSystemCount);
            Capture.Result->TryGetStringField(TEXT("status"), Status);
            Capture.Result->TryGetStringField(TEXT("compileQueueScope"), QueueScope);
            TestFalse(TEXT("an unobserved emitter queue is not completed"), bCompleted);
            TestFalse(TEXT("an unobserved emitter queue is not successful"), bSuccessful);
            TestFalse(TEXT("no affected system means no queue was observed"), bQueueObserved);
            TestEqual(TEXT("standalone emitter has no affected loaded systems"), AffectedSystemCount, 0.0);
            TestEqual(TEXT("standalone emitter status is conservative"), Status, FString(TEXT("unverified")));
            TestEqual(TEXT("emitter queue scope names loaded affected systems"),
                QueueScope, FString(TEXT("loadedSystemsUsingEmitter")));
        }
        Emitter->RemoveFromRoot();
    }
    else
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("niagara-compile-status-emitter-fixture-failed"),
            TEXT("NewObject returned null; conservative emitter-status assertions were not exercised."));
    }

    TSharedPtr<FJsonObject> MissingParams = MakeShared<FJsonObject>();
    MissingParams->SetStringField(TEXT("assetPath"), TEXT("/Game/Test/DoesNotExistNiagaraSystem_XYZZY"));
    FTestResponseCapture MissingCapture;
    InvokeHandlerWithCapture(TEXT("niagara.compile_status"), MissingParams, MissingCapture);
    TestFalse(TEXT("missing status target is not a success"), MissingCapture.bSuccess);
    TestEqual(TEXT("missing status target uses ASSET_NOT_FOUND"),
        MissingCapture.ErrorCode, FString(TEXT("ASSET_NOT_FOUND")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FNiagaraCompileSaveReportsAssetSaveStateTest,
    "PinWright.niagara.CompileSave.ReportsAssetSaveState",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraCompileSaveReportsAssetSaveStateTest::RunTest(const FString& Parameters)
{
    UNiagaraSystem* System = NewObject<UNiagaraSystem>(GetTransientPackage());
    if (!System)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("niagara-compile-save-fixture-failed"),
            TEXT("NewObject returned null; save-state assertions were not exercised."));
        return true;
    }
    System->AddToRoot();

    FNiagaraResolvedTarget Target;
    Target.Asset = System;
    Target.System = System;
    Target.AssetPath = System->GetPathName();
    Target.AssetKind = TEXT("NiagaraSystem");

    bool bCompiled = true;
    bool bSaved = true;
    NiagaraEdit::FinalizeNiagaraEdit(
        Target,
        FNiagaraEditOptions{/*bCompile=*/true, /*bSave=*/true},
        bCompiled,
        bSaved);
    TestFalse(TEXT("source-less compile did not land"), bCompiled);
    TestFalse(TEXT("save is refused when no requested compile landed"), bSaved);
    TestTrue(TEXT("refusal is retained as failed save state"),
        Target.SaveState == EAssetSaveState::Failed);

    const TSharedPtr<FJsonObject> Result = NiagaraEdit::MakeMutationResult(
        TEXT("test_compile_save"), Target,
        FNiagaraEditOptions{/*bCompile=*/true, /*bSave=*/true}, bCompiled, bSaved);
    bool bSaveRequested = false;
    bool bPendingFlush = true;
    FString SaveState;
    FString SaveDetail;
    TestTrue(TEXT("saveRequested is present"),
        Result->TryGetBoolField(TEXT("saveRequested"), bSaveRequested));
    TestTrue(TEXT("saveRequested echoes the call"), bSaveRequested);
    TestTrue(TEXT("pendingFlush is present"),
        Result->TryGetBoolField(TEXT("pendingFlush"), bPendingFlush));
    TestFalse(TEXT("terminal compile refusal is not flushable"), bPendingFlush);
    TestTrue(TEXT("saveState is present"), Result->TryGetStringField(TEXT("saveState"), SaveState));
    TestEqual(TEXT("saveState distinguishes the refusal"), SaveState, FString(TEXT("failed")));
    TestTrue(TEXT("saveDetail is present"), Result->TryGetStringField(TEXT("saveDetail"), SaveDetail));
    TestFalse(TEXT("saveDetail gives a remedy"), SaveDetail.IsEmpty());

    TSharedPtr<FJsonObject> DeferredResult = MakeShared<FJsonObject>();
    NiagaraEdit::AddNiagaraAssetSaveReport(
        DeferredResult,
        /*bSaveRequested=*/true,
        /*bSavedToDisk=*/false,
        EAssetSaveState::Deferred);
    bool bDeferredPendingFlush = false;
    FString DeferredSaveState;
    TestTrue(TEXT("deferred pendingFlush is present"),
        DeferredResult->TryGetBoolField(TEXT("pendingFlush"), bDeferredPendingFlush));
    TestTrue(TEXT("only a deferred save remains flushable"), bDeferredPendingFlush);
    TestTrue(TEXT("deferred saveState is present"),
        DeferredResult->TryGetStringField(TEXT("saveState"), DeferredSaveState));
    TestEqual(TEXT("deferred state remains distinct"),
        DeferredSaveState, FString(TEXT("deferred")));

    System->RemoveFromRoot();
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FNiagaraCustomEmitterMutationCompileSaveGateTest,
    "PinWright.niagara.CompileSave.CustomEmitterMutationsGatePersistence",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraCustomEmitterMutationCompileSaveGateTest::RunTest(const FString& Parameters)
{
    const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("PinWright"));
    const FString SourcePath = Plugin.IsValid()
        ? Plugin->GetBaseDir() / TEXT("Source/PinWright/Private/Handlers/Niagara/NiagaraHandler.cpp")
        : FString();
    FString Contents;
    if (SourcePath.IsEmpty() || !FFileHelper::LoadFileToString(Contents, *SourcePath))
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("niagara-handler-source-unreadable"),
            FString::Printf(TEXT("Could not read '%s'; custom mutation gate assertions were not exercised."), *SourcePath));
        return true;
    }

    auto CountOccurrences = [&Contents](const TCHAR* Needle)
    {
        int32 Count = 0;
        int32 Offset = 0;
        while ((Offset = Contents.Find(Needle, ESearchCase::CaseSensitive, ESearchDir::FromStart, Offset)) != INDEX_NONE)
        {
            ++Count;
            Offset += FCString::Strlen(Needle);
        }
        return Count;
    };

    TestEqual(TEXT("add/remove/refresh pass the actual request result into the engine wait"),
        CountOccurrences(TEXT("WaitForSystemCompile(*System, bCompileIssued)")), 3);
    TestEqual(TEXT("add/remove/refresh use the Niagara AssetSaveState response helper"),
        CountOccurrences(TEXT("NiagaraEdit::AddNiagaraAssetSaveReport(Result, bSaveRequested, bSaved")), 3);
    TestEqual(TEXT("add/remove/refresh each require a landed compile before a paired save"),
        CountOccurrences(TEXT("const bool bCompileAllowsSave")), 3);
    TestFalse(TEXT("custom mutation response keeps timeout detail in its save-state remedy"),
        Contents.Contains(TEXT("compileTimedOut"), ESearchCase::CaseSensitive));
    return true;
}
