// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression test for "compile_material reports compileSucceeded while the landscape
// ignores the graph edit".
//
// THE DEFECT. material.authoring.compile_material did PreEditChange(nullptr) +
// PostEditChange() and nothing else. Two caches downstream of the master keep stale data
// after that, and both are silent:
//   1. UMaterial::PostEditChangePropertyInternal (Material.cpp:5350) regenerates the
//      master's StateId and recompiles it (:5412-5433) but creates NO
//      FMaterialUpdateContext, and FMaterialUpdateContext's destructor
//      (MaterialShared.cpp:5049) is the only code that recaches dependent material
//      instances' static permutations (:5099-5156).
//   2. ALandscapeProxy::MaterialInstanceConstantMap caches the per-layer-allocation
//      "combination materials". GetCombinationMaterial reuses the cached entry whenever
//      the key and parent match (LandscapeEdit.cpp:617-618), so a master graph edit keeps
//      the combination MIC — and every component MIC parented to it — on the old shader
//      map. Only UpdateAllComponentMaterialInstances(bInInvalidateCombinationMaterials=
//      true) resets that map (LandscapeEdit.cpp:845-850).
// Measured before the fix, one fixed camera: forcing a lane gate to 0 in the terrain
// master — which must erase the entire painted road network — moved the frame 1.81%, the
// noise floor. The same edit after an assign-away-and-back round-trip on the landscape
// material moved it 17.19%.
//
// Commit 11fe111a is NOT this fix. It taught landscape.set_material to name
// LandscapeMaterial in its FPropertyChangedEvent so the engine's material branch runs.
// That branch is triggered by an ASSIGNMENT to the property; a graph edit to the master
// performs no assignment, so the mechanism never fires.
//
// DIFFERENTIAL PROPERTY, case 1. A sentinel entry is seeded into
// MaterialInstanceConstantMap and the production compile_material handler is driven on
// the material the landscape renders with:
//   * pre-fix  the sentinel survives  (nothing ever touched the landscape)   -> FAILS
//   * post-fix the map is Reset() by the forced rebuild                      -> PASSES
// That map is only emptied by the branch the defect skipped, so the assertion cannot pass
// for the wrong reason. It is paired with the response's own measured coverage
// (consumerRefresh.consumersRefreshed), which is derived from MIC pointer identity —
// UpdateMaterialInstances_Internal allocates a brand new
// ULandscapeMaterialInstanceConstant per component per rebuild (LandscapeEdit.cpp:715),
// so an unchanged pointer set proves the rebuild did not run.
//
// FAILURE DIRECTION, case 2. A material NO landscape uses must report consumersFound 0
// and raise no warning. Without it the first case would still pass if the verb simply
// claimed coverage for every landscape in the level.
#include "Misc/AutomationTest.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "Editor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Landscape.h"
#include "LandscapeProxy.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Tests/TestSkipReporting.h"
#include "Tests/TestUtils.h"
#include "Tests/TestWorldUtils.h"
#include "UObject/Package.h"

namespace
{
    // Distinctly named so the anonymous-namespace symbols in this file cannot ODR-collide
    // with the sibling landscape tests when Unity merges the TUs.
    ALandscape* FindLandscapeForConsumerRefreshTest(UWorld* World, const FString& Label)
    {
        if (!World)
        {
            return nullptr;
        }
        for (TActorIterator<ALandscape> It(World); It; ++It)
        {
            ALandscape* Landscape = *It;
            if (Landscape && Landscape->GetActorLabel().Equals(Label, ESearchCase::IgnoreCase))
            {
                return Landscape;
            }
        }
        return nullptr;
    }

    // A plain, valid, empty UMaterial in a sandbox package. Deliberately NOT an engine
    // material: /Engine/EngineMaterials/WorldGridMaterial is bUsedAsSpecialEngineMaterial,
    // and recompiling it synchronously would churn the whole editor's shader cache.
    UMaterial* CreateSandboxMaterialForConsumerRefreshTest(const FString& AssetPath)
    {
        UPackage* Package = CreatePackage(*AssetPath);
        if (!Package)
        {
            return nullptr;
        }

        UMaterial* Material = NewObject<UMaterial>(
            Package,
            FName(*FPackageName::GetLongPackageAssetName(AssetPath)),
            RF_Public | RF_Standalone);
        if (!Material)
        {
            return nullptr;
        }

        Material->PostEditChange();
        FAssetRegistryModule::AssetCreated(Material);
        return Material;
    }

