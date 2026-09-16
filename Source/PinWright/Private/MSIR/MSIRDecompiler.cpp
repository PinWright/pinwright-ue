// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "MSIR/MSIRDecompiler.h"

#include "MSIR/MSIRTypes.h"
#include "IrCore/IrTextUtils.h"

#include "UObject/Object.h"

// MetaSound modules are conditionally added to the plugin's build target. Mirror the
// gate pattern used by MetaSoundDumpBuilder.cpp so MSIR compiles out cleanly on builds
// that lack the frontend headers; the always-built shim below returns a structured
// error from BuildMetaSoundIrText in that case.
#if __has_include("MetasoundFrontendDocument.h")
#if __has_include("MetasoundAssetKey.h") && __has_include("MetasoundAssetManager.h")
#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "MetasoundAssetKey.h"
#include "MetasoundAssetManager.h"
#include "Modules/ModuleManager.h"
#define MCP_MSIR_HAS_METASOUND_ASSET_MANAGER 1
#else
#define MCP_MSIR_HAS_METASOUND_ASSET_MANAGER 0
#endif
#include "MetasoundDocumentInterface.h"
#include "MetasoundFrontendDocument.h"
#include "MetasoundFrontendLiteral.h"
#include "MetasoundSource.h"
#include "Metasound.h"
// Shared 5.6 paged-graphs compat helpers (default page-id / input-default / default-interface / default-graph access).
#include "Handlers/Audio/MetaSound/MetaSoundPathUtils.h"
#define MCP_MSIR_HAS_METASOUND_FRONTEND 1
#else
#define MCP_MSIR_HAS_METASOUND_FRONTEND 0
#endif

#if MCP_MSIR_HAS_METASOUND_FRONTEND

namespace
{
    FString FormatVersionToken(const FMetasoundFrontendVersionNumber& Version)
    {
        return FString::Printf(TEXT("v=%d.%d"), Version.Major, Version.Minor);
    }

    FString ResolveAssetKind(UObject* Asset, bool bIsPreset)
    {
        if (Asset->IsA<UMetaSoundSource>())
        {
            return bIsPreset ? TEXT("SourcePreset") : TEXT("MetaSoundSource");
        }
        if (Asset->IsA<UMetaSoundPatch>())
        {
            return bIsPreset ? TEXT("PatchPreset") : TEXT("MetaSoundPatch");
        }
        return bIsPreset ? TEXT("Preset") : TEXT("MetaSound");
    }

