// Copyright (c) 2026 Alexander Penkin. MIT License.

// TestFoliagePaintTypeRotation.cpp - regression coverage for
// B-foliage-paint-ignores-align-to-normal-and-random-yaw.
//
// WHAT WAS WRONG. foliage.paint's placement loop wrote `Instance.Rotation =
// FRotator::ZeroRotator` for every instance and never asked the UFoliageType how it wanted to be
// oriented. UFoliageType::UFoliageType sets AlignToNormal = true and RandomYaw = true on every
// instance of the class (Runtime/Foliage/Private/InstancedFoliage.cpp:585-586), so the type the
// verb resolves - and, on the auto-create path, the type the verb itself WRITES - asks for both.
// Measured live on a 33.9-degree slope, all four painted instances read back pitch 0, yaw 0,
// roll 0: four identically oriented plants standing at a right angle to a hillside, with nothing
// in the response able to reveal it.
//
// WHAT THE FIX DOES. The loop now mirrors FPotentialInstance::PlaceInstance's non-procedural
// branch (InstancedFoliage.cpp:5525-5548): RandomYaw / RandomPitchAngle are drawn on both
// branches (they need no surface) with FOLIAGE_NoRandomYaw stamped on the else branch, and
// AlignToNormal is applied on the projected branch against
// FGroundContactReport::AverageNormal - the normal the seat solve ALREADY measured under this
// instance's own footprint, not a second trace. The response grew a `rotation` block (what the
// type asked for, next to the measured alignedCount) and placed[] rows now carry the pitch, yaw
// and roll actually written.
//
// THE TWO TESTS ARE THE TWO HALVES OF THAT CONTRACT, and each is red before the fix:
//   (a) painting a type with AlignToNormal + RandomYaw onto a 30-degree slope produces instances
//       whose own +Z axis points along the slope normal, with yaws that differ from each other -
//       and a second type on the same slope with RandomYaw OFF produces instances that are still
//       aligned but share one yaw and carry FOLIAGE_NoRandomYaw. The pair is what separates
//       "reads the flag" from "always randomises". Before the fix every rotation is identity, so
//       both the alignment and the spread assertions fail.
//   (b) without a `surface` there is no normal, so AlignToNormal CANNOT be honoured - and the
//       response says which of the two happened rather than dropping the flag. Random yaw is
//       still applied there, because it needs no surface. Before the fix there is no `rotation`
//       block at all and every yaw is 0.
//
// The alignment assertion is on the instance's own up AXIS rather than on Euler angles: the
// composed rotation is FQuat(AlignRotation) * FQuat(YawRotation), whose pitch/yaw/roll triple
// depends on the random yaw while its +Z axis does not. Comparing axes tests the property the
// ticket is about without pinning a number that legitimately varies.
//
// FIXTURES. The slope is a real AStaticMeshActor - an engine cube, widened on XY and spawned with
// a 30-degree pitch - dropped into an isolated far column of the editor world and destroyed on
// scope exit, following Tests/Spatial/TestGroundPlacement.cpp and
// Tests/Environment/TestFoliagePaintGroundProjection.cpp, including the world-tick flush without
// which a just-spawned body is not yet in the scene-query structure. Foliage types are built
// through the real foliage.add_type verb under GUID-suffixed names and torn down type-scoped, so
// the host map's own foliage is never touched.
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

// Named (not anonymous) namespace: the plugin's tests share one module with Unity builds enabled,
// where same-named anonymous-namespace helpers collide across merged translation units.
namespace FoliagePaintRotationTestHelpers
{
    constexpr const TCHAR* RotCubeMeshPath = TEXT("/Engine/BasicShapes/Cube.Cube");

    // An isolated column, away from the ones TestGroundPlacement.cpp,
    // TestFoliagePaintGroundProjection.cpp and TestFoliagePlacementBehaviour.cpp use, and lifted
    // clear of any landscape a host map might carry so the only thing under the probe is this
    // test's own slope.
    constexpr double RotColX = 412700.0;
    constexpr double RotColY = 305900.0;

