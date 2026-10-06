// Copyright (c) 2026 Alexander Penkin. MIT License.

// TestGroundBoundsDisclosure.cpp - what the grounding verbs tell a caller about the BOX they
// measured, as opposed to the object.
//
//   - spatial.ground_instances used to model an instance's underside as the floor of its WORLD
//     AABB, and FBox::TransformBy re-fits that box around the rotated mesh, so a pitched or rolled
//     instance's plane sat below the mesh (#298 disclosed it, #366 fixed it). The plane is now the
//     mesh's lowest LOD0 vertex under the instance transform; the response publishes it as
//     contact.undersideZCm beside groundZCm and boundsRotationInflationCm, and omits the relief a
//     plane never measures.
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
#include "Handlers/Spatial/GroundPlacementUtils.h"
#include "MeshDescription.h"
#include "StaticMeshAttributes.h"
#include "StaticMeshResources.h"
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

    // A holder whose root HISM carries one Mesh instance per rotation, 300 cm apart along X, each
    // raised by its ZOffsets entry (0 when absent).
    AActor* SpawnScatter(FAutomationTestBase& Test, UWorld* World, const FString& ActorLabel,
                         const FVector& Location, UStaticMesh* Mesh, const TArray<FRotator>& Rotations,
                         const TArray<double>& ZOffsets = TArray<double>())
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
            Hism->AddInstance(FTransform(Rotations[Index], FVector(300.0 * Index, 0.0,
                ZOffsets.IsValidIndex(Index) ? ZOffsets[Index] : 0.0)));
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

    // A closed BASE-PIVOT octahedron: bottom vertex at the origin, top at z 200, a waist of four
    // vertices at z 100 and radius 50. It fills none of its box's corners, so a tilt drops the
    // rotated box's floor well below the mesh, while the mesh's lowest point is known exactly.
    const TArray<FVector>& DiamondPoints()
    {
        static const TArray<FVector> Points = {
            FVector(0, 0, 0), FVector(0, 0, 200),
            FVector(50, 0, 100), FVector(0, 50, 100), FVector(-50, 0, 100), FVector(0, -50, 100)};
        return Points;
    }

    // Lowest Z of the diamond's vertices under Rotation, from the analytic points rather than the
    // render data the verb reads, so the expectation does not share the code under test.
    double DiamondLowestZ(const FRotator& Rotation)
    {
        double Lowest = TNumericLimits<double>::Max();
        for (const FVector& Point : DiamondPoints())
        {
            Lowest = FMath::Min(Lowest, Rotation.RotateVector(Point).Z);
        }
        return Lowest;
    }

    // Lowest LOD0 render vertex under Transform, by full FTransform::TransformPosition per vertex
    // - a different code path from the verb's matrix-column shortcut, so a sphere miss can be told
    // apart: a tessellation gap moves both numbers, a solver defect moves only the response.
    TOptional<double> RenderLowestZ(const UStaticMesh* Mesh, const FTransform& Transform)
    {
        const FStaticMeshRenderData* RenderData = Mesh ? Mesh->GetRenderData() : nullptr;
        if (!RenderData || RenderData->LODResources.Num() == 0)
        {
            return TOptional<double>();
        }
        const FPositionVertexBuffer& Positions = RenderData->LODResources[0].VertexBuffers.PositionVertexBuffer;
        if (Positions.GetNumVertices() == 0 || !Positions.GetVertexData())
        {
            return TOptional<double>();
        }
        double Lowest = TNumericLimits<double>::Max();
        for (uint32 Index = 0; Index < Positions.GetNumVertices(); ++Index)
        {
            Lowest = FMath::Min(Lowest, Transform.TransformPosition(FVector(Positions.VertexPosition(Index))).Z);
        }
        return Lowest;
    }

    // The diamond as a transient static mesh with CPU-resident LOD0 render data.
    UStaticMesh* BuildDiamond()
    {
        UStaticMesh* Mesh = NewObject<UStaticMesh>(GetTransientPackage(),
            MakeUniqueObjectName(GetTransientPackage(), UStaticMesh::StaticClass(), TEXT("SM_PWGB_Diamond")));
        Mesh->GetStaticMaterials().Add(FStaticMaterial(nullptr, FName(TEXT("Slot"))));

        FMeshDescription Description;
        FStaticMeshAttributes Attributes(Description);
        Attributes.Register();
        TVertexAttributesRef<FVector3f> Positions = Attributes.GetVertexPositions();
        TVertexInstanceAttributesRef<FVector3f> Normals = Attributes.GetVertexInstanceNormals();
        TVertexInstanceAttributesRef<FVector3f> Tangents = Attributes.GetVertexInstanceTangents();
        TVertexInstanceAttributesRef<float> BinormalSigns = Attributes.GetVertexInstanceBinormalSigns();
        TVertexInstanceAttributesRef<FVector2f> UVs = Attributes.GetVertexInstanceUVs();
        UVs.SetNumChannels(1);
        const FPolygonGroupID Group = Description.CreatePolygonGroup();
        Attributes.GetPolygonGroupMaterialSlotNames()[Group] = FName(TEXT("Slot"));

        TArray<FVertexID> Vertices;
        for (const FVector& Point : DiamondPoints())
        {
            const FVertexID Vertex = Description.CreateVertex();
            Positions[Vertex] = FVector3f(Point);
            Vertices.Add(Vertex);
        }
        const auto AddTriangle = [&](int32 A, int32 B, int32 C)
        {
            const int32 Corners[3] = {A, B, C};
            const FVector3f Normal = FVector3f(FVector::CrossProduct(
                DiamondPoints()[C] - DiamondPoints()[A], DiamondPoints()[B] - DiamondPoints()[A]).GetSafeNormal());
            FVertexInstanceID Instances[3];
            for (int32 Corner = 0; Corner < 3; ++Corner)
            {
                Instances[Corner] = Description.CreateVertexInstance(Vertices[Corners[Corner]]);
                Normals[Instances[Corner]] = Normal;
                Tangents[Instances[Corner]] = FVector3f(1.0f, 0.0f, 0.0f);
                BinormalSigns[Instances[Corner]] = 1.0f;
                UVs.Set(Instances[Corner], 0, FVector2f(0.25f * Corner, 0.0f));
            }
            Description.CreateTriangle(Group, Instances);
        };
        for (int32 Side = 0; Side < 4; ++Side)
        {
            const int32 Waist = 2 + Side;
            const int32 Next = 2 + (Side + 1) % 4;
            AddTriangle(0, Next, Waist);
            AddTriangle(1, Waist, Next);
        }

        UStaticMesh::FBuildMeshDescriptionsParams BuildParams;
        BuildParams.bFastBuild = true;
        if (!Mesh->BuildFromMeshDescriptions({&Description}, BuildParams))
        {
            Mesh->MarkAsGarbage();
            return nullptr;
        }
        return Mesh;
    }

    TSharedPtr<FJsonObject> AnySolid()
    {
        TSharedPtr<FJsonObject> Surface = MakeShared<FJsonObject>();
        Surface->SetStringField(TEXT("preset"), TEXT("any_solid"));
        return Surface;
    }
}

