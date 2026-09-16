// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression tests for B-niagara-validate-green-while-scripts-ncs-error.
//
// niagara.validate published a nested `compile` block carrying `valid: false` and ten particle
// scripts at `compileStatus: "NCS_Error"`, and answered `valid: true` with an empty `errors` array
// at level:"strict". Nothing ever promoted a script's compile STATUS into the issues array the
// top-level normalizer consumes - AddCompileIssues only read `compile.issues`, and no code path
// wrote a status into it. The two systems the verb green-lit were the only two in the package that
// then returned `active: false` from effect.activate_niagara, and a VFX report shipped both as
// structurally verified on the strength of this verb.
//
// What is asserted:
//   1. The pure reader (PinWrightNiagara::ReadCompileVerdict) turns a compile block into the
//      three-state verdict: a block with an NCS_Error entry is Failed, one where every status is
//      null is Unverified (NOT passed - "nothing has compiled this yet" must never read as a
//      pass), one with only good statuses is Passed, and the pending-compile flags are carried
//      through. The pending case is unit-tested here because a synthetic system cannot be made to
//      hold a real in-flight compile.
//   2. End to end through the handler: a system whose particle update script is at NCS_Error
//      validates `valid: false` at BOTH levels, names the failing script in `errors`, and carries
//      the compiler's own message. The same system with the status untouched raises no such issue,
//      which is the counterfactual - before the fix the broken run was byte-identical to it.
#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Handlers/Niagara/NiagaraCompileVerdict.h"
#include "Tests/Assets/NiagaraEditTestUtils.h"
#include "Tests/TestSkipReporting.h"
#include "Tests/TestUtils.h"

#include "Misc/Guid.h"
#include "NiagaraEmitter.h"
#include "NiagaraEmitterHandle.h"
#include "NiagaraScript.h"
#include "NiagaraSystem.h"
#include "UObject/Package.h"

namespace NiagaraValidateCompileErrorTestUtils
{
    // Distinct from NiagaraEditTestUtils::NewTransientSystem, which empties the emitter handles:
    // this test needs the fixture's emitters intact, because the script whose status it breaks has
    // to be one the system actually owns and the system graph actually invokes.
    inline UNiagaraSystem* DuplicateFixtureSystem(FString& OutObjectPath)
    {
        UNiagaraSystem* Source = LoadObject<UNiagaraSystem>(nullptr, NiagaraEditTestUtils::FixtureSystemAssetPath);
        if (!Source)
        {
            return nullptr;
        }

        const FString AssetName = NiagaraEditTestUtils::MakeAssetName(TEXT("NS_ValidateCompileError"));
        const FString PackageName = FString::Printf(TEXT("/Game/PinWrightTests/%s"), *AssetName);
        UPackage* Package = CreatePackage(*PackageName);
        // Same reason NiagaraEditTestUtils::NewTransientSystem marks the package transient: the
        // autosaver would otherwise route this dirty package through UPackage::Save ->
        // WaitForCompilationComplete -> RequestCompile on a system this test deliberately left in
        // a failed-compile state.
        Package->SetFlags(RF_Transient);
        UNiagaraSystem* System = DuplicateObject<UNiagaraSystem>(Source, Package, FName(*AssetName));
        if (!System)
        {
            return nullptr;
        }
        System->SetFlags(RF_Public | RF_Standalone | RF_Transactional);
        System->AddToRoot();
        OutObjectPath = FString::Printf(TEXT("%s.%s"), *PackageName, *AssetName);
        return System;
    }

    // First emitter handle whose particle update script is owned by the duplicate. The ownership
    // check is load-bearing: if the handle pointed at the shared engine fixture's emitter instead
    // of a duplicated inner, writing a compile status onto it would corrupt that asset for every
    // other test in the process.
    inline UNiagaraScript* FindOwnedParticleUpdateScript(UNiagaraSystem& System, FString& OutEmitterName)
    {
        for (const FNiagaraEmitterHandle& Handle : System.GetEmitterHandles())
        {
            const FVersionedNiagaraEmitterData* EmitterData = Handle.GetEmitterData();
            UNiagaraScript* Script = EmitterData ? EmitterData->UpdateScriptProps.Script : nullptr;
            if (Script && Script->IsIn(&System))
            {
                OutEmitterName = Handle.GetName().ToString();
                return Script;
            }
        }
        return nullptr;
    }

