// Copyright (c) 2026 Alexander Penkin. MIT License.

// MetaSoundVariableHandler.cpp
// Implements:
//   audio.authoring.add_metasound_variable
//   audio.authoring.remove_metasound_variable
//   audio.authoring.set_metasound_variable_default
//   audio.authoring.validate_metasound
//   audio.authoring.compile_metasound

#include "Handlers/HandlerRegistration.h"
#include "Handlers/ParamSpec.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/ErrorCodes.h"
#include "Handlers/Audio/MetaSound/MetaSoundLiteralFromTypeName.h"
#include "Handlers/Audio/MetaSound/MetaSoundLiteralParams.h"
#include "Handlers/Audio/MetaSound/MetaSoundPathUtils.h"
#include "PinWrightHelpers.h"
#include "PinWrightSubsystem.h"
#include "Misc/EngineVersionComparison.h"

#include "AssetRegistry/AssetRegistryModule.h"

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

// MetaSound Frontend Document Builder (UE 5.3+)
#if __has_include("MetasoundFrontendDocumentBuilder.h")
#include "MetasoundFrontendDocumentBuilder.h"
#include "MetasoundFrontendDocument.h"
#define MCP_HAS_METASOUND_FRONTEND 1
#else
#define MCP_HAS_METASOUND_FRONTEND 0
#endif

// MetaSound asset base — exposes RebuildReferencedAssetClasses (UE 5.3+)
#if __has_include("MetasoundAssetBase.h")
#include "MetasoundAssetBase.h"
#define MCP_HAS_METASOUND_ASSET_BASE 1
#else
#define MCP_HAS_METASOUND_ASSET_BASE 0
#endif

// UObject -> FMetasoundAssetBase resolver. RebuildReferencedAssetClasses lives on
// FMetasoundAssetBase, the common base of UMetaSoundSource AND UMetaSoundPatch, so
// resolving through the registry (instead of a typed UMetaSoundSource pointer) lets
// compile_metasound accept a Patch like the other authoring mutators.
#if __has_include("MetasoundUObjectRegistry.h")
#include "MetasoundUObjectRegistry.h"
#define MCP_HAS_METASOUND_UOBJECT_REGISTRY 1
#else
#define MCP_HAS_METASOUND_UOBJECT_REGISTRY 0
#endif


