// Copyright (c) 2026 Alexander Penkin. MIT License.

// Behavioral regression coverage for B-effect-spawn-false-success. The handler used to accept any
// loadable UObject, ignore autoDestroy and a missing requested parent, and report actor creation as
// success without measuring Niagara compile or activation state.

#include "Misc/AutomationTest.h"

#include "Compat/EngineVersionCompat.h"
#include "Handlers/ErrorCodes.h"
#include "Handlers/Niagara/NiagaraCompileWait.h"
#include "Handlers/VFX/EffectSpawnTestHooks.h"
#include "Tests/Assets/NiagaraEditTestUtils.h"
#include "Tests/TestSkipReporting.h"
#include "Tests/TestUtils.h"
#include "Tests/TestWorldUtils.h"

#include "Dom/JsonObject.h"
#include "Components/SceneComponent.h"
#include "Editor.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "Misc/Guid.h"
#include "Misc/ScopeExit.h"
#include "NiagaraActor.h"
#include "NiagaraComponent.h"
#include "NiagaraEmitterHandle.h"
#include "NiagaraScript.h"
#include "NiagaraSystem.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UnrealType.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace PinWrightEffectSpawnStateTest
{
    UNiagaraScript* FindOwnedParticleUpdateScript(UNiagaraSystem& System)
    {
        for (const FNiagaraEmitterHandle& Handle : System.GetEmitterHandles())
        {
            const FVersionedNiagaraEmitterData* EmitterData = Handle.GetEmitterData();
            UNiagaraScript* Script = EmitterData ? EmitterData->UpdateScriptProps.Script : nullptr;
            if (Script && Script->IsIn(&System))
            {
                return Script;
            }
        }
        return nullptr;
    }

    int32 CountWorldComponentsUsingSystem(UWorld& World, const UNiagaraSystem* System)
    {
        int32 Count = 0;
        for (TActorIterator<AActor> It(&World); It; ++It)
        {
            TArray<UNiagaraComponent*> Components;
            It->GetComponents<UNiagaraComponent>(Components);
            for (const UNiagaraComponent* Component : Components)
            {
                Count += Component && Component->GetAsset() == System ? 1 : 0;
            }
        }
        return Count;
    }

    ANiagaraActor* FindNiagaraActorByLabel(UWorld& World, const FString& Label)
    {
        for (TActorIterator<ANiagaraActor> It(&World); It; ++It)
        {
            if (It->GetActorLabel().Equals(Label, ESearchCase::CaseSensitive))
            {
                return *It;
            }
        }
        return nullptr;
    }

    TSharedPtr<FJsonObject> MakeSpawnPayload(
        const FString& SystemPath,
        const FString& ActorLabel)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("systemPath"), SystemPath);
        Payload->SetStringField(TEXT("name"), ActorLabel);
        return Payload;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEffectSpawnNiagaraReportsMeasuredStateTest,
    "PinWright.effect.spawn_niagara.MeasuredStateAndCompileFailure",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FEffectSpawnNiagaraReportsMeasuredStateTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightEffectSpawnStateTest;

    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("No editor world is open; effect.spawn_niagara behavior was not asserted"));
        return true;
    }

    FString SystemPath;
    FString WrongAssetPath;
    // Both fixtures are RF_Standalone, so releasing the TStrongObjectPtrs is not enough to reclaim
    // them; detach them from their packages too. Declared before the owners so it runs after them.
    ON_SCOPE_EXIT
    {
        CleanupTestAsset(SystemPath);
        CleanupTestAsset(WrongAssetPath);
    };

    TStrongObjectPtr<UNiagaraSystem> SystemOwner(
        NiagaraEditTestUtils::DuplicateFixtureSystemWithEmitters(
            TEXT("NS_EffectSpawn"), SystemPath));
    UNiagaraSystem* System = SystemOwner.Get();
    if (!System)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("niagara-fixture-system-unavailable"),
            FString::Printf(TEXT("Could not duplicate '%s'"),
                NiagaraEditTestUtils::FixtureSystemAssetPath));
        return true;
    }

    FScopedEditorWorldActorGuard ActorGuard;

    const FString WrongAssetName = NiagaraEditTestUtils::MakeAssetName(TEXT("NotNiagara"));
    const FString WrongAssetPackageName = FString::Printf(
        TEXT("/Game/PinWrightTests/%s_Package"), *WrongAssetName);
    UPackage* WrongAssetPackage = CreatePackage(*WrongAssetPackageName);
    WrongAssetPackage->SetFlags(RF_Transient);
    TStrongObjectPtr<UTexture2D> WrongAsset(NewObject<UTexture2D>(
        WrongAssetPackage, FName(*WrongAssetName), RF_Public | RF_Standalone | RF_Transient));
    WrongAssetPath = WrongAsset->GetPathName();
    const FString WrongTypeLabel = FString::Printf(TEXT("PW_EffectWrongType_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8));
    FTestResponseCapture WrongType;
    TestTrue(TEXT("effect.spawn_niagara accepts the wrong-type probe"),
        InvokeHandlerWithCapture(TEXT("effect.spawn_niagara"),
            MakeSpawnPayload(WrongAssetPath, WrongTypeLabel), WrongType));
    TestFalse(TEXT("a non-Niagara asset cannot report spawn success"), WrongType.bSuccess);
    TestEqual(TEXT("wrong asset class uses INVALID_ASSET_TYPE"), WrongType.ErrorCode,
        FString(ErrorCodes::ERR_INVALID_ASSET_TYPE));
    TestNull(TEXT("wrong asset class creates no actor"),
        FindNiagaraActorByLabel(*World, WrongTypeLabel));

    const FString MissingParentLabel = FString::Printf(TEXT("PW_MissingParent_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8));
    const FString MissingParentSpawnLabel = FString::Printf(TEXT("PW_EffectNoParent_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8));
    TSharedPtr<FJsonObject> MissingParentPayload =
        MakeSpawnPayload(SystemPath, MissingParentSpawnLabel);
    MissingParentPayload->SetStringField(TEXT("attachToActor"), MissingParentLabel);
    FTestResponseCapture MissingParent;
    TestTrue(TEXT("effect.spawn_niagara accepts the missing-parent probe"),
        InvokeHandlerWithCapture(TEXT("effect.spawn_niagara"),
            MissingParentPayload, MissingParent));
    TestFalse(TEXT("a requested missing parent cannot report spawn success"),
        MissingParent.bSuccess);
    TestEqual(TEXT("missing requested parent uses ACTOR_NOT_FOUND"), MissingParent.ErrorCode,
        FString(ErrorCodes::ERR_ACTOR_NOT_FOUND));
    TestNull(TEXT("missing requested parent creates no actor"),
        FindNiagaraActorByLabel(*World, MissingParentSpawnLabel));

    UNiagaraScript* FixtureCompileScript = System->GetSystemSpawnScript();
    TestNotNull(TEXT("transient system has a compilable system spawn script"),
        FixtureCompileScript);
    if (!FixtureCompileScript || !FixtureCompileScript->IsCompilable()
        || !FixtureCompileScript->GetLatestSource())
    {
        AddError(TEXT("The saved Niagara fixture did not retain a compilable source after duplication."));
        return false;
    }

    FixtureCompileScript->InvalidateCompileResults(
        TEXT("PinWright effect.spawn_niagara activation test"));
    const bool bCompileRequested = System->RequestCompile(/*bForce=*/true);
    TestTrue(TEXT("transient system compile was requested"), bCompileRequested);
    if (!bCompileRequested)
    {
        return false;
    }

    const PinWrightNiagara::FCompileWaitOutcome FixtureCompileWait =
        PinWrightNiagara::WaitForSystemCompile(*System, bCompileRequested);
    TestFalse(TEXT("transient system compile wait did not time out"),
        FixtureCompileWait.bTimedOut);
    TestFalse(TEXT("transient system compile completed before spawn"),
        FixtureCompileWait.bOutstanding);
    if (FixtureCompileWait.bTimedOut || FixtureCompileWait.bOutstanding)
    {
        return false;
    }

    const FString ParentLabel = FString::Printf(TEXT("PW_EffectSpawnParent_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8));
    AActor* Parent = World->SpawnActor<AActor>();
    if (!Parent)
    {
        AddError(TEXT("Failed to spawn the attachment parent fixture."));
        return false;
    }
    USceneComponent* ParentRoot = NewObject<USceneComponent>(Parent, TEXT("AttachmentRoot"));
    if (!ParentRoot)
    {
        AddError(TEXT("Failed to create the attachment parent root fixture."));
        return false;
    }
    Parent->AddInstanceComponent(ParentRoot);
    Parent->SetRootComponent(ParentRoot);
    ParentRoot->RegisterComponent();
    Parent->SetActorLabel(ParentLabel);

    const FString SpawnLabel = FString::Printf(TEXT("PW_EffectSpawn_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8));
    TSharedPtr<FJsonObject> SuccessPayload = MakeSpawnPayload(SystemPath, SpawnLabel);
    SuccessPayload->SetBoolField(TEXT("autoDestroy"), true);
    SuccessPayload->SetStringField(TEXT("attachToActor"), ParentLabel);

    FTestResponseCapture Success;
    TestTrue(TEXT("effect.spawn_niagara handler found"),
        InvokeHandlerWithCapture(TEXT("effect.spawn_niagara"), SuccessPayload, Success));
    TestTrue(FString::Printf(
        TEXT("compiled transient system spawn succeeds (errorCode='%s', message='%s')"),
        *Success.ErrorCode, *Success.Message), Success.bSuccess);
    if (!Success.bSuccess || !Success.Result.IsValid())
    {
        return false;
    }

    ANiagaraActor* Spawned = FindNiagaraActorByLabel(*World, SpawnLabel);
    TestNotNull(TEXT("success leaves the requested Niagara actor in the world"), Spawned);
    UNiagaraComponent* Component = Spawned ? Spawned->GetNiagaraComponent() : nullptr;
    TestNotNull(TEXT("spawned actor owns a Niagara component"), Component);
    if (!Component)
    {
        return false;
    }

    TestTrue(TEXT("the requested system was assigned"), Component->GetAsset() == System);
    TestTrue(TEXT("the component actually became active"), Component->IsActive());
    TestTrue(TEXT("the requested parent was applied"), Spawned->GetAttachParentActor() == Parent);
    const FBoolProperty* AutoDestroyProperty =
        FindFProperty<FBoolProperty>(UNiagaraComponent::StaticClass(), TEXT("bAutoDestroy"));
    TestNotNull(TEXT("auto-destroy state is reflectable"), AutoDestroyProperty);
    if (AutoDestroyProperty)
    {
        TestTrue(TEXT("autoDestroy=true was applied"),
            AutoDestroyProperty->GetPropertyValue_InContainer(Component));
    }

    bool bReportedActive = false;
    bool bReportedAutoDestroy = false;
    bool bReportedAttached = false;
    FString CompileStatus;
    TestTrue(TEXT("response reports measured active"),
        Success.Result->TryGetBoolField(TEXT("active"), bReportedActive));
    TestTrue(TEXT("reported active matches component"),
        bReportedActive == Component->IsActive());
    TestTrue(TEXT("response reports measured autoDestroy"),
        Success.Result->TryGetBoolField(TEXT("autoDestroy"), bReportedAutoDestroy));
    TestTrue(TEXT("response reports the attachment"),
        Success.Result->TryGetBoolField(TEXT("attached"), bReportedAttached));
    TestTrue(TEXT("response reports a passed compile verdict"),
        Success.Result->TryGetStringField(TEXT("compileStatus"), CompileStatus));
    TestTrue(TEXT("success state is the requested state"),
        bReportedAutoDestroy && bReportedAttached && CompileStatus == TEXT("passed"));

    const int32 ComponentCountBeforeInactiveProbe = CountWorldComponentsUsingSystem(*World, System);
    const FString InactiveLabel = FString::Printf(TEXT("PW_EffectSpawnInactive_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8));
    bool& bForceInactive = PinWrightEffectSpawnTestHooks::ForceInactiveAfterActivation();
    bForceInactive = true;
    FTestResponseCapture Inactive;
    const bool bInactiveHandlerFound = InvokeHandlerWithCapture(TEXT("effect.spawn_niagara"),
        MakeSpawnPayload(SystemPath, InactiveLabel), Inactive);
    bForceInactive = false;
    TestTrue(TEXT("effect.spawn_niagara accepts the forced-inactive probe"), bInactiveHandlerFound);
    TestFalse(TEXT("an inactive Niagara component cannot report spawn success"), Inactive.bSuccess);
    TestEqual(TEXT("inactive component uses EFFECT_NOT_ACTIVE"), Inactive.ErrorCode,
        FString(ErrorCodes::ERR_EFFECT_NOT_ACTIVE));
    bool bDestroyedAfterFailure = false;
    TestTrue(TEXT("inactive response reports cleanup"), Inactive.Result.IsValid() &&
        Inactive.Result->TryGetBoolField(TEXT("destroyedAfterFailure"), bDestroyedAfterFailure));
    TestTrue(TEXT("inactive response confirms actor destruction"), bDestroyedAfterFailure);
    TestNull(TEXT("inactive failure leaves no actor"),
        FindNiagaraActorByLabel(*World, InactiveLabel));
    TestEqual(TEXT("inactive failure leaves no Niagara component"),
        CountWorldComponentsUsingSystem(*World, System), ComponentCountBeforeInactiveProbe);

    UNiagaraScript* BrokenScript = FindOwnedParticleUpdateScript(*System);
    TestNotNull(TEXT("transient system has an owned particle update script"), BrokenScript);
    if (!BrokenScript)
    {
        return false;
    }
    const ENiagaraScriptCompileStatus OriginalStatus =
        BrokenScript->GetVMExecutableData().LastCompileStatus;
    BrokenScript->GetVMExecutableData().LastCompileStatus =
        ENiagaraScriptCompileStatus::NCS_Error;
    ON_SCOPE_EXIT
    {
        BrokenScript->GetVMExecutableData().LastCompileStatus = OriginalStatus;
    };

    const FString RejectedLabel = FString::Printf(TEXT("PW_EffectSpawnRejected_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8));
    FTestResponseCapture Rejected;
    TestTrue(TEXT("effect.spawn_niagara accepts the failed-compile probe"),
        InvokeHandlerWithCapture(TEXT("effect.spawn_niagara"),
            MakeSpawnPayload(SystemPath, RejectedLabel), Rejected));
    TestFalse(TEXT("a failed Niagara compile cannot report spawn success"), Rejected.bSuccess);
    TestEqual(TEXT("failed compile uses SYSTEM_NOT_COMPILED"), Rejected.ErrorCode,
        FString(ErrorCodes::ERR_SYSTEM_NOT_COMPILED));
    TestNull(TEXT("failed compile creates no actor"),
        FindNiagaraActorByLabel(*World, RejectedLabel));
    if (Rejected.Result.IsValid())
    {
        FString RejectedCompileStatus;
        TestTrue(TEXT("compile failure response reports measured status"),
            Rejected.Result->TryGetStringField(TEXT("compileStatus"), RejectedCompileStatus));
        TestEqual(TEXT("compile failure status is failed"), RejectedCompileStatus,
            FString(TEXT("failed")));
    }

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEffectSpawnNiagaraDrainsQueuedCompileTest,
    "PinWright.effect.spawn_niagara.QueuedCompileRequestIsDrained",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FEffectSpawnNiagaraDrainsQueuedCompileTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightEffectSpawnStateTest;

#if UE_VERSION_OLDER_THAN(5, 4, 0)
    // UNiagaraSystem::SetCompileForEdit / GetCompileForEdit arrived in UE 5.4; on 5.3
    // bCompileForEdit is a private bitfield with no accessor, so the one public lever that parks a
    // compile request without launching it does not exist. Every other route into that state
    // (RequestCompile, PollForCompilationComplete) either starts the compile or flushes it, which
    // is exactly the condition this test needs to observe unstarted. Marked rather than silently
    // green: a skipped assertion and a passed one are otherwise indistinguishable in the totals.
    PinWrightTestSkip::SkipAssertions(*this, TEXT("queued-compile-lever-requires-5.4"),
        TEXT("UNiagaraSystem::SetCompileForEdit arrived in UE 5.4; on 5.3 a queued-but-unstarted "
             "compile request cannot be parked, so the drain cannot be asserted."));
    return true;
#else
    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("No editor world is open; the spawn gate's compile drain was not asserted"));
        return true;
    }

    FString SystemPath;
    ON_SCOPE_EXIT
    {
        CleanupTestAsset(SystemPath);
    };

    TStrongObjectPtr<UNiagaraSystem> SystemOwner(
        NiagaraEditTestUtils::DuplicateFixtureSystemWithEmitters(
            TEXT("NS_EffectSpawn"), SystemPath));
    UNiagaraSystem* System = SystemOwner.Get();
    if (!System)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("niagara-fixture-system-unavailable"),
            FString::Printf(TEXT("Could not duplicate '%s'"),
                NiagaraEditTestUtils::FixtureSystemAssetPath));
        return true;
    }

    FScopedEditorWorldActorGuard ActorGuard;

    // Settle whatever duplication left queued, so the request asserted below is this test's.
    PinWrightNiagara::WaitForSystemCompile(*System, /*bMayFlushRequestCompile=*/true);

    // SetCompileForEdit is the one public lever that parks a compile request without launching
    // it: it sets the engine's RequestCompileStatus, which is what on-demand compilation also
    // leaves behind at load. The system then reports a request outstanding while nothing is
    // actively compiling, and nothing but a flush ever clears it.
    System->SetCompileForEdit(!System->GetCompileForEdit());
    if (!PinWrightNiagara::HasPendingCompileWork(*System))
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("niagara-queued-request-not-observable"),
            TEXT("the host did not park a queued compile request on the fixture"));
        return true;
    }

    // The argument the gate used to pass: observe only, never start the queued compile. It leaves
    // the request exactly where it was, which is why the refusal could never clear.
    const PinWrightNiagara::FCompileWaitOutcome Observed =
        PinWrightNiagara::WaitForSystemCompile(*System, /*bMayFlushRequestCompile=*/false);
    TestFalse(TEXT("nothing is actively compiling on the fixture"), Observed.bOutstanding);
    TestFalse(TEXT("an observation-only wait never enters the drain loop"), Observed.bWaited);
    TestTrue(TEXT("a queued request survives an observation-only wait"),
        PinWrightNiagara::HasPendingCompileWork(*System));

    const FString SpawnLabel = FString::Printf(TEXT("PW_EffectSpawnQueued_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8));
    FTestResponseCapture Queued;
    TestTrue(TEXT("effect.spawn_niagara accepts the queued-compile probe"),
        InvokeHandlerWithCapture(TEXT("effect.spawn_niagara"),
            MakeSpawnPayload(SystemPath, SpawnLabel), Queued));
    TestNotEqual(TEXT("a queued compile request is not a permanent refusal"),
        Queued.ErrorCode, FString(ErrorCodes::ERR_SYSTEM_NOT_COMPILED));
    TestTrue(FString::Printf(
        TEXT("the gate drains the queued request and spawns (errorCode='%s', message='%s')"),
        *Queued.ErrorCode, *Queued.Message), Queued.bSuccess);
    TestFalse(TEXT("the gate leaves no queued compile request behind"),
        PinWrightNiagara::HasPendingCompileWork(*System));
    TestNotNull(TEXT("the drained spawn leaves its actor in the world"),
        FindNiagaraActorByLabel(*World, SpawnLabel));

    if (Queued.Result.IsValid())
    {
        // The refusal and the reported state must come from one predicate.
        bool bReportedOutstanding = true;
        TestTrue(TEXT("response reports the measured outstanding state"),
            Queued.Result->TryGetBoolField(TEXT("outstandingCompilationRequests"),
                bReportedOutstanding));
        TestFalse(TEXT("reported outstanding state matches the readiness predicate"),
            bReportedOutstanding);
        FString ReportedStatus;
        TestTrue(TEXT("response reports a compile status"),
            Queued.Result->TryGetStringField(TEXT("compileStatus"), ReportedStatus));
        TestNotEqual(TEXT("a drained request is not reported outstanding"),
            ReportedStatus, FString(TEXT("outstanding")));
    }

    return true;
#endif // UE_VERSION_OLDER_THAN(5, 4, 0)
}

#endif // WITH_DEV_AUTOMATION_TESTS
