// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "PinWrightHelpers.h"
#include "Dom/JsonObject.h"
#include "Templates/SharedPointer.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "AssetRegistry/AssetData.h"
#include "Engine/Blueprint.h"
#include "UObject/UObjectIterator.h"

// ============================================================================
// blueprint.list — Enumerate blueprint assets with filters and paging
// ============================================================================

REGISTER_RPC_HANDLER("blueprint.list", "blueprint",
    "Enumerate blueprint assets with filters and paging.",
    (TArray<FParamSpec>{
        { TEXT("path"), TEXT("string"), TEXT("Package path filter"), false, TEXT("") },
        { TEXT("class"), TEXT("string"), TEXT("Blueprint class filter (default: Blueprint)"), false, TEXT("Blueprint") },
        { TEXT("tag"), TEXT("string"), TEXT("Tag filter (key or key=value)"), false, TEXT("") },
        { TEXT("nameFilter"), TEXT("string"), TEXT("Name substring filter"), false, TEXT("") },
        { TEXT("pathStartsWith"), TEXT("string"), TEXT("Package path prefix filter"), false, TEXT("") },
        { TEXT("recursive"), TEXT("boolean"), TEXT("Search subfolders recursively"), false, TEXT("true") },
        { TEXT("offset"), TEXT("number"), TEXT("Pagination offset"), false, TEXT("0") },
        { TEXT("limit"), TEXT("number"), TEXT("Pagination limit (-1 for unlimited)"), false, TEXT("50") },
        { TEXT("filter"), TEXT("object"), TEXT("Nested filter object {path, class, tag, nameFilter, pathStartsWith, recursive}. The flat top-level spellings above win when both are supplied."), false, TEXT("") },
        { TEXT("pagination"), TEXT("object"), TEXT("Nested pagination object {offset, limit}. The flat top-level offset/limit win when both are supplied."), false, TEXT("") },
    }))
{
    const TSharedPtr<FJsonObject>& Payload = Ctx.GetRawPayload();
    if (!Payload.IsValid())
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("Payload missing."));
        return true;
    }

    // Parse filters (nested first, then flat override)
    FString PathFilter;
    FString ClassFilter = TEXT("Blueprint");
    FString TagFilter;
    FString NameFilter;
    FString PathStartsWith;
    bool bRecursive = true;
    int32 Offset = 0;
    int32 Limit = 50;

    bool bHasNestedFilter = false;
    bool bHasNestedPagination = false;
    bool bHasFlatFilter = false;
    bool bHasFlatPagination = false;

    const TSharedPtr<FJsonObject>* FilterObj = nullptr;
    if (Payload->TryGetObjectField(TEXT("filter"), FilterObj) && FilterObj && FilterObj->IsValid())
    {
        bHasNestedFilter = true;
        (*FilterObj)->TryGetStringField(TEXT("path"), PathFilter);
        (*FilterObj)->TryGetStringField(TEXT("class"), ClassFilter);
        (*FilterObj)->TryGetStringField(TEXT("tag"), TagFilter);
        (*FilterObj)->TryGetStringField(TEXT("nameFilter"), NameFilter);
        (*FilterObj)->TryGetStringField(TEXT("pathStartsWith"), PathStartsWith);
        (*FilterObj)->TryGetBoolField(TEXT("recursive"), bRecursive);
    }

    const TSharedPtr<FJsonObject>* PaginationObj = nullptr;
    if (Payload->TryGetObjectField(TEXT("pagination"), PaginationObj) && PaginationObj && PaginationObj->IsValid())
    {
        bHasNestedPagination = true;
        (*PaginationObj)->TryGetNumberField(TEXT("offset"), Offset);
        (*PaginationObj)->TryGetNumberField(TEXT("limit"), Limit);
    }

    if (Payload->HasTypedField<EJson::String>(TEXT("path")))
    {
        bHasFlatFilter = true;
        Payload->TryGetStringField(TEXT("path"), PathFilter);
    }

    if (Payload->HasTypedField<EJson::String>(TEXT("class")))
    {
        bHasFlatFilter = true;
        Payload->TryGetStringField(TEXT("class"), ClassFilter);
    }

    if (Payload->HasTypedField<EJson::String>(TEXT("tag")))
    {
        bHasFlatFilter = true;
        Payload->TryGetStringField(TEXT("tag"), TagFilter);
    }

    if (Payload->HasTypedField<EJson::String>(TEXT("nameFilter")))
    {
        bHasFlatFilter = true;
        Payload->TryGetStringField(TEXT("nameFilter"), NameFilter);
    }

    if (Payload->HasTypedField<EJson::String>(TEXT("pathStartsWith")))
    {
        bHasFlatFilter = true;
        Payload->TryGetStringField(TEXT("pathStartsWith"), PathStartsWith);
    }

    if (Payload->HasTypedField<EJson::Boolean>(TEXT("recursive")))
    {
        bHasFlatFilter = true;
        Payload->TryGetBoolField(TEXT("recursive"), bRecursive);
    }

    if (Payload->HasTypedField<EJson::Number>(TEXT("offset")))
    {
        bHasFlatPagination = true;
        Payload->TryGetNumberField(TEXT("offset"), Offset);
    }

    if (Payload->HasTypedField<EJson::Number>(TEXT("limit")))
    {
        bHasFlatPagination = true;
        Payload->TryGetNumberField(TEXT("limit"), Limit);
    }

    if (Offset < 0)
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("offset must be >= 0"));
        return true;
    }

    if (Limit < -1)
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("limit must be >= -1"));
        return true;
    }

    FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
    IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();

    FARFilter Filter;
    Filter.bRecursivePaths = bRecursive;
    Filter.bRecursiveClasses = true;
    // Only return on-disk assets. Without this flag, GetAssets also surfaces in-memory
    // sub-objects of any loaded level (e.g. Map.Map:PersistentLevel.ActorFoo_C_8), so a
    // native-class filter like class="Actor" leaks every placed level actor instance
    // instead of authorable Blueprint assets. Same fix asset.dump_folder shipped
    // (AssetDumpHandler.cpp); the default class="Blueprint" path never placed instances
    // in levels so it dodged the leak, but the native-class branch below did not.
    // Set on the shared Filter (before the class-branch split) so it applies to EVERY class
    // filter, not only the native branch that leaked. This intentionally also excludes any
    // dirty/unsaved in-memory-only UBlueprint from the default class="Blueprint" path — an
    // authorable-asset list should never surface unsaved, in-memory-only objects.
    Filter.bIncludeOnlyOnDiskAssets = true;

    if (!PathFilter.IsEmpty())
    {
        Filter.PackagePaths.Add(FName(*PathFilter));
    }
    else if (!PathStartsWith.IsEmpty())
    {
        Filter.PackagePaths.Add(FName(*PathStartsWith));
    }

    // Class filter
    bool bNativeParentWalkNeeded = false;
    UClass* ResolvedNativeClass = nullptr;
    if (ClassFilter == TEXT("Blueprint"))
    {
        Filter.ClassPaths.Add(UBlueprint::StaticClass()->GetClassPathName());
    }
    else if (!ClassFilter.IsEmpty())
    {
        // ResolveUClass accepts full paths and short names without firing the
        // FTopLevelAssetPath short-name ensure. Unresolved names pass through
        // with no class filter applied.
        ResolvedNativeClass = ResolveUClass(ClassFilter);
        if (ResolvedNativeClass)
        {
            Filter.ClassPaths.Add(FTopLevelAssetPath(ResolvedNativeClass->GetPathName()));
            // Native (non-UBlueprint) classes also need the BP parent-tag walk: a BP
            // .uasset's own AssetClassPath is /Script/Engine.Blueprint, so FARFilter alone
            // misses BP subclasses of native parents.
            bNativeParentWalkNeeded = !ResolvedNativeClass->IsChildOf(UBlueprint::StaticClass());
        }
    }

    TArray<FAssetData> AssetList;
    AssetRegistry.GetAssets(Filter, AssetList);

    if (bNativeParentWalkNeeded && ResolvedNativeClass)
    {
        AppendBlueprintAssetsDerivedFromNativeClass(
            AssetRegistry, ResolvedNativeClass, Filter.PackagePaths,
            Filter.bRecursivePaths, /*MaxResults=*/-1, AssetList);
    }

    if (!TagFilter.IsEmpty())
    {
        const FString NormalizedTag = TagFilter.TrimStartAndEnd();
        int32 SeparatorIndex = INDEX_NONE;
        const bool bHasTagValue = NormalizedTag.FindChar(TEXT('='), SeparatorIndex);

        FString TagKey = NormalizedTag;
        FString ExpectedTagValue;
        if (bHasTagValue)
        {
            TagKey = NormalizedTag.Left(SeparatorIndex).TrimStartAndEnd();
            ExpectedTagValue = NormalizedTag.Mid(SeparatorIndex + 1).TrimStartAndEnd();
        }

        TArray<FAssetData> TaggedAssets;
        for (const FAssetData& Asset : AssetList)
        {
            FString FoundValue;
            if (!Asset.GetTagValue(FName(*TagKey), FoundValue))
            {
                continue;
            }

            if (!bHasTagValue || FoundValue.Equals(ExpectedTagValue, ESearchCase::CaseSensitive))
            {
                TaggedAssets.Add(Asset);
            }
        }

        AssetList = MoveTemp(TaggedAssets);
    }

    if (!NameFilter.IsEmpty())
    {
        TArray<FAssetData> NameFilteredAssets;
        for (const FAssetData& Asset : AssetList)
        {
            if (Asset.AssetName.ToString().Contains(NameFilter, ESearchCase::IgnoreCase))
            {
                NameFilteredAssets.Add(Asset);
            }
        }
        AssetList = MoveTemp(NameFilteredAssets);
    }

    int32 TotalCount = AssetList.Num();

    if (Offset > 0)
    {
        if (Offset < AssetList.Num())
        {
            AssetList.RemoveAt(0, Offset);
        }
        else
        {
            AssetList.Empty();
        }
    }

    if (Limit >= 0 && AssetList.Num() > Limit)
    {
        AssetList.SetNum(Limit);
    }

    TArray<TSharedPtr<FJsonValue>> BlueprintsArray;
    for (const FAssetData& Asset : AssetList)
    {
        TSharedPtr<FJsonObject> BpObj = MakeShared<FJsonObject>();
        BpObj->SetStringField(TEXT("name"), Asset.AssetName.ToString());
        BpObj->SetStringField(TEXT("path"), Asset.GetSoftObjectPath().ToString());
        BpObj->SetStringField(TEXT("class"), Asset.AssetClassPath.ToString());
        BpObj->SetStringField(TEXT("packagePath"), Asset.PackagePath.ToString());

        FString ParentClass;
        if (Asset.GetTagValue(TEXT("ParentClass"), ParentClass))
        {
            BpObj->SetStringField(TEXT("parentClass"), ParentClass);
        }

        BlueprintsArray.Add(MakeShared<FJsonValueObject>(BpObj));
    }

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    FString InputShape = TEXT("none");
    if ((bHasNestedFilter || bHasNestedPagination) && (bHasFlatFilter || bHasFlatPagination))
    {
        InputShape = TEXT("mixed");
    }
    else if (bHasFlatFilter || bHasFlatPagination)
    {
        InputShape = TEXT("flat");
    }
    else if (bHasNestedFilter || bHasNestedPagination)
    {
        InputShape = TEXT("nested");
    }

    Resp->SetBoolField(TEXT("success"), true);
    Resp->SetArrayField(TEXT("blueprints"), BlueprintsArray);
    Resp->SetNumberField(TEXT("totalCount"), TotalCount);
    Resp->SetNumberField(TEXT("count"), BlueprintsArray.Num());
    Resp->SetNumberField(TEXT("effectiveOffset"), Offset);
    Resp->SetNumberField(TEXT("effectiveLimit"), Limit);
    Resp->SetStringField(TEXT("inputShape"), InputShape);

    Ctx.SendSuccess(Resp);
    return true;
}
