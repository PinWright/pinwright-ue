// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression test for B-landscape-set-material-stale-mics.
//
// landscape.set_material assigned ALandscapeProxy::LandscapeMaterial and then called
// a bare PostEditChange(). PostEditChange() builds an EMPTY FPropertyChangedEvent, so
// MemberProperty is null and MemberPropertyName is NAME_None. Both overrides dispatch
// the material work off a name match:
//   ALandscapeProxy::PostEditChangeProperty (LandscapeEdit.cpp:6143) matches
//     GET_MEMBER_NAME_CHECKED(ALandscapeProxy, LandscapeMaterial), then clears the
//     parents out of MaterialInstanceConstantMap, EMPTIES it, and calls
//     UpdateAllComponentMaterialInstances() (:6175)
//   ALandscape::PostEditChangeProperty (:6629) matches the same name to set
//     bMaterialChanged, which drives UpdateAllComponentMaterialInstances() (:6792)
// Neither override has a null-property catch-all, so with NAME_None the branch never
// ran for ANY assignment — not merely a same-pointer one. The per-component material
// instances kept their previously built shader map and no material edit, topology or
// constant-value, ever reached the screen. Measured on one fixed camera: baseline
// 8.85% foam coverage; edit + compile_material -> 8.91%; + set_material with the same
// material -> still 8.91%; only a full round-trip (assign a different material, then
// assign the intended one back) reached the expected 2.39%.
//
// The fix (SetLandscapeMaterialAndNotify in LandscapeHandler.cpp) constructs a real
// FPropertyChangedEvent naming LandscapeMaterial and calls the VIRTUAL
// PostEditChangeProperty, which is how the engine's own setter
// (ALandscapeProxy::EditorSetLandscapeMaterial, LandscapeBlueprintSupport.cpp:98)
// does it. That setter cannot be called from this module: ALandscapeProxy is
// UCLASS(MinimalAPI) and the setter carries no LANDSCAPE_API, so only its type info
// is exported — but PostEditChangeProperty is virtual and dispatches through the
// vtable, which needs no exported symbol.
//
// DIFFERENTIAL PROPERTY. This test seeds ALandscapeProxy::MaterialInstanceConstantMap
// with a sentinel entry, then drives the production landscape.set_material handler.
// Emptying that map is the exact engine work the empty event skipped:
//   * pre-fix  the sentinel survives the call (branch never ran)   -> FAILS
//   * post-fix the map is emptied by the LandscapeMaterial branch  -> PASSES
// It asserts the mechanism rather than a rendered pixel, so it needs no external
// content beyond one engine material and cannot pass for the wrong reason: the only
// code path in the engine that empties this map is the branch the bug skipped.
//
// The fixture is built in-code through the production landscape.create handler and a
// landscape it fails to create is a FAILURE, not a skip.
#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Editor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Landscape.h"
#include "LandscapeProxy.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Materials/MaterialInterface.h"
#include "UObject/Package.h"
#include "Tests/TestUtils.h"
#include "Tests/TestWorldUtils.h"
#include "Tests/TestSkipReporting.h"

namespace
{
    // Distinctly named (the sibling landscape test declares FindFirstLandscapeComponent)
    // so anonymous-namespace symbols don't ODR-collide when Unity merges these TUs.
    ALandscape* FindLandscapeActorForMicTest(UWorld* World, const FString& Label)
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

    // Ships with every engine install (including launcher/Rocket builds), so it is a
    // hard requirement rather than an environment-dependent skip.
    const TCHAR* const GMicTestMaterialPath =
        TEXT("/Engine/EngineMaterials/WorldGridMaterial.WorldGridMaterial");
}

