// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression test for B-duplicate-along-spline-scalevariation-clobbers-base-scale.
//
// geometry.duplicate_along_spline places `count` duplicates of a source
// DynamicMeshActor along a spline. When scaleVariation>0 the handler used to call
// NewActor->SetActorScale3D(FVector(ScaleFactor)) — an ABSOLUTE uniform scale built
// from 1.0 that never read the source's own scale. So a non-uniform template
// (e.g. a thin bollard scaled (0.35,0.35,1)) came out as fat uniform ~1.0 copies:
// the template's proportions were silently discarded. Perversely, scaleVariation:0
// PRESERVED the template scale (DuplicateActor copies it) while turning the variation
// feature ON destroyed it.
//
// The fix multiplies the variation onto the source's base scale:
// SetActorScale3D(SourceActor->GetActorScale3D() * ScaleFactor). Multiplicative
// scaling preserves the source's per-axis ratios EXACTLY (the random factor cancels
// in Z/X), so the invariant below is deterministic regardless of the RNG.
//
// Strategy (exercises the production handler end-to-end via the real dispatcher):
//   1. Build the fixture in-code: geometry.create_box a source DynamicMeshActor, then
//      force its actor scale to a non-uniform (0.35,0.35,1) so Z/X == 1/0.35 (~2.857).
//   2. spline.create_spline_actor a 3-point spline with non-zero length.
//   3. geometry.duplicate_along_spline count=4, scaleVariation=0.15.
//   4. Read each <src>_DupN duplicate's actor scale and assert its Z/X ratio still
//      equals the source's (~2.857) and that it is NOT uniform.
//
// Counterfactual: reverting the fix to SetActorScale3D(FVector(ScaleFactor)) makes
// every duplicate uniform (X==Y==Z ~= 1.0), so Z/X collapses to 1.0 and both the
// "ratio preserved" and "non-uniform" assertions fail.
#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Dispatch/RpcDispatcher.h"
#include "Tests/Infra/DispatcherTestHelpers.h"
#include "Tests/Geometry/GeometryTestHelpers.h"

#include "Editor.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Engine/Level.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "LevelUtils.h"