    int32 ReadConsumerRefreshNumber(
        const TSharedPtr<FJsonObject>& Result, const TCHAR* Field, int32 Fallback)
    {
        const TSharedPtr<FJsonObject>* Block = nullptr;
        if (!Result.IsValid() || !Result->TryGetObjectField(TEXT("consumerRefresh"), Block) ||
            !Block || !(*Block).IsValid())
        {
            return Fallback;
        }
        int32 Value = Fallback;
        (*Block)->TryGetNumberField(Field, Value);
        return Value;
    }

    bool ReadConsumerRefreshBool(
        const TSharedPtr<FJsonObject>& Result, const TCHAR* Field, bool Fallback)
    {
        const TSharedPtr<FJsonObject>* Block = nullptr;
        if (!Result.IsValid() || !Result->TryGetObjectField(TEXT("consumerRefresh"), Block) ||
            !Block || !(*Block).IsValid())
        {
            return Fallback;
        }
        bool Value = Fallback;
        (*Block)->TryGetBoolField(Field, Value);
        return Value;
    }

    // Build a real landscape rendering MaterialPath, through the production verbs.
    // Returns nullptr on any fixture failure, having already reported which step failed.
    ALandscape* BuildLandscapeRenderingMaterialForConsumerRefreshTest(
        FAutomationTestBase& Test,
        UWorld* World,
        const FString& LandscapeLabel,
        const FString& MaterialPath,
        UMaterial* Material)
    {
        TSharedPtr<FJsonObject> CreatePayload = MakeShared<FJsonObject>();
        CreatePayload->SetStringField(TEXT("name"), LandscapeLabel);
        CreatePayload->SetNumberField(TEXT("componentsX"), 1);
        CreatePayload->SetNumberField(TEXT("componentsY"), 1);
        CreatePayload->SetNumberField(TEXT("quadsPerComponent"), 63);
        CreatePayload->SetNumberField(TEXT("sectionsPerComponent"), 1);

        TSharedRef<FTestResponseCapture> CreateCapture = MakeShared<FTestResponseCapture>();
        if (!Test.TestTrue(TEXT("landscape.create invoked"),
                InvokeHandlerWithSharedCapture(TEXT("landscape.create"), CreatePayload, CreateCapture)))
        {
            return nullptr;
        }
        PumpUntilCaptured(*CreateCapture, /*TimeoutSeconds=*/30.0);
        if (!Test.TestTrue(TEXT("landscape.create succeeded (fixture built)"), CreateCapture->bSuccess))
        {
            return nullptr;
        }

        ALandscape* Landscape = FindLandscapeForConsumerRefreshTest(World, LandscapeLabel);
        if (!Test.TestNotNull(TEXT("spawned landscape located in the editor world"), Landscape))
        {
            return nullptr;
        }
        // The engine guards its material work behind a ULandscapeInfo, so an unregistered
        // landscape would make the test vacuous rather than failing.
        if (!Test.TestNotNull(TEXT("landscape registered a ULandscapeInfo"),
                Landscape->GetLandscapeInfo()))
        {
            return nullptr;
        }

        TSharedPtr<FJsonObject> SetPayload = MakeShared<FJsonObject>();
        SetPayload->SetStringField(TEXT("landscapeName"), LandscapeLabel);
        SetPayload->SetStringField(TEXT("materialPath"), MaterialPath);

        TSharedRef<FTestResponseCapture> SetCapture = MakeShared<FTestResponseCapture>();
        if (!Test.TestTrue(TEXT("landscape.set_material invoked"),
                InvokeHandlerWithSharedCapture(TEXT("landscape.set_material"), SetPayload, SetCapture)))
        {
            return nullptr;
        }
        PumpUntilCaptured(*SetCapture, /*TimeoutSeconds=*/60.0);
        if (!Test.TestTrue(TEXT("landscape.set_material succeeded"), SetCapture->bSuccess))
        {
            return nullptr;
        }

        // If the material never landed the test is vacuous: the compile would find no
        // consumer and the sentinel assertion would be meaningless.
        if (!Test.TestTrue(TEXT("landscape renders with the sandbox material"),
                Landscape->LandscapeMaterial.Get() == Material))
        {
            return nullptr;
        }
        return Landscape;
    }
}

