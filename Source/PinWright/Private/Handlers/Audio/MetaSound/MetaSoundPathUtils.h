// Copyright (c) 2026 Alexander Penkin. MIT License.

// Shared MetaSound-specific helpers: document loading, and the engine-version
// compat shims for the Frontend document model. Path normalization is NOT here -
// it is the plugin-wide NormalizeContentAssetPath (Utils/PathUtils.h), which this
// header pulls in so every MetaSound handler TU that includes this one can call it.

#pragma once

#include "CoreMinimal.h"
// UE_VERSION_OLDER_THAN — gate the MetaSound "paged graphs" API churn introduced in 5.6.
#include "Misc/EngineVersionComparison.h"
// NormalizeContentAssetPath — the one path normalizer every MetaSound handler uses. Included
// here rather than per-.cpp because every MetaSound handler TU already includes this header to
// reach the compat macros, and the local copy this replaced lived here too.
#include "Utils/PathUtils.h"

#if __has_include("MetasoundDocumentInterface.h")
#include "MetasoundDocumentInterface.h"
#include "UObject/ScriptInterface.h"
#define PW_METASOUND_PATHUTILS_HAS_DOCUMENT_INTERFACE 1
#else
#define PW_METASOUND_PATHUTILS_HAS_DOCUMENT_INTERFACE 0
#endif

// ---------------------------------------------------------------------------
// MetaSound paged-graphs compat (UE 5.6 changed the Frontend document model).
//
// The builder ctor gained a third `bPrimeCache` arg in 5.6 (and the delegate
// arg switched from TSharedRef to TSharedPtr); pre-5.6 only the 1/2-arg forms
// exist. Constructing the builder in-place via a macro avoids relying on the
// builder's move/copy semantics (it is a USTRUCT with an explicit dtor).
//
// PW_METASOUND_MAKE_BUILDER(VarName, ScriptInterface)
//   5.6+ : prime the cache (matches the prior 3-arg behavior).
//   <5.6 : 1-arg ctor — no bPrimeCache, and the 2nd arg is a TSharedRef with
//          no default so nullptr cannot be passed.
// Only define when the builder header is actually present in the include path.
// ---------------------------------------------------------------------------
#if __has_include("MetasoundFrontendDocumentBuilder.h")
#if UE_VERSION_OLDER_THAN(5, 6, 0)
#define PW_METASOUND_MAKE_BUILDER(VarName, ScriptInterface) \
    FMetaSoundFrontendDocumentBuilder VarName(ScriptInterface)
#else
#define PW_METASOUND_MAKE_BUILDER(VarName, ScriptInterface) \
    FMetaSoundFrontendDocumentBuilder VarName(ScriptInterface, nullptr, true)
#endif

// FMetaSoundFrontendDocumentBuilder::FinishBuilding() was added in UE 5.4 to flush
// the builder's edits and rebuild its cache. On 5.3 the equivalent finalize is
// ReloadCache() (FinishBuilding internally rebuilds the same cache). Use this macro at
// every "done mutating" site so call sites stay version-agnostic.
#if UE_VERSION_OLDER_THAN(5, 4, 0)
#define PW_METASOUND_FINISH_BUILDING(Builder) (Builder).ReloadCache()
#else
#define PW_METASOUND_FINISH_BUILDING(Builder) (Builder).FinishBuilding()
#endif
#endif // __has_include("MetasoundFrontendDocumentBuilder.h")

// IMetaSoundDocumentInterface::GetConstDocument() was added in UE 5.4 as an explicit const
// accessor. On 5.3 only the const overload of GetDocument() is public (the non-const overload
// is private); dereference through AsConst() so the public const overload is selected. Works
// for both a raw IMetaSoundDocumentInterface* and a TScriptInterface<IMetaSoundDocumentInterface>.
// Usage: PW_METASOUND_GET_CONST_DOCUMENT(DocInterfacePtrOrScriptInterface)
#if UE_VERSION_OLDER_THAN(5, 4, 0)
#define PW_METASOUND_GET_CONST_DOCUMENT(DocIface) (AsConst(*(DocIface)).GetDocument())
#else
#define PW_METASOUND_GET_CONST_DOCUMENT(DocIface) ((DocIface)->GetConstDocument())
#endif

