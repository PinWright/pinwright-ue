// Copyright (c) 2026 Alexander Penkin. MIT License.

// TestInstancedSurfaceFilter.cpp - the any_solid preset's FOLIAGE exclusion, and the component
// axis it needs to hold.
//
// The defect: any_solid's advertised "except foliage" guard was one ACTOR-class entry,
// ExcludeClasses = {"InstancedFoliageActor"}, while FSpatialHitFilter::Matches took an AActor*
// and nothing else. Foliage carried as instanced components on an ORDINARY actor - the shape a
// builder reaches for when they need an addressable, rebuildable vegetation layer, because the
// level's shared AInstancedFoliageActor is not prefix-addressable - is a plain AActor, so the
// entry never fired. The guard protected only the route nobody takes, and a probe that resolved
// against such a surface reported it as ground with nothing in the response to say otherwise.
//
// The fix is a component-class axis (ExcludeComponentClasses), decided from the primitive that
// ANSWERED the query rather than from the actor, because no actor class distinguishes a scatter
// holder from any other actor. The preset populates it with the two engine component classes
// that exist for vegetation and nothing else.
//
// The failure-direction assertion is the first test: a foliage component on a plain actor must
// be rejected, and the actor axis alone must be shown to be blind to it, so "the class list
// happened to match" cannot pass for the fix.
//
// The second test is the OTHER direction, and it is the one that stops this from being a
// breaking change: a plain ISM/HISM scatter is legitimate ground - paving, rocks, modular tiles
// - and the preset must still accept it. Only a caller who says so gets it excluded. Without
// this assertion the natural over-fix (exclude every instanced component) would pass the suite
// while silently moving every actor anyone had already seated on a scatter.
//
// Helper names carry an InstancedFilterTest prefix and live in one file-scope anonymous
// namespace, per the Unity-build rule in CLAUDE.md.

#include "Misc/AutomationTest.h"
#include "Handlers/Spatial/GroundPlacementUtils.h"
#include "Handlers/Spatial/SpatialTraceUtils.h"
#include "Tests/TestSkipReporting.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#include "Components/InstancedStaticMeshComponent.h"
#include "Editor.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Misc/ScopeExit.h"
#include "UObject/Class.h"
#include "UObject/UObjectGlobals.h"

namespace
{
    // A fifth isolated column, distinct from the four the other spatial suites use, so a
    // concurrent suite's actor can never be picked up by a label search here.
    constexpr double InstancedFilterTestColX = -940000.0;
    constexpr double InstancedFilterTestColY = 700000.0;

    UWorld* InstancedFilterTestEditorWorld()
    {
        return GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    }

    // The any_solid filter exactly as the RPC layer builds it.
    SpatialTraceUtils::FSpatialHitFilter InstancedFilterTestAnySolidFilter()
    {
        GroundPlacement::FGroundSurfaceSpec Spec;
        Spec.Preset = GroundPlacement::ESurfacePreset::AnySolid;
        Spec.ApplyPreset();
        return Spec.Filter;
    }