// ---- landscape.set_material rebuilds the per-component material instances ----
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLandscapeSetMaterialRebuildsMicsTest,
    "PinWright.landscape.set_material.RebuildsComponentMaterialInstances",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FLandscapeSetMaterialRebuildsMicsTest::RunTest(const FString& Parameters)
{
    // The landscape registration + PostEditChangeProperty pipeline this drives needs a
    // real editor world. The world is an environment precondition, not the fixture.
    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("Editor world not available — skipping landscape.set_material MIC test"));
        return true;
    }

    TestTrue(TEXT("landscape.create handler registered"),
        IsHandlerRegistered(TEXT("landscape.create")));
    if (!TestTrue(TEXT("landscape.set_material handler registered"),
            IsHandlerRegistered(TEXT("landscape.set_material"))))
    {
        return false;
    }

    UMaterialInterface* Material =
        LoadObject<UMaterialInterface>(nullptr, GMicTestMaterialPath);
    if (!TestNotNull(TEXT("engine fixture material loaded"), Material))
    {
        return false;
    }

    // Destroy any actor spawned during the test and restore the level dirty flag on
    // scope exit, so this test leaves the open map exactly as it found it.
    FScopedEditorWorldActorGuard WorldGuard;

    const FString LandscapeLabel = FString::Printf(TEXT("MCP_SetMaterialMics_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));

    // --- Fixture: a small real landscape via the production create handler. ---
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

    ALandscape* Landscape = FindLandscapeActorForMicTest(World, LandscapeLabel);
    if (!TestNotNull(TEXT("spawned landscape located in the editor world"), Landscape))
    {
        return false;
    }

    // The material branch only runs when the landscape has a ULandscapeInfo (the engine
    // guards every material statement behind it), so a landscape that never registered
    // would make this test vacuous rather than failing.
    if (!TestNotNull(TEXT("landscape registered a ULandscapeInfo (else the material "
                          "branch is skipped by the engine and this test is vacuous)"),
            Landscape->GetLandscapeInfo()))
    {
        return false;
    }

    // --- Seed the sentinel whose removal is the differential property. ---
    // A real MIC, because the branch dereferences every value in the map before
    // emptying it (SetParentEditorOnly(nullptr) / BasePropertyOverrides).
    const FString SentinelKey = TEXT("PinWrightStaleMicSentinel");
    UMaterialInstanceConstant* Sentinel =
        NewObject<UMaterialInstanceConstant>(GetTransientPackage(), NAME_None, RF_Transient);
    if (!TestNotNull(TEXT("sentinel material instance created"), Sentinel))
    {
        return false;
    }
    Landscape->MaterialInstanceConstantMap.Add(SentinelKey, Sentinel);
    if (!TestTrue(TEXT("sentinel is present before the assignment"),
            Landscape->MaterialInstanceConstantMap.Contains(SentinelKey)))
    {
        return false;
    }

    // --- Drive the production landscape.set_material handler. ---
    {
        TSharedPtr<FJsonObject> SetPayload = MakeShared<FJsonObject>();
        SetPayload->SetStringField(TEXT("landscapeName"), LandscapeLabel);
        SetPayload->SetStringField(TEXT("materialPath"), GMicTestMaterialPath);

        TSharedRef<FTestResponseCapture> SetCapture = MakeShared<FTestResponseCapture>();
        if (!TestTrue(TEXT("landscape.set_material invoked"),
                InvokeHandlerWithSharedCapture(TEXT("landscape.set_material"), SetPayload, SetCapture)))
        {
            return false;
        }
        PumpUntilCaptured(*SetCapture, /*TimeoutSeconds=*/60.0);
        TestTrue(TEXT("landscape.set_material responded"), SetCapture->bWasCalled);
        // The handler reported success both pre- and post-fix — that false success is
        // the context for this test, not the thing it distinguishes.
        TestTrue(TEXT("landscape.set_material reported success"), SetCapture->bSuccess);
        if (!SetCapture->bWasCalled || !SetCapture->bSuccess)
        {
            return false;
        }
    }

    // --- Core regression assertion ---
    // The LandscapeMaterial branch empties MaterialInstanceConstantMap. Pre-fix the
    // empty FPropertyChangedEvent meant that branch never ran, so the sentinel — and
    // with it every real component MIC holding a stale shader map — survived.
    TestFalse(
        TEXT("landscape.set_material emptied MaterialInstanceConstantMap, so the "
             "component material instances are rebuilt rather than kept with a stale "
             "shader map (pre-fix the bare PostEditChange() left the sentinel behind)"),
        Landscape->MaterialInstanceConstantMap.Contains(SentinelKey));

    // Guard the assignment itself: emptying the map would be meaningless if the
    // material never landed on the actor.
    TestTrue(TEXT("LandscapeMaterial holds the assigned material"),
        Landscape->LandscapeMaterial.Get() == Material);

    return true;
}
