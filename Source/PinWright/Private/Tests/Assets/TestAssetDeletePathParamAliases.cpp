// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression tests for asset.delete's `assetPath` / `assetPaths` aliases
// (AssetPathParamUtils::DeleteSinglePathKeys / DeleteBatchPathsKeys).
//
// The defect: asset.delete declared only `path` / `paths` with no aliases, while the
// surrounding asset.* surface is spelled `assetPath` (289 verbs declare it) and
// `assetPaths` (asset.bulk_delete, asset.bulk_rename, asset.checkout, asset.submit,
// asset.generate_lods, the sourcecontrol verbs). A caller arriving from any neighbouring
// asset verb got a hard UNKNOWN_PARAMS reject from the dispatcher before the handler ran.
// `path` / `paths` stay canonical — they are documented and shipped — so the fix is purely
// additive: aliases on the existing specs plus body-side resolution through
// AssetPathParamUtils::ResolveDeleteSinglePath / ResolveDeleteBatchPaths.
//
// The two halves are tested separately because either one alone is a shipped defect:
//  - Declaration: the registered FParamSpec carries the alias. Without it the dispatcher's
//    known-param set (RpcDispatcher.cpp ValidateHandlerParams -> AddKnownParamNames) omits
//    the spelling and rejects the payload with UNKNOWN_PARAMS.
//  - Acceptance: the alias must also REACH the body. A spec alias with no matching
//    body-side read passes the gate and then resolves to nothing, which asset.delete
//    reports as INVALID_ARGUMENT "No paths provided" — the exact defect shape catalogued
//    in Handlers/Actor/SpawnParamUtils.h. So the acceptance test asserts on the response
//    content (requestedCount + the per-entry path echo), not merely on the absence of a
//    param error.
//
// Because the gates live in the dispatcher, acceptance MUST route through a real
// FRpcDispatcher: Tests/TestUtils.h's InvokeHandler calls the registered function directly
// and never runs ValidateHandlerParams, so it cannot observe the bug at all.
//
// Nothing is deleted. The probe paths sit under a folder no host ships, so asset.delete
// classifies them existedBefore=false and never calls UEditorAssetLibrary::DeleteAsset.
// That is also what makes the assertion discriminating: an alias that was REJECTED yields
// an error response with no requestedCount, whereas an asset that was merely NOT FOUND
// yields a successful response naming the path in results[]/missing[] with deletedCount 0.
// Counterfactual: dropping either half fails this file — removing the Aliases makes the
// acceptance dispatch return UNKNOWN_PARAMS and the declaration test find an empty alias
// list; reverting the body to Payload->TryGetStringField(TEXT("path")) leaves the
// declaration test green and fails acceptance with INVALID_ARGUMENT.
#include "Misc/AutomationTest.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Dispatch/RpcDispatcher.h"
#include "Handlers/HandlerRegistration.h"
#include "Handlers/ParamSpec.h"
#include "Handlers/Asset/AssetPathParamUtils.h"
#include "Tests/Infra/DispatcherTestHelpers.h"
#include "Tests/Infra/ParamSpecTestHelpers.h"

// Uniquely named namespace: Unity merges test TUs into one translation unit, so an
// anonymous-namespace helper here would ODR-clash with the identically-shaped helpers in
// the sibling alias tests (same convention as ParamSpecTestHelpers.h).
namespace AssetDeleteAliasTestLocal
{
    // Paths under a folder no host ships — see the file header for why "never existed"
    // is the property that makes this test both safe and discriminating.
    const TCHAR* const MissingPathA =
        TEXT("/Game/__PW_GatewayTests/DoesNotExist_AssetDeleteAliasA");
    const TCHAR* const MissingPathB =
        TEXT("/Game/__PW_GatewayTests/DoesNotExist_AssetDeleteAliasB");

