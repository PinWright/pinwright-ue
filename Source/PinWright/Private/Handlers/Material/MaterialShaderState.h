// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Compat/EngineVersionCompat.h"
#include "Components/MeshComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/SkeletalMesh.h"
// FSkeletalMeshLODInfo and FSkeletalMaterial live here on every supported engine; through
// UE 5.4 Engine/SkeletalMesh.h does not pull it in, so the dereferences below need it named.
#include "Engine/SkinnedAssetCommon.h"
#include "Engine/StaticMesh.h"
#include "Handlers/ParamSpec.h"
#include "Handlers/Material/MaterialCompileErrorCollector.h"
#include "Handlers/Material/MaterialUsageFlags.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "MaterialShared.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstance.h"
#include "Materials/MaterialInterface.h"
#include "Rendering/SkeletalMeshRenderData.h"
#include "StaticMeshResources.h"
#include "Utils/AssetUtils.h"
#include "Utils/JsonUtils.h"

// ONE shared verdict on "does this material's shader actually compile", published by every
// material verb under one field name.
//
// THE DEFECT THIS CLOSES (board E-material-verbs-have-no-shader-compile-signal). Every material
// write verb returned a payload describing the GRAPH write - blocksCompiled, expressionsCreated,
// nodeId, "Nodes connected." - and nothing about the SHADER. A material with a malformed Custom
// HLSL node or a sampler-type mismatch writes a valid .uasset, passes asset.save, reads back with
// the right domain and wired mainInputs, and renders the engine Default Material. Exactly one verb
// (material.authoring.compile_material) asked the engine the question, and nothing routed an author
// to it, so two UI materials shipped through a full authoring / saving / disk-verification / PIE
// capture cycle before it was tried; it found both failures in seconds.
//
// TWO COSTS, TWO ENTRY POINTS, ONE FIELD.
//
//   Probe()        - NON-BLOCKING. Reads the verdict already sitting on the material's
//                    FMaterialResource: its collected compile errors, whether a compile is still in
//                    flight, whether a complete game-thread shader map is installed. Costs a
//                    pointer walk and no compile, which is why every write verb can afford to
//                    publish it unconditionally rather than leaving it to a caller's discipline
//                    (docs/rpc-design.md: structural guarantees over discipline).
//
//   ProbeAndWait() - BLOCKING, opt-in. Forces every permutation through the platform shader
//                    compiler and drains the result, via MaterialCompileErrorCollector. This is the
//                    only way to get a FINAL answer, and the reason is not performance: in a
//                    headless editor the permutation jobs are deferred until the material is first
//                    DRAWN, so a material that can never compile reports `notCompiled` from a probe
//                    forever. See the header comment on MaterialCompileErrorCollector.
//
// WHY A PROBE'S `notCompiled` IS NOT A PASS. It means "the engine has not been asked yet". A caller
// that needs "will this render" must either pass the verb's waitForShaderCompile flag or call
// material.authoring.compile_material. The `hint` field says so in every response that is not
// `completed`, because the recurring failure was an author reading a success-shaped payload and
// concluding the material was fine.
//
// CONSUMED BY THE CAPTURE SIDE TOO. B-capture-verbs-silent-default-material-fallback is the same
// fact one layer down: a thumbnail or asset preview of a material whose shader map is incomplete
// renders the Default Material with success:true and healthy-looking imageStats.
// RendersDefaultMaterial() and the exact status here are the probe those verbs need, callable
// on a bare UMaterialInterface with no handler context so a capture path can ask without a wire
// parameter.
//
// ONE MEASUREMENT IS NOT ONE VERDICT. The shader map is a single axis, and it was published under
// a name - `rendersDefaultMaterial` - that reads as a claim about the asset the caller named and
// about every mesh it is on. It is neither:
//
//   * SUBJECT. A material instance owns a shader only while bHasStaticPermutationResource is set.
//     Without it GetMaterialResource forwards to the parent, so the whole block describes the
//     PARENT. EMeasuredSubject / `measuredSubject` says which of the three was read.
//   * USAGE. A vertex factory refuses to compile its permutations for a usage the material does
//     not declare, so the map is COMPLETE without them: a material missing bUsedWithSkeletalMesh
//     reports `rendersDefaultMaterial:false` while the skinned mesh it is assigned to draws the
//     engine Default Material. DeclaredUsages / RendersDefaultMaterialForUsage close that half; the
//     full flag set with its property names is on material.authoring.get_material_info.
namespace PinWright::MaterialShaderState
{
    // The five distinguishable states, spelled EXACTLY as material.authoring.compile_material's
    // long-standing `compileStatus` field spells them, so one vocabulary spans the namespace
    // instead of two that nearly agree. NotMeasured is the sixth and is not a compile state: it
    // means nothing was looked at (no material, or an asset that has no shader of its own, such as
    // a UMaterialFunction or a parameter collection).
    enum class EStatus : uint8
    {
        NotMeasured,
        NotCompiled,
        Outstanding,
        TimedOut,
        Failed,
        Completed
    };

    // Wire spelling. `outstanding` covers the async-compiling case: a compile is in flight and the
    // verdict is not knowable yet.
    inline const TCHAR* ToWire(EStatus Status)
    {
        switch (Status)
        {
        case EStatus::NotCompiled:  return TEXT("notCompiled");
        case EStatus::Outstanding:  return TEXT("outstanding");
        case EStatus::TimedOut:     return TEXT("timedOut");
        case EStatus::Failed:       return TEXT("failed");
        case EStatus::Completed:    return TEXT("completed");
        default:                    return TEXT("notMeasured");
        }
    }

    // WHICH RESOURCE the status above was read off. Without it `rendersDefaultMaterial` reads as a
    // claim about the asset the caller named, and it is not one: a material instance with no static
    // permutation has no shader of its own, so every field in the block describes its PARENT.
    enum class EMeasuredSubject : uint8
    {
        None,
        BaseMaterial,
        InstanceStaticPermutation,
        ParentInherited
    };

    inline const TCHAR* ToWire(EMeasuredSubject Subject)
    {
        switch (Subject)
        {
        case EMeasuredSubject::BaseMaterial:              return TEXT("baseMaterial");
        case EMeasuredSubject::InstanceStaticPermutation: return TEXT("instanceStaticPermutation");
        case EMeasuredSubject::ParentInherited:           return TEXT("parentInherited");
        default:                                          return TEXT("none");
        }
    }

    // Ordering used to fold several materials into one verdict: the WORST wins. Completed is the
    // best and therefore the lowest, so an aggregate reports `completed` only when every material
    // in it did. A failure is worse than an expired wait, which is worse than work still in flight,
    // which is worse than a compile that never ran, which is worse than not having looked.
    inline int32 Severity(EStatus Status)
    {
        switch (Status)
        {
        case EStatus::Completed:    return 0;
        case EStatus::NotMeasured:  return 1;
        case EStatus::NotCompiled:  return 2;
        case EStatus::Outstanding:  return 3;
        case EStatus::TimedOut:     return 4;
        case EStatus::Failed:       return 5;
        default:                    return 1;
        }
    }

