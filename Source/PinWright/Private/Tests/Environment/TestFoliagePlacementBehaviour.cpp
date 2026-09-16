// Copyright (c) 2026 Alexander Penkin. MIT License.

// TestFoliagePlacementBehaviour.cpp - behavioural coverage for the foliage.* verbs
// (F-foliage-namespace-has-no-behavioural-tests).
//
// WHAT WAS MISSING. Before this file the namespace had 14 test cases and not one of them
// asserted where a verb put anything: six were `ValidParamsNoCrash` registration smoke, six
// asserted the response envelope (which error came back, which field was echoed), and the two
// that drove real placement asserted a scale/rotation round trip and the presence of a count
// field. `foliage.paint` and `foliage.remove` had no assertion about world state at all.
//
// EVERY ASSERTION HERE IS RE-READ FROM THE ENGINE, on BOTH of the two representations foliage
// keeps. Ground truth is gathered by iterating every AInstancedFoliageActor in the editor world -
// never the handler's own response, and never the `foliageActorPath` the response names (a
// handler that reported the wrong actor would then be checked against the actor it named). The
// response is read only to cross-check its counts AGAINST that state, which is the assertion
// that catches a verb reporting N placed while N-1 landed. Same reason
// Tests/Spatial/TestGroundPlacement.cpp re-reads every transform off the component: a
// response-trusting test passes against a silent no-op.
//
// WHY "BOTH REPRESENTATIONS" IS NOT PEDANTRY. `FFoliageInfo::Instances` is an editor-side
// bookkeeping array; what the level DRAWS lives on the info's instanced component, and the two
// are written by different engine calls. The first version of this file asserted only the array
// and called it "what the level actually holds", which it is not:
// `B-foliage-remove-empties-ledger-not-component` was a foliage.remove that emptied the array,
// left every component full, and passed every test here. Counts now go through
// RequireInstanceCount, which pins the record and the rendered instances to the same number, so a
// verb that writes one side and not the other fails no matter which side it skipped.
//
// WHAT IS DELIBERATELY NOT ASSERTED, AND WHY. Three properties of these verbs are open board
// defects whose fixes are in flight. Asserting today's behaviour would certify the gap, so each
// is named here instead:
//
//   * `foliage.paint`'s Z. The verb writes the literal requested Z with no trace and no normal
//     alignment (`B-foliage-paint-does-no-ground-projection`, OPEN/High). When that lands, a
//     painted instance's Z becomes the surface under it. So this file asserts the XY and the
//     scale of a painted instance - both survive a downward projection - and says nothing about
//     Z or rotation. That ticket carries its own regression test for the projection itself.
//
//   * `foliage.paint`'s location-less entry. An `{}` entry in `locations[]` IS a JSON object, so
//     the parser accepts it and its three missing coordinates default to a placement at the
//     world origin. That is the second silent-input defect in the same ticket. Test 2 below
//     therefore uses NON-OBJECT junk entries only, which the parser genuinely skips today, and
//     asserts they were not placed; `{}` is left to the ticket that owns it.
//
//   * `foliage.create_procedural` is not covered here at all. Its per-type `minScale`/`maxScale`/
//     `alignToNormal` are dropped on the floor and the fix rewrites exactly the three lines that
//     build each UFoliageType (`B-create-procedural-ignores-scale-and-normal-fields`, OPEN); it
//     is also the one foliage verb that already has a test driving real placement
//     (`create_procedural.ReportsInstancesSpawned`). Adding assertions to a handler mid-fix would
//     either pin the defect or land red.
//
// Also uncovered on purpose: undo. No mutating foliage verb opens an FScopedTransaction
// (`B-foliage-mutators-no-transaction`, OPEN), so an undo assertion could only fail today.
//
// FIXTURES. Each test builds its own UFoliageType through the real `foliage.add_type` verb under
// a GUID-suffixed name, so its instance store starts empty by construction and absolute counts
// are meaningful, and tears it down on scope exit (type-scoped `foliage.remove` - never
// `removeAll`, which would wipe the host map's own foliage - then CleanupTestAsset, which also
// clears the dirty /Game package a later editor-wide save-all would otherwise flush into host
// Content, per `B-tests-leak-host-content`). The only content dependency is the engine's
// BasicShapes/Cube, which the two pre-existing working foliage tests already use.
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
#include "EditorAssetLibrary.h"
#include "Editor.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "FoliageType.h"
#include "FoliageType_InstancedStaticMesh.h"
#include "InstancedFoliage.h"
#include "InstancedFoliageActor.h"
#include "Misc/Guid.h"
#include "Misc/ScopeExit.h"

// Named (not anonymous) namespace: the plugin's tests share one module with Unity builds
// enabled, where same-named anonymous-namespace helpers collide across merged translation units.
namespace FoliagePlacementTestHelpers
{
    // The only content fixture. A missing engine cube is a hard test failure, not a skip.
    constexpr const TCHAR* CubeMeshPath = TEXT("/Engine/BasicShapes/Cube.Cube");

    // An isolated column of the editor world, away from the columns TestGroundPlacement.cpp and
    // TestPlacementHandlers.cpp use. Nothing here traces against world geometry, but keeping the
    // fixtures out of any real level content keeps a failure readable in the viewport.
    constexpr double ColX = 412300.0;
    constexpr double ColY = 358700.0;
    constexpr double ColZ = 21000.0;

    // Instance positions are written as exact doubles by every verb under test, so the tolerance
    // only has to absorb the JSON double round trip.
    constexpr double PosTolerance = 0.01;

    // A value no response field can legitimately carry, so a MISSING field can never satisfy a
    // comparison the way a 0.0 fallback would.
    inline double AbsentNumber()
    {
        return TNumericLimits<double>::Lowest();
    }

    // foliage.add_type builds package "/Game/Foliage/<Name>" and names the object inside it
    // "<Name>", so the loadable object path is "/Game/Foliage/<Name>.<Name>" while asset deletion
    // takes the package path.
    inline FString UniqueTypeName(const TCHAR* Prefix)
    {
        return FString::Printf(TEXT("PWFoliage_%s_%s"), Prefix,
            *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    }

    inline FString PackagePathFor(const FString& Name)
    {
        return FString::Printf(TEXT("/Game/Foliage/%s"), *Name);
    }

    inline FString ObjectPathFor(const FString& Name)
    {
        return FString::Printf(TEXT("/Game/Foliage/%s.%s"), *Name, *Name);
    }

    inline UWorld* EditorWorld()
    {
        return GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    }

    inline TSharedPtr<FJsonObject> Vec3(double X, double Y, double Z)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetNumberField(TEXT("x"), X);
        Obj->SetNumberField(TEXT("y"), Y);
        Obj->SetNumberField(TEXT("z"), Z);
        return Obj;
    }

    // One bare {x,y,z} entry, the shape both foliage.paint's `locations[]` and
    // foliage.add_instances' legacy `locations[]` take.
    inline TSharedPtr<FJsonValue> LocationEntry(double X, double Y, double Z)
    {
        return MakeShared<FJsonValueObject>(Vec3(X, Y, Z));
    }

    // Creates a foliage type through the real verb. Every argument is passed explicitly so each
    // test states the type's placement properties rather than inheriting a default that a later
    // handler change could move under it.
    inline bool CreateFoliageType(FAutomationTestBase& Test, FRpcDispatcher& Dispatcher,
        DispatcherTestHelpers::FSinkPtr& Sink, const FString& Name, double Density,
        double MinScale, double MaxScale, bool bAlignToNormal, bool bRandomYaw)
    {
        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("name"), Name);
        Params->SetStringField(TEXT("meshPath"), CubeMeshPath);
        Params->SetNumberField(TEXT("density"), Density);
        Params->SetNumberField(TEXT("minScale"), MinScale);
        Params->SetNumberField(TEXT("maxScale"), MaxScale);
        Params->SetBoolField(TEXT("alignToNormal"), bAlignToNormal);
        Params->SetBoolField(TEXT("randomYaw"), bRandomYaw);

