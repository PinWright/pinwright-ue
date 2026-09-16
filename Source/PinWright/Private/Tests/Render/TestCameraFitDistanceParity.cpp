// Copyright (c) 2026 Alexander Penkin. MIT License.

// Parameter-parity tests for the two inputs of
// PinWrightCameraFrame::ComputeFitDistance(Radius, Fov, Padding), and for the camera verbs'
// `previewScene` declaration.
//
// WHAT WAS WRONG. camera.frame_actor and camera.orbit_shots call the same helper and exposed
// OPPOSITE HALVES of it. orbit_shots took an explicit `radius` (the distance) and welded the
// margin as `constexpr float Padding = 1.15f` -- the only 1.15f literal in the whole Private/
// tree. frame_actor took `padding` and had no distance override at all, always auto-fitting.
// Same namespace, same maths, same file, inverted gaps. camera.animation_shots already exposed
// BOTH, which is the proof this was oversight rather than design: the union already shipped.
//
// WHY THE CONVERGENCE WAVE COULD NOT HAVE CAUGHT IT. Both of its matrices used the same column
// axis -- the six subject domains -- and a fully green row is perfectly compatible with a welded
// constant. A verb that never lets a caller reach a parameter cannot fail a per-domain check.
// The general cross-verb walk is a separate piece of work; this file pins the known instance.
//
// EVERY ASSERTION BELOW THAT CARRIES A CRITERION IS HEADLESS. ComputeFitDistance,
// ResolveCameraDistance and the registered FParamSpec lists need no viewport, no RHI, no world
// and no asset, so none of them can take a conditional-skip path and report success having
// asserted nothing (board ticket B-test-skips-assertions-silently). The live-capture halves are
// ADDITIONAL: they are what proves the parameter is actually WIRED rather than merely declared --
// this is the verb on which `viewMode` shipped inert and asserting "the call succeeded" is
// exactly how that happened -- and they are guarded, so they run when a viewport exists and
// AddInfo-skip when one does not. A skipped live half never leaves a test with nothing asserted.
//
// COUNTERFACTUAL. Revert the orbit_shots half (put `constexpr float Padding = 1.15f` back and
// drop the parameter) and PaddingActuallyMovesTheCamera fails on the live ratio while
// CameraVerbsExposeBothFitInputs fails headlessly on the missing spec. Revert the frame_actor
// half (drop `distance`) and ExplicitDistanceSkipsTheFit fails on both halves. Neither reverts
// to a green suite, which is the property this file exists to have.

#include "Misc/AutomationTest.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#include "Handlers/HandlerRegistration.h"
#include "Handlers/ParamSpec.h"
#include "Handlers/Render/CameraShotPlanUtils.h"

#include "Tests/TestUtils.h"
#include "Tests/TestWorldUtils.h"
#include "Tests/TestSkipReporting.h"

#include "Editor.h"
#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "Misc/Guid.h"

// Deliberately NO file-scope using-directive for PinWrightCameraFrame, matching the rule its own
// header states and TestShotPlanSides.cpp already follows: under Unity the directive would leak
// into every later translation unit in the blob and collide with the same-named
// anonymous-namespace helpers in sibling Render files. Each test body pulls it in locally.
namespace
{
    // ---- fixture numbers, chosen so no two of them can be confused for each other ----
    //
    // 250 cm is not a bounds radius any fixture in this tree happens to produce, and 50 degrees is
    // the shipped default fov, so a distance computed here cannot coincide with one computed off a
    // spawned cube by accident.
    constexpr float GPwFitRadius = 250.0f;
    constexpr float GPwFitFov = 50.0f;

    // The margin camera.orbit_shots welded before it had a parameter, written here as a LITERAL
    // and never read back from PinWrightCameraFrame::GDefaultFitPadding. Reading the header
    // constant would make this test pass for whatever the constant is later changed to, which is
    // precisely the assertion that cannot fail. The test carries its own copy of the contract.
    constexpr float GPwFitWeldedPadding = 1.15f;

    // A distance no bounds fit in this file produces, so "the camera is at 500" can only be the
    // override. The tests assert that separation rather than assuming it.
    constexpr float GPwFitExplicitDistance = 500.0f;

