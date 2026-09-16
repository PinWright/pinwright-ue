// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/Niagara/NiagaraCompileVerdict.h"
#include "Handlers/Niagara/NiagaraComponentActivation.h"
#include "Handlers/Niagara/NiagaraDataInterfaceConsistency.h"
#include "Handlers/Niagara/NiagaraDumpBuilder.h"
#include "Handlers/Niagara/NiagaraGraphResetUtils.h"
#include "Handlers/Niagara/NiagaraSubUVAtlasCheck.h"

#include "NiagaraEmitter.h"
#include "NiagaraEmitterHandle.h"
#include "NiagaraScript.h"
#include "NiagaraSystem.h"

class UNiagaraEmitter;
class UNiagaraScript;
class UNiagaraSystem;

namespace
{
    void AddNiagaraSystemInspectAspects(TSharedPtr<FJsonObject>& Result, const UNiagaraSystem* System, bool bIncludeProperties, bool bIncludeStack, bool bIncludeGraphs, bool bIncludeCompile, bool bParametersOnly, const FString& ParameterName)
    {
        // parametersOnly is a projection: emit only the parameter list (narrowed by
        // parameterName when set) and drop system/emitter/renderer props, stack, graphs,
        // and compile, so a single-parameter readback stays inline instead of spilling.
        if (bParametersOnly)
        {
            Result->SetObjectField(TEXT("parameters"), NiagaraDumpBuilder::BuildParametersJson(System, ParameterName));
            return;
        }
        if (bIncludeProperties)
        {
            Result->SetObjectField(TEXT("system"), NiagaraDumpBuilder::BuildSystemJson(System));
            Result->SetObjectField(TEXT("emitters"), NiagaraDumpBuilder::BuildEmittersJson(System));
            Result->SetObjectField(TEXT("parameters"), NiagaraDumpBuilder::BuildParametersJson(System, ParameterName));
        }
        if (bIncludeStack)
        {
            Result->SetObjectField(TEXT("stack"), NiagaraDumpBuilder::BuildStackJson(System));
        }
        if (bIncludeGraphs)
        {
            Result->SetObjectField(TEXT("graphs"), NiagaraDumpBuilder::BuildGraphsJson(System));
        }
        if (bIncludeCompile)
        {
            Result->SetObjectField(TEXT("compile"), NiagaraDumpBuilder::BuildCompileDiagnosticsJson(System));
        }
    }

    void AddNiagaraEmitterInspectAspects(TSharedPtr<FJsonObject>& Result, const UNiagaraEmitter* Emitter, bool bIncludeProperties, bool bIncludeStack, bool bIncludeGraphs, bool bIncludeCompile, bool bParametersOnly, const FString& ParameterName)
    {
        // see AddNiagaraSystemInspectAspects for the parametersOnly projection rationale
        if (bParametersOnly)
        {
            Result->SetObjectField(TEXT("parameters"), NiagaraDumpBuilder::BuildEmitterParametersJson(Emitter, ParameterName));
            return;
        }
        if (bIncludeProperties)
        {
            Result->SetObjectField(TEXT("system"), NiagaraDumpBuilder::BuildEmitterAssetJson(Emitter));
            Result->SetObjectField(TEXT("emitters"), NiagaraDumpBuilder::BuildEmitterAssetJson(Emitter));
            Result->SetObjectField(TEXT("parameters"), NiagaraDumpBuilder::BuildEmitterParametersJson(Emitter, ParameterName));
        }
        if (bIncludeStack)
        {
            Result->SetObjectField(TEXT("stack"), NiagaraDumpBuilder::BuildEmitterStackJson(Emitter));
        }
        if (bIncludeGraphs)
        {
            Result->SetObjectField(TEXT("graphs"), NiagaraDumpBuilder::BuildEmitterGraphsJson(Emitter));
        }
        if (bIncludeCompile)
        {
            Result->SetObjectField(TEXT("compile"), NiagaraDumpBuilder::BuildEmitterCompileDiagnosticsJson(Emitter));
        }
    }

    void AddNiagaraScriptInspectAspects(TSharedPtr<FJsonObject>& Result, const UNiagaraScript* Script, bool bIncludeGraphs, bool bIncludeCompile)
    {
        if (bIncludeGraphs)
        {
            Result->SetObjectField(TEXT("graphs"), NiagaraDumpBuilder::BuildScriptGraphsJson(Script));
        }
        if (bIncludeCompile)
        {
            Result->SetObjectField(TEXT("compile"), NiagaraDumpBuilder::BuildScriptCompileDiagnosticsJson(Script));
        }
    }

    TSharedPtr<FJsonObject> MakeValidationIssue(const FString& Severity, const FString& Code, const FString& Message, const FString& EmitterName = FString())
    {
        TSharedPtr<FJsonObject> Issue = MakeShared<FJsonObject>();
        Issue->SetStringField(TEXT("severity"), Severity);
        Issue->SetStringField(TEXT("code"), Code);
        Issue->SetStringField(TEXT("message"), Message);
        if (!EmitterName.IsEmpty())
        {
            Issue->SetStringField(TEXT("emitter"), EmitterName);
        }
        return Issue;
    }

