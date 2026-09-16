// Copyright (c) 2026 Alexander Penkin. MIT License.

#if defined(__has_include) && __has_include("PCGGraph.h")

#include "Misc/AutomationTest.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Handlers/ErrorCodes.h"
#include "Tests/TestUtils.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/Package.h"

#include "PCGGraph.h"
#include "PCGNode.h"
#include "PCGSettings.h"
#include "Elements/PCGNormalToDensity.h"
#include "Elements/PCGSpatialNoise.h"
#include "Elements/PCGSelfPruning.h"

namespace PinWrightPCGTypedHelpersTests
{
    // Walk the graph and return the last-added user node (most recent). The core
    // graph tests confirm AddNodeOfType appends to Graph->GetNodes().
    UPCGNode* LastUserNode(UPCGGraph* Graph)
    {
        const TArray<UPCGNode*>& Nodes = Graph->GetNodes();
        return Nodes.Num() > 0 ? Nodes.Last() : nullptr;
    }

    TSharedPtr<FJsonObject> MakeFilterPayload(UPCGGraph* Graph)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("graphPath"), Graph->GetPathName());
        Payload->SetNumberField(TEXT("x"), 0);
        Payload->SetNumberField(TEXT("y"), 0);
        return Payload;
    }
}

// ---------------------------------------------------------------------------
// A. pcg.add_slope_filter applies Strength to the settings CDO.
//    Counterfactual: if the Strength apply line is removed, the property
//    retains its UPROPERTY default of 1.0 and the equality check fails.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPCGAddSlopeFilterAppliesStrengthTest,
    "PinWright.pcg.add_slope_filter.AppliesStrength",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPCGAddSlopeFilterAppliesStrengthTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightPCGTypedHelpersTests;

    UPCGGraph* Graph = NewObject<UPCGGraph>(GetTransientPackage(), UPCGGraph::StaticClass(),
        NAME_None, RF_Transient);
    TestNotNull(TEXT("transient graph created"), Graph);
    if (!Graph) return true;

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("graphPath"), Graph->GetPathName());
    Payload->SetNumberField(TEXT("x"), 0);
    Payload->SetNumberField(TEXT("y"), 0);
    TSharedPtr<FJsonObject> RequestedNormal = MakeShared<FJsonObject>();
    RequestedNormal->SetNumberField(TEXT("x"), 0.25);
    RequestedNormal->SetNumberField(TEXT("y"), -0.5);
    RequestedNormal->SetNumberField(TEXT("z"), 0.75);
    Payload->SetObjectField(TEXT("normal"), RequestedNormal);
    Payload->SetNumberField(TEXT("offset"), -0.35);
    Payload->SetNumberField(TEXT("strength"), 2.0);

    FTestResponseCapture Capture;
    TestTrue(TEXT("handler registered"),
        InvokeHandlerWithCapture(TEXT("pcg.add_slope_filter"), Payload, Capture));
    TestTrue(TEXT("responded"), Capture.bWasCalled);
    if (!TestTrue(TEXT("succeeded"), Capture.bSuccess))
    {
        AddError(FString::Printf(TEXT("add_slope_filter error: %s %s"),
            *Capture.ErrorCode, *Capture.Message));
        return true;
    }

    UPCGNode* Node = LastUserNode(Graph);
    TestNotNull(TEXT("node appended"), Node);
    if (!Node) return true;

    // UPCGNormalToDensitySettings has no PCG_API; resolve its UClass via reflection
    // to avoid an unresolved GetPrivateStaticClass link error on UE 5.4-5.7.
    UClass* NormalToDensityClass = FindObject<UClass>(nullptr, TEXT("/Script/PCG.PCGNormalToDensitySettings"));
    auto* RawSettings = Node->GetSettings();
    UPCGNormalToDensitySettings* Settings =
        (RawSettings && NormalToDensityClass && RawSettings->IsA(NormalToDensityClass))
            ? static_cast<UPCGNormalToDensitySettings*>(RawSettings)
            : nullptr;
    TestNotNull(TEXT("settings cast to UPCGNormalToDensitySettings"), Settings);
    if (!Settings) return true;

    // Counterfactual anchor: default Strength is 1.0; verifying 2.0 proves the
    // optional-knob apply path actually ran.
    TestEqual(TEXT("Strength applied"),
        static_cast<float>(Settings->Strength), 2.0f);

    TestTrue(TEXT("slope response is present"), Capture.Result.IsValid());
    if (!Capture.Result.IsValid()) return true;

    const TSharedPtr<FJsonObject>* ResponseNormal = nullptr;
    if (TestTrue(TEXT("response includes effective normal"),
            Capture.Result->TryGetObjectField(TEXT("normal"), ResponseNormal) && ResponseNormal))
    {
        double NormalX = 0.0;
        double NormalY = 0.0;
        double NormalZ = 0.0;
        TestTrue(TEXT("response normal.x is numeric"),
            (*ResponseNormal)->TryGetNumberField(TEXT("x"), NormalX));
        TestTrue(TEXT("response normal.y is numeric"),
            (*ResponseNormal)->TryGetNumberField(TEXT("y"), NormalY));
        TestTrue(TEXT("response normal.z is numeric"),
            (*ResponseNormal)->TryGetNumberField(TEXT("z"), NormalZ));
        TestEqual(TEXT("response normal.x matches effective setting"), NormalX,
            static_cast<double>(Settings->Normal.X));
        TestEqual(TEXT("response normal.y matches effective setting"), NormalY,
            static_cast<double>(Settings->Normal.Y));
        TestEqual(TEXT("response normal.z matches effective setting"), NormalZ,
            static_cast<double>(Settings->Normal.Z));
    }

    double ResponseOffset = 0.0;
    double ResponseStrength = 0.0;
    TestTrue(TEXT("response offset is numeric"),
        Capture.Result->TryGetNumberField(TEXT("offset"), ResponseOffset));
    TestTrue(TEXT("response strength is numeric"),
        Capture.Result->TryGetNumberField(TEXT("strength"), ResponseStrength));
    TestEqual(TEXT("response offset matches effective setting"), ResponseOffset,
        static_cast<double>(Settings->Offset));
    TestEqual(TEXT("response strength matches effective setting"), ResponseStrength,
        static_cast<double>(Settings->Strength));
    return true;
}