// ---- #366: a tilted instance is seated on its mesh's lowest vertex, not its rotated AABB floor ----
//
// Reverting the fix (the plane back at the floor of Mesh bounds.TransformBy(instance)) fails this
// test on every tilted row: the 45-degree sphere's undersideZCm drops ~20.7 cm below its real
// bottom and its inflation reads ~20.7, each resting diamond is proposed a lift of 17-48 cm and its
// undersideZCm sits that far under the floor, and undersideModel reads bounds_plane. The
// preconditions assert that gap is real before anything is measured.

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGroundInstancesSeatOnLowestVertexTest,
    "PinWright.spatial.ground_instances.SeatsTiltedInstancesOnTheLowestVertex",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGroundInstancesSeatOnLowestVertexTest::RunTest(const FString& Parameters)
{
    namespace GBD = GroundBoundsDisclosureTest;

    UWorld* World = GBD::EditorWorld();
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no_editor_world"),
            TEXT("GEditor->GetEditorWorldContext().World() returned null; the lowest-vertex "
                 "assertions were stepped over."));
        return true;
    }

    // The helper itself: a mesh with no render data has no plane to offer.
    {
        TStrongObjectPtr<UStaticMesh> Empty(NewObject<UStaticMesh>(GetTransientPackage()));
        TestFalse(TEXT("LowestVertexZ is unset for a mesh with no render data"),
            GroundPlacement::LowestVertexZ(Empty.Get(), FTransform::Identity).IsSet());
        Empty->MarkAsGarbage();
    }

    const FString FloorLabel = GBD::Label(TEXT("Floor"));
    const FString HolderLabel = GBD::Label(TEXT("Spheres"));
    AActor* Floor = GBD::SpawnFloor(*this, World, FloorLabel);
    // CENTRE PIVOT. Instance 0 upright, instance 1 pitched 45 degrees; both centres 300 cm above
    // the floor. A sphere's lowest point does not move when it rotates; its rotated box's does.
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
        AddError(TEXT("lowest-vertex sphere fixture did not spawn"));
        return true;
    }
    GBD::FlushPhysics(World);

    const FBox LocalBox = Component->GetStaticMesh()->GetBounds().GetBox();
    const double Radius = LocalBox.GetExtent().Z;
    TestTrue(*FString::Printf(TEXT("precondition: the engine sphere is centre-pivot (%s)"), *LocalBox.ToString()),
        LocalBox.GetCenter().IsNearlyZero(0.01) && Radius > 10.0);
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
    const double TiltedBoxDrop = (Worlds[1].GetLocation().Z - Radius) - Boxes[1].Min.Z;
    TestTrue(*FString::Printf(TEXT("precondition: the pitched sphere's AABB floor sits %.2f cm below "
        "its real bottom"), TiltedBoxDrop), TiltedBoxDrop > 15.0);

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
    const TSharedPtr<FJsonObject>* SeatEcho = nullptr;
    FString EchoModel;
    if (Capture.Result->TryGetObjectField(TEXT("seat"), SeatEcho) && SeatEcho)
    {
        (*SeatEcho)->TryGetStringField(TEXT("undersideModel"), EchoModel);
    }
    TestEqual(TEXT("seat.undersideModel echoes lowest_vertex"), EchoModel, FString(TEXT("lowest_vertex")));

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
        FString Model;
        Body->TryGetStringField(TEXT("undersideModel"), Model);
        TestEqual(*FString::Printf(TEXT("instance %d: undersideModel is lowest_vertex"), Index),
            Model, FString(TEXT("lowest_vertex")));
        TestFalse(*FString::Printf(TEXT("instance %d: no boundsPlaneFallback"), Index),
            Body->HasField(TEXT("boundsPlaneFallback")));

        const double UndersideZ = GBD::Number(Body, TEXT("undersideZCm"), Missing);
        const double GroundZ = GBD::Number(Body, TEXT("groundZCm"), Missing);
        const TOptional<double> RenderLowest = GBD::RenderLowestZ(Component->GetStaticMesh(), Worlds[Index]);
        TestTrue(*FString::Printf(TEXT("instance %d: precondition: the sphere's LOD0 positions are "
            "CPU-resident"), Index), RenderLowest.IsSet());
        TestEqual(*FString::Printf(TEXT("instance %d: undersideZCm is the lowest render vertex"), Index),
            UndersideZ, RenderLowest.Get(Missing), 0.01);
        // The acceptance bound: the tessellated sphere's lowest vertex is within 0.5 cm of the
        // analytic bottom at any pitch.
        TestEqual(*FString::Printf(TEXT("instance %d: undersideZCm is the sphere's real bottom"), Index),
            UndersideZ, Worlds[Index].GetLocation().Z - Radius, 0.5);
        TestEqual(*FString::Printf(TEXT("instance %d: groundZCm is the floor top"), Index),
            GroundZ, GBD::FloorTopZ, 0.5);
        TestEqual(*FString::Printf(TEXT("instance %d: a sphere's tilt does not move its plane "
            "(boundsRotationInflationCm)"), Index),
            GBD::Number(Row, TEXT("boundsRotationInflationCm"), Missing), 0.0, 0.5);
        // The published plane is the solved one: seatPercentile 0 over flat ground, no embed.
        TestEqual(*FString::Printf(TEXT("instance %d: proposedDeltaZCm == groundZCm - undersideZCm"),
            Index), GBD::Number(Row, TEXT("proposedDeltaZCm"), Missing), GroundZ - UndersideZ, 0.05);
        // A flat plane has no relief, so none is claimed.
        TestFalse(*FString::Printf(TEXT("instance %d: undersideReliefCm is omitted under a plane"),
            Index), Body->HasField(TEXT("undersideReliefCm")));
        TestTrue(*FString::Printf(TEXT("instance %d: groundSpreadCm is still reported over 9 "
            "columns"), Index), Body->HasField(TEXT("groundSpreadCm")));
    }

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

    // ---- BASE PIVOT: tilted diamonds already resting on the floor. ----
    // Each instance is raised so its analytic lowest vertex touches the floor top. At 75 degrees a
    // waist vertex swings 22.4 cm below the pivot, so that row also pins a positive inflation.
    TStrongObjectPtr<UStaticMesh> Diamond(GBD::BuildDiamond());
    const FString DiamondLabel = GBD::Label(TEXT("Diamonds"));
    AActor* DiamondHolder = nullptr;
    ON_SCOPE_EXIT
    {
        if (DiamondHolder) { DiamondHolder->Destroy(); }
        if (Diamond.IsValid())
        {
            Diamond->ClearFlags(RF_Standalone | RF_Public);
            Diamond->MarkAsGarbage();
            Diamond.Reset();
        }
    };
    if (!Diamond.IsValid())
    {
        AddError(TEXT("could not build the base-pivot diamond mesh"));
        return true;
    }
    TestEqual(TEXT("LowestVertexZ reads the diamond's base vertex under a pure translation"),
        GroundPlacement::LowestVertexZ(Diamond.Get(), FTransform(FVector(0.0, 0.0, 7.0))).Get(Missing), 7.0, 0.01);

    const TArray<FRotator> Tilts = {FRotator(20.0, 0.0, 0.0), FRotator(0.0, 0.0, 35.0),
        FRotator(-40.0, 30.0, 10.0), FRotator(75.0, 0.0, 0.0)};
    TArray<double> Raise;
    for (const FRotator& Tilt : Tilts)
    {
        Raise.Add(GBD::FloorTopZ - GBD::DiamondLowestZ(Tilt));
    }
    DiamondHolder = GBD::SpawnScatter(*this, World, DiamondLabel,
        FVector(GBD::ColX - 600.0, GBD::ColY + 500.0, 0.0), Diamond.Get(), Tilts, Raise);
    UInstancedStaticMeshComponent* DiamondComponent =
        DiamondHolder ? DiamondHolder->FindComponentByClass<UInstancedStaticMeshComponent>() : nullptr;
    if (!DiamondComponent || DiamondComponent->GetInstanceCount() != Tilts.Num())
    {
        AddError(TEXT("diamond scatter did not spawn"));
        return true;
    }
    GBD::FlushPhysics(World);

    const FBox DiamondLocal = Diamond->GetBounds().GetBox();
    for (int32 Index = 0; Index < Tilts.Num(); ++Index)
    {
        FTransform InstanceWorld;
        DiamondComponent->GetInstanceTransform(Index, InstanceWorld, /*bWorldSpace*/ true);
        const double BoxGap = GBD::FloorTopZ - DiamondLocal.TransformBy(InstanceWorld).Min.Z;
        TestTrue(*FString::Printf(TEXT("precondition: diamond %d's rotated AABB floor is %.2f cm under "
            "the floor it rests on"), Index, BoxGap), BoxGap > 10.0);
    }

    Payload->SetStringField(TEXT("actorName"), DiamondLabel);
    Payload->SetNumberField(TEXT("samples"), 3);
    Payload->SetNumberField(TEXT("seatPercentile"), 0.0);
    FTestResponseCapture DiamondCapture;
    InvokeHandlerWithCapture(TEXT("spatial.ground_instances"), Payload, DiamondCapture);
    TestTrue(TEXT("the diamond dry run succeeds"), DiamondCapture.bSuccess);
    for (int32 Index = 0; Index < Tilts.Num(); ++Index)
    {
        const TSharedPtr<FJsonObject> Row = GBD::RowForIndex(DiamondCapture.Result, Index);
        const TSharedPtr<FJsonObject> Body = GBD::Contact(Row);
        const double Proposed = GBD::Number(Row, TEXT("proposedDeltaZCm"), Missing);
        TestTrue(*FString::Printf(TEXT("diamond %d (%s) resting on the floor: seatPercentile 0 proposes "
            "no move beyond the contact tolerance (proposedDeltaZCm %.3f)"), Index, *Tilts[Index].ToString(),
            Proposed), FMath::Abs(Proposed) <= 0.05);
        TestEqual(*FString::Printf(TEXT("diamond %d: undersideZCm is the floor it rests on"), Index),
            GBD::Number(Body, TEXT("undersideZCm"), Missing), GBD::FloorTopZ, 0.05);
        TestEqual(*FString::Printf(TEXT("diamond %d: boundsRotationInflationCm is the drop of its lowest "
            "vertex from the upright base"), Index),
            GBD::Number(Row, TEXT("boundsRotationInflationCm"), Missing), -GBD::DiamondLowestZ(Tilts[Index]), 0.05);
    }
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