    // Build a ClassID -> FMetasoundFrontendClass* lookup once per document. Callers
    // previously did a linear scan of Doc.Dependencies per node; dense MetaSounds with
    // many nodes would otherwise pay O(N*D).
    TMap<FGuid, const FMetasoundFrontendClass*> BuildDependencyLookup(const FMetasoundFrontendDocument& Doc)
    {
        TMap<FGuid, const FMetasoundFrontendClass*> Lookup;
        Lookup.Reserve(Doc.Dependencies.Num());
        for (const FMetasoundFrontendClass& Dep : Doc.Dependencies)
        {
            Lookup.Add(Dep.ID, &Dep);
        }
        return Lookup;
    }

#if MCP_MSIR_HAS_METASOUND_ASSET_MANAGER
    // Scans the asset registry once for MetaSound patch/source assets and builds two
    // lookup tables keyed off their asset-class metadata:
    //   - OutAssetPathByKey: exact FMetaSoundAssetKey (ClassName + Version) -> asset path
    //   - OutAssetPathByClassName: version-agnostic ClassName -> asset path
    // The exact-key table is the precise match; the ClassName table is the fallback for
    // version skew. A referenced asset's recorded dependency version (what the parent
    // document saw at authoring time) can drift from the asset's currently-registered
    // version after the referenced asset is re-versioned; FMetaSoundAssetKey equality
    // compares BOTH ClassName and Version (MetasoundAssetKey.h), so the exact-key lookup
    // misses on skew and the GUID/ClassName synthetic identity is the only stable handle.
    void BuildAssetRegistryAssetPathCache(
        TMap<FMetaSoundAssetKey, FString>& OutAssetPathByKey,
        TMap<FMetasoundFrontendClassName, FString>& OutAssetPathByClassName)
    {
        FAssetRegistryModule& AssetRegistryModule =
            FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));

        FARFilter Filter;
        Filter.ClassPaths.Add(UMetaSoundPatch::StaticClass()->GetClassPathName());
        Filter.ClassPaths.Add(UMetaSoundSource::StaticClass()->GetClassPathName());
        Filter.bRecursiveClasses = true;

        TArray<FAssetData> AssetDataList;
        AssetRegistryModule.Get().GetAssets(Filter, AssetDataList);
        OutAssetPathByKey.Reserve(OutAssetPathByKey.Num() + AssetDataList.Num());
        OutAssetPathByClassName.Reserve(OutAssetPathByClassName.Num() + AssetDataList.Num());

        for (const FAssetData& AssetData : AssetDataList)
        {
            FMetaSoundAssetKey AssetKey;
            if (Metasound::Frontend::FMetaSoundAssetClassInfo::TryGetAssetKey(AssetData, AssetKey)
                && AssetKey.IsValid())
            {
                OutAssetPathByKey.Add(AssetKey, AssetData.GetObjectPathString());
                OutAssetPathByClassName.Add(AssetKey.ClassName, AssetData.GetObjectPathString());
                continue;
            }

            // Asset key parse failed (e.g. version tag missing/out-of-date for an
            // unloaded asset) but the class name may still parse; keep the ClassName
            // fallback usable in that case.
            FMetasoundFrontendClassName ClassName;
            if (Metasound::Frontend::FMetaSoundAssetClassInfo::TryGetAssetClassName(AssetData, ClassName)
                && ClassName.IsValid())
            {
                OutAssetPathByClassName.Add(ClassName, AssetData.GetObjectPathString());
            }
        }
    }