        bool bSuccess = false;
        FString ErrorCode;
        DispatcherTestHelpers::Dispatch(Dispatcher, Sink, TEXT("foliage.add_type"),
            TEXT("req-foliage-behaviour-add-type"), Params, bSuccess, ErrorCode);
        Test.TestTrue(*FString::Printf(TEXT("foliage.add_type created the '%s' fixture (error=%s)"),
            *Name, *ErrorCode), bSuccess);
        return bSuccess;
    }

    inline UFoliageType* LoadFoliageType(const FString& Name)
    {
        return LoadObject<UFoliageType>(nullptr, *ObjectPathFor(Name));
    }

    // GROUND TRUTH #1 - the foliage RECORD. FFoliageInfo::Instances for this type across every
    // AInstancedFoliageActor in the world - not off any handler response, and not off the actor a
    // response named.
    //
    // NOT, on its own, "what the level holds". This is the editor-side bookkeeping array; the
    // instances that are DRAWN live on the info's component, and the two are written by separate
    // calls. B-foliage-remove-empties-ledger-not-component is exactly that split: foliage.remove
    // emptied this array, left every component full, and the whole file stayed green. So this is
    // used for the transforms only - which the component cannot answer as an FFoliageInstance -
    // and every COUNT is asserted through RequireInstanceCount below, which pins both sides.
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

    // GROUND TRUTH #2 - the RENDERED instances. What the level actually draws for this type: the
    // instance count on each foliage actor's instanced component for it, which is the quantity
    // FFoliageInfo::CheckValid holds equal to Instances.Num() (and only asserts under
    // DO_FOLIAGE_CHECK, which ships at 0 - so nothing enforces the invariant at runtime). Zero for
    // a type whose component was never built: FFoliageStaticMesh creates its HISM lazily inside
    // the first AddInstance, so a fresh type legitimately has none.
    inline int32 GatherRenderedInstances(UWorld* World, const UFoliageType* Type)
    {
        if (!World || !Type)
        {
            return 0;
        }
        int32 Rendered = 0;
        for (TActorIterator<AInstancedFoliageActor> It(World); It; ++It)
        {
            AInstancedFoliageActor* Ifa = *It;
            FFoliageInfo* Info = Ifa ? Ifa->FindInfo(Type) : nullptr;
            if (const UHierarchicalInstancedStaticMeshComponent* Component =
                    Info ? Info->GetComponent() : nullptr)
            {
                Rendered += Component->GetInstanceCount();
            }
        }
        return Rendered;
    }

    // THE DIFFERENTIAL ASSERTION, and the only count assertion this file makes. Pins BOTH
    // representations to the same expected number and leaves the record's instances in
    // OutInstances for the position/rotation/scale assertions only it can answer. A verb that
    // wrote - or cleared - one side and not the other satisfies either half alone, so neither
    // half by itself says anything about the level.
    inline void RequireInstanceCount(FAutomationTestBase& Test, UWorld* World,
        const UFoliageType* Type, TArray<FFoliageInstance>& OutInstances, int32 Expected,
        const TCHAR* What)
    {
        Test.TestEqual(*FString::Printf(TEXT("%s (foliage record)"), What),
            GatherInstances(World, Type, OutInstances), Expected);
        Test.TestEqual(*FString::Printf(TEXT("%s (rendered instances)"), What),
            GatherRenderedInstances(World, Type), Expected);
    }

    // Type-scoped teardown: empties only this fixture's instances (a `removeAll` would wipe the
    // host map's own foliage) and then deletes the asset, which also clears the dirty /Game
    // package that a later editor-wide save-all would flush into host Content.
    inline void DiscardType(FRpcDispatcher& Dispatcher, DispatcherTestHelpers::FSinkPtr& Sink,
        const FString& Name)
    {
        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("foliageTypePath"), ObjectPathFor(Name));

        bool bSuccess = false;
        FString ErrorCode;
        DispatcherTestHelpers::Dispatch(Dispatcher, Sink, TEXT("foliage.remove"),
            TEXT("req-foliage-behaviour-cleanup"), Params, bSuccess, ErrorCode);

        CleanupTestAsset(PackagePathFor(Name));
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

    inline int32 CountInstancesNearXY(const TArray<FFoliageInstance>& Instances, double X, double Y,
        double Tolerance = PosTolerance)
    {
        int32 Count = 0;
        for (const FFoliageInstance& Inst : Instances)
        {
            if (FMath::Abs(Inst.Location.X - X) <= Tolerance
                && FMath::Abs(Inst.Location.Y - Y) <= Tolerance)
            {
                ++Count;
            }
        }
        return Count;
    }

    inline const FFoliageInstance* FindInstanceNearXY(const TArray<FFoliageInstance>& Instances,
        double X, double Y, double Tolerance = PosTolerance)
    {
        for (const FFoliageInstance& Inst : Instances)
        {
            if (FMath::Abs(Inst.Location.X - X) <= Tolerance
                && FMath::Abs(Inst.Location.Y - Y) <= Tolerance)
            {
                return &Inst;
            }
        }
        return nullptr;
    }

    // The verbs place at exact coordinates, so an instance sitting at the world origin is one
    // that was built from missing input rather than from a caller's number.
    inline bool AnyInstanceAtWorldOrigin(const TArray<FFoliageInstance>& Instances)
    {
        for (const FFoliageInstance& Inst : Instances)
        {
            if (Inst.Location.IsNearlyZero(1.0))
            {
                return true;
            }
        }
        return false;
    }

    // foliage.get_instances emits flat per-instance keys (x/y/z, pitch/yaw/roll, scaleX/Y/Z).
    inline double RowNumber(const TSharedPtr<FJsonObject>& Row, const TCHAR* Field)
    {
        double Value = 0.0;
        return (Row.IsValid() && Row->TryGetNumberField(Field, Value)) ? Value : AbsentNumber();
    }

    inline TSharedPtr<FJsonObject> FindRowNearXY(const TArray<TSharedPtr<FJsonValue>>* Rows,
        double X, double Y, double Tolerance = PosTolerance)
    {
        if (!Rows)
        {
            return nullptr;
        }
        for (const TSharedPtr<FJsonValue>& Value : *Rows)
        {
            const TSharedPtr<FJsonObject>* Row = nullptr;
            if (Value.IsValid() && Value->TryGetObject(Row) && Row)
            {
                if (FMath::Abs(RowNumber(*Row, TEXT("x")) - X) <= Tolerance
                    && FMath::Abs(RowNumber(*Row, TEXT("y")) - Y) <= Tolerance)
                {
                    return *Row;
                }
            }
        }
        return nullptr;
    }

    inline int32 CountRowsForType(const TArray<TSharedPtr<FJsonValue>>* Rows, const FString& TypePath)
    {
        int32 Count = 0;
        if (!Rows)
        {
            return 0;
        }
        for (const TSharedPtr<FJsonValue>& Value : *Rows)
        {
            const TSharedPtr<FJsonObject>* Row = nullptr;
            FString RowType;
            if (Value.IsValid() && Value->TryGetObject(Row) && Row
                && (*Row)->TryGetStringField(TEXT("foliageType"), RowType) && RowType == TypePath)
            {
                ++Count;
            }
        }
        return Count;
    }

    // Asserts one get_instances row against the instance the level actually holds. Nine numbers,
    // because the historical defect in this reader dropped scale in both branches and rotation in
    // the unfiltered one (E-foliage-get-instances-drops-scale) - a row that carries only a
    // position still looks like a valid answer.
    inline void TestRowMatchesInstance(FAutomationTestBase& Test, const TCHAR* Label,
        const TSharedPtr<FJsonObject>& Row, const FFoliageInstance& Inst)
    {
        Test.TestEqual(*FString::Printf(TEXT("%s x matches the stored instance"), Label),
            RowNumber(Row, TEXT("x")), Inst.Location.X, PosTolerance);
        Test.TestEqual(*FString::Printf(TEXT("%s y matches the stored instance"), Label),
            RowNumber(Row, TEXT("y")), Inst.Location.Y, PosTolerance);
        Test.TestEqual(*FString::Printf(TEXT("%s z matches the stored instance"), Label),
            RowNumber(Row, TEXT("z")), Inst.Location.Z, PosTolerance);
        Test.TestEqual(*FString::Printf(TEXT("%s pitch matches the stored instance"), Label),
            RowNumber(Row, TEXT("pitch")), Inst.Rotation.Pitch, 0.01);
        Test.TestEqual(*FString::Printf(TEXT("%s yaw matches the stored instance"), Label),
            RowNumber(Row, TEXT("yaw")), Inst.Rotation.Yaw, 0.01);
        Test.TestEqual(*FString::Printf(TEXT("%s roll matches the stored instance"), Label),
            RowNumber(Row, TEXT("roll")), Inst.Rotation.Roll, 0.01);
        Test.TestEqual(*FString::Printf(TEXT("%s scaleX matches the stored instance"), Label),
            RowNumber(Row, TEXT("scaleX")), static_cast<double>(Inst.DrawScale3D.X), 0.001);
        Test.TestEqual(*FString::Printf(TEXT("%s scaleY matches the stored instance"), Label),
            RowNumber(Row, TEXT("scaleY")), static_cast<double>(Inst.DrawScale3D.Y), 0.001);
        Test.TestEqual(*FString::Printf(TEXT("%s scaleZ matches the stored instance"), Label),
            RowNumber(Row, TEXT("scaleZ")), static_cast<double>(Inst.DrawScale3D.Z), 0.001);
    }
}

