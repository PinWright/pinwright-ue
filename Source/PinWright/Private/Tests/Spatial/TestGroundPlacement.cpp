// Copyright (c) 2026 Alexander Penkin. MIT License.

// TestGroundPlacement.cpp - spatial.ground_actors / spatial.verify_grounding.
//
// Every test here asserts a FAILURE direction, because every ground-placement bug this feature
// exists to fix survived by looking like a success:
//   - an actor with nothing under it must report unplaced, not placed:true on an invented surface
//   - an actor whose pivot is not its lowest point must end up seated, not floating or buried
//   - an actor sunk into a surface must report penetration, not "no ground" (the blind spot in
//     spatial.verify_placement's grounded check, MeasureHandler.cpp:253-257)
//   - an actor overhanging the terrain edge must report partial coverage
//   - a wide actor resting on one contact point must be distinguishable from a seated one
//   - an empty selector must be an error, not a 0-of-0 success
//   - a name pattern on the mutating verb must be bounded by a stated expectedMatches, and a
//     disagreement must be refused before anything moves rather than reported as counts after
//   - every actor the mutating verb moves must leave an undo record, successes included and at
//     every detail level, because "the move succeeded" is not "the move was intended"
//
// Fixtures are spawned through the real actor.spawn RPC into the live editor world at an
// isolated far-away column, with GUID-suffixed labels, and destroyed on scope exit - matching
// Tests/Spatial/TestPlacementHandlers.cpp. Helper names carry a GroundTest prefix and live in
// one file-scope anonymous namespace, per the Unity-build rule in CLAUDE.md.

#include "Misc/AutomationTest.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/HandlerRegistration.h"
#include "Handlers/Spatial/GroundPlacementUtils.h"
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

namespace
{
    // The engine basic Cube is a 100 x 100 x 100 mesh with a CENTRED pivot, so its pivot is 50 cm
    // above its lowest geometry. That gap is the whole point: a solver that sets pivot Z to
    // ground Z buries this actor by half its height, and one that is bottom-aware does not.
    constexpr double GroundTestCubeSize = 100.0;
    constexpr double GroundTestCubeHalf = 50.0;

    // An isolated column of the editor world, well away from any real level geometry and away
    // from the column TestPlacementHandlers.cpp uses, so nothing else can answer a ground probe.
    constexpr double GroundTestColX = 331100.0;
    constexpr double GroundTestColY = 274500.0;

    FString GroundTestLabel(const TCHAR* Prefix)
    {
        return FString::Printf(TEXT("PWG_%s_%s"), Prefix, *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    }

    UWorld* GroundTestEditorWorld()
    {
        return GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    }

    TSharedPtr<FJsonObject> GroundTestVec(double X, double Y, double Z)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetNumberField(TEXT("x"), X);
        Obj->SetNumberField(TEXT("y"), Y);
        Obj->SetNumberField(TEXT("z"), Z);
        return Obj;
    }

    // Editor worlds don't tick physics on their own; a just-spawned body isn't in the scene-query
    // structure until a world tick flushes it, so a ground probe issued in the same call would
    // miss its own fixture. Editor worlds don't simulate, so nothing moves.
    void GroundTestFlushPhysics(UWorld* World)
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

    AActor* GroundTestSpawnCube(FAutomationTestBase& Test, UWorld* World, const FString& Label,
                                const FVector& Location)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("meshPath"), TEXT("/Engine/BasicShapes/Cube.Cube"));
        Payload->SetStringField(TEXT("actorName"), Label);
        Payload->SetObjectField(TEXT("location"), GroundTestVec(Location.X, Location.Y, Location.Z));

        FTestResponseCapture Capture;
        Test.TestTrue(TEXT("actor.spawn handler registered"),
            InvokeHandlerWithCapture(TEXT("actor.spawn"), Payload, Capture));
        Test.TestTrue(*FString::Printf(TEXT("cube '%s' spawned"), *Label), Capture.bSuccess);
        return McpActorUtils::FindActorByName(World, Label);
    }

    // An actor whose ROOT is a HISM carrying InstanceCount cube instances strung out along X -
    // the shape both ground verbs must refuse. One actor, one transform, and world bounds that
    // span the whole scatter, so the sampled footprint is the union of every instance and the
    // single SetActorTransform a seat solve applies would move all of them at once.
    //
    // Built directly rather than through actor.spawn because no RPC in this plugin makes one.
    // Spawned at the origin and moved afterwards: SetRootComponent on a just-spawned actor
    // re-seats the actor transform, so a spawn-time location would not survive it.
    AActor* GroundTestSpawnScatterHolder(FAutomationTestBase& Test, UWorld* World,
                                         const FString& Label, const FVector& Location,
                                         int32 InstanceCount, double SpacingCm)
    {
        UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
        if (!World || !Cube)
        {
            Test.AddError(TEXT("engine cube mesh unavailable for the scatter holder fixture"));
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
            NewObject<UHierarchicalInstancedStaticMeshComponent>(Holder, TEXT("HISM_GroundScatter"),
                RF_Transactional);
        Holder->SetRootComponent(Hism);
        Holder->AddInstanceComponent(Hism);
        Hism->OnComponentCreated();
        Hism->SetStaticMesh(Cube);
        Hism->RegisterComponent();
        for (int32 Index = 0; Index < InstanceCount; ++Index)
        {
            // Centred on the actor pivot, so the holder's bounds straddle it the way a real
            // scatter's do.
            const double OffsetX = (static_cast<double>(Index) - 0.5 * (InstanceCount - 1)) * SpacingCm;
            Hism->AddInstance(FTransform(FVector(OffsetX, 0.0, 0.0)));
        }
        Holder->SetActorLocation(Location);
        Holder->SetActorLabel(Label);
        return Holder;
    }

    // A SECOND instanced component on an existing holder, so a test can build the multi-tenant
    // shape the per-instance verbs must refuse to guess at: AInstancedFoliageActor is a per-level
    // singleton carrying one component per foliage type for EVERY caller in the level, so the
    // component with the most instances on it is routinely a stranger's scatter. Name, count and
    // Y offset are the caller's, so a test can make the FOREIGN component the bigger one and put
    // it over the same floor - a scatter the verb could genuinely have seated is the only way to
    // prove it did not.
    UInstancedStaticMeshComponent* GroundTestAddScatterComponent(FAutomationTestBase& Test,
                                                                 AActor* Holder,
                                                                 const TCHAR* ComponentName,
                                                                 int32 InstanceCount,
                                                                 double SpacingCm, double OffsetYCm)
    {
        UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
        if (!Holder || !Holder->GetRootComponent() || !Cube)
        {
            Test.AddError(TEXT("holder or engine cube mesh unavailable for the second scatter"));
            return nullptr;
        }

        UHierarchicalInstancedStaticMeshComponent* Hism =
            NewObject<UHierarchicalInstancedStaticMeshComponent>(Holder, ComponentName,
                RF_Transactional);
        // Attached before registration, so its instances land over the same floor as the root
        // scatter's rather than at the world origin where nothing would answer a ground probe.
        Hism->SetupAttachment(Holder->GetRootComponent());
        Holder->AddInstanceComponent(Hism);
        Hism->OnComponentCreated();
        Hism->SetStaticMesh(Cube);
        Hism->RegisterComponent();
        for (int32 Index = 0; Index < InstanceCount; ++Index)
        {
            const double OffsetX = (static_cast<double>(Index) - 0.5 * (InstanceCount - 1)) * SpacingCm;
            Hism->AddInstance(FTransform(FVector(OffsetX, OffsetYCm, 0.0)));
        }
        return Hism;
    }

    // A cube floor wide enough to answer a ground probe under an entire scatter, so the holder
    // tests exercise the case where the verbs COULD have produced an answer - and a move - rather
    // than failing out on missing ground for an unrelated reason.
    AActor* GroundTestSpawnWideFloor(FAutomationTestBase& Test, UWorld* World, const FString& Label,
                                     const FVector& Location, double XYScale)
    {
        AActor* Floor = GroundTestSpawnCube(Test, World, Label, Location);
        if (Floor)
        {
            Floor->SetActorScale3D(FVector(XYScale, XYScale, 1.0));
        }
        return Floor;
    }

    // {"preset":"any_solid"}. The landscape preset is the production default but there is no
    // landscape in the automation world, so these tests state any_solid explicitly - which also
    // exercises the exclusion filters harmlessly.
    TSharedPtr<FJsonObject> GroundTestAnySolidSurface()
    {
        TSharedPtr<FJsonObject> Surface = MakeShared<FJsonObject>();
        Surface->SetStringField(TEXT("preset"), TEXT("any_solid"));
        return Surface;
    }

    // Payload naming exactly one actor, with the any_solid surface already attached.
    TSharedPtr<FJsonObject> GroundTestPayloadFor(const FString& ActorLabel)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetObjectField(TEXT("surface"), GroundTestAnySolidSurface());
        TArray<TSharedPtr<FJsonValue>> Names;
        Names.Add(MakeShared<FJsonValueString>(ActorLabel));
        Payload->SetArrayField(TEXT("actors"), Names);
        Payload->SetStringField(TEXT("detail"), TEXT("all"));
        return Payload;
    }

    // World-space bottom Z of an actor, re-derived from the engine rather than from the RPC's own
    // response, so a handler that reports a transform it did not apply cannot pass.
    double GroundTestBottomZ(AActor* Actor)
    {
        FVector Origin = FVector::ZeroVector;
        FVector Extent = FVector::ZeroVector;
        Actor->GetActorBounds(false, Origin, Extent);
        return Origin.Z - Extent.Z;
    }

    // First element of results[], which detail:"all" makes exactly one row for a single-actor
    // call. Returned by value: an invalid pointer is indistinguishable from a valid one holding
    // nothing, and a test that silently skips its assertions is worse than no test.
    TSharedPtr<FJsonObject> GroundTestFirstRow(const TSharedPtr<FJsonObject>& Result)
    {
        if (!Result.IsValid())
        {
            return nullptr;
        }
        const TArray<TSharedPtr<FJsonValue>>* Rows = nullptr;
        if (!Result->TryGetArrayField(TEXT("results"), Rows) || !Rows || Rows->Num() == 0)
        {
            return nullptr;
        }
        return (*Rows)[0].IsValid() ? (*Rows)[0]->AsObject() : nullptr;
    }

    // ---- Per-instance fixtures (actor.get_instances / actor.set_instance_transforms /
    //      spatial.ground_instances) ----

    // The scatter's component. Ground truth for every per-instance assertion below: transforms are
    // re-read from the engine rather than from the response, so a verb that reports a pose it did
    // not write cannot pass - which is exactly how the property-layer route these verbs replace
    // looked correct while nothing moved.
    UInstancedStaticMeshComponent* GroundTestScatterComponent(AActor* Holder)
    {
        return Holder ? Holder->FindComponentByClass<UInstancedStaticMeshComponent>() : nullptr;
    }

    // One instance's WORLD transform, straight from the component.
    bool GroundTestInstanceWorld(AActor* Holder, int32 Index, FTransform& Out)
    {
        UInstancedStaticMeshComponent* Component = GroundTestScatterComponent(Holder);
        return Component && Component->GetInstanceTransform(Index, Out, /*bWorldSpace*/ true);
    }

    // Payload naming one holder actor, with the any_solid surface attached and detail:"all", for
    // the per-instance verbs. The read/write verbs ignore the surface field they do not declare,
    // so each test strips or keeps it as its verb requires.
    TSharedPtr<FJsonObject> GroundTestHolderPayload(const FString& ActorLabel)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("actorName"), ActorLabel);
        return Payload;
    }

    // The row of Result's named array whose "index" field equals Index, or null. Rows are keyed by
    // instance index rather than by position, because a verb is free to page or reorder and a
    // positional lookup would then assert about a different instance.
    TSharedPtr<FJsonObject> GroundTestRowForIndex(const TSharedPtr<FJsonObject>& Result,
                                                  const TCHAR* ArrayKey, int32 Index)
    {
        if (!Result.IsValid())
        {
            return nullptr;
        }
        const TArray<TSharedPtr<FJsonValue>>* Rows = nullptr;
        if (!Result->TryGetArrayField(ArrayKey, Rows) || !Rows)
        {
            return nullptr;
        }
        for (const TSharedPtr<FJsonValue>& Value : *Rows)
        {
            const TSharedPtr<FJsonObject> Row = Value.IsValid() ? Value->AsObject() : nullptr;
            if (!Row.IsValid())
            {
                continue;
            }
            double RowIndex = -1.0;
            if (Row->TryGetNumberField(TEXT("index"), RowIndex)
                && static_cast<int32>(RowIndex) == Index)
            {
                return Row;
            }
        }
        return nullptr;
    }

    // A nested {x,y,z} / {pitch,yaw,roll} member of a row, e.g. row.location.z.
    double GroundTestNestedNumber(const TSharedPtr<FJsonObject>& Row, const TCHAR* Object,
                                  const TCHAR* Field, double Fallback)
    {
        if (!Row.IsValid())
        {
            return Fallback;
        }
        const TSharedPtr<FJsonObject>* Nested = nullptr;
        if (!Row->TryGetObjectField(Object, Nested) || !Nested)
        {
            return Fallback;
        }
        double Value = Fallback;
        (*Nested)->TryGetNumberField(Field, Value);
        return Value;
    }

    // World-space top Z of an actor - the surface a scatter above it must come to rest on.
    double GroundTestTopZ(AActor* Actor)
    {
        FVector Origin = FVector::ZeroVector;
        FVector Extent = FVector::ZeroVector;
        Actor->GetActorBounds(false, Origin, Extent);
        return Origin.Z + Extent.Z;
    }

    // A mesh actor spawned AT an explicit scale. Separate from GroundTestSpawnCube rather than a
    // generalisation of it because the scale here is not decoration: the divergent-hull fixture
    // is built out of the scale itself, so it has to be in effect when the body is first built.
    AActor* GroundTestSpawnScaledMesh(FAutomationTestBase& Test, UWorld* World,
                                      const FString& Label, const TCHAR* MeshPath,
                                      const FVector& Location, const FVector& Scale)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("meshPath"), MeshPath);
        Payload->SetStringField(TEXT("actorName"), Label);
        Payload->SetObjectField(TEXT("location"), GroundTestVec(Location.X, Location.Y, Location.Z));
        Payload->SetObjectField(TEXT("scale"), GroundTestVec(Scale.X, Scale.Y, Scale.Z));

        FTestResponseCapture Capture;
        Test.TestTrue(TEXT("actor.spawn handler registered"),
            InvokeHandlerWithCapture(TEXT("actor.spawn"), Payload, Capture));
        Test.TestTrue(*FString::Printf(TEXT("scaled mesh '%s' spawned"), *Label), Capture.bSuccess);
        return McpActorUtils::FindActorByName(World, Label);
    }

    // ---- Narrow-contact fixture (spatial.ground_instances contactRadius) ----

    // A holder carrying CONE instances flipped apex-down (negative Z scale). The engine's basic
    // cone is the primitive whose contact patch is not its silhouette: its lowest geometry is a
    // point on the pivot axis while its footprint is the full disc, which is the tree case in
    // miniature - measured, HillTree_P2 has 656.8 x 801.3 uu of bounds half-extent around a
    // 109.7 x 102.8 uu contact patch, 46.7x the area. Built from a primitive rather than from a
    // content asset, because the measured meshes live in a different project.
    //
    // What the solve reads off this mesh is only its AABB - spatial.ground_instances models every
    // instance's underside as that box's bottom plane and cannot probe an instance's geometry per
    // column - so the fixture's job is to separate the two footprints, not to model cone
    // collision. Instances are placed at LOCAL offsets under a holder at Location, the way a real
    // scatter is; the caller fixes each one's world Z afterwards.
    AActor* GroundTestSpawnNarrowContactScatter(FAutomationTestBase& Test, UWorld* World,
                                                const FString& Label, const FVector& Location,
                                                const TArray<FVector>& LocalOffsets,
                                                const TArray<double>& UniformScales)
    {
        UStaticMesh* Cone = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cone.Cone"));
        if (!World || !Cone)
        {
            Test.AddError(TEXT("engine cone mesh unavailable for the narrow-contact fixture"));
            return nullptr;
        }

        AActor* Holder = World->SpawnActor<AActor>(AActor::StaticClass(), FVector::ZeroVector,
            FRotator::ZeroRotator);
        if (!Holder)
        {
            Test.AddError(TEXT("narrow-contact holder actor did not spawn"));
            return nullptr;
        }

        UHierarchicalInstancedStaticMeshComponent* Hism =
            NewObject<UHierarchicalInstancedStaticMeshComponent>(Holder,
                TEXT("HISM_NarrowContact"), RF_Transactional);
        Holder->SetRootComponent(Hism);
        Holder->AddInstanceComponent(Hism);
        Hism->OnComponentCreated();
        Hism->SetStaticMesh(Cone);
        Hism->RegisterComponent();
        for (int32 Index = 0; Index < LocalOffsets.Num(); ++Index)
        {
            // Negative Z scale flips the cone so its narrow end is the end that touches. Per
            // instance and uniform in XY, because the parameter under test is stated in
            // MESH-LOCAL cm and scaled by exactly that mean - a scatter whose instances differ in
            // size is the case a bounds RATIO cannot serve.
            const double Scale = UniformScales.IsValidIndex(Index) ? UniformScales[Index] : 1.0;
            Hism->AddInstance(FTransform(FRotator::ZeroRotator, LocalOffsets[Index],
                FVector(Scale, Scale, -Scale)));
        }
        Holder->SetActorLocation(Location);
        Holder->SetActorLabel(Label);
        return Holder;
    }

    // Move one instance so its world AABB minimum lands ExactlyBelowCm under TargetTopZ, using the
    // SAME box expression the verb solves against (mesh bounds under the instance world transform).
    // That is what makes "already correctly seated" a fact about the verb's own model rather than
    // an assumption about the mesh: a solve run against it must propose approximately nothing.
    bool GroundTestSeatInstanceExactly(UInstancedStaticMeshComponent* Component, int32 Index,
                                       double TargetTopZ, double ExactlyBelowCm)
    {
        FTransform World;
        if (!Component || !Component->GetStaticMesh()
            || !Component->GetInstanceTransform(Index, World, /*bWorldSpace*/ true))
        {
            return false;
        }
        const FBox Box = Component->GetStaticMesh()->GetBounds().GetBox().TransformBy(World);
        World.AddToTranslation(FVector(0.0, 0.0, (TargetTopZ - ExactlyBelowCm) - Box.Min.Z));
        return Component->UpdateInstanceTransform(Index, World, /*bWorldSpace*/ true,
            /*bMarkRenderStateDirty*/ true, /*bTeleport*/ true);
    }

    // The instance's world AABB, from the same expression GroundPlacement::SeatInstance uses.
    bool GroundTestInstanceBox(UInstancedStaticMeshComponent* Component, int32 Index, FBox& Out)
    {
        FTransform World;
        if (!Component || !Component->GetStaticMesh()
            || !Component->GetInstanceTransform(Index, World, /*bWorldSpace*/ true))
        {
            return false;
        }
        Out = Component->GetStaticMesh()->GetBounds().GetBox().TransformBy(World);
        return true;
    }

    // contact.footprintHalfExtentCm.<Axis> off a results[] row, or Fallback.
    double GroundTestRowFootprintHalf(const TSharedPtr<FJsonObject>& Row, const TCHAR* Axis,
                                      double Fallback)
    {
        if (!Row.IsValid())
        {
            return Fallback;
        }
        const TSharedPtr<FJsonObject>* Contact = nullptr;
        if (!Row->TryGetObjectField(TEXT("contact"), Contact) || !Contact)
        {
            return Fallback;
        }
        return GroundTestNestedNumber(*Contact, TEXT("footprintHalfExtentCm"), Axis, Fallback);
    }

    // A plain string/number member of a row's contact object.
    FString GroundTestRowContactString(const TSharedPtr<FJsonObject>& Row, const TCHAR* Field)
    {
        if (!Row.IsValid())
        {
            return FString();
        }
        const TSharedPtr<FJsonObject>* Contact = nullptr;
        if (!Row->TryGetObjectField(TEXT("contact"), Contact) || !Contact)
        {
            return FString();
        }
        FString Value;
        (*Contact)->TryGetStringField(Field, Value);
        return Value;
    }

    double GroundTestRowContactNumber(const TSharedPtr<FJsonObject>& Row, const TCHAR* Field,
                                      double Fallback)
    {
        if (!Row.IsValid())
        {
            return Fallback;
        }
        const TSharedPtr<FJsonObject>* Contact = nullptr;
        if (!Row->TryGetObjectField(TEXT("contact"), Contact) || !Contact)
        {
            return Fallback;
        }
        double Value = Fallback;
        (*Contact)->TryGetNumberField(Field, Value);
        return Value;
    }
}

// ---- The required-surface guarantee ----

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGroundActorsMissingSurfaceRejectedTest,
    "PinWright.spatial.ground_actors.MissingSurfaceRejected",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGroundActorsMissingSurfaceRejectedTest::RunTest(const FString& Parameters)
{
    // The structural guarantee: a caller cannot get a placement without stating what counts as
    // ground. If this ever starts defaulting, every failure mode the surface spec prevents comes
    // straight back.
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    TArray<TSharedPtr<FJsonValue>> Names;
    Names.Add(MakeShared<FJsonValueString>(TEXT("NoSuchActor")));
    Payload->SetArrayField(TEXT("actors"), Names);

    FTestResponseCapture Capture;
    TestTrue(TEXT("spatial.ground_actors handler registered"),
        InvokeHandlerWithCapture(TEXT("spatial.ground_actors"), Payload, Capture));
    TestFalse(TEXT("a missing surface is rejected, not defaulted"), Capture.bSuccess);
    TestEqual(TEXT("missing surface -> INVALID_SURFACE_SPEC"),
        Capture.ErrorCode, FString(TEXT("INVALID_SURFACE_SPEC")));

    // ...and an unknown preset must not degrade into "trace everything".
    TSharedPtr<FJsonObject> BadPreset = MakeShared<FJsonObject>();
    BadPreset->SetStringField(TEXT("preset"), TEXT("whatever"));
    TSharedPtr<FJsonObject> Payload2 = MakeShared<FJsonObject>();
    Payload2->SetObjectField(TEXT("surface"), BadPreset);
    Payload2->SetArrayField(TEXT("actors"), Names);

    FTestResponseCapture Capture2;
    InvokeHandlerWithCapture(TEXT("spatial.ground_actors"), Payload2, Capture2);
    TestFalse(TEXT("an unknown preset is rejected"), Capture2.bSuccess);
    TestEqual(TEXT("unknown preset -> INVALID_SURFACE_SPEC"),
        Capture2.ErrorCode, FString(TEXT("INVALID_SURFACE_SPEC")));
    return true;
}

// ---- An empty batch is an error, not a clean run ----

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGroundActorsNoMatchIsErrorTest,
    "PinWright.spatial.ground_actors.NoActorsMatchedIsError",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGroundActorsNoMatchIsErrorTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetObjectField(TEXT("surface"), GroundTestAnySolidSurface());
    Payload->SetStringField(TEXT("prefix"), TEXT("PWG_NoSuchPrefix_ZZZ_"));

    FTestResponseCapture Capture;
    TestTrue(TEXT("spatial.ground_actors handler registered"),
        InvokeHandlerWithCapture(TEXT("spatial.ground_actors"), Payload, Capture));
    // "0 of 0 placed" is exactly how a typo in a prefix passes for a successful pass over a level.
    TestFalse(TEXT("an empty match set is an error, not a 0-of-0 success"), Capture.bSuccess);
    TestEqual(TEXT("empty match -> NO_ACTORS_MATCHED"),
        Capture.ErrorCode, FString(TEXT("NO_ACTORS_MATCHED")));

    // No selector at all must also fail rather than defaulting to the whole level.
    TSharedPtr<FJsonObject> NoSelector = MakeShared<FJsonObject>();
    NoSelector->SetObjectField(TEXT("surface"), GroundTestAnySolidSurface());
    FTestResponseCapture Capture2;
    InvokeHandlerWithCapture(TEXT("spatial.ground_actors"), NoSelector, Capture2);
    TestFalse(TEXT("no selector is rejected rather than meaning 'every actor'"), Capture2.bSuccess);
    return true;
}

