// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression tests for B-niagara-authored-emitter-forces-inert.
//
// niagara.add_emitter only extended UNiagaraSystem::GetEmitterHandles(). It never created the
// UNiagaraNodeEmitter pair in the system's SystemSpawn / SystemUpdate graph that actually invokes
// an emitter's spawn and update scripts, so a system built with create_system + add_emitter
// compiled, validated strict-clean, saved, listed the emitter in every readback - and never
// spawned a particle, because nothing called it.
//
// Counterfactuals, both measured on a system in the shape niagara.create_system leaves behind
// (UNiagaraSystemFactoryNew::InitializeSystem, the same call that verb makes):
//
//   WiresEmitterIntoSystemGraph - against the pre-fix handler the system graph carries ZERO
//   UNiagaraNodeEmitter nodes after a successful add_emitter. The count is read back through
//   niagara.graph.get, i.e. off NiagaraDumpBuilder's walk of the graph, not off the code that
//   writes the nodes, so the check can contradict the writer.
//
//   UninvokedEmitterIsAnError - the handle is added directly through the engine API, reproducing
//   the exact state the pre-fix verb left. niagara.validate answered valid:true with zero errors
//   and zero warnings on that system; there was no signal anywhere separating it from a working
//   one. It must now be a hard error.
#include "Misc/AutomationTest.h"
#include "Tests/TestUtils.h"
#include "Tests/TestSkipReporting.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/Guid.h"
#include "NiagaraEmitter.h"
#include "NiagaraEmitterHandle.h"
#include "NiagaraSystem.h"
#include "NiagaraSystemFactoryNew.h"
#include "Tests/Assets/NiagaraEditTestUtils.h"
#include "UObject/Package.h"

