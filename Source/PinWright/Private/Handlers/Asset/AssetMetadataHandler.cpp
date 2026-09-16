// Copyright (c) 2026 Alexander Penkin. MIT License.

// Asset metadata handlers: set/get metadata, set tags, get dependencies (classified),
// get asset graph.
// Migrated from PinWright_AssetWorkflowHandlers.cpp

#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/ParamAliasUtils.h"
#include "PinWrightHelpers.h"
#include "PinWrightSubsystem.h"

#include "Dom/JsonObject.h"
#include "Compat/EngineVersionCompat.h"
#include "Compat/JsonKeyCompat.h"
#include "UObject/MetaData.h"
#include "Utils/AssetUtils.h"
#include "Utils/SortedJsonWriter.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "UObject/Package.h"

// ============================================================================
// asset.set_metadata
// ============================================================================
REGISTER_RPC_HANDLER("asset.set_metadata", "asset", "Write key/value metadata onto an asset's UMetaData store. Persisted in the asset package; queryable via asset.get_metadata and asset.find_by_tag. Use asset.set_tags for boolean tags only.",
    RPC_PARAMS(
        RPC_PARAM_REQ("assetPath", "path", "Asset path whose metadata is being written."),
        RPC_PARAM_OPT("metadata", "object", "Object whose keys are metadata names (FName) and values are stringifiable JSON values."),
        RPC_PARAM_DEF("save", "boolean", "Write the updated metadata to disk; false leaves the package dirty and reports saveRequested:false (default true).", "true")
    ))
{
    FString AssetPath = Ctx.GetString(TEXT("assetPath"));
    if (AssetPath.IsEmpty())
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("assetPath required"));
        return true;
    }

    if (!ResolveAsset(AssetPath).bExists)
    {
        Ctx.SendError(TEXT("ASSET_NOT_FOUND"), TEXT("Asset not found"));
        return true;
    }

    TSharedPtr<FJsonObject> Payload = Ctx.GetRawPayload();
    const bool bSave = Ctx.GetBool(TEXT("save"), true);
    const TSharedPtr<FJsonObject>* MetadataObjPtr = nullptr;
    if (!Payload->TryGetObjectField(TEXT("metadata"), MetadataObjPtr) || !MetadataObjPtr)
    {
        // No metadata provided = no-op success
        TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
        Resp->SetBoolField(TEXT("success"), true);
        Resp->SetStringField(TEXT("assetPath"), AssetPath);
        Resp->SetNumberField(TEXT("updatedKeys"), 0);
        AddAssetSaveReport(Resp, /*bSaveRequested=*/false,
            /*bSavedToDisk=*/false, EAssetSaveState::NotRequested);
        Ctx.SendSuccess(Resp);
        return true;
    }

    UObject* Asset = ResolveAsset(AssetPath, /*bLoadObject=*/true).Object;
    if (!Asset)
    {
        Ctx.SendError(TEXT("LOAD_FAILED"), TEXT("Failed to load asset"));
        return true;
    }

    UPackage* Package = Asset->GetOutermost();
    if (!Package)
    {
        Ctx.SendError(TEXT("PACKAGE_NOT_FOUND"), TEXT("Failed to resolve package for asset"));
        return true;
    }

#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 6, 0)
    FMetaData& Meta = Package->GetMetaData();
#else
    UMetaData* Meta = Package->GetMetaData();
#endif

    const TSharedPtr<FJsonObject>& MetadataObj = *MetadataObjPtr;
    TMap<FString, FString> WrittenValues;
    int32 UpdatedCount = 0;

    for (const auto& Kvp : MetadataObj->Values)
    {
        const FString Key = EARGCompat::JsonKeyToString(Kvp.Key);
        const TSharedPtr<FJsonValue>& Val = Kvp.Value;

        FString ValueString;
        if (!Val.IsValid() || Val->IsNull())
        {
            continue;
        }
        switch (Val->Type)
        {
        case EJson::String:
            ValueString = Val->AsString();
            break;
        case EJson::Number:
            ValueString = LexToString(Val->AsNumber());
            break;
        case EJson::Boolean:
            ValueString = Val->AsBool() ? TEXT("true") : TEXT("false");
            break;
        default:
            ValueString = SortedJsonWriter::SerializeSortedJsonValue(Val);
            break;
        }

        if (!ValueString.IsEmpty())
        {
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 6, 0)
            Meta.SetValue(Asset, *Key, *ValueString);
#else
            Meta->SetValue(Asset, *Key, *ValueString);
#endif
            WrittenValues.Add(Key, ValueString);
            ++UpdatedCount;
        }
    }

    if (UpdatedCount > 0)
    {
        Package->SetDirtyFlag(true);
    }

    TSharedPtr<FJsonObject> Readback = MakeShared<FJsonObject>();
    for (const TPair<FString, FString>& Pair : WrittenValues)
    {
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 6, 0)
        const FString ActualValue = Meta.GetValue(Asset, *Pair.Key);
