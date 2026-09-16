// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Handlers/Asset/TextureSourceMipLock.h"

#include "Handlers/Asset/TexturePixelStats.h"
#include "Utils/JsonBuilders.h"
#include "TextureCompiler.h"
#include "UObject/Class.h"

namespace TextureSourceMip
{
    namespace
    {
        bool FormatAllowed(ETextureSourceFormat Format, EFormatPolicy Policy)
        {
            switch (Policy)
            {
            case EFormatPolicy::Bgra8Only:
                return Format == TSF_BGRA8;
            case EFormatPolicy::Bgra8OrG8:
                // The accepted-source-format allowlist lives in TexturePixelStats; do not
                // duplicate it here.
                return TexturePixelStats::IsReadableSourceFormat(Format);
            case EFormatPolicy::Any:
            default:
                return true;
            }
        }

        const TCHAR* PolicyExpectation(EFormatPolicy Policy)
        {
            switch (Policy)
            {
            case EFormatPolicy::Bgra8Only:
                return TEXT("an 8-bit BGRA (TSF_BGRA8) texture");
            case EFormatPolicy::Bgra8OrG8:
                return TEXT("an 8-bit BGRA (TSF_BGRA8) or grayscale (TSF_G8) texture");
            case EFormatPolicy::Any:
            default:
                return TEXT("a texture with editable source data");
            }
        }
    }

    FScopedMipLock::FScopedMipLock(UTexture* InTexture,
                                   const TCHAR* InRole,
                                   bool bInReadOnly,
                                   EFormatPolicy InPolicy,
                                   int32 InMipIndex)
    {
        const FString Role = (InRole && *InRole) ? FString(InRole) : FString(TEXT("texture"));

        if (!InTexture)
        {
            Error = FString::Printf(TEXT("%s texture is null"), *Role);
            return;
        }

        const FString AssetName = InTexture->GetPathName();
        FTextureSource& Source = InTexture->Source;

        // Every refusal below is a property of the asset, not a transient condition: the
        // tickets' failure mode was a caller retrying a soft error, so say so explicitly.
        if (!Source.IsValid())
        {
            Error = FString::Printf(
                TEXT("%s texture '%s' has no editable source data to lock (only built platform data); ")
                TEXT("re-import or re-create it. Retrying will not help."),
                *Role, *AssetName);
            return;
        }

        const int32 NumMips = Source.GetNumMips();
        if (InMipIndex < 0 || InMipIndex >= NumMips)
        {
            Error = FString::Printf(
                TEXT("mip %d is out of range on %s texture '%s' (source has %d mip(s)). Retrying will not help."),
                InMipIndex, *Role, *AssetName, NumMips);
            return;
        }

        Format = Source.GetFormat();
        if (!FormatAllowed(Format, InPolicy))
        {
            // Fail loud rather than misread the byte layout — the same policy
            // create_normal_from_height adopted after B-texture-normal-from-height-crash.
            Error = FString::Printf(
                TEXT("%s texture '%s' has source format %s, which this operation cannot decode; supply %s. ")
                TEXT("Retrying will not help."),
                *Role, *AssetName, *JsonBuilders::EnumValueToString(Format), PolicyExpectation(InPolicy));
            Format = TSF_Invalid;
            return;
        }

        // An async DDC build of this texture holds a ReadOnly lock on this very FTextureSource
        // while it runs, and LockMipInternal refuses ReadWrite under a read lock (it returns
        // null, which the handlers reported as "Failed to lock texture data"). CreateEmptyTexture
        // kicks such a build off through UpdateResource(), so the destination of a create-then-
        // write verb is the common victim. Flush before asking for the lock.
        FTextureCompilingManager::Get().FinishCompilation({ InTexture });

        MipLock = MakeUnique<FTextureSource::FMipLock>(
            bInReadOnly ? FTextureSource::ELockState::ReadOnly : FTextureSource::ELockState::ReadWrite,
            &Source,
            InMipIndex);

        if (!MipLock->IsValid())
        {
            // FMipLock leaves LockState == None when the lock was refused, so resetting here
            // releases nothing — there is nothing to leak.
            MipLock.Reset();
            Error = FString::Printf(
                TEXT("Failed to lock mip %d of %s texture '%s' for %s; another editor operation holds a ")
                TEXT("conflicting lock on its source. Retrying will not help."),
                InMipIndex, *Role, *AssetName, bInReadOnly ? TEXT("read") : TEXT("write"));
            Format = TSF_Invalid;
            return;
        }

        SizeX = FMath::Max(1, static_cast<int32>(Source.GetSizeX()) >> InMipIndex);
        SizeY = FMath::Max(1, static_cast<int32>(Source.GetSizeY()) >> InMipIndex);
        MipSizeBytes = Source.CalcMipSize(InMipIndex);
    }

    FScopedMipLock::~FScopedMipLock()
    {
        Release();
    }

    void FScopedMipLock::Release()
    {
        MipLock.Reset();
    }

    const uint8* FScopedMipLock::GetReadData() const
    {
        return MipLock.IsValid() ? static_cast<const uint8*>(MipLock->GetRawData()) : nullptr;
    }

    uint8* FScopedMipLock::GetWriteData() const
    {
        if (!MipLock.IsValid() || MipLock->LockState != FTextureSource::ELockState::ReadWrite)
        {
            return nullptr;
        }
        return static_cast<uint8*>(MipLock->GetMutableData());
    }
}