    FString NormalizeValidationSeverity(const FString& Severity, const FString& Code, const FString& Level)
    {
        if (Level.Equals(TEXT("strict"), ESearchCase::IgnoreCase)
            && (Code.Equals(TEXT("NO_EMITTERS"), ESearchCase::IgnoreCase)
                || Code.Equals(TEXT("DISABLED_EMITTER"), ESearchCase::IgnoreCase)
                || Code.Equals(TEXT("NO_RENDERERS"), ESearchCase::IgnoreCase)
                || Code.Equals(TEXT("NIAGARA_NO_ACTIVE_COMPONENT"), ESearchCase::IgnoreCase)))
        {
            return TEXT("error");
        }
        return Severity.Equals(TEXT("error"), ESearchCase::IgnoreCase) ? TEXT("error") : TEXT("warning");
    }

    void AddValidationIssue(
        TArray<TSharedPtr<FJsonValue>>& Issues,
        TArray<TSharedPtr<FJsonValue>>& Errors,
        TArray<TSharedPtr<FJsonValue>>& Warnings,
        const TSharedPtr<FJsonObject>& Issue)
    {
        Issues.Add(MakeShared<FJsonValueObject>(Issue));
        FString Severity;
        Issue->TryGetStringField(TEXT("severity"), Severity);
        if (Severity.Equals(TEXT("error"), ESearchCase::IgnoreCase))
        {
            Errors.Add(MakeShared<FJsonValueObject>(Issue));
        }
        else
        {
            Warnings.Add(MakeShared<FJsonValueObject>(Issue));
        }
    }

    void AddCompileIssues(
        const TSharedPtr<FJsonObject>& Compile,
        const FString& Level,
        TArray<TSharedPtr<FJsonValue>>& Issues,
        TArray<TSharedPtr<FJsonValue>>& Errors,
        TArray<TSharedPtr<FJsonValue>>& Warnings)
    {
        const TArray<TSharedPtr<FJsonValue>>* CompileIssues = nullptr;
        if (!Compile.IsValid() || !Compile->TryGetArrayField(TEXT("issues"), CompileIssues) || !CompileIssues)
        {
            return;
        }

        for (const TSharedPtr<FJsonValue>& IssueValue : *CompileIssues)
        {
            if (!IssueValue.IsValid() || IssueValue->Type != EJson::Object)
            {
                continue;
            }

            const TSharedPtr<FJsonObject> SourceIssue = IssueValue->AsObject();
            FString Severity;
            FString Code;
            FString Message;
            FString EmitterName;
            SourceIssue->TryGetStringField(TEXT("severity"), Severity);
            SourceIssue->TryGetStringField(TEXT("code"), Code);
            SourceIssue->TryGetStringField(TEXT("message"), Message);
            SourceIssue->TryGetStringField(TEXT("emitter"), EmitterName);

            AddValidationIssue(
                Issues,
                Errors,
                Warnings,
                MakeValidationIssue(NormalizeValidationSeverity(Severity, Code, Level), Code, Message, EmitterName));
        }
    }

    // Everything AddCompileIssues promotes comes out of the compile block's `issues` array.
    // Nothing ever wrote a script's COMPILE STATUS into that array, so a system whose particle
    // scripts were all at NCS_Error published `compile.valid: false` and ten `NCS_Error` script
    // entries beside a top-level `valid: true` with an empty `errors` - and the two systems the
    // verb green-lit were the only two in the package that refused to activate
    // (B-niagara-validate-green-while-scripts-ncs-error). This is the promotion that was missing:
    // the verdict is computed from the same block the response carries, so the nested detail and
    // the top-level answer cannot disagree again.
    //
    // Reported as an error at every level rather than a strict-only escalation, for the same
    // reason EMITTER_NOT_IN_SYSTEM_GRAPH and NIAGARA_DATA_INTERFACE_MISMATCH are: a script the
    // compiler rejected is not acceptable under any reading of the asset.
    //
    // `compile.valid` (UNiagaraSystem::IsValid) is deliberately NOT promoted alongside it. That
    // flag is false for a system with zero emitter handles as well as for one whose scripts
    // failed, so promoting it would turn the documented `basic`-level NO_EMITTERS warning into an
    // unconditional error and fail validate on every freshly-created system. The two states it
    // conflates are each already reported: NO_EMITTERS by the dumper, script failures here.
    void AddCompileStatusIssues(
        const TSharedPtr<FJsonObject>& Compile,
        const TSharedPtr<FJsonObject>& Result,
        TArray<TSharedPtr<FJsonValue>>& Issues,
        TArray<TSharedPtr<FJsonValue>>& Errors,
        TArray<TSharedPtr<FJsonValue>>& Warnings)
    {
        if (!Result.IsValid())
        {
            return;
        }

        const PinWrightNiagara::FCompileVerdict Verdict = PinWrightNiagara::ReadCompileVerdict(Compile);
        // Published on every verdict, "passed" included, so a caller can tell a checked pass from
        // an asset nothing has compiled this session - which is not a pass and must never read as
        // one. `unverified` raises nothing here: the dumper already files it as
        // COMPILE_STATE_UNINITIALIZED, and a second issue for one fact is noise.
        Result->SetStringField(TEXT("scriptCompileCheck"),
            PinWrightNiagara::ScriptCompileCheckToString(Verdict.Check));

        for (const PinWrightNiagara::FScriptCompileFailure& Failure : Verdict.FailedScripts)
        {
            TSharedPtr<FJsonObject> Issue = MakeValidationIssue(
                TEXT("error"),
                TEXT("NIAGARA_SCRIPT_COMPILE_ERROR"),
                PinWrightNiagara::DescribeScriptCompileFailure(Failure),
                Failure.OwnerKind.Equals(TEXT("emitter"), ESearchCase::IgnoreCase) ? Failure.OwnerName : FString());
            Issue->SetStringField(TEXT("scriptUsage"), Failure.ScriptUsage);
            Issue->SetStringField(TEXT("scriptPath"), Failure.ScriptPath);
            Issue->SetStringField(TEXT("compileStatus"), Failure.CompileStatus);
            TArray<TSharedPtr<FJsonValue>> ErrorMessages;
            for (const FString& Message : Failure.Errors)
            {
                ErrorMessages.Add(MakeShared<FJsonValueString>(Message));
            }
            Issue->SetArrayField(TEXT("compileErrors"), ErrorMessages);
            AddValidationIssue(Issues, Errors, Warnings, Issue);
        }

        if (!Verdict.bPendingCompileKnown)
        {
            return;
        }

        // Only measurable on a system; an emitter or script asset has no compile queue of its own.
        Result->SetBoolField(TEXT("pendingCompile"), Verdict.bPendingCompile);
        if (!Verdict.bPendingCompile)
        {
            return;
        }

        // An error, not a warning, and validate does not wait it out: every status in this
        // response describes the PREVIOUS compile, so the verdict is about an asset that no longer
        // exists in the form measured. Waiting belongs to niagara.compile, which blocks up to its
        // validated 60 s maximum and reports completion or timeout; duplicating that wait in a read
        // verb would make validate block synchronously with nothing to say about it.
        AddValidationIssue(
            Issues,
            Errors,
            Warnings,
            MakeValidationIssue(
                TEXT("error"),
                TEXT("NIAGARA_COMPILE_PENDING"),
                TEXT("A compile is still in flight on this system, so every compile status in this response describes the previous compile "
                     "and 'valid' cannot cover the asset as it will be. Run niagara.compile {wait:true} to let it land, then validate again.")));
    }

