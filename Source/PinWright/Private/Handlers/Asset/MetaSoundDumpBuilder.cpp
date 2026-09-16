// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Handlers/Asset/MetaSoundDumpBuilder.h"

#include "Dom/JsonValue.h"
// LogPinWrightSubsystem — the category every other Handlers/Asset builder logs through.
#include "PinWrightSubsystem.h"

#if __has_include("MetasoundFrontendDocument.h")
#include "MetasoundDocumentInterface.h"
#include "MetasoundFrontendDocument.h"
#include "MetasoundFrontendLiteral.h"
#include "MetasoundSource.h"
#include "Metasound.h"
// Shared 5.6 paged-graphs compat helpers (input-default / default-interface / default-graph access).
#include "Handlers/Audio/MetaSound/MetaSoundPathUtils.h"
#define MCP_DUMP_HAS_METASOUND_FRONTEND 1
#else
#define MCP_DUMP_HAS_METASOUND_FRONTEND 0
#endif

// isInterfaceMember tagging needs the MetaSound interface registry (search engine) to map
// an attached-interface version to its declared vertex names. MetaSoundPathUtils.h exports
// PW_METASOUND_HAS_SEARCH_ENGINE beside the version-safe FindAllFrontendInterfaces() for
// exactly this gate, so the membership code below keys off that shared predicate rather than
// re-testing __has_include here. Without the search engine the fields are omitted, not guessed.
#if MCP_DUMP_HAS_METASOUND_FRONTEND

namespace
{
    TSharedPtr<FJsonObject> BuildVertexJson(const FMetasoundFrontendVertex& Vertex)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetStringField(TEXT("name"), Vertex.Name.ToString());
        Obj->SetStringField(TEXT("typeName"), Vertex.TypeName.ToString());
        Obj->SetStringField(TEXT("vertexID"), Vertex.VertexID.ToString());
        return Obj;
    }

    // Tags a rootGraph interface vertex with isInterfaceMember (+ the owning
    // interface name) so a caller can tell auto-attached interface vertices
    // (e.g. UE.Source.OnPlay on a fresh UMetaSoundSource) apart from user-added
    // ones without pattern-matching UE.* names. OwnerMap is null only on engines
    // without the interface registry, where membership is not derivable — there
    // the fields are omitted rather than guessed.
    void AddInterfaceMembership(const TSharedPtr<FJsonObject>& Obj, const FName VertexName, const TMap<FName, FString>* OwnerMap)
    {
        if (!OwnerMap)
        {
            return;
        }
        const FString* OwningInterface = OwnerMap->Find(VertexName);
        Obj->SetBoolField(TEXT("isInterfaceMember"), OwningInterface != nullptr);
        if (OwningInterface)
        {
            Obj->SetStringField(TEXT("interfaceName"), *OwningInterface);
        }
    }

    TSharedPtr<FJsonObject> BuildClassInputJson(const FMetasoundFrontendClassInput& Input, const TMap<FName, FString>* InterfaceOwner)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetStringField(TEXT("name"), Input.Name.ToString());
        Obj->SetStringField(TEXT("typeName"), Input.TypeName.ToString());
        Obj->SetStringField(TEXT("vertexID"), Input.VertexID.ToString());
        // 5.6 paged input defaults; helper reads DefaultLiteral (5.4/5.5) or default page (5.6+).
        if (const FMetasoundFrontendLiteral* Default = PinWright::MetaSound::FindClassInputDefault(Input))
        {
            Obj->SetStringField(TEXT("defaultLiteral"), Default->ToString());
        }
        AddInterfaceMembership(Obj, Input.Name, InterfaceOwner);
        return Obj;
    }

    TSharedPtr<FJsonObject> BuildClassOutputJson(const FMetasoundFrontendClassOutput& Output, const TMap<FName, FString>* InterfaceOwner)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetStringField(TEXT("name"), Output.Name.ToString());
        Obj->SetStringField(TEXT("typeName"), Output.TypeName.ToString());
        Obj->SetStringField(TEXT("vertexID"), Output.VertexID.ToString());
        AddInterfaceMembership(Obj, Output.Name, InterfaceOwner);
        return Obj;
    }

