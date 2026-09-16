// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression test for board B-compile-material-blocks-and-mislabels, honesty half.
//
// THE DEFECT. material.authoring.compile_material derived its verdict from ONE measurement:
// `compileSucceeded` was `CompileErrors.Num() == 0`. An empty error list is produced by a clean
// compile - and equally by a compile that never happened. So a call that submitted no shader
// jobs at all returned `compiled: true, compileSucceeded: true, compileErrors: []`, which is the
// verb reporting the REQUEST rather than the OUTCOME. Same shape as
// B-niagara-compile-wait-does-not-wait, whose fix introduced
// PinWrightNiagara::DidCompileLand for exactly this distinction.
//
// THE FIXTURE, and why it is not faked. FShaderCompilingManager::SkipShaderCompilation(true) is
// the engine's own switch for "do not compile shaders" (ShaderCompiler.h). With it set,
// FMaterial::BeginCompileShaderMap computes bSkipCompilationForODSC and forces PrecompileMode to
// None, so the material is translated to HLSL and NO permutation jobs are submitted
// (MaterialShared.cpp). The engine then still calls SetGameThreadShaderMap with a map holding
// zero shaders - "We didn't compile any shaders but still assign the result" - which is why a
// non-null shader map is not evidence of a compile and IsGameThreadShaderMapComplete() is. This
// is a real editor state, not a stub: nothing about the handler, the collector or the response
// is mocked, and the verb runs its whole production body.
//
// COUNTERFACTUAL. Revert MaterialCompileErrorCollector::WaitAndCollect to `void` and
// compileSucceeded to `CompileErrors.Num() == 0`, and "compileSucceeded is false when no compile
// landed" fails: the reverted handler reports true beside an empty error list for a material the
// engine never compiled. "response carries compileStatus" fails outright, the field not existing.
//
// VACUITY. If this host produces a complete shader map anyway - SkipShaderCompilation is a no-op
// when AllowShaderCompiling() is false - the fixture did not create the condition under test, and
// the test says so through PinWrightTestSkip rather than asserting nothing quietly. The invariant
// assertion below still runs in that case, because it holds in both directions.

#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Tests/TestUtils.h"
#include "Tests/TestSkipReporting.h"
#include "Handlers/Material/MaterialCompileErrorCollector.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Materials/Material.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "ShaderCompiler.h"
#include "UObject/Package.h"

namespace
{
    // Restores the engine's shader-compilation switch on every exit path, including an early
    // return from a failed assertion. Leaving it set would silently disable shader compilation
    // for every test that runs after this one in the same editor.
    struct FScopedSkipShaderCompilation
    {
        bool bPreviouslySkipped = false;

        FScopedSkipShaderCompilation()
        {
            if (GShaderCompilingManager)
            {
                bPreviouslySkipped = GShaderCompilingManager->IsShaderCompilationSkipped();
                GShaderCompilingManager->SkipShaderCompilation(true);
            }
        }

        ~FScopedSkipShaderCompilation()
        {
            if (GShaderCompilingManager)
            {
                GShaderCompilingManager->SkipShaderCompilation(bPreviouslySkipped);
            }
        }