using DispatcherTestHelpers::MakeDispatcher;
using DispatcherTestHelpers::Dispatch;

// Each RunTest opens FoliagePlacementTestHelpers inside its own body rather than at file scope:
// a file-scope using-directive would leak into every other test .cpp that Unity merges after
// this one into the same translation unit.

// ============================================================================
// foliage.paint - the verb had no world-state assertion of any kind
// ============================================================================

// COUNTERFACTUALS. (a) The placement loop increments its placed counter unconditionally, even on
// the branch where AddFoliageType is followed by a FindInfo that returns null and nothing is
// stored - a store that gained 0 while the response said 3 turns this red. (b) An instance built
// from a default-constructed location (all three requested XYs collapsing onto one point, or onto
// the origin) turns the per-position assertions red. (c) A zero or unset DrawScale3D - the
// grass-variety AddZeroed shape one namespace over, which renders nothing while reporting
// success - turns the scale assertion red.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFoliagePaintReachesFoliageActorTest,
    "PinWright.foliage.paint.PaintedInstancesReachTheFoliageActor",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FFoliagePaintReachesFoliageActorTest::RunTest(const FString& Parameters)
{
    using namespace FoliagePlacementTestHelpers;

    UWorld* World = EditorWorld();
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no_editor_world"),
            TEXT("GEditor->GetEditorWorldContext().World() returned null; the foliage.paint "
                 "placement assertions were stepped over."));
        return true;
    }

    DispatcherTestHelpers::FSinkPtr Sink;
    FRpcDispatcher Dispatcher;
    MakeDispatcher(Sink, Dispatcher);

    const FString TypeName = UniqueTypeName(TEXT("PaintStore"));
    ON_SCOPE_EXIT { DiscardType(Dispatcher, Sink, TypeName); };

    // A degenerate [1,1] scale range with alignment and random yaw off pins every placement
    // property this test asserts: when foliage.paint gains ground projection
    // (B-foliage-paint-does-no-ground-projection) it will start routing through the engine's
    // PlaceInstance, which draws a scale from this range and a yaw from these flags - so a
    // one-point range and two false flags keep the scale assertion honest across that fix.
    if (!CreateFoliageType(*this, Dispatcher, Sink, TypeName, 100.0, 1.0, 1.0, false, false))
    {
        return true;
    }
    UFoliageType* Type = LoadFoliageType(TypeName);
    if (!Type)
    {
        AddError(TEXT("the foliage type fixture could not be loaded back from its own path"));
        return true;
    }

    TArray<FFoliageInstance> Stored;
    RequireInstanceCount(*this, World, Type, Stored, 0,
        TEXT("a freshly created foliage type holds no instances"));

    // Three distinct XYs, so a verb that collapsed every entry onto one point - or onto the
    // origin - cannot satisfy the per-position assertions below.
    TArray<FVector2D> Positions;
    Positions.Emplace(ColX, ColY);
    Positions.Emplace(ColX + 250.0, ColY);
    Positions.Emplace(ColX, ColY + 250.0);

    TArray<TSharedPtr<FJsonValue>> Locations;
    for (const FVector2D& Pos : Positions)
    {
        Locations.Add(LocationEntry(Pos.X, Pos.Y, ColZ));
    }

    TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
    Params->SetStringField(TEXT("foliageTypePath"), ObjectPathFor(TypeName));
    Params->SetArrayField(TEXT("locations"), Locations);

    bool bSuccess = false;
    FString ErrorCode;
    TSharedPtr<FJsonObject> Result;
    Dispatch(Dispatcher, Sink, TEXT("foliage.paint"), TEXT("req-foliage-paint-store"),
        Params, bSuccess, Result, ErrorCode);

    TestTrue(*FString::Printf(TEXT("foliage.paint succeeds (error=%s)"), *ErrorCode), bSuccess);
    if (!bSuccess || !Result.IsValid())
    {
        return true;
    }

    // GROUND TRUTH, re-read from the level rather than from the response.
    RequireInstanceCount(*this, World, Type, Stored, 3,
        TEXT("the foliage actor holds one instance per requested location"));
    const int32 StoredCount = Stored.Num();

    // The response's own count is only meaningful next to that number.
    TestEqual(TEXT("instancesPlaced agrees with what the foliage actor holds"),
        RequireNumberField(*this, Result, TEXT("instancesPlaced")), StoredCount);

    for (const FVector2D& Pos : Positions)
    {
        TestEqual(*FString::Printf(TEXT("exactly one instance landed at the requested XY (%.1f, %.1f)"),
            Pos.X, Pos.Y), CountInstancesNearXY(Stored, Pos.X, Pos.Y), 1);
    }

    // Z IS DELIBERATELY UNASSERTED. foliage.paint writes the literal requested Z today
    // (B-foliage-paint-does-no-ground-projection); once that verb projects onto the surface, Z
    // becomes the ground under the point. XY survives a downward projection, so XY is what this
    // test pins.

    for (const FFoliageInstance& Inst : Stored)
    {
        TestEqual(TEXT("a painted instance carries a renderable unit scale"),
            static_cast<double>(Inst.DrawScale3D.X), 1.0, 0.001);
        TestEqual(TEXT("a painted instance's scale is uniform"),
            static_cast<double>(Inst.DrawScale3D.Z), 1.0, 0.001);
    }

    return true;
}

