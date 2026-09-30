// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression tests for F-niagara-dynamic-input-nested-inputs.
//
// niagara.set_module_input could assign a dynamic input ({ dynamicInput: "<script>" }) but not set
// that dynamic input's own inputs, so a random range stayed at its 0-1 defaults and an Add stayed at
// its script default addend. The value shape { dynamicInput, inputs: { "<name>": <literal |
// { dynamicInput, inputs }> } } now authors them, recursively, on override pins owned by the
// dynamic-input node (the engine stack's own layout), and both the set response (`inputs`) and the
// niagara.inspect / asset.dump moduleInputs entry (`inputs`) read them back from the graph.
//
// Input names are discovered from the placed dynamic-input node through the same resolver the
// handler uses (NiagaraEdit::EnumerateModuleStackInputs -> GetStackFunctionInputs), not hardcoded.

#include "Misc/AutomationTest.h"
#include "Tests/Assets/NiagaraEditTestUtils.h"
#include "Tests/Niagara/TestNIRFixtures.h"
#include "Tests/TestUtils.h"

#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "Handlers/Niagara/NiagaraDumpBuilder.h"
#include "Handlers/Niagara/NiagaraEditTypes.h"
#include "Handlers/Niagara/NiagaraResetModuleInputHelpers.h"
#include "NiagaraGraph.h"
#include "NiagaraNodeFunctionCall.h"
#include "NiagaraScript.h"
#include "NiagaraSystem.h"
#include "NiagaraTypes.h"
#include "ViewModels/Stack/NiagaraParameterHandle.h"

namespace
{
    const TCHAR* const SpawnRatePathForNestedDiTest = TEXT("/Niagara/Modules/Emitter/SpawnRate.SpawnRate");
    const TCHAR* const AddFloatPathForNestedDiTest = TEXT("/Niagara/DynamicInputs/Add/Add_Float.Add_Float");

