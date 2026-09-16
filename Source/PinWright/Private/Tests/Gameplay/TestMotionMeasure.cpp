// Copyright (c) 2026 Alexander Penkin. MIT License.

// Tests for animation.measure_motion.
//
// Built as MATCHED PAIRS. Every metric here is a ratio whose denominator is the animation's own
// frame step, so a measurement that silently read zeros everywhere would produce 0/0 and could
// be mistaken for a clean result. Each defect fixture therefore has a clean twin that differs
// in exactly one authored value, and the clean twin asserts that every check reached status
// "pass" or "reported" - never "unmeasured".
//
// The walk fixture below is arithmetic, not art: the planted foot travels exactly 100 units
// over exactly half a second, so the animation implies exactly 200 units per second. That
// number is asserted directly. A stride measurement that is merely plausible is worthless; the
// point of deriving it numerically is that it is exact.
//
// Every test drives the PRODUCTION handler through InvokeHandlerWithCapture.

#include "Misc/AutomationTest.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/HandlerRegistration.h"
#include "Tests/Gameplay/TestAnimationFixtures.h"
#include "Tests/TestUtils.h"
#include "Tests/TestSkipReporting.h"

#include "Animation/AnimSequence.h"
#include "Animation/Skeleton.h"
#include "Animation/AnimData/IAnimationDataController.h"
#include "Animation/AnimData/IAnimationDataModel.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/EngineVersionComparison.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"
#include "ReferenceSkeleton.h"
#include "UObject/Package.h"

namespace
{
    // The cycle: 31 keys at 30 fps, i.e. one second with an explicit duplicate wrap key.
    constexpr int32 MotionFixtureKeyCount = 31;
    constexpr int32 MotionFixtureStanceEnd = 15;   // last planted key
    constexpr double MotionFixtureStanceHalfSpan = 50.0;  // foot travels +50 -> -50 while planted
    constexpr double MotionFixtureLiftHeight = 20.0;

    // Stance runs keys 0..15, i.e. 15 intervals = 0.5 s, over 100 units. 100 / 0.5 = 200 uu/s.
    constexpr double MotionFixtureExpectedGroundSpeed = 200.0;

    // One foot's component-space position at a key of the fixture cycle. Stance is a straight
    // backward slide at constant speed with the foot flat on the ground; swing is a cosine
    // ease forward with a sine arc for the lift, so the curve has no per-frame reversal and the
    // jitter classifier stays well under its threshold on correct input.
    FVector MotionFixtureFootLocation(int32 Key)
    {
        if (Key <= MotionFixtureStanceEnd)
        {
            const double Alpha = static_cast<double>(Key) / static_cast<double>(MotionFixtureStanceEnd);
            return FVector(MotionFixtureStanceHalfSpan - 2.0 * MotionFixtureStanceHalfSpan * Alpha, 0.0, 0.0);
        }
        const double U = static_cast<double>(Key - MotionFixtureStanceEnd)
            / static_cast<double>(MotionFixtureKeyCount - 1 - MotionFixtureStanceEnd);
        const double Ease = 0.5 - 0.5 * FMath::Cos(UE_DOUBLE_PI * U);
        return FVector(
            -MotionFixtureStanceHalfSpan + 2.0 * MotionFixtureStanceHalfSpan * Ease,
            0.0,
            MotionFixtureLiftHeight * FMath::Sin(UE_DOUBLE_PI * U));
    }

    TArray<FVector> MakeMotionWalkCycleLocations()
    {
        TArray<FVector> Locations;
        Locations.Reserve(MotionFixtureKeyCount);
        for (int32 Key = 0; Key < MotionFixtureKeyCount; ++Key)
        {
            Locations.Add(MotionFixtureFootLocation(Key));
        }
        return Locations;
    }

    TArray<FVector> MakeMotionJitterLocations()
    {
        TArray<FVector> Locations;
        Locations.Reserve(MotionFixtureKeyCount);
        for (int32 Key = 0; Key < MotionFixtureKeyCount; ++Key)
        {
            FVector Location(10.0 * static_cast<double>(Key), 0.0, 0.0);
            if (Key == MotionFixtureKeyCount / 2)
            {
                Location.X += 1000.0;
            }
            Locations.Add(Location);
        }
        return Locations;
    }

