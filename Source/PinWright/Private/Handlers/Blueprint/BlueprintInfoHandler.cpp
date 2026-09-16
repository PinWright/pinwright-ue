// Copyright (c) 2026 Alexander Penkin. MIT License.

// BlueprintInfoHandler.cpp - Migrated from PinWright_BlueprintHandlers.cpp
// Blueprint info queries: get, exists, probe_handle, set_metadata, ensure_exists

#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/Blueprint/BlueprintHandlerUtils.h"
#include "State/PluginState.h"
#include "State/BlueprintTracker.h"
#include "Utils/AssetUtils.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Misc/PackageName.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"

using namespace BlueprintHandlerUtils;

// ---- blueprint.exists ----
REGISTER_RPC_HANDLER("blueprint.exists", "blueprint", "Check whether a blueprint asset exists",
    RPC_PARAMS(
        BlueprintPathParamReq(TEXT("path"), TEXT("path"), TEXT("Blueprint asset path to check"))
    ))
{
    FString Path = ResolveBlueprintPath(Ctx);
    if (Path.IsEmpty())
    {
        Ctx.SendError(TEXT("INVALID_BLUEPRINT_PATH"), TEXT("blueprint.exists requires a blueprint path."));
        return true;
    }

    FString CheckPath = Path;
    if (!IsValidMountPoint(CheckPath))
    {
        if (CheckPath.StartsWith(TEXT("/")))
            CheckPath = TEXT("/Game") + CheckPath;
        else
            CheckPath = TEXT("/Game/") + CheckPath;
    }
    if (CheckPath.EndsWith(TEXT(".uasset")))
        CheckPath = CheckPath.LeftChop(7);

    bool bFound = ResolveAsset(CheckPath).bExists;
    FString Normalized = bFound ? CheckPath : Path;

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetBoolField(TEXT("exists"), bFound);
    Resp->SetStringField(TEXT("blueprintPath"), Normalized);
    Ctx.SendSuccess(Resp);
    return true;
}