// COUNTERFACTUAL. The parse loop admits an entry only when it is a JSON object; a change that
// dropped that guard would build each junk entry from a default-constructed FVector and place it
// at the world origin, and a response that echoed the input array's length rather than the parsed
// count would claim five placements. Both turn this red.
//
// The `{}` entry - an object whose three coordinates are all missing, which today IS placed at
// the world origin - is NOT exercised here. That is the open second half of
// B-foliage-paint-does-no-ground-projection, and asserting either direction for it would pin a
// defect or land red on a handler someone else is fixing.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFoliagePaintJunkEntriesPlaceNothingTest,
    "PinWright.foliage.paint.NonObjectLocationEntriesPlaceNothing",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FFoliagePaintJunkEntriesPlaceNothingTest::RunTest(const FString& Parameters)
{
    using namespace FoliagePlacementTestHelpers;

    UWorld* World = EditorWorld();
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no_editor_world"),
            TEXT("GEditor->GetEditorWorldContext().World() returned null; the foliage.paint "
                 "malformed-entry assertions were stepped over."));
        return true;
    }

    DispatcherTestHelpers::FSinkPtr Sink;
    FRpcDispatcher Dispatcher;
    MakeDispatcher(Sink, Dispatcher);

    const FString TypeName = UniqueTypeName(TEXT("PaintJunk"));
    ON_SCOPE_EXIT { DiscardType(Dispatcher, Sink, TypeName); };

    if (!CreateFoliageType(*this, Dispatcher, Sink, TypeName, 100.0, 1.0, 1.0, false, false))
    {
        return true;
    }
    UFoliageType* Type = LoadFoliageType(TypeName);
    if (!Type)
    {
        AddError(TEXT("the foliage type fixture could not be loaded back from its own path"));
        return true;
    }

    const double FirstX = ColX + 1000.0;
    const double SecondX = ColX + 1250.0;

    // Two usable positions bracketing three unusable entries, so a parser that mis-indexed would
    // also be caught by the per-position assertions below.
    TArray<TSharedPtr<FJsonValue>> Locations;
    Locations.Add(LocationEntry(FirstX, ColY, ColZ));
    Locations.Add(MakeShared<FJsonValueNumber>(42.0));
    Locations.Add(MakeShared<FJsonValueString>(TEXT("not-a-position")));
    Locations.Add(MakeShared<FJsonValueNull>());
    Locations.Add(LocationEntry(SecondX, ColY, ColZ));

    TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
    Params->SetStringField(TEXT("foliageTypePath"), ObjectPathFor(TypeName));
    Params->SetArrayField(TEXT("locations"), Locations);

    bool bSuccess = false;
    FString ErrorCode;
    TSharedPtr<FJsonObject> Result;
    Dispatch(Dispatcher, Sink, TEXT("foliage.paint"), TEXT("req-foliage-paint-junk"),
        Params, bSuccess, Result, ErrorCode);

    TestTrue(*FString::Printf(TEXT("foliage.paint succeeds on a partly-usable batch (error=%s)"),
        *ErrorCode), bSuccess);
    if (!bSuccess || !Result.IsValid())
    {
        return true;
    }

    TArray<FFoliageInstance> Stored;
    RequireInstanceCount(*this, World, Type, Stored, 2,
        TEXT("only the two usable entries were placed"));
    const int32 StoredCount = Stored.Num();
    TestEqual(TEXT("instancesPlaced agrees with what the foliage actor holds"),
        RequireNumberField(*this, Result, TEXT("instancesPlaced")), StoredCount);
    TestEqual(TEXT("the first usable position was placed"),
        CountInstancesNearXY(Stored, FirstX, ColY), 1);
    TestEqual(TEXT("the second usable position was placed"),
        CountInstancesNearXY(Stored, SecondX, ColY), 1);
    TestFalse(TEXT("no unusable entry was placed at the world origin"),
        AnyInstanceAtWorldOrigin(Stored));

    return true;
}

// ============================================================================
// foliage.add_type - the response never echoes what it wrote
// ============================================================================

// COUNTERFACTUAL, and it is the whole point of this test. EVERY requested value below differs
// from the UFoliageType CDO default (InstancedFoliage.cpp:579-590 sets Density=100,
// AlignToNormal=true, RandomYaw=true, ScaleX/Y/Z=[1,1]). A handler that created and saved the
// asset and wrote nothing into it - which is exactly what landscape.create_grass_type does with
// its AddZeroed variety (B-create-grass-type-addzeroed-never-renders): success, a valid
// asset_path, and an inert asset - fails every line here. Asserting the DEFAULTS instead would
// have been the test that certifies the gap.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFoliageAddTypeWritesRequestedFieldsTest,
    "PinWright.foliage.add_type.WritesRequestedFieldsToTheAsset",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FFoliageAddTypeWritesRequestedFieldsTest::RunTest(const FString& Parameters)
{
    using namespace FoliagePlacementTestHelpers;

    UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, CubeMeshPath);
    if (!Cube)
    {
        AddError(FString::Printf(TEXT("required fixture mesh %s did not load"), CubeMeshPath));
        return true;
    }

    DispatcherTestHelpers::FSinkPtr Sink;
    FRpcDispatcher Dispatcher;
    MakeDispatcher(Sink, Dispatcher);

    const FString TypeName = UniqueTypeName(TEXT("AddTypeFields"));
    ON_SCOPE_EXIT { DiscardType(Dispatcher, Sink, TypeName); };

    TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
    Params->SetStringField(TEXT("name"), TypeName);
    Params->SetStringField(TEXT("meshPath"), CubeMeshPath);
    Params->SetNumberField(TEXT("density"), 37.5);
    Params->SetNumberField(TEXT("minScale"), 0.5);
    Params->SetNumberField(TEXT("maxScale"), 2.5);
    Params->SetBoolField(TEXT("alignToNormal"), false);
    Params->SetBoolField(TEXT("randomYaw"), false);

    bool bSuccess = false;
    FString ErrorCode;
    TSharedPtr<FJsonObject> Result;
    Dispatch(Dispatcher, Sink, TEXT("foliage.add_type"), TEXT("req-foliage-add-type-fields"),
        Params, bSuccess, Result, ErrorCode);

    TestTrue(*FString::Printf(TEXT("foliage.add_type succeeds (error=%s)"), *ErrorCode), bSuccess);
    if (!bSuccess || !Result.IsValid())
    {
        return true;
    }

    // The asset is resolved from the path the CALLER can construct, not from the response, so a
    // handler that wrote its fields into an object at some other path fails here.
    UFoliageType_InstancedStaticMesh* Type =
        LoadObject<UFoliageType_InstancedStaticMesh>(nullptr, *ObjectPathFor(TypeName));
    if (!Type)
    {
        AddError(TEXT("foliage.add_type reported success but no UFoliageType_InstancedStaticMesh "
                      "is addressable at /Game/Foliage/<name>.<name>"));
        return true;
    }

    TestTrue(TEXT("the requested mesh is attached to the type"), Type->GetStaticMesh() == Cube);
    TestEqual(TEXT("density is written, not left at the CDO's 100"),
        static_cast<double>(Type->Density), 37.5, 0.001);
    TestEqual(TEXT("minScale reaches ScaleX.Min"),
        static_cast<double>(Type->ScaleX.Min), 0.5, 0.001);
    TestEqual(TEXT("maxScale reaches ScaleX.Max"),
        static_cast<double>(Type->ScaleX.Max), 2.5, 0.001);
    TestEqual(TEXT("minScale reaches ScaleY.Min"),
        static_cast<double>(Type->ScaleY.Min), 0.5, 0.001);
    TestEqual(TEXT("maxScale reaches ScaleY.Max"),
        static_cast<double>(Type->ScaleY.Max), 2.5, 0.001);
    TestEqual(TEXT("minScale reaches ScaleZ.Min"),
        static_cast<double>(Type->ScaleZ.Min), 0.5, 0.001);
    TestEqual(TEXT("maxScale reaches ScaleZ.Max"),
        static_cast<double>(Type->ScaleZ.Max), 2.5, 0.001);
    TestFalse(TEXT("alignToNormal:false is written, not left at the CDO's true"),
        static_cast<bool>(Type->AlignToNormal));
    TestFalse(TEXT("randomYaw:false is written, not left at the CDO's true"),
        static_cast<bool>(Type->RandomYaw));

    // The reported path must lead back to the same object, or a caller who round-trips
    // asset_path into the next verb addresses something else.
    FString ReportedPath;
    if (TestTrue(TEXT("response carries asset_path"),
            Result->TryGetStringField(TEXT("asset_path"), ReportedPath)))
    {
        TestTrue(TEXT("asset_path resolves to the asset that was written"),
            LoadObject<UFoliageType>(nullptr, *ReportedPath) == Type);
    }

    return true;
}