// Declared at global scope on purpose. An elaborated `class FJsonObject` written inside the
// namespace below declares PinWright::MetaSound::FJsonObject whenever the real one is not already
// visible, which is what happens outside a unity blob, and the definition then mismatches every
// caller passing the engine's ::FJsonObject.
class FJsonObject;

namespace PinWright::MetaSound
{
#if PW_METASOUND_PATHUTILS_HAS_DOCUMENT_INTERFACE
    UObject* LoadMetaSoundDocumentAsset(const FString& AssetPath);
    bool TryMakeMetaSoundDocumentInterface(
        UObject* Asset,
        TScriptInterface<IMetaSoundDocumentInterface>& OutDocumentInterface);

    // The mutator-family load gate. Loads any MetaSound *document* asset by path —
    // both UMetaSoundSource and the sibling UMetaSoundPatch implement
    // IMetaSoundDocumentInterface, so this accepts either. Returns nullptr when the
    // path holds no loadable asset OR an asset that is not a MetaSound document,
    // reproducing the null semantics of the legacy
    // Cast<UMetaSoundSource>(StaticLoadObject(UMetaSoundSource::StaticClass(),...))
    // gate (which callers report as ASSET_NOT_FOUND) — except it no longer rejects a
    // Patch as "not found". The returned UObject drives the class-agnostic
    // FMetaSoundFrontendDocumentBuilder (via a TScriptInterface<IMetaSoundDocumentInterface>),
    // so Patch graphs become populatable through the same handlers as Source graphs.
    UObject* LoadMetaSoundDocumentObject(const FString& AssetPath);
#endif

    // Blocks until the engine has finished versioning (migrating) this MetaSound's document.
    //
    // UE 5.8 moved document versioning — including the 5.5 single-graph -> paged-graphs
    // migration — onto an async task kicked off from PostLoad, and for an asset that holds
    // soft references to other MetaSounds it defers even kicking that task off until those
    // references finish loading ("Delaying asset versioning due to need to async load soft
    // references", MetasoundEngineAsset.h). Any caller that loads a MetaSound and reads its
    // document on the same tick therefore sees a document whose PagedGraphs array is still
    // empty, and every `Checked` frontend accessor (FindConstGraphChecked, the document
    // builder's bPrimeCache path) aborts the process on it rather than returning an error.
    //
    // The engine has the same problem when a commandlet reads a MetaSound straight after
    // load, and solves it exactly this way (MetasoundEngineAsset.h PostLoad ->
    // FVersioningManager::WaitUntilVersioningComplete under IsRunningCommandlet). PinWright
    // is in that same position on every MetaSound RPC and on asset.dump, so it takes the
    // same wait at the points where it first obtains a document.
    //
    // Safe to call with null or with a non-MetaSound object (no-op). No-op on engines before
    // 5.8, where PostLoad versions the document synchronously.
    void WaitForDocumentVersioning(const UObject* MetaSoundAsset);

    // THE save path for every MetaSound authoring verb that takes a `save` param. Routes a
    // requested save through the plugin-wide real-save chokepoint (SaveAssetToDiskReportingPresence
    // -> SaveLoadedAssetThrottled -> UEditorAssetLibrary::SaveLoadedAsset) and writes the shared
    // {saveRequested, saved, pendingFlush, saveState, saveDetail} report via AddAssetSaveReport, so
    // a MetaSound verb's persistence contract is indistinguishable from niagara.create_* /
    // audio.authoring.create_*. No MetaSound-local save or report shape exists any more.
    //
    // Why a real write and not the mark-dirty McpSafeAssetSave every MetaSound verb used to call:
    // mark-dirty persists nothing, so a whole graph authored through this surface vanished on a
    // cold editor restart while every response reported success
    // (B-metasound-create-save-no-disk-write). A MetaSound document is not a Blueprint/SCS asset,
    // so the bulkdata-corruption vector that pins McpSafeAssetSave on Blueprint edits does not
    // apply here. bForce is passed so an explicit save:true is never silently eaten by the 0.5s
    // save throttle mid-way through a burst of authoring calls.
    //
    // CALL ORDER — for any verb that mutated through FMetaSoundFrontendDocumentBuilder this MUST
    // run AFTER PW_METASOUND_FINISH_BUILDING(Builder). The builder's edits only reach the document
    // when FinishBuilding flushes its caches; a save taken before that serializes the pre-edit
    // document. The old mark-dirty calls were order-insensitive (they wrote nothing), which is why
    // several of them sat before the FinishBuilding they now follow.
    //
    // A create verb must still call FAssetRegistryModule::AssetCreated on the new asset before
    // this, or SaveLoadedAsset declines to write a never-before-seen in-memory package.
    //
    // Returns true only when the .uasset is on disk after the call (always false when
    // bSaveRequested is false). Null-safe: reports nothing and returns false.
    bool SaveMetaSoundAndReport(
        const TSharedPtr<FJsonObject>& Result,
        UObject* MetaSoundAsset,
        bool bSaveRequested);
}