    // What was OBSERVED about a material's shader. Default-constructed it reports NotMeasured with
    // no errors - the failure direction (rpc-design.md §2), so a path that forgets to measure
    // cannot report a pass.
    struct FState
    {
        EStatus Status = EStatus::NotMeasured;

        // Failed-permutation HLSL errors, verbatim from FMaterialResource::GetCompileErrors().
        TArray<FString> Errors;

        // True when the drain loop actually blocked, i.e. ProbeAndWait was used AND there was
        // in-flight work. Separates "already finished when we looked" from "we waited for it".
        bool bWaited = false;

        // Wall clock spent inside the drain loop.
        double WaitedSeconds = 0.0;

        // The renderer will draw the engine Default Material for this material interface: either
        // the parent chain resolves to the default material, or the actual interface resource has
        // no complete shader map. This is the fact a capture verb needs to stop reporting a
        // plausible grey frame as evidence.
        bool bRendersDefaultMaterial = false;

        // Which resource every field above was read off, and the asset that owns it. Emitted
        // beside rendersDefaultMaterial so the boolean cannot be read as a verdict on an asset
        // whose shaders live somewhere else.
        EMeasuredSubject Subject = EMeasuredSubject::None;
        FString MeasuredMaterialPath;

        // The EMaterialUsage set the measured interface declares, in engine spelling. The shader
        // map is COMPLETE without the permutations of an undeclared usage - the vertex factory
        // refuses to compile them - so this list is the other half of "will it render", and the
        // half `rendersDefaultMaterial` structurally cannot see.
        TArray<FString> DeclaredUsages;

        // Per-material breakdown, populated only by Accumulate (a verb that finalises SEVERAL
        // materials in one call, e.g. material.compile_mgir over a multi-entry document). One entry
        // per folded material; empty for the single-material case.
        //
        // Each row carries its OWN subject and usage set. The aggregate cannot: folding two
        // materials leaves no honest single value for either, and dropping them there is what would
        // ship the bare `rendersDefaultMaterial` this whole block exists to qualify.
        struct FPerMaterialEntry
        {
            FString AssetPath;
            EStatus Status = EStatus::NotMeasured;
            EMeasuredSubject Subject = EMeasuredSubject::None;
            TArray<FString> DeclaredUsages;
        };
        TArray<FPerMaterialEntry> PerMaterial;

        bool Succeeded() const { return Status == EStatus::Completed; }

        // A permanent failure, as opposed to a transient "not yet". This is the bit that separates
        // the retry-and-it-goes-away case from the one where retrying produces the identical broken
        // frame forever.
        bool Failed() const { return Status == EStatus::Failed; }

        // Fold one material's verdict into a running total. Worst status wins, errors union,
        // wait time sums, and the fallback flag ORs - one broken material in a batch means the
        // batch has a broken material.
        void Accumulate(const FString& AssetPath, const FState& Other)
        {
            if (Severity(Other.Status) > Severity(Status))
            {
                Status = Other.Status;
            }
            for (const FString& Error : Other.Errors)
            {
                Errors.AddUnique(Error);
            }
            bWaited |= Other.bWaited;
            WaitedSeconds += Other.WaitedSeconds;
            bRendersDefaultMaterial |= Other.bRendersDefaultMaterial;
            // Subject, path and usage set are per-material facts. They survive a single-material
            // fold and are dropped the moment the aggregate covers more than one, because no
            // honest single value exists then. They are not LOST: every row below keeps its own,
            // and AddReport still emits a subject-free rendersDefaultMaterialScope, so the
            // aggregate never ships the unqualified boolean.
            if (PerMaterial.Num() == 0)
            {
                Subject = Other.Subject;
                MeasuredMaterialPath = Other.MeasuredMaterialPath;
                DeclaredUsages = Other.DeclaredUsages;
            }
            else
            {
                Subject = EMeasuredSubject::None;
                MeasuredMaterialPath.Reset();
                DeclaredUsages.Reset();
            }

            FPerMaterialEntry& Entry = PerMaterial.AddDefaulted_GetRef();
            Entry.AssetPath = AssetPath;
            Entry.Status = Other.Status;
            Entry.Subject = Other.Subject;
            Entry.DeclaredUsages = Other.DeclaredUsages;
        }
    };

    // The base material remains useful for recognizing an intentional engine Default Material,
    // but it is not necessarily the resource the renderer uses: a material instance can own a
    // static-permutation resource with its own compile verdict.
    inline const UMaterial* ResolveBaseMaterial(const UMaterialInterface* MaterialInterface)
    {
        return MaterialInterface ? MaterialInterface->GetMaterial_Concurrent() : nullptr;
    }

    inline const FMaterialResource* ResolveMaterialResource(
        const UMaterialInterface* MaterialInterface)
    {
        if (!MaterialInterface)
        {
            return nullptr;
        }
#if UE_VERSION_OLDER_THAN(5, 7, 0)
        return MaterialInterface->GetMaterialResource(GMaxRHIFeatureLevel);
#else
        return MaterialInterface->GetMaterialResource(GMaxRHIShaderPlatform);
#endif
    }

    // Which of the three resources ResolveMaterialResource just handed back. An instance owns one
    // only while bHasStaticPermutationResource is set; without it UMaterialInstance::GetMaterialResource
    // forwards to the parent, so every measurement below describes the parent and saying so is the
    // difference between a verdict and a misattribution.
    inline EMeasuredSubject ResolveMeasuredSubject(const UMaterialInterface* MaterialInterface)
    {
        if (!MaterialInterface)
        {
            return EMeasuredSubject::None;
        }
        if (const UMaterialInstance* Instance = Cast<UMaterialInstance>(MaterialInterface))
        {
            return Instance->bHasStaticPermutationResource
                ? EMeasuredSubject::InstanceStaticPermutation
                : EMeasuredSubject::ParentInherited;
        }
        return EMeasuredSubject::BaseMaterial;
    }

    // The asset that owns the measured resource, which is the parent for a ParentInherited instance.
    inline FString ResolveMeasuredMaterialPath(const UMaterialInterface* MaterialInterface)
    {
        if (!MaterialInterface)
        {
            return FString();
        }
        if (ResolveMeasuredSubject(MaterialInterface) == EMeasuredSubject::ParentInherited)
        {
            // The resource came from Parent->GetMaterialResource, so its owner is the base of the
            // parent chain. A parentless instance has no resource and no owner to name; naming the
            // engine Default Material that GetMaterial() falls back to would be a fabrication.
            const UMaterialInstance* Instance = Cast<UMaterialInstance>(MaterialInterface);
            const UMaterial* Base = (Instance && Instance->Parent)
                ? ResolveBaseMaterial(MaterialInterface)
                : nullptr;
            return Base ? Base->GetPathName() : FString();
        }
        return MaterialInterface->GetPathName();
    }

