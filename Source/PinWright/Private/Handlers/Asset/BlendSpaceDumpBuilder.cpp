// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Handlers/Asset/BlendSpaceDumpBuilder.h"

#include "Animation/BlendSpace.h"
#include "Animation/AnimSequence.h"
#include "UObject/Class.h"
#include "Utils/JsonBuilders.h"
#include "Handlers/Asset/AssetDumpHandler.h"
#include "Utils/JsonSidecarRegistry.h"

TSharedPtr<FJsonObject> BlendSpaceDumpBuilder::BuildBlendSpaceJson(const UBlendSpace* BlendSpace)
{
    if (!BlendSpace)
    {
        return nullptr;
    }

    TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();

    // class — concrete class name so consumers can disambiguate
    // BlendSpace / BlendSpace1D / AimOffsetBlendSpace / AimOffsetBlendSpace1D.
    Root->SetStringField(TEXT("class"), BlendSpace->GetClass()->GetName());

    // axes[3] — one entry per blend parameter. Read via the public GetBlendParameter accessor
    // since the underlying array is protected.
    TArray<TSharedPtr<FJsonValue>> AxesArr;
    AxesArr.Reserve(3);
    for (int32 i = 0; i < 3; ++i)
    {
        const FBlendParameter& Param = BlendSpace->GetBlendParameter(i);
        TSharedRef<FJsonObject> Axis = MakeShared<FJsonObject>();
        Axis->SetStringField(TEXT("displayName"), Param.DisplayName);
        Axis->SetNumberField(TEXT("min"), Param.Min);
        Axis->SetNumberField(TEXT("max"), Param.Max);
        Axis->SetNumberField(TEXT("gridNum"), Param.GridNum);
        AxesArr.Add(MakeShared<FJsonValueObject>(Axis));
    }
    Root->SetArrayField(TEXT("axes"), AxesArr);

    // samples[] — read via public GetBlendSamples accessor.
    TArray<TSharedPtr<FJsonValue>> SamplesArr;
    const TArray<FBlendSample>& Samples = BlendSpace->GetBlendSamples();
    SamplesArr.Reserve(Samples.Num());
    for (const FBlendSample& Sample : Samples)
    {
        TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetStringField(TEXT("animation"), JsonBuilders::GetObjectPathSafe(Sample.Animation));
        Entry->SetNumberField(TEXT("x"), Sample.SampleValue.X);
        Entry->SetNumberField(TEXT("y"), Sample.SampleValue.Y);
        Entry->SetNumberField(TEXT("rateScale"), Sample.RateScale);
        SamplesArr.Add(MakeShared<FJsonValueObject>(Entry));
    }
    Root->SetArrayField(TEXT("samples"), SamplesArr);

    // interpolation[3] — InterpolationParam[3] is public on UBlendSpace.
    TArray<TSharedPtr<FJsonValue>> InterpArr;
    InterpArr.Reserve(3);
    for (int32 i = 0; i < 3; ++i)
    {
        const FInterpolationParameter& Param = BlendSpace->InterpolationParam[i];
        TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetNumberField(TEXT("interpolationTime"), Param.InterpolationTime);
        Entry->SetStringField(TEXT("interpolationType"), JsonBuilders::EnumValueToString<EFilterInterpolationType>(Param.InterpolationType));
        InterpArr.Add(MakeShared<FJsonValueObject>(Entry));
    }
    Root->SetArrayField(TEXT("interpolation"), InterpArr);

    return Root;
}

namespace
{
    UClass* GetBlendSpaceSidecarClass()
    {
        return UBlendSpace::StaticClass();
    }

    TSharedPtr<FJsonObject> BuildBlendSpaceSidecar(UObject* Asset)
    {
        return BlendSpaceDumpBuilder::BuildBlendSpaceJson(Cast<UBlendSpace>(Asset));
    }
}

REGISTER_DUMP_JSON_SIDECAR(TEXT("blend_space"), DumpFileNames::BlendSpace,
    &GetBlendSpaceSidecarClass, &BuildBlendSpaceSidecar,
    nullptr, nullptr, nullptr, 100);
