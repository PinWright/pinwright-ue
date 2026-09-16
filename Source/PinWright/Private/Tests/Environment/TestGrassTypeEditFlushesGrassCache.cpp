// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression tests for B-grass-varieties-edit-does-not-reach-renderer.
//
// THE DEFECT. A ULandscapeGrassType is never rendered directly: every landscape proxy
// that references one builds HISM clusters from it and keeps them in a private, transient
// cache, ALandscapeProxy::FoliageCache.CachedGrassComps, keyed on a struct that carries
// the grass type POINTER and the variety COUNT but none of the values inside an
// FGrassVariety. Editing GrassDensity or GrassMesh therefore leaves every cached key equal
// to itself. From UE 5.4 the engine stopped compensating for that:
// ULandscapeGrassType::PostEditChangeProperty flushed the consumers' grass on 5.3 and only
// invalidates a summary + recomputes StateHash from 5.4 on. So the asset on disk was
// correct, every verb reported success, and the renderer kept the old grass — measured at
// a fixed pose as meanLuminance 0.4444 -> 0.4439 for the edit (i.e. not at all) and
// 0.4439 -> 0.4287 once `grass.FlushCache` was issued by hand.
//
// WHAT THESE TESTS MEASURE, AND WHAT THEY DO NOT. They do NOT measure luminance: rendering
// grass in an automation host needs a landscape material with a grass output, baked grass
// maps and a camera in range, none of which a headless suite can be made to produce
// reliably. What they measure instead is the state one layer beneath the pixels and the
// one the pixels are built from — the per-proxy grass cache. A stale entry keyed on the
// edited grass type is seeded into a real landscape's FoliageCache; the assertion is that
// the entry is GONE afterwards and that the verb's own consumerRefresh block counted it.
// An implementation that reports success without flushing leaves the entry in place, which
// is exactly the defect, so the seeded entry stands in for the stale instances the ticket
// photographed. The gap is named rather than papered over: nothing here proves a pixel
// moved.
//
// AND A CACHE-EMPTIED ASSERTION IS NOT ENOUGH ON ITS OWN — that is the lesson these tests
// were extended to carry. The first version asserted only that the seeded entry was gone,
// and "gone" reads identically whether the grass was invalidated (rebuilt as the camera
// moves) or DESTROYED (the per-component grass density maps deleted, recovered only by the
// editor's amortised camera-driven builder, and written back to the package by the next
// save). The implementation shipped in plugin commit d8f1bc32 did the second — it copied
// `grass.FlushCache`'s defaulted bFlushGrassMaps=true — and every assertion here passed
// while a live level went from 136 grass components to 0 with no recovery short of an
// editor restart. So each test now also seeds a COMPUTED grass map onto every landscape
// component and asserts it is still there afterwards, from two independent readings: the
// engine state (ULandscapeComponent::GrassData) and the verb's own `grassMaps` block,
// which is taken on both sides of the flush inside the call.
//
// Helpers are file-scope statics with distinctive names rather than an anonymous namespace:
// Tests/ TUs merge under Unity and an anonymous-namespace helper collides with the
// same-named helper in a sibling test (docs/lessons.md).
#include "Misc/AutomationTest.h"
#include "Compat/EngineVersionCompat.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Landscape.h"
#include "LandscapeComponent.h"
#include "LandscapeGrassType.h"
#include "LandscapeProxy.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"
#include "Tests/TestSkipReporting.h"
#include "Tests/TestUtils.h"
#include "Tests/TestWorldUtils.h"
#include "UObject/Package.h"

