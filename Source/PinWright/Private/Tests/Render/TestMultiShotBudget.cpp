// Copyright (c) 2026 Alexander Penkin. MIT License.

// The caller-settable shot budget (`maxShots`, board F-multi-shot-ceiling-not-settable), the
// per-shot cost it is argued against (poseSet.elapsedMs / msPerShot), the zero-padded shot index a
// long set needs to sort in order (F-preview-turntable-capture), and `measureCoverage` on
// camera.orbit_shots (B-orbit-shots-no-subject-coverage).
//
// Every test except the two live-capture ones needs no viewport: the budget is parsed and enforced
// before any target is resolved, so both sides of it are assertable with no GPU and no window.
#include "Misc/AutomationTest.h"
#include "Handlers/ErrorCodes.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/HandlerRegistration.h"
#include "Handlers/ParamSpec.h"
#include "Handlers/Render/CameraShotPlanUtils.h"
#include "Handlers/Render/PoseListCapture.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Tests/TestUtils.h"
#include "Tests/TestSkipReporting.h"
#include "Tests/Infra/DispatcherTestHelpers.h"

#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"

namespace TestMultiShotBudgetHelpers
{
    // The four verbs the budget binds. Every one refuses a plan above it before capturing.
    const TCHAR* const BudgetVerbs[] = {
        TEXT("camera.orbit_shots"),
        TEXT("render.capture_asset_preview"),
        TEXT("camera.animation_shots"),
        TEXT("render.capture_animation_preview"),
    };

    const FParamSpec* FindDeclaredParam(const FString& Method, const FString& Name)
    {
        for (const FHandlerRegistration& Reg : FAutoRegisterHandler::GetPendingRegistrations())
        {
            if (Reg.MethodName != Method)
            {
                continue;
            }
            for (const FParamSpec& Spec : Reg.Params)
            {
                if (Spec.Name == Name)
                {
                    return &Spec;
                }
            }
        }
        return nullptr;
    }

    TSharedPtr<FJsonObject> PayloadWithMaxShots(double Value)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetNumberField(TEXT("maxShots"), Value);
        return Payload;
    }

    TSharedPtr<FJsonObject> MakeCubeSubject()
    {
        TSharedPtr<FJsonObject> Subject = MakeShared<FJsonObject>();
        Subject->SetStringField(TEXT("kind"), TEXT("staticMesh"));
        Subject->SetStringField(TEXT("path"), TEXT("/Engine/BasicShapes/Cube.Cube"));
        return Subject;
    }

    void DeleteShotFiles(const TSharedPtr<FJsonObject>& Result)
    {
        const TArray<TSharedPtr<FJsonValue>>* Shots = nullptr;
        if (!Result.IsValid() || !Result->TryGetArrayField(TEXT("shots"), Shots) || !Shots)
        {
            return;
        }
        for (const TSharedPtr<FJsonValue>& Value : *Shots)
        {
            const TSharedPtr<FJsonObject>* Shot = nullptr;
            FString Path;
            if (Value.IsValid() && Value->TryGetObject(Shot) && Shot &&
                (*Shot)->TryGetStringField(TEXT("path"), Path) && !Path.IsEmpty())
            {
                IFileManager::Get().Delete(*Path, false, true);
            }
        }
    }

    // Same typed exits the neighbouring camera tests accept when no preview can be opened here.
    bool IsTypedCaptureFailure(const FString& Code)
    {
        return Code == TEXT("NO_ACTIVE_LEVEL_VIEWPORT") || Code == TEXT("NO_EDITOR_WORLD") ||
            Code == TEXT("EDITOR_NOT_AVAILABLE") || Code == TEXT("CAPTURE_FAILED") ||
            Code == TEXT("PREVIEW_VIEWPORT_NOT_FOUND") || Code == TEXT("PREVIEW_NOT_FOUND") ||
            Code == TEXT("OPEN_FAILED") || Code == TEXT("SUBSYSTEM_MISSING") ||
            Code == TEXT("ENCODE_FAILED") || Code == TEXT("SAVE_FAILED") ||
            Code == TEXT("UNSUPPORTED_ASSET_EDITOR") || Code == TEXT("BOUNDS_EMPTY");
    }
}

