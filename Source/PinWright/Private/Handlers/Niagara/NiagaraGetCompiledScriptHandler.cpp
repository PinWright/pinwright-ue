// Copyright (c) 2026 Alexander Penkin. MIT License.

// niagara.get_compiled_script — what the Niagara translator and compilers produced for each script
// of a system, emitter or standalone script asset: generated HLSL, VM assembly, op count, bytecode
// and GPU shader permutation stats. The editor's Generated Code tab reads the same fields.

#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/ErrorCodes.h"
#include "Handlers/Niagara/NiagaraCompileWait.h"
#include "Handlers/Niagara/NiagaraEditTypes.h"
#include "Handlers/Niagara/NiagaraRapidIteration.h"
#include "Handlers/Niagara/NiagaraScriptCompileReport.h"

#include "NiagaraEmitter.h"
#include "NiagaraEmitterHandle.h"
#include "NiagaraScript.h"
#include "NiagaraShared.h"
#include "NiagaraSystem.h"
#include "UObject/Package.h"

namespace NiagaraGetCompiledScriptLocal
{
    struct FScriptEntry
    {
        UNiagaraScript* Script = nullptr;
        FString Emitter;
        FString SimTarget;
    };

    void AddEmitterScripts(const FVersionedNiagaraEmitterData& Data, const FString& EmitterName, TArray<FScriptEntry>& Out)
    {
        TArray<UNiagaraScript*> Scripts;
        Data.GetScripts(Scripts, /*bCompilableOnly=*/true);
        const FString SimTarget = Data.SimTarget == ENiagaraSimTarget::GPUComputeSim ? TEXT("GPU") : TEXT("CPU");
        for (UNiagaraScript* Script : Scripts)
        {
            if (Script && !Out.ContainsByPredicate([Script](const FScriptEntry& E) { return E.Script == Script; }))
            {
                Out.Add({Script, EmitterName, SimTarget});
            }
        }
    }

    FString UsageName(const UNiagaraScript& Script)
    {
        return NiagaraEdit::StackScriptUsageToString(Script.GetUsage());
    }

    bool UsageMatches(const UNiagaraScript& Script, const FString& Filter)
    {
        if (Filter.IsEmpty())
        {
            return true;
        }
        FString Actual = UsageName(Script);
        FString Wanted = Filter;
        Actual.RemoveFromEnd(TEXT("Script"), ESearchCase::IgnoreCase);
        Wanted.RemoveFromEnd(TEXT("Script"), ESearchCase::IgnoreCase);
        return Actual.Equals(Wanted, ESearchCase::IgnoreCase);
    }

    // Text field with an explicit absence: never an empty string standing in for "not available".
    void AddText(
        const TSharedPtr<FJsonObject>& Obj,
        const TCHAR* Field,
        const FString& Text,
        int32 MaxChars,
        const FString& MissingReason,
        const FString& MissingHint)
    {
        if (Text.IsEmpty())
        {
            TSharedPtr<FJsonObject> Missing = MakeShared<FJsonObject>();
            Missing->SetStringField(TEXT("reason"), MissingReason);
            Missing->SetStringField(TEXT("hint"), MissingHint);
            Obj->SetObjectField(FString(Field) + TEXT("Missing"), Missing);
            return;
        }
        const bool bTruncated = Text.Len() > MaxChars;
        Obj->SetStringField(Field, bTruncated ? Text.Left(MaxChars) : Text);
        Obj->SetBoolField(FString(Field) + TEXT("Truncated"), bTruncated);
        Obj->SetNumberField(FString(Field) + TEXT("TotalChars"), Text.Len());
    }
}

