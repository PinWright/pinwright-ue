// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Handlers/Drive/DriveJson.h"

#include "Utils/JsonBuilders.h"
#include "Utils/JsonUtils.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

namespace
{
    using JsonBuilders::MakeObject;

    // Note: this using-declaration is scoped to this anonymous namespace; the
    // out-of-namespace FDriveJson members below call JsonBuilders::MakeObject
    // qualified.

    TSharedPtr<FJsonValue> MakeIntArray(const TArray<int32>& Values)
    {
        TArray<TSharedPtr<FJsonValue>> Out;
        Out.Reserve(Values.Num());
        for (int32 Value : Values)
        {
            Out.Add(MakeShared<FJsonValueNumber>(Value));
        }
        return MakeShared<FJsonValueArray>(Out);
    }

    // Reads a { "x":.., "y":.. } object into an FVector2D (missing keys -> 0).
    FVector2D ReadVector2D(const TSharedPtr<FJsonObject>& Obj)
    {
        return FVector2D(
            GetJsonNumberField(Obj, TEXT("x")),
            GetJsonNumberField(Obj, TEXT("y")));
    }

    // Reads a { "min": {x,y}, "max": {x,y} } object into an FBox2D and marks it valid.
    void ReadBox2D(const TSharedPtr<FJsonObject>& Obj, FBox2D& OutBox)
    {
        const TSharedPtr<FJsonObject>* MinObj = nullptr;
        const TSharedPtr<FJsonObject>* MaxObj = nullptr;
        if (Obj->TryGetObjectField(TEXT("min"), MinObj) && MinObj)
        {
            OutBox.Min = ReadVector2D(*MinObj);
        }
        if (Obj->TryGetObjectField(TEXT("max"), MaxObj) && MaxObj)
        {
            OutBox.Max = ReadVector2D(*MaxObj);
        }
        OutBox.bIsValid = true;
    }

    TSharedPtr<FJsonObject> WriteScreenshot(const FDriveScreenshot& Screenshot)
    {
        TSharedPtr<FJsonObject> Obj = MakeObject();
        // Delivery is one of two mutually-exclusive shapes: a `path` to a written PNG
        // (screenshot_mode=file — keeps the observation small and directly Read-able) or an
        // inline `base64` blob (default). Only the populated field is emitted, so a file-mode
        // observation never carries the large base64 string.
        if (!Screenshot.Path.IsEmpty())
        {
            Obj->SetStringField(TEXT("path"), Screenshot.Path);
        }
        if (!Screenshot.Base64.IsEmpty())
        {
            Obj->SetStringField(TEXT("base64"), Screenshot.Base64);
        }
        Obj->SetStringField(TEXT("mime"), Screenshot.Mime);
        Obj->SetNumberField(TEXT("width"), Screenshot.Width);
        Obj->SetNumberField(TEXT("height"), Screenshot.Height);
        Obj->SetField(TEXT("marks_drawn"), MakeIntArray(Screenshot.MarksDrawn));
        Obj->SetField(TEXT("marks_omitted"), MakeIntArray(Screenshot.MarksOmitted));
        return Obj;
    }

    TSharedPtr<FJsonObject> WriteJournalEvent(const FDriveJournalEvent& Event)
    {
        TSharedPtr<FJsonObject> Obj = MakeObject();
        Obj->SetStringField(TEXT("id"), Event.Id);
        Obj->SetNumberField(TEXT("ts"), Event.Ts);
        Obj->SetStringField(TEXT("domain"), Event.Domain);
        Obj->SetStringField(TEXT("severity"), Event.Severity);
        Obj->SetStringField(TEXT("name"), Event.Name);

        TArray<TSharedPtr<FJsonValue>> Props;
        Props.Reserve(Event.Props.Num());
        for (const FDriveJournalProp& Prop : Event.Props)
        {
            TSharedPtr<FJsonObject> PropObj = MakeObject();
            PropObj->SetStringField(TEXT("key"), Prop.Key);
            PropObj->SetStringField(TEXT("value"), Prop.Value);
            Props.Add(MakeShared<FJsonValueObject>(PropObj));
        }
        Obj->SetArrayField(TEXT("props"), Props);
        return Obj;
    }
}

