// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression coverage for the distinction between requesting a Niagara compile and observing
// one land. The behavioral case duplicates a saved stock Niagara fixture with its graph and
// message keys intact; compiling a source-less synthetic system can crash the suite host.
#include "Misc/AutomationTest.h"

#include "Handlers/Niagara/NiagaraCompileWait.h"
#include "Handlers/Niagara/NiagaraEditTypes.h"
#include "Tests/Assets/NiagaraEditTestUtils.h"
#include "Tests/TestSkipReporting.h"

#include "Interfaces/IPluginManager.h"
#include "Misc/FileHelper.h"
#include "NiagaraEmitter.h"
#include "NiagaraEmitterHandle.h"
#include "NiagaraScript.h"
#include "NiagaraSystem.h"
#include "UObject/Package.h"

namespace NiagaraCompileWaitTestLocal
{
    UNiagaraSystem* DuplicateCompilableFixture()
    {
        UNiagaraSystem* Source = LoadObject<UNiagaraSystem>(
            nullptr, NiagaraEditTestUtils::FixtureSystemAssetPath);
        if (!Source)
        {
            return nullptr;
        }

        const FString AssetName = NiagaraEditTestUtils::MakeAssetName(TEXT("NS_CompileWait"));
        const FString PackageName = FString::Printf(TEXT("/Game/PinWrightTests/%s"), *AssetName);
        UPackage* Package = CreatePackage(*PackageName);
        Package->SetFlags(RF_Transient);
        UNiagaraSystem* System = DuplicateObject<UNiagaraSystem>(
            Source, Package, FName(*AssetName));
        if (System)
        {
            System->SetFlags(RF_Public | RF_Standalone | RF_Transactional);
            System->AddToRoot();
        }
        return System;
    }