    const FHandlerRegistration* PwFitFindRegistration(const FString& Method)
    {
        for (const FHandlerRegistration& Reg : FAutoRegisterHandler::GetPendingRegistrations())
        {
            if (Reg.MethodName == Method)
            {
                return &Reg;
            }
        }
        return nullptr;
    }

    // The REGISTERED spec, which is what the dispatcher's unknown-param gate and the wiki
    // generator both read. Deliberately not a source-text grep: a parameter present in the file
    // but absent from the registered list is unreachable, and a grep would call that fixed.
    const FParamSpec* PwFitFindParam(const FHandlerRegistration& Reg, const TCHAR* Name)
    {
        for (const FParamSpec& Spec : Reg.Params)
        {
            if (Spec.Name == Name)
            {
                return &Spec;
            }
            if (Spec.Aliases.Contains(FString(Name)))
            {
                return &Spec;
            }
        }
        return nullptr;
    }

    // Asserts the named verb declares the named parameter, and returns it (or null).
    const FParamSpec* PwFitRequireParam(FAutomationTestBase& Test, const FString& Method,
        const TCHAR* Name)
    {
        const FHandlerRegistration* Reg = PwFitFindRegistration(Method);
        Test.TestNotNull(*FString::Printf(TEXT("%s is registered"), *Method), Reg);
        if (!Reg)
        {
            return nullptr;
        }
        const FParamSpec* Spec = PwFitFindParam(*Reg, Name);
        Test.TestNotNull(*FString::Printf(TEXT("%s declares `%s`"), *Method, Name), Spec);
        return Spec;
    }

    // The typed, non-crashing exits a camera verb is allowed to return when the live capture
    // cannot complete in this run. Same list and same rationale as TestCameraFrameHandlers.cpp:
    // an empty or unknown code still fails, so a real defect cannot hide behind the guard.
    bool PwFitIsTypedCaptureFailure(const FString& ErrorCode)
    {
        return ErrorCode == TEXT("NO_ACTIVE_LEVEL_VIEWPORT") ||
            ErrorCode == TEXT("NO_EDITOR_WORLD") ||
            ErrorCode == TEXT("EDITOR_NOT_AVAILABLE") ||
            ErrorCode == TEXT("CAPTURE_FAILED") ||
            ErrorCode == TEXT("PREVIEW_VIEWPORT_NOT_FOUND") ||
            ErrorCode == TEXT("ENCODE_FAILED") ||
            ErrorCode == TEXT("SAVE_FAILED");
    }

    // A NON-transient cube the camera verbs can actually resolve. RF_Transient actors are filtered
    // out by UEditorActorSubsystem::GetAllLevelActors (UE 5.8 EditorActorSubsystem.cpp:386), which
    // every actorName lookup walks, so the shared SpawnTransientCubeActor fixture is permanently
    // invisible to these verbs and a test built on it exits before the verb does any work.
    AStaticMeshActor* PwFitSpawnResolvableCube(UWorld* World, const FString& Label)
    {
        if (!World)
        {
            return nullptr;
        }
        UStaticMesh* CubeMesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
        if (!CubeMesh)
        {
            return nullptr;
        }
        FActorSpawnParameters SpawnParams; // deliberately NOT RF_Transient (see above)
        AStaticMeshActor* Actor = World->SpawnActor<AStaticMeshActor>(
            AStaticMeshActor::StaticClass(), FVector::ZeroVector, FRotator::ZeroRotator, SpawnParams);
        if (!Actor)
        {
            return nullptr;
        }
        Actor->GetStaticMeshComponent()->SetStaticMesh(CubeMesh);
        Actor->SetActorLabel(Label);
        return Actor;
    }

    void PwFitDeleteFile(const FString& Path)
    {
        if (!Path.IsEmpty())
        {
            IFileManager::Get().Delete(*Path, /*RequireExists=*/false, /*EvenReadOnly=*/true,
                /*Quiet=*/true);
        }
    }