TSharedPtr<FJsonObject> FDriveJson::WriteElement(const FDriveElement& Element)
{
    TSharedPtr<FJsonObject> Obj = JsonBuilders::MakeObject();
    Obj->SetStringField(TEXT("handle"), Element.Handle);
    Obj->SetStringField(TEXT("type"), Element.Type);
    if (!Element.Label.IsEmpty())
    {
        Obj->SetStringField(TEXT("label"), Element.Label);
    }
    // Live typed value of an editable widget (omitted when empty); distinct from label because
    // label falls back to the widget's accessible/HINT text for an editable field.
    if (!Element.Value.IsEmpty())
    {
        Obj->SetStringField(TEXT("value"), Element.Value);
    }
    Obj->SetBoolField(TEXT("enabled"), Element.bEnabled);
    Obj->SetBoolField(TEXT("visible"), Element.bVisible);
    Obj->SetBoolField(TEXT("focused"), Element.bFocused);
    Obj->SetBoolField(TEXT("interactable"), Element.bInteractable);

    // Geometry mirrors the live-snapshot shape: geometry.absolute = {x,y,w,h}. The emitted
    // values are rounded to whole pixels to keep the payload small - whole-pixel addressing is
    // all a caller needs. FDriveElement keeps the full-precision FVector2D fields untouched, so
    // click-center math elsewhere stays accurate; only this JSON output is rounded.
    TSharedPtr<FJsonObject> Absolute = JsonBuilders::MakeObject();
    Absolute->SetNumberField(TEXT("x"), FMath::RoundToInt(Element.AbsolutePosition.X));
    Absolute->SetNumberField(TEXT("y"), FMath::RoundToInt(Element.AbsolutePosition.Y));
    Absolute->SetNumberField(TEXT("w"), FMath::RoundToInt(Element.AbsoluteSize.X));
    Absolute->SetNumberField(TEXT("h"), FMath::RoundToInt(Element.AbsoluteSize.Y));
    TSharedPtr<FJsonObject> Geometry = JsonBuilders::MakeObject();
    Geometry->SetObjectField(TEXT("absolute"), Absolute);
    // Name the coordinate space in the response rather than leaving a caller to infer it from
    // the wiki: these are desktop pixels (the window's screen origin is already included), the
    // same space drive's own injection and any external OS-level injector consume.
    Geometry->SetStringField(TEXT("space"), TEXT("desktop"));
    // Emitted only when the rect is NOT this frame's arrangement (the element or an ancestor is
    // Collapsed/Hidden, so Slate never measured it); absence means the rect is live. Zeroed
    // rather than last-frame numbers, so a caller that ignores the flag cannot aim at a plausible
    // wrong point.
    if (Element.bGeometryStale)
    {
        Geometry->SetBoolField(TEXT("stale"), true);
    }
    Obj->SetObjectField(TEXT("geometry"), Geometry);

    if (Element.MarkIndex.IsSet())
    {
        Obj->SetNumberField(TEXT("mark"), Element.MarkIndex.GetValue());
    }
    return Obj;
}

TSharedPtr<FJsonObject> FDriveJson::WriteDiffFull(const FDriveDiff& Diff)
{
    auto ToArray = [](const TArray<FString>& Handles)
    {
        TArray<TSharedPtr<FJsonValue>> Out;
        Out.Reserve(Handles.Num());
        for (const FString& Handle : Handles)
        {
            Out.Add(MakeShared<FJsonValueString>(Handle));
        }
        return Out;
    };

    TSharedPtr<FJsonObject> Obj = JsonBuilders::MakeObject();
    Obj->SetArrayField(TEXT("appeared"), ToArray(Diff.Appeared));
    Obj->SetArrayField(TEXT("disappeared"), ToArray(Diff.Disappeared));
    Obj->SetArrayField(TEXT("changed"), ToArray(Diff.Changed));
    return Obj;
}

