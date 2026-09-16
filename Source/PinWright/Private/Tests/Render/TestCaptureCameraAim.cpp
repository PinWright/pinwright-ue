// Copyright (c) 2026 Alexander Penkin. MIT License.

// Tests for where a capture's camera ACTUALLY ends up, and for whether the subject is in the frame
// at all.
//
// WHAT THIS DEFENDS. Every perspective capture through an asset-editor preview viewport was
// mis-aimed. Those viewports run with bUsingOrbitCamera true, and in orbit mode
// FEditorViewportClient::CalcViewRotationMatrix ignores the camera rotation entirely and builds
// the view from FViewportCameraTransform::ComputeOrbitMatrix instead (UE 5.8
// Editor/UnrealEd/Private/EditorViewportClient.cpp:7392-7400). SetViewLocation and SetViewRotation
// still succeed; they just do not aim anything. Measured consequences, worked out from :395-405
// and asserted below:
//
//   * the rendered yaw is 90 - requested yaw -- a MIRROR, not an offset, so `yaw:0` renders as
//     +90 and `yaw:-90` renders as 180;
//   * the requested LOCATION's direction is discarded outright. The eye is placed on a sphere
//     around the orbit pivot at radius |requested location - pivot|, so two locations pointing in
//     completely different directions produce the SAME picture as long as they are the same
//     distance from a pivot the caller never named.
//
// WHY THAT IS WORSE THAN A WRONG PICTURE. It returns a perfectly valid PNG and reports the
// REQUESTED pose back as the pose it used, so nothing in the response contradicts it. Opposed-
// camera pairs are the primary way geometry defects get diagnosed in a review, and a pair whose
// pivot is not the subject's centre can leave the subject out of both frames -- two captures came
// back byte-identical pure backdrop with blank=false and litPixelFraction=1, because `blank` only
// catches BLACK frames and a lit backdrop is not black.
//
// WHY THE ASSERTIONS ARE ON A VIEW DIRECTION AND NOT ON A RETURN CODE. A capture that returns a
// valid PNG proves nothing here; the whole defect is that it returns a valid PNG of the wrong
// side. So every test below asserts the pose the RENDERER resolves to, and the live one takes that
// pose from the engine's own CalcViewRotationMatrix rather than from the helper under test.
//
// WHY THE CORE TESTS NEED NO VIEWPORT. PinWrightRenderCapture::ResolveEffectiveViewPose is a pure
// function over a bare FViewportCameraTransform, so the aiming contract is assertable with no
// viewport, no world and no GPU -- nothing that can make a test take a conditional-skip path and
// report success without running its assertions (board ticket B-test-skips-assertions-silently).
#include "Misc/AutomationTest.h"
#include "Handlers/Render/PreviewViewportCaptureUtils.h"

#include "Dom/JsonObject.h"
#include "Editor.h"
#include "EditorViewportClient.h"
#include "Framework/Application/SlateApplication.h"
#include "Tests/TestSkipReporting.h"

namespace
{
    // Prefixed for the same reason as every other helper in Tests/Render: anonymous namespaces in
    // one Unity translation unit merge, so a bare name a sibling also uses is a latent ODR clash.
    constexpr double AimTestAngleToleranceDegrees = 0.05;
    constexpr double AimTestDistanceTolerance = 0.05;

    // Angle between two directions, in degrees. The comparison every test here makes: a rotator
    // comparison would report a spurious difference for the equivalent (pitch, yaw, roll) triples
    // a matrix round trip produces, and the thing under test is where the camera LOOKS.
    double AimTestAngleBetween(const FVector& A, const FVector& B)
    {
        const FVector UnitA = A.GetSafeNormal();
        const FVector UnitB = B.GetSafeNormal();
        if (UnitA.IsNearlyZero() || UnitB.IsNearlyZero())
        {
            return 180.0;
        }
        return FMath::RadiansToDegrees(
            FMath::Acos(FMath::Clamp(FVector::DotProduct(UnitA, UnitB), -1.0, 1.0)));
    }

    // An orbit-mode camera transform: pivot at the origin, and a caller-supplied pose that orbit
    // mode is about to ignore.
    FViewportCameraTransform AimTestOrbitTransform(const FVector& Location, const FRotator& Rotation)
    {
        FViewportCameraTransform Transform;
        Transform.SetLookAt(FVector::ZeroVector);
        Transform.SetLocation(Location);
        Transform.SetRotation(Rotation);
        return Transform;
    }

