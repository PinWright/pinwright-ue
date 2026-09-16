// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression tests for board ticket B-asset-list-class-filter-case-divergence.
//
// asset.list's class fallback compared with FString::Equals and no ESearchCase, whose
// default is CaseSensitive; asset.search's classFilterMode pinned IgnoreCase in all three
// of its modes. Same conceptual filter - "assets whose class is exactly X" - opposite
// behaviour, neither documented. A mis-cased filter.class returned an EMPTY asset.list
// page, which reads as "no such assets exist" rather than "your filter's case was wrong",
// while the identical string worked in asset.search.
//
// Converged on case-insensitive. These are failure-direction tests: the first requires a
// lowercase class filter to return the SAME rows as the canonically-cased one on asset.list
// (restore the case-sensitive Equals and the lowercase call collapses to zero), and the
// second requires an unrecognised classFilterMode to be refused rather than silently run as
// "exact".

#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Tests/TestUtils.h"
#include "Tests/TestSkipReporting.h"

namespace AssetClassFilterCaseTestUtils
{
    // A class every editor host has assets of, named in the registry's own casing. The
    // test never hard-codes a content path: it lists a mount root and filters by class.
    const TCHAR* const CanonicalClass = TEXT("StaticMesh");
    const TCHAR* const LowercasedClass = TEXT("staticmesh");
    const TCHAR* const ScopePath = TEXT("/Engine");

    // Returns -1 on a refusal so a caller cannot score one as "filtered everything out".
    inline int32 ListWithClassFilter(FAutomationTestBase& Test, const TCHAR* ClassSpelling,
                                     TSharedPtr<FJsonObject>& OutResult)
    {
        TSharedPtr<FJsonObject> FilterObj = MakeShared<FJsonObject>();
        FilterObj->SetStringField(TEXT("class"), ClassSpelling);

        TSharedPtr<FJsonObject> Pagination = MakeShared<FJsonObject>();
        Pagination->SetNumberField(TEXT("limit"), 1);

        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("path"), ScopePath);
        Payload->SetObjectField(TEXT("filter"), FilterObj);
        Payload->SetObjectField(TEXT("pagination"), Pagination);
        // Only the identity columns; the per-row tags array is irrelevant here and large.
        Payload->SetBoolField(TEXT("namesOnly"), true);

        FTestResponseCapture Capture;
        if (!InvokeHandlerWithCapture(TEXT("asset.list"), Payload, Capture))
        {
            Test.AddError(TEXT("asset.list handler is not registered"));
            return -1;
        }
        if (!Capture.bSuccess || !Capture.Result.IsValid())
        {
            Test.AddError(FString::Printf(TEXT("asset.list class:'%s' failed: %s %s"),
                ClassSpelling, *Capture.ErrorCode, *Capture.Message));
            return -1;
        }
        OutResult = Capture.Result;
        double Total = 0.0;
        Capture.Result->TryGetNumberField(TEXT("totalCount"), Total);
        return static_cast<int32>(Total);
    }
}

// ============================================================================
// asset.list must accept the spelling asset.search accepts.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetListClassFilterCaseParityTest,
    "PinWright.asset.list.ClassFilterMatchesSearchOnCase",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAssetListClassFilterCaseParityTest::RunTest(const FString& Parameters)
{
    using namespace AssetClassFilterCaseTestUtils;

    TSharedPtr<FJsonObject> CanonicalResult;
    const int32 Canonical = ListWithClassFilter(*this, CanonicalClass, CanonicalResult);
    if (Canonical <= 0)
    {
        // Nothing of that class under this mount means the comparison has no rows to
        // separate, and a 0 == 0 assertion would pass while measuring nothing.
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-assets-of-probe-class"),
            FString::Printf(TEXT("asset.list class:'%s' under %s matched %d assets, so the "
                                 "case parity of the class filter cannot be measured here."),
                CanonicalClass, ScopePath, Canonical));
        return true;
    }

    TSharedPtr<FJsonObject> LowercaseResult;
    const int32 Lowercase = ListWithClassFilter(*this, LowercasedClass, LowercaseResult);
    TestEqual(TEXT("a lowercase class filter returns the same asset.list total as the "
                   "canonically-cased one (it used to return zero, which reads as 'no such "
                   "assets exist')"),
        Lowercase, Canonical);

    // The response has to publish the policy, or a caller cannot tell which of the two
    // historical behaviours produced the page.
    if (LowercaseResult.IsValid())
    {
        FString Mode;
        TestTrue(TEXT("asset.list echoes classFilterMode"),
            LowercaseResult->TryGetStringField(TEXT("classFilterMode"), Mode));
        TestEqual(TEXT("and it is the same 'exact' vocabulary asset.search uses"),
            Mode, FString(TEXT("exact")));
        bool bCaseSensitive = true;
        TestTrue(TEXT("asset.list echoes classFilterCaseSensitive"),
            LowercaseResult->TryGetBoolField(TEXT("classFilterCaseSensitive"), bCaseSensitive));
        TestFalse(TEXT("and reports the converged case-INSENSITIVE policy"), bCaseSensitive);
    }

    // The sibling verb, same spelling, same concept: the two must not disagree again.
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("query"), TEXT("*"));
        Payload->SetStringField(TEXT("path"), ScopePath);
        Payload->SetStringField(TEXT("classFilter"), LowercasedClass);
        Payload->SetNumberField(TEXT("limit"), 1);
        FTestResponseCapture Capture;
        TestTrue(TEXT("asset.search handler registered"),
            InvokeHandlerWithCapture(TEXT("asset.search"), Payload, Capture));
        TestTrue(TEXT("asset.search accepts the same lowercase class spelling"),
            Capture.bSuccess);
        if (Capture.bSuccess && Capture.Result.IsValid())
        {
            double Matches = 0.0;
            Capture.Result->TryGetNumberField(TEXT("totalMatches"), Matches);
            TestTrue(TEXT("and finds assets with it, as asset.list now does"),
                static_cast<int32>(Matches) > 0);
        }
    }

    return true;
}

