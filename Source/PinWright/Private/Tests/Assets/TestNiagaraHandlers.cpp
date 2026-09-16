// Copyright (c) 2026 Alexander Penkin. MIT License.

// Unit tests for Niagara domain handlers
// Covers: NiagaraHandler.cpp, NiagaraGraphHandler.cpp, canonical Niagara inspection, and removed authoring registrations.
#include "Misc/AutomationTest.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/HandlerRegistration.h"
#include "Handlers/ParamSpec.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "NiagaraJsonAssertionHelpers.h"
#include "Tests/TestUtils.h"

#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraph/EdGraphSchema.h"
#include "EdGraphSchema_Niagara.h"
#include "Misc/Guid.h"
#include "NiagaraEmitter.h"
#include "NiagaraGraph.h"
#include "NiagaraNode.h"
#include "NiagaraNodeOutput.h"
#include "NiagaraNodeReroute.h"
#include "NiagaraParameterStore.h"
#include "NiagaraScript.h"
#include "NiagaraScriptSource.h"
#include "NiagaraSystem.h"
#include "NiagaraTypes.h"
#include "UObject/Package.h"


namespace
{
    using NiagaraJsonAssertionHelpers::HasArrayField;
    using NiagaraJsonAssertionHelpers::ArrayFieldNum;

    UNiagaraSystem* NewTransientHandlerTestSystem(FString& OutObjectPath)
    {
        const FString AssetName = FString::Printf(
            TEXT("NS_Handler_%s"),
            *FGuid::NewGuid().ToString(EGuidFormats::Digits));
        const FString PackageName = FString::Printf(TEXT("/Game/PinWrightTests/%s"), *AssetName);
        UPackage* Package = CreatePackage(*PackageName);
        UNiagaraSystem* System = NewObject<UNiagaraSystem>(
            Package,
            UNiagaraSystem::StaticClass(),
            *AssetName,
            RF_Public | RF_Standalone | RF_Transient);
        if (System)
        {
            System->AddToRoot();
            OutObjectPath = FString::Printf(TEXT("%s.%s"), *PackageName, *AssetName);
        }
        return System;
    }

    UNiagaraScript* NewTransientHandlerTestScript(FString& OutObjectPath)
    {
        const FString AssetName = FString::Printf(
            TEXT("NM_Handler_%s"),
            *FGuid::NewGuid().ToString(EGuidFormats::Digits));
        const FString PackageName = FString::Printf(TEXT("/Game/PinWrightTests/%s"), *AssetName);
        UPackage* Package = CreatePackage(*PackageName);
        UNiagaraScript* Script = NewObject<UNiagaraScript>(
            Package,
            UNiagaraScript::StaticClass(),
            *AssetName,
            RF_Public | RF_Standalone | RF_Transient);
        if (Script)
        {
            Script->SetUsage(ENiagaraScriptUsage::Module);
            Script->SetUsageId(FGuid::NewGuid());
            Script->AddToRoot();
            OutObjectPath = FString::Printf(TEXT("%s.%s"), *PackageName, *AssetName);
        }
        return Script;
    }

    bool HasObjectField(const TSharedPtr<FJsonObject>& Object, const TCHAR* FieldName)
    {
        return Object.IsValid() && Object->HasTypedField<EJson::Object>(FieldName);
    }

    bool GraphArrayHasNodeWithPin(const TSharedPtr<FJsonObject>& Object)
    {
        const TArray<TSharedPtr<FJsonValue>>* Graphs = nullptr;
        if (!Object.IsValid() || !Object->TryGetArrayField(TEXT("graphs"), Graphs) || !Graphs)
        {
            return false;
        }

        for (const TSharedPtr<FJsonValue>& GraphValue : *Graphs)
        {
            TSharedPtr<FJsonObject> Graph = GraphValue.IsValid() ? GraphValue->AsObject() : nullptr;
            const TArray<TSharedPtr<FJsonValue>>* Nodes = nullptr;
            if (!Graph.IsValid() || !Graph->TryGetArrayField(TEXT("nodes"), Nodes) || !Nodes)
            {
                continue;
            }

            for (const TSharedPtr<FJsonValue>& NodeValue : *Nodes)
            {
                TSharedPtr<FJsonObject> Node = NodeValue.IsValid() ? NodeValue->AsObject() : nullptr;
                if (ArrayFieldNum(Node, TEXT("pins")) > 0)
                {
                    return true;
                }
            }
        }
        return false;
    }

    void AttachHandlerTestSystemGraph(UNiagaraSystem* System)
    {
        if (!System || !System->GetSystemSpawnScript() || !System->GetSystemUpdateScript())
        {
            return;
        }

        UNiagaraScriptSource* Source = NewObject<UNiagaraScriptSource>(
            System->GetSystemSpawnScript(),
            TEXT("HandlerTestSystemScriptSource"),
            RF_Transient | RF_Transactional);
        UNiagaraGraph* Graph = Source
            ? NewObject<UNiagaraGraph>(Source, TEXT("HandlerTestSystemScriptGraph"), RF_Transient | RF_Transactional)
            : nullptr;
        if (!Graph)
        {
            return;
        }

        Graph->Schema = UEdGraphSchema_Niagara::StaticClass();
        Source->NodeGraph = Graph;
        System->GetSystemSpawnScript()->SetLatestSource(Source);
        System->GetSystemUpdateScript()->SetLatestSource(Source);

        UEdGraphNode* Node = NewObject<UEdGraphNode>(Graph, UEdGraphNode::StaticClass(), TEXT("HandlerTestNode"), RF_Transient | RF_Transactional);
        if (Node)
        {
            Node->NodeGuid = FGuid::NewGuid();
            Graph->AddNode(Node, false, false);
            Node->CreatePin(
                EGPD_Input,
                UEdGraphSchema_Niagara::TypeDefinitionToPinType(FNiagaraTypeDefinition::GetFloatDef()),
                FName(TEXT("FixtureFloat")));
        }
    }

    struct FSharedHandlerTestSystemGraph
    {
        UNiagaraGraph* SpawnGraph = nullptr;
        UNiagaraGraph* UpdateGraph = nullptr;
        UEdGraphNode* SpawnFromNode = nullptr;
        UEdGraphNode* SpawnToNode = nullptr;
        UEdGraphNode* UpdateFromNode = nullptr;
        UEdGraphNode* UpdateToNode = nullptr;
        UEdGraphPin* SpawnFromPin = nullptr;
        UEdGraphPin* SpawnToPin = nullptr;
        UEdGraphPin* UpdateFromPin = nullptr;
        UEdGraphPin* UpdateToPin = nullptr;
        UNiagaraNode* SharedNode = nullptr;
        UEdGraphNode* DisconnectedNode = nullptr;
        UEdGraphPin* SharedFromPin = nullptr;
        UEdGraphPin* DisconnectedToPin = nullptr;
    };

    bool AttachSharedHandlerTestSystemGraph(
        UNiagaraSystem* System,
        FSharedHandlerTestSystemGraph& OutGraphs)
    {
        OutGraphs = FSharedHandlerTestSystemGraph{};
        if (!System || !System->GetSystemSpawnScript() || !System->GetSystemUpdateScript())
        {
            return false;
        }

        System->GetSystemSpawnScript()->SetUsage(ENiagaraScriptUsage::SystemSpawnScript);
        System->GetSystemUpdateScript()->SetUsage(ENiagaraScriptUsage::SystemUpdateScript);
        System->GetSystemSpawnScript()->SetUsageId(FGuid::NewGuid());
        System->GetSystemUpdateScript()->SetUsageId(FGuid::NewGuid());

        UNiagaraScriptSource* Source = NewObject<UNiagaraScriptSource>(
            System->GetSystemSpawnScript(), TEXT("SharedSystemSource"), RF_Transient | RF_Transactional);
        UNiagaraGraph* Graph = Source
            ? NewObject<UNiagaraGraph>(Source, TEXT("SharedSystemGraph"), RF_Transient | RF_Transactional)
            : nullptr;
        if (!Graph)
        {
            return false;
        }
        Graph->Schema = UEdGraphSchema_Niagara::StaticClass();
        Source->NodeGraph = Graph;
        System->GetSystemSpawnScript()->SetLatestSource(Source);
        System->GetSystemUpdateScript()->SetLatestSource(Source);
        OutGraphs.SpawnGraph = Graph;
        OutGraphs.UpdateGraph = Graph;

        auto MakeNode = [](UNiagaraGraph* NodeGraph, const TCHAR* NodeName) -> UNiagaraNode*
        {
            UNiagaraNode* Node = NodeGraph
                ? NewObject<UNiagaraNode>(NodeGraph, UNiagaraNode::StaticClass(), NodeName, RF_Transient | RF_Transactional)
                : nullptr;
            if (Node)
            {
                Node->NodeGuid = FGuid::NewGuid();
                NodeGraph->AddNode(Node, false, false);
            }
            return Node;
        };

        auto MakeRerouteNode = [](UNiagaraGraph* NodeGraph, const TCHAR* NodeName) -> UNiagaraNodeReroute*
        {
            UNiagaraNodeReroute* Node = NodeGraph
                ? NewObject<UNiagaraNodeReroute>(NodeGraph, UNiagaraNodeReroute::StaticClass(), NodeName, RF_Transient | RF_Transactional)
                : nullptr;
            if (Node)
            {
                Node->NodeGuid = FGuid::NewGuid();
                NodeGraph->AddNode(Node, false, false);
            }
            return Node;
        };

        const FEdGraphPinType FloatPinType =
            UEdGraphSchema_Niagara::TypeDefinitionToPinType(FNiagaraTypeDefinition::GetFloatDef());
        const FEdGraphPinType ParameterMapPinType =
            UEdGraphSchema_Niagara::TypeDefinitionToPinType(FNiagaraTypeDefinition::GetParameterMapDef());

        OutGraphs.SpawnFromNode = MakeRerouteNode(Graph, TEXT("SharedGraphSpawnFromNode"));
        OutGraphs.SpawnToNode = MakeNode(Graph, TEXT("SharedGraphSpawnToNode"));
        OutGraphs.UpdateFromNode = MakeRerouteNode(Graph, TEXT("SharedGraphUpdateFromNode"));
        OutGraphs.UpdateToNode = MakeNode(Graph, TEXT("SharedGraphUpdateToNode"));
        OutGraphs.SharedNode = MakeNode(Graph, TEXT("SharedGraphBothStagesNode"));
        OutGraphs.DisconnectedNode = MakeNode(Graph, TEXT("SharedGraphDisconnectedNode"));

        if (OutGraphs.SpawnFromNode)
        {
            OutGraphs.SpawnFromPin = OutGraphs.SpawnFromNode->CreatePin(
                EGPD_Output, FloatPinType, FName(TEXT("OutFloat")));
        }
        if (OutGraphs.SpawnToNode)
        {
            OutGraphs.SpawnToPin = OutGraphs.SpawnToNode->CreatePin(
                EGPD_Input, FloatPinType, FName(TEXT("InFloat")));
        }
        if (OutGraphs.UpdateFromNode)
        {
            OutGraphs.UpdateFromPin = OutGraphs.UpdateFromNode->CreatePin(
                EGPD_Output, FloatPinType, FName(TEXT("OutFloat")));
        }
        if (OutGraphs.UpdateToNode)
        {
            OutGraphs.UpdateToPin = OutGraphs.UpdateToNode->CreatePin(
                EGPD_Input, FloatPinType, FName(TEXT("InFloat")));
        }

        if (OutGraphs.SharedNode)
        {
            OutGraphs.SharedFromPin = OutGraphs.SharedNode->CreatePin(
                EGPD_Output, FloatPinType, FName(TEXT("OutFloat")));
        }
        if (OutGraphs.DisconnectedNode)
        {
            OutGraphs.DisconnectedToPin = OutGraphs.DisconnectedNode->CreatePin(
                EGPD_Input, FloatPinType, FName(TEXT("InFloat")));
        }

        auto AddScopePins = [&ParameterMapPinType](UEdGraphNode* Node)
        {
            if (Node)
            {
                Node->CreatePin(EGPD_Input, ParameterMapPinType, FName(TEXT("ScopeIn")));
                Node->CreatePin(EGPD_Output, ParameterMapPinType, FName(TEXT("ScopeOut")));
            }
        };
        AddScopePins(OutGraphs.SpawnFromNode);
        AddScopePins(OutGraphs.SpawnToNode);
        AddScopePins(OutGraphs.UpdateFromNode);
        AddScopePins(OutGraphs.UpdateToNode);
        AddScopePins(OutGraphs.SharedNode);

        auto MakeOutput = [Graph, &ParameterMapPinType](
            const TCHAR* Name, ENiagaraScriptUsage Usage, const FGuid& UsageId) -> UNiagaraNodeOutput*
        {
            UNiagaraNodeOutput* Output = NewObject<UNiagaraNodeOutput>(
                Graph, UNiagaraNodeOutput::StaticClass(), Name, RF_Transient | RF_Transactional);
            if (Output)
            {
                Output->NodeGuid = FGuid::NewGuid();
                Output->SetUsage(Usage);
                Output->SetUsageId(UsageId);
                Output->CreatePin(EGPD_Input, ParameterMapPinType, FName(TEXT("ScopeIn")));
                Graph->AddNode(Output, false, false);
            }
            return Output;
        };
        UNiagaraNodeOutput* SpawnOutput = MakeOutput(
            TEXT("SystemSpawnOutput"), ENiagaraScriptUsage::SystemSpawnScript,
            System->GetSystemSpawnScript()->GetUsageId());
        UNiagaraNodeOutput* UpdateOutput = MakeOutput(
            TEXT("SystemUpdateOutput"), ENiagaraScriptUsage::SystemUpdateScript,
            System->GetSystemUpdateScript()->GetUsageId());

        auto Link = [](UEdGraphNode* From, const TCHAR* FromPin, UEdGraphNode* To, const TCHAR* ToPin)
        {
            if (From && To)
            {
                UEdGraphPin* SourcePin = From->FindPin(FName(FromPin));
                UEdGraphPin* TargetPin = To->FindPin(FName(ToPin));
                if (SourcePin && TargetPin)
                {
                    SourcePin->MakeLinkTo(TargetPin);
                }
            }
        };
        Link(OutGraphs.SpawnFromNode, TEXT("ScopeOut"), OutGraphs.SpawnToNode, TEXT("ScopeIn"));
        Link(OutGraphs.SpawnToNode, TEXT("ScopeOut"), SpawnOutput, TEXT("ScopeIn"));
        Link(OutGraphs.UpdateFromNode, TEXT("ScopeOut"), OutGraphs.UpdateToNode, TEXT("ScopeIn"));
        Link(OutGraphs.UpdateToNode, TEXT("ScopeOut"), UpdateOutput, TEXT("ScopeIn"));
        Link(OutGraphs.SharedNode, TEXT("ScopeOut"), SpawnOutput, TEXT("ScopeIn"));
        Link(OutGraphs.SharedNode, TEXT("ScopeOut"), UpdateOutput, TEXT("ScopeIn"));

        return OutGraphs.SpawnGraph
            && OutGraphs.UpdateGraph
            && OutGraphs.SpawnFromNode
            && OutGraphs.SpawnToNode
            && OutGraphs.UpdateFromNode
            && OutGraphs.UpdateToNode
            && OutGraphs.SpawnFromPin
            && OutGraphs.SpawnToPin
            && OutGraphs.UpdateFromPin
            && OutGraphs.UpdateToPin
            && OutGraphs.SharedFromPin
            && OutGraphs.DisconnectedToPin
            && SpawnOutput
            && UpdateOutput;
    }