    TSharedPtr<FJsonObject> MakeMotionPayload(const FString& AssetPath)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("assetPath"), AssetPath);
        return Payload;
    }

    void AddMotionFootBones(const TSharedPtr<FJsonObject>& Payload)
    {
        TArray<TSharedPtr<FJsonValue>> Feet;
        Feet.Add(MakeShared<FJsonValueString>(TEXT("foot_l")));
        Payload->SetArrayField(TEXT("footBones"), Feet);
    }

    const TSharedPtr<FJsonObject>* FindMotionCheck(const TSharedPtr<FJsonObject>& Result, const TCHAR* Name)
    {
        const TArray<TSharedPtr<FJsonValue>>* Checks = nullptr;
        if (!Result.IsValid() || !Result->TryGetArrayField(TEXT("checks"), Checks) || !Checks)
        {
            return nullptr;
        }
        for (const TSharedPtr<FJsonValue>& Value : *Checks)
        {
            const TSharedPtr<FJsonObject>* Object = nullptr;
            FString CheckName;
            if (Value.IsValid() && Value->TryGetObject(Object) && Object && (*Object).IsValid()
                && (*Object)->TryGetStringField(TEXT("name"), CheckName) && CheckName == Name)
            {
                return Object;
            }
        }
        return nullptr;
    }

    FString MotionCheckStatus(const TSharedPtr<FJsonObject>& Result, const TCHAR* Name)
    {
        const TSharedPtr<FJsonObject>* Check = FindMotionCheck(Result, Name);
        FString Status;
        if (Check && (*Check).IsValid())
        {
            (*Check)->TryGetStringField(TEXT("status"), Status);
        }
        return Status;
    }

    // The single foot row out of the locomotion check.
    TSharedPtr<FJsonObject> FirstMotionFootRow(const TSharedPtr<FJsonObject>& Result)
    {
        const TSharedPtr<FJsonObject>* Check = FindMotionCheck(Result, TEXT("locomotion"));
        const TArray<TSharedPtr<FJsonValue>>* Feet = nullptr;
        if (!Check || !(*Check).IsValid() || !(*Check)->TryGetArrayField(TEXT("feet"), Feet)
            || !Feet || Feet->Num() == 0)
        {
            return nullptr;
        }
        return (*Feet)[0]->AsObject();
    }

    bool RunMeasureMotion(FAutomationTestBase& Test, const TSharedPtr<FJsonObject>& Payload,
        FTestResponseCapture& Capture)
    {
        const bool bFound = InvokeHandlerWithCapture(TEXT("animation.measure_motion"), Payload, Capture);
        Test.TestTrue(TEXT("animation.measure_motion is registered"), bFound);
        Test.TestTrue(TEXT("handler responded"), Capture.bWasCalled);
        return bFound && Capture.bWasCalled;
    }
}

// UE 5.3/5.4 skip, mirroring TestAnimSequenceDumpBuilder's BoneTracksReadback: closing an
// IAnimationDataController bracket on a hand-authored transient sequence triggers the engine's
// synchronous anim-compression DDC build, which hard-asserts on a minimal transient skeleton on
// those versions. The crash is entirely engine-side; the production code under test reads only
// the raw data model and is version-agnostic. Skipping keeps the limitation visible instead of
// silently passing.
#define PINWRIGHT_MOTION_FIXTURE_UNSUPPORTED UE_VERSION_OLDER_THAN(5, 5, 0)

