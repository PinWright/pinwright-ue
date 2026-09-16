// Copyright (c) 2026 Alexander Penkin. MIT License.

// TestLevelAudit.cpp - level.audit.
//
// Every test here asserts a FAILURE direction, because every defect this verb exists to find
// has already survived an instrument that reported success:
//   * a buried actor must be FOUND, and a deliberately bedded one must NOT be flagged - the
//     two produce identical penetration numbers, and only the "is anything above it" term
//     separates them
//   * a check that could not run must report as UNRUNNABLE, never as clean, and must make the
//     call's `pass` false
//   * a misspelled check id must be an ERROR, because a run that silently checked nothing is
//     indistinguishable from a clean level
//   * a ground check without a `surface` must be refused rather than defaulted
//   * the per-check buckets must SUM to the actors examined, so an actor cannot fall out of
//     every bucket - which is how a check silently stops running
//
// Fixtures spawn through the real actor.spawn RPC into the live editor world at an isolated
// column of their own (away from the one Tests/Spatial/TestGroundPlacement.cpp uses), with
// GUID-suffixed labels, destroyed on scope exit. Helper names carry an AuditTest prefix and
// live in one file-scope anonymous namespace, per the Unity-build rule in CLAUDE.md.

#include "Misc/AutomationTest.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/HandlerRegistration.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Tests/TestSkipReporting.h"
#include "Tests/TestUtils.h"
#include "Utils/ActorUtils.h"

#include "Components/StaticMeshComponent.h"
#include "Editor.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Misc/Guid.h"
#include "Misc/ScopeExit.h"

namespace
{
    // The engine basic Cube is 100 x 100 x 100 with a CENTRED pivot.
    constexpr double AuditTestCubeHalf = 50.0;

    // An isolated column, away from real level geometry AND away from the column
    // TestGroundPlacement.cpp / TestPlacementHandlers.cpp use, so nothing else can answer a
    // ground probe issued here.
    constexpr double AuditTestColX = 341100.0;
    constexpr double AuditTestColY = 284500.0;

    FString AuditTestLabel(const TCHAR* Prefix)
    {
        return FString::Printf(TEXT("PWA_%s_%s"), Prefix,
            *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    }

    UWorld* AuditTestEditorWorld()
    {
        return GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    }

    TSharedPtr<FJsonObject> AuditTestVec(double X, double Y, double Z)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetNumberField(TEXT("x"), X);
        Obj->SetNumberField(TEXT("y"), Y);
        Obj->SetNumberField(TEXT("z"), Z);
        return Obj;
    }

    // Editor worlds don't tick physics on their own; a just-spawned body isn't in the
    // scene-query structure until a world tick flushes it. Editor worlds don't simulate, so
    // nothing moves.
    void AuditTestFlushPhysics(UWorld* World)
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

    AActor* AuditTestSpawnCube(FAutomationTestBase& Test, UWorld* World, const FString& Label,
                               const FVector& Location)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("meshPath"), TEXT("/Engine/BasicShapes/Cube.Cube"));
        Payload->SetStringField(TEXT("actorName"), Label);
        Payload->SetObjectField(TEXT("location"), AuditTestVec(Location.X, Location.Y, Location.Z));

        FTestResponseCapture Capture;
        Test.TestTrue(TEXT("actor.spawn handler registered"),
            InvokeHandlerWithCapture(TEXT("actor.spawn"), Payload, Capture));
        Test.TestTrue(*FString::Printf(TEXT("cube '%s' spawned"), *Label), Capture.bSuccess);
        return McpActorUtils::FindActorByName(World, Label);
    }

    // Payload scoped to one actor. Scoping keeps the assertions independent of whatever else
    // the host project's map happens to contain.
    TSharedPtr<FJsonObject> AuditTestPayloadFor(const FString& ActorLabel)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        TArray<TSharedPtr<FJsonValue>> Names;
        Names.Add(MakeShared<FJsonValueString>(ActorLabel));
        Payload->SetArrayField(TEXT("actors"), Names);
        return Payload;
    }

    void AuditTestSetChecks(const TSharedPtr<FJsonObject>& Payload,
                            std::initializer_list<const TCHAR*> Checks)
    {
        TArray<TSharedPtr<FJsonValue>> Array;
        for (const TCHAR* Check : Checks)
        {
            Array.Add(MakeShared<FJsonValueString>(FString(Check)));
        }
        Payload->SetArrayField(TEXT("checks"), Array);
    }

    // {"preset":"any_solid"}. The landscape preset is the production default but there is no
    // landscape in the automation world.
    TSharedPtr<FJsonObject> AuditTestAnySolidSurface()
    {
        TSharedPtr<FJsonObject> Surface = MakeShared<FJsonObject>();
        Surface->SetStringField(TEXT("preset"), TEXT("any_solid"));
        return Surface;
    }

    // The findings[] row for (ActorLabel, CheckId), or null. Returned by value: a test that
    // silently skips its assertions is worse than no test, so every caller error-checks this.
    TSharedPtr<FJsonObject> AuditTestFindFinding(const TSharedPtr<FJsonObject>& Result,
                                                 const FString& ActorLabel, const FString& CheckId)
    {
        if (!Result.IsValid())
        {
            return nullptr;
        }
        const TArray<TSharedPtr<FJsonValue>>* Rows = nullptr;
        if (!Result->TryGetArrayField(TEXT("findings"), Rows) || !Rows)
        {
            return nullptr;
        }
        for (const TSharedPtr<FJsonValue>& Value : *Rows)
        {
            const TSharedPtr<FJsonObject>* Row = nullptr;
            if (!Value.IsValid() || !Value->TryGetObject(Row) || !Row || !(*Row).IsValid())
            {
                continue;
            }
            FString Actor;
            FString Check;
            (*Row)->TryGetStringField(TEXT("actor"), Actor);
            (*Row)->TryGetStringField(TEXT("check"), Check);
            if (Actor == ActorLabel && Check == CheckId)
            {
                return *Row;
            }
        }
        return nullptr;
    }

    // The checks[] row for CheckId, or null.
    TSharedPtr<FJsonObject> AuditTestFindCheckRow(const TSharedPtr<FJsonObject>& Result,
                                                  const FString& CheckId)
    {
        return JsonArrayFindObjectByStringField(Result, TEXT("checks"), TEXT("id"), CheckId);
    }
}

