// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once


#include "CoreMinimal.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Misc/PackageName.h"
#include "Templates/Function.h"
#include "UObject/Package.h"

namespace IrTest
{
struct FScratchAsset
{
    FString PackagePath;
    FString AssetName;

    explicit FScratchAsset(const TCHAR* Prefix);
    ~FScratchAsset();

    FScratchAsset(const FScratchAsset&) = delete;
    FScratchAsset& operator=(const FScratchAsset&) = delete;
};

FString ReplaceScratchAssetName(
    const FString& InputText,
    const FScratchAsset& Source,
    const FScratchAsset& Target);

inline bool ErrorMessageContains(const FString& Error, const FString& Substring)
{
    return Error.Contains(Substring, ESearchCase::IgnoreCase);
}

template <typename TDiagnostic>
bool ErrorMessageContains(const TDiagnostic& Error, const FString& Substring)
{
    return Error.Message.Contains(Substring, ESearchCase::IgnoreCase);
}

template <typename TError>
bool ErrorsContain(const TArray<TError>& Errors, const FString& Substring)
{
    for (const TError& Error : Errors)
    {
        if (ErrorMessageContains(Error, Substring))
        {
            return true;
        }
    }
    return false;
}

template <typename TAsset, typename TFactory>
TAsset* CreateFactoryAssetAtPath(
    const FString& PackagePath,
    TFunctionRef<void(TFactory&)> ConfigureFactory)
{
    if (PackagePath.IsEmpty())
    {
        return nullptr;
    }

    UPackage* Package = CreatePackage(*PackagePath);
    if (!Package)
    {
        return nullptr;
    }

    const FString AssetName = FPackageName::GetLongPackageAssetName(PackagePath);
    if (AssetName.IsEmpty())
    {
        return nullptr;
    }

    TFactory* Factory = NewObject<TFactory>();
    if (!Factory)
    {
        return nullptr;
    }

    ConfigureFactory(*Factory);

    TAsset* Asset = Cast<TAsset>(Factory->FactoryCreateNew(
        TAsset::StaticClass(),
        Package,
        FName(*AssetName),
        RF_Public | RF_Standalone,
        nullptr,
        GWarn));
    if (Asset)
    {
        FAssetRegistryModule::AssetCreated(Asset);
    }
    return Asset;
}

template <typename TAsset, typename TFactory>
TAsset* CreateFactoryAssetAtPath(const FString& PackagePath)
{
    return CreateFactoryAssetAtPath<TAsset, TFactory>(
        PackagePath,
        [](TFactory&) {});
}

template <typename TAsset, typename TFactory>
TAsset* CreateFactoryScratchAsset(
    const FScratchAsset& Scratch,
    TFunctionRef<void(TFactory&)> ConfigureFactory)
{
    return CreateFactoryAssetAtPath<TAsset, TFactory>(
        Scratch.PackagePath,
        ConfigureFactory);
}

template <typename TAsset, typename TFactory>
TAsset* CreateFactoryScratchAsset(const FScratchAsset& Scratch)
{
    return CreateFactoryAssetAtPath<TAsset, TFactory>(Scratch.PackagePath);
}
} // namespace IrTest
