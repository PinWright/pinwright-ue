// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "RecorderResolver.h"

#include "Misc/Paths.h"
#include "HAL/FileManager.h"

namespace RecorderResolver
{

FString RecordingsDir()
{
    return FPaths::ConvertRelativePathToFull(
        FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("PinWright/Recordings")));
}

FString ResolvePath(const FString& Session)
{
    if (Session.IsEmpty())
    {
        return FString();
    }

    if (FPaths::FileExists(Session))
    {
        return FPaths::ConvertRelativePathToFull(Session);
    }

    const FString Dir = RecordingsDir();
    const FString Direct = FPaths::Combine(Dir, Session);
    if (FPaths::FileExists(Direct))
    {
        return Direct;
    }

    const FString WithExt = Direct.EndsWith(TEXT(".ndjson"), ESearchCase::IgnoreCase)
        ? Direct
        : Direct + TEXT(".ndjson");
    if (FPaths::FileExists(WithExt))
    {
        return WithExt;
    }

    return FString();
}

TArray<FSessionListing> ListSessions()
{
    TArray<FSessionListing> Out;
    const FString Dir = RecordingsDir();

    TArray<FString> Files;
    IFileManager::Get().FindFiles(Files, *FPaths::Combine(Dir, TEXT("session-*.ndjson")), true, false);

    for (const FString& File : Files)
    {
        const FString Full = FPaths::Combine(Dir, File);
        FSessionListing Listing;
        Listing.Id = FPaths::GetBaseFilename(File);
        Listing.Path = Full;
        const FDateTime ModTime = IFileManager::Get().GetTimeStamp(*Full);
        Listing.ModifiedUtcSeconds = (double)ModTime.ToUnixTimestamp();
        Out.Add(MoveTemp(Listing));
    }

    Out.Sort([](const FSessionListing& A, const FSessionListing& B)
    {
        return A.ModifiedUtcSeconds > B.ModifiedUtcSeconds;
    });
    return Out;
}

} // namespace RecorderResolver
