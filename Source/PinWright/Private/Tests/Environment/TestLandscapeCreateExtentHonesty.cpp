// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression test for B-landscape-create-sizex-divisor-produces-wrong-extent.
//
// The defect: landscape.create converted the world-unit sizeX / sizeY request through a
// hardcoded literal — componentsX = floor(sizeX / 1000) — asserting that one landscape
// component spans 1000 world units. Nothing made that true. A component spans
// ComponentSizeQuads * DrawScale.X, and this handler sets no actor scale, so the spawned
// ALandscape keeps the ALandscapeProxy CDO's 128 (engine Landscape.cpp, "Old default
// scale, preserved for compatibility" — 128 on every engine 5.3 through 5.8, and NOT the
// 100 the landscape editor's New Landscape panel defaults to). At the default 63-quad
// subsection that is 63 * 128 = 8064 uu per component, so the divisor was off by 8.064x
// in the direction that builds the landscape TOO LARGE: sizeX 5000 produced 5 components,
// i.e. 40,320 uu of terrain for a 5,000 uu request.
//
// It was silent because the success response echoed component and quad counts only —
// never a world extent and never the draw scale — so a caller who asked for 5,000 uu and
// read back componentsX:5 had been told a true fact about a landscape eight times the one
// they asked for, with nothing in the response able to reveal it.
//
// The fix does the two things the house convention requires of a constrained size:
//   1. Derives the divisor instead of asserting one: componentsX =
//      max(1, round(sizeX / (ComponentSizeQuads * DrawScale.X))), evaluated on the game
//      thread against the scale the spawned actor actually carries. Round, not floor, so
//      a request lands on the NEAREST reachable extent.
//   2. Reports the achieved extent, MEASURED off the built landscape (worldSizeX /
//      worldSizeY, beside drawScale), names the request separately as requestedSizeX /
//      requestedSizeY, and warns in warnings[] when the quantisation moved the result.
// A landscape's extent is ComponentCount * ComponentSizeQuads * DrawScale, and only the
// count is an integer this verb can choose, so an arbitrary world size is not reachable:
// it is snapped, and the snap is announced rather than hidden.
//
// The repro below asks for 5,000 uu on both axes at the defaults, which is not expressible
// (the step is 8,064 uu) and is the ticket's own worked example. Counterfactual: with the
// floor(size / 1000) divisor restored, componentsX is 5 rather than the 1 the request
// rounds to, the built extent is 40,320 uu against a 5,000 uu ask, and worldSizeX is not
// in the response at all — the first three assertions each fail on their own.
#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Landscape.h"
#include "LandscapeComponent.h"
#include "Misc/Guid.h"
#include "Tests/TestSkipReporting.h"
#include "Tests/TestUtils.h"
#include "Tests/TestWorldUtils.h"

namespace PinWrightLandscapeCreateExtentTest
{
    // The world extent of the component grid that was actually BUILT, measured off the
    // actor: the span of the components' section bases plus one component, times the
    // actor's own draw scale. Deliberately independent of the handler's reporting path
    // (it reads neither the response nor ULandscapeInfo, which is what the handler
    // measures through), so an agreement between the two is real corroboration rather
    // than the same computation asserted twice. Returns false on a hollow landscape.
    bool MeasureBuiltExtent(ALandscape* Landscape, double& OutSizeX, double& OutSizeY,
        int32& OutComponentsX, int32& OutComponentsY)
    {
        if (!Landscape || Landscape->LandscapeComponents.Num() == 0
            || Landscape->ComponentSizeQuads <= 0)
        {
            return false;
        }

        int32 MinBaseX = MAX_int32, MinBaseY = MAX_int32;
        int32 MaxBaseX = MIN_int32, MaxBaseY = MIN_int32;
        for (ULandscapeComponent* Component : Landscape->LandscapeComponents)
        {
            if (!Component)
            {
                continue;
            }
            const FIntPoint Base = Component->GetSectionBase();
            MinBaseX = FMath::Min(MinBaseX, Base.X);
            MinBaseY = FMath::Min(MinBaseY, Base.Y);
            MaxBaseX = FMath::Max(MaxBaseX, Base.X);
            MaxBaseY = FMath::Max(MaxBaseY, Base.Y);
        }
        if (MinBaseX > MaxBaseX || MinBaseY > MaxBaseY)
        {
            return false;
        }

        const int32 QuadsX = (MaxBaseX - MinBaseX) + Landscape->ComponentSizeQuads;
        const int32 QuadsY = (MaxBaseY - MinBaseY) + Landscape->ComponentSizeQuads;
        const FVector Scale = Landscape->GetActorScale3D();
        OutSizeX = QuadsX * FMath::Abs(Scale.X);
        OutSizeY = QuadsY * FMath::Abs(Scale.Y);
        OutComponentsX = QuadsX / Landscape->ComponentSizeQuads;
        OutComponentsY = QuadsY / Landscape->ComponentSizeQuads;
        return true;
    }

