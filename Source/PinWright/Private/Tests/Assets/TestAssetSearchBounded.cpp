// Copyright (c) 2026 Alexander Penkin. MIT License.

// Behaviour tests for the find-an-asset-by-name path (asset.search) and the
// truncation contract its two siblings now share (asset.search_assets,
// asset.find_by_tag).
//
// Origin: a caller who wanted "find me a grey material" could not find
// asset.search, listed /Engine/EngineMaterials with asset.list instead, and got
// ~3200 heavy rows spilled to a file. asset.search already answered the question,
// but it truncated SILENTLY — {count:50, limit:50} was indistinguishable from
// "exactly 50 exist" and "50 of 4000" — which is the response-honesty defect of
// rpc-design.md §1 applied to a capped list.
//
// Fixtures are in-memory UDataTable probes created under /Game paths unique to
// this file and registered with FAssetRegistryModule::AssetCreated, so nothing
// here depends on host-project content. FARFilter's bIncludeOnlyOnDiskAssets
// defaults to false, which is what makes never-saved probes visible to the verb.
//
// Counterfactual: revert the totalMatches/truncated fields and
// LimitCapsAndReportsTruncation / SearchAssetsReportsTotalMatches fail on the
// missing fields; revert the projection and NamesOnlyProjectionDropsColumns fails
// because class/packagePath are still emitted; revert the FParamSpec alias and
// PatternAliasIsDeclaredAndRead fails on the declaration half (the body half
// alone would still pass, because InvokeHandlerWithCapture calls Reg.Func
// directly and never crosses the dispatcher's unknown-parameter gate — see
// rpc-design.md §3).

#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Tests/TestUtils.h"
#include "Tests/TestAssetTeardown.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Engine/DataTable.h"
#include "Handlers/HandlerRegistration.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

namespace PwAssetSearchProbes
{
    // Folder names unique to this file so a stray host asset cannot satisfy or
    // break an assertion. The "Other" folder is a SIBLING of the scoped folder,
    // not a child, which is precisely the case a naive string-prefix scope would
    // wrongly include.
    static const TCHAR* ScopedFolder = TEXT("/Game/PwSearchProbeScope");
    static const TCHAR* SiblingFolder = TEXT("/Game/PwSearchProbeScopeOther");

    inline FString ObjectPathFor(const FString& Folder, const FString& AssetName)
    {
        return FString::Printf(TEXT("%s/%s.%s"), *Folder, *AssetName, *AssetName);
    }

    // One never-saved asset in the registry. AssetCreated is the same notification
    // the production create_* handlers fire, so the probe is indexed exactly the
    // way a real freshly-made asset is.
    inline UObject* Create(const FString& Folder, const FString& AssetName)
    {
        UPackage* Pkg = CreatePackage(*(Folder / AssetName));
        if (!Pkg)
        {
            return nullptr;
        }
        UDataTable* Table = NewObject<UDataTable>(
            Pkg, FName(*AssetName), RF_Public | RF_Standalone);
        if (Table)
        {
            FAssetRegistryModule::AssetCreated(Table);
        }
        return Table;
    }

    // The three in-scope probes plus one sibling-folder probe every test shares.
    // Names deliberately mixed-case so a case-sensitive matcher fails the
    // lowercase query in CaseInsensitiveSubstringMatch.
    inline void CreateAll()
    {
        Create(ScopedFolder, TEXT("PwProbeGreyAlpha"));
        Create(ScopedFolder, TEXT("PwProbeGreyBeta"));
        Create(ScopedFolder, TEXT("PwProbeGreyGamma"));
        Create(SiblingFolder, TEXT("PwProbeGreyDelta"));
    }