#else
        const FString ActualValue = Meta->GetValue(Asset, *Pair.Key);
#endif
        if (ActualValue != Pair.Value)
        {
            Ctx.SendError(TEXT("VERIFICATION_FAILED"),
                FString::Printf(TEXT("Metadata key '%s' failed readback after update"), *Pair.Key));
            return true;
        }
        Readback->SetStringField(Pair.Key, ActualValue);
    }

    FString PackageName = Package->GetName();
    int64 SizeBytes = 0;
    EAssetSaveState SaveState = EAssetSaveState::NotRequested;
    const bool bSaveRequested = bSave && UpdatedCount > 0;
    bool bSavedToDisk = false;
    if (bSaveRequested)
    {
        bSavedToDisk = SaveAssetToDiskReportingPresence(
            Asset, /*bForce=*/true, &PackageName, &SizeBytes, &SaveState);
    }

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetBoolField(TEXT("success"), true);
    Resp->SetStringField(TEXT("assetPath"), Asset->GetPathName());
    Resp->SetStringField(TEXT("package"), PackageName);
    Resp->SetNumberField(TEXT("updatedKeys"), UpdatedCount);
    Resp->SetObjectField(TEXT("metadata"), Readback);
    if (bSaveRequested)
    {
        AddAssetSaveSizeReport(Resp, SizeBytes, bSavedToDisk);
    }
    AddAssetSaveReport(Resp, bSaveRequested, bSavedToDisk, SaveState);

    AddAssetVerification(Resp, Asset);

    Ctx.SendSuccess(Resp);
    return true;
}

// ============================================================================
// asset.get_metadata
// ============================================================================
REGISTER_RPC_HANDLER("asset.get_metadata", "asset", "Read all UMetaData key/value entries plus class-derived tags for one asset. Returns the same data structure that asset.set_metadata writes.",
    RPC_PARAMS(
        RPC_PARAM_REQ("assetPath", "path", "Asset path to read metadata for.")
    ))
{
    FString AssetPath = Ctx.GetString(TEXT("assetPath"));
    if (AssetPath.IsEmpty())
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("assetPath required"));
        return true;
    }

    const FResolvedAsset Resolved = ResolveAsset(AssetPath, /*bLoadObject=*/true);
    if (!Resolved.bExists)
    {
        Ctx.SendError(TEXT("ASSET_NOT_FOUND"), Resolved.ErrorMessage);
        return true;
    }

    UObject* Asset = Resolved.Object;
    if (!Asset)
    {
        Ctx.SendError(TEXT("LOAD_FAILED"), TEXT("Failed to load asset"));
        return true;
    }

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetBoolField(TEXT("success"), true);
    Resp->SetStringField(TEXT("assetPath"), AssetPath);

    // 1. Asset Registry Tags
    FAssetData AssetData(Asset);
    // Single-sourced via BuildAssetRegistryTagsObject so asset.get and
    // asset.get_metadata emit an identically-shaped name->value tags map.
    Resp->SetObjectField(TEXT("tags"), BuildAssetRegistryTagsObject(AssetData));

    // 2. Package Metadata
    UPackage* Package = Asset->GetOutermost();
    if (Package)
    {
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 6, 0)
        FMetaData& Meta = Package->GetMetaData();
        bool bHasMeta = FMetaData::GetMapForObject(Asset) != nullptr;
        Resp->SetBoolField(TEXT("debug_has_meta"), bHasMeta);

        const TMap<FName, FString>* ObjectMeta = FMetaData::GetMapForObject(Asset);
#else
        UMetaData* Meta = Package->GetMetaData();
        bool bHasMeta = Meta->GetMapForObject(Asset) != nullptr;
        Resp->SetBoolField(TEXT("debug_has_meta"), bHasMeta);

        const TMap<FName, FString>* ObjectMeta = Meta->GetMapForObject(Asset);
