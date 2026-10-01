// Copyright (c) 2026 Alexander Penkin. MIT License.

// NiagaraStackIssueHandler.cpp
// Handlers: niagara.list_stack_issues, niagara.apply_issue_fix
//
// These are the Niagara editor stack panel's own issues - UNiagaraStackEntry::GetIssues() with
// their engine one-click FStackIssueFix delegates (unmet module dependencies, deprecated
// versions, validation-rule results, ...). They are NOT niagara.validate's issues, which
// PinWright synthesizes from the compile log and carry no fix.
//
// UE 5.8 wraps the same walk as UNiagaraExternalEditUtilities::GetStackIssues /
// ApplyStackIssueFix (NiagaraExternalSystemEditorUtilities.cpp), which does not exist before
// 5.8. This mirrors it over the low-level stack API that every supported engine exports.

#include "Handlers/Niagara/NiagaraStackIssues.h"

#include "Compat/EngineVersionCompat.h"
#include "Handlers/ErrorCodes.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/HandlerRegistration.h"
#include "Handlers/Niagara/NiagaraCompileWait.h"
#include "Handlers/Niagara/NiagaraEditTypes.h"
#include "Utils/JsonUtils.h"
#include "Utils/PackageDirtyUtils.h"

#include "NiagaraEmitter.h"
#include "NiagaraNodeFunctionCall.h"
#include "NiagaraScript.h"
#include "NiagaraScriptSourceBase.h"
#include "NiagaraSystem.h"
#include "ScopedTransaction.h"
#include "ViewModels/NiagaraEmitterHandleViewModel.h"
#include "ViewModels/NiagaraSystemViewModel.h"
#include "ViewModels/Stack/NiagaraStackModuleItem.h"
#include "ViewModels/Stack/NiagaraStackScriptItemGroup.h"
#include "ViewModels/Stack/NiagaraStackViewModel.h"

namespace PinWrightNiagara
{
    namespace
    {
        bool IsApplicableFix(const UNiagaraStackEntry::FStackIssueFix& Fix)
        {
            return Fix.GetStyle() == UNiagaraStackEntry::EStackIssueFixStyle::Fix
                && Fix.GetFixDelegate().IsBound();
        }
    }

    const TCHAR* SelectStackIssueFix(
        const UNiagaraStackEntry::FStackIssue& Issue,
        const FString& FixId,
        int32& OutIndex)
    {
        OutIndex = INDEX_NONE;
        const TArray<UNiagaraStackEntry::FStackIssueFix>& Fixes = Issue.GetFixes();
        if (!FixId.IsEmpty())
        {
            const int32 Index = Fixes.IndexOfByPredicate(
                [&FixId](const UNiagaraStackEntry::FStackIssueFix& Fix) { return Fix.GetUniqueIdentifier() == FixId; });
            if (Index == INDEX_NONE)
            {
                return ErrorCodes::ERR_FIX_NOT_FOUND;
            }
            // Link-style fixes navigate (open an asset editor, a settings page); executing one
            // from an RPC would pop UI and change nothing in the asset.
            if (Fixes[Index].GetStyle() == UNiagaraStackEntry::EStackIssueFixStyle::Link)
            {
                return ErrorCodes::ERR_FIX_IS_LINK;
            }
            if (!Fixes[Index].GetFixDelegate().IsBound())
            {
                return ErrorCodes::ERR_FIX_NOT_FOUND;
            }
            OutIndex = Index;
            return nullptr;
        }

        int32 Selected = INDEX_NONE;
        for (int32 Index = 0; Index < Fixes.Num(); ++Index)
        {
            if (!IsApplicableFix(Fixes[Index]))
            {
                continue;
            }
            if (Selected != INDEX_NONE)
            {
                return ErrorCodes::ERR_FIX_AMBIGUOUS;
            }
            Selected = Index;
        }
        if (Selected == INDEX_NONE)
        {
            return ErrorCodes::ERR_FIX_NOT_FOUND;
        }
        OutIndex = Selected;
        return nullptr;
    }
}

// Named, not anonymous: this module is a unity build, so file-local helper names would collide.
namespace PinWrightStackIssueHandler
{
    // 5.8 hashes LOCTEXT namespace/key (MakeStableIdentifierInput, NiagaraStackEntry.cpp), so ids
    // survive editor restarts and culture changes. Earlier engines hash the localized display
    // string, so an id is only good for the current session and culture.
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 8, 0)
    constexpr const TCHAR* GIdStability = TEXT("stable");
