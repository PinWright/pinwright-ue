// Copyright (c) 2026 Alexander Penkin. MIT License.

// TestEffectGeometrySurfaceFilter.cpp - the any_solid preset's effect-card exclusion.
//
// The defect: any_solid hardcoded ExcludeNames = {"FG_*"}, one project's prefix for its haze/fog
// cards. Two consequences for anyone else - a customer actor legitimately named FG_... was
// silently disqualified from being ground, and a customer whose cards use another prefix got
// none of the protection three wiki pages advertise.
//
// The exclusion itself is load-bearing: those cards are collision-boxed effect geometry that a
// naive downward trace treats as ground, which on this project put a batch of characters ~4300 uu
// in the air. So the fix is not deletion, it is inferring effect geometry from something
// intrinsic - component class and material blend mode - which a customer gets without having read
// any of our documentation.
//
// The failure-direction assertions are the first two tests: an actor carrying the old hardcoded
// prefix must NOT be excluded, and the preset must not populate a name list at all. Both fail
// against the previous implementation. The third checks the replacement actually excludes
// something, so "nothing is excluded any more" cannot pass the suite either.
//
// Helper names carry an EffectFilterTest prefix and live in one file-scope anonymous namespace,
// per the Unity-build rule in CLAUDE.md.

#include "Misc/AutomationTest.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/HandlerRegistration.h"
#include "Handlers/Spatial/GroundPlacementUtils.h"
#include "Handlers/Spatial/SpatialTraceUtils.h"
#include "Dom/JsonObject.h"
#include "Tests/TestSkipReporting.h"
#include "Tests/TestUtils.h"
#include "Utils/ActorUtils.h"

#include "Components/PrimitiveComponent.h"
#include "Editor.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Misc/Guid.h"
#include "Misc/ScopeExit.h"

namespace
{
    // A fourth isolated column, distinct from the three the other spatial suites use. Nothing
    // here traces, but keeping fixtures apart stops a concurrent suite's actor from being
    // selected by a label search.
    constexpr double EffectFilterTestColX = -880000.0;
    constexpr double EffectFilterTestColY = 640000.0;

    UWorld* EffectFilterTestEditorWorld()
    {
        return GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    }