// ---- A misspelled check is an error, not a skip ----

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLevelAuditUnknownCheckRejectedTest,
    "PinWright.level.audit.UnknownCheckRejected",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FLevelAuditUnknownCheckRejectedTest::RunTest(const FString& Parameters)
{
    // A typo that silently ran nothing is indistinguishable from a clean level, which is the
    // single most dangerous way for a lint to fail. If this ever starts skipping unknown ids,
    // an audit can report success having checked nothing at all.
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    AuditTestSetChecks(Payload, {TEXT("zero_scal")});

    FTestResponseCapture Capture;
    TestTrue(TEXT("level.audit handler registered"),
        InvokeHandlerWithCapture(TEXT("level.audit"), Payload, Capture));
    TestFalse(TEXT("an unknown check id is rejected, not skipped"), Capture.bSuccess);
    TestEqual(TEXT("unknown check -> AUDIT_UNKNOWN_CHECK"),
        Capture.ErrorCode, FString(TEXT("AUDIT_UNKNOWN_CHECK")));
    // The message must name the valid ids, or the caller has no way forward.
    TestTrue(TEXT("the rejection lists the valid checks"),
        Capture.Message.Contains(TEXT("zero_scale")));
    return true;
}

// ---- A ground check without a surface is refused, never defaulted ----

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLevelAuditGroundCheckNeedsSurfaceTest,
    "PinWright.level.audit.GroundCheckNeedsSurface",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FLevelAuditGroundCheckNeedsSurfaceTest::RunTest(const FString& Parameters)
{
    // "Which surface did you mean" is the question every observed ground failure answered
    // wrong. If this ever starts defaulting, the audit inherits every one of them.
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    AuditTestSetChecks(Payload, {TEXT("below_surface"), TEXT("airborne")});

    FTestResponseCapture Capture;
    TestTrue(TEXT("level.audit handler registered"),
        InvokeHandlerWithCapture(TEXT("level.audit"), Payload, Capture));
    TestFalse(TEXT("a ground check with no surface is rejected"), Capture.bSuccess);
    TestEqual(TEXT("no surface -> INVALID_SURFACE_SPEC"),
        Capture.ErrorCode, FString(TEXT("INVALID_SURFACE_SPEC")));
    TestTrue(TEXT("the rejection names the checks that need a surface"),
        Capture.Message.Contains(TEXT("below_surface")));

    // ...and the play-area check has the same contract with its own argument.
    TSharedPtr<FJsonObject> AreaPayload = MakeShared<FJsonObject>();
    AuditTestSetChecks(AreaPayload, {TEXT("outside_play_area")});
    FTestResponseCapture AreaCapture;
    InvokeHandlerWithCapture(TEXT("level.audit"), AreaPayload, AreaCapture);
    TestFalse(TEXT("outside_play_area with no playArea is rejected"), AreaCapture.bSuccess);
    TestEqual(TEXT("no playArea -> AUDIT_INVALID_PLAY_AREA_SPEC"),
        AreaCapture.ErrorCode, FString(TEXT("AUDIT_INVALID_PLAY_AREA_SPEC")));
    return true;
}