        bool IsActive() const
        {
            return GShaderCompilingManager && GShaderCompilingManager->IsShaderCompilationSkipped();
        }
    };
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCompileMaterialOutcomeHonestyTest,
    "PinWright.material.authoring.compile_material.DoesNotClaimSuccessWhenNoCompileLanded",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCompileMaterialOutcomeHonestyTest::RunTest(const FString& Parameters)
{
    if (!GShaderCompilingManager)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-shader-compiling-manager"),
            TEXT("GShaderCompilingManager is null on this host, so the no-compile state this "
                 "test asserts against cannot be created."));
        return true;
    }

    // Set BEFORE the material exists, so no shader map is ever produced for it - not by the
    // creation PostEditChange and not by the handler's own CacheShaders.
    FScopedSkipShaderCompilation SkipGuard;
    if (!SkipGuard.IsActive())
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("shader-compilation-not-skippable"),
            TEXT("FShaderCompilingManager::SkipShaderCompilation did not take (AllowShaderCompiling "
                 "is false on this host), so the no-compile state this test asserts against could "
                 "not be created."));
        return true;
    }

    const FString AssetPath = FString::Printf(
        TEXT("/Game/__PW_GatewayTests/CompileOutcome_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));

    UPackage* Pkg = CreatePackage(*AssetPath);
    if (!TestNotNull(TEXT("Package created"), Pkg))
        return true;

    // A perfectly valid material. Nothing here can fail to compile - the only reason it will not
    // compile is the engine switch above, which is the whole point: the response must not read
    // the absence of errors as a pass.
    UMaterial* Material = NewObject<UMaterial>(
        Pkg,
        FName(*FPackageName::GetLongPackageAssetName(AssetPath)),
        RF_Public | RF_Standalone);

    if (!TestNotNull(TEXT("Material created"), Material))
    {
        CleanupTestAsset(AssetPath);
        return true;
    }

    Material->PostEditChange();
    FAssetRegistryModule::AssetCreated(Material);

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

    bool bCompileSucceeded = false;
    Capture.Result->TryGetBoolField(TEXT("compileSucceeded"), bCompileSucceeded);

    const TArray<TSharedPtr<FJsonValue>>* Errors = nullptr;
    const bool bHasErrorArray = Capture.Result->TryGetArrayField(TEXT("compileErrors"), Errors);
    const int32 NumErrors = (bHasErrorArray && Errors) ? Errors->Num() : 0;

    // Measured independently of the response, through the same accessor the collector reads its
    // verdict off, so the assertion compares what the verb SAID against what the engine DID.
    const bool bMaterialHasCompleteShaderMap =
        MaterialCompileErrorCollector::HasCompleteShaderMap(Material);

    // The invariant, asserted on every host regardless of what the fixture achieved: a reported
    // success must be backed by a shader map the engine considers complete and by an empty error
    // list. This is the contract; everything below is the specific case that used to break it.
    if (bCompileSucceeded)
    {
        TestTrue(TEXT("compileSucceeded implies the engine installed a complete shader map"),
            bMaterialHasCompleteShaderMap);
        TestEqual(TEXT("compileSucceeded implies no compile errors"), NumErrors, 0);
    }

    if (!bMaterialHasCompleteShaderMap)
    {
        // The state under test: no compile landed. This is where the old handler reported
        // compileSucceeded:true on an empty error list.
        TestEqual(TEXT("a compile that did not land reports no errors"), NumErrors, 0);
        TestFalse(TEXT("compileSucceeded is false when no compile landed"), bCompileSucceeded);

        FString CompileStatus;
        if (TestTrue(TEXT("response carries compileStatus"),
                Capture.Result->TryGetStringField(TEXT("compileStatus"), CompileStatus)))
        {
            TestNotEqual(TEXT("compileStatus is not completed when no compile landed"),
                CompileStatus, FString(TEXT("completed")));
            TestNotEqual(TEXT("compileStatus is not failed when there are no errors"),
                CompileStatus, FString(TEXT("failed")));
        }

        // A response that merely goes quiet is the same defect one layer down: the caller has to
        // be told, in the response, that the empty error list proves nothing.
        const TArray<TSharedPtr<FJsonValue>>* Warnings = nullptr;
        if (TestTrue(TEXT("a compile that did not land carries a warning"),
                Capture.Result->TryGetArrayField(TEXT("warnings"), Warnings)) && Warnings)
        {
            bool bNamesTheStatus = false;
            for (const TSharedPtr<FJsonValue>& Value : *Warnings)
            {
                FString Text;
                if (Value->TryGetString(Text) && Text.Contains(TEXT("compileStatus")))
                {
                    bNamesTheStatus = true;
                    break;
                }
            }
            TestTrue(TEXT("a warning names compileStatus"), bNamesTheStatus);
        }
    }
    else
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("shader-compile-not-suppressed"),
            TEXT("The material ended up with a complete game-thread shader map despite "
                 "SkipShaderCompilation, so the did-not-land branch could not be asserted on this "
                 "host. The success invariant above was still checked."));
    }

    CleanupTestAsset(AssetPath);
    return true;
}