using DispatcherTestHelpers::MakeDispatcher;
using DispatcherTestHelpers::Dispatch;

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryDuplicateAlongSplineScaleVariationTest,
    "PinWright.geometry.duplicate_along_spline.ScaleVariationPreservesSourceScale",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGeometryDuplicateAlongSplineScaleVariationTest::RunTest(const FString& Parameters)
{
    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!GEditor || !IsValid(World))
    {
        // EditorContext tests run with a live editor world; a missing world is a
        // failure to exercise the fixture, not a skip.
        AddError(TEXT("No editor world available; cannot exercise geometry.duplicate_along_spline"));
        return true;
    }

    DispatcherTestHelpers::FSinkPtr Sink;
    FRpcDispatcher Dispatcher;
    MakeDispatcher(Sink, Dispatcher);

    const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString SourceLabel = FString::Printf(TEXT("PW_DupSplineScale_Src_%s"), *Suffix);
    const FString SplineLabel = FString::Printf(TEXT("PW_DupSplineScale_Spline_%s"), *Suffix);

    // --- 1. Source DynamicMeshActor with a NON-UNIFORM, non-1.0 scale ---
    {
        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("name"), SourceLabel);
        Params->SetNumberField(TEXT("width"), 40.0);
        Params->SetNumberField(TEXT("height"), 40.0);
        Params->SetNumberField(TEXT("depth"), 100.0);
        bool bCreated = false;
        FString Err;
        Dispatch(Dispatcher, Sink, TEXT("geometry.create_box"),
            TEXT("req-dupspline-src"), Params, bCreated, Err);
        if (!TestTrue(TEXT("geometry.create_box created the source actor"), bCreated))
        {
            return true;
        }
    }

    AActor* Source = GeometryTestHelpers::FindActorByLabel(SourceLabel);
    if (!TestNotNull(TEXT("source actor is present in the world"), Source))
    {
        GeometryTestHelpers::DestroyActorsWithLabel(SourceLabel);
        return true;
    }
    // Force the template's actor scale directly (authoritative — this is the exact
    // scale DuplicateActor copies and the fix reads). Z/X == 1/0.35 ~= 2.857.
    Source->SetActorScale3D(FVector(0.35, 0.35, 1.0));
    const FVector SourceScale = Source->GetActorScale3D();
    if (!TestTrue(TEXT("source scale is non-uniform with X>0 (fixture precondition)"),
            SourceScale.X > KINDA_SMALL_NUMBER
            && !FMath::IsNearlyEqual(SourceScale.X, SourceScale.Z, 0.05)))
    {
        GeometryTestHelpers::DestroyActorsWithLabel(SourceLabel);
        return true;
    }
    const double ExpectedRatio = SourceScale.Z / SourceScale.X; // ~2.857

    // --- 2. Spline actor with a non-zero length ---
    {
        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("actorName"), SplineLabel);
        TArray<TSharedPtr<FJsonValue>> Points;
        auto MakePoint = [](double X, double Y, double Z)
        {
            TSharedPtr<FJsonObject> Loc = MakeShared<FJsonObject>();
            Loc->SetNumberField(TEXT("x"), X);
            Loc->SetNumberField(TEXT("y"), Y);
            Loc->SetNumberField(TEXT("z"), Z);
            TSharedPtr<FJsonObject> Pt = MakeShared<FJsonObject>();
            Pt->SetObjectField(TEXT("location"), Loc);
            return MakeShared<FJsonValueObject>(Pt);
        };
        Points.Add(MakePoint(0.0, 0.0, 0.0));
        Points.Add(MakePoint(300.0, 0.0, 0.0));
        Points.Add(MakePoint(600.0, 200.0, 0.0));
        Params->SetArrayField(TEXT("points"), Points);
        bool bCreated = false;
        FString Err;
        Dispatch(Dispatcher, Sink, TEXT("spline.create_spline_actor"),
            TEXT("req-dupspline-spline"), Params, bCreated, Err);
        if (!TestTrue(TEXT("spline.create_spline_actor created the spline"), bCreated))
        {
            GeometryTestHelpers::DestroyActorsWithLabel(SourceLabel);
            GeometryTestHelpers::DestroyActorsWithLabel(SplineLabel);
            return true;
        }
    }

    // The production handler duplicates via UEditorActorSubsystem::DuplicateActor,
    // whose DuplicateActorsToLevel aborts (returning null, no copies) when the source
    // actor's level is locked. In the headless automation editor that level can be
    // locked either because its package is read-only on disk (guarded by
    // GEngine->bLockReadOnlyLevels) or because ULevel::bLocked is set. Clear BOTH so
    // the real duplicate path runs, and restore the original state afterward.
    ULevel* SrcLevel = Source->GetLevel();
    const bool bPrevLockReadOnly = GEngine ? (bool)GEngine->bLockReadOnlyLevels : false;
    if (GEngine)
    {
        GEngine->bLockReadOnlyLevels = false;
    }
    const bool bLevelWasLocked = (SrcLevel != nullptr) && FLevelUtils::IsLevelLocked(SrcLevel);
    if (bLevelWasLocked)
    {
        FLevelUtils::ToggleLevelLock(SrcLevel);
    }
    TestFalse(TEXT("source level is unlocked so the real DuplicateActor path can run"),
        SrcLevel != nullptr && FLevelUtils::IsLevelLocked(SrcLevel));

    // --- 3. Duplicate along the spline WITH scale variation ---
    const int32 Count = 4;
    {
        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("actorName"), SourceLabel);
        Params->SetStringField(TEXT("splineActorName"), SplineLabel);
        Params->SetNumberField(TEXT("count"), Count);
        Params->SetBoolField(TEXT("alignToSpline"), false);
        Params->SetNumberField(TEXT("scaleVariation"), 0.15);
        bool bSuccess = false;
        FString ErrorCode;
        TSharedPtr<FJsonObject> Result;
        Dispatch(Dispatcher, Sink, TEXT("geometry.duplicate_along_spline"),
            TEXT("req-dupspline-run"), Params, bSuccess, Result, ErrorCode);
        TestTrue(TEXT("geometry.duplicate_along_spline succeeded"), bSuccess);
    }

    // --- 4. Every duplicate must keep the source's proportions ---
    int32 Found = 0;
    for (int32 i = 0; i < Count; ++i)
    {
        AActor* Dup = GeometryTestHelpers::FindActorByLabel(FString::Printf(TEXT("%s_Dup%d"), *SourceLabel, i));
        if (!Dup)
        {
            continue;
        }
        ++Found;
        const FVector S = Dup->GetActorScale3D();

        // X and Y are both the source X (0.35) times the same random factor.
        TestTrue(FString::Printf(TEXT("Dup%d X~=Y (both source-X * factor)"), i),
            FMath::IsNearlyEqual(S.X, S.Y, 0.02));

        // KEY anti-regression assertion: Z/X still equals the SOURCE Z/X (~2.857).
        // The random factor cancels, so this is exact under the fix; the bug collapses
        // it to a uniform 1.0.
        const double Ratio = (S.X > KINDA_SMALL_NUMBER) ? (S.Z / S.X) : 0.0;
        TestTrue(FString::Printf(
            TEXT("Dup%d preserves source Z/X ratio (got %.4f, expected ~%.4f); the pre-fix ")
            TEXT("absolute FVector(ScaleFactor) collapses it to uniform ~1.0"),
            i, Ratio, ExpectedRatio),
            FMath::IsNearlyEqual(Ratio, ExpectedRatio, 0.1));

        // Directly the bug signature: the copy must NOT be uniform.
        TestTrue(FString::Printf(TEXT("Dup%d scale is non-uniform (bug yields uniform ~1.0)"), i),
            !FMath::IsNearlyEqual(S.X, S.Z, 0.05));
    }
    TestEqual(TEXT("all requested duplicates were created"), Found, Count);

    // Restore the level's original lock state and the read-only-levels flag.
    if (bLevelWasLocked && SrcLevel && !FLevelUtils::IsLevelLocked(SrcLevel))
    {
        FLevelUtils::ToggleLevelLock(SrcLevel);
    }
    if (GEngine)
    {
        GEngine->bLockReadOnlyLevels = bPrevLockReadOnly;
    }

    // --- Cleanup: leave no residue on the mutated fuzzing host ---
    GeometryTestHelpers::DestroyActorsWithLabel(SourceLabel);
    for (int32 i = 0; i < Count; ++i)
    {
        GeometryTestHelpers::DestroyActorsWithLabel(FString::Printf(TEXT("%s_Dup%d"), *SourceLabel, i));
    }
    GeometryTestHelpers::DestroyActorsWithLabel(SplineLabel);

    return true;
}
