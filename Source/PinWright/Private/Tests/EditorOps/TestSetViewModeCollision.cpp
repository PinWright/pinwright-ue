// Copyright (c) 2026 Alexander Penkin. MIT License.

// Tests for editor.set_view_mode's collision view modes and the collision report that ships
// with them.
//
// The classification tests are pure logic over synthesized summaries: no world, no RHI, no
// viewport, so they run everywhere and are the ones that actually pin the engine rule down.
// They encode UE 5.8 Runtime/Engine/Private/Rendering/NaniteResources.cpp:2521-2528 - both the
// UseComplexAsSimple / UseSimpleAsComplex inversion and the three independent ways a mesh can
// be invisible in a collision view.
//
// Counterfactual for the inversion tests: swap either arm of DrawsSimpleInChannel /
// DrawsComplexInChannel in CollisionSummaryUtils.cpp and the ComplexAsSimple / SimpleAsComplex
// cases below flip. Counterfactual for the invisibility tests: drop any one of the three guards
// at the top of DrawsInChannel and the matching case starts claiming the mesh is visible - the
// exact silent "looks like a clean scene" failure this feature exists to prevent.
#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Tests/TestUtils.h"
#include "Tests/TestSkipReporting.h"
#include "Utils/CollisionSummaryUtils.h"

#include "Editor.h"
#include "IAssetViewport.h"
#include "LevelEditor.h"
#include "Modules/ModuleManager.h"