// ---------------------------------------------------------------------------------------
// The clean twin plus the exact stride assertion. If the stance window or the wrap-key
// handling is wrong, impliedGroundSpeed misses 200 and this fails - which is the only reason
// deriving the number is worth anything.
// ---------------------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAnimationMeasureMotionWalkCycleTest,
    "PinWright.animation.measure_motion.WalkCycleDerivesExactGroundSpeed",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FAnimationMeasureMotionWalkCycleTest::RunTest(const FString& Parameters)
{
#if PINWRIGHT_MOTION_FIXTURE_UNSUPPORTED
    PinWrightTestSkip::SkipAssertions(*this, TEXT("engine-version-unsupported"),
        TEXT("Skipping measure_motion fixture tests on UE 5.3/5.4: engine anim compression asserts "
            "when re-compressing a hand-authored transient AnimSequence on a minimal skeleton (works on 5.5+)."));
    return true;
#else
    const TArray<FVector> Locations = MakeMotionWalkCycleLocations();
    PinWrightAnimationTestFixtures::FRegisteredAnimationFixture Fixture;
    TestTrue(TEXT("registered motion fixture created"),
        Fixture.Create(TEXT("Motion_Walk"), true, FFrameRate(30, 1), &Locations));
    UAnimSequence* Sequence = Fixture.Sequence;
    TestNotNull(TEXT("fixture sequence created"), Sequence);
    if (!Sequence)
    {
        return false;
    }

    TSharedPtr<FJsonObject> Payload = MakeMotionPayload(Sequence->GetPathName());
    AddMotionFootBones(Payload);

    FTestResponseCapture Capture;
    if (!RunMeasureMotion(*this, Payload, Capture))
    {
        return false;
    }
    TestTrue(FString::Printf(TEXT("measure_motion succeeded (errorCode='%s')"), *Capture.ErrorCode),
        Capture.bSuccess);
    if (!Capture.bSuccess || !Capture.Result.IsValid())
    {
        return false;
    }

    bool bPass = false;
    Capture.Result->TryGetBoolField(TEXT("pass"), bPass);
    TestTrue(TEXT("a sound cycle passes"), bPass);
    TestEqual(TEXT("nothing was left unmeasured"),
        static_cast<int32>(Capture.Result->GetNumberField(TEXT("unmeasuredChecks"))), 0);
    TestTrue(TEXT("every key was sampled"), Capture.Result->GetBoolField(TEXT("exhaustive")));

    TestEqual(TEXT("motion detected"), MotionCheckStatus(Capture.Result, TEXT("moving")), FString(TEXT("pass")));
    TestEqual(TEXT("loop seam checked and sound"),
        MotionCheckStatus(Capture.Result, TEXT("loopSeam")), FString(TEXT("pass")));
    TestEqual(TEXT("jitter measured and reported without a verdict"),
        MotionCheckStatus(Capture.Result, TEXT("jitter")), FString(TEXT("reported")));
    TestEqual(TEXT("locomotion measured and reported without a verdict"),
        MotionCheckStatus(Capture.Result, TEXT("locomotion")), FString(TEXT("reported")));

    const TSharedPtr<FJsonObject>* JitterCheck = FindMotionCheck(Capture.Result, TEXT("jitter"));
    if (!JitterCheck || !(*JitterCheck).IsValid())
    {
        AddError(TEXT("jitter check was not returned"));
        return false;
    }
    TestTrue(TEXT("jitter exposes the measured frameFraction"), (*JitterCheck)->HasField(TEXT("frameFraction")));
    TestTrue(TEXT("jitter exposes the measured maxRatio"), (*JitterCheck)->HasField(TEXT("maxRatio")));
    TestFalse(TEXT("the omitted jitter threshold is not echoed"),
        (*JitterCheck)->HasField(TEXT("maxJitterFraction")));

    const TSharedPtr<FJsonObject>* LocomotionCheck = FindMotionCheck(Capture.Result, TEXT("locomotion"));
    if (!LocomotionCheck || !(*LocomotionCheck).IsValid())
    {
        AddError(TEXT("locomotion check was not returned"));
        return false;
    }
    TestFalse(TEXT("expected speed is absent when no locomotion gate was supplied"),
        (*LocomotionCheck)->HasField(TEXT("expectedGroundSpeed")));
    TestFalse(TEXT("ground speed tolerance is absent when no locomotion gate was supplied"),
        (*LocomotionCheck)->HasField(TEXT("groundSpeedTolerance")));

    const TSharedPtr<FJsonObject> Foot = FirstMotionFootRow(Capture.Result);
    if (!Foot.IsValid())
    {
        AddError(TEXT("locomotion produced no foot row"));
        return false;
    }
    TestTrue(TEXT("the foot was measured"), Foot->GetBoolField(TEXT("measured")));
    TestTrue(TEXT("the duplicate wrap key was recognised and discarded"),
        Foot->GetBoolField(TEXT("droppedWrapSample")));
    TestTrue(TEXT("stance spans exactly the authored half second"),
        FMath::IsNearlyEqual(Foot->GetNumberField(TEXT("stanceSeconds")), 0.5, 1.0e-6));
    TestTrue(TEXT("the planted foot travels exactly the authored 100 units"),
        FMath::IsNearlyEqual(Foot->GetNumberField(TEXT("stanceDisplacement")), 100.0, 1.0e-3));
    TestTrue(TEXT("the measured foot publishes impliedGroundSpeed"),
        Foot->HasField(TEXT("impliedGroundSpeed")));
    TestFalse(TEXT("slideFraction is absent without expected speed"),
        Foot->HasField(TEXT("slideFraction")));
    TestFalse(TEXT("the per-foot verdict is absent without expected speed"),
        Foot->HasField(TEXT("pass")));
    TestFalse(TEXT("suggestedPlayRate is absent without expected speed"),
        Foot->HasField(TEXT("suggestedPlayRate")));
    // The whole point of the verb.
    TestTrue(FString::Printf(TEXT("implied ground speed is the authored %.0f uu/s (got %.4f)"),
            MotionFixtureExpectedGroundSpeed, Foot->GetNumberField(TEXT("impliedGroundSpeed"))),
        FMath::IsNearlyEqual(Foot->GetNumberField(TEXT("impliedGroundSpeed")),
            MotionFixtureExpectedGroundSpeed, 1.0e-3));
    TestTrue(TEXT("one cycle is one second"),
        FMath::IsNearlyEqual(Foot->GetNumberField(TEXT("cycleSeconds")), 1.0, 1.0e-6));
    TestTrue(TEXT("stride per cycle follows from the speed and the cycle length"),
        FMath::IsNearlyEqual(Foot->GetNumberField(TEXT("stridePerCycle")),
            MotionFixtureExpectedGroundSpeed, 1.0e-3));

    return true;
#endif
}

