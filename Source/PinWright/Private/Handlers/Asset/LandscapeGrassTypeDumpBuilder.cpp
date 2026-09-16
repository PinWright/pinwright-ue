// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Handlers/Asset/LandscapeGrassTypeDumpBuilder.h"

#include "LandscapeGrassType.h"
#include "Engine/StaticMesh.h"
#include "Engine/EngineTypes.h"
#include "UObject/Class.h"
#include "Utils/JsonBuilders.h"
#include "Handlers/Asset/AssetDumpHandler.h"
#include "Utils/JsonSidecarRegistry.h"

namespace
{
    TSharedRef<FJsonObject> BuildFloatIntervalJson(const FFloatInterval& Interval)
    {
        TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetNumberField(TEXT("min"), Interval.Min);
        Obj->SetNumberField(TEXT("max"), Interval.Max);
        return Obj;
    }
}

TSharedPtr<FJsonObject> LandscapeGrassTypeDumpBuilder::BuildLandscapeGrassTypeJson(const ULandscapeGrassType* GrassType)
{
    if (!GrassType)
    {
        return nullptr;
    }

    TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();

    TArray<TSharedPtr<FJsonValue>> VarietiesArr;
    VarietiesArr.Reserve(GrassType->GrassVarieties.Num());
    for (const FGrassVariety& Variety : GrassType->GrassVarieties)
    {
        TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetStringField(TEXT("grassMesh"), JsonBuilders::GetObjectPathSafe(Variety.GrassMesh));
        // FPerPlatform* expose their default scalar via the Default field.
        Entry->SetNumberField(TEXT("grassDensity"), Variety.GrassDensity.Default);
        Entry->SetNumberField(TEXT("placementJitter"), Variety.PlacementJitter);
        Entry->SetNumberField(TEXT("startCullDistance"), Variety.StartCullDistance.Default);
        Entry->SetNumberField(TEXT("endCullDistance"),   Variety.EndCullDistance.Default);
        Entry->SetNumberField(TEXT("minLOD"), Variety.MinLOD);
        Entry->SetStringField(TEXT("scaling"), JsonBuilders::EnumValueToString<EGrassScaling>(Variety.Scaling));
        Entry->SetObjectField(TEXT("scaleX"), BuildFloatIntervalJson(Variety.ScaleX));
        Entry->SetObjectField(TEXT("scaleY"), BuildFloatIntervalJson(Variety.ScaleY));
        Entry->SetObjectField(TEXT("scaleZ"), BuildFloatIntervalJson(Variety.ScaleZ));
        Entry->SetBoolField(TEXT("randomRotation"), Variety.RandomRotation);
        Entry->SetBoolField(TEXT("alignToSurface"), Variety.AlignToSurface);

        TSharedRef<FJsonObject> LightingChannels = MakeShared<FJsonObject>();
        LightingChannels->SetBoolField(TEXT("bChannel0"), Variety.LightingChannels.bChannel0);
        LightingChannels->SetBoolField(TEXT("bChannel1"), Variety.LightingChannels.bChannel1);
        LightingChannels->SetBoolField(TEXT("bChannel2"), Variety.LightingChannels.bChannel2);
        Entry->SetObjectField(TEXT("lightingChannels"), LightingChannels);

        VarietiesArr.Add(MakeShared<FJsonValueObject>(Entry));
    }
    Root->SetArrayField(TEXT("varieties"), VarietiesArr);

    return Root;
}

namespace
{
    UClass* GetLandscapeGrassTypeSidecarClass()
    {
        return ULandscapeGrassType::StaticClass();
    }

    TSharedPtr<FJsonObject> BuildLandscapeGrassTypeSidecar(UObject* Asset)
    {
        return LandscapeGrassTypeDumpBuilder::BuildLandscapeGrassTypeJson(Cast<ULandscapeGrassType>(Asset));
    }
}

REGISTER_DUMP_JSON_SIDECAR(TEXT("landscape_grass_type"), DumpFileNames::LandscapeGrassType,
    &GetLandscapeGrassTypeSidecarClass, &BuildLandscapeGrassTypeSidecar,
    nullptr, nullptr, nullptr, 100);
