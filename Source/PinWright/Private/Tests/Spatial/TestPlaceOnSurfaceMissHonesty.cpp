// Copyright (c) 2026 Alexander Penkin. MIT License.

// TestPlaceOnSurfaceMissHonesty.cpp - spatial.place_on_surface, worldPoint mode, trace miss.
//
// The defect: on a miss the verb fell back to the caller's own worldPoint, treated that as the
// surface, and returned placed:true. It failed hardest exactly where traces have nothing to hit -
// map edges, holes, above water - which is the mechanism behind props ending up beyond the map
// edge. A response that says "placed" for a surface that was never measured is indistinguishable
// from one that measured a real surface, and the agent has no other view of the world.
//
// Both tests are written in the failure direction: the first breaks the moment a miss goes back
// to reporting success, the second breaks if the opt-in path stops flagging its surface as
// unmeasured. Neither is satisfied by the verb merely returning success.
//
// Fixtures are spawned through the real actor.spawn RPC at an isolated column far from any level
// geometry (a third column, distinct from TestPlacementHandlers.cpp's and TestGroundPlacement's,
// so concurrent fixtures cannot answer each other's probes) and destroyed on scope exit. Helper
// names carry a MissTest prefix and live in one file-scope anonymous namespace, per the
// Unity-build rule in CLAUDE.md.

#include "Misc/AutomationTest.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/HandlerRegistration.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Tests/TestSkipReporting.h"
#include "Tests/TestUtils.h"
#include "Utils/ActorUtils.h"

#include "Editor.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Misc/Guid.h"
#include "Misc/ScopeExit.h"

namespace
{
    // 12 km / 9.8 km out. Far enough that no landscape, floor or sibling test fixture is under
    // the probe column, which is what makes "the trace missed" the fixture rather than an
    // assumption. The two existing spatial suites sit at ~3.3 km and ~2.2 km.
    constexpr double MissTestColX = 1200000.0;
    constexpr double MissTestColY = 980000.0;

    // Where the actor is parked, and where the probe point is. Both are in empty air; the actor
    // sits above the probe point so nothing about the fixture can answer the probe (the verb
    // ignores the placed actor anyway, and this makes that independent of the verb).
    constexpr double MissTestActorZ = 40000.0;
    constexpr double MissTestPointZ = 20000.0;

    FString MissTestLabel(const TCHAR* Prefix)
    {
        return FString::Printf(TEXT("PWM_%s_%s"), Prefix,
            *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    }

    UWorld* MissTestEditorWorld()
    {
        return GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    }

    TSharedPtr<FJsonObject> MissTestVec(double X, double Y, double Z)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetNumberField(TEXT("x"), X);
        Obj->SetNumberField(TEXT("y"), Y);
        Obj->SetNumberField(TEXT("z"), Z);
        return Obj;
    }

