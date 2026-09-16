// Copyright (c) 2026 Alexander Penkin. MIT License.

// MetaSoundIOMutationHandler.cpp
// Implements remove and rename RPCs for MetaSound graph-interface vertices
// (inputs and outputs). Wrappers around FMetaSoundFrontendDocumentBuilder
// remove/set-name APIs.
//
// RPCs registered here:
//   audio.authoring.remove_metasound_input
//   audio.authoring.remove_metasound_output
//   audio.authoring.rename_metasound_input
//   audio.authoring.rename_metasound_output

#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/Audio/MetaSound/MetaSoundLiteralFromTypeName.h"
#include "Handlers/Audio/MetaSound/MetaSoundPathUtils.h"
#include "PinWrightHelpers.h"
#include "Utils/AssetUtils.h"
#include "Compat/EngineVersionCompat.h"


// MetaSound support (UE 5.0+)
#if __has_include("MetasoundSource.h")
#if __has_include("MetasoundDocumentInterface.h")
#include "MetasoundDocumentInterface.h"
#define MCP_HAS_METASOUND_DOCUMENT_INTERFACE 1
#else
#define MCP_HAS_METASOUND_DOCUMENT_INTERFACE 0
#endif
#include "MetasoundSource.h"
#define MCP_HAS_METASOUND 1
#else
#define MCP_HAS_METASOUND_DOCUMENT_INTERFACE 0
#define MCP_HAS_METASOUND 0
#endif

#if __has_include("Metasound.h")
#include "Metasound.h"
#endif

#if __has_include("MetasoundBuilderSubsystem.h")
#include "MetasoundBuilderSubsystem.h"
#endif

// MetaSound Frontend Document Builder (UE 5.3+)
#if __has_include("MetasoundFrontendDocumentBuilder.h")
#include "MetasoundFrontendDocumentBuilder.h"
#include "MetasoundFrontendDocument.h"
#define MCP_HAS_METASOUND_FRONTEND 1
#else
#define MCP_HAS_METASOUND_FRONTEND 0
#endif



// =========================================================================
// audio.authoring.remove_metasound_input
// =========================================================================

REGISTER_RPC_HANDLER("audio.authoring.remove_metasound_input", "audio.authoring",
    "Remove a graph input from a MetaSound by name",
    RPC_PARAMS(
        RPC_PARAM_REQ("assetPath",  "path",    "Asset path of the MetaSound"),
        RPC_PARAM_REQ("inputName",  "string",  "Name of the input to remove"),
        RPC_PARAM_DEF("save",       "boolean", "Save after modification", "true")
    ))
{
#if MCP_HAS_METASOUND && MCP_HAS_METASOUND_FRONTEND
    FString AssetPath = NormalizeContentAssetPath(Ctx.GetString(TEXT("assetPath")));
    FString InputName;
    if (!Ctx.RequireString(TEXT("inputName"), InputName)) return true;

    bool bSave = Ctx.GetBool(TEXT("save"), true);

    if (AssetPath.IsEmpty())
    {
        Ctx.SendError(TEXT("MISSING_PATH"), TEXT("Asset path is required"));
        return true;
    }

    // Load gate accepts Source or Patch — see LoadMetaSoundDocumentObject.
    UObject* MetaSound = PinWright::MetaSound::LoadMetaSoundDocumentObject(AssetPath);
    if (!MetaSound)
    {
        Ctx.SendError(TEXT("ASSET_NOT_FOUND"),
            FString::Printf(TEXT("Could not load MetaSound: %s"), *AssetPath));
        return true;
    }

    TScriptInterface<IMetaSoundDocumentInterface> ScriptInterface(MetaSound);
    PW_METASOUND_MAKE_BUILDER(Builder, ScriptInterface);

    MetaSound->Modify();
    const bool bRemoved = Builder.RemoveGraphInput(FName(*InputName));

    if (!bRemoved)
    {
        Ctx.SendError(TEXT("INPUT_NOT_FOUND"),
            FString::Printf(TEXT("Input '%s' not found or could not be removed"), *InputName));
        PW_METASOUND_FINISH_BUILDING(Builder);
        return true;
    }

    TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject());
    Result->SetStringField(TEXT("inputName"), InputName);
    Result->SetStringField(TEXT("message"),
        FString::Printf(TEXT("MetaSound input '%s' removed"), *InputName));
    // Flush the builder's edits into the document BEFORE the save: the save is a real disk
    // write now, and a write taken ahead of FinishBuilding would serialize the pre-edit
    // document (audio.authoring.metasound_gotchas). The old mark-dirty call wrote nothing and
    // so was order-insensitive, which is why it used to sit above this line.
    PW_METASOUND_FINISH_BUILDING(Builder);

    PinWright::MetaSound::SaveMetaSoundAndReport(Result, MetaSound, bSave);
    AddAssetVerification(Result, MetaSound);

    Ctx.SendSuccess(Result);
    return true;
