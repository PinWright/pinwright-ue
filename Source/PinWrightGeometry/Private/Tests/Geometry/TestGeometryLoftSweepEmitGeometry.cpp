// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression test for B-geometry-loft-silent-empty-mesh (reworded to cover the
// whole geometry.loft + geometry.sweep sweep-stub family).
//
// geometry.loft and geometry.sweep both build a TArray<FVector2D> cross-section
// polygon correctly, then copied it into a DEFAULT-CONSTRUCTED
// FGeometryScriptSimplePolygon before sweeping. That engine struct's Vertices
// member is a TSharedPtr<TArray<FVector2D>> that stays null until Reset() allocates
// it (GeometryScriptTypes.h), so every `if (Polygon.Vertices.IsValid()) Add(...)`
// guard was false and the polygon was handed over EMPTY. Two of the three code
// paths were worse still: the loft no-profile branch and all of geometry.sweep
// never even called AppendSweepPolygon (they only UE_LOG'd), so those paths
// appended nothing regardless. In every case the handler still returned
// SendSuccess with profilesUsed / a sweepStatus string, leaving the caller with an
// empty mesh and a false success (trianglesAfter == trianglesBefore).
//
// The fix drops the null round-trip and passes the built PolygonVertices straight
// into AppendSweepPolygon (the pattern the spline-sweep handler in the same file
// already uses), and wires AppendSweepPolygon into the two branches that previously
// only logged.
//
// Strategy (exercises the production handlers end-to-end via the real dispatcher):
//   Phase A - multi-profile loft: create an empty target + two sphere profiles at
//     distinct Z locations (so the loft path length is non-zero), loft between
//     them, assert profilesUsed == 2 and trianglesAfter > 0.
//   Phase B - sweep: create an empty target, sweep with no spline (the linear
//     vertical fallback), assert trianglesAfter > 0.
//   Phase C - no-profile loft: create a box target, loft with no profiles (the
//     bounding-box extrusion branch), assert it APPENDED geometry
//     (trianglesAfter > trianglesBefore).
//
// Counterfactual: reverting any of the three fixes returns that phase to a silent
// empty success - the corresponding trianglesAfter assertion (> 0, or strictly
// greater than trianglesBefore) fails. Fixtures are built entirely in-code via the
// production create verbs; no external content is loaded.
#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Dispatch/RpcDispatcher.h"
#include "Tests/Infra/DispatcherTestHelpers.h"
#include "Tests/Geometry/GeometryTestHelpers.h"
#include "Tests/TestSkipReporting.h"

#include "Editor.h"
#include "Engine/World.h"

