// Copyright (c) 2026 Alexander Penkin. MIT License.

// Tests for the control-flow Niagara node emitters (NIR v1c —
// F-niagara-decompile-nir-script-graphs). Exercises NIRGraphEmit_Control's five
// concrete branches (StaticSwitch, If, Select, UsageSelector, SimTargetSelector)
// by constructing each node type directly in a transient UNiagaraGraph and
// asserting the emitted line shape. These tests target the per-family emitter
// function rather than the full BuildNiagaraIrText pipeline so they remain
// executable without a live system fixture — the system-level shape is covered
// by FNiagaraNirRpcAndDumpParityTest.

#include "Misc/AutomationTest.h"

#include "NIR/NIRTextEmitter.h"

#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_Niagara.h"
#include "NiagaraGraph.h"
#include "NiagaraNodeIf.h"
#include "NiagaraNodeSelect.h"
#include "NiagaraNodeSimTargetSelector.h"
#include "NiagaraNodeStaticSwitch.h"
#include "NiagaraNodeUsageSelector.h"
#include "NiagaraScriptSource.h"
#include "UObject/Package.h"

namespace
{
    UNiagaraGraph* NewTransientNirControlGraph()
    {
        UNiagaraScriptSource* Source = NewObject<UNiagaraScriptSource>(
            GetTransientPackage(),
            UNiagaraScriptSource::StaticClass(),
            NAME_None,
            RF_Transient);
        UNiagaraGraph* Graph = NewObject<UNiagaraGraph>(
            Source,
            UNiagaraGraph::StaticClass(),
            NAME_None,
            RF_Transient);
        Graph->Schema = UEdGraphSchema_Niagara::StaticClass();
        Source->NodeGraph = Graph;
        return Graph;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNiagaraNirGraphStaticSwitchTest,
    "PinWright.niagara.decompile_nir.GraphStaticSwitch",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraNirGraphStaticSwitchTest::RunTest(const FString& Parameters)
{
    UNiagaraGraph* Graph = NewTransientNirControlGraph();
    TestNotNull(TEXT("Transient graph constructed"), Graph);
    if (!Graph) return false;

    UNiagaraNodeStaticSwitch* Node = NewObject<UNiagaraNodeStaticSwitch>(
        Graph, UNiagaraNodeStaticSwitch::StaticClass(), NAME_None, RF_Transient);
    Node->NodePosX = 30;
    Node->NodePosY = 40;
    Node->InputParameterName = FName(TEXT("UseGravity"));
    Node->SwitchTypeData.SwitchType = ENiagaraStaticSwitchType::Bool;
    Graph->AddNode(Node, false, false);

    TArray<FString> Warnings;
    FNIRTextEmitter Emitter(&Warnings);
    const bool bHandled = NIRGraphEmit_Control(Node, Emitter);
    TestTrue(TEXT("StaticSwitch handled by control family"), bHandled);

    const FString Text = Emitter.ToString();
    TestTrue(TEXT("staticSwitch block header emitted"), Text.Contains(TEXT("staticSwitch ")));
    TestTrue(TEXT("switch parameter name emitted"), Text.Contains(TEXT("$UseGravity")));
    TestTrue(TEXT("position suffix emitted"), Text.Contains(TEXT("@(30, 40)")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNiagaraNirGraphIfTest,
    "PinWright.niagara.decompile_nir.GraphIf",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraNirGraphIfTest::RunTest(const FString& Parameters)
{
    UNiagaraGraph* Graph = NewTransientNirControlGraph();
    TestNotNull(TEXT("Transient graph constructed"), Graph);
    if (!Graph) return false;

    UNiagaraNodeIf* Node = NewObject<UNiagaraNodeIf>(
        Graph, UNiagaraNodeIf::StaticClass(), NAME_None, RF_Transient);
    Node->NodePosX = 70;
    Node->NodePosY = 80;
    Graph->AddNode(Node, false, false);

    TArray<FString> Warnings;
    FNIRTextEmitter Emitter(&Warnings);
    const bool bHandled = NIRGraphEmit_Control(Node, Emitter);
    TestTrue(TEXT("If handled by control family"), bHandled);

    const FString Text = Emitter.ToString();
    TestTrue(TEXT("if block header emitted"), Text.Contains(TEXT("if (")));
    TestTrue(TEXT("position suffix emitted"), Text.Contains(TEXT("@(70, 80)")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNiagaraNirGraphSelectTest,
    "PinWright.niagara.decompile_nir.GraphSelect",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraNirGraphSelectTest::RunTest(const FString& Parameters)
{
    UNiagaraGraph* Graph = NewTransientNirControlGraph();
    TestNotNull(TEXT("Transient graph constructed"), Graph);
    if (!Graph) return false;

    UNiagaraNodeSelect* Node = NewObject<UNiagaraNodeSelect>(
        Graph, UNiagaraNodeSelect::StaticClass(), NAME_None, RF_Transient);
    Node->NodePosX = 110;
    Node->NodePosY = 120;
    Graph->AddNode(Node, false, false);

    TArray<FString> Warnings;
    FNIRTextEmitter Emitter(&Warnings);
    const bool bHandled = NIRGraphEmit_Control(Node, Emitter);
    TestTrue(TEXT("Select handled by control family"), bHandled);

    const FString Text = Emitter.ToString();
    TestTrue(TEXT("select block header emitted"), Text.Contains(TEXT("select ")));
    TestTrue(TEXT("position suffix emitted"), Text.Contains(TEXT("@(110, 120)")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNiagaraNirGraphUsageSelectorTest,
    "PinWright.niagara.decompile_nir.GraphUsageSelector",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraNirGraphUsageSelectorTest::RunTest(const FString& Parameters)
{
    UNiagaraGraph* Graph = NewTransientNirControlGraph();
    TestNotNull(TEXT("Transient graph constructed"), Graph);
    if (!Graph) return false;

    UNiagaraNodeUsageSelector* Node = NewObject<UNiagaraNodeUsageSelector>(
        Graph, UNiagaraNodeUsageSelector::StaticClass(), NAME_None, RF_Transient);
    Node->NodePosX = 150;
    Node->NodePosY = 160;
    Graph->AddNode(Node, false, false);

    TArray<FString> Warnings;
    FNIRTextEmitter Emitter(&Warnings);
    const bool bHandled = NIRGraphEmit_Control(Node, Emitter);
    TestTrue(TEXT("UsageSelector handled by control family"), bHandled);

    const FString Text = Emitter.ToString();
    TestTrue(TEXT("selectUsage block header emitted"), Text.Contains(TEXT("selectUsage ")));
    TestTrue(TEXT("position suffix emitted"), Text.Contains(TEXT("@(150, 160)")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNiagaraNirGraphSimTargetSelectorTest,
    "PinWright.niagara.decompile_nir.GraphSimTargetSelector",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraNirGraphSimTargetSelectorTest::RunTest(const FString& Parameters)
{
    UNiagaraGraph* Graph = NewTransientNirControlGraph();
    TestNotNull(TEXT("Transient graph constructed"), Graph);
    if (!Graph) return false;

    UNiagaraNodeSimTargetSelector* Node = NewObject<UNiagaraNodeSimTargetSelector>(
        Graph, UNiagaraNodeSimTargetSelector::StaticClass(), NAME_None, RF_Transient);
    Node->NodePosX = 190;
    Node->NodePosY = 200;
    Graph->AddNode(Node, false, false);

    TArray<FString> Warnings;
    FNIRTextEmitter Emitter(&Warnings);
    const bool bHandled = NIRGraphEmit_Control(Node, Emitter);
    TestTrue(TEXT("SimTargetSelector handled by control family"), bHandled);

    const FString Text = Emitter.ToString();
    TestTrue(TEXT("selectSimTarget block header emitted"), Text.Contains(TEXT("selectSimTarget ")));
    TestTrue(TEXT("position suffix emitted"), Text.Contains(TEXT("@(190, 200)")));
    return true;
}
