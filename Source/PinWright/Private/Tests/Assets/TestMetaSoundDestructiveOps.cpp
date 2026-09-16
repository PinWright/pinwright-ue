// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Misc/AutomationTest.h"

#if __has_include("MetasoundSource.h")

#include "MetasoundDocumentInterface.h"
#include "MetasoundFrontendDocument.h"
#include "MetasoundFrontendDocumentBuilder.h"
#include "MetasoundSource.h"
#include "Metasound.h"
#include "Tests/TestSkipReporting.h"
#include "Tests/TestUtils.h"
#include "Handlers/Audio/MetaSound/MetaSoundPathUtils.h"
#include "UObject/Package.h"

// UE 5.4+ builder: 3-arg constructor + FinishBuilding
#define TEST_HAS_METASOUND_FRONTEND_V2 1

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRemoveMetaSoundNodeAndDisconnectTest,
    "PinWright.Assets.RemoveMetaSoundNodeAndDisconnect",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FRemoveMetaSoundNodeAndDisconnectTest::RunTest(const FString& Parameters)
{
#if TEST_HAS_METASOUND_FRONTEND_V2
    // Create a transient UMetaSoundSource so it carries the MetaSoundSource interface
    // which gives us a real graph to manipulate.
    const FString AssetName = FString::Printf(TEXT("MS_DestructiveTest_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    const FString PackageName = FString::Printf(
        TEXT("/Game/PinWrightTests/%s"), *AssetName);
    UPackage* Package = CreatePackage(*PackageName);

    UMetaSoundSource* Source = NewObject<UMetaSoundSource>(
        Package,
        UMetaSoundSource::StaticClass(),
        *AssetName,
        RF_Public | RF_Standalone | RF_Transient);
    TestNotNull(TEXT("Transient UMetaSoundSource created"), Source);
    if (!Source)
    {
        return false;
    }
    Source->AddToRoot();

    // A freshly NewObject'd MetaSoundSource has an empty FMetasoundFrontendDocument
    // with zero PagedGraphs. The V2 builder's BeginBuilding() (called from its ctor with
    // bPrimeCache=true) walks the default page via FindConstGraphChecked() and asserts.
    // Real assets always go through the document builder during creation, so mirror that
    // by seeding the default page first with a non-priming builder that runs InitDocument().
    {
        TScriptInterface<IMetaSoundDocumentInterface> SeedInterface(Source);
        FMetaSoundFrontendDocumentBuilder SeedBuilder(SeedInterface);
        SeedBuilder.InitDocument();
        PW_METASOUND_FINISH_BUILDING(SeedBuilder);
    }

    // Build the script interface and builder (ctor 3rd arg bPrimeCache is 5.6+; macro selects form).
    TScriptInterface<IMetaSoundDocumentInterface> ScriptInterface(Source);
    PW_METASOUND_MAKE_BUILDER(Builder, ScriptInterface);

    // Add a Sine node. Skip gracefully if the node class registry isn't populated
    // (e.g., test context started before MetaSound module fully initialized).
    FMetasoundFrontendClassName SineClassName(FName(), FName("Metasound.Sine"), FName());
    const FMetasoundFrontendNode* SineNode = Builder.AddNodeByClassName(SineClassName, 1, FGuid::NewGuid());
    if (!SineNode)
    {
        // Node class registry not available in this test run — skip without failure.
        PinWrightTestSkip::SkipAssertions(*this, TEXT("metasound-registry-uninitialized"),
            TEXT("Skipping: Metasound.Sine class not found in registry (registry may not be initialized)"));
        PW_METASOUND_FINISH_BUILDING(Builder);
        Source->RemoveFromRoot();
        return true;
    }

    FGuid NodeID = SineNode->GetID();
    TestTrue(TEXT("SineNode has valid GUID"), NodeID.IsValid());

    // --- Core contract: RemoveNode returns true and the node is gone ---

    bool bRemoved = Builder.RemoveNode(NodeID);
    TestTrue(TEXT("Builder.RemoveNode returns true for existing node"), bRemoved);

    const FMetasoundFrontendNode* NodeAfterRemove = Builder.FindNode(NodeID);
    TestNull(TEXT("FindNode returns null after RemoveNode — node is truly gone"), NodeAfterRemove);

    // Calling RemoveNode a second time on the same GUID must return false (idempotency contract).
    bool bRemovedAgain = Builder.RemoveNode(NodeID);
    TestFalse(TEXT("Builder.RemoveNode returns false for already-removed node"), bRemovedAgain);

    PW_METASOUND_FINISH_BUILDING(Builder);
    Source->RemoveFromRoot();
    return true;
#else
    PinWrightTestSkip::SkipAssertions(*this, TEXT("engine-version-unsupported"),
        TEXT("Skipping FRemoveMetaSoundNodeAndDisconnectTest: requires UE 5.4+ MetaSound frontend APIs"));
    return true;
#endif // TEST_HAS_METASOUND_FRONTEND_V2
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDisconnectMetaSoundNodesRejectsMissingEdgeTest,
    "PinWright.Assets.DisconnectMetaSoundNodesRejectsMissingEdge",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDisconnectMetaSoundNodesRejectsMissingEdgeTest::RunTest(const FString& Parameters)
{
#if TEST_HAS_METASOUND_FRONTEND_V2
    const FString AssetName = FString::Printf(TEXT("MS_DisconnectMissingEdgeTest_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    const FString PackageName = FString::Printf(
        TEXT("/Game/PinWrightTests/%s"), *AssetName);
    UPackage* Package = CreatePackage(*PackageName);

    UMetaSoundSource* Source = NewObject<UMetaSoundSource>(
        Package,
        UMetaSoundSource::StaticClass(),
        *AssetName,
        RF_Public | RF_Standalone | RF_Transient);
    TestNotNull(TEXT("Transient UMetaSoundSource created"), Source);
    if (!Source)
    {
        return false;
    }
    Source->AddToRoot();

    {
        TScriptInterface<IMetaSoundDocumentInterface> SeedInterface(Source);
        FMetaSoundFrontendDocumentBuilder SeedBuilder(SeedInterface);
        SeedBuilder.InitDocument();
        PW_METASOUND_FINISH_BUILDING(SeedBuilder);
    }

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), Source->GetPathName());
    Payload->SetStringField(TEXT("sourceNodeId"), TEXT("00000000-0000-0000-0000-000000000000"));
    Payload->SetStringField(TEXT("sourceOutputName"), TEXT("Out"));
    Payload->SetStringField(TEXT("targetNodeId"), TEXT("11111111-1111-1111-1111-111111111111"));
    Payload->SetStringField(TEXT("targetInputName"), TEXT("In"));
    Payload->SetBoolField(TEXT("save"), false);

    FTestResponseCapture Capture;
    const bool bHandlerFound = InvokeHandlerWithCapture(
        TEXT("audio.authoring.disconnect_metasound_nodes"), Payload, Capture);

    TestTrue(TEXT("disconnect_metasound_nodes handler found"), bHandlerFound);
    TestTrue(TEXT("disconnect_metasound_nodes sent a response"), Capture.bWasCalled);
    TestFalse(TEXT("missing MetaSound edge is rejected"), Capture.bSuccess);
    TestEqual(TEXT("missing MetaSound edge error code"), Capture.ErrorCode, FString(TEXT("EDGE_NOT_FOUND")));
    if (Capture.Result.IsValid())
    {
        TestNotEqual(TEXT("missing MetaSound edge does not report one removed edge"),
            Capture.Result->GetNumberField(TEXT("edgesRemoved")), 1.0);
    }

    Source->RemoveFromRoot();
    return true;
#else
    PinWrightTestSkip::SkipAssertions(*this, TEXT("engine-version-unsupported"),
        TEXT("Skipping FDisconnectMetaSoundNodesRejectsMissingEdgeTest: requires UE 5.4+ MetaSound frontend APIs"));
    return true;
#endif // TEST_HAS_METASOUND_FRONTEND_V2
}

// Regression test for B-add-metasound-node-rejects-registry-classnames.
// Proves the production add_metasound_node handler resolves the canonical dotted
// registry key that search_metasound_nodes emits ("UE.Sine.Audio"), and that the
// refreshed nodeType shorthand ("oscillator") resolves to a live UE.* class.
// Pre-fix, the handler crammed the whole dotted string into the Name field with empty
// Namespace/Variant (AudioAuthoringHandler.cpp:771) and the shorthand mapped to the
// stale "Metasound.Sine" — both returned NODE_CLASS_NOT_FOUND. This test fails if the
// FMetasoundFrontendClassName::Parse fix or the shorthand refresh is reverted.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAddMetaSoundNodeResolvesRegistryClassNameTest,
    "PinWright.Assets.AddMetaSoundNodeResolvesRegistryClassName",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAddMetaSoundNodeResolvesRegistryClassNameTest::RunTest(const FString& Parameters)
{
#if TEST_HAS_METASOUND_FRONTEND_V2
    const FString AssetName = FString::Printf(TEXT("MS_AddNodeClassNameTest_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    const FString PackageName = FString::Printf(
        TEXT("/Game/PinWrightTests/%s"), *AssetName);
    UPackage* Package = CreatePackage(*PackageName);

    UMetaSoundSource* Source = NewObject<UMetaSoundSource>(
        Package,
        UMetaSoundSource::StaticClass(),
        *AssetName,
        RF_Public | RF_Standalone | RF_Transient);
    TestNotNull(TEXT("Transient UMetaSoundSource created"), Source);
    if (!Source)
    {
        return false;
    }
    Source->AddToRoot();

    // Seed the default page so the handler's primed builder doesn't assert (same pattern
    // as the destructive-ops tests above).
    {
        TScriptInterface<IMetaSoundDocumentInterface> SeedInterface(Source);
        FMetaSoundFrontendDocumentBuilder SeedBuilder(SeedInterface);
        SeedBuilder.InitDocument();
        PW_METASOUND_FINISH_BUILDING(SeedBuilder);
    }

    const FString AssetPath = Source->GetPathName();

    // Probe whether the MetaSound class registry is populated in this test run by asking the
    // builder directly for the structured Sine key. If even the structured form is unavailable,
    // the registry isn't initialized here — skip without failure (mirrors the destructive-ops
    // skip). If the structured form DOES resolve, the handler must too, or the fix is broken.
    bool bRegistryHasSine = false;
    {
        TScriptInterface<IMetaSoundDocumentInterface> ProbeInterface(Source);
        PW_METASOUND_MAKE_BUILDER(ProbeBuilder, ProbeInterface);
        FMetasoundFrontendClassName StructuredSine(FName(TEXT("UE")), FName(TEXT("Sine")), FName(TEXT("Audio")));
        const FMetasoundFrontendNode* Probe = ProbeBuilder.AddNodeByClassName(StructuredSine, 1, FGuid::NewGuid());
        if (Probe)
        {
            bRegistryHasSine = true;
            ProbeBuilder.RemoveNode(Probe->GetID());
        }
        PW_METASOUND_FINISH_BUILDING(ProbeBuilder);
    }

    if (!bRegistryHasSine)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("metasound-registry-uninitialized"),
            TEXT("Skipping: UE.Sine.Audio not in registry (MetaSound registry not initialized in this run)"));
        Source->RemoveFromRoot();
        return true;
    }

    // 1) Canonical search-output className must round-trip into add (the core fix).
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("assetPath"), AssetPath);
        Payload->SetStringField(TEXT("nodeClassName"), TEXT("UE.Sine.Audio"));
        Payload->SetBoolField(TEXT("save"), false);

        FTestResponseCapture Capture;
        const bool bFound = InvokeHandlerWithCapture(
            TEXT("audio.authoring.add_metasound_node"), Payload, Capture);
        TestTrue(TEXT("add_metasound_node handler found"), bFound);
        TestTrue(TEXT("add_metasound_node sent a response"), Capture.bWasCalled);
        TestTrue(TEXT("UE.Sine.Audio (canonical search output) resolves and is added"), Capture.bSuccess);
        if (!Capture.bSuccess)
        {
            AddError(FString::Printf(TEXT("add_metasound_node rejected canonical className: [%s] %s"),
                *Capture.ErrorCode, *Capture.Message));
        }
        else if (Capture.Result.IsValid())
        {
            // The handler reports the registry-resolved dotted key; it must equal the input.
            TestEqual(TEXT("resolved nodeClassName round-trips to UE.Sine.Audio"),
                Capture.Result->GetStringField(TEXT("nodeClassName")), FString(TEXT("UE.Sine.Audio")));
        }
    }

    // 2) Refreshed nodeType shorthand "oscillator" must resolve to a live UE.* class.
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("assetPath"), AssetPath);
        Payload->SetStringField(TEXT("nodeType"), TEXT("oscillator"));
        Payload->SetBoolField(TEXT("save"), false);

        FTestResponseCapture Capture;
        InvokeHandlerWithCapture(TEXT("audio.authoring.add_metasound_node"), Payload, Capture);
        TestTrue(TEXT("nodeType 'oscillator' shorthand resolves to a live UE.* class"), Capture.bSuccess);
        if (!Capture.bSuccess)
        {
            AddError(FString::Printf(TEXT("oscillator shorthand rejected: [%s] %s"),
                *Capture.ErrorCode, *Capture.Message));
        }
    }

    // 3) versionMajor/versionMinor must no longer be rejected as UNKNOWN_PARAMS.
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("assetPath"), AssetPath);
        Payload->SetStringField(TEXT("nodeClassName"), TEXT("UE.Sine.Audio"));
        Payload->SetNumberField(TEXT("versionMajor"), 1);
        Payload->SetNumberField(TEXT("versionMinor"), 1);
        Payload->SetBoolField(TEXT("save"), false);

        FTestResponseCapture Capture;
        InvokeHandlerWithCapture(TEXT("audio.authoring.add_metasound_node"), Payload, Capture);
        TestNotEqual(TEXT("versionMajor/versionMinor are accepted (not UNKNOWN_PARAMS)"),
            Capture.ErrorCode, FString(TEXT("UNKNOWN_PARAMS")));
        TestTrue(TEXT("versioned add still resolves the node"), Capture.bSuccess);
    }

    Source->RemoveFromRoot();
    return true;
#else
    PinWrightTestSkip::SkipAssertions(*this, TEXT("engine-version-unsupported"),
        TEXT("Skipping FAddMetaSoundNodeResolvesRegistryClassNameTest: requires UE 5.4+ MetaSound frontend APIs"));
    return true;
#endif // TEST_HAS_METASOUND_FRONTEND_V2
}

#endif // __has_include("MetasoundSource.h")