    inline TSharedPtr<FJsonObject> MakeScriptEntry(const TCHAR* OwnerKind, const TCHAR* OwnerName, const TCHAR* Usage, const TCHAR* Status)
    {
        TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetStringField(TEXT("ownerKind"), OwnerKind);
        Entry->SetStringField(TEXT("ownerName"), OwnerName);
        Entry->SetStringField(TEXT("scriptUsage"), Usage);
        Entry->SetStringField(TEXT("path"), FString::Printf(TEXT("/Game/Fake.Fake:%s"), Usage));
        if (Status)
        {
            Entry->SetStringField(TEXT("compileStatus"), Status);
        }
        else
        {
            Entry->SetField(TEXT("compileStatus"), MakeShared<FJsonValueNull>());
        }
        return Entry;
    }

    // Returns the first issue in Result.<ArrayName> carrying Code, or null.
    inline TSharedPtr<FJsonObject> FindIssue(const TSharedPtr<FJsonObject>& Result, const TCHAR* ArrayName, const TCHAR* Code)
    {
        const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
        if (!Result.IsValid() || !Result->TryGetArrayField(ArrayName, Values) || !Values)
        {
            return nullptr;
        }
        for (const TSharedPtr<FJsonValue>& Value : *Values)
        {
            if (!Value.IsValid() || Value->Type != EJson::Object)
            {
                continue;
            }
            FString IssueCode;
            if (Value->AsObject()->TryGetStringField(TEXT("code"), IssueCode) && IssueCode.Equals(Code, ESearchCase::IgnoreCase))
            {
                return Value->AsObject();
            }
        }
        return nullptr;
    }

    inline bool ValidateSystem(FAutomationTestBase& Test, const FString& AssetPath, const TCHAR* Level, FTestResponseCapture& Capture)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("assetPath"), AssetPath);
        Payload->SetStringField(TEXT("level"), Level);
        Test.TestTrue(TEXT("niagara.validate handler found"), InvokeHandlerWithCapture(TEXT("niagara.validate"), Payload, Capture));
        Test.TestTrue(TEXT("niagara.validate succeeded"), Capture.bSuccess);
        return Capture.bSuccess && Capture.Result.IsValid();
    }
}