    // Reads a {x, y, z} object off a JSON parent.
    bool PwFitReadVector(const TSharedPtr<FJsonObject>& Parent, const TCHAR* Field, FVector& Out)
    {
        const TSharedPtr<FJsonObject>* Obj = nullptr;
        if (!Parent.IsValid() || !Parent->TryGetObjectField(Field, Obj) || !Obj || !(*Obj).IsValid())
        {
            return false;
        }
        double X = 0.0, Y = 0.0, Z = 0.0;
        if (!(*Obj)->TryGetNumberField(TEXT("x"), X) ||
            !(*Obj)->TryGetNumberField(TEXT("y"), Y) ||
            !(*Obj)->TryGetNumberField(TEXT("z"), Z))
        {
            return false;
        }
        Out = FVector(X, Y, Z);
        return true;
    }

    // The measured camera position of shot 0 of an orbit response, plus its PNG path so the test
    // can clean up. Returns false when the shot reports `requestedLocation`, which the shot
    // serializer emits only when the aim was NOT applied -- in that case the camera is not where
    // the pose asked and a distance read off it would measure the viewport, not the verb.
    bool PwFitFirstShotCamera(const TSharedPtr<FJsonObject>& Result, FVector& OutLocation,
        FString& OutPath)
    {
        const TArray<TSharedPtr<FJsonValue>>* Shots = nullptr;
        if (!Result.IsValid() || !Result->TryGetArrayField(TEXT("shots"), Shots) ||
            !Shots || Shots->Num() == 0)
        {
            return false;
        }
        const TSharedPtr<FJsonObject>* ShotObj = nullptr;
        if (!(*Shots)[0]->TryGetObject(ShotObj) || !ShotObj || !(*ShotObj).IsValid())
        {
            return false;
        }
        (*ShotObj)->TryGetStringField(TEXT("path"), OutPath);
        if ((*ShotObj)->HasField(TEXT("requestedLocation")))
        {
            return false;
        }
        return PwFitReadVector(*ShotObj, TEXT("cameraLocation"), OutLocation);
    }

    // The same, for camera.frame_actor's single-shot response, whose fields sit at the top level.
    bool PwFitSingleShotCamera(const TSharedPtr<FJsonObject>& Result, FVector& OutLocation,
        FString& OutPath)
    {
        if (!Result.IsValid())
        {
            return false;
        }
        Result->TryGetStringField(TEXT("path"), OutPath);
        if (Result->HasField(TEXT("requestedLocation")))
        {
            return false;
        }
        return PwFitReadVector(Result, TEXT("cameraLocation"), OutLocation);
    }
}