#else
    Ctx.SendError(TEXT("METASOUND_NOT_AVAILABLE"), TEXT("MetaSound Frontend Builder not available"));
    return true;
#endif
}


// =========================================================================
// audio.authoring.remove_metasound_output
// =========================================================================

REGISTER_RPC_HANDLER("audio.authoring.remove_metasound_output", "audio.authoring",
    "Remove a graph output from a MetaSound by name",
    RPC_PARAMS(
        RPC_PARAM_REQ("assetPath",  "path",    "Asset path of the MetaSound"),
        RPC_PARAM_REQ("outputName", "string",  "Name of the output to remove"),
        RPC_PARAM_DEF("save",       "boolean", "Save after modification", "true")
    ))
{
#if MCP_HAS_METASOUND && MCP_HAS_METASOUND_FRONTEND
    FString AssetPath = NormalizeContentAssetPath(Ctx.GetString(TEXT("assetPath")));
    FString OutputName;
    if (!Ctx.RequireString(TEXT("outputName"), OutputName)) return true;

    bool bSave = Ctx.GetBool(TEXT("save"), true);

    if (AssetPath.IsEmpty())
    {
        Ctx.SendError(TEXT("MISSING_PATH"), TEXT("Asset path is required"));
        return true;
    }

    // Load gate accepts Source or Patch — see LoadMetaSoundDocumentObject.
    UObject* MetaSound = PinWright::MetaSound::LoadMetaSoundDocumentObject(AssetPath);
    if (!MetaSound)
    {
        Ctx.SendError(TEXT("ASSET_NOT_FOUND"),
            FString::Printf(TEXT("Could not load MetaSound: %s"), *AssetPath));
        return true;
    }

    TScriptInterface<IMetaSoundDocumentInterface> ScriptInterface(MetaSound);
    PW_METASOUND_MAKE_BUILDER(Builder, ScriptInterface);

    MetaSound->Modify();
    const bool bRemoved = Builder.RemoveGraphOutput(FName(*OutputName));

    if (!bRemoved)
    {
        Ctx.SendError(TEXT("OUTPUT_NOT_FOUND"),
            FString::Printf(TEXT("Output '%s' not found or could not be removed"), *OutputName));
        PW_METASOUND_FINISH_BUILDING(Builder);
        return true;
    }

    TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject());
    Result->SetStringField(TEXT("outputName"), OutputName);
    Result->SetStringField(TEXT("message"),
        FString::Printf(TEXT("MetaSound output '%s' removed"), *OutputName));
    // Flush the builder's edits into the document BEFORE the save: the save is a real disk
    // write now, and a write taken ahead of FinishBuilding would serialize the pre-edit
    // document (audio.authoring.metasound_gotchas). The old mark-dirty call wrote nothing and
    // so was order-insensitive, which is why it used to sit above this line.
    PW_METASOUND_FINISH_BUILDING(Builder);

    PinWright::MetaSound::SaveMetaSoundAndReport(Result, MetaSound, bSave);
    AddAssetVerification(Result, MetaSound);

    Ctx.SendSuccess(Result);
    return true;
#else
    Ctx.SendError(TEXT("METASOUND_NOT_AVAILABLE"), TEXT("MetaSound Frontend Builder not available"));
    return true;
#endif
}


// =========================================================================
// audio.authoring.rename_metasound_input
// =========================================================================

