// Copyright (c) 2026 Alexander Penkin. MIT License.

// TestFoliageClusterTreeRebuild.cpp - regression coverage for
// B-foliage-adds-never-rebuild-the-hism-tree.
//
// WHAT WAS WRONG. `foliage.add_instances` wrote instances the level never drew. Every
// `Info->AddInstance` runs inside `FFoliageInfo::AddInstancesImpl`'s
// `Implementation->BeginUpdate()` / `EndUpdate()` bracket
// (Runtime/Foliage/Private/InstancedFoliage.cpp:2306-2326), and `FFoliageStaticMesh::BeginUpdate`
// clears `Component->bAutoRebuildTreeOnInstanceChanges` (:1355-1371) for exactly the window in
// which every instance is added. `HISM::AddInstance` sets `bIsOutOfDate` and then rebuilds only
// behind that flag (Runtime/Engine/Private/HierarchicalInstancedStaticMesh.cpp:2468-2499), and
// `EndUpdate` restores the flag WITHOUT rebuilding (:1373-1388). A HISM draws from its cluster
// tree, not from `PerInstanceSMData`, so a fresh scatter left the editor with a full instance
// array, `NumBuiltInstances == 0`, and an empty viewport. `foliage.paint` without `surface` was in
// the same state; paint WITH `surface` only escaped it as a side effect of `PostMoveInstances`
// reaching `HISM::UpdateInstanceTransform`'s own `BuildTreeIfOutdated`, which is an accident of an
// unrelated engine gate rather than a rebuild anyone asked for.
//
// WHY THE EXISTING FOLIAGE SUITE COULD NOT CATCH IT, WHICH IS HALF THE TICKET.
// `TestFoliagePlacementBehaviour.cpp`'s `GatherRenderedInstances` - added by an earlier fix
// specifically so the tests would stop trusting the ledger - reads
// `Component->GetInstanceCount()`, which is `PerInstanceSMData.Num()`
// (Runtime/Engine/Private/InstancedStaticMesh.cpp:4844-4847). That is a SECOND ARRAY on the
// component, not the tree. `FFoliageInfo::Instances` and `PerInstanceSMData` are written in
// lockstep by a single `AddInstance`, so every count assertion in that file - and the
// `ledgerMatchesRendered` response field, which compares exactly those two - is satisfied by a
// scatter that draws nothing. **This file therefore asserts `NumBuiltInstances`
// (HierarchicalInstancedStaticMeshComponent.h:156-158, "the number of instances in the
// ClusterTree") and `IsTreeFullyBuilt()`, and NOTHING ELSE would go red on the defect.** Each test
// below also asserts the array-level agreement explicitly, marked as the check that passed
// throughout - a reader comparing the two blocks can see which layer the coverage was missing.
//
// WHAT THE FIX DOES. Both write verbs close their add loop with
// `PinWrightFoliageClusterTree::RebuildFoliageClusterTreeAfterAdd`, which is
// `FFoliageInfo::Refresh(Async=false, Force=true)` - the engine's own entry point, and the exact
// call `RemoveInstancesImpl` already makes for itself at `InstancedFoliage.cpp:2507`. Synchronous
// rather than the engine's in-file `Async=true` because the same response publishes the measured
// `builtInstanceCount`, and an async build would return with the tree still out of date. Both
// verbs and `foliage.get_instances` then publish `builtInstanceCount` / `clusterTreeUpToDate`,
// which are OMITTED rather than zeroed when the scope has no cluster tree to read.
//
// COUNTERFACTUALS, one per assertion group:
//   (a) Reverting the `Refresh` in `add_instances` leaves `NumBuiltInstances` at 0 with 14
//       instances stored, and `builtInstanceCount` absent from the response.
//   (b) Reverting it in `paint` leaves the tree at whatever `add_instances` built, so the painted
//       instances are stored and undrawn - the unprojected branch is the one that never had any
//       rebuild at all.
//   (c) Passing `Async=true` instead returns before the append case's build completes, which the
//       `clusterTreeUpToDate` assertion catches on the second write.
//   (d) Publishing `builtInstanceCount` from `GetInstanceCount()` rather than `NumBuiltInstances`
//       makes it a third name for the array, and the live-component assertions above it stay red
//       while the response assertions go green - which is what separates a real fix from a
//       renamed one.
//
// FIXTURES. One `UFoliageType` per test, built through the real `foliage.add_type` verb under a
// GUID-suffixed name so its instance store starts empty by construction, torn down type-scoped
// (never `removeAll`, which would wipe the host map's own foliage) plus `CleanupTestAsset` so the
// dirty `/Game` package is not flushed into host Content by a later save-all. The only content
// dependency is the engine cube the rest of the foliage suite already uses. Nothing here traces
// against world geometry - both verbs are driven on their literal-placement branch on purpose,
// because that is the branch with no incidental rebuild in it.
//
// Requests route through the real production dispatcher (FRpcDispatcher::ProcessRequest -> the
// registered handler), the same entry the HTTP gateway uses, which also validates each payload
// against the declared ParamSpec.
#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Dispatch/RpcDispatcher.h"
#include "Tests/Infra/DispatcherTestHelpers.h"
#include "Tests/TestSkipReporting.h"
#include "Tests/TestUtils.h"

