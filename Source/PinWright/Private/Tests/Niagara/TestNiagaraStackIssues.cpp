// Copyright (c) 2026 Alexander Penkin. MIT License.

// niagara.list_stack_issues / niagara.apply_issue_fix (F-niagara-issue-autofix).
//
// The behavioral fixture provokes a real engine stack issue with a real engine fix: the stock
// fixture's force modules declare a dependency on its Solve Forces and Velocity module, so
// disabling that module makes the stack report "The module has unmet dependencies." with the
// engine's "Enable module ... which provides the dependency." fix. Applying it through the verb
// must re-enable the module (read from the graph, not from the verb's response) and the issue
// must be gone from a fresh list. Counterfactual: a verb that only reports success, or re-lists
// through the view model the fix ran in, leaves the module disabled or the issue listed.

#include "Misc/AutomationTest.h"

#include "Handlers/Niagara/NiagaraCompileWait.h"
#include "Handlers/Niagara/NiagaraStackIssues.h"
#include "Tests/Assets/NiagaraEditTestUtils.h"
#include "Tests/TestSkipReporting.h"
#include "Tests/TestUtils.h"

#include "Dom/JsonObject.h"
#include "Misc/ScopeExit.h"
#include "NiagaraEmitter.h"
#include "NiagaraEmitterHandle.h"
#include "NiagaraGraph.h"
#include "NiagaraNodeFunctionCall.h"
#include "NiagaraScript.h"
#include "NiagaraScriptSource.h"
#include "NiagaraSystem.h"
#include "UObject/StrongObjectPtr.h"
#include "ViewModels/Stack/NiagaraStackGraphUtilities.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace PinWrightNiagaraStackIssuesTest
{
    // The dependency provider in the stock fixture (/Niagara/Modules/Solvers/SolveForcesAndVelocity).
    const TCHAR* const ProviderScriptName = TEXT("SolveForcesAndVelocity");

    UNiagaraNodeFunctionCall* FindModuleNode(UNiagaraSystem& System, const TCHAR* ScriptName)
    {
        for (const FNiagaraEmitterHandle& Handle : System.GetEmitterHandles())
        {
            const FVersionedNiagaraEmitterData* Data = Handle.GetEmitterData();
            const UNiagaraScriptSource* Source = Data ? Cast<UNiagaraScriptSource>(Data->GraphSource) : nullptr;
            if (!Source || !Source->NodeGraph)
            {
                continue;
            }
            TArray<UNiagaraNodeFunctionCall*> Nodes;
            Source->NodeGraph->GetNodesOfClass(Nodes);
            for (UNiagaraNodeFunctionCall* Node : Nodes)
            {
                if (Node->FunctionScript && Node->FunctionScript->GetName() == ScriptName)
                {
                    return Node;
                }
            }
        }
        return nullptr;
    }

    bool IsEnabled(const UNiagaraNodeFunctionCall& Node)
    {
        return Node.GetDesiredEnabledState() == ENodeEnabledState::Enabled;
    }

    TSharedPtr<FJsonObject> ListIssues(FAutomationTestBase& Test, const FString& SystemPath)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("assetPath"), SystemPath);
        FTestResponseCapture Capture;
        Test.TestTrue(TEXT("niagara.list_stack_issues handler found"),
            InvokeHandlerWithCapture(TEXT("niagara.list_stack_issues"), Payload, Capture));
        Test.TestTrue(FString::Printf(TEXT("list_stack_issues succeeded (errorCode='%s' message='%s')"),
            *Capture.ErrorCode, *Capture.Message), Capture.bSuccess);
        return Capture.bSuccess ? Capture.Result : nullptr;
    }

    TSet<FString> IssueIdSet(const FJsonObject& ListResult)
    {
        TSet<FString> Ids;
        const TArray<TSharedPtr<FJsonValue>>* Issues = nullptr;
        if (ListResult.TryGetArrayField(TEXT("issues"), Issues))
        {
            for (const TSharedPtr<FJsonValue>& Value : *Issues)
            {
                Ids.Add(Value->AsObject()->GetStringField(TEXT("issueId")));
            }
        }
        return Ids;
    }

    // First issue not in Baseline that sits on a module and offers exactly one applicable fix.
    bool FindNewFixableIssue(const FJsonObject& ListResult, const TSet<FString>& Baseline,
        FString& OutIssueId, FString& OutFixId)
    {
        const TArray<TSharedPtr<FJsonValue>>* Issues = nullptr;
        if (!ListResult.TryGetArrayField(TEXT("issues"), Issues))
        {
            return false;
        }
        for (const TSharedPtr<FJsonValue>& Value : *Issues)
        {
            const TSharedPtr<FJsonObject> Issue = Value->AsObject();
            const FString IssueId = Issue->GetStringField(TEXT("issueId"));
            FString Module;
            if (Baseline.Contains(IssueId) || !Issue->TryGetStringField(TEXT("module"), Module))
            {
                continue;
            }
            FString ApplicableFixId;
            int32 ApplicableCount = 0;
            for (const TSharedPtr<FJsonValue>& FixValue : Issue->GetArrayField(TEXT("fixes")))
            {
                const TSharedPtr<FJsonObject> Fix = FixValue->AsObject();
                if (Fix->GetBoolField(TEXT("applicable")))
                {
                    ApplicableFixId = Fix->GetStringField(TEXT("fixId"));
                    ++ApplicableCount;
                }
            }
            if (ApplicableCount == 1)
            {
                OutIssueId = IssueId;
                OutFixId = ApplicableFixId;
                return true;
            }
        }
        return false;
    }

    FTestResponseCapture ApplyFix(const FString& SystemPath, const FString& IssueId, const FString& FixId)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("assetPath"), SystemPath);
        Payload->SetStringField(TEXT("issueId"), IssueId);
        if (!FixId.IsEmpty())
        {
            Payload->SetStringField(TEXT("fixId"), FixId);
        }
        FTestResponseCapture Capture;
        InvokeHandlerWithCapture(TEXT("niagara.apply_issue_fix"), Payload, Capture);
        return Capture;
    }

    // Bring the system's compiled state in line with its graphs. Building the editor view model
    // requests a non-forced compile, so listing an out-of-date system starts one and the next
    // call is refused COMPILE_IN_PROGRESS; a real caller compiles after editing, and so does this.
    bool SettleCompile(UNiagaraSystem& System)
    {
        System.RequestCompile(/*bForce=*/false);
        return !PinWrightNiagara::WaitForSystemCompile(System, /*bMayFlushRequestCompile=*/true).bOutstanding;
    }

    // Duplicates the stock fixture, lets any compile it carries finish, records the baseline
    // issue ids, then disables the dependency provider and returns the issue that appeared.
    // Returns false (after emitting a skip marker or failing) when the fixture cannot be staged.
    struct FStagedFixture
    {
        TStrongObjectPtr<UNiagaraSystem> System;
        FString Path;
        UNiagaraNodeFunctionCall* Provider = nullptr;
        TSet<FString> BaselineIds;
        FString IssueId;
        FString FixId;
    };

    bool StageDisabledProvider(FAutomationTestBase& Test, const TCHAR* Prefix, FStagedFixture& Out)
    {
        Out.System.Reset(NiagaraEditTestUtils::DuplicateFixtureSystemWithEmitters(Prefix, Out.Path));
        UNiagaraSystem* System = Out.System.Get();
        if (!System)
        {
            PinWrightTestSkip::SkipAssertions(Test, TEXT("niagara-fixture-system-unavailable"),
                FString::Printf(TEXT("Could not duplicate '%s'"), NiagaraEditTestUtils::FixtureSystemAssetPath));
            return false;
        }
        if (!SettleCompile(*System))
        {
            PinWrightTestSkip::SkipAssertions(Test, TEXT("niagara-fixture-compile-did-not-settle"),
                TEXT("the duplicated fixture still had compile work after the bounded wait"));
            return false;
        }

        Out.Provider = FindModuleNode(*System, ProviderScriptName);
        if (!Test.TestNotNull(TEXT("the fixture carries the dependency-provider module"), Out.Provider)
            || !Test.TestTrue(TEXT("the provider starts enabled"), IsEnabled(*Out.Provider)))
        {
            return false;
        }

        const TSharedPtr<FJsonObject> Baseline = ListIssues(Test, Out.Path);
        if (!Baseline.IsValid())
        {
            return false;
        }
        Out.BaselineIds = IssueIdSet(*Baseline);

        FNiagaraStackGraphUtilities::SetModuleIsEnabled(*Out.Provider, false);
        if (!Test.TestFalse(TEXT("the provider is now disabled"), IsEnabled(*Out.Provider))
            || !Test.TestTrue(TEXT("the edited fixture recompiles within the budget"), SettleCompile(*System)))
        {
            return false;
        }

        const TSharedPtr<FJsonObject> Broken = ListIssues(Test, Out.Path);
        if (!Broken.IsValid())
        {
            return false;
        }
        return Test.TestTrue(
            TEXT("disabling the provider surfaces a new module issue with exactly one applicable engine fix"),
            FindNewFixableIssue(*Broken, Out.BaselineIds, Out.IssueId, Out.FixId));
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNiagaraStackIssuesFixSelectionTest,
    "PinWright.niagara.stack_issues.FixSelection",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraStackIssuesFixSelectionTest::RunTest(const FString& Parameters)
{
    using FIssue = UNiagaraStackEntry::FStackIssue;
    using FFix = UNiagaraStackEntry::FStackIssueFix;
    const UNiagaraStackEntry::FStackIssueFixDelegate Bound =
        UNiagaraStackEntry::FStackIssueFixDelegate::CreateLambda([]() {});
    const FFix Apply(FText::FromString(TEXT("Apply")), Bound);
    const FFix Other(FText::FromString(TEXT("Other")), Bound);
    const FFix Link(FText::FromString(TEXT("Open docs")), Bound, UNiagaraStackEntry::EStackIssueFixStyle::Link);
    auto MakeIssue = [](const TArray<FFix>& Fixes)
    {
        return FIssue(EStackIssueSeverity::Error, FText::FromString(TEXT("Short")),
            FText::FromString(TEXT("Long")), TEXT("key"), false, Fixes);
    };

    int32 Index = INDEX_NONE;
    const FIssue FixAndLink = MakeIssue({ Apply, Link });
    TestNull(TEXT("omitted fixId selects the only applicable fix"),
        PinWrightNiagara::SelectStackIssueFix(FixAndLink, FString(), Index));
    TestEqual(TEXT("...which is the Fix-style one, not the link"), Index, 0);

    TestEqual(TEXT("a named link fix is refused FIX_IS_LINK"),
        FString(PinWrightNiagara::SelectStackIssueFix(FixAndLink, Link.GetUniqueIdentifier(), Index)),
        FString(TEXT("FIX_IS_LINK")));
    TestEqual(TEXT("a refusal selects nothing"), Index, INDEX_NONE);

    TestEqual(TEXT("an unknown fixId is refused FIX_NOT_FOUND"),
        FString(PinWrightNiagara::SelectStackIssueFix(FixAndLink, TEXT("not-a-fix"), Index)),
        FString(TEXT("FIX_NOT_FOUND")));

    TestNull(TEXT("a named Fix-style fix is selected"),
        PinWrightNiagara::SelectStackIssueFix(FixAndLink, Apply.GetUniqueIdentifier(), Index));
    TestEqual(TEXT("...at its own index"), Index, 0);

    TestEqual(TEXT("omitted fixId with two applicable fixes is refused FIX_AMBIGUOUS"),
        FString(PinWrightNiagara::SelectStackIssueFix(MakeIssue({ Apply, Other }), FString(), Index)),
        FString(TEXT("FIX_AMBIGUOUS")));

    TestEqual(TEXT("omitted fixId with only a link is refused FIX_NOT_FOUND"),
        FString(PinWrightNiagara::SelectStackIssueFix(MakeIssue({ Link }), FString(), Index)),
        FString(TEXT("FIX_NOT_FOUND")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNiagaraStackIssuesRoundTripTest,
    "PinWright.niagara.stack_issues.ApplyFixResolvesIssue",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraStackIssuesRoundTripTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightNiagaraStackIssuesTest;

    FStagedFixture Fixture;
    ON_SCOPE_EXIT
    {
        CleanupTestAsset(Fixture.Path);
    };
    if (!StageDisabledProvider(*this, TEXT("NS_StackIssueFix"), Fixture))
    {
        return true;
    }

    const FTestResponseCapture Applied = ApplyFix(Fixture.Path, Fixture.IssueId, Fixture.FixId);
    if (!TestTrue(FString::Printf(TEXT("apply_issue_fix succeeded (errorCode='%s' message='%s')"),
            *Applied.ErrorCode, *Applied.Message), Applied.bSuccess && Applied.Result.IsValid()))
    {
        return false;
    }

    // Read from the graph, not from the response: the write path cannot fake this.
    TestTrue(TEXT("the engine fix re-enabled the dependency provider"), IsEnabled(*Fixture.Provider));

    bool bIssueResolved = false;
    TestTrue(TEXT("the response carries a measured issueResolved"),
        Applied.Result->TryGetBoolField(TEXT("issueResolved"), bIssueResolved));
    TestTrue(TEXT("the response reports the issue resolved"), bIssueResolved);
    TestEqual(TEXT("the response echoes the applied fixId"),
        Applied.Result->GetStringField(TEXT("fixId")), Fixture.FixId);
    TestFalse(TEXT("the post-fix compile settled within the budget"),
        Applied.Result->GetObjectField(TEXT("compile"))->GetBoolField(TEXT("timedOut")));

    const TSharedPtr<FJsonObject> After = ListIssues(*this, Fixture.Path);
    if (!After.IsValid())
    {
        return false;
    }
    const TSet<FString> AfterIds = IssueIdSet(*After);
    TestFalse(TEXT("a fresh list no longer reports the fixed issue"), AfterIds.Contains(Fixture.IssueId));
    TestTrue(TEXT("a fresh list introduces nothing over the pre-disable baseline"),
        AfterIds.Difference(Fixture.BaselineIds).IsEmpty());
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNiagaraStackIssuesRefusalTest,
    "PinWright.niagara.stack_issues.UnknownIdsAreRefused",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraStackIssuesRefusalTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightNiagaraStackIssuesTest;

    FStagedFixture Fixture;
    ON_SCOPE_EXIT
    {
        CleanupTestAsset(Fixture.Path);
    };
    if (!StageDisabledProvider(*this, TEXT("NS_StackIssueRefuse"), Fixture))
    {
        return true;
    }

    const FTestResponseCapture UnknownFix = ApplyFix(Fixture.Path, Fixture.IssueId, TEXT("not-a-fix-id"));
    TestFalse(TEXT("an unknown fixId is refused"), UnknownFix.bSuccess);
    TestEqual(TEXT("an unknown fixId reports FIX_NOT_FOUND"), UnknownFix.ErrorCode, FString(TEXT("FIX_NOT_FOUND")));

    const FTestResponseCapture UnknownIssue = ApplyFix(Fixture.Path, TEXT("not-an-issue-id"), Fixture.FixId);
    TestFalse(TEXT("an unknown issueId is refused"), UnknownIssue.bSuccess);
    TestEqual(TEXT("an unknown issueId reports ISSUE_NOT_FOUND"), UnknownIssue.ErrorCode, FString(TEXT("ISSUE_NOT_FOUND")));

    // A refusal executes nothing: the issue's real fix would have re-enabled the provider.
    TestFalse(TEXT("refused calls left the provider disabled"), IsEnabled(*Fixture.Provider));
    const TSharedPtr<FJsonObject> After = ListIssues(*this, Fixture.Path);
    TestTrue(TEXT("the issue is still listed after the refusals"),
        After.IsValid() && IssueIdSet(*After).Contains(Fixture.IssueId));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNiagaraStackIssuesCompileGateTest,
    "PinWright.niagara.stack_issues.RefusedWhileCompiling",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraStackIssuesCompileGateTest::RunTest(const FString& Parameters)
{
    FString SystemPath;
    TStrongObjectPtr<UNiagaraSystem> SystemOwner(
        NiagaraEditTestUtils::DuplicateFixtureSystemWithEmitters(TEXT("NS_StackIssueCompile"), SystemPath));
    UNiagaraSystem* System = SystemOwner.Get();
    ON_SCOPE_EXIT
    {
        if (System)
        {
            PinWrightNiagara::WaitForSystemCompile(*System, /*bMayFlushRequestCompile=*/true);
        }
        CleanupTestAsset(SystemPath);
    };
    if (!System)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("niagara-fixture-system-unavailable"),
            FString::Printf(TEXT("Could not duplicate '%s'"), NiagaraEditTestUtils::FixtureSystemAssetPath));
        return true;
    }

    PinWrightNiagara::WaitForSystemCompile(*System, /*bMayFlushRequestCompile=*/true);
    if (UNiagaraScript* Script = System->GetSystemSpawnScript())
    {
        Script->InvalidateCompileResults(TEXT("PinWright stack-issue compile gate test"));
    }
    System->RequestCompile(/*bForce=*/true);
    if (!PinWrightNiagara::HasPendingCompileWork(*System))
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("niagara-compile-request-not-observable"),
            TEXT("the host did not expose an outstanding compile for the fixture"));
        return true;
    }

    TSharedPtr<FJsonObject> ListPayload = MakeShared<FJsonObject>();
    ListPayload->SetStringField(TEXT("assetPath"), SystemPath);
    NiagaraEditTestUtils::InvokeExpectError(*this, TEXT("niagara.list_stack_issues"), ListPayload, TEXT("COMPILE_IN_PROGRESS"));

    TSharedPtr<FJsonObject> ApplyPayload = MakeShared<FJsonObject>();
    ApplyPayload->SetStringField(TEXT("assetPath"), SystemPath);
    ApplyPayload->SetStringField(TEXT("issueId"), TEXT("any"));
    NiagaraEditTestUtils::InvokeExpectError(*this, TEXT("niagara.apply_issue_fix"), ApplyPayload, TEXT("COMPILE_IN_PROGRESS"));
    return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
