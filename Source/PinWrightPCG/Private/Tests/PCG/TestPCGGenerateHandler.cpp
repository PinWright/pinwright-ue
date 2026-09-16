// Copyright (c) 2026 Alexander Penkin. MIT License.

// Red test for ticket F-pcg-generate-readback: PinWright can author a UPCGGraph
// (create/add_node/connect_pins/filters/subgraph/graph parameters) but has NO RPC
// that drives a placed actor's UPCGComponent generation and reads back the produced
// point count. The proposed verb is `pcg.generate`.
//
// This is a capability gap: no `pcg.generate` handler is registered. The verb's
// param names and its result contract (which field carries the output point count,
// what the async completion payload looks like) are the implementer's design space,
// so they cannot be asserted here without pinning the test to a guessed shape that
// would stay red after a correct fix. This test therefore asserts the one thing that
// is safely differential today: the handler is registered. It fails now (verb absent)
// and flips green when `pcg.generate` is registered.
//
// The sibling pcg.* handlers register inside `#if __has_include("PCGGraph.h")`
// (PCGGraphCreate.cpp etc.), so the fix's registration will be under the same guard;
// this test mirrors that guard so it only exists where the handler can exist.

#if defined(__has_include) && __has_include("PCGGraph.h")

#include "Misc/AutomationTest.h"
#include "Misc/Guid.h"
#include "Misc/ScopeExit.h"
#include "Compat/EngineVersionCompat.h"
#include "State/JobRegistry.h"
#include "State/PluginState.h"
#include "Tests/Infra/WikiDocTestHelpers.h"
#include "Tests/TestSkipReporting.h"
#include "Tests/TestUtils.h"

#include "Dom/JsonObject.h"
#include "Editor.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "HAL/PlatformTime.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"
// FindFProperty / FStructProperty — UPCGComponent::GeneratedGraphOutput has a getter but no
// setter, so the non-point-output fixture writes it through reflection.
#include "UObject/UnrealType.h"

#include "Handlers/PCG/PCGGenerateReadback.h"
#include "Data/PCGPointData.h"
#include "PCGComponent.h"
#include "PCGGraph.h"
#include "PCGManagedResource.h"
#include "PCGParamData.h"

#include "Components/InstancedStaticMeshComponent.h"
#include "Engine/StaticMesh.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPCGGenerateHandlerRegisteredTest,
    "PinWright.pcg.generate.HandlerRegistered",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPCGGenerateHandlerRegisteredTest::RunTest(const FString& Parameters)
{
    // Pre-fix: no pcg.generate handler exists in Handlers/PCG/, so this is false and
    // the test fails. Post-fix: registering the handler flips it green.
    TestTrue(TEXT("pcg.generate handler is registered"),
        IsHandlerRegistered(TEXT("pcg.generate")));
    return true;
}

// ---------------------------------------------------------------------------
// Point-count readback. Exercises PinWrightPCG::CountGeneratedPoints — the exact
// production symbol pcg.generate resolves its async response with — against a
// synthetic output collection with a known point yield. This is the load-bearing
// readback logic; the test references the helper directly, so reverting the fix
// (removing the helper) fails compilation (differential by construction).
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPCGGeneratePointCountReadbackTest,
    "PinWright.pcg.generate.PointCountReadback",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPCGGeneratePointCountReadbackTest::RunTest(const FString& Parameters)
{
    // Two point-data entries (5 + 3 points) plus one non-point datum. The readback
    // must sum ONLY the point data (8) and skip the non-point entry, while counting
    // all three emitted items.
    UPCGPointData* FivePoints = NewObject<UPCGPointData>(GetTransientPackage());
    UPCGPointData* ThreePoints = NewObject<UPCGPointData>(GetTransientPackage());
    UPCGParamData* NonPoint = NewObject<UPCGParamData>(GetTransientPackage());
    TestNotNull(TEXT("five-point data created"), FivePoints);
    TestNotNull(TEXT("three-point data created"), ThreePoints);
    TestNotNull(TEXT("non-point data created"), NonPoint);
    if (!FivePoints || !ThreePoints || !NonPoint) return true;

    // SetNumPoints/GetNumPoints are the UPCGBasePointData (5.6+) seam; before that the
    // point array is sized through GetMutablePoints(). Size the fixtures with whichever
    // the running engine has — CountGeneratedPoints reads the same array either way.
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 6, 0)
    FivePoints->SetNumPoints(5);
    ThreePoints->SetNumPoints(3);
    TestEqual(TEXT("five-point fixture has 5 points"), FivePoints->GetNumPoints(), 5);
    TestEqual(TEXT("three-point fixture has 3 points"), ThreePoints->GetNumPoints(), 3);
#else
    FivePoints->GetMutablePoints().SetNum(5);
    ThreePoints->GetMutablePoints().SetNum(3);
    TestEqual(TEXT("five-point fixture has 5 points"), FivePoints->GetPoints().Num(), 5);
    TestEqual(TEXT("three-point fixture has 3 points"), ThreePoints->GetPoints().Num(), 3);
#endif

    FPCGDataCollection Collection;
    Collection.TaggedData.Emplace_GetRef().Data = FivePoints;
    Collection.TaggedData.Emplace_GetRef().Data = ThreePoints;
    Collection.TaggedData.Emplace_GetRef().Data = NonPoint;

    TestEqual(TEXT("point count sums only point data (5 + 3; non-point ignored)"),
        static_cast<int32>(PinWrightPCG::CountGeneratedPoints(Collection)), 8);
    TestEqual(TEXT("data count includes every emitted datum"),
        PinWrightPCG::CountGeneratedData(Collection), 3);
    return true;
}

