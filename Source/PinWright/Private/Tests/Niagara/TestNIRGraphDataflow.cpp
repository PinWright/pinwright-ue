// Copyright (c) 2026 Alexander Penkin. MIT License.

// NIR v1c data-flow node-emission tests.
//
// Each test builds a real authored Niagara system via NIRTestFixtures, walks to
// the emitter's spawn script graph, authors the node-class under test directly
// onto that graph with FGraphNodeCreator, then runs BuildNiagaraIrText against
// the spawn script itself and asserts the per-node line shape declared in
// NIRGraphEmitter_Dataflow.cpp.
//
// Why pass the *script* (not the system) to BuildNiagaraIrText:
// EmitSystem walks each emitter via AppendStack, which only enumerates
// UNiagaraNodeFunctionCall modules — it does NOT walk arbitrary nodes authored
// on the spawn-script graph. Only the script-asset path (EmitScriptPlaceholder
// → EmitScriptGraphScope → EmitGraphBody) dispatches every node to the family
// emitters. So we use the system fixture to obtain a properly PostLoad-wired
// emitter spawn script, then feed that script directly to the decompiler.
//
// Assertions anchor on at least two distinct tokens (the per-class line keyword
// plus the position-suffix "@(" or another distinctive substring) so loose
// substring noise can't pass a test.

#include "Misc/AutomationTest.h"

#include "Handlers/Niagara/NiagaraDumpBuilder.h"
#include "NIR/NIRDecompiler.h"
#include "Tests/Assets/NiagaraEditTestUtils.h"
#include "Tests/Niagara/TestNIRFixtures.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_Niagara.h"
#include "NiagaraEmitter.h"
#include "NiagaraEmitterHandle.h"
#include "NiagaraGraph.h"
#include "NiagaraNodeFunctionCall.h"
#include "NiagaraNodeInput.h"
#include "NiagaraNodeOp.h"
#include "NiagaraNodeParameterMapGet.h"
#include "NiagaraNodeParameterMapSet.h"
#include "NiagaraScript.h"
#include "NiagaraScriptSource.h"
#include "NiagaraSystem.h"
#include "NiagaraTypes.h"
#include "UObject/UObjectGlobals.h"

namespace
{
    // Locate the spawn script (and its graph) of the first emitter on the fixture
    // system. Returns nullptr on either count when the fixture is malformed.
    UNiagaraScript* GetFirstEmitterSpawnScript(UNiagaraSystem* System)
    {
        if (!System)
        {
            return nullptr;
        }
        for (const FNiagaraEmitterHandle& Handle : System->GetEmitterHandles())
        {
            if (FVersionedNiagaraEmitterData* EmitterData = Handle.GetEmitterData())
            {
                if (UNiagaraScript* Script = EmitterData->SpawnScriptProps.Script)
                {
                    return Script;
                }
            }
        }
        return nullptr;
    }

    UNiagaraGraph* GetGraphForScript(UNiagaraScript* Script)
    {
        if (!Script)
        {
            return nullptr;
        }
        if (UNiagaraScriptSource* Source = Cast<UNiagaraScriptSource>(Script->GetLatestSource()))
        {
            return Source->NodeGraph;
        }
        return nullptr;
    }

    void CaptureFixtureRoots(
        UNiagaraSystem* System,
        NiagaraEditTestUtils::FAuthorableSystemRoots& OutRoots)
    {
        OutRoots.System = System;
        if (!System)
        {
            return;
        }
        for (const FNiagaraEmitterHandle& Handle : System->GetEmitterHandles())
        {
            OutRoots.Emitter = Handle.GetInstance().Emitter.Get();
            break;
        }
    }

    // Shared fixture set-up. Returns a system whose first emitter has a valid spawn
    // script + graph, or fails the test with an explanatory log.
    bool BuildScriptHostingGraph(
        FAutomationTestBase& Test,
        FName SystemName,
        UNiagaraSystem*& OutSystem,
        UNiagaraScript*& OutScript,
        UNiagaraGraph*& OutGraph)
    {
        OutSystem = NIRTestFixtures::BuildEmptySystemWithEmitter(SystemName);
        if (!OutSystem)
        {
            Test.AddError(TEXT("Could not build fixture system; the seed Niagara asset for NewTransientSystem is unavailable in this cooked build."));
            return false;
        }
        OutScript = GetFirstEmitterSpawnScript(OutSystem);
        OutGraph = GetGraphForScript(OutScript);
        if (!OutScript || !OutGraph)
        {
            Test.AddError(TEXT("Fixture system has no emitter spawn script or graph."));
            NIRTestFixtures::DestroyFixture(OutSystem);
            return false;
        }
        return true;
    }

