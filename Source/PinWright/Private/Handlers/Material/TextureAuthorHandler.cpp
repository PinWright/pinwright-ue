// Copyright (c) 2026 Alexander Penkin. MIT License.

// texture.create_from_pixels / texture.create_text_texture - the two texture verbs that AUTHOR
// content rather than filter it (board F-texture-cannot-author-letterforms-or-pixel-data).
// Every other texture.* creator is a fixed parametric fill, so a marking, stencil, label or any
// caller-rasterised shape had no in-editor route. create_from_pixels takes the caller's own
// pixels; create_text_texture rasterises real font glyphs (any runtime UFont / UFontFace, the
// engine's Roboto by default) with FreeType on the CPU onto a background that may be fully
// transparent - no forced backing plate, unlike the 3x5 annotation font in BitmapPaint.h.

#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/ParamSpec.h"
#include "Handlers/ErrorCodes.h"
#include "Handlers/PackagePathCompose.h"
#include "Handlers/Asset/TexturePixelStats.h"
#include "Handlers/Asset/TextureSourceMipLock.h"
#include "Handlers/Material/TextureAssetHelpers.h"
#include "PinWrightHelpers.h"
#include "Utils/AssetUtils.h"
#include "Dom/JsonObject.h"
#include "Engine/Texture2D.h"
#include "Engine/Font.h"
#include "Engine/FontFace.h"
#include "Fonts/CompositeFont.h"
#include "Misc/Base64.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"

#if PLATFORM_COMPILER_HAS_GENERIC_KEYWORD
    #define generic __identifier(generic)
#endif
THIRD_PARTY_INCLUDES_START
#include "ft2build.h"
#include FT_FREETYPE_H
THIRD_PARTY_INCLUDES_END
#if PLATFORM_COMPILER_HAS_GENERIC_KEYWORD
    #undef generic
#endif

namespace TextureAuthorHandlerHelpers
{
    static constexpr int32 MaxAuthoredTextureSize = 4096;
    static constexpr int32 MaxTextPixelSize = 1024;
    // Every character costs two FT_Load_Glyph calls on the game thread; this bounds the call and
    // keeps every 26.6 layout sum far inside int32 once shifted to pixels.
    static constexpr int32 MaxTextLength = 4096;
    static const TCHAR* const DefaultTextFont = TEXT("/Engine/EngineFonts/Roboto.Roboto");