// ============================================================================
// camera.orbit_shots — omitting `padding` must solve the distance the welded
// constant used to solve.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwOrbitPaddingDefaultMatchesWeldedConstantTest,
    "PinWright.camera.orbit_shots.PaddingDefaultMatchesTheWeldedConstant",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwOrbitPaddingDefaultMatchesWeldedConstantTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightCameraFrame;

    // The DISTANCE the welded 1.15f produced, computed from the helper at assert time rather than
    // written out as a number. Comparing the two paddings would be comparing defaults; the
    // criterion is about where the camera ends up.
    const float WeldedDistance = ComputeFitDistance(GPwFitRadius, GPwFitFov, GPwFitWeldedPadding);

    // The distance the verb solves when the caller passes no `padding`: the same call the handler
    // makes, with the same default the handler applies.
    const float DefaultedDistance = ResolveCameraDistance(
        /*bDistanceProvided=*/false, /*DistanceOverride=*/0.0f,
        /*bSubjectRadiusProvided=*/false, /*SubjectRadius=*/0.0f,
        GPwFitRadius, GPwFitFov, GDefaultFitPadding);

    TestTrue(TEXT("omitting `padding` solves the distance the welded 1.15f solved"),
        FMath::IsNearlyEqual(DefaultedDistance, WeldedDistance, 1e-3f));

    // Guard against the equality above being a tautology of the helper: a DIFFERENT margin must
    // produce a different distance, or "these two agree" says nothing at all.
    TestFalse(TEXT("a different margin does not solve the same distance"),
        FMath::IsNearlyEqual(ComputeFitDistance(GPwFitRadius, GPwFitFov, 1.4f),
            WeldedDistance, 1e-3f));

    // The DOCUMENTED default and the CODE default must land on the same distance. This is the one
    // assertion that reaches the wire contract: `Default` is what the wiki generator prints, and a
    // caller reading "default 1.4" while the code applies 1.15 is a defect no pixel test sees.
    const FParamSpec* Spec = PwFitRequireParam(*this, TEXT("camera.orbit_shots"), TEXT("padding"));
    if (Spec)
    {
        TestFalse(TEXT("camera.orbit_shots' `padding` declares a default at all"),
            Spec->Default.IsEmpty());
        if (!Spec->Default.IsEmpty())
        {
            const float DeclaredPadding = FCString::Atof(*Spec->Default);
            TestTrue(TEXT("the documented default solves the same distance as the code default"),
                FMath::IsNearlyEqual(
                    ComputeFitDistance(GPwFitRadius, GPwFitFov, DeclaredPadding),
                    WeldedDistance, 1e-3f));
        }
    }

    // ---- live half: prove the HANDLER applies that default, not just the header ----
    //
    // Omitted `padding` and an explicit `padding: 1.15` must put the camera in the same place.
    // Nothing above can prove that -- the handler could read a different constant and every
    // headless assertion would still pass.
    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("Skipped the live half: no editor world. The headless assertions above ran."));
        return true;
    }

    FScopedEditorWorldActorGuard ActorGuard;
    const FString Label = FString::Printf(TEXT("PW_FitPadDefault_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    AStaticMeshActor* Actor = PwFitSpawnResolvableCube(World, Label);
    if (!Actor)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("engine-cube-unavailable"),
            TEXT("Skipped the live half: engine cube unavailable. Headless assertions ran."));
        return true;
    }

    FVector Center = FVector::ZeroVector;
    FVector Extent = FVector::ZeroVector;
    Actor->GetActorBounds(false, Center, Extent);

    TArray<FString> Paths;
    FVector OmittedCamera = FVector::ZeroVector;
    FVector ExplicitCamera = FVector::ZeroVector;
    bool bOmittedRead = false;
    bool bExplicitRead = false;

    for (int32 Pass = 0; Pass < 2; ++Pass)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("actorName"), Actor->GetActorLabel());
        Payload->SetNumberField(TEXT("count"), 1.0);
        if (Pass == 1)
        {
            Payload->SetNumberField(TEXT("padding"), GPwFitWeldedPadding);
        }

        FTestResponseCapture Capture;
        InvokeHandlerWithCapture(TEXT("camera.orbit_shots"), Payload, Capture);
        if (!Capture.bSuccess)
        {
            TestTrue(TEXT("live capture failure is typed"),
                PwFitIsTypedCaptureFailure(Capture.ErrorCode));
            AddInfo(TEXT("Skipped the live half: viewport capture unavailable."));
            break;
        }

        FString Path;
        FVector CameraLocation = FVector::ZeroVector;
        const bool bRead = PwFitFirstShotCamera(Capture.Result, CameraLocation, Path);
        Paths.AddUnique(Path);
        if (Pass == 0)
        {
            OmittedCamera = CameraLocation;
            bOmittedRead = bRead;
        }
        else
        {
            ExplicitCamera = CameraLocation;
            bExplicitRead = bRead;
        }
    }

    if (bOmittedRead && bExplicitRead)
    {
        // Same place, not merely a similar distance: an omitted `padding` and an explicit 1.15
        // are the same request, so the two poses must coincide.
        TestTrue(TEXT("omitted `padding` and an explicit 1.15 put the camera in the same place"),
            OmittedCamera.Equals(ExplicitCamera, 0.5));
        // ...and the place must be the fit distance for THIS fixture, so the equality above is not
        // two identical wrong answers.
        const float FixtureRadius = FMath::Max(static_cast<float>(Extent.Size()), 1.0f);
        const float FixtureFit = ComputeFitDistance(FixtureRadius, GPwFitFov, GPwFitWeldedPadding);
        TestTrue(TEXT("the camera sits at the welded margin's fit distance for this fixture"),
            FMath::IsNearlyEqual(static_cast<float>(
                FVector::Dist(OmittedCamera, Center)), FixtureFit, 1.0f));
    }
    else
    {
        AddInfo(TEXT("Skipped the pose comparison: the shot did not report an applied camera aim."));
    }

    for (const FString& Path : Paths)
    {
        PwFitDeleteFile(Path);
    }
    return true;
}