    UNiagaraEmitter* FindFixtureEmitter(UNiagaraSystem& System, FGuid& OutVersionGuid)
    {
        for (const FNiagaraEmitterHandle& Handle : System.GetEmitterHandles())
        {
            if (UNiagaraEmitter* Emitter = Handle.GetInstance().Emitter.Get())
            {
                OutVersionGuid = Handle.GetInstance().Version;
                return Emitter;
            }
        }
        return nullptr;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FNiagaraCompileWaitTest,
    "PinWright.niagara.CompileWait.CompletionIsMeasuredNotAssumed",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraCompileWaitTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightNiagara;

    // Only an issued request with no outstanding work is a completed compile.
    FCompileWaitOutcome Quiet;
    TestEqual(TEXT("no compile launched reports notRequested"),
        FString(DescribeCompileOutcome(/*bRequested=*/false, Quiet)), FString(TEXT("notRequested")));
    TestFalse(TEXT("no compile launched did not compile"),
        DidCompileLand(/*bRequested=*/false, Quiet));

    FCompileWaitOutcome Landed;
    Landed.bWaited = true;
    Landed.WaitedSeconds = 1.5;
    TestEqual(TEXT("finished compile reports completed"),
        FString(DescribeCompileOutcome(/*bRequested=*/true, Landed)), FString(TEXT("completed")));
    TestTrue(TEXT("finished compile reports compiled"),
        DidCompileLand(/*bRequested=*/true, Landed));

    FCompileWaitOutcome StillRunning;
    StillRunning.bOutstanding = true;
    TestEqual(TEXT("in-flight work wins over a no-new-request result"),
        FString(DescribeCompileOutcome(/*bRequested=*/false, StillRunning)), FString(TEXT("outstanding")));
    TestFalse(TEXT("in-flight compile must not report compiled"),
        DidCompileLand(/*bRequested=*/true, StillRunning));
    TestFalse(TEXT("in-flight compile must not be persisted"),
        MayPersistAfterCompileWait(StillRunning));

    FCompileWaitOutcome TimedOut;
    TimedOut.bOutstanding = true;
    TimedOut.bTimedOut = true;
    TestEqual(TEXT("expired wait reports timedOut"),
        FString(DescribeCompileOutcome(/*bRequested=*/true, TimedOut)), FString(TEXT("timedOut")));
    TestFalse(TEXT("expired wait cannot report compiled"),
        DidCompileLand(/*bRequested=*/true, TimedOut));
    TestFalse(TEXT("expired wait cannot be persisted"), MayPersistAfterCompileWait(TimedOut));
    TestTrue(TEXT("quiet compile state may be persisted"),
        MayPersistAfterCompileWait(Quiet));

    UNiagaraSystem* IdleSystem = NewObject<UNiagaraSystem>(GetTransientPackage());
    if (!IdleSystem)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("niagara-compile-wait-system-fixture-failed"),
            TEXT("NewObject returned null; request-gate assertions were not exercised."));
        return true;
    }
    IdleSystem->AddToRoot();

    // false is the load-bearing flush gate: observing a target must not turn a pending on-demand
    // state into a compile this caller is not entitled to start.
    const FCompileWaitOutcome IdleWait = WaitForSystemCompile(*IdleSystem, /*bMayFlushRequestCompile=*/false);
    TestFalse(TEXT("idle no-request target does not enter the pump loop"), IdleWait.bWaited);
    TestFalse(TEXT("idle system has nothing outstanding"), IdleWait.bOutstanding);
    TestTrue(TEXT("idle observation returns immediately"), IdleWait.WaitedSeconds < 1.0);

    FNiagaraResolvedTarget SystemTarget;
    SystemTarget.Asset = IdleSystem;
    SystemTarget.System = IdleSystem;
    SystemTarget.AssetPath = IdleSystem->GetPathName();
    SystemTarget.AssetKind = TEXT("NiagaraSystem");
    TestFalse(TEXT("source-less system does not fake a compile request"),
        NiagaraEdit::RequestNiagaraCompile(SystemTarget, /*bForce=*/true));
    TestEqual(TEXT("source-less system records no requested systems"),
        SystemTarget.CompileRequestSystems.Num(), 0);

    // An emitter used by no loaded system is the original false-completion edge: the engine's
    // RequestCompileForEmitter helper returns void, and the old plugin converted that to true.
    UNiagaraEmitter* IdleEmitter = NewObject<UNiagaraEmitter>(GetTransientPackage());
    if (IdleEmitter)
    {
        IdleEmitter->AddToRoot();
        FNiagaraResolvedTarget EmitterTarget;
        EmitterTarget.Asset = IdleEmitter;
        EmitterTarget.Emitter = IdleEmitter;
        EmitterTarget.AssetPath = IdleEmitter->GetPathName();
        EmitterTarget.AssetKind = TEXT("NiagaraEmitter");
        const bool bEmitterRequested = NiagaraEdit::RequestNiagaraCompile(EmitterTarget, /*bForce=*/true);
        TestFalse(TEXT("unreferenced emitter issues no compile"), bEmitterRequested);
        TestEqual(TEXT("unreferenced emitter records no requested systems"),
            EmitterTarget.CompileRequestSystems.Num(), 0);
        TestFalse(TEXT("unreferenced emitter cannot report completed"),
            DidCompileLand(bEmitterRequested,
                WaitForRequestedSystemCompiles(EmitterTarget.CompileRequestSystems)));
        IdleEmitter->RemoveFromRoot();
    }
    else
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("niagara-compile-wait-emitter-fixture-failed"),
            TEXT("NewObject returned null; the emitter no-request assertions were not exercised."));
    }

    IdleSystem->RemoveFromRoot();
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FNiagaraCompileWaitBehavioralTest,
    "PinWright.niagara.CompileWait.BoundedPumpCompletesTransientSystem",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraCompileWaitBehavioralTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightNiagara;

    UNiagaraSystem* System = NiagaraCompileWaitTestLocal::DuplicateCompilableFixture();
    if (!System)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("niagara-compile-fixture-unavailable"),
            FString::Printf(TEXT("could not duplicate '%s'"), NiagaraEditTestUtils::FixtureSystemAssetPath));
        return true;
    }

    UNiagaraScript* DirtyScript = System->GetSystemSpawnScript();
    FGuid EmitterVersionGuid;
    UNiagaraEmitter* Emitter =
        NiagaraCompileWaitTestLocal::FindFixtureEmitter(*System, EmitterVersionGuid);
    if (!DirtyScript || !DirtyScript->IsCompilable() || !DirtyScript->GetLatestSource()
        || !Emitter || !EmitterVersionGuid.IsValid())
    {
        System->RemoveFromRoot();
        PinWrightTestSkip::SkipAssertions(*this, TEXT("niagara-compile-fixture-not-compilable"),
            TEXT("the saved fixture lacks a compilable system script or a versioned emitter"));
        return true;
    }

    const FEmitterCompileObservation Before =
        ObserveEmitterCompiles(*Emitter, EmitterVersionGuid);
    TestTrue(TEXT("emitter fan-out finds its transient owning system"),
        Before.AffectedSystemCount > 0);

    DirtyScript->InvalidateCompileResults(TEXT("PinWright bounded compile-wait test"));
    const bool bRequested = System->RequestCompile(/*bForce=*/true);
    if (!bRequested
        || !System->HasOutstandingCompilationRequests(/*bIncludingGPUShaders=*/false))
    {
        const bool bSafeToUnroot =
            !System->HasOutstandingCompilationRequests(/*bIncludingGPUShaders=*/false);
        if (bSafeToUnroot)
        {
            System->RemoveFromRoot();
        }
        PinWrightTestSkip::SkipAssertions(*this, TEXT("niagara-compile-request-not-observable"),
            TEXT("the host did not launch and expose an outstanding compile for the valid fixture"));
        return true;
    }

    TArray<TWeakObjectPtr<UNiagaraSystem>> RequestedSystems;
    RequestedSystems.Add(System);
    const FCompileWaitOutcome TinyBudget = WaitForEmitterCompile(
        *Emitter,
        EmitterVersionGuid,
        RequestedSystems,
        /*TimeoutSeconds=*/0.000001);
    const bool bOutstandingAfterTiny =
        System->HasOutstandingCompilationRequests(/*bIncludingGPUShaders=*/false);
    TestTrue(TEXT("tiny budget reports timeout"), TinyBudget.bTimedOut);
    TestFalse(TEXT("timed-out compile is not completed"),
        DidCompileLand(bRequested, TinyBudget));
    TestEqual(TEXT("timeout outcome matches the engine's outstanding state"),
        TinyBudget.bOutstanding, bOutstandingAfterTiny);
    TestTrue(TEXT("the timed-out system is named for status polling"),
        TinyBudget.StillCompiling.Contains(System->GetPathName()));

    const FCompileWaitOutcome GenerousBudget = WaitForEmitterCompile(
        *Emitter,
        EmitterVersionGuid,
        RequestedSystems,
        DefaultCompileWaitTimeoutSeconds);
    const bool bOutstandingAfterWait =
        System->HasOutstandingCompilationRequests(/*bIncludingGPUShaders=*/false);
    TestFalse(TEXT("generous budget does not time out"), GenerousBudget.bTimedOut);
    TestFalse(TEXT("completed wait reports no outstanding work"), GenerousBudget.bOutstanding);
    TestFalse(TEXT("engine reports no outstanding work after completion"), bOutstandingAfterWait);
    TestTrue(TEXT("landed compile is reported completed"),
        DidCompileLand(bRequested, GenerousBudget));
    TestEqual(TEXT("completion outcome matches the engine's outstanding state"),
        GenerousBudget.bOutstanding, bOutstandingAfterWait);

    if (!bOutstandingAfterWait)
    {
        System->RemoveFromRoot();
    }
    else
    {
        AddWarning(TEXT("Compile fixture remains rooted because compile work is still active."));
    }
    return true;
}

