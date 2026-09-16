// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "PinWrightPackageLibrary.h"

#include "GameFramework/Actor.h"
#include "UObject/Package.h"
#include "Utils/PackageDirtyUtils.h"

// Thin reflected skin over PinWright::PackageDirty. All policy, guards and the
// read-back verification live in Utils/PackageDirtyUtils.cpp so the RPC verbs
// (asset.mark_dirty / asset.is_dirty) and this Python surface cannot drift apart.

bool UPinWrightPackageLibrary::MarkPackageDirty(UObject* Object)
{
    return PinWright::PackageDirty::MarkObjectDirty(Object).IsDirtyNow();
}

bool UPinWrightPackageLibrary::MarkActorPackageDirty(AActor* Actor)
{
    return PinWright::PackageDirty::MarkObjectDirty(Actor).IsDirtyNow();
}

bool UPinWrightPackageLibrary::MarkPackageDirtyByPath(const FString& AssetPath)
{
    return PinWright::PackageDirty::MarkPathDirty(AssetPath).IsDirtyNow();
}

bool UPinWrightPackageLibrary::IsPackageDirty(UObject* Object)
{
    return PinWright::PackageDirty::IsObjectPackageDirty(Object);
}

bool UPinWrightPackageLibrary::IsPackageDirtyByPath(const FString& AssetPath)
{
    FString Reason;
    const UPackage* Package = PinWright::PackageDirty::FindLoadedPackageForPath(AssetPath, Reason);
    return Package && Package->IsDirty();
}

FString UPinWrightPackageLibrary::DescribeMarkDirtyBlocker(UObject* Object)
{
    return PinWright::PackageDirty::DescribeBlocker(Object);
}

FString UPinWrightPackageLibrary::GetPackageName(UObject* Object)
{
    return PinWright::PackageDirty::GetPackageName(Object);
}
