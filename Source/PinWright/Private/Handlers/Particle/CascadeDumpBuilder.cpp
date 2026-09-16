// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Handlers/Particle/CascadeDumpBuilder.h"

#include "Utils/JsonBuilders.h"
#include "Utils/PropertyUtils.h"

#include "Dom/JsonValue.h"
#include "Particles/ParticleEmitter.h"
#include "Particles/ParticleLODLevel.h"
#include "Particles/ParticleModule.h"
#include "Particles/ParticleModuleRequired.h"
#include "Particles/ParticleSystem.h"
#include "Particles/Spawn/ParticleModuleSpawn.h"
#include "Particles/TypeData/ParticleModuleTypeDataBase.h"
#include "Handlers/Asset/AssetDumpHandler.h"
#include "Utils/JsonSidecarRegistry.h"

namespace
{
    TSharedPtr<FJsonValueObject> MakeCascadeObjectValue(const TSharedPtr<FJsonObject>& Obj)
    {
        return MakeShared<FJsonValueObject>(Obj);
    }

    FString ModuleTypeToString(EModuleType Type)
    {
        switch (Type)
        {
        case EPMT_General:
            return TEXT("General");
        case EPMT_TypeData:
            return TEXT("TypeData");
        case EPMT_Beam:
            return TEXT("Beam");
        case EPMT_Trail:
            return TEXT("Trail");
        case EPMT_Spawn:
            return TEXT("Spawn");
        case EPMT_Required:
            return TEXT("Required");
        case EPMT_Event:
            return TEXT("Event");
        case EPMT_Light:
            return TEXT("Light");
        case EPMT_SubUV:
            return TEXT("SubUV");
        default:
            return TEXT("Unknown");
        }
    }

    TSharedPtr<FJsonObject> BuildModuleJson(UParticleModule* Module, const TCHAR* Role, int32 Index)
    {
        TSharedPtr<FJsonObject> Obj = JsonBuilders::MakeObject();
        Obj->SetStringField(TEXT("role"), Role);
        Obj->SetNumberField(TEXT("index"), Index);

        if (!Module)
        {
            Obj->SetBoolField(TEXT("present"), false);
            return Obj;
        }

        UClass* ModuleClass = Module->GetClass();
        Obj->SetBoolField(TEXT("present"), true);
        Obj->SetStringField(TEXT("name"), Module->GetName());
        Obj->SetStringField(TEXT("path"), Module->GetPathName());
        Obj->SetStringField(TEXT("class"), ModuleClass ? ModuleClass->GetPathName() : FString());
        Obj->SetStringField(TEXT("className"), ModuleClass ? ModuleClass->GetName() : FString());
        Obj->SetStringField(TEXT("moduleType"), ModuleTypeToString(Module->GetModuleType()));
        Obj->SetBoolField(TEXT("enabled"), Module->bEnabled != 0);
        Obj->SetBoolField(TEXT("editable"), Module->bEditable != 0);
        Obj->SetBoolField(TEXT("spawnModule"), Module->bSpawnModule != 0);
        Obj->SetBoolField(TEXT("updateModule"), Module->bUpdateModule != 0);

        UObject* Baseline = ModuleClass ? ModuleClass->GetDefaultObject() : nullptr;
        Obj->SetObjectField(TEXT("properties"), BuildSparsePropertyDiffJson(Module, Baseline));
        return Obj;
    }

    TArray<TSharedPtr<FJsonValue>> BuildModuleArrayJson(
        const TArray<TObjectPtr<UParticleModule>>& Modules,
        const TCHAR* Role)
    {
        TArray<TSharedPtr<FJsonValue>> Result;
        for (int32 Index = 0; Index < Modules.Num(); ++Index)
        {
            Result.Add(MakeCascadeObjectValue(BuildModuleJson(Modules[Index].Get(), Role, Index)));
        }
        return Result;
    }