// =========================================================================
// audio.authoring.add_metasound_variable
// =========================================================================
REGISTER_RPC_HANDLER("audio.authoring.add_metasound_variable", "audio.authoring",
    "Add a typed graph variable to a MetaSound",
    RPC_PARAMS(
        RPC_PARAM_REQ("assetPath",     "path",    "Asset path of the MetaSound"),
        RPC_PARAM_REQ("variableName",  "string",  "Name for the new variable"),
        RPC_PARAM_DEF("variableType",  "string",  "Registered MetaSound data type: Float, Int/Int32, Bool/Boolean, String, Audio, Trigger, Time, WaveAsset, or any other registered name", "Float"),
        RPC_PARAM_OPT("floatValue",    "number",  "Initial float default value"),
        RPC_PARAM_OPT("intValue",      "integer", "Initial integer default value"),
        RPC_PARAM_OPT("boolValue",     "boolean", "Initial boolean default value"),
        RPC_PARAM_OPT("stringValue",   "string",  "Initial string default value"),
        RPC_PARAM_OPT("objectValue",   "path",    "Asset path of the object to bind (alias: objectPath). NOT assetPath, which names the MetaSound being edited."),
        RPC_PARAM_OPT("arrayValue",    "array",   "Values for an ARRAY-typed target, as a JSON array whose entries match the element type: numbers for Float:Array / Int32:Array, booleans for Bool:Array, strings for String:Array, asset paths for object arrays such as WaveAsset:Array. This is the ONLY param an array-typed target accepts, and a scalar-typed target refuses it. An empty array clears the value. Required to reach the Array.* node family (Array.Random Get, Array.Shuffle, Array.Get/Set/Concat), whose pins and the Weights pin of Array.Random Get are all array-typed."),
        RPC_PARAM_DEF("save",          "boolean", "Save after modification", "true")
    ))
{
#if MCP_HAS_METASOUND && MCP_HAS_METASOUND_FRONTEND
    FString AssetPath = NormalizeContentAssetPath(Ctx.GetString(TEXT("assetPath")));
    FString VariableName;
    if (!Ctx.RequireString(TEXT("variableName"), VariableName)) return true;

    FString VariableType = Ctx.GetString(TEXT("variableType"), TEXT("Float"));
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

    FMetasoundFrontendLiteral Literal;
    // Create-time: an omitted value means "seed the type's default", so the type default is allowed.
    // NoValue therefore reaches here only when variableType itself names no registered data type.
    const PinWright::MetaSound::EMetaSoundLiteralParamOutcome LiteralOutcome =
        PinWright::MetaSound::BuildMetaSoundLiteralFromParams(Ctx, VariableType,
            /*bAllowTypeDefault*/ true, Literal);
    if (LiteralOutcome == PinWright::MetaSound::EMetaSoundLiteralParamOutcome::ErrorSent)
    {
        return true;
    }
    if (LiteralOutcome != PinWright::MetaSound::EMetaSoundLiteralParamOutcome::Ok)
    {
        // §3: no fallback literal for an unknown type — name the way out instead.
        PinWright::MetaSound::SendMetaSoundUnknownTypeError(Ctx, TEXT("variableType"), VariableType);
        return true;
    }

#if UE_VERSION_OLDER_THAN(5, 6, 0)
    // Graph-variable authoring (AddGraphVariable et al.) is a UE 5.6 builder feature
    // and has no equivalent on 5.4/5.5 — return a clean, informative error rather
    // than silently doing nothing.
    (void)Literal;
    Ctx.SendUnsupportedEngineVersion(TEXT("5.6"), TEXT("Adding MetaSound graph variables"));
    return true;
#else
    TScriptInterface<IMetaSoundDocumentInterface> ScriptInterface(MetaSound);
    PW_METASOUND_MAKE_BUILDER(Builder, ScriptInterface);

    // Map the documented convenience type name (e.g. "Int") to the registry's
    // canonical data-type key (e.g. "Int32") that AddGraphVariable resolves
    // against. Without this, the documented "Int" finds no registered data type
    // and the builder returns null.
    const FString CanonicalType =
        PinWright::MetaSound::CanonicalizeMetaSoundTypeName(VariableType);

    MetaSound->Modify();
    const FMetasoundFrontendVariable* Variable = Builder.AddGraphVariable(
        FName(*VariableName), FName(*CanonicalType), &Literal);

    const bool bVariableAdded = Variable != nullptr;

    // Flush the builder's edits into the document BEFORE the save below: the save is a real
    // disk write now, and a write taken ahead of FinishBuilding would serialize the pre-edit
    // document (audio.authoring.metasound_gotchas).
    PW_METASOUND_FINISH_BUILDING(Builder);

    if (bVariableAdded)
    {
        TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject());
        Result->SetStringField(TEXT("message"),
            FString::Printf(TEXT("Variable '%s' added to MetaSound"), *VariableName));
        Result->SetStringField(TEXT("variableName"), VariableName);
        Result->SetStringField(TEXT("variableType"), VariableType);
        PinWright::MetaSound::SaveMetaSoundAndReport(Result, MetaSound, bSave);
        AddAssetVerification(Result, MetaSound);
        Ctx.SendSuccess(Result);
    }
    else
    {
        Ctx.SendError(TEXT("VARIABLE_FAILED"),
            FString::Printf(TEXT("Failed to add variable '%s' of type '%s'"), *VariableName, *VariableType));
    }

    return true;
#endif // UE_VERSION_OLDER_THAN(5, 6, 0)
#elif MCP_HAS_METASOUND
    FString VariableName = Ctx.GetString(TEXT("variableName"));
    FString VariableType = Ctx.GetString(TEXT("variableType"), TEXT("Float"));

    TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject());
    Result->SetStringField(TEXT("variableName"), VariableName);
    Result->SetStringField(TEXT("variableType"), VariableType);
    Result->SetStringField(TEXT("message"),
        FString::Printf(TEXT("Variable '%s' noted"), *VariableName));
    Result->SetStringField(TEXT("note"),
        TEXT("MetaSound Frontend Builder not available — upgrade to UE 5.3+ for full support"));
    Ctx.SendSuccess(Result);
    return true;
#else
    Ctx.SendError(TEXT("METASOUND_NOT_AVAILABLE"), TEXT("MetaSound support not available"));
    return true;
