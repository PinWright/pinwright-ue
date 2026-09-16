// Copyright (c) 2026 Alexander Penkin. MIT License.

// TestFoliageProceduralSpawnCount.cpp - regression coverage for
// B-create-procedural-spawned-count-always-zero.
//
// WHAT WAS WRONG. foliage.create_procedural reported its scatter through `instances_spawned`,
// computed as a before/after delta of FFoliageInfo::GetPlacedInstanceCount() summed over every
// AInstancedFoliageActor in the world. That engine function counts instances whose
// ProceduralGuid is NOT valid (InstancedFoliage.cpp) - "placed" means placed BY HAND, the exact
// complement of what a procedural simulation produces. Every instance this verb creates carries
// a valid guid, so both terms of the delta were blind to the whole scatter, the difference was
// zero for any correct run, and FMath::Max(0, ...) clamped away the only other reachable value.
// The verb placed instances and reported 0, on every input, with no branch that avoided it.
//
// WHAT THE FIX DOES. The count is measured AFTER the resimulation by matching
// FFoliageInstance::ProceduralGuid against the spawned volume component's own
// UProceduralFoliageComponent::GetProceduralGuid() - the same match
// AInstancedFoliageActor::ContainsInstancesFromProceduralFoliageComponent makes, which is what
// the engine's editor UI decides its "Unable to spawn instances" toast on. The hand-painted
// total (what the old counter was actually measuring, and which this verb produces none of) is
// published separately as `hand_placed_instances_in_world`, so one number can never be read as
// the other. Both are omitted rather than reported as 0 when there is nothing to measure.
//
// WHY THIS TEST IS SHAPED THE WAY IT IS. The pre-existing
// PinWright.foliage.create_procedural.ReportsInstancesSpawned asserts only that the field is
// PRESENT, so it passed on this defect and would keep passing. The assertion that catches it is
// differential: run a scatter that really places instances, then compare the reported number
// against a count re-read from FFoliageInfo::Instances - never from the response. Before the fix
// that comparison is N vs 0.
//
// FIXTURES. The tile simulation is terrain-free and its results are projected onto world
// geometry afterwards (FEdModeFoliage::AddInstances -> AInstancedFoliageActor::FoliageTrace, a
// WorldStatic sweep), so a volume over empty space scatters nothing however correct the counter
// is. This test therefore spawns a real static-mesh floor filling the volume's footprint, in an
// isolated far column, following Tests/Environment/TestFoliagePaintGroundProjection.cpp -
// including its world-tick flush, without which a just-spawned body is not yet in the
// scene-query structure and the simulation's own traces would miss the floor. A scatter that
// still places nothing FAILS here rather than skipping: a zero ground truth would make the
// comparison vacuous, which is precisely the hole this ticket was filed about.
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
#include "Tests/TestWorldUtils.h"

#include "Editor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "FoliageType.h"
#include "InstancedFoliage.h"
#include "InstancedFoliageActor.h"
#include "Misc/Guid.h"
#include "Misc/ScopeExit.h"
#include "ProceduralFoliageComponent.h"
#include "ProceduralFoliageVolume.h"
#include "UObject/Package.h"

// Named (not anonymous) namespace: the plugin's tests share one module with Unity builds
// enabled, where same-named anonymous-namespace helpers collide across merged translation units.
namespace FoliageProceduralSpawnCountHelpers
{
    // The engine unit cube, 100 x 100 x 100 with a centred pivot, used for both the floor and
    // the scattered species so the fixture needs no project content.
    constexpr const TCHAR* CubeMeshPath = TEXT("/Engine/BasicShapes/Cube.Cube");
    constexpr double CubeHalf = 50.0;

    // An isolated column, distinct from the ones TestFoliagePaintGroundProjection.cpp,
    // TestFoliagePlacementBehaviour.cpp, TestGroundPlacement.cpp and TestPlacementHandlers.cpp
    // use, and lifted well clear of any landscape a host map might carry so the only thing under
    // the volume is this test's own floor.
    constexpr double ColX = 512300.0;
    constexpr double ColY = 407900.0;
    constexpr double FloorTopZ = 26000.0;

    // The volume: 20 m square, straddling the floor's top face so the hit points the simulation
    // projects onto sit INSIDE the brush (FoliageTrace rejects a hit outside the procedural
    // volume's body instance). tileSize is set to this same 2000 so a single tile covers the
    // whole footprint and no seed is discarded for landing outside it.
    constexpr double VolumeEdge = 2000.0;
    constexpr double VolumeHeight = 600.0;
    constexpr double VolumeCentreZ = FloorTopZ + 100.0;