#else
    constexpr const TCHAR* GIdStability = TEXT("session");
#endif

    struct FFoundIssue
    {
        UNiagaraStackEntry* Entry = nullptr;
        int32 IssueIndex = INDEX_NONE;
        FString Emitter;
        FString ScriptUsage;
        FString Module;
        FString StackPath;

        const UNiagaraStackEntry::FStackIssue& Issue() const { return Entry->GetIssues()[IssueIndex]; }
    };

    struct FWalkLocation
    {
        FString Emitter;
        FString ScriptUsage;
        FString Module;
        TArray<FString> Path;
    };

    // Depth-first over unfiltered children, the order Epic's 5.8 walker uses. No extra
    // RefreshChildren: the view model built the tree (and ran the validation rules) at init.
    void WalkEntry(UNiagaraStackEntry* Entry, FWalkLocation Location, TArray<FFoundIssue>& Out)
    {
        if (!Entry)
        {
            return;
        }
        if (const UNiagaraStackScriptItemGroup* Group = Cast<UNiagaraStackScriptItemGroup>(Entry))
        {
            Location.ScriptUsage = NiagaraEdit::StackScriptUsageToString(Group->GetScriptUsage());
            Location.Module.Reset();
        }
        else if (const UNiagaraStackModuleItem* ModuleItem = Cast<UNiagaraStackModuleItem>(Entry))
        {
            Location.Module = ModuleItem->GetModuleNode().GetFunctionName();
        }
        const FString DisplayName = Entry->GetDisplayName().ToString();
        if (!DisplayName.IsEmpty())
        {
            Location.Path.Add(DisplayName);
        }

        const TArray<UNiagaraStackEntry::FStackIssue>& Issues = Entry->GetIssues();
        for (int32 IssueIndex = 0; IssueIndex < Issues.Num(); ++IssueIndex)
        {
            FFoundIssue& Found = Out.AddDefaulted_GetRef();
            Found.Entry = Entry;
            Found.IssueIndex = IssueIndex;
            Found.Emitter = Location.Emitter;
            Found.ScriptUsage = Location.ScriptUsage;
            Found.Module = Location.Module;
            Found.StackPath = FString::Join(Location.Path, TEXT("/"));
        }

        TArray<UNiagaraStackEntry*> Children;
        Entry->GetUnfilteredChildren(Children);
        for (UNiagaraStackEntry* Child : Children)
        {
            WalkEntry(Child, Location, Out);
        }
    }

    TArray<FFoundIssue> CollectIssues(FNiagaraSystemViewModel& ViewModel)
    {
        TArray<FFoundIssue> Found;
        if (UNiagaraStackViewModel* SystemStack = ViewModel.GetSystemStackViewModel())
        {
            WalkEntry(SystemStack->GetRootEntry(), FWalkLocation(), Found);
        }
        for (const TSharedRef<FNiagaraEmitterHandleViewModel>& EmitterViewModel : ViewModel.GetEmitterHandleViewModels())
        {
            if (UNiagaraStackViewModel* EmitterStack = EmitterViewModel->GetEmitterStackViewModel())
            {
                FWalkLocation Location;
                Location.Emitter = EmitterViewModel->GetName().ToString();
                WalkEntry(EmitterStack->GetRootEntry(), Location, Found);
            }
        }
        return Found;
    }

    // A FULL view model, built per phase and never cached. The session cache
    // (AcquireSystemViewModel) is data-only, and a data-only view model empties module issues
    // (UNiagaraStackModuleItem::RefreshIssues); a cached full one would keep stale validation
    // issues, because the rules run at init. Options mirror Epic's 5.8 CreateSystemViewModel.
    TSharedRef<FNiagaraSystemViewModel> MakeDiagnosticsViewModel(UNiagaraSystem& System)
    {
        TSharedRef<FNiagaraSystemViewModel> ViewModel = MakeShared<FNiagaraSystemViewModel>();
        FNiagaraSystemViewModelOptions Options;
        Options.bCanModifyEmittersFromTimeline = false;
        Options.bCanAutoCompile = false;
        Options.bCanSimulate = false;
        Options.bIsForDataProcessingOnly = false;
        Options.EditMode = ENiagaraSystemViewModelEditMode::SystemAsset;
        // Seeds the stack's message subscriptions; FGuid() trips a checkf in the message manager.
        Options.MessageLogGuid = System.GetAssetGuid();
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 6, 0)
        // Compile-for-edit (5.6+) would switch the system to edit compiles and recompile it when
        // this throwaway view model is cleaned up.
        Options.bCompileForEdit = false;
