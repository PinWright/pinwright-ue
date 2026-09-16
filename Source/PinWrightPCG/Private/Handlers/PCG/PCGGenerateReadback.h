// Copyright (c) 2026 Alexander Penkin. MIT License.

// Readback helpers for pcg.generate. Declared as named-namespace inline helpers
// (co-located with the handler per agent-conventions.md) so the handler and its
// regression tests call the SAME production symbols — a test that re-implemented the
// counts as local lambdas would stay green after the handler's readback was reverted.
// A named namespace (never anonymous) keeps it ODR-safe under unity.
//
// WHAT THE NUMBERS MEAN — the whole point of this header (B-pcg-generated-graph-output-empty-after-generate).
//
// There are THREE different numbers here and they are not interchangeable:
//
//   * pointCount   — points that reached the GRAPH'S OUTPUT NODE.
//   * instanceCount — ISM/HISM instances PCG actually spawned and still manages.
//   * spawnedActorCount — actors PCG spawned and still manages.
//
// UPCGComponent::GeneratedGraphOutput is populated ONLY from Context->InputData in
// UPCGComponent::PostProcessGraph (UE 5.8 PCGComponent.cpp:637 clears it, :660 iterates
// Context->InputData.TaggedData, :750 adds to it), i.e. exclusively from the data that
// arrived at the graph's Output node. It is NOT cleared after the completion broadcast
// (:750 fill, :803 broadcast, :823 reads it back), so capturing it inside the
// generation-complete delegate gains nothing — the delegate sees what persists.
//
// The consequence: a scatter graph shaped CreatePoints -> StaticMeshSpawner with nothing
// wired into the graph Output node emits ZERO tagged data while spawning thousands of
// instances. That is correct engine behaviour, not a failure. pointCount 0 is therefore
// meaningless as a "did this graph do anything" signal, and must never be reported as if
// it were one.
//
// For a PARTITIONED (HiGen/world-partition) original component the output is empty by
// design: UE 5.8 PCGSubsystem.cpp:748-753 states that local generation tasks are
// deliberately kept out of the original's data dependencies ("those resources should be
// managed locally"), and :844 routes them to ExecutionDependencyTasks instead. Only an
// Unbounded-grid slice (:767, :771-777) ever lands on the original.
//
// The managed-resource counts below are what actually survives a generation, on both
// local and partitioned components, and are the honest answer to "did this do anything".
//
// WHAT instanceCount CANNOT ANSWER — B-pcg-generate-instancecount-blind-to-species.
//
// instanceCount is the SUM over every managed ISM/HISM component, so it is arithmetically
// correct and species-blind at the same time. UPCGMeshSelectorWeighted (the selector
// PCGStaticMeshSpawner ships with) draws exactly one bucket per input point and emits
// exactly one instance for it (UE 5.8 PCGMeshSelectorWeighted.cpp:215 loops to
// InPointData->GetNumPoints(), :243 emplaces one transform), so weights PARTITION a fixed
// point set and cannot resize it: re-meshing an entry, dropping a species from a band, or
// re-weighting the band all leave the sum untouched. Reporting only the sum therefore
// answers "did the graph put geometry in the world" and is structurally incapable of
// answering "did it put the geometry I asked for there".
//
// The identity was already in hand — the loop below holds the UInstancedStaticMeshComponent*
// and GetStaticMesh() is one call away — so the readback now keeps it and the payload emits
// instancesByMesh rows that ATTRIBUTE the total per static mesh. instanceCount still answers
// "whether"; instancesByMesh answers "what".
//
// WHAT pointCount CANNOT ANSWER — B-pcg-generate-blind-to-non-point-data.
//
// The point counter has exactly ONE cast. Anything reaching the graph's Output node that is
// not a UPCGBasePointData contributes nothing, whichever plugin defined it — an attribute set
// (UPCGParamData, EPCGDataType::Param), a spline, a texture, or a spatial type carrying its
// payload in some other container. UE 5.8 ships one such type in the box:
// ProceduralVegetation's UPVData is a UPCGSpatialData whose product lives in an
// FManagedArrayCollection and whose GetDataType() is EPCGDataType::Other, so a graph that
// terminates in an Export node emits real output, spawns no managed resource, and used to be
// reported as pointCount 0 / instanceCount 0 / spawnedActorCount 0 — the SAME receipt a graph
// that did nothing at all produces. There was no field that could contradict the zeros.
//
// The fix is a shape change, not a new counter, and it has two halves:
//
//   * dataTypes[] — one row per DISTINCT output data class ({class, pcgDataType?, count}),
//     built in the same loop that already calls GetClass() to decide whether to count. Rows
//     sum to dataCount (a null TaggedData entry gets an explicit NullDataKey row, on the same
//     reasoning as NoStaticMeshKey), and pcgDataType carries PCG's own EPCGDataType name when
//     the reflected enum resolves the value — omitted, never guessed, for flag combinations
//     it does not name.
//
//   * pointCount is OMITTED when pointDataCount is 0. A graph whose output holds no point
//     data has no point yield to measure, so a 0 there would assert "the graph produced no
//     points" when the truth is "this readback cannot see inside what it produced". Those two
//     must never share a receipt. pointDataCount is written unconditionally next to dataCount
//     so the omission is always self-describing, and a warning names the classes that could
//     not be counted.
//
// The invariant to preserve: every number written here is a measurement of something the
// readback actually read. Availability flags and counts-of-countable-things carry the rest.