    void AttachHandlerTestScriptSource(UNiagaraScript* Script)
    {
        if (!Script)
        {
            return;
        }

        UNiagaraScriptSource* Source = NewObject<UNiagaraScriptSource>(
            Script,
            TEXT("HandlerTestStandaloneScriptSource"),
            RF_Transient | RF_Transactional);
        UNiagaraGraph* Graph = Source
            ? NewObject<UNiagaraGraph>(Source, TEXT("HandlerTestStandaloneScriptGraph"), RF_Transient | RF_Transactional)
            : nullptr;
        if (!Graph)
        {
            return;
        }

        Graph->Schema = UEdGraphSchema_Niagara::StaticClass();
        Source->NodeGraph = Graph;
        Script->SetLatestSource(Source);

        UEdGraphNode* Node = NewObject<UEdGraphNode>(Graph, UEdGraphNode::StaticClass(), TEXT("HandlerTestStandaloneNode"), RF_Transient | RF_Transactional);
        if (Node)
        {
            Node->NodeGuid = FGuid::NewGuid();
            Graph->AddNode(Node, false, false);
            Node->CreatePin(
                EGPD_Input,
                UEdGraphSchema_Niagara::TypeDefinitionToPinType(FNiagaraTypeDefinition::GetFloatDef()),
                FName(TEXT("FixtureFloat")));
        }
    }

    void EnsureHandlerTestEmitterGraphSource(UNiagaraEmitter* Emitter)
    {
        FVersionedNiagaraEmitterData* EmitterData = Emitter ? Emitter->GetLatestEmitterData() : nullptr;
        if (!EmitterData || EmitterData->GraphSource)
        {
            return;
        }

        UNiagaraScriptSource* Source = NewObject<UNiagaraScriptSource>(Emitter, TEXT("HandlerTestEmitterSource"), RF_Transient | RF_Transactional);
        UNiagaraGraph* Graph = Source
            ? NewObject<UNiagaraGraph>(Source, TEXT("HandlerTestEmitterGraph"), RF_Transient | RF_Transactional)
            : nullptr;
        if (Graph)
        {
            Graph->Schema = UEdGraphSchema_Niagara::StaticClass();
            Source->NodeGraph = Graph;
            EmitterData->GraphSource = Source;
        }
    }

    void PopulateRepresentativeHandlerSystem(UNiagaraSystem* System)
    {
        AttachHandlerTestSystemGraph(System);

        UNiagaraEmitter* Emitter = NewObject<UNiagaraEmitter>(
            System,
            FName(TEXT("HandlerTestEmitterAsset")),
            RF_Transient | RF_Transactional);
        if (Emitter)
        {
            EnsureHandlerTestEmitterGraphSource(Emitter);
            System->AddEmitterHandle(*Emitter, FName(TEXT("HandlerTestEmitter")), Emitter->GetExposedVersion().VersionGuid);
        }

        const FNiagaraVariable UserFloat(FNiagaraTypeDefinition::GetFloatDef(), FName(TEXT("User.HandlerFixtureFloat")));
        System->GetExposedParameters().SetParameterValue(4.0f, UserFloat, true);
    }
}

// ============================================================================
// niagara.inspect
// REQ: assetPath
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNiagaraInspectTransientSystemShapeTest,
    "PinWright.niagara.inspect.TransientSystemShape",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraInspectTransientSystemShapeTest::RunTest(const FString& Parameters)
{
    FString ObjectPath;
    UNiagaraSystem* System = NewTransientHandlerTestSystem(ObjectPath);
    TestNotNull(TEXT("Transient Niagara system created"), System);
    if (!System)
    {
        return false;
    }
    PopulateRepresentativeHandlerSystem(System);

    FTestResponseCapture Capture;
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), ObjectPath);

    TestTrue(TEXT("niagara.inspect handler found"),
        InvokeHandlerWithCapture(TEXT("niagara.inspect"), Payload, Capture));
    TestTrue(TEXT("niagara.inspect sent a response"), Capture.bWasCalled);
    TestTrue(TEXT("niagara.inspect succeeded"), Capture.bSuccess);
    TestNotNull(TEXT("niagara.inspect result is present"), Capture.Result.Get());
    if (!Capture.Result.IsValid())
    {
        System->RemoveFromRoot();
        return false;
    }

    const TSharedPtr<FJsonObject>& Result = Capture.Result;
    TestEqual(TEXT("assetPath echoed"), Result->GetStringField(TEXT("assetPath")), ObjectPath);
    TestEqual(TEXT("assetKind is NiagaraSystem"), Result->GetStringField(TEXT("assetKind")), FString(TEXT("NiagaraSystem")));
    TestTrue(TEXT("system aspect object present"), HasObjectField(Result, TEXT("system")));
    TestTrue(TEXT("emitters aspect object present"), HasObjectField(Result, TEXT("emitters")));
    TestTrue(TEXT("parameters aspect object present"), HasObjectField(Result, TEXT("parameters")));
    TestTrue(TEXT("stack aspect object present"), HasObjectField(Result, TEXT("stack")));
    TestTrue(TEXT("graphs aspect object present"), HasObjectField(Result, TEXT("graphs")));
    TestTrue(TEXT("compile aspect object present"), HasObjectField(Result, TEXT("compile")));

    const TSharedPtr<FJsonObject>* GraphsObject = nullptr;
    if (Result->TryGetObjectField(TEXT("graphs"), GraphsObject) && GraphsObject)
    {
        TestTrue(TEXT("graphs aspect has graphs array"), HasArrayField(*GraphsObject, TEXT("graphs")));
        TestTrue(TEXT("graphs aspect includes node/pin data"), GraphArrayHasNodeWithPin(*GraphsObject));
    }
    else
    {
        TestTrue(TEXT("graphs aspect has graphs array"), false);
    }

    const TSharedPtr<FJsonObject>* SystemObject = nullptr;
    if (Result->TryGetObjectField(TEXT("system"), SystemObject) && SystemObject)
    {
        TestEqual(TEXT("inspect system has fixture emitter handle"), ArrayFieldNum(*SystemObject, TEXT("emitterHandles")), 1);
    }
    const TSharedPtr<FJsonObject>* ParametersObject = nullptr;
    if (Result->TryGetObjectField(TEXT("parameters"), ParametersObject) && ParametersObject)
    {
        TestTrue(TEXT("inspect parameters has fixture user parameter"), ArrayFieldNum(*ParametersObject, TEXT("user")) >= 1);
    }

    System->RemoveFromRoot();
    return true;
}

