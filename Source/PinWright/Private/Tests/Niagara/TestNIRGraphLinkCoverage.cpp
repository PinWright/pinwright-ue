// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression test for F-nir-graph-connectivity-parity.
//
// Before this fix, NIR emission paired `AppendStack` (module-stack list) with
// `EmitScriptGraphScope` only on simulation-stage, event-handler, and GPU-compute
// scripts. The six main per-emitter and system scripts (EmitterSpawn/Update,
// ParticleSpawn/Update, SystemSpawn/Update) emitted only the stack summary —
// dropping the full node-link wiring that NIR is supposed to carry.
//
// This test seeds one module call into each of the six main script stacks via
// the same editor pipeline `AddModuleToStack` uses, then runs the production
// `NIRDecompiler::BuildNiagaraIrText` entry point and asserts:
//   * each of the six `graph <Usage> {` headers appears, and
//   * at least six `link ` lines exist (one per script, minimum — actual count
//     is higher in practice).
//
// Counterfactual: if the new `EmitScriptGraphScope` calls in
// `NIRDecompiler.cpp::AppendEmitterBody` / `EmitSystem` are reverted, the
// fixture's NIR text omits five of the six expected `graph <Usage> {` headers
// (only sim-stage/event/GPU paths emit graphs), so the per-usage assertions
// fail.

#include "Misc/AutomationTest.h"

#include "NIR/NIRDecompiler.h"
#include "Tests/Assets/NiagaraEditTestUtils.h"
#include "Tests/Niagara/TestNIRFixtures.h"
#include "Tests/TestSkipReporting.h"
#include "Utils/StringUtils.h"

#include "NiagaraEmitterHandle.h"
#include "NiagaraNodeFunctionCall.h"
#include "NiagaraScript.h"
#include "NiagaraSystem.h"
#include "UObject/UObjectGlobals.h"

namespace
{
    UNiagaraScript* LoadNirGraphLinkModuleScript()
    {
        // SpawnRate is a stock CPU module ship-asset whose authored graph contains
        // function-call nodes wired to the script's Output node — enough to give
        // any usage stack at least one `link` line once a module is added.
        return LoadObject<UNiagaraScript>(nullptr, TEXT("/Niagara/Modules/Spawn/SpawnRate.SpawnRate"));
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNiagaraNirGraphLinkCoverageTest,
    "PinWright.niagara.decompile_nir.GraphLinkCoverage",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraNirGraphLinkCoverageTest::RunTest(const FString& Parameters)
{
    UNiagaraScript* ModuleScript = LoadNirGraphLinkModuleScript();
    if (!ModuleScript)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("fixture-missing"),
            TEXT("SpawnRate module not available in this test build; skipping."));
        return true;
    }

    UNiagaraSystem* System = NIRTestFixtures::BuildEmptySystemWithEmitter(FName(TEXT("NirGraphLinkCoverage")));
    TestNotNull(TEXT("Fixture system created"), System);
    if (!System) return false;
    NiagaraEditTestUtils::FAuthorableSystemRoots Roots;
    Roots.System = System;
    for (const FNiagaraEmitterHandle& Handle : System->GetEmitterHandles())
    {
        Roots.Emitter = Handle.GetInstance().Emitter.Get();
        break;
    }

    // Seed one module call into each of the six main script stacks. Each module
    // gives the script's graph at least one function-call node with LinkedTo
    // edges to the Output node, so EmitGraphLinks emits at least one `link`
    // line per script once EmitScriptGraphScope is invoked.
    const ENiagaraScriptUsage Usages[] = {
        ENiagaraScriptUsage::EmitterSpawnScript,
        ENiagaraScriptUsage::EmitterUpdateScript,
        ENiagaraScriptUsage::ParticleSpawnScript,
        ENiagaraScriptUsage::ParticleUpdateScript,
        ENiagaraScriptUsage::SystemSpawnScript,
        ENiagaraScriptUsage::SystemUpdateScript,
    };
    for (ENiagaraScriptUsage Usage : Usages)
    {
        UNiagaraNodeFunctionCall* ModuleNode = NIRTestFixtures::AddModuleToStack(System, Usage, ModuleScript);
        TestNotNull(TEXT("Module added to stack for usage"), ModuleNode);
        if (!ModuleNode)
        {
            NIRTestFixtures::DestroyFixture(System);
            return false;
        }
    }

    const FNIRResult Result = NIRDecompiler::BuildNiagaraIrText(System);
    TestTrue(TEXT("NIR build succeeds"), Result.bSuccess);

    // Each of the six main scripts must now contribute its own `graph <Usage> {`
    // scope to the NIR text. Before the fix only sim-stage / event / GPU paths
    // emitted graphs, so five of these six checks would fail.
    TestTrue(TEXT("NIR contains EmitterSpawn graph scope"),
        Result.Text.Contains(TEXT("graph EmitterSpawn {")));
    TestTrue(TEXT("NIR contains EmitterUpdate graph scope"),
        Result.Text.Contains(TEXT("graph EmitterUpdate {")));
    TestTrue(TEXT("NIR contains ParticleSpawn graph scope"),
        Result.Text.Contains(TEXT("graph ParticleSpawn {")));
    TestTrue(TEXT("NIR contains ParticleUpdate graph scope"),
        Result.Text.Contains(TEXT("graph ParticleUpdate {")));
    TestTrue(TEXT("NIR contains SystemSpawn graph scope"),
        Result.Text.Contains(TEXT("graph SystemSpawn {")));
    TestTrue(TEXT("NIR contains SystemUpdate graph scope"),
        Result.Text.Contains(TEXT("graph SystemUpdate {")));

    // Floor at six: one `link` line per script minimum. The newline anchor
    // discriminates real link entries from any incidental `link` substring
    // appearing elsewhere in the text (none today, but the anchor future-proofs).
    const int32 LinkLineCount = PinWright::CountSubstring(Result.Text, TEXTVIEW("\nlink "));
    TestTrue(
        FString::Printf(TEXT("NIR contains at least six `link ` lines (got %d)"), LinkLineCount),
        LinkLineCount >= 6);

    NIRTestFixtures::DestroyFixture(System);
    return true;
}
