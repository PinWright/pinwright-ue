// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Misc/AutomationTest.h"
#include "Handlers/Niagara/NiagaraDumpBuilder.h"
#include "Handlers/Niagara/NiagaraCompileWait.h"
#include "Handlers/Niagara/NiagaraCompileVerdict.h"
#include "Tests/Assets/NiagaraEditTestUtils.h"
#include "NiagaraJsonAssertionHelpers.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Tests/TestSkipReporting.h"


#include "HAL/IConsoleManager.h"
#include "NiagaraEmitter.h"
#include "NiagaraScript.h"
#include "NiagaraSystem.h"
#include "UObject/Package.h"
#include "Misc/Guid.h"
#include "Misc/ScopeExit.h"
#include "UObject/StrongObjectPtr.h"

namespace
{
    using NiagaraJsonAssertionHelpers::IssuesContainCode;

    bool JsonFieldIsNull(const TSharedPtr<FJsonObject>& Obj, const TCHAR* FieldName)
    {
        if (!Obj.IsValid())
        {
            return false;
        }
        const TSharedPtr<FJsonValue> Value = Obj->TryGetField(FieldName);
        return Value.IsValid() && Value->Type == EJson::Null;
    }

    UNiagaraSystem* MakeTransientSystemForTest()
    {
        const FString AssetName = FString::Printf(TEXT("NS_DeferredFlagTest_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
        const FString PackageName = FString::Printf(TEXT("/Game/PinWrightTests/%s"), *AssetName);
        UPackage* Package = CreatePackage(*PackageName);
        UNiagaraSystem* System = NewObject<UNiagaraSystem>(
            Package,
            UNiagaraSystem::StaticClass(),
            *AssetName,
            RF_Public | RF_Standalone | RF_Transient);
        if (System)
        {
            System->AddToRoot();
        }
        return System;
    }

    UNiagaraEmitter* MakeTransientEmitterForTest()
    {
        const FString AssetName = FString::Printf(TEXT("NE_UninitializedTest_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
        const FString PackageName = FString::Printf(TEXT("/Game/PinWrightTests/%s"), *AssetName);
        UPackage* Package = CreatePackage(*PackageName);
        UNiagaraEmitter* Emitter = NewObject<UNiagaraEmitter>(
            Package,
            UNiagaraEmitter::StaticClass(),
            *AssetName,
            RF_Public | RF_Standalone | RF_Transient);
        if (Emitter)
        {
            Emitter->AddToRoot();
        }
        return Emitter;
    }
}

// Regression test for B-asset-dump-niagara-compile-state-stale:
// niagara_compile.json must surface fx.Niagara.OnDemandCompileEnabled state via a
// compileDeferredOnLoad boolean and emit a COMPILE_DEFERRED_ON_LOAD info-severity
// issue when that CVar is enabled and the system has never compiled (the transient system here), so consumers can distinguish post-load
// deferred state from a genuinely broken system.
//
// Counterfactual: if the new compileDeferredOnLoad field write and the
// COMPILE_DEFERRED_ON_LOAD issue emission in BuildCompileJson are reverted,
// case-A's Contains("compileDeferredOnLoad") assertion fails because the field
// is no longer written.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNiagaraDumpCompileDeferredFlagTest,
    "PinWright.Niagara.DumpCompile.DeferredFlag",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraDumpCompileDeferredFlagTest::RunTest(const FString& Parameters)
{
    IConsoleVariable* OnDemandCV = IConsoleManager::Get().FindConsoleVariable(TEXT("fx.Niagara.OnDemandCompileEnabled"));
    if (!OnDemandCV)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("cvar-absent"),
            TEXT("fx.Niagara.OnDemandCompileEnabled CVar is not registered; skipping deferred-flag test."));
        return true;
    }

    const FString Original = OnDemandCV->GetString();

    UNiagaraSystem* System = MakeTransientSystemForTest();
    TestNotNull(TEXT("Transient Niagara system created"), System);
    if (!System)
    {
        return false;
    }

    // Case A: CVar on -> field true and COMPILE_DEFERRED_ON_LOAD issue present.
    OnDemandCV->Set(TEXT("1"), ECVF_SetByCode);
    {
        TSharedPtr<FJsonObject> CompileJson = NiagaraDumpBuilder::BuildCompileDiagnosticsJson(System);
        TestTrue(TEXT("compile json contains compileDeferredOnLoad field (CVar on)"),
            CompileJson.IsValid() && CompileJson->HasField(TEXT("compileDeferredOnLoad")));
        TestTrue(TEXT("compileDeferredOnLoad is true when CVar is on"),
            CompileJson.IsValid() && CompileJson->GetBoolField(TEXT("compileDeferredOnLoad")));
        TestTrue(TEXT("issues contain COMPILE_DEFERRED_ON_LOAD when CVar is on"),
            IssuesContainCode(CompileJson, TEXT("COMPILE_DEFERRED_ON_LOAD")));
    }

    // Case B: CVar off -> field false and no COMPILE_DEFERRED_ON_LOAD issue.
    OnDemandCV->Set(TEXT("0"), ECVF_SetByCode);
    {
        TSharedPtr<FJsonObject> CompileJson = NiagaraDumpBuilder::BuildCompileDiagnosticsJson(System);
        TestTrue(TEXT("compile json contains compileDeferredOnLoad field (CVar off)"),
            CompileJson.IsValid() && CompileJson->HasField(TEXT("compileDeferredOnLoad")));
        TestFalse(TEXT("compileDeferredOnLoad is false when CVar is off"),
            CompileJson.IsValid() && CompileJson->GetBoolField(TEXT("compileDeferredOnLoad")));
        TestFalse(TEXT("issues do not contain COMPILE_DEFERRED_ON_LOAD when CVar is off"),
            IssuesContainCode(CompileJson, TEXT("COMPILE_DEFERRED_ON_LOAD")));
    }

    // Restore original CVar value.
    OnDemandCV->Set(*Original, ECVF_SetByCode);

    System->RemoveFromRoot();
    return true;
}

// Regression test for B-asset-dump-niagara-compile-state-stale-followup (System path):
// when scripts have not yet compiled (NCS_Unknown), the dump must report readyToRun as
// JSON null rather than a misleading boolean, surface a COMPILE_STATE_UNINITIALIZED
// info issue, and never serialize the literal string "NCS_Unknown" in any script's
// compileStatus field.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNiagaraDumpUninitializedSystemTest,
    "PinWright.Niagara.DumpCompile.UninitializedSystem",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraDumpUninitializedSystemTest::RunTest(const FString& Parameters)
{
    IConsoleVariable* OnDemandCV = IConsoleManager::Get().FindConsoleVariable(TEXT("fx.Niagara.OnDemandCompileEnabled"));
    if (!OnDemandCV)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("cvar-absent"),
            TEXT("fx.Niagara.OnDemandCompileEnabled CVar is not registered; skipping uninitialized-system test."));
        return true;
    }