// ---- blueprint.get ----
REGISTER_RPC_HANDLER("blueprint.get", "blueprint", "Return summary metadata for a Blueprint: parent class, variables, functions, events, and a defaults map (each member variable's CDO default value, same shape as property.get includeDefault). events[] is a live enumeration of the graph nodes, including legacy and Enhanced Input entry nodes; input entries identify their key/action in name and expose connected edges in execOutputs[]. An event that was authored and later removed is absent from it. Does NOT include components — for class-level component templates use blueprint.scs.get, or blueprint.inspect for the full structural dump (graphs/references/components).",
    RPC_PARAMS(
        BlueprintPathParamReq(TEXT("path"), TEXT("path"), TEXT("Blueprint asset path to inspect."))
    ))
{
    FString Path = ResolveBlueprintPath(Ctx);
    if (Path.IsEmpty())
    {
        Ctx.SendError(TEXT("INVALID_BLUEPRINT_PATH"), TEXT("blueprint.get requires a blueprint path."));
        return true;
    }

    auto* Subsystem = Ctx.GetSubsystem();

    FString Normalized, Err;
    UBlueprint* BP = LoadBlueprintAsset(Path, Normalized, Err);
    if (!BP)
    {
        Ctx.SendError(TEXT("NOT_FOUND"), TEXT("Blueprint not found"));
        return true;
    }

    const FString Key = !Normalized.TrimStartAndEnd().IsEmpty() ? Normalized : Path;
    TSharedPtr<FJsonObject> Entry;

    Entry = BuildBlueprintSnapshot(BP, Key);
    if (!Entry.IsValid())
    {
        Entry = MakeShared<FJsonObject>();
        Entry->SetStringField(TEXT("blueprintPath"), Key);
        Entry->SetStringField(TEXT("resolvedPath"), Key);
        Entry->SetStringField(TEXT("assetPath"), BP->GetPathName());
    }

    // Merge registry-backed fields for compatibility
    TSharedPtr<FJsonObject> RegistryEntry = EnsureBlueprintEntry(Key);
    if (RegistryEntry.IsValid())
    {
        if (RegistryEntry->HasField(TEXT("defaults")) && !Entry->HasField(TEXT("defaults")))
            Entry->SetObjectField(TEXT("defaults"), RegistryEntry->GetObjectField(TEXT("defaults")));
        if (RegistryEntry->HasField(TEXT("metadata")) && !Entry->HasField(TEXT("metadata")))
            Entry->SetObjectField(TEXT("metadata"), RegistryEntry->GetObjectField(TEXT("metadata")));

        // Merge functions
        if (RegistryEntry->HasField(TEXT("functions")))
        {
            TArray<TSharedPtr<FJsonValue>> RegFuncs = RegistryEntry->GetArrayField(TEXT("functions"));
            if (!Entry->HasField(TEXT("functions")))
            {
                Entry->SetArrayField(TEXT("functions"), RegFuncs);
            }
            else
            {
                TArray<TSharedPtr<FJsonValue>> ExistingFuncs = Entry->GetArrayField(TEXT("functions"));
                TSet<FString> KnownNames;
                for (const auto& Val : ExistingFuncs)
                {
                    const TSharedPtr<FJsonObject> Obj = Val->AsObject();
                    FString N;
                    if (Obj.IsValid() && Obj->TryGetStringField(TEXT("name"), N))
                        KnownNames.Add(N);
                }
                for (const auto& Val : RegFuncs)
                {
                    const TSharedPtr<FJsonObject> Obj = Val->AsObject();
                    FString N;
                    if (Obj.IsValid() && Obj->TryGetStringField(TEXT("name"), N) && !KnownNames.Contains(N))
                        ExistingFuncs.Add(Val);
                }
                Entry->SetArrayField(TEXT("functions"), ExistingFuncs);
            }
        }

        // Events are deliberately NOT unioned with the registry the way functions are above.
        // BuildBlueprintSnapshot always sets events[] from CollectBlueprintEvents, which walks
        // every node of every UbergraphPage, so the snapshot is a COMPLETE enumeration of the
        // graph: a registry event the snapshot does not list is an event the graph does not
        // have. Appending it made blueprint.get assert that a node exists when it does not —
        // e.g. blueprint.add_event followed by a default-mode blueprint.compile_bpir, whose
        // Phase 0 sweep deletes the add_event-created entry (docs/wiki-src/blueprint.bpir-
        // gotchas.md) while the registry record survives. That turned the readback into a
        // corroborating witness for the write path instead of an independent one.
        //
        // The one surviving registry path is the degenerate case where no snapshot could be
        // built at all, in which case a stale list beats no list.
        if (RegistryEntry->HasField(TEXT("events")) && !Entry->HasField(TEXT("events")))
        {
            Entry->SetArrayField(TEXT("events"), RegistryEntry->GetArrayField(TEXT("events")));
        }
    }

    Ctx.SendSuccess(Entry);
    return true;
}

// ---- blueprint.probe_handle ----
REGISTER_RPC_HANDLER("blueprint.probe_handle", "blueprint", "Lightweight check for blueprint existence without loading",
    RPC_PARAMS(
        BlueprintPathParamReq(TEXT("path"), TEXT("path"), TEXT("Blueprint asset path to probe"))
    ))
{
    FString Path = ResolveBlueprintPath(Ctx);
    if (Path.IsEmpty())
    {
        Ctx.SendError(TEXT("INVALID_BLUEPRINT_PATH"), TEXT("blueprint.probe_handle requires a blueprint path."));
        return true;
    }

    FString CheckPath = Path;
    if (!IsValidMountPoint(CheckPath))
    {
        if (CheckPath.StartsWith(TEXT("/")))
            CheckPath = TEXT("/Game") + CheckPath;
        else
            CheckPath = TEXT("/Game/") + CheckPath;
    }
    if (CheckPath.EndsWith(TEXT(".uasset")))
        CheckPath = CheckPath.LeftChop(7);

    bool bExists = ResolveAsset(CheckPath).bExists;
    FString AssetClass;

    if (bExists)
    {
        IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();
        FAssetData AssetData = AssetRegistry.GetAssetByObjectPath(FSoftObjectPath(CheckPath));
        if (AssetData.IsValid())
        {
            AssetClass = AssetData.AssetClassPath.GetAssetName().ToString();
        }
    }

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetBoolField(TEXT("exists"), bExists);
    Resp->SetStringField(TEXT("path"), bExists ? CheckPath : Path);
    if (!AssetClass.IsEmpty())
        Resp->SetStringField(TEXT("assetClass"), AssetClass);
    Ctx.SendSuccess(Resp);
    return true;
}