#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "Editor.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "FoliageType.h"
#include "InstancedFoliage.h"
#include "InstancedFoliageActor.h"
#include "Misc/Guid.h"
#include "Misc/ScopeExit.h"

// Named (not anonymous) namespace, and every helper carries the `Ctr` prefix: the plugin's tests
// share one module with Unity builds enabled, where same-named anonymous-namespace helpers - and
// same-named inline functions in different named namespaces reached through a file-scope
// using-directive - collide across merged translation units.
namespace FoliageClusterTreeRebuildTestHelpers
{
    constexpr const TCHAR* CtrCubeMeshPath = TEXT("/Engine/BasicShapes/Cube.Cube");

    // An isolated column, away from the ones the other foliage and spatial suites use. Nothing
    // here traces, but keeping the fixtures out of real level content keeps a failure readable in
    // the viewport.
    constexpr double CtrColX = 610400.0;
    constexpr double CtrColY = 488900.0;
    constexpr double CtrColZ = 33000.0;

    inline FString CtrUniqueName(const TCHAR* Prefix)
    {
        return FString::Printf(TEXT("PWFoliageTree_%s_%s"), Prefix,
            *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    }

    inline FString CtrPackagePathFor(const FString& Name)
    {
        return FString::Printf(TEXT("/Game/Foliage/%s"), *Name);
    }

    inline FString CtrObjectPathFor(const FString& Name)
    {
        return FString::Printf(TEXT("/Game/Foliage/%s.%s"), *Name, *Name);
    }

    inline UWorld* CtrEditorWorld()
    {
        return GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    }

    inline TSharedPtr<FJsonObject> CtrVec3(double X, double Y, double Z)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetNumberField(TEXT("x"), X);
        Obj->SetNumberField(TEXT("y"), Y);
        Obj->SetNumberField(TEXT("z"), Z);
        return Obj;
    }

    inline bool CtrCreateFoliageType(FAutomationTestBase& Test, FRpcDispatcher& Dispatcher,
        DispatcherTestHelpers::FSinkPtr& Sink, const FString& Name)
    {
        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("name"), Name);
        Params->SetStringField(TEXT("meshPath"), CtrCubeMeshPath);
        Params->SetNumberField(TEXT("density"), 100.0);
        Params->SetNumberField(TEXT("minScale"), 1.0);
        Params->SetNumberField(TEXT("maxScale"), 1.0);
        Params->SetBoolField(TEXT("alignToNormal"), false);
        Params->SetBoolField(TEXT("randomYaw"), false);

