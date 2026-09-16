// Copyright (c) 2026 Alexander Penkin. MIT License.

// Unit tests for the drive contract: FDriveObservation -> JSON round-trip plus
// FDriveCondition / FDriveSettleConfig parsing from a handler-style args object.

#include "Misc/AutomationTest.h"

#include "Handlers/Drive/DriveTypes.h"
#include "Handlers/Drive/DriveJson.h"
#include "Handlers/Drive/DriveFingerprint.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonSerializer.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDriveObservationJsonRoundTripTest,
    "PinWright.drive.contract.ObservationJsonRoundTrip",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FDriveObservationJsonRoundTripTest::RunTest(const FString& Parameters)
{
    FDriveObservation Observation;
    Observation.Surface = EDriveSurface::Game;
    Observation.RootName = TEXT("WBP_PlayerHUD_C_0");
    Observation.Frame = 1234;

    FDriveElement Element;
    Element.Handle = TEXT("el_1");
    Element.Type = TEXT("Button");
    Element.Label = TEXT("Start");
    Element.bEnabled = true;
    Element.bVisible = true;
    Element.bFocused = true;
    Element.bInteractable = true;
    Element.AbsolutePosition = FVector2D(10.0, 20.0);
    Element.AbsoluteSize = FVector2D(100.0, 40.0);
    Element.Path = TEXT("/Root/Button");
    Element.Surface = EDriveSurface::Game;
    Element.MarkIndex = 3;
    Observation.Elements.Add(Element);

    FDriveScreenshot Screenshot;
    Screenshot.Base64 = TEXT("QUJD");
    Screenshot.Mime = TEXT("image/png");
    Screenshot.Width = 256;
    Screenshot.Height = 128;
    Screenshot.MarksDrawn = { 3 };
    Observation.Screenshot = Screenshot;

    FDriveJournalEvent Event;
    Event.Id = TEXT("e1");
    Event.Ts = 1.5;
    Event.Domain = TEXT("game");
    Event.Severity = TEXT("info");
    Event.Name = TEXT("run_start");
    Event.Props.Add({ TEXT("run_id"), TEXT("42") });

    FDriveJournalDelta Delta;
    Delta.Events.Add(Event);
    Delta.ChangedVariables.Add({ TEXT("score"), TEXT("100") });
    Delta.Cursor = 7;
    Observation.Journal = Delta;

    const TSharedPtr<FJsonObject> Json = FDriveJson::WriteObservation(Observation);
    if (!TestNotNull(TEXT("observation json built"), Json.Get()))
    {
        return false;
    }

    TestEqual(TEXT("surface"), Json->GetStringField(TEXT("surface")), TEXT("game"));
    TestEqual(TEXT("root_name"), Json->GetStringField(TEXT("root_name")), TEXT("WBP_PlayerHUD_C_0"));
    TestEqual(TEXT("frame"), Json->GetNumberField(TEXT("frame")), 1234.0);
    TestTrue(TEXT("timestamp present"), Json->HasField(TEXT("timestamp")));

    const TArray<TSharedPtr<FJsonValue>>* Elements = nullptr;
    if (!TestTrue(TEXT("elements is an array"), Json->TryGetArrayField(TEXT("elements"), Elements)) ||
        !TestEqual(TEXT("one element"), Elements->Num(), 1))
    {
        return false;
    }

    const TSharedPtr<FJsonObject> ElementJson = (*Elements)[0]->AsObject();
    if (!TestNotNull(TEXT("element is an object"), ElementJson.Get()))
    {
        return false;
    }
    TestEqual(TEXT("element type"), ElementJson->GetStringField(TEXT("type")), TEXT("Button"));
    TestEqual(TEXT("element label"), ElementJson->GetStringField(TEXT("label")), TEXT("Start"));
    TestTrue(TEXT("element focused"), ElementJson->GetBoolField(TEXT("focused")));
    TestTrue(TEXT("element interactable"), ElementJson->GetBoolField(TEXT("interactable")));
    // path and surface are no longer serialized per element (redundant with the short
    // handle and the observation root's surface); the in-memory struct still carries them.
    TestFalse(TEXT("element omits path"), ElementJson->HasField(TEXT("path")));
    TestFalse(TEXT("element omits surface"), ElementJson->HasField(TEXT("surface")));
    TestEqual(TEXT("element mark"), ElementJson->GetNumberField(TEXT("mark")), 3.0);

    const TSharedPtr<FJsonObject>* GeometryJson = nullptr;
    const TSharedPtr<FJsonObject>* AbsoluteJson = nullptr;
    if (TestTrue(TEXT("element has geometry"), ElementJson->TryGetObjectField(TEXT("geometry"), GeometryJson)) &&
        TestTrue(TEXT("geometry has absolute"), (*GeometryJson)->TryGetObjectField(TEXT("absolute"), AbsoluteJson)))
    {
        TestEqual(TEXT("absolute x"), (*AbsoluteJson)->GetNumberField(TEXT("x")), 10.0);
        TestEqual(TEXT("absolute y"), (*AbsoluteJson)->GetNumberField(TEXT("y")), 20.0);
        TestEqual(TEXT("absolute w"), (*AbsoluteJson)->GetNumberField(TEXT("w")), 100.0);
        TestEqual(TEXT("absolute h"), (*AbsoluteJson)->GetNumberField(TEXT("h")), 40.0);
    }

    const TSharedPtr<FJsonObject>* ScreenshotJson = nullptr;
    if (TestTrue(TEXT("has screenshot"), Json->TryGetObjectField(TEXT("screenshot"), ScreenshotJson)))
    {
        TestEqual(TEXT("screenshot mime"), (*ScreenshotJson)->GetStringField(TEXT("mime")), TEXT("image/png"));
        TestEqual(TEXT("screenshot width"), (*ScreenshotJson)->GetNumberField(TEXT("width")), 256.0);
        const TArray<TSharedPtr<FJsonValue>>* MarksDrawn = nullptr;
        if (TestTrue(TEXT("screenshot marks_drawn array"), (*ScreenshotJson)->TryGetArrayField(TEXT("marks_drawn"), MarksDrawn)))
        {
            TestEqual(TEXT("one mark drawn"), MarksDrawn->Num(), 1);
        }
    }

    const TSharedPtr<FJsonObject>* JournalJson = nullptr;
    if (TestTrue(TEXT("has journal"), Json->TryGetObjectField(TEXT("journal"), JournalJson)))
    {
        TestEqual(TEXT("journal cursor"), (*JournalJson)->GetNumberField(TEXT("cursor")), 7.0);
        const TArray<TSharedPtr<FJsonValue>>* Events = nullptr;
        if (TestTrue(TEXT("journal events array"), (*JournalJson)->TryGetArrayField(TEXT("events"), Events)) &&
            TestEqual(TEXT("one event"), Events->Num(), 1))
        {
            const TSharedPtr<FJsonObject> EventJson = (*Events)[0]->AsObject();
            TestEqual(TEXT("event name"), EventJson->GetStringField(TEXT("name")), TEXT("run_start"));
            const TArray<TSharedPtr<FJsonValue>>* Props = nullptr;
            if (TestTrue(TEXT("event props array"), EventJson->TryGetArrayField(TEXT("props"), Props)) &&
                TestEqual(TEXT("one prop"), Props->Num(), 1))
            {
                TestEqual(TEXT("prop key"), (*Props)[0]->AsObject()->GetStringField(TEXT("key")), TEXT("run_id"));
            }
        }
    }

    return true;
}