TSharedPtr<FJsonObject> FDriveJson::WriteDiffSummary(const FDriveDiff& Diff)
{
    // Sample cap keeps the diff bounded on rich HUDs (hundreds of handles); the full
    // lists remain available via the action verbs' full_diff opt-in.
    constexpr int32 SampleCap = 15;

    auto WriteCategory =
        [SampleCap](const TSharedPtr<FJsonObject>& Obj, const TCHAR* CountKey, const TCHAR* SampleKey,
           const TArray<FString>& Handles)
    {
        Obj->SetNumberField(CountKey, Handles.Num());
        const int32 Num = FMath::Min(SampleCap, Handles.Num());
        TArray<TSharedPtr<FJsonValue>> Sample;
        Sample.Reserve(Num);
        for (int32 Index = 0; Index < Num; ++Index)
        {
            Sample.Add(MakeShared<FJsonValueString>(Handles[Index]));
        }
        Obj->SetArrayField(SampleKey, Sample);
    };

    TSharedPtr<FJsonObject> Obj = JsonBuilders::MakeObject();
    WriteCategory(Obj, TEXT("appeared_count"), TEXT("appeared_sample"), Diff.Appeared);
    WriteCategory(Obj, TEXT("disappeared_count"), TEXT("disappeared_sample"), Diff.Disappeared);
    WriteCategory(Obj, TEXT("changed_count"), TEXT("changed_sample"), Diff.Changed);

    const bool bOmitted =
        Diff.Appeared.Num() > SampleCap ||
        Diff.Disappeared.Num() > SampleCap ||
        Diff.Changed.Num() > SampleCap;
    Obj->SetBoolField(TEXT("omitted"), bOmitted);
    return Obj;
}

TSharedPtr<FJsonObject> FDriveJson::WriteObservation(const FDriveObservation& Observation)
{
    TSharedPtr<FJsonObject> Root = JsonBuilders::MakeObject();
    Root->SetStringField(TEXT("surface"), SurfaceToString(Observation.Surface));
    Root->SetStringField(TEXT("root_name"), Observation.RootName);
    Root->SetNumberField(TEXT("frame"), static_cast<double>(Observation.Frame));
    Root->SetStringField(TEXT("timestamp"), Observation.Timestamp.ToIso8601());

    TArray<TSharedPtr<FJsonValue>> Elements;
    Elements.Reserve(Observation.Elements.Num());
    for (const FDriveElement& Element : Observation.Elements)
    {
        Elements.Add(MakeShared<FJsonValueObject>(WriteElement(Element)));
    }
    Root->SetArrayField(TEXT("elements"), Elements);

    // Only emitted when a max_elements cap actually dropped elements, so a caller never
    // mistakes a shortened list for the full surface.
    if (Observation.OmittedCount > 0)
    {
        Root->SetNumberField(TEXT("omitted_count"), Observation.OmittedCount);
    }

    if (Observation.Screenshot.IsSet())
    {
        Root->SetObjectField(TEXT("screenshot"), WriteScreenshot(Observation.Screenshot.GetValue()));
    }
    if (Observation.Journal.IsSet())
    {
        Root->SetObjectField(TEXT("journal"), WriteJournalDelta(Observation.Journal.GetValue()));
    }
    return Root;
}

TSharedPtr<FJsonObject> FDriveJson::WriteJournalDelta(const FDriveJournalDelta& Delta)
{
    TSharedPtr<FJsonObject> Obj = JsonBuilders::MakeObject();

    TArray<TSharedPtr<FJsonValue>> Events;
    Events.Reserve(Delta.Events.Num());
    for (const FDriveJournalEvent& Event : Delta.Events)
    {
        Events.Add(MakeShared<FJsonValueObject>(WriteJournalEvent(Event)));
    }
    Obj->SetArrayField(TEXT("events"), Events);

    TArray<TSharedPtr<FJsonValue>> Variables;
    Variables.Reserve(Delta.ChangedVariables.Num());
    for (const FDriveJournalVariable& Variable : Delta.ChangedVariables)
    {
        TSharedPtr<FJsonObject> VarObj = JsonBuilders::MakeObject();
        VarObj->SetStringField(TEXT("name"), Variable.Name);
        VarObj->SetStringField(TEXT("value"), Variable.Value);
        Variables.Add(MakeShared<FJsonValueObject>(VarObj));
    }
    Obj->SetArrayField(TEXT("changed_variables"), Variables);

    Obj->SetNumberField(TEXT("cursor"), static_cast<double>(Delta.Cursor));
    return Obj;
}