// ---- "not selected" must be visible as its own state ----

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLevelAuditReportsNotSelectedChecksTest,
    "PinWright.level.audit.ReportsNotSelectedChecks",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FLevelAuditReportsNotSelectedChecksTest::RunTest(const FString& Parameters)
{
    // A check nobody ran must not look like a check that found nothing. Without this the
    // response's zero counts read as "the level is clean of that", which is the exact
    // reporting failure this verb was built against.
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    AuditTestSetChecks(Payload, {TEXT("zero_scale")});
    Payload->SetStringField(TEXT("detail"), TEXT("summary"));
    Payload->SetNumberField(TEXT("limit"), 50);

    FTestResponseCapture Capture;
    TestTrue(TEXT("level.audit handler registered"),
        InvokeHandlerWithCapture(TEXT("level.audit"), Payload, Capture));
    TestTrue(TEXT("the audit call succeeds"), Capture.bSuccess);
    if (!Capture.bSuccess || !Capture.Result.IsValid())
    {
        return true;
    }

    if (const TSharedPtr<FJsonObject> Row = AuditTestFindCheckRow(Capture.Result,
                                                                 TEXT("below_surface")))
    {
        bool bSelected = true;
        Row->TryGetBoolField(TEXT("selected"), bSelected);
        TestFalse(TEXT("below_surface reports selected:false"), bSelected);

        FString Reason;
        Row->TryGetStringField(TEXT("notSelectedReason"), Reason);
        TestTrue(TEXT("the unselected check says WHY it did not run"), !Reason.IsEmpty());
        // Hostile default: an absent count field must not read as a real zero.
        double Flagged = -1.0;
        TestFalse(TEXT("an unselected check emits no flagged count at all"),
            Row->TryGetNumberField(TEXT("flagged"), Flagged));
    }
    else
    {
        AddError(TEXT("level.audit result has no checks[] row for below_surface"));
    }

    if (const TSharedPtr<FJsonObject> Row = AuditTestFindCheckRow(Capture.Result,
                                                                 TEXT("zero_scale")))
    {
        bool bSelected = false;
        Row->TryGetBoolField(TEXT("selected"), bSelected);
        TestTrue(TEXT("the requested check reports selected:true"), bSelected);
        double Applicable = -1.0;
        TestTrue(TEXT("a selected check reports its applicable count"),
            Row->TryGetNumberField(TEXT("applicable"), Applicable));
    }
    else
    {
        AddError(TEXT("level.audit result has no checks[] row for zero_scale"));
    }
    return true;
}

// ---- A degenerate scale must be FOUND ----

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLevelAuditFindsZeroScaleTest,
    "PinWright.level.audit.FindsZeroScale",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FLevelAuditFindsZeroScaleTest::RunTest(const FString& Parameters)
{
    UWorld* World = AuditTestEditorWorld();
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("Editor world not available; skipping level.audit zero-scale test."));
        return true;
    }

    const FString Label = AuditTestLabel(TEXT("ZeroScale"));
    AActor* Cube = AuditTestSpawnCube(*this, World, Label,
        FVector(AuditTestColX, AuditTestColY, 500.0));
    ON_SCOPE_EXIT
    {
        if (Cube) { Cube->Destroy(); }
    };
    if (!Cube)
    {
        AddError(TEXT("fixture cube did not spawn"));
        return true;
    }
    // Collapsed on X only. The engine's own map check tests the PRODUCT of the three axes
    // (ActorEditor.cpp:1675) and so cannot say which axis went; this must.
    Cube->SetActorScale3D(FVector(0.0, 1.0, 1.0));

    TSharedPtr<FJsonObject> Payload = AuditTestPayloadFor(Label);
    AuditTestSetChecks(Payload, {TEXT("zero_scale")});

    FTestResponseCapture Capture;
    TestTrue(TEXT("level.audit handler registered"),
        InvokeHandlerWithCapture(TEXT("level.audit"), Payload, Capture));
    TestTrue(TEXT("the audit call succeeds"), Capture.bSuccess);
    if (!Capture.bSuccess || !Capture.Result.IsValid())
    {
        return true;
    }

    // Hostile default: pass must be explicitly present and false.
    bool bPass = true;
    Capture.Result->TryGetBoolField(TEXT("pass"), bPass);
    TestFalse(TEXT("a zero-scaled actor fails the audit"), bPass);

    if (const TSharedPtr<FJsonObject> Finding =
            AuditTestFindFinding(Capture.Result, Label, TEXT("zero_scale")))
    {
        FString Code;
        FString Status;
        FString Severity;
        Finding->TryGetStringField(TEXT("code"), Code);
        Finding->TryGetStringField(TEXT("status"), Status);
        Finding->TryGetStringField(TEXT("severity"), Severity);
        TestEqual(TEXT("zero scale -> AUDIT_ZERO_SCALE"), Code, FString(TEXT("AUDIT_ZERO_SCALE")));
        TestEqual(TEXT("the finding is FLAGGED, not unrunnable"), Status, FString(TEXT("flagged")));
        TestEqual(TEXT("zero scale is an error, not a warning"), Severity, FString(TEXT("error")));

        const TSharedPtr<FJsonObject>* Measurements = nullptr;
        if (Finding->TryGetObjectField(TEXT("measurements"), Measurements) && Measurements)
        {
            // The number the verdict was derived from must be in the report, or the caller
            // cannot check the audit's work.
            double MinAbs = 1.0;
            (*Measurements)->TryGetNumberField(TEXT("minAbsScale"), MinAbs);
            TestEqual(TEXT("the report carries the measured minimum axis scale"), MinAbs, 0.0);
        }
        else
        {
            AddError(TEXT("zero_scale finding carries no measurements block"));
        }
    }
    else
    {
        AddError(TEXT("level.audit did not report the zero-scaled actor"));
    }
    return true;
}