namespace
{
    // A mesh that is collided in every ordinary way: enabled, responds to both channels, four
    // simple primitives, default trace flag. Tests mutate one field at a time from here so each
    // assertion isolates exactly one cause.
    PinWrightCollisionSummary::FComponentCollisionSummary PinWrightMakeHealthySummary()
    {
        PinWrightCollisionSummary::FComponentCollisionSummary Summary;
        Summary.bValid = true;
        Summary.bHasBodySetup = true;
        Summary.bCollisionEnabled = true;
        Summary.CollisionEnabledName = TEXT("QueryAndPhysics");
        Summary.Trace = PinWrightCollisionSummary::ETraceKind::SimpleAndComplex;
        Summary.SimpleShapeCount = 4;
        Summary.BoxCount = 4;
        Summary.bRespondsToPawn = true;
        Summary.bRespondsToVisibility = true;
        return Summary;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCollisionSummaryChannelMappingTest,
    "PinWright.editor.set_view_mode.CollisionChannelMapping",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCollisionSummaryChannelMappingTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightCollisionSummary;

    // Default trace flag: the simple view draws simple, the complex view draws complex.
    const FComponentCollisionSummary Healthy = PinWrightMakeHealthySummary();
    TestTrue(TEXT("default trace: simple view draws simple geometry"),
        DrawsSimpleInChannel(Healthy, EDrawChannel::Simple));
    TestFalse(TEXT("default trace: simple view does not draw complex geometry"),
        DrawsComplexInChannel(Healthy, EDrawChannel::Simple));
    TestTrue(TEXT("default trace: complex view draws complex geometry"),
        DrawsComplexInChannel(Healthy, EDrawChannel::Complex));
    TestFalse(TEXT("default trace: complex view does not draw simple geometry"),
        DrawsSimpleInChannel(Healthy, EDrawChannel::Complex));

    // UseComplexAsSimple inverts the SIMPLE view: per-triangle geometry answers simple queries,
    // so the simple view draws complex and stops drawing the simple primitives.
    FComponentCollisionSummary ComplexAsSimple = PinWrightMakeHealthySummary();
    ComplexAsSimple.Trace = ETraceKind::ComplexAsSimple;
    TestFalse(TEXT("UseComplexAsSimple: simple view stops drawing simple geometry"),
        DrawsSimpleInChannel(ComplexAsSimple, EDrawChannel::Simple));
    TestTrue(TEXT("UseComplexAsSimple: simple view draws complex geometry instead"),
        DrawsComplexInChannel(ComplexAsSimple, EDrawChannel::Simple));

    // UseSimpleAsComplex inverts the COMPLEX view the same way, in the other direction.
    FComponentCollisionSummary SimpleAsComplex = PinWrightMakeHealthySummary();
    SimpleAsComplex.Trace = ETraceKind::SimpleAsComplex;
    TestTrue(TEXT("UseSimpleAsComplex: complex view draws the simple primitives"),
        DrawsSimpleInChannel(SimpleAsComplex, EDrawChannel::Complex));
    TestFalse(TEXT("UseSimpleAsComplex: complex view stops drawing complex geometry"),
        DrawsComplexInChannel(SimpleAsComplex, EDrawChannel::Complex));

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCollisionSummaryInvisibilityCausesTest,
    "PinWright.editor.set_view_mode.CollisionInvisibilityCauses",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCollisionSummaryInvisibilityCausesTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightCollisionSummary;

    TestTrue(TEXT("a healthy mesh is drawn in the simple collision view"),
        DrawsInChannel(PinWrightMakeHealthySummary(), EDrawChannel::Simple));

    // Cause 1: collision disabled outright.
    FComponentCollisionSummary Disabled = PinWrightMakeHealthySummary();
    Disabled.bCollisionEnabled = false;
    Disabled.CollisionEnabledName = TEXT("NoCollision");
    TestFalse(TEXT("collision disabled draws nothing"),
        DrawsInChannel(Disabled, EDrawChannel::Simple));

    // Cause 2: a body setup that holds zero simple primitives. This is the collisionless-tree
    // case that silently blocked ground traces.
    FComponentCollisionSummary NoShapes = PinWrightMakeHealthySummary();
    NoShapes.SimpleShapeCount = 0;
    NoShapes.BoxCount = 0;
    TestFalse(TEXT("a body setup with zero simple primitives draws nothing in the simple view"),
        DrawsInChannel(NoShapes, EDrawChannel::Simple));

    // Cause 3: good collision that ignores the channel this view queries.
    FComponentCollisionSummary IgnoresPawn = PinWrightMakeHealthySummary();
    IgnoresPawn.bRespondsToPawn = false;
    TestFalse(TEXT("ignoring the Pawn channel draws nothing in the simple view"),
        DrawsInChannel(IgnoresPawn, EDrawChannel::Simple));
    TestTrue(TEXT("...but the same mesh is still drawn in the complex view"),
        DrawsInChannel(IgnoresPawn, EDrawChannel::Complex));

    // Not a cause: no body setup at all (landscape heightfield collision). Reporting that as
    // "no collision" would be a false alarm, which is worse than saying nothing.
    FComponentCollisionSummary NoBodySetup;
    NoBodySetup.bValid = true;
    NoBodySetup.bCollisionEnabled = true;
    NoBodySetup.bRespondsToPawn = true;
    NoBodySetup.bRespondsToVisibility = true;
    NoBodySetup.SimpleShapeCount = -1;
    TestTrue(TEXT("a component with no body setup is not claimed invisible"),
        DrawsInChannel(NoBodySetup, EDrawChannel::Simple));

    // A zero-primitive body setup and a missing body setup must stay distinguishable: the
    // shared count reports -1 for the latter and 0 for the former, and spatial.raycast's
    // renderGeometryHit rides on exactly that distinction.
    TestEqual(TEXT("missing body setup reports -1 simple shapes"), NoBodySetup.SimpleShapeCount, -1);
    TestEqual(TEXT("empty body setup reports 0 simple shapes"), NoShapes.SimpleShapeCount, 0);

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEditorSetViewModeUnknownModeTest,
    "PinWright.editor.set_view_mode.UnknownModeStillRejected",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FEditorSetViewModeUnknownModeTest::RunTest(const FString& Parameters)
{
    if (!GEditor)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("Skipped: GEditor is null."));
        return true;
    }