#endif
}

// =========================================================================
// audio.authoring.remove_metasound_variable
// =========================================================================
REGISTER_RPC_HANDLER("audio.authoring.remove_metasound_variable", "audio.authoring",
    "Remove a graph variable from a MetaSound",
    RPC_PARAMS(
        RPC_PARAM_REQ("assetPath",    "path",    "Asset path of the MetaSound"),
        RPC_PARAM_REQ("variableName", "string",  "Name of the variable to remove"),
        RPC_PARAM_DEF("save",         "boolean", "Save after modification", "true")
    ))
{
#if MCP_HAS_METASOUND && MCP_HAS_METASOUND_FRONTEND
    FString AssetPath = NormalizeContentAssetPath(Ctx.GetString(TEXT("assetPath")));
    FString VariableName;
    if (!Ctx.RequireString(TEXT("variableName"), VariableName)) return true;

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

#if UE_VERSION_OLDER_THAN(5, 6, 0)
    // RemoveGraphVariable is a UE 5.6 builder method with no 5.4/5.5 equivalent.
    Ctx.SendUnsupportedEngineVersion(TEXT("5.6"), TEXT("Removing MetaSound graph variables"));
    return true;
#else
    TScriptInterface<IMetaSoundDocumentInterface> ScriptInterface(MetaSound);
    PW_METASOUND_MAKE_BUILDER(Builder, ScriptInterface);

    MetaSound->Modify();
    bool bRemoved = Builder.RemoveGraphVariable(FName(*VariableName));

    // Flush the builder's edits into the document BEFORE the save below — see
    // add_metasound_variable.
    PW_METASOUND_FINISH_BUILDING(Builder);

    if (bRemoved)
    {
        TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject());
        Result->SetStringField(TEXT("message"),
            FString::Printf(TEXT("Variable '%s' removed from MetaSound"), *VariableName));
        Result->SetStringField(TEXT("variableName"), VariableName);
        PinWright::MetaSound::SaveMetaSoundAndReport(Result, MetaSound, bSave);
        AddAssetVerification(Result, MetaSound);
        Ctx.SendSuccess(Result);
    }
    else
    {
        Ctx.SendError(TEXT("VARIABLE_NOT_FOUND"),
            FString::Printf(TEXT("Variable '%s' not found in MetaSound"), *VariableName));
    }

    return true;
#endif // UE_VERSION_OLDER_THAN(5, 6, 0)
#elif MCP_HAS_METASOUND
    FString VariableName = Ctx.GetString(TEXT("variableName"));

    TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject());
    Result->SetStringField(TEXT("message"),
        FString::Printf(TEXT("Variable '%s' removal noted"), *VariableName));
    Result->SetStringField(TEXT("note"),
        TEXT("MetaSound Frontend Builder not available — upgrade to UE 5.3+ for full support"));
    Ctx.SendSuccess(Result);
    return true;
#else
    Ctx.SendError(TEXT("METASOUND_NOT_AVAILABLE"), TEXT("MetaSound support not available"));
    return true;
#endif
}