// ---------------------------------------------------------------------------
// FMetasoundFrontendInterface field-access compat.
//
// In UE 5.6 the interface's Version / UClassOptions fields were relocated into
// a nested `Metadata` sub-struct (FMetasoundFrontendInterfaceMetadata). The
// top-level fields still exist on 5.6/5.7 but are UE_DEPRECATED(5.6) and only
// under WITH_EDITORONLY_DATA, so reaching for them would warn/fail. Use these
// inline accessors so every call site reads from the right location.
// ---------------------------------------------------------------------------
#if __has_include("MetasoundFrontendDocument.h")
#include "MetasoundFrontendDocument.h"

namespace PinWright::MetaSound
{
    // Single definition of "this ClassName is a synthetic asset-class-GUID dependency":
    // an empty Namespace plus a Name that parses as a GUID. MSIR's decompiler uses this
    // to detect dependencies that name a referenced MetaSound asset by its stable
    // asset-class GUID (instead of a registered node-class namespace.name), and the MSIR
    // decompiler tests reuse it to pick which dependencies to mutate — both must agree on
    // the predicate, so it lives here rather than re-derived inline in either place.
    inline bool IsAssetClassGuidName(const FMetasoundFrontendClassName& ClassName, FString& OutGuidText)
    {
        if (!ClassName.Namespace.IsNone())
        {
            return false;
        }

        const FString NameText = ClassName.Name.ToString();
        FGuid ParsedGuid;
        if (!FGuid::Parse(NameText, ParsedGuid))
        {
            return false;
        }

        OutGuidText = NameText;
        return true;
    }

    // The "default page" GUID used to key the page-0 graph / input-default / node
    // location on 5.6+. The constant Metasound::Frontend::DefaultPageID was added
    // in 5.6 and is the all-zero GUID; pre-5.6 those maps were keyed by a default-
    // constructed FGuid() (also all-zero), so this value is correct on every version.
    inline FGuid GetDefaultPageId()
    {
#if UE_VERSION_OLDER_THAN(5, 6, 0)
        return FGuid();
#else
        return Metasound::Frontend::DefaultPageID;
#endif
    }

    inline const FMetasoundFrontendVersion& GetInterfaceVersion(const FMetasoundFrontendInterface& Interface)
    {
#if UE_VERSION_OLDER_THAN(5, 6, 0)
        return Interface.Version;
#else
        return Interface.Metadata.Version;
#endif
    }

    inline const TArray<FMetasoundFrontendInterfaceUClassOptions>& GetInterfaceUClassOptions(const FMetasoundFrontendInterface& Interface)
    {
#if UE_VERSION_OLDER_THAN(5, 6, 0)
        return Interface.UClassOptions;
#else
        return Interface.Metadata.UClassOptions;
#endif
    }

    // Whether a registered interface may be detached from an asset of the given UClass.
    // The engine stores per-UClass modifiability in the interface's UClassOptions; e.g.
    // UMetaSoundSource's default output-format interfaces (UE.OutputFormat.Mono, etc.)
    // carry bIsModifiable=false (MetasoundOutputFormatInterfaces.cpp), and
    // RemoveInterface(FName) refuses these even though they ARE present on the document
    // (MetasoundFrontendDocumentBuilder.cpp: returns false at the "is not set to be
    // modifiable for given UClass" branch). Default-when-no-match policy (one place):
    //   - no UClassOptions at all  -> modifiable (the interface is not UClass-gated).
    //   - options present, none match this class -> NOT modifiable (the interface is
    //     gated and this class is not on the allow-list).
    //   - a matching entry          -> its bIsModifiable.
    inline bool IsInterfaceModifiableForClass(
        const FMetasoundFrontendInterface& Interface,
        const FTopLevelAssetPath& AssetClassPath)
    {
        const TArray<FMetasoundFrontendInterfaceUClassOptions>& UClassOptions =
            GetInterfaceUClassOptions(Interface);
        if (UClassOptions.IsEmpty())
        {
            return true;
        }

        for (const FMetasoundFrontendInterfaceUClassOptions& Option : UClassOptions)
        {
            if (Option.ClassPath == AssetClassPath)
            {
                return Option.bIsModifiable;
            }
        }

        return false;
    }