    const FString Original = OnDemandCV->GetString();
    OnDemandCV->Set(TEXT("1"), ECVF_SetByCode);

    UNiagaraSystem* System = MakeTransientSystemForTest();
    TestNotNull(TEXT("Transient Niagara system created"), System);
    if (!System)
    {
        OnDemandCV->Set(*Original, ECVF_SetByCode);
        return false;
    }

    TSharedPtr<FJsonObject> CompileJson = NiagaraDumpBuilder::BuildCompileDiagnosticsJson(System);

    TestTrue(TEXT("readyToRun is JSON null when compile state is uninitialized"),
        JsonFieldIsNull(CompileJson, TEXT("readyToRun")));
    TestTrue(TEXT("issues contain COMPILE_STATE_UNINITIALIZED"),
        IssuesContainCode(CompileJson, TEXT("COMPILE_STATE_UNINITIALIZED")));

    // No script entry should serialize compileStatus as the literal string "NCS_Unknown".
    if (CompileJson.IsValid())
    {
        const TArray<TSharedPtr<FJsonValue>>* Scripts = nullptr;
        if (CompileJson->TryGetArrayField(TEXT("scripts"), Scripts) && Scripts)
        {
            for (const TSharedPtr<FJsonValue>& ScriptValue : *Scripts)
            {
                const TSharedPtr<FJsonObject> ScriptObj = ScriptValue.IsValid() ? ScriptValue->AsObject() : nullptr;
                if (!ScriptObj.IsValid())
                {
                    continue;
                }
                FString StatusString;
                if (ScriptObj->TryGetStringField(TEXT("compileStatus"), StatusString))
                {
                    TestFalse(TEXT("no script reports compileStatus == \"NCS_Unknown\""),
                        StatusString == TEXT("NCS_Unknown"));
                }
            }
        }
    }