// ---------------------------------------------------------------------------------------
// Jitter is a measurement until the caller supplies a threshold. This fixture has one large
// authored reversal so the same measured frameFraction can prove both the reported and gated
// response shapes.
// ---------------------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAnimationMeasureMotionJitterThresholdTest,
    "PinWright.animation.measure_motion.JitterThresholdControlsVerdict",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FAnimationMeasureMotionJitterThresholdTest::RunTest(const FString& Parameters)
{
#if PINWRIGHT_MOTION_FIXTURE_UNSUPPORTED
    PinWrightTestSkip::SkipAssertions(*this, TEXT("engine-version-unsupported"),
        TEXT("Skipping measure_motion fixture tests on UE 5.3/5.4 (engine anim compression assert)."));
    return true;
#else
    const TArray<FVector> Locations = MakeMotionJitterLocations();
    PinWrightAnimationTestFixtures::FRegisteredAnimationFixture Fixture;
    TestTrue(TEXT("registered motion fixture created"),
        Fixture.Create(TEXT("Motion_Jitter"), true, FFrameRate(30, 1), &Locations));
    UAnimSequence* Sequence = Fixture.Sequence;
    if (!Sequence)
    {
        return false;
    }

    TSharedPtr<FJsonObject> ReportPayload = MakeMotionPayload(Sequence->GetPathName());
    ReportPayload->SetBoolField(TEXT("looping"), false);
    ReportPayload->SetNumberField(TEXT("expectedGroundSpeed"), MotionFixtureExpectedGroundSpeed);
    FTestResponseCapture ReportCapture;
    if (!RunMeasureMotion(*this, ReportPayload, ReportCapture)
        || !ReportCapture.bSuccess || !ReportCapture.Result.IsValid())
    {
        return false;
    }

    const TSharedPtr<FJsonObject>* ReportJitter = FindMotionCheck(ReportCapture.Result, TEXT("jitter"));
    if (!ReportJitter || !(*ReportJitter).IsValid())
    {
        AddError(TEXT("jitter check was not returned for the unthresholded measurement"));
        return false;
    }
    TestEqual(TEXT("jitter is reported without maxJitterFraction"),
        (*ReportJitter)->GetStringField(TEXT("status")), FString(TEXT("reported")));
    TestTrue(TEXT("the unthresholded response contains frameFraction"),
        (*ReportJitter)->HasField(TEXT("frameFraction")));
    TestTrue(TEXT("the unthresholded response contains maxRatio"),
        (*ReportJitter)->HasField(TEXT("maxRatio")));
    TestTrue(TEXT("expected speed does not create locomotion without footBones"),
        FindMotionCheck(ReportCapture.Result, TEXT("locomotion")) == nullptr);
    const double FrameFraction = (*ReportJitter)->GetNumberField(TEXT("frameFraction"));
    TestTrue(TEXT("the jitter fixture produces a non-zero measured frameFraction"), FrameFraction > 0.0);
    TestTrue(TEXT("the jitter fixture produces a ratio above the classifier threshold"),
        (*ReportJitter)->GetNumberField(TEXT("maxRatio")) > 2.0);

    TSharedPtr<FJsonObject> GatePayload = MakeMotionPayload(Sequence->GetPathName());
    GatePayload->SetBoolField(TEXT("looping"), false);
    GatePayload->SetNumberField(TEXT("maxJitterFraction"), 0.0);
    FTestResponseCapture GateCapture;
    if (!RunMeasureMotion(*this, GatePayload, GateCapture)
        || !GateCapture.bSuccess || !GateCapture.Result.IsValid())
    {
        return false;
    }

    const TSharedPtr<FJsonObject>* GatedJitter = FindMotionCheck(GateCapture.Result, TEXT("jitter"));
    if (!GatedJitter || !(*GatedJitter).IsValid())
    {
        AddError(TEXT("jitter check was not returned for the thresholded measurement"));
        return false;
    }
    TestEqual(TEXT("zero jitter threshold fails the measured reversal"),
        (*GatedJitter)->GetStringField(TEXT("status")), FString(TEXT("fail")));
    TestTrue(TEXT("the threshold is echoed only when supplied"),
        (*GatedJitter)->HasField(TEXT("maxJitterFraction")));
    TestTrue(TEXT("the thresholded result does not pass"), !GateCapture.Result->GetBoolField(TEXT("pass")));
    return true;
#endif
}