// ---- The ignore tag must silence an actor, and SAY that it did ----

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLevelAuditIgnoreTagSuppressesTest,
    "PinWright.level.audit.IgnoreTagSuppresses",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FLevelAuditIgnoreTagSuppressesTest::RunTest(const FString& Parameters)
{
    UWorld* World = AuditTestEditorWorld();
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("Editor world not available; skipping level.audit ignore-tag test."));
        return true;
    }

    const FString Label = AuditTestLabel(TEXT("Tagged"));
    AActor* Cube = AuditTestSpawnCube(*this, World, Label,
        FVector(AuditTestColX, AuditTestColY, 900.0));
    ON_SCOPE_EXIT
    {
        if (Cube) { Cube->Destroy(); }
    };
    if (!Cube)
    {
        AddError(TEXT("fixture cube did not spawn"));
        return true;
    }
    Cube->SetActorScale3D(FVector(0.0, 1.0, 1.0));
    Cube->Tags.AddUnique(FName(TEXT("PinWright.AuditIgnore")));

    TSharedPtr<FJsonObject> Payload = AuditTestPayloadFor(Label);
    AuditTestSetChecks(Payload, {TEXT("zero_scale")});

    FTestResponseCapture Capture;
    InvokeHandlerWithCapture(TEXT("level.audit"), Payload, Capture);
    TestTrue(TEXT("the audit call succeeds"), Capture.bSuccess);
    if (!Capture.bSuccess || !Capture.Result.IsValid())
    {
        return true;
    }

    TestNull(TEXT("a tagged actor produces no finding"),
        AuditTestFindFinding(Capture.Result, Label, TEXT("zero_scale")).Get());

    // Suppression must be VISIBLE. An ignored actor counted as clean would mean the audit's
    // clean count silently includes everything anyone ever exempted.
    if (const TSharedPtr<FJsonObject> Row = AuditTestFindCheckRow(Capture.Result,
                                                                 TEXT("zero_scale")))
    {
        double Ignored = 0.0;
        double Clean = 1.0;
        Row->TryGetNumberField(TEXT("ignored"), Ignored);
        Row->TryGetNumberField(TEXT("clean"), Clean);
        TestEqual(TEXT("the tagged actor is counted as ignored"), Ignored, 1.0);
        TestEqual(TEXT("an ignored actor is NOT counted as clean"), Clean, 0.0);
    }
    else
    {
        AddError(TEXT("level.audit result has no checks[] row for zero_scale"));
    }
    return true;
}

