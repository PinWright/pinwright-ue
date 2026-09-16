// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Compiler/CodeFunctionResolver.h"
#include "State/PluginState.h"
#include "UObject/UObjectIterator.h"
#include "UObject/UnrealType.h"

#include "Kismet/KismetMathLibrary.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Kismet/KismetStringLibrary.h"
#include "Kismet/KismetTextLibrary.h"
#include "Kismet/GameplayStatics.h"
#include "Kismet/KismetRenderingLibrary.h"
#include "Kismet/KismetArrayLibrary.h"
#include "Blueprint/WidgetBlueprintLibrary.h"

DEFINE_LOG_CATEGORY_STATIC(LogCodeFunctionResolver, Log, All);

bool FCodeFunctionResolver::IsClassSafe(const UClass* Class)
{
    return IsValid(Class) && !Class->HasAnyClassFlags(CLASS_NewerVersionExists);
}

FCodeFunctionResolver::FCodeFunctionResolver()
{
    KnownLibraries.Add(TEXT("UKismetMathLibrary"),      UKismetMathLibrary::StaticClass());
    KnownLibraries.Add(TEXT("UKismetSystemLibrary"),    UKismetSystemLibrary::StaticClass());
    KnownLibraries.Add(TEXT("UKismetStringLibrary"),    UKismetStringLibrary::StaticClass());
    KnownLibraries.Add(TEXT("UKismetTextLibrary"),      UKismetTextLibrary::StaticClass());
    KnownLibraries.Add(TEXT("UGameplayStatics"),        UGameplayStatics::StaticClass());
    KnownLibraries.Add(TEXT("UKismetRenderingLibrary"), UKismetRenderingLibrary::StaticClass());
    KnownLibraries.Add(TEXT("UKismetArrayLibrary"),     UKismetArrayLibrary::StaticClass());
    KnownLibraries.Add(TEXT("UWidgetBlueprintLibrary"), UWidgetBlueprintLibrary::StaticClass());

    // UE5 float→double migration renames and other common function aliases
    FunctionAliases.Add(TEXT("MaxOfFloat"),              TEXT("Max"));
    FunctionAliases.Add(TEXT("MinOfFloat"),              TEXT("Min"));
    FunctionAliases.Add(TEXT("Add_FloatFloat"),          TEXT("Add_DoubleDouble"));
    FunctionAliases.Add(TEXT("NotEqual_FloatFloat"),     TEXT("NotEqual_DoubleDouble"));
    FunctionAliases.Add(TEXT("Multiply_FloatFloat"),     TEXT("Multiply_DoubleDouble"));
    FunctionAliases.Add(TEXT("Div_FloatFloat"),          TEXT("Divide_DoubleDouble"));
    FunctionAliases.Add(TEXT("Greater_FloatFloat"),      TEXT("Greater_DoubleDouble"));
    FunctionAliases.Add(TEXT("Less_FloatFloat"),         TEXT("Less_DoubleDouble"));
    FunctionAliases.Add(TEXT("Subtract_FloatFloat"),     TEXT("Subtract_DoubleDouble"));
    FunctionAliases.Add(TEXT("Conv_FloatToString"),      TEXT("Conv_DoubleToString"));
    FunctionAliases.Add(TEXT("LessEqual_FloatFloat"),    TEXT("LessEqual_DoubleDouble"));
    FunctionAliases.Add(TEXT("GreaterEqual_FloatFloat"), TEXT("GreaterEqual_DoubleDouble"));
    FunctionAliases.Add(TEXT("Div_IntInt"),              TEXT("Divide_IntInt"));
    FunctionAliases.Add(TEXT("Not_Bool"),                TEXT("Not_PreBool"));
    FunctionAliases.Add(TEXT("VectorLength"),            TEXT("VSize"));
    FunctionAliases.Add(TEXT("GetName"),                 TEXT("GetObjectName"));
    FunctionAliases.Add(TEXT("GetActorLabel"),           TEXT("GetObjectName"));
    FunctionAliases.Add(TEXT("K2_GetActorLabel"),        TEXT("GetObjectName"));
    FunctionAliases.Add(TEXT("DestroyActor"),            TEXT("K2_DestroyActor"));
    FunctionAliases.Add(TEXT("SetActorLocation"),        TEXT("K2_SetActorLocation"));
    FunctionAliases.Add(TEXT("SetActorRotation"),        TEXT("K2_SetActorRotation"));
    FunctionAliases.Add(TEXT("Normalize"),               TEXT("Normal"));
    FunctionAliases.Add(TEXT("SelectBool"),              TEXT("Select"));

    // Blueprint display-name to C++ function name aliases for common trace functions
    FunctionAliases.Add(TEXT("LineTraceByChannel"),         TEXT("LineTraceSingle"));
    FunctionAliases.Add(TEXT("MultiLineTraceByChannel"),    TEXT("LineTraceMulti"));
    FunctionAliases.Add(TEXT("LineTraceForObjects"),        TEXT("LineTraceSingleForObjects"));
    FunctionAliases.Add(TEXT("MultiLineTraceForObjects"),   TEXT("LineTraceMultiForObjects"));
    FunctionAliases.Add(TEXT("SphereTraceByChannel"),       TEXT("SphereTraceSingle"));
    FunctionAliases.Add(TEXT("BoxTraceByChannel"),          TEXT("BoxTraceSingle"));
    FunctionAliases.Add(TEXT("CapsuleTraceByChannel"),      TEXT("CapsuleTraceSingle"));

    // Merge all loaded function libraries from the session-wide cache.
    // Hardcoded entries above take priority; only fill in gaps.
    for (const TPair<FString, UClass*>& Pair : FPluginState::Get().GetScannedFunctionLibraries())
    {
        if (!KnownLibraries.Contains(Pair.Key) && IsClassSafe(Pair.Value))
        {
            KnownLibraries.Add(Pair.Key, Pair.Value);
        }
    }
}

