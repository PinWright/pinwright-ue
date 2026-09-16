// Copyright (c) 2026 Alexander Penkin. MIT License.

// TestFoliagePaintGroundProjection.cpp - regression coverage for
// B-foliage-paint-does-no-ground-projection.
//
// WHAT WAS WRONG. foliage.paint's entire placement loop hand-built an FFoliageInstance and
// handed it to FFoliageInfo::AddInstance - the STORAGE call, which applies no placement at all
// (InstancedFoliage.cpp: AddInstanceImpl appends to Instances, stamps a base id and inserts into
// the location hash, and that is the whole of it). The engine's actual placement routine,
// FPotentialInstance::PlaceInstance, is driven end to end by a trace hit: `Inst.Location =
// HitLocation`, a random yaw, AlignToNormal against the hit normal, a local-space ZOffset and a
// world collision check. `paint` reached none of it, so every instance landed at the literal
// requested Z - and the response said `success`, a correct `instancesPlaced` and a hardcoded
// `existsAfter: true`, with no field a caller could read to discover that nothing had been
// measured against any surface.
//
// WHAT THE FIX DOES. Supplying `surface` seats each instance through
// GroundPlacement::SeatInstance - the same measure/seat solve spatial.ground_instances uses,
// taken as a dry run so the write can go through the foliage bookkeeping rather than through the
// component alone. Omitting `surface` still writes the literal Z, but the response then carries
// `projected: false` and a warnings[] line saying so in as many words. Either way the deciding
// fact is published.
//
// THE THREE TESTS ARE THE THREE HALVES OF THAT CONTRACT, and each is red before the fix:
//   (a) with a surface, an instance requested 800 cm above a floor ends up ON the floor - Z is
//       re-read from FFoliageInfo::Instances AND from the HISM's own instance transform, because
//       a write that moved one and not the other is the specific desync this code path risks.
//       Before the fix the instance stays at the requested Z (and `surface` is not even a
//       declared param, so the call is refused outright).
//   (b) without a surface, the response says plainly that it did not project. Before the fix
//       there is no `projected` key and no warning at all.
//   (c) a `{}` entry - three missing coordinates - is reported in skipped[] instead of being
//       placed at the world origin. Before the fix `double X = 0, Y = 0, Z = 0` survives three
//       failed TryGetNumberField calls and the entry is placed at (0,0,0) with no record.
//
// Case (c) is the second silent-input defect the same ticket carries, and is deliberately NOT
// covered by Tests/Environment/TestFoliagePlacementBehaviour.cpp, whose junk-entry test uses
// non-object entries only and says so.
//
// FIXTURES. The ground is a real AStaticMeshActor spawned through actor.spawn into an isolated
// far column of the editor world and destroyed on scope exit, following
// Tests/Spatial/TestGroundPlacement.cpp - including its world-tick flush, without which a
// just-spawned body is not yet in the scene-query structure and the probe would miss its own
// floor. The foliage type is built through the real foliage.add_type verb under a GUID-suffixed
// name and torn down type-scoped, so absolute instance counts are meaningful and the host map's
// own foliage is never touched.
//
// Requests route through the real production dispatcher (FRpcDispatcher::ProcessRequest -> the
// registered handler), which also validates each payload against the declared ParamSpec.
#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Dispatch/RpcDispatcher.h"
#include "Tests/Infra/DispatcherTestHelpers.h"
#include "Tests/TestSkipReporting.h"
#include "Tests/TestUtils.h"
#include "Utils/ActorUtils.h"

#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "Editor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "FoliageType.h"
#include "GameFramework/Actor.h"
#include "InstancedFoliage.h"
#include "InstancedFoliageActor.h"
#include "Misc/Guid.h"
#include "Misc/ScopeExit.h"

// Named (not anonymous) namespace: the plugin's tests share one module with Unity builds
// enabled, where same-named anonymous-namespace helpers collide across merged translation units.
namespace FoliagePaintProjectionTestHelpers
{
    // The only content fixture, shared by the floor and the foliage type. The engine cube is
    // 100 x 100 x 100 with a CENTRED pivot, so its pivot sits 50 cm above its lowest geometry -
    // which is what makes the expected answer discriminating: a solver that set pivot Z to ground
    // Z would bury the instance by half its height, and one that never moved it leaves it at the
    // requested Z.
    constexpr const TCHAR* CubeMeshPath = TEXT("/Engine/BasicShapes/Cube.Cube");
    constexpr double CubeHalf = 50.0;

    // An isolated column, away from the ones TestGroundPlacement.cpp, TestPlacementHandlers.cpp
    // and TestFoliagePlacementBehaviour.cpp use, and lifted well clear of any landscape a host
    // map might carry so the only thing under the probe is this test's own floor.
    constexpr double ColX = 398400.0;
    constexpr double ColY = 291600.0;

    // The floor's top face. The floor actor's pivot goes half a cube below it.
    constexpr double FloorTopZ = 20000.0;
    // Requested paint height: 800 cm of clear air above the floor, so a projection is a large,
    // unmistakable move rather than a rounding difference.
    constexpr double RequestZ = FloorTopZ + 800.0;
    // Where a correctly seated instance's PIVOT lands: its underside on the floor.
    constexpr double ExpectedSeatedZ = FloorTopZ + CubeHalf;