// ---- compile_material rebuilds the landscape consumers of the edited master ----
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCompileMaterialRefreshesLandscapeConsumersTest,
    "PinWright.material.authoring.compile_material.RefreshesLandscapeConsumers",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCompileMaterialRefreshesLandscapeConsumersTest::RunTest(const FString& Parameters)
{
    // The landscape registration + component-MIC pipeline needs a real editor world. The
    // world is an environment precondition, not the fixture.
    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("Editor world not available — skipping compile_material consumer-refresh test"));
        return true;
    }

    TestTrue(TEXT("landscape.create handler registered"),
        IsHandlerRegistered(TEXT("landscape.create")));
    if (!TestTrue(TEXT("material.authoring.compile_material handler registered"),
            IsHandlerRegistered(TEXT("material.authoring.compile_material"))))
    {
        return false;
    }

    const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString UsedMaterialPath =
        FString::Printf(TEXT("/Game/__PW_GatewayTests/LsUsed_%s"), *Suffix);
    const FString UnusedMaterialPath =
        FString::Printf(TEXT("/Game/__PW_GatewayTests/LsUnused_%s"), *Suffix);
    const FString LandscapeLabel = FString::Printf(TEXT("PW_ConsumerRefresh_%s"), *Suffix);

    ON_SCOPE_EXIT
    {
        CleanupTestAsset(UsedMaterialPath);
        CleanupTestAsset(UnusedMaterialPath);
    };

    UMaterial* UsedMaterial = CreateSandboxMaterialForConsumerRefreshTest(UsedMaterialPath);
    if (!TestNotNull(TEXT("sandbox material the landscape will render with"), UsedMaterial))
    {
        return false;
    }
    UMaterial* UnusedMaterial = CreateSandboxMaterialForConsumerRefreshTest(UnusedMaterialPath);
    if (!TestNotNull(TEXT("sandbox material no landscape uses"), UnusedMaterial))
    {
        return false;
    }

    // Destroy any actor spawned during the test and restore the level dirty flag on scope
    // exit, so this test leaves the open map exactly as it found it.
    FScopedEditorWorldActorGuard WorldGuard;

    // --- Fixture: a small real landscape. ---
    {
        TSharedPtr<FJsonObject> CreatePayload = MakeShared<FJsonObject>();
        CreatePayload->SetStringField(TEXT("name"), LandscapeLabel);
        CreatePayload->SetNumberField(TEXT("componentsX"), 1);
        CreatePayload->SetNumberField(TEXT("componentsY"), 1);
        CreatePayload->SetNumberField(TEXT("quadsPerComponent"), 63);
        CreatePayload->SetNumberField(TEXT("sectionsPerComponent"), 1);

        TSharedRef<FTestResponseCapture> CreateCapture = MakeShared<FTestResponseCapture>();
        if (!TestTrue(TEXT("landscape.create invoked"),
                InvokeHandlerWithSharedCapture(TEXT("landscape.create"), CreatePayload, CreateCapture)))
        {
            return false;
        }
        PumpUntilCaptured(*CreateCapture, /*TimeoutSeconds=*/30.0);
        TestTrue(TEXT("landscape.create responded"), CreateCapture->bWasCalled);
        TestTrue(TEXT("landscape.create succeeded (fixture built)"), CreateCapture->bSuccess);
        if (!CreateCapture->bWasCalled || !CreateCapture->bSuccess)
        {
            return false;
        }
    }

    ALandscape* Landscape = FindLandscapeForConsumerRefreshTest(World, LandscapeLabel);
    if (!TestNotNull(TEXT("spawned landscape located in the editor world"), Landscape))
    {
        return false;
    }
    // The material branch only runs when the landscape has a ULandscapeInfo (the engine
    // guards its material work behind it), so an unregistered landscape would make this
    // test vacuous rather than failing.
    if (!TestNotNull(TEXT("landscape registered a ULandscapeInfo"), Landscape->GetLandscapeInfo()))
    {
        return false;
    }

    // Assign the sandbox material through the production verb, so the per-component MICs
    // exist and are parented to it before the compile under test.
    {
        TSharedPtr<FJsonObject> SetPayload = MakeShared<FJsonObject>();
        SetPayload->SetStringField(TEXT("landscapeName"), LandscapeLabel);
        SetPayload->SetStringField(TEXT("materialPath"), UsedMaterialPath);

        TSharedRef<FTestResponseCapture> SetCapture = MakeShared<FTestResponseCapture>();
        if (!TestTrue(TEXT("landscape.set_material invoked"),
                InvokeHandlerWithSharedCapture(TEXT("landscape.set_material"), SetPayload, SetCapture)))
        {
            return false;
        }
        PumpUntilCaptured(*SetCapture, /*TimeoutSeconds=*/60.0);
        TestTrue(TEXT("landscape.set_material responded"), SetCapture->bWasCalled);
        TestTrue(TEXT("landscape.set_material succeeded"), SetCapture->bSuccess);
        if (!SetCapture->bWasCalled || !SetCapture->bSuccess)
        {
            return false;
        }
    }

    // If the material never landed the whole test is vacuous: compile_material would find
    // no consumer and the sentinel assertion would be meaningless.
    if (!TestTrue(TEXT("landscape renders with the sandbox material"),
            Landscape->LandscapeMaterial.Get() == UsedMaterial))
    {
        return false;
    }

    // --- Seed the sentinel whose removal is the differential property. ---
    // A real MIC, because the reset path dereferences the map's values.
    const FString SentinelKey = TEXT("PinWrightMasterEditStaleMicSentinel");
    UMaterialInstanceConstant* Sentinel =
        NewObject<UMaterialInstanceConstant>(GetTransientPackage(), NAME_None, RF_Transient);
    if (!TestNotNull(TEXT("sentinel material instance created"), Sentinel))
    {
        return false;
    }
    Landscape->MaterialInstanceConstantMap.Add(SentinelKey, Sentinel);
    if (!TestTrue(TEXT("sentinel is present before the compile"),
            Landscape->MaterialInstanceConstantMap.Contains(SentinelKey)))
    {
        return false;
    }

    // --- Case 1: compile the master the landscape renders with. ---
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("assetPath"), UsedMaterialPath);
        Payload->SetBoolField(TEXT("save"), false);

        FTestResponseCapture Capture;
        if (!TestTrue(TEXT("compile_material invoked"),
                InvokeHandlerWithCapture(
                    TEXT("material.authoring.compile_material"), Payload, Capture)))
        {
            return false;
        }
        TestTrue(TEXT("compile_material responded"), Capture.bWasCalled);
        // Success both pre- and post-fix — that false success is the context for this
        // test, not the thing it distinguishes.
        TestTrue(TEXT("compile_material reported success"), Capture.bSuccess);
        if (!Capture.bWasCalled || !Capture.Result.IsValid())
        {
            return false;
        }

        // Core regression assertion: the engine work the defect skipped actually ran.
        TestFalse(
            TEXT("compile_material reset MaterialInstanceConstantMap, so the landscape's "
                 "combination materials are rebuilt against the edited master rather than "
                 "kept on the previous shader map (pre-fix the sentinel survived)"),
            Landscape->MaterialInstanceConstantMap.Contains(SentinelKey));

        // The response must SAY it happened, measured. -1 fallbacks make a missing
        // consumerRefresh block fail rather than read as a clean zero.
        const int32 Found = ReadConsumerRefreshNumber(Capture.Result, TEXT("consumersFound"), -1);
        const int32 Refreshed =
            ReadConsumerRefreshNumber(Capture.Result, TEXT("consumersRefreshed"), -1);
        TestTrue(TEXT("consumerRefresh reports at least one landscape consumer found"),
            Found >= 1);
        TestTrue(TEXT("consumerRefresh reports at least one landscape consumer rebuilt"),
            Refreshed >= 1);
        TestTrue(TEXT("consumerRefresh was measured, not assumed"),
            ReadConsumerRefreshBool(Capture.Result, TEXT("measured"), false));
        TestTrue(TEXT("consumerRefresh coverage is complete"),
            ReadConsumerRefreshBool(Capture.Result, TEXT("complete"), false));

        // subObjectsRefreshed counts components whose MIC pointer set changed identity —
        // the half of the measurement the refresh path cannot fake.
        TestTrue(TEXT("at least one landscape component received a new material instance"),
            ReadConsumerRefreshNumber(Capture.Result, TEXT("subObjectsRefreshed"), -1) >= 1);
    }

    // --- Case 2 (failure direction): a master no landscape renders with. ---
    // Coverage must be zero here. A verb that claimed every landscape in the level would
    // pass case 1 and fail this one.
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("assetPath"), UnusedMaterialPath);
        Payload->SetBoolField(TEXT("save"), false);

        FTestResponseCapture Capture;
        if (!TestTrue(TEXT("compile_material invoked for the unused material"),
                InvokeHandlerWithCapture(
                    TEXT("material.authoring.compile_material"), Payload, Capture)))
        {
            return false;
        }
        TestTrue(TEXT("compile_material responded for the unused material"), Capture.bWasCalled);
        if (!Capture.Result.IsValid())
        {
            return false;
        }

        TestTrue(TEXT("no landscape is reported as a consumer of an unused master"),
            ReadConsumerRefreshNumber(Capture.Result, TEXT("consumersFound"), -1) == 0);
        TestTrue(TEXT("no landscape is reported as rebuilt for an unused master"),
            ReadConsumerRefreshNumber(Capture.Result, TEXT("consumersRefreshed"), -1) == 0);
        TestTrue(TEXT("zero consumers still counts as complete coverage"),
            ReadConsumerRefreshBool(Capture.Result, TEXT("complete"), false));
        // Nothing was missed, so nothing to warn about. A warning here would mean the
        // completeness verdict and the warning disagree.
        TestFalse(TEXT("a fully covered compile emits no warnings"),
            Capture.Result->HasField(TEXT("warnings")));
    }

    return true;
}