    void AddDisabledEmitterIssues(
        const UNiagaraSystem* System,
        const FString& Level,
        TArray<TSharedPtr<FJsonValue>>& Issues,
        TArray<TSharedPtr<FJsonValue>>& Errors,
        TArray<TSharedPtr<FJsonValue>>& Warnings)
    {
        if (!System)
        {
            return;
        }

        for (const FNiagaraEmitterHandle& Handle : System->GetEmitterHandles())
        {
            if (!Handle.GetIsEnabled())
            {
                const FString Severity = NormalizeValidationSeverity(TEXT("warning"), TEXT("DISABLED_EMITTER"), Level);
                AddValidationIssue(
                    Issues,
                    Errors,
                    Warnings,
                    MakeValidationIssue(
                        Severity,
                        TEXT("DISABLED_EMITTER"),
                        FString::Printf(TEXT("Emitter '%s' is disabled."), *Handle.GetName().ToString()),
                        Handle.GetName().ToString()));
            }
        }
    }

    // An emitter handle with no UNiagaraNodeEmitter on the system graph's parameter-map chain is
    // never invoked: its scripts compile, it carries a renderer, it is listed by every readback,
    // and it spawns nothing. Compile diagnostics cannot see it (there is no compile error), and
    // neither can any of the three structural codes above, which is what let
    // B-niagara-authored-emitter-forces-inert burn two sessions on emitters that were never
    // running. Reported as an error at every level rather than a strict-only escalation: there is
    // no reading of an asset under which an emitter that cannot run is acceptable.
    void AddUninvokedEmitterIssues(
        const UNiagaraSystem* System,
        TArray<TSharedPtr<FJsonValue>>& Issues,
        TArray<TSharedPtr<FJsonValue>>& Errors,
        TArray<TSharedPtr<FJsonValue>>& Warnings)
    {
        if (!System)
        {
            return;
        }

        TArray<FName> UninvokedHandleNames;
        if (!PinWrightNiagara::FindUninvokedEmitterHandles(*System, UninvokedHandleNames))
        {
            // No verdict was possible. Say so rather than staying silent, which would read as
            // "every emitter is invoked".
            if (System->GetEmitterHandles().Num() > 0)
            {
                AddValidationIssue(
                    Issues,
                    Errors,
                    Warnings,
                    MakeValidationIssue(
                        TEXT("error"),
                        TEXT("EMITTER_NOT_IN_SYSTEM_GRAPH"),
                        TEXT("The system's SystemSpawn/SystemUpdate graph could not be read, so no emitter in this system can be shown to be invoked.")));
            }
            return;
        }

        for (const FName& HandleName : UninvokedHandleNames)
        {
            AddValidationIssue(
                Issues,
                Errors,
                Warnings,
                MakeValidationIssue(
                    TEXT("error"),
                    TEXT("EMITTER_NOT_IN_SYSTEM_GRAPH"),
                    FString::Printf(
                        TEXT("Emitter '%s' has no emitter node in the system graph, so its spawn and update scripts are never called and it produces no particles. ")
                        TEXT("Run niagara.add_emitter or niagara.remove_emitter on this system to rebuild the emitter nodes, or rebuild the system from a complete stock system with asset.duplicate."),
                        *HandleName.ToString()),
                    HandleName.ToString()));
        }
    }

