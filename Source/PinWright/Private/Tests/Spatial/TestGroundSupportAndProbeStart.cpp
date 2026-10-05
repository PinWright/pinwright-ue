// Copyright (c) 2026 Alexander Penkin. MIT License.

// TestGroundSupportAndProbeStart.cpp - two grounding verdicts that used to be wrong or mute.
//
// (1) A single-point rest passed. coverage = SupportedColumns / ActorColumns, so on a footprint
//     whose geometry columns collapsed to one it is 1.0 by construction, and minCoverage (clamped
//     0-1) cannot reject it. minSupportedColumns is the absolute floor; the RPC verbs default it
//     to min(2, samples^2). Counterfactual: remove the SupportedColumns check from EvaluateContact
//     and SinglePointRestFailsMinSupportedColumns and PillarRestNeedsTwoSupportedColumns both see
//     a pass where they assert INSUFFICIENT_GROUND_CONTACT.
//
// (2) An actor buried below the terrain got the generic GROUND_NOT_FOUND although the response
//     already knew why: the landscape reports a height above where the downward probe started.
//     Counterfactual: drop the LandscapeZCm >= ProbeStartZCm branch and both probe-start tests
//     read GROUND_NOT_FOUND where they assert GROUND_ABOVE_PROBE_START.
//
// Fixtures are spawned through the production RPCs into the editor world inside
// FScopedEditorWorldActorGuard, which destroys them and restores the level's dirty flag.

#include "Misc/AutomationTest.h"
#include "Handlers/Spatial/GroundPlacementUtils.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Tests/TestSkipReporting.h"
#include "Tests/TestUtils.h"
#include "Tests/TestWorldUtils.h"
#include "Utils/ActorUtils.h"

#include "Editor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "Landscape.h"
#include "Misc/Guid.h"

namespace PwGroundSupportTest
{
    // An empty column of the editor world, clear of the TestGroundPlacement.cpp column.
    constexpr double ColX = 339100.0;
    constexpr double ColY = 274500.0;

    FString Label(const TCHAR* Prefix)
    {
        return FString::Printf(TEXT("PWGS_%s_%s"), Prefix, *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    }

    TSharedPtr<FJsonObject> Vec(const FVector& V)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetNumberField(TEXT("x"), V.X);
        Obj->SetNumberField(TEXT("y"), V.Y);
        Obj->SetNumberField(TEXT("z"), V.Z);
        return Obj;
    }

    // The engine basic cube (100 cm, centred pivot) through actor.spawn, at an explicit scale.
    AActor* SpawnCube(FAutomationTestBase& Test, UWorld* World, const FString& Name,
                      const FVector& Location, const FVector& Scale)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("meshPath"), TEXT("/Engine/BasicShapes/Cube.Cube"));
        Payload->SetStringField(TEXT("actorName"), Name);
        Payload->SetObjectField(TEXT("location"), Vec(Location));
        Payload->SetObjectField(TEXT("scale"), Vec(Scale));

        FTestResponseCapture Capture;
        Test.TestTrue(TEXT("actor.spawn handler registered"),
            InvokeHandlerWithCapture(TEXT("actor.spawn"), Payload, Capture));
        Test.TestTrue(*FString::Printf(TEXT("cube '%s' spawned"), *Name), Capture.bSuccess);
        return McpActorUtils::FindActorByName(World, Name);
    }

    // Editor worlds do not tick physics on their own; a just-spawned body is not in the
    // scene-query structure until a tick flushes it.
    void FlushPhysics(UWorld* World)
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

    // One actor, a surface, detail:"all" so the passing row is present too.
    TSharedPtr<FJsonObject> Payload(const FString& ActorName, const TCHAR* Preset)
    {
        TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
        TSharedPtr<FJsonObject> Surface = MakeShared<FJsonObject>();
        Surface->SetStringField(TEXT("preset"), Preset);
        Surface->SetNumberField(TEXT("maxDrop"), 1000.0);
        Out->SetObjectField(TEXT("surface"), Surface);
        TArray<TSharedPtr<FJsonValue>> Names;
        Names.Add(MakeShared<FJsonValueString>(ActorName));
        Out->SetArrayField(TEXT("actors"), Names);
        Out->SetStringField(TEXT("detail"), TEXT("all"));
        return Out;
    }

    TSharedPtr<FJsonObject> FirstRow(const TSharedPtr<FJsonObject>& Result)
    {
        const TArray<TSharedPtr<FJsonValue>>* Rows = nullptr;
        if (!Result.IsValid() || !Result->TryGetArrayField(TEXT("results"), Rows) || !Rows
            || Rows->Num() == 0 || !(*Rows)[0].IsValid())
        {
            return nullptr;
        }
        return (*Rows)[0]->AsObject();
    }

    // Runs verify_grounding and returns the row's contact object (null + an error when absent).
    TSharedPtr<FJsonObject> VerifyContact(FAutomationTestBase& Test, const TSharedPtr<FJsonObject>& In,
                                          TSharedPtr<FJsonObject>* OutResult = nullptr)
    {
        FTestResponseCapture Capture;
        Test.TestTrue(TEXT("spatial.verify_grounding handler registered"),
            InvokeHandlerWithCapture(TEXT("spatial.verify_grounding"), In, Capture));
        Test.TestTrue(TEXT("the verify call succeeds"), Capture.bSuccess);
        if (OutResult)
        {
            *OutResult = Capture.Result;
        }
        const TSharedPtr<FJsonObject> Row = FirstRow(Capture.Result);
        const TSharedPtr<FJsonObject>* Contact = nullptr;
        if (!Row.IsValid() || !Row->TryGetObjectField(TEXT("contact"), Contact) || !Contact)
        {
            Test.AddError(TEXT("verify_grounding returned no results[0].contact"));
            return nullptr;
        }
        return *Contact;
    }

    FString Str(const TSharedPtr<FJsonObject>& Obj, const TCHAR* Field)
    {
        FString Out;
        if (Obj.IsValid())
        {
            Obj->TryGetStringField(Field, Out);
        }
        return Out;
    }

    bool Pass(const TSharedPtr<FJsonObject>& Obj)
    {
        bool bOut = false;
        return Obj.IsValid() && Obj->TryGetBoolField(TEXT("pass"), bOut) && bOut;
    }
}

