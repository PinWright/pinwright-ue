// Copyright (c) 2026 Alexander Penkin. MIT License.

#if defined(__has_include) && __has_include("PCGGraph.h")

#include "Misc/AutomationTest.h"

#include "Compat/EngineVersionCompat.h"
#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "EditorAssetLibrary.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"
#include "Tests/Infra/ParamSpecTestHelpers.h"
#include "Tests/TestSkipReporting.h"
#include "Tests/TestUtils.h"
#include "UObject/Class.h"
#include "UObject/UObjectIterator.h"

#include "PCGGraph.h"
#include "PCGEdge.h"
#include "PCGNode.h"
#include "PCGPin.h"
#include "PCGSettings.h"

namespace PinWrightPCGTests
{
    FString MakeUniqueAssetFolder()
    {
        return TEXT("/Game/__PW_GatewayTests");
    }

    FString MakeUniqueAssetName()
    {
        return FString::Printf(TEXT("PCG_%s"),
            *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    }

    UClass* FindCreatePointsSettingsClass()
    {
        if (UClass* Specific = FindObject<UClass>(nullptr, TEXT("/Script/PCG.PCGCreatePointsSettings")))
        {
            return Specific;
        }
        return LoadClass<UPCGSettings>(nullptr, TEXT("/Script/PCG.PCGCreatePointsSettings"));
    }

    UClass* FindAnyPCGSettingsClass()
    {
        // Prefer a well-known concrete settings class.
        if (UClass* Specific = FindCreatePointsSettingsClass())
        {
            return Specific;
        }
        // Fall back to scanning loaded classes for any concrete UPCGSettings-derived class.
        for (TObjectIterator<UClass> It; It; ++It)
        {
            UClass* Cls = *It;
            if (Cls
                && Cls != UPCGSettings::StaticClass()
                && Cls->IsChildOf(UPCGSettings::StaticClass())
                && !Cls->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists))
            {
                return Cls;
            }
        }
        return nullptr;
    }

    const UPCGPin* FindPinByLabel(const TArray<TObjectPtr<UPCGPin>>& Pins, const FName& Label)
    {
        for (const UPCGPin* Pin : Pins)
        {
            if (Pin && Pin->Properties.Label == Label)
            {
                return Pin;
            }
        }
        return nullptr;
    }

    bool HasEdgeBetweenPins(const UPCGPin* FromPin, const UPCGPin* ToPin)
    {
        if (!FromPin || !ToPin)
        {
            return false;
        }

        for (const UPCGEdge* Edge : FromPin->Edges)
        {
            if (Edge && Edge->GetOtherPin(FromPin) == ToPin)
            {
                return true;
            }
        }
        return false;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPCGCreateGraphCreatesAssetTest,
    "PinWright.pcg.create_graph.CreatesAsset",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPCGCreateGraphCreatesAssetTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightPCGTests;

    const FString Folder = MakeUniqueAssetFolder();
    const FString Name = MakeUniqueAssetName();
    const FString FullPackagePath = FString::Printf(TEXT("%s/%s"), *Folder, *Name);

    ON_SCOPE_EXIT { CleanupTestAsset(FullPackagePath); };

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("name"), Name);
    Payload->SetStringField(TEXT("savePath"), Folder);

    FTestResponseCapture Capture;
    TestTrue(TEXT("handler registered"),
        InvokeHandlerWithCapture(TEXT("pcg.create_graph"), Payload, Capture));
    TestTrue(TEXT("responded"), Capture.bWasCalled);
    if (!TestTrue(TEXT("succeeded"), Capture.bSuccess))
    {
        AddError(FString::Printf(TEXT("create_graph error: %s %s"),
            *Capture.ErrorCode, *Capture.Message));
        return true;
    }

    TestTrue(TEXT("asset exists in registry"),
        UEditorAssetLibrary::DoesAssetExist(FullPackagePath));

    bool bExistsOnDisk = false;
    TestTrue(TEXT("response reports the graph on disk"),
        Capture.Result.IsValid()
        && Capture.Result->TryGetBoolField(TEXT("existsOnDisk"), bExistsOnDisk));
    TestTrue(TEXT("created graph is durable after the response"), bExistsOnDisk);