UFunction* FCodeFunctionResolver::ResolveFunction(UClass* TargetClass, const FString& FuncName)
{
    if (!IsClassSafe(TargetClass))
    {
        if (IsValid(TargetClass))
        {
            // Class is alive but stale (CLASS_NewerVersionExists) — safe to dereference for logging
            UE_LOG(LogCodeFunctionResolver, Warning,
                TEXT("ResolveFunction: class '%s' has NewerVersionExists, skipping lookup for '%s'"),
                *TargetClass->GetName(), *FuncName);
        }
        return nullptr;
    }

    // 1. Exact match
    UFunction* Found = TargetClass->FindFunctionByName(*FuncName);
    if (Found)
    {
        return Found;
    }

    // 2. Alias lookup
    const FString* Alias = FunctionAliases.Find(FuncName);
    if (Alias)
    {
        Found = TargetClass->FindFunctionByName(**Alias);
        if (Found)
        {
            UE_LOG(LogCodeFunctionResolver, Verbose, TEXT("Resolved '%s' via alias to '%s'"), *FuncName, **Alias);
            return Found;
        }
    }

    // 3. Space-stripped case-insensitive fallback — handles BP functions whose name contains
    //    spaces (e.g. "Set Error") when called as "SetError", "Set_Error", or backtick-quoted.
    //    Both sides are stripped of ASCII spaces before comparison.
    {
        FString NeedleStripped = FuncName.Replace(TEXT(" "), TEXT("")).ToLower();
        TArray<UFunction*> Candidates;
        for (TFieldIterator<UFunction> It(TargetClass, EFieldIteratorFlags::IncludeSuper); It; ++It)
        {
            FString CandidateStripped = It->GetName().Replace(TEXT(" "), TEXT("")).ToLower();
            if (CandidateStripped == NeedleStripped)
            {
                Candidates.Add(*It);
            }
        }
        if (Candidates.Num() == 1)
        {
            UE_LOG(LogCodeFunctionResolver, Verbose,
                TEXT("Resolved '%s' via space-stripped fallback to '%s'"),
                *FuncName, *Candidates[0]->GetName());
            return Candidates[0];
        }
        else if (Candidates.Num() > 1)
        {
            TArray<FString> CandidateNames;
            for (UFunction* Fn : Candidates)
            {
                CandidateNames.Add(Fn->GetName());
            }
            UE_LOG(LogCodeFunctionResolver, Warning,
                TEXT("Ambiguous function name '%s' — multiple matches after space-stripping: [%s]. Use the exact backtick-quoted name."),
                *FuncName, *FString::Join(CandidateNames, TEXT(", ")));
            return nullptr;
        }
    }

    // 4. DisplayName/ScriptName meta fallback — handles BP-editor-visible names that differ
    //    from the internal FName by more than spacing (e.g. "GetComponentsByClass" whose internal
    //    name is "K2_GetComponentsByClass" but carries meta=(ScriptName="GetComponentsByClass")).
    //    GetName() never exposes meta, so the space-stripped pass above can't reach these. Both the
    //    needle and each candidate's DisplayName/ScriptName meta are space-stripped and lowercased.
    {
        FString NeedleStripped = FuncName.Replace(TEXT(" "), TEXT("")).ToLower();
        TArray<UFunction*> Candidates;
        for (TFieldIterator<UFunction> It(TargetClass, EFieldIteratorFlags::IncludeSuper); It; ++It)
        {
            const FString DisplayName = It->GetMetaData(TEXT("DisplayName")).Replace(TEXT(" "), TEXT("")).ToLower();
            const FString ScriptName  = It->GetMetaData(TEXT("ScriptName")).Replace(TEXT(" "), TEXT("")).ToLower();
            if ((!DisplayName.IsEmpty() && DisplayName == NeedleStripped)
                || (!ScriptName.IsEmpty() && ScriptName == NeedleStripped))
            {
                Candidates.Add(*It);
            }
        }
        if (Candidates.Num() == 1)
        {
            UE_LOG(LogCodeFunctionResolver, Verbose,
                TEXT("Resolved '%s' via DisplayName/ScriptName meta to '%s'"),
                *FuncName, *Candidates[0]->GetName());
            return Candidates[0];
        }
        else if (Candidates.Num() > 1)
        {
            TArray<FString> CandidateNames;
            for (UFunction* Fn : Candidates)
            {
                CandidateNames.Add(Fn->GetName());
            }
            UE_LOG(LogCodeFunctionResolver, Warning,
                TEXT("Ambiguous function name '%s' — multiple matches via DisplayName/ScriptName meta: [%s]. Use the exact backtick-quoted name."),
                *FuncName, *FString::Join(CandidateNames, TEXT(", ")));
            return nullptr;
        }
    }

    return nullptr;
}