// =========================================================================
// audio.authoring.set_metasound_variable_default
// =========================================================================
REGISTER_RPC_HANDLER("audio.authoring.set_metasound_variable_default", "audio.authoring",
    "Set the default value of a MetaSound graph variable. Exactly one value param is required. objectValue binds an asset and is validated against the variable's declared data type; an array-typed variable takes arrayValue instead.",
    RPC_PARAMS(
        RPC_PARAM_REQ("assetPath",    "path",    "Asset path of the MetaSound"),
        RPC_PARAM_REQ("variableName", "string",  "Name of the variable"),
        RPC_PARAM_OPT("floatValue",   "number",  "Float default value"),
        RPC_PARAM_OPT("intValue",     "integer", "Integer default value"),
        RPC_PARAM_OPT("boolValue",    "boolean", "Boolean default value"),
        RPC_PARAM_OPT("stringValue",  "string",  "String default value"),
        RPC_PARAM_OPT("objectValue",  "path",    "Asset path of the object to bind (alias: objectPath). NOT assetPath, which names the MetaSound being edited."),
        RPC_PARAM_OPT("arrayValue",   "array",   "Values for an ARRAY-typed target, as a JSON array whose entries match the element type: numbers for Float:Array / Int32:Array, booleans for Bool:Array, strings for String:Array, asset paths for object arrays such as WaveAsset:Array. This is the ONLY param an array-typed target accepts, and a scalar-typed target refuses it. An empty array clears the value. Required to reach the Array.* node family (Array.Random Get, Array.Shuffle, Array.Get/Set/Concat), whose pins and the Weights pin of Array.Random Get are all array-typed."),
        RPC_PARAM_DEF("save",         "boolean", "Save after modification", "true")
    ))
{
#if MCP_HAS_METASOUND && MCP_HAS_METASOUND_FRONTEND
    FString AssetPath = NormalizeContentAssetPath(Ctx.GetString(TEXT("assetPath")));
    FString VariableName;
    if (!Ctx.RequireString(TEXT("variableName"), VariableName)) return true;

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

#if UE_VERSION_OLDER_THAN(5, 6, 0)
    // SetGraphVariableDefault is a UE 5.6 builder method with no 5.4/5.5 equivalent.
    Ctx.SendUnsupportedEngineVersion(TEXT("5.6"), TEXT("Setting MetaSound graph-variable defaults"));
    return true;
#else
    TScriptInterface<IMetaSoundDocumentInterface> ScriptInterface(MetaSound);
    PW_METASOUND_MAKE_BUILDER(Builder, ScriptInterface);

    // Resolve the variable first: its declared data type is what an objectValue is validated
    // against, and a missing variable is a better answer here than SetGraphVariableDefault's
    // bare false (which cannot distinguish "no such variable" from "literal rejected").
    const FMetasoundFrontendVariable* TargetVariable = Builder.FindGraphVariable(FName(*VariableName));
    if (!TargetVariable)
    {
        Ctx.SendError(TEXT("VARIABLE_NOT_FOUND"),
            FString::Printf(TEXT("MetaSound '%s' has no graph variable named '%s'. List variables with audio.authoring.describe_metasound (variables[])."),
                *AssetPath, *VariableName));
        PW_METASOUND_FINISH_BUILDING(Builder);
        return true;
    }
    const FString VariableTypeName = TargetVariable->TypeName.ToString();

    FMetasoundFrontendLiteral Literal;
    // A *set* verb with no value is a caller mistake, not a request to reset to the type
    // default, so the type-default path is disallowed here (unlike add_metasound_variable).
    const PinWright::MetaSound::EMetaSoundLiteralParamOutcome LiteralOutcome =
        PinWright::MetaSound::BuildMetaSoundLiteralFromParams(Ctx, VariableTypeName,
            /*bAllowTypeDefault*/ false, Literal);
    if (LiteralOutcome == PinWright::MetaSound::EMetaSoundLiteralParamOutcome::ErrorSent)
    {
        PW_METASOUND_FINISH_BUILDING(Builder);
        return true;
    }
    if (LiteralOutcome != PinWright::MetaSound::EMetaSoundLiteralParamOutcome::Ok)
    {
        PinWright::MetaSound::SendMetaSoundMissingValueError(Ctx,
            FString::Printf(TEXT("variable '%s'"), *VariableName), VariableTypeName);
        PW_METASOUND_FINISH_BUILDING(Builder);
        return true;
    }

    MetaSound->Modify();
    const bool bSetAccepted = Builder.SetGraphVariableDefault(FName(*VariableName), Literal);

    // Commit before reading the document or saving — see audio.authoring.metasound_gotchas.
    PW_METASOUND_FINISH_BUILDING(Builder);

    // Read the stored literal back off the DOCUMENT rather than through the builder that
    // wrote it, so the reported value cannot be an echo of the setter's argument (§4).
    FMetasoundFrontendLiteral StoredLiteral;
    bool bReadBack = false;
    if (const IMetaSoundDocumentInterface* DocInterface = Cast<IMetaSoundDocumentInterface>(MetaSound))
    {
        const FMetasoundFrontendDocument& Doc = PW_METASOUND_GET_CONST_DOCUMENT(DocInterface);
        const FMetasoundFrontendGraph& Graph = PinWright::MetaSound::GetGraphClassConstGraph(Doc.RootGraph);
        const FName VariableFName(*VariableName);
        for (const FMetasoundFrontendVariable& DocVariable : Graph.Variables)
        {
            if (DocVariable.Name == VariableFName)
            {
                StoredLiteral = DocVariable.Literal;
                bReadBack = true;
                break;
            }
        }
    }

    const bool bStoredMatches = bReadBack && StoredLiteral.IsEqual(Literal);

    TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject());
    Result->SetStringField(TEXT("variableName"), VariableName);
    Result->SetStringField(TEXT("variableType"), VariableTypeName);
    Result->SetBoolField(TEXT("readBack"), bReadBack);
    if (bReadBack)
    {
        TSharedPtr<FJsonObject> StoredJson = MakeShareable(new FJsonObject());
        PinWright::MetaSound::DescribeMetaSoundLiteral(StoredLiteral, StoredJson);
        Result->SetObjectField(TEXT("storedDefault"), StoredJson);
    }

    if (!bSetAccepted || !bStoredMatches)
    {
        Result->SetBoolField(TEXT("setAccepted"), bSetAccepted);
        Ctx.SendError(ErrorCodes::ERR_SET_DEFAULT_FAILED,
            FString::Printf(TEXT("Default for variable '%s' (type '%s') did not land: builder %s and the document read back %s."),
                *VariableName, *VariableTypeName,
                bSetAccepted ? TEXT("accepted the write") : TEXT("rejected the write"),
                bReadBack ? TEXT("a different literal") : TEXT("no variable at all")),
            Result);
        return true;
    }

    Result->SetStringField(TEXT("message"),
        FString::Printf(TEXT("Default for variable '%s' updated"), *VariableName));
    PinWright::MetaSound::SaveMetaSoundAndReport(Result, MetaSound, bSave);
    AddAssetVerification(Result, MetaSound);
    Ctx.SendSuccess(Result);
    return true;
