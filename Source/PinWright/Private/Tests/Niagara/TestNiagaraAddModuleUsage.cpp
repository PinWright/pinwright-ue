// Copyright (c) 2026 Alexander Penkin. MIT License.

// niagara.add_module must refuse a module whose ModuleUsageBitmask does not allow the target stack
// (B-niagara-add-module-ignores-usage-bitmask). SpawnRate allows EmitterSpawn/EmitterUpdate only.
// Counterfactual: without CheckModuleUsageAllowsStack in the AddModule branch, both refusal cases
// succeed and the ParticleUpdate chain grows by one.
#include "Misc/AutomationTest.h"
#include "Tests/Assets/NiagaraEditTestUtils.h"
#include "Tests/Niagara/TestNIRFixtures.h"
#include "Tests/TestSkipReporting.h"
#include "Tests/TestUtils.h"

#include "Dom/JsonObject.h"
#include "EdGraph/EdGraphPin.h"
#include "Handlers/Niagara/NiagaraGraphResetUtils.h"
#include "NiagaraEmitter.h"
#include "NiagaraEmitterHandle.h"
#include "NiagaraGraph.h"
#include "NiagaraNodeFunctionCall.h"
#include "NiagaraNodeOutput.h"
#include "NiagaraScript.h"
#include "NiagaraScriptSource.h"
#include "NiagaraSystem.h"

namespace NiagaraAddModuleUsageTestLocal
{
    const TCHAR* const FixtureEmitterName = TEXT("AddModuleUsageFixture");

    // Modules in one stack: walk the parameter-map chain backward from that stack's output node
    // (the order Niagara compiles from), counting function-call nodes.
    int32 CountModulesInStack(UNiagaraSystem& System, ENiagaraScriptUsage Usage)
    {
        for (const FNiagaraEmitterHandle& Handle : System.GetEmitterHandles())
        {
            const FVersionedNiagaraEmitterData* Data = Handle.GetEmitterData();
            const UNiagaraScriptSource* Source = Data ? Cast<UNiagaraScriptSource>(Data->GraphSource) : nullptr;
            UNiagaraNodeOutput* Output = Source && Source->NodeGraph
                ? Source->NodeGraph->FindEquivalentOutputNode(Usage, FGuid())
                : nullptr;
            if (!Output)
            {
                continue;
            }
            int32 Count = 0;
            UNiagaraNode* Current = Output;
            while (Current)
            {
                TArray<UEdGraphPin*> Pins;
                Current->GetInputPins(Pins);
                UEdGraphPin* MapPin = PinWrightNiagara::FindParameterMapPin(Pins);
                if (!MapPin || MapPin->LinkedTo.Num() != 1)
                {
                    break;
                }
                Current = Cast<UNiagaraNode>(MapPin->LinkedTo[0]->GetOwningNode());
                if (Cast<UNiagaraNodeFunctionCall>(Current))
                {
                    ++Count;
                }
            }
            return Count;
        }
        return INDEX_NONE;
    }

    TSharedPtr<FJsonObject> MakePayload(const UNiagaraSystem& System, const TCHAR* ScriptUsage)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("assetPath"), System.GetPathName());
        Payload->SetStringField(TEXT("emitter"), FixtureEmitterName);
        Payload->SetStringField(TEXT("modulePath"), TEXT("/Niagara/Modules/Emitter/SpawnRate.SpawnRate"));
        if (ScriptUsage)
        {
            Payload->SetStringField(TEXT("scriptUsage"), ScriptUsage);
        }
        Payload->SetBoolField(TEXT("compile"), false);
        return Payload;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNiagaraAddModuleUsageBitmaskTest,
    "PinWright.niagara.add_module.UsageBitmaskGatesTargetStack",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraAddModuleUsageBitmaskTest::RunTest(const FString& Parameters)
{
    using namespace NiagaraAddModuleUsageTestLocal;

    if (!LoadObject<UNiagaraScript>(nullptr, TEXT("/Niagara/Modules/Emitter/SpawnRate.SpawnRate")))
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("fixture-missing"),
            TEXT("SpawnRate module not available in this build; skipping."));
        return true;
    }
    UNiagaraSystem* System = NIRTestFixtures::BuildEmptySystemWithEmitter(FName(FixtureEmitterName));
    if (!TestNotNull(TEXT("Fixture system created"), System))
    {
        return false;
    }
    // Scoped ownership of the rooted fixture: unroots the system and its emitter on every exit.
    NiagaraEditTestUtils::FAuthorableSystemRoots Roots;
    Roots.System = System;
    for (const FNiagaraEmitterHandle& Handle : System->GetEmitterHandles())
    {
        Roots.Emitter = Handle.GetInstance().Emitter.Get();
        break;
    }

    const int32 ParticleUpdateBefore = CountModulesInStack(*System, ENiagaraScriptUsage::ParticleUpdateScript);

    // Refused: an explicit ParticleUpdate target the module's bitmask does not allow. The message
    // must name the module, the requested stack and the allowed stacks.
    {
        FTestResponseCapture Capture;
        InvokeHandlerWithCapture(TEXT("niagara.add_module"), MakePayload(*System, TEXT("ParticleUpdateScript")), Capture);
        TestFalse(TEXT("explicit ParticleUpdate add of SpawnRate is refused"), Capture.bSuccess);
        TestEqual(TEXT("refusal code"), Capture.ErrorCode, FString(TEXT("INCOMPATIBLE_STACK_GROUP")));
        TestTrue(TEXT("refusal names the module"), Capture.Message.Contains(TEXT("SpawnRate")));
        TestTrue(TEXT("refusal names the requested stack"), Capture.Message.Contains(TEXT("ParticleUpdateScript")));
        TestTrue(TEXT("refusal lists an allowed stack"), Capture.Message.Contains(TEXT("EmitterUpdateScript")));
    }

    // Refused: scriptUsage omitted. SpawnRate allows two stacks here, so nothing is derived and the
    // ParticleUpdate default must be refused rather than silently used.
    NiagaraEditTestUtils::InvokeExpectError(*this, TEXT("niagara.add_module"), MakePayload(*System, nullptr),
        TEXT("INCOMPATIBLE_STACK_GROUP"));

    TestEqual(TEXT("refused adds leave the ParticleUpdate stack unchanged"),
        CountModulesInStack(*System, ENiagaraScriptUsage::ParticleUpdateScript), ParticleUpdateBefore);

    // Allowed: EmitterUpdate is in SpawnRate's bitmask.
    const int32 EmitterUpdateBefore = CountModulesInStack(*System, ENiagaraScriptUsage::EmitterUpdateScript);
    FTestResponseCapture Allowed;
    NiagaraEditTestUtils::InvokeExpectSuccess(*this, TEXT("niagara.add_module"),
        MakePayload(*System, TEXT("EmitterUpdateScript")), Allowed);
    TestEqual(TEXT("allowed add lands in the EmitterUpdate stack"),
        CountModulesInStack(*System, ENiagaraScriptUsage::EmitterUpdateScript), EmitterUpdateBefore + 1);

    NIRTestFixtures::DestroyFixture(System);
    return true;
}