// COUNTERFACTUAL. Every argument validator in this handler runs before CreatePackage/NewObject.
// A validator moved below them - the ordinary way this regresses when a new argument is added -
// would leave a half-built, dirty /Game/Foliage/<name> package behind on a refused call, which
// a later editor-wide save-all then flushes into host Content (B-tests-leak-host-content). The
// existing add_type.MissingMeshPath test asserts the error code and stops there, so nothing
// checked the world afterwards.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFoliageAddTypeRefusalCreatesNothingTest,
    "PinWright.foliage.add_type.RefusedInputCreatesNoAsset",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FFoliageAddTypeRefusalCreatesNothingTest::RunTest(const FString& Parameters)
{
    using namespace FoliagePlacementTestHelpers;

    DispatcherTestHelpers::FSinkPtr Sink;
    FRpcDispatcher Dispatcher;
    MakeDispatcher(Sink, Dispatcher);

    const FString TypeName = UniqueTypeName(TEXT("AddTypeRefused"));
    // Torn down anyway: if the refusal DID leave an asset behind, the test must not also leak it.
    ON_SCOPE_EXIT { CleanupTestAsset(PackagePathFor(TypeName)); };

    // minScale above maxScale is an unsatisfiable range, refused by the handler's own argument
    // check rather than by the dispatcher's schema validation.
    TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
    Params->SetStringField(TEXT("name"), TypeName);
    Params->SetStringField(TEXT("meshPath"), CubeMeshPath);
    Params->SetNumberField(TEXT("minScale"), 3.0);
    Params->SetNumberField(TEXT("maxScale"), 1.0);

    bool bSuccess = false;
    FString ErrorCode;
    Dispatch(Dispatcher, Sink, TEXT("foliage.add_type"), TEXT("req-foliage-add-type-refused"),
        Params, bSuccess, ErrorCode);

    TestFalse(TEXT("minScale > maxScale is refused"), bSuccess);
    TestEqual(TEXT("an unsatisfiable scale range -> INVALID_ARGUMENT"), ErrorCode,
        FString(TEXT("INVALID_ARGUMENT")));

    TestTrue(TEXT("a refused add_type leaves no object at the requested path"),
        FindObject<UObject>(nullptr, *ObjectPathFor(TypeName)) == nullptr);
    TestFalse(TEXT("a refused add_type registers no asset at the requested path"),
        UEditorAssetLibrary::DoesAssetExist(PackagePathFor(TypeName)));

    return true;
}

// ============================================================================
// foliage.remove - scope was only ever asserted through the response's `mode` field
// ============================================================================

// COUNTERFACTUALS. (a) A scoped call that fell through to the removeAll branch - or a FindInfo
// that matched the wrong type - empties the second fixture too, which the survivor assertions
// catch; the response would look identical either way, which is why `mode` alone was never
// enough. (b) A handler that computed instancesRemoved from the info's count but did not empty
// the array reports the same number while every instance is still there, and the record re-read
// catches it. (b') THE ONE THAT GOT THROUGH, until B-foliage-remove-empties-ledger-not-component:
// a handler that empties the record and leaves the components full also reports the same number,
// and a record-only re-read agrees with it. Both halves of RequireInstanceCount are therefore
// load-bearing here, and .RemovalReachesTheRenderedInstances below drives it end to end through
// the readback verb as well. (c) Asserting the survivors' exact positions catches a removal that
// rebuilt the remaining instances instead of leaving them untouched.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFoliageRemoveScopedSparesOtherTypesTest,
    "PinWright.foliage.remove.ScopedRemovalSparesOtherTypes",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FFoliageRemoveScopedSparesOtherTypesTest::RunTest(const FString& Parameters)
{
    using namespace FoliagePlacementTestHelpers;

    UWorld* World = EditorWorld();
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no_editor_world"),
            TEXT("GEditor->GetEditorWorldContext().World() returned null; the foliage.remove "
                 "scope assertions were stepped over."));
        return true;
    }

    DispatcherTestHelpers::FSinkPtr Sink;
    FRpcDispatcher Dispatcher;
    MakeDispatcher(Sink, Dispatcher);

    const FString TargetName = UniqueTypeName(TEXT("RemoveTarget"));
    const FString SurvivorName = UniqueTypeName(TEXT("RemoveSurvivor"));
    ON_SCOPE_EXIT
    {
        DiscardType(Dispatcher, Sink, TargetName);
        DiscardType(Dispatcher, Sink, SurvivorName);
    };

    if (!CreateFoliageType(*this, Dispatcher, Sink, TargetName, 100.0, 1.0, 1.0, false, false)
        || !CreateFoliageType(*this, Dispatcher, Sink, SurvivorName, 100.0, 1.0, 1.0, false, false))
    {
        return true;
    }
    UFoliageType* TargetType = LoadFoliageType(TargetName);
    UFoliageType* SurvivorType = LoadFoliageType(SurvivorName);
    if (!TargetType || !SurvivorType)
    {
        AddError(TEXT("a foliage type fixture could not be loaded back from its own path"));
        return true;
    }

    const double TargetX = ColX + 2000.0;
    const double SurvivorX = ColX + 3000.0;

    // add_instances (not paint) writes the literal transform it is handed - that is its
    // documented contract and no board ticket disputes it - so the survivors' exact positions
    // are a legitimate thing to pin.
    auto Populate = [&](const FString& Name, double BaseX, int32 Count)
    {
        TArray<TSharedPtr<FJsonValue>> Locations;
        for (int32 Index = 0; Index < Count; ++Index)
        {
            Locations.Add(LocationEntry(BaseX + Index * 250.0, ColY, ColZ));
        }
        TSharedPtr<FJsonObject> AddParams = MakeShared<FJsonObject>();
        AddParams->SetStringField(TEXT("foliageTypePath"), ObjectPathFor(Name));
        AddParams->SetArrayField(TEXT("locations"), Locations);

        bool bAdded = false;
        FString AddErrorCode;
        Dispatch(Dispatcher, Sink, TEXT("foliage.add_instances"),
            TEXT("req-foliage-remove-populate"), AddParams, bAdded, AddErrorCode);
        TestTrue(*FString::Printf(TEXT("foliage.add_instances populated '%s' (error=%s)"),
            *Name, *AddErrorCode), bAdded);
    };
    Populate(TargetName, TargetX, 3);
    Populate(SurvivorName, SurvivorX, 2);

    TArray<FFoliageInstance> TargetStored;
    TArray<FFoliageInstance> SurvivorStored;
    // Fixture precondition: without it every assertion below is vacuous.
    RequireInstanceCount(*this, World, TargetType, TargetStored, 3,
        TEXT("the target type starts with three instances"));
    RequireInstanceCount(*this, World, SurvivorType, SurvivorStored, 2,
        TEXT("the survivor type starts with two instances"));

    TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
    Params->SetStringField(TEXT("foliageTypePath"), ObjectPathFor(TargetName));

    bool bSuccess = false;
    FString ErrorCode;
    TSharedPtr<FJsonObject> Result;
    Dispatch(Dispatcher, Sink, TEXT("foliage.remove"), TEXT("req-foliage-remove-scoped"),
        Params, bSuccess, Result, ErrorCode);

    TestTrue(*FString::Printf(TEXT("a scoped foliage.remove succeeds (error=%s)"), *ErrorCode),
        bSuccess);
    if (!bSuccess || !Result.IsValid())
    {
        return true;
    }

    TestEqual(TEXT("instancesRemoved matches what the target type held"),
        RequireNumberField(*this, Result, TEXT("instancesRemoved")), 3);
    FString Mode;
    Result->TryGetStringField(TEXT("mode"), Mode);
    TestEqual(TEXT("the applied scope is echoed as a type-scoped removal"), Mode,
        FString(TEXT("type")));

    // GROUND TRUTH on both sides of the scope boundary, and on both representations of each
    // side: the record AND the rendered instances.
    RequireInstanceCount(*this, World, TargetType, TargetStored, 0,
        TEXT("the named type's instances are gone from the foliage actor"));
    RequireInstanceCount(*this, World, SurvivorType, SurvivorStored, 2,
        TEXT("the unnamed type's instances are untouched"));
    for (int32 Index = 0; Index < 2; ++Index)
    {
        TestEqual(*FString::Printf(TEXT("survivor instance %d is still exactly where it was placed"),
            Index), CountInstancesNearXY(SurvivorStored, SurvivorX + Index * 250.0, ColY), 1);
    }

    return true;
}

