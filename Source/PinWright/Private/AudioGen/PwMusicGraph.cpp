// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "AudioGen/PwMusicGraph.h"

#include "Handlers/ErrorCodes.h"
#include "PinWrightHelpers.h"
#include "Utils/AssetCreatePolicy.h"
#include "Utils/AssetUtils.h"
#include "Utils/PathUtils.h"

#include "Misc/PackageName.h"
#include "Sound/SoundWave.h"
#include "UObject/Package.h"

// MetaSound availability is probed the same way AudioAuthoringHandler.cpp probes it, so a host
// without the plugin's headers compiles this file to a single honest rejection instead of
// failing to build.
#if __has_include("MetasoundSource.h") && __has_include("MetasoundFrontendDocumentBuilder.h") && __has_include("MetasoundFactory.h")
#include "MetasoundDocumentInterface.h"
#include "MetasoundFrontendDocument.h"
#include "MetasoundFrontendDocumentBuilder.h"
#include "MetasoundFactory.h"
#include "MetasoundSource.h"
#include "Interfaces/MetasoundOutputFormatInterfaces.h"

// Builder ctor / FinishBuilding / paged-graph accessors, plus PW_METASOUND_HAS_SEARCH_ENGINE.
#include "Handlers/Audio/MetaSound/MetaSoundLiteralFromTypeName.h"
#include "Handlers/Audio/MetaSound/MetaSoundPathUtils.h"

#define PW_MUSIC_GRAPH_SUPPORTED (MCP_HAS_METASOUND_LITERAL_HELPER && PW_METASOUND_HAS_SEARCH_ENGINE)
#else
#define PW_MUSIC_GRAPH_SUPPORTED 0
#endif

#if PW_MUSIC_GRAPH_SUPPORTED

// Named (not anonymous) namespace: the main module builds with Unity on and two anonymous
// namespaces merged into one TU collide by name. See CLAUDE.md > Building.
namespace PwMusicGraphInternal
{
    // ---------------------------------------------------------------------------------
    // Registry keys and pin names, all verified against UE 5.8 engine source. Nothing here
    // is a display name: these are the FVertexName / FNodeClassName values the frontend
    // indexes on, which is why they carry spaces exactly as the engine declares them.
    //   Wave Player   MetasoundWavePlayerNode.cpp:24-57 (vertex names), :147 (class name)
    //   Map Range     MetasoundMapRangeNode.cpp:19-29 (vertex names), :297 (class name)
    //   Multiply      MetasoundMathNodes.cpp:57-61 (vertex names), :46 + :1417 (class name)
    //   Audio Mixer   MetasoundMixerNode.cpp:261-338 (vertex names), :360 (class name)
    // ---------------------------------------------------------------------------------
    const FName WavePlayerNamespace(TEXT("UE"));
    const FName WavePlayerName(TEXT("Wave Player"));
    const FName WavePlayerMonoVariant(TEXT("Mono"));
    const FName WavePlayerStereoVariant(TEXT("Stereo"));

    const FName PinPlay(TEXT("Play"));
    const FName PinStop(TEXT("Stop"));
    const FName PinWaveAsset(TEXT("Wave Asset"));
    const FName PinLoop(TEXT("Loop"));
    const FName PinLoopStart(TEXT("Loop Start"));
    const FName PinLoopDuration(TEXT("Loop Duration"));
    const FName PinOutMono(TEXT("Out Mono"));
    const FName PinOutLeft(TEXT("Out Left"));
    const FName PinOutRight(TEXT("Out Right"));

    const FName MapRangeNamespace(TEXT("MapRange"));
    const FName MapRangeName(TEXT("MapRange"));
    const FName MapRangeFloatVariant(TEXT("Float"));

    const FName PinMapIn(TEXT("In"));
    const FName PinMapInRangeA(TEXT("In Range A"));
    const FName PinMapInRangeB(TEXT("In Range B"));
    const FName PinMapOutRangeA(TEXT("Out Range A"));
    const FName PinMapOutRangeB(TEXT("Out Range B"));
    const FName PinMapClamped(TEXT("Clamped"));
    const FName PinMapOutValue(TEXT("Out Value"));

    const FName MultiplyNamespace(TEXT("UE"));
    const FName MultiplyName(TEXT("Multiply"));
    const FName MultiplyAudioByFloatVariant(TEXT("Audio by Float"));

    const FName PinPrimaryOperand(TEXT("PrimaryOperand"));
    const FName PinAdditionalOperands(TEXT("AdditionalOperands"));
    const FName PinMultiplyOut(TEXT("Out"));

    const FName MixerNamespace(TEXT("AudioMixer"));

    // MetaSound data-type names for the graph inputs this assembler creates.
    const TCHAR* const TypeFloat = TEXT("Float");
    const TCHAR* const TypeInt32 = TEXT("Int32");
    const TCHAR* const TypeTrigger = TEXT("Trigger");
    const TCHAR* const TypeWaveAsset = TEXT("WaveAsset");

    // A stem after resolution, plus the document identities the build hands back to the
    // read-back pass. Every field is filled from a measurement, never from an argument echo.
    struct FResolvedStem
    {
        FString AssetPath;
        FName InputName;
        USoundWave* Wave = nullptr;
        int32 NumChannels = 0;

        // The literal actually bound to the stem's graph input, kept so the read-back can
        // compare the document's stored literal against it rather than against the path text.
        FMetasoundFrontendLiteral WaveLiteral;

        double LoopStartSeconds = 0.0;
        double LoopDurationSeconds = 0.0;
        double IntensityThreshold = 0.0;

        FMetasoundFrontendClassName PlayerClassName;

        FGuid InputNodeId;
        FGuid PlayerNodeId;
        FGuid MapRangeNodeId;
        // One per channel of the STEM (1 for mono, 2 for stereo), not of the graph.
        TArray<FGuid> MultiplyNodeIds;
    };

    bool Fail(const TCHAR* Code, FString Message, FString& OutErrorCode, FString& OutError)
    {
        OutErrorCode = Code;
        OutError = MoveTemp(Message);
        return false;
    }

    // Confirms a dotted registry key really names a registered node class BEFORE the build
    // commits to it. The alternative - letting AddNodeByClassName return null - only fails
    // after nodes have already been added, and there is no verb that changes a node's class
    // afterwards, so an unconfirmed key is an unfixable asset.
    bool IsRegisteredNodeClass(const FMetasoundFrontendClassName& ClassName)
    {
        FMetasoundFrontendClass Found;
        return Metasound::Frontend::ISearchEngine::Get().FindClassWithHighestVersion(ClassName, Found);
    }

    FMetasoundFrontendClassName MakeWavePlayerClassName(int32 NumChannels)
    {
        return FMetasoundFrontendClassName(
            WavePlayerNamespace, WavePlayerName,
            NumChannels == 2 ? WavePlayerStereoVariant : WavePlayerMonoVariant);
    }

    FMetasoundFrontendClassName MakeMixerClassName(int32 GraphChannels, int32 NumStems)
    {
        const FString OperatorName = FString::Printf(TEXT("Audio Mixer (%s, %d)"),
            GraphChannels == 2 ? TEXT("Stereo") : TEXT("Mono"), NumStems);
        return FMetasoundFrontendClassName(MixerNamespace, FName(*OperatorName), FName());
    }