    // Adding the collision branches must not turn the unknown-mode rejection into a fallthrough.
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("viewMode"), TEXT("CollisionSomethingElse"));

    FTestResponseCapture Capture;
    TestTrue(TEXT("editor.set_view_mode handler found"),
        InvokeHandlerWithCapture(TEXT("editor.set_view_mode"), Payload, Capture));
    TestFalse(TEXT("an unrecognised mode is rejected"), Capture.bSuccess);
    TestEqual(TEXT("rejection is typed UNKNOWN_VIEW_MODE"), Capture.ErrorCode,
        FString(TEXT("UNKNOWN_VIEW_MODE")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEditorSetViewModeCollisionReportTest,
    "PinWright.editor.set_view_mode.CollisionModeReturnsReport",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FEditorSetViewModeCollisionReportTest::RunTest(const FString& Parameters)
{
    if (!GEditor)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("Skipped: GEditor is null; no editor to resolve a level viewport against."));
        return true;
    }

    FLevelEditorModule& LevelEditorModule =
        FModuleManager::GetModuleChecked<FLevelEditorModule>("LevelEditor");
    if (!LevelEditorModule.GetFirstActiveViewport().IsValid())
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-level-viewport"),
            TEXT("Skipped: no active level viewport; the viewport-client path is unreachable in this run."));
        return true;
    }

    // Lower-case alias on purpose: the mode table matches case-insensitively, and the response
    // echoes the canonical spelling regardless of how the caller wrote it.
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("viewMode"), TEXT("worldcollision"));

    FTestResponseCapture Capture;
    TestTrue(TEXT("editor.set_view_mode handler found"),
        InvokeHandlerWithCapture(TEXT("editor.set_view_mode"), Payload, Capture));
    TestTrue(TEXT("collision view mode reports success"), Capture.bSuccess);

    if (!Capture.Result.IsValid())
    {
        AddError(TEXT("collision view mode returned no result object"));
        return true;
    }

    FString ReportedMode;
    Capture.Result->TryGetStringField(TEXT("viewMode"), ReportedMode);
    TestEqual(TEXT("the 'worldcollision' alias resolves to CollisionSimple"), ReportedMode,
        FString(TEXT("CollisionSimple")));

    // The data half is the point of the feature: without it an empty picture is
    // indistinguishable from a fully collided scene.
    const TSharedPtr<FJsonObject>* Collision = nullptr;
    if (!Capture.Result->TryGetObjectField(TEXT("collision"), Collision) || !Collision)
    {
        AddError(TEXT("collision view mode returned no 'collision' report"));
        return true;
    }

    FString Channel;
    (*Collision)->TryGetStringField(TEXT("channel"), Channel);
    TestEqual(TEXT("report names the channel it describes"), Channel, FString(TEXT("collisionSimple")));

    FString Draws;
    (*Collision)->TryGetStringField(TEXT("draws"), Draws);
    TestEqual(TEXT("the simple/world view is reported as drawing simple geometry"), Draws,
        FString(TEXT("simple")));

    FString Scope;
    (*Collision)->TryGetStringField(TEXT("scope"), Scope);
    TestEqual(TEXT("report states its scope rather than implying 'in frame'"), Scope,
        FString(TEXT("level")));

    const TSharedPtr<FJsonObject>* Counts = nullptr;
    TestTrue(TEXT("report carries exact counts"),
        (*Collision)->TryGetObjectField(TEXT("counts"), Counts) && Counts != nullptr);

    // Restore Lit so a collision view is not left on the shared viewport for the next test or
    // for a concurrent agent. set_view_mode deliberately does not self-restore.
    TSharedPtr<FJsonObject> RestorePayload = MakeShared<FJsonObject>();
    RestorePayload->SetStringField(TEXT("viewMode"), TEXT("Lit"));
    FTestResponseCapture RestoreCapture;
    InvokeHandlerWithCapture(TEXT("editor.set_view_mode"), RestorePayload, RestoreCapture);

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEditorSetViewModeCollisionReportOptOutTest,
    "PinWright.editor.set_view_mode.CollisionReportOptOut",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FEditorSetViewModeCollisionReportOptOutTest::RunTest(const FString& Parameters)
{
    if (!GEditor)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("Skipped: GEditor is null."));
        return true;
    }
    FLevelEditorModule& LevelEditorModule =
        FModuleManager::GetModuleChecked<FLevelEditorModule>("LevelEditor");
    if (!LevelEditorModule.GetFirstActiveViewport().IsValid())
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-level-viewport"),
            TEXT("Skipped: no active level viewport."));
        return true;
    }

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("viewMode"), TEXT("collisionComplex"));
    Payload->SetBoolField(TEXT("collisionReport"), false);

    FTestResponseCapture Capture;
    InvokeHandlerWithCapture(TEXT("editor.set_view_mode"), Payload, Capture);
    TestTrue(TEXT("opting out still switches the view mode"), Capture.bSuccess);

    if (Capture.Result.IsValid())
    {
        const TSharedPtr<FJsonObject>* Collision = nullptr;
        TestFalse(TEXT("collisionReport:false omits the report"),
            Capture.Result->TryGetObjectField(TEXT("collision"), Collision));
    }

    TSharedPtr<FJsonObject> RestorePayload = MakeShared<FJsonObject>();
    RestorePayload->SetStringField(TEXT("viewMode"), TEXT("Lit"));
    FTestResponseCapture RestoreCapture;
    InvokeHandlerWithCapture(TEXT("editor.set_view_mode"), RestorePayload, RestoreCapture);

    return true;
}
