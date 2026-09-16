// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once
#include "CoreMinimal.h"

class UFunction;
class UClass;

class PINWRIGHT_API FCodeFunctionResolver
{
public:
    FCodeFunctionResolver();
    UFunction* ResolveFunction(UClass* TargetClass, const FString& FuncName);
    UFunction* ResolveLibraryFunction(const FString& ClassName, const FString& FuncName);
    UFunction* ResolveFunctionAcrossLibraries(const FString& FuncName);
    FString PreProcessKnownConversions(const FString& Expression);
    FString GetSearchDiagnostic(UClass* BPClass) const;
    UFunction* ResolveFunctionBroadSearch(const FString& FuncName);
    const TMap<FString, UClass*>& GetKnownLibraries() const { return KnownLibraries; }

private:
    /** Returns true if Class is non-null, not pending kill, and not a stale hot-reload remnant. */
    static bool IsClassSafe(const UClass* Class);

    TMap<FString, UClass*> KnownLibraries;
    TMap<FString, FString> FunctionAliases;
    TMap<FString, UFunction*> BroadFunctionCache;
    bool bBroadCacheBuilt = false;
};