    // Placement is exact arithmetic on both paths; the tolerance only absorbs the JSON double
    // round trip and the solve's own float noise, and is far tighter than the 750 cm the
    // projection has to move the instance.
    constexpr double PosTolerance = 1.0;

    inline UWorld* EditorWorld()
    {
        return GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    }

    inline FString UniqueName(const TCHAR* Prefix)
    {
        return FString::Printf(TEXT("PWFoliageProj_%s_%s"), Prefix,
            *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    }

    // foliage.add_type builds package "/Game/Foliage/<Name>" and names the object inside it
    // "<Name>", so the loadable object path is "/Game/Foliage/<Name>.<Name>" while asset deletion
    // takes the package path.
    inline FString PackagePathFor(const FString& Name)
    {
        return FString::Printf(TEXT("/Game/Foliage/%s"), *Name);
    }

    inline FString ObjectPathFor(const FString& Name)
    {
        return FString::Printf(TEXT("/Game/Foliage/%s.%s"), *Name, *Name);
    }

    inline TSharedPtr<FJsonObject> Vec3(double X, double Y, double Z)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetNumberField(TEXT("x"), X);
        Obj->SetNumberField(TEXT("y"), Y);
        Obj->SetNumberField(TEXT("z"), Z);
        return Obj;
    }

    inline TSharedPtr<FJsonValue> LocationEntry(double X, double Y, double Z)
    {
        return MakeShared<FJsonValueObject>(Vec3(X, Y, Z));
    }

    // {"preset":"any_solid"}. The landscape preset is the production default for terrain, but the
    // automation world has no landscape at this column, so the floor is an ordinary static mesh
    // actor and any_solid is the preset that accepts it.
    inline TSharedPtr<FJsonObject> AnySolidSurface()
    {
        TSharedPtr<FJsonObject> Surface = MakeShared<FJsonObject>();
        Surface->SetStringField(TEXT("preset"), TEXT("any_solid"));
        return Surface;
    }

    // Editor worlds do not tick physics on their own, so a just-spawned body is absent from the
    // scene-query structure until a tick flushes it and a ground probe issued in the same call
    // would miss this test's own floor. Editor worlds do not simulate, so nothing moves.
    inline void FlushPhysics(UWorld* World)
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

    // The ground. Spawned through the real actor.spawn verb (as TestGroundPlacement.cpp does)
    // rather than built by hand, then widened on XY so the instance's own 100 x 100 footprint is
    // comfortably inside it and coverage cannot be the reason a seat fails. Scaling XY leaves the
    // Z half-extent at 50, so the top face stays exactly at FloorTopZ.
    inline AActor* SpawnFloor(FAutomationTestBase& Test, FRpcDispatcher& Dispatcher,
        DispatcherTestHelpers::FSinkPtr& Sink, UWorld* World, const FString& Label)
    {
        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("meshPath"), CubeMeshPath);
        Params->SetStringField(TEXT("actorName"), Label);
        Params->SetObjectField(TEXT("location"), Vec3(ColX, ColY, FloorTopZ - CubeHalf));
        Params->SetObjectField(TEXT("scale"), Vec3(4.0, 4.0, 1.0));

        bool bSuccess = false;
        FString ErrorCode;
        DispatcherTestHelpers::Dispatch(Dispatcher, Sink, TEXT("actor.spawn"),
            TEXT("req-foliage-projection-floor"), Params, bSuccess, ErrorCode);
        Test.TestTrue(*FString::Printf(TEXT("the floor fixture spawned (error=%s)"), *ErrorCode),
            bSuccess);