    // True when the renderer substitutes the engine Default Material for this material interface.
    // FMaterialRenderProxy::GetMaterialWithFallback does so whenever the resource is absent or its
    // render-thread shader map is incomplete, not only after a compiler error. The game-thread
    // completeness flag is the safe mirror available to handlers.
    inline bool RendersDefaultMaterial(const UMaterialInterface* MaterialInterface)
    {
        const UMaterial* Base = ResolveBaseMaterial(MaterialInterface);
        if (!Base)
        {
            return false;
        }
        if (Base->IsDefaultMaterial())
        {
            return true;
        }
        const FMaterialResource* Resource = ResolveMaterialResource(MaterialInterface);
        return Resource == nullptr || !Resource->IsGameThreadShaderMapComplete();
    }

    // The same question asked about ONE consumer class, which is the question every caller of
    // RendersDefaultMaterial actually had. A complete shader map is only half the answer: the
    // permutations of a usage the material does not declare are never compiled into it (the vertex
    // factory's ShouldCompilePermutation refuses them), so a material can hold a complete map,
    // report rendersDefaultMaterial:false, and still be substituted on the exact mesh it is
    // assigned to. That is the false green this predicate exists to prevent.
    inline bool RendersDefaultMaterialForUsage(const UMaterialInterface* MaterialInterface,
        EMaterialUsage Usage)
    {
        if (!MaterialInterface)
        {
            return false;
        }
        return RendersDefaultMaterial(MaterialInterface) ||
            !PinWright::MaterialUsage::DeclaresUsage(MaterialInterface, Usage);
    }

    // Stamp what was measured onto a state the caller just filled in. One place, so a probe, a
    // wait outcome and a post-capture re-probe cannot describe their subject differently.
    inline void AnnotateSubject(FState& State, const UMaterialInterface* MaterialInterface)
    {
        State.Subject = ResolveMeasuredSubject(MaterialInterface);
        State.MeasuredMaterialPath = ResolveMeasuredMaterialPath(MaterialInterface);
        State.DeclaredUsages = PinWright::MaterialUsage::DeclaredUsageNames(MaterialInterface);
    }

    // NON-BLOCKING read of the verdict already on the material. Never submits a compile, so it is
    // safe to call from every verb on every response. Returns NotMeasured for a null interface or
    // one with no base material.
    inline FState Probe(const UMaterialInterface* MaterialInterface)
    {
        FState State;
        const UMaterial* Base = ResolveBaseMaterial(MaterialInterface);
        if (!Base)
        {
            return State;
        }
        AnnotateSubject(State, MaterialInterface);

        const FMaterialResource* Resource = ResolveMaterialResource(MaterialInterface);
        if (!Resource)
        {
            // No resource for the running platform. Nothing was compiled and nothing can be read;
            // that is a distinct answer from "compiled clean".
            State.Status = EStatus::NotCompiled;
            State.bRendersDefaultMaterial = true;
            return State;
        }

        for (const FString& Error : Resource->GetCompileErrors())
        {
            State.Errors.AddUnique(Error);
        }

        if (State.Errors.Num() > 0)
        {
            State.Status = EStatus::Failed;
        }
        else if (!Resource->IsCompilationFinished())
        {
            State.Status = EStatus::Outstanding;
        }
        else if (Resource->IsGameThreadShaderMapComplete())
        {
            State.Status = EStatus::Completed;
        }
        else
        {
            // A shader map object exists but is not complete. FMaterial::BeginCompileShaderMap
            // installs a zero-shader map when compilation is skipped, so this is "no compile ran",
            // not "compiled and produced nothing".
            State.Status = EStatus::NotCompiled;
        }

        State.bRendersDefaultMaterial = Base->IsDefaultMaterial() ||
            !Resource->IsGameThreadShaderMapComplete();
        return State;
    }

    // Translate a completed MaterialCompileErrorCollector wait into the shared verdict. Shared with
    // material.authoring.compile_material so its long-standing `compileStatus` field and the
    // `shaderCompile` block in the same response cannot disagree - including on the timedOut edge,
    // where a re-probe would read `outstanding` and lose the fact that a wait expired.
    inline FState FromWaitOutcome(const MaterialCompileErrorCollector::FCompileWaitOutcome& Outcome,
        const TArray<FString>& Errors, const UMaterialInterface* MaterialInterface)
    {
        FState State;
        State.Errors = Errors;
        State.bWaited = Outcome.bWaited;
        State.WaitedSeconds = Outcome.WaitedSeconds;

        if (Outcome.bTimedOut)
        {
            State.Status = EStatus::TimedOut;
        }
        else if (Outcome.bOutstanding)
        {
            State.Status = EStatus::Outstanding;
        }
        else if (State.Errors.Num() > 0)
        {
            State.Status = EStatus::Failed;
        }
        else
        {
            State.Status = Outcome.bShaderMapComplete ? EStatus::Completed : EStatus::NotCompiled;
        }

        const UMaterial* Base = ResolveBaseMaterial(MaterialInterface);
        State.bRendersDefaultMaterial = (Base != nullptr && Base->IsDefaultMaterial()) ||
            State.Status != EStatus::Completed;
        AnnotateSubject(State, MaterialInterface);
        return State;
    }

    // Capture already asked the render thread for this material. Give any lazily submitted jobs
    // one bounded, game-thread-pumped chance to land before classifying the captured frame. Unlike
    // ProbeAndWait this never starts a new permutation compile, so it cannot change what the frame
    // asked the renderer to draw.
    inline FState ProbeAfterCapture(UMaterialInterface* MaterialInterface, double DeadlineSeconds)
    {
        const double StartSeconds = FPlatformTime::Seconds();
        bool bWaited = false;
        bool bTimedOut = false;
        const FMaterialResource* Resource = ResolveMaterialResource(MaterialInterface);
        while (Resource && !Resource->IsCompilationFinished())
        {
            bWaited = true;
            if (FPlatformTime::Seconds() >= DeadlineSeconds)
            {
                bTimedOut = true;
                break;
            }
            PinWright::AssetCompile::AdvanceOnGameThread();
            FPlatformProcess::Sleep(MaterialCompileErrorCollector::CompilePollIntervalSeconds);
            Resource = ResolveMaterialResource(MaterialInterface);
        }

        FState State = Probe(MaterialInterface);
        State.bWaited = bWaited;
        State.WaitedSeconds = FPlatformTime::Seconds() - StartSeconds;
        if (bTimedOut && State.Status == EStatus::Outstanding)
        {
            State.Status = EStatus::TimedOut;
        }
        return State;
    }

    // BLOCKING. Forces every permutation through the platform shader compiler and drains the
    // result, so the returned status is a measurement rather than a snapshot. This is the only
    // entry point that can turn a headless editor's permanent `notCompiled` into `completed` or
    // `failed`; see the file header for why.
    inline FState ProbeAndWait(UMaterialInterface* MaterialInterface,
        double TimeoutSeconds = MaterialCompileErrorCollector::CompileWaitTimeoutSeconds)
    {
        UMaterial* Base = MaterialInterface ? MaterialInterface->GetMaterial() : nullptr;
        if (!Base)
        {
            return FState();
        }

        // Measured on the interface the caller named, not on its base material. Passing the base
        // here compiled and reported the MASTER's permutation for an instance that owns its own
        // static permutation - the one case where the two verdicts genuinely differ, and the case
        // a static-switch override creates. WaitAndCollect resolves which object to compile.
        TArray<FString> Errors;
        const MaterialCompileErrorCollector::FCompileWaitOutcome Outcome =
            MaterialCompileErrorCollector::WaitAndCollect(MaterialInterface, Errors, TimeoutSeconds);
        return FromWaitOutcome(Outcome, Errors, MaterialInterface);
    }