// Regression: niagara.inspect's parametersOnly projection + parameterName filter keep a
// single-parameter readback inline (board E-niagara-inspect-no-param-readback-projection).
// parametersOnly must drop every non-parameter aspect; parameterName must narrow the
// parameter set to matching names. Reverting either (unregistering the params or ignoring
// them) makes the dropped-aspect and filtered-out assertions fail.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNiagaraInspectParameterProjectionTest,
    "PinWright.niagara.inspect.ParameterProjection",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraInspectParameterProjectionTest::RunTest(const FString& Parameters)
{
    // The narrowing params must be discoverable so callers can reach for them.
    TestNotNull(TEXT("niagara.inspect exposes parametersOnly"),
        GetRegisteredParamSpec(TEXT("niagara.inspect"), TEXT("parametersOnly")));
    TestNotNull(TEXT("niagara.inspect exposes parameterName"),
        GetRegisteredParamSpec(TEXT("niagara.inspect"), TEXT("parameterName")));

    FString ObjectPath;
    UNiagaraSystem* System = NewTransientHandlerTestSystem(ObjectPath);
    TestNotNull(TEXT("Transient Niagara system created"), System);
    if (!System)
    {
        return false;
    }
    // Adds User.HandlerFixtureFloat and an emitter/graph so the full inspect has every aspect.
    PopulateRepresentativeHandlerSystem(System);
    // A second user parameter the filter must exclude when narrowing to the first.
    const FNiagaraVariable OtherFloat(FNiagaraTypeDefinition::GetFloatDef(), FName(TEXT("User.OtherFixtureValue")));
    System->GetExposedParameters().SetParameterValue(7.0f, OtherFloat, true);

    // Baseline: a default inspect emits the non-parameter aspects (so the parametersOnly
    // drop below is meaningful, not vacuous).
    {
        FTestResponseCapture Capture;
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("assetPath"), ObjectPath);
        TestTrue(TEXT("niagara.inspect handler found"),
            InvokeHandlerWithCapture(TEXT("niagara.inspect"), Payload, Capture));
        TestTrue(TEXT("baseline inspect succeeded"), Capture.bSuccess);
        if (Capture.Result.IsValid())
        {
            TestTrue(TEXT("baseline inspect includes system aspect"), HasObjectField(Capture.Result, TEXT("system")));
            TestTrue(TEXT("baseline inspect includes stack aspect"), HasObjectField(Capture.Result, TEXT("stack")));
            TestTrue(TEXT("baseline inspect includes parameters aspect"), HasObjectField(Capture.Result, TEXT("parameters")));
        }
    }

    // parametersOnly projection: only the parameters aspect survives.
    {
        FTestResponseCapture Capture;
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("assetPath"), ObjectPath);
        Payload->SetBoolField(TEXT("parametersOnly"), true);
        TestTrue(TEXT("niagara.inspect handler found"),
            InvokeHandlerWithCapture(TEXT("niagara.inspect"), Payload, Capture));
        TestTrue(TEXT("parametersOnly inspect succeeded"), Capture.bSuccess);
        if (Capture.Result.IsValid())
        {
            TestTrue(TEXT("parametersOnly keeps parameters aspect"), HasObjectField(Capture.Result, TEXT("parameters")));
            TestFalse(TEXT("parametersOnly drops system aspect"), HasObjectField(Capture.Result, TEXT("system")));
            TestFalse(TEXT("parametersOnly drops emitters aspect"), HasObjectField(Capture.Result, TEXT("emitters")));
            TestFalse(TEXT("parametersOnly drops stack aspect"), HasObjectField(Capture.Result, TEXT("stack")));
            TestFalse(TEXT("parametersOnly drops graphs aspect"), HasObjectField(Capture.Result, TEXT("graphs")));
            TestFalse(TEXT("parametersOnly drops compile aspect"), HasObjectField(Capture.Result, TEXT("compile")));

            const TSharedPtr<FJsonObject>* ParametersObject = nullptr;
            if (Capture.Result->TryGetObjectField(TEXT("parameters"), ParametersObject) && ParametersObject)
            {
                TestTrue(TEXT("unfiltered projection keeps both user params"),
                    JsonArrayHasObjectWithStringField(*ParametersObject, TEXT("user"), TEXT("name"), TEXT("User.HandlerFixtureFloat"))
                    && JsonArrayHasObjectWithStringField(*ParametersObject, TEXT("user"), TEXT("name"), TEXT("User.OtherFixtureValue")));
            }
        }
    }

    // parameterName filter: only the matching parameter survives.
    {
        FTestResponseCapture Capture;
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("assetPath"), ObjectPath);
        Payload->SetBoolField(TEXT("parametersOnly"), true);
        Payload->SetStringField(TEXT("parameterName"), TEXT("HandlerFixtureFloat"));
        TestTrue(TEXT("niagara.inspect handler found"),
            InvokeHandlerWithCapture(TEXT("niagara.inspect"), Payload, Capture));
        TestTrue(TEXT("filtered inspect succeeded"), Capture.bSuccess);
        if (Capture.Result.IsValid())
        {
            const TSharedPtr<FJsonObject>* ParametersObject = nullptr;
            if (Capture.Result->TryGetObjectField(TEXT("parameters"), ParametersObject) && ParametersObject)
            {
                TestTrue(TEXT("filter keeps the matching user param"),
                    JsonArrayHasObjectWithStringField(*ParametersObject, TEXT("user"), TEXT("name"), TEXT("User.HandlerFixtureFloat")));
                TestFalse(TEXT("filter drops the non-matching user param"),
                    JsonArrayHasObjectWithStringField(*ParametersObject, TEXT("user"), TEXT("name"), TEXT("User.OtherFixtureValue")));
                TestEqual(TEXT("filter narrows user array to exactly the match"),
                    ArrayFieldNum(*ParametersObject, TEXT("user")), 1);
            }
        }
    }

    System->RemoveFromRoot();
    return true;
}

// ============================================================================
// niagara.graph.get
// REQ: assetPath
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNiagaraGraphGetTransientSystemShapeTest,
    "PinWright.niagara.graph.get.TransientSystemShape",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraGraphGetTransientSystemShapeTest::RunTest(const FString& Parameters)
{
    FString ObjectPath;
    UNiagaraSystem* System = NewTransientHandlerTestSystem(ObjectPath);
    TestNotNull(TEXT("Transient Niagara system created"), System);
    if (!System)
    {
        return false;
    }
    PopulateRepresentativeHandlerSystem(System);

    FTestResponseCapture Capture;
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), ObjectPath);
    Payload->SetStringField(TEXT("scriptUsage"), TEXT("SystemSpawn"));

    TestTrue(TEXT("niagara.graph.get handler found"),
        InvokeHandlerWithCapture(TEXT("niagara.graph.get"), Payload, Capture));
    TestTrue(TEXT("niagara.graph.get sent a response"), Capture.bWasCalled);
    TestTrue(TEXT("niagara.graph.get succeeded"), Capture.bSuccess);
    TestNotNull(TEXT("niagara.graph.get result is present"), Capture.Result.Get());
    if (!Capture.Result.IsValid())
    {
        System->RemoveFromRoot();
        return false;
    }

    const TSharedPtr<FJsonObject>& Result = Capture.Result;
    TestEqual(TEXT("assetPath echoed"), Result->GetStringField(TEXT("assetPath")), ObjectPath);
    TestEqual(TEXT("assetKind is NiagaraSystem"), Result->GetStringField(TEXT("assetKind")), FString(TEXT("NiagaraSystem")));
    TestEqual(TEXT("scriptUsage echoed"), Result->GetStringField(TEXT("scriptUsage")), FString(TEXT("SystemSpawn")));
    TestTrue(TEXT("graphs array present"), HasArrayField(Result, TEXT("graphs")));
    TestTrue(TEXT("count field present"), Result->HasField(TEXT("count")));
    TestEqual(TEXT("filtered graph count"), static_cast<int32>(Result->GetNumberField(TEXT("count"))), 1);
    TestTrue(TEXT("filtered graph includes node/pin data"), GraphArrayHasNodeWithPin(Result));

    System->RemoveFromRoot();
    return true;
}

// Counterfactual: reverting the UNiagaraScript branches in live read handlers returns
// UNSUPPORTED_ASSET for inspect/graph.get and Unsupported with an issue for validate.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNiagaraStandaloneScriptLiveReadHandlersTest,
    "PinWright.niagara.StandaloneScript.LiveReadHandlers",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraStandaloneScriptLiveReadHandlersTest::RunTest(const FString& Parameters)
{
    FString ObjectPath;
    UNiagaraScript* Script = NewTransientHandlerTestScript(ObjectPath);
    TestNotNull(TEXT("Transient Niagara script created"), Script);
    if (!Script)
    {
        return false;
    }

    AttachHandlerTestScriptSource(Script);

    {
        FTestResponseCapture Capture;
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("assetPath"), ObjectPath);

        TestTrue(TEXT("niagara.inspect handler found"),
            InvokeHandlerWithCapture(TEXT("niagara.inspect"), Payload, Capture));
        TestTrue(TEXT("niagara.inspect sent a response"), Capture.bWasCalled);
        TestTrue(TEXT("niagara.inspect succeeded"), Capture.bSuccess);
        TestNotNull(TEXT("niagara.inspect result is present"), Capture.Result.Get());
        if (Capture.Result.IsValid())
        {
            TestEqual(TEXT("inspect assetKind is NiagaraScript"), Capture.Result->GetStringField(TEXT("assetKind")), FString(TEXT("NiagaraScript")));
            TestTrue(TEXT("inspect graphs aspect object present"), HasObjectField(Capture.Result, TEXT("graphs")));
            TestTrue(TEXT("inspect compile aspect object present"), HasObjectField(Capture.Result, TEXT("compile")));
        }
    }

    {
        FTestResponseCapture Capture;
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("assetPath"), ObjectPath);
        Payload->SetStringField(TEXT("scriptUsage"), TEXT("Module"));

        TestTrue(TEXT("niagara.graph.get handler found"),
            InvokeHandlerWithCapture(TEXT("niagara.graph.get"), Payload, Capture));
        TestTrue(TEXT("niagara.graph.get sent a response"), Capture.bWasCalled);
        TestTrue(TEXT("niagara.graph.get succeeded"), Capture.bSuccess);
        TestNotNull(TEXT("niagara.graph.get result is present"), Capture.Result.Get());
        if (Capture.Result.IsValid())
        {
            TestEqual(TEXT("graph.get assetKind is NiagaraScript"), Capture.Result->GetStringField(TEXT("assetKind")), FString(TEXT("NiagaraScript")));
            TestEqual(TEXT("graph.get filtered graph count"), static_cast<int32>(Capture.Result->GetNumberField(TEXT("count"))), 1);
            TestTrue(TEXT("graph.get includes node/pin data"), GraphArrayHasNodeWithPin(Capture.Result));
        }
    }

    {
        FTestResponseCapture Capture;
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("assetPath"), ObjectPath);

        TestTrue(TEXT("niagara.validate handler found"),
            InvokeHandlerWithCapture(TEXT("niagara.validate"), Payload, Capture));
        TestTrue(TEXT("niagara.validate sent a response"), Capture.bWasCalled);
        TestTrue(TEXT("niagara.validate succeeded"), Capture.bSuccess);
        TestNotNull(TEXT("niagara.validate result is present"), Capture.Result.Get());
        if (Capture.Result.IsValid())
        {
            TestEqual(TEXT("validate assetKind is NiagaraScript"), Capture.Result->GetStringField(TEXT("assetKind")), FString(TEXT("NiagaraScript")));
            TestTrue(TEXT("validate compile object present"), HasObjectField(Capture.Result, TEXT("compile")));
        }
    }

    Script->RemoveFromRoot();
    return true;
}

