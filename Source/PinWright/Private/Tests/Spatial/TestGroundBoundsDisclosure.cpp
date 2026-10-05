// Copyright (c) 2026 Alexander Penkin. MIT License.

// TestGroundBoundsDisclosure.cpp - what the grounding verbs tell a caller about the BOX they
// measured, as opposed to the object.
//
//   - spatial.ground_instances models an instance's underside as the floor of its WORLD AABB, and
//     FBox::TransformBy re-fits that box around the rotated mesh, so a pitched or rolled instance's
//     plane sits below the mesh. The response used to publish only differences against that plane
//     (maxGapCm = UndersideZ - GroundZ, neither term known) and asserted undersideReliefCm: 0 - a
//     measured flat underside - about tumbled rocks. It now publishes contact.undersideZCm /
//     groundZCm, the per-row boundsRotationInflationCm, and omits the relief it never measured.
//   - samples:1 makes coverage, contactPoints and the gap terms fixed by the parameter, at a
//     column placed on the world-AABB centre rather than the pivot. The response now says so in
//     warnings[], names the centre (contact.footprintCentreCm), and omits groundSpreadCm /
//     undersideReliefCm rather than reporting them as a measured 0.
//
// The fixture is a SPHERE scatter on purpose: a sphere's lowest point does not move when it is
// rotated, while its rotated bounding box's floor does - so "the plane is below the mesh" is a
// geometric fact here rather than a property of a particular asset.
//
// Helpers live in the GroundBoundsDisclosureTest namespace (Unity-build rule).

#include "Misc/AutomationTest.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/HandlerRegistration.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Tests/TestUtils.h"
#include "Tests/TestSkipReporting.h"
#include "Utils/ActorUtils.h"

#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Editor.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Misc/Guid.h"
#include "Misc/ScopeExit.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

namespace GroundBoundsDisclosureTest
{
    // An isolated column, distinct from every other spatial suite's.
    constexpr double ColX = -412300.0;
    constexpr double ColY = 287700.0;
    constexpr double FloorTopZ = 0.0;

    UWorld* EditorWorld()
    {
        return GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    }

    FString Label(const TCHAR* Prefix)
    {
        return FString::Printf(TEXT("PWGB_%s_%s"), Prefix, *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    }

    TSharedPtr<FJsonObject> Vec(double X, double Y, double Z)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetNumberField(TEXT("x"), X);
        Obj->SetNumberField(TEXT("y"), Y);
        Obj->SetNumberField(TEXT("z"), Z);
        return Obj;
    }

    // A just-spawned body is not in the scene-query structure until a world tick flushes it.
    void FlushPhysics(UWorld* World)
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