// ============================================================================
// WriteDiffSummary is the compact diff embedded in action results by default:
// per-category counts + a sample capped at 15 handles + an `omitted` flag.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDriveDiffSummaryTest,
    "PinWright.drive.contract.DiffSummaryCompact",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FDriveDiffSummaryTest::RunTest(const FString& Parameters)
{
    FDriveDiff Diff;
    // 20 appeared (> the 15 cap), 3 disappeared (<= cap), 0 changed.
    for (int32 Index = 0; Index < 20; ++Index)
    {
        Diff.Appeared.Add(FString::Printf(TEXT("app_%02d"), Index));
    }
    Diff.Disappeared = { TEXT("d0"), TEXT("d1"), TEXT("d2") };

    const TSharedPtr<FJsonObject> Json = FDriveJson::WriteDiffSummary(Diff);
    if (!TestNotNull(TEXT("summary json built"), Json.Get()))
    {
        return false;
    }

    // Counts reflect the full category sizes, not the truncated samples.
    TestEqual(TEXT("appeared_count"), Json->GetNumberField(TEXT("appeared_count")), 20.0);
    TestEqual(TEXT("disappeared_count"), Json->GetNumberField(TEXT("disappeared_count")), 3.0);
    TestEqual(TEXT("changed_count"), Json->GetNumberField(TEXT("changed_count")), 0.0);

    const TArray<TSharedPtr<FJsonValue>>* AppearedSample = nullptr;
    if (TestTrue(TEXT("appeared_sample is an array"),
            Json->TryGetArrayField(TEXT("appeared_sample"), AppearedSample)))
    {
        TestEqual(TEXT("appeared_sample is capped at 15"), AppearedSample->Num(), 15);
        TestEqual(TEXT("appeared_sample preserves order"),
            (*AppearedSample)[0]->AsString(), FString(TEXT("app_00")));
    }

    const TArray<TSharedPtr<FJsonValue>>* DisappearedSample = nullptr;
    if (TestTrue(TEXT("disappeared_sample is an array"),
            Json->TryGetArrayField(TEXT("disappeared_sample"), DisappearedSample)))
    {
        TestEqual(TEXT("disappeared_sample carries all 3 (under cap)"), DisappearedSample->Num(), 3);
    }

    // `omitted` is true because appeared exceeded the 15-handle cap.
    TestTrue(TEXT("omitted flag set when a category overflows"), Json->GetBoolField(TEXT("omitted")));

    // A small diff (nothing over the cap) reports omitted=false.
    FDriveDiff Small;
    Small.Changed = { TEXT("c0"), TEXT("c1") };
    const TSharedPtr<FJsonObject> SmallJson = FDriveJson::WriteDiffSummary(Small);
    TestFalse(TEXT("omitted flag clear when nothing truncated"),
        SmallJson->GetBoolField(TEXT("omitted")));
    TestEqual(TEXT("small changed_count"), SmallJson->GetNumberField(TEXT("changed_count")), 2.0);

    return true;
}