// Creates the smallest landscape landscape.create can make (1x1 component of 7-quad
// subsections) so the fixture stays cheap. No materialPath: the grass cache is seeded by
// hand below, so the landscape does not need a material that declares a grass type.
static ALandscape* CreateTinyLandscapeForGrassFlushTest(
    FAutomationTestBase& Test, UWorld* World, const FString& Label)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("name"), Label);
    Payload->SetNumberField(TEXT("componentsX"), 1);
    Payload->SetNumberField(TEXT("componentsY"), 1);
    Payload->SetNumberField(TEXT("quadsPerComponent"), 7);
    Payload->SetNumberField(TEXT("sectionsPerComponent"), 1);

    TSharedRef<FTestResponseCapture> Capture = MakeShared<FTestResponseCapture>();
    if (!InvokeHandlerWithSharedCapture(TEXT("landscape.create"), Payload, Capture))
    {
        Test.AddError(TEXT("landscape.create handler is not registered"));
        return nullptr;
    }
    PumpUntilCaptured(*Capture, /*TimeoutSeconds=*/30.0);
    Test.TestTrue(TEXT("landscape.create fixture succeeded"), Capture->bSuccess);
    if (!Capture->bSuccess)
    {
        return nullptr;
    }

    for (TActorIterator<ALandscape> It(World); It; ++It)
    {
        if (It->GetActorLabel().Equals(Label, ESearchCase::IgnoreCase))
        {
            return *It;
        }
    }
    Test.AddError(TEXT("landscape.create reported success but no actor with that label exists"));
    return nullptr;
}

// A ULandscapeGrassType carrying one default-constructed variety, in its own sandbox
// package so property.set can resolve it by object path.
static ULandscapeGrassType* CreateGrassTypeAssetForGrassFlushTest(const FString& PackagePath)
{
    UPackage* Package = CreatePackage(*PackagePath);
    if (!Package)
    {
        return nullptr;
    }
    const FString AssetName = FPackageName::GetLongPackageAssetName(PackagePath);
    ULandscapeGrassType* GrassType = NewObject<ULandscapeGrassType>(
        Package, FName(*AssetName), RF_Public | RF_Standalone);
    if (!GrassType)
    {
        return nullptr;
    }
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 4, 0)
    GrassType->GrassVarieties.AddDefaulted();
#else
    // FGrassVariety is declared without LANDSCAPE_API before UE 5.4, so its out-of-line
    // default constructor is not exported; the generated StaticStruct IS, and
    // InitializeStruct runs the same constructor through the Landscape module.
    FGrassVariety& Variety = GrassType->GrassVarieties[GrassType->GrassVarieties.AddZeroed()];
    FGrassVariety::StaticStruct()->InitializeStruct(&Variety);
#endif
    FAssetRegistryModule::AssetCreated(GrassType);
    return GrassType;
}

// Plants one cache entry keyed on GrassType, standing in for grass the proxy built from a
// previous version of the asset. Only BasedOn and GrassType are set: the rest of
// FGrassCompKey is default-constructed, which keeps this compiling on every supported
// engine (5.8 added a GrassName member the older keys do not have).
static void SeedStaleGrassCacheEntryForGrassFlushTest(
    ALandscapeProxy* Proxy, ULandscapeGrassType* GrassType)
{
    if (!Proxy || !GrassType || Proxy->LandscapeComponents.Num() == 0)
    {
        return;
    }
    FCachedLandscapeFoliage::FGrassComp Entry;
    Entry.Key.BasedOn = Proxy->LandscapeComponents[0].Get();
    Entry.Key.GrassType = GrassType;
    Proxy->FoliageCache.CachedGrassComps.Add(Entry);
}

// How many cache entries the proxy still holds for this grass type. This is the readback
// the fix has to move, and it reads engine state rather than anything the handler wrote.
static int32 CountCachedGrassEntriesForGrassFlushTest(
    const ALandscapeProxy* Proxy, const ULandscapeGrassType* GrassType)
{
    if (!Proxy || !GrassType)
    {
        return 0;
    }
    int32 Count = 0;
    for (const FCachedLandscapeFoliage::FGrassComp& Comp : Proxy->FoliageCache.CachedGrassComps)
    {
        if (Comp.Key.GrassType.Get() == GrassType)
        {
            ++Count;
        }
    }
    return Count;
}