// ---- (1) EvaluateContact: a footprint collapsed to one actor column ----

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGroundSinglePointRestFailsMinSupportedColumnsTest,
    "PinWright.spatial.verify_grounding.SinglePointRestFailsMinSupportedColumns",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGroundSinglePointRestFailsMinSupportedColumnsTest::RunTest(const FString& Parameters)
{
    using namespace GroundPlacement;

    // 3x3 grid; the actor's own geometry answered in the centre column only, touching flat
    // ground. The other eight see ground but no actor, so they are excluded from coverage.
    TArray<FGroundColumn> Columns;
    for (int32 Index = 0; Index < 9; ++Index)
    {
        FGroundColumn Column;
        Column.bHasActorGeometry = (Index == 4);
        Column.UndersideZ = 0.0;
        Column.bHasGround = true;
        Column.GroundZ = 0.0;
        Columns.Add(Column);
    }

    FContactThresholds Thresholds;
    FGroundContactReport Report;
    AggregateColumns(Columns, Thresholds, Report);
    TestEqual(TEXT("one actor column"), Report.ActorColumns, 1);
    TestEqual(TEXT("one supported column"), Report.SupportedColumns, 1);
    TestTrue(*FString::Printf(TEXT("coverage is 1.0 by construction (got %.3f)"), Report.Coverage),
        FMath::IsNearlyEqual(Report.Coverage, 1.0));

    // Internal default (1): the old verdict, a flawless row.
    EvaluateContact(Report, Thresholds);
    TestTrue(*FString::Printf(TEXT("minSupportedColumns 1 accepts it (failReason: %s)"),
        *Report.FailReason), Report.bPass);

    // The RPC default at samples >= 2.
    Thresholds.MinSupportedColumns = 2;
    EvaluateContact(Report, Thresholds);
    TestFalse(TEXT("minSupportedColumns 2 rejects a single-point rest"), Report.bPass);
    TestEqual(TEXT("-> INSUFFICIENT_GROUND_CONTACT"), Report.FailReasonCode,
        FString(TEXT("INSUFFICIENT_GROUND_CONTACT")));
    TestTrue(*FString::Printf(TEXT("the reason names the threshold and the collapse (got '%s')"),
        *Report.FailReason),
        Report.FailReason.Contains(TEXT("minSupportedColumns 2"))
            && Report.FailReason.Contains(TEXT("1 of 9 sampled columns"))
            && Report.FailReason.Contains(TEXT("minSupportedColumns: 1")));
    return true;
}