    inline void DestroyAll()
    {
        PwTestAssetTeardown::DiscardCreatedAssetByObjectPath(ObjectPathFor(ScopedFolder, TEXT("PwProbeGreyAlpha")));
        PwTestAssetTeardown::DiscardCreatedAssetByObjectPath(ObjectPathFor(ScopedFolder, TEXT("PwProbeGreyBeta")));
        PwTestAssetTeardown::DiscardCreatedAssetByObjectPath(ObjectPathFor(ScopedFolder, TEXT("PwProbeGreyGamma")));
        PwTestAssetTeardown::DiscardCreatedAssetByObjectPath(ObjectPathFor(SiblingFolder, TEXT("PwProbeGreyDelta")));
    }

    // Collects the `name` field of every returned row.
    inline TArray<FString> RowNames(const TSharedPtr<FJsonObject>& Result)
    {
        TArray<FString> Names;
        const TArray<TSharedPtr<FJsonValue>>* Assets = nullptr;
        if (!Result.IsValid() || !Result->TryGetArrayField(TEXT("assets"), Assets) || !Assets)
        {
            return Names;
        }
        for (const TSharedPtr<FJsonValue>& Value : *Assets)
        {
            const TSharedPtr<FJsonObject>* Row = nullptr;
            if (Value.IsValid() && Value->TryGetObject(Row) && Row)
            {
                FString Name;
                (*Row)->TryGetStringField(TEXT("name"), Name);
                Names.Add(Name);
            }
        }
        return Names;
    }
}

// ============================================================================
// Matching is case-insensitive on the NAME. "find me a grey material" is the
// reported use, and an all-lowercase query against mixed-case asset names is the
// smallest thing that proves the matcher does not just happen to work when the
// caller types the exact capitalisation.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetSearchCaseInsensitiveTest,
    "PinWright.asset.search.CaseInsensitiveSubstringMatch",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetSearchCaseInsensitiveTest::RunTest(const FString& Parameters)
{
    PwAssetSearchProbes::CreateAll();

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("query"), TEXT("pwprobegrey"));   // all lowercase
    Payload->SetStringField(TEXT("path"), PwAssetSearchProbes::ScopedFolder);

    FTestResponseCapture Capture;
    TestTrue(TEXT("asset.search handler found"),
        InvokeHandlerWithCapture(TEXT("asset.search"), Payload, Capture));
    TestTrue(TEXT("response reports success"), Capture.bSuccess);

    const TArray<FString> Names = PwAssetSearchProbes::RowNames(Capture.Result);
    TestEqual(TEXT("a lowercase query matches the three mixed-case probes"), Names.Num(), 3);
    TestTrue(TEXT("PwProbeGreyAlpha matched"), Names.Contains(TEXT("PwProbeGreyAlpha")));
    TestTrue(TEXT("PwProbeGreyBeta matched"), Names.Contains(TEXT("PwProbeGreyBeta")));
    TestTrue(TEXT("PwProbeGreyGamma matched"), Names.Contains(TEXT("PwProbeGreyGamma")));

    PwAssetSearchProbes::DestroyAll();
    return true;
}