// Stamps a COMPUTED grass density map onto every component of the landscape, and returns
// how many it stamped.
//
// This is the fixture for the second regression, the one the first version of these tests
// could not see (board #3/#4). A grass map is the per-component density/weight data the
// landscape material's grass output rasterises into ULandscapeComponent::GrassData; a
// freshly-built automation landscape has never had one computed, so without this seed the
// "the maps survived" assertion below would be vacuously true and would pass against the
// destructive implementation.
//
// NumElements is the engine's own validity flag, and it is written directly rather than
// through FLandscapeComponentGrassData::HasValidData(): that struct carries no
// LANDSCAPE_API (LandscapeComponent.h:207 on 5.3-5.8) so its out-of-line members do not
// link from a plugin. >= 0 means computed, UnknownNumElements (-1) means never computed
// (LandscapeComponent.h:230-233, and LandscapeGrass.cpp:1653-1659 for the accessor's body).
// ULandscapeComponent::RemoveGrassMap() installs a default-constructed struct, which reads
// -1 - so this seed is the exact state that call destroys.
static int32 SeedComputedGrassMapForGrassFlushTest(ALandscapeProxy* Proxy)
{
    if (!Proxy)
    {
        return 0;
    }
    int32 Seeded = 0;
    for (ULandscapeComponent* Component : Proxy->LandscapeComponents)
    {
        if (!Component)
        {
            continue;
        }
        Component->GrassData->HeightWeightData.SetNumZeroed(4);
        Component->GrassData->NumElements = 4;
        ++Seeded;
    }
    return Seeded;
}

// How many of the proxy's components still hold a computed grass map. Pre-fix this reads 0
// after the refresh, because FlushGrassComponents(nullptr, bFlushGrassMaps=true) calls
// RemoveGrassMap() on every one of them (5.8 LandscapeGrass.cpp:2726-2736).
static int32 CountComponentsHoldingGrassMapsForGrassFlushTest(const ALandscapeProxy* Proxy)
{
    if (!Proxy)
    {
        return 0;
    }
    int32 Count = 0;
    for (const ULandscapeComponent* Component : Proxy->LandscapeComponents)
    {
        if (Component && Component->GrassData->NumElements >= 0)
        {
            ++Count;
        }
    }
    return Count;
}

// Pulls the measured consumerRefresh block out of a response. Returns null when the verb
// did not emit one, which is itself a failure the callers assert on.
static TSharedPtr<FJsonObject> ReadConsumerRefreshForGrassFlushTest(
    const TSharedPtr<FJsonObject>& Response)
{
    const TSharedPtr<FJsonObject>* Block = nullptr;
    if (Response.IsValid() && Response->TryGetObjectField(TEXT("consumerRefresh"), Block) && Block)
    {
        return *Block;
    }
    return nullptr;
}

// Asserts the shape every consumerRefresh block owes a caller: it was measured, it found
// the landscape, and it names it in refreshed[] rather than only counting it.
static void AssertGrassRefreshCountedTheLandscape(FAutomationTestBase& Test,
    const TSharedPtr<FJsonObject>& Block, const FString& LandscapeLabel)
{
    if (!Test.TestTrue(TEXT("response carries a consumerRefresh block"), Block.IsValid()))
    {
        return;
    }

    bool bMeasured = false;
    Block->TryGetBoolField(TEXT("measured"), bMeasured);
    Test.TestTrue(TEXT("consumerRefresh.measured is true (the run actually enumerated consumers)"),
        bMeasured);

    double ConsumersFound = 0.0;
    Block->TryGetNumberField(TEXT("consumersFound"), ConsumersFound);
    Test.TestTrue(
        *FString::Printf(TEXT("consumerRefresh.consumersFound counts the seeded landscape (found %.0f)"),
            ConsumersFound),
        ConsumersFound >= 1.0);

    double ConsumersRefreshed = 0.0;
    Block->TryGetNumberField(TEXT("consumersRefreshed"), ConsumersRefreshed);
    Test.TestTrue(
        *FString::Printf(TEXT("consumerRefresh.consumersRefreshed counts the rebuild (refreshed %.0f)"),
            ConsumersRefreshed),
        ConsumersRefreshed >= 1.0);

    bool bNamed = false;
    const TArray<TSharedPtr<FJsonValue>>* Refreshed = nullptr;
    if (Block->TryGetArrayField(TEXT("refreshed"), Refreshed) && Refreshed)
    {
        for (const TSharedPtr<FJsonValue>& Value : *Refreshed)
        {
            if (Value.IsValid() && Value->AsString().Equals(LandscapeLabel, ESearchCase::IgnoreCase))
            {
                bNamed = true;
                break;
            }
        }
    }
    Test.TestTrue(
        *FString::Printf(TEXT("consumerRefresh.refreshed[] names '%s' (a count alone is not checkable)"),
            *LandscapeLabel),
        bNamed);
}

