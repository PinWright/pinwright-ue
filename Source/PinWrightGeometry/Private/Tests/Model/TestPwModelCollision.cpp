// Copyright (c) 2026 Alexander Penkin. MIT License.

// Unit tests for BuildCollision - the .pwmodel `collision` block turned into an FKAggregateGeom.
//
// Collision is the one part of a compiled model that is invisible in a render: a wrong box extent,
// a capsule that is silently half the requested height, or a hull that quietly kept its concavity
// all produce an asset that looks perfect and behaves wrongly. Every test here pins a conversion
// that has no visual tell:
//
//   1. `capsule height=` is TOTAL height; FKSphylElem::Length is the cylindrical segment only
//      (SphylElem.h:38). Drop the -2*radius and every authored capsule is silently 2*radius too
//      long, which is exactly the sort of error that ships.
//   2. `hull { … }` discards concavity. HullDiscardsConcavity pins that by construction rather
//      than by documentation: a box with an interior cavity hulls to the same element as the
//      plain box. Anyone who "fixes" hull to preserve the cut breaks this test instead of
//      shipping collision that no longer matches the spec.
//   3. `box size=` is FULL extents, not half-extents - the single most common collision mistake.
//   4. `rotate=` is (roll, pitch, yaw), which is NOT FRotator's constructor order. Reorder the
//      tuple and a rotated element is wrong in a way no count or element type reveals.
//   5. The complexity and auto-method vocabularies are derived from their engine enums, and the
//      valid-entry list from the op table. The tests assert the derived spellings, so a
//      derivation that starts emitting `use_simple_and_complex` fails here rather than silently
//      rejecting every document that says `simple_and_complex`.
//   6. Clamping WARNS (contract rule 5). `simplify_to` and `max_hulls_per_component` are both
//      raised to a floor, and a silent raise means the number that ran is not the number the
//      author wrote.
//   8. `max_hulls_per_component` is spent PER CONNECTED COMPONENT, never per asset. The engine
//      splits the merged mesh into components before decomposing and gives each one the whole
//      budget, so the element count is components * budget. That is the semantics the parameter
//      was renamed to state, and AutoHullBudgetIsPerConnectedComponent is what stops a future
//      reading of the old name from quietly coming back.
//   7. `auto` produces a shape of a particular TYPE. The format forces bAutoDetectBoxes /
//      Spheres / Capsules to false against engine defaults of true, so `method=convex_hulls` on
//      a box mesh must come back as an FKConvexElem and not an FKBoxElem - a difference no
//      element count, warning or diagnostic reveals. `convex` is exact for the same reason in
//      reverse: its points are stored verbatim, which only a point set larger than its own hull
//      can distinguish from a re-hull.
#include "Misc/AutomationTest.h"
#include "Compat/EngineVersionCompat.h"

#include "Model/PwModelAst.h"
#include "Model/PwModelCollision.h"
#include "Model/PwModelDiagnostic.h"
#include "Model/PwModelParser.h"

#include "UDynamicMesh.h"
#include "UObject/Package.h"

#include "GeometryScript/MeshBooleanFunctions.h"
#include "GeometryScript/MeshPrimitiveFunctions.h"

namespace
{
// Uniquely prefixed: this module builds with bUseUnity = true, so an anonymous-namespace helper
// with a common name would collide with a sibling test TU once Unity merges them.

FPwValue PwModelCollisionTest_Number(double Number)
{
    FPwValue Value;
    Value.Type = EPwValueType::Number;
    Value.Number = Number;
    return Value;
}

FPwValue PwModelCollisionTest_Tuple(double X, double Y, double Z)
{
    FPwValue Value;
    Value.Type = EPwValueType::Tuple;
    Value.Tuple = { X, Y, Z };
    return Value;
}

FPwValue PwModelCollisionTest_Identifier(const TCHAR* Text)
{
    FPwValue Value;
    Value.Type = EPwValueType::Identifier;
    Value.Text = Text;
    return Value;
}

FPwValue PwModelCollisionTest_PointList(const TArray<TArray<double>>& Points)
{
    FPwValue Value;
    Value.Type = EPwValueType::TupleList;
    Value.TupleList = Points;
    return Value;
}

FPwOp PwModelCollisionTest_Op(const TCHAR* OpName)
{
    FPwOp Op;
    Op.OpName = OpName;
    Op.Line = 7;
    Op.Column = 5;
    return Op;
}

UDynamicMesh* PwModelCollisionTest_NewMesh()
{
    return NewObject<UDynamicMesh>(GetTransientPackage());
}

void PwModelCollisionTest_AppendBox(UDynamicMesh* Mesh, const FVector& Center, double Dimension)
{
    FGeometryScriptPrimitiveOptions Options;
    UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendBox(
        Mesh, Options, FTransform(Center),
        static_cast<float>(Dimension), static_cast<float>(Dimension), static_cast<float>(Dimension),
        0, 0, 0,
        EGeometryScriptPrimitiveOriginMode::Center, nullptr);
}

void PwModelCollisionTest_AppendSphere(UDynamicMesh* Mesh, const FVector& Center, double Radius)
{
    FGeometryScriptPrimitiveOptions Options;
    UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendSphereLatLong(
        Mesh, Options, FTransform(Center), static_cast<float>(Radius), 16, 16,
        EGeometryScriptPrimitiveOriginMode::Center, nullptr);
}

// A `hull` body that always yields the same box. Signature matches FBuildMeshFromOps.
bool PwModelCollisionTest_BoxStub(const TArray<FPwOp>&, UDynamicMesh* Out, TArray<FString>&)
{
    PwModelCollisionTest_AppendBox(Out, FVector::ZeroVector, 100.0);
    return true;
}

bool PwModelCollisionTest_EmptyStub(const TArray<FPwOp>&, UDynamicMesh*, TArray<FString>&)
{
    return true;
}

bool PwModelCollisionTest_FailingStub(const TArray<FPwOp>&, UDynamicMesh*, TArray<FString>& OutErrors)
{
    OutErrors.Add(TEXT("stub refused"));
    return false;
}

FString PwModelCollisionTest_Describe(const FPwModelCollisionResult& Result)
{
    return FString::Printf(TEXT("success=%d code=%s message=%s warnings=[%s]"),
        Result.bSuccess ? 1 : 0, *Result.ErrorCode, *Result.ErrorMessage,
        *FString::Join(Result.Warnings, TEXT(" | ")));
}

// True when every vertex of B has a match in A within Tolerance and the counts agree, i.e. the
// two hulls are the same point set.
bool PwModelCollisionTest_SameVertexSet(const TArray<FVector>& A, const TArray<FVector>& B, double Tolerance)
{
    if (A.Num() != B.Num())
    {
        return false;
    }

    for (const FVector& Point : B)
    {
        bool bFound = false;
        for (const FVector& Candidate : A)
        {
            if (FVector::Dist(Point, Candidate) <= Tolerance)
            {
                bFound = true;
                break;
            }
        }
        if (!bFound)
        {
            return false;
        }
    }

    return true;
}
}

