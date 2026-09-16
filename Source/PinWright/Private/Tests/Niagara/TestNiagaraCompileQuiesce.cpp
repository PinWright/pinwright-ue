// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression test for the compile-while-live-instance editor kill.
//
// `compile: true` on every niagara.* edit verb routes through
// NiagaraEdit::FinalizeNiagaraEdit, which used to call UNiagaraSystem::RequestCompile(true)
// while FNiagaraSystemInstances of that system were still ticking on task-graph workers. The
// swapped-in bytecode then indexed data sets the old exec context never allocated and the
// VectorVM asserted (`DataSetIdx < ExecCtx->DataSets.Num()`) off the game thread. That is an
// appError, so the whole editor process died.
//
// The crash cannot be reproduced here: an appError would take the suite host down with it. So
// this asserts the GUARD instead - every UNiagaraComponent bound to the target system has its
// instance destroyed before the compile is requested, only when a compile was actually
// requested, and only for the targeted system.
//
// Probe: UNiagaraComponent::DestroyInstance() unconditionally broadcasts
// OnSystemInstanceChanged() as its last act, so counting that delegate is an exact observation
// of PinWrightNiagara::KillSystemInstances having run, and it needs no live simulation - which
// a transient fixture system could not provide anyway. A bare NewObject<UNiagaraSystem> has no
// UNiagaraScript::GetLatestSource(), so FinalizeNiagaraEdit takes its bare-system branch and
// never launches a real compile; the guard runs ahead of that branch and is what is measured.
//
// Counterfactual: without the KillSystemInstances call the counts stay at 0.
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"

#include "Handlers/Niagara/NiagaraEditTypes.h"
#include "Handlers/Niagara/NiagaraInstanceUtils.h"
#include "Tests/TestSkipReporting.h"

#include "NiagaraComponent.h"
#include "NiagaraEmitter.h"
#include "NiagaraSystem.h"
#include "UObject/Package.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FNiagaraCompileQuiesceTest,
    "PinWright.niagara.CompileQuiesce.KillsLiveInstancesBeforeRecompile",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraCompileQuiesceTest::RunTest(const FString& Parameters)
{
    UNiagaraSystem* TargetSystem = NewObject<UNiagaraSystem>(GetTransientPackage());
    UNiagaraSystem* OtherSystem = NewObject<UNiagaraSystem>(GetTransientPackage());
    UNiagaraComponent* TargetComponent = NewObject<UNiagaraComponent>(GetTransientPackage());
    UNiagaraComponent* OtherComponent = NewObject<UNiagaraComponent>(GetTransientPackage());
    if (!TargetSystem || !OtherSystem || !TargetComponent || !OtherComponent)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("niagara-fixture-construction-failed"),
            TEXT("NewObject returned null for one of the transient UNiagaraSystem / UNiagaraComponent probes."));
        return true;
    }

    TargetSystem->AddToRoot();
    OtherSystem->AddToRoot();
    TargetComponent->AddToRoot();
    OtherComponent->AddToRoot();

    // SetAsset destroys the (absent) previous instance and therefore broadcasts once itself;
    // bind the counters afterwards so only the guard's own kill is measured.
    TargetComponent->SetAsset(TargetSystem);
    OtherComponent->SetAsset(OtherSystem);

    int32 TargetKills = 0;
    int32 OtherKills = 0;
    const FDelegateHandle TargetHandle =
        TargetComponent->OnSystemInstanceChanged().AddLambda([&TargetKills]() { ++TargetKills; });
    const FDelegateHandle OtherHandle =
        OtherComponent->OnSystemInstanceChanged().AddLambda([&OtherKills]() { ++OtherKills; });

    ON_SCOPE_EXIT
    {
        // The lambdas capture stack state, so they must not outlive this frame.
        TargetComponent->OnSystemInstanceChanged().Remove(TargetHandle);
        OtherComponent->OnSystemInstanceChanged().Remove(OtherHandle);
        TargetComponent->RemoveFromRoot();
        OtherComponent->RemoveFromRoot();
        TargetSystem->RemoveFromRoot();
        OtherSystem->RemoveFromRoot();
    };

    FNiagaraResolvedTarget Target;
    Target.Asset = TargetSystem;
    Target.System = TargetSystem;
    Target.AssetPath = TEXT("/Transient/PinWrightNiagaraCompileQuiesce");
    Target.AssetKind = TEXT("NiagaraSystem");

    // 1. An edit that did not ask for a compile must leave running previews alone: quiescing is
    //    the price of the recompile, not a side effect every edit pays.
    {
        bool bCompiled = false;
        bool bSaved = false;
        NiagaraEdit::FinalizeNiagaraEdit(Target, FNiagaraEditOptions{false, false}, bCompiled, bSaved);
        TestEqual(TEXT("compile:false does not destroy the target system's instances"), TargetKills, 0);
        TestEqual(TEXT("compile:false does not destroy other systems' instances"), OtherKills, 0);
        TestFalse(TEXT("compile:false reports compiled=false"), bCompiled);
    }

    // 2. compile:true must quiesce every component bound to the target system before the
    //    recompile, and must not reach components bound to a different system.
    {
        bool bCompiled = false;
        bool bSaved = false;
        NiagaraEdit::FinalizeNiagaraEdit(Target, FNiagaraEditOptions{true, false}, bCompiled, bSaved);
        TestEqual(TEXT("compile:true destroys the target system's live instance first"), TargetKills, 1);
        TestEqual(TEXT("compile:true leaves components of other systems untouched"), OtherKills, 0);
        TestFalse(TEXT("compile-only does not claim completion without a wait"), bCompiled);
    }

    // 3. The emitter-scoped guard covers RequestCompileForEmitter's reach and nothing wider: an
    //    emitter no loaded system uses must not tear down unrelated previews.
    {
        UNiagaraEmitter* UnusedEmitter = NewObject<UNiagaraEmitter>(GetTransientPackage());
        if (UnusedEmitter)
        {
            UnusedEmitter->AddToRoot();
            PinWrightNiagara::KillSystemInstancesUsingEmitter(*UnusedEmitter, FGuid());
            TestEqual(TEXT("unused emitter does not quiesce the target system"), TargetKills, 1);
            TestEqual(TEXT("unused emitter does not quiesce other systems"), OtherKills, 0);
            UnusedEmitter->RemoveFromRoot();
        }
        else
        {
            PinWrightTestSkip::SkipAssertions(*this, TEXT("niagara-emitter-fixture-construction-failed"),
                TEXT("NewObject returned null for the transient UNiagaraEmitter probe; the emitter-scoped guard was not exercised."));
        }
    }

    return true;
}