// The assertion consumerRefresh structurally cannot make: the refresh INVALIDATED the built
// grass rather than DESTROYING the data it is built from.
//
// Both outcomes empty the cache entries consumerRefresh counts, so every assertion above
// passes identically for either — which is how a version that deleted the grass density
// maps on every reflected write to a grass type shipped, reporting
// `consumersRefreshed: 1, subObjectsRefreshed: 136` as coverage while the measured field
// count was 136 destroyed. The `grassMaps` block is the second instrument, taken on both
// sides of the flush inside the same call, so it cannot be confused by anything a later
// tick does.
static void AssertGrassMapsSurvivedForGrassFlushTest(FAutomationTestBase& Test,
    const TSharedPtr<FJsonObject>& Response, int32 ExpectedComponentsHoldingMaps)
{
    const TSharedPtr<FJsonObject>* BlockPtr = nullptr;
    if (!Test.TestTrue(TEXT("response carries a grassMaps block (absent = the verb never looked)"),
            Response.IsValid() && Response->TryGetObjectField(TEXT("grassMaps"), BlockPtr)
                && BlockPtr != nullptr))
    {
        return;
    }
    const TSharedPtr<FJsonObject>& Block = *BlockPtr;

    bool bMeasured = false;
    Block->TryGetBoolField(TEXT("measured"), bMeasured);
    Test.TestTrue(TEXT("grassMaps.measured is true (the components were actually counted)"), bMeasured);

    double Before = 0.0;
    double After = 0.0;
    Block->TryGetNumberField(TEXT("componentsHoldingMapsBefore"), Before);
    Block->TryGetNumberField(TEXT("componentsHoldingMapsAfter"), After);
    Test.TestEqual(TEXT("grassMaps.componentsHoldingMapsBefore counts the seeded maps"),
        static_cast<int32>(Before), ExpectedComponentsHoldingMaps);
    Test.TestEqual(
        TEXT("grassMaps.componentsHoldingMapsAfter equals before - the refresh destroyed no "
             "grass density map"),
        static_cast<int32>(After), ExpectedComponentsHoldingMaps);

    // Omitted on every correct run, so its presence is the signal (GrassTypeConsumers.h).
    double Discarded = 0.0;
    Test.TestFalse(
        TEXT("grassMaps carries no `discarded` field (a destructive flush would add one)"),
        Block->TryGetNumberField(TEXT("discarded"), Discarded));
}

