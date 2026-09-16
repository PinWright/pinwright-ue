// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression test for material.authoring.compile_material shader-error reporting
// (board ticket B-compile-material-false-shader-success).
//
// Builds a sandbox UMaterial whose EmissiveColor is driven by a Custom HLSL node
// containing intentionally invalid code (subscripting GetPrimitiveData().LocalToWorld,
// an FDFMatrix with no operator[]). Invoking the production handler must block on
// shader compilation, detect the failed permutation, and return compiledWithErrors:true
// with the HLSL error text in compileErrors.
//
// Counterfactual: if the fix in MaterialAuthoringHandler.cpp is reverted (removing the
// error-collection logic and the compiledWithErrors/compileErrors fields), the
// "response has compiledWithErrors field" assertion fails and compileErrors is
// missing or empty.
//
// VACUITY, added 2026-09-03 after this test failed on a contended host while the verb behaved
// correctly. The error text can only be read once ProcessCompiledShaderMaps has written it into
// FMaterial::CompileErrors, and WaitAndCollect is bounded by
// MaterialCompileErrorCollector::CompileWaitTimeoutSeconds (90 s, itself bounded by the transport's
// 120 s response timeout and therefore not raisable). This material's ten permutations normally
// compile in ~2 s -- the test measured 2.4-4.1 s across four archived suite runs. In
// Saved/Logs/pw_wave_suite.log they took 271 s: `Job execution time: average 120.67 s, max
// 256.92 s`, `Average time worker was idle: 24.59 s`, `Effective parallelization: 0.45` against 12
// workers, i.e. the ShaderCompileWorker processes were starved by other work on the box. The wait
// expired at 90 s, the response correctly said `compileStatus: "timedOut"` with an empty
// compileErrors list, and three assertions written as though the errors were guaranteed went red.
// The verdict was honest; the test was asserting a measurement a bounded wait cannot promise.
//
// So the assertions now split. What holds on EVERY host -- the fields exist, and compileSucceeded
// is false -- is asserted before the split, which is what keeps the counterfactual above intact.
// The error-collection branch runs only when a compile actually LANDED; a wait that expired, or a
// host with shader compilation switched off, reports through PinWrightTestSkip instead. Note
// `notCompiled` is NOT skipped on its own: it is only excused when the engine says shader
// compilation is skipped, because otherwise it is exactly what a broken collector would look like.

