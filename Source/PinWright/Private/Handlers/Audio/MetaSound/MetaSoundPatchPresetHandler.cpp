// Copyright (c) 2026 Alexander Penkin. MIT License.

// MetaSoundPatchPresetHandler.cpp
// Implements audio.authoring.create_metasound_patch and
// audio.authoring.create_metasound_preset RPCs.
//
// Patch creation:   UMetaSoundFactory -> UMetaSoundPatch
// Preset creation:  based on the referenced asset's class —
//                     UMetaSoundSourceFactory (source preset) or
//                     UMetaSoundFactory (patch preset)
//                   The parent link travels on the factory's `Template`
//                   (TInstancedStruct<FMetaSoundFrontendPresetTemplate>), NOT on the
//                   deprecated `ReferencedMetaSoundObject` — see MetaSoundPathUtils.h for the
//                   engine citation chain. Presetness is verified before success is reported.
//
// The optional "overrides" param (initial input overrides at preset creation)
// is deferred — callers should use audio.authoring.set_metasound_default
// post-creation instead.

#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/ErrorCodes.h"
#include "Handlers/ParamSpec.h"
#include "Handlers/Audio/AudioPackagePathGuard.h"
#include "PinWrightHelpers.h"
#include "Utils/AssetUtils.h"
#include "Handlers/Audio/MetaSound/MetaSoundPathUtils.h"
#include "Compat/EngineVersionCompat.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "UObject/SavePackage.h"
#include "Misc/PackageName.h"

// MetaSound source class
#if __has_include("MetasoundSource.h")
#include "MetasoundSource.h"
#define MCP_HAS_METASOUND 1
#else
#define MCP_HAS_METASOUND 0
#endif

// UMetaSoundPatch class (Metasound.h)
#if __has_include("Metasound.h")
#include "Metasound.h"
#endif

// Editor factories
#if __has_include("MetasoundFactory.h")
#include "MetasoundFactory.h"
#define MCP_HAS_METASOUND_FACTORY 1
#else
#define MCP_HAS_METASOUND_FACTORY 0
#endif


// =========================================================================
// audio.authoring.create_metasound_patch
// =========================================================================

REGISTER_RPC_HANDLER("audio.authoring.create_metasound_patch", "audio.authoring",
    "Create an empty UMetaSoundPatch asset (a reusable MetaSound sub-graph). "
    "Use audio.authoring.add_metasound_input / add_metasound_output / "
    "add_metasound_node to build the graph, then connect_metasound_nodes to wire it.",
    RPC_PARAMS(
        RPC_PARAM_REQ("name", "string", "Name of the MetaSound Patch asset"),
        RPC_PARAM_OPT("path", "path", "Content path (default /Game/Audio/MetaSounds)"),
        RPC_PARAM_DEF("save", "boolean", "Save after creation", "true")
    ))
{
#if MCP_HAS_METASOUND && MCP_HAS_METASOUND_FACTORY

    FString Name;
    if (!Ctx.RequireString(TEXT("name"), Name)) return true;

    FString Path = NormalizeContentAssetPath(Ctx.GetString(TEXT("path"), TEXT("/Game/Audio/MetaSounds")));
    bool bSave = Ctx.GetBool(TEXT("save"), true);

    FString PackagePath;
    if (!PinWrightAudioPackagePath::ComposeAudioAssetPackagePathOrRefuse(Ctx, Path, Name, PackagePath))
    {
        return true;
    }

    UPackage* Package = CreatePackage(*PackagePath);
    if (!Package)
    {
        Ctx.SendError(TEXT("PACKAGE_ERROR"), TEXT("Failed to create package"));
        return true;
    }

    UMetaSoundFactory* Factory = NewObject<UMetaSoundFactory>();
    UMetaSoundPatch* Patch = Cast<UMetaSoundPatch>(
        Factory->FactoryCreateNew(UMetaSoundPatch::StaticClass(), Package,
                                  FName(*Name), RF_Public | RF_Standalone,
                                  nullptr, GWarn));

    if (!Patch)
    {
        Ctx.SendError(TEXT("CREATE_FAILED"), TEXT("Failed to create MetaSound Patch asset"));
        return true;
    }

    // Register the brand-new asset with the asset registry before saving. Without
    // this, UEditorAssetLibrary::SaveLoadedAsset (inside SaveAssetToDiskReportingPresence)
    // does not write a never-before-seen in-memory package to disk, so save:true
    // would report saved:false. Mirrors the niagara.create_system / create_emitter path.
    FAssetRegistryModule::AssetCreated(Patch);

    TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject());
    Result->SetStringField(TEXT("assetPath"), Patch->GetPathName());
    Result->SetStringField(TEXT("message"), FString::Printf(TEXT("MetaSound Patch '%s' created"), *Name));
    Result->SetStringField(TEXT("className"), TEXT("UMetaSoundPatch"));
    // One save path for the whole MetaSound surface: a real disk write plus the shared
    // {saveRequested, saved, pendingFlush, saveState, saveDetail} report. This used to be an
    // equivalent block open-coded here; it moved into the helper so the create verbs and the
    // mutators cannot drift apart (B-metasound-create-save-no-disk-write).
    PinWright::MetaSound::SaveMetaSoundAndReport(Result, Patch, bSave);
    AddAssetVerification(Result, Patch);
    Ctx.SendSuccess(Result);
    return true;

