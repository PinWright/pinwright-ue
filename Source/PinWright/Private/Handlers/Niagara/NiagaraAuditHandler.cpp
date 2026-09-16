// Copyright (c) 2026 Alexander Penkin. MIT License.

// NiagaraAuditHandler.cpp - niagara.audit_level, the level-scope pre-flight for the fault that
// kills a shared editor mid-capture.
//
// THE FAULT. A UNiagaraSystem whose compiled DataInterfaceInfo count disagrees with its resolved
// set asserts inside the VectorVM (`DataSetIdx < ExecCtx->DataSets.Num()`) on a concurrent worker
// the next time anything ticks it. That is an appError, so it takes the whole editor process down,
// minutes after and unrelated to the write that armed it.
//
// WHY THE SUBJECT IS THE LEVEL AND NOT A SEQUENCE'S BINDINGS. Every other surface that can see
// this state takes a `systemPath` the caller already suspects; nothing answered "is anything
// loaded here fatal on its next tick". Two engine facts make the scope non-negotiable:
//
//   * SCRUBBING FORCES A WORLD TICK BY EXPLICIT DESIGN. LevelEditorSequencerIntegration registers
//     OnSequencerEvaluated on OnGlobalTimeChanged, so it fires on every playhead move; it calls
//     ReRenderLevelViewports(), whose engine-authored comment states the intent outright - "Request
//     a single real-time frame to be rendered to ensure that we tick the world and update the
//     viewport" - via RequestRealTimeFrames(1). That promotes the next frame to
//     LEVELTICK_ViewportsOnly (LEVELTICK_TimeOnly runs no tick groups at all), which runs the full
//     tick-group pass, which reaches FNiagaraWorldManagerTickFunction::ExecuteTick and then
//     ExecuteSimulations - iterating EVERY system simulation in the world, with no reference to
//     Sequencer bindings.
//   * A BINDING-SCOPED CHECK WOULD INSPECT THE SAFEST SUBSET.
//     FNiagaraSystemUpdateDesiredAgeExecutionToken::Execute calls SetForceSolo(true) on the objects
//     it binds, which pulls those systems OUT of the batched world simulation and onto their own
//     component tick. The systems such a gate would check are precisely the ones removed from the
//     path that ticks everything else. It is unsound, not merely narrow.
//
// So this verb walks every live UNiagaraComponent through TObjectIterator - the same walk
// PinWrightNiagara::KillSystemInstances already performs - and never Sequencer's FindBoundObjects.
//
// KEYED BY SYSTEM, NOT BY ACTOR. The subject that is fatal is the UNiagaraSystem asset. N actors
// can share one system, and a component in an asset-editor preview scene has no actor at all, so
// results are deduplicated to distinct systems and the components / owning actors are reported as
// the system's row rather than as the key. The measurement therefore runs once per system.
//
// NO NEW MEASUREMENT CODE. PinWrightNiagara::CheckDataInterfaceCounts and
// FindOrphanResolvedDataInterfaces are the measurement; this file is discovery plus the shared
// audit contract (Audit/AuditFramework.h, docs/rpc-design.md section 18). `unverified` maps to an
// UNRUNNABLE row and never a clean one: a system nothing has compiled this session is unmeasured,
// and an empty orphan list on Unverified is not evidence of cleanliness.
//
// Read-only: loads nothing, compiles nothing, saves nothing, dirties nothing.

#include "Audit/AuditFramework.h"
#include "Handlers/ErrorCodes.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/HandlerRegistration.h"
#include "Handlers/Niagara/NiagaraDataInterfaceConsistency.h"
#include "Handlers/ParamSpec.h"
#include "PinWrightSubsystem.h" // LogPinWrightSubsystem
#include "Utils/ActorUtils.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "NiagaraComponent.h"
#include "NiagaraSystem.h"
#include "UObject/UObjectIterator.h"

// This plugin builds with bUseUnity = true, so every name below is prefixed rather than relying on
// the anonymous namespace to keep it out of a sibling translation unit merged into the same blob.
namespace
{
    // ---- The check table --------------------------------------------------------------------
    //
    // One check today. The TABLE is the point: the next level-scope Niagara health check (a live
    // component against stale compile state) lands as a row here rather than as a second verb.
    enum class ENiagaraAuditCheck : uint8
    {
        DataInterfaceCounts = 0,
        Count
    };