// ---- The distinction that matters: buried vs. deliberately bedded ----

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLevelAuditBuriedVsBeddedTest,
    "PinWright.level.audit.BuriedActorFoundBeddedActorNot",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FLevelAuditBuriedVsBeddedTest::RunTest(const FString& Parameters)
{
    UWorld* World = AuditTestEditorWorld();
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("Editor world not available; skipping level.audit burial test."));
        return true;
    }

    // Two independent columns, each with its own floor whose TOP face is Z = 0.
    const double BuriedX = AuditTestColX;
    const double BeddedX = AuditTestColX + 1000.0;

    const FString BuriedFloorLabel = AuditTestLabel(TEXT("BuriedFloor"));
    const FString BuriedLabel = AuditTestLabel(TEXT("Buried"));
    const FString BeddedFloorLabel = AuditTestLabel(TEXT("BeddedFloor"));
    const FString BeddedLabel = AuditTestLabel(TEXT("Bedded"));

    AActor* BuriedFloor = AuditTestSpawnCube(*this, World, BuriedFloorLabel,
        FVector(BuriedX, AuditTestColY, -AuditTestCubeHalf));
    // Bounds -350..-250: entirely below the floor above it. Nothing of it is above ground.
    AActor* Buried = AuditTestSpawnCube(*this, World, BuriedLabel,
        FVector(BuriedX, AuditTestColY, -300.0));

    AActor* BeddedFloor = AuditTestSpawnCube(*this, World, BeddedFloorLabel,
        FVector(BeddedX, AuditTestColY, -AuditTestCubeHalf));
    // Bounds -30..70: 30 cm of it is under the surface and 70 cm of it is above. This is what
    // every correctly bedded rock in the project looks like, and its PENETRATION (30 cm) is a
    // larger number than plenty of real defects - which is exactly why penetration alone
    // cannot be the test.
    AActor* Bedded = AuditTestSpawnCube(*this, World, BeddedLabel,
        FVector(BeddedX, AuditTestColY, 20.0));

    ON_SCOPE_EXIT
    {
        if (BuriedFloor) { BuriedFloor->Destroy(); }
        if (Buried)      { Buried->Destroy(); }
        if (BeddedFloor) { BeddedFloor->Destroy(); }
        if (Bedded)      { Bedded->Destroy(); }
    };
    if (!BuriedFloor || !Buried || !BeddedFloor || !Bedded)
    {
        AddError(TEXT("fixture cubes did not spawn"));
        return true;
    }
    AuditTestFlushPhysics(World);

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    TArray<TSharedPtr<FJsonValue>> Names;
    Names.Add(MakeShared<FJsonValueString>(BuriedLabel));
    Names.Add(MakeShared<FJsonValueString>(BeddedLabel));
    Payload->SetArrayField(TEXT("actors"), Names);
    Payload->SetObjectField(TEXT("surface"), AuditTestAnySolidSurface());
    AuditTestSetChecks(Payload, {TEXT("below_surface")});

    FTestResponseCapture Capture;
    TestTrue(TEXT("level.audit handler registered"),
        InvokeHandlerWithCapture(TEXT("level.audit"), Payload, Capture));
    TestTrue(TEXT("the audit call succeeds"), Capture.bSuccess);
    if (!Capture.bSuccess || !Capture.Result.IsValid())
    {
        return true;
    }

    // The buried actor must be found.
    if (const TSharedPtr<FJsonObject> Finding =
            AuditTestFindFinding(Capture.Result, BuriedLabel, TEXT("below_surface")))
    {
        FString Code;
        FString Status;
        Finding->TryGetStringField(TEXT("code"), Code);
        Finding->TryGetStringField(TEXT("status"), Status);
        TestEqual(TEXT("buried -> AUDIT_BELOW_SURFACE"), Code,
            FString(TEXT("AUDIT_BELOW_SURFACE")));
        TestEqual(TEXT("the buried actor is FLAGGED, not merely unmeasurable"), Status,
            FString(TEXT("flagged")));

        const TSharedPtr<FJsonObject>* Measurements = nullptr;
        if (Finding->TryGetObjectField(TEXT("measurements"), Measurements) && Measurements)
        {
            // Bounds top is at Z = -250 and the surface above it is at Z = 0, so the shallowest
            // cover is 250 cm. This is the term no previous instrument here produced: every
            // one of them measured clearance DOWNWARD from the underside and could not ask
            // whether anything was above the actor.
            double Cover = 0.0;
            (*Measurements)->TryGetNumberField(TEXT("coverDepthCm"), Cover);
            TestTrue(*FString::Printf(TEXT("coverDepthCm ~ 250 (got %.2f)"), Cover),
                FMath::Abs(Cover - 250.0) < 5.0);
        }
        else
        {
            AddError(TEXT("below_surface finding carries no measurements block"));
        }
    }
    else
    {
        AddError(TEXT("level.audit did not find the entirely-buried actor"));
    }

    // The deliberately bedded actor must NOT be. If this ever starts firing, the audit flags
    // every foundation and every bedded rock in the map and becomes useless.
    TestNull(TEXT("a deliberately bedded actor is NOT flagged as below the surface"),
        AuditTestFindFinding(Capture.Result, BeddedLabel, TEXT("below_surface")).Get());

    if (const TSharedPtr<FJsonObject> Row = AuditTestFindCheckRow(Capture.Result,
                                                                 TEXT("below_surface")))
    {
        double Flagged = 0.0;
        double Clean = 0.0;
        double Unrunnable = 1.0;
        Row->TryGetNumberField(TEXT("flagged"), Flagged);
        Row->TryGetNumberField(TEXT("clean"), Clean);
        Row->TryGetNumberField(TEXT("unrunnable"), Unrunnable);
        TestEqual(TEXT("exactly one of the two actors is flagged"), Flagged, 1.0);
        TestEqual(TEXT("the bedded actor is counted CLEAN, having actually been measured"),
            Clean, 1.0);
        TestEqual(TEXT("neither actor was unmeasurable"), Unrunnable, 0.0);
    }
    else
    {
        AddError(TEXT("level.audit result has no checks[] row for below_surface"));
    }
    return true;
}

