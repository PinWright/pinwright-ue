// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Misc/AutomationTest.h"

#include "BpirLayoutSettings.h"
#include "Compiler/BpirCompiler.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "EditorAssetLibrary.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "GameFramework/Actor.h"
#include "Handlers/Blueprint/BlueprintHandlerUtils.h"
#include "K2Node_CallFunction.h"
#include "K2Node_Event.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"
#include "Tests/Bpir/BpirGraphTestHelpers.h"
#include "Tests/Bpir/CompilerTestUtils.h"
#include "Tests/TestUtils.h"
#include "UObject/Package.h"

namespace BpirInterfaceOutputOverrideTestUtils
{
    FString MakeInterfaceAssetPath()
    {
        return FString::Printf(
            TEXT("/Game/__PW_GatewayTests/BPI_BpirOutputOverride_%s"),
            *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    }

    FString MakeTargetAssetPath()
    {
        return FString::Printf(
            TEXT("/Game/__PW_GatewayTests/BP_BpirOutputRollback_%s"),
            *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    }

    UBlueprint* CreateActorBlueprint(const FString& AssetPath)
    {
        UPackage* Package = CreatePackage(*AssetPath);
        return Package
            ? FKismetEditorUtilities::CreateBlueprint(
                AActor::StaticClass(),
                Package,
                FName(*FPackageName::GetLongPackageAssetName(AssetPath)),
                BPTYPE_Normal,
                UBlueprint::StaticClass(),
                UBlueprintGeneratedClass::StaticClass())
            : nullptr;
    }

    struct FPinStructureSnapshot
    {
        FString Name;
        EEdGraphPinDirection Direction = EGPD_Input;
        FEdGraphPinType Type;
        TArray<FString> LinkedEndpoints;
        FString SortKey;
    };

    struct FNodeStructureSnapshot
    {
        FString ClassPath;
        TArray<FPinStructureSnapshot> Pins;
        FString SortKey;
    };

    struct FGraphStructureSnapshot
    {
        TArray<FNodeStructureSnapshot> Nodes;
    };

    FString DescribeLinkedEndpoint(const UEdGraphPin* Pin)
    {
        const UEdGraphNode* Owner = Pin ? Pin->GetOwningNode() : nullptr;
        return FString::Printf(
            TEXT("%s|%s|%d"),
            Owner ? *Owner->GetClass()->GetPathName() : TEXT("<no-node>"),
            Pin ? *Pin->PinName.ToString() : TEXT("<no-pin>"),
            Pin ? static_cast<int32>(Pin->Direction) : -1);
    }

    FGraphStructureSnapshot CaptureGraphStructure(const UEdGraph* Graph)
    {
        FGraphStructureSnapshot Snapshot;
        if (!Graph)
        {
            return Snapshot;
        }

        for (const UEdGraphNode* Node : Graph->Nodes)
        {
            if (!Node)
            {
                continue;
            }

            FNodeStructureSnapshot NodeSnapshot;
            NodeSnapshot.ClassPath = Node->GetClass()->GetPathName();
            for (const UEdGraphPin* Pin : Node->Pins)
            {
                if (!Pin)
                {
                    continue;
                }

                FPinStructureSnapshot PinSnapshot;
                PinSnapshot.Name = Pin->PinName.ToString();
                PinSnapshot.Direction = Pin->Direction;
                PinSnapshot.Type = Pin->PinType;
                for (const UEdGraphPin* LinkedPin : Pin->LinkedTo)
                {
                    PinSnapshot.LinkedEndpoints.Add(DescribeLinkedEndpoint(LinkedPin));
                }
                PinSnapshot.LinkedEndpoints.Sort();
                PinSnapshot.SortKey = FString::Printf(
                    TEXT("%s|%d|%s|%s"),
                    *PinSnapshot.Name,
                    static_cast<int32>(PinSnapshot.Direction),
                    *BlueprintHandlerUtils::DescribePinType(PinSnapshot.Type),
                    *FString::Join(PinSnapshot.LinkedEndpoints, TEXT(",")));
                NodeSnapshot.Pins.Add(MoveTemp(PinSnapshot));
            }

            NodeSnapshot.Pins.Sort([](
                const FPinStructureSnapshot& Left,
                const FPinStructureSnapshot& Right)
            {
                return Left.SortKey < Right.SortKey;
            });

            TArray<FString> PinKeys;
            PinKeys.Reserve(NodeSnapshot.Pins.Num());
            for (const FPinStructureSnapshot& Pin : NodeSnapshot.Pins)
            {
                PinKeys.Add(Pin.SortKey);
            }
            NodeSnapshot.SortKey = FString::Printf(
                TEXT("%s|%s"),
                *NodeSnapshot.ClassPath,
                *FString::Join(PinKeys, TEXT(";")));
            Snapshot.Nodes.Add(MoveTemp(NodeSnapshot));
        }

        Snapshot.Nodes.Sort([](
            const FNodeStructureSnapshot& Left,
            const FNodeStructureSnapshot& Right)
        {
            return Left.SortKey < Right.SortKey;
        });
        return Snapshot;
    }

    void TestGraphStructureEqual(
        FAutomationTestBase& Test,
        const FGraphStructureSnapshot& Expected,
        const FGraphStructureSnapshot& Actual)
    {
        Test.TestEqual(TEXT("rollback restores structural node count"),
            Actual.Nodes.Num(), Expected.Nodes.Num());
        const int32 NodeCount = FMath::Min(Actual.Nodes.Num(), Expected.Nodes.Num());
        for (int32 NodeIndex = 0; NodeIndex < NodeCount; ++NodeIndex)
        {
            const FNodeStructureSnapshot& ExpectedNode = Expected.Nodes[NodeIndex];
            const FNodeStructureSnapshot& ActualNode = Actual.Nodes[NodeIndex];
            Test.TestEqual(
                *FString::Printf(TEXT("rollback restores node class %d"), NodeIndex),
                ActualNode.ClassPath,
                ExpectedNode.ClassPath);
            Test.TestEqual(
                *FString::Printf(TEXT("rollback restores pin count for node %d"), NodeIndex),
                ActualNode.Pins.Num(),
                ExpectedNode.Pins.Num());

            const int32 PinCount = FMath::Min(ActualNode.Pins.Num(), ExpectedNode.Pins.Num());
            for (int32 PinIndex = 0; PinIndex < PinCount; ++PinIndex)
            {
                const FPinStructureSnapshot& ExpectedPin = ExpectedNode.Pins[PinIndex];
                const FPinStructureSnapshot& ActualPin = ActualNode.Pins[PinIndex];
                const FString PinLabel = FString::Printf(
                    TEXT("node %d pin %d"), NodeIndex, PinIndex);
                Test.TestEqual(*FString::Printf(TEXT("rollback restores %s name"), *PinLabel),
                    ActualPin.Name, ExpectedPin.Name);
                Test.TestEqual(*FString::Printf(TEXT("rollback restores %s direction"), *PinLabel),
                    static_cast<int32>(ActualPin.Direction),
                    static_cast<int32>(ExpectedPin.Direction));
                Test.TestTrue(*FString::Printf(TEXT("rollback restores %s type"), *PinLabel),
                    ActualPin.Type == ExpectedPin.Type);
                Test.TestEqual(
                    *FString::Printf(TEXT("rollback restores %s link count"), *PinLabel),
                    ActualPin.LinkedEndpoints.Num(),
                    ExpectedPin.LinkedEndpoints.Num());
                const int32 LinkCount = FMath::Min(
                    ActualPin.LinkedEndpoints.Num(), ExpectedPin.LinkedEndpoints.Num());
                for (int32 LinkIndex = 0; LinkIndex < LinkCount; ++LinkIndex)
                {
                    Test.TestEqual(
                        *FString::Printf(
                            TEXT("rollback restores %s link %d"), *PinLabel, LinkIndex),
                        ActualPin.LinkedEndpoints[LinkIndex],
                        ExpectedPin.LinkedEndpoints[LinkIndex]);
                }
            }
        }
    }

    bool CreateInterfaceAsset(FAutomationTestBase& Test, const FString& InterfacePath)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("name"), FPackageName::GetLongPackageAssetName(InterfacePath));
        Payload->SetStringField(TEXT("savePath"), FPackageName::GetLongPackagePath(InterfacePath));
        Payload->SetStringField(TEXT("blueprintType"), TEXT("interface"));

        FTestResponseCapture Capture;
        Test.TestTrue(TEXT("blueprint.create interface handler found"),
            InvokeHandlerWithCapture(TEXT("blueprint.create"), Payload, Capture));
        if (!Test.TestTrue(TEXT("interface creation succeeded"), Capture.bSuccess))
        {
            Test.AddError(FString::Printf(
                TEXT("interface create failed: %s %s"),
                *Capture.ErrorCode,
                *Capture.Message));
            return false;
        }
        return true;
    }