    OnDemandCV->Set(*Original, ECVF_SetByCode);
    System->RemoveFromRoot();
    return true;
}

// Regression test for B-asset-dump-niagara-compile-state-stale-followup (Emitter path):
// the standalone-emitter compile dump must reach schema parity with the system path —
// it must include compileDeferredOnLoad, null out readyToRun when scripts are
// uninitialized, and emit the same COMPILE_STATE_UNINITIALIZED info issue.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNiagaraDumpUninitializedEmitterTest,
    "PinWright.Niagara.DumpCompile.UninitializedEmitter",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraDumpUninitializedEmitterTest::RunTest(const FString& Parameters)
{
    IConsoleVariable* OnDemandCV = IConsoleManager::Get().FindConsoleVariable(TEXT("fx.Niagara.OnDemandCompileEnabled"));
    if (!OnDemandCV)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("cvar-absent"),
            TEXT("fx.Niagara.OnDemandCompileEnabled CVar is not registered; skipping uninitialized-emitter test."));
        return true;
    }

    const FString Original = OnDemandCV->GetString();
    OnDemandCV->Set(TEXT("1"), ECVF_SetByCode);

    UNiagaraEmitter* Emitter = MakeTransientEmitterForTest();
    TestNotNull(TEXT("Transient Niagara emitter created"), Emitter);
    if (!Emitter)
    {
        OnDemandCV->Set(*Original, ECVF_SetByCode);
        return false;
    }

    TSharedPtr<FJsonObject> CompileJson = NiagaraDumpBuilder::BuildEmitterCompileDiagnosticsJson(Emitter);

    TestTrue(TEXT("emitter compile json contains compileDeferredOnLoad field"),
        CompileJson.IsValid() && CompileJson->HasField(TEXT("compileDeferredOnLoad")));
    TestTrue(TEXT("readyToRun is JSON null when emitter compile state is uninitialized"),
        JsonFieldIsNull(CompileJson, TEXT("readyToRun")));
    TestTrue(TEXT("emitter issues contain COMPILE_STATE_UNINITIALIZED"),
        IssuesContainCode(CompileJson, TEXT("COMPILE_STATE_UNINITIALIZED")));

    OnDemandCV->Set(*Original, ECVF_SetByCode);
    Emitter->RemoveFromRoot();
    return true;
}