// ============================================================================
// 1. The pure verdict reader.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNiagaraCompileVerdictReaderTest,
    "PinWright.niagara.validate.CompileVerdictReader",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraCompileVerdictReaderTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightNiagara;
    using namespace NiagaraValidateCompileErrorTestUtils;

    TestEqual(TEXT("passed spelling"), FString(ScriptCompileCheckToString(EScriptCompileCheck::Passed)), FString(TEXT("passed")));
    TestEqual(TEXT("failed spelling"), FString(ScriptCompileCheckToString(EScriptCompileCheck::Failed)), FString(TEXT("failed")));
    TestEqual(TEXT("unverified spelling"), FString(ScriptCompileCheckToString(EScriptCompileCheck::Unverified)), FString(TEXT("unverified")));

    // A null block is Unverified, never Passed.
    TestTrue(TEXT("null compile block is unverified"),
        ReadCompileVerdict(nullptr).Check == EScriptCompileCheck::Unverified);

    // Every status null (the post-load deferred state) is Unverified, not Passed. This is the
    // distinction the whole reader exists for: "nothing has compiled this" is not a pass.
    {
        TSharedPtr<FJsonObject> Compile = MakeShared<FJsonObject>();
        TArray<TSharedPtr<FJsonValue>> Scripts;
        Scripts.Add(MakeShared<FJsonValueObject>(MakeScriptEntry(TEXT("system"), TEXT("NS"), TEXT("SystemSpawnScript"), nullptr)));
        Scripts.Add(MakeShared<FJsonValueObject>(MakeScriptEntry(TEXT("emitter"), TEXT("E"), TEXT("ParticleUpdateScript"), nullptr)));
        Compile->SetArrayField(TEXT("scripts"), Scripts);
        const FCompileVerdict Verdict = ReadCompileVerdict(Compile);
        TestTrue(TEXT("all-null statuses read as unverified"), Verdict.Check == EScriptCompileCheck::Unverified);
        TestEqual(TEXT("unverified yields no failures"), Verdict.FailedScripts.Num(), 0);
        TestFalse(TEXT("absent pending flags are not measured"), Verdict.bPendingCompileKnown);
    }

    // One known-good sibling must not hide an unknown script. The reader used to pass this block
    // because it counted only the entries that had a status, contradicting its all-scripts
    // contract and making compile_status claim completion too early.
    {
        TSharedPtr<FJsonObject> Compile = MakeShared<FJsonObject>();
        TArray<TSharedPtr<FJsonValue>> Scripts;
        Scripts.Add(MakeShared<FJsonValueObject>(MakeScriptEntry(
            TEXT("system"), TEXT("NS"), TEXT("SystemSpawnScript"), TEXT("NCS_UpToDate"))));
        Scripts.Add(MakeShared<FJsonValueObject>(MakeScriptEntry(
            TEXT("emitter"), TEXT("E"), TEXT("ParticleUpdateScript"), nullptr)));
        Compile->SetArrayField(TEXT("scripts"), Scripts);
        const FCompileVerdict Verdict = ReadCompileVerdict(Compile);
        TestTrue(TEXT("mixed known and unknown statuses remain unverified"),
            Verdict.Check == EScriptCompileCheck::Unverified);
        TestEqual(TEXT("mixed unverified verdict has no failures"), Verdict.FailedScripts.Num(), 0);
    }

    // Terminal success statuses only - including the two "compiled, with warnings" statuses.
    {
        TSharedPtr<FJsonObject> Compile = MakeShared<FJsonObject>();
        TArray<TSharedPtr<FJsonValue>> Scripts;
        Scripts.Add(MakeShared<FJsonValueObject>(MakeScriptEntry(TEXT("system"), TEXT("NS"), TEXT("SystemSpawnScript"), TEXT("NCS_UpToDate"))));
        Scripts.Add(MakeShared<FJsonValueObject>(MakeScriptEntry(TEXT("emitter"), TEXT("E"), TEXT("ParticleUpdateScript"), TEXT("NCS_UpToDateWithWarnings"))));
        Scripts.Add(MakeShared<FJsonValueObject>(MakeScriptEntry(TEXT("emitter"), TEXT("E"), TEXT("ParticleSpawnScript"), TEXT("NCS_ComputeUpToDateWithWarnings"))));
        Compile->SetArrayField(TEXT("scripts"), Scripts);
        Compile->SetBoolField(TEXT("hasOutstandingCompilationRequests"), false);
        Compile->SetBoolField(TEXT("hasActiveCompilations"), false);
        const FCompileVerdict Verdict = ReadCompileVerdict(Compile);
        TestTrue(TEXT("good statuses read as passed"), Verdict.Check == EScriptCompileCheck::Passed);
        TestEqual(TEXT("passed yields no failures"), Verdict.FailedScripts.Num(), 0);
        TestTrue(TEXT("pending flags present are measured"), Verdict.bPendingCompileKnown);
        TestFalse(TEXT("nothing in flight"), Verdict.bPendingCompile);
        const FCompileProbeVerdict Probe = SummarizeCompileProbe(
            Verdict, Verdict.bPendingCompile, /*bCompileQueueObserved=*/true);
        TestEqual(TEXT("terminal successes complete the probe"), FString(Probe.Status), FString(TEXT("completed")));
        TestTrue(TEXT("terminal successes are successful"), Probe.bSuccessful);
        const FCompileProbeVerdict Unobserved = SummarizeCompileProbe(
            Verdict, Verdict.bPendingCompile, /*bCompileQueueObserved=*/false);
        TestEqual(TEXT("an unobserved queue stays unverified"),
            FString(Unobserved.Status), FString(TEXT("unverified")));
        TestFalse(TEXT("an unobserved queue is not completed"), Unobserved.bCompleted);
    }

    // Dirty means the authored graph has moved past the cached bytecode. It may still instance,
    // but it is not evidence that the requested revision completed.
    {
        TSharedPtr<FJsonObject> Compile = MakeShared<FJsonObject>();
        TArray<TSharedPtr<FJsonValue>> Scripts;
        Scripts.Add(MakeShared<FJsonValueObject>(MakeScriptEntry(
            TEXT("system"), TEXT("NS"), TEXT("SystemSpawnScript"), TEXT("NCS_UpToDate"))));
        Scripts.Add(MakeShared<FJsonValueObject>(MakeScriptEntry(
            TEXT("emitter"), TEXT("E"), TEXT("ParticleUpdateScript"), TEXT("NCS_Dirty"))));
        Compile->SetArrayField(TEXT("scripts"), Scripts);
        const FCompileVerdict Verdict = ReadCompileVerdict(Compile);
        TestTrue(TEXT("a dirty sibling keeps the verdict unverified"),
            Verdict.Check == EScriptCompileCheck::Unverified);
        TestEqual(TEXT("dirty is stale rather than failed"), Verdict.FailedScripts.Num(), 0);
    }

    // One NCS_Error among good statuses fails the whole verdict and is reported with its owner,
    // slot and compiler messages.
    {
        TSharedPtr<FJsonObject> Compile = MakeShared<FJsonObject>();
        TArray<TSharedPtr<FJsonValue>> Scripts;
        Scripts.Add(MakeShared<FJsonValueObject>(MakeScriptEntry(TEXT("system"), TEXT("NS"), TEXT("SystemSpawnScript"), TEXT("NCS_UpToDate"))));
        TSharedPtr<FJsonObject> Broken = MakeScriptEntry(TEXT("emitter"), TEXT("Shockwave"), TEXT("ParticleUpdateScript"), TEXT("NCS_Error"));
        TArray<TSharedPtr<FJsonValue>> CompileErrors;
        CompileErrors.Add(MakeShared<FJsonValueString>(TEXT("Invalid swizzle / mask 'RampInOut'")));
        Broken->SetArrayField(TEXT("compileErrors"), CompileErrors);
        Scripts.Add(MakeShared<FJsonValueObject>(Broken));
        Compile->SetArrayField(TEXT("scripts"), Scripts);
        const FCompileVerdict Verdict = ReadCompileVerdict(Compile);
        TestTrue(TEXT("an NCS_Error fails the verdict"), Verdict.Check == EScriptCompileCheck::Failed);
        TestEqual(TEXT("one failure reported"), Verdict.FailedScripts.Num(), 1);
        if (Verdict.FailedScripts.Num() == 1)
        {
            const FScriptCompileFailure& Failure = Verdict.FailedScripts[0];
            TestEqual(TEXT("failure names its emitter"), Failure.OwnerName, FString(TEXT("Shockwave")));
            TestEqual(TEXT("failure names its script slot"), Failure.ScriptUsage, FString(TEXT("ParticleUpdateScript")));
            TestEqual(TEXT("failure carries the compiler message"), Failure.Errors.Num(), 1);
            const FString Described = DescribeScriptCompileFailure(Failure);
            TestTrue(TEXT("description names the script"), Described.Contains(TEXT("Shockwave.ParticleUpdateScript")));
            TestTrue(TEXT("description quotes the compiler message"), Described.Contains(TEXT("Invalid swizzle")));
        }
    }

    // A compile still in flight is carried through, whichever of the two flags reports it.
    {
        TSharedPtr<FJsonObject> Compile = MakeShared<FJsonObject>();
        TArray<TSharedPtr<FJsonValue>> Scripts;
        Scripts.Add(MakeShared<FJsonValueObject>(MakeScriptEntry(TEXT("system"), TEXT("NS"), TEXT("SystemSpawnScript"), TEXT("NCS_UpToDate"))));
        Compile->SetArrayField(TEXT("scripts"), Scripts);
        Compile->SetBoolField(TEXT("hasOutstandingCompilationRequests"), false);
        Compile->SetBoolField(TEXT("hasActiveCompilations"), true);
        const FCompileVerdict Verdict = ReadCompileVerdict(Compile);
        TestTrue(TEXT("an active compilation is pending"), Verdict.bPendingCompile);
        TestTrue(TEXT("pending state is measured"), Verdict.bPendingCompileKnown);
        const FCompileProbeVerdict Probe = SummarizeCompileProbe(
            Verdict, Verdict.bPendingCompile, /*bCompileQueueObserved=*/true);
        TestEqual(TEXT("pending work wins over cached successful statuses"),
            FString(Probe.Status), FString(TEXT("compiling")));
        TestFalse(TEXT("pending work is not completed"), Probe.bCompleted);
        TestFalse(TEXT("pending work is not successful"), Probe.bSuccessful);

        // BuildCompileDiagnosticsJson populates this broad flag with
        // HasOutstandingCompilationRequests(true), so it also covers GPU-only residue.
        Compile->SetBoolField(TEXT("hasActiveCompilations"), false);
        Compile->SetBoolField(TEXT("hasOutstandingCompilationRequests"), true);
        const FCompileVerdict BroadPendingVerdict = ReadCompileVerdict(Compile);
        const FCompileProbeVerdict BroadPendingProbe = SummarizeCompileProbe(
            BroadPendingVerdict,
            BroadPendingVerdict.bPendingCompile,
            /*bCompileQueueObserved=*/true);
        TestEqual(TEXT("the GPU-inclusive pending flag also prevents completion"),
            FString(BroadPendingProbe.Status), FString(TEXT("compiling")));
        TestFalse(TEXT("GPU-inclusive pending work is not completed"), BroadPendingProbe.bCompleted);
    }

    return true;
}