// ---------------------------------------------------------------------------
// B. Invalid noiseType values must be rejected before AddNodeOfType mutates the graph.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPCGAddNoiseFilterRejectsInvalidSpatialNoiseTypeAtomicallyTest,
    "PinWright.pcg.add_noise_filter.InvalidSpatialNoiseTypeNoMutation",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPCGAddNoiseFilterRejectsInvalidSpatialNoiseTypeAtomicallyTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightPCGTypedHelpersTests;

    UPCGGraph* Graph = NewObject<UPCGGraph>(GetTransientPackage(), UPCGGraph::StaticClass(),
        NAME_None, RF_Transient);
    TestNotNull(TEXT("transient graph created"), Graph);
    if (!Graph) return true;

    const int32 NodeCountBefore = Graph->GetNodes().Num();
    const bool bPackageDirtyBefore = Graph->GetOutermost()->IsDirty();
    TSharedPtr<FJsonObject> Payload = MakeFilterPayload(Graph);
    Payload->SetStringField(TEXT("kind"), TEXT("spatial"));
    Payload->SetStringField(TEXT("noiseType"), TEXT("NotARealNoiseMode"));

    FTestResponseCapture Capture;
    TestTrue(TEXT("handler registered"),
        InvokeHandlerWithCapture(TEXT("pcg.add_noise_filter"), Payload, Capture));
    TestTrue(TEXT("responded"), Capture.bWasCalled);
    TestFalse(TEXT("invalid spatial noiseType refused"), Capture.bSuccess);
    TestEqual(TEXT("error code == INVALID_NOISE_TYPE"),
        Capture.ErrorCode, FString(ErrorCodes::ERR_INVALID_NOISE_TYPE));
    TestEqual(TEXT("invalid spatial noiseType leaves node count unchanged"),
        Graph->GetNodes().Num(), NodeCountBefore);
    TestEqual(TEXT("invalid spatial noiseType preserves package dirtiness"),
        Graph->GetOutermost()->IsDirty(), bPackageDirtyBefore);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPCGAddNoiseFilterRejectsInvalidAttributeNoiseTypeAtomicallyTest,
    "PinWright.pcg.add_noise_filter.InvalidAttributeNoiseTypeNoMutation",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPCGAddNoiseFilterRejectsInvalidAttributeNoiseTypeAtomicallyTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightPCGTypedHelpersTests;

    UPCGGraph* Graph = NewObject<UPCGGraph>(GetTransientPackage(), UPCGGraph::StaticClass(),
        NAME_None, RF_Transient);
    TestNotNull(TEXT("transient graph created"), Graph);
    if (!Graph) return true;

    const int32 NodeCountBefore = Graph->GetNodes().Num();
    const bool bPackageDirtyBefore = Graph->GetOutermost()->IsDirty();
    TSharedPtr<FJsonObject> Payload = MakeFilterPayload(Graph);
    Payload->SetStringField(TEXT("kind"), TEXT("attribute"));
    Payload->SetStringField(TEXT("noiseType"), TEXT("NotARealNoiseMode"));

    FTestResponseCapture Capture;
    TestTrue(TEXT("handler registered"),
        InvokeHandlerWithCapture(TEXT("pcg.add_noise_filter"), Payload, Capture));
    TestTrue(TEXT("responded"), Capture.bWasCalled);
    TestFalse(TEXT("invalid attribute noiseType refused"), Capture.bSuccess);
    TestEqual(TEXT("error code == INVALID_NOISE_TYPE"),
        Capture.ErrorCode, FString(ErrorCodes::ERR_INVALID_NOISE_TYPE));
    TestEqual(TEXT("invalid attribute noiseType leaves node count unchanged"),
        Graph->GetNodes().Num(), NodeCountBefore);
    TestEqual(TEXT("invalid attribute noiseType preserves package dirtiness"),
        Graph->GetOutermost()->IsDirty(), bPackageDirtyBefore);
    return true;
}