    // "In 3" on a mono mixer, "In 3 L" / "In 3 R" on a stereo one.
    FName MakeMixerInputPin(int32 StemIndex, int32 GraphChannels, int32 ChannelIndex)
    {
        if (GraphChannels == 2)
        {
            return FName(*FString::Printf(TEXT("In %d %s"), StemIndex,
                ChannelIndex == 0 ? TEXT("L") : TEXT("R")));
        }
        return FName(*FString::Printf(TEXT("In %d"), StemIndex));
    }

    FName MakeMixerOutputPin(int32 GraphChannels, int32 ChannelIndex)
    {
        if (GraphChannels == 2)
        {
            return ChannelIndex == 0 ? FName(TEXT("Out L")) : FName(TEXT("Out R"));
        }
        return FName(TEXT("Out"));
    }

    // The wave player's audio output for one channel of ITS OWN format (not the graph's).
    FName MakePlayerOutputPin(int32 StemChannels, int32 ChannelIndex)
    {
        if (StemChannels == 2)
        {
            return ChannelIndex == 0 ? PinOutLeft : PinOutRight;
        }
        return PinOutMono;
    }

    FString JoinPinNames(const TArray<const FMetasoundFrontendVertex*>& Vertices)
    {
        TArray<FString> Names;
        Names.Reserve(Vertices.Num());
        for (const FMetasoundFrontendVertex* Vertex : Vertices)
        {
            if (Vertex)
            {
                Names.Add(Vertex->Name.ToString());
            }
        }
        return FString::Join(Names, TEXT(", "));
    }

    // Resolves a node input pin by name and reports the pins that DO exist when it is absent -
    // the failure mode this guards is an engine version renaming a pin, which would otherwise
    // surface as a silently unconnected input.
    bool FindInputVertexId(const FMetaSoundFrontendDocumentBuilder& Builder, const FGuid& NodeId,
                           FName PinName, const TCHAR* NodeDescription,
                           FGuid& OutVertexId, FString& OutErrorCode, FString& OutError)
    {
        if (const FMetasoundFrontendVertex* Vertex = Builder.FindNodeInput(NodeId, PinName))
        {
            OutVertexId = Vertex->VertexID;
            return true;
        }
        return Fail(ErrorCodes::ERR_INPUT_NOT_FOUND,
            FString::Printf(TEXT("PwBuildInteractiveMusicGraph: the %s node has no input pin '%s'. Its pins are: %s."),
                NodeDescription, *PinName.ToString(), *JoinPinNames(Builder.FindNodeInputs(NodeId))),
            OutErrorCode, OutError);
    }

    bool SetPinLiteral(FMetaSoundFrontendDocumentBuilder& Builder, const FGuid& NodeId, FName PinName,
                       const FMetasoundFrontendLiteral& Literal, const TCHAR* NodeDescription,
                       FString& OutErrorCode, FString& OutError)
    {
        FGuid VertexId;
        if (!FindInputVertexId(Builder, NodeId, PinName, NodeDescription, VertexId, OutErrorCode, OutError))
        {
            return false;
        }
        if (!Builder.SetNodeInputDefault(NodeId, VertexId, Literal))
        {
            return Fail(ErrorCodes::ERR_SET_DEFAULT_FAILED,
                FString::Printf(TEXT("PwBuildInteractiveMusicGraph: the builder rejected the literal for pin '%s' on the %s node."),
                    *PinName.ToString(), NodeDescription),
                OutErrorCode, OutError);
        }
        return true;
    }

    bool Connect(FMetaSoundFrontendDocumentBuilder& Builder,
                 const FGuid& FromNodeId, FName FromPin,
                 const FGuid& ToNodeId, FName ToPin,
                 const TCHAR* EdgeDescription, FString& OutErrorCode, FString& OutError)
    {
        TSet<Metasound::Frontend::FNamedEdge> Edges;
        Edges.Add(Metasound::Frontend::FNamedEdge{ FromNodeId, FromPin, ToNodeId, ToPin });

        TArray<const FMetasoundFrontendEdge*> Created;
        const bool bAccepted = Builder.AddNamedEdges(Edges, &Created, /*bReplaceExistingConnections=*/true);
        if (!bAccepted || Created.Num() != 1)
        {
            return Fail(ErrorCodes::ERR_EDGE_FAILED,
                FString::Printf(TEXT("PwBuildInteractiveMusicGraph: could not connect %s ('%s' -> '%s'); the builder %s and created %d edge(s)."),
                    EdgeDescription, *FromPin.ToString(), *ToPin.ToString(),
                    bAccepted ? TEXT("accepted the request") : TEXT("rejected the request"), Created.Num()),
                OutErrorCode, OutError);
        }
        return true;
    }

    // AddGraphInput with the paged-default write the engine actually reads. Writing
    // FMetasoundFrontendClassInput::DefaultLiteral instead (deprecated in 5.5) leaves Defaults
    // empty and AddGraphInput then hard-asserts inside FindConstDefaultChecked.
    const FMetasoundFrontendNode* AddGraphInputWithDefault(
        FMetaSoundFrontendDocumentBuilder& Builder, FName InputName, const FString& TypeName,
        const FMetasoundFrontendLiteral& Literal)
    {
        FMetasoundFrontendClassInput ClassInput;
        ClassInput.Name = InputName;
        ClassInput.TypeName = FName(*PinWright::MetaSound::CanonicalizeMetaSoundTypeName(TypeName));
        ClassInput.VertexID = FGuid::NewGuid();
        ClassInput.NodeID = FGuid::NewGuid();
        ClassInput.AccessType = EMetasoundFrontendVertexAccessType::Reference;
        PinWright::MetaSound::SetClassInputDefault(ClassInput, Literal);
        return Builder.AddGraphInput(MoveTemp(ClassInput));
    }

    bool AddControlInput(FMetaSoundFrontendDocumentBuilder& Builder, FName InputName, const FString& TypeName,
                         TArray<FString>& OutInputNames, FString& OutErrorCode, FString& OutError)
    {
        FMetasoundFrontendLiteral Literal;
        if (!PinWright::MetaSound::MakeDefaultLiteralForMetaSoundType(TypeName, Literal))
        {
            return Fail(ErrorCodes::ERR_INVALID_TYPE,
                FString::Printf(TEXT("PwBuildInteractiveMusicGraph: '%s' names no registered MetaSound data type, so graph input '%s' cannot be created."),
                    *TypeName, *InputName.ToString()),
                OutErrorCode, OutError);
        }
        if (!AddGraphInputWithDefault(Builder, InputName, TypeName, Literal))
        {
            return Fail(ErrorCodes::ERR_INPUT_FAILED,
                FString::Printf(TEXT("PwBuildInteractiveMusicGraph: the builder refused graph input '%s' of type '%s'."),
                    *InputName.ToString(), *TypeName),
                OutErrorCode, OutError);
        }
        OutInputNames.Add(InputName.ToString());
        return true;
    }

    // Document-side counters. Both are read off the const document rather than off the
    // builder, so a builder call that silently no-ops cannot be scored as work (§1).
    struct FDocumentCounts
    {
        int32 Nodes = 0;
        int32 Edges = 0;
    };

    bool MeasureDocumentCounts(UObject* Asset, FDocumentCounts& Out)
    {
        IMetaSoundDocumentInterface* DocInterface = Cast<IMetaSoundDocumentInterface>(Asset);
        if (!DocInterface)
        {
            return false;
        }
        const FMetasoundFrontendDocument& Doc = PW_METASOUND_GET_CONST_DOCUMENT(DocInterface);
        const FMetasoundFrontendGraph* Graph = PinWright::MetaSound::FindGraphClassConstGraph(Doc.RootGraph);
        if (!Graph)
        {
            return false;
        }
        Out.Nodes = Graph->Nodes.Num();
        Out.Edges = Graph->Edges.Num();
        return true;
    }