#endif
        if (ObjectMeta)
        {
            TSharedPtr<FJsonObject> MetaObj = MakeShared<FJsonObject>();
            for (const auto& Entry : *ObjectMeta)
            {
                MetaObj->SetStringField(Entry.Key.ToString(), Entry.Value);
            }
            Resp->SetObjectField(TEXT("metadata"), MetaObj);
        }
    }

    Ctx.SendSuccess(Resp);
    return true;
}

// ============================================================================
// asset.set_tags
// ============================================================================
REGISTER_RPC_HANDLER("asset.set_tags", "asset", "Write boolean metadata tags on an asset (each tag is stored as key=true in UMetaData). Convenience wrapper around asset.set_metadata for tag-only flags. Idempotent; previously-set tags not in the new array are NOT removed.",
    RPC_PARAMS(
        RPC_PARAM_REQ("assetPath", "path", "Asset path to tag."),
        RPC_PARAM_OPT("tags", "array", "Array of tag name strings to set as boolean metadata; empty array is a no-op.")
    ))
{
    FString AssetPath = Ctx.GetString(TEXT("assetPath"));
    if (AssetPath.IsEmpty())
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("assetPath required"));
        return true;
    }

    TSharedPtr<FJsonObject> Payload = Ctx.GetRawPayload();
    const TArray<TSharedPtr<FJsonValue>>* TagsArray = nullptr;
    TArray<FString> Tags;
    if (Payload->TryGetArrayField(TEXT("tags"), TagsArray) && TagsArray)
    {
        for (const TSharedPtr<FJsonValue>& Val : *TagsArray)
        {
            if (Val.IsValid() && Val->Type == EJson::String)
            {
                Tags.Add(Val->AsString());
            }
        }
    }

    if (Tags.Num() == 0)
    {
        TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
        Resp->SetBoolField(TEXT("success"), true);
        Resp->SetStringField(TEXT("assetPath"), AssetPath);
        Resp->SetNumberField(TEXT("appliedTags"), 0);
        Ctx.SendSuccess(TEXT("No tags provided; no-op"), Resp);
        return true;
    }

    const FResolvedAsset Resolved = ResolveAsset(AssetPath, /*bLoadObject=*/true);
    if (!Resolved.bExists)
    {
        Ctx.SendError(TEXT("ASSET_NOT_FOUND"), Resolved.ErrorMessage);
        return true;
    }

    UObject* Asset = Resolved.Object;
    if (!Asset)
    {
        Ctx.SendError(TEXT("LOAD_FAILED"), TEXT("Failed to load asset"));
        return true;
    }

    Asset->Modify();
    int32 AppliedCount = 0;
    for (const FString& Tag : Tags)
    {
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 6, 0)
        Asset->GetPackage()->GetMetaData().SetValue(Asset, *Tag, TEXT("true"));
#else
        Asset->GetPackage()->GetMetaData()->SetValue(Asset, *Tag, TEXT("true"));
#endif
        AppliedCount++;
    }

    Asset->MarkPackageDirty();

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetBoolField(TEXT("success"), true);
    Resp->SetBoolField(TEXT("markedDirty"), true);
    Resp->SetStringField(TEXT("assetPath"), AssetPath);
    Resp->SetNumberField(TEXT("appliedTags"), AppliedCount);
    Ctx.SendSuccess(TEXT("Tags applied as metadata"), Resp);

    return true;
}