namespace
{
    // Shared negative-path assertion for the niagara.* handlers: invoke the method,
    // then assert the standard error contract (handler found / response sent / failed
    // / error-code equals / message non-empty). Pass ExpectedMessageSubstring to also
    // assert the error message names a specific token (e.g. the actual param type).
    bool InvokeNiagaraHandlerExpectError(
        FAutomationTestBase& Test,
        const TCHAR* Method,
        const TSharedPtr<FJsonObject>& Payload,
        const TCHAR* ExpectedErrorCode,
        const TCHAR* ExpectedMessageSubstring = nullptr)
    {
        FTestResponseCapture Capture;
        Test.TestTrue(FString::Printf(TEXT("%s handler found"), Method), InvokeHandlerWithCapture(Method, Payload, Capture));
        Test.TestTrue(FString::Printf(TEXT("%s sent a response"), Method), Capture.bWasCalled);
        Test.TestFalse(FString::Printf(TEXT("%s failed"), Method), Capture.bSuccess);
        Test.TestEqual(FString::Printf(TEXT("%s error code"), Method), Capture.ErrorCode, FString(ExpectedErrorCode));
        Test.TestTrue(FString::Printf(TEXT("%s error message present"), Method), !Capture.Message.IsEmpty());
        if (ExpectedMessageSubstring)
        {
            Test.TestTrue(
                FString::Printf(TEXT("%s error message contains '%s'"), Method, ExpectedMessageSubstring),
                Capture.Message.Contains(ExpectedMessageSubstring));
        }
        return Capture.bWasCalled && !Capture.bSuccess && Capture.ErrorCode == ExpectedErrorCode;
    }
}

// ============================================================================
// niagara.create_system
// REQ: name, savePath
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNiagaraCreateSystemValidParamsNoCrashTest,
    "PinWright.niagara.create_system.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraCreateSystemValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    const FString SystemName = FString::Printf(
        TEXT("NS_CreateSystem_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    const FString SavePath = TEXT("/Game/PinWrightTests");
    const FString PackagePath = FString::Printf(TEXT("%s/%s"), *SavePath, *SystemName);
    CleanupTestAsset(PackagePath);

    FTestResponseCapture Capture;
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("name"), SystemName);
    Payload->SetStringField(TEXT("savePath"), SavePath);

    TestTrue(TEXT("niagara.create_system handler found"),
        InvokeHandlerWithCapture(TEXT("niagara.create_system"), Payload, Capture));
    TestTrue(TEXT("niagara.create_system sent a response"), Capture.bWasCalled);
    TestTrue(TEXT("niagara.create_system succeeded"), Capture.bSuccess);
    TestNotNull(TEXT("niagara.create_system result is present"), Capture.Result.Get());
    if (!Capture.Result.IsValid())
    {
        CleanupTestAsset(PackagePath);
        return false;
    }

    FString SystemPath;
    TestTrue(TEXT("niagara.create_system returned systemPath"),
        Capture.Result->TryGetStringField(TEXT("systemPath"), SystemPath));
    TestFalse(TEXT("systemPath is not empty"), SystemPath.IsEmpty());

    UNiagaraSystem* CreatedSystem = LoadObject<UNiagaraSystem>(nullptr, *SystemPath);
    TestNotNull(TEXT("created Niagara system loads from systemPath"), CreatedSystem);
    if (CreatedSystem)
    {
        UNiagaraScript* SpawnScript = CreatedSystem->GetSystemSpawnScript();
        UNiagaraScript* UpdateScript = CreatedSystem->GetSystemUpdateScript();
        TestNotNull(TEXT("system spawn script exists"), SpawnScript);
        TestNotNull(TEXT("system update script exists"), UpdateScript);
        TestNotNull(TEXT("system spawn latest source exists"), SpawnScript ? SpawnScript->GetLatestSource() : nullptr);
        TestNotNull(TEXT("system update latest source exists"), UpdateScript ? UpdateScript->GetLatestSource() : nullptr);
        TestEqual(TEXT("created system starts empty"), CreatedSystem->GetEmitterHandles().Num(), 0);
    }

    CleanupTestAsset(PackagePath);
    return true;
}

// ============================================================================
// niagara.create_emitter
// REQ: name, savePath
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNiagaraCreateEmitterValidParamsNoCrashTest,
    "PinWright.niagara.create_emitter.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraCreateEmitterValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("name"), TEXT("MyEmitter"));
    Payload->SetStringField(TEXT("savePath"), TEXT("/Game/Niagara"));
    TestTrue(TEXT("Handler found and invoked"), InvokeHandler(TEXT("niagara.create_emitter"), Payload));
    CleanupTestAsset(TEXT("/Game/Niagara/MyEmitter"));
    return true;
}

// ============================================================================
// niagara.spawn_actor
// REQ: systemPath  OPT: location, name
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNiagaraSpawnActorValidParamsNoCrashTest,
    "PinWright.niagara.spawn_actor.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraSpawnActorValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("systemPath"), TEXT("/Game/Niagara/NS_Fire"));
    Payload->SetStringField(TEXT("name"), TEXT("FireActor"));
    TestTrue(TEXT("Handler found and invoked"), InvokeHandler(TEXT("niagara.spawn_actor"), Payload));
    return true;
}

// ============================================================================
// niagara.modify_parameter
// REQ: actorName, parameterName, value  OPT: parameterType, type
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNiagaraModifyParameterValidParamsNoCrashTest,
    "PinWright.niagara.modify_parameter.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraModifyParameterValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("actorName"), TEXT("FireActor"));
    Payload->SetStringField(TEXT("parameterName"), TEXT("SpawnRate"));
    Payload->SetNumberField(TEXT("value"), 100.0);
    Payload->SetStringField(TEXT("parameterType"), TEXT("Float"));
    TestTrue(TEXT("Handler found and invoked"), InvokeHandler(TEXT("niagara.modify_parameter"), Payload));
    return true;
}

// ============================================================================
// niagara.create_ribbon
// REQ: systemPath  OPT: name, start, end, width, color
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNiagaraCreateRibbonValidParamsNoCrashTest,
    "PinWright.niagara.create_ribbon.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraCreateRibbonValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("systemPath"), TEXT("/Game/Niagara/NS_Ribbon"));
    Payload->SetStringField(TEXT("name"), TEXT("NiagaraRibbon"));
    Payload->SetNumberField(TEXT("width"), 10.0);
    TestTrue(TEXT("Handler found and invoked"), InvokeHandler(TEXT("niagara.create_ribbon"), Payload));
    return true;
}

