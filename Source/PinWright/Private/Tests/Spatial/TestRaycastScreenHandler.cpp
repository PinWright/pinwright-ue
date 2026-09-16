// Copyright (c) 2026 Alexander Penkin. MIT License.

// Unit tests for spatial.raycast_screen - map a capture pixel back to a world ray + hit.
//
// The handler deprojects the pixel by rebuilding the capture's view against the active
// Level Editor viewport (PinWrightViewProjection). A headless automation host may have no
// active viewport; then the deproject fails with a typed NO_ACTIVE_LEVEL_VIEWPORT /
// EDITOR_NOT_AVAILABLE / NO_EDITOR_WORLD / VIEW_BUILD_FAILED code. The pose-dependent tests
// detect that and AddInfo+skip rather than false-negative. Pure param-validation cases
// (out-of-range pixel, unknown arg) run before any deproject and need no viewport.

#include "Misc/AutomationTest.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/HandlerRegistration.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Tests/TestUtils.h"
#include "Tests/TestSkipReporting.h"
#include "Tests/Infra/DispatcherTestHelpers.h"
#include "Utils/ActorUtils.h"

#include "Dispatch/RpcDispatcher.h"
#include "Editor.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Misc/Guid.h"
#include "Misc/ScopeExit.h"

namespace
{
    TSharedPtr<FJsonObject> RSVec(double X, double Y, double Z)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetNumberField(TEXT("x"), X);
        Obj->SetNumberField(TEXT("y"), Y);
        Obj->SetNumberField(TEXT("z"), Z);
        return Obj;
    }

    TSharedPtr<FJsonObject> RSRot(double Pitch, double Yaw, double Roll)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetNumberField(TEXT("pitch"), Pitch);
        Obj->SetNumberField(TEXT("yaw"), Yaw);
        Obj->SetNumberField(TEXT("roll"), Roll);
        return Obj;
    }

    UWorld* RSEditorWorld()
    {
        return GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    }

    // The editor world doesn't tick physics on its own, so a just-spawned body isn't in the
    // scene-query acceleration structure until a world tick flushes the registration. Tick a
    // couple of frames so an immediate trace can see the new floor. Editor worlds don't
    // simulate, so the floor doesn't move. (Same pattern as TestRaycastHandlers.cpp.)
    void RSFlushEditorPhysics(UWorld* World)
    {
        if (!World || World->bInTick)
        {
            return;
        }
        for (int32 Iteration = 0; Iteration < 2; ++Iteration)
        {
            World->Tick(LEVELTICK_All, 1.0f / 60.0f);
        }
    }

    // The deproject-failure codes that mean "this host has no usable viewport to rebuild the
    // view" - a skip condition, not a real failure.
    bool RSIsNoViewportCode(const FString& Code)
    {
        return Code == TEXT("NO_ACTIVE_LEVEL_VIEWPORT")
            || Code == TEXT("EDITOR_NOT_AVAILABLE")
            || Code == TEXT("NO_EDITOR_WORLD")
            || Code == TEXT("VIEW_BUILD_FAILED");
    }

    // Reads {x,y,z} from a nested object field into an FVector; returns false if absent.
    bool RSReadVector(const TSharedPtr<FJsonObject>& Owner, const FString& Field, FVector& Out)
    {
        const TSharedPtr<FJsonObject>* Obj = nullptr;
        if (!Owner.IsValid() || !Owner->TryGetObjectField(Field, Obj) || !Obj || !(*Obj).IsValid())
        {
            return false;
        }
        double X = 0.0, Y = 0.0, Z = 0.0;
        (*Obj)->TryGetNumberField(TEXT("x"), X);
        (*Obj)->TryGetNumberField(TEXT("y"), Y);
        (*Obj)->TryGetNumberField(TEXT("z"), Z);
        Out = FVector(X, Y, Z);
        return true;
    }
}