// ============================================================================
// camera.orbit_shots — `padding` must actually reach the camera.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwOrbitPaddingActuallyMovesTheCameraTest,
    "PinWright.camera.orbit_shots.PaddingActuallyMovesTheCamera",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwOrbitPaddingActuallyMovesTheCameraTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightCameraFrame;

    const float ExpectedRatio = 2.0f / GPwFitWeldedPadding;

    const float Near = ResolveCameraDistance(false, 0.0f, false, 0.0f,
        GPwFitRadius, GPwFitFov, GPwFitWeldedPadding);
    const float Far = ResolveCameraDistance(false, 0.0f, false, 0.0f,
        GPwFitRadius, GPwFitFov, 2.0f);

    TestTrue(TEXT("padding 2.0 is strictly further out than padding 1.15"), Far > Near);
    // The RATIO, within 1e-3, not "further out by some amount": a tolerance wider than the
    // difference would pass on a parameter that moved the camera by a centimetre.
    TestTrue(TEXT("the distance ratio is exactly 2.0/1.15"),
        FMath::IsNearlyEqual(Far / Near, ExpectedRatio, 1e-3f));

    // ---- live half: the parameter must be WIRED, not merely declared ----
    //
    // This is the verb on which `viewMode` shipped inert -- accepted, echoed, and changing
    // nothing. Asserting that a call with `padding: 2.0` succeeds would reproduce that defect
    // exactly. The measured camera position is the only thing that cannot be faked.
    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("Skipped the live half: no editor world. The headless assertions above ran."));
        return true;
    }

    FScopedEditorWorldActorGuard ActorGuard;
    const FString Label = FString::Printf(TEXT("PW_FitPadMoves_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    AStaticMeshActor* Actor = PwFitSpawnResolvableCube(World, Label);
    if (!Actor)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("engine-cube-unavailable"),
            TEXT("Skipped the live half: engine cube unavailable. Headless assertions ran."));
        return true;
    }

    FVector Center = FVector::ZeroVector;
    FVector Extent = FVector::ZeroVector;
    Actor->GetActorBounds(false, Center, Extent);

    const float Paddings[2] = { GPwFitWeldedPadding, 2.0f };
    double Distances[2] = { 0.0, 0.0 };
    bool bRead[2] = { false, false };
    TArray<FString> Paths;

    for (int32 Pass = 0; Pass < 2; ++Pass)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("actorName"), Actor->GetActorLabel());
        Payload->SetNumberField(TEXT("count"), 1.0);
        Payload->SetNumberField(TEXT("padding"), Paddings[Pass]);

        FTestResponseCapture Capture;
        InvokeHandlerWithCapture(TEXT("camera.orbit_shots"), Payload, Capture);
        if (!Capture.bSuccess)
        {
            TestTrue(TEXT("live capture failure is typed"),
                PwFitIsTypedCaptureFailure(Capture.ErrorCode));
            AddInfo(TEXT("Skipped the live half: viewport capture unavailable."));
            break;
        }

        // A supplied `padding` that the verb honoured must NOT be reported as ignored. The field
        // is only emitted when an explicit distance won, and nothing here supplies one.
        TestFalse(TEXT("an honoured `padding` is not reported as ignored"),
            Capture.Result.IsValid() && Capture.Result->HasField(TEXT("paddingIgnored")));

        FString Path;
        FVector CameraLocation = FVector::ZeroVector;
        bRead[Pass] = PwFitFirstShotCamera(Capture.Result, CameraLocation, Path);
        Distances[Pass] = FVector::Dist(CameraLocation, Center);
        Paths.AddUnique(Path);
    }

    if (bRead[0] && bRead[1] && Distances[0] > KINDA_SMALL_NUMBER)
    {
        TestTrue(TEXT("padding 2.0 places the camera strictly further out than padding 1.15"),
            Distances[1] > Distances[0]);
        TestTrue(TEXT("the measured distance ratio is 2.0/1.15 within 1e-3"),
            FMath::IsNearlyEqual(static_cast<float>(Distances[1] / Distances[0]),
                ExpectedRatio, 1e-3f));
    }
    else
    {
        AddInfo(TEXT("Skipped the pose comparison: the shots did not report an applied camera aim."));
    }

    for (const FString& Path : Paths)
    {
        PwFitDeleteFile(Path);
    }
    return true;
}