#endif
        ViewModel->Initialize(System, Options);
        return ViewModel;
    }

    const TCHAR* SeverityToString(EStackIssueSeverity Severity)
    {
        switch (Severity)
        {
        case EStackIssueSeverity::Error:   return TEXT("error");
        case EStackIssueSeverity::Warning: return TEXT("warning");
        case EStackIssueSeverity::Info:    return TEXT("info");
        default:                           return TEXT("none");
        }
    }

    TArray<TSharedPtr<FJsonValue>> FixesToJson(const UNiagaraStackEntry::FStackIssue& Issue)
    {
        TArray<TSharedPtr<FJsonValue>> Fixes;
        for (const UNiagaraStackEntry::FStackIssueFix& Fix : Issue.GetFixes())
        {
            TSharedPtr<FJsonObject> FixJson = MakeShared<FJsonObject>();
            FixJson->SetStringField(TEXT("fixId"), Fix.GetUniqueIdentifier());
            FixJson->SetStringField(TEXT("description"), Fix.GetDescription().ToString());
            FixJson->SetStringField(TEXT("style"),
                Fix.GetStyle() == UNiagaraStackEntry::EStackIssueFixStyle::Link ? TEXT("link") : TEXT("fix"));
            FixJson->SetBoolField(TEXT("applicable"), PinWrightNiagara::IsApplicableFix(Fix));
            Fixes.Add(MakeShared<FJsonValueObject>(FixJson));
        }
        return Fixes;
    }

    TSharedPtr<FJsonValue> OptionalString(const FString& Value)
    {
        return Value.IsEmpty()
            ? StaticCastSharedRef<FJsonValue>(MakeShared<FJsonValueNull>())
            : StaticCastSharedRef<FJsonValue>(MakeShared<FJsonValueString>(Value));
    }

    // Writes issues[] plus per-severity counts onto Result.
    void AddIssues(FJsonObject& Result, const TArray<FFoundIssue>& Found)
    {
        TArray<TSharedPtr<FJsonValue>> Issues;
        int32 Errors = 0;
        int32 Warnings = 0;
        int32 Infos = 0;
        for (const FFoundIssue& Entry : Found)
        {
            const UNiagaraStackEntry::FStackIssue& Issue = Entry.Issue();
            TSharedPtr<FJsonObject> IssueJson = MakeShared<FJsonObject>();
            IssueJson->SetStringField(TEXT("issueId"), Issue.GetUniqueIdentifier());
            IssueJson->SetStringField(TEXT("severity"), SeverityToString(Issue.GetSeverity()));
            IssueJson->SetStringField(TEXT("shortDescription"), Issue.GetShortDescription().ToString());
            IssueJson->SetStringField(TEXT("longDescription"), Issue.GetLongDescription().ToString());
            IssueJson->SetBoolField(TEXT("canBeDismissed"), Issue.GetCanBeDismissed());
            IssueJson->SetField(TEXT("emitter"), OptionalString(Entry.Emitter));
            IssueJson->SetField(TEXT("scriptUsage"), OptionalString(Entry.ScriptUsage));
            IssueJson->SetField(TEXT("module"), OptionalString(Entry.Module));
            IssueJson->SetStringField(TEXT("stackPath"), Entry.StackPath);
            IssueJson->SetArrayField(TEXT("fixes"), FixesToJson(Issue));
            Issues.Add(MakeShared<FJsonValueObject>(IssueJson));

            Errors += Issue.GetSeverity() == EStackIssueSeverity::Error ? 1 : 0;
            Warnings += Issue.GetSeverity() == EStackIssueSeverity::Warning ? 1 : 0;
            Infos += Issue.GetSeverity() == EStackIssueSeverity::Info ? 1 : 0;
        }
        Result.SetStringField(TEXT("idStability"), GIdStability);
        Result.SetNumberField(TEXT("issueCount"), Found.Num());
        Result.SetNumberField(TEXT("errorCount"), Errors);
        Result.SetNumberField(TEXT("warningCount"), Warnings);
        Result.SetNumberField(TEXT("infoCount"), Infos);
        Result.SetArrayField(TEXT("issues"), Issues);
    }

    TArray<FString> IssueIds(const TArray<FFoundIssue>& Found)
    {
        TArray<FString> Ids;
        for (const FFoundIssue& Entry : Found)
        {
            Ids.Add(Entry.Issue().GetUniqueIdentifier());
        }
        return Ids;
    }

    // Loads the system and refuses anything a stack walk cannot honestly answer for.
    bool ResolveIdleSystem(FHandlerContext& Ctx, const FString& AssetPath, UNiagaraSystem*& OutSystem)
    {
        OutSystem = LoadObject<UNiagaraSystem>(nullptr, *AssetPath);
        if (!OutSystem)
        {
            if (LoadObject<UNiagaraEmitter>(nullptr, *AssetPath))
            {
                Ctx.SendError(ErrorCodes::ERR_ASSET_WRONG_TYPE, FString::Printf(
                    TEXT("'%s' is a Niagara Emitter asset. Stack issues are read through a Niagara System: pass a system that uses this emitter; its issues carry the emitter name."),
                    *AssetPath));
            }
            else
            {
                Ctx.SendError(ErrorCodes::ERR_ASSET_NOT_FOUND,
                    FString::Printf(TEXT("Could not load Niagara System '%s'."), *AssetPath));
            }
            return false;
        }

        // FNiagaraSystemViewModel::Initialize compiles a system whose scripts have no source and
        // crashes (see AcquireSystemViewModel); only a bare NewObject system is in this state.
        const UNiagaraScript* SpawnScript = OutSystem->GetSystemSpawnScript();
        const UNiagaraScript* UpdateScript = OutSystem->GetSystemUpdateScript();
        if (!SpawnScript || !SpawnScript->GetLatestSource() || !UpdateScript || !UpdateScript->GetLatestSource())
        {
            Ctx.SendError(ErrorCodes::ERR_ASSET_WRONG_TYPE, FString::Printf(
                TEXT("Niagara System '%s' has no system script source (an uninitialized object, not a saved asset), so it has no editor stack."),
                *AssetPath));
            return false;
        }

        // Stack issues depend on compile output; a snapshot mid-compile mixes both states, and a
        // fix id taken from it may already be stale. Epic's 5.8 facade refuses the same way.
        if (PinWrightNiagara::HasPendingCompileWork(*OutSystem))
        {
            Ctx.SendError(ErrorCodes::ERR_COMPILE_IN_PROGRESS, FString::Printf(
                TEXT("Niagara System '%s' has compile work in flight. Wait for it (niagara.compile {wait:true} or poll niagara.compile_status), then retry."),
                *AssetPath));
            return false;
        }
        return true;
    }
}