// ---- Nothing underneath must report unplaced ----

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGroundActorsNoGroundReportsUnplacedTest,
    "PinWright.spatial.ground_actors.NoGroundReportsUnplaced",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGroundActorsNoGroundReportsUnplacedTest::RunTest(const FString& Parameters)
{
    UWorld* World = GroundTestEditorWorld();
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("Editor world not available; skipping ground_actors no-ground test."));
        return true;
    }

    // A cube in open air with no floor spawned beneath it. This is the rock-in-the-sky case: the
    // old verb invented a surface at the caller's own point and reported placed:true.
    const FString Label = GroundTestLabel(TEXT("NoGround"));
    AActor* Cube = GroundTestSpawnCube(*this, World, Label,
        FVector(GroundTestColX, GroundTestColY, 90000.0));
    ON_SCOPE_EXIT
    {
        if (Cube) { Cube->Destroy(); }
    };
    if (!Cube)
    {
        AddError(TEXT("fixture cube did not spawn"));
        return true;
    }
    GroundTestFlushPhysics(World);

    const double BeforeZ = Cube->GetActorLocation().Z;

    TSharedPtr<FJsonObject> Payload = GroundTestPayloadFor(Label);
    // Bound the drop so the probe cannot reach whatever the real level has near Z=0.
    TSharedPtr<FJsonObject> Surface = GroundTestAnySolidSurface();
    Surface->SetNumberField(TEXT("maxDrop"), 1000.0);
    Payload->SetObjectField(TEXT("surface"), Surface);

    FTestResponseCapture Capture;
    TestTrue(TEXT("spatial.ground_actors handler registered"),
        InvokeHandlerWithCapture(TEXT("spatial.ground_actors"), Payload, Capture));
    TestTrue(TEXT("the batch call itself succeeds"), Capture.bSuccess);
    if (!Capture.bSuccess || !Capture.Result.IsValid())
    {
        return true;
    }

    // Hostile defaults: a missing field must fail the assertion, not satisfy it.
    double Placed = 1.0;
    double Failed = 0.0;
    double Moved = 1.0;
    Capture.Result->TryGetNumberField(TEXT("placed"), Placed);
    Capture.Result->TryGetNumberField(TEXT("failed"), Failed);
    Capture.Result->TryGetNumberField(TEXT("moved"), Moved);
    TestEqual(TEXT("nothing was placed"), Placed, 0.0);
    TestEqual(TEXT("the actor is counted as failed"), Failed, 1.0);
    TestEqual(TEXT("nothing was moved"), Moved, 0.0);

    if (const TSharedPtr<FJsonObject> Row = GroundTestFirstRow(Capture.Result))
    {
        bool bRowPlaced = true;
        Row->TryGetBoolField(TEXT("placed"), bRowPlaced);
        TestFalse(TEXT("the per-actor row reports placed:false"), bRowPlaced);

        FString ReasonCode;
        Row->TryGetStringField(TEXT("reasonCode"), ReasonCode);
        TestEqual(TEXT("no ground -> GROUND_NOT_FOUND"), ReasonCode, FString(TEXT("GROUND_NOT_FOUND")));
    }
    else
    {
        AddError(TEXT("ground_actors result missing the per-actor results row"));
    }

    // Ground truth, re-read from the engine: an unplaceable actor must not have been moved.
    TestEqual(TEXT("an unplaceable actor is left exactly where it was"),
        Cube->GetActorLocation().Z, BeforeZ);
    return true;
}

// ---- A pivot that is not the lowest point must end up SEATED ----

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGroundActorsSeatsBottomNotPivotTest,
    "PinWright.spatial.ground_actors.SeatsBottomNotPivot",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGroundActorsSeatsBottomNotPivotTest::RunTest(const FString& Parameters)
{
    UWorld* World = GroundTestEditorWorld();
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("Editor world not available; skipping ground_actors seat test."));
        return true;
    }

    // Floor spans Z -100..0, so its top face is exactly Z = 0.
    const FString FloorLabel = GroundTestLabel(TEXT("SeatFloor"));
    const FString DropLabel = GroundTestLabel(TEXT("SeatDrop"));
    AActor* Floor = GroundTestSpawnCube(*this, World, FloorLabel,
        FVector(GroundTestColX, GroundTestColY, -GroundTestCubeHalf));
    AActor* Drop = GroundTestSpawnCube(*this, World, DropLabel,
        FVector(GroundTestColX, GroundTestColY, 800.0));
    ON_SCOPE_EXIT
    {
        if (Floor) { Floor->Destroy(); }
        if (Drop) { Drop->Destroy(); }
    };
    if (!Floor || !Drop)
    {
        AddError(TEXT("fixture cubes did not spawn"));
        return true;
    }
    GroundTestFlushPhysics(World);

    TSharedPtr<FJsonObject> Payload = GroundTestPayloadFor(DropLabel);
    // embedFraction 0 makes the expected answer exact: the bounds bottom lands ON the surface.
    Payload->SetNumberField(TEXT("embedFraction"), 0.0);

    FTestResponseCapture Capture;
    TestTrue(TEXT("spatial.ground_actors handler registered"),
        InvokeHandlerWithCapture(TEXT("spatial.ground_actors"), Payload, Capture));
    TestTrue(TEXT("the batch call succeeds"), Capture.bSuccess);
    if (!Capture.bSuccess || !Capture.Result.IsValid())
    {
        return true;
    }

    double Placed = 0.0;
    double Failed = 1.0;
    double Requested = 0.0;
    Capture.Result->TryGetNumberField(TEXT("placed"), Placed);
    Capture.Result->TryGetNumberField(TEXT("failed"), Failed);
    Capture.Result->TryGetNumberField(TEXT("requested"), Requested);
    TestEqual(TEXT("the actor is placed"), Placed, 1.0);
    TestEqual(TEXT("nothing failed"), Failed, 0.0);
    // The counting invariant: every requested actor lands in exactly one bucket, so a batch can
    // never silently drop an actor out of both.
    TestEqual(TEXT("placed + failed == requested"), Placed + Failed, Requested);

    // Ground truth from the engine. The cube's pivot is 50 cm above its lowest geometry; a solver
    // that set pivot Z to ground Z would leave BottomZ at -50 (half buried), and one that never
    // moved it would leave BottomZ at 750.
    const double BottomZ = GroundTestBottomZ(Drop);
    TestTrue(*FString::Printf(TEXT("bounds bottom rests at Z ~ 0 (got %.3f)"), BottomZ),
        FMath::Abs(BottomZ) < 1.0);
    return true;
}

// ---- Embedding must actually sink the actor ----

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGroundActorsEmbedSinksActorTest,
    "PinWright.spatial.ground_actors.EmbedSinksActor",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGroundActorsEmbedSinksActorTest::RunTest(const FString& Parameters)
{
    UWorld* World = GroundTestEditorWorld();
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("Editor world not available; skipping ground_actors embed test."));
        return true;
    }

    const FString FloorLabel = GroundTestLabel(TEXT("EmbedFloor"));
    const FString DropLabel = GroundTestLabel(TEXT("EmbedDrop"));
    AActor* Floor = GroundTestSpawnCube(*this, World, FloorLabel,
        FVector(GroundTestColX, GroundTestColY, -GroundTestCubeHalf));
    AActor* Drop = GroundTestSpawnCube(*this, World, DropLabel,
        FVector(GroundTestColX, GroundTestColY, 800.0));
    ON_SCOPE_EXIT
    {
        if (Floor) { Floor->Destroy(); }
        if (Drop) { Drop->Destroy(); }
    };
    if (!Floor || !Drop)
    {
        AddError(TEXT("fixture cubes did not spawn"));
        return true;
    }
    GroundTestFlushPhysics(World);

    // 5% of a 100 cm tall cube = 5 cm of deliberate bedding.
    const double EmbedFraction = 0.05;
    const double ExpectedEmbed = EmbedFraction * GroundTestCubeSize;

    TSharedPtr<FJsonObject> Payload = GroundTestPayloadFor(DropLabel);
    Payload->SetNumberField(TEXT("embedFraction"), EmbedFraction);

    FTestResponseCapture Capture;
    InvokeHandlerWithCapture(TEXT("spatial.ground_actors"), Payload, Capture);
    TestTrue(TEXT("the batch call succeeds"), Capture.bSuccess);
    if (!Capture.bSuccess)
    {
        return true;
    }

    // A rock resting exactly tangent to the ground reads as balanced; bedding it in is the fix,
    // so the bottom must end up BELOW the surface by the requested amount.
    const double BottomZ = GroundTestBottomZ(Drop);
    TestTrue(*FString::Printf(TEXT("embedded bottom is below the surface (got %.3f)"), BottomZ),
        BottomZ < -0.5);
    TestTrue(*FString::Printf(TEXT("embed depth ~ %.1f cm (got %.3f)"), ExpectedEmbed, -BottomZ),
        FMath::Abs(-BottomZ - ExpectedEmbed) < 1.0);
    return true;
}

// ---- verify_grounding must SEE a floating actor ----

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVerifyGroundingFloatingFailsTest,
    "PinWright.spatial.verify_grounding.FloatingActorFails",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FVerifyGroundingFloatingFailsTest::RunTest(const FString& Parameters)
{
    UWorld* World = GroundTestEditorWorld();
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("Editor world not available; skipping verify_grounding floating test."));
        return true;
    }

    const FString FloorLabel = GroundTestLabel(TEXT("FloatFloor"));
    const FString FloatLabel = GroundTestLabel(TEXT("FloatCube"));
    AActor* Floor = GroundTestSpawnCube(*this, World, FloorLabel,
        FVector(GroundTestColX, GroundTestColY, -GroundTestCubeHalf));
    // Bottom at Z = 150, i.e. 150 cm of clear air under it.
    AActor* Floater = GroundTestSpawnCube(*this, World, FloatLabel,
        FVector(GroundTestColX, GroundTestColY, 200.0));
    ON_SCOPE_EXIT
    {
        if (Floor) { Floor->Destroy(); }
        if (Floater) { Floater->Destroy(); }
    };
    if (!Floor || !Floater)
    {
        AddError(TEXT("fixture cubes did not spawn"));
        return true;
    }
    GroundTestFlushPhysics(World);

    TSharedPtr<FJsonObject> Payload = GroundTestPayloadFor(FloatLabel);

    FTestResponseCapture Capture;
    TestTrue(TEXT("spatial.verify_grounding handler registered"),
        InvokeHandlerWithCapture(TEXT("spatial.verify_grounding"), Payload, Capture));
    TestTrue(TEXT("the verify call succeeds"), Capture.bSuccess);
    if (!Capture.bSuccess || !Capture.Result.IsValid())
    {
        return true;
    }

    bool bPass = true;
    Capture.Result->TryGetBoolField(TEXT("pass"), bPass);
    TestFalse(TEXT("a floating actor does not pass"), bPass);

    if (const TSharedPtr<FJsonObject> Row = GroundTestFirstRow(Capture.Result))
    {
        const TSharedPtr<FJsonObject>* Contact = nullptr;
        if (Row->TryGetObjectField(TEXT("contact"), Contact) && Contact)
        {
            FString FailCode;
            (*Contact)->TryGetStringField(TEXT("failCode"), FailCode);
            TestEqual(TEXT("floating -> ACTOR_NOT_GROUNDED"), FailCode,
                FString(TEXT("ACTOR_NOT_GROUNDED")));

            double MaxGap = 0.0;
            (*Contact)->TryGetNumberField(TEXT("maxGapCm"), MaxGap);
            TestTrue(*FString::Printf(TEXT("maxGapCm ~ 150 (got %.3f)"), MaxGap),
                FMath::Abs(MaxGap - 150.0) < 2.0);

            double ContactPoints = 1.0;
            (*Contact)->TryGetNumberField(TEXT("contactPoints"), ContactPoints);
            TestEqual(TEXT("a floating actor touches nothing"), ContactPoints, 0.0);
        }
        else
        {
            AddError(TEXT("verify_grounding row missing the contact block"));
        }
    }
    else
    {
        AddError(TEXT("verify_grounding result missing the per-actor results row"));
    }
    return true;
}

// ---- verify_grounding must SEE a sunk actor (the old grounded check could not) ----

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVerifyGroundingSunkReportsPenetrationTest,
    "PinWright.spatial.verify_grounding.SunkActorReportsPenetration",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FVerifyGroundingSunkReportsPenetrationTest::RunTest(const FString& Parameters)
{
    UWorld* World = GroundTestEditorWorld();
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("Editor world not available; skipping verify_grounding sunk test."));
        return true;
    }

    const FString FloorLabel = GroundTestLabel(TEXT("SunkFloor"));
    const FString SunkLabel = GroundTestLabel(TEXT("SunkCube"));
    // Floor top face at Z = 0.
    AActor* Floor = GroundTestSpawnCube(*this, World, FloorLabel,
        FVector(GroundTestColX, GroundTestColY, -GroundTestCubeHalf));
    // Bottom at Z = -20: the cube pokes up through the floor, buried 20 cm.
    AActor* Sunk = GroundTestSpawnCube(*this, World, SunkLabel,
        FVector(GroundTestColX, GroundTestColY, GroundTestCubeHalf - 20.0));
    ON_SCOPE_EXIT
    {
        if (Floor) { Floor->Destroy(); }
        if (Sunk) { Sunk->Destroy(); }
    };
    if (!Floor || !Sunk)
    {
        AddError(TEXT("fixture cubes did not spawn"));
        return true;
    }
    GroundTestFlushPhysics(World);

    TSharedPtr<FJsonObject> Payload = GroundTestPayloadFor(SunkLabel);
    Payload->SetNumberField(TEXT("maxPenetration"), 2.0);

    FTestResponseCapture Capture;
    InvokeHandlerWithCapture(TEXT("spatial.verify_grounding"), Payload, Capture);
    TestTrue(TEXT("the verify call succeeds"), Capture.bSuccess);
    if (!Capture.bSuccess || !Capture.Result.IsValid())
    {
        return true;
    }

    bool bPass = true;
    Capture.Result->TryGetBoolField(TEXT("pass"), bPass);
    TestFalse(TEXT("an actor buried 20 cm does not pass a 2 cm penetration limit"), bPass);

    if (const TSharedPtr<FJsonObject> Row = GroundTestFirstRow(Capture.Result))
    {
        const TSharedPtr<FJsonObject>* Contact = nullptr;
        if (Row->TryGetObjectField(TEXT("contact"), Contact) && Contact)
        {
            // The load-bearing assertion: penetration is MEASURED as a number, where
            // spatial.verify_placement's downward-from-the-bottom ray can only report a miss
            // (MeasureHandler.cpp:253-257). A miss and a 20 cm burial must not look alike.
            double Penetration = 0.0;
            (*Contact)->TryGetNumberField(TEXT("penetrationCm"), Penetration);
            TestTrue(*FString::Printf(TEXT("penetrationCm ~ 20 (got %.3f)"), Penetration),
                FMath::Abs(Penetration - 20.0) < 2.0);

            double Supported = 0.0;
            (*Contact)->TryGetNumberField(TEXT("supportedColumns"), Supported);
            TestTrue(TEXT("a sunk actor still reports ground under it, not a miss"), Supported > 0.0);

            FString FailCode;
            (*Contact)->TryGetStringField(TEXT("failCode"), FailCode);
            TestEqual(TEXT("buried -> ACTOR_BURIED"), FailCode, FString(TEXT("ACTOR_BURIED")));
        }
        else
        {
            AddError(TEXT("verify_grounding row missing the contact block"));
        }
    }
    else
    {
        AddError(TEXT("verify_grounding result missing the per-actor results row"));
    }
    return true;
}

// ---- An actor overhanging the edge must report partial coverage ----

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVerifyGroundingPartialCoverageFailsTest,
    "PinWright.spatial.verify_grounding.PartialCoverageFails",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FVerifyGroundingPartialCoverageFailsTest::RunTest(const FString& Parameters)
{
    UWorld* World = GroundTestEditorWorld();
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("Editor world not available; skipping verify_grounding coverage test."));
        return true;
    }

    // Floor spans X [Col-50, Col+50]. The target sits 70 cm along +X, so only its trailing
    // sample column is over the floor - the single-centre-point probe that this feature replaces
    // would report the actor perfectly grounded, because its CENTRE is off the floor entirely.
    const FString FloorLabel = GroundTestLabel(TEXT("EdgeFloor"));
    const FString EdgeLabel = GroundTestLabel(TEXT("EdgeCube"));
    AActor* Floor = GroundTestSpawnCube(*this, World, FloorLabel,
        FVector(GroundTestColX, GroundTestColY, -GroundTestCubeHalf));
    AActor* Edge = GroundTestSpawnCube(*this, World, EdgeLabel,
        FVector(GroundTestColX + 70.0, GroundTestColY, GroundTestCubeHalf));
    ON_SCOPE_EXIT
    {
        if (Floor) { Floor->Destroy(); }
        if (Edge) { Edge->Destroy(); }
    };
    if (!Floor || !Edge)
    {
        AddError(TEXT("fixture cubes did not spawn"));
        return true;
    }
    GroundTestFlushPhysics(World);

    TSharedPtr<FJsonObject> Payload = GroundTestPayloadFor(EdgeLabel);
    // Bound the drop so the probe cannot find whatever the real level has far below.
    TSharedPtr<FJsonObject> Surface = GroundTestAnySolidSurface();
    Surface->SetNumberField(TEXT("maxDrop"), 1000.0);
    Payload->SetObjectField(TEXT("surface"), Surface);

    FTestResponseCapture Capture;
    InvokeHandlerWithCapture(TEXT("spatial.verify_grounding"), Payload, Capture);
    TestTrue(TEXT("the verify call succeeds"), Capture.bSuccess);
    if (!Capture.bSuccess || !Capture.Result.IsValid())
    {
        return true;
    }

    bool bPass = true;
    Capture.Result->TryGetBoolField(TEXT("pass"), bPass);
    TestFalse(TEXT("an actor two-thirds off the floor does not pass"), bPass);

    if (const TSharedPtr<FJsonObject> Row = GroundTestFirstRow(Capture.Result))
    {
        const TSharedPtr<FJsonObject>* Contact = nullptr;
        if (Row->TryGetObjectField(TEXT("contact"), Contact) && Contact)
        {
            double Coverage = 1.0;
            (*Contact)->TryGetNumberField(TEXT("coverage"), Coverage);
            TestTrue(*FString::Printf(TEXT("coverage is partial (got %.3f)"), Coverage),
                Coverage < 0.99);

            FString FailCode;
            (*Contact)->TryGetStringField(TEXT("failCode"), FailCode);
            TestEqual(TEXT("overhang -> PARTIAL_GROUND_COVERAGE"), FailCode,
                FString(TEXT("PARTIAL_GROUND_COVERAGE")));
        }
        else
        {
            AddError(TEXT("verify_grounding row missing the contact block"));
        }
    }
    else
    {
        AddError(TEXT("verify_grounding result missing the per-actor results row"));
    }
    return true;
}

// ---- Balanced-on-one-step vs. seated: seatPercentile must change the contact count ----

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGroundActorsPercentileChangesContactTest,
    "PinWright.spatial.ground_actors.SeatPercentileChangesContact",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGroundActorsPercentileChangesContactTest::RunTest(const FString& Parameters)
{
    UWorld* World = GroundTestEditorWorld();
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("Editor world not available; skipping seatPercentile test."));
        return true;
    }

    // Two abutting floor cubes 10 cm apart in height: a step under the target's footprint.
    // Low floor top = 0 over X [Col-100, Col]; high floor top = 10 over X [Col, Col+100].
    const FString LowLabel = GroundTestLabel(TEXT("StepLow"));
    const FString HighLabel = GroundTestLabel(TEXT("StepHigh"));
    const FString TargetLabel = GroundTestLabel(TEXT("StepTarget"));
    AActor* Low = GroundTestSpawnCube(*this, World, LowLabel,
        FVector(GroundTestColX - GroundTestCubeHalf, GroundTestColY, -GroundTestCubeHalf));
    AActor* High = GroundTestSpawnCube(*this, World, HighLabel,
        FVector(GroundTestColX + GroundTestCubeHalf, GroundTestColY, -GroundTestCubeHalf + 10.0));
    AActor* Target = GroundTestSpawnCube(*this, World, TargetLabel,
        FVector(GroundTestColX, GroundTestColY, 600.0));
    ON_SCOPE_EXIT
    {
        if (Low) { Low->Destroy(); }
        if (High) { High->Destroy(); }
        if (Target) { Target->Destroy(); }
    };
    if (!Low || !High || !Target)
    {
        AddError(TEXT("fixture cubes did not spawn"));
        return true;
    }
    GroundTestFlushPhysics(World);

    // seatPercentile 0 = rest on first contact. Physically correct, and it is what leaves a
    // boulder balanced: the low half of the footprint hangs in the air.
    TSharedPtr<FJsonObject> RestPayload = GroundTestPayloadFor(TargetLabel);
    RestPayload->SetNumberField(TEXT("seatPercentile"), 0.0);
    RestPayload->SetNumberField(TEXT("embedFraction"), 0.0);
    FTestResponseCapture RestCapture;
    InvokeHandlerWithCapture(TEXT("spatial.ground_actors"), RestPayload, RestCapture);
    TestTrue(TEXT("the first-contact seat succeeds"), RestCapture.bSuccess);

    double RestMaxGap = 0.0;
    if (RestCapture.bSuccess)
    {
        if (const TSharedPtr<FJsonObject> Row = GroundTestFirstRow(RestCapture.Result))
        {
            const TSharedPtr<FJsonObject>* Contact = nullptr;
            if (Row->TryGetObjectField(TEXT("contact"), Contact) && Contact)
            {
                (*Contact)->TryGetNumberField(TEXT("maxGapCm"), RestMaxGap);
            }
        }
    }
    // The step is 10 cm, so resting on the high side leaves the low side ~10 cm in the air.
    TestTrue(*FString::Printf(TEXT("first-contact seat leaves part of the footprint floating "
        "(maxGapCm %.3f)"), RestMaxGap), RestMaxGap > 5.0);

    // seatPercentile 1 = sink until NO column floats. This is the fix for the balanced look.
    TSharedPtr<FJsonObject> FlushPayload = GroundTestPayloadFor(TargetLabel);
    FlushPayload->SetNumberField(TEXT("seatPercentile"), 1.0);
    FlushPayload->SetNumberField(TEXT("embedFraction"), 0.0);
    FTestResponseCapture FlushCapture;
    InvokeHandlerWithCapture(TEXT("spatial.ground_actors"), FlushPayload, FlushCapture);
    TestTrue(TEXT("the flush seat succeeds"), FlushCapture.bSuccess);
    if (!FlushCapture.bSuccess)
    {
        return true;
    }

    if (const TSharedPtr<FJsonObject> Row = GroundTestFirstRow(FlushCapture.Result))
    {
        const TSharedPtr<FJsonObject>* Contact = nullptr;
        if (Row->TryGetObjectField(TEXT("contact"), Contact) && Contact)
        {
            double MaxGap = 100.0;
            (*Contact)->TryGetNumberField(TEXT("maxGapCm"), MaxGap);
            TestTrue(*FString::Printf(TEXT("no part of the footprint floats (maxGapCm %.3f)"), MaxGap),
                MaxGap < 1.0);

            double ContactPoints = 0.0;
            double Supported = 0.0;
            (*Contact)->TryGetNumberField(TEXT("contactPoints"), ContactPoints);
            (*Contact)->TryGetNumberField(TEXT("supportedColumns"), Supported);
            // Every supported column touches - more than one contact point, which is the
            // difference between "seated" and "balanced".
            TestTrue(TEXT("more than one footprint column is in contact"), ContactPoints > 1.0);
            TestEqual(TEXT("every supported column is in contact"), ContactPoints, Supported);
        }
        else
        {
            AddError(TEXT("ground_actors row missing the contact block"));
        }
    }
    else
    {
        AddError(TEXT("ground_actors result missing the per-actor results row"));
    }
    return true;
}

