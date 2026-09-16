// Copyright (c) 2026 Alexander Penkin. MIT License.

// Failure-direction guard for B-extrude-polygon-uncapped-nonconvex.
//
// The defect is upstream: Geometry Script's capped extrude accepts any three-or-more points and
// caps by a flat triangulation whose ear clipper forces a non-ear when it can find none, so an
// invalid cross-section produces a full-count but geometrically wrong cap and reports success.
// PinWright cannot fix the engine; it can refuse to hand it an input it is documented not to take.
//
// Two things are asserted here, and the second is the one the adversarial reviews on the ticket
// asked for. First, PolygonIsSimple rejects what is actually invalid. Second - the control that
// keeps the guard honest - it ACCEPTS a strongly concave outline, because concavity is not the
// defect: the engine's ear test handles concave ears explicitly, and a guard that refused every
// non-convex cross-section would be a worse lie than the one it replaced. A known-good convex
// square, a known-good concave L and an arrowhead sit next to a bowtie, a collinear run and a
// repeated point, so the predicate is measured against both directions rather than only the
// failing one.
#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Dispatch/RpcDispatcher.h"
#include "Handlers/Geometry/GeometryOps_Primitives.h"
#include "Tests/Infra/DispatcherTestHelpers.h"
#include "Tests/Geometry/GeometryTestHelpers.h"

#include "Editor.h"
#include "Engine/World.h"

using DispatcherTestHelpers::MakeDispatcher;
using DispatcherTestHelpers::Dispatch;
using DispatcherTestHelpers::FSinkPtr;
using GeometryTestHelpers::DestroyActorsWithLabel;

// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryCrossSectionSimplicityTest,
    "PinWright.geometry.cross_section.SimplicityAcceptsConcaveAndRejectsSelfIntersecting",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGeometryCrossSectionSimplicityTest::RunTest(const FString& Parameters)
{
    FString Reason;

    // --- known-good controls -------------------------------------------------------------
    // Convex. If this ever fails the predicate is broken, not the input, and every rejection
    // below would be meaningless.
    const TArray<FVector2D> Square = { {0, 0}, {10, 0}, {10, 10}, {0, 10} };
    TestTrue(TEXT("a convex square is a cappable cross-section"),
        GeometryOps::PolygonIsSimple(Square, Reason));

    // Strongly concave, and legal. This is the control the ticket's reviewers asked for: the
    // engine's ear clipper handles concave ears, so concavity alone must not be refused.
    const TArray<FVector2D> LShape = { {0, 0}, {30, 0}, {30, 10}, {10, 10}, {10, 30}, {0, 30} };
    TestTrue(TEXT("a concave L outline is still a cappable cross-section"),
        GeometryOps::PolygonIsSimple(LShape, Reason));

    // A reflex vertex deep enough that ear clipping must skip candidates to find an ear.
    const TArray<FVector2D> Arrowhead = { {0, 0}, {20, 40}, {40, 0}, {20, 12} };
    TestTrue(TEXT("an arrowhead with a deep reflex vertex is still cappable"),
        GeometryOps::PolygonIsSimple(Arrowhead, Reason));

    // --- known-bad controls --------------------------------------------------------------
    // Self-intersecting. This is the case that produces a full-count, geometrically wrong cap.
    const TArray<FVector2D> Bowtie = { {0, 0}, {10, 10}, {10, 0}, {0, 10} };
    TestFalse(TEXT("a self-intersecting bowtie is refused"),
        GeometryOps::PolygonIsSimple(Bowtie, Reason));
    TestTrue(TEXT("the bowtie refusal names the crossing"), Reason.Contains(TEXT("crosses")));

    // Collinear: three points enclosing nothing. The engine emits N-2 cap triangles anyway.
    const TArray<FVector2D> Collinear = { {0, 0}, {10, 0}, {20, 0} };
    TestFalse(TEXT("a collinear run is refused"),
        GeometryOps::PolygonIsSimple(Collinear, Reason));
    TestTrue(TEXT("the collinear refusal says the outline encloses no area"),
        Reason.Contains(TEXT("no area")));

    // A repeated point makes a zero-length edge, which makes every orientation test on it
    // meaningless - so it has to be caught before the crossing scan, not by it.
    const TArray<FVector2D> Repeated = { {0, 0}, {10, 0}, {10, 0}, {0, 10} };
    TestFalse(TEXT("a repeated point is refused"),
        GeometryOps::PolygonIsSimple(Repeated, Reason));

    const TArray<FVector2D> TooFew = { {0, 0}, {10, 0} };
    TestFalse(TEXT("two points are not a cross-section"),
        GeometryOps::PolygonIsSimple(TooFew, Reason));

    // Signed area carries orientation, which is what tells a mirrored outline from a valid one.
    TestTrue(TEXT("a counter-clockwise square has positive signed area"),
        GeometryOps::PolygonSignedArea(Square) > 0.0);
    const TArray<FVector2D> Reversed = { {0, 10}, {10, 10}, {10, 0}, {0, 0} };
    TestTrue(TEXT("reversing the winding flips the sign of the area"),
        GeometryOps::PolygonSignedArea(Reversed) < 0.0);
    return true;
}

// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryCreateRampDegenerateRefusedTest,
    "PinWright.geometry.create_ramp.DegenerateWedgeIsRefusedNotSilentlyCapped",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGeometryCreateRampDegenerateRefusedTest::RunTest(const FString& Parameters)
{
    if (!GEditor || !IsValid(GEditor->GetEditorWorldContext().World()))
    {
        AddWarning(TEXT("PINWRIGHT_ASSERTIONS_SKIPPED: no editor world available; "
                        "create_ramp degenerate-wedge assertions did not run"));
        return true;
    }

    // create_ramp is the only verb in the plugin that drives AppendSimpleExtrudePolygon. Its
    // wedge is synthesised from three caller numbers, so the caller controls whether the outline
    // is cappable - height 0 collapses it onto a line, and the engine caps that silently.
    auto TryRamp = [this](const TCHAR* What, double Length, double Height, double Width,
                          bool bExpectSuccess)
    {
        const FString Label = FString::Printf(TEXT("PW_RampGuardProbe_%s"),
            *FGuid::NewGuid().ToString(EGuidFormats::Digits));

        FSinkPtr Sink;
        FRpcDispatcher Dispatcher;
        MakeDispatcher(Sink, Dispatcher);

        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("name"), Label);
        Params->SetNumberField(TEXT("length"), Length);
        Params->SetNumberField(TEXT("height"), Height);
        Params->SetNumberField(TEXT("width"), Width);

        bool bOk = false;
        FString ErrorCode;
        TSharedPtr<FJsonObject> Result;
        Dispatch(Dispatcher, Sink, TEXT("geometry.create_ramp"), TEXT("req-ramp-guard"),
            Params, bOk, Result, ErrorCode);
        DestroyActorsWithLabel(Label);

        if (bExpectSuccess)
        {
            TestTrue(FString::Printf(TEXT("%s must still build"), What), bOk);
        }
        else
        {
            TestFalse(FString::Printf(
                TEXT("%s must be refused, not returned as a successful mesh with a garbage cap"),
                What), bOk);
            TestEqual(FString::Printf(TEXT("%s is refused as an argument error"), What),
                ErrorCode, FString(TEXT("INVALID_ARGUMENT")));
        }
    };

    // The known-good control first, so a guard that refused everything could not pass this test.
    TryRamp(TEXT("an ordinary ramp"), 200.0, 50.0, 100.0, /*bExpectSuccess=*/true);

    TryRamp(TEXT("a ramp of zero height"), 200.0, 0.0, 100.0, /*bExpectSuccess=*/false);
    TryRamp(TEXT("a ramp of zero length"), 0.0, 50.0, 100.0, /*bExpectSuccess=*/false);
    TryRamp(TEXT("a ramp of negative height"), 200.0, -50.0, 100.0, /*bExpectSuccess=*/false);
    TryRamp(TEXT("a ramp of zero width"), 200.0, 50.0, 0.0, /*bExpectSuccess=*/false);
    return true;
}
