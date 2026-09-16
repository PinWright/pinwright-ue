// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression test for "river width set through spline.set_spline_point_scale survives
// read-back and save, then reverts on load".
//
// THE DEFECT. UE Water stores a river's width and depth in UWaterSplineMetadata
// (RiverWidth / Depth) and DERIVES the spline point scale from them:
// UWaterSplineComponent::SynchronizeWaterProperties assigns Scale.X = RiverWidth
// (WaterSplineComponent.cpp:231-236) and Scale.Y = Depth (:238-244) and writes the result
// back with SetScaleAtSplinePoint (:246). PostLoad calls it (:26-39), as do PostDuplicate
// (:41-53), PostEditChangeProperty (:152-163) and PostEditImport (:165-172).
//
// So writing Scale.X was a write to a derived value. It read back at the requested
// number, it saved into the map package, and it was recomputed away on the next level
// load — invisible to every check short of a reload. One recorded session set a river to
// 3490.6, verified the read-back, saved, committed, and found 4800 on the next load.
//
// THE GATE. USplineComponent::AllowsSplinePointScaleEditing (SplineComponent.h:425)
// defaults to true; UWaterSplineComponent overrides it to false
// (WaterSplineComponent.h:54) precisely because its scale is not the authority. The verb
// now refuses on that engine predicate rather than on a hand-maintained type list.
//
// FAILURE DIRECTION. Every assertion here is written so it breaks if the verb goes back
// to reporting success:
//   * spline.set_spline_point_scale on a river must FAIL, with DERIVED_PROPERTY and a
//     payload naming the authoritative verb.
//   * The refused call must leave Scale.X exactly as it found it — a "refusal" that still
//     wrote would be worse than the original bug.
//   * water.set_river_width_at_spline_point must move Scale.X, read off the COMPONENT and
//     not off the response, because Scale.X is written by the engine's own
//     SynchronizeWaterProperties and is therefore the half of the check the handler
//     cannot fake.
//
// Deliberately no Water headers here: the fixture is built through the production
// water.* verbs and inspected through the plain USplineComponent API, so this test
// compiles on a host where the Water plugin is disabled and skips itself at runtime when
// water.spawn_water_body reports WATER_PLUGIN_NOT_AVAILABLE.
#include "Misc/AutomationTest.h"
#include "Components/SplineComponent.h"
#include "Dom/JsonObject.h"
#include "Editor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "Misc/Guid.h"
#include "Tests/TestSkipReporting.h"
#include "Tests/TestUtils.h"
#include "Tests/TestWorldUtils.h"

namespace
{
    AActor* FindActorForWaterSplineTest(UWorld* World, const FString& Label)
    {
        if (!World)
        {
            return nullptr;
        }
        for (TActorIterator<AActor> It(World); It; ++It)
        {
            AActor* Actor = *It;
            if (Actor &&
                (Actor->GetActorLabel().Equals(Label, ESearchCase::IgnoreCase) ||
                 Actor->GetName().Equals(Label, ESearchCase::IgnoreCase)))
            {
                return Actor;
            }
        }
        return nullptr;
    }

    USplineComponent* FindSplineForWaterSplineTest(AActor* Actor)
    {
        if (!Actor)
        {
            return nullptr;
        }
        TArray<USplineComponent*> Splines;
        Actor->GetComponents<USplineComponent>(Splines);
        return Splines.Num() > 0 ? Splines[0] : nullptr;
    }

    // Reads a string out of the nested derivedWrite block, or empty when the block is
    // missing — so a response with no derivedWrite fails the assertion rather than
    // silently matching a default.
    FString ReadDerivedWriteString(const TSharedPtr<FJsonObject>& Result, const TCHAR* Field)
    {
        const TSharedPtr<FJsonObject>* Block = nullptr;
        if (!Result.IsValid() || !Result->TryGetObjectField(TEXT("derivedWrite"), Block) ||
            !Block || !(*Block).IsValid())
        {
            return FString();
        }
        FString Value;
        (*Block)->TryGetStringField(Field, Value);
        return Value;
    }
}