// ============================================================================
// Explicit primitives
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwModelCollisionPrimitivesTest,
    "PinWright.Model.Collision.ThreePrimitivesGiveThreeElements",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPwModelCollisionPrimitivesTest::RunTest(const FString& Parameters)
{
    FPwModelCollision Block;

    FPwOp Box = PwModelCollisionTest_Op(TEXT("box"));
    Box.Params.Add(TEXT("size"), PwModelCollisionTest_Tuple(20.0, 30.0, 40.0));
    Box.Params.Add(TEXT("at"), PwModelCollisionTest_Tuple(1.0, 2.0, 3.0));
    // (roll, pitch, yaw) - deliberately three different values so a reordered read is visible.
    Box.Params.Add(TEXT("rotate"), PwModelCollisionTest_Tuple(10.0, 20.0, 30.0));
    Block.Elements.Add(Box);

    FPwOp Sphere = PwModelCollisionTest_Op(TEXT("sphere"));
    Sphere.Params.Add(TEXT("radius"), PwModelCollisionTest_Number(5.0));
    Sphere.Params.Add(TEXT("at"), PwModelCollisionTest_Tuple(0.0, 0.0, 10.0));
    Block.Elements.Add(Sphere);

    FPwOp Capsule = PwModelCollisionTest_Op(TEXT("capsule"));
    Capsule.Params.Add(TEXT("radius"), PwModelCollisionTest_Number(4.0));
    Capsule.Params.Add(TEXT("height"), PwModelCollisionTest_Number(20.0));
    Block.Elements.Add(Capsule);

    const FPwModelCollisionResult Result = BuildCollision(Block, nullptr, PwModelCollisionTest_BoxStub);

    TestTrue(*FString::Printf(TEXT("build succeeded. %s"), *PwModelCollisionTest_Describe(Result)), Result.bSuccess);
    TestEqual(TEXT("box elements"), Result.Geom.BoxElems.Num(), 1);
    TestEqual(TEXT("sphere elements"), Result.Geom.SphereElems.Num(), 1);
    TestEqual(TEXT("sphyl elements"), Result.Geom.SphylElems.Num(), 1);
    TestEqual(TEXT("elements written"), Result.ElementsWritten, 3);

    if (Result.Geom.BoxElems.Num() == 1)
    {
        const FKBoxElem& Elem = Result.Geom.BoxElems[0];
        // Full extents, not half-extents.
        TestEqual(TEXT("box X is the full extent"), Elem.X, 20.0f);
        TestEqual(TEXT("box Y is the full extent"), Elem.Y, 30.0f);
        TestEqual(TEXT("box Z is the full extent"), Elem.Z, 40.0f);
        TestTrue(TEXT("box centre came from at="), Elem.Center.Equals(FVector(1.0, 2.0, 3.0)));
        TestEqual(TEXT("rotate tuple element 0 is roll"), Elem.Rotation.Roll, 10.0);
        TestEqual(TEXT("rotate tuple element 1 is pitch"), Elem.Rotation.Pitch, 20.0);
        TestEqual(TEXT("rotate tuple element 2 is yaw"), Elem.Rotation.Yaw, 30.0);
    }

    if (Result.Geom.SphereElems.Num() == 1)
    {
        TestEqual(TEXT("sphere radius"), Result.Geom.SphereElems[0].Radius, 5.0f);
        TestTrue(TEXT("sphere centre"), Result.Geom.SphereElems[0].Center.Equals(FVector(0.0, 0.0, 10.0)));
    }

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwModelCollisionSphylLengthTest,
    "PinWright.Model.Collision.CapsuleHeightConvertsToSphylLength",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPwModelCollisionSphylLengthTest::RunTest(const FString& Parameters)
{
    FPwModelCollision Block;

    FPwOp Capsule = PwModelCollisionTest_Op(TEXT("capsule"));
    Capsule.Params.Add(TEXT("radius"), PwModelCollisionTest_Number(10.0));
    Capsule.Params.Add(TEXT("height"), PwModelCollisionTest_Number(60.0));
    // A capsule's placement is read by the same two lines as a box's and is pinned nowhere else:
    // the box test covers FKBoxElem::Center/Rotation, so dropping FKSphylElem's pair moves every
    // authored capsule to the origin with no count, element type or diagnostic changing.
    Capsule.Params.Add(TEXT("at"), PwModelCollisionTest_Tuple(1.0, 2.0, 3.0));
    Capsule.Params.Add(TEXT("rotate"), PwModelCollisionTest_Tuple(10.0, 20.0, 30.0));
    Block.Elements.Add(Capsule);

    const FPwModelCollisionResult Result = BuildCollision(Block, nullptr, PwModelCollisionTest_BoxStub);

    TestTrue(*FString::Printf(TEXT("build succeeded. %s"), *PwModelCollisionTest_Describe(Result)), Result.bSuccess);
    if (!TestEqual(TEXT("one sphyl"), Result.Geom.SphylElems.Num(), 1))
    {
        return false;
    }

    const FKSphylElem& Elem = Result.Geom.SphylElems[0];
    TestEqual(TEXT("Length excludes both hemispherical caps"), Elem.Length, 40.0f);
    TestEqual(TEXT("Radius is the authored radius"), Elem.Radius, 10.0f);
    // The round-trip the author actually cares about.
    TestEqual(TEXT("Length + 2 * Radius is the requested total height"), Elem.Length + 2.0f * Elem.Radius, 60.0f);
    TestTrue(TEXT("capsule centre came from at="), Elem.Center.Equals(FVector(1.0, 2.0, 3.0)));
    TestEqual(TEXT("capsule rotate tuple element 0 is roll"), Elem.Rotation.Roll, 10.0);
    TestEqual(TEXT("capsule rotate tuple element 1 is pitch"), Elem.Rotation.Pitch, 20.0);
    TestEqual(TEXT("capsule rotate tuple element 2 is yaw"), Elem.Rotation.Yaw, 30.0);
    TestEqual(TEXT("no warning for a representable capsule"), Result.Warnings.Num(), 0);

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwModelCollisionSphylClampTest,
    "PinWright.Model.Collision.CapsuleShorterThanDiameterClampsAndWarns",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPwModelCollisionSphylClampTest::RunTest(const FString& Parameters)
{
    FPwModelCollision Block;

    FPwOp Capsule = PwModelCollisionTest_Op(TEXT("capsule"));
    Capsule.Params.Add(TEXT("radius"), PwModelCollisionTest_Number(10.0));
    Capsule.Params.Add(TEXT("height"), PwModelCollisionTest_Number(15.0));
    Block.Elements.Add(Capsule);

    const FPwModelCollisionResult Result = BuildCollision(Block, nullptr, PwModelCollisionTest_BoxStub);

    TestTrue(*FString::Printf(TEXT("build succeeded. %s"), *PwModelCollisionTest_Describe(Result)), Result.bSuccess);
    if (!TestEqual(TEXT("one sphyl"), Result.Geom.SphylElems.Num(), 1))
    {
        return false;
    }

    TestEqual(TEXT("Length clamps to 0 rather than going negative"), Result.Geom.SphylElems[0].Length, 0.0f);
    // The clamp leaves a sphere of the authored radius, which is the whole content of the
    // warning's promise: without this the element could collapse to nothing and still "clamp".
    TestEqual(TEXT("the clamped capsule keeps its radius"), Result.Geom.SphylElems[0].Radius, 10.0f);

    // A count alone is satisfied by ANY warning - and this fixture would grow one the moment the
    // block gained a complexity or an unrelated clamp. The clamp's own message is what makes the
    // silent raise visible to an author, so it is what gets asserted.
    const FString Warnings = FString::Join(Result.Warnings, TEXT(" | "));
    TestTrue(*FString::Printf(TEXT("the clamp names the height it could not represent. %s"),
        *PwModelCollisionTest_Describe(Result)), Warnings.Contains(TEXT("height=15")));
    TestTrue(*FString::Printf(TEXT("the clamp names the diameter it fell below. %s"),
        *PwModelCollisionTest_Describe(Result)), Warnings.Contains(TEXT("2 * radius (20)")));

    return true;
}

// ============================================================================
// Hulls
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwModelCollisionHullTest,
    "PinWright.Model.Collision.HullBlockYieldsOneConvexElem",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPwModelCollisionHullTest::RunTest(const FString& Parameters)
{
    FPwModelCollision Block;

    FPwOp Hull = PwModelCollisionTest_Op(TEXT("hull"));
    Hull.Children.Add(PwModelCollisionTest_Op(TEXT("box")));
    Block.Elements.Add(Hull);

    // The block's children must reach the callback: a hull built from ops nobody ran is an empty
    // hull that reports success.
    TArray<FString> SeenOps;
    auto Stub = [&SeenOps](const TArray<FPwOp>& Ops, UDynamicMesh* Out, TArray<FString>&)
    {
        for (const FPwOp& Op : Ops)
        {
            SeenOps.Add(Op.OpName);
        }
        PwModelCollisionTest_AppendBox(Out, FVector::ZeroVector, 100.0);
        return true;
    };

    const FPwModelCollisionResult Result = BuildCollision(Block, nullptr, Stub);

    TestTrue(*FString::Printf(TEXT("build succeeded. %s"), *PwModelCollisionTest_Describe(Result)), Result.bSuccess);
    TestEqual(TEXT("the hull body's ops reached the callback"), SeenOps.Num(), 1);
    if (SeenOps.Num() == 1)
    {
        // The children, not the block's own entry: passing `Element` instead of `Element.Children`
        // is a one-word slip that still hands the callback exactly one op.
        TestEqual(TEXT("the op passed through is the hull body's child"), SeenOps[0], FString(TEXT("box")));
    }
    if (!TestEqual(TEXT("one convex element"), Result.Geom.ConvexElems.Num(), 1))
    {
        return false;
    }

    // The stub's mesh is a box, which is convex, so every one of its vertices is a hull vertex.
    UDynamicMesh* Reference = PwModelCollisionTest_NewMesh();
    PwModelCollisionTest_AppendBox(Reference, FVector::ZeroVector, 100.0);
    const int32 ReferenceVertexCount = Reference->GetMeshRef().VertexCount();
    Reference->MarkAsGarbage();

    TestEqual(TEXT("a convex source keeps all its vertices in the hull"),
        Result.Geom.ConvexElems[0].VertexData.Num(), ReferenceVertexCount);
    TestTrue(TEXT("ElemBox spans the source box"),
        Result.Geom.ConvexElems[0].ElemBox.GetSize().Equals(FVector(100.0, 100.0, 100.0), 0.01));

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwModelCollisionTwoHullsTest,
    "PinWright.Model.Collision.TwoHullBlocksYieldTwoConvexElems",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPwModelCollisionTwoHullsTest::RunTest(const FString& Parameters)
{
    FPwModelCollision Block;

    FPwOp First = PwModelCollisionTest_Op(TEXT("hull"));
    First.Children.Add(PwModelCollisionTest_Op(TEXT("box")));
    Block.Elements.Add(First);

    FPwOp Second = PwModelCollisionTest_Op(TEXT("hull"));
    Second.Children.Add(PwModelCollisionTest_Op(TEXT("box")));
    Block.Elements.Add(Second);

    // Several hull blocks are the hand-authored convex-decomposition path, so each must land as
    // its own element rather than merging into one.
    int32 Call = 0;
    auto Stub = [&Call](const TArray<FPwOp>&, UDynamicMesh* Out, TArray<FString>&)
    {
        PwModelCollisionTest_AppendBox(Out, FVector(Call++ * 200.0, 0.0, 0.0), 100.0);
        return true;
    };

    const FPwModelCollisionResult Result = BuildCollision(Block, nullptr, Stub);

    TestTrue(*FString::Printf(TEXT("build succeeded. %s"), *PwModelCollisionTest_Describe(Result)), Result.bSuccess);
    TestEqual(TEXT("two convex elements"), Result.Geom.ConvexElems.Num(), 2);
    TestEqual(TEXT("elements written"), Result.ElementsWritten, 2);
    TestEqual(TEXT("the callback ran once per hull block"), Call, 2);

    // Counting the elements does not say WHICH mesh each was hulled from. The stub places its two
    // boxes 200 apart precisely so that a scratch mesh hoisted out of the loop - the obvious
    // "avoid a NewObject per hull" edit - is visible: both hulls would then span both boxes,
    // giving two identical 300-long elements while the count, ElementsWritten and the callback
    // tally all stay exactly as asserted above.
    if (Result.Geom.ConvexElems.Num() == 2)
    {
        const FKConvexElem& FirstHull = Result.Geom.ConvexElems[0];
        const FKConvexElem& SecondHull = Result.Geom.ConvexElems[1];

        TestTrue(*FString::Printf(TEXT("the first hull is the first body's box alone (centre %s, size %s)"),
                *FirstHull.ElemBox.GetCenter().ToString(), *FirstHull.ElemBox.GetSize().ToString()),
            FirstHull.ElemBox.GetCenter().Equals(FVector::ZeroVector, 0.01)
                && FirstHull.ElemBox.GetSize().Equals(FVector(100.0, 100.0, 100.0), 0.01));

        TestTrue(*FString::Printf(TEXT("the second hull is the second body's box alone (centre %s, size %s)"),
                *SecondHull.ElemBox.GetCenter().ToString(), *SecondHull.ElemBox.GetSize().ToString()),
            SecondHull.ElemBox.GetCenter().Equals(FVector(200.0, 0.0, 0.0), 0.01)
                && SecondHull.ElemBox.GetSize().Equals(FVector(100.0, 100.0, 100.0), 0.01));
    }

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwModelCollisionHullConcavityTest,
    "PinWright.Model.Collision.HullDiscardsConcavity",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPwModelCollisionHullConcavityTest::RunTest(const FString& Parameters)
{
    FPwModelCollision Block;
    FPwOp Hull = PwModelCollisionTest_Op(TEXT("hull"));
    Hull.Children.Add(PwModelCollisionTest_Op(TEXT("box")));
    Block.Elements.Add(Hull);

    const FPwModelCollisionResult PlainResult = BuildCollision(Block, nullptr, PwModelCollisionTest_BoxStub);

    // Same box with a sphere carved out of its interior: a genuinely concave surface whose convex
    // hull is still the box. This is the documented semantic of `hull { … }` and the reason the
    // keyword is not called `convex`.
    auto CarvedStub = [](const TArray<FPwOp>&, UDynamicMesh* Out, TArray<FString>&)
    {
        PwModelCollisionTest_AppendBox(Out, FVector::ZeroVector, 100.0);

        UDynamicMesh* Tool = PwModelCollisionTest_NewMesh();
        PwModelCollisionTest_AppendSphere(Tool, FVector::ZeroVector, 30.0);

        FGeometryScriptMeshBooleanOptions BoolOptions;
        UGeometryScriptLibrary_MeshBooleanFunctions::ApplyMeshBoolean(
            Out, FTransform::Identity, Tool, FTransform::Identity,
            EGeometryScriptBooleanOperation::Subtract, BoolOptions, nullptr);

        Tool->MarkAsGarbage();
        return true;
    };

    const FPwModelCollisionResult CarvedResult = BuildCollision(Block, nullptr, CarvedStub);

    TestTrue(*FString::Printf(TEXT("plain build succeeded. %s"), *PwModelCollisionTest_Describe(PlainResult)),
        PlainResult.bSuccess);
    TestTrue(*FString::Printf(TEXT("carved build succeeded. %s"), *PwModelCollisionTest_Describe(CarvedResult)),
        CarvedResult.bSuccess);

    if (PlainResult.Geom.ConvexElems.Num() != 1 || CarvedResult.Geom.ConvexElems.Num() != 1)
    {
        AddError(TEXT("expected exactly one convex element from each build"));
        return false;
    }

    const FKConvexElem& Plain = PlainResult.Geom.ConvexElems[0];
    const FKConvexElem& Carved = CarvedResult.Geom.ConvexElems[0];

    TestTrue(*FString::Printf(TEXT("the carved mesh hulls to the same point set as the plain box (%d vs %d vertices)"),
        Plain.VertexData.Num(), Carved.VertexData.Num()),
        PwModelCollisionTest_SameVertexSet(Plain.VertexData, Carved.VertexData, 0.01));

    TestTrue(TEXT("the carved hull spans the same box"),
        Plain.ElemBox.GetSize().Equals(Carved.ElemBox.GetSize(), 0.01));

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwModelCollisionEmptyHullTest,
    "PinWright.Model.Collision.EmptyHullBodyErrors",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPwModelCollisionEmptyHullTest::RunTest(const FString& Parameters)
{
    FPwModelCollision Block;
    FPwOp Hull = PwModelCollisionTest_Op(TEXT("hull"));
    Hull.Children.Add(PwModelCollisionTest_Op(TEXT("box")));
    Block.Elements.Add(Hull);

    const FPwModelCollisionResult Empty = BuildCollision(Block, nullptr, PwModelCollisionTest_EmptyStub);
    TestFalse(TEXT("a hull body that produces no geometry fails"), Empty.bSuccess);
    TestEqual(TEXT("empty hull body code"), Empty.ErrorCode,
        FString(PwModelDiagnosticCodes::PWMODEL_DEGENERATE_HULL));
    TestEqual(TEXT("the diagnostic is anchored on the hull entry"), Empty.ErrorLine, 7);
    TestEqual(TEXT("no elements survive a failure"), Empty.ElementsWritten, 0);

    const FPwModelCollisionResult Failed = BuildCollision(Block, nullptr, PwModelCollisionTest_FailingStub);
    TestFalse(TEXT("a hull body whose ops fail fails"), Failed.bSuccess);
    TestEqual(TEXT("failed hull body code"), Failed.ErrorCode,
        FString(PwModelDiagnosticCodes::PWMODEL_DEGENERATE_HULL));
    TestTrue(*FString::Printf(TEXT("the callback's reason is carried through. %s"),
        *PwModelCollisionTest_Describe(Failed)), Failed.ErrorMessage.Contains(TEXT("stub refused")));

    // Both blocks above fail on their FIRST element, so nothing had accumulated and the
    // discard-on-failure is trivially satisfied - ElementsWritten is a counter the failure path
    // sets to 0 by hand, not the geometry itself. A box AHEAD of the failing hull is the only
    // arrangement where the two can disagree: drop Geom.EmptyElements() from the failure path and
    // the caller receives a half-built aggregate alongside an error, which is a collision body
    // the author never described.
    FPwModelCollision Partial;
    FPwOp Box = PwModelCollisionTest_Op(TEXT("box"));
    Box.Params.Add(TEXT("size"), PwModelCollisionTest_Tuple(20.0, 30.0, 40.0));
    Partial.Elements.Add(Box);
    FPwOp LateHull = PwModelCollisionTest_Op(TEXT("hull"));
    LateHull.Children.Add(PwModelCollisionTest_Op(TEXT("box")));
    Partial.Elements.Add(LateHull);

    const FPwModelCollisionResult Discarded = BuildCollision(Partial, nullptr, PwModelCollisionTest_EmptyStub);
    TestFalse(TEXT("a later failing hull still fails the block"), Discarded.bSuccess);
    TestEqual(*FString::Printf(TEXT("the geometry built before the failure is discarded. %s"),
        *PwModelCollisionTest_Describe(Discarded)), Discarded.Geom.GetElementCount(), 0);
    TestEqual(TEXT("no elements are reported either"), Discarded.ElementsWritten, 0);

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwModelCollisionConvexPointsTest,
    "PinWright.Model.Collision.ConvexPointsNeedFourPoints",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPwModelCollisionConvexPointsTest::RunTest(const FString& Parameters)
{
    FPwModelCollision TooFew;
    FPwOp Convex = PwModelCollisionTest_Op(TEXT("convex"));
    Convex.Params.Add(TEXT("points"), PwModelCollisionTest_PointList({
        { 0.0, 0.0, 0.0 }, { 10.0, 0.0, 0.0 }, { 0.0, 10.0, 0.0 } }));
    TooFew.Elements.Add(Convex);

    const FPwModelCollisionResult Rejected = BuildCollision(TooFew, nullptr, PwModelCollisionTest_BoxStub);
    TestFalse(TEXT("three points bound no volume"), Rejected.bSuccess);
    TestEqual(TEXT("degenerate hull code"), Rejected.ErrorCode,
        FString(PwModelDiagnosticCodes::PWMODEL_DEGENERATE_HULL));

    FPwModelCollision Tetrahedron;
    FPwOp Valid = PwModelCollisionTest_Op(TEXT("convex"));
    Valid.Params.Add(TEXT("points"), PwModelCollisionTest_PointList({
        { 0.0, 0.0, 0.0 }, { 10.0, 0.0, 0.0 }, { 0.0, 10.0, 0.0 }, { 0.0, 0.0, 10.0 } }));
    Tetrahedron.Elements.Add(Valid);

    const FPwModelCollisionResult Accepted = BuildCollision(Tetrahedron, nullptr, PwModelCollisionTest_BoxStub);
    TestTrue(*FString::Printf(TEXT("four points build. %s"), *PwModelCollisionTest_Describe(Accepted)),
        Accepted.bSuccess);
    if (Accepted.Geom.ConvexElems.Num() == 1)
    {
        TestEqual(TEXT("all four points are stored"), Accepted.Geom.ConvexElems[0].VertexData.Num(), 4);
    }
    else
    {
        AddError(TEXT("expected one convex element"));
    }

    // A tetrahedron IS its own hull, so the fixture above passes whether the points are stored
    // verbatim or pushed through the same hull reduction `hull { … }` uses - the two are
    // indistinguishable on it. One point strictly inside the tetrahedron separates them: exact
    // storage keeps five, a re-hull silently drops the interior one, which is precisely the
    // caller's own decomposition being discarded.
    FPwModelCollision WithInterior;
    FPwOp Redundant = PwModelCollisionTest_Op(TEXT("convex"));
    Redundant.Params.Add(TEXT("points"), PwModelCollisionTest_PointList({
        { 0.0, 0.0, 0.0 }, { 10.0, 0.0, 0.0 }, { 0.0, 10.0, 0.0 }, { 0.0, 0.0, 10.0 },
        { 1.0, 1.0, 1.0 } }));
    WithInterior.Elements.Add(Redundant);

    const FPwModelCollisionResult Exact = BuildCollision(WithInterior, nullptr, PwModelCollisionTest_BoxStub);
    TestTrue(*FString::Printf(TEXT("five points build. %s"), *PwModelCollisionTest_Describe(Exact)),
        Exact.bSuccess);
    if (Exact.Geom.ConvexElems.Num() == 1)
    {
        const FKConvexElem& Elem = Exact.Geom.ConvexElems[0];
        TestEqual(TEXT("the interior point is stored rather than hulled away"), Elem.VertexData.Num(), 5);
        // Chaos hulls VertexData at cook time, so IndexData is what a nav-mesh build and every
        // in-editor draw read. Computed here or nowhere.
        TestTrue(*FString::Printf(TEXT("the hull indices are computed (%d)"), Elem.IndexData.Num()),
            Elem.IndexData.Num() > 0);
        // UpdateElemBox is the only thing that makes ElemBox anything but the default box.
        TestTrue(*FString::Printf(TEXT("ElemBox spans the point set (%s)"), *Elem.ElemBox.GetSize().ToString()),
            Elem.ElemBox.GetSize().Equals(FVector(10.0, 10.0, 10.0), 0.01));
    }
    else
    {
        AddError(TEXT("expected one convex element from the five-point block"));
    }

    return true;
}

// ============================================================================
// complexity
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwModelCollisionComplexityTest,
    "PinWright.Model.Collision.ComplexityValuesMapToTraceFlags",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPwModelCollisionComplexityTest::RunTest(const FString& Parameters)
{
    struct FCase
    {
        const TCHAR* Name;
        ECollisionTraceFlag Flag;
    };

    const FCase Cases[] = {
        { TEXT("simple_and_complex"), ECollisionTraceFlag::CTF_UseSimpleAndComplex },
        { TEXT("simple_as_complex"),  ECollisionTraceFlag::CTF_UseSimpleAsComplex },
        { TEXT("complex_as_simple"),  ECollisionTraceFlag::CTF_UseComplexAsSimple },
        { TEXT("use_default"),        ECollisionTraceFlag::CTF_UseDefault },
    };

    for (const FCase& Case : Cases)
    {
        FPwModelCollision Block;
        Block.Complexity = PwModelCollisionTest_Identifier(Case.Name);

        const FPwModelCollisionResult Result = BuildCollision(Block, nullptr, PwModelCollisionTest_BoxStub);

        TestTrue(*FString::Printf(TEXT("'%s' builds. %s"), Case.Name, *PwModelCollisionTest_Describe(Result)),
            Result.bSuccess);
        TestEqual(*FString::Printf(TEXT("'%s' maps to its trace flag"), Case.Name),
            static_cast<int32>(Result.Trace), static_cast<int32>(Case.Flag));
    }

    // The derived vocabulary is what the parser and the spec both publish; a derivation that
    // starts emitting `use_simple_and_complex` would reject every document that says otherwise.
    const TArray<FString>& Names = PwModelCollisionNames::ComplexityNames();
    TestEqual(TEXT("four complexity spellings"), Names.Num(), 4);
    for (const FCase& Case : Cases)
    {
        TestTrue(*FString::Printf(TEXT("'%s' is in the derived vocabulary: [%s]"),
            Case.Name, *FString::Join(Names, TEXT(", "))), Names.Contains(FString(Case.Name)));
    }

    FPwModelCollision Unknown;
    Unknown.Complexity = PwModelCollisionTest_Identifier(TEXT("use_simple_and_complex"));
    const FPwModelCollisionResult Rejected = BuildCollision(Unknown, nullptr, PwModelCollisionTest_BoxStub);
    TestFalse(TEXT("an engine-spelled flag is not accepted"), Rejected.bSuccess);
    TestEqual(TEXT("unknown complexity code"), Rejected.ErrorCode,
        FString(PwSourceDiagnosticCodes::PWSRC_BAD_VALUE));

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwModelCollisionDefaultComplexityTest,
    "PinWright.Model.Collision.OmittedComplexityIsSimpleAndComplex",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPwModelCollisionDefaultComplexityTest::RunTest(const FString& Parameters)
{
    FPwModelCollision Block;
    FPwOp Sphere = PwModelCollisionTest_Op(TEXT("sphere"));
    Sphere.Params.Add(TEXT("radius"), PwModelCollisionTest_Number(5.0));
    Block.Elements.Add(Sphere);

    const FPwModelCollisionResult Result = BuildCollision(Block, nullptr, PwModelCollisionTest_BoxStub);

    TestTrue(*FString::Printf(TEXT("build succeeded. %s"), *PwModelCollisionTest_Describe(Result)), Result.bSuccess);
    TestEqual(TEXT("an omitted complexity is the format's default, not the project's"),
        static_cast<int32>(Result.Trace), static_cast<int32>(ECollisionTraceFlag::CTF_UseSimpleAndComplex));

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwModelCollisionComplexAsSimpleWarnsTest,
    "PinWright.Model.Collision.ComplexAsSimpleWarns",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPwModelCollisionComplexAsSimpleWarnsTest::RunTest(const FString& Parameters)
{
    FPwModelCollision Bare;
    Bare.Complexity = PwModelCollisionTest_Identifier(TEXT("complex_as_simple"));

    const FPwModelCollisionResult BareResult = BuildCollision(Bare, nullptr, PwModelCollisionTest_BoxStub);
    TestTrue(*FString::Printf(TEXT("complex_as_simple warns on its own. %s"),
        *PwModelCollisionTest_Describe(BareResult)), BareResult.Warnings.Num() == 1);
    // A count is satisfied by any warning at all; the consequence is what the author needs told.
    TestTrue(*FString::Printf(TEXT("the warning names the consequence. %s"),
        *PwModelCollisionTest_Describe(BareResult)),
        FString::Join(BareResult.Warnings, TEXT(" | ")).Contains(TEXT("never simulated")));

    FPwModelCollision WithElements;
    WithElements.Complexity = PwModelCollisionTest_Identifier(TEXT("complex_as_simple"));
    FPwOp Sphere = PwModelCollisionTest_Op(TEXT("sphere"));
    Sphere.Params.Add(TEXT("radius"), PwModelCollisionTest_Number(5.0));
    WithElements.Elements.Add(Sphere);

    const FPwModelCollisionResult WithResult = BuildCollision(WithElements, nullptr, PwModelCollisionTest_BoxStub);
    TestTrue(*FString::Printf(TEXT("explicit elements under complex_as_simple warn separately. %s"),
        *PwModelCollisionTest_Describe(WithResult)), WithResult.Warnings.Num() == 2);
    // The second warning is the one that counts the elements being built and never queried, so
    // it has to report the actual number rather than merely exist.
    TestTrue(*FString::Printf(TEXT("the second warning counts the wasted elements. %s"),
        *PwModelCollisionTest_Describe(WithResult)),
        FString::Join(WithResult.Warnings, TEXT(" | ")).Contains(TEXT("with 1 simple collision element(s)")));

    return true;
}

// ============================================================================
// auto
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwModelCollisionAutoTest,
    "PinWright.Model.Collision.AutoConvexHullsProducesElements",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPwModelCollisionAutoTest::RunTest(const FString& Parameters)
{
    UDynamicMesh* Merged = PwModelCollisionTest_NewMesh();
    PwModelCollisionTest_AppendBox(Merged, FVector::ZeroVector, 100.0);
    const int32 TrianglesBefore = Merged->GetTriangleCount();

    FPwModelCollision Block;
    FPwOp Auto = PwModelCollisionTest_Op(TEXT("auto"));
    Auto.Params.Add(TEXT("method"), PwModelCollisionTest_Identifier(TEXT("convex_hulls")));
    Auto.Params.Add(TEXT("max_hulls_per_component"), PwModelCollisionTest_Number(1.0));
    Auto.Params.Add(TEXT("simplify_to"), PwModelCollisionTest_Number(8.0));
    Block.Elements.Add(Auto);

    const FPwModelCollisionResult Result = BuildCollision(Block, Merged, PwModelCollisionTest_BoxStub);

    // simplify_to works on a copy: the merged mesh is what the asset is built from, and a
    // collision option that quietly decimated it would ship a simplified model.
    const int32 TrianglesAfter = Merged->GetTriangleCount();
    Merged->MarkAsGarbage();

    TestTrue(*FString::Printf(TEXT("build succeeded. %s"), *PwModelCollisionTest_Describe(Result)), Result.bSuccess);
    // max_hulls=1 over one connected component is exactly one hull, so ">= 1" was a band where a
    // number is known - and it is the ONLY thing the old assertion looked at.
    TestEqual(TEXT("max_hulls=1 gives exactly one element"), Result.ElementsWritten, 1);
    TestEqual(TEXT("and it is a convex hull"), Result.Geom.ConvexElems.Num(), 1);
    TestEqual(TEXT("auto leaves the source mesh untouched"), TrianglesAfter, TrianglesBefore);

    // Everything above is a count: it holds for a hull of the wrong shape, of the wrong source,
    // or of the wrong ELEMENT TYPE. This second build drops simplify_to so the answer is knowable
    // exactly - the convex hull of an unmodified 100 box IS that box - and pins two options the
    // format overrides against engine defaults that all say `true`:
    //
    //   Options.bAutoDetectBoxes / bAutoDetectSpheres / bAutoDetectCapsules = false. Every one of
    //   the three defaults to TRUE in FGeometryScriptCollisionFromMeshOptions and is fed straight
    //   into FMeshSimpleShapeApproximation's detection pass, so deleting those three lines makes
    //   a box mesh come back as an FKBoxElem instead of the convex hull the document asked for.
    //   ElementsWritten, the warnings and the diagnostics are all identical either way.
    //
    //   Options.Method = Method, which selects the generator outright. Its engine default is
    //   MinVolumeShapes, not ConvexHulls, so dropping the line runs a different approximation
    //   than `method=` named while still yielding one element.
    UDynamicMesh* Plain = PwModelCollisionTest_NewMesh();
    PwModelCollisionTest_AppendBox(Plain, FVector::ZeroVector, 100.0);

    FPwModelCollision PlainBlock;
    FPwOp PlainAuto = PwModelCollisionTest_Op(TEXT("auto"));
    PlainAuto.Params.Add(TEXT("method"), PwModelCollisionTest_Identifier(TEXT("convex_hulls")));
    PlainAuto.Params.Add(TEXT("max_hulls_per_component"), PwModelCollisionTest_Number(1.0));
    PlainBlock.Elements.Add(PlainAuto);

    const FPwModelCollisionResult PlainResult =
        BuildCollision(PlainBlock, Plain, PwModelCollisionTest_BoxStub);
    Plain->MarkAsGarbage();

    TestTrue(*FString::Printf(TEXT("unsimplified build succeeded. %s"),
        *PwModelCollisionTest_Describe(PlainResult)), PlainResult.bSuccess);
    TestEqual(TEXT("convex_hulls produces a convex element"), PlainResult.Geom.ConvexElems.Num(), 1);
    TestEqual(TEXT("and no auto-detected box"), PlainResult.Geom.BoxElems.Num(), 0);
    TestEqual(TEXT("and no auto-detected sphere"), PlainResult.Geom.SphereElems.Num(), 0);
    TestEqual(TEXT("and no auto-detected capsule"), PlainResult.Geom.SphylElems.Num(), 0);

    if (PlainResult.Geom.ConvexElems.Num() == 1)
    {
        const FKConvexElem& Hull = PlainResult.Geom.ConvexElems[0];
        TestTrue(*FString::Printf(TEXT("the hull encloses the source box (centre %s, size %s)"),
                *Hull.ElemBox.GetCenter().ToString(), *Hull.ElemBox.GetSize().ToString()),
            Hull.ElemBox.GetCenter().Equals(FVector::ZeroVector, 1.0)
                && Hull.ElemBox.GetSize().Equals(FVector(100.0, 100.0, 100.0), 1.0));
        TestTrue(*FString::Printf(TEXT("the hull carries vertices (%d)"), Hull.VertexData.Num()),
            Hull.VertexData.Num() >= 4);
    }

    // The derived method vocabulary matches the spec's list.
    const TArray<FString>& Methods = PwModelCollisionNames::AutoMethodNames();
    // AutoMethodNames() is derived from EGeometryScriptCollisionGenerationMethod at runtime, so
    // the vocabulary size follows the engine: UE 5.4 added LevelSets as an eighth method, 5.3 has
    // seven. Pinned per engine so a silently dropped method is still caught on both.
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 4, 0)
    TestEqual(TEXT("eight generation methods"), Methods.Num(), 8);
#else
    TestEqual(TEXT("seven generation methods"), Methods.Num(), 7);
#endif
    TestTrue(*FString::Printf(TEXT("convex_hulls is in [%s]"), *FString::Join(Methods, TEXT(", "))),
        Methods.Contains(FString(TEXT("convex_hulls"))));
    TestTrue(TEXT("min_volume_shapes snake_cases correctly"),
        Methods.Contains(FString(TEXT("min_volume_shapes"))));

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwModelCollisionAutoUnknownMethodTest,
    "PinWright.Model.Collision.UnknownAutoMethodIsRejectedByName",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPwModelCollisionAutoUnknownMethodTest::RunTest(const FString& Parameters)
{
    UDynamicMesh* Merged = PwModelCollisionTest_NewMesh();
    PwModelCollisionTest_AppendBox(Merged, FVector::ZeroVector, 100.0);

    FPwModelCollision Block;
    FPwOp Auto = PwModelCollisionTest_Op(TEXT("auto"));
    Auto.Params.Add(TEXT("method"), PwModelCollisionTest_Identifier(TEXT("spherical")));
    Block.Elements.Add(Auto);

    const FPwModelCollisionResult Result = BuildCollision(Block, Merged, PwModelCollisionTest_BoxStub);
    Merged->MarkAsGarbage();

    TestFalse(TEXT("an unknown method is rejected"), Result.bSuccess);
    TestEqual(TEXT("bad value code"), Result.ErrorCode, FString(PwSourceDiagnosticCodes::PWSRC_BAD_VALUE));
    TestTrue(*FString::Printf(TEXT("the message lists the valid methods. %s"),
        *PwModelCollisionTest_Describe(Result)), Result.ErrorMessage.Contains(TEXT("convex_hulls")));

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwModelCollisionAutoClampsWarnTest,
    "PinWright.Model.Collision.AutoClampsAreWarnedNotSilent",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPwModelCollisionAutoClampsWarnTest::RunTest(const FString& Parameters)
{
    // Contract rule 5: clamping warns. `simplify_to` below the 4 triangles that bound a volume
    // and `max_hulls` below 1 are both raised, and both used to be raised silently while the
    // capsule path warned for the same class of problem - so the author's number and the number
    // that ran differed with nothing to say so.
    UDynamicMesh* Merged = PwModelCollisionTest_NewMesh();
    PwModelCollisionTest_AppendBox(Merged, FVector::ZeroVector, 100.0);

    FPwModelCollision Block;
    FPwOp Auto = PwModelCollisionTest_Op(TEXT("auto"));
    Auto.Params.Add(TEXT("simplify_to"), PwModelCollisionTest_Number(2.0));
    Auto.Params.Add(TEXT("max_hulls_per_component"), PwModelCollisionTest_Number(0.0));
    Block.Elements.Add(Auto);

    const FPwModelCollisionResult Result = BuildCollision(Block, Merged, PwModelCollisionTest_BoxStub);
    Merged->MarkAsGarbage();

    TestTrue(*FString::Printf(TEXT("build succeeded. %s"), *PwModelCollisionTest_Describe(Result)),
        Result.bSuccess);

    const FString Warnings = FString::Join(Result.Warnings, TEXT(" | "));
    TestTrue(*FString::Printf(TEXT("the simplify_to clamp is reported. %s"),
        *PwModelCollisionTest_Describe(Result)), Warnings.Contains(TEXT("simplify_to=2")));
    TestTrue(*FString::Printf(TEXT("the max_hulls clamp is reported. %s"),
        *PwModelCollisionTest_Describe(Result)), Warnings.Contains(TEXT("max_hulls_per_component=0")));

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwModelCollisionAutoPerComponentBudgetTest,
    "PinWright.Model.Collision.AutoHullBudgetIsPerConnectedComponent",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPwModelCollisionAutoPerComponentBudgetTest::RunTest(const FString& Parameters)
{
    // The measured defect, pinned as SEMANTICS rather than fixed: `max_hulls` read as an
    // asset-wide budget and never was one. On an 11-component mesh it produced 11 elements at 1,
    // 22 at 2 and 176 at 16, because ComputeCollisionFromMesh splits the input into one submesh
    // per connected component BEFORE any decomposition (CollisionFunctions.cpp:63-94) and both
    // Generate_ConvexHulls and Generate_ConvexHullDecompositions ParallelFor over the submeshes
    // spending the identical budget on each (MeshSimpleShapeApproximation.cpp:306, :335).
    //
    // The old test could not see it: it built ONE box, and one component times any budget is the
    // budget. Three disjoint boxes is the smallest mesh where the two readings differ.
    //
    // The parameter was renamed rather than reinterpreted, so this asserts the multiplication is
    // still there. Scaling the figure down by the component count could not have honoured a
    // budget below the component count - one hull per component is the engine's floor - and the
    // only real whole-asset cap (MergeSimpleCollisionShapes) merges hulls ACROSS disjoint pieces,
    // which swallows the air between them. If someone later makes this test's numbers smaller,
    // they have changed what the collision covers, not just its element count.
    UDynamicMesh* Merged = PwModelCollisionTest_NewMesh();
    PwModelCollisionTest_AppendBox(Merged, FVector(-400.0, 0.0, 0.0), 100.0);
    PwModelCollisionTest_AppendBox(Merged, FVector::ZeroVector, 100.0);
    PwModelCollisionTest_AppendBox(Merged, FVector(400.0, 0.0, 0.0), 100.0);

    FPwModelCollision Block;
    FPwOp Auto = PwModelCollisionTest_Op(TEXT("auto"));
    Auto.Params.Add(TEXT("method"), PwModelCollisionTest_Identifier(TEXT("convex_hulls")));
    Auto.Params.Add(TEXT("max_hulls_per_component"), PwModelCollisionTest_Number(1.0));
    Block.Elements.Add(Auto);

    const FPwModelCollisionResult Result = BuildCollision(Block, Merged, PwModelCollisionTest_BoxStub);
    Merged->MarkAsGarbage();

    TestTrue(*FString::Printf(TEXT("build succeeded. %s"), *PwModelCollisionTest_Describe(Result)),
        Result.bSuccess);

    // ONE per piece, not one for the asset. This is the whole point of the rename.
    TestEqual(TEXT("a budget of 1 over three disconnected pieces gives three elements"),
        Result.ElementsWritten, 3);

    // And it says so. An author who reads only the element count cannot tell 3 pieces at 1 from
    // 1 piece at 3; the warning names the budget, the total and the piece count.
    const FString Warnings = FString::Join(Result.Warnings, TEXT(" | "));
    TestTrue(*FString::Printf(TEXT("the overrun is reported. %s"),
        *PwModelCollisionTest_Describe(Result)),
        Warnings.Contains(TEXT("max_hulls_per_component=1")) && Warnings.Contains(TEXT("3 collision elements")));
    TestTrue(*FString::Printf(TEXT("and it names the piece count. %s"),
        *PwModelCollisionTest_Describe(Result)),
        Warnings.Contains(TEXT("3 disconnected pieces")));

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwModelCollisionAutoLegacyMaxHullsTest,
    "PinWright.Model.Collision.AutoStillAcceptsTheOldMaxHullsSpelling",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPwModelCollisionAutoLegacyMaxHullsTest::RunTest(const FString& Parameters)
{
    // Documents were written against `max_hulls`, so it still runs - and warns, because the name
    // it carries is the thing that was wrong. Removing the alias silently breaks those documents;
    // removing the warning silently restores the misreading.
    UDynamicMesh* Merged = PwModelCollisionTest_NewMesh();
    PwModelCollisionTest_AppendBox(Merged, FVector::ZeroVector, 100.0);

    FPwModelCollision Block;
    FPwOp Auto = PwModelCollisionTest_Op(TEXT("auto"));
    Auto.Params.Add(TEXT("method"), PwModelCollisionTest_Identifier(TEXT("convex_hulls")));
    Auto.Params.Add(TEXT("max_hulls"), PwModelCollisionTest_Number(1.0));
    Block.Elements.Add(Auto);

    const FPwModelCollisionResult Result = BuildCollision(Block, Merged, PwModelCollisionTest_BoxStub);
    Merged->MarkAsGarbage();

    TestTrue(*FString::Printf(TEXT("the old spelling still builds. %s"),
        *PwModelCollisionTest_Describe(Result)), Result.bSuccess);
    TestEqual(TEXT("and still means the same number"), Result.ElementsWritten, 1);

    const FString Warnings = FString::Join(Result.Warnings, TEXT(" | "));
    TestTrue(*FString::Printf(TEXT("the rename is reported. %s"),
        *PwModelCollisionTest_Describe(Result)),
        Warnings.Contains(TEXT("max_hulls_per_component")));

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwModelCollisionUnknownEntryListsTableTest,
    "PinWright.Model.Collision.UnknownEntryListsTheOpTablesVocabulary",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPwModelCollisionUnknownEntryListsTableTest::RunTest(const FString& Parameters)
{
    // The valid-entry list is derived from the op table, not hand-written beside the loop. A
    // collision entry added to the table and not to the loop would otherwise be published by
    // model.describe_ops and rejected by a message claiming it does not exist.
    FPwModelCollision Block;
    Block.Elements.Add(PwModelCollisionTest_Op(TEXT("cylinder")));

    const FPwModelCollisionResult Result = BuildCollision(Block, nullptr, PwModelCollisionTest_BoxStub);

    TestFalse(TEXT("an unknown entry is rejected"), Result.bSuccess);
    TestEqual(TEXT("unknown op code"), Result.ErrorCode, FString(PwSourceDiagnosticCodes::PWSRC_UNKNOWN_OP));

    // HAND-WRITTEN, not read back out of the table. Deriving the expectation from
    // NamesInContext - the same call that builds the message - makes the check a round-trip
    // through one source: an entry dropped from the table disappears from both sides and the
    // loop still passes, and a table that returned nothing at all would run zero assertions and
    // report green. These six are the collision vocabulary the format documents; adding a
    // seventh is meant to fail here, and the fix is to add it to this line.
    const TArray<FString> Expected = {
        TEXT("auto"), TEXT("box"), TEXT("capsule"), TEXT("convex"), TEXT("hull"), TEXT("sphere") };

    const TArray<FString> Published = PwModelOpTable::NamesInContext(EPwModelOpContext::Collision);
    TestEqual(*FString::Printf(TEXT("the op table publishes six collision entries: [%s]"),
        *FString::Join(Published, TEXT(", "))), Published.Num(), Expected.Num());

    for (const FString& Name : Expected)
    {
        TestTrue(*FString::Printf(TEXT("the op table still carries '%s': [%s]"),
            *Name, *FString::Join(Published, TEXT(", "))), Published.Contains(Name));
        TestTrue(*FString::Printf(TEXT("the message lists '%s'. %s"),
            *Name, *PwModelCollisionTest_Describe(Result)), Result.ErrorMessage.Contains(Name));
    }

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwModelCollisionAutoConflictTest,
    "PinWright.Model.Collision.AutoBesideExplicitElementsConflicts",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPwModelCollisionAutoConflictTest::RunTest(const FString& Parameters)
{
    UDynamicMesh* Merged = PwModelCollisionTest_NewMesh();
    PwModelCollisionTest_AppendBox(Merged, FVector::ZeroVector, 100.0);

    FPwModelCollision Block;
    FPwOp Sphere = PwModelCollisionTest_Op(TEXT("sphere"));
    Sphere.Params.Add(TEXT("radius"), PwModelCollisionTest_Number(5.0));
    Block.Elements.Add(Sphere);
    Block.Elements.Add(PwModelCollisionTest_Op(TEXT("auto")));

    const FPwModelCollisionResult Result = BuildCollision(Block, Merged, PwModelCollisionTest_BoxStub);
    Merged->MarkAsGarbage();

    // Whichever ran second would silently win, so neither runs.
    TestFalse(TEXT("auto beside explicit elements is refused"), Result.bSuccess);
    TestEqual(TEXT("conflict code"), Result.ErrorCode,
        FString(PwModelDiagnosticCodes::PWMODEL_COLLISION_CONFLICT));
    TestEqual(TEXT("nothing is written"), Result.ElementsWritten, 0);
    TestEqual(TEXT("and no geometry is left behind"), Result.Geom.GetElementCount(), 0);

    // The guard has a second arm - `AutoCount > 1` - that the case above never reaches, and `auto`
    // ASSIGNS the aggregate geometry rather than adding to it, so a second rule silently
    // discarding the first is the exact failure the guard exists for.
    UDynamicMesh* TwoAutoMesh = PwModelCollisionTest_NewMesh();
    PwModelCollisionTest_AppendBox(TwoAutoMesh, FVector::ZeroVector, 100.0);

    FPwModelCollision TwoAuto;
    TwoAuto.Elements.Add(PwModelCollisionTest_Op(TEXT("auto")));
    TwoAuto.Elements.Add(PwModelCollisionTest_Op(TEXT("auto")));

    const FPwModelCollisionResult TwoAutoResult =
        BuildCollision(TwoAuto, TwoAutoMesh, PwModelCollisionTest_BoxStub);
    TwoAutoMesh->MarkAsGarbage();

    TestFalse(*FString::Printf(TEXT("two auto rules are refused. %s"),
        *PwModelCollisionTest_Describe(TwoAutoResult)), TwoAutoResult.bSuccess);
    TestEqual(TEXT("two-auto conflict code"), TwoAutoResult.ErrorCode,
        FString(PwModelDiagnosticCodes::PWMODEL_COLLISION_CONFLICT));
    TestEqual(TEXT("nothing is written for two auto rules"), TwoAutoResult.ElementsWritten, 0);

    return true;
}

// ============================================================================
// Parser round-trip
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwModelCollisionFromParsedDocumentTest,
    "PinWright.Model.Collision.ParsedCollisionBlockBuilds",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPwModelCollisionFromParsedDocumentTest::RunTest(const FString& Parameters)
{
    // The parameter names below come from the parser's op table. Building straight off a parsed
    // document is what catches the two halves drifting apart: rename `size` on one side and this
    // test reports a zero-extent box rather than a compile error.
    const TCHAR* Source =
        TEXT("pwmodel 0\n")
        TEXT("part body {\n")
        TEXT("    box size=(10, 10, 10)\n")
        TEXT("}\n")
        TEXT("collision {\n")
        TEXT("    complexity = simple_as_complex\n")
        TEXT("    box size=(78, 48, 38) at=(0, 0, 5)\n")
        TEXT("    capsule radius=6 height=40\n")
        TEXT("    hull { cylinder radius=20 height=60 }\n")
        TEXT("}\n");

    FPwModelDocument Document;
    TArray<FPwDiagnostic> Diagnostics;
    const bool bParsed = FPwModelParser::Parse(Source, Document, Diagnostics);

    TestTrue(*FString::Printf(TEXT("document parsed. Diagnostics: [%s]"),
        *JoinPwDiagnostics(Diagnostics)), bParsed);
    if (!Document.Collision.IsSet())
    {
        AddError(TEXT("the parsed document carries no collision block"));
        return false;
    }

    const FPwModelCollisionResult Result =
        BuildCollision(Document.Collision.GetValue(), nullptr, PwModelCollisionTest_BoxStub);

    TestTrue(*FString::Printf(TEXT("build succeeded. %s"), *PwModelCollisionTest_Describe(Result)), Result.bSuccess);
    TestEqual(TEXT("trace flag from the parsed complexity"),
        static_cast<int32>(Result.Trace), static_cast<int32>(ECollisionTraceFlag::CTF_UseSimpleAsComplex));
    TestEqual(TEXT("three elements"), Result.ElementsWritten, 3);

    if (Result.Geom.BoxElems.Num() == 1)
    {
        // All three axes: the tuple is deliberately (78, 48, 38), so a reader that broadcast one
        // component across the extents - or read the tuple in a different order - is visible.
        TestEqual(TEXT("parsed box X"), Result.Geom.BoxElems[0].X, 78.0f);
        TestEqual(TEXT("parsed box Y"), Result.Geom.BoxElems[0].Y, 48.0f);
        TestEqual(TEXT("parsed box Z"), Result.Geom.BoxElems[0].Z, 38.0f);
        TestTrue(TEXT("parsed box centre"), Result.Geom.BoxElems[0].Center.Equals(FVector(0.0, 0.0, 5.0)));
    }
    else
    {
        AddError(TEXT("expected one box element from the parsed block"));
    }

    if (Result.Geom.SphylElems.Num() == 1)
    {
        TestEqual(TEXT("parsed capsule Length"), Result.Geom.SphylElems[0].Length, 28.0f);
        TestEqual(TEXT("parsed capsule Radius"), Result.Geom.SphylElems[0].Radius, 6.0f);
    }
    else
    {
        AddError(TEXT("expected one sphyl element from the parsed block"));
    }

    // ElementsWritten == 3 above is satisfied by any three elements: the hull entry landing as
    // something other than a convex element - or the parser handing its body to nobody - is only
    // visible as a per-type count.
    TestEqual(TEXT("the parsed hull block became a convex element"), Result.Geom.ConvexElems.Num(), 1);

    return true;
}