// (a) Matrix consistency, no target needed: a camera above Z=0 looking straight down maps
// the CENTER pixel to a ray pointing along the camera forward (straight down, -Z).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRaycastScreenCenterRayForwardTest,
    "PinWright.spatial.raycast_screen.CenterRayIsForward",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FRaycastScreenCenterRayForwardTest::RunTest(const FString& Parameters)
{
    // Camera at (0,0,1000) looking straight down (pitch -90 => forward (0,0,-1)), 1024x1024.
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetObjectField(TEXT("location"), RSVec(0.0, 0.0, 1000.0));
    Payload->SetObjectField(TEXT("rotation"), RSRot(-90.0, 0.0, 0.0));
    Payload->SetNumberField(TEXT("x"), 512.0);
    Payload->SetNumberField(TEXT("y"), 512.0);
    Payload->SetNumberField(TEXT("width"), 1024.0);
    Payload->SetNumberField(TEXT("height"), 1024.0);

    FTestResponseCapture Capture;
    TestTrue(TEXT("spatial.raycast_screen handler registered"),
        InvokeHandlerWithCapture(TEXT("spatial.raycast_screen"), Payload, Capture));

    if (!Capture.bSuccess)
    {
        if (RSIsNoViewportCode(Capture.ErrorCode))
        {
            PinWrightTestSkip::SkipAssertions(*this, TEXT("no-level-viewport"),
                FString::Printf(
                    TEXT("Skipped: no usable Level Editor viewport for the rebuilt view (%s)."),
                    *Capture.ErrorCode));
            return true;
        }
        TestTrue(*FString::Printf(TEXT("raycast_screen failed unexpectedly: %s / %s"),
            *Capture.ErrorCode, *Capture.Message), false);
        return true;
    }

    if (!TestTrue(TEXT("result is present"), Capture.Result.IsValid()))
    {
        return true;
    }

    // The load-bearing check: the center ray direction must point along the camera forward
    // (straight down, -Z) for a pose looking down.
    const TSharedPtr<FJsonObject>* RayObj = nullptr;
    if (TestTrue(TEXT("result echoes ray"),
            Capture.Result->TryGetObjectField(TEXT("ray"), RayObj) && RayObj))
    {
        FVector Dir = FVector::ZeroVector;
        if (TestTrue(TEXT("ray carries a direction"), RSReadVector(*RayObj, TEXT("direction"), Dir)))
        {
            TestTrue(*FString::Printf(TEXT("center ray points straight down (-Z); got (%.3f,%.3f,%.3f)"),
                Dir.X, Dir.Y, Dir.Z), Dir.Equals(FVector(0.0, 0.0, -1.0), 0.02));
        }
    }

    // The view echo must round-trip the exact pose + dims + pixel we deprojected against.
    const TSharedPtr<FJsonObject>* ViewObj = nullptr;
    if (TestTrue(TEXT("result echoes view"),
            Capture.Result->TryGetObjectField(TEXT("view"), ViewObj) && ViewObj))
    {
        FString ProjMode;
        (*ViewObj)->TryGetStringField(TEXT("projectionMode"), ProjMode);
        TestEqual(TEXT("view echoes projectionMode"), ProjMode, FString(TEXT("perspective")));

        const TSharedPtr<FJsonObject>* PixelObj = nullptr;
        if ((*ViewObj)->TryGetObjectField(TEXT("pixel"), PixelObj) && PixelObj)
        {
            double PxX = -1.0;
            (*PixelObj)->TryGetNumberField(TEXT("x"), PxX);
            TestEqual(TEXT("view echoes the deprojected pixel x"), PxX, 512.0);
        }
    }

    return true;
}

