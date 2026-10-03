// Copyright (c) 2026 Alexander Penkin. MIT License.

// texture.create_from_pixels / texture.create_text_texture (board
// F-texture-cannot-author-letterforms-or-pixel-data). Every assertion reads the editable source
// mip the verb wrote - the buffer a material samples from after the platform build - so a verb
// that reported success over the wrong pixels, a forced backing plate, or a placeholder shape
// fails here rather than passing on "an asset was produced".

#include "Misc/AutomationTest.h"
#include "Misc/Base64.h"
#include "Engine/Texture2D.h"

#include "Handlers/ErrorCodes.h"
#include "Tests/TestUtils.h"

namespace TextureAuthorVerbsTestHelpers
{
    static const TCHAR* const AuthorTestFolder = TEXT("/Game/PinWrightTests/Texture");

    struct FAuthoredImage
    {
        int32 Width = 0;
        int32 Height = 0;
        TArray<FColor> Pixels; // row-major from the top-left

        const FColor& At(int32 X, int32 Y) const { return Pixels[Y * Width + X]; }
    };

    FString MakeAuthorAssetName(const TCHAR* Prefix)
    {
        return FString::Printf(TEXT("T_%s_%s"), Prefix, *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    }

    TSharedPtr<FJsonObject> MakeAuthorPayload(const FString& AssetName)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("name"), AssetName);
        Payload->SetStringField(TEXT("path"), AuthorTestFolder);
        Payload->SetBoolField(TEXT("save"), false);
        return Payload;
    }

    TSharedPtr<FJsonObject> MakeAuthorColor(float R, float G, float B, float A)
    {
        TSharedPtr<FJsonObject> C = MakeShared<FJsonObject>();
        C->SetNumberField(TEXT("r"), R);
        C->SetNumberField(TEXT("g"), G);
        C->SetNumberField(TEXT("b"), B);
        C->SetNumberField(TEXT("a"), A);
        return C;
    }

    UTexture2D* FindAuthoredTexture(const FString& AssetName)
    {
        const FString PackagePath = FString::Printf(TEXT("%s/%s"), AuthorTestFolder, *AssetName);
        return Cast<UTexture2D>(StaticFindObject(UTexture2D::StaticClass(), nullptr, *ToObjectPath(PackagePath)));
    }

    // save:false wrote nothing to disk; clearing the dirty flag is the whole teardown.
    void DeDirtyAuthoredTexture(const FString& AssetName)
    {
        if (UTexture2D* Created = FindAuthoredTexture(AssetName))
        {
            Created->GetOutermost()->SetDirtyFlag(false);
        }
    }

    bool ReadAuthoredImage(UTexture2D* Texture, FAuthoredImage& Out)
    {
        FTextureSource& Source = Texture->Source;
        if (!Source.IsValid() || Source.GetFormat() != TSF_BGRA8)
        {
            return false;
        }
        Out.Width = static_cast<int32>(Source.GetSizeX());
        Out.Height = static_cast<int32>(Source.GetSizeY());
        const uint8* Data = Source.LockMipReadOnly(0);
        if (!Data)
        {
            return false;
        }
        Out.Pixels.SetNumUninitialized(Out.Width * Out.Height);
        FMemory::Memcpy(Out.Pixels.GetData(), Data, Out.Pixels.Num() * sizeof(FColor));
        Source.UnlockMip(0);
        return true;
    }

    // Runs the verb, asserts success, and reads back what it wrote. Dedirties on every path.
    bool RunAuthorVerb(FAutomationTestBase& Test, const TCHAR* Method, const FString& AssetName,
        const TSharedPtr<FJsonObject>& Payload, FAuthoredImage& OutImage, TSharedPtr<FJsonObject>* OutResult = nullptr)
    {
        FTestResponseCapture Capture;
        const bool bFound = InvokeHandlerWithCapture(Method, Payload, Capture);
        Test.TestTrue(FString::Printf(TEXT("%s is registered"), Method), bFound);
        const bool bOk = Test.TestTrue(FString::Printf(TEXT("%s succeeded (%s: %s)"), Method,
            *Capture.ErrorCode, *Capture.Message), Capture.bWasCalled && Capture.bSuccess);
        if (OutResult)
        {
            *OutResult = Capture.Result;
        }
        UTexture2D* Created = FindAuthoredTexture(AssetName);
        const bool bRead = bOk && Test.TestNotNull(TEXT("the created texture is findable"), Created)
            && Test.TestTrue(TEXT("the created texture's BGRA8 source mip is readable"), ReadAuthoredImage(Created, OutImage));
        DeDirtyAuthoredTexture(AssetName);
        return bRead;
    }

    // Runs the verb and asserts a refusal with the given code and that no asset was created.
    void ExpectAuthorRefusal(FAutomationTestBase& Test, const TCHAR* Method, const FString& AssetName,
        const TSharedPtr<FJsonObject>& Payload, const TCHAR* ExpectedCode, const TCHAR* What)
    {
        FTestResponseCapture Capture;
        InvokeHandlerWithCapture(Method, Payload, Capture);
        Test.TestTrue(FString::Printf(TEXT("%s is refused"), What), Capture.bWasCalled && !Capture.bSuccess);
        Test.TestEqual(FString::Printf(TEXT("%s error code"), What), Capture.ErrorCode, FString(ExpectedCode));
        Test.TestNull(FString::Printf(TEXT("%s creates no asset"), What), FindAuthoredTexture(AssetName));
    }

    // Alpha-ink bounding box of an image (alpha > 0); false when nothing is inked.
    bool MeasureInk(const FAuthoredImage& Image, FIntRect& OutBox, int32& OutCount)
    {
        OutBox = FIntRect(Image.Width, Image.Height, -1, -1);
        OutCount = 0;
        for (int32 Y = 0; Y < Image.Height; ++Y)
        {
            for (int32 X = 0; X < Image.Width; ++X)
            {
                if (Image.At(X, Y).A > 0)
                {
                    ++OutCount;
                    OutBox.Min.X = FMath::Min(OutBox.Min.X, X);
                    OutBox.Min.Y = FMath::Min(OutBox.Min.Y, Y);
                    OutBox.Max.X = FMath::Max(OutBox.Max.X, X);
                    OutBox.Max.Y = FMath::Max(OutBox.Max.Y, Y);
                }
            }
        }
        return OutCount > 0;
    }

    // Fraction of pixels with alpha >= 128 inside [X0,X1)x[Y0,Y1).
    float SolidFraction(const FAuthoredImage& Image, int32 X0, int32 Y0, int32 X1, int32 Y1)
    {
        int32 Solid = 0;
        int32 Total = 0;
        for (int32 Y = Y0; Y < Y1; ++Y)
        {
            for (int32 X = X0; X < X1; ++X)
            {
                ++Total;
                Solid += Image.At(X, Y).A >= 128 ? 1 : 0;
            }
        }
        return Total > 0 ? static_cast<float>(Solid) / Total : 0.0f;
    }
}