// ============================================================================
// camera.frame_actor — an explicit `distance` must skip the bounds fit.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwFrameActorExplicitDistanceSkipsTheFitTest,
    "PinWright.camera.frame_actor.ExplicitDistanceSkipsTheFit",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwFrameActorExplicitDistanceSkipsTheFitTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightCameraFrame;

    const float FitDistance = ResolveCameraDistance(false, 0.0f, false, 0.0f,
        GPwFitRadius, GPwFitFov, GDefaultFitPadding);

    // PRECONDITION, asserted rather than assumed: the un-overridden fit must NOT already be 500,
    // or "the camera is at 500" proves nothing about the override.
    TestFalse(TEXT("the un-overridden fit distance is not already 500"),
        FMath::IsNearlyEqual(FitDistance, GPwFitExplicitDistance, 1.0f));

    const float Overridden = ResolveCameraDistance(true, GPwFitExplicitDistance, false, 0.0f,
        GPwFitRadius, GPwFitFov, GDefaultFitPadding);
    TestTrue(TEXT("an explicit distance is used verbatim"),
        FMath::IsNearlyEqual(Overridden, GPwFitExplicitDistance, 1e-3f));

    // ...and it must be independent of the bounds, which is what "skips the fit" means. A ten-fold
    // radius must not move it.
    TestTrue(TEXT("an explicit distance ignores the bounds entirely"),
        FMath::IsNearlyEqual(
            ResolveCameraDistance(true, GPwFitExplicitDistance, false, 0.0f,
                GPwFitRadius * 10.0f, GPwFitFov, GDefaultFitPadding),
            GPwFitExplicitDistance, 1e-3f));

    // A non-positive override is ABSENT, not a request to put the camera inside the subject --
    // the rule camera.orbit_shots' `radius` already shipped with, now shared.
    TestTrue(TEXT("a zero override falls back to the fit"),
        FMath::IsNearlyEqual(
            ResolveCameraDistance(true, 0.0f, false, 0.0f, GPwFitRadius, GPwFitFov,
                GDefaultFitPadding), FitDistance, 1e-3f));
    TestTrue(TEXT("a negative override falls back to the fit"),
        FMath::IsNearlyEqual(
            ResolveCameraDistance(true, -5.0f, false, 0.0f, GPwFitRadius, GPwFitFov,
                GDefaultFitPadding), FitDistance, 1e-3f));

    // ---- live half: the parameter must be WIRED ----
    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("Skipped the live half: no editor world. The headless assertions above ran."));
        return true;
    }

    FScopedEditorWorldActorGuard ActorGuard;
    const FString Label = FString::Printf(TEXT("PW_FitDistance_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    AStaticMeshActor* Actor = PwFitSpawnResolvableCube(World, Label);
    if (!Actor)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("engine-cube-unavailable"),
            TEXT("Skipped the live half: engine cube unavailable. Headless assertions ran."));
        return true;
    }

    FVector Center = FVector::ZeroVector;
    FVector Extent = FVector::ZeroVector;
    Actor->GetActorBounds(false, Center, Extent);

    TArray<FString> Paths;
    double FitMeasured = 0.0;
    double OverriddenMeasured = 0.0;
    bool bFitRead = false;
    bool bOverriddenRead = false;

    for (int32 Pass = 0; Pass < 2; ++Pass)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("actorName"), Actor->GetActorLabel());
        if (Pass == 1)
        {
            Payload->SetNumberField(TEXT("distance"), GPwFitExplicitDistance);
            // Supplied alongside the override on purpose: the response must SAY that padding did
            // nothing rather than dropping it, which is the same rule this verb's sibling applies
            // to `elevation` under distribution:'sphere'.
            Payload->SetNumberField(TEXT("padding"), 2.0);
        }

        FTestResponseCapture Capture;
        InvokeHandlerWithCapture(TEXT("camera.frame_actor"), Payload, Capture);
        if (!Capture.bSuccess)
        {
            TestTrue(TEXT("live capture failure is typed"),
                PwFitIsTypedCaptureFailure(Capture.ErrorCode));
            AddInfo(TEXT("Skipped the live half: viewport capture unavailable."));
            break;
        }

        if (Pass == 1)
        {
            bool bPaddingIgnored = false;
            TestTrue(TEXT("an inert `padding` is named in the response"),
                Capture.Result.IsValid() &&
                Capture.Result->TryGetBoolField(TEXT("paddingIgnored"), bPaddingIgnored) &&
                bPaddingIgnored);
            FString Warning;
            TestTrue(TEXT("the warning names `distance` as what skipped the fit"),
                Capture.Result.IsValid() &&
                Capture.Result->TryGetStringField(TEXT("paddingWarning"), Warning) &&
                Warning.Contains(TEXT("distance")));
        }
        else
        {
            TestFalse(TEXT("a fitted shot does not report an ignored padding"),
                Capture.Result.IsValid() && Capture.Result->HasField(TEXT("paddingIgnored")));
        }

        FString Path;
        FVector CameraLocation = FVector::ZeroVector;
        const bool bRead = PwFitSingleShotCamera(Capture.Result, CameraLocation, Path);
        Paths.AddUnique(Path);
        if (Pass == 0)
        {
            FitMeasured = FVector::Dist(CameraLocation, Center);
            bFitRead = bRead;
        }
        else
        {
            OverriddenMeasured = FVector::Dist(CameraLocation, Center);
            bOverriddenRead = bRead;
        }
    }

    if (bFitRead && bOverriddenRead)
    {
        // PRECONDITION on the real fixture, not just the synthetic radius: the fit must differ
        // from 500 for the override assertion to mean anything.
        TestFalse(TEXT("this fixture's own fit distance is not already 500"),
            FMath::IsNearlyEqual(static_cast<float>(FitMeasured), GPwFitExplicitDistance, 1.0f));
        TestTrue(TEXT("`distance: 500` places the camera at 500 regardless of bounds"),
            FMath::IsNearlyEqual(static_cast<float>(OverriddenMeasured),
                GPwFitExplicitDistance, 1.0f));
    }
    else
    {
        AddInfo(TEXT("Skipped the pose comparison: the shots did not report an applied camera aim."));
    }

    for (const FString& Path : Paths)
    {
        PwFitDeleteFile(Path);
    }
    return true;
}