    constexpr int32 NiagaraAuditCheckCount = static_cast<int32>(ENiagaraAuditCheck::Count);
    static_assert(NiagaraAuditCheckCount <= 32, "The check selection bitmask is a uint32.");

    struct FNiagaraAuditCheckInfo
    {
        ENiagaraAuditCheck Check;
        const TCHAR* Id;
        const TCHAR* Code;
        PinWrightAudit::ESeverity Severity;
        bool bDefaultOn;
        const TCHAR* Summary;
    };

    const TArray<FNiagaraAuditCheckInfo>& NiagaraAuditAllChecks()
    {
        static const TArray<FNiagaraAuditCheckInfo> Checks = {
            { ENiagaraAuditCheck::DataInterfaceCounts, TEXT("data_interface_counts"),
              ErrorCodes::ERR_NIAGARA_DATA_INTERFACE_MISMATCH, PinWrightAudit::ESeverity::Error, true,
              TEXT("Compares every script's compiled data-interface count against the resolved set "
                   "its execution context would be populated from. A disagreement asserts inside "
                   "the VectorVM on a concurrent worker the next time anything ticks the system, "
                   "which is an appError and takes the editor process with it. A system with no "
                   "resolved set to compare is UNRUNNABLE, never clean.") },
        };
        static_assert(NiagaraAuditCheckCount == 1,
            "NiagaraAuditAllChecks must list every ENiagaraAuditCheck in order.");
        return Checks;
    }

    // One distinct UNiagaraSystem reached through the live components that hold it.
    struct FNiagaraAuditSystemRow
    {
        FString SystemPath;
        FString SystemName;
        // Every live component holding this system, and the distinct actors that own them. A
        // preview-scene component has no owner, so ActorPaths is legitimately shorter than
        // ComponentPaths and the two are reported separately rather than zipped.
        TArray<FString> ComponentPaths;
        TArray<FString> ActorPaths;
        TArray<FString> ActorNames;
        PinWrightNiagara::EDataInterfaceConsistency Verdict =
            PinWrightNiagara::EDataInterfaceConsistency::Unverified;
        TArray<PinWrightNiagara::FDataInterfaceCountMismatch> Mismatches;
        // Only populated for detail:"orphans". bOrphansMeasured distinguishes "asked and found
        // none" from "never asked", which an empty array alone cannot.
        TArray<PinWrightNiagara::FOrphanResolvedDataInterface> Orphans;
        bool bOrphansMeasured = false;
    };

    // Same per-script shape niagara.add_emitter's NIAGARA_DATA_INTERFACE_MISMATCH payload,
    // NiagaraEdit::MakeMutationResult and niagara.list_orphan_data_interfaces already publish, so
    // the namespace has one vocabulary for one fact and a caller can pipe a row straight into the
    // repair verbs without translating.
    TSharedPtr<FJsonObject> NiagaraAuditMismatchJson(
        const PinWrightNiagara::FDataInterfaceCountMismatch& Mismatch)
    {
        TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetStringField(TEXT("scriptPath"), Mismatch.ScriptPath);
        Entry->SetStringField(TEXT("emitter"), Mismatch.EmitterName);
        Entry->SetNumberField(TEXT("compiledDataInterfaces"), Mismatch.CompiledCount);
        Entry->SetNumberField(TEXT("resolvedDataInterfaces"), Mismatch.ResolvedCount);
        return Entry;
    }

    // Likewise the orphan entry: identical to niagara.list_orphan_data_interfaces' rows.
    TSharedPtr<FJsonObject> NiagaraAuditOrphanJson(
        const PinWrightNiagara::FOrphanResolvedDataInterface& Orphan)
    {
        TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetStringField(TEXT("scriptPath"), Orphan.ScriptPath);
        Entry->SetStringField(TEXT("emitter"), Orphan.EmitterName);
        Entry->SetNumberField(TEXT("resolvedIndex"), Orphan.ResolvedIndex);
        Entry->SetStringField(TEXT("name"), Orphan.Name);
        Entry->SetStringField(TEXT("compileName"), Orphan.CompileName);
        Entry->SetStringField(TEXT("type"), Orphan.TypeName);
        Entry->SetStringField(TEXT("dataInterfaceClass"), Orphan.DataInterfaceClass);
        Entry->SetBoolField(TEXT("internal"), Orphan.bIsInternal);
        return Entry;
    }