// ---- A name pattern is not a scope: the mutating verb must be told what it should match ----

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGroundActorsPatternScopeGuardTest,
    "PinWright.spatial.ground_actors.PatternSelectorNeedsExpectedMatches",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGroundActorsPatternScopeGuardTest::RunTest(const FString& Parameters)
{
    UWorld* World = GroundTestEditorWorld();
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no_editor_world"),
            TEXT("GEditor->GetEditorWorldContext().World() returned null; the pattern-scope "
                 "assertions were stepped over."));
        return true;
    }

    // Two actors sharing one label prefix - "mine" and "somebody else's". The prefix is
    // GUID-derived, so nothing else in the level can answer to it and totalMatches is exactly 2.
    const FString SharedPrefix = GroundTestLabel(TEXT("Scope"));
    AActor* Mine = GroundTestSpawnCube(*this, World, SharedPrefix + TEXT("_Mine"),
        FVector(GroundTestColX, GroundTestColY, 90000.0));
    AActor* Foreign = GroundTestSpawnCube(*this, World, SharedPrefix + TEXT("_Foreign"),
        FVector(GroundTestColX + 400.0, GroundTestColY, 90000.0));
    ON_SCOPE_EXIT
    {
        if (Mine) { Mine->Destroy(); }
        if (Foreign) { Foreign->Destroy(); }
    };
    if (!Mine || !Foreign)
    {
        AddError(TEXT("fixture cubes did not spawn"));
        return true;
    }
    GroundTestFlushPhysics(World);

    const FVector MineBefore = Mine->GetActorLocation();
    const FVector ForeignBefore = Foreign->GetActorLocation();

    // 1. A bare pattern selector on the MUTATING verb is refused outright. There is no safe
    //    default for "how wide is this pattern allowed to be" in a level someone else is also
    //    editing, so the verb asks instead of guessing.
    TSharedPtr<FJsonObject> Bare = MakeShared<FJsonObject>();
    Bare->SetObjectField(TEXT("surface"), GroundTestAnySolidSurface());
    Bare->SetStringField(TEXT("prefix"), SharedPrefix);

    FTestResponseCapture BareCapture;
    TestTrue(TEXT("spatial.ground_actors handler registered"),
        InvokeHandlerWithCapture(TEXT("spatial.ground_actors"), Bare, BareCapture));
    TestFalse(TEXT("a bare pattern selector is refused, not run"), BareCapture.bSuccess);
    TestEqual(TEXT("bare pattern -> MISSING_REQUIRED_PARAM"),
        BareCapture.ErrorCode, FString(TEXT("MISSING_REQUIRED_PARAM")));

    // 2. A stated count the level disagrees with is refused BEFORE anything moves, and the
    //    refusal names what the pattern caught. This is the case the fix exists for: the same
    //    call used to seat both actors and report it only as a totalMatches number the caller
    //    had to independently know the expected value of.
    TSharedPtr<FJsonObject> Wrong = MakeShared<FJsonObject>();
    Wrong->SetObjectField(TEXT("surface"), GroundTestAnySolidSurface());
    Wrong->SetStringField(TEXT("prefix"), SharedPrefix);
    Wrong->SetNumberField(TEXT("expectedMatches"), 1);

    FTestResponseCapture WrongCapture;
    InvokeHandlerWithCapture(TEXT("spatial.ground_actors"), Wrong, WrongCapture);
    TestFalse(TEXT("a selector that caught more than the caller expected is refused"),
        WrongCapture.bSuccess);
    TestEqual(TEXT("count disagreement -> MATCH_COUNT_MISMATCH"),
        WrongCapture.ErrorCode, FString(TEXT("MATCH_COUNT_MISMATCH")));

    if (WrongCapture.Result.IsValid())
    {
        // Hostile default: a missing field must fail the assertion, not satisfy it.
        double Total = 0.0;
        WrongCapture.Result->TryGetNumberField(TEXT("totalMatches"), Total);
        TestEqual(TEXT("the refusal reports the true match count"), Total, 2.0);

        const TArray<TSharedPtr<FJsonValue>>* Named = nullptr;
        if (WrongCapture.Result->TryGetArrayField(TEXT("matchedActors"), Named) && Named)
        {
            TArray<FString> Labels;
            for (const TSharedPtr<FJsonValue>& Value : *Named)
            {
                FString Item;
                if (Value.IsValid() && Value->TryGetString(Item))
                {
                    Labels.Add(Item);
                }
            }
            // A count cannot say WHICH actors a pattern caught. The names can, and that is the
            // difference between "the number is wrong" and "these ones are not mine".
            TestTrue(TEXT("the refusal names the caller's own actor"),
                Labels.Contains(Mine->GetActorLabel()));
            TestTrue(TEXT("the refusal names the actor the pattern caught by accident"),
                Labels.Contains(Foreign->GetActorLabel()));
        }
        else
        {
            AddError(TEXT("MATCH_COUNT_MISMATCH carried no matchedActors list"));
        }
    }
    else
    {
        AddError(TEXT("MATCH_COUNT_MISMATCH carried no detail object"));
    }

    // Ground truth, re-read from the engine rather than from the response: a refused call moves
    // nothing. Before the fix both of these actors had been relocated by now.
    TestTrue(TEXT("the caller's own actor was not moved by a refused call"),
        Mine->GetActorLocation().Equals(MineBefore, 0.001));
    TestTrue(TEXT("the foreign actor was not moved by a refused call"),
        Foreign->GetActorLocation().Equals(ForeignBefore, 0.001));

    // 3. The guard rejects a WRONG scope, not every pattern. Nothing is under these cubes, so
    //    the batch places nothing - what this asserts is that the call was allowed to run.
    TSharedPtr<FJsonObject> Right = MakeShared<FJsonObject>();
    TSharedPtr<FJsonObject> BoundedSurface = GroundTestAnySolidSurface();
    BoundedSurface->SetNumberField(TEXT("maxDrop"), 1000.0);
    Right->SetObjectField(TEXT("surface"), BoundedSurface);
    Right->SetStringField(TEXT("prefix"), SharedPrefix);
    Right->SetNumberField(TEXT("expectedMatches"), 2);

    FTestResponseCapture RightCapture;
    InvokeHandlerWithCapture(TEXT("spatial.ground_actors"), Right, RightCapture);
    TestTrue(TEXT("a pattern whose stated count matches the level is allowed to run"),
        RightCapture.bSuccess);
    return true;
}

// ---- Every move leaves an undo record, successes included, at every detail level ----

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGroundActorsMovedUndoRecordTest,
    "PinWright.spatial.ground_actors.MovedActorsEchoPreviousTransform",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGroundActorsMovedUndoRecordTest::RunTest(const FString& Parameters)
{
    UWorld* World = GroundTestEditorWorld();
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no_editor_world"),
            TEXT("GEditor->GetEditorWorldContext().World() returned null; the undo-record "
                 "assertions were stepped over."));
        return true;
    }

    // Floor spans Z -100..0, so its top face is exactly Z = 0; the drop cube starts 800 cm up.
    const FString FloorLabel = GroundTestLabel(TEXT("UndoFloor"));
    const FString DropLabel = GroundTestLabel(TEXT("UndoDrop"));
    AActor* Floor = GroundTestSpawnCube(*this, World, FloorLabel,
        FVector(GroundTestColX, GroundTestColY, -GroundTestCubeHalf));
    AActor* Drop = GroundTestSpawnCube(*this, World, DropLabel,
        FVector(GroundTestColX, GroundTestColY, 800.0));
    ON_SCOPE_EXIT
    {
        if (Floor) { Floor->Destroy(); }
        if (Drop) { Drop->Destroy(); }
    };
    if (!Floor || !Drop)
    {
        AddError(TEXT("fixture cubes did not spawn"));
        return true;
    }
    GroundTestFlushPhysics(World);

    // ---- detail:"summary" - the level at which six foreign actors were moved with nothing in
    // the response naming them or saying where they had been. ----
    const FVector BeforeFirst = Drop->GetActorLocation();

    TSharedPtr<FJsonObject> Summary = GroundTestPayloadFor(DropLabel);
    Summary->SetStringField(TEXT("detail"), TEXT("summary"));
    // A pure tangent rest, so the first call lands the cube at a known Z and the second call's
    // own delta is the only thing that can move it again.
    Summary->SetNumberField(TEXT("embedFraction"), 0.0);

    FTestResponseCapture SummaryCapture;
    TestTrue(TEXT("spatial.ground_actors handler registered"),
        InvokeHandlerWithCapture(TEXT("spatial.ground_actors"), Summary, SummaryCapture));
    TestTrue(TEXT("the batch call succeeds"), SummaryCapture.bSuccess);
    if (!SummaryCapture.bSuccess || !SummaryCapture.Result.IsValid())
    {
        AddError(TEXT("ground_actors did not return a result to read the undo record from"));
        return true;
    }

    double MovedCount = 0.0;
    double PlacedCount = 0.0;
    SummaryCapture.Result->TryGetNumberField(TEXT("moved"), MovedCount);
    SummaryCapture.Result->TryGetNumberField(TEXT("placed"), PlacedCount);
    TestEqual(TEXT("the actor was moved"), MovedCount, 1.0);
    TestEqual(TEXT("the move was a SUCCESS - the case that had no undo before the fix"),
        PlacedCount, 1.0);

    // detail:"summary" still emits no diagnostic rows...
    const TArray<TSharedPtr<FJsonValue>>* DiagnosticRows = nullptr;
    TestFalse(TEXT("detail:summary still emits no per-actor diagnostic rows"),
        SummaryCapture.Result->TryGetArrayField(TEXT("results"), DiagnosticRows));

    // ...but the undo record is a receipt for a mutation, not a diagnostic, so it is there.
    const TArray<TSharedPtr<FJsonValue>>* MovedRows = nullptr;
    if (SummaryCapture.Result->TryGetArrayField(TEXT("movedActors"), MovedRows) && MovedRows
        && MovedRows->Num() == 1)
    {
        const TSharedPtr<FJsonObject> Entry =
            (*MovedRows)[0].IsValid() ? (*MovedRows)[0]->AsObject() : nullptr;
        if (Entry.IsValid())
        {
            FString EntryLabel;
            Entry->TryGetStringField(TEXT("actor"), EntryLabel);
            TestEqual(TEXT("the undo record names the actor it belongs to"),
                EntryLabel, Drop->GetActorLabel());

            const TSharedPtr<FJsonObject>* Previous = nullptr;
            const TSharedPtr<FJsonObject>* PreviousLocation = nullptr;
            if (Entry->TryGetObjectField(TEXT("previousTransform"), Previous) && Previous
                && (*Previous)->TryGetObjectField(TEXT("location"), PreviousLocation)
                && PreviousLocation)
            {
                double PreviousZ = 0.0;
                (*PreviousLocation)->TryGetNumberField(TEXT("z"), PreviousZ);
                TestTrue(*FString::Printf(TEXT("the undo record carries the PRE-move Z "
                    "(got %.3f, expected %.3f)"), PreviousZ, BeforeFirst.Z),
                    FMath::IsNearlyEqual(PreviousZ, BeforeFirst.Z, 0.01));
            }
            else
            {
                AddError(TEXT("movedActors entry carried no previousTransform.location"));
            }
        }
        else
        {
            AddError(TEXT("movedActors entry was not an object"));
        }
    }
    else
    {
        AddError(TEXT("detail:summary carried no movedActors record for the actor it moved"));
    }

    // The pre-move transform must be the transform the actor NO LONGER has - otherwise the
    // "undo" is an echo of where the actor already is, which restores nothing.
    TestFalse(TEXT("the actor really did move away from the transform the record names"),
        Drop->GetActorLocation().Equals(BeforeFirst, 1.0));

    // ---- detail:"all" - a SUCCESS row must carry previousTransform too. Before the fix the
    // field was gated on !IsSeated(), so the rows that carried it were exactly the rows whose
    // actors had not been left somewhere new. ----
    const FVector BeforeSecond = Drop->GetActorLocation();

    TSharedPtr<FJsonObject> All = GroundTestPayloadFor(DropLabel);
    // 5 cm of bedding, so this second call genuinely relocates the already-seated cube and the
    // pre-move transform it echoes is one the actor no longer has.
    All->SetNumberField(TEXT("embedFraction"), 0.05);

    FTestResponseCapture AllCapture;
    InvokeHandlerWithCapture(TEXT("spatial.ground_actors"), All, AllCapture);
    TestTrue(TEXT("the second batch call succeeds"), AllCapture.bSuccess);
    if (!AllCapture.bSuccess)
    {
        return true;
    }

    if (const TSharedPtr<FJsonObject> Row = GroundTestFirstRow(AllCapture.Result))
    {
        bool bRowPlaced = false;
        Row->TryGetBoolField(TEXT("placed"), bRowPlaced);
        TestTrue(TEXT("the row under test is a SUCCESS row"), bRowPlaced);

        const TSharedPtr<FJsonObject>* Previous = nullptr;
        const TSharedPtr<FJsonObject>* PreviousLocation = nullptr;
        if (Row->TryGetObjectField(TEXT("previousTransform"), Previous) && Previous
            && (*Previous)->TryGetObjectField(TEXT("location"), PreviousLocation)
            && PreviousLocation)
        {
            double PreviousZ = 0.0;
            (*PreviousLocation)->TryGetNumberField(TEXT("z"), PreviousZ);
            TestTrue(*FString::Printf(TEXT("a successful row echoes the pre-move Z "
                "(got %.3f, expected %.3f)"), PreviousZ, BeforeSecond.Z),
                FMath::IsNearlyEqual(PreviousZ, BeforeSecond.Z, 0.01));
            // And it is a transform the actor no longer holds: the 5 cm of bedding moved it.
            TestTrue(*FString::Printf(TEXT("the successful move really relocated the actor "
                "(now %.3f, was %.3f)"), Drop->GetActorLocation().Z, BeforeSecond.Z),
                Drop->GetActorLocation().Z < BeforeSecond.Z - 1.0);
        }
        else
        {
            AddError(TEXT("a successful results row carried no previousTransform.location"));
        }
    }
    else
    {
        AddError(TEXT("ground_actors result missing the per-actor results row"));
    }
    return true;
}

// ---- A shaped underside is not a float ----
//
// The verdict arithmetic, driven on constructed columns rather than on a level. That is the
// right instrument here: the shapes that break the old rule are underside PROFILES - a capital
// wider than the base it stands on, a cylinder resting on its flank - and expressing them as
// numbers is both exact and independent of what any engine mesh's collision happens to be.
// Ground is flat at Z = 0 in every fixture, so the ONLY variable between them is the mesh, which
// is the control that isolated this defect in the field.
//
// Before the fix `pass` was gated on the largest RAW column clearance, so the first two fixtures
// failed ACTOR_NOT_GROUNDED reporting a 2352 cm / 61 cm "float" that was the capital and the
// cylinder flank sitting exactly where they belong. The fourth fixture is the other half of the
// counterfactual: a genuinely airborne actor must still fail, through the same gate.

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVerifyGroundingOverhangIsNotFloatingTest,
    "PinWright.spatial.verify_grounding.OverhangIsNotFloating",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FVerifyGroundingOverhangIsNotFloatingTest::RunTest(const FString& Parameters)
{
    using namespace GroundPlacement;

    auto Column = [](double UndersideZ, double GroundZ) -> FGroundColumn
    {
        FGroundColumn Out;
        Out.bHasActorGeometry = true;
        Out.UndersideZ = UndersideZ;
        Out.bHasGround = true;
        Out.GroundZ = GroundZ;
        Out.GroundNormal = FVector::UpVector;
        return Out;
    };

    FContactThresholds Thresholds;
    // The call the field report used: default 2 cm float allowance, penetration raised past the
    // deliberate bedding. Everything else default.
    Thresholds.MaxGapCm = 2.0;
    Thresholds.MaxPenetrationCm = 60.0;

    auto Verdict = [&](const TArray<FGroundColumn>& Columns) -> FGroundContactReport
    {
        FGroundContactReport Report;
        AggregateColumns(Columns, Thresholds, Report);
        EvaluateContact(Report, Thresholds);
        // MinGap <= MaxGap <= MaxColumnClearance holds for every possible sample set, and it is
        // what makes the new rule strictly weaker than the old one: nothing that passed the
        // silhouette gate can start failing.
        TestTrue(*FString::Printf(TEXT("minGap %.2f <= maxGap %.2f <= maxColumnClearance %.2f"),
            Report.MinGapCm, Report.MaxGapCm, Report.MaxColumnClearanceCm),
            Report.MinGapCm <= Report.MaxGapCm + UE_KINDA_SMALL_NUMBER
                && Report.MaxGapCm <= Report.MaxColumnClearanceCm + UE_KINDA_SMALL_NUMBER);
        return Report;
    };

    // ---- (1) A column with a capital wider than its base, bedded 48 cm into flat ground.
    // 3x3 grid: the four corner columns clear the round base and see only the capital's
    // underside 2352 up; the other five see the base, 48.43 below the surface.
    {
        TArray<FGroundColumn> Columns;
        for (int32 Index = 0; Index < 9; ++Index)
        {
            const bool bCorner = (Index == 0 || Index == 2 || Index == 6 || Index == 8);
            Columns.Add(Column(bCorner ? 2352.0 : -48.43, 0.0));
        }

        const FGroundContactReport Report = Verdict(Columns);
        TestTrue(*FString::Printf(TEXT("a bedded column with a capital PASSES (failReason: %s)"),
            *Report.FailReason), Report.bPass);
        TestTrue(*FString::Printf(TEXT("the gated gap is the seating, not the capital "
            "(maxGapCm %.2f)"), Report.MaxGapCm),
            FMath::IsNearlyEqual(Report.MaxGapCm, -48.43, 0.01));
        // The silhouette figure is still reported - it is just no longer the criterion.
        TestTrue(*FString::Printf(TEXT("the capital is still visible as maxColumnClearanceCm "
            "(%.2f)"), Report.MaxColumnClearanceCm),
            FMath::IsNearlyEqual(Report.MaxColumnClearanceCm, 2352.0, 0.01));
        TestTrue(*FString::Printf(TEXT("undersideReliefCm names the shape term (%.2f)"),
            Report.UndersideReliefCm),
            FMath::IsNearlyEqual(Report.UndersideReliefCm, 2400.43, 0.01));
        TestEqual(TEXT("the five base columns are in contact"), Report.ContactPoints, 5);
    }

    // ---- (2) A cylinder lying on its flank, bedded 22 cm in. Underside depends only on the
    // across-axis sample: the middle row is the line of contact, the outer rows are the flank,
    // 82.9 cm up on a radius of 147 sampled at 90% of the half-width.
    {
        TArray<FGroundColumn> Columns;
        for (int32 Across = 0; Across < 3; ++Across)
        {
            const double UndersideZ = (Across == 1) ? -22.0 : (-22.0 + 82.9);
            for (int32 Along = 0; Along < 3; ++Along)
            {
                Columns.Add(Column(UndersideZ, 0.0));
            }
        }

        const FGroundContactReport Report = Verdict(Columns);
        TestTrue(*FString::Printf(TEXT("a cylinder bedded along its length PASSES (failReason: %s)"),
            *Report.FailReason), Report.bPass);
        TestTrue(*FString::Printf(TEXT("curvature does not read as float (maxGapCm %.2f)"),
            Report.MaxGapCm), FMath::IsNearlyEqual(Report.MaxGapCm, -22.0, 0.01));
        TestTrue(*FString::Printf(TEXT("the flank is reported, not gated (maxColumnClearanceCm "
            "%.2f)"), Report.MaxColumnClearanceCm),
            FMath::IsNearlyEqual(Report.MaxColumnClearanceCm, 60.9, 0.01));
        TestEqual(TEXT("the contact line is three columns wide"), Report.ContactPoints, 3);
    }

    // ---- (3) A genuinely floating box: flat underside, 500 cm of clear air, nothing touching.
    {
        TArray<FGroundColumn> Columns;
        for (int32 Index = 0; Index < 9; ++Index)
        {
            Columns.Add(Column(500.0, 0.0));
        }

        const FGroundContactReport Report = Verdict(Columns);
        TestFalse(TEXT("a floating box FAILS"), Report.bPass);
        TestEqual(TEXT("floating -> ACTOR_NOT_GROUNDED"), Report.FailReasonCode,
            FString(TEXT("ACTOR_NOT_GROUNDED")));
        TestEqual(TEXT("a floating box touches nothing"), Report.ContactPoints, 0);
        TestTrue(*FString::Printf(TEXT("the float is the whole 500 cm (maxGapCm %.2f)"),
            Report.MaxGapCm), FMath::IsNearlyEqual(Report.MaxGapCm, 500.0, 0.01));
        TestTrue(TEXT("a flat underside has no relief to blame it on"),
            FMath::IsNearlyZero(Report.UndersideReliefCm));
    }

    // ---- (4) The gate must still fire when the actor really does hang: a flat-bottomed slab
    // resting on one high point with the ground 300 cm below the rest of its footprint. It has
    // contact, so the zero-contact branch cannot catch it - only the gap bound can.
    {
        TArray<FGroundColumn> Columns;
        for (int32 Index = 0; Index < 9; ++Index)
        {
            Columns.Add(Column(0.0, (Index == 4) ? 0.0 : -300.0));
        }

        const FGroundContactReport Report = Verdict(Columns);
        TestFalse(TEXT("a slab perched on one high point FAILS"), Report.bPass);
        TestEqual(TEXT("perched -> ACTOR_NOT_GROUNDED"), Report.FailReasonCode,
            FString(TEXT("ACTOR_NOT_GROUNDED")));
        TestEqual(TEXT("it does touch, so the zero-contact branch did not answer this"),
            Report.ContactPoints, 1);
        TestTrue(*FString::Printf(TEXT("the gap bound is what rejected it (maxGapCm %.2f)"),
            Report.MaxGapCm), FMath::IsNearlyEqual(Report.MaxGapCm, 300.0, 0.01));
        // ...and the reason says what was measured, not "part of this actor floats".
        TestTrue(*FString::Printf(TEXT("failReason names the lowest point (got '%s')"),
            *Report.FailReason), Report.FailReason.Contains(TEXT("lowest point")));
    }

    return true;
}