// ============================================================================
// The instance, as a spec walk: both halves of ComputeFitDistance on both verbs.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwCameraVerbsExposeBothFitInputsTest,
    "PinWright.camera.parameter_parity.CameraVerbsExposeBothFitInputs",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwCameraVerbsExposeBothFitInputsTest::RunTest(const FString& Parameters)
{
    // ComputeFitDistance(Radius, Fov, Padding) has two caller-reachable inputs: a margin and a
    // distance override that skips the fit. Every verb that calls it must expose BOTH or neither,
    // and all three of these call it.
    PwFitRequireParam(*this, TEXT("camera.frame_actor"), TEXT("padding"));
    PwFitRequireParam(*this, TEXT("camera.frame_actor"), TEXT("distance"));

    PwFitRequireParam(*this, TEXT("camera.orbit_shots"), TEXT("padding"));
    PwFitRequireParam(*this, TEXT("camera.orbit_shots"), TEXT("radius"));

    // The parity REFERENCE, asserted so it cannot quietly stop being one: camera.animation_shots
    // already exposed both inputs before this fix, and it is the evidence that the union is a
    // shipped, workable surface rather than a new invention.
    PwFitRequireParam(*this, TEXT("camera.animation_shots"), TEXT("padding"));
    PwFitRequireParam(*this, TEXT("camera.animation_shots"), TEXT("radius"));

    return true;
}

