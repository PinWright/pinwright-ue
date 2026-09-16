// Copyright (c) 2026 Alexander Penkin. MIT License.

// SpawnMaterialUtils.h - shared `materialPath` / `materialPaths` support for the spawn
// verbs (actor.spawn, actor.spawn_shape, actor.spawn_batch).
//
// Mirrors blueprint.scs.add_component's materialPath slot (PinWright_SCSHandlers.cpp
// AddSCSComponent :738-749 - LoadObject<UMaterialInterface> -> SetMaterial(0, Mat) on the
// component, echoed back as `material_applied`) and widens it to a per-slot materialPaths
// array.
//
// STORAGE. Material slots on a placed actor live on the mesh COMPONENT, in
// UMeshComponent::OverrideMaterials - a plain serialized UPROPERTY
// (Engine/Classes/Components/MeshComponent.h:29-31; EditAnywhere|AdvancedDisplay,
// Category=Rendering, no Transient/DuplicateTransient). Writing it is therefore what
// persists through map save/reload and what the Details panel shows. The StaticMesh
// ASSET's StaticMaterials array that static_mesh.set_material writes is a DIFFERENT store
// (the shared mesh default, affecting every instance everywhere) and is deliberately not
// touched here - a spawn verb must produce a per-instance override.
//
// WRITE PATH. We go through UMeshComponent::SetMaterial (Engine/Private/Components/
// MeshComponent.cpp:63-142) rather than assigning OverrideMaterials directly: only
// SetMaterial grows the array, invalidates the render state + PSO cache + streaming
// manager + body-instance physical materials, and rebroadcasts static-lighting
// registration. The UPROPERTY's own comment forbids direct writes (GC races the rendering
// thread). Neither UStaticMeshComponent nor USkeletalMeshComponent overrides SetMaterial,
// so the UMeshComponent body is the implementation for both mesh kinds.
//
// EDITOR CEREMONY. SetMaterial calls neither Modify() nor MarkPackageDirty(). The engine's
// own drag-a-material-onto-an-actor path is FComponentEditorUtils::
// AttemptApplyMaterialToComponent (Editor/UnrealEd/Private/Kismet2/ComponentEditorUtils.cpp
// :925-981): FScopedTransaction -> Modify() -> PreEditChange(OverrideMaterials) ->
// SetMaterial -> MarkRenderStateDirty() -> PostEditChangeProperty(ValueSet) ->
// GEditor->OnSceneMaterialsModified(). We take its slot bound
// (FMath::Max(OverrideMaterials.Num(), GetNumMaterials())) and its Modify/SetMaterial/
// MarkRenderStateDirty core, and deliberately drop PreEditChange/PostEditChangeProperty:
// they are not needed for persistence (Modify() is what dirties the package -
// UObject::PreEditChange, Obj.cpp:521-546, only forwards to Modify(bShouldMarkAsDirty)),
// UActorComponent::PreEditChange builds an FComponentReregisterContext and flushes
// rendering commands per call (512x for a full spawn_batch) and checkf's that a paired
// PostEditChangeProperty follows, and no actor.* handler in this plugin uses them - the
// house pattern is Modify() -> mutate -> MarkRenderStateDirty() -> MarkPackageDirty()
// (ActorTransformHandler.cpp:57-63, ComponentHandler.cpp:225,300-305,
// SplineHandler.cpp:929-937, EnvironmentHandler.cpp:889-905).
//
// Kept as inline functions in a NAMED namespace (not an anonymous one) so all three spawn
// handler translation units share one definition without an ODR / redefinition error when
// Unity merges them into a single TU - same rationale as the sibling ShapeSpawnUtils.h.
#pragma once

#include "CoreMinimal.h"
#include "Editor.h"
#include "Components/MeshComponent.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "GameFramework/Actor.h"
#include "Materials/MaterialInterface.h"
#include "Handlers/ErrorCodes.h"
#include "Handlers/ParamAliasUtils.h"
#include "Handlers/ParamSpec.h"
#include "Utils/JsonUtils.h"
#include "Utils/PathUtils.h"

namespace SpawnMaterialUtils
{
    // ---- wire names / param specs -----------------------------------------

    // Single-slot material path, canonical first. `materialPath` matches
    // blueprint.scs.add_component exactly; the snake_case alias follows the plugin-wide
    // camelCase/snake_case param convention (Conventions section of CLAUDE.md).
    inline const TArray<FString>& MaterialPathKeys()
    {
        static const TArray<FString> Keys = { TEXT("materialPath"), TEXT("material_path") };
        return Keys;
    }

