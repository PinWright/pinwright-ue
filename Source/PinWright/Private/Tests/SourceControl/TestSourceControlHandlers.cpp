// Copyright (c) 2026 Alexander Penkin. MIT License.

// Tests for source_control.* handlers — covers the get_provider response
// shape and the disabled-guard counterfactual on source_control.status.
// Counterfactual: if get_provider's SetStringField(TEXT("providerName"), ...)
// is omitted or misspelled, the HasField("providerName") assertion below fails.

#include "Misc/AutomationTest.h"
#include "Tests/TestUtils.h"
#include "Tests/TestSkipReporting.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "ISourceControlModule.h"
#include "ISourceControlProvider.h"
#include "ISourceControlState.h"
#include "ISourceControlRevision.h"
#include "Handlers/SourceControl/SourceControlStateFlags.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSourceControlGetProviderTest,
    "PinWright.source_control.GetProvider",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSourceControlGetProviderTest::RunTest(const FString& Parameters)
{
    FTestResponseCapture Capture;
    TestTrue(TEXT("source_control.get_provider handler found"),
        InvokeHandlerWithCapture(TEXT("source_control.get_provider"), MakeShared<FJsonObject>(), Capture));
    TestTrue(TEXT("response was sent"), Capture.bWasCalled);
    TestTrue(TEXT("handler reported success"), Capture.bSuccess);

    if (!Capture.Result.IsValid())
    {
        AddError(TEXT("Capture.Result is null"));
        return false;
    }

    TestTrue(TEXT("result has providerName"), Capture.Result->HasField(TEXT("providerName")));
    TestTrue(TEXT("result has isEnabled"), Capture.Result->HasField(TEXT("isEnabled")));
    TestTrue(TEXT("result has isAvailable"), Capture.Result->HasField(TEXT("isAvailable")));

    const FString ExpectedName = ISourceControlModule::Get().GetProvider().GetName().ToString();
    TestEqual(TEXT("providerName matches active provider"),
        Capture.Result->GetStringField(TEXT("providerName")),
        ExpectedName);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSourceControlStatusDisabledTest,
    "PinWright.source_control.StatusDisabledReturnsError",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSourceControlStatusDisabledTest::RunTest(const FString& Parameters)
{
    if (ISourceControlModule::Get().IsEnabled())
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("source-control-enabled"),
            TEXT("SCM enabled in test env, skipping disabled-path coverage"));
        return true;
    }

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    TArray<TSharedPtr<FJsonValue>> Paths;
    Paths.Add(MakeShared<FJsonValueString>(TEXT("/Game/Test")));
    Payload->SetArrayField(TEXT("assetPaths"), Paths);

    FTestResponseCapture Capture;
    TestTrue(TEXT("source_control.status handler found"),
        InvokeHandlerWithCapture(TEXT("source_control.status"), Payload, Capture));
    TestTrue(TEXT("response was sent"), Capture.bWasCalled);
    TestFalse(TEXT("handler reported error"), Capture.bSuccess);
    TestEqual(TEXT("error code is SOURCE_CONTROL_DISABLED"),
        Capture.ErrorCode, FString(TEXT("SOURCE_CONTROL_DISABLED")));
    return true;
}

// Regression guard for source_control.connect's provider-name validation. The
// handler MUST reject an unregistered provider name with INVALID_PROVIDER
// BEFORE reaching ISourceControlModule::SetProvider() — that engine API asserts
// (hard editor crash) on a name it never registered. If the membership guard
// before SetProvider were removed, invoking connect with a bogus name would
// crash/assert instead of returning this clean error, so this test would fail.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSourceControlConnectInvalidProviderTest,
    "PinWright.source_control.ConnectInvalidProviderRejected",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSourceControlConnectInvalidProviderTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("providerName"), TEXT("NoSuchProvider_xyz123"));

    FTestResponseCapture Capture;
    TestTrue(TEXT("source_control.connect handler found"),
        InvokeHandlerWithCapture(TEXT("source_control.connect"), Payload, Capture));
    TestTrue(TEXT("response was sent"), Capture.bWasCalled);
    TestFalse(TEXT("handler rejected the bogus provider (not a fake success)"), Capture.bSuccess);
    TestEqual(TEXT("error code is INVALID_PROVIDER"),
        Capture.ErrorCode, FString(TEXT("INVALID_PROVIDER")));

    // The rejection payload must list the valid provider names so a caller can
    // recover; this also proves the handler enumerated GetProviderNames() rather
    // than blindly forwarding the name to SetProvider().
    if (Capture.Result.IsValid())
    {
        const TArray<TSharedPtr<FJsonValue>>* Available = nullptr;
        TestTrue(TEXT("error result carries availableProviders array"),
            Capture.Result->TryGetArrayField(TEXT("availableProviders"), Available) && Available != nullptr);
    }
    else
    {
        AddError(TEXT("INVALID_PROVIDER response carried no result payload"));
    }
    return true;
}