// ============================================================================
// An unrecognised classFilterMode is a caller error, not a mode to guess at.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetSearchUnknownClassFilterModeTest,
    "PinWright.asset.search.UnknownClassFilterModeIsRefused",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAssetSearchUnknownClassFilterModeTest::RunTest(const FString& Parameters)
{
    using namespace AssetClassFilterCaseTestUtils;

    // "regex" is the shared NameMatch vocabulary a caller learns on actor.list and
    // reasonably tries here; "exakt" is the typo. Both used to run as "exact".
    const TArray<FString> Rejected = { TEXT("regex"), TEXT("exakt"), TEXT("equals") };
    for (const FString& Mode : Rejected)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("query"), TEXT("*"));
        Payload->SetStringField(TEXT("path"), ScopePath);
        Payload->SetStringField(TEXT("classFilter"), CanonicalClass);
        Payload->SetStringField(TEXT("classFilterMode"), Mode);
        Payload->SetNumberField(TEXT("limit"), 1);

        FTestResponseCapture Capture;
        TestTrue(TEXT("asset.search handler registered"),
            InvokeHandlerWithCapture(TEXT("asset.search"), Payload, Capture));
        TestFalse(*FString::Printf(TEXT("classFilterMode '%s' is not answered as a success"), *Mode),
            Capture.bSuccess);
        TestEqual(*FString::Printf(TEXT("classFilterMode '%s' is refused with INVALID_MODE"), *Mode),
            Capture.ErrorCode, FString(TEXT("INVALID_MODE")));
        TestTrue(*FString::Printf(TEXT("the refusal of '%s' enumerates the accepted modes"), *Mode),
            Capture.Message.Contains(TEXT("exact")) && Capture.Message.Contains(TEXT("prefix")) &&
                Capture.Message.Contains(TEXT("contains")));
    }

    // The canonical spellings and their aliases still work and still resolve to one mode.
    const TArray<FString> Accepted = {
        TEXT("exact"), TEXT("prefix"), TEXT("contains"),
        TEXT("substring"), TEXT("starts_with"), TEXT("Starts-With")
    };
    for (const FString& Mode : Accepted)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("query"), TEXT("*"));
        Payload->SetStringField(TEXT("path"), ScopePath);
        Payload->SetStringField(TEXT("classFilter"), CanonicalClass);
        Payload->SetStringField(TEXT("classFilterMode"), Mode);
        Payload->SetNumberField(TEXT("limit"), 1);

        FTestResponseCapture Capture;
        TestTrue(TEXT("asset.search handler registered"),
            InvokeHandlerWithCapture(TEXT("asset.search"), Payload, Capture));
        TestTrue(*FString::Printf(TEXT("classFilterMode '%s' is accepted"), *Mode),
            Capture.bSuccess);
        if (Capture.bSuccess && Capture.Result.IsValid())
        {
            FString Echoed;
            Capture.Result->TryGetStringField(TEXT("classFilterMode"), Echoed);
            TestTrue(*FString::Printf(TEXT("classFilterMode '%s' echoes a canonical spelling"), *Mode),
                Echoed == TEXT("exact") || Echoed == TEXT("prefix") || Echoed == TEXT("contains"));
        }
    }

    return true;
}
