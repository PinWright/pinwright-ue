// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression test for B-niagara-add-emitter-reshapes-handles-before-kill.
//
// niagara.remove_emitter killed live system instances BEFORE reshaping the emitter-handle array;
// niagara.add_emitter called AddEmitterHandle first and quiesced only around the compile, so
// `compile: false` reshaped the array - and rebuilt every UNiagaraNodeEmitter in the system graph -
// with nothing having stopped the instances running against it.
//
// The engine settles which ordering is right. A live FNiagaraEmitterInstance caches its POSITION in
// the handle array (FNiagaraSystemInstance::InitEmitters passes EmitterIdx to
// EmitterInstance->Init) and reads the handle back through it unchecked -
// `Sys->GetEmitterHandles()[EmitterIndex]`, NiagaraEmitterInstance.cpp:104-108 - and
// UNiagaraSystem::EmitterHandles is a TArray, so an append can reallocate the buffer that reference
// points into; and surviving indices are no reprieve, because the system-side execution order is
// resized from EmitterHandles.Num() and then indexes the instance's shorter Emitters array
// unchecked in Tick_Concurrent. Accordingly the engine's own add path opens with
// KillSystemInstances under the comment
// "Kill all system instances before modifying the emitter handle list to prevent accessing
// deleted data" (FNiagaraEditorUtilities::AddEmitterToSystem, NiagaraEditorUtilities.cpp:2141) -
// the same words its removal counterpart carries at :2199. There is no asymmetry to preserve.
//
// The crash itself cannot be reproduced here: it is a dangling read or a TArray range check on a
// task-graph worker, i.e. an appError that would take the suite host down. So this asserts the
// GUARD, and specifically its ORDER, which is the thing the ticket was about.
//
// Probe: UNiagaraComponent::DestroyInstance() unconditionally broadcasts OnSystemInstanceChanged()
// as its last act (the same probe PinWright.niagara.CompileQuiesce.* relies on), so the delegate is
// an exact observation of PinWrightNiagara::KillSystemInstances having run, and it needs no live
// simulation - which a fixture system could not provide anyway. Recording
// System->GetEmitterHandles().Num() from inside that broadcast is what turns a presence check into
// an ordering check: the count the kill sees is 0 only if it ran ahead of AddEmitterHandle.
//
// Counterfactual against the pre-fix handler, with compile:false: zero broadcasts, so the kill
// count is 0 and the observed handle count stays INDEX_NONE. Had the kill merely been moved after
// the append instead of before it, the count would read 1.
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"

#include "Tests/Assets/NiagaraEditTestUtils.h"
#include "Tests/TestSkipReporting.h"
#include "Tests/TestUtils.h"

#include "Dom/JsonObject.h"
#include "Misc/Guid.h"
#include "NiagaraComponent.h"
#include "NiagaraEmitter.h"
#include "NiagaraSystem.h"
#include "NiagaraSystemFactoryNew.h"
#include "UObject/Package.h"