    AActor* SpawnMesh(FAutomationTestBase& Test, UWorld* World, const FString& ActorLabel,
                      const TCHAR* MeshPath, const FVector& Location, const FVector& Scale)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("meshPath"), MeshPath);
        Payload->SetStringField(TEXT("actorName"), ActorLabel);
        Payload->SetObjectField(TEXT("location"), Vec(Location.X, Location.Y, Location.Z));
        Payload->SetObjectField(TEXT("scale"), Vec(Scale.X, Scale.Y, Scale.Z));
        FTestResponseCapture Capture;
        Test.TestTrue(TEXT("actor.spawn handler registered"),
            InvokeHandlerWithCapture(TEXT("actor.spawn"), Payload, Capture));
        Test.TestTrue(*FString::Printf(TEXT("'%s' spawned"), *ActorLabel), Capture.bSuccess);
        return McpActorUtils::FindActorByName(World, ActorLabel);
    }

    // A 2000 x 2000 cube floor whose top is FloorTopZ.
    AActor* SpawnFloor(FAutomationTestBase& Test, UWorld* World, const FString& ActorLabel)
    {
        return SpawnMesh(Test, World, ActorLabel, TEXT("/Engine/BasicShapes/Cube.Cube"),
            FVector(ColX, ColY, FloorTopZ - 50.0), FVector(20.0, 20.0, 1.0));
    }

    // A holder whose root HISM carries one Mesh instance per rotation, 300 cm apart along X.
    AActor* SpawnScatter(FAutomationTestBase& Test, UWorld* World, const FString& ActorLabel,
                         const FVector& Location, UStaticMesh* Mesh, const TArray<FRotator>& Rotations)
    {
        if (!World || !Mesh)
        {
            Test.AddError(TEXT("mesh unavailable for the scatter fixture"));
            return nullptr;
        }
        AActor* Holder = World->SpawnActor<AActor>(AActor::StaticClass(), FVector::ZeroVector,
            FRotator::ZeroRotator);
        if (!Holder)
        {
            Test.AddError(TEXT("scatter holder actor did not spawn"));
            return nullptr;
        }
        UHierarchicalInstancedStaticMeshComponent* Hism =
            NewObject<UHierarchicalInstancedStaticMeshComponent>(Holder, TEXT("HISM_BoundsDisclosure"),
                RF_Transactional);
        Holder->SetRootComponent(Hism);
        Holder->AddInstanceComponent(Hism);
        Hism->OnComponentCreated();
        Hism->SetStaticMesh(Mesh);
        Hism->RegisterComponent();
        for (int32 Index = 0; Index < Rotations.Num(); ++Index)
        {
            Hism->AddInstance(FTransform(Rotations[Index], FVector(300.0 * Index, 0.0, 0.0)));
        }
        Holder->SetActorLocation(Location);
        Holder->SetActorLabel(ActorLabel);
        return Holder;
    }

    TSharedPtr<FJsonObject> RowForIndex(const TSharedPtr<FJsonObject>& Result, int32 Index)
    {
        const TArray<TSharedPtr<FJsonValue>>* Rows = nullptr;
        if (!Result.IsValid() || !Result->TryGetArrayField(TEXT("results"), Rows) || !Rows)
        {
            return nullptr;
        }
        for (const TSharedPtr<FJsonValue>& Value : *Rows)
        {
            const TSharedPtr<FJsonObject> Row = Value.IsValid() ? Value->AsObject() : nullptr;
            double RowIndex = -1.0;
            if (Row.IsValid() && Row->TryGetNumberField(TEXT("index"), RowIndex)
                && static_cast<int32>(RowIndex) == Index)
            {
                return Row;
            }
        }
        return nullptr;
    }

    TSharedPtr<FJsonObject> FirstRow(const TSharedPtr<FJsonObject>& Result)
    {
        const TArray<TSharedPtr<FJsonValue>>* Rows = nullptr;
        if (!Result.IsValid() || !Result->TryGetArrayField(TEXT("results"), Rows) || !Rows
            || Rows->Num() == 0 || !(*Rows)[0].IsValid())
        {
            return nullptr;
        }
        return (*Rows)[0]->AsObject();
    }

    TSharedPtr<FJsonObject> Contact(const TSharedPtr<FJsonObject>& Row)
    {
        const TSharedPtr<FJsonObject>* Out = nullptr;
        return (Row.IsValid() && Row->TryGetObjectField(TEXT("contact"), Out) && Out) ? *Out : nullptr;
    }

    // Field of Obj, or Fallback when absent - absence is part of what these tests assert.
    double Number(const TSharedPtr<FJsonObject>& Obj, const TCHAR* Field, double Fallback)
    {
        double Value = Fallback;
        if (Obj.IsValid())
        {
            Obj->TryGetNumberField(Field, Value);
        }
        return Value;
    }

    double Nested(const TSharedPtr<FJsonObject>& Obj, const TCHAR* Object, const TCHAR* Field,
                  double Fallback)
    {
        const TSharedPtr<FJsonObject>* Inner = nullptr;
        if (!Obj.IsValid() || !Obj->TryGetObjectField(Object, Inner) || !Inner)
        {
            return Fallback;
        }
        return Number(*Inner, Field, Fallback);
    }

    // The joined warnings[] text, empty when the key is absent.
    FString Warnings(const TSharedPtr<FJsonObject>& Result)
    {
        const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
        if (!Result.IsValid() || !Result->TryGetArrayField(TEXT("warnings"), Array) || !Array)
        {
            return FString();
        }
        FString Joined;
        for (const TSharedPtr<FJsonValue>& Value : *Array)
        {
            Joined += Value.IsValid() ? Value->AsString() : FString();
            Joined += TEXT("\n");
        }
        return Joined;
    }

    TSharedPtr<FJsonObject> AnySolid()
    {
        TSharedPtr<FJsonObject> Surface = MakeShared<FJsonObject>();
        Surface->SetStringField(TEXT("preset"), TEXT("any_solid"));
        return Surface;
    }
}