    UEdGraphPin* AddDynamicParameterPin(
        FAutomationTestBase& Test,
        UNiagaraNode* Node,
        EEdGraphPinDirection Direction,
        const FNiagaraTypeDefinition& Type,
        FName PinName)
    {
        if (!Node)
        {
            Test.AddError(TEXT("Cannot add dynamic parameter pin to a null Niagara node."));
            return nullptr;
        }

        const FName DynamicAddPinSubCategory(TEXT("DynamicAddPin"));
        UEdGraphPin* AddPin = nullptr;
        for (UEdGraphPin* Pin : Node->Pins)
        {
            if (Pin
                && Pin->Direction == Direction
                && Pin->PinType.PinCategory == UEdGraphSchema_Niagara::PinCategoryMisc
                && Pin->PinType.PinSubCategory == DynamicAddPinSubCategory)
            {
                AddPin = Pin;
                break;
            }
        }

        if (!AddPin)
        {
            Test.AddError(TEXT("Niagara node has no matching dynamic add pin."));
            return nullptr;
        }

        AddPin->PinType = UEdGraphSchema_Niagara::TypeDefinitionToPinType(Type);
        AddPin->PinType.PinSubCategory = FName(TEXT("ParameterPin"));
        AddPin->PinName = PinName;
        AddPin->PinFriendlyName = FText::AsCultureInvariant(PinName.ToString());
        if (!AddPin->PersistentGuid.IsValid())
        {
            AddPin->PersistentGuid = FGuid::NewGuid();
        }

        Node->CreatePin(
            Direction,
            FEdGraphPinType(
                UEdGraphSchema_Niagara::PinCategoryMisc,
                DynamicAddPinSubCategory,
                nullptr,
                EPinContainerType::None,
                false,
                FEdGraphTerminalType()),
            TEXT("Add"));

        return AddPin;
    }

    int32 CountSubstring(const FString& Text, const FString& Needle)
    {
        int32 Count = 0;
        int32 SearchStart = 0;
        while (true)
        {
            const int32 Found = Text.Find(Needle, ESearchCase::CaseSensitive, ESearchDir::FromStart, SearchStart);
            if (Found == INDEX_NONE)
            {
                return Count;
            }
            ++Count;
            SearchStart = Found + Needle.Len();
        }
    }
}

// FGraphNodeCreator<T> requires T::StaticClass(), which is not exported for these UCLASS()
// Niagara node types. Build the node through the reflected UClass and replicate Finalize().
template <typename T>
static T* CreateNiagaraNodeByPath(UNiagaraGraph& Graph, const TCHAR* ScriptPath)
{
    UClass* Cls = FindObject<UClass>(nullptr, ScriptPath);
    if (!Cls) return nullptr;
    T* Node = static_cast<T*>(NewObject<UEdGraphNode>(&Graph, Cls, NAME_None, RF_Transactional));
    Graph.AddNode(Node, /*bUserAction=*/false, /*bSelectNewNode=*/false);
    Node->CreateNewGuid();
    Node->PostPlacedNewNode();
    if (Node->Pins.Num() == 0)
    {
        Node->AllocateDefaultPins();
    }
    return Node;
}

// 1. UNiagaraNodeOp — assert "= op <OpName>(...) @(..." with two anchor tokens.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNiagaraNirGraphOpTest,
    "PinWright.niagara.decompile_nir.GraphOp",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraNirGraphOpTest::RunTest(const FString& Parameters)
{
    UNiagaraSystem* System = nullptr;
    UNiagaraScript* Script = nullptr;
    UNiagaraGraph* Graph = nullptr;
    if (!BuildScriptHostingGraph(*this, FName(TEXT("NirGraphOp")), System, Script, Graph))
    {
        return false;
    }
    NiagaraEditTestUtils::FAuthorableSystemRoots Roots;
    CaptureFixtureRoots(System, Roots);

    FGraphNodeCreator<UNiagaraNodeOp> Creator(*Graph);
    UNiagaraNodeOp* Node = Creator.CreateNode();
    // Numeric::Add is a stock registered op; AllocateDefaultPins looks up FNiagaraOpInfo
    // by this name to seed inputs/outputs, so OpName must be set BEFORE Finalize.
    Node->OpName = TEXT("Numeric::Add");
    Creator.Finalize();

    const FNIRResult Result = NIRDecompiler::BuildNiagaraIrText(Script);
    NIRTestFixtures::DestroyFixture(System);

    TestTrue(TEXT("NIR build succeeds"), Result.bSuccess);
    TestTrue(TEXT("Op line carries op-name token"), Result.Text.Contains(TEXT("op Numeric::Add")));
    TestTrue(TEXT("Op line carries position suffix"), Result.Text.Contains(TEXT("@(")));
    return true;
}