// ============================================================================
// The cap is honest in BOTH directions: a truncated page says so and publishes
// the full match count, and a page that happens to be exactly `limit` rows long
// reports truncated:false. The second half is the one a naive count == limit
// check at the call site gets wrong, so it is asserted explicitly.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetSearchLimitTruncationTest,
    "PinWright.asset.search.LimitCapsAndReportsTruncation",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetSearchLimitTruncationTest::RunTest(const FString& Parameters)
{
    PwAssetSearchProbes::CreateAll();

    // (a) Truncated: 3 match, 2 requested.
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("query"), TEXT("PwProbeGrey"));
        Payload->SetStringField(TEXT("path"), PwAssetSearchProbes::ScopedFolder);
        Payload->SetNumberField(TEXT("limit"), 2.0);

        FTestResponseCapture Capture;
        InvokeHandlerWithCapture(TEXT("asset.search"), Payload, Capture);
        TestTrue(TEXT("capped search succeeds"), Capture.bSuccess);

        if (Capture.Result.IsValid())
        {
            int32 Count = -1, TotalMatches = -1;
            bool bTruncated = false;
            Capture.Result->TryGetNumberField(TEXT("count"), Count);
            Capture.Result->TryGetNumberField(TEXT("totalMatches"), TotalMatches);
            Capture.Result->TryGetBoolField(TEXT("truncated"), bTruncated);

            TestEqual(TEXT("limit caps the rows returned"), Count, 2);
            // The whole point: the scan is NOT capped, so the caller learns a
            // third match exists rather than inferring it from count == limit.
            TestEqual(TEXT("totalMatches counts every match, not just the returned page"),
                TotalMatches, 3);
            TestTrue(TEXT("truncated is true when rows were withheld"), bTruncated);
        }
        else
        {
            AddError(TEXT("capped search returned no result object"));
        }
    }

    // (b) Not truncated: 3 match, 3 requested. Negative calibration for the flag.
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("query"), TEXT("PwProbeGrey"));
        Payload->SetStringField(TEXT("path"), PwAssetSearchProbes::ScopedFolder);
        Payload->SetNumberField(TEXT("limit"), 3.0);

        FTestResponseCapture Capture;
        InvokeHandlerWithCapture(TEXT("asset.search"), Payload, Capture);
        TestTrue(TEXT("exact-fit search succeeds"), Capture.bSuccess);

        if (Capture.Result.IsValid())
        {
            int32 Count = -1, TotalMatches = -1;
            bool bTruncated = true;
            Capture.Result->TryGetNumberField(TEXT("count"), Count);
            Capture.Result->TryGetNumberField(TEXT("totalMatches"), TotalMatches);
            Capture.Result->TryGetBoolField(TEXT("truncated"), bTruncated);

            TestEqual(TEXT("all three rows returned"), Count, 3);
            TestEqual(TEXT("totalMatches equals count when nothing was withheld"),
                TotalMatches, 3);
            // A count == limit page that IS the whole set must not claim truncation.
            TestFalse(TEXT("truncated is false when count happens to equal limit"), bTruncated);
        }
        else
        {
            AddError(TEXT("exact-fit search returned no result object"));
        }
    }

    PwAssetSearchProbes::DestroyAll();
    return true;
}

// ============================================================================
// `path` scopes by asset-registry path node, not by string prefix: the sibling
// folder /Game/PwSearchProbeScopeOther shares a textual prefix with the scoped
// folder and must NOT be returned. Both directions are asserted, because a scope
// that excluded everything would also pass the exclusion half alone.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetSearchPathScopeTest,
    "PinWright.asset.search.PathScopeExcludesOutOfScopeHits",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetSearchPathScopeTest::RunTest(const FString& Parameters)
{
    PwAssetSearchProbes::CreateAll();

    // Scoped: the sibling-folder probe is excluded.
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("query"), TEXT("PwProbeGrey"));
        Payload->SetStringField(TEXT("path"), PwAssetSearchProbes::ScopedFolder);
        Payload->SetNumberField(TEXT("limit"), 500.0);

        FTestResponseCapture Capture;
        InvokeHandlerWithCapture(TEXT("asset.search"), Payload, Capture);
        const TArray<FString> Names = PwAssetSearchProbes::RowNames(Capture.Result);

        TestTrue(TEXT("in-scope probe returned"), Names.Contains(TEXT("PwProbeGreyAlpha")));
        TestFalse(TEXT("prefix-sharing SIBLING folder is not treated as in scope"),
            Names.Contains(TEXT("PwProbeGreyDelta")));
    }

    // Widened: the same probe IS reachable, which proves the exclusion above was
    // the scope doing its job and not the probe being invisible to the registry.
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("query"), TEXT("PwProbeGrey"));
        Payload->SetStringField(TEXT("path"), TEXT("/Game"));
        Payload->SetNumberField(TEXT("limit"), 500.0);

        FTestResponseCapture Capture;
        InvokeHandlerWithCapture(TEXT("asset.search"), Payload, Capture);
        const TArray<FString> Names = PwAssetSearchProbes::RowNames(Capture.Result);

        TestTrue(TEXT("sibling-folder probe is reachable from a wider scope"),
            Names.Contains(TEXT("PwProbeGreyDelta")));
    }

    PwAssetSearchProbes::DestroyAll();
    return true;
}