// ---- #298: the bounds plane a tilted instance is seated against is in the response ----
//
// Reverting the fix fails this test four ways: contact.undersideZCm / groundZCm and the row's
// boundsRotationInflationCm disappear (read back as the -1e9 fallback), and undersideReliefCm
// comes back as an asserted 0. The solve identity proposedDeltaZCm == groundZCm - undersideZCm
// is what proves the published plane is the one the solve actually used.

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGroundInstancesPublishesTiltedBoundsPlaneTest,
    "PinWright.spatial.ground_instances.PublishesTheTiltedBoundsPlane",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGroundInstancesPublishesTiltedBoundsPlaneTest::RunTest(const FString& Parameters)
{
    namespace GBD = GroundBoundsDisclosureTest;

    UWorld* World = GBD::EditorWorld();
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no_editor_world"),
            TEXT("GEditor->GetEditorWorldContext().World() returned null; the bounds-plane "
                 "disclosure assertions were stepped over."));
        return true;
    }

    const FString FloorLabel = GBD::Label(TEXT("Floor"));
    const FString HolderLabel = GBD::Label(TEXT("Spheres"));
    AActor* Floor = GBD::SpawnFloor(*this, World, FloorLabel);
    // Instance 0 upright, instance 1 pitched 45 degrees; both centres 300 cm above the floor.
    AActor* Holder = GBD::SpawnScatter(*this, World, HolderLabel, FVector(GBD::ColX - 150.0, GBD::ColY, 300.0),
        LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Sphere.Sphere")),
        {FRotator::ZeroRotator, FRotator(45.0, 0.0, 0.0)});
    ON_SCOPE_EXIT
    {
        if (Floor) { Floor->Destroy(); }
        if (Holder) { Holder->Destroy(); }
    };
    UInstancedStaticMeshComponent* Component =
        Holder ? Holder->FindComponentByClass<UInstancedStaticMeshComponent>() : nullptr;
    if (!Floor || !Component || !Component->GetStaticMesh())
    {
        AddError(TEXT("bounds-disclosure fixture did not spawn"));
        return true;
    }
    GBD::FlushPhysics(World);

    // Ground truth from the engine: each instance's world AABB, and the mesh's local half-height.
    const FBox LocalBox = Component->GetStaticMesh()->GetBounds().GetBox();
    FBox Boxes[2];
    FTransform Worlds[2];
    for (int32 Index = 0; Index < 2; ++Index)
    {
        if (!Component->GetInstanceTransform(Index, Worlds[Index], /*bWorldSpace*/ true))
        {
            AddError(*FString::Printf(TEXT("instance %d could not be read"), Index));
            return true;
        }
        Boxes[Index] = LocalBox.TransformBy(Worlds[Index]);
    }
    // Precondition: the tilt really inflates the box, and the sphere's real bottom does not move.
    const double TiltedInflation = Boxes[1].GetExtent().Z - LocalBox.GetExtent().Z;
    TestTrue(*FString::Printf(TEXT("precondition: a 45-degree pitch inflates the sphere's AABB "
        "(%.2f cm)"), TiltedInflation), TiltedInflation > 15.0);

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("actorName"), HolderLabel);
    Payload->SetObjectField(TEXT("surface"), GBD::AnySolid());
    Payload->SetBoolField(TEXT("apply"), false);
    Payload->SetNumberField(TEXT("embedFraction"), 0.0);
    Payload->SetStringField(TEXT("detail"), TEXT("all"));

    FTestResponseCapture Capture;
    TestTrue(TEXT("spatial.ground_instances handler registered"),
        InvokeHandlerWithCapture(TEXT("spatial.ground_instances"), Payload, Capture));
    TestTrue(TEXT("the dry run succeeds"), Capture.bSuccess);
    if (!Capture.bSuccess || !Capture.Result.IsValid())
    {
        return true;
    }
    TestTrue(TEXT("samples:3 carries no single-column warning"), GBD::Warnings(Capture.Result).IsEmpty());

    constexpr double Missing = -1.0e9;
    for (int32 Index = 0; Index < 2; ++Index)
    {
        const TSharedPtr<FJsonObject> Row = GBD::RowForIndex(Capture.Result, Index);
        const TSharedPtr<FJsonObject> Body = GBD::Contact(Row);
        if (!Body.IsValid())
        {
            AddError(*FString::Printf(TEXT("no contact row for instance %d"), Index));
            continue;
        }
        const double UndersideZ = GBD::Number(Body, TEXT("undersideZCm"), Missing);
        const double GroundZ = GBD::Number(Body, TEXT("groundZCm"), Missing);
        const double Inflation = GBD::Number(Row, TEXT("boundsRotationInflationCm"), Missing);

        TestEqual(*FString::Printf(TEXT("instance %d: undersideZCm is the floor of its world AABB"),
            Index), UndersideZ, Boxes[Index].Min.Z, 0.01);
        TestEqual(*FString::Printf(TEXT("instance %d: groundZCm is the floor top"), Index),
            GroundZ, GBD::FloorTopZ, 0.5);
        TestEqual(*FString::Printf(TEXT("instance %d: boundsRotationInflationCm"), Index),
            Inflation, Boxes[Index].GetExtent().Z - LocalBox.GetExtent().Z, 0.01);
        // The published plane is the solved one: seatPercentile 0 over flat ground, no embed.
        TestEqual(*FString::Printf(TEXT("instance %d: proposedDeltaZCm == groundZCm - undersideZCm"),
            Index), GBD::Number(Row, TEXT("proposedDeltaZCm"), Missing), GroundZ - UndersideZ, 0.05);
        // A flat plane has no relief, so none is claimed.
        TestFalse(*FString::Printf(TEXT("instance %d: undersideReliefCm is omitted under "
            "bounds_plane"), Index), Body->HasField(TEXT("undersideReliefCm")));
        TestTrue(*FString::Printf(TEXT("instance %d: groundSpreadCm is still reported over 9 "
            "columns"), Index), Body->HasField(TEXT("groundSpreadCm")));
    }
    TestTrue(TEXT("an upright instance has no rotation inflation"),
        FMath::Abs(GBD::Number(GBD::RowForIndex(Capture.Result, 0), TEXT("boundsRotationInflationCm"), Missing))
            < 0.01);

    // The defect itself, visible from the response alone: the tilted sphere's plane sits below
    // its real lowest point (rotation-invariant for a sphere) by the published inflation.
    const double TiltedPlane = GBD::Number(GBD::Contact(GBD::RowForIndex(Capture.Result, 1)), TEXT("undersideZCm"),
        Missing);
    const double SphereBottom = Worlds[1].GetLocation().Z + LocalBox.Min.Z;
    TestEqual(TEXT("the tilted plane is below the sphere's real bottom by boundsRotationInflationCm"),
        SphereBottom - TiltedPlane, TiltedInflation, 0.05);

    // #42 on the same fixture: samples:1 warns, and names where its one column went.
    Payload->SetNumberField(TEXT("samples"), 1);
    FTestResponseCapture Single;
    InvokeHandlerWithCapture(TEXT("spatial.ground_instances"), Payload, Single);
    TestTrue(TEXT("the samples:1 dry run succeeds"), Single.bSuccess);
    TestTrue(TEXT("samples:1 on ground_instances carries the single-column warning"),
        GBD::Warnings(Single.Result).Contains(TEXT("samples:1")));
    const TSharedPtr<FJsonObject> SingleBody = GBD::Contact(GBD::RowForIndex(Single.Result, 1));
    TestEqual(TEXT("footprintCentreCm.x is the tilted instance's world-AABB centre"),
        GBD::Nested(SingleBody, TEXT("footprintCentreCm"), TEXT("x"), Missing), Boxes[1].GetCenter().X, 0.01);
    TestEqual(TEXT("footprintCentreCm.y is the tilted instance's world-AABB centre"),
        GBD::Nested(SingleBody, TEXT("footprintCentreCm"), TEXT("y"), Missing), Boxes[1].GetCenter().Y, 0.01);

    // ---- A BASE-PIVOT mesh taller than wide: the case a centred sphere cannot catch. ----
    // A transient copy of the engine cube with its bounds pushed to local x,y -50..50, z 0..200:
    // pivot at the base, 100 x 100 x 200. Pitched 20 degrees about that base, the box's floor
    // drops by 50*sin(20) = 17.10 cm (a lower corner swings below the pivot), while the
    // half-height difference the field used to publish reads 50*sin(20) + 100*cos(20) - 100 =
    // 11.07 - so the old formula fails here by 6 cm and, on a narrower mesh, by its sign.
    UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
    TStrongObjectPtr<UStaticMesh> Tall(Cube ? DuplicateObject<UStaticMesh>(Cube, GetTransientPackage(),
        MakeUniqueObjectName(GetTransientPackage(), UStaticMesh::StaticClass(), TEXT("SM_PWGB_BasePivotTall")))
        : nullptr);
    const FString TallLabel = GBD::Label(TEXT("BasePivot"));
    AActor* TallHolder = nullptr;
    ON_SCOPE_EXIT
    {
        if (TallHolder) { TallHolder->Destroy(); }
        if (Tall.IsValid())
        {
            Tall->ClearFlags(RF_Standalone | RF_Public);
            Tall->MarkAsGarbage();
            Tall.Reset();
        }
    };
    if (!Tall.IsValid())
    {
        AddError(TEXT("could not duplicate the engine cube for the base-pivot fixture"));
        return true;
    }
    // CalculateExtendedBounds does Min -= Negative, Max += Positive (StaticMesh.cpp:7309-7310).
    const FBox CubeLocal = Tall->GetBounds().GetBox();
    Tall->SetNegativeBoundsExtension(FVector(0.0, 0.0, CubeLocal.Min.Z));
    Tall->SetPositiveBoundsExtension(FVector(0.0, 0.0, 200.0 - CubeLocal.Max.Z));
    Tall->CalculateExtendedBounds();
    const FBox TallLocal = Tall->GetBounds().GetBox();
    TestTrue(*FString::Printf(TEXT("precondition: the fixture box is base-pivot and taller than wide "
        "(%s)"), *TallLocal.ToString()),
        FMath::IsNearlyZero(TallLocal.Min.Z, 0.01) && FMath::IsNearlyEqual(TallLocal.Max.Z, 200.0, 0.01)
            && FMath::IsNearlyEqual(TallLocal.GetExtent().X, 50.0, 0.01));

    TallHolder = GBD::SpawnScatter(*this, World, TallLabel, FVector(GBD::ColX + 500.0, GBD::ColY, 300.0),
        Tall.Get(), {FRotator(20.0, 0.0, 0.0)});
    UInstancedStaticMeshComponent* TallComponent =
        TallHolder ? TallHolder->FindComponentByClass<UInstancedStaticMeshComponent>() : nullptr;
    FTransform TallWorld;
    if (!TallComponent || !TallComponent->GetInstanceTransform(0, TallWorld, /*bWorldSpace*/ true))
    {
        AddError(TEXT("base-pivot scatter did not spawn"));
        return true;
    }
    GBD::FlushPhysics(World);
    const double ExpectedDrop = TallWorld.GetLocation().Z + TallLocal.Min.Z
        - TallLocal.TransformBy(TallWorld).Min.Z;
    const double OldFormula = TallLocal.TransformBy(TallWorld).GetExtent().Z - TallLocal.GetExtent().Z;
    TestEqual(TEXT("precondition: a 20-degree pitch lowers the base-pivot box's floor by 50*sin(20)"),
        ExpectedDrop, 50.0 * FMath::Sin(FMath::DegreesToRadians(20.0)), 0.05);
    TestTrue(*FString::Printf(TEXT("precondition: the half-height difference (%.2f) is not the drop "
        "(%.2f)"), OldFormula, ExpectedDrop), FMath::Abs(OldFormula - ExpectedDrop) > 5.0);

    Payload->SetStringField(TEXT("actorName"), TallLabel);
    Payload->SetNumberField(TEXT("samples"), 3);
    FTestResponseCapture TallCapture;
    InvokeHandlerWithCapture(TEXT("spatial.ground_instances"), Payload, TallCapture);
    TestTrue(TEXT("the base-pivot dry run succeeds"), TallCapture.bSuccess);
    TestEqual(TEXT("boundsRotationInflationCm is the drop of the box's floor, not a half-height "
                   "difference"),
        GBD::Number(GBD::RowForIndex(TallCapture.Result, 0), TEXT("boundsRotationInflationCm"), Missing),
        ExpectedDrop, 0.05);
    return true;
}