// ---------------------------------------------------------------------------
// C. Malformed normal values must be rejected before AddNodeOfType mutates the graph.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPCGAddSlopeFilterRejectsMalformedNormalArrayAtomicallyTest,
    "PinWright.pcg.add_slope_filter.MalformedNormalArrayNoMutation",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPCGAddSlopeFilterRejectsMalformedNormalArrayAtomicallyTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightPCGTypedHelpersTests;

    UPCGGraph* Graph = NewObject<UPCGGraph>(GetTransientPackage(), UPCGGraph::StaticClass(),
        NAME_None, RF_Transient);
    TestNotNull(TEXT("transient graph created"), Graph);
    if (!Graph) return true;

    const int32 NodeCountBefore = Graph->GetNodes().Num();
    const bool bPackageDirtyBefore = Graph->GetOutermost()->IsDirty();
    TSharedPtr<FJsonObject> Payload = MakeFilterPayload(Graph);
    TArray<TSharedPtr<FJsonValue>> NormalValues;
    NormalValues.Add(MakeShared<FJsonValueNumber>(0.0));
    NormalValues.Add(MakeShared<FJsonValueNumber>(1.0));
    Payload->SetArrayField(TEXT("normal"), NormalValues);

    FTestResponseCapture Capture;
    TestTrue(TEXT("handler registered"),
        InvokeHandlerWithCapture(TEXT("pcg.add_slope_filter"), Payload, Capture));
    TestTrue(TEXT("responded"), Capture.bWasCalled);
    TestFalse(TEXT("short normal array refused"), Capture.bSuccess);
    TestEqual(TEXT("error code == INVALID_ARGUMENT"),
        Capture.ErrorCode, FString(ErrorCodes::ERR_INVALID_ARGUMENT));
    TestEqual(TEXT("short normal array leaves node count unchanged"),
        Graph->GetNodes().Num(), NodeCountBefore);
    TestEqual(TEXT("short normal array preserves package dirtiness"),
        Graph->GetOutermost()->IsDirty(), bPackageDirtyBefore);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPCGAddSlopeFilterRejectsMalformedNormalObjectAtomicallyTest,
    "PinWright.pcg.add_slope_filter.MalformedNormalObjectNoMutation",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPCGAddSlopeFilterRejectsMalformedNormalObjectAtomicallyTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightPCGTypedHelpersTests;

    UPCGGraph* Graph = NewObject<UPCGGraph>(GetTransientPackage(), UPCGGraph::StaticClass(),
        NAME_None, RF_Transient);
    TestNotNull(TEXT("transient graph created"), Graph);
    if (!Graph) return true;

    const int32 NodeCountBefore = Graph->GetNodes().Num();
    const bool bPackageDirtyBefore = Graph->GetOutermost()->IsDirty();
    TSharedPtr<FJsonObject> Payload = MakeFilterPayload(Graph);
    TSharedPtr<FJsonObject> NormalObject = MakeShared<FJsonObject>();
    NormalObject->SetNumberField(TEXT("X"), 0.0);
    NormalObject->SetNumberField(TEXT("Z"), 1.0);
    Payload->SetObjectField(TEXT("normal"), NormalObject);

    FTestResponseCapture Capture;
    TestTrue(TEXT("handler registered"),
        InvokeHandlerWithCapture(TEXT("pcg.add_slope_filter"), Payload, Capture));
    TestTrue(TEXT("responded"), Capture.bWasCalled);
    TestFalse(TEXT("normal object missing an axis refused"), Capture.bSuccess);
    TestEqual(TEXT("error code == INVALID_ARGUMENT"),
        Capture.ErrorCode, FString(ErrorCodes::ERR_INVALID_ARGUMENT));
    TestEqual(TEXT("normal object missing an axis leaves node count unchanged"),
        Graph->GetNodes().Num(), NodeCountBefore);
    TestEqual(TEXT("normal object missing an axis preserves package dirtiness"),
        Graph->GetOutermost()->IsDirty(), bPackageDirtyBefore);
    return true;
}