// ---- A check that could not run must say so, and must not pass ----

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLevelAuditUnrunnableIsNotAPassTest,
    "PinWright.level.audit.UnrunnableIsNotAPass",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FLevelAuditUnrunnableIsNotAPassTest::RunTest(const FString& Parameters)
{
    UWorld* World = AuditTestEditorWorld();
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("Editor world not available; skipping level.audit unrunnable test."));
        return true;
    }

    const FString Label = AuditTestLabel(TEXT("Budget"));
    AActor* Cube = AuditTestSpawnCube(*this, World, Label,
        FVector(AuditTestColX, AuditTestColY, 1300.0));
    ON_SCOPE_EXIT
    {
        if (Cube) { Cube->Destroy(); }
    };
    if (!Cube)
    {
        AddError(TEXT("fixture cube did not spawn"));
        return true;
    }

    // A ground budget of zero means no actor gets measured. The honest outcome is a per-actor
    // UNRUNNABLE row, not silence: this project's entire history is verbs that reported
    // success for work they did not do.
    TSharedPtr<FJsonObject> Payload = AuditTestPayloadFor(Label);
    Payload->SetObjectField(TEXT("surface"), AuditTestAnySolidSurface());
    AuditTestSetChecks(Payload, {TEXT("below_surface")});
    Payload->SetNumberField(TEXT("maxGroundActors"), 0);

    FTestResponseCapture Capture;
    InvokeHandlerWithCapture(TEXT("level.audit"), Payload, Capture);
    TestTrue(TEXT("the audit call succeeds"), Capture.bSuccess);
    if (!Capture.bSuccess || !Capture.Result.IsValid())
    {
        return true;
    }

    if (const TSharedPtr<FJsonObject> Finding =
            AuditTestFindFinding(Capture.Result, Label, TEXT("below_surface")))
    {
        FString Status;
        FString Code;
        Finding->TryGetStringField(TEXT("status"), Status);
        Finding->TryGetStringField(TEXT("code"), Code);
        TestEqual(TEXT("an unmeasured actor reports status 'unrunnable'"), Status,
            FString(TEXT("unrunnable")));
        TestEqual(TEXT("the reason is the exhausted budget, named"), Code,
            FString(TEXT("AUDIT_TRACE_BUDGET_EXHAUSTED")));
    }
    else
    {
        AddError(TEXT("level.audit silently skipped an actor it could not measure"));
    }

    if (const TSharedPtr<FJsonObject> Row = AuditTestFindCheckRow(Capture.Result,
                                                                 TEXT("below_surface")))
    {
        double Clean = 1.0;
        double Unrunnable = 0.0;
        Row->TryGetNumberField(TEXT("clean"), Clean);
        Row->TryGetNumberField(TEXT("unrunnable"), Unrunnable);
        TestEqual(TEXT("an unmeasured actor is NOT counted clean"), Clean, 0.0);
        TestEqual(TEXT("it is counted unrunnable"), Unrunnable, 1.0);
    }
    else
    {
        AddError(TEXT("level.audit result has no checks[] row for below_surface"));
    }

    // The whole call must fail. An unmeasured check is not a passed check, whatever failOn
    // says - so even failOn:"none" cannot make this green.
    bool bPass = true;
    Capture.Result->TryGetBoolField(TEXT("pass"), bPass);
    TestFalse(TEXT("a run with an unrunnable check does not pass"), bPass);

    TSharedPtr<FJsonObject> Lenient = AuditTestPayloadFor(Label);
    Lenient->SetObjectField(TEXT("surface"), AuditTestAnySolidSurface());
    AuditTestSetChecks(Lenient, {TEXT("below_surface")});
    Lenient->SetNumberField(TEXT("maxGroundActors"), 0);
    Lenient->SetStringField(TEXT("failOn"), TEXT("none"));
    FTestResponseCapture LenientCapture;
    // Both asserted OUTSIDE the guard. The strict call above proves level.audit runs in
    // this environment, so a failure here is a real regression - and without these two
    // lines the failOn assertion below is silently skipped whenever the lenient call
    // errors, leaving the whole "failOn cannot launder an unrunnable check" rule
    // unprotected while the test still reports success.
    TestTrue(TEXT("level.audit handler registered (lenient failOn:'none' call)"),
        InvokeHandlerWithCapture(TEXT("level.audit"), Lenient, LenientCapture));
    TestTrue(TEXT("the lenient failOn:'none' call succeeded"), LenientCapture.bSuccess);
    if (LenientCapture.bSuccess && LenientCapture.Result.IsValid())
    {
        bool bLenientPass = true;
        LenientCapture.Result->TryGetBoolField(TEXT("pass"), bLenientPass);
        TestFalse(TEXT("failOn:'none' still cannot pass an unrunnable check"), bLenientPass);
    }
    return true;
}