// ---- blueprint.set_metadata ----
REGISTER_RPC_HANDLER("blueprint.set_metadata", "blueprint", "Set metadata on a blueprint asset",
    RPC_PARAMS(
        BlueprintPathParamReq(TEXT("path"), TEXT("path"), TEXT("Blueprint asset path")),
        RPC_PARAM_REQ("metadata", "object", "Key-value metadata to set")
    ))
{
    FString Path = ResolveBlueprintPath(Ctx);
    if (Path.IsEmpty())
    {
        Ctx.SendError(TEXT("INVALID_BLUEPRINT_PATH"), TEXT("blueprint.set_metadata requires a blueprint path."));
        return true;
    }

    const TSharedPtr<FJsonObject>& Payload = Ctx.GetRawPayload();
    const TSharedPtr<FJsonObject>* MetadataObj = nullptr;
    if (!Payload->TryGetObjectField(TEXT("metadata"), MetadataObj) || !MetadataObj || !(*MetadataObj).IsValid())
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("metadata object required"));
        return true;
    }

    auto* Subsystem = Ctx.GetSubsystem();
    FString Normalized, LoadErr;
    UBlueprint* BP = LoadBlueprintAsset(Path, Normalized, LoadErr);
    if (!BP)
    {
        Ctx.SendError(TEXT("BLUEPRINT_NOT_FOUND"), LoadErr.IsEmpty() ? TEXT("Failed to load blueprint") : *LoadErr);
        return true;
    }

    const FString RegistryKey = Normalized.IsEmpty() ? Path : Normalized;

    TArray<FString> MetadataSet;
    for (const TPair<FString, TSharedPtr<FJsonValue>> Pair : (*MetadataObj)->Values)
    {
        if (!Pair.Value.IsValid()) continue;
        const FName MetaKey = ResolveMetadataKey(Pair.Key);
        FString MetaValue;
        if (Pair.Value->Type == EJson::String) MetaValue = Pair.Value->AsString();
        else if (Pair.Value->Type == EJson::Boolean) MetaValue = Pair.Value->AsBool() ? TEXT("true") : TEXT("false");
        else if (Pair.Value->Type == EJson::Number) MetaValue = FString::Printf(TEXT("%g"), Pair.Value->AsNumber());
        else continue;

        if (BP->GeneratedClass)
            BP->GeneratedClass->SetMetaData(MetaKey, *MetaValue);
        MetadataSet.Add(Pair.Key);
    }

    FBlueprintEditorUtils::MarkBlueprintAsModified(BP);
    // WasSavePersisted, not the raw call: the throttle skip and the transient-package
    // early-out used to return true, so a second edit inside the 0.5s window reported
    // saved:true with the change still only in memory.
    const bool bSaved = WasSavePersisted(SaveLoadedAssetThrottled(BP));

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetBoolField(TEXT("success"), true);
    Resp->SetStringField(TEXT("blueprintPath"), RegistryKey);
    TArray<TSharedPtr<FJsonValue>> MetaArray;
    for (const FString& Key : MetadataSet)
        MetaArray.Add(MakeShared<FJsonValueString>(Key));
    Resp->SetArrayField(TEXT("metadataSet"), MetaArray);
    Resp->SetBoolField(TEXT("saved"), bSaved);
    Ctx.SendSuccess(Resp);
    return true;
}