// ============================================================================
// 2. End to end: the handler's verdict on a system with a failed script.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNiagaraValidateScriptCompileErrorTest,
    "PinWright.niagara.validate.ScriptCompileErrorFailsVerdict",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraValidateScriptCompileErrorTest::RunTest(const FString& Parameters)
{
    using namespace NiagaraValidateCompileErrorTestUtils;

    TestTrue(TEXT("niagara.validate is registered"), IsHandlerRegistered(TEXT("niagara.validate")));

    FString SystemPath;
    // FAuthorableSystemRoots below unroots the duplicate, but it is also RF_Standalone, which the
    // periodic suite GC keeps. Declared before Roots so it runs after that unroot.
    ON_SCOPE_EXIT
    {
        CleanupTestAsset(SystemPath);
    };

    UNiagaraSystem* System = DuplicateFixtureSystem(SystemPath);
    if (!System)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("niagara-fixture-system-unavailable"),
            FString::Printf(TEXT("could not duplicate '%s'"), NiagaraEditTestUtils::FixtureSystemAssetPath));
        return true;
    }
    NiagaraEditTestUtils::FAuthorableSystemRoots Roots;
    Roots.System = System;

    FString EmitterName;
    UNiagaraScript* Script = FindOwnedParticleUpdateScript(*System, EmitterName);
    if (!Script)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("niagara-fixture-owned-script-unavailable"),
            TEXT("the fixture system exposes no particle update script owned by the duplicate"));
        return true;
    }

    // ---- Control: the same system, status untouched. ----
    {
        FTestResponseCapture Capture;
        if (ValidateSystem(*this, SystemPath, TEXT("strict"), Capture))
        {
            TestNull(TEXT("an untouched system raises no script-compile error"),
                FindIssue(Capture.Result, TEXT("errors"), TEXT("NIAGARA_SCRIPT_COMPILE_ERROR")).Get());
            FString Check;
            TestTrue(TEXT("scriptCompileCheck is published on every verdict"),
                Capture.Result->TryGetStringField(TEXT("scriptCompileCheck"), Check));
            TestFalse(TEXT("an untouched system does not read as failed"), Check.Equals(TEXT("failed")));
        }
    }

    // ---- Break it: exactly the state the ticket measured on disk. ----
    const ENiagaraScriptCompileStatus OriginalStatus = Script->GetVMExecutableData().LastCompileStatus;