    // The slope actor's pivot. Its top face runs through pivot + (-25, 0, +43.3) and spans
    // roughly [pivot.X - 371, pivot.X + 321] on X and pivot.Y +/- 400 on Y once the cube is
    // scaled 8x on XY, so every painted column below lands comfortably inside it.
    constexpr double RotSlopePivotZ = 20000.0;
    // Tilt, in degrees, applied as the slope actor's PITCH. A cube pitched by P has its top-face
    // normal exactly P degrees off vertical, which is the whole fixture: an instance that ignores
    // AlignToNormal reads 0 degrees off vertical and one that honours it reads P.
    constexpr double RotSlopePitchDeg = 30.0;
    // Requested paint height: clear air above the highest point of the tilted top face, so the
    // seat solve has an unmistakable drop to make and no instance starts inside the slope.
    constexpr double RotRequestZ = RotSlopePivotZ + 1000.0;

    // Angular tolerance for the alignment assertion, in degrees. The measured normal is the mean
    // of the accepted ground normals under the footprint and every one of them comes off the same
    // flat face, so the only error is float noise and the JSON double round trip; 2 degrees is
    // far tighter than the 30 the fixture tilts by.
    constexpr double RotAngleToleranceDeg = 2.0;

    // The angular diameter a random-yaw batch must exceed. Twelve draws from a uniform 0-360 all
    // landing inside one 45-degree arc has probability ~4e-9, so this cannot flake in practice
    // while still failing outright against a constant rotator.
    constexpr int32 RotRandomYawSamples = 12;
    constexpr double RotMinYawSpreadDeg = 45.0;
    // The fixed-yaw control's permitted diameter. Every instance in that batch is aligned to the
    // same measured normal from the same flat face, so the only spread is float noise.
    constexpr double RotFixedYawToleranceDeg = 0.1;

    // The fixed-yaw control batch. Four is enough to show every instance shares one yaw.
    constexpr int32 RotFixedYawSamples = 4;

    inline UWorld* RotEditorWorld()
    {
        return GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    }

    inline FString RotUniqueName(const TCHAR* Prefix)
    {
        return FString::Printf(TEXT("PWFoliageRot_%s_%s"), Prefix,
            *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    }

    // foliage.add_type builds package "/Game/Foliage/<Name>" and names the object inside it
    // "<Name>", so the loadable object path is "/Game/Foliage/<Name>.<Name>" while asset deletion
    // takes the package path.
    inline FString RotPackagePathFor(const FString& Name)
    {
        return FString::Printf(TEXT("/Game/Foliage/%s"), *Name);
    }

    inline FString RotObjectPathFor(const FString& Name)
    {
        return FString::Printf(TEXT("/Game/Foliage/%s.%s"), *Name, *Name);
    }

    inline TSharedPtr<FJsonObject> RotVec3(double X, double Y, double Z)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetNumberField(TEXT("x"), X);
        Obj->SetNumberField(TEXT("y"), Y);
        Obj->SetNumberField(TEXT("z"), Z);
        return Obj;
    }

