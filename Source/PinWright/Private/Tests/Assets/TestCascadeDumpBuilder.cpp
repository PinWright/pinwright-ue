// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Misc/AutomationTest.h"
#include "Handlers/Asset/AssetDumpHandler.h"
#include "Handlers/Particle/CascadeDumpBuilder.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "AssetDumpTestHelpers.h"
#include "Tests/TestUtils.h"


#include "Particles/Color/ParticleModuleColor.h"
#include "Particles/ParticleLODLevel.h"
#include "Particles/ParticleModuleRequired.h"
#include "Particles/ParticleSpriteEmitter.h"
#include "Particles/ParticleSystem.h"
#include "Particles/Spawn/ParticleModuleSpawn.h"
#include "Particles/TypeData/ParticleModuleTypeDataMesh.h"
#include "UObject/Package.h"

namespace
{
    using AssetDumpTestHelpers::HasDumpFile;
    using AssetDumpTestHelpers::FindDumpFile;
    using AssetDumpTestHelpers::LoadJsonFile;

    FString MakeUniqueCascadeTestAssetName(const TCHAR* Prefix)
    {
        return FString::Printf(TEXT("%s_%s"), Prefix, *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    }

    UParticleSystem* NewTransientCascadeSystem(FString& OutObjectPath)
    {
        const FString AssetName = MakeUniqueCascadeTestAssetName(TEXT("PS_CascadeDump"));
        const FString PackageName = FString::Printf(TEXT("/Game/PinWrightTests/%s"), *AssetName);
        UPackage* Package = CreatePackage(*PackageName);
        UParticleSystem* ParticleSystem = NewObject<UParticleSystem>(
            Package,
            UParticleSystem::StaticClass(),
            *AssetName,
            RF_Public | RF_Standalone | RF_Transient);
        if (!ParticleSystem)
        {
            return nullptr;
        }

        UParticleSpriteEmitter* Emitter = NewObject<UParticleSpriteEmitter>(
            ParticleSystem,
            UParticleSpriteEmitter::StaticClass(),
            TEXT("SmokeEmitter"),
            RF_Transient);
        Emitter->EmitterName = TEXT("Smoke");

        UParticleLODLevel* LODLevel = NewObject<UParticleLODLevel>(
            Emitter,
            UParticleLODLevel::StaticClass(),
            TEXT("LODLevel_0"),
            RF_Transient);
        LODLevel->Level = 0;
        LODLevel->bEnabled = true;

        LODLevel->RequiredModule = NewObject<UParticleModuleRequired>(
            ParticleSystem,
            UParticleModuleRequired::StaticClass(),
            TEXT("RequiredModule"),
            RF_Transient);
        LODLevel->SpawnModule = NewObject<UParticleModuleSpawn>(
            ParticleSystem,
            UParticleModuleSpawn::StaticClass(),
            TEXT("SpawnModule"),
            RF_Transient);
        LODLevel->TypeDataModule = NewObject<UParticleModuleTypeDataMesh>(
            ParticleSystem,
            UParticleModuleTypeDataMesh::StaticClass(),
            TEXT("MeshTypeData"),
            RF_Transient);

        UParticleModuleColor* ColorModule = NewObject<UParticleModuleColor>(
            ParticleSystem,
            UParticleModuleColor::StaticClass(),
            TEXT("InitialColor"),
            RF_Transient);
        LODLevel->Modules.Add(ColorModule);

        Emitter->LODLevels.Add(LODLevel);
        ParticleSystem->Emitters.Add(Emitter);
        ParticleSystem->AddToRoot();
        OutObjectPath = FString::Printf(TEXT("%s.%s"), *PackageName, *AssetName);
        return ParticleSystem;
    }

}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCascadeDumpBuilderShapeTest,
    "PinWright.Assets.Cascade.DumpBuilder.Shape",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCascadeDumpBuilderShapeTest::RunTest(const FString& Parameters)
{
    FString ObjectPath;
    UParticleSystem* ParticleSystem = NewTransientCascadeSystem(ObjectPath);
    TestNotNull(TEXT("Transient Cascade particle system created"), ParticleSystem);
    if (!ParticleSystem)
    {
        return false;
    }

    TSharedPtr<FJsonObject> CascadeJson = CascadeDumpBuilder::BuildCascadeJson(ParticleSystem);
    TestEqual(TEXT("assetKind"), CascadeJson->GetStringField(TEXT("assetKind")), FString(TEXT("CascadeParticleSystem")));
    TestEqual(TEXT("path"), CascadeJson->GetStringField(TEXT("path")), ObjectPath);
    TestEqual(TEXT("emitterCount"), static_cast<int32>(CascadeJson->GetNumberField(TEXT("emitterCount"))), 1);

    const TArray<TSharedPtr<FJsonValue>>* Emitters = nullptr;
    TestTrue(TEXT("emitters array exists"), CascadeJson->TryGetArrayField(TEXT("emitters"), Emitters));
    TestTrue(TEXT("emitters array has fixture emitter"), Emitters && Emitters->Num() == 1);
    if (Emitters && Emitters->Num() == 1)
    {
        const TSharedPtr<FJsonObject> Emitter = (*Emitters)[0]->AsObject();
        TestEqual(TEXT("emitterName"), Emitter->GetStringField(TEXT("emitterName")), FString(TEXT("Smoke")));

        const TArray<TSharedPtr<FJsonValue>>* LODLevels = nullptr;
        TestTrue(TEXT("lodLevels array exists"), Emitter->TryGetArrayField(TEXT("lodLevels"), LODLevels));
        TestTrue(TEXT("lodLevels array has fixture LOD"), LODLevels && LODLevels->Num() == 1);
        if (LODLevels && LODLevels->Num() == 1)
        {
            const TSharedPtr<FJsonObject> LODLevel = (*LODLevels)[0]->AsObject();

            const TSharedPtr<FJsonObject>* RequiredModule = nullptr;
            const TSharedPtr<FJsonObject>* SpawnModule = nullptr;
            const TSharedPtr<FJsonObject>* TypeDataModule = nullptr;
            TestTrue(TEXT("requiredModule object exists"), LODLevel->TryGetObjectField(TEXT("requiredModule"), RequiredModule));
            TestTrue(TEXT("spawnModule object exists"), LODLevel->TryGetObjectField(TEXT("spawnModule"), SpawnModule));
            TestTrue(TEXT("typeDataModule object exists"), LODLevel->TryGetObjectField(TEXT("typeDataModule"), TypeDataModule));
            if (RequiredModule)
            {
                TestEqual(TEXT("required className"), (*RequiredModule)->GetStringField(TEXT("className")), FString(TEXT("ParticleModuleRequired")));
                TestTrue(TEXT("required properties diff exists"), (*RequiredModule)->HasTypedField<EJson::Object>(TEXT("properties")));
            }
            if (SpawnModule)
            {
                TestEqual(TEXT("spawn className"), (*SpawnModule)->GetStringField(TEXT("className")), FString(TEXT("ParticleModuleSpawn")));
                TestTrue(TEXT("spawn properties diff exists"), (*SpawnModule)->HasTypedField<EJson::Object>(TEXT("properties")));
            }
            if (TypeDataModule)
            {
                TestEqual(TEXT("type-data className"), (*TypeDataModule)->GetStringField(TEXT("className")), FString(TEXT("ParticleModuleTypeDataMesh")));
                TestTrue(TEXT("type-data properties diff exists"), (*TypeDataModule)->HasTypedField<EJson::Object>(TEXT("properties")));
            }

            const TArray<TSharedPtr<FJsonValue>>* Modules = nullptr;
            TestTrue(TEXT("modules array exists"), LODLevel->TryGetArrayField(TEXT("modules"), Modules));
            TestTrue(TEXT("modules array has fixture module"), Modules && Modules->Num() == 1);
            if (Modules && Modules->Num() == 1)
            {
                const TSharedPtr<FJsonObject> Module = (*Modules)[0]->AsObject();
                TestEqual(TEXT("module className"), Module->GetStringField(TEXT("className")), FString(TEXT("ParticleModuleColor")));
                TestTrue(TEXT("module properties diff exists"), Module->HasTypedField<EJson::Object>(TEXT("properties")));
            }
        }
    }

    ParticleSystem->RemoveFromRoot();
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCascadeAssetDumpWritesCascadeAspectFileTest,
    "PinWright.Assets.Cascade.AssetDump.WritesCascadeAspectFile",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCascadeAssetDumpWritesCascadeAspectFileTest::RunTest(const FString& Parameters)
{
    FString ObjectPath;
    UParticleSystem* ParticleSystem = NewTransientCascadeSystem(ObjectPath);
    TestNotNull(TEXT("Transient Cascade particle system created"), ParticleSystem);
    if (!ParticleSystem)
    {
        return false;
    }

    const FString ScratchRoot = FPaths::ConvertRelativePathToFull(FPaths::ProjectIntermediateDir())
        / TEXT("CascadeDumpBuilderTests") / FGuid::NewGuid().ToString(EGuidFormats::Digits);

    const AssetDumpHandler::FDumpSingleResult Result =
        AssetDumpHandler::DumpSingleAsset(ObjectPath, ScratchRoot, /*bDiff=*/false);

    TestTrue(TEXT("asset.dump succeeds for transient Cascade system"), Result.ErrorCode.IsEmpty());
    TestTrue(TEXT("properties.json remains present"), HasDumpFile(Result.WrittenPaths, DumpFileNames::Properties));
    TestTrue(TEXT("cascade.json is written"), HasDumpFile(Result.WrittenPaths, DumpFileNames::Cascade));
    TestTrue(TEXT("Cascade dump has more than generic meta/properties"), Result.WrittenPaths.Num() > 2);

    const FString CascadePath = FindDumpFile(Result.WrittenPaths, DumpFileNames::Cascade);
    TSharedPtr<FJsonObject> CascadeJson = LoadJsonFile(CascadePath);
    TestTrue(TEXT("cascade.json parses"), CascadeJson.IsValid());
    if (CascadeJson.IsValid())
    {
        TestEqual(TEXT("cascade.json assetKind"), CascadeJson->GetStringField(TEXT("assetKind")), FString(TEXT("CascadeParticleSystem")));
        TestEqual(TEXT("cascade.json emitterCount"), static_cast<int32>(CascadeJson->GetNumberField(TEXT("emitterCount"))), 1);
    }

    IFileManager::Get().DeleteDirectory(*ScratchRoot, /*RequireExists=*/false, /*Tree=*/true);
    ParticleSystem->RemoveFromRoot();
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCascadeNoEditRpcHandlersTest,
    "PinWright.Assets.Cascade.NoEditRpcHandlers",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCascadeNoEditRpcHandlersTest::RunTest(const FString& Parameters)
{
    auto IsCascadeEditMethod = [](const FString& MethodName)
    {
        if (!MethodName.StartsWith(TEXT("cascade."))
            && !MethodName.StartsWith(TEXT("particle.cascade.")))
        {
            return false;
        }

        return MethodName.Contains(TEXT(".set"))
            || MethodName.Contains(TEXT(".add"))
            || MethodName.Contains(TEXT(".remove"))
            || MethodName.Contains(TEXT(".move"))
            || MethodName.Contains(TEXT(".edit"))
            || MethodName.Contains(TEXT(".enable"))
            || MethodName.Contains(TEXT(".disable"))
            || MethodName.Contains(TEXT(".create"))
            || MethodName.Contains(TEXT(".delete"))
            || MethodName.Contains(TEXT(".update"));
    };

    for (const FHandlerRegistration& Reg : FAutoRegisterHandler::GetPendingRegistrations())
    {
        TestFalse(
            FString::Printf(TEXT("%s is not a Cascade edit handler"), *Reg.MethodName),
            IsCascadeEditMethod(Reg.MethodName));
    }
    return true;
}