// ---- #42: samples:1 is announced, and the fields it cannot measure are omitted ----
//
// Reverting the fix fails this test: warnings[] is absent at samples:1, groundSpreadCm and
// undersideReliefCm come back as a measured-looking 0, and footprintCentreCm is missing. The
// samples:3 control proves the omission is about the column count, not a blanket removal.

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVerifyGroundingSingleColumnDisclosureTest,
    "PinWright.spatial.verify_grounding.SingleColumnWarnsAndOmitsWhatItCannotMeasure",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FVerifyGroundingSingleColumnDisclosureTest::RunTest(const FString& Parameters)
{
    namespace GBD = GroundBoundsDisclosureTest;

    UWorld* World = GBD::EditorWorld();
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no_editor_world"),
            TEXT("GEditor->GetEditorWorldContext().World() returned null; the single-column "
                 "disclosure assertions were stepped over."));
        return true;
    }

    const FString FloorLabel = GBD::Label(TEXT("Floor1"));
    const FString CubeLabel = GBD::Label(TEXT("Cube1"));
    AActor* Floor = GBD::SpawnFloor(*this, World, FloorLabel);
    // A cube resting on the floor (centred pivot, 100 cm tall), off the column's centre line.
    AActor* Cube = GBD::SpawnMesh(*this, World, CubeLabel, TEXT("/Engine/BasicShapes/Cube.Cube"),
        FVector(GBD::ColX + 400.0, GBD::ColY + 250.0, GBD::FloorTopZ + 50.0), FVector(1.0, 1.0, 1.0));
    ON_SCOPE_EXIT
    {
        if (Floor) { Floor->Destroy(); }
        if (Cube) { Cube->Destroy(); }
    };
    if (!Floor || !Cube)
    {
        AddError(TEXT("single-column fixture did not spawn"));
        return true;
    }
    GBD::FlushPhysics(World);

    FVector Origin = FVector::ZeroVector;
    FVector Extent = FVector::ZeroVector;
    Cube->GetActorBounds(false, Origin, Extent);

    auto Verify = [&](int32 Samples, FTestResponseCapture& Out)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetObjectField(TEXT("surface"), GBD::AnySolid());
        TArray<TSharedPtr<FJsonValue>> Names;
        Names.Add(MakeShared<FJsonValueString>(CubeLabel));
        Payload->SetArrayField(TEXT("actors"), Names);
        Payload->SetStringField(TEXT("detail"), TEXT("all"));
        Payload->SetNumberField(TEXT("samples"), Samples);
        TestTrue(TEXT("spatial.verify_grounding handler registered"),
            InvokeHandlerWithCapture(TEXT("spatial.verify_grounding"), Payload, Out));
        TestTrue(*FString::Printf(TEXT("samples:%d verify succeeds"), Samples), Out.bSuccess);
    };

    FTestResponseCapture Single;
    Verify(1, Single);
    const FString SingleWarnings = GBD::Warnings(Single.Result);
    TestTrue(*FString::Printf(TEXT("samples:1 carries a warnings[] entry (got '%s')"), *SingleWarnings),
        SingleWarnings.Contains(TEXT("samples:1")));
    TestTrue(TEXT("...naming that coverage is fixed by the parameter"),
        SingleWarnings.Contains(TEXT("coverage")));
    const TSharedPtr<FJsonObject> SingleBody = GBD::Contact(GBD::FirstRow(Single.Result));
    if (!SingleBody.IsValid())
    {
        AddError(TEXT("samples:1 verify returned no contact block"));
        return true;
    }
    TestEqual(TEXT("precondition: samples:1 is one supported column"),
        GBD::Number(SingleBody, TEXT("supportedColumns"), -1.0), 1.0);
    TestFalse(TEXT("one column has no groundSpreadCm to report"),
        SingleBody->HasField(TEXT("groundSpreadCm")));
    TestFalse(TEXT("one column has no undersideReliefCm to report"),
        SingleBody->HasField(TEXT("undersideReliefCm")));
    TestEqual(TEXT("footprintCentreCm.x is the world-bounds centre"),
        GBD::Nested(SingleBody, TEXT("footprintCentreCm"), TEXT("x"), -1.0e9), Origin.X, 0.01);
    TestEqual(TEXT("footprintCentreCm.y is the world-bounds centre"),
        GBD::Nested(SingleBody, TEXT("footprintCentreCm"), TEXT("y"), -1.0e9), Origin.Y, 0.01);
    TestEqual(TEXT("groundZCm is the floor top"),
        GBD::Number(SingleBody, TEXT("groundZCm"), -1.0e9), GBD::FloorTopZ, 0.5);

    FTestResponseCapture Grid;
    Verify(3, Grid);
    TestTrue(TEXT("samples:3 carries no single-column warning"), GBD::Warnings(Grid.Result).IsEmpty());
    const TSharedPtr<FJsonObject> GridBody = GBD::Contact(GBD::FirstRow(Grid.Result));
    TestTrue(TEXT("samples:3 still reports groundSpreadCm"),
        GridBody.IsValid() && GridBody->HasField(TEXT("groundSpreadCm")));
    TestTrue(TEXT("samples:3 under the mesh model still reports undersideReliefCm"),
        GridBody.IsValid() && GridBody->HasField(TEXT("undersideReliefCm")));
    return true;
}
