// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Compat/EngineVersionCompat.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstance.h"
#include "Materials/MaterialInterface.h"
#include "Utils/JsonUtils.h"

// A material's EMaterialUsage set, read through the engine's own accessor and published as one
// block.
//
// THE DEFECT THIS CLOSES. A UMaterial drawn by a consumer whose usage it does not declare is
// substituted with the engine Default Material - MATUSAGE_SkeletalMesh for a USkeletalMeshComponent,
// MATUSAGE_InstancedStaticMeshes for an ISM/HISM, MATUSAGE_NiagaraSprites for a sprite renderer.
// Nothing else in a material response distinguishes that state: the domain, blend mode, shading
// model, node count, wired mainInputs and parameters all read correct, and so does the shader-compile
// verdict, because the missing permutation is not part of the shader map at all
// (GPUSkinVertexFactory.cpp: ShouldCompilePermutation returns false unless bIsUsedWithSkeletalMesh or
// bIsUsedWithMorphTargets, so the map is COMPLETE without it).
//
// The editor hides the condition by repairing it: UMaterial::SetMaterialUsage sets the flag and
// recompiles when bAutomaticallySetUsageInEditor, then dirties the package - and if that dirty
// package is never saved it re-pays the recompile every launch and a packaged build ships the
// substitution. So "no visible change in the editor" is the signature, not the mitigation, which is
// why the flags have to be readable rather than inferable from a render.
//
// READ HERE, WRITE THROUGH THE REFLECTION VERBS. Every row names the UMaterial UPROPERTY that backs
// it, so property.get / property.set can act on the answer; the property name is DERIVED from the
// engine's usage name and then VERIFIED against UMaterial's reflection data, so a future engine that
// breaks the pattern yields an empty string rather than a fabricated property name.
namespace PinWright::MaterialUsage
{
    // Engine spelling of one usage ("SkeletalMesh", "InstancedStaticMeshes", ...). Only values below
    // MATUSAGE_MAX are ever passed, which is exactly the set UMaterialInterface::GetUsageName covers;
    // it logs Fatal on anything else.
    //
    // UMaterialInterface::GetUsageName is 5.8-only. The only pre-5.8 spelling is
    // UMaterial::GetUsageName, which is neither static nor ENGINE_API-exported (so a plugin TU
    // cannot link it) and returns the PROPERTY name ("bUsedWithSkeletalMesh") rather than the bare
    // usage name 5.8 publishes. The enumerator carries that same bare text for every usage but
    // MATUSAGE_SplineMesh, whose engine name is plural - and the UMaterial UPROPERTY is the
    // authority on that, so the plural is taken only when reflection says this engine spells it so.
    // A hardcoded table would not compile across the matrix: the enumerator set differs by version
    // (no MATUSAGE_StaticMesh on 5.5, no MATUSAGE_MaterialCache on 5.7).
    inline FString GetUsageName(EMaterialUsage Usage)
    {
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 8, 0)
        return UMaterialInterface::GetUsageName(Usage);
#else
        const UEnum* UsageEnum = StaticEnum<EMaterialUsage>();
        const UClass* MaterialClass = UMaterial::StaticClass();
        if (!UsageEnum || !MaterialClass)
        {
            return FString();
        }
        FString Name = UsageEnum->GetNameStringByValue(static_cast<int64>(Usage));
        Name.RemoveFromStart(TEXT("MATUSAGE_"));
        if (!Name.IsEmpty()
            && MaterialClass->FindPropertyByName(FName(*(TEXT("bUsedWith") + Name))) == nullptr
            && MaterialClass->FindPropertyByName(
                FName(*(TEXT("bUsedWith") + Name + TEXT("s")))) != nullptr)
        {
            Name += TEXT("s");
        }
        return Name;
#endif
    }

    // The UMaterial UPROPERTY backing a usage, or empty when this engine does not spell it that way.
    inline FString FindUsagePropertyName(EMaterialUsage Usage)
    {
        const FString Candidate = TEXT("bUsedWith") + GetUsageName(Usage);
        return UMaterial::StaticClass()->FindPropertyByName(FName(*Candidate)) != nullptr
            ? Candidate
            : FString();
    }

    // Declared on this interface. Correct for an instance as well as a base material:
    // UMaterialInstance::UsageFlags is the parent's set with the instance's own overrides applied
    // (MaterialInstance.cpp, UpdateOverridableBaseProperties).
    //
    // UMaterialInterface::GetUsageByFlag is 5.8-only - that is the release that gave an INSTANCE
    // its own overridable usage flags. Before it, usage is a property of the base UMaterial alone
    // and an instance inherits it, so the base material is the object that answers.
    inline bool DeclaresUsage(const UMaterialInterface* MaterialInterface, EMaterialUsage Usage)
    {
        if (!MaterialInterface)
        {
            return false;
        }
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 8, 0)
        return MaterialInterface->GetUsageByFlag(Usage);
