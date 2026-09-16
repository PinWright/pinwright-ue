// Copyright (c) 2026 Alexander Penkin. MIT License.

// SequencerMotionMeasureHandler.cpp - sequencer.measure_motion.
//
// The judge side of the transform-keyframe write path. sequence.add_keyframe /
// sequencer.add_keyframes author the numbers and sequencer.list_sections {includeKeys:true} reads
// them back; between them a caller can verify every AUTHORED number on a 3D transform track and
// still ship a camera move that reads as jagged, because nothing in the namespace reported a
// DERIVATIVE. This verb does, mirroring animation.measure_motion's check/status shape.
//
// See SequencerMotionAnalysis.h for the evaluator, the units and what this cannot see. The two
// contracts worth repeating at the call site:
//
//   1. IT MEASURES ONE TRANSFORM SECTION'S CHANNELS, NOT THE COMPOSITED POSE. Attachment to a
//      moving parent, an overlapping blended section, a rig rail — none of it is here. The
//      response says so in `source`, because a number that silently means something narrower
//      than the caller assumed is worse than no number.
//   2. IT IS NOT A SMOOTHNESS SCORE. A camera with uniformly continuous velocity and zero jerk is
//      a floaty camera with no opinion; real camera work decelerates into a subject and cuts.
//      speed / acceleration / jerk / angular are therefore `reported`, never gated. curvature
//      gates only when the caller supplies minTurnRadius. loopSeam is the one genuine pass/fail,
//      because a seam is a hidden cut and any mismatch there announces the join.

#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/ParamSpec.h"
#include "Handlers/Sequencer/SequencerMotionAnalysis.h"
#include "Utils/JsonBuilders.h"
#include "Utils/AssetUtils.h"
#include "Utils/MovieSceneJsonUtils.h"

#include "Channels/MovieSceneChannelProxy.h"
#include "Channels/MovieSceneDoubleChannel.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/IConsoleManager.h"
#include "LevelSequence.h"
#include "MovieScene.h"
#include "Sections/MovieScene3DTransformSection.h"
#include "Tracks/MovieScene3DTransformTrack.h"

// Names from PinWrightSequencerMotion are qualified throughout rather than pulled in with a
// using-directive: this module builds with Unity enabled, where a file-scope using-directive
// leaks into every translation unit merged after it in the same blob.
namespace
{
    // Sample budget. Above it the grid is widened and the effective rate is reported, the way
    // animation.measure_motion strides its key range - every metric here is either analytic or a
    // min/max/mean over the grid, so a coarser grid resolves less without becoming inconsistent.
    constexpr int32 DefaultMaxSequencerMotionSamples = 1200;

    // Rows emitted per per-key / per-segment table before truncation.
    constexpr int32 DefaultMaxReportedRows = 64;

    // Transform section channel layout: 0-2 Location, 3-5 Rotation (Roll, Pitch, Yaw), 6-8 Scale.
    constexpr int32 LocationChannelBase = 0;
    constexpr int32 RotationChannelBase = 3;
    constexpr int32 TransformChannelCount = 9;

    TSharedPtr<FJsonObject> MakeSequencerMotionCheck(const TCHAR* Name, const TCHAR* Status)
    {
        TSharedPtr<FJsonObject> Check = MakeShared<FJsonObject>();
        Check->SetStringField(TEXT("name"), Name);
        Check->SetStringField(TEXT("status"), Status);
        return Check;
    }

    // Copy one double channel's authored keys into the analysis form. Returns false when any key
    // carries a weighted tangent: FWeightedCubicInterpolation reparameterizes the segment, and
    // reading such a curve through the unweighted Bezier basis would produce numbers that look
    // right and are not. Refusing to measure is the only honest answer.
    bool ReadCurveChannel(const FMovieSceneDoubleChannel* Channel,
        PinWrightSequencerMotion::FCurveChannel& OutChannel, bool bLinearActsAsCubic)
    {
        OutChannel.Keys.Reset();
        OutChannel.bLinearActsAsCubic = bLinearActsAsCubic;
        if (!Channel)
        {
            return true;
        }
        const TArrayView<const FFrameNumber> Times = Channel->GetData().GetTimes();
        const TArrayView<const FMovieSceneDoubleValue> Values = Channel->GetData().GetValues();
        const int32 Count = FMath::Min(Times.Num(), Values.Num());
        OutChannel.Keys.Reserve(Count);
        bool bUnweighted = true;
        for (int32 KeyIndex = 0; KeyIndex < Count; ++KeyIndex)
        {
            const FMovieSceneDoubleValue& Value = Values[KeyIndex];
            if (Value.Tangent.TangentWeightMode != RCTWM_WeightedNone)
            {
                bUnweighted = false;
            }
            PinWrightSequencerMotion::FCurveKey& Key = OutChannel.Keys.AddDefaulted_GetRef();
            Key.Tick = Times[KeyIndex].Value;
            Key.Value = Value.Value;
            Key.ArriveTangent = Value.Tangent.ArriveTangent;
            Key.LeaveTangent = Value.Tangent.LeaveTangent;
            Key.Interp = static_cast<ERichCurveInterpMode>(Value.InterpMode.GetValue());
        }
        return bUnweighted;
    }

    TSharedPtr<FJsonObject> MakeTickFrameJson(int32 Tick, const FFrameRate& TickResolution,
        const FFrameRate& DisplayRate)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetNumberField(TEXT("tick"), Tick);
        Obj->SetNumberField(TEXT("frame"),
            FFrameRate::TransformTime(FFrameTime(FFrameNumber(Tick)), TickResolution, DisplayRate)
                .AsDecimal());
        return Obj;
    }
}

