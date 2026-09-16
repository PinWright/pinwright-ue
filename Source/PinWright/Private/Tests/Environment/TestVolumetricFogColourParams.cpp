// Copyright (c) 2026 Alexander Penkin. MIT License.

// lighting.setup_volumetric_fog — the four colour fields are declared AND written.
//
// The gap guarded here: the verb registered exactly one optional parameter (`viewDistance`) while
// the four fields that actually colour volumetric fog — VolumetricFogAlbedo, VolumetricFogEmissive,
// VolumetricFogExtinctionScale, VolumetricFogScatteringDistribution — had no typed surface at all.
// Inside VolumetricFogDistance those four are the ONLY live colour controls: the volumetric
// integration replaces the height fog's analytic inscattering there, so FogInscatteringLuminance
// applies only beyond that distance. The route the missing surface forced callers onto was a raw
// property.set on the fog component, which has a measured silent-failure path.
//
// Both directions are asserted, because each alone is a false green. The DECLARATION side uses the
// dispatcher's own accepted-name set: Tests/TestUtils.h's InvokeHandler calls the handler function
// directly and never runs ValidateHandlerParams, so a body-level round-trip proves the handler USES
// a key and says nothing about whether a caller may SUPPLY it. The BODY side round-trips real
// values off the component.
//
// The omission sub-case is not padding. `scatteringDistribution` is legitimately negative and
// `extinctionScale` legitimately any positive value, so the sentinel-default shape the existing
// `viewDistance` read uses (GetNumber(key, -1.0) plus a >= 0 test) cannot express "absent" for
// either — an implementation that reached for it would write 0 over whatever the level had every
// time the key was omitted, and no round-trip assertion would notice.
//
// The test restores all six written fields, the fog actor when it spawned one, and both package
// dirty flags: it runs against the host project's real open level, and a forced-clean flag left
// behind would make a later level.save lose work.

#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "Dom/JsonObject.h"
#include "Components/ExponentialHeightFogComponent.h"
#include "Engine/ExponentialHeightFog.h"
#include "Engine/Level.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Editor.h"
#include "UObject/Package.h"
#include "Tests/Infra/ParamSpecTestHelpers.h"
#include "Tests/TestSkipReporting.h"
#include "Tests/TestUtils.h"

// Uniquely named namespace: Unity merges test TUs into one translation unit, so an
// anonymous-namespace helper here would ODR-clash with the identically-shaped FindHeightFog in
// Tests/Environment/TestEnvironmentDirtyFlags.cpp.
namespace VolumetricFogColourParamsTestLocal
{
    AExponentialHeightFog* FindHeightFog(UWorld* World)
    {
        if (!World) return nullptr;
        for (TActorIterator<AExponentialHeightFog> It(World); It; ++It)
        {
            return *It;
        }
        return nullptr;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLightingSetupVolumetricFogColourParamsTest,
    "PinWright.lighting.setup_volumetric_fog.ColourParamsReachTheComponent",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FLightingSetupVolumetricFogColourParamsTest::RunTest(const FString& Parameters)
{
    const TCHAR* Method = TEXT("lighting.setup_volumetric_fog");

    // Declaration side — needs no world, so it runs even on a host that skips the rest.
    TestTrue(TEXT("`albedo` clears the dispatcher's declared-parameter gate"),
        ParamSpecTestHelpers::IsParamAccepted(Method, TEXT("albedo")));
    TestTrue(TEXT("`emissive` clears the dispatcher's declared-parameter gate"),
        ParamSpecTestHelpers::IsParamAccepted(Method, TEXT("emissive")));
    TestTrue(TEXT("`extinctionScale` clears the dispatcher's declared-parameter gate"),
        ParamSpecTestHelpers::IsParamAccepted(Method, TEXT("extinctionScale")));
    TestTrue(TEXT("`scatteringDistribution` clears the dispatcher's declared-parameter gate"),
        ParamSpecTestHelpers::IsParamAccepted(Method, TEXT("scatteringDistribution")));

    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("the verb has nowhere to find or spawn an ExponentialHeightFog, so the "
                 "component round-trip could not be measured"));
        return true;
    }

    UPackage* LevelPkg = World->PersistentLevel ? World->PersistentLevel->GetPackage() : nullptr;
    const bool bLevelWasDirty = LevelPkg && LevelPkg->IsDirty();

    // Seed a fog actor so the round-trip has a component, and remember whether this test is the
    // one that created it — a fixture the level already owned must survive.
    const bool bPreExistingFog =
        (VolumetricFogColourParamsTestLocal::FindHeightFog(World) != nullptr);
    TestTrue(TEXT("lighting.setup_volumetric_fog handler found (seed)"),
        InvokeHandler(Method, MakeShared<FJsonObject>()));