    const FMetasoundFrontendNode* FindDocumentNode(const FMetasoundFrontendGraph& Graph, const FGuid& NodeId)
    {
        for (const FMetasoundFrontendNode& Node : Graph.Nodes)
        {
            if (Node.GetID() == NodeId)
            {
                return &Node;
            }
        }
        return nullptr;
    }

    const FMetasoundFrontendVertex* FindDocumentNodeInput(const FMetasoundFrontendNode& Node, FName PinName)
    {
        for (const FMetasoundFrontendVertex& Vertex : Node.Interface.Inputs)
        {
            if (Vertex.Name == PinName)
            {
                return &Vertex;
            }
        }
        return nullptr;
    }

    const FMetasoundFrontendLiteral* FindDocumentNodeInputLiteral(const FMetasoundFrontendNode& Node, const FGuid& VertexId)
    {
        for (const FMetasoundFrontendVertexLiteral& VertexLiteral : Node.InputLiterals)
        {
            if (VertexLiteral.VertexID == VertexId)
            {
                return &VertexLiteral.Value;
            }
        }
        return nullptr;
    }

    bool DocumentHasEdgeInto(const FMetasoundFrontendGraph& Graph, const FGuid& FromNodeId,
                             const FGuid& ToNodeId, const FGuid& ToVertexId)
    {
        for (const FMetasoundFrontendEdge& Edge : Graph.Edges)
        {
            if (Edge.FromNodeID == FromNodeId && Edge.ToNodeID == ToNodeId && Edge.ToVertexID == ToVertexId)
            {
                return true;
            }
        }
        return false;
    }
}

#endif // PW_MUSIC_GRAPH_SUPPORTED