// ---- ...and the verb wires it through, on a real curved mesh ----

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVerifyGroundingCurvedUndersidePassesTest,
    "PinWright.spatial.verify_grounding.CurvedUndersideStillPasses",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FVerifyGroundingCurvedUndersidePassesTest::RunTest(const FString& Parameters)
{
    UWorld* World = GroundTestEditorWorld();
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no_editor_world"),
            TEXT("GEditor->GetEditorWorldContext().World() returned null; the curved-underside "
                 "assertions were stepped over."));
        return true;
    }

    // The engine sphere is 100 across with a centred pivot, so spawning it at Z = 50 rests it
    // exactly tangent on a floor whose top is Z = 0. Its footprint samples then sit on a curve:
    // touching at the centre column and ~28 cm clear at 90% of the half-width - a gap that is
    // the sphere's own shape, over ground that is right there.
    const FString FloorLabel = GroundTestLabel(TEXT("CurveFloor"));
    const FString SphereLabel = GroundTestLabel(TEXT("CurveSphere"));
    AActor* Floor = GroundTestSpawnCube(*this, World, FloorLabel,
        FVector(GroundTestColX, GroundTestColY, -GroundTestCubeHalf));

    TSharedPtr<FJsonObject> SpherePayload = MakeShared<FJsonObject>();
    SpherePayload->SetStringField(TEXT("meshPath"), TEXT("/Engine/BasicShapes/Sphere.Sphere"));
    SpherePayload->SetStringField(TEXT("actorName"), SphereLabel);
    SpherePayload->SetObjectField(TEXT("location"),
        GroundTestVec(GroundTestColX, GroundTestColY, GroundTestCubeHalf));
    FTestResponseCapture SpawnCapture;
    InvokeHandlerWithCapture(TEXT("actor.spawn"), SpherePayload, SpawnCapture);
    TestTrue(TEXT("the sphere fixture spawned"), SpawnCapture.bSuccess);
    AActor* Sphere = McpActorUtils::FindActorByName(World, SphereLabel);

    ON_SCOPE_EXIT
    {
        if (Floor) { Floor->Destroy(); }
        if (Sphere) { Sphere->Destroy(); }
    };
    if (!Floor || !Sphere)
    {
        AddError(TEXT("fixture actors did not spawn"));
        return true;
    }
    GroundTestFlushPhysics(World);

    FTestResponseCapture Capture;
    TestTrue(TEXT("spatial.verify_grounding handler registered"),
        InvokeHandlerWithCapture(TEXT("spatial.verify_grounding"),
            GroundTestPayloadFor(SphereLabel), Capture));
    TestTrue(TEXT("the verify call succeeds"), Capture.bSuccess);
    if (!Capture.bSuccess || !Capture.Result.IsValid())
    {
        return true;
    }

    bool bPass = false;
    Capture.Result->TryGetBoolField(TEXT("pass"), bPass);

    const TSharedPtr<FJsonObject> Row = GroundTestFirstRow(Capture.Result);
    const TSharedPtr<FJsonObject>* Contact = nullptr;
    if (!Row.IsValid() || !Row->TryGetObjectField(TEXT("contact"), Contact) || !Contact)
    {
        AddError(TEXT("verify_grounding returned no contact block for the sphere"));
        return true;
    }

    FString FailReason;
    (*Contact)->TryGetStringField(TEXT("failReason"), FailReason);
    // Before the fix this failed ACTOR_NOT_GROUNDED on a ~28 cm "float" that was the sphere's
    // own curvature over ground it was resting on.
    TestTrue(*FString::Printf(TEXT("a sphere resting on a floor PASSES (failReason: %s)"),
        *FailReason), bPass);

    double MaxGap = 1000.0;
    double MaxColumnClearance = 0.0;
    double UndersideRelief = 0.0;
    (*Contact)->TryGetNumberField(TEXT("maxGapCm"), MaxGap);
    (*Contact)->TryGetNumberField(TEXT("maxColumnClearanceCm"), MaxColumnClearance);
    (*Contact)->TryGetNumberField(TEXT("undersideReliefCm"), UndersideRelief);

    TestTrue(*FString::Printf(TEXT("the gated gap is ~0 - it is resting (maxGapCm %.3f)"), MaxGap),
        MaxGap < 2.0);
    // The two diagnostic fields must actually carry the curvature, or the row cannot be used to
    // judge a borderline case - which is the other half of what this ticket asked for.
    TestTrue(*FString::Printf(TEXT("the curvature is reported as maxColumnClearanceCm (%.3f)"),
        MaxColumnClearance), MaxColumnClearance > 10.0);
    TestTrue(*FString::Printf(TEXT("the curvature is reported as undersideReliefCm (%.3f)"),
        UndersideRelief), UndersideRelief > 10.0);
    return true;
}

// ---- An ISM/HISM scatter holder must be REFUSED by both verbs, not measured or moved ----
//
// The two tests below are deliberately a pair. A holder's bounds are the union of every instance,
// so the seat verb's single SetActorTransform relocates the whole scatter and the verify verb's
// verdict is a number about nothing. Both fixtures put real ground under the whole scatter, so
// before the refusal existed the verbs COULD answer - verify returned a gap verdict and
// ground_actors moved all three instances at once and reported placed:true. Refusing on only one
// side would leave the other doing exactly that, which is why neither test stands alone.

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVerifyGroundingInstancedHolderRefusedTest,
    "PinWright.spatial.verify_grounding.InstancedHolderRefused",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FVerifyGroundingInstancedHolderRefusedTest::RunTest(const FString& Parameters)
{
    UWorld* World = GroundTestEditorWorld();
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no_editor_world"),
            TEXT("GEditor->GetEditorWorldContext().World() returned null; the verify_grounding "
                 "holder-refusal assertions were stepped over."));
        return true;
    }

    const FString FloorLabel = GroundTestLabel(TEXT("HolderVFloor"));
    const FString HolderLabel = GroundTestLabel(TEXT("HolderVerify"));
    // 2000 x 2000 of floor with its top at Z = 0, so every column of the scatter finds ground and
    // the verb has a full-coverage measurement to be tempted into reporting.
    AActor* Floor = GroundTestSpawnWideFloor(*this, World, FloorLabel,
        FVector(GroundTestColX, GroundTestColY, -GroundTestCubeHalf), 20.0);
    // Three cube instances 600 cm apart, their undersides 250 cm above that floor.
    AActor* Holder = GroundTestSpawnScatterHolder(*this, World, HolderLabel,
        FVector(GroundTestColX, GroundTestColY, 300.0), 3, 600.0);
    ON_SCOPE_EXIT
    {
        if (Floor) { Floor->Destroy(); }
        if (Holder) { Holder->Destroy(); }
    };
    if (!Floor || !Holder)
    {
        AddError(TEXT("holder fixture did not spawn"));
        return true;
    }
    GroundTestFlushPhysics(World);

    FTestResponseCapture Capture;
    TestTrue(TEXT("spatial.verify_grounding handler registered"),
        InvokeHandlerWithCapture(TEXT("spatial.verify_grounding"), GroundTestPayloadFor(HolderLabel),
            Capture));
    TestTrue(TEXT("the verify call itself succeeds"), Capture.bSuccess);
    if (!Capture.bSuccess || !Capture.Result.IsValid())
    {
        return true;
    }

    bool bPass = true;
    Capture.Result->TryGetBoolField(TEXT("pass"), bPass);
    TestFalse(TEXT("a scatter holder does not pass"), bPass);

    const TSharedPtr<FJsonObject> Row = GroundTestFirstRow(Capture.Result);
    if (!Row.IsValid())
    {
        AddError(TEXT("verify_grounding result missing the per-actor results row"));
        return true;
    }

    const TSharedPtr<FJsonObject>* Contact = nullptr;
    if (!Row->TryGetObjectField(TEXT("contact"), Contact) || !Contact)
    {
        AddError(TEXT("verify_grounding row missing the contact block"));
        return true;
    }

    FString FailCode;
    (*Contact)->TryGetStringField(TEXT("failCode"), FailCode);
    // Before the refusal existed this read ACTOR_NOT_GROUNDED - a true statement about the union
    // of three instances and a meaningless one about any of them.
    TestEqual(TEXT("a scatter holder -> HOLDER_NOT_SEATABLE"), FailCode,
        FString(TEXT("HOLDER_NOT_SEATABLE")));

    // The refusal has to name the component, or a caller cannot find what to fix on an actor
    // carrying several.
    FString FailReason;
    (*Contact)->TryGetStringField(TEXT("failReason"), FailReason);
    TestTrue(*FString::Printf(TEXT("the refusal names the offending component (got: %s)"),
        *FailReason), FailReason.Contains(TEXT("HISM_GroundScatter")));
    TestTrue(TEXT("the refusal points at the per-instance path"),
        FailReason.Contains(TEXT("instance")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGroundActorsInstancedHolderRefusedTest,
    "PinWright.spatial.ground_actors.InstancedHolderRefused",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGroundActorsInstancedHolderRefusedTest::RunTest(const FString& Parameters)
{
    UWorld* World = GroundTestEditorWorld();
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no_editor_world"),
            TEXT("GEditor->GetEditorWorldContext().World() returned null; the ground_actors "
                 "holder-refusal assertions were stepped over."));
        return true;
    }

    const FString FloorLabel = GroundTestLabel(TEXT("HolderGFloor"));
    const FString HolderLabel = GroundTestLabel(TEXT("HolderGround"));
    AActor* Floor = GroundTestSpawnWideFloor(*this, World, FloorLabel,
        FVector(GroundTestColX, GroundTestColY, -GroundTestCubeHalf), 20.0);
    AActor* Holder = GroundTestSpawnScatterHolder(*this, World, HolderLabel,
        FVector(GroundTestColX, GroundTestColY, 300.0), 3, 600.0);
    ON_SCOPE_EXIT
    {
        if (Floor) { Floor->Destroy(); }
        if (Holder) { Holder->Destroy(); }
    };
    if (!Floor || !Holder)
    {
        AddError(TEXT("holder fixture did not spawn"));
        return true;
    }
    GroundTestFlushPhysics(World);

    const FVector BeforeLocation = Holder->GetActorLocation();

    FTestResponseCapture Capture;
    TestTrue(TEXT("spatial.ground_actors handler registered"),
        InvokeHandlerWithCapture(TEXT("spatial.ground_actors"), GroundTestPayloadFor(HolderLabel),
            Capture));
    TestTrue(TEXT("the batch call itself succeeds"), Capture.bSuccess);
    if (!Capture.bSuccess || !Capture.Result.IsValid())
    {
        return true;
    }

    // Hostile defaults: a missing field must fail the assertion, not satisfy it.
    double Placed = 1.0;
    double Moved = 1.0;
    Capture.Result->TryGetNumberField(TEXT("placed"), Placed);
    Capture.Result->TryGetNumberField(TEXT("moved"), Moved);
    TestEqual(TEXT("a scatter holder is never reported placed"), Placed, 0.0);
    TestEqual(TEXT("a scatter holder is never moved"), Moved, 0.0);
    // movedActors[] is the undo receipt; a refusal that moved nothing must not write one.
    const TArray<TSharedPtr<FJsonValue>>* MovedRows = nullptr;
    TestFalse(TEXT("no undo record is written for a refused holder"),
        Capture.Result->TryGetArrayField(TEXT("movedActors"), MovedRows));

    const TSharedPtr<FJsonObject> Row = GroundTestFirstRow(Capture.Result);
    if (!Row.IsValid())
    {
        AddError(TEXT("ground_actors result missing the per-actor results row"));
        return true;
    }

    FString ReasonCode;
    Row->TryGetStringField(TEXT("reasonCode"), ReasonCode);
    TestEqual(TEXT("a scatter holder -> HOLDER_NOT_SEATABLE"), ReasonCode,
        FString(TEXT("HOLDER_NOT_SEATABLE")));

    FString Status;
    Row->TryGetStringField(TEXT("status"), Status);
    TestEqual(TEXT("the seat status is its own, not a borrowed no-ground one"), Status,
        FString(TEXT("holder_not_seatable")));

    FString Reason;
    Row->TryGetStringField(TEXT("reason"), Reason);
    TestTrue(*FString::Printf(TEXT("the refusal names the offending component (got: %s)"), *Reason),
        Reason.Contains(TEXT("HISM_GroundScatter")));
    TestTrue(TEXT("the refusal points at the per-instance path"),
        Reason.Contains(TEXT("instance")));

    // Ground truth, re-read from the engine rather than from the response: before the refusal
    // existed this call seated the holder and dropped all three instances by ~250 cm.
    TestTrue(*FString::Printf(TEXT("the holder is left exactly where it was (Z %.3f -> %.3f)"),
        BeforeLocation.Z, Holder->GetActorLocation().Z),
        Holder->GetActorLocation().Equals(BeforeLocation, 0.001));
    return true;
}

// ---- The probe must SAY which of a mesh's two surfaces it measured ----
//
// A ground probe resolves against SIMPLE collision by default, and that default is correct: a
// complex probe is blocked by collisionless foliage, which is a worse failure. But a mesh has TWO
// surfaces, the hull and the render geometry, and they are not the same surface. Where they
// differ the verb measures a floor the mesh does not have and reports it with numbers that are
// internally flawless - every gap term agrees with every other one precisely because they all
// read the same wrong surface, so no amount of cross-checking them can break the tie. The fix is
// therefore not "trace complex"; it is that the response must say WHICH surface answered.
//
// The fixture needs no authored asset and no boolean detail, which is the point: UE scales an
// FKSphereElem radius by the MINIMUM absolute scale component (BodySetup.cpp,
// FKSphereElem::GetFinalScaled), so the engine sphere - whose simple collision is exactly one
// sphere elem - at scale (1,1,2) renders 200 cm tall while its hull stays a 100 cm ball. Render
// apex and hull apex end up ~50 cm apart with nothing done wrongly by anyone, so any project that
// non-uniformly scales an actor with a sphere hull is exposed on any map.
//
// A cube is then parked with its underside exactly on the surface a viewer sees, and the verb is
// asked about it. The assertion is NOT that the answer must be the render surface - it must not
// be, for the foliage reason above - but that the contact block must disclose that a hull
// answered. Before this, the probe computed the face index, the simple-shape count and the
// render-geometry flag on the very hit it read the ground Z off, and discarded all three before
// serializing. spatial.ground_actors shares this contact writer, so it is covered by the same
// assertion.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVerifyGroundingReportsGroundProvenanceTest,
    "PinWright.spatial.verify_grounding.GroundProvenanceNamesTheMeasuredSurface",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FVerifyGroundingReportsGroundProvenanceTest::RunTest(const FString& Parameters)
{
    UWorld* World = GroundTestEditorWorld();
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no_editor_world"),
            TEXT("GEditor->GetEditorWorldContext().World() returned null; the ground-provenance "
                 "assertions were stepped over."));
        return true;
    }

    const FString FloorLabel = GroundTestLabel(TEXT("ProvFloor"));
    const FString PropLabel = GroundTestLabel(TEXT("ProvProp"));

    AActor* Floor = GroundTestSpawnScaledMesh(*this, World, FloorLabel,
        TEXT("/Engine/BasicShapes/Sphere.Sphere"),
        FVector(GroundTestColX, GroundTestColY, 0.0), FVector(1.0, 1.0, 2.0));
    AActor* Prop = nullptr;
    ON_SCOPE_EXIT
    {
        if (Prop) { Prop->Destroy(); }
        if (Floor) { Floor->Destroy(); }
    };
    if (!Floor)
    {
        AddError(TEXT("divergent-hull floor did not spawn"));
        return true;
    }

    // Taken from the engine rather than assumed, so the fixture cannot drift with the asset.
    // GetActorBounds(false) is the RENDER footprint - the same call every spatial verb uses.
    FVector FloorOrigin = FVector::ZeroVector;
    FVector FloorExtent = FVector::ZeroVector;
    Floor->GetActorBounds(false, FloorOrigin, FloorExtent);
    const double RenderApexZ = FloorOrigin.Z + FloorExtent.Z;

    // Underside exactly on the visible surface: a placement a reviewer looking at the level would
    // call correct.
    Prop = GroundTestSpawnCube(*this, World, PropLabel,
        FVector(GroundTestColX, GroundTestColY, RenderApexZ + GroundTestCubeHalf));
    if (!Prop)
    {
        AddError(TEXT("prop did not spawn"));
        return true;
    }
    GroundTestFlushPhysics(World);

    TSharedPtr<FJsonObject> Payload = GroundTestPayloadFor(PropLabel);
    // One column, at the prop's centre - i.e. over the sphere's apex, where the two surfaces are
    // farthest apart and neither reading is ambiguous.
    Payload->SetNumberField(TEXT("samples"), 1);

    FTestResponseCapture Capture;
    TestTrue(TEXT("spatial.verify_grounding handler registered"),
        InvokeHandlerWithCapture(TEXT("spatial.verify_grounding"), Payload, Capture));
    TestTrue(TEXT("the verify call itself succeeds"), Capture.bSuccess);
    if (!Capture.bSuccess || !Capture.Result.IsValid())
    {
        return true;
    }

    const TSharedPtr<FJsonObject> Row = GroundTestFirstRow(Capture.Result);
    if (!Row.IsValid())
    {
        AddError(TEXT("verify_grounding result missing the per-actor results row"));
        return true;
    }
    const TSharedPtr<FJsonObject>* Contact = nullptr;
    if (!Row->TryGetObjectField(TEXT("contact"), Contact) || !Contact)
    {
        AddError(TEXT("verify_grounding row missing the contact block"));
        return true;
    }

    // The fixture, restated from the response itself: the prop's underside is on the render apex,
    // so maxGapCm IS the hull-vs-render divergence at this column. A verb answering about the
    // visible surface would report ~0 here.
    double MaxGap = 0.0;
    (*Contact)->TryGetNumberField(TEXT("maxGapCm"), MaxGap);
    TestTrue(*FString::Printf(
        TEXT("the probe answered about a surface well below the visible one (maxGapCm %.2f, "
             "render apex Z %.2f)"), MaxGap, RenderApexZ), MaxGap > 20.0);

    // THE REGRESSION. Without this block the response above is a clean, on-contract measurement
    // of a surface nobody can see, and nothing in it says so.
    const TSharedPtr<FJsonObject>* Provenance = nullptr;
    if (!(*Contact)->TryGetObjectField(TEXT("groundProvenance"), Provenance) || !Provenance)
    {
        AddError(TEXT("the contact block does not say WHICH collision representation answered "
                      "the ground probe (groundProvenance missing)"));
        return true;
    }

    int32 PrimitiveColumns = 0;
    int32 TriangleColumns = -1;
    (*Provenance)->TryGetNumberField(TEXT("primitiveColumns"), PrimitiveColumns);
    (*Provenance)->TryGetNumberField(TEXT("triangleColumns"), TriangleColumns);
    TestEqual(TEXT("the column is reported as answered by a simple primitive, i.e. by the hull"),
        PrimitiveColumns, 1);
    TestEqual(TEXT("...and not by a triangle mesh or heightfield"), TriangleColumns, 0);

    int32 MaxShapes = 0;
    TestTrue(TEXT("the struck component's simple-primitive count is published"),
        (*Provenance)->TryGetNumberField(TEXT("maxSimpleCollisionShapes"), MaxShapes));
    TestEqual(TEXT("the sphere hull is a single primitive"), MaxShapes, 1);

    // Counted is not the same as told: the count is triage, the warning is what a caller who
    // never read the wiki page actually sees.
    FString Warning;
    (*Provenance)->TryGetStringField(TEXT("warning"), Warning);
    TestTrue(TEXT("a hull measurement is warned about, not merely counted"), !Warning.IsEmpty());
    return true;
}