    // An emitter handle that INHERITS from an emitter asset holds a copy of it plus
    // VersionedParentAtLastMerge - the parent as it stood the last time the two were reconciled.
    // Editing the parent asset does not touch the system: the merge runs on load, or when
    // niagara.refresh_emitter asks for it. Until then the system compiles clean, saves clean,
    // strict-validates clean and runs the OLD emitter, which is exactly the signature of
    // B-niagara-add-emitter-snapshots-emitter-silently - three encounters, the last of which
    // shipped a smoke effect with no size-over-life because the ramp lived only in the emitter
    // asset while every reader who checked that asset saw a correct ramp.
    //
    // A warning at every level, not an error and not a strict-only escalation. The system is
    // runnable and its content is a deliberate earlier state of the parent until someone asks for
    // the newer one, so it is not in the "cannot run at all" class that EMITTER_NOT_IN_SYSTEM_GRAPH
    // and NIAGARA_DATA_INTERFACE_MISMATCH occupy. What was missing was any signal at all.
    //
    // A SNAPSHOT handle (no parent) is deliberately silent here: it is not stale, it is unlinked,
    // and there is nothing to compare it against. niagara.inspect's
    // emitters[].versionedEmitterData.parent is where that distinction is readable.
    void AddStaleParentEmitterIssues(
        const UNiagaraSystem* System,
        TArray<TSharedPtr<FJsonValue>>& Issues,
        TArray<TSharedPtr<FJsonValue>>& Errors,
        TArray<TSharedPtr<FJsonValue>>& Warnings)
    {
        if (!System)
        {
            return;
        }

        for (const FNiagaraEmitterHandle& Handle : System->GetEmitterHandles())
        {
            const FVersionedNiagaraEmitterData* Data = Handle.GetEmitterData();
            if (!Data)
            {
                continue;
            }
            const UNiagaraEmitter* Parent = Data->GetParent().Emitter;
            if (!Parent || Data->IsSynchronizedWithParent())
            {
                continue;
            }

            AddValidationIssue(
                Issues,
                Errors,
                Warnings,
                MakeValidationIssue(
                    TEXT("warning"),
                    TEXT("EMITTER_PARENT_STALE"),
                    FString::Printf(
                        TEXT("Emitter '%s' inherits from '%s', which has changed since this system last merged from it, ")
                        TEXT("so the system is running an older version of that emitter than the asset now holds. ")
                        TEXT("Run niagara.refresh_emitter on this system to merge the changes in (it keeps the system's own ")
                        TEXT("per-handle overrides), then compile and save."),
                        *Handle.GetName().ToString(),
                        *Parent->GetPathName()),
                    Handle.GetName().ToString()));
        }
    }

    // A UNiagaraSystem whose compiled data-interface count disagrees with its resolved one is
    // fatal on its NEXT tick: the bytecode indexes a data set the execution context never
    // allocated and VectorVM asserts on a concurrent worker
    // (`DataSetIdx < ExecCtx->DataSets.Num()`). That assert is an appError, so it takes the whole
    // editor process down, minutes after and unrelated to the write that caused it.
    //
    // niagara.add_emitter / niagara.remove_emitter refuse to save a system in that state, but a
    // system that reached it before that gate shipped - or through a verb that does not gate - is
    // still on disk, and validate is where an author looks to ask whether an asset is sound.
    //
    // Reported as an error at every level rather than a strict-only escalation, for the same
    // reason EMITTER_NOT_IN_SYSTEM_GRAPH is: there is no reading of an asset under which a system
    // that cannot be ticked is acceptable.
    //
    // Unverified is a warning: neither an error nor silence. Nothing could be compared - usually
    // because the system has not been compiled in this session - so an empty mismatch list is not
    // evidence there are none, and staying silent would read as "checked and clean". It is not an
    // error because having no resolved set is the normal state of an asset nothing has ticked yet,
    // and erroring there would fail validate on sound systems.
    void AddDataInterfaceConsistencyIssues(
        const UNiagaraSystem* System,
        const TSharedPtr<FJsonObject>& Result,
        TArray<TSharedPtr<FJsonValue>>& Issues,
        TArray<TSharedPtr<FJsonValue>>& Errors,
        TArray<TSharedPtr<FJsonValue>>& Warnings)
    {
        if (!System || !Result.IsValid())
        {
            return;
        }

        TArray<PinWrightNiagara::FDataInterfaceCountMismatch> Mismatches;
        const PinWrightNiagara::EDataInterfaceConsistency Consistency =
            PinWrightNiagara::CheckDataInterfaceCounts(*System, Mismatches);
        // Published on every verdict, "consistent" included, so a caller can tell a verified pass
        // from one this check was unable to make.
        Result->SetStringField(TEXT("dataInterfaceCheck"),
            PinWrightNiagara::DataInterfaceConsistencyToString(Consistency));

        if (Consistency == PinWrightNiagara::EDataInterfaceConsistency::Unverified)
        {
            AddValidationIssue(
                Issues,
                Errors,
                Warnings,
                MakeValidationIssue(
                    TEXT("warning"),
                    TEXT("NIAGARA_DATA_INTERFACE_UNVERIFIED"),
                    TEXT("This system has no resolved data-interface set to compare its compiled one against, so the check that ")
                    TEXT("catches a system fatal on its next tick did not run and 'valid' does not cover it. ")
                    TEXT("Run niagara.compile on the system and validate again to get a verdict.")));
            return;
        }

        for (const PinWrightNiagara::FDataInterfaceCountMismatch& Mismatch : Mismatches)
        {
            AddValidationIssue(
                Issues,
                Errors,
                Warnings,
                MakeValidationIssue(
                    TEXT("error"),
                    TEXT("NIAGARA_DATA_INTERFACE_MISMATCH"),
                    FString::Printf(
                        TEXT("Script '%s' was compiled against %d data interface(s) but resolves %d. ")
                        TEXT("Ticking this system asserts inside the VectorVM on a worker thread and kills the editor process. ")
                        TEXT("Run niagara.list_orphan_data_interfaces to see the resolved entries the bytecode no longer references, ")
                        TEXT("niagara.remove_orphan_data_interfaces to drop them, or recompile the system."),
                        *Mismatch.ScriptPath,
                        Mismatch.CompiledCount,
                        Mismatch.ResolvedCount),
                    Mismatch.EmitterName));
        }
    }