    inline TSharedPtr<FJsonObject> RotRotator(double Pitch, double Yaw, double Roll)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetNumberField(TEXT("pitch"), Pitch);
        Obj->SetNumberField(TEXT("yaw"), Yaw);
        Obj->SetNumberField(TEXT("roll"), Roll);
        return Obj;
    }

    // Painted columns, spread over the slope so a verb that collapsed the batch onto one point
    // could not satisfy the per-instance reads. All of them sit inside the tilted top face.
    inline TArray<TSharedPtr<FJsonValue>> RotSlopeLocations(int32 Count)
    {
        TArray<TSharedPtr<FJsonValue>> Locations;
        for (int32 Index = 0; Index < Count; ++Index)
        {
            const double OffsetX = -120.0 + 24.0 * static_cast<double>(Index);
            const double OffsetY = -120.0 + 22.0 * static_cast<double>(Index);
            Locations.Add(MakeShared<FJsonValueObject>(
                RotVec3(RotColX + OffsetX, RotColY + OffsetY, RotRequestZ)));
        }
        return Locations;
    }

    // {"preset":"any_solid"}. The automation world has no landscape at this column, so the slope
    // is an ordinary static mesh actor and any_solid is the preset that accepts it.
    inline TSharedPtr<FJsonObject> RotAnySolidSurface()
    {
        TSharedPtr<FJsonObject> Surface = MakeShared<FJsonObject>();
        Surface->SetStringField(TEXT("preset"), TEXT("any_solid"));
        return Surface;
    }

    // Editor worlds do not tick physics on their own, so a just-spawned body is absent from the
    // scene-query structure until a tick flushes it and a ground probe issued in the same call
    // would miss this test's own slope. Editor worlds do not simulate, so nothing moves.
    inline void RotFlushPhysics(UWorld* World)
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

    // The slope. Spawned through the real actor.spawn verb rather than built by hand, widened on
    // XY so an instance's 100 x 100 footprint is comfortably inside the tilted face, and pitched
    // so its top-face normal is RotSlopePitchDeg off vertical.
    inline AActor* RotSpawnSlope(FAutomationTestBase& Test, FRpcDispatcher& Dispatcher,
        DispatcherTestHelpers::FSinkPtr& Sink, UWorld* World, const FString& Label)
    {
        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("meshPath"), RotCubeMeshPath);
        Params->SetStringField(TEXT("actorName"), Label);
        Params->SetObjectField(TEXT("location"), RotVec3(RotColX, RotColY, RotSlopePivotZ));
        Params->SetObjectField(TEXT("rotation"), RotRotator(RotSlopePitchDeg, 0.0, 0.0));
        Params->SetObjectField(TEXT("scale"), RotVec3(8.0, 8.0, 1.0));

        bool bSuccess = false;
        FString ErrorCode;
        DispatcherTestHelpers::Dispatch(Dispatcher, Sink, TEXT("actor.spawn"),
            TEXT("req-foliage-rotation-slope"), Params, bSuccess, ErrorCode);
        Test.TestTrue(*FString::Printf(TEXT("the slope fixture spawned (error=%s)"), *ErrorCode),
            bSuccess);

        return McpActorUtils::FindActorByName(World, Label);
    }

    inline bool RotCreateFoliageType(FAutomationTestBase& Test, FRpcDispatcher& Dispatcher,
        DispatcherTestHelpers::FSinkPtr& Sink, const FString& Name, bool bAlignToNormal,
        bool bRandomYaw)
    {
        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("name"), Name);
        Params->SetStringField(TEXT("meshPath"), RotCubeMeshPath);
        // A degenerate [1,1] scale range keeps the instance footprint - and therefore the seat
        // solve - identical between the two types this test compares.
        Params->SetNumberField(TEXT("minScale"), 1.0);
        Params->SetNumberField(TEXT("maxScale"), 1.0);
        Params->SetBoolField(TEXT("alignToNormal"), bAlignToNormal);
        Params->SetBoolField(TEXT("randomYaw"), bRandomYaw);

        bool bSuccess = false;
        FString ErrorCode;
        DispatcherTestHelpers::Dispatch(Dispatcher, Sink, TEXT("foliage.add_type"),
            TEXT("req-foliage-rotation-add-type"), Params, bSuccess, ErrorCode);
        Test.TestTrue(*FString::Printf(TEXT("foliage.add_type created the '%s' fixture (error=%s)"),
            *Name, *ErrorCode), bSuccess);
        return bSuccess;
    }

    inline UFoliageType* RotLoadFoliageType(const FString& Name)
    {
        return LoadObject<UFoliageType>(nullptr, *RotObjectPathFor(Name));
    }

    // Type-scoped teardown: empties only this fixture's instances (a `removeAll` would wipe the
    // host map's own foliage) and then deletes the asset, which also clears the dirty /Game
    // package a later editor-wide save-all would otherwise flush into host Content.
    inline void RotDiscardType(FRpcDispatcher& Dispatcher, DispatcherTestHelpers::FSinkPtr& Sink,
        const FString& Name)
    {
        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("foliageTypePath"), RotObjectPathFor(Name));

        bool bSuccess = false;
        FString ErrorCode;
        DispatcherTestHelpers::Dispatch(Dispatcher, Sink, TEXT("foliage.remove"),
            TEXT("req-foliage-rotation-cleanup"), Params, bSuccess, ErrorCode);

        CleanupTestAsset(RotPackagePathFor(Name));
    }

    // GROUND TRUTH #1 - the foliage RECORD. Every instance the level holds for this type, read
    // off FFoliageInfo::Instances across every AInstancedFoliageActor in the world, never off a
    // handler response.
    inline int32 RotGatherInstances(UWorld* World, const UFoliageType* Type,
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
    // one instance written by different calls, so a fix that rotated the record and left the
    // component would look correct to #1 alone while the viewport showed vertical plants.
    inline bool RotReadComponentInstanceWorld(UWorld* World, const UFoliageType* Type, int32 Index,
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

    // Angle in degrees between two unit-ish vectors. The alignment assertion is expressed this
    // way rather than as a dot-product threshold so a failure message reports the tilt a reader
    // can compare against the fixture's own slope angle.
    inline double RotAngleBetweenDeg(const FVector& A, const FVector& B)
    {
        const FVector UnitA = A.GetSafeNormal();
        const FVector UnitB = B.GetSafeNormal();
        if (UnitA.IsNearlyZero() || UnitB.IsNearlyZero())
        {
            return 180.0;
        }
        return FMath::RadiansToDegrees(
            FMath::Acos(FMath::Clamp(FVector::DotProduct(UnitA, UnitB), -1.0, 1.0)));
    }

    // Largest shortest-arc angle between any two yaws in the batch, i.e. the batch's angular
    // DIAMETER. A plain max-minus-min would be wrong twice over: the aligned batch's yaws come
    // out of FQuat::Rotator() normalised to (-180, 180], so two yaws a hair apart either side of
    // 180 would read as a 360-degree spread, and the fixed-yaw control - whose yaw lands exactly
    // on that seam - is precisely the batch that would flake on it.
    inline double RotMaxPairwiseYawDeltaDeg(const TArray<FFoliageInstance>& Instances)
    {
        double Worst = 0.0;
        for (int32 A = 0; A < Instances.Num(); ++A)
        {
            for (int32 B = A + 1; B < Instances.Num(); ++B)
            {
                const double Delta = FMath::Abs(FMath::FindDeltaAngleDegrees(
                    static_cast<double>(Instances[A].Rotation.Yaw),
                    static_cast<double>(Instances[B].Rotation.Yaw)));
                Worst = FMath::Max(Worst, Delta);
            }
        }
        return Worst;
    }

    // Reads an integer response field, failing when the key is absent so a dropped field never
    // reads as a zero that silently satisfies a comparison.
    inline int32 RotRequireNumberField(FAutomationTestBase& Test,
        const TSharedPtr<FJsonObject>& Result, const TCHAR* Field)
    {
        double Value = 0.0;
        const bool bPresent = Result.IsValid() && Result->TryGetNumberField(Field, Value);
        Test.TestTrue(*FString::Printf(TEXT("response carries %s"), Field), bPresent);
        return bPresent ? static_cast<int32>(Value) : -1;
    }

    // The `rotation` block the fix added. Absent before it, which is what makes every assertion
    // built on this a hard failure against the old response rather than a soft comparison.
    inline TSharedPtr<FJsonObject> RotRequireRotationBlock(FAutomationTestBase& Test,
        const TSharedPtr<FJsonObject>& Result)
    {
        const TSharedPtr<FJsonObject>* Block = nullptr;
        const bool bPresent = Result.IsValid()
            && Result->TryGetObjectField(TEXT("rotation"), Block) && Block;
        Test.TestTrue(TEXT("the response carries a rotation block naming what the type asked for"),
            bPresent);
        return bPresent ? *Block : nullptr;
    }

    inline void RotPaint(FRpcDispatcher& Dispatcher, DispatcherTestHelpers::FSinkPtr& Sink,
        const FString& TypeName, int32 Count, bool bWithSurface, const TCHAR* RequestId,
        bool& bOutSuccess, TSharedPtr<FJsonObject>& OutResult, FString& OutErrorCode)
    {
        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("foliageTypePath"), RotObjectPathFor(TypeName));
        Params->SetArrayField(TEXT("locations"), RotSlopeLocations(Count));
        if (bWithSurface)
        {
            Params->SetObjectField(TEXT("surface"), RotAnySolidSurface());
        }
        DispatcherTestHelpers::Dispatch(Dispatcher, Sink, TEXT("foliage.paint"), RequestId,
            Params, bOutSuccess, OutResult, OutErrorCode);
    }
}

