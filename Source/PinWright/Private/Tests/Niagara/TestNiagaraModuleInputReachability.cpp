// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression test for B-niagara-module-input-inert-when-static-switch-gates-it: a module input
// whose only consumer is a static switch's "if true" pin, on a switch that reads false, is dead
// code — but the write succeeded, the pin holds the value, and every published signal called it
// configuration.
//
// Fixture is the ticket's own measured shape: the stock SubUVAnimation module, whose
// `Start Frame Range Override` terminates only on the `UseStartFrame` switch's `if true` pins
// (through a reroute and an int->float convert), with `UseStartFrame` false out of the box.
// Driven through NiagaraDumpBuilder::BuildModuleInputsJson, the shared builder behind
// niagara.inspect's stack aspect and the niagara_stack.json dump.
//
// The counterfactual is inside the test: flipping the caller pin that holds the switch value
// must flip `reachable`, which proves the field tracks the gate rather than being a constant.

#include "Misc/AutomationTest.h"
#include "Tests/Assets/NiagaraEditTestUtils.h"
#include "Tests/Niagara/TestNIRFixtures.h"
#include "Tests/TestSkipReporting.h"
#include "Tests/TestUtils.h"

#include "Handlers/Niagara/NiagaraDumpBuilder.h"
#include "Handlers/Niagara/NiagaraEditTypes.h"

#include "EdGraph/EdGraphPin.h"
#include "NiagaraCommon.h"
#include "NiagaraEmitterHandle.h"
#include "NiagaraNodeFunctionCall.h"
#include "NiagaraNodeStaticSwitch.h"
#include "NiagaraScript.h"
#include "NiagaraSystem.h"

namespace
{
    const TCHAR* const GatedInputName = TEXT("Start Frame Range Override");
    const TCHAR* const GatingSwitchName = TEXT("UseStartFrame");

    TSharedPtr<FJsonObject> ReadModuleInput(UNiagaraNodeFunctionCall* ModuleNode, const TCHAR* InputName)
    {
        TSharedPtr<FJsonObject> Wrapper = MakeShared<FJsonObject>();
        Wrapper->SetArrayField(TEXT("moduleInputs"), NiagaraDumpBuilder::BuildModuleInputsJson(ModuleNode));
        return JsonArrayFindObjectByStringField(Wrapper, TEXT("moduleInputs"), TEXT("name"), InputName);
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FNiagaraModuleInputStaticSwitchGateTest,
    "PinWright.niagara.dump.ModuleInputStaticSwitchGate",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraModuleInputStaticSwitchGateTest::RunTest(const FString& Parameters)
{
    const TCHAR* ModulePath = TEXT("/Niagara/Modules/Update/SubUV/V2/SubUVAnimation.SubUVAnimation");
    UNiagaraScript* ModuleScript = LoadObject<UNiagaraScript>(nullptr, ModulePath);
    if (!ModuleScript)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("niagara_fixture_assets_absent"),
            FString::Printf(TEXT("could not load '%s'"), ModulePath));
        return true;
    }