// ---- the parser: default, both bounds, and refusal of anything that is not a whole number ----
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FShotBudgetParseBoundsTest,
    "PinWright.render.shot_budget.ParseMaxShotsBounds",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FShotBudgetParseBoundsTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightCameraFrame;
    using namespace TestMultiShotBudgetHelpers;

    // The default is the bound these verbs always had, so no existing call shape moves; and the
    // ceiling is above it, or the parameter could only ever lower the budget.
    TestEqual(TEXT("the default budget is still 24"), GMaxOrbitShots, 24);
    TestTrue(TEXT("the hard ceiling is above the default"), GMaxShotsCeiling > GMaxOrbitShots);

    int32 MaxShots = -1;
    FString Code;
    FString Msg;
    TestTrue(TEXT("an absent maxShots parses"), ParseMaxShots(MakeShared<FJsonObject>(), MaxShots, Code, Msg));
    TestEqual(TEXT("and takes the default"), MaxShots, GMaxOrbitShots);

    TestTrue(TEXT("25 is accepted"), ParseMaxShots(PayloadWithMaxShots(25), MaxShots, Code, Msg));
    TestEqual(TEXT("and is used as given"), MaxShots, 25);
    TestTrue(TEXT("the ceiling itself is accepted"),
        ParseMaxShots(PayloadWithMaxShots(GMaxShotsCeiling), MaxShots, Code, Msg));
    TestEqual(TEXT("at the ceiling"), MaxShots, GMaxShotsCeiling);

    TestFalse(TEXT("one over the ceiling is refused"),
        ParseMaxShots(PayloadWithMaxShots(GMaxShotsCeiling + 1), MaxShots, Code, Msg));
    TestEqual(TEXT("as TOO_MANY_SHOTS"), Code, FString(ErrorCodes::ERR_TOO_MANY_SHOTS));

    const double Malformed[] = { 0.0, -3.0, 1.5 };
    for (const double Value : Malformed)
    {
        Code.Reset();
        TestFalse(*FString::Printf(TEXT("maxShots %g is refused"), Value),
            ParseMaxShots(PayloadWithMaxShots(Value), MaxShots, Code, Msg));
        TestEqual(*FString::Printf(TEXT("maxShots %g is INVALID_ARGUMENT"), Value),
            Code, FString(ErrorCodes::ERR_INVALID_ARGUMENT));
    }
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("maxShots"), TEXT("lots"));
        Code.Reset();
        TestFalse(TEXT("a non-number is refused"), ParseMaxShots(Payload, MaxShots, Code, Msg));
        TestEqual(TEXT("as INVALID_ARGUMENT"), Code, FString(ErrorCodes::ERR_INVALID_ARGUMENT));
    }
    return true;
}