// COMPILE_DEFERRED_ON_LOAD is raised only while the system's compile state is not current. A
// system compiled in this session keeps compileDeferredOnLoad=true (the session fact) and carries
// no issue. Counterfactual: raising the issue whenever the cvar is on fails the TestFalse below.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNiagaraDumpDeferredIssueClearsAfterCompileTest,
    "PinWright.Niagara.DumpCompile.DeferredIssueClearsAfterCompile",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraDumpDeferredIssueClearsAfterCompileTest::RunTest(const FString& Parameters)
{
    IConsoleVariable* OnDemandCV = IConsoleManager::Get().FindConsoleVariable(TEXT("fx.Niagara.OnDemandCompileEnabled"));
    if (!OnDemandCV)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("cvar-absent"),
            TEXT("fx.Niagara.OnDemandCompileEnabled CVar is not registered; skipping compiled-system test."));
        return true;
    }

    // A saved stock system with graphs intact; compiling a source-less synthetic system can crash the host.
    // RF_Standalone fixture: the owner keeps it alive, CleanupTestAsset detaches it. Declared before
    // the owner so it runs after it.
    FString SystemPath;
    ON_SCOPE_EXIT { CleanupTestAsset(SystemPath); };
    TStrongObjectPtr<UNiagaraSystem> SystemOwner(
        NiagaraEditTestUtils::DuplicateFixtureSystemWithEmitters(TEXT("NS_DeferredCompiled"), SystemPath));
    UNiagaraSystem* System = SystemOwner.Get();
    if (!System)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("fixture-missing"),
            FString::Printf(TEXT("could not duplicate '%s'"), NiagaraEditTestUtils::FixtureSystemAssetPath));
        return true;
    }

    System->RequestCompile(/*bForce=*/true);
    const PinWrightNiagara::FCompileWaitOutcome Wait =
        PinWrightNiagara::WaitForSystemCompile(*System, /*bMayFlushRequestCompile=*/true);
    TestFalse(TEXT("forced compile landed within the wait budget"), Wait.bTimedOut || Wait.bOutstanding);

    const FString Original = OnDemandCV->GetString();
    OnDemandCV->Set(TEXT("1"), ECVF_SetByCode);
    TSharedPtr<FJsonObject> CompileJson = NiagaraDumpBuilder::BuildCompileDiagnosticsJson(System);
    OnDemandCV->Set(*Original, ECVF_SetByCode);

    // Premise: the compile landed on the system scripts. Not UNiagaraSystem::IsReadyToRun(): its
    // IsReadyToRunInternal returns false whenever !FApp::CanEverRender() (-NullRHI), so it would
    // fail headless although the compile landed; the gate under test does not depend on rendering.
    const auto IsCompiledOk = [](const UNiagaraScript* Script)
    {
        const ENiagaraScriptCompileStatus Status = Script ? Script->GetLastCompileStatus() : ENiagaraScriptCompileStatus::NCS_Unknown;
        return Status == ENiagaraScriptCompileStatus::NCS_UpToDate
            || Status == ENiagaraScriptCompileStatus::NCS_UpToDateWithWarnings;
    };
    TestTrue(TEXT("premise: the forced compile left both system scripts up to date"),
        IsCompiledOk(System->GetSystemSpawnScript()) && IsCompiledOk(System->GetSystemUpdateScript()));
    TestTrue(TEXT("compileDeferredOnLoad still reports the session fact"),
        CompileJson.IsValid() && CompileJson->GetBoolField(TEXT("compileDeferredOnLoad")));
    TestFalse(TEXT("no COMPILE_DEFERRED_ON_LOAD issue for a system compiled in this session"),
        IssuesContainCode(CompileJson, TEXT("COMPILE_DEFERRED_ON_LOAD")));

    return true;
}