    // Validate the destination and refuse to overwrite. Sends the error and returns false on
    // refusal; run before any expensive work so a bad name is not reported after a full raster.
    static bool ValidateAuthoredDestination(FHandlerContext& Ctx, const FString& RawName, const FString& RawPath,
        FString& OutName, FString& OutPath, FString& OutObjectPath)
    {
        const FString Name = SanitizeAssetName(RawName);
        const FString Path = SanitizeProjectRelativePath(RawPath);
        FString PackagePath;
        FString PathError;
        if (Name.IsEmpty() || Path.IsEmpty()
            || !PinWrightComposeAssetPackagePath(NormalizeContentAssetPath(Path), Name, PackagePath, PathError))
        {
            Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT, FString::Printf(
                TEXT("Invalid texture destination name '%s' / path '%s'%s%s"), *RawName, *RawPath,
                PathError.IsEmpty() ? TEXT("") : TEXT(": "), *PathError));
            return false;
        }
        // NewObject over a live object of the same name reinitialises it in place under every
        // existing reference, so an existing asset is refused rather than silently replaced.
        const FString ObjectPath = PackagePath + TEXT(".") + Name;
        if (FPackageName::DoesPackageExist(PackagePath) || StaticFindObject(UObject::StaticClass(), nullptr, *ObjectPath))
        {
            Ctx.SendError(ErrorCodes::ERR_ASSET_ALREADY_EXISTS, FString::Printf(
                TEXT("Asset '%s' already exists; pick another name or delete it first"), *ObjectPath));
            return false;
        }
        OutName = Name;
        OutPath = Path;
        OutObjectPath = ObjectPath;
        return true;
    }

    // Shared by both verbs: validate the destination, create the BGRA8 texture, copy Pixels into
    // its source mip, save, and answer with a pixel-stats block read back from the written source
    // (the measurement the write path cannot fake).
    static bool CreateAndSendAuthoredTexture(FHandlerContext& Ctx, const FString& RawName, const FString& RawPath,
        int32 Width, int32 Height, const TArray<FColor>& Pixels, bool bSave, const TSharedPtr<FJsonObject>& Response)
    {
        FString Name;
        FString Path;
        FString ObjectPath;
        if (!ValidateAuthoredDestination(Ctx, RawName, RawPath, Name, Path, ObjectPath))
        {
            return true;
        }

        FString CreateErrorCode;
        FString CreateError;
        UTexture2D* Texture = PinWrightTextureAssets::CreateEmptyTexture(Path, Name, Width, Height, /*bHDR=*/false,
            CreateErrorCode, CreateError);
        if (!Texture)
        {
            Ctx.SendError(ErrorCodes::ERR_CREATE_FAILED, FString::Printf(TEXT("Failed to create texture '%s': %s"), *ObjectPath, *CreateError));
            return true;
        }
        {
            TextureSourceMip::FScopedMipLock MipLock(Texture, TEXT("output"), /*bReadOnly=*/false);
            if (!MipLock.IsValid())
            {
                Ctx.SendError(ErrorCodes::ERR_CREATE_FAILED, MipLock.GetError());
                return true;
            }
            // FColor's in-memory order is B,G,R,A - exactly the TSF_BGRA8 source layout.
            FMemory::Memcpy(MipLock.GetWriteData(), Pixels.GetData(), Pixels.Num() * sizeof(FColor));
        }
        Texture->UpdateResource();
        PinWrightTextureAssets::McpSaveTextureToDisk(Response, Texture, bSave);

        Response->SetStringField(TEXT("assetPath"), Texture->GetPathName());
        Response->SetNumberField(TEXT("width"), Width);
        Response->SetNumberField(TEXT("height"), Height);
        FString StatsError;
        TSharedPtr<FJsonObject> Stats = TexturePixelStats::BuildPixelStatsJson(Texture, 0, StatsError);
        if (Stats.IsValid())
        {
            Response->SetObjectField(TEXT("pixelStats"), Stats);
        }
        else
        {
            Response->SetStringField(TEXT("pixelStatsError"), StatsError);
        }
        AddAssetVerification(Response, Texture);
        Ctx.SendSuccess(Response);
        return true;
    }

    static bool CheckAuthoredSize(FHandlerContext& Ctx, int32 Width, int32 Height)
    {
        if (Width < 1 || Height < 1 || Width > MaxAuthoredTextureSize || Height > MaxAuthoredTextureSize)
        {
            Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT, FString::Printf(
                TEXT("Texture size %dx%d is out of range; width and height must be 1..%d"),
                Width, Height, MaxAuthoredTextureSize));
            return false;
        }
        return true;
    }

    // {r,g,b,a} in 0..1, written as bytes without a colour-space conversion - the same reading
    // every other texture.* creator gives its colour objects.
    static FLinearColor ReadColor(const TSharedPtr<FJsonObject>& Obj, const FLinearColor& Default)
    {
        if (!Obj.IsValid())
        {
            return Default;
        }
        return FLinearColor(
            static_cast<float>(GetJsonNumberField(Obj, TEXT("r"), Default.R)),
            static_cast<float>(GetJsonNumberField(Obj, TEXT("g"), Default.G)),
            static_cast<float>(GetJsonNumberField(Obj, TEXT("b"), Default.B)),
            static_cast<float>(GetJsonNumberField(Obj, TEXT("a"), Default.A)));
    }

    static uint8 UnitToByte(float V)
    {
        return static_cast<uint8>(FMath::RoundToInt(FMath::Clamp(V, 0.0f, 1.0f) * 255.0f));
    }

    // Straight-alpha "over": the text colour at Coverage over the background. Where the result
    // is fully transparent the RGB is the text colour, not black, so mip filtering of a
    // transparent-background label does not grow a dark fringe around the glyphs.
    static FColor CompositeTextPixel(const FLinearColor& Fg, const FLinearColor& Bg, float Coverage)
    {
        const float Sa = FMath::Clamp(Fg.A, 0.0f, 1.0f) * Coverage;
        const float Ba = FMath::Clamp(Bg.A, 0.0f, 1.0f);
        const float OutA = Sa + Ba * (1.0f - Sa);
        FLinearColor Out = Fg;
        if (OutA > 0.0f)
        {
            Out = (Fg * Sa + Bg * (Ba * (1.0f - Sa))) / OutA;
        }
        return FColor(UnitToByte(Out.R), UnitToByte(Out.G), UnitToByte(Out.B), UnitToByte(OutA));
    }

    // TCHAR is UTF-16 on some platforms; FreeType wants code points.
    static TArray<uint32> ToCodePoints(const FString& Line)
    {
        TArray<uint32> Out;
        for (int32 i = 0; i < Line.Len(); ++i)
        {
            uint32 C = static_cast<uint32>(Line[i]);
            if (C >= 0xD800 && C <= 0xDBFF && i + 1 < Line.Len())
            {
                const uint32 Low = static_cast<uint32>(Line[i + 1]);
                if (Low >= 0xDC00 && Low <= 0xDFFF)
                {
                    C = 0x10000 + ((C - 0xD800) << 10) + (Low - 0xDC00);
                    ++i;
                }
            }
            Out.Add(C);
        }
        return Out;
    }

    // Owns the FreeType library + face for one call.
    struct FScopedFreeTypeFace
    {
        FT_Library Library = nullptr;
        FT_Face Face = nullptr;
        ~FScopedFreeTypeFace()
        {
            if (Face)
            {
                FT_Done_Face(Face);
            }
            if (Library)
            {
                FT_Done_FreeType(Library);
            }
        }
    };
}

