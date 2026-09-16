// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Misc/AutomationTest.h"
#include "Tests/Assets/NiagaraEditTestUtils.h"
#include "Tests/Niagara/TestNIRFixtures.h"
#include "Tests/TestUtils.h"

#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_Niagara.h"
#include "NiagaraGraph.h"
#include "NiagaraEmitterHandle.h"
#include "NiagaraNodeFunctionCall.h"
#include "NiagaraScript.h"
#include "NiagaraSystem.h"
#include "NiagaraTypes.h"

namespace
{
    // UE's Niagara quaternion pin codec serializes each component with three decimal
    // places, so schema decode is allowed that bounded quantization error.
    constexpr float QuatPinDefaultQuantizationTolerance = 0.00051f;

    TSharedPtr<FJsonValue> MakeNumberArray(const TArray<double>& Numbers)
    {
        TArray<TSharedPtr<FJsonValue>> Values;
        Values.Reserve(Numbers.Num());
        for (double Number : Numbers)
        {
            Values.Add(MakeShared<FJsonValueNumber>(Number));
        }
        return MakeShared<FJsonValueArray>(Values);
    }

    TSharedPtr<FJsonObject> MakeModuleInputPayload(
        UNiagaraSystem* System,
        UNiagaraNodeFunctionCall* ModuleNode,
        const TCHAR* InputName,
        const TSharedPtr<FJsonValue>& Value)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("assetPath"), System->GetPathName());
        Payload->SetStringField(TEXT("emitter"), TEXT("Fountain"));
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
            if (!Node)
            {
                continue;
            }
            for (UEdGraphPin* Pin : Node->Pins)
            {
                if (Pin && Pin->PinId.ToString() == PinId)
                {
                    return Pin;
                }
            }
        }
        return nullptr;
    }

    UNiagaraNodeFunctionCall* BuildShapeLocationFixture(
        FAutomationTestBase& Test,
        NiagaraEditTestUtils::FAuthorableSystemRoots& Roots,
        UNiagaraSystem*& OutSystem)
    {
        OutSystem = nullptr;
        const TCHAR* ShapeLocationPath = TEXT("/Niagara/Modules/Spawn/Location/V2/ShapeLocation.ShapeLocation");
        UNiagaraScript* ShapeLocationScript = LoadObject<UNiagaraScript>(nullptr, ShapeLocationPath);
        if (!Test.TestNotNull(TEXT("ShapeLocation module script loads"), ShapeLocationScript))
        {
            return nullptr;
        }

        OutSystem = NIRTestFixtures::BuildEmptySystemWithEmitter(FName(TEXT("Fountain")));
        if (!Test.TestNotNull(TEXT("Niagara system fixture created"), OutSystem))
        {
            return nullptr;
        }
        Roots.System = OutSystem;
        for (const FNiagaraEmitterHandle& Handle : OutSystem->GetEmitterHandles())
        {
            Roots.Emitter = Handle.GetInstance().Emitter.Get();
            break;
        }

        UNiagaraNodeFunctionCall* ModuleNode = NIRTestFixtures::AddModuleToStack(
            OutSystem,
            ENiagaraScriptUsage::ParticleSpawnScript,
            ShapeLocationScript);
        if (!Test.TestNotNull(TEXT("ShapeLocation module added to ParticleSpawn"), ModuleNode))
        {
            NIRTestFixtures::DestroyFixture(OutSystem);
            Roots.System = nullptr;
            Roots.Emitter = nullptr;
            OutSystem = nullptr;
            return nullptr;
        }

        TSharedPtr<FJsonObject> SwitchPayload = MakeShared<FJsonObject>();
        SwitchPayload->SetStringField(TEXT("assetPath"), OutSystem->GetPathName());
        SwitchPayload->SetStringField(TEXT("emitter"), TEXT("Fountain"));
        SwitchPayload->SetStringField(TEXT("entryId"), ModuleNode->NodeGuid.ToString());
        SwitchPayload->SetStringField(TEXT("inputName"), TEXT("Transform Method"));
        SwitchPayload->SetStringField(TEXT("value"), TEXT("Custom Matrix"));
        SwitchPayload->SetStringField(TEXT("scriptUsage"), TEXT("ParticleSpawnScript"));
        SwitchPayload->SetBoolField(TEXT("compile"), false);
        SwitchPayload->SetBoolField(TEXT("save"), false);
        FTestResponseCapture SwitchCapture;
        if (!NiagaraEditTestUtils::InvokeExpectSuccess(
                Test,
                TEXT("niagara.set_static_switch"),
                SwitchPayload,
                SwitchCapture))
        {
            NIRTestFixtures::DestroyFixture(OutSystem);
            Roots.System = nullptr;
            Roots.Emitter = nullptr;
            OutSystem = nullptr;
            return nullptr;
        }
        return ModuleNode;
    }

    void AssertWrittenPin(
        FAutomationTestBase& Test,
        UNiagaraNodeFunctionCall* ModuleNode,
        const FTestResponseCapture& Capture,
        const FNiagaraTypeDefinition& ExpectedType,
        const FNiagaraVariable& ExpectedValue)
    {
        const FString PinId = Capture.Result->GetStringField(TEXT("pinId"));
        UEdGraphPin* Pin = FindPinById(ModuleNode, PinId);
        if (!Test.TestNotNull(TEXT("response pinId resolves to the override pin"), Pin))
        {
            return;
        }
        Test.TestTrue(TEXT("override pin keeps the module input's declared type"),
            UEdGraphSchema_Niagara::PinToTypeDefinition(Pin) == ExpectedType);
        const FNiagaraVariable ActualValue = UEdGraphSchema_Niagara::PinToNiagaraVariable(Pin, true);
        Test.TestTrue(TEXT("schema decoder preserves the declared type"), ActualValue.GetType() == ExpectedType);
        Test.TestTrue(TEXT("schema decoder allocated a value"), ActualValue.IsDataAllocated());
        if (!ActualValue.IsDataAllocated() || ActualValue.GetType() != ExpectedType)
        {
            return;
        }
        if (ExpectedType == FNiagaraTypeDefinition::GetMatrix4Def())
        {
            const FNiagaraMatrix& ExpectedMatrix = *reinterpret_cast<const FNiagaraMatrix*>(ExpectedValue.GetData());
            const FNiagaraMatrix& ActualMatrix = *reinterpret_cast<const FNiagaraMatrix*>(ActualValue.GetData());
            const FVector4f* ExpectedRows[] = {&ExpectedMatrix.Row0, &ExpectedMatrix.Row1, &ExpectedMatrix.Row2, &ExpectedMatrix.Row3};
            const FVector4f* ActualRows[] = {&ActualMatrix.Row0, &ActualMatrix.Row1, &ActualMatrix.Row2, &ActualMatrix.Row3};
            for (int32 Row = 0; Row < 4; ++Row)
            {
                for (int32 Column = 0; Column < 4; ++Column)
                {
                    Test.TestTrue(FString::Printf(TEXT("decoded matrix component [%d][%d] matches"), Row, Column),
                        FMath::IsNearlyEqual((*ExpectedRows[Row])[Column], (*ActualRows[Row])[Column]));
                }
            }
        }
        else if (ExpectedType == FNiagaraTypeDefinition::GetQuatDef())
        {
            const FQuat4f& ExpectedQuat = *reinterpret_cast<const FQuat4f*>(ExpectedValue.GetData());
            const FQuat4f& ActualQuat = *reinterpret_cast<const FQuat4f*>(ActualValue.GetData());
            Test.TestTrue(TEXT("decoded quaternion X matches"), FMath::IsNearlyEqual(ExpectedQuat.X, ActualQuat.X, QuatPinDefaultQuantizationTolerance));
            Test.TestTrue(TEXT("decoded quaternion Y matches"), FMath::IsNearlyEqual(ExpectedQuat.Y, ActualQuat.Y, QuatPinDefaultQuantizationTolerance));
            Test.TestTrue(TEXT("decoded quaternion Z matches"), FMath::IsNearlyEqual(ExpectedQuat.Z, ActualQuat.Z, QuatPinDefaultQuantizationTolerance));
            Test.TestTrue(TEXT("decoded quaternion W matches"), FMath::IsNearlyEqual(ExpectedQuat.W, ActualQuat.W, QuatPinDefaultQuantizationTolerance));
        }
        Test.TestEqual(TEXT("response echoes the override pin read-back"),
            Capture.Result->GetStringField(TEXT("value")), Pin->GetDefaultAsString());
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FNiagaraSetModuleInputRefusesUnsupportedMatrixLiteralTest,
    "PinWright.niagara.set_module_input.RefusesUnsupportedMatrixLiteral",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraSetModuleInputRefusesUnsupportedMatrixLiteralTest::RunTest(const FString& Parameters)
{
    NiagaraEditTestUtils::FAuthorableSystemRoots Roots;
    UNiagaraSystem* System = nullptr;
    UNiagaraNodeFunctionCall* ModuleNode = BuildShapeLocationFixture(*this, Roots, System);
    if (!ModuleNode)
    {
        return false;
    }

    const TArray<double> Numbers = {
        1, 2, 3, 4,
        5, 6, 7, 8,
        9, 10, 11, 12,
        13, 14, 15, 16};
    const int32 NodesBefore = ModuleNode->GetNiagaraGraph()->Nodes.Num();
    FTestResponseCapture Capture;
    TestTrue(TEXT("niagara.set_module_input handler found"), InvokeHandlerWithCapture(
        TEXT("niagara.set_module_input"),
        MakeModuleInputPayload(System, ModuleNode, TEXT("Custom Transform Matrix"), MakeNumberArray(Numbers)),
        Capture));
    TestTrue(TEXT("niagara.set_module_input sent a response"), Capture.bWasCalled);
    TestFalse(TEXT("UE 5.8 refuses Matrix without a schema pin-default encoder"), Capture.bSuccess);
    TestEqual(TEXT("unsupported Matrix pin-default conversion returns INVALID_INPUT_VALUE"),
        Capture.ErrorCode, FString(TEXT("INVALID_INPUT_VALUE")));
    TestEqual(TEXT("unsupported Matrix conversion creates no override graph nodes"),
        ModuleNode->GetNiagaraGraph()->Nodes.Num(), NodesBefore);

    NIRTestFixtures::DestroyFixture(System);
    Roots.System = nullptr;
    Roots.Emitter = nullptr;
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FNiagaraSetModuleInputWritesNormalizedQuatLiteralTest,
    "PinWright.niagara.set_module_input.WritesNormalizedQuatLiteral",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraSetModuleInputWritesNormalizedQuatLiteralTest::RunTest(const FString& Parameters)
{
    NiagaraEditTestUtils::FAuthorableSystemRoots Roots;
    UNiagaraSystem* System = nullptr;
    UNiagaraNodeFunctionCall* ModuleNode = BuildShapeLocationFixture(*this, Roots, System);
    if (!ModuleNode)
    {
        return false;
    }

    FTestResponseCapture Capture;
    const bool bSucceeded = NiagaraEditTestUtils::InvokeExpectSuccess(
        *this,
        TEXT("niagara.set_module_input"),
        MakeModuleInputPayload(System, ModuleNode, TEXT("Rotation Quaternion"), MakeNumberArray({0, 0, 2, 2})),
        Capture);
    TestTrue(TEXT("non-identity quaternion write succeeds through the schema codec"), bSucceeded);
    if (bSucceeded)
    {
        FQuat4f ExpectedQuat(0, 0, 2, 2);
        ExpectedQuat.Normalize();
        const FNiagaraTypeDefinition QuatType = FNiagaraTypeDefinition::GetQuatDef();
        FNiagaraVariable ExpectedValue(QuatType, FName(TEXT("Rotation Quaternion")));
        ExpectedValue.SetData(reinterpret_cast<const uint8*>(&ExpectedQuat));
        AssertWrittenPin(*this, ModuleNode, Capture, QuatType, ExpectedValue);
    }

    NIRTestFixtures::DestroyFixture(System);
    Roots.System = nullptr;
    Roots.Emitter = nullptr;
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FNiagaraSetModuleInputRefusesWrongMatrixArityTest,
    "PinWright.niagara.set_module_input.RefusesWrongMatrixArity",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraSetModuleInputRefusesWrongMatrixArityTest::RunTest(const FString& Parameters)
{
    NiagaraEditTestUtils::FAuthorableSystemRoots Roots;
    UNiagaraSystem* System = nullptr;
    UNiagaraNodeFunctionCall* ModuleNode = BuildShapeLocationFixture(*this, Roots, System);
    if (!ModuleNode)
    {
        return false;
    }

    const int32 NodesBefore = ModuleNode->GetNiagaraGraph()->Nodes.Num();
    FTestResponseCapture Capture;
    TestTrue(TEXT("niagara.set_module_input handler found"), InvokeHandlerWithCapture(
        TEXT("niagara.set_module_input"),
        MakeModuleInputPayload(System, ModuleNode, TEXT("Custom Transform Matrix"),
            MakeNumberArray({1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0})),
        Capture));
    TestFalse(TEXT("15-component matrix is refused"), Capture.bSuccess);
    TestEqual(TEXT("wrong matrix arity returns INVALID_VALUE"), Capture.ErrorCode, FString(TEXT("INVALID_VALUE")));
    TestTrue(TEXT("wrong-arity message names the required 16-number shape"),
        Capture.Message.Contains(TEXT("exactly 16 finite numbers")));
    TestEqual(TEXT("wrong-arity refusal creates no override graph nodes"),
        ModuleNode->GetNiagaraGraph()->Nodes.Num(), NodesBefore);

    NIRTestFixtures::DestroyFixture(System);
    Roots.System = nullptr;
    Roots.Emitter = nullptr;
    return true;
}
