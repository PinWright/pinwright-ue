// Copyright (c) 2026 Alexander Penkin. MIT License.

// BlueprintApiIndexHandler.cpp
// blueprint.build_api_index — walk all UClasses via reflection and emit Saved/AI/ApiIndex.json
// blueprint.search_api       — keyword-score search over the cached ApiIndex.json

#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/ParamSpec.h"

#include "Async/Async.h"
#include "UObject/UObjectIterator.h"
#include "UObject/UnrealType.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonSerializer.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Utils/SortedJsonWriter.h"
#include "Utils/PropertyInspection.h"

namespace { static bool bApiIndexDirty = false; }

// ============================================================================
// blueprint.build_api_index
// ============================================================================

REGISTER_RPC_HANDLER("blueprint.build_api_index", "blueprint",
    "Scan all UClasses via reflection and write a Blueprint-callable API index to Saved/AI/ApiIndex.json",
    RPC_PARAMS(
        RPC_PARAM_OPT("classFilter", "array", "Limit scan to specific class names; omit to scan all classes")
    ))
{
    // Build optional class-name filter set before entering the async job
    TSet<FString> FilterSet;
    bool bHasFilter = false;
    if (const TArray<TSharedPtr<FJsonValue>>* FilterArray = Ctx.GetArray(TEXT("classFilter")))
    {
        for (const TSharedPtr<FJsonValue>& Val : *FilterArray)
        {
            if (Val.IsValid() && Val->Type == EJson::String)
            {
                FString Name = Val->AsString();
                FilterSet.Add(Name);
                // Also add with common UE prefix stripped (AActor -> Actor, UActorComponent -> ActorComponent)
                if (Name.Len() > 1)
                {
                    TCHAR Prefix = Name[0];
                    if ((Prefix == TEXT('A') || Prefix == TEXT('U') || Prefix == TEXT('F')) && FChar::IsUpper(Name[1]))
                    {
                        FilterSet.Add(Name.Mid(1));
                    }
                }
            }
        }
        bHasFilter = !FilterSet.IsEmpty();
    }

    FJobBindArgs Args;
    Args.Method = TEXT("blueprint.build_api_index");
    Args.StartedPayload = MakeShared<FJsonObject>();

    Args.BindNativeDelegate =
        [FilterSet = MoveTemp(FilterSet), bHasFilter](FJobOnComplete OnComplete) mutable
    {
        AsyncTask(ENamedThreads::GameThread, [FilterSet = MoveTemp(FilterSet), bHasFilter, OnComplete]() mutable
        {
            TSharedPtr<FJsonObject> IndexRoot = MakeShared<FJsonObject>();
            TArray<TSharedPtr<FJsonValue>> ClassesArray;
            int32 ClassCount = 0;
            int32 FunctionCount = 0;

            for (TObjectIterator<UClass> ClassIt; ClassIt; ++ClassIt)
            {
                UClass* Class = *ClassIt;
                if (!Class || Class->HasAnyClassFlags(CLASS_Deprecated | CLASS_NewerVersionExists))
                    continue;

                if (bHasFilter && !FilterSet.Contains(Class->GetName()))
                    continue;

                TArray<TSharedPtr<FJsonValue>> FunctionsArray;

                for (TFieldIterator<UFunction> FuncIt(Class, EFieldIteratorFlags::ExcludeSuper); FuncIt; ++FuncIt)
                {
                    UFunction* Func = *FuncIt;
                    if (!Func || !Func->HasAnyFunctionFlags(FUNC_BlueprintCallable))
                        continue;

                    FString ReturnType = TEXT("void");
                    TArray<TSharedPtr<FJsonValue>> ParamsArray;
                    FString SigParams;

                    for (TFieldIterator<FProperty> ParamIt(Func); ParamIt; ++ParamIt)
                    {
                        FProperty* Param = *ParamIt;
                        if (!Param->HasAnyPropertyFlags(CPF_Parm))
                            continue;

                        if (Param->HasAnyPropertyFlags(CPF_ReturnParm))
                        {
                            ReturnType = GetPropertyCppTypeWithParams(Param);
                        }
                        else
                        {
                            TSharedPtr<FJsonObject> ParamObj = MakeShared<FJsonObject>();
                            ParamObj->SetStringField(TEXT("name"), Param->GetName());
                            ParamObj->SetStringField(TEXT("type"), GetPropertyCppTypeWithParams(Param));
                            // CPF_ReferenceParm implies CPF_OutParm even for `const T&`, so const refs
                            // (efficient pass-by-ref inputs) are NOT outputs. Mirrors UE's canonical
                            // classification (EdGraphSchema_K2.cpp / KismetCompiler.cpp).
                            ParamObj->SetBoolField(TEXT("isOutput"),
                                Param->HasAnyPropertyFlags(CPF_OutParm)
                                && !Param->HasAnyPropertyFlags(CPF_ReturnParm)
                                && !Param->HasAnyPropertyFlags(CPF_ConstParm));
                            ParamsArray.Add(MakeShared<FJsonValueObject>(ParamObj));

                            if (!SigParams.IsEmpty())
                                SigParams += TEXT(", ");
                            SigParams += GetPropertyCppTypeWithParams(Param) + TEXT(" ") + Param->GetName();
                        }
                    }

                    FString Signature = FString::Printf(TEXT("%s %s::%s(%s)"),
                        *ReturnType, *Class->GetName(), *Func->GetName(), *SigParams);

                    TSharedPtr<FJsonObject> FuncObj = MakeShared<FJsonObject>();
                    FuncObj->SetStringField(TEXT("name"), Func->GetName());
                    FuncObj->SetStringField(TEXT("returnType"), ReturnType);
                    FuncObj->SetStringField(TEXT("signature"), Signature);
                    FuncObj->SetArrayField(TEXT("parameters"), ParamsArray);
                    FunctionsArray.Add(MakeShared<FJsonValueObject>(FuncObj));
                    ++FunctionCount;
                }

                if (FunctionsArray.Num() > 0)
                {
                    TSharedPtr<FJsonObject> ClassObj = MakeShared<FJsonObject>();
                    ClassObj->SetStringField(TEXT("className"), Class->GetName());
                    ClassObj->SetArrayField(TEXT("functions"), FunctionsArray);
                    ClassesArray.Add(MakeShared<FJsonValueObject>(ClassObj));
                    ++ClassCount;
                }
            }

            // Write to Saved/AI/ApiIndex.json
            FString SaveDir = FPaths::ProjectSavedDir() / TEXT("AI");
            IFileManager::Get().MakeDirectory(*SaveDir, /*Tree=*/true);
            FString IndexPath = SaveDir / TEXT("ApiIndex.json");

            // Build set of class names from this scan for dedup during merge
            TSet<FString> NewClassNames;
            for (const auto& ClassVal : ClassesArray)
            {
                auto ClassObj = ClassVal->AsObject();
                if (ClassObj.IsValid())
                {
                    FString CN;
                    ClassObj->TryGetStringField(TEXT("className"), CN);
                    NewClassNames.Add(CN);
                }
            }

            // Load existing index and merge — retain classes not covered by this scan
            TArray<TSharedPtr<FJsonValue>> MergedClasses;
            int32 MergedFunctionCount = FunctionCount;
            {
                FString ExistingJson;
                if (FFileHelper::LoadFileToString(ExistingJson, *IndexPath))
                {
                    TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ExistingJson);
                    TSharedPtr<FJsonObject> ExistingIndex;
                    if (FJsonSerializer::Deserialize(Reader, ExistingIndex) && ExistingIndex.IsValid())
                    {
                        const TArray<TSharedPtr<FJsonValue>>* ExistingClasses = nullptr;
                        if (ExistingIndex->TryGetArrayField(TEXT("classes"), ExistingClasses) && ExistingClasses)
                        {
                            for (const auto& Val : *ExistingClasses)
                            {
                                auto Obj = Val->AsObject();
                                if (Obj.IsValid())
                                {
                                    FString CN;
                                    Obj->TryGetStringField(TEXT("className"), CN);
                                    if (!NewClassNames.Contains(CN))
                                    {
                                        MergedClasses.Add(Val);
                                        // Count retained functions toward the total
                                        const TArray<TSharedPtr<FJsonValue>>* Funcs = nullptr;
                                        if (Obj->TryGetArrayField(TEXT("functions"), Funcs) && Funcs)
                                            MergedFunctionCount += Funcs->Num();
                                    }
                                }
                            }
                        }
                    }
                }
            }

            // Append newly scanned classes
            for (const auto& NewClass : ClassesArray)
                MergedClasses.Add(NewClass);

            IndexRoot->SetArrayField(TEXT("classes"), MergedClasses);

            const FString JsonString = SortedJsonWriter::SerializeSortedJsonObject(IndexRoot);

            if (!FFileHelper::SaveStringToFile(JsonString, *IndexPath))
            {
                OnComplete(false, nullptr, TEXT("WRITE_FAILED"));
                return;
            }

            bApiIndexDirty = true;

            TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
            Result->SetStringField(TEXT("message"), TEXT("API index built successfully"));
            Result->SetNumberField(TEXT("classCount"), ClassCount);
            Result->SetNumberField(TEXT("functionCount"), FunctionCount);
            Result->SetNumberField(TEXT("totalClasses"), MergedClasses.Num());
            Result->SetNumberField(TEXT("totalFunctions"), MergedFunctionCount);
            Result->SetStringField(TEXT("indexPath"), IndexPath);
            OnComplete(true, Result, FString());
        });
    };

    Ctx.StartJob(Args);
    return true;
}