        return McpActorUtils::FindActorByName(World, Label);
    }

    inline bool CreateFoliageType(FAutomationTestBase& Test, FRpcDispatcher& Dispatcher,
        DispatcherTestHelpers::FSinkPtr& Sink, const FString& Name)
    {
        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("name"), Name);
        Params->SetStringField(TEXT("meshPath"), CubeMeshPath);
        // A degenerate [1,1] scale range pins the instance's footprint, so the expected seated Z
        // is an exact number rather than a range.
        Params->SetNumberField(TEXT("minScale"), 1.0);
        Params->SetNumberField(TEXT("maxScale"), 1.0);
        Params->SetBoolField(TEXT("alignToNormal"), false);
        Params->SetBoolField(TEXT("randomYaw"), false);

        bool bSuccess = false;
        FString ErrorCode;
        DispatcherTestHelpers::Dispatch(Dispatcher, Sink, TEXT("foliage.add_type"),
            TEXT("req-foliage-projection-add-type"), Params, bSuccess, ErrorCode);
        Test.TestTrue(*FString::Printf(TEXT("foliage.add_type created the '%s' fixture (error=%s)"),
            *Name, *ErrorCode), bSuccess);
        return bSuccess;
    }

    inline UFoliageType* LoadFoliageType(const FString& Name)
    {
        return LoadObject<UFoliageType>(nullptr, *ObjectPathFor(Name));
    }

    // Type-scoped teardown: empties only this fixture's instances (a `removeAll` would wipe the
    // host map's own foliage) and then deletes the asset, which also clears the dirty /Game
    // package a later editor-wide save-all would otherwise flush into host Content.
    inline void DiscardType(FRpcDispatcher& Dispatcher, DispatcherTestHelpers::FSinkPtr& Sink,
        const FString& Name)
    {
        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("foliageTypePath"), ObjectPathFor(Name));

        bool bSuccess = false;
        FString ErrorCode;
        DispatcherTestHelpers::Dispatch(Dispatcher, Sink, TEXT("foliage.remove"),
            TEXT("req-foliage-projection-cleanup"), Params, bSuccess, ErrorCode);

        CleanupTestAsset(PackagePathFor(Name));
    }

    // GROUND TRUTH #1 - the foliage RECORD. Every instance the level holds for this type, read
    // off FFoliageInfo::Instances across every AInstancedFoliageActor in the world, never off a
    // handler response.
    inline int32 GatherInstances(UWorld* World, const UFoliageType* Type,
        TArray<FFoliageInstance>& OutInstances)
    {
        OutInstances.Reset();
        if (!World || !Type)
        {
            return 0;
        }
        for (TActorIterator<AInstancedFoliageActor> It(World); It; ++It)
        {
            if (AInstancedFoliageActor* Ifa = *It)
            {
                if (const FFoliageInfo* Info = Ifa->FindInfo(Type))
                {
                    OutInstances.Append(Info->Instances);
                }
            }
        }
        return OutInstances.Num();
    }

    // GROUND TRUTH #2 - the RENDER instance. The record and the HISM are two representations of
    // one instance and they are written by different calls, so a fix that moved the record and
    // left the component (or the reverse) would look correct to #1 alone while the viewport
    // showed vegetation in mid-air. Returns false when the type has no instanced component or no
    // instance at Index.
    inline bool ReadComponentInstanceWorld(UWorld* World, const UFoliageType* Type, int32 Index,
        FTransform& OutTransform)
    {
        if (!World || !Type)
        {
            return false;
        }
        for (TActorIterator<AInstancedFoliageActor> It(World); It; ++It)
        {
            AInstancedFoliageActor* Ifa = *It;
            FFoliageInfo* Info = Ifa ? Ifa->FindInfo(Type) : nullptr;
            if (!Info)
            {
                continue;
            }
            UHierarchicalInstancedStaticMeshComponent* Component = Info->GetComponent();
            if (Component && Component->GetInstanceTransform(Index, OutTransform,
                    /*bWorldSpace*/ true))
            {
                return true;
            }
        }
        return false;
    }

    // Reads an integer response field, failing when the key is absent so a dropped field never
    // reads as a zero that silently satisfies a comparison.
    inline int32 RequireNumberField(FAutomationTestBase& Test,
        const TSharedPtr<FJsonObject>& Result, const TCHAR* Field)
    {
        double Value = 0.0;
        const bool bPresent = Result.IsValid() && Result->TryGetNumberField(Field, Value);
        Test.TestTrue(*FString::Printf(TEXT("response carries %s"), Field), bPresent);
        return bPresent ? static_cast<int32>(Value) : -1;
    }
}

using DispatcherTestHelpers::MakeDispatcher;
using DispatcherTestHelpers::Dispatch;

// Each RunTest opens FoliagePaintProjectionTestHelpers inside its own body rather than at file
// scope: a file-scope using-directive would leak into every other test .cpp that Unity merges
// after this one into the same translation unit.

// ============================================================================
// (a) With a surface, paint projects.
// ============================================================================

