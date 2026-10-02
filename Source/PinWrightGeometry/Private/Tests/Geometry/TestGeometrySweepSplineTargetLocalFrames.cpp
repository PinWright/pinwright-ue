// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression test for B-geometry-sweep-spline-frames-world-space.
//
// geometry.sweep and geometry.extrude_along_spline sample their spline in WORLD space and append
// the swept tube to the target actor's UDynamicMesh, which is a LOCAL-space buffer. Nothing
// applied the inverse of the target's transform, so a target anywhere but the identity got its
// tube displaced (and rotated/scaled) away from the spline by its own transform, while the verb
// reported success. The fix brings every sampled frame into the target component's space in the
// shared sampler both verbs call.
//
// Strategy (production handlers through the real dispatcher): an empty procedural-mesh target and
// a 3-point spline actor, BOTH at non-identity transforms (offset, rotated; the target also
// uniformly scaled). Run the verb, then take every mesh vertex to world space through the
// target's component transform and measure its distance to the spline curve. An empty target
// sweeps the default 50-unit profile, which the target's 1.5 scale makes 75 in world space, so
// every vertex must sit within that radius of the curve (plus chord/tessellation slack).
//
// Counterfactual: reverting the fix re-applies the target transform to world-space frames, putting
// the tube more than a thousand units off the spline - the max-distance assertion fails.
#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Dispatch/RpcDispatcher.h"
#include "Tests/Infra/DispatcherTestHelpers.h"
#include "Tests/Geometry/GeometryTestHelpers.h"

#include "Components/DynamicMeshComponent.h"
#include "Components/SplineComponent.h"
#include "DynamicMesh/DynamicMesh3.h"
#include "DynamicMeshActor.h"
#include "UDynamicMesh.h"
#include "Editor.h"
#include "Engine/World.h"

using DispatcherTestHelpers::MakeDispatcher;
using DispatcherTestHelpers::Dispatch;