// ---- Per-instance transforms: read, write, ground -----------------------------------------
//
// These four are the counterpart of the two holder-refusal tests above. Those assert that neither
// ground verb answers FOR a scatter; these assert that the per-instance path the refusal now names
// actually exists and actually moves the world.
//
// Every assertion about a transform is re-read from the component, never from the response. That
// is not belt-and-braces: the route these verbs replace (property.set / container.array.set on
// PerInstanceSMData) stores the matrix, reports success, and READS BACK the value it just stored
// while the render buffers, the instance bodies, navigation and the HISM cluster tree keep the old
// one. A test that trusted the response would have passed against that defect.

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FActorGetInstancesReadsDecomposedTransformsTest,
    "PinWright.actor.get_instances.ReadsDecomposedWorldAndLocalTransforms",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FActorGetInstancesReadsDecomposedTransformsTest::RunTest(const FString& Parameters)
{
    UWorld* World = GroundTestEditorWorld();
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no_editor_world"),
            TEXT("GEditor->GetEditorWorldContext().World() returned null; the actor.get_instances "
                 "assertions were stepped over."));
        return true;
    }

    const FString HolderLabel = GroundTestLabel(TEXT("ReadInstances"));
    const FVector HolderLocation(GroundTestColX, GroundTestColY, 300.0);
    AActor* Holder = GroundTestSpawnScatterHolder(*this, World, HolderLabel, HolderLocation, 3, 600.0);
    ON_SCOPE_EXIT
    {
        if (Holder) { Holder->Destroy(); }
    };
    if (!Holder)
    {
        AddError(TEXT("scatter holder fixture did not spawn"));
        return true;
    }

    UInstancedStaticMeshComponent* Component = GroundTestScatterComponent(Holder);
    if (!Component)
    {
        AddError(TEXT("scatter holder carries no instanced component"));
        return true;
    }

    // Give the middle instance a rotation and a non-unit scale. Scale is the field
    // foliage.get_instances drops (E-foliage-get-instances-drops-scale) and the one a raw
    // PerInstanceSMData read hands back only as part of an undecomposed FMatrix, so a reader that
    // silently normalises it would look correct on an all-identity scatter.
    Component->UpdateInstanceTransform(1,
        FTransform(FRotator(0.0, 45.0, 0.0), FVector::ZeroVector, FVector(2.0, 2.0, 2.0)),
        /*bWorldSpace*/ false, /*bMarkRenderStateDirty*/ true, /*bTeleport*/ true);

    FTestResponseCapture Capture;
    TestTrue(TEXT("actor.get_instances handler registered"),
        InvokeHandlerWithCapture(TEXT("actor.get_instances"), GroundTestHolderPayload(HolderLabel),
            Capture));
    TestTrue(TEXT("the read succeeds"), Capture.bSuccess);
    if (!Capture.bSuccess || !Capture.Result.IsValid())
    {
        return true;
    }

    double InstanceCount = 0.0;
    double Returned = 0.0;
    Capture.Result->TryGetNumberField(TEXT("instanceCount"), InstanceCount);
    Capture.Result->TryGetNumberField(TEXT("returned"), Returned);
    TestEqual(TEXT("the whole scatter is reported"), InstanceCount, 3.0);
    TestEqual(TEXT("every instance comes back"), Returned, 3.0);

    FString ComponentName;
    Capture.Result->TryGetStringField(TEXT("component"), ComponentName);
    TestEqual(TEXT("the component that was read is named"), ComponentName,
        FString(TEXT("HISM_GroundScatter")));

    FString Space;
    Capture.Result->TryGetStringField(TEXT("space"), Space);
    TestEqual(TEXT("world space is the default"), Space, FString(TEXT("world")));

    // Index 1 is the centre instance: its local offset is 0, so its world pose is the holder's.
    const TSharedPtr<FJsonObject> Middle =
        GroundTestRowForIndex(Capture.Result, TEXT("instances"), 1);
    if (!Middle.IsValid())
    {
        AddError(TEXT("actor.get_instances returned no row for instance 1"));
        return true;
    }
    TestEqual(TEXT("world X of the centre instance is the holder's"),
        GroundTestNestedNumber(Middle, TEXT("location"), TEXT("x"), 0.0), HolderLocation.X, 0.01);
    TestEqual(TEXT("world Z of the centre instance is the holder's"),
        GroundTestNestedNumber(Middle, TEXT("location"), TEXT("z"), 0.0), HolderLocation.Z, 0.01);
    TestEqual(TEXT("rotation survives decomposition"),
        GroundTestNestedNumber(Middle, TEXT("rotation"), TEXT("yaw"), 0.0), 45.0, 0.01);
    // The whole point of carrying scale from day one.
    TestEqual(TEXT("scale is carried, not dropped"),
        GroundTestNestedNumber(Middle, TEXT("scale"), TEXT("x"), 0.0), 2.0, 0.001);
    TestEqual(TEXT("scale is carried on Z too"),
        GroundTestNestedNumber(Middle, TEXT("scale"), TEXT("z"), 0.0), 2.0, 0.001);

    // The outer instances are strung out along X, so a reader that returned COMPONENT-LOCAL
    // transforms while claiming world would put them at +-600 from the origin instead of from the
    // holder. That is precisely the mistake a caller reading PerInstanceSMData by hand makes.
    const TSharedPtr<FJsonObject> Last = GroundTestRowForIndex(Capture.Result, TEXT("instances"), 2);
    if (Last.IsValid())
    {
        TestEqual(TEXT("world X of the last instance is holder + 600"),
            GroundTestNestedNumber(Last, TEXT("location"), TEXT("x"), 0.0),
            HolderLocation.X + 600.0, 0.01);
    }
    else
    {
        AddError(TEXT("actor.get_instances returned no row for instance 2"));
    }

    // ...and space:"local" must answer the other question rather than the same one.
    TSharedPtr<FJsonObject> LocalPayload = GroundTestHolderPayload(HolderLabel);
    LocalPayload->SetStringField(TEXT("space"), TEXT("local"));
    FTestResponseCapture LocalCapture;
    InvokeHandlerWithCapture(TEXT("actor.get_instances"), LocalPayload, LocalCapture);
    TestTrue(TEXT("the local-space read succeeds"), LocalCapture.bSuccess);
    const TSharedPtr<FJsonObject> LocalLast =
        GroundTestRowForIndex(LocalCapture.Result, TEXT("instances"), 2);
    if (LocalLast.IsValid())
    {
        TestEqual(TEXT("local X of the last instance is +600 from the component"),
            GroundTestNestedNumber(LocalLast, TEXT("location"), TEXT("x"), 0.0), 600.0, 0.01);
    }
    else
    {
        AddError(TEXT("the local-space read returned no row for instance 2"));
    }

    // A stale index must be refused rather than skipped: instances are positional, so a silently
    // dropped row reads as "that instance is gone" when it may simply have been renumbered.
    TSharedPtr<FJsonObject> BadPayload = GroundTestHolderPayload(HolderLabel);
    TArray<TSharedPtr<FJsonValue>> BadIndices;
    BadIndices.Add(MakeShared<FJsonValueNumber>(99));
    BadPayload->SetArrayField(TEXT("indices"), BadIndices);
    FTestResponseCapture BadCapture;
    InvokeHandlerWithCapture(TEXT("actor.get_instances"), BadPayload, BadCapture);
    TestFalse(TEXT("an out-of-range index is refused"), BadCapture.bSuccess);
    TestEqual(TEXT("out-of-range index -> INSTANCE_INDEX_OUT_OF_RANGE"), BadCapture.ErrorCode,
        FString(TEXT("INSTANCE_INDEX_OUT_OF_RANGE")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FActorSetInstanceTransformsMovesTheInstanceTest,
    "PinWright.actor.set_instance_transforms.WriteReachesTheComponent",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FActorSetInstanceTransformsMovesTheInstanceTest::RunTest(const FString& Parameters)
{
    UWorld* World = GroundTestEditorWorld();
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no_editor_world"),
            TEXT("GEditor->GetEditorWorldContext().World() returned null; the "
                 "actor.set_instance_transforms assertions were stepped over."));
        return true;
    }

    const FString HolderLabel = GroundTestLabel(TEXT("WriteInstances"));
    const FVector HolderLocation(GroundTestColX, GroundTestColY, 300.0);
    AActor* Holder = GroundTestSpawnScatterHolder(*this, World, HolderLabel, HolderLocation, 3, 600.0);
    ON_SCOPE_EXIT
    {
        if (Holder) { Holder->Destroy(); }
    };
    if (!Holder)
    {
        AddError(TEXT("scatter holder fixture did not spawn"));
        return true;
    }

    FTransform BeforeTarget;
    FTransform BeforeNeighbour;
    if (!GroundTestInstanceWorld(Holder, 2, BeforeTarget)
        || !GroundTestInstanceWorld(Holder, 0, BeforeNeighbour))
    {
        AddError(TEXT("could not read the fixture's instance transforms"));
        return true;
    }
    const double TargetZ = BeforeTarget.GetLocation().Z - 175.0;

    TSharedPtr<FJsonObject> Location = MakeShared<FJsonObject>();
    Location->SetNumberField(TEXT("x"), BeforeTarget.GetLocation().X);
    Location->SetNumberField(TEXT("y"), BeforeTarget.GetLocation().Y);
    Location->SetNumberField(TEXT("z"), TargetZ);
    TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
    Entry->SetNumberField(TEXT("index"), 2);
    Entry->SetObjectField(TEXT("location"), Location);
    TArray<TSharedPtr<FJsonValue>> Entries;
    Entries.Add(MakeShared<FJsonValueObject>(Entry));

    TSharedPtr<FJsonObject> Payload = GroundTestHolderPayload(HolderLabel);
    Payload->SetArrayField(TEXT("instances"), Entries);

    FTestResponseCapture Capture;
    TestTrue(TEXT("actor.set_instance_transforms handler registered"),
        InvokeHandlerWithCapture(TEXT("actor.set_instance_transforms"), Payload, Capture));
    TestTrue(TEXT("the write succeeds"), Capture.bSuccess);
    if (!Capture.bSuccess || !Capture.Result.IsValid())
    {
        return true;
    }

    // Hostile default: a missing field must fail the assertion, not satisfy it.
    double Updated = -1.0;
    Capture.Result->TryGetNumberField(TEXT("updated"), Updated);
    TestEqual(TEXT("one instance is reported updated"), Updated, 1.0);

    // GROUND TRUTH. This is the assertion the property-layer route fails: it reports success and
    // reads back its own store while the component keeps the old transform.
    FTransform AfterTarget;
    if (!GroundTestInstanceWorld(Holder, 2, AfterTarget))
    {
        AddError(TEXT("instance 2 could not be re-read after the write"));
        return true;
    }
    TestEqual(*FString::Printf(TEXT("the component holds the new Z (%.3f -> %.3f)"),
        BeforeTarget.GetLocation().Z, AfterTarget.GetLocation().Z),
        AfterTarget.GetLocation().Z, TargetZ, 0.01);
    // An omitted field keeps what the instance had, rather than resetting it to an identity the
    // caller never asked for.
    TestEqual(TEXT("an omitted scale is preserved"), AfterTarget.GetScale3D().X, 1.0, 0.001);

    FTransform AfterNeighbour;
    GroundTestInstanceWorld(Holder, 0, AfterNeighbour);
    TestTrue(TEXT("an unnamed instance is left exactly where it was"),
        AfterNeighbour.GetLocation().Equals(BeforeNeighbour.GetLocation(), 0.001));

    // The undo receipt, and it must carry the pose from BEFORE the write. previousTransform is a
    // whole transform, so the Z lives at previousTransform.location.z - a missing object is an
    // AddError rather than a fallback number, because a fallback of 0.0 is indistinguishable from
    // an instance genuinely read at the origin.
    const TSharedPtr<FJsonObject> Undo =
        GroundTestRowForIndex(Capture.Result, TEXT("movedInstances"), 2);
    const TSharedPtr<FJsonObject>* UndoTransform = nullptr;
    if (!Undo.IsValid())
    {
        AddError(TEXT("no movedInstances row for the instance that was written"));
    }
    else if (!Undo->TryGetObjectField(TEXT("previousTransform"), UndoTransform) || !UndoTransform)
    {
        AddError(TEXT("the movedInstances row carried no previousTransform"));
    }
    else
    {
        TestEqual(TEXT("the undo record carries the pre-write Z"),
            GroundTestNestedNumber(*UndoTransform, TEXT("location"), TEXT("z"), 0.0),
            BeforeTarget.GetLocation().Z, 0.01);
    }

    // And it is an undo IN THE SHAPE THIS VERB TAKES BACK: the rows go into `instances`
    // unchanged and the scatter returns to where it was. A row whose pose the verb cannot read
    // parses as "change nothing", re-applies the instance's current pose, and reports it as
    // updated - a success that restores nothing.
    const TArray<TSharedPtr<FJsonValue>>* UndoRows = nullptr;
    if (Capture.Result->TryGetArrayField(TEXT("movedInstances"), UndoRows) && UndoRows)
    {
        TSharedPtr<FJsonObject> RestorePayload = GroundTestHolderPayload(HolderLabel);
        RestorePayload->SetArrayField(TEXT("instances"), *UndoRows);
        FTestResponseCapture RestoreCapture;
        InvokeHandlerWithCapture(TEXT("actor.set_instance_transforms"), RestorePayload,
            RestoreCapture);
        TestTrue(TEXT("the undo record is accepted back verbatim"), RestoreCapture.bSuccess);
        FTransform Restored;
        if (GroundTestInstanceWorld(Holder, 2, Restored))
        {
            TestEqual(TEXT("replaying movedInstances[] puts the instance back"),
                Restored.GetLocation().Z, BeforeTarget.GetLocation().Z, 0.01);
        }
        else
        {
            AddError(TEXT("instance 2 could not be re-read after the undo replay"));
        }
    }
    else
    {
        AddError(TEXT("the write reported no movedInstances array"));
    }

    // ---- Failure directions: a refused batch must move NOTHING ----

    TSharedPtr<FJsonObject> StaleEntry = MakeShared<FJsonObject>();
    StaleEntry->SetNumberField(TEXT("index"), 99);
    StaleEntry->SetObjectField(TEXT("location"), Location);
    TSharedPtr<FJsonObject> GoodEntry = MakeShared<FJsonObject>();
    GoodEntry->SetNumberField(TEXT("index"), 0);
    GoodEntry->SetObjectField(TEXT("location"), Location);
    TArray<TSharedPtr<FJsonValue>> MixedEntries;
    MixedEntries.Add(MakeShared<FJsonValueObject>(GoodEntry));
    MixedEntries.Add(MakeShared<FJsonValueObject>(StaleEntry));
    TSharedPtr<FJsonObject> StalePayload = GroundTestHolderPayload(HolderLabel);
    StalePayload->SetArrayField(TEXT("instances"), MixedEntries);

    FTestResponseCapture StaleCapture;
    InvokeHandlerWithCapture(TEXT("actor.set_instance_transforms"), StalePayload, StaleCapture);
    TestFalse(TEXT("one stale index refuses the whole batch"), StaleCapture.bSuccess);
    TestEqual(TEXT("stale index -> INSTANCE_INDEX_OUT_OF_RANGE"), StaleCapture.ErrorCode,
        FString(TEXT("INSTANCE_INDEX_OUT_OF_RANGE")));
    // The valid row came FIRST. A verb that applied rows as it validated them would have moved
    // instance 0 before reaching the bad one, leaving a half-applied batch keyed on indices the
    // caller has already been told not to trust.
    FTransform AfterRefusal;
    GroundTestInstanceWorld(Holder, 0, AfterRefusal);
    TestTrue(TEXT("the valid row of a refused batch was not applied"),
        AfterRefusal.GetLocation().Equals(BeforeNeighbour.GetLocation(), 0.001));

    // expectedCount guards the positional addressing itself.
    TSharedPtr<FJsonObject> CountPayload = GroundTestHolderPayload(HolderLabel);
    CountPayload->SetArrayField(TEXT("instances"), Entries);
    CountPayload->SetNumberField(TEXT("expectedCount"), 99);
    FTestResponseCapture CountCapture;
    InvokeHandlerWithCapture(TEXT("actor.set_instance_transforms"), CountPayload, CountCapture);
    TestFalse(TEXT("a disagreeing expectedCount refuses the call"), CountCapture.bSuccess);
    TestEqual(TEXT("expectedCount disagreement -> MATCH_COUNT_MISMATCH"), CountCapture.ErrorCode,
        FString(TEXT("MATCH_COUNT_MISMATCH")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGroundInstancesSeatsEachInstanceTest,
    "PinWright.spatial.ground_instances.SeatsEachInstanceWithoutMovingTheHolder",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGroundInstancesSeatsEachInstanceTest::RunTest(const FString& Parameters)
{
    UWorld* World = GroundTestEditorWorld();
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no_editor_world"),
            TEXT("GEditor->GetEditorWorldContext().World() returned null; the "
                 "spatial.ground_instances assertions were stepped over."));
        return true;
    }

    const FString FloorLabel = GroundTestLabel(TEXT("SeatIFloor"));
    const FString HolderLabel = GroundTestLabel(TEXT("SeatInstances"));
    AActor* Floor = GroundTestSpawnWideFloor(*this, World, FloorLabel,
        FVector(GroundTestColX, GroundTestColY, -GroundTestCubeHalf), 20.0);
    // Three cube instances 600 cm apart, their undersides 250 cm above that floor.
    const FVector HolderLocation(GroundTestColX, GroundTestColY, 300.0);
    AActor* Holder = GroundTestSpawnScatterHolder(*this, World, HolderLabel, HolderLocation, 3, 600.0);
    ON_SCOPE_EXIT
    {
        if (Floor) { Floor->Destroy(); }
        if (Holder) { Holder->Destroy(); }
    };
    if (!Floor || !Holder)
    {
        AddError(TEXT("instance-seating fixture did not spawn"));
        return true;
    }
    GroundTestFlushPhysics(World);

    const double FloorTopZ = GroundTestTopZ(Floor);
    const FVector HolderBefore = Holder->GetActorLocation();

    TSharedPtr<FJsonObject> Payload = GroundTestHolderPayload(HolderLabel);
    Payload->SetObjectField(TEXT("surface"), GroundTestAnySolidSurface());
    Payload->SetStringField(TEXT("detail"), TEXT("all"));

    FTestResponseCapture Capture;
    TestTrue(TEXT("spatial.ground_instances handler registered"),
        InvokeHandlerWithCapture(TEXT("spatial.ground_instances"), Payload, Capture));
    TestTrue(TEXT("the batch call itself succeeds"), Capture.bSuccess);
    if (!Capture.bSuccess || !Capture.Result.IsValid())
    {
        return true;
    }

    bool bApplied = false;
    double Placed = -1.0;
    double Moved = -1.0;
    Capture.Result->TryGetBoolField(TEXT("applied"), bApplied);
    Capture.Result->TryGetNumberField(TEXT("placed"), Placed);
    Capture.Result->TryGetNumberField(TEXT("moved"), Moved);
    TestTrue(TEXT("apply defaults to true"), bApplied);
    TestEqual(TEXT("every instance is placed"), Placed, 3.0);
    TestEqual(TEXT("every instance is moved"), Moved, 3.0);

    // GROUND TRUTH, re-read from the component. The engine cube is 100 cm tall with a centred
    // pivot, so a seated instance's centre sits half a cube above the floor, minus the default
    // 2 cm embed. A solver that set the instance pivot to the ground Z would bury it by 50.
    const double ExpectedCentreZ = FloorTopZ + GroundTestCubeHalf - 2.0;
    for (int32 Index = 0; Index < 3; ++Index)
    {
        FTransform After;
        if (!GroundTestInstanceWorld(Holder, Index, After))
        {
            AddError(*FString::Printf(TEXT("instance %d could not be re-read"), Index));
            continue;
        }
        TestEqual(*FString::Printf(TEXT("instance %d is seated on the floor (Z %.3f)"),
            Index, After.GetLocation().Z), After.GetLocation().Z, ExpectedCentreZ, 0.5);
    }

    // The distinguishing fact of this verb, and the reason spatial.ground_actors refuses the same
    // actor: the HOLDER never moves. One SetActorTransform here would have dropped all three
    // instances by the same amount and reported success.
    TestTrue(*FString::Printf(TEXT("the holder actor is left exactly where it was (Z %.3f -> %.3f)"),
        HolderBefore.Z, Holder->GetActorLocation().Z),
        Holder->GetActorLocation().Equals(HolderBefore, 0.001));

    // The undo receipt: one row per instance moved, carrying the pre-move pose. previousTransform
    // is a whole transform, so the Z lives at previousTransform.location.z - a missing object is
    // an AddError rather than a fallback number, because a fallback of 0.0 is indistinguishable
    // from an instance genuinely read at the origin.
    const TSharedPtr<FJsonObject> Undo =
        GroundTestRowForIndex(Capture.Result, TEXT("movedInstances"), 1);
    const TSharedPtr<FJsonObject>* UndoTransform = nullptr;
    if (!Undo.IsValid())
    {
        AddError(TEXT("no movedInstances row for a moved instance"));
    }
    else if (!Undo->TryGetObjectField(TEXT("previousTransform"), UndoTransform) || !UndoTransform)
    {
        AddError(TEXT("the movedInstances row carried no previousTransform"));
    }
    else
    {
        TestEqual(TEXT("the undo record carries the pre-move Z"),
            GroundTestNestedNumber(*UndoTransform, TEXT("location"), TEXT("z"), 0.0),
            HolderLocation.Z, 0.01);
    }

    // This verb writes no transaction, so those rows ARE the undo - and they are only an undo if
    // the verb documented as writing them back takes them unchanged. Replay them into
    // actor.set_instance_transforms and every seated instance must return to the pre-seat Z.
    const TArray<TSharedPtr<FJsonValue>>* UndoRows = nullptr;
    if (Capture.Result->TryGetArrayField(TEXT("movedInstances"), UndoRows) && UndoRows)
    {
        TSharedPtr<FJsonObject> RestorePayload = GroundTestHolderPayload(HolderLabel);
        RestorePayload->SetArrayField(TEXT("instances"), *UndoRows);
        FTestResponseCapture RestoreCapture;
        InvokeHandlerWithCapture(TEXT("actor.set_instance_transforms"), RestorePayload,
            RestoreCapture);
        TestTrue(TEXT("ground_instances' undo record is accepted back verbatim"),
            RestoreCapture.bSuccess);
        FTransform Restored;
        if (GroundTestInstanceWorld(Holder, 1, Restored))
        {
            TestEqual(TEXT("replaying movedInstances[] un-seats the instance"),
                Restored.GetLocation().Z, HolderLocation.Z, 0.01);
        }
        else
        {
            AddError(TEXT("instance 1 could not be re-read after the undo replay"));
        }
    }
    else
    {
        AddError(TEXT("the seat reported no movedInstances array"));
    }

    const TSharedPtr<FJsonObject> Row = GroundTestRowForIndex(Capture.Result, TEXT("results"), 1);
    if (!Row.IsValid())
    {
        AddError(TEXT("ground_instances returned no results row for instance 1"));
        return true;
    }
    FString Status;
    Row->TryGetStringField(TEXT("status"), Status);
    TestEqual(TEXT("a seated instance reports the seated status"), Status, FString(TEXT("seated")));
    bool bRowPlaced = false;
    Row->TryGetBoolField(TEXT("placed"), bRowPlaced);
    TestTrue(TEXT("the row's placed is derived from the post-move measurement"), bRowPlaced);
    return true;
}

// ---- Scope: an omitted `component` must not be resolved by a popularity contest ----
//
// The failure direction is the only one that matters here, and it is not "the verb errored". It is
// that instances belonging to a component the caller never named DID NOT MOVE. All three
// per-instance verbs used to resolve an omitted `component` to the one carrying the MOST
// instances; on the level's shared AInstancedFoliageActor - one component per foliage type, for
// every caller in the level - that is a property of the level rather than of the call, and it
// relocated 2,048 instances of other callers' scatters across three incidents in one session, two
// of them unrecoverable. The verb named the component it had chosen only after the move.
//
// So the fixture makes the FOREIGN component the bigger one, puts it over the same floor the
// caller's scatter sits on (a scatter the verb could genuinely have seated is the only way to
// prove it did not), and re-reads every foreign instance from the engine after each call.

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGroundInstancesOmittedComponentRefusedTest,
    "PinWright.spatial.ground_instances.OmittedComponentRefusedOnAMultiComponentActor",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGroundInstancesOmittedComponentRefusedTest::RunTest(const FString& Parameters)
{
    UWorld* World = GroundTestEditorWorld();
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no_editor_world"),
            TEXT("GEditor->GetEditorWorldContext().World() returned null; the per-instance "
                 "component-scope assertions were stepped over."));
        return true;
    }

    const FString FloorLabel = GroundTestLabel(TEXT("ScopeFloor"));
    const FString HolderLabel = GroundTestLabel(TEXT("ScopeHolder"));
    AActor* Floor = GroundTestSpawnWideFloor(*this, World, FloorLabel,
        FVector(GroundTestColX, GroundTestColY, -GroundTestCubeHalf), 20.0);
    const FVector HolderLocation(GroundTestColX, GroundTestColY, 300.0);
    // The caller's own scatter - and deliberately the SMALLER of the two components.
    AActor* Holder = GroundTestSpawnScatterHolder(*this, World, HolderLabel, HolderLocation, 3, 600.0);
    ON_SCOPE_EXIT
    {
        if (Floor) { Floor->Destroy(); }
        if (Holder) { Holder->Destroy(); }
    };
    if (!Floor || !Holder)
    {
        AddError(TEXT("the multi-component scope fixture did not spawn"));
        return true;
    }

    UInstancedStaticMeshComponent* Own =
        Cast<UInstancedStaticMeshComponent>(Holder->GetRootComponent());
    // Somebody else's scatter on the same actor, five instances to the caller's three, so
    // "the component carrying the most instances" names theirs. Resolved by pointer rather than by
    // FindComponentByClass, which would answer with whichever component iterated first.
    UInstancedStaticMeshComponent* Foreign = GroundTestAddScatterComponent(*this, Holder,
        TEXT("HISM_ForeignScatter"), 5, 300.0, 400.0);
    if (!Own || !Foreign)
    {
        AddError(TEXT("the scope fixture is missing one of its two instanced components"));
        return true;
    }
    GroundTestFlushPhysics(World);

    // Ground truth for "none of theirs moved", read from the engine before the first call.
    TArray<FTransform> ForeignBefore;
    for (int32 Index = 0; Index < Foreign->GetInstanceCount(); ++Index)
    {
        FTransform Before;
        if (Foreign->GetInstanceTransform(Index, Before, /*bWorldSpace*/ true))
        {
            ForeignBefore.Add(Before);
        }
    }
    if (ForeignBefore.Num() != 5)
    {
        AddError(TEXT("the foreign scatter did not report its five instances"));
        return true;
    }

    const auto ForeignIsUntouched = [this, Foreign, &ForeignBefore](const TCHAR* What)
    {
        for (int32 Index = 0; Index < ForeignBefore.Num(); ++Index)
        {
            FTransform Now;
            if (!Foreign->GetInstanceTransform(Index, Now, /*bWorldSpace*/ true))
            {
                AddError(*FString::Printf(
                    TEXT("foreign instance %d could not be re-read"), Index));
                continue;
            }
            TestTrue(*FString::Printf(
                TEXT("%s: foreign instance %d is where it was (Z %.3f -> %.3f)"),
                What, Index, ForeignBefore[Index].GetLocation().Z, Now.GetLocation().Z),
                Now.GetLocation().Equals(ForeignBefore[Index].GetLocation(), 0.001));
        }
    };

    // ---- The defect: `component` omitted on an actor carrying two ----
    TSharedPtr<FJsonObject> Payload = GroundTestHolderPayload(HolderLabel);
    Payload->SetObjectField(TEXT("surface"), GroundTestAnySolidSurface());
    Payload->SetStringField(TEXT("detail"), TEXT("all"));

    FTestResponseCapture Capture;
    TestTrue(TEXT("spatial.ground_instances handler registered"),
        InvokeHandlerWithCapture(TEXT("spatial.ground_instances"), Payload, Capture));
    TestFalse(TEXT("an omitted component on a two-component actor is refused"), Capture.bSuccess);
    TestEqual(TEXT("omitted component -> AMBIGUOUS_INSTANCED_COMPONENT"), Capture.ErrorCode,
        FString(TEXT("AMBIGUOUS_INSTANCED_COMPONENT")));
    // The refusal has to be usable, not merely correct: it is the only surface that publishes what
    // the actor carries, so a caller with no legal value for `component` gets one from it.
    TestTrue(TEXT("the refusal names the caller's component"),
        Capture.Message.Contains(TEXT("HISM_GroundScatter")));
    TestTrue(TEXT("the refusal names the foreign component"),
        Capture.Message.Contains(TEXT("HISM_ForeignScatter")));

    // THE ASSERTION THIS TEST EXISTS FOR. Under largest-wins all five foreign instances were
    // seated onto the floor - roughly 250 cm down, in a component the caller never named.
    ForeignIsUntouched(TEXT("after a refused omitted-component call"));
    for (int32 Index = 0; Index < 3; ++Index)
    {
        FTransform Untouched;
        if (Own->GetInstanceTransform(Index, Untouched, /*bWorldSpace*/ true))
        {
            TestEqual(*FString::Printf(
                TEXT("a refused call moves nothing at all (own instance %d)"), Index),
                Untouched.GetLocation().Z, HolderLocation.Z, 0.001);
        }
        else
        {
            AddError(*FString::Printf(TEXT("own instance %d could not be re-read"), Index));
        }
    }

    // ---- Naming the component is the way through, and the seat stays inside what was named ----
    TSharedPtr<FJsonObject> NamedPayload = GroundTestHolderPayload(HolderLabel);
    NamedPayload->SetObjectField(TEXT("surface"), GroundTestAnySolidSurface());
    NamedPayload->SetStringField(TEXT("component"), TEXT("HISM_GroundScatter"));
    FTestResponseCapture NamedCapture;
    InvokeHandlerWithCapture(TEXT("spatial.ground_instances"), NamedPayload, NamedCapture);
    TestTrue(TEXT("naming the component is accepted"), NamedCapture.bSuccess);
    if (NamedCapture.bSuccess && NamedCapture.Result.IsValid())
    {
        FString Seated;
        NamedCapture.Result->TryGetStringField(TEXT("component"), Seated);
        TestEqual(TEXT("the response names the component that was seated"), Seated,
            FString(TEXT("HISM_GroundScatter")));
        double MovedCount = -1.0;
        NamedCapture.Result->TryGetNumberField(TEXT("moved"), MovedCount);
        TestEqual(TEXT("only the named component's three instances move"), MovedCount, 3.0);
    }
    ForeignIsUntouched(TEXT("after seating the named component"));

    // ---- expectedCount covers what the component rule cannot see ----
    // A component NAME is not a stable handle: it can be reassigned to a different scatter between
    // the call that read the indices and this one, and it still resolves cleanly.
    TSharedPtr<FJsonObject> CountPayload = GroundTestHolderPayload(HolderLabel);
    CountPayload->SetObjectField(TEXT("surface"), GroundTestAnySolidSurface());
    CountPayload->SetStringField(TEXT("component"), TEXT("HISM_ForeignScatter"));
    CountPayload->SetNumberField(TEXT("expectedCount"), 3);
    FTestResponseCapture CountCapture;
    InvokeHandlerWithCapture(TEXT("spatial.ground_instances"), CountPayload, CountCapture);
    TestFalse(TEXT("an expectedCount that disagrees refuses the seat"), CountCapture.bSuccess);
    TestEqual(TEXT("expectedCount disagreement -> MATCH_COUNT_MISMATCH"), CountCapture.ErrorCode,
        FString(TEXT("MATCH_COUNT_MISMATCH")));
    ForeignIsUntouched(TEXT("after a refused expectedCount"));

    // ---- One picker, three verbs: the other two must refuse the same omission ----
    TSharedPtr<FJsonObject> WriteRow = MakeShared<FJsonObject>();
    WriteRow->SetNumberField(TEXT("index"), 0);
    WriteRow->SetObjectField(TEXT("location"),
        GroundTestVec(GroundTestColX, GroundTestColY, 1000.0));
    TArray<TSharedPtr<FJsonValue>> WriteRows;
    WriteRows.Add(MakeShared<FJsonValueObject>(WriteRow));
    TSharedPtr<FJsonObject> WritePayload = GroundTestHolderPayload(HolderLabel);
    WritePayload->SetArrayField(TEXT("instances"), WriteRows);
    FTestResponseCapture WriteCapture;
    InvokeHandlerWithCapture(TEXT("actor.set_instance_transforms"), WritePayload, WriteCapture);
    TestFalse(TEXT("actor.set_instance_transforms refuses the same omission"),
        WriteCapture.bSuccess);
    TestEqual(TEXT("the write verb refuses with the same code"), WriteCapture.ErrorCode,
        FString(TEXT("AMBIGUOUS_INSTANCED_COMPONENT")));
    ForeignIsUntouched(TEXT("after a refused write"));

    // The read verb refuses too. Its cost is a wrong ANSWER rather than damage - but the indices
    // it hands back are exactly what the two write verbs address, so reading the wrong component
    // is how a caller comes to write to it.
    FTestResponseCapture ReadCapture;
    InvokeHandlerWithCapture(TEXT("actor.get_instances"), GroundTestHolderPayload(HolderLabel),
        ReadCapture);
    TestFalse(TEXT("actor.get_instances refuses the same omission"), ReadCapture.bSuccess);
    TestEqual(TEXT("the read verb refuses with the same code"), ReadCapture.ErrorCode,
        FString(TEXT("AMBIGUOUS_INSTANCED_COMPONENT")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGroundInstancesDryRunWritesNothingTest,
    "PinWright.spatial.ground_instances.DryRunSolvesAndWritesNothing",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGroundInstancesDryRunWritesNothingTest::RunTest(const FString& Parameters)
{
    UWorld* World = GroundTestEditorWorld();
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no_editor_world"),
            TEXT("GEditor->GetEditorWorldContext().World() returned null; the "
                 "spatial.ground_instances dry-run assertions were stepped over."));
        return true;
    }

    const FString FloorLabel = GroundTestLabel(TEXT("DryIFloor"));
    const FString HolderLabel = GroundTestLabel(TEXT("DryInstances"));
    AActor* Floor = GroundTestSpawnWideFloor(*this, World, FloorLabel,
        FVector(GroundTestColX, GroundTestColY, -GroundTestCubeHalf), 20.0);
    const FVector HolderLocation(GroundTestColX, GroundTestColY, 300.0);
    AActor* Holder = GroundTestSpawnScatterHolder(*this, World, HolderLabel, HolderLocation, 3, 600.0);
    ON_SCOPE_EXIT
    {
        if (Floor) { Floor->Destroy(); }
        if (Holder) { Holder->Destroy(); }
    };
    if (!Floor || !Holder)
    {
        AddError(TEXT("instance dry-run fixture did not spawn"));
        return true;
    }
    GroundTestFlushPhysics(World);

    const double FloorTopZ = GroundTestTopZ(Floor);
    FTransform Before;
    if (!GroundTestInstanceWorld(Holder, 1, Before))
    {
        AddError(TEXT("could not read the fixture's instance transform"));
        return true;
    }

    TSharedPtr<FJsonObject> Payload = GroundTestHolderPayload(HolderLabel);
    Payload->SetObjectField(TEXT("surface"), GroundTestAnySolidSurface());
    Payload->SetStringField(TEXT("detail"), TEXT("all"));
    Payload->SetBoolField(TEXT("apply"), false);

    FTestResponseCapture Capture;
    TestTrue(TEXT("spatial.ground_instances handler registered"),
        InvokeHandlerWithCapture(TEXT("spatial.ground_instances"), Payload, Capture));
    TestTrue(TEXT("the dry run itself succeeds"), Capture.bSuccess);
    if (!Capture.bSuccess || !Capture.Result.IsValid())
    {
        return true;
    }

    bool bApplied = true;
    double Moved = -1.0;
    double Solved = -1.0;
    double Failed = -1.0;
    Capture.Result->TryGetBoolField(TEXT("applied"), bApplied);
    Capture.Result->TryGetNumberField(TEXT("moved"), Moved);
    Capture.Result->TryGetNumberField(TEXT("solved"), Solved);
    Capture.Result->TryGetNumberField(TEXT("failed"), Failed);
    TestFalse(TEXT("the response says it did not apply"), bApplied);
    TestEqual(TEXT("a dry run moves nothing"), Moved, 0.0);
    TestEqual(TEXT("a dry run solves every instance"), Solved, 3.0);
    // A dry run is not a batch of failures. Counting it as one is how "nothing was attempted"
    // reads as "nothing worked".
    TestEqual(TEXT("a dry run reports no failures"), Failed, 0.0);

    // No mutation receipt, because there was no mutation.
    const TArray<TSharedPtr<FJsonValue>>* MovedRows = nullptr;
    TestFalse(TEXT("no undo record is written for a dry run"),
        Capture.Result->TryGetArrayField(TEXT("movedInstances"), MovedRows));

    const TSharedPtr<FJsonObject> Row = GroundTestRowForIndex(Capture.Result, TEXT("results"), 1);
    if (!Row.IsValid())
    {
        AddError(TEXT("the dry run returned no results row for instance 1"));
        return true;
    }
    FString Status;
    Row->TryGetStringField(TEXT("status"), Status);
    TestEqual(TEXT("a dry-run row says so"), Status, FString(TEXT("dry_run")));

    // The proposed move must be the one apply would make: drop the instance's underside onto the
    // floor, then embed 2 cm (0.02 of a 100 cm cube).
    double ProposedDeltaZ = 0.0;
    Row->TryGetNumberField(TEXT("proposedDeltaZCm"), ProposedDeltaZ);
    const double ExpectedDeltaZ =
        -((Before.GetLocation().Z - GroundTestCubeHalf) - FloorTopZ + 2.0);
    TestEqual(TEXT("proposedDeltaZCm is the move apply would have made"), ProposedDeltaZ,
        ExpectedDeltaZ, 0.5);

    // GROUND TRUTH: the component still holds the original transform.
    FTransform After;
    GroundTestInstanceWorld(Holder, 1, After);
    TestTrue(*FString::Printf(TEXT("the instance did not move (Z %.3f -> %.3f)"),
        Before.GetLocation().Z, After.GetLocation().Z),
        After.GetLocation().Equals(Before.GetLocation(), 0.001));
    return true;
}