// (b) Integration: a floor plane at Z=0, camera directly above looking down, the center
// pixel raycasts to a hit at Z ~ 0. Complex trace so the plane's render geometry is hit
// regardless of whether the engine Plane ships simple collision.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRaycastScreenHitsFloorTest,
    "PinWright.spatial.raycast_screen.HitsFloor",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FRaycastScreenHitsFloorTest::RunTest(const FString& Parameters)
{
    UWorld* World = RSEditorWorld();
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("Editor world not available; skipping raycast_screen floor test."));
        return true;
    }

    // Isolated column so no other level geometry sits under the straight-down ray.
    const double ColumnX = 222220.0;
    const double ColumnY = 88880.0;
    const FString Label = FString::Printf(TEXT("PW_RaycastScreenFloor_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));

    // Spawn a large flat plane at Z=0 (scaled up so the center-pixel ray lands well inside it).
    TSharedPtr<FJsonObject> SpawnPayload = MakeShared<FJsonObject>();
    SpawnPayload->SetStringField(TEXT("meshPath"), TEXT("/Engine/BasicShapes/Plane.Plane"));
    SpawnPayload->SetStringField(TEXT("actorName"), Label);
    SpawnPayload->SetObjectField(TEXT("location"), RSVec(ColumnX, ColumnY, 0.0));
    SpawnPayload->SetObjectField(TEXT("scale"), RSVec(20.0, 20.0, 1.0));

    FTestResponseCapture SpawnCapture;
    TestTrue(TEXT("actor.spawn handler registered"),
        InvokeHandlerWithCapture(TEXT("actor.spawn"), SpawnPayload, SpawnCapture));
    TestTrue(TEXT("floor plane spawned"), SpawnCapture.bSuccess);

    AActor* Spawned = McpActorUtils::FindActorByName(World, Label);
    ON_SCOPE_EXIT
    {
        if (Spawned)
        {
            Spawned->Destroy();
        }
    };

    if (!SpawnCapture.bSuccess)
    {
        return true;
    }

    // Flush the just-registered plane body into the scene-query structure.
    RSFlushEditorPhysics(World);

    // Camera directly above the column looking straight down; 256x256 => center pixel (128,128).
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetObjectField(TEXT("location"), RSVec(ColumnX, ColumnY, 1000.0));
    Payload->SetObjectField(TEXT("rotation"), RSRot(-90.0, 0.0, 0.0));
    Payload->SetNumberField(TEXT("x"), 128.0);
    Payload->SetNumberField(TEXT("y"), 128.0);
    Payload->SetNumberField(TEXT("width"), 256.0);
    Payload->SetNumberField(TEXT("height"), 256.0);
    Payload->SetBoolField(TEXT("traceComplex"), true);

    FTestResponseCapture Capture;
    TestTrue(TEXT("spatial.raycast_screen handler registered"),
        InvokeHandlerWithCapture(TEXT("spatial.raycast_screen"), Payload, Capture));

    if (!Capture.bSuccess)
    {
        if (RSIsNoViewportCode(Capture.ErrorCode))
        {
            PinWrightTestSkip::SkipAssertions(*this, TEXT("no-level-viewport"),
                FString::Printf(
                    TEXT("Skipped: no usable Level Editor viewport for the rebuilt view (%s)."),
                    *Capture.ErrorCode));
            return true;
        }
        TestTrue(*FString::Printf(TEXT("raycast_screen failed unexpectedly: %s / %s"),
            *Capture.ErrorCode, *Capture.Message), false);
        return true;
    }

    if (!Capture.Result.IsValid())
    {
        return true;
    }

    bool bHit = false;
    Capture.Result->TryGetBoolField(TEXT("hit"), bHit);
    TestTrue(TEXT("center-pixel ray hit the floor plane"), bHit);

    FVector HitLoc = FVector::ZeroVector;
    if (bHit && RSReadVector(Capture.Result, TEXT("location"), HitLoc))
    {
        TestTrue(*FString::Printf(TEXT("impact is on the Z=0 floor; got Z=%.3f"), HitLoc.Z),
            FMath::Abs(HitLoc.Z) < 2.0);
    }

    return true;
}

// (c) A pixel outside [0,width) x [0,height) is a caller error (INVALID_PARAMS). This runs
// before any deproject, so it needs no viewport.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRaycastScreenPixelOutOfRangeTest,
    "PinWright.spatial.raycast_screen.PixelOutOfRange",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FRaycastScreenPixelOutOfRangeTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetObjectField(TEXT("location"), RSVec(0.0, 0.0, 1000.0));
    Payload->SetObjectField(TEXT("rotation"), RSRot(-90.0, 0.0, 0.0));
    Payload->SetNumberField(TEXT("x"), 2000.0); // >= width
    Payload->SetNumberField(TEXT("y"), 512.0);
    Payload->SetNumberField(TEXT("width"), 1024.0);
    Payload->SetNumberField(TEXT("height"), 1024.0);

    FTestResponseCapture Capture;
    TestTrue(TEXT("spatial.raycast_screen handler registered"),
        InvokeHandlerWithCapture(TEXT("spatial.raycast_screen"), Payload, Capture));
    TestFalse(TEXT("out-of-range pixel is an error"), Capture.bSuccess);
    TestEqual(TEXT("out-of-range pixel -> INVALID_PARAMS"),
        Capture.ErrorCode, FString(TEXT("INVALID_PARAMS")));

    return true;
}