REGISTER_RPC_HANDLER("audio.authoring.rename_metasound_input", "audio.authoring",
    "Rename a MetaSound graph input, preserving its type, default, and connections",
    RPC_PARAMS(
        RPC_PARAM_REQ("assetPath", "path",    "Asset path of the MetaSound"),
        RPC_PARAM_REQ("oldName",   "string",  "Current name of the input"),
        RPC_PARAM_REQ("newName",   "string",  "New name for the input"),
        RPC_PARAM_DEF("save",      "boolean", "Save after modification", "true")
    ))
{
#if MCP_HAS_METASOUND && MCP_HAS_METASOUND_FRONTEND
    FString AssetPath = NormalizeContentAssetPath(Ctx.GetString(TEXT("assetPath")));
    FString OldName;
    if (!Ctx.RequireString(TEXT("oldName"), OldName)) return true;
    FString NewName;
    if (!Ctx.RequireString(TEXT("newName"), NewName)) return true;

    bool bSave = Ctx.GetBool(TEXT("save"), true);

    if (AssetPath.IsEmpty())
    {
        Ctx.SendError(TEXT("MISSING_PATH"), TEXT("Asset path is required"));
        return true;
    }

    // Load gate accepts Source or Patch — see LoadMetaSoundDocumentObject.
    UObject* MetaSound = PinWright::MetaSound::LoadMetaSoundDocumentObject(AssetPath);
    if (!MetaSound)
    {
        Ctx.SendError(TEXT("ASSET_NOT_FOUND"),
            FString::Printf(TEXT("Could not load MetaSound: %s"), *AssetPath));
        return true;
    }

    TScriptInterface<IMetaSoundDocumentInterface> ScriptInterface(MetaSound);
    PW_METASOUND_MAKE_BUILDER(Builder, ScriptInterface);

    MetaSound->Modify();
    // Verify input exists before attempting rename
    const FMetasoundFrontendClassInput* Existing = Builder.FindGraphInput(FName(*OldName));
    if (!Existing)
    {
        Ctx.SendError(TEXT("INPUT_NOT_FOUND"),
            FString::Printf(TEXT("Input '%s' not found"), *OldName));
        PW_METASOUND_FINISH_BUILDING(Builder);
        return true;
    }

    // SetGraphInputName is the atomic rename API, but it only exists on UE 5.6+
    // (added alongside the paged-graph builder rework). On 5.5 the equivalent
    // is SwapGraphInput, which replaces the existing graph-input vertex with a copy
    // carrying the new name while preserving TypeName, AccessType, default, and edges.
    //
    // On UE 5.4, SwapGraphInput is broken: step 3 of its implementation removes the
    // vertex via RemoveGraphOutput(name) instead of RemoveGraphInput(name) — so the
    // removal returns false and the engine hard-asserts (checkf bRemovedVertex,
    // "Failed to swap MetaSound input expected to exist"). Epic fixed this in 5.5
    // (RemoveGraphInput). The edge-preserving swap can't be replicated from the public
    // builder API (it needs the engine-internal node/edge caches), so reject the rename
    // on 5.4 rather than crash the editor.
#if UE_VERSION_OLDER_THAN(5, 5, 0)
    PW_METASOUND_FINISH_BUILDING(Builder);
    Ctx.SendUnsupportedEngineVersion(TEXT("5.5"), TEXT("Renaming a MetaSound graph input"));
    return true;
#elif UE_VERSION_OLDER_THAN(5, 6, 0)
    FMetasoundFrontendClassVertex NewVertex = *Existing;
    NewVertex.Name = FName(*NewName);
    const bool bRenamed = Builder.SwapGraphInput(*Existing, NewVertex);
#else
    const bool bRenamed = Builder.SetGraphInputName(FName(*OldName), FName(*NewName));
#endif

    // bRenamed is only declared on 5.5+ (5.4 returns early above via the unsupported path).
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 5, 0)
    if (!bRenamed)
    {
        Ctx.SendError(TEXT("RENAME_FAILED"),
            FString::Printf(TEXT("Failed to rename input '%s' to '%s'"), *OldName, *NewName));
        PW_METASOUND_FINISH_BUILDING(Builder);
        return true;
    }
#endif

    TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject());
    Result->SetStringField(TEXT("oldName"), OldName);
    Result->SetStringField(TEXT("newName"), NewName);
    Result->SetStringField(TEXT("message"),
        FString::Printf(TEXT("MetaSound input renamed '%s' -> '%s'"), *OldName, *NewName));
    // Flush the builder's edits into the document BEFORE the save: the save is a real disk
    // write now, and a write taken ahead of FinishBuilding would serialize the pre-edit
    // document (audio.authoring.metasound_gotchas). The old mark-dirty call wrote nothing and
    // so was order-insensitive, which is why it used to sit above this line.
    PW_METASOUND_FINISH_BUILDING(Builder);

    PinWright::MetaSound::SaveMetaSoundAndReport(Result, MetaSound, bSave);
    AddAssetVerification(Result, MetaSound);

    Ctx.SendSuccess(Result);
    return true;