// ============================================================================
// 1. property.set on a ULandscapeGrassType invalidates the built grass.
//
// This is the ticket's own failure: a durable, correct, successfully-reported write whose
// effect never reached the thing that draws. The seam is keyed on the TARGET CLASS, not on
// the property name — the refresh fires for any reflected write to a ULandscapeGrassType —
// so a plain top-level bool stands in for the GrassVarieties edit that was measured, and
// the test does not additionally depend on the resolver's array-subscript support.
//
// DIFFERENTIAL PROOF: against the pre-fix handler the seeded entry survives the write (the
// engine's 5.4+ PostEditChangeProperty touches only the grass-type summary and StateHash)
// and no consumerRefresh block is emitted at all, so every assertion below fails while
// property.set still answers applied: true.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPropertySetGrassTypeEditFlushesGrassCacheTest,
    "PinWright.property.set.GrassTypeEditFlushesGrassCache",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPropertySetGrassTypeEditFlushesGrassCacheTest::RunTest(const FString& Parameters)
{
    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("GEditor->GetEditorWorldContext().World() is null, so no landscape proxy could be "
                 "created to hold a stale grass cache entry."));
        return true;
    }
    if (!TestTrue(TEXT("property.set handler registered"),
            IsHandlerRegistered(TEXT("property.set"))))
    {
        return false;
    }

    const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString AssetPath = FString::Printf(TEXT("/Game/__PW_GatewayTests/PW_GrassFlush_%s"), *Suffix);
    ON_SCOPE_EXIT
    {
        CleanupTestAsset(AssetPath);
    };

    ULandscapeGrassType* GrassType = CreateGrassTypeAssetForGrassFlushTest(AssetPath);
    if (!TestNotNull(TEXT("sandbox ULandscapeGrassType created"), GrassType))
    {
        return false;
    }

    FScopedEditorWorldActorGuard WorldGuard;
    const FString Label = FString::Printf(TEXT("PW_GrassFlushLandscape_%s"), *Suffix);
    ALandscape* Landscape = CreateTinyLandscapeForGrassFlushTest(*this, World, Label);
    if (!Landscape)
    {
        return false;
    }

    SeedStaleGrassCacheEntryForGrassFlushTest(Landscape, GrassType);
    if (!TestEqual(TEXT("fixture seeded exactly one stale grass cache entry"),
            CountCachedGrassEntriesForGrassFlushTest(Landscape, GrassType), 1))
    {
        return false;
    }

    const int32 SeededGrassMaps = SeedComputedGrassMapForGrassFlushTest(Landscape);
    if (!TestTrue(TEXT("fixture stamped a computed grass map on at least one component"),
            SeededGrassMaps > 0))
    {
        return false;
    }

    // The edit. bEnableDensityScaling is a plain editable UPROPERTY on ULandscapeGrassType,
    // so this exercises the generic reflected write path the ticket's GrassVarieties edit
    // also travels.
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("objectPath"), ToObjectPath(AssetPath));
    Payload->SetStringField(TEXT("propertyName"), TEXT("bEnableDensityScaling"));
    Payload->SetBoolField(TEXT("value"), false);

    TSharedRef<FTestResponseCapture> Capture = MakeShared<FTestResponseCapture>();
    if (!TestTrue(TEXT("property.set invoked"),
            InvokeHandlerWithSharedCapture(TEXT("property.set"), Payload, Capture)))
    {
        return false;
    }
    PumpUntilCaptured(*Capture, /*TimeoutSeconds=*/60.0);
    if (!TestTrue(
            *FString::Printf(TEXT("property.set on the grass type succeeded (error was '%s': %s)"),
                *Capture->ErrorCode, *Capture->Message),
            Capture->bSuccess))
    {
        return false;
    }

    // Core assertion: the stale entry is gone. Pre-fix it is still there and the write
    // still reported success, which is the whole defect.
    TestEqual(
        TEXT("the grass built from the edited type was invalidated (0 stale cache entries remain)"),
        CountCachedGrassEntriesForGrassFlushTest(Landscape, GrassType), 0);

    AssertGrassRefreshCountedTheLandscape(
        *this, ReadConsumerRefreshForGrassFlushTest(Capture->Result), Label);

    // Second core assertion, and the one the first version of this test was missing: the
    // invalidation above was an invalidation and not a destruction. This is the assertion
    // that fails against the shipped d8f1bc32 implementation, where every property.set on
    // a grass type ran FlushGrassComponents(nullptr, bFlushGrassMaps=true) and left every
    // landscape component with a freshly-allocated empty grass map.
    TestEqual(
        TEXT("every seeded grass density map survived the property.set refresh"),
        CountComponentsHoldingGrassMapsForGrassFlushTest(Landscape), SeededGrassMaps);
    AssertGrassMapsSurvivedForGrassFlushTest(*this, Capture->Result, SeededGrassMaps);

    return true;
}