// ---------------------------------------------------------------------------
// THE REGRESSION — B-pcg-generated-graph-output-empty-after-generate.
//
// pcg.generate reported `pointCount: 0` alongside success for generations that had
// demonstrably spawned 97 and 64 ISM instances. The cause is not a bug in the counting
// arithmetic: UPCGComponent::GetGeneratedGraphOutput() is populated ONLY from the data
// that reached the GRAPH'S OUTPUT NODE (UE 5.8 PCGComponent.cpp:637 clears it, :660
// iterates Context->InputData.TaggedData, :750 fills it), so a graph that ends in a
// Static Mesh Spawner without wiring its Out pin to the graph Output node emits nothing
// there while spawning thousands of instances.
//
// This test reproduces exactly that state synthetically — empty graph output, real
// managed ISM resource carrying a known instance count — and asserts:
//   1. the instance count is read correctly from the managed resources (the honest number),
//   2. the payload OMITS pointCount rather than reporting a bare 0,
//   3. HasProducedOutput() still says "this produced something".
//
// Differential by construction: the pre-fix readback returns 0 instances and the pre-fix
// payload always writes pointCount, so both assertions fail without the fix.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPCGGenerateCountsSpawnedInstancesTest,
    "PinWright.pcg.generate.CountsSpawnedInstancesNotBareZero",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPCGGenerateCountsSpawnedInstancesTest::RunTest(const FString& Parameters)
{
    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("GEditor->GetEditorWorldContext().World() returned null, so the managed-ISM "
                 "fixture could not be built and the instance-count readback assertions "
                 "could not run."));
        return true;
    }

    AActor* Actor = World->SpawnActor<AActor>();
    if (!Actor)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("fixture-unavailable"),
            TEXT("World->SpawnActor<AActor>() returned null, so the instance-count readback "
                 "assertions could not run."));
        return true;
    }
    ON_SCOPE_EXIT { if (IsValid(Actor)) { World->DestroyActor(Actor); } };

    Actor->SetActorLabel(FString::Printf(TEXT("MCP_PcgGenCount_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits)));

    UPCGComponent* PcgComp = NewObject<UPCGComponent>(Actor, UPCGComponent::StaticClass(),
        NAME_None, RF_Transactional);
    if (!PcgComp)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("fixture-unavailable"),
            TEXT("NewObject<UPCGComponent>() returned null, so the instance-count readback "
                 "assertions could not run."));
        return true;
    }
    Actor->AddInstanceComponent(PcgComp);
    PcgComp->RegisterComponent();

    // A real ISM component with a known, independently readable instance count. This is
    // the same shape PCG's Static Mesh Spawner leaves behind, and GetInstanceCount() is
    // the same accessor the live verification used through Python.
    UInstancedStaticMeshComponent* ISMC = NewObject<UInstancedStaticMeshComponent>(Actor);
    if (!ISMC)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("fixture-unavailable"),
            TEXT("NewObject<UInstancedStaticMeshComponent>() returned null, so the "
                 "instance-count readback assertions could not run."));
        return true;
    }
    Actor->AddInstanceComponent(ISMC);
    ISMC->RegisterComponent();

    constexpr int32 ExpectedInstances = 97;
    for (int32 Index = 0; Index < ExpectedInstances; ++Index)
    {
        ISMC->AddInstance(FTransform(FVector(Index * 100.0, 0.0, 0.0)));
    }
    // Independently obtained count — asserted against the readback below so the test does
    // not simply restate its own fixture constant. A meshless ISMC that refuses instances
    // is a fixture problem, not a readback regression, so skip rather than report a
    // failure that would point at the wrong code.
    const int32 IndependentInstanceCount = ISMC->GetInstanceCount();
    if (IndependentInstanceCount != ExpectedInstances)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("fixture-did-not-populate"),
            FString::Printf(
                TEXT("ISM fixture holds %d of %d instances in this world, so the readback "
                     "could not be matched against an independent count."),
                IndependentInstanceCount, ExpectedInstances));
        return true;
    }

    // Register it as a PCG-managed resource, exactly as the spawner element does.
    UPCGManagedISMComponent* ManagedISM = NewObject<UPCGManagedISMComponent>(PcgComp);
    if (!ManagedISM)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("fixture-unavailable"),
            TEXT("NewObject<UPCGManagedISMComponent>() returned null, so the instance-count "
                 "readback assertions could not run."));
        return true;
    }
    ManagedISM->SetComponent(ISMC);
    PcgComp->AddToManagedResources(ManagedISM);

    const PinWrightPCG::FGenerationReadback Readback = PinWrightPCG::ReadGeneration(PcgComp);

    // The precondition that made the old code lie: no graph output at all.
    TestFalse(TEXT("graph output is empty (the state that produced the false 0)"),
        Readback.bGraphOutputAvailable);

    // The honest number, matched against the independently obtained instance count.
    TestTrue(TEXT("managed-resource counts are available"), Readback.bResourceCountsAvailable);
    TestEqual(TEXT("instance count matches the ISM's own GetInstanceCount()"),
        static_cast<int32>(Readback.InstanceCount), IndependentInstanceCount);
    TestEqual(TEXT("one instanced component was counted"), Readback.InstancedComponentCount, 1);

    // A generation in this state HAS produced something; the old graph-output-only gate said no.
    TestTrue(TEXT("HasProducedOutput reports work was done despite empty graph output"),
        PinWrightPCG::HasProducedOutput(Readback));

    // ---- The payload contract: never a bare 0 alongside success. ----
    TSharedPtr<FJsonObject> Payload = PinWrightPCG::BuildGenerationPayload(
        Readback, TEXT("TestActor"), TEXT("TestGraph"), TEXT("/Game/TestGraph"),
        TEXT("generated_delegate"), 1);
    if (!Payload.IsValid())
    {
        AddError(TEXT("BuildGenerationPayload returned no payload"));
        return true;
    }

    // THE load-bearing assertion. Pre-fix this field was always written, as 0.
    TestFalse(TEXT("pointCount is OMITTED, not reported as a bare 0"),
        Payload->HasField(TEXT("pointCount")));
    TestFalse(TEXT("dataCount is OMITTED, not reported as a bare 0"),
        Payload->HasField(TEXT("dataCount")));

    // The omission must be detectable rather than looking like a dropped field.
    bool bGraphOutputAvailable = true;
    TestTrue(TEXT("graphOutputAvailable is always present"),
        Payload->TryGetBoolField(TEXT("graphOutputAvailable"), bGraphOutputAvailable));
    TestFalse(TEXT("graphOutputAvailable is false, naming the omission"), bGraphOutputAvailable);

    // And the real count must be there, under a name that says what it counted.
    double ReportedInstances = 0.0;
    TestTrue(TEXT("instanceCount is present"),
        Payload->TryGetNumberField(TEXT("instanceCount"), ReportedInstances));
    TestEqual(TEXT("instanceCount carries the true spawned-instance count"),
        static_cast<int32>(ReportedInstances), IndependentInstanceCount);

    // The engine limitation must be stated, not left for the caller to infer.
    const TArray<TSharedPtr<FJsonValue>>* Warnings = nullptr;
    TestTrue(TEXT("warnings[] explains the omission"),
        Payload->TryGetArrayField(TEXT("warnings"), Warnings));
    if (Warnings)
    {
        FString Joined;
        for (const TSharedPtr<FJsonValue>& Value : *Warnings)
        {
            if (Value.IsValid()) { Joined += Value->AsString(); }
        }
        TestTrue(TEXT("warning names the graph Output node as the reason"),
            Joined.Contains(TEXT("Output node")));
        TestTrue(TEXT("warning steers the caller to instanceCount"),
            Joined.Contains(TEXT("instanceCount")));
    }
    return true;
}

// ---------------------------------------------------------------------------
// The complementary half: when the graph DOES wire data into its Output node, the
// point count is reported and is not suppressed. Without this, "omit pointCount" could
// be satisfied by never reporting it at all.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPCGGeneratePayloadReportsRealPointCountTest,
    "PinWright.pcg.generate.PayloadReportsRealPointCount",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPCGGeneratePayloadReportsRealPointCountTest::RunTest(const FString& Parameters)
{
    PinWrightPCG::FGenerationReadback Readback;
    Readback.bGraphOutputAvailable = true;
    Readback.PointCount = 8;
    Readback.DataCount = 3;
    // Two of the three emitted data objects were point data — which is what makes
    // PointCount a measurement rather than an unmeasured 0
    // (B-pcg-generate-blind-to-non-point-data). Without it the payload rightly omits
    // pointCount, so this fixture has to state it.
    Readback.PointDataCount = 2;
    Readback.bResourceCountsAvailable = true;
    Readback.InstanceCount = 0;

    TSharedPtr<FJsonObject> Payload = PinWrightPCG::BuildGenerationPayload(
        Readback, TEXT("A"), TEXT("G"), TEXT("/Game/G"), TEXT("generated_delegate"), 0);
    if (!Payload.IsValid())
    {
        AddError(TEXT("BuildGenerationPayload returned no payload"));
        return true;
    }

    double PointCount = -1.0;
    TestTrue(TEXT("pointCount is present when the graph output really has data"),
        Payload->TryGetNumberField(TEXT("pointCount"), PointCount));
    TestEqual(TEXT("pointCount is the real value"), static_cast<int32>(PointCount), 8);

    double DataCount = -1.0;
    TestTrue(TEXT("dataCount is present"),
        Payload->TryGetNumberField(TEXT("dataCount"), DataCount));
    TestEqual(TEXT("dataCount is the real value"), static_cast<int32>(DataCount), 3);

    // A genuine zero-instance result is still reported as 0 — the fix suppresses only the
    // numbers that cannot be known, never the ones that can.
    double InstanceCount = -1.0;
    TestTrue(TEXT("instanceCount is present when resource counts are readable"),
        Payload->TryGetNumberField(TEXT("instanceCount"), InstanceCount));
    TestEqual(TEXT("a genuine 0 instances is still reported"), static_cast<int32>(InstanceCount), 0);
    return true;
}