// ---- The report must NAME the primitive that answered, not only its representation ----
//
// groundProvenance already said WHICH collision representation replied - a hull or a triangle
// mesh - and that is the answer to a different question from the one that costs a level builder
// a pass. A vegetation scatter carried as HISM components on an ordinary actor answers a probe
// with a perfectly ordinary hull, so every count in the block reads exactly as it does for
// stone. Nothing else in the report separates them either: the ground ACTOR is never published,
// and a scatter holder is an AActor whose class names nothing. That is the blind spot the
// component axis closed on the FILTER (FSpatialHitFilter::ExcludeComponentClasses) and this
// closes on the REPORT - without it the next blind spot in that filter is as invisible as the
// last one was.
//
// The fixture is two candidate surfaces that differ ONLY in the component carrying them: a wide
// static-mesh floor low down, and a HISM scatter 200 cm above it. Both opaque, both blocking,
// both accepted by any_solid, so nothing but the component distinguishes them.
//
// The assertion runs in BOTH directions, because a report that named some component regardless
// of which one replied would pass a one-directional test:
//   - the prop rests on the scatter, so the block must name the HISM component and its class,
//     and must NOT name the floor's; and
//   - with excludeComponentClasses peeling the scatter off, the SAME prop in the SAME position
//     reports the floor's StaticMeshComponent instead, and the gap number moves by the 200 cm
//     between the two surfaces - so the name tracks what actually answered rather than being
//     read off the actor or off whatever happens to be in the level.
//
// Red before the fix in the plainest possible way: surfaceComponents did not exist.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVerifyGroundingNamesTheAnsweringComponentTest,
    "PinWright.spatial.verify_grounding.GroundProvenanceNamesTheAnsweringComponent",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FVerifyGroundingNamesTheAnsweringComponentTest::RunTest(const FString& Parameters)
{
    UWorld* World = GroundTestEditorWorld();
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no_editor_world"),
            TEXT("GEditor->GetEditorWorldContext().World() returned null; the answering-component "
                 "assertions were stepped over."));
        return true;
    }

    // The two surfaces. The floor is wide enough that it answers every column once the scatter
    // above it is excluded, so the second half of the test measures a real second surface rather
    // than a partial-coverage failure.
    constexpr double FloorPivotZ = 0.0;
    const double FloorTopZ = FloorPivotZ + GroundTestCubeHalf;
    constexpr double ScatterPivotZ = 200.0;
    const double ScatterTopZ = ScatterPivotZ + GroundTestCubeHalf;

    const FString FloorLabel = GroundTestLabel(TEXT("NameFloor"));
    const FString ScatterLabel = GroundTestLabel(TEXT("NameScatter"));
    const FString PropLabel = GroundTestLabel(TEXT("NameProp"));

    AActor* Floor = GroundTestSpawnWideFloor(*this, World, FloorLabel,
        FVector(GroundTestColX, GroundTestColY, FloorPivotZ), 6.0);
    AActor* Scatter = nullptr;
    AActor* Prop = nullptr;
    ON_SCOPE_EXIT
    {
        if (Prop) { Prop->Destroy(); }
        if (Scatter) { Scatter->Destroy(); }
        if (Floor) { Floor->Destroy(); }
    };
    if (!Floor)
    {
        AddError(TEXT("the floor fixture did not spawn"));
        return true;
    }

    // One instance, so the holder is NOT a scatter holder in the seat-refusal sense (that needs
    // two) and the only thing under test is which component the report names.
    Scatter = GroundTestSpawnScatterHolder(*this, World, ScatterLabel,
        FVector(GroundTestColX, GroundTestColY, ScatterPivotZ), 1, 0.0);
    if (!Scatter)
    {
        AddError(TEXT("the scatter fixture did not spawn"));
        return true;
    }

    // Stated rather than inherited: the fixture is only a fixture if the probe can actually be
    // answered by it, and a component that blocks nothing would silently make this a test of the
    // floor twice.
    UInstancedStaticMeshComponent* ScatterComponent = GroundTestScatterComponent(Scatter);
    if (!ScatterComponent)
    {
        AddError(TEXT("the scatter holder carries no instanced component"));
        return true;
    }
    ScatterComponent->SetCollisionProfileName(TEXT("BlockAll"));

    // Resting exactly on the scatter, so the scatter is what a probe from this prop meets first.
    Prop = GroundTestSpawnCube(*this, World, PropLabel,
        FVector(GroundTestColX, GroundTestColY, ScatterTopZ + GroundTestCubeHalf));
    if (!Prop)
    {
        AddError(TEXT("the prop fixture did not spawn"));
        return true;
    }
    GroundTestFlushPhysics(World);

    // contact block of the single results[] row.
    auto ContactOf = [](const TSharedPtr<FJsonObject>& Result) -> TSharedPtr<FJsonObject>
    {
        const TSharedPtr<FJsonObject> Row = GroundTestFirstRow(Result);
        const TSharedPtr<FJsonObject>* Contact = nullptr;
        if (!Row.IsValid() || !Row->TryGetObjectField(TEXT("contact"), Contact) || !Contact)
        {
            return nullptr;
        }
        return *Contact;
    };

    // The highest-tally surfaceComponents row, i.e. the primitive that answered most of the
    // footprint. Returned by value so an absent block is distinguishable from an empty one.
    auto TopSurfaceComponent = [](const TSharedPtr<FJsonObject>& Contact) -> TSharedPtr<FJsonObject>
    {
        const TSharedPtr<FJsonObject>* Provenance = nullptr;
        if (!Contact.IsValid() || !Contact->TryGetObjectField(TEXT("groundProvenance"), Provenance)
            || !Provenance)
        {
            return nullptr;
        }
        const TArray<TSharedPtr<FJsonValue>>* Rows = nullptr;
        if (!(*Provenance)->TryGetArrayField(TEXT("surfaceComponents"), Rows) || !Rows
            || Rows->Num() == 0)
        {
            return nullptr;
        }
        return (*Rows)[0].IsValid() ? (*Rows)[0]->AsObject() : nullptr;
    };

    // ---- Direction 1: the scatter answers, and the report says so ----

    FTestResponseCapture ScatterCapture;
    TestTrue(TEXT("spatial.verify_grounding handler registered"),
        InvokeHandlerWithCapture(TEXT("spatial.verify_grounding"),
            GroundTestPayloadFor(PropLabel), ScatterCapture));
    TestTrue(TEXT("the verify call over the scatter succeeds"), ScatterCapture.bSuccess);
    if (!ScatterCapture.bSuccess || !ScatterCapture.Result.IsValid())
    {
        return true;
    }

    const TSharedPtr<FJsonObject> ScatterContact = ContactOf(ScatterCapture.Result);
    if (!ScatterContact.IsValid())
    {
        AddError(TEXT("verify_grounding returned no contact block for the prop on the scatter"));
        return true;
    }

    double SupportedColumns = 0.0;
    ScatterContact->TryGetNumberField(TEXT("supportedColumns"), SupportedColumns);
    TestTrue(TEXT("the probe found ground under the prop's footprint"), SupportedColumns > 0.0);

    // THE REGRESSION. Before this the response described the surface's collision representation
    // and never said the surface was a scatter.
    const TSharedPtr<FJsonObject> ScatterRow = TopSurfaceComponent(ScatterContact);
    if (!ScatterRow.IsValid())
    {
        AddError(TEXT("groundProvenance does not name the component that answered the ground "
                      "probe (surfaceComponents missing or empty)"));
        return true;
    }

    FString ScatterClass;
    FString ScatterComponentName;
    FString ScatterActorLabel;
    double ScatterColumns = 0.0;
    ScatterRow->TryGetStringField(TEXT("componentClass"), ScatterClass);
    ScatterRow->TryGetStringField(TEXT("component"), ScatterComponentName);
    ScatterRow->TryGetStringField(TEXT("actor"), ScatterActorLabel);
    ScatterRow->TryGetNumberField(TEXT("columns"), ScatterColumns);

    TestEqual(TEXT("the report names the class of the component that answered"),
        ScatterClass, ScatterComponent->GetClass()->GetName());
    TestEqual(TEXT("...and that component by name"),
        ScatterComponentName, ScatterComponent->GetName());
    TestEqual(TEXT("...and the actor carrying it"), ScatterActorLabel, ScatterLabel);
    // The tally is over the same columns every other provenance number is folded over. One
    // surface under the whole footprint means it answered all of them.
    TestEqual(TEXT("the tally covers every supported column"), ScatterColumns, SupportedColumns);

    // The negative half: the floor is in the level, under the prop, and accepted by the same
    // preset - it is simply not what answered. A report that named it here would be naming the
    // level rather than the hit.
    TestNotEqual(TEXT("the floor's component is NOT reported as the surface"),
        ScatterComponentName, FString(TEXT("StaticMeshComponent")));

    // ---- Direction 2: peel the scatter off, and the SAME prop reports the floor ----

    TSharedPtr<FJsonObject> FloorPayload = GroundTestPayloadFor(PropLabel);
    TSharedPtr<FJsonObject> Surface = GroundTestAnySolidSurface();
    TArray<TSharedPtr<FJsonValue>> ExcludedComponents;
    ExcludedComponents.Add(MakeShared<FJsonValueString>(ScatterComponent->GetClass()->GetName()));
    Surface->SetArrayField(TEXT("excludeComponentClasses"), ExcludedComponents);
    FloorPayload->SetObjectField(TEXT("surface"), Surface);

    FTestResponseCapture FloorCapture;
    TestTrue(TEXT("spatial.verify_grounding handler registered"),
        InvokeHandlerWithCapture(TEXT("spatial.verify_grounding"), FloorPayload, FloorCapture));
    TestTrue(TEXT("the verify call over the floor succeeds"), FloorCapture.bSuccess);
    if (!FloorCapture.bSuccess || !FloorCapture.Result.IsValid())
    {
        return true;
    }

    const TSharedPtr<FJsonObject> FloorContact = ContactOf(FloorCapture.Result);
    if (!FloorContact.IsValid())
    {
        AddError(TEXT("verify_grounding returned no contact block for the prop over the floor"));
        return true;
    }

    // The world moved under the prop by exactly the distance between the two surfaces, which is
    // what makes this a different HIT rather than a different label on the same one.
    double FloorMaxGap = 0.0;
    FloorContact->TryGetNumberField(TEXT("maxGapCm"), FloorMaxGap);
    TestEqual(TEXT("the probe now answers from the floor, 200 cm below the scatter"),
        FloorMaxGap, ScatterTopZ - FloorTopZ, 1.0);

    const TSharedPtr<FJsonObject> FloorRow = TopSurfaceComponent(FloorContact);
    if (!FloorRow.IsValid())
    {
        AddError(TEXT("groundProvenance does not name the component that answered once the "
                      "scatter was excluded"));
        return true;
    }

    FString FloorClass;
    FString FloorActorLabel;
    FloorRow->TryGetStringField(TEXT("componentClass"), FloorClass);
    FloorRow->TryGetStringField(TEXT("actor"), FloorActorLabel);
    TestEqual(TEXT("the report now names the floor's own component class"),
        FloorClass, FString(TEXT("StaticMeshComponent")));
    TestEqual(TEXT("...and the floor actor"), FloorActorLabel, FloorLabel);
    TestNotEqual(TEXT("the scatter's class is no longer reported as the surface"),
        FloorClass, ScatterComponent->GetClass()->GetName());

    return true;
}

