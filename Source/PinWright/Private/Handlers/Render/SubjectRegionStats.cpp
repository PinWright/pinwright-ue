// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Handlers/Render/SubjectRegionStats.h"

#include "Dom/JsonObject.h"
#include "Math/RotationMatrix.h"

namespace PinWrightSubjectRegion
{

namespace
{
    // Prefixed because Private/ builds under Unity: an anonymous namespace merges with every other
    // one in the same translation unit, so a bare `Luminance` here is a latent ODR clash with a
    // sibling capture file.

    // The SAME Rec.709 coefficients CalculateCaptureImageStats uses. Spelled out rather than
    // shared because sharing it would mean including PreviewViewportCaptureUtils.h, which already
    // includes this header for the field it carries on FViewportCaptureOutput.
    double SubjectRegionLuminance(const FColor& Color)
    {
        return (0.2126 * static_cast<double>(Color.R) +
                0.7152 * static_cast<double>(Color.G) +
                0.0722 * static_cast<double>(Color.B)) / 255.0;
    }

    // Half-angle tangents of the frame, one per axis.
    //
    // WHICH AXIS `fov` NAMES is an engine setting, ULevelEditorViewportSettings::
    // AspectRatioAxisConstraint, whose default is AspectRatio_MajorAxisFOV -- the field of view is
    // the MAJOR (longer) axis and the minor axis follows from the aspect ratio. That default is
    // what is assumed here, and it is inert in the case that matters: every capture verb defaults
    // to a SQUARE frame (DefaultCaptureEdge), where the two axes coincide and no assumption is
    // being made at all. On a non-square frame a wrong reading scales the ellipse along one axis;
    // it cannot move its centre and it cannot change which pixels are dark.
    void SubjectRegionHalfFovTangents(double FovDegrees, int32 Width, int32 Height,
        double& OutTanHalfX, double& OutTanHalfY)
    {
        const double TanHalfMajor =
            FMath::Tan(FMath::DegreesToRadians(FMath::Clamp(FovDegrees, 1.0, 170.0) * 0.5));
        if (Width >= Height)
        {
            OutTanHalfX = TanHalfMajor;
            OutTanHalfY = TanHalfMajor * (static_cast<double>(Height) / static_cast<double>(Width));
        }
        else
        {
            OutTanHalfY = TanHalfMajor;
            OutTanHalfX = TanHalfMajor * (static_cast<double>(Width) / static_cast<double>(Height));
        }
    }