    // The override pin OwnerNode owns for InputName ("<OwnerFunctionName>.<InputName>"), resolved by
    // name because every replacing write deletes and recreates override pins.
    UEdGraphPin* FindOverridePinForNestedDiTest(UNiagaraNodeFunctionCall* OwnerNode, const FString& InputName)
    {
        UNiagaraGraph* Graph = OwnerNode ? OwnerNode->GetNiagaraGraph() : nullptr;
        if (!Graph)
        {
            return nullptr;
        }
        const FName AliasedName(*FString::Printf(TEXT("%s.%s"), *OwnerNode->GetFunctionName(), *InputName));
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

    // The dynamic-input node driving OwnerNode's InputName, or null when the input is not driven by one.
    UNiagaraNodeFunctionCall* FindDrivingDynamicInputForNestedDiTest(UNiagaraNodeFunctionCall* OwnerNode, const FString& InputName)
    {
        UEdGraphPin* Pin = FindOverridePinForNestedDiTest(OwnerNode, InputName);
        if (!Pin || Pin->LinkedTo.Num() != 1 || !Pin->LinkedTo[0])
        {
            return nullptr;
        }
        return Cast<UNiagaraNodeFunctionCall>(Pin->LinkedTo[0]->GetOwningNode());
    }

    // Short names of the float stack inputs a placed dynamic-input node declares.
    TArray<FString> FloatInputNamesForNestedDiTest(const UNiagaraNodeFunctionCall& Node)
    {
        TArray<FNiagaraVariable> Inputs;
        NiagaraEdit::EnumerateModuleStackInputs(Node, Inputs);
        TArray<FString> Names;
        for (const FNiagaraVariable& Input : Inputs)
        {
            if (Input.GetType() == FNiagaraTypeDefinition::GetFloatDef())
            {
                Names.AddUnique(FNiagaraParameterHandle(Input.GetName()).GetName().ToString());
            }
        }
        return Names;
    }

    // The entry named Name in a moduleInputs-shaped array, or null.
    TSharedPtr<FJsonObject> FindInputEntryForNestedDiTest(const TArray<TSharedPtr<FJsonValue>>& Entries, const FString& Name)
    {
        for (const TSharedPtr<FJsonValue>& Value : Entries)
        {
            const TSharedPtr<FJsonObject> Entry = Value.IsValid() ? Value->AsObject() : nullptr;
            if (Entry.IsValid() && Entry->GetStringField(TEXT("name")).Equals(Name, ESearchCase::IgnoreCase))
            {
                return Entry;
            }
        }
        return nullptr;
    }

    // The `inputs` array of an entry, empty when absent.
    TArray<TSharedPtr<FJsonValue>> NestedInputsForNestedDiTest(const TSharedPtr<FJsonObject>& Entry)
    {
        const TArray<TSharedPtr<FJsonValue>>* Inputs = nullptr;
        return Entry.IsValid() && Entry->TryGetArrayField(TEXT("inputs"), Inputs) ? *Inputs : TArray<TSharedPtr<FJsonValue>>();
    }

    TSharedPtr<FJsonObject> MakePayloadForNestedDiTest(UNiagaraSystem* System, UNiagaraNodeFunctionCall* ModuleNode, const TSharedPtr<FJsonObject>& Value)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("assetPath"), System->GetPathName());
        Payload->SetStringField(TEXT("emitter"), TEXT("Fountain"));
        Payload->SetStringField(TEXT("entryId"), ModuleNode->NodeGuid.ToString());
        Payload->SetStringField(TEXT("inputName"), TEXT("SpawnRate"));
        Payload->SetObjectField(TEXT("value"), Value);
        Payload->SetStringField(TEXT("scriptUsage"), TEXT("EmitterUpdateScript"));
        Payload->SetBoolField(TEXT("compile"), false);
        Payload->SetBoolField(TEXT("save"), false);
        return Payload;
    }

    TSharedPtr<FJsonObject> MakeDynamicInputValueForNestedDiTest(const TSharedPtr<FJsonObject>& Inputs)
    {
        TSharedPtr<FJsonObject> Value = MakeShared<FJsonObject>();
        Value->SetStringField(TEXT("dynamicInput"), AddFloatPathForNestedDiTest);
        Value->SetObjectField(TEXT("inputs"), Inputs);
        return Value;
    }

    // Shared setup: a system with a SpawnRate module whose SpawnRate input carries an Add_Float placed by
    // the fixture pipeline (not the verb under test), so the dynamic input's float input names can be
    // read off a real placed node. Returns false (after cleaning up) when a stock fixture is missing or
    // Add_Float declares fewer than MinFloatInputs float inputs.
    struct FNestedDiFixture
    {
        UNiagaraSystem* System = nullptr;
        UNiagaraNodeFunctionCall* ModuleNode = nullptr;
        UNiagaraScript* AddFloat = nullptr;
        TArray<FString> FloatInputs;

        bool Build(FAutomationTestBase& Test, int32 MinFloatInputs)
        {
            UNiagaraScript* SpawnRateScript = LoadObject<UNiagaraScript>(nullptr, SpawnRatePathForNestedDiTest);
            AddFloat = LoadObject<UNiagaraScript>(nullptr, AddFloatPathForNestedDiTest);
            if (!Test.TestNotNull(TEXT("SpawnRate module script loads"), SpawnRateScript)
                || !Test.TestNotNull(TEXT("Add_Float dynamic-input script loads"), AddFloat))
            {
                return false;
            }
            System = NIRTestFixtures::BuildEmptySystemWithEmitter(FName(TEXT("Fountain")));
            if (!Test.TestNotNull(TEXT("system with Fountain emitter created"), System))
            {
                return false;
            }
            ModuleNode = NIRTestFixtures::AddModuleToStack(System, ENiagaraScriptUsage::EmitterUpdateScript, SpawnRateScript);
            UNiagaraNodeFunctionCall* Probe = ModuleNode
                ? NIRTestFixtures::SetModuleInputDynamicInput(ModuleNode, FName(TEXT("SpawnRate")), AddFloat)
                : nullptr;
            if (!Test.TestNotNull(TEXT("fixture placed an Add_Float on SpawnRate"), Probe))
            {
                return false;
            }
            FloatInputs = FloatInputNamesForNestedDiTest(*Probe);
            return Test.TestTrue(
                FString::Printf(TEXT("Add_Float declares at least %d float inputs (found: %s)"), MinFloatInputs, *FString::Join(FloatInputs, TEXT(", "))),
                FloatInputs.Num() >= MinFloatInputs);
        }

        ~FNestedDiFixture()
        {
            if (System)
            {
                NIRTestFixtures::DestroyFixture(System);
            }
        }
    };
}