    // The floor is scaled to 40 x 40 cubes = 40 m square, double the volume's footprint on each
    // axis, so coverage can never be the reason a seed fails to find ground. Scaling XY only
    // leaves the Z half-extent at 50, keeping the top face exactly at FloorTopZ.
    constexpr double FloorScaleXY = 40.0;

    inline UWorld* EditorWorld()
    {
        return GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    }

    inline FString UniqueName(const TCHAR* Prefix)
    {
        return FString::Printf(TEXT("PWProcCount_%s_%s"), Prefix,
            *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    }

    inline TSharedPtr<FJsonObject> Vec3(double X, double Y, double Z)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetNumberField(TEXT("x"), X);
        Obj->SetNumberField(TEXT("y"), Y);
        Obj->SetNumberField(TEXT("z"), Z);
        return Obj;
    }

    // Editor worlds do not tick physics on their own, so a just-spawned body is absent from the
    // scene-query structure until a tick flushes it, and the simulation's own downward sweeps
    // would miss this test's floor. Editor worlds do not simulate, so nothing moves.
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

    // The ground, spawned through the real actor.spawn verb rather than built by hand (as
    // TestFoliagePaintGroundProjection.cpp does), so it is a normal non-transient level actor
    // with real WorldStatic collision - which is what FoliageTrace's
    // SweepMultiByObjectType(ECC_WorldStatic) needs to see.
    inline bool SpawnFloor(FAutomationTestBase& Test, FRpcDispatcher& Dispatcher,
        DispatcherTestHelpers::FSinkPtr& Sink, const FString& Label)
    {
        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("meshPath"), CubeMeshPath);
        Params->SetStringField(TEXT("actorName"), Label);
        Params->SetObjectField(TEXT("location"), Vec3(ColX, ColY, FloorTopZ - CubeHalf));
        Params->SetObjectField(TEXT("scale"), Vec3(FloorScaleXY, FloorScaleXY, 1.0));

        bool bSuccess = false;
        FString ErrorCode;
        DispatcherTestHelpers::Dispatch(Dispatcher, Sink, TEXT("actor.spawn"),
            TEXT("req-proc-count-floor"), Params, bSuccess, ErrorCode);
        Test.TestTrue(*FString::Printf(TEXT("the floor fixture spawned (error=%s)"), *ErrorCode),
            bSuccess);
        return bSuccess;
    }

    // Resolve the volume the verb spawned so its component's ProceduralGuid - the thing the
    // ground-truth count is keyed on - can be read from the WORLD. Only the actor LABEL comes
    // from the response (the handler echoes GetActorLabel() verbatim as `volume_actor`, so this
    // is a reliable handle); the guid, and every instance count derived from it, are read off
    // live objects.
    inline UProceduralFoliageComponent* FindSpawnedProceduralComponent(
        UWorld* World, const FString& VolumeLabel)
    {
        if (!World)
        {
            return nullptr;
        }
        for (TActorIterator<AProceduralFoliageVolume> It(World); It; ++It)
        {
            AProceduralFoliageVolume* Volume = *It;
            if (Volume && Volume->GetActorLabel() == VolumeLabel)
            {
                return Volume->ProceduralComponent;
            }
        }
        return nullptr;
    }

    // GROUND TRUTH. Walks FFoliageInfo::Instances across every AInstancedFoliageActor in the
    // world and splits them on FFoliageInstance::ProceduralGuid: instances stamped with this
    // scatter's guid, and hand-painted instances (an invalid guid). Deliberately reimplemented
    // here rather than shared with the handler - a test that called the handler's own helper
    // would agree with it by construction and could not have caught this defect.
    inline void CountInstancesByGuid(UWorld* World, const FGuid& ProceduralGuid,
        int32& OutFromThisScatter, int32& OutHandPlaced)
    {
        OutFromThisScatter = 0;
        OutHandPlaced = 0;
        if (!World || !ProceduralGuid.IsValid())
        {
            return;
        }
        for (TActorIterator<AInstancedFoliageActor> It(World); It; ++It)
        {
            AInstancedFoliageActor* Ifa = *It;
            if (!Ifa)
            {
                continue;
            }
            Ifa->ForEachFoliageInfo([&](UFoliageType*, FFoliageInfo& Info)
            {
                for (const FFoliageInstance& Instance : Info.Instances)
                {
                    if (Instance.ProceduralGuid == ProceduralGuid)
                    {
                        ++OutFromThisScatter;
                    }
                    else if (!Instance.ProceduralGuid.IsValid())
                    {
                        ++OutHandPlaced;
                    }
                }
                return true;
            });
        }
    }

    // Type-scoped teardown for the auto-created species: empties only this fixture's instances
    // (a world-wide removal would wipe the host map's own foliage) and then deletes the asset,
    // which also clears the dirty /Game package a later editor-wide save-all would otherwise
    // flush into host Content. The handler names its assets <name>_Spawner and
    // <name>_Spawner_FT_<n> under /Game/ProceduralFoliage.
    inline void DiscardGeneratedType(FRpcDispatcher& Dispatcher,
        DispatcherTestHelpers::FSinkPtr& Sink, const FString& PackagePath, const FString& AssetName)
    {
        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("foliageTypePath"),
            FString::Printf(TEXT("%s.%s"), *PackagePath, *AssetName));

        bool bSuccess = false;
        FString ErrorCode;
        DispatcherTestHelpers::Dispatch(Dispatcher, Sink, TEXT("foliage.remove"),
            TEXT("req-proc-count-cleanup"), Params, bSuccess, ErrorCode);

        CleanupTestAsset(PackagePath);
        // Force-delete can re-dirty the emptied package; clear the flag so nothing dirty
        // survives for a later editor-wide save-all to write. B-tests-leak-host-content.
        if (UPackage* Remaining = FindPackage(nullptr, *PackagePath))
        {
            Remaining->SetDirtyFlag(false);
        }
    }
}

