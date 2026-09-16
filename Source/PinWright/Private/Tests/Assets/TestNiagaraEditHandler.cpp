// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Misc/AutomationTest.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/Niagara/NiagaraEditTypes.h"
#include "NiagaraEditTestUtils.h"
#include "Tests/TestUtils.h"

#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_Niagara.h"
#include "Misc/Guid.h"
#include "NiagaraEmitter.h"
#include "NiagaraEmitterHandle.h"
#include "NiagaraGraph.h"
#include "NiagaraParameterStore.h"
#include "NiagaraScript.h"
#include "NiagaraScriptSource.h"
#include "Misc/EngineVersionComparison.h"
#include "NiagaraSystem.h"
// NiagaraTypeRegistry.h was split out of NiagaraTypes.h in UE 5.6; on UE 5.4 the class lives in NiagaraTypes.h below.
#if __has_include("NiagaraTypeRegistry.h")
#include "NiagaraTypeRegistry.h"
#endif
#include "NiagaraTypes.h"
#include "UObject/Package.h"

namespace
{
    TSharedPtr<FJsonObject> MakeTargetObject(const TCHAR* Kind)
    {
        TSharedPtr<FJsonObject> Target = MakeShared<FJsonObject>();
        Target->SetStringField(TEXT("kind"), Kind);
        return Target;
    }