#else
        const UMaterial* BaseMaterial = MaterialInterface->GetMaterial_Concurrent();
        return BaseMaterial != nullptr && BaseMaterial->GetUsageByFlag(Usage);
#endif
    }

    // Every usage this interface declares, in engine spelling. Short in practice - most materials
    // declare none or one - which is why it is cheap enough to ride along on every probe.
    inline TArray<FString> DeclaredUsageNames(const UMaterialInterface* MaterialInterface)
    {
        TArray<FString> Names;
        if (!MaterialInterface)
        {
            return Names;
        }
        for (int32 Index = 0; Index < MATUSAGE_MAX; ++Index)
        {
            const EMaterialUsage Usage = static_cast<EMaterialUsage>(Index);
            if (DeclaresUsage(MaterialInterface, Usage))
            {
                Names.Add(GetUsageName(Usage));
            }
        }
        return Names;
    }

    // What to do with a usage row. Named once so the read verbs cannot describe the write route
    // differently: there is deliberately no usage-setter verb, because property.set writes the
    // deprecated member directly and does not run the SetMaterialUsage recompile pairing.
    inline const TCHAR* WriteRouteHint()
    {
        return TEXT("Each row's `property` is the UMaterial UPROPERTY behind the flag: read it with "
            "property.get and write it with property.set on the BASE material, then call "
            "material.authoring.compile_material so the new permutation is built. A consumer whose "
            "usage is not declared here draws the engine Default Material; in the editor "
            "UMaterial::SetMaterialUsage silently patches the flag at draw time when "
            "autoSetInEditor is true, so the material looks correct on screen until that dirtied "
            "package is saved - unsaved, it recompiles every launch and a packaged build ships the "
            "substitution.");
    }

    // The whole EMaterialUsage set, not a chosen subset: choosing one costs a second ticket the
    // first time a different usage matters, and the whole set is 28 booleans.
    //
    // Pass Parent to compare an instance against what it inherits; the rows then also carry
    // `parentDeclared` and `overridden`, which is the only way to see that an instance turned a
    // usage OFF that its parent declares.
    inline TSharedPtr<FJsonObject> BuildUsageReport(const UMaterialInterface* MaterialInterface,
        const UMaterialInterface* Parent = nullptr)
    {
        const TSharedPtr<FJsonObject> Block = MakeShared<FJsonObject>();
        if (!MaterialInterface)
        {
            return Block;
        }

        TArray<TSharedPtr<FJsonValue>> Rows;
        TArray<FString> Declared;
        TArray<FString> ParentDeclared;
        for (int32 Index = 0; Index < MATUSAGE_MAX; ++Index)
        {
            const EMaterialUsage Usage = static_cast<EMaterialUsage>(Index);
            const FString Name = GetUsageName(Usage);
            const bool bDeclared = DeclaresUsage(MaterialInterface, Usage);
            if (bDeclared)
            {
                Declared.Add(Name);
            }

            const TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
            Row->SetStringField(TEXT("usage"), Name);
            Row->SetBoolField(TEXT("declared"), bDeclared);
            const FString PropertyName = FindUsagePropertyName(Usage);
            if (!PropertyName.IsEmpty())
            {
                Row->SetStringField(TEXT("property"), PropertyName);
            }
            if (Parent)
            {
                const bool bParentDeclared = DeclaresUsage(Parent, Usage);
                if (bParentDeclared)
                {
                    ParentDeclared.Add(Name);
                }
                Row->SetBoolField(TEXT("parentDeclared"), bParentDeclared);
                Row->SetBoolField(TEXT("overridden"), bDeclared != bParentDeclared);
            }
            Rows.Add(MakeShared<FJsonValueObject>(Row));
        }

        Block->SetArrayField(TEXT("declared"), EmitStringArray(Declared));
        if (Parent)
        {
            Block->SetArrayField(TEXT("parentDeclared"), EmitStringArray(ParentDeclared));
            Block->SetStringField(TEXT("parentPath"), Parent->GetPathName());
        }
        Block->SetArrayField(TEXT("flags"), Rows);

        // The editor's auto-repair is a property of the BASE material and is the reason a missing
        // flag is invisible in every frame a caller can capture, so it belongs beside the flags.
        if (const UMaterial* Base = MaterialInterface->GetMaterial_Concurrent())
        {
            Block->SetBoolField(TEXT("autoSetInEditor"), Base->bAutomaticallySetUsageInEditor != 0);
        }
        Block->SetStringField(TEXT("hint"), WriteRouteHint());
        return Block;
    }

    inline void AddUsageReport(const TSharedPtr<FJsonObject>& Result,
        const UMaterialInterface* MaterialInterface, const UMaterialInterface* Parent = nullptr)
    {
        if (Result.IsValid() && MaterialInterface)
        {
            Result->SetObjectField(TEXT("usage"), BuildUsageReport(MaterialInterface, Parent));
        }
    }
}