// ============================================================================
// Both camera verbs declare the scoped preview-scene rig.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwCameraVerbsDeclarePreviewSceneRigTest,
    "PinWright.camera.parameter_parity.BothCameraVerbsDeclareThePreviewSceneRig",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwCameraVerbsDeclarePreviewSceneRigTest::RunTest(const FString& Parameters)
{
    // Read off the REGISTERED spec list, which is what the dispatcher's unknown-param gate reads.
    // A source grep would pass on a parameter that never reached the registry and is therefore
    // rejected on the wire.
    const FParamSpec* FrameSpec =
        PwFitRequireParam(*this, TEXT("camera.frame_actor"), TEXT("previewScene"));
    if (FrameSpec)
    {
        TestEqual(TEXT("camera.frame_actor's `previewScene` is an object"),
            FrameSpec->Type, FString(TEXT("object")));
        TestFalse(TEXT("camera.frame_actor's `previewScene` is optional"), FrameSpec->bRequired);
    }

    const FParamSpec* OrbitSpec =
        PwFitRequireParam(*this, TEXT("camera.orbit_shots"), TEXT("previewScene"));
    if (OrbitSpec)
    {
        TestEqual(TEXT("camera.orbit_shots' `previewScene` is an object"),
            OrbitSpec->Type, FString(TEXT("object")));
        TestFalse(TEXT("camera.orbit_shots' `previewScene` is optional"), OrbitSpec->bRequired);
    }

    return true;
}

// ============================================================================
// A rig against the Level Editor viewport is refused, not silently dropped.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwOrbitPreviewSceneRigOnLevelTargetRefusedTest,
    "PinWright.camera.orbit_shots.PreviewSceneRigOnALevelTargetIsRefused",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwOrbitPreviewSceneRigOnLevelTargetRefusedTest::RunTest(const FString& Parameters)
{
    // A bare `point` target, so this needs no world, no actor and no viewport: the refusal is
    // decidable from the payload and is answered above the viewport acquisition, which is what
    // makes it assertable with no GPU.
    TSharedPtr<FJsonObject> Point = MakeShared<FJsonObject>();
    Point->SetNumberField(TEXT("x"), 0.0);
    Point->SetNumberField(TEXT("y"), 0.0);
    Point->SetNumberField(TEXT("z"), 0.0);

    TSharedPtr<FJsonObject> Key = MakeShared<FJsonObject>();
    Key->SetNumberField(TEXT("intensity"), 4.0);
    TSharedPtr<FJsonObject> Rig = MakeShared<FJsonObject>();
    Rig->SetObjectField(TEXT("key"), Key);

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetObjectField(TEXT("point"), Point);
    Payload->SetNumberField(TEXT("radius"), 100.0);
    Payload->SetObjectField(TEXT("previewScene"), Rig);

    FTestResponseCapture Capture;
    TestTrue(TEXT("camera.orbit_shots is registered"),
        InvokeHandlerWithCapture(TEXT("camera.orbit_shots"), Payload, Capture));

    TestFalse(TEXT("a rig against the level viewport is refused, not accepted"), Capture.bSuccess);
    // The CODE, not merely "it failed": a bad payload fails too, and the two must be
    // distinguishable. No new error code was minted for this -- UNSUPPORTED_ASSET_EDITOR already
    // says "that viewport has no advanced preview scene".
    TestEqual(TEXT("the refusal is UNSUPPORTED_ASSET_EDITOR"), Capture.ErrorCode,
        FString(TEXT("UNSUPPORTED_ASSET_EDITOR")));
    TestTrue(TEXT("the message names the parameter that cannot be honoured"),
        Capture.Message.Contains(TEXT("previewScene")));
    // ...and it must steer, not just refuse: the caller has to be told what WOULD work.
    TestTrue(TEXT("the message names the asset-subject route that would work"),
        Capture.Message.Contains(TEXT("subject")));

    // The contrast case: the same target with NO rig must not be refused for this reason. Without
    // it the assertion above would pass on a verb that had simply stopped accepting bare points.
    TSharedPtr<FJsonObject> NoRig = MakeShared<FJsonObject>();
    NoRig->SetObjectField(TEXT("point"), Point);
    NoRig->SetNumberField(TEXT("radius"), 100.0);

    FTestResponseCapture NoRigCapture;
    InvokeHandlerWithCapture(TEXT("camera.orbit_shots"), NoRig, NoRigCapture);
    TestNotEqual(TEXT("the same target with no rig is not refused as an unsupported editor"),
        NoRigCapture.ErrorCode, FString(TEXT("UNSUPPORTED_ASSET_EDITOR")));

    return true;
}