// ============================================================================
// niagara.graph.connect_pins
// REQ: assetPath, fromNode, fromPin, toNode, toPin  OPT: emitterName, scriptType
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNiagaraGraphConnectPinsAssetMissingStructuredErrorTest,
    "PinWright.niagara.graph.connect_pins.AssetMissingStructuredError",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraGraphConnectPinsAssetMissingStructuredErrorTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), TEXT("/Game/Niagara/NS_Fire"));
    Payload->SetStringField(TEXT("fromNode"), TEXT("SpawnRate"));
    Payload->SetStringField(TEXT("fromPin"), TEXT("OutRate"));
    Payload->SetStringField(TEXT("toNode"), TEXT("InitializeParticle"));
    Payload->SetStringField(TEXT("toPin"), TEXT("InRate"));
    InvokeNiagaraHandlerExpectError(*this, TEXT("niagara.graph.connect_pins"), Payload, TEXT("ASSET_NOT_FOUND"));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNiagaraGraphConnectPinsRejectsUnknownScriptTypeWithoutMutationTest,
    "PinWright.niagara.graph.connect_pins.RejectsUnknownScriptTypeWithoutMutation",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraGraphConnectPinsRejectsUnknownScriptTypeWithoutMutationTest::RunTest(const FString& Parameters)
{
    FString ObjectPath;
    UNiagaraSystem* System = NewTransientHandlerTestSystem(ObjectPath);
    TestNotNull(TEXT("Transient Niagara system created"), System);
    if (!System)
    {
        return false;
    }

    FSharedHandlerTestSystemGraph Graphs;
    TestTrue(TEXT("shared Spawn and Update graph created"),
        AttachSharedHandlerTestSystemGraph(System, Graphs));
    if (!Graphs.SpawnGraph || !Graphs.UpdateGraph
        || !Graphs.SpawnFromNode || !Graphs.SpawnToNode
        || !Graphs.UpdateFromNode || !Graphs.UpdateToNode
        || !Graphs.SpawnFromPin || !Graphs.SpawnToPin
        || !Graphs.UpdateFromPin || !Graphs.UpdateToPin)
    {
        System->RemoveFromRoot();
        return false;
    }

    const int32 SpawnNodeCount = Graphs.SpawnGraph->Nodes.Num();
    const int32 UpdateNodeCount = Graphs.UpdateGraph->Nodes.Num();
    const int32 SpawnFromLinkCount = Graphs.SpawnFromPin->LinkedTo.Num();
    const int32 SpawnToLinkCount = Graphs.SpawnToPin->LinkedTo.Num();
    const int32 UpdateFromLinkCount = Graphs.UpdateFromPin->LinkedTo.Num();
    const int32 UpdateToLinkCount = Graphs.UpdateToPin->LinkedTo.Num();

    bool bSpawnGraphChanged = false;
    bool bUpdateGraphChanged = false;
    const FDelegateHandle SpawnChangedHandle = Graphs.SpawnGraph->AddOnGraphChangedHandler(
        FOnGraphChanged::FDelegate::CreateLambda(
            [&bSpawnGraphChanged](const FEdGraphEditAction&) { bSpawnGraphChanged = true; }));
    const FDelegateHandle UpdateChangedHandle = Graphs.UpdateGraph->AddOnGraphChangedHandler(
        FOnGraphChanged::FDelegate::CreateLambda(
            [&bUpdateGraphChanged](const FEdGraphEditAction&) { bUpdateGraphChanged = true; }));

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), ObjectPath);
    Payload->SetStringField(TEXT("scriptType"), TEXT("Bogus"));
    Payload->SetStringField(TEXT("fromNode"), Graphs.SpawnFromNode->NodeGuid.ToString());
    Payload->SetStringField(TEXT("fromPin"), TEXT("OutFloat"));
    Payload->SetStringField(TEXT("toNode"), Graphs.SpawnToNode->NodeGuid.ToString());
    Payload->SetStringField(TEXT("toPin"), TEXT("InFloat"));

    FTestResponseCapture Capture;
    TestTrue(TEXT("niagara.graph.connect_pins handler found"),
        InvokeHandlerWithCapture(TEXT("niagara.graph.connect_pins"), Payload, Capture));

    Graphs.SpawnGraph->RemoveOnGraphChangedHandler(SpawnChangedHandle);
    Graphs.UpdateGraph->RemoveOnGraphChangedHandler(UpdateChangedHandle);

    TestTrue(TEXT("connect_pins sent a response"), Capture.bWasCalled);
    TestFalse(TEXT("connect_pins rejected the unknown script type"), Capture.bSuccess);
    TestEqual(TEXT("error code is TARGET_NOT_FOUND"),
        Capture.ErrorCode, FString(TEXT("TARGET_NOT_FOUND")));
    TestTrue(TEXT("error names the requested script type"), Capture.Message.Contains(TEXT("Bogus")));
    TestTrue(TEXT("error lists Spawn candidate"), Capture.Message.Contains(TEXT("Spawn")));
    TestTrue(TEXT("error lists Update candidate"), Capture.Message.Contains(TEXT("Update")));

    TestEqual(TEXT("Spawn node count is unchanged"), Graphs.SpawnGraph->Nodes.Num(), SpawnNodeCount);
    TestEqual(TEXT("Update node count is unchanged"), Graphs.UpdateGraph->Nodes.Num(), UpdateNodeCount);
    TestEqual(TEXT("Spawn source pin has no new links"), Graphs.SpawnFromPin->LinkedTo.Num(), SpawnFromLinkCount);
    TestEqual(TEXT("Spawn destination pin has no new links"), Graphs.SpawnToPin->LinkedTo.Num(), SpawnToLinkCount);
    TestEqual(TEXT("Update source pin has no new links"), Graphs.UpdateFromPin->LinkedTo.Num(), UpdateFromLinkCount);
    TestEqual(TEXT("Update destination pin has no new links"), Graphs.UpdateToPin->LinkedTo.Num(), UpdateToLinkCount);
    TestFalse(TEXT("invalid target does not notify Spawn graph"), bSpawnGraphChanged);
    TestFalse(TEXT("invalid target does not notify Update graph"), bUpdateGraphChanged);

    System->RemoveFromRoot();
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNiagaraGraphConnectPinsRejectsAmbiguousAliasWithoutMutationTest,
    "PinWright.niagara.graph.connect_pins.RejectsAmbiguousAliasWithoutMutation",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraGraphConnectPinsRejectsAmbiguousAliasWithoutMutationTest::RunTest(const FString& Parameters)
{
    FString ObjectPath;
    UNiagaraSystem* System = NewTransientHandlerTestSystem(ObjectPath);
    TestNotNull(TEXT("Transient Niagara system created"), System);
    if (!System)
    {
        return false;
    }

    FSharedHandlerTestSystemGraph Graphs;
    TestTrue(TEXT("shared Spawn and Update graph created"),
        AttachSharedHandlerTestSystemGraph(System, Graphs));
    if (!Graphs.SpawnGraph || !Graphs.UpdateGraph
        || !Graphs.SpawnFromNode || !Graphs.SpawnToNode
        || !Graphs.UpdateFromNode || !Graphs.UpdateToNode
        || !Graphs.SpawnFromPin || !Graphs.SpawnToPin
        || !Graphs.UpdateFromPin || !Graphs.UpdateToPin)
    {
        System->RemoveFromRoot();
        return false;
    }

    const FString SharedTitle = Graphs.SpawnFromNode->GetNodeTitle(ENodeTitleType::ListView).ToString();
    TestEqual(TEXT("mixed-scope fixture nodes share a display title"),
        SharedTitle, Graphs.UpdateFromNode->GetNodeTitle(ENodeTitleType::ListView).ToString());
    UEdGraphPin* UpdateFromOutputPin = Graphs.UpdateFromPin;
    TestNotNull(TEXT("outside-scope ambiguous candidate has an output pin"), UpdateFromOutputPin);
    if (!UpdateFromOutputPin)
    {
        System->RemoveFromRoot();
        return false;
    }
    const FString SpawnFromGuid = Graphs.SpawnFromNode->NodeGuid.ToString();
    const FString SpawnToGuid = Graphs.SpawnToNode->NodeGuid.ToString();
    const FString UpdateFromGuid = Graphs.UpdateFromNode->NodeGuid.ToString();
    const int32 SpawnFromLinkCount = Graphs.SpawnFromPin->LinkedTo.Num();
    const int32 SpawnToLinkCount = Graphs.SpawnToPin->LinkedTo.Num();
    const int32 UpdateFromOutputLinkCount = UpdateFromOutputPin->LinkedTo.Num();
    const int32 UpdateFromLinkCount = Graphs.UpdateFromPin->LinkedTo.Num();
    const int32 UpdateToLinkCount = Graphs.UpdateToPin->LinkedTo.Num();

    bool bSpawnGraphChanged = false;
    bool bUpdateGraphChanged = false;
    const FDelegateHandle SpawnChangedHandle = Graphs.SpawnGraph->AddOnGraphChangedHandler(
        FOnGraphChanged::FDelegate::CreateLambda(
            [&bSpawnGraphChanged](const FEdGraphEditAction&) { bSpawnGraphChanged = true; }));
    const FDelegateHandle UpdateChangedHandle = Graphs.UpdateGraph->AddOnGraphChangedHandler(
        FOnGraphChanged::FDelegate::CreateLambda(
            [&bUpdateGraphChanged](const FEdGraphEditAction&) { bUpdateGraphChanged = true; }));

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), ObjectPath);
    Payload->SetStringField(TEXT("scriptType"), TEXT("Spawn"));
    Payload->SetStringField(TEXT("fromNode"), SharedTitle);
    Payload->SetStringField(TEXT("fromPin"), TEXT("OutFloat"));
    Payload->SetStringField(TEXT("toNode"), SpawnToGuid);
    Payload->SetStringField(TEXT("toPin"), TEXT("InFloat"));

    FTestResponseCapture Capture;
    TestTrue(TEXT("niagara.graph.connect_pins handler found"),
        InvokeHandlerWithCapture(TEXT("niagara.graph.connect_pins"), Payload, Capture));

    Graphs.SpawnGraph->RemoveOnGraphChangedHandler(SpawnChangedHandle);
    Graphs.UpdateGraph->RemoveOnGraphChangedHandler(UpdateChangedHandle);

    TestTrue(TEXT("connect_pins sent a response"), Capture.bWasCalled);
    TestFalse(TEXT("connect_pins rejected the ambiguous alias"), Capture.bSuccess);
    TestEqual(TEXT("error code is AMBIGUOUS_NODE"),
        Capture.ErrorCode, FString(TEXT("AMBIGUOUS_NODE")));
    TestTrue(TEXT("error lists the first candidate GUID"), Capture.Message.Contains(SpawnFromGuid));
    TestTrue(TEXT("error lists the outside-scope candidate GUID"), Capture.Message.Contains(UpdateFromGuid));
    TestEqual(TEXT("Spawn source pin has no new links"), Graphs.SpawnFromPin->LinkedTo.Num(), SpawnFromLinkCount);
    TestEqual(TEXT("Spawn destination pin has no new links"), Graphs.SpawnToPin->LinkedTo.Num(), SpawnToLinkCount);
    TestFalse(TEXT("scope-filtering counterfactual link was not created"),
        Graphs.SpawnFromPin->LinkedTo.Contains(Graphs.SpawnToPin));
    TestEqual(TEXT("outside-scope candidate has no new links"), UpdateFromOutputPin->LinkedTo.Num(), UpdateFromOutputLinkCount);
    TestEqual(TEXT("Update source pin has no new links"), Graphs.UpdateFromPin->LinkedTo.Num(), UpdateFromLinkCount);
    TestEqual(TEXT("Update destination pin has no new links"), Graphs.UpdateToPin->LinkedTo.Num(), UpdateToLinkCount);
    TestFalse(TEXT("ambiguous alias does not notify Spawn graph"), bSpawnGraphChanged);
    TestFalse(TEXT("ambiguous alias does not notify Update graph"), bUpdateGraphChanged);

    System->RemoveFromRoot();
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNiagaraGraphConnectPinsDefaultsToSpawnAndReturnsCanonicalIdentityTest,
    "PinWright.niagara.graph.connect_pins.DefaultsToSpawnAndReturnsCanonicalIdentity",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraGraphConnectPinsDefaultsToSpawnAndReturnsCanonicalIdentityTest::RunTest(const FString& Parameters)
{
    FString ObjectPath;
    UNiagaraSystem* System = NewTransientHandlerTestSystem(ObjectPath);
    TestNotNull(TEXT("Transient Niagara system created"), System);
    if (!System)
    {
        return false;
    }

    FSharedHandlerTestSystemGraph Graphs;
    TestTrue(TEXT("shared Spawn and Update graph created"),
        AttachSharedHandlerTestSystemGraph(System, Graphs));
    if (!Graphs.SpawnGraph || !Graphs.UpdateGraph
        || !Graphs.SpawnFromNode || !Graphs.SpawnToNode
        || !Graphs.UpdateFromNode || !Graphs.UpdateToNode
        || !Graphs.SpawnFromPin || !Graphs.SpawnToPin
        || !Graphs.UpdateFromPin || !Graphs.UpdateToPin)
    {
        System->RemoveFromRoot();
        return false;
    }

    const FString SpawnFromGuid = Graphs.SpawnFromNode->NodeGuid.ToString();
    const FString SpawnToGuid = Graphs.SpawnToNode->NodeGuid.ToString();
    const FString SpawnFromAlias = Graphs.SpawnFromNode->GetName();
    const FString SpawnToAlias = Graphs.SpawnToNode->GetName();
    TestNotEqual(TEXT("successful fixture aliases are unique"), SpawnFromAlias, SpawnToAlias);
    bool bSpawnGraphChanged = false;
    bool bUpdateGraphChanged = false;
    const FDelegateHandle SpawnChangedHandle = Graphs.SpawnGraph->AddOnGraphChangedHandler(
        FOnGraphChanged::FDelegate::CreateLambda(
            [&bSpawnGraphChanged](const FEdGraphEditAction&) { bSpawnGraphChanged = true; }));
    const FDelegateHandle UpdateChangedHandle = Graphs.UpdateGraph->AddOnGraphChangedHandler(
        FOnGraphChanged::FDelegate::CreateLambda(
            [&bUpdateGraphChanged](const FEdGraphEditAction&) { bUpdateGraphChanged = true; }));

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), ObjectPath);
    Payload->SetStringField(TEXT("fromNode"), SpawnFromAlias);
    Payload->SetStringField(TEXT("fromPin"), TEXT("OutFloat"));
    Payload->SetStringField(TEXT("toNode"), SpawnToAlias);
    Payload->SetStringField(TEXT("toPin"), TEXT("InFloat"));

    FTestResponseCapture Capture;
    TestTrue(TEXT("niagara.graph.connect_pins handler found"),
        InvokeHandlerWithCapture(TEXT("niagara.graph.connect_pins"), Payload, Capture));

    Graphs.SpawnGraph->RemoveOnGraphChangedHandler(SpawnChangedHandle);
    Graphs.UpdateGraph->RemoveOnGraphChangedHandler(UpdateChangedHandle);

    TestTrue(TEXT("connect_pins sent a response"), Capture.bWasCalled);
    TestTrue(TEXT("omitted scriptType defaults to Spawn"), Capture.bSuccess);
    TestTrue(TEXT("Spawn pins are linked"), Graphs.SpawnFromPin->LinkedTo.Contains(Graphs.SpawnToPin));
    TestTrue(TEXT("Spawn graph was notified"), bSpawnGraphChanged);
    TestTrue(TEXT("shared graph listener was notified"), bUpdateGraphChanged);
    if (Capture.Result.IsValid())
    {
        TestEqual(TEXT("response reports canonical Spawn graph usage"),
            Capture.Result->GetStringField(TEXT("scriptUsage")), FString(TEXT("SystemSpawn")));
        TestEqual(TEXT("response reports canonical source node GUID"),
            Capture.Result->GetStringField(TEXT("fromNode")), SpawnFromGuid);
        TestEqual(TEXT("response reports canonical destination node GUID"),
            Capture.Result->GetStringField(TEXT("toNode")), SpawnToGuid);
        TestEqual(TEXT("response preserves source pin"),
            Capture.Result->GetStringField(TEXT("fromPin")), FString(TEXT("OutFloat")));
        TestEqual(TEXT("response preserves destination pin"),
            Capture.Result->GetStringField(TEXT("toPin")), FString(TEXT("InFloat")));
    }
    else
    {
        TestNotNull(TEXT("successful connect_pins result is present"), Capture.Result.Get());
    }

    System->RemoveFromRoot();
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNiagaraGraphConnectPinsTargetsMixedCaseUpdateTest,
    "PinWright.niagara.graph.connect_pins.TargetsMixedCaseUpdate",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraGraphConnectPinsTargetsMixedCaseUpdateTest::RunTest(const FString& Parameters)
{
    FString ObjectPath;
    UNiagaraSystem* System = NewTransientHandlerTestSystem(ObjectPath);
    TestNotNull(TEXT("Transient Niagara system created"), System);
    if (!System)
    {
        return false;
    }

    FSharedHandlerTestSystemGraph Graphs;
    TestTrue(TEXT("shared Spawn and Update graph created"),
        AttachSharedHandlerTestSystemGraph(System, Graphs));
    if (!Graphs.SpawnGraph || !Graphs.UpdateGraph
        || !Graphs.SpawnFromNode || !Graphs.SpawnToNode
        || !Graphs.UpdateFromNode || !Graphs.UpdateToNode
        || !Graphs.SpawnFromPin || !Graphs.SpawnToPin
        || !Graphs.UpdateFromPin || !Graphs.UpdateToPin)
    {
        System->RemoveFromRoot();
        return false;
    }

    const FString UpdateFromGuid = Graphs.UpdateFromNode->NodeGuid.ToString();
    const FString UpdateToGuid = Graphs.UpdateToNode->NodeGuid.ToString();
    bool bSpawnGraphChanged = false;
    bool bUpdateGraphChanged = false;
    const FDelegateHandle SpawnChangedHandle = Graphs.SpawnGraph->AddOnGraphChangedHandler(
        FOnGraphChanged::FDelegate::CreateLambda(
            [&bSpawnGraphChanged](const FEdGraphEditAction&) { bSpawnGraphChanged = true; }));
    const FDelegateHandle UpdateChangedHandle = Graphs.UpdateGraph->AddOnGraphChangedHandler(
        FOnGraphChanged::FDelegate::CreateLambda(
            [&bUpdateGraphChanged](const FEdGraphEditAction&) { bUpdateGraphChanged = true; }));

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), ObjectPath);
    Payload->SetStringField(TEXT("scriptType"), TEXT("uPdAtE"));
    Payload->SetStringField(TEXT("fromNode"), UpdateFromGuid);
    Payload->SetStringField(TEXT("fromPin"), TEXT("OutFloat"));
    Payload->SetStringField(TEXT("toNode"), UpdateToGuid);
    Payload->SetStringField(TEXT("toPin"), TEXT("InFloat"));

    FTestResponseCapture Capture;
    TestTrue(TEXT("niagara.graph.connect_pins handler found"),
        InvokeHandlerWithCapture(TEXT("niagara.graph.connect_pins"), Payload, Capture));

    Graphs.SpawnGraph->RemoveOnGraphChangedHandler(SpawnChangedHandle);
    Graphs.UpdateGraph->RemoveOnGraphChangedHandler(UpdateChangedHandle);

    TestTrue(TEXT("connect_pins sent a response"), Capture.bWasCalled);
    TestTrue(TEXT("mixed-case Update target succeeds"), Capture.bSuccess);
    TestTrue(TEXT("Update pins are linked"), Graphs.UpdateFromPin->LinkedTo.Contains(Graphs.UpdateToPin));
    TestFalse(TEXT("Spawn pins remain unlinked"), Graphs.SpawnFromPin->LinkedTo.Contains(Graphs.SpawnToPin));
    TestTrue(TEXT("shared graph listener was notified"), bSpawnGraphChanged);
    TestTrue(TEXT("Update graph was notified"), bUpdateGraphChanged);
    if (Capture.Result.IsValid())
    {
        TestEqual(TEXT("response reports canonical Update graph usage"),
            Capture.Result->GetStringField(TEXT("scriptUsage")), FString(TEXT("SystemUpdate")));
        TestEqual(TEXT("response reports canonical source node GUID"),
            Capture.Result->GetStringField(TEXT("fromNode")), UpdateFromGuid);
        TestEqual(TEXT("response reports canonical destination node GUID"),
            Capture.Result->GetStringField(TEXT("toNode")), UpdateToGuid);
    }
    else
    {
        TestNotNull(TEXT("successful connect_pins result is present"), Capture.Result.Get());
    }

    System->RemoveFromRoot();
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNiagaraGraphConnectPinsRejectsSpawnNodeForUpdateOnSharedGraphTest,
    "PinWright.niagara.graph.connect_pins.RejectsSpawnNodeForUpdateOnSharedGraph",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraGraphConnectPinsRejectsSpawnNodeForUpdateOnSharedGraphTest::RunTest(const FString& Parameters)
{
    FString ObjectPath;
    UNiagaraSystem* System = NewTransientHandlerTestSystem(ObjectPath);
    FSharedHandlerTestSystemGraph Graphs;
    TestTrue(TEXT("shared Spawn and Update graph created"),
        System && AttachSharedHandlerTestSystemGraph(System, Graphs));
    if (!System || !Graphs.SpawnGraph || !Graphs.SpawnFromPin || !Graphs.UpdateToPin)
    {
        if (System) System->RemoveFromRoot();
        return false;
    }

    const int32 NodeCount = Graphs.SpawnGraph->Nodes.Num();
    const int32 SpawnLinkCount = Graphs.SpawnFromPin->LinkedTo.Num();
    const int32 UpdateLinkCount = Graphs.UpdateToPin->LinkedTo.Num();
    bool bGraphChanged = false;
    const FDelegateHandle ChangedHandle = Graphs.SpawnGraph->AddOnGraphChangedHandler(
        FOnGraphChanged::FDelegate::CreateLambda(
            [&bGraphChanged](const FEdGraphEditAction&) { bGraphChanged = true; }));

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), ObjectPath);
    Payload->SetStringField(TEXT("scriptType"), TEXT("Update"));
    Payload->SetStringField(TEXT("fromNode"), Graphs.SpawnFromNode->NodeGuid.ToString());
    Payload->SetStringField(TEXT("fromPin"), TEXT("OutFloat"));
    Payload->SetStringField(TEXT("toNode"), Graphs.UpdateToNode->NodeGuid.ToString());
    Payload->SetStringField(TEXT("toPin"), TEXT("InFloat"));

    FTestResponseCapture Capture;
    TestTrue(TEXT("connect_pins handler found"),
        InvokeHandlerWithCapture(TEXT("niagara.graph.connect_pins"), Payload, Capture));
    Graphs.SpawnGraph->RemoveOnGraphChangedHandler(ChangedHandle);

    TestFalse(TEXT("Update rejects Spawn-only node"), Capture.bSuccess);
    TestEqual(TEXT("error code is TARGET_NOT_FOUND"), Capture.ErrorCode, FString(TEXT("TARGET_NOT_FOUND")));
    TestTrue(TEXT("error names requested SystemUpdate"), Capture.Message.Contains(TEXT("SystemUpdate")));
    TestTrue(TEXT("error names reachable SystemSpawn"), Capture.Message.Contains(TEXT("SystemSpawn")));
    TestEqual(TEXT("shared graph node count is unchanged"), Graphs.SpawnGraph->Nodes.Num(), NodeCount);
    TestEqual(TEXT("Spawn pin links are unchanged"), Graphs.SpawnFromPin->LinkedTo.Num(), SpawnLinkCount);
    TestEqual(TEXT("Update pin links are unchanged"), Graphs.UpdateToPin->LinkedTo.Num(), UpdateLinkCount);
    TestFalse(TEXT("refusal emits no graph notification"), bGraphChanged);

    System->RemoveFromRoot();
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNiagaraGraphConnectPinsAcceptsNodeSharedBySpawnAndUpdateTest,
    "PinWright.niagara.graph.connect_pins.AcceptsNodeSharedBySpawnAndUpdate",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraGraphConnectPinsAcceptsNodeSharedBySpawnAndUpdateTest::RunTest(const FString& Parameters)
{
    FString ObjectPath;
    UNiagaraSystem* System = NewTransientHandlerTestSystem(ObjectPath);
    FSharedHandlerTestSystemGraph Graphs;
    TestTrue(TEXT("shared Spawn and Update graph created"),
        System && AttachSharedHandlerTestSystemGraph(System, Graphs));
    if (!System || !Graphs.SharedFromPin || !Graphs.DisconnectedToPin)
    {
        if (System) System->RemoveFromRoot();
        return false;
    }

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), ObjectPath);
    Payload->SetStringField(TEXT("scriptType"), TEXT("Update"));
    Payload->SetStringField(TEXT("fromNode"), Graphs.SharedNode->NodeGuid.ToString());
    Payload->SetStringField(TEXT("fromPin"), TEXT("OutFloat"));
    Payload->SetStringField(TEXT("toNode"), Graphs.DisconnectedNode->NodeGuid.ToString());
    Payload->SetStringField(TEXT("toPin"), TEXT("InFloat"));

    FTestResponseCapture Capture;
    TestTrue(TEXT("connect_pins handler found"),
        InvokeHandlerWithCapture(TEXT("niagara.graph.connect_pins"), Payload, Capture));
    TestTrue(TEXT("Update accepts a node reachable from both outputs"), Capture.bSuccess);
    TestTrue(TEXT("shared node was connected"), Graphs.SharedFromPin->LinkedTo.Contains(Graphs.DisconnectedToPin));
    if (Capture.Result.IsValid())
    {
        TestEqual(TEXT("response reports selected SystemUpdate usage"),
            Capture.Result->GetStringField(TEXT("scriptUsage")), FString(TEXT("SystemUpdate")));
    }
    else
    {
        TestNotNull(TEXT("successful connect_pins result is present"), Capture.Result.Get());
    }

    System->RemoveFromRoot();
    return true;
}

