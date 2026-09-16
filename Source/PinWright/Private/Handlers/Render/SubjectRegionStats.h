// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"
// TConstArrayView, for the readback buffer below. CoreMinimal.h pulls in Containers/Array.h but
// not this one.
#include "Containers/ArrayView.h"

class FJsonObject;

// Does the SUBJECT carry light, or only the frame around it?
//
// WHY THIS EXISTS. Every luminance number a capture publishes today is a statistic OF THE FRAME:
// `meanLuminance`, `litPixelCount`, `blank`, `crushed`. Board ticket
// B-capture-asset-preview-renders-foliage-black is the failure that has no measurement here at
// all -- four foliage assets came back from `render.capture_asset_preview` as a PURE BLACK SUBJECT
// inside a CORRECTLY LIT preview environment, and every published field was true and healthy:
// `blank: false` was correct (the frame genuinely carries content), the frame mean was normal
// (the backdrop dominates it), and the tone range was fine (the backdrop spreads over dozens of
// levels). A caller that gates on the response rather than on the pixels passed a useless image
// through, which is how the defect shipped.
//
// A frame mean cannot answer this by construction. The subject occupies a minority of the frame,
// so its contribution to any whole-frame statistic is swamped by the backdrop, the floor and the
// sky gradient -- which is exactly the reading `B-capture-preview-default-camera-vs-light` reached
// and then correctly disclaimed as "not evidence the subject is better shaded".
//
// WHAT IS MEASURED. The subject's own bounding sphere, projected into the frame the pixels were
// rendered with, then the UNLIT SHARE inside that ellipse compared against the unlit share of
// everything outside it. The comparison is the whole point: an absolute darkness threshold cannot
// separate "the asset rendered black" from "this whole preview is dark" or from "the environment
// is switched off and the backdrop is black too", and all three of those are ordinary states a
// capture can legitimately be in. A DIFFERENCE between the subject and its own backdrop is a
// statement about the subject and nothing else.
//
// "Unlit" is not a new threshold. It is the exact complement of the `litPixelCount` criterion the
// frame already publishes (`PinWrightRenderCapture::BlankLitLuminanceThreshold`), passed in rather
// than redeclared so the two can never drift and so a caller reading `subjectRegion.unlitFraction`
// beside `imageStats.litPixelFraction` is reading one threshold, not two.
//
// WHAT IT DELIBERATELY DOES NOT CLAIM. Not the CAUSE. A genuinely black asset and an asset that
// failed to shade produce identical pixels, and no readback can separate them. The verdict names
// the observation and points at the fields in the same response that narrow the cause.
//
// THE ESTABLISHED CAUSE OF THE TICKET ABOVE, AND THE TRAP THAT COST TWO PASSES. It was NOT the
// preview scene's lighting and it was NOT foliage. All four failing captures pinned exposure
// (`exposure:{mode:"fixed", ev100:-0.5}`), which is DARKER than the exposure an asset-editor
// preview scene resolves to on its own. The arithmetic, all of it from recorded responses in this
// project: the pinned captures report `adapted` 1.4142 at ev100 -0.5, and gain = 1/(LuminanceMax *
// 2^EV100) (Ev100ToExposureGain -> RenderUtils.h EV100ToLuminance), so LuminanceMax is 1 here and
// EV100 = -log2(gain). A Static Mesh Editor preview in the same project under `{mode:"auto"}`
// measured `adapted` 4.338, i.e. EV100 -2.12 -- so the pin sat 1.62 stops under, and a second
// probe pinned at ev100 0 sat 2.12 stops under.
//
// That second probe is the A/B, and it is what settles this: the SAME asset in the SAME preview,
// minutes apart, renders normally under `{mode:"auto"}` and as a black silhouette with the
// backdrop still correct at `ev100: 0`. Camera azimuth, framing and profile were unchanged
// between the two; only the exposure moved. The asset is a flat-shaded low-poly tree -- no
// foliage shading model, no WPO, no masked leaves -- which is what rules the foliage story out.
// (The auto reading predates the preview-rig block, so "same preview scene" there is same
// project and same editor rather than a recorded profile name.)
//
// Compounding, and NOT to be conflated: the preview key arrives near azimuth 110 degrees, so a
// default camera looks at the subject's dark side and its fill is genuinely near zero
// (B-capture-preview-default-camera-vs-light, docs/wiki-src/visual-review.model-rig.md). The pin
// decides whether what little light IS there survives the tone curve. Both matter; only the
// exposure was varied in the A/B above.
//
// WHY THE BACKDROP SURVIVES AND SO WHY "a correctly lit environment" IS NOT EVIDENCE. The visible
// backdrop of an FAdvancedPreviewScene is the sky SPHERE: an inverse-normals mesh carrying
// `M_SkyBox` with the profile's HDR panorama (AdvancedPreviewScene.cpp:63-86). It is EMISSIVE. It
// renders identically with the key light, the sky light and every other light in the scene
// switched off, and it is several stops brighter than anything the 1-intensity preview key can
// put on a surface. So an underexposure that leaves the panorama legible drops the only genuinely
// LIT geometry in the frame -- the subject -- below the tone curve's toe. "Lit environment, black
// subject" is the expected appearance of an underexposed asset preview, not a shading defect.
//
// Nor does it claim to segment the subject. The bounding SPHERE circumscribes the asset, so its
// projected ellipse always contains backdrop as well -- a thin tree fills perhaps a quarter of its
// own disc. That dilutes the reading toward the backdrop, so it can only ever WEAKEN the
// difference, never invent one. The error is in the safe direction: this measurement can miss a
// black subject, it cannot manufacture one.
//
// WHY NOT THE COVERAGE MASK, which would segment the subject exactly. `subjectCoverage` already
// draws every shot twice, once with the preview component hidden, and the pixels that CHANGED are
// the subject to the pixel (PoseListCapture.cpp, MeasureChangedPixelFraction). It was considered
// and rejected: hiding the component also removes the subject's SHADOW, so the changed set is the
// subject plus its shadow on the preview floor -- and a shadow is genuinely unlit. Segmenting that
// way would report a correctly shaded asset standing on a lit floor as substantially unlit, i.e.
// it would produce FALSE POSITIVES, while the ellipse's dilution produces false negatives. On a
// verdict whose whole job is to make a caller distrust an image, the error has to point the other
// way. Do not "sharpen" this by swapping in the coverage mask.
//
// `subjectCoverage` also answers a different question and stays the right field for it: whether
// the subject is in the picture AT ALL. A black subject has perfectly ordinary coverage -- black
// pixels differ from backdrop pixels just as well as lit ones do -- which is precisely why it did
// not catch this.
namespace PinWrightSubjectRegion
{
    // The pose and frame the pixels were rendered with, plus the subject's world-space bounding
    // sphere. Every field is a value the capture has already MEASURED (the effective pose the
    // renderer resolved to, not the requested one) or resolved from the asset; nothing here is
    // re-derived from a request.
    struct FSubjectRegionView
    {
        FVector CameraLocation = FVector::ZeroVector;
        FRotator CameraRotation = FRotator::ZeroRotator;
        int32 Width = 0;
        int32 Height = 0;
        bool bOrthographic = false;
        // Perspective only, degrees.
        double Fov = 50.0;
        // Orthographic only, world centimetres left to right.
        double OrthoWidth = 0.0;
        FVector BoundsOrigin = FVector::ZeroVector;
        // <= 0 means the caller has no bounds for this subject, and the region is reported as
        // unmeasured rather than as a zero-sized one.
        double BoundsRadius = 0.0;

