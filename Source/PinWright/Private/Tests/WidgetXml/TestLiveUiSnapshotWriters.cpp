// Copyright (c) 2026 Alexander Penkin. MIT License.

// Unit tests for live UI snapshot XML/JSON writers.

#include "Misc/AutomationTest.h"

#include "Handlers/UI/LiveUiSnapshot.h"
#include "Handlers/UI/LiveUiSnapshotXmlWriter.h"
#include "Handlers/UI/LiveUiSnapshotJsonWriter.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

namespace
{
    bool ExpectJsonObjectField(
        FAutomationTestBase& Test,
        const TSharedPtr<FJsonObject>& Object,
        const TCHAR* FieldName,
        const TSharedPtr<FJsonObject>*& OutField)
    {
        const bool bHasField = Object.IsValid() && Object->TryGetObjectField(FieldName, OutField);
        Test.TestTrue(FString::Printf(TEXT("json has object field %s"), FieldName), bHasField);
        return bHasField;
    }

    bool ExpectJsonArrayField(
        FAutomationTestBase& Test,
        const TSharedPtr<FJsonObject>& Object,
        const TCHAR* FieldName,
        const TArray<TSharedPtr<FJsonValue>>*& OutField)
    {
        const bool bHasField = Object.IsValid() && Object->TryGetArrayField(FieldName, OutField);
        Test.TestTrue(FString::Printf(TEXT("json has array field %s"), FieldName), bHasField);
        return bHasField;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLiveUiSnapshotXmlCompactTest,
    "PinWright.widget_xml.live_snapshot.XmlCompactOmitsDefaults",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FLiveUiSnapshotXmlCompactTest::RunTest(const FString& Parameters)
{
    FLiveUiSnapshot Snapshot;
    Snapshot.RootNode.SlateType = TEXT("SOverlay");
    Snapshot.RootNode.DebugName = TEXT("MainOverlay");
    Snapshot.RootNode.RuntimeState.bEnabled.Reset();

    const FString Xml = FLiveUiSnapshotXmlWriter::Write(Snapshot);

    TestTrue(TEXT("xml preserves Slate class tag name"), Xml.Contains(TEXT("<SOverlay")));
    TestTrue(TEXT("xml preserves debug name"), Xml.Contains(TEXT("name=\"MainOverlay\"")));
    TestFalse(TEXT("compact xml omits empty visibility"), Xml.Contains(TEXT("visibility=")));
    TestFalse(TEXT("compact xml omits unset enabled"), Xml.Contains(TEXT("enabled=")));
    TestFalse(TEXT("compact xml omits zero desired size"), Xml.Contains(TEXT("desired_w=")));
    TestFalse(TEXT("compact xml omits geometry when not included"), Xml.Contains(TEXT("<Geometry")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLiveUiSnapshotXmlGeometryAndDisabledTest,
    "PinWright.widget_xml.live_snapshot.XmlGeometryAndDisabled",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FLiveUiSnapshotXmlGeometryAndDisabledTest::RunTest(const FString& Parameters)
{
    FLiveUiSnapshot Snapshot;
    Snapshot.bGeometryIncluded = true;
    Snapshot.RootNode.SlateType = TEXT("SOverlay");
    Snapshot.RootNode.DebugName = TEXT("DisabledOverlay");
    Snapshot.RootNode.RuntimeState.bEnabled = false;
    // Four DISTINCT values. The predecessor of this test left all four geometry numbers at
    // their zero default and asserted abs_x/abs_y/abs_w/abs_h were each "0.000" — true for
    // any wiring at LiveUiSnapshotXmlWriter.cpp:289-292, including one that swaps
    // AbsolutePosition with AbsoluteSize, swaps X with Y, or writes the same number four
    // times. Distinct values make each attribute's source identifiable.
    Snapshot.RootNode.RuntimeState.AbsolutePosition = FVector2D(12.0, 34.0);
    Snapshot.RootNode.RuntimeState.AbsoluteSize = FVector2D(56.0, 78.0);

    const FString Xml = FLiveUiSnapshotXmlWriter::Write(Snapshot);

    TestTrue(TEXT("xml includes false enabled state"), Xml.Contains(TEXT("enabled=\"false\"")));
    TestTrue(TEXT("xml includes geometry block"), Xml.Contains(TEXT("<Geometry")));
    TestTrue(TEXT("abs_x is AbsolutePosition.X"), Xml.Contains(TEXT("abs_x=\"12.000\"")));
    TestTrue(TEXT("abs_y is AbsolutePosition.Y"), Xml.Contains(TEXT("abs_y=\"34.000\"")));
    TestTrue(TEXT("abs_w is AbsoluteSize.X"), Xml.Contains(TEXT("abs_w=\"56.000\"")));
    TestTrue(TEXT("abs_h is AbsoluteSize.Y"), Xml.Contains(TEXT("abs_h=\"78.000\"")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLiveUiSnapshotJsonShapeTest,
    "PinWright.widget_xml.live_snapshot.JsonShape",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FLiveUiSnapshotJsonShapeTest::RunTest(const FString& Parameters)
{
    FLiveUiSnapshot Snapshot;
    Snapshot.bVerbose = true;
    Snapshot.bGeometryIncluded = true;
    Snapshot.RootNode.SlateType = TEXT("SConstraintCanvas");
    Snapshot.RootNode.RuntimeState.bEnabled.Reset();

    TSharedPtr<FJsonObject> Json = FLiveUiSnapshotJsonWriter::Write(Snapshot);

    TestEqual(TEXT("capture_source is live"), Json->GetStringField(TEXT("capture_source")), TEXT("live"));

    bool bVerbose = false;
    TestTrue(TEXT("verbose field exists"), Json->TryGetBoolField(TEXT("verbose"), bVerbose));
    TestTrue(TEXT("verbose is true"), bVerbose);

    bool bGeometryIncluded = false;
    TestTrue(TEXT("geometry_included field exists"), Json->TryGetBoolField(TEXT("geometry_included"), bGeometryIncluded));
    TestTrue(TEXT("geometry_included is true"), bGeometryIncluded);

    const TSharedPtr<FJsonObject>* RootPtr = nullptr;
    if (!ExpectJsonObjectField(*this, Json, TEXT("root"), RootPtr) || !RootPtr || !RootPtr->IsValid())
    {
        return false;
    }

    const TSharedPtr<FJsonObject>& Root = *RootPtr;
    TestTrue(TEXT("root has slate_type"), Root->HasField(TEXT("slate_type")));
    TestTrue(TEXT("root has debug_name"), Root->HasField(TEXT("debug_name")));
    TestTrue(TEXT("root has runtime_state"), Root->HasField(TEXT("runtime_state")));
    TestTrue(TEXT("root has source"), Root->HasField(TEXT("source")));
    TestTrue(TEXT("root has slot"), Root->HasField(TEXT("slot")));
    TestTrue(TEXT("root has properties"), Root->HasField(TEXT("properties")));
    TestTrue(TEXT("root has bindings"), Root->HasField(TEXT("bindings")));
    TestTrue(TEXT("root has delegates"), Root->HasField(TEXT("delegates")));
    TestTrue(TEXT("root has geometry"), Root->HasField(TEXT("geometry")));
    TestTrue(TEXT("root has children"), Root->HasField(TEXT("children")));

    const TSharedPtr<FJsonObject>* RuntimeStatePtr = nullptr;
    if (ExpectJsonObjectField(*this, Root, TEXT("runtime_state"), RuntimeStatePtr) && RuntimeStatePtr && RuntimeStatePtr->IsValid())
    {
        TestFalse(TEXT("unset enabled is omitted from runtime_state"), (*RuntimeStatePtr)->HasField(TEXT("enabled")));
    }

    const TSharedPtr<FJsonObject>* SourcePtr = nullptr;
    if (ExpectJsonObjectField(*this, Root, TEXT("source"), SourcePtr) && SourcePtr && SourcePtr->IsValid())
    {
        TestEqual(TEXT("empty source object is present"), (*SourcePtr)->Values.Num(), 0);
    }

    const TSharedPtr<FJsonObject>* SlotPtr = nullptr;
    if (ExpectJsonObjectField(*this, Root, TEXT("slot"), SlotPtr) && SlotPtr && SlotPtr->IsValid())
    {
        TestEqual(TEXT("empty slot object is present"), (*SlotPtr)->Values.Num(), 0);
    }

    const TSharedPtr<FJsonObject>* PropertiesPtr = nullptr;
    if (ExpectJsonObjectField(*this, Root, TEXT("properties"), PropertiesPtr) && PropertiesPtr && PropertiesPtr->IsValid())
    {
        TestEqual(TEXT("empty properties object is present"), (*PropertiesPtr)->Values.Num(), 0);
    }

    const TSharedPtr<FJsonObject>* GeometryPtr = nullptr;
    if (ExpectJsonObjectField(*this, Root, TEXT("geometry"), GeometryPtr) && GeometryPtr && GeometryPtr->IsValid())
    {
        const TSharedPtr<FJsonObject>* AbsolutePtr = nullptr;
        TestTrue(TEXT("geometry has absolute block"), (*GeometryPtr)->TryGetObjectField(TEXT("absolute"), AbsolutePtr));
        if (AbsolutePtr && AbsolutePtr->IsValid())
        {
            TestEqual(TEXT("geometry absolute x defaults to zero"), (*AbsolutePtr)->GetNumberField(TEXT("x")), 0.0);
            TestEqual(TEXT("geometry absolute y defaults to zero"), (*AbsolutePtr)->GetNumberField(TEXT("y")), 0.0);
            TestEqual(TEXT("geometry absolute width defaults to zero"), (*AbsolutePtr)->GetNumberField(TEXT("w")), 0.0);
            TestEqual(TEXT("geometry absolute height defaults to zero"), (*AbsolutePtr)->GetNumberField(TEXT("h")), 0.0);
        }
    }

    const TArray<TSharedPtr<FJsonValue>>* Bindings = nullptr;
    if (ExpectJsonArrayField(*this, Root, TEXT("bindings"), Bindings) && Bindings)
    {
        TestEqual(TEXT("empty bindings array is present"), Bindings->Num(), 0);
    }

    const TArray<TSharedPtr<FJsonValue>>* Delegates = nullptr;
    if (ExpectJsonArrayField(*this, Root, TEXT("delegates"), Delegates) && Delegates)
    {
        TestEqual(TEXT("empty delegates array is present"), Delegates->Num(), 0);
    }

    const TArray<TSharedPtr<FJsonValue>>* Children = nullptr;
    if (ExpectJsonArrayField(*this, Root, TEXT("children"), Children) && Children)
    {
        TestEqual(TEXT("empty children array is present"), Children->Num(), 0);
    }

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLiveUiSnapshotJsonDisabledTest,
    "PinWright.widget_xml.live_snapshot.JsonDisabled",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FLiveUiSnapshotJsonDisabledTest::RunTest(const FString& Parameters)
{
    FLiveUiSnapshot Snapshot;
    Snapshot.RootNode.SlateType = TEXT("SButton");
    Snapshot.RootNode.RuntimeState.bEnabled = false;

    TSharedPtr<FJsonObject> Json = FLiveUiSnapshotJsonWriter::Write(Snapshot);

    const TSharedPtr<FJsonObject>* RootPtr = nullptr;
    if (!ExpectJsonObjectField(*this, Json, TEXT("root"), RootPtr) || !RootPtr || !RootPtr->IsValid())
    {
        return false;
    }

    const TSharedPtr<FJsonObject>* RuntimeStatePtr = nullptr;
    if (!ExpectJsonObjectField(*this, *RootPtr, TEXT("runtime_state"), RuntimeStatePtr) ||
        !RuntimeStatePtr ||
        !RuntimeStatePtr->IsValid())
    {
        return false;
    }

    bool bEnabled = true;
    TestTrue(TEXT("disabled runtime_state includes enabled field"), (*RuntimeStatePtr)->TryGetBoolField(TEXT("enabled"), bEnabled));
    TestFalse(TEXT("enabled is false"), bEnabled);
    return true;
}