// COUNTERFACTUAL. This is the ticket itself. A `paint` that writes the requested Z leaves the
// instance at RequestZ - 800 cm of clear air above the floor - and both ground-truth reads land
// on that number instead of ExpectedSeatedZ. A `paint` that projected the PIVOT onto the hit
// instead of seating the instance's underside would land at FloorTopZ, half a cube buried, which
// the centred-pivot cube is chosen to expose. A `paint` that moved the record but not the HISM
// passes the first read and fails the second.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFoliagePaintSeatsOntoSurfaceTest,
    "PinWright.foliage.paint.SeatsInstancesOntoTheNamedSurface",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FFoliagePaintSeatsOntoSurfaceTest::RunTest(const FString& Parameters)
{
    using namespace FoliagePaintProjectionTestHelpers;

    UWorld* World = EditorWorld();
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no_editor_world"),
            TEXT("GEditor->GetEditorWorldContext().World() returned null; the foliage.paint "
                 "ground-projection assertions were stepped over."));
        return true;
    }

    DispatcherTestHelpers::FSinkPtr Sink;
    FRpcDispatcher Dispatcher;
    MakeDispatcher(Sink, Dispatcher);

    const FString TypeName = UniqueName(TEXT("Seat"));
    const FString FloorLabel = UniqueName(TEXT("SeatFloor"));

    AActor* Floor = SpawnFloor(*this, Dispatcher, Sink, World, FloorLabel);
    ON_SCOPE_EXIT
    {
        DiscardType(Dispatcher, Sink, TypeName);
        if (Floor)
        {
            Floor->Destroy();
        }
    };
    if (!Floor)
    {
        AddError(TEXT("the floor fixture did not spawn, so there was no surface to project onto"));
        return true;
    }
    FlushPhysics(World);

    if (!CreateFoliageType(*this, Dispatcher, Sink, TypeName))
    {
        return true;
    }
    UFoliageType* Type = LoadFoliageType(TypeName);
    if (!Type)
    {
        AddError(TEXT("the foliage type fixture could not be loaded back from its own path"));
        return true;
    }

    TArray<TSharedPtr<FJsonValue>> Locations;
    Locations.Add(LocationEntry(ColX, ColY, RequestZ));

    TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
    Params->SetStringField(TEXT("foliageTypePath"), ObjectPathFor(TypeName));
    Params->SetArrayField(TEXT("locations"), Locations);
    Params->SetObjectField(TEXT("surface"), AnySolidSurface());

    bool bSuccess = false;
    FString ErrorCode;
    TSharedPtr<FJsonObject> Result;
    Dispatch(Dispatcher, Sink, TEXT("foliage.paint"), TEXT("req-foliage-paint-seat"),
        Params, bSuccess, Result, ErrorCode);

    TestTrue(*FString::Printf(TEXT("foliage.paint with a surface succeeds (error=%s)"), *ErrorCode),
        bSuccess);
    if (!bSuccess || !Result.IsValid())
    {
        return true;
    }

    bool bProjected = false;
    TestTrue(TEXT("the response carries the projected flag"),
        Result->TryGetBoolField(TEXT("projected"), bProjected));
    TestTrue(TEXT("the response reports that projection happened"), bProjected);
    TestEqual(TEXT("one instance was projected"),
        RequireNumberField(*this, Result, TEXT("projectedCount")), 1);
    TestEqual(TEXT("one instance was placed"),
        RequireNumberField(*this, Result, TEXT("instancesPlaced")), 1);
    TestEqual(TEXT("a well-formed single-entry batch skips nothing"),
        RequireNumberField(*this, Result, TEXT("skippedCount")), 0);

    // GROUND TRUTH #1: the foliage record.
    TArray<FFoliageInstance> Stored;
    const int32 StoredCount = GatherInstances(World, Type, Stored);
    TestEqual(TEXT("the foliage actor holds exactly the one painted instance"), StoredCount, 1);
    if (StoredCount != 1)
    {
        return true;
    }
    TestEqual(TEXT("the instance kept the requested X"), Stored[0].Location.X, ColX, PosTolerance);
    TestEqual(TEXT("the instance kept the requested Y"), Stored[0].Location.Y, ColY, PosTolerance);
    TestEqual(*FString::Printf(
            TEXT("the instance was seated onto the floor rather than left at the requested Z "
                 "(expected %.1f, requested %.1f, got %.3f)"),
            ExpectedSeatedZ, RequestZ, Stored[0].Location.Z),
        Stored[0].Location.Z, ExpectedSeatedZ, PosTolerance);

    // GROUND TRUTH #2: the render instance the viewer sees.
    FTransform ComponentWorld;
    if (TestTrue(TEXT("the type's instanced component carries the painted instance"),
            ReadComponentInstanceWorld(World, Type, 0, ComponentWorld)))
    {
        TestEqual(TEXT("the component instance moved with the foliage record"),
            ComponentWorld.GetTranslation().Z, ExpectedSeatedZ, PosTolerance);
    }

    // The response must publish the move it made, not just the fact that it made one: a caller
    // reading deltaZCm can tell a request that already sat on the surface from one that was
    // dropped 750 cm onto it.
    const TArray<TSharedPtr<FJsonValue>>* PlacedRows = nullptr;
    if (TestTrue(TEXT("the response carries a placed[] row per projected instance"),
            Result->TryGetArrayField(TEXT("placed"), PlacedRows) && PlacedRows
                && PlacedRows->Num() == 1))
    {
        const TSharedPtr<FJsonObject>* Row = nullptr;
        if ((*PlacedRows)[0].IsValid() && (*PlacedRows)[0]->TryGetObject(Row) && Row)
        {
            double DeltaZ = 0.0;
            TestTrue(TEXT("the placed[] row carries deltaZCm"),
                (*Row)->TryGetNumberField(TEXT("deltaZCm"), DeltaZ));
            TestEqual(TEXT("deltaZCm is the signed drop from the requested Z"),
                DeltaZ, ExpectedSeatedZ - RequestZ, PosTolerance);
        }
    }

    return true;
}

// ============================================================================
// (b) Without a surface, the response says it did not project.
// ============================================================================