// Regression for E-niagara-connect-pins-add-pin-opaque: a schema-blocked
// niagara.graph.connect_pins must surface the Niagara schema's OWN rejection reason
// (via CanCreateConnection().Message), name both pins, report the distinct
// CONNECTION_DISALLOWED code (parity with the singular niagara.connect_pin), and — when
// the target is a Map Set '+' add pin (DynamicAddPin) — add a supported-path hint. This
// replaces the old opaque "[CONNECTION_FAILED] Failed to connect pins (schema blocked
// connection)." that named no pin/type/cause. The fixture wires two pins on the SAME node,
// a deterministic schema DISALLOW (its very first check) that does not depend on the
// numeric type-conversion matrix, while the target pin's DynamicAddPin subcategory drives
// the handler's add-pin hint branch. Reverting the handler to the opaque passthrough drops
// the pin names, the schema reason, the add-pin hint, and the CONNECTION_DISALLOWED code.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNiagaraGraphConnectPinsDisallowedSurfacesReasonTest,
    "PinWright.niagara.graph.connect_pins.DisallowedSurfacesReason",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraGraphConnectPinsDisallowedSurfacesReasonTest::RunTest(const FString& Parameters)
{
    FString ObjectPath;
    UNiagaraSystem* System = NewTransientHandlerTestSystem(ObjectPath);
    TestNotNull(TEXT("Transient Niagara system created"), System);
    if (!System)
    {
        return false;
    }
    // A fresh UNiagaraSystem carries its system spawn script (the graphs-aspect tests above
    // rely on this); a missing script is a real fixture failure, not a skip.
    UNiagaraScript* SpawnScript = System->GetSystemSpawnScript();
    TestNotNull(TEXT("system spawn script exists"), SpawnScript);
    if (!SpawnScript)
    {
        System->RemoveFromRoot();
        return false;
    }

    // Build a SystemSpawn graph carrying one node with a float OUTPUT pin and a Map Set '+'
    // add pin (Misc / "DynamicAddPin") INPUT pin.
    UNiagaraScriptSource* Source = NewObject<UNiagaraScriptSource>(
        SpawnScript, TEXT("ConnectPinsTestSource"), RF_Transient | RF_Transactional);
    UNiagaraGraph* Graph = NewObject<UNiagaraGraph>(Source, TEXT("ConnectPinsTestGraph"), RF_Transient | RF_Transactional);
    Graph->Schema = UEdGraphSchema_Niagara::StaticClass();
    Source->NodeGraph = Graph;
    SpawnScript->SetLatestSource(Source);

    UEdGraphNode* Node = NewObject<UEdGraphNode>(Graph, UEdGraphNode::StaticClass(), TEXT("ConnectPinsTestNode"), RF_Transient | RF_Transactional);
    Node->NodeGuid = FGuid::NewGuid();
    Graph->AddNode(Node, false, false);

    UEdGraphPin* OutPin = Node->CreatePin(EGPD_Output,
        UEdGraphSchema_Niagara::TypeDefinitionToPinType(FNiagaraTypeDefinition::GetFloatDef()),
        FName(TEXT("OutFloat")));

    FEdGraphPinType AddPinType;
    AddPinType.PinCategory = UEdGraphSchema_Niagara::PinCategoryMisc;
    AddPinType.PinSubCategory = FName(TEXT("DynamicAddPin"));
    UEdGraphPin* AddPin = Node->CreatePin(EGPD_Input, AddPinType, FName(TEXT("Add")));

    TestNotNull(TEXT("output pin created"), OutPin);
    TestNotNull(TEXT("add pin created"), AddPin);
    if (!OutPin || !AddPin)
    {
        System->RemoveFromRoot();
        return false;
    }

    // Precondition: the schema genuinely refuses this wire, so the assertions below exercise
    // the real disallow path (not a vacuous success). Capture the schema's own reason text so
    // the "surfaces the reason" assertion is tied to real engine behavior, not a hardcode.
    const FPinConnectionResponse Response = Graph->GetSchema()->CanCreateConnection(OutPin, AddPin);
    TestTrue(TEXT("schema disallows the same-node wire"),
        Response.Response == CONNECT_RESPONSE_DISALLOW);
    const FString ExpectedReason = Response.Message.ToString();

    const FString NodeId = Node->NodeGuid.ToString();

    FTestResponseCapture Capture;
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), ObjectPath);
    Payload->SetStringField(TEXT("fromNode"), NodeId);
    Payload->SetStringField(TEXT("fromPin"), TEXT("OutFloat"));
    Payload->SetStringField(TEXT("toNode"), NodeId);
    Payload->SetStringField(TEXT("toPin"), TEXT("Add"));

    TestTrue(TEXT("niagara.graph.connect_pins handler found"),
        InvokeHandlerWithCapture(TEXT("niagara.graph.connect_pins"), Payload, Capture));
    TestTrue(TEXT("connect_pins sent a response"), Capture.bWasCalled);
    TestFalse(TEXT("connect_pins failed (schema blocked)"), Capture.bSuccess);

    // Distinct disallow code (parity with the singular niagara.connect_pin), not the old
    // opaque CONNECTION_FAILED.
    TestEqual(TEXT("error code is CONNECTION_DISALLOWED"),
        Capture.ErrorCode, FString(TEXT("CONNECTION_DISALLOWED")));

    // The message must no longer be the opaque generic sentinel.
    TestFalse(TEXT("message is not the old opaque generic string"),
        Capture.Message.Contains(TEXT("Failed to connect pins (schema blocked connection)")));

    // It must name both pins so the caller can tell WHAT was blocked.
    TestTrue(TEXT("message names the source pin"), Capture.Message.Contains(TEXT("OutFloat")));
    TestTrue(TEXT("message names the target pin"), Capture.Message.Contains(TEXT("Add")));

    // It must surface the schema's OWN rejection reason.
    if (!ExpectedReason.IsEmpty())
    {
        TestTrue(TEXT("message surfaces the schema's rejection reason"),
            Capture.Message.Contains(ExpectedReason));
    }

    // The target is a DynamicAddPin, so the message must name the add-pin case + supported path.
    TestTrue(TEXT("message names the Map Set add pin (DynamicAddPin) case"),
        Capture.Message.Contains(TEXT("DynamicAddPin")));

    System->RemoveFromRoot();
    return true;
}