    // A perspective capture request with a caller-chosen pose.
    PinWrightRenderCapture::FViewportCaptureRequest AimTestPerspectiveRequest(
        const FVector& Location, const FRotator& Rotation)
    {
        PinWrightRenderCapture::FViewportCaptureRequest Request;
        Request.ProjectionMode = TEXT("perspective");
        Request.Width = 1024;
        Request.Height = 1024;
        Request.Location = Location;
        Request.Rotation = Rotation;
        Request.bLocationProvided = true;
        Request.bRotationProvided = true;
        Request.Fov = 50.0f;
        return Request;
    }

    // The camera pose the ENGINE resolves for this client, reconstructed the way CalcSceneView
    // assembles it -- FTranslationMatrix(-ViewOrigin) * CalcViewRotationMatrix(ViewRotation), with
    // ViewOrigin the client's view location (EditorViewportClient.cpp:1113, :1141, :1253) --
    // inverted. Deliberately routed through the engine's own virtual rather than through
    // MeasureEffectiveViewPose, so the live test has an oracle independent of the code it checks.
    void AimTestEngineCameraPose(const FEditorViewportClient& Client, FVector& OutEye,
        FVector& OutForward)
    {
        const FMatrix WorldToView =
            FTranslationMatrix(-Client.GetViewLocation()) *
            Client.CalcViewRotationMatrix(Client.GetViewRotation());
        const FMatrix ViewToWorld = WorldToView.Inverse();
        OutEye = ViewToWorld.GetOrigin();
        OutForward = ViewToWorld.GetUnitAxis(EAxis::X);
    }
}

// ============================================================================
// 1. The defect itself, on a bare camera transform: orbit mode throws the requested pose away.
//
// This is the counterfactual that keeps every later test from being vacuous. If it ever stops
// failing to reproduce the requested pose, orbit mode has changed in the engine and the
// suppression below is no longer load-bearing.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCaptureCameraAimOrbitDiscardsRequestedPoseTest,
    "PinWright.render.camera_aim.OrbitModeDiscardsTheRequestedPose",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCaptureCameraAimOrbitDiscardsRequestedPoseTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightRenderCapture;

    // The exact pose the field report was taken at: yaw -90 from (32, 800, 78), which a free
    // camera renders as a look down -Y and an orbit camera renders as a look down -X.
    const FVector RequestedLocation(32.0, 800.0, 78.0);
    const FRotator RequestedRotation(0.0, -90.0, 0.0);
    const FViewportCameraTransform Transform =
        AimTestOrbitTransform(RequestedLocation, RequestedRotation);

    const FEffectiveViewPose Orbiting = ResolveEffectiveViewPose(/*bOrbitCameraInForce=*/true, Transform);
    TestTrue(TEXT("the pose is reported as orbit-derived"), Orbiting.bFromOrbitCamera);

    // Rendered yaw is 90 - requested yaw: -90 renders as 180, i.e. a look down -X.
    TestTrue(TEXT("orbit renders yaw -90 as a look down -X, not the requested -Y"),
        AimTestAngleBetween(Orbiting.Rotation.Vector(), FVector(-1.0, 0.0, 0.0)) <
            AimTestAngleToleranceDegrees);
    TestTrue(TEXT("and that is NOT the requested direction"),
        AimTestAngleBetween(Orbiting.Rotation.Vector(), RequestedRotation.Vector()) > 80.0);

    // The eye sits on the pivot sphere at the requested location's DISTANCE, on an axis the
    // requested location had nothing to do with.
    const double PivotRadius = RequestedLocation.Size();
    TestTrue(TEXT("the orbit eye keeps the requested distance from the pivot"),
        FMath::Abs(Orbiting.Location.Size() - PivotRadius) < AimTestDistanceTolerance);
    TestTrue(TEXT("but not the requested location"),
        FVector::Dist(Orbiting.Location, RequestedLocation) > 100.0);

    // The discarded-direction property, stated as a property rather than as a magic number: a
    // completely different location at the same pivot distance renders the identical view. This is
    // the half that makes a badly aimed capture reproducible and therefore convincing -- two calls
    // from opposite sides of a mesh returned byte-identical PNGs.
    const FViewportCameraTransform Elsewhere =
        AimTestOrbitTransform(FVector(0.0, 0.0, PivotRadius), RequestedRotation);
    const FEffectiveViewPose ElsewherePose =
        ResolveEffectiveViewPose(/*bOrbitCameraInForce=*/true, Elsewhere);
    TestTrue(TEXT("a location pointing somewhere else entirely renders the same eye"),
        FVector::Dist(ElsewherePose.Location, Orbiting.Location) < AimTestDistanceTolerance);
    TestTrue(TEXT("and the same direction"),
        AimTestAngleBetween(ElsewherePose.Rotation.Vector(), Orbiting.Rotation.Vector()) <
            AimTestAngleToleranceDegrees);

    // The mirror, spelled out at a second yaw so a constant-offset "fix" cannot pass: yaw 0 must
    // render as +90 while yaw -90 renders as 180. No single added constant maps both.
    const FEffectiveViewPose YawZero = ResolveEffectiveViewPose(/*bOrbitCameraInForce=*/true,
        AimTestOrbitTransform(RequestedLocation, FRotator(0.0, 0.0, 0.0)));
    TestTrue(TEXT("orbit renders yaw 0 as a look down +Y"),
        AimTestAngleBetween(YawZero.Rotation.Vector(), FVector(0.0, 1.0, 0.0)) <
            AimTestAngleToleranceDegrees);
    return true;
}