    // FMetasoundFrontendClass's default interface (its Inputs/Outputs). In 5.6 the
    // public `Interface` member was deprecated in favor of GetDefaultInterface();
    // on 5.4/5.5 GetDefaultInterface() does not exist, so read the member directly.
    inline const FMetasoundFrontendClassInterface& GetClassDefaultInterface(const FMetasoundFrontendClass& Class)
    {
#if UE_VERSION_OLDER_THAN(5, 6, 0)
        return Class.Interface;
#else
        return Class.GetDefaultInterface();
#endif
    }

    // FMetasoundFrontendClassInput default-literal access. 5.5 paged the input
    // defaults: writes go through InitDefault() and reads through
    // FindConstDefault(DefaultPageID). The legacy DefaultLiteral member was
    // deprecated in 5.5 ("rolled into DefaultLiterals Array") and assigning it no
    // longer populates the Defaults array — so AddGraphInput's internal
    // FindConstDefaultChecked(DefaultPageID) hard-asserts (check(Literal),
    // MetasoundFrontendDocument.cpp:941). On 5.4 the single DefaultLiteral member
    // is the only storage.
    inline void SetClassInputDefault(FMetasoundFrontendClassInput& ClassInput, const FMetasoundFrontendLiteral& Literal)
    {
#if UE_VERSION_OLDER_THAN(5, 5, 0)
        ClassInput.DefaultLiteral = Literal;
#else
        ClassInput.InitDefault(Literal);
#endif
    }

    // Returns the input's default literal, or nullptr if none. On 5.5+ this reads
    // the default page (Metasound::Frontend::DefaultPageID), which is where the
    // single legacy DefaultLiteral was migrated.
    inline const FMetasoundFrontendLiteral* FindClassInputDefault(const FMetasoundFrontendClassInput& ClassInput)
    {
#if UE_VERSION_OLDER_THAN(5, 5, 0)
        return &ClassInput.DefaultLiteral;
#else
        return ClassInput.FindConstDefault(Metasound::Frontend::DefaultPageID);
#endif
    }

    // A graph class's default (page-0) graph. Paged graphs landed in 5.5: the legacy
    // `.Graph` member is deprecated there and never populated by the document builder
    // (which writes PagedGraphs only), so 5.5 must already read via
    // FindConstGraphChecked(DefaultPageID). Only 5.3/5.4 hold the graph in the single
    // `.Graph` member (no pages).
    inline const FMetasoundFrontendGraph& GetGraphClassConstGraph(const FMetasoundFrontendGraphClass& GraphClass)
    {
#if UE_VERSION_OLDER_THAN(5, 5, 0)
        return GraphClass.Graph;
#else
        return GraphClass.FindConstGraphChecked(Metasound::Frontend::DefaultPageID);
#endif
    }

    // Pointer form of the above (null if absent). On 5.3/5.4 the single `.Graph`
    // always exists, so this never returns null there.
    inline const FMetasoundFrontendGraph* FindGraphClassConstGraph(const FMetasoundFrontendGraphClass& GraphClass)
    {
#if UE_VERSION_OLDER_THAN(5, 5, 0)
        return &GraphClass.Graph;
#else
        return GraphClass.FindConstGraph(Metasound::Frontend::DefaultPageID);
#endif
    }
}
#endif // __has_include("MetasoundFrontendDocument.h")