    struct FCaptureSubjectReadiness
    {
        FString SubjectId;
        FString MaterialPath;
        FState State;
        bool bIncludedByFallbackPolicy = true;
        bool bUnassignedMaterial = false;

        bool IsKnownFallbackSubstitution() const
        {
            return bIncludedByFallbackPolicy &&
                (bUnassignedMaterial ||
                 (State.Failed() && State.bRendersDefaultMaterial));
        }

        bool IsFallbackPossible() const
        {
            return bIncludedByFallbackPolicy && !bUnassignedMaterial &&
                State.bRendersDefaultMaterial &&
                State.Status != EStatus::Completed && !State.Failed();
        }

        bool IsKnownDefaultMaterialUse() const
        {
            return bUnassignedMaterial ||
                (State.bRendersDefaultMaterial &&
                 (State.Status == EStatus::Completed || State.Failed()));
        }
    };

    enum class ECaptureMeshUsagePolicy : uint8
    {
        ThumbnailLod0Sections,
        CapturedComponentSections
    };

    struct FCaptureReadiness
    {
        TArray<FCaptureSubjectReadiness> Subjects;
        ECaptureMeshUsagePolicy MeshUsagePolicy = ECaptureMeshUsagePolicy::ThumbnailLod0Sections;
        bool bHasMeshSubjects = false;
        int32 RenderedLodIndex = INDEX_NONE;
        FString Scope;

        void AddSubject(const FString& MaterialPath, const FState& State,
            bool bIncludedByFallbackPolicy = true)
        {
            if (State.Status != EStatus::NotMeasured)
            {
                FCaptureSubjectReadiness Subject;
                Subject.SubjectId = MaterialPath;
                Subject.MaterialPath = MaterialPath;
                Subject.State = State;
                Subject.bIncludedByFallbackPolicy = bIncludedByFallbackPolicy;
                Subjects.Add(MoveTemp(Subject));
            }
        }

        void AddUnassignedSubject(const FString& SubjectId, bool bIncludedByFallbackPolicy)
        {
            FCaptureSubjectReadiness Subject;
            Subject.SubjectId = SubjectId;
            Subject.State.bRendersDefaultMaterial = true;
            Subject.bIncludedByFallbackPolicy = bIncludedByFallbackPolicy;
            Subject.bUnassignedMaterial = true;
            Subjects.Add(MoveTemp(Subject));
        }
    };

    inline FCaptureReadiness ProbeCaptureAsset(UObject* Asset,
        ECaptureMeshUsagePolicy MeshUsagePolicy = ECaptureMeshUsagePolicy::ThumbnailLod0Sections,
        int32 RenderedLodIndex = 0,
        const TSet<int32>* RenderedSectionIndices = nullptr,
        UMeshComponent* RenderedComponent = nullptr,
        const FString& Scope = TEXT("thumbnailLod0Sections"))
    {
        FCaptureReadiness Readiness;
        Readiness.MeshUsagePolicy = MeshUsagePolicy;
        Readiness.RenderedLodIndex = RenderedLodIndex;
        Readiness.Scope = Scope;
        const double CaptureWaitDeadline = FPlatformTime::Seconds() +
            MaterialCompileErrorCollector::CompileWaitTimeoutSeconds;
        TMap<const FMaterialResource*, FState> StateByResource;
        TMap<UMaterialInterface*, int32> SubjectIndexByInterface;
        const auto AddMaterial = [&Readiness, &StateByResource, &SubjectIndexByInterface,
                                  CaptureWaitDeadline](
            UMaterialInterface* Material, bool bIncludedByFallbackPolicy)
        {
            if (!Material)
            {
                return;
            }

            if (const int32* ExistingIndex = SubjectIndexByInterface.Find(Material))
            {
                if (bIncludedByFallbackPolicy &&
                    !Readiness.Subjects[*ExistingIndex].bIncludedByFallbackPolicy)
                {
                    Readiness.Subjects[*ExistingIndex].State =
                        ProbeAfterCapture(Material, CaptureWaitDeadline);
                }
                Readiness.Subjects[*ExistingIndex].bIncludedByFallbackPolicy |=
                    bIncludedByFallbackPolicy;
                return;
            }

            const FMaterialResource* Resource = ResolveMaterialResource(Material);
            FState State;
            if (bIncludedByFallbackPolicy)
            {
                State = ProbeAfterCapture(Material, CaptureWaitDeadline);
                if (Resource)
                {
                    StateByResource.Add(Resource, State);
                }
            }
            else if (Resource)
            {
                if (const FState* ExistingState = StateByResource.Find(Resource))
                {
                    State = *ExistingState;
                }
                else
                {
                    State = Probe(Material);
                    StateByResource.Add(Resource, State);
                }
            }
            else
            {
                State = Probe(Material);
            }
            const UMaterial* Base = ResolveBaseMaterial(Material);
            State.bRendersDefaultMaterial = (Base && Base->IsDefaultMaterial()) ||
                State.Status != EStatus::Completed;

            const int32 NewIndex = Readiness.Subjects.Num();
            Readiness.AddSubject(Material->GetPathName(), State, bIncludedByFallbackPolicy);
            if (Readiness.Subjects.Num() > NewIndex)
            {
                SubjectIndexByInterface.Add(Material, NewIndex);
            }
        };

        if (UMaterialInterface* Material = Cast<UMaterialInterface>(Asset))
        {
            Readiness.RenderedLodIndex = INDEX_NONE;
            Readiness.Scope = TEXT("materialThumbnail");
            AddMaterial(Material, true);
        }
        else if (UStaticMesh* StaticMesh = Cast<UStaticMesh>(Asset))
        {
            Readiness.bHasMeshSubjects = true;
            TSet<int32> UsedMaterialIndices;
            const FStaticMeshRenderData* RenderData = StaticMesh->GetRenderData();
            if (RenderData && RenderData->LODResources.IsValidIndex(RenderedLodIndex))
            {
                const FStaticMeshSectionArray& Sections =
                    RenderData->LODResources[RenderedLodIndex].Sections;
                for (int32 SectionIndex = 0; SectionIndex < Sections.Num(); ++SectionIndex)
                {
                    if (RenderedSectionIndices && !RenderedSectionIndices->Contains(SectionIndex))
                    {
                        continue;
                    }
                    const FStaticMeshSection& Section = Sections[SectionIndex];
                    if (Section.NumTriangles > 0)
                    {
                        UsedMaterialIndices.Add(Section.MaterialIndex);
                    }
                }
            }

            const TArray<FStaticMaterial>& Slots = StaticMesh->GetStaticMaterials();
            for (int32 SlotIndex = 0; SlotIndex < Slots.Num(); ++SlotIndex)
            {
                const bool bUsed = UsedMaterialIndices.Contains(SlotIndex);
                UMaterialInterface* SlotMaterial = RenderedComponent
                    ? RenderedComponent->GetMaterial(SlotIndex)
                    : Slots[SlotIndex].MaterialInterface.Get();
                if (SlotMaterial)
                {
                    AddMaterial(SlotMaterial, bUsed);
                }
                else
                {
                    Readiness.AddUnassignedSubject(FString::Printf(
                        TEXT("%s:materialSlot[%d]"), *StaticMesh->GetPathName(), SlotIndex), bUsed);
                }
            }
            for (const int32 UsedIndex : UsedMaterialIndices)
            {
                if (!Slots.IsValidIndex(UsedIndex))
                {
                    Readiness.AddUnassignedSubject(FString::Printf(
                        TEXT("%s:materialSlot[%d]"), *StaticMesh->GetPathName(), UsedIndex), true);
                }
            }
        }
        else if (USkeletalMesh* SkeletalMesh = Cast<USkeletalMesh>(Asset))
        {
            Readiness.bHasMeshSubjects = true;
            TSet<int32> UsedMaterialIndices;
            const TArray<FSkeletalMaterial>& Slots = SkeletalMesh->GetMaterials();
            const FSkeletalMeshRenderData* RenderData = SkeletalMesh->GetResourceForRendering();
            if (RenderData && RenderData->LODRenderData.IsValidIndex(RenderedLodIndex))
            {
                const FSkeletalMeshLODInfo* LODInfo = SkeletalMesh->GetLODInfo(RenderedLodIndex);
                const TArray<FSkelMeshRenderSection>& Sections =
                    RenderData->LODRenderData[RenderedLodIndex].RenderSections;
                for (int32 SectionIndex = 0; SectionIndex < Sections.Num(); ++SectionIndex)
                {
                    if (RenderedSectionIndices && !RenderedSectionIndices->Contains(SectionIndex))
                    {
                        continue;
                    }
                    const FSkelMeshRenderSection& Section = Sections[SectionIndex];
                    if (Section.NumTriangles > 0 && !Section.bDisabled)
                    {
                        int32 MaterialIndex = Section.MaterialIndex;
                        if (LODInfo && LODInfo->LODMaterialMap.IsValidIndex(SectionIndex) &&
                            Slots.IsValidIndex(LODInfo->LODMaterialMap[SectionIndex]))
                        {
                            MaterialIndex = LODInfo->LODMaterialMap[SectionIndex];
                        }
                        UsedMaterialIndices.Add(MaterialIndex);
                    }
                }
            }

            for (int32 SlotIndex = 0; SlotIndex < Slots.Num(); ++SlotIndex)
            {
                const bool bUsed = UsedMaterialIndices.Contains(SlotIndex);
                UMaterialInterface* SlotMaterial = RenderedComponent
                    ? RenderedComponent->GetMaterial(SlotIndex)
                    : Slots[SlotIndex].MaterialInterface.Get();
                if (SlotMaterial)
                {
                    AddMaterial(SlotMaterial, bUsed);
                }
                else
                {
                    Readiness.AddUnassignedSubject(FString::Printf(
                        TEXT("%s:materialSlot[%d]"), *SkeletalMesh->GetPathName(), SlotIndex), bUsed);
                }
            }
            for (const int32 UsedIndex : UsedMaterialIndices)
            {
                if (!Slots.IsValidIndex(UsedIndex))
                {
                    Readiness.AddUnassignedSubject(FString::Printf(
                        TEXT("%s:materialSlot[%d]"), *SkeletalMesh->GetPathName(), UsedIndex), true);
                }
            }
        }
        return Readiness;
    }