#endif

    bool TryResolveAssetClassPath(
        const FMetasoundFrontendClass& Class,
#if MCP_MSIR_HAS_METASOUND_ASSET_MANAGER
        TMap<FMetaSoundAssetKey, FString>& AssetRegistryAssetPathByKey,
        TMap<FMetasoundFrontendClassName, FString>& AssetRegistryAssetPathByClassName,
        bool& bAssetRegistryAssetPathsCached,
#endif
        FString& OutAssetPath)
    {
#if MCP_MSIR_HAS_METASOUND_ASSET_MANAGER
        if (!FMetaSoundAssetKey::IsValidType(Class.Metadata.GetType()))
        {
            return false;
        }

        const FMetaSoundAssetKey AssetKey(Class.Metadata);
        if (!AssetKey.IsValid())
        {
            return false;
        }

        // 1. Exact-key asset-manager lookup (ClassName + recorded Version).
        if (Metasound::Frontend::IMetaSoundAssetManager* AssetManager =
            Metasound::Frontend::IMetaSoundAssetManager::Get())
        {
            const FTopLevelAssetPath AssetPath = AssetManager->FindAssetPath(AssetKey);
            if (AssetPath.IsValid())
            {
                OutAssetPath = AssetPath.ToString();
                return true;
            }
        }

        if (!bAssetRegistryAssetPathsCached)
        {
            BuildAssetRegistryAssetPathCache(AssetRegistryAssetPathByKey, AssetRegistryAssetPathByClassName);
            bAssetRegistryAssetPathsCached = true;
        }

        // 2. Exact-key asset-registry lookup (same ClassName + Version requirement).
        if (const FString* AssetPath = AssetRegistryAssetPathByKey.Find(AssetKey))
        {
            OutAssetPath = *AssetPath;
            return true;
        }

        // 3. Version-agnostic ClassName fallback. The dependency records the version the
        // parent saw at authoring time; once the referenced asset is re-versioned the
        // exact-key lookups above all miss even though the asset still exists, because
        // FMetaSoundAssetKey equality compares ClassName AND Version (MetasoundAssetKey.h)
        // and both IMetaSoundAssetManager::FindAssetPath/FindAssetPaths key on the full
        // key. The asset-class GUID lives in ClassName.Name and is stable across versions,
        // so a ClassName-only match resolves the same referenced asset across version skew.
        if (const FString* AssetPath = AssetRegistryAssetPathByClassName.Find(AssetKey.ClassName))
        {
            OutAssetPath = *AssetPath;
            return true;
        }

        return false;
#else
        return false;
#endif
    }

    bool TryResolveAssetClassPathCached(
        const FGuid& ClassID,
        const FMetasoundFrontendClass& Class,
        TMap<FGuid, FString>& ResolvedAssetPathByClassID,
        TSet<FGuid>& UnresolvedAssetPathClassIDs,
#if MCP_MSIR_HAS_METASOUND_ASSET_MANAGER
        TMap<FMetaSoundAssetKey, FString>& AssetRegistryAssetPathByKey,
        TMap<FMetasoundFrontendClassName, FString>& AssetRegistryAssetPathByClassName,
        bool& bAssetRegistryAssetPathsCached,
#endif
        FString& OutAssetPath)
    {
        if (const FString* CachedAssetPath = ResolvedAssetPathByClassID.Find(ClassID))
        {
            OutAssetPath = *CachedAssetPath;
            return true;
        }

        if (UnresolvedAssetPathClassIDs.Contains(ClassID))
        {
            return false;
        }

        FString AssetPath;
        if (TryResolveAssetClassPath(
            Class,
#if MCP_MSIR_HAS_METASOUND_ASSET_MANAGER
            AssetRegistryAssetPathByKey,
            AssetRegistryAssetPathByClassName,
            bAssetRegistryAssetPathsCached,
#endif
            AssetPath))
        {
            OutAssetPath = AssetPath;
            ResolvedAssetPathByClassID.Add(ClassID, MoveTemp(AssetPath));
            return true;
        }

        UnresolvedAssetPathClassIDs.Add(ClassID);
        return false;
    }

    bool TryReadNodePosition(const FMetasoundFrontendNode& Node, int32& OutX, int32& OutY)
    {
        // 5.6 keys node locations by DefaultPageID; on 5.4/5.5 they were keyed by a
        // default-constructed FGuid() (same all-zero value). Helper returns the right key.
        const FVector2D* Position = Node.Style.Display.Locations.Find(PinWright::MetaSound::GetDefaultPageId());
        if (!Position)
        {
            return false;
        }
        OutX = FMath::RoundToInt(Position->X);
        OutY = FMath::RoundToInt(Position->Y);
        return true;
    }

    FString FormatFields(const TArray<FMSIRNodePortLiteral>& Fields)
    {
        TArray<FString> Parts;
        Parts.Reserve(Fields.Num());
        for (const FMSIRNodePortLiteral& Field : Fields)
        {
            Parts.Add(FString::Printf(TEXT("%s: %s"),
                *FIrTextUtils::FormatNameToken(Field.InputName), *Field.Literal));
        }
        return FString::Join(Parts, TEXT(", "));
    }

    void BuildInputs(const FMetasoundFrontendClassInterface& Iface, FMSIRDocument& OutDoc)
    {
        OutDoc.Inputs.Reserve(Iface.Inputs.Num());
        for (const FMetasoundFrontendClassInput& Input : Iface.Inputs)
        {
            FMSIRInput Entry;
            Entry.TypeName = Input.TypeName.ToString();
            Entry.Name = Input.Name.ToString();
            // 5.6 paged input defaults; helper reads DefaultLiteral (5.4/5.5) or default page (5.6+).
            if (const FMetasoundFrontendLiteral* Default = PinWright::MetaSound::FindClassInputDefault(Input))
            {
                Entry.bHasDefault = true;
                Entry.DefaultLiteral = Default->ToString();
            }
            OutDoc.Inputs.Add(MoveTemp(Entry));
        }
    }

    void BuildOutputs(const FMetasoundFrontendClassInterface& Iface, FMSIRDocument& OutDoc)
    {
        OutDoc.Outputs.Reserve(Iface.Outputs.Num());
        for (const FMetasoundFrontendClassOutput& Output : Iface.Outputs)
        {
            FMSIROutput Entry;
            Entry.TypeName = Output.TypeName.ToString();
            Entry.Name = Output.Name.ToString();
            OutDoc.Outputs.Add(MoveTemp(Entry));
        }
    }

    void BuildVariables(const FMetasoundFrontendGraph& Graph, FMSIRDocument& OutDoc)
    {
        OutDoc.Variables.Reserve(Graph.Variables.Num());
        int32 VIndex = 1;
        for (const FMetasoundFrontendVariable& Var : Graph.Variables)
        {
            FMSIRVariable Entry;
            Entry.TypeName = Var.TypeName.ToString();
            Entry.Name = Var.Name.ToString();
            Entry.Sigil = FString::Printf(TEXT("v%d"), VIndex++);
            Entry.Literal = Var.Literal.ToString();
            OutDoc.Variables.Add(MoveTemp(Entry));
        }
    }

    // Resolves a node-vertex reference to either a sigil ($Name) for graph I/O nodes,
    // or to "nLocal.VertexName" for general nodes. The Input/Output template classes
    // represent graph-level interface members; their single vertex name matches the
    // graph interface vertex. VertexNameByNodeVertex is precomputed once per graph
    // during the node walk, keyed by (NodeID, VertexID), so this is O(1).
    FString FormatVertexRef(
        const FGuid& NodeID,
        const FGuid& VertexID,
        const TMap<FGuid, const FMetasoundFrontendNode*>& NodeByID,
        const TMap<FGuid, FString>& NodeLocalByID,
        const TMap<FGuid, EMetasoundFrontendClassType>& ClassTypeByID,
        const TMap<TPair<FGuid, FGuid>, FName>& VertexNameByNodeVertex,
        bool& bOutResolved)
    {
        bOutResolved = false;
        const FMetasoundFrontendNode* const * NodePtr = NodeByID.Find(NodeID);
        if (!NodePtr || !*NodePtr)
        {
            return TEXT("?");
        }
        const FMetasoundFrontendNode& Node = **NodePtr;

        const FName* VertexNamePtr = VertexNameByNodeVertex.Find(TPair<FGuid, FGuid>(NodeID, VertexID));
        const FString VertexStr = (VertexNamePtr && !VertexNamePtr->IsNone()) ? VertexNamePtr->ToString() : TEXT("?");

        const EMetasoundFrontendClassType* TypePtr = ClassTypeByID.Find(Node.ClassID);
        const bool bIsInterfaceNode = TypePtr && (*TypePtr == EMetasoundFrontendClassType::Input || *TypePtr == EMetasoundFrontendClassType::Output);
        if (bIsInterfaceNode)
        {
            bOutResolved = true;
            return FString::Printf(TEXT("$%s"), *VertexStr);
        }

        const FString* Local = NodeLocalByID.Find(NodeID);
        if (!Local)
        {
            return FString::Printf(TEXT("?.%s"), *VertexStr);
        }
        bOutResolved = true;
        return FString::Printf(TEXT("%s.%s"), **Local, *VertexStr);
    }

    void BuildNodesAndWires(
        const FMetasoundFrontendDocument& Doc,
        const FMetasoundFrontendGraph& Graph,
        FMSIRDocument& OutDoc,
        TArray<FString>& OutWarnings)
    {
        TMap<FGuid, const FMetasoundFrontendNode*> NodeByID;
        TMap<FGuid, FString> NodeLocalByID;
        TMap<FGuid, EMetasoundFrontendClassType> ClassTypeByID;
        // (NodeID, VertexID) -> VertexName for both Inputs and Outputs. Populated during
        // the node walk so the wire emission loop below stays O(E) instead of O(E * V).
        TMap<TPair<FGuid, FGuid>, FName> VertexNameByNodeVertex;
        NodeByID.Reserve(Graph.Nodes.Num());
        NodeLocalByID.Reserve(Graph.Nodes.Num());

        const TMap<FGuid, const FMetasoundFrontendClass*> DepByID = BuildDependencyLookup(Doc);
        TMap<FGuid, FString> ResolvedAssetPathByClassID;
        TSet<FGuid> UnresolvedAssetPathClassIDs;
        ResolvedAssetPathByClassID.Reserve(Doc.Dependencies.Num());
        UnresolvedAssetPathClassIDs.Reserve(Doc.Dependencies.Num());
#if MCP_MSIR_HAS_METASOUND_ASSET_MANAGER
        TMap<FMetaSoundAssetKey, FString> AssetRegistryAssetPathByKey;
        TMap<FMetasoundFrontendClassName, FString> AssetRegistryAssetPathByClassName;
        bool bAssetRegistryAssetPathsCached = false;
#endif

        int32 NIndex = 1;
        OutDoc.Nodes.Reserve(Graph.Nodes.Num());
        for (const FMetasoundFrontendNode& Node : Graph.Nodes)
        {
            FMSIRNode Entry;
            Entry.Local = FIrTextUtils::FormatNumericLocalId(NIndex++);

            const FMetasoundFrontendClass* const * DepPtr = DepByID.Find(Node.ClassID);
            const FMetasoundFrontendClass* Class = DepPtr ? *DepPtr : nullptr;
            if (Class)
            {
                ClassTypeByID.Add(Node.ClassID, Class->Metadata.GetType());
                const FMetasoundFrontendClassName& ClassName = Class->Metadata.GetClassName();
                const FMetasoundFrontendVersionNumber& Version = Class->Metadata.GetVersion();
                Entry.ClassRef.Major = Version.Major;
                Entry.ClassRef.Minor = Version.Minor;

                FString AssetClassGuid;
                if (PinWright::MetaSound::IsAssetClassGuidName(ClassName, AssetClassGuid))
                {
                    FString AssetPath;
                    if (TryResolveAssetClassPathCached(
                        Node.ClassID,
                        *Class,
                        ResolvedAssetPathByClassID,
                        UnresolvedAssetPathClassIDs,
#if MCP_MSIR_HAS_METASOUND_ASSET_MANAGER
                        AssetRegistryAssetPathByKey,
                        AssetRegistryAssetPathByClassName,
                        bAssetRegistryAssetPathsCached,
#endif
                        AssetPath))
                    {
                        Entry.ClassRef.Namespace.Empty();
                        Entry.ClassRef.Name = MoveTemp(AssetPath);
                    }
                    else
                    {
                        Entry.ClassRef.Namespace = ClassName.Namespace.ToString();
                        Entry.ClassRef.Name = ClassName.Name.ToString();
                        OutWarnings.Add(FString::Printf(
                            TEXT("MSIR_UNRESOLVED_CLASS_REF: Node '%s' references asset class GUID %s, but neither the MetaSound asset manager nor the asset registry resolved an asset path."),
                            *Node.Name.ToString(), *AssetClassGuid));
                    }
                }
                else
                {
                    Entry.ClassRef.Namespace = ClassName.Namespace.ToString();
                    Entry.ClassRef.Name = ClassName.Name.ToString();
                }
            }
            else
            {
                Entry.ClassRef.Name = Node.ClassID.ToString(EGuidFormats::Digits);
                OutWarnings.Add(FString::Printf(TEXT("MSIR_UNRESOLVED_CLASS_REF: Node '%s' references missing dependency ClassID %s."),
                    *Node.Name.ToString(), *Node.ClassID.ToString()));
            }

            int32 X = 0, Y = 0;
            if (TryReadNodePosition(Node, X, Y))
            {
                Entry.bHasPosition = true;
                Entry.X = X;
                Entry.Y = Y;
            }

            // Walk Node.InputLiterals once into a VertexID -> literal map so the per-input emission
            // stays O(N+M) rather than O(N*M); mirrors MetaSoundDumpBuilder's BuildLiteralLookup.
            TMap<FGuid, FString> LiteralByVertexID;
            LiteralByVertexID.Reserve(Node.InputLiterals.Num());
            for (const FMetasoundFrontendVertexLiteral& VL : Node.InputLiterals)
            {
                LiteralByVertexID.Add(VL.VertexID, VL.Value.ToString());
            }
            for (const FMetasoundFrontendVertex& V : Node.Interface.Inputs)
            {
                if (const FString* Lit = LiteralByVertexID.Find(V.VertexID))
                {
                    FMSIRNodePortLiteral Field;
                    Field.InputName = V.Name.ToString();
                    Field.Literal = *Lit;
                    Entry.InputLiterals.Add(MoveTemp(Field));
                }
            }

            const FGuid NodeID = Node.GetID();
            for (const FMetasoundFrontendVertex& V : Node.Interface.Inputs)
            {
                VertexNameByNodeVertex.Add(TPair<FGuid, FGuid>(NodeID, V.VertexID), V.Name);
            }
            for (const FMetasoundFrontendVertex& V : Node.Interface.Outputs)
            {
                VertexNameByNodeVertex.Add(TPair<FGuid, FGuid>(NodeID, V.VertexID), V.Name);
            }

            NodeByID.Add(NodeID, &Node);
            NodeLocalByID.Add(NodeID, Entry.Local);
            OutDoc.Nodes.Add(MoveTemp(Entry));
        }

        OutDoc.Wires.Reserve(Graph.Edges.Num());
        for (const FMetasoundFrontendEdge& Edge : Graph.Edges)
        {
            bool bFromOk = false;
            bool bToOk = false;
            FMSIRWire Wire;
            Wire.From = FormatVertexRef(Edge.FromNodeID, Edge.FromVertexID, NodeByID, NodeLocalByID, ClassTypeByID, VertexNameByNodeVertex, bFromOk);
            Wire.To = FormatVertexRef(Edge.ToNodeID, Edge.ToVertexID, NodeByID, NodeLocalByID, ClassTypeByID, VertexNameByNodeVertex, bToOk);
            if (!bFromOk || !bToOk)
            {
                OutWarnings.Add(FString::Printf(TEXT("Edge has unresolved endpoint: %s -> %s"), *Wire.From, *Wire.To));
            }
            OutDoc.Wires.Add(MoveTemp(Wire));
        }
    }

    void EmitText(const FMSIRDocument& Doc, FString& OutText)
    {
        TArray<FString> Lines;
        Lines.Reserve(32 + Doc.Inputs.Num() + Doc.Outputs.Num() + Doc.Nodes.Num() * 2 + Doc.Wires.Num());

        FString Header = FString::Printf(TEXT("metasound %s kind=%s"),
            *FIrTextUtils::FormatNameToken(Doc.LocalName), *Doc.Kind);
        if (!Doc.BasedOn.IsEmpty())
        {
            Header += FString::Printf(TEXT(" based_on=%s"), *FIrTextUtils::Quote(Doc.BasedOn));
        }
        Header += TEXT(" {");
        Lines.Add(Header);

        for (const FMSIRInterfaceVersion& Iface : Doc.Interfaces)
        {
            FString Combined = Iface.Namespace;
            if (!Combined.IsEmpty() && !Iface.Name.IsEmpty())
            {
                Combined.AppendChar(TEXT('.'));
            }
            Combined += Iface.Name;
            Lines.Add(FString::Printf(TEXT("  interface %s v=%d.%d"),
                *FIrTextUtils::FormatNameToken(Combined), Iface.Major, Iface.Minor));
        }

        for (const FMSIRInput& Input : Doc.Inputs)
        {
            FString Line = FString::Printf(TEXT("  input %s %s"),
                *Input.TypeName, *Input.Name);
            if (Input.bHasDefault)
            {
                Line += FString::Printf(TEXT(" = %s"), *Input.DefaultLiteral);
            }
            Lines.Add(Line);
        }

        for (const FMSIROutput& Output : Doc.Outputs)
        {
            Lines.Add(FString::Printf(TEXT("  output %s %s"), *Output.TypeName, *Output.Name));
        }

        for (const FMSIRVariable& Var : Doc.Variables)
        {
            Lines.Add(FString::Printf(TEXT("  variable %s %s = %s  // id=$%s"),
                *Var.TypeName, *Var.Name, *Var.Literal, *Var.Sigil));
        }

        for (const FMSIRNode& Node : Doc.Nodes)
        {
            FString ClassToken;
            {
                FString Combined = Node.ClassRef.Namespace;
                if (!Combined.IsEmpty() && !Node.ClassRef.Name.IsEmpty())
                {
                    Combined.AppendChar(TEXT('.'));
                }
                Combined += Node.ClassRef.Name;
                ClassToken = FIrTextUtils::FormatNameToken(Combined);
            }

            FMetasoundFrontendVersionNumber NodeVersion;
            NodeVersion.Major = Node.ClassRef.Major;
            NodeVersion.Minor = Node.ClassRef.Minor;
            FString Line = FString::Printf(TEXT("  node %s = %s %s"),
                *Node.Local, *ClassToken, *FormatVersionToken(NodeVersion));
            if (Node.bHasPosition)
            {
                Line += FIrTextUtils::FormatPositionSuffix(Node.X, Node.Y);
            }
            if (Node.InputLiterals.Num() > 0)
            {
                Line += FString::Printf(TEXT(" { %s }"), *FormatFields(Node.InputLiterals));
            }
            Lines.Add(Line);
        }

        for (const FMSIRWire& Wire : Doc.Wires)
        {
            Lines.Add(FString::Printf(TEXT("  wire %s -> %s"), *Wire.From, *Wire.To));
        }

        for (const FMSIRInput& Override : Doc.PresetOverrides)
        {
            FString Line = FString::Printf(TEXT("  override input %s %s"),
                *Override.TypeName, *Override.Name);
            if (Override.bHasDefault)
            {
                Line += FString::Printf(TEXT(" = %s"), *Override.DefaultLiteral);
            }
            Lines.Add(Line);
        }

        Lines.Add(TEXT("}"));
        OutText = FString::Join(Lines, TEXT("\n")) + TEXT("\n");
    }
}