namespace TestNiagaraDumpCompileDeferredHelpers
{
    // Null entries marked compiledIntoSystemScripts, and unmarked entries not at a terminal success.
    void CountCompileEntries(const TArray<TSharedPtr<FJsonValue>>& Scripts, int32& OutNullFolded, int32& OutNonTerminalOwn)
    {
        OutNullFolded = 0;
        OutNonTerminalOwn = 0;
        for (const TSharedPtr<FJsonValue>& Value : Scripts)
        {
            const TSharedPtr<FJsonObject> Entry = Value.IsValid() ? Value->AsObject() : nullptr;
            if (!Entry.IsValid() || !Entry->HasField(TEXT("compileStatus")))
            {
                continue;
            }
            const bool bNull = Entry->TryGetField(TEXT("compileStatus"))->Type == EJson::Null;
            bool bFolded = false;
            if (Entry->TryGetBoolField(TEXT("compiledIntoSystemScripts"), bFolded) && bFolded)
            {
                OutNullFolded += bNull ? 1 : 0;
                continue;
            }
            FString Status;
            Entry->TryGetStringField(TEXT("compileStatus"), Status);
            if (bNull || (Status != TEXT("NCS_UpToDate") && Status != TEXT("NCS_UpToDateWithWarnings") && Status != TEXT("NCS_ComputeUpToDateWithWarnings")))
            {
                ++OutNonTerminalOwn;
            }
        }
    }
}

