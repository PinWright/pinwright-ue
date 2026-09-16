// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Tests/IrCore/IrTestFixture.h"


#include "Misc/Guid.h"
#include "Tests/TestUtils.h"

namespace IrTest
{
FScratchAsset::FScratchAsset(const TCHAR* Prefix)
{
    const TCHAR* EffectivePrefix = Prefix ? Prefix : TEXT("Scratch");
    PackagePath = FString::Printf(
        TEXT("/Game/PinWrightTests/%s_%s"),
        EffectivePrefix,
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    AssetName = FPackageName::GetLongPackageAssetName(PackagePath);
}

FScratchAsset::~FScratchAsset()
{
    CleanupTestAsset(PackagePath);
}

FString ReplaceScratchAssetName(
    const FString& InputText,
    const FScratchAsset& Source,
    const FScratchAsset& Target)
{
    if (Source.AssetName.IsEmpty())
    {
        return InputText;
    }

    return InputText.Replace(
        *Source.AssetName,
        *Target.AssetName,
        ESearchCase::CaseSensitive);
}
} // namespace IrTest