// ---------------------------------------------------------------------------
// One level: { dynamicInput: Add_Float, inputs: { <A>: 42 } } lands 42 on an override pin owned by the
// new Add_Float node, and both the response and the inspect builder read it back.
//
// Counterfactual: before the fix `inputs` was ignored -- the call succeeded, the Add_Float node carried
// no override pin for <A>, and neither readback had an `inputs` array, so the pin and both readback
// assertions fail.
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FNiagaraSetModuleInputNestedDynamicInputLiteralTest,
    "PinWright.niagara.set_module_input.NestedDynamicInput.LiteralReadsBack",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraSetModuleInputNestedDynamicInputLiteralTest::RunTest(const FString& Parameters)
{
    FNestedDiFixture Fixture;
    if (!Fixture.Build(*this, 1))
    {
        return false;
    }
    const FString InputA = Fixture.FloatInputs[0];

    TSharedPtr<FJsonObject> Inputs = MakeShared<FJsonObject>();
    Inputs->SetNumberField(InputA, 42.0);
    FTestResponseCapture Capture;
    if (!NiagaraEditTestUtils::InvokeExpectSuccess(*this, TEXT("niagara.set_module_input"),
            MakePayloadForNestedDiTest(Fixture.System, Fixture.ModuleNode, MakeDynamicInputValueForNestedDiTest(Inputs)), Capture))
    {
        return false;
    }

    UNiagaraNodeFunctionCall* DynamicInputNode = FindDrivingDynamicInputForNestedDiTest(Fixture.ModuleNode, TEXT("SpawnRate"));
    if (!TestTrue(TEXT("SpawnRate is driven by an Add_Float node"), DynamicInputNode && DynamicInputNode->FunctionScript == Fixture.AddFloat))
    {
        return false;
    }
    UEdGraphPin* NestedPin = FindOverridePinForNestedDiTest(DynamicInputNode, InputA);
    TestNotNull(TEXT("the Add_Float node owns an override pin for the nested input"), NestedPin);
    if (NestedPin)
    {
        TestEqual(TEXT("nested override pin carries no link"), NestedPin->LinkedTo.Num(), 0);
        TestEqual(TEXT("nested override pin carries the written literal"), FCString::Atof(*NestedPin->DefaultValue), 42.0f);
    }

    // Response readback: the new node's inputs, from the graph.
    const TArray<TSharedPtr<FJsonValue>>* ResponseInputs = nullptr;
    if (TestTrue(TEXT("response carries inputs"), Capture.Result->TryGetArrayField(TEXT("inputs"), ResponseInputs)))
    {
        const TSharedPtr<FJsonObject> Entry = FindInputEntryForNestedDiTest(*ResponseInputs, InputA);
        TestTrue(TEXT("response inputs name the nested input as a local literal"),
            Entry.IsValid() && Entry->GetStringField(TEXT("valueMode")) == TEXT("local"));
        TestEqual(TEXT("response inputs echo the literal"),
            Entry.IsValid() ? FCString::Atof(*Entry->GetStringField(TEXT("value"))) : 0.0f, 42.0f);
    }

    // Inspect / asset.dump readback.
    const TSharedPtr<FJsonObject> ModuleEntry =
        FindInputEntryForNestedDiTest(NiagaraDumpBuilder::BuildModuleInputsJson(Fixture.ModuleNode), TEXT("SpawnRate"));
    TestTrue(TEXT("inspect reports SpawnRate as dynamicInput"),
        ModuleEntry.IsValid() && ModuleEntry->GetStringField(TEXT("valueMode")) == TEXT("dynamicInput"));
    const TSharedPtr<FJsonObject> NestedEntry = FindInputEntryForNestedDiTest(NestedInputsForNestedDiTest(ModuleEntry), InputA);
    TestTrue(TEXT("inspect reads the nested input back as a local literal of 42"),
        NestedEntry.IsValid()
            && NestedEntry->GetStringField(TEXT("valueMode")) == TEXT("local")
            && FCString::Atof(*NestedEntry->GetStringField(TEXT("value"))) == 42.0f);
    return true;
}