// ---- (1) The verbs wire it through: a cube resting on a 10 cm pillar ----

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGroundPillarRestNeedsTwoSupportedColumnsTest,
    "PinWright.spatial.verify_grounding.PillarRestNeedsTwoSupportedColumns",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGroundPillarRestNeedsTwoSupportedColumnsTest::RunTest(const FString& Parameters)
{
    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("Editor world not available; skipping the pillar-rest grounding test."));
        return true;
    }
    FScopedEditorWorldActorGuard WorldGuard;

    // Pillar: 10 x 10 cm, top at Z 0. Target: a 100 cm cube whose bottom rests on it. Of the 3x3
    // grid only the centre column is over the pillar, so 1 of 9 columns finds ground.
    const FString PillarName = PwGroundSupportTest::Label(TEXT("Pillar"));
    const FString TargetName = PwGroundSupportTest::Label(TEXT("OnPillar"));
    const FVector Col(PwGroundSupportTest::ColX, PwGroundSupportTest::ColY, 0.0);
    AActor* Pillar = PwGroundSupportTest::SpawnCube(*this, World, PillarName,
        Col + FVector(0.0, 0.0, -50.0), FVector(0.1, 0.1, 1.0));
    AActor* Target = PwGroundSupportTest::SpawnCube(*this, World, TargetName,
        Col + FVector(0.0, 0.0, 50.0), FVector(1.0));
    if (!Pillar || !Target)
    {
        AddError(TEXT("fixture cubes did not spawn"));
        return true;
    }
    PwGroundSupportTest::FlushPhysics(World);

    // minCoverage 0 takes the ratio out, so only the absolute floor can reject it.
    TSharedPtr<FJsonObject> In = PwGroundSupportTest::Payload(TargetName, TEXT("any_solid"));
    In->SetNumberField(TEXT("minCoverage"), 0.0);

    TSharedPtr<FJsonObject> Result;
    const TSharedPtr<FJsonObject> Contact = PwGroundSupportTest::VerifyContact(*this, In, &Result);
    double SupportedColumns = -1.0;
    if (Contact.IsValid())
    {
        Contact->TryGetNumberField(TEXT("supportedColumns"), SupportedColumns);
    }
    TestEqual(TEXT("precondition: exactly the centre column found ground"), SupportedColumns, 1.0);
    TestFalse(TEXT("a cube on a pillar does not pass at the default"),
        PwGroundSupportTest::Pass(Contact));
    TestEqual(TEXT("-> INSUFFICIENT_GROUND_CONTACT"),
        PwGroundSupportTest::Str(Contact, TEXT("failCode")),
        FString(TEXT("INSUFFICIENT_GROUND_CONTACT")));
    const TSharedPtr<FJsonObject>* Criteria = nullptr;
    double EchoedDefault = -1.0;
    if (Result.IsValid() && Result->TryGetObjectField(TEXT("criteria"), Criteria) && Criteria)
    {
        (*Criteria)->TryGetNumberField(TEXT("minSupportedColumns"), EchoedDefault);
    }
    TestEqual(TEXT("criteria echoes the default of 2 at samples 3"), EchoedDefault, 2.0);

    // Control: the same seat with the floor lowered to 1 passes, so nothing else rejected it.
    In->SetNumberField(TEXT("minSupportedColumns"), 1);
    TestTrue(TEXT("minSupportedColumns 1 accepts the same rest"),
        PwGroundSupportTest::Pass(PwGroundSupportTest::VerifyContact(*this, In)));

    // samples:1 asked for one column, so the default drops to 1 there.
    In->RemoveField(TEXT("minSupportedColumns"));
    In->SetNumberField(TEXT("samples"), 1);
    TSharedPtr<FJsonObject> OneColumnResult;
    TestTrue(TEXT("samples:1 keeps passing a one-column rest"),
        PwGroundSupportTest::Pass(PwGroundSupportTest::VerifyContact(*this, In, &OneColumnResult)));
    double EchoedOne = -1.0;
    if (OneColumnResult.IsValid()
        && OneColumnResult->TryGetObjectField(TEXT("criteria"), Criteria) && Criteria)
    {
        (*Criteria)->TryGetNumberField(TEXT("minSupportedColumns"), EchoedOne);
    }
    TestEqual(TEXT("criteria echoes 1 at samples 1"), EchoedOne, 1.0);

    // The seat verb refuses it before moving anything.
    TSharedPtr<FJsonObject> Seat = PwGroundSupportTest::Payload(TargetName, TEXT("any_solid"));
    Seat->SetNumberField(TEXT("minCoverage"), 0.0);
    const double BeforeZ = Target->GetActorLocation().Z;
    FTestResponseCapture SeatCapture;
    TestTrue(TEXT("spatial.ground_actors handler registered"),
        InvokeHandlerWithCapture(TEXT("spatial.ground_actors"), Seat, SeatCapture));
    const TSharedPtr<FJsonObject> SeatRow = PwGroundSupportTest::FirstRow(SeatCapture.Result);
    TestEqual(TEXT("ground_actors refuses with INSUFFICIENT_GROUND_CONTACT"),
        PwGroundSupportTest::Str(SeatRow, TEXT("reasonCode")),
        FString(TEXT("INSUFFICIENT_GROUND_CONTACT")));
    TestEqual(TEXT("...under the partial_ground_coverage status"),
        PwGroundSupportTest::Str(SeatRow, TEXT("status")), FString(TEXT("partial_ground_coverage")));
    TestEqual(TEXT("the refused actor did not move"), Target->GetActorLocation().Z, BeforeZ);
    return true;
}