    // Publishes the survey onto Result and returns its verdict, leaving OutPlaced holding
    // what was found. Shared by validate and inspect so the two cannot describe the same
    // level differently: inspect is the verb an agent reaches for to answer "what is this
    // thing", and sending it to validate for "is anything running it" splits one question
    // across two calls. The survey costs one TActorIterator pass over the loaded actors of
    // the open level, which is far below what inspect already spends serializing every
    // graph node and pin of a real system.
    PinWrightNiagara::EComponentActivation PublishComponentActivation(
        const UNiagaraSystem& System,
        const TSharedPtr<FJsonObject>& Result,
        TArray<PinWrightNiagara::FPlacedNiagaraComponent>& OutPlaced)
    {
        const PinWrightNiagara::EComponentActivation Activation =
            PinWrightNiagara::SurveyPlacedComponents(System, OutPlaced);
        Result->SetStringField(TEXT("componentActivation"),
            PinWrightNiagara::ComponentActivationToString(Activation));

        if (Activation == PinWrightNiagara::EComponentActivation::Unverified)
        {
            // No count and no list: publishing zero here would read as "the level places none",
            // which is the fabricated pass this whole check exists to stop.
            return Activation;
        }

        // The list is capped: a common ambient system is placed hundreds of times over and the
        // whole array would spill the response for no extra information. Compare the array's
        // length against componentCount to see whether it was truncated.
        constexpr int32 MaxListedComponents = 16;
        TArray<TSharedPtr<FJsonValue>> ComponentsJson;
        for (const PinWrightNiagara::FPlacedNiagaraComponent& Placement : OutPlaced)
        {
            if (ComponentsJson.Num() >= MaxListedComponents)
            {
                break;
            }
            TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
            Entry->SetStringField(TEXT("actor"), Placement.ActorLabel);
            Entry->SetStringField(TEXT("component"), Placement.ComponentName);
            Entry->SetStringField(TEXT("componentPath"), Placement.ComponentPath);
            Entry->SetBoolField(TEXT("isActive"), Placement.bIsActive);
            Entry->SetBoolField(TEXT("autoActivate"), Placement.bAutoActivate);
            ComponentsJson.Add(MakeShared<FJsonValueObject>(Entry));
        }
        Result->SetNumberField(TEXT("componentCount"), OutPlaced.Num());
        Result->SetArrayField(TEXT("components"), ComponentsJson);
        return Activation;
    }

    // Everything above validates the ASSET. None of it can say whether anything in a level is
    // running that asset: that is a property of the placed UNiagaraComponent, not of the system.
    // So a system whose placed components were all inactive returned `valid: true` with zero
    // errors and zero warnings while the level rendered nothing, and the only signal that
    // disagreed was a raw reflected `object.call_function IsActive` - a first-class readback
    // being missing is exactly what reaching for that call means
    // (B-niagara-validate-green-while-component-inactive: three shipped systems dark for a day,
    // every verb green, one overridden bAutoActivate between the working and the broken set).
    //
    // The verdict is published on every system result, "no_components" included, so a caller can
    // tell a survey that found running content from one that found nothing to look at, and both
    // from one that could not look.
    //
    // Layered warning-at-basic / error-at-strict like the three structural codes, and not the
    // every-level error EMITTER_NOT_IN_SYSTEM_GRAPH is: a system with no active placed component
    // is entirely legitimate when gameplay spawns or activates it, so erroring at `basic` would
    // fail validate on sound content. `strict` already means "I expect this asset to be
    // render-complete", and under that reading a level that runs none of it is the defect it was
    // reported as.
    //
    // NoComponents raises nothing. The survey only reaches the loaded actors of the open level,
    // so "found none" is not "there are none" - an unloaded World Partition cell or a level that
    // is simply not open both look identical to it - and warning there would fire on every system
    // validated without its level open, which is most of them.
    void AddComponentActivationIssues(
        const UNiagaraSystem* System,
        const FString& Level,
        const TSharedPtr<FJsonObject>& Result,
        TArray<TSharedPtr<FJsonValue>>& Issues,
        TArray<TSharedPtr<FJsonValue>>& Errors,
        TArray<TSharedPtr<FJsonValue>>& Warnings)
    {
        if (!System || !Result.IsValid())
        {
            return;
        }

        TArray<PinWrightNiagara::FPlacedNiagaraComponent> Placed;
        const PinWrightNiagara::EComponentActivation Activation =
            PublishComponentActivation(*System, Result, Placed);

        if (Activation == PinWrightNiagara::EComponentActivation::Unverified)
        {
            AddValidationIssue(
                Issues,
                Errors,
                Warnings,
                MakeValidationIssue(
                    TEXT("warning"),
                    TEXT("NIAGARA_COMPONENT_ACTIVATION_UNVERIFIED"),
                    TEXT("There is no editor world to read, so whether anything in a level is running this system could not be ")
                    TEXT("surveyed and 'valid' does not cover it. Open the level the system is placed in and validate again.")));
            return;
        }

        if (Activation != PinWrightNiagara::EComponentActivation::NoneActive)
        {
            return;
        }

        int32 AutoActivateOffCount = 0;
        for (const PinWrightNiagara::FPlacedNiagaraComponent& Placement : Placed)
        {
            if (!Placement.bAutoActivate)
            {
                ++AutoActivateOffCount;
            }
        }

        const FString AutoActivateNote = AutoActivateOffCount > 0
            ? FString::Printf(
                TEXT("%d of them have bAutoActivate off - a level override that outlives the session and is saved into the .umap. "),
                AutoActivateOffCount)
            : FString();
        AddValidationIssue(
            Issues,
            Errors,
            Warnings,
            MakeValidationIssue(
                NormalizeValidationSeverity(TEXT("warning"), TEXT("NIAGARA_NO_ACTIVE_COMPONENT"), Level),
                TEXT("NIAGARA_NO_ACTIVE_COMPONENT"),
                FString::Printf(
                    TEXT("The open level places %d component(s) using this system and not one of them is active, so the level ")
                    TEXT("renders nothing from it however sound the asset is. %sSee the 'components' block for the actors, then ")
                    TEXT("restore bAutoActivate with actor.set_component_properties or start one with effect.activate_niagara. ")
                    TEXT("Expected instead of a defect when gameplay is what spawns or activates this system."),
                    Placed.Num(),
                    *AutoActivateNote)));
    }