namespace
{
    // A system in the shape niagara.create_system leaves behind: SystemSpawn / SystemUpdate
    // scripts sharing one node graph that already holds its two output nodes and NO emitter
    // nodes. That is the exact starting state this ticket was filed against, and it is why the
    // "before" count below is a clean zero rather than whatever a duplicated stock asset carries.
    UNiagaraSystem* NewCreateSystemShapedFixture(FString& OutObjectPath)
    {
        const FString AssetName = FString::Printf(
            TEXT("NS_EmitterWiring_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
        const FString PackageName = FString::Printf(TEXT("/Game/PinWrightTests/%s"), *AssetName);
        UPackage* Package = CreatePackage(*PackageName);
        if (!Package)
        {
            return nullptr;
        }
        // Package-level RF_Transient keeps FPackageAutoSaver away from the fixture; see the
        // rationale on NiagaraEditTestUtils::NewTransientSystem. The path stays under /Game/ so
        // the handlers can still resolve it with LoadObject.
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

    // Emitter-node census taken through niagara.graph.get - the dump-builder walk, a different
    // subsystem from the one that creates the nodes. Filtering to SystemSpawn returns exactly one
    // graph (the spawn and update scripts share it), so every emitter node in the system is
    // counted once. Returns INDEX_NONE when the response could not be read at all.
    int32 CountEmitterNodesInSystemGraph(FAutomationTestBase& Test, const FString& SystemPath)
    {
        FTestResponseCapture Capture;
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("assetPath"), SystemPath);
        Payload->SetStringField(TEXT("scriptUsage"), TEXT("SystemSpawn"));

        if (!InvokeHandlerWithCapture(TEXT("niagara.graph.get"), Payload, Capture)
            || !Capture.bSuccess
            || !Capture.Result.IsValid())
        {
            Test.AddError(TEXT("niagara.graph.get did not return a readable system graph"));
            return INDEX_NONE;
        }

        const TArray<TSharedPtr<FJsonValue>>* Graphs = nullptr;
        if (!Capture.Result->TryGetArrayField(TEXT("graphs"), Graphs) || !Graphs)
        {
            Test.AddError(TEXT("niagara.graph.get response carries no graphs array"));
            return INDEX_NONE;
        }

        int32 EmitterNodeCount = 0;
        for (const TSharedPtr<FJsonValue>& GraphValue : *Graphs)
        {
            const TSharedPtr<FJsonObject> Graph = GraphValue.IsValid() ? GraphValue->AsObject() : nullptr;
            const TArray<TSharedPtr<FJsonValue>>* Nodes = nullptr;
            if (!Graph.IsValid() || !Graph->TryGetArrayField(TEXT("nodes"), Nodes) || !Nodes)
            {
                continue;
            }
            for (const TSharedPtr<FJsonValue>& NodeValue : *Nodes)
            {
                const TSharedPtr<FJsonObject> Node = NodeValue.IsValid() ? NodeValue->AsObject() : nullptr;
                FString NodeClass;
                if (Node.IsValid()
                    && Node->TryGetStringField(TEXT("class"), NodeClass)
                    && NodeClass.EndsWith(TEXT("NiagaraNodeEmitter")))
                {
                    ++EmitterNodeCount;
                }
            }
        }
        return EmitterNodeCount;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FNiagaraAddEmitterWiresEmitterIntoSystemGraphTest,
    "PinWright.niagara.add_emitter.WiresEmitterIntoSystemGraph",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraAddEmitterWiresEmitterIntoSystemGraphTest::RunTest(const FString& Parameters)
{
    FString SystemPath;
    UNiagaraSystem* System = NewCreateSystemShapedFixture(SystemPath);
    if (!TestNotNull(TEXT("create_system-shaped fixture built"), System))
    {
        return false;
    }

    FString EmitterPath;
    UNiagaraEmitter* Emitter = NiagaraEditTestUtils::NewTransientEmitter(EmitterPath);
    if (!Emitter)
    {
        System->RemoveFromRoot();
        PinWrightTestSkip::SkipAssertions(*this, TEXT("niagara-fixture-emitter-unavailable"),
            TEXT("NewTransientEmitter returned null; system-graph emitter wiring not asserted"));
        return true;
    }

    // The state the ticket describes: handles can be added, but nothing in the graph calls them.
    TestEqual(TEXT("a create_system-shaped system starts with no emitter nodes"),
        CountEmitterNodesInSystemGraph(*this, SystemPath), 0);

    FTestResponseCapture Capture;
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("systemPath"), SystemPath);
    Payload->SetStringField(TEXT("emitterPath"), EmitterPath);
    Payload->SetStringField(TEXT("name"), TEXT("WiredEmitter"));
    // compile:false / save:false - compiling a synthetic system is what the fixture header warns
    // crashes inside FNiagaraCompilationGraphDigested::Digest, and the wiring is asserted off the
    // graph rather than off a compile result anyway.
    Payload->SetBoolField(TEXT("compile"), false);
    Payload->SetBoolField(TEXT("save"), false);

    if (NiagaraEditTestUtils::InvokeExpectSuccess(*this, TEXT("niagara.add_emitter"), Payload, Capture))
    {
        // TryGet rather than Get so an absent field fails the assertion instead of dereferencing
        // an invalid JSON value and taking the suite host with it.
        double EmitterCount = -1.0;
        double InvokedEmitters = -1.0;
        double RebuiltNodes = -1.0;
        Capture.Result->TryGetNumberField(TEXT("emitterCount"), EmitterCount);
        Capture.Result->TryGetNumberField(TEXT("emittersInvokedBySystemGraph"), InvokedEmitters);
        Capture.Result->TryGetNumberField(TEXT("emitterNodesRebuilt"), RebuiltNodes);

        TestEqual(TEXT("one emitter handle after add"), static_cast<int32>(EmitterCount), 1);
        // The two fields that separate an emitter that will run from one that will not.
        TestEqual(TEXT("the system graph invokes every handle it now has"),
            static_cast<int32>(InvokedEmitters), 1);
        TestEqual(TEXT("a spawn/update emitter-node pair was rebuilt"),
            static_cast<int32>(RebuiltNodes), 2);
    }

    // The load-bearing assertion, read back through a different subsystem: the graph itself now
    // carries the pair. This is 0 against the pre-fix handler.
    TestEqual(TEXT("the system graph carries the emitter's spawn and update nodes"),
        CountEmitterNodesInSystemGraph(*this, SystemPath), 2);

    Emitter->RemoveFromRoot();
    System->RemoveFromRoot();
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FNiagaraValidateUninvokedEmitterIsAnErrorTest,
    "PinWright.niagara.validate.UninvokedEmitterIsAnError",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraValidateUninvokedEmitterIsAnErrorTest::RunTest(const FString& Parameters)
{
    FString SystemPath;
    UNiagaraSystem* System = NewCreateSystemShapedFixture(SystemPath);
    if (!TestNotNull(TEXT("create_system-shaped fixture built"), System))
    {
        return false;
    }

    FString EmitterPath;
    UNiagaraEmitter* Emitter = NiagaraEditTestUtils::NewTransientEmitter(EmitterPath);
    if (!Emitter)
    {
        System->RemoveFromRoot();
        PinWrightTestSkip::SkipAssertions(*this, TEXT("niagara-fixture-emitter-unavailable"),
            TEXT("NewTransientEmitter returned null; validate's uninvoked-emitter verdict not asserted"));
        return true;
    }

    // Reproduce the pre-fix state directly through the engine API rather than through the verb:
    // a handle with no emitter node behind it. This is what every system built with
    // create_system + add_emitter looked like, and what validate used to call healthy.
    System->AddEmitterHandle(*Emitter, FName(TEXT("Uninvoked")), Emitter->GetExposedVersion().VersionGuid);
    TestEqual(TEXT("the fixture has one emitter handle"), System->GetEmitterHandles().Num(), 1);
    TestEqual(TEXT("and no emitter node invoking it"),
        CountEmitterNodesInSystemGraph(*this, SystemPath), 0);

    FTestResponseCapture Capture;
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), SystemPath);
    Payload->SetStringField(TEXT("level"), TEXT("basic"));

    if (NiagaraEditTestUtils::InvokeExpectSuccess(*this, TEXT("niagara.validate"), Payload, Capture))
    {
        // Pre-fix: valid:true, errors:[], warnings:[]. The emitter could never run and nothing
        // said so - even at level:strict, which escalates only NO_EMITTERS / DISABLED_EMITTER /
        // NO_RENDERERS and has nothing to say about an emitter the system graph never calls.
        TestFalse(TEXT("an emitter the system graph never invokes makes the system invalid"),
            Capture.Result->GetBoolField(TEXT("valid")));
        TestTrue(TEXT("errors name the uninvoked emitter"),
            JsonArrayHasObjectWithStringField(Capture.Result, TEXT("errors"), TEXT("code"), TEXT("EMITTER_NOT_IN_SYSTEM_GRAPH")));
    }

    Emitter->RemoveFromRoot();
    System->RemoveFromRoot();
    return true;
}