    // An ordinary AActor - NOT an AInstancedFoliageActor - carrying one instanced component of
    // the named class. This is the whole point of the fixture: the actor is indistinguishable
    // from any other actor in the level, so only the component can say what the probe hit.
    //
    // Resolved by reflection rather than StaticClass() because UFoliageInstancedStaticMeshComponent
    // is MinimalAPI and cannot be referenced cross-module, and because it matches how the
    // production predicate recognises a class - by name ancestry. Returns null (having warned)
    // when the class is not resolvable on this host.
    UInstancedStaticMeshComponent* InstancedFilterTestMakeHolder(FAutomationTestBase& Test,
                                                                 UWorld* World,
                                                                 const TCHAR* ComponentClassPath,
                                                                 AActor*& OutActor)
    {
        OutActor = nullptr;

        UClass* ComponentClass = FindObject<UClass>(nullptr, ComponentClassPath);
        if (!ComponentClass)
        {
            PinWrightTestSkip::SkipAssertions(Test, TEXT("component-class-unresolvable"),
                FString::Printf(TEXT("'%s' did not resolve on this host, so the surface-filter "
                                     "assertions were stepped over."), ComponentClassPath));
            return nullptr;
        }

        AActor* Holder = World->SpawnActor<AActor>(AActor::StaticClass(),
            FTransform(FVector(InstancedFilterTestColX, InstancedFilterTestColY, 0.0)));
        if (!Holder)
        {
            Test.AddError(TEXT("fixture holder actor did not spawn"));
            return nullptr;
        }
        OutActor = Holder;

        UInstancedStaticMeshComponent* Instanced = NewObject<UInstancedStaticMeshComponent>(
            Holder, ComponentClass, TEXT("InstancedFilterTestScatter"));
        if (!Instanced)
        {
            Test.AddError(TEXT("instanced component not created"));
            return nullptr;
        }
        Holder->SetRootComponent(Instanced);
        Instanced->RegisterComponent();

        // A real scatter, not a bare component: two instances of a mesh, with collision on. The
        // exclusion is only ever reached by a probe that this component ANSWERED, so a fixture
        // that could not block one would be testing a state the filter never sees.
        if (UStaticMesh* Cube =
                LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube")))
        {
            Instanced->SetStaticMesh(Cube);
            Instanced->AddInstance(FTransform(FVector(0.0, 0.0, 0.0)));
            Instanced->AddInstance(FTransform(FVector(300.0, 0.0, 0.0)));
        }
        Instanced->SetCollisionEnabled(ECollisionEnabled::QueryOnly);

        return Instanced;
    }

    TSharedPtr<FJsonValue> InstancedFilterTestStr(const TCHAR* Value)
    {
        return MakeShared<FJsonValueString>(FString(Value));
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFoliageComponentOnOrdinaryActorIsExcludedTest,
    "PinWright.spatial.surface.FoliageComponentOnOrdinaryActorIsExcluded",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FFoliageComponentOnOrdinaryActorIsExcludedTest::RunTest(const FString& Parameters)
{
    UWorld* World = InstancedFilterTestEditorWorld();
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("foliage-component exclusion assertions were stepped over."));
        return true;
    }

    AActor* Holder = nullptr;
    UInstancedStaticMeshComponent* Foliage = InstancedFilterTestMakeHolder(*this, World,
        TEXT("/Script/Foliage.FoliageInstancedStaticMeshComponent"), Holder);
    ON_SCOPE_EXIT
    {
        if (Holder) { Holder->Destroy(); }
    };
    if (!Foliage)
    {
        return true;
    }

    // The premise the defect rests on, asserted rather than assumed: the holder is an ordinary
    // actor. If this ever stopped being true the test below would pass for the wrong reason.
    TestFalse(TEXT("the scatter holder is NOT an AInstancedFoliageActor"),
        Holder->GetClass()->GetName().Contains(TEXT("InstancedFoliageActor")));

    const SpatialTraceUtils::FSpatialHitFilter Filter = InstancedFilterTestAnySolidFilter();

    // The assertion the actor-only filter cannot pass: a foliage component answered the probe,
    // and any_solid promises to exclude foliage.
    TestFalse(TEXT("any_solid rejects a foliage component on an ordinary actor as ground"),
        Filter.Matches(Holder, Foliage));

    // ...and it is the COMPONENT axis that decided it. A filter carrying only the actor-class
    // entry accepts the very same hit, which is exactly the blind spot this ticket names: the
    // old guard could never have fired here, so "the class list happened to match" cannot be
    // mistaken for the fix.
    SpatialTraceUtils::FSpatialHitFilter ActorAxisOnly;
    ActorAxisOnly.ExcludeClasses.Add(TEXT("InstancedFoliageActor"));
    TestTrue(TEXT("the actor-class axis alone is blind to it"),
        ActorAxisOnly.Matches(Holder, Foliage));

    // The preset must carry both entries: the two vegetation component classes are SIBLINGS
    // under UHierarchicalInstancedStaticMeshComponent, so neither covers the other by ancestry.
    TestTrue(TEXT("any_solid excludes the foliage component class"),
        Filter.ExcludeComponentClasses.Contains(TEXT("FoliageInstancedStaticMeshComponent")));
    TestTrue(TEXT("any_solid excludes the landscape-grass component class"),
        Filter.ExcludeComponentClasses.Contains(TEXT("GrassInstancedStaticMeshComponent")));

    // A hit whose component could not be resolved must not be rejected on the strength of an
    // unknown - that would be the same silent wrong answer pointed the other way.
    TestTrue(TEXT("a hit with no resolvable component is not excluded by the component axis"),
        Filter.Matches(Holder, nullptr));

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPlainInstancedScatterIsStillGroundTest,
    "PinWright.spatial.surface.PlainInstancedScatterIsStillGround",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPlainInstancedScatterIsStillGroundTest::RunTest(const FString& Parameters)
{
    UWorld* World = InstancedFilterTestEditorWorld();
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("plain-scatter acceptance assertions were stepped over."));
        return true;
    }

    // A plain HISM scatter: paving stones, rocks, modular tiles. Same shape as the foliage
    // fixture, different component class - which is the entire distinction the preset draws.
    AActor* Holder = nullptr;
    UInstancedStaticMeshComponent* Paving = InstancedFilterTestMakeHolder(*this, World,
        TEXT("/Script/Engine.HierarchicalInstancedStaticMeshComponent"), Holder);
    ON_SCOPE_EXIT
    {
        if (Holder) { Holder->Destroy(); }
    };
    if (!Paving)
    {
        return true;
    }