// ---- niagara.list_stack_issues ----
REGISTER_RPC_HANDLER("niagara.list_stack_issues", "niagara",
    "List the Niagara editor stack's own issues for a system (what the stack panel flags), each with an issueId, its location and the engine's one-click fixes for niagara.apply_issue_fix.",
    RPC_PARAMS(
        RPC_PARAM_REQ("assetPath", "path", "Niagara System asset path. Emitter-level issues are listed through the owning system.")
    ))
{
    using namespace PinWrightStackIssueHandler;

    FString AssetPath;
    if (!Ctx.RequireString(TEXT("assetPath"), AssetPath)) return true;

    UNiagaraSystem* System = nullptr;
    if (!ResolveIdleSystem(Ctx, AssetPath, System)) return true;

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("assetPath"), AssetPath);
    {
        // A read verb must leave the dirty flag as it found it; building a full view model
        // refreshes stack editor data that may Modify() the asset.
        PinWright::PackageDirty::FScopedPackageDirtyRestore DirtyGuard;
        DirtyGuard.Capture(System);
        const TSharedRef<FNiagaraSystemViewModel> ViewModel = MakeDiagnosticsViewModel(*System);
        AddIssues(*Result, CollectIssues(*ViewModel));
    }
    // FNiagaraSystemViewModel::RefreshAll requests a non-forced compile at init, which starts one
    // only for an out-of-date system. Report it rather than hide it.
    Result->SetBoolField(TEXT("compilePending"), PinWrightNiagara::HasPendingCompileWork(*System));
    Ctx.SendSuccess(Result);
    return true;
}