        bool bSuccess = false;
        FString ErrorCode;
        DispatcherTestHelpers::Dispatch(Dispatcher, Sink, TEXT("foliage.add_type"),
            TEXT("req-foliage-cluster-tree-add-type"), Params, bSuccess, ErrorCode);
        Test.TestTrue(*FString::Printf(TEXT("foliage.add_type created the '%s' fixture (error=%s)"),
            *Name, *ErrorCode), bSuccess);
        return bSuccess;
    }

    inline UFoliageType* CtrLoadFoliageType(const FString& Name)
    {
        return LoadObject<UFoliageType>(nullptr, *CtrObjectPathFor(Name));
    }

    inline void CtrDiscardType(FRpcDispatcher& Dispatcher, DispatcherTestHelpers::FSinkPtr& Sink,
        const FString& Name)
    {
        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("foliageTypePath"), CtrObjectPathFor(Name));

        bool bSuccess = false;
        FString ErrorCode;
        DispatcherTestHelpers::Dispatch(Dispatcher, Sink, TEXT("foliage.remove"),
            TEXT("req-foliage-cluster-tree-cleanup"), Params, bSuccess, ErrorCode);

        CleanupTestAsset(CtrPackagePathFor(Name));
    }

    // The four numbers a foliage type's HISMs can answer, gathered LIVE off the components rather
    // than off any handler response. `Built` is the only one the defect moves.
    struct FCtrTreeReading
    {
        bool bFoundComponent = false;
        // PerInstanceSMData.Num() - the array every pre-existing count assertion reads.
        int32 ComponentInstances = 0;
        // NumBuiltInstances - the instances in the built cluster tree, which is what draws.
        int32 Built = 0;
        bool bAllTreesUpToDate = true;
        // The record. Kept alongside so a test can state which layers agreed.
        int32 LedgerInstances = 0;
    };

    inline FCtrTreeReading CtrReadTreeState(UWorld* World, const UFoliageType* Type)
    {
        FCtrTreeReading Reading;
        if (!World || !Type)
        {
            return Reading;
        }
        for (TActorIterator<AInstancedFoliageActor> It(World); It; ++It)
        {
            AInstancedFoliageActor* Ifa = *It;
            FFoliageInfo* Info = Ifa ? Ifa->FindInfo(Type) : nullptr;
            if (!Info)
            {
                continue;
            }
            Reading.LedgerInstances += Info->Instances.Num();
            if (const UHierarchicalInstancedStaticMeshComponent* Component = Info->GetComponent())
            {
                Reading.bFoundComponent = true;
                Reading.ComponentInstances += Component->GetInstanceCount();
                Reading.Built += Component->NumBuiltInstances;
                Reading.bAllTreesUpToDate &= Component->IsTreeFullyBuilt();
            }
        }
        return Reading;
    }

    // THE ASSERTION THE DEFECT MOVES, and the only one that does. Kept in one place so the two
    // tests cannot drift, and so a reader can see at a glance that it never touches
    // GetInstanceCount().
    inline void CtrRequireClusterTreeHolds(FAutomationTestBase& Test, UWorld* World,
        const UFoliageType* Type, int32 Expected, const TCHAR* What)
    {
        const FCtrTreeReading Reading = CtrReadTreeState(World, Type);

        Test.TestTrue(*FString::Printf(TEXT("%s (a foliage component exists to read a tree off)"),
            What), Reading.bFoundComponent);
        Test.TestEqual(
            *FString::Printf(TEXT("%s (NumBuiltInstances - the instances in the CLUSTER TREE)"),
                What),
            Reading.Built, Expected);
        Test.TestTrue(
            *FString::Printf(TEXT("%s (IsTreeFullyBuilt - the tree is not left marked out of date)"),
                What),
            Reading.bAllTreesUpToDate);

        // Stated, not skipped: these two agreed throughout the defect, which is exactly why the
        // pre-existing suite stayed green while the level drew nothing. They are here to document
        // the blind spot, not to detect it.
        Test.TestEqual(
            *FString::Printf(TEXT("%s (the array layer, which agreed even on the defect)"), What),
            Reading.ComponentInstances, Expected);
        Test.TestEqual(
            *FString::Printf(TEXT("%s (the ledger layer, which agreed even on the defect)"), What),
            Reading.LedgerInstances, Expected);
    }

    // Reads an integer response field, failing when the key is ABSENT so an omitted field can
    // never read as a zero that satisfies a comparison.
    inline int32 CtrRequireNumberField(FAutomationTestBase& Test,
        const TSharedPtr<FJsonObject>& Result, const TCHAR* Field)
    {
        double Value = 0.0;
        const bool bPresent = Result.IsValid() && Result->TryGetNumberField(Field, Value);
        Test.TestTrue(*FString::Printf(TEXT("response carries %s"), Field), bPresent);
        return bPresent ? static_cast<int32>(Value) : -1;
    }

    inline void CtrRequireTrueBoolField(FAutomationTestBase& Test,
        const TSharedPtr<FJsonObject>& Result, const TCHAR* Field)
    {
        bool bValue = false;
        const bool bPresent = Result.IsValid() && Result->TryGetBoolField(Field, bValue);
        Test.TestTrue(*FString::Printf(TEXT("response carries %s"), Field), bPresent);
        Test.TestTrue(*FString::Printf(TEXT("%s is true"), Field), bPresent && bValue);
    }

    // The one host condition under which a rebuild legitimately cannot happen:
    // `BuildTreeIfOutdated` returns early while the static mesh is still compiling
    // (HierarchicalInstancedStaticMesh.cpp:2782-2786). Reported through the shared skip marker so
    // the run is classified COMPLETED_WITH_SKIPS rather than quietly green.
    inline bool CtrFixtureMeshIsCompiling()
    {
        const UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, CtrCubeMeshPath);
        return Mesh && Mesh->IsCompiling();
    }
}

