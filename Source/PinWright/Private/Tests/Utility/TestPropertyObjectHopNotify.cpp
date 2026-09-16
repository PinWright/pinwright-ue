// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression tests for B-property-set-object-hop-notification-noop.
//
// property.set and the 11 container.* mutators resolve a dotted path by walking it, and a
// segment that is an FObjectProperty hops onto a DIFFERENT UObject: the store lands in
// that inner object's memory. The change notification did not follow - it was dispatched
// to the object the path started from - so the inner object's PostEditChangeProperty, the
// override that exists to recompute derived state from the leaf, never ran. Measured on a
// PCG node: property.set of SettingsInterface.LowerBound answered applied:true,
// markedDirty:true, a separate property.get read the written value back, and pcg.generate
// produced an unchanged instance count.
//
// THE READ-BACK IS BLIND TO THIS. property.get walks the same path to the same inner
// container and reports memory faithfully; memory is the half that was always correct.
// Every assertion below is therefore on DOWNSTREAM state - a value the fixture recomputes
// only inside its own override - never on the stored value.
//
// Counterfactual, per test:
//   - ObjectHopNotifiesTheInnerObject: with the notification back on RootObject, every
//     DerivedFrom* stays at its -1 sentinel, the inner NotifyCount is 0 and the outer's
//     is 1. The stored values still land, which is the point.
//   - StructHopStillNotifiesTheRootObject: passes before and after. It is the guard that
//     the retarget did not move struct hops, whose RootObject-relative behaviour was
//     already measured correct (B-property-set-container-empty-change-event #7).
#include "TestPropertyObjectHopNotifyHost.h"
#include "Misc/AutomationTest.h"
#include "Tests/TestUtils.h"
#include "Dom/JsonObject.h"
#include "UObject/Package.h"

namespace
{
    // Distinct helper names: Unity merges this TU with its siblings, and
    // TestPropertyChangeEventShape.cpp already owns MakeChangeEventHost/MakeTargetPayload.
    UTestObjectHopNotifyOuter* MakeHopOuter(const TCHAR* OuterName)
    {
        UTestObjectHopNotifyOuter* Outer =
            NewObject<UTestObjectHopNotifyOuter>(GetTransientPackage(), OuterName);
        if (Outer)
        {
            Outer->Inner = NewObject<UTestObjectHopNotifyInner>(Outer);
            Outer->InnerArray.Add(NewObject<UTestObjectHopNotifyInner>(Outer));
        }
        return Outer;
    }

    TSharedPtr<FJsonObject> MakeHopTargetPayload(const FString& ObjectPath, const TCHAR* PropertyName)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("objectPath"), ObjectPath);
        Payload->SetStringField(TEXT("propertyName"), PropertyName);
        return Payload;
    }
}

