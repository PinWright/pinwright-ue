// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression test for E-foliage-remove-silent-edge-inputs.
//
// foliage.remove used to return the same undifferentiated {success:true,
// instancesRemoved:0} for two distinct edge inputs, hiding the real outcome from
// the caller (FoliageHandler.cpp remove handler):
//   Case 1 — a nonexistent / typo'd foliageTypePath: the DoesAssetExist gate had
//            no else branch, so RemovedCount stayed 0 and no "type not found" error
//            was raised (a typo was falsely confirmed as a removal).
//   Case 2 — neither foliageTypePath nor removeAll supplied: neither branch ran, so
//            an under-specified call was a silent no-op success.
//
// The fix validates both as caller-input errors BEFORE the world/IFA lookup
// (mirroring foliage.paint / add_instances, which reject a bad foliageTypePath
// before touching the world): a nonexistent path -> ASSET_NOT_FOUND, an omit-both
// call -> INVALID_ARGUMENT. (Case 3 — removeAll winning over a co-supplied path —
// is surfaced by echoing a `mode` field on success; it needs a populated IFA to
// exercise and is documented on the foliage.remove wiki overlay.)
//
// This routes both payloads through the real production dispatcher
// (FRpcDispatcher::ProcessRequest -> the registered foliage.remove handler), the
// same entry the HTTP gateway uses — not a copy of the handler logic. The exact
// error-code equality is the discriminating assertion: on the reverted code these
// inputs yield success:true (or FOLIAGE_ACTOR_NOT_FOUND when no IFA exists),
// neither of which equals the asserted code, so the test fails if the fix is
// reverted. Because both checks run before the world/IFA lookup, the outcome does
// not depend on host content or world state; the case-1 path carries a fresh GUID
// so no residual/registry asset on a mutated fuzzing host can make DoesAssetExist
// true for it, and DoesAssetExist queries the registry without a load, so no
// asset-load error is logged (which the automation framework would score as a
// failure).
#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Misc/Guid.h"
#include "Dispatch/RpcDispatcher.h"
#include "Tests/Infra/DispatcherTestHelpers.h"

using DispatcherTestHelpers::MakeDispatcher;
using DispatcherTestHelpers::Dispatch;

// Case 1: a foliageTypePath that resolves to no asset is rejected ASSET_NOT_FOUND
// instead of being confirmed as a success:0 removal.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFoliageRemoveNonexistentPathErrorsTest,
    "PinWright.foliage.remove.NonexistentPathErrors",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FFoliageRemoveNonexistentPathErrorsTest::RunTest(const FString& Parameters)
{
    DispatcherTestHelpers::FSinkPtr Sink;
    FRpcDispatcher Dispatcher;
    MakeDispatcher(Sink, Dispatcher);

    // A guaranteed-absent foliage type path: the fresh GUID keeps DoesAssetExist
    // false regardless of host content state.
    const FString MissingType = FString::Printf(
        TEXT("/Game/Foliage/DoesNotExist_FoliageRemove_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));

    TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
    Params->SetStringField(TEXT("foliageTypePath"), MissingType);

    bool bSuccess = true;
    FString ErrorCode;
    Dispatch(Dispatcher, Sink, TEXT("foliage.remove"),
        TEXT("req-foliage-remove-nonexistent-path"), Params, bSuccess, ErrorCode);

    TestFalse(TEXT("foliage.remove on a nonexistent foliageTypePath does not report success"),
        bSuccess);
    TestEqual(TEXT("foliage.remove on a nonexistent foliageTypePath returns ASSET_NOT_FOUND"),
        ErrorCode, FString(TEXT("ASSET_NOT_FOUND")));
    return true;
}

// Case 2: omitting both foliageTypePath and removeAll is rejected INVALID_ARGUMENT
// instead of returning a silent no-op success.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFoliageRemoveMissingScopeErrorsTest,
    "PinWright.foliage.remove.MissingScopeErrors",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FFoliageRemoveMissingScopeErrorsTest::RunTest(const FString& Parameters)
{
    DispatcherTestHelpers::FSinkPtr Sink;
    FRpcDispatcher Dispatcher;
    MakeDispatcher(Sink, Dispatcher);

    // Neither foliageTypePath nor removeAll — the under-specified call.
    TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();

    bool bSuccess = true;
    FString ErrorCode;
    Dispatch(Dispatcher, Sink, TEXT("foliage.remove"),
        TEXT("req-foliage-remove-missing-scope"), Params, bSuccess, ErrorCode);

    TestFalse(TEXT("foliage.remove with neither foliageTypePath nor removeAll does not report success"),
        bSuccess);
    TestEqual(TEXT("foliage.remove with neither scope returns INVALID_ARGUMENT"),
        ErrorCode, FString(TEXT("INVALID_ARGUMENT")));
    return true;
}
