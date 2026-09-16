// Copyright (c) 2026 Alexander Penkin. MIT License.

// Tests for PwBuildInteractiveMusicGraph (AudioGen/PwMusicGraph.h).
//
// Every structural assertion here reads the FMetasoundFrontendDocument through
// IMetaSoundDocumentInterface::GetConstDocument() - i.e. through the same const document
// describe_metasound reads, never through the builder that wrote the graph and never through
// the counters the function itself reported (rpc-design.md §4). Where a count IS taken from
// the result, the document count is asserted alongside it, so a report that drifted from the
// asset breaks the test rather than agreeing with itself.
//
// Fixture pattern follows Tests/Media/TestPwAudioDecode.cpp's MakeWaveWithPcm: a USoundWave
// carrying a SerializeWaveFile payload with USoundWave::NumChannels set (GetImportedSoundWaveData
// asserts check(NumChannels > 0), and NumChannels is also exactly what picks the Wave Player
// variant under test). These waves live in a /Game package rather than the transient one because
// the assembler resolves stems BY PATH.
//
// Teardown mirrors DiscardCreatedAssetByObjectPath in Tests/Media/TestAudioHandlers.cpp - that
// copy is file-local to an anonymous namespace there, so the technique is replicated rather
// than linked. It is NOT UEditorAssetLibrary::DeleteAsset / ObjectTools::ForceDeleteObjects:
// force-delete's referencer gathering serializes a freshly-created, never-reloaded
// UMetaSoundSource and crashes the suite under -unattended.

#include "Misc/AutomationTest.h"

#if __has_include("MetasoundSource.h") && __has_include("MetasoundFrontendDocumentBuilder.h") && __has_include("MetasoundFactory.h")

#include "AudioGen/PwMusicGraph.h"
#include "Compat/EngineVersionCompat.h"
#include "Handlers/Audio/MetaSound/MetaSoundPathUtils.h"
#include "Handlers/ErrorCodes.h"

#include "Audio.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Memory/SharedBuffer.h"
#include "MetasoundDocumentInterface.h"
#include "MetasoundFactory.h"
#include "MetasoundFrontendDocument.h"
#include "MetasoundSource.h"
#include "Misc/ScopeExit.h"
#include "Sound/SoundWave.h"
#include "UObject/GarbageCollection.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

// Named (not anonymous) namespace: the main module builds with Unity on and two anonymous
// namespaces merged into one TU collide by name. See CLAUDE.md > Building.
namespace PwMusicGraphTest
{
    const TCHAR* const TestFolder = TEXT("/Game/PinWrightTests");