UFunction* FCodeFunctionResolver::ResolveLibraryFunction(const FString& ClassName, const FString& FuncName)
{
    UClass* const* LibClass = KnownLibraries.Find(ClassName);
    if (!LibClass)
    {
        UE_LOG(LogCodeFunctionResolver, Warning, TEXT("Unknown library class '%s'"), *ClassName);
        return nullptr;
    }
    return ResolveFunction(*LibClass, FuncName);
}

UFunction* FCodeFunctionResolver::ResolveFunctionAcrossLibraries(const FString& FuncName)
{
    for (const TPair<FString, UClass*>& Entry : KnownLibraries)
    {
        UFunction* Found = ResolveFunction(Entry.Value, FuncName);
        if (Found)
        {
            UE_LOG(LogCodeFunctionResolver, Verbose,
                TEXT("'%s' found in library '%s'"), *FuncName, *Entry.Key);
            return Found;
        }
    }
    return nullptr;
}


FString FCodeFunctionResolver::PreProcessKnownConversions(const FString& Expression)
{
    FString Result = Expression;
    Result = Result.Replace(TEXT("FString::FromInt"),    TEXT("UKismetStringLibrary::Conv_IntToString"));
    Result = Result.Replace(TEXT("FText::FromString"),   TEXT("UKismetTextLibrary::Conv_StringToText"));
    return Result;
}

FString FCodeFunctionResolver::GetSearchDiagnostic(UClass* BPClass) const
{
    FString Diag;
    if (BPClass)
    {
        Diag += FString::Printf(TEXT("  - Blueprint class: %s\n"), *BPClass->GetName());
    }
    int32 Count = 0;
    for (const TPair<FString, UClass*>& Entry : KnownLibraries)
    {
        if (Count < 8)
        {
            Diag += FString::Printf(TEXT("  - %s\n"), *Entry.Key);
            Count++;
        }
    }
    if (KnownLibraries.Num() > 8)
    {
        Diag += FString::Printf(TEXT("  - ... and %d more libraries\n"), KnownLibraries.Num() - 8);
    }
    if (bBroadCacheBuilt)
    {
        Diag += FString::Printf(TEXT("  - Broad search: %d static functions from non-library classes\n"), BroadFunctionCache.Num());
    }
    return Diag;
}

UFunction* FCodeFunctionResolver::ResolveFunctionBroadSearch(const FString& FuncName)
{
    if (!bBroadCacheBuilt)
    {
        for (TObjectIterator<UClass> It; It; ++It)
        {
            UClass* Class = *It;
            if (!IsValid(Class))
            {
                continue;
            }
            if (Class->HasAnyClassFlags(CLASS_Abstract | CLASS_NewerVersionExists))
            {
                continue;
            }
            if (Class->IsChildOf(UBlueprintFunctionLibrary::StaticClass()))
            {
                continue;
            }
            for (TFieldIterator<UFunction> FuncIt(Class, EFieldIteratorFlags::ExcludeSuper); FuncIt; ++FuncIt)
            {
                UFunction* Func = *FuncIt;
                if (Func->HasAllFunctionFlags(FUNC_Static | FUNC_BlueprintCallable)
                    && !BroadFunctionCache.Contains(Func->GetName()))
                {
                    BroadFunctionCache.Add(Func->GetName(), Func);
                }
            }
        }
        bBroadCacheBuilt = true;
        UE_LOG(LogCodeFunctionResolver, Verbose, TEXT("Broad function cache built: %d entries"), BroadFunctionCache.Num());
    }

    if (UFunction** Found = BroadFunctionCache.Find(FuncName))
    {
        if (IsValid(*Found))
        {
            return *Found;
        }
        UE_LOG(LogCodeFunctionResolver, Warning,
            TEXT("ResolveFunctionBroadSearch: cached UFunction '%s' is stale, evicting"), *FuncName);
        BroadFunctionCache.Remove(FuncName);
    }

    const FString* Alias = FunctionAliases.Find(FuncName);
    if (Alias)
    {
        if (UFunction** Found = BroadFunctionCache.Find(*Alias))
        {
            if (IsValid(*Found))
            {
                return *Found;
            }
            UE_LOG(LogCodeFunctionResolver, Warning,
                TEXT("ResolveFunctionBroadSearch: cached UFunction '%s' (alias of '%s') is stale, evicting"), **Alias, *FuncName);
            BroadFunctionCache.Remove(*Alias);
        }
    }

    return nullptr;
}