// ---- (2) EvaluateContact: landscape above the probe start is named ----

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGroundAboveProbeStartNamesProbeLiftTest,
    "PinWright.spatial.verify_grounding.GroundAboveProbeStartNamesProbeLift",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGroundAboveProbeStartNamesProbeLiftTest::RunTest(const FString& Parameters)
{
    using namespace GroundPlacement;

    auto Evaluate = [](TOptional<bool> bOverLandscape, TOptional<double> LandscapeZ) -> FGroundContactReport
    {
        FGroundContactReport Report;
        Report.bMeasured = true;
        Report.SampledColumns = 9;
        Report.ActorColumns = 9;
        Report.SupportedColumns = 0;
        Report.bOverLandscape = bOverLandscape;
        Report.LandscapeZCm = LandscapeZ;
        Report.ProbeStartZCm = 1100.0;
        EvaluateContact(Report, FContactThresholds());
        return Report;
    };

    // The field case: terrain at 1500, probe started at 1100 (scatter plane + 500 cm lift).
    const FGroundContactReport Buried = Evaluate(true, 1500.0);
    TestFalse(TEXT("buried actor fails"), Buried.bPass);
    TestEqual(TEXT("-> GROUND_ABOVE_PROBE_START"), Buried.FailReasonCode,
        FString(TEXT("GROUND_ABOVE_PROBE_START")));
    TestTrue(*FString::Printf(TEXT("names both heights, probeLift and the lift needed (got '%s')"),
        *Buried.FailReason),
        Buried.FailReason.Contains(TEXT("1500.0")) && Buried.FailReason.Contains(TEXT("1100.0"))
            && Buried.FailReason.Contains(TEXT("probeLift")) && Buried.FailReason.Contains(TEXT("401")));

    // Landscape below the probe start: the probe could have reached it, so the cause is not
    // claimed - still the generic code.
    const FGroundContactReport Missed = Evaluate(true, 0.0);
    TestEqual(TEXT("landscape below the probe start -> GROUND_NOT_FOUND"), Missed.FailReasonCode,
        FString(TEXT("GROUND_NOT_FOUND")));

    // No landscape height at all: the off-terrain branch is unchanged.
    const FGroundContactReport OffTerrain = Evaluate(false, TOptional<double>());
    TestEqual(TEXT("off terrain -> GROUND_NOT_FOUND"), OffTerrain.FailReasonCode,
        FString(TEXT("GROUND_NOT_FOUND")));
    TestTrue(TEXT("...naming the off-terrain cause"), OffTerrain.FailReason.Contains(TEXT("off the")));
    return true;
}