// ============================================================================
// 2. With orbit off, the pose IS the pose. The other half of the contract the fix relies on.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCaptureCameraAimFreeCameraRendersRequestTest,
    "PinWright.render.camera_aim.FreeCameraRendersTheRequestedPose",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCaptureCameraAimFreeCameraRendersRequestTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightRenderCapture;

    const FVector RequestedLocation(32.0, 800.0, 78.0);
    const FRotator RequestedRotation(-11.0, -90.0, 0.0);
    const FEffectiveViewPose Free = ResolveEffectiveViewPose(/*bOrbitCameraInForce=*/false,
        AimTestOrbitTransform(RequestedLocation, RequestedRotation));

    TestFalse(TEXT("the pose is not orbit-derived"), Free.bFromOrbitCamera);
    TestTrue(TEXT("a free camera is where it was put"),
        FVector::Dist(Free.Location, RequestedLocation) < AimTestDistanceTolerance);
    TestTrue(TEXT("and looks where it was aimed"),
        AimTestAngleBetween(Free.Rotation.Vector(), RequestedRotation.Vector()) <
            AimTestAngleToleranceDegrees);
    return true;
}

// ============================================================================
// 3. Live: ApplyCaptureCamera aims a PERSPECTIVE capture through an orbiting viewport.
//
// The regression test proper. Delete the ToggleOrbitCamera(false) from ApplyCaptureCamera's
// perspective branch and this fails on both assertions -- the eye lands on the pivot sphere and
// the forward direction comes back 90 degrees mirrored -- because the oracle is the engine's own
// CalcViewRotationMatrix, not anything this change added.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCaptureCameraAimPerspectiveAimsWhereAskedTest,
    "PinWright.render.camera_aim.PerspectiveCaptureAimsWhereAsked",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCaptureCameraAimPerspectiveAimsWhereAskedTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightRenderCapture;

    if (!GEditor || !FSlateApplication::IsInitialized())
    {
        // Constructing an FEditorViewportClient registers with GEditor and subscribes to Slate's
        // DPI delegate, neither of which exists in a commandlet host. Tests 1, 2 and 5 carry the
        // aiming contract with no environment at all, so this is a narrowing of coverage rather
        // than a silent pass.
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-or-slate-host"),
            TEXT("No GEditor / Slate: skipping the live ApplyCaptureCamera assertions."));
        return true;
    }

    // A standalone client rather than the user's Level Editor viewport: it is the same class the
    // capture path drives, and nothing here can disturb a viewport somebody is working in.
    FEditorViewportClient Client(nullptr);
    Client.SetViewportType(LVT_Perspective);
    Client.SetLookAtLocation(FVector::ZeroVector, /*bRecalculateView=*/false);
    Client.SetViewLocation(FVector(0.0, -600.0, 200.0));
    Client.SetViewRotation(FRotator(-18.0, 90.0, 0.0));
    Client.ToggleOrbitCamera(true);
    // Without this the whole test could pass on a client that was never orbiting.
    if (!TestTrue(TEXT("the fixture viewport really is in orbit mode"), Client.bUsingOrbitCamera))
    {
        return true;
    }

    const FVector RequestedLocation(32.0, 800.0, 78.0);
    const FRotator RequestedRotation(-6.0, -90.0, 0.0);
    const FViewportCaptureRequest Request =
        AimTestPerspectiveRequest(RequestedLocation, RequestedRotation);
    ApplyCaptureCamera(Client, nullptr, Request);

    TestFalse(TEXT("a perspective capture takes the viewport out of orbit mode"),
        Client.bUsingOrbitCamera);

    FVector EngineEye = FVector::ZeroVector;
    FVector EngineForward = FVector::ZeroVector;
    AimTestEngineCameraPose(Client, EngineEye, EngineForward);

    // THE assertion: the direction the renderer will draw along, not the value stored on the
    // client and not the request echoed back.
    TestTrue(TEXT("the rendered view direction is the requested one"),
        AimTestAngleBetween(EngineForward, RequestedRotation.Vector()) < AimTestAngleToleranceDegrees);
    TestTrue(TEXT("and the eye is at the requested location, direction included"),
        FVector::Dist(EngineEye, RequestedLocation) < AimTestDistanceTolerance);

    // The reported pose agrees with the engine's, so a response built from it is trustworthy.
    const FEffectiveViewPose Measured = MeasureEffectiveViewPose(Client);
    TestTrue(TEXT("MeasureEffectiveViewPose agrees with the engine's own view matrix"),
        AimTestAngleBetween(Measured.Rotation.Vector(), EngineForward) < AimTestAngleToleranceDegrees);
    TestTrue(TEXT("on the eye as well"),
        FVector::Dist(Measured.Location, EngineEye) < AimTestDistanceTolerance);

    return true;
}