// ---- niagara.apply_issue_fix ----
REGISTER_RPC_HANDLER("niagara.apply_issue_fix", "niagara",
    "Apply one engine stack-issue fix (issueId/fixId from niagara.list_stack_issues), recompile with a bounded wait, and report the issue set before and after. Does not save.",
    RPC_PARAMS(
        RPC_PARAM_REQ("assetPath", "path", "Niagara System asset path"),
        RPC_PARAM_REQ("issueId", "string", "issueId from niagara.list_stack_issues"),
        RPC_PARAM_OPT("fixId", "string",
            "fixId from the issue's fixes. Omit only when the issue has exactly one applicable fix (FIX_AMBIGUOUS otherwise)."),
        RPC_PARAM_DEF("timeoutSeconds", "number",
            "Budget for the post-fix compile wait (0.01-60). On expiry the fix stays applied, issueResolved is null and stillCompiling names the system.", "60")
    ))
{
    using namespace PinWrightStackIssueHandler;

    FString AssetPath;
    FString IssueId;
    if (!Ctx.RequireString(TEXT("assetPath"), AssetPath)) return true;
    if (!Ctx.RequireString(TEXT("issueId"), IssueId)) return true;
    const FString FixId = Ctx.GetString(TEXT("fixId"));

    double TimeoutSeconds = PinWrightNiagara::DefaultCompileWaitTimeoutSeconds;
    const TSharedPtr<FJsonObject>& Payload = Ctx.GetRawPayload();
    if (Payload.IsValid() && Payload->HasField(TEXT("timeoutSeconds")))
    {
        if (!Ctx.RequireNumber(TEXT("timeoutSeconds"), TimeoutSeconds)) return true;
        if (!FMath::IsFinite(TimeoutSeconds)
            || TimeoutSeconds < PinWrightNiagara::MinCompileWaitTimeoutSeconds
            || TimeoutSeconds > PinWrightNiagara::MaxCompileWaitTimeoutSeconds)
        {
            Ctx.SendError(ErrorCodes::ERR_INVALID_PARAMS,
                FString::Printf(TEXT("timeoutSeconds must be between %.2f and %.0f."),
                    PinWrightNiagara::MinCompileWaitTimeoutSeconds,
                    PinWrightNiagara::MaxCompileWaitTimeoutSeconds));
            return true;
        }
    }

    UNiagaraSystem* System = nullptr;
    if (!ResolveIdleSystem(Ctx, AssetPath, System)) return true;

    TArray<FString> BeforeIds;
    FString AppliedFixId;
    FString AppliedFixDescription;
    {
        const TSharedRef<FNiagaraSystemViewModel> ViewModel = MakeDiagnosticsViewModel(*System);
        const TArray<FFoundIssue> Before = CollectIssues(*ViewModel);
        BeforeIds = IssueIds(Before);

        const FFoundIssue* Match = nullptr;
        int32 MatchCount = 0;
        for (const FFoundIssue& Found : Before)
        {
            if (Found.Issue().GetUniqueIdentifier() == IssueId)
            {
                Match = &Found;
                ++MatchCount;
            }
        }
        if (MatchCount == 0)
        {
            Ctx.SendError(ErrorCodes::ERR_ISSUE_NOT_FOUND, FString::Printf(
                TEXT("No stack issue '%s' on '%s' (%d issues now). Ids change when the issue's text or the stack changes; re-run niagara.list_stack_issues."),
                *IssueId, *AssetPath, Before.Num()));
            return true;
        }
        if (MatchCount > 1)
        {
            Ctx.SendError(ErrorCodes::ERR_ISSUE_AMBIGUOUS, FString::Printf(
                TEXT("Stack issue '%s' is reported by %d entries of '%s'; refusing to guess which one to fix."),
                *IssueId, MatchCount, *AssetPath));
            return true;
        }

        const UNiagaraStackEntry::FStackIssue& Issue = Match->Issue();
        int32 FixIndex = INDEX_NONE;
        if (const TCHAR* Code = PinWrightNiagara::SelectStackIssueFix(Issue, FixId, FixIndex))
        {
            TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
            Data->SetStringField(TEXT("issueId"), IssueId);
            Data->SetArrayField(TEXT("fixes"), FixesToJson(Issue));
            Ctx.SendError(Code, FString::Printf(
                TEXT("Issue '%s' (%s): fixId '%s' selects no single applicable fix. Pass an applicable fixId (style \"fix\") from data.fixes; link-style fixes only open editor UI."),
                *IssueId, *Issue.GetShortDescription().ToString(), *FixId),
                Data);
            return true;
        }

        const UNiagaraStackEntry::FStackIssueFix& Fix = Issue.GetFixes()[FixIndex];
        AppliedFixId = Fix.GetUniqueIdentifier();
        AppliedFixDescription = Fix.GetDescription().ToString();
        // Copy before executing: the fix may refresh the entry and free the array it lives in.
        // Executed while the view model is alive, since fix delegates capture it.
        const UNiagaraStackEntry::FStackIssueFixDelegate FixDelegate = Fix.GetFixDelegate();
        FScopedTransaction Transaction(FText::FromString(TEXT("MCP: niagara.apply_issue_fix")));
        FixDelegate.Execute();
    }

    // The fix edited graphs/properties; compile so the re-list reads post-fix compile output.
    FNiagaraResolvedTarget Target;
    Target.Asset = System;
    Target.System = System;
    Target.AssetPath = AssetPath;
    Target.AssetKind = TEXT("NiagaraSystem");
    const bool bRequested = NiagaraEdit::RequestNiagaraCompile(Target, /*bForce=*/false);
    // This call mutated the asset, so it is entitled to drain a queued request.
    const PinWrightNiagara::FCompileWaitOutcome Wait =
        PinWrightNiagara::WaitForSystemCompile(*System, /*bMayFlushRequestCompile=*/true, TimeoutSeconds);

    TSharedPtr<FJsonObject> Compile = MakeShared<FJsonObject>();
    Compile->SetBoolField(TEXT("requested"), bRequested);
    Compile->SetBoolField(TEXT("waited"), Wait.bWaited);
    Compile->SetNumberField(TEXT("waitedMs"), Wait.WaitedSeconds * 1000.0);
    Compile->SetBoolField(TEXT("timedOut"), Wait.bTimedOut);
    Compile->SetArrayField(TEXT("stillCompiling"), EmitStringArray(Wait.StillCompiling));
    Compile->SetStringField(TEXT("status"), PinWrightNiagara::DescribeCompileOutcome(bRequested, Wait));

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("assetPath"), AssetPath);
    Result->SetStringField(TEXT("issueId"), IssueId);
    Result->SetStringField(TEXT("fixId"), AppliedFixId);
    Result->SetStringField(TEXT("fixDescription"), AppliedFixDescription);
    Result->SetObjectField(TEXT("compile"), Compile);
    Result->SetArrayField(TEXT("issueIdsBefore"), EmitStringArray(BeforeIds));

    if (Wait.bOutstanding)
    {
        // A list taken now would mix pre- and post-compile state; publish no verdict.
        Result->SetField(TEXT("issueResolved"), MakeShared<FJsonValueNull>());
    }
    else
    {
        // A NEW view model: the one the fix ran in keeps external/validation issues across
        // RefreshChildren, so only a fresh build shows what the fix actually changed.
        const TSharedRef<FNiagaraSystemViewModel> ViewModel = MakeDiagnosticsViewModel(*System);
        const TArray<FFoundIssue> After = CollectIssues(*ViewModel);
        const TArray<FString> AfterIds = IssueIds(After);
        const TSet<FString> BeforeSet(BeforeIds);
        const TSet<FString> AfterSet(AfterIds);
        TArray<FString> Resolved = BeforeSet.Difference(AfterSet).Array();
        TArray<FString> Introduced = AfterSet.Difference(BeforeSet).Array();
        Resolved.Sort();
        Introduced.Sort();

        Result->SetBoolField(TEXT("issueResolved"), !AfterSet.Contains(IssueId));
        Result->SetArrayField(TEXT("resolvedIssueIds"), EmitStringArray(Resolved));
        Result->SetArrayField(TEXT("introducedIssueIds"), EmitStringArray(Introduced));
        TSharedPtr<FJsonObject> AfterJson = MakeShared<FJsonObject>();
        AddIssues(*AfterJson, After);
        Result->SetObjectField(TEXT("after"), AfterJson);
    }
    Result->SetBoolField(TEXT("packageDirty"), System->GetPackage()->IsDirty());
    Ctx.SendSuccess(Result);
    return true;
}