// ============================================================================
// WriteDiffFull is the full_diff opt-in: complete appeared/disappeared/changed
// handle lists, no truncation.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDriveDiffFullTest,
    "PinWright.drive.contract.DiffFullLists",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FDriveDiffFullTest::RunTest(const FString& Parameters)
{
    FDriveDiff Diff;
    for (int32 Index = 0; Index < 20; ++Index)
    {
        Diff.Appeared.Add(FString::Printf(TEXT("app_%02d"), Index));
    }
    Diff.Changed = { TEXT("c0") };

    const TSharedPtr<FJsonObject> Json = FDriveJson::WriteDiffFull(Diff);
    if (!TestNotNull(TEXT("full json built"), Json.Get()))
    {
        return false;
    }

    const TArray<TSharedPtr<FJsonValue>>* Appeared = nullptr;
    if (TestTrue(TEXT("appeared is an array"), Json->TryGetArrayField(TEXT("appeared"), Appeared)))
    {
        TestEqual(TEXT("appeared carries ALL 20 handles (no cap)"), Appeared->Num(), 20);
    }
    const TArray<TSharedPtr<FJsonValue>>* Changed = nullptr;
    if (TestTrue(TEXT("changed is an array"), Json->TryGetArrayField(TEXT("changed"), Changed)))
    {
        TestEqual(TEXT("changed carries 1 handle"), Changed->Num(), 1);
    }
    // The full form has no summary fields.
    TestFalse(TEXT("full form omits appeared_count"), Json->HasField(TEXT("appeared_count")));
    TestFalse(TEXT("full form omits omitted flag"), Json->HasField(TEXT("omitted")));

    return true;
}

// ============================================================================
// WriteElement serializes a single element (used for wait_for's `matched`).
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDriveWriteElementTest,
    "PinWright.drive.contract.WriteSingleElement",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FDriveWriteElementTest::RunTest(const FString& Parameters)
{
    FDriveElement Element;
    Element.Handle = TEXT("pw-7");
    Element.Type = TEXT("Button");
    Element.Label = TEXT("Play");
    Element.bInteractable = true;
    Element.Surface = EDriveSurface::Game;

    const TSharedPtr<FJsonObject> Json = FDriveJson::WriteElement(Element);
    if (!TestNotNull(TEXT("element json built"), Json.Get()))
    {
        return false;
    }
    TestEqual(TEXT("handle"), Json->GetStringField(TEXT("handle")), TEXT("pw-7"));
    TestEqual(TEXT("label"), Json->GetStringField(TEXT("label")), TEXT("Play"));
    TestTrue(TEXT("interactable"), Json->GetBoolField(TEXT("interactable")));
    TestTrue(TEXT("has geometry"), Json->HasField(TEXT("geometry")));
    return true;
}