// ---- every one of the four verbs declares it and reads it, through the real dispatcher ----
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FShotBudgetReachesEveryVerbTest,
    "PinWright.render.shot_budget.EveryMultiShotVerbReadsMaxShots",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FShotBudgetReachesEveryVerbTest::RunTest(const FString& Parameters)
{
    using namespace TestMultiShotBudgetHelpers;

    for (const TCHAR* Method : BudgetVerbs)
    {
        const FParamSpec* Spec = FindDeclaredParam(Method, TEXT("maxShots"));
        if (TestNotNull(*FString::Printf(TEXT("%s declares maxShots"), Method), Spec) && Spec)
        {
            TestEqual(*FString::Printf(TEXT("%s: maxShots is an integer"), Method),
                Spec->Type, FString(TEXT("integer")));
            TestFalse(*FString::Printf(TEXT("%s: maxShots is optional"), Method), Spec->bRequired);
        }
    }

    // Read, not merely declared: a malformed value is refused by the parser, which runs before any
    // target is resolved. RequireRenderer runs first on every one of these verbs, hence the skip.
    if (PinWrightTestSkip::SkipIfRenderingUnavailable(*this)) { return true; }

    DispatcherTestHelpers::FSinkPtr Sink;
    FRpcDispatcher Dispatcher;
    DispatcherTestHelpers::MakeDispatcher(Sink, Dispatcher);
    for (const TCHAR* Method : BudgetVerbs)
    {
        // The dispatcher checks REQUIRED params before the handler runs; camera.animation_shots
        // requires sequencePath. A nonexistent one is fine: ParseMaxShots refuses first.
        auto Budget = [Method](double Value)
        {
            TSharedPtr<FJsonObject> Payload = PayloadWithMaxShots(Value);
            if (FCString::Strcmp(Method, TEXT("camera.animation_shots")) == 0)
            {
                Payload->SetStringField(TEXT("sequencePath"), TEXT("/Game/PinWrightTests/PW_NoSuchSequenceForShotBudget"));
            }
            return Payload;
        };
        bool bSuccess = true;
        FString ErrorCode;
        DispatcherTestHelpers::Dispatch(Dispatcher, Sink, Method,
            FString::Printf(TEXT("req-maxshots-zero-%s"), Method), Budget(0),
            bSuccess, ErrorCode);
        TestFalse(*FString::Printf(TEXT("%s refuses maxShots 0"), Method), bSuccess);
        TestEqual(*FString::Printf(TEXT("%s: maxShots 0 is INVALID_ARGUMENT (not UNKNOWN_PARAMS)"), Method),
            ErrorCode, FString(ErrorCodes::ERR_INVALID_ARGUMENT));

        DispatcherTestHelpers::Dispatch(Dispatcher, Sink, Method,
            FString::Printf(TEXT("req-maxshots-ceiling-%s"), Method),
            Budget(PinWrightCameraFrame::GMaxShotsCeiling + 1), bSuccess, ErrorCode);
        TestEqual(*FString::Printf(TEXT("%s: a budget above the ceiling is TOO_MANY_SHOTS"), Method),
            ErrorCode, FString(ErrorCodes::ERR_TOO_MANY_SHOTS));
    }
    return true;
}

// ---- the budget moves the refusal, in both directions ----
//
// camera.orbit_shots refuses the plan before it resolves the target, so a nonexistent actor makes
// the NEXT refusal ACTOR_NOT_FOUND: proof the plan was accepted, without a viewport.
// render.capture_asset_preview is asserted on the lowering side only (a 6-shot plan under a budget
// of 5), because an accepted set there would open an asset editor and capture it.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FShotBudgetMovesTheRefusalTest,
    "PinWright.render.shot_budget.BudgetMovesTheRefusal",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FShotBudgetMovesTheRefusalTest::RunTest(const FString& Parameters)
{
    if (PinWrightTestSkip::SkipIfRenderingUnavailable(*this)) { return true; }
    const int32 OverDefault = PinWrightCameraFrame::GMaxOrbitShots + 1;

    auto Orbit = [](int32 Count, TOptional<int32> MaxShots, FTestResponseCapture& Capture)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("actorName"), TEXT("PW_NoSuchActorForShotBudget"));
        Payload->SetNumberField(TEXT("count"), Count);
        if (MaxShots.IsSet())
        {
            Payload->SetNumberField(TEXT("maxShots"), MaxShots.GetValue());
        }
        return InvokeHandlerWithCapture(TEXT("camera.orbit_shots"), Payload, Capture);
    };

    {
        FTestResponseCapture Capture;
        TestTrue(TEXT("camera.orbit_shots handler found"), Orbit(OverDefault, {}, Capture));
        TestEqual(TEXT("without maxShots, one over the default is still refused"),
            Capture.ErrorCode, FString(ErrorCodes::ERR_TOO_MANY_SHOTS));
        TestTrue(TEXT("and the refusal names the lever"), Capture.Message.Contains(TEXT("maxShots")));
    }
    {
        FTestResponseCapture Capture;
        Orbit(OverDefault, OverDefault, Capture);
        TestFalse(TEXT("the unresolvable fixture still fails"), Capture.bSuccess);
        TestEqual(TEXT("with maxShots raised, the same plan is accepted and fails on the target"),
            Capture.ErrorCode, FString(ErrorCodes::ERR_ACTOR_NOT_FOUND));
    }
    {
        FTestResponseCapture Capture;
        Orbit(4, 3, Capture);
        TestEqual(TEXT("a budget below the plan is refused even under the default"),
            Capture.ErrorCode, FString(ErrorCodes::ERR_TOO_MANY_SHOTS));
    }
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetObjectField(TEXT("subject"), TestMultiShotBudgetHelpers::MakeCubeSubject());
        Payload->SetNumberField(TEXT("count"), 6);
        Payload->SetNumberField(TEXT("maxShots"), 5);
        FTestResponseCapture Capture;
        TestTrue(TEXT("render.capture_asset_preview handler found"),
            InvokeHandlerWithCapture(TEXT("render.capture_asset_preview"), Payload, Capture));
        TestEqual(TEXT("render.capture_asset_preview refuses a plan above the caller's budget"),
            Capture.ErrorCode, FString(ErrorCodes::ERR_TOO_MANY_SHOTS));
    }
    return true;
}