// ---------------------------------------------------------------------------
// E. pcg.add_noise_filter (kind=spatial) parses noiseType into Mode.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPCGAddNoiseFilterSpatialModeTest,
    "PinWright.pcg.add_noise_filter.SpatialVoronoi",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPCGAddNoiseFilterSpatialModeTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightPCGTypedHelpersTests;

    UPCGGraph* Graph = NewObject<UPCGGraph>(GetTransientPackage(), UPCGGraph::StaticClass(),
        NAME_None, RF_Transient);
    TestNotNull(TEXT("transient graph created"), Graph);
    if (!Graph) return true;

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("graphPath"), Graph->GetPathName());
    Payload->SetNumberField(TEXT("x"), 0);
    Payload->SetNumberField(TEXT("y"), 0);
    Payload->SetStringField(TEXT("kind"), TEXT("spatial"));
    Payload->SetStringField(TEXT("noiseType"), TEXT("Voronoi2D"));

    FTestResponseCapture Capture;
    TestTrue(TEXT("handler registered"),
        InvokeHandlerWithCapture(TEXT("pcg.add_noise_filter"), Payload, Capture));
    TestTrue(TEXT("responded"), Capture.bWasCalled);
    if (!TestTrue(TEXT("succeeded"), Capture.bSuccess))
    {
        AddError(FString::Printf(TEXT("add_noise_filter error: %s %s"),
            *Capture.ErrorCode, *Capture.Message));
        return true;
    }

    UPCGNode* Node = LastUserNode(Graph);
    TestNotNull(TEXT("node appended"), Node);
    if (!Node) return true;

    // UPCGSpatialNoiseSettings has no PCG_API; resolve its UClass via reflection
    // to avoid an unresolved GetPrivateStaticClass link error on UE 5.4-5.7.
    UClass* SpatialNoiseClass = FindObject<UClass>(nullptr, TEXT("/Script/PCG.PCGSpatialNoiseSettings"));
    auto* RawSettings = Node->GetSettings();
    UPCGSpatialNoiseSettings* Settings =
        (RawSettings && SpatialNoiseClass && RawSettings->IsA(SpatialNoiseClass))
            ? static_cast<UPCGSpatialNoiseSettings*>(RawSettings)
            : nullptr;
    TestNotNull(TEXT("settings cast to UPCGSpatialNoiseSettings"), Settings);
    if (!Settings) return true;

    TestEqual(TEXT("Mode parsed to Voronoi2D"),
        static_cast<int32>(Settings->Mode),
        static_cast<int32>(PCGSpatialNoiseMode::Voronoi2D));
    return true;
}