#endif // UE_VERSION_OLDER_THAN(5, 6, 0)
#elif MCP_HAS_METASOUND
    FString VariableName = Ctx.GetString(TEXT("variableName"));

    TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject());
    Result->SetStringField(TEXT("message"),
        FString::Printf(TEXT("Variable default for '%s' noted"), *VariableName));
    Result->SetStringField(TEXT("note"),
        TEXT("MetaSound Frontend Builder not available — upgrade to UE 5.3+ for full support"));
    Ctx.SendSuccess(Result);
    return true;
#else
    Ctx.SendError(TEXT("METASOUND_NOT_AVAILABLE"), TEXT("MetaSound support not available"));
    return true;
#endif
}

// =========================================================================
// audio.authoring.validate_metasound
// =========================================================================
REGISTER_RPC_HANDLER("audio.authoring.validate_metasound", "audio.authoring",
    "Run builder-level validation on a MetaSound and return diagnostics",
    RPC_PARAMS(
        RPC_PARAM_REQ("assetPath", "path", "Asset path of the MetaSound")
    ))
{
#if MCP_HAS_METASOUND && MCP_HAS_METASOUND_FRONTEND
    FString AssetPath = NormalizeContentAssetPath(Ctx.GetString(TEXT("assetPath")));
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

    // FMetaSoundFrontendDocumentBuilder::IsValid() is a UE 5.6 addition. On 5.4/5.5
    // there is no builder-level structural check, so treat as valid here — the
    // in-editor compiler (noted below) remains the authoritative validity source.
#if UE_VERSION_OLDER_THAN(5, 6, 0)
    bool bValid = true;
#else
    bool bValid = Builder.IsValid();
#endif

    PW_METASOUND_FINISH_BUILDING(Builder);

    TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject());
    Result->SetBoolField(TEXT("valid"), bValid);
    // Builder-level validation does not surface per-diagnostic messages on this UE version.
    // In-editor compile remains authoritative for semantic errors (type mismatches,
    // missing interface vertices). IsValid catches structural invalidity.
    Result->SetArrayField(TEXT("diagnostics"), TArray<TSharedPtr<FJsonValue>>());
    Result->SetStringField(TEXT("note"),
        TEXT("Builder-level validation checks structural integrity. "
             "Semantic errors (type mismatches, missing interface vertices) "
             "surface only in the in-editor compiler."));
    Ctx.SendSuccess(Result);
    return true;
#elif MCP_HAS_METASOUND
    TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject());
    Result->SetBoolField(TEXT("valid"), true);
    Result->SetArrayField(TEXT("diagnostics"), TArray<TSharedPtr<FJsonValue>>());
    Result->SetStringField(TEXT("note"),
        TEXT("Builder-level validation API not exposed on this UE version; in-editor compile is authoritative"));
    Ctx.SendSuccess(Result);
    return true;