// REGRESSION for B-foliage-remove-empties-ledger-not-component. foliage.remove used to call
// FFoliageInfo::Instances.Empty() and never touch the component that draws, so after a removal
// the verb reported an exact instancesRemoved, foliage.get_instances reported count:0, and the
// level still drew every instance - measured live at 56/84/60/54/12/10 with a post-removal
// capture pixel-identical to the frame before it. Neither honesty surface the namespace had could
// see it, because both read the same array. The divergence is not cosmetic: FFoliageStaticMesh::
// Reapply reconciles the two in the record's favour by calling Component->RemoveInstances on the
// surplus, so the still-drawn instances are deleted for real later, from an unrelated
// PostEditUndo, with nothing linking it back to the verb that caused it.
//
// The fix routes both branches through FFoliageInfo::RemoveInstances - the API foliage.paint in
// the same file already uses - and adds renderedInstanceCount/ledgerMatchesRendered to the
// readback so a caller can observe the split at all.
//
// COUNTERFACTUALS. (a) The defect itself: an Instances.Empty() removal leaves the rendered count
// at 3 and ledgerMatchesRendered false, so the second and fourth assertions below fail. (b) The
// mirror defect - clearing the component and leaving the record - fails the record half of
// RequireInstanceCount and flips ledgerMatchesRendered the other way. (c) A get_instances that
// derived renderedInstanceCount from Instances.Num() instead of the component would pass every
// assertion after the fix and every assertion before it, which is why the test reads the
// component directly (GatherRenderedInstances) rather than trusting the new field alone.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFoliageRemoveReachesRenderedInstancesTest,
    "PinWright.foliage.remove.RemovalReachesTheRenderedInstances",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FFoliageRemoveReachesRenderedInstancesTest::RunTest(const FString& Parameters)
{
    using namespace FoliagePlacementTestHelpers;

    UWorld* World = EditorWorld();
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no_editor_world"),
            TEXT("GEditor->GetEditorWorldContext().World() returned null; the foliage.remove "
                 "render-side assertions were stepped over."));
        return true;
    }

    DispatcherTestHelpers::FSinkPtr Sink;
    FRpcDispatcher Dispatcher;
    MakeDispatcher(Sink, Dispatcher);

    const FString TypeName = UniqueTypeName(TEXT("RemoveRendered"));
    ON_SCOPE_EXIT
    {
        DiscardType(Dispatcher, Sink, TypeName);
    };

    if (!CreateFoliageType(*this, Dispatcher, Sink, TypeName, 100.0, 1.0, 1.0, false, false))
    {
        return true;
    }
    UFoliageType* Type = LoadFoliageType(TypeName);
    if (!Type)
    {
        AddError(TEXT("the foliage type fixture could not be loaded back from its own path"));
        return true;
    }

    const double BaseX = ColX + 4000.0;

    TArray<TSharedPtr<FJsonValue>> Locations;
    for (int32 Index = 0; Index < 3; ++Index)
    {
        Locations.Add(LocationEntry(BaseX + Index * 250.0, ColY, ColZ));
    }
    TSharedPtr<FJsonObject> AddParams = MakeShared<FJsonObject>();
    AddParams->SetStringField(TEXT("foliageTypePath"), ObjectPathFor(TypeName));
    AddParams->SetArrayField(TEXT("locations"), Locations);

    bool bAdded = false;
    FString AddErrorCode;
    Dispatch(Dispatcher, Sink, TEXT("foliage.add_instances"),
        TEXT("req-foliage-remove-rendered-populate"), AddParams, bAdded, AddErrorCode);
    if (!bAdded)
    {
        AddError(FString::Printf(TEXT("foliage.add_instances could not populate the fixture "
            "(error=%s)"), *AddErrorCode));
        return true;
    }

    // Precondition, on both representations: the add path reaches the component, which is the
    // asymmetry that made the removal defect visible in the first place.
    TArray<FFoliageInstance> Stored;
    RequireInstanceCount(*this, World, Type, Stored, 3,
        TEXT("the fixture starts with three instances"));

    // And the readback agrees with the level before anything is removed.
    {
        TSharedPtr<FJsonObject> ReadParams = MakeShared<FJsonObject>();
        ReadParams->SetStringField(TEXT("foliageTypePath"), ObjectPathFor(TypeName));

        bool bRead = false;
        FString ReadErrorCode;
        TSharedPtr<FJsonObject> ReadResult;
        Dispatch(Dispatcher, Sink, TEXT("foliage.get_instances"),
            TEXT("req-foliage-remove-rendered-read-before"), ReadParams, bRead, ReadResult,
            ReadErrorCode);
        TestTrue(*FString::Printf(TEXT("foliage.get_instances succeeds before the removal "
            "(error=%s)"), *ReadErrorCode), bRead);
        if (bRead && ReadResult.IsValid())
        {
            TestEqual(TEXT("get_instances reports the three rendered instances before the removal"),
                RequireNumberField(*this, ReadResult, TEXT("renderedInstanceCount")), 3);
            bool bAgrees = false;
            TestTrue(TEXT("get_instances carries ledgerMatchesRendered"),
                ReadResult->TryGetBoolField(TEXT("ledgerMatchesRendered"), bAgrees));
            TestTrue(TEXT("the record and the rendered instances agree before the removal"),
                bAgrees);
        }
    }

    TSharedPtr<FJsonObject> RemoveParams = MakeShared<FJsonObject>();
    RemoveParams->SetStringField(TEXT("foliageTypePath"), ObjectPathFor(TypeName));

    bool bRemoved = false;
    FString RemoveErrorCode;
    TSharedPtr<FJsonObject> RemoveResult;
    Dispatch(Dispatcher, Sink, TEXT("foliage.remove"), TEXT("req-foliage-remove-rendered"),
        RemoveParams, bRemoved, RemoveResult, RemoveErrorCode);

    TestTrue(*FString::Printf(TEXT("foliage.remove succeeds (error=%s)"), *RemoveErrorCode),
        bRemoved);
    if (!bRemoved || !RemoveResult.IsValid())
    {
        return true;
    }
    TestEqual(TEXT("instancesRemoved matches what the type held"),
        RequireNumberField(*this, RemoveResult, TEXT("instancesRemoved")), 3);

    // THE ASSERTION THIS TICKET EXISTS FOR. Read off the component, not the record: with the
    // defect in place the record half is 0 and this half is still 3.
    TestEqual(TEXT("the removal reached the instances the level draws"),
        GatherRenderedInstances(World, Type), 0);
    RequireInstanceCount(*this, World, Type, Stored, 0,
        TEXT("nothing is left on either representation after the removal"));

    // And the readback can now say so on its own, which is what makes a future regression
    // observable through the namespace instead of only through a screenshot.
    {
        TSharedPtr<FJsonObject> ReadParams = MakeShared<FJsonObject>();
        ReadParams->SetStringField(TEXT("foliageTypePath"), ObjectPathFor(TypeName));

        bool bRead = false;
        FString ReadErrorCode;
        TSharedPtr<FJsonObject> ReadResult;
        Dispatch(Dispatcher, Sink, TEXT("foliage.get_instances"),
            TEXT("req-foliage-remove-rendered-read-after"), ReadParams, bRead, ReadResult,
            ReadErrorCode);
        TestTrue(*FString::Printf(TEXT("foliage.get_instances succeeds after the removal "
            "(error=%s)"), *ReadErrorCode), bRead);
        if (bRead && ReadResult.IsValid())
        {
            TestEqual(TEXT("get_instances reports no records after the removal"),
                RequireNumberField(*this, ReadResult, TEXT("count")), 0);
            TestEqual(TEXT("get_instances reports no rendered instances after the removal"),
                RequireNumberField(*this, ReadResult, TEXT("renderedInstanceCount")), 0);
            bool bAgrees = false;
            TestTrue(TEXT("get_instances carries ledgerMatchesRendered after the removal"),
                ReadResult->TryGetBoolField(TEXT("ledgerMatchesRendered"), bAgrees));
            TestTrue(TEXT("the record and the rendered instances agree after the removal"),
                bAgrees);
        }
    }

    return true;
}

// ============================================================================
// foliage.add_instances - four tests exist and none reads the store
// ============================================================================