using DispatcherTestHelpers::MakeDispatcher;
using DispatcherTestHelpers::Dispatch;

// Each RunTest opens FoliagePaintRotationTestHelpers inside its own body rather than at file
// scope: a file-scope using-directive would leak into every other test .cpp that Unity merges
// after this one into the same translation unit.

// ============================================================================
// (a) On a slope, the type's AlignToNormal and RandomYaw are both honoured.
// ============================================================================

// COUNTERFACTUAL. This is the ticket itself. A `paint` that writes FRotator::ZeroRotator leaves
// every instance's +Z axis on world up, so the alignment assertion measures 30 degrees of error
// against a 2-degree tolerance, the yaw spread is exactly 0 against a 45-degree floor, and the
// `rotation` block is absent entirely. A fix that rotated the foliage record but not the HISM
// passes the record read and fails the component read. A fix that always randomised the yaw -
// rather than reading the flag - passes the first batch and fails the fixed-yaw control batch.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFoliagePaintAppliesTypeRotationTest,
    "PinWright.foliage.paint.AppliesTheTypesAlignToNormalAndRandomYaw",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FFoliagePaintAppliesTypeRotationTest::RunTest(const FString& Parameters)
{
    using namespace FoliagePaintRotationTestHelpers;

    UWorld* World = RotEditorWorld();
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no_editor_world"),
            TEXT("GEditor->GetEditorWorldContext().World() returned null; the foliage.paint "
                 "type-rotation assertions were stepped over."));
        return true;
    }

    DispatcherTestHelpers::FSinkPtr Sink;
    FRpcDispatcher Dispatcher;
    MakeDispatcher(Sink, Dispatcher);

    const FString RandomTypeName = RotUniqueName(TEXT("Random"));
    const FString FixedTypeName = RotUniqueName(TEXT("Fixed"));
    const FString SlopeLabel = RotUniqueName(TEXT("Slope"));

    AActor* Slope = RotSpawnSlope(*this, Dispatcher, Sink, World, SlopeLabel);
    ON_SCOPE_EXIT
    {
        RotDiscardType(Dispatcher, Sink, RandomTypeName);
        RotDiscardType(Dispatcher, Sink, FixedTypeName);
        if (Slope)
        {
            Slope->Destroy();
        }
    };
    if (!Slope)
    {
        AddError(TEXT("the slope fixture did not spawn, so there was no tilted surface to align "
                      "against"));
        return true;
    }
    RotFlushPhysics(World);

    // The fixture's own answer, read off the spawned actor rather than recomputed from the
    // constants: whatever actor.spawn actually applied is what the ground probe will measure.
    const FVector ExpectedNormal = Slope->GetActorQuat().RotateVector(FVector::UpVector);
    const double FixtureTiltDeg = RotAngleBetweenDeg(ExpectedNormal, FVector::UpVector);
    TestEqual(TEXT("the slope fixture is tilted by the requested angle"), FixtureTiltDeg,
        RotSlopePitchDeg, 0.5);

    // ---- Batch 1: AlignToNormal + RandomYaw ----
    if (!RotCreateFoliageType(*this, Dispatcher, Sink, RandomTypeName,
            /*bAlignToNormal*/ true, /*bRandomYaw*/ true))
    {
        return true;
    }
    UFoliageType* RandomType = RotLoadFoliageType(RandomTypeName);
    if (!RandomType)
    {
        AddError(TEXT("the random-yaw foliage type fixture could not be loaded back from its own "
                      "path"));
        return true;
    }

    bool bSuccess = false;
    FString ErrorCode;
    TSharedPtr<FJsonObject> Result;
    RotPaint(Dispatcher, Sink, RandomTypeName, RotRandomYawSamples, /*bWithSurface*/ true,
        TEXT("req-foliage-paint-rotation-random"), bSuccess, Result, ErrorCode);

    TestTrue(*FString::Printf(TEXT("foliage.paint onto the slope succeeds (error=%s)"), *ErrorCode),
        bSuccess);
    if (!bSuccess || !Result.IsValid())
    {
        return true;
    }
    TestEqual(TEXT("every requested column was placed"),
        RotRequireNumberField(*this, Result, TEXT("instancesPlaced")), RotRandomYawSamples);
    TestEqual(TEXT("a well-formed batch over the slope skips nothing"),
        RotRequireNumberField(*this, Result, TEXT("skippedCount")), 0);

    // The response must say what the type asked for AND how much of it was done.
    if (TSharedPtr<FJsonObject> RotationBlock = RotRequireRotationBlock(*this, Result))
    {
        bool bReportedAlign = false;
        TestTrue(TEXT("the rotation block reports the type's alignToNormal"),
            RotationBlock->TryGetBoolField(TEXT("alignToNormal"), bReportedAlign));
        TestTrue(TEXT("the rotation block echoes alignToNormal as enabled"), bReportedAlign);

        bool bReportedYaw = false;
        TestTrue(TEXT("the rotation block reports the type's randomYaw"),
            RotationBlock->TryGetBoolField(TEXT("randomYaw"), bReportedYaw));
        TestTrue(TEXT("the rotation block echoes randomYaw as enabled"), bReportedYaw);

        TestEqual(TEXT("alignedCount is the MEASURED number of instances actually tilted"),
            RotRequireNumberField(*this, RotationBlock, TEXT("alignedCount")),
            RotRandomYawSamples);
    }

    // GROUND TRUTH #1: the foliage record.
    TArray<FFoliageInstance> Stored;
    const int32 StoredCount = RotGatherInstances(World, RandomType, Stored);
    TestEqual(TEXT("the foliage actor holds one instance per requested column"), StoredCount,
        RotRandomYawSamples);
    if (StoredCount != RotRandomYawSamples)
    {
        return true;
    }

    int32 IdentityRotations = 0;
    double WorstAlignErrorDeg = 0.0;
    int32 MissingAlignFlag = 0;
    for (const FFoliageInstance& Inst : Stored)
    {
        if (Inst.Rotation.IsNearlyZero())
        {
            ++IdentityRotations;
        }
        const FVector InstanceUp =
            Inst.GetInstanceWorldTransform().GetUnitAxis(EAxis::Z);
        WorstAlignErrorDeg = FMath::Max(WorstAlignErrorDeg,
            RotAngleBetweenDeg(InstanceUp, ExpectedNormal));
        if ((Inst.Flags & FOLIAGE_AlignToNormal) == 0)
        {
            ++MissingAlignFlag;
        }
    }

    // THE TICKET, stated as an assertion: not one instance may be left standing vertical.
    TestEqual(TEXT("no painted instance was left at the identity rotation"), IdentityRotations, 0);
    TestTrue(*FString::Printf(
            TEXT("every instance's own +Z axis points along the measured ground normal "
                 "(worst error %.3f deg, tolerance %.1f deg, slope tilt %.1f deg)"),
            WorstAlignErrorDeg, RotAngleToleranceDeg, FixtureTiltDeg),
        WorstAlignErrorDeg <= RotAngleToleranceDeg);
    TestEqual(TEXT("every instance carries the engine's FOLIAGE_AlignToNormal flag"),
        MissingAlignFlag, 0);

    // RandomYaw, asserted as a spread rather than as exact values: foliage.paint has no seed
    // surface and the engine's own painting draws from the unseeded FMath::FRand, so the honest
    // property to pin is that the yaws differ, not what they are.
    const double YawSpreadDeg = RotMaxPairwiseYawDeltaDeg(Stored);
    TestTrue(*FString::Printf(
            TEXT("random yaw produced a spread of yaws across the batch (%.2f deg over %d "
                 "instances, floor %.1f deg)"),
            YawSpreadDeg, StoredCount, RotMinYawSpreadDeg),
        YawSpreadDeg > RotMinYawSpreadDeg);

    // GROUND TRUTH #2: the render instance the viewer sees.
    FTransform ComponentWorld;
    if (TestTrue(TEXT("the type's instanced component carries the painted instance"),
            RotReadComponentInstanceWorld(World, RandomType, 0, ComponentWorld)))
    {
        const double ComponentErrorDeg =
            RotAngleBetweenDeg(ComponentWorld.GetUnitAxis(EAxis::Z), ExpectedNormal);
        TestTrue(*FString::Printf(
                TEXT("the drawn instance was rotated with the foliage record (error %.3f deg)"),
                ComponentErrorDeg),
            ComponentErrorDeg <= RotAngleToleranceDeg);
    }

    // The per-instance readback: the response must publish the rotation it actually wrote, not
    // merely the flags the type carried.
    const TArray<TSharedPtr<FJsonValue>>* PlacedRows = nullptr;
    if (TestTrue(TEXT("the response carries a placed[] row per instance"),
            Result->TryGetArrayField(TEXT("placed"), PlacedRows) && PlacedRows
                && PlacedRows->Num() == RotRandomYawSamples))
    {
        const TSharedPtr<FJsonObject>* Row = nullptr;
        if ((*PlacedRows)[0].IsValid() && (*PlacedRows)[0]->TryGetObject(Row) && Row)
        {
            double ReportedPitch = 0.0;
            double ReportedYaw = 0.0;
            double ReportedRoll = 0.0;
            const bool bHasRotation =
                (*Row)->TryGetNumberField(TEXT("pitch"), ReportedPitch)
                && (*Row)->TryGetNumberField(TEXT("yaw"), ReportedYaw)
                && (*Row)->TryGetNumberField(TEXT("roll"), ReportedRoll);
            TestTrue(TEXT("a placed[] row carries the pitch, yaw and roll actually written"),
                bHasRotation);
            if (bHasRotation)
            {
                TestEqual(TEXT("the reported pitch is the stored instance's pitch"),
                    ReportedPitch, static_cast<double>(Stored[0].Rotation.Pitch), 0.01);
                TestEqual(TEXT("the reported yaw is the stored instance's yaw"),
                    ReportedYaw, static_cast<double>(Stored[0].Rotation.Yaw), 0.01);
                TestEqual(TEXT("the reported roll is the stored instance's roll"),
                    ReportedRoll, static_cast<double>(Stored[0].Rotation.Roll), 0.01);
            }
            bool bReportedAligned = false;
            TestTrue(TEXT("a placed[] row states whether the instance was tilted to the ground"),
                (*Row)->TryGetBoolField(TEXT("alignedToNormal"), bReportedAligned));
            TestTrue(TEXT("the row reports the instance as aligned"), bReportedAligned);
        }
    }

    // ---- Batch 2: the control. AlignToNormal on, RandomYaw OFF ----
    // Without this batch, a handler that ignored the flag and always randomised would pass every
    // assertion above.
    if (!RotCreateFoliageType(*this, Dispatcher, Sink, FixedTypeName,
            /*bAlignToNormal*/ true, /*bRandomYaw*/ false))
    {
        return true;
    }
    UFoliageType* FixedType = RotLoadFoliageType(FixedTypeName);
    if (!FixedType)
    {
        AddError(TEXT("the fixed-yaw foliage type fixture could not be loaded back from its own "
                      "path"));
        return true;
    }

    bool bFixedSuccess = false;
    FString FixedErrorCode;
    TSharedPtr<FJsonObject> FixedResult;
    RotPaint(Dispatcher, Sink, FixedTypeName, RotFixedYawSamples, /*bWithSurface*/ true,
        TEXT("req-foliage-paint-rotation-fixed"), bFixedSuccess, FixedResult, FixedErrorCode);
    TestTrue(*FString::Printf(TEXT("foliage.paint of the fixed-yaw type succeeds (error=%s)"),
        *FixedErrorCode), bFixedSuccess);
    if (!bFixedSuccess)
    {
        return true;
    }

    TArray<FFoliageInstance> FixedStored;
    const int32 FixedCount = RotGatherInstances(World, FixedType, FixedStored);
    TestEqual(TEXT("the fixed-yaw batch reached the foliage actor"), FixedCount,
        RotFixedYawSamples);
    if (FixedCount != RotFixedYawSamples)
    {
        return true;
    }

    const double FixedYawSpreadDeg = RotMaxPairwiseYawDeltaDeg(FixedStored);
    TestTrue(*FString::Printf(
            TEXT("randomYaw:false produces ONE yaw across the batch (spread %.4f deg, "
                 "tolerance %.2f deg)"),
            FixedYawSpreadDeg, RotFixedYawToleranceDeg),
        FixedYawSpreadDeg <= RotFixedYawToleranceDeg);

    int32 MissingNoRandomYawFlag = 0;
    double FixedWorstAlignErrorDeg = 0.0;
    for (const FFoliageInstance& Inst : FixedStored)
    {
        if ((Inst.Flags & FOLIAGE_NoRandomYaw) == 0)
        {
            ++MissingNoRandomYawFlag;
        }
        FixedWorstAlignErrorDeg = FMath::Max(FixedWorstAlignErrorDeg,
            RotAngleBetweenDeg(Inst.GetInstanceWorldTransform().GetUnitAxis(EAxis::Z),
                ExpectedNormal));
    }
    // FOLIAGE_NoRandomYaw is what stops a later reapply from re-randomising a deliberately fixed
    // yaw, so its absence is a real defect rather than a cosmetic one.
    TestEqual(TEXT("a fixed-yaw instance carries FOLIAGE_NoRandomYaw"), MissingNoRandomYawFlag, 0);
    TestTrue(*FString::Printf(
            TEXT("a fixed-yaw instance is still aligned to the slope (worst error %.3f deg)"),
            FixedWorstAlignErrorDeg),
        FixedWorstAlignErrorDeg <= RotAngleToleranceDeg);

    return true;
}