// ============================================================================
// 4. Live: the ORTHOGRAPHIC path is untouched.
//
// Orthographic aiming was already correct -- an editor builds that view matrix from the
// ELevelViewportType and never consults the orbit transform -- so the fix must not reach it. Two
// separate claims: the resolved viewport type is still the one the rotation asks for, and the
// client's orbit flag is left exactly as it was found (an orthographic capture must not silently
// take a user's viewport out of orbit mode).
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCaptureCameraAimOrthographicUntouchedTest,
    "PinWright.render.camera_aim.OrthographicPathIsUntouched",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCaptureCameraAimOrthographicUntouchedTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightRenderCapture;

    // The type resolution is pure and runs everywhere: yaw 90 is the +Y-looking profile.
    FOrthographicViewResolution Resolution;
    TestTrue(TEXT("yaw 90 resolves to a cardinal orthographic view"),
        ResolveOrthographicView(FRotator(0.0, 90.0, 0.0), Resolution));
    TestTrue(TEXT("and it looks down +Y"),
        AimTestAngleBetween(Resolution.EffectiveRotation.Vector(), FVector(0.0, 1.0, 0.0)) <
            AimTestAngleToleranceDegrees);

    if (!GEditor || !FSlateApplication::IsInitialized())
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-or-slate-host"),
            TEXT("No GEditor / Slate: skipping the live orthographic assertions."));
        return true;
    }

    FEditorViewportClient Client(nullptr);
    Client.SetViewportType(LVT_Perspective);
    Client.SetLookAtLocation(FVector::ZeroVector, /*bRecalculateView=*/false);
    Client.SetViewLocation(FVector(0.0, -600.0, 200.0));
    Client.SetViewRotation(FRotator(-18.0, 90.0, 0.0));
    Client.ToggleOrbitCamera(true);
    if (!TestTrue(TEXT("the fixture viewport really is in orbit mode"), Client.bUsingOrbitCamera))
    {
        return true;
    }

    FViewportCaptureRequest Request = AimTestPerspectiveRequest(
        FVector(0.0, -2000.0, 0.0), FRotator(0.0, 90.0, 0.0));
    Request.ProjectionMode = TEXT("orthographic");
    Request.OrthoWidth = 420.0f;
    ApplyCaptureCamera(Client, nullptr, Request);

    TestTrue(TEXT("an orthographic capture leaves the orbit flag exactly as it found it"),
        Client.bUsingOrbitCamera);
    TestEqual(TEXT("and resolves the rotation to the +Y-looking viewport type"),
        static_cast<int32>(Client.GetViewportType()), static_cast<int32>(Resolution.ViewportType));

    return true;
}