REGISTER_RPC_HANDLER("texture.create_from_pixels", "Texture",
    "Create a texture from caller-supplied pixel data (base64 RGBA8 or Gray8 bytes, row-major from the top-left). The general route for any shape, stencil or mask the caller rasterises itself; texture.create_text_texture draws real font glyphs",
    RPC_PARAMS(
        RPC_PARAM_REQ("name", "string", "Asset name for the new texture. An existing asset of that name is refused, not replaced"),
        RPC_PARAM_DEF("path", "path", "Destination package path", "/Game/Textures"),
        RPC_PARAM_REQ("width", "integer", "Texture width in pixels (1..4096)"),
        RPC_PARAM_REQ("height", "integer", "Texture height in pixels (1..4096)"),
        RPC_PARAM_REQ("data", "string", "Base64 of the raw pixel bytes, row-major from the top-left, exactly width*height*bytesPerPixel bytes (no PNG/compression). The request body limit (HttpMaxRequestBodyBytes, 1 MiB by default) bounds how much fits in one call"),
        RPC_PARAM_DEF("format", "string", "Byte layout of data: RGBA8 (4 bytes per pixel, R,G,B,A) or Gray8 (1 byte per pixel, written as R=G=B with alpha 255). Any other value is refused", "RGBA8"),
        RPC_PARAM_DEF("save", "boolean", "Save the asset to disk", "true")))
{
    using namespace TextureAuthorHandlerHelpers;

    FString Name;
    FString Data;
    int32 Width = 0;
    int32 Height = 0;
    if (!Ctx.RequireString(TEXT("name"), Name) || !Ctx.RequireInt(TEXT("width"), Width)
        || !Ctx.RequireInt(TEXT("height"), Height) || !Ctx.RequireString(TEXT("data"), Data))
    {
        return true;
    }
    if (!CheckAuthoredSize(Ctx, Width, Height))
    {
        return true;
    }
    const FString Format = Ctx.GetString(TEXT("format"), TEXT("RGBA8"));
    int32 BytesPerPixel = 0;
    if (Format.Equals(TEXT("RGBA8"), ESearchCase::IgnoreCase))
    {
        BytesPerPixel = 4;
    }
    else if (Format.Equals(TEXT("Gray8"), ESearchCase::IgnoreCase))
    {
        BytesPerPixel = 1;
    }
    else
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
            FString::Printf(TEXT("Unknown format '%s'; expected RGBA8 or Gray8"), *Format));
        return true;
    }

    TArray<uint8> Bytes;
    if (!FBase64::Decode(Data, Bytes))
    {
        Ctx.SendError(ErrorCodes::ERR_DECODE_FAILED, TEXT("data is not valid base64"));
        return true;
    }
    const int64 Expected = static_cast<int64>(Width) * Height * BytesPerPixel;
    if (Bytes.Num() != Expected)
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT, FString::Printf(
            TEXT("data decodes to %d bytes but %dx%d %s needs exactly %lld"),
            Bytes.Num(), Width, Height, *Format, Expected));
        return true;
    }

    TArray<FColor> Pixels;
    Pixels.SetNumUninitialized(Width * Height);
    for (int32 i = 0; i < Pixels.Num(); ++i)
    {
        const uint8* P = Bytes.GetData() + static_cast<int64>(i) * BytesPerPixel;
        Pixels[i] = BytesPerPixel == 4 ? FColor(P[0], P[1], P[2], P[3]) : FColor(P[0], P[0], P[0], 255);
    }

    TSharedPtr<FJsonObject> Response = MakeShared<FJsonObject>();
    Response->SetStringField(TEXT("format"), BytesPerPixel == 4 ? TEXT("RGBA8") : TEXT("Gray8"));
    return CreateAndSendAuthoredTexture(Ctx, Name, Ctx.GetString(TEXT("path"), TEXT("/Game/Textures")),
        Width, Height, Pixels, Ctx.GetBool(TEXT("save"), true), Response);
}

