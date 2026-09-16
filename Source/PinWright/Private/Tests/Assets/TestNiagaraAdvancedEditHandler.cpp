// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Misc/AutomationTest.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/Niagara/NiagaraEditTypes.h"
#include "NiagaraEditTestUtils.h"
#include "Tests/TestUtils.h"

#include "NiagaraDataInterface.h"
#include "NiagaraDataInterfaceCurve.h"
#include "NiagaraEmitter.h"
#include "NiagaraEmitterHandle.h"
#include "NiagaraGraph.h"
#include "NiagaraNodeOutput.h"
#include "NiagaraParameterStore.h"
#include "NiagaraScript.h"
#include "NiagaraScriptSource.h"
#include "NiagaraSimulationStageBase.h"
#include "NiagaraSystem.h"
#include "NiagaraTypes.h"

namespace
{
    UNiagaraNodeOutput* FindOutputNodeForUsage(UNiagaraGraph* Graph, ENiagaraScriptUsage Usage, FGuid UsageId)
    {
        if (!Graph)
        {
            return nullptr;
        }
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            if (UNiagaraNodeOutput* Out = Cast<UNiagaraNodeOutput>(Node))
            {
                if (Out->GetUsage() == Usage && Out->GetUsageId() == UsageId)
                {
                    return Out;
                }
            }
        }
        return nullptr;
    }

    UNiagaraGraph* GetEmitterParticleGraph(UNiagaraEmitter* Emitter)
    {
        if (!Emitter)
        {
            return nullptr;
        }
        FVersionedNiagaraEmitterData* EmitterData = Emitter->GetLatestEmitterData();
        UNiagaraScript* SpawnScript = EmitterData ? EmitterData->SpawnScriptProps.Script : nullptr;
        UNiagaraScriptSource* Source = SpawnScript ? Cast<UNiagaraScriptSource>(SpawnScript->GetLatestSource()) : nullptr;
        return Source ? Source->NodeGraph : nullptr;
    }
}

