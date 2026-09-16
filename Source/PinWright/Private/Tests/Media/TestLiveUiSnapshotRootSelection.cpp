// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression tests for multi-root live UMG disambiguation
// (F-widget-describe-live-root-disambiguation).
//
// These exercise the production root-selection logic FLiveUiSnapshotService::SelectRootCandidate
// directly with synthetic candidate-name lists, so they run deterministically in headless CI
// without a live PIE viewport. Before the fix, FLiveUiSnapshotService::Capture unconditionally
// errored AMBIGUOUS_LIVE_ROOT whenever more than one UMG root was present and consulted no
// selector; these tests fail if that behavior is restored (the selector would be ignored).

#include "Misc/AutomationTest.h"

#include "Handlers/UI/LiveUiSnapshot.h"

namespace
{
    // Mirrors the ticket repro: a level's own HUD plus a ui.create_hud instance coexist,
    // each backing-widget name as DescribeCandidate would resolve it.
    TArray<FString> TwoRootCandidateNames()
    {
        return {TEXT("WBP_PlayerHUD_C_0"), TEXT("WBP_PlayerHUD_StateTree_C_0")};
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLiveUiSnapshotRootSelectionSingleRootTest,
    "PinWright.widget.LiveSnapshot.RootSelection.SingleRoot",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FLiveUiSnapshotRootSelectionSingleRootTest::RunTest(const FString& Parameters)
{
    // One root, no selector: unambiguous, picks index 0.
    FLiveUiSnapshotRequest Request;
    const TArray<FString> Candidates = {TEXT("WBP_PlayerHUD_C_0")};

    int32 Selected = INDEX_NONE;
    FString ErrorCode;
    FString ErrorMessage;
    const bool bOk = FLiveUiSnapshotService::SelectRootCandidate(
        Candidates, Request, Selected, ErrorCode, ErrorMessage);

    TestTrue(TEXT("single root resolves without a selector"), bOk);
    TestEqual(TEXT("single root selects index 0"), Selected, 0);
    TestTrue(TEXT("no error on single root"), ErrorCode.IsEmpty());
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLiveUiSnapshotRootSelectionMultiRootNoSelectorIsAmbiguousTest,
    "PinWright.widget.LiveSnapshot.RootSelection.MultiRootNoSelectorAmbiguous",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FLiveUiSnapshotRootSelectionMultiRootNoSelectorIsAmbiguousTest::RunTest(const FString& Parameters)
{
    // Two roots, no selector: still AMBIGUOUS_LIVE_ROOT (the pre-fix behavior is preserved
    // for the no-selector case), but the message now names the parameters to pass.
    FLiveUiSnapshotRequest Request;
    const TArray<FString> Candidates = TwoRootCandidateNames();

    int32 Selected = INDEX_NONE;
    FString ErrorCode;
    FString ErrorMessage;
    const bool bOk = FLiveUiSnapshotService::SelectRootCandidate(
        Candidates, Request, Selected, ErrorCode, ErrorMessage);

    TestFalse(TEXT("two roots with no selector is rejected"), bOk);
    TestEqual(TEXT("error code is AMBIGUOUS_LIVE_ROOT"), ErrorCode, TEXT("AMBIGUOUS_LIVE_ROOT"));
    TestTrue(TEXT("message names instance_name selector"), ErrorMessage.Contains(TEXT("instance_name")));
    TestTrue(TEXT("message names root_index selector"), ErrorMessage.Contains(TEXT("root_index")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLiveUiSnapshotRootSelectionInstanceNameResolvesTest,
    "PinWright.widget.LiveSnapshot.RootSelection.InstanceNameResolves",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FLiveUiSnapshotRootSelectionInstanceNameResolvesTest::RunTest(const FString& Parameters)
{
    // The core fix: instance_name picks the matching root out of an ambiguous set.
    // This is exactly the ticket's repro step 3->4 that previously dead-ended.
    FLiveUiSnapshotRequest Request;
    Request.InstanceName = TEXT("StateTree");
    const TArray<FString> Candidates = TwoRootCandidateNames();

    int32 Selected = INDEX_NONE;
    FString ErrorCode;
    FString ErrorMessage;
    const bool bOk = FLiveUiSnapshotService::SelectRootCandidate(
        Candidates, Request, Selected, ErrorCode, ErrorMessage);

    TestTrue(TEXT("instance_name disambiguates the multi-root set"), bOk);
    TestEqual(TEXT("instance_name selects the StateTree HUD (index 1)"), Selected, 1);
    TestTrue(TEXT("no error when instance_name matches one root"), ErrorCode.IsEmpty());
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLiveUiSnapshotRootSelectionInstanceNameFullKeyResolvesTest,
    "PinWright.widget.LiveSnapshot.RootSelection.InstanceNameFullKey",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FLiveUiSnapshotRootSelectionInstanceNameFullKeyResolvesTest::RunTest(const FString& Parameters)
{
    // Passing the full key ui.create_hud returns (and remove_widget_from_viewport accepts)
    // must also resolve — substring match against the candidate name.
    FLiveUiSnapshotRequest Request;
    Request.InstanceName = TEXT("WBP_PlayerHUD_StateTree_C_0");
    const TArray<FString> Candidates = TwoRootCandidateNames();

    int32 Selected = INDEX_NONE;
    FString ErrorCode;
    FString ErrorMessage;
    const bool bOk = FLiveUiSnapshotService::SelectRootCandidate(
        Candidates, Request, Selected, ErrorCode, ErrorMessage);

    TestTrue(TEXT("full create_hud key disambiguates"), bOk);
    TestEqual(TEXT("full key selects index 1"), Selected, 1);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLiveUiSnapshotRootSelectionInstanceNameNoMatchTest,
    "PinWright.widget.LiveSnapshot.RootSelection.InstanceNameNoMatch",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FLiveUiSnapshotRootSelectionInstanceNameNoMatchTest::RunTest(const FString& Parameters)
{
    // A selector that matches nothing reports LIVE_ROOT_NOT_FOUND (not a silent fallback,
    // and not a dead-end AMBIGUOUS_LIVE_ROOT).
    FLiveUiSnapshotRequest Request;
    Request.InstanceName = TEXT("NotPresentHUD");
    const TArray<FString> Candidates = TwoRootCandidateNames();

    int32 Selected = INDEX_NONE;
    FString ErrorCode;
    FString ErrorMessage;
    const bool bOk = FLiveUiSnapshotService::SelectRootCandidate(
        Candidates, Request, Selected, ErrorCode, ErrorMessage);

    TestFalse(TEXT("non-matching instance_name is rejected"), bOk);
    TestEqual(TEXT("error code is LIVE_ROOT_NOT_FOUND"), ErrorCode, TEXT("LIVE_ROOT_NOT_FOUND"));
    TestTrue(TEXT("message enumerates the available candidates"),
        ErrorMessage.Contains(TEXT("WBP_PlayerHUD_StateTree_C_0")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLiveUiSnapshotRootSelectionAmbiguousInstanceNameTest,
    "PinWright.widget.LiveSnapshot.RootSelection.AmbiguousInstanceName",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FLiveUiSnapshotRootSelectionAmbiguousInstanceNameTest::RunTest(const FString& Parameters)
{
    // A too-broad instance_name that matches both roots stays AMBIGUOUS_LIVE_ROOT rather
    // than silently picking the first — the caller must narrow it.
    FLiveUiSnapshotRequest Request;
    Request.InstanceName = TEXT("WBP_PlayerHUD");
    const TArray<FString> Candidates = TwoRootCandidateNames();

    int32 Selected = INDEX_NONE;
    FString ErrorCode;
    FString ErrorMessage;
    const bool bOk = FLiveUiSnapshotService::SelectRootCandidate(
        Candidates, Request, Selected, ErrorCode, ErrorMessage);

    TestFalse(TEXT("ambiguous instance_name is rejected"), bOk);
    TestEqual(TEXT("error code is AMBIGUOUS_LIVE_ROOT"), ErrorCode, TEXT("AMBIGUOUS_LIVE_ROOT"));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLiveUiSnapshotRootSelectionRootIndexResolvesTest,
    "PinWright.widget.LiveSnapshot.RootSelection.RootIndexResolves",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FLiveUiSnapshotRootSelectionRootIndexResolvesTest::RunTest(const FString& Parameters)
{
    // The positional selector picks the Nth root, including roots whose backing name
    // could not be resolved (empty string) and are therefore not addressable by name.
    FLiveUiSnapshotRequest Request;
    Request.RootIndex = 0;
    const TArray<FString> Candidates = TwoRootCandidateNames();

    int32 Selected = INDEX_NONE;
    FString ErrorCode;
    FString ErrorMessage;
    const bool bOk = FLiveUiSnapshotService::SelectRootCandidate(
        Candidates, Request, Selected, ErrorCode, ErrorMessage);

    TestTrue(TEXT("root_index 0 resolves"), bOk);
    TestEqual(TEXT("root_index 0 selects index 0"), Selected, 0);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLiveUiSnapshotRootSelectionRootIndexOutOfRangeTest,
    "PinWright.widget.LiveSnapshot.RootSelection.RootIndexOutOfRange",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FLiveUiSnapshotRootSelectionRootIndexOutOfRangeTest::RunTest(const FString& Parameters)
{
    FLiveUiSnapshotRequest Request;
    Request.RootIndex = 5;
    const TArray<FString> Candidates = TwoRootCandidateNames();

    int32 Selected = INDEX_NONE;
    FString ErrorCode;
    FString ErrorMessage;
    const bool bOk = FLiveUiSnapshotService::SelectRootCandidate(
        Candidates, Request, Selected, ErrorCode, ErrorMessage);

    TestFalse(TEXT("out-of-range root_index is rejected"), bOk);
    TestEqual(TEXT("error code is LIVE_ROOT_NOT_FOUND"), ErrorCode, TEXT("LIVE_ROOT_NOT_FOUND"));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLiveUiSnapshotRootSelectionEmptyCandidatesTest,
    "PinWright.widget.LiveSnapshot.RootSelection.EmptyCandidates",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FLiveUiSnapshotRootSelectionEmptyCandidatesTest::RunTest(const FString& Parameters)
{
    FLiveUiSnapshotRequest Request;
    const TArray<FString> Candidates;

    int32 Selected = INDEX_NONE;
    FString ErrorCode;
    FString ErrorMessage;
    const bool bOk = FLiveUiSnapshotService::SelectRootCandidate(
        Candidates, Request, Selected, ErrorCode, ErrorMessage);

    TestFalse(TEXT("no candidates is rejected"), bOk);
    TestEqual(TEXT("error code is LIVE_UI_NOT_FOUND"), ErrorCode, TEXT("LIVE_UI_NOT_FOUND"));
    return true;
}