// ---- blueprint.ensure_exists ----
REGISTER_RPC_HANDLER("blueprint.ensure_exists", "blueprint", "Idempotent BP existence guard: probes the asset and creates it (via blueprint.create) when missing. Useful at the top of provisioning scripts so subsequent operations always have a target asset.",
    RPC_PARAMS(
        BlueprintPathParamReq(TEXT("path"), TEXT("path"), TEXT("Blueprint asset path to probe / create.")),
        RPC_PARAM_OPT("parentClass", "classref", "Parent UClass to use if creation is needed; defaults to AActor when omitted."),
        RPC_PARAM_DEF("createIfMissing", "boolean", "When true (default) a missing asset is auto-created; false makes the call probe-only.", "true")
    ))
{
    FString Path = ResolveBlueprintPath(Ctx);
    if (Path.IsEmpty())
    {
        Ctx.SendError(TEXT("INVALID_BLUEPRINT_PATH"), TEXT("blueprint.ensure_exists requires a blueprint path."));
        return true;
    }

    const TSharedPtr<FJsonObject>& Payload = Ctx.GetRawPayload();
    FString ParentClass;
    Payload->TryGetStringField(TEXT("parentClass"), ParentClass);
    bool bCreateIfMissing = true;
    if (Payload->HasField(TEXT("createIfMissing")))
        Payload->TryGetBoolField(TEXT("createIfMissing"), bCreateIfMissing);

    FString CheckPath = Path;
    if (!IsValidMountPoint(CheckPath))
    {
        if (CheckPath.StartsWith(TEXT("/")))
            CheckPath = TEXT("/Game") + CheckPath;
        else
            CheckPath = TEXT("/Game/") + CheckPath;
    }
    if (CheckPath.EndsWith(TEXT(".uasset")))
        CheckPath = CheckPath.LeftChop(7);

    bool bExists = ResolveAsset(CheckPath).bExists;
    bool bCreated = false;

    if (!bExists && bCreateIfMissing)
    {
        auto* Subsystem = Ctx.GetSubsystem();
        if (!Subsystem)
        {
            Ctx.SendError(TEXT("SUBSYSTEM_NOT_FOUND"),
                TEXT("blueprint.ensure_exists cannot auto-create without a live subsystem."));
            return true;
        }

        // blueprint.create requires a bare asset `name` plus a separate `savePath`
        // folder — it never reads `blueprintPath`, so passing a combined path here
        // made the create dispatch fail its required-`name` validation and the
        // auto-create branch was dead. Split the normalized CheckPath into folder +
        // name and dispatch the params blueprint.create actually consumes.
        // Use the engine's package/object helpers (the idiom the sibling blueprint
        // handlers use) instead of hand-rolled slash/dot surgery: ObjectPathToPackageName
        // drops any Package.Object suffix, then GetLongPackagePath / GetLongPackageAssetName
        // split the package into folder + bare asset name.
        const FString CreatePackage = FPackageName::ObjectPathToPackageName(CheckPath);
        FString CreateFolder = FPackageName::GetLongPackagePath(CreatePackage);
        const FString CreateName = FPackageName::GetLongPackageAssetName(CreatePackage);
        if (CreateFolder.IsEmpty())
            CreateFolder = TEXT("/Game");

        TSharedPtr<FJsonObject> CreatePayload = MakeShared<FJsonObject>();
        CreatePayload->SetStringField(TEXT("name"), CreateName);
        CreatePayload->SetStringField(TEXT("savePath"), CreateFolder);
        if (!ParentClass.IsEmpty())
            CreatePayload->SetStringField(TEXT("parentClass"), ParentClass);

        bool bCreateResult = Subsystem->DispatchMethod(TEXT("blueprint.create"), Ctx.GetRequestId(), CreatePayload);
        if (bCreateResult) return true;

        bExists = ResolveAsset(CheckPath).bExists;
        bCreated = bExists;
    }

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetBoolField(TEXT("exists"), bExists);
    Resp->SetBoolField(TEXT("created"), bCreated);
    Resp->SetStringField(TEXT("blueprintPath"), bExists ? CheckPath : Path);
    Ctx.SendSuccess(Resp);
    return true;
}