// Every caller byte lands at its own pixel, in order, for both layouts.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTextureCreateFromPixelsWritesCallerBytesTest,
    "PinWright.texture.create_from_pixels.WritesCallerBytesExactly",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FTextureCreateFromPixelsWritesCallerBytesTest::RunTest(const FString& Parameters)
{
    using namespace TextureAuthorVerbsTestHelpers;

    // 5x3 RGBA8 with every channel of every pixel distinct, so a transposed, row-shifted or
    // channel-swapped write cannot coincide with the input anywhere.
    const int32 W = 5;
    const int32 H = 3;
    TArray<uint8> Rgba;
    for (int32 i = 0; i < W * H; ++i)
    {
        Rgba.Add(static_cast<uint8>(10 + i));       // R
        Rgba.Add(static_cast<uint8>(60 + i * 2));   // G
        Rgba.Add(static_cast<uint8>(130 + i * 3));  // B
        Rgba.Add(static_cast<uint8>(255 - i * 7));  // A
    }
    const FString RgbaName = MakeAuthorAssetName(TEXT("PixelsRgba"));
    TSharedPtr<FJsonObject> RgbaPayload = MakeAuthorPayload(RgbaName);
    RgbaPayload->SetNumberField(TEXT("width"), W);
    RgbaPayload->SetNumberField(TEXT("height"), H);
    RgbaPayload->SetStringField(TEXT("data"), FBase64::Encode(Rgba));
    FAuthoredImage RgbaImage;
    if (RunAuthorVerb(*this, TEXT("texture.create_from_pixels"), RgbaName, RgbaPayload, RgbaImage))
    {
        TestEqual(TEXT("RGBA8 width"), RgbaImage.Width, W);
        TestEqual(TEXT("RGBA8 height"), RgbaImage.Height, H);
        int32 Mismatches = 0;
        for (int32 i = 0; i < W * H && RgbaImage.Pixels.Num() == W * H; ++i)
        {
            const FColor Expected(Rgba[i * 4 + 0], Rgba[i * 4 + 1], Rgba[i * 4 + 2], Rgba[i * 4 + 3]);
            Mismatches += RgbaImage.Pixels[i] == Expected ? 0 : 1;
        }
        TestEqual(TEXT("every RGBA8 pixel equals the caller's bytes"), Mismatches, 0);
    }

    // Gray8: one byte per pixel, written as R=G=B with opaque alpha.
    const TArray<uint8> Gray = { 0, 64, 128, 255, 17, 200 };
    const FString GrayName = MakeAuthorAssetName(TEXT("PixelsGray"));
    TSharedPtr<FJsonObject> GrayPayload = MakeAuthorPayload(GrayName);
    GrayPayload->SetNumberField(TEXT("width"), 3);
    GrayPayload->SetNumberField(TEXT("height"), 2);
    GrayPayload->SetStringField(TEXT("format"), TEXT("Gray8"));
    GrayPayload->SetStringField(TEXT("data"), FBase64::Encode(Gray));
    FAuthoredImage GrayImage;
    if (RunAuthorVerb(*this, TEXT("texture.create_from_pixels"), GrayName, GrayPayload, GrayImage)
        && TestEqual(TEXT("Gray8 pixel count"), GrayImage.Pixels.Num(), Gray.Num()))
    {
        int32 Mismatches = 0;
        for (int32 i = 0; i < Gray.Num(); ++i)
        {
            Mismatches += GrayImage.Pixels[i] == FColor(Gray[i], Gray[i], Gray[i], 255) ? 0 : 1;
        }
        TestEqual(TEXT("every Gray8 pixel is R=G=B=byte with alpha 255"), Mismatches, 0);
    }
    return true;
}