// 2. UNiagaraNodeParameterMapGet — assert "= get $Namespace.Name : <Type>".
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNiagaraNirGraphParameterMapGetLineFormatTest,
    "PinWright.niagara.decompile_nir.GraphParameterMapGet.GetLineFormat",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraNirGraphParameterMapGetLineFormatTest::RunTest(const FString& Parameters)
{
    UNiagaraSystem* System = nullptr;
    UNiagaraScript* Script = nullptr;
    UNiagaraGraph* Graph = nullptr;
    if (!BuildScriptHostingGraph(*this, FName(TEXT("NirGraphMapGet")), System, Script, Graph))
    {
        return false;
    }
    NiagaraEditTestUtils::FAuthorableSystemRoots Roots;
    CaptureFixtureRoots(System, Roots);

    UNiagaraNodeParameterMapGet* Node = CreateNiagaraNodeByPath<UNiagaraNodeParameterMapGet>(
        *Graph, TEXT("/Script/NiagaraEditor.NiagaraNodeParameterMapGet"));
    TestNotNull(TEXT("ParameterMapGet node created"), Node);
    if (!Node)
    {
        NIRTestFixtures::DestroyFixture(System);
        return false;
    }
    // ParameterMapGet starts with only the implicit "Source" map pin after AllocateDefaultPins;
    // we add a typed output pin reading "Engine.DeltaTime" so the emitter has one non-map output
    // to format as "get $Engine.DeltaTime : <Type>".
    AddDynamicParameterPin(*this, Node, EGPD_Output, FNiagaraTypeDefinition::GetFloatDef(), FName(TEXT("Engine.DeltaTime")));

    const FNIRResult Result = NIRDecompiler::BuildNiagaraIrText(Script);
    NIRTestFixtures::DestroyFixture(System);

    TestTrue(TEXT("NIR build succeeds"), Result.bSuccess);
    TestTrue(TEXT("Get line names parameter"),
        Result.Text.Contains(TEXT("= get $Engine.DeltaTime")));
    TestTrue(TEXT("Get line carries position suffix"), Result.Text.Contains(TEXT("@(")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNiagaraNirGraphParameterMapGetMetadataTypeTest,
    "PinWright.niagara.decompile_nir.GraphParameterMapGet.MetadataType",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraNirGraphParameterMapGetMetadataTypeTest::RunTest(const FString& Parameters)
{
    UNiagaraSystem* System = nullptr;
    UNiagaraScript* Script = nullptr;
    UNiagaraGraph* Graph = nullptr;
    if (!BuildScriptHostingGraph(*this, FName(TEXT("NirGraphMapGetMetadataType")), System, Script, Graph))
    {
        return false;
    }
    NiagaraEditTestUtils::FAuthorableSystemRoots Roots;
    CaptureFixtureRoots(System, Roots);

    UNiagaraNodeParameterMapGet* Node = CreateNiagaraNodeByPath<UNiagaraNodeParameterMapGet>(
        *Graph, TEXT("/Script/NiagaraEditor.NiagaraNodeParameterMapGet"));
    TestNotNull(TEXT("ParameterMapGet node created"), Node);
    if (!Node)
    {
        NIRTestFixtures::DestroyFixture(System);
        return false;
    }
    UEdGraphPin* MetadataTypedPin = AddDynamicParameterPin(
        *this,
        Node,
        EGPD_Output,
        FNiagaraTypeDefinition::GetIDDef(),
        FName(TEXT("Engine.OwnerID")));
    if (MetadataTypedPin)
    {
        MetadataTypedPin->PinType.PinCategory = UEdGraphSchema_Niagara::PinCategoryMisc;
    }

    const FNIRResult Result = NIRDecompiler::BuildNiagaraIrText(Script);
    NIRTestFixtures::DestroyFixture(System);

    const FString ExpectedType = FNiagaraTypeDefinition::GetIDDef().GetName();
    TestTrue(TEXT("NIR build succeeds"), Result.bSuccess);
    TestTrue(TEXT("Get line preserves non-primitive metadata type"),
        Result.Text.Contains(FString::Printf(TEXT("= get $Engine.OwnerID : %s"), *ExpectedType)));
    TestFalse(TEXT("Get line does not render metadata type as Unknown"),
        Result.Text.Contains(TEXT("= get $Engine.OwnerID : Unknown")));
    return true;
}

// 3. UNiagaraNodeParameterMapSet — assert "set $Namespace.Name = %value @(".
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNiagaraNirGraphParameterMapSetLineFormatTest,
    "PinWright.niagara.decompile_nir.GraphParameterMapSet.SetLineFormat",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraNirGraphParameterMapSetLineFormatTest::RunTest(const FString& Parameters)
{
    UNiagaraSystem* System = nullptr;
    UNiagaraScript* Script = nullptr;
    UNiagaraGraph* Graph = nullptr;
    if (!BuildScriptHostingGraph(*this, FName(TEXT("NirGraphMapSet")), System, Script, Graph))
    {
        return false;
    }
    NiagaraEditTestUtils::FAuthorableSystemRoots Roots;
    CaptureFixtureRoots(System, Roots);

    UNiagaraNodeParameterMapSet* Node = CreateNiagaraNodeByPath<UNiagaraNodeParameterMapSet>(
        *Graph, TEXT("/Script/NiagaraEditor.NiagaraNodeParameterMapSet"));
    TestNotNull(TEXT("ParameterMapSet node created"), Node);
    if (!Node)
    {
        NIRTestFixtures::DestroyFixture(System);
        return false;
    }
    // Add a typed input pin writing "Particles.Velocity"; the emitter formats this as
    // "set $Particles.Velocity = %... @(x, y)".
    AddDynamicParameterPin(*this, Node, EGPD_Input, FNiagaraTypeDefinition::GetVec3Def(), FName(TEXT("Particles.Velocity")));

    const FNIRResult Result = NIRDecompiler::BuildNiagaraIrText(Script);
    NIRTestFixtures::DestroyFixture(System);

    TestTrue(TEXT("NIR build succeeds"), Result.bSuccess);
    TestTrue(TEXT("Set line names parameter"),
        Result.Text.Contains(TEXT("set $Particles.Velocity =")));
    TestTrue(TEXT("Set line carries position suffix"), Result.Text.Contains(TEXT("@(")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNiagaraNirGraphExplicitLinkTest,
    "PinWright.niagara.decompile_nir.GraphExplicitLink",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraNirGraphExplicitLinkTest::RunTest(const FString& Parameters)
{
    UNiagaraSystem* System = nullptr;
    UNiagaraScript* Script = nullptr;
    UNiagaraGraph* Graph = nullptr;
    if (!BuildScriptHostingGraph(*this, FName(TEXT("NirGraphExplicitLink")), System, Script, Graph))
    {
        return false;
    }
    NiagaraEditTestUtils::FAuthorableSystemRoots Roots;
    CaptureFixtureRoots(System, Roots);

    UNiagaraNodeParameterMapGet* GetNode = CreateNiagaraNodeByPath<UNiagaraNodeParameterMapGet>(
        *Graph, TEXT("/Script/NiagaraEditor.NiagaraNodeParameterMapGet"));
    TestNotNull(TEXT("ParameterMapGet node created"), GetNode);
    if (!GetNode)
    {
        NIRTestFixtures::DestroyFixture(System);
        return false;
    }
    UEdGraphPin* SourcePin = AddDynamicParameterPin(
        *this,
        GetNode,
        EGPD_Output,
        FNiagaraTypeDefinition::GetFloatDef(),
        FName(TEXT("Engine.DeltaTime")));

    UNiagaraNodeParameterMapSet* SetNode = CreateNiagaraNodeByPath<UNiagaraNodeParameterMapSet>(
        *Graph, TEXT("/Script/NiagaraEditor.NiagaraNodeParameterMapSet"));
    TestNotNull(TEXT("ParameterMapSet node created"), SetNode);
    if (!SetNode)
    {
        NIRTestFixtures::DestroyFixture(System);
        return false;
    }
    UEdGraphPin* TargetPin = AddDynamicParameterPin(
        *this,
        SetNode,
        EGPD_Input,
        FNiagaraTypeDefinition::GetFloatDef(),
        FName(TEXT("Particles.CustomDeltaTime")));

    if (!SourcePin || !TargetPin || !Graph->GetSchema()->TryCreateConnection(SourcePin, TargetPin))
    {
        AddError(TEXT("Could not create explicit Niagara graph link fixture."));
        NIRTestFixtures::DestroyFixture(System);
        return false;
    }

    UNiagaraNodeParameterMapGet* DuplicateGetNode = CreateNiagaraNodeByPath<UNiagaraNodeParameterMapGet>(
        *Graph, TEXT("/Script/NiagaraEditor.NiagaraNodeParameterMapGet"));
    TestNotNull(TEXT("ParameterMapGet duplicate node created"), DuplicateGetNode);
    if (!DuplicateGetNode)
    {
        NIRTestFixtures::DestroyFixture(System);
        return false;
    }
    AddDynamicParameterPin(
        *this,
        DuplicateGetNode,
        EGPD_Output,
        FNiagaraTypeDefinition::GetFloatDef(),
        FName(TEXT("Engine.Age")));

    const TSharedPtr<FJsonObject> GraphsJson = NiagaraDumpBuilder::BuildScriptGraphsJson(Script);
    const TArray<TSharedPtr<FJsonValue>> Graphs = GraphsJson.IsValid()
        ? GraphsJson->GetArrayField(TEXT("graphs"))
        : TArray<TSharedPtr<FJsonValue>>();
    const TSharedPtr<FJsonObject> FirstGraph = !Graphs.IsEmpty() && Graphs[0].IsValid()
        ? Graphs[0]->AsObject()
        : nullptr;
    const int32 JsonLinkCount = FirstGraph.IsValid()
        ? FirstGraph->GetArrayField(TEXT("links")).Num()
        : 0;

    const FNIRResult Result = NIRDecompiler::BuildNiagaraIrText(Script);
    NIRTestFixtures::DestroyFixture(System);

    TestTrue(TEXT("NIR build succeeds"), Result.bSuccess);
    TestTrue(TEXT("Graph emits node identity lines"), Result.Text.Contains(TEXT("node ")));
    TestTrue(TEXT("Duplicate node display names are suffixed"),
        Result.Text.Contains(TEXT("_2 : NiagaraNodeParameterMapGet")));
    TestTrue(TEXT("Graph emits ASCII link arrow"), Result.Text.Contains(TEXT(" -> ")));
    TestEqual(TEXT("NIR link count matches graph JSON link count"),
        CountSubstring(Result.Text, TEXT("link ")),
        JsonLinkCount);
    TestTrue(TEXT("Graph link references source pin"),
        Result.Text.Contains(TEXT("Engine.DeltaTime")));
    TestTrue(TEXT("Graph link references target pin"),
        Result.Text.Contains(TEXT("Particles.CustomDeltaTime")));
    return true;
}

// 4. UNiagaraNodeFunctionCall — assert "call @<ModuleName>" with anchor tokens.
// FunctionCall emission is already exercised by FNiagaraNirOverrideDynamicInputTest
// in TestNIRDecompiler.cpp (which builds a real function-call chain via the
// stack-edit pipeline). We synthesize a minimal call here directly on the graph
// so the family dispatch path's call-line shape is asserted in isolation.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNiagaraNirGraphFunctionCallTest,
    "PinWright.niagara.decompile_nir.GraphFunctionCall",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraNirGraphFunctionCallTest::RunTest(const FString& Parameters)
{
    UNiagaraSystem* System = nullptr;
    UNiagaraScript* Script = nullptr;
    UNiagaraGraph* Graph = nullptr;
    if (!BuildScriptHostingGraph(*this, FName(TEXT("NirGraphCall")), System, Script, Graph))
    {
        return false;
    }
    NiagaraEditTestUtils::FAuthorableSystemRoots Roots;
    CaptureFixtureRoots(System, Roots);

    // Load a real DynamicInput script to use as the call target. The Add_Float
    // dynamic input is the smallest stock script and exists in default Niagara content.
    UNiagaraScript* TargetScript = LoadObject<UNiagaraScript>(
        nullptr, TEXT("/Niagara/DynamicInputs/Add/Add_Float.Add_Float"));
    if (!TargetScript)
    {
        AddError(TEXT("Add_Float dynamic-input script not available; cannot exercise FunctionCall emission."));
        NIRTestFixtures::DestroyFixture(System);
        return false;
    }

    FGraphNodeCreator<UNiagaraNodeFunctionCall> Creator(*Graph);
    UNiagaraNodeFunctionCall* Node = Creator.CreateNode();
    Node->FunctionScript = TargetScript;
    Creator.Finalize();

    const FNIRResult Result = NIRDecompiler::BuildNiagaraIrText(Script);
    NIRTestFixtures::DestroyFixture(System);

    TestTrue(TEXT("NIR build succeeds"), Result.bSuccess);
    TestTrue(TEXT("Call line carries call-keyword + module name"),
        Result.Text.Contains(TEXT("call @Add_Float")));
    TestTrue(TEXT("Call line carries position suffix"), Result.Text.Contains(TEXT("@(")));
    return true;
}

// 5. UNiagaraNodeInput — assert "input $Namespace.Name : <Type>".
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNiagaraNirGraphInputTest,
    "PinWright.niagara.decompile_nir.GraphInput",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraNirGraphInputTest::RunTest(const FString& Parameters)
{
    UNiagaraSystem* System = nullptr;
    UNiagaraScript* Script = nullptr;
    UNiagaraGraph* Graph = nullptr;
    if (!BuildScriptHostingGraph(*this, FName(TEXT("NirGraphInput")), System, Script, Graph))
    {
        return false;
    }
    NiagaraEditTestUtils::FAuthorableSystemRoots Roots;
    CaptureFixtureRoots(System, Roots);

    FGraphNodeCreator<UNiagaraNodeInput> Creator(*Graph);
    UNiagaraNodeInput* Node = Creator.CreateNode();
    Node->Input = FNiagaraVariable(FNiagaraTypeDefinition::GetFloatDef(), FName(TEXT("Module.Strength")));
    Creator.Finalize();

    const FNIRResult Result = NIRDecompiler::BuildNiagaraIrText(Script);
    NIRTestFixtures::DestroyFixture(System);

    TestTrue(TEXT("NIR build succeeds"), Result.bSuccess);
    TestTrue(TEXT("Input line names parameter"),
        Result.Text.Contains(TEXT("input $Module.Strength")));
    TestTrue(TEXT("Input line carries position suffix"), Result.Text.Contains(TEXT("@(")));
    return true;
}

// 6. UNiagaraNodeOutput — the fixture-built emitter spawn script's graph always
// terminates in an output node added by NewTransientEmitter's PostLoad pipeline.
// Assert the line shape "output <UsageName>" with the position suffix.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNiagaraNirGraphOutputTest,
    "PinWright.niagara.decompile_nir.GraphOutput",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraNirGraphOutputTest::RunTest(const FString& Parameters)
{
    UNiagaraSystem* System = nullptr;
    UNiagaraScript* Script = nullptr;
    UNiagaraGraph* Graph = nullptr;
    if (!BuildScriptHostingGraph(*this, FName(TEXT("NirGraphOutput")), System, Script, Graph))
    {
        return false;
    }
    NiagaraEditTestUtils::FAuthorableSystemRoots Roots;
    CaptureFixtureRoots(System, Roots);

    const FNIRResult Result = NIRDecompiler::BuildNiagaraIrText(Script);
    NIRTestFixtures::DestroyFixture(System);

    TestTrue(TEXT("NIR build succeeds"), Result.bSuccess);
    // ParticleSpawnScript maps to the "ParticleSpawn" token in FormatUsageName.
    TestTrue(TEXT("Output line names ParticleSpawn usage"),
        Result.Text.Contains(TEXT("output ParticleSpawn")));
    TestTrue(TEXT("Output line carries position suffix"), Result.Text.Contains(TEXT("@(")));
    return true;
}

// 7. UNiagaraNodeParameterMapGet — verify the dynamic "Add" sentinel pin is suppressed.
// AddDynamicParameterPin promotes the initial Add pin to a real typed pin AND creates a
// fresh trailing Add pin (DynamicAddPin sub-category) — the new sentinel must not appear
// in the emitted NIR as "get $Add : Unknown".
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNiagaraNirGraphParameterMapGetAddPinSuppressedTest,
    "PinWright.niagara.decompile_nir.GraphParameterMapGet.AddPinSuppressed",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraNirGraphParameterMapGetAddPinSuppressedTest::RunTest(const FString& Parameters)
{
    UNiagaraSystem* System = nullptr;
    UNiagaraScript* Script = nullptr;
    UNiagaraGraph* Graph = nullptr;
    if (!BuildScriptHostingGraph(*this, FName(TEXT("NirGraphMapGetAddSuppress")), System, Script, Graph))
    {
        return false;
    }
    NiagaraEditTestUtils::FAuthorableSystemRoots Roots;
    CaptureFixtureRoots(System, Roots);

    UNiagaraNodeParameterMapGet* Node = CreateNiagaraNodeByPath<UNiagaraNodeParameterMapGet>(
        *Graph, TEXT("/Script/NiagaraEditor.NiagaraNodeParameterMapGet"));
    TestNotNull(TEXT("ParameterMapGet node created"), Node);
    if (!Node)
    {
        NIRTestFixtures::DestroyFixture(System);
        return false;
    }
    // Promotes the original Add pin to a real "Engine.DeltaTime : float" output and
    // creates a fresh trailing Add sentinel pin — the sentinel is what we're suppressing.
    AddDynamicParameterPin(*this, Node, EGPD_Output, FNiagaraTypeDefinition::GetFloatDef(), FName(TEXT("Engine.DeltaTime")));

    const FNIRResult Result = NIRDecompiler::BuildNiagaraIrText(Script);
    NIRTestFixtures::DestroyFixture(System);

    TestTrue(TEXT("NIR build succeeds"), Result.bSuccess);
    TestFalse(TEXT("Add sentinel pin does not appear in NIR"),
        Result.Text.Contains(TEXT("get $Add :")));
    TestFalse(TEXT("No Unknown type emission on MapGet"),
        Result.Text.Contains(TEXT(": Unknown")));
    TestTrue(TEXT("Real typed pin still emits"),
        Result.Text.Contains(TEXT("= get $Engine.DeltaTime")));
    return true;
}

// 8. UNiagaraNodeParameterMapSet — symmetric Add-pin suppression check.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNiagaraNirGraphParameterMapSetAddPinSuppressedTest,
    "PinWright.niagara.decompile_nir.GraphParameterMapSet.AddPinSuppressed",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraNirGraphParameterMapSetAddPinSuppressedTest::RunTest(const FString& Parameters)
{
    UNiagaraSystem* System = nullptr;
    UNiagaraScript* Script = nullptr;
    UNiagaraGraph* Graph = nullptr;
    if (!BuildScriptHostingGraph(*this, FName(TEXT("NirGraphMapSetAddSuppress")), System, Script, Graph))
    {
        return false;
    }
    NiagaraEditTestUtils::FAuthorableSystemRoots Roots;
    CaptureFixtureRoots(System, Roots);

    UNiagaraNodeParameterMapSet* Node = CreateNiagaraNodeByPath<UNiagaraNodeParameterMapSet>(
        *Graph, TEXT("/Script/NiagaraEditor.NiagaraNodeParameterMapSet"));
    TestNotNull(TEXT("ParameterMapSet node created"), Node);
    if (!Node)
    {
        NIRTestFixtures::DestroyFixture(System);
        return false;
    }
    AddDynamicParameterPin(*this, Node, EGPD_Input, FNiagaraTypeDefinition::GetVec3Def(), FName(TEXT("Particles.Velocity")));

    const FNIRResult Result = NIRDecompiler::BuildNiagaraIrText(Script);
    NIRTestFixtures::DestroyFixture(System);

    TestTrue(TEXT("NIR build succeeds"), Result.bSuccess);
    TestFalse(TEXT("Add sentinel pin does not appear in NIR"),
        Result.Text.Contains(TEXT("set $Add =")));
    TestFalse(TEXT("No Unknown type emission on MapSet"),
        Result.Text.Contains(TEXT(": Unknown")));
    TestTrue(TEXT("Real typed pin still emits"),
        Result.Text.Contains(TEXT("set $Particles.Velocity =")));
    return true;
}