// ---- The trust warning must be able to fire on a surface that answered WITH TRIANGLES ----
//
// groundProvenance's `warning` is the one field in the block that makes a CLAIM rather than
// reporting a count, and it was gated on Provenance.PrimitiveColumns > 0 - i.e. on the ABSENCE
// of a face index. Every surface that answers WITH one therefore sat outside it by construction,
// however wrong a ground it was: a complex-traced HISM scatter, a collisionless foliage mesh
// whose render triangles blocked the probe, a UseComplexAsSimple body. Those are the cases the
// alarm is most needed for, and the response already carried the answer - surfaceComponents[]
// names the HISM right beside the silent warning, so the alarm was derived from a proxy that
// could not see what the roster next to it already knew.
//
// Two halves, because the live-verb half cannot pin the STRUCTURAL claim on its own: whether a
// complex trace against an instanced body comes back with a face index is the physics backend's
// business, and a fixture that happened to land in primitiveColumns would raise the OLD warning
// and hide the defect behind a green.
//
//   (1) Constructed columns, no physics at all. Every column carries GroundFaceIndex >= 0, so
//       PrimitiveColumns is 0 and the old predicate provably cannot fire on it under ANY
//       argument shape - then the column is pointed at a real HISM component, and at a real
//       StaticMeshComponent flagged as a render-geometry hit. Both must raise the warning and
//       NAME the component. The two silences are pinned here too: a clean surface reads
//       "trusted", a column whose primitive never resolved reads "undetermined" and withholds
//       the all-clear, and before this both were the same absent string.
//   (2) The live verb over a real HISM scatter at traceComplex:true - the workaround
//       B-ground-probe-hits-hull-not-render publishes, i.e. taking the published remedy is what
//       used to guarantee the alarm could not fire. Excluding the scatter's component class must
//       take the scatter alarm away with it, so the warning tracks what ANSWERED rather than
//       what happens to be in the level.
//
// Not a failCode on either half: any_solid admits a scatter as ground deliberately, so a HISM of
// paving stones is a legitimate seat and the caller is the one who decides.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVerifyGroundingWarnsOnUntrustedSurfaceTest,
    "PinWright.spatial.verify_grounding.GroundProvenanceWarnsOnUntrustedSurface",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FVerifyGroundingWarnsOnUntrustedSurfaceTest::RunTest(const FString& Parameters)
{
    using namespace GroundPlacement;

    UWorld* World = GroundTestEditorWorld();
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no_editor_world"),
            TEXT("GEditor->GetEditorWorldContext().World() returned null; the surface-trust "
                 "assertions were stepped over."));
        return true;
    }

    constexpr double FloorPivotZ = 0.0;
    constexpr double ScatterPivotZ = 200.0;
    const double ScatterTopZ = ScatterPivotZ + GroundTestCubeHalf;

    const FString FloorLabel = GroundTestLabel(TEXT("TrustFloor"));
    const FString ScatterLabel = GroundTestLabel(TEXT("TrustScatter"));
    const FString PropLabel = GroundTestLabel(TEXT("TrustProp"));

    AActor* Floor = GroundTestSpawnWideFloor(*this, World, FloorLabel,
        FVector(GroundTestColX, GroundTestColY, FloorPivotZ), 6.0);
    AActor* Scatter = nullptr;
    AActor* Prop = nullptr;
    ON_SCOPE_EXIT
    {
        if (Prop) { Prop->Destroy(); }
        if (Scatter) { Scatter->Destroy(); }
        if (Floor) { Floor->Destroy(); }
    };
    if (!Floor)
    {
        AddError(TEXT("the floor fixture did not spawn"));
        return true;
    }

    // One instance, so the holder is not a scatter HOLDER in the seat-refusal sense (that needs
    // two) and the only thing under test is what the report says about the surface.
    Scatter = GroundTestSpawnScatterHolder(*this, World, ScatterLabel,
        FVector(GroundTestColX, GroundTestColY, ScatterPivotZ), 1, 0.0);
    if (!Scatter)
    {
        AddError(TEXT("the scatter fixture did not spawn"));
        return true;
    }

    UInstancedStaticMeshComponent* ScatterComponent = GroundTestScatterComponent(Scatter);
    UStaticMeshComponent* FloorComponent = Floor->FindComponentByClass<UStaticMeshComponent>();
    if (!ScatterComponent || !FloorComponent)
    {
        AddError(TEXT("the fixtures do not carry the primitives the assertions point columns at"));
        return true;
    }
    ScatterComponent->SetCollisionProfileName(TEXT("BlockAll"));

    // ---- (1) Constructed columns: the structural case, with no physics in the way ----

    // One supported column answered by Component. GroundFaceIndex is deliberately >= 0 - that is
    // what puts the column in triangleColumns and takes it out of the old warning's reach.
    auto TrustColumn = [](UPrimitiveComponent* Component, AActor* Owner,
                          bool bRenderGeometry) -> FGroundColumn
    {
        FGroundColumn Out;
        Out.bHasActorGeometry = true;
        Out.UndersideZ = 100.0;
        Out.bHasGround = true;
        Out.GroundZ = 100.0;
        Out.GroundNormal = FVector::UpVector;
        Out.GroundActor = Owner;
        Out.GroundComponent = Component;
        Out.GroundFaceIndex = 7;
        // A zero simple-shape count is what makes a render-geometry hit possible in the first
        // place, so the two are set together rather than independently.
        Out.GroundSimpleCollisionShapes = bRenderGeometry ? 0 : 1;
        Out.bGroundRenderGeometryHit = bRenderGeometry;
        return Out;
    };

    auto ProvenanceOf = [](const FGroundColumn& Column, bool bTraceComplex)
        -> TSharedPtr<FJsonObject>
    {
        TArray<FGroundColumn> Columns;
        Columns.Add(Column);
        FContactThresholds Thresholds;
        FGroundContactReport Report;
        // Set before the fold, exactly as MeasureContactForBounds does: it describes the PROBE,
        // and AggregateColumns deliberately does not clear it.
        Report.Provenance.bTraceComplex = bTraceComplex;
        AggregateColumns(Columns, Thresholds, Report);
        return MakeProvenanceJson(Report);
    };

    // (1a) A scatter that answered with triangles - the exact set the old predicate excluded.
    {
        const TSharedPtr<FJsonObject> Prov =
            ProvenanceOf(TrustColumn(ScatterComponent, Scatter, /*bRenderGeometry*/ false),
                         /*bTraceComplex*/ true);

        int32 PrimitiveColumns = -1;
        int32 TriangleColumns = -1;
        Prov->TryGetNumberField(TEXT("primitiveColumns"), PrimitiveColumns);
        Prov->TryGetNumberField(TEXT("triangleColumns"), TriangleColumns);
        TestEqual(TEXT("the scatter answered WITH a face index, so the old hull predicate is 0"),
            PrimitiveColumns, 0);
        TestEqual(TEXT("...and the column is counted as a triangle answer"), TriangleColumns, 1);

        int32 ScatterColumns = -1;
        TestTrue(TEXT("the response counts the columns an instanced scatter answered"),
            Prov->TryGetNumberField(TEXT("instancedScatterColumns"), ScatterColumns));
        TestEqual(TEXT("...over exactly the supported columns"), ScatterColumns, 1);

        FString Trust;
        Prov->TryGetStringField(TEXT("surfaceTrust"), Trust);
        TestEqual(TEXT("the surface is stated untrusted rather than left silently clean"),
            Trust, FString(TEXT("untrusted")));

        // THE REGRESSION. This string was empty: the only gate on it was the ABSENCE of the
        // face index this very column carries.
        FString Warning;
        Prov->TryGetStringField(TEXT("warning"), Warning);
        TestTrue(TEXT("a triangle-answering instanced scatter raises the warning"),
            !Warning.IsEmpty());
        TestTrue(*FString::Printf(TEXT("...and the warning NAMES the component that answered "
                                       "(warning was: %s)"), *Warning),
            Warning.Contains(ScatterComponent->GetName()));
        TestTrue(TEXT("...and its class"),
            Warning.Contains(ScatterComponent->GetClass()->GetName()));

        // The row beside it carries the same verdict per surface, so a footprint straddling two
        // surfaces keeps one answer each instead of a single collapsed label.
        const TArray<TSharedPtr<FJsonValue>>* Rows = nullptr;
        const bool bHasRows = Prov->TryGetArrayField(TEXT("surfaceComponents"), Rows)
            && Rows && Rows->Num() == 1 && (*Rows)[0].IsValid();
        TestTrue(TEXT("the surface roster still names the one primitive that answered"), bHasRows);
        if (bHasRows)
        {
            const TSharedPtr<FJsonObject> Row = (*Rows)[0]->AsObject();
            bool bInstancedScatter = false;
            TestTrue(TEXT("the row itself is flagged as an instanced scatter"),
                Row.IsValid() && Row->TryGetBoolField(TEXT("instancedScatter"), bInstancedScatter));
            TestTrue(TEXT("...and the flag reads true"), bInstancedScatter);
        }
    }

    // (1b) A collisionless surface whose RENDER triangles blocked the probe. renderGeometryColumns
    // already counted this and nothing turned the count into a warning.
    {
        const TSharedPtr<FJsonObject> Prov =
            ProvenanceOf(TrustColumn(FloorComponent, Floor, /*bRenderGeometry*/ true),
                         /*bTraceComplex*/ true);

        int32 RenderColumns = -1;
        TestTrue(TEXT("the render-geometry columns are still counted"),
            Prov->TryGetNumberField(TEXT("renderGeometryColumns"), RenderColumns));
        TestEqual(TEXT("...over the one column that hit render triangles"), RenderColumns, 1);

        FString Trust;
        Prov->TryGetStringField(TEXT("surfaceTrust"), Trust);
        TestEqual(TEXT("a render-triangle answer is stated untrusted"),
            Trust, FString(TEXT("untrusted")));

        FString Warning;
        Prov->TryGetStringField(TEXT("warning"), Warning);
        TestTrue(TEXT("a collisionless surface answering off render triangles raises the warning"),
            !Warning.IsEmpty());
        TestTrue(*FString::Printf(TEXT("...naming the component (warning was: %s)"), *Warning),
            Warning.Contains(FloorComponent->GetName()));
    }

    // (1c) The two silences that must not read alike.
    {
        const TSharedPtr<FJsonObject> Clean =
            ProvenanceOf(TrustColumn(FloorComponent, Floor, /*bRenderGeometry*/ false),
                         /*bTraceComplex*/ true);
        FString CleanTrust;
        Clean->TryGetStringField(TEXT("surfaceTrust"), CleanTrust);
        TestEqual(TEXT("an ordinary static-mesh surface answering with triangles reads trusted"),
            CleanTrust, FString(TEXT("trusted")));
        TestFalse(TEXT("...and raises no warning"), Clean->HasField(TEXT("warning")));

        const TSharedPtr<FJsonObject> Unnamed =
            ProvenanceOf(TrustColumn(nullptr, nullptr, /*bRenderGeometry*/ false),
                         /*bTraceComplex*/ true);
        FString UnnamedTrust;
        Unnamed->TryGetStringField(TEXT("surfaceTrust"), UnnamedTrust);
        TestEqual(TEXT("a column whose primitive never resolved reads undetermined, not clean"),
            UnnamedTrust, FString(TEXT("undetermined")));
        TestFalse(TEXT("...and no all-clear is written over a surface nobody could examine"),
            Unnamed->HasField(TEXT("warning")));
        int32 Unclassified = -1;
        TestTrue(TEXT("...with the count of unexaminable columns published beside it"),
            Unnamed->TryGetNumberField(TEXT("unclassifiedColumns"), Unclassified));
        TestEqual(TEXT("...over the one column that had no primitive"), Unclassified, 1);
    }

    // ---- (2) The live verb, at the traceComplex the parent ticket's workaround publishes ----

    Prop = GroundTestSpawnCube(*this, World, PropLabel,
        FVector(GroundTestColX, GroundTestColY, ScatterTopZ + GroundTestCubeHalf));
    if (!Prop)
    {
        AddError(TEXT("the prop fixture did not spawn"));
        return true;
    }
    GroundTestFlushPhysics(World);

    // groundProvenance of the single results[] row.
    auto ProvenanceFromResponse = [](const TSharedPtr<FJsonObject>& Result)
        -> TSharedPtr<FJsonObject>
    {
        const TSharedPtr<FJsonObject> Row = GroundTestFirstRow(Result);
        const TSharedPtr<FJsonObject>* Contact = nullptr;
        if (!Row.IsValid() || !Row->TryGetObjectField(TEXT("contact"), Contact) || !Contact)
        {
            return nullptr;
        }
        const TSharedPtr<FJsonObject>* Provenance = nullptr;
        if (!(*Contact)->TryGetObjectField(TEXT("groundProvenance"), Provenance) || !Provenance)
        {
            return nullptr;
        }
        return *Provenance;
    };

    auto ComplexSurface = []() -> TSharedPtr<FJsonObject>
    {
        TSharedPtr<FJsonObject> Surface = GroundTestAnySolidSurface();
        Surface->SetBoolField(TEXT("traceComplex"), true);
        return Surface;
    };

    TSharedPtr<FJsonObject> ScatterPayload = GroundTestPayloadFor(PropLabel);
    ScatterPayload->SetObjectField(TEXT("surface"), ComplexSurface());

    FTestResponseCapture ScatterCapture;
    TestTrue(TEXT("spatial.verify_grounding handler registered"),
        InvokeHandlerWithCapture(TEXT("spatial.verify_grounding"), ScatterPayload, ScatterCapture));
    TestTrue(TEXT("the complex-traced verify call succeeds"), ScatterCapture.bSuccess);
    if (!ScatterCapture.bSuccess || !ScatterCapture.Result.IsValid())
    {
        return true;
    }

    const TSharedPtr<FJsonObject> ScatterProv = ProvenanceFromResponse(ScatterCapture.Result);
    if (!ScatterProv.IsValid())
    {
        AddError(TEXT("the complex-traced verify call published no groundProvenance"));
        return true;
    }

    // Stated from the response rather than assumed: if the complex trace fell through to the
    // floor this is not the fixture the assertions below describe, and a mis-scoped green is
    // exactly the failure mode this block exists to close.
    const TArray<TSharedPtr<FJsonValue>>* ScatterRows = nullptr;
    TSharedPtr<FJsonObject> TopRow;
    if (ScatterProv->TryGetArrayField(TEXT("surfaceComponents"), ScatterRows) && ScatterRows
        && ScatterRows->Num() > 0 && (*ScatterRows)[0].IsValid())
    {
        TopRow = (*ScatterRows)[0]->AsObject();
    }
    FString AnsweringComponent;
    if (TopRow.IsValid())
    {
        TopRow->TryGetStringField(TEXT("component"), AnsweringComponent);
    }
    TestEqual(TEXT("the complex-traced probe resolved against the scatter, not the floor below it"),
        AnsweringComponent, ScatterComponent->GetName());

    int32 LiveScatterColumns = -1;
    TestTrue(TEXT("the live response counts the columns the scatter answered"),
        ScatterProv->TryGetNumberField(TEXT("instancedScatterColumns"), LiveScatterColumns));
    TestTrue(*FString::Printf(TEXT("...over at least one column (%d)"), LiveScatterColumns),
        LiveScatterColumns > 0);

    FString LiveTrust;
    ScatterProv->TryGetStringField(TEXT("surfaceTrust"), LiveTrust);
    TestEqual(TEXT("the live response states the surface untrusted"),
        LiveTrust, FString(TEXT("untrusted")));

    FString LiveWarning;
    ScatterProv->TryGetStringField(TEXT("warning"), LiveWarning);
    TestTrue(TEXT("the live response warns about the scatter it was seated on"),
        !LiveWarning.IsEmpty());
    TestTrue(*FString::Printf(TEXT("...and names the component (warning was: %s)"), *LiveWarning),
        LiveWarning.Contains(ScatterComponent->GetName()));

    // The negative half: peel the scatter off with the mechanism the preset's own comment points
    // at, and the scatter alarm must go with it. An alarm that stayed would be describing the
    // level rather than the hit.
    TSharedPtr<FJsonObject> FloorPayload = GroundTestPayloadFor(PropLabel);
    TSharedPtr<FJsonObject> FloorSurface = ComplexSurface();
    TArray<TSharedPtr<FJsonValue>> ExcludedComponents;
    ExcludedComponents.Add(MakeShared<FJsonValueString>(ScatterComponent->GetClass()->GetName()));
    FloorSurface->SetArrayField(TEXT("excludeComponentClasses"), ExcludedComponents);
    FloorPayload->SetObjectField(TEXT("surface"), FloorSurface);

    FTestResponseCapture FloorCapture;
    TestTrue(TEXT("spatial.verify_grounding handler registered"),
        InvokeHandlerWithCapture(TEXT("spatial.verify_grounding"), FloorPayload, FloorCapture));
    TestTrue(TEXT("the verify call over the floor succeeds"), FloorCapture.bSuccess);
    if (!FloorCapture.bSuccess || !FloorCapture.Result.IsValid())
    {
        return true;
    }

    const TSharedPtr<FJsonObject> FloorProv = ProvenanceFromResponse(FloorCapture.Result);
    if (!FloorProv.IsValid())
    {
        AddError(TEXT("the floor verify call published no groundProvenance"));
        return true;
    }

    TestFalse(TEXT("with the scatter excluded, no column is attributed to an instanced scatter"),
        FloorProv->HasField(TEXT("instancedScatterColumns")));
    FString FloorWarning;
    FloorProv->TryGetStringField(TEXT("warning"), FloorWarning);
    TestFalse(TEXT("...and the scatter is no longer named in the warning"),
        FloorWarning.Contains(ScatterComponent->GetName()));
    // Whatever the verdict is over the floor, it must be STATED: the field is the affirmative
    // half of the block, and an absent one is the silence this ticket exists to remove.
    TestTrue(TEXT("the floor response still states a surface-trust verdict"),
        FloorProv->HasField(TEXT("surfaceTrust")));

    return true;
}

// ---- The undo record must say what its numbers MEAN, and a replay that cannot restore the
//      scatter must refuse rather than report success ----
//
// movedInstances[] rows are lifted out of the response they arrived in as a matter of routine -
// pasted into a script, stored, handed to another agent - and the space they are in was recorded
// ONLY in the top-level `space` echo beside the array. A batch written with space:"local"
// therefore produced component-local rows whose naive replay fell through to
// actor.set_instance_transforms' world default: the pre-flight accepted them, previousTransform
// became the row's base, UpdateInstanceTransform wrote local numbers as WORLD coordinates, the
// post-write verification re-read in the same wrong space and agreed, and the response said
// updated: 1 with the scatter relocated to garbage.
//
// Red before the fix in three independent places: the row carries no `space` at all; the naive
// replay lands instance 1 at the world origin instead of returning it to the holder; and a replay
// that explicitly disagrees with the record is accepted rather than refused. Every transform
// assertion is against a value re-read from the COMPONENT, and every field read out of the
// response uses a sentinel fallback no real measurement can produce, so a missing field fails the
// assertion instead of satisfying it.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSetInstanceTransformsUndoRecordStatesItsSpaceTest,
    "PinWright.actor.set_instance_transforms.UndoRecordStatesItsSpaceAndReplaysOrRefuses",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSetInstanceTransformsUndoRecordStatesItsSpaceTest::RunTest(const FString& Parameters)
{
    UWorld* World = GroundTestEditorWorld();
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no_editor_world"),
            TEXT("GEditor->GetEditorWorldContext().World() returned null; the undo-record space "
                 "assertions were stepped over."));
        return true;
    }

    const FString HolderLabel = GroundTestLabel(TEXT("UndoSpace"));
    // Three instances 600 cm apart about the pivot, so instance 1 sits at component-local
    // (0, 0, 0) - a local pose that is nowhere near the holder's world location, which is what
    // makes a local-as-world misread unmistakable.
    const FVector HolderLocation(GroundTestColX, GroundTestColY, 300.0);
    AActor* Holder = GroundTestSpawnScatterHolder(*this, World, HolderLabel, HolderLocation, 3, 600.0);
    ON_SCOPE_EXIT
    {
        if (Holder) { Holder->Destroy(); }
    };
    if (!Holder)
    {
        AddError(TEXT("undo-record fixture did not spawn"));
        return true;
    }
    UInstancedStaticMeshComponent* Component = GroundTestScatterComponent(Holder);
    if (!Component)
    {
        AddError(TEXT("undo-record fixture carries no instanced component"));
        return true;
    }

    FTransform BeforeWorld;
    FTransform BeforeLocal;
    if (!Component->GetInstanceTransform(1, BeforeWorld, /*bWorldSpace*/ true)
        || !Component->GetInstanceTransform(1, BeforeLocal, /*bWorldSpace*/ false))
    {
        AddError(TEXT("instance 1 could not be read before the write"));
        return true;
    }

    // A LOCAL-space write. Its record is therefore local, and this is the only call in the test
    // that states a space.
    const auto WriteLocal = [&](FTestResponseCapture& Capture)
    {
        TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
        Row->SetNumberField(TEXT("index"), 1);
        Row->SetObjectField(TEXT("location"), GroundTestVec(
            BeforeLocal.GetLocation().X, BeforeLocal.GetLocation().Y,
            BeforeLocal.GetLocation().Z + 500.0));
        TArray<TSharedPtr<FJsonValue>> Rows;
        Rows.Add(MakeShared<FJsonValueObject>(Row));
        TSharedPtr<FJsonObject> Payload = GroundTestHolderPayload(HolderLabel);
        Payload->SetStringField(TEXT("space"), TEXT("local"));
        Payload->SetArrayField(TEXT("instances"), Rows);
        InvokeHandlerWithCapture(TEXT("actor.set_instance_transforms"), Payload, Capture);
    };

    FTestResponseCapture WriteCapture;
    WriteLocal(WriteCapture);
    TestTrue(TEXT("the local-space write succeeds"), WriteCapture.bSuccess);
    if (!WriteCapture.bSuccess || !WriteCapture.Result.IsValid())
    {
        return true;
    }

    // GROUND TRUTH: 500 cm of LOCAL Z on an unrotated, unscaled holder is 500 cm of world Z.
    FTransform AfterWrite;
    if (!Component->GetInstanceTransform(1, AfterWrite, /*bWorldSpace*/ true))
    {
        AddError(TEXT("instance 1 could not be re-read after the local write"));
        return true;
    }
    TestEqual(TEXT("the local-space write moved the instance 500 cm up in world Z"),
        AfterWrite.GetLocation().Z, BeforeWorld.GetLocation().Z + 500.0, 0.01);

    // ---- 1. The row states its own space ----
    const TSharedPtr<FJsonObject> UndoRow =
        GroundTestRowForIndex(WriteCapture.Result, TEXT("movedInstances"), 1);
    if (!UndoRow.IsValid())
    {
        AddError(TEXT("no movedInstances row for the instance that was written"));
        return true;
    }
    FString RowSpace;
    UndoRow->TryGetStringField(TEXT("space"), RowSpace);
    TestEqual(TEXT("a local-space write's undo row is stamped local"), RowSpace,
        FString(TEXT("local")));

    // ...and the pose it carries is the LOCAL pre-write pose, read at the correct nesting depth
    // (previousTransform.location.z) against a sentinel no instance in this fixture can produce.
    const TSharedPtr<FJsonObject>* UndoTransform = nullptr;
    if (!UndoRow->TryGetObjectField(TEXT("previousTransform"), UndoTransform) || !UndoTransform)
    {
        AddError(TEXT("the movedInstances row carried no previousTransform"));
        return true;
    }
    constexpr double UndoSpaceMissingFieldSentinel = -987654.0;
    TestEqual(TEXT("the undo row carries the pre-write LOCAL Z, not the world one"),
        GroundTestNestedNumber(*UndoTransform, TEXT("location"), TEXT("z"),
            UndoSpaceMissingFieldSentinel),
        BeforeLocal.GetLocation().Z, 0.01);

    // ---- 2. The naive replay - the documented "hand the record straight back" undo, with no
    //         top-level space - must RESTORE the scatter, not write local numbers as world ----
    const TArray<TSharedPtr<FJsonValue>>* UndoRows = nullptr;
    if (!WriteCapture.Result->TryGetArrayField(TEXT("movedInstances"), UndoRows) || !UndoRows)
    {
        AddError(TEXT("the write reported no movedInstances array"));
        return true;
    }

    TSharedPtr<FJsonObject> ReplayPayload = GroundTestHolderPayload(HolderLabel);
    ReplayPayload->SetArrayField(TEXT("instances"), *UndoRows);
    FTestResponseCapture ReplayCapture;
    InvokeHandlerWithCapture(TEXT("actor.set_instance_transforms"), ReplayPayload, ReplayCapture);
    TestTrue(TEXT("the record replays without a restated space"), ReplayCapture.bSuccess);
    if (ReplayCapture.bSuccess && ReplayCapture.Result.IsValid())
    {
        FString EchoedSpace;
        FString SpaceFrom;
        ReplayCapture.Result->TryGetStringField(TEXT("space"), EchoedSpace);
        ReplayCapture.Result->TryGetStringField(TEXT("spaceFrom"), SpaceFrom);
        TestEqual(TEXT("the replay adopts the rows' space rather than its own world default"),
            EchoedSpace, FString(TEXT("local")));
        // The echoed space is what the call USED; this names where it came from, so a caller
        // cannot read it as a default they never passed.
        TestEqual(TEXT("...and the response says the space came from the rows"), SpaceFrom,
            FString(TEXT("instances[].space")));
    }

    // GROUND TRUTH, and the assertion the defect fails: before the fix the replay wrote the
    // component-local numbers as WORLD coordinates, putting instance 1 near the world origin
    // while answering updated: 1.
    FTransform AfterReplay;
    if (!Component->GetInstanceTransform(1, AfterReplay, /*bWorldSpace*/ true))
    {
        AddError(TEXT("instance 1 could not be re-read after the replay"));
        return true;
    }
    TestTrue(*FString::Printf(TEXT("replaying the record restores the world pose exactly "
        "(%.3f, %.3f, %.3f -> %.3f, %.3f, %.3f)"),
        BeforeWorld.GetLocation().X, BeforeWorld.GetLocation().Y, BeforeWorld.GetLocation().Z,
        AfterReplay.GetLocation().X, AfterReplay.GetLocation().Y, AfterReplay.GetLocation().Z),
        AfterReplay.GetLocation().Equals(BeforeWorld.GetLocation(), 0.01));
    TestTrue(TEXT("...and the rotation and scale with it"),
        AfterReplay.GetRotation().Equals(BeforeWorld.GetRotation(), 0.001)
            && AfterReplay.GetScale3D().Equals(BeforeWorld.GetScale3D(), 0.001));

    // ---- 3. A replay that STATES a space the record disagrees with must be refused, not
    //         reinterpreted. This is the direction that used to succeed while destroying the
    //         scatter. ----
    FTestResponseCapture SecondWrite;
    WriteLocal(SecondWrite);
    TestTrue(TEXT("the second local-space write succeeds"), SecondWrite.bSuccess);
    const TArray<TSharedPtr<FJsonValue>>* SecondUndoRows = nullptr;
    if (!SecondWrite.bSuccess || !SecondWrite.Result.IsValid()
        || !SecondWrite.Result->TryGetArrayField(TEXT("movedInstances"), SecondUndoRows)
        || !SecondUndoRows || SecondUndoRows->Num() == 0)
    {
        AddError(TEXT("the second write reported no movedInstances array"));
        return true;
    }
    FTransform BeforeRefusal;
    Component->GetInstanceTransform(1, BeforeRefusal, /*bWorldSpace*/ true);

    TSharedPtr<FJsonObject> ConflictPayload = GroundTestHolderPayload(HolderLabel);
    ConflictPayload->SetArrayField(TEXT("instances"), *SecondUndoRows);
    ConflictPayload->SetStringField(TEXT("space"), TEXT("world"));
    FTestResponseCapture ConflictCapture;
    InvokeHandlerWithCapture(TEXT("actor.set_instance_transforms"), ConflictPayload,
        ConflictCapture);
    TestFalse(TEXT("a local record replayed as world is REFUSED, not silently reinterpreted"),
        ConflictCapture.bSuccess);
    TestEqual(TEXT("the space conflict refuses with INVALID_ARGUMENT"), ConflictCapture.ErrorCode,
        FString(TEXT("INVALID_ARGUMENT")));
    FTransform AfterRefusal;
    Component->GetInstanceTransform(1, AfterRefusal, /*bWorldSpace*/ true);
    TestTrue(TEXT("nothing was written by the refused replay"),
        AfterRefusal.GetLocation().Equals(BeforeRefusal.GetLocation(), 0.001));

    // Agreeing with the record is accepted, and restores.
    TSharedPtr<FJsonObject> AgreePayload = GroundTestHolderPayload(HolderLabel);
    AgreePayload->SetArrayField(TEXT("instances"), *SecondUndoRows);
    AgreePayload->SetStringField(TEXT("space"), TEXT("local"));
    FTestResponseCapture AgreeCapture;
    InvokeHandlerWithCapture(TEXT("actor.set_instance_transforms"), AgreePayload, AgreeCapture);
    TestTrue(TEXT("a replay that agrees with the record is accepted"), AgreeCapture.bSuccess);
    FTransform AfterAgree;
    Component->GetInstanceTransform(1, AfterAgree, /*bWorldSpace*/ true);
    TestTrue(TEXT("...and restores the world pose"),
        AfterAgree.GetLocation().Equals(BeforeWorld.GetLocation(), 0.01));

    // ---- 4. Rows that disagree with EACH OTHER have no single correct reading ----
    TSharedPtr<FJsonObject> WorldRow = MakeShared<FJsonObject>();
    WorldRow->SetNumberField(TEXT("index"), 0);
    WorldRow->SetStringField(TEXT("space"), TEXT("world"));
    WorldRow->SetObjectField(TEXT("location"), GroundTestVec(GroundTestColX, GroundTestColY, 900.0));
    TArray<TSharedPtr<FJsonValue>> MixedRows;
    MixedRows.Add((*SecondUndoRows)[0]);
    MixedRows.Add(MakeShared<FJsonValueObject>(WorldRow));
    TSharedPtr<FJsonObject> MixedPayload = GroundTestHolderPayload(HolderLabel);
    MixedPayload->SetArrayField(TEXT("instances"), MixedRows);
    FTestResponseCapture MixedCapture;
    InvokeHandlerWithCapture(TEXT("actor.set_instance_transforms"), MixedPayload, MixedCapture);
    TestFalse(TEXT("rows declaring different spaces refuse the batch"), MixedCapture.bSuccess);
    TestEqual(TEXT("a mixed-space batch refuses with INVALID_ARGUMENT"), MixedCapture.ErrorCode,
        FString(TEXT("INVALID_ARGUMENT")));

    // ---- 5. A row that requests NO pose restores nothing, so it must not be counted as updated.
    //         It is also what a misspelled row key degrades to: the dispatcher's UNKNOWN_PARAMS
    //         gate is top-level only and never inspects instances[] row keys. ----
    FTransform BeforePoseless;
    Component->GetInstanceTransform(0, BeforePoseless, /*bWorldSpace*/ true);
    TSharedPtr<FJsonObject> PoselessRow = MakeShared<FJsonObject>();
    PoselessRow->SetNumberField(TEXT("index"), 0);
    TArray<TSharedPtr<FJsonValue>> PoselessRows;
    PoselessRows.Add(MakeShared<FJsonValueObject>(PoselessRow));
    TSharedPtr<FJsonObject> PoselessPayload = GroundTestHolderPayload(HolderLabel);
    PoselessPayload->SetArrayField(TEXT("instances"), PoselessRows);
    FTestResponseCapture PoselessCapture;
    InvokeHandlerWithCapture(TEXT("actor.set_instance_transforms"), PoselessPayload,
        PoselessCapture);
    TestFalse(TEXT("a row requesting no pose is refused rather than counted as updated"),
        PoselessCapture.bSuccess);
    TestEqual(TEXT("a pose-less row refuses with INVALID_ARGUMENT"), PoselessCapture.ErrorCode,
        FString(TEXT("INVALID_ARGUMENT")));
    FTransform AfterPoseless;
    Component->GetInstanceTransform(0, AfterPoseless, /*bWorldSpace*/ true);
    TestTrue(TEXT("the refused pose-less batch wrote nothing"),
        AfterPoseless.GetLocation().Equals(BeforePoseless.GetLocation(), 0.001));

    // A row carrying an explicit pose is still accepted - the guard refuses "no pose", not "no
    // previousTransform".
    TSharedPtr<FJsonObject> PosedRow = MakeShared<FJsonObject>();
    PosedRow->SetNumberField(TEXT("index"), 0);
    PosedRow->SetObjectField(TEXT("location"), GroundTestVec(
        BeforePoseless.GetLocation().X, BeforePoseless.GetLocation().Y,
        BeforePoseless.GetLocation().Z + 25.0));
    TArray<TSharedPtr<FJsonValue>> PosedRows;
    PosedRows.Add(MakeShared<FJsonValueObject>(PosedRow));
    TSharedPtr<FJsonObject> PosedPayload = GroundTestHolderPayload(HolderLabel);
    PosedPayload->SetArrayField(TEXT("instances"), PosedRows);
    FTestResponseCapture PosedCapture;
    InvokeHandlerWithCapture(TEXT("actor.set_instance_transforms"), PosedPayload, PosedCapture);
    TestTrue(TEXT("a row with an explicit location is still accepted"), PosedCapture.bSuccess);
    FTransform AfterPosed;
    Component->GetInstanceTransform(0, AfterPosed, /*bWorldSpace*/ true);
    TestEqual(TEXT("...and it wrote what it asked for"), AfterPosed.GetLocation().Z,
        BeforePoseless.GetLocation().Z + 25.0, 0.01);
    return true;
}