        // Was the frame drawn at a FIXED exposure, and at which EV100. Not published -- the
        // response already carries both under `viewport.exposure` and duplicating them there
        // would give a caller two places to read one fact. They are carried here for one purpose:
        // the silhouette warning names the exposure pin as the leading cause when a pin was in
        // force, because that is what the measured case turned out to be (see the
        // ESTABLISHED CAUSE note above).
        //
        // The predicate is "the pixels were drawn fixed", NOT "this call pinned it": a viewport
        // left pinned through the editor's own EV100 control produces the identical frame, and a
        // caller who pinned nothing is the one who most needs to be told. Feed it from
        // FViewportCaptureOutput::bExposureFixedApplied / Ev100Applied, both of which are read
        // back off the client rather than echoed from the request.
        bool bFixedExposure = false;
        double Ev100 = 0.0;
    };

    struct FSubjectRegionStats
    {
        // FALSE IS THE DEFAULT AND IT MATTERS, for the same reason FCaptureImageStats gates its
        // tone fields: a zeroed subject region would read as "the subject is entirely black",
        // which is the loudest possible claim, reached by nobody.
        bool bMeasured = false;
        // Why not, when bMeasured is false. Published, because "no bounds were supplied" and "the
        // camera is inside the subject" are different facts and a caller can act on the second.
        FString NotMeasuredReason;

