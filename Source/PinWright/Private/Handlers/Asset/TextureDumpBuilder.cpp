// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Handlers/Asset/TextureDumpBuilder.h"

#include "Compat/EngineVersionCompat.h"
#include "Engine/Texture.h"
#include "Engine/Texture2D.h"
#include "Engine/Texture2DArray.h"
#include "Engine/TextureCube.h"
#include "Engine/TextureCubeArray.h"
#include "Engine/TextureRenderTarget.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Interfaces/Interface_AsyncCompilation.h"
#if UE_VERSION_OLDER_THAN(5, 4, 0)
// On 5.3 the base UTextureRenderTarget has no GetFormat()/CanConvertToTexture(); the
// concrete render-target subclasses each expose their own GetFormat(), so the 5.3-only
// fallbacks below need the concrete types.
#include "Engine/TextureRenderTargetCube.h"
#include "Engine/TextureRenderTarget2DArray.h"
#include "Engine/TextureRenderTargetVolume.h"
#endif
#include "Engine/VolumeTexture.h"
#include "PixelFormat.h"
#include "Handlers/Asset/TextureTextEmitter.h"
#include "Handlers/Asset/AssetDumpHandler.h"
#include "Utils/JsonSidecarRegistry.h"

namespace
{
    template <typename TEnum>
    void AddEnumField(TSharedPtr<FJsonObject>& Root, const TCHAR* FieldName, TEnum Value)
    {
        if (const UEnum* Enum = StaticEnum<TEnum>())
        {
            Root->SetStringField(FieldName, Enum->GetNameStringByValue(static_cast<int64>(Value)));
        }
    }

    void AddPixelFormatField(TSharedPtr<FJsonObject>& Root, EPixelFormat PixelFormat)
    {
        if (PixelFormat != PF_Unknown)
        {
            Root->SetStringField(TEXT("pixelFormat"), GetPixelFormatString(PixelFormat));
        }
    }

#if UE_VERSION_OLDER_THAN(5, 4, 0)
    // 5.3 only: the base UTextureRenderTarget has no virtual GetFormat(). Each concrete
    // render-target subclass exposes its own non-virtual GetFormat(), so resolve the
    // pixel format by dynamic-casting to the concrete kinds. (On 5.4+ the base GetFormat()
    // is virtual and the generic UTextureRenderTarget branch already covers every kind.)
    EPixelFormat GetRenderTargetPixelFormat_53(const UTextureRenderTarget* RenderTarget)
    {
        if (const UTextureRenderTarget2D* RT2D = Cast<UTextureRenderTarget2D>(RenderTarget))
        {
            return RT2D->GetFormat();
        }
        if (const UTextureRenderTargetCube* RTCube = Cast<UTextureRenderTargetCube>(RenderTarget))
        {
            return RTCube->GetFormat();
        }
        if (const UTextureRenderTarget2DArray* RT2DArray = Cast<UTextureRenderTarget2DArray>(RenderTarget))
        {
            return RT2DArray->GetFormat();
        }
        if (const UTextureRenderTargetVolume* RTVolume = Cast<UTextureRenderTargetVolume>(RenderTarget))
        {
            return RTVolume->GetFormat();
        }
        return PF_Unknown;
    }

    // 5.3 only: mirrors UTextureRenderTarget2D::GetTextureFormatForConversionToTexture2D's
    // pixel-format → source-format mapping (which is 2D-only on 5.3) so non-2D render targets
    // still report a TSF_* source format. On 5.4+ the base CanConvertToTexture() supplies this.
    ETextureSourceFormat SourceFormatFromPixelFormat_53(EPixelFormat PixelFormat)
    {
        switch (PixelFormat)
        {
        case PF_B8G8R8A8: return TSF_BGRA8;
        case PF_FloatRGBA: return TSF_RGBA16F;
        case PF_G8:        return TSF_G8;
        default:           return TSF_Invalid;
        }
    }
#endif

