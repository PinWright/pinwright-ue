// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Misc/AutomationTest.h"
#include "TestContainerMapGapHost.h"
#include "Utils/PropertyExport.h"
#include "Utils/SortedJsonWriter.h"
#include "UObject/Package.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPropertyExportSetOrderingTest,
    "PinWright.utils.property_export.SetOrdering",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPropertyExportSetOrderingTest::RunTest(const FString& Parameters)
{
    UTestContainerMapGapHost* Forward = NewObject<UTestContainerMapGapHost>(
        GetTransientPackage(), TEXT("PropertyExportSetOrdering_Forward"));
    UTestContainerMapGapHost* Reverse = NewObject<UTestContainerMapGapHost>(
        GetTransientPackage(), TEXT("PropertyExportSetOrdering_Reverse"));
    TestNotNull(TEXT("Forward fixture created"), Forward);
    TestNotNull(TEXT("Reverse fixture created"), Reverse);
    if (!Forward || !Reverse)
    {
        return false;
    }

    Forward->NameSet.Add(FName(TEXT("Zeta")));
    Forward->NameSet.Add(FName(TEXT("Alpha")));
    Forward->NameSet.Add(FName(TEXT("Mu")));
    Reverse->NameSet.Add(FName(TEXT("Mu")));
    Reverse->NameSet.Add(FName(TEXT("Alpha")));
    Reverse->NameSet.Add(FName(TEXT("Zeta")));

    FProperty* Property = UTestContainerMapGapHost::StaticClass()->FindPropertyByName(
        GET_MEMBER_NAME_CHECKED(UTestContainerMapGapHost, NameSet));
    TestNotNull(TEXT("NameSet property found"), Property);
    if (!Property)
    {
        return false;
    }

    const TSharedPtr<FJsonValue> ForwardJson = ExportPropertyToJsonValue(Forward, Property);
    const TSharedPtr<FJsonValue> ReverseJson = ExportPropertyToJsonValue(Reverse, Property);
    TestTrue(TEXT("Forward set exported"), ForwardJson.IsValid());
    TestTrue(TEXT("Reverse set exported"), ReverseJson.IsValid());
    if (!ForwardJson.IsValid() || !ReverseJson.IsValid())
    {
        return false;
    }

    const FString ForwardText = SortedJsonWriter::SerializeSortedJsonValue(ForwardJson);
    const FString ReverseText = SortedJsonWriter::SerializeSortedJsonValue(ReverseJson);
    TestEqual(TEXT("Different insertion orders serialize identically"), ForwardText, ReverseText);

    // SerializeSortedJsonValue writes through TPrettyJsonPrintPolicy, so a container root
    // comes back with line breaks and indentation. Whitespace is not part of the ordering
    // contract, so rebuild the compact form from each element's own canonical
    // serialization -- the exact sort key PropertyExport orders the set on.
    TArray<FString> ElementTexts;
    for (const TSharedPtr<FJsonValue>& Element : ForwardJson->AsArray())
    {
        ElementTexts.Add(SortedJsonWriter::SerializeSortedJsonValue(Element));
    }
    TestEqual(TEXT("Set elements use canonical serialized-value order"),
        FString::Printf(TEXT("[%s]"), *FString::Join(ElementTexts, TEXT(","))),
        FString(TEXT("[\"Alpha\",\"Mu\",\"Zeta\"]")));
    return true;
}