    TSharedPtr<FJsonObject> MakeSystemPropertyPayload(
        const FString& ObjectPath,
        const TCHAR* PropertyPath,
        const TSharedPtr<FJsonValue>& Value)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("assetPath"), ObjectPath);
        Payload->SetObjectField(TEXT("target"), MakeTargetObject(TEXT("system")));
        Payload->SetStringField(TEXT("propertyPath"), PropertyPath);
        Payload->SetField(TEXT("value"), Value);
        return Payload;
    }

    bool AttachMinimalSystemGraph(UNiagaraSystem* System, UNiagaraGraph*& OutGraph)
    {
        OutGraph = nullptr;
        if (!System || !System->GetSystemSpawnScript() || !System->GetSystemUpdateScript())
        {
            return false;
        }

        UNiagaraScriptSource* Source = NewObject<UNiagaraScriptSource>(
            System->GetSystemSpawnScript(),
            TEXT("EditTestSystemScriptSource"),
            RF_Transient | RF_Transactional);
        if (!Source)
        {
            return false;
        }

        UNiagaraGraph* Graph = NewObject<UNiagaraGraph>(
            Source,
            TEXT("EditTestSystemScriptGraph"),
            RF_Transient | RF_Transactional);
        if (!Graph)
        {
            return false;
        }

        Graph->Schema = UEdGraphSchema_Niagara::StaticClass();
        Source->NodeGraph = Graph;
        System->GetSystemSpawnScript()->SetLatestSource(Source);
        System->GetSystemUpdateScript()->SetLatestSource(Source);
        OutGraph = Graph;
        return true;
    }

    UEdGraphNode* AddMinimalGraphNode(UNiagaraGraph* Graph, const TCHAR* NodeName)
    {
        UEdGraphNode* Node = NewObject<UEdGraphNode>(
            Graph,
            UEdGraphNode::StaticClass(),
            NodeName,
            RF_Transient | RF_Transactional);
        if (Node && Graph)
        {
            Node->NodeGuid = FGuid::NewGuid();
            Graph->AddNode(Node, false, false);
        }
        return Node;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNiagaraEditSetPropertyMissingAssetTest,
    "PinWright.Assets.Niagara.Edit.SetProperty.MissingAsset",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraEditSetPropertyMissingAssetTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeSystemPropertyPayload(
        TEXT("/Game/PinWrightTests/NS_Missing.NS_Missing"),
        TEXT("bFixedBounds"),
        MakeShared<FJsonValueBoolean>(true));

    NiagaraEditTestUtils::InvokeExpectError(*this, TEXT("niagara.set_property"), Payload, TEXT("ASSET_NOT_FOUND"));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNiagaraEditSetPropertyUnknownTargetKindTest,
    "PinWright.Assets.Niagara.Edit.SetProperty.UnknownTargetKind",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraEditSetPropertyUnknownTargetKindTest::RunTest(const FString& Parameters)
{
    FString ObjectPath;
    UNiagaraSystem* System = NiagaraEditTestUtils::NewTransientSystem(ObjectPath);
    TestNotNull(TEXT("Transient Niagara system created"), System);
    if (!System)
    {
        return false;
    }

    const bool bInitialFixedBounds = System->bFixedBounds;
    TSharedPtr<FJsonObject> Payload = MakeSystemPropertyPayload(
        ObjectPath,
        TEXT("bFixedBounds"),
        MakeShared<FJsonValueBoolean>(!bInitialFixedBounds));
    Payload->SetObjectField(TEXT("target"), MakeTargetObject(TEXT("notANiagaraTarget")));

    NiagaraEditTestUtils::InvokeExpectError(*this, TEXT("niagara.set_property"), Payload, TEXT("INVALID_TARGET_KIND"));
    TestEqual(TEXT("Unknown target kind does not mutate system property"), static_cast<bool>(System->bFixedBounds), bInitialFixedBounds);

    System->RemoveFromRoot();
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNiagaraEditSetPropertyInvalidPropertyPathTest,
    "PinWright.Assets.Niagara.Edit.SetProperty.InvalidPropertyPath",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraEditSetPropertyInvalidPropertyPathTest::RunTest(const FString& Parameters)
{
    FString ObjectPath;
    UNiagaraSystem* System = NiagaraEditTestUtils::NewTransientSystem(ObjectPath);
    TestNotNull(TEXT("Transient Niagara system created"), System);
    if (!System)
    {
        return false;
    }

    const bool bInitialFixedBounds = System->bFixedBounds;
    TSharedPtr<FJsonObject> Payload = MakeSystemPropertyPayload(
        ObjectPath,
        TEXT("DefinitelyNotANiagaraSystemProperty"),
        MakeShared<FJsonValueBoolean>(true));

    NiagaraEditTestUtils::InvokeExpectError(*this, TEXT("niagara.set_property"), Payload, TEXT("PROPERTY_NOT_FOUND"));
    TestEqual(TEXT("Invalid property path leaves existing property unchanged"), static_cast<bool>(System->bFixedBounds), bInitialFixedBounds);

    System->RemoveFromRoot();
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNiagaraEditSetPropertyMutatesTransientSystemTest,
    "PinWright.Assets.Niagara.Edit.SetProperty.MutatesTransientSystem",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraEditSetPropertyMutatesTransientSystemTest::RunTest(const FString& Parameters)
{
    FString ObjectPath;
    UNiagaraSystem* System = NiagaraEditTestUtils::NewTransientSystem(ObjectPath);
    TestNotNull(TEXT("Transient Niagara system created"), System);
    if (!System)
    {
        return false;
    }

    System->bFixedBounds = false;
    TSharedPtr<FJsonObject> Payload = MakeSystemPropertyPayload(
        ObjectPath,
        TEXT("bFixedBounds"),
        MakeShared<FJsonValueBoolean>(true));

    FTestResponseCapture Capture;
    if (NiagaraEditTestUtils::InvokeExpectSuccess(*this, TEXT("niagara.set_property"), Payload, Capture))
    {
        TestEqual(TEXT("operation is set_property"), Capture.Result->GetStringField(TEXT("operation")), FString(TEXT("set_property")));
        TestEqual(TEXT("propertyPath echoed"), Capture.Result->GetStringField(TEXT("propertyPath")), FString(TEXT("bFixedBounds")));
    }
    TestTrue(TEXT("set_property changed bFixedBounds"), static_cast<bool>(System->bFixedBounds));

    System->RemoveFromRoot();
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNiagaraEditSetParameterBadTypeRejectedTest,
    "PinWright.Assets.Niagara.Edit.SetParameter.BadParameterType",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraEditSetParameterBadTypeRejectedTest::RunTest(const FString& Parameters)
{
    FString ObjectPath;
    UNiagaraSystem* System = NiagaraEditTestUtils::NewTransientSystem(ObjectPath);
    TestNotNull(TEXT("Transient Niagara system created"), System);
    if (!System)
    {
        return false;
    }

    const int32 InitialParameterCount = System->GetExposedParameters().Num();
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), ObjectPath);
    Payload->SetStringField(TEXT("scope"), TEXT("user"));
    Payload->SetStringField(TEXT("name"), TEXT("User.BadType"));
    Payload->SetStringField(TEXT("type"), TEXT("Matrix4x4"));
    Payload->SetNumberField(TEXT("value"), 1.0);

    NiagaraEditTestUtils::InvokeExpectError(*this, TEXT("niagara.set_parameter"), Payload, TEXT("INVALID_PARAMETER_TYPE"));
    TestEqual(TEXT("Bad parameter type does not add user parameter"), System->GetExposedParameters().Num(), InitialParameterCount);

    System->RemoveFromRoot();
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNiagaraEditLinearColorUserParameterMutationTest,
    "PinWright.Assets.Niagara.Edit.Parameter.LinearColorMutation",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraEditLinearColorUserParameterMutationTest::RunTest(const FString& Parameters)
{
    FString ObjectPath;
    UNiagaraSystem* System = NiagaraEditTestUtils::NewTransientSystem(ObjectPath);
    TestNotNull(TEXT("Transient Niagara system created"), System);
    if (!System)
    {
        return false;
    }

    const FName ParameterName(TEXT("User.EditTestColor"));
    FNiagaraVariable ColorVariable(FNiagaraTypeDefinition::GetColorDef(), ParameterName);

    TSharedPtr<FJsonObject> InitialColor = MakeShared<FJsonObject>();
    InitialColor->SetNumberField(TEXT("r"), 0.25);
    InitialColor->SetNumberField(TEXT("g"), 0.5);
    InitialColor->SetNumberField(TEXT("b"), 0.75);
    InitialColor->SetNumberField(TEXT("a"), 1.0);

    TSharedPtr<FJsonObject> AddPayload = MakeShared<FJsonObject>();
    AddPayload->SetStringField(TEXT("assetPath"), ObjectPath);
    AddPayload->SetStringField(TEXT("scope"), TEXT("user"));
    AddPayload->SetStringField(TEXT("name"), ParameterName.ToString());
    AddPayload->SetStringField(TEXT("type"), TEXT("LinearColor"));
    AddPayload->SetObjectField(TEXT("defaultValue"), InitialColor);

    FTestResponseCapture AddCapture;
    NiagaraEditTestUtils::InvokeExpectSuccess(*this, TEXT("niagara.add_parameter"), AddPayload, AddCapture);
    TestTrue(TEXT("LinearColor user parameter added"), System->GetExposedParameters().IndexOf(ColorVariable) != INDEX_NONE);

    TSharedPtr<FJsonObject> UpdatedColor = MakeShared<FJsonObject>();
    UpdatedColor->SetNumberField(TEXT("r"), 1.0);
    UpdatedColor->SetNumberField(TEXT("g"), 0.125);
    UpdatedColor->SetNumberField(TEXT("b"), 0.25);
    UpdatedColor->SetNumberField(TEXT("a"), 0.5);

    TSharedPtr<FJsonObject> SetPayload = MakeShared<FJsonObject>();
    SetPayload->SetStringField(TEXT("assetPath"), ObjectPath);
    SetPayload->SetStringField(TEXT("scope"), TEXT("user"));
    SetPayload->SetStringField(TEXT("name"), ParameterName.ToString());
    SetPayload->SetStringField(TEXT("type"), TEXT("LinearColor"));
    SetPayload->SetObjectField(TEXT("value"), UpdatedColor);

    FTestResponseCapture SetCapture;
    NiagaraEditTestUtils::InvokeExpectSuccess(*this, TEXT("niagara.set_parameter"), SetPayload, SetCapture);

    const FLinearColor StoredColor = System->GetExposedParameters().GetParameterValue<FLinearColor>(ColorVariable);
    TestEqual(TEXT("LinearColor red changed"), StoredColor.R, 1.0f);
    TestEqual(TEXT("LinearColor green changed"), StoredColor.G, 0.125f);
    TestEqual(TEXT("LinearColor blue changed"), StoredColor.B, 0.25f);
    TestEqual(TEXT("LinearColor alpha changed"), StoredColor.A, 0.5f);

    System->RemoveFromRoot();
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNiagaraEditNiagaraBoolAliasParameterMutationTest,
    "PinWright.Assets.Niagara.Edit.Parameter.NiagaraBoolAliasMutation",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraEditNiagaraBoolAliasParameterMutationTest::RunTest(const FString& Parameters)
{
    FString ObjectPath;
    UNiagaraSystem* System = NiagaraEditTestUtils::NewTransientSystem(ObjectPath);
    TestNotNull(TEXT("Transient Niagara system created"), System);
    if (!System)
    {
        return false;
    }

    const FName ParameterName(TEXT("User.EditNiagaraBoolAlias"));
    const FNiagaraTypeDefinition NiagaraBoolType(FNiagaraBool::StaticStruct());
    const FNiagaraVariable BoolVariable(NiagaraBoolType, ParameterName);
    const FNiagaraBool InitialValue(false);
    System->GetExposedParameters().SetParameterData(reinterpret_cast<const uint8*>(&InitialValue), BoolVariable, /*bAdd=*/true);

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), ObjectPath);
    Payload->SetStringField(TEXT("scope"), TEXT("user"));
    Payload->SetStringField(TEXT("name"), ParameterName.ToString());
    Payload->SetStringField(TEXT("type"), TEXT("NiagaraBool"));
    Payload->SetBoolField(TEXT("value"), true);

    FTestResponseCapture Capture;
    NiagaraEditTestUtils::InvokeExpectSuccess(*this, TEXT("niagara.set_parameter"), Payload, Capture);

    FNiagaraBool StoredValue(false);
    const bool bCopied = System->GetExposedParameters().CopyParameterData(BoolVariable, reinterpret_cast<uint8*>(&StoredValue));
    TestTrue(TEXT("NiagaraBool alias CopyParameterData succeeds"), bCopied);
    if (bCopied)
    {
        TestTrue(TEXT("NiagaraBool alias changed value"), StoredValue.GetValue());
    }

    System->RemoveFromRoot();
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNiagaraEditRemoveParameterMutatesTransientSystemTest,
    "PinWright.Assets.Niagara.Edit.Parameter.RemoveMutation",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraEditRemoveParameterMutatesTransientSystemTest::RunTest(const FString& Parameters)
{
    FString ObjectPath;
    UNiagaraSystem* System = NiagaraEditTestUtils::NewTransientSystem(ObjectPath);
    TestNotNull(TEXT("Transient Niagara system created"), System);
    if (!System)
    {
        return false;
    }

    const FName ParameterName(TEXT("User.EditRemoveFloat"));
    const FNiagaraVariable FloatVariable(FNiagaraTypeDefinition::GetFloatDef(), ParameterName);
    System->GetExposedParameters().SetParameterValue(7.0f, FloatVariable, true);
    TestTrue(TEXT("Fixture parameter exists before remove"), System->GetExposedParameters().IndexOf(FloatVariable) != INDEX_NONE);

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), ObjectPath);
    Payload->SetStringField(TEXT("scope"), TEXT("user"));
    Payload->SetStringField(TEXT("name"), ParameterName.ToString());

    FTestResponseCapture Capture;
    if (NiagaraEditTestUtils::InvokeExpectSuccess(*this, TEXT("niagara.remove_parameter"), Payload, Capture))
    {
        TestEqual(TEXT("operation is remove_parameter"), Capture.Result->GetStringField(TEXT("operation")), FString(TEXT("remove_parameter")));
    }
    TestTrue(TEXT("Fixture parameter removed"), System->GetExposedParameters().IndexOf(FloatVariable) == INDEX_NONE);

    System->RemoveFromRoot();
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNiagaraEditRendererMissingEmitterTest,
    "PinWright.Assets.Niagara.Edit.Renderer.MissingEmitter",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraEditRendererMissingEmitterTest::RunTest(const FString& Parameters)
{
    FString ObjectPath;
    UNiagaraSystem* System = NiagaraEditTestUtils::NewTransientSystem(ObjectPath);
    TestNotNull(TEXT("Transient Niagara system created"), System);
    if (!System)
    {
        return false;
    }

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), ObjectPath);
    Payload->SetStringField(TEXT("rendererClassPath"), TEXT("NiagaraSpriteRendererProperties"));

    NiagaraEditTestUtils::InvokeExpectError(*this, TEXT("niagara.add_renderer"), Payload, TEXT("EMITTER_REQUIRED"));

    System->RemoveFromRoot();
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNiagaraEditInvalidRendererIndexTest,
    "PinWright.Assets.Niagara.Edit.Renderer.InvalidIndex",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraEditInvalidRendererIndexTest::RunTest(const FString& Parameters)
{
    FString ObjectPath;
    UNiagaraEmitter* Emitter = NiagaraEditTestUtils::NewTransientEmitter(ObjectPath);
    TestNotNull(TEXT("Transient Niagara emitter created"), Emitter);
    if (!Emitter)
    {
        return false;
    }

    FVersionedNiagaraEmitterData* EmitterData = Emitter->GetLatestEmitterData();
    TestNotNull(TEXT("Emitter data exists"), EmitterData);
    if (!EmitterData)
    {
        Emitter->RemoveFromRoot();
        return false;
    }

    const int32 InitialRendererCount = EmitterData->GetRenderers().Num();
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), ObjectPath);
    Payload->SetNumberField(TEXT("index"), InitialRendererCount);

    NiagaraEditTestUtils::InvokeExpectError(*this, TEXT("niagara.remove_renderer"), Payload, TEXT("RENDERER_NOT_FOUND"));
    TestEqual(TEXT("Invalid renderer index leaves renderer count unchanged"), EmitterData->GetRenderers().Num(), InitialRendererCount);

    Emitter->RemoveFromRoot();
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNiagaraEditRendererAddRemoveTransientEmitterTest,
    "PinWright.Assets.Niagara.Edit.Renderer.AddRemoveTransientEmitter",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraEditRendererAddRemoveTransientEmitterTest::RunTest(const FString& Parameters)
{
    FString ObjectPath;
    UNiagaraEmitter* Emitter = NiagaraEditTestUtils::NewTransientEmitter(ObjectPath);
    TestNotNull(TEXT("Transient Niagara emitter created"), Emitter);
    if (!Emitter)
    {
        return false;
    }

    FVersionedNiagaraEmitterData* EmitterData = Emitter->GetLatestEmitterData();
    TestNotNull(TEXT("Emitter data exists"), EmitterData);
    if (!EmitterData)
    {
        Emitter->RemoveFromRoot();
        return false;
    }

    const int32 InitialRendererCount = EmitterData->GetRenderers().Num();
    TSharedPtr<FJsonObject> AddPayload = MakeShared<FJsonObject>();
    AddPayload->SetStringField(TEXT("assetPath"), ObjectPath);
    AddPayload->SetStringField(TEXT("rendererClassPath"), TEXT("NiagaraSpriteRendererProperties"));

    FTestResponseCapture AddCapture;
    NiagaraEditTestUtils::InvokeExpectSuccess(*this, TEXT("niagara.add_renderer"), AddPayload, AddCapture);
    TestEqual(TEXT("Renderer count increases after add_renderer"), EmitterData->GetRenderers().Num(), InitialRendererCount + 1);
    TestNotNull(TEXT("Added renderer stored in emitter data"), EmitterData->GetRenderers().IsValidIndex(InitialRendererCount) ? EmitterData->GetRenderers()[InitialRendererCount] : nullptr);

    TSharedPtr<FJsonObject> RemovePayload = MakeShared<FJsonObject>();
    RemovePayload->SetStringField(TEXT("assetPath"), ObjectPath);
    RemovePayload->SetNumberField(TEXT("index"), InitialRendererCount);

    FTestResponseCapture RemoveCapture;
    NiagaraEditTestUtils::InvokeExpectSuccess(*this, TEXT("niagara.remove_renderer"), RemovePayload, RemoveCapture);
    TestEqual(TEXT("Renderer count returns after remove_renderer"), EmitterData->GetRenderers().Num(), InitialRendererCount);

    Emitter->RemoveFromRoot();
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNiagaraEditRendererMoveTransientEmitterTest,
    "PinWright.Assets.Niagara.Edit.Renderer.MoveTransientEmitter",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraEditRendererMoveTransientEmitterTest::RunTest(const FString& Parameters)
{
    FString ObjectPath;
    UNiagaraEmitter* Emitter = NiagaraEditTestUtils::NewTransientEmitter(ObjectPath);
    TestNotNull(TEXT("Transient Niagara emitter created"), Emitter);
    if (!Emitter)
    {
        return false;
    }

    FVersionedNiagaraEmitterData* EmitterData = Emitter->GetLatestEmitterData();
    TestNotNull(TEXT("Emitter data exists"), EmitterData);
    if (!EmitterData)
    {
        Emitter->RemoveFromRoot();
        return false;
    }

    const int32 InitialRendererCount = EmitterData->GetRenderers().Num();
    for (int32 Index = 0; Index < 2; ++Index)
    {
        TSharedPtr<FJsonObject> AddPayload = MakeShared<FJsonObject>();
        AddPayload->SetStringField(TEXT("assetPath"), ObjectPath);
        AddPayload->SetStringField(TEXT("rendererClassPath"), TEXT("NiagaraSpriteRendererProperties"));
        FTestResponseCapture AddCapture;
        NiagaraEditTestUtils::InvokeExpectSuccess(*this, TEXT("niagara.add_renderer"), AddPayload, AddCapture);
    }
    TestEqual(TEXT("Renderer fixture has two added renderers"), EmitterData->GetRenderers().Num(), InitialRendererCount + 2);

    TSharedPtr<FJsonObject> MovePayload = MakeShared<FJsonObject>();
    MovePayload->SetStringField(TEXT("assetPath"), ObjectPath);
    MovePayload->SetNumberField(TEXT("index"), InitialRendererCount);
    MovePayload->SetNumberField(TEXT("toIndex"), InitialRendererCount + 1);

    FTestResponseCapture MoveCapture;
    if (NiagaraEditTestUtils::InvokeExpectSuccess(*this, TEXT("niagara.move_renderer"), MovePayload, MoveCapture))
    {
        TestEqual(TEXT("operation is move_renderer"), MoveCapture.Result->GetStringField(TEXT("operation")), FString(TEXT("move_renderer")));
    }
    TestEqual(TEXT("Renderer count unchanged by move_renderer"), EmitterData->GetRenderers().Num(), InitialRendererCount + 2);

    Emitter->RemoveFromRoot();
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNiagaraEditPinInvalidNodeTest,
    "PinWright.Assets.Niagara.Edit.Graph.InvalidNode",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraEditPinInvalidNodeTest::RunTest(const FString& Parameters)
{
    FString ObjectPath;
    UNiagaraSystem* System = NiagaraEditTestUtils::NewTransientSystem(ObjectPath);
    TestNotNull(TEXT("Transient Niagara system created"), System);
    if (!System)
    {
        return false;
    }

    UNiagaraGraph* Graph = nullptr;
    TestTrue(TEXT("Minimal graph fixture attached"), AttachMinimalSystemGraph(System, Graph));
    if (!Graph)
    {
        System->RemoveFromRoot();
        return false;
    }

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), ObjectPath);
    Payload->SetStringField(TEXT("nodeId"), TEXT("MissingGraphNode"));
    Payload->SetStringField(TEXT("pin"), TEXT("AnyPin"));
    Payload->SetNumberField(TEXT("defaultValue"), 1.0);

    NiagaraEditTestUtils::InvokeExpectError(*this, TEXT("niagara.set_pin_default"), Payload, TEXT("NODE_NOT_FOUND"));

    System->RemoveFromRoot();
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNiagaraEditPinInvalidPinTest,
    "PinWright.Assets.Niagara.Edit.Graph.InvalidPin",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraEditPinInvalidPinTest::RunTest(const FString& Parameters)
{
    FString ObjectPath;
    UNiagaraSystem* System = NiagaraEditTestUtils::NewTransientSystem(ObjectPath);
    TestNotNull(TEXT("Transient Niagara system created"), System);
    if (!System)
    {
        return false;
    }

    UNiagaraGraph* Graph = nullptr;
    TestTrue(TEXT("Minimal graph fixture attached"), AttachMinimalSystemGraph(System, Graph));
    UEdGraphNode* Node = AddMinimalGraphNode(Graph, TEXT("NodeWithoutRequestedPin"));
    TestNotNull(TEXT("Minimal graph node created"), Node);
    if (!Graph || !Node)
    {
        System->RemoveFromRoot();
        return false;
    }

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), ObjectPath);
    Payload->SetStringField(TEXT("nodeId"), Node->NodeGuid.ToString());
    Payload->SetStringField(TEXT("pin"), TEXT("MissingPin"));
    Payload->SetNumberField(TEXT("defaultValue"), 1.0);

    NiagaraEditTestUtils::InvokeExpectError(*this, TEXT("niagara.set_pin_default"), Payload, TEXT("PIN_NOT_FOUND"));

    System->RemoveFromRoot();
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNiagaraEditPinDefaultMutatesLowRiskGraphFixtureTest,
    "PinWright.Assets.Niagara.Edit.Graph.SetPinDefaultLowRiskFixture",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraEditPinDefaultMutatesLowRiskGraphFixtureTest::RunTest(const FString& Parameters)
{
    FString ObjectPath;
    UNiagaraSystem* System = NiagaraEditTestUtils::NewTransientSystem(ObjectPath);
    TestNotNull(TEXT("Transient Niagara system created"), System);
    if (!System)
    {
        return false;
    }

    UNiagaraGraph* Graph = nullptr;
    TestTrue(TEXT("Minimal graph fixture attached"), AttachMinimalSystemGraph(System, Graph));
    UEdGraphNode* Node = AddMinimalGraphNode(Graph, TEXT("NodeWithEditablePin"));
    TestNotNull(TEXT("Minimal graph node created"), Node);
    if (!Graph || !Node)
    {
        System->RemoveFromRoot();
        return false;
    }

    // Full stack module mutation needs a compiled Niagara stack fixture; this lower-risk graph fixture still exercises parser, validation, schema defaulting, and data mutation.
    UEdGraphPin* Pin = Node->CreatePin(
        EGPD_Input,
        UEdGraphSchema_Niagara::TypeDefinitionToPinType(FNiagaraTypeDefinition::GetFloatDef()),
        FName(TEXT("EditableFloat")));
    TestNotNull(TEXT("Editable graph pin created"), Pin);
    if (!Pin)
    {
        System->RemoveFromRoot();
        return false;
    }
    Pin->DefaultValue = TEXT("1.0");

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), ObjectPath);
    Payload->SetStringField(TEXT("nodeId"), Node->NodeGuid.ToString());
    Payload->SetStringField(TEXT("pin"), TEXT("EditableFloat"));
    Payload->SetNumberField(TEXT("defaultValue"), 42.0);

    FTestResponseCapture Capture;
    if (NiagaraEditTestUtils::InvokeExpectSuccess(*this, TEXT("niagara.set_pin_default"), Payload, Capture))
    {
        TestEqual(TEXT("operation is set_pin_default"), Capture.Result->GetStringField(TEXT("operation")), FString(TEXT("set_pin_default")));
        TestEqual(TEXT("result defaultValue matches pin"), Capture.Result->GetStringField(TEXT("defaultValue")), Pin->DefaultValue);
    }
    TestNotEqual(TEXT("Pin default changed from fixture value"), Pin->DefaultValue, FString(TEXT("1.0")));

    System->RemoveFromRoot();
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNiagaraEditModuleSurfacesStructuredErrorsTest,
    "PinWright.Assets.Niagara.Edit.Module.StructuredErrors",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraEditModuleSurfacesStructuredErrorsTest::RunTest(const FString& Parameters)
{
    FString ObjectPath;
    UNiagaraSystem* System = NiagaraEditTestUtils::NewTransientSystem(ObjectPath);
    TestNotNull(TEXT("Transient Niagara system created"), System);
    if (!System)
    {
        return false;
    }

    UNiagaraGraph* Graph = nullptr;
    TestTrue(TEXT("Minimal graph fixture attached"), AttachMinimalSystemGraph(System, Graph));
    if (!Graph)
    {
        System->RemoveFromRoot();
        return false;
    }

    TSharedPtr<FJsonObject> AddPayload = MakeShared<FJsonObject>();
    AddPayload->SetStringField(TEXT("assetPath"), ObjectPath);
    AddPayload->SetStringField(TEXT("modulePath"), TEXT("/Game/PinWrightTests/MissingModule.MissingModule"));
    AddPayload->SetStringField(TEXT("scriptUsage"), TEXT("SystemSpawn"));
    NiagaraEditTestUtils::InvokeExpectError(*this, TEXT("niagara.add_module"), AddPayload, TEXT("OUTPUT_NODE_NOT_FOUND"));

    TSharedPtr<FJsonObject> RemovePayload = MakeShared<FJsonObject>();
    RemovePayload->SetStringField(TEXT("assetPath"), ObjectPath);
    RemovePayload->SetStringField(TEXT("entryId"), TEXT("MissingModuleEntry"));
    RemovePayload->SetStringField(TEXT("scriptUsage"), TEXT("SystemSpawn"));
    NiagaraEditTestUtils::InvokeExpectError(*this, TEXT("niagara.remove_module"), RemovePayload, TEXT("MODULE_NOT_FOUND"));

    TSharedPtr<FJsonObject> MovePayload = MakeShared<FJsonObject>();
    MovePayload->SetStringField(TEXT("assetPath"), ObjectPath);
    MovePayload->SetStringField(TEXT("entryId"), TEXT("MissingModuleEntry"));
    MovePayload->SetStringField(TEXT("scriptUsage"), TEXT("SystemSpawn"));
    MovePayload->SetNumberField(TEXT("toIndex"), 0);
    NiagaraEditTestUtils::InvokeExpectError(*this, TEXT("niagara.move_module"), MovePayload, TEXT("MODULE_NOT_FOUND"));

    TSharedPtr<FJsonObject> EnabledPayload = MakeShared<FJsonObject>();
    EnabledPayload->SetStringField(TEXT("assetPath"), ObjectPath);
    EnabledPayload->SetStringField(TEXT("entryId"), TEXT("MissingModuleEntry"));
    EnabledPayload->SetStringField(TEXT("scriptUsage"), TEXT("SystemSpawn"));
    EnabledPayload->SetBoolField(TEXT("enabled"), false);
    NiagaraEditTestUtils::InvokeExpectError(*this, TEXT("niagara.set_stack_enabled"), EnabledPayload, TEXT("MODULE_NOT_FOUND"));

    TSharedPtr<FJsonObject> InputPayload = MakeShared<FJsonObject>();
    InputPayload->SetStringField(TEXT("assetPath"), ObjectPath);
    InputPayload->SetStringField(TEXT("entryId"), TEXT("MissingModuleEntry"));
    InputPayload->SetStringField(TEXT("scriptUsage"), TEXT("SystemSpawn"));
    InputPayload->SetStringField(TEXT("inputName"), TEXT("SpawnRate"));
    InputPayload->SetNumberField(TEXT("value"), 60.0);
    NiagaraEditTestUtils::InvokeExpectError(*this, TEXT("niagara.set_module_input"), InputPayload, TEXT("MODULE_NOT_FOUND"));

    System->RemoveFromRoot();
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNiagaraEditConnectDisconnectPinStructuredErrorsTest,
    "PinWright.Assets.Niagara.Edit.Graph.ConnectDisconnectStructuredErrors",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraEditConnectDisconnectPinStructuredErrorsTest::RunTest(const FString& Parameters)
{
    FString ObjectPath;
    UNiagaraSystem* System = NiagaraEditTestUtils::NewTransientSystem(ObjectPath);
    TestNotNull(TEXT("Transient Niagara system created"), System);
    if (!System)
    {
        return false;
    }

    UNiagaraGraph* Graph = nullptr;
    TestTrue(TEXT("Minimal graph fixture attached"), AttachMinimalSystemGraph(System, Graph));
    if (!Graph)
    {
        System->RemoveFromRoot();
        return false;
    }

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), ObjectPath);
    Payload->SetStringField(TEXT("fromNode"), TEXT("MissingSourceNode"));
    Payload->SetStringField(TEXT("fromPin"), TEXT("Out"));
    Payload->SetStringField(TEXT("toNode"), TEXT("MissingTargetNode"));
    Payload->SetStringField(TEXT("toPin"), TEXT("In"));
    Payload->SetStringField(TEXT("scriptUsage"), TEXT("SystemSpawn"));

    NiagaraEditTestUtils::InvokeExpectError(*this, TEXT("niagara.connect_pin"), Payload, TEXT("NODE_NOT_FOUND"));
    NiagaraEditTestUtils::InvokeExpectError(*this, TEXT("niagara.disconnect_pin"), Payload, TEXT("NODE_NOT_FOUND"));

    System->RemoveFromRoot();
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNiagaraEditOperationsArrayRejectedTest,
    "PinWright.Assets.Niagara.Edit.OneOperation.RejectsOperationsArray",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraEditOperationsArrayRejectedTest::RunTest(const FString& Parameters)
{
    FString ObjectPath;
    UNiagaraSystem* System = NiagaraEditTestUtils::NewTransientSystem(ObjectPath);
    TestNotNull(TEXT("Transient Niagara system created"), System);
    if (!System)
    {
        return false;
    }
    const int32 InitialParameterCount = System->GetExposedParameters().Num();

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), ObjectPath);
    Payload->SetStringField(TEXT("scope"), TEXT("user"));
    Payload->SetStringField(TEXT("name"), TEXT("User.BatchRejected"));
    Payload->SetStringField(TEXT("type"), TEXT("Float"));
    Payload->SetNumberField(TEXT("value"), 1.0);
    TArray<TSharedPtr<FJsonValue>> Operations;
    Operations.Add(MakeShared<FJsonValueObject>(MakeShared<FJsonObject>()));
    Payload->SetArrayField(TEXT("operations"), Operations);

    NiagaraEditTestUtils::InvokeExpectError(*this, TEXT("niagara.set_parameter"), Payload, TEXT("INVALID_ARGUMENT"));
    TestEqual(TEXT("operations rejection did not add valid-looking parameter"), System->GetExposedParameters().Num(), InitialParameterCount);

    System->RemoveFromRoot();
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNiagaraEditCustomStructParameterMutationTest,
    "PinWright.Assets.Niagara.Edit.Parameter.CustomStructMutation",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraEditCustomStructParameterMutationTest::RunTest(const FString& Parameters)
{
    FString ObjectPath;
    UNiagaraSystem* System = NiagaraEditTestUtils::NewTransientSystem(ObjectPath);
    TestNotNull(TEXT("Transient Niagara system created"), System);
    if (!System)
    {
        return false;
    }

    const FName ParameterName(TEXT("User.EditConvertedTransform"));
    UScriptStruct* TransformStruct = TBaseStructure<FTransform>::Get();
    FNiagaraTypeHelper::GetSWCStruct(TransformStruct);
    const FNiagaraVariable TransformVar(FNiagaraTypeRegistry::GetTypeForStruct(TransformStruct), ParameterName);
    const FTransform InitialValue(FQuat::Identity, FVector(1.0, 2.0, 3.0), FVector::OneVector);
    System->GetExposedParameters().SetParameterData(reinterpret_cast<const uint8*>(&InitialValue), TransformVar, /*bAdd=*/true);

    TSharedPtr<FJsonObject> Value = MakeShared<FJsonObject>();
    TArray<TSharedPtr<FJsonValue>> Translation;
    Translation.Add(MakeShared<FJsonValueNumber>(4.25));
    Translation.Add(MakeShared<FJsonValueNumber>(-8.5));
    Translation.Add(MakeShared<FJsonValueNumber>(16.0));
    Value->SetArrayField(TEXT("Translation"), Translation);

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), ObjectPath);
    Payload->SetStringField(TEXT("scope"), TEXT("user"));
    Payload->SetStringField(TEXT("name"), ParameterName.ToString());
    Payload->SetStringField(TEXT("type"), TransformStruct->GetPathName());
    Payload->SetObjectField(TEXT("value"), Value);

    FTestResponseCapture Capture;
    NiagaraEditTestUtils::InvokeExpectSuccess(*this, TEXT("niagara.set_parameter"), Payload, Capture);

    FTransform StoredValue = FTransform::Identity;
    const bool bCopied = System->GetExposedParameters().CopyParameterData(TransformVar, reinterpret_cast<uint8*>(&StoredValue));
    TestTrue(TEXT("CopyParameterData returns source struct bytes"), bCopied);
    if (bCopied)
    {
        TestEqual(TEXT("converted transform Translation.X"), StoredValue.GetTranslation().X, 4.25);
        TestEqual(TEXT("converted transform Translation.Y"), StoredValue.GetTranslation().Y, -8.5);
        TestEqual(TEXT("converted transform Translation.Z"), StoredValue.GetTranslation().Z, 16.0);
    }

    System->RemoveFromRoot();
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNiagaraEditVectorStructPathParameterMutationTest,
    "PinWright.Assets.Niagara.Edit.Parameter.VectorStructPathMutation",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraEditVectorStructPathParameterMutationTest::RunTest(const FString& Parameters)
{
    FString ObjectPath;
    UNiagaraSystem* System = NiagaraEditTestUtils::NewTransientSystem(ObjectPath);
    TestNotNull(TEXT("Transient Niagara system created"), System);
    if (!System)
    {
        return false;
    }

    const FName ParameterName(TEXT("User.EditVectorStructPath"));
    const FNiagaraVariable VectorVar(FNiagaraTypeDefinition::GetVec3Def(), ParameterName);
    const FVector3f InitialValue(1.0f, 2.0f, 3.0f);
    System->GetExposedParameters().SetParameterValue(InitialValue, VectorVar, /*bAdd=*/true);

    TSharedPtr<FJsonObject> Value = MakeShared<FJsonObject>();
    Value->SetNumberField(TEXT("x"), 6.0);
    Value->SetNumberField(TEXT("y"), -7.5);
    Value->SetNumberField(TEXT("z"), 12.0);

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), ObjectPath);
    Payload->SetStringField(TEXT("scope"), TEXT("user"));
    Payload->SetStringField(TEXT("name"), ParameterName.ToString());
    Payload->SetStringField(TEXT("type"), TEXT("/Script/CoreUObject.Vector"));
    Payload->SetObjectField(TEXT("value"), Value);

    FTestResponseCapture Capture;
    NiagaraEditTestUtils::InvokeExpectSuccess(*this, TEXT("niagara.set_parameter"), Payload, Capture);

    FVector3f StoredValue = FVector3f::ZeroVector;
    const bool bCopied = System->GetExposedParameters().CopyParameterData(VectorVar, reinterpret_cast<uint8*>(&StoredValue));
    TestTrue(TEXT("Vector path CopyParameterData succeeds"), bCopied);
    if (bCopied)
    {
        TestEqual(TEXT("vector path X"), StoredValue.X, 6.0f);
        TestEqual(TEXT("vector path Y"), StoredValue.Y, -7.5f);
        TestEqual(TEXT("vector path Z"), StoredValue.Z, 12.0f);
    }

    System->RemoveFromRoot();
    return true;
}