// COUNTERFACTUAL. Before the fix the response carried success, foliageTypePath, instancesPlaced,
// foliageActorPath, foliageActorName and a hardcoded existsAfter - and nothing else. There was no
// `projected` key and no warnings[], so a caller could not tell an unprojected batch from a
// seated one. Both assertions below are absent-key failures against that response. The Z
// assertion is the other half: literal placement must still be literal, so a caller who did not
// ask for projection gets exactly the number they sent.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFoliagePaintUnprojectedBatchSaysSoTest,
    "PinWright.foliage.paint.UnprojectedBatchDisclosesThatItDidNotProject",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FFoliagePaintUnprojectedBatchSaysSoTest::RunTest(const FString& Parameters)
{
    using namespace FoliagePaintProjectionTestHelpers;

    UWorld* World = EditorWorld();
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no_editor_world"),
            TEXT("GEditor->GetEditorWorldContext().World() returned null; the foliage.paint "
                 "disclosure assertions were stepped over."));
        return true;
    }

    DispatcherTestHelpers::FSinkPtr Sink;
    FRpcDispatcher Dispatcher;
    MakeDispatcher(Sink, Dispatcher);

    const FString TypeName = UniqueName(TEXT("Literal"));
    ON_SCOPE_EXIT { DiscardType(Dispatcher, Sink, TypeName); };

    if (!CreateFoliageType(*this, Dispatcher, Sink, TypeName))
    {
        return true;
    }
    UFoliageType* Type = LoadFoliageType(TypeName);
    if (!Type)
    {
        AddError(TEXT("the foliage type fixture could not be loaded back from its own path"));
        return true;
    }

    TArray<TSharedPtr<FJsonValue>> Locations;
    Locations.Add(LocationEntry(ColX, ColY, RequestZ));

    TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
    Params->SetStringField(TEXT("foliageTypePath"), ObjectPathFor(TypeName));
    Params->SetArrayField(TEXT("locations"), Locations);
    // No `surface`, which is the whole point: this is the call shape that used to look identical
    // to a projected one.

    bool bSuccess = false;
    FString ErrorCode;
    TSharedPtr<FJsonObject> Result;
    Dispatch(Dispatcher, Sink, TEXT("foliage.paint"), TEXT("req-foliage-paint-literal"),
        Params, bSuccess, Result, ErrorCode);

    TestTrue(*FString::Printf(TEXT("foliage.paint without a surface still succeeds (error=%s)"),
        *ErrorCode), bSuccess);
    if (!bSuccess || !Result.IsValid())
    {
        return true;
    }

    bool bProjected = true;
    TestTrue(TEXT("the response carries the projected flag even when nothing was projected"),
        Result->TryGetBoolField(TEXT("projected"), bProjected));
    TestFalse(TEXT("the response states that no projection happened"), bProjected);

    const TArray<TSharedPtr<FJsonValue>>* Warnings = nullptr;
    if (TestTrue(TEXT("the response carries a non-empty warnings[] array"),
            Result->TryGetArrayField(TEXT("warnings"), Warnings) && Warnings
                && Warnings->Num() > 0))
    {
        // The warning has to name the remedy, not merely admit the omission: a caller who reads
        // it must learn which argument turns projection on.
        const FString Text = (*Warnings)[0].IsValid() ? (*Warnings)[0]->AsString() : FString();
        TestTrue(TEXT("the warning names the surface argument that would enable projection"),
            Text.Contains(TEXT("surface")));
    }

    // Literal placement must stay literal.
    TArray<FFoliageInstance> Stored;
    const int32 StoredCount = GatherInstances(World, Type, Stored);
    TestEqual(TEXT("the foliage actor holds exactly the one painted instance"), StoredCount, 1);
    if (StoredCount == 1)
    {
        TestEqual(TEXT("an unprojected instance sits at exactly the requested Z"),
            Stored[0].Location.Z, RequestZ, PosTolerance);
    }

    return true;
}

// ============================================================================
// (c) An entry with no coordinates is reported, not placed at the world origin.
// ============================================================================

