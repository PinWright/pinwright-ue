// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "PinWrightSubsystem.h"
#include "PinWrightHelpers.h"
#include "Utils/AssetUtils.h"
#include "Utils/ClassUtils.h"
#include "Utils/PropertyUtils.h"

#include "AssetRegistry/ARFilter.h"
#include "AssetRegistry/AssetRegistryModule.h"

#include "Compat/EngineVersionCompat.h"
#include "UObject/MetaData.h"

// ---- asset.get_dependencies ----
REGISTER_RPC_HANDLER("asset.get_dependencies", "asset", "Return assets that the given asset directly hard-references (Hard package dependencies). For the inverse (who references this asset) use asset.dependencies; for classified deps use asset.get_dependencies_classified.",
    RPC_PARAMS(
        RPC_PARAM_REQ("assetPath", "path", "Asset path whose outbound dependencies are being read. Both the /Game/Foo/Bar and /Game/Foo/Bar.Bar spellings resolve to the same package; a path that resolves to no package is an ASSET_NOT_FOUND error, never an empty dependency list."),
        RPC_PARAM_OPT("recursive", "boolean", "When true, transitively follows dependencies; defaults to false (immediate dependencies only).")
    ))
{
    const FString RequestedPath = Ctx.GetString(TEXT("assetPath"));
    bool bRecursive = Ctx.GetBool(TEXT("recursive"), false);

    // A path that resolves to nothing must ERROR, not report an empty list.
    // This verb sits directly on the delete path: a caller doing the responsible
    // thing - "is anything still using this before I remove it?" - reads
    // {"dependencies":[]} with isError:false as permission to proceed, and a
    // typo'd, unmounted or already-deleted path used to produce exactly that.
    // The old code only ran FPackageName::ObjectPathToPackageName, which is a
    // string operation with no notion of existence: it returns its input
    // unchanged when there is no '.', so every unresolvable path sailed through
    // into a GetDependencies call that found no graph node and answered nothing.
    //
    // ResolveAssetPathToPackage does the existence proof and yields the package
    // FName GetDependencies is keyed on. Its header comment (Utils/AssetUtils.h)
    // states exactly which spellings it accepts - including a package whose leaf
    // name differs from the asset name, and a package holding several assets,
    // which all resolve to the one package node this graph is keyed on.
    const FResolvedAssetPackage Resolved = ResolveAssetPathToPackage(RequestedPath);
    if (!Resolved.bIsValid)
    {
        Ctx.SendError(TEXT("ASSET_NOT_FOUND"), Resolved.ErrorMessage);
        return true;
    }
    const FName PackageName = Resolved.PackageName;

    FAssetRegistryModule& AssetRegistryModule =
        FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
    TArray<FName> Dependencies;
    UE::AssetRegistry::EDependencyCategory Category = UE::AssetRegistry::EDependencyCategory::Package;
    UE::AssetRegistry::EDependencyQuery Query = UE::AssetRegistry::EDependencyQuery::Hard;

    if (bRecursive)
    {
        TSet<FName> Visited;
        TArray<FName> Queue;
        Queue.Add(PackageName);
        while (Queue.Num() > 0)
        {
            FName Current = Queue.Pop();
            if (Visited.Contains(Current)) continue;
            Visited.Add(Current);
            TArray<FName> DirectDeps;
            AssetRegistryModule.Get().GetDependencies(Current, DirectDeps, Category, Query);
            for (const FName& Dep : DirectDeps)
            {
                Dependencies.AddUnique(Dep);
                if (!Visited.Contains(Dep)) Queue.Add(Dep);
            }
        }
    }
    else
    {
        AssetRegistryModule.Get().GetDependencies(PackageName, Dependencies, Category, Query);
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    TArray<TSharedPtr<FJsonValue>> DepArray;
    for (const FName& Dep : Dependencies)
    {
        DepArray.Add(MakeShared<FJsonValueString>(Dep.ToString()));
    }
    Result->SetArrayField(TEXT("dependencies"), DepArray);
    // Echo both the string the caller sent and the package it resolved to, so an
    // empty list is readable as "this package really has no hard dependencies"
    // rather than "maybe I named the wrong thing".
    Result->SetStringField(TEXT("assetPath"), RequestedPath);
    Result->SetStringField(TEXT("packageName"), PackageName.ToString());

    Ctx.SendSuccess(Result);
    return true;
}

// ---- asset.map_references ----
REGISTER_RPC_HANDLER("asset.map_references", "asset", "Return soft-UWorld UPROPERTY references for a single loaded asset, matching map_references.json from asset.dump.",
    RPC_PARAMS(
        RPC_PARAM_REQ("assetPath", "path", "Asset path whose soft-UWorld UPROPERTY references are being read.")
    ))
{
    FString AssetPath;
    if (!Ctx.RequireString(TEXT("assetPath"), AssetPath)) return true;

    UObject* Asset = ResolveAsset(AssetPath, /*bLoadObject=*/true).Object;

    TSharedPtr<FJsonObject> Result = BuildMapReferencesJson(Asset);
    if (!Result.IsValid())
    {
        Result = MakeShared<FJsonObject>();
        Result->SetNumberField(TEXT("schemaVersion"), 1);
        Result->SetArrayField(TEXT("references"), TArray<TSharedPtr<FJsonValue>>());
    }

    Ctx.SendSuccess(Result);
    return true;
}

// ---- asset.find_by_tag ----
REGISTER_RPC_HANDLER("asset.find_by_tag", "asset", "Search the asset registry for assets whose UMetaData contains a given tag, optionally filtered by tag value. This is a TAG search, not a name search — to find an asset by what it is CALLED use asset.search. For finding world actors by tag instead see asset.find_objects_by_tag or system.inspect.find_by_tag.",
    RPC_PARAMS(
        RPC_PARAM_REQ("tag", "string", "Metadata tag name (FName) to look for on each asset."),
        RPC_PARAM_OPT("value", "string", "Expected tag value (string-compared); when empty any asset with the tag set matches."),
        RPC_PARAM_OPT("path", "path", "Content-browser folder to limit the scan (e.g. '/Game/Foo'); defaults to '/Game'."),
        RPC_PARAM_OPT("limit", "number", "Max matches to RETURN (default 50, max 500). This bounds the output only - a tag with few or no matches never fills it, so it cannot bound the walk; use scanLimit for that."),
        RPC_PARAM_OPT("scanLimit", "number", "Max candidates to LOAD AND TEST (default 500, max 10000). Reading UMetaData requires loading each candidate synchronously on the game thread, so this is the parameter that bounds the cost. When the walk stops here the response reports scanComplete:false, truncated:true and stopReason 'scanLimit', and omits totalMatches.")
    ))
{
    FString Tag = Ctx.GetString(TEXT("tag"));
    FString ExpectedValue = Ctx.GetString(TEXT("value"));

    if (Tag.IsEmpty())
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("tag required"));
        return true;
    }

    FString Path = Ctx.GetString(TEXT("path"), TEXT("/Game"));
    const int32 Limit = FMath::Clamp(Ctx.GetInt(TEXT("limit"), 50), 1, 500);
    // The scan gets its OWN budget. `limit` counts matches, and a tag that
    // matches nothing never reaches it, so a match cap can never bound a walk
    // whose per-step cost is a synchronous package load.
    const int32 ScanLimit = FMath::Clamp(Ctx.GetInt(TEXT("scanLimit"), 500), 1, 10000);

    FAssetRegistryModule& AssetRegistryModule =
        FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
    FARFilter Filter;
    Filter.PackagePaths.Add(FName(*Path));
    Filter.bRecursivePaths = true;

    TArray<FAssetData> AssetDataList;
    AssetRegistryModule.Get().GetAssets(Filter, AssetDataList);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    TArray<TSharedPtr<FJsonValue>> AssetsArray;

    // UMetaData lives in the package, not in the registry row, so every candidate
    // has to be LOADED before it can be tested. Unbounded, that is a full load of
    // the content tree on the game thread for a query that usually wants a handful
    // of rows.
    //
    // TWO independent bounds, because they measure different things. `Limit`
    // counts MATCHES and bounds the output. It does NOT bound the walk: a tag
    // that matches nothing (a typo, a tag no asset carries, a wrong `path`) never
    // fills it, so the loop used to run to the end of the candidate list and load
    // every asset under /Game before answering "0 matches". The comment that used
    // to sit here claimed the cap bounded the scan as well; it did not, and that
    // claim was part of the defect. `ScanLimit` counts CANDIDATES EXAMINED and is
    // the bound that actually holds in the worst case.
    //
    // The counters make whichever stop happened visible: `scanned` is what was
    // examined, `scanCandidates` is what the registry filter offered,
    // `stopReason` says which bound ended the walk, and `totalMatches` is emitted
    // only on a complete scan - a total derived from a stopped walk would measure
    // the bound, not the content.
    int32 Scanned = 0;
    bool bHitMatchLimit = false;
    bool bHitScanLimit = false;
    for (const FAssetData& Data : AssetDataList)
    {
        if (AssetsArray.Num() >= Limit)
        {
            bHitMatchLimit = true;
            break;
        }
        if (Scanned >= ScanLimit)
        {
            bHitScanLimit = true;
            break;
        }
        ++Scanned;

        const FString AssetPath_Local = Data.GetSoftObjectPath().ToString();
        UObject* Asset = ResolveAsset(AssetPath_Local, /*bLoadObject=*/true).Object;
        if (!Asset) continue;

        FString MetadataValue;
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 6, 0)
        const TMap<FName, FString>* ObjectMeta = FMetaData::GetMapForObject(Asset);