// ---------------------------------------------------------------------------
// Two levels: { dynamicInput: Add_Float, inputs: { <A>: { dynamicInput: Add_Float, inputs: { <A>: 7 } },
// <B>: 3 } } wires a second, distinct Add_Float into the first one's <A>, sets 7 on the inner <A> and 3
// on the outer <B>, and inspect reads both levels back.
//
// Counterfactual: before the fix the whole `inputs` object was ignored, so the outer <A> had no driving
// node and every structural and readback assertion below fails.
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FNiagaraSetModuleInputNestedDynamicInputTwoLevelsTest,
    "PinWright.niagara.set_module_input.NestedDynamicInput.TwoLevelsReadBack",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraSetModuleInputNestedDynamicInputTwoLevelsTest::RunTest(const FString& Parameters)
{
    FNestedDiFixture Fixture;
    if (!Fixture.Build(*this, 2))
    {
        return false;
    }
    const FString InputA = Fixture.FloatInputs[0];
    const FString InputB = Fixture.FloatInputs[1];

    TSharedPtr<FJsonObject> InnerInputs = MakeShared<FJsonObject>();
    InnerInputs->SetNumberField(InputA, 7.0);
    TSharedPtr<FJsonObject> OuterInputs = MakeShared<FJsonObject>();
    OuterInputs->SetObjectField(InputA, MakeDynamicInputValueForNestedDiTest(InnerInputs));
    OuterInputs->SetNumberField(InputB, 3.0);
    FTestResponseCapture Capture;
    if (!NiagaraEditTestUtils::InvokeExpectSuccess(*this, TEXT("niagara.set_module_input"),
            MakePayloadForNestedDiTest(Fixture.System, Fixture.ModuleNode, MakeDynamicInputValueForNestedDiTest(OuterInputs)), Capture))
    {
        return false;
    }

    UNiagaraNodeFunctionCall* Outer = FindDrivingDynamicInputForNestedDiTest(Fixture.ModuleNode, TEXT("SpawnRate"));
    UNiagaraNodeFunctionCall* Inner = FindDrivingDynamicInputForNestedDiTest(Outer, InputA);
    if (!TestTrue(TEXT("outer Add_Float drives SpawnRate and a second Add_Float drives its first input"),
            Outer && Inner && Outer != Inner && Inner->FunctionScript == Fixture.AddFloat))
    {
        return false;
    }
    TestNotEqual(TEXT("the two Add_Float nodes have distinct function names (distinct override aliases)"),
        Inner->GetFunctionName(), Outer->GetFunctionName());
    UEdGraphPin* InnerPin = FindOverridePinForNestedDiTest(Inner, InputA);
    UEdGraphPin* OuterBPin = FindOverridePinForNestedDiTest(Outer, InputB);
    TestTrue(TEXT("inner Add_Float carries 7 on its first input"), InnerPin && FCString::Atof(*InnerPin->DefaultValue) == 7.0f);
    TestTrue(TEXT("outer Add_Float carries 3 on its second input"), OuterBPin && FCString::Atof(*OuterBPin->DefaultValue) == 3.0f);

    const TSharedPtr<FJsonObject> ModuleEntry =
        FindInputEntryForNestedDiTest(NiagaraDumpBuilder::BuildModuleInputsJson(Fixture.ModuleNode), TEXT("SpawnRate"));
    const TArray<TSharedPtr<FJsonValue>> Level1 = NestedInputsForNestedDiTest(ModuleEntry);
    const TSharedPtr<FJsonObject> Level1A = FindInputEntryForNestedDiTest(Level1, InputA);
    const TSharedPtr<FJsonObject> Level1B = FindInputEntryForNestedDiTest(Level1, InputB);
    TestTrue(TEXT("inspect: outer first input is a dynamic input"),
        Level1A.IsValid() && Level1A->GetStringField(TEXT("valueMode")) == TEXT("dynamicInput"));
    TestTrue(TEXT("inspect: outer second input is the literal 3"),
        Level1B.IsValid() && FCString::Atof(*Level1B->GetStringField(TEXT("value"))) == 3.0f);
    const TSharedPtr<FJsonObject> Level2A = FindInputEntryForNestedDiTest(NestedInputsForNestedDiTest(Level1A), InputA);
    TestTrue(TEXT("inspect: inner first input is the literal 7"),
        Level2A.IsValid()
            && Level2A->GetStringField(TEXT("valueMode")) == TEXT("local")
            && FCString::Atof(*Level2A->GetStringField(TEXT("value"))) == 7.0f);
    return true;
}

