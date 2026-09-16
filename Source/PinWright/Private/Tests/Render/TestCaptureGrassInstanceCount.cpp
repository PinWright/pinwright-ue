// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression tests for B-capture-grass-instances-always-zero.
//
// THE DEFECT. `viewport.grass.instances` -- the one field in the block that says HOW MUCH grass was
// built -- summed `UHierarchicalInstancedStaticMeshComponent::GetInstanceCount()` over the landscape
// foliage cache. That accessor is `return PerInstanceSMData.Num();` (UE 5.8
// Runtime/Engine/Private/InstancedStaticMesh.cpp:4844-4847, declared non-virtual at
// Classes/Components/InstancedStaticMeshComponent.h:435, not overridden by HISM), and landscape
// grass never writes that array. It is not "usually empty" or "empty at this moment": it is empty
// BY ASSERTION -- `UGrassInstancedStaticMeshComponent::AcceptPrebuiltTree`, the only path by which
// a grass component ever receives instances (LandscapeGrass.cpp:3387), opens with
// `check(!PerInstanceSMData.Num())` (Runtime/Foliage/Private/InstancedGrass.cpp:52) and hands the
// built buffer straight to the render path. So the field was a structural zero over arbitrarily
// much drawn geometry: 149 grass components rendering a dense carpet summed to 0, measured three
// times at three poses beside `components` counts of 190/155/184 that moved correctly.
//
// WHY THAT IS WORSE THAN A COSMETIC ZERO. `settled: true` with `instances: 0` was DOCUMENTED as
// meaning "that ground really is bare" (docs/wiki-src/render.md). The field could not produce any
// other reading, so following the documentation turned a false negative into an affirmative and
// wrong statement about the level.
//
// WHERE THE COUNT ACTUALLY LIVES. Not in `PerInstanceRenderData` -- the ticket proposed that and it
// does not exist on UE 5.8's `UInstancedStaticMeshComponent` at all (only a forward declaration
// survives, InstancedStaticMeshComponent.h:26). The live quantity is `NumBuiltRenderInstances`,
// which `AcceptPrebuiltTree` sets from `InstanceBuffer.GetNumInstances()` (InstancedGrass.cpp:55)
// and which the grass component publishes through its override of the public virtual
// `UInstancedStaticMeshComponent::GetNumRenderInstances()`
// ("Number of instances in the render-side instance buffer", InstancedStaticMeshComponent.h:639;
// the override is GrassInstancedStaticMeshComponent.h:22). Same signature on 5.3 through 5.8.
//
// COUNTERFACTUAL. Put `GetInstanceCount()` back into `GrassComponentInstanceCount` and the first
// test below reads 0 against a component holding 41237 instances. Publish `instances`
// unconditionally again and the second test can no longer tell an unmeasurable total from measured
// bare ground -- which is the whole reason the field exists.

#include "Misc/AutomationTest.h"

#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "Dom/JsonObject.h"
#include "Handlers/Render/LandscapeGrassSettle.h"
#include "Tests/TestSkipReporting.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UObjectGlobals.h"

namespace
{
    using PinWrightCaptureGrass::FGrassBuildReport;

    // File-unique names throughout. Unity merges the Tests/Render/ translation units, so a helper
    // called anything as generic as `MakeReport` or `MakeComponent` here would redefine a sibling
    // file's (docs/lessons.md).

    // A component in EXACTLY the state landscape grass leaves one in: `PerInstanceSMData` empty,
    // `NumBuiltRenderInstances` carrying the built count. Both halves are what
    // `AcceptPrebuiltTree` does (InstancedGrass.cpp:52-56), so a fixture that skipped either would
    // be testing something other than grass.
    //
    // Resolved by reflection rather than `NewObject<UGrassInstancedStaticMeshComponent>`: the class
    // is `MinimalAPI` in another module, and the plugin's standing convention for such types is
    // `FindObject<UClass>` + `NewObject<ExportedBase>` (CLAUDE.md, UE version compat). It is also
    // the more faithful fixture -- the production loop only ever holds the base pointer.
    UHierarchicalInstancedStaticMeshComponent* PinWrightGrassInstancesMakeGrassComponent(
        int32 BuiltRenderInstances)
    {
        UClass* GrassClass = FindObject<UClass>(nullptr,
            TEXT("/Script/Foliage.GrassInstancedStaticMeshComponent"));
        if (!GrassClass)
        {
            return nullptr;
        }
        UHierarchicalInstancedStaticMeshComponent* Component =
            NewObject<UHierarchicalInstancedStaticMeshComponent>(GetTransientPackage(), GrassClass);
        if (Component)
        {
            // What AcceptPrebuiltTree writes. PerInstanceSMData is deliberately left untouched --
            // that call asserts it is empty.
            Component->NumBuiltRenderInstances = BuiltRenderInstances;
            Component->InstanceCountToRender = BuiltRenderInstances;
        }
        return Component;
    }

