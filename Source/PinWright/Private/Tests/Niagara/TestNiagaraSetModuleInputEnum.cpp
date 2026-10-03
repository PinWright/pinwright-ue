// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression tests for enum-typed module inputs on niagara.set_module_input
// (B-niagara-module-input-enum-by-name, B-niagara-module-input-enum-display-name-rejected).
//
// AddVelocityInCone's "Cone Axis Coordinate Space" is an ENiagaraCoordinateSpace module input
// (Simulation=0, World=1, Local=2), not a static switch. Before the fix its name was refused with
// UNSUPPORTED_INPUT_VALUE and a number was inferred as a float, so the override pin was created
// float-typed with "2.0" and any index — 97 included — was accepted. The fixture is a transient
// system built in-test, so no stock system is duplicated and nothing depends on its compile state.

#include "Misc/AutomationTest.h"
#include "Tests/Assets/NiagaraEditTestUtils.h"
#include "Tests/Niagara/TestNIRFixtures.h"
#include "Tests/TestSkipReporting.h"
#include "Tests/TestUtils.h"

#include "Handlers/Niagara/NiagaraDumpBuilder.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_Niagara.h"
#include "NiagaraEmitterHandle.h"
#include "NiagaraGraph.h"
#include "NiagaraNodeFunctionCall.h"
#include "NiagaraScript.h"
#include "NiagaraSystem.h"
#include "NiagaraTypes.h"
#include "ViewModels/Stack/NiagaraParameterHandle.h"
#include "ViewModels/Stack/NiagaraStackGraphUtilities.h"

namespace TestNiagaraSetModuleInputEnumHelpers
{
    const TCHAR* const EmitterName = TEXT("EnumInputs");
    const TCHAR* const InputName = TEXT("Cone Axis Coordinate Space");

