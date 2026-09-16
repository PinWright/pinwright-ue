// Copyright (c) 2026 Alexander Penkin. MIT License.

// niagara.compile_status published `outstandingIncludesGpuShaders: true` as the only GPU signal.
// That field is an honest statement about the queue check's scope, but read as a measurement it
// accused a CPUSim-only system of pending GPU shader work and sent a reviewer after a blocker that
// asset cannot have. The measurement now has its own field, and this pins both.

#include "Misc/AutomationTest.h"

#include "Tests/Assets/NiagaraEditTestUtils.h"
#include "Tests/TestSkipReporting.h"
#include "Tests/TestUtils.h"

#include "Dom/JsonObject.h"
#include "Misc/ScopeExit.h"
#include "NiagaraEmitter.h"
#include "NiagaraEmitterHandle.h"
#include "NiagaraScript.h"
#include "NiagaraSystem.h"
#include "UObject/StrongObjectPtr.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace PinWrightNiagaraCompileStatusGpuScopeTest
{
    // Independent oracle, walked here rather than through the production helper so the assertion
    // is not the fix checking itself. It pins the production contract exactly: an emitter counts
    // only when it declares GPU simulation AND owns the compute script that work would compile.
    bool SystemDeclaresGpuSimulation(const UNiagaraSystem& System)
    {
        for (const FNiagaraEmitterHandle& Handle : System.GetEmitterHandles())
        {
            const FVersionedNiagaraEmitterData* EmitterData = Handle.GetEmitterData();
            if (EmitterData
                && EmitterData->SimTarget == ENiagaraSimTarget::GPUComputeSim
                && EmitterData->GetGPUComputeScript() != nullptr)
            {
                return true;
            }
        }
        return false;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNiagaraCompileStatusGpuScopeTest,
    "PinWright.niagara.CompileStatus.GpuShaderScopeIsMeasured",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraCompileStatusGpuScopeTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightNiagaraCompileStatusGpuScopeTest;

    FString SystemPath;
    ON_SCOPE_EXIT
    {
        CleanupTestAsset(SystemPath);
    };

    TStrongObjectPtr<UNiagaraSystem> SystemOwner(
        NiagaraEditTestUtils::DuplicateFixtureSystemWithEmitters(
            TEXT("NS_CompileStatus"), SystemPath));
    UNiagaraSystem* System = SystemOwner.Get();
    if (!System)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("niagara-fixture-system-unavailable"),
            FString::Printf(TEXT("Could not duplicate '%s'"),
                NiagaraEditTestUtils::FixtureSystemAssetPath));
        return true;
    }

    TestTrue(TEXT("the fixture kept its emitters"), System->GetEmitterHandles().Num() > 0);
    const bool bDeclaresGpuSimulation = SystemDeclaresGpuSimulation(*System);
    TestFalse(TEXT("the stock fixture is CPU-only"), bDeclaresGpuSimulation);

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), SystemPath);
    FTestResponseCapture Status;
    TestTrue(TEXT("niagara.compile_status handler found"),
        InvokeHandlerWithCapture(TEXT("niagara.compile_status"), Payload, Status));
    TestTrue(FString::Printf(TEXT("compile_status answers for the fixture (errorCode='%s')"),
        *Status.ErrorCode), Status.bSuccess);
    if (!Status.bSuccess || !Status.Result.IsValid())
    {
        return false;
    }

    // The scope statement stays true on every asset: the queue check really does run GPU-inclusive.
    bool bReportedGpuScope = false;
    TestTrue(TEXT("compile_status states the queue check's scope"),
        Status.Result->TryGetBoolField(TEXT("outstandingIncludesGpuShaders"), bReportedGpuScope));
    TestTrue(TEXT("the queue check is GPU-inclusive whatever the asset carries"), bReportedGpuScope);

    // The measurement is the asset's own, and is what a caller should read before blaming GPU work.
    bool bReportedGpuSimulation = true;
    TestTrue(TEXT("compile_status measures the asset's GPU simulation"),
        Status.Result->TryGetBoolField(TEXT("hasGpuSimulation"), bReportedGpuSimulation));
    TestTrue(TEXT("the reported GPU simulation state is the asset's own"),
        bReportedGpuSimulation == bDeclaresGpuSimulation);
    TestFalse(TEXT("a CPU-only system reports no GPU simulation"), bReportedGpuSimulation);

    return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