// ============================================================================
// foliage.create_procedural - instances_spawned counts what the scatter placed
// ============================================================================

// COUNTERFACTUAL. Before the fix `instances_spawned` is the before/after delta of
// GetPlacedInstanceCount(), which counts only instances with an INVALID ProceduralGuid. Every
// instance this scatter places carries a valid one, so the delta is 0 while the ground-truth
// walk below finds N > 0, and the equality assertion fails N vs 0. A fix that swapped the
// accumulator to the raw Instances.Num() delta would pass this test but attribute any
// concurrent foliage change to this call; the guid match is what makes the number attributable.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFoliageProceduralSpawnCountMatchesFoliageActorsTest,
    "PinWright.foliage.create_procedural.SpawnedCountMatchesTheFoliageActors",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FFoliageProceduralSpawnCountMatchesFoliageActorsTest::RunTest(const FString& Parameters)
{
    using namespace FoliageProceduralSpawnCountHelpers;

    UWorld* World = EditorWorld();
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no_editor_world"),
            TEXT("GEditor->GetEditorWorldContext().World() returned null; the "
                 "foliage.create_procedural placed-count assertions were stepped over."));
        return true;
    }

    // Destroys the floor, the procedural volume and any AInstancedFoliageActor the bake created,
    // and restores the persistent level's dirty flag, so the open map is left as found.
    FScopedEditorWorldActorGuard WorldGuard;

    DispatcherTestHelpers::FSinkPtr Sink;
    FRpcDispatcher Dispatcher;
    DispatcherTestHelpers::MakeDispatcher(Sink, Dispatcher);

    const FString VolumeName = UniqueName(TEXT("Vol"));
    const FString FloorLabel = UniqueName(TEXT("Floor"));
    const FString SpawnerPackage = FString::Printf(
        TEXT("/Game/ProceduralFoliage/%s_Spawner"), *VolumeName);
    const FString TypePackage = FString::Printf(
        TEXT("/Game/ProceduralFoliage/%s_Spawner_FT_0"), *VolumeName);

    ON_SCOPE_EXIT
    {
        DiscardGeneratedType(Dispatcher, Sink, TypePackage,
            FString::Printf(TEXT("%s_Spawner_FT_0"), *VolumeName));
        CleanupTestAsset(SpawnerPackage);
        if (UPackage* Remaining = FindPackage(nullptr, *SpawnerPackage))
        {
            Remaining->SetDirtyFlag(false);
        }
    };

    if (!SpawnFloor(*this, Dispatcher, Sink, FloorLabel))
    {
        AddError(TEXT("the floor fixture did not spawn, so the scatter would have had no "
                      "surface to project onto and the count could not be exercised"));
        return true;
    }
    FlushPhysics(World);

    TSharedPtr<FJsonObject> Bounds = MakeShared<FJsonObject>();
    Bounds->SetObjectField(TEXT("location"), Vec3(ColX, ColY, VolumeCentreZ));
    Bounds->SetObjectField(TEXT("size"), Vec3(VolumeEdge, VolumeEdge, VolumeHeight));

    TSharedPtr<FJsonObject> Type = MakeShared<FJsonObject>();
    Type->SetStringField(TEXT("meshPath"), CubeMeshPath);
    Type->SetNumberField(TEXT("density"), 50.0);
    TArray<TSharedPtr<FJsonValue>> Types;
    Types.Add(MakeShared<FJsonValueObject>(Type));

    TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
    Params->SetStringField(TEXT("name"), VolumeName);
    Params->SetObjectField(TEXT("bounds"), Bounds);
    Params->SetArrayField(TEXT("foliageTypes"), Types);
    Params->SetNumberField(TEXT("seed"), 4242.0);
    // One tile the size of the volume: every seed the simulation casts lands inside the brush,
    // so the scatter is as dense as the fixture can make it and a zero result cannot be blamed
    // on seeds falling outside the volume.
    Params->SetNumberField(TEXT("tileSize"), VolumeEdge);
    Params->SetNumberField(TEXT("numUniqueTiles"), 1.0);

    bool bSuccess = false;
    FString ErrorCode;
    TSharedPtr<FJsonObject> Result;
    DispatcherTestHelpers::Dispatch(Dispatcher, Sink, TEXT("foliage.create_procedural"),
        TEXT("req-proc-count-create"), Params, bSuccess, Result, ErrorCode);

    TestTrue(*FString::Printf(TEXT("foliage.create_procedural succeeds (error=%s)"), *ErrorCode),
        bSuccess);
    if (!bSuccess || !Result.IsValid())
    {
        return true;
    }

    FString VolumeLabel;
    TestTrue(TEXT("the response names the spawned volume actor"),
        Result->TryGetStringField(TEXT("volume_actor"), VolumeLabel));

    UProceduralFoliageComponent* ProcComp = FindSpawnedProceduralComponent(World, VolumeLabel);
    TestNotNull(TEXT("the spawned volume carries a procedural component"), ProcComp);
    if (!ProcComp)
    {
        return true;
    }

    const FGuid ProceduralGuid = ProcComp->GetProceduralGuid();
    TestTrue(TEXT("the procedural component carries a valid ProceduralGuid"),
        ProceduralGuid.IsValid());
    if (!ProceduralGuid.IsValid())
    {
        return true;
    }

    int32 GroundTruthSpawned = 0;
    int32 GroundTruthHandPlaced = 0;
    CountInstancesByGuid(World, ProceduralGuid, GroundTruthSpawned, GroundTruthHandPlaced);

    // The fixture's own precondition, asserted rather than skipped. A scatter that placed
    // nothing would make the comparison below vacuously true on the defect, which is exactly how
    // the old `instances_spawned` survived three separate readings as evidence.
    TestTrue(*FString::Printf(
        TEXT("the fixture actually scattered: %d instances in the world carry this volume's "
             "ProceduralGuid"), GroundTruthSpawned),
        GroundTruthSpawned > 0);

    // THE LOAD-BEARING ASSERTION. Reported count vs a count re-read from FFoliageInfo::Instances.
    // Before the fix this reads 0 against a positive ground truth.
    double ReportedSpawned = -1.0;
    TestTrue(TEXT("the response carries instances_spawned"),
        Result->TryGetNumberField(TEXT("instances_spawned"), ReportedSpawned));
    TestEqual(TEXT("instances_spawned equals the instances the foliage actors actually hold for "
                   "this scatter"),
        static_cast<int32>(ReportedSpawned), GroundTruthSpawned);

    // The sibling number, published under a name that says it is the OTHER set. This is what the
    // old counter measured; a fix that merely renamed the field would fail here, because the
    // hand-placed total must not move with the scatter.
    double ReportedHandPlaced = -1.0;
    TestTrue(TEXT("the response carries hand_placed_instances_in_world"),
        Result->TryGetNumberField(TEXT("hand_placed_instances_in_world"), ReportedHandPlaced));
    TestEqual(TEXT("hand_placed_instances_in_world equals the world's non-procedural instances"),
        static_cast<int32>(ReportedHandPlaced), GroundTruthHandPlaced);

    return true;
}