// ---- A null mesh must be found; an actor with no mesh component is not-applicable ----

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLevelAuditFindsNullMeshTest,
    "PinWright.level.audit.FindsNullMesh",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FLevelAuditFindsNullMeshTest::RunTest(const FString& Parameters)
{
    UWorld* World = AuditTestEditorWorld();
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("Editor world not available; skipping level.audit null-mesh test."));
        return true;
    }

    const FString Label = AuditTestLabel(TEXT("NullMesh"));
    AActor* Cube = AuditTestSpawnCube(*this, World, Label,
        FVector(AuditTestColX, AuditTestColY, 1700.0));
    ON_SCOPE_EXIT
    {
        if (Cube) { Cube->Destroy(); }
    };
    if (!Cube)
    {
        AddError(TEXT("fixture cube did not spawn"));
        return true;
    }

    TArray<UStaticMeshComponent*> Meshes;
    Cube->GetComponents<UStaticMeshComponent>(Meshes);
    if (Meshes.Num() == 0 || !Meshes[0])
    {
        AddError(TEXT("fixture cube has no static mesh component to clear"));
        return true;
    }
    Meshes[0]->SetStaticMesh(nullptr);

    TSharedPtr<FJsonObject> Payload = AuditTestPayloadFor(Label);
    AuditTestSetChecks(Payload, {TEXT("no_mesh")});

    FTestResponseCapture Capture;
    InvokeHandlerWithCapture(TEXT("level.audit"), Payload, Capture);
    TestTrue(TEXT("the audit call succeeds"), Capture.bSuccess);
    if (!Capture.bSuccess || !Capture.Result.IsValid())
    {
        return true;
    }

    if (const TSharedPtr<FJsonObject> Finding =
            AuditTestFindFinding(Capture.Result, Label, TEXT("no_mesh")))
    {
        FString Code;
        FString Status;
        Finding->TryGetStringField(TEXT("code"), Code);
        Finding->TryGetStringField(TEXT("status"), Status);
        TestEqual(TEXT("null mesh -> AUDIT_MISSING_MESH"), Code,
            FString(TEXT("AUDIT_MISSING_MESH")));
        TestEqual(TEXT("the finding is flagged"), Status, FString(TEXT("flagged")));
    }
    else
    {
        AddError(TEXT("level.audit did not report the actor whose mesh asset is null"));
    }
    return true;
}