    // A settled build over a landscape, with every cache entry readable. The zero instance count
    // this carries is a MEASUREMENT -- the bare-ground reading the block exists to make sayable.
    FGrassBuildReport PinWrightGrassInstancesMakeBareGroundReport()
    {
        FGrassBuildReport Report;
        Report.bMeasured = true;
        Report.LandscapeProxies = 1;
        Report.bBuiltForPose = true;
        Report.CameraLocation = FVector(400.0, 800.0, 300.0);
        Report.ComponentsBefore = 6;
        Report.ComponentsAfter = 6;
        Report.InstancesAfter = 0;
        Report.bInstancesMeasured = true;
        Report.bSettled = true;
        Report.BuildMs = 8.0;
        return Report;
    }

    bool PinWrightGrassInstancesHasWarning(const TSharedPtr<FJsonObject>& Grass, FString& OutText)
    {
        OutText.Reset();
        return Grass.IsValid() && Grass->TryGetStringField(TEXT("grassWarning"), OutText);
    }
}

// ============================================================================
// The count itself, against a component in the state landscape grass produces.
// ============================================================================

// THE test for this ticket. A grass component holding instances must be counted as holding them.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCaptureGrassInstancesAreCountedTest,
    "PinWright.render.grass_instances.LandscapeGrassInstancesAreCounted",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCaptureGrassInstancesAreCountedTest::RunTest(const FString& Parameters)
{
    constexpr int32 BuiltInstances = 41237;
    UHierarchicalInstancedStaticMeshComponent* Grass =
        PinWrightGrassInstancesMakeGrassComponent(BuiltInstances);
    if (!Grass)
    {
        // The Foliage module is a hard dependency of this plugin, so this is a broken host rather
        // than an absent optional feature -- but it is still an environment fact, and stepping over
        // the assertions silently is what the skip marker exists to prevent.
        PinWrightTestSkip::SkipAssertions(*this, TEXT("grass-component-class-unresolved"),
            TEXT("FindObject on /Script/Foliage.GrassInstancedStaticMeshComponent returned null, "
                 "so no component in the landscape-grass state could be built."));
        return true;
    }
    const TStrongObjectPtr<UHierarchicalInstancedStaticMeshComponent> GrassGuard(Grass);

    // The defect, stated as an assertion rather than as a comment: the OLD instrument reads zero
    // off a component holding 41237 instances. If this ever stops being true the fallback below
    // starts carrying the grass case and this file needs re-reading.
    TestEqual(TEXT("the old instrument (GetInstanceCount / PerInstanceSMData.Num) reads zero off "
                   "a grass component, which is why it could never report grass"),
        Grass->GetInstanceCount(), 0);

    // The fix. This is the expression the production loops now sum.
    TestEqual(TEXT("a grass component holding 41237 built instances is counted as holding them"),
        PinWrightCaptureGrass::GrassComponentInstanceCount(*Grass), BuiltInstances);

    // Non-zero is the property that matters to a caller, asserted separately from the exact value
    // so a future change to the fixture's number cannot quietly satisfy only the equality above.
    TestTrue(TEXT("the count a caller reads is non-zero for grass that exists"),
        PinWrightCaptureGrass::GrassComponentInstanceCount(*Grass) > 0);

    // The other direction, so the fix cannot be "always return the render count": a hand-authored
    // component whose cluster tree has not been built yet holds its instances in PerInstanceSMData
    // and reads zero through the virtual (HISM's own override is `SortedInstances.Num()`,
    // HierarchicalInstancedStaticMeshComponent.h:342). Both kinds of component have to be counted.
    UHierarchicalInstancedStaticMeshComponent* Authored =
        NewObject<UHierarchicalInstancedStaticMeshComponent>(GetTransientPackage());
    const TStrongObjectPtr<UHierarchicalInstancedStaticMeshComponent> AuthoredGuard(Authored);
    if (Authored)
    {
        constexpr int32 AuthoredInstances = 7;
        Authored->PerInstanceSMData.AddDefaulted(AuthoredInstances);
        TestEqual(TEXT("an unbuilt authored HISM reports nothing through the render-side count"),
            Authored->GetNumRenderInstances(), 0);
        TestEqual(TEXT("and its authored instances are still counted"),
            PinWrightCaptureGrass::GrassComponentInstanceCount(*Authored), AuthoredInstances);
    }
    else
    {
        AddError(TEXT("a plain UHierarchicalInstancedStaticMeshComponent could not be created, so "
                      "the authored-instance direction of the count went unchecked"));
    }

    // Genuinely empty stays genuinely empty. Without this the fix could report any non-zero
    // constant and the assertions above would still pass.
    UHierarchicalInstancedStaticMeshComponent* Empty =
        PinWrightGrassInstancesMakeGrassComponent(0);
    const TStrongObjectPtr<UHierarchicalInstancedStaticMeshComponent> EmptyGuard(Empty);
    if (Empty)
    {
        TestEqual(TEXT("a grass component that built nothing is counted as zero"),
            PinWrightCaptureGrass::GrassComponentInstanceCount(*Empty), 0);
    }
    return true;
}