// Regression for B-niagara-connect-pins-sends-no-graph-notification: a SUCCESSFUL
// niagara.graph.connect_pins must end by broadcasting the graph's OnGraphChanged.
// UEdGraphSchema_Niagara::TryCreateConnection reaches only UNiagaraNode::PinConnectionListChanged,
// whose NotifyGraphNeedsRecompile broadcasts OnGraphNeedsRecompile and returns early WITHOUT
// calling Super::NotifyGraphChanged; the base UEdGraphNode does not notify at all. So without the
// handler's own NotifyGraphChanged() nothing reaches OnGraphChanged and an open editor keeps
// UNiagaraStackFunctionInput::OverridePinCache and SGraphPanel's connection splines on the
// pre-edit wiring. The fixture wires a float output on one node to a float input on a SECOND node
// (a plain CONNECT_RESPONSE_MAKE - the schema refuses any same-node wire before it looks at
// types), listens on OnGraphChanged, and fails if nothing fires.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNiagaraGraphConnectPinsNotifiesGraphChangedTest,
    "PinWright.niagara.graph.connect_pins.NotifiesGraphChanged",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraGraphConnectPinsNotifiesGraphChangedTest::RunTest(const FString& Parameters)
{
    FString ObjectPath;
    UNiagaraSystem* System = NewTransientHandlerTestSystem(ObjectPath);
    TestNotNull(TEXT("Transient Niagara system created"), System);
    if (!System)
    {
        return false;
    }

    UNiagaraScript* SpawnScript = System->GetSystemSpawnScript();
    TestNotNull(TEXT("system spawn script exists"), SpawnScript);
    if (!SpawnScript)
    {
        System->RemoveFromRoot();
        return false;
    }

    UNiagaraScriptSource* Source = NewObject<UNiagaraScriptSource>(
        SpawnScript, TEXT("ConnectPinsNotifySource"), RF_Transient | RF_Transactional);
    UNiagaraGraph* Graph = NewObject<UNiagaraGraph>(Source, TEXT("ConnectPinsNotifyGraph"), RF_Transient | RF_Transactional);
    Graph->Schema = UEdGraphSchema_Niagara::StaticClass();
    Source->NodeGraph = Graph;
    SpawnScript->SetLatestSource(Source);

    const FEdGraphPinType FloatPinType =
        UEdGraphSchema_Niagara::TypeDefinitionToPinType(FNiagaraTypeDefinition::GetFloatDef());

    UEdGraphNode* SourceNode = NewObject<UEdGraphNode>(Graph, UEdGraphNode::StaticClass(), TEXT("ConnectPinsNotifySourceNode"), RF_Transient | RF_Transactional);
    SourceNode->NodeGuid = FGuid::NewGuid();
    Graph->AddNode(SourceNode, false, false);
    UEdGraphPin* OutPin = SourceNode->CreatePin(EGPD_Output, FloatPinType, FName(TEXT("OutFloat")));

    UEdGraphNode* TargetNode = NewObject<UEdGraphNode>(Graph, UEdGraphNode::StaticClass(), TEXT("ConnectPinsNotifyTargetNode"), RF_Transient | RF_Transactional);
    TargetNode->NodeGuid = FGuid::NewGuid();
    Graph->AddNode(TargetNode, false, false);
    UEdGraphPin* InPin = TargetNode->CreatePin(EGPD_Input, FloatPinType, FName(TEXT("InFloat")));

    TestNotNull(TEXT("output pin created"), OutPin);
    TestNotNull(TEXT("input pin created"), InPin);
    if (!OutPin || !InPin)
    {
        System->RemoveFromRoot();
        return false;
    }

    // Precondition: the schema genuinely permits this wire, so the notification assertion below
    // measures the missing notify rather than a rejected connection.
    const FPinConnectionResponse Response = Graph->GetSchema()->CanCreateConnection(OutPin, InPin);
    TestFalse(TEXT("schema permits the float output -> float input wire"),
        Response.Response == CONNECT_RESPONSE_DISALLOW);

    bool bGraphChangedFired = false;
    const FDelegateHandle ChangedHandle = Graph->AddOnGraphChangedHandler(
        FOnGraphChanged::FDelegate::CreateLambda(
            [&bGraphChangedFired](const FEdGraphEditAction&) { bGraphChangedFired = true; }));

    FTestResponseCapture Capture;
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), ObjectPath);
    Payload->SetStringField(TEXT("fromNode"), SourceNode->NodeGuid.ToString());
    Payload->SetStringField(TEXT("fromPin"), TEXT("OutFloat"));
    Payload->SetStringField(TEXT("toNode"), TargetNode->NodeGuid.ToString());
    Payload->SetStringField(TEXT("toPin"), TEXT("InFloat"));

    TestTrue(TEXT("niagara.graph.connect_pins handler found"),
        InvokeHandlerWithCapture(TEXT("niagara.graph.connect_pins"), Payload, Capture));

    Graph->RemoveOnGraphChangedHandler(ChangedHandle);

    TestTrue(TEXT("connect_pins sent a response"), Capture.bWasCalled);
    TestTrue(TEXT("connect_pins succeeded"), Capture.bSuccess);

    // The wire really landed, so the notification assertion is about a real mutation.
    TestTrue(TEXT("pins are linked after connect_pins"), OutPin->LinkedTo.Contains(InPin));

    // The regression itself.
    TestTrue(TEXT("connect_pins broadcast OnGraphChanged"), bGraphChangedFired);

    System->RemoveFromRoot();
    return true;
}