    bool TryAddConcretePixelFormat(TSharedPtr<FJsonObject>& Root, const UTexture* Texture)
    {
        if (const UTexture2D* Texture2D = Cast<UTexture2D>(Texture))
        {
            AddPixelFormatField(Root, Texture2D->GetPixelFormat(0));
            return Root->HasField(TEXT("pixelFormat"));
        }
        if (const UTextureCube* TextureCube = Cast<UTextureCube>(Texture))
        {
            AddPixelFormatField(Root, TextureCube->GetPixelFormat());
            return Root->HasField(TEXT("pixelFormat"));
        }
        if (const UTexture2DArray* Texture2DArray = Cast<UTexture2DArray>(Texture))
        {
            AddPixelFormatField(Root, Texture2DArray->GetPixelFormat());
            return Root->HasField(TEXT("pixelFormat"));
        }
        if (const UTextureCubeArray* TextureCubeArray = Cast<UTextureCubeArray>(Texture))
        {
            AddPixelFormatField(Root, TextureCubeArray->GetPixelFormat());
            return Root->HasField(TEXT("pixelFormat"));
        }
        if (const UVolumeTexture* VolumeTexture = Cast<UVolumeTexture>(Texture))
        {
            AddPixelFormatField(Root, VolumeTexture->GetPixelFormat());
            return Root->HasField(TEXT("pixelFormat"));
        }
        if (const UTextureRenderTarget* RenderTarget = Cast<UTextureRenderTarget>(Texture))
        {
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 4, 0)
            // UTextureRenderTarget::GetFormat() was promoted to the base class in UE 5.4. On 5.4+
            // this resolves pixelFormat for every render-target kind, so the 5.3-only fallback
            // (and the RenderTarget2D `format` enum it emits) is skipped — newer-engine output
            // is byte-identical to before this change.
            AddPixelFormatField(Root, RenderTarget->GetFormat());
#else
            // On 5.3 the base has no GetFormat(); resolve via the concrete render-target kind
            // (2D, cube, 2D-array, volume) so non-2D render targets still report pixelFormat.
            AddPixelFormatField(Root, GetRenderTargetPixelFormat_53(RenderTarget));
            if (const UTextureRenderTarget2D* RenderTarget2D = Cast<UTextureRenderTarget2D>(RenderTarget))
            {
                AddEnumField(Root, TEXT("format"), RenderTarget2D->RenderTargetFormat.GetValue());
            }
#endif
            return Root->HasField(TEXT("pixelFormat")) || Root->HasField(TEXT("format"));
        }
        return false;
    }

    bool TryAddRenderTargetSource(TSharedPtr<FJsonObject>& Root, const UTextureRenderTarget* RenderTarget)
    {
        ETextureSourceFormat SourceFormat = TSF_Invalid;
        EPixelFormat PixelFormat = PF_Unknown;
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 4, 0)
        // UTextureRenderTarget::CanConvertToTexture() was promoted to the base class in UE 5.4.
        if (!RenderTarget->CanConvertToTexture(SourceFormat, PixelFormat, nullptr) || SourceFormat == TSF_Invalid)
        {
            return false;
        }
#else
        // On 5.3 the base has no CanConvertToTexture(); resolve the pixel format from the
        // concrete render-target kind and map it to a source format the same way 2D does.
        PixelFormat = GetRenderTargetPixelFormat_53(RenderTarget);
        if (const UTextureRenderTarget2D* RenderTarget2D = Cast<UTextureRenderTarget2D>(RenderTarget))
        {
            SourceFormat = RenderTarget2D->GetTextureFormatForConversionToTexture2D();
        }
        else
        {
            SourceFormat = SourceFormatFromPixelFormat_53(PixelFormat);
        }
        if (SourceFormat == TSF_Invalid)
        {
            return false;
        }
#endif

        TSharedPtr<FJsonObject> Source = MakeShared<FJsonObject>();
        Source->SetStringField(TEXT("sourceKind"), TEXT("renderTarget"));
        Source->SetNumberField(TEXT("x"), RenderTarget->GetSurfaceWidth());
        Source->SetNumberField(TEXT("y"), RenderTarget->GetSurfaceHeight());
        Source->SetNumberField(TEXT("z"), RenderTarget->GetSurfaceDepth());
        Source->SetNumberField(TEXT("slices"), RenderTarget->GetSurfaceArraySize());
        AddEnumField(Source, TEXT("format"), SourceFormat);
        if (PixelFormat != PF_Unknown)
        {
            Source->SetStringField(TEXT("pixelFormat"), GetPixelFormatString(PixelFormat));
        }
        Root->SetObjectField(TEXT("source"), Source);
        return true;
    }
}

