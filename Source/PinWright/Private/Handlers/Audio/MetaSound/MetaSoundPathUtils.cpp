// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "MetaSoundPathUtils.h"

#include "Utils/PathUtils.h"
// SaveAssetToDiskReportingPresence + AddAssetSaveReport — the shared real-save path and the
// shared save-report shape SaveMetaSoundAndReport routes every MetaSound verb through.
#include "Utils/AssetUtils.h"
#include "Utils/AssetSaveState.h"
#include "PinWrightSubsystem.h"
#include "Dom/JsonObject.h"
#include "Misc/PackageName.h"

// FVersioningManager backs WaitForDocumentVersioning (see the header for why the wait exists).
// The manager is a 5.8 addition and its whole header is #if WITH_EDITORONLY_DATA; on 5.3-5.7
// the header does not exist at all and PostLoad versions the document synchronously, so both
// arms of this gate mean "nothing to wait for".
#if WITH_EDITORONLY_DATA && __has_include("MetasoundFrontendDocumentVersioning.h")
#include "MetasoundFrontendDocumentVersioning.h"
#define PW_METASOUND_HAS_VERSIONING_MANAGER 1
#else
#define PW_METASOUND_HAS_VERSIONING_MANAGER 0
#endif

namespace PinWright::MetaSound
{
#if PW_METASOUND_PATHUTILS_HAS_DOCUMENT_INTERFACE
    UObject* LoadMetaSoundDocumentAsset(const FString& AssetPath)
    {
        FString ResolveError;
        UObject* LoadedAsset = ResolveUObjectByPath(AssetPath, ResolveError);

        // ResolveUObjectByPath maps a bare long package path (e.g. "/Game/.../MS_Foo")
        // to the UPackage via StaticFindObject; UPackage is non-null but does not
        // implement IMetaSoundDocumentInterface. Retry with the "Package.AssetName"
        // object-path form so the inner document asset is returned.
        const bool bIsDocumentObject =
            LoadedAsset != nullptr && Cast<IMetaSoundDocumentInterface>(LoadedAsset) != nullptr;
        if (!bIsDocumentObject && FPackageName::IsValidLongPackageName(AssetPath))
        {
            const FString AssetName = FPackageName::GetLongPackageAssetName(AssetPath);
            const FString ObjectPath = FString::Printf(TEXT("%s.%s"), *AssetPath, *AssetName);
            if (UObject* DocumentAsset = ResolveUObjectByPath(ObjectPath, ResolveError))
            {
                WaitForDocumentVersioning(DocumentAsset);
                return DocumentAsset;
            }
        }

        // Every mutator reaches its document through this gate, and the document builder they
        // then construct primes its cache through a Checked accessor — so the wait belongs
        // here, at the single point where the plugin turns a path into a MetaSound object,
        // rather than at each of the accessors that would otherwise abort the editor.
        WaitForDocumentVersioning(LoadedAsset);
        return LoadedAsset;
    }

    bool TryMakeMetaSoundDocumentInterface(
        UObject* Asset,
        TScriptInterface<IMetaSoundDocumentInterface>& OutDocumentInterface)
    {
        OutDocumentInterface = TScriptInterface<IMetaSoundDocumentInterface>();
        IMetaSoundDocumentInterface* DocumentInterface = Cast<IMetaSoundDocumentInterface>(Asset);
        if (!Asset || !DocumentInterface)
        {
            return false;
        }

        OutDocumentInterface.SetObject(Asset);
        OutDocumentInterface.SetInterface(DocumentInterface);
        return true;
    }

    UObject* LoadMetaSoundDocumentObject(const FString& AssetPath)
    {
        UObject* LoadedAsset = LoadMetaSoundDocumentAsset(AssetPath);
        // Only return MetaSound *documents* (Source or Patch). A path that loads a
        // non-MetaSound asset returns nullptr so callers keep emitting the single
        // ASSET_NOT_FOUND the legacy typed-cast gate produced — the sole behavior
        // change is that a UMetaSoundPatch is now accepted instead of rejected.
        if (!LoadedAsset || !Cast<IMetaSoundDocumentInterface>(LoadedAsset))
        {
            return nullptr;
        }
        return LoadedAsset;
    }
#endif

#if PW_METASOUND_HAS_VERSIONING_MANAGER
    void WaitForDocumentVersioning(const UObject* MetaSoundAsset)
    {
        if (!MetaSoundAsset)
        {
            return;
        }

        // Flushes the deferred soft-reference load first and then joins the versioning task,
        // so on return the document is migrated. Returns immediately for an object that has no
        // versioning task registered (already migrated, or not a serialized MetaSound asset).
        ::Metasound::Frontend::FVersioningManager::Get().WaitUntilVersioningComplete(*MetaSoundAsset);
    }
#else
    void WaitForDocumentVersioning(const UObject* /*MetaSoundAsset*/)
    {
    }
#endif

    bool SaveMetaSoundAndReport(
        const TSharedPtr<FJsonObject>& Result,
        UObject* MetaSoundAsset,
        bool bSaveRequested)
    {
        if (!Result.IsValid() || !MetaSoundAsset)
        {
            return false;
        }

        if (!bSaveRequested)
        {
            // Nothing was attempted, so nothing is claimed. NotRequested is the state that tells
            // a caller reading saved:false that no flush is owed by THIS call.
            AddAssetSaveReport(Result, /*bSaveRequested=*/false, /*bSavedToDisk=*/false,
                EAssetSaveState::NotRequested);
            return false;
        }

        EAssetSaveState SaveState = EAssetSaveState::NotRequested;
        const bool bSavedToDisk = SaveAssetToDiskReportingPresence(
            MetaSoundAsset, /*bForce=*/true, /*OutPackageName=*/nullptr, /*OutSizeBytes=*/nullptr,
            &SaveState);
        AddAssetSaveReport(Result, /*bSaveRequested=*/true, bSavedToDisk, SaveState);
        return bSavedToDisk;
    }
}