// ============================================================================
// A query that matches nothing is a SUCCESS with an empty list, not an error.
// Discovery verbs are read-only probes; erroring on "no match" would force every
// caller to special-case the normal answer to "does this exist?".
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetSearchEmptyResultTest,
    "PinWright.asset.search.EmptyResultIsSuccessNotError",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetSearchEmptyResultTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("query"), TEXT("PwProbeNoSuchAssetNameZZZQQ"));
    Payload->SetStringField(TEXT("path"), TEXT("/Game"));

    FTestResponseCapture Capture;
    TestTrue(TEXT("asset.search handler found"),
        InvokeHandlerWithCapture(TEXT("asset.search"), Payload, Capture));
    TestTrue(TEXT("a no-match query is a success, not an error"), Capture.bSuccess);
    TestEqual(TEXT("no error code is set"), Capture.ErrorCode, FString());

    if (Capture.Result.IsValid())
    {
        const TArray<TSharedPtr<FJsonValue>>* Assets = nullptr;
        TestTrue(TEXT("assets array is present"),
            Capture.Result->TryGetArrayField(TEXT("assets"), Assets));
        if (Assets)
        {
            TestEqual(TEXT("assets array is empty"), (*Assets).Num(), 0);
        }

        int32 TotalMatches = -1;
        bool bTruncated = true;
        Capture.Result->TryGetNumberField(TEXT("totalMatches"), TotalMatches);
        Capture.Result->TryGetBoolField(TEXT("truncated"), bTruncated);
        TestEqual(TEXT("totalMatches is zero"), TotalMatches, 0);
        TestFalse(TEXT("an empty result is not truncated"), bTruncated);
    }
    else
    {
        AddError(TEXT("no-match query returned no result object"));
    }

    return true;
}

// ============================================================================
// The `pattern` alias must be declared on the FParamSpec AND read by the body.
// Only the first half is enforceable through the dispatcher's unknown-parameter
// gate, and only the second half is observable through InvokeHandlerWithCapture
// (which calls Reg.Func directly and never crosses that gate) — so an alias
// declared in one place only clears the gate and reaches nothing, or reaches the
// body and is rejected before it gets there. Both halves are asserted.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetSearchPatternAliasTest,
    "PinWright.asset.search.PatternAliasIsDeclaredAndRead",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetSearchPatternAliasTest::RunTest(const FString& Parameters)
{
    // Half 1 — the declaration the dispatcher's gate consults.
    bool bFoundRegistration = false;
    bool bAliasDeclared = false;
    for (const FHandlerRegistration& Reg : FAutoRegisterHandler::GetPendingRegistrations())
    {
        if (Reg.MethodName != TEXT("asset.search"))
        {
            continue;
        }
        bFoundRegistration = true;
        for (const FParamSpec& Spec : Reg.Params)
        {
            if (Spec.Name == TEXT("query"))
            {
                bAliasDeclared = Spec.Aliases.Contains(TEXT("pattern"));
            }
        }
    }
    TestTrue(TEXT("asset.search is registered"), bFoundRegistration);
    TestTrue(TEXT("the query param declares 'pattern' as an alias so the dispatcher gate admits it"),
        bAliasDeclared);

    // Half 2 — the body actually reads it.
    PwAssetSearchProbes::CreateAll();

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("pattern"), TEXT("PwProbeGreyAlpha"));   // no `query` at all
    Payload->SetStringField(TEXT("path"), PwAssetSearchProbes::ScopedFolder);

    FTestResponseCapture Capture;
    InvokeHandlerWithCapture(TEXT("asset.search"), Payload, Capture);
    TestTrue(TEXT("a pattern-only call succeeds instead of erroring on a missing query"),
        Capture.bSuccess);

    const TArray<FString> Names = PwAssetSearchProbes::RowNames(Capture.Result);
    TestTrue(TEXT("the alias reached the matcher"), Names.Contains(TEXT("PwProbeGreyAlpha")));

    PwAssetSearchProbes::DestroyAll();
    return true;
}