// ---- the TIME bound: size and coverage, not only count, decide how long a set may be ----
//
// rpc-design §9: a synchronous verb is bounded on its cost driver. 360 shots at 256 px fit without
// coverage (~75 s predicted) and do not with it (~149 s, past the 120 s response timeout); 24 shots
// at the maximum size are refused although 24 is the default count budget.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FShotBudgetPredictedTimeTest,
    "PinWright.render.shot_budget.PredictedTimeRefusesPastTheTimeout",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FShotBudgetPredictedTimeTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightCameraFrame;
    FString Msg;
    TestTrue(TEXT("360 shots at 256 px without coverage fit"),
        CheckShotSetCost(GMaxShotsCeiling, 256, 256, false, Msg));
    TestFalse(TEXT("the same set with coverage does not"),
        CheckShotSetCost(GMaxShotsCeiling, 256, 256, true, Msg));
    TestTrue(TEXT("the refusal states the predicted seconds"),
        Msg.Contains(TEXT("predicted")) && Msg.Contains(TEXT(" s ")));
    TestTrue(TEXT("and names the coverage lever"), Msg.Contains(TEXT("measureCoverage:false")));
    TestFalse(TEXT("24 shots at the maximum size are refused"),
        CheckShotSetCost(GMaxOrbitShots, GMaxCaptureDimension, GMaxCaptureDimension, false, Msg));
    // Coverage adds one draw per shot; the warm-up adds one draw per set (a 0-shot set is it alone).
    const double OneDraw = PredictShotSetSeconds(0, 512, 512, false);
    TestTrue(TEXT("coverage adds one draw per shot"), FMath::IsNearlyEqual(
        PredictShotSetSeconds(10, 512, 512, true) - PredictShotSetSeconds(10, 512, 512, false), 10.0 * OneDraw));
    TestTrue(TEXT("the warm-up is counted"), FMath::IsNearlyEqual(
        PredictShotSetSeconds(10, 512, 512, false), 11.0 * OneDraw));
    TestFalse(TEXT("3 shots at the maximum size are refused (4 full-size draws with the warm-up)"),
        CheckShotSetCost(3, GMaxCaptureDimension, GMaxCaptureDimension, false, Msg));
    TestTrue(TEXT("size raises the prediction"),
        PredictShotSetSeconds(10, 2048, 2048, false) > PredictShotSetSeconds(10, 256, 256, false));

    // Through the verbs, before any target is resolved. RequireRenderer runs first, hence the skip.
    if (PinWrightTestSkip::SkipIfRenderingUnavailable(*this)) { return true; }
    auto Run = [](const TCHAR* Method, bool bSubject, int32 Count, int32 Edge, FTestResponseCapture& Capture)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        if (bSubject)
        {
            Payload->SetObjectField(TEXT("subject"), TestMultiShotBudgetHelpers::MakeCubeSubject());
        }
        else
        {
            Payload->SetStringField(TEXT("actorName"), TEXT("PW_NoSuchActorForShotBudget"));
        }
        Payload->SetNumberField(TEXT("count"), Count);
        Payload->SetNumberField(TEXT("maxShots"), FMath::Max(Count, GMaxOrbitShots));
        Payload->SetNumberField(TEXT("width"), Edge);
        Payload->SetNumberField(TEXT("height"), Edge);
        return InvokeHandlerWithCapture(Method, Payload, Capture);
    };
    {
        FTestResponseCapture Capture;
        TestTrue(TEXT("camera.orbit_shots handler found"),
            Run(TEXT("camera.orbit_shots"), false, GMaxOrbitShots, GMaxCaptureDimension, Capture));
        TestEqual(TEXT("orbit: 24 shots at the maximum size are TOO_MANY_SHOTS"),
            Capture.ErrorCode, FString(ErrorCodes::ERR_TOO_MANY_SHOTS));
        TestTrue(TEXT("orbit: with the predicted seconds"), Capture.Message.Contains(TEXT("predicted")));
    }
    {
        FTestResponseCapture Capture;
        Run(TEXT("camera.orbit_shots"), false, GMaxShotsCeiling, 256, Capture);
        TestEqual(TEXT("orbit: 360 actor shots at 256 px pay no coverage draw, fit, and fail on the target"),
            Capture.ErrorCode, FString(ErrorCodes::ERR_ACTOR_NOT_FOUND));
    }
    {
        FTestResponseCapture Capture;
        Run(TEXT("camera.orbit_shots"), true, GMaxShotsCeiling, 256, Capture);
        TestEqual(TEXT("orbit: 360 subject shots at 256 px with default coverage are refused"),
            Capture.ErrorCode, FString(ErrorCodes::ERR_TOO_MANY_SHOTS));
    }
    {
        FTestResponseCapture Capture;
        TestTrue(TEXT("render.capture_asset_preview handler found"),
            Run(TEXT("render.capture_asset_preview"), true, 300, 256, Capture));
        TestEqual(TEXT("asset preview: 300 shots at 256 px with default coverage are refused"),
            Capture.ErrorCode, FString(ErrorCodes::ERR_TOO_MANY_SHOTS));
        TestTrue(TEXT("asset preview: with the predicted seconds"), Capture.Message.Contains(TEXT("predicted")));
    }
    return true;
}

