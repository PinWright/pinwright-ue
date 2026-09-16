// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression tests for B-property-set-container-empty-change-event.
//
// property.set, property.reset and 11 container.* verbs notified the mutated object
// with a bare RootObject->PostEditChange(). UObject::PostEditChange builds an EMPTY
// FPropertyChangedEvent (Obj.cpp:549-553, `FPropertyChangedEvent
// EmptyPropertyUpdateStruct(NULL)`) and forwards it, so Property and MemberProperty
// were both null, GetPropertyName() returned NAME_None, and every engine override
// written as `if (PropertyName == GET_MEMBER_NAME_CHECKED(Class, Field))` matched
// NOTHING for any assignment. The write landed, the read-back was correct, the verb
// reported success — and the derived state the override exists to recompute never ran.
//
// The defect is invisible from the outside: the bare call DOES reach
// PostEditChangeProperty, so an override cannot be used to detect it by running at
// all. Only the SHAPE of the event it receives distinguishes the two. These tests
// therefore record Property / MemberProperty / ChangeType on a fixture object and
// assert them.
//
// Counterfactual for every assertion below: with the bare PostEditChange() restored,
// each recorded property name is NAME_None and each recorded change type is
// Unspecified.
#include "TestPropertyChangeEventHost.h"
#include "Misc/AutomationTest.h"
#include "Tests/TestUtils.h"
#include "Dom/JsonObject.h"
#include "UObject/Package.h"
#include "Compat/EngineVersionCompat.h"

namespace
{
    // Spawns the fixture under a stable name so its path is a deterministic literal the
    // handlers' objectPath resolver (FindObject) can resolve.
    UTestPropertyChangeEventHost* MakeChangeEventHost(const TCHAR* HostName)
    {
        return NewObject<UTestPropertyChangeEventHost>(GetTransientPackage(), HostName);
    }

    TSharedPtr<FJsonObject> MakeTargetPayload(const FString& ObjectPath, const TCHAR* PropertyName)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("objectPath"), ObjectPath);
        Payload->SetStringField(TEXT("propertyName"), PropertyName);
        return Payload;
    }
}

// ============================================================================
// property.set
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPropertySetChangeEventNamesLeafAndMemberTest,
    "PinWright.property.set.ChangeEventNamesLeafAndMember",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPropertySetChangeEventNamesLeafAndMemberTest::RunTest(const FString& Parameters)
{
    UTestPropertyChangeEventHost* Host =
        MakeChangeEventHost(TEXT("FPropertySetChangeEventNamesLeafAndMemberTest_Host"));
    TestNotNull(TEXT("Fixture host created"), Host);
    if (!Host) return false;

    const FString ObjectPath = Host->GetPathName();

    // --- Top-level write: the leaf IS the member. ---
    {
        Host->ResetChangeRecord();
        TSharedPtr<FJsonObject> Payload = MakeTargetPayload(ObjectPath, TEXT("Scalar"));
        Payload->SetNumberField(TEXT("value"), 42);

        FTestResponseCapture Capture;
        TestTrue(TEXT("property.set registered"),
            InvokeHandlerWithCapture(TEXT("property.set"), Payload, Capture));
        TestTrue(TEXT("property.set succeeded"), Capture.bSuccess);

        TestEqual(TEXT("value actually landed"), Host->Scalar, 42);
        TestEqual(TEXT("object was notified exactly once"), Host->NotifyCount, 1);
        // Counterfactual: NAME_None with the bare PostEditChange().
        TestEqual(TEXT("event names the written property"),
            Host->LastPropertyName, FName(TEXT("Scalar")));
        TestEqual(TEXT("member defaults to the same property for a top-level write"),
            Host->LastMemberPropertyName, FName(TEXT("Scalar")));
        // Counterfactual: Unspecified with the bare PostEditChange().
        TestEqual(TEXT("change type is ValueSet"),
            static_cast<int32>(Host->LastChangeType),
            static_cast<int32>(EPropertyChangeType::ValueSet));
    }

    // --- Nested write: leaf and member must DIFFER, or every member-matched engine
    //     branch (GET_MEMBER_NAME_CHECKED on the containing struct) is skipped. ---
    {
        Host->ResetChangeRecord();
        TSharedPtr<FJsonObject> Payload = MakeTargetPayload(ObjectPath, TEXT("Nested.InnerScalar"));
        Payload->SetNumberField(TEXT("value"), 7);

        FTestResponseCapture Capture;
        InvokeHandlerWithCapture(TEXT("property.set"), Payload, Capture);
        TestTrue(TEXT("nested property.set succeeded"), Capture.bSuccess);

        TestEqual(TEXT("nested value actually landed"), Host->Nested.InnerScalar, 7);
        TestEqual(TEXT("nested write notified exactly once"), Host->NotifyCount, 1);
        TestEqual(TEXT("event names the leaf that was written"),
            Host->LastPropertyName, FName(TEXT("InnerScalar")));
        // Counterfactual: NAME_None before the fix, and still the LEAF name if the fix
        // named only the leaf and never called SetActiveMemberProperty.
        TestEqual(TEXT("event names the top-level member that contains it"),
            Host->LastMemberPropertyName, FName(TEXT("Nested")));
    }

    return true;
}