    ALandscape* FindLandscapeByLabel(UWorld* World, const FString& Label)
    {
        for (TActorIterator<ALandscape> It(World); It; ++It)
        {
            if (It->GetActorLabel().Equals(Label, ESearchCase::IgnoreCase))
            {
                return *It;
            }
        }
        return nullptr;
    }
}

// ============================================================================
// A world-unit size request that is not expressible is SNAPPED, and the response
// reports the extent that was actually built rather than echoing the request.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLandscapeCreateReportsAchievedExtentTest,
    "PinWright.landscape.create.ReportsAchievedExtent",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FLandscapeCreateReportsAchievedExtentTest::RunTest(const FString& Parameters)
{
    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("Editor world not available — skipping landscape.create achieved-extent test"));
        return true;
    }

    FScopedEditorWorldActorGuard WorldGuard;
    const FString Label = FString::Printf(TEXT("PW_CreateExtent_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));

    // 5,000 uu on both axes at the default 63-quad / 1-section geometry. The reachable
    // step is 8,064 uu, so 5,000 is NOT expressible: it must round to one component
    // (8,064 uu) and the response must say so. The old divisor produced 5 components.
    const double RequestedSize = 5000.0;
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("name"), Label);
    Payload->SetNumberField(TEXT("sizeX"), RequestedSize);
    Payload->SetNumberField(TEXT("sizeY"), RequestedSize);

    TSharedRef<FTestResponseCapture> Capture = MakeShared<FTestResponseCapture>();
    TestTrue(TEXT("landscape.create handler is registered"),
        InvokeHandlerWithSharedCapture(TEXT("landscape.create"), Payload, Capture));
    PumpUntilCaptured(*Capture, /*TimeoutSeconds=*/60.0);
    TestTrue(TEXT("landscape.create succeeded"), Capture->bSuccess);
    if (!Capture->bSuccess || !Capture->Result.IsValid())
    {
        return false;
    }

    ALandscape* Landscape = PinWrightLandscapeCreateExtentTest::FindLandscapeByLabel(World, Label);
    if (!Landscape)
    {
        AddError(TEXT("landscape.create reported success but no actor with that label exists"));
        return false;
    }

    double BuiltSizeX = 0.0, BuiltSizeY = 0.0;
    int32 BuiltComponentsX = 0, BuiltComponentsY = 0;
    if (!PinWrightLandscapeCreateExtentTest::MeasureBuiltExtent(
            Landscape, BuiltSizeX, BuiltSizeY, BuiltComponentsX, BuiltComponentsY))
    {
        AddError(TEXT("landscape.create produced no measurable component grid"));
        return false;
    }

    const TSharedPtr<FJsonObject>& Result = Capture->Result;

    // 1. The response answers a world-unit question in world units at all. Before the
    //    fix there was no extent field of any kind, which is what made the 8x error
    //    invisible on the wire.
    bool bExtentMeasured = false;
    TestTrue(TEXT("world extent was measured"),
        Result->TryGetBoolField(TEXT("worldExtentMeasured"), bExtentMeasured) && bExtentMeasured);

    double ReportedSizeX = 0.0, ReportedSizeY = 0.0;
    const bool bHasWorldSizeX = Result->TryGetNumberField(TEXT("worldSizeX"), ReportedSizeX);
    const bool bHasWorldSizeY = Result->TryGetNumberField(TEXT("worldSizeY"), ReportedSizeY);
    TestTrue(TEXT("response carries worldSizeX"), bHasWorldSizeX);
    TestTrue(TEXT("response carries worldSizeY"), bHasWorldSizeY);
    if (!bHasWorldSizeX || !bHasWorldSizeY)
    {
        return false;
    }

    // 2. The reported extent is the one the landscape actually has — measured off the
    //    component grid and the actor's own scale, not recomputed from the inputs.
    TestTrue(FString::Printf(
        TEXT("worldSizeX %.1f matches the built grid %.1f"), ReportedSizeX, BuiltSizeX),
        FMath::IsNearlyEqual(ReportedSizeX, BuiltSizeX, 0.5));
    TestTrue(FString::Printf(
        TEXT("worldSizeY %.1f matches the built grid %.1f"), ReportedSizeY, BuiltSizeY),
        FMath::IsNearlyEqual(ReportedSizeY, BuiltSizeY, 0.5));

    // 3. The component count the request converted to is the NEAREST reachable one for
    //    the scale the actor carries — the derived divisor, not the hardcoded 1000. This
    //    is the assertion the old floor(size / 1000) fails outright: 5 components against
    //    the 1 that 5,000 uu rounds to at an 8,064 uu step.
    const double ComponentSpanX =
        Landscape->ComponentSizeQuads * FMath::Abs(Landscape->GetActorScale3D().X);
    const double ComponentSpanY =
        Landscape->ComponentSizeQuads * FMath::Abs(Landscape->GetActorScale3D().Y);
    const int32 ExpectedComponentsX =
        FMath::Max(1, FMath::RoundToInt(RequestedSize / ComponentSpanX));
    const int32 ExpectedComponentsY =
        FMath::Max(1, FMath::RoundToInt(RequestedSize / ComponentSpanY));
    TestEqual(TEXT("built componentsX is the nearest reachable count"),
        BuiltComponentsX, ExpectedComponentsX);
    TestEqual(TEXT("built componentsY is the nearest reachable count"),
        BuiltComponentsY, ExpectedComponentsY);
    int32 ReportedComponentsX = 0, ReportedComponentsY = 0;
    Result->TryGetNumberField(TEXT("componentsX"), ReportedComponentsX);
    Result->TryGetNumberField(TEXT("componentsY"), ReportedComponentsY);
    TestEqual(TEXT("response componentsX matches the grid that was built"),
        ReportedComponentsX, BuiltComponentsX);
    TestEqual(TEXT("response componentsY matches the grid that was built"),
        ReportedComponentsY, BuiltComponentsY);

    // 4. The request is named separately from the achievement, and the two are not
    //    conflated: this size is deliberately not expressible, so they must differ.
    double ReportedRequestX = 0.0, ReportedRequestY = 0.0;
    TestTrue(TEXT("response carries requestedSizeX"),
        Result->TryGetNumberField(TEXT("requestedSizeX"), ReportedRequestX));
    TestTrue(TEXT("response carries requestedSizeY"),
        Result->TryGetNumberField(TEXT("requestedSizeY"), ReportedRequestY));
    TestEqual(TEXT("requestedSizeX echoes what was asked for"),
        ReportedRequestX, RequestedSize);
    TestTrue(TEXT("worldSizeX is the achieved extent, not the requested one"),
        !FMath::IsNearlyEqual(ReportedSizeX, RequestedSize, 1.0));

    // 5. The divergence is announced rather than left for the caller to notice.
    const TArray<TSharedPtr<FJsonValue>>* WarningsArray = nullptr;
    TestTrue(TEXT("a non-expressible size produces a warning"),
        Result->TryGetArrayField(TEXT("warnings"), WarningsArray)
            && WarningsArray != nullptr && WarningsArray->Num() > 0);

    // 6. The draw scale — the term the old divisor ignored — is published, so the caller
    //    can reproduce the conversion instead of guessing at it.
    const TSharedPtr<FJsonObject>* DrawScale = nullptr;
    TestTrue(TEXT("response carries drawScale"),
        Result->TryGetObjectField(TEXT("drawScale"), DrawScale) && DrawScale != nullptr);
    if (DrawScale && DrawScale->IsValid())
    {
        TestEqual(TEXT("drawScale.x is the actor's scale"),
            (*DrawScale)->GetNumberField(TEXT("x")), Landscape->GetActorScale3D().X);
    }

    return true;
}