namespace NiagaraCompileWaitStructureTestLocal
{
    FString HandlerSourcePath(const TCHAR* FileName)
    {
        const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("PinWright"));
        if (!Plugin.IsValid())
        {
            return FString();
        }
        return Plugin->GetBaseDir()
            / TEXT("Source") / TEXT("PinWright") / TEXT("Private")
            / TEXT("Handlers") / TEXT("Niagara") / FileName;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FNiagaraCompileWaitUsesBoundedPumpTest,
    "PinWright.niagara.CompileWait.UsesBoundedCompilePump",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraCompileWaitUsesBoundedPumpTest::RunTest(const FString& Parameters)
{
    const FString SourcePath =
        NiagaraCompileWaitStructureTestLocal::HandlerSourcePath(TEXT("NiagaraCompileWait.cpp"));
    const FString EditTypesSourcePath =
        NiagaraCompileWaitStructureTestLocal::HandlerSourcePath(TEXT("NiagaraEditTypes.cpp"));
    FString Contents;
    FString EditTypesContents;
    if (SourcePath.IsEmpty()
        || EditTypesSourcePath.IsEmpty()
        || !FFileHelper::LoadFileToString(Contents, *SourcePath)
        || !FFileHelper::LoadFileToString(EditTypesContents, *EditTypesSourcePath))
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("compile-wait-source-unreadable"),
            FString::Printf(TEXT("Could not read '%s' or '%s'; engine-completion assertions were not exercised."),
                *SourcePath, *EditTypesSourcePath));
        return true;
    }

    TestTrue(TEXT("bounded wait advances the registered engine compile managers"),
        Contents.Contains(TEXT("AssetCompilePump.h"), ESearchCase::CaseSensitive));
    TestTrue(TEXT("bounded wait polls Niagara completion without blocking inside the engine"),
        Contents.Contains(TEXT("PollForCompilationComplete("), ESearchCase::CaseSensitive));
    TestTrue(TEXT("bounded wait enforces a wall-clock deadline"),
        Contents.Contains(TEXT("DeadlineSeconds"), ESearchCase::CaseSensitive));
    TestFalse(TEXT("bounded wait never enters Niagara's unbounded wait API"),
        Contents.Contains(TEXT("System.WaitForCompilationComplete("), ESearchCase::CaseSensitive));
    TestTrue(TEXT("emitter waits enumerate all loaded affected systems"),
        Contents.Contains(TEXT("UsesEmitter(*System, Emitter, VersionGuid)"), ESearchCase::CaseSensitive));
    TestTrue(TEXT("paired emitter save uses the affected-system wait"),
        EditTypesContents.Contains(TEXT("WaitForEmitterCompile("), ESearchCase::CaseSensitive));
    TestTrue(TEXT("direct system requests require valid compile sources"),
        EditTypesContents.Contains(
            TEXT("if (HasSystemCompileSources(*Target.System))"),
            ESearchCase::CaseSensitive));
    const int32 EmitterSourceGuardIndex = EditTypesContents.Find(
        TEXT("&& HasSystemCompileSources(*System)"), ESearchCase::CaseSensitive);
    const int32 EmitterRequestIndex = EmitterSourceGuardIndex != INDEX_NONE
        ? EditTypesContents.Find(
            TEXT("&& System->RequestCompile(bForce)"),
            ESearchCase::CaseSensitive,
            ESearchDir::FromStart,
            EmitterSourceGuardIndex)
        : INDEX_NONE;
    TestTrue(TEXT("emitter fan-out applies the same source guard before RequestCompile"),
        EmitterSourceGuardIndex != INDEX_NONE && EmitterRequestIndex > EmitterSourceGuardIndex);
    TestTrue(TEXT("shared request guard checks the system spawn source"),
        EditTypesContents.Contains(
            TEXT("SpawnScript->GetLatestSource() != nullptr"),
            ESearchCase::CaseSensitive));
    TestTrue(TEXT("shared request guard checks the system update source"),
        EditTypesContents.Contains(
            TEXT("UpdateScript->GetLatestSource() != nullptr"),
            ESearchCase::CaseSensitive));

    return true;
}
