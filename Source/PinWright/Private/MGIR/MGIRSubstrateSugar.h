// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"


struct FMGIRSubstrateSugarMapping
{
    const TCHAR* Alias;
    const TCHAR* ClassPath;
};

class FMGIRSubstrateSugar
{
public:
    static bool TryGetClassPathForAlias(const FString& Alias, FString& OutClassPath);
    static bool TryGetAliasForClassPath(const FString& ClassPath, FString& OutAlias);
    static bool IsSubstrateExpressionClassPath(const FString& ClassPath);
    static TConstArrayView<FMGIRSubstrateSugarMapping> GetMappings();
};