// File-local, uniquely named for Unity builds. Runs `Method` with a spline actor and target both
// off the identity transform and asserts the swept vertices land on the spline in world space.
static void SweepSplineLocalFramesTest_Run(FAutomationTestBase& Test, const TCHAR* Method,
                                           const TCHAR* StepsParam)
{
    if (!GEditor || !IsValid(GEditor->GetEditorWorldContext().World()))
    {
        Test.AddError(TEXT("No editor world available; cannot exercise the spline sweep verbs"));
        return;
    }

    DispatcherTestHelpers::FSinkPtr Sink;
    FRpcDispatcher Dispatcher;
    MakeDispatcher(Sink, Dispatcher);

    const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString TargetLabel = FString::Printf(TEXT("PW_SweepLocal_Target_%s"), *Suffix);
    const FString SplineLabel = FString::Printf(TEXT("PW_SweepLocal_Spline_%s"), *Suffix);
    ON_SCOPE_EXIT
    {
        GeometryTestHelpers::DestroyActorsWithLabel(TargetLabel);
        GeometryTestHelpers::DestroyActorsWithLabel(SplineLabel);
    };

    {
        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("name"), TargetLabel);
        bool bOk = false;
        FString Err;
        Dispatch(Dispatcher, Sink, TEXT("geometry.create_procedural_mesh"), TEXT("req-sweeplocal-target"),
            Params, bOk, Err);
        if (!Test.TestTrue(TEXT("geometry.create_procedural_mesh created the target"), bOk))
        {
            return;
        }
    }
    {
        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("actorName"), SplineLabel);
        TArray<TSharedPtr<FJsonValue>> Points;
        auto MakePoint = [](double X, double Y, double Z)
        {
            TSharedPtr<FJsonObject> Loc = MakeShared<FJsonObject>();
            Loc->SetNumberField(TEXT("x"), X);
            Loc->SetNumberField(TEXT("y"), Y);
            Loc->SetNumberField(TEXT("z"), Z);
            TSharedPtr<FJsonObject> Pt = MakeShared<FJsonObject>();
            Pt->SetObjectField(TEXT("location"), Loc);
            return MakeShared<FJsonValueObject>(Pt);
        };
        Points.Add(MakePoint(0.0, 0.0, 0.0));
        Points.Add(MakePoint(300.0, 0.0, 0.0));
        Points.Add(MakePoint(600.0, 200.0, 100.0));
        Params->SetArrayField(TEXT("points"), Points);
        bool bOk = false;
        FString Err;
        Dispatch(Dispatcher, Sink, TEXT("spline.create_spline_actor"), TEXT("req-sweeplocal-spline"),
            Params, bOk, Err);
        if (!Test.TestTrue(TEXT("spline.create_spline_actor created the spline"), bOk))
        {
            return;
        }
    }

    ADynamicMeshActor* Target = Cast<ADynamicMeshActor>(GeometryTestHelpers::FindActorByLabel(TargetLabel));
    AActor* SplineActor = GeometryTestHelpers::FindActorByLabel(SplineLabel);
    if (!Test.TestNotNull(TEXT("target actor is present"), Target) ||
        !Test.TestNotNull(TEXT("spline actor is present"), SplineActor))
    {
        return;
    }
    USplineComponent* SplineComp = SplineActor->FindComponentByClass<USplineComponent>();
    UDynamicMeshComponent* DMC = Target->GetDynamicMeshComponent();
    if (!Test.TestNotNull(TEXT("spline actor has a USplineComponent"), SplineComp) ||
        !Test.TestNotNull(TEXT("target has a DynamicMeshComponent"), DMC))
    {
        return;
    }

    // Both actors off the identity: the bug displaced the tube by the TARGET's transform, and a
    // non-identity spline actor proves the spline's own transform is still honoured.
    constexpr double TargetScale = 1.5;
    SplineActor->SetActorTransform(FTransform(FRotator(0.0, 30.0, 0.0), FVector(-400.0, 300.0, 50.0)));
    Target->SetActorTransform(FTransform(FRotator(20.0, 90.0, 0.0), FVector(1000.0, -500.0, 200.0),
        FVector(TargetScale)));

    {
        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("actorName"), TargetLabel);
        Params->SetStringField(TEXT("splineActorName"), SplineLabel);
        Params->SetNumberField(StepsParam, 32);
        bool bOk = false;
        FString Err;
        TSharedPtr<FJsonObject> Result;
        Dispatch(Dispatcher, Sink, Method, TEXT("req-sweeplocal-run"), Params, bOk, Result, Err);
        if (!Test.TestTrue(FString::Printf(TEXT("%s succeeded (error: %s)"), Method, *Err), bOk))
        {
            return;
        }
    }

    // Default profile radius of an empty target is 50 local units; in world it is scaled by the
    // target. Slack covers the polyline-vs-curve chord error at 32 steps.
    const double MaxAllowed = 50.0 * TargetScale * 1.05 + 2.0;
    const FTransform ToWorld = DMC->GetComponentTransform();
    int32 VertexCount = 0;
    double MaxDist = 0.0;
    FVector Worst = FVector::ZeroVector;
    DMC->GetDynamicMesh()->ProcessMesh([&](const UE::Geometry::FDynamicMesh3& Mesh)
    {
        for (const int32 Vid : Mesh.VertexIndicesItr())
        {
            const FVector W = ToWorld.TransformPosition(Mesh.GetVertex(Vid));
            const FVector OnSpline = SplineComp->FindLocationClosestToWorldLocation(W, ESplineCoordinateSpace::World);
            const double Dist = FVector::Dist(W, OnSpline);
            if (Dist > MaxDist)
            {
                MaxDist = Dist;
                Worst = W;
            }
            ++VertexCount;
        }
    });

    Test.TestTrue(FString::Printf(TEXT("%s appended vertices"), Method), VertexCount > 0);
    Test.TestTrue(FString::Printf(
        TEXT("%s: every swept vertex lies within the world-space profile radius of the spline ")
        TEXT("(max %.1f at %s, allowed %.1f); world-space frames fed to the local mesh put it ")
        TEXT("off the spline by the target's transform"),
        Method, MaxDist, *Worst.ToString(), MaxAllowed),
        MaxDist <= MaxAllowed);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometrySweepSplineTargetLocalFramesTest,
    "PinWright.geometry.sweep.SplineFramesFollowSplineOffOrigin",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGeometrySweepSplineTargetLocalFramesTest::RunTest(const FString& Parameters)
{
    SweepSplineLocalFramesTest_Run(*this, TEXT("geometry.sweep"), TEXT("steps"));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryExtrudeAlongSplineTargetLocalFramesTest,
    "PinWright.geometry.extrude_along_spline.SplineFramesFollowSplineOffOrigin",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGeometryExtrudeAlongSplineTargetLocalFramesTest::RunTest(const FString& Parameters)
{
    SweepSplineLocalFramesTest_Run(*this, TEXT("geometry.extrude_along_spline"), TEXT("segments"));
    return true;
}
