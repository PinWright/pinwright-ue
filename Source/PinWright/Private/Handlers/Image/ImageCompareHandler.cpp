// Copyright (c) 2026 Alexander Penkin. MIT License.

// image.compare - paired output for two images: a side-by-side and a difference composite, plus
// the measurements a reader needs to interpret them.
//
// IT REPORTS; IT DOES NOT JUDGE. There is no threshold, no verdict, no "match" score. The
// difference composite is a per-channel absolute difference with an optional integer-ish gain, and
// every number beside it is a plain statistic. A verb that decided "these are the same" would be
// wrong quietly, which is worse than not existing.
//
// It refuses rather than comparing incomparable things:
//   * different pixel dimensions -> IMAGE_SIZE_MISMATCH (no pixel of one corresponds to a pixel of
//     the other, so a difference image would be a picture of the misalignment);
//   * georeferences that describe different ground -> GEOREFERENCE_MISMATCH.
// Those are separate codes because the recoveries differ: re-capture at the same resolution, vs.
// re-derive the georeference. Two images CAN legitimately be the same ground at different
// resolutions - the georeference carries no pixel size for exactly that reason - so the pixel
// check must not be allowed to masquerade as a ground check.
//
// `bestFitGain` is included because a luminance-shaped difference can be entirely explained by
// exposure: eye adaptation is a scalar gain on the whole frame, and a real content change survives
// dividing the least-squares best-fit gain out while an exposure difference collapses. A pair whose
// gain is ~1.000 proves the exposure never moved (rpc-design.md §5b). It is a reported number, not
// a verdict.

#include "Handlers/HandlerContext.h"
#include "Handlers/HandlerRegistration.h"
#include "Handlers/ParamSpec.h"
#include "Handlers/ErrorCodes.h"
#include "Handlers/Image/ImageOps.h"
#include "Handlers/Render/BitmapPaint.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "Misc/Paths.h"

// No file-scope `using namespace` - see the note in ImageTileHandler.cpp.
namespace
{
    // Per-channel accumulators over the two images. All three failure directions are separable:
    // a difference in R only, a uniform gain, and a handful of hot pixels score differently.
    struct FPwCompareStats
    {
        int64 DifferingPixels = 0;
        int64 SumAbs[3] = { 0, 0, 0 };
        int32 MaxAbs[3] = { 0, 0, 0 };
        double SumAB[3] = { 0.0, 0.0, 0.0 };
        double SumBB[3] = { 0.0, 0.0, 0.0 };
    };

    TSharedPtr<FJsonObject> PwCompareChannelObject(double R, double G, double B)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetNumberField(TEXT("r"), R);
        Obj->SetNumberField(TEXT("g"), G);
        Obj->SetNumberField(TEXT("b"), B);
        return Obj;
    }
}