bool PwBuildInteractiveMusicGraph(const FString& PackagePath, const FString& AssetName,
                                  const TArray<FPwMusicGraphStem>& Stems, bool bSaveToDisk,
                                  FPwMusicGraphResult& Out, FString& OutErrorCode, FString& OutError)
{
    // Failure is the default: the report is cleared on entry and `bMeasured` is set on the
    // single full-success path at the very end (rpc-design.md §1).
    Out = FPwMusicGraphResult();
    OutErrorCode.Reset();
    OutError.Reset();

#if !PW_MUSIC_GRAPH_SUPPORTED
    (void)PackagePath; (void)AssetName; (void)Stems; (void)bSaveToDisk;
    OutErrorCode = ErrorCodes::ERR_METASOUND_NOT_AVAILABLE;
    OutError = TEXT("PwBuildInteractiveMusicGraph: this editor has no MetaSound frontend builder, "
                    "factory or class-search engine, so no interactive-music graph can be assembled.");
    return false;
#else
    using namespace PwMusicGraphInternal;

    // -----------------------------------------------------------------------------
    // 1. Argument validation. Everything that can be rejected is rejected BEFORE any
    //    package or asset exists, so a bad call leaves nothing behind (§12).
    // -----------------------------------------------------------------------------
    if (Stems.Num() == 0)
    {
        return Fail(ErrorCodes::ERR_INVALID_ARGUMENT,
            TEXT("PwBuildInteractiveMusicGraph: the stem list is empty. A graph with no stems has "
                 "nothing to play and nothing for Intensity to act on, so it is an error rather than "
                 "an empty success."),
            OutErrorCode, OutError);
    }
    if (Stems.Num() > PwMusicGraphMaxStems)
    {
        return Fail(ErrorCodes::ERR_INVALID_ARGUMENT,
            FString::Printf(TEXT("PwBuildInteractiveMusicGraph: %d stems requested but the engine registers Audio Mixer nodes for 2..%d inputs only. Pre-mix stems, or build one graph per group."),
                Stems.Num(), PwMusicGraphMaxStems),
            OutErrorCode, OutError);
    }

    FString ValidatedPackageName;
    FString PathError;
    if (!ValidateAssetCreationPath(PackagePath, AssetName, ValidatedPackageName, PathError))
    {
        return Fail(ErrorCodes::ERR_INVALID_ASSET_PATH,
            FString::Printf(TEXT("PwBuildInteractiveMusicGraph: '%s' / '%s' is not a usable asset location: %s"),
                *PackagePath, *AssetName, *PathError),
            OutErrorCode, OutError);
    }

    const FString ValidatedFolder = FPackageName::GetLongPackagePath(ValidatedPackageName);
    if (!ValidatedFolder.Equals(TEXT("/Game")) && !ValidatedFolder.StartsWith(TEXT("/Game/")))
    {
        return Fail(ErrorCodes::ERR_INVALID_ASSET_PATH,
            FString::Printf(TEXT("PwBuildInteractiveMusicGraph: '%s' resolves to '%s', which is outside /Game. Music graphs are project content."),
                *PackagePath, *ValidatedFolder),
            OutErrorCode, OutError);
    }

    // -----------------------------------------------------------------------------
    // 2. Resolve every stem. A stem that does not resolve, is not a USoundWave, or whose
    //    channel count cannot be read is an ERROR - never a guess and never a downmix (§3).
    // -----------------------------------------------------------------------------
    TArray<FResolvedStem> Resolved;
    Resolved.Reserve(Stems.Num());

    TSet<FName> UsedInputNames;
    UsedInputNames.Add(FName(PwMusicGraphIntensityInput));
    UsedInputNames.Add(FName(PwMusicGraphSectionInput));
    UsedInputNames.Add(FName(PwMusicGraphPlayInput));
    UsedInputNames.Add(FName(PwMusicGraphStopInput));

    for (int32 StemIndex = 0; StemIndex < Stems.Num(); ++StemIndex)
    {
        const FPwMusicGraphStem& Stem = Stems[StemIndex];

        FResolvedStem Entry;
        Entry.AssetPath = Stem.AssetPath;
        Entry.InputName = Stem.InputName.IsEmpty()
            ? FName(*FString::Printf(TEXT("Stem%d"), StemIndex))
            : FName(*Stem.InputName);

        bool bNameAlreadyUsed = false;
        UsedInputNames.Add(Entry.InputName, &bNameAlreadyUsed);
        if (bNameAlreadyUsed)
        {
            return Fail(ErrorCodes::ERR_INVALID_ARGUMENT,
                FString::Printf(TEXT("PwBuildInteractiveMusicGraph: stem %d wants graph input '%s', which is already taken by another stem or by a reserved control input (Intensity, Section, Play, Stop). Graph input names are unique; the second one would resolve to the first."),
                    StemIndex, *Entry.InputName.ToString()),
                OutErrorCode, OutError);
        }

        if (Stem.LoopStartSeconds < 0.0)
        {
            return Fail(ErrorCodes::ERR_INVALID_ARGUMENT,
                FString::Printf(TEXT("PwBuildInteractiveMusicGraph: stem %d ('%s') has LoopStartSeconds %.6f. A negative loop start is not a seek backwards; it is a caller mistake, and clamping it to zero would move the loop silently."),
                    StemIndex, *Stem.AssetPath, Stem.LoopStartSeconds),
                OutErrorCode, OutError);
        }
        if (Stem.IntensityThreshold < 0.0 || Stem.IntensityThreshold > 1.0)
        {
            return Fail(ErrorCodes::ERR_INVALID_ARGUMENT,
                FString::Printf(TEXT("PwBuildInteractiveMusicGraph: stem %d ('%s') has IntensityThreshold %.6f, outside the [0, 1] Intensity range. Clamping it would produce a layer that either never comes in or is always on, reported as success."),
                    StemIndex, *Stem.AssetPath, Stem.IntensityThreshold),
                OutErrorCode, OutError);
        }

        // One resolver for path + class: the MetaSound data-type registry decides whether an
        // object is acceptable for a WaveAsset vertex, so the pairing is never hardcoded here.
        UObject* ResolvedObject = nullptr;
        FString ExpectedClassPath;
        const PinWright::MetaSound::EMetaSoundObjectLiteralResult ObjectResult =
            PinWright::MetaSound::MakeObjectLiteralForMetaSoundType(
                Stem.AssetPath, TypeWaveAsset, Entry.WaveLiteral, ResolvedObject, ExpectedClassPath);

        switch (ObjectResult)
        {
        case PinWright::MetaSound::EMetaSoundObjectLiteralResult::Ok:
            break;
        case PinWright::MetaSound::EMetaSoundObjectLiteralResult::PathRejected:
            return Fail(ErrorCodes::ERR_INVALID_ASSET_PATH,
                FString::Printf(TEXT("PwBuildInteractiveMusicGraph: stem %d's AssetPath '%s' is not a usable asset path; nothing was created."),
                    StemIndex, *Stem.AssetPath),
                OutErrorCode, OutError);
        case PinWright::MetaSound::EMetaSoundObjectLiteralResult::ObjectNotFound:
            return Fail(ErrorCodes::ERR_ASSET_NOT_FOUND,
                FString::Printf(TEXT("PwBuildInteractiveMusicGraph: stem %d's AssetPath '%s' resolves to no loadable object; nothing was created."),
                    StemIndex, *Stem.AssetPath),
                OutErrorCode, OutError);
        case PinWright::MetaSound::EMetaSoundObjectLiteralResult::WrongClass:
        default:
            return Fail(ErrorCodes::ERR_INVALID_ASSET_TYPE,
                FString::Printf(TEXT("PwBuildInteractiveMusicGraph: stem %d's AssetPath '%s' is a %s, but a Wave Player's Wave Asset pin requires %s; nothing was created."),
                    StemIndex, *Stem.AssetPath,
                    ResolvedObject ? *ResolvedObject->GetClass()->GetPathName() : TEXT("different class"),
                    ExpectedClassPath.IsEmpty() ? TEXT("a USoundWave") : *ExpectedClassPath),
                OutErrorCode, OutError);
        }

        Entry.Wave = Cast<USoundWave>(ResolvedObject);
        if (!Entry.Wave)
        {
            return Fail(ErrorCodes::ERR_INVALID_ASSET_TYPE,
                FString::Printf(TEXT("PwBuildInteractiveMusicGraph: stem %d's AssetPath '%s' passed the WaveAsset data-type check but is not a USoundWave, so its channel count cannot be read."),
                    StemIndex, *Stem.AssetPath),
                OutErrorCode, OutError);
        }

        Entry.NumChannels = Entry.Wave->NumChannels;
        if (Entry.NumChannels < 1 || Entry.NumChannels > 2)
        {
            // The channel count picks the Wave Player class, and no verb changes a node's
            // class after creation - so an unknown count cannot be defaulted to Mono. The
            // engine's own fallback (MetasoundRandomizerTemplate.cpp:96-100 warns and uses
            // Mono) is exactly the silent downmix this refuses to reproduce.
            return Fail(ErrorCodes::ERR_UNSUPPORTED_CHANNEL,
                FString::Printf(TEXT("PwBuildInteractiveMusicGraph: stem %d ('%s') reports NumChannels=%d. This assembler wires 1- and 2-channel stems only; a count outside that range cannot pick a Wave Player variant, and guessing Mono would silently downmix the stem."),
                    StemIndex, *Stem.AssetPath, Entry.NumChannels),
                OutErrorCode, OutError);
        }

        Entry.LoopStartSeconds = Stem.LoopStartSeconds;
        Entry.LoopDurationSeconds = Stem.LoopDurationSeconds > 0.0
            ? Stem.LoopDurationSeconds
            : PwMusicGraphLoopWholeAsset;
        Entry.IntensityThreshold = Stem.IntensityThreshold;
        Entry.PlayerClassName = MakeWavePlayerClassName(Entry.NumChannels);

        Resolved.Add(MoveTemp(Entry));
    }

    // The graph carries the widest stem, so no stem loses a channel to the graph either.
    int32 GraphChannels = 1;
    for (const FResolvedStem& Entry : Resolved)
    {
        GraphChannels = FMath::Max(GraphChannels, Entry.NumChannels);
    }

    // -----------------------------------------------------------------------------
    // 3. Confirm every node class against the live registry before creating anything.
    // -----------------------------------------------------------------------------
    const FMetasoundFrontendClassName MapRangeClassName(MapRangeNamespace, MapRangeName, MapRangeFloatVariant);
    const FMetasoundFrontendClassName MultiplyClassName(MultiplyNamespace, MultiplyName, MultiplyAudioByFloatVariant);
    const bool bUseMixer = Resolved.Num() > 1;
    const FMetasoundFrontendClassName MixerClassName = MakeMixerClassName(GraphChannels, Resolved.Num());

    {
        TArray<FMetasoundFrontendClassName> Required;
        Required.Add(MapRangeClassName);
        Required.Add(MultiplyClassName);
        if (bUseMixer)
        {
            Required.Add(MixerClassName);
        }
        for (const FResolvedStem& Entry : Resolved)
        {
            Required.AddUnique(Entry.PlayerClassName);
        }

        for (const FMetasoundFrontendClassName& ClassName : Required)
        {
            if (!IsRegisteredNodeClass(ClassName))
            {
                return Fail(ErrorCodes::ERR_NODE_CLASS_NOT_FOUND,
                    FString::Printf(TEXT("PwBuildInteractiveMusicGraph: '%s' is not in this editor's MetaSound class registry, so the graph cannot be assembled. List what is registered with audio.authoring.search_metasound_nodes."),
                        *ClassName.ToString()),
                    OutErrorCode, OutError);
            }
        }
    }

    // -----------------------------------------------------------------------------
    // 4. Claim the asset path (dialog-free) and create or reuse the MetaSound Source.
    // -----------------------------------------------------------------------------
    const AssetCreatePolicy::FResolution Resolution = AssetCreatePolicy::Resolve(
        ValidatedPackageName, AssetName, UMetaSoundSource::StaticClass(),
        /*bOverwriteRequested=*/false, /*bRequireExactClass=*/true);
    if (Resolution.IsRejected())
    {
        return Fail(*Resolution.ErrorCode, Resolution.ErrorMessage, OutErrorCode, OutError);
    }

    UPackage* Package = CreatePackage(*ValidatedPackageName);
    if (!Package)
    {
        return Fail(ErrorCodes::ERR_PACKAGE_ERROR,
            FString::Printf(TEXT("PwBuildInteractiveMusicGraph: CreatePackage('%s') returned null."), *ValidatedPackageName),
            OutErrorCode, OutError);
    }

    // UMetaSoundFactory produces a Patch; UMetaSoundSourceFactory is the Source factory, and a
    // Source is what a UAudioComponent can play and drive parameters on. Both the Create and
    // the UpdateInPlace branches run this same call: NewObject on an occupied name reconstructs
    // the existing same-class object in place, which is what makes a retried build converge on
    // one asset instead of accumulating "_1" siblings (§8).
    UMetaSoundSourceFactory* Factory = NewObject<UMetaSoundSourceFactory>();
    UMetaSoundSource* Source = Cast<UMetaSoundSource>(
        Factory->FactoryCreateNew(UMetaSoundSource::StaticClass(), Package, FName(*AssetName),
                                  RF_Public | RF_Standalone, nullptr, GWarn));
    if (!Source)
    {
        return Fail(ErrorCodes::ERR_CREATION_FAILED,
            FString::Printf(TEXT("PwBuildInteractiveMusicGraph: UMetaSoundSourceFactory produced no UMetaSoundSource at '%s'."),
                *ValidatedPackageName),
            OutErrorCode, OutError);
    }

    const FString CreatedAssetPath = Source->GetPathName();

    IMetaSoundDocumentInterface* DocInterface = Cast<IMetaSoundDocumentInterface>(Source);
    if (!DocInterface)
    {
        return Fail(ErrorCodes::ERR_METASOUND_DOCUMENT_INTERFACE_NOT_SUPPORTED,
            FString::Printf(TEXT("PwBuildInteractiveMusicGraph: '%s' does not implement IMetaSoundDocumentInterface, so it carries no graph to build."),
                *CreatedAssetPath),
            OutErrorCode, OutError);
    }

    // -----------------------------------------------------------------------------
    // 5. Build the graph.
    // -----------------------------------------------------------------------------
    TScriptInterface<IMetaSoundDocumentInterface> ScriptInterface(Source);
    PW_METASOUND_MAKE_BUILDER(Builder, ScriptInterface);

    // Output format first, because swapping it adds and removes graph output nodes. The node /
    // edge deltas reported below are measured AFTER this swap, so they count the music graph
    // and not the asset's channel format.
    TArray<FName> GraphOutputVertexNames;
    {
        using namespace Metasound::Engine;

        const EMetaSoundOutputAudioFormat DesiredFormat = (GraphChannels == 2)
            ? EMetaSoundOutputAudioFormat::Stereo
            : EMetaSoundOutputAudioFormat::Mono;

        const FOutputAudioFormatInfoMap& FormatMap = GetOutputAudioFormatInfo();
        const FOutputAudioFormatInfo* DesiredInfo = FormatMap.Find(DesiredFormat);
        if (!DesiredInfo)
        {
            PW_METASOUND_FINISH_BUILDING(Builder);
            return Fail(ErrorCodes::ERR_UNSUPPORTED_CHANNEL,
                FString::Printf(TEXT("PwBuildInteractiveMusicGraph: this editor declares no %d-channel MetaSound output format."), GraphChannels),
                OutErrorCode, OutError);
        }
        GraphOutputVertexNames = DesiredInfo->OutputVertexChannelOrder;

        const FMetasoundFrontendDocument& Doc = PW_METASOUND_GET_CONST_DOCUMENT(DocInterface);
        if (!Doc.Interfaces.Contains(DesiredInfo->InterfaceVersion))
        {
            // Same shape as UMetaSoundSourceBuilder::SetFormat: add the wanted format and
            // remove every other declared one, in a single ModifyInterfaces transaction.
            TArray<FMetasoundFrontendVersion> ToAdd;
            ToAdd.Add(DesiredInfo->InterfaceVersion);

            TArray<FMetasoundFrontendVersion> ToRemove;
            for (const FOutputAudioFormatInfoPair& Pair : FormatMap)
            {
                if (Doc.Interfaces.Contains(Pair.Value.InterfaceVersion) &&
                    !ToAdd.Contains(Pair.Value.InterfaceVersion))
                {
                    ToRemove.Add(Pair.Value.InterfaceVersion);
                }
            }

            Metasound::Frontend::FModifyInterfaceOptions Options(ToRemove, ToAdd);
#if WITH_EDITORONLY_DATA
            Options.bSetDefaultNodeLocations = true;
#endif
            if (!Builder.ModifyInterfaces(MoveTemp(Options)))
            {
                PW_METASOUND_FINISH_BUILDING(Builder);
                return Fail(ErrorCodes::ERR_UNSUPPORTED_CHANNEL,
                    FString::Printf(TEXT("PwBuildInteractiveMusicGraph: could not switch '%s' to the %d-channel output format; a stem would lose a channel, so the build stops here."),
                        *CreatedAssetPath, GraphChannels),
                    OutErrorCode, OutError);
            }
        }
    }

    FDocumentCounts Before;
    if (!MeasureDocumentCounts(Source, Before))
    {
        PW_METASOUND_FINISH_BUILDING(Builder);
        return Fail(ErrorCodes::ERR_INTERNAL_ERROR,
            FString::Printf(TEXT("PwBuildInteractiveMusicGraph: '%s' exposes no readable root graph, so nothing about the build could be measured."),
                *CreatedAssetPath),
            OutErrorCode, OutError);
    }

    // --- 5a. Control inputs, in a fixed order so GraphInputs is deterministic. Names are
    //     collected locally and only published on the success path, so a failed build cannot
    //     hand back half an interface listing beside bMeasured:false.
    TArray<FString> GraphInputNames;
    if (!AddControlInput(Builder, FName(PwMusicGraphIntensityInput), TypeFloat, GraphInputNames, OutErrorCode, OutError) ||
        !AddControlInput(Builder, FName(PwMusicGraphSectionInput), TypeInt32, GraphInputNames, OutErrorCode, OutError) ||
        !AddControlInput(Builder, FName(PwMusicGraphPlayInput), TypeTrigger, GraphInputNames, OutErrorCode, OutError) ||
        !AddControlInput(Builder, FName(PwMusicGraphStopInput), TypeTrigger, GraphInputNames, OutErrorCode, OutError))
    {
        PW_METASOUND_FINISH_BUILDING(Builder);
        return false;
    }

    const FMetasoundFrontendNode* IntensityNode = Builder.FindGraphInputNode(FName(PwMusicGraphIntensityInput));
    const FMetasoundFrontendNode* PlayNode = Builder.FindGraphInputNode(FName(PwMusicGraphPlayInput));
    const FMetasoundFrontendNode* StopNode = Builder.FindGraphInputNode(FName(PwMusicGraphStopInput));
    if (!IntensityNode || !PlayNode || !StopNode)
    {
        PW_METASOUND_FINISH_BUILDING(Builder);
        return Fail(ErrorCodes::ERR_INPUT_NOT_FOUND,
            TEXT("PwBuildInteractiveMusicGraph: the builder accepted the Intensity/Play/Stop graph inputs "
                 "but does not report a node for at least one of them, so nothing can be wired to it."),
            OutErrorCode, OutError);
    }
    const FGuid IntensityNodeId = IntensityNode->GetID();
    const FGuid PlayNodeId = PlayNode->GetID();
    const FGuid StopNodeId = StopNode->GetID();

    // --- 5b. Per stem: WaveAsset input, Wave Player, Map Range, Multiply per stem channel. ---
    for (FResolvedStem& Entry : Resolved)
    {
        const FMetasoundFrontendNode* StemInputNode =
            AddGraphInputWithDefault(Builder, Entry.InputName, TypeWaveAsset, Entry.WaveLiteral);
        if (!StemInputNode)
        {
            PW_METASOUND_FINISH_BUILDING(Builder);
            return Fail(ErrorCodes::ERR_INPUT_FAILED,
                FString::Printf(TEXT("PwBuildInteractiveMusicGraph: the builder refused WaveAsset graph input '%s' for stem '%s'."),
                    *Entry.InputName.ToString(), *Entry.AssetPath),
                OutErrorCode, OutError);
        }
        Entry.InputNodeId = StemInputNode->GetID();
        GraphInputNames.Add(Entry.InputName.ToString());

        const FMetasoundFrontendNode* PlayerNode =
            Builder.AddNodeByClassName(Entry.PlayerClassName, /*InMajorVersion=*/1, FGuid::NewGuid());
        if (!PlayerNode)
        {
            PW_METASOUND_FINISH_BUILDING(Builder);
            return Fail(ErrorCodes::ERR_ADD_NODE_FAILED,
                FString::Printf(TEXT("PwBuildInteractiveMusicGraph: '%s' passed the registry check but the builder would not add it for stem '%s'."),
                    *Entry.PlayerClassName.ToString(), *Entry.AssetPath),
                OutErrorCode, OutError);
        }
        Entry.PlayerNodeId = PlayerNode->GetID();

        const FMetasoundFrontendNode* MapNode =
            Builder.AddNodeByClassName(MapRangeClassName, /*InMajorVersion=*/1, FGuid::NewGuid());
        if (!MapNode)
        {
            PW_METASOUND_FINISH_BUILDING(Builder);
            return Fail(ErrorCodes::ERR_ADD_NODE_FAILED,
                FString::Printf(TEXT("PwBuildInteractiveMusicGraph: the builder would not add '%s' for stem '%s'."),
                    *MapRangeClassName.ToString(), *Entry.AssetPath),
                OutErrorCode, OutError);
        }
        Entry.MapRangeNodeId = MapNode->GetID();

        for (int32 ChannelIndex = 0; ChannelIndex < Entry.NumChannels; ++ChannelIndex)
        {
            const FMetasoundFrontendNode* MultiplyNode =
                Builder.AddNodeByClassName(MultiplyClassName, /*InMajorVersion=*/1, FGuid::NewGuid());
            if (!MultiplyNode)
            {
                PW_METASOUND_FINISH_BUILDING(Builder);
                return Fail(ErrorCodes::ERR_ADD_NODE_FAILED,
                    FString::Printf(TEXT("PwBuildInteractiveMusicGraph: the builder would not add '%s' for channel %d of stem '%s'."),
                        *MultiplyClassName.ToString(), ChannelIndex, *Entry.AssetPath),
                    OutErrorCode, OutError);
            }
            Entry.MultiplyNodeIds.Add(MultiplyNode->GetID());
        }
    }

    // --- 5c. The mixer, when there is more than one stem to mix. ---
    FGuid MixerNodeId;
    if (bUseMixer)
    {
        const FMetasoundFrontendNode* MixerNode =
            Builder.AddNodeByClassName(MixerClassName, /*InMajorVersion=*/1, FGuid::NewGuid());
        if (!MixerNode)
        {
            PW_METASOUND_FINISH_BUILDING(Builder);
            return Fail(ErrorCodes::ERR_ADD_NODE_FAILED,
                FString::Printf(TEXT("PwBuildInteractiveMusicGraph: the builder would not add '%s'."),
                    *MixerClassName.ToString()),
                OutErrorCode, OutError);
        }
        MixerNodeId = MixerNode->GetID();
    }

    // --- 5d. Node-input literals: loop points and the intensity window. ---
    for (const FResolvedStem& Entry : Resolved)
    {
        // Loop is what makes the Wave Player read Loop Start / Loop Duration at all.
        FMetasoundFrontendLiteral LoopLiteral;
        LoopLiteral.Set(true);
        FMetasoundFrontendLiteral LoopStartLiteral;
        LoopStartLiteral.Set(static_cast<float>(Entry.LoopStartSeconds));
        FMetasoundFrontendLiteral LoopDurationLiteral;
        LoopDurationLiteral.Set(static_cast<float>(Entry.LoopDurationSeconds));

        if (!SetPinLiteral(Builder, Entry.PlayerNodeId, PinLoop, LoopLiteral, TEXT("Wave Player"), OutErrorCode, OutError) ||
            !SetPinLiteral(Builder, Entry.PlayerNodeId, PinLoopStart, LoopStartLiteral, TEXT("Wave Player"), OutErrorCode, OutError) ||
            !SetPinLiteral(Builder, Entry.PlayerNodeId, PinLoopDuration, LoopDurationLiteral, TEXT("Wave Player"), OutErrorCode, OutError))
        {
            PW_METASOUND_FINISH_BUILDING(Builder);
            return false;
        }

        FMetasoundFrontendLiteral InRangeA;
        InRangeA.Set(static_cast<float>(Entry.IntensityThreshold));
        FMetasoundFrontendLiteral InRangeB;
        InRangeB.Set(static_cast<float>(Entry.IntensityThreshold + PwMusicGraphIntensityFadeWidth));
        FMetasoundFrontendLiteral OutRangeA;
        OutRangeA.Set(0.0f);
        FMetasoundFrontendLiteral OutRangeB;
        OutRangeB.Set(1.0f);
        FMetasoundFrontendLiteral ClampedLiteral;
        ClampedLiteral.Set(true);

        if (!SetPinLiteral(Builder, Entry.MapRangeNodeId, PinMapInRangeA, InRangeA, TEXT("Map Range"), OutErrorCode, OutError) ||
            !SetPinLiteral(Builder, Entry.MapRangeNodeId, PinMapInRangeB, InRangeB, TEXT("Map Range"), OutErrorCode, OutError) ||
            !SetPinLiteral(Builder, Entry.MapRangeNodeId, PinMapOutRangeA, OutRangeA, TEXT("Map Range"), OutErrorCode, OutError) ||
            !SetPinLiteral(Builder, Entry.MapRangeNodeId, PinMapOutRangeB, OutRangeB, TEXT("Map Range"), OutErrorCode, OutError) ||
            !SetPinLiteral(Builder, Entry.MapRangeNodeId, PinMapClamped, ClampedLiteral, TEXT("Map Range"), OutErrorCode, OutError))
        {
            PW_METASOUND_FINISH_BUILDING(Builder);
            return false;
        }
    }

    // --- 5e. Edges. ---
    for (int32 StemIndex = 0; StemIndex < Resolved.Num(); ++StemIndex)
    {
        const FResolvedStem& Entry = Resolved[StemIndex];
        const FString StemLabel = FString::Printf(TEXT("stem %d ('%s')"), StemIndex, *Entry.AssetPath);

        if (!Connect(Builder, Entry.InputNodeId, Entry.InputName, Entry.PlayerNodeId, PinWaveAsset,
                *StemLabel, OutErrorCode, OutError) ||
            !Connect(Builder, PlayNodeId, FName(PwMusicGraphPlayInput), Entry.PlayerNodeId, PinPlay,
                *StemLabel, OutErrorCode, OutError) ||
            !Connect(Builder, StopNodeId, FName(PwMusicGraphStopInput), Entry.PlayerNodeId, PinStop,
                *StemLabel, OutErrorCode, OutError) ||
            !Connect(Builder, IntensityNodeId, FName(PwMusicGraphIntensityInput), Entry.MapRangeNodeId, PinMapIn,
                *StemLabel, OutErrorCode, OutError))
        {
            PW_METASOUND_FINISH_BUILDING(Builder);
            return false;
        }

        for (int32 ChannelIndex = 0; ChannelIndex < Entry.MultiplyNodeIds.Num(); ++ChannelIndex)
        {
            const FGuid& MultiplyNodeId = Entry.MultiplyNodeIds[ChannelIndex];
            if (!Connect(Builder, Entry.PlayerNodeId, MakePlayerOutputPin(Entry.NumChannels, ChannelIndex),
                    MultiplyNodeId, PinPrimaryOperand, *StemLabel, OutErrorCode, OutError) ||
                !Connect(Builder, Entry.MapRangeNodeId, PinMapOutValue,
                    MultiplyNodeId, PinAdditionalOperands, *StemLabel, OutErrorCode, OutError))
            {
                PW_METASOUND_FINISH_BUILDING(Builder);
                return false;
            }
        }

        // A mono stem inside a stereo graph feeds its one scaled channel to both sides, i.e.
        // centered. That is a stated placement, not a dropped channel.
        for (int32 GraphChannelIndex = 0; GraphChannelIndex < GraphChannels; ++GraphChannelIndex)
        {
            const FGuid& SourceMultiply = Entry.MultiplyNodeIds[
                FMath::Min(GraphChannelIndex, Entry.MultiplyNodeIds.Num() - 1)];

            if (bUseMixer)
            {
                if (!Connect(Builder, SourceMultiply, PinMultiplyOut, MixerNodeId,
                        MakeMixerInputPin(StemIndex, GraphChannels, GraphChannelIndex),
                        *StemLabel, OutErrorCode, OutError))
                {
                    PW_METASOUND_FINISH_BUILDING(Builder);
                    return false;
                }
            }
            else
            {
                // Single stem: no mixer exists (the engine registers Audio Mixer nodes from two
                // inputs upward), so the multiply drives the graph output directly.
                if (!GraphOutputVertexNames.IsValidIndex(GraphChannelIndex))
                {
                    PW_METASOUND_FINISH_BUILDING(Builder);
                    return Fail(ErrorCodes::ERR_INTERNAL_ERROR,
                        FString::Printf(TEXT("PwBuildInteractiveMusicGraph: the %d-channel output format declares only %d output vertices."),
                            GraphChannels, GraphOutputVertexNames.Num()),
                        OutErrorCode, OutError);
                }
                const FName OutputName = GraphOutputVertexNames[GraphChannelIndex];
                const FMetasoundFrontendNode* OutputNode = Builder.FindGraphOutputNode(OutputName);
                if (!OutputNode)
                {
                    PW_METASOUND_FINISH_BUILDING(Builder);
                    return Fail(ErrorCodes::ERR_INPUT_NOT_FOUND,
                        FString::Printf(TEXT("PwBuildInteractiveMusicGraph: the document has no graph output node named '%s'."),
                            *OutputName.ToString()),
                        OutErrorCode, OutError);
                }
                if (!Connect(Builder, SourceMultiply, PinMultiplyOut, OutputNode->GetID(), OutputName,
                        *StemLabel, OutErrorCode, OutError))
                {
                    PW_METASOUND_FINISH_BUILDING(Builder);
                    return false;
                }
            }
        }
    }

    if (bUseMixer)
    {
        for (int32 GraphChannelIndex = 0; GraphChannelIndex < GraphChannels; ++GraphChannelIndex)
        {
            if (!GraphOutputVertexNames.IsValidIndex(GraphChannelIndex))
            {
                PW_METASOUND_FINISH_BUILDING(Builder);
                return Fail(ErrorCodes::ERR_INTERNAL_ERROR,
                    FString::Printf(TEXT("PwBuildInteractiveMusicGraph: the %d-channel output format declares only %d output vertices."),
                        GraphChannels, GraphOutputVertexNames.Num()),
                    OutErrorCode, OutError);
            }
            const FName OutputName = GraphOutputVertexNames[GraphChannelIndex];
            const FMetasoundFrontendNode* OutputNode = Builder.FindGraphOutputNode(OutputName);
            if (!OutputNode)
            {
                PW_METASOUND_FINISH_BUILDING(Builder);
                return Fail(ErrorCodes::ERR_INPUT_NOT_FOUND,
                    FString::Printf(TEXT("PwBuildInteractiveMusicGraph: the document has no graph output node named '%s'."),
                        *OutputName.ToString()),
                    OutErrorCode, OutError);
            }
            if (!Connect(Builder, MixerNodeId, MakeMixerOutputPin(GraphChannels, GraphChannelIndex),
                    OutputNode->GetID(), OutputName, TEXT("the mixer output"), OutErrorCode, OutError))
            {
                PW_METASOUND_FINISH_BUILDING(Builder);
                return false;
            }
        }
    }

    // Commit the builder's caches before anything reads the document or saves it - see
    // audio.authoring.metasound_gotchas ("FinishBuilding() must be called before save").
    PW_METASOUND_FINISH_BUILDING(Builder);

    // -----------------------------------------------------------------------------
    // 6. Read-back (§4). Nothing below asks the builder that wrote the graph; every
    //    assertion is against the const document, the same one describe_metasound and the
    //    MSIR decompiler read. A failure here fails the build.
    // -----------------------------------------------------------------------------
    const FMetasoundFrontendDocument& Doc = PW_METASOUND_GET_CONST_DOCUMENT(DocInterface);
    const FMetasoundFrontendGraph* Graph = PinWright::MetaSound::FindGraphClassConstGraph(Doc.RootGraph);
    if (!Graph)
    {
        return Fail(ErrorCodes::ERR_VERIFICATION_FAILED,
            FString::Printf(TEXT("PwBuildInteractiveMusicGraph: '%s' has no default-page root graph after the build."),
                *CreatedAssetPath),
            OutErrorCode, OutError);
    }

    // 6a. Graph inputs, by name AND declared data type.
    const FMetasoundFrontendClassInterface& ClassInterface =
        PinWright::MetaSound::GetClassDefaultInterface(Doc.RootGraph);

    auto VerifyGraphInput = [&ClassInterface](FName InputName, const TCHAR* ExpectedType) -> bool
    {
        for (const FMetasoundFrontendClassInput& ClassInput : ClassInterface.Inputs)
        {
            if (ClassInput.Name == InputName)
            {
                return ClassInput.TypeName == FName(ExpectedType);
            }
        }
        return false;
    };

    struct FExpectedInput { FName Name; const TCHAR* Type; };
    TArray<FExpectedInput> ExpectedInputs;
    ExpectedInputs.Add({ FName(PwMusicGraphIntensityInput), TypeFloat });
    ExpectedInputs.Add({ FName(PwMusicGraphSectionInput), TypeInt32 });
    ExpectedInputs.Add({ FName(PwMusicGraphPlayInput), TypeTrigger });
    ExpectedInputs.Add({ FName(PwMusicGraphStopInput), TypeTrigger });
    for (const FResolvedStem& Entry : Resolved)
    {
        ExpectedInputs.Add({ Entry.InputName, TypeWaveAsset });
    }

    for (const FExpectedInput& Expected : ExpectedInputs)
    {
        if (!VerifyGraphInput(Expected.Name, Expected.Type))
        {
            return Fail(ErrorCodes::ERR_VERIFICATION_FAILED,
                FString::Printf(TEXT("PwBuildInteractiveMusicGraph: the saved document of '%s' does not carry a graph input '%s' of type '%s'."),
                    *CreatedAssetPath, *Expected.Name.ToString(), Expected.Type),
                OutErrorCode, OutError);
        }
    }

    // 6b. Per stem: the player's class name, its Wave Asset feed, and its loop literals.
    for (int32 StemIndex = 0; StemIndex < Resolved.Num(); ++StemIndex)
    {
        const FResolvedStem& Entry = Resolved[StemIndex];

        const FMetasoundFrontendNode* PlayerNode = FindDocumentNode(*Graph, Entry.PlayerNodeId);
        if (!PlayerNode)
        {
            return Fail(ErrorCodes::ERR_VERIFICATION_FAILED,
                FString::Printf(TEXT("PwBuildInteractiveMusicGraph: stem %d's Wave Player node is absent from the built document."), StemIndex),
                OutErrorCode, OutError);
        }

        // The class name is the whole mono/stereo guard: read it from the document's dependency
        // table rather than from the name the builder was handed.
        const FMetasoundFrontendClass* PlayerClass = Doc.Dependencies.FindByPredicate(
            [&PlayerNode](const FMetasoundFrontendClass& Candidate) { return Candidate.ID == PlayerNode->ClassID; });
        if (!PlayerClass || PlayerClass->Metadata.GetClassName() != Entry.PlayerClassName)
        {
            return Fail(ErrorCodes::ERR_VERIFICATION_FAILED,
                FString::Printf(TEXT("PwBuildInteractiveMusicGraph: stem %d ('%s', %d channel(s)) should use '%s' but the document records '%s'. A wrong Wave Player variant silently up/downmixes the stem."),
                    StemIndex, *Entry.AssetPath, Entry.NumChannels, *Entry.PlayerClassName.ToString(),
                    PlayerClass ? *PlayerClass->Metadata.GetClassName().ToString() : TEXT("no class at all")),
                OutErrorCode, OutError);
        }

        const FMetasoundFrontendVertex* WaveAssetVertex = FindDocumentNodeInput(*PlayerNode, PinWaveAsset);
        if (!WaveAssetVertex ||
            !DocumentHasEdgeInto(*Graph, Entry.InputNodeId, Entry.PlayerNodeId, WaveAssetVertex->VertexID))
        {
            return Fail(ErrorCodes::ERR_VERIFICATION_FAILED,
                FString::Printf(TEXT("PwBuildInteractiveMusicGraph: stem %d's Wave Player has no edge from graph input '%s' into its '%s' pin, so the player would carry no wave."),
                    StemIndex, *Entry.InputName.ToString(), *PinWaveAsset.ToString()),
                OutErrorCode, OutError);
        }

        // The wave itself: the stem input's stored default literal, read off the document.
        const FMetasoundFrontendClassInput* StemClassInput = ClassInterface.Inputs.FindByPredicate(
            [&Entry](const FMetasoundFrontendClassInput& Candidate) { return Candidate.Name == Entry.InputName; });
        const FMetasoundFrontendLiteral* StoredWave = StemClassInput
            ? PinWright::MetaSound::FindClassInputDefault(*StemClassInput)
            : nullptr;
        UObject* StoredObject = nullptr;
        if (!StoredWave || !StoredWave->TryGet(StoredObject) || StoredObject != Entry.Wave)
        {
            return Fail(ErrorCodes::ERR_VERIFICATION_FAILED,
                FString::Printf(TEXT("PwBuildInteractiveMusicGraph: graph input '%s' should default to '%s' but the document holds '%s'."),
                    *Entry.InputName.ToString(), *Entry.AssetPath,
                    StoredObject ? *StoredObject->GetPathName() : TEXT("no object")),
                OutErrorCode, OutError);
        }

        struct FExpectedPin { FName Pin; FMetasoundFrontendLiteral Literal; };
        TArray<FExpectedPin> ExpectedPins;
        {
            FMetasoundFrontendLiteral LoopLiteral;
            LoopLiteral.Set(true);
            ExpectedPins.Add({ PinLoop, LoopLiteral });

            FMetasoundFrontendLiteral LoopStartLiteral;
            LoopStartLiteral.Set(static_cast<float>(Entry.LoopStartSeconds));
            ExpectedPins.Add({ PinLoopStart, LoopStartLiteral });

            FMetasoundFrontendLiteral LoopDurationLiteral;
            LoopDurationLiteral.Set(static_cast<float>(Entry.LoopDurationSeconds));
            ExpectedPins.Add({ PinLoopDuration, LoopDurationLiteral });
        }

        for (const FExpectedPin& Expected : ExpectedPins)
        {
            const FMetasoundFrontendVertex* Vertex = FindDocumentNodeInput(*PlayerNode, Expected.Pin);
            const FMetasoundFrontendLiteral* Stored = Vertex
                ? FindDocumentNodeInputLiteral(*PlayerNode, Vertex->VertexID)
                : nullptr;
            if (!Stored || !Stored->IsEqual(Expected.Literal))
            {
                return Fail(ErrorCodes::ERR_VERIFICATION_FAILED,
                    FString::Printf(TEXT("PwBuildInteractiveMusicGraph: stem %d's Wave Player pin '%s' should hold %s but the document holds %s."),
                        StemIndex, *Expected.Pin.ToString(), *Expected.Literal.ToString(),
                        Stored ? *Stored->ToString() : TEXT("no literal")),
                    OutErrorCode, OutError);
            }
        }
    }

    FDocumentCounts After;
    if (!MeasureDocumentCounts(Source, After))
    {
        return Fail(ErrorCodes::ERR_VERIFICATION_FAILED,
            FString::Printf(TEXT("PwBuildInteractiveMusicGraph: '%s' could not be re-measured after the build."), *CreatedAssetPath),
            OutErrorCode, OutError);
    }

    // -----------------------------------------------------------------------------
    // 7. Persistence. The report carries no persistence field, so a save that does not
    //    reach disk is a failure rather than a quiet downgrade (§5).
    // -----------------------------------------------------------------------------
    // Register the new object with the asset registry and mark the package dirty. This runs on
    // both branches, and it runs BEFORE the disk write because SaveAssetToDiskReportingPresence's
    // freshness gate reads the package's dirty flag going in - a clean package makes an unchanged
    // .uasset indistinguishable from a throttle-skipped save.
    McpSafeAssetSave(Source);

    if (bSaveToDisk && !SaveAssetToDiskReportingPresence(Source, /*bForce=*/true))
    {
        return Fail(ErrorCodes::ERR_SAVE_FAILED,
            FString::Printf(TEXT("PwBuildInteractiveMusicGraph: the graph was built and verified in memory but no .uasset reached disk for '%s'."),
                *CreatedAssetPath),
            OutErrorCode, OutError);
    }

    Out.AssetPath = CreatedAssetPath;
    Out.NodesAdded = After.Nodes - Before.Nodes;
    Out.ConnectionsMade = After.Edges - Before.Edges;
    Out.GraphInputs = MoveTemp(GraphInputNames);
    Out.Warnings.Add(FString::Printf(
        TEXT("Graph input '%s' (Int32) is exposed on the interface but nothing in this graph reads it. "
             "Set it from the game like any other parameter; branching on it needs a stem set per section, "
             "which this assembler does not build."),
        PwMusicGraphSectionInput));
    Out.bMeasured = true;
    return true;
#endif // PW_MUSIC_GRAPH_SUPPORTED
}