    // A sprite/mesh renderer's SubImageSize declares the frame grid the vertex factory slices the
    // assigned texture into, and nothing else in this response - or in niagara.inspect - joins that
    // number to the texture actually sampled. So a renderer set to 8x8 over a 6x6 atlas is reported
    // healthy by every read there is: its scripts compile, its emitter is invoked, its material is
    // valid. NiagaraSpriteVertexFactory.ush remaps TexCoord0 into (SubImageCol + u,
    // SubImageRow + v) * SubImageSize.zw unconditionally, so every sprite samples a window that
    // straddles cell boundaries and shows inter-cell gutter with a clipped fragment shoved against
    // one edge, never a centred frame - and the SubUVAnimation module compounds it by taking its
    // frame count from the same wrong property.
    //
    // Reported at both levels, not as a strict-only escalation: this renders wrong under any
    // reading of the asset, so there is no level at which it is acceptable.
    //
    // ONLY the gutter-period measurement raises an error, because it is the only signal that
    // reads the atlas's real cell size. The trailing-empty measurement, the name heuristic and
    // the divisibility test all raise a warning: each of them fires on shapes that are entirely
    // legitimate (an 8x8 sheet holding 56 authored frames, an alpha-faded flipbook whose final
    // row is fully transparent, a deliberately non-square cell), so an error there would be an
    // over-report on sound content.
    //
    // The verdict is published on every system result, `not_applicable` included, so a caller can
    // tell a checked pass from a system with no SubUV renderer to check.
    void AddSubUVAtlasIssues(
        const UNiagaraSystem* System,
        const TSharedPtr<FJsonObject>& Result,
        TArray<TSharedPtr<FJsonValue>>& Issues,
        TArray<TSharedPtr<FJsonValue>>& Errors,
        TArray<TSharedPtr<FJsonValue>>& Warnings)
    {
        if (!System || !Result.IsValid())
        {
            return;
        }

        TArray<PinWrightNiagara::FSubUVRendererAtlasFinding> Findings;
        const PinWrightNiagara::ESubUVAtlasVerdict Verdict =
            PinWrightNiagara::EvaluateSubUVAtlases(*System, Findings);
        Result->SetStringField(TEXT("subUVAtlasCheck"),
            PinWrightNiagara::SubUVAtlasVerdictToString(Verdict));

        for (const PinWrightNiagara::FSubUVRendererAtlasFinding& Finding : Findings)
        {
            if (Finding.IsMismatch())
            {
                // Severity tracks the strength of the evidence, not the fault: only a measured
                // cell period is an error; the trailing-empty, name and divisibility signals are
                // warnings about the same fault, and the message says which produced it.
                TSharedPtr<FJsonObject> Issue = MakeValidationIssue(
                    Finding.HasDetectedGridMismatch() ? TEXT("error") : TEXT("warning"),
                    TEXT("NIAGARA_SUBUV_ATLAS_SIZE_MISMATCH"),
                    PinWrightNiagara::DescribeSubUVAtlasFinding(Finding),
                    Finding.EmitterName);
                Issue->SetNumberField(TEXT("rendererIndex"), Finding.RendererIndex);
                Issue->SetStringField(TEXT("texturePath"), Finding.TexturePath);
                Issue->SetNumberField(TEXT("declaredColumns"), Finding.DeclaredSubImageSize.X);
                Issue->SetNumberField(TEXT("declaredRows"), Finding.DeclaredSubImageSize.Y);
                Issue->SetNumberField(TEXT("detectedColumns"), Finding.DetectedGrid.X);
                Issue->SetNumberField(TEXT("detectedRows"), Finding.DetectedGrid.Y);
                Issue->SetNumberField(TEXT("populatedColumns"), Finding.PopulatedGrid.X);
                Issue->SetNumberField(TEXT("populatedRows"), Finding.PopulatedGrid.Y);
                Issue->SetNumberField(TEXT("emptyTileCount"), Finding.EmptyTileCount);
                AddValidationIssue(Issues, Errors, Warnings, Issue);
            }

            // Not an `else`: a renderer whose name or dimensions disagree AND whose pixels could
            // not be measured carries both facts, and dropping the second one would report a
            // heuristic finding as if the atlas had been read.
            if (!Finding.UnverifiedReason.IsEmpty())
            {
                TSharedPtr<FJsonObject> Issue = MakeValidationIssue(
                    TEXT("warning"),
                    TEXT("NIAGARA_SUBUV_ATLAS_SIZE_INDETERMINATE"),
                    FString::Printf(
                        TEXT("Renderer %d on emitter '%s' declares SubImageSize %d x %d, but %s. ")
                        TEXT("'valid' does not cover whether that grid matches the atlas it slices."),
                        Finding.RendererIndex,
                        *Finding.EmitterName,
                        Finding.DeclaredSubImageSize.X,
                        Finding.DeclaredSubImageSize.Y,
                        *Finding.UnverifiedReason),
                    Finding.EmitterName);
                Issue->SetNumberField(TEXT("rendererIndex"), Finding.RendererIndex);
                Issue->SetStringField(TEXT("texturePath"), Finding.TexturePath);
                AddValidationIssue(Issues, Errors, Warnings, Issue);
            }
        }
    }