#else
        const TMap<FName, FString>* ObjectMeta =
            Asset->GetPackage()->GetMetaData()->GetMapForObject(Asset);
#endif
        if (ObjectMeta)
        {
            if (const FString* Value = ObjectMeta->Find(FName(*Tag)))
            {
                MetadataValue = *Value;
            }
        }

        bool bMatches = !MetadataValue.IsEmpty();
        if (bMatches && !ExpectedValue.IsEmpty())
        {
            bMatches = MetadataValue.Equals(ExpectedValue, ESearchCase::IgnoreCase);
        }

        if (bMatches)
        {
            TSharedPtr<FJsonObject> AssetObj = MakeShared<FJsonObject>();
            AssetObj->SetStringField(TEXT("assetName"), Data.AssetName.ToString());
            AssetObj->SetStringField(TEXT("assetPath"), AssetPath_Local);
            AssetObj->SetStringField(TEXT("classPath"), Data.AssetClassPath.ToString());
            AssetObj->SetStringField(TEXT("tagValue"), MetadataValue);
            AddAssetVerification(AssetObj, Asset);
            AssetsArray.Add(MakeShared<FJsonValueObject>(AssetObj));
        }
    }

    // Every break above leaves an unconsumed candidate, so Scanned < Num there and
    // a stopped walk can never report complete. An empty candidate list is
    // complete (0 >= 0), which is correct: nothing was withheld.
    const bool bScanComplete = (Scanned >= AssetDataList.Num());

    Result->SetArrayField(TEXT("assets"), AssetsArray);
    Result->SetNumberField(TEXT("count"), AssetsArray.Num());
    Result->SetNumberField(TEXT("limit"), Limit);
    Result->SetNumberField(TEXT("scanLimit"), ScanLimit);
    Result->SetNumberField(TEXT("scanned"), Scanned);
    Result->SetNumberField(TEXT("scanCandidates"), AssetDataList.Num());
    Result->SetBoolField(TEXT("scanComplete"), bScanComplete);
    // truncated is true whenever rows could still be out there — i.e. whenever the
    // walk stopped early, for EITHER bound. On a completed scan the count IS the
    // total, so it is also safe to publish totalMatches; on a stopped one there is
    // no measured total and the field is omitted rather than filled with a bound.
    Result->SetBoolField(TEXT("truncated"), !bScanComplete);
    // Which bound ended the walk, so a caller knows what to raise: "limit" means
    // more matches exist, "scanLimit" means more candidates were never looked at
    // (and says nothing about how many of them match). The three cases are
    // exhaustive and mutually exclusive: a break always leaves an unconsumed
    // candidate, so exactly one flag is set iff bScanComplete is false.
    const TCHAR* StopReason = TEXT("complete");
    if (bHitMatchLimit)
    {
        StopReason = TEXT("limit");
    }
    else if (bHitScanLimit)
    {
        StopReason = TEXT("scanLimit");
    }
    Result->SetStringField(TEXT("stopReason"), StopReason);
    if (bScanComplete)
    {
        Result->SetNumberField(TEXT("totalMatches"), AssetsArray.Num());
    }
    Ctx.SendSuccess(Result);
    return true;
}