// ---- The tally identities: an actor cannot fall out of every bucket ----

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLevelAuditTallyIdentitiesHoldTest,
    "PinWright.level.audit.TallyIdentitiesHold",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FLevelAuditTallyIdentitiesHoldTest::RunTest(const FString& Parameters)
{
    // This is the structural test behind every honesty claim the verb makes. If a check ever
    // stops evaluating some actors without saying so, the buckets stop summing and this fails,
    // where a "no new failures" run would not notice.
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("detail"), TEXT("summary"));
    Payload->SetNumberField(TEXT("limit"), 400);

    FTestResponseCapture Capture;
    TestTrue(TEXT("level.audit handler registered"),
        InvokeHandlerWithCapture(TEXT("level.audit"), Payload, Capture));
    TestTrue(TEXT("the whole-level audit succeeds"), Capture.bSuccess);
    if (!Capture.bSuccess || !Capture.Result.IsValid())
    {
        return true;
    }

    const TSharedPtr<FJsonObject>* Summary = nullptr;
    if (!Capture.Result->TryGetObjectField(TEXT("summary"), Summary) || !Summary)
    {
        AddError(TEXT("level.audit result has no summary block"));
        return true;
    }
    double Examined = -1.0;
    (*Summary)->TryGetNumberField(TEXT("actorsExamined"), Examined);
    TestTrue(TEXT("the audit examined at least one actor"), Examined > 0.0);

    const TArray<TSharedPtr<FJsonValue>>* Rows = nullptr;
    if (!Capture.Result->TryGetArrayField(TEXT("checks"), Rows) || !Rows)
    {
        AddError(TEXT("level.audit result has no checks[] array"));
        return true;
    }

    int32 SelectedChecks = 0;
    for (const TSharedPtr<FJsonValue>& Value : *Rows)
    {
        const TSharedPtr<FJsonObject>* Row = nullptr;
        if (!Value.IsValid() || !Value->TryGetObject(Row) || !Row || !(*Row).IsValid())
        {
            continue;
        }
        bool bSelected = false;
        (*Row)->TryGetBoolField(TEXT("selected"), bSelected);
        if (!bSelected)
        {
            continue;
        }
        ++SelectedChecks;

        FString Id;
        (*Row)->TryGetStringField(TEXT("id"), Id);
        double Applicable = -1.0;
        double NotApplicable = -1.0;
        double Ignored = -1.0;
        double Flagged = -1.0;
        double Unrunnable = -1.0;
        double Clean = -1.0;
        (*Row)->TryGetNumberField(TEXT("applicable"), Applicable);
        (*Row)->TryGetNumberField(TEXT("notApplicable"), NotApplicable);
        (*Row)->TryGetNumberField(TEXT("ignored"), Ignored);
        (*Row)->TryGetNumberField(TEXT("flagged"), Flagged);
        (*Row)->TryGetNumberField(TEXT("unrunnable"), Unrunnable);
        (*Row)->TryGetNumberField(TEXT("clean"), Clean);

        TestEqual(*FString::Printf(
            TEXT("%s: applicable + notApplicable + ignored == actorsExamined"), *Id),
            Applicable + NotApplicable + Ignored, Examined);
        TestEqual(*FString::Printf(
            TEXT("%s: flagged + unrunnable + clean == applicable"), *Id),
            Flagged + Unrunnable + Clean, Applicable);
    }
    TestTrue(TEXT("the default check set is not empty"), SelectedChecks > 0);
    return true;
}

// ---- The audit must not move anything ----

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLevelAuditIsNonMutatingTest,
    "PinWright.level.audit.IsNonMutating",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FLevelAuditIsNonMutatingTest::RunTest(const FString& Parameters)
{
    UWorld* World = AuditTestEditorWorld();
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("Editor world not available; skipping level.audit non-mutation test."));
        return true;
    }

    // A lint that "helpfully" fixes what it finds is a lint nobody can run on a shared map.
    const FString FloorLabel = AuditTestLabel(TEXT("StillFloor"));
    const FString FloatLabel = AuditTestLabel(TEXT("StillFloater"));
    AActor* Floor = AuditTestSpawnCube(*this, World, FloorLabel,
        FVector(AuditTestColX + 2000.0, AuditTestColY, -AuditTestCubeHalf));
    AActor* Floater = AuditTestSpawnCube(*this, World, FloatLabel,
        FVector(AuditTestColX + 2000.0, AuditTestColY, 400.0));
    ON_SCOPE_EXIT
    {
        if (Floor)   { Floor->Destroy(); }
        if (Floater) { Floater->Destroy(); }
    };
    if (!Floor || !Floater)
    {
        AddError(TEXT("fixture cubes did not spawn"));
        return true;
    }
    AuditTestFlushPhysics(World);

    const FTransform Before = Floater->GetActorTransform();

    TSharedPtr<FJsonObject> Payload = AuditTestPayloadFor(FloatLabel);
    Payload->SetObjectField(TEXT("surface"), AuditTestAnySolidSurface());
    AuditTestSetChecks(Payload, {TEXT("airborne"), TEXT("below_surface"), TEXT("balanced")});

    FTestResponseCapture Capture;
    InvokeHandlerWithCapture(TEXT("level.audit"), Payload, Capture);
    TestTrue(TEXT("the audit call succeeds"), Capture.bSuccess);

    // The floating actor must be REPORTED...
    if (Capture.bSuccess && Capture.Result.IsValid())
    {
        TestNotNull(TEXT("a floating actor is reported airborne"),
            AuditTestFindFinding(Capture.Result, FloatLabel, TEXT("airborne")).Get());
    }
    // ...and re-read from the engine, not moved by the reporting.
    const FTransform After = Floater->GetActorTransform();
    TestTrue(TEXT("the audit did not move the actor it flagged"),
        After.GetLocation().Equals(Before.GetLocation(), 0.001));
    TestTrue(TEXT("the audit did not rescale the actor it flagged"),
        After.GetScale3D().Equals(Before.GetScale3D(), 0.001));
    return true;
}