    AExponentialHeightFog* FogActor = VolumetricFogColourParamsTestLocal::FindHeightFog(World);
    UExponentialHeightFogComponent* FogComp = FogActor ? FogActor->GetComponent() : nullptr;
    if (!FogComp)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("fog-component-unavailable"),
            TEXT("no AExponentialHeightFog with a component after the seed call, so nothing "
                 "could be written or read back"));
        return true;
    }

    UPackage* FogPkg = FogActor->GetPackage();
    const bool bFogPkgWasDirty = FogPkg && FogPkg->IsDirty();

    const bool bOrigEnabled = FogComp->bEnableVolumetricFog;
    const float OrigDistance = FogComp->VolumetricFogDistance;
    const FColor OrigAlbedo = FogComp->VolumetricFogAlbedo;
    const FLinearColor OrigEmissive = FogComp->VolumetricFogEmissive;
    const float OrigExtinction = FogComp->VolumetricFogExtinctionScale;
    const float OrigDistribution = FogComp->VolumetricFogScatteringDistribution;

    // One block so the ordering is deterministic: restore the fields, destroy the fixture we
    // created (DestroyActor dirties unconditionally), then restore the flags we cleared.
    ON_SCOPE_EXIT
    {
        FogComp->bEnableVolumetricFog = bOrigEnabled;
        FogComp->VolumetricFogDistance = OrigDistance;
        FogComp->VolumetricFogAlbedo = OrigAlbedo;
        FogComp->VolumetricFogEmissive = OrigEmissive;
        FogComp->VolumetricFogExtinctionScale = OrigExtinction;
        FogComp->VolumetricFogScatteringDistribution = OrigDistribution;
        FogComp->MarkRenderStateDirty();

        if (!bPreExistingFog)
        {
            FogActor->Destroy();
        }
        if (FogPkg)
        {
            FogPkg->SetDirtyFlag(bFogPkgWasDirty);
        }
        if (LevelPkg)
        {
            LevelPkg->SetDirtyFlag(bLevelWasDirty);
        }
    };

    // ---- all four written ----
    TSharedPtr<FJsonObject> Albedo = MakeShared<FJsonObject>();
    Albedo->SetNumberField(TEXT("r"), 0.25);
    Albedo->SetNumberField(TEXT("g"), 0.5);
    Albedo->SetNumberField(TEXT("b"), 0.75);

    TSharedPtr<FJsonObject> Emissive = MakeShared<FJsonObject>();
    Emissive->SetNumberField(TEXT("r"), 0.125);
    Emissive->SetNumberField(TEXT("g"), 0.25);
    Emissive->SetNumberField(TEXT("b"), 0.5);

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetObjectField(TEXT("albedo"), Albedo);
    Payload->SetObjectField(TEXT("emissive"), Emissive);
    Payload->SetNumberField(TEXT("extinctionScale"), 2.5);
    // Negative on purpose: the value a sentinel-default read cannot distinguish from "absent".
    Payload->SetNumberField(TEXT("scatteringDistribution"), -0.4);

    TestTrue(TEXT("lighting.setup_volumetric_fog handler found (write)"),
        InvokeHandler(Method, Payload));

    // VolumetricFogAlbedo is an 8-bit FColor the renderer decodes as sRGB, so the assertion is on
    // the linear value that comes back out, not on the byte triple: an implementation detail
    // re-asserted here would only restate itself. 0.02 covers the sRGB quantization step, which is
    // under 0.01 in linear terms across this range.
    const FLinearColor ReadBackAlbedo(FogComp->VolumetricFogAlbedo);
    TestEqual(TEXT("albedo.r survives the sRGB round-trip"), ReadBackAlbedo.R, 0.25f, 0.02f);
    TestEqual(TEXT("albedo.g survives the sRGB round-trip"), ReadBackAlbedo.G, 0.5f, 0.02f);
    TestEqual(TEXT("albedo.b survives the sRGB round-trip"), ReadBackAlbedo.B, 0.75f, 0.02f);

    TestEqual(TEXT("emissive.r round-trips"), FogComp->VolumetricFogEmissive.R, 0.125f);
    TestEqual(TEXT("emissive.g round-trips"), FogComp->VolumetricFogEmissive.G, 0.25f);
    TestEqual(TEXT("emissive.b round-trips"), FogComp->VolumetricFogEmissive.B, 0.5f);
    TestEqual(TEXT("emissive alpha defaults to 1 when omitted"),
        FogComp->VolumetricFogEmissive.A, 1.0f);

    TestEqual(TEXT("extinctionScale round-trips"), FogComp->VolumetricFogExtinctionScale, 2.5f);
    TestEqual(TEXT("a negative scatteringDistribution round-trips"),
        FogComp->VolumetricFogScatteringDistribution, -0.4f);

    const FColor WrittenAlbedo = FogComp->VolumetricFogAlbedo;

    // ---- omitted means untouched ----
    // A second call naming only viewDistance must leave all four alone. This is the assertion a
    // sentinel-default implementation fails: it would write 0 over each of them.
    TSharedPtr<FJsonObject> DistanceOnly = MakeShared<FJsonObject>();
    DistanceOnly->SetNumberField(TEXT("viewDistance"), 6000.0);

    TestTrue(TEXT("lighting.setup_volumetric_fog handler found (omission)"),
        InvokeHandler(Method, DistanceOnly));

    TestEqual(TEXT("viewDistance still writes"), FogComp->VolumetricFogDistance, 6000.0f);
    TestEqual(TEXT("an omitted albedo leaves VolumetricFogAlbedo alone"),
        FogComp->VolumetricFogAlbedo, WrittenAlbedo);
    TestEqual(TEXT("an omitted emissive leaves VolumetricFogEmissive alone"),
        FogComp->VolumetricFogEmissive.G, 0.25f);
    TestEqual(TEXT("an omitted extinctionScale leaves VolumetricFogExtinctionScale alone"),
        FogComp->VolumetricFogExtinctionScale, 2.5f);
    TestEqual(TEXT("an omitted scatteringDistribution leaves VolumetricFogScatteringDistribution alone"),
        FogComp->VolumetricFogScatteringDistribution, -0.4f);

    return true;
}