    // Dispatch asset.delete with ExpectedPaths carried under the single wire key Key
    // (string slot when bBatch is false, string array when true) and assert the value
    // reached the handler body.
    void CheckDeleteSlotReachesBody(FAutomationTestBase& Test, const TCHAR* Key,
        const TArray<FString>& ExpectedPaths, bool bBatch)
    {
        DispatcherTestHelpers::FSinkPtr Sink;
        FRpcDispatcher Dispatcher;
        DispatcherTestHelpers::MakeDispatcher(Sink, Dispatcher);

        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        if (bBatch)
        {
            TArray<TSharedPtr<FJsonValue>> Values;
            for (const FString& Path : ExpectedPaths)
            {
                Values.Add(MakeShared<FJsonValueString>(Path));
            }
            Params->SetArrayField(Key, Values);
        }
        else
        {
            Params->SetStringField(Key, ExpectedPaths[0]);
        }

        bool bSuccess = false;
        FString ErrorCode;
        TSharedPtr<FJsonObject> Result;
        DispatcherTestHelpers::Dispatch(Dispatcher, Sink, TEXT("asset.delete"),
            FString::Printf(TEXT("req-asset-delete-alias-%s"), Key),
            Params, bSuccess, Result, ErrorCode);

        // 1. The dispatcher's param gates admitted the spelling.
        Test.TestNotEqual(*FString::Printf(
                TEXT("asset.delete does not reject '%s' as UNKNOWN_PARAMS"), Key),
            ErrorCode, FString(TEXT("UNKNOWN_PARAMS")));
        Test.TestNotEqual(*FString::Printf(
                TEXT("asset.delete does not reject '%s' as MISSING_REQUIRED_PARAM"), Key),
            ErrorCode, FString(TEXT("MISSING_REQUIRED_PARAM")));

        // 2. The body READ the slot. INVALID_ARGUMENT here is the "No paths provided"
        //    branch, i.e. an alias that cleared the gate and then resolved to nothing.
        Test.TestNotEqual(*FString::Printf(
                TEXT("asset.delete resolves '%s' body-side (not 'No paths provided')"), Key),
            ErrorCode, FString(TEXT("INVALID_ARGUMENT")));

        if (!Test.TestTrue(*FString::Printf(TEXT("asset.delete succeeds with '%s'"), Key),
                bSuccess))
        {
            return;
        }
        if (!Test.TestNotNull(*FString::Printf(TEXT("asset.delete returns a result for '%s'"),
                Key), Result.Get()))
        {
            return;
        }

        // 3. The values reached the delete loop: it counted them and echoed each one back.
        double RequestedCount = -1.0;
        Test.TestTrue(*FString::Printf(TEXT("'%s' response carries requestedCount"), Key),
            Result->TryGetNumberField(TEXT("requestedCount"), RequestedCount));
        Test.TestEqual(*FString::Printf(TEXT("'%s' contributed %d path(s)"),
                Key, ExpectedPaths.Num()),
            static_cast<int32>(RequestedCount), ExpectedPaths.Num());

        const TArray<TSharedPtr<FJsonValue>>* Entries = nullptr;
        if (Test.TestTrue(*FString::Printf(TEXT("'%s' response carries results[]"), Key),
                Result->TryGetArrayField(TEXT("results"), Entries)) && Entries)
        {
            TArray<FString> EchoedPaths;
            for (const TSharedPtr<FJsonValue>& Entry : *Entries)
            {
                const TSharedPtr<FJsonObject>* EntryObj = nullptr;
                if (Entry.IsValid() && Entry->TryGetObject(EntryObj) && EntryObj)
                {
                    FString EntryPath;
                    (*EntryObj)->TryGetStringField(TEXT("path"), EntryPath);
                    EchoedPaths.Add(EntryPath);
                }
            }
            for (const FString& Expected : ExpectedPaths)
            {
                Test.TestTrue(*FString::Printf(
                        TEXT("'%s' value '%s' reached the handler and is named in results[]"),
                        Key, *Expected),
                    EchoedPaths.Contains(Expected));
            }
        }

        // 4. Not-found is distinguished from alias-rejected: this call removed nothing,
        //    and the paths are reported missing rather than deleted.
        double DeletedCount = -1.0;
        Result->TryGetNumberField(TEXT("deletedCount"), DeletedCount);
        Test.TestEqual(*FString::Printf(
                TEXT("'%s' deleted nothing (the probe paths never existed)"), Key),
            static_cast<int32>(DeletedCount), 0);

        double MissingCount = -1.0;
        Result->TryGetNumberField(TEXT("missingCount"), MissingCount);
        Test.TestEqual(*FString::Printf(TEXT("'%s' reports every probe path as missing"), Key),
            static_cast<int32>(MissingCount), ExpectedPaths.Num());
    }
}