// ---------------------------------------------------------------------------
// Registered-interface enumeration compat.
//
// UE 5.8 deprecated ISearchEngine::FindAllInterfaces() and turned it into a
// non-pure-virtual stub that returns an empty array. The replacement is
// ISearchEngine::FindAllInterfaceVersions() (a 5.8 addition returning
// FMetasoundFrontendVersion) refined through IInterfaceRegistry::FindInterface
// (the version-keyed overload, also 5.8-only). On 5.3-5.7 FindAllInterfaces()
// is the working pure-virtual, so keep using it there. This helper hands every
// call site a single API that yields the full FMetasoundFrontendInterface list
// on all engines.
// ---------------------------------------------------------------------------
#if __has_include("MetasoundFrontendSearchEngine.h")
#include "MetasoundFrontendSearchEngine.h"
#if !UE_VERSION_OLDER_THAN(5, 8, 0)
#include "Interfaces/MetasoundFrontendInterfaceRegistry.h"
#endif

// Single predicate for "the MetaSound interface search engine is available." It is what
// maps an attached-interface version to its declared vertex names, so registry-dependent
// features (e.g. describe_metasound's isInterfaceMember tagging) gate on this one macro
// instead of each call site re-testing __has_include("MetasoundFrontendSearchEngine.h").
// Defined beside FindAllFrontendInterfaces() because they share the availability condition.
#define PW_METASOUND_HAS_SEARCH_ENGINE 1

namespace PinWright::MetaSound
{
    inline TArray<FMetasoundFrontendInterface> FindAllFrontendInterfaces()
    {
#if UE_VERSION_OLDER_THAN(5, 8, 0)
        return Metasound::Frontend::ISearchEngine::Get().FindAllInterfaces();
#else
        TArray<FMetasoundFrontendInterface> Interfaces;
        const TArray<FMetasoundFrontendVersion> Versions =
            Metasound::Frontend::ISearchEngine::Get().FindAllInterfaceVersions();
        Metasound::Frontend::IInterfaceRegistry& Registry = Metasound::Frontend::IInterfaceRegistry::Get();
        Interfaces.Reserve(Versions.Num());
        for (const FMetasoundFrontendVersion& Version : Versions)
        {
            FMetasoundFrontendInterface Interface;
            if (Registry.FindInterface(Version, Interface))
            {
                Interfaces.Add(MoveTemp(Interface));
            }
        }
        return Interfaces;
#endif
    }
}
#else
#define PW_METASOUND_HAS_SEARCH_ENGINE 0
#endif // __has_include("MetasoundFrontendSearchEngine.h")

// ---------------------------------------------------------------------------
// MetaSound preset construction (UE 5.8 document templates).
//
// `UMetaSoundBaseFactory::ReferencedMetaSoundObject` is
// `UPROPERTY(Transient, meta = (Deprecated = 5.8, DeprecationMessage = "Use document template
// instead"))` (MetasoundFactory.h:23-24) and has ZERO readers left anywhere in the MetaSound
// plugin. The `meta=(Deprecated=...)` spelling emits NO compiler warning, so assigning it
// compiles clean, does nothing, and the factory hands back an ordinary EMPTY patch/source that
// merely looks like a preset to a caller reading the RPC response.
//
// The live 5.8 path is the factory's `Template`:
//   * UMetaSoundFactory / UMetaSoundSourceFactory::FactoryCreateNew forward `Template` and
//     `SelectedObjects` into UMetaSoundEditorSubsystem::InitAsset
//     (MetasoundFactory.cpp:41-45 and :66-70).
//   * InitAsset stores the template on the document (SetTemplate ->
//     FMetasoundFrontendDocument::Template, MetasoundFrontendDocumentBuilder.cpp:5931-5937),
//     calls FMetaSoundFrontendPresetTemplate::OnAssetInitialized, then ConfigureDocument
//     (MetasoundEditorSubsystem.cpp:376-382).
//   * ConfigureDocument is what actually makes it a preset: it locks the graph and runs
//     FRebuildPresetRootGraph against the parent's builder
//     (MetasoundFrontendPresetTemplate.cpp, ConfigureDocument).
//
// MakeMetaSoundPresetTemplate mirrors the engine's own (deprecated) InitAsset overload
// verbatim — MetasoundEditorSubsystem.cpp:323-324 — which is the canonical construction.
//
// `FMetasoundFrontendDocument::Template` is a serialized UPROPERTY (MetasoundFrontendDocument.h:2075),
// so presetness survives to the .uasset and can be asserted against disk rather than against
// the object just written.
// ---------------------------------------------------------------------------
#if PW_METASOUND_PATHUTILS_HAS_DOCUMENT_INTERFACE && __has_include("DocumentTemplates/MetasoundFrontendPresetTemplate.h")
#include "DocumentTemplates/MetasoundFrontendDocumentTemplate.h"
#include "DocumentTemplates/MetasoundFrontendPresetTemplate.h"
#include "StructUtils/InstancedStruct.h"