// ---------------------------------------------------------------------------------------
// Defect twin 1: nothing moves. This is the case a capture set provably cannot detect - a mesh
// frozen in bind pose photographs identically at every frame and every angle - which is why it
// is the first check in the verb.
// ---------------------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAnimationMeasureMotionFrozenFailsTest,
    "PinWright.animation.measure_motion.FrozenSequenceFails",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FAnimationMeasureMotionFrozenFailsTest::RunTest(const FString& Parameters)
{
#if PINWRIGHT_MOTION_FIXTURE_UNSUPPORTED
    PinWrightTestSkip::SkipAssertions(*this, TEXT("engine-version-unsupported"),
        TEXT("Skipping measure_motion fixture tests on UE 5.3/5.4 (engine anim compression assert)."));
    return true;
#else
    TArray<FVector> Locations;
    Locations.Init(FVector(10.0, 0.0, 0.0), MotionFixtureKeyCount);
    PinWrightAnimationTestFixtures::FRegisteredAnimationFixture Fixture;
    TestTrue(TEXT("registered motion fixture created"),
        Fixture.Create(TEXT("Motion_Frozen"), true, FFrameRate(30, 1), &Locations));
    UAnimSequence* Sequence = Fixture.Sequence;
    if (!Sequence)
    {
        return false;
    }

    FTestResponseCapture Capture;
    if (!RunMeasureMotion(*this, MakeMotionPayload(Sequence->GetPathName()), Capture)
        || !Capture.bSuccess || !Capture.Result.IsValid())
    {
        return false;
    }

    bool bPass = true;
    Capture.Result->TryGetBoolField(TEXT("pass"), bPass);
    TestFalse(TEXT("a frozen sequence must not pass"), bPass);
    TestEqual(TEXT("the moving check failed"),
        MotionCheckStatus(Capture.Result, TEXT("moving")), FString(TEXT("fail")));

    const TSharedPtr<FJsonObject>* Check = FindMotionCheck(Capture.Result, TEXT("moving"));
    if (Check && (*Check).IsValid())
    {
        TestEqual(TEXT("no bone counted as moving"),
            static_cast<int32>((*Check)->GetNumberField(TEXT("movingBones"))), 0);
    }

    // The seam and jitter checks have no frame step to divide by on a frozen clip, so they must
    // say UNMEASURED rather than report a flawless zero-ratio loop. This is the assertion that
    // stops a frozen animation from scoring better than a real one.
    TestEqual(TEXT("loop seam reports unmeasured, not a perfect seam"),
        MotionCheckStatus(Capture.Result, TEXT("loopSeam")), FString(TEXT("unmeasured")));
    TestEqual(TEXT("jitter reports unmeasured, not zero jitter"),
        MotionCheckStatus(Capture.Result, TEXT("jitter")), FString(TEXT("unmeasured")));
    return true;
#endif
}