bool TextureDumpBuilder::IsAsyncCompilationInProgress(
    const IInterface_AsyncCompilation* AsyncCompilation)
{
#if WITH_EDITOR
    return AsyncCompilation && AsyncCompilation->IsCompiling();
#else
    return false;
#endif
}

bool TextureDumpBuilder::IsCompiling(const UTexture* Texture)
{
#if WITH_EDITOR
    if (!Texture
        || !Texture->GetClass()->ImplementsInterface(UInterface_AsyncCompilation::StaticClass()))
    {
        return false;
    }

    const IInterface_AsyncCompilation* AsyncCompilation =
        Cast<IInterface_AsyncCompilation>(Texture);
    return IsAsyncCompilationInProgress(AsyncCompilation);
#else
    return false;
#endif
}

TSharedPtr<FJsonObject> TextureDumpBuilder::BuildTextureJson(const UTexture* Texture)
{
    if (!Texture)
    {
        return nullptr;
    }

    TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
    Root->SetStringField(TEXT("kind"), Texture->GetClass()->GetName());
    AddEnumField(Root, TEXT("textureClass"), Texture->GetTextureClass());

    TSharedPtr<FJsonObject> Size = MakeShared<FJsonObject>();
    Size->SetNumberField(TEXT("x"), Texture->GetSurfaceWidth());
    Size->SetNumberField(TEXT("y"), Texture->GetSurfaceHeight());
    Size->SetNumberField(TEXT("z"), Texture->GetSurfaceDepth());
    Root->SetObjectField(TEXT("size"), Size);
    Root->SetNumberField(TEXT("arraySize"), Texture->GetSurfaceArraySize());

    TryAddConcretePixelFormat(Root, Texture);

    AddEnumField(Root, TEXT("compressionSettings"), Texture->CompressionSettings.GetValue());
    AddEnumField(Root, TEXT("lodGroup"), Texture->LODGroup.GetValue());
    Root->SetNumberField(TEXT("lodBias"), Texture->LODBias);
    Root->SetBoolField(TEXT("srgb"), Texture->SRGB != 0);
    AddEnumField(Root, TEXT("mipGenSettings"), Texture->MipGenSettings.GetValue());
    Root->SetBoolField(TEXT("neverStream"), Texture->NeverStream != 0);
    Root->SetBoolField(TEXT("virtualTextureStreaming"), Texture->VirtualTextureStreaming != 0);
    AddEnumField(Root, TEXT("filter"), Texture->Filter.GetValue());
    // Address/wrap via the base UTexture virtual accessors (overridden by UTexture2D and
    // the other concrete classes) so the generic builder emits wrap for every texture kind
    // without a UTexture2D downcast; the base default is TA_Wrap for classes that don't store it.
    AddEnumField(Root, TEXT("addressX"), Texture->GetTextureAddressX());
    AddEnumField(Root, TEXT("addressY"), Texture->GetTextureAddressY());

    if (Texture->Source.IsValid())
    {
        TSharedPtr<FJsonObject> Source = MakeShared<FJsonObject>();
        Source->SetNumberField(TEXT("x"), static_cast<double>(Texture->Source.GetSizeX()));
        Source->SetNumberField(TEXT("y"), static_cast<double>(Texture->Source.GetSizeY()));
        Source->SetNumberField(TEXT("slices"), Texture->Source.GetNumSlices());
        AddEnumField(Source, TEXT("format"), Texture->Source.GetFormat());
        Root->SetObjectField(TEXT("source"), Source);
    }
    else if (const UTextureRenderTarget* RenderTarget = Cast<UTextureRenderTarget>(Texture))
    {
        TryAddRenderTargetSource(Root, RenderTarget);
    }

    return Root;
}

namespace
{
    UClass* GetTextureSidecarClass()
    {
        return UTexture::StaticClass();
    }

    TSharedPtr<FJsonObject> BuildTextureSidecar(UObject* Asset)
    {
        return TextureDumpBuilder::BuildTextureJson(Cast<UTexture>(Asset));
    }
}

REGISTER_DUMP_JSON_SIDECAR(TEXT("texture"), DumpFileNames::Texture,
    &GetTextureSidecarClass, &BuildTextureSidecar,
    DumpFileNames::TextureTxt, &TextureTextEmitter::BuildText,
    nullptr, 100);