// Shared feature gate. Lives in this header (not in a handler .cpp) so test TUs see the same
// value — a macro defined only in a .cpp expands to 0 in a test TU with no warning, and the
// guarded test body silently becomes dead code (docs/lessons.md).
#define PW_METASOUND_HAS_PRESET_TEMPLATE 1

namespace PinWright::MetaSound
{
    // The document template that turns a freshly created MetaSound into a preset of `Parent`.
    // Assign to UMetaSoundBaseFactory::Template BEFORE FactoryCreateNew.
    inline TInstancedStruct<FMetaSoundFrontendDocumentTemplate> MakeMetaSoundPresetTemplate(UObject* Parent)
    {
        TInstancedStruct<FMetaSoundFrontendDocumentTemplate> PresetTemplate =
            TInstancedStruct<FMetaSoundFrontendDocumentTemplate>::Make<FMetaSoundFrontendPresetTemplate>();
        PresetTemplate.GetMutable<FMetaSoundFrontendPresetTemplate>().Parent = Parent;
        return PresetTemplate;
    }

    // The one definition of "this MetaSound really is a preset": returns the parent MetaSound
    // its document is a preset OF, or nullptr when the document carries no preset template.
    // A blank (non-preset) patch or source has an empty FMetasoundFrontendDocument::Template and
    // therefore returns nullptr here — which is exactly what the deprecated
    // ReferencedMetaSoundObject path produced while reporting success.
    inline UObject* FindMetaSoundPresetParent(UObject* Asset)
    {
        if (!Asset)
        {
            return nullptr;
        }
        const IMetaSoundDocumentInterface* DocInterface = Cast<IMetaSoundDocumentInterface>(Asset);
        if (!DocInterface)
        {
            return nullptr;
        }
        const FMetaSoundFrontendPresetTemplate* PresetTemplate =
            PW_METASOUND_GET_CONST_DOCUMENT(DocInterface).Template.GetPtr<FMetaSoundFrontendPresetTemplate>();
        return PresetTemplate ? PresetTemplate->Parent.GetObject() : nullptr;
    }
}
#else
#define PW_METASOUND_HAS_PRESET_TEMPLATE 0
#endif // preset document template available

// ---------------------------------------------------------------------------
// "Is this document a preset?" — the one reader every describe/dump/decompile surface must use.
//
// FMetasoundFrontendGraphClassPresetOptions::bIsPreset (MetasoundFrontendDocument.h:1901) is
// `UPROPERTY(meta = (DeprecatedProperty, DeprecationMessage = "5.8 - Preset options are now
// serialized in the Preset MetaSound Template. See 'FMetaSoundFrontendPresetTemplate'"))`.
// On 5.8 it is not merely unused — the versioning pass READS it once and then CLEARS it
// (MetasoundFrontendDocumentVersioning.cpp, the preset-options migration), and nothing sets it
// again, so it is permanently false for every preset the 5.8 editor or this plugin creates.
// Reading it compiles clean (the DeprecatedProperty meta emits no C++ warning) and silently
// answers "not a preset" for every preset. The engine's own predicate is
// FMetaSoundFrontendDocumentBuilder::IsPreset() -> IsDocumentConfiguredBy<FPresetTemplate>()
// (MetasoundFrontendDocumentBuilder.cpp:4423-4428); this is the same test without needing a
// builder in hand.
// ---------------------------------------------------------------------------
#if __has_include("MetasoundFrontendDocument.h")
namespace PinWright::MetaSound
{
    inline bool IsMetaSoundDocumentPreset(const FMetasoundFrontendDocument& Document)
    {
#if PW_METASOUND_HAS_PRESET_TEMPLATE
        return Document.Template.GetPtr<FMetaSoundFrontendPresetTemplate>() != nullptr;
#else
        // Pre-template engines: the legacy flag is the only storage and is still populated.
        return Document.RootGraph.PresetOptions.bIsPreset;
#endif
    }
}
#endif // __has_include("MetasoundFrontendDocument.h")