FMSIRResult FMSIRDecompiler::BuildMetaSoundIrText(UObject* MetaSoundAsset)
{
    if (!MetaSoundAsset)
    {
        return FMSIRResult::MakeError(TEXT("MetaSound asset is null."));
    }

    IMetaSoundDocumentInterface* DocInterface = Cast<IMetaSoundDocumentInterface>(MetaSoundAsset);
    if (!DocInterface)
    {
        return FMSIRResult::MakeError(TEXT("Asset does not implement IMetaSoundDocumentInterface."));
    }

    const FMetasoundFrontendDocument& Doc = PW_METASOUND_GET_CONST_DOCUMENT(DocInterface);
    const FMetasoundFrontendGraphClass& RootGraph = Doc.RootGraph;
    // Paged graphs (5.5+); helper reads &RootGraph.Graph (5.3/5.4) or the default page (5.5+).
    const FMetasoundFrontendGraph* GraphPtr = PinWright::MetaSound::FindGraphClassConstGraph(RootGraph);
    if (!GraphPtr)
    {
        return FMSIRResult::MakeError(TEXT("MetaSound document has no default page graph."));
    }

    FMSIRResult Result;
    Result.bSuccess = true;

    FMSIRDocument IrDoc;
    IrDoc.LocalName = MetaSoundAsset->GetName();
    // NOT RootGraph.PresetOptions.bIsPreset — that field is meta=(DeprecatedProperty) and UE 5.8
    // actively clears it during document versioning, so it reads false for every 5.8 preset and
    // took both IrDoc.Kind and the preset-emit branch below down with it. See
    // MetaSoundPathUtils.h → IsMetaSoundDocumentPreset for the citation chain.
    IrDoc.bIsPreset = PinWright::MetaSound::IsMetaSoundDocumentPreset(Doc);
    IrDoc.Kind = ResolveAssetKind(MetaSoundAsset, IrDoc.bIsPreset);

    for (const FMetasoundFrontendVersion& Version : Doc.Interfaces)
    {
        FMSIRInterfaceVersion Entry;
        const FString FullName = Version.Name.ToString();
        int32 DotIndex = INDEX_NONE;
        if (FullName.FindLastChar(TEXT('.'), DotIndex))
        {
            Entry.Namespace = FullName.Left(DotIndex);
            Entry.Name = FullName.Mid(DotIndex + 1);
        }
        else
        {
            Entry.Name = FullName;
        }
        Entry.Major = Version.Number.Major;
        Entry.Minor = Version.Number.Minor;
        IrDoc.Interfaces.Add(MoveTemp(Entry));
    }

    // 5.6 deprecated Class.Interface in favor of GetDefaultInterface(); helper picks per version.
    const FMetasoundFrontendClassInterface& DefaultInterface = PinWright::MetaSound::GetClassDefaultInterface(RootGraph);
    BuildInputs(DefaultInterface, IrDoc);
    BuildOutputs(DefaultInterface, IrDoc);

    if (IrDoc.bIsPreset)
    {
        // Presets keep their referenced-source state on the single preset node's
        // InputLiterals; surface those as override-input lines and skip the body
        // emit (presets never carry a freestanding graph in MSIR v1).
        for (const FMetasoundFrontendNode& Node : GraphPtr->Nodes)
        {
            TMap<FGuid, FString> LitByVertex;
            LitByVertex.Reserve(Node.InputLiterals.Num());
            for (const FMetasoundFrontendVertexLiteral& VL : Node.InputLiterals)
            {
                LitByVertex.Add(VL.VertexID, VL.Value.ToString());
            }
            for (const FMetasoundFrontendVertex& V : Node.Interface.Inputs)
            {
                FMSIRInput Override;
                Override.TypeName = V.TypeName.ToString();
                Override.Name = V.Name.ToString();
                if (const FString* Lit = LitByVertex.Find(V.VertexID))
                {
                    Override.bHasDefault = true;
                    Override.DefaultLiteral = *Lit;
                }
                IrDoc.PresetOverrides.Add(MoveTemp(Override));
            }
        }
        Result.Warnings.Add(TEXT("MSIR v1: preset based_on path is not resolved; emit shows class name dependency only."));
    }
    else
    {
        BuildVariables(*GraphPtr, IrDoc);
        BuildNodesAndWires(Doc, *GraphPtr, IrDoc, Result.Warnings);
    }

    EmitText(IrDoc, Result.Text);
    return Result;
}

#else // MCP_MSIR_HAS_METASOUND_FRONTEND

FMSIRResult FMSIRDecompiler::BuildMetaSoundIrText(UObject* /*MetaSoundAsset*/)
{
    return FMSIRResult::MakeError(TEXT("MetaSound modules not available in this build."));
}

#endif // MCP_MSIR_HAS_METASOUND_FRONTEND
