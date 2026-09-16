// Copyright (c) 2026 Alexander Penkin. MIT License.

// MotionMeasureHandler.cpp - animation.measure_motion.
//
// The motion half of the numeric-gate pair whose skinning half is skeleton.audit_skin_weights.
// See MotionMetricsAnalysis.h for what each metric measures and why every threshold here is a
// ratio against the animation's own frame step rather than an absolute number.
//
// WHERE THE POSES COME FROM, AND WHY NOT FROM A COMPONENT
//
// Everything is sampled straight off the sequence's editor data model
// (IAnimationDataModel::GetBoneTrackTransforms) and accumulated into component space against
// the sequence's own skeleton reference pose. No component, no world, no registration, no
// playhead, no rendering. That matters for three reasons:
//
//   1. A bound skeletal mesh only evaluates in the editor while Sequencer scrubs it
//      (bUpdateAnimationInEditor is transient and defaults false), so any component-based
//      measurement would need a level, a sequence and a scrub per sample.
//   2. USkinnedMeshComponent::GetBoneTransform returns FTransform::Identity, with no error,
//      whenever the component is not registered. A gate built on it can silently measure a
//      pile of identities and call the result clean.
//   3. Data-model transforms are the authored source: no compression error, no retargeting,
//      no additive resolution. Two runs on the same asset return the same numbers forever,
//      which is what makes a regression gate a gate.
//
// The cost of that choice, stated plainly: this measures the ANIMATION, not the actor. It
// cannot tell you the animation is not bound to anything (sequencer.list_tracks does), and it
// applies no retargeting, so an animation evaluated here is evaluated in its own skeleton's
// proportions rather than a particular mesh's.

#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/ParamSpec.h"
#include "Handlers/ErrorCodes.h"
#include "Handlers/Animation/MotionMetricsAnalysis.h"
#include "Utils/JsonBuilders.h"

#include "Animation/AnimSequence.h"
#include "Animation/AnimSequenceBase.h"
#include "Animation/Skeleton.h"
#include "Animation/AnimData/IAnimationDataModel.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "ReferenceSkeleton.h"

// Names from PinWrightMotionMetrics are qualified throughout rather than pulled in with a
// using-directive: this module builds with Unity enabled, where a file-scope using-directive
// leaks into every translation unit merged after it in the same blob.
namespace
{
    // Sample budget. 240 samples is eight seconds of a 30 fps clip at full rate, and every
    // metric here is a ratio against the sampled step, so a strided run stays internally
    // consistent - it just resolves less. The response always reports both counts.
    constexpr int32 DefaultMaxMotionSamples = 240;

    // Fractional mismatch between the animation's implied ground speed and the speed the
    // caller says the body actually travels at, above which the feet visibly skate. Derived
    // from what the number means rather than chosen: a 10% mismatch slips a tenth of a stride
    // per step, which on a half-metre stride is five centimetres of the foot sliding on the
    // ground every footfall.
    constexpr double MotionDefaultGroundSpeedTolerance = 0.1;

    UAnimSequenceBase* LoadAnimSequenceFromPathMotion(const FString& AssetPath, FString& OutError)
    {
        OutError.Reset();
        if (AssetPath.IsEmpty())
        {
            OutError = TEXT("Animation asset path is required");
            return nullptr;
        }
        UObject* Asset = StaticLoadObject(UAnimSequenceBase::StaticClass(), nullptr, *AssetPath);
        if (!Asset)
        {
            OutError = FString::Printf(TEXT("Failed to load animation sequence: %s"), *AssetPath);
            return nullptr;
        }
        UAnimSequenceBase* Sequence = Cast<UAnimSequenceBase>(Asset);
        if (!Sequence)
        {
            OutError = FString::Printf(TEXT("Asset is not an animation sequence: %s"), *AssetPath);
            return nullptr;
        }
        return Sequence;
    }

    TSharedPtr<FJsonObject> MakeMotionCheck(const TCHAR* Name, const TCHAR* Status)
    {
        TSharedPtr<FJsonObject> Check = MakeShared<FJsonObject>();
        Check->SetStringField(TEXT("name"), Name);
        Check->SetStringField(TEXT("status"), Status);
        return Check;
    }

    void MotionReadNameArray(const TSharedPtr<FJsonObject>& Payload, const TCHAR* Field, TArray<FName>& OutNames)
    {
        const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
        if (!Payload.IsValid() || !Payload->TryGetArrayField(Field, Values) || !Values)
        {
            return;
        }
        for (const TSharedPtr<FJsonValue>& Value : *Values)
        {
            FString Name;
            if (Value.IsValid() && Value->TryGetString(Name) && !Name.IsEmpty())
            {
                OutNames.AddUnique(FName(*Name));
            }
        }
    }
}

