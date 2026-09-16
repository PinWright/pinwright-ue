// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression test for E-niagara-inspect-params-stale-after-override.
//
// The ticket's own advice sends a set-then-verify loop to niagara.inspect's `graphs` aspect for
// the live override value, because the `parameters` aspect reports the untouched rapid-iteration
// template default. That aspect reported UEdGraphPin::DefaultValue unconditionally, so an
// override pin that had since gained an inbound link — a dynamic-input chain or a User.* binding —
// answered with the literal a caller had written earlier, which the graph no longer reads. The
// caller read back their own write and concluded it had landed.
//
// The write side already refuses that shape (MODULE_INPUT_OVERRIDE_LINKED, from
// B-niagara-literal-over-linked-override-pin). The readback now makes the same statement: a linked
// input pin's stored default is omitted rather than echoed as `defaultValue`, and the entry
// carries rawDefaultValue + defaultValueError naming that code — the shape
// B-niagara-decode-pin-default-coerces-to-zero established for an unusable stored value.
//
// The same pin is read twice, before and after the link is wired, so the control and the case
// differ only in the link. Counterfactual — on the pre-fix builder the second readback carried
// `defaultValue: "42.0"` and neither of the two new fields, so all three assertions on it fail.

#include "Misc/AutomationTest.h"
#include "Tests/Assets/NiagaraEditTestUtils.h"
#include "Tests/Niagara/TestNIRFixtures.h"
#include "Tests/TestUtils.h"

#include "Handlers/Niagara/NiagaraDumpBuilder.h"

#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "NiagaraCommon.h"
#include "NiagaraGraph.h"
#include "NiagaraNodeFunctionCall.h"
#include "NiagaraScript.h"
#include "NiagaraSystem.h"

namespace
{
    // Resolve the module input's override pin by its aliased name ("<FunctionName>.<InputName>").
    // Re-resolved rather than cached because the dynamic-input wiring may replace the pin object.
    // Uniquely suffixed so the unity build cannot collide it with the sibling Niagara test TUs'
    // anonymous-namespace helpers.
    UEdGraphPin* FindOverridePinForLinkedPinDefaultTest(UNiagaraNodeFunctionCall* ModuleNode, const TCHAR* InputName)
    {
        if (!ModuleNode)
        {
            return nullptr;
        }
        UNiagaraGraph* Graph = ModuleNode->GetNiagaraGraph();
        if (!Graph)
        {
            return nullptr;
        }
        const FName AliasedName(*FString::Printf(TEXT("%s.%s"), *ModuleNode->GetFunctionName(), InputName));
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            if (!Node)
            {
                continue;
            }
            for (UEdGraphPin* Pin : Node->Pins)
            {
                if (Pin && Pin->Direction == EGPD_Input && Pin->PinName == AliasedName)
                {
                    return Pin;
                }
            }
        }
        return nullptr;
    }

    // Walk the graphs aspect (graphs[].nodes[].pins[]) for the pin entry carrying this pin id.
    TSharedPtr<FJsonObject> FindPinEntryForLinkedPinDefaultTest(const TSharedPtr<FJsonObject>& GraphsJson, const FString& PinId)
    {
        const TArray<TSharedPtr<FJsonValue>>* Graphs = nullptr;
        if (!GraphsJson.IsValid() || !GraphsJson->TryGetArrayField(TEXT("graphs"), Graphs) || !Graphs)
        {
            return nullptr;
        }
        for (const TSharedPtr<FJsonValue>& GraphValue : *Graphs)
        {
            const TSharedPtr<FJsonObject>* GraphObj = nullptr;
            if (!GraphValue.IsValid() || !GraphValue->TryGetObject(GraphObj) || !GraphObj)
            {
                continue;
            }
            const TArray<TSharedPtr<FJsonValue>>* Nodes = nullptr;
            if (!(*GraphObj)->TryGetArrayField(TEXT("nodes"), Nodes) || !Nodes)
            {
                continue;
            }
            for (const TSharedPtr<FJsonValue>& NodeValue : *Nodes)
            {
                const TSharedPtr<FJsonObject>* NodeObj = nullptr;
                if (!NodeValue.IsValid() || !NodeValue->TryGetObject(NodeObj) || !NodeObj)
                {
                    continue;
                }
                if (TSharedPtr<FJsonObject> Pin =
                        JsonArrayFindObjectByStringField(*NodeObj, TEXT("pins"), TEXT("id"), PinId))
                {
                    return Pin;
                }
            }
        }
        return nullptr;
    }
}