// ============================================================================
// The response contract: a total that could not be taken is omitted, not zeroed.
// ============================================================================

// The house rule this ticket exists to restore. `instances: 0` beside `settled: true` is the
// caller's evidence that ground is bare; it is worth nothing if a total nobody could take reads the
// same. docs/rpc-design.md section 4.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCaptureGrassUnreadableCountOmittedTest,
    "PinWright.render.grass_instances.UnreadableCountIsOmittedNotZeroed",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCaptureGrassUnreadableCountOmittedTest::RunTest(const FString& Parameters)
{
    // Measured bare ground: every component readable, and none of them holding anything.
    const FGrassBuildReport BareGround = PinWrightGrassInstancesMakeBareGroundReport();
    const TSharedPtr<FJsonObject> BareJson =
        PinWrightCaptureGrass::MakeGrassBuildInfoObject(BareGround);
    double BareInstances = -1.0;
    const bool bBarePublishesCount =
        BareJson->TryGetNumberField(TEXT("instances"), BareInstances);
    TestTrue(TEXT("a complete count is published even when it is zero"), bBarePublishesCount);
    TestEqual(TEXT("and it is the zero that means bare ground"), BareInstances, 0.0);
    FString BareWarning;
    TestFalse(TEXT("measured bare ground carries no warning -- warning about the honest answer is "
                   "what trains callers to discount it"),
        PinWrightGrassInstancesHasWarning(BareJson, BareWarning));
    double BareUnreadable = -1.0;
    TestFalse(TEXT("and nothing is reported unreadable"),
        BareJson->TryGetNumberField(TEXT("unreadableComponents"), BareUnreadable));

    // The same picture, from a survey that could not read every component. Before the fix this
    // published `instances: 0` and said nothing, so it was byte-identical to the case above.
    FGrassBuildReport Incomplete = PinWrightGrassInstancesMakeBareGroundReport();
    Incomplete.InstancesAfter = 0;
    Incomplete.UnreadableComponents = 3;
    Incomplete.bInstancesMeasured = false;
    const TSharedPtr<FJsonObject> IncompleteJson =
        PinWrightCaptureGrass::MakeGrassBuildInfoObject(Incomplete);
    double IncompleteInstances = -1.0;
    const bool bIncompletePublishesCount =
        IncompleteJson->TryGetNumberField(TEXT("instances"), IncompleteInstances);
    TestFalse(TEXT("a total that could not be taken is OMITTED rather than emitted as zero"),
        bIncompletePublishesCount);
    double Unreadable = 0.0;
    TestTrue(TEXT("the response says how many components could not be read"),
        IncompleteJson->TryGetNumberField(TEXT("unreadableComponents"), Unreadable));
    TestEqual(TEXT("and how many that was"), Unreadable, 3.0);
    FString IncompleteWarning;
    TestTrue(TEXT("an absent count is warned about, so it cannot read as an older server"),
        PinWrightGrassInstancesHasWarning(IncompleteJson, IncompleteWarning));
    TestTrue(FString::Printf(
            TEXT("the warning tells the caller NOT to read the absence as zero (got: %s)"),
            *IncompleteWarning),
        IncompleteWarning.Contains(TEXT("do NOT read")));

    // The decisive property, asserted on the field a caller branches on rather than inferred: the
    // two responses above are distinguishable.
    TestTrue(TEXT("measured bare ground and an unmeasurable total differ in whether `instances` is "
                  "published at all"),
        bBarePublishesCount != bIncompletePublishesCount);

    // The neighbouring counts stay published on the incomplete branch -- `components` is measured
    // independently of any component pointer resolving, and it is the field the warning sends the
    // caller to.
    double Components = -1.0;
    TestTrue(TEXT("the component count survives an unreadable entry"),
        IncompleteJson->TryGetNumberField(TEXT("components"), Components));
    TestEqual(TEXT("and still reports every cache entry"), Components, 6.0);
    return true;
}