// Shared transient-system fixture + RAII unroot now live in NiagaraEditTestUtils.h so both this
// advanced-edit suite and the property-edit suite reuse one copy.
using NiagaraEditTestUtils::MakeAuthorableSystem;
using NiagaraEditTestUtils::FAuthorableSystemRoots;

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNiagaraAddEventHandlerAddsAndRemovesTest,
    "PinWright.Assets.Niagara.AdvancedEdit.EventHandler.AddRemoveRoundTrip",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraAddEventHandlerAddsAndRemovesTest::RunTest(const FString& Parameters)
{
    FString SystemPath;
    UNiagaraSystem* System = nullptr;
    UNiagaraEmitter* SourceEmitter = nullptr;
    FName EmitterName;
    const bool bSetupOk = MakeAuthorableSystem(SystemPath, System, SourceEmitter, EmitterName);
    FAuthorableSystemRoots Roots{System, SourceEmitter};
    if (!TestTrue(TEXT("authorable system created"), bSetupOk))
    {
        return false;
    }

    TSharedPtr<FJsonObject> AddPayload = MakeShared<FJsonObject>();
    AddPayload->SetStringField(TEXT("assetPath"), SystemPath);
    AddPayload->SetStringField(TEXT("emitter"), EmitterName.ToString());
    AddPayload->SetStringField(TEXT("executionMode"), TEXT("SpawnedParticles"));
    AddPayload->SetNumberField(TEXT("spawnNumber"), 7);
    AddPayload->SetStringField(TEXT("sourceEventName"), TEXT("Collision"));

    FTestResponseCapture AddCapture;
    if (!NiagaraEditTestUtils::InvokeExpectSuccess(*this, TEXT("niagara.add_event_handler"), AddPayload, AddCapture))
    {
        return false;
    }

    FNiagaraEmitterHandle* AddedHandle = nullptr;
    for (const FNiagaraEmitterHandle& H : System->GetEmitterHandles())
    {
        if (H.GetName() == EmitterName)
        {
            AddedHandle = const_cast<FNiagaraEmitterHandle*>(&H);
            break;
        }
    }
    TestNotNull(TEXT("emitter handle is present after add"), AddedHandle);
    if (!AddedHandle)
    {
        return false;
    }
    FVersionedNiagaraEmitterData* EmitterData = AddedHandle->GetEmitterData();
    UNiagaraEmitter* Emitter = AddedHandle->GetInstance().Emitter;
    TestNotNull(TEXT("emitter data resolved"), EmitterData);
    TestNotNull(TEXT("emitter resolved"), Emitter);
    if (!EmitterData || !Emitter)
    {
        return false;
    }

    TestEqual(TEXT("event handler added"), EmitterData->EventHandlerScriptProps.Num(), 1);
    if (EmitterData->EventHandlerScriptProps.Num() != 1)
    {
        return false;
    }
    const FNiagaraEventScriptProperties& Props = EmitterData->EventHandlerScriptProps[0];
    TestNotNull(TEXT("event handler script created"), Props.Script.Get());
    if (!Props.Script)
    {
        return false;
    }
    TestEqual(TEXT("event handler usage is ParticleEventScript"),
        static_cast<int32>(Props.Script->GetUsage()), static_cast<int32>(ENiagaraScriptUsage::ParticleEventScript));
    TestEqual(TEXT("execution mode applied"), static_cast<int32>(Props.ExecutionMode), static_cast<int32>(EScriptExecutionMode::SpawnedParticles));
    TestEqual(TEXT("spawn number applied"), static_cast<int32>(Props.SpawnNumber), 7);

    // Counterfactual anchor: bResetGraphForOutput=true must have produced an output node in
    // the emitter's particle graph for the new event script's UsageId.
    UNiagaraGraph* ParticleGraph = GetEmitterParticleGraph(Emitter);
    UNiagaraNodeOutput* OutputNode = FindOutputNodeForUsage(ParticleGraph, ENiagaraScriptUsage::ParticleEventScript, Props.Script->GetUsageId());
    TestNotNull(TEXT("particle event output node created (proves bResetGraphForOutput=true)"), OutputNode);

    const FString ReturnedId = AddCapture.Result.IsValid() ? AddCapture.Result->GetStringField(TEXT("eventHandlerId")) : FString();
    TestEqual(TEXT("returned eventHandlerId matches script usage id"), ReturnedId, Props.Script->GetUsageId().ToString());

    TSharedPtr<FJsonObject> RemovePayload = MakeShared<FJsonObject>();
    RemovePayload->SetStringField(TEXT("assetPath"), SystemPath);
    RemovePayload->SetStringField(TEXT("emitter"), EmitterName.ToString());
    RemovePayload->SetStringField(TEXT("eventHandlerId"), Props.Script->GetUsageId().ToString());

    FTestResponseCapture RemoveCapture;
    if (!NiagaraEditTestUtils::InvokeExpectSuccess(*this, TEXT("niagara.remove_event_handler"), RemovePayload, RemoveCapture))
    {
        return false;
    }

    EmitterData = AddedHandle->GetEmitterData();
    TestEqual(TEXT("event handler removed"), EmitterData ? EmitterData->EventHandlerScriptProps.Num() : -1, 0);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNiagaraAddSimulationStageRoundTripTest,
    "PinWright.Assets.Niagara.AdvancedEdit.SimulationStage.AddRemoveRoundTrip",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraAddSimulationStageRoundTripTest::RunTest(const FString& Parameters)
{
    FString SystemPath;
    UNiagaraSystem* System = nullptr;
    UNiagaraEmitter* SourceEmitter = nullptr;
    FName EmitterName;
    const bool bSetupOk = MakeAuthorableSystem(SystemPath, System, SourceEmitter, EmitterName);
    FAuthorableSystemRoots Roots{System, SourceEmitter};
    if (!TestTrue(TEXT("authorable system created"), bSetupOk))
    {
        return false;
    }

    TSharedPtr<FJsonObject> AddPayload = MakeShared<FJsonObject>();
    AddPayload->SetStringField(TEXT("assetPath"), SystemPath);
    AddPayload->SetStringField(TEXT("emitter"), EmitterName.ToString());

    FTestResponseCapture AddCapture;
    if (!NiagaraEditTestUtils::InvokeExpectSuccess(*this, TEXT("niagara.add_simulation_stage"), AddPayload, AddCapture))
    {
        return false;
    }

    FNiagaraEmitterHandle* Handle = nullptr;
    for (const FNiagaraEmitterHandle& H : System->GetEmitterHandles())
    {
        if (H.GetName() == EmitterName)
        {
            Handle = const_cast<FNiagaraEmitterHandle*>(&H);
            break;
        }
    }
    TestNotNull(TEXT("emitter handle present"), Handle);
    if (!Handle)
    {
        return false;
    }
    FVersionedNiagaraEmitterData* EmitterData = Handle->GetEmitterData();
    UNiagaraEmitter* Emitter = Handle->GetInstance().Emitter;
    TestNotNull(TEXT("emitter data resolved"), EmitterData);
    if (!EmitterData)
    {
        return false;
    }

    const TArray<UNiagaraSimulationStageBase*>& Stages = EmitterData->GetSimulationStages();
    TestEqual(TEXT("simulation stage added"), Stages.Num(), 1);
    if (Stages.Num() != 1)
    {
        return false;
    }
    UNiagaraSimulationStageBase* Stage = Stages[0];
    TestNotNull(TEXT("stage non-null"), Stage);
    if (!Stage)
    {
        return false;
    }
    TestTrue(TEXT("stage class is UNiagaraSimulationStageGeneric"), Stage->IsA(UNiagaraSimulationStageGeneric::StaticClass()));
    TestNotNull(TEXT("stage script created"), Stage->Script.Get());
    if (!Stage->Script)
    {
        return false;
    }
    TestEqual(TEXT("stage script usage is ParticleSimulationStageScript"),
        static_cast<int32>(Stage->Script->GetUsage()),
        static_cast<int32>(ENiagaraScriptUsage::ParticleSimulationStageScript));

    // Counterfactual anchor: vendored ResetGraphForOutput must have added an output node
    // for the new stage's UsageId in the emitter's particle graph.
    UNiagaraGraph* ParticleGraph = GetEmitterParticleGraph(Emitter);
    UNiagaraNodeOutput* OutputNode = FindOutputNodeForUsage(ParticleGraph, ENiagaraScriptUsage::ParticleSimulationStageScript, Stage->Script->GetUsageId());
    TestNotNull(TEXT("particle sim-stage output node created (proves vendored ResetGraphForOutput ran)"), OutputNode);

    TSharedPtr<FJsonObject> RemovePayload = MakeShared<FJsonObject>();
    RemovePayload->SetStringField(TEXT("assetPath"), SystemPath);
    RemovePayload->SetStringField(TEXT("emitter"), EmitterName.ToString());
    RemovePayload->SetStringField(TEXT("stageId"), Stage->Script->GetUsageId().ToString());

    FTestResponseCapture RemoveCapture;
    if (!NiagaraEditTestUtils::InvokeExpectSuccess(*this, TEXT("niagara.remove_simulation_stage"), RemovePayload, RemoveCapture))
    {
        return false;
    }

    EmitterData = Handle->GetEmitterData();
    TestEqual(TEXT("simulation stage removed"), EmitterData ? EmitterData->GetSimulationStages().Num() : -1, 0);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNiagaraAddDataInterfaceRoundTripTest,
    "PinWright.Assets.Niagara.AdvancedEdit.DataInterface.AddRemoveRoundTrip",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraAddDataInterfaceRoundTripTest::RunTest(const FString& Parameters)
{
    FString SystemPath;
    UNiagaraSystem* System = NiagaraEditTestUtils::NewTransientSystem(SystemPath);
    FAuthorableSystemRoots Roots{System, nullptr};
    TestNotNull(TEXT("transient system created"), System);
    if (!System)
    {
        return false;
    }

    const TCHAR* DIClassPath = TEXT("/Script/Niagara.NiagaraDataInterfaceCurve");
    const TCHAR* ParameterName = TEXT("MyDI");

    TSharedPtr<FJsonObject> AddPayload = MakeShared<FJsonObject>();
    AddPayload->SetStringField(TEXT("assetPath"), SystemPath);
    AddPayload->SetStringField(TEXT("scope"), TEXT("user"));
    AddPayload->SetStringField(TEXT("parameterName"), ParameterName);
    AddPayload->SetStringField(TEXT("dataInterfaceClass"), DIClassPath);

    FTestResponseCapture AddCapture;
    if (!NiagaraEditTestUtils::InvokeExpectSuccess(*this, TEXT("niagara.add_data_interface"), AddPayload, AddCapture))
    {
        return false;
    }

    FNiagaraParameterStore& UserStore = System->GetExposedParameters();
    FNiagaraVariable Probe{FNiagaraTypeDefinition(UNiagaraDataInterfaceCurve::StaticClass()), FName(ParameterName)};
    const int32 Offset = UserStore.IndexOf(Probe);
    TestTrue(TEXT("parameter offset is valid"), Offset != INDEX_NONE);
    if (Offset == INDEX_NONE)
    {
        return false;
    }
    const TArray<UNiagaraDataInterface*>& DIs = UserStore.GetDataInterfaces();
    TestTrue(TEXT("DI slot in range"), DIs.IsValidIndex(Offset));
    if (!DIs.IsValidIndex(Offset))
    {
        return false;
    }
    UNiagaraDataInterface* DI = DIs[Offset];
    TestNotNull(TEXT("data interface assigned (proves SetDataInterface ran, not just AddParameter)"), DI);
    if (!DI)
    {
        return false;
    }
    TestTrue(TEXT("DI class matches request"), DI->IsA(UNiagaraDataInterfaceCurve::StaticClass()));

    TSharedPtr<FJsonObject> RemovePayload = MakeShared<FJsonObject>();
    RemovePayload->SetStringField(TEXT("assetPath"), SystemPath);
    RemovePayload->SetStringField(TEXT("scope"), TEXT("user"));
    RemovePayload->SetStringField(TEXT("parameterName"), ParameterName);

    FTestResponseCapture RemoveCapture;
    if (!NiagaraEditTestUtils::InvokeExpectSuccess(*this, TEXT("niagara.remove_data_interface"), RemovePayload, RemoveCapture))
    {
        return false;
    }

    const int32 OffsetAfter = UserStore.IndexOf(Probe);
    TestEqual(TEXT("parameter removed"), OffsetAfter, INDEX_NONE);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNiagaraSetPropertyOnEventHandlerTest,
    "PinWright.Assets.Niagara.AdvancedEdit.SetProperty.EventHandlerKind",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraSetPropertyOnEventHandlerTest::RunTest(const FString& Parameters)
{
    FString SystemPath;
    UNiagaraSystem* System = nullptr;
    UNiagaraEmitter* SourceEmitter = nullptr;
    FName EmitterName;
    const bool bSetupOk = MakeAuthorableSystem(SystemPath, System, SourceEmitter, EmitterName);
    FAuthorableSystemRoots Roots{System, SourceEmitter};
    if (!TestTrue(TEXT("authorable system created"), bSetupOk))
    {
        return false;
    }

    TSharedPtr<FJsonObject> AddPayload = MakeShared<FJsonObject>();
    AddPayload->SetStringField(TEXT("assetPath"), SystemPath);
    AddPayload->SetStringField(TEXT("emitter"), EmitterName.ToString());

    FTestResponseCapture AddCapture;
    if (!NiagaraEditTestUtils::InvokeExpectSuccess(*this, TEXT("niagara.add_event_handler"), AddPayload, AddCapture))
    {
        return false;
    }

    TSharedPtr<FJsonObject> Target = MakeShared<FJsonObject>();
    Target->SetStringField(TEXT("kind"), TEXT("eventHandler"));
    Target->SetStringField(TEXT("emitter"), EmitterName.ToString());
    Target->SetNumberField(TEXT("index"), 0);

    TSharedPtr<FJsonObject> SetPayload = MakeShared<FJsonObject>();
    SetPayload->SetStringField(TEXT("assetPath"), SystemPath);
    SetPayload->SetObjectField(TEXT("target"), Target);
    SetPayload->SetStringField(TEXT("propertyPath"), TEXT("SpawnNumber"));
    SetPayload->SetNumberField(TEXT("value"), 5);

    FTestResponseCapture SetCapture;
    if (!NiagaraEditTestUtils::InvokeExpectSuccess(*this, TEXT("niagara.set_property"), SetPayload, SetCapture))
    {
        return false;
    }

    FNiagaraEmitterHandle* Handle = nullptr;
    for (const FNiagaraEmitterHandle& H : System->GetEmitterHandles())
    {
        if (H.GetName() == EmitterName)
        {
            Handle = const_cast<FNiagaraEmitterHandle*>(&H);
            break;
        }
    }
    if (!TestNotNull(TEXT("emitter handle"), Handle))
    {
        return false;
    }
    FVersionedNiagaraEmitterData* EmitterData = Handle->GetEmitterData();
    if (!TestNotNull(TEXT("emitter data"), EmitterData))
    {
        return false;
    }
    TestEqual(TEXT("event handler count is 1"), EmitterData->EventHandlerScriptProps.Num(), 1);
    if (EmitterData->EventHandlerScriptProps.Num() != 1)
    {
        return false;
    }
    TestEqual(TEXT("SpawnNumber set via eventHandler kind"),
        static_cast<int32>(EmitterData->EventHandlerScriptProps[0].SpawnNumber), 5);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNiagaraSetPropertyOnSimulationStageTest,
    "PinWright.Assets.Niagara.AdvancedEdit.SetProperty.SimulationStageKind",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraSetPropertyOnSimulationStageTest::RunTest(const FString& Parameters)
{
    FString SystemPath;
    UNiagaraSystem* System = nullptr;
    UNiagaraEmitter* SourceEmitter = nullptr;
    FName EmitterName;
    const bool bSetupOk = MakeAuthorableSystem(SystemPath, System, SourceEmitter, EmitterName);
    FAuthorableSystemRoots Roots{System, SourceEmitter};
    if (!TestTrue(TEXT("authorable system created"), bSetupOk))
    {
        return false;
    }

    TSharedPtr<FJsonObject> AddPayload = MakeShared<FJsonObject>();
    AddPayload->SetStringField(TEXT("assetPath"), SystemPath);
    AddPayload->SetStringField(TEXT("emitter"), EmitterName.ToString());

    FTestResponseCapture AddCapture;
    if (!NiagaraEditTestUtils::InvokeExpectSuccess(*this, TEXT("niagara.add_simulation_stage"), AddPayload, AddCapture))
    {
        return false;
    }

    TSharedPtr<FJsonObject> Target = MakeShared<FJsonObject>();
    Target->SetStringField(TEXT("kind"), TEXT("simulationStage"));
    Target->SetStringField(TEXT("emitter"), EmitterName.ToString());
    Target->SetNumberField(TEXT("index"), 0);

    TSharedPtr<FJsonObject> SetPayload = MakeShared<FJsonObject>();
    SetPayload->SetStringField(TEXT("assetPath"), SystemPath);
    SetPayload->SetObjectField(TEXT("target"), Target);
    SetPayload->SetStringField(TEXT("propertyPath"), TEXT("bEnabled"));
    SetPayload->SetBoolField(TEXT("value"), false);

    FTestResponseCapture SetCapture;
    if (!NiagaraEditTestUtils::InvokeExpectSuccess(*this, TEXT("niagara.set_property"), SetPayload, SetCapture))
    {
        return false;
    }

    FNiagaraEmitterHandle* Handle = nullptr;
    for (const FNiagaraEmitterHandle& H : System->GetEmitterHandles())
    {
        if (H.GetName() == EmitterName)
        {
            Handle = const_cast<FNiagaraEmitterHandle*>(&H);
            break;
        }
    }
    if (!TestNotNull(TEXT("emitter handle"), Handle))
    {
        return false;
    }
    FVersionedNiagaraEmitterData* EmitterData = Handle->GetEmitterData();
    if (!TestNotNull(TEXT("emitter data"), EmitterData))
    {
        return false;
    }
    const TArray<UNiagaraSimulationStageBase*>& Stages = EmitterData->GetSimulationStages();
    TestEqual(TEXT("stage count is 1"), Stages.Num(), 1);
    if (Stages.Num() != 1 || !Stages[0])
    {
        return false;
    }
    TestEqual(TEXT("bEnabled flipped via simulationStage kind"), static_cast<int32>(Stages[0]->bEnabled), 0);
    return true;
}