// ---- asset.search_assets ----
REGISTER_RPC_HANDLER("asset.search_assets", "asset", "List assets by CLASS and/or package path — the no-name-in-hand path (\"every SoundWave\", \"everything under /Game/Maps\"). Every parameter is optional and there is no name axis at all; if you know roughly what the asset is called, use asset.search instead. Reports totalMatches and truncated so a capped page is visible.",
    RPC_PARAMS(
        RPC_PARAM_OPT("classNames", "array", "Array of class names. Accepts short names without U/A prefix (e.g. 'Blueprint', 'StaticMesh', 'MyDataAsset') or full paths (e.g. '/Script/Engine.Blueprint')"),
        RPC_PARAM_OPT("packagePaths", "array", "Array of package paths to search (e.g. ['/Game', '/SomePlugin/Maps'])"),
        RPC_PARAM_OPT("recursivePaths", "boolean", "Recurse into subfolders (default true)"),
        RPC_PARAM_OPT("recursiveClasses", "boolean", "Include subclasses in results, e.g. classNames=['DataAsset'] with recursiveClasses=true finds all UDataAsset-derived types (default false)"),
        RPC_PARAM_OPT("limit", "number", "Max results to return, 0 for unlimited (default 100)")
    ))
{
    TSharedPtr<FJsonObject> Payload = Ctx.GetRawPayload();
    FARFilter Filter;

    // Parse Class Names
    TArray<FString> UnresolvedNames;
    const TArray<TSharedPtr<FJsonValue>>* ClassNamesPtr;
    if (Payload->TryGetArrayField(TEXT("classNames"), ClassNamesPtr) && ClassNamesPtr)
    {
        for (const TSharedPtr<FJsonValue>& Val : *ClassNamesPtr)
        {
            const FString ClassName = Val->AsString();
            if (!ClassName.IsEmpty())
            {
                if (ClassName.Contains(TEXT("/")))
                {
                    Filter.ClassPaths.Add(FTopLevelAssetPath(ClassName));
                }
                else
                {
                    // Map common short names to full paths
                    struct FClassMapping { const TCHAR* ShortName; const TCHAR* ScriptPath; const TCHAR* ClassName50; };
                    static const FClassMapping Mappings[] = {
                        { TEXT("Blueprint"),              TEXT("/Script/Engine"), TEXT("Blueprint") },
                        { TEXT("StaticMesh"),             TEXT("/Script/Engine"), TEXT("StaticMesh") },
                        { TEXT("SkeletalMesh"),           TEXT("/Script/Engine"), TEXT("SkeletalMesh") },
                        { TEXT("Material"),               TEXT("/Script/Engine"), TEXT("Material") },
                        { TEXT("MaterialInstanceConstant"), TEXT("/Script/Engine"), TEXT("MaterialInstanceConstant") },
                        { TEXT("MaterialInstance"),       TEXT("/Script/Engine"), TEXT("MaterialInstanceConstant") },
                        { TEXT("Texture2D"),              TEXT("/Script/Engine"), TEXT("Texture2D") },
                        { TEXT("Level"),                  TEXT("/Script/Engine"), TEXT("World") },
                        { TEXT("World"),                  TEXT("/Script/Engine"), TEXT("World") },
                        { TEXT("SoundCue"),               TEXT("/Script/Engine"), TEXT("SoundCue") },
                        { TEXT("SoundWave"),              TEXT("/Script/Engine"), TEXT("SoundWave") },
                    };

                    bool bMapped = false;
                    for (const auto& M : Mappings)
                    {
                        if (ClassName.Equals(M.ShortName, ESearchCase::IgnoreCase))
                        {
                            Filter.ClassPaths.Add(FTopLevelAssetPath(M.ScriptPath, M.ClassName50));
                            bMapped = true;
                            break;
                        }
                    }

                    if (!bMapped)
                    {
                        // Unified resolver handles short names, U/A-prefix stripping,
                        // full /Script/ paths, and /Game/ BP classes.
                        UClass* FoundClass = ResolveUClass(ClassName);

                        if (FoundClass)
                        {
                            Filter.ClassPaths.Add(FoundClass->GetClassPathName());
                        }
                        else
                        {
                            UnresolvedNames.Add(ClassName);
                        }
                    }
                }
            }
        }
    }

    if (UnresolvedNames.Num() > 0)
    {
        Ctx.SendError(TEXT("CLASS_NOT_FOUND"),
            FString::Printf(TEXT("Could not resolve class name(s): %s. Use full path (e.g. /Script/Engine.Blueprint)"),
            *FString::Join(UnresolvedNames, TEXT(", "))));
        return true;
    }

    // Parse Package Paths
    const TArray<TSharedPtr<FJsonValue>>* PackagePathsPtr;
    if (Payload->TryGetArrayField(TEXT("packagePaths"), PackagePathsPtr) && PackagePathsPtr)
    {
        for (const TSharedPtr<FJsonValue>& Val : *PackagePathsPtr)
        {
            Filter.PackagePaths.Add(FName(*Val->AsString()));
        }
    }

    // Parse Recursion
    bool bRecursivePaths = true;
    if (Payload->HasField(TEXT("recursivePaths")))
        Payload->TryGetBoolField(TEXT("recursivePaths"), bRecursivePaths);
    Filter.bRecursivePaths = bRecursivePaths;

    bool bRecursiveClasses = false;
    if (Payload->HasField(TEXT("recursiveClasses")))
        Payload->TryGetBoolField(TEXT("recursiveClasses"), bRecursiveClasses);
    Filter.bRecursiveClasses = bRecursiveClasses;

    // Execute Query
    FAssetRegistryModule& AssetRegistryModule =
        FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
    TArray<FAssetData> AssetDataList;
    AssetRegistryModule.Get().GetAssets(Filter, AssetDataList);

    // Apply Limit. TotalMatches is captured BEFORE the truncation so the response
    // can say how many rows were withheld; the previous version SetNum'd the array
    // and reported only the post-truncation count, so a caller had no way to tell
    // "100 assets of this class exist" from "the first 100 of 3000".
    const int32 TotalMatches = AssetDataList.Num();
    int32 Limit = Ctx.GetInt(TEXT("limit"), 100);
    if (Limit > 0 && AssetDataList.Num() > Limit)
    {
        AssetDataList.SetNum(Limit);
    }

    // Build Response
    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    TArray<TSharedPtr<FJsonValue>> AssetsArray;

    for (const FAssetData& Data : AssetDataList)
    {
        TSharedPtr<FJsonObject> AssetObj = MakeShared<FJsonObject>();
        AssetObj->SetStringField(TEXT("assetName"), Data.AssetName.ToString());
        AssetObj->SetStringField(TEXT("assetPath"), Data.GetSoftObjectPath().ToString());
        AssetObj->SetStringField(TEXT("classPath"), Data.AssetClassPath.ToString());
        AssetsArray.Add(MakeShared<FJsonValueObject>(AssetObj));
    }

    Result->SetBoolField(TEXT("success"), true);
    Result->SetArrayField(TEXT("assets"), AssetsArray);
    Result->SetNumberField(TEXT("count"), AssetsArray.Num());
    Result->SetNumberField(TEXT("totalMatches"), TotalMatches);
    Result->SetBoolField(TEXT("truncated"), TotalMatches > AssetsArray.Num());

    Ctx.SendSuccess(Result);
    return true;
}