    AActor* MissTestSpawnCube(FAutomationTestBase& Test, UWorld* World, const FString& Label,
                              const FVector& Location)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("meshPath"), TEXT("/Engine/BasicShapes/Cube.Cube"));
        Payload->SetStringField(TEXT("actorName"), Label);
        Payload->SetObjectField(TEXT("location"), MissTestVec(Location.X, Location.Y, Location.Z));

        FTestResponseCapture Capture;
        Test.TestTrue(TEXT("actor.spawn handler registered"),
            InvokeHandlerWithCapture(TEXT("actor.spawn"), Payload, Capture));
        Test.TestTrue(*FString::Printf(TEXT("cube '%s' spawned"), *Label), Capture.bSuccess);
        return McpActorUtils::FindActorByName(World, Label);
    }

    // place_on_surface payload aimed at a point with nothing under it.
    TSharedPtr<FJsonObject> MissTestPayload(const FString& ActorLabel, bool bAssume)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("actorName"), ActorLabel);
        Payload->SetObjectField(TEXT("worldPoint"),
            MissTestVec(MissTestColX, MissTestColY, MissTestPointZ));
        if (bAssume)
        {
            Payload->SetBoolField(TEXT("assumePointIsSurface"), true);
        }
        return Payload;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPlaceOnSurfaceWorldPointMissIsAnErrorTest,
    "PinWright.spatial.place_on_surface.WorldPointMissIsAnError",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPlaceOnSurfaceWorldPointMissIsAnErrorTest::RunTest(const FString& Parameters)
{
    UWorld* World = MissTestEditorWorld();
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("Editor world not available; skipping place_on_surface miss test."));
        return true;
    }

    const FString Label = MissTestLabel(TEXT("MissProp"));
    const FVector StartLocation(MissTestColX, MissTestColY, MissTestActorZ);
    AActor* Prop = MissTestSpawnCube(*this, World, Label, StartLocation);
    ON_SCOPE_EXIT
    {
        if (Prop) { Prop->Destroy(); }
    };
    if (!Prop)
    {
        AddError(TEXT("fixture cube did not spawn"));
        return true;
    }

    const FVector Before = Prop->GetActorLocation();

    FTestResponseCapture Capture;
    TestTrue(TEXT("spatial.place_on_surface handler registered"),
        InvokeHandlerWithCapture(TEXT("spatial.place_on_surface"),
            MissTestPayload(Label, /*bAssume=*/false), Capture));

    // The assertion the old behaviour cannot satisfy: it answered success with placed:true on
    // an invented surface.
    TestFalse(TEXT("a worldPoint trace miss is reported as a failure"), Capture.bSuccess);
    TestEqual(TEXT("the miss carries its own code, not the generic surface code"),
        Capture.ErrorCode, FString(TEXT("SURFACE_TRACE_MISSED")));

    // Reading the world back, not the response: a verb that refuses must also not have moved
    // anything on its way to refusing.
    TestTrue(TEXT("the actor was not moved by a refused placement"),
        Prop->GetActorLocation().Equals(Before, 0.01));

    // The recovery is in the message, per the errors-carry-recovery rule; a caller that hits
    // this needs to be told the opt-in exists.
    TestTrue(TEXT("the message names the opt-in parameter"),
        Capture.Message.Contains(TEXT("assumePointIsSurface")));

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPlaceOnSurfaceAssumedSurfaceIsReportedUnmeasuredTest,
    "PinWright.spatial.place_on_surface.AssumedSurfaceIsReportedUnmeasured",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPlaceOnSurfaceAssumedSurfaceIsReportedUnmeasuredTest::RunTest(const FString& Parameters)
{
    UWorld* World = MissTestEditorWorld();
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("Editor world not available; skipping place_on_surface assumed-surface test."));
        return true;
    }

    const FString Label = MissTestLabel(TEXT("AssumeProp"));
    AActor* Prop = MissTestSpawnCube(*this, World, Label,
        FVector(MissTestColX, MissTestColY, MissTestActorZ));
    ON_SCOPE_EXIT
    {
        if (Prop) { Prop->Destroy(); }
    };
    if (!Prop)
    {
        AddError(TEXT("fixture cube did not spawn"));
        return true;
    }

    FTestResponseCapture Capture;
    TestTrue(TEXT("spatial.place_on_surface handler registered"),
        InvokeHandlerWithCapture(TEXT("spatial.place_on_surface"),
            MissTestPayload(Label, /*bAssume=*/true), Capture));
    TestTrue(TEXT("the opt-in path still places"), Capture.bSuccess);

    if (!Capture.Result.IsValid())
    {
        AddError(TEXT("place_on_surface returned no result object"));
        return true;
    }

    // Opting in must not buy silence. The surface was never measured and the response has to
    // say so, otherwise the opt-in just relocates the original lie.
    const TSharedPtr<FJsonObject>* Surface = nullptr;
    if (!Capture.Result->TryGetObjectField(TEXT("surface"), Surface) || !Surface || !Surface->IsValid())
    {
        AddError(TEXT("result missing surface block"));
        return true;
    }
    bool bMeasured = true;
    TestTrue(TEXT("surface block carries a measured flag"),
        (*Surface)->TryGetBoolField(TEXT("measured"), bMeasured));
    TestFalse(TEXT("an assumed surface is reported as NOT measured"), bMeasured);

    const TSharedPtr<FJsonObject>* Verification = nullptr;
    if (Capture.Result->TryGetObjectField(TEXT("verification"), Verification)
        && Verification && Verification->IsValid())
    {
        bool bAgainstMeasured = true;
        TestTrue(TEXT("verification says what restingGap was measured against"),
            (*Verification)->TryGetBoolField(TEXT("againstMeasuredSurface"), bAgainstMeasured));
        TestFalse(TEXT("restingGap is not claimed to be against a measured surface"),
            bAgainstMeasured);
    }
    else
    {
        AddError(TEXT("result missing verification block"));
    }

    return true;
}