// ============================================================================
// property.reset
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPropertyResetChangeEventNamesPropertyTest,
    "PinWright.property.reset.ChangeEventNamesProperty",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPropertyResetChangeEventNamesPropertyTest::RunTest(const FString& Parameters)
{
    UTestPropertyChangeEventHost* Host =
        MakeChangeEventHost(TEXT("FPropertyResetChangeEventNamesPropertyTest_Host"));
    TestNotNull(TEXT("Fixture host created"), Host);
    if (!Host) return false;

    Host->Scalar = 99;
    const FString ObjectPath = Host->GetPathName();

    Host->ResetChangeRecord();
    FTestResponseCapture Capture;
    TestTrue(TEXT("property.reset registered"),
        InvokeHandlerWithCapture(TEXT("property.reset"),
            MakeTargetPayload(ObjectPath, TEXT("Scalar")), Capture));
    TestTrue(TEXT("property.reset succeeded"), Capture.bSuccess);

    TestEqual(TEXT("value reset to the class default"), Host->Scalar, 0);
    // Two notifications would mean the override-metadata path and the value path each
    // fired one; one write owes the object exactly one change event. Holds whether or
    // not FOverridableManager exists on this host (it may have been created by a
    // sibling test), because only one of the two branches now notifies.
    TestEqual(TEXT("reset notified exactly once"), Host->NotifyCount, 1);
    // Counterfactual: NAME_None with the bare PostEditChange().
    TestEqual(TEXT("event names the reset property"),
        Host->LastPropertyName, FName(TEXT("Scalar")));
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 6, 0)
    // ResetToDefault arrived in 5.6 (UnrealType.h:6969); earlier engines fall back to
    // ValueSet, which is asserted nowhere here because the fallback is version-only.
    TestEqual(TEXT("change type is ResetToDefault"),
        static_cast<int32>(Host->LastChangeType),
        static_cast<int32>(EPropertyChangeType::ResetToDefault));
#endif

    return true;
}

// ============================================================================
// container.array.*
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FContainerArrayChangeEventNamesOperationTest,
    "PinWright.container.array.ChangeEventNamesPropertyAndOperation",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FContainerArrayChangeEventNamesOperationTest::RunTest(const FString& Parameters)
{
    UTestPropertyChangeEventHost* Host =
        MakeChangeEventHost(TEXT("FContainerArrayChangeEventNamesOperationTest_Host"));
    TestNotNull(TEXT("Fixture host created"), Host);
    if (!Host) return false;

    const FString ObjectPath = Host->GetPathName();

    // Drives one container verb and asserts the event shape it produced. Every case
    // names the ARRAY property (the element has no independent identity in a non-chain
    // event) and differs only in the change type, which is what tells an override
    // whether the container grew, shrank, emptied, or merely had one element rewritten.
    auto RunCase = [&](const TCHAR* Method, TSharedPtr<FJsonObject> Payload,
                       EPropertyChangeType::Type ExpectedChangeType, const TCHAR* Label)
    {
        Host->ResetChangeRecord();
        FTestResponseCapture Capture;
        TestTrue(FString::Printf(TEXT("%s registered"), Method),
            InvokeHandlerWithCapture(Method, Payload, Capture));
        TestTrue(FString::Printf(TEXT("%s succeeded"), Method), Capture.bSuccess);

        TestEqual(*FString::Printf(TEXT("%s notified exactly once"), Method),
            Host->NotifyCount, 1);
        // Counterfactual: NAME_None with the bare PostEditChange().
        TestEqual(*FString::Printf(TEXT("%s names the array property"), Method),
            Host->LastPropertyName, FName(TEXT("Numbers")));
        // Counterfactual: Unspecified with the bare PostEditChange(); ValueSet if the
        // fix named the property but shipped one change type for every operation.
        TestEqual(*FString::Printf(TEXT("%s reports %s"), Method, Label),
            static_cast<int32>(Host->LastChangeType),
            static_cast<int32>(ExpectedChangeType));
    };

    {
        TSharedPtr<FJsonObject> Payload = MakeTargetPayload(ObjectPath, TEXT("Numbers"));
        Payload->SetNumberField(TEXT("value"), 11);
        RunCase(TEXT("container.array.append"), Payload,
            EPropertyChangeType::ArrayAdd, TEXT("ArrayAdd"));
        TestEqual(TEXT("append grew the array"), Host->Numbers.Num(), 1);
    }

    {
        TSharedPtr<FJsonObject> Payload = MakeTargetPayload(ObjectPath, TEXT("Numbers"));
        Payload->SetNumberField(TEXT("index"), 0);
        Payload->SetNumberField(TEXT("value"), 22);
        RunCase(TEXT("container.array.insert"), Payload,
            EPropertyChangeType::ArrayAdd, TEXT("ArrayAdd"));
        TestEqual(TEXT("insert grew the array"), Host->Numbers.Num(), 2);
    }

    {
        TSharedPtr<FJsonObject> Payload = MakeTargetPayload(ObjectPath, TEXT("Numbers"));
        Payload->SetNumberField(TEXT("index"), 0);
        Payload->SetNumberField(TEXT("value"), 33);
        RunCase(TEXT("container.array.set"), Payload,
            EPropertyChangeType::ValueSet, TEXT("ValueSet"));
        TestEqual(TEXT("set rewrote the element in place"), Host->Numbers[0], 33);
        TestEqual(TEXT("set did not change the array size"), Host->Numbers.Num(), 2);
    }

    {
        TSharedPtr<FJsonObject> Payload = MakeTargetPayload(ObjectPath, TEXT("Numbers"));
        Payload->SetNumberField(TEXT("index"), 0);
        RunCase(TEXT("container.array.remove"), Payload,
            EPropertyChangeType::ArrayRemove, TEXT("ArrayRemove"));
        TestEqual(TEXT("remove shrank the array"), Host->Numbers.Num(), 1);
    }

    {
        RunCase(TEXT("container.array.clear"), MakeTargetPayload(ObjectPath, TEXT("Numbers")),
            EPropertyChangeType::ArrayClear, TEXT("ArrayClear"));
        TestEqual(TEXT("clear emptied the array"), Host->Numbers.Num(), 0);
    }

    return true;
}
