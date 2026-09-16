// Copyright (c) 2026 Alexander Penkin. MIT License.

// MetaSoundInterfaceHandler.cpp
// Implements:
//   audio.authoring.add_metasound_interface
//   audio.authoring.remove_metasound_interface
//   audio.authoring.list_metasound_interfaces

#include "Handlers/HandlerRegistration.h"
#include "Handlers/ParamSpec.h"
#include "Handlers/HandlerContext.h"
#include "PinWrightHelpers.h"
#include "Handlers/Audio/MetaSound/MetaSoundPathUtils.h"

// MetaSound document support (UE 5.0+)
#if __has_include("MetasoundDocumentInterface.h")
#include "MetasoundDocumentInterface.h"
#define MCP_HAS_METASOUND_DOCUMENT_INTERFACE 1
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

// MetaSound Frontend Search Engine (UE 5.3+)
#if __has_include("MetasoundFrontendSearchEngine.h")
#include "MetasoundFrontendSearchEngine.h"
#define MCP_HAS_METASOUND_SEARCH_ENGINE 1
#else
#define MCP_HAS_METASOUND_SEARCH_ENGINE 0
#endif

namespace
{
#if MCP_HAS_METASOUND && MCP_HAS_METASOUND_DOCUMENT_INTERFACE && MCP_HAS_METASOUND_FRONTEND && MCP_HAS_METASOUND_SEARCH_ENGINE
    bool ResolveMetaSoundInterface(const FString& InterfaceName, FMetasoundFrontendInterface& OutInterface)
    {
        return Metasound::Frontend::ISearchEngine::Get().FindInterfaceWithHighestVersion(
            FName(*InterfaceName), OutInterface);
    }

    TSharedPtr<FJsonObject> BuildInterfaceVertexJson(const FMetasoundFrontendClassVertex& Vertex)
    {
        TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
        Json->SetStringField(TEXT("name"), Vertex.Name.ToString());
        Json->SetStringField(TEXT("type"), Vertex.TypeName.ToString());
        return Json;
    }

    TSharedPtr<FJsonObject> BuildInterfaceJson(const FMetasoundFrontendInterface& Interface)
    {
        // 5.6 moved Version/UClassOptions into Interface.Metadata; accessor picks the right field.
        const FMetasoundFrontendVersion& Version = PinWright::MetaSound::GetInterfaceVersion(Interface);

        TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
        Json->SetStringField(TEXT("name"), Version.Name.ToString());
        Json->SetNumberField(TEXT("versionMajor"), Version.Number.Major);
        Json->SetNumberField(TEXT("versionMinor"), Version.Number.Minor);

        TSharedPtr<FJsonObject> VersionJson = MakeShared<FJsonObject>();
        VersionJson->SetStringField(TEXT("name"), Version.Name.ToString());
        VersionJson->SetNumberField(TEXT("major"), Version.Number.Major);
        VersionJson->SetNumberField(TEXT("minor"), Version.Number.Minor);
        Json->SetObjectField(TEXT("version"), VersionJson);

        TArray<TSharedPtr<FJsonValue>> Inputs;
        for (const FMetasoundFrontendClassInput& Input : Interface.Inputs)
        {
            Inputs.Add(MakeShared<FJsonValueObject>(BuildInterfaceVertexJson(Input)));
        }
        Json->SetArrayField(TEXT("inputs"), Inputs);

        TArray<TSharedPtr<FJsonValue>> Outputs;
        for (const FMetasoundFrontendClassOutput& Output : Interface.Outputs)
        {
            Outputs.Add(MakeShared<FJsonValueObject>(BuildInterfaceVertexJson(Output)));
        }
        Json->SetArrayField(TEXT("outputs"), Outputs);

        TArray<TSharedPtr<FJsonValue>> ClassOptions;
        for (const FMetasoundFrontendInterfaceUClassOptions& Option : PinWright::MetaSound::GetInterfaceUClassOptions(Interface))
        {
            TSharedPtr<FJsonObject> OptionJson = MakeShared<FJsonObject>();
            OptionJson->SetStringField(TEXT("classPath"), Option.ClassPath.ToString());
            OptionJson->SetBoolField(TEXT("modifiable"), Option.bIsModifiable);
            OptionJson->SetBoolField(TEXT("default"), Option.bIsDefault);
            ClassOptions.Add(MakeShared<FJsonValueObject>(OptionJson));
        }
        Json->SetArrayField(TEXT("classOptions"), ClassOptions);

        return Json;
    }