    bool bSaved = false;
    TestTrue(TEXT("response reports a durable save"),
        Capture.Result.IsValid() && Capture.Result->TryGetBoolField(TEXT("saved"), bSaved));
    TestTrue(TEXT("created graph save is true"), bSaved);

    FString SaveState;
    TestTrue(TEXT("response reports the measured save state"),
        Capture.Result.IsValid() && Capture.Result->TryGetStringField(TEXT("saveState"), SaveState));
    TestTrue(TEXT("created graph save state is durable"),
        SaveState == TEXT("written") || SaveState == TEXT("alreadyCurrent"));

    double SizeBytes = 0.0;
    TestTrue(TEXT("response reports the measured disk size"),
        Capture.Result.IsValid() && Capture.Result->TryGetNumberField(TEXT("sizeBytes"), SizeBytes));
    TestTrue(TEXT("created graph reports a positive disk size"), SizeBytes > 0.0);
    TestFalse(TEXT("durable create does not report a pending flush"),
        Capture.Result.IsValid() && Capture.Result->HasField(TEXT("pendingFlush")));

    FString PackageFilename;
    TestTrue(TEXT("graph package resolves to a filename"),
        FPackageName::TryConvertLongPackageNameToFilename(
            FullPackagePath, PackageFilename, FPackageName::GetAssetPackageExtension()));
    if (!PackageFilename.IsEmpty())
    {
        TestTrue(TEXT("graph package file exists after create_graph returns"),
            IFileManager::Get().FileSize(*PackageFilename) > 0);
    }

    const FString ObjectPath = FString::Printf(TEXT("%s.%s"), *FullPackagePath, *Name);
    UPCGGraph* Loaded = LoadObject<UPCGGraph>(nullptr, *ObjectPath);
    TestNotNull(TEXT("loaded UPCGGraph"), Loaded);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPCGAddNodeIncrementsNodeCountTest,
    "PinWright.pcg.add_node.IncrementsCount",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPCGAddNodeIncrementsNodeCountTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightPCGTests;