REGISTER_RPC_HANDLER("animation.measure_motion", "animation",
    "Numeric pass/fail gate over an AnimSequence's authored motion, sampled straight from the editor data model with no component, no world, no playhead and no rendering. The moving, loopSeam, and jitter checks are always present; a valid footBones name emits the locomotion check. When jitter is measurable it reports the measured frameFraction and maxRatio. Omitting maxJitterFraction leaves jitter at status reported; supplying it turns frameFraction into a pass/fail threshold. A measured foot row reports impliedGroundSpeed and stride values. When footBones resolves to a foot track with a usable stance, supplying expectedGroundSpeed adds slideFraction, per-foot pass fields, the locomotion pass/fail verdict, and suggestedPlayRate only for a failed foot; without it a measured locomotion check stays reported and those gate fields are absent. If no resolved foot track has a usable stance, locomotion is unmeasured regardless of expectedGroundSpeed and no foot-level gate fields are emitted. Every check reports status pass, fail, reported or unmeasured, and the top-level pass is false whenever any check could not be measured.",
    RPC_PARAMS(
        RPC_PARAM_REQ("assetPath", "path", "Path to the AnimSequence"),
        RPC_PARAM_OPT("bones", "array", "Bone names to measure (array of strings). Defaults to every bone the sequence carries a track for"),
        RPC_PARAM_OPT("footBones", "array", "Bone names to treat as feet (array of strings). Supplying these enables the locomotion measurement; omitting them omits the locomotion check"),
        RPC_PARAM_DEF("looping", "bool", "Whether this clip is a cycle. Controls both the loop-seam verdict and whether a stance window may wrap the end of the clip", "true"),
        RPC_PARAM_DEF("playRate", "number", "Play rate the implied ground speed is reported at. impliedGroundSpeed scales linearly with it, so this is how you check a play rate you already chose", "1.0"),
        RPC_PARAM_DEF("maxSamples", "number", "Sample budget. Above it the key range is strided uniformly and both counts are reported", "240"),
        RPC_PARAM_DEF("maxSeamRatio", "number", "Loop-seam gate, in units of normal frame steps. 0 (an explicit duplicate wrap key) and 1 (last key one step before the wrap) are both correct authoring, so the gate sits above both", "3.0"),
        RPC_PARAM_OPT("maxJitterFraction", "number", "Optional threshold for the measured jitter frameFraction. When omitted, jitter reports frameFraction and maxRatio with status reported; when supplied, jitter is pass or fail and echoes this threshold"),
        RPC_PARAM_OPT("expectedGroundSpeed", "number", "Optional body speed in world units per second. When footBones resolves to a foot track with a usable stance, supplying it adds slideFraction and per-foot pass fields, the locomotion pass/fail verdict, and a failed-foot suggestedPlayRate; if no resolved foot track has a usable stance, locomotion remains unmeasured and those fields are absent. Omitting it reports impliedGroundSpeed without a verdict"),
        RPC_PARAM_DEF("groundSpeedTolerance", "number", "Fractional mismatch between expected and implied ground speed that still passes", "0.1"),
        RPC_PARAM_DEF("plantBandFraction", "number", "Fraction of a foot's vertical travel, above its lowest point, still counted as planted", "0.15"),
        RPC_PARAM_DEF("maxReported", "number", "Per-bone rows reported per check", "8")
    ))
{
    FString AssetPath;
    if (!Ctx.RequireAssetPath(TEXT("assetPath"), AssetPath))
    {
        return true;
    }

    FString LoadError;
    UAnimSequenceBase* Sequence = LoadAnimSequenceFromPathMotion(AssetPath, LoadError);
    if (!Sequence)
    {
        Ctx.SendError(TEXT("ANIMATION_NOT_FOUND"), LoadError);
        return true;
    }

    USkeleton* Skeleton = Sequence->GetSkeleton();
    if (!Skeleton)
    {
        Ctx.SendError(TEXT("SKELETON_NOT_FOUND"),
            TEXT("The animation has no skeleton, so its local bone tracks cannot be accumulated into component space."));
        return true;
    }

    const IAnimationDataModel* Model = Sequence->GetDataModel();
    if (!Model)
    {
        // Not a pass with nothing found: there is no source data to read. A cooked-only asset
        // reaches here, and reporting that as a clean animation is the exact false confidence
        // this verb exists to prevent.
        Ctx.SendError(TEXT("ANIMATION_INVALID"),
            TEXT("The animation has no editor data model, so its authored bone tracks cannot be read. "
                 "This gate needs source data; a cooked-only asset cannot be measured."));
        return true;
    }

    const int32 KeyCount = Model->GetNumberOfKeys();
    const FFrameRate FrameRate = Model->GetFrameRate();
    if (KeyCount < 2 || FrameRate.Numerator <= 0 || FrameRate.Denominator <= 0)
    {
        Ctx.SendError(TEXT("ANIMATION_INVALID"),
            FString::Printf(TEXT("The animation has %d keys at frame rate %d/%d; at least two keys and a valid "
                "frame rate are needed to measure motion."),
                KeyCount, FrameRate.Numerator, FrameRate.Denominator));
        return true;
    }

    // ---- Parameters ----

    const bool bLooping = Ctx.GetBool(TEXT("looping"), true);
    const double PlayRate = Ctx.GetNumber(TEXT("playRate"), 1.0);
    if (!(PlayRate > 0.0))
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("playRate must be greater than zero"));
        return true;
    }
    const int32 MaxSamples = Ctx.GetInt(TEXT("maxSamples"), DefaultMaxMotionSamples);
    if (MaxSamples < 3)
    {
        // Jitter needs a second difference and locomotion needs a stance run; under three
        // samples both would report unmeasured and the call would be pure cost.
        Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("maxSamples must be at least 3"));
        return true;
    }
    const double MaxSeamRatio = Ctx.GetNumber(TEXT("maxSeamRatio"),
        PinWrightMotionMetrics::DefaultMaxSeamRatio);
    if (MaxSeamRatio < 0.0)
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("maxSeamRatio must not be negative"));
        return true;
    }
    const double PlantBandFraction = Ctx.GetNumber(TEXT("plantBandFraction"),
        PinWrightMotionMetrics::DefaultPlantBandFraction);
    if (PlantBandFraction <= 0.0 || PlantBandFraction >= 1.0)
    {
        // At 0 no sample is ever inside the band and no stance is found; at 1 every sample is,
        // and the "stance" spans the whole clip for an infinite implied speed.
        Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("plantBandFraction must be in (0, 1)"));
        return true;
    }
    const double GroundSpeedTolerance = Ctx.GetNumber(TEXT("groundSpeedTolerance"),
        MotionDefaultGroundSpeedTolerance);
    if (GroundSpeedTolerance < 0.0)
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("groundSpeedTolerance must not be negative"));
        return true;
    }
    const int32 MaxReported = FMath::Max(0, Ctx.GetInt(TEXT("maxReported"), 8));

    const TSharedPtr<FJsonObject>& Payload = Ctx.GetRawPayload();
    const bool bHasJitterThreshold = Payload.IsValid() && Payload->HasField(TEXT("maxJitterFraction"));
    const double MaxJitterFraction = Ctx.GetNumber(TEXT("maxJitterFraction"), 0.0);
    if (bHasJitterThreshold && (MaxJitterFraction < 0.0 || MaxJitterFraction > 1.0))
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("maxJitterFraction must be in [0, 1]"));
        return true;
    }
    const bool bHasExpectedSpeed = Payload.IsValid() && Payload->HasField(TEXT("expectedGroundSpeed"));
    const double ExpectedGroundSpeed = Ctx.GetNumber(TEXT("expectedGroundSpeed"), 0.0);
    if (bHasExpectedSpeed && !(ExpectedGroundSpeed > 0.0))
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"),
            TEXT("expectedGroundSpeed must be greater than zero; omit it to report the implied speed without a verdict"));
        return true;
    }

    TArray<FName> RequestedBones;
    MotionReadNameArray(Payload, TEXT("bones"), RequestedBones);
    TArray<FName> FootBones;
    MotionReadNameArray(Payload, TEXT("footBones"), FootBones);

    // ---- Sample the poses ----

    const FReferenceSkeleton& RefSkeleton = Skeleton->GetReferenceSkeleton();
    // Both counts, not just GetNum(): the bone-info array and the reference pose array are
    // supposed to be the same length, and indexing one by the other's count on a malformed
    // skeleton is an out-of-bounds read rather than a bad measurement.
    const int32 NumBones = FMath::Min(RefSkeleton.GetNum(), RefSkeleton.GetRefBonePose().Num());
    if (NumBones == 0)
    {
        Ctx.SendError(TEXT("SKELETON_NOT_FOUND"),
            TEXT("The animation's skeleton has no bones, so there is no hierarchy to accumulate into component space."));
        return true;
    }

    const int32 Stride = FMath::Max(1, FMath::DivideAndRoundUp(KeyCount, MaxSamples));
    TArray<FFrameNumber> SampleFrames;
    for (int32 Key = 0; Key < KeyCount; Key += Stride)
    {
        SampleFrames.Add(FFrameNumber(Key));
    }
    // The last key carries the loop seam. A stride that skipped it would compare the seam
    // against an interior frame and quietly report a clean loop on a popping one.
    if (SampleFrames.Num() > 0 && SampleFrames.Last().Value != KeyCount - 1)
    {
        SampleFrames.Add(FFrameNumber(KeyCount - 1));
    }
    const int32 SampleCount = SampleFrames.Num();
    const double SampleSeconds = FrameRate.AsInterval() * static_cast<double>(Stride) / PlayRate;

    TArray<FName> TrackNames;
    Model->GetBoneTrackNames(TrackNames);

    // Local-space transforms per bone per sample: the animation's track where it has one, the
    // reference pose where it does not. A bone with no track still has to participate, because
    // a foot's component-space height depends on every ancestor between it and the root.
    const TArray<FTransform>& RefPose = RefSkeleton.GetRefBonePose();
    TArray<TArray<FTransform>> LocalBySample;
    LocalBySample.SetNum(SampleCount);
    for (int32 Sample = 0; Sample < SampleCount; ++Sample)
    {
        LocalBySample[Sample] = RefPose;
    }

    int32 ResolvedTrackCount = 0;
    for (const FName& TrackName : TrackNames)
    {
        const int32 BoneIndex = RefSkeleton.FindBoneIndex(TrackName);
        if (BoneIndex == INDEX_NONE || BoneIndex >= NumBones)
        {
            // A track naming a bone the skeleton does not have. Counted through the response's
            // orphanTracks field rather than silently ignored, because it means the animation
            // and the skeleton have drifted apart.
            continue;
        }
        TArray<FTransform> Transforms;
        Model->GetBoneTrackTransforms(TrackName, SampleFrames, Transforms);
        if (Transforms.Num() != SampleCount)
        {
            continue;
        }
        ++ResolvedTrackCount;
        for (int32 Sample = 0; Sample < SampleCount; ++Sample)
        {
            LocalBySample[Sample][BoneIndex] = Transforms[Sample];
        }
    }

    // Which bones to report on. Parents always precede children in a reference skeleton, so
    // one forward pass per sample accumulates component space correctly.
    TArray<int32> MeasuredBoneIndices;
    TArray<FName> UnknownBones;
    if (RequestedBones.Num() > 0)
    {
        for (const FName& BoneName : RequestedBones)
        {
            const int32 BoneIndex = RefSkeleton.FindBoneIndex(BoneName);
            if (BoneIndex == INDEX_NONE || BoneIndex >= NumBones)
            {
                UnknownBones.Add(BoneName);
                continue;
            }
            MeasuredBoneIndices.AddUnique(BoneIndex);
        }
    }
    else
    {
        for (const FName& TrackName : TrackNames)
        {
            const int32 BoneIndex = RefSkeleton.FindBoneIndex(TrackName);
            if (BoneIndex != INDEX_NONE && BoneIndex < NumBones)
            {
                MeasuredBoneIndices.AddUnique(BoneIndex);
            }
        }
    }

    TArray<int32> FootBoneIndices;
    for (const FName& BoneName : FootBones)
    {
        const int32 BoneIndex = RefSkeleton.FindBoneIndex(BoneName);
        if (BoneIndex == INDEX_NONE || BoneIndex >= NumBones)
        {
            UnknownBones.AddUnique(BoneName);
            continue;
        }
        FootBoneIndices.AddUnique(BoneIndex);
        MeasuredBoneIndices.AddUnique(BoneIndex);
    }

    if (UnknownBones.Num() > 0)
    {
        // Naming a bone that does not exist is a caller error that would otherwise present as
        // a missing row in a results table nobody reads carefully.
        TArray<FString> Names;
        for (const FName& BoneName : UnknownBones)
        {
            Names.Add(BoneName.ToString());
        }
        Ctx.SendError(TEXT("BONE_NOT_FOUND"),
            FString::Printf(TEXT("Skeleton '%s' has no bone named: %s"),
                *Skeleton->GetName(), *FString::Join(Names, TEXT(", "))));
        return true;
    }

    if (MeasuredBoneIndices.Num() == 0)
    {
        Ctx.SendError(TEXT("ANIMATION_INVALID"),
            TEXT("The animation carries no bone tracks whose names match its skeleton, so there is nothing to "
                 "measure. Check animation.describe_sequence.boneTracks against skeleton.list_bones."));
        return true;
    }

    TMap<int32, int32> TrackSlotByBone;
    TArray<PinWrightMotionMetrics::FBoneTrack> BoneTracks;
    BoneTracks.SetNum(MeasuredBoneIndices.Num());
    for (int32 Slot = 0; Slot < MeasuredBoneIndices.Num(); ++Slot)
    {
        BoneTracks[Slot].BoneName = RefSkeleton.GetBoneName(MeasuredBoneIndices[Slot]);
        BoneTracks[Slot].Locations.Reserve(SampleCount);
        BoneTracks[Slot].Rotations.Reserve(SampleCount);
        TrackSlotByBone.Add(MeasuredBoneIndices[Slot], Slot);
    }

    TArray<FTransform> ComponentSpace;
    ComponentSpace.SetNum(NumBones);
    for (int32 Sample = 0; Sample < SampleCount; ++Sample)
    {
        const TArray<FTransform>& Local = LocalBySample[Sample];
        for (int32 BoneIndex = 0; BoneIndex < NumBones; ++BoneIndex)
        {
            const int32 ParentIndex = RefSkeleton.GetParentIndex(BoneIndex);
            ComponentSpace[BoneIndex] = (ParentIndex >= 0 && ParentIndex < BoneIndex)
                ? Local[BoneIndex] * ComponentSpace[ParentIndex]
                : Local[BoneIndex];
        }
        for (const TPair<int32, int32>& Pair : TrackSlotByBone)
        {
            BoneTracks[Pair.Value].Locations.Add(ComponentSpace[Pair.Key].GetLocation());
            BoneTracks[Pair.Value].Rotations.Add(ComponentSpace[Pair.Key].GetRotation());
        }
    }

    // ---- Run the checks ----

    TArray<TSharedPtr<FJsonValue>> CheckValues;
    int32 FailedChecks = 0;
    int32 UnmeasuredChecks = 0;
    TArray<FString> Warnings;

    PinWrightMotionMetrics::FFrozenStats FrozenStats;
    PinWrightMotionMetrics::AccumulateMotionExtent(BoneTracks, MaxReported, FrozenStats);

    {
        const bool bMeasured = FrozenStats.TrackCount > 0 && FrozenStats.SampleCount >= 2;
        TSharedPtr<FJsonObject> Check = MakeMotionCheck(TEXT("moving"),
            !bMeasured ? TEXT("unmeasured") : (FrozenStats.MovingBoneCount > 0 ? TEXT("pass") : TEXT("fail")));
        if (!bMeasured)
        {
            Check->SetStringField(TEXT("reason"),
                TEXT("Fewer than two samples were produced, so no displacement exists to measure."));
            ++UnmeasuredChecks;
        }
        else if (FrozenStats.MovingBoneCount == 0)
        {
            ++FailedChecks;
            Check->SetStringField(TEXT("note"),
                TEXT("No measured bone departs from its first-frame pose. A mesh playing this sequence "
                     "photographs identically at every frame, which no capture set can distinguish from "
                     "'animating slowly'."));
        }
        Check->SetNumberField(TEXT("measuredBones"), FrozenStats.TrackCount);
        Check->SetNumberField(TEXT("movingBones"), FrozenStats.MovingBoneCount);
        Check->SetNumberField(TEXT("maxLocationDelta"),
            JsonBuilders::SanitizeFinite(FrozenStats.MaxLocationDelta));
        Check->SetNumberField(TEXT("maxRotationDegrees"),
            JsonBuilders::SanitizeFinite(FrozenStats.MaxRotationDegrees));
        if (!FrozenStats.MostMovedBone.IsNone())
        {
            Check->SetStringField(TEXT("mostMovedBone"), FrozenStats.MostMovedBone.ToString());
        }

        TArray<TSharedPtr<FJsonValue>> ExtentValues;
        for (const PinWrightMotionMetrics::FMotionExtent& Extent : FrozenStats.Extents)
        {
            TSharedPtr<FJsonObject> ExtentObject = MakeShared<FJsonObject>();
            ExtentObject->SetStringField(TEXT("bone"), Extent.BoneName.ToString());
            ExtentObject->SetNumberField(TEXT("locationDelta"),
                JsonBuilders::SanitizeFinite(Extent.MaxLocationDelta));
            ExtentObject->SetNumberField(TEXT("rotationDegrees"),
                JsonBuilders::SanitizeFinite(Extent.MaxRotationDegrees));
            ExtentValues.Add(MakeShared<FJsonValueObject>(ExtentObject));
        }
        Check->SetArrayField(TEXT("bones"), ExtentValues);
        CheckValues.Add(MakeShared<FJsonValueObject>(Check));
    }

    {
        PinWrightMotionMetrics::FSeamStats SeamStats;
        PinWrightMotionMetrics::MeasureLoopSeam(BoneTracks, SeamStats);

        const double WorstRatio = FMath::Max(SeamStats.LocationRatio, SeamStats.RotationRatio);
        const TCHAR* Status = TEXT("unmeasured");
        if (SeamStats.bMeasurable)
        {
            // A clip the caller declared non-looping has no seam to judge: its first and last
            // poses are supposed to differ. The numbers are still emitted, because a caller who
            // mislabelled a cycle wants to see them.
            Status = !bLooping ? TEXT("reported") : (WorstRatio <= MaxSeamRatio ? TEXT("pass") : TEXT("fail"));
        }
        TSharedPtr<FJsonObject> Check = MakeMotionCheck(TEXT("loopSeam"), Status);
        if (!SeamStats.bMeasurable)
        {
            Check->SetStringField(TEXT("reason"),
                TEXT("No measured bone has a per-frame step above the static epsilon, so there is no frame step "
                     "to compare the seam against. This is the same condition the 'moving' check fails on."));
            ++UnmeasuredChecks;
        }
        else
        {
            if (bLooping && WorstRatio > MaxSeamRatio)
            {
                ++FailedChecks;
            }
            if (!bLooping)
            {
                Check->SetStringField(TEXT("note"),
                    TEXT("Reported without a verdict because looping was false: a one-shot clip is expected to "
                         "end somewhere other than where it started."));
            }
            Check->SetNumberField(TEXT("comparedBones"), SeamStats.ComparedBoneCount);
            Check->SetStringField(TEXT("worstBone"), SeamStats.WorstBone.ToString());
            Check->SetNumberField(TEXT("locationDelta"), JsonBuilders::SanitizeFinite(SeamStats.LocationDelta));
            Check->SetNumberField(TEXT("medianStepLocation"),
                JsonBuilders::SanitizeFinite(SeamStats.MedianStepLocation));
            Check->SetNumberField(TEXT("locationRatio"), JsonBuilders::SanitizeFinite(SeamStats.LocationRatio));
            Check->SetNumberField(TEXT("rotationDegrees"), JsonBuilders::SanitizeFinite(SeamStats.RotationDelta));
            Check->SetNumberField(TEXT("medianStepRotationDegrees"),
                JsonBuilders::SanitizeFinite(SeamStats.MedianStepRotation));
            Check->SetNumberField(TEXT("rotationRatio"), JsonBuilders::SanitizeFinite(SeamStats.RotationRatio));
            Check->SetNumberField(TEXT("maxSeamRatio"), MaxSeamRatio);
        }
        CheckValues.Add(MakeShared<FJsonValueObject>(Check));
    }

    {
        PinWrightMotionMetrics::FJitterStats JitterStats;
        PinWrightMotionMetrics::MeasureJitter(BoneTracks, JitterStats);

        const TCHAR* Status = TEXT("unmeasured");
        bool bFailed = false;
        if (JitterStats.bMeasurable)
        {
            if (!bHasJitterThreshold)
            {
                // Real animation has genuine velocity discontinuities - a footfall is one - so
                // a fixed fraction would flag correct work on some rigs and miss noise on
                // others. Reported, with the classifier's meaning stated, and gated only when
                // a caller has looked at their own numbers.
                Status = TEXT("reported");
            }
            else
            {
                bFailed = JitterStats.FrameFraction > MaxJitterFraction;
                Status = bFailed ? TEXT("fail") : TEXT("pass");
            }
        }
        TSharedPtr<FJsonObject> Check = MakeMotionCheck(TEXT("jitter"), Status);
        if (!JitterStats.bMeasurable)
        {
            Check->SetStringField(TEXT("reason"),
                TEXT("Fewer than three samples, or no bone with a measurable frame step, so no second "
                     "difference exists to normalize."));
            ++UnmeasuredChecks;
        }
        else
        {
            if (bFailed)
            {
                ++FailedChecks;
            }
            Check->SetNumberField(TEXT("comparedBones"), JitterStats.ComparedBoneCount);
            Check->SetNumberField(TEXT("evaluatedSamples"), JitterStats.EvaluatedSamples);
            Check->SetNumberField(TEXT("jitteringSamples"), JitterStats.JitteringSamples);
            Check->SetNumberField(TEXT("frameFraction"), JsonBuilders::SanitizeFinite(JitterStats.FrameFraction));
            Check->SetNumberField(TEXT("maxRatio"), JsonBuilders::SanitizeFinite(JitterStats.MaxRatio));
            if (!JitterStats.WorstBone.IsNone())
            {
                Check->SetStringField(TEXT("worstBone"), JitterStats.WorstBone.ToString());
                Check->SetNumberField(TEXT("worstSample"), JitterStats.WorstSample);
            }
            if (bHasJitterThreshold)
            {
                Check->SetNumberField(TEXT("maxJitterFraction"), MaxJitterFraction);
            }
            else
            {
                Check->SetStringField(TEXT("note"),
                    TEXT("Reported without a verdict. A sample counts as jittering when its second difference "
                         "exceeds twice the bone's median frame step, which is exactly a curve reversing "
                         "direction between samples. Genuine footfalls do that too, so pick maxJitterFraction "
                         "from the frameFraction a clip you trust produces, not from a guess."));
            }
        }
        CheckValues.Add(MakeShared<FJsonValueObject>(Check));
    }

    if (FootBoneIndices.Num() > 0)
    {
        TArray<TSharedPtr<FJsonValue>> FootValues;
        int32 MeasuredFeet = 0;
        int32 FailedFeet = 0;

        for (int32 FootBoneIndex : FootBoneIndices)
        {
            const int32* Slot = TrackSlotByBone.Find(FootBoneIndex);
            if (!Slot)
            {
                continue;
            }
            PinWrightMotionMetrics::FLocomotionStats Stats;
            PinWrightMotionMetrics::MeasureLocomotion(BoneTracks[*Slot], SampleSeconds, bLooping,
                PlantBandFraction, Stats);

            TSharedPtr<FJsonObject> FootObject = MakeShared<FJsonObject>();
            FootObject->SetStringField(TEXT("bone"), BoneTracks[*Slot].BoneName.ToString());
            FootObject->SetBoolField(TEXT("measured"), Stats.bMeasurable);
            FootObject->SetNumberField(TEXT("heightRange"), JsonBuilders::SanitizeFinite(Stats.HeightRange));

            if (!Stats.bMeasurable)
            {
                FootObject->SetStringField(TEXT("reason"),
                    FString::Printf(TEXT("This bone's component-space height varies by only %.3f units across the "
                        "clip, or no stance run of at least two samples fell inside the plant band. There is no "
                        "stance to separate from a swing, so no stride can be derived. Either this is not a "
                        "locomotion cycle, or the animation carries root motion and the feet are stationary "
                        "relative to the root - check animation.authoring.get_animation_info.hasRootMotion."),
                        Stats.HeightRange));
            }
            else
            {
                ++MeasuredFeet;
                FootObject->SetNumberField(TEXT("minHeight"), JsonBuilders::SanitizeFinite(Stats.MinHeight));
                FootObject->SetNumberField(TEXT("maxHeight"), JsonBuilders::SanitizeFinite(Stats.MaxHeight));
                FootObject->SetNumberField(TEXT("plantBandHeight"),
                    JsonBuilders::SanitizeFinite(Stats.PlantBandHeight));
                FootObject->SetNumberField(TEXT("stanceStartSample"), Stats.StanceStartSample);
                FootObject->SetNumberField(TEXT("stanceEndSample"), Stats.StanceEndSample);
                FootObject->SetNumberField(TEXT("stanceSamples"), Stats.StanceSampleCount);
                FootObject->SetBoolField(TEXT("stanceWrapped"), Stats.bStanceWrapped);
                // Reported because it changes the denominator of every speed below by one
                // sample, and a caller diffing two clips authored under different wrap-key
                // conventions must be able to see that rather than infer it.
                FootObject->SetBoolField(TEXT("droppedWrapSample"), Stats.bDroppedWrapSample);
                FootObject->SetNumberField(TEXT("cycleSeconds"), JsonBuilders::SanitizeFinite(Stats.CycleSeconds));
                FootObject->SetNumberField(TEXT("stanceSeconds"), JsonBuilders::SanitizeFinite(Stats.StanceSeconds));
                FootObject->SetNumberField(TEXT("stanceDisplacement"),
                    JsonBuilders::SanitizeFinite(Stats.StanceDisplacement));
                FootObject->SetNumberField(TEXT("impliedGroundSpeed"),
                    JsonBuilders::SanitizeFinite(Stats.ImpliedGroundSpeed));
                FootObject->SetNumberField(TEXT("stridePerCycle"),
                    JsonBuilders::SanitizeFinite(Stats.StridePerCycle));

                if (bHasExpectedSpeed)
                {
                    const double Mismatch =
                        FMath::Abs(Stats.ImpliedGroundSpeed - ExpectedGroundSpeed) / ExpectedGroundSpeed;
                    const bool bFootPassed = Mismatch <= GroundSpeedTolerance;
                    FootObject->SetNumberField(TEXT("slideFraction"), JsonBuilders::SanitizeFinite(Mismatch));
                    FootObject->SetBoolField(TEXT("pass"), bFootPassed);
                    if (!bFootPassed)
                    {
                        ++FailedFeet;
                        // The play rate that would reconcile the two, which is the number a
                        // caller actually needs next. A real foot-slide fix arrived at
                        // 1.0933333 by hand from exactly this ratio.
                        FootObject->SetNumberField(TEXT("suggestedPlayRate"),
                            JsonBuilders::SanitizeFinite(
                                Stats.ImpliedGroundSpeed > 0.0
                                    ? PlayRate * ExpectedGroundSpeed / Stats.ImpliedGroundSpeed
                                    : 0.0));
                    }
                }
            }
            FootValues.Add(MakeShared<FJsonValueObject>(FootObject));
        }

        const bool bMeasured = MeasuredFeet > 0;
        const TCHAR* Status = TEXT("unmeasured");
        if (bMeasured)
        {
            Status = !bHasExpectedSpeed ? TEXT("reported") : (FailedFeet == 0 ? TEXT("pass") : TEXT("fail"));
        }
        TSharedPtr<FJsonObject> Check = MakeMotionCheck(TEXT("locomotion"), Status);
        if (!bMeasured)
        {
            Check->SetStringField(TEXT("reason"),
                TEXT("No named foot bone produced a usable stance window; see each foot's own reason."));
            ++UnmeasuredChecks;
        }
        else if (bHasExpectedSpeed && FailedFeet > 0)
        {
            ++FailedChecks;
        }
        Check->SetNumberField(TEXT("measuredFeet"), MeasuredFeet);
        Check->SetNumberField(TEXT("playRate"), PlayRate);
        if (bHasExpectedSpeed)
        {
            Check->SetNumberField(TEXT("expectedGroundSpeed"), ExpectedGroundSpeed);
            Check->SetNumberField(TEXT("groundSpeedTolerance"), GroundSpeedTolerance);
        }
        else
        {
            Check->SetStringField(TEXT("note"),
                TEXT("Reported without a verdict because no expectedGroundSpeed was supplied. impliedGroundSpeed "
                     "is what this animation wants the body to travel at, at the given playRate; compare it with "
                     "the translation the transform track actually applies and the difference is the foot slide."));
        }
        Check->SetArrayField(TEXT("feet"), FootValues);
        CheckValues.Add(MakeShared<FJsonValueObject>(Check));
    }

    // ---- Response ----

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("measurement"), TEXT("motion"));
    Result->SetStringField(TEXT("assetPath"), AssetPath);
    Result->SetStringField(TEXT("skeleton"), Skeleton->GetPathName());
    Result->SetStringField(TEXT("poseSource"), TEXT("dataModelRaw"));
    Result->SetNumberField(TEXT("keyCount"), KeyCount);
    Result->SetNumberField(TEXT("sampleCount"), SampleCount);
    Result->SetNumberField(TEXT("sampleStride"), Stride);
    Result->SetBoolField(TEXT("exhaustive"), Stride == 1);
    Result->SetNumberField(TEXT("frameRate"), FrameRate.AsDecimal());
    Result->SetNumberField(TEXT("sampleSeconds"), JsonBuilders::SanitizeFinite(SampleSeconds));
    Result->SetNumberField(TEXT("playRate"), PlayRate);
    Result->SetBoolField(TEXT("looping"), bLooping);
    Result->SetNumberField(TEXT("boneTrackCount"), TrackNames.Num());
    Result->SetNumberField(TEXT("resolvedTrackCount"), ResolvedTrackCount);
    Result->SetNumberField(TEXT("measuredBoneCount"), BoneTracks.Num());
    Result->SetArrayField(TEXT("checks"), CheckValues);
    Result->SetNumberField(TEXT("failedChecks"), FailedChecks);
    Result->SetNumberField(TEXT("unmeasuredChecks"), UnmeasuredChecks);

    // An unmeasured check counts against the verdict exactly as a failed one does. "I could
    // not look" and "I looked and it was fine" must never produce the same pass:true.
    const bool bPass = FailedChecks == 0 && UnmeasuredChecks == 0;
    Result->SetBoolField(TEXT("pass"), bPass);

    if (ResolvedTrackCount < TrackNames.Num())
    {
        Warnings.Add(FString::Printf(
            TEXT("%d of %d bone tracks were ignored: they name a bone the skeleton does not have, or the data "
                 "model returned a different number of transforms than frames requested. Compare "
                 "animation.describe_sequence.boneTracks with skeleton.list_bones."),
            TrackNames.Num() - ResolvedTrackCount, TrackNames.Num()));
    }
    if (Stride > 1)
    {
        Warnings.Add(FString::Printf(
            TEXT("Sampled every %d keys to stay inside maxSamples. Every ratio here is relative to the SAMPLED "
                 "frame step, so the numbers are internally consistent but a jitter spike lasting fewer than %d "
                 "keys can fall between samples."),
            Stride, Stride));
    }
    if (UnmeasuredChecks > 0)
    {
        Warnings.Add(FString::Printf(
            TEXT("%d of the checks could not be measured, so this result is not evidence of sound motion. "
                 "Read each check's status and reason."), UnmeasuredChecks));
    }
    if (Warnings.Num() > 0)
    {
        TArray<TSharedPtr<FJsonValue>> WarningValues;
        for (const FString& Warning : Warnings)
        {
            WarningValues.Add(MakeShared<FJsonValueString>(Warning));
        }
        Result->SetArrayField(TEXT("warnings"), WarningValues);
    }

    Ctx.SendSuccess(Result);
    return true;
}