    // Whether a registered interface is non-modifiable (mandatory / non-detachable)
    // for the given asset's UClass. Delegates to the shared
    // PinWright::MetaSound::IsInterfaceModifiableForClass rule (the
    // single definition of per-UClass modifiability, incl. the default-when-no-match
    // policy) so this handler and the tests cannot drift. A null MetaSound is treated
    // as modifiable (nothing to refuse).
    bool IsInterfaceNonModifiableForAsset(const FMetasoundFrontendInterface& Interface, const UObject* MetaSound)
    {
        if (!MetaSound)
        {
            return false;
        }

        return !PinWright::MetaSound::IsInterfaceModifiableForClass(
            Interface, MetaSound->GetClass()->GetClassPathName());
    }
#endif
}


// =========================================================================
// audio.authoring.add_metasound_interface
// =========================================================================

REGISTER_RPC_HANDLER("audio.authoring.add_metasound_interface", "audio.authoring",
    "Attach a named interface to a MetaSound graph (add/remove required I/O vertices)",
    RPC_PARAMS(
        RPC_PARAM_REQ("assetPath", "path", "Asset path of the MetaSound"),
        RPC_PARAM_REQ("interfaceName", "string", "Registered MetaSound frontend interface name (e.g. UE.OutputFormat.Mono)"),
        RPC_PARAM_DEF("save", "boolean", "Save after modification", "true")
    ))
{
#if MCP_HAS_METASOUND && MCP_HAS_METASOUND_DOCUMENT_INTERFACE && MCP_HAS_METASOUND_FRONTEND
    FString AssetPath = NormalizeContentAssetPath(Ctx.GetString(TEXT("assetPath")));
    FString InterfaceName;
    if (!Ctx.RequireString(TEXT("interfaceName"), InterfaceName)) return true;
    bool bSave = Ctx.GetBool(TEXT("save"), true);

    if (AssetPath.IsEmpty())
    {
        Ctx.SendError(TEXT("MISSING_PATH"), TEXT("Asset path is required"));
        return true;
    }

    FName ResolvedInterfaceName(*InterfaceName);
#if MCP_HAS_METASOUND_SEARCH_ENGINE
    FMetasoundFrontendInterface ResolvedInterface;
    if (!ResolveMetaSoundInterface(InterfaceName, ResolvedInterface))
    {
        Ctx.SendError(TEXT("INTERFACE_NOT_FOUND"),
            FString::Printf(TEXT("Interface '%s' is not registered"), *InterfaceName));
        return true;
    }
    ResolvedInterfaceName = PinWright::MetaSound::GetInterfaceVersion(ResolvedInterface).Name;
#endif

    UObject* MetaSound = PinWright::MetaSound::LoadMetaSoundDocumentAsset(AssetPath);
    if (!MetaSound)
    {
        Ctx.SendError(TEXT("ASSET_NOT_FOUND"),
            FString::Printf(TEXT("Could not load MetaSound: %s"), *AssetPath));
        return true;
    }

    TScriptInterface<IMetaSoundDocumentInterface> ScriptInterface;
    if (!PinWright::MetaSound::TryMakeMetaSoundDocumentInterface(MetaSound, ScriptInterface))
    {
        Ctx.SendError(TEXT("INTERFACE_ERROR"), TEXT("MetaSound does not implement document interface"));
        return true;
    }

    PW_METASOUND_MAKE_BUILDER(Builder, ScriptInterface);

    MetaSound->Modify();
    bool bAdded = Builder.AddInterface(ResolvedInterfaceName);

    PW_METASOUND_FINISH_BUILDING(Builder);

    if (!bAdded)
    {
        Ctx.SendError(TEXT("INTERFACE_NOT_FOUND"),
            FString::Printf(TEXT("Interface '%s' could not be attached to this MetaSound asset"), *InterfaceName));
        return true;
    }

    TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject());
    Result->SetStringField(TEXT("message"),
        FString::Printf(TEXT("Interface '%s' attached to MetaSound"), *ResolvedInterfaceName.ToString()));
    Result->SetStringField(TEXT("interfaceName"), ResolvedInterfaceName.ToString());
    PinWright::MetaSound::SaveMetaSoundAndReport(Result, MetaSound, bSave);
    AddAssetVerification(Result, MetaSound);
    Ctx.SendSuccess(Result);
    return true;

#elif MCP_HAS_METASOUND && MCP_HAS_METASOUND_FRONTEND
    Ctx.SendError(TEXT("METASOUND_DOCUMENT_INTERFACE_NOT_SUPPORTED"),
        TEXT("add_metasound_interface requires MetaSound document interface support"));
    return true;
#elif MCP_HAS_METASOUND
    Ctx.SendError(TEXT("METASOUND_FRONTEND_NOT_SUPPORTED"),
        TEXT("add_metasound_interface requires MetaSound Frontend Builder (UE 5.3+)"));
    return true;
