// Copyright (c) 2026 Alexander Penkin. MIT License.

// Contract tests for pcg.set_node_property (board ticket F-pcg-set-node-property).
//
// Three things are checked, and the second is the one that matters most: a generic property
// writer that echoes its input back is indistinguishable from one that works, so every test here
// reads the value from a source the handler did not write into the response — the settings
// object's own memory — and, for the round-trip test, from the graph's change notification.

#if defined(__has_include) && __has_include("PCGGraph.h")

#include "Misc/AutomationTest.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Tests/TestUtils.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/Package.h"

#include "Compat/EngineVersionCompat.h"
#include "PCGGraph.h"
#include "PCGNode.h"
#include "PCGSettings.h"
#include "Elements/PCGSelfPruning.h"

namespace PinWrightPCGSetNodePropertyTests
{
    // UE 5.4 moved the self-pruning knobs into a nested FPCGSelfPruningParameters `Parameters`
    // member; on 5.3 they sit flat on the settings object. Same split PCGSetSelfPruningSettings.cpp
    // handles, expressed here as the property PATH the verb is handed — which is also what makes
    // this a dotted-path test on 5.4+.
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 4, 0)
    const TCHAR* const PruningTypePath = TEXT("Parameters.PruningType");
#else
    const TCHAR* const PruningTypePath = TEXT("PruningType");
#endif

    // Add a self-pruning node through the real pcg.add_node handler and hand back the node id the
    // caller would use. Returns an empty string (and reports the failure) when the add did not
    // succeed, so each test can bail without asserting on a graph it never got.
    FString AddSelfPruningNode(FAutomationTestBase& Test, UPCGGraph* Graph)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("graphPath"), Graph->GetPathName());
        Payload->SetStringField(TEXT("nodeClass"), TEXT("/Script/PCG.PCGSelfPruningSettings"));
        Payload->SetNumberField(TEXT("x"), 0);
        Payload->SetNumberField(TEXT("y"), 0);

        FTestResponseCapture Capture;
        if (!Test.TestTrue(TEXT("pcg.add_node registered"),
                InvokeHandlerWithCapture(TEXT("pcg.add_node"), Payload, Capture)))
        {
            return FString();
        }
        if (!Test.TestTrue(TEXT("pcg.add_node succeeded"), Capture.bSuccess) || !Capture.Result.IsValid())
        {
            Test.AddError(FString::Printf(TEXT("add_node error: %s %s"),
                *Capture.ErrorCode, *Capture.Message));
            return FString();
        }

        FString NodeId;
        Capture.Result->TryGetStringField(TEXT("nodeId"), NodeId);
        return NodeId;
    }

    UPCGSelfPruningSettings* LastSelfPruningSettings(UPCGGraph* Graph)
    {
        const TArray<UPCGNode*>& Nodes = Graph->GetNodes();
        UPCGNode* Node = Nodes.Num() > 0 ? Nodes.Last() : nullptr;
        return Node ? Cast<UPCGSelfPruningSettings>(Node->GetSettings()) : nullptr;
    }
}

