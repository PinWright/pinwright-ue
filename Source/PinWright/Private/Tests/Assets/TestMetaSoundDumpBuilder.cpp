// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Misc/AutomationTest.h"
#include "Handlers/Asset/AssetDumpHandler.h"
#include "Handlers/Asset/MetaSoundDumpBuilder.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "AssetDumpTestHelpers.h"
#include "Tests/TestUtils.h"

#if __has_include("MetasoundSource.h")

#include "MetasoundDocumentInterface.h"
#include "MetasoundFrontendDocument.h"
#include "MetasoundFrontendDocumentBuilder.h"
#include "MetasoundSource.h"
#include "Metasound.h"
#include "UObject/Package.h"
// Shared 5.4+/5.6 builder-lifecycle compat (PW_METASOUND_MAKE_BUILDER / PW_METASOUND_FINISH_BUILDING)
// and the PW_METASOUND_HAS_SEARCH_ENGINE availability predicate.
#include "Handlers/Audio/MetaSound/MetaSoundPathUtils.h"

// isInterfaceMember tagging is only derivable when the MetaSound interface registry (search
// engine) is present; the regression test for it gates on the shared
// PW_METASOUND_HAS_SEARCH_ENGINE predicate from MetaSoundPathUtils.h, matching the dump
// builder's own guard.

namespace
{
    using AssetDumpTestHelpers::HasDumpFile;
    using AssetDumpTestHelpers::FindDumpFile;
    using AssetDumpTestHelpers::LoadJsonFile;