// ============================================================================
// property.set through an object hop
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPropertySetObjectHopNotifiesInnerObjectTest,
    "PinWright.property.set.ObjectHopNotifiesTheInnerObject",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPropertySetObjectHopNotifiesInnerObjectTest::RunTest(const FString& Parameters)
{
    UTestObjectHopNotifyOuter* Outer =
        MakeHopOuter(TEXT("FPropertySetObjectHopNotifiesInnerObjectTest_Outer"));
    TestNotNull(TEXT("Fixture outer created"), Outer);
    if (!Outer || !Outer->Inner || Outer->InnerArray.Num() != 1) return false;

    const FString ObjectPath = Outer->GetPathName();

    // --- Leaf directly on the hopped-into object. ---
    {
        Outer->ResetChangeRecord();
        Outer->Inner->ResetChangeRecord();
        TSharedPtr<FJsonObject> Payload = MakeHopTargetPayload(ObjectPath, TEXT("Inner.Source"));
        Payload->SetNumberField(TEXT("value"), 5);

        FTestResponseCapture Capture;
        TestTrue(TEXT("property.set registered"),
            InvokeHandlerWithCapture(TEXT("property.set"), Payload, Capture));
        TestTrue(TEXT("property.set succeeded"), Capture.bSuccess);

        // The store was never the defect - asserted only so a failure below cannot be
        // mistaken for a write that did not land.
        TestEqual(TEXT("value landed in the inner object"), Outer->Inner->Source, 5);

        // THE ASSERTION THIS FILE EXISTS FOR: derived state only the inner object's own
        // override produces. -1 before the fix.
        TestEqual(TEXT("the inner object recomputed from the write"),
            Outer->Inner->DerivedFromSource, 50);

        TestEqual(TEXT("the inner object was notified exactly once"),
            Outer->Inner->NotifyCount, 1);
        // One write owes exactly one change event, and it belongs to the object that
        // holds the value - not to the object the path started from.
        TestEqual(TEXT("the outer object was not notified"), Outer->NotifyCount, 0);

        TestEqual(TEXT("event names the written leaf"),
            Outer->Inner->LastPropertyName, FName(TEXT("Source")));
        // Member is resolved against the NOTIFIED object, so for a leaf sitting directly
        // on it the member is that same leaf. Before the fix this read "Inner", a
        // property the inner class does not declare at all.
        TestEqual(TEXT("member equals the leaf for a leaf on the notified object"),
            Outer->Inner->LastMemberPropertyName, FName(TEXT("Source")));
        TestEqual(TEXT("change type is ValueSet"),
            static_cast<int32>(Outer->Inner->LastChangeType),
            static_cast<int32>(EPropertyChangeType::ValueSet));
    }

    // --- Object hop THEN struct hop: the member half must be relative to the inner
    //     object, or every member-matched branch on it is skipped. ---
    {
        Outer->ResetChangeRecord();
        Outer->Inner->ResetChangeRecord();
        TSharedPtr<FJsonObject> Payload =
            MakeHopTargetPayload(ObjectPath, TEXT("Inner.Nested.InnerScalar"));
        Payload->SetNumberField(TEXT("value"), 7);

        FTestResponseCapture Capture;
        InvokeHandlerWithCapture(TEXT("property.set"), Payload, Capture);
        TestTrue(TEXT("nested property.set succeeded"), Capture.bSuccess);

        TestEqual(TEXT("nested value landed in the inner object"),
            Outer->Inner->Nested.InnerScalar, 7);
        TestEqual(TEXT("the inner object recomputed from the nested write"),
            Outer->Inner->DerivedFromNested, 70);
        TestEqual(TEXT("the inner object was notified exactly once"),
            Outer->Inner->NotifyCount, 1);
        TestEqual(TEXT("the outer object was not notified"), Outer->NotifyCount, 0);
        TestEqual(TEXT("event names the written leaf"),
            Outer->Inner->LastPropertyName, FName(TEXT("InnerScalar")));
        TestEqual(TEXT("member names the struct on the notified object"),
            Outer->Inner->LastMemberPropertyName, FName(TEXT("Nested")));
    }

    // --- Object hop through an ARRAY ELEMENT: the same retarget, on the other of the
    //     resolver's two object-hop sites. ---
    {
        UTestObjectHopNotifyInner* Element = Outer->InnerArray[0];
        Outer->ResetChangeRecord();
        Element->ResetChangeRecord();
        TSharedPtr<FJsonObject> Payload =
            MakeHopTargetPayload(ObjectPath, TEXT("InnerArray[0].Source"));
        Payload->SetNumberField(TEXT("value"), 3);

        FTestResponseCapture Capture;
        InvokeHandlerWithCapture(TEXT("property.set"), Payload, Capture);
        TestTrue(TEXT("array-element property.set succeeded"), Capture.bSuccess);

        TestEqual(TEXT("value landed in the array element's object"), Element->Source, 3);
        TestEqual(TEXT("the array element's object recomputed from the write"),
            Element->DerivedFromSource, 30);
        TestEqual(TEXT("the array element's object was notified exactly once"),
            Element->NotifyCount, 1);
        TestEqual(TEXT("the outer object was not notified"), Outer->NotifyCount, 0);
    }

    return true;
}