namespace
{
    // A system in the shape niagara.create_system leaves behind: SystemSpawn / SystemUpdate scripts
    // sharing one node graph that already holds its two output nodes and no emitter nodes. The
    // handle list starts empty, which is what makes "0 handles when the kill fired" a meaningful
    // reading rather than an accident of the fixture.
    UNiagaraSystem* NewQuiesceProbeSystemFixture(FString& OutObjectPath)
    {
        const FString AssetName = FString::Printf(
            TEXT("NS_AddEmitterQuiesce_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
        const FString PackageName = FString::Printf(TEXT("/Game/PinWrightTests/%s"), *AssetName);
        UPackage* Package = CreatePackage(*PackageName);
        if (!Package)
        {
            return nullptr;
        }
        // Package-level RF_Transient keeps FPackageAutoSaver away from the fixture; the path stays
        // under /Game/ so the handler can still resolve it with LoadObject.
        Package->SetFlags(RF_Transient);

        UNiagaraSystem* System = NewObject<UNiagaraSystem>(
            Package, FName(*AssetName), RF_Public | RF_Standalone | RF_Transactional);
        if (!System)
        {
            return nullptr;
        }
        UNiagaraSystemFactoryNew::InitializeSystem(System, /*bCreateDefaultNodes=*/true);
        System->AddToRoot();
        OutObjectPath = FString::Printf(TEXT("%s.%s"), *PackageName, *AssetName);
        return System;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FNiagaraAddEmitterQuiescesBeforeReshapingHandlesTest,
    "PinWright.niagara.add_emitter.QuiescesBeforeReshapingHandles",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraAddEmitterQuiescesBeforeReshapingHandlesTest::RunTest(const FString& Parameters)
{
    FString SystemPath;
    UNiagaraSystem* System = NewQuiesceProbeSystemFixture(SystemPath);
    if (!TestNotNull(TEXT("create_system-shaped fixture built"), System))
    {
        return false;
    }

    FString EmitterPath;
    UNiagaraEmitter* Emitter = NiagaraEditTestUtils::NewTransientEmitter(EmitterPath);
    UNiagaraComponent* Component = NewObject<UNiagaraComponent>(GetTransientPackage());
    if (!Emitter || !Component)
    {
        System->RemoveFromRoot();
        if (Emitter)
        {
            Emitter->RemoveFromRoot();
        }
        PinWrightTestSkip::SkipAssertions(*this, TEXT("niagara-quiesce-fixture-unavailable"),
            TEXT("NewTransientEmitter or the UNiagaraComponent probe could not be constructed; ")
            TEXT("add_emitter's pre-reshape quiesce was not asserted."));
        return true;
    }
    Component->AddToRoot();

    // SetAsset destroys the (absent) previous instance and therefore broadcasts once itself; bind
    // the counters afterwards so only the handler's own kill is measured.
    Component->SetAsset(System);

    int32 Kills = 0;
    int32 HandlesWhenFirstKilled = INDEX_NONE;
    const FDelegateHandle KillHandle = Component->OnSystemInstanceChanged().AddLambda(
        [&Kills, &HandlesWhenFirstKilled, System]()
        {
            if (Kills == 0)
            {
                HandlesWhenFirstKilled = System->GetEmitterHandles().Num();
            }
            ++Kills;
        });

    ON_SCOPE_EXIT
    {
        // The lambda captures stack state, so it must not outlive this frame.
        Component->OnSystemInstanceChanged().Remove(KillHandle);
        Component->RemoveFromRoot();
        Emitter->RemoveFromRoot();
        System->RemoveFromRoot();
    };

    TestEqual(TEXT("the fixture starts with an empty handle array"),
        System->GetEmitterHandles().Num(), 0);

    FTestResponseCapture Capture;
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("systemPath"), SystemPath);
    Payload->SetStringField(TEXT("emitterPath"), EmitterPath);
    Payload->SetStringField(TEXT("name"), TEXT("QuiescedEmitter"));
    // compile:false is the whole point: the compile path already quiesced, so it is the
    // no-compile path that reshaped the array under a live instance.
    Payload->SetBoolField(TEXT("compile"), false);
    Payload->SetBoolField(TEXT("save"), false);

    NiagaraEditTestUtils::InvokeExpectSuccess(*this, TEXT("niagara.add_emitter"), Payload, Capture);

    // The mutation happened at all - otherwise the ordering below would be vacuous.
    TestEqual(TEXT("add_emitter appended the handle"), System->GetEmitterHandles().Num(), 1);

    // Presence: 0 against the pre-fix handler, which quiesced only when compiling.
    TestTrue(TEXT("compile:false still quiesces the system's live instances"), Kills >= 1);

    // Order: the load-bearing assertion. The kill must observe the array as it was BEFORE
    // AddEmitterHandle reallocated it, i.e. still empty. A kill moved to after the append reads 1.
    TestEqual(TEXT("the quiesce ran before the handle array was reshaped"),
        HandlesWhenFirstKilled, 0);

    return true;
}

// Regression test for B-niagara-mutation-scope-blanks-open-preview and
// E-niagara-mutation-result-no-quiesced-count.
//
// The kill above is correct and stays. What was missing is the disclosure: BeginEmitterMutationScope
// destroys every running instance of the asset - the one backing an open Niagara toolkit's preview
// viewport included - and no field in any mutation response said so, so from the other side the
// viewport went blank with no stated cause. MakeMutationResult now reports `quiescedInstances`.
//
// niagara.add_simulation_stage is the narrowest verb that exercises exactly the reported path:
// BeginEmitterMutationScope -> EndEmitterMutationScope -> MakeMutationResult, with `compile: false`
// so the compile path's own quiesce contributes nothing to the number under test.
//
// Counterfactual against the pre-fix handler: the field is absent, so the first assertion fails.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FNiagaraMutationScopeReportsQuiescedInstancesTest,
    "PinWright.niagara.mutation_scope.ReportsQuiescedInstanceCount",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraMutationScopeReportsQuiescedInstancesTest::RunTest(const FString& Parameters)
{
    FString SystemPath;
    UNiagaraSystem* System = nullptr;
    UNiagaraEmitter* SourceEmitter = nullptr;
    FName EmitterName;
    const bool bSetupOk = NiagaraEditTestUtils::MakeAuthorableSystem(SystemPath, System, SourceEmitter, EmitterName);
    NiagaraEditTestUtils::FAuthorableSystemRoots Roots{System, SourceEmitter};

    UNiagaraComponent* Component = bSetupOk ? NewObject<UNiagaraComponent>(GetTransientPackage()) : nullptr;
    if (!bSetupOk || !Component)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("niagara-quiesce-fixture-unavailable"),
            TEXT("MakeAuthorableSystem or the UNiagaraComponent probe could not be constructed; ")
            TEXT("the mutation response's quiescedInstances field was not asserted."));
        return true;
    }
    Component->AddToRoot();

    // SetAsset destroys the (absent) previous instance and therefore broadcasts once itself; bind
    // the counter afterwards so only the handler's own kill is measured.
    Component->SetAsset(System);

    int32 Kills = 0;
    const FDelegateHandle KillHandle = Component->OnSystemInstanceChanged().AddLambda(
        [&Kills]()
        {
            ++Kills;
        });

    ON_SCOPE_EXIT
    {
        // The lambda captures stack state, so it must not outlive this frame. Runs before
        // FAuthorableSystemRoots unroots the system the component points at.
        Component->OnSystemInstanceChanged().Remove(KillHandle);
        Component->RemoveFromRoot();
    };

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), SystemPath);
    Payload->SetStringField(TEXT("emitter"), EmitterName.ToString());
    Payload->SetBoolField(TEXT("compile"), false);
    Payload->SetBoolField(TEXT("save"), false);

    FTestResponseCapture Capture;
    if (!NiagaraEditTestUtils::InvokeExpectSuccess(*this, TEXT("niagara.add_simulation_stage"), Payload, Capture))
    {
        return false;
    }

    // The disclosure itself, and the whole of the pre-fix failure: the field did not exist.
    if (!TestTrue(TEXT("the mutation response reports quiescedInstances"),
            Capture.Result->HasTypedField<EJson::Number>(TEXT("quiescedInstances"))))
    {
        return false;
    }

    // Anchor: the scope's sweep did reach a component bound to this system, so the number below
    // is a reading of that sweep rather than of a path that never ran.
    TestTrue(TEXT("BeginEmitterMutationScope swept the component bound to the system"), Kills >= 1);

    // ...and what it counts is RUNNING instances stopped, not components swept. The probe is bound
    // but never registered, so it holds no FNiagaraSystemInstanceController and nothing of it was
    // stopped. An implementation counting every swept component instead reads 1 here.
    TestEqual(TEXT("a swept but idle component is not reported as a stopped instance"),
        static_cast<int32>(Capture.Result->GetNumberField(TEXT("quiescedInstances"))), 0);

    return true;
}