// ---------------------------------------------------------------------------------------
// Defect twin 2: the loop pops. Identical to the clean fixture except the last key, so the only
// thing that can move the verdict is the seam itself.
// ---------------------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAnimationMeasureMotionSeamPopFailsTest,
    "PinWright.animation.measure_motion.LoopSeamPopFails",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FAnimationMeasureMotionSeamPopFailsTest::RunTest(const FString& Parameters)
{
#if PINWRIGHT_MOTION_FIXTURE_UNSUPPORTED
    PinWrightTestSkip::SkipAssertions(*this, TEXT("engine-version-unsupported"),
        TEXT("Skipping measure_motion fixture tests on UE 5.3/5.4 (engine anim compression assert)."));
    return true;
#else
    TArray<FVector> Locations = MakeMotionWalkCycleLocations();
    // The wrap key lands 50 units from where the cycle started: eight normal frame steps of
    // travel arriving in one frame.
    Locations.Last() = FVector(0.0, 0.0, 0.0);

    PinWrightAnimationTestFixtures::FRegisteredAnimationFixture Fixture;
    TestTrue(TEXT("registered motion fixture created"),
        Fixture.Create(TEXT("Motion_Seam"), true, FFrameRate(30, 1), &Locations));
    UAnimSequence* Sequence = Fixture.Sequence;
    if (!Sequence)
    {
        return false;
    }

    FTestResponseCapture Capture;
    if (!RunMeasureMotion(*this, MakeMotionPayload(Sequence->GetPathName()), Capture)
        || !Capture.bSuccess || !Capture.Result.IsValid())
    {
        return false;
    }

    bool bPass = true;
    Capture.Result->TryGetBoolField(TEXT("pass"), bPass);
    TestFalse(TEXT("a popping loop must not pass"), bPass);
    TestEqual(TEXT("the loop seam check failed"),
        MotionCheckStatus(Capture.Result, TEXT("loopSeam")), FString(TEXT("fail")));
    // The motion itself is unchanged, so the check that would fire on any broken fixture must
    // still pass - otherwise this test would prove nothing about the seam specifically.
    TestEqual(TEXT("motion is still detected"),
        MotionCheckStatus(Capture.Result, TEXT("moving")), FString(TEXT("pass")));

    const TSharedPtr<FJsonObject>* Check = FindMotionCheck(Capture.Result, TEXT("loopSeam"));
    if (Check && (*Check).IsValid())
    {
        TestTrue(TEXT("the seam gap is reported in frame steps, not world units alone"),
            (*Check)->GetNumberField(TEXT("locationRatio")) > 3.0);
        TestEqual(TEXT("the popping bone is named"),
            (*Check)->GetStringField(TEXT("worstBone")), FString(TEXT("foot_l")));
    }

    // Declaring the clip non-looping must turn the same measurement into a report with no
    // verdict: a one-shot is supposed to end somewhere else.
    {
        TSharedPtr<FJsonObject> Payload = MakeMotionPayload(Sequence->GetPathName());
        Payload->SetBoolField(TEXT("looping"), false);
        FTestResponseCapture NonLooping;
        if (RunMeasureMotion(*this, Payload, NonLooping) && NonLooping.bSuccess
            && NonLooping.Result.IsValid())
        {
            TestEqual(TEXT("a non-looping clip's seam is reported, not judged"),
                MotionCheckStatus(NonLooping.Result, TEXT("loopSeam")), FString(TEXT("reported")));
        }
    }
    return true;
#endif
}

// ---------------------------------------------------------------------------------------
// The gate the real foot-slide incident needed: the animation implies 200 uu/s, the caller says
// the body travels at 400, and the verb both fails and hands back the play rate that
// reconciles them.
// The matched half is the same call with the correct speed, which must pass.
// ---------------------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAnimationMeasureMotionFootSlideGateTest,
    "PinWright.animation.measure_motion.FootSlideGateAndPlayRateSuggestion",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FAnimationMeasureMotionFootSlideGateTest::RunTest(const FString& Parameters)
{
#if PINWRIGHT_MOTION_FIXTURE_UNSUPPORTED
    PinWrightTestSkip::SkipAssertions(*this, TEXT("engine-version-unsupported"),
        TEXT("Skipping measure_motion fixture tests on UE 5.3/5.4 (engine anim compression assert)."));
    return true;
#else
    const TArray<FVector> Locations = MakeMotionWalkCycleLocations();
    PinWrightAnimationTestFixtures::FRegisteredAnimationFixture Fixture;
    TestTrue(TEXT("registered motion fixture created"),
        Fixture.Create(TEXT("Motion_Slide"), true, FFrameRate(30, 1), &Locations));
    UAnimSequence* Sequence = Fixture.Sequence;
    if (!Sequence)
    {
        return false;
    }

    // Matching speed: passes.
    {
        TSharedPtr<FJsonObject> Payload = MakeMotionPayload(Sequence->GetPathName());
        AddMotionFootBones(Payload);
        Payload->SetNumberField(TEXT("expectedGroundSpeed"), MotionFixtureExpectedGroundSpeed);
        FTestResponseCapture Capture;
        if (!RunMeasureMotion(*this, Payload, Capture) || !Capture.bSuccess || !Capture.Result.IsValid())
        {
            return false;
        }
        TestEqual(TEXT("a matched body speed passes the locomotion gate"),
            MotionCheckStatus(Capture.Result, TEXT("locomotion")), FString(TEXT("pass")));
        bool bPass = false;
        Capture.Result->TryGetBoolField(TEXT("pass"), bPass);
        TestTrue(TEXT("and the whole measurement passes"), bPass);
    }

    // Twice the speed the animation implies: fails, and the suggested play rate is exactly 2.
    {
        TSharedPtr<FJsonObject> Payload = MakeMotionPayload(Sequence->GetPathName());
        AddMotionFootBones(Payload);
        Payload->SetNumberField(TEXT("expectedGroundSpeed"), MotionFixtureExpectedGroundSpeed * 2.0);
        FTestResponseCapture Capture;
        if (!RunMeasureMotion(*this, Payload, Capture) || !Capture.bSuccess || !Capture.Result.IsValid())
        {
            return false;
        }
        TestEqual(TEXT("a body moving twice as fast as the feet fails the locomotion gate"),
            MotionCheckStatus(Capture.Result, TEXT("locomotion")), FString(TEXT("fail")));

        const TSharedPtr<FJsonObject> Foot = FirstMotionFootRow(Capture.Result);
        if (!Foot.IsValid())
        {
            AddError(TEXT("locomotion produced no foot row"));
            return false;
        }
        TestTrue(TEXT("expected speed adds slideFraction"), Foot->HasField(TEXT("slideFraction")));
        TestTrue(TEXT("expected speed adds a per-foot verdict"), Foot->HasField(TEXT("pass")));
        TestTrue(TEXT("a failed foot gets suggestedPlayRate"), Foot->HasField(TEXT("suggestedPlayRate")));
        TestTrue(TEXT("the slide is reported as a full 100% of the expected speed"),
            FMath::IsNearlyEqual(Foot->GetNumberField(TEXT("slideFraction")), 0.5, 1.0e-3));
        TestTrue(TEXT("and the play rate that would fix it is handed back"),
            FMath::IsNearlyEqual(Foot->GetNumberField(TEXT("suggestedPlayRate")), 2.0, 1.0e-3));
    }

    // Applying that play rate must move the implied speed to match, which is what makes the
    // suggestion actionable rather than decorative.
    {
        TSharedPtr<FJsonObject> Payload = MakeMotionPayload(Sequence->GetPathName());
        AddMotionFootBones(Payload);
        Payload->SetNumberField(TEXT("playRate"), 2.0);
        Payload->SetNumberField(TEXT("expectedGroundSpeed"), MotionFixtureExpectedGroundSpeed * 2.0);
        FTestResponseCapture Capture;
        if (!RunMeasureMotion(*this, Payload, Capture) || !Capture.bSuccess || !Capture.Result.IsValid())
        {
            return false;
        }
        TestEqual(TEXT("the suggested play rate makes the same clip pass"),
            MotionCheckStatus(Capture.Result, TEXT("locomotion")), FString(TEXT("pass")));

        const TSharedPtr<FJsonObject> Foot = FirstMotionFootRow(Capture.Result);
        if (Foot.IsValid())
        {
            TestTrue(TEXT("implied ground speed scales exactly with play rate"),
                FMath::IsNearlyEqual(Foot->GetNumberField(TEXT("impliedGroundSpeed")),
                    MotionFixtureExpectedGroundSpeed * 2.0, 1.0e-3));
        }
    }
    return true;
#endif
}

