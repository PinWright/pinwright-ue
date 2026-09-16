// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "PwSource/PwSourcePathUtils.h"

#include "Misc/PackageName.h"
#include "Misc/Paths.h"

namespace PinWrightPwSourcePaths
{
bool TryDeriveOutputAssetPath(
    const FString& ResolvedSourcePath,
    const FString& ExpectedExtension,
    FString& OutAssetPath,
    FString& OutReason)
{
    OutAssetPath.Reset();
    OutReason.Reset();

    FString SourcePath = ResolvedSourcePath;
    FPaths::NormalizeFilename(SourcePath);
    FPaths::CollapseRelativeDirectories(SourcePath);
    if (SourcePath.IsEmpty() || FPaths::IsRelative(SourcePath))
    {
        OutReason = TEXT("the source path did not resolve to one absolute file");
        return false;
    }

    FString ContentDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectContentDir());
    FPaths::NormalizeDirectoryName(ContentDir);
    FPaths::CollapseRelativeDirectories(ContentDir);
    const FString ContentPrefix = ContentDir + TEXT("/");
    if (!SourcePath.StartsWith(ContentPrefix, ESearchCase::IgnoreCase))
    {
        OutReason = FString::Printf(
            TEXT("the source is outside the project Content directory '%s'"), *ContentDir);
        return false;
    }

    const FString ActualExtension = FPaths::GetExtension(SourcePath, /*bIncludeDot=*/true);
    if (!ActualExtension.Equals(ExpectedExtension, ESearchCase::IgnoreCase))
    {
        OutReason = FString::Printf(
            TEXT("the source extension is '%s', not the expected '%s'"),
            ActualExtension.IsEmpty() ? TEXT("<none>") : *ActualExtension,
            *ExpectedExtension);
        return false;
    }

    const FString RelativeSource = SourcePath.Mid(ContentPrefix.Len());
    const FString AssetName = FPaths::GetBaseFilename(RelativeSource);
    if (AssetName.IsEmpty())
    {
        OutReason = TEXT("the source has no basename to use as an asset name");
        return false;
    }

    const FString RelativeDirectory = FPaths::GetPath(RelativeSource);
    FString RelativeAsset = RelativeDirectory.IsEmpty()
        ? AssetName
        : FPaths::Combine(RelativeDirectory, AssetName);
    FPaths::NormalizeFilename(RelativeAsset);
    OutAssetPath = TEXT("/Game/") + RelativeAsset;

    FText InvalidReason;
    if (!FPackageName::IsValidLongPackageName(
            OutAssetPath, /*bIncludeReadOnlyRoots=*/false, &InvalidReason))
    {
        OutReason = FString::Printf(
            TEXT("the beside-source mapping produces invalid asset path '%s': %s"),
            *OutAssetPath, *InvalidReason.ToString());
        OutAssetPath.Reset();
        return false;
    }

    return true;
}
}
