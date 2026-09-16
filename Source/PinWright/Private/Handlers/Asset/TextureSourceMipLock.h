// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Engine/Texture.h"
#include "Engine/TextureDefines.h"
#include "Templates/UniquePtr.h"

// Scoped, leak-proof access to a texture's *editable source* mip — the one buffer every
// pixel-reading and pixel-writing texture verb is allowed to touch.
//
// Two defects share this file's reason to exist, both filed Critical
// (B-combine-textures-leaks-bulkdata-lock-then-crashes,
// B-texture-action-bulkdata-lock-assert-fatal):
//
//  1. LOCK LEAK. The handlers used to read pixels from the *built platform* mip via
//     `GetPlatformData()->Mips[0].BulkData.LockReadOnly()`. FBulkData::LockReadOnly
//     (BulkData.cpp) sets LOCKSTATUS_ReadOnlyLock *before* returning
//     GetDataBufferReadOnly(), which is null for an empty / GPU-resident payload — so a null
//     return still means LOCKED. Every call site guarded its unlock on the returned pointer
//     (`if (BaseData) BaseMip.BulkData.Unlock();`), i.e. it skipped the unlock in exactly the
//     case where the lock had been taken. The payload stayed locked, and the next
//     LockReadOnly() on it hit `check(IsUnlocked())` — a Fatal assertion that ends the editor
//     PROCESS and every co-tenant agent's unsaved work with it.
//  2. WHY THE READ RETURNED NULL AT ALL. A built platform mip is the compressed / GPU-side
//     copy; for a TFO_AutoDXT texture it carries no readable CPU payload (the identical
//     finding behind B-texture-normal-from-height-crash). FTextureSource is the CPU-resident,
//     uncompressed editor copy and is what the pixel verbs must read.
//
// The fix is subtractive: there is no FBulkData lock left in the texture handlers to leak.
// The only lock is FTextureSource::FMipLock, whose ctor/dtor pair is the engine's own RAII —
// a failed lock leaves LockState == None and the dtor no-ops, so "released on every exit
// path" is a property of the type rather than a discipline at 22 call sites.
//
// Construction also flushes any in-flight async texture build first. That build holds its own
// ReadOnly lock on the same FTextureSource, and FTextureSource::LockMipInternal refuses a
// ReadWrite lock while a read lock is held (Texture.cpp, "cannot lock for write when
// previously locked for read") — it returns null, which is precisely the soft
// "Failed to lock texture data" the tickets report one call after CreateEmptyTexture kicked
// that build off.
namespace TextureSourceMip
{
    // Which source byte layouts a caller can decode. The pixel loops in the texture verbs
    // index 4 bytes per pixel in B,G,R,A order, so they demand Bgra8Only; readers that carry
    // their own grayscale branch ask for Bgra8OrG8; a destination buffer the verb itself just
    // created (e.g. TSF_RGBA16F HDR) passes Any.
    enum class EFormatPolicy : uint8
    {
        Any,
        Bgra8Only,
        Bgra8OrG8,
    };

    class PINWRIGHT_API FScopedMipLock
    {
    public:
        // Role names the texture's part in the operation ("base", "overlay", "output", ...)
        // and is woven into GetError(), so a caller learns WHICH texture refused and why —
        // the old "Failed to lock texture data" named neither the asset nor the side.
        FScopedMipLock(UTexture* InTexture,
                       const TCHAR* InRole,
                       bool bInReadOnly,
                       EFormatPolicy InPolicy = EFormatPolicy::Bgra8Only,
                       int32 InMipIndex = 0);
        ~FScopedMipLock();

        FScopedMipLock(const FScopedMipLock&) = delete;
        FScopedMipLock& operator=(const FScopedMipLock&) = delete;

        bool IsValid() const { return MipLock.IsValid(); }

        // Populated whenever the lock was not taken. Already names the texture, the role and
        // the reason, and says the failure is not retryable — the tickets' point being that
        // retrying the old soft error is what killed the editor.
        const FString& GetError() const { return Error; }

        // Null unless IsValid(). GetWriteData() additionally requires a ReadWrite lock.
        const uint8* GetReadData() const;
        uint8* GetWriteData() const;

        // Dimensions of the *locked* mip, so the pixel loop's bounds and its bytes always
        // come from the same buffer. UTexture2D::GetSizeX() reports the PLATFORM size, which
        // an LOD bias or a max-texture-size clamp can shrink below the source size.
        int32 GetSizeX() const { return SizeX; }
        int32 GetSizeY() const { return SizeY; }
        int64 GetNumPixels() const { return static_cast<int64>(SizeX) * static_cast<int64>(SizeY); }
        ETextureSourceFormat GetFormat() const { return Format; }
        int64 GetMipSizeBytes() const { return MipSizeBytes; }

        // Release the lock before the enclosing scope ends. Required before handing the
        // texture back to the engine (UpdateResource / PostEditChange / save): those paths
        // run FTextureSource::CheckTextureIsUnlocked, which asserts on a still-locked source.
        // Idempotent; the destructor is the safety net for every path that forgets.
        void Release();

    private:
        TUniquePtr<FTextureSource::FMipLock> MipLock;
        FString Error;
        int32 SizeX = 0;
        int32 SizeY = 0;
        ETextureSourceFormat Format = TSF_Invalid;
        int64 MipSizeBytes = 0;
    };
}
