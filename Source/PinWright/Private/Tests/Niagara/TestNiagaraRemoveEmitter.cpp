// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression test for niagara.remove_emitter handler.
// Exercises UNiagaraSystem::RemoveEmitterHandlesById directly on a transient system,
// and asserts that niagara.remove_emitter is registered in the dispatcher.
#include "Misc/AutomationTest.h"
#include "Tests/TestUtils.h"

#include "Misc/Guid.h"
#include "NiagaraEmitter.h"
#include "NiagaraEmitterHandle.h"
#include "NiagaraSystem.h"
#include "Tests/Assets/NiagaraEditTestUtils.h"
#include "UObject/Package.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FNiagaraRemoveEmitterTest,
    "PinWright.niagara.remove_emitter.Basic",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraRemoveEmitterTest::RunTest(const FString& Parameters)
{
    // Assert dispatcher registration — this fails if the handler block is reverted.
    TestTrue(TEXT("niagara.remove_emitter is registered"), IsRegistered(TEXT("niagara.remove_emitter")));

    // Build a transient UNiagaraSystem and two transient emitters by duplicating the saved
    // fixture asset. Duplicating a real PostLoaded emitter is the only honest way to obtain a
    // UNiagaraEmitter with a baked GraphSource / VersionData / UNiagaraScriptSource — calling
    // NewObject<UNiagaraEmitter>() yields an empty shell that trips
    // `ensure(GraphSource != nullptr)` inside FVersionedNiagaraEmitterData::PostLoad the moment
    // AddEmitterHandle StaticDuplicateObjects it, then null-derefs on the next line.
    FString SystemObjectPath;
    UNiagaraSystem* System = NiagaraEditTestUtils::NewTransientSystem(SystemObjectPath);
    if (!TestNotNull(TEXT("Transient UNiagaraSystem constructed"), System))
    {
        return false;
    }

    FString EmAObjectPath;
    FString EmBObjectPath;
    UNiagaraEmitter* EmA = NiagaraEditTestUtils::NewTransientEmitter(EmAObjectPath);
    UNiagaraEmitter* EmB = NiagaraEditTestUtils::NewTransientEmitter(EmBObjectPath);
    if (!TestNotNull(TEXT("EmA constructed"), EmA) || !TestNotNull(TEXT("EmB constructed"), EmB))
    {
        System->RemoveFromRoot();
        return false;
    }

    // AddEmitterHandle StaticDuplicateObjects the source emitter; both copies must carry a
    // valid GraphSource so PostLoad on the duplicated emitter doesn't trip the ensure.
    const FGuid VersionA = FGuid::NewGuid();
    const FGuid VersionB = FGuid::NewGuid();
    const FNiagaraEmitterHandle HandleA = System->AddEmitterHandle(*EmA, FName(TEXT("EmA")), VersionA);
    const FNiagaraEmitterHandle HandleB = System->AddEmitterHandle(*EmB, FName(TEXT("EmB")), VersionB);

    if (!TestTrue(TEXT("Both handles added"), System->GetEmitterHandles().Num() == 2))
    {
        EmA->RemoveFromRoot();
        EmB->RemoveFromRoot();
        System->RemoveFromRoot();
        return false;
    }

    const FGuid IdA = HandleA.GetId();

    // Exercise the engine API directly.
    TSet<FGuid> ToRemove;
    ToRemove.Add(IdA);
    System->RemoveEmitterHandlesById(ToRemove);

    TestEqual(TEXT("Remaining emitter count is 1"), System->GetEmitterHandles().Num(), 1);

    const TArray<FNiagaraEmitterHandle>& Remaining = System->GetEmitterHandles();
    TestEqual(TEXT("Surviving handle is EmB"), Remaining[0].GetName().ToString(), FString(TEXT("EmB")));

    // Cleanup.
    EmA->RemoveFromRoot();
    EmB->RemoveFromRoot();
    System->RemoveFromRoot();

    return true;
}