// ---------------------------------------------------------------------------
// THE REGRESSION — B-pcg-generate-instancecount-blind-to-species.
//
// pcg.generate reported a single instanceCount for a generation that spawned several
// DIFFERENT meshes, and the response named no mesh anywhere. Four materially different
// edits to one graph's weighted mesh entries (three spawners re-meshed, a species dropped
// from a band, weights changed) each returned a byte-identical 26664, because
// UPCGMeshSelectorWeighted emits exactly one instance per input point (UE 5.8
// PCGMeshSelectorWeighted.cpp:215 loops to GetNumPoints(), :243 emplaces one transform) —
// weights partition a fixed point set and no mesh-entry edit can move the sum. The number
// is arithmetically correct and structurally unable to say WHAT was spawned.
//
// This test builds the state the total cannot describe: one PCG component managing two
// ISM components carrying two DISTINCT static meshes with different instance counts, and
// asserts the response attributes the total per species and that the attribution is
// complete (the rows sum to instanceCount).
//
// Differential by construction: the pre-fix payload has no instancesByMesh field at all,
// so the presence assertion fails; and a fix that reported only one component's mesh, or
// assigned instead of accumulated, fails the sum.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPCGGenerateAttributesInstancesByMeshTest,
    "PinWright.pcg.generate.AttributesInstancesByMesh",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPCGGenerateAttributesInstancesByMeshTest::RunTest(const FString& Parameters)
{
    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("GEditor->GetEditorWorldContext().World() returned null, so the two-species "
                 "managed-ISM fixture could not be built and the per-mesh attribution "
                 "assertions could not run."));
        return true;
    }

    // Two meshes that ship with every UE install, so the fixture needs no project content.
    UStaticMesh* MeshA = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
    UStaticMesh* MeshB = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Sphere.Sphere"));
    if (!MeshA || !MeshB || MeshA == MeshB)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("fixture-unavailable"),
            TEXT("/Engine/BasicShapes/Cube.Cube and /Engine/BasicShapes/Sphere.Sphere did not both "
                 "load as distinct meshes, so the per-mesh attribution assertions could not run."));
        return true;
    }

    AActor* Actor = World->SpawnActor<AActor>();
    if (!Actor)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("fixture-unavailable"),
            TEXT("World->SpawnActor<AActor>() returned null, so the per-mesh attribution "
                 "assertions could not run."));
        return true;
    }
    ON_SCOPE_EXIT { if (IsValid(Actor)) { World->DestroyActor(Actor); } };

    Actor->SetActorLabel(FString::Printf(TEXT("MCP_PcgGenSpecies_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits)));

    UPCGComponent* PcgComp = NewObject<UPCGComponent>(Actor, UPCGComponent::StaticClass(),
        NAME_None, RF_Transactional);
    if (!PcgComp)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("fixture-unavailable"),
            TEXT("NewObject<UPCGComponent>() returned null, so the per-mesh attribution "
                 "assertions could not run."));
        return true;
    }
    Actor->AddInstanceComponent(PcgComp);
    PcgComp->RegisterComponent();

    // Deliberately different counts: equal counts would let a fix that reported one row
    // twice, or halved the total, still satisfy the sum.
    constexpr int32 ExpectedA = 11;
    constexpr int32 ExpectedB = 4;

    auto AddManagedSpecies = [&](UStaticMesh* Mesh, int32 InstanceCount) -> UInstancedStaticMeshComponent*
    {
        UInstancedStaticMeshComponent* ISMC = NewObject<UInstancedStaticMeshComponent>(Actor);
        if (!ISMC)
        {
            return nullptr;
        }
        ISMC->SetStaticMesh(Mesh);
        Actor->AddInstanceComponent(ISMC);
        ISMC->RegisterComponent();
        for (int32 Index = 0; Index < InstanceCount; ++Index)
        {
            ISMC->AddInstance(FTransform(FVector(Index * 100.0, 0.0, 0.0)));
        }

        UPCGManagedISMComponent* ManagedISM = NewObject<UPCGManagedISMComponent>(PcgComp);
        if (!ManagedISM)
        {
            return nullptr;
        }
        ManagedISM->SetComponent(ISMC);
        PcgComp->AddToManagedResources(ManagedISM);
        return ISMC;
    };

    UInstancedStaticMeshComponent* ISMA = AddManagedSpecies(MeshA, ExpectedA);
    UInstancedStaticMeshComponent* ISMB = AddManagedSpecies(MeshB, ExpectedB);
    if (!ISMA || !ISMB)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("fixture-unavailable"),
            TEXT("One of the two managed ISM fixtures could not be constructed, so the per-mesh "
                 "attribution assertions could not run."));
        return true;
    }

    // Independently obtained, read off the components themselves rather than restated from
    // the fixture constants — a meshless or refusing ISMC is a fixture problem, not a
    // readback regression, so skip rather than report a failure pointing at the wrong code.
    const int32 IndependentA = ISMA->GetInstanceCount();
    const int32 IndependentB = ISMB->GetInstanceCount();
    if (IndependentA != ExpectedA || IndependentB != ExpectedB)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("fixture-did-not-populate"),
            FString::Printf(
                TEXT("ISM fixtures hold %d/%d and %d/%d instances in this world, so the per-mesh "
                     "attribution could not be matched against independent counts."),
                IndependentA, ExpectedA, IndependentB, ExpectedB));
        return true;
    }
    const FString PathA = ISMA->GetStaticMesh()->GetPathName();
    const FString PathB = ISMB->GetStaticMesh()->GetPathName();

    const PinWrightPCG::FGenerationReadback Readback = PinWrightPCG::ReadGeneration(PcgComp);
    TestTrue(TEXT("managed-resource counts are available"), Readback.bResourceCountsAvailable);
    TestEqual(TEXT("both instanced components were counted"), Readback.InstancedComponentCount, 2);

    TSharedPtr<FJsonObject> Payload = PinWrightPCG::BuildGenerationPayload(
        Readback, TEXT("TestActor"), TEXT("TestGraph"), TEXT("/Game/TestGraph"),
        TEXT("generated_delegate"), 1);
    if (!Payload.IsValid())
    {
        AddError(TEXT("BuildGenerationPayload returned no payload"));
        return true;
    }

    double ReportedTotal = -1.0;
    TestTrue(TEXT("instanceCount is present"),
        Payload->TryGetNumberField(TEXT("instanceCount"), ReportedTotal));
    TestEqual(TEXT("instanceCount is the sum of both species"),
        static_cast<int32>(ReportedTotal), IndependentA + IndependentB);

    // THE load-bearing assertion. Pre-fix this field does not exist, so the total was the
    // only thing the caller could read and two different graphs produced the same answer.
    const TArray<TSharedPtr<FJsonValue>>* Rows = nullptr;
    if (!Payload->TryGetArrayField(TEXT("instancesByMesh"), Rows) || !Rows)
    {
        AddError(TEXT("instancesByMesh is absent: instanceCount is reported with no attribution, "
                      "so a species change is invisible to the response"));
        return true;
    }
    TestEqual(TEXT("one row per distinct mesh"), Rows->Num(), 2);

    int32 SummedRows = 0;
    int32 CountForA = -1;
    int32 CountForB = -1;
    for (const TSharedPtr<FJsonValue>& Value : *Rows)
    {
        const TSharedPtr<FJsonObject>* Row = nullptr;
        if (!Value.IsValid() || !Value->TryGetObject(Row) || !Row)
        {
            AddError(TEXT("instancesByMesh carries a row that is not an object"));
            continue;
        }
        FString MeshPath;
        double RowCount = 0.0;
        TestTrue(TEXT("row names the mesh it counted"),
            (*Row)->TryGetStringField(TEXT("mesh"), MeshPath));
        TestTrue(TEXT("row carries its instance count"),
            (*Row)->TryGetNumberField(TEXT("instanceCount"), RowCount));
        SummedRows += static_cast<int32>(RowCount);
        if (MeshPath == PathA) { CountForA = static_cast<int32>(RowCount); }
        else if (MeshPath == PathB) { CountForB = static_cast<int32>(RowCount); }
    }

    // Attribution is per species, matched against each component's own instance count.
    TestEqual(TEXT("the first mesh's row carries that mesh's own instance count"),
        CountForA, IndependentA);
    TestEqual(TEXT("the second mesh's row carries that mesh's own instance count"),
        CountForB, IndependentB);

    // Attribution is COMPLETE: nothing was counted into the total that no row explains.
    TestEqual(TEXT("the per-mesh rows sum to instanceCount"),
        SummedRows, static_cast<int32>(ReportedTotal));

    double DistinctMeshCount = -1.0;
    TestTrue(TEXT("distinctMeshCount is present"),
        Payload->TryGetNumberField(TEXT("distinctMeshCount"), DistinctMeshCount));
    TestEqual(TEXT("distinctMeshCount counts the species, not the components"),
        static_cast<int32>(DistinctMeshCount), 2);

    bool bTruncated = true;
    TestTrue(TEXT("instancesByMeshTruncated is always present"),
        Payload->TryGetBoolField(TEXT("instancesByMeshTruncated"), bTruncated));
    TestFalse(TEXT("two species are well under the row cap, so nothing was elided"), bTruncated);
    return true;
}