// ============================================================================
// namesOnly drops the two non-identity columns, which is the lever that keeps a
// 500-row page inside the inline budget on deep /Game paths.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetSearchNamesOnlyTest,
    "PinWright.asset.search.NamesOnlyProjectionDropsColumns",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetSearchNamesOnlyTest::RunTest(const FString& Parameters)
{
    PwAssetSearchProbes::CreateAll();

    // Unprojected first, so the assertion below proves the columns were REMOVED
    // rather than never present.
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("query"), TEXT("PwProbeGreyAlpha"));
        Payload->SetStringField(TEXT("path"), PwAssetSearchProbes::ScopedFolder);

        FTestResponseCapture Capture;
        InvokeHandlerWithCapture(TEXT("asset.search"), Payload, Capture);

        const TArray<TSharedPtr<FJsonValue>>* Assets = nullptr;
        if (Capture.Result.IsValid() && Capture.Result->TryGetArrayField(TEXT("assets"), Assets)
            && Assets && (*Assets).Num() > 0)
        {
            const TSharedPtr<FJsonObject>* Row = nullptr;
            if ((*Assets)[0]->TryGetObject(Row) && Row)
            {
                TestTrue(TEXT("unprojected row carries class"), (*Row)->HasField(TEXT("class")));
                TestTrue(TEXT("unprojected row carries packagePath"),
                    (*Row)->HasField(TEXT("packagePath")));
            }
        }
        else
        {
            AddError(TEXT("unprojected search returned no rows"));
        }
    }

    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("query"), TEXT("PwProbeGreyAlpha"));
        Payload->SetStringField(TEXT("path"), PwAssetSearchProbes::ScopedFolder);
        Payload->SetBoolField(TEXT("namesOnly"), true);

        FTestResponseCapture Capture;
        InvokeHandlerWithCapture(TEXT("asset.search"), Payload, Capture);

        const TArray<TSharedPtr<FJsonValue>>* Assets = nullptr;
        if (Capture.Result.IsValid() && Capture.Result->TryGetArrayField(TEXT("assets"), Assets)
            && Assets && (*Assets).Num() > 0)
        {
            const TSharedPtr<FJsonObject>* Row = nullptr;
            if ((*Assets)[0]->TryGetObject(Row) && Row)
            {
                TestTrue(TEXT("namesOnly keeps name"), (*Row)->HasField(TEXT("name")));
                TestTrue(TEXT("namesOnly keeps path"), (*Row)->HasField(TEXT("path")));
                TestFalse(TEXT("namesOnly drops class"), (*Row)->HasField(TEXT("class")));
                TestFalse(TEXT("namesOnly drops packagePath"),
                    (*Row)->HasField(TEXT("packagePath")));
            }
        }
        else
        {
            AddError(TEXT("namesOnly search returned no rows"));
        }
    }

    PwAssetSearchProbes::DestroyAll();
    return true;
}

