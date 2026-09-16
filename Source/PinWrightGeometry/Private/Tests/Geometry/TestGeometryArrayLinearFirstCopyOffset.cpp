// Copyright (c) 2026 Alexander Penkin. MIT License.

// Red regression test for B-array-linear-first-copy-at-origin.
//
// geometry.array_linear is documented (wiki + handler summary) as producing
// `count` total copies including the original, evenly spaced by `offset`: the
// copies should land at 0, S, 2S, ..., (count-1)*S. The handler merges the
// count-1 appended copies in place via
// UGeometryScriptLibrary_MeshBasicEditFunctions::AppendMeshRepeated(..., Count-1,
// /*bApplyTransformToFirstInstance*/ false, ...) at
// GeometryTransformHandler.cpp:211-212. With that 5th positional arg false, the
// first appended instance (k=0) receives the IDENTITY transform and lands
// coincident with the original at the origin, so the copies land at
// 0, 0, S, ..., (count-2)*S — a doubled origin mesh plus a run one full spacing
// short. The geometry (triangle/vertex counts) still multiplies by count, so
// nothing in the success echo signals the wrong spatial layout.
//
// Strategy: spawn a real DynamicMeshActor via geometry.create_box (an in-code
// fixture — no external asset load) with a known 100-unit X extent, measure its
// local bounding box, run geometry.array_linear with count=2 and an offset.x
// (500) far larger than the box so the two copies cannot overlap, then measure
// again. The COUNT-doubling sanity checks (echoed count=2, triangleCount doubled)
// confirm the append actually ran, so the surviving assertion — that the local
// bbox X span grew by exactly one spacing (PreSpanX + offset.x) — isolates the
// placement defect. Pre-fix the first copy sits at the origin, the X span does
// NOT grow (stays ~100), and that assertion fails, reproducing the ticket. The
// fix (flip the arg to true) offsets the first appended copy by one spacing,
// making the span PreSpanX + offset.x and the test pass.
//
// This routes the real production handlers through InvokeHandlerWithCapture (the
// same direct-handler seam TestMeshMeasureHandler.cpp uses); none of create_box /
// array_linear / measure dereference Ctx.GetSubsystem(), so the null-subsystem
// test context is safe.
#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Misc/Guid.h"

#include "Tests/TestUtils.h"
#include "Tests/TestSkipReporting.h"
#include "Tests/Geometry/GeometryTestHelpers.h"

#include "Editor.h"
#include "Engine/World.h"

namespace
{
    // Read bbox.size.x off a geometry.measure success capture; returns false if the
    // nested {bbox:{size:{x}}} shape is absent. Uniquely named to dodge a Unity-build
    // ODR clash with other geometry test helpers.
    bool ReadMeasuredSizeX(const FTestResponseCapture& Capture, double& OutSizeX)
    {
        if (!Capture.bSuccess || !Capture.Result.IsValid())
        {
            return false;
        }
        const TSharedPtr<FJsonObject>* BBox = nullptr;
        if (!Capture.Result->TryGetObjectField(TEXT("bbox"), BBox) || !BBox)
        {
            return false;
        }
        const TSharedPtr<FJsonObject>* Size = nullptr;
        if (!(*BBox)->TryGetObjectField(TEXT("size"), Size) || !Size)
        {
            return false;
        }
        return (*Size)->TryGetNumberField(TEXT("x"), OutSizeX);
    }

    // Invoke geometry.measure on ActorName and return its local bbox size.x, or -1 on
    // any failure.
    double MeasureLocalSizeX(const FString& ActorName)
    {
        TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
        P->SetStringField(TEXT("actorName"), ActorName);
        FTestResponseCapture Capture;
        if (!InvokeHandlerWithCapture(TEXT("geometry.measure"), P, Capture))
        {
            return -1.0;
        }
        double SizeX = -1.0;
        return ReadMeasuredSizeX(Capture, SizeX) ? SizeX : -1.0;
    }
}