using DispatcherTestHelpers::MakeDispatcher;
using DispatcherTestHelpers::Dispatch;

// Each RunTest opens FoliageClusterTreeRebuildTestHelpers inside its own body rather than at file
// scope: a file-scope using-directive would leak into every other test .cpp that Unity merges
// after this one into the same translation unit.

// ============================================================================
// foliage.add_instances - the scatter reaches the cluster tree, not just the array
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFoliageAddInstancesReachClusterTreeTest,
    "PinWright.foliage.add_instances.AddedInstancesReachTheClusterTree",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FFoliageAddInstancesReachClusterTreeTest::RunTest(const FString& Parameters)
{
    using namespace FoliageClusterTreeRebuildTestHelpers;

    UWorld* World = CtrEditorWorld();
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no_editor_world"),
            TEXT("GEditor->GetEditorWorldContext().World() returned null; the cluster-tree "
                 "assertions for foliage.add_instances were stepped over."));
        return true;
    }
    if (CtrFixtureMeshIsCompiling())
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("fixture_static_mesh_compiling"),
            TEXT("the engine cube fixture is still compiling, and BuildTreeIfOutdated returns "
                 "early for a compiling mesh, so a tree that is not built proves nothing here."));
        return true;
    }

    DispatcherTestHelpers::FSinkPtr Sink;
    FRpcDispatcher Dispatcher;
    MakeDispatcher(Sink, Dispatcher);

    const FString TypeName = CtrUniqueName(TEXT("AddTree"));
    ON_SCOPE_EXIT { CtrDiscardType(Dispatcher, Sink, TypeName); };

    if (!CtrCreateFoliageType(*this, Dispatcher, Sink, TypeName))
    {
        return true;
    }
    UFoliageType* Type = CtrLoadFoliageType(TypeName);
    if (!Type)
    {
        AddError(TEXT("the foliage type fixture could not be loaded back from its own path"));
        return true;
    }

    // Fourteen, which is the count the ticket was measured on. A batch rather than a single
    // instance on purpose: the whole batch shares ONE suppressed BeginUpdate/EndUpdate window, so
    // a fix that rebuilt per instance and a fix that rebuilds once are both correct here while a
    // fix that rebuilds never is not.
    constexpr int32 FirstBatch = 14;
    TArray<TSharedPtr<FJsonValue>> Transforms;
    for (int32 Index = 0; Index < FirstBatch; ++Index)
    {
        TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetObjectField(TEXT("location"),
            CtrVec3(CtrColX + Index * 300.0, CtrColY, CtrColZ));
        Transforms.Add(MakeShared<FJsonValueObject>(Entry));
    }

    TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
    Params->SetStringField(TEXT("foliageTypePath"), CtrObjectPathFor(TypeName));
    Params->SetArrayField(TEXT("transforms"), Transforms);

    bool bSuccess = false;
    FString ErrorCode;
    TSharedPtr<FJsonObject> Result;
    Dispatch(Dispatcher, Sink, TEXT("foliage.add_instances"),
        TEXT("req-foliage-cluster-tree-add"), Params, bSuccess, Result, ErrorCode);

    TestTrue(*FString::Printf(TEXT("foliage.add_instances succeeds (error=%s)"), *ErrorCode),
        bSuccess);
    if (!bSuccess || !Result.IsValid())
    {
        return true;
    }

    // THE REGRESSION. Before the fix this is 0 built against 14 stored.
    CtrRequireClusterTreeHolds(*this, World, Type, FirstBatch,
        TEXT("a fresh scatter is in the cluster tree"));

    TestEqual(TEXT("instances_count agrees with what was asked for"),
        CtrRequireNumberField(*this, Result, TEXT("instances_count")), FirstBatch);
    // The MEASURED field, which is absent entirely before the fix rather than reporting a wrong
    // number - so a caller could not have been told the truth by reading harder.
    TestEqual(TEXT("builtInstanceCount reports the tree the verb rebuilt"),
        CtrRequireNumberField(*this, Result, TEXT("builtInstanceCount")), FirstBatch);
    CtrRequireTrueBoolField(*this, Result, TEXT("clusterTreeUpToDate"));
    TestFalse(TEXT("no clusterTreeWarning on a scatter whose tree was rebuilt"),
        Result->HasField(TEXT("clusterTreeWarning")));

    // A SECOND batch onto the SAME component, which is the case the engine does NOT force
    // synchronous: `bForceSync` in BuildTreeIfOutdated (:2812) holds only while
    // NumBuiltInstances == 0, so an async Refresh would return here with the tree still short.
    constexpr int32 SecondBatch = 5;
    TArray<TSharedPtr<FJsonValue>> MoreTransforms;
    for (int32 Index = 0; Index < SecondBatch; ++Index)
    {
        TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetObjectField(TEXT("location"),
            CtrVec3(CtrColX + Index * 300.0, CtrColY + 900.0, CtrColZ));
        MoreTransforms.Add(MakeShared<FJsonValueObject>(Entry));
    }

    TSharedPtr<FJsonObject> MoreParams = MakeShared<FJsonObject>();
    MoreParams->SetStringField(TEXT("foliageTypePath"), CtrObjectPathFor(TypeName));
    MoreParams->SetArrayField(TEXT("transforms"), MoreTransforms);

    TSharedPtr<FJsonObject> MoreResult;
    Dispatch(Dispatcher, Sink, TEXT("foliage.add_instances"),
        TEXT("req-foliage-cluster-tree-add-again"), MoreParams, bSuccess, MoreResult, ErrorCode);
    TestTrue(*FString::Printf(TEXT("the second foliage.add_instances succeeds (error=%s)"),
        *ErrorCode), bSuccess);
    if (!bSuccess || !MoreResult.IsValid())
    {
        return true;
    }

    CtrRequireClusterTreeHolds(*this, World, Type, FirstBatch + SecondBatch,
        TEXT("an append reaches the cluster tree too"));
    TestEqual(TEXT("builtInstanceCount covers the whole type after an append"),
        CtrRequireNumberField(*this, MoreResult, TEXT("builtInstanceCount")),
        FirstBatch + SecondBatch);
    CtrRequireTrueBoolField(*this, MoreResult, TEXT("clusterTreeUpToDate"));

    // And the read verb publishes the same measured layer, beside the field that cannot fail.
    TSharedPtr<FJsonObject> ReadParams = MakeShared<FJsonObject>();
    ReadParams->SetStringField(TEXT("foliageTypePath"), CtrObjectPathFor(TypeName));
    TSharedPtr<FJsonObject> ReadResult;
    Dispatch(Dispatcher, Sink, TEXT("foliage.get_instances"),
        TEXT("req-foliage-cluster-tree-read"), ReadParams, bSuccess, ReadResult, ErrorCode);
    TestTrue(*FString::Printf(TEXT("foliage.get_instances succeeds (error=%s)"), *ErrorCode),
        bSuccess);
    if (bSuccess && ReadResult.IsValid())
    {
        TestEqual(TEXT("get_instances publishes the cluster-tree count"),
            CtrRequireNumberField(*this, ReadResult, TEXT("builtInstanceCount")),
            FirstBatch + SecondBatch);
        CtrRequireTrueBoolField(*this, ReadResult, TEXT("clusterTreeUpToDate"));
        // Asserted so the relationship between the two layers is pinned rather than assumed: the
        // pre-existing verdict field agrees here, and it also agreed on the defect. It is the new
        // field beside it that carries the information.
        CtrRequireTrueBoolField(*this, ReadResult, TEXT("ledgerMatchesRendered"));
        TestEqual(TEXT("renderedInstanceCount is the array layer, unchanged in meaning"),
            CtrRequireNumberField(*this, ReadResult, TEXT("renderedInstanceCount")),
            FirstBatch + SecondBatch);
    }

    return true;
}