// ============================================================================
// asset.search_assets shared the same silent-truncation defect: it SetNum'd the
// array to `limit` and reported only the post-cut count.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetSearchAssetsTruncationTest,
    "PinWright.asset.search_assets.ReportsTotalMatchesAndTruncated",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetSearchAssetsTruncationTest::RunTest(const FString& Parameters)
{
    PwAssetSearchProbes::CreateAll();

    TArray<TSharedPtr<FJsonValue>> ClassNames;
    // Full class path, not the short name: the short-name route depends on
    // ResolveUClass's search order, which is not what this test is about.
    ClassNames.Add(MakeShared<FJsonValueString>(TEXT("/Script/Engine.DataTable")));

    TArray<TSharedPtr<FJsonValue>> PackagePaths;
    PackagePaths.Add(MakeShared<FJsonValueString>(PwAssetSearchProbes::ScopedFolder));

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetArrayField(TEXT("classNames"), ClassNames);
    Payload->SetArrayField(TEXT("packagePaths"), PackagePaths);
    Payload->SetNumberField(TEXT("limit"), 2.0);

    FTestResponseCapture Capture;
    TestTrue(TEXT("asset.search_assets handler found"),
        InvokeHandlerWithCapture(TEXT("asset.search_assets"), Payload, Capture));
    TestTrue(TEXT("response reports success"), Capture.bSuccess);

    if (Capture.Result.IsValid())
    {
        int32 Count = -1, TotalMatches = -1;
        bool bTruncated = false;
        Capture.Result->TryGetNumberField(TEXT("count"), Count);
        Capture.Result->TryGetNumberField(TEXT("totalMatches"), TotalMatches);
        Capture.Result->TryGetBoolField(TEXT("truncated"), bTruncated);

        TestEqual(TEXT("limit caps the rows returned"), Count, 2);
        TestEqual(TEXT("totalMatches counts the pre-cap match set"), TotalMatches, 3);
        TestTrue(TEXT("truncated reports the withheld rows"), bTruncated);
    }
    else
    {
        AddError(TEXT("asset.search_assets returned no result object"));
    }

    PwAssetSearchProbes::DestroyAll();
    return true;
}

// ============================================================================
// asset.find_by_tag must LOAD every candidate to read UMetaData, so the walk is
// the expensive part. On a scan that ran to completion the counters must agree
// and totalMatches must be published; a tag nothing carries is the branch that
// proves an empty result is a success with honest counters rather than a silent
// zero.
//
// This test exercises the COMPLETE branch. `limit` does not reach the stopped
// branch at all - it counts matches, and a tag nothing carries never fills it -
// which was the defect; the stopped branch is driven by `scanLimit` in
// ScanBudgetBoundsTheWalk below.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetFindByTagScanCountersTest,
    "PinWright.asset.find_by_tag.ReportsScanCountersOnCompleteScan",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetFindByTagScanCountersTest::RunTest(const FString& Parameters)
{
    PwAssetSearchProbes::CreateAll();

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("tag"), TEXT("PwProbeNoSuchMetadataTagZZZQQ"));
    Payload->SetStringField(TEXT("path"), PwAssetSearchProbes::ScopedFolder);
    Payload->SetNumberField(TEXT("limit"), 500.0);

    FTestResponseCapture Capture;
    TestTrue(TEXT("asset.find_by_tag handler found"),
        InvokeHandlerWithCapture(TEXT("asset.find_by_tag"), Payload, Capture));
    TestTrue(TEXT("a no-match tag query is a success, not an error"), Capture.bSuccess);

    if (Capture.Result.IsValid())
    {
        int32 Count = -1, Scanned = -1, ScanCandidates = -1, TotalMatches = -1;
        bool bScanComplete = false;
        bool bTruncated = true;
        Capture.Result->TryGetNumberField(TEXT("count"), Count);
        Capture.Result->TryGetNumberField(TEXT("scanned"), Scanned);
        Capture.Result->TryGetNumberField(TEXT("scanCandidates"), ScanCandidates);
        Capture.Result->TryGetBoolField(TEXT("scanComplete"), bScanComplete);
        Capture.Result->TryGetBoolField(TEXT("truncated"), bTruncated);

        TestEqual(TEXT("no asset carries the tag"), Count, 0);
        TestTrue(TEXT("the three probes were offered as scan candidates"), ScanCandidates >= 3);
        TestEqual(TEXT("an uncapped run scans every candidate"), Scanned, ScanCandidates);
        TestTrue(TEXT("scanComplete is true when the walk finished"), bScanComplete);
        TestFalse(TEXT("a complete scan is not truncated"), bTruncated);

        // totalMatches is published ONLY on a complete scan — a total derived
        // from a capped walk would measure the cap, not the content.
        TestTrue(TEXT("a complete scan publishes totalMatches"),
            Capture.Result->TryGetNumberField(TEXT("totalMatches"), TotalMatches));
        TestEqual(TEXT("totalMatches is zero"), TotalMatches, 0);
    }
    else
    {
        AddError(TEXT("asset.find_by_tag returned no result object"));
    }

    PwAssetSearchProbes::DestroyAll();
    return true;
}