    TSharedPtr<FJsonObject> MakeValidationResult(const FString& AssetPath, const FString& Level)
    {
        TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
        Result->SetStringField(TEXT("assetPath"), AssetPath);
        Result->SetStringField(TEXT("level"), Level);
        return Result;
    }

    void FinishValidationResult(
        FHandlerContext& Ctx,
        const TSharedPtr<FJsonObject>& Result,
        TArray<TSharedPtr<FJsonValue>>& Issues,
        TArray<TSharedPtr<FJsonValue>>& Errors,
        TArray<TSharedPtr<FJsonValue>>& Warnings)
    {
        Result->SetBoolField(TEXT("valid"), Errors.Num() == 0);
        Result->SetArrayField(TEXT("issues"), Issues);
        Result->SetArrayField(TEXT("errors"), Errors);
        Result->SetArrayField(TEXT("warnings"), Warnings);
        Ctx.SendSuccess(Result);
    }
}

REGISTER_RPC_HANDLER("niagara.inspect", "niagara", "Inspect a Niagara system, emitter, or script asset using the same structured aspects emitted by asset.dump. A system result also carries the measured componentActivation of its placed components in the open level.",
    RPC_PARAMS(
        RPC_PARAM_REQ("assetPath", "path", "Path to the Niagara System, Niagara Emitter, or Niagara Script asset"),
        RPC_PARAM_OPT("includeProperties", "boolean", "Include system/emitter, renderer, and parameter aspects. Defaults true."),
        RPC_PARAM_OPT("includeStack", "boolean", "Include stack/module order. Defaults true."),
        RPC_PARAM_OPT("includeGraphs", "boolean", "Include graph nodes, pins, and links. Defaults true."),
        RPC_PARAM_OPT("includeCompile", "boolean", "Include compile and validation summary. Defaults true."),
        RPC_PARAM_OPT("parametersOnly", "boolean", "Return only the 'parameters' aspect (drop system/emitter/renderer props, stack, graphs, compile) to keep a parameter readback inline. Defaults false."),
        RPC_PARAM_OPT("parameterName", "string", "Case-insensitive substring; keep only parameters whose name contains it (any store). Pair with parametersOnly to read back one User.* parameter without spilling.")
    ))
{
    const FString AssetPath = Ctx.GetString(TEXT("assetPath"));
    if (AssetPath.IsEmpty())
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("Missing 'assetPath'."));
        return true;
    }

    UObject* Asset = LoadObject<UObject>(nullptr, *AssetPath);
    if (!Asset)
    {
        Ctx.SendError(TEXT("ASSET_NOT_FOUND"), TEXT("Could not load Niagara asset."));
        return true;
    }

    const bool bIncludeProperties = Ctx.GetBool(TEXT("includeProperties"), true);
    const bool bIncludeStack = Ctx.GetBool(TEXT("includeStack"), true);
    const bool bIncludeGraphs = Ctx.GetBool(TEXT("includeGraphs"), true);
    const bool bIncludeCompile = Ctx.GetBool(TEXT("includeCompile"), true);
    const bool bParametersOnly = Ctx.GetBool(TEXT("parametersOnly"), false);
    const FString ParameterName = Ctx.GetString(TEXT("parameterName"));

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("assetPath"), AssetPath);
    Result->SetStringField(TEXT("assetClass"), Asset->GetClass()->GetPathName());

    if (const UNiagaraSystem* System = Cast<UNiagaraSystem>(Asset))
    {
        Result->SetStringField(TEXT("assetKind"), TEXT("NiagaraSystem"));
        AddNiagaraSystemInspectAspects(Result, System, bIncludeProperties, bIncludeStack, bIncludeGraphs, bIncludeCompile, bParametersOnly, ParameterName);
        // Every aspect above describes the ASSET. "Is anything in the level actually running
        // it" is a property of the placed UNiagaraComponent, and answering it used to need a
        // raw reflected object.call_function IsActive
        // (B-niagara-validate-green-while-component-inactive). Published here in exactly the
        // shape validate publishes it, but with no issue attached: inspect reports, it does
        // not judge. Skipped under parametersOnly, which exists so a single-parameter readback
        // stays inline and would be defeated by adding a block to it.
        if (!bParametersOnly)
        {
            TArray<PinWrightNiagara::FPlacedNiagaraComponent> Placed;
            PublishComponentActivation(*System, Result, Placed);
        }
        Ctx.SendSuccess(Result);
        return true;
    }

    if (const UNiagaraEmitter* Emitter = Cast<UNiagaraEmitter>(Asset))
    {
        Result->SetStringField(TEXT("assetKind"), TEXT("NiagaraEmitter"));
        AddNiagaraEmitterInspectAspects(Result, Emitter, bIncludeProperties, bIncludeStack, bIncludeGraphs, bIncludeCompile, bParametersOnly, ParameterName);
        Ctx.SendSuccess(Result);
        return true;
    }

    if (const UNiagaraScript* Script = Cast<UNiagaraScript>(Asset))
    {
        Result->SetStringField(TEXT("assetKind"), TEXT("NiagaraScript"));
        AddNiagaraScriptInspectAspects(Result, Script, bIncludeGraphs, bIncludeCompile);
        Ctx.SendSuccess(Result);
        return true;
    }

    Ctx.SendError(TEXT("UNSUPPORTED_ASSET"), TEXT("Asset is not a Niagara System, Niagara Emitter, or Niagara Script."));
    return true;
}