#else
    Ctx.SendError(TEXT("METASOUND_NOT_AVAILABLE"), TEXT("MetaSound support not available"));
    return true;
#endif
}

// =========================================================================
// audio.authoring.compile_metasound
// =========================================================================
REGISTER_RPC_HANDLER("audio.authoring.compile_metasound", "audio.authoring",
    "Force a MetaSound to rebuild its referenced asset classes and run validation",
    RPC_PARAMS(
        RPC_PARAM_REQ("assetPath", "path",    "Asset path of the MetaSound"),
        RPC_PARAM_DEF("save",      "boolean", "Save after rebuild", "false")
    ))
{
#if MCP_HAS_METASOUND && MCP_HAS_METASOUND_FRONTEND
    FString AssetPath = NormalizeContentAssetPath(Ctx.GetString(TEXT("assetPath")));
    bool bSave = Ctx.GetBool(TEXT("save"), false);

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

    // Rebuild referenced asset classes when the API is available (UE 5.3+, MetasoundAssetBase.h).
    // Resolve the FMetasoundAssetBase through the UObject registry rather than a typed
    // UMetaSoundSource pointer — both UMetaSoundSource and UMetaSoundPatch derive from
    // FMetasoundAssetBase, so this works for a Patch too. Updates the node class registry
    // so references to other MetaSounds resolve correctly.
    bool bRebuilt = false;
#if MCP_HAS_METASOUND_ASSET_BASE && MCP_HAS_METASOUND_UOBJECT_REGISTRY
    MetaSound->Modify();
    if (FMetasoundAssetBase* AssetBase =
            Metasound::IMetasoundUObjectRegistry::Get().GetObjectAsAssetBase(MetaSound))
    {
        AssetBase->RebuildReferencedAssetClasses();
        bRebuilt = true;
    }
#endif

    TScriptInterface<IMetaSoundDocumentInterface> ScriptInterface(MetaSound);
    PW_METASOUND_MAKE_BUILDER(Builder, ScriptInterface);

    // Builder::IsValid() is UE 5.6+; on 5.4/5.5 fall back to valid (in-editor compile authoritative).
#if UE_VERSION_OLDER_THAN(5, 6, 0)
    bool bValid = true;
#else
    bool bValid = Builder.IsValid();
#endif

    PW_METASOUND_FINISH_BUILDING(Builder);

    TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject());
    Result->SetBoolField(TEXT("compiled"), bRebuilt);
    // `valid` answers "does this graph compile", which is a property of the DOCUMENT and is
    // true whether or not the document is on disk. It is deliberately not conflated with
    // persistence: the save report below and AddAssetVerification's existsOnDisk/pendingSave
    // are what say where the graph lives (B-metasound-create-save-no-disk-write #4 read a
    // valid:true on a memory-only graph as a false success; the missing signal was the save
    // report, not a different meaning for `valid`).
    Result->SetBoolField(TEXT("valid"), bValid);
    Result->SetArrayField(TEXT("diagnostics"), TArray<TSharedPtr<FJsonValue>>());
    if (!bRebuilt)
    {
        Result->SetStringField(TEXT("note"),
            TEXT("RebuildReferencedAssetClasses not available on this UE version; "
                 "IsValid() result is from builder cache only."));
    }
    PinWright::MetaSound::SaveMetaSoundAndReport(Result, MetaSound, bSave && bValid);
    AddAssetVerification(Result, MetaSound);
    Ctx.SendSuccess(Result);
    return true;
#elif MCP_HAS_METASOUND
    TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject());
    Result->SetBoolField(TEXT("compiled"), false);
    Result->SetBoolField(TEXT("valid"), true);
    Result->SetArrayField(TEXT("diagnostics"), TArray<TSharedPtr<FJsonValue>>());
    Result->SetStringField(TEXT("note"),
        TEXT("MetaSound Frontend Builder not available — upgrade to UE 5.3+ for full support"));
    Ctx.SendSuccess(Result);
    return true;
#else
    Ctx.SendError(TEXT("METASOUND_NOT_AVAILABLE"), TEXT("MetaSound support not available"));
    return true;
#endif
}