// ---------------------------------------------------------------------------
// An unknown nested input name is refused with MODULE_INPUT_NOT_FOUND listing the dynamic input's real
// inputs, and the whole write is rolled back: the literal SpawnRate carried before is still there and
// no Add_Float node was left behind.
//
// Counterfactual: before the fix `inputs` was ignored and the call succeeded, replacing the literal with
// an Add_Float -- the error-code assertion and both rollback assertions fail.
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FNiagaraSetModuleInputNestedDynamicInputUnknownRollsBackTest,
    "PinWright.niagara.set_module_input.NestedDynamicInput.UnknownInputRollsBack",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraSetModuleInputNestedDynamicInputUnknownRollsBackTest::RunTest(const FString& Parameters)
{
    FNestedDiFixture Fixture;
    if (!Fixture.Build(*this, 1))
    {
        return false;
    }

    // Replace the fixture's probe dynamic input with a plain literal the rollback must preserve.
    NiagaraResetModuleInput::ClearModuleInputOverride(
        *Fixture.ModuleNode,
        FNiagaraParameterHandle::CreateAliasedModuleParameterHandle(
            FNiagaraParameterHandle::CreateModuleParameterHandle(FName(TEXT("SpawnRate"))), Fixture.ModuleNode),
        *Fixture.ModuleNode->GetNiagaraGraph());
    FNiagaraVariable Literal(FNiagaraTypeDefinition::GetFloatDef(), FName(TEXT("SpawnRate")));
    Literal.SetValue(5.0f);
    NIRTestFixtures::SetModuleInputLiteral(Fixture.ModuleNode, FName(TEXT("SpawnRate")), Literal);
    UEdGraphPin* Before = FindOverridePinForNestedDiTest(Fixture.ModuleNode, TEXT("SpawnRate"));
    if (!TestTrue(TEXT("precondition: SpawnRate carries the literal 5"),
            Before && Before->LinkedTo.Num() == 0 && FCString::Atof(*Before->DefaultValue) == 5.0f))
    {
        return false;
    }

    TSharedPtr<FJsonObject> Inputs = MakeShared<FJsonObject>();
    Inputs->SetNumberField(Fixture.FloatInputs[0], 1.0);
    Inputs->SetNumberField(TEXT("NotARealInput"), 1.0);
    NiagaraEditTestUtils::InvokeExpectError(*this, TEXT("niagara.set_module_input"),
        MakePayloadForNestedDiTest(Fixture.System, Fixture.ModuleNode, MakeDynamicInputValueForNestedDiTest(Inputs)),
        TEXT("MODULE_INPUT_NOT_FOUND"));

    UEdGraphPin* After = FindOverridePinForNestedDiTest(Fixture.ModuleNode, TEXT("SpawnRate"));
    TestTrue(TEXT("rollback: SpawnRate still carries the literal 5 with no link"),
        After && After->LinkedTo.Num() == 0 && FCString::Atof(*After->DefaultValue) == 5.0f);
    int32 AddFloatNodes = 0;
    for (UEdGraphNode* Node : Fixture.ModuleNode->GetNiagaraGraph()->Nodes)
    {
        const UNiagaraNodeFunctionCall* Call = Cast<UNiagaraNodeFunctionCall>(Node);
        AddFloatNodes += (Call && Call->FunctionScript == Fixture.AddFloat) ? 1 : 0;
    }
    TestEqual(TEXT("rollback: no Add_Float node left in the graph"), AddFloatNodes, 0);
    return true;
}
