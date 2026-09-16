// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Misc/AutomationTest.h"
#include "Handlers/Niagara/NiagaraDumpBuilder.h"
#include "NiagaraJsonAssertionHelpers.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Tests/TestSkipReporting.h"


#include "HAL/IConsoleManager.h"
#include "NiagaraEmitter.h"
#include "NiagaraSystem.h"
#include "UObject/Package.h"
#include "Misc/Guid.h"

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
// niagara_compile.json must surface fx.Niagara.OnDemandCompile state via a
// compileDeferredOnLoad boolean and emit a COMPILE_DEFERRED_ON_LOAD info-severity
// issue when that CVar is enabled, so consumers can distinguish post-load
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
    IConsoleVariable* OnDemandCV = IConsoleManager::Get().FindConsoleVariable(TEXT("fx.Niagara.OnDemandCompile"));
    if (!OnDemandCV)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("cvar-absent"),
            TEXT("fx.Niagara.OnDemandCompile CVar is not registered; skipping deferred-flag test."));
        return true;
    }

    const bool bOriginal = OnDemandCV->GetBool();

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
    OnDemandCV->Set(bOriginal ? TEXT("1") : TEXT("0"), ECVF_SetByCode);

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
    IConsoleVariable* OnDemandCV = IConsoleManager::Get().FindConsoleVariable(TEXT("fx.Niagara.OnDemandCompile"));
    if (!OnDemandCV)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("cvar-absent"),
            TEXT("fx.Niagara.OnDemandCompile CVar is not registered; skipping uninitialized-system test."));
        return true;
    }

    const bool bOriginal = OnDemandCV->GetBool();
    OnDemandCV->Set(TEXT("1"), ECVF_SetByCode);

    UNiagaraSystem* System = MakeTransientSystemForTest();
    TestNotNull(TEXT("Transient Niagara system created"), System);
    if (!System)
    {
        OnDemandCV->Set(bOriginal ? TEXT("1") : TEXT("0"), ECVF_SetByCode);
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

    OnDemandCV->Set(bOriginal ? TEXT("1") : TEXT("0"), ECVF_SetByCode);
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
    IConsoleVariable* OnDemandCV = IConsoleManager::Get().FindConsoleVariable(TEXT("fx.Niagara.OnDemandCompile"));
    if (!OnDemandCV)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("cvar-absent"),
            TEXT("fx.Niagara.OnDemandCompile CVar is not registered; skipping uninitialized-emitter test."));
        return true;
    }

    const bool bOriginal = OnDemandCV->GetBool();
    OnDemandCV->Set(TEXT("1"), ECVF_SetByCode);

    UNiagaraEmitter* Emitter = MakeTransientEmitterForTest();
    TestNotNull(TEXT("Transient Niagara emitter created"), Emitter);
    if (!Emitter)
    {
        OnDemandCV->Set(bOriginal ? TEXT("1") : TEXT("0"), ECVF_SetByCode);
        return false;
    }

    TSharedPtr<FJsonObject> CompileJson = NiagaraDumpBuilder::BuildEmitterCompileDiagnosticsJson(Emitter);

    TestTrue(TEXT("emitter compile json contains compileDeferredOnLoad field"),
        CompileJson.IsValid() && CompileJson->HasField(TEXT("compileDeferredOnLoad")));
    TestTrue(TEXT("readyToRun is JSON null when emitter compile state is uninitialized"),
        JsonFieldIsNull(CompileJson, TEXT("readyToRun")));
    TestTrue(TEXT("emitter issues contain COMPILE_STATE_UNINITIALIZED"),
        IssuesContainCode(CompileJson, TEXT("COMPILE_STATE_UNINITIALIZED")));

    OnDemandCV->Set(bOriginal ? TEXT("1") : TEXT("0"), ECVF_SetByCode);
    Emitter->RemoveFromRoot();
    return true;
}