// COUNTERFACTUALS. (a) Dropping the `continue` that rejects a location-less transforms[] entry
// would place it at the world origin from a default-constructed FVector, which the origin
// assertion catches - the response's skippedCount would still read 2 on some plausible
// variants, which is why this is asserted against the store. (b) Losing the rotation or scale
// parse (the E-foliage-get-instances-drops-scale defect class, proven here at the STORE rather
// than through the read verb, so a symmetric write+read loss cannot hide it) turns the
// per-field assertions red. (c) Losing the array-form [x,y,z] location branch drops that
// instance entirely. (d) An instances_count echoing the input array's length rather than what
// was placed disagrees with the store count.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFoliageAddInstancesOnlyParsableReachStoreTest,
    "PinWright.foliage.add_instances.OnlyParsableEntriesReachTheStore",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FFoliageAddInstancesOnlyParsableReachStoreTest::RunTest(const FString& Parameters)
{
    using namespace FoliagePlacementTestHelpers;

    UWorld* World = EditorWorld();
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no_editor_world"),
            TEXT("GEditor->GetEditorWorldContext().World() returned null; the "
                 "foliage.add_instances store assertions were stepped over."));
        return true;
    }

    DispatcherTestHelpers::FSinkPtr Sink;
    FRpcDispatcher Dispatcher;
    MakeDispatcher(Sink, Dispatcher);

    const FString TypeName = UniqueTypeName(TEXT("AddInstancesStore"));
    ON_SCOPE_EXIT { DiscardType(Dispatcher, Sink, TypeName); };

    if (!CreateFoliageType(*this, Dispatcher, Sink, TypeName, 100.0, 1.0, 1.0, false, false))
    {
        return true;
    }
    UFoliageType* Type = LoadFoliageType(TypeName);
    if (!Type)
    {
        AddError(TEXT("the foliage type fixture could not be loaded back from its own path"));
        return true;
    }

    const double FullX = ColX + 4000.0;
    const double ArrayX = ColX + 4250.0;
    const double Pitch = 10.0, Yaw = 45.0, Roll = 20.0;
    const double ScaleX = 2.5, ScaleY = 0.5, ScaleZ = 3.0;

    // Entry 0: the full object form, with a non-zero rotation and a non-uniform scale that no
    // default could produce.
    TSharedPtr<FJsonObject> Rotation = MakeShared<FJsonObject>();
    Rotation->SetNumberField(TEXT("pitch"), Pitch);
    Rotation->SetNumberField(TEXT("yaw"), Yaw);
    Rotation->SetNumberField(TEXT("roll"), Roll);
    TSharedPtr<FJsonObject> Full = MakeShared<FJsonObject>();
    Full->SetObjectField(TEXT("location"), Vec3(FullX, ColY, ColZ));
    Full->SetObjectField(TEXT("rotation"), Rotation);
    Full->SetObjectField(TEXT("scale"), Vec3(ScaleX, ScaleY, ScaleZ));

    // Entry 1: a rotation with no location. Documented as unusable and reported in skipped[].
    TSharedPtr<FJsonObject> NoLocation = MakeShared<FJsonObject>();
    NoLocation->SetObjectField(TEXT("rotation"), Rotation);

    // Entry 2: the array location form, which is a separate parse branch.
    TArray<TSharedPtr<FJsonValue>> LocationArray;
    LocationArray.Add(MakeShared<FJsonValueNumber>(ArrayX));
    LocationArray.Add(MakeShared<FJsonValueNumber>(ColY));
    LocationArray.Add(MakeShared<FJsonValueNumber>(ColZ));
    TSharedPtr<FJsonObject> ArrayForm = MakeShared<FJsonObject>();
    ArrayForm->SetArrayField(TEXT("location"), LocationArray);

    TArray<TSharedPtr<FJsonValue>> Transforms;
    Transforms.Add(MakeShared<FJsonValueObject>(Full));
    Transforms.Add(MakeShared<FJsonValueObject>(NoLocation));
    Transforms.Add(MakeShared<FJsonValueObject>(ArrayForm));
    // Entry 3: not an object at all.
    Transforms.Add(MakeShared<FJsonValueNumber>(7.0));

    TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
    Params->SetStringField(TEXT("foliageTypePath"), ObjectPathFor(TypeName));
    Params->SetArrayField(TEXT("transforms"), Transforms);

    bool bSuccess = false;
    FString ErrorCode;
    TSharedPtr<FJsonObject> Result;
    Dispatch(Dispatcher, Sink, TEXT("foliage.add_instances"),
        TEXT("req-foliage-add-instances-store"), Params, bSuccess, Result, ErrorCode);

    TestTrue(*FString::Printf(TEXT("foliage.add_instances succeeds on a partly-usable batch (error=%s)"),
        *ErrorCode), bSuccess);
    if (!bSuccess || !Result.IsValid())
    {
        return true;
    }

    TArray<FFoliageInstance> Stored;
    RequireInstanceCount(*this, World, Type, Stored, 2,
        TEXT("only the two usable entries reached the foliage actor"));
    const int32 StoredCount = Stored.Num();
    TestEqual(TEXT("instances_count agrees with what the foliage actor holds"),
        RequireNumberField(*this, Result, TEXT("instances_count")), StoredCount);
    TestEqual(TEXT("both unusable entries are accounted for"),
        RequireNumberField(*this, Result, TEXT("skippedCount")), 2);
    TestFalse(TEXT("the location-less entry was not placed at the world origin"),
        AnyInstanceAtWorldOrigin(Stored));

    if (const FFoliageInstance* Placed = FindInstanceNearXY(Stored, FullX, ColY))
    {
        TestEqual(TEXT("the object-form location reaches the store exactly"),
            Placed->Location.Z, ColZ, PosTolerance);
        TestEqual(TEXT("pitch reaches the store"), Placed->Rotation.Pitch, Pitch, 0.01);
        TestEqual(TEXT("yaw reaches the store"), Placed->Rotation.Yaw, Yaw, 0.01);
        TestEqual(TEXT("roll reaches the store"), Placed->Rotation.Roll, Roll, 0.01);
        TestEqual(TEXT("a non-uniform scale reaches the store on X"),
            static_cast<double>(Placed->DrawScale3D.X), ScaleX, 0.001);
        TestEqual(TEXT("a non-uniform scale reaches the store on Y"),
            static_cast<double>(Placed->DrawScale3D.Y), ScaleY, 0.001);
        TestEqual(TEXT("a non-uniform scale reaches the store on Z"),
            static_cast<double>(Placed->DrawScale3D.Z), ScaleZ, 0.001);
    }
    else
    {
        AddError(TEXT("the fully-specified transform did not reach the foliage actor"));
    }

    if (const FFoliageInstance* Placed = FindInstanceNearXY(Stored, ArrayX, ColY))
    {
        TestEqual(TEXT("the array-form location reaches the store exactly"),
            Placed->Location.Z, ColZ, PosTolerance);
        TestEqual(TEXT("an entry with no rotation is stored unrotated"),
            Placed->Rotation.Yaw, 0.0, 0.01);
        TestEqual(TEXT("an entry with no scale is stored at unit scale"),
            static_cast<double>(Placed->DrawScale3D.X), 1.0, 0.001);
    }
    else
    {
        AddError(TEXT("the array-form location did not reach the foliage actor"));
    }

    return true;
}

// ============================================================================
// foliage.get_instances - the existing round trip cannot see a symmetric loss
// ============================================================================

