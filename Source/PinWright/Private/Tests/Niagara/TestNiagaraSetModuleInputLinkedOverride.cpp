// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression test for B-niagara-literal-over-linked-override-pin.
//
// niagara.set_module_input's literal branch used to call TrySetDefaultValue on the input's
// override pin without inspecting UEdGraphPin::LinkedTo. A pin with an inbound link is not a
// value slot — the graph evaluates the link and never reads the pin default — so the write was
// a no-op that still returned success and still echoed the literal back as `value`. The echo is
// what made it convincing: a caller following the verb's own "confirm a literal edit from this
// result directly" advice received confirmation of a value the graph does not use.
//
// The two sibling value modes on the same operation (linked-parameter and dynamic-input) both
// clear any prior override before writing. The literal branch now makes the same decision
// explicitly: it classifies the override pin with NiagaraEdit::ClassifyModuleInputBindings — the
// same walk the inspect / asset.dump `valueMode` readback uses — and refuses a linked pin with
// MODULE_INPUT_OVERRIDE_LINKED unless the caller passes breakExistingLink:true, in which case the
// link and its orphaned upstream chain are removed and the response reports what was displaced.
//
// The fixture wires the dynamic-input chain through NIRTestFixtures::SetModuleInputDynamicInput
// (the same FNiagaraStackGraphUtilities::SetDynamicInputForFunctionInput call the production
// dynamic-input branch makes), so the precondition does not depend on the verb under test.

#include "Misc/AutomationTest.h"
#include "Tests/Assets/NiagaraEditTestUtils.h"
#include "Tests/Niagara/TestNIRFixtures.h"
#include "Tests/TestUtils.h"

#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "NiagaraGraph.h"
#include "NiagaraNodeFunctionCall.h"
#include "NiagaraScript.h"
#include "NiagaraSystem.h"

namespace
{
    // Locate the module input's override pin by its aliased name ("<FunctionName>.<InputName>").
    // Resolving by name rather than by a pin id kept from an earlier call matters here: the
    // breakExistingLink path DELETES the old override pin and creates a fresh one, so a cached
    // pointer or id would name a pin that no longer exists. Uniquely suffixed so the unity build
    // cannot collide it with the sibling Niagara test TUs' anonymous-namespace helpers.
    UEdGraphPin* FindOverridePinByNameForLinkedOverrideTest(UNiagaraNodeFunctionCall* ModuleNode, const TCHAR* InputName)
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

    // True when the override pin is driven upstream by a function-call node running the given
    // dynamic-input script — the structural signature of a live dynamic-input chain, as opposed
    // to a pin carrying a literal default.
    bool PinDrivenByScriptForLinkedOverrideTest(UEdGraphPin* OverridePin, UNiagaraScript* DynamicInputScript)
    {
        if (!OverridePin || !DynamicInputScript)
        {
            return false;
        }
        for (UEdGraphPin* UpstreamPin : OverridePin->LinkedTo)
        {
            UEdGraphNode* UpstreamNode = UpstreamPin ? UpstreamPin->GetOwningNode() : nullptr;
            if (UNiagaraNodeFunctionCall* FunctionNode = Cast<UNiagaraNodeFunctionCall>(UpstreamNode))
            {
                if (FunctionNode->FunctionScript == DynamicInputScript)
                {
                    return true;
                }
            }
        }
        return false;
    }

    TSharedPtr<FJsonObject> MakeLiteralWritePayload(
        UNiagaraSystem* System,
        const FString& ModuleNodeId,
        double Value,
        bool bBreakExistingLink)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("assetPath"), System->GetPathName());
        Payload->SetStringField(TEXT("emitter"), TEXT("Fountain"));
        Payload->SetStringField(TEXT("entryId"), ModuleNodeId);
        Payload->SetStringField(TEXT("inputName"), TEXT("SpawnRate"));
        Payload->SetNumberField(TEXT("value"), Value);
        Payload->SetStringField(TEXT("scriptUsage"), TEXT("EmitterUpdateScript"));
        if (bBreakExistingLink)
        {
            Payload->SetBoolField(TEXT("breakExistingLink"), true);
        }
        Payload->SetBoolField(TEXT("compile"), false);
        Payload->SetBoolField(TEXT("save"), false);
        return Payload;
    }
}