// ---------------------------------------------------------------------------
// The graphs aspect reports an override pin's literal while nothing is linked into it, and stops
// reporting it as the pin's value once a dynamic-input chain drives the pin.
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FNiagaraGraphsLinkedPinDefaultNotEchoedTest,
    "PinWright.niagara.dump.LinkedPinDefaultNotEchoed",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraGraphsLinkedPinDefaultNotEchoedTest::RunTest(const FString& Parameters)
{
    // A missing required fixture is a FAILURE, not a skip.
    const TCHAR* SpawnRatePath = TEXT("/Niagara/Modules/Emitter/SpawnRate.SpawnRate");
    UNiagaraScript* SpawnRateScript = LoadObject<UNiagaraScript>(nullptr, SpawnRatePath);
    if (!TestNotNull(TEXT("Emitter SpawnRate module script loads"), SpawnRateScript))
    {
        return false;
    }

    // Add_Float is a stock float DynamicInput-usage script shipped with the Niagara plugin.
    const TCHAR* DynamicInputPath = TEXT("/Niagara/DynamicInputs/Add/Add_Float.Add_Float");
    UNiagaraScript* DynamicInputScript = LoadObject<UNiagaraScript>(nullptr, DynamicInputPath);
    if (!TestNotNull(TEXT("Add_Float dynamic-input script loads"), DynamicInputScript))
    {
        return false;
    }

    UNiagaraSystem* System = NIRTestFixtures::BuildEmptySystemWithEmitter(FName(TEXT("LinkedPinDefault")));
    if (!TestNotNull(TEXT("Transient system with emitter created"), System))
    {
        return false;
    }

    UNiagaraNodeFunctionCall* ModuleNode = NIRTestFixtures::AddModuleToStack(
        System,
        ENiagaraScriptUsage::EmitterUpdateScript,
        SpawnRateScript);
    if (!TestNotNull(TEXT("SpawnRate module added to EmitterUpdate stack"), ModuleNode))
    {
        NIRTestFixtures::DestroyFixture(System);
        return false;
    }

    // 1. Write the literal through the production verb, on a pin nothing is linked into — the
    //    clean case the ticket's repro describes, where the write really does land on the pin.
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), System->GetPathName());
    Payload->SetStringField(TEXT("emitter"), TEXT("LinkedPinDefault"));
    Payload->SetStringField(TEXT("entryId"), ModuleNode->NodeGuid.ToString());
    Payload->SetStringField(TEXT("inputName"), TEXT("SpawnRate"));
    Payload->SetNumberField(TEXT("value"), 42.0);
    Payload->SetStringField(TEXT("scriptUsage"), TEXT("EmitterUpdateScript"));
    Payload->SetBoolField(TEXT("compile"), false);
    Payload->SetBoolField(TEXT("save"), false);

    FTestResponseCapture Capture;
    if (!NiagaraEditTestUtils::InvokeExpectSuccess(*this, TEXT("niagara.set_module_input"), Payload, Capture))
    {
        NIRTestFixtures::DestroyFixture(System);
        return false;
    }

    UEdGraphPin* OverridePin = FindOverridePinForLinkedPinDefaultTest(ModuleNode, TEXT("SpawnRate"));
    if (!TestNotNull(TEXT("literal write created the SpawnRate override pin"), OverridePin))
    {
        NIRTestFixtures::DestroyFixture(System);
        return false;
    }

    // 2. Control: with no inbound link the pin's stored literal IS its effective value, and the
    //    graphs aspect reports it as `defaultValue`. This is what the set_module_input docs send
    //    a verify loop to read, and it must keep working.
    {
        TSharedPtr<FJsonObject> Entry =
            FindPinEntryForLinkedPinDefaultTest(NiagaraDumpBuilder::BuildGraphsJson(System), OverridePin->PinId.ToString());
        if (TestTrue(TEXT("graphs aspect lists the unlinked override pin"), Entry.IsValid()))
        {
            FString DefaultValue;
            TestTrue(
                TEXT("unlinked override pin still reports defaultValue"),
                Entry->TryGetStringField(TEXT("defaultValue"), DefaultValue));
            TestEqual(TEXT("unlinked override pin reports the literal it was given"), DefaultValue, FString(TEXT("42.0")));
        }
    }

    // 3. Drive the same input from a dynamic-input chain, wired by the fixture rather than by a
    //    verb under test. UE leaves the pin's DefaultValue string in place when a link is made;
    //    stamp it so the stale-literal precondition holds regardless of what the engine's
    //    SetDynamicInputForFunctionInput does with the old pin.
    NIRTestFixtures::SetModuleInputDynamicInput(ModuleNode, FName(TEXT("SpawnRate")), DynamicInputScript);
    UEdGraphPin* LinkedPin = FindOverridePinForLinkedPinDefaultTest(ModuleNode, TEXT("SpawnRate"));
    if (!TestNotNull(TEXT("override pin survives the dynamic-input wiring"), LinkedPin))
    {
        NIRTestFixtures::DestroyFixture(System);
        return false;
    }
    LinkedPin->DefaultValue = TEXT("42.0");
    if (!TestTrue(TEXT("fixture left an inbound link on the override pin"), LinkedPin->LinkedTo.Num() > 0))
    {
        NIRTestFixtures::DestroyFixture(System);
        return false;
    }

    // 4. The case: the graph now reads the chain, so the readback must not answer with the
    //    literal — the value a caller wrote and would otherwise take as confirmation.
    {
        TSharedPtr<FJsonObject> Entry =
            FindPinEntryForLinkedPinDefaultTest(NiagaraDumpBuilder::BuildGraphsJson(System), LinkedPin->PinId.ToString());
        if (TestTrue(TEXT("graphs aspect lists the linked override pin"), Entry.IsValid()))
        {
            TestFalse(
                TEXT("linked override pin reports no defaultValue"),
                Entry->HasField(TEXT("defaultValue")));

            FString ValueError;
            if (TestTrue(
                    TEXT("linked override pin carries defaultValueError"),
                    Entry->TryGetStringField(TEXT("defaultValueError"), ValueError)))
            {
                TestTrue(
                    TEXT("defaultValueError names the linked-override refusal code"),
                    ValueError.Contains(TEXT("MODULE_INPUT_OVERRIDE_LINKED")));
            }

            // The literal is not lost, only demoted: it is still readable for a restore or a diff.
            FString RawDefaultValue;
            TestTrue(
                TEXT("linked override pin carries rawDefaultValue"),
                Entry->TryGetStringField(TEXT("rawDefaultValue"), RawDefaultValue));
            TestEqual(TEXT("rawDefaultValue is the inert stored literal"), RawDefaultValue, FString(TEXT("42.0")));
        }
    }

    NIRTestFixtures::DestroyFixture(System);
    return true;
}