    // Probe the exact mesh component that produced an asset-preview frame. Skeletal preview LOD is
    // measured after the draw (forced LOD first, then predicted LOD); Static Mesh exposes no
    // predicted LOD on the game thread, so automatic mode falls back explicitly to LOD0. Editor
    // section/material preview filters and skeletal hidden-material state are applied before a
    // slot can affect fallback policy.
    inline FCaptureReadiness ProbeCaptureComponent(UMeshComponent* Component)
    {
        TSet<int32> RenderedSections;
        if (UStaticMeshComponent* StaticComponent = Cast<UStaticMeshComponent>(Component))
        {
            UStaticMesh* Mesh = StaticComponent->GetStaticMesh();
            const FStaticMeshRenderData* RenderData = Mesh ? Mesh->GetRenderData() : nullptr;
            if (!RenderData || RenderData->LODResources.Num() == 0)
            {
                return FCaptureReadiness();
            }
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 6, 0)
            const int32 ForcedLod = StaticComponent->GetForcedLodModel();
#else
            // UStaticMeshComponent gained GetForcedLodModel() in UE 5.6. Before that the value it
            // returns is read straight off the public ForcedLodModel UPROPERTY it wraps.
            const int32 ForcedLod = StaticComponent->ForcedLodModel;
#endif
            const int32 LodIndex = ForcedLod > 0
                ? FMath::Clamp(ForcedLod - 1, 0, RenderData->LODResources.Num() - 1)
                : 0;
            const FStaticMeshSectionArray& Sections = RenderData->LODResources[LodIndex].Sections;
            for (int32 SectionIndex = 0; SectionIndex < Sections.Num(); ++SectionIndex)
            {
                const FStaticMeshSection& Section = Sections[SectionIndex];
                if (Section.NumTriangles > 0 &&
                    (StaticComponent->SectionIndexPreview == INDEX_NONE ||
                     StaticComponent->SectionIndexPreview == SectionIndex) &&
                    (StaticComponent->MaterialIndexPreview == INDEX_NONE ||
                     StaticComponent->MaterialIndexPreview == Section.MaterialIndex))
                {
                    RenderedSections.Add(SectionIndex);
                }
            }
            return ProbeCaptureAsset(Mesh, ECaptureMeshUsagePolicy::CapturedComponentSections,
                LodIndex, &RenderedSections, StaticComponent,
                ForcedLod > 0 ? TEXT("staticMeshForcedLod") : TEXT("staticMeshLod0Fallback"));
        }