// COUNTERFACTUALS. (a) A filtered read that ignored its filter returns five rows instead of two.
// (b) The unfiltered branch walks every type through ForEachFoliageInfo and its lambda returns
// true to continue; a variant returning false stops after the first type, which reads as a
// perfectly ordinary success and is caught only by requiring BOTH fixtures to appear. (c) The
// unfiltered branch historically dropped rotation and scale entirely
// (E-foliage-get-instances-drops-scale) - the non-uniform scale and non-zero rotation live on
// the instance read through THAT branch for exactly this reason. (d) `count` disagreeing with
// the array it accompanies. Every expected value is taken from the level's own store rather
// than from what was sent, so a write verb that mangled the transform cannot make this test pass
// by mangling the read the same way.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFoliageGetInstancesFilterScopeTest,
    "PinWright.foliage.get_instances.FilterScopeMatchesTheStore",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FFoliageGetInstancesFilterScopeTest::RunTest(const FString& Parameters)
{
    using namespace FoliagePlacementTestHelpers;

    UWorld* World = EditorWorld();
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no_editor_world"),
            TEXT("GEditor->GetEditorWorldContext().World() returned null; the "
                 "foliage.get_instances scope assertions were stepped over."));
        return true;
    }

    DispatcherTestHelpers::FSinkPtr Sink;
    FRpcDispatcher Dispatcher;
    MakeDispatcher(Sink, Dispatcher);

    const FString FilteredName = UniqueTypeName(TEXT("ReadFiltered"));
    const FString OtherName = UniqueTypeName(TEXT("ReadOther"));
    ON_SCOPE_EXIT
    {
        DiscardType(Dispatcher, Sink, FilteredName);
        DiscardType(Dispatcher, Sink, OtherName);
    };

    if (!CreateFoliageType(*this, Dispatcher, Sink, FilteredName, 100.0, 1.0, 1.0, false, false)
        || !CreateFoliageType(*this, Dispatcher, Sink, OtherName, 100.0, 1.0, 1.0, false, false))
    {
        return true;
    }
    UFoliageType* FilteredType = LoadFoliageType(FilteredName);
    UFoliageType* OtherType = LoadFoliageType(OtherName);
    if (!FilteredType || !OtherType)
    {
        AddError(TEXT("a foliage type fixture could not be loaded back from its own path"));
        return true;
    }

    const double FilteredX = ColX + 5000.0;
    const double OtherX = ColX + 6000.0;

    // Two plain instances on the filtered type.
    {
        TArray<TSharedPtr<FJsonValue>> Locations;
        Locations.Add(LocationEntry(FilteredX, ColY, ColZ));
        Locations.Add(LocationEntry(FilteredX + 250.0, ColY, ColZ));
        TSharedPtr<FJsonObject> AddParams = MakeShared<FJsonObject>();
        AddParams->SetStringField(TEXT("foliageTypePath"), ObjectPathFor(FilteredName));
        AddParams->SetArrayField(TEXT("locations"), Locations);

        bool bAdded = false;
        FString AddErrorCode;
        Dispatch(Dispatcher, Sink, TEXT("foliage.add_instances"),
            TEXT("req-foliage-read-populate-filtered"), AddParams, bAdded, AddErrorCode);
        TestTrue(*FString::Printf(TEXT("the filtered fixture was populated (error=%s)"),
            *AddErrorCode), bAdded);
    }

    // One instance on the other type, carrying the rotation and non-uniform scale that the
    // unfiltered read branch used to drop.
    {
        TSharedPtr<FJsonObject> Rotation = MakeShared<FJsonObject>();
        Rotation->SetNumberField(TEXT("pitch"), 12.0);
        Rotation->SetNumberField(TEXT("yaw"), 34.0);
        Rotation->SetNumberField(TEXT("roll"), 56.0);
        TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetObjectField(TEXT("location"), Vec3(OtherX, ColY, ColZ));
        Entry->SetObjectField(TEXT("rotation"), Rotation);
        Entry->SetObjectField(TEXT("scale"), Vec3(2.5, 0.5, 3.0));

        TArray<TSharedPtr<FJsonValue>> Transforms;
        Transforms.Add(MakeShared<FJsonValueObject>(Entry));
        TSharedPtr<FJsonObject> AddParams = MakeShared<FJsonObject>();
        AddParams->SetStringField(TEXT("foliageTypePath"), ObjectPathFor(OtherName));
        AddParams->SetArrayField(TEXT("transforms"), Transforms);

        bool bAdded = false;
        FString AddErrorCode;
        Dispatch(Dispatcher, Sink, TEXT("foliage.add_instances"),
            TEXT("req-foliage-read-populate-other"), AddParams, bAdded, AddErrorCode);
        TestTrue(*FString::Printf(TEXT("the second fixture was populated (error=%s)"),
            *AddErrorCode), bAdded);
    }

    TArray<FFoliageInstance> FilteredStored;
    TArray<FFoliageInstance> OtherStored;
    RequireInstanceCount(*this, World, FilteredType, FilteredStored, 2,
        TEXT("the filtered type holds two instances"));
    RequireInstanceCount(*this, World, OtherType, OtherStored, 1,
        TEXT("the other type holds one instance"));
    if (FilteredStored.Num() != 2 || OtherStored.Num() != 1)
    {
        return true;
    }

    // ---- Filtered read: exactly the named type's instances, matching the store ----
    {
        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("foliageTypePath"), ObjectPathFor(FilteredName));

        bool bSuccess = false;
        FString ErrorCode;
        TSharedPtr<FJsonObject> Result;
        Dispatch(Dispatcher, Sink, TEXT("foliage.get_instances"),
            TEXT("req-foliage-get-filtered"), Params, bSuccess, Result, ErrorCode);

        TestTrue(*FString::Printf(TEXT("a filtered foliage.get_instances succeeds (error=%s)"),
            *ErrorCode), bSuccess);
        if (bSuccess && Result.IsValid())
        {
            const TArray<TSharedPtr<FJsonValue>>* Rows = nullptr;
            if (TestTrue(TEXT("the filtered read carries an instances array"),
                    Result->TryGetArrayField(TEXT("instances"), Rows) && Rows))
            {
                TestEqual(TEXT("the filtered read returns only the named type's instances"),
                    Rows->Num(), 2);
                TestEqual(TEXT("count agrees with the array it accompanies"),
                    RequireNumberField(*this, Result, TEXT("count")), Rows->Num());
                for (const FFoliageInstance& Inst : FilteredStored)
                {
                    const TSharedPtr<FJsonObject> Row =
                        FindRowNearXY(Rows, Inst.Location.X, Inst.Location.Y);
                    if (Row.IsValid())
                    {
                        TestRowMatchesInstance(*this, TEXT("filtered row"), Row, Inst);
                    }
                    else
                    {
                        AddError(FString::Printf(
                            TEXT("the filtered read omitted the instance at (%.1f, %.1f)"),
                            Inst.Location.X, Inst.Location.Y));
                    }
                }
            }
        }
    }

    // ---- Unfiltered read: every type is walked, and each row carries its full transform ----
    {
        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();

        bool bSuccess = false;
        FString ErrorCode;
        TSharedPtr<FJsonObject> Result;
        Dispatch(Dispatcher, Sink, TEXT("foliage.get_instances"),
            TEXT("req-foliage-get-unfiltered"), Params, bSuccess, Result, ErrorCode);

        TestTrue(*FString::Printf(TEXT("an unfiltered foliage.get_instances succeeds (error=%s)"),
            *ErrorCode), bSuccess);
        if (bSuccess && Result.IsValid())
        {
            const TArray<TSharedPtr<FJsonValue>>* Rows = nullptr;
            if (TestTrue(TEXT("the unfiltered read carries an instances array"),
                    Result->TryGetArrayField(TEXT("instances"), Rows) && Rows))
            {
                TestEqual(TEXT("count agrees with the array it accompanies"),
                    RequireNumberField(*this, Result, TEXT("count")), Rows->Num());
                // Both fixtures must appear: an iteration that stopped after the first type
                // would still report a plausible success.
                TestEqual(TEXT("the unfiltered read reports both of the first type's instances"),
                    CountRowsForType(Rows, FilteredType->GetPathName()), 2);
                TestEqual(TEXT("the unfiltered read reports the second type's instance"),
                    CountRowsForType(Rows, OtherType->GetPathName()), 1);

                const TSharedPtr<FJsonObject> Row =
                    FindRowNearXY(Rows, OtherStored[0].Location.X, OtherStored[0].Location.Y);
                if (Row.IsValid())
                {
                    TestRowMatchesInstance(*this, TEXT("unfiltered row"), Row, OtherStored[0]);
                }
                else
                {
                    AddError(TEXT("the unfiltered read omitted the second type's instance"));
                }
            }
        }
    }

    return true;
}