#else
    Ctx.SendError(TEXT("METASOUND_NOT_AVAILABLE"), TEXT("MetaSound support not available"));
    return true;
#endif
}

// =========================================================================
// audio.authoring.remove_metasound_interface
// =========================================================================

REGISTER_RPC_HANDLER("audio.authoring.remove_metasound_interface", "audio.authoring",
    "Detach a named interface from a MetaSound graph",
    RPC_PARAMS(
        RPC_PARAM_REQ("assetPath", "path", "Asset path of the MetaSound"),
        RPC_PARAM_REQ("interfaceName", "string", "Registered interface name to remove"),
        RPC_PARAM_DEF("save", "boolean", "Save after modification", "true")
    ))
{
#if MCP_HAS_METASOUND && MCP_HAS_METASOUND_DOCUMENT_INTERFACE && MCP_HAS_METASOUND_FRONTEND
    FString AssetPath = NormalizeContentAssetPath(Ctx.GetString(TEXT("assetPath")));
    FString InterfaceName;
    if (!Ctx.RequireString(TEXT("interfaceName"), InterfaceName)) return true;
    bool bSave = Ctx.GetBool(TEXT("save"), true);

    if (AssetPath.IsEmpty())
    {
        Ctx.SendError(TEXT("MISSING_PATH"), TEXT("Asset path is required"));
        return true;
    }

    FName ResolvedInterfaceName(*InterfaceName);
#if MCP_HAS_METASOUND_SEARCH_ENGINE
    FMetasoundFrontendInterface ResolvedInterface;
    if (!ResolveMetaSoundInterface(InterfaceName, ResolvedInterface))
    {
        Ctx.SendError(TEXT("INTERFACE_NOT_FOUND"),
            FString::Printf(TEXT("Interface '%s' is not registered"), *InterfaceName));
        return true;
    }
    ResolvedInterfaceName = PinWright::MetaSound::GetInterfaceVersion(ResolvedInterface).Name;
#endif

    UObject* MetaSound = PinWright::MetaSound::LoadMetaSoundDocumentAsset(AssetPath);
    if (!MetaSound)
    {
        Ctx.SendError(TEXT("ASSET_NOT_FOUND"),
            FString::Printf(TEXT("Could not load MetaSound: %s"), *AssetPath));
        return true;
    }

    TScriptInterface<IMetaSoundDocumentInterface> ScriptInterface;
    if (!PinWright::MetaSound::TryMakeMetaSoundDocumentInterface(MetaSound, ScriptInterface))
    {
        Ctx.SendError(TEXT("INTERFACE_ERROR"), TEXT("MetaSound does not implement document interface"));
        return true;
    }

    PW_METASOUND_MAKE_BUILDER(Builder, ScriptInterface);

#if MCP_HAS_METASOUND_SEARCH_ENGINE
    // The interface is genuinely absent only if the document does not declare it.
    // (This version-keyed check and RemoveInterface's own internal gate both resolve
    //  the highest version and test the same Document.Interfaces membership set, so
    //  this pre-check is the authoritative "is it attached?" answer — it must stay:
    //  RemoveInterface(FName) returns TRUE for an absent-but-modifiable interface,
    //  so relying on its return value alone would report fake success for genuine
    //  absence.)
    if (!Builder.IsInterfaceDeclared(PinWright::MetaSound::GetInterfaceVersion(ResolvedInterface)))
    {
        PW_METASOUND_FINISH_BUILDING(Builder);
        Ctx.SendError(TEXT("INTERFACE_NOT_FOUND"),
            FString::Printf(TEXT("Interface '%s' is not attached to this MetaSound"), *InterfaceName));
        return true;
    }

    // The interface IS attached. If it is a non-modifiable (mandatory/default)
    // interface for this asset's UClass — e.g. UE.OutputFormat.Mono on a
    // UMetaSoundSource — RemoveInterface will refuse it. Report that accurately up
    // front rather than letting the failure surface as the misleading
    // "is not attached" text.
    if (IsInterfaceNonModifiableForAsset(ResolvedInterface, MetaSound))
    {
        PW_METASOUND_FINISH_BUILDING(Builder);
        Ctx.SendError(TEXT("INTERFACE_NOT_REMOVABLE"),
            FString::Printf(
                TEXT("Interface '%s' is attached but is a mandatory/default interface for asset class '%s' and cannot be detached"),
                *InterfaceName, *MetaSound->GetClass()->GetName()));
        return true;
    }
#endif

    MetaSound->Modify();
    bool bRemoved = Builder.RemoveInterface(ResolvedInterfaceName);

    PW_METASOUND_FINISH_BUILDING(Builder);

    if (!bRemoved)
    {
#if MCP_HAS_METASOUND_SEARCH_ENGINE
        // The pre-check above proved the interface IS attached and is not flagged
        // non-modifiable for this asset class, yet RemoveInterface still refused it.
        // This is a removal failure, not absence — do not claim "is not attached".
        Ctx.SendError(TEXT("INTERFACE_NOT_REMOVABLE"),
            FString::Printf(TEXT("Interface '%s' is attached but could not be detached from this MetaSound"), *InterfaceName));
#else
        // Without the search engine we have no pre-check; RemoveInterface(FName)
        // returning false here means the name was unregistered or absent.
        Ctx.SendError(TEXT("INTERFACE_NOT_FOUND"),
            FString::Printf(TEXT("Interface '%s' is not attached to this MetaSound"), *InterfaceName));
#endif
        return true;
    }

    TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject());
    Result->SetStringField(TEXT("message"),
        FString::Printf(TEXT("Interface '%s' detached from MetaSound"), *ResolvedInterfaceName.ToString()));
    Result->SetStringField(TEXT("interfaceName"), ResolvedInterfaceName.ToString());
    PinWright::MetaSound::SaveMetaSoundAndReport(Result, MetaSound, bSave);
    AddAssetVerification(Result, MetaSound);
    Ctx.SendSuccess(Result);
    return true;