    TSharedPtr<FJsonObject> MakePayload(UNiagaraSystem* System, UNiagaraNodeFunctionCall* ModuleNode, const TSharedPtr<FJsonValue>& Value)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("assetPath"), System->GetPathName());
        Payload->SetStringField(TEXT("emitter"), EmitterName);
        Payload->SetStringField(TEXT("entryId"), ModuleNode->NodeGuid.ToString());
        Payload->SetStringField(TEXT("inputName"), InputName);
        Payload->SetField(TEXT("value"), Value);
        Payload->SetStringField(TEXT("scriptUsage"), TEXT("ParticleSpawnScript"));
        Payload->SetBoolField(TEXT("compile"), false);
        Payload->SetBoolField(TEXT("save"), false);
        return Payload;
    }

    UEdGraphPin* FindPinById(UNiagaraNodeFunctionCall* ModuleNode, const FString& PinId)
    {
        UNiagaraGraph* Graph = ModuleNode ? ModuleNode->GetNiagaraGraph() : nullptr;
        if (!Graph)
        {
            return nullptr;
        }
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            for (UEdGraphPin* Pin : Node ? Node->Pins : TArray<UEdGraphPin*>())
            {
                if (Pin && Pin->PinId.ToString() == PinId)
                {
                    return Pin;
                }
            }
        }
        return nullptr;
    }

    // A transient system with AddVelocityInCone on the particle spawn stack, or null after a
    // skip marker (stock module absent) or a failed precondition.
    UNiagaraNodeFunctionCall* BuildFixture(
        FAutomationTestBase& Test,
        NiagaraEditTestUtils::FAuthorableSystemRoots& Roots,
        UNiagaraSystem*& OutSystem)
    {
        OutSystem = nullptr;
        const TCHAR* ModulePath = TEXT("/Niagara/Modules/Spawn/Velocity/AddVelocityInCone.AddVelocityInCone");
        UNiagaraScript* ModuleScript = LoadObject<UNiagaraScript>(nullptr, ModulePath);
        if (!ModuleScript)
        {
            PinWrightTestSkip::SkipAssertions(Test, TEXT("niagara_fixture_assets_absent"),
                FString::Printf(TEXT("could not load '%s'"), ModulePath));
            return nullptr;
        }
        OutSystem = NIRTestFixtures::BuildEmptySystemWithEmitter(FName(EmitterName));
        if (!Test.TestNotNull(TEXT("Niagara system fixture created"), OutSystem))
        {
            return nullptr;
        }
        Roots.System = OutSystem;
        if (OutSystem->GetEmitterHandles().Num() > 0)
        {
            Roots.Emitter = OutSystem->GetEmitterHandles()[0].GetInstance().Emitter.Get();
        }
        UNiagaraNodeFunctionCall* ModuleNode = NIRTestFixtures::AddModuleToStack(
            OutSystem, ENiagaraScriptUsage::ParticleSpawnScript, ModuleScript);
        if (!Test.TestNotNull(TEXT("AddVelocityInCone added to the particle spawn stack"), ModuleNode))
        {
            NIRTestFixtures::DestroyFixture(OutSystem);
            Roots.System = nullptr;
            Roots.Emitter = nullptr;
            OutSystem = nullptr;
        }
        return ModuleNode;
    }

    void Teardown(NiagaraEditTestUtils::FAuthorableSystemRoots& Roots, UNiagaraSystem* System)
    {
        NIRTestFixtures::DestroyFixture(System);
        Roots.System = nullptr;
        Roots.Emitter = nullptr;
    }

    // The pin the response names must be enum-typed and decode to ExpectedValue.
    void AssertEnumPin(FAutomationTestBase& Test, UNiagaraNodeFunctionCall* ModuleNode, const FTestResponseCapture& Capture, int32 ExpectedValue)
    {
        UEdGraphPin* Pin = FindPinById(ModuleNode, Capture.Result->GetStringField(TEXT("pinId")));
        if (!Test.TestNotNull(TEXT("response pinId resolves to the override pin"), Pin))
        {
            return;
        }
        const FNiagaraTypeDefinition PinType = UEdGraphSchema_Niagara::PinToTypeDefinition(Pin);
        Test.TestNotNull(TEXT("override pin is enum-typed, not the float an inferred number made"), PinType.GetEnum());
        Test.TestEqual(TEXT("override pin carries the declared enum type"), PinType.GetName(), FString(TEXT("ENiagaraCoordinateSpace")));
        const FNiagaraVariable Decoded = UEdGraphSchema_Niagara::PinToNiagaraVariable(Pin, true);
        if (Test.TestTrue(TEXT("enum pin default decodes through Niagara's enum codec"), Decoded.IsDataAllocated() && Decoded.GetType().GetEnum() != nullptr))
        {
            Test.TestEqual(TEXT("decoded enum value"), Decoded.GetValue<FNiagaraInt32>().Value, ExpectedValue);
        }
        Test.TestEqual(TEXT("response value echoes the pin read-back"),
            Capture.Result->GetStringField(TEXT("value")), Pin->GetDefaultAsString());
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FNiagaraSetModuleInputEnumAcceptsEntryNameTest,
    "PinWright.niagara.set_module_input.EnumInputAcceptsEntryName",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraSetModuleInputEnumAcceptsEntryNameTest::RunTest(const FString& Parameters)
{
    using namespace TestNiagaraSetModuleInputEnumHelpers;
    NiagaraEditTestUtils::FAuthorableSystemRoots Roots;
    UNiagaraSystem* System = nullptr;
    UNiagaraNodeFunctionCall* ModuleNode = BuildFixture(*this, Roots, System);
    if (!ModuleNode)
    {
        return true;
    }

    // Readback: the input's option table carries the index beside each name.
    {
        TSharedPtr<FJsonObject> Wrapper = MakeShared<FJsonObject>();
        Wrapper->SetArrayField(TEXT("moduleInputs"), NiagaraDumpBuilder::BuildModuleInputsJson(ModuleNode));
        TSharedPtr<FJsonObject> Entry = JsonArrayFindObjectByStringField(Wrapper, TEXT("moduleInputs"), TEXT("name"), InputName);
        const TArray<TSharedPtr<FJsonValue>>* Options = nullptr;
        if (TestTrue(TEXT("moduleInputs lists the enum input with enumOptions"),
                Entry.IsValid() && Entry->TryGetArrayField(TEXT("enumOptions"), Options) && Options && Options->Num() == 3))
        {
            const TSharedPtr<FJsonObject> Local = (*Options)[2]->AsObject();
            if (TestTrue(TEXT("enumOptions entries are {index,name,displayName} objects"), Local.IsValid()))
            {
                TestEqual(TEXT("third option index"), static_cast<int32>(Local->GetNumberField(TEXT("index"))), 2);
                TestEqual(TEXT("third option name"), Local->GetStringField(TEXT("name")), FString(TEXT("Local")));
            }
        }
    }

    FTestResponseCapture Capture;
    if (NiagaraEditTestUtils::InvokeExpectSuccess(*this, TEXT("niagara.set_module_input"),
            MakePayload(System, ModuleNode, MakeShared<FJsonValueString>(TEXT("Local"))), Capture))
    {
        AssertEnumPin(*this, ModuleNode, Capture, 2);
        TestEqual(TEXT("value is the entry name, not a float spelling"), Capture.Result->GetStringField(TEXT("value")), FString(TEXT("Local")));
        TestEqual(TEXT("enumIndex echoes the branch"), static_cast<int32>(Capture.Result->GetNumberField(TEXT("enumIndex"))), 2);
        TestTrue(TEXT("enumPath names the enum"), Capture.Result->GetStringField(TEXT("enumPath")).EndsWith(TEXT("ENiagaraCoordinateSpace")));
        const TArray<TSharedPtr<FJsonValue>>* Options = nullptr;
        TestTrue(TEXT("success echoes the enumOptions table"),
            Capture.Result->TryGetArrayField(TEXT("enumOptions"), Options) && Options && Options->Num() == 3);
    }

    // Labels match case-insensitively; a number is the branch index.
    Capture.Reset();
    if (NiagaraEditTestUtils::InvokeExpectSuccess(*this, TEXT("niagara.set_module_input"),
            MakePayload(System, ModuleNode, MakeShared<FJsonValueString>(TEXT("simulation"))), Capture))
    {
        AssertEnumPin(*this, ModuleNode, Capture, 0);
    }
    Capture.Reset();
    if (NiagaraEditTestUtils::InvokeExpectSuccess(*this, TEXT("niagara.set_module_input"),
            MakePayload(System, ModuleNode, MakeShared<FJsonValueNumber>(1)), Capture))
    {
        AssertEnumPin(*this, ModuleNode, Capture, 1);
        TestEqual(TEXT("index 1 is written as its entry name"), Capture.Result->GetStringField(TEXT("value")), FString(TEXT("World")));
    }

    Teardown(Roots, System);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FNiagaraSetModuleInputEnumRefusesOffTableValuesTest,
    "PinWright.niagara.set_module_input.EnumInputRefusesOffTableValues",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraSetModuleInputEnumRefusesOffTableValuesTest::RunTest(const FString& Parameters)
{
    using namespace TestNiagaraSetModuleInputEnumHelpers;
    NiagaraEditTestUtils::FAuthorableSystemRoots Roots;
    UNiagaraSystem* System = nullptr;
    UNiagaraNodeFunctionCall* ModuleNode = BuildFixture(*this, Roots, System);
    if (!ModuleNode)
    {
        return true;
    }

    const int32 NodesBefore = ModuleNode->GetNiagaraGraph()->Nodes.Num();
    const TPair<const TCHAR*, TSharedPtr<FJsonValue>> Rejected[] = {
        { TEXT("out-of-table index 97"), MakeShared<FJsonValueNumber>(97) },
        { TEXT("negative index"), MakeShared<FJsonValueNumber>(-1) },
        { TEXT("fractional index 1.5"), MakeShared<FJsonValueNumber>(1.5) },
        { TEXT("unknown name"), MakeShared<FJsonValueString>(TEXT("Sideways")) },
        { TEXT("boolean"), MakeShared<FJsonValueBoolean>(true) },
    };
    for (const TPair<const TCHAR*, TSharedPtr<FJsonValue>>& Case : Rejected)
    {
        FTestResponseCapture Capture;
        TestTrue(FString::Printf(TEXT("%s: handler found"), Case.Key),
            InvokeHandlerWithCapture(TEXT("niagara.set_module_input"), MakePayload(System, ModuleNode, Case.Value), Capture));
        TestFalse(FString::Printf(TEXT("%s is refused"), Case.Key), Capture.bSuccess);
        TestEqual(FString::Printf(TEXT("%s returns INVALID_VALUE"), Case.Key), Capture.ErrorCode, FString(TEXT("INVALID_VALUE")));
        TestTrue(FString::Printf(TEXT("%s: message lists the table"), Case.Key), Capture.Message.Contains(TEXT("Local (index 2)")));
        const TArray<TSharedPtr<FJsonValue>>* Options = nullptr;
        TestTrue(FString::Printf(TEXT("%s: rejection carries enumOptions"), Case.Key),
            Capture.Result.IsValid() && Capture.Result->TryGetArrayField(TEXT("enumOptions"), Options) && Options && Options->Num() == 3);
        TestTrue(FString::Printf(TEXT("%s: rejection carries enumPath"), Case.Key),
            Capture.Result.IsValid() && Capture.Result->HasField(TEXT("enumPath")));
    }
    TestEqual(TEXT("refusals create no override graph nodes"), ModuleNode->GetNiagaraGraph()->Nodes.Num(), NodesBefore);

    Teardown(Roots, System);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FNiagaraSetModuleInputEnumReplacesMistypedOverrideTest,
    "PinWright.niagara.set_module_input.EnumInputReplacesMistypedOverride",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraSetModuleInputEnumReplacesMistypedOverrideTest::RunTest(const FString& Parameters)
{
    using namespace TestNiagaraSetModuleInputEnumHelpers;
    NiagaraEditTestUtils::FAuthorableSystemRoots Roots;
    UNiagaraSystem* System = nullptr;
    UNiagaraNodeFunctionCall* ModuleNode = BuildFixture(*this, Roots, System);
    if (!ModuleNode)
    {
        return true;
    }

    // The state the old number inference left behind: a float override pin holding "2.0".
    const FNiagaraParameterHandle Aliased = FNiagaraParameterHandle::CreateAliasedModuleParameterHandle(
        FNiagaraParameterHandle::CreateModuleParameterHandle(FName(InputName)), ModuleNode);
    UEdGraphPin& FloatPin = FNiagaraStackGraphUtilities::GetOrCreateStackFunctionInputOverridePin(
        *ModuleNode, Aliased, FNiagaraTypeDefinition::GetFloatDef(), FGuid(), FGuid());
    GetDefault<UEdGraphSchema_Niagara>()->TrySetDefaultValue(FloatPin, TEXT("2.0"), true);
    if (!TestTrue(TEXT("precondition: a float-typed override pin exists"),
            UEdGraphSchema_Niagara::PinToTypeDefinition(&FloatPin) == FNiagaraTypeDefinition::GetFloatDef()))
    {
        Teardown(Roots, System);
        return true;
    }

    FTestResponseCapture Capture;
    if (NiagaraEditTestUtils::InvokeExpectSuccess(*this, TEXT("niagara.set_module_input"),
            MakePayload(System, ModuleNode, MakeShared<FJsonValueString>(TEXT("Local"))), Capture))
    {
        AssertEnumPin(*this, ModuleNode, Capture, 2);
    }

    Teardown(Roots, System);
    return true;
}