// ============================================================================
// foliage.paint without `surface` - the branch that never had an incidental rebuild
// ============================================================================

// COUNTERFACTUAL. This is the half of the ticket that was source-only. Paint's projecting branch
// rebuilds by accident, through PostMoveInstances -> SetInstanceWorldTransform ->
// HISM::UpdateInstanceTransform -> BuildTreeIfOutdated (:2362), reachable only because the editor
// never takes that function's in-place branch (bAllowInPlaceUpdateForRotationOrScaleChange =
// bIsGameWorld, :2331-2335). The unprojected `else` branch calls none of it. So this test drives
// paint with NO `surface`, which is also the branch every pre-existing paint test uses.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFoliageUnprojectedPaintReachesClusterTreeTest,
    "PinWright.foliage.paint.UnprojectedPaintReachesTheClusterTree",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FFoliageUnprojectedPaintReachesClusterTreeTest::RunTest(const FString& Parameters)
{
    using namespace FoliageClusterTreeRebuildTestHelpers;

    UWorld* World = CtrEditorWorld();
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no_editor_world"),
            TEXT("GEditor->GetEditorWorldContext().World() returned null; the cluster-tree "
                 "assertions for foliage.paint were stepped over."));
        return true;
    }
    if (CtrFixtureMeshIsCompiling())
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("fixture_static_mesh_compiling"),
            TEXT("the engine cube fixture is still compiling, and BuildTreeIfOutdated returns "
                 "early for a compiling mesh, so a tree that is not built proves nothing here."));
        return true;
    }

    DispatcherTestHelpers::FSinkPtr Sink;
    FRpcDispatcher Dispatcher;
    MakeDispatcher(Sink, Dispatcher);

    const FString TypeName = CtrUniqueName(TEXT("PaintTree"));
    ON_SCOPE_EXIT { CtrDiscardType(Dispatcher, Sink, TypeName); };

    if (!CtrCreateFoliageType(*this, Dispatcher, Sink, TypeName))
    {
        return true;
    }
    UFoliageType* Type = CtrLoadFoliageType(TypeName);
    if (!Type)
    {
        AddError(TEXT("the foliage type fixture could not be loaded back from its own path"));
        return true;
    }

    constexpr int32 PaintCount = 6;
    TArray<TSharedPtr<FJsonValue>> Locations;
    for (int32 Index = 0; Index < PaintCount; ++Index)
    {
        Locations.Add(MakeShared<FJsonValueObject>(
            CtrVec3(CtrColX + Index * 300.0, CtrColY + 2400.0, CtrColZ)));
    }

    TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
    Params->SetStringField(TEXT("foliageTypePath"), CtrObjectPathFor(TypeName));
    Params->SetArrayField(TEXT("locations"), Locations);
    // No `surface`, so no projection - and therefore no PostMoveInstances to rebuild the tree as
    // a side effect. This is the branch under test.

    bool bSuccess = false;
    FString ErrorCode;
    TSharedPtr<FJsonObject> Result;
    Dispatch(Dispatcher, Sink, TEXT("foliage.paint"),
        TEXT("req-foliage-cluster-tree-paint"), Params, bSuccess, Result, ErrorCode);

    TestTrue(*FString::Printf(TEXT("foliage.paint succeeds without a surface (error=%s)"),
        *ErrorCode), bSuccess);
    if (!bSuccess || !Result.IsValid())
    {
        return true;
    }

    bool bProjected = true;
    TestTrue(TEXT("the response confirms this was the UNPROJECTED branch"),
        Result->TryGetBoolField(TEXT("projected"), bProjected) && !bProjected);

    // THE REGRESSION. Before the fix the unprojected branch leaves NumBuiltInstances at 0.
    CtrRequireClusterTreeHolds(*this, World, Type, PaintCount,
        TEXT("an unprojected paint is in the cluster tree"));

    TestEqual(TEXT("instancesPlaced agrees with what was asked for"),
        CtrRequireNumberField(*this, Result, TEXT("instancesPlaced")), PaintCount);
    TestEqual(TEXT("builtInstanceCount reports the tree paint rebuilt"),
        CtrRequireNumberField(*this, Result, TEXT("builtInstanceCount")), PaintCount);
    CtrRequireTrueBoolField(*this, Result, TEXT("clusterTreeUpToDate"));
    TestFalse(TEXT("no clusterTreeWarning on a paint whose tree was rebuilt"),
        Result->HasField(TEXT("clusterTreeWarning")));

    return true;
}
