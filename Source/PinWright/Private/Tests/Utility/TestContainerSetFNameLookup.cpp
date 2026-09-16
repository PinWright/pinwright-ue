// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression test for B-container-set-fname-lookup-broken.
//
// container.set.contains and container.set.remove iterate a TSet<FName>'s elements
// comparing each against the requested value, but their element-match loops handled
// only FStrProperty / FIntProperty / FFloatProperty — there was NO FNameProperty
// branch (unlike container.set.add, which coerces the JSON string to an FName). For
// a TSet<FName> no element could ever match, so contains silently returned
// {contains:false} for genuinely-present members and remove rejected them with
// [ELEMENT_NOT_FOUND]. The fix adds the missing FNameProperty branch to both loops,
// mirroring set.add's FName coercion (linear value comparison, not a hash lookup).
//
// This test drives the real registered handlers end-to-end against a reflected
// TSet<FName> on a transient fixture UObject. If either FNameProperty branch is
// reverted, contains==true and the successful remove both fail.
#include "TestContainerSetFNameHost.h"
#include "Misc/AutomationTest.h"
#include "Tests/TestUtils.h"
#include "Dom/JsonObject.h"
#include "UObject/Package.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FContainerSetFNameLookupTest,
    "PinWright.container.set.FNameLookup",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FContainerSetFNameLookupTest::RunTest(const FString& Parameters)
{
    // Stable name so the object's path is a deterministic literal we can hand to the
    // handler's objectPath resolver (ResolveObjectForProperty -> FindObject).
    UTestContainerSetFNameHost* Host = NewObject<UTestContainerSetFNameHost>(
        GetTransientPackage(), TEXT("FContainerSetFNameLookupTest_Host"));
    TestNotNull(TEXT("Host UObject created"), Host);
    if (!Host) return false;

    // Three genuinely-present FName members + one that is deliberately absent.
    Host->FNameSet.Add(FName(TEXT("FuzzTag_Alpha")));
    Host->FNameSet.Add(FName(TEXT("FuzzTag_Beta")));
    Host->FNameSet.Add(FName(TEXT("FuzzTag_Gamma")));

    const FString ObjectPath = Host->GetPathName();

    // Sanity: the path resolves back to the same object the handlers will see.
    TestTrue(TEXT("Host path resolves to same object"),
        FindObject<UObject>(nullptr, *ObjectPath) == Host);

    auto MakePayload = [&ObjectPath](const TCHAR* Value)
    {
        TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
        P->SetStringField(TEXT("objectPath"), ObjectPath);
        P->SetStringField(TEXT("propertyName"), TEXT("FNameSet"));
        P->SetStringField(TEXT("value"), Value);
        return P;
    };

    // --- container.set.contains: present member -> true (was false pre-fix) ---
    {
        FTestResponseCapture Capture;
        const bool bFound = InvokeHandlerWithCapture(
            TEXT("container.set.contains"), MakePayload(TEXT("FuzzTag_Beta")), Capture);
        TestTrue(TEXT("container.set.contains registered"), bFound);
        TestTrue(TEXT("contains succeeded"), Capture.bSuccess);
        bool bContains = false;
        if (Capture.Result.IsValid())
        {
            Capture.Result->TryGetBoolField(TEXT("contains"), bContains);
        }
        // Counterfactual: without the FNameProperty branch this is false.
        TestTrue(TEXT("present FName member reports contains==true"), bContains);
    }

    // --- container.set.contains: absent member -> false (guards against a trivial
    //     always-true pass) ---
    {
        FTestResponseCapture Capture;
        InvokeHandlerWithCapture(
            TEXT("container.set.contains"), MakePayload(TEXT("FuzzTag_NotPresent")), Capture);
        TestTrue(TEXT("contains(absent) succeeded"), Capture.bSuccess);
        bool bContains = true;
        if (Capture.Result.IsValid())
        {
            Capture.Result->TryGetBoolField(TEXT("contains"), bContains);
        }
        TestFalse(TEXT("absent FName member reports contains==false"), bContains);
    }

    // --- container.set.remove: present member removed (was ELEMENT_NOT_FOUND pre-fix) ---
    {
        FTestResponseCapture Capture;
        const bool bFound = InvokeHandlerWithCapture(
            TEXT("container.set.remove"), MakePayload(TEXT("FuzzTag_Gamma")), Capture);
        TestTrue(TEXT("container.set.remove registered"), bFound);
        // Counterfactual: without the FNameProperty branch this is an error
        // (ErrorCode == "ELEMENT_NOT_FOUND") instead of success.
        TestTrue(TEXT("remove of present FName member succeeded"), Capture.bSuccess);
        TestEqual(TEXT("remove did not raise ELEMENT_NOT_FOUND"),
            Capture.ErrorCode, FString());
        // The element really left the set.
        TestFalse(TEXT("removed element no longer in set"),
            Host->FNameSet.Contains(FName(TEXT("FuzzTag_Gamma"))));
        TestEqual(TEXT("set now has two members"), Host->FNameSet.Num(), 2);
    }

    // --- container.set.remove: absent member -> ELEMENT_NOT_FOUND (still rejected) ---
    {
        FTestResponseCapture Capture;
        InvokeHandlerWithCapture(
            TEXT("container.set.remove"), MakePayload(TEXT("FuzzTag_NotPresent")), Capture);
        TestFalse(TEXT("remove of absent member is an error"), Capture.bSuccess);
        TestEqual(TEXT("absent remove reports ELEMENT_NOT_FOUND"),
            Capture.ErrorCode, FString(TEXT("ELEMENT_NOT_FOUND")));
    }

    return true;
}