// ============================================================================
// The SCAN has its own bound, and it is the one that actually holds.
//
// The defect: the walk broke only on `AssetsArray.Num() >= Limit` - the number
// of MATCHES - while calling UEditorAssetLibrary::LoadAsset on every candidate.
// A tag with few or zero matches therefore never reached the cap and loaded
// every asset under `path`, synchronously, on the game thread; the in-code
// comment claiming "the cap therefore bounds the SCAN as well as the output" was
// false. `scanLimit` bounds the candidates EXAMINED, which is the quantity the
// cost is proportional to.
//
// The probes here are the same three no-match rows the complete-scan test uses,
// so the only difference between the two verdicts is which bound was hit. A tag
// nothing carries is deliberate: it is the case a match cap can never bound.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetFindByTagScanBudgetTest,
    "PinWright.asset.find_by_tag.ScanBudgetBoundsTheWalk",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetFindByTagScanBudgetTest::RunTest(const FString& Parameters)
{
    PwAssetSearchProbes::CreateAll();

    // (a) The bounded walk. limit is 500 and nothing matches, so the MATCH cap is
    //     unreachable by construction: anything that stops this walk is the scan
    //     budget, and anything that does not stop it scans every candidate.
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("tag"), TEXT("PwProbeNoSuchMetadataTagZZZQQ"));
        Payload->SetStringField(TEXT("path"), PwAssetSearchProbes::ScopedFolder);
        Payload->SetNumberField(TEXT("limit"), 500.0);
        Payload->SetNumberField(TEXT("scanLimit"), 1.0);

        FTestResponseCapture Capture;
        TestTrue(TEXT("asset.find_by_tag handler found"),
            InvokeHandlerWithCapture(TEXT("asset.find_by_tag"), Payload, Capture));
        TestTrue(TEXT("a scan-bounded query is still a success, not an error"), Capture.bSuccess);

        if (Capture.Result.IsValid())
        {
            int32 Scanned = -1, ScanCandidates = -1, ScanLimit = -1, TotalMatches = -1;
            bool bScanComplete = true;
            bool bTruncated = false;
            FString StopReason;
            Capture.Result->TryGetNumberField(TEXT("scanned"), Scanned);
            Capture.Result->TryGetNumberField(TEXT("scanCandidates"), ScanCandidates);
            Capture.Result->TryGetNumberField(TEXT("scanLimit"), ScanLimit);
            Capture.Result->TryGetBoolField(TEXT("scanComplete"), bScanComplete);
            Capture.Result->TryGetBoolField(TEXT("truncated"), bTruncated);
            Capture.Result->TryGetStringField(TEXT("stopReason"), StopReason);

            // Precondition: there was strictly more to scan than the budget
            // allowed, so a bounded walk and an exhaustive one are distinguishable.
            TestTrue(TEXT("more candidates were offered than the scan budget allows"),
                ScanCandidates >= 3);

            // The load-bearing assertion. Pre-fix this reads 3 (every candidate
            // loaded) because the only break was on the unreachable match cap.
            TestEqual(TEXT("scanned never exceeds the scan budget"), Scanned, 1);
            TestTrue(TEXT("the walk stopped short of the candidate list"),
                Scanned < ScanCandidates);
            TestEqual(TEXT("the scan budget is echoed"), ScanLimit, 1);

            // ...and it says so. A bounded walk that reported scanComplete:true
            // would trade one dishonesty for another.
            TestFalse(TEXT("a walk stopped by the scan budget is not complete"), bScanComplete);
            TestTrue(TEXT("a walk stopped by the scan budget reports truncated"), bTruncated);
            TestEqual(TEXT("stopReason names the scan budget, not the match cap"),
                StopReason, FString(TEXT("scanLimit")));

            // No measured total exists after a stopped walk, so the field must be
            // absent rather than filled with a number that describes the bound.
            TestFalse(TEXT("a stopped walk publishes no totalMatches"),
                Capture.Result->TryGetNumberField(TEXT("totalMatches"), TotalMatches));
        }
        else
        {
            AddError(TEXT("scan-bounded asset.find_by_tag returned no result object"));
        }
    }

    // (b) Negative calibration for the same query: with a budget larger than the
    //     candidate list the walk completes, so (a) proved the budget stopped it
    //     rather than the probes being invisible or the query being empty.
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("tag"), TEXT("PwProbeNoSuchMetadataTagZZZQQ"));
        Payload->SetStringField(TEXT("path"), PwAssetSearchProbes::ScopedFolder);
        Payload->SetNumberField(TEXT("limit"), 500.0);
        Payload->SetNumberField(TEXT("scanLimit"), 500.0);

        FTestResponseCapture Capture;
        InvokeHandlerWithCapture(TEXT("asset.find_by_tag"), Payload, Capture);
        TestTrue(TEXT("an unbounded-enough query succeeds"), Capture.bSuccess);

        if (Capture.Result.IsValid())
        {
            int32 Scanned = -1, ScanCandidates = -2, TotalMatches = -1;
            bool bScanComplete = false;
            bool bTruncated = true;
            FString StopReason;
            Capture.Result->TryGetNumberField(TEXT("scanned"), Scanned);
            Capture.Result->TryGetNumberField(TEXT("scanCandidates"), ScanCandidates);
            Capture.Result->TryGetBoolField(TEXT("scanComplete"), bScanComplete);
            Capture.Result->TryGetBoolField(TEXT("truncated"), bTruncated);
            Capture.Result->TryGetStringField(TEXT("stopReason"), StopReason);

            TestEqual(TEXT("a budget above the candidate count scans every candidate"),
                Scanned, ScanCandidates);
            TestTrue(TEXT("scanComplete is true when the walk finished"), bScanComplete);
            TestFalse(TEXT("a complete scan is not truncated"), bTruncated);
            TestEqual(TEXT("stopReason is complete"), StopReason, FString(TEXT("complete")));
            TestTrue(TEXT("a complete scan publishes totalMatches"),
                Capture.Result->TryGetNumberField(TEXT("totalMatches"), TotalMatches));
        }
        else
        {
            AddError(TEXT("unbounded asset.find_by_tag returned no result object"));
        }
    }

    // (c) The default must itself be a bound. Omitting scanLimit has to produce a
    //     finite echoed budget - a caller who passes nothing still gets a walk
    //     that cannot run away over /Game.
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("tag"), TEXT("PwProbeNoSuchMetadataTagZZZQQ"));
        Payload->SetStringField(TEXT("path"), PwAssetSearchProbes::ScopedFolder);

        FTestResponseCapture Capture;
        InvokeHandlerWithCapture(TEXT("asset.find_by_tag"), Payload, Capture);
        TestTrue(TEXT("a default-budget query succeeds"), Capture.bSuccess);

        int32 ScanLimit = -1;
        const bool bHasScanLimit = Capture.Result.IsValid()
            && Capture.Result->TryGetNumberField(TEXT("scanLimit"), ScanLimit);
        TestTrue(TEXT("the response always states the scan budget in force"), bHasScanLimit);
        TestEqual(TEXT("the default scan budget is 500"), ScanLimit, 500);
    }

    PwAssetSearchProbes::DestroyAll();
    return true;
}