    const SpatialTraceUtils::FSpatialHitFilter Filter = InstancedFilterTestAnySolidFilter();

    // The non-breaking guarantee. An ISM/HISM scatter IS legitimate ground, and a preset that
    // excluded every instanced component would move every actor already seated on one. This is
    // the assertion the natural over-fix fails.
    TestTrue(TEXT("any_solid still accepts a plain HISM scatter as ground"),
        Filter.Matches(Holder, Paving));

    // And the preset must not have reached for the generic class to get there.
    TestFalse(TEXT("any_solid does not exclude the generic instanced component class"),
        Filter.ExcludeComponentClasses.Contains(TEXT("InstancedStaticMeshComponent")));

    // The lever that did not exist before: a caller whose scatter is NOT ground can now say so
    // on an axis that can actually name it. Before this, no actor class distinguished the
    // holder and the only remaining option was a project-prefix name pattern.
    SpatialTraceUtils::FSpatialHitFilter CallerExcluded = Filter;
    CallerExcluded.ExcludeComponentClasses.Add(
        TEXT("HierarchicalInstancedStaticMeshComponent"));
    TestFalse(TEXT("a caller-supplied component class excludes the same scatter"),
        CallerExcluded.Matches(Holder, Paving));

    // The exclusion is scoped to the COMPONENT that answered, not to the actor: the same filter
    // still accepts the same actor on a hit the entry does not name. A component axis that
    // condemned the whole actor would be the actor axis again, with a longer list.
    TestTrue(TEXT("the caller's entry does not condemn the actor on a hit it does not name"),
        CallerExcluded.Matches(Holder, nullptr));

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSurfaceJsonCarriesComponentClassAxisTest,
    "PinWright.spatial.surface.SurfaceJsonCarriesComponentClassAxis",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSurfaceJsonCarriesComponentClassAxisTest::RunTest(const FString& Parameters)
{
    // The wire half: the axis is useless if a caller cannot reach it. Both spellings land in
    // the same list, and the preset's own entries survive alongside - explicit lists ADD to the
    // preset here, exactly as excludeClasses does.
    TSharedPtr<FJsonObject> SurfaceObj = MakeShared<FJsonObject>();
    SurfaceObj->SetStringField(TEXT("preset"), TEXT("any_solid"));
    SurfaceObj->SetArrayField(TEXT("excludeComponentClasses"),
        TArray<TSharedPtr<FJsonValue>>{
            InstancedFilterTestStr(TEXT("HierarchicalInstancedStaticMeshComponent"))});
    SurfaceObj->SetArrayField(TEXT("exclude_component_classes"),
        TArray<TSharedPtr<FJsonValue>>{ InstancedFilterTestStr(TEXT("SplineMeshComponent")) });

    GroundPlacement::FGroundSurfaceSpec Spec;
    TArray<FString> UnresolvedIgnores;
    FString Error;
    TestTrue(TEXT("surface spec parses"),
        GroundPlacement::ParseSurfaceJson(SurfaceObj, nullptr, Spec, UnresolvedIgnores, Error));

    TestTrue(TEXT("camelCase spelling reaches the filter"),
        Spec.Filter.ExcludeComponentClasses.Contains(
            TEXT("HierarchicalInstancedStaticMeshComponent")));
    TestTrue(TEXT("snake_case spelling reaches the same list"),
        Spec.Filter.ExcludeComponentClasses.Contains(TEXT("SplineMeshComponent")));
    TestTrue(TEXT("the caller's entries ADD to the preset rather than replacing it"),
        Spec.Filter.ExcludeComponentClasses.Contains(
            TEXT("FoliageInstancedStaticMeshComponent")));

    // A custom spec implies nothing, so a caller who asked for no help gets none.
    TSharedPtr<FJsonObject> CustomObj = MakeShared<FJsonObject>();
    CustomObj->SetStringField(TEXT("preset"), TEXT("custom"));
    GroundPlacement::FGroundSurfaceSpec CustomSpec;
    TArray<FString> CustomIgnores;
    FString CustomError;
    TestTrue(TEXT("custom surface spec parses"),
        GroundPlacement::ParseSurfaceJson(CustomObj, nullptr, CustomSpec, CustomIgnores,
                                          CustomError));
    TestEqual(TEXT("the custom preset excludes no component class behind the caller's back"),
        CustomSpec.Filter.ExcludeComponentClasses.Num(), 0);

    // An empty filter must stay empty: the new axis is part of IsEmpty(), or a filter carrying
    // only it would short-circuit to "accept everything" and never be consulted.
    SpatialTraceUtils::FSpatialHitFilter ComponentOnly;
    ComponentOnly.ExcludeComponentClasses.Add(TEXT("FoliageInstancedStaticMeshComponent"));
    TestFalse(TEXT("a filter carrying only the component axis is not IsEmpty()"),
        ComponentOnly.IsEmpty());

    return true;
}