    UClass* SettingsClass = FindAnyPCGSettingsClass();
    if (!SettingsClass)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("fixture-unavailable"),
            TEXT("No concrete UPCGSettings subclass is loaded on this host, so pcg.add_node "
                 "had no node type to add and the node-count assertions could not run."));
        return true;
    }

    UPCGGraph* Graph = NewObject<UPCGGraph>(GetTransientPackage(), UPCGGraph::StaticClass(),
        NAME_None, RF_Transient);
    TestNotNull(TEXT("transient graph created"), Graph);
    if (!Graph) return true;

    const int32 NodeCountBefore = Graph->GetNodes().Num();
    TestEqual(TEXT("graph starts empty (no user nodes)"), NodeCountBefore, 0);

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("graphPath"), Graph->GetPathName());
    Payload->SetStringField(TEXT("nodeClass"), SettingsClass->GetPathName());
    Payload->SetNumberField(TEXT("x"), 100);
    Payload->SetNumberField(TEXT("y"), 200);

    FTestResponseCapture Capture;
    TestTrue(TEXT("handler registered"),
        InvokeHandlerWithCapture(TEXT("pcg.add_node"), Payload, Capture));
    TestTrue(TEXT("responded"), Capture.bWasCalled);
    if (!TestTrue(TEXT("succeeded"), Capture.bSuccess))
    {
        AddError(FString::Printf(TEXT("add_node error: %s %s"),
            *Capture.ErrorCode, *Capture.Message));
        return true;
    }

    // Counterfactual: if AddNodeOfType is reverted to a no-op, this assertion fails
    // because the node was never appended to Graph->GetNodes().
    TestEqual(TEXT("node count increased by exactly 1"),
        Graph->GetNodes().Num(), NodeCountBefore + 1);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPCGConnectPinsResolvesImplicitOutputNodeTest,
    "PinWright.pcg.connect_pins.ResolvesImplicitOutputNode",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPCGConnectPinsResolvesImplicitOutputNodeTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightPCGTests;

    UClass* SettingsClass = FindCreatePointsSettingsClass();
    if (!SettingsClass)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("fixture-unavailable"),
            TEXT("UPCGCreatePointsSettings is not loaded on this host, so no source node "
                 "could be authored and the implicit-output-node resolution assertions "
                 "could not run."));
        return true;
    }

    UPCGGraph* Graph = NewObject<UPCGGraph>(GetTransientPackage(), UPCGGraph::StaticClass(),
        NAME_None, RF_Transient);
    TestNotNull(TEXT("transient graph created"), Graph);
    if (!Graph) return true;

    UPCGNode* OutputNode = Graph->GetOutputNode();
    TestNotNull(TEXT("implicit output node present"), OutputNode);
    if (!OutputNode) return true;

    UPCGSettings* DefaultSettings = nullptr;
    UPCGNode* FromNode = Graph->AddNodeOfType(TSubclassOf<UPCGSettings>(SettingsClass), DefaultSettings);
    TestNotNull(TEXT("source PCG node created"), FromNode);
    if (!FromNode) return true;

    const FName PinLabel(TEXT("Out"));
    const UPCGPin* FromPin = FindPinByLabel(FromNode->GetOutputPins(), PinLabel);
    const UPCGPin* ToPin = FindPinByLabel(OutputNode->GetInputPins(), PinLabel);
    TestNotNull(TEXT("source Out pin present"), FromPin);
    TestNotNull(TEXT("implicit output Out input pin present"), ToPin);
    if (!FromPin || !ToPin) return true;

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("graphPath"), Graph->GetPathName());
    Payload->SetStringField(TEXT("fromNode"), FromNode->GetName());
    Payload->SetStringField(TEXT("fromPin"), PinLabel.ToString());
    Payload->SetStringField(TEXT("toNode"), OutputNode->GetName());
    Payload->SetStringField(TEXT("toPin"), PinLabel.ToString());

    FTestResponseCapture Capture;
    TestTrue(TEXT("handler registered"),
        InvokeHandlerWithCapture(TEXT("pcg.connect_pins"), Payload, Capture));
    TestTrue(TEXT("responded"), Capture.bWasCalled);
    if (!TestTrue(TEXT("succeeded"), Capture.bSuccess))
    {
        AddError(FString::Printf(TEXT("connect_pins error: %s %s"),
            *Capture.ErrorCode, *Capture.Message));
        return true;
    }

    TestTrue(TEXT("edge from source node to implicit output node exists"),
        HasEdgeBetweenPins(FromPin, ToPin));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPCGInspectReportsEdgeDirectionTest,
    "PinWright.pcg.inspect.EdgeDirection",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPCGInspectReportsEdgeDirectionTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightPCGTests;

    UClass* SettingsClass = FindCreatePointsSettingsClass();
    if (!SettingsClass)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("fixture-unavailable"),
            TEXT("UPCGCreatePointsSettings is not loaded on this host, so no real edge "
                 "could be wired and the pcg.inspect edge-direction assertions could not "
                 "run."));
        return true;
    }

    UPCGGraph* Graph = NewObject<UPCGGraph>(GetTransientPackage(), UPCGGraph::StaticClass(),
        NAME_None, RF_Transient);
    TestNotNull(TEXT("transient graph created"), Graph);
    if (!Graph) return true;

    // Wire a real edge: source node's output pin -> implicit output node's
    // input pin. Data flows source-output -> dest-input, so pcg.inspect must
    // report from=source / to=output-node. If the handler's pin roles are
    // reverted (reading Edge->OutputPin as source), from/to invert and the
    // direction assertions below fail.
    UPCGNode* OutputNode = Graph->GetOutputNode();
    TestNotNull(TEXT("implicit output node present"), OutputNode);
    if (!OutputNode) return true;

    UPCGSettings* DefaultSettings = nullptr;
    UPCGNode* SourceNode = Graph->AddNodeOfType(TSubclassOf<UPCGSettings>(SettingsClass), DefaultSettings);
    TestNotNull(TEXT("source PCG node created"), SourceNode);
    if (!SourceNode) return true;

    const FName SourcePinLabel(TEXT("Out"));
    // The implicit output node (UPCGGraphInputOutputSettings, bIsInput=false) labels
    // its single default pin with PCGPinConstants::DefaultOutputLabel ("Out"), so its
    // input pin is labeled "Out" — not "In". (See PCGInputOutputSettings.cpp
    // GetStaticOutLabels; matches the sibling connect_pins test which also uses "Out".)
    const FName DestPinLabel(TEXT("Out"));
    UPCGPin* SourceOutPin = const_cast<UPCGPin*>(FindPinByLabel(SourceNode->GetOutputPins(), SourcePinLabel));
    UPCGPin* DestInPin = const_cast<UPCGPin*>(FindPinByLabel(OutputNode->GetInputPins(), DestPinLabel));
    TestNotNull(TEXT("source Out pin present"), SourceOutPin);
    TestNotNull(TEXT("dest input pin present"), DestInPin);
    if (!SourceOutPin || !DestInPin) return true;

    const bool bConnected = SourceOutPin->AddEdgeTo(DestInPin);
    TestTrue(TEXT("edge created on source output pin"), bConnected);

    const FString SourceName = SourceNode->GetName();
    const FString DestName = OutputNode->GetName();

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("graphPath"), Graph->GetPathName());

    FTestResponseCapture Capture;
    TestTrue(TEXT("handler registered"),
        InvokeHandlerWithCapture(TEXT("pcg.inspect"), Payload, Capture));
    TestTrue(TEXT("responded"), Capture.bWasCalled);
    if (!TestTrue(TEXT("succeeded"), Capture.bSuccess))
    {
        AddError(FString::Printf(TEXT("inspect error: %s %s"),
            *Capture.ErrorCode, *Capture.Message));
        return true;
    }

    TestNotNull(TEXT("result object present"), Capture.Result.Get());
    if (!Capture.Result.IsValid()) return true;

    const TArray<TSharedPtr<FJsonValue>>* Edges = nullptr;
    TestTrue(TEXT("edges array present"), Capture.Result->TryGetArrayField(TEXT("edges"), Edges));
    if (!Edges) return true;

    // Find the edge we authored (matching the source/dest node names) and
    // assert its reported direction matches data flow.
    bool bFoundEdge = false;
    for (const TSharedPtr<FJsonValue>& EdgeVal : *Edges)
    {
        const TSharedPtr<FJsonObject>* EdgeObj = nullptr;
        if (!EdgeVal.IsValid() || !EdgeVal->TryGetObject(EdgeObj) || !EdgeObj) continue;

        const FString From = (*EdgeObj)->GetStringField(TEXT("from"));
        const FString To = (*EdgeObj)->GetStringField(TEXT("to"));
        // Matching from=source/to=dest is itself the direction guard: a reverted
        // handler would report from=dest/to=source, so no edge would match here.
        if (From == SourceName && To == DestName)
        {
            bFoundEdge = true;
            TestEqual(TEXT("fromPin == source output pin label"),
                (*EdgeObj)->GetStringField(TEXT("fromPin")), SourcePinLabel.ToString());
            TestEqual(TEXT("toPin == dest input pin label"),
                (*EdgeObj)->GetStringField(TEXT("toPin")), DestPinLabel.ToString());
            break;
        }
    }
    TestTrue(TEXT("inspect reported edge source->dest in correct direction"), bFoundEdge);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPCGRemoveImmutableNodeRejectedTest,
    "PinWright.pcg.remove_node.ImmutableRejected",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPCGRemoveImmutableNodeRejectedTest::RunTest(const FString& Parameters)
{
    UPCGGraph* Graph = NewObject<UPCGGraph>(GetTransientPackage(), UPCGGraph::StaticClass(),
        NAME_None, RF_Transient);
    TestNotNull(TEXT("transient graph created"), Graph);
    if (!Graph) return true;

    UPCGNode* InputNode = Graph->GetInputNode();
    TestNotNull(TEXT("input node present"), InputNode);
    if (!InputNode) return true;

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("graphPath"), Graph->GetPathName());
    Payload->SetStringField(TEXT("nodeId"), InputNode->GetName());

    FTestResponseCapture Capture;
    TestTrue(TEXT("handler registered"),
        InvokeHandlerWithCapture(TEXT("pcg.remove_node"), Payload, Capture));
    TestTrue(TEXT("responded"), Capture.bWasCalled);
    TestFalse(TEXT("remove input node refused"), Capture.bSuccess);
    TestEqual(TEXT("error code == IMMUTABLE_NODE"),
        Capture.ErrorCode, FString(TEXT("IMMUTABLE_NODE")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPCGCreateGraphGraphClassSelectsSubclassTest,
    "PinWright.pcg.create_graph.GraphClassSelectsSubclass",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

// Counterfactual against the pre-graphClass handler, which hardcoded NewObject<UPCGGraph> and
// declared only name/savePath. Each of the first three blocks fails on that handler: the
// parameter is not accepted on the wire, the response carries no graphClass, and a graphClass
// naming a non-UPCGGraph class is silently ignored and an asset created anyway.
bool FPCGCreateGraphGraphClassSelectsSubclassTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightPCGTests;

    // 1. The dispatcher must accept the key. Tests/TestUtils.h's InvokeHandlerWithCapture calls
    //    the handler directly and never runs ValidateHandlerParams, so a body that reads the key
    //    proves nothing about whether a caller may supply it.
    TestTrue(TEXT("graphClass is accepted on the wire"),
        ParamSpecTestHelpers::IsParamAccepted(TEXT("pcg.create_graph"), TEXT("graphClass")));

    // 2. Omitting graphClass still yields a stock UPCGGraph, and the response says which class
    //    was built — a caller cannot otherwise tell a default apart from the subclass it asked for.
    {
        const FString Folder = MakeUniqueAssetFolder();
        const FString Name = MakeUniqueAssetName();
        const FString FullPackagePath = FString::Printf(TEXT("%s/%s"), *Folder, *Name);
        ON_SCOPE_EXIT { CleanupTestAsset(FullPackagePath); };

        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("name"), Name);
        Payload->SetStringField(TEXT("savePath"), Folder);

        FTestResponseCapture Capture;
        TestTrue(TEXT("handler registered"),
            InvokeHandlerWithCapture(TEXT("pcg.create_graph"), Payload, Capture));
        if (!TestTrue(TEXT("default create succeeded"), Capture.bSuccess))
        {
            AddError(FString::Printf(TEXT("create_graph error: %s %s"),
                *Capture.ErrorCode, *Capture.Message));
            return true;
        }

        FString EchoedClass;
        TestTrue(TEXT("response echoes graphClass"),
            Capture.Result.IsValid()
            && Capture.Result->TryGetStringField(TEXT("graphClass"), EchoedClass));
        TestEqual(TEXT("defaulted graphClass is UPCGGraph"),
            EchoedClass, UPCGGraph::StaticClass()->GetPathName());

        const FString ObjectPath = FString::Printf(TEXT("%s.%s"), *FullPackagePath, *Name);
        UPCGGraph* Loaded = LoadObject<UPCGGraph>(nullptr, *ObjectPath);
        if (TestNotNull(TEXT("default graph loaded"), Loaded))
        {
            TestEqual(TEXT("default graph is exactly UPCGGraph"),
                Loaded->GetClass()->GetPathName(), UPCGGraph::StaticClass()->GetPathName());
        }
    }

    // 3. A resolvable class that is not a UPCGGraph must be refused, and must leave no asset
    //    behind. The pre-change handler ignored the key and created a plain UPCGGraph here.
    {
        const FString Folder = MakeUniqueAssetFolder();
        const FString Name = MakeUniqueAssetName();
        const FString FullPackagePath = FString::Printf(TEXT("%s/%s"), *Folder, *Name);
        ON_SCOPE_EXIT { CleanupTestAsset(FullPackagePath); };

        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("name"), Name);
        Payload->SetStringField(TEXT("savePath"), Folder);
        Payload->SetStringField(TEXT("graphClass"), TEXT("/Script/Engine.Actor"));

        FTestResponseCapture Capture;
        TestTrue(TEXT("handler registered"),
            InvokeHandlerWithCapture(TEXT("pcg.create_graph"), Payload, Capture));
        TestFalse(TEXT("non-UPCGGraph graphClass refused"), Capture.bSuccess);
        TestEqual(TEXT("error code == CLASS_NOT_FOUND"),
            Capture.ErrorCode, FString(TEXT("CLASS_NOT_FOUND")));
        TestFalse(TEXT("refused create left no asset"),
            UEditorAssetLibrary::DoesAssetExist(FullPackagePath));
    }

    // 4. The point of the parameter: a non-default class produces a graph OF that class. The
    //    only stock UE 5.8 subclass is UProceduralVegetationGraph, which ships in an
    //    experimental plugin that is off by default, so the class is discovered by reflection
    //    and the assertion is skipped (visibly) on a host that has none.
    UClass* SubClass = nullptr;
    for (TObjectIterator<UClass> It; It; ++It)
    {
        UClass* Candidate = *It;
        if (Candidate
            && Candidate != UPCGGraph::StaticClass()
            && Candidate->IsChildOf(UPCGGraph::StaticClass())
            && !Candidate->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists))
        {
            SubClass = Candidate;
            break;
        }
    }

    if (!SubClass)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-upcggraph-subclass-loaded"),
            TEXT("No concrete UPCGGraph subclass is loaded on this host (UE 5.8 ships only "
                 "UProceduralVegetationGraph, whose plugin is disabled by default), so the "
                 "non-default-class assertion could not run."));
        return true;
    }

    const FString Folder = MakeUniqueAssetFolder();
    const FString Name = MakeUniqueAssetName();
    const FString FullPackagePath = FString::Printf(TEXT("%s/%s"), *Folder, *Name);
    ON_SCOPE_EXIT { CleanupTestAsset(FullPackagePath); };

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("name"), Name);
    Payload->SetStringField(TEXT("savePath"), Folder);
    Payload->SetStringField(TEXT("graphClass"), SubClass->GetPathName());

    FTestResponseCapture Capture;
    TestTrue(TEXT("handler registered"),
        InvokeHandlerWithCapture(TEXT("pcg.create_graph"), Payload, Capture));
    if (!TestTrue(TEXT("subclass create succeeded"), Capture.bSuccess))
    {
        AddError(FString::Printf(TEXT("create_graph(%s) error: %s %s"),
            *SubClass->GetPathName(), *Capture.ErrorCode, *Capture.Message));
        return true;
    }

    FString EchoedClass;
    TestTrue(TEXT("response echoes graphClass"),
        Capture.Result.IsValid()
        && Capture.Result->TryGetStringField(TEXT("graphClass"), EchoedClass));
    TestEqual(TEXT("echoed graphClass is the requested subclass"),
        EchoedClass, SubClass->GetPathName());

    const FString ObjectPath = FString::Printf(TEXT("%s.%s"), *FullPackagePath, *Name);
    UPCGGraph* Loaded = LoadObject<UPCGGraph>(nullptr, *ObjectPath);
    if (TestNotNull(TEXT("subclass graph loaded"), Loaded))
    {
        // The assertion the ticket exists for: the asset on disk is the requested class, not
        // a stock UPCGGraph that merely reported the right name.
        TestEqual(TEXT("created asset is of the requested subclass"),
            Loaded->GetClass()->GetPathName(), SubClass->GetPathName());
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPCGConnectPinsReportsReplacedEdgeTest,
    "PinWright.pcg.connect_pins.ReportsReplacedEdge",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

// Counterfactual against the pre-fix handler, which called UPCGGraph::AddEdge (discarding the
// "the To pin removed other edges" bool AddLabeledEdge returns) and wrote a literal
// connected:true. On that handler the response carries exactly five fields — connected plus
// four echoes of the request — so replacedExistingEdge and replacedEdges are simply absent and
// every assertion in the second-connect block below fails, while the graph has silently lost
// the first edge.
bool FPCGConnectPinsReportsReplacedEdgeTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightPCGTests;

    UClass* SettingsClass = FindCreatePointsSettingsClass();
    if (!SettingsClass)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("fixture-unavailable"),
            TEXT("UPCGCreatePointsSettings is not loaded on this host, so no two source nodes "
                 "could contend for one single-connection input pin and the replaced-edge "
                 "disclosure assertions could not run."));
        return true;
    }

    UPCGGraph* Graph = NewObject<UPCGGraph>(GetTransientPackage(), UPCGGraph::StaticClass(),
        NAME_None, RF_Transient);
    TestNotNull(TEXT("transient graph created"), Graph);
    if (!Graph) return true;

    UPCGNode* OutputNode = Graph->GetOutputNode();
    TestNotNull(TEXT("implicit output node present"), OutputNode);
    if (!OutputNode) return true;

    UPCGSettings* DefaultSettingsA = nullptr;
    UPCGNode* SourceA = Graph->AddNodeOfType(TSubclassOf<UPCGSettings>(SettingsClass), DefaultSettingsA);
    UPCGSettings* DefaultSettingsB = nullptr;
    UPCGNode* SourceB = Graph->AddNodeOfType(TSubclassOf<UPCGSettings>(SettingsClass), DefaultSettingsB);
    TestNotNull(TEXT("first source node created"), SourceA);
    TestNotNull(TEXT("second source node created"), SourceB);
    if (!SourceA || !SourceB) return true;

    // The implicit output node labels its single input pin "Out" (see the sibling
    // EdgeDirection test).
    const FName PinLabel(TEXT("Out"));
    UPCGPin* SourceAOutPin = const_cast<UPCGPin*>(FindPinByLabel(SourceA->GetOutputPins(), PinLabel));
    UPCGPin* SourceBOutPin = const_cast<UPCGPin*>(FindPinByLabel(SourceB->GetOutputPins(), PinLabel));
    UPCGPin* TargetInPin = const_cast<UPCGPin*>(FindPinByLabel(OutputNode->GetInputPins(), PinLabel));
    TestNotNull(TEXT("first source Out pin present"), SourceAOutPin);
    TestNotNull(TEXT("second source Out pin present"), SourceBOutPin);
    TestNotNull(TEXT("target input pin present"), TargetInPin);
    if (!SourceAOutPin || !SourceBOutPin || !TargetInPin) return true;

    // Make the target single-connection, which is the condition under test. This is the
    // default for every Procedural Vegetation node's In pin (UPVBaseSettings::InputPinProperties
    // calls SetAllowMultipleConnections(false)), but that plugin is experimental and off by
    // default, so the property is set directly here instead of depending on it being loaded.
    // Nothing in AddLabeledEdge's notification path calls UPCGNode::UpdatePins — that runs only
    // on a settings change — so the flag survives both connect calls below.