// Regression for E-niagara-set-property-emitter-alias: the EmitterHandle target kind must wire
// FNiagaraEmitterHandle as its reflected container so handle UPROPERTYs (bIsEnabled) are settable
// through kind:"emitter". Before the fix ResolveTarget's EmitterHandle case returned success but
// set no ReflectedStruct/Container, so this set_property failed with UNSUPPORTED_TARGET. This test
// fails (reverts to UNSUPPORTED_TARGET / no mutation) if that wiring is removed.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNiagaraEditSetPropertyEmitterHandleEnabledTest,
    "PinWright.Assets.Niagara.Edit.SetProperty.EmitterHandleEnabled",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraEditSetPropertyEmitterHandleEnabledTest::RunTest(const FString& Parameters)
{
    FString SystemObjectPath;
    UNiagaraSystem* System = nullptr;
    UNiagaraEmitter* SourceEmitter = nullptr;
    FName EmitterName;
    const bool bSetupOk = NiagaraEditTestUtils::MakeAuthorableSystem(SystemObjectPath, System, SourceEmitter, EmitterName);
    NiagaraEditTestUtils::FAuthorableSystemRoots Roots{System, SourceEmitter};
    if (!TestTrue(TEXT("authorable system created"), bSetupOk))
    {
        return false;
    }
    if (!TestEqual(TEXT("System has one emitter handle"), System->GetEmitterHandles().Num(), 1))
    {
        return false;
    }

    // bIsEnabled is a UPROPERTY() on FNiagaraEmitterHandle (NiagaraEmitterHandle.h) — reachable only
    // through the handle kind, never through emitterData. Flip it from its current value via kind:"emitter".
    const bool bInitialEnabled = System->GetEmitterHandles()[0].GetIsEnabled();

    TSharedPtr<FJsonObject> Target = MakeShared<FJsonObject>();
    Target->SetStringField(TEXT("kind"), TEXT("emitter"));
    Target->SetStringField(TEXT("emitter"), EmitterName.ToString());

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), SystemObjectPath);
    Payload->SetObjectField(TEXT("target"), Target);
    Payload->SetStringField(TEXT("propertyPath"), TEXT("bIsEnabled"));
    Payload->SetBoolField(TEXT("value"), !bInitialEnabled);

    FTestResponseCapture Capture;
    if (NiagaraEditTestUtils::InvokeExpectSuccess(*this, TEXT("niagara.set_property"), Payload, Capture))
    {
        TestEqual(TEXT("operation is set_property"), Capture.Result->GetStringField(TEXT("operation")), FString(TEXT("set_property")));
        TestEqual(TEXT("propertyPath echoed"), Capture.Result->GetStringField(TEXT("propertyPath")), FString(TEXT("bIsEnabled")));
    }
    TestEqual(TEXT("emitter-handle bIsEnabled flipped via kind:emitter"), System->GetEmitterHandles()[0].GetIsEnabled(), !bInitialEnabled);

    return true;
}