// ============================================================================
// WriteElement rounds the serialized geometry to whole pixels (payload size),
// while leaving the in-memory FDriveElement geometry at full precision so the
// click-center math elsewhere stays accurate.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDriveWriteElementGeometryRoundedTest,
    "PinWright.drive.contract.WriteElementGeometryRounded",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FDriveWriteElementGeometryRoundedTest::RunTest(const FString& Parameters)
{
    FDriveElement Element;
    Element.Handle = TEXT("h");
    Element.Type = TEXT("SButton");
    Element.Surface = EDriveSurface::Game;
    Element.AbsolutePosition = FVector2D(100.6, 200.4);
    Element.AbsoluteSize = FVector2D(49.5, 80.49);

    const TSharedPtr<FJsonObject> Json = FDriveJson::WriteElement(Element);
    if (!TestNotNull(TEXT("element json built"), Json.Get()))
    {
        return false;
    }

    const TSharedPtr<FJsonObject>* GeometryJson = nullptr;
    const TSharedPtr<FJsonObject>* AbsoluteJson = nullptr;
    if (TestTrue(TEXT("element has geometry"), Json->TryGetObjectField(TEXT("geometry"), GeometryJson)) &&
        TestTrue(TEXT("geometry has absolute"), (*GeometryJson)->TryGetObjectField(TEXT("absolute"), AbsoluteJson)))
    {
        // Rounded to the nearest whole pixel (RoundToInt: .5 rounds up).
        TestEqual(TEXT("absolute x rounded"), (*AbsoluteJson)->GetNumberField(TEXT("x")), 101.0);
        TestEqual(TEXT("absolute y rounded"), (*AbsoluteJson)->GetNumberField(TEXT("y")), 200.0);
        TestEqual(TEXT("absolute w rounded"), (*AbsoluteJson)->GetNumberField(TEXT("w")), 50.0);
        TestEqual(TEXT("absolute h rounded"), (*AbsoluteJson)->GetNumberField(TEXT("h")), 80.0);

        // Each emitted value is integral (no fractional pixel survives into the JSON).
        TestTrue(TEXT("x is integral"),
            FMath::IsNearlyZero(FMath::Frac((*AbsoluteJson)->GetNumberField(TEXT("x")))));
        TestTrue(TEXT("h is integral"),
            FMath::IsNearlyZero(FMath::Frac((*AbsoluteJson)->GetNumberField(TEXT("h")))));
    }

    // Serialized output carries the geometry as integers - no decimal point anywhere. The
    // element's other fields are dot-free strings/bools, so any '.' would be a fractional pixel.
    FString Serialized;
    const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Serialized);
    if (TestTrue(TEXT("element serializes"), FJsonSerializer::Serialize(Json.ToSharedRef(), Writer)))
    {
        TestFalse(TEXT("no fractional pixel in serialized geometry"), Serialized.Contains(TEXT(".")));
    }

    // The in-memory struct is untouched: full precision preserved for click-center math.
    TestEqual(TEXT("struct position X kept full precision"), Element.AbsolutePosition.X, 100.6);
    TestEqual(TEXT("struct size Y kept full precision"), Element.AbsoluteSize.Y, 80.49);

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDriveConditionParseTest,
    "PinWright.drive.contract.ConditionParse",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FDriveConditionParseTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
    Args->SetStringField(TEXT("type"), TEXT("text_equals"));
    Args->SetStringField(TEXT("target"), TEXT("ScoreLabel"));
    Args->SetStringField(TEXT("expected_text"), TEXT("100"));
    Args->SetNumberField(TEXT("expected_count"), 3);
    Args->SetStringField(TEXT("count_op"), TEXT("gte"));
    Args->SetStringField(TEXT("severity"), TEXT("error"));

    TSharedPtr<FJsonObject> Min = MakeShared<FJsonObject>();
    Min->SetNumberField(TEXT("x"), 0.0);
    Min->SetNumberField(TEXT("y"), 0.0);
    TSharedPtr<FJsonObject> Max = MakeShared<FJsonObject>();
    Max->SetNumberField(TEXT("x"), 50.0);
    Max->SetNumberField(TEXT("y"), 60.0);
    TSharedPtr<FJsonObject> Bounds = MakeShared<FJsonObject>();
    Bounds->SetObjectField(TEXT("min"), Min);
    Bounds->SetObjectField(TEXT("max"), Max);
    Args->SetObjectField(TEXT("expected_bounds"), Bounds);

    FDriveCondition Condition;
    if (!TestTrue(TEXT("condition parses"), FDriveJson::ParseCondition(Args, Condition)))
    {
        return false;
    }

    TestEqual(TEXT("type"), static_cast<int32>(Condition.Type), static_cast<int32>(EDriveConditionType::TextEquals));
    TestEqual(TEXT("target"), Condition.Target, TEXT("ScoreLabel"));
    TestEqual(TEXT("expected text"), Condition.ExpectedText, TEXT("100"));
    TestEqual(TEXT("expected count"), Condition.ExpectedCount, 3);
    TestEqual(TEXT("count op"), static_cast<int32>(Condition.CountOp), static_cast<int32>(EDriveCompareOp::GreaterOrEqual));
    TestEqual(TEXT("severity"), Condition.SeverityThreshold, TEXT("error"));
    TestTrue(TEXT("bounds valid"), Condition.ExpectedBounds.bIsValid);
    TestEqual(TEXT("bounds max x"), Condition.ExpectedBounds.Max.X, 50.0);

    // An unrecognized type is a parse failure.
    TSharedPtr<FJsonObject> BadArgs = MakeShared<FJsonObject>();
    BadArgs->SetStringField(TEXT("type"), TEXT("not_a_real_condition"));
    FDriveCondition Unused;
    TestFalse(TEXT("unknown condition type rejected"), FDriveJson::ParseCondition(BadArgs, Unused));

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDriveSettleConfigParseTest,
    "PinWright.drive.contract.SettleConfigParse",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FDriveSettleConfigParseTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
    Args->SetNumberField(TEXT("stable_ticks"), 4);
    Args->SetNumberField(TEXT("quiet_budget_ms"), 250);
    Args->SetNumberField(TEXT("settle_budget_ms"), 2000);
    Args->SetNumberField(TEXT("wait_for_timeout_ms"), 8000);

    TSharedPtr<FJsonObject> WaitFor = MakeShared<FJsonObject>();
    WaitFor->SetStringField(TEXT("type"), TEXT("widget_visible"));
    WaitFor->SetStringField(TEXT("target"), TEXT("PlayButton"));
    Args->SetObjectField(TEXT("wait_for"), WaitFor);

    FDriveSettleConfig Config;
    if (!TestTrue(TEXT("settle config parses"), FDriveJson::ParseSettleConfig(Args, Config)))
    {
        return false;
    }

    TestEqual(TEXT("stable ticks"), Config.StableTicks, 4);
    TestEqual(TEXT("quiet budget"), Config.QuietBudgetMs, 250);
    TestEqual(TEXT("settle budget"), Config.SettleBudgetMs, 2000);
    TestEqual(TEXT("wait for timeout"), Config.WaitForTimeoutMs, 8000);
    if (TestTrue(TEXT("wait_for is set"), Config.WaitFor.IsSet()))
    {
        TestEqual(TEXT("wait_for type"),
            static_cast<int32>(Config.WaitFor.GetValue().Type),
            static_cast<int32>(EDriveConditionType::WidgetVisible));
        TestEqual(TEXT("wait_for target"), Config.WaitFor.GetValue().Target, TEXT("PlayButton"));
    }

    // An empty args object keeps the defaults.
    TSharedPtr<FJsonObject> Empty = MakeShared<FJsonObject>();
    FDriveSettleConfig Defaults;
    if (TestTrue(TEXT("empty settle config parses"), FDriveJson::ParseSettleConfig(Empty, Defaults)))
    {
        TestEqual(TEXT("default stable ticks"), Defaults.StableTicks, 2);
        TestEqual(TEXT("default quiet budget"), Defaults.QuietBudgetMs, 500);
        TestEqual(TEXT("default settle budget"), Defaults.SettleBudgetMs, 1500);
        TestEqual(TEXT("default wait for timeout"), Defaults.WaitForTimeoutMs, 5000);
        TestFalse(TEXT("no wait_for by default"), Defaults.WaitFor.IsSet());
    }

    return true;
}