    // Multi-slot material path array, canonical first.
    inline const TArray<FString>& MaterialPathsKeys()
    {
        static const TArray<FString> Keys = { TEXT("materialPaths"), TEXT("material_paths") };
        return Keys;
    }

    inline FParamSpec MaterialPathParam()
    {
        return ParamAliasUtils::MakeAliasParamSpec(TEXT("materialPath"), TEXT("path"),
            TEXT("Material asset path assigned to material slot 0 of the spawned actor's mesh component - same slot-0 semantics as blueprint.scs.add_component's materialPath. Ignored when materialPaths is also supplied. An unsafe or unloadable path fails the call with SECURITY_VIOLATION / MATERIAL_NOT_FOUND before anything is spawned."),
            /*bRequired=*/false, MaterialPathKeys());
    }

    inline FParamSpec MaterialPathsParam()
    {
        return ParamAliasUtils::MakeAliasParamSpec(TEXT("materialPaths"), TEXT("array"),
            TEXT("Material asset paths for a multi-slot mesh: index i is assigned to slot i. Takes precedence over materialPath. An empty string or null entry leaves that slot on the mesh default. Entries past the mesh's slot count are reported in 'warnings' and do not fail the call."),
            /*bRequired=*/false, MaterialPathsKeys());
    }

    // ---- parse ------------------------------------------------------------

    // One request's slot->path bindings, read off the wire but not yet loaded. Index i
    // binds slot i; an empty entry means "leave slot i on the mesh default".
    struct FMaterialSpec
    {
        TArray<FString> SlotPaths;

        bool HasAny() const
        {
            for (const FString& Path : SlotPaths)
            {
                if (!Path.IsEmpty())
                {
                    return true;
                }
            }
            return false;
        }
    };

    // Reads materialPath / materialPaths out of any JSON object: the RPC payload for
    // actor.spawn / actor.spawn_shape / actor.spawn_batch's batch-level defaults, or a
    // single transforms[] entry for a per-placement override. materialPaths wins when both
    // are present. Returns false with OutError set when materialPaths is present but is not
    // an array of strings/nulls (caller sends INVALID_PARAMS).
    //
    // bOutPresent is true when either key was present at all - that is what lets a
    // spawn_batch entry distinguish "no override, inherit the batch default" from an
    // explicit empty override, exactly as the existing per-entry `name` slot distinguishes
    // "absent -> auto-label" from a supplied label.
    inline bool ParseFromJson(const TSharedPtr<FJsonObject>& Obj, FMaterialSpec& OutSpec,
                              bool& bOutPresent, FString& OutError)
    {
        OutSpec = FMaterialSpec();
        bOutPresent = false;
        OutError.Empty();
        if (!Obj.IsValid())
        {
            return true;
        }

        for (const FString& Key : MaterialPathsKeys())
        {
            if (!Obj->HasField(Key))
            {
                continue;
            }
            const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
            if (!Obj->TryGetArrayField(Key, Arr) || !Arr)
            {
                OutError = FString::Printf(
                    TEXT("'%s' must be an array of material asset path strings"), *Key);
                return false;
            }
            bOutPresent = true;
            OutSpec.SlotPaths.Reserve(Arr->Num());
            for (int32 i = 0; i < Arr->Num(); ++i)
            {
                const TSharedPtr<FJsonValue>& Value = (*Arr)[i];
                if (!Value.IsValid() || Value->Type == EJson::Null)
                {
                    OutSpec.SlotPaths.Add(FString());
                    continue;
                }
                if (Value->Type != EJson::String)
                {
                    OutError = FString::Printf(
                        TEXT("'%s'[%d] must be a material asset path string, or null/\"\" to leave that slot on the mesh default"),
                        *Key, i);
                    return false;
                }
                OutSpec.SlotPaths.Add(Value->AsString().TrimStartAndEnd());
            }
            return true; // materialPaths wins over materialPath
        }

        for (const FString& Key : MaterialPathKeys())
        {
            FString Single;
            if (Obj->TryGetStringField(Key, Single))
            {
                bOutPresent = true;
                Single.TrimStartAndEndInline();
                if (!Single.IsEmpty())
                {
                    OutSpec.SlotPaths.Add(Single);
                }
                return true;
            }
        }
        return true;
    }

    // ---- resolve ----------------------------------------------------------