// A byte count that does not match width*height*bpp, an unknown format, an out-of-range size and
// an already-existing asset are each refused without creating (or replacing) anything.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTextureCreateFromPixelsRefusesBadInputTest,
    "PinWright.texture.create_from_pixels.RefusesMismatchedOrUnknownInput",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FTextureCreateFromPixelsRefusesBadInputTest::RunTest(const FString& Parameters)
{
    using namespace TextureAuthorVerbsTestHelpers;

    const TArray<uint8> FifteenBytes = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 };
    {
        const FString Name = MakeAuthorAssetName(TEXT("PixelsShort"));
        TSharedPtr<FJsonObject> P = MakeAuthorPayload(Name);
        P->SetNumberField(TEXT("width"), 2);
        P->SetNumberField(TEXT("height"), 2);
        P->SetStringField(TEXT("data"), FBase64::Encode(FifteenBytes)); // 2x2 RGBA8 needs 16
        ExpectAuthorRefusal(*this, TEXT("texture.create_from_pixels"), Name, P, ErrorCodes::ERR_INVALID_ARGUMENT, TEXT("a 15-byte 2x2 RGBA8"));
    }
    {
        const FString Name = MakeAuthorAssetName(TEXT("PixelsFormat"));
        TSharedPtr<FJsonObject> P = MakeAuthorPayload(Name);
        P->SetNumberField(TEXT("width"), 5);
        P->SetNumberField(TEXT("height"), 1);
        P->SetStringField(TEXT("format"), TEXT("RGB8"));
        P->SetStringField(TEXT("data"), FBase64::Encode(FifteenBytes));
        ExpectAuthorRefusal(*this, TEXT("texture.create_from_pixels"), Name, P, ErrorCodes::ERR_INVALID_ARGUMENT, TEXT("format RGB8"));
    }
    {
        const FString Name = MakeAuthorAssetName(TEXT("PixelsHuge"));
        TSharedPtr<FJsonObject> P = MakeAuthorPayload(Name);
        P->SetNumberField(TEXT("width"), 5000);
        P->SetNumberField(TEXT("height"), 1);
        P->SetStringField(TEXT("data"), FBase64::Encode(FifteenBytes));
        ExpectAuthorRefusal(*this, TEXT("texture.create_from_pixels"), Name, P, ErrorCodes::ERR_INVALID_ARGUMENT, TEXT("width 5000"));
    }
    {
        const FString Name = MakeAuthorAssetName(TEXT("PixelsB64"));
        TSharedPtr<FJsonObject> P = MakeAuthorPayload(Name);
        P->SetNumberField(TEXT("width"), 1);
        P->SetNumberField(TEXT("height"), 1);
        P->SetStringField(TEXT("data"), TEXT("@@not*base64@@"));
        ExpectAuthorRefusal(*this, TEXT("texture.create_from_pixels"), Name, P, ErrorCodes::ERR_DECODE_FAILED, TEXT("non-base64 data"));
    }
    {
        // Create once, then ask again under the same name: the second call must refuse and leave
        // the first texture's pixels untouched.
        const FString Name = MakeAuthorAssetName(TEXT("PixelsDup"));
        const TArray<uint8> First = { 9, 9, 9, 9 };
        TSharedPtr<FJsonObject> P = MakeAuthorPayload(Name);
        P->SetNumberField(TEXT("width"), 1);
        P->SetNumberField(TEXT("height"), 1);
        P->SetStringField(TEXT("data"), FBase64::Encode(First));
        FAuthoredImage Image;
        RunAuthorVerb(*this, TEXT("texture.create_from_pixels"), Name, P, Image);

        const TArray<uint8> Second = { 200, 200, 200, 200 };
        P->SetStringField(TEXT("data"), FBase64::Encode(Second));
        FTestResponseCapture Capture;
        InvokeHandlerWithCapture(TEXT("texture.create_from_pixels"), P, Capture);
        TestTrue(TEXT("a second create under an existing name is refused"), Capture.bWasCalled && !Capture.bSuccess);
        TestEqual(TEXT("existing-name refusal code"), Capture.ErrorCode, FString(ErrorCodes::ERR_ASSET_ALREADY_EXISTS));
        FAuthoredImage After;
        UTexture2D* Existing = FindAuthoredTexture(Name);
        if (TestNotNull(TEXT("the first texture still exists after the refused second create"), Existing))
        {
            if (TestTrue(TEXT("the existing texture is still readable"), ReadAuthoredImage(Existing, After)))
            {
                TestEqual(TEXT("the existing texture keeps its first pixel"), After.At(0, 0), FColor(9, 9, 9, 9));
            }
        }
        DeDirtyAuthoredTexture(Name);
    }
    return true;
}