        // The projected bounding-sphere ellipse, in pixels, TOP-LEFT origin -- the same origin
        // convention PinWrightViewProjection uses, so an overlay and this region agree.
        double CenterX = 0.0;
        double CenterY = 0.0;
        double RadiusX = 0.0;
        double RadiusY = 0.0;

        int64 SubjectPixelCount = 0;
        double SubjectMeanLuminance = 0.0;
        // The brightest pixel anywhere in the ellipse. The "black or merely dark" number: a
        // subject that shaded darkly still has highlights, one that did not shade has none.
        double SubjectMaxLuminance = 0.0;
        double SubjectUnlitFraction = 0.0;

        int64 BackdropPixelCount = 0;
        double BackdropMeanLuminance = 0.0;
        double BackdropUnlitFraction = 0.0;

        // The threshold both unlit fractions were counted against, published so a caller can
        // reproduce the verdict without inheriting a constant.
        double LitLuminanceThreshold = 0.0;

        // The subject's disc is materially darker than its own backdrop, and the backdrop is
        // genuinely lit. See the constants below for both terms.
        bool bSilhouette = false;

        // Copied through from FSubjectRegionView, not published -- see the note there. Only the
        // warning reads them.
        bool bFixedExposure = false;
        double Ev100 = 0.0;
    };

    // How much MORE of the subject's disc has to be unlit than of the rest of the frame before the
    // difference is a silhouette rather than sampling noise. Five percent of the disc is far above
    // what an antialiased edge or a contact shadow contributes and far below the quarter-to-half a
    // black subject produces.
    constexpr double SilhouetteExcessUnlitShare = 0.05;
    // ...AND the rest of the frame has to actually be lit. Without this term "the subject is
    // darker than the backdrop" is a statement about a dark FRAME -- a wireframe or debug view, a
    // preview profile with its environment switched off, an exposure crushed past the end of the
    // range -- all of which are already named by their own fields. This is also why no separate
    // lit-view-mode gate is needed here: a wireframe frame is unlit almost everywhere, so this
    // term withholds the verdict on its own. Do not "finish the fix" by removing it.
    constexpr double SilhouetteLitBackdropShare = 0.50;

    // Measure the subject region out of a readback buffer.
    //
    // ColorData is the buffer CalculateCaptureImageStats ran on, Width*Height entries; a short or
    // mismatched buffer yields bMeasured false rather than a partial reading. LitLuminanceThreshold
    // is the frame's own lit threshold -- pass PinWrightRenderCapture::BlankLitLuminanceThreshold.
    FSubjectRegionStats MeasureSubjectRegion(TConstArrayView<FColor> ColorData,
        const FSubjectRegionView& View, double LitLuminanceThreshold);

    // The `subjectRegion` response object. Always returns an object: an unmeasured region reports
    // `measured: false` plus the reason, never silence, because absent fields read as "measured
    // and fine".
    TSharedPtr<FJsonObject> MakeSubjectRegionObject(const FSubjectRegionStats& Stats);

    // Empty unless the subject is effectively invisible against a lit backdrop.
    FString MakeSubjectRegionWarning(const FSubjectRegionStats& Stats);

    // Attach `subjectRegion` and, when it fires, `subjectRegionWarning` to a response.
    void AddSubjectRegionFields(const FSubjectRegionStats& Stats,
        const TSharedPtr<FJsonObject>& Result);
}