#pragma once

#include "CoreMinimal.h"

#if defined(__has_include) && __has_include("PCGData.h")

// Compat header, not the raw engine one: UE 5.3 lacks UE_VERSION_NEWER_THAN_OR_EQUAL.
#include "Compat/EngineVersionCompat.h"
#include "PCGData.h"
#include "PCGComponent.h"
#include "PCGManagedResource.h"

#include "Components/InstancedStaticMeshComponent.h"
// GetStaticMesh() returns UStaticMesh*, and the per-mesh attribution calls GetPathName() on
// it, which needs the complete type rather than the forward declaration.
#include "Engine/StaticMesh.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
// EPCGDataType is a reflected UENUM (PCGCommon.h); StaticEnum<>() + UEnum::GetIndexByValue
// is what turns one datum's GetDataType() into PCG's own type NAME rather than a raw bitmask.
#include "UObject/Class.h"
#include "UObject/ReflectedTypeAccessors.h"

// UPCGSubsystem::ForAllRegisteredLocalComponents is the 5.7+ seam for walking a
// partitioned component's local components; verified absent on 5.3-5.6.
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 7, 0)
#include "Subsystems/PCGSubsystem.h"
#endif

// UPCGBasePointData::GetNumPoints() is the 5.6+ point-count seam; earlier PCG only
// has UPCGPointData with an FPCGPoint array. Include (and count) the one that exists.
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 6, 0)
#include "Data/PCGBasePointData.h"
#else
#include "Data/PCGPointData.h"
#endif

namespace PinWrightPCG
{
    // Key an instancesByMesh row carries when the managed ISM component holds no static
    // mesh. It gets an explicit key rather than being dropped, because "PCG spawned a
    // component with no mesh" is itself a finding, and because dropping it would break the
    // one property that makes the breakdown trustworthy: the rows sum to instanceCount.
    // It cannot collide with a real key — every UObject path name starts with '/'.
    inline constexpr const TCHAR* NoStaticMeshKey = TEXT("(no static mesh)");

    // Row cap for instancesByMesh. Bounded by DISTINCT MESHES, not by instances, so a
    // realistic vegetation graph (a handful of spawners, a species or two each) is never
    // truncated; the cap only exists so a pathological graph cannot produce an unbounded
    // payload. When it bites, the rows are the largest ones, distinctMeshCount carries the
    // full total, and instancesByMeshTruncated says so.
    inline constexpr int32 MaxInstancesByMeshRows = 32;

