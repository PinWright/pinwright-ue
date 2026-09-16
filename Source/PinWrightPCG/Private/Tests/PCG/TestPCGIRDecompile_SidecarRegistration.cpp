// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression test for F-pcg-decompile-ir dual-surface invariant.
//
// Verifies that the asset-dump sidecar layer carries an entry for
// UPCGGraph -> pcgir.txt. The plugin's IrSidecarRegistry drives both
// the dump dispatch (AssetDumpHandler.cpp:328) and folder-sweep
// manifest reconciliation, so the registration is the single source
// of truth for sidecar parity. Also invokes the registered build
// function on a transient graph to confirm it produces non-empty
// text without an orchestrator.
//
// Counterfactual: if the REGISTER_DECOMPILE_IR(pcgir.graph, ...)
// line in PCGDecompileHandler.cpp is removed, the dump sidecar
// silently vanishes — this test catches the regression without
// needing a full asset.dump integration test.
#if WITH_DEV_AUTOMATION_TESTS && __has_include("PCGGraph.h")

#include "Misc/AutomationTest.h"

#include "Handlers/Asset/AssetDumpHandler.h"
#include "Utils/IrSidecarRegistry.h"
#include "PCGGraph.h"
#include "UObject/Package.h"


IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPCGIRSidecar_RegisteredAndDispatched,
    "PinWright.pcgir.decompile.SidecarRegistration",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPCGIRSidecar_RegisteredAndDispatched::RunTest(const FString& Parameters)
{
    const TArray<IrSidecarRegistry::FIrSidecarSpec> Specs = IrSidecarRegistry::GetRegisteredIrSidecars();

    const IrSidecarRegistry::FIrSidecarSpec* MatchingSpec = nullptr;
    for (const IrSidecarRegistry::FIrSidecarSpec& Spec : Specs)
    {
        if (Spec.FileName && FCString::Strcmp(Spec.FileName, DumpFileNames::PcgIr) == 0
            && Spec.ClassFn && Spec.ClassFn() == UPCGGraph::StaticClass())
        {
            MatchingSpec = &Spec;
            break;
        }
    }

    TestNotNull(TEXT("Registry contains a UPCGGraph -> pcgir.txt sidecar entry"), MatchingSpec);
    if (!MatchingSpec || !MatchingSpec->BuildFn) return false;

    UPCGGraph* Graph = NewObject<UPCGGraph>(GetTransientPackage());
    TestNotNull(TEXT("Transient UPCGGraph created"), Graph);
    if (!Graph) return false;

    const IrSidecarRegistry::FIrSidecarResult Result = MatchingSpec->BuildFn(Graph);
    TestTrue(TEXT("Registered build function reports success on transient graph"), Result.bSuccess);
    TestFalse(TEXT("Registered build function produced non-empty text"), Result.Text.IsEmpty());

    return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS && __has_include("PCGGraph.h")
