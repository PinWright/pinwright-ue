// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Utils/GatewayPortFile.h"

#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

DEFINE_LOG_CATEGORY_STATIC(LogGatewayPortFile, Log, All);

namespace GatewayPortFile
{
// Named namespace (not anonymous): unity build merges TUs, so file-scope helpers
// must have a unique namespace to avoid ODR collisions.
namespace Detail
{
#if WITH_DEV_AUTOMATION_TESTS
    FString& RootOverrideForTests()
    {
        static FString RootOverride;
        return RootOverride;
    }
#endif

    FString GetPortFileRoot()
    {
#if WITH_DEV_AUTOMATION_TESTS
        const FString& RootOverride = RootOverrideForTests();
        if (!RootOverride.IsEmpty())
        {
            FString Root = FPaths::ConvertRelativePathToFull(RootOverride);
            FPaths::NormalizeDirectoryName(Root);
            return Root;
        }
#endif

        FString Root = FPaths::ProjectSavedDir() / TEXT("PinWright");
        Root = FPaths::ConvertRelativePathToFull(Root);
        FPaths::NormalizeDirectoryName(Root);
        return Root;
    }
}

FString GetPortFilePath()
{
    FString Path = Detail::GetPortFileRoot() / TEXT("gateway-port");
    Path = FPaths::ConvertRelativePathToFull(Path);
    FPaths::NormalizeFilename(Path);
    return Path;
}

bool WritePortFile(int32 Port)
{
    const FString FinalPath = GetPortFilePath();

    IFileManager& FileManager = IFileManager::Get();
    FileManager.MakeDirectory(*FPaths::GetPath(FinalPath), /*Tree=*/true);

    // tmp-then-move so readers never observe a partially written port value.
    const FString TmpPath = FinalPath + TEXT(".tmp");
    const bool bSaved = FFileHelper::SaveStringToFile(
        FString::FromInt(Port),
        *TmpPath,
        FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
    if (!bSaved)
    {
        FileManager.Delete(*TmpPath, /*RequireExists=*/false);
        UE_LOG(LogGatewayPortFile, Error,
            TEXT("Failed to write gateway port tmp file: %s"), *TmpPath);
        return false;
    }

    if (!FileManager.Move(*FinalPath, *TmpPath, /*bReplace=*/true, /*bEvenReadOnly=*/false))
    {
        FileManager.Delete(*TmpPath, /*RequireExists=*/false);
        UE_LOG(LogGatewayPortFile, Error,
            TEXT("Failed to move gateway port tmp file to final path: %s"), *FinalPath);
        return false;
    }

    return true;
}

bool ReadPortFile(int32& OutPort)
{
    FString Content;
    if (!FFileHelper::LoadFileToString(Content, *GetPortFilePath()))
    {
        return false;
    }
    Content.TrimStartAndEndInline();
    if (Content.IsEmpty() || !Content.IsNumeric())
    {
        return false;
    }
    // IsNumeric() accepts a leading sign and a decimal point; the range check below is what
    // actually rejects those, because a port file is never legitimately either.
    const int64 Parsed = FCString::Atoi64(*Content);
    if (Parsed < 1 || Parsed > 65535 || FString::Printf(TEXT("%lld"), Parsed) != Content)
    {
        return false;
    }
    OutPort = static_cast<int32>(Parsed);
    return true;
}

bool RemovePortFile()
{
    const FString FinalPath = GetPortFilePath();
    IFileManager& FileManager = IFileManager::Get();
    if (!FileManager.FileExists(*FinalPath))
    {
        return true;
    }
    if (!FileManager.Delete(*FinalPath, /*RequireExists=*/false, /*EvenReadOnly=*/true))
    {
        UE_LOG(LogGatewayPortFile, Error,
            TEXT("Failed to retract the stale gateway port file: %s. A proxy reading it will keep ")
            TEXT("being pointed at an endpoint no editor of this project is serving."), *FinalPath);
        return false;
    }
    return true;
}

#if WITH_DEV_AUTOMATION_TESTS
void SetRootOverrideForTests(const FString& Root)
{
    Detail::RootOverrideForTests() = Root;
}
#endif
}