// E-niagara-validate-compile-state-uninitialized-undocumented: emitter spawn/update scripts are not
// compilable (the engine folds them into the system scripts and pins them at NCS_Unknown), so they
// stayed null after a completed compile and kept COMPILE_STATE_UNINITIALIZED and
// scriptCompileCheck:"unverified" on every system with an emitter, forever. Counterfactual: without
// the compiledIntoSystemScripts marker the precondition fails, and without the folds skipping it the
// issue, the null readyToRun and the unverified verdict all come back.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNiagaraDumpUninitializedIssueClearsAfterCompileTest,
    "PinWright.Niagara.DumpCompile.UninitializedIssueClearsAfterCompile",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraDumpUninitializedIssueClearsAfterCompileTest::RunTest(const FString& Parameters)
{
    FString SystemPath;
    ON_SCOPE_EXIT { CleanupTestAsset(SystemPath); };
    TStrongObjectPtr<UNiagaraSystem> SystemOwner(
        NiagaraEditTestUtils::DuplicateFixtureSystemWithEmitters(TEXT("NS_UninitCleared"), SystemPath));
    UNiagaraSystem* System = SystemOwner.Get();
    if (!System)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("fixture-missing"),
            FString::Printf(TEXT("could not duplicate '%s'"), NiagaraEditTestUtils::FixtureSystemAssetPath));
        return true;
    }

    System->RequestCompile(/*bForce=*/true);
    const PinWrightNiagara::FCompileWaitOutcome Wait =
        PinWrightNiagara::WaitForSystemCompile(*System, /*bMayFlushRequestCompile=*/true);
    if (!TestFalse(TEXT("premise: forced compile landed within the wait budget"), Wait.bTimedOut || Wait.bOutstanding))
    {
        return false;
    }

    const TSharedPtr<FJsonObject> CompileJson = NiagaraDumpBuilder::BuildCompileDiagnosticsJson(System);
    const TArray<TSharedPtr<FJsonValue>>* Scripts = nullptr;
    if (!TestTrue(TEXT("compile block has scripts"), CompileJson.IsValid() && CompileJson->TryGetArrayField(TEXT("scripts"), Scripts) && Scripts))
    {
        return false;
    }

    // Premises: the fixture has emitter scripts the engine left at NCS_Unknown after the compile,
    // and every script that compiles on its own reached a terminal status.
    int32 NullFoldedScripts = 0;
    int32 NonTerminalOwnScripts = 0;
    TestNiagaraDumpCompileDeferredHelpers::CountCompileEntries(*Scripts, NullFoldedScripts, NonTerminalOwnScripts);
    TestTrue(TEXT("premise: emitter spawn/update entries are marked and still null after the compile"), NullFoldedScripts > 0);
    TestEqual(TEXT("premise: every script that compiles on its own is up to date"), NonTerminalOwnScripts, 0);

    TestFalse(TEXT("no COMPILE_STATE_UNINITIALIZED after a completed compile"),
        IssuesContainCode(CompileJson, TEXT("COMPILE_STATE_UNINITIALIZED")));
    TestFalse(TEXT("readyToRun is measured, not null, after a completed compile"),
        JsonFieldIsNull(CompileJson, TEXT("readyToRun")));
    TestTrue(TEXT("verdict passes after a completed compile"),
        PinWrightNiagara::ReadCompileVerdict(CompileJson).Check == PinWrightNiagara::EScriptCompileCheck::Passed);

    // The same fold through niagara.compile_status, the verb the ticket saw stuck at unverified.
    TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
    Params->SetStringField(TEXT("assetPath"), SystemPath);
    FTestResponseCapture Capture;
    TestTrue(TEXT("niagara.compile_status handler found"),
        InvokeHandlerWithCapture(TEXT("niagara.compile_status"), Params, Capture));
    FString ScriptCheck;
    TestTrue(TEXT("compile_status succeeds with scriptCompileCheck"),
        Capture.bSuccess && Capture.Result.IsValid() && Capture.Result->TryGetStringField(TEXT("scriptCompileCheck"), ScriptCheck));
    TestEqual(TEXT("compile_status scriptCompileCheck passes after a completed compile"), ScriptCheck, FString(TEXT("passed")));

    // Emitter path (ScriptsHaveNonTerminalCompileStatus): the compiled system's own emitter has
    // up-to-date particle scripts and null emitter spawn/update scripts. Neither the deferred-compile
    // issue nor COMPILE_STATE_UNINITIALIZED may be raised for the null folded pair.
    const TArray<FNiagaraEmitterHandle>& Handles = System->GetEmitterHandles();
    UNiagaraEmitter* Emitter = Handles.Num() > 0 ? Handles[0].GetInstance().Emitter : nullptr;
    if (!TestNotNull(TEXT("premise: compiled fixture has an emitter"), Emitter))
    {
        return false;
    }
    IConsoleVariable* OnDemandCV = IConsoleManager::Get().FindConsoleVariable(TEXT("fx.Niagara.OnDemandCompileEnabled"));
    const FString Original = OnDemandCV ? OnDemandCV->GetString() : FString();
    if (OnDemandCV)
    {
        OnDemandCV->Set(TEXT("1"), ECVF_SetByCode);
    }
    const TSharedPtr<FJsonObject> EmitterJson = NiagaraDumpBuilder::BuildEmitterCompileDiagnosticsJson(Emitter);
    if (OnDemandCV)
    {
        OnDemandCV->Set(*Original, ECVF_SetByCode);
    }
    const TArray<TSharedPtr<FJsonValue>>* EmitterScripts = nullptr;
    if (!TestTrue(TEXT("emitter compile block has scripts"),
        EmitterJson.IsValid() && EmitterJson->TryGetArrayField(TEXT("scripts"), EmitterScripts) && EmitterScripts))
    {
        return false;
    }
    int32 EmitterNullFolded = 0;
    int32 EmitterNonTerminalOwn = 0;
    TestNiagaraDumpCompileDeferredHelpers::CountCompileEntries(*EmitterScripts, EmitterNullFolded, EmitterNonTerminalOwn);
    TestTrue(TEXT("premise: the emitter's spawn/update entries are marked and null"), EmitterNullFolded > 0);
    TestEqual(TEXT("premise: the emitter's particle scripts are up to date"), EmitterNonTerminalOwn, 0);
    if (OnDemandCV)
    {
        TestFalse(TEXT("no COMPILE_DEFERRED_ON_LOAD for an emitter whose only null scripts are folded"),
            IssuesContainCode(EmitterJson, TEXT("COMPILE_DEFERRED_ON_LOAD")));
    }
    TestFalse(TEXT("no COMPILE_STATE_UNINITIALIZED for an emitter whose only null scripts are folded"),
        IssuesContainCode(EmitterJson, TEXT("COMPILE_STATE_UNINITIALIZED")));
    return true;
}