// ============================================================================
// asset.get_dependencies_classified (rich version with mode/role filtering)
// ============================================================================
REGISTER_RPC_HANDLER("asset.get_dependencies_classified", "asset", "Get classified asset dependencies with mode and role filtering",
    RPC_PARAMS(
        RPC_PARAM_REQ("assetPath", "path", "Path to the asset. Both the /Game/Foo/Bar and /Game/Foo/Bar.Bar spellings resolve to the same package; a path that resolves to no package is an ASSET_NOT_FOUND error, never an empty dependency list."),
        RPC_PARAM_OPT("recursive", "boolean", "Get dependencies recursively (default false)"),
        RPC_PARAM_OPT_ALIAS("mode", "string", "Filter mode: all, runtime_only, ui_tree_only, editor_only, code_call_refs", "dependencyMode"),
        RPC_PARAM_OPT_ALIAS("role", "string", "Filter role: all, package, manage, searchable_name", "dependencyRole")
    ))
{
    FString AssetPath = Ctx.GetString(TEXT("assetPath"));
    if (AssetPath.IsEmpty())
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("assetPath required"));
        return true;
    }

    if (!IsValidAssetPath(AssetPath))
    {
        Ctx.SendError(TEXT("INVALID_PATH"), TEXT("Invalid asset path"));
        return true;
    }

    TSharedPtr<FJsonObject> Payload = Ctx.GetRawPayload();
    bool bRecursive = false;
    Payload->TryGetBoolField(TEXT("recursive"), bRecursive);

    FString Mode = TEXT("all");
    if (!Payload->TryGetStringField(TEXT("mode"), Mode))
    {
        Payload->TryGetStringField(TEXT("dependencyMode"), Mode);
    }
    Mode = Mode.TrimStartAndEnd().ToLower();
    if (Mode.IsEmpty())
    {
        Mode = TEXT("all");
    }
    const bool bModeIsValid = Mode == TEXT("all") || Mode == TEXT("runtime_only") ||
                              Mode == TEXT("ui_tree_only") ||
                              Mode == TEXT("editor_only") ||
                              Mode == TEXT("code_call_refs");
    if (!bModeIsValid)
    {
        Ctx.SendError(TEXT("INVALID_MODE"),
            FString::Printf(
                TEXT("Invalid mode '%s'. Allowed values: all, runtime_only, ui_tree_only, editor_only, code_call_refs"),
                *Mode));
        return true;
    }

    FString Role = TEXT("all");
    if (!Payload->TryGetStringField(TEXT("role"), Role))
    {
        Payload->TryGetStringField(TEXT("dependencyRole"), Role);
    }
    Role = Role.TrimStartAndEnd().ToLower();
    if (Role.IsEmpty())
    {
        Role = TEXT("all");
    }

    // Same defect and same fix as asset.get_dependencies: the old code only ran
    // FPackageName::ObjectPathToPackageName, a pure string operation, so an
    // unresolvable path produced an empty classified result with isError:false -
    // indistinguishable from "nothing depends on this". The shared resolver
    // (Utils/AssetUtils.h, which documents exactly what it accepts and rejects)
    // proves the package exists and yields the FName GetDependencies is keyed on.
    //
    // Placed AFTER the mode/role parse on purpose: `mode` is caller input and a
    // bad one must still report INVALID_MODE rather than be masked by whatever
    // the path did.
    const FResolvedAssetPackage Resolved = ResolveAssetPathToPackage(AssetPath);
    if (!Resolved.bIsValid)
    {
        Ctx.SendError(TEXT("ASSET_NOT_FOUND"), Resolved.ErrorMessage);
        return true;
    }
    const FName RootPackageName = Resolved.PackageName;

    FAssetRegistryModule& AssetRegistryModule =
        FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
    IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();

    struct FDependencyRoleFlags
    {
        bool bPackage = false;
        bool bManage = false;
        bool bSearchableName = false;
    };

    TMap<FString, FDependencyRoleFlags> DependencyRoleMap;

    auto MergeDependenciesForCategory = [&](const FName& Node,
                                            UE::AssetRegistry::EDependencyCategory Category,
                                            const FString& CategoryName,
                                            TArray<FName>& OutDiscovered)
    {
        TArray<FName> CategoryDependencies;
        AssetRegistry.GetDependencies(Node, CategoryDependencies, Category,
                                      UE::AssetRegistry::EDependencyQuery::Hard);
        for (const FName& Dep : CategoryDependencies)
        {
            const FString DepPath = Dep.ToString();
            FDependencyRoleFlags& Flags = DependencyRoleMap.FindOrAdd(DepPath);
            if (CategoryName == TEXT("package"))
            {
                Flags.bPackage = true;
            }
            else if (CategoryName == TEXT("manage"))
            {
                Flags.bManage = true;
            }
            else if (CategoryName == TEXT("searchable_name"))
            {
                Flags.bSearchableName = true;
            }
            OutDiscovered.Add(Dep);
        }
    };

    // Visited-set BFS mirroring asset.get_dependencies: seed the queue with the
    // root package, and for each popped node re-run the three-category merge,
    // enqueuing unseen deps when recursion is requested. Non-recursive stays a
    // single level (only the root is processed, its deps are never enqueued).
    TSet<FName> Visited;
    TArray<FName> Queue;
    Queue.Add(RootPackageName);
    while (Queue.Num() > 0)
    {
        const FName Current = Queue.Pop();
        if (Visited.Contains(Current))
        {
            continue;
        }
        Visited.Add(Current);

        TArray<FName> Discovered;
        MergeDependenciesForCategory(Current, UE::AssetRegistry::EDependencyCategory::Package, TEXT("package"), Discovered);
        MergeDependenciesForCategory(Current, UE::AssetRegistry::EDependencyCategory::Manage, TEXT("manage"), Discovered);
        MergeDependenciesForCategory(Current, UE::AssetRegistry::EDependencyCategory::SearchableName, TEXT("searchable_name"), Discovered);

        if (bRecursive)
        {
            for (const FName& Dep : Discovered)
            {
                if (!Visited.Contains(Dep))
                {
                    Queue.Add(Dep);
                }
            }
        }
    }

    auto MatchesRole = [&](const FDependencyRoleFlags& Flags) -> bool
    {
        if (Role == TEXT("all")) return true;
        if (Role == TEXT("package")) return Flags.bPackage;
        if (Role == TEXT("manage")) return Flags.bManage;
        if (Role == TEXT("searchable_name")) return Flags.bSearchableName;
        return true;
    };

    auto ClassifyBucket = [&](const FString& DepPath, const FDependencyRoleFlags& Flags) -> FString
    {
        FString Lower = DepPath.ToLower();
        if (Flags.bSearchableName || Lower.StartsWith(TEXT("/script/")) ||
            Lower.Contains(TEXT(".h")) || Lower.Contains(TEXT(".cpp")))
        {
            return TEXT("code_call_ref");
        }
        if (Lower.Contains(TEXT("/editor/")) || Lower.Contains(TEXT("/developers/")) ||
            Lower.Contains(TEXT("/developertools/")) || Lower.Contains(TEXT("editorutility")))
        {
            return TEXT("editor");
        }
        if (Lower.Contains(TEXT("/ui/")) || Lower.Contains(TEXT("/hud/")) ||
            Lower.Contains(TEXT("/umg/")) || Lower.Contains(TEXT("widget")))
        {
            return TEXT("ui_tree");
        }
        return TEXT("runtime");
    };

    auto MatchesMode = [&](const FString& Bucket) -> bool
    {
        if (Mode == TEXT("all")) return true;
        if (Mode == TEXT("runtime_only")) return Bucket == TEXT("runtime");
        if (Mode == TEXT("ui_tree_only")) return Bucket == TEXT("ui_tree");
        if (Mode == TEXT("editor_only")) return Bucket == TEXT("editor");
        if (Mode == TEXT("code_call_refs")) return Bucket == TEXT("code_call_ref");
        return true;
    };

    TArray<TSharedPtr<FJsonValue>> DepArray;
    TArray<TSharedPtr<FJsonValue>> ClassifiedArray;

    int32 RuntimeCount = 0;
    int32 UiCount = 0;
    int32 EditorCount = 0;
    int32 CodeCount = 0;
    int32 PackageRoleCount = 0;
    int32 ManageRoleCount = 0;
    int32 SearchableRoleCount = 0;

    for (const TPair<FString, FDependencyRoleFlags>& Pair : DependencyRoleMap)
    {
        const FString& DepPath = Pair.Key;
        const FDependencyRoleFlags& Flags = Pair.Value;

        if (!MatchesRole(Flags))
        {
            continue;
        }

        if (Flags.bPackage) PackageRoleCount++;
        if (Flags.bManage) ManageRoleCount++;
        if (Flags.bSearchableName) SearchableRoleCount++;

        const FString Bucket = ClassifyBucket(DepPath, Flags);
        if (Bucket == TEXT("runtime")) RuntimeCount++;
        else if (Bucket == TEXT("ui_tree")) UiCount++;
        else if (Bucket == TEXT("editor")) EditorCount++;
        else if (Bucket == TEXT("code_call_ref")) CodeCount++;

        if (!MatchesMode(Bucket))
        {
            continue;
        }

        DepArray.Add(MakeShared<FJsonValueString>(DepPath));

        TSharedPtr<FJsonObject> Classified = MakeShared<FJsonObject>();
        Classified->SetStringField(TEXT("path"), DepPath);
        Classified->SetStringField(TEXT("category"), Bucket);
        Classified->SetBoolField(TEXT("isPackageDependency"), Flags.bPackage);
        Classified->SetBoolField(TEXT("isManageDependency"), Flags.bManage);
        Classified->SetBoolField(TEXT("isSearchableNameDependency"), Flags.bSearchableName);
        ClassifiedArray.Add(MakeShared<FJsonValueObject>(Classified));
    }

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetBoolField(TEXT("success"), true);
    Resp->SetArrayField(TEXT("dependencies"), DepArray);
    Resp->SetArrayField(TEXT("classifiedDependencies"), ClassifiedArray);
    Resp->SetStringField(TEXT("dependencyMode"), Mode);
    Resp->SetStringField(TEXT("dependencyRole"), Role);
    // Echo what the path resolved to, so an empty list reads as "this package has
    // no classified dependencies" and not "maybe I named the wrong thing".
    Resp->SetStringField(TEXT("assetPath"), AssetPath);
    Resp->SetStringField(TEXT("packageName"), RootPackageName.ToString());
    Resp->SetBoolField(TEXT("nativeRoleAware"), true);
    Resp->SetBoolField(TEXT("recursive"), bRecursive);
    Resp->SetNumberField(TEXT("dependencyCount"), DepArray.Num());
    Resp->SetNumberField(TEXT("classifiedCount"), ClassifiedArray.Num());

    TSharedPtr<FJsonObject> CategoryCounts = MakeShared<FJsonObject>();
    CategoryCounts->SetNumberField(TEXT("runtime"), RuntimeCount);
    CategoryCounts->SetNumberField(TEXT("ui_tree"), UiCount);
    CategoryCounts->SetNumberField(TEXT("editor"), EditorCount);
    CategoryCounts->SetNumberField(TEXT("code_call_ref"), CodeCount);
    Resp->SetObjectField(TEXT("categoryCounts"), CategoryCounts);

    TSharedPtr<FJsonObject> RoleCounts = MakeShared<FJsonObject>();
    RoleCounts->SetNumberField(TEXT("package"), PackageRoleCount);
    RoleCounts->SetNumberField(TEXT("manage"), ManageRoleCount);
    RoleCounts->SetNumberField(TEXT("searchable_name"), SearchableRoleCount);
    Resp->SetObjectField(TEXT("roleCounts"), RoleCounts);

    Ctx.SendSuccess(Resp);
    return true;
}

