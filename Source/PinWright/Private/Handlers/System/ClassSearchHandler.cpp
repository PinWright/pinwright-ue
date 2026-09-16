// Copyright (c) 2026 Alexander Penkin. MIT License.

// ClassSearchHandler.cpp
// system.inspect.search_classes — ranked keyword search over the native UClass catalog
// (TObjectIterator<UClass>) with parent / module / abstract / deprecated filters.

#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/ParamSpec.h"

#include "Utils/ClassUtils.h"

#include "UObject/UObjectIterator.h"
#include "UObject/Class.h"
#include "UObject/Package.h"
#include "Modules/ModuleManager.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

namespace
{
    struct FCachedClassEntry
    {
        TWeakObjectPtr<UClass> Class;
        FString NameLower;
        FString DisplayNameLower;
        FString CategoryLower;
        FString ModuleLower;
        FString ModuleName;
    };

    // Extract the module name from a UClass's outer package.
    // Native UClasses live in `/Script/<Module>`; strip the prefix so callers
    // see e.g. "Engine" rather than "/Script/Engine".
    static FString ExtractModuleName(const UClass* Class)
    {
        if (!Class) return FString();
        const UPackage* Outermost = Class->GetOutermost();
        if (!Outermost) return FString();
        FString PackageName = Outermost->GetName();
        const FString ScriptPrefix = TEXT("/Script/");
        if (PackageName.StartsWith(ScriptPrefix))
        {
            return PackageName.Mid(ScriptPrefix.Len());
        }
        return PackageName;
    }

    // Cache state held by a function-local static so the delegate handle's
    // lifetime is tied to the cache instance, not raw TU-scope statics.
    struct FClassSearchCache
    {
        TArray<FCachedClassEntry> Entries;
        bool bDirty = true;
        FDelegateHandle ModulesChangedHandle;

        static FClassSearchCache& Get()
        {
            static FClassSearchCache C;
            return C;
        }
    };

    // Returns the weighted substring-match score for a single needle against
    // an entry's lowercase name/displayName/category/module fields.
    // Weights: name=3, displayName=2, category=1, module=1.
    static int32 ScoreSubstring(const FCachedClassEntry& Entry, const FString& Substr)
    {
        int32 Score = 0;
        if (Entry.NameLower.Contains(Substr))        Score += 3;
        if (Entry.DisplayNameLower.Contains(Substr)) Score += 2;
        if (Entry.CategoryLower.Contains(Substr))    Score += 1;
        if (Entry.ModuleLower.Contains(Substr))      Score += 1;
        return Score;
    }

    static void RebuildCacheIfDirty()
    {
        FClassSearchCache& Cache = FClassSearchCache::Get();
        if (!Cache.ModulesChangedHandle.IsValid())
        {
            Cache.ModulesChangedHandle = FModuleManager::Get().OnModulesChanged().AddLambda(
                [](FName, EModuleChangeReason) { FClassSearchCache::Get().bDirty = true; });
        }

        if (!Cache.bDirty) return;

        Cache.Entries.Reset();
        for (TObjectIterator<UClass> It; It; ++It)
        {
            UClass* Cls = *It;
            if (!Cls) continue;
            // REINST_ shells from hot-reload are not real classes; never surface them.
            if (Cls->HasAnyClassFlags(CLASS_NewerVersionExists)) continue;

            FCachedClassEntry Entry;
            Entry.Class = Cls;
            Entry.NameLower = Cls->GetName().ToLower();
            Entry.DisplayNameLower = Cls->GetMetaData(TEXT("DisplayName")).ToLower();
            Entry.CategoryLower = Cls->GetMetaData(TEXT("Category")).ToLower();
            Entry.ModuleName = ExtractModuleName(Cls);
            Entry.ModuleLower = Entry.ModuleName.ToLower();
            Cache.Entries.Add(MoveTemp(Entry));
        }

        Cache.bDirty = false;
    }
}