// ============================================================================
// The case that must NOT change: a struct hop stays on the root object
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPropertySetStructHopNotifiesRootObjectTest,
    "PinWright.property.set.StructHopStillNotifiesTheRootObject",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPropertySetStructHopNotifiesRootObjectTest::RunTest(const FString& Parameters)
{
    UTestObjectHopNotifyOuter* Outer =
        MakeHopOuter(TEXT("FPropertySetStructHopNotifiesRootObjectTest_Outer"));
    TestNotNull(TEXT("Fixture outer created"), Outer);
    if (!Outer || !Outer->Inner) return false;

    Outer->ResetChangeRecord();
    Outer->Inner->ResetChangeRecord();

    TSharedPtr<FJsonObject> Payload =
        MakeHopTargetPayload(Outer->GetPathName(), TEXT("OuterNested.InnerScalar"));
    Payload->SetNumberField(TEXT("value"), 4);

    FTestResponseCapture Capture;
    TestTrue(TEXT("property.set registered"),
        InvokeHandlerWithCapture(TEXT("property.set"), Payload, Capture));
    TestTrue(TEXT("struct-hop property.set succeeded"), Capture.bSuccess);

    TestEqual(TEXT("struct value landed"), Outer->OuterNested.InnerScalar, 4);
    // A struct hop keeps the value inside the root object's own memory, so the root object
    // is still the object whose override has to run. Retargeting it would be the mirror
    // defect of the one this file fixes.
    TestEqual(TEXT("the root object was notified exactly once"), Outer->NotifyCount, 1);
    TestEqual(TEXT("the root object recomputed from the write"),
        Outer->DerivedFromOuterNested, 40);
    TestEqual(TEXT("event names the written leaf"),
        Outer->LastPropertyName, FName(TEXT("InnerScalar")));
    TestEqual(TEXT("member names the top-level struct on the root object"),
        Outer->LastMemberPropertyName, FName(TEXT("OuterNested")));
    TestEqual(TEXT("no unrelated object was notified"), Outer->Inner->NotifyCount, 0);

    return true;
}

// ============================================================================
// container.* shares the exposure: same helper, same retarget
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FContainerArrayObjectHopNotifiesInnerObjectTest,
    "PinWright.container.array.ObjectHopNotifiesTheInnerObject",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FContainerArrayObjectHopNotifiesInnerObjectTest::RunTest(const FString& Parameters)
{
    UTestObjectHopNotifyOuter* Outer =
        MakeHopOuter(TEXT("FContainerArrayObjectHopNotifiesInnerObjectTest_Outer"));
    TestNotNull(TEXT("Fixture outer created"), Outer);
    if (!Outer || !Outer->Inner) return false;

    const FString ObjectPath = Outer->GetPathName();

    Outer->ResetChangeRecord();
    Outer->Inner->ResetChangeRecord();

    TSharedPtr<FJsonObject> Payload =
        MakeHopTargetPayload(ObjectPath, TEXT("Inner.SourceNumbers"));
    Payload->SetNumberField(TEXT("value"), 9);

    FTestResponseCapture Capture;
    TestTrue(TEXT("container.array.append registered"),
        InvokeHandlerWithCapture(TEXT("container.array.append"), Payload, Capture));
    TestTrue(TEXT("container.array.append succeeded"), Capture.bSuccess);

    TestEqual(TEXT("element landed in the inner object's array"),
        Outer->Inner->SourceNumbers.Num(), 1);
    // The 11 container.* mutators route their notification through the same helper as
    // property.set, so they carried the same wrong target for the same paths.
    TestEqual(TEXT("the inner object recomputed from the container mutation"),
        Outer->Inner->DerivedFromNumbers, 1);
    TestEqual(TEXT("the inner object was notified exactly once"),
        Outer->Inner->NotifyCount, 1);
    TestEqual(TEXT("the outer object was not notified"), Outer->NotifyCount, 0);
    TestEqual(TEXT("event names the array property on the notified object"),
        Outer->Inner->LastPropertyName, FName(TEXT("SourceNumbers")));
    TestEqual(TEXT("change type is ArrayAdd"),
        static_cast<int32>(Outer->Inner->LastChangeType),
        static_cast<int32>(EPropertyChangeType::ArrayAdd));

    return true;
}