// ---------------------------------------------------------------------------
// A literal written over an override pin that still has an inbound link is refused, leaves the
// link intact, and echoes nothing; the same write with breakExistingLink:true replaces the link
// and reports what it displaced.
//
// Counterfactual — every assertion below fails on the pre-fix handler:
//   * the first write returned success, not MODULE_INPUT_OVERRIDE_LINKED;
//   * the dynamic-input chain survived that "successful" write (the no-op);
//   * the response carried value:"42.0" for a value the graph was not reading (the false echo);
//   * breakExistingLink was not a declared parameter, so the opt-in call was rejected outright
//     by the dispatcher's unknown-parameter gate.
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FNiagaraSetModuleInputRefusesLiteralOverLinkedOverrideTest,
    "PinWright.niagara.set_module_input.RefusesLiteralOverLinkedOverride",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraSetModuleInputRefusesLiteralOverLinkedOverrideTest::RunTest(const FString& Parameters)
{
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

    UNiagaraSystem* System = NIRTestFixtures::BuildEmptySystemWithEmitter(FName(TEXT("Fountain")));
    if (!TestNotNull(TEXT("Transient system with Fountain emitter created"), System))
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

    const FString ModuleNodeId = ModuleNode->NodeGuid.ToString();

    // Precondition: drive the float SpawnRate input from a dynamic-input chain, built by the
    // fixture rather than by the verb under test.
    NIRTestFixtures::SetModuleInputDynamicInput(ModuleNode, FName(TEXT("SpawnRate")), DynamicInputScript);
    UEdGraphPin* LinkedPin = FindOverridePinByNameForLinkedOverrideTest(ModuleNode, TEXT("SpawnRate"));
    if (!TestTrue(
            TEXT("fixture wired SpawnRate's override pin to the Add_Float dynamic input"),
            PinDrivenByScriptForLinkedOverrideTest(LinkedPin, DynamicInputScript)))
    {
        NIRTestFixtures::DestroyFixture(System);
        return false;
    }

    // 1. A bare literal over that linked pin is refused rather than written-and-echoed.
    NiagaraEditTestUtils::InvokeExpectError(
        *this,
        TEXT("niagara.set_module_input"),
        MakeLiteralWritePayload(System, ModuleNodeId, 42.0, /*bBreakExistingLink=*/false),
        TEXT("MODULE_INPUT_OVERRIDE_LINKED"));

    // 2. The refusal is non-destructive: the chain the caller did not ask to replace is intact.
    TestTrue(
        TEXT("refused literal left the dynamic-input chain driving the input"),
        PinDrivenByScriptForLinkedOverrideTest(
            FindOverridePinByNameForLinkedOverrideTest(ModuleNode, TEXT("SpawnRate")),
            DynamicInputScript));

    // 3. The opt-in replaces the link, and the echoed value is now one the graph reads.
    FTestResponseCapture Capture;
    const bool bReplaced = NiagaraEditTestUtils::InvokeExpectSuccess(
        *this,
        TEXT("niagara.set_module_input"),
        MakeLiteralWritePayload(System, ModuleNodeId, 42.0, /*bBreakExistingLink=*/true),
        Capture);
    if (!bReplaced)
    {
        NIRTestFixtures::DestroyFixture(System);
        return false;
    }

    FString Echoed;
    TestTrue(TEXT("breakExistingLink write echoes a value"), Capture.Result->TryGetStringField(TEXT("value"), Echoed));
    TestEqual(TEXT("echoed value is the canonical pin default"), Echoed, FString(TEXT("42.0")));

    // The destruction the caller authorized is reported, not left to be inferred from silence.
    const TSharedPtr<FJsonObject>* ReplacedOverride = nullptr;
    if (TestTrue(
            TEXT("breakExistingLink write reports replacedOverride"),
            Capture.Result->TryGetObjectField(TEXT("replacedOverride"), ReplacedOverride)))
    {
        FString ReplacedValueMode;
        (*ReplacedOverride)->TryGetStringField(TEXT("valueMode"), ReplacedValueMode);
        TestEqual(
            TEXT("replacedOverride names the dynamic-input mode that was displaced"),
            ReplacedValueMode,
            FString(TEXT("dynamicInput")));
        FString ReplacedSource;
        (*ReplacedOverride)->TryGetStringField(TEXT("source"), ReplacedSource);
        TestEqual(
            TEXT("replacedOverride names the displaced dynamic-input script"),
            ReplacedSource,
            DynamicInputScript->GetPathName());
    }

    // The load-bearing structural assertion: the override pin the literal landed on has no
    // inbound link left, so the graph reads the literal rather than the old chain.
    UEdGraphPin* WrittenPin = FindOverridePinByNameForLinkedOverrideTest(ModuleNode, TEXT("SpawnRate"));
    if (TestNotNull(TEXT("override pin exists after the replacing write"), WrittenPin))
    {
        TestEqual(TEXT("override pin has no inbound link after breakExistingLink"), WrittenPin->LinkedTo.Num(), 0);
        TestEqual(TEXT("override pin carries the written literal"), WrittenPin->DefaultValue, FString(TEXT("42.0")));
    }

    NIRTestFixtures::DestroyFixture(System);
    return true;
}