using DispatcherTestHelpers::MakeDispatcher;
using DispatcherTestHelpers::Dispatch;

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryLoftSweepEmitGeometryTest,
    "PinWright.geometry.LoftSweepEmitGeometryNotSilentEmpty",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGeometryLoftSweepEmitGeometryTest::RunTest(const FString& Parameters)
{
    if (!GEditor || !IsValid(GEditor->GetEditorWorldContext().World()))
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("No editor world available; skipping loft/sweep emit-geometry test"));
        return true;
    }

    const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits);

    DispatcherTestHelpers::FSinkPtr Sink;
    FRpcDispatcher Dispatcher;
    MakeDispatcher(Sink, Dispatcher);

    TArray<FString> SpawnedLabels;
    auto Cleanup = [&SpawnedLabels]()
    {
        for (const FString& L : SpawnedLabels)
        {
            GeometryTestHelpers::DestroyActorsWithLabel(L);
        }
    };

    // Spawn a DynamicMeshActor via a create verb; a failed spawn is a test FAILURE
    // (not a skip) because these in-code fixtures are required.
    auto CreateActor = [&](const TCHAR* Method, const TSharedPtr<FJsonObject>& Params,
        const FString& Label, const TCHAR* What) -> bool
    {
        SpawnedLabels.Add(Label);
        bool bOk = false;
        FString Err;
        Dispatch(Dispatcher, Sink, Method,
            FString::Printf(TEXT("req-create-%s"), What), Params, bOk, Err);
        return TestTrue(FString::Printf(TEXT("%s create verb spawned its fixture actor"), What), bOk);
    };

    // ---- Phase A: multi-profile loft emits a swept surface ----
    {
        const FString Target = FString::Printf(TEXT("PW_LoftTarget_%s"), *Suffix);
        const FString Prof0 = FString::Printf(TEXT("PW_LoftProfA_%s"), *Suffix);
        const FString Prof1 = FString::Printf(TEXT("PW_LoftProfB_%s"), *Suffix);

        TSharedPtr<FJsonObject> TP = MakeShared<FJsonObject>();
        TP->SetStringField(TEXT("name"), Target);
        if (CreateActor(TEXT("geometry.create_procedural_mesh"), TP, Target, TEXT("loft-target")))
        {
            // Two sphere profiles at distinct Z so StartPos != EndPos and the loft's
            // PathLength > KINDA_SMALL_NUMBER guard passes.
            auto MakeProfile = [&](const FString& Label, double Z) -> bool
            {
                TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
                P->SetStringField(TEXT("name"), Label);
                P->SetNumberField(TEXT("radius"), 40.0);
                TSharedPtr<FJsonObject> Loc = MakeShared<FJsonObject>();
                Loc->SetNumberField(TEXT("x"), 0.0);
                Loc->SetNumberField(TEXT("y"), 0.0);
                Loc->SetNumberField(TEXT("z"), Z);
                P->SetObjectField(TEXT("location"), Loc);
                return CreateActor(TEXT("geometry.create_sphere"), P, Label, TEXT("loft-profile"));
            };

            const bool bP0 = MakeProfile(Prof0, 0.0);
            const bool bP1 = MakeProfile(Prof1, 200.0);
            if (bP0 && bP1)
            {
                TSharedPtr<FJsonObject> LP = MakeShared<FJsonObject>();
                LP->SetStringField(TEXT("actorName"), Target);
                TArray<TSharedPtr<FJsonValue>> Profiles;
                Profiles.Add(MakeShared<FJsonValueString>(Prof0));
                Profiles.Add(MakeShared<FJsonValueString>(Prof1));
                LP->SetArrayField(TEXT("profileActors"), Profiles);
                LP->SetNumberField(TEXT("subdivisions"), 8);

                bool bLoft = false;
                FString LoftErr;
                TSharedPtr<FJsonObject> LoftResult;
                Dispatch(Dispatcher, Sink, TEXT("geometry.loft"), TEXT("req-loft-multi"),
                    LP, bLoft, LoftResult, LoftErr);

                if (TestTrue(TEXT("geometry.loft (multi-profile) succeeded"), bLoft) &&
                    TestTrue(TEXT("geometry.loft (multi-profile) returned a result"), LoftResult.IsValid()))
                {
                    double ProfilesUsed = 0.0, TrisAfter = 0.0;
                    LoftResult->TryGetNumberField(TEXT("profilesUsed"), ProfilesUsed);
                    LoftResult->TryGetNumberField(TEXT("trianglesAfter"), TrisAfter);
                    TestEqual(TEXT("loft resolved both profile actors"), (int32)ProfilesUsed, 2);
                    TestTrue(TEXT("loft emitted a non-empty mesh (was 0 with the null-polygon bug)"),
                        TrisAfter > 0.0);
                }
            }
        }
    }

    // ---- Phase B: sweep emits geometry along its linear fallback path ----
    {
        const FString Target = FString::Printf(TEXT("PW_SweepTarget_%s"), *Suffix);
        TSharedPtr<FJsonObject> TP = MakeShared<FJsonObject>();
        TP->SetStringField(TEXT("name"), Target);
        if (CreateActor(TEXT("geometry.create_procedural_mesh"), TP, Target, TEXT("sweep-target")))
        {
            TSharedPtr<FJsonObject> SP = MakeShared<FJsonObject>();
            SP->SetStringField(TEXT("actorName"), Target);
            SP->SetNumberField(TEXT("steps"), 12);

            bool bSweep = false;
            FString SweepErr;
            TSharedPtr<FJsonObject> SweepResult;
            Dispatch(Dispatcher, Sink, TEXT("geometry.sweep"), TEXT("req-sweep"),
                SP, bSweep, SweepResult, SweepErr);

            if (TestTrue(TEXT("geometry.sweep succeeded"), bSweep) &&
                TestTrue(TEXT("geometry.sweep returned a result"), SweepResult.IsValid()))
            {
                double TrisAfter = 0.0;
                SweepResult->TryGetNumberField(TEXT("trianglesAfter"), TrisAfter);
                TestTrue(TEXT("sweep emitted a non-empty mesh (was 0 - it never called AppendSweepPolygon)"),
                    TrisAfter > 0.0);
            }
        }
    }

    // ---- Phase C: no-profile loft (bounding-box extrusion) appends geometry ----
    {
        const FString Target = FString::Printf(TEXT("PW_LoftBoxTarget_%s"), *Suffix);
        TSharedPtr<FJsonObject> TP = MakeShared<FJsonObject>();
        TP->SetStringField(TEXT("name"), Target);
        TP->SetNumberField(TEXT("width"), 100.0);
        TP->SetNumberField(TEXT("height"), 100.0);
        TP->SetNumberField(TEXT("depth"), 100.0);
        if (CreateActor(TEXT("geometry.create_box"), TP, Target, TEXT("loft-box-target")))
        {
            TSharedPtr<FJsonObject> LP = MakeShared<FJsonObject>();
            LP->SetStringField(TEXT("actorName"), Target);
            LP->SetNumberField(TEXT("subdivisions"), 8);

            bool bLoft = false;
            FString LoftErr;
            TSharedPtr<FJsonObject> LoftResult;
            Dispatch(Dispatcher, Sink, TEXT("geometry.loft"), TEXT("req-loft-noprofile"),
                LP, bLoft, LoftResult, LoftErr);

            if (TestTrue(TEXT("geometry.loft (no profiles) succeeded"), bLoft) &&
                TestTrue(TEXT("geometry.loft (no profiles) returned a result"), LoftResult.IsValid()))
            {
                double TrisBefore = 0.0, TrisAfter = 0.0;
                LoftResult->TryGetNumberField(TEXT("trianglesBefore"), TrisBefore);
                LoftResult->TryGetNumberField(TEXT("trianglesAfter"), TrisAfter);
                TestTrue(TEXT("no-profile loft starts from a non-empty box mesh"), TrisBefore > 0.0);
                TestTrue(TEXT("no-profile loft appended geometry (was a no-op that never called AppendSweepPolygon)"),
                    TrisAfter > TrisBefore);
            }
        }
    }

    Cleanup();
    return true;
}