// ---- the set publishes its own cost, so the bound's premise is checkable ----
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FShotBudgetPoseSetPublishesCostTest,
    "PinWright.render.shot_budget.PoseSetPublishesPerShotCost",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FShotBudgetPoseSetPublishesCostTest::RunTest(const FString& Parameters)
{
    PinWrightPoseCapture::FPoseListCaptureRequest Request;
    Request.bWarmupShot = false;
    for (int32 Index = 0; Index < 3; ++Index)
    {
        PinWrightPoseCapture::FCameraPose Pose;
        Pose.Location = FVector(-1000.0, 0.0, 0.0);
        Pose.Filename = FString::Printf(TEXT("ShotBudgetCost_%d.png"), Index);
        Request.Poses.Add(Pose);
    }
    // A stub frame that costs real wall time, so a zero elapsed figure cannot pass by accident.
    const PinWrightPoseCapture::FPoseFrameCapturer Capturer =
        [](const PinWrightRenderCapture::FViewportCaptureRequest& Frame, bool /*bWarmupFrame*/,
           PinWrightRenderCapture::FViewportCaptureOutput& OutCapture, FString&, FString&)
    {
        FPlatformProcess::Sleep(0.005f);
        OutCapture.Filename = Frame.Filename;
        OutCapture.Width = Frame.Width;
        OutCapture.Height = Frame.Height;
        OutCapture.Path = FString::Printf(TEXT("/stub/%s"), *Frame.Filename);
        OutCapture.EffectiveLocation = Frame.Location;
        OutCapture.EffectiveRotation = Frame.Rotation;
        return true;
    };

    PinWrightPoseCapture::FPoseListCaptureOutput Output;
    FString ErrorCode;
    FString ErrorMessage;
    if (!TestTrue(TEXT("the stub set captured"),
            PinWrightPoseCapture::RunPoseListCapture(Request, Capturer, Output, ErrorCode, ErrorMessage)))
    {
        AddError(FString::Printf(TEXT("%s / %s"), *ErrorCode, *ErrorMessage));
        return false;
    }
    TestEqual(TEXT("precondition: three shots"), Output.Captures.Num(), 3);
    TestTrue(TEXT("the sequence measured its own wall time"), Output.ElapsedSeconds >= 0.012);

    const TSharedPtr<FJsonObject> Info = PinWrightPoseCapture::MakePoseSetInfoObject(Output);
    double ElapsedMs = -1.0;
    double MsPerShot = -1.0;
    TestTrue(TEXT("poseSet publishes elapsedMs"), Info->TryGetNumberField(TEXT("elapsedMs"), ElapsedMs));
    TestTrue(TEXT("poseSet publishes msPerShot"), Info->TryGetNumberField(TEXT("msPerShot"), MsPerShot));
    TestTrue(TEXT("msPerShot is elapsedMs over the captured shots"),
        FMath::IsNearlyEqual(MsPerShot, ElapsedMs / 3.0, 1e-6));
    return true;
}