    UNiagaraSystem* System = NIRTestFixtures::BuildEmptySystemWithEmitter(FName(TEXT("ModuleInputReachability")));
    if (!System)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("niagara_fixture_unavailable"),
            TEXT("BuildEmptySystemWithEmitter did not produce a transient Niagara system"));
        return true;
    }
    // The fixture factory roots both the system and its emitter; this owns that pin so both are
    // released on the skip returns below as well as on the normal exit.
    NiagaraEditTestUtils::FAuthorableSystemRoots Roots;
    Roots.System = System;
    for (const FNiagaraEmitterHandle& Handle : System->GetEmitterHandles())
    {
        Roots.Emitter = Handle.GetInstance().Emitter.Get();
        break;
    }

    UNiagaraNodeFunctionCall* ModuleNode = NIRTestFixtures::AddModuleToStack(
        System,
        ENiagaraScriptUsage::ParticleUpdateScript,
        ModuleScript);
    if (!ModuleNode)
    {
        NIRTestFixtures::DestroyFixture(System);
        PinWrightTestSkip::SkipAssertions(*this, TEXT("niagara_fixture_unavailable"),
            TEXT("SubUVAnimation could not be added to the particle update stack"));
        return true;
    }

    // The switch is false out of the box, so the input's branch is not the one compiled.
    {
        TSharedPtr<FJsonObject> Gated = ReadModuleInput(ModuleNode, GatedInputName);
        if (TestTrue(TEXT("moduleInputs lists the switch-gated input"), Gated.IsValid()))
        {
            bool bReachable = true;
            TestTrue(TEXT("A gated input publishes reachable"), Gated->TryGetBoolField(TEXT("reachable"), bReachable));
            TestFalse(TEXT("Start Frame Range Override is unreachable while UseStartFrame is false"), bReachable);

            const TSharedPtr<FJsonObject>* GatedBy = nullptr;
            if (TestTrue(TEXT("An unreachable input names its gate"), Gated->TryGetObjectField(TEXT("gatedBy"), GatedBy) && GatedBy))
            {
                FString SwitchName;
                (*GatedBy)->TryGetStringField(TEXT("switch"), SwitchName);
                TestEqual(TEXT("gatedBy names the gating static switch"), SwitchName, FString(GatingSwitchName));

                bool bCurrentValue = true;
                TestTrue(TEXT("gatedBy reports the switch's current value"),
                    (*GatedBy)->TryGetBoolField(TEXT("value"), bCurrentValue));
                TestFalse(TEXT("The gating switch currently reads false"), bCurrentValue);

                bool bRequiredValue = false;
                TestTrue(TEXT("gatedBy reports the value that would enable the input"),
                    (*GatedBy)->TryGetBoolField(TEXT("requiredValue"), bRequiredValue));
                TestTrue(TEXT("Setting the switch true is what routes the input into the graph"), bRequiredValue);

                FString BranchTaken;
                (*GatedBy)->TryGetStringField(TEXT("branchTaken"), BranchTaken);
                TestEqual(TEXT("gatedBy names the branch the switch takes today"), BranchTaken, FString(TEXT("false")));
            }
        }
    }

    // Not every input of a gate-carrying module is reported unreachable: the walk must not
    // blanket-fail a module just because it contains static switches.
    {
        TSharedPtr<FJsonObject> Wrapper = MakeShared<FJsonObject>();
        Wrapper->SetArrayField(TEXT("moduleInputs"), NiagaraDumpBuilder::BuildModuleInputsJson(ModuleNode));
        const TArray<TSharedPtr<FJsonValue>>* Entries = nullptr;
        int32 ReachableCount = 0;
        if (Wrapper->TryGetArrayField(TEXT("moduleInputs"), Entries) && Entries)
        {
            for (const TSharedPtr<FJsonValue>& Value : *Entries)
            {
                const TSharedPtr<FJsonObject>* Entry = nullptr;
                bool bReachable = false;
                if (Value.IsValid() && Value->TryGetObject(Entry) && Entry
                    && (*Entry)->TryGetBoolField(TEXT("reachable"), bReachable) && bReachable)
                {
                    ++ReachableCount;
                }
            }
        }
        TestTrue(TEXT("Ungated inputs of the same module stay reachable"), ReachableCount > 0);
    }

    // Counterfactual: flip the switch's caller pin to true and the same input must become
    // reachable. A `reachable` field that did not read the switch could not do this.
    {
        UEdGraphPin* SwitchPin = nullptr;
        for (UEdGraphPin* Pin : ModuleNode->Pins)
        {
            if (Pin && Pin->Direction == EGPD_Input && Pin->PinName == FName(GatingSwitchName))
            {
                SwitchPin = Pin;
                break;
            }
        }
        if (TestNotNull(TEXT("The placed module exposes the gating switch's caller pin"), SwitchPin))
        {
            FString PinDefault;
            FString EncodeError;
            TestTrue(TEXT("Bool switch value encodes to a caller-pin default"),
                NiagaraStaticSwitch::EncodePinDefault(
                    MakeShared<FJsonValueBoolean>(true),
                    ENiagaraStaticSwitchType::Bool,
                    nullptr,
                    PinDefault,
                    EncodeError));
            SwitchPin->DefaultValue = PinDefault;

            TSharedPtr<FJsonObject> Gated = ReadModuleInput(ModuleNode, GatedInputName);
            if (TestTrue(TEXT("moduleInputs still lists the input after flipping the switch"), Gated.IsValid()))
            {
                bool bReachable = false;
                Gated->TryGetBoolField(TEXT("reachable"), bReachable);
                TestTrue(TEXT("The input becomes reachable once UseStartFrame is true"), bReachable);
                TestFalse(TEXT("A reachable input carries no gatedBy"), Gated->HasField(TEXT("gatedBy")));
            }
        }
    }

    NIRTestFixtures::DestroyFixture(System);
    return true;
}