// ---------------------------------------------------------------------------
// The two decided policies of the breakdown, driven synthetically because neither is
// reachable from a two-mesh fixture: a managed ISM with NO static mesh gets an explicit
// row rather than being dropped (so the rows still sum to instanceCount and the condition
// is visible), and a graph with more distinct meshes than the row cap keeps the largest
// rows while saying, in both a flag and a warning, that the rest were elided.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPCGGenerateInstancesByMeshPoliciesTest,
    "PinWright.pcg.generate.InstancesByMeshNullKeyAndCap",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPCGGenerateInstancesByMeshPoliciesTest::RunTest(const FString& Parameters)
{
    // ---- A meshless managed component is a finding, not a dropped row. ----
    {
        PinWrightPCG::FGenerationReadback Readback;
        Readback.bResourceCountsAvailable = true;
        Readback.InstanceCount = 9;
        Readback.InstancedComponentCount = 2;
        Readback.InstancesByMesh.Add(TEXT("/Engine/BasicShapes/Cube.Cube"), 6);
        Readback.InstancesByMesh.Add(PinWrightPCG::NoStaticMeshKey, 3);

        TSharedPtr<FJsonObject> Payload = PinWrightPCG::BuildGenerationPayload(
            Readback, TEXT("A"), TEXT("G"), TEXT("/Game/G"), TEXT("generated_delegate"), 0);
        if (!Payload.IsValid())
        {
            AddError(TEXT("BuildGenerationPayload returned no payload"));
            return true;
        }

        const TArray<TSharedPtr<FJsonValue>>* Rows = nullptr;
        if (Payload->TryGetArrayField(TEXT("instancesByMesh"), Rows) && Rows)
        {
            int32 Summed = 0;
            bool bFoundNullKey = false;
            for (const TSharedPtr<FJsonValue>& Value : *Rows)
            {
                const TSharedPtr<FJsonObject>* Row = nullptr;
                if (!Value.IsValid() || !Value->TryGetObject(Row) || !Row) { continue; }
                Summed += static_cast<int32>((*Row)->GetNumberField(TEXT("instanceCount")));
                if ((*Row)->GetStringField(TEXT("mesh")) == PinWrightPCG::NoStaticMeshKey)
                {
                    bFoundNullKey = true;
                }
            }
            TestTrue(TEXT("the meshless component gets an explicit row, not silence"), bFoundNullKey);
            TestEqual(TEXT("rows still sum to instanceCount with a meshless component present"),
                Summed, 9);
        }
        else
        {
            AddError(TEXT("instancesByMesh is absent"));
        }

        const TArray<TSharedPtr<FJsonValue>>* Warnings = nullptr;
        FString Joined;
        if (Payload->TryGetArrayField(TEXT("warnings"), Warnings) && Warnings)
        {
            for (const TSharedPtr<FJsonValue>& Value : *Warnings)
            {
                if (Value.IsValid()) { Joined += Value->AsString(); }
            }
        }
        TestTrue(TEXT("a meshless managed component is called out in warnings[]"),
            Joined.Contains(PinWrightPCG::NoStaticMeshKey));
    }

    // ---- Past the cap: the largest rows survive and the elision is stated. ----
    {
        PinWrightPCG::FGenerationReadback Readback;
        Readback.bResourceCountsAvailable = true;
        const int32 Species = PinWrightPCG::MaxInstancesByMeshRows + 5;
        for (int32 Index = 0; Index < Species; ++Index)
        {
            // Ascending counts, so the LAST meshes added are the ones that must survive.
            Readback.InstancesByMesh.Add(
                FString::Printf(TEXT("/Engine/Synthetic/Mesh_%02d.Mesh_%02d"), Index, Index),
                Index + 1);
            Readback.InstanceCount += Index + 1;
        }
        Readback.InstancedComponentCount = Species;

        TSharedPtr<FJsonObject> Payload = PinWrightPCG::BuildGenerationPayload(
            Readback, TEXT("A"), TEXT("G"), TEXT("/Game/G"), TEXT("generated_delegate"), 0);
        if (!Payload.IsValid())
        {
            AddError(TEXT("BuildGenerationPayload returned no payload"));
            return true;
        }

        const TArray<TSharedPtr<FJsonValue>>* Rows = nullptr;
        TestTrue(TEXT("instancesByMesh is present past the cap"),
            Payload->TryGetArrayField(TEXT("instancesByMesh"), Rows) && Rows != nullptr);
        if (Rows)
        {
            TestEqual(TEXT("the row cap bounds the array"),
                Rows->Num(), PinWrightPCG::MaxInstancesByMeshRows);
            // Sorted instances-descending, so the biggest species is first and is kept.
            const TSharedPtr<FJsonObject>* First = nullptr;
            if (Rows->Num() > 0 && (*Rows)[0].IsValid() && (*Rows)[0]->TryGetObject(First) && First)
            {
                TestEqual(TEXT("the largest species is the row that survives first"),
                    static_cast<int32>((*First)->GetNumberField(TEXT("instanceCount"))), Species);
            }
        }

        double DistinctMeshCount = -1.0;
        TestTrue(TEXT("distinctMeshCount is present"),
            Payload->TryGetNumberField(TEXT("distinctMeshCount"), DistinctMeshCount));
        TestEqual(TEXT("distinctMeshCount carries the FULL species total, not the row count"),
            static_cast<int32>(DistinctMeshCount), Species);

        bool bTruncated = false;
        TestTrue(TEXT("instancesByMeshTruncated is present"),
            Payload->TryGetBoolField(TEXT("instancesByMeshTruncated"), bTruncated));
        TestTrue(TEXT("truncation is declared, so a short list is never read as the whole set"),
            bTruncated);

        const TArray<TSharedPtr<FJsonValue>>* Warnings = nullptr;
        FString Joined;
        if (Payload->TryGetArrayField(TEXT("warnings"), Warnings) && Warnings)
        {
            for (const TSharedPtr<FJsonValue>& Value : *Warnings)
            {
                if (Value.IsValid()) { Joined += Value->AsString(); }
            }
        }
        TestTrue(TEXT("the warning says the rows no longer sum to instanceCount"),
            Joined.Contains(TEXT("do NOT")) && Joined.Contains(TEXT("instanceCount")));
    }
    return true;
}

