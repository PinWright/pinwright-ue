// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression test for property.list nameMatch / propertyNames filters.
// Counterfactual: if the nameMatch and propertyNames gates in the
// TFieldIterator loop are removed, case 1 returns 6 entries instead of 3
// and case 3 returns 6 instead of 2.
#include "TestPropertyListNameFilter.h"

#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "UObject/Package.h"

#include "Tests/TestUtils.h"
#include "Tests/Utility/PropertyListTestHelpers.h"

namespace
{
    TSharedPtr<FJsonObject> MakeBasePayload(const FString& ObjectPath)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("objectPath"), ObjectPath);
        Payload->SetBoolField(TEXT("includeValues"), false);
        Payload->SetBoolField(TEXT("includeDefault"), false);
        Payload->SetBoolField(TEXT("includeOverrideState"), false);
        Payload->SetBoolField(TEXT("includeMetadata"), false);
        return Payload;
    }

    int32 GetPropertyCount(const TSharedPtr<FJsonObject>& Result)
    {
        if (!Result.IsValid())
        {
            return -1;
        }
        const TArray<TSharedPtr<FJsonValue>>* Properties = nullptr;
        if (!Result->TryGetArrayField(TEXT("properties"), Properties) || !Properties)
        {
            return -1;
        }
        return Properties->Num();
    }

    bool InvokeList(
        FAutomationTestBase& Test,
        const TSharedPtr<FJsonObject>& Payload,
        FTestResponseCapture& Capture,
        const TCHAR* Label)
    {
        Test.TestTrue(
            FString::Printf(TEXT("[%s] property.list handler found"), Label),
            InvokeHandlerWithCapture(TEXT("property.list"), Payload, Capture));
        Test.TestTrue(
            FString::Printf(TEXT("[%s] property.list succeeded"), Label),
            Capture.bSuccess);
        Test.TestTrue(
            FString::Printf(TEXT("[%s] property.list returned payload"), Label),
            Capture.Result.IsValid());
        return Capture.bSuccess && Capture.Result.IsValid();
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTestPropertyListNameFilter,
    "PinWright.Property.List.NameFilter",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FTestPropertyListNameFilter::RunTest(const FString& Parameters)
{
    UTestPropertyListNameFilterHost* Host =
        NewObject<UTestPropertyListNameFilterHost>(GetTransientPackage());
    TestNotNull(TEXT("Host UObject created"), Host);
    if (!Host)
    {
        return false;
    }
    // Keep Host alive across the handler calls; some property resolution paths can trigger GC.
    Host->AddToRoot();

    const FString ObjectPath = Host->GetPathName();

    // Case 5 (baseline): no filter -> all 6 UPROPERTYs returned.
    {
        FTestResponseCapture Capture;
        TSharedPtr<FJsonObject> Payload = MakeBasePayload(ObjectPath);
        if (!InvokeList(*this, Payload, Capture, TEXT("no-filter")))
        {
            Host->RemoveFromRoot();
            return false;
        }
        TestEqual(TEXT("no-filter returns all 6 properties"), GetPropertyCount(Capture.Result), 6);
    }

    // Case 1: nameMatch "Forced" -> 3 entries, all containing "Forced".
    {
        FTestResponseCapture Capture;
        TSharedPtr<FJsonObject> Payload = MakeBasePayload(ObjectPath);
        Payload->SetStringField(TEXT("nameMatch"), TEXT("Forced"));
        if (!InvokeList(*this, Payload, Capture, TEXT("nameMatch=Forced")))
        {
            Host->RemoveFromRoot();
            return false;
        }
        TestEqual(TEXT("nameMatch 'Forced' returns 3 properties"), GetPropertyCount(Capture.Result), 3);
        TestTrue(TEXT("ForcedAltitude present"), PropertyListTestHelpers::FindPropertyEntry(Capture.Result, TEXT("ForcedAltitude")).IsValid());
        TestTrue(TEXT("ForcedSpeed present"), PropertyListTestHelpers::FindPropertyEntry(Capture.Result, TEXT("ForcedSpeed")).IsValid());
        TestTrue(TEXT("ForcedHeading present"), PropertyListTestHelpers::FindPropertyEntry(Capture.Result, TEXT("ForcedHeading")).IsValid());
        TestFalse(TEXT("Description absent"), PropertyListTestHelpers::FindPropertyEntry(Capture.Result, TEXT("Description")).IsValid());
    }

    // Case 2: nameMatch lowercase "forced" -> still 3 entries (case-insensitive).
    {
        FTestResponseCapture Capture;
        TSharedPtr<FJsonObject> Payload = MakeBasePayload(ObjectPath);
        Payload->SetStringField(TEXT("nameMatch"), TEXT("forced"));
        if (!InvokeList(*this, Payload, Capture, TEXT("nameMatch=forced")))
        {
            Host->RemoveFromRoot();
            return false;
        }
        TestEqual(TEXT("nameMatch 'forced' (lowercase) returns 3 properties"),
            GetPropertyCount(Capture.Result), 3);
    }

    // Case 3: propertyNames allow-list -> 2 entries; ForcedSpeed absent.
    {
        FTestResponseCapture Capture;
        TSharedPtr<FJsonObject> Payload = MakeBasePayload(ObjectPath);
        TArray<TSharedPtr<FJsonValue>> Allow;
        Allow.Add(MakeShared<FJsonValueString>(TEXT("ForcedAltitude")));
        Allow.Add(MakeShared<FJsonValueString>(TEXT("bIsActive")));
        Payload->SetArrayField(TEXT("propertyNames"), Allow);
        if (!InvokeList(*this, Payload, Capture, TEXT("propertyNames")))
        {
            Host->RemoveFromRoot();
            return false;
        }
        TestEqual(TEXT("propertyNames returns 2 properties"), GetPropertyCount(Capture.Result), 2);
        TestTrue(TEXT("ForcedAltitude present"), PropertyListTestHelpers::FindPropertyEntry(Capture.Result, TEXT("ForcedAltitude")).IsValid());
        TestTrue(TEXT("bIsActive present"), PropertyListTestHelpers::FindPropertyEntry(Capture.Result, TEXT("bIsActive")).IsValid());
        TestFalse(TEXT("ForcedSpeed absent"), PropertyListTestHelpers::FindPropertyEntry(Capture.Result, TEXT("ForcedSpeed")).IsValid());
    }

    // Case 4: AND combination -> nameMatch "Forced" intersect propertyNames {ForcedAltitude} -> 1.
    {
        FTestResponseCapture Capture;
        TSharedPtr<FJsonObject> Payload = MakeBasePayload(ObjectPath);
        Payload->SetStringField(TEXT("nameMatch"), TEXT("Forced"));
        TArray<TSharedPtr<FJsonValue>> Allow;
        Allow.Add(MakeShared<FJsonValueString>(TEXT("ForcedAltitude")));
        Payload->SetArrayField(TEXT("propertyNames"), Allow);
        if (!InvokeList(*this, Payload, Capture, TEXT("AND combo")))
        {
            Host->RemoveFromRoot();
            return false;
        }
        TestEqual(TEXT("AND combo returns 1 property"), GetPropertyCount(Capture.Result), 1);
        TestTrue(TEXT("ForcedAltitude present"),
            PropertyListTestHelpers::FindPropertyEntry(Capture.Result, TEXT("ForcedAltitude")).IsValid());
    }

    Host->RemoveFromRoot();
    return true;
}