    // Key a dataTypes row carries for a TaggedData entry whose Data pointer is null. Same
    // reasoning as NoStaticMeshKey: an explicit key rather than a dropped row, so the rows
    // keep summing to dataCount and "PCG emitted an empty slot" stays visible.
    // It cannot collide with a real key — every UObject class path name starts with '/'.
    inline constexpr const TCHAR* NullDataKey = TEXT("(null data)");

    // One dataTypes row's worth of tally, keyed by the output datum's class path name.
    struct FOutputDataTypeTally
    {
        // Output data objects of this class.
        int32 Count = 0;

        // PCG's OWN classification of the type — the EPCGDataType name (Point, Param, which
        // the editor displays as "Attribute Set", Spline, Other, …). Left empty, and then
        // omitted from the row, when the reflected enum does not name the value (it is a
        // bitmask, so flag combinations have no single name) — never guessed.
        FString PcgDataTypeName;
    };

    // EPCGDataType name for one datum, or empty when the reflected enum does not name the
    // exact value. Empty is a real answer here: the caller still has the class path, and a
    // made-up label would be worse than an absent field.
    inline FString ResolvePcgDataTypeName(const UPCGData* Data)
    {
        if (!Data)
        {
            return FString();
        }
        const UEnum* DataTypeEnum = StaticEnum<EPCGDataType>();
        if (!DataTypeEnum)
        {
            return FString();
        }
        const int32 Index = DataTypeEnum->GetIndexByValue(static_cast<int64>(Data->GetDataType()));
        return Index == INDEX_NONE ? FString() : DataTypeEnum->GetNameStringByIndex(Index);
    }

    // One pass over a PCG output collection: how much of it there is, WHAT it is by class,
    // and how much of it this readback could actually count.
    struct FGeneratedDataSummary
    {
        // Every TaggedData entry, whatever its type (null entries included).
        int32 DataCount = 0;

        // Entries the point counter could read. PointCount is a measurement only when this
        // is > 0; at 0 a reported "pointCount: 0" would assert something never measured.
        int32 PointDataCount = 0;
        int64 PointCount = 0;

        // Class path name -> tally. Values sum to DataCount.
        TMap<FString, FOutputDataTypeTally> TypeCounts;
    };