    FString MakeUniqueMetaSoundTestAssetName(const TCHAR* Prefix)
    {
        return FString::Printf(TEXT("%s_%s"), Prefix, *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    }

    UMetaSoundPatch* NewTransientMetaSoundPatch(FString& OutObjectPath)
    {
        const FString AssetName = MakeUniqueMetaSoundTestAssetName(TEXT("MS_PatchDump"));
        const FString PackageName = FString::Printf(TEXT("/Game/PinWrightTests/%s"), *AssetName);
        UPackage* Package = CreatePackage(*PackageName);
        UMetaSoundPatch* Patch = NewObject<UMetaSoundPatch>(
            Package,
            UMetaSoundPatch::StaticClass(),
            *AssetName,
            RF_Public | RF_Standalone | RF_Transient);
        if (!Patch)
        {
            return nullptr;
        }
        Patch->AddToRoot();

        // A freshly NewObject'd MetaSound asset has an empty FMetasoundFrontendDocument
        // with zero PagedGraphs. Production code (and the dump builder) calls
        // FindConstGraphChecked(DefaultPageID) which asserts on the empty list.
        // Real assets always go through the document builder during creation, so
        // mirror that here to produce a representative fixture.
        TScriptInterface<IMetaSoundDocumentInterface> DocInterface(Patch);
        FMetaSoundFrontendDocumentBuilder Builder(DocInterface);
        Builder.InitDocument();

        OutObjectPath = FString::Printf(TEXT("%s.%s"), *PackageName, *AssetName);
        return Patch;
    }

    // Creates an empty transient UMetaSoundSource (package + NewObject with the standard public/
    // standalone/transient flags + AddToRoot) and returns it; callers build their own
    // FMetaSoundFrontendDocumentBuilder on top. The builder is intentionally left to the caller as a
    // stack local because it must outlive the subsequent AddInterface/AddGraphOutput calls, and it
    // must use the raw non-priming 1-arg ctor: a freshly NewObject'd source has no paged graph yet,
    // so priming (BeginBuilding -> FindConstGraphChecked) asserts before InitDocument() creates the
    // graph. The 5.6+ PW_METASOUND_MAKE_BUILDER macro forces bPrimeCache=true, so it can't be used
    // here. Returns nullptr if NewObject fails.
    UMetaSoundSource* NewEmptyTransientSource(const TCHAR* Prefix)
    {
        const FString AssetName = MakeUniqueMetaSoundTestAssetName(Prefix);
        const FString PackageName = FString::Printf(TEXT("/Game/PinWrightTests/%s"), *AssetName);
        UPackage* Package = CreatePackage(*PackageName);
        UMetaSoundSource* Source = NewObject<UMetaSoundSource>(
            Package,
            UMetaSoundSource::StaticClass(),
            *AssetName,
            RF_Public | RF_Standalone | RF_Transient);
        if (Source)
        {
            Source->AddToRoot();
        }
        return Source;
    }

    // Adds one user-authored Audio (reference-access) graph output with fresh vertex/node GUIDs.
    // AddGraphOutput inserts (and returns) a real member node, giving the graph a node whose
    // per-vertex inputs/outputs the describe_metasound compact/nodeIds knobs trim.
    void AddAudioGraphOutput(FMetaSoundFrontendDocumentBuilder& Builder, FName OutputName)
    {
        FMetasoundFrontendClassOutput Output;
        Output.Name = OutputName;
        Output.TypeName = FName(TEXT("Audio"));
        Output.VertexID = FGuid::NewGuid();
        Output.NodeID = FGuid::NewGuid();
        Output.AccessType = EMetasoundFrontendVertexAccessType::Reference;
        Builder.AddGraphOutput(Output);
    }

    // Builds a transient UMetaSoundSource with two user-authored graph outputs, so the resulting
    // graph has nodes whose per-vertex inputs/outputs the describe_metasound compact/nodeIds knobs
    // trim. No interface is attached, so this fixture is independent of the interface search engine
    // and runs on every engine.
    UMetaSoundSource* NewTransientSourceWithTwoOutputs()
    {
        UMetaSoundSource* Source = NewEmptyTransientSource(TEXT("MS_CompactDump"));
        if (!Source)
        {
            return nullptr;
        }

        TScriptInterface<IMetaSoundDocumentInterface> DocInterface(Source);
        FMetaSoundFrontendDocumentBuilder Builder(DocInterface);
        Builder.InitDocument();

        AddAudioGraphOutput(Builder, FName(TEXT("OutA")));
        AddAudioGraphOutput(Builder, FName(TEXT("OutB")));

        PW_METASOUND_FINISH_BUILDING(Builder);
        return Source;
    }

#if PW_METASOUND_HAS_SEARCH_ENGINE
    // Build a transient UMetaSoundSource that has a registered output-format interface
    // attached (so its declared vertices land in rootGraph.interface AND in
    // Document.Interfaces — the exact auto-attached case) plus one user-authored output
    // that belongs to no interface. This lets the dump builder's isInterfaceMember
    // tagging be exercised end-to-end against production code.
    UMetaSoundSource* NewTransientSourceWithInterfaceAndUserOutput(FName UserOutputName)
    {
        UMetaSoundSource* Source = NewEmptyTransientSource(TEXT("MS_SourceDump"));
        if (!Source)
        {
            return nullptr;
        }

        // Raw non-priming 1-arg builder ctor — see NewEmptyTransientSource for why priming asserts
        // on a freshly NewObject'd source before InitDocument() creates the graph.
        TScriptInterface<IMetaSoundDocumentInterface> DocInterface(Source);
        FMetaSoundFrontendDocumentBuilder Builder(DocInterface);
        Builder.InitDocument();

        // Attach a known interface; its declared I/O vertices are copied into the
        // rootGraph interface with their declared names, which is what the dump
        // builder matches against Document.Interfaces.
        Builder.AddInterface(FName(TEXT("UE.OutputFormat.Mono")));

        // A user-authored output that is not part of any attached interface.
        AddAudioGraphOutput(Builder, UserOutputName);

        PW_METASOUND_FINISH_BUILDING(Builder);
        return Source;
    }
#endif // PW_METASOUND_HAS_SEARCH_ENGINE

}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMetaSoundDumpBuilderShapeTest,
    "PinWright.Assets.MetaSound.DumpBuilder.Shape",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMetaSoundDumpBuilderShapeTest::RunTest(const FString& Parameters)
{
    FString ObjectPath;
    UMetaSoundPatch* Patch = NewTransientMetaSoundPatch(ObjectPath);
    TestNotNull(TEXT("Transient MetaSoundPatch created"), Patch);
    if (!Patch)
    {
        return false;
    }

    TSharedPtr<FJsonObject> Json = MetaSoundDumpBuilder::BuildMetaSoundJson(Patch);
    TestTrue(TEXT("BuildMetaSoundJson returns non-null"), Json.IsValid());
    if (!Json.IsValid())
    {
        Patch->RemoveFromRoot();
        return false;
    }

    TestEqual(TEXT("assetKind"), Json->GetStringField(TEXT("assetKind")), FString(TEXT("MetaSoundPatch")));
    TestEqual(TEXT("assetPath"), Json->GetStringField(TEXT("assetPath")), Patch->GetPathName());
    TestTrue(TEXT("rootGraph object exists"), Json->HasTypedField<EJson::Object>(TEXT("rootGraph")));
    TestTrue(TEXT("nodes array exists"), Json->HasTypedField<EJson::Array>(TEXT("nodes")));
    TestTrue(TEXT("edges array exists"), Json->HasTypedField<EJson::Array>(TEXT("edges")));
    TestTrue(TEXT("variables array exists"), Json->HasTypedField<EJson::Array>(TEXT("variables")));

    Patch->RemoveFromRoot();
    return true;
}

// Regression guard for B-asset-dump-metasound-unmigrated-assert: a MetaSound whose document
// carries no default-page graph used to abort the editor inside FindConstGraphChecked, which
// took the whole asset.dump_folder sweep with it and left the mirror silently half-written. The
// builder must report the gap in-band instead. In production that state is an asset whose 5.8
// async document versioning has not run yet; the equivalent reachable-from-a-test state is a
// freshly NewObject'd source with no InitDocument() call, whose PagedGraphs array is likewise
// empty (see NewEmptyTransientSource).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMetaSoundDumpBuilderMissingGraphTest,
    "PinWright.Assets.MetaSound.DumpBuilder.MissingGraphIsSkippedNotFatal",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMetaSoundDumpBuilderMissingGraphTest::RunTest(const FString& Parameters)
{
#if !UE_VERSION_OLDER_THAN(5, 5, 0)
    // The builder logs exactly once when it emits the stub.
    AddExpectedMessagePlain(TEXT("has no default-page graph after versioning"),
        ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1);
#endif

    UMetaSoundSource* Source = NewEmptyTransientSource(TEXT("MS_NoGraphDump"));
    TestNotNull(TEXT("Transient MetaSoundSource created"), Source);
    if (!Source)
    {
        return false;
    }

    TSharedPtr<FJsonObject> Json = MetaSoundDumpBuilder::BuildMetaSoundJson(Source);
    TestTrue(TEXT("BuildMetaSoundJson survives a document with no default-page graph"), Json.IsValid());
    if (!Json.IsValid())
    {
        Source->RemoveFromRoot();
        return false;
    }

    // Identity is still emitted: the document loaded, only its graph is missing.
    TestEqual(TEXT("assetKind"), Json->GetStringField(TEXT("assetKind")), FString(TEXT("MetaSoundSource")));
    TestEqual(TEXT("assetPath"), Json->GetStringField(TEXT("assetPath")), Source->GetPathName());

#if UE_VERSION_OLDER_THAN(5, 5, 0)
    // Pre-paged-graphs engines keep the graph in the single FMetasoundFrontendGraphClass::Graph
    // member, which always exists — there is no missing-graph state to report there.
    TestTrue(TEXT("nodes array exists"), Json->HasTypedField<EJson::Array>(TEXT("nodes")));
#else
    bool bSkipped = false;
    TestTrue(TEXT("skipped flag is set"),
        Json->TryGetBoolField(TEXT("skipped"), bSkipped) && bSkipped);
    TestEqual(TEXT("skipReason names the missing graph"),
        Json->GetStringField(TEXT("skipReason")),
        FString(MetaSoundDumpBuilder::SkipReasonGraphUnavailable));
    // Omitted rather than emitted empty: an empty nodes[] is indistinguishable from a graph
    // that genuinely has no nodes, and this document's node count is unknown, not zero.
    TestFalse(TEXT("nodes array omitted"), Json->HasField(TEXT("nodes")));
    TestFalse(TEXT("edges array omitted"), Json->HasField(TEXT("edges")));
    TestFalse(TEXT("variables array omitted"), Json->HasField(TEXT("variables")));
#endif

    Source->RemoveFromRoot();
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMetaSoundAssetDumpWritesMetaSoundAspectFileTest,
    "PinWright.Assets.MetaSound.AssetDump.WritesMetaSoundAspectFile",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMetaSoundAssetDumpWritesMetaSoundAspectFileTest::RunTest(const FString& Parameters)
{
    FString ObjectPath;
    UMetaSoundPatch* Patch = NewTransientMetaSoundPatch(ObjectPath);
    TestNotNull(TEXT("Transient MetaSoundPatch created"), Patch);
    if (!Patch)
    {
        return false;
    }

    const FString ScratchRoot = FPaths::ProjectIntermediateDir()
        / TEXT("MetaSoundDumpBuilderTests") / FGuid::NewGuid().ToString(EGuidFormats::Digits);

    const AssetDumpHandler::FDumpSingleResult Result =
        AssetDumpHandler::DumpSingleAsset(ObjectPath, ScratchRoot, /*bDiff=*/false);

    TestTrue(TEXT("asset.dump succeeds for transient MetaSoundPatch"), Result.ErrorCode.IsEmpty());
    TestTrue(TEXT("metasound.json is written"), HasDumpFile(Result.WrittenPaths, DumpFileNames::MetaSound));
    TestTrue(TEXT("properties.json is written"), HasDumpFile(Result.WrittenPaths, DumpFileNames::Properties));
    TestFalse(TEXT("sound_wave.json is NOT written (MetaSound must short-circuit SoundWave dispatch)"),
        HasDumpFile(Result.WrittenPaths, DumpFileNames::SoundWave));

    const FString PropertiesPath = FindDumpFile(Result.WrittenPaths, DumpFileNames::Properties);
    TSharedPtr<FJsonObject> PropertiesJson = LoadJsonFile(PropertiesPath);
    TestTrue(TEXT("properties.json parses"), PropertiesJson.IsValid());
    if (PropertiesJson.IsValid())
    {
        TestFalse(TEXT("RootMetasoundDocument was dropped from properties.json"),
            PropertiesJson->HasField(TEXT("RootMetasoundDocument")));
    }

    const FString MetaSoundPath = FindDumpFile(Result.WrittenPaths, DumpFileNames::MetaSound);
    TSharedPtr<FJsonObject> MetaSoundJson = LoadJsonFile(MetaSoundPath);
    TestTrue(TEXT("metasound.json parses"), MetaSoundJson.IsValid());
    if (MetaSoundJson.IsValid())
    {
        TestEqual(TEXT("metasound.json assetKind"),
            MetaSoundJson->GetStringField(TEXT("assetKind")), FString(TEXT("MetaSoundPatch")));
    }

    IFileManager::Get().DeleteDirectory(*ScratchRoot, /*RequireExists=*/false, /*Tree=*/true);
    Patch->RemoveFromRoot();
    return true;
}

// Regression guard for E-describe-metasound-no-compact-mode: describe_metasound's opt-in
// compact/nodeIds knobs must shrink the payload (drop per-node vertices / restrict nodes[]) and
// add a summary header, WITHOUT changing the default snapshot (asset.dump's metasound.json
// parity). Reverting the fix removes the options overload (build break) or stops trimming
// (these assertions fail).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMetaSoundDumpBuilderCompactAndNodeIdsTest,
    "PinWright.Assets.MetaSound.DumpBuilder.CompactAndNodeIds",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMetaSoundDumpBuilderCompactAndNodeIdsTest::RunTest(const FString& Parameters)
{
    UMetaSoundSource* Source = NewTransientSourceWithTwoOutputs();
    TestNotNull(TEXT("Transient MetaSoundSource created"), Source);
    if (!Source)
    {
        return false;
    }

    auto NodesOf = [](const TSharedPtr<FJsonObject>& Json) -> const TArray<TSharedPtr<FJsonValue>>*
    {
        const TArray<TSharedPtr<FJsonValue>>* Nodes = nullptr;
        return (Json.IsValid() && Json->TryGetArrayField(TEXT("nodes"), Nodes)) ? Nodes : nullptr;
    };

    // Default snapshot: full per-node vertices, no summary header (asset.dump parity).
    TSharedPtr<FJsonObject> FullJson = MetaSoundDumpBuilder::BuildMetaSoundJson(Source);
    const TArray<TSharedPtr<FJsonValue>>* FullNodes = NodesOf(FullJson);
    TestNotNull(TEXT("default nodes array present"), FullNodes);
    if (!FullNodes || FullNodes->Num() == 0)
    {
        TestTrue(TEXT("fixture produced at least one graph node"), FullNodes && FullNodes->Num() > 0);
        Source->RemoveFromRoot();
        return false;
    }
    const int32 TotalNodes = FullNodes->Num();
    TestFalse(TEXT("default snapshot omits compact header"), FullJson->HasField(TEXT("compact")));
    TestFalse(TEXT("default snapshot omits nodeCount header"), FullJson->HasField(TEXT("nodeCount")));

    FString FirstNodeId;
    {
        const TSharedPtr<FJsonObject> First = (*FullNodes)[0]->AsObject();
        TestTrue(TEXT("default node carries inputs array"), First.IsValid() && First->HasField(TEXT("inputs")));
        TestTrue(TEXT("default node carries outputs array"), First.IsValid() && First->HasField(TEXT("outputs")));
        if (First.IsValid())
        {
            First->TryGetStringField(TEXT("id"), FirstNodeId);
        }
    }
    TestTrue(TEXT("captured a node id from the default dump"), !FirstNodeId.IsEmpty());

    // compact=true: every node drops its per-vertex inputs/outputs; summary header appears.
    MetaSoundDumpBuilder::FMetaSoundDumpOptions CompactOpts;
    CompactOpts.bCompact = true;
    TSharedPtr<FJsonObject> CompactJson = MetaSoundDumpBuilder::BuildMetaSoundJson(Source, CompactOpts);
    const TArray<TSharedPtr<FJsonValue>>* CompactNodes = NodesOf(CompactJson);
    TestNotNull(TEXT("compact nodes array present"), CompactNodes);
    if (CompactNodes)
    {
        TestEqual(TEXT("compact keeps every node"), CompactNodes->Num(), TotalNodes);
        bool bAnyVertices = false;
        for (const TSharedPtr<FJsonValue>& Value : *CompactNodes)
        {
            const TSharedPtr<FJsonObject> Obj = Value.IsValid() ? Value->AsObject() : nullptr;
            if (Obj.IsValid() && (Obj->HasField(TEXT("inputs")) || Obj->HasField(TEXT("outputs"))))
            {
                bAnyVertices = true;
            }
        }
        TestFalse(TEXT("compact nodes drop per-vertex inputs/outputs"), bAnyVertices);
    }
    bool bCompactFlag = false;
    TestTrue(TEXT("compact header flag present and true"),
        CompactJson.IsValid() && CompactJson->TryGetBoolField(TEXT("compact"), bCompactFlag) && bCompactFlag);
    TestTrue(TEXT("compact nodeCount header present"), CompactJson.IsValid() && CompactJson->HasField(TEXT("nodeCount")));
    if (CompactJson.IsValid() && CompactJson->HasField(TEXT("nodeCount")))
    {
        TestEqual(TEXT("compact nodeCount equals full node count"),
            static_cast<int32>(CompactJson->GetNumberField(TEXT("nodeCount"))), TotalNodes);
        TestEqual(TEXT("compact returnedNodeCount equals full node count (no filter)"),
            static_cast<int32>(CompactJson->GetNumberField(TEXT("returnedNodeCount"))), TotalNodes);
    }

    // nodeIds filter: only the requested node is returned; header still reports the full count.
    MetaSoundDumpBuilder::FMetaSoundDumpOptions FilterOpts;
    FilterOpts.NodeIdFilter.Add(FirstNodeId);
    TSharedPtr<FJsonObject> FilterJson = MetaSoundDumpBuilder::BuildMetaSoundJson(Source, FilterOpts);
    const TArray<TSharedPtr<FJsonValue>>* FilterNodes = NodesOf(FilterJson);
    TestNotNull(TEXT("filtered nodes array present"), FilterNodes);
    if (FilterNodes)
    {
        TestEqual(TEXT("nodeIds returns exactly the requested node"), FilterNodes->Num(), 1);
        if (FilterNodes->Num() == 1)
        {
            FString GotId;
            const TSharedPtr<FJsonObject> Only = (*FilterNodes)[0]->AsObject();
            TestTrue(TEXT("filtered node id matches request"),
                Only.IsValid() && Only->TryGetStringField(TEXT("id"), GotId) && GotId == FirstNodeId);
        }
    }
    TestTrue(TEXT("filtered snapshot carries the summary header"),
        FilterJson.IsValid() && FilterJson->HasField(TEXT("nodeCount")));
    if (FilterJson.IsValid() && FilterJson->HasField(TEXT("nodeCount")))
    {
        TestEqual(TEXT("filtered nodeCount still reports full graph size"),
            static_cast<int32>(FilterJson->GetNumberField(TEXT("nodeCount"))), TotalNodes);
        TestEqual(TEXT("filtered returnedNodeCount is 1"),
            static_cast<int32>(FilterJson->GetNumberField(TEXT("returnedNodeCount"))), 1);
    }

    Source->RemoveFromRoot();
    return true;
}

#if PW_METASOUND_HAS_SEARCH_ENGINE
// Regression guard for E-describe-metasound-interface-vertices-unflagged: every
// rootGraph interface output must carry an isInterfaceMember flag, the auto-attached
// interface vertex must be flagged true (with an owning interfaceName), and the
// user-added output must be flagged false. Before the fix BuildClassOutputJson emitted
// no membership field at all, so each TryGetBoolField below would fail.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMetaSoundDumpBuilderFlagsInterfaceMembersTest,
    "PinWright.Assets.MetaSound.DumpBuilder.FlagsInterfaceMembers",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMetaSoundDumpBuilderFlagsInterfaceMembersTest::RunTest(const FString& Parameters)
{
    const FName UserOutputName(TEXT("MyUserOut"));
    UMetaSoundSource* Source = NewTransientSourceWithInterfaceAndUserOutput(UserOutputName);
    TestNotNull(TEXT("Transient MetaSoundSource created"), Source);
    if (!Source)
    {
        return false;
    }

    TSharedPtr<FJsonObject> Json = MetaSoundDumpBuilder::BuildMetaSoundJson(Source);
    TestTrue(TEXT("BuildMetaSoundJson returns non-null"), Json.IsValid());
    if (!Json.IsValid())
    {
        Source->RemoveFromRoot();
        return false;
    }

    const TSharedPtr<FJsonObject>* RootGraph = nullptr;
    const TSharedPtr<FJsonObject>* InterfaceObj = nullptr;
    const TArray<TSharedPtr<FJsonValue>>* Outputs = nullptr;
    const bool bShape = Json->TryGetObjectField(TEXT("rootGraph"), RootGraph)
        && (*RootGraph)->TryGetObjectField(TEXT("interface"), InterfaceObj)
        && (*InterfaceObj)->TryGetArrayField(TEXT("outputs"), Outputs);
    TestTrue(TEXT("rootGraph.interface.outputs present"), bShape);
    if (!bShape)
    {
        Source->RemoveFromRoot();
        return false;
    }

    bool bFoundInterfaceMember = false;
    bool bFoundUserOutput = false;
    bool bUserOutputFlaggedFalse = false;
    for (const TSharedPtr<FJsonValue>& Value : *Outputs)
    {
        const TSharedPtr<FJsonObject> Obj = Value.IsValid() ? Value->AsObject() : nullptr;
        if (!Obj.IsValid())
        {
            continue;
        }

        FString Name;
        Obj->TryGetStringField(TEXT("name"), Name);

        bool bIsMember = false;
        const bool bHasFlag = Obj->TryGetBoolField(TEXT("isInterfaceMember"), bIsMember);
        TestTrue(FString::Printf(TEXT("output '%s' carries isInterfaceMember"), *Name), bHasFlag);

        if (Name == UserOutputName.ToString())
        {
            bFoundUserOutput = true;
            bUserOutputFlaggedFalse = bHasFlag && !bIsMember;
        }
        else if (bHasFlag && bIsMember)
        {
            bFoundInterfaceMember = true;
            FString InterfaceName;
            TestTrue(TEXT("interface member carries a non-empty interfaceName"),
                Obj->TryGetStringField(TEXT("interfaceName"), InterfaceName) && !InterfaceName.IsEmpty());
        }
    }

    TestTrue(TEXT("user-added output present"), bFoundUserOutput);
    TestTrue(TEXT("user-added output flagged isInterfaceMember=false"), bUserOutputFlaggedFalse);
    TestTrue(TEXT("at least one auto-attached interface output flagged isInterfaceMember=true"),
        bFoundInterfaceMember);

    Source->RemoveFromRoot();
    return bFoundUserOutput && bUserOutputFlaggedFalse && bFoundInterfaceMember;
}
#endif // PW_METASOUND_HAS_SEARCH_ENGINE

#endif // __has_include("MetasoundSource.h")