    bool AddInterfaceGetter(
        FAutomationTestBase& Test,
        const FString& InterfacePath,
        const TCHAR* FunctionName,
        const TCHAR* OutputName)
    {
        TSharedPtr<FJsonObject> Output = MakeShared<FJsonObject>();
        Output->SetStringField(TEXT("name"), OutputName);
        Output->SetStringField(TEXT("type"), TEXT("float"));

        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("path"), InterfacePath);
        Payload->SetStringField(TEXT("functionName"), FunctionName);
        TArray<TSharedPtr<FJsonValue>> Outputs;
        Outputs.Add(MakeShared<FJsonValueObject>(Output));
        Payload->SetArrayField(TEXT("outputs"), Outputs);

        FTestResponseCapture Capture;
        Test.TestTrue(TEXT("blueprint.add_function interface handler found"),
            InvokeHandlerWithCapture(TEXT("blueprint.add_function"), Payload, Capture));
        if (!Test.TestTrue(
            *FString::Printf(TEXT("interface getter %s creation succeeded"), FunctionName),
            Capture.bSuccess))
        {
            Test.AddError(FString::Printf(
                TEXT("interface getter create failed: %s %s"),
                *Capture.ErrorCode,
                *Capture.Message));
            return false;
        }
        return true;
    }

    bool GraphContainsNode(const UEdGraph* Graph, const UEdGraphNode* Node)
    {
        return Graph && Node && Graph->Nodes.Contains(Node);
    }

    UEdGraphPin* FindNamedFloatResultPin(UEdGraph* Graph, const FString& OutputName)
    {
        if (!Graph)
        {
            return nullptr;
        }
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            UK2Node_FunctionResult* Result = Cast<UK2Node_FunctionResult>(Node);
            if (!Result)
            {
                continue;
            }
            UEdGraphPin* Pin = Result->FindPin(FName(*OutputName), EGPD_Input);
            if (Pin
                && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Real
                && Pin->PinType.PinSubCategory == UEdGraphSchema_K2::PC_Float)
            {
                return Pin;
            }
        }
        return nullptr;
    }

    bool ImplementInterface(
        FAutomationTestBase& Test,
        UBlueprint* TargetBlueprint,
        UClass* InterfaceClass,
        const TCHAR* Label)
    {
        if (!TargetBlueprint || !InterfaceClass)
        {
            return false;
        }
        const bool bImplemented = FBlueprintEditorUtils::ImplementNewInterface(
            TargetBlueprint,
            InterfaceClass->GetClassPathName());
        Test.TestTrue(*FString::Printf(TEXT("%s implements interface"), Label), bImplemented);
        if (bImplemented)
        {
            FKismetEditorUtilities::CompileBlueprint(TargetBlueprint);
        }
        return bImplemented;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirInterfaceNamedOutputsOverrideTest,
    "PinWright.bpir.compiler.integration.InterfaceNamedOutputsOverride",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBpirInterfaceNamedOutputsOverrideTest::RunTest(const FString& Parameters)
{
    using namespace BpirInterfaceOutputOverrideTestUtils;

    const FString InterfacePath = MakeInterfaceAssetPath();
    ON_SCOPE_EXIT
    {
        CleanupTestAsset(InterfacePath);
    };

    UBpirLayoutSettings* LayoutSettings = GetMutableDefault<UBpirLayoutSettings>();
    if (!TestNotNull(TEXT("BPIR layout settings exist"), LayoutSettings))
    {
        return true;
    }
    const bool bOriginalLayoutEnabled = LayoutSettings->bEnableBpirLayoutPass;
    const int32 OriginalGridSize = LayoutSettings->InternalGridPx;
    LayoutSettings->bEnableBpirLayoutPass = true;
    LayoutSettings->InternalGridPx = 8;
    ON_SCOPE_EXIT
    {
        LayoutSettings->bEnableBpirLayoutPass = bOriginalLayoutEnabled;
        LayoutSettings->InternalGridPx = OriginalGridSize;
    };

    if (!CreateInterfaceAsset(*this, InterfacePath)
        || !AddInterfaceGetter(*this, InterfacePath, TEXT("GetAutoValue"), TEXT("AutoValue"))
        || !AddInterfaceGetter(*this, InterfacePath, TEXT("GetExplicitValue"), TEXT("ExplicitValue")))
    {
        return true;
    }

    UBlueprint* InterfaceBlueprint = Cast<UBlueprint>(
        UEditorAssetLibrary::LoadAsset(ToObjectPath(InterfacePath)));
    if (!TestNotNull(TEXT("interface Blueprint loads"), InterfaceBlueprint)
        || !TestNotNull(TEXT("interface generated class exists"),
            InterfaceBlueprint ? InterfaceBlueprint->GeneratedClass.Get() : nullptr))
    {
        return true;
    }

    UBlueprint* AutoTarget = CompilerTestUtils::CreateTransientTestBP(
        TEXT("BpirInterfaceAutoOverride"));
    UBlueprint* ExplicitTarget = CompilerTestUtils::CreateTransientTestBP(
        TEXT("BpirInterfaceExplicitOverride"));
    if (!TestNotNull(TEXT("auto-override target created"), AutoTarget)
        || !TestNotNull(TEXT("explicit-override target created"), ExplicitTarget)
        || !ImplementInterface(
            *this,
            AutoTarget,
            InterfaceBlueprint->GeneratedClass,
            TEXT("auto-override target"))
        || !ImplementInterface(
            *this,
            ExplicitTarget,
            InterfaceBlueprint->GeneratedClass,
            TEXT("explicit-override target")))
    {
        return true;
    }

    UEdGraph* AutoGraph = FindImplementedInterfaceGraphByName(AutoTarget, TEXT("GetAutoValue"));
    UEdGraph* AutoSecondGraph = FindImplementedInterfaceGraphByName(
        AutoTarget,
        TEXT("GetExplicitValue"));
    UEdGraph* ExplicitGraph = FindImplementedInterfaceGraphByName(
        ExplicitTarget,
        TEXT("GetExplicitValue"));
    if (!TestNotNull(TEXT("auto-override interface graph exists"), AutoGraph)
        || !TestNotNull(TEXT("second auto-override interface graph exists"), AutoSecondGraph)
        || !TestNotNull(TEXT("explicit-override interface graph exists"), ExplicitGraph))
    {
        return true;
    }

    UK2Node_CallFunction* AutoSentinel = CompilerTestUtils::SpawnPrintStringCall(AutoGraph, 500, 0);
    UK2Node_CallFunction* AutoSecondSentinel = CompilerTestUtils::SpawnPrintStringCall(AutoSecondGraph, 500, 0);
    UK2Node_CallFunction* ExplicitSentinel = CompilerTestUtils::SpawnPrintStringCall(ExplicitGraph, 500, 0);
    TestNotNull(TEXT("auto graph body sentinel created"), AutoSentinel);
    TestNotNull(TEXT("second auto graph body sentinel created"), AutoSecondSentinel);
    TestNotNull(TEXT("explicit graph body sentinel created"), ExplicitSentinel);

    TArray<UK2Node_FunctionEntry*> AutoEntryNodes;
    AutoGraph->GetNodesOfClass(AutoEntryNodes);
    TestEqual(TEXT("first auto graph has one function entry"), AutoEntryNodes.Num(), 1);
    if (AutoEntryNodes.Num() != 1)
    {
        return true;
    }
    AutoEntryNodes[0]->NodePosX = 13;
    AutoEntryNodes[0]->NodePosY = 17;

    FBpirCompiler AutoCompiler(AutoTarget);
    const FCompileResult AutoResult = AutoCompiler.Compile(
        TEXT("entry function GetAutoValue() -> (float AutoValue) {\n")
        TEXT("    call PrintString(InString: \"layout-first\")\n")
        TEXT("    return (AutoValue: 0.25)\n")
        TEXT("}\n\n")
        TEXT("entry function GetExplicitValue() -> (float ExplicitValue) {\n")
        TEXT("    return (ExplicitValue: 0.5)\n")
        TEXT("}"),
        EBpirCompileMode::Replace);
    if (!AutoResult.bSuccess)
    {
        for (const FCompileError& Error : AutoResult.Errors)
        {
            AddError(FString::Printf(TEXT("auto override L%d: %s"), Error.Line, *Error.Message));
        }
    }
    TestTrue(TEXT("entry function auto-override compiles"), AutoResult.bSuccess);

    FBpirCompiler ExplicitCompiler(ExplicitTarget);
    const FCompileResult ExplicitResult = ExplicitCompiler.Compile(
        TEXT("entry override GetExplicitValue() -> (float ExplicitValue) {\n")
        TEXT("    return (ExplicitValue: 0.75)\n")
        TEXT("}"),
        EBpirCompileMode::Replace);
    if (!ExplicitResult.bSuccess)
    {
        for (const FCompileError& Error : ExplicitResult.Errors)
        {
            AddError(FString::Printf(TEXT("explicit override L%d: %s"), Error.Line, *Error.Message));
        }
    }
    TestTrue(TEXT("entry override compiles"), ExplicitResult.bSuccess);

    TestTrue(TEXT("auto-override reuses the interface-owned graph"),
        FindImplementedInterfaceGraphByName(AutoTarget, TEXT("GetAutoValue")) == AutoGraph);
    TestTrue(TEXT("second auto-override reuses the interface-owned graph"),
        FindImplementedInterfaceGraphByName(AutoTarget, TEXT("GetExplicitValue")) == AutoSecondGraph);
    TestTrue(TEXT("explicit override reuses the interface-owned graph"),
        FindImplementedInterfaceGraphByName(ExplicitTarget, TEXT("GetExplicitValue")) == ExplicitGraph);
    TestFalse(TEXT("replace removes only the old auto body node"),
        GraphContainsNode(AutoGraph, AutoSentinel));
    TestFalse(TEXT("replace removes only the old second auto body node"),
        GraphContainsNode(AutoSecondGraph, AutoSecondSentinel));
    TestFalse(TEXT("replace removes only the old explicit body node"),
        GraphContainsNode(ExplicitGraph, ExplicitSentinel));
    TestFalse(TEXT("auto-override creates no ordinary duplicate graph"),
        HasOrdinaryFunctionGraphByName(AutoTarget, TEXT("GetAutoValue")));
    TestFalse(TEXT("second auto-override creates no ordinary duplicate graph"),
        HasOrdinaryFunctionGraphByName(AutoTarget, TEXT("GetExplicitValue")));
    TestFalse(TEXT("explicit override creates no ordinary duplicate graph"),
        HasOrdinaryFunctionGraphByName(ExplicitTarget, TEXT("GetExplicitValue")));
    TestFalse(TEXT("auto-override creates no event"),
        HasOverrideEventByName(AutoTarget, TEXT("GetAutoValue")));
    TestFalse(TEXT("second auto-override creates no event"),
        HasOverrideEventByName(AutoTarget, TEXT("GetExplicitValue")));
    TestFalse(TEXT("explicit override creates no event"),
        HasOverrideEventByName(ExplicitTarget, TEXT("GetExplicitValue")));
    TestNotNull(TEXT("auto named float output survives"),
        FindNamedFloatResultPin(AutoGraph, TEXT("AutoValue")));
    TestNotNull(TEXT("second auto named float output survives"),
        FindNamedFloatResultPin(AutoSecondGraph, TEXT("ExplicitValue")));
    TestNotNull(TEXT("explicit named float output survives"),
        FindNamedFloatResultPin(ExplicitGraph, TEXT("ExplicitValue")));

    UK2Node_CallFunction* FirstAutoBodyCall = nullptr;
    for (UEdGraphNode* Node : AutoGraph->Nodes)
    {
        UK2Node_CallFunction* CallNode = Cast<UK2Node_CallFunction>(Node);
        if (CallNode && CallNode->FunctionReference.GetMemberName() == FName(TEXT("PrintString")))
        {
            FirstAutoBodyCall = CallNode;
            break;
        }
    }
    if (TestNotNull(TEXT("first of two interface graphs has its authored body call"), FirstAutoBodyCall))
    {
        TestEqual(TEXT("first interface graph body call is laid out on the X grid"),
            FirstAutoBodyCall->NodePosX % LayoutSettings->InternalGridPx,
            0);
        TestEqual(TEXT("first interface graph body call is laid out on the Y grid"),
            FirstAutoBodyCall->NodePosY % LayoutSettings->InternalGridPx,
            0);
    }

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirInterfaceGraphFinalCompileRollbackTest,
    "PinWright.blueprint.compile_bpir.InterfaceGraphRollbackAfterFinalCompileFailure",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBpirInterfaceGraphFinalCompileRollbackTest::RunTest(const FString& Parameters)
{
    using namespace BpirInterfaceOutputOverrideTestUtils;

    const FString InterfacePath = MakeInterfaceAssetPath();
    const FString TargetPath = MakeTargetAssetPath();
    ON_SCOPE_EXIT
    {
        CleanupTestAsset(TargetPath);
        CleanupTestAsset(InterfacePath);
    };

    if (!CreateInterfaceAsset(*this, InterfacePath)
        || !AddInterfaceGetter(
            *this, InterfacePath, TEXT("GetRollbackValue"), TEXT("RollbackValue")))
    {
        return true;
    }

    UBlueprint* InterfaceBlueprint = Cast<UBlueprint>(
        UEditorAssetLibrary::LoadAsset(ToObjectPath(InterfacePath)));
    UBlueprint* TargetBlueprint = CreateActorBlueprint(TargetPath);
    if (!TestNotNull(TEXT("rollback interface Blueprint loads"), InterfaceBlueprint)
        || !TestNotNull(TEXT("rollback interface generated class exists"),
            InterfaceBlueprint ? InterfaceBlueprint->GeneratedClass.Get() : nullptr)
        || !TestNotNull(TEXT("rollback target Blueprint created"), TargetBlueprint)
        || !ImplementInterface(
            *this,
            TargetBlueprint,
            InterfaceBlueprint->GeneratedClass,
            TEXT("rollback target")))
    {
        return true;
    }

    UEdGraph* InterfaceGraph = FindImplementedInterfaceGraphByName(
        TargetBlueprint, TEXT("GetRollbackValue"));
    if (!TestNotNull(TEXT("rollback interface-owned graph exists"), InterfaceGraph))
    {
        return true;
    }

    TargetBlueprint->SetFlags(RF_Transactional);
    InterfaceGraph->SetFlags(RF_Transactional);
    UK2Node_CallFunction* Sentinel = CompilerTestUtils::SpawnPrintStringCall(
        InterfaceGraph, 480, 96);
    if (!TestNotNull(TEXT("rollback graph sentinel created"), Sentinel))
    {
        return true;
    }
    Sentinel->SetFlags(RF_Transactional);

    UEdGraph* EventGraph = FBlueprintEditorUtils::FindEventGraph(TargetBlueprint);
    if (!TestNotNull(TEXT("rollback target event graph exists"), EventGraph))
    {
        return true;
    }
    EventGraph->SetFlags(RF_Transactional);
    UK2Node_Event* BeginPlay = BpirGraphTestHelpers::EnsureBeginPlayNode(EventGraph);
    UK2Node_CallFunction* BadCall = CompilerTestUtils::SpawnPrintStringCall(EventGraph, 320, 0);
    if (!TestNotNull(TEXT("rollback fixture BeginPlay exists"), BeginPlay)
        || !TestNotNull(TEXT("rollback fixture bad call exists"), BadCall))
    {
        return true;
    }
    BeginPlay->SetFlags(RF_Transactional);
    BadCall->SetFlags(RF_Transactional);
    BpirGraphTestHelpers::WireExec(BeginPlay, BadCall);
    BadCall->FunctionReference.SetExternalMember(
        FName(TEXT("PW_NoSuchFunction_InterfaceRollback_ZZZ")), AActor::StaticClass());

    const BlueprintHandlerUtils::FBlueprintCompileDiagnostics BaselineDiagnostics =
        BlueprintHandlerUtils::CompileBlueprintWithDiagnostics(TargetBlueprint);
    TestFalse(TEXT("rollback fixture has a deterministic final compile failure"),
        BaselineDiagnostics.bCompiled);
    TestTrue(TEXT("rollback fixture reports at least one compile error"),
        BaselineDiagnostics.Errors.Num() > 0);

    const int32 NodeCountBefore = InterfaceGraph->Nodes.Num();
    const FGraphStructureSnapshot GraphStructureBefore = CaptureGraphStructure(InterfaceGraph);
    const FGuid SentinelGuid = Sentinel->NodeGuid;

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), TargetPath);
    Payload->SetStringField(TEXT("mode"), TEXT("replace"));
    Payload->SetStringField(
        TEXT("code"),
        TEXT("entry function GetRollbackValue() -> (float RollbackValue) {\n")
        TEXT("    call PrintString(InString: \"must roll back\")\n")
        TEXT("    return (RollbackValue: 0.75)\n")
        TEXT("}\n"));

    FTestResponseCapture Capture;
    TestTrue(TEXT("rollback compile_bpir handler found"),
        InvokeHandlerWithCapture(TEXT("blueprint.compile_bpir"), Payload, Capture));
    TestTrue(TEXT("rollback compile_bpir responded"), Capture.bWasCalled);
    TestFalse(TEXT("final Blueprint compile failure returns an error"), Capture.bSuccess);
    TestEqual(TEXT("failure reaches the final Blueprint compile path"),
        Capture.ErrorCode, FString(TEXT("BLUEPRINT_COMPILE_FAILED")));

    UEdGraph* RestoredGraph = FindImplementedInterfaceGraphByName(
        TargetBlueprint, TEXT("GetRollbackValue"));
    if (!TestNotNull(TEXT("interface graph survives rollback"), RestoredGraph))
    {
        return true;
    }
    TestTrue(TEXT("rollback preserves interface graph identity"), RestoredGraph == InterfaceGraph);
    TestEqual(TEXT("rollback restores the exact interface graph node count"),
        RestoredGraph->Nodes.Num(), NodeCountBefore);
    TestGraphStructureEqual(
        *this, GraphStructureBefore, CaptureGraphStructure(RestoredGraph));
    TestNotNull(TEXT("rollback restores the original body sentinel"),
        FBlueprintEditorUtils::GetNodeByGUID(TargetBlueprint, SentinelGuid));

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirInterfaceGraphDecompileRoundTripTest,
    "PinWright.bpir.decompiler.InterfaceGraphAggregateAndNamedRoundTrip",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBpirInterfaceGraphDecompileRoundTripTest::RunTest(const FString& Parameters)
{
    using namespace BpirInterfaceOutputOverrideTestUtils;

    const FString InterfacePath = MakeInterfaceAssetPath();
    const FString SourcePath = MakeTargetAssetPath();
    const FString AggregateTargetPath = MakeTargetAssetPath();
    const FString NamedTargetPath = MakeTargetAssetPath();
    ON_SCOPE_EXIT
    {
        CleanupTestAsset(NamedTargetPath);
        CleanupTestAsset(AggregateTargetPath);
        CleanupTestAsset(SourcePath);
        CleanupTestAsset(InterfacePath);
    };

    if (!CreateInterfaceAsset(*this, InterfacePath)
        || !AddInterfaceGetter(
            *this, InterfacePath, TEXT("GetRoundTripValue"), TEXT("RoundTripValue")))
    {
        return true;
    }

    UBlueprint* InterfaceBlueprint = Cast<UBlueprint>(
        UEditorAssetLibrary::LoadAsset(ToObjectPath(InterfacePath)));
    UBlueprint* SourceBlueprint = CreateActorBlueprint(SourcePath);
    UBlueprint* AggregateTarget = CreateActorBlueprint(AggregateTargetPath);
    UBlueprint* NamedTarget = CreateActorBlueprint(NamedTargetPath);
    if (!TestNotNull(TEXT("round-trip interface Blueprint loads"), InterfaceBlueprint)
        || !TestNotNull(TEXT("round-trip interface generated class exists"),
            InterfaceBlueprint ? InterfaceBlueprint->GeneratedClass.Get() : nullptr)
        || !TestNotNull(TEXT("source Blueprint created"), SourceBlueprint)
        || !TestNotNull(TEXT("aggregate target Blueprint created"), AggregateTarget)
        || !TestNotNull(TEXT("named target Blueprint created"), NamedTarget)
        || !ImplementInterface(
            *this,
            SourceBlueprint,
            InterfaceBlueprint->GeneratedClass,
            TEXT("source Blueprint"))
        || !ImplementInterface(
            *this,
            AggregateTarget,
            InterfaceBlueprint->GeneratedClass,
            TEXT("aggregate target Blueprint"))
        || !ImplementInterface(
            *this,
            NamedTarget,
            InterfaceBlueprint->GeneratedClass,
            TEXT("named target Blueprint")))
    {
        return true;
    }

    auto CompileThroughHandler = [this](
        const FString& TargetPath,
        const FString& Code,
        const TCHAR* Label) -> bool
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("assetPath"), TargetPath);
        Payload->SetStringField(TEXT("mode"), TEXT("replace"));
        Payload->SetStringField(TEXT("code"), Code);

        FTestResponseCapture Capture;
        if (!TestTrue(
                *FString::Printf(TEXT("%s compile_bpir handler found"), Label),
                InvokeHandlerWithCapture(TEXT("blueprint.compile_bpir"), Payload, Capture)))
        {
            return false;
        }
        if (!TestTrue(
                *FString::Printf(TEXT("%s BPIR compiles"), Label),
                Capture.bSuccess))
        {
            AddError(FString::Printf(
                TEXT("%s compile error: %s %s"),
                Label,
                *Capture.ErrorCode,
                *Capture.Message));
            return false;
        }
        return true;
    };

    const FString SourceCode =
        TEXT("entry function GetRoundTripValue() -> (float RoundTripValue) {\n")
        TEXT("    call PrintString(InString: \"interface-body-sentinel\")\n")
        TEXT("    return (RoundTripValue: 0.75)\n")
        TEXT("}");
    if (!CompileThroughHandler(SourcePath, SourceCode, TEXT("source")))
    {
        return true;
    }

    auto DecompileThroughHandler = [this, &SourcePath](
        const FString& GraphName,
        FString& OutBpir,
        const TCHAR* Label) -> bool
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("assetPath"), SourcePath);
        if (!GraphName.IsEmpty())
        {
            Payload->SetStringField(TEXT("graphName"), GraphName);
        }

        FTestResponseCapture Capture;
        if (!TestTrue(
                *FString::Printf(TEXT("%s decompile handler found"), Label),
                InvokeHandlerWithCapture(TEXT("blueprint.decompile"), Payload, Capture)))
        {
            return false;
        }
        if (!TestTrue(
                *FString::Printf(TEXT("%s decompile succeeds"), Label),
                Capture.bSuccess))
        {
            AddError(FString::Printf(
                TEXT("%s decompile error: %s %s"),
                Label,
                *Capture.ErrorCode,
                *Capture.Message));
            return false;
        }
        if (!TestTrue(
                *FString::Printf(TEXT("%s response payload exists"), Label),
                Capture.Result.IsValid()))
        {
            return false;
        }
        return TestTrue(
            *FString::Printf(TEXT("%s response carries BPIR"), Label),
            Capture.Result->TryGetStringField(TEXT("bpir"), OutBpir));
    };

    FString AggregateBpir;
    FString NamedBpir;
    if (!DecompileThroughHandler(FString(), AggregateBpir, TEXT("aggregate"))
        || !DecompileThroughHandler(
            TEXT("GetRoundTripValue"), NamedBpir, TEXT("named interface graph")))
    {
        return true;
    }

    TSharedPtr<FJsonObject> FunctionPayload = MakeShared<FJsonObject>();
    FunctionPayload->SetStringField(TEXT("assetPath"), SourcePath);
    FunctionPayload->SetStringField(TEXT("functionName"), TEXT("GetRoundTripValue"));
    FTestResponseCapture FunctionCapture;
    TestTrue(TEXT("decompile_function handler finds the interface graph"),
        InvokeHandlerWithCapture(
            TEXT("blueprint.decompile_function"), FunctionPayload, FunctionCapture));
    TestTrue(TEXT("decompile_function succeeds for the interface graph"),
        FunctionCapture.bSuccess);
    FString FunctionBpir;
    if (TestTrue(TEXT("decompile_function response payload exists"),
            FunctionCapture.Result.IsValid()))
    {
        TestTrue(TEXT("decompile_function response carries BPIR"),
            FunctionCapture.Result->TryGetStringField(TEXT("bpir"), FunctionBpir));
    }

    const FString InterfaceEntry = TEXT("entry override GetRoundTripValue()");
    const int32 AggregateFirstEntry = AggregateBpir.Find(InterfaceEntry);
    const int32 AggregateLastEntry = AggregateBpir.Find(
        InterfaceEntry,
        ESearchCase::CaseSensitive,
        ESearchDir::FromEnd);
    TestTrue(TEXT("aggregate output includes the interface override entry"),
        AggregateFirstEntry != INDEX_NONE);
    TestEqual(TEXT("aggregate output emits the interface graph once after deduplication"),
        AggregateLastEntry, AggregateFirstEntry);
    TestTrue(TEXT("aggregate output includes the interface body"),
        AggregateBpir.Contains(TEXT("interface-body-sentinel")));
    TestTrue(TEXT("named output uses the interface override entry kind"),
        NamedBpir.Contains(InterfaceEntry));
    TestTrue(TEXT("named output includes the interface body"),
        NamedBpir.Contains(TEXT("interface-body-sentinel")));
    TestEqual(TEXT("decompile_function matches named graph output"),
        FunctionBpir, NamedBpir);

    if (!CompileThroughHandler(
            AggregateTargetPath, AggregateBpir, TEXT("aggregate round-trip"))
        || !CompileThroughHandler(NamedTargetPath, NamedBpir, TEXT("named round-trip")))
    {
        return true;
    }

    UEdGraph* AggregateGraph = FindImplementedInterfaceGraphByName(
        AggregateTarget, TEXT("GetRoundTripValue"));
    UEdGraph* NamedGraph = FindImplementedInterfaceGraphByName(
        NamedTarget, TEXT("GetRoundTripValue"));
    if (!TestNotNull(TEXT("aggregate round-trip keeps the interface-owned graph"), AggregateGraph)
        || !TestNotNull(TEXT("named round-trip keeps the interface-owned graph"), NamedGraph))
    {
        return true;
    }

    auto HasPrintStringCall = [](const UEdGraph* Graph) -> bool
    {
        if (!Graph)
        {
            return false;
        }
        for (const UEdGraphNode* Node : Graph->Nodes)
        {
            const UK2Node_CallFunction* CallNode = Cast<UK2Node_CallFunction>(Node);
            if (CallNode
                && CallNode->FunctionReference.GetMemberName() == FName(TEXT("PrintString")))
            {
                return true;
            }
        }
        return false;
    };

    TestTrue(TEXT("aggregate round-trip restores the interface body"),
        HasPrintStringCall(AggregateGraph));
    TestTrue(TEXT("named round-trip restores the interface body"),
        HasPrintStringCall(NamedGraph));
    TestNotNull(TEXT("aggregate round-trip preserves the named output"),
        FindNamedFloatResultPin(AggregateGraph, TEXT("RoundTripValue")));
    TestNotNull(TEXT("named round-trip preserves the named output"),
        FindNamedFloatResultPin(NamedGraph, TEXT("RoundTripValue")));
    TestFalse(TEXT("aggregate round-trip creates no ordinary duplicate graph"),
        HasOrdinaryFunctionGraphByName(AggregateTarget, TEXT("GetRoundTripValue")));
    TestFalse(TEXT("named round-trip creates no ordinary duplicate graph"),
        HasOrdinaryFunctionGraphByName(NamedTarget, TEXT("GetRoundTripValue")));

    return true;
}
