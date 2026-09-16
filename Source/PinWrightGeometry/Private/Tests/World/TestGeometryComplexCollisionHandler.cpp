// Copyright (c) 2026 Alexander Penkin. MIT License.

// Behavioral coverage for geometry.generate_complex_collision's response contract.
#include "Misc/AutomationTest.h"

#include "Handlers/HandlerContext.h"
#include "Tests/TestSkipReporting.h"
#include "Tests/TestUtils.h"
#include "Tests/TestWorldUtils.h"

#include "Components/DynamicMeshComponent.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "DynamicMeshActor.h"
#include "Editor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Misc/Guid.h"
#include "PhysicsEngine/BodySetup.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryGenerateComplexCollisionReportsMeasuredHullCountTest,
    "PinWright.geometry.generate_complex_collision.ReportsMeasuredHullCountAndClamp",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGeometryGenerateComplexCollisionReportsMeasuredHullCountTest::RunTest(const FString& Parameters)
{
    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("GEditor->GetEditorWorldContext().World() returned null, so the complex-collision "
                 "fixture and response assertions could not run."));
        return true;
    }

    FScopedEditorWorldActorGuard WorldGuard;

    const FString ActorLabel = FString::Printf(
        TEXT("PW_ComplexCollisionHullCount_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));

    TSharedPtr<FJsonObject> CreatePayload = MakeShared<FJsonObject>();
    CreatePayload->SetStringField(TEXT("name"), ActorLabel);
    CreatePayload->SetNumberField(TEXT("outerRadius"), 50.0);
    CreatePayload->SetNumberField(TEXT("innerRadius"), 15.0);
    CreatePayload->SetNumberField(TEXT("height"), 160.0);
    CreatePayload->SetNumberField(TEXT("radialSteps"), 24.0);
    TestTrue(TEXT("geometry.create_pipe handler is registered"),
        InvokeHandler(TEXT("geometry.create_pipe"), CreatePayload));

    ADynamicMeshActor* Actor = nullptr;
    for (TActorIterator<ADynamicMeshActor> It(World); It; ++It)
    {
        if (IsValid(*It) && It->GetActorLabel() == ActorLabel)
        {
            Actor = *It;
            break;
        }
    }

    TestNotNull(TEXT("the complex-collision fixture actor was spawned"), Actor);
    if (!Actor)
    {
        return true;
    }

    auto ExerciseClampCase = [&](int32 RequestedMaxHullCount, int32 ExpectedEffectiveMaxHullCount,
        const TCHAR* CaseLabel)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("actorName"), ActorLabel);
        Payload->SetNumberField(TEXT("maxHullCount"), RequestedMaxHullCount);

        FTestResponseCapture Capture;
        TestTrue(*FString::Printf(TEXT("%s handler is registered"), CaseLabel),
            InvokeHandlerWithCapture(TEXT("geometry.generate_complex_collision"), Payload, Capture));
        TestTrue(*FString::Printf(TEXT("%s response is successful"), CaseLabel), Capture.bSuccess);
        TestTrue(*FString::Printf(TEXT("%s response has a result payload"), CaseLabel),
            Capture.Result.IsValid());
        if (!Capture.bSuccess || !Capture.Result.IsValid())
        {
            return;
        }

        const UDynamicMeshComponent* Component = Actor->GetDynamicMeshComponent();
        const UBodySetup* BodySetup = Component ? Component->GetBodySetup() : nullptr;
        TestNotNull(*FString::Printf(TEXT("%s built a component body setup"), CaseLabel), BodySetup);

        double ReportedHullCount = -1.0;
        TestTrue(*FString::Printf(TEXT("%s response reports hullCount"), CaseLabel),
            Capture.Result->TryGetNumberField(TEXT("hullCount"), ReportedHullCount));
        TestTrue(*FString::Printf(TEXT("%s hullCount is within the effective budget"), CaseLabel),
            ReportedHullCount >= 0.0
                && ReportedHullCount <= static_cast<double>(ExpectedEffectiveMaxHullCount));
        if (BodySetup)
        {
            TestTrue(*FString::Printf(TEXT("%s hullCount matches built convex elements"), CaseLabel),
                FMath::IsNearlyEqual(ReportedHullCount,
                    static_cast<double>(BodySetup->AggGeom.ConvexElems.Num())));

            double ReportedShapeCount = -1.0;
            TestTrue(*FString::Printf(TEXT("%s response reports shapeCount"), CaseLabel),
                Capture.Result->TryGetNumberField(TEXT("shapeCount"), ReportedShapeCount));
            TestTrue(*FString::Printf(TEXT("%s shapeCount matches built collision elements"), CaseLabel),
                FMath::IsNearlyEqual(ReportedShapeCount,
                    static_cast<double>(BodySetup->AggGeom.GetElementCount())));
        }

        double ReportedRequestedMaxHullCount = -1.0;
        TestTrue(*FString::Printf(TEXT("%s response retains requested maxHullCount"), CaseLabel),
            Capture.Result->TryGetNumberField(TEXT("requestedMaxHullCount"),
                ReportedRequestedMaxHullCount));
        TestTrue(*FString::Printf(TEXT("%s requested maxHullCount is not rewritten"), CaseLabel),
            FMath::IsNearlyEqual(ReportedRequestedMaxHullCount,
                static_cast<double>(RequestedMaxHullCount)));

        double ReportedEffectiveMaxHullCount = -1.0;
        TestTrue(*FString::Printf(TEXT("%s response reports effective maxHullCount"), CaseLabel),
            Capture.Result->TryGetNumberField(TEXT("effectiveMaxHullCount"),
                ReportedEffectiveMaxHullCount));
        TestTrue(*FString::Printf(TEXT("%s effective maxHullCount is clamped"), CaseLabel),
            FMath::IsNearlyEqual(ReportedEffectiveMaxHullCount,
                static_cast<double>(ExpectedEffectiveMaxHullCount)));

        bool bReportedClamped = false;
        TestTrue(*FString::Printf(TEXT("%s response reports clamped"), CaseLabel),
            Capture.Result->TryGetBoolField(TEXT("clamped"), bReportedClamped));
        TestTrue(*FString::Printf(TEXT("%s response marks the request as clamped"), CaseLabel),
            bReportedClamped);

        double ReportedLimit = -1.0;
        TestTrue(*FString::Printf(TEXT("%s response reports the clamp limit"), CaseLabel),
            Capture.Result->TryGetNumberField(TEXT("limit"), ReportedLimit));
        TestTrue(*FString::Printf(TEXT("%s clamp limit is 64"), CaseLabel),
            FMath::IsNearlyEqual(ReportedLimit, 64.0));

        const TArray<TSharedPtr<FJsonValue>>* Warnings = nullptr;
        bool bHasClampWarning = false;
        if (Capture.Result->TryGetArrayField(TEXT("warnings"), Warnings) && Warnings)
        {
            for (const TSharedPtr<FJsonValue>& Warning : *Warnings)
            {
                bHasClampWarning |= Warning.IsValid()
                    && Warning->AsString().Contains(TEXT("maxHullCount clamped"));
            }
        }
        TestTrue(*FString::Printf(TEXT("%s response includes a clamp warning"), CaseLabel),
            bHasClampWarning);
    };

    // Both out-of-range directions must disclose the effective engine budget while hullCount
    // comes from the collision that was actually built.
    ExerciseClampCase(0, 1, TEXT("lower-bound clamp"));
    ExerciseClampCase(100, 64, TEXT("upper-bound clamp"));

    return true;
}