// ---------------------------------------------------------------------------
// A. A top-level scalar write lands in the settings object, is reported MEASURED, and reaches the
//    graph's change notification — the chain a later pcg.generate depends on.
//    Counterfactual: drop the ApplyJsonValueToProperty call and Seed keeps its class default;
//    drop the NotifyPropertyChanged call and bGraphChanged stays false while the value still
//    reads back correctly, which is exactly the silent no-op this verb has to rule out.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPCGSetNodePropertyScalarRoundTripTest,
    "PinWright.pcg.set_node_property.ScalarRoundTripNotifiesGraph",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPCGSetNodePropertyScalarRoundTripTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightPCGSetNodePropertyTests;

    UPCGGraph* Graph = NewObject<UPCGGraph>(GetTransientPackage(), UPCGGraph::StaticClass(),
        NAME_None, RF_Transient);
    if (!TestNotNull(TEXT("transient graph created"), Graph)) return true;

    const FString NodeId = AddSelfPruningNode(*this, Graph);
    if (NodeId.IsEmpty()) return true;

    UPCGSelfPruningSettings* Settings = LastSelfPruningSettings(Graph);
    if (!TestNotNull(TEXT("self-pruning settings resolved"), Settings)) return true;

    const int32 RequestedSeed = 4242;
    // FAutomationTestBase::TestNotEqual has no integer overload, so the counterfactual anchor is
    // asserted as a plain predicate.
    TestTrue(TEXT("requested seed differs from the class default (counterfactual anchor)"),
        Settings->Seed != RequestedSeed);

    // The graph's own change notification is the signal a later pcg.generate rides on. Bound
    // before the write so nothing else in the test can have set it.
    bool bGraphChanged = false;
    FDelegateHandle GraphChangedHandle = Graph->OnGraphChangedDelegate.AddLambda(
        [&bGraphChanged](UPCGGraphInterface*, EPCGChangeType) { bGraphChanged = true; });

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("graphPath"), Graph->GetPathName());
    Payload->SetStringField(TEXT("nodeId"), NodeId);
    Payload->SetStringField(TEXT("property"), TEXT("Seed"));
    Payload->SetNumberField(TEXT("value"), RequestedSeed);

    FTestResponseCapture Capture;
    TestTrue(TEXT("handler registered"),
        InvokeHandlerWithCapture(TEXT("pcg.set_node_property"), Payload, Capture));

    Graph->OnGraphChangedDelegate.Remove(GraphChangedHandle);

    TestTrue(TEXT("responded"), Capture.bWasCalled);
    if (!TestTrue(TEXT("succeeded"), Capture.bSuccess) || !Capture.Result.IsValid())
    {
        AddError(FString::Printf(TEXT("set_node_property error: %s %s"),
            *Capture.ErrorCode, *Capture.Message));
        return true;
    }

    // Read from the object, not from the response: this is the assertion an echoing
    // implementation cannot pass.
    TestEqual(TEXT("Seed written into the settings object"), Settings->Seed, RequestedSeed);

    double MeasuredValue = 0.0;
    TestTrue(TEXT("response carries a measured 'value'"),
        Capture.Result->TryGetNumberField(TEXT("value"), MeasuredValue));
    TestEqual(TEXT("measured value equals the written Seed"),
        static_cast<int32>(MeasuredValue), RequestedSeed);

    bool bMatches = false;
    TestTrue(TEXT("valueMatchesRequest published for a comparable shape"),
        Capture.Result->TryGetBoolField(TEXT("valueMatchesRequest"), bMatches));
    TestTrue(TEXT("valueMatchesRequest true"), bMatches);

    bool bChangeNotified = false;
    Capture.Result->TryGetBoolField(TEXT("changeNotified"), bChangeNotified);
    TestTrue(TEXT("changeNotified reports the engine change path ran"), bChangeNotified);

    // requestedValue is the request, kept separate from the measurement.
    double RequestedEcho = 0.0;
    TestTrue(TEXT("requestedValue present"),
        Capture.Result->TryGetNumberField(TEXT("requestedValue"), RequestedEcho));
    TestEqual(TEXT("requestedValue echoes the request"),
        static_cast<int32>(RequestedEcho), RequestedSeed);

    // The write is useless to generation if it never reaches the graph.
    TestTrue(TEXT("the settings write reached UPCGGraph::OnGraphChangedDelegate"), bGraphChanged);

    // settingsPath names the object the write landed on — pcg.inspect reports only the class.
    FString SettingsPath;
    Capture.Result->TryGetStringField(TEXT("settingsPath"), SettingsPath);
    TestEqual(TEXT("settingsPath names the settings object that was written"),
        SettingsPath, Settings->GetPathName());

    return true;
}