    FString MakeUniqueName(const TCHAR* Prefix)
    {
        return FString::Printf(TEXT("%s_%s"), Prefix, *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    }

    // Detach, retag transient, rename into the transient package and collect - the way the
    // engine itself discards a never-saved asset.
    void DiscardAssetByObjectPath(const FString& ObjectPath)
    {
        UObject* Asset = StaticFindObject(UObject::StaticClass(), nullptr, *ObjectPath);
        if (!Asset)
        {
            return;
        }
        FAssetRegistryModule::AssetDeleted(Asset);
        Asset->ClearFlags(RF_Public | RF_Standalone);
        Asset->SetFlags(RF_Transient);
        Asset->Rename(nullptr, GetTransientPackage(),
            REN_DontCreateRedirectors | REN_NonTransactional | MCP_REN_NO_RESET_LOADERS);
        CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
    }

    // A path-resolvable USoundWave with a real RIFF payload and NumChannels set. Returns the
    // /Game object path the assembler is given as FPwMusicGraphStem::AssetPath.
    USoundWave* MakeStemWave(int32 NumChannels, FString& OutObjectPath)
    {
        const FString AssetName = MakeUniqueName(NumChannels == 2 ? TEXT("SW_MusicStemStereo") : TEXT("SW_MusicStemMono"));
        const FString PackageName = FString::Printf(TEXT("%s/%s"), TestFolder, *AssetName);
        UPackage* Package = CreatePackage(*PackageName);
        if (!Package)
        {
            return nullptr;
        }

        USoundWave* Wave = NewObject<USoundWave>(Package, USoundWave::StaticClass(), *AssetName,
            RF_Public | RF_Standalone | RF_Transient);
        if (!Wave)
        {
            return nullptr;
        }

        // NumChannels is the field the assembler reads to pick the Wave Player variant, and
        // GetImportedSoundWaveData asserts on it, so it is set explicitly rather than inferred.
        Wave->NumChannels = NumChannels;

        TArray<int16> Interleaved;
        Interleaved.AddZeroed(64 * NumChannels);

        TArray<uint8> WavBytes;
        SerializeWaveFile(WavBytes,
            reinterpret_cast<const uint8*>(Interleaved.GetData()),
            Interleaved.Num() * static_cast<int32>(sizeof(int16)),
            NumChannels, /*SampleRate=*/48000);
        Wave->RawData.UpdatePayload(FSharedBuffer::Clone(WavBytes.GetData(), WavBytes.Num()));

        Wave->AddToRoot();
        OutObjectPath = FString::Printf(TEXT("%s.%s"), *PackageName, *AssetName);
        return Wave;
    }

    UMetaSoundSource* FindBuiltSource(const FString& ObjectPath)
    {
        return Cast<UMetaSoundSource>(StaticFindObject(UMetaSoundSource::StaticClass(), nullptr, *ObjectPath));
    }

    // The read-back door: the const document, not the builder.
    const FMetasoundFrontendGraph* GetRootGraph(UMetaSoundSource* Source)
    {
        IMetaSoundDocumentInterface* DocInterface = Cast<IMetaSoundDocumentInterface>(Source);
        if (!DocInterface)
        {
            return nullptr;
        }
        const FMetasoundFrontendDocument& Doc = PW_METASOUND_GET_CONST_DOCUMENT(DocInterface);
        return PinWright::MetaSound::FindGraphClassConstGraph(Doc.RootGraph);
    }

    const FMetasoundFrontendDocument* GetDocument(UMetaSoundSource* Source)
    {
        IMetaSoundDocumentInterface* DocInterface = Cast<IMetaSoundDocumentInterface>(Source);
        if (!DocInterface)
        {
            return nullptr;
        }
        return &PW_METASOUND_GET_CONST_DOCUMENT(DocInterface);
    }

    const FMetasoundFrontendClassInput* FindGraphInput(const FMetasoundFrontendDocument& Doc, const TCHAR* Name)
    {
        const FMetasoundFrontendClassInterface& ClassInterface =
            PinWright::MetaSound::GetClassDefaultInterface(Doc.RootGraph);
        return ClassInterface.Inputs.FindByPredicate(
            [Name](const FMetasoundFrontendClassInput& Candidate) { return Candidate.Name == FName(Name); });
    }

    FString FindNodeClassName(const FMetasoundFrontendDocument& Doc, const FMetasoundFrontendNode& Node)
    {
        const FMetasoundFrontendClass* Class = Doc.Dependencies.FindByPredicate(
            [&Node](const FMetasoundFrontendClass& Candidate) { return Candidate.ID == Node.ClassID; });
        return Class ? Class->Metadata.GetClassName().ToString() : FString();
    }

    // Every node in the graph whose registry class name starts with Prefix, in document order.
    TArray<const FMetasoundFrontendNode*> FindNodesByClassPrefix(
        const FMetasoundFrontendDocument& Doc, const FMetasoundFrontendGraph& Graph, const TCHAR* Prefix)
    {
        TArray<const FMetasoundFrontendNode*> Matches;
        for (const FMetasoundFrontendNode& Node : Graph.Nodes)
        {
            if (FindNodeClassName(Doc, Node).StartsWith(Prefix))
            {
                Matches.Add(&Node);
            }
        }
        return Matches;
    }

    const FMetasoundFrontendVertex* FindNodeInput(const FMetasoundFrontendNode& Node, const TCHAR* PinName)
    {
        return Node.Interface.Inputs.FindByPredicate(
            [PinName](const FMetasoundFrontendVertex& Candidate) { return Candidate.Name == FName(PinName); });
    }

    const FMetasoundFrontendLiteral* FindNodeInputLiteral(const FMetasoundFrontendNode& Node, const TCHAR* PinName)
    {
        const FMetasoundFrontendVertex* Vertex = FindNodeInput(Node, PinName);
        if (!Vertex)
        {
            return nullptr;
        }
        for (const FMetasoundFrontendVertexLiteral& VertexLiteral : Node.InputLiterals)
        {
            if (VertexLiteral.VertexID == Vertex->VertexID)
            {
                return &VertexLiteral.Value;
            }
        }
        return nullptr;
    }

    // The node that feeds a given pin, resolved through the document's edge list.
    const FMetasoundFrontendNode* FindNodeFeeding(
        const FMetasoundFrontendGraph& Graph, const FMetasoundFrontendNode& Node, const TCHAR* PinName)
    {
        const FMetasoundFrontendVertex* Vertex = FindNodeInput(Node, PinName);
        if (!Vertex)
        {
            return nullptr;
        }
        for (const FMetasoundFrontendEdge& Edge : Graph.Edges)
        {
            if (Edge.ToNodeID == Node.GetID() && Edge.ToVertexID == Vertex->VertexID)
            {
                for (const FMetasoundFrontendNode& Candidate : Graph.Nodes)
                {
                    if (Candidate.GetID() == Edge.FromNodeID)
                    {
                        return &Candidate;
                    }
                }
            }
        }
        return nullptr;
    }

    // The graph-input CLASS entry a given input node belongs to, matched by node id - which is
    // how a Wave Player's "Wave Asset" feed is traced back to the wave it actually carries.
    const FMetasoundFrontendClassInput* FindGraphInputByNodeId(
        const FMetasoundFrontendDocument& Doc, const FGuid& NodeId)
    {
        const FMetasoundFrontendClassInterface& ClassInterface =
            PinWright::MetaSound::GetClassDefaultInterface(Doc.RootGraph);
        return ClassInterface.Inputs.FindByPredicate(
            [&NodeId](const FMetasoundFrontendClassInput& Candidate) { return Candidate.NodeID == NodeId; });
    }

    const TCHAR* const WavePlayerClassPrefix = TEXT("UE.Wave Player");
    const TCHAR* const MonoWavePlayerClassName = TEXT("UE.Wave Player.Mono");
    const TCHAR* const StereoWavePlayerClassName = TEXT("UE.Wave Player.Stereo");

    // A bare factory-created UMetaSoundSource carries interface nodes (On Play, On Finished,
    // the output-format audio output) before any music graph exists. Measuring that baseline
    // here rather than hardcoding it is what lets the count assertions be absolute document
    // counts instead of a re-statement of the numbers under test.
    struct FBaselineCounts
    {
        bool bMeasured = false;
        int32 Nodes = 0;
        int32 Edges = 0;
    };

    FBaselineCounts MeasureEmptySourceBaseline(FString& OutObjectPath)
    {
        FBaselineCounts Baseline;

        const FString AssetName = MakeUniqueName(TEXT("MS_MusicBaseline"));
        const FString PackageName = FString::Printf(TEXT("%s/%s"), TestFolder, *AssetName);
        UPackage* Package = CreatePackage(*PackageName);
        if (!Package)
        {
            return Baseline;
        }

        UMetaSoundSourceFactory* Factory = NewObject<UMetaSoundSourceFactory>();
        UMetaSoundSource* Source = Cast<UMetaSoundSource>(
            Factory->FactoryCreateNew(UMetaSoundSource::StaticClass(), Package, *AssetName,
                                      RF_Public | RF_Standalone, nullptr, GWarn));
        if (!Source)
        {
            return Baseline;
        }
        OutObjectPath = FString::Printf(TEXT("%s.%s"), *PackageName, *AssetName);

        if (const FMetasoundFrontendGraph* Graph = GetRootGraph(Source))
        {
            Baseline.Nodes = Graph->Nodes.Num();
            Baseline.Edges = Graph->Edges.Num();
            Baseline.bMeasured = true;
        }
        return Baseline;
    }
}

// =========================================================================
// A. Two mono stems: the document itself carries the expected node and edge counts,
//    and the reported counts agree with it.
//
//    The 13 nodes are: 4 control inputs (Intensity/Section/Play/Stop) + 2 WaveAsset stem
//    inputs + 2 Wave Players + 2 Map Range + 2 Multiply + 1 Audio Mixer.
//    The 15 edges are, per stem: Stem->Wave Asset, Play->Play, Stop->Stop, Intensity->In,
//    Player->PrimaryOperand, MapRange->AdditionalOperands, Multiply->mixer input (7 x 2),
//    plus the mixer's single output edge into the mono graph output.
// =========================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwMusicGraphTwoStemCountsTest,
    "PinWright.audio.music.graph.TwoStemCounts",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwMusicGraphTwoStemCountsTest::RunTest(const FString& Parameters)
{
    using namespace PwMusicGraphTest;

    FString StemPathA, StemPathB;
    USoundWave* WaveA = MakeStemWave(1, StemPathA);
    USoundWave* WaveB = MakeStemWave(1, StemPathB);
    TestNotNull(TEXT("mono stem A created"), WaveA);
    TestNotNull(TEXT("mono stem B created"), WaveB);
    if (!WaveA || !WaveB) return false;

    const FString AssetName = MakeUniqueName(TEXT("MS_MusicTwoStem"));
    FPwMusicGraphResult Result;
    FString ErrorCode, Error;
    FString BaselinePath;

    ON_SCOPE_EXIT
    {
        DiscardAssetByObjectPath(Result.AssetPath);
        DiscardAssetByObjectPath(BaselinePath);
        WaveA->RemoveFromRoot();
        WaveB->RemoveFromRoot();
        DiscardAssetByObjectPath(StemPathA);
        DiscardAssetByObjectPath(StemPathB);
    };

    // What an empty MetaSound Source already contains, measured rather than assumed.
    const FBaselineCounts Baseline = MeasureEmptySourceBaseline(BaselinePath);
    TestTrue(TEXT("the empty-source baseline was measured"), Baseline.bMeasured);
    if (!Baseline.bMeasured) return false;

    TArray<FPwMusicGraphStem> Stems;
    Stems.Add(FPwMusicGraphStem{ StemPathA, FString(), 0.0, 0.0, 0.0 });
    Stems.Add(FPwMusicGraphStem{ StemPathB, FString(), 0.0, 0.0, 0.5 });

    const bool bBuilt = PwBuildInteractiveMusicGraph(TestFolder, AssetName, Stems,
        /*bSaveToDisk=*/false, Result, ErrorCode, Error);
    TestTrue(FString::Printf(TEXT("build succeeded (code='%s', error='%s')"), *ErrorCode, *Error), bBuilt);
    if (!bBuilt) return false;

    TestTrue(TEXT("result is measured"), Result.bMeasured);

    UMetaSoundSource* Source = FindBuiltSource(Result.AssetPath);
    TestNotNull(TEXT("built MetaSound Source resolves by its reported path"), Source);
    if (!Source) return false;

    const FMetasoundFrontendGraph* Graph = GetRootGraph(Source);
    const FMetasoundFrontendDocument* Doc = GetDocument(Source);
    TestNotNull(TEXT("built asset exposes a root graph"), Graph);
    TestNotNull(TEXT("built asset exposes a document"), Doc);
    if (!Graph || !Doc) return false;

    // Both sides of each count: what the DOCUMENT holds over the measured empty-source
    // baseline, and what the function reported. A report that drifted from the asset fails
    // here rather than agreeing with itself.
    TestEqual(TEXT("the document gained 13 nodes for two mono stems"),
        Graph->Nodes.Num() - Baseline.Nodes, 13);
    TestEqual(TEXT("the document gained 15 edges for two mono stems"),
        Graph->Edges.Num() - Baseline.Edges, 15);
    TestEqual(TEXT("13 nodes reported added for two mono stems"), Result.NodesAdded, 13);
    TestEqual(TEXT("15 connections reported for two mono stems"), Result.ConnectionsMade, 15);

    // The same 13 broken down by class, so a miscount cannot be absorbed by the wrong node
    // being present in the right quantity.
    TestEqual(TEXT("document carries two Wave Player nodes"),
        FindNodesByClassPrefix(*Doc, *Graph, WavePlayerClassPrefix).Num(), 2);
    TestEqual(TEXT("document carries two Map Range nodes"),
        FindNodesByClassPrefix(*Doc, *Graph, TEXT("MapRange.MapRange")).Num(), 2);
    TestEqual(TEXT("document carries two Multiply nodes"),
        FindNodesByClassPrefix(*Doc, *Graph, TEXT("UE.Multiply")).Num(), 2);
    TestEqual(TEXT("document carries one Audio Mixer node"),
        FindNodesByClassPrefix(*Doc, *Graph, TEXT("AudioMixer.")).Num(), 1);

    TestEqual(TEXT("six graph inputs reported"), Result.GraphInputs.Num(), 6);
    TestTrue(TEXT("the unread Section input is reported as a warning"),
        Result.Warnings.ContainsByPredicate([](const FString& W) { return W.Contains(TEXT("Section")); }));

    return true;
}

// =========================================================================
// B. The four control inputs exist with the right DECLARED DATA TYPE. Trigger is asserted by
//    type name, not by literal kind: Metasound::FTrigger registers with ELiteralType::Boolean
//    (MetasoundPrimitives.cpp), so a Trigger input whose default literal is a bool is correct
//    and a bool-typed input would look identical if the literal were what was checked.
// =========================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwMusicGraphControlInputTypesTest,
    "PinWright.audio.music.graph.ControlInputTypes",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwMusicGraphControlInputTypesTest::RunTest(const FString& Parameters)
{
    using namespace PwMusicGraphTest;

    FString StemPathA, StemPathB;
    USoundWave* WaveA = MakeStemWave(1, StemPathA);
    USoundWave* WaveB = MakeStemWave(1, StemPathB);
    if (!WaveA || !WaveB) return false;

    const FString AssetName = MakeUniqueName(TEXT("MS_MusicControls"));
    FPwMusicGraphResult Result;
    FString ErrorCode, Error;

    ON_SCOPE_EXIT
    {
        DiscardAssetByObjectPath(Result.AssetPath);
        WaveA->RemoveFromRoot();
        WaveB->RemoveFromRoot();
        DiscardAssetByObjectPath(StemPathA);
        DiscardAssetByObjectPath(StemPathB);
    };

    TArray<FPwMusicGraphStem> Stems;
    Stems.Add(FPwMusicGraphStem{ StemPathA, TEXT("Bed"), 0.0, 0.0, 0.0 });
    Stems.Add(FPwMusicGraphStem{ StemPathB, TEXT("Lead"), 0.0, 0.0, 0.6 });

    const bool bBuilt = PwBuildInteractiveMusicGraph(TestFolder, AssetName, Stems,
        /*bSaveToDisk=*/false, Result, ErrorCode, Error);
    TestTrue(FString::Printf(TEXT("build succeeded (code='%s', error='%s')"), *ErrorCode, *Error), bBuilt);
    if (!bBuilt) return false;

    UMetaSoundSource* Source = FindBuiltSource(Result.AssetPath);
    if (!Source) return false;
    const FMetasoundFrontendDocument* Doc = GetDocument(Source);
    TestNotNull(TEXT("built asset exposes a document"), Doc);
    if (!Doc) return false;

    struct FCase { const TCHAR* Name; const TCHAR* Type; };
    const FCase Cases[] =
    {
        { PwMusicGraphIntensityInput, TEXT("Float") },
        { PwMusicGraphSectionInput,   TEXT("Int32") },
        { PwMusicGraphPlayInput,      TEXT("Trigger") },
        { PwMusicGraphStopInput,      TEXT("Trigger") },
        { TEXT("Bed"),                TEXT("WaveAsset") },
        { TEXT("Lead"),               TEXT("WaveAsset") },
    };

    for (const FCase& Case : Cases)
    {
        const FMetasoundFrontendClassInput* Input = FindGraphInput(*Doc, Case.Name);
        TestNotNull(*FString::Printf(TEXT("graph input '%s' exists"), Case.Name), Input);
        if (Input)
        {
            TestEqual(*FString::Printf(TEXT("graph input '%s' is declared as '%s'"), Case.Name, Case.Type),
                Input->TypeName.ToString(), FString(Case.Type));
        }
    }

    return true;
}

// =========================================================================
// C. Each Wave Player's "Wave Asset" pin is fed by its own stem's graph input, and that
//    input's stored default resolves to the USoundWave that was passed in. Traced through the
//    document's edge list and default literal - never through the arguments the caller gave.
// =========================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwMusicGraphWaveAssetBindingTest,
    "PinWright.audio.music.graph.WaveAssetBinding",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwMusicGraphWaveAssetBindingTest::RunTest(const FString& Parameters)
{
    using namespace PwMusicGraphTest;

    FString StemPathA, StemPathB;
    USoundWave* WaveA = MakeStemWave(1, StemPathA);
    USoundWave* WaveB = MakeStemWave(1, StemPathB);
    if (!WaveA || !WaveB) return false;

    const FString AssetName = MakeUniqueName(TEXT("MS_MusicBinding"));
    FPwMusicGraphResult Result;
    FString ErrorCode, Error;

    ON_SCOPE_EXIT
    {
        DiscardAssetByObjectPath(Result.AssetPath);
        WaveA->RemoveFromRoot();
        WaveB->RemoveFromRoot();
        DiscardAssetByObjectPath(StemPathA);
        DiscardAssetByObjectPath(StemPathB);
    };

    TArray<FPwMusicGraphStem> Stems;
    Stems.Add(FPwMusicGraphStem{ StemPathA, TEXT("Bed"), 0.0, 0.0, 0.0 });
    Stems.Add(FPwMusicGraphStem{ StemPathB, TEXT("Lead"), 0.0, 0.0, 0.6 });

    const bool bBuilt = PwBuildInteractiveMusicGraph(TestFolder, AssetName, Stems,
        /*bSaveToDisk=*/false, Result, ErrorCode, Error);
    TestTrue(FString::Printf(TEXT("build succeeded (code='%s', error='%s')"), *ErrorCode, *Error), bBuilt);
    if (!bBuilt) return false;

    UMetaSoundSource* Source = FindBuiltSource(Result.AssetPath);
    if (!Source) return false;
    const FMetasoundFrontendDocument* Doc = GetDocument(Source);
    const FMetasoundFrontendGraph* Graph = GetRootGraph(Source);
    if (!Doc || !Graph) return false;

    // Which wave each named input must carry. Both players are Mono here, so the players are
    // told apart by the input feeding them rather than by their class.
    TMap<FString, USoundWave*> ExpectedByInputName;
    ExpectedByInputName.Add(TEXT("Bed"), WaveA);
    ExpectedByInputName.Add(TEXT("Lead"), WaveB);

    const TArray<const FMetasoundFrontendNode*> Players =
        FindNodesByClassPrefix(*Doc, *Graph, WavePlayerClassPrefix);
    TestEqual(TEXT("two Wave Player nodes present"), Players.Num(), 2);

    int32 Matched = 0;
    for (const FMetasoundFrontendNode* Player : Players)
    {
        const FMetasoundFrontendNode* Feeder = FindNodeFeeding(*Graph, *Player, TEXT("Wave Asset"));
        TestNotNull(TEXT("Wave Player's 'Wave Asset' pin has an incoming edge"), Feeder);
        if (!Feeder)
        {
            continue;
        }

        const FMetasoundFrontendClassInput* StemInput = FindGraphInputByNodeId(*Doc, Feeder->GetID());
        TestNotNull(TEXT("the feeding node is a graph input"), StemInput);
        if (!StemInput)
        {
            continue;
        }

        USoundWave** Expected = ExpectedByInputName.Find(StemInput->Name.ToString());
        TestNotNull(*FString::Printf(TEXT("'%s' is one of the stem inputs"), *StemInput->Name.ToString()),
            Expected);
        if (!Expected)
        {
            continue;
        }

        const FMetasoundFrontendLiteral* Stored = PinWright::MetaSound::FindClassInputDefault(*StemInput);
        TestNotNull(TEXT("stem input carries a stored default literal"), Stored);
        if (!Stored)
        {
            continue;
        }

        UObject* StoredObject = nullptr;
        TestTrue(TEXT("stored default is a UObject literal"), Stored->TryGet(StoredObject));
        TestTrue(*FString::Printf(TEXT("'%s' carries the wave it was bound to (document holds '%s')"),
                *StemInput->Name.ToString(),
                StoredObject ? *StoredObject->GetPathName() : TEXT("no object")),
            StoredObject == static_cast<UObject*>(*Expected));
        if (StoredObject == static_cast<UObject*>(*Expected))
        {
            ++Matched;
        }
    }

    TestEqual(TEXT("both players resolved to their own stem's wave"), Matched, 2);
    return true;
}

// =========================================================================
// D. The silent-channel-loss guard. A stereo stem must select UE.Wave Player.Stereo and a mono
//    stem UE.Wave Player.Mono. A Mono player fed a stereo wave downmixes without erroring at
//    any layer, and no verb changes a node's class after creation - so this is the assertion
//    that a wrong variant is unrecoverable, not a cosmetic one.
// =========================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwMusicGraphWavePlayerVariantTest,
    "PinWright.audio.music.graph.WavePlayerVariantMatchesChannels",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwMusicGraphWavePlayerVariantTest::RunTest(const FString& Parameters)
{
    using namespace PwMusicGraphTest;

    FString MonoPath, StereoPath;
    USoundWave* MonoWave = MakeStemWave(1, MonoPath);
    USoundWave* StereoWave = MakeStemWave(2, StereoPath);
    if (!MonoWave || !StereoWave) return false;

    const FString AssetName = MakeUniqueName(TEXT("MS_MusicVariants"));
    FPwMusicGraphResult Result;
    FString ErrorCode, Error;

    ON_SCOPE_EXIT
    {
        DiscardAssetByObjectPath(Result.AssetPath);
        MonoWave->RemoveFromRoot();
        StereoWave->RemoveFromRoot();
        DiscardAssetByObjectPath(MonoPath);
        DiscardAssetByObjectPath(StereoPath);
    };

    TArray<FPwMusicGraphStem> Stems;
    Stems.Add(FPwMusicGraphStem{ MonoPath, TEXT("MonoStem"), 0.0, 0.0, 0.0 });
    Stems.Add(FPwMusicGraphStem{ StereoPath, TEXT("StereoStem"), 0.0, 0.0, 0.0 });

    const bool bBuilt = PwBuildInteractiveMusicGraph(TestFolder, AssetName, Stems,
        /*bSaveToDisk=*/false, Result, ErrorCode, Error);
    TestTrue(FString::Printf(TEXT("build succeeded (code='%s', error='%s')"), *ErrorCode, *Error), bBuilt);
    if (!bBuilt) return false;

    UMetaSoundSource* Source = FindBuiltSource(Result.AssetPath);
    if (!Source) return false;
    const FMetasoundFrontendDocument* Doc = GetDocument(Source);
    const FMetasoundFrontendGraph* Graph = GetRootGraph(Source);
    if (!Doc || !Graph) return false;

    const TArray<const FMetasoundFrontendNode*> Players =
        FindNodesByClassPrefix(*Doc, *Graph, WavePlayerClassPrefix);
    TestEqual(TEXT("two Wave Player nodes present"), Players.Num(), 2);

    // Which class each player has, keyed by the stem input feeding its Wave Asset pin.
    TMap<FString, FString> ClassByInputName;
    for (const FMetasoundFrontendNode* Player : Players)
    {
        const FMetasoundFrontendNode* Feeder = FindNodeFeeding(*Graph, *Player, TEXT("Wave Asset"));
        if (!Feeder)
        {
            continue;
        }
        if (const FMetasoundFrontendClassInput* StemInput = FindGraphInputByNodeId(*Doc, Feeder->GetID()))
        {
            ClassByInputName.Add(StemInput->Name.ToString(), FindNodeClassName(*Doc, *Player));
        }
    }

    const FString* MonoPlayerClass = ClassByInputName.Find(TEXT("MonoStem"));
    const FString* StereoPlayerClass = ClassByInputName.Find(TEXT("StereoStem"));
    TestNotNull(TEXT("the mono stem's player was found"), MonoPlayerClass);
    TestNotNull(TEXT("the stereo stem's player was found"), StereoPlayerClass);
    if (!MonoPlayerClass || !StereoPlayerClass) return false;

    TestEqual(TEXT("a 1-channel stem selects the Mono Wave Player"),
        *MonoPlayerClass, FString(MonoWavePlayerClassName));
    TestEqual(TEXT("a 2-channel stem selects the Stereo Wave Player - not the Mono one, which "
                   "would downmix it while every stage reported success"),
        *StereoPlayerClass, FString(StereoWavePlayerClassName));

    return true;
}

// =========================================================================
// E. Loop pins carry the supplied values. Loop must be true or the player ignores the other
//    two entirely, and a non-positive LoopDurationSeconds must land as the node's own
//    "whole asset" convention rather than as a zero-length loop.
// =========================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwMusicGraphLoopPinsTest,
    "PinWright.audio.music.graph.LoopPinsCarrySuppliedValues",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwMusicGraphLoopPinsTest::RunTest(const FString& Parameters)
{
    using namespace PwMusicGraphTest;

    FString StemPathA, StemPathB;
    USoundWave* WaveA = MakeStemWave(1, StemPathA);
    USoundWave* WaveB = MakeStemWave(1, StemPathB);
    if (!WaveA || !WaveB) return false;

    const FString AssetName = MakeUniqueName(TEXT("MS_MusicLoops"));
    FPwMusicGraphResult Result;
    FString ErrorCode, Error;

    ON_SCOPE_EXIT
    {
        DiscardAssetByObjectPath(Result.AssetPath);
        WaveA->RemoveFromRoot();
        WaveB->RemoveFromRoot();
        DiscardAssetByObjectPath(StemPathA);
        DiscardAssetByObjectPath(StemPathB);
    };

    TArray<FPwMusicGraphStem> Stems;
    Stems.Add(FPwMusicGraphStem{ StemPathA, TEXT("Bed"),  0.25, 3.5, 0.0 });
    // Zero duration means "loop the whole asset", which the node spells as a negative value.
    Stems.Add(FPwMusicGraphStem{ StemPathB, TEXT("Lead"), 0.0,  0.0, 0.5 });

    const bool bBuilt = PwBuildInteractiveMusicGraph(TestFolder, AssetName, Stems,
        /*bSaveToDisk=*/false, Result, ErrorCode, Error);
    TestTrue(FString::Printf(TEXT("build succeeded (code='%s', error='%s')"), *ErrorCode, *Error), bBuilt);
    if (!bBuilt) return false;

    UMetaSoundSource* Source = FindBuiltSource(Result.AssetPath);
    if (!Source) return false;
    const FMetasoundFrontendDocument* Doc = GetDocument(Source);
    const FMetasoundFrontendGraph* Graph = GetRootGraph(Source);
    if (!Doc || !Graph) return false;

    TMap<FString, const FMetasoundFrontendNode*> PlayerByInputName;
    for (const FMetasoundFrontendNode* Player : FindNodesByClassPrefix(*Doc, *Graph, WavePlayerClassPrefix))
    {
        if (const FMetasoundFrontendNode* Feeder = FindNodeFeeding(*Graph, *Player, TEXT("Wave Asset")))
        {
            if (const FMetasoundFrontendClassInput* StemInput = FindGraphInputByNodeId(*Doc, Feeder->GetID()))
            {
                PlayerByInputName.Add(StemInput->Name.ToString(), Player);
            }
        }
    }

    struct FCase { const TCHAR* InputName; float LoopStart; float LoopDuration; };
    const FCase Cases[] =
    {
        { TEXT("Bed"),  0.25f, 3.5f },
        { TEXT("Lead"), 0.0f,  static_cast<float>(PwMusicGraphLoopWholeAsset) },
    };

    for (const FCase& Case : Cases)
    {
        const FMetasoundFrontendNode* const* Player = PlayerByInputName.Find(Case.InputName);
        TestNotNull(*FString::Printf(TEXT("'%s' has a Wave Player"), Case.InputName), Player);
        if (!Player) continue;

        FMetasoundFrontendLiteral ExpectedLoop;
        ExpectedLoop.Set(true);
        const FMetasoundFrontendLiteral* StoredLoop = FindNodeInputLiteral(**Player, TEXT("Loop"));
        TestNotNull(*FString::Printf(TEXT("'%s' Wave Player has a Loop literal"), Case.InputName), StoredLoop);
        if (StoredLoop)
        {
            TestTrue(*FString::Printf(TEXT("'%s' Wave Player loops (%s)"), Case.InputName, *StoredLoop->ToString()),
                StoredLoop->IsEqual(ExpectedLoop));
        }

        FMetasoundFrontendLiteral ExpectedStart;
        ExpectedStart.Set(Case.LoopStart);
        const FMetasoundFrontendLiteral* StoredStart = FindNodeInputLiteral(**Player, TEXT("Loop Start"));
        TestNotNull(*FString::Printf(TEXT("'%s' Wave Player has a Loop Start literal"), Case.InputName), StoredStart);
        if (StoredStart)
        {
            TestTrue(*FString::Printf(TEXT("'%s' Loop Start is %.4f (document holds %s)"),
                    Case.InputName, Case.LoopStart, *StoredStart->ToString()),
                StoredStart->IsEqual(ExpectedStart));
        }

        FMetasoundFrontendLiteral ExpectedDuration;
        ExpectedDuration.Set(Case.LoopDuration);
        const FMetasoundFrontendLiteral* StoredDuration = FindNodeInputLiteral(**Player, TEXT("Loop Duration"));
        TestNotNull(*FString::Printf(TEXT("'%s' Wave Player has a Loop Duration literal"), Case.InputName), StoredDuration);
        if (StoredDuration)
        {
            TestTrue(*FString::Printf(TEXT("'%s' Loop Duration is %.4f (document holds %s)"),
                    Case.InputName, Case.LoopDuration, *StoredDuration->ToString()),
                StoredDuration->IsEqual(ExpectedDuration));
        }
    }

    return true;
}

// =========================================================================
// F. Failure direction (rpc-design.md §12). Each rejection asserts the CODE and that no asset
//    was left behind - a build that half-created an asset before bailing would otherwise be
//    indistinguishable from one that refused cleanly.
// =========================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwMusicGraphRejectionsLeaveNoAssetTest,
    "PinWright.audio.music.graph.RejectionsLeaveNoAsset",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwMusicGraphRejectionsLeaveNoAssetTest::RunTest(const FString& Parameters)
{
    using namespace PwMusicGraphTest;

    FString GoodStemPath;
    USoundWave* GoodWave = MakeStemWave(1, GoodStemPath);
    if (!GoodWave) return false;
    ON_SCOPE_EXIT
    {
        GoodWave->RemoveFromRoot();
        DiscardAssetByObjectPath(GoodStemPath);
    };

    // --- F1. A stem path that resolves to nothing. ---
    {
        const FString AssetName = MakeUniqueName(TEXT("MS_MusicBadStem"));
        TArray<FPwMusicGraphStem> Stems;
        Stems.Add(FPwMusicGraphStem{ GoodStemPath, FString(), 0.0, 0.0, 0.0 });
        Stems.Add(FPwMusicGraphStem{ TEXT("/Game/PinWrightTests/SW_DoesNotExist_ForMusicGraph"),
            FString(), 0.0, 0.0, 0.5 });

        FPwMusicGraphResult Result;
        FString ErrorCode, Error;
        const bool bBuilt = PwBuildInteractiveMusicGraph(TestFolder, AssetName, Stems,
            /*bSaveToDisk=*/false, Result, ErrorCode, Error);

        TestFalse(TEXT("an unresolvable stem fails the build"), bBuilt);
        TestFalse(TEXT("an unresolvable stem leaves the result unmeasured"), Result.bMeasured);
        TestEqual(TEXT("an unresolvable stem reports ASSET_NOT_FOUND"),
            ErrorCode, FString(ErrorCodes::ERR_ASSET_NOT_FOUND));
        TestTrue(TEXT("an unresolvable stem produces an explanation"), !Error.IsEmpty());
        TestNull(TEXT("an unresolvable stem leaves no MetaSound behind"),
            StaticFindObject(UObject::StaticClass(), nullptr,
                *FString::Printf(TEXT("%s/%s.%s"), TestFolder, *AssetName, *AssetName)));
    }

    // --- F2. An empty stem list: an error, not an empty success. ---
    {
        const FString AssetName = MakeUniqueName(TEXT("MS_MusicNoStems"));
        FPwMusicGraphResult Result;
        FString ErrorCode, Error;
        const bool bBuilt = PwBuildInteractiveMusicGraph(TestFolder, AssetName,
            TArray<FPwMusicGraphStem>(), /*bSaveToDisk=*/false, Result, ErrorCode, Error);

        TestFalse(TEXT("a zero-stem list fails the build"), bBuilt);
        TestFalse(TEXT("a zero-stem list leaves the result unmeasured"), Result.bMeasured);
        TestEqual(TEXT("a zero-stem list reports INVALID_ARGUMENT"),
            ErrorCode, FString(ErrorCodes::ERR_INVALID_ARGUMENT));
        TestNull(TEXT("a zero-stem list leaves no MetaSound behind"),
            StaticFindObject(UObject::StaticClass(), nullptr,
                *FString::Printf(TEXT("%s/%s.%s"), TestFolder, *AssetName, *AssetName)));
    }

    // --- F3. A package path outside /Game. ---
    {
        const FString AssetName = MakeUniqueName(TEXT("MS_MusicBadPath"));
        TArray<FPwMusicGraphStem> Stems;
        Stems.Add(FPwMusicGraphStem{ GoodStemPath, FString(), 0.0, 0.0, 0.0 });

        FPwMusicGraphResult Result;
        FString ErrorCode, Error;
        const bool bBuilt = PwBuildInteractiveMusicGraph(TEXT("/Engine/PinWrightTests"), AssetName,
            Stems, /*bSaveToDisk=*/false, Result, ErrorCode, Error);

        TestFalse(TEXT("an out-of-/Game package path fails the build"), bBuilt);
        TestFalse(TEXT("an out-of-/Game package path leaves the result unmeasured"), Result.bMeasured);
        TestEqual(TEXT("an out-of-/Game package path reports INVALID_ASSET_PATH"),
            ErrorCode, FString(ErrorCodes::ERR_INVALID_ASSET_PATH));
        TestNull(TEXT("an out-of-/Game package path leaves no MetaSound behind"),
            StaticFindObject(UObject::StaticClass(), nullptr,
                *FString::Printf(TEXT("/Engine/PinWrightTests/%s.%s"), *AssetName, *AssetName)));
    }

    return true;
}

// =========================================================================
// G. A re-run with the same name updates the same asset instead of creating a "_1" sibling.
//    The transport's response-only timeout means a client retry is normal traffic, so a build
//    that accumulated assets would quietly multiply them (rpc-design.md §8).
// =========================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwMusicGraphRerunIsIdempotentTest,
    "PinWright.audio.music.graph.RerunIsIdempotent",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwMusicGraphRerunIsIdempotentTest::RunTest(const FString& Parameters)
{
    using namespace PwMusicGraphTest;

    FString StemPathA, StemPathB;
    USoundWave* WaveA = MakeStemWave(1, StemPathA);
    USoundWave* WaveB = MakeStemWave(1, StemPathB);
    if (!WaveA || !WaveB) return false;

    const FString AssetName = MakeUniqueName(TEXT("MS_MusicRerun"));
    const FString ExpectedObjectPath = FString::Printf(TEXT("%s/%s.%s"), TestFolder, *AssetName, *AssetName);
    const FString SiblingObjectPath = FString::Printf(TEXT("%s/%s_1.%s_1"), TestFolder, *AssetName, *AssetName);

    ON_SCOPE_EXIT
    {
        DiscardAssetByObjectPath(ExpectedObjectPath);
        DiscardAssetByObjectPath(SiblingObjectPath);
        WaveA->RemoveFromRoot();
        WaveB->RemoveFromRoot();
        DiscardAssetByObjectPath(StemPathA);
        DiscardAssetByObjectPath(StemPathB);
    };

    TArray<FPwMusicGraphStem> Stems;
    Stems.Add(FPwMusicGraphStem{ StemPathA, FString(), 0.0, 0.0, 0.0 });
    Stems.Add(FPwMusicGraphStem{ StemPathB, FString(), 0.0, 0.0, 0.5 });

    FPwMusicGraphResult First;
    FString FirstCode, FirstError;
    const bool bFirst = PwBuildInteractiveMusicGraph(TestFolder, AssetName, Stems,
        /*bSaveToDisk=*/false, First, FirstCode, FirstError);
    TestTrue(FString::Printf(TEXT("first build succeeded (code='%s', error='%s')"), *FirstCode, *FirstError), bFirst);
    if (!bFirst) return false;

    FPwMusicGraphResult Second;
    FString SecondCode, SecondError;
    const bool bSecond = PwBuildInteractiveMusicGraph(TestFolder, AssetName, Stems,
        /*bSaveToDisk=*/false, Second, SecondCode, SecondError);
    TestTrue(FString::Printf(TEXT("second build succeeded (code='%s', error='%s')"), *SecondCode, *SecondError), bSecond);
    if (!bSecond) return false;

    TestEqual(TEXT("the re-run reports the same asset path"), Second.AssetPath, First.AssetPath);
    TestEqual(TEXT("the re-run reports the same node count"), Second.NodesAdded, First.NodesAdded);
    TestEqual(TEXT("the re-run reports the same connection count"), Second.ConnectionsMade, First.ConnectionsMade);
    TestNull(TEXT("the re-run created no '_1' sibling"),
        StaticFindObject(UObject::StaticClass(), nullptr, *SiblingObjectPath));

    // The document must be the rebuilt graph, not the first one with a second graph stacked on
    // top of it - which is what an in-place update that skipped the reconstruction would give.
    UMetaSoundSource* Source = FindBuiltSource(Second.AssetPath);
    TestNotNull(TEXT("the updated asset resolves"), Source);
    if (!Source) return false;

    const FMetasoundFrontendGraph* Graph = GetRootGraph(Source);
    TestNotNull(TEXT("the updated asset exposes a root graph"), Graph);
    if (!Graph) return false;

    const FMetasoundFrontendDocument* Doc = GetDocument(Source);
    TestNotNull(TEXT("the updated asset exposes a document"), Doc);
    if (!Doc) return false;

    // Doubled node counts are what an in-place update that appended instead of rebuilding
    // would produce, and the reported counts alone could not tell the two apart.
    TestEqual(TEXT("the updated document still carries exactly two Wave Players"),
        FindNodesByClassPrefix(*Doc, *Graph, WavePlayerClassPrefix).Num(), 2);
    TestEqual(TEXT("the updated document still carries exactly two Multiply nodes"),
        FindNodesByClassPrefix(*Doc, *Graph, TEXT("UE.Multiply")).Num(), 2);
    TestEqual(TEXT("the updated document still carries exactly one Audio Mixer node"),
        FindNodesByClassPrefix(*Doc, *Graph, TEXT("AudioMixer.")).Num(), 1);

    return true;
}

#endif // MetaSound headers present