// ============================================================================
// 5. A frame that contains none of the subject is flagged.
//
// `blank` cannot do this: it catches BLACK frames, and the failure it missed was a brightly lit
// backdrop reported as blank:false with litPixelFraction 1. The test is conservative in the
// direction the production code is -- a false "not framed" verdict would send a caller chasing a
// working capture -- so the framed cases below must all come back true.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCaptureCameraAimBoundsFramingTest,
    "PinWright.render.framing.BoundsOutOfFrameAreFlagged",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCaptureCameraAimBoundsFramingTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightRenderCapture;

    const FVector Centre = FVector::ZeroVector;
    constexpr double Radius = 50.0;
    const FVector Eye(-500.0, 0.0, 0.0);
    const FViewportCaptureRequest Perspective = AimTestPerspectiveRequest(Eye, FRotator::ZeroRotator);

    // Looking straight at it.
    {
        const FBoundsFramingCheck Check =
            EvaluateBoundsFraming(Perspective, Eye, FRotator(0.0, 0.0, 0.0), Centre, Radius);
        TestTrue(TEXT("a subject on the view axis is evaluated"), Check.bEvaluated);
        TestTrue(TEXT("and is in frame"), Check.bBoundsInFrame);
        TestFalse(TEXT("and is not behind the camera"), Check.bBehindCamera);
    }

    // The defect: same camera position, aimed 90 degrees off -- which is exactly what orbit mode
    // did to a requested yaw.
    {
        const FBoundsFramingCheck Check =
            EvaluateBoundsFraming(Perspective, Eye, FRotator(0.0, 90.0, 0.0), Centre, Radius);
        TestFalse(TEXT("a subject 90 degrees off the view axis is in frame"),
            Check.bBoundsInFrame);
        TestFalse(TEXT("it is beside the camera, not behind it"), Check.bBehindCamera);
        TestEqual(TEXT("the perspective verdict is reported in degrees"),
            Check.Units, FString(TEXT("degrees")));
    }

    // Aimed away from it entirely.
    {
        const FBoundsFramingCheck Check =
            EvaluateBoundsFraming(Perspective, Eye, FRotator(0.0, 180.0, 0.0), Centre, Radius);
        TestTrue(TEXT("a subject behind the camera is flagged as behind"), Check.bBehindCamera);
        TestFalse(TEXT("and out of frame"), Check.bBoundsInFrame);
    }

    // A camera inside the subject cannot be judged by a sphere test, and the conservative answer
    // is the permissive one.
    {
        const FBoundsFramingCheck Check = EvaluateBoundsFraming(
            Perspective, FVector(10.0, 0.0, 0.0), FRotator(0.0, 180.0, 0.0), Centre, Radius);
        TestTrue(TEXT("a camera inside the bounds is not reported as out of frame"),
            Check.bBoundsInFrame);
    }

    // No usable bounds is reported as such rather than guessed at.
    {
        const FBoundsFramingCheck Check =
            EvaluateBoundsFraming(Perspective, Eye, FRotator(0.0, 90.0, 0.0), Centre, 0.0);
        TestFalse(TEXT("a zero-radius subject is not evaluated"), Check.bEvaluated);
        TestTrue(TEXT("and defaults to the permissive verdict"), Check.bBoundsInFrame);
    }

    // Orthographic: the frame is a box of orthoWidth centimetres, so being in frame is a distance
    // question, not an angle one.
    {
        FViewportCaptureRequest Ortho = Perspective;
        Ortho.ProjectionMode = TEXT("orthographic");
        Ortho.OrthoWidth = 200.0f;

        const FBoundsFramingCheck OnAxis =
            EvaluateBoundsFraming(Ortho, Eye, FRotator(0.0, 0.0, 0.0), Centre, Radius);
        TestTrue(TEXT("an orthographic frame centred on the subject contains it"),
            OnAxis.bBoundsInFrame);
        TestEqual(TEXT("the orthographic verdict is reported in centimetres"),
            OnAxis.Units, FString(TEXT("centimetres")));

        const FBoundsFramingCheck OffAxis = EvaluateBoundsFraming(
            Ortho, Eye, FRotator(0.0, 0.0, 0.0), FVector(0.0, 5000.0, 0.0), Radius);
        TestFalse(TEXT("a subject 5000 cm to the side of a 200 cm frame is not in it"),
            OffAxis.bBoundsInFrame);
    }

    // The response block says so, and says it only when there is something to say.
    {
        const FBoundsFramingCheck Check =
            EvaluateBoundsFraming(Perspective, Eye, FRotator(0.0, 90.0, 0.0), Centre, Radius);
        const TSharedPtr<FJsonObject> Framing = MakeBoundsFramingObject(Check);
        if (TestTrue(TEXT("a framing block is built"), Framing.IsValid()))
        {
            bool bInFrame = true;
            TestTrue(TEXT("it reports the verdict"),
                Framing->TryGetBoolField(TEXT("boundsInFrame"), bInFrame));
            TestFalse(TEXT("as false"), bInFrame);
            TestTrue(TEXT("and carries a warning naming the consequence"),
                Framing->HasField(TEXT("framingWarning")));
        }

        const FBoundsFramingCheck Framed =
            EvaluateBoundsFraming(Perspective, Eye, FRotator(0.0, 0.0, 0.0), Centre, Radius);
        const TSharedPtr<FJsonObject> FramedBlock = MakeBoundsFramingObject(Framed);
        if (TestTrue(TEXT("a framing block is built for the good case too"), FramedBlock.IsValid()))
        {
            TestFalse(TEXT("a framed capture carries no warning"),
                FramedBlock->HasField(TEXT("framingWarning")));
        }
    }
    return true;
}