// ============================================================================
// (b) Without a surface there is no normal, and the response says so.
// ============================================================================

// COUNTERFACTUAL. Before the fix the literal branch wrote the same zero rotator, so the yaws are
// all exactly 0 and the `rotation` block does not exist. A fix that applied random yaw but stayed
// silent about the dropped AlignToNormal passes the yaw assertion and fails the warning one -
// which is the point: ignoring a type flag is only acceptable with a stated reason.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFoliagePaintUnhonouredAlignIsReportedTest,
    "PinWright.foliage.paint.UnhonouredAlignToNormalIsReportedNotDropped",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FFoliagePaintUnhonouredAlignIsReportedTest::RunTest(const FString& Parameters)
{
    using namespace FoliagePaintRotationTestHelpers;

    UWorld* World = RotEditorWorld();
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no_editor_world"),
            TEXT("GEditor->GetEditorWorldContext().World() returned null; the foliage.paint "
                 "unhonoured-alignment assertions were stepped over."));
        return true;
    }

    DispatcherTestHelpers::FSinkPtr Sink;
    FRpcDispatcher Dispatcher;
    MakeDispatcher(Sink, Dispatcher);

    const FString TypeName = RotUniqueName(TEXT("NoSurface"));
    ON_SCOPE_EXIT { RotDiscardType(Dispatcher, Sink, TypeName); };

    if (!RotCreateFoliageType(*this, Dispatcher, Sink, TypeName,
            /*bAlignToNormal*/ true, /*bRandomYaw*/ true))
    {
        return true;
    }
    UFoliageType* Type = RotLoadFoliageType(TypeName);
    if (!Type)
    {
        AddError(TEXT("the foliage type fixture could not be loaded back from its own path"));
        return true;
    }

    // No slope actor and no `surface`: this is the branch that has no normal to align to, and the
    // one the ticket says must state that rather than silently dropping the flag.
    bool bSuccess = false;
    FString ErrorCode;
    TSharedPtr<FJsonObject> Result;
    RotPaint(Dispatcher, Sink, TypeName, RotRandomYawSamples, /*bWithSurface*/ false,
        TEXT("req-foliage-paint-rotation-literal"), bSuccess, Result, ErrorCode);

    TestTrue(*FString::Printf(TEXT("foliage.paint without a surface still succeeds (error=%s)"),
        *ErrorCode), bSuccess);
    if (!bSuccess || !Result.IsValid())
    {
        return true;
    }

    if (TSharedPtr<FJsonObject> RotationBlock = RotRequireRotationBlock(*this, Result))
    {
        TestEqual(TEXT("alignedCount is 0 - the flag was asked for and NOT applied"),
            RotRequireNumberField(*this, RotationBlock, TEXT("alignedCount")), 0);
    }

    // The reason, in the same warnings[] channel the branch already uses to report
    // projected:false. The projection warning stays first; the alignment one is appended.
    const TArray<TSharedPtr<FJsonValue>>* Warnings = nullptr;
    bool bNamesAlignToNormal = false;
    if (TestTrue(TEXT("the response carries a warnings[] array"),
            Result->TryGetArrayField(TEXT("warnings"), Warnings) && Warnings
                && Warnings->Num() > 0))
    {
        for (const TSharedPtr<FJsonValue>& Warning : *Warnings)
        {
            const FString Text = Warning.IsValid() ? Warning->AsString() : FString();
            if (Text.Contains(TEXT("alignToNormal")))
            {
                bNamesAlignToNormal = true;
            }
        }
    }
    TestTrue(TEXT("a warning names the alignToNormal the verb could not honour"),
        bNamesAlignToNormal);

    // Random yaw needs no surface, so it must still have been applied on this branch.
    TArray<FFoliageInstance> Stored;
    const int32 StoredCount = RotGatherInstances(World, Type, Stored);
    TestEqual(TEXT("the foliage actor holds one instance per requested column"), StoredCount,
        RotRandomYawSamples);
    if (StoredCount != RotRandomYawSamples)
    {
        return true;
    }

    const double YawSpreadDeg = RotMaxPairwiseYawDeltaDeg(Stored);
    TestTrue(*FString::Printf(
            TEXT("random yaw is applied on the literal branch too (%.2f deg spread over %d "
                 "instances, floor %.1f deg)"),
            YawSpreadDeg, StoredCount, RotMinYawSpreadDeg),
        YawSpreadDeg > RotMinYawSpreadDeg);

    // Nothing was tilted, because nothing was measured - the honest counterpart to the warning.
    int32 AlignedFlagCount = 0;
    for (const FFoliageInstance& Inst : Stored)
    {
        if ((Inst.Flags & FOLIAGE_AlignToNormal) != 0)
        {
            ++AlignedFlagCount;
        }
    }
    TestEqual(TEXT("no instance claims to have been aligned to a normal that was never measured"),
        AlignedFlagCount, 0);

    return true;
}