    // Result of resolving one FMaterialSpec. Index i is slot i; a null entry means
    // "leave slot i alone" (empty input path).
    struct FResolvedMaterials
    {
        TArray<UMaterialInterface*> SlotMaterials;

        bool HasAny() const
        {
            for (UMaterialInterface* Mat : SlotMaterials)
            {
                if (Mat)
                {
                    return true;
                }
            }
            return false;
        }
    };

    // Loads every non-empty path in Spec. On failure returns false with OutErrorCode set to
    // SECURITY_VIOLATION (path outside a project asset root / traversal) or
    // MATERIAL_NOT_FOUND (nothing loadable), matching static_mesh.set_material's wording.
    //
    // CALLERS MUST RUN THIS BEFORE MUTATING THE WORLD. Failing here costs nothing; failing
    // after the spawn would leave an orphan actor behind. This is the reason the spawn verbs
    // are stricter than blueprint.scs.add_component, which cannot pre-flight (its SCS node
    // already exists by the time it loads the material).
    //
    // ContextLabel is appended to the message (e.g. "transforms[7]") so a spawn_batch error
    // names the offending entry.
    inline bool Resolve(const FMaterialSpec& Spec, FResolvedMaterials& Out,
                        FString& OutErrorCode, FString& OutErrorMessage,
                        const FString& ContextLabel = FString())
    {
        Out = FResolvedMaterials();
        Out.SlotMaterials.SetNumZeroed(Spec.SlotPaths.Num());

        const FString Where = ContextLabel.IsEmpty()
            ? FString()
            : FString::Printf(TEXT(" of %s"), *ContextLabel);

        for (int32 Slot = 0; Slot < Spec.SlotPaths.Num(); ++Slot)
        {
            const FString& RawPath = Spec.SlotPaths[Slot];
            if (RawPath.IsEmpty())
            {
                continue;
            }

            // SECURITY: constrain the material to a project-relative asset root, matching
            // static_mesh.set_material / spline.set_spline_mesh_material /
            // landscape.set_material.
            const FString SafePath = SanitizeProjectRelativePath(RawPath);
            if (SafePath.IsEmpty())
            {
                OutErrorCode = ErrorCodes::ERR_SECURITY_VIOLATION;
                OutErrorMessage = FString::Printf(
                    TEXT("Invalid or unsafe material path for slot %d%s: %s. Path must be relative to project (e.g. /Game/...)"),
                    Slot, *Where, *RawPath);
                return false;
            }

            UMaterialInterface* Material = LoadObject<UMaterialInterface>(nullptr, *SafePath);
            if (!Material)
            {
                OutErrorCode = ErrorCodes::ERR_MATERIAL_NOT_FOUND;
                OutErrorMessage = FString::Printf(
                    TEXT("Material not found for slot %d%s: %s"), Slot, *Where, *SafePath);
                return false;
            }
            Out.SlotMaterials[Slot] = Material;
        }
        return true;
    }

    // Convenience: parse + resolve in one step off a payload/entry object. Returns false with
    // OutErrorCode/OutErrorMessage populated; INVALID_PARAMS for a malformed materialPaths.
    inline bool ParseAndResolve(const TSharedPtr<FJsonObject>& Obj, FMaterialSpec& OutSpec,
                                FResolvedMaterials& OutResolved, bool& bOutPresent,
                                FString& OutErrorCode, FString& OutErrorMessage,
                                const FString& ContextLabel = FString())
    {
        FString ParseError;
        if (!ParseFromJson(Obj, OutSpec, bOutPresent, ParseError))
        {
            OutErrorCode = ErrorCodes::ERR_INVALID_PARAMS;
            OutErrorMessage = ContextLabel.IsEmpty()
                ? ParseError
                : FString::Printf(TEXT("%s (in %s)"), *ParseError, *ContextLabel);
            return false;
        }
        return Resolve(OutSpec, OutResolved, OutErrorCode, OutErrorMessage, ContextLabel);
    }

    // ---- apply ------------------------------------------------------------

    // The mesh component that owns an actor's material slots. Root-first, because
    // AStaticMeshActor / ASkeletalMeshActor put their mesh component at the root; falls back
    // to the first UMeshComponent anywhere on the actor so a BP / arbitrary-class spawn with a
    // non-root mesh still works. Returns nullptr for an actor with no mesh component at all
    // (a light, a volume, a bare AActor) - the graceful-degradation case, reported as a
    // warning rather than an error because the actor itself spawned fine and the miss is
    // only detectable after the spawn.
    inline UMeshComponent* FindMaterialTargetComponent(AActor* Actor)
    {
        if (!Actor)
        {
            return nullptr;
        }
        if (UMeshComponent* RootMesh = Cast<UMeshComponent>(Actor->GetRootComponent()))
        {
            return RootMesh;
        }
        return Actor->FindComponentByClass<UMeshComponent>();
    }