// ============================================================================
// 2. landscape.flush_grass exists and does the same thing on demand.
//
// The ticket's second complaint is that the flush routes a caller reaches for by name do
// not exist: `grass.FlushCacheAll` is not a registered console command on any engine, and
// `unreal.Landscape.flush_grass_components` is not a Python attribute because
// ALandscapeProxy::FlushGrassComponents is not a UFUNCTION. Neither name appears anywhere
// in this plugin or its docs, so there was no wrong text to correct — the fix is that a
// verb with a real name now exists for edits made outside this plugin.
//
// DIFFERENTIAL PROOF: pre-fix the handler is not registered at all, so the registration
// assertion fails before anything else runs.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLandscapeFlushGrassInvalidatesStaleCacheTest,
    "PinWright.landscape.flush_grass.StaleGrassCacheIsInvalidated",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FLandscapeFlushGrassInvalidatesStaleCacheTest::RunTest(const FString& Parameters)
{
    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("GEditor->GetEditorWorldContext().World() is null, so no landscape proxy could be "
                 "created to hold a stale grass cache entry."));
        return true;
    }
    if (!TestTrue(TEXT("landscape.flush_grass handler registered"),
            IsHandlerRegistered(TEXT("landscape.flush_grass"))))
    {
        return false;
    }

    const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString AssetPath = FString::Printf(TEXT("/Game/__PW_GatewayTests/PW_GrassFlushVerb_%s"), *Suffix);
    ON_SCOPE_EXIT
    {
        CleanupTestAsset(AssetPath);
    };

    ULandscapeGrassType* GrassType = CreateGrassTypeAssetForGrassFlushTest(AssetPath);
    if (!TestNotNull(TEXT("sandbox ULandscapeGrassType created"), GrassType))
    {
        return false;
    }

    FScopedEditorWorldActorGuard WorldGuard;
    const FString Label = FString::Printf(TEXT("PW_GrassFlushVerbLandscape_%s"), *Suffix);
    ALandscape* Landscape = CreateTinyLandscapeForGrassFlushTest(*this, World, Label);
    if (!Landscape)
    {
        return false;
    }

    SeedStaleGrassCacheEntryForGrassFlushTest(Landscape, GrassType);
    if (!TestEqual(TEXT("fixture seeded exactly one stale grass cache entry"),
            CountCachedGrassEntriesForGrassFlushTest(Landscape, GrassType), 1))
    {
        return false;
    }

    const int32 SeededGrassMaps = SeedComputedGrassMapForGrassFlushTest(Landscape);
    if (!TestTrue(TEXT("fixture stamped a computed grass map on at least one component"),
            SeededGrassMaps > 0))
    {
        return false;
    }

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("grassTypePath"), ToObjectPath(AssetPath));

    TSharedRef<FTestResponseCapture> Capture = MakeShared<FTestResponseCapture>();
    if (!TestTrue(TEXT("landscape.flush_grass invoked"),
            InvokeHandlerWithSharedCapture(TEXT("landscape.flush_grass"), Payload, Capture)))
    {
        return false;
    }
    PumpUntilCaptured(*Capture, /*TimeoutSeconds=*/60.0);
    if (!TestTrue(
            *FString::Printf(TEXT("landscape.flush_grass succeeded (error was '%s': %s)"),
                *Capture->ErrorCode, *Capture->Message),
            Capture->bSuccess))
    {
        return false;
    }

    TestEqual(TEXT("landscape.flush_grass invalidated the stale cache entry"),
        CountCachedGrassEntriesForGrassFlushTest(Landscape, GrassType), 0);

    AssertGrassRefreshCountedTheLandscape(
        *this, ReadConsumerRefreshForGrassFlushTest(Capture->Result), Label);

    // The verb is a flush, not a delete, and this is the pair of assertions that says so.
    // The shipped d8f1bc32 build passed everything above while taking a live level from 136
    // grass components to 0 with no recovery short of an editor restart; a cache-entry
    // count cannot tell those apart, and this can.
    TestEqual(TEXT("every seeded grass density map survived landscape.flush_grass"),
        CountComponentsHoldingGrassMapsForGrassFlushTest(Landscape), SeededGrassMaps);
    AssertGrassMapsSurvivedForGrassFlushTest(*this, Capture->Result, SeededGrassMaps);

    // The gap has to be named, not implied: this verb reaches the editor world only.
    const TSharedPtr<FJsonObject> Block = ReadConsumerRefreshForGrassFlushTest(Capture->Result);
    if (Block.IsValid())
    {
        const TArray<TSharedPtr<FJsonValue>>* NotRefreshed = nullptr;
        TestTrue(TEXT("consumerRefresh names the consumer kinds it does not reach"),
            Block->TryGetArrayField(TEXT("notRefreshed"), NotRefreshed)
                && NotRefreshed != nullptr && NotRefreshed->Num() > 0);
    }

    return true;
}