// connect is the bring-up affordance, so unlike status/log/revert it must NOT be
// gated by RequireEnabledProvider — in a disabled session it has to actually run
// (and report flags), never short-circuit with SOURCE_CONTROL_DISABLED. Omitting
// providerName re-attempts the current provider; the handler must still respond
// with a success payload exposing the connectivity flags.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSourceControlConnectNotDisabledGatedTest,
    "PinWright.source_control.ConnectIsNotDisabledGated",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSourceControlConnectNotDisabledGatedTest::RunTest(const FString& Parameters)
{
    FTestResponseCapture Capture;
    TestTrue(TEXT("source_control.connect handler found"),
        InvokeHandlerWithCapture(TEXT("source_control.connect"), MakeShared<FJsonObject>(), Capture));
    TestTrue(TEXT("response was sent"), Capture.bWasCalled);
    // Whatever the connection outcome, the verb must never refuse with the
    // disabled-gate error the mutating verbs use — that would make it useless as
    // the bring-up entry point.
    TestNotEqual(TEXT("connect never returns SOURCE_CONTROL_DISABLED"),
        Capture.ErrorCode, FString(TEXT("SOURCE_CONTROL_DISABLED")));
    if (Capture.bSuccess)
    {
        TestTrue(TEXT("success result reports isEnabled flag"),
            Capture.Result.IsValid() && Capture.Result->HasField(TEXT("isEnabled")));
        TestTrue(TEXT("success result reports providerName"),
            Capture.Result.IsValid() && Capture.Result->HasField(TEXT("providerName")));
    }
    return true;
}

namespace
{
    // In-code ISourceControlState double for the status flag-derivation test.
    // Only the predicates source_control.status reads are configurable; every
    // other pure virtual returns an inert default. The clean-git configuration
    // mirrors FGitSourceControlState for EWorkingCopyState::Unchanged: under
    // control, no pending change, yet IsCheckedOut()==true (Git is lockless, so
    // every tracked file reads checked-out). Distinctly named to avoid an
    // anonymous-namespace collision if Unity merges this TU with a sibling test.
    class FFakeSourceControlStateForStatusTest final : public ISourceControlState
    {
    public:
        bool bSourceControlled = true;
        bool bModified = false;
        bool bAdded = false;
        bool bDeleted = false;
        bool bCheckedOut = false;
        bool bUnknown = false;

        // Predicates the status flag derivation consumes.
        virtual bool IsSourceControlled() const override { return bSourceControlled; }
        virtual bool IsModified() const override { return bModified; }
        virtual bool IsAdded() const override { return bAdded; }
        virtual bool IsDeleted() const override { return bDeleted; }
        virtual bool IsCheckedOut() const override { return bCheckedOut; }
        virtual bool IsUnknown() const override { return bUnknown; }
        virtual bool CanCheckout() const override { return false; }

        // Inert defaults for the remainder of the interface.
        virtual int32 GetHistorySize() const override { return 0; }
        virtual TSharedPtr<ISourceControlRevision, ESPMode::ThreadSafe> GetHistoryItem(int32) const override { return nullptr; }
        virtual TSharedPtr<ISourceControlRevision, ESPMode::ThreadSafe> FindHistoryRevision(int32) const override { return nullptr; }
        virtual TSharedPtr<ISourceControlRevision, ESPMode::ThreadSafe> FindHistoryRevision(const FString&) const override { return nullptr; }
        virtual TSharedPtr<ISourceControlRevision, ESPMode::ThreadSafe> GetCurrentRevision() const override { return nullptr; }
#if SOURCE_CONTROL_WITH_SLATE
        virtual FSlateIcon GetIcon() const override { return FSlateIcon(); }
#endif
        virtual FText GetDisplayName() const override { return FText::GetEmpty(); }
        virtual FText GetDisplayTooltip() const override { return FText::GetEmpty(); }
        virtual const FString& GetFilename() const override { return Filename; }
        virtual const FDateTime& GetTimeStamp() const override { return TimeStamp; }
        virtual bool CanCheckIn() const override { return false; }
        virtual bool IsCheckedOutOther(FString*) const override { return false; }
        virtual bool IsCheckedOutInOtherBranch(const FString&) const override { return false; }
        virtual bool IsModifiedInOtherBranch(const FString&) const override { return false; }
        virtual bool IsCheckedOutOrModifiedInOtherBranch(const FString&) const override { return false; }
        virtual TArray<FString> GetCheckedOutBranches() const override { return TArray<FString>(); }
        virtual FString GetOtherUserBranchCheckedOuts() const override { return FString(); }
        virtual bool GetOtherBranchHeadModification(FString&, FString&, int32&) const override { return false; }
        virtual bool IsCurrent() const override { return true; }
        virtual bool IsIgnored() const override { return false; }
        virtual bool CanEdit() const override { return true; }
        virtual bool CanDelete() const override { return false; }
        virtual bool CanAdd() const override { return false; }
        virtual bool CanRevert() const override { return false; }

