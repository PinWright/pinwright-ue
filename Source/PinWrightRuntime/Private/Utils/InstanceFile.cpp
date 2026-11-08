// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Utils/InstanceFile.h"

#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Runtime/Launch/Resources/Version.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

#if PLATFORM_WINDOWS
#include "Windows/AllowWindowsPlatformTypes.h"
#include <sddl.h>
#include "Windows/HideWindowsPlatformTypes.h"
#elif PLATFORM_MAC || PLATFORM_LINUX
#include <sys/stat.h>
#endif

DEFINE_LOG_CATEGORY_STATIC(LogPinWrightInstanceFile, Log, All);

namespace PinWrightInstanceFile
{
// Named namespace (not anonymous): unity build merges TUs.
namespace Detail
{
    // Static-init time of this module, which loads with the process: the instance's start time.
    const FDateTime ProcessStartUtc = FDateTime::UtcNow();

#if WITH_DEV_AUTOMATION_TESTS
    FString& RootOverrideForTests()
    {
        static FString RootOverride;
        return RootOverride;
    }
#endif

    FString GetRoot()
    {
        FString Root = FPaths::ProjectSavedDir() / TEXT("PinWright");
#if WITH_DEV_AUTOMATION_TESTS
        if (!RootOverrideForTests().IsEmpty())
        {
            Root = RootOverrideForTests();
        }
#endif
        Root = FPaths::ConvertRelativePathToFull(Root);
        FPaths::NormalizeDirectoryName(Root);
        return Root;
    }

    // The file carries a bearer token: owner (and SYSTEM on Windows) only.
    bool RestrictToUser(const FString& Path)
    {
#if PLATFORM_WINDOWS
        PSECURITY_DESCRIPTOR Descriptor = nullptr;
        if (!::ConvertStringSecurityDescriptorToSecurityDescriptorW(
                TEXT("D:P(A;;FA;;;OW)(A;;FA;;;SY)"), SDDL_REVISION_1, &Descriptor, nullptr))
        {
            return false;
        }
        const bool bSet = ::SetFileSecurityW(*Path, DACL_SECURITY_INFORMATION, Descriptor) != 0;
        ::LocalFree(Descriptor);
        return bSet;
#elif PLATFORM_MAC || PLATFORM_LINUX
        return chmod(TCHAR_TO_UTF8(*Path), S_IRUSR | S_IWUSR) == 0;
#else
        return true;
#endif
    }
}

FString GetPath()
{
    FString Path = Detail::GetRoot() / TEXT("instances") /
        FString::Printf(TEXT("%u.json"), FPlatformProcess::GetCurrentProcessId());
    FPaths::NormalizeFilename(Path);
    return Path;
}

bool Write(const int32 Port, const FString& Token, const FString& Kind,
           const FString& Reason, const FString& Map)
{
    const TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
    Obj->SetNumberField(TEXT("pid"), static_cast<double>(FPlatformProcess::GetCurrentProcessId()));
    Obj->SetNumberField(TEXT("port"), Port);
    Obj->SetStringField(TEXT("token"), Token);
    Obj->SetStringField(TEXT("kind"), Kind);
    Obj->SetStringField(TEXT("project"), FPaths::ConvertRelativePathToFull(FPaths::GetProjectFilePath()));
    Obj->SetStringField(TEXT("startTime"), Detail::ProcessStartUtc.ToIso8601());
    Obj->SetStringField(TEXT("reason"), Reason);
    Obj->SetStringField(TEXT("map"), Map);
    Obj->SetStringField(TEXT("engine"),
        FString::Printf(TEXT("%d.%d"), ENGINE_MAJOR_VERSION, ENGINE_MINOR_VERSION));

    FString Text;
    const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
        TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Text);
    FJsonSerializer::Serialize(Obj, Writer);

    const FString FinalPath = GetPath();
    IFileManager& FileManager = IFileManager::Get();
    FileManager.MakeDirectory(*FPaths::GetPath(FinalPath), /*Tree=*/true);

    // Restricted before it holds the token and before the move, so no reader ever sees an open
    // file; an NTFS rename within the directory keeps the DACL.
    const FString TmpPath = FinalPath + TEXT(".tmp");
    const bool bWritten =
        FFileHelper::SaveStringToFile(FString(), *TmpPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)
        && Detail::RestrictToUser(TmpPath)
        && FFileHelper::SaveStringToFile(Text, *TmpPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)
        && FileManager.Move(*FinalPath, *TmpPath, /*bReplace=*/true, /*bEvenReadOnly=*/false);
    if (!bWritten)
    {
        FileManager.Delete(*TmpPath, /*RequireExists=*/false);
        UE_LOG(LogPinWrightInstanceFile, Error,
               TEXT("Serving on port %d but the instance file %s could not be written; clients that ")
               TEXT("discover instances by file will not find this process."),
               Port, *FinalPath);
        return false;
    }
    return true;
}

void Delete(const FString& Path)
{
    if (!Path.IsEmpty())
    {
        IFileManager::Get().Delete(*Path, /*RequireExists=*/false, /*EvenReadOnly=*/true);
    }
}

#if WITH_DEV_AUTOMATION_TESTS
void SetRootOverrideForTests(const FString& Root)
{
    Detail::RootOverrideForTests() = Root;
}
#endif
}