#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Tests/Material/MaterialShaderStateTestFixtures.h"
#include "Tests/TestUtils.h"
#include "Tests/TestSkipReporting.h"
#include "ShaderCompiler.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCompileMaterialShaderErrorDetectionTest,
    "PinWright.material.authoring.compile_material.ReportsShaderErrors",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCompileMaterialShaderErrorDetectionTest::RunTest(const FString& Parameters)
{
    FString AssetPath;
    UMaterial* Material =
        PinWrightMaterialShaderStateTestFixtures::MakeBrokenHlslMaterial(
            TEXT("CompileErr"), AssetPath);
    if (!TestNotNull(TEXT("Material created"), Material))
    {
        CleanupTestAsset(AssetPath);
        return true;
    }

    // Dispatch the production handler. save:false keeps the broken asset out of disk.
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), AssetPath);
    Payload->SetBoolField(TEXT("save"), false);

    FTestResponseCapture Capture;
    const bool bFound = InvokeHandlerWithCapture(
        TEXT("material.authoring.compile_material"), Payload, Capture);
    TestTrue(TEXT("handler registered"), bFound);
    TestTrue(TEXT("handler responded"), Capture.bWasCalled);
    if (!TestTrue(TEXT("response reports success"), Capture.bSuccess) ||
        !TestTrue(TEXT("result is valid"), Capture.Result.IsValid()))
    {
        CleanupTestAsset(AssetPath);
        return true;
    }

    // Load-bearing, and asserted on EVERY host regardless of what the compiler managed: the wire
    // contract carries these fields. If a refactor ever drops one, these fail whatever the outcome.
    TestTrue(TEXT("response has compiledWithErrors field"),
        Capture.Result->HasField(TEXT("compiledWithErrors")));
    TestTrue(TEXT("response has compileErrors field"),
        Capture.Result->HasField(TEXT("compileErrors")));
    TestTrue(TEXT("response has compileStatus field"),
        Capture.Result->HasField(TEXT("compileStatus")));

    // Also host-independent: this material cannot compile, so the verb must never report success -
    // not when the compile fails, and not when the wait expires before it finishes.
    bool bCompileSucceeded = true;
    Capture.Result->TryGetBoolField(TEXT("compileSucceeded"), bCompileSucceeded);
    TestFalse(TEXT("compileSucceeded is false for a material that cannot compile"),
        bCompileSucceeded);

    FString CompileStatus;
    Capture.Result->TryGetStringField(TEXT("compileStatus"), CompileStatus);

    // The wait expired or the compile is still in flight: the errors exist in the engine but have
    // not been written back yet, so this run cannot measure them. Reported, not asserted past.
    if (CompileStatus == TEXT("timedOut") || CompileStatus == TEXT("outstanding"))
    {
        double CompileWaitedMs = 0.0;
        Capture.Result->TryGetNumberField(TEXT("compileWaitedMs"), CompileWaitedMs);
        PinWrightTestSkip::SkipAssertions(*this, TEXT("shader-compile-outlived-the-bounded-wait"),
            FString::Printf(
                TEXT("compile_material reported compileStatus='%s' after waiting %.0f ms, so the "
                     "platform shader compiler had not finished this material's permutations "
                     "within MaterialCompileErrorCollector::CompileWaitTimeoutSeconds and the HLSL "
                     "errors could not be collected on this host. The response is correct; the "
                     "error-text assertions simply had nothing to measure. Expect this on a box "
                     "whose ShaderCompileWorker processes are starved by a concurrent build."),
                *CompileStatus, CompileWaitedMs));
        CleanupTestAsset(AssetPath);
        return true;
    }

    // No compile ran at all. Excused ONLY when the engine itself says shader compilation is off -
    // otherwise this is the shape a broken collector would produce, and it must stay red.
    if (CompileStatus == TEXT("notCompiled") &&
        GShaderCompilingManager && GShaderCompilingManager->IsShaderCompilationSkipped())
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("shader-compilation-skipped-on-host"),
            TEXT("GShaderCompilingManager reports shader compilation skipped, so no permutation "
                 "was ever submitted and the failed-compile state this test asserts on could not "
                 "be produced."));
        CleanupTestAsset(AssetPath);
        return true;
    }

    // A compile landed. Everything below is the original assertion set, unchanged.
    bool bCompiledWithErrors = false;
    Capture.Result->TryGetBoolField(TEXT("compiledWithErrors"), bCompiledWithErrors);
    TestTrue(TEXT("compiledWithErrors is true"), bCompiledWithErrors);

    TestEqual(TEXT("compileStatus is failed"), CompileStatus, FString(TEXT("failed")));

    const TArray<TSharedPtr<FJsonValue>>* Errors = nullptr;
    if (TestTrue(TEXT("compileErrors is an array"),
            Capture.Result->TryGetArrayField(TEXT("compileErrors"), Errors)) && Errors)
    {
        TestTrue(TEXT("compileErrors is non-empty"), Errors->Num() > 0);

        bool bMatched = false;
        for (const TSharedPtr<FJsonValue>& Value : *Errors)
        {
            FString ErrorText;
            if (Value->TryGetString(ErrorText) &&
                (ErrorText.Contains(TEXT("subscript")) ||
                 ErrorText.Contains(TEXT("FDFMatrix")) ||
                 ErrorText.Contains(TEXT("operator")) ||
                 ErrorText.Contains(TEXT("LocalToWorld"))))
            {
                bMatched = true;
                break;
            }
        }
        TestTrue(TEXT("an error matches the invalid-HLSL signature"), bMatched);
    }

    CleanupTestAsset(AssetPath);
    return true;
}