// ---------------------------------------------------------------------------
// Handler dispatch + validation. Drives the real pcg.generate handler through the
// dispatcher (not just the registration check): a bogus actor identity must be
// rejected synchronously with ACTOR_NOT_FOUND, never faked-success — proving the
// handler actually runs its validation before the async generation path.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPCGGenerateMissingActorTest,
    "PinWright.pcg.generate.MissingActorRejected",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPCGGenerateMissingActorTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("actorName"),
        FString::Printf(TEXT("PWNoSuchActor_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits)));

    FTestResponseCapture Capture;
    TestTrue(TEXT("pcg.generate handler registered"),
        InvokeHandlerWithCapture(TEXT("pcg.generate"), Payload, Capture));
    TestTrue(TEXT("handler responded synchronously"), Capture.bWasCalled);
    TestFalse(TEXT("missing actor rejected, not faked-success"), Capture.bSuccess);
    TestEqual(TEXT("error code is ACTOR_NOT_FOUND"),
        Capture.ErrorCode, FString(TEXT("ACTOR_NOT_FOUND")));
    return true;
}

// ---------------------------------------------------------------------------
// No-schedule completion (anti-hang). When UPCGComponent::GenerateInternal cannot
// schedule a task it returns InvalidPCGTaskId and broadcasts NO completion delegate.
// An implementation that binds ONLY OnPCGGraphGeneratedDelegate and fires the void
// GenerateLocal(bool) never resolves in that case and the client hangs to the
// transport timeout. The handler triggers via GenerateLocalGetTaskId(bForce) and, on
// InvalidPCGTaskId, answers INLINE — no job ticket is allocated for work that never
// started, so a caller is never handed a ticket that can never move. This test forces
// that path with a deactivated component (ShouldGenerate()'s first guard is
// !bActivated) and asserts a synchronous PCG_GENERATION_NOT_SCHEDULED error —
// bWasCalled proves no hang.
// Differential: reverting to GenerateLocal(bool) leaves bWasCalled false.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPCGGenerateNoScheduleResolvesTest,
    "PinWright.pcg.generate.NoScheduleResolvesToken",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPCGGenerateNoScheduleResolvesTest::RunTest(const FString& Parameters)
{
    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("GEditor->GetEditorWorldContext().World() returned null, so the deactivated "
                 "UPCGComponent fixture could not be built and the no-schedule token "
                 "assertions could not run."));
        return true;
    }

    AActor* Actor = World->SpawnActor<AActor>();
    if (!Actor)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("fixture-unavailable"),
            TEXT("World->SpawnActor<AActor>() returned null, so the no-schedule token "
                 "assertions could not run."));
        return true;
    }
    ON_SCOPE_EXIT { if (IsValid(Actor)) { World->DestroyActor(Actor); } };

    const FString Label = FString::Printf(TEXT("MCP_PcgGenNoSchedule_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    Actor->SetActorLabel(Label);

    // A deactivated component makes UPCGComponent::ShouldGenerate() return false
    // deterministically (its first guard is !bActivated), so GenerateLocalGetTaskId
    // returns InvalidPCGTaskId without scheduling — the exact no-broadcast path.
    UPCGComponent* PcgComp = NewObject<UPCGComponent>(Actor, UPCGComponent::StaticClass(),
        NAME_None, RF_Transactional);
    if (!PcgComp)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("fixture-unavailable"),
            TEXT("NewObject<UPCGComponent>() returned null, so the no-schedule token "
                 "assertions could not run."));
        return true;
    }
    PcgComp->bActivated = false;
    Actor->AddInstanceComponent(PcgComp);
    PcgComp->RegisterComponent();

    // Assign a graph so the handler passes its NO_PCG_GRAPH guard and reaches the
    // generation trigger under test.
    if (UPCGGraph* Graph = NewObject<UPCGGraph>(GetTransientPackage()))
    {
        PcgComp->SetGraphLocal(Graph);
    }
    if (!PcgComp->GetGraph())
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("fixture-unavailable"),
            TEXT("UPCGComponent::SetGraphLocal left GetGraph() null in this world, so the "
                 "handler's NO_PCG_GRAPH guard would have answered first and the "
                 "no-schedule token assertions could not run."));
        return true;
    }

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("actorName"), Label);
    Payload->SetBoolField(TEXT("force"), true);

    // Async token needs a shared-owned capture (the token routes through a weak handle).
    TSharedRef<FTestResponseCapture> Capture = MakeShared<FTestResponseCapture>();
    TestTrue(TEXT("pcg.generate handler invoked"),
        InvokeHandlerWithSharedCapture(TEXT("pcg.generate"), Payload, Capture));

    // The no-schedule path resolves synchronously inside the handler call — no pump.
    TestTrue(TEXT("async token resolved (no client hang)"), Capture->bWasCalled);
    TestFalse(TEXT("no-schedule reported as error, not faked success"), Capture->bSuccess);
    TestEqual(TEXT("error code is PCG_GENERATION_NOT_SCHEDULED"),
        Capture->ErrorCode, FString(TEXT("PCG_GENERATION_NOT_SCHEDULED")));
    return true;
}