// Real letterforms on a genuinely transparent background: an "L" drawn in the engine's default
// font must have ink down its left edge and along its bottom, nothing in its upper-right, the
// requested colour on every inked pixel, and zero alpha everywhere off the glyph (no plate).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTextureCreateTextTextureDrawsLetterformTest,
    "PinWright.texture.create_text_texture.DrawsLetterformOnTransparentBackground",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FTextureCreateTextTextureDrawsLetterformTest::RunTest(const FString& Parameters)
{
    using namespace TextureAuthorVerbsTestHelpers;

    const FString Name = MakeAuthorAssetName(TEXT("TextL"));
    TSharedPtr<FJsonObject> P = MakeAuthorPayload(Name);
    P->SetStringField(TEXT("text"), TEXT("L"));
    P->SetNumberField(TEXT("size"), 96);
    P->SetObjectField(TEXT("color"), MakeAuthorColor(1.0f, 0.0f, 0.0f, 1.0f));
    FAuthoredImage Image;
    TSharedPtr<FJsonObject> Result;
    if (!RunAuthorVerb(*this, TEXT("texture.create_text_texture"), Name, P, Image, &Result))
    {
        return false;
    }

    FIntRect Ink;
    int32 InkCount = 0;
    if (!TestTrue(TEXT("the glyph inked some pixels"), MeasureInk(Image, Ink, InkCount)))
    {
        return false;
    }
    const int32 InkW = Ink.Max.X - Ink.Min.X + 1;
    const int32 InkH = Ink.Max.Y - Ink.Min.Y + 1;
    AddInfo(FString::Printf(TEXT("'L' at 96px: canvas %dx%d, ink box (%d,%d) %dx%d, %d inked pixels"),
        Image.Width, Image.Height, Ink.Min.X, Ink.Min.Y, InkW, InkH, InkCount));
    TestTrue(TEXT("an L at 96px is a real-size glyph (ink taller than 40px), not a 3x5 cell"), InkH > 40);
    TestTrue(TEXT("an L is taller than it is wide"), InkH > InkW);

    // Shape: the left eighth of the ink box is solid down the whole height, the bottom eighth is
    // solid across the whole width, and the upper-right quadrant is empty.
    const int32 StemW = FMath::Max(1, InkW / 8);
    const int32 FootH = FMath::Max(1, InkH / 8);
    TestTrue(TEXT("the L's stem (left edge) is solid ink"),
        SolidFraction(Image, Ink.Min.X, Ink.Min.Y, Ink.Min.X + StemW, Ink.Max.Y + 1) > 0.75f);
    TestTrue(TEXT("the L's foot (bottom edge) is solid ink"),
        SolidFraction(Image, Ink.Min.X, Ink.Max.Y + 1 - FootH, Ink.Max.X + 1, Ink.Max.Y + 1) > 0.75f);
    TestTrue(TEXT("the L's upper-right quadrant is empty"),
        SolidFraction(Image, Ink.Min.X + InkW / 2, Ink.Min.Y, Ink.Max.X + 1, Ink.Min.Y + InkH / 2) < 0.02f);

    // Transparent background, no plate, colour carried on every inked pixel.
    int32 ZeroAlpha = 0;
    int32 WrongColour = 0;
    for (const FColor& C : Image.Pixels)
    {
        ZeroAlpha += C.A == 0 ? 1 : 0;
        WrongColour += (C.A > 0 && (C.R != 255 || C.G != 0 || C.B != 0)) ? 1 : 0;
    }
    TestEqual(TEXT("every inked pixel carries the requested red"), WrongColour, 0);
    TestTrue(TEXT("most of the canvas is fully transparent (no backing plate)"), ZeroAlpha > Image.Pixels.Num() / 2);
    TestEqual(TEXT("the top-right canvas corner is fully transparent"), static_cast<int32>(Image.At(Image.Width - 1, 0).A), 0);

    // The response's own measurement agrees with the readback.
    if (TestTrue(TEXT("the response carries a result"), Result.IsValid()))
    {
        TestEqual(TEXT("response inkPixels matches readback"), static_cast<int32>(Result->GetNumberField(TEXT("inkPixels"))), InkCount);
        TestFalse(TEXT("an auto-sized single glyph is not clipped"), Result->GetBoolField(TEXT("clipped")));
        const TSharedPtr<FJsonObject>* InkObj = nullptr;
        if (TestTrue(TEXT("response carries inkBounds"), Result->TryGetObjectField(TEXT("inkBounds"), InkObj)))
        {
            TestEqual(TEXT("inkBounds.width matches readback"), static_cast<int32>((*InkObj)->GetNumberField(TEXT("width"))), InkW);
            TestEqual(TEXT("inkBounds.height matches readback"), static_cast<int32>((*InkObj)->GetNumberField(TEXT("height"))), InkH);
        }
    }
    return true;
}