// ============================================================================
// asset.get_asset_graph
// ============================================================================
REGISTER_RPC_HANDLER("asset.get_asset_graph", "asset", "Get asset dependency graph via BFS traversal",
    RPC_PARAMS(
        RPC_PARAM_REQ("assetPath", "path", "Root asset path"),
        RPC_PARAM_OPT("maxDepth", "number", "Max traversal depth (default 3)")
    ))
{
    FString AssetPath = Ctx.GetString(TEXT("assetPath"));
    if (AssetPath.IsEmpty())
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("assetPath required"));
        return true;
    }

    if (!IsValidAssetPath(AssetPath))
    {
        Ctx.SendError(TEXT("INVALID_PATH"), TEXT("Invalid asset path"));
        return true;
    }

    TSharedPtr<FJsonObject> Payload = Ctx.GetRawPayload();
    int32 MaxDepth = 3;
    Payload->TryGetNumberField(TEXT("maxDepth"), MaxDepth);

    FAssetRegistryModule& AssetRegistryModule =
        FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
    IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();

    TSharedPtr<FJsonObject> GraphObj = MakeShared<FJsonObject>();

    TArray<FString> Queue;
    Queue.Add(AssetPath);

    TSet<FString> Visited;
    Visited.Add(AssetPath);

    TMap<FString, int32> Depths;
    Depths.Add(AssetPath, 0);

    int32 Head = 0;
    while (Head < Queue.Num())
    {
        FString Current = Queue[Head++];
        int32 CurrentDepth = Depths[Current];

        TArray<FName> Dependencies;
        AssetRegistry.GetDependencies(FName(*Current), Dependencies);

        TArray<TSharedPtr<FJsonValue>> DepArray;
        for (const FName& Dep : Dependencies)
        {
            FString DepStr = Dep.ToString();
            if (!DepStr.StartsWith(TEXT("/Game")))
                continue;

            DepArray.Add(MakeShared<FJsonValueString>(DepStr));

            if (CurrentDepth < MaxDepth)
            {
                if (!Visited.Contains(DepStr))
                {
                    Visited.Add(DepStr);
                    Depths.Add(DepStr, CurrentDepth + 1);
                    Queue.Add(DepStr);
                }
            }
        }
        GraphObj->SetArrayField(Current, DepArray);
    }

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetBoolField(TEXT("success"), true);
    Resp->SetObjectField(TEXT("graph"), GraphObj);
    Ctx.SendSuccess(Resp);
    return true;
}