    private:
        FString Filename;
        FDateTime TimeStamp;
    };
}

// Regression guard for board E-source-control-status-isunchanged-always-false-git.
// The per-file "isUnchanged" (pristine) flag must NOT fold in IsCheckedOut():
// under the lockless Git provider every tracked file reports IsCheckedOut()==true
// (FGitSourceControlState::IsCheckedOut() returns IsSourceControlled()), so the
// earlier "&& !IsCheckedOut()" term pinned isUnchanged=false for EVERY clean git
// file and the field could never answer "is this file clean?". This drives the
// shared production derivation (WriteStateFlags — the exact code path
// source_control.status runs to emit these flags) with in-code state doubles, so
// it needs no live provider. If the !IsCheckedOut() term is restored, the
// clean-git-file case below flips isUnchanged back to false and this test fails.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSourceControlStatusCleanGitFileUnchangedTest,
    "PinWright.source_control.StatusCleanGitFileReportsUnchanged",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSourceControlStatusCleanGitFileUnchangedTest::RunTest(const FString& Parameters)
{
    using namespace PinWright::SourceControl;

    // Clean, tracked file under lockless Git: checked-out yet pristine.
    {
        FFakeSourceControlStateForStatusTest CleanGit;
        CleanGit.bSourceControlled = true;
        CleanGit.bCheckedOut = true;   // Git: every tracked file reads checked-out
        CleanGit.bModified = false;
        CleanGit.bAdded = false;
        CleanGit.bDeleted = false;

        FJsonObject Out;
        WriteStateFlags(CleanGit, Out);

        TestTrue(TEXT("clean tracked git file passes isCheckedOut:true through"),
            Out.GetBoolField(TEXT("isCheckedOut")));
        TestFalse(TEXT("clean file reports isModified:false"),
            Out.GetBoolField(TEXT("isModified")));
        // The regression: with the old "&& !IsCheckedOut()" term this was false.
        TestTrue(TEXT("clean tracked git file reports isUnchanged:true"),
            Out.GetBoolField(TEXT("isUnchanged")));
    }

    // A modified tracked file must never report unchanged.
    {
        FFakeSourceControlStateForStatusTest Dirty;
        Dirty.bSourceControlled = true;
        Dirty.bCheckedOut = true;
        Dirty.bModified = true;

        FJsonObject Out;
        WriteStateFlags(Dirty, Out);
        TestTrue(TEXT("modified file reports isModified:true"),
            Out.GetBoolField(TEXT("isModified")));
        TestFalse(TEXT("modified file reports isUnchanged:false"),
            Out.GetBoolField(TEXT("isUnchanged")));
    }

    // A file marked for add is a pending change, not unchanged.
    {
        FFakeSourceControlStateForStatusTest Added;
        Added.bSourceControlled = true;
        Added.bCheckedOut = true;
        Added.bAdded = true;

        FJsonObject Out;
        WriteStateFlags(Added, Out);
        TestFalse(TEXT("added file reports isUnchanged:false"),
            Out.GetBoolField(TEXT("isUnchanged")));
    }

    // A file not under source control is not "unchanged from the depot" even
    // though it carries no pending add/delete/modify (guards the
    // IsSourceControlled() term; the old formula wrongly reported it unchanged).
    {
        FFakeSourceControlStateForStatusTest Untracked;
        Untracked.bSourceControlled = false;
        Untracked.bCheckedOut = false;
        Untracked.bModified = false;

        FJsonObject Out;
        WriteStateFlags(Untracked, Out);
        TestFalse(TEXT("not-source-controlled file reports isUnchanged:false"),
            Out.GetBoolField(TEXT("isUnchanged")));
    }

    return true;
}