// ---- a long set's filenames sort in shot order ----
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FShotBudgetShotIndexSortsTest,
    "PinWright.render.shot_budget.ShotIndexSortsInOrder",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FShotBudgetShotIndexSortsTest::RunTest(const FString& Parameters)
{
    using PinWrightCameraFrame::FormatShotIndex;
    // Sets of up to 100 keep the two digits they always had, so their names are byte-identical.
    TestEqual(TEXT("24-shot set"), FormatShotIndex(5, 24), FString(TEXT("05")));
    TestEqual(TEXT("100-shot set"), FormatShotIndex(99, 100), FString(TEXT("99")));
    TestEqual(TEXT("240-shot set"), FormatShotIndex(5, 240), FString(TEXT("005")));
    TestEqual(TEXT("240-shot set, last"), FormatShotIndex(239, 240), FString(TEXT("239")));

    // The property the turntable needs: lexical order IS shot order for every index of a long set.
    const int32 Total = PinWrightCameraFrame::GMaxShotsCeiling;
    for (int32 Index = 1; Index < Total; ++Index)
    {
        if (!(FormatShotIndex(Index - 1, Total) < FormatShotIndex(Index, Total)))
        {
            AddError(FString::Printf(TEXT("shot %d sorts before shot %d"), Index, Index - 1));
            break;
        }
    }
    return true;
}

// ---- measureCoverage reaches camera.orbit_shots, declared with the sibling's semantics ----
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOrbitCoverageDeclaredTest,
    "PinWright.camera.orbit_shots.MeasureCoverageDeclaredLikeAssetPreview",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FOrbitCoverageDeclaredTest::RunTest(const FString& Parameters)
{
    using namespace TestMultiShotBudgetHelpers;
    const FParamSpec* Orbit = FindDeclaredParam(TEXT("camera.orbit_shots"), TEXT("measureCoverage"));
    const FParamSpec* Preview = FindDeclaredParam(TEXT("render.capture_asset_preview"), TEXT("measureCoverage"));
    if (!TestNotNull(TEXT("camera.orbit_shots declares measureCoverage"), Orbit) ||
        !TestNotNull(TEXT("render.capture_asset_preview declares measureCoverage"), Preview) ||
        !Orbit || !Preview)
    {
        return false;
    }
    TestEqual(TEXT("same type"), Orbit->Type, Preview->Type);
    TestEqual(TEXT("same published default"), Orbit->Default, Preview->Default);
    TestFalse(TEXT("optional"), Orbit->bRequired);
    return true;
}