    // Walks the collection ONCE. The class is already in hand at the cast that decides
    // whether a datum is countable, so recording it costs one map insert and is what lets
    // the payload say "the graph produced three of these and this readback cannot count
    // inside them" instead of a confident zero (B-pcg-generate-blind-to-non-point-data).
    inline FGeneratedDataSummary SummarizeGeneratedData(const FPCGDataCollection& Output)
    {
        FGeneratedDataSummary Summary;
        // TaggedData (the public UPROPERTY array) rather than GetAllInputs(): that
        // accessor only exists on 5.6+ and returns this very array.
        Summary.DataCount = Output.TaggedData.Num();
        for (const FPCGTaggedData& Tagged : Output.TaggedData)
        {
            const UPCGData* Data = Tagged.Data.Get();
            if (!Data)
            {
                ++Summary.TypeCounts.FindOrAdd(FString(NullDataKey)).Count;
                continue;
            }

            FOutputDataTypeTally& Tally = Summary.TypeCounts.FindOrAdd(Data->GetClass()->GetPathName());
            ++Tally.Count;
            if (Tally.PcgDataTypeName.IsEmpty())
            {
                Tally.PcgDataTypeName = ResolvePcgDataTypeName(Data);
            }

            // The one cast that decides countability. Everything else — attribute sets,
            // splines, textures, and any spatial type a plugin defines — is real output
            // this readback cannot measure, which is why it is enumerated above rather
            // than silently contributing 0.
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 6, 0)
            if (const UPCGBasePointData* PointData = Cast<UPCGBasePointData>(Data))
            {
                ++Summary.PointDataCount;
                Summary.PointCount += PointData->GetNumPoints();
            }
#else
            if (const UPCGPointData* PointData = Cast<UPCGPointData>(Data))
            {
                ++Summary.PointDataCount;
                Summary.PointCount += PointData->GetPoints().Num();
            }
#endif
        }
        return Summary;
    }

    // Total number of generated points across every point-data entry in a PCG output
    // collection. Non-point data (params, spatial, settings) is skipped and contributes
    // 0, so the result is exactly the point yield that reached the graph's output node.
    // A bare 0 from this is NOT "the graph produced nothing" — read it alongside
    // FGeneratedDataSummary::PointDataCount, which says whether anything was countable.
    inline int64 CountGeneratedPoints(const FPCGDataCollection& Output)
    {
        return SummarizeGeneratedData(Output).PointCount;
    }

    // Number of data items (any type) the graph emitted on its output pins.
    inline int32 CountGeneratedData(const FPCGDataCollection& Output)
    {
        return Output.TaggedData.Num();
    }

    // What a generation left behind, with an explicit availability flag per number so a
    // caller never has to read a 0 as either "none" or "unknown".
    struct FGenerationReadback
    {
        // Graph-output readback (points/data that reached the graph Output node).
        // bGraphOutputAvailable false => PointCount/DataCount are MEANINGLESS, omit them.
        bool bGraphOutputAvailable = false;
        int64 PointCount = 0;
        int32 DataCount = 0;

        // Output data objects the point counter could actually read. PointCount is a
        // measurement ONLY when this is > 0 — at 0 the graph emitted output of shapes this
        // readback cannot count, and reporting "pointCount: 0" would claim a measurement
        // that never happened (B-pcg-generate-blind-to-non-point-data).
        int32 PointDataCount = 0;

        // What the graph output IS, by data class: class path name -> tally. Counts sum to
        // DataCount (a null TaggedData entry lands under NullDataKey), so a caller can tell
        // "produced no points" from "produced something that is not points".
        TMap<FString, FOutputDataTypeTally> DataTypeCounts;

        // Managed-resource readback — what PCG actually spawned and still owns.
        // Available on every supported engine (5.3+).
        bool bResourceCountsAvailable = false;
        int64 InstanceCount = 0;
        int32 InstancedComponentCount = 0;
        int32 SpawnedActorCount = 0;

        // InstanceCount attributed per species: static-mesh object path -> instances.
        // A managed ISM with no static mesh lands under NoStaticMeshKey, and a component
        // carrying zero instances still contributes its key, so the map always covers
        // every counted component and its values always sum to InstanceCount.
        TMap<FString, int64> InstancesByMesh;

        bool bPartitioned = false;
        // Local partition components walked (5.7+ only). On a partitioned component
        // where this is false the resource counts cover the ORIGINAL component only,
        // which for a partitioned component is normally empty.
        bool bLocalComponentsWalked = false;
        int32 LocalComponentCount = 0;
    };

    // Sums the ISM/HISM instances and spawned actors a single component still manages.
    // Mirrors the engine's own idiom (UE 5.8
    // PCG/Private/Tests/Elements/PCGStaticMeshSpawnerTest.cpp:183) — ForEachManagedResource
    // + Cast<UPCGManagedISMComponent> + GetComponent()->GetInstanceCount(). Both of those
    // symbols are present on every supported engine (verified 5.3-5.8), so this route
    // needs no version gate.
    inline void AccumulateManagedResourceCounts(UPCGComponent* Comp, FGenerationReadback& Out)
    {
        if (!Comp)
        {
            return;
        }

#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 6, 0)
        // 5.6+ can say whether the container is safe to walk (it is locked while the
        // generation is mutating it). Skipping the walk is better than a torn count.
        if (!Comp->AreManagedResourcesAccessible())
        {
            return;
        }
#endif

        Comp->ForEachManagedResource([&Out](UPCGManagedResource* Resource)
        {
            if (UPCGManagedISMComponent* ManagedISM = Cast<UPCGManagedISMComponent>(Resource))
            {
                if (UInstancedStaticMeshComponent* ISMC = ManagedISM->GetComponent())
                {
                    // HISM derives from ISM, so this covers both.
                    ++Out.InstancedComponentCount;
                    const int64 Instances = static_cast<int64>(ISMC->GetInstanceCount());
                    Out.InstanceCount += Instances;

                    // The species this component's instances actually are. Discarding it
                    // here is what made four materially different mesh-entry edits report a
                    // byte-identical instanceCount (B-pcg-generate-instancecount-blind-to-species).
                    // Several components can share one mesh, so accumulate rather than assign.
                    const UStaticMesh* Mesh = ISMC->GetStaticMesh();
                    Out.InstancesByMesh.FindOrAdd(Mesh ? Mesh->GetPathName() : FString(NoStaticMeshKey))
                        += Instances;
                }
            }
            else if (UPCGManagedActors* ManagedActors = Cast<UPCGManagedActors>(Resource))
            {
                // The set became an array in 5.6; the old member is deprecated there.
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 6, 0)
                Out.SpawnedActorCount += ManagedActors->GetConstGeneratedActors().Num();
#else
                Out.SpawnedActorCount += ManagedActors->GeneratedActors.Num();
#endif
            }
        });

        Out.bResourceCountsAvailable = true;
    }

    // Full readback for one component, including the local partition components when the
    // engine exposes them. Never returns a number the caller could mistake for a count of
    // something else — every figure carries its own availability flag.
    inline FGenerationReadback ReadGeneration(UPCGComponent* Comp)
    {
        FGenerationReadback Out;
        if (!Comp)
        {
            return Out;
        }

        const FPCGDataCollection& GraphOutput = Comp->GetGeneratedGraphOutput();
        Out.bGraphOutputAvailable = GraphOutput.TaggedData.Num() > 0;
        if (Out.bGraphOutputAvailable)
        {
            // One walk, not three: the class enumeration and the point count come from the
            // same pass, so they can never disagree about what was in the collection.
            const FGeneratedDataSummary Summary = SummarizeGeneratedData(GraphOutput);
            Out.PointCount = Summary.PointCount;
            Out.DataCount = Summary.DataCount;
            Out.PointDataCount = Summary.PointDataCount;
            Out.DataTypeCounts = Summary.TypeCounts;
        }

        Out.bPartitioned = Comp->IsPartitioned();

        AccumulateManagedResourceCounts(Comp, Out);

#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 7, 0)
        // A partitioned component owns nothing itself — its resources live on the local
        // components of the APCGPartitionActors. Walking them is the only way to get a
        // true count for a partitioned generation.
        if (Out.bPartitioned)
        {
            if (UPCGSubsystem* Subsystem = Comp->GetSubsystem())
            {
                Subsystem->ForAllRegisteredLocalComponents(Comp,
                    [&Out](UPCGComponent* LocalComp)
                    {
                        ++Out.LocalComponentCount;
                        AccumulateManagedResourceCounts(LocalComp, Out);
                    });
                Out.bLocalComponentsWalked = true;
            }
        }
#endif

        return Out;
    }

    // "Did this generation leave anything behind?" — the predicate the handler's
    // completion paths gate on. Deliberately NOT `GetGeneratedGraphOutput().TaggedData.Num() > 0`:
    // that reads 0 for every graph that spawns through a StaticMeshSpawner without wiring
    // its Out pin to the graph Output node (see the header comment), which is the common
    // case, so gating on it alone reports "produced nothing" for real work.
    inline bool HasProducedOutput(const FGenerationReadback& Readback)
    {
        return Readback.bGraphOutputAvailable
            || Readback.InstanceCount > 0
            || Readback.SpawnedActorCount > 0;
    }

    // The instancesByMesh rows: instanceCount attributed per species, sorted (instances
    // descending, then mesh path ascending) so the output is deterministic for a given
    // generation and the largest species survive the row cap. Rows rather than a bare
    // map object, matching the census envelope the rest of the surface uses
    // (system.inspect.list_actor_* rows + distinctCount + truncated), so an elided species
    // is detectable instead of silently absent.
    inline TArray<TSharedPtr<FJsonValue>> BuildInstancesByMeshRows(
        const TMap<FString, int64>& InstancesByMesh, int32 MaxRows)
    {
        TArray<TPair<FString, int64>> Sorted;
        Sorted.Reserve(InstancesByMesh.Num());
        for (const TPair<FString, int64>& Pair : InstancesByMesh)
        {
            Sorted.Emplace(Pair.Key, Pair.Value);
        }
        Sorted.Sort([](const TPair<FString, int64>& A, const TPair<FString, int64>& B)
        {
            if (A.Value != B.Value)
            {
                return A.Value > B.Value;
            }
            return A.Key.Compare(B.Key) < 0;
        });

        TArray<TSharedPtr<FJsonValue>> Rows;
        for (const TPair<FString, int64>& Entry : Sorted)
        {
            if (MaxRows > 0 && Rows.Num() >= MaxRows)
            {
                break;
            }
            TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
            Row->SetStringField(TEXT("mesh"), Entry.Key);
            Row->SetNumberField(TEXT("instanceCount"), static_cast<double>(Entry.Value));
            Rows.Add(MakeShared<FJsonValueObject>(Row));
        }
        return Rows;
    }

    // The dataTypes rows: dataCount attributed per output data CLASS, sorted (count
    // descending, then class path ascending) so the output is deterministic for a given
    // generation. No row cap, unlike instancesByMesh: this is bounded by the number of
    // distinct UPCGData subclasses a single collection can hold, not by content, so it
    // cannot grow with the size of the generation.
    inline TArray<TSharedPtr<FJsonValue>> BuildDataTypeRows(
        const TMap<FString, FOutputDataTypeTally>& DataTypeCounts)
    {
        TArray<TPair<FString, FOutputDataTypeTally>> Sorted;
        Sorted.Reserve(DataTypeCounts.Num());
        for (const TPair<FString, FOutputDataTypeTally>& Pair : DataTypeCounts)
        {
            Sorted.Emplace(Pair.Key, Pair.Value);
        }
        Sorted.Sort([](const TPair<FString, FOutputDataTypeTally>& A,
                       const TPair<FString, FOutputDataTypeTally>& B)
        {
            if (A.Value.Count != B.Value.Count)
            {
                return A.Value.Count > B.Value.Count;
            }
            return A.Key.Compare(B.Key) < 0;
        });

        TArray<TSharedPtr<FJsonValue>> Rows;
        Rows.Reserve(Sorted.Num());
        for (const TPair<FString, FOutputDataTypeTally>& Entry : Sorted)
        {
            TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
            Row->SetStringField(TEXT("class"), Entry.Key);
            Row->SetNumberField(TEXT("count"), Entry.Value.Count);
            // Omitted rather than emitted empty when the reflected enum does not name the
            // value: an absent field says "PCG's own type name did not resolve", where an
            // empty string would look like a type called nothing.
            if (!Entry.Value.PcgDataTypeName.IsEmpty())
            {
                Row->SetStringField(TEXT("pcgDataType"), Entry.Value.PcgDataTypeName);
            }
            Rows.Add(MakeShared<FJsonValueObject>(Row));
        }
        return Rows;
    }

    // Assembles the pcg.generate result payload from a readback. Lives here rather than
    // inside the handler so the regression test asserts the SHIPPING payload shape — a
    // test that rebuilt the JSON itself would keep passing after the handler regressed.
    //
    // The load-bearing rule: a number is written ONLY when its availability flag is true,
    // and every flag is written unconditionally. A caller therefore never sees a 0 that
    // could mean either "none" or "could not tell".
    inline TSharedPtr<FJsonObject> BuildGenerationPayload(
        const FGenerationReadback& Readback,
        const FString& ActorLabel,
        const FString& GraphName,
        const FString& GraphAssetPath,
        const TCHAR* CompletionSignal,
        int32 ElapsedSeconds)
    {
        TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
        Result->SetStringField(TEXT("actor"), ActorLabel);
        Result->SetStringField(TEXT("graph"), GraphName);
        Result->SetStringField(TEXT("graphPath"), GraphAssetPath);

        TArray<TSharedPtr<FJsonValue>> Warnings;
        auto NoteWarning = [&Warnings](const FString& Text)
        {
            Warnings.Add(MakeShared<FJsonValueString>(Text));
        };

        // Always present, so an absent pointCount is unambiguous.
        Result->SetBoolField(TEXT("graphOutputAvailable"), Readback.bGraphOutputAvailable);
        if (Readback.bGraphOutputAvailable)
        {
            Result->SetNumberField(TEXT("dataCount"), static_cast<double>(Readback.DataCount));

            // WHAT the graph emitted, by data class. dataCount alone is a container count:
            // "dataCount: 3" cannot be told apart from three point datas, three attribute
            // sets, or three of some plugin's own spatial type carrying a finished result.
            const TArray<TSharedPtr<FJsonValue>> TypeRows = BuildDataTypeRows(Readback.DataTypeCounts);
            Result->SetArrayField(TEXT("dataTypes"), TypeRows);
            Result->SetNumberField(TEXT("distinctDataTypeCount"), Readback.DataTypeCounts.Num());

            // Written unconditionally, so an absent pointCount is unambiguous even when the
            // graph output IS available: 0 here means nothing in the output was point data.
            Result->SetNumberField(TEXT("pointDataCount"), Readback.PointDataCount);
            if (Readback.PointDataCount > 0)
            {
                Result->SetNumberField(TEXT("pointCount"), static_cast<double>(Readback.PointCount));
            }
            else
            {
                // The B-pcg-generate-blind-to-non-point-data case: real output, none of it
                // countable. A 0 here would be a claim about the graph; the truth is a
                // statement about this readback, so it goes in warnings[] and pointCount
                // is omitted rather than fabricated.
                FString ClassList;
                for (const TSharedPtr<FJsonValue>& RowValue : TypeRows)
                {
                    const TSharedPtr<FJsonObject>* Row = nullptr;
                    if (!RowValue.IsValid() || !RowValue->TryGetObject(Row) || !Row)
                    {
                        continue;
                    }
                    if (!ClassList.IsEmpty())
                    {
                        ClassList += TEXT(", ");
                    }
                    ClassList += (*Row)->GetStringField(TEXT("class"));
                }
                NoteWarning(FString::Printf(
                    TEXT("pointCount is OMITTED although the graph output is available: none of the %d data ")
                    TEXT("object(s) the graph emitted is point data (UPCGBasePointData), the only shape this ")
                    TEXT("readback can count. Reporting 0 would have said 'the graph produced no points' when ")
                    TEXT("what is true is 'this readback cannot see inside what it produced' — the generation ")
                    TEXT("DID emit output. dataTypes[] names it: %s. Judge this generation by dataTypes / ")
                    TEXT("dataCount, not by pointCount or instanceCount; convert to point data before the ")
                    TEXT("graph's Output node if you need a point yield."),
                    Readback.DataCount, *ClassList));
            }
        }
        else
        {
            NoteWarning(TEXT("pointCount/dataCount are omitted: this component's generated graph output is empty. ")
                TEXT("UPCGComponent::GetGeneratedGraphOutput() only ever holds what reached the GRAPH'S OUTPUT NODE, ")
                TEXT("so a graph that ends in a spawner (Static Mesh Spawner, Spawn Actor) without connecting its Out pin ")
                TEXT("to the graph Output node produces nothing there even when it spawned a great deal. ")
                TEXT("Use instanceCount / spawnedActorCount to judge whether the generation did work; ")
                TEXT("connect the graph's Output node if you specifically need a point count."));
        }

        // Always present, so an absent instanceCount is unambiguous.
        Result->SetBoolField(TEXT("resourceCountsAvailable"), Readback.bResourceCountsAvailable);
        if (Readback.bResourceCountsAvailable)
        {
            Result->SetNumberField(TEXT("instanceCount"), static_cast<double>(Readback.InstanceCount));
            Result->SetNumberField(TEXT("instancedComponentCount"), Readback.InstancedComponentCount);
            Result->SetNumberField(TEXT("spawnedActorCount"), Readback.SpawnedActorCount);

            // instanceCount is a sum across every species the graph spawned, so on its own
            // it cannot tell a re-meshed, re-speciated or re-weighted graph from the one
            // before it. These rows attribute it, and are always emitted alongside the
            // total (an empty array when nothing spawned) so a caller never has to guess
            // whether an absent breakdown means "one species" or "not measured".
            const TArray<TSharedPtr<FJsonValue>> MeshRows =
                BuildInstancesByMeshRows(Readback.InstancesByMesh, MaxInstancesByMeshRows);
            Result->SetArrayField(TEXT("instancesByMesh"), MeshRows);
            Result->SetNumberField(TEXT("distinctMeshCount"), Readback.InstancesByMesh.Num());
            const bool bMeshRowsTruncated = MeshRows.Num() < Readback.InstancesByMesh.Num();
            Result->SetBoolField(TEXT("instancesByMeshTruncated"), bMeshRowsTruncated);
            if (bMeshRowsTruncated)
            {
                NoteWarning(FString::Printf(
                    TEXT("instancesByMesh lists only the %d largest of %d distinct meshes, so its rows do NOT ")
                    TEXT("sum to instanceCount; the difference is what the elided meshes hold. ")
                    TEXT("distinctMeshCount carries the full total."),
                    MeshRows.Num(), Readback.InstancesByMesh.Num()));
            }
            if (Readback.InstancesByMesh.Contains(FString(NoStaticMeshKey)))
            {
                NoteWarning(FString::Printf(
                    TEXT("An instancesByMesh row is keyed '%s': PCG is managing an instanced component that ")
                    TEXT("carries no static mesh, so its instances render nothing. That is normally a mesh ")
                    TEXT("entry whose Descriptor has an unset or unresolvable StaticMesh in the spawner node."),
                    NoStaticMeshKey));
            }
        }
        else
        {
            NoteWarning(TEXT("instanceCount/spawnedActorCount/instancesByMesh are omitted: the PCG component's managed-resource list ")
                TEXT("was not readable at completion time (it is locked while a generation mutates it). ")
                TEXT("Re-read with another pcg.generate using force=false."));
        }

        Result->SetBoolField(TEXT("partitioned"), Readback.bPartitioned);
        if (Readback.bPartitioned)
        {
            Result->SetBoolField(TEXT("localComponentsWalked"), Readback.bLocalComponentsWalked);
            if (Readback.bLocalComponentsWalked)
            {
                Result->SetNumberField(TEXT("localComponentCount"), Readback.LocalComponentCount);
            }
            else
            {
                NoteWarning(TEXT("This is a partitioned (HiGen/world-partition) component, and this engine version ")
                    TEXT("does not expose UPCGSubsystem::ForAllRegisteredLocalComponents (5.7+), so the counts above ")
                    TEXT("cover only the ORIGINAL component. A partitioned original owns no resources itself — its ")
                    TEXT("output and its spawned instances live on the local components of the APCGPartitionActors — ")
                    TEXT("so treat these counts as a lower bound, not as the generation's true yield."));
            }
        }

        Result->SetStringField(TEXT("completionSignal"), CompletionSignal);
        Result->SetNumberField(TEXT("elapsedSeconds"), ElapsedSeconds);
        if (Warnings.Num() > 0)
        {
            Result->SetArrayField(TEXT("warnings"), Warnings);
        }
        return Result;
    }
}

#endif // __has_include("PCGData.h")