// ============================================================================
// blueprint.search_api
// ============================================================================

REGISTER_RPC_HANDLER("blueprint.search_api", "blueprint",
    "Keyword search over the API index built by blueprint.build_api_index",
    RPC_PARAMS(
        RPC_PARAM_REQ("query", "string", "Space-separated keywords, e.g. \"get actor location\""),
        RPC_PARAM_DEF("limit", "number", "Maximum number of results to return", "5")
    ))
{
    FString Query;
    if (!Ctx.RequireString(TEXT("query"), Query))
        return true;

    int32 Limit = Ctx.GetInt(TEXT("limit"), 5);
    if (Limit <= 0)
        Limit = 5;

    // File-scoped cache — reloaded whenever the path changes (i.e. after a rebuild)
    static TSharedPtr<FJsonObject> CachedApiIndex;
    static FString CachedIndexPath;

    FString IndexPath = FPaths::ProjectSavedDir() / TEXT("AI") / TEXT("ApiIndex.json");
    if (!CachedApiIndex.IsValid() || CachedIndexPath != IndexPath || bApiIndexDirty)
    {
        bApiIndexDirty = false;
        FString JsonString;
        if (!FFileHelper::LoadFileToString(JsonString, *IndexPath))
        {
            Ctx.SendError(TEXT("INDEX_NOT_FOUND"),
                TEXT("API index not found. Run blueprint.build_api_index first."));
            return true;
        }
        TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonString);
        TSharedPtr<FJsonObject> Parsed;
        if (!FJsonSerializer::Deserialize(Reader, Parsed) || !Parsed.IsValid())
        {
            Ctx.SendError(TEXT("INDEX_PARSE_ERROR"), TEXT("Failed to parse ApiIndex.json"));
            return true;
        }
        CachedApiIndex = Parsed;
        CachedIndexPath = IndexPath;
    }

    // Tokenize query (lowercase)
    TArray<FString> Tokens;
    Query.ToLower().ParseIntoArray(Tokens, TEXT(" "), /*bCullEmpty=*/true);

    struct FScoredResult
    {
        FString ClassName;
        FString FunctionName;
        FString Signature;
        FString ReturnType;
        TArray<TSharedPtr<FJsonValue>> Parameters;
        int32 Score = 0;
    };

    TArray<FScoredResult> ScoredResults;

    const TArray<TSharedPtr<FJsonValue>>* ClassesPtr = nullptr;
    if (!CachedApiIndex->TryGetArrayField(TEXT("classes"), ClassesPtr) || !ClassesPtr)
    {
        Ctx.SendError(TEXT("INDEX_MALFORMED"), TEXT("ApiIndex.json is missing 'classes' array"));
        return true;
    }

    for (const TSharedPtr<FJsonValue>& ClassVal : *ClassesPtr)
    {
        const TSharedPtr<FJsonObject> ClassObj = ClassVal->AsObject();
        if (!ClassObj.IsValid())
            continue;

        FString ClassName;
        ClassObj->TryGetStringField(TEXT("className"), ClassName);
        FString ClassNameLower = ClassName.ToLower();

        const TArray<TSharedPtr<FJsonValue>>* FuncsPtr = nullptr;
        if (!ClassObj->TryGetArrayField(TEXT("functions"), FuncsPtr) || !FuncsPtr)
            continue;

        for (const TSharedPtr<FJsonValue>& FuncVal : *FuncsPtr)
        {
            const TSharedPtr<FJsonObject> FuncObj = FuncVal->AsObject();
            if (!FuncObj.IsValid())
                continue;

            FString FuncName;
            FuncObj->TryGetStringField(TEXT("name"), FuncName);
            FString FuncNameLower = FuncName.ToLower();

            FString ReturnType;
            FuncObj->TryGetStringField(TEXT("returnType"), ReturnType);

            FString Signature;
            FuncObj->TryGetStringField(TEXT("signature"), Signature);

            TArray<TSharedPtr<FJsonValue>> Params;
            if (const TArray<TSharedPtr<FJsonValue>>* ParamsPtr = nullptr;
                FuncObj->TryGetArrayField(TEXT("parameters"), ParamsPtr) && ParamsPtr)
            {
                Params = *ParamsPtr;
            }

            int32 Score = 0;
            for (const FString& Token : Tokens)
            {
                // +3 for function name match
                if (FuncNameLower.Contains(Token))
                    Score += 3;

                // +2 for class name match
                if (ClassNameLower.Contains(Token))
                    Score += 2;

                // +1 for parameter name or type match
                for (const TSharedPtr<FJsonValue>& ParamVal : Params)
                {
                    const TSharedPtr<FJsonObject> ParamObj = ParamVal->AsObject();
                    if (!ParamObj.IsValid())
                        continue;
                    FString ParamName, ParamType;
                    ParamObj->TryGetStringField(TEXT("name"), ParamName);
                    ParamObj->TryGetStringField(TEXT("type"), ParamType);
                    if (ParamName.ToLower().Contains(Token) || ParamType.ToLower().Contains(Token))
                    {
                        Score += 1;
                        break; // Count each param set once per token
                    }
                }
            }

            if (Score > 0)
            {
                FScoredResult Entry;
                Entry.ClassName = ClassName;
                Entry.FunctionName = FuncName;
                Entry.Signature = Signature;
                Entry.ReturnType = ReturnType;
                Entry.Parameters = Params;
                Entry.Score = Score;
                ScoredResults.Add(MoveTemp(Entry));
            }
        }
    }

    // Sort descending by score
    ScoredResults.Sort([](const FScoredResult& A, const FScoredResult& B)
    {
        return A.Score > B.Score;
    });

    int32 TotalMatches = ScoredResults.Num();
    if (ScoredResults.Num() > Limit)
        ScoredResults.SetNum(Limit);

    TArray<TSharedPtr<FJsonValue>> ResultsArray;
    for (const FScoredResult& Entry : ScoredResults)
    {
        TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
        Item->SetStringField(TEXT("className"), Entry.ClassName);
        Item->SetStringField(TEXT("functionName"), Entry.FunctionName);
        Item->SetStringField(TEXT("signature"), Entry.Signature);
        Item->SetStringField(TEXT("returnType"), Entry.ReturnType);
        Item->SetArrayField(TEXT("parameters"), Entry.Parameters);
        Item->SetNumberField(TEXT("score"), Entry.Score);
        ResultsArray.Add(MakeShared<FJsonValueObject>(Item));
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetArrayField(TEXT("results"), ResultsArray);
    Result->SetNumberField(TEXT("totalMatches"), TotalMatches);
    Ctx.SendSuccess(Result);
    return true;
}