// ---- a water spline's point scale is derived, and the verbs say so ----
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWaterSplineScaleIsDerivedTest,
    "PinWright.spline.set_spline_point_scale.RefusesDerivedWaterSplineScale",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWaterSplineScaleIsDerivedTest::RunTest(const FString& Parameters)
{
    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("Editor world not available — skipping water spline derived-scale test"));
        return true;
    }

    TestTrue(TEXT("spline.set_spline_point_scale handler registered"),
        IsHandlerRegistered(TEXT("spline.set_spline_point_scale")));
    if (!TestTrue(TEXT("water.set_river_width_at_spline_point handler registered"),
            IsHandlerRegistered(TEXT("water.set_river_width_at_spline_point"))))
    {
        return false;
    }

    const FString RiverLabel = FString::Printf(TEXT("PW_DerivedScale_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));

    // Destroys the spawned river and restores the level dirty flag on scope exit.
    FScopedEditorWorldActorGuard WorldGuard;

    // --- Fixture: a river, spawned through the production verb. ---
    {
        TSharedPtr<FJsonObject> SpawnPayload = MakeShared<FJsonObject>();
        SpawnPayload->SetStringField(TEXT("type"), TEXT("River"));
        SpawnPayload->SetStringField(TEXT("name"), RiverLabel);

        FTestResponseCapture SpawnCapture;
        if (!TestTrue(TEXT("water.spawn_water_body invoked"),
                InvokeHandlerWithCapture(TEXT("water.spawn_water_body"), SpawnPayload, SpawnCapture)))
        {
            return false;
        }
        if (!SpawnCapture.bSuccess)
        {
            // A host with the Water plugin disabled compiles the handler bodies out and
            // answers WATER_PLUGIN_NOT_AVAILABLE. That is an environment fact, not a
            // regression — every other failure is a real failure.
            if (SpawnCapture.ErrorCode == TEXT("WATER_PLUGIN_NOT_AVAILABLE"))
            {
                PinWrightTestSkip::SkipAssertions(*this, TEXT("water-plugin-unavailable"),
                    TEXT("Water plugin not available — skipping water spline derived-scale test"));
                return true;
            }
            TestTrue(FString::Printf(TEXT("water.spawn_water_body succeeded (got %s: %s)"),
                *SpawnCapture.ErrorCode, *SpawnCapture.Message), false);
            return false;
        }
    }

    AActor* River = FindActorForWaterSplineTest(World, RiverLabel);
    if (!TestNotNull(TEXT("spawned river located in the editor world"), River))
    {
        return false;
    }

    USplineComponent* Spline = FindSplineForWaterSplineTest(River);
    if (!TestNotNull(TEXT("river has a spline component"), Spline))
    {
        return false;
    }
    if (!TestTrue(TEXT("river spline has at least one point"),
            Spline->GetNumberOfSplinePoints() > 0))
    {
        return false;
    }

    // The gate this fix hangs on. If a future engine drops the override, the refusal
    // below stops firing and this states why.
    if (!TestFalse(TEXT("engine reports the water spline's point scale as not editable "
                        "(USplineComponent::AllowsSplinePointScaleEditing)"),
            Spline->AllowsSplinePointScaleEditing()))
    {
        return false;
    }

    const double ScaleXBefore = Spline->GetScaleAtSplinePoint(0).X;
    // Pick a target that cannot coincide with the current value, so "unchanged" and
    // "changed to the request" are never the same number.
    const double TargetWidth = ScaleXBefore + 1234.5;

    // --- Case 1 (failure direction): the derived write must be refused. ---
    {
        TSharedPtr<FJsonObject> ScalePayload = MakeShared<FJsonObject>();
        ScalePayload->SetStringField(TEXT("actorName"), RiverLabel);
        ScalePayload->SetNumberField(TEXT("pointIndex"), 0);

        TSharedPtr<FJsonObject> ScaleVec = MakeShared<FJsonObject>();
        ScaleVec->SetNumberField(TEXT("x"), TargetWidth);
        ScaleVec->SetNumberField(TEXT("y"), Spline->GetScaleAtSplinePoint(0).Y);
        ScaleVec->SetNumberField(TEXT("z"), 1.0);
        ScalePayload->SetObjectField(TEXT("pointScale"), ScaleVec);

        FTestResponseCapture ScaleCapture;
        if (!TestTrue(TEXT("spline.set_spline_point_scale invoked"),
                InvokeHandlerWithCapture(
                    TEXT("spline.set_spline_point_scale"), ScalePayload, ScaleCapture)))
        {
            return false;
        }
        TestTrue(TEXT("spline.set_spline_point_scale responded"), ScaleCapture.bWasCalled);

        // The whole point: reporting success here is the defect.
        TestFalse(TEXT("spline.set_spline_point_scale must NOT report success for a scale "
                       "write the engine re-derives from water spline metadata"),
            ScaleCapture.bSuccess);
        TestEqual(TEXT("refusal uses the registered DERIVED_PROPERTY code"),
            ScaleCapture.ErrorCode, FString(TEXT("DERIVED_PROPERTY")));

        // A refusal that does not name a reachable remedy is barely better than a
        // silent failure (rpc-design.md §7).
        TestEqual(TEXT("refusal names the authoritative verb in the structured payload"),
            ReadDerivedWriteString(ScaleCapture.Result, TEXT("authoritativeVerb")),
            FString(TEXT("water.set_river_width_at_spline_point")));
        TestTrue(TEXT("refusal names the state the value is derived from"),
            ReadDerivedWriteString(ScaleCapture.Result, TEXT("derivedFrom")).Contains(TEXT("RiverWidth")));

        // The refusal must be total: nothing written.
        TestTrue(TEXT("the refused call left the spline point scale untouched"),
            FMath::IsNearlyEqual(Spline->GetScaleAtSplinePoint(0).X, ScaleXBefore, 0.01));
    }

    // --- Case 2: the authoritative verb moves the value the engine derives from. ---
    {
        TSharedPtr<FJsonObject> WidthPayload = MakeShared<FJsonObject>();
        WidthPayload->SetStringField(TEXT("actor"), RiverLabel);
        WidthPayload->SetNumberField(TEXT("width"), TargetWidth);
        WidthPayload->SetNumberField(TEXT("pointIndex"), 0);

        FTestResponseCapture WidthCapture;
        if (!TestTrue(TEXT("water.set_river_width_at_spline_point invoked"),
                InvokeHandlerWithCapture(
                    TEXT("water.set_river_width_at_spline_point"), WidthPayload, WidthCapture)))
        {
            return false;
        }
        TestTrue(TEXT("water.set_river_width_at_spline_point responded"), WidthCapture.bWasCalled);
        TestTrue(FString::Printf(TEXT("water.set_river_width_at_spline_point succeeded (got %s: %s)"),
                *WidthCapture.ErrorCode, *WidthCapture.Message),
            WidthCapture.bSuccess);
        if (!WidthCapture.bSuccess || !WidthCapture.Result.IsValid())
        {
            return false;
        }

        bool bApplied = false;
        WidthCapture.Result->TryGetBoolField(TEXT("applied"), bApplied);
        TestTrue(TEXT("the authoritative verb reports the point applied"), bApplied);

        // Measured off the component, not off the response. Scale.X here was written by
        // UWaterSplineComponent::SynchronizeWaterProperties from the metadata this verb
        // set — the handler never assigns it, so it cannot fake this number.
        TestTrue(TEXT("the engine re-derived Scale.X from the metadata the verb wrote, so "
                      "the value now survives a reload"),
            FMath::IsNearlyEqual(Spline->GetScaleAtSplinePoint(0).X, TargetWidth, 0.01));
    }

    return true;
}