// ---------------------------------------------------------------------------
// Ticketed kickoff — the B-pcg-generate-deadlocks-game-thread regression.
//
// PCG generation advances only inside UPCGSubsystem::Tick (game thread, time-budgeted,
// over an unbounded number of frames), so the kickoff must NOT wait for it. The
// previous implementation held the request open on a single FAsyncResponseToken until
// OnPCGGraphGeneratedDelegate fired; when the generation outran the transport deadline
// the caller was told the call FAILED while the generation went on to succeed, with no
// ticket to recover the result from.
//
// This test drives the real handler against a live, activated component and asserts on
// the SAME STACK (InvokeHandlerWithSharedCapture does not pump) that a response already
// exists, and that a scheduled generation handed back a registered job ticket. It is
// differential by construction: the token-based implementation leaves bWasCalled false
// for a scheduled generation, and emits no ticket_id at all.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPCGGenerateTicketedKickoffTest,
    "PinWright.pcg.generate.TicketedKickoffDoesNotBlock",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPCGGenerateTicketedKickoffTest::RunTest(const FString& Parameters)
{
    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("GEditor->GetEditorWorldContext().World() returned null, so the activated "
                 "UPCGComponent fixture could not be built and the synchronous-kickoff "
                 "assertions could not run."));
        return true;
    }

    AActor* Actor = World->SpawnActor<AActor>();
    if (!Actor)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("fixture-unavailable"),
            TEXT("World->SpawnActor<AActor>() returned null, so the synchronous-kickoff "
                 "assertions could not run."));
        return true;
    }
    ON_SCOPE_EXIT { if (IsValid(Actor)) { World->DestroyActor(Actor); } };

    const FString Label = FString::Printf(TEXT("MCP_PcgGenTicket_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    Actor->SetActorLabel(Label);

    // Activated (unlike the no-schedule fixture above) so UPCGComponent::ShouldGenerate
    // can pass and the scheduler path is the one under test.
    UPCGComponent* PcgComp = NewObject<UPCGComponent>(Actor, UPCGComponent::StaticClass(),
        NAME_None, RF_Transactional);
    if (!PcgComp)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("fixture-unavailable"),
            TEXT("NewObject<UPCGComponent>() returned null, so the synchronous-kickoff "
                 "assertions could not run."));
        return true;
    }
    Actor->AddInstanceComponent(PcgComp);
    PcgComp->RegisterComponent();
    if (UPCGGraph* Graph = NewObject<UPCGGraph>(GetTransientPackage()))
    {
        PcgComp->SetGraphLocal(Graph);
    }
    if (!PcgComp->GetGraph())
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("fixture-unavailable"),
            TEXT("UPCGComponent::SetGraphLocal left GetGraph() null in this world, so the "
                 "handler's NO_PCG_GRAPH guard would have answered first and the "
                 "synchronous-kickoff assertions could not run."));
        return true;
    }

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("actorName"), Label);
    Payload->SetBoolField(TEXT("force"), true);

    TSharedRef<FTestResponseCapture> Capture = MakeShared<FTestResponseCapture>();
    const double StartSeconds = FPlatformTime::Seconds();
    TestTrue(TEXT("pcg.generate handler invoked"),
        InvokeHandlerWithSharedCapture(TEXT("pcg.generate"), Payload, Capture));
    const double ElapsedSeconds = FPlatformTime::Seconds() - StartSeconds;

    // The load-bearing assertion: answered on this stack, without pumping anything.
    TestTrue(TEXT("kickoff answered synchronously — never waits on the PCG scheduler"),
        Capture->bWasCalled);
    TestTrue(TEXT("kickoff returned promptly (< 5s)"), ElapsedSeconds < 5.0);
    if (!Capture->bWasCalled)
    {
        return true;
    }

    if (!Capture->bSuccess)
    {
        // The ONE inline rejection a bare test world may legitimately produce: the PCG
        // subsystem could not schedule, so PCGGenerateHandler answers inline with
        // PCG_GENERATION_NOT_SCHEDULED (ERR_PCG_GENERATION_NOT_SCHEDULED). That is a
        // visible skip, pinned to that exact code so it cannot look like anything else:
        // the previous `TestFalse(ErrorCode.IsEmpty())` was satisfied by ANY error, so a
        // regression that made the handler reject a perfectly good actor (or that broke
        // the scheduler trigger) passed here silently.
        PinWrightTestSkip::SkipAssertions(*this, TEXT("generation-resolved-inline"),
            FString::Printf(
                TEXT("pcg.generate resolved inline with '%s' in this world, so the ticket "
                     "contract assertions could not run."),
                *Capture->ErrorCode));
        // Two codes -- and only two -- are legitimate here, because both are resolved
        // INLINE by PCGGenerateHandler.cpp BEFORE the Ctx.StartJob at :379, so neither
        // can mask the missing-ticket regression this test guards:
        //   PCG_GENERATION_NOT_SCHEDULED (:342) -- the trigger returned InvalidPCGTaskId,
        //     nothing was scheduled, so no completion delegate will ever fire.
        //   PCG_GENERATION_CANCELLED (:313) -- OnPCGGraphCancelledDelegate fired on this
        //     same stack because the pass aborted immediately, which is what a bare
        //     automation world without a live PCG scheduler actually produces.
        // Any OTHER code still fails: a handler that rejected a good actor, or that lost
        // the scheduler trigger, does not land on either of these.
        const bool bAcceptableInlineRejection =
            Capture->ErrorCode == TEXT("PCG_GENERATION_NOT_SCHEDULED")
            || Capture->ErrorCode == TEXT("PCG_GENERATION_CANCELLED");
        TestTrue(
            *FString::Printf(
                TEXT("inline rejection is an unschedulable-world code, got '%s'"),
                *Capture->ErrorCode),
            bAcceptableInlineRejection);
        return true;
    }

    if (!Capture->Result.IsValid())
    {
        AddError(TEXT("pcg.generate returned success with no result payload"));
        return true;
    }

    FString TicketId;
    if (!Capture->Result->TryGetStringField(TEXT("ticket_id"), TicketId))
    {
        // A success carrying no ticket is legitimate ONLY on the handler's inline
        // readback path (nothing scheduled, prior output existed). That payload comes
        // from PinWrightPCG::BuildGenerationPayload, which always writes
        // `completionSignal` and never writes the ticket response's fields; a ticket
        // response that simply lost its `ticket_id` has no `completionSignal` at all.
        // Distinguishing the two is the whole point — the previous bare AddInfo let a
        // missing ticket pass as if it were the readback skip.
        FString CompletionSignal;
        if (!Capture->Result->TryGetStringField(TEXT("completionSignal"), CompletionSignal))
        {
            AddError(TEXT("pcg.generate reported success with neither a ticket_id nor an inline generation readback (no completionSignal) — the ticketed-kickoff contract is broken."));
            return true;
        }
        PinWrightTestSkip::SkipAssertions(*this, TEXT("inline-readback-not-ticketed"),
            FString::Printf(
                TEXT("pcg.generate answered inline with an existing readback "
                     "(completionSignal='%s'; nothing scheduled), so the ticket contract "
                     "assertions could not run."),
                *CompletionSignal));
        // An inline readback is not a half-built ticket response: it never carries the
        // ticket status field, so a ticket response stripped of only its ticket_id can
        // never be mistaken for this skip.
        TestFalse(TEXT("an inline readback carries no ticket 'status' field"),
            Capture->Result->HasField(TEXT("status")));
        return true;
    }

    FString Status;
    Capture->Result->TryGetStringField(TEXT("status"), Status);
    TestEqual(TEXT("ticketed kickoff reports status=running"), Status, FString(TEXT("running")));

    FJobTicket Ticket;
    TestTrue(TEXT("kickoff ticket is registered with the job registry (pollable via system.job_status)"),
        FPluginState::Get().GetJobRegistry().Get(TicketId, Ticket));
    TestEqual(TEXT("ticket is owned by pcg.generate"), Ticket.Method, FString(TEXT("pcg.generate")));

    // Don't leave a live PCG pass (and its watchdog ticker) behind: cancelling the
    // ticket runs the handler's cancel callback, which cancels the generation.
    FPluginState::Get().GetJobRegistry().Cancel(TicketId);
    return true;
}

// ---------------------------------------------------------------------------
// Doc contract. The method page an agent lands on when it calls pcg.generate must
// state the ticketed-async contract and point at the system.job_status poll, and the
// namespace page must carry the two engine-side traps that cost real tuning cycles
// (Cleanup's bRemoveComponents argument; PointExtents dominating PointsPerSquaredMeter).
// Rendered through the live WikiHandler path, not a copy of the overlay text.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPCGGenerateAsyncDocsTest,
    "PinWright.pcg.generate.AsyncDocs",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPCGGenerateAsyncDocsTest::RunTest(const FString& Parameters)
{
    FString MethodPage;
    if (!WikiDocTestHelpers::RenderOrFail(*this, TEXT("pcg.generate"), MethodPage))
    {
        return false;
    }
    static const TCHAR* const GenerateVerb[] = { TEXT("pcg.generate") };
    WikiDocTestHelpers::AssertAsyncTicketPollContract(*this, TEXT("pcg.generate"), MethodPage, GenerateVerb);

    FString NamespacePage;
    if (!WikiDocTestHelpers::RenderOrFail(*this, TEXT("pcg"), NamespacePage))
    {
        return false;
    }
    TestTrue(TEXT("pcg page warns that Cleanup's bRemoveComponents argument decides whether components survive"),
        NamespacePage.Contains(TEXT("bRemoveComponents")) && NamespacePage.Contains(TEXT("cleanup")));
    TestTrue(TEXT("pcg page explains PointExtents vs PointsPerSquaredMeter spacing"),
        NamespacePage.Contains(TEXT("PointExtents")) && NamespacePage.Contains(TEXT("PointsPerSquaredMeter")));
    return true;
}

// ---------------------------------------------------------------------------
// THE REGRESSION — B-pcg-generate-blind-to-non-point-data.
//
// Every countable field pcg.generate published was structurally zero for a graph whose
// product is neither points nor spawned resources. pointCount counts only what survives
// Cast<UPCGBasePointData>; instanceCount / instancedComponentCount / spawnedActorCount need
// managed resources, which such a graph creates none of; and dataCount is a container count
// that says nothing about content. So a graph that WORKED and a graph that produced nothing
// returned the same confident zeros, and the wiki nominated instanceCount — a field that is
// structurally zero for this whole class of graph — as the one to judge a generation by.
//
// This test drives PinWrightPCG::ReadGeneration against two REAL UPCGComponents in the
// editor world: one holding a graph output of non-point data (attribute sets — the same cast
// failure UE 5.8's UPVData hits, without needing an experimental plugin), and one that
// produced nothing at all. It then asserts the two receipts are DISTINGUISHABLE — the exact
// comparison the ticket says is impossible today.
//
// Differential by construction, three ways: the pre-fix payload always wrote pointCount (as
// a bare 0) whenever the graph output was available, so the omission assertion fails; it had
// no dataTypes field at all, so the "name what was produced" assertion fails; and the test
// names PinWrightPCG::NullDataKey and FGenerationReadback::PointDataCount directly, so
// reverting the readback fails compilation rather than going quietly green.
// ---------------------------------------------------------------------------
namespace PcgGenerateNonPointOutputFixture
{
    // UPCGComponent::GeneratedGraphOutput is a private UPROPERTY with a const getter and no
    // setter — PCG fills it from PostProcessGraph. Reflection is the only way to put the
    // readback in front of a KNOWN collection, and going through the component (rather than
    // calling the summariser on a loose collection) is what keeps ReadGeneration itself under
    // test instead of a test-local re-implementation of it.
    inline bool WriteGeneratedGraphOutput(UPCGComponent* Comp, const FPCGDataCollection& Collection)
    {
        FStructProperty* OutputProperty = FindFProperty<FStructProperty>(
            UPCGComponent::StaticClass(), TEXT("GeneratedGraphOutput"));
        if (!Comp || !OutputProperty || OutputProperty->Struct != FPCGDataCollection::StaticStruct())
        {
            return false;
        }
        *OutputProperty->ContainerPtrToValuePtr<FPCGDataCollection>(Comp) = Collection;
        return true;
    }