#if PW_METASOUND_HAS_SEARCH_ENGINE
    // Maps each rootGraph interface vertex name to the attached interface that owns it.
    // A UMetaSoundSource ships with the standard source interfaces already attached
    // (UE.Source.OnPlay, UE.Source.OneShot.OnFinished, UE.OutputFormat.Mono.Audio, ...),
    // so the rootGraph interface arrays interleave those auto-attached vertices with
    // user-added ones. The document records its attached interfaces in Doc.Interfaces;
    // resolving each against the registry yields the declared vertex names that the
    // engine copied verbatim into the rootGraph interface, so a name match is exact.
    struct FInterfaceMembership
    {
        TMap<FName, FString> InputOwner;
        TMap<FName, FString> OutputOwner;
    };

    FInterfaceMembership BuildInterfaceMembership(const FMetasoundFrontendDocument& Doc)
    {
        FInterfaceMembership Membership;
        if (Doc.Interfaces.Num() == 0)
        {
            return Membership;
        }

        for (const FMetasoundFrontendInterface& Interface : PinWright::MetaSound::FindAllFrontendInterfaces())
        {
            const FMetasoundFrontendVersion& Version = PinWright::MetaSound::GetInterfaceVersion(Interface);
            if (!Doc.Interfaces.Contains(Version))
            {
                continue;
            }

            const FString InterfaceName = Version.Name.ToString();
            for (const FMetasoundFrontendClassInput& Input : Interface.Inputs)
            {
                Membership.InputOwner.Add(Input.Name, InterfaceName);
            }
            for (const FMetasoundFrontendClassOutput& Output : Interface.Outputs)
            {
                Membership.OutputOwner.Add(Output.Name, InterfaceName);
            }
        }
        return Membership;
    }
#endif // PW_METASOUND_HAS_SEARCH_ENGINE

    // Walks Node.InputLiterals once into a VertexID -> literal-string map so the per-input
    // emission stays O(N+M) rather than O(N*M) for nodes with many ports.
    TMap<FGuid, FString> BuildLiteralLookup(const FMetasoundFrontendNode& Node)
    {
        TMap<FGuid, FString> Lookup;
        Lookup.Reserve(Node.InputLiterals.Num());
        for (const FMetasoundFrontendVertexLiteral& Entry : Node.InputLiterals)
        {
            Lookup.Add(Entry.VertexID, Entry.Value.ToString());
        }
        return Lookup;
    }

    TSharedPtr<FJsonObject> BuildNodeJson(const FMetasoundFrontendNode& Node, bool bCompact)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetStringField(TEXT("id"), Node.GetID().ToString());
        Obj->SetStringField(TEXT("classID"), Node.ClassID.ToString());
        Obj->SetStringField(TEXT("name"), Node.Name.ToString());

        if (bCompact)
        {
            // Compact view: drop the per-vertex inputs/outputs arrays (vertex GUIDs + per-input
            // literal lookup) that make even a 2-node graph overflow the inline-readback budget.
            // Node identity stays; callers fetch full pin detail for a node via NodeIdFilter.
            return Obj;
        }

        const TMap<FGuid, FString> LiteralLookup = BuildLiteralLookup(Node);

        TArray<TSharedPtr<FJsonValue>> Inputs;
        Inputs.Reserve(Node.Interface.Inputs.Num());
        for (const FMetasoundFrontendVertex& Vertex : Node.Interface.Inputs)
        {
            TSharedPtr<FJsonObject> VertexObj = BuildVertexJson(Vertex);
            if (const FString* Literal = LiteralLookup.Find(Vertex.VertexID))
            {
                VertexObj->SetStringField(TEXT("defaultLiteral"), *Literal);
            }
            Inputs.Add(MakeShared<FJsonValueObject>(VertexObj));
        }
        Obj->SetArrayField(TEXT("inputs"), Inputs);

        TArray<TSharedPtr<FJsonValue>> Outputs;
        Outputs.Reserve(Node.Interface.Outputs.Num());
        for (const FMetasoundFrontendVertex& Vertex : Node.Interface.Outputs)
        {
            Outputs.Add(MakeShared<FJsonValueObject>(BuildVertexJson(Vertex)));
        }
        Obj->SetArrayField(TEXT("outputs"), Outputs);
        return Obj;
    }

    TSharedPtr<FJsonObject> BuildEdgeJson(const FMetasoundFrontendEdge& Edge)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetStringField(TEXT("fromNodeID"), Edge.FromNodeID.ToString());
        Obj->SetStringField(TEXT("fromVertexID"), Edge.FromVertexID.ToString());
        Obj->SetStringField(TEXT("toNodeID"), Edge.ToNodeID.ToString());
        Obj->SetStringField(TEXT("toVertexID"), Edge.ToVertexID.ToString());
        return Obj;
    }

    TSharedPtr<FJsonObject> BuildVariableJson(const FMetasoundFrontendVariable& Variable)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetStringField(TEXT("id"), Variable.ID.ToString());
        Obj->SetStringField(TEXT("name"), Variable.Name.ToString());
        Obj->SetStringField(TEXT("typeName"), Variable.TypeName.ToString());
        Obj->SetStringField(TEXT("literal"), Variable.Literal.ToString());
        return Obj;
    }

    FString ResolveAssetKind(UObject* Asset)
    {
        if (Asset->IsA<UMetaSoundSource>())
        {
            return TEXT("MetaSoundSource");
        }
        if (Asset->IsA<UMetaSoundPatch>())
        {
            return TEXT("MetaSoundPatch");
        }
        checkNoEntry();
        return TEXT("");
    }
}