// (d) An unknown argument is rejected by the dispatcher's auto-validation with
// UNKNOWN_PARAMS. That path only runs through the real dispatcher, so drive it via a
// wired dispatcher fixture (the direct InvokeHandlerWithCapture bypass skips the check).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRaycastScreenUnknownArgRejectedTest,
    "PinWright.spatial.raycast_screen.UnknownArgRejected",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FRaycastScreenUnknownArgRejectedTest::RunTest(const FString& Parameters)
{
    bSuppressLogWarnings = true;

    DispatcherTestHelpers::FSinkPtr Sink;
    FRpcDispatcher Dispatcher;
    DispatcherTestHelpers::MakeDispatcher(Sink, Dispatcher);

    TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
    Params->SetObjectField(TEXT("location"), RSVec(0.0, 0.0, 1000.0));
    Params->SetObjectField(TEXT("rotation"), RSRot(-90.0, 0.0, 0.0));
    Params->SetNumberField(TEXT("x"), 512.0);
    Params->SetNumberField(TEXT("y"), 512.0);
    Params->SetNumberField(TEXT("width"), 1024.0);
    Params->SetNumberField(TEXT("height"), 1024.0);
    Params->SetStringField(TEXT("bogusParam"), TEXT("nope"));

    bool bSuccess = true;
    FString ErrorCode;
    DispatcherTestHelpers::Dispatch(Dispatcher, Sink, TEXT("spatial.raycast_screen"),
        TEXT("req-raycast-screen-unknown"), Params, bSuccess, ErrorCode);

    TestFalse(TEXT("unknown param is rejected"), bSuccess);
    TestEqual(TEXT("unknown param -> UNKNOWN_PARAMS"), ErrorCode, FString(TEXT("UNKNOWN_PARAMS")));

    return true;
}

// (e) Headless honesty: a fully valid request must either succeed or fail ONLY with a typed
// deproject/no-viewport code - never a spurious param error. This pins the contract that a
// missing viewport is reported as such, not misclassified.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRaycastScreenHeadlessTypedErrorTest,
    "PinWright.spatial.raycast_screen.HeadlessTypedError",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FRaycastScreenHeadlessTypedErrorTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetObjectField(TEXT("location"), RSVec(0.0, 0.0, 1000.0));
    Payload->SetObjectField(TEXT("rotation"), RSRot(-90.0, 0.0, 0.0));
    Payload->SetNumberField(TEXT("x"), 512.0);
    Payload->SetNumberField(TEXT("y"), 512.0);
    Payload->SetNumberField(TEXT("width"), 1024.0);
    Payload->SetNumberField(TEXT("height"), 1024.0);

    FTestResponseCapture Capture;
    TestTrue(TEXT("spatial.raycast_screen handler registered"),
        InvokeHandlerWithCapture(TEXT("spatial.raycast_screen"), Payload, Capture));

    if (Capture.bSuccess)
    {
        // Succeeded: the host had a viewport. The response must carry the ray echo.
        TestTrue(TEXT("successful result echoes the ray"),
            Capture.Result.IsValid() && Capture.Result->HasField(TEXT("ray")));
        return true;
    }

    // Failed: it must be a typed no-viewport/deproject code, not a spurious param error.
    TestTrue(*FString::Printf(TEXT("valid request fails only with a typed deproject code; got '%s'"),
        *Capture.ErrorCode), RSIsNoViewportCode(Capture.ErrorCode));

    return true;
}