// COUNTERFACTUAL. `{}` IS a JSON object, so the old parse loop admitted it, ran three
// TryGetNumberField calls that all failed, and kept the `double X = 0, Y = 0, Z = 0`
// initialisers - producing an instance at (0,0,0) that the caller never asked for, counted in
// instancesPlaced, with no skipped[] and no reason. All three assertions below are red against
// that: the count is 2 instead of 1, skippedCount is absent, and an instance sits at the origin.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFoliagePaintCoordinatelessEntryIsReportedTest,
    "PinWright.foliage.paint.LocationWithoutCoordinatesIsReportedNotPlacedAtTheOrigin",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FFoliagePaintCoordinatelessEntryIsReportedTest::RunTest(const FString& Parameters)
{
    using namespace FoliagePaintProjectionTestHelpers;

    UWorld* World = EditorWorld();
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no_editor_world"),
            TEXT("GEditor->GetEditorWorldContext().World() returned null; the foliage.paint "
                 "coordinate-less entry assertions were stepped over."));
        return true;
    }

    DispatcherTestHelpers::FSinkPtr Sink;
    FRpcDispatcher Dispatcher;
    MakeDispatcher(Sink, Dispatcher);

    const FString TypeName = UniqueName(TEXT("NoCoords"));
    ON_SCOPE_EXIT { DiscardType(Dispatcher, Sink, TypeName); };

    if (!CreateFoliageType(*this, Dispatcher, Sink, TypeName))
    {
        return true;
    }
    UFoliageType* Type = LoadFoliageType(TypeName);
    if (!Type)
    {
        AddError(TEXT("the foliage type fixture could not be loaded back from its own path"));
        return true;
    }

    // One coordinate-less object bracketed by nothing, plus one usable entry, so the response's
    // counts have to split rather than collapse.
    TArray<TSharedPtr<FJsonValue>> Locations;
    Locations.Add(MakeShared<FJsonValueObject>(MakeShared<FJsonObject>()));
    Locations.Add(LocationEntry(ColX, ColY, RequestZ));

    TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
    Params->SetStringField(TEXT("foliageTypePath"), ObjectPathFor(TypeName));
    Params->SetArrayField(TEXT("locations"), Locations);

    bool bSuccess = false;
    FString ErrorCode;
    TSharedPtr<FJsonObject> Result;
    Dispatch(Dispatcher, Sink, TEXT("foliage.paint"), TEXT("req-foliage-paint-no-coords"),
        Params, bSuccess, Result, ErrorCode);

    TestTrue(*FString::Printf(TEXT("foliage.paint on a partly-usable batch succeeds (error=%s)"),
        *ErrorCode), bSuccess);
    if (!bSuccess || !Result.IsValid())
    {
        return true;
    }

    TestEqual(TEXT("only the usable entry was placed"),
        RequireNumberField(*this, Result, TEXT("instancesPlaced")), 1);
    TestEqual(TEXT("the coordinate-less entry is counted as skipped"),
        RequireNumberField(*this, Result, TEXT("skippedCount")), 1);

    const TArray<TSharedPtr<FJsonValue>>* SkippedRows = nullptr;
    if (TestTrue(TEXT("the response carries the skipped[] detail array"),
            Result->TryGetArrayField(TEXT("skipped"), SkippedRows) && SkippedRows
                && SkippedRows->Num() == 1))
    {
        const TSharedPtr<FJsonObject>* Row = nullptr;
        if ((*SkippedRows)[0].IsValid() && (*SkippedRows)[0]->TryGetObject(Row) && Row)
        {
            double RowIndex = -1.0;
            (*Row)->TryGetNumberField(TEXT("index"), RowIndex);
            TestEqual(TEXT("the skipped[] row names the caller's own entry index"), RowIndex, 0.0);

            FString Reason;
            (*Row)->TryGetStringField(TEXT("reason"), Reason);
            TestTrue(TEXT("the skipped[] reason names the missing coordinates"),
                Reason.Contains(TEXT("x")) && Reason.Contains(TEXT("y"))
                    && Reason.Contains(TEXT("z")));
        }
    }

    // GROUND TRUTH: one instance, and it is the one that carried coordinates.
    TArray<FFoliageInstance> Stored;
    const int32 StoredCount = GatherInstances(World, Type, Stored);
    TestEqual(TEXT("the foliage actor holds only the usable entry's instance"), StoredCount, 1);
    for (const FFoliageInstance& Inst : Stored)
    {
        TestFalse(TEXT("no instance was placed at the world origin"),
            Inst.Location.IsNearlyZero(1.0));
    }

    return true;
}

// ============================================================================
// (d) A projected instance publishes the same ground provenance the spatial verbs do.
// ============================================================================