namespace MetaSoundDumpBuilder
{
    TSharedPtr<FJsonObject> BuildMetaSoundJson(UObject* Asset)
    {
        return BuildMetaSoundJson(Asset, FMetaSoundDumpOptions());
    }

    TSharedPtr<FJsonObject> BuildMetaSoundJson(UObject* Asset, const FMetaSoundDumpOptions& Options)
    {
        if (!Asset)
        {
            return nullptr;
        }

        IMetaSoundDocumentInterface* DocInterface = Cast<IMetaSoundDocumentInterface>(Asset);
        if (!DocInterface)
        {
            return nullptr;
        }

        // asset.dump loads the asset and reads its document on the same tick, which on 5.8 can
        // land before the engine's async document versioning has migrated it. Join that first —
        // otherwise the paged graph below is genuinely absent. See WaitForDocumentVersioning.
        PinWright::MetaSound::WaitForDocumentVersioning(Asset);

        const FMetasoundFrontendDocument& Doc = PW_METASOUND_GET_CONST_DOCUMENT(DocInterface);
        const FMetasoundFrontendGraphClass& RootGraph = Doc.RootGraph;

        TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
        Root->SetStringField(TEXT("assetKind"), ResolveAssetKind(Asset));
        Root->SetStringField(TEXT("assetPath"), Asset->GetPathName());

        TSharedPtr<FJsonObject> RootGraphJson = MakeShared<FJsonObject>();
        RootGraphJson->SetStringField(TEXT("classID"), RootGraph.ID.ToString());
        RootGraphJson->SetStringField(TEXT("className"), RootGraph.Metadata.GetClassName().ToString());
        // NOT RootGraph.PresetOptions.bIsPreset — meta=(DeprecatedProperty), cleared by UE 5.8's
        // document versioning and never repopulated, so it emitted isPreset:false for every
        // preset while the sibling assetKind above (an IsA<> test) stayed correct. The
        // disagreement between the two fields was the only visible symptom.
        // See MetaSoundPathUtils.h → IsMetaSoundDocumentPreset.
        RootGraphJson->SetBoolField(TEXT("isPreset"),
            PinWright::MetaSound::IsMetaSoundDocumentPreset(Doc));

        // Derive which rootGraph interface vertices are auto-attached interface members
        // so each emitted entry can carry isInterfaceMember. Null on engines without the
        // interface registry — there the membership fields are omitted, not guessed.
#if PW_METASOUND_HAS_SEARCH_ENGINE
        const FInterfaceMembership Membership = BuildInterfaceMembership(Doc);
        const TMap<FName, FString>* InputOwner = &Membership.InputOwner;
        const TMap<FName, FString>* OutputOwner = &Membership.OutputOwner;
#else
        const TMap<FName, FString>* InputOwner = nullptr;
        const TMap<FName, FString>* OutputOwner = nullptr;
#endif

        // 5.6 deprecated Class.Interface in favor of GetDefaultInterface(); helper picks per version.
        const FMetasoundFrontendClassInterface& ClassInterface = PinWright::MetaSound::GetClassDefaultInterface(RootGraph);
        TSharedPtr<FJsonObject> InterfaceJson = MakeShared<FJsonObject>();

        TArray<TSharedPtr<FJsonValue>> InterfaceInputs;
        InterfaceInputs.Reserve(ClassInterface.Inputs.Num());
        for (const FMetasoundFrontendClassInput& Input : ClassInterface.Inputs)
        {
            InterfaceInputs.Add(MakeShared<FJsonValueObject>(BuildClassInputJson(Input, InputOwner)));
        }
        InterfaceJson->SetArrayField(TEXT("inputs"), InterfaceInputs);

        TArray<TSharedPtr<FJsonValue>> InterfaceOutputs;
        InterfaceOutputs.Reserve(ClassInterface.Outputs.Num());
        for (const FMetasoundFrontendClassOutput& Output : ClassInterface.Outputs)
        {
            InterfaceOutputs.Add(MakeShared<FJsonValueObject>(BuildClassOutputJson(Output, OutputOwner)));
        }
        InterfaceJson->SetArrayField(TEXT("outputs"), InterfaceOutputs);

        RootGraphJson->SetObjectField(TEXT("interface"), InterfaceJson);
        Root->SetObjectField(TEXT("rootGraph"), RootGraphJson);

        // Paged graphs (5.5+); helper reads RootGraph.Graph (5.3/5.4) or the default page (5.5+).
        // Deliberately the pointer form: the `Checked` sibling calls FindConstGraphChecked, which
        // aborts the process instead of failing, and one such asset takes an entire
        // asset.dump_folder sweep down mid-run. A document can still reach here with no default
        // page (versioning ran and failed, or an asset the engine never migrated), so that case is
        // reported in-band. On 5.3/5.4 the single `.Graph` member always exists and this is never
        // null. See B-asset-dump-metasound-unmigrated-assert.
        const FMetasoundFrontendGraph* GraphPtr = PinWright::MetaSound::FindGraphClassConstGraph(RootGraph);
        if (!GraphPtr)
        {
            UE_LOG(LogPinWrightSubsystem, Warning,
                TEXT("MetaSound '%s' has no default-page graph after versioning; emitting a skipped metasound.json."),
                *Asset->GetPathName());
            Root->SetBoolField(TEXT("skipped"), true);
            Root->SetStringField(TEXT("skipReason"), SkipReasonGraphUnavailable);
            Root->SetStringField(TEXT("skipMessage"),
                TEXT("Document has no graph for the default page; nodes/edges/variables are unavailable."));
            // nodes/edges/variables are omitted rather than emitted empty: an empty array is
            // indistinguishable from a graph that genuinely has none, and this document's is
            // unknown, not empty.
            return Root;
        }
        const FMetasoundFrontendGraph& Graph = *GraphPtr;

        const bool bFilterNodes = Options.NodeIdFilter.Num() > 0;
        TArray<TSharedPtr<FJsonValue>> NodesJson;
        NodesJson.Reserve(Graph.Nodes.Num());
        for (const FMetasoundFrontendNode& Node : Graph.Nodes)
        {
            if (bFilterNodes && !Options.NodeIdFilter.Contains(Node.GetID().ToString()))
            {
                continue;
            }
            NodesJson.Add(MakeShared<FJsonValueObject>(BuildNodeJson(Node, Options.bCompact)));
        }
        Root->SetArrayField(TEXT("nodes"), NodesJson);

        TArray<TSharedPtr<FJsonValue>> EdgesJson;
        EdgesJson.Reserve(Graph.Edges.Num());
        for (const FMetasoundFrontendEdge& Edge : Graph.Edges)
        {
            EdgesJson.Add(MakeShared<FJsonValueObject>(BuildEdgeJson(Edge)));
        }
        Root->SetArrayField(TEXT("edges"), EdgesJson);

        TArray<TSharedPtr<FJsonValue>> VariablesJson;
        VariablesJson.Reserve(Graph.Variables.Num());
        for (const FMetasoundFrontendVariable& Variable : Graph.Variables)
        {
            VariablesJson.Add(MakeShared<FJsonValueObject>(BuildVariableJson(Variable)));
        }
        Root->SetArrayField(TEXT("variables"), VariablesJson);

        // Summary header for the reduced views only — keeps the default (and asset.dump's
        // metasound.json sidecar) byte-identical so no aspect bump is needed. compact/nodeCount/
        // returnedNodeCount let a caller see it got a trimmed snapshot and how many nodes the
        // full graph has, so it can decide whether to fetch the rest.
        if (!Options.IsDefault())
        {
            Root->SetBoolField(TEXT("compact"), Options.bCompact);
            Root->SetNumberField(TEXT("nodeCount"), Graph.Nodes.Num());
            Root->SetNumberField(TEXT("returnedNodeCount"), NodesJson.Num());
        }

        return Root;
    }
}

#else // MCP_DUMP_HAS_METASOUND_FRONTEND

namespace MetaSoundDumpBuilder
{
    TSharedPtr<FJsonObject> BuildMetaSoundJson(UObject* /*Asset*/)
    {
        return nullptr;
    }

    TSharedPtr<FJsonObject> BuildMetaSoundJson(UObject* /*Asset*/, const FMetaSoundDumpOptions& /*Options*/)
    {
        return nullptr;
    }
}

#endif // MCP_DUMP_HAS_METASOUND_FRONTEND