REGISTER_RPC_HANDLER("image.compare", "image",
    "Write a side-by-side and a per-channel difference composite for two images, with measured difference statistics. Refuses mismatched dimensions or mismatched georeferences instead of comparing incomparable things.",
    RPC_PARAMS(
        RPC_PARAM_REQ("imageA", "filepath",
            "Path to the first image (the reference, by convention). Absolute, or relative to the project directory."),
        RPC_PARAM_REQ("imageB", "filepath",
            "Path to the second image (the capture, by convention). Must have the same pixel dimensions as imageA."),
        RPC_PARAM_OPT("georeferenceA", "object",
            "The ground imageA covers, in the same shape image.tile takes and writes. Optional, but if either georeference is supplied BOTH must be: a one-sided georeference proves nothing about whether the pair is comparable."),
        RPC_PARAM_OPT("georeferenceB", "object",
            "The ground imageB covers. Compared field by field against georeferenceA; any disagreement is GEOREFERENCE_MISMATCH naming the first differing field."),
        RPC_PARAM_DEF("amplify", "number",
            "Multiplier applied to each channel of the absolute difference before clamping to 255, so a small but real difference is visible. Reported back; it changes the picture, never the statistics.",
            "1.0"),
        RPC_PARAM_DEF("gapPx", "number",
            "Width in pixels of the divider between the two halves of the side-by-side.",
            "8"),
        RPC_PARAM_DEF("labels", "boolean",
            "Draw an A / B label on the corresponding half of the side-by-side.",
            "true"),
        RPC_PARAM_OPT("outputDir", "filepath",
            "Directory for both output PNGs. Defaults to <ProjectSaved>/PinWright/image/compare."),
        RPC_PARAM_OPT("outputPrefix", "string",
            "Filename stem for both outputs (<prefix>_side_by_side.png and <prefix>_difference.png). Defaults to <baseA>_vs_<baseB>."),
        RPC_PARAM_DEF("overwrite", "boolean",
            "Replace existing files at the target paths. When false (the default) the call refuses before writing anything.",
            "false")
    ))
{
    using namespace PinWrightImage;
    using namespace PinWrightBitmapPaint;

    FString RequestedA;
    FString RequestedB;
    if (!Ctx.RequireString(TEXT("imageA"), RequestedA) || !Ctx.RequireString(TEXT("imageB"), RequestedB))
    {
        return true;
    }

    const FString PathA = ResolveInputPath(RequestedA);
    const FString PathB = ResolveInputPath(RequestedB);
    FBitmap A;
    FBitmap B;
    FString ErrCode;
    FString ErrMsg;
    if (!LoadBitmap(PathA, A, ErrCode, ErrMsg) || !LoadBitmap(PathB, B, ErrCode, ErrMsg))
    {
        Ctx.SendError(ErrCode, ErrMsg);
        return true;
    }

    if (A.Width != B.Width || A.Height != B.Height)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("imageA"), PathA);
        Payload->SetNumberField(TEXT("widthA"), A.Width);
        Payload->SetNumberField(TEXT("heightA"), A.Height);
        Payload->SetStringField(TEXT("imageB"), PathB);
        Payload->SetNumberField(TEXT("widthB"), B.Width);
        Payload->SetNumberField(TEXT("heightB"), B.Height);
        Ctx.SendError(ErrorCodes::ERR_IMAGE_SIZE_MISMATCH,
            FString::Printf(TEXT("imageA is %d x %d and imageB is %d x %d. No pixel of one corresponds to a pixel of the other, so a difference composite would be a picture of the misalignment. Re-capture at a matching resolution, or tile both through image.tile on the same georeference and compare tile by tile."),
                A.Width, A.Height, B.Width, B.Height),
            Payload);
        return true;
    }

    // ---- georeferences: both or neither, and they must describe the same ground ----
    const TSharedPtr<FJsonObject> GeoObjectA = Ctx.GetObject(TEXT("georeferenceA"));
    const TSharedPtr<FJsonObject> GeoObjectB = Ctx.GetObject(TEXT("georeferenceB"));
    if (GeoObjectA.IsValid() != GeoObjectB.IsValid())
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_GEOREFERENCE,
            FString::Printf(TEXT("'%s' was supplied without '%s'. A one-sided georeference proves nothing about whether the pair covers the same ground, so it is refused rather than half-checked."),
                GeoObjectA.IsValid() ? TEXT("georeferenceA") : TEXT("georeferenceB"),
                GeoObjectA.IsValid() ? TEXT("georeferenceB") : TEXT("georeferenceA")));
        return true;
    }

    FGeoreference GeoA;
    FGeoreference GeoB;
    const bool bGeoreferenced = GeoObjectA.IsValid();
    if (bGeoreferenced)
    {
        if (!ParseGeoreference(GeoObjectA, A.Width, A.Height, /*bImageIsSingleTile=*/false, GeoA, ErrCode, ErrMsg))
        {
            Ctx.SendError(ErrCode, FString::Printf(TEXT("georeferenceA: %s"), *ErrMsg));
            return true;
        }
        if (!ParseGeoreference(GeoObjectB, B.Width, B.Height, /*bImageIsSingleTile=*/false, GeoB, ErrCode, ErrMsg))
        {
            Ctx.SendError(ErrCode, FString::Printf(TEXT("georeferenceB: %s"), *ErrMsg));
            return true;
        }
        FString Why;
        if (!GeoreferencesAgree(GeoA, GeoB, Why))
        {
            TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
            Payload->SetObjectField(TEXT("georeferenceA"), SerializeGeoreference(GeoA));
            Payload->SetObjectField(TEXT("georeferenceB"), SerializeGeoreference(GeoB));
            Payload->SetStringField(TEXT("difference"), Why);
            Ctx.SendError(ErrorCodes::ERR_GEOREFERENCE_MISMATCH,
                FString::Printf(TEXT("The two images do not cover the same ground: %s. Comparing them pixel by pixel would produce a difference image of two different places."), *Why),
                Payload);
            return true;
        }
    }

    // ---- options ----
    const double Amplify = Ctx.GetNumber(TEXT("amplify"), 1.0);
    if (!(Amplify > 0.0) || Amplify > 255.0)
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_PARAMS,
            FString::Printf(TEXT("'amplify' must be greater than 0 and at most 255, got %f"), Amplify));
        return true;
    }
    const int32 GapPx = Ctx.GetInt(TEXT("gapPx"), 8);
    if (GapPx < 0 || GapPx > 512)
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_PARAMS,
            FString::Printf(TEXT("'gapPx' must be between 0 and 512, got %d"), GapPx));
        return true;
    }
    const bool bLabels = Ctx.GetBool(TEXT("labels"), true);

    const FString DefaultPrefix = SanitizeBaseName(FPaths::GetBaseFilename(PathA), TEXT("a")) +
        TEXT("_vs_") + SanitizeBaseName(FPaths::GetBaseFilename(PathB), TEXT("b"));
    const FString Prefix = SanitizeBaseName(Ctx.GetString(TEXT("outputPrefix")), DefaultPrefix);
    const FString OutputDir = ResolveOutputDir(Ctx.GetString(TEXT("outputDir")), TEXT("compare"));
    const FString SideBySidePath = OutputDir / (Prefix + TEXT("_side_by_side.png"));
    const FString DifferencePath = OutputDir / (Prefix + TEXT("_difference.png"));

    if (!Ctx.GetBool(TEXT("overwrite"), false))
    {
        TArray<FString> Existing;
        if (IFileManager::Get().FileExists(*SideBySidePath)) { Existing.Add(SideBySidePath); }
        if (IFileManager::Get().FileExists(*DifferencePath)) { Existing.Add(DifferencePath); }
        if (Existing.Num() > 0)
        {
            Ctx.SendError(ErrorCodes::ERR_ALREADY_EXISTS,
                FString::Printf(TEXT("%s already exist(s) and overwrite is false. Nothing was written."),
                    *FString::Join(Existing, TEXT(", "))));
            return true;
        }
    }

    // ---- difference composite + statistics, one pass ----
    FBitmap Difference;
    FString MakeErr;
    if (!MakeBitmap(A.Width, A.Height, FColor(0, 0, 0, 255), Difference, MakeErr))
    {
        Ctx.SendError(ErrorCodes::ERR_ENCODE_FAILED, MakeErr);
        return true;
    }

    FPwCompareStats Stats;
    for (int32 Index = 0; Index < A.Pixels.Num(); ++Index)
    {
        const FColor& PixelA = A.Pixels[Index];
        const FColor& PixelB = B.Pixels[Index];
        const int32 Channels[3][2] = {
            { PixelA.R, PixelB.R }, { PixelA.G, PixelB.G }, { PixelA.B, PixelB.B } };

        bool bDiffers = false;
        uint8 Out[3] = { 0, 0, 0 };
        for (int32 C = 0; C < 3; ++C)
        {
            const int32 Delta = FMath::Abs(Channels[C][0] - Channels[C][1]);
            Stats.SumAbs[C] += Delta;
            Stats.MaxAbs[C] = FMath::Max(Stats.MaxAbs[C], Delta);
            Stats.SumAB[C] += static_cast<double>(Channels[C][0]) * static_cast<double>(Channels[C][1]);
            Stats.SumBB[C] += static_cast<double>(Channels[C][1]) * static_cast<double>(Channels[C][1]);
            bDiffers = bDiffers || Delta != 0;
            Out[C] = static_cast<uint8>(FMath::Clamp(FMath::RoundToInt(Delta * Amplify), 0, 255));
        }
        if (bDiffers)
        {
            ++Stats.DifferingPixels;
        }
        // Alpha 255 unconditionally: the composite is a picture of the difference, and a
        // transparent difference image is the blank-capture failure in another costume.
        Difference.Pixels[Index] = FColor(Out[0], Out[1], Out[2], 255);
    }

    // ---- side by side ----
    FBitmap SideBySide;
    if (!MakeBitmap(A.Width + GapPx + B.Width, A.Height, FColor(24, 24, 24, 255), SideBySide, MakeErr))
    {
        Ctx.SendError(ErrorCodes::ERR_ENCODE_FAILED, MakeErr);
        return true;
    }
    FString BlitErr;
    if (!BlitInto(SideBySide, A, 0, 0, BlitErr) ||
        !BlitInto(SideBySide, B, A.Width + GapPx, 0, BlitErr))
    {
        Ctx.SendError(ErrorCodes::ERR_ENCODE_FAILED, BlitErr);
        return true;
    }
    if (bLabels)
    {
        FSurface Surface = SideBySide.Surface();
        FLabelStyle Style;
        Style.Anchor = ELabelAnchor::TextTopLeft;
        DrawLabel(Surface, TEXT("A"), 4, 4, Style);
        DrawLabel(Surface, TEXT("B"), A.Width + GapPx + 4, 4, Style);
    }

    // The two PNGs are written one after the other, so the difference write can fail with the
    // side-by-side already on disk. That leftover is NAMED, never deleted - the image.tile
    // staleFiles rule. Deleting it would be an unannounced mutation that can itself fail, and
    // under overwrite:true it would take the caller's previous side-by-side with it; a partial
    // output nobody reported is what makes the next run look complete.
    if (!SaveBitmapPng(SideBySidePath, SideBySide, ErrCode, ErrMsg))
    {
        Ctx.SendError(ErrCode, FString::Printf(TEXT("%s (side-by-side). Neither output was written."), *ErrMsg));
        return true;
    }
    if (!SaveBitmapPng(DifferencePath, Difference, ErrCode, ErrMsg))
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        TArray<TSharedPtr<FJsonValue>> Partial;
        Partial.Add(MakeShared<FJsonValueString>(SideBySidePath));
        Payload->SetArrayField(TEXT("partialOutputs"), Partial);
        Payload->SetStringField(TEXT("sideBySide"), SideBySidePath);
        Payload->SetStringField(TEXT("difference"), DifferencePath);
        Ctx.SendError(ErrCode,
            FString::Printf(TEXT("%s (difference composite). %s WAS written and is still on disk, so the pair is incomplete: delete it or re-run with overwrite:true. No statistics are reported for a comparison whose outputs could not both be written."),
                *ErrMsg, *SideBySidePath),
            Payload);
        return true;
    }

    // ---- report ----
    const double PixelCount = static_cast<double>(A.Pixels.Num());
    TSharedPtr<FJsonObject> StatsObj = MakeShared<FJsonObject>();
    StatsObj->SetNumberField(TEXT("pixelCount"), PixelCount);
    StatsObj->SetNumberField(TEXT("differingPixels"), static_cast<double>(Stats.DifferingPixels));
    StatsObj->SetNumberField(TEXT("differingFraction"),
        PixelCount > 0.0 ? static_cast<double>(Stats.DifferingPixels) / PixelCount : 0.0);
    StatsObj->SetObjectField(TEXT("meanAbsDifference"), PwCompareChannelObject(
        PixelCount > 0.0 ? Stats.SumAbs[0] / PixelCount : 0.0,
        PixelCount > 0.0 ? Stats.SumAbs[1] / PixelCount : 0.0,
        PixelCount > 0.0 ? Stats.SumAbs[2] / PixelCount : 0.0));
    StatsObj->SetObjectField(TEXT("maxAbsDifference"),
        PwCompareChannelObject(Stats.MaxAbs[0], Stats.MaxAbs[1], Stats.MaxAbs[2]));
    StatsObj->SetNumberField(TEXT("meanAbsDifferenceOverall"),
        PixelCount > 0.0 ? (Stats.SumAbs[0] + Stats.SumAbs[1] + Stats.SumAbs[2]) / (3.0 * PixelCount) : 0.0);

    // Omitted per channel when the denominator is zero (a fully black B channel): a gain of 0 or 1
    // would be a number nobody measured.
    TSharedPtr<FJsonObject> GainObj = MakeShared<FJsonObject>();
    static const TCHAR* const ChannelNames[3] = { TEXT("r"), TEXT("g"), TEXT("b") };
    for (int32 C = 0; C < 3; ++C)
    {
        if (Stats.SumBB[C] > 0.0)
        {
            GainObj->SetNumberField(ChannelNames[C], Stats.SumAB[C] / Stats.SumBB[C]);
        }
    }
    StatsObj->SetObjectField(TEXT("bestFitGain"), GainObj);
    // Measured, not asserted: it is the count reaching zero, not a separate claim.
    StatsObj->SetBoolField(TEXT("identical"), Stats.DifferingPixels == 0);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("imageA"), PathA);
    Result->SetStringField(TEXT("imageB"), PathB);
    Result->SetNumberField(TEXT("width"), A.Width);
    Result->SetNumberField(TEXT("height"), A.Height);
    Result->SetStringField(TEXT("sideBySide"), SideBySidePath);
    Result->SetStringField(TEXT("difference"), DifferencePath);
    Result->SetNumberField(TEXT("amplify"), Amplify);
    Result->SetNumberField(TEXT("gapPx"), GapPx);
    Result->SetBoolField(TEXT("georeferenced"), bGeoreferenced);
    if (bGeoreferenced)
    {
        Result->SetObjectField(TEXT("georeference"), SerializeGeoreference(GeoA));
        Result->SetObjectField(TEXT("grid"), DescribeGrid(GeoA));
    }
    Result->SetObjectField(TEXT("stats"), StatsObj);

    Ctx.SendSuccess(FString::Printf(TEXT("Compared %d x %d images: %lld of %.0f pixels differ. Wrote %s and %s"),
        A.Width, A.Height, Stats.DifferingPixels, PixelCount, *SideBySidePath, *DifferencePath), Result);
    return true;
}