// ============================================================================
// 1. Declaration: asset.delete keeps `path` / `paths` canonical and declares exactly
//    `assetPath` / `assetPaths` as aliases. An exact-set assertion so a future edit that
//    drops one spelling (the original defect) or silently widens a slot fails here.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDeleteDeclaresAssetPathAliasesTest,
    "PinWright.asset.aliases.DeleteDeclaresAssetPathAliases",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAssetDeleteDeclaresAssetPathAliasesTest::RunTest(const FString& Parameters)
{
    const FParamSpec* SingleSpec =
        ParamSpecTestHelpers::FindParamSpec(TEXT("asset.delete"), TEXT("path"));
    if (TestNotNull(TEXT("asset.delete declares the canonical 'path' slot"), SingleSpec))
    {
        TestEqual(TEXT("asset.delete 'path' is a path param"),
            SingleSpec->Type, FString(TEXT("path")));
        TestFalse(TEXT("asset.delete 'path' stays optional"), SingleSpec->bRequired);
        TestTrue(TEXT("asset.delete 'path' carries the 'assetPath' alias"),
            SingleSpec->Aliases.Contains(TEXT("assetPath")));
        TestEqual(TEXT("asset.delete 'path' declares exactly one alias"),
            SingleSpec->Aliases.Num(), 1);
    }

    const FParamSpec* BatchSpec =
        ParamSpecTestHelpers::FindParamSpec(TEXT("asset.delete"), TEXT("paths"));
    if (TestNotNull(TEXT("asset.delete declares the canonical 'paths' slot"), BatchSpec))
    {
        TestEqual(TEXT("asset.delete 'paths' is an array param"),
            BatchSpec->Type, FString(TEXT("array")));
        TestFalse(TEXT("asset.delete 'paths' stays optional"), BatchSpec->bRequired);
        TestTrue(TEXT("asset.delete 'paths' carries the 'assetPaths' alias"),
            BatchSpec->Aliases.Contains(TEXT("assetPaths")));
        TestEqual(TEXT("asset.delete 'paths' declares exactly one alias"),
            BatchSpec->Aliases.Num(), 1);
    }

    // The aliases must be aliases, not a second pair of canonical params: two canonical
    // slots for one value would let a caller populate both and would document the verb as
    // taking four path parameters.
    TestNull(TEXT("asset.delete does not register 'assetPath' as its own canonical param"),
        ParamSpecTestHelpers::FindParamSpec(TEXT("asset.delete"), TEXT("assetPath")));
    TestNull(TEXT("asset.delete does not register 'assetPaths' as its own canonical param"),
        ParamSpecTestHelpers::FindParamSpec(TEXT("asset.delete"), TEXT("assetPaths")));

    // The keys the production helper offers must be exactly the declared contract, so the
    // spec and the body-side resolver can never drift apart. Order matters: the head of
    // each list is what MakeAliasParamSpec turns into FParamSpec.Name, so a reordering
    // would silently make `assetPath` canonical and demote the shipped `path`.
    TestEqual(TEXT("DeleteSinglePathKeys is {path, assetPath} in that order"),
        FString::Join(AssetPathParamUtils::DeleteSinglePathKeys(), TEXT(",")),
        FString(TEXT("path,assetPath")));
    TestEqual(TEXT("DeleteBatchPathsKeys is {paths, assetPaths} in that order"),
        FString::Join(AssetPathParamUtils::DeleteBatchPathsKeys(), TEXT(",")),
        FString(TEXT("paths,assetPaths")));
    return true;
}

// ============================================================================
// 2. Acceptance: `assetPath` / `assetPaths` are admitted by the dispatcher AND resolved
//    by the handler body. The response must name the paths that were passed.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDeleteAcceptsAssetPathAliasesOnWireTest,
    "PinWright.asset.aliases.DeleteAcceptsAssetPathAliasesOnWire",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAssetDeleteAcceptsAssetPathAliasesOnWireTest::RunTest(const FString& Parameters)
{
    AssetDeleteAliasTestLocal::CheckDeleteSlotReachesBody(*this, TEXT("assetPath"),
        {AssetDeleteAliasTestLocal::MissingPathA}, /*bBatch=*/false);

    AssetDeleteAliasTestLocal::CheckDeleteSlotReachesBody(*this, TEXT("assetPaths"),
        {AssetDeleteAliasTestLocal::MissingPathA, AssetDeleteAliasTestLocal::MissingPathB},
        /*bBatch=*/true);
    return true;
}

// ============================================================================
// 3. Control: the shipped canonical spellings still work. The change is additive, so a
//    caller already using `path` / `paths` (build scripts do — see
//    docs/wiki-src/level-building.build-scripts.md) must be unaffected.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDeleteCanonicalSpellingsStillWorkTest,
    "PinWright.asset.aliases.DeleteCanonicalSpellingsStillWork",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAssetDeleteCanonicalSpellingsStillWorkTest::RunTest(const FString& Parameters)
{
    AssetDeleteAliasTestLocal::CheckDeleteSlotReachesBody(*this, TEXT("path"),
        {AssetDeleteAliasTestLocal::MissingPathA}, /*bBatch=*/false);

    AssetDeleteAliasTestLocal::CheckDeleteSlotReachesBody(*this, TEXT("paths"),
        {AssetDeleteAliasTestLocal::MissingPathA, AssetDeleteAliasTestLocal::MissingPathB},
        /*bBatch=*/true);
    return true;
}

// ============================================================================
// 4. Negative control: an empty payload still fails with the INVALID_ARGUMENT
//    "No paths provided" branch. This pins the signal the acceptance tests use to
//    distinguish "the alias reached nothing" from "the asset was not found" — without it,
//    an acceptance assertion on the absence of INVALID_ARGUMENT proves nothing.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDeleteEmptyPayloadStillRejectedTest,
    "PinWright.asset.aliases.DeleteEmptyPayloadStillRejected",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAssetDeleteEmptyPayloadStillRejectedTest::RunTest(const FString& Parameters)
{
    DispatcherTestHelpers::FSinkPtr Sink;
    FRpcDispatcher Dispatcher;
    DispatcherTestHelpers::MakeDispatcher(Sink, Dispatcher);

    bool bSuccess = false;
    FString ErrorCode;
    TSharedPtr<FJsonObject> Result;
    DispatcherTestHelpers::Dispatch(Dispatcher, Sink, TEXT("asset.delete"),
        TEXT("req-asset-delete-empty"), MakeShared<FJsonObject>(), bSuccess, Result, ErrorCode);

    TestFalse(TEXT("asset.delete with no path slot fails"), bSuccess);
    TestEqual(TEXT("asset.delete with no path slot reports INVALID_ARGUMENT"),
        ErrorCode, FString(TEXT("INVALID_ARGUMENT")));
    return true;
}
