// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "MGIR/MGIRSubstrateSugar.h"


namespace
{
const FMGIRSubstrateSugarMapping GMGIRSubstrateSugarMappings[] =
{
    { TEXT("slab"), TEXT("/Script/Engine.MaterialExpressionSubstrateSlabBSDF") },
    { TEXT("mix_h"), TEXT("/Script/Engine.MaterialExpressionSubstrateHorizontalMixing") },
    { TEXT("layer"), TEXT("/Script/Engine.MaterialExpressionSubstrateVerticalLayering") },
    { TEXT("mix_v"), TEXT("/Script/Engine.MaterialExpressionSubstrateVerticalLayering") },
    { TEXT("weight"), TEXT("/Script/Engine.MaterialExpressionSubstrateWeight") },
    { TEXT("add"), TEXT("/Script/Engine.MaterialExpressionSubstrateAdd") },
    { TEXT("select"), TEXT("/Script/Engine.MaterialExpressionSubstrateSelect") },
};
}

TConstArrayView<FMGIRSubstrateSugarMapping> FMGIRSubstrateSugar::GetMappings()
{
    return MakeArrayView(GMGIRSubstrateSugarMappings);
}

bool FMGIRSubstrateSugar::TryGetClassPathForAlias(const FString& Alias, FString& OutClassPath)
{
    for (const FMGIRSubstrateSugarMapping& Mapping : GetMappings())
    {
        if (Alias.Equals(Mapping.Alias, ESearchCase::IgnoreCase))
        {
            OutClassPath = Mapping.ClassPath;
            return true;
        }
    }

    return false;
}

bool FMGIRSubstrateSugar::TryGetAliasForClassPath(const FString& ClassPath, FString& OutAlias)
{
    for (const FMGIRSubstrateSugarMapping& Mapping : GetMappings())
    {
        if (ClassPath.Equals(Mapping.ClassPath, ESearchCase::IgnoreCase))
        {
            OutAlias = Mapping.Alias;
            return true;
        }
    }

    return false;
}

bool FMGIRSubstrateSugar::IsSubstrateExpressionClassPath(const FString& ClassPath)
{
    return ClassPath.Contains(TEXT(".MaterialExpressionSubstrate"), ESearchCase::IgnoreCase)
        || ClassPath.Contains(TEXT("/MaterialExpressionSubstrate"), ESearchCase::IgnoreCase);
}