#else
    Ctx.SendError(TEXT("METASOUND_NOT_AVAILABLE"),
        TEXT("MetaSound support (MetasoundSource.h + MetasoundFactory.h) not available in this engine version"));
    return true;
#endif // MCP_HAS_METASOUND && MCP_HAS_METASOUND_FACTORY
}

// =========================================================================
// audio.authoring.create_metasound_preset
// =========================================================================

REGISTER_RPC_HANDLER("audio.authoring.create_metasound_preset", "audio.authoring",
    "Create a MetaSound preset asset that references an existing MetaSoundSource or "
    "MetaSoundPatch and overrides only its input defaults. "
    "If the referenced asset is a UMetaSoundSource, a source preset is produced. "
    "If it is a UMetaSoundPatch, a patch preset is produced. "
    "Post-creation input overrides can be applied via audio.authoring.set_metasound_default.",
    RPC_PARAMS(
        RPC_PARAM_REQ("name", "string", "Name of the new preset asset"),
        RPC_PARAM_REQ("referencedSource", "path", "Asset path of the MetaSoundSource or MetaSoundPatch to base this preset on"),
        RPC_PARAM_OPT("path", "path", "Content path (default /Game/Audio/MetaSounds)"),
        RPC_PARAM_DEF("save", "boolean", "Save after creation", "true")
    ))
{
// PW_METASOUND_HAS_PRESET_TEMPLATE (MetaSoundPathUtils.h) gates the 5.8 document-template
// construction. Without it there is no working way to author a preset from here — the older
// ReferencedMetaSoundObject path is a silent no-op on 5.8 — so refuse rather than hand back a
// blank asset labelled a preset.
#if MCP_HAS_METASOUND && MCP_HAS_METASOUND_FACTORY && PW_METASOUND_HAS_PRESET_TEMPLATE

#if (UE_VERSION_NEWER_THAN_OR_EQUAL(5, 5, 0) && UE_VERSION_OLDER_THAN(5, 6, 0))
    // UE 5.5 only: UMetaSound*Factory::FactoryCreateNew with ReferencedMetaSoundObject
    // set routes through UMetaSoundEditorSubsystem::InitAsset -> Builder.ConvertToPreset(doc),
    // and 5.5's ConvertToPreset has a buggy default argument of TSharedRef = {} (a
    // default-constructed TSharedRef). That hits the internal-only TSharedRef() ctor and
    // hard-fatals ("The TSharedRef() constructor is for internal usage only for hot-reload
    // purposes", CoreMisc.cpp:435). The default arg is TSharedPtr (safe null) on 5.4/5.6/5.7,
    // so preset creation is only unrunnable on 5.5. Reject cleanly rather than crash.
    //
    // Currently UNREACHABLE, deliberately kept: the outer guard requires
    // PW_METASOUND_HAS_PRESET_TEMPLATE, whose header does not exist on 5.5, so a 5.5 build takes
    // the METASOUND_NOT_AVAILABLE branch before reaching here. A backport that restores the
    // pre-template construction under #else must keep this arm — the crash it documents is real
    // and only 5.5 has it.
    Ctx.SendUnsupportedEngineVersion(TEXT("5.6"), TEXT("Creating a MetaSound preset asset"));
    return true;
#else
    FString Name;
    if (!Ctx.RequireString(TEXT("name"), Name)) return true;

    FString ReferencedPath;
    if (!Ctx.RequireString(TEXT("referencedSource"), ReferencedPath)) return true;

    FString Path = NormalizeContentAssetPath(Ctx.GetString(TEXT("path"), TEXT("/Game/Audio/MetaSounds")));
    bool bSave = Ctx.GetBool(TEXT("save"), true);

    // Normalize the referenced asset path
    FString NormalizedRefPath = NormalizeContentAssetPath(ReferencedPath);
    if (NormalizedRefPath.IsEmpty())
    {
        Ctx.SendError(TEXT("INVALID_REFERENCED_ASSET"), TEXT("referencedSource path is invalid or rejected"));
        return true;
    }

    // Load the referenced asset
    UObject* Referenced = StaticLoadObject(UObject::StaticClass(), nullptr, *NormalizedRefPath);
    if (!Referenced)
    {
        Ctx.SendError(TEXT("INVALID_REFERENCED_ASSET"),
            FString::Printf(TEXT("Could not load referenced asset at '%s'"), *NormalizedRefPath));
        return true;
    }

    // Determine which factory to use based on the referenced asset's class
    UMetaSoundSource* RefSource = Cast<UMetaSoundSource>(Referenced);
    UMetaSoundPatch*  RefPatch  = Cast<UMetaSoundPatch>(Referenced);

    if (!RefSource && !RefPatch)
    {
        Ctx.SendError(TEXT("INVALID_REFERENCED_ASSET"),
            FString::Printf(
                TEXT("Referenced asset is '%s', expected UMetaSoundSource or UMetaSoundPatch"),
                *Referenced->GetClass()->GetName()));
        return true;
    }

    FString PackagePath;
    if (!PinWrightAudioPackagePath::ComposeAudioAssetPackagePathOrRefuse(Ctx, Path, Name, PackagePath))
    {
        return true;
    }

    UPackage* Package = CreatePackage(*PackagePath);
    if (!Package)
    {
        Ctx.SendError(TEXT("PACKAGE_ERROR"), TEXT("Failed to create package"));
        return true;
    }

    UObject* NewAsset = nullptr;
    FString ClassName;

    // The parent link rides on the factory's `Template`. Setting the older
    // `ReferencedMetaSoundObject` instead is a SILENT no-op on 5.8 (meta=(Deprecated=5.8),
    // no compiler warning, zero engine readers) and yields a blank non-preset — the whole
    // point of this handler's fix. Citation chain: MetaSoundPathUtils.h.
    if (RefSource)
    {
        UMetaSoundSourceFactory* Factory = NewObject<UMetaSoundSourceFactory>();
        Factory->Template = PinWright::MetaSound::MakeMetaSoundPresetTemplate(Referenced);
        Factory->SelectedObjects.Add(Referenced);
        NewAsset = Factory->FactoryCreateNew(UMetaSoundSource::StaticClass(), Package,
                                             FName(*Name), RF_Public | RF_Standalone,
                                             nullptr, GWarn);
        ClassName = TEXT("UMetaSoundSource");
    }
    else
    {
        UMetaSoundFactory* Factory = NewObject<UMetaSoundFactory>();
        Factory->Template = PinWright::MetaSound::MakeMetaSoundPresetTemplate(Referenced);
        Factory->SelectedObjects.Add(Referenced);
        NewAsset = Factory->FactoryCreateNew(UMetaSoundPatch::StaticClass(), Package,
                                             FName(*Name), RF_Public | RF_Standalone,
                                             nullptr, GWarn);
        ClassName = TEXT("UMetaSoundPatch");
    }

    if (!NewAsset)
    {
        Ctx.SendError(TEXT("CREATE_FAILED"), TEXT("Failed to create MetaSound preset asset"));
        return true;
    }

    // Verify presetness against the DOCUMENT before reporting anything, and refuse rather than
    // report a success whose referencedAssetPath describes a link that does not exist. This is
    // the assertion the old code lacked: FactoryCreateNew returns a perfectly valid empty
    // patch/source when the parent never reaches the document, so a non-null return proves
    // nothing about presetness.
    UObject* AppliedParent = PinWright::MetaSound::FindMetaSoundPresetParent(NewAsset);
    if (AppliedParent != Referenced)
    {
        Ctx.SendError(ErrorCodes::ERR_PRESET_NOT_APPLIED,
            FString::Printf(
                TEXT("Created '%s' but its document carries no preset template referencing '%s' ")
                TEXT("(parent on document: '%s') — the asset would be a blank MetaSound, not a preset"),
                *Name, *Referenced->GetPathName(),
                AppliedParent ? *AppliedParent->GetPathName() : TEXT("<none>")));
        return true;
    }

    // Register the brand-new asset with the asset registry before saving (see the
    // create_metasound_patch handler above) so SaveAssetToDiskReportingPresence
    // actually flushes the never-before-seen package to disk.
    FAssetRegistryModule::AssetCreated(NewAsset);

    TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject());
    Result->SetStringField(TEXT("assetPath"), NewAsset->GetPathName());
    Result->SetStringField(TEXT("message"), FString::Printf(TEXT("MetaSound preset '%s' created"), *Name));
    Result->SetStringField(TEXT("className"), ClassName);
    Result->SetStringField(TEXT("referencedAssetPath"), Referenced->GetPathName());
    // Read back off the created asset's document, not off the request: this is the field that
    // was a lie before the fix (referencedAssetPath described a link the asset did not carry).
    // Unconditionally true here only because the PRESET_NOT_APPLIED gate above already
    // returned for every other outcome.
    Result->SetBoolField(TEXT("isPreset"), true);
    Result->SetStringField(TEXT("presetParentAssetPath"), AppliedParent->GetPathName());
    // Same one save path as create_metasound_patch above.
    PinWright::MetaSound::SaveMetaSoundAndReport(Result, NewAsset, bSave);
    AddAssetVerification(Result, NewAsset);
    Ctx.SendSuccess(Result);
    return true;
#endif // UE 5.5-only preset crash guard

#else
    Ctx.SendError(TEXT("METASOUND_NOT_AVAILABLE"),
        TEXT("MetaSound preset support (MetasoundSource.h + MetasoundFactory.h + ")
        TEXT("DocumentTemplates/MetasoundFrontendPresetTemplate.h) not available in this engine version"));
    return true;
#endif // MCP_HAS_METASOUND && MCP_HAS_METASOUND_FACTORY && PW_METASOUND_HAS_PRESET_TEMPLATE
}