    inline FString JoinWarnings(const TSharedPtr<FJsonObject>& Payload)
    {
        FString Joined;
        const TArray<TSharedPtr<FJsonValue>>* Warnings = nullptr;
        if (Payload.IsValid() && Payload->TryGetArrayField(TEXT("warnings"), Warnings) && Warnings)
        {
            for (const TSharedPtr<FJsonValue>& Value : *Warnings)
            {
                if (Value.IsValid()) { Joined += Value->AsString(); }
            }
        }
        return Joined;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPCGGenerateNonPointOutputIsDistinguishableTest,
    "PinWright.pcg.generate.NonPointOutputIsDistinguishableFromNothing",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPCGGenerateNonPointOutputIsDistinguishableTest::RunTest(const FString& Parameters)
{
    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("GEditor->GetEditorWorldContext().World() returned null, so the non-point "
                 "graph-output fixture could not be built and the distinguishability "
                 "assertions could not run."));
        return true;
    }

    // Two actors, one PCG component each: PCG treats a component as its actor's generator,
    // so two on one actor is a shape the engine never produces and not one to test against.
    AActor* WorkedActor = World->SpawnActor<AActor>();
    AActor* NothingActor = World->SpawnActor<AActor>();
    ON_SCOPE_EXIT { if (IsValid(WorkedActor)) { World->DestroyActor(WorkedActor); } };
    ON_SCOPE_EXIT { if (IsValid(NothingActor)) { World->DestroyActor(NothingActor); } };
    if (!WorkedActor || !NothingActor)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("fixture-unavailable"),
            TEXT("World->SpawnActor<AActor>() returned null, so the distinguishability "
                 "assertions could not run."));
        return true;
    }

    auto AddPcgComponent = [](AActor* OwningActor, const TCHAR* LabelPrefix) -> UPCGComponent*
    {
        OwningActor->SetActorLabel(FString::Printf(TEXT("%s_%s"), LabelPrefix,
            *FGuid::NewGuid().ToString(EGuidFormats::Digits)));
        UPCGComponent* Comp = NewObject<UPCGComponent>(OwningActor, UPCGComponent::StaticClass(),
            NAME_None, RF_Transactional);
        if (!Comp)
        {
            return nullptr;
        }
        OwningActor->AddInstanceComponent(Comp);
        Comp->RegisterComponent();
        return Comp;
    };

    UPCGComponent* WorkedComp = AddPcgComponent(WorkedActor, TEXT("MCP_PcgGenNonPoint"));
    UPCGComponent* NothingComp = AddPcgComponent(NothingActor, TEXT("MCP_PcgGenNothing"));
    if (!WorkedComp || !NothingComp)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("fixture-unavailable"),
            TEXT("NewObject<UPCGComponent>() returned null, so the distinguishability "
                 "assertions could not run."));
        return true;
    }

    // Three attribute sets (UPCGParamData, EPCGDataType::Param) plus one empty tagged slot.
    // UPCGParamData stands in for the whole class of output the point counter cannot see:
    // the counting path has exactly one cast, so anything reaching the Output node that is
    // not a UPCGBasePointData behaves identically, whichever plugin defined it. The measured
    // case was ProceduralVegetation's UPVData, which needs an experimental plugin this test
    // must not depend on.
    constexpr int32 AttributeSetCount = 3;
    FPCGDataCollection NonPointOutput;
    bool bAttributeSetsBuilt = true;
    for (int32 Index = 0; Index < AttributeSetCount; ++Index)
    {
        UPCGParamData* AttributeSet = NewObject<UPCGParamData>(GetTransientPackage());
        bAttributeSetsBuilt = bAttributeSetsBuilt && (AttributeSet != nullptr);
        NonPointOutput.TaggedData.Emplace_GetRef().Data = AttributeSet;
    }
    // Deliberate: a tagged entry carrying no data object. It must show up as its own row so
    // the rows keep summing to dataCount, on the same reasoning as the (no static mesh) key.
    NonPointOutput.TaggedData.Emplace_GetRef().Data = nullptr;
    const int32 ExpectedDataCount = AttributeSetCount + 1;

    if (!bAttributeSetsBuilt)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("fixture-unavailable"),
            TEXT("NewObject<UPCGParamData>() returned null, so the non-point graph output "
                 "could not be built and the distinguishability assertions could not run."));
        return true;
    }
    if (!PcgGenerateNonPointOutputFixture::WriteGeneratedGraphOutput(WorkedComp, NonPointOutput))
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("fixture-unavailable"),
            TEXT("UPCGComponent::GeneratedGraphOutput could not be resolved as an "
                 "FPCGDataCollection property on this engine, so a known non-point graph "
                 "output could not be installed and the assertions could not run."));
        return true;
    }

    // Independently obtained from the component itself rather than restated from the
    // fixture constants — if the write did not land, that is a fixture problem and must not
    // be reported as a readback regression.
    const int32 InstalledDataCount = WorkedComp->GetGeneratedGraphOutput().TaggedData.Num();
    if (InstalledDataCount != ExpectedDataCount)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("fixture-did-not-populate"),
            FString::Printf(
                TEXT("The component reports %d of %d installed output data entries, so the "
                     "distinguishability assertions could not run."),
                InstalledDataCount, ExpectedDataCount));
        return true;
    }

    const PinWrightPCG::FGenerationReadback WorkedReadback = PinWrightPCG::ReadGeneration(WorkedComp);
    const PinWrightPCG::FGenerationReadback NothingReadback = PinWrightPCG::ReadGeneration(NothingComp);

    // ---- The state the ticket describes: the graph WORKED, and every counter is 0. ----
    TestTrue(TEXT("worked: the graph output is available"), WorkedReadback.bGraphOutputAvailable);
    TestFalse(TEXT("nothing: the graph output is empty"), NothingReadback.bGraphOutputAvailable);
    TestEqual(TEXT("worked: the point arithmetic really does come out 0"),
        static_cast<int32>(WorkedReadback.PointCount), 0);
    TestEqual(TEXT("worked: nothing in the output was countable, which is what makes that 0 a non-measurement"),
        WorkedReadback.PointDataCount, 0);
    TestEqual(TEXT("worked: instanceCount is 0 too — this shape of graph spawns no managed resources"),
        static_cast<int32>(WorkedReadback.InstanceCount), 0);
    TestEqual(TEXT("nothing: instanceCount is 0 as well, so it cannot separate the two"),
        static_cast<int32>(NothingReadback.InstanceCount), 0);
    TestEqual(TEXT("worked: dataCount counts the containers, including the empty slot"),
        WorkedReadback.DataCount, InstalledDataCount);

    TSharedPtr<FJsonObject> Worked = PinWrightPCG::BuildGenerationPayload(
        WorkedReadback, TEXT("TestActor"), TEXT("TestGraph"), TEXT("/Game/TestGraph"),
        TEXT("generated_delegate"), 1);
    TSharedPtr<FJsonObject> Nothing = PinWrightPCG::BuildGenerationPayload(
        NothingReadback, TEXT("TestActor"), TEXT("TestGraph"), TEXT("/Game/TestGraph"),
        TEXT("generated_delegate"), 1);
    if (!Worked.IsValid() || !Nothing.IsValid())
    {
        AddError(TEXT("BuildGenerationPayload returned no payload"));
        return true;
    }

    // ---- 1. No confident zero. THE load-bearing assertion. ----
    // Pre-fix this field was written unconditionally whenever the graph output existed, so
    // "produced no points" and "produced something that is not points" were the same receipt.
    TestFalse(TEXT("pointCount is OMITTED when nothing in the output is point data — a 0 there "
                   "would claim a measurement that never happened"),
        Worked->HasField(TEXT("pointCount")));
    double ReportedPointDataCount = -1.0;
    TestTrue(TEXT("pointDataCount is always present when the graph output is available, so the omission is self-describing"),
        Worked->TryGetNumberField(TEXT("pointDataCount"), ReportedPointDataCount));
    TestEqual(TEXT("pointDataCount is 0, naming the reason pointCount is absent"),
        static_cast<int32>(ReportedPointDataCount), 0);

    // ---- 2. The receipt names WHAT the generation produced. ----
    const TArray<TSharedPtr<FJsonValue>>* TypeRows = nullptr;
    if (!Worked->TryGetArrayField(TEXT("dataTypes"), TypeRows) || !TypeRows)
    {
        AddError(TEXT("dataTypes is absent: the response carries a container count and a set of "
                      "zeros and names nothing it produced, so a graph that worked and a graph "
                      "that is broken return the same receipt"));
        return true;
    }
    const FString ParamClassPath = UPCGParamData::StaticClass()->GetPathName();
    int32 SummedRows = 0;
    int32 CountForParam = -1;
    int32 CountForNullSlot = -1;
    FString ParamPcgDataType;
    for (const TSharedPtr<FJsonValue>& Value : *TypeRows)
    {
        const TSharedPtr<FJsonObject>* Row = nullptr;
        if (!Value.IsValid() || !Value->TryGetObject(Row) || !Row)
        {
            AddError(TEXT("dataTypes carries a row that is not an object"));
            continue;
        }
        FString ClassPath;
        double RowCount = 0.0;
        TestTrue(TEXT("row names the data class it counted"),
            (*Row)->TryGetStringField(TEXT("class"), ClassPath));
        TestTrue(TEXT("row carries its count"),
            (*Row)->TryGetNumberField(TEXT("count"), RowCount));
        SummedRows += static_cast<int32>(RowCount);
        if (ClassPath == ParamClassPath)
        {
            CountForParam = static_cast<int32>(RowCount);
            (*Row)->TryGetStringField(TEXT("pcgDataType"), ParamPcgDataType);
        }
        else if (ClassPath == PinWrightPCG::NullDataKey)
        {
            CountForNullSlot = static_cast<int32>(RowCount);
        }
    }

    TestEqual(TEXT("the attribute-set row carries every attribute set the graph emitted"),
        CountForParam, AttributeSetCount);
    TestEqual(TEXT("the empty tagged slot gets an explicit row, not silence"), CountForNullSlot, 1);
    TestEqual(TEXT("the rows account for every emitted datum (they sum to dataCount)"),
        SummedRows, InstalledDataCount);
    // PCG's own classification, read off the datum rather than inferred from the class name.
    TestEqual(TEXT("the row carries PCG's own EPCGDataType name for an attribute set"),
        ParamPcgDataType, FString(TEXT("Param")));

    double ReportedDataCount = -1.0;
    TestTrue(TEXT("dataCount is present"),
        Worked->TryGetNumberField(TEXT("dataCount"), ReportedDataCount));
    TestEqual(TEXT("dataCount is the container total"),
        static_cast<int32>(ReportedDataCount), InstalledDataCount);
    double DistinctDataTypeCount = -1.0;
    TestTrue(TEXT("distinctDataTypeCount is present"),
        Worked->TryGetNumberField(TEXT("distinctDataTypeCount"), DistinctDataTypeCount));
    TestEqual(TEXT("distinctDataTypeCount counts the classes, not the data objects"),
        static_cast<int32>(DistinctDataTypeCount), 2);

    // ---- 3. The verb SAYS it could not count, rather than reporting zeros. ----
    const FString WorkedWarnings = PcgGenerateNonPointOutputFixture::JoinWarnings(Worked);
    TestTrue(TEXT("warnings[] states that pointCount was omitted"),
        WorkedWarnings.Contains(TEXT("pointCount")) && WorkedWarnings.Contains(TEXT("OMITTED")));
    TestTrue(TEXT("warnings[] names the class it could not count inside"),
        WorkedWarnings.Contains(ParamClassPath));
    TestTrue(TEXT("warnings[] steers the caller to dataTypes rather than to a structurally-zero counter"),
        WorkedWarnings.Contains(TEXT("dataTypes")));

    // ---- 4. THE COMPARISON THE TICKET SAYS IS IMPOSSIBLE. ----
    // Every number these two receipts share is 0 in both. What separates them is that the
    // working generation's receipt names its output and the other's has nothing to name.
    TestFalse(TEXT("the 'produced nothing' receipt names no output data type"),
        Nothing->HasField(TEXT("dataTypes")));
    TestFalse(TEXT("the 'produced nothing' receipt carries no dataCount"),
        Nothing->HasField(TEXT("dataCount")));
    TestFalse(TEXT("the 'produced nothing' receipt carries no pointDataCount"),
        Nothing->HasField(TEXT("pointDataCount")));
    TestFalse(TEXT("neither receipt fabricates a pointCount"),
        Nothing->HasField(TEXT("pointCount")));
    double WorkedInstances = -1.0;
    double NothingInstances = -2.0;
    TestTrue(TEXT("the working receipt reports instanceCount"),
        Worked->TryGetNumberField(TEXT("instanceCount"), WorkedInstances));
    TestTrue(TEXT("the 'produced nothing' receipt reports instanceCount"),
        Nothing->TryGetNumberField(TEXT("instanceCount"), NothingInstances));
    TestEqual(TEXT("instanceCount is identical across both receipts — it is structurally "
                   "incapable of separating them, which is why the wiki must not nominate it"),
        static_cast<int32>(WorkedInstances), static_cast<int32>(NothingInstances));

    // ---- 5. The doc defect: the wiki must not name a structurally-zero field as THE one. ----
    // Rendering stops at the first `###`, so the "judge a generation by" prose splits: the
    // `## Running a graph` steer lands on the NAMESPACE page and the per-field contract on
    // the `pcg.generate` METHOD page. Both are asserted where they actually render.
    FString NamespacePage;
    if (WikiDocTestHelpers::RenderOrFail(*this, TEXT("pcg"), NamespacePage))
    {
        TestTrue(TEXT("the pcg page routes a non-spawner generation to dataTypes instead of nominating one number"),
            NamespacePage.Contains(TEXT("dataTypes")));
    }

    FString MethodPage;
    if (WikiDocTestHelpers::RenderOrFail(*this, TEXT("pcg.generate"), MethodPage))
    {
        // The sentence that converted a correct 0 into a wrong conclusion. It is false for
        // any graph whose product is neither ISM/HISM instances nor spawned actors.
        TestFalse(TEXT("the pcg.generate page no longer claims instanceCount is non-zero exactly when the graph put geometry in the world"),
            MethodPage.Contains(TEXT("non-zero exactly when the graph put geometry in the world")));
        TestTrue(TEXT("the pcg.generate page documents dataTypes and pointDataCount"),
            MethodPage.Contains(TEXT("dataTypes")) && MethodPage.Contains(TEXT("pointDataCount")));
        TestTrue(TEXT("the pcg.generate page states instanceCount is structurally zero for a non-spawner graph"),
            MethodPage.Contains(TEXT("structurally zero")));
    }
    return true;
}

#endif // __has_include("PCGGraph.h")