// WHAT WAS WRONG. `paint` seated each instance through GroundPlacement::SeatInstance - the very
// solve spatial.ground_instances runs - and then published only where the instance ended up.
// WHICH surface answered, the fact the whole `surface` argument exists to control, was measured
// on every probe and dropped before serialization, because the writer for that block was
// file-local to GroundPlacementHandler.cpp. So a batch seated onto a neighbouring HISM scatter
// and a batch seated onto terrain produced byte-identical responses: same `projected`, same
// `placedCount`, same plausible `deltaZCm`. That is the same shape of silent-wrong-output the
// projection itself was added to close, one level down.
//
// WHAT THE FIX DOES. The writer moved to GroundPlacement::MakeProvenanceJson and `paint` calls
// it per projected instance, so the block in placed[] is not merely similar to the ground
// verbs' - it is the same function's output.
//
// THE TEST IS AN EQUIVALENCE, not a shape check, because a second hand-written copy of the
// block would pass a shape check while being free to drift. One floor answers two probes: a
// painted foliage instance seated on it, and an ordinary prop verified on it 120 cm away. Both
// probes resolve against the SAME primitive, so every provenance field they publish about that
// primitive must agree, field for field. A `paint` that grew its own writer, named the foliage's
// own component instead of the surface, or omitted the block entirely fails here.
//
// Red before the fix: placed[] rows carried {index, x, y, z, deltaZCm} and no groundProvenance
// at all, so the first assertion fails outright.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFoliagePaintPublishesGroundProvenanceTest,
    "PinWright.foliage.paint.ProjectionPublishesTheSameGroundProvenanceAsTheSpatialVerbs",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FFoliagePaintPublishesGroundProvenanceTest::RunTest(const FString& Parameters)
{
    using namespace FoliagePaintProjectionTestHelpers;

    UWorld* World = EditorWorld();
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no_editor_world"),
            TEXT("GEditor->GetEditorWorldContext().World() returned null; the paint "
                 "ground-provenance assertions were stepped over."));
        return true;
    }

    DispatcherTestHelpers::FSinkPtr Sink;
    FRpcDispatcher Dispatcher;
    MakeDispatcher(Sink, Dispatcher);

    const FString TypeName = UniqueName(TEXT("Prov"));
    const FString FloorLabel = UniqueName(TEXT("ProvFloor"));
    const FString ProbeLabel = UniqueName(TEXT("ProvProbe"));

    // The floor spans 400 x 400 around the column, so a prop 120 cm to the +X side is still well
    // inside it while sitting clear of the 100 cm foliage instance at the centre. Two probes, one
    // surface, no interference.
    constexpr double ProbeOffsetX = 120.0;

    AActor* Floor = SpawnFloor(*this, Dispatcher, Sink, World, FloorLabel);
    AActor* Probe = nullptr;
    ON_SCOPE_EXIT
    {
        DiscardType(Dispatcher, Sink, TypeName);
        if (Probe)
        {
            Probe->Destroy();
        }
        if (Floor)
        {
            Floor->Destroy();
        }
    };
    if (!Floor)
    {
        AddError(TEXT("the floor fixture did not spawn, so there was no shared surface to probe"));
        return true;
    }

    // The comparison prop, resting exactly on the same floor. Spawned through the real actor.spawn
    // verb, like the floor, so nothing here is a hand-built actor the production path never sees.
    {
        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("meshPath"), CubeMeshPath);
        Params->SetStringField(TEXT("actorName"), ProbeLabel);
        Params->SetObjectField(TEXT("location"),
            Vec3(ColX + ProbeOffsetX, ColY, FloorTopZ + CubeHalf));

        bool bSpawned = false;
        FString SpawnError;
        DispatcherTestHelpers::Dispatch(Dispatcher, Sink, TEXT("actor.spawn"),
            TEXT("req-foliage-provenance-probe"), Params, bSpawned, SpawnError);
        TestTrue(*FString::Printf(TEXT("the comparison prop spawned (error=%s)"), *SpawnError),
            bSpawned);
    }
    Probe = McpActorUtils::FindActorByName(World, ProbeLabel);
    if (!Probe)
    {
        AddError(TEXT("the comparison prop did not spawn, so there was nothing to compare against"));
        return true;
    }
    FlushPhysics(World);

    if (!CreateFoliageType(*this, Dispatcher, Sink, TypeName))
    {
        return true;
    }

    // ---- Probe 1: foliage.paint, projecting onto the floor ----

    TArray<TSharedPtr<FJsonValue>> Locations;
    Locations.Add(LocationEntry(ColX, ColY, RequestZ));

    TSharedPtr<FJsonObject> PaintParams = MakeShared<FJsonObject>();
    PaintParams->SetStringField(TEXT("foliageTypePath"), ObjectPathFor(TypeName));
    PaintParams->SetArrayField(TEXT("locations"), Locations);
    PaintParams->SetObjectField(TEXT("surface"), AnySolidSurface());

    bool bPaintSuccess = false;
    FString PaintError;
    TSharedPtr<FJsonObject> PaintResult;
    Dispatch(Dispatcher, Sink, TEXT("foliage.paint"), TEXT("req-foliage-paint-provenance"),
        PaintParams, bPaintSuccess, PaintResult, PaintError);
    TestTrue(*FString::Printf(TEXT("foliage.paint with a surface succeeds (error=%s)"), *PaintError),
        bPaintSuccess);
    if (!bPaintSuccess || !PaintResult.IsValid())
    {
        return true;
    }

    const TArray<TSharedPtr<FJsonValue>>* PlacedRows = nullptr;
    if (!PaintResult->TryGetArrayField(TEXT("placed"), PlacedRows) || !PlacedRows
        || PlacedRows->Num() != 1)
    {
        AddError(TEXT("foliage.paint returned no placed[] row for the projected instance"));
        return true;
    }
    const TSharedPtr<FJsonObject> PlacedRow =
        (*PlacedRows)[0].IsValid() ? (*PlacedRows)[0]->AsObject() : nullptr;

    // THE REGRESSION. The instance was seated against a stated surface and the response did not
    // say which surface that was.
    const TSharedPtr<FJsonObject>* PaintProvenance = nullptr;
    if (!PlacedRow.IsValid()
        || !PlacedRow->TryGetObjectField(TEXT("groundProvenance"), PaintProvenance)
        || !PaintProvenance)
    {
        AddError(TEXT("the placed[] row does not say WHAT the instance was seated on "
                      "(groundProvenance missing)"));
        return true;
    }

    // ---- Probe 2: spatial.verify_grounding, over the same floor ----

    TSharedPtr<FJsonObject> VerifyParams = MakeShared<FJsonObject>();
    TArray<TSharedPtr<FJsonValue>> ProbeNames;
    ProbeNames.Add(MakeShared<FJsonValueString>(ProbeLabel));
    VerifyParams->SetArrayField(TEXT("actors"), ProbeNames);
    VerifyParams->SetObjectField(TEXT("surface"), AnySolidSurface());
    VerifyParams->SetStringField(TEXT("detail"), TEXT("all"));

    bool bVerifySuccess = false;
    FString VerifyError;
    TSharedPtr<FJsonObject> VerifyResult;
    Dispatch(Dispatcher, Sink, TEXT("spatial.verify_grounding"),
        TEXT("req-foliage-provenance-verify"), VerifyParams, bVerifySuccess, VerifyResult,
        VerifyError);
    TestTrue(*FString::Printf(TEXT("spatial.verify_grounding succeeds (error=%s)"), *VerifyError),
        bVerifySuccess);
    if (!bVerifySuccess || !VerifyResult.IsValid())
    {
        return true;
    }

    const TArray<TSharedPtr<FJsonValue>>* VerifyRows = nullptr;
    if (!VerifyResult->TryGetArrayField(TEXT("results"), VerifyRows) || !VerifyRows
        || VerifyRows->Num() == 0)
    {
        AddError(TEXT("spatial.verify_grounding returned no per-actor results row"));
        return true;
    }
    const TSharedPtr<FJsonObject> VerifyRow =
        (*VerifyRows)[0].IsValid() ? (*VerifyRows)[0]->AsObject() : nullptr;
    const TSharedPtr<FJsonObject>* VerifyContact = nullptr;
    const TSharedPtr<FJsonObject>* VerifyProvenance = nullptr;
    if (!VerifyRow.IsValid() || !VerifyRow->TryGetObjectField(TEXT("contact"), VerifyContact)
        || !VerifyContact
        || !(*VerifyContact)->TryGetObjectField(TEXT("groundProvenance"), VerifyProvenance)
        || !VerifyProvenance)
    {
        AddError(TEXT("spatial.verify_grounding published no groundProvenance for the same floor"));
        return true;
    }

    // ---- The two probes must agree about the primitive they both hit ----

    auto NamedSurface = [](const TSharedPtr<FJsonObject>& Provenance,
                           FString& OutActor, FString& OutComponent, FString& OutClass) -> bool
    {
        const TArray<TSharedPtr<FJsonValue>>* Rows = nullptr;
        if (!Provenance.IsValid() || !Provenance->TryGetArrayField(TEXT("surfaceComponents"), Rows)
            || !Rows || Rows->Num() == 0)
        {
            return false;
        }
        const TSharedPtr<FJsonObject> Row = (*Rows)[0].IsValid() ? (*Rows)[0]->AsObject() : nullptr;
        if (!Row.IsValid())
        {
            return false;
        }
        Row->TryGetStringField(TEXT("actor"), OutActor);
        Row->TryGetStringField(TEXT("component"), OutComponent);
        Row->TryGetStringField(TEXT("componentClass"), OutClass);
        return true;
    };

    FString PaintActor, PaintComponent, PaintClass;
    FString VerifyActor, VerifyComponent, VerifyClass;
    const bool bPaintNamed = NamedSurface(*PaintProvenance, PaintActor, PaintComponent, PaintClass);
    const bool bVerifyNamed =
        NamedSurface(*VerifyProvenance, VerifyActor, VerifyComponent, VerifyClass);
    TestTrue(TEXT("paint names the primitive that answered its probe"), bPaintNamed);
    TestTrue(TEXT("verify_grounding names the primitive that answered its probe"), bVerifyNamed);
    if (!bPaintNamed || !bVerifyNamed)
    {
        return true;
    }

    TestEqual(TEXT("both verbs name the same surface actor"), PaintActor, VerifyActor);
    TestEqual(TEXT("both verbs name the same surface component"), PaintComponent, VerifyComponent);
    TestEqual(TEXT("both verbs name the same surface component class"), PaintClass, VerifyClass);

    // ...and it is the FLOOR, not the foliage's own component. A verb that reported the primitive
    // it had just written into would agree with itself and describe nothing.
    TestEqual(TEXT("the named surface is the floor actor"), PaintActor, FloorLabel);
    TestEqual(TEXT("the named surface is a static mesh component"), PaintClass,
        FString(TEXT("StaticMeshComponent")));
    TestFalse(TEXT("the foliage's own instanced component is not reported as its ground"),
        PaintClass.Contains(TEXT("Instanced")) || PaintClass.Contains(TEXT("Foliage")));

    // The rest of the block has to agree too, or the two verbs are describing one probe with two
    // vocabularies - which is exactly what a second copy of the writer would produce.
    bool bPaintTraceComplex = true;
    bool bVerifyTraceComplex = false;
    TestTrue(TEXT("paint publishes the probe's traceComplex setting"),
        (*PaintProvenance)->TryGetBoolField(TEXT("traceComplex"), bPaintTraceComplex));
    TestTrue(TEXT("verify_grounding publishes the probe's traceComplex setting"),
        (*VerifyProvenance)->TryGetBoolField(TEXT("traceComplex"), bVerifyTraceComplex));
    TestTrue(TEXT("both probes ran at the same traceComplex"),
        bPaintTraceComplex == bVerifyTraceComplex);

    double PaintCount = 0.0;
    double VerifyCount = 0.0;
    (*PaintProvenance)->TryGetNumberField(TEXT("surfaceComponentCount"), PaintCount);
    (*VerifyProvenance)->TryGetNumberField(TEXT("surfaceComponentCount"), VerifyCount);
    TestEqual(TEXT("one surface answered paint's probe"), PaintCount, 1.0);
    TestEqual(TEXT("one surface answered verify_grounding's probe"), VerifyCount, 1.0);

    return true;
}
