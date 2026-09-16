// Copyright (c) 2026 Alexander Penkin. MIT License.

// Tests for the utility-family Niagara node emitters (NIR v1c —
// F-niagara-decompile-nir-script-graphs). Exercises NIRGraphEmit_Util's three
// concrete branches (CustomHlsl, Convert, Reroute) by constructing each node
// type directly in a transient UNiagaraGraph and asserting the emitted line
// shape. These tests target the per-family emitter function rather than the
// full BuildNiagaraIrText pipeline so they remain executable without a live
// system fixture — the system-level shape is covered by FNiagaraNirRpcAndDumpParityTest.

#include "Misc/AutomationTest.h"

#include "NIR/NIRTextEmitter.h"

#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_Niagara.h"
#include "NiagaraGraph.h"
#include "NiagaraNodeConvert.h"
#include "NiagaraNodeCustomHlsl.h"
#include "NiagaraNodeReroute.h"
#include "NiagaraScriptSource.h"
#include "UObject/Package.h"

namespace
{
    UNiagaraGraph* NewTransientNirUtilGraph()
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNiagaraNirGraphCustomHlslTest,
    "PinWright.niagara.decompile_nir.GraphCustomHlsl",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraNirGraphCustomHlslTest::RunTest(const FString& Parameters)
{
    UNiagaraGraph* Graph = NewTransientNirUtilGraph();
    TestNotNull(TEXT("Transient graph constructed"), Graph);
    if (!Graph) return false;

    UNiagaraNodeCustomHlsl* Node = NewObject<UNiagaraNodeCustomHlsl>(
        Graph, UNiagaraNodeCustomHlsl::StaticClass(), NAME_None, RF_Transient);
    Node->NodePosX = 100;
    Node->NodePosY = 200;
    // Assign HLSL source through the reflected UPROPERTY — SetCustomHlsl is not
    // NIAGARAEDITOR_API in UE 5.6, mirroring the path NiagaraGraphHandler.cpp uses.
    if (FStrProperty* CustomHlslProp = FindFProperty<FStrProperty>(
            UNiagaraNodeCustomHlsl::StaticClass(), TEXT("CustomHlsl")))
    {
        CustomHlslProp->SetPropertyValue_InContainer(Node, FString(TEXT("return 1.0f;")));
    }
    Graph->AddNode(Node, false, false);

    TArray<FString> Warnings;
    FNIRTextEmitter Emitter(&Warnings);
    const bool bHandled = NIRGraphEmit_Util(Node, Emitter);
    TestTrue(TEXT("CustomHlsl handled by util family"), bHandled);

    const FString Text = Emitter.ToString();
    TestTrue(TEXT("CustomHlsl block header emitted"), Text.Contains(TEXT("customHlsl ")));
    TestTrue(TEXT("HLSL body line emitted verbatim"), Text.Contains(TEXT("return 1.0f;")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNiagaraNirGraphConvertTest,
    "PinWright.niagara.decompile_nir.GraphConvert",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraNirGraphConvertTest::RunTest(const FString& Parameters)
{
    UNiagaraGraph* Graph = NewTransientNirUtilGraph();
    TestNotNull(TEXT("Transient graph constructed"), Graph);
    if (!Graph) return false;

    // UNiagaraNodeConvert has no NIAGARAEDITOR_API; resolve its UClass via reflection
    // to avoid an unresolved GetPrivateStaticClass link error on UE 5.4-5.7.
    UClass* ConvertClass = FindObject<UClass>(nullptr, TEXT("/Script/NiagaraEditor.NiagaraNodeConvert"));
    TestNotNull(TEXT("UNiagaraNodeConvert class resolved via reflection"), ConvertClass);
    if (!ConvertClass) return false;
    UNiagaraNodeConvert* Node = static_cast<UNiagaraNodeConvert*>(
        NewObject<UEdGraphNode>(Graph, ConvertClass, NAME_None, RF_Transient));
    Node->NodePosX = 50;
    Node->NodePosY = 60;
    Graph->AddNode(Node, false, false);

    TArray<FString> Warnings;
    FNIRTextEmitter Emitter(&Warnings);
    const bool bHandled = NIRGraphEmit_Util(Node, Emitter);
    TestTrue(TEXT("Convert handled by util family"), bHandled);

    const FString Text = Emitter.ToString();
    TestTrue(TEXT("convert line emitted"), Text.Contains(TEXT("convert (")));
    TestTrue(TEXT("position suffix emitted"), Text.Contains(TEXT("@(50, 60)")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNiagaraNirGraphRerouteTest,
    "PinWright.niagara.decompile_nir.GraphReroute",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraNirGraphRerouteTest::RunTest(const FString& Parameters)
{
    UNiagaraGraph* Graph = NewTransientNirUtilGraph();
    TestNotNull(TEXT("Transient graph constructed"), Graph);
    if (!Graph) return false;

    UNiagaraNodeReroute* Node = NewObject<UNiagaraNodeReroute>(
        Graph, UNiagaraNodeReroute::StaticClass(), NAME_None, RF_Transient);
    Node->NodePosX = 10;
    Node->NodePosY = 20;
    Graph->AddNode(Node, false, false);

    TArray<FString> Warnings;
    FNIRTextEmitter Emitter(&Warnings);
    const bool bHandled = NIRGraphEmit_Util(Node, Emitter);
    TestTrue(TEXT("Reroute handled by util family"), bHandled);

    const FString Text = Emitter.ToString();
    TestTrue(TEXT("reroute line emitted"), Text.Contains(TEXT("reroute ")));
    TestTrue(TEXT("position suffix emitted"), Text.Contains(TEXT("@(10, 20)")));
    return true;
}