    TSharedPtr<FJsonObject> BuildLODLevelJson(UParticleLODLevel* LODLevel, int32 Index)
    {
        TSharedPtr<FJsonObject> Obj = JsonBuilders::MakeObject();
        Obj->SetNumberField(TEXT("index"), Index);

        if (!LODLevel)
        {
            Obj->SetBoolField(TEXT("present"), false);
            return Obj;
        }

        Obj->SetBoolField(TEXT("present"), true);
        Obj->SetStringField(TEXT("path"), LODLevel->GetPathName());
        Obj->SetNumberField(TEXT("level"), LODLevel->Level);
        Obj->SetBoolField(TEXT("enabled"), LODLevel->bEnabled != 0);
        Obj->SetObjectField(TEXT("requiredModule"), BuildModuleJson(LODLevel->RequiredModule.Get(), TEXT("required"), 0));
        Obj->SetObjectField(TEXT("spawnModule"), BuildModuleJson(LODLevel->SpawnModule.Get(), TEXT("spawn"), 0));
        Obj->SetObjectField(TEXT("typeDataModule"), BuildModuleJson(LODLevel->TypeDataModule.Get(), TEXT("typeData"), 0));
        Obj->SetArrayField(TEXT("modules"), BuildModuleArrayJson(LODLevel->Modules, TEXT("module")));
        return Obj;
    }

    TArray<TSharedPtr<FJsonValue>> BuildLODLevelArrayJson(UParticleEmitter* Emitter)
    {
        TArray<TSharedPtr<FJsonValue>> Result;
        if (!Emitter)
        {
            return Result;
        }

        for (int32 Index = 0; Index < Emitter->LODLevels.Num(); ++Index)
        {
            Result.Add(MakeCascadeObjectValue(BuildLODLevelJson(Emitter->LODLevels[Index], Index)));
        }
        return Result;
    }

    TSharedPtr<FJsonObject> BuildEmitterJson(UParticleEmitter* Emitter, int32 Index)
    {
        TSharedPtr<FJsonObject> Obj = JsonBuilders::MakeObject();
        Obj->SetNumberField(TEXT("index"), Index);

        if (!Emitter)
        {
            Obj->SetBoolField(TEXT("present"), false);
            return Obj;
        }

        UClass* EmitterClass = Emitter->GetClass();
        Obj->SetBoolField(TEXT("present"), true);
        Obj->SetStringField(TEXT("name"), Emitter->GetName());
        Obj->SetStringField(TEXT("emitterName"), Emitter->EmitterName.ToString());
        Obj->SetStringField(TEXT("path"), Emitter->GetPathName());
        Obj->SetStringField(TEXT("class"), EmitterClass ? EmitterClass->GetPathName() : FString());
        Obj->SetStringField(TEXT("className"), EmitterClass ? EmitterClass->GetName() : FString());
        Obj->SetNumberField(TEXT("lodLevelCount"), Emitter->LODLevels.Num());
        Obj->SetArrayField(TEXT("lodLevels"), BuildLODLevelArrayJson(Emitter));
        return Obj;
    }

    TArray<TSharedPtr<FJsonValue>> BuildEmitterArrayJson(UParticleSystem* ParticleSystem)
    {
        TArray<TSharedPtr<FJsonValue>> Result;
        if (!ParticleSystem)
        {
            return Result;
        }

        for (int32 Index = 0; Index < ParticleSystem->Emitters.Num(); ++Index)
        {
            Result.Add(MakeCascadeObjectValue(BuildEmitterJson(ParticleSystem->Emitters[Index], Index)));
        }
        return Result;
    }
}

namespace CascadeDumpBuilder
{
    TSharedPtr<FJsonObject> BuildCascadeJson(UParticleSystem* ParticleSystem)
    {
        TSharedPtr<FJsonObject> Obj = JsonBuilders::MakeObject();
        Obj->SetNumberField(TEXT("schemaVersion"), 1);
        Obj->SetStringField(TEXT("assetKind"), TEXT("CascadeParticleSystem"));
        Obj->SetStringField(TEXT("path"), JsonBuilders::GetObjectPathSafe(ParticleSystem));

        const int32 EmitterCount = ParticleSystem ? ParticleSystem->Emitters.Num() : 0;
        Obj->SetNumberField(TEXT("emitterCount"), EmitterCount);
        Obj->SetArrayField(TEXT("emitters"), BuildEmitterArrayJson(ParticleSystem));
        return Obj;
    }
}

namespace
{
    UClass* GetCascadeSidecarClass()
    {
        return UParticleSystem::StaticClass();
    }

    TSharedPtr<FJsonObject> BuildCascadeSidecar(UObject* Asset)
    {
        return CascadeDumpBuilder::BuildCascadeJson(Cast<UParticleSystem>(Asset));
    }
}

REGISTER_DUMP_JSON_SIDECAR(TEXT("cascade"), DumpFileNames::Cascade,
    &GetCascadeSidecarClass, &BuildCascadeSidecar,
    nullptr, nullptr, nullptr, 100);