// typeface, align, backgroundColor and an explicit canvas each reach the raster: Bold inks more
// than Regular for the same text, Left/Right move the ink to opposite sides, an opaque background
// fills every non-glyph pixel, text wider than the canvas reports clipped, and an unknown typeface
// is refused.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTextureCreateTextTextureOptionsReachRasterTest,
    "PinWright.texture.create_text_texture.OptionsReachTheRaster",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FTextureCreateTextTextureOptionsReachRasterTest::RunTest(const FString& Parameters)
{
    using namespace TextureAuthorVerbsTestHelpers;

    auto Render = [this](const TCHAR* Prefix, TFunctionRef<void(FJsonObject&)> Configure, FAuthoredImage& Out,
        TSharedPtr<FJsonObject>* OutResult = nullptr)
    {
        const FString Name = MakeAuthorAssetName(Prefix);
        TSharedPtr<FJsonObject> P = MakeAuthorPayload(Name);
        P->SetStringField(TEXT("text"), TEXT("ABC"));
        P->SetNumberField(TEXT("size"), 48);
        Configure(*P);
        return RunAuthorVerb(*this, TEXT("texture.create_text_texture"), Name, P, Out, OutResult);
    };

    FAuthoredImage Regular, Bold;
    FIntRect Box;
    int32 RegularInk = 0, BoldInk = 0;
    if (Render(TEXT("TextRegular"), [](FJsonObject& P) { P.SetStringField(TEXT("typeface"), TEXT("Regular")); }, Regular)
        && Render(TEXT("TextBold"), [](FJsonObject& P) { P.SetStringField(TEXT("typeface"), TEXT("Bold")); }, Bold))
    {
        MeasureInk(Regular, Box, RegularInk);
        MeasureInk(Bold, Box, BoldInk);
        AddInfo(FString::Printf(TEXT("'ABC' 48px inked pixels: Regular %d, Bold %d"), RegularInk, BoldInk));
        TestTrue(TEXT("Bold inks clearly more than Regular (typeface reaches the rasteriser)"),
            BoldInk > RegularInk + RegularInk / 10);
    }

    FAuthoredImage Left, Right;
    FIntRect LeftBox, RightBox;
    int32 Unused = 0;
    auto Wide = [](const TCHAR* Align)
    {
        return [Align](FJsonObject& P)
        {
            P.SetNumberField(TEXT("width"), 512);
            P.SetNumberField(TEXT("height"), 96);
            P.SetStringField(TEXT("align"), Align);
        };
    };
    if (Render(TEXT("TextLeft"), Wide(TEXT("Left")), Left) && Render(TEXT("TextRight"), Wide(TEXT("Right")), Right)
        && MeasureInk(Left, LeftBox, Unused) && MeasureInk(Right, RightBox, Unused))
    {
        TestEqual(TEXT("explicit width is honoured"), Left.Width, 512);
        TestTrue(TEXT("align Left puts the ink in the left quarter"), LeftBox.Max.X < 512 / 4);
        TestTrue(TEXT("align Right puts the ink in the right quarter"), RightBox.Min.X > 512 * 3 / 4);
    }

    FAuthoredImage Plate;
    TSharedPtr<FJsonObject> PlateResult;
    if (Render(TEXT("TextPlate"), [](FJsonObject& P)
        {
            P.SetObjectField(TEXT("backgroundColor"), TextureAuthorVerbsTestHelpers::MakeAuthorColor(0.0f, 0.0f, 1.0f, 1.0f));
        }, Plate, &PlateResult))
    {
        TestEqual(TEXT("an opaque background fills the corner"), Plate.At(0, 0), FColor(0, 0, 255, 255));
        int32 NonOpaque = 0;
        for (const FColor& C : Plate.Pixels)
        {
            NonOpaque += C.A == 255 ? 0 : 1;
        }
        TestEqual(TEXT("an opaque background leaves no transparent pixel"), NonOpaque, 0);
    }

    FAuthoredImage Clipped;
    TSharedPtr<FJsonObject> ClippedResult;
    if (Render(TEXT("TextClip"), [](FJsonObject& P) { P.SetNumberField(TEXT("width"), 20); }, Clipped, &ClippedResult)
        && TestTrue(TEXT("clip result present"), ClippedResult.IsValid()))
    {
        TestTrue(TEXT("text wider than a 20px canvas reports clipped"), ClippedResult->GetBoolField(TEXT("clipped")));
        TestTrue(TEXT("clippedPixels counts the lost coverage"), ClippedResult->GetNumberField(TEXT("clippedPixels")) > 0);
    }

    const FString BadName = MakeAuthorAssetName(TEXT("TextBadFace"));
    TSharedPtr<FJsonObject> Bad = MakeAuthorPayload(BadName);
    Bad->SetStringField(TEXT("text"), TEXT("A"));
    Bad->SetStringField(TEXT("typeface"), TEXT("NoSuchTypeface"));
    ExpectAuthorRefusal(*this, TEXT("texture.create_text_texture"), BadName, Bad, ErrorCodes::ERR_INVALID_ARGUMENT, TEXT("an unknown typeface"));
    return true;
}

