// Copyright (c) 2026 Alexander Penkin. MIT License.

// F-niagara-get-compiled-script: niagara.get_compiled_script reads the translator / compiler output
// of a script. Asserted: stats only by default; hlsl/assembly on request with maxChars truncation
// reporting truncated + totalChars; text the engine did not retain is reported as <field>Missing,
// never ""; forceCompile fills the transient CPU HLSL and leaves the package dirty flag as found
// (both starting states); a GPU emitter returns its GPU HLSL and a permutation count.

#include "Misc/AutomationTest.h"
#include "Tests/Assets/NiagaraEditTestUtils.h"
#include "Tests/TestSkipReporting.h"
#include "Tests/TestUtils.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/ScopeExit.h"
#include "NiagaraEmitter.h"
#include "NiagaraEmitterHandle.h"
#include "NiagaraScript.h"
#include "NiagaraSystem.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace NiagaraGetCompiledScriptTest
{
    // Emitter narrows the selection: the stock fixture has several emitters, each with its own
    // ParticleUpdate script, so a usage filter alone matches one script per emitter.
    TSharedPtr<FJsonObject> MakePayload(const FString& AssetPath, const TCHAR* ScriptUsage, std::initializer_list<const TCHAR*> Include, bool bForce, const FString& Emitter = FString())
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("assetPath"), AssetPath);
        if (!Emitter.IsEmpty())
        {
            Payload->SetStringField(TEXT("emitter"), Emitter);
        }
        if (ScriptUsage)
        {
            Payload->SetStringField(TEXT("scriptUsage"), ScriptUsage);
        }
        if (Include.size() > 0)
        {
            TArray<TSharedPtr<FJsonValue>> Values;
            for (const TCHAR* Item : Include)
            {
                Values.Add(MakeShared<FJsonValueString>(Item));
            }
            Payload->SetArrayField(TEXT("include"), Values);
        }
        Payload->SetBoolField(TEXT("forceCompile"), bForce);
        return Payload;
    }

    TSharedPtr<FJsonObject> OnlyScript(FAutomationTestBase& Test, const FTestResponseCapture& Capture)
    {
        const TArray<TSharedPtr<FJsonValue>>& Scripts = Capture.Result->GetArrayField(TEXT("scripts"));
        Test.TestEqual(TEXT("exactly one script matched"), Scripts.Num(), 1);
        return Scripts.Num() == 1 ? Scripts[0]->AsObject() : nullptr;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNiagaraGetCompiledScriptTextTest,
    "PinWright.niagara.get_compiled_script.StatsByDefaultTextOnRequest",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraGetCompiledScriptTextTest::RunTest(const FString& Parameters)
{
    using namespace NiagaraGetCompiledScriptTest;

    FString SystemPath;
    ON_SCOPE_EXIT { CleanupTestAsset(SystemPath); };
    TStrongObjectPtr<UNiagaraSystem> Owner(NiagaraEditTestUtils::DuplicateFixtureSystemWithEmitters(TEXT("NS_CompiledScript"), SystemPath));
    if (!Owner.IsValid())
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("niagara-fixture-system-unavailable"), NiagaraEditTestUtils::FixtureSystemAssetPath);
        return true;
    }
    const FString Emitter = Owner->GetEmitterHandles().Num() > 0
        ? Owner->GetEmitterHandles()[0].GetName().ToString() : FString();

    // include omitted: stats only, no text fields at all.
    FTestResponseCapture Stats;
    if (!NiagaraEditTestUtils::InvokeExpectSuccess(*this, TEXT("niagara.get_compiled_script"),
            MakePayload(SystemPath, TEXT("ParticleUpdate"), {}, false, Emitter), Stats))
    {
        return false;
    }
    if (TSharedPtr<FJsonObject> Script = OnlyScript(*this, Stats))
    {
        TestTrue(TEXT("stats: byteCodeBytes"), Script->HasField(TEXT("byteCodeBytes")));
        TestTrue(TEXT("stats: numTempRegisters"), Script->HasField(TEXT("numTempRegisters")));
        TestTrue(TEXT("stats: compileEvents"), Script->HasField(TEXT("compileEvents")));
        TestFalse(TEXT("no hlsl without include"), Script->HasField(TEXT("hlsl")) || Script->HasField(TEXT("hlslMissing")));
        TestFalse(TEXT("no assembly without include"), Script->HasField(TEXT("assembly")) || Script->HasField(TEXT("assemblyMissing")));
    }

    // Before any compile in this session the transient CPU HLSL may be absent: then it must be
    // reported as hlslMissing with a reason, never as an empty string.
    FTestResponseCapture Cold;
    if (NiagaraEditTestUtils::InvokeExpectSuccess(*this, TEXT("niagara.get_compiled_script"),
            MakePayload(SystemPath, TEXT("ParticleUpdate"), { TEXT("hlsl") }, false, Emitter), Cold))
    {
        if (TSharedPtr<FJsonObject> Script = OnlyScript(*this, Cold))
        {
            FString Hlsl;
            if (Script->TryGetStringField(TEXT("hlsl"), Hlsl))
            {
                TestFalse(TEXT("a present hlsl is never empty"), Hlsl.IsEmpty());
            }
            else
            {
                const TSharedPtr<FJsonObject>* Missing = nullptr;
                TestTrue(TEXT("absent hlsl is reported as hlslMissing"), Script->TryGetObjectField(TEXT("hlslMissing"), Missing));
                TestFalse(TEXT("hlslMissing names a reason"), Missing && (*Missing)->GetStringField(TEXT("reason")).IsEmpty());
            }
            TestFalse(TEXT("stats were not requested"), Script->HasField(TEXT("byteCodeBytes")));
        }
    }

    // forceCompile fills the CPU HLSL; maxChars truncates it and says so.
    TSharedPtr<FJsonObject> Truncating = MakePayload(SystemPath, TEXT("ParticleUpdate"), { TEXT("hlsl"), TEXT("assembly") }, true, Emitter);
    Truncating->SetNumberField(TEXT("maxChars"), 64);
    FTestResponseCapture Forced;
    if (NiagaraEditTestUtils::InvokeExpectSuccess(*this, TEXT("niagara.get_compiled_script"), Truncating, Forced))
    {
        TestTrue(TEXT("forcedCompile reported"), Forced.Result->GetBoolField(TEXT("forcedCompile")));
        if (TSharedPtr<FJsonObject> Script = OnlyScript(*this, Forced))
        {
            FString Hlsl;
            TestTrue(TEXT("forceCompile populated the CPU hlsl"), Script->TryGetStringField(TEXT("hlsl"), Hlsl));
            TestEqual(TEXT("hlsl is cut at maxChars"), Hlsl.Len(), 64);
            TestTrue(TEXT("hlslTruncated"), Script->GetBoolField(TEXT("hlslTruncated")));
            TestTrue(TEXT("hlslTotalChars exceeds maxChars"), Script->GetIntegerField(TEXT("hlslTotalChars")) > 64);
        }
    }

    NiagaraEditTestUtils::InvokeExpectError(*this, TEXT("niagara.get_compiled_script"),
        MakePayload(SystemPath, TEXT("NotAUsage"), {}, false), TEXT("TARGET_NOT_FOUND"));
    NiagaraEditTestUtils::InvokeExpectError(*this, TEXT("niagara.get_compiled_script"),
        MakePayload(SystemPath, nullptr, { TEXT("everything") }, false), TEXT("INVALID_ARGUMENT"));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNiagaraGetCompiledScriptDirtyTest,
    "PinWright.niagara.get_compiled_script.ForceCompilePreservesDirtyFlag",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraGetCompiledScriptDirtyTest::RunTest(const FString& Parameters)
{
    using namespace NiagaraGetCompiledScriptTest;

    FString SystemPath;
    ON_SCOPE_EXIT { CleanupTestAsset(SystemPath); };
    TStrongObjectPtr<UNiagaraSystem> Owner(NiagaraEditTestUtils::DuplicateFixtureSystemWithEmitters(TEXT("NS_CompiledDirty"), SystemPath));
    if (!Owner.IsValid())
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("niagara-fixture-system-unavailable"), NiagaraEditTestUtils::FixtureSystemAssetPath);
        return true;
    }
    const FString Emitter = Owner->GetEmitterHandles().Num() > 0
        ? Owner->GetEmitterHandles()[0].GetName().ToString() : FString();

    // RF_Transient makes MarkPackageDirty a no-op, which would hide the dirtying this test is
    // about. Cleared for the measurement and restored, clean, afterwards (no early return between).
    UPackage* const Package = Owner->GetOutermost();
    Package->ClearFlags(RF_Transient);

    // Clean stays clean.
    Package->SetDirtyFlag(false);
    FTestResponseCapture Clean;
    NiagaraEditTestUtils::InvokeExpectSuccess(*this, TEXT("niagara.get_compiled_script"),
        MakePayload(SystemPath, TEXT("ParticleUpdate"), { TEXT("hlsl") }, true, Emitter), Clean);
    TestFalse(TEXT("a forced compile leaves a clean package clean"), Package->IsDirty());
    if (Clean.Result.IsValid())
    {
        if (TSharedPtr<FJsonObject> Script = OnlyScript(*this, Clean))
        {
            FString Hlsl;
            TestTrue(TEXT("the forced compile produced CPU hlsl"), Script->TryGetStringField(TEXT("hlsl"), Hlsl) && !Hlsl.IsEmpty());
        }
    }

    // Dirty stays dirty: preserve, not clear.
    Package->SetDirtyFlag(true);
    FTestResponseCapture Dirty;
    NiagaraEditTestUtils::InvokeExpectSuccess(*this, TEXT("niagara.get_compiled_script"),
        MakePayload(SystemPath, TEXT("ParticleUpdate"), {}, true, Emitter), Dirty);
    TestTrue(TEXT("a forced compile leaves a dirty package dirty"), Package->IsDirty());

    Package->SetDirtyFlag(false);
    Package->SetFlags(RF_Transient);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNiagaraGetCompiledScriptGpuTest,
    "PinWright.niagara.get_compiled_script.GpuEmitterReportsHlslAndPermutations",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraGetCompiledScriptGpuTest::RunTest(const FString& Parameters)
{
    using namespace NiagaraGetCompiledScriptTest;

    FString SystemPath;
    ON_SCOPE_EXIT { CleanupTestAsset(SystemPath); };
    TStrongObjectPtr<UNiagaraSystem> Owner(NiagaraEditTestUtils::DuplicateFixtureSystemWithEmitters(TEXT("NS_CompiledGpu"), SystemPath));
    if (!Owner.IsValid() || Owner->GetEmitterHandles().Num() == 0 || !Owner->GetEmitterHandles()[0].GetEmitterData())
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("niagara-fixture-system-unavailable"), NiagaraEditTestUtils::FixtureSystemAssetPath);
        return true;
    }
    // The stock fixture is CPU; switch its emitter to GPU so the compile produces the compute script.
    Owner->GetEmitterHandles()[0].GetEmitterData()->SimTarget = ENiagaraSimTarget::GPUComputeSim;

    FTestResponseCapture Gpu;
    if (!NiagaraEditTestUtils::InvokeExpectSuccess(*this, TEXT("niagara.get_compiled_script"),
            MakePayload(SystemPath, TEXT("ParticleGPUComputeScript"), { TEXT("stats"), TEXT("hlsl") }, true), Gpu))
    {
        return false;
    }
    if (TSharedPtr<FJsonObject> Script = OnlyScript(*this, Gpu))
    {
        TestEqual(TEXT("simTarget is GPU"), Script->GetStringField(TEXT("simTarget")), FString(TEXT("GPU")));
        FString Hlsl;
        TestTrue(TEXT("the GPU compute script returns non-empty hlsl"), Script->TryGetStringField(TEXT("hlsl"), Hlsl) && !Hlsl.IsEmpty());
        const TSharedPtr<FJsonObject>* GpuStats = nullptr;
        if (TestTrue(TEXT("gpu stats block present"), Script->TryGetObjectField(TEXT("gpu"), GpuStats)))
        {
            TestTrue(TEXT("a permutation count is reported"), (*GpuStats)->GetIntegerField(TEXT("permutations")) >= 1);
        }
    }
    return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