REGISTER_RPC_HANDLER("niagara.validate", "niagara", "Validate a Niagara system, emitter, or script asset and return structured issues.",
    RPC_PARAMS(
        RPC_PARAM_OPT("assetPath", "path", "Path to the Niagara System, Niagara Emitter, or Niagara Script asset."),
        RPC_PARAM_OPT("systemPath", "path", "Alias for assetPath when validating a Niagara System."),
        RPC_PARAM_OPT("level", "string", "Validation level: basic or strict. Defaults basic.")
    ))
{
    const FString AssetPath = Ctx.GetStringFirstOf({ TEXT("assetPath"), TEXT("systemPath") });
    if (AssetPath.IsEmpty())
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("Missing 'assetPath' or 'systemPath'."));
        return true;
    }

    FString Level = Ctx.GetString(TEXT("level"), TEXT("basic"));
    if (!Level.Equals(TEXT("basic"), ESearchCase::IgnoreCase) && !Level.Equals(TEXT("strict"), ESearchCase::IgnoreCase))
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("'level' must be 'basic' or 'strict'."));
        return true;
    }
    Level = Level.Equals(TEXT("strict"), ESearchCase::IgnoreCase) ? TEXT("strict") : TEXT("basic");

    TArray<TSharedPtr<FJsonValue>> Issues;
    TArray<TSharedPtr<FJsonValue>> Errors;
    TArray<TSharedPtr<FJsonValue>> Warnings;
    TSharedPtr<FJsonObject> Result = MakeValidationResult(AssetPath, Level);

    UObject* Asset = LoadObject<UObject>(nullptr, *AssetPath);
    if (!Asset)
    {
        AddValidationIssue(
            Issues,
            Errors,
            Warnings,
            MakeValidationIssue(TEXT("error"), TEXT("ASSET_NOT_FOUND"), FString::Printf(TEXT("Could not load asset '%s'."), *AssetPath)));
        FinishValidationResult(Ctx, Result, Issues, Errors, Warnings);
        return true;
    }

    Result->SetStringField(TEXT("assetClass"), Asset->GetClass()->GetPathName());

    if (const UNiagaraSystem* System = Cast<UNiagaraSystem>(Asset))
    {
        Result->SetStringField(TEXT("assetKind"), TEXT("NiagaraSystem"));
        TSharedPtr<FJsonObject> Compile = NiagaraDumpBuilder::BuildCompileDiagnosticsJson(System);
        Result->SetObjectField(TEXT("compile"), Compile);
        AddCompileIssues(Compile, Level, Issues, Errors, Warnings);
        AddCompileStatusIssues(Compile, Result, Issues, Errors, Warnings);
        AddDisabledEmitterIssues(System, Level, Issues, Errors, Warnings);
        AddUninvokedEmitterIssues(System, Issues, Errors, Warnings);
        AddStaleParentEmitterIssues(System, Issues, Errors, Warnings);
        AddDataInterfaceConsistencyIssues(System, Result, Issues, Errors, Warnings);
        AddComponentActivationIssues(System, Level, Result, Issues, Errors, Warnings);
        AddSubUVAtlasIssues(System, Result, Issues, Errors, Warnings);
        FinishValidationResult(Ctx, Result, Issues, Errors, Warnings);
        return true;
    }

    if (const UNiagaraEmitter* Emitter = Cast<UNiagaraEmitter>(Asset))
    {
        Result->SetStringField(TEXT("assetKind"), TEXT("NiagaraEmitter"));
        TSharedPtr<FJsonObject> Compile = NiagaraDumpBuilder::BuildEmitterCompileDiagnosticsJson(Emitter);
        Result->SetObjectField(TEXT("compile"), Compile);
        AddCompileIssues(Compile, Level, Issues, Errors, Warnings);
        AddCompileStatusIssues(Compile, Result, Issues, Errors, Warnings);
        FinishValidationResult(Ctx, Result, Issues, Errors, Warnings);
        return true;
    }

    if (const UNiagaraScript* Script = Cast<UNiagaraScript>(Asset))
    {
        Result->SetStringField(TEXT("assetKind"), TEXT("NiagaraScript"));
        TSharedPtr<FJsonObject> Compile = NiagaraDumpBuilder::BuildScriptCompileDiagnosticsJson(Script);
        Result->SetObjectField(TEXT("compile"), Compile);
        AddCompileIssues(Compile, Level, Issues, Errors, Warnings);
        AddCompileStatusIssues(Compile, Result, Issues, Errors, Warnings);
        FinishValidationResult(Ctx, Result, Issues, Errors, Warnings);
        return true;
    }

    Result->SetStringField(TEXT("assetKind"), TEXT("Unsupported"));
    AddValidationIssue(
        Issues,
        Errors,
        Warnings,
        MakeValidationIssue(TEXT("error"), TEXT("UNSUPPORTED_ASSET"), TEXT("Asset is not a Niagara System, Niagara Emitter, or Niagara Script.")));
    FinishValidationResult(Ctx, Result, Issues, Errors, Warnings);
    return true;
}
