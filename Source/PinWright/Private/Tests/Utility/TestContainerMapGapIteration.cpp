// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression test for B-container-map-remove-no-rehash.
//
// The container.map.* and container.set.* read/scan handlers walk a container's raw
// internal index space with `for (int32 i = 0; i < Helper.Num(); ++i)` +
// `if (!Helper.IsValidIndex(i)) continue;`. That bound is wrong: a script map/set has
// GAPS in its internal index space after a RemoveAt (the non-compact sparse set frees
// the slot in place, leaving an invalid hole while survivors keep their internal
// indices). The canonical bound for a gapped scan is Helper.GetMaxIndex() (>= Num()),
// NOT Helper.Num() (the live element count). After removing a NON-LAST key, the
// highest-indexed survivor lives at internal index == Num(), so a Num()-bounded loop
// stops one index short and silently drops that survivor — get_keys / has_key / get
// (map) and contains (set) report it as gone even though it is still in the container
// (matching the live repro: remove "Gold", "Crystal" vanishes).
//
// This test drives the real registered handlers end-to-end against a reflected
// TMap<FString,int32> and TSet<FName> on a transient fixture UObject, building enough
// entries that the removed key is non-last (so a survivor sits at internal index ==
// Num()), then asserts the survivor is still visible. With the scan bound reverted to
// Helper.Num() the survivor is skipped and these assertions fail.
#include "TestContainerMapGapHost.h"
#include "Misc/AutomationTest.h"
#include "Tests/TestUtils.h"
#include "Dom/JsonObject.h"
#include "UObject/Package.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FContainerMapGapIterationTest,
    "PinWright.container.map.GapIteration",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FContainerMapGapIterationTest::RunTest(const FString& Parameters)
{
    UTestContainerMapGapHost* Host = NewObject<UTestContainerMapGapHost>(
        GetTransientPackage(), TEXT("FContainerMapGapIterationTest_Host"));
    TestNotNull(TEXT("Host UObject created"), Host);
    if (!Host) return false;

    const FString ObjectPath = Host->GetPathName();
    TestTrue(TEXT("Host path resolves to same object"),
        FindObject<UObject>(nullptr, *ObjectPath) == Host);

    // The five keys, inserted in this order. Internal indices 0..4 follow insertion
    // order in an initially-empty sparse map. "Removed" is index 3 (non-last);
    // "Survivor" is the last-inserted, index 4 — the entry a Num()-bounded scan drops
    // once index 3 becomes a gap (Num() falls to 4, so `i < 4` never reaches index 4).
    const TCHAR* const Keys[] = { TEXT("Wood"), TEXT("Stone"), TEXT("Iron"), TEXT("Gold"), TEXT("Crystal") };
    const int32 NumKeys = UE_ARRAY_COUNT(Keys);
    const TCHAR* const RemovedKey = TEXT("Gold");     // internal index 3
    const TCHAR* const SurvivorKey = TEXT("Crystal"); // internal index 4 — the canary

    // ---------------------------------------------------------------------------
    // Map path: set x5 -> remove(non-last) -> get_keys / has_key / get see survivor
    // ---------------------------------------------------------------------------
    auto MapSetPayload = [&ObjectPath](const TCHAR* Key, int32 Value)
    {
        TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
        P->SetStringField(TEXT("objectPath"), ObjectPath);
        P->SetStringField(TEXT("propertyName"), TEXT("StrIntMap"));
        P->SetStringField(TEXT("key"), Key);
        P->SetNumberField(TEXT("value"), Value);
        return P;
    };
    auto MapKeyPayload = [&ObjectPath](const TCHAR* Key)
    {
        TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
        P->SetStringField(TEXT("objectPath"), ObjectPath);
        P->SetStringField(TEXT("propertyName"), TEXT("StrIntMap"));
        P->SetStringField(TEXT("key"), Key);
        return P;
    };
    auto MapNoKeyPayload = [&ObjectPath]()
    {
        TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
        P->SetStringField(TEXT("objectPath"), ObjectPath);
        P->SetStringField(TEXT("propertyName"), TEXT("StrIntMap"));
        return P;
    };

    for (int32 i = 0; i < NumKeys; ++i)
    {
        FTestResponseCapture Capture;
        const bool bFound = InvokeHandlerWithCapture(
            TEXT("container.map.set"), MapSetPayload(Keys[i], (i + 1) * 10), Capture);
        TestTrue(TEXT("container.map.set registered"), bFound);
        TestTrue(*FString::Printf(TEXT("map.set(%s) succeeded"), Keys[i]), Capture.bSuccess);
    }
    TestEqual(TEXT("map built all five entries"), Host->StrIntMap.Num(), NumKeys);

    // Remove the non-last key. mapSize/Num() must drop to 4 — this is reported correctly
    // even by the buggy code (Num() is the count); the bug is in the SCAN that follows.
    {
        FTestResponseCapture Capture;
        const bool bFound = InvokeHandlerWithCapture(
            TEXT("container.map.remove"), MapKeyPayload(RemovedKey), Capture);
        TestTrue(TEXT("container.map.remove registered"), bFound);
        TestTrue(TEXT("map.remove(non-last key) succeeded"), Capture.bSuccess);
        double MapSize = -1.0;
        if (Capture.Result.IsValid())
        {
            Capture.Result->TryGetNumberField(TEXT("mapSize"), MapSize);
        }
        TestEqual(TEXT("map.remove reports mapSize 4"), (int32)MapSize, NumKeys - 1);
    }
    TestEqual(TEXT("underlying map really has four entries"), Host->StrIntMap.Num(), NumKeys - 1);

    // get_keys must return exactly the 4 survivors INCLUDING the high-index survivor.
    // Pre-fix (bound == Num()) this returns only 3 keys and omits SurvivorKey.
    {
        FTestResponseCapture Capture;
        InvokeHandlerWithCapture(TEXT("container.map.get_keys"), MapNoKeyPayload(), Capture);
        TestTrue(TEXT("map.get_keys succeeded"), Capture.bSuccess);

        double KeyCount = -1.0;
        if (Capture.Result.IsValid())
        {
            Capture.Result->TryGetNumberField(TEXT("keyCount"), KeyCount);
        }
        // Counterfactual: pre-fix this is 3 (the survivor at internal index 4 is skipped).
        TestEqual(TEXT("map.get_keys returns all four surviving keys"),
            (int32)KeyCount, NumKeys - 1);

        // The high-index survivor must be among the returned keys.
        TestTrue(TEXT("map.get_keys includes the high-index survivor"),
            JsonStringArrayContains(Capture.Result, TEXT("keys"), SurvivorKey));
        // The removed key must NOT be returned (guards against a trivial all-pass).
        TestFalse(TEXT("map.get_keys excludes the removed key"),
            JsonStringArrayContains(Capture.Result, TEXT("keys"), RemovedKey));
    }

    // has_key(survivor) must be true (pre-fix: false — the scan never reaches index 4).
    {
        FTestResponseCapture Capture;
        InvokeHandlerWithCapture(TEXT("container.map.has_key"), MapKeyPayload(SurvivorKey), Capture);
        TestTrue(TEXT("map.has_key(survivor) succeeded"), Capture.bSuccess);
        bool bHasKey = false;
        if (Capture.Result.IsValid())
        {
            Capture.Result->TryGetBoolField(TEXT("hasKey"), bHasKey);
        }
        TestTrue(TEXT("map.has_key(high-index survivor) reports true"), bHasKey);
    }

    // has_key(removed) must be false.
    {
        FTestResponseCapture Capture;
        InvokeHandlerWithCapture(TEXT("container.map.has_key"), MapKeyPayload(RemovedKey), Capture);
        bool bHasKey = true;
        if (Capture.Result.IsValid())
        {
            Capture.Result->TryGetBoolField(TEXT("hasKey"), bHasKey);
        }
        TestFalse(TEXT("map.has_key(removed key) reports false"), bHasKey);
    }

    // get(survivor) must succeed (pre-fix: KEY_NOT_FOUND).
    {
        FTestResponseCapture Capture;
        InvokeHandlerWithCapture(TEXT("container.map.get"), MapKeyPayload(SurvivorKey), Capture);
        TestTrue(TEXT("map.get(high-index survivor) succeeded"), Capture.bSuccess);
        TestEqual(TEXT("map.get(survivor) did not raise KEY_NOT_FOUND"),
            Capture.ErrorCode, FString());
    }

    // ---------------------------------------------------------------------------
    // Set path: add x5 -> remove(non-last) -> contains(survivor) still true
    // (container.set.contains / .remove share the identical scan-bound bug)
    // ---------------------------------------------------------------------------
    auto SetPayload = [&ObjectPath](const TCHAR* Value)
    {
        TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
        P->SetStringField(TEXT("objectPath"), ObjectPath);
        P->SetStringField(TEXT("propertyName"), TEXT("NameSet"));
        P->SetStringField(TEXT("value"), Value);
        return P;
    };

    for (int32 i = 0; i < NumKeys; ++i)
    {
        FTestResponseCapture Capture;
        const bool bFound = InvokeHandlerWithCapture(
            TEXT("container.set.add"), SetPayload(Keys[i]), Capture);
        TestTrue(TEXT("container.set.add registered"), bFound);
        TestTrue(*FString::Printf(TEXT("set.add(%s) succeeded"), Keys[i]), Capture.bSuccess);
    }
    TestEqual(TEXT("set built all five elements"), Host->NameSet.Num(), NumKeys);

    // Remove the non-last element via the production handler (uses the same scan to
    // FIND the element — the match scan must reach the element it removes, but the
    // canary is the SURVIVOR's later visibility through contains).
    {
        FTestResponseCapture Capture;
        const bool bFound = InvokeHandlerWithCapture(
            TEXT("container.set.remove"), SetPayload(RemovedKey), Capture);
        TestTrue(TEXT("container.set.remove registered"), bFound);
        TestTrue(TEXT("set.remove(non-last element) succeeded"), Capture.bSuccess);
    }
    TestEqual(TEXT("underlying set really has four elements"), Host->NameSet.Num(), NumKeys - 1);

    // contains(survivor) must be true (pre-fix: false — the scan stops before index 4).
    {
        FTestResponseCapture Capture;
        InvokeHandlerWithCapture(TEXT("container.set.contains"), SetPayload(SurvivorKey), Capture);
        TestTrue(TEXT("set.contains(survivor) succeeded"), Capture.bSuccess);
        bool bContains = false;
        if (Capture.Result.IsValid())
        {
            Capture.Result->TryGetBoolField(TEXT("contains"), bContains);
        }
        TestTrue(TEXT("set.contains(high-index survivor) reports true"), bContains);
    }

    // contains(removed) must be false.
    {
        FTestResponseCapture Capture;
        InvokeHandlerWithCapture(TEXT("container.set.contains"), SetPayload(RemovedKey), Capture);
        bool bContains = true;
        if (Capture.Result.IsValid())
        {
            Capture.Result->TryGetBoolField(TEXT("contains"), bContains);
        }
        TestFalse(TEXT("set.contains(removed element) reports false"), bContains);
    }

    return true;
}
