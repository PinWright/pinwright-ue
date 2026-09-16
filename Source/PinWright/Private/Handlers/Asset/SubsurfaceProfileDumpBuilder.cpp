// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Handlers/Asset/SubsurfaceProfileDumpBuilder.h"

#include "Engine/SubsurfaceProfile.h"
#include "Misc/EngineVersionComparison.h"
#include "UObject/Class.h"
#include "Utils/JsonBuilders.h"
#include "Handlers/Asset/AssetDumpHandler.h"
#include "Utils/JsonSidecarRegistry.h"

TSharedPtr<FJsonObject> SubsurfaceProfileDumpBuilder::BuildSubsurfaceProfileJson(const USubsurfaceProfile* Profile)
{
    if (!Profile)
    {
        return nullptr;
    }

    TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
    const FSubsurfaceProfileStruct& S = Profile->Settings;

    Root->SetObjectField(TEXT("surfaceAlbedo"),         JsonBuilders::BuildLinearColorJson(S.SurfaceAlbedo));
    Root->SetObjectField(TEXT("meanFreePathColor"),     JsonBuilders::BuildLinearColorJson(S.MeanFreePathColor));
    Root->SetNumberField(TEXT("meanFreePathDistance"),  S.MeanFreePathDistance);
    Root->SetNumberField(TEXT("worldUnitScale"),        S.WorldUnitScale);
    Root->SetBoolField(TEXT("bEnableBurley"),           S.bEnableBurley);
    Root->SetBoolField(TEXT("bEnableMeanFreePath"),     S.bEnableMeanFreePath);
    Root->SetObjectField(TEXT("tint"),                  JsonBuilders::BuildLinearColorJson(S.Tint));
    Root->SetNumberField(TEXT("scatterRadius"),         S.ScatterRadius);
    Root->SetObjectField(TEXT("subsurfaceColor"),       JsonBuilders::BuildLinearColorJson(S.SubsurfaceColor));
    Root->SetObjectField(TEXT("falloffColor"),          JsonBuilders::BuildLinearColorJson(S.FalloffColor));
    Root->SetObjectField(TEXT("boundaryColorBleed"),    JsonBuilders::BuildLinearColorJson(S.BoundaryColorBleed));
    // 5.6+: FSubsurfaceProfileStruct::Implementation and the enum
    // ESubsurfaceImplementationTechniqueHint were added. On 5.4/5.5 neither exists, so we
    // emit the same "implementation" key with an empty string to keep the JSON shape stable.
#if UE_VERSION_OLDER_THAN(5, 6, 0)
    Root->SetStringField(TEXT("implementation"),        FString());
#else
    Root->SetStringField(TEXT("implementation"),        JsonBuilders::EnumValueToString<ESubsurfaceImplementationTechniqueHint>(S.Implementation));
#endif
    Root->SetNumberField(TEXT("extinctionScale"),       S.ExtinctionScale);
    Root->SetNumberField(TEXT("normalScale"),           S.NormalScale);
    Root->SetNumberField(TEXT("scatteringDistribution"), S.ScatteringDistribution);
    Root->SetNumberField(TEXT("ior"),                   S.IOR);
    Root->SetNumberField(TEXT("roughness0"),            S.Roughness0);
    Root->SetNumberField(TEXT("roughness1"),            S.Roughness1);
    Root->SetNumberField(TEXT("lobeMix"),               S.LobeMix);
    Root->SetObjectField(TEXT("transmissionTintColor"), JsonBuilders::BuildLinearColorJson(S.TransmissionTintColor));

    return Root;
}

namespace
{
    UClass* GetSubsurfaceProfileSidecarClass()
    {
        return USubsurfaceProfile::StaticClass();
    }

    TSharedPtr<FJsonObject> BuildSubsurfaceProfileSidecar(UObject* Asset)
    {
        return SubsurfaceProfileDumpBuilder::BuildSubsurfaceProfileJson(Cast<USubsurfaceProfile>(Asset));
    }
}

REGISTER_DUMP_JSON_SIDECAR(TEXT("subsurface_profile"), DumpFileNames::SubsurfaceProfile,
    &GetSubsurfaceProfileSidecarClass, &BuildSubsurfaceProfileSidecar,
    nullptr, nullptr, nullptr, 100);