#elif MCP_HAS_METASOUND && MCP_HAS_METASOUND_FRONTEND
    Ctx.SendError(TEXT("METASOUND_DOCUMENT_INTERFACE_NOT_SUPPORTED"),
        TEXT("remove_metasound_interface requires MetaSound document interface support"));
    return true;
#elif MCP_HAS_METASOUND
    Ctx.SendError(TEXT("METASOUND_FRONTEND_NOT_SUPPORTED"),
        TEXT("remove_metasound_interface requires MetaSound Frontend Builder (UE 5.3+)"));
    return true;
#else
    Ctx.SendError(TEXT("METASOUND_NOT_AVAILABLE"), TEXT("MetaSound support not available"));
    return true;
#endif
}

// =========================================================================
// audio.authoring.list_metasound_interfaces
// =========================================================================

REGISTER_RPC_HANDLER("audio.authoring.list_metasound_interfaces", "audio.authoring",
    "List all registered MetaSound interfaces with their input/output vertex shapes",
    RPC_NO_PARAMS)
{
#if MCP_HAS_METASOUND && MCP_HAS_METASOUND_DOCUMENT_INTERFACE && MCP_HAS_METASOUND_FRONTEND && MCP_HAS_METASOUND_SEARCH_ENGINE
    {
        TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject());
        TArray<TSharedPtr<FJsonValue>> InterfacesArray;
        TArray<FMetasoundFrontendInterface> Interfaces =
            PinWright::MetaSound::FindAllFrontendInterfaces();

        Interfaces.Sort([](const FMetasoundFrontendInterface& A, const FMetasoundFrontendInterface& B)
        {
            // 5.6 relocated Version into Interface.Metadata; accessor reads the right field per version.
            const FMetasoundFrontendVersion& AVer = PinWright::MetaSound::GetInterfaceVersion(A);
            const FMetasoundFrontendVersion& BVer = PinWright::MetaSound::GetInterfaceVersion(B);
            const FString AName = AVer.Name.ToString();
            const FString BName = BVer.Name.ToString();
            if (AName != BName)
            {
                return AName < BName;
            }
            if (AVer.Number.Major != BVer.Number.Major)
            {
                return AVer.Number.Major < BVer.Number.Major;
            }
            return AVer.Number.Minor < BVer.Number.Minor;
        });

        for (const FMetasoundFrontendInterface& Interface : Interfaces)
        {
            InterfacesArray.Add(MakeShared<FJsonValueObject>(BuildInterfaceJson(Interface)));
        }

        Result->SetArrayField(TEXT("interfaces"), InterfacesArray);
        Result->SetNumberField(TEXT("totalCount"), InterfacesArray.Num());
        Result->SetStringField(TEXT("note"),
            TEXT("Use the returned MetaSound frontend interface names with add_metasound_interface and remove_metasound_interface."));
        Ctx.SendSuccess(Result);
        return true;
    }
#else
    TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject());
    TArray<TSharedPtr<FJsonValue>> InterfacesArray;
    Result->SetArrayField(TEXT("interfaces"), InterfacesArray);
    Result->SetNumberField(TEXT("totalCount"), 0);
    Result->SetStringField(TEXT("note"),
        TEXT("MetaSound frontend search interface enumeration is not available on this UE version. "
             "Use add_metasound_interface and remove_metasound_interface with known interface names."));
    Ctx.SendSuccess(Result);
    return true;
#endif
}