bool FDriveJson::ParseCondition(const TSharedPtr<FJsonObject>& Json, FDriveCondition& OutCondition)
{
    if (!Json.IsValid())
    {
        return false;
    }

    const FString TypeToken = GetJsonStringField(Json, TEXT("type"));
    if (!ConditionTypeFromString(TypeToken, OutCondition.Type))
    {
        return false;
    }

    OutCondition.Target = GetJsonStringField(Json, TEXT("target"));
    OutCondition.ExpectedText = GetJsonStringField(Json, TEXT("expected_text"));
    OutCondition.ExpectedCount = GetJsonIntField(Json, TEXT("expected_count"), OutCondition.ExpectedCount);

    const FString OpToken = GetJsonStringField(Json, TEXT("count_op"));
    if (!OpToken.IsEmpty())
    {
        CompareOpFromString(OpToken, OutCondition.CountOp);
    }

    const TSharedPtr<FJsonObject>* BoundsObj = nullptr;
    if (Json->TryGetObjectField(TEXT("expected_bounds"), BoundsObj) && BoundsObj)
    {
        ReadBox2D(*BoundsObj, OutCondition.ExpectedBounds);
    }

    OutCondition.SeverityThreshold = GetJsonStringField(Json, TEXT("severity"));
    return true;
}

bool FDriveJson::ParseSettleConfig(const TSharedPtr<FJsonObject>& Json, FDriveSettleConfig& OutConfig)
{
    if (!Json.IsValid())
    {
        return true;
    }

    OutConfig.StableTicks = GetJsonIntField(Json, TEXT("stable_ticks"), OutConfig.StableTicks);
    OutConfig.QuietBudgetMs = GetJsonIntField(Json, TEXT("quiet_budget_ms"), OutConfig.QuietBudgetMs);
    OutConfig.SettleBudgetMs = GetJsonIntField(Json, TEXT("settle_budget_ms"), OutConfig.SettleBudgetMs);
    OutConfig.WaitForTimeoutMs = GetJsonIntField(Json, TEXT("wait_for_timeout_ms"), OutConfig.WaitForTimeoutMs);

    const TSharedPtr<FJsonObject>* WaitForObj = nullptr;
    if (Json->TryGetObjectField(TEXT("wait_for"), WaitForObj) && WaitForObj && WaitForObj->IsValid())
    {
        FDriveCondition Condition;
        if (!ParseCondition(*WaitForObj, Condition))
        {
            return false;
        }
        OutConfig.WaitFor = Condition;
    }
    return true;
}

FString FDriveJson::SurfaceToString(EDriveSurface Surface)
{
    switch (Surface)
    {
    case EDriveSurface::Game:         return TEXT("game");
    case EDriveSurface::EditorChrome: return TEXT("editor_chrome");
    case EDriveSurface::Web:          return TEXT("web");
    case EDriveSurface::Auto:
    default:                          return TEXT("auto");
    }
}

bool FDriveJson::SurfaceFromString(const FString& Token, EDriveSurface& OutSurface)
{
    if (Token.Equals(TEXT("auto"), ESearchCase::IgnoreCase))          { OutSurface = EDriveSurface::Auto; return true; }
    if (Token.Equals(TEXT("game"), ESearchCase::IgnoreCase))          { OutSurface = EDriveSurface::Game; return true; }
    if (Token.Equals(TEXT("editor_chrome"), ESearchCase::IgnoreCase)) { OutSurface = EDriveSurface::EditorChrome; return true; }
    if (Token.Equals(TEXT("web"), ESearchCase::IgnoreCase))           { OutSurface = EDriveSurface::Web; return true; }
    return false;
}