    // ---- construction-script notice ---------------------------------------

    // Stable head of the construction-script notice. It is BOTH the literal the warning is
    // built from and the prefix AddMaterialReport matches on to emit the machine-readable
    // flag, so the two can never drift apart - and no spawn handler has to thread an extra
    // out-param from Apply through to its response builder.
    inline const TCHAR* ConstructionScriptWarningPrefix()
    {
        return TEXT("materialPath was applied to '");
    }

    // A Blueprint's generated class is '<Asset>_C'; the asset is what the caller opens and
    // edits, so that is the name the notice should carry.
    inline FString SpawnedClassDisplayName(const AActor* Actor)
    {
        FString Name = (Actor && Actor->GetClass()) ? Actor->GetClass()->GetName() : FString();
        Name.RemoveFromEnd(TEXT("_C"));
        return Name;
    }

    inline FString MakeConstructionScriptWarning(const FString& ComponentName,
                                                 const FString& OwnerDisplayName)
    {
        return FString::Printf(
            TEXT("%s%s', which is created by %s's construction script. The per-instance override is preserved across construction-script reruns and across a Blueprint recompile, including one that renames the component. It is still a per-instance delta, so it is lost if the component is removed from the Blueprint or the instance is replaced. For a durable default, set the material on the Blueprint itself with blueprint.scs.add_component {materialPath}."),
            ConstructionScriptWarningPrefix(), *ComponentName, *OwnerDisplayName);
    }

    inline bool HasConstructionScriptWarning(const TArray<FString>& Warnings)
    {
        for (const FString& Warning : Warnings)
        {
            if (Warning.StartsWith(ConstructionScriptWarningPrefix()))
            {
                return true;
            }
        }
        return false;
    }

    // Applies resolved slot materials to Actor's mesh component, appending any non-fatal miss
    // to OutWarnings. Returns the number of slots actually written.
    //
    // Slot bound is FMath::Max(GetNumMaterials(), OverrideMaterials.Num()), exactly as
    // FComponentEditorUtils::AttemptApplyMaterialToComponent computes it - the asset's slot
    // count, widened by anything already overridden. Indices past it are skipped with a
    // warning rather than hard-failing: UMeshComponent::SetMaterial would happily AddZeroed
    // past the real slot count, producing phantom override entries the editor's
    // CleanUpOverrideMaterials later trims. We refuse to create them.
    inline int32 Apply(AActor* Actor, const FResolvedMaterials& Resolved,
                       TArray<FString>& OutWarnings, FString& OutComponentName)
    {
        OutComponentName.Empty();
        if (!Actor || !Resolved.HasAny())
        {
            return 0;
        }

        UMeshComponent* MeshComp = FindMaterialTargetComponent(Actor);
        if (!MeshComp)
        {
            OutWarnings.Add(FString::Printf(
                TEXT("Actor '%s' has no mesh component; %d requested material slot(s) were not applied"),
                *Actor->GetActorLabel(), Resolved.SlotMaterials.Num()));
            return 0;
        }
        OutComponentName = MeshComp->GetName();

        const int32 SlotCount =
            FMath::Max(MeshComp->GetNumMaterials(), MeshComp->OverrideMaterials.Num());

        int32 Applied = 0;
        bool bModified = false;
        for (int32 Slot = 0; Slot < Resolved.SlotMaterials.Num(); ++Slot)
        {
            UMaterialInterface* Material = Resolved.SlotMaterials[Slot];
            if (!Material)
            {
                continue;
            }
            if (Slot >= SlotCount)
            {
                OutWarnings.Add(FString::Printf(
                    TEXT("Material slot %d is out of range on '%s' (%s has %d slot(s)); '%s' was not applied"),
                    Slot, *Actor->GetActorLabel(), *MeshComp->GetName(), SlotCount,
                    *Material->GetPathName()));
                continue;
            }
            if (!bModified)
            {
                // Modify() is what dirties the level package (UObject::PreEditChange does
                // nothing else for that purpose) and records the pre-change state when a
                // transaction is open. Once per component, not once per slot.
                MeshComp->Modify();
                bModified = true;
            }
            MeshComp->SetMaterial(Slot, Material);
            ++Applied;
        }

        if (bModified)
        {
            MeshComp->MarkRenderStateDirty();
            MeshComp->MarkPackageDirty();
            Actor->MarkPackageDirty();
        }

        // DETECT AND REPORT, NEVER REFUSE. The engine's drag-a-material-onto-an-actor path
        // declines a construction-script component outright (AttemptApplyObjToComponent,
        // Editor/UnrealEd/Private/LevelEditorViewport.cpp:876), but
        // UActorComponent::IsCreatedByConstructionScript answers true for SCS-created
        // components as well as UserConstructionScript ones (ActorComponent.cpp:994-997), and
        // on a classPath Blueprint spawn the mesh component is almost always the SCS one -
        // refusing here would reject nearly every Blueprint spawn, i.e. remove the feature.
        //
        // The write does survive a construction-script rerun: AActor::RerunConstructionScripts
        // re-applies an FComponentInstanceDataCache after the SCS and again after the user CS
        // (ActorConstruction.cpp:927-929 / 960-962), and OverrideMaterials is captured by that
        // cache because it is CPF_Edit and non-transient (the skip rules are
        // ComponentInstanceDataCache.cpp:53-66; MeshComponent.h:29-31 declares it
        // EditAnywhere with no Transient/DuplicateTransient). What it does NOT survive is a
        // Blueprint recompile that renames or retypes the component - component instance data
        // is matched by name and creation method - which is what the notice names.
        // AddUnique so a homogeneous spawn_batch reports the condition once, not once per actor.
        if (Applied > 0 && MeshComp->IsCreatedByConstructionScript())
        {
            OutWarnings.AddUnique(MakeConstructionScriptWarning(
                MeshComp->GetName(), SpawnedClassDisplayName(Actor)));
        }
        return Applied;
    }