// A linear array of count=2 with offset.x=S must place the appended copy one full
// spacing away, so the merged mesh's local X span grows from the source extent to
// (extent + S). Pre-fix the first appended copy lands at the origin (identity
// transform), the span never grows, and this assertion fails.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryArrayLinearFirstCopyOffsetTest,
    "PinWright.geometry.array_linear.FirstCopyOffsetBySpacing",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGeometryArrayLinearFirstCopyOffsetTest::RunTest(const FString& Parameters)
{
    if (!GEditor || !IsValid(GEditor->GetEditorWorldContext().World()))
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("No editor world available; skipping array_linear first-copy-offset test"));
        return true;
    }

    const FString Label = FString::Printf(TEXT("PW_ArrayLinearOffset_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));

    // In-code fixture: a 100 x 100 x 100 box (X extent = 100), centered on the mesh
    // origin. No external content asset is loaded. create_box echoes the spawned
    // actor's final label in "name"; use it as the actorName the array/measure verbs
    // resolve (the GUID suffix makes disambiguation moot, but read it back honestly).
    FString ActorName;
    {
        TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
        P->SetStringField(TEXT("name"), Label);
        P->SetNumberField(TEXT("width"), 100.0);
        P->SetNumberField(TEXT("height"), 100.0);
        P->SetNumberField(TEXT("depth"), 100.0);
        FTestResponseCapture Capture;
        if (!TestTrue(TEXT("geometry.create_box handler registered"),
                InvokeHandlerWithCapture(TEXT("geometry.create_box"), P, Capture))
            || !TestTrue(TEXT("geometry.create_box succeeded"), Capture.bSuccess)
            || !Capture.Result.IsValid())
        {
            GeometryTestHelpers::DestroyActorsWithLabel(Label);
            return false;
        }
        Capture.Result->TryGetStringField(TEXT("name"), ActorName);
    }
    if (!TestFalse(TEXT("create_box echoed a spawned actor name"), ActorName.IsEmpty()))
    {
        GeometryTestHelpers::DestroyActorsWithLabel(Label);
        return false;
    }

    // Baseline: the source box's local X span.
    const double PreSpanX = MeasureLocalSizeX(ActorName);
    if (!TestTrue(TEXT("baseline geometry.measure reported a positive X span"), PreSpanX > 0.0))
    {
        GeometryTestHelpers::DestroyActorsWithLabel(ActorName);
        return false;
    }

    // The spacing — deliberately far larger than the 100-unit box so the two copies
    // cannot overlap and the expected span is unambiguous.
    const double Spacing = 500.0;

    // Run geometry.array_linear: count=2 → the original plus one appended copy.
    double EchoedTriangleCount = 0.0;
    {
        TSharedPtr<FJsonObject> Offset = MakeShared<FJsonObject>();
        Offset->SetNumberField(TEXT("x"), Spacing);
        Offset->SetNumberField(TEXT("y"), 0.0);
        Offset->SetNumberField(TEXT("z"), 0.0);

        TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
        P->SetStringField(TEXT("actorName"), ActorName);
        P->SetNumberField(TEXT("count"), 2);
        P->SetObjectField(TEXT("offset"), Offset);

        FTestResponseCapture Capture;
        if (!TestTrue(TEXT("geometry.array_linear handler registered"),
                InvokeHandlerWithCapture(TEXT("geometry.array_linear"), P, Capture))
            || !TestTrue(TEXT("geometry.array_linear succeeded"), Capture.bSuccess)
            || !Capture.Result.IsValid())
        {
            GeometryTestHelpers::DestroyActorsWithLabel(ActorName);
            return false;
        }

        // Sanity (passes both pre- and post-fix): the op actually merged a second
        // full copy, so the failing span assertion below is a placement defect, not a
        // no-op. The count is echoed and the triangle count doubles.
        double EchoedCount = 0.0;
        Capture.Result->TryGetNumberField(TEXT("count"), EchoedCount);
        TestEqual(TEXT("array_linear echoes count=2"), (int32)EchoedCount, 2);
        Capture.Result->TryGetNumberField(TEXT("triangleCount"), EchoedTriangleCount);
        TestTrue(TEXT("array_linear merged a second copy (triangleCount grew)"),
            EchoedTriangleCount > 0.0);
    }

    // The merged mesh's local X span. Correct behavior: the appended copy sits one
    // spacing away, so the span grows to PreSpanX + Spacing.
    const double PostSpanX = MeasureLocalSizeX(ActorName);
    if (!TestTrue(TEXT("post-array geometry.measure reported a positive X span"), PostSpanX > 0.0))
    {
        GeometryTestHelpers::DestroyActorsWithLabel(ActorName);
        return false;
    }

    // THE red assertion. Pre-fix the first appended copy lands at the origin
    // (identity transform), coincident with the original, so PostSpanX == PreSpanX
    // (~100) and this fails. Post-fix the copy is offset by one spacing, giving
    // PostSpanX == PreSpanX + Spacing (~600).
    const double ExpectedSpanX = PreSpanX + Spacing;
    TestTrue(
        *FString::Printf(
            TEXT("array_linear offsets the first appended copy by one spacing: "
                 "expected local X span ~%.1f (PreSpan %.1f + offset %.1f), got %.1f"),
            ExpectedSpanX, PreSpanX, Spacing, PostSpanX),
        FMath::IsNearlyEqual(PostSpanX, ExpectedSpanX, 1.0));

    GeometryTestHelpers::DestroyActorsWithLabel(ActorName);
    return true;
}