FString FDriveJson::ConditionTypeToString(EDriveConditionType Type)
{
    switch (Type)
    {
    case EDriveConditionType::WidgetPresent:    return TEXT("widget_present");
    case EDriveConditionType::WidgetAbsent:     return TEXT("widget_absent");
    case EDriveConditionType::WidgetEnabled:    return TEXT("widget_enabled");
    case EDriveConditionType::WidgetVisible:    return TEXT("widget_visible");
    case EDriveConditionType::TextEquals:       return TEXT("text_equals");
    case EDriveConditionType::TextContains:     return TEXT("text_contains");
    case EDriveConditionType::Count:            return TEXT("count");
    case EDriveConditionType::GeometryInBounds: return TEXT("geometry_in_bounds");
    case EDriveConditionType::JournalEvent:     return TEXT("journal_event");
    case EDriveConditionType::JournalSeverity:  return TEXT("journal_severity");
    default:                                    return TEXT("widget_present");
    }
}

bool FDriveJson::ConditionTypeFromString(const FString& Token, EDriveConditionType& OutType)
{
    if (Token.Equals(TEXT("widget_present"), ESearchCase::IgnoreCase))     { OutType = EDriveConditionType::WidgetPresent; return true; }
    if (Token.Equals(TEXT("widget_absent"), ESearchCase::IgnoreCase))      { OutType = EDriveConditionType::WidgetAbsent; return true; }
    if (Token.Equals(TEXT("widget_enabled"), ESearchCase::IgnoreCase))     { OutType = EDriveConditionType::WidgetEnabled; return true; }
    if (Token.Equals(TEXT("widget_visible"), ESearchCase::IgnoreCase))     { OutType = EDriveConditionType::WidgetVisible; return true; }
    if (Token.Equals(TEXT("text_equals"), ESearchCase::IgnoreCase))        { OutType = EDriveConditionType::TextEquals; return true; }
    if (Token.Equals(TEXT("text_contains"), ESearchCase::IgnoreCase))      { OutType = EDriveConditionType::TextContains; return true; }
    if (Token.Equals(TEXT("count"), ESearchCase::IgnoreCase))              { OutType = EDriveConditionType::Count; return true; }
    if (Token.Equals(TEXT("geometry_in_bounds"), ESearchCase::IgnoreCase)) { OutType = EDriveConditionType::GeometryInBounds; return true; }
    if (Token.Equals(TEXT("journal_event"), ESearchCase::IgnoreCase))      { OutType = EDriveConditionType::JournalEvent; return true; }
    if (Token.Equals(TEXT("journal_severity"), ESearchCase::IgnoreCase))   { OutType = EDriveConditionType::JournalSeverity; return true; }
    return false;
}

FString FDriveJson::CompareOpToString(EDriveCompareOp Op)
{
    switch (Op)
    {
    case EDriveCompareOp::NotEqual:       return TEXT("ne");
    case EDriveCompareOp::Less:           return TEXT("lt");
    case EDriveCompareOp::LessOrEqual:    return TEXT("lte");
    case EDriveCompareOp::Greater:        return TEXT("gt");
    case EDriveCompareOp::GreaterOrEqual: return TEXT("gte");
    case EDriveCompareOp::Equal:
    default:                              return TEXT("eq");
    }
}

bool FDriveJson::CompareOpFromString(const FString& Token, EDriveCompareOp& OutOp)
{
    if (Token.Equals(TEXT("eq"), ESearchCase::IgnoreCase))  { OutOp = EDriveCompareOp::Equal; return true; }
    if (Token.Equals(TEXT("ne"), ESearchCase::IgnoreCase))  { OutOp = EDriveCompareOp::NotEqual; return true; }
    if (Token.Equals(TEXT("lt"), ESearchCase::IgnoreCase))  { OutOp = EDriveCompareOp::Less; return true; }
    if (Token.Equals(TEXT("lte"), ESearchCase::IgnoreCase)) { OutOp = EDriveCompareOp::LessOrEqual; return true; }
    if (Token.Equals(TEXT("gt"), ESearchCase::IgnoreCase))  { OutOp = EDriveCompareOp::Greater; return true; }
    if (Token.Equals(TEXT("gte"), ESearchCase::IgnoreCase)) { OutOp = EDriveCompareOp::GreaterOrEqual; return true; }
    return false;
}