REGISTER_RPC_HANDLER("sequencer.measure_motion", "Sequencer",
    "Measure the MOTION of one binding's 3D transform track: speed, acceleration, jerk, path curvature, angular rate and loop-seam velocity continuity. Every other reader in this namespace reports authored numbers (keys, values, tangent modes, tangent values); none reports a derivative, so a camera can pass all of them and still read as jagged. Derivatives are computed ANALYTICALLY from the cubic Bezier basis the engine itself evaluates - not by differencing a bake, whose second and third derivatives are float32 noise divided by dt^2 and dt^3 - so segment jerk is exact rather than sampled. Reports in per-SECOND units throughout (uu/s, uu/s^2, uu/s^3, deg/s, deg/s^2, turn radius in uu) even though the underlying tangents are stored per TICK. This is deliberately NOT a smoothness score: a camera with zero jerk everywhere is a floaty camera with no opinion, and real camera work decelerates into a subject and cuts. speed, acceleration, jerk and angular are always status reported; curvature gates only when minTurnRadius is supplied; loopSeam is the one genuine pass/fail, and it fails on a stopped seam as well as a mismatched one - a camera halted on BOTH sides has a mismatch of zero and is the exact defect a seam check exists to catch. Measures ONE transform section's own channels, not the composited pose: a parent attachment, an overlapping blended section or a rig rail is invisible here (use sequencer.get_binding_transform for a composited pose at one frame). Every check reports status pass, fail, reported or unmeasured, and the top-level pass is false whenever any check failed or could not be measured.",
    RPC_PARAMS(
        RPC_PARAM_REQ("path", "path", "Level sequence asset path"),
        RPC_PARAM_REQ("bindingId", "string", "GUID of the binding whose transform track is measured (from sequencer.get_bindings or sequencer.add_actor)"),
        RPC_PARAM_DEF("looping", "bool", "Whether this move is a cycle. Controls only the loopSeam verdict: on a one-shot the seam numbers are still reported, without a pass/fail, because a one-shot is expected to end somewhere other than where it started", "true"),
        RPC_PARAM_OPT("sampleRate", "number", "Grid samples per second. Defaults to the sequence's display rate. Every key tick is added to the grid regardless, so the tightest corner - which is usually at a key - is never sampled past"),
        RPC_PARAM_DEF("maxSamples", "number", "Grid budget. Above it the step is widened to fit and the effective sample rate is reported", "1200"),
        RPC_PARAM_DEF("maxSeamVelocityMismatch", "number", "loopSeam gate: |v_out - v_in| over the mean of the two seam speeds. Relative, so it is invariant to how fast the move is", "0.05"),
        RPC_PARAM_OPT("minTurnRadius", "number", "Optional minimum turn radius in world units. Supplying it turns curvature from reported into pass/fail; omitting it reports the tightest radius and where it is without a verdict. A radius far below the distance to the subject is a hairpin, and shrinking a tangent at a key where the path also turns is the one-call way to author one"),
        RPC_PARAM_DEF("sectionIndex", "integer", "Which section of the transform track to measure. Overlapping sections blend at evaluation time and cannot be differentiated analytically, so exactly one is measured and a multi-section track is reported as a warning", "0"),
        RPC_PARAM_DEF("maxReported", "number", "Rows emitted per per-key and per-segment table before truncation", "64")
    ))
{
    FString AssetPath;
    if (!Ctx.RequireAssetPath(TEXT("path"), AssetPath))
    {
        return true;
    }
    FString BindingIdStr;
    if (!Ctx.RequireString(TEXT("bindingId"), BindingIdStr))
    {
        return true;
    }
    FGuid BindingGuid;
    if (!FGuid::Parse(BindingIdStr, BindingGuid) || !BindingGuid.IsValid())
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"),
            FString::Printf(TEXT("bindingId '%s' is not a GUID. Take it from sequencer.get_bindings."),
                *BindingIdStr));
        return true;
    }

    // ---- Parameters ----

    const bool bLooping = Ctx.GetBool(TEXT("looping"), true);
    const int32 MaxSamples = Ctx.GetInt(TEXT("maxSamples"), DefaultMaxSequencerMotionSamples);
    if (MaxSamples < 3)
    {
        // Under three grid samples there is no interior to find a min speed or a corner in.
        Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("maxSamples must be at least 3"));
        return true;
    }
    const double MaxSeamVelocityMismatch = Ctx.GetNumber(TEXT("maxSeamVelocityMismatch"),
        PinWrightSequencerMotion::DefaultMaxSeamVelocityMismatch);
    if (MaxSeamVelocityMismatch < 0.0)
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("maxSeamVelocityMismatch must not be negative"));
        return true;
    }
    const int32 MaxReported = FMath::Max(0, Ctx.GetInt(TEXT("maxReported"), DefaultMaxReportedRows));
    const int32 SectionIndex = Ctx.GetInt(TEXT("sectionIndex"), 0);

    const TSharedPtr<FJsonObject>& Payload = Ctx.GetRawPayload();
    const bool bHasMinTurnRadius = Payload.IsValid() && Payload->HasField(TEXT("minTurnRadius"));
    const double MinTurnRadius = Ctx.GetNumber(TEXT("minTurnRadius"), 0.0);
    if (bHasMinTurnRadius && !(MinTurnRadius > 0.0))
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"),
            TEXT("minTurnRadius must be greater than zero; omit it to report the tightest radius without a verdict"));
        return true;
    }
    const bool bHasSampleRate = Payload.IsValid() && Payload->HasField(TEXT("sampleRate"));
    const double RequestedSampleRate = Ctx.GetNumber(TEXT("sampleRate"), 0.0);
    if (bHasSampleRate && !(RequestedSampleRate > 0.0))
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("sampleRate must be greater than zero"));
        return true;
    }

    // ---- Resolve the track's channels ----

    UObject* Asset = ResolveAsset(AssetPath, /*bLoadObject=*/true).Object;
    if (!Asset)
    {
        Ctx.SendError(TEXT("SEQUENCE_NOT_FOUND"),
            FString::Printf(TEXT("Sequence not found: %s"), *AssetPath));
        return true;
    }
    ULevelSequence* LevelSeq = Cast<ULevelSequence>(Asset);
    if (!LevelSeq)
    {
        Ctx.SendError(TEXT("INVALID_SEQUENCE_TYPE"), TEXT("Sequence object is not a LevelSequence"));
        return true;
    }
    UMovieScene* MovieScene = LevelSeq->GetMovieScene();
    if (!MovieScene)
    {
        Ctx.SendError(TEXT("INVALID_SEQUENCE"), TEXT("Sequence has no MovieScene"));
        return true;
    }
    if (!MovieScene->FindBinding(BindingGuid))
    {
        Ctx.SendError(TEXT("BINDING_NOT_FOUND"),
            FString::Printf(TEXT("No binding '%s' in %s. List them with sequencer.get_bindings."),
                *BindingIdStr, *AssetPath));
        return true;
    }

    UMovieScene3DTransformTrack* Track =
        MovieScene->FindTrack<UMovieScene3DTransformTrack>(BindingGuid, FName("Transform"));
    if (!Track)
    {
        Ctx.SendError(TEXT("TRACK_NOT_FOUND"),
            TEXT("The binding carries no 3D transform track, so it has no authored motion to measure. "
                 "sequence.add_keyframe / sequencer.add_keyframes create one."));
        return true;
    }
    const TArray<UMovieSceneSection*>& Sections = Track->GetAllSections();
    if (!Sections.IsValidIndex(SectionIndex))
    {
        Ctx.SendError(TEXT("SECTION_NOT_FOUND"),
            FString::Printf(TEXT("sectionIndex %d is out of range: the transform track has %d section(s)."),
                SectionIndex, Sections.Num()));
        return true;
    }
    UMovieScene3DTransformSection* Section = Cast<UMovieScene3DTransformSection>(Sections[SectionIndex]);
    if (!Section)
    {
        Ctx.SendError(TEXT("SECTION_TYPE_MISMATCH"),
            TEXT("The selected section is not a 3D transform section."));
        return true;
    }
    TArrayView<FMovieSceneDoubleChannel*> Channels =
        Section->GetChannelProxy().GetChannels<FMovieSceneDoubleChannel>();
    if (Channels.Num() < TransformChannelCount)
    {
        Ctx.SendError(TEXT("SECTION_TYPE_MISMATCH"),
            FString::Printf(TEXT("The transform section exposes %d double channels; nine are needed "
                "(0-2 Location, 3-5 Rotation, 6-8 Scale)."), Channels.Num()));
        return true;
    }

    // The engine promotes a linear key whose successor is cubic to a cubic segment when this cvar
    // is set (MovieSceneCurveChannelImpl.cpp). Read it rather than assume it, or the evaluator
    // stops agreeing with what the viewport plays.
    bool bLinearActsAsCubic = true;
    if (const IConsoleVariable* CVar =
        IConsoleManager::Get().FindConsoleVariable(TEXT("Sequencer.LinearCubicInterpolation")))
    {
        bLinearActsAsCubic = CVar->GetInt() != 0;
    }

    PinWrightSequencerMotion::FVectorChannel Location;
    PinWrightSequencerMotion::FVectorChannel Rotation;
    bool bUnweighted = true;
    for (int32 AxisIndex = 0; AxisIndex < 3; ++AxisIndex)
    {
        bUnweighted &= ReadCurveChannel(Channels[LocationChannelBase + AxisIndex],
            Location.Axis[AxisIndex], bLinearActsAsCubic);
        bUnweighted &= ReadCurveChannel(Channels[RotationChannelBase + AxisIndex],
            Rotation.Axis[AxisIndex], bLinearActsAsCubic);
    }

    TArray<int32> LocationKeyTicks;
    PinWrightSequencerMotion::CollectKeyTicks(Location, LocationKeyTicks);
    TArray<int32> RotationKeyTicks;
    PinWrightSequencerMotion::CollectKeyTicks(Rotation, RotationKeyTicks);

    // The measured domain is the authored one: outside the outermost key every channel holds
    // constant, and including that dead air would inject a fabricated stop into every profile.
    TArray<int32> DomainTicks = LocationKeyTicks;
    for (const int32 Tick : RotationKeyTicks)
    {
        DomainTicks.AddUnique(Tick);
    }
    DomainTicks.Sort();
    if (DomainTicks.Num() < 2)
    {
        Ctx.SendError(TEXT("SEQUENCE_INVALID"),
            FString::Printf(TEXT("The transform section carries %d distinct keyed tick(s) across its "
                "location and rotation channels; at least two are needed for a derivative to exist."),
                DomainTicks.Num()));
        return true;
    }
    const int32 StartTick = DomainTicks[0];
    const int32 EndTick = DomainTicks.Last();

    const FFrameRate TickResolution = MovieScene->GetTickResolution();
    const FFrameRate DisplayRate = MovieScene->GetDisplayRate();
    const double TicksPerSecond = TickResolution.AsDecimal();
    if (!(TicksPerSecond > 0.0))
    {
        Ctx.SendError(TEXT("SEQUENCE_INVALID"),
            TEXT("The sequence has a zero tick resolution, so ticks cannot be converted to seconds "
                 "and no per-second derivative can be reported."));
        return true;
    }

    // ---- Build the sampling grid ----

    const double SampleRate = bHasSampleRate ? RequestedSampleRate
                                             : FMath::Max(DisplayRate.AsDecimal(), 1.0);
    const double SpanTicks = static_cast<double>(EndTick - StartTick);
    // Clamped into [1, span] before it is ever an int32: FMath::RoundToInt/CeilToInt on a double
    // return int64, and a sampleRate near zero would otherwise produce a step that overflows the
    // grid loop's own counter rather than just being coarse.
    const int64 MaxStepTicks = FMath::Max<int64>(1, static_cast<int64>(EndTick) - StartTick);
    int64 StepTicks64 = FMath::RoundToInt64(TicksPerSecond / SampleRate);
    if (SpanTicks / static_cast<double>(FMath::Max<int64>(1, StepTicks64)) + 1.0
        > static_cast<double>(MaxSamples))
    {
        StepTicks64 = FMath::CeilToInt64(SpanTicks / static_cast<double>(MaxSamples - 1));
    }
    const int32 StepTicks = static_cast<int32>(FMath::Clamp<int64>(StepTicks64, 1, MaxStepTicks));

    TArray<int32> GridTicks;
    for (int32 Tick = StartTick; Tick < EndTick; Tick += StepTicks)
    {
        GridTicks.Add(Tick);
    }
    GridTicks.Add(EndTick);
    // Key ticks join the grid unconditionally: the tightest corner of a path is almost always at
    // a key, and a uniform grid that steps over it reports a radius the move does not have.
    for (const int32 Tick : DomainTicks)
    {
        GridTicks.AddUnique(Tick);
    }
    GridTicks.Sort();
    const double EffectiveSampleRate = TicksPerSecond / static_cast<double>(StepTicks);

    TArray<PinWrightSequencerMotion::FPathSample> Samples;
    PinWrightSequencerMotion::BuildPathSamples(Location, GridTicks, TicksPerSecond, Samples);

    PinWrightSequencerMotion::FSpeedStats SpeedStats;
    PinWrightSequencerMotion::MeasureSpeed(Samples, SpeedStats);

    TArray<PinWrightSequencerMotion::FKeyMotion> KeyMotion;
    PinWrightSequencerMotion::MeasureKeys(Location, LocationKeyTicks, TicksPerSecond, KeyMotion);

    // ---- Run the checks ----

    TArray<TSharedPtr<FJsonValue>> CheckValues;
    int32 FailedChecks = 0;
    int32 UnmeasuredChecks = 0;
    TArray<FString> Warnings;

    // A weighted tangent anywhere invalidates every derivative on this section, so the whole set
    // is reported unmeasured rather than half of it silently reading from the wrong basis.
    const TCHAR* const WeightedReason =
        TEXT("A key on this section carries a weighted tangent (RCTWM_Weighted*). Weighted tangents "
             "reparameterize the segment through FWeightedCubicInterpolation, and reading them "
             "through the unweighted Bezier basis produces plausible numbers that are wrong. "
             "Re-author those keys with tangentMode user or break, whose weights are unset.");
    const bool bLocationMeasurable = bUnweighted && LocationKeyTicks.Num() >= 2;

    {
        const TCHAR* Status = bLocationMeasurable && SpeedStats.bMeasurable
            ? TEXT("reported") : TEXT("unmeasured");
        TSharedPtr<FJsonObject> Check = MakeSequencerMotionCheck(TEXT("speed"), Status);
        if (!bLocationMeasurable || !SpeedStats.bMeasurable)
        {
            Check->SetStringField(TEXT("reason"), !bUnweighted ? WeightedReason
                : TEXT("Fewer than two keyed ticks on the location channels, so no positional "
                       "derivative exists."));
            ++UnmeasuredChecks;
        }
        else
        {
            Check->SetNumberField(TEXT("min"), JsonBuilders::SanitizeFinite(SpeedStats.Min));
            Check->SetNumberField(TEXT("max"), JsonBuilders::SanitizeFinite(SpeedStats.Max));
            Check->SetNumberField(TEXT("mean"), JsonBuilders::SanitizeFinite(SpeedStats.Mean));
            Check->SetNumberField(TEXT("ratio"), JsonBuilders::SanitizeFinite(SpeedStats.Ratio));
            Check->SetNumberField(TEXT("pathLength"), JsonBuilders::SanitizeFinite(SpeedStats.PathLength));
            Check->SetObjectField(TEXT("minAt"),
                MakeTickFrameJson(SpeedStats.MinTick, TickResolution, DisplayRate));
            Check->SetObjectField(TEXT("maxAt"),
                MakeTickFrameJson(SpeedStats.MaxTick, TickResolution, DisplayRate));
            Check->SetStringField(TEXT("note"),
                TEXT("Reported without a verdict. Read the per-key speeds: a key sitting at a local "
                     "minimum approaching zero is the 'stops dead at every keyframe' defect, which "
                     "ships whenever cubic/auto keys are written without their tangents being solved."));

            TArray<TSharedPtr<FJsonValue>> KeyValues;
            for (int32 Index = 0; Index < KeyMotion.Num() && Index < MaxReported; ++Index)
            {
                const PinWrightSequencerMotion::FKeyMotion& Key = KeyMotion[Index];
                TSharedPtr<FJsonObject> Row =
                    MakeTickFrameJson(Key.Tick, TickResolution, DisplayRate);
                Row->SetNumberField(TEXT("arriveSpeed"), JsonBuilders::SanitizeFinite(Key.ArriveSpeed));
                Row->SetNumberField(TEXT("leaveSpeed"), JsonBuilders::SanitizeFinite(Key.LeaveSpeed));
                KeyValues.Add(MakeShared<FJsonValueObject>(Row));
            }
            Check->SetArrayField(TEXT("keys"), KeyValues);
            Check->SetNumberField(TEXT("keyCount"), KeyMotion.Num());
        }
        CheckValues.Add(MakeShared<FJsonValueObject>(Check));
    }

    {
        PinWrightSequencerMotion::FAccelerationStats AccelStats;
        PinWrightSequencerMotion::SummarizeAccelerationSteps(KeyMotion, AccelStats);

        // "No interior key" is a MEASUREMENT, not a blind spot: an acceleration step is the
        // discontinuity BETWEEN two segments, so a two-key move has exactly one segment and
        // genuinely has none. Reporting that as unmeasured would fail the verdict on the most
        // ordinary move there is. A weighted tangent, by contrast, really is a blind spot.
        TSharedPtr<FJsonObject> Check = MakeSequencerMotionCheck(TEXT("acceleration"),
            bLocationMeasurable ? TEXT("reported") : TEXT("unmeasured"));
        if (!bLocationMeasurable)
        {
            Check->SetStringField(TEXT("reason"), !bUnweighted ? WeightedReason
                : TEXT("Fewer than two keyed ticks on the location channels, so there are no "
                       "segments to find a discontinuity between."));
            ++UnmeasuredChecks;
        }
        else if (!AccelStats.bMeasurable)
        {
            Check->SetNumberField(TEXT("interiorKeyCount"), 0);
            Check->SetStringField(TEXT("note"),
                TEXT("No interior key. The acceleration step is the discontinuity BETWEEN two "
                     "segments, so a move with a single segment has none to report - this is a "
                     "measurement, not a gap."));
        }
        else
        {
            Check->SetNumberField(TEXT("interiorKeyCount"), AccelStats.InteriorKeyCount);
            Check->SetNumberField(TEXT("medianStep"), JsonBuilders::SanitizeFinite(AccelStats.MedianStep));
            Check->SetNumberField(TEXT("maxStep"), JsonBuilders::SanitizeFinite(AccelStats.MaxStep));
            Check->SetNumberField(TEXT("maxRelativeStep"),
                JsonBuilders::SanitizeFinite(AccelStats.MaxRelativeStep));
            Check->SetObjectField(TEXT("maxStepAt"),
                MakeTickFrameJson(AccelStats.WorstTick, TickResolution, DisplayRate));
            Check->SetStringField(TEXT("note"),
                TEXT("Reported without a verdict, and deliberately: cubic Hermite curves are C1 and "
                     "not C2, so a step at every key is EXPECTED rather than a fault. The useful "
                     "output is the distribution and the outliers - for each one, ask what beat it "
                     "is. Gating on low acceleration optimises a film flat."));

            TArray<TSharedPtr<FJsonValue>> KeyValues;
            for (int32 Index = 0; Index < KeyMotion.Num() && Index < MaxReported; ++Index)
            {
                const PinWrightSequencerMotion::FKeyMotion& Key = KeyMotion[Index];
                if (!Key.bInterior)
                {
                    continue;
                }
                TSharedPtr<FJsonObject> Row =
                    MakeTickFrameJson(Key.Tick, TickResolution, DisplayRate);
                Row->SetObjectField(TEXT("arrive"), JsonBuilders::BuildVectorJson(Key.ArriveAcceleration));
                Row->SetObjectField(TEXT("leave"), JsonBuilders::BuildVectorJson(Key.LeaveAcceleration));
                Row->SetNumberField(TEXT("step"), JsonBuilders::SanitizeFinite(Key.AccelerationStep));
                Row->SetNumberField(TEXT("relativeStep"),
                    JsonBuilders::SanitizeFinite(Key.RelativeAccelerationStep));
                KeyValues.Add(MakeShared<FJsonValueObject>(Row));
            }
            Check->SetArrayField(TEXT("keys"), KeyValues);
        }
        CheckValues.Add(MakeShared<FJsonValueObject>(Check));
    }

    {
        TArray<PinWrightSequencerMotion::FSegmentJerk> Segments;
        PinWrightSequencerMotion::MeasureJerk(Location, LocationKeyTicks, TicksPerSecond, Segments);

        const bool bMeasurable = bLocationMeasurable && Segments.Num() > 0;
        TSharedPtr<FJsonObject> Check =
            MakeSequencerMotionCheck(TEXT("jerk"), bMeasurable ? TEXT("reported") : TEXT("unmeasured"));
        if (!bMeasurable)
        {
            Check->SetStringField(TEXT("reason"), !bUnweighted ? WeightedReason
                : TEXT("No segment of non-zero length between two location keys."));
            ++UnmeasuredChecks;
        }
        else
        {
            PinWrightSequencerMotion::FJerkStats JerkStats;
            PinWrightSequencerMotion::SummarizeJerk(Segments, JerkStats);
            Check->SetNumberField(TEXT("median"), JsonBuilders::SanitizeFinite(JerkStats.Median));
            Check->SetNumberField(TEXT("max"), JsonBuilders::SanitizeFinite(JerkStats.Max));
            Check->SetObjectField(TEXT("maxAt"),
                MakeTickFrameJson(JerkStats.MaxStartTick, TickResolution, DisplayRate));
            // The worst segment's duration next to its magnitude, because an outlier on a short
            // segment is a spacing artefact rather than a shape problem.
            Check->SetNumberField(TEXT("maxSegmentDurationSeconds"),
                JsonBuilders::SanitizeFinite(JerkStats.MaxDurationSeconds));
            Check->SetNumberField(TEXT("segmentCount"), Segments.Num());
            Check->SetStringField(TEXT("note"),
                TEXT("Exact, not sampled: jerk is constant within a cubic segment. Read it ALONGSIDE "
                     "durationSeconds - jerk scales as 1/h^3, so a segment half its neighbours' "
                     "length carries eight times their jerk for the same authored shape. An outlier "
                     "on a short segment is a re-timing problem, not a re-shaping one."));

            TArray<TSharedPtr<FJsonValue>> SegmentValues;
            for (int32 Index = 0; Index < Segments.Num() && Index < MaxReported; ++Index)
            {
                const PinWrightSequencerMotion::FSegmentJerk& Segment = Segments[Index];
                TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
                Row->SetObjectField(TEXT("start"),
                    MakeTickFrameJson(Segment.StartTick, TickResolution, DisplayRate));
                Row->SetObjectField(TEXT("end"),
                    MakeTickFrameJson(Segment.EndTick, TickResolution, DisplayRate));
                Row->SetNumberField(TEXT("durationSeconds"),
                    JsonBuilders::SanitizeFinite(Segment.DurationSeconds));
                Row->SetNumberField(TEXT("jerk"), JsonBuilders::SanitizeFinite(Segment.Magnitude));
                SegmentValues.Add(MakeShared<FJsonValueObject>(Row));
            }
            Check->SetArrayField(TEXT("segments"), SegmentValues);
        }
        CheckValues.Add(MakeShared<FJsonValueObject>(Check));
    }

    {
        PinWrightSequencerMotion::FCurvatureStats CurvatureStats;
        PinWrightSequencerMotion::MeasureCurvature(Samples, SpeedStats.Mean,
            SpeedStats.PathLength, CurvatureStats);

        const bool bMeasurable = bLocationMeasurable && CurvatureStats.bMeasurable;
        // A straight path has no turn to report and passes any minimum-radius gate correctly;
        // calling that unmeasured would fail the whole verdict on a perfectly good dolly.
        const bool bFailed = bMeasurable && bHasMinTurnRadius && !CurvatureStats.bStraight
            && CurvatureStats.MinRadius < MinTurnRadius;
        const TCHAR* Status = TEXT("unmeasured");
        if (bMeasurable)
        {
            Status = !bHasMinTurnRadius ? TEXT("reported") : (bFailed ? TEXT("fail") : TEXT("pass"));
        }
        TSharedPtr<FJsonObject> Check = MakeSequencerMotionCheck(TEXT("curvature"), Status);
        if (!bMeasurable)
        {
            Check->SetStringField(TEXT("reason"), !bUnweighted ? WeightedReason
                : TEXT("No grid sample cleared the speed floor, so every radius would be |v|^3 "
                       "float noise rather than a corner."));
            ++UnmeasuredChecks;
        }
        else
        {
            if (bFailed)
            {
                ++FailedChecks;
            }
            Check->SetBoolField(TEXT("straight"), CurvatureStats.bStraight);
            if (CurvatureStats.bStraight)
            {
                Check->SetStringField(TEXT("note"),
                    TEXT("The tightest radius found exceeds a thousand times the path's own arc "
                         "length, which bends the path by under a milliradian over its whole "
                         "extent. There is no corner here."));
            }
            else
            {
                Check->SetNumberField(TEXT("minTurnRadius"),
                    JsonBuilders::SanitizeFinite(CurvatureStats.MinRadius));
                Check->SetObjectField(TEXT("minTurnRadiusAt"),
                    MakeTickFrameJson(CurvatureStats.MinRadiusTick, TickResolution, DisplayRate));
                Check->SetNumberField(TEXT("speedAtMinRadius"),
                    JsonBuilders::SanitizeFinite(CurvatureStats.SpeedAtMinRadius));
                Check->SetNumberField(TEXT("tangentialAcceleration"),
                    JsonBuilders::SanitizeFinite(CurvatureStats.TangentialAccelerationAtMinRadius));
                Check->SetNumberField(TEXT("normalAcceleration"),
                    JsonBuilders::SanitizeFinite(CurvatureStats.NormalAccelerationAtMinRadius));
                Check->SetStringField(TEXT("note"),
                    TEXT("Compare minTurnRadius against the distance to the subject: a radius far "
                         "below it is a hairpin. Shrinking a key's tangent to author a deceleration "
                         "is the obvious move, but doing it at a key where the path ALSO changes "
                         "direction concentrates the whole turn into that key, and the authored "
                         "numbers all still read as intended."));
            }
            if (bHasMinTurnRadius)
            {
                Check->SetNumberField(TEXT("threshold"), MinTurnRadius);
            }
        }
        CheckValues.Add(MakeShared<FJsonValueObject>(Check));
    }

    {
        PinWrightSequencerMotion::FAngularStats AngularStats;
        PinWrightSequencerMotion::MeasureAngular(Rotation, GridTicks, TicksPerSecond, AngularStats);

        // "No rotation channel carries two keys" is a MEASUREMENT - this binding does not turn -
        // not a failure to look, so it stays `reported` with animated:false rather than dragging
        // the whole verdict down the way a genuine blind spot (a weighted tangent) must.
        TSharedPtr<FJsonObject> Check =
            MakeSequencerMotionCheck(TEXT("angular"), bUnweighted ? TEXT("reported") : TEXT("unmeasured"));
        Check->SetBoolField(TEXT("animated"), AngularStats.bMeasurable);
        if (!bUnweighted)
        {
            Check->SetStringField(TEXT("reason"), WeightedReason);
            ++UnmeasuredChecks;
        }
        else if (!AngularStats.bMeasurable)
        {
            Check->SetStringField(TEXT("note"),
                TEXT("No rotation channel carries two or more keys, so this binding does not turn "
                     "and there is no angular rate to report. A camera that translates without "
                     "reorienting is a legitimate move; this is a measurement, not a gap."));
        }
        else
        {
            static const TCHAR* const AxisNames[3] = { TEXT("roll"), TEXT("pitch"), TEXT("yaw") };
            TArray<TSharedPtr<FJsonValue>> AxisValues;
            for (int32 AxisIndex = 0; AxisIndex < 3; ++AxisIndex)
            {
                const PinWrightSequencerMotion::FAngularAxisStats& Axis = AngularStats.Axis[AxisIndex];
                TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
                Row->SetStringField(TEXT("axis"), AxisNames[AxisIndex]);
                Row->SetNumberField(TEXT("maxRate"), JsonBuilders::SanitizeFinite(Axis.MaxRate));
                Row->SetObjectField(TEXT("maxRateAt"),
                    MakeTickFrameJson(Axis.MaxRateTick, TickResolution, DisplayRate));
                Row->SetNumberField(TEXT("maxAcceleration"),
                    JsonBuilders::SanitizeFinite(Axis.MaxAcceleration));
                Row->SetObjectField(TEXT("maxAccelerationAt"),
                    MakeTickFrameJson(Axis.MaxAccelerationTick, TickResolution, DisplayRate));
                AxisValues.Add(MakeShared<FJsonValueObject>(Row));
            }
            Check->SetArrayField(TEXT("axes"), AxisValues);
            Check->SetStringField(TEXT("note"),
                TEXT("Reported without a verdict. Angular jerk feels worse than positional jerk and "
                     "is routinely never looked at: a yaw acceleration spike can sit at a key whose "
                     "positional numbers read entirely ordinary."));
        }
        CheckValues.Add(MakeShared<FJsonValueObject>(Check));
    }

    {
        PinWrightSequencerMotion::FSeamStats SeamStats;
        PinWrightSequencerMotion::MeasureLoopSeam(Location, Rotation, StartTick, EndTick,
            TicksPerSecond, SpeedStats.Mean, SeamStats);

        const bool bMeasurable = bUnweighted && SeamStats.bMeasurable;
        const bool bFailed = bMeasurable && bLooping
            && (SeamStats.bStopsDead || SeamStats.RelativeMismatch > MaxSeamVelocityMismatch);
        const TCHAR* Status = TEXT("unmeasured");
        if (bMeasurable)
        {
            Status = !bLooping ? TEXT("reported") : (bFailed ? TEXT("fail") : TEXT("pass"));
        }
        TSharedPtr<FJsonObject> Check = MakeSequencerMotionCheck(TEXT("loopSeam"), Status);
        if (!bMeasurable)
        {
            Check->SetStringField(TEXT("reason"), !bUnweighted ? WeightedReason
                : TEXT("Fewer than two keyed ticks on the location channels, so there is no "
                       "one-sided velocity to compare across the wrap."));
            ++UnmeasuredChecks;
        }
        else
        {
            if (bFailed)
            {
                ++FailedChecks;
            }
            Check->SetObjectField(TEXT("arriveVelocity"),
                JsonBuilders::BuildVectorJson(SeamStats.ArriveVelocity));
            Check->SetObjectField(TEXT("leaveVelocity"),
                JsonBuilders::BuildVectorJson(SeamStats.LeaveVelocity));
            Check->SetNumberField(TEXT("arriveSpeed"), JsonBuilders::SanitizeFinite(SeamStats.ArriveSpeed));
            Check->SetNumberField(TEXT("leaveSpeed"), JsonBuilders::SanitizeFinite(SeamStats.LeaveSpeed));
            Check->SetNumberField(TEXT("mismatch"), JsonBuilders::SanitizeFinite(SeamStats.Mismatch));
            Check->SetNumberField(TEXT("relativeMismatch"),
                JsonBuilders::SanitizeFinite(SeamStats.RelativeMismatch));
            Check->SetNumberField(TEXT("angularMismatch"),
                JsonBuilders::SanitizeFinite(SeamStats.AngularMismatch));
            Check->SetNumberField(TEXT("positionGap"), JsonBuilders::SanitizeFinite(SeamStats.PositionGap));
            Check->SetBoolField(TEXT("stopsDead"), SeamStats.bStopsDead);
            Check->SetNumberField(TEXT("maxSeamVelocityMismatch"), MaxSeamVelocityMismatch);
            if (!bLooping)
            {
                Check->SetStringField(TEXT("note"),
                    TEXT("Reported without a verdict because looping was false: a one-shot move is "
                         "expected to end somewhere other than where it started."));
            }
            else if (SeamStats.bStopsDead)
            {
                Check->SetStringField(TEXT("note"),
                    TEXT("The move HALTS on both sides of the wrap, so mismatch is zero and proves "
                         "nothing - this is a full stop at the one frame a loop shows most often, "
                         "not a clean C1 join. Auto tangents force the first key's leave tangent "
                         "and the last key's arrive tangent flat; author both explicitly with "
                         "tangentMode user and matching arriveTangent/leaveTangent (value per TICK)."));
            }
            else
            {
                Check->SetStringField(TEXT("note"),
                    TEXT("One-sided velocities are exact rather than finite-differenced, so there is "
                         "no sampling step and no convergence ratio to interpret. positionGap is "
                         "reported, never gated: a move that deliberately ends elsewhere is not a "
                         "defect, but a caller calling it a loop needs to see it."));
            }
        }
        CheckValues.Add(MakeShared<FJsonValueObject>(Check));
    }

    // ---- Response ----

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("measurement"), TEXT("sequencerMotion"));
    Result->SetStringField(TEXT("path"), AssetPath);
    Result->SetStringField(TEXT("bindingId"), BindingGuid.ToString());
    Result->SetStringField(TEXT("trackName"), Track->GetName());
    Result->SetNumberField(TEXT("sectionIndex"), SectionIndex);
    Result->SetNumberField(TEXT("sectionCount"), Sections.Num());
    Result->SetStringField(TEXT("source"), TEXT("transformSectionChannels"));
    Result->SetObjectField(TEXT("tickResolution"), MovieSceneJsonUtils::MakeFrameRateObject(TickResolution));
    Result->SetObjectField(TEXT("displayRate"), MovieSceneJsonUtils::MakeFrameRateObject(DisplayRate));
    Result->SetNumberField(TEXT("ticksPerSecond"), JsonBuilders::SanitizeFinite(TicksPerSecond));

    TSharedPtr<FJsonObject> RangeObj = MakeShared<FJsonObject>();
    RangeObj->SetObjectField(TEXT("start"), MakeTickFrameJson(StartTick, TickResolution, DisplayRate));
    RangeObj->SetObjectField(TEXT("end"), MakeTickFrameJson(EndTick, TickResolution, DisplayRate));
    RangeObj->SetNumberField(TEXT("durationSeconds"),
        JsonBuilders::SanitizeFinite(SpanTicks / TicksPerSecond));
    Result->SetObjectField(TEXT("range"), RangeObj);

    Result->SetNumberField(TEXT("locationKeyCount"), LocationKeyTicks.Num());
    Result->SetNumberField(TEXT("rotationKeyCount"), RotationKeyTicks.Num());
    Result->SetNumberField(TEXT("sampleCount"), GridTicks.Num());
    Result->SetNumberField(TEXT("sampleRate"), JsonBuilders::SanitizeFinite(EffectiveSampleRate));
    Result->SetNumberField(TEXT("sampleStepTicks"), StepTicks);
    Result->SetBoolField(TEXT("looping"), bLooping);
    Result->SetBoolField(TEXT("linearActsAsCubic"), bLinearActsAsCubic);

    // The unit block is part of the contract, not decoration: the stored tangents are per TICK and
    // everything below is per SECOND, which is the one place this verb could be silently wrong by
    // a factor of the tick resolution.
    TSharedPtr<FJsonObject> UnitsObj = MakeShared<FJsonObject>();
    UnitsObj->SetStringField(TEXT("speed"), TEXT("uu/s"));
    UnitsObj->SetStringField(TEXT("acceleration"), TEXT("uu/s^2"));
    UnitsObj->SetStringField(TEXT("jerk"), TEXT("uu/s^3"));
    UnitsObj->SetStringField(TEXT("angularRate"), TEXT("deg/s"));
    UnitsObj->SetStringField(TEXT("angularAcceleration"), TEXT("deg/s^2"));
    UnitsObj->SetStringField(TEXT("turnRadius"), TEXT("uu"));
    UnitsObj->SetStringField(TEXT("authoredTangent"),
        TEXT("curve value per TICK - divide a per-second slope by ticksPerSecond before writing it"));
    Result->SetObjectField(TEXT("units"), UnitsObj);

    Result->SetArrayField(TEXT("checks"), CheckValues);
    Result->SetNumberField(TEXT("failedChecks"), FailedChecks);
    Result->SetNumberField(TEXT("unmeasuredChecks"), UnmeasuredChecks);

    // An unmeasured check counts against the verdict exactly as a failed one does. "I could not
    // look" and "I looked and it was fine" must never produce the same pass:true.
    Result->SetBoolField(TEXT("pass"), FailedChecks == 0 && UnmeasuredChecks == 0);

    if (Sections.Num() > 1)
    {
        Warnings.Add(FString::Printf(
            TEXT("The transform track has %d sections and only section %d was measured. Overlapping "
                 "sections blend at evaluation time, which no analytic basis can differentiate; "
                 "measure each one separately with sectionIndex."),
            Sections.Num(), SectionIndex));
    }
    if (!bUnweighted)
    {
        Warnings.Add(FString(WeightedReason));
    }
    if (SpeedStats.bMeasurable && !(SpeedStats.Max > 0.0))
    {
        // Otherwise this reads as a clean bill of health: a point that never moves has a
        // trivially continuous seam, zero jerk and no corner, and every check passes on it.
        Warnings.Add(FString(
            TEXT("The location channels never move: the maximum speed over the whole range is zero. "
                 "Every positional check below is therefore trivially satisfied and none of them is "
                 "evidence of anything. The loopSeam verdict in particular is about a stationary "
                 "point, not about a camera move.")));
    }
    if (KeyMotion.Num() > MaxReported)
    {
        Warnings.Add(FString::Printf(
            TEXT("%d of %d per-key rows were emitted (maxReported). The summary statistics are over "
                 "ALL keys; only the tables are truncated."), MaxReported, KeyMotion.Num()));
    }
    if (UnmeasuredChecks > 0)
    {
        Warnings.Add(FString::Printf(
            TEXT("%d of the checks could not be measured, so this result is not evidence of sound "
                 "motion. Read each check's status and reason."), UnmeasuredChecks));
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