// Review fixes: auto-size fits the INK, not just the advance (an italic overhang and a leading
// 'j' with a negative left bearing reach outside [0, advance) and were clipped); a text longer
// than the cap is refused; an existing name is refused; missing glyphs are listed; '\n' stacks
// lines.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTextureCreateTextTextureFitsInkAndBoundsInputTest,
    "PinWright.texture.create_text_texture.AutoSizeFitsInkAndBoundsInput",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FTextureCreateTextTextureFitsInkAndBoundsInputTest::RunTest(const FString& Parameters)
{
    using namespace TextureAuthorVerbsTestHelpers;

    // Roboto Italic 'f' overhangs its advance on the right; 'j' hangs left of its pen. Neither may
    // lose coverage off an auto-sized canvas.
    struct FOverhangCase { const TCHAR* Prefix; const TCHAR* Text; const TCHAR* Typeface; };
    const FOverhangCase Cases[] = {
        { TEXT("TextItalicF"), TEXT("ff"), TEXT("Italic") },
        { TEXT("TextLeadingJ"), TEXT("j"), TEXT("Regular") },
        { TEXT("TextItalicJf"), TEXT("jf"), TEXT("Bold Italic") },
    };
    for (const FOverhangCase& Case : Cases)
    {
        const FString Name = MakeAuthorAssetName(Case.Prefix);
        TSharedPtr<FJsonObject> P = MakeAuthorPayload(Name);
        P->SetStringField(TEXT("text"), Case.Text);
        P->SetStringField(TEXT("typeface"), Case.Typeface);
        P->SetNumberField(TEXT("size"), 128);
        FAuthoredImage Image;
        TSharedPtr<FJsonObject> Result;
        if (RunAuthorVerb(*this, TEXT("texture.create_text_texture"), Name, P, Image, &Result)
            && TestTrue(TEXT("overhang result present"), Result.IsValid()))
        {
            AddInfo(FString::Printf(TEXT("'%s' %s 128px: canvas %dx%d, clippedPixels %d"), Case.Text, Case.Typeface,
                Image.Width, Image.Height, static_cast<int32>(Result->GetNumberField(TEXT("clippedPixels")))));
            TestFalse(FString::Printf(TEXT("auto-sized '%s' (%s) loses no coverage off the canvas"), Case.Text, Case.Typeface),
                Result->GetBoolField(TEXT("clipped")));
        }
    }

    {
        const FString Name = MakeAuthorAssetName(TEXT("TextTooLong"));
        TSharedPtr<FJsonObject> P = MakeAuthorPayload(Name);
        P->SetStringField(TEXT("text"), FString::ChrN(4097, TEXT('A')));
        // A fixed small canvas keeps the auto-size range check from refusing first, so only the
        // text cap can produce this refusal; its message names the cap.
        P->SetNumberField(TEXT("width"), 64);
        P->SetNumberField(TEXT("height"), 64);
        FTestResponseCapture Capture;
        InvokeHandlerWithCapture(TEXT("texture.create_text_texture"), P, Capture);
        TestTrue(TEXT("a 4097-character text is refused"), Capture.bWasCalled && !Capture.bSuccess);
        TestEqual(TEXT("a 4097-character text error code"), Capture.ErrorCode, FString(ErrorCodes::ERR_INVALID_ARGUMENT));
        TestTrue(FString::Printf(TEXT("the refusal names the 4096-character cap (got: %s)"), *Capture.Message),
            Capture.Message.Contains(TEXT("4096")));
        TestNull(TEXT("a 4097-character text creates no asset"), FindAuthoredTexture(Name));
    }

    {
        const FString Name = MakeAuthorAssetName(TEXT("TextDup"));
        TSharedPtr<FJsonObject> P = MakeAuthorPayload(Name);
        P->SetStringField(TEXT("text"), TEXT("A"));
        FAuthoredImage First;
        RunAuthorVerb(*this, TEXT("texture.create_text_texture"), Name, P, First);
        FTestResponseCapture Capture;
        InvokeHandlerWithCapture(TEXT("texture.create_text_texture"), P, Capture);
        TestTrue(TEXT("a second create_text_texture under an existing name is refused"), Capture.bWasCalled && !Capture.bSuccess);
        TestEqual(TEXT("create_text_texture existing-name refusal code"), Capture.ErrorCode, FString(ErrorCodes::ERR_ASSET_ALREADY_EXISTS));
        DeDirtyAuthoredTexture(Name);
    }

    {
        // U+E000 (private use) has no glyph in Roboto; two lines stack to twice one line's height.
        const FString OneName = MakeAuthorAssetName(TEXT("TextOneLine"));
        TSharedPtr<FJsonObject> One = MakeAuthorPayload(OneName);
        One->SetStringField(TEXT("text"), TEXT("AB"));
        const FString TwoName = MakeAuthorAssetName(TEXT("TextTwoLines"));
        TSharedPtr<FJsonObject> Two = MakeAuthorPayload(TwoName);
        FString TwoText = TEXT("AB\n");
        TwoText.AppendChar(static_cast<TCHAR>(0xE000));
        Two->SetStringField(TEXT("text"), TwoText);
        FAuthoredImage OneImage, TwoImage;
        TSharedPtr<FJsonObject> TwoResult;
        if (RunAuthorVerb(*this, TEXT("texture.create_text_texture"), OneName, One, OneImage)
            && RunAuthorVerb(*this, TEXT("texture.create_text_texture"), TwoName, Two, TwoImage, &TwoResult)
            && TestTrue(TEXT("two-line result present"), TwoResult.IsValid()))
        {
            TestEqual(TEXT("two lines are reported"), static_cast<int32>(TwoResult->GetNumberField(TEXT("lines"))), 2);
            TestEqual(TEXT("two lines auto-size to twice one line's height"), TwoImage.Height, OneImage.Height * 2);
            const TArray<TSharedPtr<FJsonValue>>* Missing = nullptr;
            if (TestTrue(TEXT("missingCharacters is present"), TwoResult->TryGetArrayField(TEXT("missingCharacters"), Missing)))
            {
                TestEqual(TEXT("one character is missing"), Missing->Num(), 1);
                TestEqual(TEXT("the missing character is U+E000"), Missing->Num() == 1 ? (*Missing)[0]->AsString() : FString(), FString(TEXT("U+E000")));
            }
        }
    }
    return true;
}
