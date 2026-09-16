// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Handlers/ParamSpec.h"

namespace WidgetHandlerUtils
{

inline TArray<FString> MakeWidgetAssetPathParamAliases(const TCHAR* CanonicalName)
{
    TArray<FString> Aliases;
    const FString Canonical(CanonicalName);

    auto AddAlias = [&Aliases, &Canonical](const TCHAR* AliasName)
    {
        if (!Canonical.Equals(AliasName))
        {
            Aliases.Add(AliasName);
        }
    };

    AddAlias(TEXT("assetPath"));
    AddAlias(TEXT("path"));
    AddAlias(TEXT("blueprintPath"));
    AddAlias(TEXT("blueprint_path"));
    AddAlias(TEXT("requestedPath"));
    return Aliases;
}

inline FParamSpec WidgetAssetPathParamReq(const TCHAR* Description)
{
    FParamSpec Spec{TEXT("widgetPath"), TEXT("path"), FString(Description), true, TEXT("")};
    Spec.Aliases = MakeWidgetAssetPathParamAliases(TEXT("widgetPath"));
    return Spec;
}

inline FParamSpec WidgetAssetPathParamOpt(const TCHAR* Description)
{
    FParamSpec Spec{TEXT("widgetPath"), TEXT("path"), FString(Description), false, TEXT("")};
    Spec.Aliases = MakeWidgetAssetPathParamAliases(TEXT("widgetPath"));
    return Spec;
}

inline TArray<FString> WidgetAssetPathParamNames()
{
    TArray<FString> Names;
    Names.Add(TEXT("widgetPath"));
    Names.Append(MakeWidgetAssetPathParamAliases(TEXT("widgetPath")));
    return Names;
}

inline void AppendLegacyWidgetAssetPathSnakeAliases(TArray<FString>& Names)
{
    Names.Add(TEXT("widget_path"));
    Names.Add(TEXT("asset_path"));
}

} // namespace WidgetHandlerUtils