// ---------------------------------------------------------------------------
// F. pcg.add_subgraph rejects self-reference with RECURSIVE_SUBGRAPH.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPCGAddSubgraphRecursiveRejectedTest,
    "PinWright.pcg.add_subgraph.RecursiveRejected",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPCGAddSubgraphRecursiveRejectedTest::RunTest(const FString& Parameters)
{
    UPCGGraph* Graph = NewObject<UPCGGraph>(GetTransientPackage(), UPCGGraph::StaticClass(),
        NAME_None, RF_Transient);
    TestNotNull(TEXT("transient graph created"), Graph);
    if (!Graph) return true;

    const FString SelfPath = Graph->GetPathName();

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("graphPath"), SelfPath);
    Payload->SetNumberField(TEXT("x"), 0);
    Payload->SetNumberField(TEXT("y"), 0);
    Payload->SetStringField(TEXT("subgraphAsset"), SelfPath);

    FTestResponseCapture Capture;
    TestTrue(TEXT("handler registered"),
        InvokeHandlerWithCapture(TEXT("pcg.add_subgraph"), Payload, Capture));
    TestTrue(TEXT("responded"), Capture.bWasCalled);
    TestFalse(TEXT("self-reference refused"), Capture.bSuccess);
    TestEqual(TEXT("error code == RECURSIVE_SUBGRAPH"),
        Capture.ErrorCode, FString(TEXT("RECURSIVE_SUBGRAPH")));
    return true;
}

// ---------------------------------------------------------------------------
// G. pcg.set_self_pruning_settings rejects WRONG_NODE_TYPE when targeting a
//    non-self-pruning node (NormalToDensity).
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPCGSetSelfPruningSettingsWrongNodeTest,
    "PinWright.pcg.set_self_pruning_settings.WrongType",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPCGSetSelfPruningSettingsWrongNodeTest::RunTest(const FString& Parameters)
{
    UPCGGraph* Graph = NewObject<UPCGGraph>(GetTransientPackage(), UPCGGraph::StaticClass(),
        NAME_None, RF_Transient);
    TestNotNull(TEXT("transient graph created"), Graph);
    if (!Graph) return true;

    // UPCGNormalToDensitySettings has no PCG_API; resolve its UClass via reflection
    // to avoid an unresolved GetPrivateStaticClass link error on UE 5.4-5.7.
    UClass* NormalToDensityClassD = FindObject<UClass>(nullptr, TEXT("/Script/PCG.PCGNormalToDensitySettings"));
    TestNotNull(TEXT("UPCGNormalToDensitySettings class resolved via reflection"), NormalToDensityClassD);
    if (!NormalToDensityClassD) return true;
    UPCGSettings* DefaultSettings = nullptr;
    UPCGNode* WrongNode = Graph->AddNodeOfType(
        TSubclassOf<UPCGSettings>(NormalToDensityClassD),
        DefaultSettings);
    TestNotNull(TEXT("seeded non-pruning node"), WrongNode);
    if (!WrongNode) return true;

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("graphPath"), Graph->GetPathName());
    Payload->SetStringField(TEXT("nodeId"), WrongNode->GetName());
    Payload->SetStringField(TEXT("pruningType"), TEXT("LargeToSmall"));

    FTestResponseCapture Capture;
    TestTrue(TEXT("handler registered"),
        InvokeHandlerWithCapture(TEXT("pcg.set_self_pruning_settings"), Payload, Capture));
    TestTrue(TEXT("responded"), Capture.bWasCalled);
    TestFalse(TEXT("wrong-type rejected"), Capture.bSuccess);
    TestEqual(TEXT("error code == WRONG_NODE_TYPE"),
        Capture.ErrorCode, FString(TEXT("WRONG_NODE_TYPE")));
    return true;
}

#endif // __has_include("PCGGraph.h")