// ---- (2) End to end: a cube 3000 cm under a real landscape ----

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGroundBuriedUnderLandscapeReportsProbeStartTest,
    "PinWright.spatial.verify_grounding.BuriedUnderLandscapeReportsProbeStart",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGroundBuriedUnderLandscapeReportsProbeStartTest::RunTest(const FString& Parameters)
{
    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World || !World->PersistentLevel)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("Editor world not available; skipping the buried-under-landscape test."));
        return true;
    }
    FScopedEditorWorldActorGuard WorldGuard;

    // A flat 1x1-component landscape (7 quads) at Z 0.
    const FString LandscapeName = PwGroundSupportTest::Label(TEXT("Terrain"));
    TSharedPtr<FJsonObject> Create = MakeShared<FJsonObject>();
    Create->SetStringField(TEXT("name"), LandscapeName);
    Create->SetObjectField(TEXT("location"),
        PwGroundSupportTest::Vec(FVector(PwGroundSupportTest::ColX, PwGroundSupportTest::ColY, 0.0)));
    Create->SetNumberField(TEXT("componentsX"), 1);
    Create->SetNumberField(TEXT("componentsY"), 1);
    Create->SetNumberField(TEXT("quadsPerComponent"), 7);
    Create->SetNumberField(TEXT("sectionsPerComponent"), 1);
    TSharedRef<FTestResponseCapture> CreateCapture = MakeShared<FTestResponseCapture>();
    if (!InvokeHandlerWithSharedCapture(TEXT("landscape.create"), Create, CreateCapture))
    {
        AddError(TEXT("landscape.create handler is not registered"));
        return true;
    }
    PumpUntilCaptured(*CreateCapture, /*TimeoutSeconds=*/30.0);
    TestTrue(TEXT("landscape.create fixture succeeded"), CreateCapture->bSuccess);
    ALandscape* Landscape = nullptr;
    for (TActorIterator<ALandscape> It(World); It; ++It)
    {
        if (It->GetActorLabel().Equals(LandscapeName, ESearchCase::IgnoreCase))
        {
            Landscape = *It;
        }
    }
    if (!Landscape)
    {
        AddError(TEXT("landscape fixture not found in the world"));
        return true;
    }
    PwGroundSupportTest::FlushPhysics(World);

    FVector Origin = FVector::ZeroVector;
    FVector Extent = FVector::ZeroVector;
    Landscape->GetActorBounds(false, Origin, Extent);
    const TOptional<float> TerrainZ = Landscape->GetHeightAtLocation(FVector(Origin.X, Origin.Y, 0.0));
    TestTrue(TEXT("precondition: the landscape reports a height at its centre"), TerrainZ.IsSet());
    if (!TerrainZ.IsSet())
    {
        return true;
    }

    // Cube top 2950 cm under the terrain: probe start = top + 500 is still 2450 cm below it.
    const FString CubeName = PwGroundSupportTest::Label(TEXT("Buried"));
    AActor* Cube = PwGroundSupportTest::SpawnCube(*this, World, CubeName,
        FVector(Origin.X, Origin.Y, TerrainZ.GetValue() - 3000.0), FVector(1.0));
    if (!Cube)
    {
        AddError(TEXT("buried cube did not spawn"));
        return true;
    }
    PwGroundSupportTest::FlushPhysics(World);

    TSharedPtr<FJsonObject> In = PwGroundSupportTest::Payload(CubeName, TEXT("landscape"));
    const TSharedPtr<FJsonObject> Contact = PwGroundSupportTest::VerifyContact(*this, In);
    TestEqual(TEXT("buried under the landscape -> GROUND_ABOVE_PROBE_START"),
        PwGroundSupportTest::Str(Contact, TEXT("failCode")), FString(TEXT("GROUND_ABOVE_PROBE_START")));
    const FString Reason = PwGroundSupportTest::Str(Contact, TEXT("failReason"));
    TestTrue(*FString::Printf(TEXT("the reason names probeLift (got '%s')"), *Reason),
        Reason.Contains(TEXT("probeLift")));

    // The remedy the message names works: a lift past the terrain finds it.
    TSharedPtr<FJsonObject> Lifted = PwGroundSupportTest::Payload(CubeName, TEXT("landscape"));
    const TSharedPtr<FJsonObject>* Surface = nullptr;
    if (Lifted->TryGetObjectField(TEXT("surface"), Surface) && Surface)
    {
        (*Surface)->SetNumberField(TEXT("probeLift"), 5000.0);
    }
    const TSharedPtr<FJsonObject> LiftedContact = PwGroundSupportTest::VerifyContact(*this, Lifted);
    double Supported = 0.0;
    if (LiftedContact.IsValid())
    {
        LiftedContact->TryGetNumberField(TEXT("supportedColumns"), Supported);
    }
    TestTrue(*FString::Printf(TEXT("probeLift 5000 finds the terrain (supportedColumns %.0f)"), Supported),
        Supported > 0.0);
    TestNotEqual(TEXT("...and no longer reports the probe start"),
        PwGroundSupportTest::Str(LiftedContact, TEXT("failCode")), FString(TEXT("GROUND_ABOVE_PROBE_START")));
    return true;
}
