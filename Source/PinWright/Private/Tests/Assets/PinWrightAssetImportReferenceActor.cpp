// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Tests/Assets/PinWrightAssetImportReferenceActor.h"

APinWrightAssetImportReferenceActor::APinWrightAssetImportReferenceActor()
{
    PrimaryActorTick.bCanEverTick = false;
    SetFlags(RF_Transient);
}