#if WITH_EDITORONLY_DATA
    const TArray<FNiagaraCompileEvent> OriginalEvents = Script->GetVMExecutableData().LastCompileEvents;
    const FString InjectedMessage(TEXT("PinWrightTest: Invalid swizzle / mask 'RampInOut'"));
    Script->GetVMExecutableData().LastCompileEvents.Add(
        FNiagaraCompileEvent(FNiagaraCompileEventSeverity::Error, InjectedMessage));
#endif
    Script->GetVMExecutableData().LastCompileStatus = ENiagaraScriptCompileStatus::NCS_Error;

    for (const TCHAR* Level : { TEXT("basic"), TEXT("strict") })
    {
        FTestResponseCapture Capture;
        if (!ValidateSystem(*this, SystemPath, Level, Capture))
        {
            continue;
        }

        const FString Where = FString::Printf(TEXT("[level=%s] "), Level);
        bool bValid = true;
        Capture.Result->TryGetBoolField(TEXT("valid"), bValid);
        TestFalse(*(Where + TEXT("a failed script makes the asset invalid")), bValid);

        FString Check;
        Capture.Result->TryGetStringField(TEXT("scriptCompileCheck"), Check);
        TestEqual(*(Where + TEXT("scriptCompileCheck reports the failure")), Check, FString(TEXT("failed")));

        const TSharedPtr<FJsonObject> Issue = FindIssue(Capture.Result, TEXT("errors"), TEXT("NIAGARA_SCRIPT_COMPILE_ERROR"));
        TestNotNull(*(Where + TEXT("errors name the failed script")), Issue.Get());
        if (!Issue.IsValid())
        {
            continue;
        }
        FString IssueEmitter;
        Issue->TryGetStringField(TEXT("emitter"), IssueEmitter);
        TestEqual(*(Where + TEXT("the issue names the emitter")), IssueEmitter, EmitterName);

        FString IssueUsage;
        Issue->TryGetStringField(TEXT("scriptUsage"), IssueUsage);
        TestEqual(*(Where + TEXT("the issue names the script slot")), IssueUsage, FString(TEXT("ParticleUpdateScript")));

        FString IssueStatus;
        Issue->TryGetStringField(TEXT("compileStatus"), IssueStatus);
        TestEqual(*(Where + TEXT("the issue carries the raw compile status")), IssueStatus, FString(TEXT("NCS_Error")));
#if WITH_EDITORONLY_DATA
        FString IssueMessage;
        Issue->TryGetStringField(TEXT("message"), IssueMessage);
        TestTrue(*(Where + TEXT("the issue carries the compiler's own message")),
            IssueMessage.Contains(InjectedMessage));
        TestTrue(*(Where + TEXT("the issue lists the compile errors")),
            JsonStringArrayContains(Issue, TEXT("compileErrors"), InjectedMessage));
#endif
    }

    // Restore so nothing downstream inherits the injected failure.
    Script->GetVMExecutableData().LastCompileStatus = OriginalStatus;
#if WITH_EDITORONLY_DATA
    Script->GetVMExecutableData().LastCompileEvents = OriginalEvents;
#endif
    return true;
}