    // Project the bounding sphere to an ellipse in pixels, TOP-LEFT origin. Returns false with a
    // reason when no ellipse exists for this pose.
    bool SubjectRegionProject(const FSubjectRegionView& View, FSubjectRegionStats& OutStats)
    {
        const FMatrix Basis = FRotationMatrix(View.CameraRotation);
        const FVector Forward = Basis.GetScaledAxis(EAxis::X).GetSafeNormal();
        const FVector Right = Basis.GetScaledAxis(EAxis::Y).GetSafeNormal();
        const FVector Up = Basis.GetScaledAxis(EAxis::Z).GetSafeNormal();
        if (Forward.IsNearlyZero())
        {
            OutStats.NotMeasuredReason = TEXT("the camera rotation is degenerate, so no frame "
                "geometry can be derived from it");
            return false;
        }

        const FVector ToCentre = View.BoundsOrigin - View.CameraLocation;
        const double LateralRight = FVector::DotProduct(ToCentre, Right);
        const double LateralUp = FVector::DotProduct(ToCentre, Up);

        double NdcX = 0.0;
        double NdcY = 0.0;
        double RadiusNdcX = 0.0;
        double RadiusNdcY = 0.0;

        if (View.bOrthographic)
        {
            const double HalfWidthWorld = View.OrthoWidth * 0.5;
            if (!(HalfWidthWorld > 0.0))
            {
                OutStats.NotMeasuredReason = TEXT("the orthographic frame has no width, so the "
                    "subject's bounds cannot be placed in it");
                return false;
            }
            const double HalfHeightWorld =
                HalfWidthWorld * (static_cast<double>(View.Height) / static_cast<double>(View.Width));
            NdcX = LateralRight / HalfWidthWorld;
            NdcY = LateralUp / HalfHeightWorld;
            RadiusNdcX = View.BoundsRadius / HalfWidthWorld;
            RadiusNdcY = View.BoundsRadius / HalfHeightWorld;
        }
        else
        {
            const double Depth = FVector::DotProduct(ToCentre, Forward);
            if (Depth <= View.BoundsRadius)
            {
                // The camera sits inside or behind the bounding sphere. There is no ellipse: the
                // sphere's silhouette is not a bounded figure in this frame, and any rectangle
                // this returned would be a guess.
                OutStats.NotMeasuredReason = TEXT("the camera is inside or behind the subject's "
                    "bounding sphere, so the subject has no bounded silhouette in this frame");
                return false;
            }
            double TanHalfX = 0.0;
            double TanHalfY = 0.0;
            SubjectRegionHalfFovTangents(View.Fov, View.Width, View.Height, TanHalfX, TanHalfY);
            if (!(TanHalfX > 0.0) || !(TanHalfY > 0.0))
            {
                OutStats.NotMeasuredReason = TEXT("the field of view is degenerate");
                return false;
            }

            // tan of the half-angle the sphere subtends, from sin(theta) = R / distance. Exact for
            // a sphere centred on the view axis and a slight over-estimate off it, which widens
            // the ellipse -- the direction that dilutes the reading rather than sharpening it.
            const double Distance = ToCentre.Size();
            const double TangentLeg =
                FMath::Sqrt(FMath::Max((Distance * Distance) -
                    (View.BoundsRadius * View.BoundsRadius), UE_DOUBLE_SMALL_NUMBER));
            const double TanAngularRadius = View.BoundsRadius / TangentLeg;

            NdcX = (LateralRight / Depth) / TanHalfX;
            NdcY = (LateralUp / Depth) / TanHalfY;
            RadiusNdcX = TanAngularRadius / TanHalfX;
            RadiusNdcY = TanAngularRadius / TanHalfY;
        }

        OutStats.CenterX = ((NdcX * 0.5) + 0.5) * static_cast<double>(View.Width);
        OutStats.CenterY = (0.5 - (NdcY * 0.5)) * static_cast<double>(View.Height);
        OutStats.RadiusX = RadiusNdcX * 0.5 * static_cast<double>(View.Width);
        OutStats.RadiusY = RadiusNdcY * 0.5 * static_cast<double>(View.Height);
        if (!(OutStats.RadiusX > 0.0) || !(OutStats.RadiusY > 0.0))
        {
            OutStats.NotMeasuredReason = TEXT("the subject projects to less than a pixel in this "
                "frame");
            return false;
        }
        return true;
    }
}

FSubjectRegionStats MeasureSubjectRegion(TConstArrayView<FColor> ColorData,
    const FSubjectRegionView& View, double LitLuminanceThreshold)
{
    FSubjectRegionStats Stats;
    Stats.LitLuminanceThreshold = LitLuminanceThreshold;
    // Copied before every early return, so the warning can name the exposure pin even on a frame
    // whose region could not be measured. Costs nothing and cannot change a verdict: no code path
    // reads either field except MakeSubjectRegionWarning, and that returns empty unless
    // bSilhouette fired.
    Stats.bFixedExposure = View.bFixedExposure;
    Stats.Ev100 = View.Ev100;

    if (!(View.BoundsRadius > 0.0))
    {
        Stats.NotMeasuredReason = TEXT("no subject bounds were supplied for this capture, so "
            "there is no subject region to measure");
        return Stats;
    }
    if (View.Width <= 0 || View.Height <= 0 ||
        ColorData.Num() != static_cast<int64>(View.Width) * static_cast<int64>(View.Height))
    {
        Stats.NotMeasuredReason = TEXT("the readback buffer does not match the frame dimensions, "
            "so no pixel can be attributed to the subject or to the backdrop");
        return Stats;
    }
    if (!SubjectRegionProject(View, Stats))
    {
        // SubjectRegionProject wrote the reason.
        return Stats;
    }

    double SubjectSum = 0.0;
    double BackdropSum = 0.0;
    int64 SubjectUnlit = 0;
    int64 BackdropUnlit = 0;
    const double InvRadiusX = 1.0 / Stats.RadiusX;
    const double InvRadiusY = 1.0 / Stats.RadiusY;

    for (int32 Y = 0; Y < View.Height; ++Y)
    {
        // Pixel CENTRES, so a one-pixel ellipse contains the pixel it lands on rather than the
        // corner between four of them.
        const double OffsetY = ((static_cast<double>(Y) + 0.5) - Stats.CenterY) * InvRadiusY;
        const double OffsetYSquared = OffsetY * OffsetY;
        for (int32 X = 0; X < View.Width; ++X)
        {
            const double OffsetX = ((static_cast<double>(X) + 0.5) - Stats.CenterX) * InvRadiusX;
            const double Luminance =
                SubjectRegionLuminance(ColorData[(Y * View.Width) + X]);
            const bool bUnlit = Luminance <= LitLuminanceThreshold;
            if ((OffsetX * OffsetX) + OffsetYSquared <= 1.0)
            {
                ++Stats.SubjectPixelCount;
                SubjectSum += Luminance;
                Stats.SubjectMaxLuminance = FMath::Max(Stats.SubjectMaxLuminance, Luminance);
                SubjectUnlit += bUnlit ? 1 : 0;
            }
            else
            {
                ++Stats.BackdropPixelCount;
                BackdropSum += Luminance;
                BackdropUnlit += bUnlit ? 1 : 0;
            }
        }
    }

    if (Stats.SubjectPixelCount <= 0 || Stats.BackdropPixelCount <= 0)
    {
        // Either the subject's ellipse fell entirely outside the frame, or it swallowed it. Both
        // leave the comparison this measurement IS with nothing to compare against, and a
        // one-sided number reported as a subject reading would be the exact frame-mean confusion
        // this file exists to end.
        Stats.SubjectPixelCount = 0;
        Stats.BackdropPixelCount = 0;
        Stats.SubjectMaxLuminance = 0.0;
        Stats.NotMeasuredReason = TEXT("the subject's projected bounds cover none of this frame or "
            "all of it, so there is no backdrop to compare the subject against");
        return Stats;
    }

    Stats.SubjectMeanLuminance = SubjectSum / static_cast<double>(Stats.SubjectPixelCount);
    Stats.SubjectUnlitFraction =
        static_cast<double>(SubjectUnlit) / static_cast<double>(Stats.SubjectPixelCount);
    Stats.BackdropMeanLuminance = BackdropSum / static_cast<double>(Stats.BackdropPixelCount);
    Stats.BackdropUnlitFraction =
        static_cast<double>(BackdropUnlit) / static_cast<double>(Stats.BackdropPixelCount);

    Stats.bSilhouette =
        (Stats.SubjectUnlitFraction - Stats.BackdropUnlitFraction) >= SilhouetteExcessUnlitShare &&
        (1.0 - Stats.BackdropUnlitFraction) >= SilhouetteLitBackdropShare;
    // Last, so every early return above leaves it false and no partial reading can be read as a
    // measurement.
    Stats.bMeasured = true;
    return Stats;
}

TSharedPtr<FJsonObject> MakeSubjectRegionObject(const FSubjectRegionStats& Stats)
{
    TSharedPtr<FJsonObject> Region = MakeShared<FJsonObject>();
    Region->SetBoolField(TEXT("measured"), Stats.bMeasured);
    if (!Stats.bMeasured)
    {
        // Stated, not omitted: an absent block reads as "the subject is fine", which is the one
        // thing an unmeasured region does not say.
        if (!Stats.NotMeasuredReason.IsEmpty())
        {
            Region->SetStringField(TEXT("notMeasured"), Stats.NotMeasuredReason);
        }
        return Region;
    }

    TSharedPtr<FJsonObject> Ellipse = MakeShared<FJsonObject>();
    Ellipse->SetNumberField(TEXT("centerX"), Stats.CenterX);
    Ellipse->SetNumberField(TEXT("centerY"), Stats.CenterY);
    Ellipse->SetNumberField(TEXT("radiusX"), Stats.RadiusX);
    Ellipse->SetNumberField(TEXT("radiusY"), Stats.RadiusY);
    // The subject's world-space bounding SPHERE projected into this frame, so a caller that wants
    // to crop the subject out of the PNG, or to overlay it, has the same figure the verdict used.
    Region->SetObjectField(TEXT("ellipse"), Ellipse);

    TSharedPtr<FJsonObject> Subject = MakeShared<FJsonObject>();
    Subject->SetNumberField(TEXT("pixelCount"), static_cast<double>(Stats.SubjectPixelCount));
    Subject->SetNumberField(TEXT("meanLuminance"), Stats.SubjectMeanLuminance);
    Subject->SetNumberField(TEXT("maxLuminance"), Stats.SubjectMaxLuminance);
    Subject->SetNumberField(TEXT("unlitFraction"), Stats.SubjectUnlitFraction);
    Region->SetObjectField(TEXT("subject"), Subject);

    TSharedPtr<FJsonObject> Backdrop = MakeShared<FJsonObject>();
    Backdrop->SetNumberField(TEXT("pixelCount"), static_cast<double>(Stats.BackdropPixelCount));
    Backdrop->SetNumberField(TEXT("meanLuminance"), Stats.BackdropMeanLuminance);
    Backdrop->SetNumberField(TEXT("unlitFraction"), Stats.BackdropUnlitFraction);
    Region->SetObjectField(TEXT("backdrop"), Backdrop);

    Region->SetNumberField(TEXT("litLuminanceThreshold"), Stats.LitLuminanceThreshold);
    Region->SetBoolField(TEXT("silhouette"), Stats.bSilhouette);
    return Region;
}

FString MakeSubjectRegionWarning(const FSubjectRegionStats& Stats)
{
    if (!Stats.bMeasured || !Stats.bSilhouette)
    {
        return FString();
    }
    // Names both fractions, both means and the subject's brightest pixel -- the last is the
    // number that separates "rendered black" from "rendered dark", because a darkly shaded
    // subject still carries a highlight and one that never shaded carries none. Then the LEAD
    // CAUSE, which is a fixed exposure whenever the frame was drawn at one.
    const FString Observation = FString::Printf(
        TEXT("The subject rendered as a dark shape inside a lit frame: %.1f%% of its projected ")
        TEXT("bounds is unlit against %.1f%% of the rest of the frame (mean luminance %.6f inside ")
        TEXT("versus %.6f outside), and the brightest pixel anywhere on the subject is %.6f. ")
        TEXT("`blank` is false and correct -- the FRAME carries content -- and the frame mean is ")
        TEXT("the backdrop's, so neither of those fields can see this. "),
        Stats.SubjectUnlitFraction * 100.0,
        Stats.BackdropUnlitFraction * 100.0,
        Stats.SubjectMeanLuminance,
        Stats.BackdropMeanLuminance,
        Stats.SubjectMaxLuminance);

    // The measured cause, and the remedy, when the pixels were drawn at a fixed EV100.
    //
    // WHY THIS LEADS. B-capture-asset-preview-renders-foliage-black spent two passes on the
    // preview scene's lighting because a lit backdrop looked like proof the lights worked. It is
    // not: the backdrop is the EMISSIVE sky sphere and renders the same with every light off (see
    // the header). What the four failing captures actually shared was a pin at EV100 -0.5 on a
    // preview that resolves to EV100 -2.12 unpinned -- 1.62 stops under, which the tone curve's
    // toe turns into a black subject long before it dims a panorama.
    //
    // The remedy is the one PINWRIGHT_EXPOSURE_PARAM_DESC already prescribes and nothing in a
    // response used to point at from here: derive the number, do not pick it.
    const FString Cause = Stats.bFixedExposure
        ? FString::Printf(
            TEXT("LEAD CAUSE: these pixels were drawn at a FIXED exposure, EV100 %.2f. An "
                 "asset-editor preview scene lights its subject with a key of intensity ~1 while "
                 "its backdrop is an EMISSIVE sky sphere several stops brighter, so a pin that "
                 "leaves the backdrop looking right can still be well under the subject and the "
                 "tone curve's toe crushes it to black -- a lit-looking environment is NOT "
                 "evidence the scene's lights reached the asset. Before reading this as a shading "
                 "defect, re-shoot with `exposure:{mode:\"auto\"}`, read "
                 "`viewport.exposure.ev100Equivalent` off THAT response, and pass that number as "
                 "`ev100` instead of a hand-picked one. "),
            Stats.Ev100)
        : FString(
            TEXT("This frame was NOT drawn at a fixed exposure, so the usual cause -- an exposure "
                 "pin below what the scene resolves to -- is ruled out here. "));

    const FString Rest = FString::Printf(
        TEXT("What this does NOT establish is why: a genuinely black asset and an asset that ")
        TEXT("failed to shade produce the same pixels. If the exposure is already right, read ")
        TEXT("`viewport.previewScene.showEnvironment`, `sky.visible`, `sky.intensity` and ")
        TEXT("`key.intensity` in this same response, and compare against the same asset placed ")
        TEXT("in a level. A pixel counts as unlit at or below luminance %.2f, the same threshold ")
        TEXT("`imageStats.litPixelFraction` is counted against."),
        Stats.LitLuminanceThreshold);

    return Observation + Cause + Rest;
}

void AddSubjectRegionFields(const FSubjectRegionStats& Stats,
    const TSharedPtr<FJsonObject>& Result)
{
    if (!Result.IsValid())
    {
        return;
    }
    Result->SetObjectField(TEXT("subjectRegion"), MakeSubjectRegionObject(Stats));
    const FString Warning = MakeSubjectRegionWarning(Stats);
    if (!Warning.IsEmpty())
    {
        Result->SetStringField(TEXT("subjectRegionWarning"), Warning);
    }
}

}