// ---- live: a set longer than the old ceiling, over an asset subject, carries coverage ----
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOrbitLongSetWithCoverageTest,
    "PinWright.camera.orbit_shots.LongSetOverTheOldCeilingCarriesCoverage",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FOrbitLongSetWithCoverageTest::RunTest(const FString& Parameters)
{
    if (PinWrightTestSkip::SkipIfRenderingUnavailable(*this)) { return true; }
    using namespace TestMultiShotBudgetHelpers;
    const int32 Count = PinWrightCameraFrame::GMaxOrbitShots + 1;

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetObjectField(TEXT("subject"), MakeCubeSubject());
    Payload->SetNumberField(TEXT("count"), Count);
    Payload->SetNumberField(TEXT("maxShots"), Count);
    // 256, the constant size every neighbouring camera test uses: a varying capture size within
    // one session is the FViewport::GetHitProxy assert class.
    Payload->SetNumberField(TEXT("width"), 256);
    Payload->SetNumberField(TEXT("height"), 256);

    FTestResponseCapture Capture;
    TestTrue(TEXT("camera.orbit_shots handler found"),
        InvokeHandlerWithCapture(TEXT("camera.orbit_shots"), Payload, Capture));
    if (!Capture.bSuccess || !Capture.Result.IsValid())
    {
        TestTrue(FString::Printf(TEXT("capture failure is typed (%s)"), *Capture.ErrorCode),
            IsTypedCaptureFailure(Capture.ErrorCode));
        PinWrightTestSkip::SkipAssertions(*this, TEXT("asset-preview-capture-unavailable"),
            FString::Printf(TEXT("Skipped the long-set assertions: preview capture unavailable (%s). "
                "BudgetMovesTheRefusal covers the bound without a viewport."), *Capture.ErrorCode));
        return true;
    }

    const TArray<TSharedPtr<FJsonValue>>* Shots = nullptr;
    if (TestTrue(TEXT("shots array present"), Capture.Result->TryGetArrayField(TEXT("shots"), Shots)) && Shots)
    {
        TestEqual(TEXT("every shot of a set over the old 24 was captured"), Shots->Num(), Count);
        int32 Measured = 0;
        for (const TSharedPtr<FJsonValue>& Value : *Shots)
        {
            const TSharedPtr<FJsonObject>* Shot = nullptr;
            double Coverage = -1.0;
            if (Value->TryGetObject(Shot) && Shot && (*Shot)->TryGetNumberField(TEXT("subjectCoverage"), Coverage))
            {
                ++Measured;
                TestTrue(TEXT("the cube covers part of each frame"), Coverage > 0.0);
            }
        }
        TestEqual(TEXT("subjectCoverage is published on every shot (default on)"), Measured, Count);
    }
    const TSharedPtr<FJsonObject>* PoseSet = nullptr;
    if (TestTrue(TEXT("poseSet present"), Capture.Result->TryGetObjectField(TEXT("poseSet"), PoseSet)) && PoseSet)
    {
        TestEqual(TEXT("the bound in force is the caller's budget"),
            static_cast<int32>((*PoseSet)->GetNumberField(TEXT("maxPosesPerCall"))), Count);
        TestEqual(TEXT("one reference frame per shot"),
            static_cast<int32>((*PoseSet)->GetNumberField(TEXT("coverageReferenceShots"))), Count);
        TestTrue(TEXT("the set published its per-shot cost"), (*PoseSet)->HasField(TEXT("msPerShot")));
    }
    DeleteShotFiles(Capture.Result);

    // The opt-out is honoured: no differential, no figure, no reference frames.
    Payload->SetNumberField(TEXT("count"), 2);
    Payload->RemoveField(TEXT("maxShots"));
    Payload->SetBoolField(TEXT("measureCoverage"), false);
    FTestResponseCapture OptOut;
    InvokeHandlerWithCapture(TEXT("camera.orbit_shots"), Payload, OptOut);
    if (TestTrue(TEXT("the opt-out capture succeeded"), OptOut.bSuccess && OptOut.Result.IsValid()))
    {
        const TArray<TSharedPtr<FJsonValue>>* OptOutShots = nullptr;
        if (OptOut.Result->TryGetArrayField(TEXT("shots"), OptOutShots) && OptOutShots)
        {
            for (const TSharedPtr<FJsonValue>& Value : *OptOutShots)
            {
                const TSharedPtr<FJsonObject>* Shot = nullptr;
                TestFalse(TEXT("measureCoverage:false publishes no subjectCoverage"),
                    Value->TryGetObject(Shot) && Shot && (*Shot)->HasField(TEXT("subjectCoverage")));
            }
        }
        DeleteShotFiles(OptOut.Result);
    }
    return true;
}