// ---------------------------------------------------------------------------------------
// A foot that never leaves the ground has no stance to separate from a swing. It must report
// unmeasured, not a stride of zero - a zero stride would read as "this animation implies the
// character stands still", which is a claim, and the verb has no basis for it.
// ---------------------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAnimationMeasureMotionNoStanceIsUnmeasuredTest,
    "PinWright.animation.measure_motion.FootWithNoLiftIsUnmeasured",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FAnimationMeasureMotionNoStanceIsUnmeasuredTest::RunTest(const FString& Parameters)
{
#if PINWRIGHT_MOTION_FIXTURE_UNSUPPORTED
    PinWrightTestSkip::SkipAssertions(*this, TEXT("engine-version-unsupported"),
        TEXT("Skipping measure_motion fixture tests on UE 5.3/5.4 (engine anim compression assert)."));
    return true;
#else
    // Slides horizontally the whole time and never rises: motion, but not locomotion.
    TArray<FVector> Locations;
    for (int32 Key = 0; Key < MotionFixtureKeyCount; ++Key)
    {
        const double Alpha = static_cast<double>(Key) / static_cast<double>(MotionFixtureKeyCount - 1);
        Locations.Add(FVector(100.0 * FMath::Sin(2.0 * UE_DOUBLE_PI * Alpha), 0.0, 0.0));
    }
    PinWrightAnimationTestFixtures::FRegisteredAnimationFixture Fixture;
    TestTrue(TEXT("registered motion fixture created"),
        Fixture.Create(TEXT("Motion_NoLift"), true, FFrameRate(30, 1), &Locations));
    UAnimSequence* Sequence = Fixture.Sequence;
    if (!Sequence)
    {
        return false;
    }

    TSharedPtr<FJsonObject> Payload = MakeMotionPayload(Sequence->GetPathName());
    AddMotionFootBones(Payload);
    Payload->SetNumberField(TEXT("expectedGroundSpeed"), MotionFixtureExpectedGroundSpeed);

    FTestResponseCapture Capture;
    if (!RunMeasureMotion(*this, Payload, Capture) || !Capture.bSuccess || !Capture.Result.IsValid())
    {
        return false;
    }

    TestEqual(TEXT("motion is detected"),
        MotionCheckStatus(Capture.Result, TEXT("moving")), FString(TEXT("pass")));
    TestEqual(TEXT("locomotion reports unmeasured rather than a fabricated stride"),
        MotionCheckStatus(Capture.Result, TEXT("locomotion")), FString(TEXT("unmeasured")));

    const TSharedPtr<FJsonObject>* LocomotionCheck = FindMotionCheck(Capture.Result, TEXT("locomotion"));
    if (!LocomotionCheck || !(*LocomotionCheck).IsValid())
    {
        AddError(TEXT("locomotion check was not returned for the unmeasured stance"));
        return false;
    }
    TestEqual(TEXT("no foot is counted as measured when no stance is usable"),
        static_cast<int32>((*LocomotionCheck)->GetNumberField(TEXT("measuredFeet"))), 0);
    FString LocomotionReason;
    TestTrue(TEXT("the unmeasured locomotion check explains the missing stance"),
        (*LocomotionCheck)->TryGetStringField(TEXT("reason"), LocomotionReason)
        && !LocomotionReason.IsEmpty());
    TestTrue(TEXT("expected speed is echoed on the unmeasured locomotion check"),
        FMath::IsNearlyEqual((*LocomotionCheck)->GetNumberField(TEXT("expectedGroundSpeed")),
            MotionFixtureExpectedGroundSpeed, 1.0e-3));
    TestTrue(TEXT("ground speed tolerance is echoed on the unmeasured locomotion check"),
        FMath::IsNearlyEqual((*LocomotionCheck)->GetNumberField(TEXT("groundSpeedTolerance")),
            0.1, 1.0e-6));

    bool bPass = true;
    Capture.Result->TryGetBoolField(TEXT("pass"), bPass);
    TestFalse(TEXT("an unmeasured check drags the verdict down exactly as a failure does"), bPass);

    const TSharedPtr<FJsonObject> Foot = FirstMotionFootRow(Capture.Result);
    if (!Foot.IsValid())
    {
        AddError(TEXT("the unmeasured locomotion check did not return its foot row"));
        return false;
    }
    else
    {
        TestFalse(TEXT("the foot row says it was not measured"), Foot->GetBoolField(TEXT("measured")));
        FString Reason;
        TestTrue(TEXT("and says why"), Foot->TryGetStringField(TEXT("reason"), Reason) && !Reason.IsEmpty());
        TestFalse(TEXT("an unmeasured foot has no implied ground speed"),
            Foot->HasField(TEXT("impliedGroundSpeed")));
        TestFalse(TEXT("an unmeasured foot has no slide fraction"),
            Foot->HasField(TEXT("slideFraction")));
        TestFalse(TEXT("an unmeasured foot has no per-foot verdict"),
            Foot->HasField(TEXT("pass")));
        TestFalse(TEXT("an unmeasured foot has no suggested play rate"),
            Foot->HasField(TEXT("suggestedPlayRate")));
    }
    return true;
#endif
}