// ---------------------------------------------------------------------------
// B. An enum written by symbolic name through a DOTTED path lands on the nested struct member,
//    and the read-back reports the enum's name rather than an ordinal.
//    Counterfactual: PruningType defaults to LargeToSmall, so SmallToLarge cannot be the
//    pre-existing value.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPCGSetNodePropertyNestedEnumTest,
    "PinWright.pcg.set_node_property.NestedEnumByName",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPCGSetNodePropertyNestedEnumTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightPCGSetNodePropertyTests;

    UPCGGraph* Graph = NewObject<UPCGGraph>(GetTransientPackage(), UPCGGraph::StaticClass(),
        NAME_None, RF_Transient);
    if (!TestNotNull(TEXT("transient graph created"), Graph)) return true;

    const FString NodeId = AddSelfPruningNode(*this, Graph);
    if (NodeId.IsEmpty()) return true;

    UPCGSelfPruningSettings* Settings = LastSelfPruningSettings(Graph);
    if (!TestNotNull(TEXT("self-pruning settings resolved"), Settings)) return true;

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("graphPath"), Graph->GetPathName());
    Payload->SetStringField(TEXT("nodeId"), NodeId);
    Payload->SetStringField(TEXT("property"), PruningTypePath);
    Payload->SetStringField(TEXT("value"), TEXT("SmallToLarge"));

    FTestResponseCapture Capture;
    TestTrue(TEXT("handler registered"),
        InvokeHandlerWithCapture(TEXT("pcg.set_node_property"), Payload, Capture));
    if (!TestTrue(TEXT("succeeded"), Capture.bSuccess) || !Capture.Result.IsValid())
    {
        AddError(FString::Printf(TEXT("set_node_property error: %s %s"),
            *Capture.ErrorCode, *Capture.Message));
        return true;
    }

#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 4, 0)
    const EPCGSelfPruningType WrittenType = Settings->Parameters.PruningType;
#else
    const EPCGSelfPruningType WrittenType = Settings->PruningType;
#endif
    TestEqual(TEXT("nested enum member written in the settings object"),
        static_cast<int32>(WrittenType), static_cast<int32>(EPCGSelfPruningType::SmallToLarge));

    FString MeasuredValue;
    TestTrue(TEXT("response carries a measured 'value'"),
        Capture.Result->TryGetStringField(TEXT("value"), MeasuredValue));
    TestEqual(TEXT("measured value is the enum's symbolic name"),
        MeasuredValue, FString(TEXT("SmallToLarge")));

    bool bMatches = false;
    TestTrue(TEXT("valueMatchesRequest published for an enum name"),
        Capture.Result->TryGetBoolField(TEXT("valueMatchesRequest"), bMatches));
    TestTrue(TEXT("valueMatchesRequest true"), bMatches);

    return true;
}

// ---------------------------------------------------------------------------
// C. A property the settings class does not declare is refused, and nothing is written.
//    Guards against the failure mode where an unresolved name silently succeeds.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPCGSetNodePropertyUnknownPropertyTest,
    "PinWright.pcg.set_node_property.UnknownPropertyRejected",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPCGSetNodePropertyUnknownPropertyTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightPCGSetNodePropertyTests;

    UPCGGraph* Graph = NewObject<UPCGGraph>(GetTransientPackage(), UPCGGraph::StaticClass(),
        NAME_None, RF_Transient);
    if (!TestNotNull(TEXT("transient graph created"), Graph)) return true;

    const FString NodeId = AddSelfPruningNode(*this, Graph);
    if (NodeId.IsEmpty()) return true;

    UPCGSelfPruningSettings* Settings = LastSelfPruningSettings(Graph);
    if (!TestNotNull(TEXT("self-pruning settings resolved"), Settings)) return true;

    const int32 SeedBefore = Settings->Seed;

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("graphPath"), Graph->GetPathName());
    Payload->SetStringField(TEXT("nodeId"), NodeId);
    Payload->SetStringField(TEXT("property"), TEXT("ThisPropertyDoesNotExist"));
    Payload->SetNumberField(TEXT("value"), 1);

    FTestResponseCapture Capture;
    TestTrue(TEXT("handler registered"),
        InvokeHandlerWithCapture(TEXT("pcg.set_node_property"), Payload, Capture));
    TestFalse(TEXT("refused"), Capture.bSuccess);
    TestEqual(TEXT("PROPERTY_NOT_FOUND"), Capture.ErrorCode, FString(TEXT("PROPERTY_NOT_FOUND")));
    TestEqual(TEXT("nothing was written"), Settings->Seed, SeedBefore);

    return true;
}

#endif // __has_include("PCGGraph.h")