    // One OnSceneMaterialsModified per RPC (not per actor) so viewports and the
    // material-usage caches refresh once after a whole batch. No-op without GEditor.
    inline void NotifySceneMaterialsModified(int32 AppliedCount)
    {
        if (AppliedCount > 0 && GEditor)
        {
            GEditor->OnSceneMaterialsModified();
        }
    }

    // ---- report -----------------------------------------------------------

    // Echoes the material outcome onto a spawn response. Field names match the existing
    // material-echoing handlers: `material_applied` as in blueprint.scs.add_component and
    // environment.create_procedural_terrain, `materialPath` as in asset/landscape/spline/
    // render, plus `materialPaths` / `materialSlotsApplied` for the multi-slot form and the
    // house-standard `warnings` string array.
    //
    // BACKWARD COMPATIBILITY: when no material was requested and nothing warned, this writes
    // nothing at all, so a legacy call's response JSON stays byte-identical.
    inline void AddMaterialReport(const TSharedPtr<FJsonObject>& Data,
                                  const FMaterialSpec& Spec,
                                  int32 AppliedCount,
                                  const FString& ComponentName,
                                  const TArray<FString>& Warnings)
    {
        if (!Data.IsValid() || (Spec.SlotPaths.Num() == 0 && Warnings.Num() == 0))
        {
            return;
        }
        Data->SetBoolField(TEXT("material_applied"), AppliedCount > 0);
        Data->SetNumberField(TEXT("materialSlotsApplied"), AppliedCount);
        if (Spec.SlotPaths.Num() == 1)
        {
            Data->SetStringField(TEXT("materialPath"), Spec.SlotPaths[0]);
        }
        else if (Spec.SlotPaths.Num() > 1)
        {
            Data->SetArrayField(TEXT("materialPaths"), EmitStringArray(Spec.SlotPaths));
        }
        if (!ComponentName.IsEmpty())
        {
            Data->SetStringField(TEXT("materialComponent"), ComponentName);
        }
        if (Warnings.Num() > 0)
        {
            Data->SetArrayField(TEXT("warnings"), EmitStringArray(Warnings));
        }
        // Machine-readable form of the construction-script notice Apply appended, derived from
        // the warning itself (ConstructionScriptWarningPrefix) so the flag needs no extra
        // out-param through the three spawn handlers. Written only when the condition holds,
        // so a spawn that never touched a construction-script component - every
        // actor.spawn_shape, every native-class actor.spawn - keeps a byte-identical response.
        if (HasConstructionScriptWarning(Warnings))
        {
            Data->SetBoolField(TEXT("materialOnConstructionScriptComponent"), true);
        }
    }
}