// ---------------------------------------------------------------------------------------
// Naming a bone the skeleton does not have is a caller error, not a quietly missing row.
// ---------------------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAnimationMeasureMotionUnknownBoneErrorsTest,
    "PinWright.animation.measure_motion.UnknownBoneErrors",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FAnimationMeasureMotionUnknownBoneErrorsTest::RunTest(const FString& Parameters)
{
#if PINWRIGHT_MOTION_FIXTURE_UNSUPPORTED
    PinWrightTestSkip::SkipAssertions(*this, TEXT("engine-version-unsupported"),
        TEXT("Skipping measure_motion fixture tests on UE 5.3/5.4 (engine anim compression assert)."));
    return true;
#else
    const TArray<FVector> Locations = MakeMotionWalkCycleLocations();
    PinWrightAnimationTestFixtures::FRegisteredAnimationFixture Fixture;
    TestTrue(TEXT("registered motion fixture created"),
        Fixture.Create(TEXT("Motion_Unknown"), true, FFrameRate(30, 1), &Locations));
    UAnimSequence* Sequence = Fixture.Sequence;
    if (!Sequence)
    {
        return false;
    }

    TSharedPtr<FJsonObject> Payload = MakeMotionPayload(Sequence->GetPathName());
    TArray<TSharedPtr<FJsonValue>> Feet;
    Feet.Add(MakeShared<FJsonValueString>(TEXT("ball_r_that_does_not_exist")));
    Payload->SetArrayField(TEXT("footBones"), Feet);

    FTestResponseCapture Capture;
    if (!RunMeasureMotion(*this, Payload, Capture))
    {
        return false;
    }
    TestFalse(TEXT("a bone the skeleton lacks must not silently produce a clean measurement"),
        Capture.bSuccess);
    TestEqual(TEXT("and it must say which"), Capture.ErrorCode, FString(TEXT("BONE_NOT_FOUND")));
    return true;
#endif
}