// ---- spatial.ground_instances' record must state world space rather than rely on a coincidence
//      ----
//
// GroundPlacement::SeatInstance hardcodes bWorldSpace=true on both its read and its write, and the
// verb declares no `space` parameter at all - so its rows really are world-space, and
// actor.set_instance_transforms really does default to world. Correct BY COINCIDENCE: one verb's
// hardcode happened to equal the other verb's default, recorded in no field, comment or doc on
// either side, so changing either would silently stop them agreeing. Red before the fix: no row
// carried a `space` field for the checked contract to read.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGroundInstancesUndoRecordStatesWorldSpaceTest,
    "PinWright.spatial.ground_instances.UndoRecordStatesWorldSpace",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGroundInstancesUndoRecordStatesWorldSpaceTest::RunTest(const FString& Parameters)
{
    UWorld* World = GroundTestEditorWorld();
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no_editor_world"),
            TEXT("GEditor->GetEditorWorldContext().World() returned null; the ground_instances "
                 "undo-record space assertions were stepped over."));
        return true;
    }

    const FString FloorLabel = GroundTestLabel(TEXT("UndoWFloor"));
    const FString HolderLabel = GroundTestLabel(TEXT("UndoWSeat"));
    AActor* Floor = GroundTestSpawnWideFloor(*this, World, FloorLabel,
        FVector(GroundTestColX, GroundTestColY, -GroundTestCubeHalf), 20.0);
    const FVector HolderLocation(GroundTestColX, GroundTestColY, 300.0);
    AActor* Holder = GroundTestSpawnScatterHolder(*this, World, HolderLabel, HolderLocation, 3, 600.0);
    ON_SCOPE_EXIT
    {
        if (Floor) { Floor->Destroy(); }
        if (Holder) { Holder->Destroy(); }
    };
    if (!Floor || !Holder)
    {
        AddError(TEXT("ground_instances undo-space fixture did not spawn"));
        return true;
    }
    GroundTestFlushPhysics(World);

    TSharedPtr<FJsonObject> Payload = GroundTestHolderPayload(HolderLabel);
    Payload->SetObjectField(TEXT("surface"), GroundTestAnySolidSurface());
    FTestResponseCapture Capture;
    InvokeHandlerWithCapture(TEXT("spatial.ground_instances"), Payload, Capture);
    TestTrue(TEXT("the seat itself succeeds"), Capture.bSuccess);
    if (!Capture.bSuccess || !Capture.Result.IsValid())
    {
        return true;
    }

    const TArray<TSharedPtr<FJsonValue>>* MovedRows = nullptr;
    if (!Capture.Result->TryGetArrayField(TEXT("movedInstances"), MovedRows) || !MovedRows
        || MovedRows->Num() == 0)
    {
        AddError(TEXT("the seat wrote no movedInstances record to assert about"));
        return true;
    }

    // EVERY row, not the first: a stamp applied by one branch of the writer and not another is
    // exactly the inconsistency the shared row builder exists to prevent.
    int32 WorldStamped = 0;
    for (const TSharedPtr<FJsonValue>& Value : *MovedRows)
    {
        const TSharedPtr<FJsonObject> Row = Value.IsValid() ? Value->AsObject() : nullptr;
        if (!Row.IsValid())
        {
            continue;
        }
        FString RowSpace;
        Row->TryGetStringField(TEXT("space"), RowSpace);
        WorldStamped += (RowSpace == TEXT("world")) ? 1 : 0;
    }
    TestEqual(TEXT("every ground_instances undo row is stamped world"), WorldStamped,
        MovedRows->Num());
    return true;
}

// ---- The footprint is the silhouette, and for a trunked mesh the silhouette is the canopy ----
//
// spatial.ground_instances derived BOTH where it probed and what the underside looked like from
// one source: the instance's world AABB. For a rock, a slab, a log or a boulder that box IS the
// contact patch - measured bounds/contact area ratios 0.98-2.49x - and the model is right. For a
// trunked tree it is the canopy: 46.7x the contact area on HillTree_P2, 80.8x on SM_Dead_Tree. The
// measured consequence in a real level was that 177 of 177 already-correctly-planted instances
// were proposed for a LIFT, p50 +196 cm and max +773 cm, while every row reported coverage 1.00,
// pass true and undersideReliefCm 0 - all three truthful ABOUT THE BOX, which is exactly why none
// of them caught it.
//
// This fixture reproduces the mechanism rather than the asset (the measured meshes live in another
// project): a cone instance apex-down over a flat floor, with a raised ledge placed where the
// BOUNDS sample grid reaches and a contact-sized grid does not. Both instances are seated exactly,
// beforehand, by the verb's own model - mesh bounds under the instance transform, lowest point one
// embed depth under the floor - so the only correct proposal is approximately zero and any lift is
// the defect itself.
//
// The second run is what makes this a bug report rather than a feature request: footprintInset
// driven to its 0.45 ceiling still spans 55% of the bounds, still reaches the ledge, and still
// proposes the same lift. That is the miniature of the measured result, where the ceiling left
// 361 x 441 uu of sample half-extent around a 100 uu contact radius and a p50 of +76.9 cm.

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGroundInstancesContactRadiusSamplesTheContactPatchTest,
    "PinWright.spatial.ground_instances.ContactRadiusSamplesTheContactPatch",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGroundInstancesContactRadiusSamplesTheContactPatchTest::RunTest(const FString& Parameters)
{
    UWorld* World = GroundTestEditorWorld();
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no_editor_world"),
            TEXT("GEditor->GetEditorWorldContext().World() returned null; the contactRadius "
                 "footprint assertions were stepped over."));
        return true;
    }

    UStaticMesh* Cone = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cone.Cone"));
    if (!Cone)
    {
        AddError(TEXT("engine cone mesh unavailable for the narrow-contact fixture"));
        return true;
    }

    // Every distance below is derived from the mesh, so the fixture cannot silently stop
    // straddling the ledge if the engine primitive's bounds ever change. Sample-grid X columns, as
    // fractions of the mesh half-extent: bounds with the default inset 0.90, bounds with the
    // maximum inset 0.55, contactRadius 0.20. The ledge spans 0.40 to 2.00 - under the first two,
    // clear of the third.
    const double MeshHalfX = Cone->GetBounds().BoxExtent.X;
    if (MeshHalfX <= 1.0)
    {
        AddError(TEXT("the engine cone reports no XY footprint to sample"));
        return true;
    }
    const double ContactRadiusCm = 0.20 * MeshHalfX;
    const double LedgeInnerX = 0.40 * MeshHalfX;
    const double LedgeOuterX = 2.00 * MeshHalfX;
    const double LedgeRiseCm = 60.0;
    const double FarInstanceOffsetX = -8.0 * MeshHalfX;

    const FString FloorLabel = GroundTestLabel(TEXT("ContactFloor"));
    const FString LedgeLabel = GroundTestLabel(TEXT("ContactLedge"));
    const FString HolderLabel = GroundTestLabel(TEXT("ContactScatter"));

    AActor* Floor = GroundTestSpawnWideFloor(*this, World, FloorLabel,
        FVector(GroundTestColX, GroundTestColY, -GroundTestCubeHalf), 20.0);
    AActor* Ledge = nullptr;
    AActor* Holder = nullptr;
    ON_SCOPE_EXIT
    {
        if (Floor) { Floor->Destroy(); }
        if (Ledge) { Ledge->Destroy(); }
        if (Holder) { Holder->Destroy(); }
    };
    if (!Floor)
    {
        AddError(TEXT("narrow-contact floor did not spawn"));
        return true;
    }
    const double FloorTopZ = GroundTestTopZ(Floor);

    // Spanning [LedgeInnerX, LedgeOuterX] in X and wide enough in Y to answer all three rows of
    // the bounds grid's outer column. Its top sits LedgeRiseCm above the floor, which is exactly
    // the lift a first-contact solve over the wrong footprint has to propose.
    Ledge = GroundTestSpawnScaledMesh(*this, World, LedgeLabel,
        TEXT("/Engine/BasicShapes/Cube.Cube"),
        FVector(GroundTestColX + 0.5 * (LedgeInnerX + LedgeOuterX), GroundTestColY,
            FloorTopZ + LedgeRiseCm - GroundTestCubeHalf),
        FVector(0.5 * (LedgeOuterX - LedgeInnerX) / GroundTestCubeHalf, 2.0, 1.0));

    // Two instances at different scales: the one over the ledge, and a control far from it whose
    // job is the scaled half-extent - contactRadius is MESH-LOCAL, so instance 1 at scale 2 must
    // sample twice the box instance 0 does off the same stated number.
    Holder = GroundTestSpawnNarrowContactScatter(*this, World, HolderLabel,
        FVector(GroundTestColX, GroundTestColY, 0.0),
        TArray<FVector>({FVector::ZeroVector, FVector(FarInstanceOffsetX, 0.0, 0.0)}),
        TArray<double>({1.0, 2.0}));
    if (!Ledge || !Holder)
    {
        AddError(TEXT("narrow-contact ledge or scatter did not spawn"));
        return true;
    }
    GroundTestFlushPhysics(World);

    UInstancedStaticMeshComponent* Component = GroundTestScatterComponent(Holder);
    if (!Component || Component->GetInstanceCount() != 2)
    {
        AddError(TEXT("the narrow-contact scatter did not carry its two instances"));
        return true;
    }

    // GROUND TRUTH: seat both instances EXACTLY where this verb's own model says a seated instance
    // belongs - lowest AABB point one default embed depth under the floor - so the expected
    // proposal is zero for structural reasons rather than by tuning. It also makes the expected
    // lift independent of the embed and of the mesh height: both cancel out of
    // -(seatClearance + embed), leaving exactly the ledge rise.
    for (int32 Index = 0; Index < 2; ++Index)
    {
        FBox Box(ForceInit);
        if (!GroundTestInstanceBox(Component, Index, Box)
            || !GroundTestSeatInstanceExactly(Component, Index, FloorTopZ,
                   GroundPlacement::DefaultEmbedFraction * Box.GetSize().Z))
        {
            AddError(*FString::Printf(TEXT("instance %d could not be pre-seated"), Index));
            return true;
        }
    }

    // ---- Run 1: the bounds footprint, i.e. what the verb did before contactRadius existed ----

    TSharedPtr<FJsonObject> BoundsPayload = GroundTestHolderPayload(HolderLabel);
    BoundsPayload->SetObjectField(TEXT("surface"), GroundTestAnySolidSurface());
    BoundsPayload->SetBoolField(TEXT("apply"), false);
    BoundsPayload->SetStringField(TEXT("detail"), TEXT("all"));

    FTestResponseCapture BoundsCapture;
    TestTrue(TEXT("spatial.ground_instances handler registered"),
        InvokeHandlerWithCapture(TEXT("spatial.ground_instances"), BoundsPayload, BoundsCapture));
    TestTrue(TEXT("the bounds-footprint dry run succeeds"), BoundsCapture.bSuccess);
    const TSharedPtr<FJsonObject> BoundsRow =
        GroundTestRowForIndex(BoundsCapture.Result, TEXT("results"), 0);
    const TSharedPtr<FJsonObject> BoundsFarRow =
        GroundTestRowForIndex(BoundsCapture.Result, TEXT("results"), 1);
    if (!BoundsRow.IsValid() || !BoundsFarRow.IsValid())
    {
        AddError(TEXT("the bounds-footprint dry run returned no per-instance rows"));
        return true;
    }

    double BoundsProposed = 0.0;
    BoundsRow->TryGetNumberField(TEXT("proposedDeltaZCm"), BoundsProposed);
    TestEqual(TEXT("the bounds footprint reaches the ledge and proposes lifting a correctly-seated "
                   "instance clear of it"),
        BoundsProposed, LedgeRiseCm, 0.5);

    // The three fields a caller gates on, all green about a box the caller never asked about.
    TestEqual(TEXT("...while reporting coverage 1.00"),
        GroundTestRowContactNumber(BoundsRow, TEXT("coverage"), -1.0), 1.0, 0.001);
    TestEqual(TEXT("...and undersideReliefCm 0"),
        GroundTestRowContactNumber(BoundsRow, TEXT("undersideReliefCm"), -1.0), 0.0, 0.001);
    const TSharedPtr<FJsonObject>* BoundsContact = nullptr;
    bool bBoundsPass = false;
    if (BoundsRow->TryGetObjectField(TEXT("contact"), BoundsContact) && BoundsContact)
    {
        (*BoundsContact)->TryGetBoolField(TEXT("pass"), bBoundsPass);
    }
    TestTrue(TEXT("...and pass true. None of the three can catch this, because all three are "
                  "correct about the box that was sampled"), bBoundsPass);

    // Which is why the box itself has to be in the response.
    TestEqual(TEXT("the response NAMES the footprint it sampled: the inset bounds half-extent"),
        GroundTestRowFootprintHalf(BoundsRow, TEXT("x"), -1.0),
        MeshHalfX * (1.0 - GroundPlacement::DefaultFootprintInset), 0.01);
    TestEqual(TEXT("...and states where it came from"),
        GroundTestRowContactString(BoundsRow, TEXT("footprintSource")), FString(TEXT("bounds")));

    // The control: an identical solve away from the ledge is already a no-op, so the lift above is
    // the footprint reaching foreign ground and not a mis-seated fixture.
    double BoundsFarProposed = -1.0;
    BoundsFarRow->TryGetNumberField(TEXT("proposedDeltaZCm"), BoundsFarProposed);
    TestEqual(TEXT("the control instance, clear of the ledge, is already a no-op"),
        BoundsFarProposed, 0.0, 0.5);

    // ---- Run 2: footprintInset at its ceiling - the parameter that looks like the remedy ----

    TSharedPtr<FJsonObject> InsetPayload = GroundTestHolderPayload(HolderLabel);
    InsetPayload->SetObjectField(TEXT("surface"), GroundTestAnySolidSurface());
    InsetPayload->SetBoolField(TEXT("apply"), false);
    InsetPayload->SetStringField(TEXT("detail"), TEXT("all"));
    InsetPayload->SetNumberField(TEXT("footprintInset"), 0.45);

    FTestResponseCapture InsetCapture;
    InvokeHandlerWithCapture(TEXT("spatial.ground_instances"), InsetPayload, InsetCapture);
    TestTrue(TEXT("the max-inset dry run succeeds"), InsetCapture.bSuccess);
    const TSharedPtr<FJsonObject> InsetRow =
        GroundTestRowForIndex(InsetCapture.Result, TEXT("results"), 0);
    if (!InsetRow.IsValid())
    {
        AddError(TEXT("the max-inset dry run returned no row for instance 0"));
        return true;
    }
    double InsetProposed = 0.0;
    InsetRow->TryGetNumberField(TEXT("proposedDeltaZCm"), InsetProposed);
    TestEqual(TEXT("footprintInset at its 0.45 ceiling still spans 55 percent of the bounds, still "
                   "reaches the ledge, and still proposes the same lift"),
        InsetProposed, LedgeRiseCm, 0.5);
    TestEqual(TEXT("...and the sampled half-extent is still a fraction of the bounds"),
        GroundTestRowFootprintHalf(InsetRow, TEXT("x"), -1.0), MeshHalfX * 0.55, 0.01);

    // ---- Run 3: contactRadius - sample the contact patch instead ----

    TSharedPtr<FJsonObject> ContactPayload = GroundTestHolderPayload(HolderLabel);
    ContactPayload->SetObjectField(TEXT("surface"), GroundTestAnySolidSurface());
    ContactPayload->SetBoolField(TEXT("apply"), false);
    ContactPayload->SetStringField(TEXT("detail"), TEXT("all"));
    ContactPayload->SetNumberField(TEXT("contactRadius"), ContactRadiusCm);

    FTestResponseCapture ContactCapture;
    InvokeHandlerWithCapture(TEXT("spatial.ground_instances"), ContactPayload, ContactCapture);
    TestTrue(TEXT("the contact-footprint dry run succeeds"), ContactCapture.bSuccess);
    const TSharedPtr<FJsonObject> ContactRow =
        GroundTestRowForIndex(ContactCapture.Result, TEXT("results"), 0);
    const TSharedPtr<FJsonObject> ContactFarRow =
        GroundTestRowForIndex(ContactCapture.Result, TEXT("results"), 1);
    if (!ContactRow.IsValid() || !ContactFarRow.IsValid())
    {
        AddError(TEXT("the contact-footprint dry run returned no per-instance rows"));
        return true;
    }

    double ContactProposed = -1.0;
    ContactRow->TryGetNumberField(TEXT("proposedDeltaZCm"), ContactProposed);
    TestEqual(TEXT("with the contact footprint the same already-seated instance is proposed no "
                   "move at all"),
        ContactProposed, 0.0, 0.5);

    TestEqual(TEXT("the sampled half-extent is the stated contact radius, in X"),
        GroundTestRowFootprintHalf(ContactRow, TEXT("x"), -1.0), ContactRadiusCm, 0.01);
    TestEqual(TEXT("...and in Y"),
        GroundTestRowFootprintHalf(ContactRow, TEXT("y"), -1.0), ContactRadiusCm, 0.01);
    TestEqual(TEXT("...and the row states which source produced it"),
        GroundTestRowContactString(ContactRow, TEXT("footprintSource")),
        FString(TEXT("contact_radius")));

    // MESH-LOCAL, not a world constant and not a bounds ratio: instance 1 is the same mesh at
    // twice the scale off the same stated number, so its sampled patch is twice as wide. A bounds
    // ratio would have produced the same FRACTION of two different boxes instead.
    TestEqual(TEXT("contactRadius is mesh-local: a 2x instance samples twice the half-extent"),
        GroundTestRowFootprintHalf(ContactFarRow, TEXT("x"), -1.0), 2.0 * ContactRadiusCm, 0.01);

    // The verdict must not be bought at the cost of the measurement: the contact columns are all
    // over real floor, so coverage stays exactly as green as it was - honestly this time.
    TestEqual(TEXT("the contact footprint still finds ground under all of itself"),
        GroundTestRowContactNumber(ContactRow, TEXT("coverage"), -1.0), 1.0, 0.001);

    // Call-level echo: which question the batch asked, beside the per-row answers.
    const TSharedPtr<FJsonObject>* SeatEcho = nullptr;
    if (!ContactCapture.Result.IsValid()
        || !ContactCapture.Result->TryGetObjectField(TEXT("seat"), SeatEcho) || !SeatEcho)
    {
        AddError(TEXT("the contact-footprint run echoed no seat block"));
        return true;
    }
    FString EchoedSource;
    (*SeatEcho)->TryGetStringField(TEXT("footprintSource"), EchoedSource);
    TestEqual(TEXT("the seat echo states the footprint source for the batch"), EchoedSource,
        FString(TEXT("contact_radius")));
    double EchoedRadius = -1.0;
    (*SeatEcho)->TryGetNumberField(TEXT("contactRadiusCm"), EchoedRadius);
    TestEqual(TEXT("...and echoes the radius it was given"), EchoedRadius, ContactRadiusCm, 0.001);
    return true;
}