    TSharedPtr<FJsonObject> EffectFilterTestVec(double X, double Y, double Z)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetNumberField(TEXT("x"), X);
        Obj->SetNumberField(TEXT("y"), Y);
        Obj->SetNumberField(TEXT("z"), Z);
        return Obj;
    }

    // Deliberately FG_-prefixed: this is the customer actor the old hardcoded pattern threw away.
    FString EffectFilterTestCustomerLabel()
    {
        return FString::Printf(TEXT("FG_CustomerGround_%s"),
            *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    }

    AActor* EffectFilterTestSpawnCube(FAutomationTestBase& Test, UWorld* World, const FString& Label)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("meshPath"), TEXT("/Engine/BasicShapes/Cube.Cube"));
        Payload->SetStringField(TEXT("actorName"), Label);
        Payload->SetObjectField(TEXT("location"),
            EffectFilterTestVec(EffectFilterTestColX, EffectFilterTestColY, 0.0));

        FTestResponseCapture Capture;
        Test.TestTrue(TEXT("actor.spawn handler registered"),
            InvokeHandlerWithCapture(TEXT("actor.spawn"), Payload, Capture));
        Test.TestTrue(*FString::Printf(TEXT("cube '%s' spawned"), *Label), Capture.bSuccess);
        return McpActorUtils::FindActorByName(World, Label);
    }

    // The any_solid filter exactly as the RPC layer builds it.
    SpatialTraceUtils::FSpatialHitFilter EffectFilterTestAnySolidFilter()
    {
        GroundPlacement::FGroundSurfaceSpec Spec;
        Spec.Preset = GroundPlacement::ESurfacePreset::AnySolid;
        Spec.ApplyPreset();
        return Spec.Filter;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAnySolidPresetCarriesNoProjectNamePatternTest,
    "PinWright.spatial.surface.AnySolidCarriesNoProjectNamePattern",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAnySolidPresetCarriesNoProjectNamePatternTest::RunTest(const FString& Parameters)
{
    const SpatialTraceUtils::FSpatialHitFilter Filter = EffectFilterTestAnySolidFilter();

    // The preset must contribute NO name pattern. Any entry here is one project's naming
    // convention shipped as everyone's behaviour - the defect itself, expressed as a count.
    TestEqual(TEXT("any_solid contributes no name-pattern exclusions"),
        Filter.ExcludeNames.Num(), 0);

    // ...and must still exclude effect geometry, by the intrinsic route.
    TestTrue(TEXT("any_solid excludes effect geometry intrinsically"),
        Filter.bExcludeEffectGeometry);
    TestTrue(TEXT("any_solid still excludes foliage actors by class"),
        Filter.ExcludeClasses.Contains(TEXT("InstancedFoliageActor")));

    // A custom spec implies nothing, so a caller who asked for no help gets none.
    GroundPlacement::FGroundSurfaceSpec CustomSpec;
    CustomSpec.Preset = GroundPlacement::ESurfacePreset::Custom;
    CustomSpec.ApplyPreset();
    TestFalse(TEXT("the custom preset does not turn effect exclusion on behind the caller's back"),
        CustomSpec.Filter.bExcludeEffectGeometry);

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCustomerPrefixedGroundIsNotExcludedTest,
    "PinWright.spatial.surface.CustomerPrefixedGroundIsNotExcluded",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCustomerPrefixedGroundIsNotExcludedTest::RunTest(const FString& Parameters)
{
    UWorld* World = EffectFilterTestEditorWorld();
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("Editor world not available; skipping customer-prefix exclusion test."));
        return true;
    }

    // An ordinary opaque static mesh that happens to carry the prefix this plugin used to
    // hardcode. It is real ground and must be treated as such.
    const FString Label = EffectFilterTestCustomerLabel();
    AActor* Ground = EffectFilterTestSpawnCube(*this, World, Label);
    ON_SCOPE_EXIT
    {
        if (Ground) { Ground->Destroy(); }
    };
    if (!Ground)
    {
        AddError(TEXT("fixture cube did not spawn"));
        return true;
    }

    const SpatialTraceUtils::FSpatialHitFilter Filter = EffectFilterTestAnySolidFilter();

    // The assertion the old implementation cannot pass: ExcludeNames held "FG_*", which
    // wildcard-matches this label, so Matches() returned false and the actor was silently
    // disqualified from ever being ground.
    TestTrue(TEXT("an opaque actor named with the old hardcoded prefix is accepted as ground"),
        Filter.Matches(Ground));

    // And the intrinsic test itself must not misclassify ordinary opaque geometry - a false
    // positive here would swap one silent disqualification for another.
    TestFalse(TEXT("an opaque static mesh is not classified as effect geometry"),
        SpatialTraceUtils::IsEffectGeometryActor(Ground));

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEffectComponentActorIsExcludedTest,
    "PinWright.spatial.surface.EffectComponentActorIsExcluded",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FEffectComponentActorIsExcludedTest::RunTest(const FString& Parameters)
{
    UWorld* World = EffectFilterTestEditorWorld();
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("Editor world not available; skipping effect-component exclusion test."));
        return true;
    }

    // Resolved by reflection rather than StaticClass() so this test needs no link-time reference
    // to the component type, matching how the production predicate recognises it (by class-name
    // ancestry) and the reflection idiom in CLAUDE.md.
    UClass* BillboardClass =
        FindObject<UClass>(nullptr, TEXT("/Script/Engine.BillboardComponent"));
    if (!BillboardClass)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("component-class-unresolvable"),
            TEXT("BillboardComponent class not resolvable; skipping."));
        return true;
    }

    AActor* Card = World->SpawnActor<AActor>(AActor::StaticClass(),
        FTransform(FVector(EffectFilterTestColX, EffectFilterTestColY, 500.0)));
    ON_SCOPE_EXIT
    {
        if (Card) { Card->Destroy(); }
    };
    if (!Card)
    {
        AddError(TEXT("fixture actor did not spawn"));
        return true;
    }

    UPrimitiveComponent* Billboard =
        NewObject<UPrimitiveComponent>(Card, BillboardClass, TEXT("EffectFilterTestBillboard"));
    if (!Billboard)
    {
        AddError(TEXT("billboard component not created"));
        return true;
    }
    Card->SetRootComponent(Billboard);
    Billboard->RegisterComponent();
    // Effect cards are only a problem BECAUSE they block a trace, so the fixture has to block
    // one too; a collisionless component is correctly ignored by the predicate.
    Billboard->SetCollisionEnabled(ECollisionEnabled::QueryOnly);

    TestTrue(TEXT("an actor whose only collidable component is an effect/marker class is "
                  "classified as effect geometry"),
        SpatialTraceUtils::IsEffectGeometryActor(Card));

    const SpatialTraceUtils::FSpatialHitFilter Filter = EffectFilterTestAnySolidFilter();
    TestFalse(TEXT("any_solid rejects it as ground"), Filter.Matches(Card));

    // Turning the knob off must genuinely turn it off, so a caller whose ground really is one of
    // these is not stuck with our judgement.
    SpatialTraceUtils::FSpatialHitFilter Permissive = Filter;
    Permissive.bExcludeEffectGeometry = false;
    TestTrue(TEXT("excludeEffectGeometry:false accepts it again"), Permissive.Matches(Card));

    return true;
}
