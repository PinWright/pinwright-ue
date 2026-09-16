// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Utils/GatewayAuthToken.h"

#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

#if PLATFORM_MAC || PLATFORM_LINUX
#include <sys/stat.h>
#endif

// OpenSSL's <openssl/ossl_typ.h> declares `typedef struct ui_st UI;`. Under unity
// builds this TU is merged with UObject headers that declare `namespace UI`
// (ObjectMacros.h), which collides (C2365). RAND_* never uses the UI type, so
// rename OpenSSL's `UI` away for the duration of the include.
#if PLATFORM_WINDOWS
#include "Windows/AllowWindowsPlatformTypes.h"
#endif
THIRD_PARTY_INCLUDES_START
#define UI OpenSSL_UI
#include <openssl/rand.h>
#undef UI
THIRD_PARTY_INCLUDES_END
#if PLATFORM_WINDOWS
#include "Windows/HideWindowsPlatformTypes.h"
#endif

DEFINE_LOG_CATEGORY_STATIC(LogGatewayAuthToken, Log, All);

namespace GatewayAuthToken
{
// Named namespace (not anonymous): unity build merges TUs, so file-scope helpers
// must have a unique namespace to avoid ODR collisions.
namespace Detail
{
    // 32 CSPRNG bytes hex-encode to 64 lowercase characters.
    constexpr int32 TokenNumBytes = 32;

#if WITH_DEV_AUTOMATION_TESTS
    FString& RootOverrideForTests()
    {
        static FString RootOverride;
        return RootOverride;
    }
#endif

    FString GetTokenRoot()
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

    FString GenerateToken()
    {
        uint8 Bytes[TokenNumBytes];
        if (RAND_bytes(Bytes, TokenNumBytes) != 1)
        {
            UE_LOG(LogGatewayAuthToken, Error,
                TEXT("RAND_bytes failed to produce %d bytes of CSPRNG output; "
                     "refusing to issue a weak gateway auth token."),
                TokenNumBytes);
            return FString();
        }

        FString Token = BytesToHex(Bytes, TokenNumBytes);
        Token.ToLowerInline();
        return Token;
    }

    bool WriteTokenFile(const FString& Token)
    {
        const FString FinalPath = GetTokenFilePath();

        IFileManager& FileManager = IFileManager::Get();
        FileManager.MakeDirectory(*FPaths::GetPath(FinalPath), /*Tree=*/true);

        const FString TmpPath = FinalPath + TEXT(".tmp");
        const bool bSaved = FFileHelper::SaveStringToFile(
            Token,
            *TmpPath,
            FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
        if (!bSaved)
        {
            FileManager.Delete(*TmpPath, /*RequireExists=*/false);
            UE_LOG(LogGatewayAuthToken, Error,
                TEXT("Failed to write gateway auth token tmp file: %s"), *TmpPath);
            return false;
        }

#if PLATFORM_MAC || PLATFORM_LINUX
        // Owner-only read/write so the shared secret is not world-readable on POSIX.
        chmod(TCHAR_TO_UTF8(*TmpPath), S_IRUSR | S_IWUSR);
#endif

        if (!FileManager.Move(*FinalPath, *TmpPath, /*bReplace=*/true, /*bEvenReadOnly=*/false))
        {
            FileManager.Delete(*TmpPath, /*RequireExists=*/false);
            UE_LOG(LogGatewayAuthToken, Error,
                TEXT("Failed to move gateway auth token tmp file to final path: %s"), *FinalPath);
            return false;
        }

        return true;
    }
}

FString GetTokenFilePath()
{
    FString Path = Detail::GetTokenRoot() / TEXT("gateway-token");
    Path = FPaths::ConvertRelativePathToFull(Path);
    FPaths::NormalizeFilename(Path);
    return Path;
}

FString GetOrCreateToken()
{
    const FString Path = GetTokenFilePath();

    FString Existing;
    if (FFileHelper::LoadFileToString(Existing, *Path))
    {
        Existing.TrimStartAndEndInline();
        if (!Existing.IsEmpty())
        {
            return Existing;
        }
    }

    const FString Token = Detail::GenerateToken();
    if (Token.IsEmpty() || !Detail::WriteTokenFile(Token))
    {
        return FString();
    }

    return Token;
}

bool ConstantTimeEquals(const FString& A, const FString& B)
{
    // Length is not secret, so bail early on mismatch.
    if (A.Len() != B.Len())
    {
        return false;
    }

    // OR-accumulate per-character XOR diffs (as OpenSSL CRYPTO_memcmp does): the
    // loop touches every character regardless of where a mismatch first appears,
    // so timing does not leak the matching prefix length.
    volatile uint8 Acc = 0;
    const int32 Len = A.Len();
    for (int32 Index = 0; Index < Len; ++Index)
    {
        Acc |= static_cast<uint8>(A[Index]) ^ static_cast<uint8>(B[Index]);
    }
    return Acc == 0;
}

#if WITH_DEV_AUTOMATION_TESTS
void SetTokenRootOverrideForTests(const FString& Root)
{
    Detail::RootOverrideForTests() = Root;
}
#endif
}