REGISTER_RPC_HANDLER("niagara.get_compiled_script", "niagara",
    "Read what the last compile produced for each script of a Niagara system, emitter or standalone script asset: "
    "status and compile events, bytecode/register/attribute stats, GPU shader permutations, and on request the generated "
    "HLSL and VM assembly. Text the engine did not retain is reported as <field>Missing with the reason, never as \"\".",
    RPC_PARAMS(
        RPC_PARAM_REQ("assetPath", "path", "Niagara System, Niagara Emitter, or standalone Niagara Script asset"),
        RPC_PARAM_OPT("emitter", "string", "Only this emitter's scripts (Niagara System assets only)"),
        RPC_PARAM_OPT("scriptUsage", "string", "Only scripts of this usage, e.g. ParticleUpdate, SystemSpawn, ParticleGPUComputeScript, Module"),
        RPC_PARAM_OPT("include", "array", "Any of \"stats\", \"hlsl\", \"assembly\". Default [\"stats\"]."),
        RPC_PARAM_DEF("maxChars", "number", "Per-text cap; longer hlsl/assembly is cut and reports <field>Truncated:true with <field>TotalChars", "20000"),
        RPC_PARAM_DEF("forceCompile", "boolean",
            "Force a fresh translate+compile first (system and script assets), so the transient CPU HLSL/assembly/op count are populated. "
            "The asset's package dirty flag is restored to its prior value; running instances of a system are stopped.", "false")
    ))
{
    using namespace NiagaraGetCompiledScriptLocal;

    FString AssetPath;
    if (!Ctx.RequireString(TEXT("assetPath"), AssetPath)) return true;
    const TSharedPtr<FJsonObject>& Raw = Ctx.GetRawPayload();

    bool bStats = true;
    bool bHlsl = false;
    bool bAssembly = false;
    const TArray<TSharedPtr<FJsonValue>>* IncludeValues = nullptr;
    if (Raw->TryGetArrayField(TEXT("include"), IncludeValues) && IncludeValues)
    {
        bStats = false;
        for (const TSharedPtr<FJsonValue>& Value : *IncludeValues)
        {
            FString Item;
            if (!Value.IsValid() || !Value->TryGetString(Item))
            {
                Item = TEXT("<non-string>");
            }
            if (Item == TEXT("stats")) { bStats = true; }
            else if (Item == TEXT("hlsl")) { bHlsl = true; }
            else if (Item == TEXT("assembly")) { bAssembly = true; }
            else
            {
                Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
                    FString::Printf(TEXT("Unknown include '%s'. Valid: stats, hlsl, assembly."), *Item));
                return true;
            }
        }
    }

    double MaxCharsNumber = 20000.0;
    Raw->TryGetNumberField(TEXT("maxChars"), MaxCharsNumber);
    if (!FMath::IsFinite(MaxCharsNumber) || MaxCharsNumber < 1.0)
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT, TEXT("maxChars must be a positive number."));
        return true;
    }
    const int32 MaxChars = static_cast<int32>(FMath::Min(MaxCharsNumber, static_cast<double>(MAX_int32)));

    const FString EmitterFilter = Ctx.GetString(TEXT("emitter"));
    const FString UsageFilter = Ctx.GetString(TEXT("scriptUsage"));
    const bool bForceCompile = Ctx.GetBool(TEXT("forceCompile"), false);

    UObject* Asset = LoadObject<UObject>(nullptr, *AssetPath);
    UNiagaraSystem* System = Cast<UNiagaraSystem>(Asset);
    UNiagaraEmitter* Emitter = Cast<UNiagaraEmitter>(Asset);
    UNiagaraScript* StandaloneScript = Cast<UNiagaraScript>(Asset);
    if (!System && !Emitter && !StandaloneScript)
    {
        Ctx.SendError(Asset ? ErrorCodes::ERR_UNSUPPORTED_ASSET : ErrorCodes::ERR_ASSET_NOT_FOUND,
            FString::Printf(TEXT("'%s' is not a loadable Niagara System, Niagara Emitter, or Niagara Script."), *AssetPath));
        return true;
    }
    if (!System && !EmitterFilter.IsEmpty())
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT, TEXT("'emitter' filters a Niagara System's emitters; this asset has none to choose from."));
        return true;
    }
    if (Emitter && bForceCompile)
    {
        // A standalone emitter's scripts are compiled only as part of the systems that use it, and
        // those systems compile their own copies, so a forced compile here would not refresh what
        // this call reads. Read the system instead.
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
            TEXT("forceCompile is supported for Niagara System and Niagara Script assets. Read a system that uses this emitter instead."));
        return true;
    }

    // --- optional forced compile (a read verb: the package dirty flag is put back as found) ---
    TSharedPtr<FJsonObject> ForcedCompile;
    if (bForceCompile)
    {
        UPackage* const Package = Asset->GetOutermost();
        const bool bWasDirty = Package->IsDirty();
        ForcedCompile = MakeShared<FJsonObject>();
        if (System)
        {
            // Same rapid-iteration preservation as niagara.compile: a forced compile rebuilds the
            // system-script stores and would otherwise discard authored values.
            PinWrightNiagara::FRapidIterationValueSnapshot Snapshot;
            Snapshot.Capture(*System);
            FNiagaraResolvedTarget Target;
            Target.Asset = System;
            Target.System = System;
            Target.AssetPath = AssetPath;
            const bool bRequested = NiagaraEdit::RequestNiagaraCompile(Target, /*bForce=*/true);
            const PinWrightNiagara::FCompileWaitOutcome Wait =
                PinWrightNiagara::WaitForSystemCompile(*System, bRequested);
            if (bRequested && Wait.bWaited)
            {
                Snapshot.MergeBack();
            }
            ForcedCompile->SetStringField(TEXT("status"), PinWrightNiagara::DescribeCompileOutcome(bRequested, Wait));
            ForcedCompile->SetNumberField(TEXT("waitedMs"), Wait.WaitedSeconds * 1000.0);
            ForcedCompile->SetNumberField(TEXT("quiescedInstances"), Target.QuiescedInstances);
        }
        else
        {
            // UNiagaraScript::RequestCompile is synchronous.
            const bool bCompilable = StandaloneScript->IsCompilable();
            if (bCompilable)
            {
                StandaloneScript->RequestCompile(StandaloneScript->GetExposedVersion().VersionGuid, /*bForceCompile=*/true);
            }
            ForcedCompile->SetStringField(TEXT("status"), bCompilable ? TEXT("completed") : TEXT("notRequested"));
        }
        const bool bDirtiedByCompile = !bWasDirty && Package->IsDirty();
        if (bDirtiedByCompile)
        {
            Package->SetDirtyFlag(false);
        }
        ForcedCompile->SetBoolField(TEXT("dirtyFlagRestored"), bDirtiedByCompile);
    }

    // --- collect ---
    TArray<FScriptEntry> Entries;
    if (System)
    {
        if (EmitterFilter.IsEmpty())
        {
            for (UNiagaraScript* Script : { System->GetSystemSpawnScript(), System->GetSystemUpdateScript() })
            {
                if (Script)
                {
                    Entries.Add({Script, FString(), TEXT("CPU")});
                }
            }
        }
        for (const FNiagaraEmitterHandle& Handle : System->GetEmitterHandles())
        {
            const FString HandleName = Handle.GetName().ToString();
            const FVersionedNiagaraEmitterData* Data = Handle.GetEmitterData();
            if (Data && (EmitterFilter.IsEmpty() || HandleName.Equals(EmitterFilter, ESearchCase::IgnoreCase)))
            {
                AddEmitterScripts(*Data, HandleName, Entries);
            }
        }
    }
    else if (Emitter)
    {
        if (const FVersionedNiagaraEmitterData* Data = Emitter->GetLatestEmitterData())
        {
            AddEmitterScripts(*Data, Emitter->GetName(), Entries);
        }
    }
    else
    {
        Entries.Add({StandaloneScript, FString(), TEXT("standalone")});
    }

    TArray<FString> Available;
    Entries.RemoveAll([&UsageFilter, &Available](const FScriptEntry& Entry)
    {
        Available.AddUnique(UsageName(*Entry.Script));
        return !UsageMatches(*Entry.Script, UsageFilter);
    });
    if (Entries.Num() == 0)
    {
        Ctx.SendError(ErrorCodes::ERR_TARGET_NOT_FOUND, FString::Printf(
            TEXT("No script matched emitter '%s' / scriptUsage '%s'. Usages present: %s."),
            *EmitterFilter, *UsageFilter, Available.Num() ? *FString::Join(Available, TEXT(", ")) : TEXT("none")));
        return true;
    }

    // --- read ---
    const FString ForceHint = Emitter
        ? TEXT("Read a Niagara System that uses this emitter with forceCompile:true.")
        : TEXT("Call again with forceCompile:true.");
    TArray<TSharedPtr<FJsonValue>> Scripts;
    for (const FScriptEntry& Entry : Entries)
    {
        const UNiagaraScript& Script = *Entry.Script;
        const FNiagaraVMExecutableData& Data = Script.GetVMExecutableData();
        const bool bGpuCompute = Script.GetUsage() == ENiagaraScriptUsage::ParticleGPUComputeScript;
        const FString Status = PinWrightNiagara::DescribeScriptCompileStatus(Script);
        const bool bNeverCompiled = Status == TEXT("notCompiled");

        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetStringField(TEXT("usage"), UsageName(Script));
        Obj->SetStringField(TEXT("usageId"), Script.GetUsageId().ToString());
        Obj->SetStringField(TEXT("emitter"), Entry.Emitter);
        Obj->SetStringField(TEXT("simTarget"), Entry.SimTarget);
        Obj->SetStringField(TEXT("scriptPath"), Script.GetPathName());
        Obj->SetStringField(TEXT("compileStatus"), Status);

        if (bStats)
        {
            Obj->SetNumberField(TEXT("byteCodeBytes"), Data.ByteCode.GetLength());
            Obj->SetNumberField(TEXT("numTempRegisters"), Data.NumTempRegisters);
            Obj->SetNumberField(TEXT("attributeCount"), Data.Attributes.Num());
            Obj->SetNumberField(TEXT("simStages"), Data.SimulationStageMetaData.Num());
#if WITH_EDITORONLY_DATA
            Obj->SetNumberField(TEXT("dataInterfaceCount"), Data.DataInterfaceInfo.Num());
            // LastOpCount is Transient: 0 after a restart or a DDC-hit compile is "not retained",
            // not "zero ops", so it is published only while the assembly it was counted from is.
            if (!Data.LastAssemblyTranslation.IsEmpty())
            {
                Obj->SetNumberField(TEXT("opCount"), Data.LastOpCount);
            }
            else
            {
                Obj->SetField(TEXT("opCount"), MakeShared<FJsonValueNull>());
            }
#endif
            Obj->SetArrayField(TEXT("compileEvents"), PinWrightNiagara::BuildScriptCompileEventsJson(Script, /*bErrorsOnly=*/false));
            if (bGpuCompute)
            {
                TSharedPtr<FJsonObject> Gpu = MakeShared<FJsonObject>();
                const FNiagaraShaderScript* ShaderScript = Script.GetRenderThreadScript();
                Gpu->SetNumberField(TEXT("permutations"), ShaderScript ? ShaderScript->GetNumPermutations() : 0);
                Gpu->SetBoolField(TEXT("shaderCompileFinished"), !Script.IsScriptCompilationPending(/*bGPUScript=*/true));
                Gpu->SetBoolField(TEXT("shaderCompileSucceeded"), Script.DidScriptCompilationSucceed(/*bGPUScript=*/true));
                TArray<TSharedPtr<FJsonValue>> ShaderErrors;
                if (ShaderScript)
                {
                    for (const FString& Error : ShaderScript->GetCompileErrors())
                    {
                        ShaderErrors.Add(MakeShared<FJsonValueString>(Error));
                    }
                }
                Gpu->SetArrayField(TEXT("shaderErrors"), ShaderErrors);
                Obj->SetObjectField(TEXT("gpu"), Gpu);
            }
        }

#if WITH_EDITORONLY_DATA
        if (bHlsl)
        {
            AddText(Obj, TEXT("hlsl"),
                bGpuCompute ? Data.LastHlslTranslationGPU : Data.LastHlslTranslation, MaxChars,
                bNeverCompiled ? TEXT("notCompiled")
                : bGpuCompute ? TEXT("gpuTranslationEmpty")
                : TEXT("transientNotRetained"),
                bGpuCompute
                    ? FString(TEXT("The persisted GPU translation is empty; compile the system."))
                    : FString(TEXT("CPU HLSL is a Transient field: empty after an editor restart or a DDC-hit compile. ")) + ForceHint);
        }
        if (bAssembly)
        {
            if (bGpuCompute)
            {
                TSharedPtr<FJsonObject> Missing = MakeShared<FJsonObject>();
                Missing->SetStringField(TEXT("reason"), TEXT("gpuScriptHasNoVmAssembly"));
                Missing->SetStringField(TEXT("hint"), TEXT("GPU compute scripts compile to shaders, not VM bytecode."));
                Obj->SetObjectField(TEXT("assemblyMissing"), Missing);
            }
            else
            {
                AddText(Obj, TEXT("assembly"), Data.LastAssemblyTranslation, MaxChars,
                    bNeverCompiled ? TEXT("notCompiled") : TEXT("transientNotRetained"),
                    FString(TEXT("VM assembly is a Transient field: empty after an editor restart or a DDC-hit compile. ")) + ForceHint);
            }
        }
#endif
        Scripts.Add(MakeShared<FJsonValueObject>(Obj));
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("assetPath"), AssetPath);
    Result->SetStringField(TEXT("assetKind"),
        System ? TEXT("NiagaraSystem") : Emitter ? TEXT("NiagaraEmitter") : TEXT("NiagaraScript"));
    Result->SetBoolField(TEXT("forcedCompile"), bForceCompile);
    if (ForcedCompile.IsValid())
    {
        Result->SetObjectField(TEXT("compile"), ForcedCompile);
    }
    Result->SetNumberField(TEXT("count"), Scripts.Num());
    Result->SetArrayField(TEXT("scripts"), Scripts);
    Ctx.SendSuccess(Result);
    return true;
}