        if (USkeletalMeshComponent* SkeletalComponent = Cast<USkeletalMeshComponent>(Component))
        {
            USkeletalMesh* Mesh = SkeletalComponent->GetSkeletalMeshAsset();
            const FSkeletalMeshRenderData* RenderData = Mesh ? Mesh->GetResourceForRendering() : nullptr;
            if (!RenderData || RenderData->LODRenderData.Num() == 0)
            {
                return FCaptureReadiness();
            }
            const int32 ForcedLod = SkeletalComponent->GetForcedLOD();
            const int32 LodIndex = FMath::Clamp(
                ForcedLod > 0 ? ForcedLod - 1 : SkeletalComponent->GetPredictedLODLevel(),
                0, RenderData->LODRenderData.Num() - 1);
            const FSkeletalMeshLODInfo* LODInfo = Mesh->GetLODInfo(LodIndex);
            const TArray<FSkeletalMaterial>& Slots = Mesh->GetMaterials();
            const TArray<FSkelMeshRenderSection>& Sections =
                RenderData->LODRenderData[LodIndex].RenderSections;
            for (int32 SectionIndex = 0; SectionIndex < Sections.Num(); ++SectionIndex)
            {
                const FSkelMeshRenderSection& Section = Sections[SectionIndex];
                int32 MaterialIndex = Section.MaterialIndex;
                if (LODInfo && LODInfo->LODMaterialMap.IsValidIndex(SectionIndex) &&
                    Slots.IsValidIndex(LODInfo->LODMaterialMap[SectionIndex]))
                {
                    MaterialIndex = LODInfo->LODMaterialMap[SectionIndex];
                }
                if (Section.NumTriangles > 0 && !Section.bDisabled &&
                    SkeletalComponent->IsMaterialSectionShown(MaterialIndex, LodIndex) &&
                    (SkeletalComponent->GetSectionPreview() == INDEX_NONE ||
                     SkeletalComponent->GetSectionPreview() == SectionIndex) &&
                    (SkeletalComponent->GetMaterialPreview() == INDEX_NONE ||
                     SkeletalComponent->GetMaterialPreview() == MaterialIndex))
                {
                    RenderedSections.Add(SectionIndex);
                }
            }
            return ProbeCaptureAsset(Mesh, ECaptureMeshUsagePolicy::CapturedComponentSections,
                LodIndex, &RenderedSections, SkeletalComponent,
                ForcedLod > 0 ? TEXT("skeletalMeshForcedLod") : TEXT("skeletalMeshPredictedLod"));
        }
        return FCaptureReadiness();
    }

    enum class ECaptureReadinessReason : uint8
    {
        None,
        ShaderMapFailed,
        ShaderMapIncomplete,
        UnassignedMaterialSlot
    };

    inline const TCHAR* ToWire(ECaptureReadinessReason Reason)
    {
        switch (Reason)
        {
        case ECaptureReadinessReason::ShaderMapFailed:     return TEXT("shaderMapFailed");
        case ECaptureReadinessReason::ShaderMapIncomplete: return TEXT("shaderMapIncomplete");
        default:                                          return TEXT("unassignedMaterialSlot");
        }
    }

    inline ECaptureReadinessReason AddCaptureReadiness(const TSharedPtr<FJsonObject>& Result,
        const FCaptureReadiness& Readiness, bool bAllowFallback,
        bool bCaptureUsesSubjectMaterials = true)
    {
        if (!Result.IsValid())
        {
            return ECaptureReadinessReason::None;
        }

        bool bAllCompiled = Readiness.Subjects.Num() > 0;
        bool bAnyCompiling = false;
        bool bAnyFailed = false;
        bool bUsingDefaultMaterial = false;
        bool bFallbackOccurred = false;
        bool bFallbackPossible = false;
        bool bShaderMapFailedFallback = false;
        TArray<TSharedPtr<FJsonValue>> Subjects;
        Subjects.Reserve(Readiness.Subjects.Num());

        for (const FCaptureSubjectReadiness& Subject : Readiness.Subjects)
        {
            const bool bCompiled = Subject.State.Status == EStatus::Completed;
            const bool bCompiling = Subject.State.Status == EStatus::Outstanding ||
                Subject.State.Status == EStatus::TimedOut;
            const bool bSubjectFallback = bCaptureUsesSubjectMaterials &&
                Subject.IsKnownFallbackSubstitution();
            const bool bSubjectFallbackPossible = bCaptureUsesSubjectMaterials &&
                Subject.IsFallbackPossible();
            bAllCompiled &= bCompiled;
            bAnyCompiling |= bCompiling;
            bAnyFailed |= Subject.State.Failed();
            bUsingDefaultMaterial |= bCaptureUsesSubjectMaterials &&
                Subject.bIncludedByFallbackPolicy && Subject.IsKnownDefaultMaterialUse();
            bFallbackOccurred |= bSubjectFallback;
            bFallbackPossible |= bSubjectFallbackPossible;
            bShaderMapFailedFallback |= bSubjectFallback && Subject.State.Failed();

            const TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
            Row->SetStringField(TEXT("subjectId"), Subject.SubjectId);
            Row->SetStringField(TEXT("materialPath"), Subject.MaterialPath);
            Row->SetStringField(TEXT("status"), ToWire(Subject.State.Status));
            Row->SetBoolField(TEXT("compiled"), bCompiled);
            Row->SetBoolField(TEXT("compiling"), bCompiling);
            Row->SetBoolField(TEXT("failed"), Subject.State.Failed());
            Row->SetBoolField(TEXT("usingDefaultMaterial"),
                Subject.IsKnownDefaultMaterialUse());
            Row->SetBoolField(TEXT("includedByFallbackPolicy"),
                Subject.bIncludedByFallbackPolicy);
            if (Readiness.bHasMeshSubjects)
            {
                Row->SetBoolField(TEXT("usedByRenderedSections"),
                    Subject.bIncludedByFallbackPolicy);
            }
            Row->SetBoolField(TEXT("fallbackOccurred"), bSubjectFallback);
            Row->SetBoolField(TEXT("fallbackPossible"), bSubjectFallbackPossible);
            Row->SetBoolField(TEXT("unassignedMaterial"), Subject.bUnassignedMaterial);
            Row->SetNumberField(TEXT("errorCount"), Subject.State.Errors.Num());
            Row->SetArrayField(TEXT("errors"), EmitStringArray(Subject.State.Errors));
            Subjects.Add(MakeShared<FJsonValueObject>(Row));
        }

        const TSharedPtr<FJsonObject> Block = MakeShared<FJsonObject>();
        Block->SetBoolField(TEXT("measured"), Readiness.Subjects.Num() > 0);
        Block->SetNumberField(TEXT("subjectCount"), Readiness.Subjects.Num());
        Block->SetBoolField(TEXT("compiled"), bAllCompiled);
        Block->SetBoolField(TEXT("compiling"), bAnyCompiling);
        Block->SetBoolField(TEXT("failed"), bAnyFailed);
        Block->SetBoolField(TEXT("usingDefaultMaterial"), bUsingDefaultMaterial);
        Block->SetBoolField(TEXT("fallbackOccurred"), bFallbackOccurred);
        Block->SetBoolField(TEXT("fallbackPossible"), bFallbackPossible);
        Block->SetBoolField(TEXT("allowFallback"), bAllowFallback);
        Block->SetBoolField(TEXT("subjectMaterialsRendered"), bCaptureUsesSubjectMaterials);
        if (Readiness.bHasMeshSubjects)
        {
            Block->SetStringField(TEXT("meshUsagePolicy"),
                Readiness.MeshUsagePolicy == ECaptureMeshUsagePolicy::CapturedComponentSections
                    ? TEXT("capturedComponentSections")
                    : TEXT("thumbnailLod0Sections"));
            Block->SetNumberField(TEXT("renderedLodIndex"), Readiness.RenderedLodIndex);
            Block->SetStringField(TEXT("scope"), Readiness.Scope);
        }
        Block->SetArrayField(TEXT("subjects"), Subjects);
        const ECaptureReadinessReason ReadinessReason = bFallbackOccurred
            ? (bShaderMapFailedFallback
                ? ECaptureReadinessReason::ShaderMapFailed
                : ECaptureReadinessReason::UnassignedMaterialSlot)
            : (bFallbackPossible
                ? ECaptureReadinessReason::ShaderMapIncomplete
                : ECaptureReadinessReason::None);
        if (bFallbackOccurred)
        {
            Block->SetStringField(TEXT("reason"), ToWire(ReadinessReason));
        }
        else if (bFallbackPossible)
        {
            Block->SetStringField(TEXT("possibleReason"), ToWire(ReadinessReason));
        }
        Result->SetObjectField(TEXT("materialReadiness"), Block);
        return ReadinessReason;
    }

    // Adds the stable wire block and returns whether a handler may publish a successful capture.
    inline bool ApplyCaptureFallbackPolicy(const TSharedPtr<FJsonObject>& Result,
        const FCaptureReadiness& Readiness, bool bAllowFallback,
        bool bCaptureUsesSubjectMaterials = true)
    {
        const ECaptureReadinessReason ReadinessReason = AddCaptureReadiness(
            Result, Readiness, bAllowFallback, bCaptureUsesSubjectMaterials);
        if (ReadinessReason == ECaptureReadinessReason::None)
        {
            return true;
        }
        const bool bKnownFallback =
            ReadinessReason != ECaptureReadinessReason::ShaderMapIncomplete;
        if (bKnownFallback && !bAllowFallback)
        {
            return false;
        }

        TArray<TSharedPtr<FJsonValue>> Warnings;
        const TArray<TSharedPtr<FJsonValue>>* ExistingWarnings = nullptr;
        if (Result.IsValid() &&
            Result->TryGetArrayField(TEXT("warnings"), ExistingWarnings) && ExistingWarnings)
        {
            Warnings.Append(*ExistingWarnings);
        }
        const TCHAR* Warning = ReadinessReason == ECaptureReadinessReason::ShaderMapFailed
            ? TEXT("The captured subject used the engine Default Material because shader "
                   "compilation failed. The image is retained only because allowFallback is "
                   "true; see materialReadiness.subjects[].errors for the compile errors.")
            : (ReadinessReason == ECaptureReadinessReason::ShaderMapIncomplete
                ? TEXT("The captured subject may have used the engine Default Material because "
                       "its shader map was still incomplete after the bounded post-capture wait. "
                       "No failed compile or confirmed substitution was measured, so the image is "
                       "retained without requiring allowFallback; see "
                       "materialReadiness.subjects[].status and fallbackPossible.")
                : TEXT("The capture policy found an unassigned material slot that uses the engine "
                       "Default Material. The image is retained only because allowFallback is true; "
                       "see materialReadiness.subjects[].subjectId and unassignedMaterial."));
        Warnings.Add(MakeShared<FJsonValueString>(Warning));
        if (Result.IsValid())
        {
            Result->SetArrayField(TEXT("warnings"), Warnings);
        }
        return true;
    }

    // What a caller should do next, for every status that is not a clean pass. Empty for
    // `completed` - a verb that has nothing to warn about should say nothing.
    inline FString DescribeRemedy(const FState& State)
    {
        switch (State.Status)
        {
        case EStatus::Failed:
            return TEXT("The shader FAILED to compile, so this material renders as the engine "
                "Default Material. The HLSL errors are in shaderCompile.errors; fix them and "
                "re-run the write. A capture of this material is not evidence of anything.");
        case EStatus::Outstanding:
            return TEXT("A shader compile is still in flight, so no verdict is available yet. "
                "Re-read it, or call material.authoring.compile_material to block on it.");
        case EStatus::TimedOut:
            return TEXT("The bounded wait expired with the compile still running. The compile was "
                "NOT abandoned; call material.authoring.compile_material to read the final verdict.");
        case EStatus::NotCompiled:
            // The parentInherited wording is the whole point of measuredSubject: on an instance
            // with no static permutation this status is the PARENT's, and a caller who reads it as
            // "my instance draws nothing" is chasing an asset that has no shader to fix.
            if (State.Subject == EMeasuredSubject::ParentInherited)
            {
                // A parentless instance has no resource at all, so there is no
                // measuredMaterialPath to point at and no compile anyone can run on it.
                return State.MeasuredMaterialPath.IsEmpty()
                    ? FString(TEXT("This material instance has NO PARENT, so it owns no shader and "
                        "inherits none: there is no resource to compile and nothing for "
                        "material.authoring.compile_material to measure. Assign a parent with "
                        "material.authoring.set_material_instance_parent first."))
                    : FString(TEXT("No shader compile has run, so an empty errors list is NOT "
                        "evidence that this renders. Note what was measured: this material "
                        "instance owns no shader (no static permutation), so the status above was "
                        "read off its parent's resource named in "
                        "shaderCompile.measuredMaterialPath, and the instance itself cannot be in "
                        "a broken state the parent is not. material.authoring.compile_material "
                        "accepts this instance and compiles that resource without dirtying or "
                        "saving the parent."));
            }
            return TEXT("No shader compile has run for this material, so an empty errors "
                "list is NOT evidence that it compiles - a graph write is not a shader compile. "
                "Call material.authoring.compile_material (or pass waitForShaderCompile:true "
                "where the verb offers it) before trusting a render of this material.");
        default:
            return FString();
        }
    }

    // What `rendersDefaultMaterial` does and does not cover, said in the response that carries it.
    //
    // It is a shader-map answer about one resource, and the usage flags are the other half: a
    // vertex factory refuses to compile its permutations for a usage the material does not declare
    // (GPUSkinVertexFactory.cpp gates on bIsUsedWithSkeletalMesh), so the map is COMPLETE without
    // them and this boolean reads false while the mesh that uses the material draws the engine
    // Default Material. Five builds of one stream were spent on that gap.
    inline FString DescribeMeasurementScope(const FState& State)
    {
        const TCHAR* SubjectClause = TEXT("this material's own shader map");
        switch (State.Subject)
        {
        case EMeasuredSubject::ParentInherited:
            SubjectClause = State.MeasuredMaterialPath.IsEmpty()
                // A parentless instance inherits no resource, so nothing was measured and the
                // flag is the failure-direction default rather than a reading.
                ? TEXT("nothing (this instance has no parent and therefore no shader resource)")
                : TEXT("the PARENT material's shader map (this instance owns no static permutation)");
            break;
        case EMeasuredSubject::InstanceStaticPermutation:
            SubjectClause = TEXT("this instance's own static-permutation shader map");
            break;
        case EMeasuredSubject::None:
            // The multi-material fold. There is no single subject, and saying so is what keeps
            // this sentence — and therefore the qualification — on that path too.
            SubjectClause = TEXT("the shader map of EACH material folded into this response, "
                "OR-ed together, so it is true when any one of them would be substituted; "
                "shaderCompile.materials[] carries the per-material subject and usage set");
            break;
        default:
            break;
        }

        return FString::Printf(TEXT("rendersDefaultMaterial was measured on %s. It covers only the "
            "permutations those shader maps contain, and it is NOT a verdict about a particular "
            "consumer: the renderer substitutes the engine Default Material for any mesh, particle "
            "or instanced-mesh consumer whose EMaterialUsage the material does not declare, and "
            "those permutations are never compiled into the map, so this flag stays false while "
            "that substitution happens. The declared set is in shaderCompile.declaredUsages, or "
            "per row in shaderCompile.materials[] on a multi-material response; the full set with "
            "the property behind each flag is on material.authoring.get_material_info."),
            SubjectClause);
    }

    // Emits `shaderCompile: {status, succeeded, failed, errorCount, errors[], waited, waitedMs,
    // rendersDefaultMaterial, rendersDefaultMaterialScope, measuredSubject?,
    // measuredMaterialPath?, declaredUsages?, hint?, materials[]?}`.
    //
    // `rendersDefaultMaterialScope` is unconditional: the boolean is never published bare. The
    // three subject fields are omitted on the multi-material fold, where `materials[]` carries a
    // subject and usage set per row instead.
    //
    // `status` is the single field to branch on and uses the same five spellings as
    // material.authoring.compile_material's `compileStatus`. `succeeded` is true ONLY for
    // `completed`: a probe that never triggered a compile reports an empty error list and is not a
    // success. `failed` is the permanent case, kept as its own boolean because the recurring
    // mis-read is treating a permanent failure as a transient "still warming up".
    //
    // Nothing is emitted for a NotMeasured state on an asset that has no shader of its own (a
    // material function, a parameter collection, a texture), so the block never appears on a
    // response where it would be noise.
    inline void AddReport(const TSharedPtr<FJsonObject>& Result, const FState& State)
    {
        if (!Result.IsValid() || State.Status == EStatus::NotMeasured)
        {
            return;
        }

        const TSharedPtr<FJsonObject> Block = MakeShared<FJsonObject>();
        Block->SetStringField(TEXT("status"), ToWire(State.Status));
        Block->SetBoolField(TEXT("succeeded"), State.Succeeded());
        Block->SetBoolField(TEXT("failed"), State.Failed());
        Block->SetNumberField(TEXT("errorCount"), State.Errors.Num());

        Block->SetArrayField(TEXT("errors"), EmitStringArray(State.Errors));

        Block->SetBoolField(TEXT("waited"), State.bWaited);
        Block->SetNumberField(TEXT("waitedMs"), State.WaitedSeconds * 1000.0);
        Block->SetBoolField(TEXT("rendersDefaultMaterial"), State.bRendersDefaultMaterial);

        // The qualification travels with the boolean on EVERY path — including the multi-material
        // fold, where there is no single subject but the usage half still applies and the sentence
        // says where the per-material subjects are. Emitting the bare boolean anywhere is the
        // defect this block exists to close.
        Block->SetStringField(TEXT("rendersDefaultMaterialScope"), DescribeMeasurementScope(State));
        if (State.Subject != EMeasuredSubject::None)
        {
            Block->SetStringField(TEXT("measuredSubject"), ToWire(State.Subject));
            // Omitted rather than emitted empty for a parentless instance: a field naming nothing
            // sends the caller looking for an asset that does not exist.
            if (!State.MeasuredMaterialPath.IsEmpty())
            {
                Block->SetStringField(TEXT("measuredMaterialPath"), State.MeasuredMaterialPath);
            }
            Block->SetArrayField(TEXT("declaredUsages"), EmitStringArray(State.DeclaredUsages));
        }

        const FString Remedy = DescribeRemedy(State);
        if (!Remedy.IsEmpty())
        {
            Block->SetStringField(TEXT("hint"), Remedy);
        }

        if (State.PerMaterial.Num() > 0)
        {
            TArray<TSharedPtr<FJsonValue>> PerMaterialArray;
            for (const FState::FPerMaterialEntry& Entry : State.PerMaterial)
            {
                const TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
                Row->SetStringField(TEXT("assetPath"), Entry.AssetPath);
                Row->SetStringField(TEXT("status"), ToWire(Entry.Status));
                // The per-material half of the qualification the aggregate cannot carry.
                Row->SetStringField(TEXT("measuredSubject"), ToWire(Entry.Subject));
                Row->SetArrayField(TEXT("declaredUsages"),
                    EmitStringArray(Entry.DeclaredUsages));
                PerMaterialArray.Add(MakeShared<FJsonValueObject>(Row));
            }
            Block->SetArrayField(TEXT("materials"), PerMaterialArray);
        }

        Result->SetObjectField(TEXT("shaderCompile"), Block);
    }

    // Probe whatever the verb was handed and publish the block. A no-op for anything that is not a
    // UMaterialInterface, so a verb that reports a material function, a layer info object or a
    // parameter collection through the same funnel is unaffected.
    inline void AddReportForAsset(const TSharedPtr<FJsonObject>& Result, UObject* Asset,
        bool bWaitForShaderCompile = false)
    {
        UMaterialInterface* MaterialInterface = Cast<UMaterialInterface>(Asset);
        if (!MaterialInterface)
        {
            return;
        }
        AddReport(Result, bWaitForShaderCompile ? ProbeAndWait(MaterialInterface)
                                                : Probe(MaterialInterface));
    }

    // Canonical wire name for the opt-in blocking wait, so the verbs that offer it cannot spell it
    // differently. Verbs that do NOT offer it publish the non-blocking probe and point at
    // material.authoring.compile_material through the `hint` above.
    inline const TCHAR* WaitParamName()
    {
        return TEXT("waitForShaderCompile");
    }

    inline FParamSpec WaitParamSpec()
    {
        return FParamSpec{
            FString(WaitParamName()),
            TEXT("boolean"),
            TEXT("Block until this material's shaders finish compiling and report the real verdict "
                 "in shaderCompile.status (default false, which reports the non-blocking probe - "
                 "and in a headless editor an un-drawn material probes as notCompiled forever)"),
            false,
            TEXT("false")};
    }

    inline const TCHAR* AllowFallbackParamName()
    {
        return TEXT("allowFallback");
    }

    inline FParamSpec AllowFallbackParamSpec()
    {
        return FParamSpec{
            FString(AllowFallbackParamName()),
            TEXT("boolean"),
            TEXT("Allow a capture with a known engine Default Material substitution. Defaults to "
                 "false; without this opt-in a failed shader or unassigned rendered slot returns "
                 "MATERIAL_FALLBACK. An incomplete shader status remains successful with a "
                 "fallbackPossible warning because it is not a confirmed failure."),
            false,
            TEXT("false")};
    }
}

namespace PinWright::Material
{
    // The material-layer replacement for the bare AddAssetVerification(Result, Asset) that every
    // material write verb used to end on. Same persistence verification, plus the shader-compile
    // verdict for the material it just wrote.
    //
    // It exists as one funnel rather than 35 remembered call sites because the defect being closed
    // is precisely that the information was available and nobody asked for it; a convention a verb
    // author has to remember reproduces the defect on verb 36.
    inline void AddMaterialVerification(const TSharedPtr<FJsonObject>& Result, UObject* Asset,
        bool bWaitForShaderCompile = false)
    {
        AddAssetVerification(Result, Asset);
        PinWright::MaterialShaderState::AddReportForAsset(Result, Asset, bWaitForShaderCompile);
    }
}