// ---- material.compile_mgir rebuilds the landscape consumers of the compiled master ----
//
// THE SECOND DOOR. The fix above (0fe35187) taught material.authoring.compile_material to
// push a master edit into the landscapes caching instances of it. It was recorded as
// closing the defect; it closed one entry point. material.compile_mgir — the bulk text-IR
// path the wiki actively recommends over repeated imperative calls — reached the same
// master through FinalizeMaterial (MGIRCompiler.cpp) and did LESS than the pre-fix
// compile_material did: a bare PreEditChange(nullptr) + PostEditChange() +
// ForceRecompileForRendering(), with no FMaterialUpdateContext and no landscape rebuild,
// and then it SAVED the asset. An agent authoring a landscape master through MGIR
// therefore got a correct .uasset on disk, a success payload with assetPaths and
// expressionsCreated, and a terrain still rendering the previous shader map.
//
// In Append mode (the default) the compile empties and rebuilds the whole expression
// collection first, which is exactly the layer-allocation change
// ALandscapeProxy::MaterialInstanceConstantMap keys its combination materials on
// (LandscapeEdit.cpp:617-618) — the worst case for this defect, not an edge of it.
//
// DIFFERENTIAL PROPERTY: identical to the sibling test above, so the two cannot drift.
// A sentinel is seeded into MaterialInstanceConstantMap; only the branch the defect
// skipped empties that map, so the assertion cannot pass for the wrong reason.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCompileMgirRefreshesLandscapeConsumersTest,
    "PinWright.material.compile_mgir.RefreshesLandscapeConsumers",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCompileMgirRefreshesLandscapeConsumersTest::RunTest(const FString& Parameters)
{
    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("Editor world not available — skipping compile_mgir consumer-refresh test"));
        return true;
    }

    if (!TestTrue(TEXT("material.compile_mgir handler registered"),
            IsHandlerRegistered(TEXT("material.compile_mgir"))))
    {
        return false;
    }

    const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString UsedMaterialPath =
        FString::Printf(TEXT("/Game/__PW_GatewayTests/MgirLsUsed_%s"), *Suffix);
    const FString LandscapeLabel = FString::Printf(TEXT("PW_MgirConsumerRefresh_%s"), *Suffix);

    ON_SCOPE_EXIT
    {
        CleanupTestAsset(UsedMaterialPath);
    };

    UMaterial* UsedMaterial = CreateSandboxMaterialForConsumerRefreshTest(UsedMaterialPath);
    if (!TestNotNull(TEXT("sandbox material the landscape will render with"), UsedMaterial))
    {
        return false;
    }

    FScopedEditorWorldActorGuard WorldGuard;

    ALandscape* Landscape = BuildLandscapeRenderingMaterialForConsumerRefreshTest(
        *this, World, LandscapeLabel, UsedMaterialPath, UsedMaterial);
    if (!Landscape)
    {
        return false;
    }

    const FString SentinelKey = TEXT("PinWrightMgirMasterEditStaleMicSentinel");
    UMaterialInstanceConstant* Sentinel =
        NewObject<UMaterialInstanceConstant>(GetTransientPackage(), NAME_None, RF_Transient);
    if (!TestNotNull(TEXT("sentinel material instance created"), Sentinel))
    {
        return false;
    }
    Landscape->MaterialInstanceConstantMap.Add(SentinelKey, Sentinel);
    if (!TestTrue(TEXT("sentinel is present before the MGIR compile"),
            Landscape->MaterialInstanceConstantMap.Contains(SentinelKey)))
    {
        return false;
    }

    // Compile a trivial but REAL graph into the master the landscape renders with.
    // save:false keeps the sandbox asset off disk; the defect and the fix are both in
    // FinalizeMaterial, which runs either way.
    {
        const FString MgirText = FString::Printf(
            TEXT("entry material `%s` {\n")
            TEXT("    %%base = constant Float3(1.0, 0.0, 1.0) @(0, 0)\n")
            TEXT("    output BaseColor: %%base\n")
            TEXT("}\n"),
            *UsedMaterialPath);

        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("text"), MgirText);
        Payload->SetBoolField(TEXT("save"), false);

        FTestResponseCapture Capture;
        if (!TestTrue(TEXT("compile_mgir invoked"),
                InvokeHandlerWithCapture(TEXT("material.compile_mgir"), Payload, Capture)))
        {
            return false;
        }
        TestTrue(TEXT("compile_mgir responded"), Capture.bWasCalled);
        // Success both pre- and post-fix — the false success is the context for this test,
        // not the thing it distinguishes.
        TestTrue(TEXT("compile_mgir reported success"), Capture.bSuccess);
        if (!Capture.bWasCalled || !Capture.Result.IsValid())
        {
            return false;
        }

        // The graph really was compiled, so a passing refresh assertion is not being made
        // about a compile that did nothing.
        int32 BlocksCompiled = 0;
        Capture.Result->TryGetNumberField(TEXT("blocksCompiled"), BlocksCompiled);
        TestTrue(TEXT("compile_mgir compiled the material block"), BlocksCompiled >= 1);

        // Core regression assertion: the engine work the defect skipped actually ran.
        TestFalse(
            TEXT("compile_mgir reset MaterialInstanceConstantMap, so the landscape's "
                 "combination materials are rebuilt against the compiled master rather "
                 "than kept on the previous shader map (pre-fix the sentinel survived)"),
            Landscape->MaterialInstanceConstantMap.Contains(SentinelKey));

        // The response must SAY it happened, measured. -1 fallbacks make a missing
        // consumerRefresh block fail rather than read as a clean zero.
        TestTrue(TEXT("consumerRefresh reports at least one landscape consumer found"),
            ReadConsumerRefreshNumber(Capture.Result, TEXT("consumersFound"), -1) >= 1);
        TestTrue(TEXT("consumerRefresh reports at least one landscape consumer rebuilt"),
            ReadConsumerRefreshNumber(Capture.Result, TEXT("consumersRefreshed"), -1) >= 1);
        TestTrue(TEXT("consumerRefresh was measured, not assumed"),
            ReadConsumerRefreshBool(Capture.Result, TEXT("measured"), false));
        TestTrue(TEXT("consumerRefresh coverage is complete"),
            ReadConsumerRefreshBool(Capture.Result, TEXT("complete"), false));
        TestTrue(TEXT("at least one landscape component received a new material instance"),
            ReadConsumerRefreshNumber(Capture.Result, TEXT("subObjectsRefreshed"), -1) >= 1);
    }

    return true;
}