// ============================================================================
// niagara.graph.remove_node
// REQ: assetPath, nodeId  OPT: emitterName, scriptType
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNiagaraGraphRemoveNodeAssetMissingStructuredErrorTest,
    "PinWright.niagara.graph.remove_node.AssetMissingStructuredError",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraGraphRemoveNodeAssetMissingStructuredErrorTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), TEXT("/Game/Niagara/NS_Fire"));
    Payload->SetStringField(TEXT("nodeId"), TEXT("AAAAAAAA-BBBB-CCCC-DDDD-EEEEEEEEEEEE"));
    InvokeNiagaraHandlerExpectError(*this, TEXT("niagara.graph.remove_node"), Payload, TEXT("ASSET_NOT_FOUND"));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNiagaraGraphRemoveNodeRejectsUnknownScriptTypeWithoutMutationTest,
    "PinWright.niagara.graph.remove_node.RejectsUnknownScriptTypeWithoutMutation",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraGraphRemoveNodeRejectsUnknownScriptTypeWithoutMutationTest::RunTest(const FString& Parameters)
{
    FString ObjectPath;
    UNiagaraSystem* System = NewTransientHandlerTestSystem(ObjectPath);
    TestNotNull(TEXT("Transient Niagara system created"), System);
    if (!System)
    {
        return false;
    }

    FSharedHandlerTestSystemGraph Graphs;
    TestTrue(TEXT("shared Spawn and Update graph created"),
        AttachSharedHandlerTestSystemGraph(System, Graphs));
    if (!Graphs.SpawnGraph || !Graphs.UpdateGraph
        || !Graphs.SpawnFromNode || !Graphs.SpawnToNode
        || !Graphs.UpdateFromNode || !Graphs.UpdateToNode)
    {
        System->RemoveFromRoot();
        return false;
    }

    const int32 SpawnNodeCount = Graphs.SpawnGraph->Nodes.Num();
    const int32 UpdateNodeCount = Graphs.UpdateGraph->Nodes.Num();
    bool bSpawnGraphChanged = false;
    bool bUpdateGraphChanged = false;
    const FDelegateHandle SpawnChangedHandle = Graphs.SpawnGraph->AddOnGraphChangedHandler(
        FOnGraphChanged::FDelegate::CreateLambda(
            [&bSpawnGraphChanged](const FEdGraphEditAction&) { bSpawnGraphChanged = true; }));
    const FDelegateHandle UpdateChangedHandle = Graphs.UpdateGraph->AddOnGraphChangedHandler(
        FOnGraphChanged::FDelegate::CreateLambda(
            [&bUpdateGraphChanged](const FEdGraphEditAction&) { bUpdateGraphChanged = true; }));

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), ObjectPath);
    Payload->SetStringField(TEXT("scriptType"), TEXT("Bogus"));
    Payload->SetStringField(TEXT("nodeId"), Graphs.SpawnFromNode->NodeGuid.ToString());

    FTestResponseCapture Capture;
    TestTrue(TEXT("niagara.graph.remove_node handler found"),
        InvokeHandlerWithCapture(TEXT("niagara.graph.remove_node"), Payload, Capture));

    Graphs.SpawnGraph->RemoveOnGraphChangedHandler(SpawnChangedHandle);
    Graphs.UpdateGraph->RemoveOnGraphChangedHandler(UpdateChangedHandle);

    TestTrue(TEXT("remove_node sent a response"), Capture.bWasCalled);
    TestFalse(TEXT("remove_node rejected the unknown script type"), Capture.bSuccess);
    TestEqual(TEXT("error code is TARGET_NOT_FOUND"),
        Capture.ErrorCode, FString(TEXT("TARGET_NOT_FOUND")));
    TestTrue(TEXT("error names the requested script type"), Capture.Message.Contains(TEXT("Bogus")));
    TestTrue(TEXT("error lists Spawn candidate"), Capture.Message.Contains(TEXT("Spawn")));
    TestTrue(TEXT("error lists Update candidate"), Capture.Message.Contains(TEXT("Update")));

    TestEqual(TEXT("Spawn node count is unchanged"), Graphs.SpawnGraph->Nodes.Num(), SpawnNodeCount);
    TestEqual(TEXT("Update node count is unchanged"), Graphs.UpdateGraph->Nodes.Num(), UpdateNodeCount);
    TestTrue(TEXT("addressed Spawn node still exists"), Graphs.SpawnGraph->Nodes.Contains(Graphs.SpawnFromNode));
    TestTrue(TEXT("Update graph node still exists"), Graphs.UpdateGraph->Nodes.Contains(Graphs.UpdateFromNode));
    TestFalse(TEXT("invalid target does not notify Spawn graph"), bSpawnGraphChanged);
    TestFalse(TEXT("invalid target does not notify Update graph"), bUpdateGraphChanged);

    System->RemoveFromRoot();
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNiagaraGraphRemoveNodeRejectsSpawnNodeForUpdateOnSharedGraphTest,
    "PinWright.niagara.graph.remove_node.RejectsSpawnNodeForUpdateOnSharedGraph",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraGraphRemoveNodeRejectsSpawnNodeForUpdateOnSharedGraphTest::RunTest(const FString& Parameters)
{
    FString ObjectPath;
    UNiagaraSystem* System = NewTransientHandlerTestSystem(ObjectPath);
    FSharedHandlerTestSystemGraph Graphs;
    TestTrue(TEXT("shared Spawn and Update graph created"),
        System && AttachSharedHandlerTestSystemGraph(System, Graphs));
    if (!System || !Graphs.SpawnGraph || !Graphs.SpawnFromNode)
    {
        if (System) System->RemoveFromRoot();
        return false;
    }

    const int32 NodeCount = Graphs.SpawnGraph->Nodes.Num();
    UEdGraphPin* SpawnScopeOut = Graphs.SpawnFromNode->FindPin(FName(TEXT("ScopeOut")));
    const int32 ScopeLinkCount = SpawnScopeOut ? SpawnScopeOut->LinkedTo.Num() : INDEX_NONE;
    bool bGraphChanged = false;
    const FDelegateHandle ChangedHandle = Graphs.SpawnGraph->AddOnGraphChangedHandler(
        FOnGraphChanged::FDelegate::CreateLambda(
            [&bGraphChanged](const FEdGraphEditAction&) { bGraphChanged = true; }));

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), ObjectPath);
    Payload->SetStringField(TEXT("scriptType"), TEXT("Update"));
    Payload->SetStringField(TEXT("nodeId"), Graphs.SpawnFromNode->NodeGuid.ToString());

    FTestResponseCapture Capture;
    TestTrue(TEXT("remove_node handler found"),
        InvokeHandlerWithCapture(TEXT("niagara.graph.remove_node"), Payload, Capture));
    Graphs.SpawnGraph->RemoveOnGraphChangedHandler(ChangedHandle);

    TestFalse(TEXT("Update rejects removal of Spawn-only node"), Capture.bSuccess);
    TestEqual(TEXT("error code is TARGET_NOT_FOUND"), Capture.ErrorCode, FString(TEXT("TARGET_NOT_FOUND")));
    TestTrue(TEXT("error names requested SystemUpdate"), Capture.Message.Contains(TEXT("SystemUpdate")));
    TestTrue(TEXT("error names reachable SystemSpawn"), Capture.Message.Contains(TEXT("SystemSpawn")));
    TestEqual(TEXT("shared graph node count is unchanged"), Graphs.SpawnGraph->Nodes.Num(), NodeCount);
    TestTrue(TEXT("Spawn-only node remains in shared graph"), Graphs.SpawnGraph->Nodes.Contains(Graphs.SpawnFromNode));
    TestEqual(TEXT("Spawn-only node links are unchanged"),
        SpawnScopeOut ? SpawnScopeOut->LinkedTo.Num() : INDEX_NONE, ScopeLinkCount);
    TestFalse(TEXT("refusal emits no graph notification"), bGraphChanged);

    System->RemoveFromRoot();
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNiagaraGraphRemoveNodeReturnsCanonicalIdentityTest,
    "PinWright.niagara.graph.remove_node.ReturnsCanonicalIdentity",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraGraphRemoveNodeReturnsCanonicalIdentityTest::RunTest(const FString& Parameters)
{
    FString ObjectPath;
    UNiagaraSystem* System = NewTransientHandlerTestSystem(ObjectPath);
    TestNotNull(TEXT("Transient Niagara system created"), System);
    if (!System)
    {
        return false;
    }

    FSharedHandlerTestSystemGraph Graphs;
    TestTrue(TEXT("shared Spawn and Update graph created"),
        AttachSharedHandlerTestSystemGraph(System, Graphs));
    if (!Graphs.SpawnGraph || !Graphs.UpdateGraph
        || !Graphs.SpawnFromNode || !Graphs.SpawnToNode
        || !Graphs.UpdateFromNode || !Graphs.UpdateToNode)
    {
        System->RemoveFromRoot();
        return false;
    }

    const FString UpdateNodeGuid = Graphs.UpdateFromNode->NodeGuid.ToString();
    const int32 SpawnNodeCount = Graphs.SpawnGraph->Nodes.Num();
    const int32 UpdateNodeCount = Graphs.UpdateGraph->Nodes.Num();

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), ObjectPath);
    Payload->SetStringField(TEXT("scriptType"), TEXT("uPdAtE"));
    Payload->SetStringField(TEXT("nodeId"), UpdateNodeGuid);

    FTestResponseCapture Capture;
    TestTrue(TEXT("niagara.graph.remove_node handler found"),
        InvokeHandlerWithCapture(TEXT("niagara.graph.remove_node"), Payload, Capture));

    TestTrue(TEXT("remove_node sent a response"), Capture.bWasCalled);
    TestTrue(TEXT("remove_node succeeds for mixed-case Update"), Capture.bSuccess);
    TestEqual(TEXT("shared graph loses one node"), Graphs.SpawnGraph->Nodes.Num(), SpawnNodeCount - 1);
    TestEqual(TEXT("Update graph loses one node"), Graphs.UpdateGraph->Nodes.Num(), UpdateNodeCount - 1);
    TestFalse(TEXT("addressed Update node was removed"), Graphs.UpdateGraph->Nodes.Contains(Graphs.UpdateFromNode));
    TestTrue(TEXT("Spawn graph still contains its nodes"), Graphs.SpawnGraph->Nodes.Contains(Graphs.SpawnFromNode));
    if (Capture.Result.IsValid())
    {
        TestEqual(TEXT("response reports canonical Update graph usage"),
            Capture.Result->GetStringField(TEXT("scriptUsage")), FString(TEXT("SystemUpdate")));
        TestEqual(TEXT("response reports canonical node GUID"),
            Capture.Result->GetStringField(TEXT("nodeId")), UpdateNodeGuid);
    }
    else
    {
        TestNotNull(TEXT("successful remove_node result is present"), Capture.Result.Get());
    }

    System->RemoveFromRoot();
    return true;
}

// ============================================================================
// Removed niagara.authoring.* handlers
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNiagaraAuthoringRemovedHandlersAbsentTest,
    "PinWright.niagara.authoring.RemovedHandlersAbsent",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraAuthoringRemovedHandlersAbsentTest::RunTest(const FString& Parameters)
{
    const TCHAR* RemovedHandlers[] = {
        TEXT("niagara.authoring.create_niagara_system"),
        TEXT("niagara.authoring.create_niagara_emitter"),
        TEXT("niagara.authoring.add_emitter_to_system"),
        TEXT("niagara.authoring.set_emitter_properties"),
        TEXT("niagara.authoring.add_spawn_rate_module"),
        TEXT("niagara.authoring.add_spawn_burst_module"),
        TEXT("niagara.authoring.add_spawn_per_unit_module"),
        TEXT("niagara.authoring.add_initialize_particle_module"),
        TEXT("niagara.authoring.add_particle_state_module"),
        TEXT("niagara.authoring.add_force_module"),
        TEXT("niagara.authoring.add_velocity_module"),
        TEXT("niagara.authoring.add_acceleration_module"),
        TEXT("niagara.authoring.add_size_module"),
        TEXT("niagara.authoring.add_color_module"),
        TEXT("niagara.authoring.add_sprite_renderer_module"),
        TEXT("niagara.authoring.add_mesh_renderer_module"),
        TEXT("niagara.authoring.add_ribbon_renderer_module"),
        TEXT("niagara.authoring.add_light_renderer_module"),
        TEXT("niagara.authoring.add_collision_module"),
        TEXT("niagara.authoring.add_kill_particles_module"),
        TEXT("niagara.authoring.add_camera_offset_module"),
        TEXT("niagara.authoring.add_user_parameter"),
        TEXT("niagara.authoring.set_parameter_value"),
        TEXT("niagara.authoring.bind_parameter_to_source"),
        TEXT("niagara.authoring.add_skeletal_mesh_data_interface"),
        TEXT("niagara.authoring.add_static_mesh_data_interface"),
        TEXT("niagara.authoring.add_spline_data_interface"),
        TEXT("niagara.authoring.add_audio_spectrum_data_interface"),
        TEXT("niagara.authoring.add_collision_query_data_interface"),
        TEXT("niagara.authoring.add_event_generator"),
        TEXT("niagara.authoring.add_event_receiver"),
        TEXT("niagara.authoring.configure_event_payload"),
        TEXT("niagara.authoring.enable_gpu_simulation"),
        TEXT("niagara.authoring.add_simulation_stage"),
        TEXT("niagara.authoring.get_niagara_info"),
        TEXT("niagara.authoring.validate_niagara_system")
    };

    for (const TCHAR* HandlerName : RemovedHandlers)
    {
        TestFalse(FString::Printf(TEXT("%s is not registered"), HandlerName), IsRegistered(HandlerName));
    }

    return true;
}