#else
    Ctx.SendError(TEXT("METASOUND_NOT_AVAILABLE"), TEXT("MetaSound Frontend Builder not available"));
    return true;
#endif
}


// =========================================================================
// audio.authoring.rename_metasound_output
// =========================================================================

REGISTER_RPC_HANDLER("audio.authoring.rename_metasound_output", "audio.authoring",
    "Rename a MetaSound graph output, preserving its type and connections",
    RPC_PARAMS(
        RPC_PARAM_REQ("assetPath", "path",    "Asset path of the MetaSound"),
        RPC_PARAM_REQ("oldName",   "string",  "Current name of the output"),
        RPC_PARAM_REQ("newName",   "string",  "New name for the output"),
        RPC_PARAM_DEF("save",      "boolean", "Save after modification", "true")
    ))
{
#if MCP_HAS_METASOUND && MCP_HAS_METASOUND_FRONTEND
    FString AssetPath = NormalizeContentAssetPath(Ctx.GetString(TEXT("assetPath")));
    FString OldName;
    if (!Ctx.RequireString(TEXT("oldName"), OldName)) return true;
    FString NewName;
    if (!Ctx.RequireString(TEXT("newName"), NewName)) return true;

    bool bSave = Ctx.GetBool(TEXT("save"), true);

    if (AssetPath.IsEmpty())
    {
        Ctx.SendError(TEXT("MISSING_PATH"), TEXT("Asset path is required"));
        return true;
    }

    // Load gate accepts Source or Patch — see LoadMetaSoundDocumentObject.
    UObject* MetaSound = PinWright::MetaSound::LoadMetaSoundDocumentObject(AssetPath);
    if (!MetaSound)
    {
        Ctx.SendError(TEXT("ASSET_NOT_FOUND"),
            FString::Printf(TEXT("Could not load MetaSound: %s"), *AssetPath));
        return true;
    }

    TScriptInterface<IMetaSoundDocumentInterface> ScriptInterface(MetaSound);
    PW_METASOUND_MAKE_BUILDER(Builder, ScriptInterface);

    MetaSound->Modify();
    // Verify output exists before attempting rename
    const FMetasoundFrontendClassOutput* Existing = Builder.FindGraphOutput(FName(*OldName));
    if (!Existing)
    {
        Ctx.SendError(TEXT("OUTPUT_NOT_FOUND"),
            FString::Printf(TEXT("Output '%s' not found"), *OldName));
        PW_METASOUND_FINISH_BUILDING(Builder);
        return true;
    }

    // SetGraphOutputName is the atomic rename API, but it only exists on UE 5.6+.
    // On 5.4/5.5 the equivalent is SwapGraphOutput, which replaces the existing
    // graph-output vertex with a renamed copy, preserving TypeName, AccessType, and edges.
#if UE_VERSION_OLDER_THAN(5, 6, 0)
    FMetasoundFrontendClassVertex NewVertex = *Existing;
    NewVertex.Name = FName(*NewName);
    const bool bRenamed = Builder.SwapGraphOutput(*Existing, NewVertex);
#else
    const bool bRenamed = Builder.SetGraphOutputName(FName(*OldName), FName(*NewName));
#endif

    if (!bRenamed)
    {
        Ctx.SendError(TEXT("RENAME_FAILED"),
            FString::Printf(TEXT("Failed to rename output '%s' to '%s'"), *OldName, *NewName));
        PW_METASOUND_FINISH_BUILDING(Builder);
        return true;
    }

    TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject());
    Result->SetStringField(TEXT("oldName"), OldName);
    Result->SetStringField(TEXT("newName"), NewName);
    Result->SetStringField(TEXT("message"),
        FString::Printf(TEXT("MetaSound output renamed '%s' -> '%s'"), *OldName, *NewName));
    // Flush the builder's edits into the document BEFORE the save: the save is a real disk
    // write now, and a write taken ahead of FinishBuilding would serialize the pre-edit
    // document (audio.authoring.metasound_gotchas). The old mark-dirty call wrote nothing and
    // so was order-insensitive, which is why it used to sit above this line.
    PW_METASOUND_FINISH_BUILDING(Builder);

    PinWright::MetaSound::SaveMetaSoundAndReport(Result, MetaSound, bSave);
    AddAssetVerification(Result, MetaSound);

    Ctx.SendSuccess(Result);
    return true;
#else
    Ctx.SendError(TEXT("METASOUND_NOT_AVAILABLE"), TEXT("MetaSound Frontend Builder not available"));
    return true;
#endif
}