#if UE_VERSION_OLDER_THAN(5, 4, 0)
    // FPCGPinProperties::SetAllowMultipleConnections arrived in 5.4; on 5.3 the flag is a
    // public field.
    TargetInPin->Properties.bAllowMultipleConnections = false;
#else
    TargetInPin->Properties.SetAllowMultipleConnections(false);
#endif

    const FString SourceAName = SourceA->GetName();
    const FString SourceBName = SourceB->GetName();
    const FString TargetName = OutputNode->GetName();
    const FString PinName = PinLabel.ToString();

    auto ConnectPins = [&](const FString& FromNodeName, FTestResponseCapture& Capture) -> bool
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("graphPath"), Graph->GetPathName());
        Payload->SetStringField(TEXT("fromNode"), FromNodeName);
        Payload->SetStringField(TEXT("fromPin"), PinName);
        Payload->SetStringField(TEXT("toNode"), TargetName);
        Payload->SetStringField(TEXT("toPin"), PinName);
        return InvokeHandlerWithCapture(TEXT("pcg.connect_pins"), Payload, Capture);
    };

    // 1. First connect displaces nothing, and must say so rather than staying silent: a caller
    //    cannot read "no replacement" out of a response that never mentions replacement.
    {
        FTestResponseCapture Capture;
        TestTrue(TEXT("handler registered"), ConnectPins(SourceAName, Capture));
        if (!TestTrue(TEXT("first connect succeeded"), Capture.bSuccess))
        {
            AddError(FString::Printf(TEXT("connect_pins error: %s %s"),
                *Capture.ErrorCode, *Capture.Message));
            return true;
        }
        TestNotNull(TEXT("first connect result object present"), Capture.Result.Get());
        if (!Capture.Result.IsValid()) return true;

        bool bReplaced = true;
        TestTrue(TEXT("first connect reports replacedExistingEdge"),
            Capture.Result->TryGetBoolField(TEXT("replacedExistingEdge"), bReplaced));
        TestFalse(TEXT("first connect replaced nothing"), bReplaced);

        const TArray<TSharedPtr<FJsonValue>>* ReplacedEdges = nullptr;
        TestTrue(TEXT("first connect carries replacedEdges"),
            Capture.Result->TryGetArrayField(TEXT("replacedEdges"), ReplacedEdges));
        if (ReplacedEdges)
        {
            TestEqual(TEXT("first connect replacedEdges is empty"), ReplacedEdges->Num(), 0);
        }

        bool bConnected = false;
        TestTrue(TEXT("first connect reports connected"),
            Capture.Result->TryGetBoolField(TEXT("connected"), bConnected));
        TestTrue(TEXT("first connect measured connected == true"), bConnected);
    }

    TestTrue(TEXT("first edge really exists in the graph"),
        HasEdgeBetweenPins(SourceAOutPin, TargetInPin));

    // 2. The defect: wiring a second source into the same single-connection pin destroys the
    //    first edge. The response must name the destroyed edge in full, and warn.
    {
        FTestResponseCapture Capture;
        TestTrue(TEXT("handler registered"), ConnectPins(SourceBName, Capture));
        if (!TestTrue(TEXT("second connect succeeded"), Capture.bSuccess))
        {
            AddError(FString::Printf(TEXT("connect_pins error: %s %s"),
                *Capture.ErrorCode, *Capture.Message));
            return true;
        }
        TestNotNull(TEXT("second connect result object present"), Capture.Result.Get());
        if (!Capture.Result.IsValid()) return true;

        bool bConnected = false;
        TestTrue(TEXT("second connect reports connected"),
            Capture.Result->TryGetBoolField(TEXT("connected"), bConnected));
        TestTrue(TEXT("second connect measured connected == true"), bConnected);

        bool bReplaced = false;
        TestTrue(TEXT("second connect reports replacedExistingEdge"),
            Capture.Result->TryGetBoolField(TEXT("replacedExistingEdge"), bReplaced));
        TestTrue(TEXT("second connect reports a replacement happened"), bReplaced);

        const TArray<TSharedPtr<FJsonValue>>* ReplacedEdges = nullptr;
        TestTrue(TEXT("second connect carries replacedEdges"),
            Capture.Result->TryGetArrayField(TEXT("replacedEdges"), ReplacedEdges));
        if (ReplacedEdges && TestEqual(TEXT("exactly one edge was destroyed"), ReplacedEdges->Num(), 1))
        {
            const TSharedPtr<FJsonObject>* DestroyedObj = nullptr;
            if (TestTrue(TEXT("destroyed edge is an object"),
                (*ReplacedEdges)[0].IsValid() && (*ReplacedEdges)[0]->TryGetObject(DestroyedObj)) && DestroyedObj)
            {
                // All four endpoints, so the caller can re-create the wire it just lost.
                TestEqual(TEXT("destroyed edge names the displaced source node"),
                    (*DestroyedObj)->GetStringField(TEXT("fromNode")), SourceAName);
                TestEqual(TEXT("destroyed edge names the displaced source pin"),
                    (*DestroyedObj)->GetStringField(TEXT("fromPin")), PinName);
                TestEqual(TEXT("destroyed edge names the target node"),
                    (*DestroyedObj)->GetStringField(TEXT("toNode")), TargetName);
                TestEqual(TEXT("destroyed edge names the target pin"),
                    (*DestroyedObj)->GetStringField(TEXT("toPin")), PinName);
            }
        }

        const TArray<TSharedPtr<FJsonValue>>* Warnings = nullptr;
        TestTrue(TEXT("a replacement is warned about, not reported as a plain success"),
            Capture.Result->TryGetArrayField(TEXT("warnings"), Warnings)
            && Warnings != nullptr && Warnings->Num() > 0);
    }

    // 3. The response's claims must match the graph. Asserting only the response would let a
    //    handler that fabricates replacedEdges pass.
    TestFalse(TEXT("first edge is gone from the graph, as the response said"),
        HasEdgeBetweenPins(SourceAOutPin, TargetInPin));
    TestTrue(TEXT("second edge is present in the graph"),
        HasEdgeBetweenPins(SourceBOutPin, TargetInPin));
    return true;
}

#endif // __has_include("PCGGraph.h")