REGISTER_RPC_HANDLER("system.inspect.search_classes", "system.inspect",
    "Ranked keyword search over the native UClass catalog with parent / module / flag filters.",
    RPC_PARAMS(
        RPC_PARAM_REQ("query", "string", "Space-separated keywords; ranked match on class name, DisplayName, Category, module"),
        RPC_PARAM_OPT("parentClass", "classref", "Restrict to subclasses of this UClass (short name or /Script/Module.Name path)"),
        RPC_PARAM_OPT("moduleFilter", "array", "Restrict to specific modules (e.g. [\"Engine\", \"UMG\"]); case-insensitive"),
        RPC_PARAM_DEF("includeAbstract", "bool", "Include CLASS_Abstract classes", "false"),
        RPC_PARAM_DEF("includeDeprecated", "bool", "Include CLASS_Deprecated classes", "false"),
        RPC_PARAM_DEF("limit", "number", "Maximum results to return", "20")
    ))
{
    FString Query;
    if (!Ctx.RequireString(TEXT("query"), Query)) return true;

    UClass* ParentClass = nullptr;
    const FString ParentClassParam = Ctx.GetString(TEXT("parentClass"));
    if (!ParentClassParam.IsEmpty())
    {
        ParentClass = ResolveUClass(ParentClassParam);
        if (!ParentClass)
        {
            Ctx.SendError(TEXT("CLASS_NOT_FOUND"),
                FString::Printf(TEXT("parentClass '%s' could not be resolved"), *ParentClassParam));
            return true;
        }
    }

    TArray<FString> ModuleFilterLower;
    if (const TArray<TSharedPtr<FJsonValue>>* ModuleFilterArray = Ctx.GetArray(TEXT("moduleFilter")))
    {
        for (const TSharedPtr<FJsonValue>& Val : *ModuleFilterArray)
        {
            if (Val.IsValid() && Val->Type == EJson::String)
            {
                ModuleFilterLower.Add(Val->AsString().ToLower());
            }
        }
    }

    const bool bIncludeAbstract = Ctx.GetBool(TEXT("includeAbstract"), false);
    const bool bIncludeDeprecated = Ctx.GetBool(TEXT("includeDeprecated"), false);
    int32 Limit = Ctx.GetInt(TEXT("limit"), 20);
    if (Limit <= 0) Limit = 20;

    RebuildCacheIfDirty();

    const FString QueryLower = Query.ToLower();
    TArray<FString> Tokens;
    QueryLower.ParseIntoArray(Tokens, TEXT(" "), /*bCullEmpty=*/true);

    struct FScoredEntry
    {
        UClass* Class = nullptr;
        FString ModuleName;
        int32 Score = 0;
    };
    TArray<FScoredEntry> Scored;

    for (const FCachedClassEntry& Entry : FClassSearchCache::Get().Entries)
    {
        UClass* Cls = Entry.Class.Get();
        if (!Cls) continue;

        if (ParentClass && !Cls->IsChildOf(ParentClass)) continue;
        if (!bIncludeAbstract && Cls->HasAnyClassFlags(CLASS_Abstract)) continue;
        if (!bIncludeDeprecated && Cls->HasAnyClassFlags(CLASS_Deprecated)) continue;

        if (ModuleFilterLower.Num() > 0)
        {
            bool bMatchedModule = false;
            for (const FString& Wanted : ModuleFilterLower)
            {
                if (Entry.ModuleLower == Wanted) { bMatchedModule = true; break; }
            }
            if (!bMatchedModule) continue;
        }

        int32 Score = 0;
        for (const FString& Token : Tokens)
        {
            Score += ScoreSubstring(Entry, Token);
        }
        // Whole-query exact-substring bonus, additive on top of per-token score.
        Score += ScoreSubstring(Entry, QueryLower);

        if (Score == 0) continue;

        FScoredEntry SE;
        SE.Class = Cls;
        SE.ModuleName = Entry.ModuleName;
        SE.Score = Score;
        Scored.Add(MoveTemp(SE));
    }

    Scored.Sort([](const FScoredEntry& A, const FScoredEntry& B)
    {
        if (A.Score != B.Score) return A.Score > B.Score;
        return A.Class->GetName() < B.Class->GetName();
    });

    const int32 TotalMatches = Scored.Num();
    if (Scored.Num() > Limit) Scored.SetNum(Limit);

    TArray<TSharedPtr<FJsonValue>> ResultsArray;
    for (const FScoredEntry& SE : Scored)
    {
        UClass* Cls = SE.Class;
        TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
        Row->SetStringField(TEXT("className"), Cls->GetName());
        Row->SetStringField(TEXT("fullPath"), Cls->GetPathName());
        Row->SetStringField(TEXT("parentClass"),
            Cls->GetSuperClass() ? Cls->GetSuperClass()->GetName() : TEXT("None"));
        Row->SetStringField(TEXT("module"), SE.ModuleName);
        Row->SetBoolField(TEXT("isAbstract"), Cls->HasAnyClassFlags(CLASS_Abstract));

        TArray<TSharedPtr<FJsonValue>> Flags;
        // GetBoolMetaData honors explicit meta=(Blueprintable=false); HasMetaData would
        // misreport that as "Blueprintable". Same for BlueprintType.
        if (Cls->GetBoolMetaData(TEXT("Blueprintable")))
            Flags.Add(MakeShared<FJsonValueString>(TEXT("Blueprintable")));
        if (Cls->GetBoolMetaData(TEXT("BlueprintType")))
            Flags.Add(MakeShared<FJsonValueString>(TEXT("BlueprintType")));
        if (Cls->HasAnyClassFlags(CLASS_Abstract))
            Flags.Add(MakeShared<FJsonValueString>(TEXT("Abstract")));
        if (Cls->HasAnyClassFlags(CLASS_Deprecated))
            Flags.Add(MakeShared<FJsonValueString>(TEXT("Deprecated")));
        if (Cls->HasAnyClassFlags(CLASS_Interface))
            Flags.Add(MakeShared<FJsonValueString>(TEXT("Interface")));
        if (Cls->HasAnyClassFlags(CLASS_Native))
            Flags.Add(MakeShared<FJsonValueString>(TEXT("Native")));
        Row->SetArrayField(TEXT("flags"), Flags);

        Row->SetNumberField(TEXT("score"), SE.Score);
        ResultsArray.Add(MakeShared<FJsonValueObject>(Row));
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetArrayField(TEXT("results"), ResultsArray);
    Result->SetNumberField(TEXT("totalMatches"), TotalMatches);
    Ctx.SendSuccess(Result);
    return true;
}