    void NiagaraAuditWriteStrings(const TSharedPtr<FJsonObject>& Object, const TCHAR* Key,
                                  const TArray<FString>& Values)
    {
        TArray<TSharedPtr<FJsonValue>> Array;
        Array.Reserve(Values.Num());
        for (const FString& Value : Values)
        {
            Array.Add(MakeShared<FJsonValueString>(Value));
        }
        Object->SetArrayField(Key, Array);
    }
}

REGISTER_RPC_HANDLER("niagara.audit_level", "niagara",
    "Sweep every LIVE Niagara system in the level ONCE and report the ones that are fatal on their "
    "next tick: a system whose compiled data-interface count disagrees with its resolved set "
    "asserts inside the VectorVM on a concurrent worker and takes the whole editor process down. "
    "Run this before a capture or scrub session, or after a niagara.* write - scrubbing a sequence "
    "forces a world tick by design (OnSequencerEvaluated -> RequestRealTimeFrames(1) -> "
    "LEVELTICK_ViewportsOnly), and what ticks is EVERY system in the world, never just the "
    "sequence's bindings. Results are keyed by SYSTEM, not by actor: N actors can share one system "
    "and a preview-scene component has no actor at all, so each row names the system plus the "
    "components and owning actors that hold it. A system that has never resolved its data "
    "interfaces is reported UNRUNNABLE, never clean - it was not measured, and unmeasured is not a "
    "pass. Read-only: loads nothing, compiles nothing, saves nothing, dirties nothing. A content "
    "defect returns success with pass:false, never an RPC error.",
    RPC_PARAMS(
        RPC_PARAM_DEF("scope", "string",
            "Which components to walk. 'world' (default) is every UNiagaraComponent in the editor "
            "world - the set a scrub or a realtime viewport ticks. 'all' additionally covers "
            "asset-editor preview scenes, PIE, and components not yet attached to any world; "
            "preview components tick too, so a capture that opens an asset editor wants this.",
            "world"),
        RPC_PARAM_DEF("detail", "string",
            "'summary' (counts only), 'findings' (default: counts plus one row per system), or "
            "'orphans' (adds each system's orphan resolved data-interface entries, the same rows "
            "niagara.list_orphan_data_interfaces returns, so a repair can be planned from this one "
            "call). An unknown value is rejected rather than defaulted.",
            "findings"),
        RPC_PARAM_OPT("checks", "array",
            "Wire ids of the checks to run: data_interface_counts. All run by default. An unknown "
            "id is rejected with AUDIT_UNKNOWN_CHECK rather than silently skipped - a typo that ran "
            "nothing is indistinguishable from a level that passed."),
        RPC_PARAM_DEF("failOn", "string",
            "Severity that makes pass false: 'error' (default), 'any', or 'none'. It moves the "
            "severity bar and NOTHING else: pass is false whenever any system was unrunnable, "
            "whatever failOn says, because a system that could not be measured is not a system that "
            "passed.",
            "error")
    ))
{
    // ---- scope ----
    const FString ScopeToken = Ctx.GetString(TEXT("scope"), TEXT("world")).ToLower();
    bool bScopeAll = false;
    if (ScopeToken == TEXT("all"))
    {
        bScopeAll = true;
    }
    else if (ScopeToken != TEXT("world"))
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
            FString::Printf(TEXT("Unknown scope '%s'. Valid: world, all."), *ScopeToken));
        return true;
    }

    // ---- detail ----
    const FString DetailToken = Ctx.GetString(TEXT("detail"), TEXT("findings")).ToLower();
    bool bIncludeRows = true;
    bool bIncludeOrphans = false;
    if (DetailToken == TEXT("summary"))
    {
        bIncludeRows = false;
    }
    else if (DetailToken == TEXT("orphans"))
    {
        bIncludeOrphans = true;
    }
    else if (DetailToken != TEXT("findings"))
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
            FString::Printf(TEXT("Unknown detail '%s'. Valid: summary, findings, orphans."),
                *DetailToken));
        return true;
    }

    // ---- checks ----
    uint32 SelectedChecks = PinWrightAudit::DefaultCheckMask(NiagaraAuditAllChecks());
    if (const TArray<TSharedPtr<FJsonValue>>* CheckArray = Ctx.GetArray(TEXT("checks")))
    {
        SelectedChecks = 0;
        for (const TSharedPtr<FJsonValue>& Value : *CheckArray)
        {
            FString Id;
            ENiagaraAuditCheck Check;
            if (!Value.IsValid() || !Value->TryGetString(Id)
                || !PinWrightAudit::ParseCheckId(NiagaraAuditAllChecks(), Id, Check))
            {
                Ctx.SendError(ErrorCodes::ERR_AUDIT_UNKNOWN_CHECK,
                    FString::Printf(TEXT("Unknown check '%s'. Valid ids: %s."),
                        *Id, *PinWrightAudit::ValidCheckIdList(NiagaraAuditAllChecks())));
                return true;
            }
            SelectedChecks |= PinWrightAudit::CheckBit(Check);
        }
        if (SelectedChecks == 0)
        {
            Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
                TEXT("`checks` was empty, so nothing would be measured. Omit it to run every check."));
            return true;
        }
    }

    // ---- failOn ----
    const FString FailOnToken = Ctx.GetString(TEXT("failOn"), TEXT("error")).ToLower();
    PinWrightAudit::EFailOn FailOnMode;
    if (!PinWrightAudit::ParseFailOn(FailOnToken, FailOnMode))
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
            FString::Printf(TEXT("Unknown failOn '%s'. Valid: error, any, none."), *FailOnToken));
        return true;
    }

    // ---- the world the default scope means ----
    FString ResolvedWorldMode;
    UWorld* EditorWorld = McpActorUtils::ResolveQueryWorld(TEXT("editor"), ResolvedWorldMode);
    if (!bScopeAll && !EditorWorld)
    {
        Ctx.SendError(ErrorCodes::ERR_EDITOR_WORLD_NOT_AVAILABLE,
            TEXT("No editor world is available to sweep. Pass scope:\"all\" to audit every loaded "
                 "Niagara component regardless of world."));
        return true;
    }

    // ---- the walk ----
    //
    // TObjectIterator, not TActorIterator and not Sequencer's FindBoundObjects: the subject is a
    // system reached through a live component, and a component in an asset-editor preview scene
    // has no actor. Class default objects and archetypes are skipped - they hold an asset but
    // nothing ever ticks them, so counting them would put an unrunnable row on every Blueprint in
    // the project.
    const bool bRunDataInterfaceCheck =
        PinWrightAudit::HasCheck(SelectedChecks, ENiagaraAuditCheck::DataInterfaceCounts);

    TArray<FNiagaraAuditSystemRow> Rows;
    TMap<const UNiagaraSystem*, int32> RowBySystem;
    int32 ComponentsExamined = 0;
    int32 ComponentsWithoutAsset = 0;

    for (TObjectIterator<UNiagaraComponent> It; It; ++It)
    {
        UNiagaraComponent* Component = *It;
        if (!IsValid(Component) || Component->IsTemplate())
        {
            continue;
        }
        if (!bScopeAll && Component->GetWorld() != EditorWorld)
        {
            continue;
        }

        UNiagaraSystem* System = Component->GetAsset();
        if (!System)
        {
            // Counted rather than dropped: a component with no asset cannot arm this fault, but a
            // sweep that silently shrinks the set it examined is how a clean verdict gets reported
            // over subjects nobody saw.
            ++ComponentsWithoutAsset;
            continue;
        }

        ++ComponentsExamined;

        int32* ExistingIndex = RowBySystem.Find(System);
        if (!ExistingIndex)
        {
            // Measured ONCE per system, not once per component - the whole reason results are
            // keyed by system.
            FNiagaraAuditSystemRow NewRow;
            NewRow.SystemPath = System->GetPathName();
            NewRow.SystemName = System->GetName();
            if (bRunDataInterfaceCheck)
            {
                NewRow.Verdict =
                    PinWrightNiagara::CheckDataInterfaceCounts(*System, NewRow.Mismatches);
                if (bIncludeOrphans)
                {
                    PinWrightNiagara::FindOrphanResolvedDataInterfaces(*System, NewRow.Orphans);
                    NewRow.bOrphansMeasured = true;
                }
            }
            ExistingIndex = &RowBySystem.Add(System, Rows.Add(MoveTemp(NewRow)));
        }

        FNiagaraAuditSystemRow& Row = Rows[*ExistingIndex];
        Row.ComponentPaths.AddUnique(Component->GetPathName());
        if (const AActor* Owner = Component->GetOwner())
        {
            const FString ActorPath = Owner->GetPathName();
            if (!Row.ActorPaths.Contains(ActorPath))
            {
                Row.ActorPaths.Add(ActorPath);
                Row.ActorNames.Add(Owner->GetActorLabel());
            }
        }
    }

    // TObjectIterator order is an allocation detail. Sorting by system path makes two sweeps of an
    // unchanged level comparable.
    Rows.Sort([](const FNiagaraAuditSystemRow& A, const FNiagaraAuditSystemRow& B)
    {
        return A.SystemPath < B.SystemPath;
    });

    // ---- the verdict ----
    PinWrightAudit::FVerdict Verdict;
    int32 ConsistentCount = 0;
    int32 MismatchedCount = 0;
    int32 UnverifiedCount = 0;
    TArray<TSharedPtr<FJsonValue>> FindingRows;
    TArray<TSharedPtr<FJsonValue>> SystemRows;

    for (const FNiagaraAuditSystemRow& Row : Rows)
    {
        const TCHAR* Status = TEXT("clean");
        FString Message;
        if (!bRunDataInterfaceCheck)
        {
            // No selected check applies to this subject. Not the same as clean and not the same as
            // unrunnable: the check has no verdict here because nobody asked for it.
            Status = TEXT("not_applicable");
        }
        else if (Row.Verdict == PinWrightNiagara::EDataInterfaceConsistency::Mismatched)
        {
            ++MismatchedCount;
            ++Verdict.ErrorCount;
            Status = TEXT("flagged");
            Message = FString::Printf(
                TEXT("System '%s' has script(s) whose compiled data-interface count differs from the ")
                TEXT("resolved count: %s. Ticking it asserts inside the VectorVM on a worker thread and ")
                TEXT("kills the editor process, and a scrub ticks the whole world. Held by %d live ")
                TEXT("component(s) on %d actor(s). Run niagara.list_orphan_data_interfaces / ")
                TEXT("niagara.remove_orphan_data_interfaces on it, recompile it, or detach the named ")
                TEXT("components before the session."),
                *Row.SystemPath,
                *PinWrightNiagara::DescribeDataInterfaceMismatches(Row.Mismatches),
                Row.ComponentPaths.Num(),
                Row.ActorPaths.Num());
        }
        else if (Row.Verdict == PinWrightNiagara::EDataInterfaceConsistency::Unverified)
        {
            // NOT a clean row. Nothing could be compared, so an empty mismatch list - and an empty
            // orphan list beside it - is not evidence that this system is sound. The unrunnable
            // term is the one failOn cannot reach, which is exactly what makes it useful here.
            ++UnverifiedCount;
            ++Verdict.UnrunnableCount;
            Status = TEXT("unrunnable");
            Message = FString::Printf(
                TEXT("System '%s' has no resolved data-interface set to compare its compiled one ")
                TEXT("against, so the check that catches a system fatal on its next tick DID NOT RUN ")
                TEXT("on it. An empty result here is not evidence the system is sound. Run ")
                TEXT("niagara.compile on it, then re-run this audit for a verdict."),
                *Row.SystemPath);
        }
        else
        {
            ++ConsistentCount;
        }

        if (!Message.IsEmpty())
        {
            const FNiagaraAuditCheckInfo& Info = PinWrightAudit::CheckInfo(
                NiagaraAuditAllChecks(), ENiagaraAuditCheck::DataInterfaceCounts);
            const bool bUnrunnable =
                Row.Verdict == PinWrightNiagara::EDataInterfaceConsistency::Unverified;
            TSharedPtr<FJsonObject> Finding = MakeShared<FJsonObject>();
            Finding->SetStringField(TEXT("check"), Info.Id);
            Finding->SetStringField(TEXT("status"), PinWrightAudit::StatusToWire(
                bUnrunnable ? PinWrightAudit::EFindingStatus::Unrunnable
                            : PinWrightAudit::EFindingStatus::Flagged));
            Finding->SetStringField(TEXT("severity"), PinWrightAudit::SeverityToWire(Info.Severity));
            Finding->SetStringField(TEXT("code"), bUnrunnable
                ? ErrorCodes::ERR_NIAGARA_DATA_INTERFACE_UNVERIFIED
                : ErrorCodes::ERR_NIAGARA_DATA_INTERFACE_MISMATCH);
            Finding->SetStringField(TEXT("systemPath"), Row.SystemPath);
            Finding->SetStringField(TEXT("message"), Message);
            FindingRows.Add(MakeShared<FJsonValueObject>(Finding));
        }

        if (bIncludeRows)
        {
            TSharedPtr<FJsonObject> SystemJson = MakeShared<FJsonObject>();
            SystemJson->SetStringField(TEXT("systemPath"), Row.SystemPath);
            SystemJson->SetStringField(TEXT("systemName"), Row.SystemName);
            SystemJson->SetStringField(TEXT("status"), Status);
            SystemJson->SetStringField(TEXT("dataInterfaceCheck"),
                PinWrightNiagara::DataInterfaceConsistencyToString(Row.Verdict));
            SystemJson->SetNumberField(TEXT("componentCount"), Row.ComponentPaths.Num());
            NiagaraAuditWriteStrings(SystemJson, TEXT("components"), Row.ComponentPaths);
            NiagaraAuditWriteStrings(SystemJson, TEXT("actors"), Row.ActorPaths);
            NiagaraAuditWriteStrings(SystemJson, TEXT("actorNames"), Row.ActorNames);

            TArray<TSharedPtr<FJsonValue>> MismatchValues;
            MismatchValues.Reserve(Row.Mismatches.Num());
            for (const PinWrightNiagara::FDataInterfaceCountMismatch& Mismatch : Row.Mismatches)
            {
                MismatchValues.Add(MakeShared<FJsonValueObject>(NiagaraAuditMismatchJson(Mismatch)));
            }
            SystemJson->SetArrayField(TEXT("mismatchedScripts"), MismatchValues);

            if (Row.bOrphansMeasured)
            {
                TArray<TSharedPtr<FJsonValue>> OrphanValues;
                OrphanValues.Reserve(Row.Orphans.Num());
                for (const PinWrightNiagara::FOrphanResolvedDataInterface& Orphan : Row.Orphans)
                {
                    OrphanValues.Add(MakeShared<FJsonValueObject>(NiagaraAuditOrphanJson(Orphan)));
                }
                SystemJson->SetNumberField(TEXT("orphanCount"), Row.Orphans.Num());
                SystemJson->SetArrayField(TEXT("orphans"), OrphanValues);
            }
            SystemRows.Add(MakeShared<FJsonValueObject>(SystemJson));
        }
    }

    // The shared rule (Audit/AuditFramework.h): an unrunnable subject makes pass false whatever
    // failOn says. bTruncated stays false - the walk visits every component the iterator reaches
    // in one pass and the rows are uncapped, so this sweep always covers the set it reports on.
    const bool bPass = Verdict.DerivePass(FailOnMode);

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetBoolField(TEXT("success"), true);
    Resp->SetBoolField(TEXT("pass"), bPass);
    Resp->SetStringField(TEXT("failOn"), PinWrightAudit::FailOnToWire(FailOnMode));
    Resp->SetStringField(TEXT("passRule"),
        PinWrightAudit::PassRuleText(/*bIncludeTruncation=*/false,
            TEXT("A system whose data interfaces could not be compared is an unrunnable row, never "
                 "a clean one: an empty result on an unverified system is not evidence that it is "
                 "sound.")));
    Resp->SetStringField(TEXT("scope"), bScopeAll ? TEXT("all") : TEXT("world"));
    Resp->SetStringField(TEXT("detail"), DetailToken);
    Resp->SetStringField(TEXT("worldPath"), EditorWorld ? EditorWorld->GetPathName() : FString());

    // The buckets sum: consistent + mismatched + unverified == systems. A subject that falls out
    // of every bucket is exactly how a check silently stops running, and the sum is what makes
    // that detectable (docs/rpc-design.md section 18).
    TSharedPtr<FJsonObject> Summary = MakeShared<FJsonObject>();
    Summary->SetNumberField(TEXT("systems"), Rows.Num());
    Summary->SetNumberField(TEXT("componentsExamined"), ComponentsExamined);
    Summary->SetNumberField(TEXT("componentsWithoutAsset"), ComponentsWithoutAsset);
    Summary->SetNumberField(TEXT("consistent"), ConsistentCount);
    Summary->SetNumberField(TEXT("mismatched"), MismatchedCount);
    Summary->SetNumberField(TEXT("unverified"), UnverifiedCount);
    Summary->SetNumberField(TEXT("findings"), FindingRows.Num());
    Summary->SetNumberField(TEXT("errors"), Verdict.ErrorCount);
    Summary->SetNumberField(TEXT("warnings"), Verdict.WarningCount);
    Summary->SetNumberField(TEXT("unrunnable"), Verdict.UnrunnableCount);
    Summary->SetBoolField(TEXT("truncated"), Verdict.bTruncated);
    Resp->SetObjectField(TEXT("summary"), Summary);

    // Every check, selected or not, with the buckets it filled. A check that is not listed is how
    // a check silently stops running.
    TArray<TSharedPtr<FJsonValue>> CheckRows;
    for (const FNiagaraAuditCheckInfo& Info : NiagaraAuditAllChecks())
    {
        const bool bSelected = PinWrightAudit::HasCheck(SelectedChecks, Info.Check);
        TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
        Row->SetStringField(TEXT("id"), Info.Id);
        Row->SetStringField(TEXT("code"), Info.Code);
        Row->SetStringField(TEXT("severity"), PinWrightAudit::SeverityToWire(Info.Severity));
        Row->SetBoolField(TEXT("selected"), bSelected);
        Row->SetBoolField(TEXT("defaultOn"), Info.bDefaultOn);
        if (bSelected)
        {
            Row->SetNumberField(TEXT("subjects"), Rows.Num());
            Row->SetNumberField(TEXT("flagged"), MismatchedCount);
            Row->SetNumberField(TEXT("unrunnable"), UnverifiedCount);
            Row->SetNumberField(TEXT("clean"), ConsistentCount);
        }
        else
        {
            Row->SetStringField(TEXT("notSelectedReason"),
                TEXT("not selected: not named in `checks`"));
        }
        Row->SetStringField(TEXT("summary"), Info.Summary);
        CheckRows.Add(MakeShared<FJsonValueObject>(Row));
    }
    Resp->SetArrayField(TEXT("checks"), CheckRows);

    if (bIncludeRows)
    {
        Resp->SetArrayField(TEXT("systems"), SystemRows);
        Resp->SetArrayField(TEXT("findings"), FindingRows);
    }

    // Stated limits, not oversights. A green here means less than it looks like it means, and the
    // verb says so in its own output rather than only in the wiki.
    TArray<TSharedPtr<FJsonValue>> Limits;
    Limits.Add(MakeShared<FJsonValueString>(
        TEXT("This measures ONE invariant - compiled versus resolved data-interface counts. A system "
             "that passes here can still be fatal for another reason.")));
    Limits.Add(MakeShared<FJsonValueString>(
        TEXT("Discovery is the loaded object graph, not the level file. A World Partition cell that "
             "is not loaded holds systems this sweep cannot see, and one streamed in after the sweep "
             "is not covered by it.")));
    Limits.Add(MakeShared<FJsonValueString>(
        TEXT("This is a PRE-FLIGHT, not a gate. In a shared editor another session can arm the fault "
             "between this sweep and the next tick, so run it immediately before the session and "
             "again after any niagara.* write.")));
    Limits.Add(MakeShared<FJsonValueString>(
        TEXT("scope:\"world\" covers what a scrub or a realtime viewport ticks. It does NOT cover "
             "asset-editor preview scenes, which tick too - pass scope:\"all\" when an asset editor "
             "is open.")));
    Resp->SetArrayField(TEXT("limits"), Limits);

    UE_LOG(LogPinWrightSubsystem, Display,
        TEXT("niagara.audit_level: scope=%s, %d component(s) over %d system(s) - %d consistent, "
             "%d mismatched, %d unverified, pass=%s"),
        bScopeAll ? TEXT("all") : TEXT("world"), ComponentsExamined, Rows.Num(),
        ConsistentCount, MismatchedCount, UnverifiedCount, bPass ? TEXT("true") : TEXT("false"));

    Ctx.SendSuccess(Resp);
    return true;
}