REGISTER_RPC_HANDLER("texture.create_text_texture", "Texture",
    "Create a texture with text rasterised from a real font (any runtime UFont or UFontFace; the engine's Roboto by default) at an authorable pixel size, on a background that may be fully transparent. For labels, markings, serial numbers and stencil sources",
    RPC_PARAMS(
        RPC_PARAM_REQ("name", "string", "Asset name for the new texture. An existing asset of that name is refused, not replaced"),
        RPC_PARAM_DEF("path", "path", "Destination package path", "/Game/Textures"),
        RPC_PARAM_REQ("text", "string", "Text to draw, at most 4096 characters (longer is refused); '\\n' starts a new line. Characters the font has no glyph for are listed in missingCharacters"),
        RPC_PARAM_DEF("font", "path", "Font asset: a runtime UFont (composite font) or a UFontFace. Offline (bitmap-cache) fonts are refused", "/Engine/EngineFonts/Roboto.Roboto"),
        RPC_PARAM_OPT("typeface", "string", "Typeface name inside a composite font's default typeface (Roboto: Regular, Bold, Italic, Bold Italic, Light). Defaults to the font's first typeface; an unknown name is refused with the available list"),
        RPC_PARAM_DEF("size", "integer", "Font size in pixels (FreeType pixel em size, 1..1024)", "64"),
        RPC_PARAM_DEF("align", "string", "Horizontal alignment of each line: Left, Center or Right. The block of lines is centred vertically", "Center"),
        RPC_PARAM_OPT("color", "object", "Text color {r,g,b,a} in 0..1 (default opaque white)"),
        RPC_PARAM_OPT("backgroundColor", "object", "Background color {r,g,b,a} in 0..1 (default fully transparent {0,0,0,0}; text alpha is the glyph coverage)"),
        RPC_PARAM_OPT("width", "integer", "Texture width in pixels (1..4096). Omitted: fits the widest line's advance and ink (italic overhang and negative bearings included)"),
        RPC_PARAM_OPT("height", "integer", "Texture height in pixels (1..4096). Omitted: fits the line block (lines x font line height)"),
        RPC_PARAM_DEF("save", "boolean", "Save the asset to disk", "true")))
{
    using namespace TextureAuthorHandlerHelpers;

    FString Name;
    FString Text;
    if (!Ctx.RequireString(TEXT("name"), Name) || !Ctx.RequireString(TEXT("text"), Text))
    {
        return true;
    }
    if (Text.Len() > MaxTextLength)
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT, FString::Printf(
            TEXT("text is %d characters; at most %d are accepted per texture"), Text.Len(), MaxTextLength));
        return true;
    }
    {
        FString CheckedName, CheckedPath, CheckedObjectPath;
        if (!ValidateAuthoredDestination(Ctx, Name, Ctx.GetString(TEXT("path"), TEXT("/Game/Textures")),
                CheckedName, CheckedPath, CheckedObjectPath))
        {
            return true;
        }
    }
    const int32 Size = Ctx.GetInt(TEXT("size"), 64);
    if (Size < 1 || Size > MaxTextPixelSize)
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
            FString::Printf(TEXT("size %d is out of range 1..%d"), Size, MaxTextPixelSize));
        return true;
    }
    const FString Align = Ctx.GetString(TEXT("align"), TEXT("Center"));
    const bool bAlignLeft = Align.Equals(TEXT("Left"), ESearchCase::IgnoreCase);
    const bool bAlignRight = Align.Equals(TEXT("Right"), ESearchCase::IgnoreCase);
    if (!bAlignLeft && !bAlignRight && !Align.Equals(TEXT("Center"), ESearchCase::IgnoreCase))
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
            FString::Printf(TEXT("Unknown align '%s'; expected Left, Center or Right"), *Align));
        return true;
    }

    // Resolve the font bytes: a UFontFace directly, or one typeface entry of a composite UFont.
    const FString FontPath = Ctx.GetString(TEXT("font"), DefaultTextFont);
    UObject* FontObject = LoadObject<UObject>(nullptr, *FontPath);
    if (!FontObject)
    {
        Ctx.SendError(ErrorCodes::ERR_ASSET_NOT_FOUND, FString::Printf(TEXT("Could not load font '%s'"), *FontPath));
        return true;
    }
    const FString Typeface = Ctx.GetString(TEXT("typeface"));
    FString ResolvedTypeface;
    FFontFaceDataConstPtr FaceData;
    FString FaceFilename;
    int32 SubFaceIndex = 0; // which face of a .ttc collection the typeface entry names
    if (const UFontFace* FontFace = Cast<UFontFace>(FontObject))
    {
        FaceData = FontFace->GetFontFaceData();
        FaceFilename = FontFace->GetFontFilename();
    }
    else if (const UFont* Font = Cast<UFont>(FontObject))
    {
        const FCompositeFont* Composite = Font->GetCompositeFont();
        if (!Composite || Composite->DefaultTypeface.Fonts.Num() == 0)
        {
            Ctx.SendError(ErrorCodes::ERR_INVALID_ASSET_TYPE, FString::Printf(
                TEXT("Font '%s' has no runtime typefaces (an offline/bitmap-cache font); pass a runtime UFont or a UFontFace"), *FontPath));
            return true;
        }
        const FTypefaceEntry* Entry = &Composite->DefaultTypeface.Fonts[0];
        if (!Typeface.IsEmpty())
        {
            Entry = Composite->DefaultTypeface.Fonts.FindByPredicate(
                [&Typeface](const FTypefaceEntry& E) { return E.Name.ToString().Equals(Typeface, ESearchCase::IgnoreCase); });
            if (!Entry)
            {
                TArray<FString> Names;
                for (const FTypefaceEntry& E : Composite->DefaultTypeface.Fonts)
                {
                    Names.Add(E.Name.ToString());
                }
                Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT, FString::Printf(
                    TEXT("Font '%s' has no typeface '%s'; available: %s"), *FontPath, *Typeface, *FString::Join(Names, TEXT(", "))));
                return true;
            }
        }
        ResolvedTypeface = Entry->Name.ToString();
        FaceData = Entry->Font.GetFontFaceData();
        FaceFilename = Entry->Font.GetFontFilename();
        SubFaceIndex = Entry->Font.GetSubFaceIndex();
    }
    else
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ASSET_TYPE, FString::Printf(
            TEXT("'%s' is a %s, not a UFont or UFontFace"), *FontPath, *FontObject->GetClass()->GetName()));
        return true;
    }
    if (!Typeface.IsEmpty() && ResolvedTypeface.IsEmpty())
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT, TEXT("typeface applies only to a composite UFont, not a UFontFace"));
        return true;
    }
    // Stream-policy faces keep no bytes in memory; read the backing file instead.
    TArray<uint8> FileBytes;
    const TArray<uint8>* FontBytes = (FaceData.IsValid() && FaceData->HasData()) ? &FaceData->GetData() : nullptr;
    if (!FontBytes && !FaceFilename.IsEmpty() && FFileHelper::LoadFileToArray(FileBytes, *FaceFilename))
    {
        FontBytes = &FileBytes;
    }
    if (!FontBytes || FontBytes->Num() == 0)
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ASSET_TYPE, FString::Printf(TEXT("Font '%s' carries no font data"), *FontPath));
        return true;
    }

    FScopedFreeTypeFace FT;
    if (FT_Init_FreeType(&FT.Library) != 0
        || FT_New_Memory_Face(FT.Library, FontBytes->GetData(), FontBytes->Num(), SubFaceIndex, &FT.Face) != 0
        || FT_Set_Pixel_Sizes(FT.Face, 0, Size) != 0)
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ASSET_TYPE, FString::Printf(TEXT("FreeType could not open font '%s' at size %d"), *FontPath, Size));
        return true;
    }
    const bool bKerning = FT_HAS_KERNING(FT.Face);
    const int32 Ascender = static_cast<int32>(FT.Face->size->metrics.ascender >> 6);
    const int32 LineHeight = FMath::Max(1, static_cast<int32>(FT.Face->size->metrics.height >> 6));

    // Layout pass: per-line glyph indices, pen x positions (26.6), advance width, and the ink's
    // horizontal pixel extent relative to the line origin (an italic overhang, a leading 'j' or any
    // negative bearing reaches past [0, advance), so the advance alone would clip it).
    // ponytail: no shaping (HarfBuzz) - fine for Latin/Cyrillic/Greek labels, wrong for complex scripts.
    struct FLineLayout
    {
        TArray<FT_UInt> Glyphs;
        TArray<int64> PenX;
        int64 Advance = 0;
        int32 InkMinPx = 0;
        int32 InkMaxPx = 0;
        bool bHasInk = false;
        // The line's box in pixels from its origin: the advance plus any ink outside it.
        int32 BoxLeftPx() const { return FMath::Min(0, InkMinPx); }
        int32 BoxWidthPx() const { return FMath::Max(static_cast<int32>((Advance + 63) >> 6), InkMaxPx) - BoxLeftPx(); }
    };
    TArray<FString> LineStrings;
    Text.ReplaceInline(TEXT("\r\n"), TEXT("\n"));
    Text.ParseIntoArray(LineStrings, TEXT("\n"), /*InCullEmpty=*/false);
    TArray<FLineLayout> Lines;
    TSet<FString> Missing;
    for (const FString& LineString : LineStrings)
    {
        FLineLayout& L = Lines.AddDefaulted_GetRef();
        FT_UInt Prev = 0;
        for (uint32 CodePoint : ToCodePoints(LineString))
        {
            const FT_UInt Glyph = FT_Get_Char_Index(FT.Face, CodePoint);
            if (Glyph == 0)
            {
                Missing.Add(FString::Printf(TEXT("U+%04X"), CodePoint));
            }
            if (bKerning && Prev && Glyph)
            {
                FT_Vector Kern;
                if (FT_Get_Kerning(FT.Face, Prev, Glyph, FT_KERNING_DEFAULT, &Kern) == 0)
                {
                    L.Advance += Kern.x;
                }
            }
            if (FT_Load_Glyph(FT.Face, Glyph, FT_LOAD_DEFAULT) == 0)
            {
                // Same pixel rounding the raster pass uses: pen floored, bitmap box floored/ceiled.
                const FT_Glyph_Metrics& M = FT.Face->glyph->metrics;
                if (M.width > 0)
                {
                    const int32 PenPx = static_cast<int32>(L.Advance >> 6);
                    const int32 InkL = PenPx + static_cast<int32>(M.horiBearingX >> 6);
                    const int32 InkR = PenPx + static_cast<int32>((M.horiBearingX + M.width + 63) >> 6);
                    L.InkMinPx = L.bHasInk ? FMath::Min(L.InkMinPx, InkL) : InkL;
                    L.InkMaxPx = L.bHasInk ? FMath::Max(L.InkMaxPx, InkR) : InkR;
                    L.bHasInk = true;
                }
                L.Glyphs.Add(Glyph);
                L.PenX.Add(L.Advance);
                L.Advance += FT.Face->glyph->advance.x;
            }
            Prev = Glyph;
        }
    }
    int32 MaxBoxWidth = 0;
    for (const FLineLayout& L : Lines)
    {
        MaxBoxWidth = FMath::Max(MaxBoxWidth, L.BoxWidthPx());
    }
    const int32 BlockHeight = Lines.Num() * LineHeight;
    const TSharedPtr<FJsonObject>& Raw = Ctx.GetRawPayload();
    const int32 Width = (Raw.IsValid() && Raw->HasField(TEXT("width")))
        ? Ctx.GetInt(TEXT("width")) : FMath::Max(1, MaxBoxWidth);
    const int32 Height = (Raw.IsValid() && Raw->HasField(TEXT("height")))
        ? Ctx.GetInt(TEXT("height")) : FMath::Max(1, BlockHeight);
    if (!CheckAuthoredSize(Ctx, Width, Height))
    {
        return true;
    }

    // Raster pass: accumulate 8-bit glyph coverage (max where glyphs overlap); count coverage
    // that fell outside the canvas so clipping is a measurement, not a layout guess.
    TArray<uint8> Coverage;
    Coverage.SetNumZeroed(Width * Height);
    int64 ClippedPixels = 0;
    const int32 BlockTop = (Height - BlockHeight) / 2;
    for (int32 LineIndex = 0; LineIndex < Lines.Num(); ++LineIndex)
    {
        const FLineLayout& L = Lines[LineIndex];
        const int32 BoxWidthPx = L.BoxWidthPx();
        const int32 BoxX = bAlignLeft ? 0 : (bAlignRight ? Width - BoxWidthPx : (Width - BoxWidthPx) / 2);
        const int32 OriginX = BoxX - L.BoxLeftPx();
        const int32 Baseline = BlockTop + LineIndex * LineHeight + Ascender;
        for (int32 g = 0; g < L.Glyphs.Num(); ++g)
        {
            if (FT_Load_Glyph(FT.Face, L.Glyphs[g], FT_LOAD_RENDER) != 0)
            {
                continue;
            }
            const FT_GlyphSlot Slot = FT.Face->glyph;
            const FT_Bitmap& Bmp = Slot->bitmap;
            if (Bmp.pixel_mode != FT_PIXEL_MODE_GRAY)
            {
                continue;
            }
            const int32 X0 = OriginX + static_cast<int32>(L.PenX[g] >> 6) + Slot->bitmap_left;
            const int32 Y0 = Baseline - Slot->bitmap_top;
            for (uint32 Row = 0; Row < Bmp.rows; ++Row)
            {
                const uint8* Src = Bmp.buffer + static_cast<int64>(Row) * Bmp.pitch;
                for (uint32 Col = 0; Col < Bmp.width; ++Col)
                {
                    const uint8 C = Src[Col];
                    if (C == 0)
                    {
                        continue;
                    }
                    const int32 X = X0 + static_cast<int32>(Col);
                    const int32 Y = Y0 + static_cast<int32>(Row);
                    if (X < 0 || Y < 0 || X >= Width || Y >= Height)
                    {
                        ++ClippedPixels;
                        continue;
                    }
                    uint8& Dst = Coverage[Y * Width + X];
                    Dst = FMath::Max(Dst, C);
                }
            }
        }
    }

    const FLinearColor Fg = ReadColor(Ctx.GetObject(TEXT("color")), FLinearColor(1, 1, 1, 1));
    const FLinearColor Bg = ReadColor(Ctx.GetObject(TEXT("backgroundColor")), FLinearColor(0, 0, 0, 0));
    TArray<FColor> Pixels;
    Pixels.SetNumUninitialized(Width * Height);
    int64 InkPixels = 0;
    int32 MinX = Width, MinY = Height, MaxX = -1, MaxY = -1;
    for (int32 Y = 0; Y < Height; ++Y)
    {
        for (int32 X = 0; X < Width; ++X)
        {
            const uint8 C = Coverage[Y * Width + X];
            Pixels[Y * Width + X] = CompositeTextPixel(Fg, Bg, C / 255.0f);
            if (C > 0)
            {
                ++InkPixels;
                MinX = FMath::Min(MinX, X);
                MinY = FMath::Min(MinY, Y);
                MaxX = FMath::Max(MaxX, X);
                MaxY = FMath::Max(MaxY, Y);
            }
        }
    }

    TSharedPtr<FJsonObject> Response = MakeShared<FJsonObject>();
    Response->SetStringField(TEXT("font"), FontObject->GetPathName());
    if (!ResolvedTypeface.IsEmpty())
    {
        Response->SetStringField(TEXT("typeface"), ResolvedTypeface);
    }
    Response->SetNumberField(TEXT("size"), Size);
    Response->SetNumberField(TEXT("lines"), Lines.Num());
    Response->SetNumberField(TEXT("inkPixels"), static_cast<double>(InkPixels));
    if (InkPixels > 0)
    {
        TSharedPtr<FJsonObject> Ink = MakeShared<FJsonObject>();
        Ink->SetNumberField(TEXT("x"), MinX);
        Ink->SetNumberField(TEXT("y"), MinY);
        Ink->SetNumberField(TEXT("width"), MaxX - MinX + 1);
        Ink->SetNumberField(TEXT("height"), MaxY - MinY + 1);
        Response->SetObjectField(TEXT("inkBounds"), Ink);
    }
    Response->SetBoolField(TEXT("clipped"), ClippedPixels > 0);
    Response->SetNumberField(TEXT("clippedPixels"), static_cast<double>(ClippedPixels));
    TArray<TSharedPtr<FJsonValue>> MissingValues;
    for (const FString& M : Missing)
    {
        MissingValues.Add(MakeShared<FJsonValueString>(M));
    }
    Response->SetArrayField(TEXT("missingCharacters"), MissingValues);
    return CreateAndSendAuthoredTexture(Ctx, Name, Ctx.GetString(TEXT("path"), TEXT("/Game/Textures")),
        Width, Height, Pixels, Ctx.GetBool(TEXT("save"), true), Response);
}
